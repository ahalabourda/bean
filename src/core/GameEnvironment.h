#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace bean::core {

enum class WowEdition {
    Unknown,
    Retail,
    Ptr
};

const char* WowEditionLabel(WowEdition edition);

// Detection of everything bean needs from the machine it runs on: where WoW and
// OBS are installed, whether the game is configured for advanced combat
// logging, and whether a conflicting recorder is running.
//
// These live in core rather than the UI because they are domain rules, not
// presentation, and because keeping them here makes them testable. Every one is
// a pure query with no caching; callers own the polling policy.

// Local drive roots, C: first, since that is where installs usually are.
std::vector<std::filesystem::path> EnumerateDriveRoots();

// Honours the BEAN_OBS_ROOT override before scanning drives.
bool ResolveObsInstallRoot(std::filesystem::path& root);

// True only when the OBS install has everything the recorder actually needs:
// libobs, the ffmpeg muxer helper, plugins, data, and a graphics backend.
bool IsUsableObsInstallPresent();

// Best guess at the WoW installation directory when the user has not chosen one.
std::filesystem::path ResolveDefaultWowInstallDirectory();

// Resolves the game-mandated log path for an install and edition. Unknown uses
// Retail as the safe default. The returned path may not exist yet.
std::filesystem::path ResolveWowLogDirectoryFromInstallDirectory(
    const std::filesystem::path& installDirectory,
    WowEdition edition);

// Compatibility helper for older callers/configuration migration.
std::filesystem::path ResolveDefaultWowLogDirectory();

// "<install>/_retail_/Logs" or "<install>/_ptr_/Logs" -> "<install>". Empty
// when the path is not shaped like a WoW log directory.
std::optional<std::filesystem::path> ResolveWowInstallDirectoryFromLogDirectory(
    const std::filesystem::path& logDirectory);

// Reads Config.wtf to see whether advancedCombatLogging is on. Without it the
// combat log omits the fields the run detector depends on. Falls back to the
// auto-detected install when installDirectory is empty.
bool IsAdvancedCombatLoggingEnabled(
    const std::filesystem::path& installDirectory,
    WowEdition edition);

// Warcraft Recorder competes for the same game-capture hooks, so bean warns
// when it is running.
bool IsWarcraftRecorderRunning();

} // namespace bean::core
