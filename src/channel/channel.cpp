#include "channel/channel.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

#include "channel/bus_presets.h"
#include "j2534/message_codec.h"

namespace obdlink {

namespace {

struct ProtocolEntry {
    j2534::ULong protocolId;
    ChannelSpec spec;
};

// Parameters the STN cannot change; they are reported and accept only their fixed value.
constexpr std::array<std::pair<j2534::ULong, j2534::ULong>, 6> FixedParameters { {
    { j2534::config::BsTx, j2534::UseEcuFlowControl },
    { j2534::config::StminTx, j2534::UseEcuFlowControl },
    { j2534::config::Iso15765WftMax, 0 },
    { j2534::config::BitSamplePoint, 80 },
    { j2534::config::SyncJumpWidth, 15 },
    { j2534::config::CanMixedFormat, 0 },
} };

struct ParameterRule {
    j2534::ULong parameter;
    bool (*accepts)(j2534::ULong);
    long rejection;
};

j2534::ULong idWidthStatus(bool extended)
{
    return extended ? j2534::rx_status::Can29BitId : 0;
}

}

std::optional<ChannelSpec> Channel::specFor(j2534::ULong protocolId)
{
    static constexpr std::array<ProtocolEntry, 4> protocols { {
        { j2534::protocol::Can, { ChannelKind::Can, false } },
        { j2534::protocol::Iso15765, { ChannelKind::Iso15765, false } },
        { j2534::protocol::CanPs, { ChannelKind::Can, true } },
        { j2534::protocol::Iso15765Ps, { ChannelKind::Iso15765, true } },
    } };
    const auto entry = std::ranges::find(protocols, protocolId, &ProtocolEntry::protocolId);
    if (entry == protocols.end()) {
        return std::nullopt;
    }
    return entry->spec;
}

long Channel::validateConnectFlags(j2534::ULong flags)
{
    const auto supported = j2534::connect_flag::Can29BitId | j2534::connect_flag::CanIdBoth;
    return (flags & ~supported) == 0 ? j2534::status::NoError : j2534::status::InvalidFlags;
}

Channel::Channel(j2534::ULong protocolId, j2534::ULong flags, j2534::ULong baud, std::size_t rxCapacity)
    : protocolId_(protocolId)
    , spec_(specFor(protocolId).value())
    , flags_(flags)
    , filters_(spec_.kind)
    , rxQueue_(rxCapacity)
{
    parameters_[j2534::config::DataRate] = baud;
    parameters_[j2534::config::Loopback] = 0;
    if (spec_.kind == ChannelKind::Iso15765) {
        parameters_[j2534::config::Iso15765Bs] = 0;
        parameters_[j2534::config::Iso15765Stmin] = 0;
    }
    if (spec_.pinSwitched) {
        parameters_[j2534::config::J1962Pins] = 0;
    }
}

j2534::ULong Channel::protocolId() const
{
    return protocolId_;
}

ChannelKind Channel::kind() const
{
    return spec_.kind;
}

std::optional<BusSetup> Channel::busSetup() const
{
    const auto selectedPins = pins();
    if (!selectedPins) {
        return std::nullopt;
    }
    const auto format = spec_.kind == ChannelKind::Iso15765 ? FrameFormat::IsoTp : FrameFormat::Raw;
    const auto preset = stnPresetFor(*selectedPins, format, extendedIds());
    if (!preset) {
        return std::nullopt;
    }
    BusSetup setup;
    setup.preset = *preset;
    setup.baud = parameters_.at(j2534::config::DataRate);
    setup.format = format;
    if (format == FrameFormat::Raw) {
        setup.passFilters = filters_.stnPassFilters(extendedIds());
        return setup;
    }
    setup.flowControlFilters = filters_.stnFlowControlFilters();
    setup.flowControlPairs = filters_.flowControlPairs();
    setup.blockSize = static_cast<std::uint8_t>(parameters_.at(j2534::config::Iso15765Bs));
    setup.separationTime = static_cast<std::uint8_t>(parameters_.at(j2534::config::Iso15765Stmin));
    return setup;
}

MessageFilters& Channel::filters()
{
    return filters_;
}

RxQueue& Channel::rxQueue()
{
    return rxQueue_;
}

long Channel::getConfig(j2534::SConfig& item) const
{
    if (const auto found = parameters_.find(item.Parameter); found != parameters_.end()) {
        item.Value = found->second;
        return j2534::status::NoError;
    }
    const auto fixed = std::ranges::find(FixedParameters, item.Parameter, &std::pair<j2534::ULong, j2534::ULong>::first);
    if (fixed == FixedParameters.end()) {
        return j2534::status::NotSupported;
    }
    item.Value = fixed->second;
    return j2534::status::NoError;
}

long Channel::setConfig(const j2534::SConfig& item)
{
    const auto fixed = std::ranges::find(FixedParameters, item.Parameter, &std::pair<j2534::ULong, j2534::ULong>::first);
    if (fixed != FixedParameters.end()) {
        return item.Value == fixed->second ? j2534::status::NoError : j2534::status::NotSupported;
    }
    const auto current = parameters_.find(item.Parameter);
    if (current == parameters_.end()) {
        return j2534::status::NotSupported;
    }
    static constexpr std::array<ParameterRule, 5> rules { {
        { j2534::config::DataRate, [](j2534::ULong value) { return value > 0; }, j2534::status::InvalidBaudrate },
        { j2534::config::Loopback, [](j2534::ULong value) { return value <= 1; }, j2534::status::InvalidIoctlValue },
        { j2534::config::Iso15765Bs, [](j2534::ULong value) { return value <= 0xFF; }, j2534::status::InvalidIoctlValue },
        { j2534::config::Iso15765Stmin, [](j2534::ULong value) { return value <= 0xFF; }, j2534::status::InvalidIoctlValue },
        {
            j2534::config::J1962Pins,
            [](j2534::ULong value) { return value == HighSpeedCanPins || value == MediumSpeedCanPins; },
            j2534::status::PinInvalid,
        },
    } };
    const auto rule = std::ranges::find(rules, item.Parameter, &ParameterRule::parameter);
    if (!rule->accepts(item.Value)) {
        return rule->rejection;
    }
    current->second = item.Value;
    return j2534::status::NoError;
}

long Channel::prepareTransmit(const j2534::PassThruMsg& message, TransmitRequest& request) const
{
    if (message.ProtocolID != protocolId_) {
        return j2534::status::MsgProtocolId;
    }
    const bool isoTp = spec_.kind == ChannelKind::Iso15765;
    const auto maxPayload = isoTp ? j2534::MaxIsoTpPayload : std::size_t { 8 };
    if (message.DataSize < j2534::MessageIdSize || message.DataSize - j2534::MessageIdSize > maxPayload) {
        return j2534::status::InvalidMsg;
    }
    if ((message.TxFlags & j2534::tx_flag::Iso15765AddrType) != 0) {
        return j2534::status::NotSupported;
    }
    const bool extended = (message.TxFlags & j2534::tx_flag::Can29BitId) != 0;
    if (!permitsIdWidth(extended)) {
        return j2534::status::InvalidMsg;
    }
    const auto payload = j2534::payloadOf(message);
    const auto id = j2534::canIdOf(message);
    if (id > (extended ? 0x1FFFFFFFu : 0x7FFu) || (isoTp && payload.empty())) {
        return j2534::status::InvalidMsg;
    }
    if (isoTp && payload.size() > MaxSingleFramePayload && !filters_.hasFlowControlFor(id, extended)) {
        return j2534::status::NoFlowControl;
    }
    request = TransmitRequest {
        id,
        extended,
        { payload.begin(), payload.end() },
        isoTp && (message.TxFlags & j2534::tx_flag::Iso15765FramePad) != 0,
    };
    return j2534::status::NoError;
}

void Channel::deliverTransmitted(const j2534::PassThruMsg& message, j2534::ULong timestamp)
{
    const bool extended = (message.TxFlags & j2534::tx_flag::Can29BitId) != 0;
    const auto echoStatus = j2534::rx_status::TxMsgType | idWidthStatus(extended);
    if (spec_.kind == ChannelKind::Iso15765) {
        rxQueue_.push(j2534::makeIndication(protocolId_, echoStatus | j2534::rx_status::TxIndication, timestamp, j2534::canIdOf(message)));
    }
    if (parameters_.at(j2534::config::Loopback) == 0) {
        return;
    }
    auto echo = message;
    echo.RxStatus = echoStatus;
    echo.Timestamp = timestamp;
    echo.ExtraDataIndex = echo.DataSize;
    rxQueue_.push(echo);
}

void Channel::deliverReceived(const std::vector<ReceivedFrame>& frames, const j2534::TimestampClock& clock)
{
    for (const auto& received : frames) {
        if (spec_.kind == ChannelKind::Iso15765) {
            deliverIsoTpFrame(received, clock);
            continue;
        }
        deliverRawFrame(received, clock);
    }
}

long Channel::addPeriodic(
    const j2534::PassThruMsg& message,
    j2534::ULong intervalMs,
    std::chrono::steady_clock::time_point now,
    j2534::ULong& periodicId)
{
    if (intervalMs < 5 || intervalMs > 65535) {
        return j2534::status::InvalidTimeInterval;
    }
    if (periodic_.size() >= 10) {
        return j2534::status::ExceededLimit;
    }
    TransmitRequest request;
    if (const auto status = prepareTransmit(message, request); status != j2534::status::NoError) {
        return status;
    }
    const std::chrono::milliseconds interval(intervalMs);
    periodicId = nextPeriodicId_++;
    periodic_.emplace(periodicId, PeriodicMessage { message, interval, now + interval });
    return j2534::status::NoError;
}

long Channel::removePeriodic(j2534::ULong periodicId)
{
    return periodic_.erase(periodicId) == 1 ? j2534::status::NoError : j2534::status::InvalidMsgId;
}

void Channel::clearPeriodic()
{
    periodic_.clear();
}

std::vector<std::pair<j2534::ULong, j2534::PassThruMsg>> Channel::takeDuePeriodic(std::chrono::steady_clock::time_point now)
{
    auto due = periodic_ | std::views::filter([now](const auto& entry) { return entry.second.due <= now; });
    auto messages = due
        | std::views::transform([](const auto& entry) { return std::pair { entry.first, entry.second.message }; })
        | std::ranges::to<std::vector>();
    std::ranges::for_each(due, [now](auto& entry) { entry.second.due = now + entry.second.interval; });
    return messages;
}

bool Channel::hasPeriodic(j2534::ULong periodicId) const
{
    return periodic_.contains(periodicId);
}

std::optional<std::chrono::steady_clock::time_point> Channel::nextPeriodicDue() const
{
    if (periodic_.empty()) {
        return std::nullopt;
    }
    return std::ranges::min(periodic_ | std::views::values | std::views::transform(&PeriodicMessage::due));
}

void Channel::recordBackgroundFailure(std::string reason)
{
    if (!backgroundFailure_) {
        backgroundFailure_ = std::move(reason);
    }
}

std::optional<std::string> Channel::takeBackgroundFailure()
{
    return std::exchange(backgroundFailure_, std::nullopt);
}

std::optional<j2534::ULong> Channel::pins() const
{
    if (!spec_.pinSwitched) {
        return HighSpeedCanPins;
    }
    const auto selected = parameters_.at(j2534::config::J1962Pins);
    if (selected == 0) {
        return std::nullopt;
    }
    return selected;
}

bool Channel::extendedIds() const
{
    return (flags_ & j2534::connect_flag::Can29BitId) != 0;
}

bool Channel::permitsIdWidth(bool extended) const
{
    if ((flags_ & j2534::connect_flag::CanIdBoth) != 0) {
        return true;
    }
    return extended == extendedIds();
}

void Channel::deliverIsoTpFrame(const ReceivedFrame& received, const j2534::TimestampClock& clock)
{
    const auto& frame = received.frame;
    if (!filters_.acceptsIsoTpFrom(frame.id, frame.extended)) {
        return;
    }
    const auto timestamp = clock.at(received.receivedAt);
    const auto widthStatus = idWidthStatus(frame.extended);
    for (const auto& event : isoTpReceiver_.accept(frame)) {
        if (const auto* message = std::get_if<IsoTpMessage>(&event)) {
            rxQueue_.push(j2534::makeMessage(protocolId_, widthStatus, timestamp, message->id, message->payload));
            continue;
        }
        rxQueue_.push(j2534::makeIndication(protocolId_, widthStatus | j2534::rx_status::StartOfMessage, timestamp, frame.id));
    }
}

void Channel::deliverRawFrame(const ReceivedFrame& received, const j2534::TimestampClock& clock)
{
    const auto& frame = received.frame;
    const auto message = j2534::makeMessage(protocolId_, idWidthStatus(frame.extended), clock.at(received.receivedAt), frame.id, frame.data);
    if (!filters_.acceptsFrame(std::span<const std::uint8_t>(message.Data, message.DataSize))) {
        return;
    }
    rxQueue_.push(message);
}

}
