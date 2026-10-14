#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <variant>
#include <vector>

#include "stn/can_frame.h"

namespace obdlink {

inline constexpr std::size_t MaxSingleFramePayload = 7;

struct IsoTpFirstFrame {
    std::uint32_t id;
    bool extended;
};

struct IsoTpMessage {
    std::uint32_t id;
    bool extended;
    std::vector<std::uint8_t> payload;
};

using IsoTpEvent = std::variant<IsoTpFirstFrame, IsoTpMessage>;

// Reassembles ISO 15765-2 normal-addressing segments per source CAN ID. Flow control
// frames are produced by the adapter, so received flow control frames carry nothing here.
class IsoTpReceiver {
public:
    std::vector<IsoTpEvent> accept(const CanFrame& frame);

    void reset();

private:
    struct Assembly {
        std::size_t expectedLength = 0;
        std::uint8_t nextSequence = 1;
        std::vector<std::uint8_t> payload;
    };

    using Key = std::pair<std::uint32_t, bool>;

    std::vector<IsoTpEvent> acceptSingle(const CanFrame& frame);
    std::vector<IsoTpEvent> acceptFirst(const CanFrame& frame);
    std::vector<IsoTpEvent> acceptConsecutive(const CanFrame& frame);

    std::map<Key, Assembly> assemblies_;
};

}
