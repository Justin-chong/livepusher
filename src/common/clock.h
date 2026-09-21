#pragma once

#include <cstdint>
#include <chrono>

namespace livepusher {

class Clock {
public:
    static int64_t now_us() {
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration);
        return us.count();
    }

    static int64_t now_ms() {
        return now_us() / 1000;
    }

    void start() {
        start_us_ = now_us();
        started_ = true;
    }

    int64_t elapsed_us() const {
        if (!started_) {
            return 0;
        }
        return now_us() - start_us_;
    }

    int64_t elapsed_ms() const {
        return elapsed_us() / 1000;
    }

private:
    int64_t start_us_ = 0;
    bool started_ = false;
};

}  // namespace livepusher
