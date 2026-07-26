#pragma once

namespace bean::core {

enum class OrchestratorState {
    Idle,
    Armed,
    Recording
};

enum class RecordingStartReason {
    Manual,
    MythicStart
};

enum class RecordingStopReason {
    Manual,
    Shutdown,
    CombatLogIdleTimeout,
    MythicSuccess,
    MythicFailure,
    MythicRestart
};

} // namespace bean::core
