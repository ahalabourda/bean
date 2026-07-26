#pragma once

#include "core/RunRepository.h"
#include "core/RecordingTypes.h"
#include "log/MythicRunDetector.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bean::core {

// Snapshot of a finished recording used only for metadata persistence.
struct FinishedRecordingSnapshot {
    RecordingStartReason triggerReason = RecordingStartReason::Manual;
    std::filesystem::path videoPath;
    std::chrono::system_clock::time_point recordingStartedAt{};
    std::chrono::system_clock::time_point recordingEndedAt{};
    std::optional<std::chrono::system_clock::time_point> mythicRunStartedAt;
    std::optional<std::chrono::system_clock::time_point> mythicRunEndedAt;
    std::optional<int> challengeMapId;
    std::optional<int> keystoneLevel;
    std::optional<std::string> observedDungeonName;
    std::vector<log::MythicParticipant> participants;
};

class RunMetadataWriter {
public:
    using StatusCallback = std::function<void(const std::string&)>;

    explicit RunMetadataWriter(std::shared_ptr<RunRepository> repository = nullptr);

    void SetRepository(std::shared_ptr<RunRepository> repository);
    void SetStatusCallback(StatusCallback callback);
    void Persist(FinishedRecordingSnapshot snapshot, RecordingStopReason stopReason);

private:
    void PushStatus(const std::string& status) const;

    std::shared_ptr<RunRepository> repository_;
    mutable std::mutex statusCallbackMutex_;
    StatusCallback statusCallback_;
};

} // namespace bean::core
