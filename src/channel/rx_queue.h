#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

#include "j2534/j2534_defs.h"

namespace obdlink {

class RxQueue {
public:
    explicit RxQueue(std::size_t capacity);

    void push(const j2534::PassThruMsg& message);

    // Waits until `count` messages (at most the capacity) are available, the queue is closed,
    // or the timeout elapses, then returns up to `count`.
    std::vector<j2534::PassThruMsg> pop(std::size_t count, std::chrono::milliseconds timeout);

    void markOverflow();

    bool takeOverflow();

    void clear();

    void close();

private:
    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<j2534::PassThruMsg> messages_;
    bool overflow_ = false;
    bool closed_ = false;
};

}
