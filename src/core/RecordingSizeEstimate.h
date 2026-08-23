#pragma once

#include "obs/IRecorderEngine.h"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace bean::core {

inline constexpr int kTypicalRecordingDurationMinutes = 35;
inline constexpr int kLowDiskSpaceHeadroomMultiplier = 3;
inline constexpr int kEstimatedAudioBitrateKbps = 160;

enum class DiskSpaceStatus {
    Unknown,
    Ok,
    Warning
};

std::uint64_t EstimateTypicalRecordingBytes(const obs::RecordingConfig& config);
std::uint64_t LowDiskSpaceWarningThresholdBytes(const obs::RecordingConfig& config);
DiskSpaceStatus EvaluateDiskSpaceStatus(
    std::optional<std::uint64_t> availableBytes,
    std::uint64_t warningThresholdBytes);
std::optional<std::uint64_t> QueryAvailableDiskBytes(const std::filesystem::path& path);

} // namespace bean::core
