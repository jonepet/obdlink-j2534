#include <gtest/gtest.h>

#include "stn/stn_response.h"

namespace obdlink {
namespace {

TEST(StnResponse, ParsesStandardFrameWithPci)
{
    const auto response = parseResponseLine("7E8 06 50 03 00 32 01 F4");

    const auto& frame = std::get<CanFrame>(response);
    EXPECT_EQ(frame.id, 0x7E8u);
    EXPECT_FALSE(frame.extended);
    EXPECT_EQ(frame.data, (std::vector<std::uint8_t> { 0x06, 0x50, 0x03, 0x00, 0x32, 0x01, 0xF4 }));
}

TEST(StnResponse, ParsesExtendedFrame)
{
    const auto response = parseResponseLine("18 DA F1 10 03 7F 31 78");

    const auto& frame = std::get<CanFrame>(response);
    EXPECT_EQ(frame.id, 0x18DAF110u);
    EXPECT_TRUE(frame.extended);
    EXPECT_EQ(frame.data, (std::vector<std::uint8_t> { 0x03, 0x7F, 0x31, 0x78 }));
}

TEST(StnResponse, ClassifiesStatusLines)
{
    EXPECT_EQ(std::get<StnStatus>(parseResponseLine("NO DATA")), StnStatus::NoData);
    EXPECT_EQ(std::get<StnStatus>(parseResponseLine("FC RX TIMEOUT")), StnStatus::FlowControlTimeout);
    EXPECT_EQ(std::get<StnStatus>(parseResponseLine("RMT BUF OVF")), StnStatus::FlowControlOverflow);
    EXPECT_EQ(std::get<StnStatus>(parseResponseLine("CAN ERROR")), StnStatus::BusFailure);
    EXPECT_EQ(std::get<StnStatus>(parseResponseLine("BUFFER FULL")), StnStatus::Overflow);
    EXPECT_EQ(std::get<StnStatus>(parseResponseLine("?")), StnStatus::Rejected);
}

TEST(StnResponse, FlagsFrameWithErrorMarkerAsCorrupt)
{
    EXPECT_EQ(std::get<StnStatus>(parseResponseLine("7E8 06 50 03 <DATA ERROR")), StnStatus::CorruptFrame);
}

TEST(StnResponse, RejectsTextAndOversizedFrames)
{
    EXPECT_EQ(std::get<StnStatus>(parseResponseLine("ELM327 v1.4b")), StnStatus::Unrecognized);
    EXPECT_EQ(std::get<StnStatus>(parseResponseLine("7E8 01 02 03 04 05 06 07 08 09")), StnStatus::Unrecognized);
}

}
}
