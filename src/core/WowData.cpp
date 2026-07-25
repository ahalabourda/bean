#include "core/WowData.h"

#include <array>
#include <string_view>
#include <utility>

namespace bean::core {
namespace {

// Blizzard adds dungeons every season, so keep this as a plain table that is
// obvious to extend rather than a switch buried in a translation unit.
constexpr std::array<std::pair<int, std::string_view>, 8> kDungeonsByChallengeMapId{{
    {161, "Skyreach"},
    {239, "Seat of the Triumvirate"},
    {402, "Algeth'ar Academy"},
    {556, "Pit of Saron"},
    {557, "Windrunner Spire"},
    {558, "Magisters' Terrace"},
    {559, "Nexus-Point Xenas"},
    {560, "Maisara Caverns"},
}};

} // namespace

std::string DungeonNameForChallengeMap(int challengeMapId)
{
    for (const auto& [mapId, name] : kDungeonsByChallengeMapId) {
        if (mapId == challengeMapId) {
            return std::string(name);
        }
    }
    return {};
}

} // namespace bean::core
