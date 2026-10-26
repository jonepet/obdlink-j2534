#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

#include "api/passthru_api.h"
#include "fake_stn.h"
#include "j2534/message_codec.h"

namespace obdlink {
namespace {

using namespace std::chrono_literals;

class SharedStream : public ByteStream {
public:
    explicit SharedStream(std::shared_ptr<testing::FakeStn> device)
        : device_(std::move(device))
    {
    }

    void write(std::string_view bytes) override
    {
        device_->write(bytes);
    }

    std::size_t read(char* buffer, std::size_t capacity, std::chrono::milliseconds timeout) override
    {
        return device_->read(buffer, capacity, timeout);
    }

    void setBaudRate(unsigned baud) override
    {
        device_->setBaudRate(baud);
    }

    void discardInput() override
    {
        device_->discardInput();
    }

private:
    std::shared_ptr<testing::FakeStn> device_;
};

DeviceSettings testSettings()
{
    DeviceSettings settings;
    settings.portName = "COM9";
    settings.hostBaudRate = 115200;
    settings.timing = AdapterTiming { 200ms, 200ms, 300ms, 20ms };
    return settings;
}

j2534::PassThruMsg isoMessage(std::uint32_t id, std::vector<std::uint8_t> payload)
{
    auto message = j2534::makeMessage(j2534::protocol::Iso15765, 0, 0, id, payload);
    message.TxFlags = j2534::tx_flag::Iso15765FramePad;
    return message;
}

std::vector<std::uint8_t> dataOf(const j2534::PassThruMsg& message)
{
    return { message.Data, message.Data + message.DataSize };
}

class PassThruApiTest : public ::testing::Test {
protected:
    PassThruApiTest()
        : device_(std::make_shared<testing::FakeStn>())
        , api_(
              testSettings,
              [this](const DeviceSettings&) -> std::unique_ptr<ByteStream> { return std::make_unique<SharedStream>(device_); },
              "test")
    {
        device_->respond("STI", { "STN2232 v5.10.3" });
        device_->respond("STDI", { "OBDLink EX r1.0.0" });
        device_->respond("STVR 3", { "12.345" });
    }

    void openAndConnect()
    {
        ASSERT_EQ(api_.open(nullptr, &deviceId_), j2534::status::NoError);
        ASSERT_EQ(api_.connect(deviceId_, j2534::protocol::Iso15765, 0, 500000, &channelId_), j2534::status::NoError);
        const auto mask = j2534::makeMessage(j2534::protocol::Iso15765, 0, 0, 0xFFFFFFFF, {});
        const auto pattern = j2534::makeMessage(j2534::protocol::Iso15765, 0, 0, 0x7E8, {});
        const auto flowControl = j2534::makeMessage(j2534::protocol::Iso15765, 0, 0, 0x7E0, {});
        j2534::ULong filterId = 0;
        ASSERT_EQ(
            api_.startFilter(channelId_, j2534::filter_type::FlowControl, &mask, &pattern, &flowControl, &filterId),
            j2534::status::NoError);
    }

    long write(const j2534::PassThruMsg& message)
    {
        j2534::ULong count = 1;
        return api_.writeMessages(channelId_, &message, &count);
    }

    std::vector<j2534::PassThruMsg> read(j2534::ULong count)
    {
        std::vector<j2534::PassThruMsg> messages(count);
        api_.readMessages(channelId_, messages.data(), &count, 0);
        messages.resize(count);
        return messages;
    }

    std::string lastError()
    {
        std::array<char, 80> text {};
        api_.lastError(text.data());
        return text.data();
    }

    bool commandSent(const std::string& command) const
    {
        return std::ranges::contains(device_->commands(), command);
    }

