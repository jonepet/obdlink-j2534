#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>

#include "channel/channel.h"
#include "channel/message_filters.h"
#include "device/device_settings.h"
#include "j2534/j2534_defs.h"
#include "j2534/timestamp_clock.h"
#include "log/trace_log.h"
#include "stn/stn_adapter.h"
#include "stn/stn_link.h"
#include "transport/byte_stream.h"

namespace obdlink {

// One opened adapter. Adapter I/O is serialized by the link mutex; channel state by the
// registry mutex, which is always taken after the link mutex and never held during I/O.
// While no caller needs the link, a monitor thread holds it to receive frames for channels with
// receive filters; every other user takes the link through LinkUse, which stops that monitor.
class Device {
public:
    Device(std::unique_ptr<ByteStream> stream, DeviceSettings settings, j2534::ULong firstChannelId);

    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    void open();

    void close();

    const AdapterIdentity& identity() const;

    bool hasChannel(j2534::ULong channelId) const;

    long connect(j2534::ULong protocolId, j2534::ULong flags, j2534::ULong baud, j2534::ULong& channelId);

    long disconnect(j2534::ULong channelId);

    long readMessages(j2534::ULong channelId, std::span<j2534::PassThruMsg> out, j2534::ULong& count, std::chrono::milliseconds timeout);

    long writeMessages(j2534::ULong channelId, std::span<const j2534::PassThruMsg> messages, j2534::ULong& count);

    long startFilter(j2534::ULong channelId, j2534::ULong type, const FilterMessages& messages, j2534::ULong& filterId);

    long stopFilter(j2534::ULong channelId, j2534::ULong filterId);

    long startPeriodic(j2534::ULong channelId, const j2534::PassThruMsg& message, j2534::ULong intervalMs, j2534::ULong& periodicId);

    long stopPeriodic(j2534::ULong channelId, j2534::ULong periodicId);

    long channelIoctl(j2534::ULong channelId, j2534::ULong ioctlId, void* input, void* output);

    unsigned readBatteryMillivolts();

    void note(std::string_view text);

private:
    using IoctlHandler = long (Device::*)(Channel&, void*, void*);

    class LinkUse {
    public:
        explicit LinkUse(Device& device);
        ~LinkUse();
        LinkUse(const LinkUse&) = delete;
        LinkUse& operator=(const LinkUse&) = delete;

    private:
        Device& device_;
        std::unique_lock<std::mutex> lock_;
    };

    void restartMonitor();
    void runMonitor(std::stop_token stop);
    void monitorLocked(const BusSetup& setup, std::stop_token stop);
    std::optional<BusSetup> monitoredBus() const;
    void deliverMonitored(const BusSetup& setup, ReceivedFrame frame);

    bool protocolConnected(j2534::ULong protocolId) const;
    bool protocolConnectedLocked(j2534::ULong protocolId) const;
    std::shared_ptr<Channel> findChannel(j2534::ULong channelId) const;
    void throwBackgroundFailure(Channel& channel) const;
    void requireAvailable() const;
    void initializeAt(unsigned baud);
    void ensureBus(const BusSetup& setup);
    struct DuePeriodic {
        std::shared_ptr<Channel> channel;
        j2534::ULong periodicId;
        j2534::PassThruMsg message;
    };

    long send(const std::shared_ptr<Channel>& channel, const j2534::PassThruMsg& message);
    long sendPeriodic(const std::shared_ptr<Channel>& channel, j2534::ULong periodicId, const j2534::PassThruMsg& message);
    std::optional<std::string> sendPeriodicReportingFailure(const DuePeriodic& entry);
    bool stillScheduled(const std::shared_ptr<Channel>& channel, j2534::ULong periodicId) const;
    long sendLocked(const std::shared_ptr<Channel>& channel, const j2534::PassThruMsg& message);
    void transmit(
        const std::shared_ptr<Channel>& channel,
        const j2534::PassThruMsg& message,
        const TransmitRequest& request,
        const BusSetup& setup);
    void runScheduler(std::stop_token stop);
    void schedulePeriodic(std::stop_token stop);
    void sendDuePeriodic(std::unique_lock<std::mutex>& registryLock);
    void notifyScheduler();

    template <typename Operation>
    auto withLink(Operation operation);

    long getConfig(Channel& channel, void* input, void* output);
    long setConfig(Channel& channel, void* input, void* output);
    long clearTxBuffer(Channel& channel, void* input, void* output);
    long clearRxBuffer(Channel& channel, void* input, void* output);
    long clearPeriodicMessages(Channel& channel, void* input, void* output);
    long clearFilters(Channel& channel, void* input, void* output);
    long unsupportedIoctl(Channel& channel, void* input, void* output);

    std::unique_ptr<ByteStream> stream_;
    DeviceSettings settings_;
    TraceLog trace_;
    StnLink link_;
    StnAdapter adapter_;
    j2534::TimestampClock clock_;
    AdapterIdentity identity_;
    mutable std::mutex linkMutex_;
    mutable std::mutex registryMutex_;
    std::condition_variable_any schedulerWake_;
    std::map<j2534::ULong, std::shared_ptr<Channel>> channels_;
    j2534::ULong nextChannelId_;
    std::optional<BusSetup> appliedBus_;
    bool protocolOpen_ = false;
    std::atomic<bool> unavailable_ { false };
    std::atomic<bool> closed_ { false };
    unsigned long schedulerGeneration_ = 0;
    std::mutex monitorMutex_;
    std::condition_variable_any monitorWake_;
    std::atomic<unsigned> linkWaiters_ { 0 };
    unsigned long linkReleases_ = 0;
    std::jthread scheduler_;
    std::jthread monitor_;
};

}
