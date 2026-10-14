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
    return std::ranges::contains(
        std::array {
            StnStatus::Rejected,
            StnStatus::BusFailure,
            StnStatus::FlowControlTimeout,
            StnStatus::OutOfMemory,
            StnStatus::Stopped,
            StnStatus::DeviceReset,
        },
        status);
}

bool losesData(StnStatus status)
{
    return status == StnStatus::Overflow || status == StnStatus::CorruptFrame;
}

}

StnAdapter::StnAdapter(StnLink& link, AdapterTiming timing)
    : link_(link)
    , timing_(timing)
{
}

AdapterIdentity StnAdapter::initialize()
{
    link_.resynchronize(timing_.busyProbe, timing_.commandTimeout);
    link_.execute("ATZ", timing_.resetTimeout);
    for (const auto setting : std::array<std::string_view, 8> { "ATE0", "ATL0", "ATS1", "ATH1", "ATD0", "ATCAF1", "ATCFC1", "ATAT1" }) {
        expectOk(std::string(setting));
    }
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

void StnAdapter::applyBus(const BusSetup& setup)
{
    expectOk("STPC");
    expectOk(std::format("STP {}", setup.preset));
    const auto baudCommand = std::format("STPBR {}", setup.baud);
    const auto baudLines = link_.execute(baudCommand, timing_.commandTimeout);
    if (!containsOk(baudLines)) {
        throw StnBaudRateRejected(baudCommand + " failed: " + joinedText(baudLines));
    }
    expectOk(setup.format == FrameFormat::IsoTp ? "ATCAF1" : "ATCAF0");
    applyFilters(setup);
    applyFlowControl(setup);
}

TransmitOutcome StnAdapter::transmit(const TransmitRequest& request)
{
    applyPadding(request.padded);

    TransmitOutcome outcome;
    const auto sink = [this, &outcome](const StnLine& line) {
        auto response = parseResponseLine(line.text);
        if (auto* frame = std::get_if<CanFrame>(&response)) {
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
        }
    };

    const auto command = std::format(
        "STPX h:{}, t:{}, l:{}",
        canIdHex(request.id, request.extended),
        timing_.responseTimeout.count(),
        request.data.size());
    link_.executeWithPayload(command, bytesHex(request.data), transmitSilenceAllowance(request.data.size()), sink);
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

// Configured silence timeout plus the adapter's maximum separation time between consecutive
// frames (127 ms, FRPM STCSTM) for each consecutive frame the payload needs.
std::chrono::milliseconds StnAdapter::transmitSilenceAllowance(std::size_t payloadSize) const
{
    const auto consecutiveFrames = payloadSize <= MaxSingleFramePayload ? std::size_t { 0 } : (payloadSize - 6 + 7 - 1) / 7;
    return timing_.transferSilenceTimeout + std::chrono::milliseconds(127) * consecutiveFrames;
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
