#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bean::core {

// Owns the ffmpeg trim worker, clip request journal, and pending clip queue.
// RecordingOrchestrator remains the coordinator; this service is the only place
// that shells out to ffmpeg or mutates the clip journal.
class ClipExportService {
public:
    using StatusCallback = std::function<void(const std::string&)>;

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

    ClipExportService() = default;
    ~ClipExportService();

    ClipExportService(const ClipExportService&) = delete;
    ClipExportService& operator=(const ClipExportService&) = delete;

    void SetStatusCallback(StatusCallback callback);
    void SetJournalDirectory(std::filesystem::path journalDirectory);
    void SetClipDurationSeconds(int seconds);

    // Queue a clip against the active recording. Journals the request so a crash
    // before export still recovers on the next monitoring start.
    bool RequestClip(
        const std::filesystem::path& videoPath,
        std::chrono::system_clock::time_point recordingStartedAt,
        std::chrono::steady_clock::time_point recordingStartedAtSteady,
        std::string& error);

    // After StopRecording: enqueue cheap post-run trim (optional) and flush
    // pending clip exports onto the worker queue.
    void OnRecordingStopped(
        const std::filesystem::path& videoPath,
        std::chrono::system_clock::time_point recordingStartedAt,
        std::chrono::steady_clock::time_point recordingStartedAtSteady,
        std::optional<std::chrono::system_clock::time_point> logicalEndAt);

    void RecoverPendingWork();
    void Stop();

private:
    void EnsureTrimWorkerRunning();
    void EnqueueTrimJob(const TrimJob& job);
    void TrimWorkerLoop();
    bool RunCheapTrim(const TrimJob& job, std::string& error) const;
    bool AppendClipJournalRequest(const std::filesystem::path& videoPath, const ClipRequest& request, std::string& error) const;
    bool AppendClipJournalFailure(std::uint64_t requestId, const std::string& error) const;
    bool RemoveClipJournalRequest(std::uint64_t requestId) const;
    std::filesystem::path ClipJournalPath() const;
    std::filesystem::path BuildClipPath(const std::filesystem::path& videoPath, const ClipRequest& request) const;
    void PushStatus(const std::string& status) const;

    mutable std::mutex statusCallbackMutex_;
    StatusCallback statusCallback_;

    std::filesystem::path journalDirectory_;
    int clipDurationSeconds_ = 30;

    std::mutex pendingMutex_;
    std::vector<ClipRequest> pendingClipRequests_;
    std::uint64_t nextClipRequestId_ = 1;

    std::thread trimWorker_;
    std::mutex trimQueueMutex_;
    std::condition_variable trimQueueCv_;
    std::deque<TrimJob> trimQueue_;
    mutable std::mutex clipJournalMutex_;
    bool trimWorkerStopRequested_ = false;
};

} // namespace bean::core
