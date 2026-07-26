#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace bean::core {

// Single source of truth for where a recording file lands on disk.
// Orchestrator metadata and the OBS engine must both call this so the
// database never points at a path the engine did not write.
std::filesystem::path BuildRecordingPath(
    const std::filesystem::path& outputDirectory,
    const std::string& fileStem,
    std::string_view containerFormat);

} // namespace bean::core