    std::shared_ptr<testing::FakeStn> device_;
    PassThruApi api_;
    j2534::ULong deviceId_ = 0;
    j2534::ULong channelId_ = 0;
};

TEST_F(PassThruApiTest, OpenConfiguresAdapterSession)
{
    j2534::ULong deviceId = 0;

    ASSERT_EQ(api_.open(nullptr, &deviceId), j2534::status::NoError);

    for (const auto* command : { "ATZ", "ATE0", "ATS1", "ATH1", "ATCAF1", "ATCFC1", "STCMM 1", "STCTOR 1000, 1000", "STI" }) {
        EXPECT_TRUE(commandSent(command)) << command;
    }
    EXPECT_EQ(device_->baudRates(), (std::vector<unsigned> { 115200, testing::FakeStn::PowerUpBaud }));
    EXPECT_EQ(api_.open(nullptr, &deviceId), j2534::status::DeviceInUse);
}

TEST(PassThruApiOpen, RecoversAdapterLeftAtSwitchedBaudRate)
{
    auto device = std::make_shared<testing::FakeStn>();
    device->respond("STI", { "STN2232 v5.10.3" });
    device->respond("STDI", { "OBDLink EX r1.0.0" });
    device->runAt(2000000);
    PassThruApi api(
        [] {
            auto settings = testSettings();
            settings.adapterBaudRate = 2000000;
            return settings;
        },
        [device](const DeviceSettings&) -> std::unique_ptr<ByteStream> { return std::make_unique<SharedStream>(device); },
        "test");
    j2534::ULong deviceId = 0;

    ASSERT_EQ(api.open(nullptr, &deviceId), j2534::status::NoError);
    ASSERT_EQ(api.close(deviceId), j2534::status::NoError);
    ASSERT_EQ(api.open(nullptr, &deviceId), j2534::status::NoError);

    EXPECT_TRUE(std::ranges::contains(device->commands(), "STSBR 2000000"));
    EXPECT_EQ(device->baudRates().back(), 2000000u);
}

TEST_F(PassThruApiTest, OpenRejectsNonStnAdapter)
{
    device_->respond("STI", { "?" });
    j2534::ULong deviceId = 0;

    EXPECT_EQ(api_.open(nullptr, &deviceId), j2534::status::Failed);
    EXPECT_NE(lastError().find("STI"), std::string::npos);
}

TEST_F(PassThruApiTest, ConnectSelectsHighSpeedIsoTpPreset)
{
    openAndConnect();

    EXPECT_TRUE(commandSent("STP 33"));
    EXPECT_TRUE(commandSent("STPBR 500000"));
    EXPECT_TRUE(commandSent("STPO"));
}

TEST_F(PassThruApiTest, ReceivesFramesWhileNoTransmitRuns)
{
    device_->monitorLines({ "59E 9E 10 FF FF FF FF FF FF" });
    ASSERT_EQ(api_.open(nullptr, &deviceId_), j2534::status::NoError);
    ASSERT_EQ(api_.connect(deviceId_, j2534::protocol::Can, 0, 500000, &channelId_), j2534::status::NoError);
    const auto mask = j2534::makeMessage(j2534::protocol::Can, 0, 0, 0xFFFFFFFF, {});
    const auto pattern = j2534::makeMessage(j2534::protocol::Can, 0, 0, 0x59E, {});
    j2534::ULong filterId = 0;
    ASSERT_EQ(api_.startFilter(channelId_, j2534::filter_type::Pass, &mask, &pattern, nullptr, &filterId), j2534::status::NoError);

    std::array<j2534::PassThruMsg, 1> messages {};
    j2534::ULong count = 1;
    ASSERT_EQ(api_.readMessages(channelId_, messages.data(), &count, 1000), j2534::status::NoError);

    EXPECT_EQ(dataOf(messages[0]), (std::vector<std::uint8_t> { 0x00, 0x00, 0x05, 0x9E, 0x9E, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }));
    EXPECT_TRUE(commandSent("STCMM 1"));
    EXPECT_TRUE(commandSent("STM"));
}

TEST_F(PassThruApiTest, MonitorFailureIsReportedToReceivingChannel)
{
    ASSERT_EQ(api_.open(nullptr, &deviceId_), j2534::status::NoError);
    ASSERT_EQ(api_.connect(deviceId_, j2534::protocol::Can, 0, 500000, &channelId_), j2534::status::NoError);
    device_->respond("STFAC", { "?" });
    const auto mask = j2534::makeMessage(j2534::protocol::Can, 0, 0, 0xFFFFFFFF, {});
    const auto pattern = j2534::makeMessage(j2534::protocol::Can, 0, 0, 0x59E, {});
    j2534::ULong filterId = 0;
    ASSERT_EQ(api_.startFilter(channelId_, j2534::filter_type::Pass, &mask, &pattern, nullptr, &filterId), j2534::status::NoError);

    std::array<j2534::PassThruMsg, 1> messages {};
    j2534::ULong count = 1;
    api_.readMessages(channelId_, messages.data(), &count, 200);
    count = 1;

    EXPECT_EQ(api_.readMessages(channelId_, messages.data(), &count, 0), j2534::status::Failed);
    EXPECT_NE(lastError().find("STFAC failed"), std::string::npos) << lastError();
}

TEST_F(PassThruApiTest, TxDoneIsNotStampedAfterTheResponse)
{
    openAndConnect();
    device_->respondToPayload("22F188", { "7E8 03 7F 22 31" });

    ASSERT_EQ(write(isoMessage(0x7E0, { 0x22, 0xF1, 0x88 })), j2534::status::NoError);
    const auto messages = read(2);
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_NE(messages[0].RxStatus & j2534::rx_status::TxIndication, 0u);
    EXPECT_LE(messages[0].Timestamp, messages[1].Timestamp);
}

TEST_F(PassThruApiTest, BusErrorFailsTransmitWithoutResending)
{
    openAndConnect();
    device_->respondToPayload("22F188F188F188F188", { "CAN ERROR" });
    device_->respond("ATCS", { "T:7F R:00" });

    EXPECT_EQ(write(isoMessage(0x7E0, { 0x22, 0xF1, 0x88, 0xF1, 0x88, 0xF1, 0x88, 0xF1, 0x88 })), j2534::status::Failed);
    EXPECT_EQ(std::ranges::count(device_->commands(), std::string("PAYLOAD 22F188F188F188F188")), 1);
    EXPECT_EQ(lastError(), "CAN bus error (adapter: \"CAN ERROR, ATCS T:7F R:00\")");
}

TEST_F(PassThruApiTest, FlowControlOverflowFailsTransmitWithoutTxDone)
{
    openAndConnect();
    device_->respondToPayload("22F188F188F188F188", { "RMT BUF OVF" });

    EXPECT_EQ(write(isoMessage(0x7E0, { 0x22, 0xF1, 0x88, 0xF1, 0x88, 0xF1, 0x88, 0xF1, 0x88 })), j2534::status::Failed);
    EXPECT_EQ(lastError(), "ECU flow control overflow: message too long (adapter: \"RMT BUF OVF\")");
    EXPECT_TRUE(read(4).empty());
}

TEST_F(PassThruApiTest, UnrecognizedAdapterReplyFailsTransmit)
{
    openAndConnect();
    device_->respondToPayload("22F188", { "SOMETHING NEW" });

    EXPECT_EQ(write(isoMessage(0x7E0, { 0x22, 0xF1, 0x88 })), j2534::status::Failed);
    EXPECT_NE(lastError().find("SOMETHING NEW"), std::string::npos) << lastError();
}

TEST_F(PassThruApiTest, FilterChangeKeepsProtocolOpen)
{
    openAndConnect();
    ASSERT_EQ(write(isoMessage(0x7E0, { 0x22, 0xF1, 0x88 })), j2534::status::NoError);

    const auto commands = device_->commands();
    EXPECT_EQ(std::ranges::count(commands, std::string("STPC")), 1);
    EXPECT_TRUE(commandSent("STFFCA 7E8, 7FF"));
    EXPECT_TRUE(commandSent("STCFCPA 7E0, 7E8"));
}

TEST_F(PassThruApiTest, ConnectReportsRejectedBaudRate)
{
    ASSERT_EQ(api_.open(nullptr, &deviceId_), j2534::status::NoError);
    device_->respond("STPBR 1", { "?" });

    EXPECT_EQ(api_.connect(deviceId_, j2534::protocol::Iso15765, 0, 1, &channelId_), j2534::status::InvalidBaudrate);
}

TEST_F(PassThruApiTest, SingleFrameRequestQueuesTxDoneThenResponse)
{
    openAndConnect();
    device_->respondToPayload("1003", { "7E8 06 50 03 00 32 01 F4 00" });

    ASSERT_EQ(write(isoMessage(0x7E0, { 0x10, 0x03 })), j2534::status::NoError);
    const auto messages = read(2);

    EXPECT_TRUE(commandSent("STFFCA 7E8, 7FF"));
    EXPECT_TRUE(commandSent("STCFCPA 7E0, 7E8"));
    EXPECT_TRUE(commandSent("STPX h:7E0, l:2"));
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].RxStatus, j2534::rx_status::TxMsgType | j2534::rx_status::TxIndication);
    EXPECT_EQ(dataOf(messages[0]), (std::vector<std::uint8_t> { 0x00, 0x00, 0x07, 0xE0 }));
    EXPECT_EQ(messages[1].RxStatus, 0u);
    EXPECT_EQ(dataOf(messages[1]), (std::vector<std::uint8_t> { 0x00, 0x00, 0x07, 0xE8, 0x50, 0x03, 0x00, 0x32, 0x01, 0xF4 }));
}

