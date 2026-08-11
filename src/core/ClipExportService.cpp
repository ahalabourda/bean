#include "core/ClipExportService.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace bean::core {
namespace {

constexpr auto kMinCheapTrimSeconds = std::chrono::seconds(5);
constexpr int kMinClipDurationSeconds = 1;
constexpr int kMaxClipDurationSeconds = 3600;
constexpr DWORD kFfmpegProcessTimeoutMs = 30 * 60 * 1000;

std::string FormatWallClock(const std::chrono::system_clock::time_point& value)
{
    const auto time = std::chrono::system_clock::to_time_t(value);
    std::tm tm{};
    localtime_s(&tm, &time);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return os.str();
}

std::string FormatDurationClock(std::chrono::seconds duration)
{
    auto total = duration.count();
    if (total < 0) {
        total = 0;
    }
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto seconds = total % 60;
    std::ostringstream os;
    os << std::setfill('0') << std::setw(2) << hours
       << ":" << std::setw(2) << minutes
       << ":" << std::setw(2) << seconds;
    return os.str();
}

std::wstring QuoteArg(const std::filesystem::path& path)
{
    return L"\"" + path.wstring() + L"\"";
}

std::string FormatWinExitCode(DWORD exitCode)
{
    std::ostringstream os;
    os << exitCode << " (0x" << std::uppercase << std::hex << exitCode << std::nouppercase << std::dec << ")";
    return os.str();
}

std::int64_t ToEpochMilliseconds(const std::chrono::system_clock::time_point& value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point FromEpochMilliseconds(std::int64_t value)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}

std::vector<std::filesystem::path> ResolveFfmpegExecutableCandidates()
{
    std::vector<std::filesystem::path> candidates;
    const auto existsExe = [](const std::filesystem::path& candidate) -> std::optional<std::filesystem::path> {
        std::error_code ec;
        if (!candidate.empty() && std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
        return std::nullopt;
    };

    wchar_t modulePath[MAX_PATH] = {};
    const DWORD moduleLen = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (moduleLen > 0) {
        const auto localCandidate = std::filesystem::path(modulePath).parent_path() / "ffmpeg.exe";
        if (const auto resolved = existsExe(localCandidate); resolved.has_value()) {
            candidates.push_back(resolved->lexically_normal());
        }
    }

    return candidates;
}

bool WaitForProcessExit(HANDLE process, DWORD& exitCode)
{
    const DWORD waitResult = WaitForSingleObject(process, kFfmpegProcessTimeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process, ERROR_TIMEOUT);
        WaitForSingleObject(process, 5000);
        exitCode = ERROR_TIMEOUT;
        return false;
    }
    if (waitResult != WAIT_OBJECT_0) {
        exitCode = GetLastError();
        return false;
    }
    return GetExitCodeProcess(process, &exitCode) != 0;
}

bool RunProcessAndWait(const std::wstring& commandLine, DWORD& exitCode)
{
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    std::wstring mutableCommandLine = commandLine;
    if (!CreateProcessW(
            nullptr,
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo)) {
        return false;
    }

    const bool gotCode = WaitForProcessExit(processInfo.hProcess, exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return gotCode;
}

bool RunProcessAndCapture(
    const std::wstring& commandLine,
    const std::filesystem::path& capturePath,
    DWORD& exitCode)
{
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;
    HANDLE captureFile = CreateFileW(
        capturePath.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &securityAttributes,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (captureFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = captureFile;
    startupInfo.hStdError = captureFile;
    PROCESS_INFORMATION processInfo{};
    std::wstring mutableCommandLine = commandLine;
    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);
    CloseHandle(captureFile);
    if (!created) {
        return false;
    }

    const bool gotCode = WaitForProcessExit(processInfo.hProcess, exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return gotCode;
}

bool ProbeKeyframeTimes(
    const std::filesystem::path& ffmpegPath,
    const std::filesystem::path& videoPath,
    std::vector<double>& keyframes,
    std::string& error)
{
    keyframes.clear();
    error.clear();
    const auto capturePath = videoPath.parent_path() /
        (videoPath.stem().string() + ".keyframes.tmp.txt");
    std::error_code cleanupEc;
    std::filesystem::remove(capturePath, cleanupEc);

    const std::wstring commandLine = QuoteArg(ffmpegPath)
        + L" -hide_banner -loglevel info -skip_frame nokey -i " + QuoteArg(videoPath)
        + L" -an -vf showinfo -f null NUL";
    DWORD exitCode = 1;
    if (!RunProcessAndCapture(commandLine, capturePath, exitCode)) {
        error = "Unable to launch FFmpeg keyframe probe.";
        return false;
    }

    std::ifstream stream(capturePath);
    if (!stream.is_open()) {
        error = "Unable to read FFmpeg keyframe probe output.";
        return false;
    }
    const std::regex ptsPattern(R"(pts_time:([0-9]+(?:\.[0-9]+)?))");
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("iskey:1") == std::string::npos) {
            continue;
        }
        std::smatch match;
        if (std::regex_search(line, match, ptsPattern)) {
            keyframes.push_back(std::stod(match[1].str()));
        }
    }
    stream.close();
    std::filesystem::remove(capturePath, cleanupEc);
    if (keyframes.empty()) {
        error = "FFmpeg did not report any keyframes.";
        return false;
    }
    if (exitCode != 0) {
        error = "FFmpeg keyframe probe failed with exit code " + FormatWinExitCode(exitCode) + ".";
        return false;
    }
    return true;
}


} // namespace

ClipExportService::~ClipExportService()
{
    Stop();
}

void ClipExportService::SetStatusCallback(StatusCallback callback)
{
    std::scoped_lock lock(statusCallbackMutex_);
    statusCallback_ = std::move(callback);
}

void ClipExportService::SetJournalDirectory(std::filesystem::path journalDirectory)
{
    std::scoped_lock lock(pendingMutex_);
    journalDirectory_ = std::move(journalDirectory);
}

void ClipExportService::SetClipDurationSeconds(int seconds)
{
    std::scoped_lock lock(pendingMutex_);
    clipDurationSeconds_ = (std::clamp)(seconds, kMinClipDurationSeconds, kMaxClipDurationSeconds);
}

void ClipExportService::PushStatus(const std::string& status) const
{
    StatusCallback callback;
    {
        std::scoped_lock lock(statusCallbackMutex_);
        callback = statusCallback_;
    }
    if (callback) {
        callback(status);
    }
}

namespace {

std::int64_t EpochMs(const std::chrono::system_clock::time_point& value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

} // namespace

bool ClipExportService::RequestClip(
    const std::filesystem::path& videoPath,
    std::chrono::system_clock::time_point recordingStartedAt,
    std::chrono::steady_clock::time_point recordingStartedAtSteady,
    std::string& error)
{
    std::scoped_lock lock(pendingMutex_);
    error.clear();
    ClipRequest request;
    request.videoPath = videoPath;
    request.recordingStartedAt = recordingStartedAt;
    request.recordingStartedAtSteady = recordingStartedAtSteady;
    request.requestedAt = std::chrono::system_clock::now();
    request.requestedAtSteady = std::chrono::steady_clock::now();
    request.id = (static_cast<std::uint64_t>(EpochMs(request.requestedAt)) << 10)
        ^ (nextClipRequestId_++ & 0x3FFu);
    request.durationSeconds = clipDurationSeconds_;
    request.clipIndex = static_cast<int>(pendingClipRequests_.size()) + 1;
    request.outputPath = BuildClipPath(request.videoPath, request);
    if (!AppendClipJournalRequest(request.videoPath, request, error)) {
        PushStatus("Clip request journal write failed: " + error);
        return false;
    }
    pendingClipRequests_.push_back(request);
    PushStatus(
        "Clip requested (" + std::to_string(request.durationSeconds)
        + "s); it will be exported when the recording finishes.");
    return true;
}

void ClipExportService::OnRecordingStopped(
    const std::filesystem::path& videoPath,
    std::chrono::system_clock::time_point recordingStartedAt,
    std::chrono::steady_clock::time_point recordingStartedAtSteady,
    std::optional<std::chrono::system_clock::time_point> logicalEndAt)
{
    if (logicalEndAt.has_value()
        && recordingStartedAt + kMinCheapTrimSeconds < *logicalEndAt
        && *logicalEndAt < std::chrono::system_clock::now()) {
        TrimJob job;
        job.videoPath = videoPath;
        job.recordingStartedAt = recordingStartedAt;
        job.trimEndAt = *logicalEndAt;
        EnqueueTrimJob(job);
    }

    std::vector<ClipRequest> pending;
    {
        std::scoped_lock lock(pendingMutex_);
        pending = std::move(pendingClipRequests_);
        pendingClipRequests_.clear();
    }
    const auto now = std::chrono::system_clock::now();
    for (const auto& request : pending) {
        const auto requestedAt = (std::min)(request.requestedAt, now);
        const auto requestedElapsed = request.requestedAtSteady - recordingStartedAtSteady;
        const auto clipStartAt = requestedElapsed < std::chrono::seconds(request.durationSeconds)
            ? recordingStartedAt
            : (std::max)(recordingStartedAt, requestedAt - std::chrono::seconds(request.durationSeconds));
        TrimJob job;
        job.videoPath = videoPath;
        job.outputPath = request.outputPath;
        job.recordingStartedAt = recordingStartedAt;
        job.clipStartAt = clipStartAt;
        job.trimEndAt = requestedAt;
        job.clipRequestId = request.id;
        job.isClip = true;
        EnqueueTrimJob(job);
    }
}

void ClipExportService::Stop()
{
    {
        std::scoped_lock lock(trimQueueMutex_);
        trimWorkerStopRequested_ = true;
    }
    trimQueueCv_.notify_all();
    if (trimWorker_.joinable()) {
        trimWorker_.join();
    }
}

void ClipExportService::EnsureTrimWorkerRunning()
{
    if (trimWorker_.joinable()) {
        return;
    }
    trimWorkerStopRequested_ = false;
    trimWorker_ = std::thread([this]() { TrimWorkerLoop(); });
}

void ClipExportService::EnqueueTrimJob(const TrimJob& job)
{
    const auto trimDuration = std::chrono::duration_cast<std::chrono::seconds>(job.trimEndAt - job.recordingStartedAt);
    {
        std::scoped_lock lock(trimQueueMutex_);
        trimQueue_.push_back(job);
        EnsureTrimWorkerRunning();
    }
    PushStatus("Trim queued for '" + job.videoPath.filename().string() +
        "': logical end " + FormatWallClock(job.trimEndAt) +
        ", keep duration " + FormatDurationClock(trimDuration) + ".");
    trimQueueCv_.notify_one();
}

std::filesystem::path ClipExportService::ClipJournalPath() const
{
    return journalDirectory_ / "bean-clip-journal.log";
}

bool ClipExportService::AppendClipJournalRequest(
    const std::filesystem::path& videoPath,
    const ClipRequest& request,
    std::string& error) const
{
    std::scoped_lock journalLock(clipJournalMutex_);
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories(videoPath.parent_path() / "Clips", ec);
    if (ec) {
        error = "Unable to create Clips folder: " + ec.message();
        return false;
    }
    ec.clear();
    std::filesystem::create_directories(ClipJournalPath().parent_path(), ec);
    if (ec) {
        error = "Unable to create clip journal folder: " + ec.message();
        return false;
    }
    std::ofstream stream(ClipJournalPath(), std::ios::app);
    if (!stream.is_open()) {
        error = "Unable to open clip journal.";
        return false;
    }
    stream << "REQ " << request.id
           << ' ' << ToEpochMilliseconds(request.recordingStartedAt)
           << ' ' << ToEpochMilliseconds(request.requestedAt)
           << ' ' << request.durationSeconds
           << ' ' << request.clipIndex
           << ' ' << std::quoted(videoPath.string())
           << ' ' << std::quoted(request.outputPath.string()) << '\n';
    stream.flush();
    if (!stream.good()) {
        error = "Failed while writing clip journal.";
        return false;
    }
    return true;
}

bool ClipExportService::AppendClipJournalFailure(
    std::uint64_t requestId,
    const std::string& error) const
{
    std::scoped_lock journalLock(clipJournalMutex_);
    std::ofstream stream(ClipJournalPath(), std::ios::app);
    if (!stream.is_open()) {
        return false;
    }
    stream << "FAIL " << requestId;
    if (!error.empty()) {
        stream << ' ' << std::quoted(error);
    }
    stream << '\n';
    stream.flush();
    return stream.good();
}

bool ClipExportService::RemoveClipJournalRequest(std::uint64_t requestId) const
{
    std::scoped_lock journalLock(clipJournalMutex_);
    const auto journalPath = ClipJournalPath();
    std::ifstream input(journalPath);
    if (!input.is_open()) {
        return true;
    }

    const auto temporaryPath = journalPath.parent_path() / (journalPath.filename().string() + ".tmp");
    std::ofstream output(temporaryPath, std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(input, line)) {
        std::istringstream lineStream(line);
        std::string type;
        std::uint64_t lineRequestId = 0;
        lineStream >> type >> lineRequestId;
        if ((type == "REQ" || type == "FAIL" || type == "DONE") && lineRequestId == requestId) {
            continue;
        }
        output << line << '\n';
    }
    output.flush();
    if (!output.good()) {
        std::filesystem::remove(temporaryPath);
        return false;
    }
    output.close();
    input.close();

    std::error_code ec;
    const auto backupPath = journalPath.parent_path() / (journalPath.filename().string() + ".bak");
    std::filesystem::remove(backupPath, ec);
    ec.clear();
    std::filesystem::rename(journalPath, backupPath, ec);
    if (ec) {
        std::filesystem::remove(temporaryPath);
        return false;
    }
    ec.clear();
    std::filesystem::rename(temporaryPath, journalPath, ec);
    if (ec) {
        std::error_code restoreEc;
        std::filesystem::rename(backupPath, journalPath, restoreEc);
        std::filesystem::remove(temporaryPath);
        return false;
    }
    std::filesystem::remove(backupPath, ec);
    return true;
}

std::filesystem::path ClipExportService::BuildClipPath(
    const std::filesystem::path& videoPath,
    const ClipRequest& request) const
{
    std::ostringstream name;
    name << videoPath.stem().string()
         << "-clip";
    if (request.clipIndex > 1) {
        name << "-" << request.clipIndex;
    }
    name << videoPath.extension().string();
    return videoPath.parent_path() / "Clips" / name.str();
}

void ClipExportService::RecoverPendingWork()
{
    std::ifstream stream(ClipJournalPath());
    if (!stream.is_open()) {
        return;
    }

    std::unordered_set<std::uint64_t> completed;
    std::unordered_map<std::uint64_t, int> failureCounts;
    std::vector<ClipRequest> requests;
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream input(line);
        std::string type;
        input >> type;
        if (type == "DONE") {
            std::uint64_t id = 0;
            if (input >> id) {
                completed.insert(id);
            }
            continue;
        }
        if (type == "FAIL") {
            std::uint64_t id = 0;
            if (input >> id) {
                ++failureCounts[id];
            }
            continue;
        }
        if (type != "REQ") {
            continue;
        }
        ClipRequest request;
        std::int64_t recordingStartMs = 0;
        std::int64_t requestedMs = 0;
        std::string pathText;
        if (!(input >> request.id >> recordingStartMs >> requestedMs >> request.durationSeconds)) {
            continue;
        }
        input >> std::ws;
        if (input.peek() == '"') {
            // Compatibility with journals written before clip numbering was added.
            if (!(input >> std::quoted(pathText))) {
                continue;
            }
        } else if (!(input >> request.clipIndex >> std::quoted(pathText))) {
            continue;
        }
        request.recordingStartedAt = FromEpochMilliseconds(recordingStartMs);
        request.requestedAt = FromEpochMilliseconds(requestedMs);
        request.videoPath = pathText;
        request.durationSeconds = (std::clamp)(request.durationSeconds, kMinClipDurationSeconds, kMaxClipDurationSeconds);
        std::string outputText;
        if (input >> std::quoted(outputText)) {
            request.outputPath = outputText;
        }
        requests.push_back(std::move(request));
    }

