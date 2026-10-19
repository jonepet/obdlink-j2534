#include <gtest/gtest.h>

#include <array>

#include "channel/message_filters.h"
#include "j2534/message_codec.h"

namespace obdlink {
namespace {

j2534::PassThruMsg idMessage(std::uint32_t id)
{
    return j2534::makeMessage(j2534::protocol::Iso15765, 0, 0, id, {});
}

j2534::PassThruMsg bytesMessage(std::span<const std::uint8_t> bytes)
{
    j2534::PassThruMsg message {};
    message.ProtocolID = j2534::protocol::Can;
    std::ranges::copy(bytes, message.Data);
    message.DataSize = static_cast<j2534::ULong>(bytes.size());
    return message;
}

TEST(MessageFilters, FlowControlFilterDrivesMatchingAndAdapterSetup)
{
    MessageFilters filters(ChannelKind::Iso15765);
    const auto mask = idMessage(0xFFFFFFFF);
    const auto pattern = idMessage(0x7E8);
    const auto flowControl = idMessage(0x7E0);
    j2534::ULong filterId = 0;

    ASSERT_EQ(filters.add(j2534::filter_type::FlowControl, { &mask, &pattern, &flowControl }, filterId), j2534::status::NoError);

    EXPECT_TRUE(filters.acceptsIsoTpFrom(0x7E8, false));
    EXPECT_FALSE(filters.acceptsIsoTpFrom(0x7E9, false));
    EXPECT_TRUE(filters.hasFlowControlFor(0x7E0, false));
    EXPECT_EQ(filters.stnFlowControlFilters(), (std::vector<StnFilter> { { 0x7E8, 0x7FF, false } }));
    EXPECT_EQ(filters.flowControlPairs(), (std::vector<FlowControlPair> { { 0x7E0, 0x7E8, false } }));
}

TEST(MessageFilters, AcceptsPaddingFlagOnFlowControlMessageOnly)
{
    MessageFilters filters(ChannelKind::Iso15765);
    const auto mask = idMessage(0xFFFFFFFF);
    const auto pattern = idMessage(0x7E8);
    auto flowControl = idMessage(0x7E0);
    flowControl.TxFlags = j2534::tx_flag::Iso15765FramePad;
    j2534::ULong filterId = 0;

    EXPECT_EQ(filters.add(j2534::filter_type::FlowControl, { &mask, &pattern, &flowControl }, filterId), j2534::status::NoError);
}

TEST(MessageFilters, RejectsMixedIdWidthsInOneFilter)
{
    MessageFilters filters(ChannelKind::Iso15765);
    const auto mask = idMessage(0xFFFFFFFF);
    const auto pattern = idMessage(0x7E8);
    auto flowControl = idMessage(0x7E0);
    flowControl.TxFlags = j2534::tx_flag::Can29BitId;
    j2534::ULong filterId = 0;

    EXPECT_EQ(filters.add(j2534::filter_type::FlowControl, { &mask, &pattern, &flowControl }, filterId), j2534::status::InvalidMsg);
}

TEST(MessageFilters, RejectsReuseOfFlowControlIds)
{
    MessageFilters filters(ChannelKind::Iso15765);
    const auto mask = idMessage(0xFFFFFFFF);
    const auto pattern = idMessage(0x7E8);
    const auto flowControl = idMessage(0x7E0);
    j2534::ULong filterId = 0;
    filters.add(j2534::filter_type::FlowControl, { &mask, &pattern, &flowControl }, filterId);

    const auto otherTester = idMessage(0x7E1);

    EXPECT_EQ(filters.add(j2534::filter_type::FlowControl, { &mask, &pattern, &otherTester }, filterId), j2534::status::NotUnique);
}

TEST(MessageFilters, AcceptsSixtyFourFlowControlFiltersAndRejectsMore)
{
    MessageFilters filters(ChannelKind::Iso15765);
    const auto mask = idMessage(0xFFFFFFFF);
    j2534::ULong filterId = 0;
    for (std::uint32_t i = 0; i < 64; ++i) {
        const auto pattern = idMessage(0x600 + i);
        const auto flowControl = idMessage(0x700 + i);
        ASSERT_EQ(filters.add(j2534::filter_type::FlowControl, { &mask, &pattern, &flowControl }, filterId), j2534::status::NoError) << i;
    }

    const auto pattern = idMessage(0x640);
    const auto flowControl = idMessage(0x740);

    EXPECT_EQ(filters.add(j2534::filter_type::FlowControl, { &mask, &pattern, &flowControl }, filterId), j2534::status::ExceededLimit);
    EXPECT_EQ(filters.stnFlowControlFilters().size(), 64u);
}

TEST(MessageFilters, RejectsFilterTypeForWrongChannelKind)
{
    MessageFilters isoTp(ChannelKind::Iso15765);
    MessageFilters raw(ChannelKind::Can);
    const auto mask = idMessage(0xFFFFFFFF);
    const auto pattern = idMessage(0x7E8);
    j2534::ULong filterId = 0;

    EXPECT_EQ(isoTp.add(j2534::filter_type::Pass, { &mask, &pattern, nullptr }, filterId), j2534::status::InvalidFilterId);
    EXPECT_EQ(raw.add(j2534::filter_type::FlowControl, { &mask, &pattern, &pattern }, filterId), j2534::status::InvalidFilterId);
}

TEST(MessageFilters, RequiresMaskAndPattern)
{
    MessageFilters filters(ChannelKind::Iso15765);
    const auto pattern = idMessage(0x7E8);
    j2534::ULong filterId = 0;

    EXPECT_EQ(filters.add(j2534::filter_type::FlowControl, { nullptr, &pattern, &pattern }, filterId), j2534::status::NullParameter);
}

TEST(MessageFilters, BlockFilterOverridesPassFilter)
{
    MessageFilters filters(ChannelKind::Can);
    const std::array<std::uint8_t, 4> idMask { 0xFF, 0xFF, 0xFF, 0x00 };
    const std::array<std::uint8_t, 4> idPattern { 0x00, 0x00, 0x07, 0x00 };
    const std::array<std::uint8_t, 4> blockedMask { 0xFF, 0xFF, 0xFF, 0xFF };
    const std::array<std::uint8_t, 4> blockedPattern { 0x00, 0x00, 0x07, 0x22 };
    const auto passMask = bytesMessage(idMask);
    const auto passPattern = bytesMessage(idPattern);
    const auto blockMask = bytesMessage(blockedMask);
    const auto blockPattern = bytesMessage(blockedPattern);
    j2534::ULong filterId = 0;
    filters.add(j2534::filter_type::Pass, { &passMask, &passPattern, nullptr }, filterId);
    filters.add(j2534::filter_type::Block, { &blockMask, &blockPattern, nullptr }, filterId);

    const std::array<std::uint8_t, 5> accepted { 0x00, 0x00, 0x07, 0x21, 0x01 };
    const std::array<std::uint8_t, 5> blocked { 0x00, 0x00, 0x07, 0x22, 0x01 };
    const std::array<std::uint8_t, 5> unmatched { 0x00, 0x00, 0x06, 0x21, 0x01 };

    EXPECT_TRUE(filters.acceptsFrame(accepted));
    EXPECT_FALSE(filters.acceptsFrame(blocked));
    EXPECT_FALSE(filters.acceptsFrame(unmatched));
    EXPECT_EQ(filters.stnPassFilters(false), (std::vector<StnFilter> { { 0x700, 0x700, false } }));
}

}
}
