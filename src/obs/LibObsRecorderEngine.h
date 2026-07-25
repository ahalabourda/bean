#pragma once

#include "obs/IRecorderEngine.h"

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>

namespace bean::obs {

class LibObsRecorderEngine final : public IRecorderEngine {
public:
    ~LibObsRecorderEngine() override;

    bool Initialize(const RecordingConfig& config, std::string& error) override;
    bool StartRecording(const std::string& fileStem, std::string& error) override;
    bool StopRecording(std::string& error) override;
    bool SetMicrophoneNoiseSuppressionEnabled(bool enabled, std::string& error) override;
    bool IsRecording() const override;
    std::string GetLastStartDiagnostics() const override;

private:
    bool ResolveObsInstallRoot(std::filesystem::path& root) const;
    bool LoadObsApi(const std::filesystem::path& obsBinDir, std::string& error);
    bool InitializeObsCore(const std::filesystem::path& obsRoot, std::string& error);
    bool StartObsCore(const std::filesystem::path& obsRoot, std::string& error);
    bool ApplyVideoConfig(std::string& error);
    bool ApplyMicrophoneNoiseSuppressionFilter(bool enabled, std::string& error);
    bool SetupGameCaptureSource(std::string& error);
    bool SetupAudioSources(std::string& audioDebug, std::string& error);
    bool SetupMicrophoneSource(std::string& micDebug, std::string& error);
    bool SetupSceneAndChatBlocker(std::string& blockerDebug, std::string& error);
    bool StartOutputWithEncoders(
        const std::filesystem::path& outputPath,
        const std::string& audioDebug,
        const std::string& micDebug,
        const std::string& blockerDebug,
        std::string& error);
    void ReleaseObsObjects();
    void ShutdownObsCore();

    struct ObsApi;

    struct AppliedVideoConfig {
        uint32_t baseWidth = 0;
        uint32_t baseHeight = 0;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        uint32_t fps = 0;

        bool operator==(const AppliedVideoConfig&) const = default;
    };

    mutable std::mutex mutex_;
    RecordingConfig config_{};
    // coreStarted_ tracks the one-time obs_startup/module load. initialized_ is
    // the caller-visible "ready to record" flag and is recomputed on every
    // Initialize so that resolution and FPS changes are re-applied.
    bool coreStarted_ = false;
    AppliedVideoConfig appliedVideoConfig_{};
    bool initialized_ = false;
    // Atomic so IsRecording() can be polled from the UI thread without waiting
    // on mutex_ during encoder setup. The game-capture warmup sleep no longer
    // holds the mutex.
    std::atomic<bool> recording_{false};
    std::filesystem::path obsRoot_;
    std::filesystem::path obsBinDir_;
    HMODULE obsModule_ = nullptr;
    ObsApi* api_ = nullptr;
    void* scene_ = nullptr;
    void* gameCaptureSource_ = nullptr;
    void* chatBlockerSource_ = nullptr;
    void* desktopAudioSource_ = nullptr;
    void* discordAudioSource_ = nullptr;
    void* microphoneAudioSource_ = nullptr;
    void* output_ = nullptr;
    void* videoEncoder_ = nullptr;
    void* audioEncoder_ = nullptr;
    uint32_t videoWidth_ = 1920;
    uint32_t videoHeight_ = 1080;
    std::string lastStartDiagnostics_;
};

} // namespace bean::obs
