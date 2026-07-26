#pragma once

#include "core/ClipExportService.h"
#include "core/RecordingTypes.h"
#include "core/RunMetadataWriter.h"
#include "core/RunRepository.h"
#include "core/SettingsStore.h"
#include "log/CombatLogWatcher.h"
#include "log/MythicRunDetector.h"
#include "obs/IRecorderEngine.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bean::core {

class RecordingOrchestrator {
public:
    using StatusCallback = std::function<void(const std::string&)>;

    explicit RecordingOrchestrator(std::unique_ptr<obs::IRecorderEngine> engine);
    ~RecordingOrchestrator();

    void SetStatusCallback(StatusCallback callback);
    void SetRunRepository(std::shared_ptr<RunRepository> repository);
    void ApplySettings(const AppSettings& settings);

    bool StartMonitoring(std::string& error);
    void StopMonitoring();

    bool StartManualRecording(std::string& error);
    bool StopManualRecording(std::string& error);
    bool RequestClip(std::string& error);
    void Tick();

    OrchestratorState GetState() const;
    std::uint64_t GetRecordingSessionId() const;
    bool IsMonitoring() const;

private:
    void HandleCombatLogLine(const std::string& line);
    void ProcessCombatLogLine(const std::string& line);
    void CombatLogWorkerLoop();
    void StartCombatLogWorker();
    void StopCombatLogWorker();
    void ResetMythicTrackingState();
    // Caller must own `lock`. The lock is released for the duration of the
    // blocking engine Initialize/StartRecording calls so Tick and other
    // orchestrator entry points are not stalled behind OBS warmup.
    bool StartRecordingInternal(
        RecordingStartReason reason,
        std::string& error,
        std::unique_lock<std::mutex>& lock);
    bool StopRecordingInternal(
        RecordingStopReason reason,
        std::string& error,
        std::optional<std::chrono::system_clock::time_point> logicalEndAt = std::nullopt);
    std::string BuildFileStem(RecordingStartReason reason) const;
    void PersistRunRecord(RecordingStopReason stopReason);
    void PushStatus(const std::string& status) const;
    void SyncClipExportSettingsLocked();

    struct ActiveRecordingMetadata {
        RecordingStartReason triggerReason = RecordingStartReason::Manual;
        std::filesystem::path videoPath;
        std::chrono::system_clock::time_point recordingStartedAt;
        std::chrono::steady_clock::time_point recordingStartedAtSteady;
        std::optional<std::chrono::system_clock::time_point> recordingEndedAt;
        std::optional<int> challengeMapId;
        std::optional<int> keystoneLevel;
        std::optional<std::string> observedDungeonName;
        std::vector<log::MythicParticipant> participants;
        std::optional<std::chrono::system_clock::time_point> mythicRunStartedAt;
        std::optional<std::chrono::system_clock::time_point> mythicRunEndedAt;
    };

    mutable std::mutex mutex_;
    std::unique_ptr<obs::IRecorderEngine> engine_;
    log::CombatLogWatcher watcher_;
    log::MythicRunDetector detector_;
    ClipExportService clipExport_;
    RunMetadataWriter runMetadataWriter_;
    AppSettings settings_{};
    OrchestratorState state_ = OrchestratorState::Idle;
    // True while Initialize/StartRecording are running outside mutex_. Guards
    // against overlapping starts and tells Tick not to treat the gap as idle.
    bool recordingStartInProgress_ = false;
    // Guarded separately from mutex_ because the trim worker pushes status from
    // a background thread while the UI thread may be installing the callback.
    mutable std::mutex statusCallbackMutex_;
    StatusCallback statusCallback_;
    // Combat log lines are handed off by the watcher thread and processed here,
    // so neither the watcher nor the UI thread ever blocks on recording start.
    std::thread combatLogWorker_;
    std::mutex combatLogQueueMutex_;
    std::condition_variable combatLogQueueCv_;
    std::deque<std::string> combatLogQueue_;
    bool combatLogWorkerStopRequested_ = false;
    std::optional<std::chrono::steady_clock::time_point> mythicRunStartedAt_;
    std::optional<std::chrono::steady_clock::time_point> postRunStopAt_;
    std::optional<RecordingStopReason> postRunStopReason_;
    std::optional<std::chrono::steady_clock::time_point> lastCombatLogLineAt_;
    std::optional<std::chrono::system_clock::time_point> lastCombatLogLineAtWallClock_;
    std::optional<std::chrono::steady_clock::time_point> lastCombatLogFileWriteAt_;
    std::optional<std::chrono::system_clock::time_point> lastCombatLogFileWriteAtWallClock_;
    std::optional<std::filesystem::path> observedCombatLogFile_;
    std::optional<std::filesystem::file_time_type> observedCombatLogWriteTime_;
    std::shared_ptr<RunRepository> runRepository_;
    std::optional<ActiveRecordingMetadata> activeRecordingMetadata_;
    std::optional<int> lastChallengeMapId_;
    std::optional<int> lastKeystoneLevel_;
    std::uint64_t recordingSessionId_ = 0;
    std::uint64_t lastObservedWatcherSeekRecoveries_ = 0;
};

} // namespace bean::core
