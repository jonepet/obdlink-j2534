#pragma once

#include <memory>
#include <string>

#include <windows.h>

#include "transport/byte_stream.h"

namespace obdlink {

class WinSerialPort final : public ByteStream {
public:
    WinSerialPort(const std::string& portName, unsigned baud, std::chrono::milliseconds writeTimeout);

    void write(std::string_view bytes) override;
    std::size_t read(char* buffer, std::size_t capacity, std::chrono::milliseconds timeout) override;
    void setBaudRate(unsigned baud) override;
    void discardInput() override;

private:
    struct HandleCloser {
        void operator()(HANDLE handle) const
        {
            CloseHandle(handle);
        }
    };

    void configure(unsigned baud);
    void applyReadTimeout(std::chrono::milliseconds timeout);
    void applyTimeouts(const COMMTIMEOUTS& timeouts);

    std::string portName_;
    std::unique_ptr<void, HandleCloser> handle_;
    COMMTIMEOUTS timeouts_ {};
};

}
