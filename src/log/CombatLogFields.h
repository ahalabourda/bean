#pragma once

#include <cstddef>

namespace bean::log {

// Combat-log CSV layouts as observed on retail 12.1+. Indices are relative to
// the event payload (after the "timestamp  " prefix), where field 0 is the
// event name. When Blizzard shifts a layout, bump the comment and the indices
// together — do not sprinkle magic numbers at call sites.

namespace ChallengeModeStartFields {
enum : std::size_t {
    Event = 0,
    DungeonName = 1,
    ChallengeModeId = 2,
    ChallengeMapId = 3,
    KeystoneLevel = 4,
    MinCount = 5
};
}

namespace ChallengeModeEndFields {
enum : std::size_t {
    Event = 0,
    ChallengeMapId = 1,
    Success = 2,
    KeystoneLevel = 3,
    TotalTimeMs = 4,
    OnTimeDeltaSeconds = 5,
    TimerLimitSeconds = 6,
    MinCount = 7
};
}

namespace CombatantInfoFields {
enum : std::size_t {
    Event = 0,
    Guid = 1,
    // Spec id has drifted between adjacent columns across builds; try primary
    // first, then the adjacent fallback when primary is missing/zero.
    SpecIdPrimary = 25,
    SpecIdFallback = 26,
    MinCount = 26
};
}

namespace CombatEventActorFields {
enum : std::size_t {
    // Standard layout: sourceGuid, sourceName, ..., destGuid, destName
    SourceGuid = 1,
    SourceName = 2,
    DestGuid = 5,
    DestName = 6,
    // hideCaster inserted after event name shifts source/dest by +1
    SourceGuidWithHideCaster = 2,
    SourceNameWithHideCaster = 3,
    DestGuidWithHideCaster = 6,
    DestNameWithHideCaster = 7
};
}

} // namespace bean::log
