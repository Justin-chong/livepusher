#pragma once

#include <cstdint>
#include <chrono>

namespace livepusher {

class Clock {
public:
    static int64_t now_us() {
        // 进程级静态起点：第一次调用时记录，之后所有调用都减它
        // 这样返回的是「进程启动以来的相对微秒」，全进程共享同一个 0 点
        static auto origin = std::chrono::steady_clock::now();//static 局部变量的初始化只执行一次。
        auto now = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - origin);
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