TEST_F(PassThruApiTest, SegmentedResponseIsReassembledAfterFirstFrameIndication)
{
    openAndConnect();
    device_->respondToPayload("22F190", {
        "7E8 10 14 62 F1 90 31 46 54",
        "7E8 21 46 57 31 45 32 34 4D",
        "7E8 22 46 41 31 32 33 34 35",
    });

    ASSERT_EQ(write(isoMessage(0x7E0, { 0x22, 0xF1, 0x90 })), j2534::status::NoError);
    const auto messages = read(3);

    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[1].RxStatus, j2534::rx_status::StartOfMessage);
    EXPECT_EQ(messages[1].DataSize, 4u);
    EXPECT_EQ(messages[2].DataSize, 24u);
    EXPECT_EQ(messages[2].Data[4], 0x62);
    EXPECT_EQ(messages[2].Data[23], 0x35);
}

TEST_F(PassThruApiTest, ResponsePendingIsFollowedByFinalResponse)
{
    openAndConnect();
    device_->respondToPayload("3101FF00", { "7E8 03 7F 31 78", "7E8 05 71 01 FF 00 00" });

    ASSERT_EQ(write(isoMessage(0x7E0, { 0x31, 0x01, 0xFF, 0x00 })), j2534::status::NoError);
    const auto messages = read(3);

    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(dataOf(messages[1]), (std::vector<std::uint8_t> { 0x00, 0x00, 0x07, 0xE8, 0x7F, 0x31, 0x78 }));
    EXPECT_EQ(dataOf(messages[2]), (std::vector<std::uint8_t> { 0x00, 0x00, 0x07, 0xE8, 0x71, 0x01, 0xFF, 0x00, 0x00 }));
}

