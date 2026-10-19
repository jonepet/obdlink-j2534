#include "api/passthru_api.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <ranges>
#include <span>
#include <string_view>

#include "device/device_error.h"

namespace obdlink {

namespace {

std::string_view describeStatus(long status)
{
    static constexpr std::array<std::pair<long, std::string_view>, 26> descriptions { {
        { j2534::status::NotSupported, "not supported by this device" },
        { j2534::status::InvalidChannelId, "invalid channel ID" },
        { j2534::status::InvalidProtocolId, "protocol not supported" },
        { j2534::status::NullParameter, "NULL pointer supplied where a valid pointer is required" },
        { j2534::status::InvalidIoctlValue, "invalid IOCTL parameter value" },
        { j2534::status::InvalidFlags, "invalid flags" },
        { j2534::status::Failed, "operation failed" },
        { j2534::status::DeviceNotConnected, "device not connected" },
        { j2534::status::Timeout, "timeout" },
        { j2534::status::InvalidMsg, "invalid message" },
        { j2534::status::InvalidTimeInterval, "invalid time interval" },
        { j2534::status::ExceededLimit, "limit exceeded" },
        { j2534::status::InvalidMsgId, "invalid message ID" },
        { j2534::status::DeviceInUse, "device already open" },
        { j2534::status::InvalidIoctlId, "invalid IOCTL ID" },
        { j2534::status::BufferEmpty, "no messages available" },
        { j2534::status::BufferFull, "transmit buffer full" },
        { j2534::status::BufferOverflow, "receive buffer overflowed, messages were lost" },
        { j2534::status::PinInvalid, "invalid or unset J1962 pins" },
        { j2534::status::ChannelInUse, "protocol already connected" },
        { j2534::status::MsgProtocolId, "message protocol does not match channel" },
        { j2534::status::InvalidFilterId, "invalid filter ID or type" },
        { j2534::status::NoFlowControl, "no flow control filter matches the message" },
        { j2534::status::NotUnique, "filter CAN ID already in use" },
        { j2534::status::InvalidBaudrate, "baud rate not achievable" },
        { j2534::status::InvalidDeviceId, "invalid device ID" },
    } };
    const auto found = std::ranges::find(descriptions, status, &std::pair<long, std::string_view>::first);
    return found == descriptions.end() ? std::string_view("unknown status") : found->second;
}

void copyText(std::string_view text, char* destination)
{
    const auto length = std::min<std::size_t>(text.size(), 79);
    std::memcpy(destination, text.data(), length);
    destination[length] = '\0';
}

bool isRoutineReadOutcome(long status)
{
    return status == j2534::status::NoError || status == j2534::status::BufferEmpty || status == j2534::status::Timeout;
}

std::string describeConfigList(j2534::ULong ioctlId, const void* input)
{
    const auto* list = static_cast<const j2534::SConfigList*>(input);
    const bool hasConfig = ioctlId == j2534::ioctl::GetConfig || ioctlId == j2534::ioctl::SetConfig;
    if (!hasConfig || list == nullptr || list->ConfigPtr == nullptr) {
        return {};
    }
    return std::span(list->ConfigPtr, list->NumOfParams) | std::views::transform([](const j2534::SConfig& item) {
        return std::format(" {:#x}={:#x}", item.Parameter, item.Value);
    }) | std::views::join | std::ranges::to<std::string>();
}

}

PassThruApi::PassThruApi(SettingsLoader loadSettings, StreamFactory openStream, std::string dllVersion)
    : loadSettings_(std::move(loadSettings))
    , openStream_(std::move(openStream))
    , dllVersion_(std::move(dllVersion))
{
}

template <typename Call>
long PassThruApi::guarded(Call call)
{
    try {
        const auto status = call();
        if (status != j2534::status::NoError) {
            setLastError(std::string(describeStatus(status)));
        }
        return status;
    } catch (const DeviceUnavailable& error) {
        setLastError(error.what());
        return j2534::status::DeviceNotConnected;
    } catch (const StnBaudRateRejected& error) {
        setLastError(error.what());
        return j2534::status::InvalidBaudrate;
    } catch (const StnTimeout& error) {
        setLastError(error.what());
        return j2534::status::Timeout;
    } catch (const std::exception& error) {
        setLastError(error.what());
        return j2534::status::Failed;
    } catch (...) {
        setLastError("unexpected internal error");
        return j2534::status::Failed;
    }
}

template <typename Call>
long PassThruApi::traced(const std::string& description, Call call)
{
    const auto status = guarded(call);
    trace(description, status);
    return status;
}

template <typename Call>
long PassThruApi::tracedOnFailure(const std::string& description, Call call)
{
    const auto status = guarded(call);
    if (!isRoutineReadOutcome(status)) {
        trace(description, status);
    }
    return status;
}

void PassThruApi::trace(const std::string& description, long status)
{
    std::shared_ptr<Device> device;
    std::string error;
    {
        std::lock_guard lock(mutex_);
        device = device_;
        error = lastError_;
    }
    if (!device) {
        return;
    }
    if (status == j2534::status::NoError) {
        device->note(description + " -> OK");
        return;
    }
    device->note(std::format("{} -> {:#x} {}", description, status, error));
}

long PassThruApi::open(const void* name, j2534::ULong* deviceId)
{
    return traced("PassThruOpen", [&]() -> long {
        if (deviceId == nullptr) {
            return j2534::status::NullParameter;
        }
        std::lock_guard lifecycleLock(lifecycleMutex_);
        if (openDevice(DeviceId)) {
            return j2534::status::DeviceInUse;
        }
        auto settings = loadSettings_();
        if (const auto* requested = static_cast<const char*>(name); requested != nullptr && *requested != '\0') {
            settings.portName = requested;
        }
        if (settings.portName.empty()) {
            throw StnError("no COM port configured: set the ComPort registry value or pass the port name to PassThruOpen");
        }
        auto device = std::make_shared<Device>(openStream_(settings), settings, DeviceId + 1);
        device->open();
        {
            std::lock_guard lock(mutex_);
            device_ = std::move(device);
        }
        *deviceId = DeviceId;
        return j2534::status::NoError;
    });
}

long PassThruApi::close(j2534::ULong deviceId)
{
    return guarded([&]() -> long {
        std::lock_guard lifecycleLock(lifecycleMutex_);
        std::shared_ptr<Device> device;
        {
            std::lock_guard lock(mutex_);
            if (!device_ || deviceId != DeviceId) {
                return j2534::status::InvalidDeviceId;
            }
            device = std::exchange(device_, nullptr);
        }
        device->close();
        return j2534::status::NoError;
    });
}

long PassThruApi::connect(j2534::ULong deviceId, j2534::ULong protocolId, j2534::ULong flags, j2534::ULong baud, j2534::ULong* channelId)
{
    const auto description = std::format("PassThruConnect protocol={:#x} flags={:#x} baud={}", protocolId, flags, baud);
    return traced(description, [&]() -> long {
        if (channelId == nullptr) {
            return j2534::status::NullParameter;
        }
        const auto device = openDevice(deviceId);
        if (!device) {
            return j2534::status::InvalidDeviceId;
        }
        return device->connect(protocolId, flags, baud, *channelId);
    });
}

long PassThruApi::disconnect(j2534::ULong channelId)
{
    return traced(std::format("PassThruDisconnect channel={}", channelId), [&]() -> long {
        const auto device = deviceOwning(channelId);
        return device ? device->disconnect(channelId) : j2534::status::InvalidChannelId;
    });
}

long PassThruApi::readMessages(j2534::ULong channelId, j2534::PassThruMsg* messages, j2534::ULong* count, j2534::ULong timeoutMs)
{
    return tracedOnFailure(std::format("PassThruReadMsgs channel={}", channelId), [&]() -> long {
        if (messages == nullptr || count == nullptr) {
            return j2534::status::NullParameter;
        }
        const auto device = deviceOwning(channelId);
        if (!device) {
            return j2534::status::InvalidChannelId;
        }
        j2534::ULong read = 0;
        const auto status = device->readMessages(channelId, std::span(messages, *count), read, std::chrono::milliseconds(timeoutMs));
        *count = read;
        return status;
    });
}

long PassThruApi::writeMessages(j2534::ULong channelId, const j2534::PassThruMsg* messages, j2534::ULong* count)
{
    return tracedOnFailure(std::format("PassThruWriteMsgs channel={}", channelId), [&]() -> long {
        if (messages == nullptr || count == nullptr) {
            return j2534::status::NullParameter;
        }
        const auto device = deviceOwning(channelId);
        if (!device) {
            return j2534::status::InvalidChannelId;
        }
        j2534::ULong written = 0;
        const auto requested = *count;
        *count = 0;
        const auto status = device->writeMessages(channelId, std::span(messages, requested), written);
        *count = written;
        return status;
    });
}

long PassThruApi::startPeriodic(j2534::ULong channelId, const j2534::PassThruMsg* message, j2534::ULong* periodicId, j2534::ULong intervalMs)
{
    return traced(std::format("PassThruStartPeriodicMsg channel={} interval={}", channelId, intervalMs), [&]() -> long {
        if (message == nullptr || periodicId == nullptr) {
            return j2534::status::NullParameter;
        }
        const auto device = deviceOwning(channelId);
        return device ? device->startPeriodic(channelId, *message, intervalMs, *periodicId) : j2534::status::InvalidChannelId;
    });
}

long PassThruApi::stopPeriodic(j2534::ULong channelId, j2534::ULong periodicId)
{
    return traced(std::format("PassThruStopPeriodicMsg channel={} id={}", channelId, periodicId), [&]() -> long {
        const auto device = deviceOwning(channelId);
        return device ? device->stopPeriodic(channelId, periodicId) : j2534::status::InvalidChannelId;
    });
}

long PassThruApi::startFilter(
    j2534::ULong channelId,
    j2534::ULong type,
    const j2534::PassThruMsg* mask,
    const j2534::PassThruMsg* pattern,
    const j2534::PassThruMsg* flowControl,
    j2534::ULong* filterId)
{
    return traced(std::format("PassThruStartMsgFilter channel={} type={}", channelId, type), [&]() -> long {
        if (filterId == nullptr) {
            return j2534::status::NullParameter;
        }
        const auto device = deviceOwning(channelId);
        if (!device) {
            return j2534::status::InvalidChannelId;
        }
        return device->startFilter(channelId, type, FilterMessages { mask, pattern, flowControl }, *filterId);
    });
}

long PassThruApi::stopFilter(j2534::ULong channelId, j2534::ULong filterId)
{
    return traced(std::format("PassThruStopMsgFilter channel={} id={}", channelId, filterId), [&]() -> long {
        const auto device = deviceOwning(channelId);
        return device ? device->stopFilter(channelId, filterId) : j2534::status::InvalidChannelId;
    });
}

long PassThruApi::setProgrammingVoltage(j2534::ULong deviceId, j2534::ULong pin, j2534::ULong voltage)
{
    return traced(std::format("PassThruSetProgrammingVoltage pin={} voltage={:#x}", pin, voltage), [&]() -> long {
        if (!openDevice(deviceId)) {
            return j2534::status::InvalidDeviceId;
        }
        if (voltage == j2534::VoltageOff) {
            return j2534::status::NoError;
        }
        throw StnError("OBDLink adapters cannot supply programming voltage");
    });
}

long PassThruApi::readVersion(j2534::ULong deviceId, char* firmwareVersion, char* dllVersion, char* apiVersion)
{
    return traced("PassThruReadVersion", [&]() -> long {
        if (firmwareVersion == nullptr || dllVersion == nullptr || apiVersion == nullptr) {
            return j2534::status::NullParameter;
        }
        const auto device = openDevice(deviceId);
        if (!device) {
            return j2534::status::InvalidDeviceId;
        }
        copyText(std::format("{} ({})", device->identity().chip, device->identity().device), firmwareVersion);
        copyText(dllVersion_, dllVersion);
        copyText(j2534::ApiVersion, apiVersion);
        return j2534::status::NoError;
    });
}

long PassThruApi::lastError(char* description)
{
    if (description == nullptr) {
        return j2534::status::NullParameter;
    }
    std::lock_guard lock(mutex_);
    copyText(lastError_, description);
    return j2534::status::NoError;
}

long PassThruApi::ioctl(j2534::ULong id, j2534::ULong ioctlId, void* input, void* output)
{
    const auto description = std::format("PassThruIoctl id={} ioctl={:#x}{}", id, ioctlId, describeConfigList(ioctlId, input));
    return traced(description, [&]() -> long {
        const auto device = id == DeviceId ? openDevice(id) : deviceOwning(id);
        if (!device) {
            return id == DeviceId ? j2534::status::InvalidDeviceId : j2534::status::InvalidChannelId;
        }
        if (ioctlId == j2534::ioctl::ReadProgVoltage) {
            return j2534::status::NotSupported;
        }
        if (ioctlId != j2534::ioctl::ReadVbatt) {
            return id == DeviceId ? j2534::status::InvalidChannelId : device->channelIoctl(id, ioctlId, input, output);
        }
        if (output == nullptr) {
            return j2534::status::NullParameter;
        }
        *static_cast<j2534::ULong*>(output) = device->readBatteryMillivolts();
        return j2534::status::NoError;
    });
}

std::shared_ptr<Device> PassThruApi::openDevice(j2534::ULong deviceId)
{
    std::lock_guard lock(mutex_);
    return deviceId == DeviceId ? device_ : nullptr;
}

std::shared_ptr<Device> PassThruApi::deviceOwning(j2534::ULong channelId)
{
    std::lock_guard lock(mutex_);
    return device_ && device_->hasChannel(channelId) ? device_ : nullptr;
}

long PassThruApi::reportFailure(std::string description)
{
    setLastError(std::move(description));
    return j2534::status::Failed;
}

void PassThruApi::setLastError(std::string text)
{
    std::lock_guard lock(mutex_);
    lastError_ = std::move(text);
}

}
