#include <gtest/gtest.h>

#include "isotp/isotp_receiver.h"

namespace obdlink {
namespace {

CanFrame frame(std::vector<std::uint8_t> data)
{
    return CanFrame { 0x7E8, false, std::move(data) };
}

TEST(IsoTpReceiver, DeliversSingleFramePayload)
{
    IsoTpReceiver receiver;

    const auto events = receiver.accept(frame({ 0x03, 0x7F, 0x31, 0x78, 0x00, 0x00, 0x00, 0x00 }));

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(std::get<IsoTpMessage>(events[0]).payload, (std::vector<std::uint8_t> { 0x7F, 0x31, 0x78 }));
}

TEST(IsoTpReceiver, ReassemblesSegmentedMessageAfterFirstFrameIndication)
{
    IsoTpReceiver receiver;

    const auto first = receiver.accept(frame({ 0x10, 0x0A, 1, 2, 3, 4, 5, 6 }));
    const auto last = receiver.accept(frame({ 0x21, 7, 8, 9, 10, 0xAA, 0xAA, 0xAA }));

    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(std::get<IsoTpFirstFrame>(first[0]).id, 0x7E8u);
    ASSERT_EQ(last.size(), 1u);
    EXPECT_EQ(std::get<IsoTpMessage>(last[0]).payload, (std::vector<std::uint8_t> { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }));
}

TEST(IsoTpReceiver, DropsMessageOnSequenceGap)
{
    IsoTpReceiver receiver;
    receiver.accept(frame({ 0x10, 0x10, 1, 2, 3, 4, 5, 6 }));

    const auto skipped = receiver.accept(frame({ 0x22, 7, 8, 9, 10, 11, 12, 13 }));
    const auto late = receiver.accept(frame({ 0x21, 7, 8, 9, 10, 11, 12, 13 }));

    EXPECT_TRUE(skipped.empty());
    EXPECT_TRUE(late.empty());
}

TEST(IsoTpReceiver, IgnoresFlowControlFrames)
{
    IsoTpReceiver receiver;

    EXPECT_TRUE(receiver.accept(frame({ 0x30, 0x00, 0x00 })).empty());
}

}
}
