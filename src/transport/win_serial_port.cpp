#include "transport/win_serial_port.h"

#include <algorithm>
#include <array>
#include <format>

#include "stn/stn_error.h"

namespace obdlink {

namespace {

std::string systemMessage(DWORD code)
{
    std::array<char, 512> text {};
    const auto length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        text.data(),
        static_cast<DWORD>(text.size()),
        nullptr);
    std::string message = length > 0 ? std::string(text.data(), length) : std::format("error {}", code);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

[[noreturn]] void throwLastError(const std::string& action, const std::string& portName)
{
    throw StnError(std::format("{} {}: {}", action, portName, systemMessage(GetLastError())));
}

}

// With ReadIntervalTimeout and ReadTotalTimeoutMultiplier both MAXDWORD, ReadFile returns as soon
// as any byte is buffered and otherwise waits up to ReadTotalTimeoutConstant for the first one.
WinSerialPort::WinSerialPort(const std::string& portName, unsigned baud, std::chrono::milliseconds writeTimeout)
    : portName_(portName)
{
    const auto handle = CreateFileA(("\\\\.\\" + portName).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throwLastError("cannot open", portName_);
    }
    handle_.reset(handle);
    timeouts_.ReadIntervalTimeout = MAXDWORD;
    timeouts_.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeouts_.ReadTotalTimeoutConstant = 1;
    timeouts_.WriteTotalTimeoutConstant = static_cast<DWORD>(std::clamp<long long>(writeTimeout.count(), 1, MAXDWORD - 1));
    applyTimeouts(timeouts_);
    configure(baud);
}

void WinSerialPort::configure(unsigned baud)
{
    DCB state {};
    state.DCBlength = sizeof(state);
    if (!GetCommState(handle_.get(), &state)) {
        throwLastError("cannot read settings of", portName_);
    }
    state.BaudRate = baud;
    state.ByteSize = 8;
    state.Parity = NOPARITY;
    state.StopBits = ONESTOPBIT;
    state.fBinary = TRUE;
    state.fParity = FALSE;
    state.fOutxCtsFlow = FALSE;
    state.fOutxDsrFlow = FALSE;
    state.fDsrSensitivity = FALSE;
    state.fOutX = FALSE;
    state.fInX = FALSE;
    state.fDtrControl = DTR_CONTROL_ENABLE;
    state.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(handle_.get(), &state)) {
        throwLastError("cannot configure", portName_);
    }
    if (!PurgeComm(handle_.get(), PURGE_RXCLEAR | PURGE_TXCLEAR)) {
        throwLastError("cannot purge", portName_);
    }
}

void WinSerialPort::write(std::string_view bytes)
{
    while (!bytes.empty()) {
        DWORD written = 0;
        if (!WriteFile(handle_.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
            throwLastError("cannot write to", portName_);
        }
        if (written == 0) {
            throw StnTimeout(std::format("write to {} timed out", portName_));
        }
        bytes.remove_prefix(written);
    }
}

std::size_t WinSerialPort::read(char* buffer, std::size_t capacity, std::chrono::milliseconds timeout)
{
    applyReadTimeout(timeout);
    DWORD received = 0;
    if (!ReadFile(handle_.get(), buffer, static_cast<DWORD>(capacity), &received, nullptr)) {
        throwLastError("cannot read from", portName_);
    }
    return received;
}

void WinSerialPort::setBaudRate(unsigned baud)
{
    DCB state {};
    state.DCBlength = sizeof(state);
    if (!GetCommState(handle_.get(), &state)) {
        throwLastError("cannot read settings of", portName_);
    }
    state.BaudRate = baud;
    if (!SetCommState(handle_.get(), &state)) {
        throwLastError("cannot set baud rate of", portName_);
    }
}

void WinSerialPort::discardInput()
{
    if (!PurgeComm(handle_.get(), PURGE_RXCLEAR)) {
        throwLastError("cannot purge", portName_);
    }
}

void WinSerialPort::applyReadTimeout(std::chrono::milliseconds timeout)
{
    const auto requested = static_cast<DWORD>(std::clamp<long long>(timeout.count(), 1, MAXDWORD - 1));
    if (requested == timeouts_.ReadTotalTimeoutConstant) {
        return;
    }
    auto timeouts = timeouts_;
    timeouts.ReadTotalTimeoutConstant = requested;
    applyTimeouts(timeouts);
}

void WinSerialPort::applyTimeouts(const COMMTIMEOUTS& timeouts)
{
    auto requested = timeouts;
    if (!SetCommTimeouts(handle_.get(), &requested)) {
        throwLastError("cannot set timeouts of", portName_);
    }
    timeouts_ = timeouts;
}

}
