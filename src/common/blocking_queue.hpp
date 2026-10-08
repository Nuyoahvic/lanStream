#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

// For frame transfer from the decoding thread to the main rendering thread; drops old frames when full to maintain low latency.
template<typename T>
class BlockingQueue
{
    std::queue<T>           q_;
    std::mutex              mtx_;
    std::condition_variable cv_;
    size_t                  max_size_;
    bool                    closed_ = false;
public:
    explicit BlockingQueue(size_t max_size = 3) : max_size_(max_size) {}

    void push(T val) {
        std::unique_lock<std::mutex> lk(mtx_);
        while (q_.size() >= max_size_) q_.pop();   // 丢旧帧
        q_.push(std::move(val));
        cv_.notify_one();
    }

    std::optional<T> pop(int timeout_ms = 50) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [this]{ return !q_.empty() || closed_; });
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front()); q_.pop();
        return v;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mtx_);
        closed_ = true;
        cv_.notify_all();
    }
};
