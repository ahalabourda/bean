#include "core/RecordingSizeEstimate.h"

#include <algorithm>

namespace bean::core {

std::uint64_t EstimateTypicalRecordingBytes(const obs::RecordingConfig& config)
{
    const int videoKbps = (std::max)(1, obs::ResolveEstimatedVideoBitrateKbps(config));
    const int totalKbps = videoKbps + kEstimatedAudioBitrateKbps;
    const std::uint64_t seconds =
        static_cast<std::uint64_t>(kTypicalRecordingDurationMinutes) * 60ull;
    // kbps * 1000 bits/sec / 8 bits/byte = kbps * 125 bytes/sec.
    return static_cast<std::uint64_t>(totalKbps) * 125ull * seconds;
}

std::uint64_t LowDiskSpaceWarningThresholdBytes(const obs::RecordingConfig& config)
{
    return EstimateTypicalRecordingBytes(config)
        * static_cast<std::uint64_t>(kLowDiskSpaceHeadroomMultiplier);
}

DiskSpaceStatus EvaluateDiskSpaceStatus(
    std::optional<std::uint64_t> availableBytes,
    std::uint64_t warningThresholdBytes)
{
    if (!availableBytes.has_value()) {
        return DiskSpaceStatus::Unknown;
    }
    return *availableBytes < warningThresholdBytes
        ? DiskSpaceStatus::Warning
        : DiskSpaceStatus::Ok;
}

std::optional<std::uint64_t> QueryAvailableDiskBytes(const std::filesystem::path& path)
{
    if (path.empty()) {
        return std::nullopt;
    }

    std::error_code ec;
    auto current = std::filesystem::absolute(path, ec);
    if (ec) {
        current = path;
        ec.clear();
    }

    while (!current.empty()) {
        const auto info = std::filesystem::space(current, ec);
        if (!ec) {
            return static_cast<std::uint64_t>(info.available);
        }
        auto parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = std::move(parent);
        ec.clear();
    }
    return std::nullopt;
}

} // namespace bean::core
