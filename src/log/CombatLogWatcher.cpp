#include "log/CombatLogWatcher.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace bean::log {
namespace {

// Idle cost of a poll is a single GetFileSizeEx, so this can be far tighter
// than the directory-scanning loop it replaced.
constexpr DWORD kPollIntervalMs = 150;
// Safety net for filesystems where change notifications are unreliable.
constexpr auto kDirectoryRescanInterval = std::chrono::seconds(30);
constexpr std::size_t kReadBufferBytes = 128 * 1024;

// Matches "WoWCombatLog-MMDDYY_HHMMSS.txt". Hand-rolled rather than std::regex
// because this runs against every file in the directory on each rescan.
bool IsCombatLogName(const std::filesystem::path& path)
{
    constexpr std::string_view kPrefix = "WoWCombatLog-";
    constexpr std::string_view kSuffix = ".txt";
    constexpr std::size_t kDateDigits = 6;
    constexpr std::size_t kTimeDigits = 6;
    constexpr std::size_t kExpectedLength =
        kPrefix.size() + kDateDigits + 1 + kTimeDigits + kSuffix.size();

    const auto name = path.filename().string();
    if (name.size() != kExpectedLength) {
        return false;
    }
    if (std::string_view(name).substr(0, kPrefix.size()) != kPrefix) {
        return false;
    }
    if (std::string_view(name).substr(name.size() - kSuffix.size()) != kSuffix) {
        return false;
    }

    const auto isDigitRun = [&name](std::size_t offset, std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            if (std::isdigit(static_cast<unsigned char>(name[offset + index])) == 0) {
                return false;
            }
        }
        return true;
    };

    const std::size_t dateOffset = kPrefix.size();
    const std::size_t separatorOffset = dateOffset + kDateDigits;
    const std::size_t timeOffset = separatorOffset + 1;
    return isDigitRun(dateOffset, kDateDigits)
        && name[separatorOffset] == '_'
        && isDigitRun(timeOffset, kTimeDigits);
}

// Holds one combat log open and yields complete lines as they are appended.
//
// The partial trailing line lives in `pending_` across reads. That is the whole
// reason this is a class: WoW flushes in buffer-sized chunks that routinely land
// mid-line, and a reader that forgets the fragment both loses that line and
// mis-frames the next one.
class LogFileTail {
public:
    LogFileTail() = default;

    ~LogFileTail()
    {
        Close();
    }

    LogFileTail(const LogFileTail&) = delete;
    LogFileTail& operator=(const LogFileTail&) = delete;

    bool Open(const std::filesystem::path& path, bool tailFromEnd)
    {
        Close();

        // Sharing is negotiated at open time and is what decides whether WoW can
        // keep writing. Read-only access plus all three share bits is strictly
        // more permissive than the CRT's _SH_DENYNO, which omits delete sharing
        // and would block the file being renamed or removed while we hold it.
        handle_ = CreateFileW(
            path.c_str(),
            FILE_READ_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }
        BY_HANDLE_FILE_INFORMATION fileInfo{};
        if (!GetFileInformationByHandle(handle_, &fileInfo)) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return false;
        }

        path_ = path;
        volumeSerialNumber_ = fileInfo.dwVolumeSerialNumber;
        fileIndex_ = (static_cast<std::uint64_t>(fileInfo.nFileIndexHigh) << 32)
            | fileInfo.nFileIndexLow;
        identityValid_ = true;
        buffer_.resize(kReadBufferBytes);
        pending_.clear();

