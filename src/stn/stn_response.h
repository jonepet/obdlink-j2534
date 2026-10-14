#pragma once

#include <string_view>
#include <variant>

#include "stn/can_frame.h"

namespace obdlink {

enum class StnStatus {
    Ok,
    NoData,
    Rejected,
    BusFailure,
    FlowControlTimeout,
    Overflow,
    OutOfMemory,
    CorruptFrame,
    Stopped,
    DeviceReset,
    Unrecognized,
};

using StnResponse = std::variant<CanFrame, StnStatus>;

// Parses one response line printed with ATH1, ATS1 and ATD0: a frame line is the CAN ID
// followed by the data bytes, PCI byte included (11-bit: "7E8 10 14 49 02 01 31 47 31").
// A first token that is not three digits is read as a 29-bit ID of four bytes.
StnResponse parseResponseLine(std::string_view line);

std::string_view describe(StnStatus status);

}
