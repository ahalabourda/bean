#include "core/SettingsStore.h"

#include "util/Json.h"
#include "util/Strings.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <vector>

namespace bean::core {
namespace {

using bean::util::EscapeJson;
using bean::util::Trim;

// Guards config.json across every SettingsStore instance in the process. The
// UI thread and the detached YouTube workers all persist settings, and several
// transient SettingsStore objects resolve to the same path, so this cannot be
// a per-instance member.
std::mutex& ConfigFileMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::string HexEncode(const BYTE* data, DWORD size)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(static_cast<size_t>(size) * 2);
    for (DWORD i = 0; i < size; ++i) {
        result.push_back(digits[(data[i] >> 4) & 0x0F]);
        result.push_back(digits[data[i] & 0x0F]);
    }
    return result;
}

std::optional<std::vector<BYTE>> HexDecode(const std::string& value)
{
    if (value.empty() || value.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<BYTE> result(value.size() / 2);
    for (size_t i = 0; i < result.size(); ++i) {
        const auto hexValue = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') {
                return ch - '0';
            }
            if (ch >= 'a' && ch <= 'f') {
                return ch - 'a' + 10;
            }
            if (ch >= 'A' && ch <= 'F') {
                return ch - 'A' + 10;
            }
            return -1;
        };
        const int high = hexValue(value[i * 2]);
        const int low = hexValue(value[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        result[i] = static_cast<BYTE>((high << 4) | low);
    }
    return result;
}

std::optional<std::string> ProtectRefreshToken(const std::string& value)
{
    if (value.empty()) {
        return std::string{};
    }
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(value.data()));
    input.cbData = static_cast<DWORD>(value.size());
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Bean YouTube refresh token", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return std::nullopt;
    }
    const std::string protectedValue = "dpapi:" + HexEncode(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return protectedValue;
}

std::optional<std::string> UnprotectRefreshToken(const std::string& value)
{
    if (value.rfind("dpapi:", 0) != 0) {
        return value;
    }
    const auto encrypted = HexDecode(value.substr(6));
    if (!encrypted.has_value()) {
        return std::nullopt;
    }
    DATA_BLOB input{};
    input.pbData = const_cast<BYTE*>(encrypted->data());
    input.cbData = static_cast<DWORD>(encrypted->size());
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return std::nullopt;
    }
    const std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

std::string NormalizeEncoderPreset(std::string preset)
{
    preset = Trim(preset);
    if (preset == "ultra" || preset == "high" || preset == "medium" || preset == "low" || preset == "minimum") {
        return preset;
    }
    // Migrate legacy names to the new quality tiers.
    if (preset == "quality") {
        return "high";
    }
    if (preset == "balanced") {
        return "medium";
    }
    if (preset == "speed") {
        return "low";
    }
    return "high";
}

namespace SettingsKeys {
constexpr char kSchemaVersion[] = "schemaVersion";
constexpr char kOutputDirectory[] = "outputDirectory";
constexpr char kWowLogDirectory[] = "wowLogDirectory";
constexpr char kEncoderPreset[] = "encoderPreset";
constexpr char kVideoEncoder[] = "videoEncoder";
constexpr char kVideoContainer[] = "videoContainer";
constexpr char kAudioCaptureScope[] = "audioCaptureScope";
constexpr char kCaptureWowProcessAudioOnly[] = "captureWowProcessAudioOnly";
constexpr char kCaptureMicrophone[] = "captureMicrophone";
constexpr char kMicrophoneNoiseSuppression[] = "microphoneNoiseSuppression";
constexpr char kMicrophoneDeviceId[] = "microphoneDeviceId";
constexpr char kWindowWidth[] = "windowWidth";
constexpr char kWindowHeight[] = "windowHeight";
constexpr char kRecordingResolutionHeight[] = "recordingResolutionHeight";
constexpr char kFps[] = "fps";
constexpr char kPostRunStopDelaySeconds[] = "postRunStopDelaySeconds";
constexpr char kClipDurationSeconds[] = "clipDurationSeconds";
constexpr char kClipKeybindModifiers[] = "clipKeybindModifiers";
constexpr char kClipKeybindVirtualKey[] = "clipKeybindVirtualKey";
constexpr char kManualStartKeybindModifiers[] = "manualStartKeybindModifiers";
constexpr char kManualStartKeybindVirtualKey[] = "manualStartKeybindVirtualKey";
constexpr char kManualStopKeybindModifiers[] = "manualStopKeybindModifiers";
constexpr char kManualStopKeybindVirtualKey[] = "manualStopKeybindVirtualKey";
constexpr char kChatBlockerEnabled[] = "chatBlockerEnabled";
constexpr char kChatBlockerUseCustomImage[] = "chatBlockerUseCustomImage";
constexpr char kChatBlockerCustomImagePath[] = "chatBlockerCustomImagePath";
constexpr char kChatBlockerCustomImageSourceWidth[] = "chatBlockerCustomImageSourceWidth";
constexpr char kChatBlockerCustomImageSourceHeight[] = "chatBlockerCustomImageSourceHeight";
constexpr char kChatBlockerCustomImageSizesByFileName[] = "chatBlockerCustomImageSizesByFileName";
constexpr char kChatBlockerWidth[] = "chatBlockerWidth";
constexpr char kChatBlockerHeight[] = "chatBlockerHeight";
constexpr char kChatBlockerAnchor[] = "chatBlockerAnchor";
constexpr char kYoutubeClientId[] = "youtubeClientId";
constexpr char kYoutubeRefreshToken[] = "youtubeRefreshToken";
constexpr char kYoutubeChannelId[] = "youtubeChannelId";
constexpr char kYoutubeChannelTitle[] = "youtubeChannelTitle";
}

// Thin aliases so the many call sites below stay readable.
std::string ReadQuoted(const std::string& content, const std::string& key)
{
    return util::ReadJsonString(content, key);
}

int ReadInt(const std::string& content, const std::string& key, int fallback)
{
    return util::ReadJsonInt(content, key, fallback);
}

bool ReadBool(const std::string& content, const std::string& key, bool fallback)
{
    return util::ReadJsonBool(content, key, fallback);
}

int ClampInt(int value, int minValue, int maxValue, int fallback)
{
    if (value < minValue || value > maxValue) {
        return fallback;
    }
    return value;
}

const char* ChatBlockerAnchorToString(AppSettings::ChatBlockerAnchor anchor)
{
    switch (anchor) {
    case AppSettings::ChatBlockerAnchor::BottomRight:
        return "bottom_right";
    case AppSettings::ChatBlockerAnchor::TopLeft:
        return "top_left";
    case AppSettings::ChatBlockerAnchor::TopRight:
        return "top_right";
    case AppSettings::ChatBlockerAnchor::BottomLeft:
    default:
        return "bottom_left";
    }
}

const char* AudioCaptureScopeToString(AppSettings::AudioCaptureScope scope)
{
    switch (scope) {
    case AppSettings::AudioCaptureScope::WowAndDiscord:
        return "wow_and_discord";
    case AppSettings::AudioCaptureScope::AllDesktop:
        return "all_desktop";
    case AppSettings::AudioCaptureScope::WowOnly:
    default:
        return "wow_only";
    }
}

AppSettings::AudioCaptureScope ParseAudioCaptureScope(const std::string& value)
{
    if (value == "wow_and_discord") {
        return AppSettings::AudioCaptureScope::WowAndDiscord;
    }
    if (value == "all_desktop") {
        return AppSettings::AudioCaptureScope::AllDesktop;
    }
    return AppSettings::AudioCaptureScope::WowOnly;
}

AppSettings::ChatBlockerAnchor ParseChatBlockerAnchor(const std::string& value)
{
    if (value == "bottom_right") {
        return AppSettings::ChatBlockerAnchor::BottomRight;
    }
    if (value == "top_left") {
        return AppSettings::ChatBlockerAnchor::TopLeft;
    }
    if (value == "top_right") {
        return AppSettings::ChatBlockerAnchor::TopRight;
    }
    return AppSettings::ChatBlockerAnchor::BottomLeft;
}

std::filesystem::path GetKnownFolderFromEnv(const char* name)
{
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr || len == 0) {
        if (value) {
            free(value);
        }
        return {};
    }