        std::uintmax_t startOffset = 0;
        if (tailFromEnd) {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(handle_, &size)) {
                startOffset = static_cast<std::uintmax_t>(size.QuadPart);
            }
        }
        if (!Seek(startOffset)) {
            Close();
            return false;
        }
        offset_ = startOffset;
        return true;
    }

    void Close()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        path_.clear();
        pending_.clear();
        offset_ = 0;
        volumeSerialNumber_ = 0;
        fileIndex_ = 0;
        identityValid_ = false;
    }

    bool IsOpen() const
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    const std::filesystem::path& Path() const
    {
        return path_;
    }

    std::uintmax_t Offset() const
    {
        return offset_;
    }

    bool IsSameFileOnDisk(const std::filesystem::path& candidate) const
    {
        if (!identityValid_ || candidate.empty()) {
            return false;
        }
        const HANDLE candidateHandle = CreateFileW(
            candidate.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (candidateHandle == INVALID_HANDLE_VALUE) {
            return false;
        }
        BY_HANDLE_FILE_INFORMATION fileInfo{};
        const bool readIdentity = GetFileInformationByHandle(candidateHandle, &fileInfo) != FALSE;
        CloseHandle(candidateHandle);
        if (!readIdentity) {
            return false;
        }
        const auto fileIndex = (static_cast<std::uint64_t>(fileInfo.nFileIndexHigh) << 32)
            | fileInfo.nFileIndexLow;
        return fileInfo.dwVolumeSerialNumber == volumeSerialNumber_
            && fileIndex == fileIndex_;
    }

    // Delivers every complete line currently available. Returns how many.
    std::size_t Drain(
        const CombatLogWatcher::LineCallback& callback,
        std::uint64_t& truncationRecoveries,
        bool& readError)
    {
        readError = false;
        if (!IsOpen()) {
            return 0;
        }

        LARGE_INTEGER sizeInfo{};
        if (!GetFileSizeEx(handle_, &sizeInfo)) {
            readError = true;
            return 0;
        }
        const auto fileSize = static_cast<std::uintmax_t>(sizeInfo.QuadPart);

        if (fileSize < offset_) {
            // The log can be truncated in place; restart from the beginning.
            ++truncationRecoveries;
            pending_.clear();
            if (!Seek(0)) {
                return 0;
            }
            offset_ = 0;
        }
        if (fileSize == offset_) {
            // The common idle case: one syscall and we are done.
            return 0;
        }

        std::size_t delivered = 0;
        for (;;) {
            DWORD bytesRead = 0;
            if (!ReadFile(handle_, buffer_.data(), static_cast<DWORD>(buffer_.size()), &bytesRead, nullptr)) {
                readError = true;
                break;
            }
            if (bytesRead == 0) {
                break;
            }
            offset_ += bytesRead;

            const char* cursor = buffer_.data();
            const char* const end = cursor + bytesRead;
            while (cursor < end) {
                const auto available = static_cast<std::size_t>(end - cursor);
                const auto* newline = static_cast<const char*>(std::memchr(cursor, '\n', available));
                if (newline == nullptr) {
                    // Incomplete tail; hold it until the newline shows up.
                    pending_.append(cursor, available);
                    break;
                }
                pending_.append(cursor, static_cast<std::size_t>(newline - cursor));
                if (!pending_.empty() && pending_.back() == '\r') {
                    pending_.pop_back();
                }
                callback(pending_);
                // clear() keeps the capacity, so steady-state parsing does not
                // allocate.
                pending_.clear();
                ++delivered;
                cursor = newline + 1;
            }

            if (bytesRead < buffer_.size()) {
                break;
            }
        }

        return delivered;
    }

private:
    bool Seek(std::uintmax_t position)
    {
        LARGE_INTEGER distance{};
        distance.QuadPart = static_cast<LONGLONG>(position);
        return SetFilePointerEx(handle_, distance, nullptr, FILE_BEGIN) != FALSE;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::filesystem::path path_;
    std::uintmax_t offset_ = 0;
    std::string pending_;
    std::vector<char> buffer_;
    DWORD volumeSerialNumber_ = 0;
    std::uint64_t fileIndex_ = 0;
    bool identityValid_ = false;
};

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
    std::error_code directoryEc;
    if (!std::filesystem::exists(directory_, directoryEc)
        || directoryEc
        || !std::filesystem::is_directory(directory_, directoryEc)
        || directoryEc) {
        running_ = false;
        error = "Combat log directory does not exist.";
        return false;
    }

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent_ == nullptr) {
        running_ = false;
        error = "Unable to create watcher stop event.";
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
    if (stopEvent_ != nullptr) {
        SetEvent(static_cast<HANDLE>(stopEvent_));
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (stopEvent_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stopEvent_));
        stopEvent_ = nullptr;
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

    std::error_code ec;
    std::filesystem::directory_iterator iterator(directory_, ec);
    if (ec) {
        return newestPath;
    }

    for (const auto& entry : iterator) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            continue;
        }
        const auto& path = entry.path();
        if (!IsCombatLogName(path)) {
            continue;
        }

        // Deliberately compared by write time, not by name: the MMDDYY stamp in
        // the filename does not sort chronologically across a year boundary.
        const auto writeTime = entry.last_write_time(entryEc);
        if (entryEc) {
            continue;
        }
        if (!foundAny || writeTime > newestTime) {
            foundAny = true;
            newestTime = writeTime;
            newestPath = path;
        }
    }

    return newestPath;
}

