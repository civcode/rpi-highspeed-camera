#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace hscam::internal {

template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    bool tryPush(T value)
    {
        std::lock_guard lock(mutex_);
        if (closed_ || queue_.size() >= capacity_)
            return false;
        queue_.push_back(std::move(value));
        cv_.notify_one();
        return true;
    }

    std::optional<T> popFor(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [&] { return closed_ || !queue_.empty(); }))
            return std::nullopt;
        if (queue_.empty())
            return std::nullopt;
        T value = std::move(queue_.front());
        queue_.pop_front();
        return value;
    }

    void close()
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

    [[nodiscard]] std::size_t size() const
    {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    bool closed_{};
};

} // namespace hscam::internal
