#include "stn/stn_adapter.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <numeric>
#include <ranges>
#include <string_view>

#include "isotp/isotp_receiver.h"
#include "stn/stn_error.h"

namespace obdlink {

namespace {

// ISO 15765-2: N_Bs and N_Cr maximum, and the largest STmin a receiver may request.
constexpr std::chrono::milliseconds IsoTpFrameTimeout(1000);
constexpr std::chrono::milliseconds MaxSeparationTime(127);

std::string canIdHex(std::uint32_t id, bool extended)
{
    return extended ? std::format("{:08X}", id) : std::format("{:03X}", id);
}

std::string bytesHex(const std::vector<std::uint8_t>& bytes)
{
    return std::accumulate(bytes.begin(), bytes.end(), std::string {}, [](std::string text, std::uint8_t byte) {
        return std::move(text) + std::format("{:02X}", byte);
    });
}

std::string joinedText(const std::vector<StnLine>& lines)
{
    auto texts = lines | std::views::transform(&StnLine::text) | std::views::join_with(std::string_view(" / "));
    return { texts.begin(), texts.end() };
}

bool containsOk(const std::vector<StnLine>& lines)
{
    return std::ranges::any_of(lines, [](const StnLine& line) { return line.text == "OK"; });
}

bool isTransmitFailure(StnStatus status)
{
    return !std::ranges::contains(
        std::array {
            StnStatus::Ok,
            StnStatus::NoData,
            StnStatus::Overflow,
            StnStatus::CorruptFrame,
        },
        status);
}

bool losesData(StnStatus status)
{
    return status == StnStatus::Overflow || status == StnStatus::CorruptFrame;
}

// ISO 14229-1: negative response 0x7F, service, NRC 0x78 (requestCorrectlyReceived-ResponsePending).
bool isResponsePending(const std::vector<std::uint8_t>& payload)
{
    return payload.size() == 3 && payload[0] == 0x7F && payload[2] == 0x78;
}

bool completesResponse(IsoTpReceiver& receiver, const CanFrame& frame)
{
    return std::ranges::any_of(receiver.accept(frame), [](const IsoTpEvent& event) {
        const auto* message = std::get_if<IsoTpMessage>(&event);
        return message && !isResponsePending(message->payload);
    });
}

}

StnAdapter::StnAdapter(StnLink& link, AdapterTiming timing)
    : link_(link)
    , timing_(timing)
{
}

AdapterIdentity StnAdapter::initialize(unsigned powerUpBaud)
{
    link_.resynchronize(timing_.busyProbe, timing_.commandTimeout);
    link_.reset(powerUpBaud, timing_.resetTimeout);
    static constexpr std::array<std::string_view, 9> settings {
        "ATE0", "ATL0", "ATS1", "ATH1", "ATD0", "ATCAF1", "ATCFC1", "ATAT1", "STCMM 1",
    };
    for (const auto setting : settings) {
        expectOk(std::string(setting));
    }
    expectOk(std::format("STCTOR {}, {}", IsoTpFrameTimeout.count(), IsoTpFrameTimeout.count()));
    padded_.reset();

    auto chip = singleLine("STI");
    if (!chip.starts_with("STN")) {
        throw StnError("adapter is not STN-based (reports \"" + chip + "\")");
    }
    return AdapterIdentity { std::move(chip), singleLine("STDI") };
}

void StnAdapter::switchBaudRate(unsigned baud)
{
    link_.switchBaudRate(baud, timing_.commandTimeout);
}

bool StnAdapter::applyBus(const BusSetup& setup, const std::optional<BusSetup>& current)
{
    const bool reselect = !current
        || current->preset != setup.preset
        || current->baud != setup.baud
        || current->format != setup.format;
    format_ = setup.format;
    if (reselect) {
        selectProtocol(setup);
    }
    if (reselect || current->flowControlFilters != setup.flowControlFilters || current->passFilters != setup.passFilters) {
        applyFilters(setup);
    }
    const bool flowControlChanged = reselect
        || current->flowControlPairs != setup.flowControlPairs
        || current->blockSize != setup.blockSize
        || current->separationTime != setup.separationTime;
    if (flowControlChanged) {
        applyFlowControl(setup);
    }
    return reselect;
}

// FRPM 8.6: STP selects the protocol without opening it; STPO opens it.
void StnAdapter::openProtocol()
{
    expectOk("STPO");
}

// FRPM 8.9: STM monitors with the current filters; the adapter sends no flow control frames
// while monitoring, and leaving the monitor closes the protocol.
void StnAdapter::monitor(std::chrono::milliseconds poll, const FrameSink& sink, const std::function<bool()>& keepRunning)
{
    const auto lineSink = [&sink](const StnLine& line) {
        auto response = parseResponseLine(line.text);
        if (auto* frame = std::get_if<CanFrame>(&response)) {
            sink(ReceivedFrame { std::move(*frame), line.receivedAt });
        }
    };
    link_.monitor("STM", poll, timing_.commandTimeout, lineSink, keepRunning);
}

TransmitOutcome StnAdapter::transmit(const TransmitRequest& request)
{
    applyPadding(request.padded);

    TransmitOutcome outcome;
    IsoTpReceiver receiver;
    bool complete = false;
    const bool isoTp = format_ == FrameFormat::IsoTp;
    const auto sink = [this, &outcome, &receiver, &complete, isoTp](const StnLine& line) {
        auto response = parseResponseLine(line.text);
        if (auto* frame = std::get_if<CanFrame>(&response)) {
            complete = complete || (isoTp && completesResponse(receiver, *frame));
            outcome.frames.push_back(ReceivedFrame { std::move(*frame), line.receivedAt });
            return;
        }
        const auto status = std::get<StnStatus>(response);
        if (status == StnStatus::DeviceReset) {
            padded_.reset();
        }
        outcome.dataLost = outcome.dataLost || losesData(status);
        if (isTransmitFailure(status) && !outcome.failure) {
            outcome.failure = status;
            outcome.failureLine = line.text;
        }
    };

    const auto command = std::format("STPX h:{}, l:{}", canIdHex(request.id, request.extended), request.data.size());
    link_.executeWithPayload(
        command,
        bytesHex(request.data),
        transmitSilenceAllowance(request.data.size()),
        sink,
        [&complete] { return complete; });
    if (outcome.failure == StnStatus::BusFailure) {
        outcome.failureLine += ", ATCS " + singleLine("ATCS");
    }
    return outcome;
}

unsigned StnAdapter::readBatteryMillivolts()
{
    const auto text = singleLine("STVR 3");
    double volts = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), volts);
    if (error != std::errc {}) {
        throw StnError("unreadable battery voltage \"" + text + "\"");
    }
    return static_cast<unsigned>(std::lround(volts * 1000));
}