    std::filesystem::path out(value);
    free(value);
    return out;
}

std::string PercentEncodeToken(const std::string& input)
{
    std::ostringstream os;
    os << std::uppercase << std::hex;
    for (unsigned char ch : input) {
        const bool safe = (ch >= 'a' && ch <= 'z')
            || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9')
            || ch == '-'
            || ch == '_'
            || ch == '.';
        if (safe) {
            os << static_cast<char>(ch);
        } else {
            os << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return os.str();
}

std::string PercentDecodeToken(const std::string& input)
{
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '%' && i + 2 < input.size()) {
            const std::string hexByte = input.substr(i + 1, 2);
            try {
                output.push_back(static_cast<char>(std::stoi(hexByte, nullptr, 16)));
                i += 2;
                continue;
            } catch (...) {
            }
        }
        output.push_back(ch);
    }
    return output;
}

std::unordered_map<std::string, std::pair<int, int>> ParseChatBlockerImageSizesMap(const std::string& encoded)
{
    std::unordered_map<std::string, std::pair<int, int>> sizesByFileName;
    if (encoded.empty()) {
        return sizesByFileName;
    }
    size_t cursor = 0;
    while (cursor < encoded.size()) {
        const size_t entryEnd = encoded.find(';', cursor);
        const std::string entry = encoded.substr(cursor, entryEnd == std::string::npos ? std::string::npos : entryEnd - cursor);
        if (!entry.empty()) {
            const size_t separator = entry.find(':');
            if (separator != std::string::npos) {
                const std::string encodedFileName = entry.substr(0, separator);
                const std::string dimensions = entry.substr(separator + 1);
                const size_t xPos = dimensions.find('x');
                if (!encodedFileName.empty() && xPos != std::string::npos) {
                    try {
                        const int width = std::stoi(dimensions.substr(0, xPos));
                        const int height = std::stoi(dimensions.substr(xPos + 1));
                        if (width > 0 && height > 0) {
                            sizesByFileName[PercentDecodeToken(encodedFileName)] = std::make_pair(width, height);
                        }
                    } catch (...) {
                    }
                }
            }
        }
        if (entryEnd == std::string::npos) {
            break;
        }
        cursor = entryEnd + 1;
    }
    return sizesByFileName;
}

