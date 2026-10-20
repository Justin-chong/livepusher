#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstddef>
#include <utility>

namespace livepusher {

// 线程安全的有界阻塞队列
// 用于多线程之间传递数据（采集->编码->推流）
template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t max_size)
        : max_size_(max_size), closed_(false) {}

    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue(ThreadSafeQueue&&) = delete;
    ThreadSafeQueue& operator=(ThreadSafeQueue&&) = delete;

    bool push(const T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] {
            return queue_.size() < max_size_ || closed_;
        });
        if (closed_) return false;

        queue_.push(item);
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    bool push(T&& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] {
            return queue_.size() < max_size_ || closed_;
        });
        if (closed_) return false;

        queue_.push(std::move(item));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] {
            return !queue_.empty() || closed_;
        });
        if (queue_.empty()) return false;

        item = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    bool try_pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;

        item = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    // 非阻塞入队：塞满就丢弃，立即返回 false，绝不等待。
    // 给「采集线程」用——采集必须永远不被下游（编码/网络）拖慢，
    // 否则 dshow 实时缓冲没人读就会溢出丢帧。
    bool try_push(const T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) return false;
        if (queue_.size() >= max_size_) return false;   // 满了，丢

        queue_.push(item);
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    void close() {
        std::unique_lock<std::mutex> lock(mutex_);
        closed_ = true;
        lock.unlock();
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    // 重新打开队列：清空残留数据、复位 closed 标志，供「停止后再次推流」复用。
    // 必须在所有生产/消费线程都退出后调用，否则有数据竞争。
    void reset() {
        std::unique_lock<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
        closed_ = false;
    }

    bool is_closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    size_t max_size_;
    bool closed_;
};

}  // namespace livepusher