void StnAdapter::closeProtocol()
{
    expectOk("STPC");
}

// ISO 15765-2 bounds the transmit at N_Bs plus the maximum STmin per consecutive frame (block
// size 1); ISO 14229-2 bounds the response at P2*Server_max after each response-pending reply.
std::chrono::milliseconds StnAdapter::transmitSilenceAllowance(std::size_t payloadSize) const
{
    const auto consecutiveFrames = payloadSize <= MaxSingleFramePayload ? std::size_t { 0 } : (payloadSize - 6 + 7 - 1) / 7;
    return timing_.responsePendingTimeout + (IsoTpFrameTimeout + MaxSeparationTime) * consecutiveFrames;
}

void StnAdapter::expectOk(const std::string& command)
{
    const auto lines = link_.execute(command, timing_.commandTimeout);
    if (!containsOk(lines)) {
        throw StnError(command + " failed: " + joinedText(lines));
    }
}

std::string StnAdapter::singleLine(const std::string& command)
{
    const auto lines = link_.execute(command, timing_.commandTimeout);
    if (lines.size() != 1 || lines.front().text == "?") {
        throw StnError(command + " failed: " + joinedText(lines));
    }
    return lines.front().text;
}

void StnAdapter::applyPadding(bool padded)
{
    if (padded_ == padded) {
        return;
    }
    expectOk(padded ? "ATV0" : "ATV1");
    padded_ = padded;
}

void StnAdapter::selectProtocol(const BusSetup& setup)
{
    expectOk("STPC");
    expectOk(std::format("STP {}", setup.preset));
    const auto baudCommand = std::format("STPBR {}", setup.baud);
    const auto baudLines = link_.execute(baudCommand, timing_.commandTimeout);
    if (!containsOk(baudLines)) {
        throw StnBaudRateRejected(baudCommand + " failed: " + joinedText(baudLines));
    }
    expectOk(setup.format == FrameFormat::IsoTp ? "ATCAF1" : "ATCAF0");
}

void StnAdapter::applyFilters(const BusSetup& setup)
{
    expectOk("STFAC");
    for (const auto& filter : setup.flowControlFilters) {
        expectOk(std::format("STFFCA {}, {}", canIdHex(filter.id, filter.extended), canIdHex(filter.mask, filter.extended)));
    }
    for (const auto& filter : setup.passFilters) {
        expectOk(std::format("STFPA {}, {}", canIdHex(filter.id, filter.extended), canIdHex(filter.mask, filter.extended)));
    }
}

void StnAdapter::applyFlowControl(const BusSetup& setup)
{
    expectOk("STCFCPC");
    for (const auto& pair : setup.flowControlPairs) {
        expectOk(std::format("STCFCPA {}, {}", canIdHex(pair.testerId, pair.extended), canIdHex(pair.ecuId, pair.extended)));
    }
    if (setup.blockSize == 0 && setup.separationTime == 0) {
        expectOk("ATFCSM0");
        return;
    }
    expectOk(std::format("ATFCSD 30 {:02X} {:02X}", setup.blockSize, setup.separationTime));
    expectOk("ATFCSM2");
}

}