std::string SerializeChatBlockerImageSizesMap(const std::unordered_map<std::string, std::pair<int, int>>& sizesByFileName)
{
    std::ostringstream os;
    bool first = true;
    for (const auto& [fileName, dimensions] : sizesByFileName) {
        if (fileName.empty() || dimensions.first <= 0 || dimensions.second <= 0) {
            continue;
        }
        if (!first) {
            os << ';';
        }
        os << PercentEncodeToken(fileName) << ':' << dimensions.first << 'x' << dimensions.second;
        first = false;
    }
    return os.str();
}

} // namespace

const char* AudioCaptureScopeLabel(AppSettings::AudioCaptureScope scope)
{
    switch (scope) {
    case AppSettings::AudioCaptureScope::WowAndDiscord:
        return "wow+discord";
    case AppSettings::AudioCaptureScope::AllDesktop:
        return "all-desktop";
    case AppSettings::AudioCaptureScope::WowOnly:
    default:
        return "wow-only";
    }
}

SettingsStore::SettingsStore()
{
    std::filesystem::path base = GetKnownFolderFromEnv("APPDATA");
    if (base.empty()) {
        base = std::filesystem::temp_directory_path();
    }
    configPath_ = base / "Battle Encounter Archival Nexus" / "config.json";
}

std::filesystem::path SettingsStore::GetConfigPath() const
{
    return configPath_;
}

