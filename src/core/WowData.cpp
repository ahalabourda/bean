#include "core/WowData.h"

#include <array>
#include <string_view>

namespace bean::core {
namespace {

struct DungeonInfo {
    int challengeMapId = 0;
    int instanceId = 0;
    std::string_view name;
    int timerLimitSeconds = 0;
};

// Blizzard adds dungeons every season, so keep this as a plain table that is
// obvious to extend rather than a switch buried in a translation unit.
// challengeMapId 0 means the MapChallengeMode id is not yet confirmed; instance
// id is still enough to classify CHALLENGE_MODE_END.
constexpr std::array<DungeonInfo, 16> kDungeons{{
    // Midnight Season 1
    {161, 1209, "Skyreach", 28 * 60},
    {239, 1753, "Seat of the Triumvirate", 34 * 60},
    {402, 2526, "Algeth'ar Academy", 29 * 60 + 30},
    {556, 658, "Pit of Saron", 31 * 60},
    {557, 2805, "Windrunner Spire", 33 * 60 + 30},
    {558, 2811, "Magisters' Terrace", 33 * 60},
    {559, 2915, "Nexus-Point Xenas", 29 * 60 + 30},
    {560, 2874, "Maisara Caverns", 33 * 60},
    // Midnight Season 2
    {249, 1762, "Kings' Rest", 33 * 60},
    {250, 1877, "Temple of Sethraliss", 33 * 60},
    {399, 2521, "Ruby Life Pools", 28 * 60},
    {586, 2825, "Den of Nalorakk", 32 * 60},
    {587, 2813, "Murder Row", 34 * 60},
    {588, 2993, "Altar of Fangs", 30 * 60},
    {0, 2859, "The Blinding Vale", 31 * 60},
    {0, 2923, "Voidscar Arena", 30 * 60},
}};

const DungeonInfo* FindDungeon(int mapId)
{
    if (mapId <= 0) {
        return nullptr;
    }
    for (const auto& dungeon : kDungeons) {
        if (dungeon.challengeMapId == mapId || dungeon.instanceId == mapId) {
            return &dungeon;
        }
    }
    return nullptr;
}

} // namespace

std::string DungeonNameForChallengeMap(int challengeMapId)
{
    if (const auto* dungeon = FindDungeon(challengeMapId); dungeon != nullptr) {
        return std::string(dungeon->name);
    }
    return {};
}

std::optional<int> MythicTimerLimitSeconds(int mapId)
{
    if (const auto* dungeon = FindDungeon(mapId); dungeon != nullptr && dungeon->timerLimitSeconds > 0) {
        return dungeon->timerLimitSeconds;
    }
    return std::nullopt;
}

} // namespace bean::core
