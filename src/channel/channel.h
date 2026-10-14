#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "channel/channel_kind.h"
#include "channel/message_filters.h"
#include "channel/rx_queue.h"
#include "isotp/isotp_receiver.h"
#include "j2534/j2534_defs.h"
#include "j2534/timestamp_clock.h"
#include "stn/stn_adapter.h"

namespace obdlink {

struct ChannelSpec {
    ChannelKind kind;
    bool pinSwitched;
};

// J2534 state of one logical channel. Not synchronized: the owning device serializes access.
class Channel {
public:
    static std::optional<ChannelSpec> specFor(j2534::ULong protocolId);

    static long validateConnectFlags(j2534::ULong flags);

    Channel(j2534::ULong protocolId, j2534::ULong flags, j2534::ULong baud, std::size_t rxCapacity);

    j2534::ULong protocolId() const;

    ChannelKind kind() const;

    std::optional<BusSetup> busSetup() const;

    MessageFilters& filters();

    RxQueue& rxQueue();

    long getConfig(j2534::SConfig& item) const;

    long setConfig(const j2534::SConfig& item);

    long prepareTransmit(const j2534::PassThruMsg& message, TransmitRequest& request) const;

    void deliverTransmitted(const j2534::PassThruMsg& message, j2534::ULong timestamp);

    void deliverReceived(const std::vector<ReceivedFrame>& frames, const j2534::TimestampClock& clock);

    long addPeriodic(
        const j2534::PassThruMsg& message,
        j2534::ULong intervalMs,
        std::chrono::steady_clock::time_point now,
        j2534::ULong& periodicId);

    long removePeriodic(j2534::ULong periodicId);

    void clearPeriodic();

    std::vector<std::pair<j2534::ULong, j2534::PassThruMsg>> takeDuePeriodic(std::chrono::steady_clock::time_point now);

    bool hasPeriodic(j2534::ULong periodicId) const;

    std::optional<std::chrono::steady_clock::time_point> nextPeriodicDue() const;

    void recordBackgroundFailure(std::string reason);

    std::optional<std::string> takeBackgroundFailure();

private:
    struct PeriodicMessage {
        j2534::PassThruMsg message;
        std::chrono::milliseconds interval;
        std::chrono::steady_clock::time_point due;
    };

    std::optional<j2534::ULong> pins() const;
    bool extendedIds() const;
    bool permitsIdWidth(bool extended) const;
    void deliverIsoTpFrame(const ReceivedFrame& received, const j2534::TimestampClock& clock);
    void deliverRawFrame(const ReceivedFrame& received, const j2534::TimestampClock& clock);

    j2534::ULong protocolId_;
    ChannelSpec spec_;
    j2534::ULong flags_;
    std::map<j2534::ULong, j2534::ULong> parameters_;
    MessageFilters filters_;
    RxQueue rxQueue_;
    IsoTpReceiver isoTpReceiver_;
    std::map<j2534::ULong, PeriodicMessage> periodic_;
    j2534::ULong nextPeriodicId_ = 1;
    std::optional<std::string> backgroundFailure_;
};

}
