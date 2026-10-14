#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "device/device.h"
#include "device/device_settings.h"
#include "j2534/j2534_defs.h"
#include "transport/byte_stream.h"

namespace obdlink {

// J2534-1 v04.04 semantics behind the exported functions: handle validation, status codes,
// and the last-error text. Holds at most one open device. Open and close serialize on the
// lifecycle mutex, which may be held across adapter I/O; the state mutex never is.
class PassThruApi {
public:
    using SettingsLoader = std::function<DeviceSettings()>;
    using StreamFactory = std::function<std::unique_ptr<ByteStream>(const DeviceSettings&)>;

    PassThruApi(SettingsLoader loadSettings, StreamFactory openStream, std::string dllVersion);

    long open(const void* name, j2534::ULong* deviceId);
    long close(j2534::ULong deviceId);
    long connect(j2534::ULong deviceId, j2534::ULong protocolId, j2534::ULong flags, j2534::ULong baud, j2534::ULong* channelId);
    long disconnect(j2534::ULong channelId);
    long readMessages(j2534::ULong channelId, j2534::PassThruMsg* messages, j2534::ULong* count, j2534::ULong timeoutMs);
    long writeMessages(j2534::ULong channelId, const j2534::PassThruMsg* messages, j2534::ULong* count);
    long startPeriodic(j2534::ULong channelId, const j2534::PassThruMsg* message, j2534::ULong* periodicId, j2534::ULong intervalMs);
    long stopPeriodic(j2534::ULong channelId, j2534::ULong periodicId);
    long startFilter(
        j2534::ULong channelId,
        j2534::ULong type,
        const j2534::PassThruMsg* mask,
        const j2534::PassThruMsg* pattern,
        const j2534::PassThruMsg* flowControl,
        j2534::ULong* filterId);
    long stopFilter(j2534::ULong channelId, j2534::ULong filterId);
    long setProgrammingVoltage(j2534::ULong deviceId, j2534::ULong pin, j2534::ULong voltage);
    long readVersion(j2534::ULong deviceId, char* firmwareVersion, char* dllVersion, char* apiVersion);
    long lastError(char* description);
    long ioctl(j2534::ULong id, j2534::ULong ioctlId, void* input, void* output);

    long reportFailure(std::string description);

    static constexpr j2534::ULong DeviceId = 1;

private:
    std::shared_ptr<Device> openDevice(j2534::ULong deviceId);
    std::shared_ptr<Device> deviceOwning(j2534::ULong channelId);

    template <typename Call>
    long guarded(Call call);

    template <typename Call>
    long traced(const std::string& description, Call call);

    template <typename Call>
    long tracedOnFailure(const std::string& description, Call call);

    void trace(const std::string& description, long status);
    void setLastError(std::string text);

    SettingsLoader loadSettings_;
    StreamFactory openStream_;
    std::string dllVersion_;
    std::mutex lifecycleMutex_;
    std::mutex mutex_;
    std::shared_ptr<Device> device_;
    std::string lastError_;
};

}