TEST_F(PassThruApiTest, CompleteResponseInterruptsAdapterReceiveWindow)
{
    openAndConnect();
    device_->respondToPayloadWithoutPrompt("1003", { "7E8 06 50 03 00 32 01 F4 00" });

    ASSERT_EQ(write(isoMessage(0x7E0, { 0x10, 0x03 })), j2534::status::NoError);
    const auto messages = read(2);

    EXPECT_TRUE(device_->receiveInterrupted());
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(dataOf(messages[1]), (std::vector<std::uint8_t> { 0x00, 0x00, 0x07, 0xE8, 0x50, 0x03, 0x00, 0x32, 0x01, 0xF4 }));
}

TEST_F(PassThruApiTest, FinalResponseAfterPendingIsDeliveredBeforeInterrupt)
{
    openAndConnect();
    device_->respondToPayloadWithoutPrompt("3101FF00", { "7E8 03 7F 31 78", "7E8 05 71 01 FF 00 00" });

    ASSERT_EQ(write(isoMessage(0x7E0, { 0x31, 0x01, 0xFF, 0x00 })), j2534::status::NoError);
    const auto messages = read(3);

    EXPECT_TRUE(device_->receiveInterrupted());
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(dataOf(messages[1]), (std::vector<std::uint8_t> { 0x00, 0x00, 0x07, 0xE8, 0x7F, 0x31, 0x78 }));
    EXPECT_EQ(dataOf(messages[2]), (std::vector<std::uint8_t> { 0x00, 0x00, 0x07, 0xE8, 0x71, 0x01, 0xFF, 0x00, 0x00 }));
}

