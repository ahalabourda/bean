#pragma once

#include "obs/IRecorderEngine.h"

#include <Windows.h>

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
    bool ApplyMicrophoneNoiseSuppressionFilter(bool enabled, std::string& error);
    void ReleaseObsObjects();
    void ShutdownObsCore();

    struct ObsApi;

    mutable std::mutex mutex_;
    RecordingConfig config_{};
    bool initialized_ = false;
    bool recording_ = false;
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
