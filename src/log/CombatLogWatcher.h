#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace bean::log {

class CombatLogWatcher {
public:
    using LineCallback = std::function<void(const std::string&)>;
    struct DebugSnapshot {
        std::filesystem::path activeFile;
        std::uintmax_t lastPosition = 0;
        bool streamOpen = false;
        std::optional<std::chrono::system_clock::time_point> lastLineAt;
        std::uint64_t staleSeekRecoveries = 0;
    };

    CombatLogWatcher() = default;
    ~CombatLogWatcher();

    CombatLogWatcher(const CombatLogWatcher&) = delete;
    CombatLogWatcher& operator=(const CombatLogWatcher&) = delete;

    void SetLogDirectory(std::filesystem::path directory);
    bool Start(LineCallback callback, std::string& error);
    void Stop();
    bool IsRunning() const;
    DebugSnapshot GetDebugSnapshot() const;

private:
    std::filesystem::path FindLatestLogFile() const;
    void RunLoop(LineCallback callback);

    std::filesystem::path directory_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex debugMutex_;
    DebugSnapshot debugSnapshot_{};
};

} // namespace bean::log
