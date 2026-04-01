#pragma once
#include "types.h"
#include <mutex>
#include <vector>

namespace u2g {

struct LogEntry {
    LogLevel level;
    std::string message;
};

class Log {
public:
    void setCallback(LogCallback cb) {
        std::lock_guard<std::mutex> lk(mutex_);
        callback_ = std::move(cb);
    }

    void info (const std::string& msg) { log(LogLevel::Info,  msg); }
    void warn (const std::string& msg) { log(LogLevel::Warn,  msg); }
    void error(const std::string& msg) { log(LogLevel::Error, msg); }
    void fatal(const std::string& msg) { log(LogLevel::Fatal, msg); }

    std::vector<LogEntry> entries() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return entries_;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mutex_);
        entries_.clear();
    }

private:
    void log(LogLevel lvl, const std::string& msg) {
        std::lock_guard<std::mutex> lk(mutex_);
        entries_.push_back({lvl, msg});
        if (callback_) callback_(lvl, msg);
    }

    std::vector<LogEntry> entries_;
    LogCallback callback_;
    mutable std::mutex mutex_;
};

} // namespace u2g
