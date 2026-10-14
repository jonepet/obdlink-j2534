#pragma once

#include <chrono>

#include "j2534/j2534_defs.h"

namespace j2534 {

// J2534 timestamps are microseconds in a 32-bit counter that wraps.
class TimestampClock {
public:
    TimestampClock()
        : epoch_(std::chrono::steady_clock::now())
    {
    }

    ULong at(std::chrono::steady_clock::time_point point) const
    {
        return static_cast<ULong>(std::chrono::duration_cast<std::chrono::microseconds>(point - epoch_).count());
    }

    ULong now() const
    {
        return at(std::chrono::steady_clock::now());
    }

private:
    std::chrono::steady_clock::time_point epoch_;
};

}
