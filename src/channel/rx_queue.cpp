#include "channel/rx_queue.h"

#include <algorithm>
#include <utility>

namespace obdlink {

RxQueue::RxQueue(std::size_t capacity)
    : capacity_(capacity)
{
}

void RxQueue::push(const j2534::PassThruMsg& message)
{
    {
        std::lock_guard lock(mutex_);
        if (messages_.size() >= capacity_) {
            overflow_ = true;
            return;
        }
        messages_.push_back(message);
    }
    available_.notify_all();
}

std::vector<j2534::PassThruMsg> RxQueue::pop(std::size_t count, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    const auto satisfiable = std::min(count, capacity_);
    available_.wait_for(lock, timeout, [this, satisfiable] { return closed_ || messages_.size() >= satisfiable; });
    const auto taken = std::min(count, messages_.size());
    std::vector<j2534::PassThruMsg> result(messages_.begin(), messages_.begin() + static_cast<std::ptrdiff_t>(taken));
    messages_.erase(messages_.begin(), messages_.begin() + static_cast<std::ptrdiff_t>(taken));
    return result;
}

void RxQueue::markOverflow()
{
    std::lock_guard lock(mutex_);
    overflow_ = true;
}

bool RxQueue::takeOverflow()
{
    std::lock_guard lock(mutex_);
    return std::exchange(overflow_, false);
}

void RxQueue::clear()
{
    std::lock_guard lock(mutex_);
    messages_.clear();
    overflow_ = false;
}

void RxQueue::close()
{
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    available_.notify_all();
}

}
