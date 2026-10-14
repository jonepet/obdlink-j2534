#pragma once

#include <cstdint>
#include <vector>

namespace obdlink {

struct CanFrame {
    std::uint32_t id = 0;
    bool extended = false;
    std::vector<std::uint8_t> data;
};

}
