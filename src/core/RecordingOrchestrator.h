#pragma once

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

enum class OrchestratorState {
    Idle,
    Armed,
    Recording
};

enum class RecordingStartReason {
    Manual,
    MythicStart
};

enum class RecordingStopReason {
    Manual,
    Shutdown,
    CombatLogIdleTimeout,
    MythicSuccess,
    MythicFailure,
    MythicRestart
};

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
    struct TrimJob {
        std::filesystem::path videoPath;
        std::filesystem::path outputPath;
        std::chrono::system_clock::time_point recordingStartedAt{};
        std::chrono::system_clock::time_point clipStartAt{};
        std::chrono::system_clock::time_point trimEndAt{};
        std::uint64_t clipRequestId = 0;
        int journalFailureCount = 0;
        bool isClip = false;
    };

    struct ClipRequest {
        std::uint64_t id = 0;
        std::filesystem::path videoPath;
        std::filesystem::path outputPath;
        std::chrono::system_clock::time_point recordingStartedAt{};
        std::chrono::steady_clock::time_point recordingStartedAtSteady{};
        std::chrono::system_clock::time_point requestedAt{};
        std::chrono::steady_clock::time_point requestedAtSteady{};
        int durationSeconds = 30;
        int clipIndex = 1;
    };

    void HandleCombatLogLine(const std::string& line);
    void ResetMythicTrackingState();
    bool StartRecordingInternal(RecordingStartReason reason, std::string& error);
    bool StopRecordingInternal(
        RecordingStopReason reason,
        std::string& error,
        std::optional<std::chrono::system_clock::time_point> logicalEndAt = std::nullopt);
    std::string BuildFileStem(RecordingStartReason reason) const;
    void PersistRunRecord(RecordingStopReason stopReason);
    void PushStatus(const std::string& status) const;
    void EnsureTrimWorkerRunning();
    void StopTrimWorker();
    void EnqueueTrimJob(const TrimJob& job);
    void TrimWorkerLoop();
    bool RunCheapTrim(const TrimJob& job, std::string& error) const;
    bool AppendClipJournalRequest(const std::filesystem::path& videoPath, const ClipRequest& request, std::string& error) const;
    bool AppendClipJournalFailure(std::uint64_t requestId, const std::string& error) const;
    bool RemoveClipJournalRequest(std::uint64_t requestId) const;
    void RecoverClipJournal();
    std::filesystem::path ClipJournalPath() const;
    std::filesystem::path BuildClipPath(const std::filesystem::path& videoPath, const ClipRequest& request) const;

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
    AppSettings settings_{};
    OrchestratorState state_ = OrchestratorState::Idle;
    StatusCallback statusCallback_;
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
    std::vector<ClipRequest> pendingClipRequests_;
    std::optional<int> lastChallengeMapId_;
    std::optional<int> lastKeystoneLevel_;
    std::uint64_t recordingSessionId_ = 0;
    std::uint64_t lastObservedWatcherSeekRecoveries_ = 0;
    std::thread trimWorker_;
    std::mutex trimQueueMutex_;
    std::condition_variable trimQueueCv_;
    std::deque<TrimJob> trimQueue_;
    mutable std::mutex clipJournalMutex_;
    bool trimWorkerStopRequested_ = false;
    std::uint64_t nextClipRequestId_ = 1;
};

} // namespace bean::core
