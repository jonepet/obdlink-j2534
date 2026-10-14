#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>

#include "fake_stn.h"
#include "stn/stn_error.h"
#include "stn/stn_link.h"

namespace obdlink {
namespace {

using namespace std::chrono_literals;

std::vector<std::string> texts(const std::vector<StnLine>& lines)
{
    std::vector<std::string> result;
    std::ranges::transform(lines, std::back_inserter(result), &StnLine::text);
    return result;
}

TEST(StnLink, ReturnsResponseLinesUpToPrompt)
{
    testing::FakeStn device;
    device.respond("STI", { "STN2232 v5.10.3" });
    StnLink link(device);

    EXPECT_EQ(texts(link.execute("STI", 200ms)), (std::vector<std::string> { "STN2232 v5.10.3" }));
}

TEST(StnLink, SendsPayloadAfterDataPrompt)
{
    testing::FakeStn device;
    device.respondToPayload("1003", { "7E8 06 50 03 00 32 01 F4" });
    StnLink link(device);
    std::vector<std::string> received;

    link.executeWithPayload("STPX h:7E0, t:100, l:2", "1003", 200ms, [&](const StnLine& line) { received.push_back(line.text); });

    EXPECT_EQ(device.commands(), (std::vector<std::string> { "STPX h:7E0, t:100, l:2", "PAYLOAD 1003" }));
    EXPECT_EQ(received, (std::vector<std::string> { "7E8 06 50 03 00 32 01 F4" }));
}

TEST(StnLink, ResynchronizesIdleDeviceWithoutLoneCarriageReturn)
{
    testing::FakeStn device;
    StnLink link(device);

    link.resynchronize(20ms, 200ms);

    EXPECT_EQ(device.commands(), (std::vector<std::string> { "X" }));
}

TEST(StnLink, TimesOutWhenDeviceStaysSilent)
{
    testing::FakeStn device;
    device.silence("ATI");
    StnLink link(device);

    EXPECT_THROW(link.execute("ATI", 50ms), StnTimeout);
}

TEST(StnLink, TracesTrafficInBothDirections)
{
    testing::FakeStn device;
    StnLink link(device);
    std::vector<std::pair<TraceDirection, std::string>> trace;
    link.setTrace([&](TraceDirection direction, std::string_view text) { trace.emplace_back(direction, std::string(text)); });

    link.execute("ATE0", 200ms);

    ASSERT_EQ(trace.size(), 2u);
    EXPECT_EQ(trace[0], (std::pair<TraceDirection, std::string> { TraceDirection::Sent, "ATE0\r" }));
    EXPECT_EQ(trace[1], (std::pair<TraceDirection, std::string> { TraceDirection::Received, "OK" }));
}

}
}
