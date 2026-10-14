#pragma once

#include <chrono>
#include <cstddef>
#include <string_view>

namespace obdlink {

class ByteStream {
public:
    virtual ~ByteStream() = default;

    virtual void write(std::string_view bytes) = 0;

    // Returns the number of bytes read; 0 means the timeout elapsed with nothing received.
    virtual std::size_t read(char* buffer, std::size_t capacity, std::chrono::milliseconds timeout) = 0;

    virtual void setBaudRate(unsigned baud) = 0;

    virtual void discardInput() = 0;
};

}
