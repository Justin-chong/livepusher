#pragma once

#include <iostream>
#include <sstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <thread>
#include <string>
#include <functional>

#include "log_level.h"

namespace livepusher {

class Logger {
public:
    // 日志输出回调：GUI 注册后，每条日志都会通过它转发出去（用于显示到界面）。
    // 签名：void(const std::string& 已格式化的一行日志)
    using LogSink = std::function<void(const std::string&)>;

    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void set_level(LogLevel level) {
        level_ = level;
    }

    LogLevel level() const {
        return level_;
    }

    // 注册/取消日志回调。注册后日志会同时发到回调；std::cout 输出保留。
    void set_sink(LogSink sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        sink_ = std::move(sink);
    }

    void write(const std::string& log) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << log << std::endl;
        if (sink_) {
            sink_(log);
        }
    }

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel level_ = LogLevel::DEBUG;
    std::mutex mutex_;
    LogSink sink_;
};

class LogStream {
public:
    LogStream(LogLevel level, const char* file, int line)
        : level_(level) {
        init_timestamp();
        init_thread_id();
        extract_filename(file);
        line_ = line;
    }

    ~LogStream() {
        if (level_ < Logger::instance().level()) {
            return;
        }
        std::stringstream ss;
        ss << "[" << timestamp_str_ << "]"
           << "[" << level_str(level_) << "]"
           << "[" << thread_id_ << "]"
           << "[" << filename_ << ":" << line_ << "] "
           << message_stream_.str()
           << std::endl;
        Logger::instance().write(ss.str());
    }

    template <typename T>
    LogStream& operator<<(const T& value) {
        message_stream_ << value;
        return *this;
    }

private:
    void init_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &time_t_now);
#else
        localtime_r(&time_t_now, &tm_buf);
#endif

        std::stringstream ss;
        ss << std::put_time(&tm_buf, "%H:%M:%S")
           << "."
           << std::setfill('0')
           << std::setw(3)
           << ms.count();
        timestamp_str_ = ss.str();
    }

    void init_thread_id() {
        std::stringstream ss;
        ss << std::this_thread::get_id();
        thread_id_ = ss.str();
    }

    void extract_filename(const char* file) {
        const char* p = file;
        const char* last = file;
        while (*p) {
            if (*p == '/' || *p == '\\') {
                last = p + 1;
            }
            ++p;
        }
        filename_ = last;
    }

    static const char* level_str(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERR:   return "ERROR";
            default:              return "UNKNOWN";
        }
    }

    LogLevel level_;
    std::string timestamp_str_;
    std::string thread_id_;
    std::string filename_;
    int line_ = 0;
    std::ostringstream message_stream_;
};

#define LOG_DEBUG \
    ::livepusher::LogStream(::livepusher::LogLevel::DEBUG, __FILE__, __LINE__)
#define LOG_INFO \
    ::livepusher::LogStream(::livepusher::LogLevel::INFO, __FILE__, __LINE__)
#define LOG_WARN \
    ::livepusher::LogStream(::livepusher::LogLevel::WARN, __FILE__, __LINE__)
#define LOG_ERROR \
    ::livepusher::LogStream(::livepusher::LogLevel::ERR, __FILE__, __LINE__)

}  // namespace livepusher
