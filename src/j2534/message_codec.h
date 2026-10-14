#pragma once

#include <cstdint>
#include <span>

#include "j2534/j2534_defs.h"

namespace j2534 {

std::uint32_t canIdOf(const PassThruMsg& message);

std::span<const std::uint8_t> payloadOf(const PassThruMsg& message);

PassThruMsg makeMessage(ULong protocolId, ULong rxStatus, ULong timestamp, std::uint32_t canId, std::span<const std::uint8_t> payload);

// Indications carry only the CAN ID and, per J2534-1 Appendix A, an ExtraDataIndex of zero.
PassThruMsg makeIndication(ULong protocolId, ULong rxStatus, ULong timestamp, std::uint32_t canId);

}
