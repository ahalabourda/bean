#pragma once

#include <string>

namespace bean::core {

// Maps a Mythic+ challenge map id to its dungeon name.
//
// Single source of truth: this table previously existed twice (once for
// building recording filenames, once for the recordings list) and the two
// copies were already drifting. Returns an empty string for unknown ids, in
// which case callers fall back to the name observed in the combat log.
std::string DungeonNameForChallengeMap(int challengeMapId);

} // namespace bean::core
