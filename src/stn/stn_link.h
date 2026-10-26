#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "transport/byte_stream.h"

namespace obdlink {

struct StnLine {
    std::string text;
    std::chrono::steady_clock::time_point receivedAt;
};

enum class TraceDirection { Sent, Received };

// Command/response framing of the STN UART protocol: every command is CR-terminated and the
// device signals readiness for the next one with a '>' prompt.
class StnLink {
public:
    using LineSink = std::function<void(const StnLine&)>;
    using TraceSink = std::function<void(TraceDirection, std::string_view)>;

    explicit StnLink(ByteStream& stream);

    void setTrace(TraceSink trace);

    std::vector<StnLine> execute(std::string_view command, std::chrono::milliseconds timeout);

    void executeStreaming(std::string_view command, std::chrono::milliseconds timeout, const LineSink& sink);

    // For STPX l:<n>: sends the command, waits for the DATA> prompt, then sends the hex payload.
    // Once isComplete() turns true after a delivered line, the device's receive is interrupted and
    // everything up to the prompt, including the resulting STOPPED, is discarded.
    void executeWithPayload(
        std::string_view command,
        std::string_view hexPayload,
        std::chrono::milliseconds timeout,
        const LineSink& sink,
        const std::function<bool()>& isComplete = {});

    // STSBR: the device answers OK at the current rate, then switches and prompts at the new one.
    void switchBaudRate(unsigned baud, std::chrono::milliseconds timeout);

    // ATZ: the device restarts at its power-up rate, whatever rate an earlier STSBR selected.
    void reset(unsigned powerUpBaud, std::chrono::milliseconds timeout);

    // Runs a monitoring command, passing every line to the sink, until keepRunning() turns false
    // (checked at least every poll interval), then stops it and waits for the prompt.
    void monitor(
        std::string_view command,
        std::chrono::milliseconds poll,
        std::chrono::milliseconds stopTimeout,
        const LineSink& sink,
        const std::function<bool()>& keepRunning);

    // Brings the device back to an idle prompt with an empty command line, whether it was idle
    // or still executing. Never sends a lone CR: the device repeats the last command on one.
    void resynchronize(std::chrono::milliseconds busyProbe, std::chrono::milliseconds timeout);

private:
    enum class EventKind { Line, Prompt, DataPrompt, Timeout, Completed };

    struct Event {
        EventKind kind;
        StnLine line;
    };

    void send(std::string_view text);
    void writeTraced(std::string_view bytes);
    Event nextEvent(std::chrono::steady_clock::time_point deadline);
    std::optional<Event> drainInbox();
    std::optional<Event> consume(char c);
    EventKind drainUntilTerminator(
        std::chrono::milliseconds silenceTimeout,
        const LineSink& sink,
        const std::function<bool()>& isComplete = {});
    void expectTerminator(
        EventKind expected,
        std::chrono::milliseconds silenceTimeout,
        const LineSink& sink,
        const std::function<bool()>& isComplete = {});
    void interruptReceive(std::chrono::milliseconds timeout);
    void resetBuffers();

    ByteStream& stream_;
    TraceSink trace_;
    std::string inbox_;
    std::size_t inboxPosition_ = 0;
    std::string pending_;
    std::string lastCommand_;
};

}
