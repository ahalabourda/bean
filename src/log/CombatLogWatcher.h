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

// Tails the active WoW combat log.
//
// The log is opened once and held open with full sharing (read/write/delete) so
// WoW keeps appending normally, and growth is detected with a single cheap size
// check rather than by reopening the file. New log files - which is what a game
// crash and relaunch mid-dungeon produces - are picked up from a directory
// change notification, with a slow rescan as a safety net.
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
    void RunLoop(const LineCallback& callback);

    std::filesystem::path directory_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    // Win32 event used to wake the worker out of its wait immediately on Stop.
    // Typed as void* so this header stays free of <windows.h>.
    void* stopEvent_ = nullptr;
    mutable std::mutex debugMutex_;
    DebugSnapshot debugSnapshot_{};
};

} // namespace bean::log
