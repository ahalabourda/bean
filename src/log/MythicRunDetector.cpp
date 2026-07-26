#include "log/MythicRunDetector.h"
#include "log/CombatLogFields.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <vector>

namespace bean::log {
namespace {

constexpr size_t kExpectedMythicPartySize = 5;

struct ParsedCombatLogLine {
    std::string_view eventName;
    std::vector<std::string_view> fields;
    // Owns the payload string that `fields` views into.
    std::string payload;
};

std::string ExtractEventPayload(const std::string& line)
{
    const auto firstSpace = line.find("  ");
    if (firstSpace == std::string::npos || firstSpace + 2 >= line.size()) {
        // Timestamp-less fixtures / journal lines: treat the whole line as payload.
        return line;
    }
    return line.substr(firstSpace + 2);
}

std::vector<std::string_view> SplitCsvTopLevel(std::string_view input)
{
    std::vector<std::string_view> out;
    size_t start = 0;
    bool inQuotes = false;
    int bracketDepth = 0;
    int parenDepth = 0;

    for (size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (!inQuotes) {
            if (ch == '[') {
                ++bracketDepth;
            } else if (ch == ']' && bracketDepth > 0) {
                --bracketDepth;
            } else if (ch == '(') {
                ++parenDepth;
            } else if (ch == ')' && parenDepth > 0) {
                --parenDepth;
            } else if (ch == ',' && bracketDepth == 0 && parenDepth == 0) {
                out.push_back(input.substr(start, i - start));
                start = i + 1;
            }
        }
    }
    out.push_back(input.substr(start));
    return out;
}

std::optional<ParsedCombatLogLine> ParseCombatLogLine(const std::string& line)
{
    ParsedCombatLogLine parsed;
    parsed.payload = ExtractEventPayload(line);
    if (parsed.payload.empty()) {
        return std::nullopt;
    }
    parsed.fields = SplitCsvTopLevel(parsed.payload);
    if (parsed.fields.empty()) {
        return std::nullopt;
    }
    parsed.eventName = parsed.fields[0];
    return parsed;
}

std::optional<int> ParseInt(std::string_view token)
{
    int value = 0;
    const auto begin = token.data();
    const auto end = token.data() + token.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> ParseDouble(std::string_view token)
{
    if (token.empty()) {
        return std::nullopt;
    }
    std::string copy(token);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return value;
}

std::string UnquoteToken(std::string_view token)
{
    if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
        return std::string(token.substr(1, token.size() - 2));
    }
    return std::string(token);
}

bool IsPlayerGuid(std::string_view value)
{
    constexpr std::string_view kPrefix = "Player-";
    return value.rfind(kPrefix, 0) == 0;
}

bool IsLikelyInvalidPlayerName(std::string_view value)
{
    if (value.empty() || value == "nil") {
        return true;
    }
    bool allDigits = true;
    for (unsigned char ch : value) {
        if (!std::isdigit(ch)) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        return true;
    }
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        return true;
    }
    return false;
}

bool IsLikelyInvalidRealm(std::string_view value)
{
    return value.empty() || value == "nil";
}

void ParseNameRealm(std::string_view nameRealmToken,
    std::optional<std::string>& outName,
    std::optional<std::string>& outRealm)
{
    const auto value = UnquoteToken(nameRealmToken);
    if (value.empty()) {
        return;
    }
    const auto dashPos = value.find('-');
    if (dashPos == std::string::npos) {
        if (!IsLikelyInvalidPlayerName(value)) {
            const bool canReplace =
                !outName.has_value() || outName->empty() || IsLikelyInvalidPlayerName(*outName);
            if (canReplace) {
                outName = value;
            }
        }
        return;
    }

    const auto name = value.substr(0, dashPos);
    const auto realm = value.substr(dashPos + 1);
    if (!IsLikelyInvalidPlayerName(name)) {
        const bool canReplace =
            !outName.has_value() || outName->empty() || IsLikelyInvalidPlayerName(*outName);
        if (canReplace) {
            outName = name;
        }
    }
    if (!IsLikelyInvalidRealm(realm)) {
        outRealm = realm;
    }
}

void PopulateChallengeDetails(const ParsedCombatLogLine& parsed, MythicEvent& event)
{
    namespace Start = ChallengeModeStartFields;
    namespace End = ChallengeModeEndFields;

    if (parsed.eventName == "CHALLENGE_MODE_START") {
        if (parsed.fields.size() < Start::MinCount) {
            return;
        }
        const auto dungeonName = UnquoteToken(parsed.fields[Start::DungeonName]);
        if (!dungeonName.empty()) {
            event.mapName = dungeonName;
        }
        if (const auto challengeMapId = ParseInt(parsed.fields[Start::ChallengeMapId]);
            challengeMapId.has_value() && *challengeMapId > 0) {
            event.challengeMapId = *challengeMapId;
        }
        if (const auto level = ParseInt(parsed.fields[Start::KeystoneLevel]);
            level.has_value() && *level > 0 && *level <= 40) {
            event.keystoneLevel = *level;
        }
        return;
    }

    if (parsed.eventName == "CHALLENGE_MODE_END") {
        if (parsed.fields.size() < End::MinCount) {
            return;
        }
        if (const auto mapId = ParseInt(parsed.fields[End::ChallengeMapId]);
            mapId.has_value() && *mapId > 0) {
            event.challengeMapId = *mapId;
        }
        if (const auto level = ParseInt(parsed.fields[End::KeystoneLevel]);
            level.has_value() && *level > 0 && *level <= 40) {
            event.keystoneLevel = *level;
        }
    }
}

bool IsChallengeModeEndTimed(const ParsedCombatLogLine& parsed)
{
    namespace End = ChallengeModeEndFields;
    if (parsed.eventName != "CHALLENGE_MODE_END" || parsed.fields.size() < End::MinCount) {
        return false;
    }

    const auto mapId = ParseInt(parsed.fields[End::ChallengeMapId]).value_or(0);
    const auto successFlag = ParseInt(parsed.fields[End::Success]).value_or(0);
    const auto keystoneLevel = ParseInt(parsed.fields[End::KeystoneLevel]).value_or(0);
    const auto totalTimeMs = ParseDouble(parsed.fields[End::TotalTimeMs]).value_or(0.0);
    const auto onTimeDeltaSeconds = ParseDouble(parsed.fields[End::OnTimeDeltaSeconds]).value_or(0.0);
    const auto timerLimitSeconds = ParseDouble(parsed.fields[End::TimerLimitSeconds]).value_or(0.0);

    const bool allZeroEndPayload = mapId == 0
        && successFlag == 0
        && keystoneLevel == 0
        && totalTimeMs == 0.0
        && onTimeDeltaSeconds == 0.0
        && timerLimitSeconds == 0.0;
    if (allZeroEndPayload) {
        return false;
    }

    if (successFlag <= 0) {
        return false;
    }
    return onTimeDeltaSeconds >= 0.0;
}

} // namespace

