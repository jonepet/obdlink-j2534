#include "channel/bus_presets.h"

#include <algorithm>
#include <array>

namespace obdlink {

namespace {

struct PresetEntry {
    j2534::ULong pins;
    FrameFormat format;
    bool extended;
    int preset;
};

constexpr std::array<PresetEntry, 8> Presets { {
    { HighSpeedCanPins, FrameFormat::Raw, false, 31 },
    { HighSpeedCanPins, FrameFormat::Raw, true, 32 },
    { HighSpeedCanPins, FrameFormat::IsoTp, false, 33 },
    { HighSpeedCanPins, FrameFormat::IsoTp, true, 34 },
    { MediumSpeedCanPins, FrameFormat::Raw, false, 51 },
    { MediumSpeedCanPins, FrameFormat::Raw, true, 52 },
    { MediumSpeedCanPins, FrameFormat::IsoTp, false, 53 },
    { MediumSpeedCanPins, FrameFormat::IsoTp, true, 54 },
} };

}

std::optional<int> stnPresetFor(j2534::ULong pins, FrameFormat format, bool extended)
{
    const auto entry = std::ranges::find_if(Presets, [&](const PresetEntry& candidate) {
        return candidate.pins == pins && candidate.format == format && candidate.extended == extended;
    });
    if (entry == Presets.end()) {
        return std::nullopt;
    }
    return entry->preset;
}

}