bool SettingsStore::Load(AppSettings& settings, std::string& error) const
{
    std::scoped_lock lock(ConfigFileMutex());
    return LoadLocked(settings, error);
}

bool SettingsStore::LoadLocked(AppSettings& settings, std::string& error) const
{
    error.clear();

    if (!std::filesystem::exists(configPath_)) {
        return true;
    }

    std::ifstream stream(configPath_);
    if (!stream.is_open()) {
        error = "Unable to open config file for read.";
        return false;
    }

    std::stringstream buffer;
    buffer << stream.rdbuf();
    const std::string content = buffer.str();

    // schemaVersion is informational today: missing keys mean version 0 (legacy).
    // Future migrations branch on the loaded version before applying field reads.
    const int loadedSchemaVersion = ReadInt(content, SettingsKeys::kSchemaVersion, 0);
    (void)loadedSchemaVersion;

    const auto output = ReadQuoted(content, SettingsKeys::kOutputDirectory);
    if (!output.empty()) {
        settings.outputDirectory = output;
    }
    const auto wowDir = ReadQuoted(content, SettingsKeys::kWowLogDirectory);
    if (!wowDir.empty()) {
        settings.wowLogDirectory = wowDir;
    }
    const auto preset = ReadQuoted(content, SettingsKeys::kEncoderPreset);
    settings.encoderPreset = NormalizeEncoderPreset(preset);
    const auto encoder = ReadQuoted(content, SettingsKeys::kVideoEncoder);
    if (encoder == "gpu_auto" || encoder == "qsv" || encoder == "nvenc" || encoder == "amf" || encoder == "x264") {
        settings.videoEncoder = encoder;
    }
    const auto container = ReadQuoted(content, SettingsKeys::kVideoContainer);
    if (container == "mp4" || container == "mkv") {
        settings.videoContainer = container;
    }
    const auto audioCaptureScope = ReadQuoted(content, SettingsKeys::kAudioCaptureScope);
    if (!audioCaptureScope.empty()) {
        settings.audioCaptureScope = ParseAudioCaptureScope(audioCaptureScope);
    } else {
        // Backward compatibility for older configs that only supported WoW-only/all-desktop.
        const bool wowOnly = ReadBool(content, SettingsKeys::kCaptureWowProcessAudioOnly, settings.audioCaptureScope == AppSettings::AudioCaptureScope::WowOnly);
        settings.audioCaptureScope = wowOnly ? AppSettings::AudioCaptureScope::WowOnly : AppSettings::AudioCaptureScope::AllDesktop;
    }
    settings.captureMicrophone = ReadBool(content, SettingsKeys::kCaptureMicrophone, settings.captureMicrophone);
    settings.microphoneNoiseSuppression = ReadBool(content, SettingsKeys::kMicrophoneNoiseSuppression, settings.microphoneNoiseSuppression);
    const auto microphoneDeviceId = ReadQuoted(content, SettingsKeys::kMicrophoneDeviceId);
    if (!microphoneDeviceId.empty()) {
        settings.microphoneDeviceId = microphoneDeviceId;
    }

    settings.windowWidth = ClampInt(ReadInt(content, SettingsKeys::kWindowWidth, settings.windowWidth), 930, 16384, kDefaultWindowWidth);
    settings.windowHeight = ClampInt(ReadInt(content, SettingsKeys::kWindowHeight, settings.windowHeight), 560, 16384, kDefaultWindowHeight);
    settings.recordingResolutionHeight = ReadInt(
        content,
        SettingsKeys::kRecordingResolutionHeight,
        settings.recordingResolutionHeight);
    if (settings.recordingResolutionHeight < 0 || settings.recordingResolutionHeight > 16384) {
        settings.recordingResolutionHeight = kDefaultRecordingResolutionHeight;
    }
    settings.fps = ReadInt(content, SettingsKeys::kFps, settings.fps);
    settings.postRunStopDelaySeconds = ClampInt(ReadInt(content, SettingsKeys::kPostRunStopDelaySeconds, settings.postRunStopDelaySeconds), 0, 600, 30);
    settings.clipDurationSeconds = ClampInt(ReadInt(content, SettingsKeys::kClipDurationSeconds, settings.clipDurationSeconds), 1, 3600, 30);
    settings.clipKeybind.modifiers = static_cast<std::uint32_t>(
        ClampInt(ReadInt(content, SettingsKeys::kClipKeybindModifiers, static_cast<int>(settings.clipKeybind.modifiers)), 0, 31, 6));
    settings.clipKeybind.virtualKey = static_cast<std::uint32_t>(
        ClampInt(ReadInt(content, SettingsKeys::kClipKeybindVirtualKey, static_cast<int>(settings.clipKeybind.virtualKey)), 0, 255, 0x77));
    settings.manualStartKeybind.modifiers = static_cast<std::uint32_t>(
        ClampInt(ReadInt(content, SettingsKeys::kManualStartKeybindModifiers, static_cast<int>(settings.manualStartKeybind.modifiers)), 0, 31, 6));
    settings.manualStartKeybind.virtualKey = static_cast<std::uint32_t>(
        ClampInt(ReadInt(content, SettingsKeys::kManualStartKeybindVirtualKey, static_cast<int>(settings.manualStartKeybind.virtualKey)), 0, 255, 0x78));
    settings.manualStopKeybind.modifiers = static_cast<std::uint32_t>(
        ClampInt(ReadInt(content, SettingsKeys::kManualStopKeybindModifiers, static_cast<int>(settings.manualStopKeybind.modifiers)), 0, 31, 6));
    settings.manualStopKeybind.virtualKey = static_cast<std::uint32_t>(
        ClampInt(ReadInt(content, SettingsKeys::kManualStopKeybindVirtualKey, static_cast<int>(settings.manualStopKeybind.virtualKey)), 0, 255, 0x79));
    settings.chatBlockerEnabled = ReadBool(content, SettingsKeys::kChatBlockerEnabled, settings.chatBlockerEnabled);
    settings.chatBlockerUseCustomImage = ReadBool(content, SettingsKeys::kChatBlockerUseCustomImage, settings.chatBlockerUseCustomImage);
    const auto chatBlockerCustomImagePath = ReadQuoted(content, SettingsKeys::kChatBlockerCustomImagePath);
    if (!chatBlockerCustomImagePath.empty()) {
        settings.chatBlockerCustomImagePath = chatBlockerCustomImagePath;
    }
    settings.chatBlockerCustomImageSourceWidth =
        ClampInt(ReadInt(content, SettingsKeys::kChatBlockerCustomImageSourceWidth, settings.chatBlockerCustomImageSourceWidth), 0, 16384, 0);
    settings.chatBlockerCustomImageSourceHeight =
        ClampInt(ReadInt(content, SettingsKeys::kChatBlockerCustomImageSourceHeight, settings.chatBlockerCustomImageSourceHeight), 0, 16384, 0);
    settings.chatBlockerCustomImageSizesByFileName =
        ParseChatBlockerImageSizesMap(ReadQuoted(content, SettingsKeys::kChatBlockerCustomImageSizesByFileName));
    settings.chatBlockerWidth = ClampInt(ReadInt(content, SettingsKeys::kChatBlockerWidth, settings.chatBlockerWidth), 0, 8192, 0);
    settings.chatBlockerHeight = ClampInt(ReadInt(content, SettingsKeys::kChatBlockerHeight, settings.chatBlockerHeight), 0, 8192, 0);
    settings.chatBlockerAnchor = ParseChatBlockerAnchor(ReadQuoted(content, SettingsKeys::kChatBlockerAnchor));
    settings.youtubeClientId = ReadQuoted(content, SettingsKeys::kYoutubeClientId);
    const auto rawRefreshToken = ReadQuoted(content, SettingsKeys::kYoutubeRefreshToken);
    const auto storedRefreshToken = UnprotectRefreshToken(rawRefreshToken);
    if (!storedRefreshToken.has_value()) {
        error = "Unable to decrypt the stored YouTube refresh token.";
        return false;
    }
    settings.youtubeRefreshToken = *storedRefreshToken;
    settings.youtubeChannelId = ReadQuoted(content, SettingsKeys::kYoutubeChannelId);
    settings.youtubeChannelTitle = ReadQuoted(content, SettingsKeys::kYoutubeChannelTitle);
    if (!rawRefreshToken.empty() && rawRefreshToken.rfind("dpapi:", 0) != 0) {
        std::string migrationError;
        // SaveLocked, not Save: the mutex is already held by our caller.
        if (!SaveLocked(settings, migrationError)) {
            error = "Unable to protect the stored YouTube refresh token: " + migrationError;
            return false;
        }
    }
    return true;
}

