#include "core/RecordingOrchestrator.h"

#include "core/WowData.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace bean::core {
namespace {

constexpr auto kPostStartIdleTimeoutSuppress = std::chrono::seconds(180);
constexpr auto kCombatLogIdleTimeout = std::chrono::seconds(720);
constexpr auto kMinCheapTrimSeconds = std::chrono::seconds(5);
constexpr int kMinClipDurationSeconds = 1;
constexpr int kMaxClipDurationSeconds = 3600;

std::string TimestampNow()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &time);

    std::ostringstream os;
    os << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return os.str();
}

std::string FormatWallClock(const std::chrono::system_clock::time_point& value)
{
    const auto time = std::chrono::system_clock::to_time_t(value);
    std::tm tm{};
    localtime_s(&tm, &time);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return os.str();
}

std::string FormatDurationForStatus(int seconds)
{
    if (seconds > 0 && seconds % 60 == 0) {
        const int minutes = seconds / 60;
        return std::to_string(minutes) + " minute" + (minutes == 1 ? "" : "s");
    }
    return std::to_string(seconds) + "s";
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

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    bool gotCode = (GetExitCodeProcess(processInfo.hProcess, &exitCode) != 0);
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

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    const bool gotCode = GetExitCodeProcess(processInfo.hProcess, &exitCode) != 0;
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

std::string BuildWatcherDebugStatus(const log::CombatLogWatcher::DebugSnapshot& snapshot)
{
    std::ostringstream os;
    os << "watcher file='";
    if (snapshot.activeFile.empty()) {
        os << "<none>";
    } else {
        os << snapshot.activeFile.string();
    }
    os << "' offset=" << snapshot.lastPosition;
    os << " streamOpen=" << (snapshot.streamOpen ? "yes" : "no");
    os << " staleSeekRecoveries=" << snapshot.staleSeekRecoveries;
    if (snapshot.lastLineAt.has_value()) {
        os << " lastLineAt=" << FormatWallClock(*snapshot.lastLineAt);
    } else {
        os << " lastLineAt=<none>";
    }
    return os.str();
}

std::string BuildFileToken(std::string value, const std::string& fallback)
{
    std::string token;
    token.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) != 0) {
            token.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return token.empty() ? fallback : token;
}

const char* BoolLabel(bool value)
{
    return value ? "yes" : "no";
}

const char* ToString(RecordingStartReason reason)
{
    switch (reason) {
    case RecordingStartReason::Manual:
        return "manual";
    case RecordingStartReason::MythicStart:
        return "mythic-start";
    default:
        return "manual";
    }
}

const char* ToString(RecordingStopReason reason)
{
    switch (reason) {
    case RecordingStopReason::Manual:
        return "manual";
    case RecordingStopReason::Shutdown:
        return "shutdown";
    case RecordingStopReason::CombatLogIdleTimeout:
        return "combat-log-idle-timeout";
    case RecordingStopReason::MythicSuccess:
        return "mythic-success";
    case RecordingStopReason::MythicFailure:
        return "mythic-failure";
    case RecordingStopReason::MythicRestart:
        return "mythic-restart";
    default:
        return "manual";
    }
}

} // namespace

RecordingOrchestrator::RecordingOrchestrator(std::unique_ptr<obs::IRecorderEngine> engine)
    : engine_(std::move(engine))
{
}

RecordingOrchestrator::~RecordingOrchestrator()
{
    StopMonitoring();
    std::string error;
    StopRecordingInternal(RecordingStopReason::Shutdown, error);
    StopTrimWorker();
}

void RecordingOrchestrator::SetStatusCallback(StatusCallback callback)
{
    std::scoped_lock lock(statusCallbackMutex_);
    statusCallback_ = std::move(callback);
}

void RecordingOrchestrator::SetRunRepository(std::shared_ptr<RunRepository> repository)
{
    std::scoped_lock lock(mutex_);
    runRepository_ = std::move(repository);
}

