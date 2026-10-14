#include "channel/message_filters.h"

#include <algorithm>
#include <ranges>

#include "j2534/message_codec.h"

namespace obdlink {

namespace {

std::vector<std::uint8_t> bytesOf(const j2534::PassThruMsg& message)
{
    return { message.Data, message.Data + message.DataSize };
}

bool matches(const std::vector<std::uint8_t>& mask, const std::vector<std::uint8_t>& pattern, std::span<const std::uint8_t> data)
{
    if (data.size() < mask.size()) {
        return false;
    }
    return std::ranges::all_of(std::views::iota(std::size_t { 0 }, mask.size()), [&](std::size_t i) {
        return (data[i] & mask[i]) == (pattern[i] & mask[i]);
    });
}

std::uint32_t rawId(const std::vector<std::uint8_t>& bytes)
{
    const auto present = std::min(bytes.size(), j2534::MessageIdSize);
    return std::ranges::fold_left(
        std::views::iota(std::size_t { 0 }, j2534::MessageIdSize),
        std::uint32_t { 0 },
        [&](std::uint32_t id, std::size_t i) { return (id << 8) | (i < present ? bytes[i] : 0); });
}

std::uint32_t stnId(const std::vector<std::uint8_t>& bytes, bool extended)
{
    return rawId(bytes) & (extended ? 0x1FFFFFFFu : 0x7FFu);
}

bool sameShape(const j2534::PassThruMsg& a, const j2534::PassThruMsg& b)
{
    return a.DataSize == b.DataSize && a.TxFlags == b.TxFlags;
}

}

MessageFilters::MessageFilters(ChannelKind kind)
    : kind_(kind)
{
}

long MessageFilters::add(j2534::ULong type, const FilterMessages& messages, j2534::ULong& filterId)
{
    if (const auto status = validate(type, messages); status != j2534::status::NoError) {
        return status;
    }
    Filter filter {
        type,
        bytesOf(*messages.mask),
        bytesOf(*messages.pattern),
        type == j2534::filter_type::FlowControl ? bytesOf(*messages.flowControl) : std::vector<std::uint8_t> {},
        (messages.pattern->TxFlags & j2534::tx_flag::Can29BitId) != 0,
    };
    filterId = nextId_++;
    filters_.emplace(filterId, std::move(filter));
    return j2534::status::NoError;
}

long MessageFilters::remove(j2534::ULong filterId)
{
    return filters_.erase(filterId) == 1 ? j2534::status::NoError : j2534::status::InvalidFilterId;
}

void MessageFilters::clear()
{
    filters_.clear();
}

bool MessageFilters::acceptsFrame(std::span<const std::uint8_t> idAndData) const
{
    const auto passes = std::ranges::any_of(filtersOfType(j2534::filter_type::Pass), [&](const Filter* filter) {
        return matches(filter->mask, filter->pattern, idAndData);
    });
    const auto blocked = std::ranges::any_of(filtersOfType(j2534::filter_type::Block), [&](const Filter* filter) {
        return matches(filter->mask, filter->pattern, idAndData);
    });
    return passes && !blocked;
}

bool MessageFilters::acceptsIsoTpFrom(std::uint32_t ecuId, bool extended) const
{
    const auto message = j2534::makeMessage(0, 0, 0, ecuId, {});
    const std::span<const std::uint8_t> id(message.Data, j2534::MessageIdSize);
    return std::ranges::any_of(filtersOfType(j2534::filter_type::FlowControl), [&](const Filter* filter) {
        return filter->extended == extended && matches(filter->mask, filter->pattern, id);
    });
}

bool MessageFilters::hasFlowControlFor(std::uint32_t testerId, bool extended) const
{
    return std::ranges::any_of(filtersOfType(j2534::filter_type::FlowControl), [&](const Filter* filter) {
        return filter->extended == extended && rawId(filter->flowControl) == testerId;
    });
}

std::vector<StnFilter> MessageFilters::stnFlowControlFilters() const
{
    auto converted = filtersOfType(j2534::filter_type::FlowControl) | std::views::transform([](const Filter* filter) {
        return StnFilter { stnId(filter->pattern, filter->extended), stnId(filter->mask, filter->extended), filter->extended };
    });
    return { converted.begin(), converted.end() };
}

std::vector<StnFilter> MessageFilters::stnPassFilters(bool extended) const
{
    auto converted = filtersOfType(j2534::filter_type::Pass) | std::views::transform([extended](const Filter* filter) {
        return StnFilter { stnId(filter->pattern, extended), stnId(filter->mask, extended), extended };
    });
    return { converted.begin(), converted.end() };
}

std::vector<FlowControlPair> MessageFilters::flowControlPairs() const
{
    auto converted = filtersOfType(j2534::filter_type::FlowControl) | std::views::transform([](const Filter* filter) {
        return FlowControlPair {
            stnId(filter->flowControl, filter->extended),
            stnId(filter->pattern, filter->extended),
            filter->extended,
        };
    });
    return { converted.begin(), converted.end() };
}

long MessageFilters::validate(j2534::ULong type, const FilterMessages& messages) const
{
    const bool isoTpChannel = kind_ == ChannelKind::Iso15765;
    const bool flowControlType = type == j2534::filter_type::FlowControl;
    const bool knownType = flowControlType || type == j2534::filter_type::Pass || type == j2534::filter_type::Block;
    if (!knownType || flowControlType != isoTpChannel) {
        return j2534::status::InvalidFilterId;
    }
    if (messages.mask == nullptr || messages.pattern == nullptr || (flowControlType && messages.flowControl == nullptr)) {
        return j2534::status::NullParameter;
    }
    if (!sameShape(*messages.mask, *messages.pattern)) {
        return j2534::status::InvalidMsg;
    }
    if (filters_.size() >= 10) {
        return j2534::status::ExceededLimit;
    }
    if (isoTpChannel) {
        return validateIsoTp(messages);
    }
    const auto size = messages.pattern->DataSize;
    return size >= 1 && size <= 12 ? j2534::status::NoError : j2534::status::InvalidMsg;
}

long MessageFilters::validateIsoTp(const FilterMessages& messages) const
{
    if (!sameShape(*messages.pattern, *messages.flowControl)) {
        return j2534::status::InvalidMsg;
    }
    if ((messages.pattern->TxFlags & j2534::tx_flag::Iso15765AddrType) != 0) {
        return j2534::status::NotSupported;
    }
    if (messages.pattern->DataSize != j2534::MessageIdSize) {
        return j2534::status::InvalidMsg;
    }
    const auto ecuId = j2534::canIdOf(*messages.pattern);
    const auto testerId = j2534::canIdOf(*messages.flowControl);
    const auto clashes = std::ranges::any_of(filtersOfType(j2534::filter_type::FlowControl), [&](const Filter* filter) {
        const auto existingEcu = rawId(filter->pattern);
        const auto existingTester = rawId(filter->flowControl);
        return existingEcu == ecuId || existingEcu == testerId || existingTester == ecuId || existingTester == testerId;
    });
    return clashes ? j2534::status::NotUnique : j2534::status::NoError;
}

std::vector<const MessageFilters::Filter*> MessageFilters::filtersOfType(j2534::ULong type) const
{
    auto selected = filters_
        | std::views::values
        | std::views::filter([type](const Filter& filter) { return filter.type == type; })
        | std::views::transform([](const Filter& filter) { return &filter; });
    return { selected.begin(), selected.end() };
}

}
