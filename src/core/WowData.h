#pragma once

#include <optional>
#include <string>

namespace bean::core {

// Maps a Mythic+ challenge map id or instance/map id to its dungeon name.
//
// Single source of truth: this table previously existed twice (once for
// building recording filenames, once for the recordings list) and the two
// copies were already drifting. Returns an empty string for unknown ids, in
// which case callers fall back to the name observed in the combat log.
//
// CHALLENGE_MODE_START carries both ids (instance then challenge map).
// CHALLENGE_MODE_END only carries the instance id. Lookup accepts either.
std::string DungeonNameForChallengeMap(int challengeMapId);

// Base Mythic+ timer in seconds for a challenge map id or instance/map id.
// Empty when the dungeon is not in the table — callers must not invent a
// timed/overtime result from the extra CHALLENGE_MODE_END floats, which are
// not remaining-time / timer-limit on retail 12.1+.
std::optional<int> MythicTimerLimitSeconds(int mapId);

} // namespace bean::core