    for (const auto& request : requests) {
        if (completed.contains(request.id)) {
            RemoveClipJournalRequest(request.id);
            continue;
        }
        const int failureCount = failureCounts[request.id];
        if (failureCount >= 3) {
            RemoveClipJournalRequest(request.id);
            PushStatus("Giving up on clip request " + std::to_string(request.id) + " after three failed attempts.");
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::exists(request.videoPath, ec) || ec) {
            continue;
        }
        const auto outputPath = request.outputPath.empty()
            ? BuildClipPath(request.videoPath, request)
            : request.outputPath;
        if (std::filesystem::exists(outputPath, ec) && !ec) {
            RemoveClipJournalRequest(request.id);
            continue;
        }
        TrimJob job;
        job.videoPath = request.videoPath;
        job.outputPath = outputPath;
        job.recordingStartedAt = request.recordingStartedAt;
        job.clipStartAt = (std::max)(
            request.recordingStartedAt,
            request.requestedAt - std::chrono::seconds(request.durationSeconds));
        job.trimEndAt = request.requestedAt;
        job.clipRequestId = request.id;
        job.journalFailureCount = failureCount;
        job.isClip = true;
        EnqueueTrimJob(job);
        PushStatus("Recovered pending clip request " + std::to_string(request.id) + " from the clip journal.");
    }
}

void ClipExportService::TrimWorkerLoop()
{
    for (;;) {
        TrimJob job;
        {
            std::unique_lock lock(trimQueueMutex_);
            trimQueueCv_.wait(lock, [this]() { return trimWorkerStopRequested_ || !trimQueue_.empty(); });
            if (trimWorkerStopRequested_ && trimQueue_.empty()) {
                return;
            }
            job = trimQueue_.front();
            trimQueue_.pop_front();
        }
        const auto trimStart = job.isClip ? job.clipStartAt : job.recordingStartedAt;
        const auto trimDuration = std::chrono::duration_cast<std::chrono::seconds>(job.trimEndAt - trimStart);
        PushStatus((job.isClip ? "Clip export started for '" : "Trim started for '") + job.videoPath.filename().string() +
            "' -> logical end " + FormatWallClock(job.trimEndAt) +
            " (keep " + FormatDurationClock(trimDuration) + ").");
        std::string trimError;
        bool trimSucceeded = false;
        for (int attempt = 0; attempt < (job.isClip ? 10 : 1); ++attempt) {
            if (RunCheapTrim(job, trimError)) {
                trimSucceeded = true;
                break;
            }
            if (job.isClip && attempt < 9) {
                Sleep(500);
            }
        }
        if (trimSucceeded) {
            PushStatus((job.isClip ? "Clip export finished for '" : "Trim finished for '") + job.videoPath.filename().string() +
                "' (kept " + FormatDurationClock(trimDuration) + ").");
            if (job.isClip) {
                if (!RemoveClipJournalRequest(job.clipRequestId)) {
                    PushStatus("Warning: could not remove completed clip request "
                        + std::to_string(job.clipRequestId) + " from the journal.");
                }
                PushStatus("Clip created: '" + job.outputPath.filename().string() + "'.");
            }
        } else {
            PushStatus("Trim failed for '" + job.videoPath.filename().string() + "': " + trimError);
            if (job.isClip) {
                if (job.journalFailureCount >= 2) {
                    if (RemoveClipJournalRequest(job.clipRequestId)) {
                        PushStatus("Giving up on clip request " + std::to_string(job.clipRequestId)
                            + " after three failed attempts.");
                    } else {
                        PushStatus("Warning: could not remove abandoned clip request "
                            + std::to_string(job.clipRequestId) + " from the journal.");
                    }
                } else if (!AppendClipJournalFailure(job.clipRequestId, trimError)) {
                    PushStatus("Warning: could not record clip failure for request "
                        + std::to_string(job.clipRequestId) + ".");
                } else {
                    PushStatus("Clip export failed for request " + std::to_string(job.clipRequestId)
                        + "; it will be retried on the next launch.");
                }
            }
        }
    }
}

bool ClipExportService::RunCheapTrim(const TrimJob& job, std::string& error) const
{
    error.clear();
    std::error_code ec;
    if (!std::filesystem::exists(job.videoPath, ec) || ec) {
        error = "Source video missing.";
        return false;
    }

    const auto trimStart = job.isClip ? job.clipStartAt : job.recordingStartedAt;
    const auto durationSeconds = std::chrono::duration_cast<std::chrono::seconds>(job.trimEndAt - trimStart);
    if (durationSeconds < (job.isClip ? std::chrono::seconds(kMinClipDurationSeconds) : kMinCheapTrimSeconds)) {
        error = "Trim duration too short.";
        return false;
    }

    const auto ffmpegCandidates = ResolveFfmpegExecutableCandidates();
    if (ffmpegCandidates.empty()) {
        error = "Bundled ffmpeg.exe is missing beside the Bean executable.";
        return false;
    }
    std::vector<std::string> candidateErrors;
    candidateErrors.reserve(ffmpegCandidates.size());
    for (const auto& ffmpegPath : ffmpegCandidates) {
        const auto outputPath = job.isClip ? job.outputPath : job.videoPath;
        const auto tempPath = outputPath.parent_path() /
            (outputPath.stem().string() + (job.isClip ? ".cliptmp" : ".trimtmp") + outputPath.extension().string());
        const auto backupPath = job.videoPath.parent_path() /
            (job.videoPath.stem().string() + ".pretrim" + job.videoPath.extension().string());

        std::error_code cleanupEc;
        std::filesystem::remove(tempPath, cleanupEc);
        cleanupEc.clear();
        std::filesystem::remove(backupPath, cleanupEc);

        double exportStartOffsetSeconds = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                trimStart - job.recordingStartedAt).count()) / 1000.0;
        double exportDurationSeconds = static_cast<double>(durationSeconds.count());
        if (job.isClip) {
            std::vector<double> keyframes;
            std::string probeError;
            if (ProbeKeyframeTimes(ffmpegPath, job.videoPath, keyframes, probeError)) {
                const double requestedEndOffsetSeconds = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        job.trimEndAt - job.recordingStartedAt).count()) / 1000.0;
                const auto endAtOrAfter = std::lower_bound(
                    keyframes.begin(), keyframes.end(), requestedEndOffsetSeconds);
                const double selectedEndOffsetSeconds = endAtOrAfter != keyframes.end()
                    ? *endAtOrAfter
                    : keyframes.back();
                const double targetStartOffsetSeconds =
                    selectedEndOffsetSeconds - exportDurationSeconds;
                const auto startAfterTarget = std::upper_bound(
                    keyframes.begin(), keyframes.end(), targetStartOffsetSeconds);
                const double selectedStartOffsetSeconds = startAfterTarget == keyframes.begin()
                    ? keyframes.front()
                    : *std::prev(startAfterTarget);
                if (selectedStartOffsetSeconds < selectedEndOffsetSeconds) {
                    exportStartOffsetSeconds = selectedStartOffsetSeconds;
                    exportDurationSeconds = selectedEndOffsetSeconds - selectedStartOffsetSeconds;
                }
            }
        }

        std::wstring commandLine = QuoteArg(ffmpegPath);
        if (job.isClip) {
            commandLine += L" -ss " + std::to_wstring(exportStartOffsetSeconds);
        }
        commandLine += L" -y -v error -i " + QuoteArg(job.videoPath)
            + L" -t " + std::to_wstring(exportDurationSeconds)
            + L" -map 0 -c copy " + QuoteArg(tempPath);

        DWORD exitCode = 1;
        if (!RunProcessAndWait(commandLine, exitCode)) {
            candidateErrors.push_back(ffmpegPath.string() + ": failed to launch process");
            continue;
        }
        if (exitCode != 0) {
            std::string candidateError = ffmpegPath.string() + ": exited with " + FormatWinExitCode(exitCode);
            if (exitCode == 3221225781u) { // STATUS_DLL_NOT_FOUND
                candidateError += " (missing runtime DLL dependency)";
            }
            candidateErrors.push_back(std::move(candidateError));
            continue;
        }
        if (!std::filesystem::exists(tempPath, ec) || ec) {
            candidateErrors.push_back(ffmpegPath.string() + ": did not produce output file");
            continue;
        }

        if (job.isClip) {
            std::filesystem::remove(outputPath, cleanupEc);
            std::error_code renameEc;
            std::filesystem::rename(tempPath, outputPath, renameEc);
            if (renameEc) {
                candidateErrors.push_back(ffmpegPath.string() + ": unable to create clip output");
                continue;
            }
            return true;
        }

        std::error_code renameEc;
        std::filesystem::rename(job.videoPath, backupPath, renameEc);
        if (renameEc) {
            std::filesystem::remove(tempPath, cleanupEc);
            candidateErrors.push_back(ffmpegPath.string() + ": unable to stage original video for replacement");
            continue;
        }

        renameEc.clear();
        std::filesystem::rename(tempPath, job.videoPath, renameEc);
        if (renameEc) {
            std::error_code restoreEc;
            std::filesystem::rename(backupPath, job.videoPath, restoreEc);
            std::filesystem::remove(tempPath, cleanupEc);
            candidateErrors.push_back(ffmpegPath.string() + ": unable to replace original video with trimmed output");
            continue;
        }

        std::filesystem::remove(backupPath, cleanupEc);
        return true;
    }

    std::ostringstream os;
    os << "All ffmpeg candidates failed";
    if (!candidateErrors.empty()) {
        os << ": ";
        for (size_t i = 0; i < candidateErrors.size(); ++i) {
            if (i > 0) {
                os << " | ";
            }
            os << candidateErrors[i];
        }
    }
    error = os.str();
    return false;
}


} // namespace bean::core
