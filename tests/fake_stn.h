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
class FakeStn : public ByteStream {
public:
    void respond(const std::string& command, std::vector<std::string> lines);
    void respondToPayload(const std::string& payloadHex, std::vector<std::string> lines);
    void silence(const std::string& command);

    std::vector<std::string> commands() const;
    std::vector<unsigned> baudRates() const;

    bool waitForCommand(const std::string& command, std::chrono::milliseconds timeout);

    void write(std::string_view bytes) override;
    std::size_t read(char* buffer, std::size_t capacity, std::chrono::milliseconds timeout) override;
    void setBaudRate(unsigned baud) override;
    void discardInput() override;

private:
    void handleCommand(const std::string& line);
    void handlePayload(const std::string& hex);
    void emit(const std::vector<std::string>& lines);

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, std::vector<std::string>> responses_;
    std::map<std::string, std::vector<std::string>> payloadResponses_;
    std::set<std::string> silent_;
    std::vector<std::string> commands_;
    std::vector<unsigned> baudRates_;
    std::string input_;
    std::string output_;
    bool awaitingPayload_ = false;
};

}
