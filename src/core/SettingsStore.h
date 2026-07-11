#pragma once

#include "obs/IRecorderEngine.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

namespace bean::core {

struct AppSettings {
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
    std::filesystem::path wowLogDirectory;
    std::string videoEncoder = "gpu_auto";
    std::string encoderPreset = "high";
    std::string videoContainer = "mp4";
    AudioCaptureScope audioCaptureScope = AudioCaptureScope::WowOnly;
    bool captureMicrophone = false;
    bool microphoneNoiseSuppression = false;
    std::string microphoneDeviceId = "default";
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int postRunStopDelaySeconds = 30;
    bool chatBlockerEnabled = true;
    bool chatBlockerUseCustomImage = false;
    std::filesystem::path chatBlockerCustomImagePath;
    int chatBlockerCustomImageSourceWidth = 0;
    int chatBlockerCustomImageSourceHeight = 0;
    std::unordered_map<std::string, std::pair<int, int>> chatBlockerCustomImageSizesByFileName;
    int chatBlockerWidth = 500;
    int chatBlockerHeight = 300;
    ChatBlockerAnchor chatBlockerAnchor = ChatBlockerAnchor::BottomLeft;
    std::string youtubeClientId;
    std::string youtubeRefreshToken;
    std::string youtubeChannelId;
    std::string youtubeChannelTitle;
};

class SettingsStore {
public:
    SettingsStore();

    std::filesystem::path GetConfigPath() const;

    bool Load(AppSettings& settings, std::string& error) const;
    bool Save(const AppSettings& settings, std::string& error) const;

private:
    std::filesystem::path configPath_;
};

obs::RecordingConfig ToRecordingConfig(const AppSettings& settings);

} // namespace bean::core
