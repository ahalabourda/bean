#include "core/SettingsStore.h"

#include <windows.h>
#include <wincrypt.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <vector>

namespace bean::core {
namespace {

std::string Trim(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
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

std::string EscapeJson(std::string value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::string UnescapeJson(const std::string& value)
{
    std::string unescaped;
    unescaped.reserve(value.size());

    bool escape = false;
    for (char ch : value) {
        if (escape) {
            switch (ch) {
            case '\\':
                unescaped.push_back('\\');
                break;
            case '"':
                unescaped.push_back('"');
                break;
            case 'n':
                unescaped.push_back('\n');
                break;
            case 'r':
                unescaped.push_back('\r');
                break;
            case 't':
                unescaped.push_back('\t');
                break;
            default:
                unescaped.push_back(ch);
                break;
            }
            escape = false;
            continue;
        }

        if (ch == '\\') {
            escape = true;
            continue;
        }
        unescaped.push_back(ch);
    }

    if (escape) {
        unescaped.push_back('\\');
    }
    return unescaped;
}

std::string ReadQuoted(const std::string& content, const std::string& key)
{
    const std::string marker = "\"" + key + "\"";
    const auto keyPos = content.find(marker);
    if (keyPos == std::string::npos) {
        return {};
    }
    const auto colonPos = content.find(':', keyPos + marker.size());
    if (colonPos == std::string::npos) {
        return {};
    }
    const auto firstQuote = content.find('"', colonPos + 1);
    if (firstQuote == std::string::npos) {
        return {};
    }
    size_t secondQuote = std::string::npos;
    bool escape = false;
    for (size_t i = firstQuote + 1; i < content.size(); ++i) {
        const char ch = content[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            secondQuote = i;
            break;
        }
    }
    if (secondQuote == std::string::npos) {
        return {};
    }
    return UnescapeJson(content.substr(firstQuote + 1, secondQuote - firstQuote - 1));
}

int ReadInt(const std::string& content, const std::string& key, int fallback)
{
    const std::string marker = "\"" + key + "\"";
    const auto keyPos = content.find(marker);
    if (keyPos == std::string::npos) {
        return fallback;
    }
    const auto colonPos = content.find(':', keyPos + marker.size());
    if (colonPos == std::string::npos) {
        return fallback;
    }
    auto valueStart = content.find_first_not_of(" \t\r\n", colonPos + 1);
    if (valueStart == std::string::npos) {
        return fallback;
    }
    auto valueEnd = content.find_first_of(",}\r\n", valueStart);
    if (valueEnd == std::string::npos) {
        valueEnd = content.size();
    }
    const std::string token = Trim(content.substr(valueStart, valueEnd - valueStart));
    try {
        return std::stoi(token);
    } catch (...) {
        return fallback;
    }
}

bool ReadBool(const std::string& content, const std::string& key, bool fallback)
{
    const std::string marker = "\"" + key + "\"";
    const auto keyPos = content.find(marker);
    if (keyPos == std::string::npos) {
        return fallback;
    }
    const auto colonPos = content.find(':', keyPos + marker.size());
    if (colonPos == std::string::npos) {
        return fallback;
    }
    auto valueStart = content.find_first_not_of(" \t\r\n", colonPos + 1);
    if (valueStart == std::string::npos) {
        return fallback;
    }
    auto valueEnd = content.find_first_of(",}\r\n", valueStart);
    if (valueEnd == std::string::npos) {
        valueEnd = content.size();
    }
    const std::string token = Trim(content.substr(valueStart, valueEnd - valueStart));
    if (token == "true") {
        return true;
    }
    if (token == "false") {
        return false;
    }
    return fallback;
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

    const auto output = ReadQuoted(content, "outputDirectory");
    if (!output.empty()) {
        settings.outputDirectory = output;
    }
    const auto wowDir = ReadQuoted(content, "wowLogDirectory");
    if (!wowDir.empty()) {
        settings.wowLogDirectory = wowDir;
    }
    const auto preset = ReadQuoted(content, "encoderPreset");
    settings.encoderPreset = NormalizeEncoderPreset(preset);
    const auto encoder = ReadQuoted(content, "videoEncoder");
    if (encoder == "gpu_auto" || encoder == "qsv" || encoder == "nvenc" || encoder == "amf" || encoder == "x264") {
        settings.videoEncoder = encoder;
    }
    const auto container = ReadQuoted(content, "videoContainer");
    if (container == "mp4" || container == "mkv") {
        settings.videoContainer = container;
    }
    const auto audioCaptureScope = ReadQuoted(content, "audioCaptureScope");
    if (!audioCaptureScope.empty()) {
        settings.audioCaptureScope = ParseAudioCaptureScope(audioCaptureScope);
    } else {
        // Backward compatibility for older configs that only supported WoW-only/all-desktop.
        const bool wowOnly = ReadBool(content, "captureWowProcessAudioOnly", settings.audioCaptureScope == AppSettings::AudioCaptureScope::WowOnly);
        settings.audioCaptureScope = wowOnly ? AppSettings::AudioCaptureScope::WowOnly : AppSettings::AudioCaptureScope::AllDesktop;
    }
    settings.captureMicrophone = ReadBool(content, "captureMicrophone", settings.captureMicrophone);
    settings.microphoneNoiseSuppression = ReadBool(content, "microphoneNoiseSuppression", settings.microphoneNoiseSuppression);
    const auto microphoneDeviceId = ReadQuoted(content, "microphoneDeviceId");
    if (!microphoneDeviceId.empty()) {
        settings.microphoneDeviceId = microphoneDeviceId;
    }

    settings.width = ReadInt(content, "width", settings.width);
    settings.height = ReadInt(content, "height", settings.height);
    settings.fps = ReadInt(content, "fps", settings.fps);
    settings.postRunStopDelaySeconds = ClampInt(ReadInt(content, "postRunStopDelaySeconds", settings.postRunStopDelaySeconds), 0, 600, 30);
    settings.chatBlockerEnabled = ReadBool(content, "chatBlockerEnabled", settings.chatBlockerEnabled);
    settings.chatBlockerUseCustomImage = ReadBool(content, "chatBlockerUseCustomImage", settings.chatBlockerUseCustomImage);
    const auto chatBlockerCustomImagePath = ReadQuoted(content, "chatBlockerCustomImagePath");
    if (!chatBlockerCustomImagePath.empty()) {
        settings.chatBlockerCustomImagePath = chatBlockerCustomImagePath;
    }
    settings.chatBlockerCustomImageSourceWidth =
        ClampInt(ReadInt(content, "chatBlockerCustomImageSourceWidth", settings.chatBlockerCustomImageSourceWidth), 0, 16384, 0);
    settings.chatBlockerCustomImageSourceHeight =
        ClampInt(ReadInt(content, "chatBlockerCustomImageSourceHeight", settings.chatBlockerCustomImageSourceHeight), 0, 16384, 0);
    settings.chatBlockerCustomImageSizesByFileName =
        ParseChatBlockerImageSizesMap(ReadQuoted(content, "chatBlockerCustomImageSizesByFileName"));
    settings.chatBlockerWidth = ClampInt(ReadInt(content, "chatBlockerWidth", settings.chatBlockerWidth), 0, 8192, 0);
    settings.chatBlockerHeight = ClampInt(ReadInt(content, "chatBlockerHeight", settings.chatBlockerHeight), 0, 8192, 0);
    settings.chatBlockerAnchor = ParseChatBlockerAnchor(ReadQuoted(content, "chatBlockerAnchor"));
    settings.youtubeClientId = ReadQuoted(content, "youtubeClientId");
    const auto rawRefreshToken = ReadQuoted(content, "youtubeRefreshToken");
    const auto storedRefreshToken = UnprotectRefreshToken(rawRefreshToken);
    if (!storedRefreshToken.has_value()) {
        error = "Unable to decrypt the stored YouTube refresh token.";
        return false;
    }
    settings.youtubeRefreshToken = *storedRefreshToken;
    settings.youtubeChannelId = ReadQuoted(content, "youtubeChannelId");
    settings.youtubeChannelTitle = ReadQuoted(content, "youtubeChannelTitle");
    if (!rawRefreshToken.empty() && rawRefreshToken.rfind("dpapi:", 0) != 0) {
        std::string migrationError;
        if (!Save(settings, migrationError)) {
            error = "Unable to protect the stored YouTube refresh token: " + migrationError;
            return false;
        }
    }
    return true;
}

bool SettingsStore::Save(const AppSettings& settings, std::string& error) const
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

    std::ofstream stream(configPath_, std::ios::trunc);
    if (!stream.is_open()) {
        error = "Unable to open config file for write.";
        return false;
    }

    stream
        << "{\n"
        << "  \"outputDirectory\": \"" << EscapeJson(settings.outputDirectory.string()) << "\",\n"
        << "  \"wowLogDirectory\": \"" << EscapeJson(settings.wowLogDirectory.string()) << "\",\n"
        << "  \"videoEncoder\": \"" << EscapeJson(settings.videoEncoder) << "\",\n"
        << "  \"encoderPreset\": \"" << EscapeJson(settings.encoderPreset) << "\",\n"
        << "  \"videoContainer\": \"" << EscapeJson(settings.videoContainer) << "\",\n"
        << "  \"audioCaptureScope\": \"" << AudioCaptureScopeToString(settings.audioCaptureScope) << "\",\n"
        << "  \"captureMicrophone\": " << (settings.captureMicrophone ? "true" : "false") << ",\n"
        << "  \"microphoneNoiseSuppression\": " << (settings.microphoneNoiseSuppression ? "true" : "false") << ",\n"
        << "  \"microphoneDeviceId\": \"" << EscapeJson(settings.microphoneDeviceId) << "\",\n"
        << "  \"width\": " << settings.width << ",\n"
        << "  \"height\": " << settings.height << ",\n"
        << "  \"fps\": " << settings.fps << ",\n"
        << "  \"postRunStopDelaySeconds\": " << settings.postRunStopDelaySeconds << ",\n"
        << "  \"chatBlockerEnabled\": " << (settings.chatBlockerEnabled ? "true" : "false") << ",\n"
        << "  \"chatBlockerUseCustomImage\": " << (settings.chatBlockerUseCustomImage ? "true" : "false") << ",\n"
        << "  \"chatBlockerCustomImagePath\": \"" << EscapeJson(settings.chatBlockerCustomImagePath.string()) << "\",\n"
        << "  \"chatBlockerCustomImageSourceWidth\": " << settings.chatBlockerCustomImageSourceWidth << ",\n"
        << "  \"chatBlockerCustomImageSourceHeight\": " << settings.chatBlockerCustomImageSourceHeight << ",\n"
        << "  \"chatBlockerCustomImageSizesByFileName\": \"" << EscapeJson(SerializeChatBlockerImageSizesMap(settings.chatBlockerCustomImageSizesByFileName)) << "\",\n"
        << "  \"chatBlockerWidth\": " << settings.chatBlockerWidth << ",\n"
        << "  \"chatBlockerHeight\": " << settings.chatBlockerHeight << ",\n"
        << "  \"chatBlockerAnchor\": \"" << ChatBlockerAnchorToString(settings.chatBlockerAnchor) << "\",\n"
        << "  \"youtubeClientId\": \"" << EscapeJson(settings.youtubeClientId) << "\",\n"
        << "  \"youtubeRefreshToken\": \"" << EscapeJson(*protectedRefreshToken) << "\",\n"
        << "  \"youtubeChannelId\": \"" << EscapeJson(settings.youtubeChannelId) << "\",\n"
        << "  \"youtubeChannelTitle\": \"" << EscapeJson(settings.youtubeChannelTitle) << "\"\n"
        << "}\n";

    if (!stream.good()) {
        error = "Failed while writing config file.";
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
    config.width = settings.width;
    config.height = settings.height;
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
