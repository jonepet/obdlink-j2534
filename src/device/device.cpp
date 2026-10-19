#include "device/device.h"

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <ranges>
#include <utility>
#include <vector>

#include "device/device_error.h"

namespace obdlink {

namespace {

std::string_view withoutTerminator(std::string_view text)
{
    while (!text.empty() && text.back() == '\r') {
        text.remove_suffix(1);
    }
    return text;
}

// ISO 15765-2: the server indicates a request only after its last frame, so the first frame from
// the paired response ID follows the end of the transmit. Without one, the prompt is the bound.
std::chrono::steady_clock::time_point transmittedAt(
    const TransmitRequest& request,
    const BusSetup& setup,
    const std::vector<ReceivedFrame>& frames)
{
    const auto isResponse = [&](const ReceivedFrame& received) {
        return std::ranges::any_of(setup.flowControlPairs, [&](const FlowControlPair& pair) {
            return pair.testerId == request.id
                && pair.extended == request.extended
                && pair.ecuId == received.frame.id
                && pair.extended == received.frame.extended;
        });
    };
    const auto response = std::ranges::find_if(frames, isResponse);
    return response == frames.end() ? std::chrono::steady_clock::now() : response->receivedAt;
}

}

Device::Device(std::unique_ptr<ByteStream> stream, DeviceSettings settings, j2534::ULong firstChannelId)
    : stream_(std::move(stream))
    , settings_(std::move(settings))
    , trace_(settings_.logPath.empty() ? TraceLog() : TraceLog(settings_.logPath))
    , link_(*stream_)
    , adapter_(link_, settings_.timing)
    , nextChannelId_(firstChannelId)
{
    link_.setTrace([this](TraceDirection direction, std::string_view text) {
        trace_.write(direction == TraceDirection::Sent ? "TX" : "RX", withoutTerminator(text));
    });
}

Device::~Device() = default;

Device::LinkUse::LinkUse(Device& device)
    : device_(device)
{
    {
        std::lock_guard wakeLock(device_.monitorMutex_);
        ++device_.linkWaiters_;
    }
    lock_ = std::unique_lock(device_.linkMutex_);
    std::lock_guard wakeLock(device_.monitorMutex_);
    --device_.linkWaiters_;
}

Device::LinkUse::~LinkUse()
{
    lock_.unlock();
    {
        std::lock_guard wakeLock(device_.monitorMutex_);
        ++device_.linkReleases_;
    }
    device_.monitorWake_.notify_all();
}

template <typename Operation>
auto Device::withLink(Operation operation)
{
    LinkUse link(*this);
    requireAvailable();
    try {
        return operation();
    } catch (const StnTimeout& error) {
        trace_.write("ERR", error.what());
        throw;
    }
}

void Device::open()
{
    std::lock_guard linkLock(linkMutex_);
    try {
        initializeAt(settings_.hostBaudRate);
    } catch (const StnTimeout&) {
        if (settings_.adapterBaudRate == 0) {
            throw;
        }
        initializeAt(settings_.adapterBaudRate);
    }
    trace_.write("INFO", std::format("adapter {} / {}", identity_.chip, identity_.device));
    scheduler_ = std::jthread([this](std::stop_token stop) { runScheduler(stop); });
    monitor_ = std::jthread([this](std::stop_token stop) { runMonitor(stop); });
}

void Device::close()
{
    scheduler_.request_stop();
    if (scheduler_.joinable()) {
        scheduler_.join();
    }
    monitor_.request_stop();
    if (monitor_.joinable()) {
        monitor_.join();
    }
    std::exception_ptr failure;
    {
        std::lock_guard linkLock(linkMutex_);
        if (!unavailable_ && !closed_) {
            try {
                adapter_.closeProtocol();
            } catch (const StnError&) {
                failure = std::current_exception();
            }
        }
        closed_ = true;
        stream_.reset();
    }
    {
        std::lock_guard registryLock(registryMutex_);
        for (const auto& channel : channels_ | std::views::values) {
            channel->rxQueue().close();
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

const AdapterIdentity& Device::identity() const
{
    return identity_;
}

bool Device::hasChannel(j2534::ULong channelId) const
{
    return findChannel(channelId) != nullptr;
}

long Device::connect(j2534::ULong protocolId, j2534::ULong flags, j2534::ULong baud, j2534::ULong& channelId)
{
    if (!Channel::specFor(protocolId)) {
        return j2534::status::InvalidProtocolId;
    }
    if (const auto status = Channel::validateConnectFlags(flags); status != j2534::status::NoError) {
        return status;
    }
    if (protocolConnected(protocolId)) {
        return j2534::status::ChannelInUse;
    }
    auto channel = std::make_shared<Channel>(protocolId, flags, baud, 1024);
    if (const auto setup = channel->busSetup()) {
        withLink([this, &setup] { ensureBus(*setup); });
    }
    {
        std::lock_guard registryLock(registryMutex_);
        if (protocolConnectedLocked(protocolId)) {
            return j2534::status::ChannelInUse;
        }
        channelId = nextChannelId_++;
        channels_.emplace(channelId, std::move(channel));
    }
    restartMonitor();
    return j2534::status::NoError;
}

void Device::restartMonitor()
{
    LinkUse link(*this);
}

long Device::disconnect(j2534::ULong channelId)
{
    {
        LinkUse link(*this);
        std::lock_guard registryLock(registryMutex_);
        const auto found = channels_.find(channelId);
        if (found == channels_.end()) {
            return j2534::status::InvalidChannelId;
        }
        found->second->rxQueue().close();
        channels_.erase(found);
    }
    notifyScheduler();
    return j2534::status::NoError;
}

long Device::readMessages(j2534::ULong channelId, std::span<j2534::PassThruMsg> out, j2534::ULong& count, std::chrono::milliseconds timeout)
{
    const auto channel = findChannel(channelId);
    if (!channel) {
        return j2534::status::InvalidChannelId;
    }
    throwBackgroundFailure(*channel);
    const auto messages = channel->rxQueue().pop(out.size(), timeout);
    std::ranges::copy(messages, out.begin());
    count = static_cast<j2534::ULong>(messages.size());
    if (channel->rxQueue().takeOverflow()) {
        return j2534::status::BufferOverflow;
    }
    if (messages.size() == out.size()) {
        return j2534::status::NoError;
    }
    if (messages.empty()) {
        return j2534::status::BufferEmpty;
    }
    return timeout.count() == 0 ? j2534::status::NoError : j2534::status::Timeout;
}

long Device::writeMessages(j2534::ULong channelId, std::span<const j2534::PassThruMsg> messages, j2534::ULong& count)
{
    const auto channel = findChannel(channelId);
    if (!channel) {
        return j2534::status::InvalidChannelId;
    }
    throwBackgroundFailure(*channel);
    count = 0;
    for (const auto& message : messages) {
        if (const auto status = send(channel, message); status != j2534::status::NoError) {
            return status;
        }
        ++count;
    }
    return j2534::status::NoError;
}

long Device::startFilter(j2534::ULong channelId, j2534::ULong type, const FilterMessages& messages, j2534::ULong& filterId)
{
    LinkUse link(*this);
    std::lock_guard registryLock(registryMutex_);
    const auto found = channels_.find(channelId);
    if (found == channels_.end()) {
        return j2534::status::InvalidChannelId;
    }
    return found->second->filters().add(type, messages, filterId);
}

long Device::stopFilter(j2534::ULong channelId, j2534::ULong filterId)
{
    LinkUse link(*this);
    std::lock_guard registryLock(registryMutex_);
    const auto found = channels_.find(channelId);
    if (found == channels_.end()) {
        return j2534::status::InvalidChannelId;
    }
    return found->second->filters().remove(filterId);
}

long Device::startPeriodic(j2534::ULong channelId, const j2534::PassThruMsg& message, j2534::ULong intervalMs, j2534::ULong& periodicId)
{
    long status = j2534::status::InvalidChannelId;
    {
        std::lock_guard registryLock(registryMutex_);
        const auto found = channels_.find(channelId);
        if (found != channels_.end()) {
            status = found->second->addPeriodic(message, intervalMs, std::chrono::steady_clock::now(), periodicId);
        }
    }
    notifyScheduler();
    return status;
}

long Device::stopPeriodic(j2534::ULong channelId, j2534::ULong periodicId)
{
    long status = j2534::status::InvalidChannelId;
    {
        LinkUse link(*this);
        std::lock_guard registryLock(registryMutex_);
        const auto found = channels_.find(channelId);
        if (found != channels_.end()) {
            status = found->second->removePeriodic(periodicId);
        }
    }
    notifyScheduler();
    return status;
}

long Device::channelIoctl(j2534::ULong channelId, j2534::ULong ioctlId, void* input, void* output)
{
    static constexpr std::array<std::pair<j2534::ULong, IoctlHandler>, 11> handlers { {
        { j2534::ioctl::GetConfig, &Device::getConfig },
        { j2534::ioctl::SetConfig, &Device::setConfig },
        { j2534::ioctl::ClearTxBuffer, &Device::clearTxBuffer },
        { j2534::ioctl::ClearRxBuffer, &Device::clearRxBuffer },
        { j2534::ioctl::ClearPeriodicMsgs, &Device::clearPeriodicMessages },
        { j2534::ioctl::ClearMsgFilters, &Device::clearFilters },
        { j2534::ioctl::FiveBaudInit, &Device::unsupportedIoctl },
        { j2534::ioctl::FastInit, &Device::unsupportedIoctl },
        { j2534::ioctl::ClearFunctMsgLookupTable, &Device::unsupportedIoctl },
        { j2534::ioctl::AddToFunctMsgLookupTable, &Device::unsupportedIoctl },
        { j2534::ioctl::DeleteFromFunctMsgLookupTable, &Device::unsupportedIoctl },
    } };
    const auto handler = std::ranges::find(handlers, ioctlId, &std::pair<j2534::ULong, IoctlHandler>::first);
    if (handler == handlers.end()) {
        return j2534::status::InvalidIoctlId;
    }
    long status = j2534::status::InvalidChannelId;
    {
        LinkUse link(*this);
        std::lock_guard registryLock(registryMutex_);
        const auto found = channels_.find(channelId);
        if (found != channels_.end()) {
            status = (this->*handler->second)(*found->second, input, output);
        }
    }
    notifyScheduler();
    return status;
}

unsigned Device::readBatteryMillivolts()
{
    return withLink([this] { return adapter_.readBatteryMillivolts(); });
}

void Device::note(std::string_view text)
{
    trace_.write("API", text);
}

bool Device::protocolConnected(j2534::ULong protocolId) const
{
    std::lock_guard registryLock(registryMutex_);
    return protocolConnectedLocked(protocolId);
}

bool Device::protocolConnectedLocked(j2534::ULong protocolId) const
{
    return std::ranges::any_of(channels_ | std::views::values, [protocolId](const auto& existing) {
        return existing->protocolId() == protocolId;
    });
}

std::shared_ptr<Channel> Device::findChannel(j2534::ULong channelId) const
{
    std::lock_guard registryLock(registryMutex_);
    const auto found = channels_.find(channelId);
    return found == channels_.end() ? nullptr : found->second;
}

void Device::throwBackgroundFailure(Channel& channel) const
{
    std::optional<std::string> failure;
    {
        std::lock_guard registryLock(registryMutex_);
        failure = channel.takeBackgroundFailure();
    }
    if (failure) {
        throw TransmitFailed(*failure);
    }
}

void Device::requireAvailable() const
{
    if (closed_) {
        throw DeviceUnavailable("device is closed");
    }
    if (unavailable_) {
        throw DeviceUnavailable("adapter stopped responding; close and reopen the device");
    }
}

void Device::initializeAt(unsigned baud)
{
    stream_->setBaudRate(baud);
    identity_ = adapter_.initialize(settings_.hostBaudRate);
    if (settings_.adapterBaudRate != 0 && settings_.adapterBaudRate != settings_.hostBaudRate) {
        adapter_.switchBaudRate(settings_.adapterBaudRate);
    }
    appliedBus_.reset();
    protocolOpen_ = false;
}

void Device::ensureBus(const BusSetup& setup)
{
    if (appliedBus_ != setup) {
        const auto current = std::exchange(appliedBus_, std::nullopt);
        if (adapter_.applyBus(setup, current)) {
            protocolOpen_ = false;
        }
        appliedBus_ = setup;
    }
    if (!protocolOpen_) {
        adapter_.openProtocol();
        protocolOpen_ = true;
    }
}

void Device::runMonitor(std::stop_token stop)
{
    unsigned long seenReleases = 0;
    while (!stop.stop_requested()) {
        {
            std::unique_lock wakeLock(monitorMutex_);
            monitorWake_.wait(wakeLock, stop, [&] { return linkWaiters_ == 0 && linkReleases_ != seenReleases; });
            seenReleases = linkReleases_;
        }
        std::unique_lock linkLock(linkMutex_);
        if (stop.stop_requested() || linkWaiters_ > 0 || unavailable_ || closed_) {
            continue;
        }
        if (const auto setup = monitoredBus()) {
            monitorLocked(*setup, stop);
        }
    }
}

void Device::monitorLocked(const BusSetup& setup, std::stop_token stop)
{
    try {
        ensureBus(setup);
        protocolOpen_ = false;
        adapter_.monitor(
            std::chrono::milliseconds(5),
            [this, &setup](ReceivedFrame frame) { deliverMonitored(setup, std::move(frame)); },
            [this, &stop] { return linkWaiters_ == 0 && !stop.stop_requested(); });
    } catch (const StnError& error) {
        trace_.write("ERR", error.what());
        std::lock_guard registryLock(registryMutex_);
        for (const auto& channel : channels_ | std::views::values) {
            if (channel->busSetup() == setup) {
                channel->recordBackgroundFailure(error.what());
            }
        }
    }
}

std::optional<BusSetup> Device::monitoredBus() const
{
    std::lock_guard registryLock(registryMutex_);
    auto receiving = channels_ | std::views::values | std::views::transform([](const auto& channel) {
        return channel->busSetup();
    }) | std::views::filter([](const auto& setup) {
        return setup && (!setup->passFilters.empty() || !setup->flowControlFilters.empty());
    });
    if (std::ranges::contains(receiving, appliedBus_)) {
        return appliedBus_;
    }
    const auto first = receiving.begin();
    return first == receiving.end() ? std::nullopt : *first;
}

void Device::deliverMonitored(const BusSetup& setup, ReceivedFrame frame)
{
    std::lock_guard registryLock(registryMutex_);
    const std::vector<ReceivedFrame> frames { std::move(frame) };
    for (const auto& channel : channels_ | std::views::values) {
        if (channel->busSetup() == setup) {
            channel->deliverReceived(frames, clock_);
        }
    }
}

long Device::send(const std::shared_ptr<Channel>& channel, const j2534::PassThruMsg& message)
{
    return withLink([&] { return sendLocked(channel, message); });
}

long Device::sendPeriodic(const std::shared_ptr<Channel>& channel, j2534::ULong periodicId, const j2534::PassThruMsg& message)
{
    return withLink([&] {
        if (!stillScheduled(channel, periodicId)) {
            return j2534::status::NoError;
        }
        return sendLocked(channel, message);
    });
}

bool Device::stillScheduled(const std::shared_ptr<Channel>& channel, j2534::ULong periodicId) const
{
    std::lock_guard registryLock(registryMutex_);
    const auto registered = std::ranges::contains(channels_ | std::views::values, channel);
    return registered && channel->hasPeriodic(periodicId);
}

long Device::sendLocked(const std::shared_ptr<Channel>& channel, const j2534::PassThruMsg& message)
{
    TransmitRequest request;
    std::optional<BusSetup> setup;
    {
        std::lock_guard registryLock(registryMutex_);
        if (const auto status = channel->prepareTransmit(message, request); status != j2534::status::NoError) {
            return status;
        }
        setup = channel->busSetup();
    }
    if (!setup) {
        return j2534::status::PinInvalid;
    }
    transmit(channel, message, request, *setup);
    return j2534::status::NoError;
}

void Device::transmit(
    const std::shared_ptr<Channel>& channel,
    const j2534::PassThruMsg& message,
    const TransmitRequest& request,
    const BusSetup& setup)
{
    ensureBus(setup);
    const auto outcome = adapter_.transmit(request);

    std::lock_guard registryLock(registryMutex_);
    if (!outcome.failure) {
        channel->deliverTransmitted(message, clock_.at(transmittedAt(request, setup, outcome.frames)));
    }
    channel->deliverReceived(outcome.frames, clock_);
    if (outcome.dataLost) {
        channel->rxQueue().markOverflow();
    }
    if (!outcome.failure) {
        return;
    }
    const auto reason = std::format("{} (adapter: \"{}\")", describe(*outcome.failure), outcome.failureLine);
    trace_.write("ERR", reason);
    if (*outcome.failure == StnStatus::DeviceReset) {
        unavailable_ = true;
        throw DeviceUnavailable(reason);
    }
    throw TransmitFailed(reason);
}

void Device::runScheduler(std::stop_token stop)
{
    try {
        schedulePeriodic(stop);
    } catch (...) {
        unavailable_ = true;
    }
}

void Device::schedulePeriodic(std::stop_token stop)
{
    std::unique_lock registryLock(registryMutex_);
    while (!stop.stop_requested()) {
        const auto generation = schedulerGeneration_;
        const auto changed = [this, generation] { return schedulerGeneration_ != generation; };
        auto dues = channels_ | std::views::values | std::views::transform([](const auto& channel) {
            return channel->nextPeriodicDue();
        }) | std::views::filter([](const auto& due) { return due.has_value(); });
        if (dues.empty()) {
            schedulerWake_.wait(registryLock, stop, changed);
            continue;
        }
        const auto next = **std::ranges::min_element(dues);
        if (schedulerWake_.wait_until(registryLock, stop, next, changed) || stop.stop_requested()) {
            continue;
        }
        sendDuePeriodic(registryLock);
    }
}

void Device::sendDuePeriodic(std::unique_lock<std::mutex>& registryLock)
{
    const auto now = std::chrono::steady_clock::now();
    const auto due = channels_
        | std::views::values
        | std::views::transform([now](const auto& channel) {
              return channel->takeDuePeriodic(now) | std::views::transform([channel](const auto& entry) {
                  return DuePeriodic { channel, entry.first, entry.second };
              }) | std::ranges::to<std::vector>();
          })
        | std::views::join
        | std::ranges::to<std::vector>();
    registryLock.unlock();
    std::vector<std::pair<std::shared_ptr<Channel>, std::string>> failures;
    for (const auto& entry : due) {
        if (auto reason = sendPeriodicReportingFailure(entry)) {
            failures.emplace_back(entry.channel, std::move(*reason));
        }
    }
    registryLock.lock();
    for (const auto& [channel, reason] : failures) {
        trace_.write("ERR", reason);
        channel->recordBackgroundFailure(reason);
    }
}

std::optional<std::string> Device::sendPeriodicReportingFailure(const DuePeriodic& entry)
{
    try {
        const auto status = sendPeriodic(entry.channel, entry.periodicId, entry.message);
        if (status == j2534::status::NoError) {
            return std::nullopt;
        }
        return std::format("periodic message rejected with J2534 status {:#x}", status);
    } catch (const std::exception& error) {
        return std::format("periodic message failed: {}", error.what());
    }
}

void Device::notifyScheduler()
{
    {
        std::lock_guard registryLock(registryMutex_);
        ++schedulerGeneration_;
    }
    schedulerWake_.notify_all();
}

long Device::getConfig(Channel& channel, void* input, void*)
{
    auto* list = static_cast<j2534::SConfigList*>(input);
    if (list == nullptr || (list->NumOfParams > 0 && list->ConfigPtr == nullptr)) {
        return j2534::status::NullParameter;
    }
    for (auto& item : std::span(list->ConfigPtr, list->NumOfParams)) {
        if (const auto status = channel.getConfig(item); status != j2534::status::NoError) {
            return status;
        }
    }
    return j2534::status::NoError;
}

long Device::setConfig(Channel& channel, void* input, void*)
{
    const auto* list = static_cast<const j2534::SConfigList*>(input);
    if (list == nullptr || (list->NumOfParams > 0 && list->ConfigPtr == nullptr)) {
        return j2534::status::NullParameter;
    }
    for (const auto& item : std::span(list->ConfigPtr, list->NumOfParams)) {
        if (const auto status = channel.setConfig(item); status != j2534::status::NoError) {
            return status;
        }
    }
    return j2534::status::NoError;
}

long Device::clearTxBuffer(Channel&, void*, void*)
{
    return j2534::status::NoError;
}

long Device::clearRxBuffer(Channel& channel, void*, void*)
{
    channel.rxQueue().clear();
    return j2534::status::NoError;
}

long Device::clearPeriodicMessages(Channel& channel, void*, void*)
{
    channel.clearPeriodic();
    return j2534::status::NoError;
}

long Device::clearFilters(Channel& channel, void*, void*)
{
    channel.filters().clear();
    return j2534::status::NoError;
}

long Device::unsupportedIoctl(Channel&, void*, void*)
{
    return j2534::status::NotSupported;
}

}