void CombatLogWatcher::RunLoop(const LineCallback& callback)
{
    auto tail = std::make_unique<LogFileTail>();
    std::uint64_t truncationRecoveries = 0;
    bool attachedBefore = false;

    // Fires on file create/delete/rename only. Write activity is deliberately
    // excluded: WoW appends constantly and would swamp the wait.
    HANDLE directoryNotify = FindFirstChangeNotificationW(
        directory_.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);

    bool rescanDirectory = true;
    auto lastDirectoryScan = std::chrono::steady_clock::now();

    while (running_.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastDirectoryScan >= kDirectoryRescanInterval) {
            rescanDirectory = true;
        }

        if (rescanDirectory) {
            rescanDirectory = false;
            lastDirectoryScan = now;

            const auto latest = FindLatestLogFile();
            const bool activeFileReplaced = tail->IsOpen()
                && latest == tail->Path()
                && !tail->IsSameFileOnDisk(latest);
            if (!latest.empty()
                && (!tail->IsOpen() || latest != tail->Path() || activeFileReplaced)) {
                auto next = std::make_unique<LogFileTail>();
                // A brand new log can be momentarily unopenable while WoW or an
                // antivirus scanner is still touching it, so keep tailing the
                // current file until the replacement is actually open.
                //
                // First attach tails from the end to avoid replaying an old
                // session. A later switch reads from the start, because that is
                // a relaunch and the run may already be underway.
                if (next->Open(latest, !attachedBefore)) {
                    if (tail->IsOpen()) {
                        // Final drain: on a crash these are the last lines
                        // written before the game died.
                        bool ignoredReadError = false;
                        tail->Drain(callback, truncationRecoveries, ignoredReadError);
                    }
                    tail = std::move(next);
                    attachedBefore = true;

                    std::scoped_lock lock(debugMutex_);
                    debugSnapshot_.activeFile = tail->Path();
                    debugSnapshot_.streamOpen = true;
                    debugSnapshot_.lastPosition = tail->Offset();
                    debugSnapshot_.staleSeekRecoveries = truncationRecoveries;
                }
            }
        }

        if (tail->IsOpen()) {
            bool readError = false;
            const auto delivered = tail->Drain(callback, truncationRecoveries, readError);
            if (readError) {
                tail->Close();
                attachedBefore = false;
                rescanDirectory = true;
            }

            // Snapshot once per batch rather than once per line; at dungeon log
            // rates the per-line version was pure overhead on the hot path.
            std::scoped_lock lock(debugMutex_);
            debugSnapshot_.activeFile = tail->Path();
            debugSnapshot_.streamOpen = tail->IsOpen();
            debugSnapshot_.lastPosition = tail->Offset();
            debugSnapshot_.staleSeekRecoveries = truncationRecoveries;
            if (delivered > 0) {
                debugSnapshot_.lastLineAt = std::chrono::system_clock::now();
            }
        } else {
            std::scoped_lock lock(debugMutex_);
            debugSnapshot_.streamOpen = false;
        }

        HANDLE waitHandles[2] = {static_cast<HANDLE>(stopEvent_), directoryNotify};
        const DWORD waitCount = (directoryNotify != INVALID_HANDLE_VALUE) ? 2u : 1u;
        const DWORD waitResult = WaitForMultipleObjects(waitCount, waitHandles, FALSE, kPollIntervalMs);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }
        if (waitCount == 2 && waitResult == WAIT_OBJECT_0 + 1) {
            rescanDirectory = true;
            FindNextChangeNotification(directoryNotify);
        }
    }

    if (directoryNotify != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(directoryNotify);
    }
}

} // namespace bean::log
