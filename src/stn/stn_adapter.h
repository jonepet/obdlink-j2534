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
    std::chrono::milliseconds responseTimeout;
    std::chrono::milliseconds transferSilenceTimeout;
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
    bool dataLost = false;
};

// Maps bus configuration and transmissions onto the STN command set. Responses are only
// received while a transmit command runs, so every transmit also returns what arrived.
class StnAdapter {
public:
    StnAdapter(StnLink& link, AdapterTiming timing);

    AdapterIdentity initialize();

    void switchBaudRate(unsigned baud);

    void applyBus(const BusSetup& setup);

    TransmitOutcome transmit(const TransmitRequest& request);

    unsigned readBatteryMillivolts();

    void closeProtocol();

private:
    std::chrono::milliseconds transmitSilenceAllowance(std::size_t payloadSize) const;
    void expectOk(const std::string& command);
    std::string singleLine(const std::string& command);
    void applyPadding(bool padded);
    void applyFilters(const BusSetup& setup);
    void applyFlowControl(const BusSetup& setup);

    StnLink& link_;
    AdapterTiming timing_;
    std::optional<bool> padded_;
};

}
