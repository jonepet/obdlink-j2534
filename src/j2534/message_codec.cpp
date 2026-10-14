#include "j2534/message_codec.h"

#include <algorithm>

namespace j2534 {

std::uint32_t canIdOf(const PassThruMsg& message)
{
    return (static_cast<std::uint32_t>(message.Data[0]) << 24)
        | (static_cast<std::uint32_t>(message.Data[1]) << 16)
        | (static_cast<std::uint32_t>(message.Data[2]) << 8)
        | message.Data[3];
}

std::span<const std::uint8_t> payloadOf(const PassThruMsg& message)
{
    return { message.Data + MessageIdSize, message.DataSize - MessageIdSize };
}

PassThruMsg makeMessage(ULong protocolId, ULong rxStatus, ULong timestamp, std::uint32_t canId, std::span<const std::uint8_t> payload)
{
    PassThruMsg message {};
    message.ProtocolID = protocolId;
    message.RxStatus = rxStatus;
    message.Timestamp = timestamp;
    message.Data[0] = static_cast<std::uint8_t>(canId >> 24);
    message.Data[1] = static_cast<std::uint8_t>(canId >> 16);
    message.Data[2] = static_cast<std::uint8_t>(canId >> 8);
    message.Data[3] = static_cast<std::uint8_t>(canId);
    std::ranges::copy(payload, message.Data + MessageIdSize);
    message.DataSize = static_cast<ULong>(MessageIdSize + payload.size());
    message.ExtraDataIndex = message.DataSize;
    return message;
}

PassThruMsg makeIndication(ULong protocolId, ULong rxStatus, ULong timestamp, std::uint32_t canId)
{
    auto message = makeMessage(protocolId, rxStatus, timestamp, canId, {});
    message.ExtraDataIndex = 0;
    return message;
}

}
