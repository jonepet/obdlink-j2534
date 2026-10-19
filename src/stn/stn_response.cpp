#include "stn/stn_response.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace obdlink {

namespace {

constexpr std::size_t ExtendedIdBytes = 4;

constexpr std::array<std::pair<std::string_view, StnStatus>, 18> StatusLines { {
    { "OK", StnStatus::Ok },
    { "NO DATA", StnStatus::NoData },
    { "?", StnStatus::Rejected },
    { "CAN ERROR", StnStatus::BusFailure },
    { "BUS ERROR", StnStatus::BusFailure },
    { "BUS BUSY", StnStatus::BusFailure },
    { "FB ERROR", StnStatus::BusFailure },
    { "UNABLE TO CONNECT", StnStatus::BusFailure },
    { "FC RX TIMEOUT", StnStatus::FlowControlTimeout },
    { "RMT BUF OVF", StnStatus::FlowControlOverflow },
    { "BUFFER FULL", StnStatus::Overflow },
    { "UART RX OVERFLOW", StnStatus::Overflow },
    { "OUT OF MEMORY", StnStatus::OutOfMemory },
    { "DATA ERROR", StnStatus::CorruptFrame },
    { "STOPPED", StnStatus::Stopped },
    { "LV RESET", StnStatus::DeviceReset },
    { "ACT ALERT", StnStatus::DeviceReset },
    { "LP ALERT", StnStatus::DeviceReset },
} };

std::vector<std::string_view> tokens(std::string_view line)
{
    auto words = line
        | std::views::split(' ')
        | std::views::filter([](auto word) { return !word.empty(); })
        | std::views::transform([](auto word) { return std::string_view(word.begin(), word.end()); });
    return { words.begin(), words.end() };
}

std::optional<std::uint32_t> parseHex(std::string_view text)
{
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (error != std::errc {} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

bool isByte(std::string_view text)
{
    return text.size() == 2 && parseHex(text).has_value();
}

std::optional<std::vector<std::uint8_t>> parseBytes(std::span<const std::string_view> parts)
{
    if (!std::ranges::all_of(parts, isByte)) {
        return std::nullopt;
    }
    auto bytes = parts | std::views::transform([](std::string_view part) {
        return static_cast<std::uint8_t>(*parseHex(part));
    });
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

std::optional<CanFrame> parseStandardFrame(std::span<const std::string_view> parts)
{
    const auto id = parseHex(parts.front());
    auto data = parseBytes(parts.subspan(1));
    if (!id || !data) {
        return std::nullopt;
    }
    return CanFrame { *id, false, std::move(*data) };
}

std::optional<CanFrame> parseExtendedFrame(std::span<const std::string_view> parts)
{
    auto bytes = parseBytes(parts);
    if (!bytes || bytes->size() < ExtendedIdBytes) {
        return std::nullopt;
    }
    const auto idBytes = std::span(*bytes).first(ExtendedIdBytes);
    const auto id = std::ranges::fold_left(idBytes, std::uint32_t { 0 }, [](std::uint32_t value, std::uint8_t byte) {
        return (value << 8) | byte;
    });
    return CanFrame { id, true, { bytes->begin() + ExtendedIdBytes, bytes->end() } };
}

std::optional<CanFrame> parseFrame(std::string_view line)
{
    const auto parts = tokens(line);
    if (parts.empty()) {
        return std::nullopt;
    }
    auto frame = parts.front().size() == 3 ? parseStandardFrame(parts) : parseExtendedFrame(parts);
    if (!frame || frame->data.size() > 8) {
        return std::nullopt;
    }
    return frame;
}

}

StnResponse parseResponseLine(std::string_view line)
{
    if (line.find('<') != std::string_view::npos) {
        return StnStatus::CorruptFrame;
    }
    const auto status = std::ranges::find(StatusLines, line, &std::pair<std::string_view, StnStatus>::first);
    if (status != StatusLines.end()) {
        return status->second;
    }
    if (auto frame = parseFrame(line)) {
        return std::move(*frame);
    }
    return StnStatus::Unrecognized;
}

std::string_view describe(StnStatus status)
{
    static constexpr std::array<std::pair<StnStatus, std::string_view>, 12> descriptions { {
        { StnStatus::Ok, "OK" },
        { StnStatus::NoData, "no response" },
        { StnStatus::Rejected, "command rejected by adapter" },
        { StnStatus::BusFailure, "CAN bus error" },
        { StnStatus::FlowControlTimeout, "ECU sent no flow control frame" },
        { StnStatus::FlowControlOverflow, "ECU flow control overflow: message too long" },
        { StnStatus::Overflow, "adapter buffer overflow, data lost" },
        { StnStatus::OutOfMemory, "message too large for adapter" },
        { StnStatus::CorruptFrame, "corrupt frame received" },
        { StnStatus::Stopped, "adapter operation interrupted" },
        { StnStatus::DeviceReset, "adapter reset or entered low-power mode" },
        { StnStatus::Unrecognized, "unrecognized adapter response" },
    } };
    return std::ranges::find(descriptions, status, &std::pair<StnStatus, std::string_view>::first)->second;
}

}