bool SettingsStore::Save(const AppSettings& settings, std::string& error) const
{
    std::scoped_lock lock(ConfigFileMutex());
    return SaveLocked(settings, error);
}

bool SettingsStore::SaveLocked(const AppSettings& settings, std::string& error) const
{
    error.clear();
    const auto protectedRefreshToken = ProtectRefreshToken(settings.youtubeRefreshToken);
    if (!protectedRefreshToken.has_value()) {
        error = "Unable to encrypt the YouTube refresh token.";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(configPath_.parent_path(), ec);
    if (ec) {
        error = "Unable to create config directory: " + ec.message();
        return false;
    }

    // Write to a sibling temp file and swap it in, so an interrupted write can
    // never leave a truncated config behind. This file holds the DPAPI-wrapped
    // YouTube refresh token, so losing it costs the user their account link.
    auto tempPath = configPath_;
    tempPath += L".tmp";

    std::ofstream stream(tempPath, std::ios::trunc | std::ios::binary);
    if (!stream.is_open()) {
        error = "Unable to open config file for write.";
        return false;
    }

    stream
        << "{\n"
        << "  \"" << SettingsKeys::kSchemaVersion << "\": " << kSettingsSchemaVersion << ",\n"
        << "  \"" << SettingsKeys::kOutputDirectory << "\": \"" << EscapeJson(settings.outputDirectory.string()) << "\",\n"
        << "  \"" << SettingsKeys::kWowLogDirectory << "\": \"" << EscapeJson(settings.wowLogDirectory.string()) << "\",\n"
        << "  \"" << SettingsKeys::kVideoEncoder << "\": \"" << EscapeJson(settings.videoEncoder) << "\",\n"
        << "  \"" << SettingsKeys::kEncoderPreset << "\": \"" << EscapeJson(settings.encoderPreset) << "\",\n"
        << "  \"" << SettingsKeys::kVideoContainer << "\": \"" << EscapeJson(settings.videoContainer) << "\",\n"
        << "  \"" << SettingsKeys::kAudioCaptureScope << "\": \"" << AudioCaptureScopeToString(settings.audioCaptureScope) << "\",\n"
        << "  \"" << SettingsKeys::kCaptureMicrophone << "\": " << (settings.captureMicrophone ? "true" : "false") << ",\n"
        << "  \"" << SettingsKeys::kMicrophoneNoiseSuppression << "\": " << (settings.microphoneNoiseSuppression ? "true" : "false") << ",\n"
        << "  \"" << SettingsKeys::kMicrophoneDeviceId << "\": \"" << EscapeJson(settings.microphoneDeviceId) << "\",\n"
        << "  \"" << SettingsKeys::kWindowWidth << "\": " << settings.windowWidth << ",\n"
        << "  \"" << SettingsKeys::kWindowHeight << "\": " << settings.windowHeight << ",\n"
        << "  \"" << SettingsKeys::kRecordingResolutionHeight << "\": " << settings.recordingResolutionHeight << ",\n"
        << "  \"" << SettingsKeys::kFps << "\": " << settings.fps << ",\n"
        << "  \"" << SettingsKeys::kPostRunStopDelaySeconds << "\": " << settings.postRunStopDelaySeconds << ",\n"
        << "  \"" << SettingsKeys::kClipDurationSeconds << "\": " << settings.clipDurationSeconds << ",\n"
        << "  \"" << SettingsKeys::kClipKeybindModifiers << "\": " << settings.clipKeybind.modifiers << ",\n"
        << "  \"" << SettingsKeys::kClipKeybindVirtualKey << "\": " << settings.clipKeybind.virtualKey << ",\n"
        << "  \"" << SettingsKeys::kManualStartKeybindModifiers << "\": " << settings.manualStartKeybind.modifiers << ",\n"
        << "  \"" << SettingsKeys::kManualStartKeybindVirtualKey << "\": " << settings.manualStartKeybind.virtualKey << ",\n"
        << "  \"" << SettingsKeys::kManualStopKeybindModifiers << "\": " << settings.manualStopKeybind.modifiers << ",\n"
        << "  \"" << SettingsKeys::kManualStopKeybindVirtualKey << "\": " << settings.manualStopKeybind.virtualKey << ",\n"
        << "  \"" << SettingsKeys::kChatBlockerEnabled << "\": " << (settings.chatBlockerEnabled ? "true" : "false") << ",\n"
        << "  \"" << SettingsKeys::kChatBlockerUseCustomImage << "\": " << (settings.chatBlockerUseCustomImage ? "true" : "false") << ",\n"
        << "  \"" << SettingsKeys::kChatBlockerCustomImagePath << "\": \"" << EscapeJson(settings.chatBlockerCustomImagePath.string()) << "\",\n"
        << "  \"" << SettingsKeys::kChatBlockerCustomImageSourceWidth << "\": " << settings.chatBlockerCustomImageSourceWidth << ",\n"
        << "  \"" << SettingsKeys::kChatBlockerCustomImageSourceHeight << "\": " << settings.chatBlockerCustomImageSourceHeight << ",\n"
        << "  \"" << SettingsKeys::kChatBlockerCustomImageSizesByFileName << "\": \"" << EscapeJson(SerializeChatBlockerImageSizesMap(settings.chatBlockerCustomImageSizesByFileName)) << "\",\n"
        << "  \"" << SettingsKeys::kChatBlockerWidth << "\": " << settings.chatBlockerWidth << ",\n"
        << "  \"" << SettingsKeys::kChatBlockerHeight << "\": " << settings.chatBlockerHeight << ",\n"
        << "  \"" << SettingsKeys::kChatBlockerAnchor << "\": \"" << ChatBlockerAnchorToString(settings.chatBlockerAnchor) << "\",\n"
        << "  \"" << SettingsKeys::kYoutubeClientId << "\": \"" << EscapeJson(settings.youtubeClientId) << "\",\n"
        << "  \"" << SettingsKeys::kYoutubeRefreshToken << "\": \"" << EscapeJson(*protectedRefreshToken) << "\",\n"
        << "  \"" << SettingsKeys::kYoutubeChannelId << "\": \"" << EscapeJson(settings.youtubeChannelId) << "\",\n"
        << "  \"" << SettingsKeys::kYoutubeChannelTitle << "\": \"" << EscapeJson(settings.youtubeChannelTitle) << "\"\n"
        << "}\n";

    stream.flush();
    if (!stream.good()) {
        stream.close();
        std::error_code removeEc;
        std::filesystem::remove(tempPath, removeEc);
        error = "Failed while writing config file.";
        return false;
    }
    stream.close();

    // Push the bytes to disk before the swap, otherwise a power loss can leave
    // the rename applied to a file whose contents never landed.
    const HANDLE tempHandle = CreateFileW(
        tempPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (tempHandle != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(tempHandle);
        CloseHandle(tempHandle);
    }

    // Keep the previous config as a .bak so a bad write is recoverable by hand.
    if (std::filesystem::exists(configPath_)) {
        auto backupPath = configPath_;
        backupPath += L".bak";
        std::error_code backupEc;
        std::filesystem::copy_file(
            configPath_, backupPath, std::filesystem::copy_options::overwrite_existing, backupEc);
    }

    if (!MoveFileExW(
            tempPath.c_str(),
            configPath_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        std::error_code removeEc;
        std::filesystem::remove(tempPath, removeEc);
        error = "Failed to replace config file (error " + std::to_string(moveError) + ").";
        return false;
    }

    return true;
}

obs::RecordingConfig ToRecordingConfig(const AppSettings& settings)
{
    obs::RecordingConfig config;
    config.outputDirectory = settings.outputDirectory;
    config.videoEncoder = settings.videoEncoder;
    config.encoderPreset = settings.encoderPreset;
    config.containerFormat = settings.videoContainer;
    switch (settings.audioCaptureScope) {
    case AppSettings::AudioCaptureScope::WowAndDiscord:
        config.audioCaptureScope = obs::RecordingConfig::AudioCaptureScope::WowAndDiscord;
        break;
    case AppSettings::AudioCaptureScope::AllDesktop:
        config.audioCaptureScope = obs::RecordingConfig::AudioCaptureScope::AllDesktop;
        break;
    case AppSettings::AudioCaptureScope::WowOnly:
    default:
        config.audioCaptureScope = obs::RecordingConfig::AudioCaptureScope::WowOnly;
        break;
    }
    config.captureMicrophone = settings.captureMicrophone;
    config.microphoneNoiseSuppression = settings.microphoneNoiseSuppression;
    config.microphoneDeviceId = settings.microphoneDeviceId;
    const int sourceWidth = settings.detectedWowClientWidth > 0 ? settings.detectedWowClientWidth : 1920;
    const int sourceHeight = settings.detectedWowClientHeight > 0 ? settings.detectedWowClientHeight : 1080;
    config.baseWidth = sourceWidth;
    config.baseHeight = sourceHeight;
    int outputWidth = sourceWidth;
    int outputHeight = sourceHeight;
    if (settings.recordingResolutionHeight > 0 && settings.recordingResolutionHeight < sourceHeight) {
        outputHeight = settings.recordingResolutionHeight;
        outputWidth = static_cast<int>(
            (static_cast<long long>(sourceWidth) * outputHeight + sourceHeight / 2) / sourceHeight);
        outputWidth = (std::max)(2, outputWidth & ~1);
        outputHeight = (std::max)(2, outputHeight & ~1);
    }
    config.width = outputWidth;
    config.height = outputHeight;
    config.fps = settings.fps;
    config.chatBlockerEnabled = settings.chatBlockerEnabled;
    config.chatBlockerUseCustomImage = settings.chatBlockerUseCustomImage;
    config.chatBlockerCustomImagePath = settings.chatBlockerCustomImagePath;
    config.chatBlockerCustomImageSourceWidth = settings.chatBlockerCustomImageSourceWidth;
    config.chatBlockerCustomImageSourceHeight = settings.chatBlockerCustomImageSourceHeight;
    config.chatBlockerWidth = settings.chatBlockerWidth;
    config.chatBlockerHeight = settings.chatBlockerHeight;
    switch (settings.chatBlockerAnchor) {
    case AppSettings::ChatBlockerAnchor::BottomRight:
        config.chatBlockerAnchor = obs::RecordingConfig::ChatBlockerAnchor::BottomRight;
        break;
    case AppSettings::ChatBlockerAnchor::TopLeft:
        config.chatBlockerAnchor = obs::RecordingConfig::ChatBlockerAnchor::TopLeft;
        break;
    case AppSettings::ChatBlockerAnchor::TopRight:
        config.chatBlockerAnchor = obs::RecordingConfig::ChatBlockerAnchor::TopRight;
        break;
    case AppSettings::ChatBlockerAnchor::BottomLeft:
    default:
        config.chatBlockerAnchor = obs::RecordingConfig::ChatBlockerAnchor::BottomLeft;
        break;
    }
    return config;
}

} // namespace bean::core