void RecordingOrchestrator::ApplySettings(const AppSettings& settings)
{
    std::scoped_lock lock(mutex_);
    const bool updateMicNoiseSuppressionLive =
        engine_->IsRecording()
        && settings_.captureMicrophone
        && settings.captureMicrophone
        && settings_.microphoneNoiseSuppression != settings.microphoneNoiseSuppression;
    settings_ = settings;
    if (updateMicNoiseSuppressionLive) {
        std::string error;
        if (!engine_->SetMicrophoneNoiseSuppressionEnabled(settings_.microphoneNoiseSuppression, error)) {
            PushStatus("Live microphone noise suppression update failed: " + error);
        } else {
            PushStatus(
                std::string("Live microphone noise suppression ")
                + (settings_.microphoneNoiseSuppression ? "enabled." : "disabled."));
        }
    }
}

bool RecordingOrchestrator::StartMonitoring(std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();

    settings_.outputDirectory = settings_.outputDirectory.empty()
        ? std::filesystem::temp_directory_path() / "Battle Encounter Archival Nexus Recordings"
        : settings_.outputDirectory;

    watcher_.SetLogDirectory(settings_.wowLogDirectory);
    PushStatus(
        "Monitoring start requested: output='" + settings_.outputDirectory.string()
        + "', wow-log='" + settings_.wowLogDirectory.string()
        + "', encoder=" + settings_.videoEncoder
        + ", quality=" + settings_.encoderPreset
        + ", container=" + settings_.videoContainer
        + ", recording-resolution-height=" + std::to_string(settings_.recordingResolutionHeight)
        + "@" + std::to_string(settings_.fps)
        + ", audio-scope=" + AudioCaptureScopeLabel(settings_.audioCaptureScope)
        + ", microphone=" + BoolLabel(settings_.captureMicrophone)
        + ", microphone-noise-suppression=" + BoolLabel(settings_.microphoneNoiseSuppression));

    auto recordingConfig = ToRecordingConfig(settings_);
    if (!engine_->Initialize(recordingConfig, error)) {
        PushStatus("OBS initialize failed: " + error);
        return false;
    }

    RecoverClipJournal();

    // Start the consumer before the producer so no line is dropped.
    StartCombatLogWorker();
    if (!watcher_.Start([this](const std::string& line) { HandleCombatLogLine(line); }, error)) {
        PushStatus("Monitoring start failed: " + error);
        return false;
    }
    lastObservedWatcherSeekRecoveries_ = watcher_.GetDebugSnapshot().staleSeekRecoveries;

    state_ = engine_->IsRecording() ? OrchestratorState::Recording : OrchestratorState::Armed;
    PushStatus("Monitoring enabled.");
    return true;
}

void RecordingOrchestrator::StopMonitoring()
{
    // Shut the producer and consumer down *before* taking mutex_. Both threads
    // can be mid-callback needing that lock, and joining them while holding it
    // would deadlock.
    watcher_.Stop();
    StopCombatLogWorker();

    std::scoped_lock lock(mutex_);
    ResetMythicTrackingState();
    if (state_ == OrchestratorState::Armed) {
        state_ = OrchestratorState::Idle;
    }
    PushStatus("Monitoring disabled.");
}

void RecordingOrchestrator::ResetMythicTrackingState()
{
    mythicRunStartedAt_.reset();
    postRunStopAt_.reset();
    postRunStopReason_.reset();
    lastCombatLogLineAt_.reset();
    lastCombatLogLineAtWallClock_.reset();
    lastCombatLogFileWriteAt_.reset();
    lastCombatLogFileWriteAtWallClock_.reset();
    observedCombatLogFile_.reset();
    observedCombatLogWriteTime_.reset();
    lastObservedWatcherSeekRecoveries_ = 0;
    detector_ = log::MythicRunDetector{};
}

bool RecordingOrchestrator::StartManualRecording(std::string& error)
{
    std::unique_lock lock(mutex_);
    ResetMythicTrackingState();
    return StartRecordingInternal(RecordingStartReason::Manual, error, lock);
}

bool RecordingOrchestrator::StopManualRecording(std::string& error)
{
    std::scoped_lock lock(mutex_);
    const bool stopped = StopRecordingInternal(RecordingStopReason::Manual, error);
    ResetMythicTrackingState();
    return stopped;
}

