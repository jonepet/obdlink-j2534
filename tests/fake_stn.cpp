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

void FakeStn::silence(const std::string& command)
{
    std::lock_guard lock(mutex_);
    silent_.insert(command);
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

void FakeStn::write(std::string_view bytes)
{
    {
        std::lock_guard lock(mutex_);
        for (const char c : bytes) {
            if (c != '\r') {
                input_.push_back(c);
                continue;
            }
            auto line = std::exchange(input_, {});
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
    const auto count = std::min(capacity, output_.size());
    std::copy_n(output_.begin(), count, buffer);
    output_.erase(0, count);
    return count;
}

void FakeStn::setBaudRate(unsigned baud)
{
    std::lock_guard lock(mutex_);
    baudRates_.push_back(baud);
}

void FakeStn::discardInput()
{
    std::lock_guard lock(mutex_);
    output_.clear();
}

void FakeStn::handleCommand(const std::string& line)
{
    commands_.push_back(line);
    if (silent_.contains(line)) {
        return;
    }
    if (line.starts_with("STPX") && line.find("l:") != std::string::npos) {
        output_ += "DATA>";
        awaitingPayload_ = true;
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
    emit(found == payloadResponses_.end() ? std::vector<std::string> { "NO DATA" } : found->second);
}

void FakeStn::emit(const std::vector<std::string>& lines)
{
    for (const auto& line : lines) {
        output_ += line + "\r";
    }
    output_ += "\r>";
}

}
