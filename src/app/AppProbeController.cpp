#include "app/AppProbeController.h"

#include "app/AppClips.h"
#include "app/AppLiveStatus.h"
#include "app/AppRecordingHelpers.h"
#include "app/AppUtilities.h"
#include "core/FileHash.h"
#include "core/RecordingSizeEstimate.h"
#include "util/Strings.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using bean::util::ToWide;

void RequestFolderAvailabilityRefresh(AppContext* ctx)
{
    if (!ctx || !ctx->mainWindow || ctx->shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    ++ctx->folderAvailabilityRequestId;
    SetTimer(ctx->mainWindow, kFolderAvailabilityTimerId, kFolderAvailabilityDebounceMs, nullptr);
}

void BeginFolderAvailabilityProbe(AppContext* ctx)
{
    if (!ctx
        || !ctx->mainWindow
        || ctx->shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    if (ctx->folderAvailabilityProbeInFlight.exchange(true)) {
        return;
    }

    const std::uint64_t requestId = ctx->folderAvailabilityRequestId;
    const std::wstring outputPath = GetWindowTextString(ctx->outputEdit);
    const std::wstring wowInstallPath = GetWindowTextString(ctx->wowLogEdit);
    if (!LaunchAppWorker(ctx, [ctx, requestId, outputPath, wowInstallPath]() {
        auto* result = new FolderAvailabilityResult();
        result->requestId = requestId;
        result->outputAvailable = DirectoryExists(outputPath);
        result->outputFolderWillBeCreatedOnRecordStart =
            !result->outputAvailable && !outputPath.empty();
        result->wowLogAvailable = DirectoryExists(wowInstallPath);
        if (!PostOwnedAppMessage(ctx, WM_BEAN_FOLDER_AVAILABILITY_COMPLETE, result)) {
            ctx->folderAvailabilityProbeInFlight.store(false, std::memory_order_release);
        }
    })) {
        ctx->folderAvailabilityProbeInFlight.store(false, std::memory_order_release);
    }
}

void BeginRecordingReconciliation(AppContext* ctx)
{
    if (!ctx
        || !ctx->mainWindow
        || ctx->shuttingDown.load(std::memory_order_acquire)
        || !ctx->runRepository) {
        return;
    }

    const std::uint64_t requestId = ++ctx->recordingReconciliationRequestId;
    if (ctx->recordingReconciliationInFlight.exchange(true)) {
        return;
    }

    const auto repository = ctx->runRepository;
    const auto currentOutput = ResolveRecordingsFolderPath(ctx);
    if (!LaunchAppWorker(ctx, [ctx, repository, currentOutput, requestId]() {
        auto* result = new RecordingReconciliationResult();
        result->requestId = requestId;

        std::string dbError;
        auto runs = repository->ListRuns(dbError);
        std::vector<std::filesystem::path> folders;
        AddKnownRecordingFolder(folders, currentOutput);
        for (const auto& run : runs) {
            AddKnownRecordingFolder(folders, run.videoPath.parent_path());
            for (const auto& alias : run.pathAliases) {
                AddKnownRecordingFolder(folders, alias.parent_path());
            }
        }

        const auto files = EnumerateRecordingMediaFilesInFolders(folders);
        std::unordered_map<std::string, std::size_t> runByPath;
        std::unordered_map<std::string, std::vector<std::size_t>> runsByFileName;
        std::unordered_map<std::string, std::vector<std::size_t>> runsByHash;
        std::vector<bool> runMatched(runs.size(), false);
        for (std::size_t index = 0; index < runs.size(); ++index) {
            runByPath[RecordingPathKey(runs[index].videoPath)] = index;
            for (const auto& alias : runs[index].pathAliases) {
                runByPath[RecordingPathKey(alias)] = index;
            }
            auto& pathNameCandidates = runsByFileName[RecordingFileNameKey(runs[index].videoPath)];
            pathNameCandidates.push_back(index);
            if (!runs[index].videoFileName.empty()) {
                auto& storedNameCandidates = runsByFileName[
                    RecordingFileNameKey(std::filesystem::path(runs[index].videoFileName))];
                if (std::find(storedNameCandidates.begin(), storedNameCandidates.end(), index)
                    == storedNameCandidates.end()) {
                    storedNameCandidates.push_back(index);
                }
            }
            if (runs[index].contentHash.has_value() && !runs[index].contentHash->empty()) {
                runsByHash[*runs[index].contentHash].push_back(index);
            }
        }

        std::unordered_map<std::string, std::string> fileHashes;
        const auto hashFile = [&fileHashes](const std::filesystem::path& path) -> std::optional<std::string> {
            const auto key = RecordingPathKey(path);
            const auto cached = fileHashes.find(key);
            if (cached != fileHashes.end()) {
                return cached->second;
            }
            std::string hashError;
            const auto hash = bean::core::ComputeFileSha256(path, hashError);
            if (hash.has_value()) {
                fileHashes.emplace(key, *hash);
            }
            return hash;
        };

        for (const auto& file : files) {
            const auto exact = runByPath.find(RecordingPathKey(file));
            if (exact == runByPath.end()) {
                continue;
            }
            const auto runIndex = exact->second;
            runMatched[runIndex] = true;
        }

        for (const auto& file : files) {
            if (runByPath.find(RecordingPathKey(file)) != runByPath.end()) {
                continue;
            }

            std::optional<std::size_t> matchedRun;
            const auto filenameIt = runsByFileName.find(RecordingFileNameKey(file));
            if (filenameIt != runsByFileName.end()) {
                std::vector<std::size_t> candidates;
                for (const auto runIndex : filenameIt->second) {
                    if (!runMatched[runIndex]) {
                        candidates.push_back(runIndex);
                    }
                }
                if (candidates.size() == 1) {
                    const auto runIndex = candidates.front();
                    if (runs[runIndex].contentHash.has_value()) {
                        if (const auto hash = hashFile(file); hash.has_value()
                            && *hash == *runs[runIndex].contentHash) {
                            matchedRun = runIndex;
                        }
                    } else {
                        matchedRun = runIndex;
                    }
                }
            }

            if (!matchedRun.has_value() && !runsByHash.empty()) {
                if (const auto hash = hashFile(file); hash.has_value()) {
                    const auto hashIt = runsByHash.find(*hash);
                    if (hashIt != runsByHash.end()) {
                        std::vector<std::size_t> candidates;
                        for (const auto runIndex : hashIt->second) {
                            if (!runMatched[runIndex]) {
                                candidates.push_back(runIndex);
                            }
                        }
                        if (candidates.size() == 1) {
                            matchedRun = candidates.front();
                        }
                    }
                }
            }

            if (!matchedRun.has_value()) {
                continue;
            }
            const auto runIndex = *matchedRun;
            std::string relocateError;
            if (repository->RelocateRun(runs[runIndex].videoPath, file, relocateError)) {
                runMatched[runIndex] = true;
                runByPath.erase(RecordingPathKey(runs[runIndex].videoPath));
                runByPath[RecordingPathKey(file)] = runIndex;
                runs[runIndex].pathAliases.push_back(runs[runIndex].videoPath);
                runs[runIndex].videoPath = file;
                ++result->relocatedCount;
            } else if (result->error.empty() && !relocateError.empty()) {
                result->error = ToWide(relocateError);
            }
        }

        if (!dbError.empty() && result->error.empty()) {
            result->error = ToWide(dbError);
        }
        const bool posted = PostOwnedAppMessage(
            ctx,
            WM_BEAN_RECORDING_RECONCILIATION_COMPLETE,
            result);
        ctx->recordingReconciliationInFlight.store(false, std::memory_order_release);

        // Seed hashes only after the relocation result has reached the UI.
        // This can read many large files, so it must never delay the list
        // refresh or make a tab appear unresponsive.
        for (const auto& file : files) {
            const auto exact = runByPath.find(RecordingPathKey(file));
            if (exact == runByPath.end()) {
                continue;
            }
            const auto runIndex = exact->second;
            if (runs[runIndex].contentHash.has_value()) {
                continue;
            }
            if (const auto hash = hashFile(file); hash.has_value()) {
                std::string hashError;
                repository->SetContentHash(runs[runIndex].videoPath, *hash, hashError);
            }
        }
        if (!posted) {
            ctx->recordingReconciliationInFlight.store(false, std::memory_order_release);
        }
    })) {
        ctx->recordingReconciliationInFlight.store(false, std::memory_order_release);
    }
}

void BeginDiskSpaceProbe(AppContext* ctx)
{
    if (!ctx
        || !ctx->mainWindow
        || ctx->shuttingDown.load(std::memory_order_acquire)) {
        return;
    }

    const std::uint64_t requestId = ++ctx->diskSpaceRequestId;
    const auto outputPath = ResolveRecordingsFolderPath(ctx);
    const auto recordingConfig = bean::core::ToRecordingConfig(ctx->settings);
    if (ctx->diskSpaceProbeInFlight.exchange(true)) {
        return;
    }

    if (!LaunchAppWorker(ctx, [ctx, requestId, outputPath, recordingConfig]() {
        auto* result = new DiskSpaceProbeResult();
        result->requestId = requestId;
        result->estimatedRecordingBytes = bean::core::EstimateTypicalRecordingBytes(recordingConfig);
        result->warningThresholdBytes =
            bean::core::LowDiskSpaceWarningThresholdBytes(recordingConfig);
        const auto available = bean::core::QueryAvailableDiskBytes(outputPath);
        result->status = bean::core::EvaluateDiskSpaceStatus(
            available,
            result->warningThresholdBytes);
        if (available.has_value()) {
            result->availableBytes = *available;
        }
        if (!PostOwnedAppMessage(ctx, WM_BEAN_DISK_SPACE_COMPLETE, result)) {
            ctx->diskSpaceProbeInFlight.store(false, std::memory_order_release);
        }
    })) {
        ctx->diskSpaceProbeInFlight.store(false, std::memory_order_release);
    }
}

void BeginFfmpegProbe(AppContext* ctx)
{
    if (!ctx
        || !ctx->mainWindow
        || ctx->shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    if (ctx->ffmpegProbeInFlight.exchange(true)) {
        return;
    }

    if (!LaunchAppWorker(ctx, [ctx]() {
        auto* result = new FfmpegProbeResult();
        // Resolve without the context: this thread must not touch AppContext.
        result->executablePath = ResolveFfmpegExecutablePath(nullptr);
        result->runnable = result->executablePath.has_value()
            && IsFfmpegExecutableRunnable(*result->executablePath);
        if (!PostOwnedAppMessage(ctx, WM_BEAN_FFMPEG_PROBE_COMPLETE, result)) {
            ctx->ffmpegProbeInFlight.store(false, std::memory_order_release);
        }
    })) {
        ctx->ffmpegProbeInFlight.store(false, std::memory_order_release);
    }
}

