#include "log/CombatLogWatcher.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <regex>
#if defined(_WIN32)
#include <share.h>
#endif

namespace bean::log {
namespace {

bool IsCombatLogName(const std::filesystem::path& path)
{
    static const std::regex kPattern(R"(WoWCombatLog-\d{6}_\d{6}\.txt)");
    return std::regex_match(path.filename().string(), kPattern);
}

} // namespace

CombatLogWatcher::~CombatLogWatcher()
{
    Stop();
}

void CombatLogWatcher::SetLogDirectory(std::filesystem::path directory)
{
    directory_ = std::move(directory);
}

bool CombatLogWatcher::Start(LineCallback callback, std::string& error)
{
    error.clear();
    if (running_.exchange(true)) {
        error = "Watcher already running.";
        return false;
    }
    if (directory_.empty()) {
        running_ = false;
        error = "Combat log directory is not set.";
        return false;
    }
    if (!std::filesystem::exists(directory_)) {
        running_ = false;
        error = "Combat log directory does not exist.";
        return false;
    }

    worker_ = std::thread([this, callback]() { RunLoop(callback); });
    return true;
}

void CombatLogWatcher::Stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool CombatLogWatcher::IsRunning() const
{
    return running_.load();
}

CombatLogWatcher::DebugSnapshot CombatLogWatcher::GetDebugSnapshot() const
{
    std::scoped_lock lock(debugMutex_);
    return debugSnapshot_;
}

std::filesystem::path CombatLogWatcher::FindLatestLogFile() const
{
    std::filesystem::path newestPath;
    std::filesystem::file_time_type newestTime{};
    bool foundAny = false;

    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto& path = entry.path();
        if (!IsCombatLogName(path)) {
            continue;
        }

        const auto writeTime = entry.last_write_time();
        if (!foundAny || writeTime > newestTime) {
            foundAny = true;
            newestTime = writeTime;
            newestPath = path;
        }
    }

    return newestPath;
}

