#include "core/RecordingOrchestrator.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace bean::core {
namespace {

constexpr auto kPostStartIdleTimeoutSuppress = std::chrono::seconds(180);
constexpr auto kCombatLogIdleTimeout = std::chrono::seconds(720);
constexpr auto kMinCheapTrimSeconds = std::chrono::seconds(5);

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

std::wstring ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), needed);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

std::wstring GetEnvWide(const char* name)
{
    char* buffer = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buffer, &len, name) != 0 || buffer == nullptr || len == 0) {
        if (buffer) {
            free(buffer);
        }
        return {};
    }
    std::string value(buffer);
    free(buffer);
    return ToWide(value);
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

std::vector<std::filesystem::path> EnumerateDriveRootsStartingAtC()
{
    std::vector<std::filesystem::path> roots;
    const DWORD logicalDrives = GetLogicalDrives();
    if (logicalDrives == 0) {
        roots.emplace_back(R"(C:\)");
        return roots;
    }

    const auto addDriveIfPresent = [&](wchar_t driveLetter) {
        const DWORD bit = 1u << (driveLetter - L'A');
        if ((logicalDrives & bit) != 0) {
            std::wstring root;
            root.push_back(driveLetter);
            root += L":\\";
            roots.emplace_back(std::move(root));
        }
    };

    for (wchar_t driveLetter = L'C'; driveLetter <= L'Z'; ++driveLetter) {
        addDriveIfPresent(driveLetter);
    }
    for (wchar_t driveLetter = L'A'; driveLetter < L'C'; ++driveLetter) {
        addDriveIfPresent(driveLetter);
    }

    if (roots.empty()) {
        roots.emplace_back(R"(C:\)");
    }
    return roots;
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
    const auto addCandidate = [&](const std::filesystem::path& candidate) {
        if (candidate.empty()) {
            return;
        }
        const auto canonical = candidate.lexically_normal();
        if (std::find(candidates.begin(), candidates.end(), canonical) == candidates.end()) {
            candidates.push_back(canonical);
        }
    };

    if (const auto candidate = GetEnvWide("BEAN_FFMPEG_PATH"); !candidate.empty()) {
        if (const auto resolved = existsExe(std::filesystem::path(candidate)); resolved.has_value()) {
            addCandidate(*resolved);
        }
    }

    if (const auto obsRoot = GetEnvWide("BEAN_OBS_ROOT"); !obsRoot.empty()) {
        const auto candidate = std::filesystem::path(obsRoot) / "bin" / "64bit" / "ffmpeg.exe";
        if (const auto resolved = existsExe(candidate); resolved.has_value()) {
            addCandidate(*resolved);
        }
    }

    for (const auto& driveRoot : EnumerateDriveRootsStartingAtC()) {
        const std::filesystem::path defaultObsRoots[] = {
            driveRoot / "Program Files" / "obs-studio",
            driveRoot / "Program Files (x86)" / "obs-studio"
        };
        for (const auto& root : defaultObsRoots) {
            const auto candidate = root / "bin" / "64bit" / "ffmpeg.exe";
            if (const auto resolved = existsExe(candidate); resolved.has_value()) {
                addCandidate(*resolved);
            }
        }
    }

    for (const auto& driveRoot : EnumerateDriveRootsStartingAtC()) {
        const std::filesystem::path commonFfmpegLocations[] = {
            driveRoot / "ffmpeg" / "bin" / "ffmpeg.exe",
            driveRoot / "Program Files" / "ffmpeg" / "bin" / "ffmpeg.exe",
            driveRoot / "Program Files (x86)" / "ffmpeg" / "bin" / "ffmpeg.exe",
            driveRoot / "ProgramData" / "chocolatey" / "bin" / "ffmpeg.exe"
        };
        for (const auto& candidate : commonFfmpegLocations) {
            if (const auto resolved = existsExe(candidate); resolved.has_value()) {
                addCandidate(*resolved);
            }
        }
    }

    wchar_t pathResolved[MAX_PATH] = {};
    const DWORD pathLen = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(std::size(pathResolved)), pathResolved, nullptr);
    if (pathLen > 0 && pathLen < std::size(pathResolved)) {
        if (const auto resolved = existsExe(std::filesystem::path(pathResolved)); resolved.has_value()) {
            addCandidate(*resolved);
        }
    }

    wchar_t modulePath[MAX_PATH] = {};
    const DWORD moduleLen = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (moduleLen > 0) {
        const auto localCandidate = std::filesystem::path(modulePath).parent_path() / "ffmpeg.exe";
        if (const auto resolved = existsExe(localCandidate); resolved.has_value()) {
            addCandidate(*resolved);
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

std::string DungeonNameForChallengeMap(int challengeMapId)
{
    switch (challengeMapId) {
    case 161: return "Skyreach";
    case 239: return "Seat of the Triumvirate";
    case 402: return "Algeth'ar Academy";
    case 556: return "Pit of Saron";
    case 557: return "Windrunner Spire";
    case 558: return "Magisters' Terrace";
    case 559: return "Nexus-Point Xenas";
    case 560: return "Maisara Caverns";
    default: return {};
    }
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

const char* AudioCaptureScopeLabel(AppSettings::AudioCaptureScope scope)
{
    switch (scope) {
    case AppSettings::AudioCaptureScope::WowAndDiscord:
        return "wow+discord";
    case AppSettings::AudioCaptureScope::AllDesktop:
        return "all-desktop";
    case AppSettings::AudioCaptureScope::WowOnly:
    default:
        return "wow-only";
    }
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
    std::scoped_lock lock(mutex_);
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
        + ", video=" + std::to_string(settings_.width) + "x" + std::to_string(settings_.height) + "@" + std::to_string(settings_.fps)
        + ", audio-scope=" + AudioCaptureScopeLabel(settings_.audioCaptureScope)
        + ", microphone=" + BoolLabel(settings_.captureMicrophone)
        + ", microphone-noise-suppression=" + BoolLabel(settings_.microphoneNoiseSuppression));

    auto recordingConfig = ToRecordingConfig(settings_);
    if (!engine_->Initialize(recordingConfig, error)) {
        PushStatus("OBS initialize failed: " + error);
        return false;
    }

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
    std::scoped_lock lock(mutex_);
    watcher_.Stop();
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
    std::scoped_lock lock(mutex_);
    ResetMythicTrackingState();
    return StartRecordingInternal(RecordingStartReason::Manual, error);
}

bool RecordingOrchestrator::StopManualRecording(std::string& error)
{
    std::scoped_lock lock(mutex_);
    const bool stopped = StopRecordingInternal(RecordingStopReason::Manual, error);
    ResetMythicTrackingState();
    return stopped;
}

void RecordingOrchestrator::Tick()
{
    std::scoped_lock lock(mutex_);
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
    std::scoped_lock lock(mutex_);
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
        StartRecordingInternal(RecordingStartReason::MythicStart, error);
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

bool RecordingOrchestrator::StartRecordingInternal(RecordingStartReason reason, std::string& error)
{
    if (engine_->IsRecording()) {
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

    // Reinitialize before every recording so setting changes made while monitoring
    // is already armed (for example audio capture scope) are applied immediately.
    auto recordingConfig = ToRecordingConfig(settings_);
    if (!engine_->Initialize(recordingConfig, error)) {
        PushStatus("Initialize failed: " + error);
        return false;
    }

    const auto fileStem = BuildFileStem(reason);
    if (!engine_->StartRecording(fileStem, error)) {
        PushStatus("Start recording failed: " + error);
        return false;
    }

    ++recordingSessionId_;
    state_ = OrchestratorState::Recording;
    ActiveRecordingMetadata metadata;
    metadata.triggerReason = reason;
    const bool useMp4 = settings_.videoContainer == "mp4";
    const auto extension = useMp4 ? ".mp4" : ".mkv";
    metadata.videoPath = settings_.outputDirectory / (fileStem + extension);
    metadata.recordingStartedAt = std::chrono::system_clock::now();
    metadata.challengeMapId = lastChallengeMapId_;
    metadata.keystoneLevel = lastKeystoneLevel_;
    metadata.participants = detector_.GetParticipants();
    if (mythicRunStartedAt_.has_value()) {
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
        const auto trimDuration = std::chrono::duration_cast<std::chrono::seconds>(job.trimEndAt - job.recordingStartedAt);
        PushStatus("Trim started for '" + job.videoPath.filename().string() +
            "' -> logical end " + FormatWallClock(job.trimEndAt) +
            " (keep " + FormatDurationClock(trimDuration) + ").");
        std::string trimError;
        if (RunCheapTrim(job, trimError)) {
            PushStatus("Trim finished for '" + job.videoPath.filename().string() +
                "' (kept " + FormatDurationClock(trimDuration) + ").");
        } else {
            PushStatus("Trim failed for '" + job.videoPath.filename().string() + "': " + trimError);
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

    const auto durationSeconds = std::chrono::duration_cast<std::chrono::seconds>(job.trimEndAt - job.recordingStartedAt);
    if (durationSeconds < kMinCheapTrimSeconds) {
        error = "Trim duration too short.";
        return false;
    }

    const auto ffmpegCandidates = ResolveFfmpegExecutableCandidates();
    if (ffmpegCandidates.empty()) {
        error = "Unable to locate ffmpeg executable (set BEAN_FFMPEG_PATH or install ffmpeg in PATH).";
        return false;
    }
    std::vector<std::string> candidateErrors;
    candidateErrors.reserve(ffmpegCandidates.size());
    for (const auto& ffmpegPath : ffmpegCandidates) {
        const auto tempPath = job.videoPath.parent_path() /
            (job.videoPath.stem().string() + ".trimtmp" + job.videoPath.extension().string());
        const auto backupPath = job.videoPath.parent_path() /
            (job.videoPath.stem().string() + ".pretrim" + job.videoPath.extension().string());

        std::error_code cleanupEc;
        std::filesystem::remove(tempPath, cleanupEc);
        cleanupEc.clear();
        std::filesystem::remove(backupPath, cleanupEc);

        std::wstring commandLine = QuoteArg(ffmpegPath)
            + L" -y -v error -i " + QuoteArg(job.videoPath)
            + L" -t " + std::to_wstring(durationSeconds.count())
            + L" -c copy " + QuoteArg(tempPath);

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
    if (statusCallback_) {
        statusCallback_(status);
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
