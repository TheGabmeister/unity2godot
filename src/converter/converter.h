#pragma once
#include "util/types.h"
#include "util/log.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

namespace u2g {

struct SkipCategory {
    std::string name;
    std::vector<std::string> files;
};

class Converter {
public:
    ~Converter();

    void start(const std::string& packagePath, const std::string& outputDir);
    void cancel();

    bool isRunning() const { return running_.load(); }
    bool isDone()    const { return done_.load(); }

    Log& log() { return log_; }

    ConversionProgress currentProgress() const {
        std::lock_guard<std::mutex> lk(progressMutex_);
        return progress_;
    }

    std::vector<SkipCategory> skipReport() const {
        std::lock_guard<std::mutex> lk(skipMutex_);
        return skipReport_;
    }

private:
    void run(std::string packagePath, std::string outputDir);
    void setProgress(const std::string& phase, const std::string& asset, float pct);

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> done_{false};
    std::atomic<bool> cancelled_{false};
    Log log_;

    ConversionProgress progress_;
    mutable std::mutex progressMutex_;

    std::vector<SkipCategory> skipReport_;
    mutable std::mutex skipMutex_;
};

} // namespace u2g
