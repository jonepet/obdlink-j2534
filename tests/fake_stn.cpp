#include "fake_stn.h"

#include <algorithm>
#include <utility>

namespace obdlink::testing {

void FakeStn::respond(const std::string& command, std::vector<std::string> lines)
{
    std::lock_guard lock(mutex_);
    responses_[command] = std::move(lines);
}

void FakeStn::respondToPayload(const std::string& payloadHex, std::vector<std::string> lines)
{
    std::lock_guard lock(mutex_);
    payloadResponses_[payloadHex] = std::move(lines);
}

void FakeStn::respondToPayloadWithoutPrompt(const std::string& payloadHex, std::vector<std::string> lines)
{
    std::lock_guard lock(mutex_);
    payloadResponses_[payloadHex] = std::move(lines);
    promptWithheld_.insert(payloadHex);
}

void FakeStn::respondToPayloadWithLatePrompt(const std::string& payloadHex, std::vector<std::string> lines)
{
    std::lock_guard lock(mutex_);
    payloadResponses_[payloadHex] = std::move(lines);
    promptLate_.insert(payloadHex);
}

bool FakeStn::receiveInterrupted() const
{
    std::lock_guard lock(mutex_);
    return receiveInterrupted_;
}

void FakeStn::monitorLines(std::vector<std::string> lines)
{
    std::lock_guard lock(mutex_);
    monitorLines_ = std::move(lines);
}

void FakeStn::silence(const std::string& command)
{
    std::lock_guard lock(mutex_);
    silent_.insert(command);
}

void FakeStn::runAt(unsigned baud)
{
    std::lock_guard lock(mutex_);
    deviceBaud_ = baud;
}

std::vector<std::string> FakeStn::commands() const
{
    std::lock_guard lock(mutex_);
    return commands_;
}

std::vector<unsigned> FakeStn::baudRates() const
{
    std::lock_guard lock(mutex_);
    return baudRates_;
}

bool FakeStn::waitForCommand(const std::string& command, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return std::ranges::contains(commands_, command); });
}

bool FakeStn::waitForCommandCount(const std::string& command, std::size_t count, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] {
        return static_cast<std::size_t>(std::ranges::count(commands_, command)) >= count;
    });
}

void FakeStn::write(std::string_view bytes)
{
    {
        std::lock_guard lock(mutex_);
        if (hostBaud_ != deviceBaud_) {
            return;
        }
        if (promptPending_ && !bytes.empty()) {
            promptPending_ = false;
            emitRaw("\r>");
        }
        if ((monitoring_ || receiving_) && !bytes.empty()) {
            receiveInterrupted_ = receiveInterrupted_ || receiving_;
            monitoring_ = false;
            receiving_ = false;
            emit({ "STOPPED" });
            commandTruncated_ = bytes.front() != 'X';
            bytes.remove_prefix(1);
        }
        for (const char c : bytes) {
            if (c != '\r') {
                input_.push_back(c);
                continue;
            }
            auto line = std::exchange(input_, {});
            if (std::exchange(commandTruncated_, false)) {
                commands_.push_back(line);
                emit({ "?" });
                continue;
            }
            if (std::exchange(awaitingPayload_, false)) {
                handlePayload(line);
                continue;
            }
            handleCommand(line);
        }
    }
    changed_.notify_all();
}

std::size_t FakeStn::read(char* buffer, std::size_t capacity, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, timeout, [this] { return !output_.empty(); })) {
        return 0;
    }
    const auto garbled = std::ranges::find(outputRates_, hostBaud_) - outputRates_.begin();
    output_.erase(0, static_cast<std::size_t>(garbled));
    outputRates_.erase(outputRates_.begin(), outputRates_.begin() + garbled);
    const auto readable = std::ranges::find_if(outputRates_, [this](unsigned rate) { return rate != hostBaud_; }) - outputRates_.begin();
    const auto count = std::min(capacity, static_cast<std::size_t>(readable));
    std::copy_n(output_.begin(), count, buffer);
    output_.erase(0, count);
    outputRates_.erase(outputRates_.begin(), outputRates_.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
}

void FakeStn::setBaudRate(unsigned baud)
{
    std::lock_guard lock(mutex_);
    baudRates_.push_back(baud);
    hostBaud_ = baud;
}

void FakeStn::discardInput()
{
    std::lock_guard lock(mutex_);
    output_.clear();
    outputRates_.clear();
}

void FakeStn::handleCommand(const std::string& line)
{
    commands_.push_back(line);
    if (silent_.contains(line)) {
        return;
    }
    if (line.starts_with("STPX") && line.find("l:") != std::string::npos) {
        emitRaw("DATA>");
        awaitingPayload_ = true;
        return;
    }
    if (line == "ATZ") {
        deviceBaud_ = PowerUpBaud;
        emit({ "ELM327 v1.4b" });
        return;
    }
    if (line.starts_with("STSBR ")) {
        switchRate(line);
        return;
    }
    if (line == "STM") {
        monitoring_ = true;
        for (const auto& monitored : monitorLines_) {
            emitRaw(monitored + "\r");
        }
        return;
    }
    if (const auto found = responses_.find(line); found != responses_.end()) {
        emit(found->second);
        return;
    }
    emit({ line == "X" ? "?" : "OK" });
}

void FakeStn::handlePayload(const std::string& hex)
{
    commands_.push_back("PAYLOAD " + hex);
    const auto found = payloadResponses_.find(hex);
    if (found == payloadResponses_.end()) {
        emit({ "NO DATA" });
        return;
    }
    if (promptLate_.contains(hex)) {
        for (const auto& line : found->second) {
            emitRaw(line + "\r");
        }
        promptPending_ = true;
        return;
    }
    if (!promptWithheld_.contains(hex)) {
        emit(found->second);
        return;
    }
    for (const auto& line : found->second) {
        emitRaw(line + "\r");
    }
    receiving_ = true;
}

void FakeStn::emit(const std::vector<std::string>& lines)
{
    for (const auto& line : lines) {
        emitRaw(line + "\r");
    }
    emitRaw("\r>");
}

void FakeStn::emitRaw(const std::string& bytes)
{
    output_ += bytes;
    outputRates_.insert(outputRates_.end(), bytes.size(), deviceBaud_);
}

void FakeStn::switchRate(const std::string& command)
{
    emitRaw("OK\r");
    deviceBaud_ = static_cast<unsigned>(std::stoul(command.substr(command.find(' ') + 1)));
    emitRaw("\r>");
}

}
