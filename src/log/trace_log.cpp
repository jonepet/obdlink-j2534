#include "log/trace_log.h"

#include <format>
#include <stdexcept>

namespace obdlink {

TraceLog::TraceLog(const std::string& path)
    : file_(path, std::ios::app)
{
    if (!file_) {
        throw std::runtime_error("cannot open log file " + path);
    }
}

void TraceLog::write(std::string_view tag, std::string_view text)
{
    if (!file_.is_open()) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - epoch_);
    std::lock_guard lock(mutex_);
    file_ << std::format("{:>12} {} {}\n", elapsed.count(), tag, text);
    file_.flush();
}

}
