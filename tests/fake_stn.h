#pragma once

#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "transport/byte_stream.h"

namespace obdlink::testing {

// Emulates the STN UART protocol: CR-terminated commands answered with lines and a '>' prompt,
// STPX l:<n> answered with DATA> and the scripted response lines for the payload that follows.
// Bytes only get through when host and device run the same UART rate; STSBR switches the device
// rate after its OK and ATZ returns it to the power-up rate.
class FakeStn : public ByteStream {
public:
    static constexpr unsigned PowerUpBaud = 115200;

    void respond(const std::string& command, std::vector<std::string> lines);
    void respondToPayload(const std::string& payloadHex, std::vector<std::string> lines);
    // Like respondToPayload, but the prompt stays withheld until the host sends a character,
    // which stops the receive with STOPPED.
    void respondToPayloadWithoutPrompt(const std::string& payloadHex, std::vector<std::string> lines);
    // Like respondToPayload, but the prompt only goes out when the host's next byte arrives, and
    // that byte starts a new command line: the host's interrupt crosses the device's prompt.
    void respondToPayloadWithLatePrompt(const std::string& payloadHex, std::vector<std::string> lines);
    bool receiveInterrupted() const;
    // Lines printed once STM starts; any single character then stops the monitor with STOPPED.
    void monitorLines(std::vector<std::string> lines);
    void silence(const std::string& command);
    void runAt(unsigned baud);

    std::vector<std::string> commands() const;
    std::vector<unsigned> baudRates() const;

    bool waitForCommand(const std::string& command, std::chrono::milliseconds timeout);
    bool waitForCommandCount(const std::string& command, std::size_t count, std::chrono::milliseconds timeout);

    void write(std::string_view bytes) override;
    std::size_t read(char* buffer, std::size_t capacity, std::chrono::milliseconds timeout) override;
    void setBaudRate(unsigned baud) override;
    void discardInput() override;

private:
    void handleCommand(const std::string& line);
    void handlePayload(const std::string& hex);
    void emit(const std::vector<std::string>& lines);
    void emitRaw(const std::string& bytes);
    void switchRate(const std::string& command);

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, std::vector<std::string>> responses_;
    std::map<std::string, std::vector<std::string>> payloadResponses_;
    std::vector<std::string> monitorLines_;
    std::set<std::string> promptWithheld_;
    std::set<std::string> promptLate_;
    bool promptPending_ = false;
    bool commandTruncated_ = false;
    bool monitoring_ = false;
    bool receiving_ = false;
    bool receiveInterrupted_ = false;
    std::set<std::string> silent_;
    std::vector<std::string> commands_;
    std::vector<unsigned> baudRates_;
    std::string input_;
    std::string output_;
    std::vector<unsigned> outputRates_;
    unsigned deviceBaud_ = PowerUpBaud;
    unsigned hostBaud_ = PowerUpBaud;
    bool awaitingPayload_ = false;
};

}
