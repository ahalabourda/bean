#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bean::log {

struct SpecInfo {
    const char* specName = nullptr;
    const char* className = nullptr;
};

std::optional<SpecInfo> ResolveSpecInfo(int specId);

enum class MythicEventType {
    RunStarted,
    RunEndedSuccess,
    RunEndedFailure
};

struct MythicEvent {
    MythicEventType type;
    std::string rawLine;
    std::optional<int> challengeMapId;
    std::optional<int> keystoneLevel;
    std::optional<int> mapId;
    std::optional<std::string> mapName;
};

struct MythicParticipant {
    std::string guid;
    std::optional<std::string> name;
    std::optional<std::string> realm;
    std::optional<std::string> region;
    std::optional<int> specId;
    std::optional<std::string> specName;
    std::optional<std::string> className;
};

class MythicRunDetector {
public:
    std::optional<MythicEvent> ProcessLine(const std::string& line);
    std::vector<MythicParticipant> GetParticipants() const;

private:
    bool HasResolvedMythicParty() const;
    void UpdateParticipantFromCombatantInfo(const std::string& line);
    void UpdateParticipantFromCombatEvent(const std::string& line);

    bool isInRun_ = false;
    std::unordered_map<std::string, MythicParticipant> participantsByGuid_;
    bool participantCollectionComplete_ = false;
};

} // namespace bean::log
