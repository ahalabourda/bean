#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace bean::obs {

struct RecordingConfig {
    enum class ChatBlockerAnchor {
        BottomLeft,
        BottomRight,
        TopLeft,
        TopRight
    };
    enum class AudioCaptureScope {
        WowOnly,
        WowAndDiscord,
        AllDesktop
    };

    std::filesystem::path outputDirectory;
    std::string videoEncoder = "gpu_auto";
    std::string encoderPreset = "high";
    std::string containerFormat = "mkv";
    AudioCaptureScope audioCaptureScope = AudioCaptureScope::WowOnly;
    bool captureMicrophone = false;
    bool microphoneNoiseSuppression = false;
    std::string microphoneDeviceId = "default";
    int width = 1920;
    int height = 1080;
    int fps = 60;
    std::string captureWindowTitle = "World of Warcraft";
    bool chatBlockerEnabled = true;
    bool chatBlockerUseCustomImage = false;
    std::filesystem::path chatBlockerCustomImagePath;
    int chatBlockerCustomImageSourceWidth = 0;
    int chatBlockerCustomImageSourceHeight = 0;
    int chatBlockerWidth = 0;
    int chatBlockerHeight = 0;
    ChatBlockerAnchor chatBlockerAnchor = ChatBlockerAnchor::BottomLeft;
};

int ResolveConstantQualityValueForPreset(const std::string& encoderPreset);

class IRecorderEngine {
public:
    virtual ~IRecorderEngine() = default;

    virtual bool Initialize(const RecordingConfig& config, std::string& error) = 0;
    virtual bool StartRecording(const std::string& fileStem, std::string& error) = 0;
    virtual bool StopRecording(std::string& error) = 0;
    virtual bool SetMicrophoneNoiseSuppressionEnabled(bool enabled, std::string& error) = 0;
    virtual bool IsRecording() const = 0;
    virtual std::string GetLastStartDiagnostics() const = 0;
};

} // namespace bean::obs
