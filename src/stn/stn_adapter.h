#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "stn/can_frame.h"
#include "stn/stn_link.h"
#include "stn/stn_response.h"

namespace obdlink {

struct AdapterTiming {
    std::chrono::milliseconds commandTimeout;
    std::chrono::milliseconds resetTimeout;
    std::chrono::milliseconds responsePendingTimeout;
    std::chrono::milliseconds busyProbe;
};

struct AdapterIdentity {
    std::string chip;
    std::string device;
};

struct StnFilter {
    std::uint32_t id = 0;
    std::uint32_t mask = 0;
    bool extended = false;

    bool operator==(const StnFilter&) const = default;
};

struct FlowControlPair {
    std::uint32_t testerId = 0;
    std::uint32_t ecuId = 0;
    bool extended = false;

    bool operator==(const FlowControlPair&) const = default;
};

enum class FrameFormat { IsoTp, Raw };

struct BusSetup {
    int preset = 0;
    unsigned baud = 0;
    FrameFormat format = FrameFormat::IsoTp;
    std::vector<StnFilter> flowControlFilters;
    std::vector<StnFilter> passFilters;
    std::vector<FlowControlPair> flowControlPairs;
    std::uint8_t blockSize = 0;
    std::uint8_t separationTime = 0;

    bool operator==(const BusSetup&) const = default;
};

struct ReceivedFrame {
    CanFrame frame;
    std::chrono::steady_clock::time_point receivedAt;
};

struct TransmitRequest {
    std::uint32_t id = 0;
    bool extended = false;
    std::vector<std::uint8_t> data;
    bool padded = true;
};

struct TransmitOutcome {
    std::vector<ReceivedFrame> frames;
    std::optional<StnStatus> failure;
    std::string failureLine;
    bool dataLost = false;
};

// Maps bus configuration and transmissions onto the STN command set. Responses are only
// received while a transmit command runs, so every transmit also returns what arrived.
class StnAdapter {
public:
    StnAdapter(StnLink& link, AdapterTiming timing);

    AdapterIdentity initialize(unsigned powerUpBaud);

    void switchBaudRate(unsigned baud);

    using FrameSink = std::function<void(ReceivedFrame)>;

    // Brings the adapter from the current setup (none when unknown) to the requested one,
    // selecting the protocol again only when the preset, bit rate or frame format changes.
    // Returns whether it did, which leaves the protocol closed.
    bool applyBus(const BusSetup& setup, const std::optional<BusSetup>& current);

    void openProtocol();

    // Receives frames with the current filters until keepRunning() turns false; closes the protocol.
    void monitor(std::chrono::milliseconds poll, const FrameSink& sink, const std::function<bool()>& keepRunning);

    TransmitOutcome transmit(const TransmitRequest& request);

    unsigned readBatteryMillivolts();

    void closeProtocol();

private:
    std::chrono::milliseconds transmitSilenceAllowance(std::size_t payloadSize) const;
    void expectOk(const std::string& command);
    std::string singleLine(const std::string& command);
    void applyPadding(bool padded);
    void selectProtocol(const BusSetup& setup);
    void applyFilters(const BusSetup& setup);
    void applyFlowControl(const BusSetup& setup);

    StnLink& link_;
    AdapterTiming timing_;
    std::optional<bool> padded_;
};

}
