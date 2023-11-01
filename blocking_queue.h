#pragma once

#include <cstddef>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

template <typename T>
class BlockingQueue {
  public:
    explicit BlockingQueue(std::size_t max_size = 0) : max_size_(max_size) {}

    bool push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || full()) {
                return false;
            }
            queue_.push(std::move(value));
        }

        cv_.notify_one();
        return true;
    }
    std::optional<T> wait_and_pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || closed_; });

        if (queue_.empty()) {
            return std::nullopt;
        }

        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }

        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    std::size_t max_size_;
    bool closed_ = false;

    bool full() const {
        return max_size_ != 0 && queue_.size() >= max_size_;
    }
};
