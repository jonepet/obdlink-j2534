#include "stn/stn_link.h"

#include <array>

#include "stn/stn_error.h"

namespace obdlink {

namespace {

constexpr char CommandTerminator = '\r';
constexpr char Prompt = '>';

std::string trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(' ');
    return std::string(text.substr(first, last - first + 1));
}

void discardLine(const StnLine&)
{
}

}

StnLink::StnLink(ByteStream& stream)
    : stream_(stream)
    , trace_([](TraceDirection, std::string_view) {})
{
}

std::vector<StnLine> StnLink::execute(std::string_view command, std::chrono::milliseconds timeout)
{
    std::vector<StnLine> lines;
    executeStreaming(command, timeout, [&lines](const StnLine& line) { lines.push_back(line); });
    return lines;
}

void StnLink::executeStreaming(std::string_view command, std::chrono::milliseconds timeout, const LineSink& sink)
{
    send(command);
    expectTerminator(EventKind::Prompt, timeout, sink);
}

void StnLink::executeWithPayload(
    std::string_view command,
    std::string_view hexPayload,
    std::chrono::milliseconds timeout,
    const LineSink& sink,
    const std::function<bool()>& isComplete)
{
    send(command);
    expectTerminator(EventKind::DataPrompt, timeout, sink);
    send(hexPayload);
    expectTerminator(EventKind::Prompt, timeout, sink, isComplete);
}

void StnLink::switchBaudRate(unsigned baud, std::chrono::milliseconds timeout)
{
    send("STSBR " + std::to_string(baud));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto event = nextEvent(deadline);
        if (event.kind == EventKind::Timeout) {
            throw StnTimeout("no acknowledgement for " + lastCommand_);
        }
        if (event.kind != EventKind::Line) {
            throw StnError("device rejected baud rate " + std::to_string(baud));
        }
        if (event.line.text == "OK") {
            break;
        }
    }
    stream_.setBaudRate(baud);
    resetBuffers();
    expectTerminator(EventKind::Prompt, timeout, discardLine);
}

void StnLink::reset(unsigned powerUpBaud, std::chrono::milliseconds timeout)
{
    send("ATZ");
    stream_.setBaudRate(powerUpBaud);
    resetBuffers();
    expectTerminator(EventKind::Prompt, timeout, discardLine);
}

void StnLink::monitor(
    std::string_view command,
    std::chrono::milliseconds poll,
    std::chrono::milliseconds stopTimeout,
    const LineSink& sink,
    const std::function<bool()>& keepRunning)
{
    send(command);
    while (keepRunning()) {
        auto event = nextEvent(std::chrono::steady_clock::now() + poll);
        if (event.kind == EventKind::Line) {
            sink(event.line);
            continue;
        }
        if (event.kind != EventKind::Timeout) {
            return;
        }
    }
    // FRPM 8.9: any single character stops monitoring and is discarded; STOPPED and the prompt follow.
    writeTraced("X");
    expectTerminator(EventKind::Prompt, stopTimeout, sink);
}

void StnLink::resynchronize(std::chrono::milliseconds busyProbe, std::chrono::milliseconds timeout)
{
    stream_.discardInput();
    resetBuffers();
    lastCommand_.clear();

    writeTraced("X");
    if (drainUntilTerminator(busyProbe, discardLine) == EventKind::Prompt) {
        return;
    }
    writeTraced(std::string(1, CommandTerminator));
    expectTerminator(EventKind::Prompt, timeout, discardLine);
}

void StnLink::setTrace(TraceSink trace)
{
    trace_ = std::move(trace);
}

void StnLink::send(std::string_view text)
{
    lastCommand_ = std::string(text);
    writeTraced(lastCommand_ + CommandTerminator);
}

void StnLink::writeTraced(std::string_view bytes)
{
    trace_(TraceDirection::Sent, bytes);
    stream_.write(bytes);
}

void StnLink::expectTerminator(
    EventKind expected,
    std::chrono::milliseconds silenceTimeout,
    const LineSink& sink,
    const std::function<bool()>& isComplete)
{
    const auto terminator = drainUntilTerminator(silenceTimeout, sink, isComplete);
    if (terminator == EventKind::Completed) {
        interruptReceive(silenceTimeout);
        return;
    }
    if (terminator == EventKind::Timeout) {
        throw StnTimeout("no prompt after " + lastCommand_);
    }
    if (terminator != expected) {
        throw StnError("unexpected prompt after " + lastCommand_);
    }
}

// A prompt already received means the device is idle, where a stray character would corrupt the next command.
void StnLink::interruptReceive(std::chrono::milliseconds timeout)
{
    if (drainUntilTerminator(std::chrono::milliseconds::zero(), discardLine) == EventKind::Prompt) {
        return;
    }
    writeTraced("X");
    expectTerminator(EventKind::Prompt, timeout, discardLine);
}

StnLink::EventKind StnLink::drainUntilTerminator(
    std::chrono::milliseconds silenceTimeout,
    const LineSink& sink,
    const std::function<bool()>& isComplete)
{
    while (true) {
        auto event = nextEvent(std::chrono::steady_clock::now() + silenceTimeout);
        if (event.kind != EventKind::Line) {
            return event.kind;
        }
        sink(event.line);
        if (isComplete && isComplete()) {
            return EventKind::Completed;
        }
    }
}

StnLink::Event StnLink::nextEvent(std::chrono::steady_clock::time_point deadline)
{
    std::array<char, 512> buffer {};
    while (true) {
        if (auto event = drainInbox()) {
            return std::move(*event);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return Event { EventKind::Timeout, {} };
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto received = stream_.read(buffer.data(), buffer.size(), remaining);
        inbox_.append(buffer.data(), received);
    }
}

std::optional<StnLink::Event> StnLink::drainInbox()
{
    while (inboxPosition_ < inbox_.size()) {
        if (auto event = consume(inbox_[inboxPosition_++])) {
            return event;
        }
    }
    inbox_.clear();
    inboxPosition_ = 0;
    return std::nullopt;
}

std::optional<StnLink::Event> StnLink::consume(char c)
{
    if (c == Prompt) {
        const auto kind = pending_ == "DATA" ? EventKind::DataPrompt : EventKind::Prompt;
        pending_.clear();
        return Event { kind, {} };
    }
    if (c == '\n' || c == '\0') {
        return std::nullopt;
    }
    if (c != CommandTerminator) {
        pending_.push_back(c);
        return std::nullopt;
    }
    auto text = trimmed(pending_);
    pending_.clear();
    if (text.empty() || text == lastCommand_) {
        return std::nullopt;
    }
    trace_(TraceDirection::Received, text);
    return Event { EventKind::Line, StnLine { std::move(text), std::chrono::steady_clock::now() } };
}

void StnLink::resetBuffers()
{
    inbox_.clear();
    inboxPosition_ = 0;
    pending_.clear();
}

}
