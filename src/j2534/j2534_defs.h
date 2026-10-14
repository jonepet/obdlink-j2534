#pragma once

#include <cstdint>

namespace j2534 {

using ULong = std::uint32_t;

namespace protocol {
inline constexpr ULong Can = 5;
inline constexpr ULong Iso15765 = 6;
inline constexpr ULong CanPs = 0x8004;
inline constexpr ULong Iso15765Ps = 0x8005;
}

namespace status {
inline constexpr long NoError = 0x00;
inline constexpr long NotSupported = 0x01;
inline constexpr long InvalidChannelId = 0x02;
inline constexpr long InvalidProtocolId = 0x03;
inline constexpr long NullParameter = 0x04;
inline constexpr long InvalidIoctlValue = 0x05;
inline constexpr long InvalidFlags = 0x06;
inline constexpr long Failed = 0x07;
inline constexpr long DeviceNotConnected = 0x08;
inline constexpr long Timeout = 0x09;
inline constexpr long InvalidMsg = 0x0A;
inline constexpr long InvalidTimeInterval = 0x0B;
inline constexpr long ExceededLimit = 0x0C;
inline constexpr long InvalidMsgId = 0x0D;
inline constexpr long DeviceInUse = 0x0E;
inline constexpr long InvalidIoctlId = 0x0F;
inline constexpr long BufferEmpty = 0x10;
inline constexpr long BufferFull = 0x11;
inline constexpr long BufferOverflow = 0x12;
inline constexpr long PinInvalid = 0x13;
inline constexpr long ChannelInUse = 0x14;
inline constexpr long MsgProtocolId = 0x15;
inline constexpr long InvalidFilterId = 0x16;
inline constexpr long NoFlowControl = 0x17;
inline constexpr long NotUnique = 0x18;
inline constexpr long InvalidBaudrate = 0x19;
inline constexpr long InvalidDeviceId = 0x1A;
}

namespace connect_flag {
inline constexpr ULong Can29BitId = 0x100;
inline constexpr ULong CanIdBoth = 0x800;
}

namespace tx_flag {
inline constexpr ULong Iso15765FramePad = 0x40;
inline constexpr ULong Iso15765AddrType = 0x80;
inline constexpr ULong Can29BitId = 0x100;
}

namespace rx_status {
inline constexpr ULong TxMsgType = 0x01;
inline constexpr ULong StartOfMessage = 0x02;
inline constexpr ULong TxIndication = 0x08;
inline constexpr ULong Iso15765AddrType = 0x80;
inline constexpr ULong Can29BitId = 0x100;
}

namespace filter_type {
inline constexpr ULong Pass = 1;
inline constexpr ULong Block = 2;
inline constexpr ULong FlowControl = 3;
}

namespace ioctl {
inline constexpr ULong GetConfig = 0x01;
inline constexpr ULong SetConfig = 0x02;
inline constexpr ULong ReadVbatt = 0x03;
inline constexpr ULong FiveBaudInit = 0x04;
inline constexpr ULong FastInit = 0x05;
inline constexpr ULong ClearTxBuffer = 0x07;
inline constexpr ULong ClearRxBuffer = 0x08;
inline constexpr ULong ClearPeriodicMsgs = 0x09;
inline constexpr ULong ClearMsgFilters = 0x0A;
inline constexpr ULong ClearFunctMsgLookupTable = 0x0B;
inline constexpr ULong AddToFunctMsgLookupTable = 0x0C;
inline constexpr ULong DeleteFromFunctMsgLookupTable = 0x0D;
inline constexpr ULong ReadProgVoltage = 0x0E;
}

namespace config {
inline constexpr ULong DataRate = 0x01;
inline constexpr ULong Loopback = 0x03;
inline constexpr ULong BitSamplePoint = 0x17;
inline constexpr ULong SyncJumpWidth = 0x18;
inline constexpr ULong Iso15765Bs = 0x1E;
inline constexpr ULong Iso15765Stmin = 0x1F;
inline constexpr ULong BsTx = 0x22;
inline constexpr ULong StminTx = 0x23;
inline constexpr ULong Iso15765WftMax = 0x25;
inline constexpr ULong CanMixedFormat = 0x8000;
inline constexpr ULong J1962Pins = 0x8001;
}

inline constexpr ULong ShortToGround = 0xFFFFFFFE;
inline constexpr ULong VoltageOff = 0xFFFFFFFF;
inline constexpr ULong UseEcuFlowControl = 0xFFFF;
inline constexpr std::size_t MaxDataSize = 4128;
inline constexpr std::size_t MessageIdSize = 4;
inline constexpr std::size_t MaxIsoTpPayload = 4095;
inline constexpr char ApiVersion[] = "04.04";

#pragma pack(push, 1)
struct PassThruMsg {
    ULong ProtocolID;
    ULong RxStatus;
    ULong TxFlags;
    ULong Timestamp;
    ULong DataSize;
    ULong ExtraDataIndex;
    unsigned char Data[MaxDataSize];
};

struct SConfig {
    ULong Parameter;
    ULong Value;
};

struct SConfigList {
    ULong NumOfParams;
    SConfig* ConfigPtr;
};
#pragma pack(pop)

}
