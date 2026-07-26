#include "core/RunMetadataWriter.h"
#include "core/WowData.h"

namespace bean::core {
namespace {

const char* StartReasonLabel(RecordingStartReason reason)
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

const char* StopReasonLabel(RecordingStopReason reason)
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

RunMetadataWriter::RunMetadataWriter(std::shared_ptr<RunRepository> repository)
    : repository_(std::move(repository))
{
}

void RunMetadataWriter::SetRepository(std::shared_ptr<RunRepository> repository)
{
    repository_ = std::move(repository);
}

void RunMetadataWriter::SetStatusCallback(StatusCallback callback)
{
    std::scoped_lock lock(statusCallbackMutex_);
    statusCallback_ = std::move(callback);
}

void RunMetadataWriter::PushStatus(const std::string& status) const
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

void RunMetadataWriter::Persist(FinishedRecordingSnapshot snapshot, RecordingStopReason stopReason)
{
    if (!repository_) {
        PushStatus("Run repository is unavailable; metadata not persisted.");
        return;
    }

    if ((stopReason == RecordingStopReason::MythicSuccess || stopReason == RecordingStopReason::MythicFailure)
        && !snapshot.mythicRunEndedAt.has_value()) {
        snapshot.mythicRunEndedAt = snapshot.recordingEndedAt;
    }

    std::string result = "unknown";
    if (stopReason == RecordingStopReason::MythicSuccess) {
        result = "success";
    } else if (stopReason == RecordingStopReason::MythicFailure) {
        result = "failure";
    }

    RunRecord record;
    record.videoPath = snapshot.videoPath;
    record.videoFileName = snapshot.videoPath.filename().string();
    record.triggerReason = StartReasonLabel(snapshot.triggerReason);
    record.stopReason = StopReasonLabel(stopReason);
    record.result = result;
    record.recordingStartedAt = snapshot.recordingStartedAt;
    record.recordingEndedAt = snapshot.recordingEndedAt;
    record.mythicRunStartedAt = snapshot.mythicRunStartedAt;
    record.mythicRunEndedAt = snapshot.mythicRunEndedAt;
    record.challengeMapId = snapshot.challengeMapId;
    record.keystoneLevel = snapshot.keystoneLevel;
    for (const auto& participant : snapshot.participants) {
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
    if (snapshot.challengeMapId.has_value()) {
        const auto dungeonName = DungeonNameForChallengeMap(*snapshot.challengeMapId);
        if (!dungeonName.empty()) {
            record.dungeonName = dungeonName;
        }
    }
    if (!record.dungeonName.has_value() && snapshot.observedDungeonName.has_value()) {
        record.dungeonName = *snapshot.observedDungeonName;
    }
    if (!record.dungeonName.has_value() && snapshot.triggerReason == RecordingStartReason::Manual) {
        record.dungeonName = "Manual Recording";
    }

    std::string dbError;
    if (!repository_->UpsertRun(record, dbError)) {
        PushStatus("Failed to persist run metadata: " + dbError);
    }
}

} // namespace bean::core