void CombatLogWatcher::RunLoop(LineCallback callback)
{
    std::filesystem::path activeFile;
    std::uintmax_t lastPosition = 0;

    while (running_.load()) {
        std::error_code ec;
        if (!std::filesystem::exists(directory_, ec)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        const auto latestFile = FindLatestLogFile();
        if (!latestFile.empty() && latestFile != activeFile) {
            const bool hadActiveFile = !activeFile.empty();
            activeFile = latestFile;
            if (hadActiveFile) {
                // Mid-session file switch: read from beginning so startup events
                // in the new log are not missed.
                lastPosition = 0;
            } else {
                // Initial attach on monitoring start: tail from end to avoid
                // replaying historical events from an older existing log.
                std::error_code initialSizeEc;
                const auto initialSize = std::filesystem::file_size(activeFile, initialSizeEc);
                lastPosition = initialSizeEc ? 0 : initialSize;
            }
            {
                std::scoped_lock lock(debugMutex_);
                debugSnapshot_.activeFile = activeFile;
                debugSnapshot_.streamOpen = false;
                debugSnapshot_.lastPosition = lastPosition;
            }
        }

        if (!activeFile.empty()) {
            std::error_code sizeEc;
            const auto fileSize = std::filesystem::file_size(activeFile, sizeEc);
            if (!sizeEc && fileSize < lastPosition) {
                // Combat log can be truncated/rotated in place; restart from beginning.
                lastPosition = 0;
            }

            bool streamOpen = false;
#if defined(_WIN32)
            // Open with explicit read/write sharing so WoW can keep appending.
            FILE* stream = _wfsopen(activeFile.c_str(), L"rb", _SH_DENYNO);
            if (stream != nullptr) {
                streamOpen = true;
                if (_fseeki64(stream, static_cast<__int64>(lastPosition), SEEK_SET) != 0) {
                    std::error_code seekSizeEc;
                    const auto seekSize = std::filesystem::file_size(activeFile, seekSizeEc);
                    if (!seekSizeEc) {
                        lastPosition = seekSize;
                    } else {
                        lastPosition = 0;
                    }
                    {
                        std::scoped_lock lock(debugMutex_);
                        debugSnapshot_.activeFile = activeFile;
                        debugSnapshot_.streamOpen = true;
                        debugSnapshot_.lastPosition = lastPosition;
                        ++debugSnapshot_.staleSeekRecoveries;
                    }
                } else {
                    std::string line;
                    int ch = std::fgetc(stream);
                    while (ch != EOF) {
                        if (ch == '\n') {
                            if (!line.empty() && line.back() == '\r') {
                                line.pop_back();
                            }
                            callback(line);
                            line.clear();
                            const auto newPos = _ftelli64(stream);
                            if (newPos >= 0) {
                                lastPosition = static_cast<std::uintmax_t>(newPos);
                            }
                            {
                                std::scoped_lock lock(debugMutex_);
                                debugSnapshot_.activeFile = activeFile;
                                debugSnapshot_.streamOpen = true;
                                debugSnapshot_.lastPosition = lastPosition;
                                debugSnapshot_.lastLineAt = std::chrono::system_clock::now();
                            }
                        } else {
                            line.push_back(static_cast<char>(ch));
                        }
                        ch = std::fgetc(stream);
                    }

                    std::error_code eofSizeEc;
                    const auto eofSize = std::filesystem::file_size(activeFile, eofSizeEc);
                    if (!eofSizeEc) {
                        lastPosition = eofSize;
                    }
                    {
                        std::scoped_lock lock(debugMutex_);
                        debugSnapshot_.activeFile = activeFile;
                        debugSnapshot_.streamOpen = true;
                        debugSnapshot_.lastPosition = lastPosition;
                    }
                }
                std::fclose(stream);
            }
#else
            // Keep the stream in binary mode so seek/tell positions are byte offsets.
            std::ifstream stream(activeFile, std::ios::in | std::ios::binary);
            if (stream.is_open()) {
                streamOpen = true;
                stream.seekg(static_cast<std::streamoff>(lastPosition), std::ios::beg);
                if (!stream.good()) {
                    stream.clear();
                    std::error_code seekSizeEc;
                    const auto seekSize = std::filesystem::file_size(activeFile, seekSizeEc);
                    if (!seekSizeEc) {
                        lastPosition = seekSize;
                    } else {
                        lastPosition = 0;
                    }
                    {
                        std::scoped_lock lock(debugMutex_);
                        debugSnapshot_.activeFile = activeFile;
                        debugSnapshot_.streamOpen = true;
                        debugSnapshot_.lastPosition = lastPosition;
                        ++debugSnapshot_.staleSeekRecoveries;
                    }
                } else {
                    std::string line;
                    while (std::getline(stream, line)) {
                        if (!line.empty() && line.back() == '\r') {
                            line.pop_back();
                        }
                        callback(line);
                        const auto newPos = stream.tellg();
                        if (newPos >= 0) {
                            lastPosition = static_cast<std::uintmax_t>(newPos);
                        }
                        {
                            std::scoped_lock lock(debugMutex_);
                            debugSnapshot_.activeFile = activeFile;
                            debugSnapshot_.streamOpen = true;
                            debugSnapshot_.lastPosition = lastPosition;
                            debugSnapshot_.lastLineAt = std::chrono::system_clock::now();
                        }
                    }

                    if (stream.eof()) {
                        std::error_code eofSizeEc;
                        const auto eofSize = std::filesystem::file_size(activeFile, eofSizeEc);
                        if (!eofSizeEc) {
                            lastPosition = eofSize;
                        }
                        {
                            std::scoped_lock lock(debugMutex_);
                            debugSnapshot_.activeFile = activeFile;
                            debugSnapshot_.streamOpen = true;
                            debugSnapshot_.lastPosition = lastPosition;
                        }
                    } else if (stream.fail()) {
                        stream.clear();
                    }
                }
            }
#endif

            if (!streamOpen) {
                std::scoped_lock lock(debugMutex_);
                debugSnapshot_.activeFile = activeFile;
                debugSnapshot_.streamOpen = false;
                debugSnapshot_.lastPosition = lastPosition;
            }
        } else {
            std::scoped_lock lock(debugMutex_);
            debugSnapshot_.activeFile = activeFile;
            debugSnapshot_.streamOpen = false;
            debugSnapshot_.lastPosition = lastPosition;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace bean::log