bool RecordingOrchestrator::RequestClip(std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();
    if (!engine_->IsRecording() || !activeRecordingMetadata_.has_value()) {
        error = "A clip can only be requested while recording.";
        return false;
    }

    ClipRequest request;
    request.videoPath = activeRecordingMetadata_->videoPath;
    request.recordingStartedAt = activeRecordingMetadata_->recordingStartedAt;
    request.recordingStartedAtSteady = activeRecordingMetadata_->recordingStartedAtSteady;
    request.requestedAt = std::chrono::system_clock::now();
    request.requestedAtSteady = std::chrono::steady_clock::now();
    request.id = (static_cast<std::uint64_t>(ToEpochMilliseconds(request.requestedAt)) << 10)
        ^ (nextClipRequestId_++ & 0x3FFu);
    request.durationSeconds = (std::clamp)(settings_.clipDurationSeconds, kMinClipDurationSeconds, kMaxClipDurationSeconds);
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

void RecordingOrchestrator::Tick()
{
    // Called from the UI timer, so it must never block. StartRecordingInternal
    // releases mutex_ while OBS warms up, so try_lock usually succeeds; the
    // recordingStartInProgress_ flag covers that window instead.
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }
    if (recordingStartInProgress_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto nowWallClock = std::chrono::system_clock::now();

    const auto watcherDebug = watcher_.GetDebugSnapshot();
    if (!watcherDebug.activeFile.empty()) {
        std::error_code writeTimeEc;
        const auto currentWriteTime = std::filesystem::last_write_time(watcherDebug.activeFile, writeTimeEc);
        if (!writeTimeEc) {
            const bool hadObservedFile = observedCombatLogFile_.has_value();
            const bool switchedFile = !hadObservedFile || (*observedCombatLogFile_ != watcherDebug.activeFile);
            if (switchedFile) {
                const char* attachMode = hadObservedFile ? "from beginning (file switch)" : "from end (initial attach)";
                PushStatus(
                    "Combat log watcher reading file: " + watcherDebug.activeFile.filename().string()
                    + " [" + attachMode + "].");
            }
            const bool writeTimeAdvanced =
                switchedFile ||
                !observedCombatLogWriteTime_.has_value() ||
                currentWriteTime > *observedCombatLogWriteTime_;
            observedCombatLogFile_ = watcherDebug.activeFile;
            if (writeTimeAdvanced) {
                observedCombatLogWriteTime_ = currentWriteTime;
                lastCombatLogFileWriteAt_ = now;
                lastCombatLogFileWriteAtWallClock_ = nowWallClock;
            }
        }
    }
    if (watcherDebug.staleSeekRecoveries > lastObservedWatcherSeekRecoveries_) {
        const auto recoveredCount = watcherDebug.staleSeekRecoveries - lastObservedWatcherSeekRecoveries_;
        lastObservedWatcherSeekRecoveries_ = watcherDebug.staleSeekRecoveries;
        PushStatus("DIAG: watcher recovered stale combat-log seek " + std::to_string(recoveredCount) +
            " time(s); total=" + std::to_string(watcherDebug.staleSeekRecoveries) + " (" +
            BuildWatcherDebugStatus(watcherDebug) + ").");
    }
    const bool watcherHealthy = watcher_.IsRunning() && watcherDebug.streamOpen && !watcherDebug.activeFile.empty();

    if (engine_->IsRecording() &&
        mythicRunStartedAt_.has_value() &&
        watcherHealthy &&
        (lastCombatLogLineAt_.has_value() || lastCombatLogFileWriteAt_.has_value())) {
        const auto sinceRunStart = now - *mythicRunStartedAt_;
        if (sinceRunStart >= kPostStartIdleTimeoutSuppress) {
            const auto lastActivityAt = [&]() -> std::optional<std::chrono::steady_clock::time_point> {
                if (lastCombatLogLineAt_.has_value() && lastCombatLogFileWriteAt_.has_value()) {
                    return (*lastCombatLogLineAt_ > *lastCombatLogFileWriteAt_) ? lastCombatLogLineAt_ : lastCombatLogFileWriteAt_;
                }
                if (lastCombatLogLineAt_.has_value()) {
                    return lastCombatLogLineAt_;
                }
                return lastCombatLogFileWriteAt_;
            }();
            if (!lastActivityAt.has_value()) {
                return;
            }
            const auto idleElapsed = now - *lastActivityAt;
            if (idleElapsed >= kCombatLogIdleTimeout) {
                std::optional<std::chrono::system_clock::time_point> logicalEndAt;
                if (lastCombatLogLineAtWallClock_.has_value() && lastCombatLogFileWriteAtWallClock_.has_value()) {
                    logicalEndAt = (*lastCombatLogLineAtWallClock_ > *lastCombatLogFileWriteAtWallClock_)
                        ? lastCombatLogLineAtWallClock_
                        : lastCombatLogFileWriteAtWallClock_;
                } else if (lastCombatLogLineAtWallClock_.has_value()) {
                    logicalEndAt = lastCombatLogLineAtWallClock_;
                } else {
                    logicalEndAt = lastCombatLogFileWriteAtWallClock_;
                }
                std::string error;
                if (!StopRecordingInternal(RecordingStopReason::CombatLogIdleTimeout, error, logicalEndAt)) {
                    PushStatus("Idle combat-log timeout stop failed: " + error);
                } else {
                    std::ostringstream status;
                    status << "No new combat-log lines for "
                           << FormatDurationForStatus(static_cast<int>(kCombatLogIdleTimeout.count()))
                           << " (" << kCombatLogIdleTimeout.count() << "s). Recording stopped";
                    status << " (" << BuildWatcherDebugStatus(watcherDebug) << ").";
                    PushStatus(status.str());
                }
                return;
            }
        }
    }

    if (postRunStopAt_.has_value()) {
        if (!engine_->IsRecording()) {
            postRunStopAt_.reset();
            postRunStopReason_.reset();
        } else if (now >= *postRunStopAt_) {
            std::string error;
            const RecordingStopReason reason = postRunStopReason_.value_or(RecordingStopReason::MythicSuccess);
            if (!StopRecordingInternal(reason, error)) {
                PushStatus("Delayed run-end stop failed: " + error);
            }
            postRunStopAt_.reset();
            postRunStopReason_.reset();
            return;
        }
    }

}

OrchestratorState RecordingOrchestrator::GetState() const
{
    std::scoped_lock lock(mutex_);
    return state_;
}

std::uint64_t RecordingOrchestrator::GetRecordingSessionId() const
{
    std::scoped_lock lock(mutex_);
    return recordingSessionId_;
}

bool RecordingOrchestrator::IsMonitoring() const
{
    return watcher_.IsRunning();
}

void RecordingOrchestrator::HandleCombatLogLine(const std::string& line)
{
    // Runs on the watcher thread. Hand the line off and return immediately:
    // processing it can start a recording, which takes seconds, and stalling
    // the watcher means log lines queue up behind it unread.
    {
        std::scoped_lock queueLock(combatLogQueueMutex_);
        if (combatLogWorkerStopRequested_) {
            return;
        }
        combatLogQueue_.push_back(line);
    }
    combatLogQueueCv_.notify_one();
}

void RecordingOrchestrator::CombatLogWorkerLoop()
{
    for (;;) {
        std::string line;
        {
            std::unique_lock queueLock(combatLogQueueMutex_);
            combatLogQueueCv_.wait(queueLock, [this]() {
                return combatLogWorkerStopRequested_ || !combatLogQueue_.empty();
            });
            if (combatLogWorkerStopRequested_) {
                return;
            }
            line = std::move(combatLogQueue_.front());
            combatLogQueue_.pop_front();
        }
        ProcessCombatLogLine(line);
    }
}

void RecordingOrchestrator::StartCombatLogWorker()
{
    std::scoped_lock queueLock(combatLogQueueMutex_);
    combatLogWorkerStopRequested_ = false;
    combatLogQueue_.clear();
    if (!combatLogWorker_.joinable()) {
        combatLogWorker_ = std::thread([this]() { CombatLogWorkerLoop(); });
    }
}

void RecordingOrchestrator::StopCombatLogWorker()
{
    // Must not be called while holding mutex_: the worker takes mutex_ inside
    // ProcessCombatLogLine, so joining it under that lock would deadlock.
    {
        std::scoped_lock queueLock(combatLogQueueMutex_);
        combatLogWorkerStopRequested_ = true;
        combatLogQueue_.clear();
    }
    combatLogQueueCv_.notify_all();
    if (combatLogWorker_.joinable()) {
        combatLogWorker_.join();
    }
}

void RecordingOrchestrator::ProcessCombatLogLine(const std::string& line)
{
    std::unique_lock lock(mutex_);
    lastCombatLogLineAt_ = std::chrono::steady_clock::now();
    lastCombatLogLineAtWallClock_ = std::chrono::system_clock::now();
    const auto participantsBeforeProcessingLine = detector_.GetParticipants();
    const auto event = detector_.ProcessLine(line);
    if (activeRecordingMetadata_.has_value()) {
        // A CHALLENGE_MODE_START can trigger a mythic-restart stop for the previous
        // recording. Preserve the participant snapshot from before processing this
        // line so we do not persist an empty roster for the previous run.
        if (event.has_value() && event->type == log::MythicEventType::RunStarted) {
            activeRecordingMetadata_->participants = participantsBeforeProcessingLine;
        } else {
            activeRecordingMetadata_->participants = detector_.GetParticipants();
        }
    }
    if (!event.has_value()) {
        return;
    }

    std::string error;
    switch (event->type) {
    case log::MythicEventType::RunStarted:
        mythicRunStartedAt_ = std::chrono::steady_clock::now();
        lastChallengeMapId_ = event->challengeMapId;
        lastKeystoneLevel_ = event->keystoneLevel;
        postRunStopAt_.reset();
        postRunStopReason_.reset();
        if (engine_->IsRecording()) {
            std::string stopError;
            if (!StopRecordingInternal(RecordingStopReason::MythicRestart, stopError)) {
                PushStatus("Mythic restart stop failed: " + stopError);
                break;
            }
        }
        if (!StartRecordingInternal(RecordingStartReason::MythicStart, error, lock)) {
            // This is the moment the whole app exists for. A failure here is
            // otherwise invisible until the player finds no video afterwards.
            PushStatus("AUTO-RECORD FAILED for detected mythic start: "
                + (error.empty() ? std::string("unknown error") : error));
            break;
        }
        if (activeRecordingMetadata_.has_value() && event->mapName.has_value() && !event->mapName->empty()) {
            activeRecordingMetadata_->observedDungeonName = *event->mapName;
        }
        break;
    case log::MythicEventType::RunEndedSuccess:
        mythicRunStartedAt_.reset();
        if (event->challengeMapId.has_value()) {
            lastChallengeMapId_ = event->challengeMapId;
            if (activeRecordingMetadata_.has_value()) {
                activeRecordingMetadata_->challengeMapId = event->challengeMapId;
            }
        }
        if (event->keystoneLevel.has_value()) {
            lastKeystoneLevel_ = event->keystoneLevel;
            if (activeRecordingMetadata_.has_value()) {
                activeRecordingMetadata_->keystoneLevel = event->keystoneLevel;
            }
        }
        if (activeRecordingMetadata_.has_value() && !activeRecordingMetadata_->mythicRunEndedAt.has_value()) {
            activeRecordingMetadata_->mythicRunEndedAt = std::chrono::system_clock::now();
        }
        if (engine_->IsRecording()) {
            const int delaySeconds = (std::max)(0, settings_.postRunStopDelaySeconds);
            postRunStopAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(delaySeconds);
            postRunStopReason_ = RecordingStopReason::MythicSuccess;
            PushStatus("Challenge ended. Recording will stop in " + std::to_string(delaySeconds) + "s to capture post-run context.");
        } else {
            postRunStopAt_.reset();
            postRunStopReason_.reset();
        }
        break;
    case log::MythicEventType::RunEndedFailure:
        mythicRunStartedAt_.reset();
        if (event->challengeMapId.has_value()) {
            lastChallengeMapId_ = event->challengeMapId;
            if (activeRecordingMetadata_.has_value()) {
                activeRecordingMetadata_->challengeMapId = event->challengeMapId;
            }
        }
        if (event->keystoneLevel.has_value()) {
            lastKeystoneLevel_ = event->keystoneLevel;
            if (activeRecordingMetadata_.has_value()) {
                activeRecordingMetadata_->keystoneLevel = event->keystoneLevel;
            }
        }
        if (activeRecordingMetadata_.has_value() && !activeRecordingMetadata_->mythicRunEndedAt.has_value()) {
            activeRecordingMetadata_->mythicRunEndedAt = std::chrono::system_clock::now();
        }
        if (engine_->IsRecording()) {
            const int delaySeconds = (std::max)(0, settings_.postRunStopDelaySeconds);
            postRunStopAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(delaySeconds);
            postRunStopReason_ = RecordingStopReason::MythicFailure;
            PushStatus("Challenge ended. Recording will stop in " + std::to_string(delaySeconds) + "s to capture post-run context.");
        } else {
            postRunStopAt_.reset();
            postRunStopReason_.reset();
        }
        break;
    }
}

bool RecordingOrchestrator::StartRecordingInternal(
    RecordingStartReason reason,
    std::string& error,
    std::unique_lock<std::mutex>& lock)
{
    if (!lock.owns_lock()) {
        error = "Internal error: StartRecordingInternal called without lock.";
        return false;
    }
    if (recordingStartInProgress_ || engine_->IsRecording()) {
        error = "Already recording.";
        return false;
    }
    postRunStopAt_.reset();
    postRunStopReason_.reset();
    const auto now = std::chrono::steady_clock::now();
    const auto nowWallClock = std::chrono::system_clock::now();
    lastCombatLogLineAt_ = now;
    lastCombatLogLineAtWallClock_ = nowWallClock;
    lastCombatLogFileWriteAt_ = now;
    lastCombatLogFileWriteAtWallClock_ = nowWallClock;
    observedCombatLogFile_.reset();
    observedCombatLogWriteTime_.reset();

    // Snapshot everything the unlocked engine calls need. Settings must not be
    // read again after the unlock, because ApplySettings can mutate them.
    auto recordingConfig = ToRecordingConfig(settings_);
    const auto fileStem = BuildFileStem(reason);
    const int selectedHeight = settings_.recordingResolutionHeight;
    const bool useMp4 = settings_.videoContainer == "mp4";
    const auto outputDirectory = settings_.outputDirectory;
    const auto challengeMapId = lastChallengeMapId_;
    const auto keystoneLevel = lastKeystoneLevel_;
    const auto participants = detector_.GetParticipants();
    const bool mythicRunActive = mythicRunStartedAt_.has_value();

    PushStatus(
        "Recording video output: " + std::to_string(recordingConfig.width)
        + "x" + std::to_string(recordingConfig.height)
        + " (selected height=" + std::to_string(selectedHeight) + ")");

    recordingStartInProgress_ = true;
    lock.unlock();

    // Reinitialize before every recording so setting changes made while monitoring
    // is already armed (for example audio capture scope) are applied immediately.
    bool initialized = engine_->Initialize(recordingConfig, error);
    bool started = false;
    if (initialized) {
        started = engine_->StartRecording(fileStem, error);
    }

    lock.lock();
    recordingStartInProgress_ = false;

    if (!initialized) {
        PushStatus("Initialize failed: " + error);
        return false;
    }
    if (!started) {
        PushStatus("Start recording failed: " + error);
        return false;
    }
    if (!engine_->IsRecording()) {
        error = "Recording stopped unexpectedly during start.";
        PushStatus(error);
        return false;
    }

    ++recordingSessionId_;
    state_ = OrchestratorState::Recording;
    ActiveRecordingMetadata metadata;
    metadata.triggerReason = reason;
    const auto extension = useMp4 ? ".mp4" : ".mkv";
    metadata.videoPath = outputDirectory / (fileStem + extension);
    metadata.recordingStartedAt = std::chrono::system_clock::now();
    metadata.recordingStartedAtSteady = std::chrono::steady_clock::now();
    metadata.challengeMapId = challengeMapId;
    metadata.keystoneLevel = keystoneLevel;
    metadata.participants = participants;
    if (mythicRunActive) {
        metadata.mythicRunStartedAt = std::chrono::system_clock::now();
    }
    activeRecordingMetadata_ = std::move(metadata);
    PushStatus("Recording started (" + std::string(ToString(reason)) + ").");
    const std::string startDiagnostics = engine_->GetLastStartDiagnostics();
    if (!startDiagnostics.empty()) {
        PushStatus("Recording audio settings: " + startDiagnostics);
    }
    return true;
}

bool RecordingOrchestrator::StopRecordingInternal(
    RecordingStopReason reason,
    std::string& error,
    std::optional<std::chrono::system_clock::time_point> logicalEndAt)
{
    if (!engine_->IsRecording()) {
        error = "Not recording.";
        return false;
    }

    if (!engine_->StopRecording(error)) {
        PushStatus("Stop recording failed: " + error);
        return false;
    }

    state_ = watcher_.IsRunning() ? OrchestratorState::Armed : OrchestratorState::Idle;
    postRunStopAt_.reset();
    postRunStopReason_.reset();
    lastCombatLogLineAt_.reset();
    lastCombatLogLineAtWallClock_.reset();
    lastCombatLogFileWriteAt_.reset();
    lastCombatLogFileWriteAtWallClock_.reset();
    observedCombatLogFile_.reset();
    observedCombatLogWriteTime_.reset();
    if (activeRecordingMetadata_.has_value() &&
        logicalEndAt.has_value() &&
        activeRecordingMetadata_->recordingStartedAt + kMinCheapTrimSeconds < *logicalEndAt &&
        *logicalEndAt < std::chrono::system_clock::now()) {
        TrimJob job;
        job.videoPath = activeRecordingMetadata_->videoPath;
        job.recordingStartedAt = activeRecordingMetadata_->recordingStartedAt;
        job.trimEndAt = *logicalEndAt;
        EnqueueTrimJob(job);
    }
    if (activeRecordingMetadata_.has_value() && !pendingClipRequests_.empty()) {
        const auto recording = *activeRecordingMetadata_;
        const auto now = std::chrono::system_clock::now();
        for (const auto& request : pendingClipRequests_) {
            const auto requestedAt = (std::min)(request.requestedAt, now);
            const auto requestedElapsed = request.requestedAtSteady - recording.recordingStartedAtSteady;
            const auto clipStartAt = requestedElapsed < std::chrono::seconds(request.durationSeconds)
                ? recording.recordingStartedAt
                : (std::max)(recording.recordingStartedAt, requestedAt - std::chrono::seconds(request.durationSeconds));
            TrimJob job;
            job.videoPath = recording.videoPath;
            job.outputPath = request.outputPath;
            job.recordingStartedAt = recording.recordingStartedAt;
            job.clipStartAt = clipStartAt;
            job.trimEndAt = requestedAt;
            job.clipRequestId = request.id;
            job.isClip = true;
            EnqueueTrimJob(job);
        }
        pendingClipRequests_.clear();
    }
    PersistRunRecord(reason);
    PushStatus("Recording stopped (" + std::string(ToString(reason)) + ").");
    return true;
}

void RecordingOrchestrator::EnsureTrimWorkerRunning()
{
    if (trimWorker_.joinable()) {
        return;
    }
    trimWorkerStopRequested_ = false;
    trimWorker_ = std::thread([this]() { TrimWorkerLoop(); });
}

void RecordingOrchestrator::StopTrimWorker()
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

void RecordingOrchestrator::EnqueueTrimJob(const TrimJob& job)
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

std::filesystem::path RecordingOrchestrator::ClipJournalPath() const
{
    if (runRepository_) {
        return runRepository_->GetDatabasePath().parent_path() / "bean-clip-journal.log";
    }
    return settings_.outputDirectory / "bean-clip-journal.log";
}

bool RecordingOrchestrator::AppendClipJournalRequest(
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

bool RecordingOrchestrator::AppendClipJournalFailure(
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

bool RecordingOrchestrator::RemoveClipJournalRequest(std::uint64_t requestId) const
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

std::filesystem::path RecordingOrchestrator::BuildClipPath(
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

void RecordingOrchestrator::RecoverClipJournal()
{
    if (engine_->IsRecording()) {
        return;
    }
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

void RecordingOrchestrator::TrimWorkerLoop()
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

bool RecordingOrchestrator::RunCheapTrim(const TrimJob& job, std::string& error) const
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

std::string RecordingOrchestrator::BuildFileStem(RecordingStartReason reason) const
{
    const auto timestamp = TimestampNow();
    if (reason == RecordingStartReason::Manual) {
        return timestamp + "-manual";
    }

    std::string dungeonToken = "dungeon";
    if (lastChallengeMapId_.has_value()) {
        const auto dungeonName = DungeonNameForChallengeMap(*lastChallengeMapId_);
        if (!dungeonName.empty()) {
            dungeonToken = BuildFileToken(dungeonName, dungeonToken);
        }
    }

    std::string keystoneToken = "00";
    if (lastKeystoneLevel_.has_value() && *lastKeystoneLevel_ > 0) {
        std::ostringstream os;
        os << std::setfill('0') << std::setw(2) << *lastKeystoneLevel_;
        keystoneToken = os.str();
    }

    return timestamp + "-" + dungeonToken + "-" + keystoneToken;
}

void RecordingOrchestrator::PushStatus(const std::string& status) const
{
    // Copy under the lock and invoke outside it. The trim worker calls this from
    // a background thread, so reading statusCallback_ directly would race with
    // SetStatusCallback, and holding the lock during the callback would let UI
    // code re-enter the orchestrator.
    StatusCallback callback;
    {
        std::scoped_lock lock(statusCallbackMutex_);
        callback = statusCallback_;
    }
    if (callback) {
        callback(status);
    }
}

void RecordingOrchestrator::PersistRunRecord(RecordingStopReason stopReason)
{
    if (!activeRecordingMetadata_.has_value()) {
        return;
    }
    if (!runRepository_) {
        PushStatus("Run repository is unavailable; metadata not persisted.");
        activeRecordingMetadata_.reset();
        return;
    }

    auto& metadata = *activeRecordingMetadata_;
    metadata.recordingEndedAt = std::chrono::system_clock::now();
    if ((stopReason == RecordingStopReason::MythicSuccess || stopReason == RecordingStopReason::MythicFailure)
        && !metadata.mythicRunEndedAt.has_value()) {
        metadata.mythicRunEndedAt = metadata.recordingEndedAt;
    }

    std::string result = "unknown";
    if (stopReason == RecordingStopReason::MythicSuccess) {
        result = "success";
    } else if (stopReason == RecordingStopReason::MythicFailure) {
        result = "failure";
    }

    RunRecord record;
    record.videoPath = metadata.videoPath;
    record.videoFileName = metadata.videoPath.filename().string();
    record.triggerReason = ToString(metadata.triggerReason);
    record.stopReason = ToString(stopReason);
    record.result = result;
    record.recordingStartedAt = metadata.recordingStartedAt;
    record.recordingEndedAt = *metadata.recordingEndedAt;
    record.mythicRunStartedAt = metadata.mythicRunStartedAt;
    record.mythicRunEndedAt = metadata.mythicRunEndedAt;
    record.challengeMapId = metadata.challengeMapId;
    record.keystoneLevel = metadata.keystoneLevel;
    for (const auto& participant : metadata.participants) {
        RunRecord::Participant runParticipant;
        runParticipant.guid = participant.guid;
        runParticipant.name = participant.name;
        runParticipant.realm = participant.realm;
        runParticipant.region = participant.region;
        runParticipant.specId = participant.specId;
        runParticipant.specName = participant.specName;
        runParticipant.className = participant.className;
        record.participants.push_back(std::move(runParticipant));
    }
    if (metadata.challengeMapId.has_value()) {
        const auto dungeonName = DungeonNameForChallengeMap(*metadata.challengeMapId);
        if (!dungeonName.empty()) {
            record.dungeonName = dungeonName;
        }
    }
    if (!record.dungeonName.has_value() && metadata.observedDungeonName.has_value()) {
        record.dungeonName = *metadata.observedDungeonName;
    }
    if (!record.dungeonName.has_value() && metadata.triggerReason == RecordingStartReason::Manual) {
        record.dungeonName = "Manual Recording";
    }
    std::string dbError;
    if (!runRepository_->UpsertRun(record, dbError)) {
        PushStatus("Failed to persist run metadata: " + dbError);
    }
    activeRecordingMetadata_.reset();
}

} // namespace bean::core