std::optional<SpecInfo> ResolveSpecInfo(int specId)
{
    switch (specId) {
    case 62: return SpecInfo{"Arcane", "Mage"};
    case 63: return SpecInfo{"Fire", "Mage"};
    case 64: return SpecInfo{"Frost", "Mage"};
    case 65: return SpecInfo{"Holy", "Paladin"};
    case 66: return SpecInfo{"Protection", "Paladin"};
    case 70: return SpecInfo{"Retribution", "Paladin"};
    case 71: return SpecInfo{"Arms", "Warrior"};
    case 72: return SpecInfo{"Fury", "Warrior"};
    case 73: return SpecInfo{"Protection", "Warrior"};
    case 102: return SpecInfo{"Balance", "Druid"};
    case 103: return SpecInfo{"Feral", "Druid"};
    case 104: return SpecInfo{"Guardian", "Druid"};
    case 105: return SpecInfo{"Restoration", "Druid"};
    case 1467: return SpecInfo{"Devastation", "Evoker"};
    case 1468: return SpecInfo{"Preservation", "Evoker"};
    case 1473: return SpecInfo{"Augmentation", "Evoker"};
    case 250: return SpecInfo{"Blood", "Death Knight"};
    case 251: return SpecInfo{"Frost", "Death Knight"};
    case 252: return SpecInfo{"Unholy", "Death Knight"};
    case 253: return SpecInfo{"Beast Mastery", "Hunter"};
    case 254: return SpecInfo{"Marksmanship", "Hunter"};
    case 255: return SpecInfo{"Survival", "Hunter"};
    case 256: return SpecInfo{"Discipline", "Priest"};
    case 257: return SpecInfo{"Holy", "Priest"};
    case 258: return SpecInfo{"Shadow", "Priest"};
    case 259: return SpecInfo{"Assassination", "Rogue"};
    case 260: return SpecInfo{"Outlaw", "Rogue"};
    case 261: return SpecInfo{"Subtlety", "Rogue"};
    case 262: return SpecInfo{"Elemental", "Shaman"};
    case 263: return SpecInfo{"Enhancement", "Shaman"};
    case 264: return SpecInfo{"Restoration", "Shaman"};
    case 265: return SpecInfo{"Affliction", "Warlock"};
    case 266: return SpecInfo{"Demonology", "Warlock"};
    case 267: return SpecInfo{"Destruction", "Warlock"};
    case 268: return SpecInfo{"Brewmaster", "Monk"};
    case 269: return SpecInfo{"Windwalker", "Monk"};
    case 270: return SpecInfo{"Mistweaver", "Monk"};
    case 577: return SpecInfo{"Havoc", "Demon Hunter"};
    case 581: return SpecInfo{"Vengeance", "Demon Hunter"};
    case 1480: return SpecInfo{"Devourer", "Demon Hunter"};
    default: return std::nullopt;
    }
}

