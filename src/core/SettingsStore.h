#pragma once

#include "obs/IRecorderEngine.h"

#include <filesystem>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace bean::core {

struct Keybind {
    std::uint32_t modifiers = 0;
    std::uint32_t virtualKey = 0;

    bool IsBound() const
    {
        return virtualKey != 0;
    }
};

inline constexpr char kDefaultVideoEncoder[] = "gpu_auto";
inline constexpr char kDefaultEncoderPreset[] = "high";
inline constexpr char kDefaultVideoContainer[] = "mp4";
inline constexpr char kDefaultMicrophoneDeviceId[] = "default";
inline constexpr int kDefaultWindowWidth = 960;
inline constexpr int kDefaultWindowHeight = 560;
inline constexpr int kDefaultRecordingResolutionHeight = 0; // Full game resolution.
inline constexpr int kDefaultFps = 60;
inline constexpr int kDefaultPostRunStopDelaySeconds = 30;
inline constexpr int kDefaultClipDurationSeconds = 30;
inline constexpr Keybind kDefaultClipKeybind{6, 0x77};        // Ctrl+Shift+F8
inline constexpr Keybind kDefaultManualStartKeybind{6, 0x78}; // Ctrl+Shift+F9
inline constexpr Keybind kDefaultManualStopKeybind{6, 0x79};  // Ctrl+Shift+F10
inline constexpr bool kDefaultChatBlockerEnabled = true;
inline constexpr bool kDefaultChatBlockerUseCustomImage = false;
inline constexpr int kDefaultChatBlockerWidth = 500;
inline constexpr int kDefaultChatBlockerHeight = 300;

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
    std::string videoEncoder = kDefaultVideoEncoder;
    std::string encoderPreset = kDefaultEncoderPreset;
    std::string videoContainer = kDefaultVideoContainer;
    AudioCaptureScope audioCaptureScope = AudioCaptureScope::WowOnly;
    bool captureMicrophone = false;
    bool microphoneNoiseSuppression = false;
    std::string microphoneDeviceId = kDefaultMicrophoneDeviceId;
    int windowWidth = kDefaultWindowWidth;
    int windowHeight = kDefaultWindowHeight;
    int recordingResolutionHeight = kDefaultRecordingResolutionHeight;
    // Runtime-only WoW client dimensions. These are never persisted.
    int detectedWowClientWidth = 0;
    int detectedWowClientHeight = 0;
    int fps = kDefaultFps;
    int postRunStopDelaySeconds = kDefaultPostRunStopDelaySeconds;
    int clipDurationSeconds = kDefaultClipDurationSeconds;
    Keybind clipKeybind = kDefaultClipKeybind;
    Keybind manualStartKeybind = kDefaultManualStartKeybind;
    Keybind manualStopKeybind = kDefaultManualStopKeybind;
    bool chatBlockerEnabled = kDefaultChatBlockerEnabled;
    bool chatBlockerUseCustomImage = kDefaultChatBlockerUseCustomImage;
    std::filesystem::path chatBlockerCustomImagePath;
    int chatBlockerCustomImageSourceWidth = 0;
    int chatBlockerCustomImageSourceHeight = 0;
    std::unordered_map<std::string, std::pair<int, int>> chatBlockerCustomImageSizesByFileName;
    int chatBlockerWidth = kDefaultChatBlockerWidth;
    int chatBlockerHeight = kDefaultChatBlockerHeight;
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