TEST_F(PassThruApiTest, MissingFlowControlFromEcuFailsWithoutTxDone)
{
    openAndConnect();
    device_->respondToPayload("36010102030405060708", { "FC RX TIMEOUT" });

    EXPECT_EQ(write(isoMessage(0x7E0, { 0x36, 0x01, 1, 2, 3, 4, 5, 6, 7, 8 })), j2534::status::Failed);
    EXPECT_EQ(lastError(), "ECU sent no flow control frame (adapter: \"FC RX TIMEOUT\")");
    EXPECT_TRUE(read(1).empty());
}

TEST_F(PassThruApiTest, SegmentedRequestWithoutFlowControlFilterIsRejected)
{
    openAndConnect();

    EXPECT_EQ(write(isoMessage(0x7E1, { 0x36, 0x01, 1, 2, 3, 4, 5, 6, 7, 8 })), j2534::status::NoFlowControl);
}

TEST_F(PassThruApiTest, LoopbackEchoesTransmittedMessage)
{
    openAndConnect();
    j2534::SConfig loopback { j2534::config::Loopback, 1 };
    j2534::SConfigList list { 1, &loopback };
    ASSERT_EQ(api_.ioctl(channelId_, j2534::ioctl::SetConfig, &list, nullptr), j2534::status::NoError);

    ASSERT_EQ(write(isoMessage(0x7E0, { 0x3E, 0x00 })), j2534::status::NoError);
    const auto messages = read(2);

    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[1].RxStatus, j2534::rx_status::TxMsgType);
    EXPECT_EQ(dataOf(messages[1]), (std::vector<std::uint8_t> { 0x00, 0x00, 0x07, 0xE0, 0x3E, 0x00 }));
}

TEST_F(PassThruApiTest, ReportsFixedParametersAndRejectsOverrides)
{
    openAndConnect();
    j2534::SConfig blockSize { j2534::config::BsTx, 0 };
    j2534::SConfigList readList { 1, &blockSize };
    j2534::SConfig separation { j2534::config::StminTx, 5 };
    j2534::SConfigList writeList { 1, &separation };

    ASSERT_EQ(api_.ioctl(channelId_, j2534::ioctl::GetConfig, &readList, nullptr), j2534::status::NoError);
    EXPECT_EQ(blockSize.Value, j2534::UseEcuFlowControl);
    EXPECT_EQ(api_.ioctl(channelId_, j2534::ioctl::SetConfig, &writeList, nullptr), j2534::status::NotSupported);
}

TEST_F(PassThruApiTest, ReadsBatteryVoltageInMillivolts)
{
    ASSERT_EQ(api_.open(nullptr, &deviceId_), j2534::status::NoError);
    j2534::ULong millivolts = 0;

    ASSERT_EQ(api_.ioctl(deviceId_, j2534::ioctl::ReadVbatt, nullptr, &millivolts), j2534::status::NoError);
    EXPECT_EQ(millivolts, 12345u);
}

TEST_F(PassThruApiTest, PeriodicMessageIsTransmittedUntilStopped)
{
    openAndConnect();
    const auto testerPresent = isoMessage(0x7E0, { 0x3E, 0x80 });
    j2534::ULong periodicId = 0;

    ASSERT_EQ(api_.startPeriodic(channelId_, &testerPresent, &periodicId, 20), j2534::status::NoError);

    EXPECT_TRUE(device_->waitForCommand("PAYLOAD 3E80", 2s));
    EXPECT_EQ(api_.stopPeriodic(channelId_, periodicId), j2534::status::NoError);
}

