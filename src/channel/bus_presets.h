#pragma once

#include <optional>

#include "j2534/j2534_defs.h"
#include "stn/stn_adapter.h"

namespace obdlink {

inline constexpr j2534::ULong HighSpeedCanPins = 0x060E;
inline constexpr j2534::ULong MediumSpeedCanPins = 0x030B;

// STN protocol preset for a J1962 pin pair, frame format and identifier width (FRPM STP table).
std::optional<int> stnPresetFor(j2534::ULong pins, FrameFormat format, bool extended);

}
