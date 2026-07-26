#include "core/RecordingOrchestrator.h"
#include "core/RecordingPath.h"
#include "core/ClipExportService.h"
#include "core/RunMetadataWriter.h"

#include "core/WowData.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <fstream>
#include <iterator>
#include <sstream>

namespace bean::core {
namespace {

constexpr auto kPostStartIdleTimeoutSuppress = std::chrono::seconds(180);
constexpr auto kCombatLogIdleTimeout = std::chrono::seconds(720);

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
    clipExport_.Stop();
}

void RecordingOrchestrator::SetStatusCallback(StatusCallback callback)
{
    {
        std::scoped_lock lock(statusCallbackMutex_);
        statusCallback_ = callback;
    }
    clipExport_.SetStatusCallback(callback);
    runMetadataWriter_.SetStatusCallback(std::move(callback));
}

void RecordingOrchestrator::SetRunRepository(std::shared_ptr<RunRepository> repository)
{
    std::scoped_lock lock(mutex_);
    runRepository_ = repository;
    runMetadataWriter_.SetRepository(std::move(repository));
    SyncClipExportSettingsLocked();
}

void RecordingOrchestrator::SyncClipExportSettingsLocked()
{
    clipExport_.SetClipDurationSeconds(settings_.clipDurationSeconds);
    if (runRepository_) {
        clipExport_.SetJournalDirectory(runRepository_->GetDatabasePath().parent_path());
    } else if (!settings_.outputDirectory.empty()) {
        clipExport_.SetJournalDirectory(settings_.outputDirectory);
    }
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
    SyncClipExportSettingsLocked();
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

    SyncClipExportSettingsLocked();
    clipExport_.RecoverPendingWork();

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

    return clipExport_.RequestClip(
        activeRecordingMetadata_->videoPath,
        activeRecordingMetadata_->recordingStartedAt,
        activeRecordingMetadata_->recordingStartedAtSteady,
        error);
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
    const auto videoContainer = settings_.videoContainer;
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
    metadata.videoPath = BuildRecordingPath(outputDirectory, fileStem, videoContainer);
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
    if (activeRecordingMetadata_.has_value()) {
        clipExport_.OnRecordingStopped(
            activeRecordingMetadata_->videoPath,
            activeRecordingMetadata_->recordingStartedAt,
            activeRecordingMetadata_->recordingStartedAtSteady,
            logicalEndAt);
    }
    PersistRunRecord(reason);
    PushStatus("Recording stopped (" + std::string(ToString(reason)) + ").");
    return true;
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

    FinishedRecordingSnapshot snapshot;
    snapshot.triggerReason = activeRecordingMetadata_->triggerReason;
    snapshot.videoPath = activeRecordingMetadata_->videoPath;
    snapshot.recordingStartedAt = activeRecordingMetadata_->recordingStartedAt;
    snapshot.recordingEndedAt = std::chrono::system_clock::now();
    snapshot.mythicRunStartedAt = activeRecordingMetadata_->mythicRunStartedAt;
    snapshot.mythicRunEndedAt = activeRecordingMetadata_->mythicRunEndedAt;
    snapshot.challengeMapId = activeRecordingMetadata_->challengeMapId;
    snapshot.keystoneLevel = activeRecordingMetadata_->keystoneLevel;
    snapshot.observedDungeonName = activeRecordingMetadata_->observedDungeonName;
    snapshot.participants = activeRecordingMetadata_->participants;
    activeRecordingMetadata_.reset();
    runMetadataWriter_.Persist(std::move(snapshot), stopReason);
}

} // namespace bean::core
