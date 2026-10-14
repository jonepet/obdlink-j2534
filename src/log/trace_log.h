#pragma once

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace obdlink {

class TraceLog {
public:
    TraceLog() = default;

    explicit TraceLog(const std::string& path);

    void write(std::string_view tag, std::string_view text);

private:
    std::mutex mutex_;
    std::ofstream file_;
    std::chrono::steady_clock::time_point epoch_ = std::chrono::steady_clock::now();
};

}