TEST_F(PassThruApiTest, TimedOutTransmitReturnsTimeoutAndKeepsDeviceUsable)
{
    openAndConnect();
    device_->silence("STPX h:7E0, l:2");
    device_->respondToPayload("22F188", { "7E8 03 7F 22 31" });

    EXPECT_EQ(write(isoMessage(0x7E0, { 0x10, 0x02 })), j2534::status::Timeout);
    EXPECT_EQ(write(isoMessage(0x7E0, { 0x22, 0xF1, 0x88 })), j2534::status::NoError);
}

TEST_F(PassThruApiTest, PinSwitchedChannelUsesMediumSpeedPinsOnceSelected)
{
    ASSERT_EQ(api_.open(nullptr, &deviceId_), j2534::status::NoError);
    ASSERT_EQ(api_.connect(deviceId_, j2534::protocol::Iso15765Ps, 0, 500000, &channelId_), j2534::status::NoError);
    auto message = isoMessage(0x726, { 0x3E, 0x00 });
    message.ProtocolID = j2534::protocol::Iso15765Ps;
    j2534::ULong count = 1;

    EXPECT_EQ(api_.writeMessages(channelId_, &message, &count), j2534::status::PinInvalid);

    j2534::SConfig pins { j2534::config::J1962Pins, 0x030B };
    j2534::SConfigList list { 1, &pins };
    ASSERT_EQ(api_.ioctl(channelId_, j2534::ioctl::SetConfig, &list, nullptr), j2534::status::NoError);
    count = 1;
    ASSERT_EQ(api_.writeMessages(channelId_, &message, &count), j2534::status::NoError);

    EXPECT_TRUE(commandSent("STP 53"));
    EXPECT_FALSE(commandSent("STP 33"));
}

TEST(PassThruApiTrace, RecordsApiCallsWithArgumentsAndResults)
{
    const auto logPath = std::filesystem::temp_directory_path() / "obdlink_api_trace_test.log";
    std::filesystem::remove(logPath);
    auto device = std::make_shared<testing::FakeStn>();
    device->respond("STI", { "STN2232 v5.10.3" });
    device->respond("STDI", { "OBDLink EX r1.0.0" });
    PassThruApi api(
        [&logPath] {
            auto settings = testSettings();
            settings.logPath = logPath.string();
            return settings;
        },
        [device](const DeviceSettings&) -> std::unique_ptr<ByteStream> { return std::make_unique<SharedStream>(device); },
        "test");
    j2534::ULong deviceId = 0;
    j2534::ULong channelId = 0;
    ASSERT_EQ(api.open(nullptr, &deviceId), j2534::status::NoError);
    ASSERT_EQ(api.connect(deviceId, j2534::protocol::Iso15765Ps, 0, 500000, &channelId), j2534::status::NoError);
    j2534::SConfig pins { j2534::config::J1962Pins, 0x0102 };
    j2534::SConfigList list { 1, &pins };
    api.ioctl(channelId, j2534::ioctl::SetConfig, &list, nullptr);
    api.close(deviceId);

    std::ifstream log(logPath);
    const std::string text { std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>() };
    EXPECT_NE(text.find("API PassThruConnect protocol=0x8005 flags=0x0 baud=500000 -> OK"), std::string::npos) << text;
    EXPECT_NE(text.find("API PassThruIoctl id=2 ioctl=0x2 0x8001=0x102 -> 0x13"), std::string::npos) << text;
    std::filesystem::remove(logPath);
}

TEST_F(PassThruApiTest, ProgrammingVoltageCanOnlyBeSwitchedOff)
{
    ASSERT_EQ(api_.open(nullptr, &deviceId_), j2534::status::NoError);

    EXPECT_EQ(api_.setProgrammingVoltage(deviceId_, 13, j2534::VoltageOff), j2534::status::NoError);
    EXPECT_EQ(api_.setProgrammingVoltage(deviceId_, 13, 18000), j2534::status::Failed);
}

}
}