bool MythicRunDetector::HasResolvedMythicParty() const
{
    size_t resolvedCount = 0;
    for (const auto& [_, participant] : participantsByGuid_) {
        const bool hasName = participant.name.has_value() && !participant.name->empty();
        const bool hasRealm = participant.realm.has_value() && !participant.realm->empty();
        const bool hasClass = participant.className.has_value() && !participant.className->empty();
        const bool hasSpec = participant.specName.has_value() && !participant.specName->empty();
        if (hasName && hasRealm && hasClass && hasSpec) {
            ++resolvedCount;
            if (resolvedCount >= kExpectedMythicPartySize) {
                return true;
            }
        }
    }
    return false;
}

void MythicRunDetector::UpdateParticipantFromCombatantInfo(const std::vector<std::string_view>& fields)
{
    namespace Info = CombatantInfoFields;
    if (fields.size() <= Info::SpecIdPrimary) {
        return;
    }
    if (fields[Info::Event] != "COMBATANT_INFO") {
        return;
    }
    if (!IsPlayerGuid(fields[Info::Guid])) {
        return;
    }

    const auto guid = std::string(fields[Info::Guid]);
    auto& participant = participantsByGuid_[guid];
    participant.guid = guid;

    std::optional<int> specId = ParseInt(fields[Info::SpecIdPrimary]);
    if ((!specId.has_value() || *specId <= 0) && fields.size() > Info::SpecIdFallback) {
        specId = ParseInt(fields[Info::SpecIdFallback]);
    }
    if (specId.has_value() && *specId > 0) {
        participant.specId = *specId;
        if (const auto specInfo = ResolveSpecInfo(*specId); specInfo.has_value()) {
            participant.specName = specInfo->specName;
            participant.className = specInfo->className;
        }
    }
}

void MythicRunDetector::UpdateParticipantFromCombatEvent(const std::vector<std::string_view>& fields)
{
    namespace Actor = CombatEventActorFields;
    if (fields.empty()) {
        return;
    }

    const auto captureAt = [&](size_t guidIndex, size_t nameIndex) {
        if (guidIndex >= fields.size() || nameIndex >= fields.size()) {
            return;
        }
        if (!IsPlayerGuid(fields[guidIndex])) {
            return;
        }
        const auto guid = std::string(fields[guidIndex]);
        auto& participant = participantsByGuid_[guid];
        participant.guid = guid;
        ParseNameRealm(fields[nameIndex], participant.name, participant.realm);
    };

    captureAt(Actor::SourceGuid, Actor::SourceName);
    captureAt(Actor::DestGuid, Actor::DestName);
    captureAt(Actor::SourceGuidWithHideCaster, Actor::SourceNameWithHideCaster);
    captureAt(Actor::DestGuidWithHideCaster, Actor::DestNameWithHideCaster);
}

std::vector<MythicParticipant> MythicRunDetector::GetParticipants() const
{
    std::vector<MythicParticipant> participants;
    participants.reserve(participantsByGuid_.size());
    for (const auto& [_, participant] : participantsByGuid_) {
        participants.push_back(participant);
    }

    std::sort(participants.begin(), participants.end(), [](const MythicParticipant& a, const MythicParticipant& b) {
        const std::string aName = a.name.value_or(std::string{});
        const std::string bName = b.name.value_or(std::string{});
        if (aName != bName) {
            return aName < bName;
        }
        return a.guid < b.guid;
    });
    return participants;
}

std::optional<MythicEvent> MythicRunDetector::ProcessLine(const std::string& line)
{
    const auto parsed = ParseCombatLogLine(line);
    if (!parsed.has_value()) {
        return std::nullopt;
    }

    // Match on the event-name field only. Substring scans of the full line
    // falsely triggered on addon chatter that mentioned these tokens.
    if (parsed->eventName == "CHALLENGE_MODE_START") {
        isInRun_ = true;
        participantsByGuid_.clear();
        participantCollectionComplete_ = false;
        MythicEvent event{MythicEventType::RunStarted, line};
        PopulateChallengeDetails(*parsed, event);
        return event;
    }

    if (parsed->eventName == "CHALLENGE_MODE_END" && isInRun_) {
        isInRun_ = false;
        participantCollectionComplete_ = false;
        const MythicEventType endType = IsChallengeModeEndTimed(*parsed)
            ? MythicEventType::RunEndedSuccess
            : MythicEventType::RunEndedFailure;
        MythicEvent event{endType, line};
        PopulateChallengeDetails(*parsed, event);
        return event;
    }

    if (parsed->eventName == "CHALLENGE_MODE_RESET" && isInRun_) {
        isInRun_ = false;
        participantCollectionComplete_ = false;
        MythicEvent event{MythicEventType::RunEndedFailure, line};
        PopulateChallengeDetails(*parsed, event);
        return event;
    }

    if (!isInRun_) {
        return std::nullopt;
    }

    if (!participantCollectionComplete_) {
        if (parsed->eventName == "COMBATANT_INFO") {
            UpdateParticipantFromCombatantInfo(parsed->fields);
        } else {
            UpdateParticipantFromCombatEvent(parsed->fields);
        }
        participantCollectionComplete_ = HasResolvedMythicParty();
    }

    return std::nullopt;
}

} // namespace bean::log
