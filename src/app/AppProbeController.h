#pragma once

#include "app/AppContext.h"
#include "core/RecordingSizeEstimate.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct FfmpegProbeResult {
    bool runnable = false;
    std::optional<std::filesystem::path> executablePath;
};

struct FolderAvailabilityResult {
    std::uint64_t requestId = 0;
    bool outputAvailable = false;
    bool outputFolderWillBeCreatedOnRecordStart = false;
    bool wowLogAvailable = false;
};

struct DiskSpaceProbeResult {
    std::uint64_t requestId = 0;
    bean::core::DiskSpaceStatus status = bean::core::DiskSpaceStatus::Unknown;
    std::uint64_t availableBytes = 0;
    std::uint64_t estimatedRecordingBytes = 0;
    std::uint64_t warningThresholdBytes = 0;
};

struct RecordingReconciliationResult {
    std::uint64_t requestId = 0;
    std::size_t hashedCount = 0;
    std::size_t relocatedCount = 0;
    std::wstring error;
};

void RequestFolderAvailabilityRefresh(AppContext* ctx);
void BeginFolderAvailabilityProbe(AppContext* ctx);
void BeginRecordingReconciliation(AppContext* ctx);
void BeginDiskSpaceProbe(AppContext* ctx);
void BeginFfmpegProbe(AppContext* ctx);
