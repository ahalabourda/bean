#include "obs/LibObsRecorderEngine.h"

#include <Windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace bean::obs {
namespace {

constexpr auto kGameCaptureWarmupDelay = std::chrono::milliseconds(1500);
constexpr int kObsOrderMoveTop = 2;
constexpr int kBlockerImageSizePx = 8;
constexpr char kMicrophoneNoiseFilterName[] = "BeanMicrophoneNoiseSuppression";

#pragma comment(lib, "Gdiplus.lib")

std::mutex gObsLogMutex;
std::string gLastObsLogLine;
std::vector<std::string> gObsRecentLogLines;

void ObsLogHandler(int, const char* msg, va_list args, void*)
{
    if (!msg) {
        return;
    }
    char buffer[2048] = {};
    vsnprintf(buffer, sizeof(buffer), msg, args);
    std::scoped_lock lock(gObsLogMutex);
    gLastObsLogLine = buffer;
    gObsRecentLogLines.emplace_back(buffer);
    if (gObsRecentLogLines.size() > 24) {
        gObsRecentLogLines.erase(gObsRecentLogLines.begin());
    }
}

std::string GetLastObsLogLine()
{
    std::scoped_lock lock(gObsLogMutex);
    return gLastObsLogLine;
}

void ClearLastObsLogLine()
{
    std::scoped_lock lock(gObsLogMutex);
    gLastObsLogLine.clear();
    gObsRecentLogLines.clear();
}

std::string GetRecentObsLogSnippet()
{
    std::scoped_lock lock(gObsLogMutex);
    if (gObsRecentLogLines.empty()) {
        return {};
    }

    std::ostringstream os;
    int emitted = 0;
    for (auto it = gObsRecentLogLines.rbegin(); it != gObsRecentLogLines.rend() && emitted < 8; ++it) {
        if (it->empty()) {
            continue;
        }
        if (it->find("Freeing OBS context data") != std::string::npos) {
            continue;
        }
        if (it->find("Number of memory leaks") != std::string::npos) {
            continue;
        }
        if (emitted > 0) {
            os << " | ";
        }
        os << *it;
        ++emitted;
    }

    return os.str();
}

enum class VideoQualityTier {
    Ultra,
    High,
    Medium,
    Low,
    Minimum
};

VideoQualityTier ResolveVideoQualityTier(const RecordingConfig& config)
{
    if (config.encoderPreset == "ultra") {
        return VideoQualityTier::Ultra;
    }
    if (config.encoderPreset == "medium" || config.encoderPreset == "balanced") {
        return VideoQualityTier::Medium;
    }
    if (config.encoderPreset == "low" || config.encoderPreset == "speed") {
        return VideoQualityTier::Low;
    }
    if (config.encoderPreset == "minimum") {
        return VideoQualityTier::Minimum;
    }
    // Default "high", and migrate legacy "quality" into this tier.
    return VideoQualityTier::High;
}

int ResolveConstantQualityValue(const RecordingConfig& config)
{
    switch (ResolveVideoQualityTier(config)) {
    case VideoQualityTier::Ultra:
        return 20;
    case VideoQualityTier::High:
        return 24;
    case VideoQualityTier::Medium:
        return 28;
    case VideoQualityTier::Low:
        return 32;
    case VideoQualityTier::Minimum:
    default:
        return 40;
    }
}

const char* ResolveX264Preset(const RecordingConfig& config)
{
    switch (ResolveVideoQualityTier(config)) {
    case VideoQualityTier::Ultra:
        return "medium";
    case VideoQualityTier::High:
    case VideoQualityTier::Medium:
        return "veryfast";
    case VideoQualityTier::Low:
    case VideoQualityTier::Minimum:
    default:
        return "superfast";
    }
}

int ResolveFallbackBitrateKbps(const RecordingConfig& config)
{
    int baseBitrateKbps = 20000;
    switch (ResolveVideoQualityTier(config)) {
    case VideoQualityTier::Ultra:
        baseBitrateKbps = 30000;
        break;
    case VideoQualityTier::High:
        baseBitrateKbps = 20000;
        break;
    case VideoQualityTier::Medium:
        baseBitrateKbps = 12000;
        break;
    case VideoQualityTier::Low:
        baseBitrateKbps = 8000;
        break;
    case VideoQualityTier::Minimum:
        baseBitrateKbps = 6000;
        break;
    }

    const double widthScale = static_cast<double>((std::max)(1, config.width)) / 1920.0;
    const double heightScale = static_cast<double>((std::max)(1, config.height)) / 1080.0;
    const double fpsScale = static_cast<double>((std::max)(1, config.fps)) / 60.0;
    const double scaled = static_cast<double>(baseBitrateKbps) * widthScale * heightScale * fpsScale;
    const int clamped = static_cast<int>(scaled + 0.5);
    return (std::max)(3000, (std::min)(80000, clamped));
}

bool IsNvencEncoder(const std::string& encoderId)
{
    return encoderId == "jim_nvenc" || encoderId == "ffmpeg_nvenc";
}

bool IsAmfEncoder(const std::string& encoderId)
{
    return encoderId == "h264_texture_amf" || encoderId == "h264_ffmpeg_amf";
}

bool IsQsvEncoder(const std::string& encoderId)
{
    return encoderId == "obs_qsv11";
}

bool IsX264Encoder(const std::string& encoderId)
{
    return encoderId == "obs_x264";
}

bool SupportsQualityTarget(const std::string& encoderId)
{
    return IsNvencEncoder(encoderId) || IsAmfEncoder(encoderId) || IsQsvEncoder(encoderId) || IsX264Encoder(encoderId);
}

void ApplyVideoEncoderSettings(
    void* videoSettings,
    const std::string& encoderId,
    const RecordingConfig& config,
    bool useCbrFallback,
    void (*obs_data_set_int)(void*, const char*, long long),
    void (*obs_data_set_string)(void*, const char*, const char*))
{
    const int cqValue = ResolveConstantQualityValue(config);
    if (!useCbrFallback) {
        if (IsX264Encoder(encoderId)) {
            obs_data_set_string(videoSettings, "rate_control", "CRF");
            obs_data_set_int(videoSettings, "crf", cqValue);
            obs_data_set_string(videoSettings, "preset", ResolveX264Preset(config));
            return;
        }
        if (IsNvencEncoder(encoderId)) {
            obs_data_set_string(videoSettings, "rate_control", "CQP");
            obs_data_set_int(videoSettings, "cq", cqValue);
            obs_data_set_int(videoSettings, "cqp", cqValue);
            return;
        }
        if (IsAmfEncoder(encoderId)) {
            obs_data_set_string(videoSettings, "rate_control", "CQP");
            obs_data_set_int(videoSettings, "qp_i", cqValue);
            obs_data_set_int(videoSettings, "qp_p", cqValue);
            obs_data_set_int(videoSettings, "qp_b", cqValue);
            return;
        }
        if (IsQsvEncoder(encoderId)) {
            obs_data_set_string(videoSettings, "rate_control", "ICQ");
            obs_data_set_int(videoSettings, "icq_quality", cqValue);
            return;
        }
    }

    obs_data_set_int(videoSettings, "bitrate", ResolveFallbackBitrateKbps(config));
    obs_data_set_string(videoSettings, "rate_control", "CBR");
    if (IsX264Encoder(encoderId)) {
        obs_data_set_string(videoSettings, "preset", ResolveX264Preset(config));
    }
}

struct obs_vec2 {
    float x;
    float y;
};

obs_vec2 ResolveChatBlockerPosition(const RecordingConfig& config, uint32_t videoWidth, uint32_t videoHeight)
{
    const int blockerWidth = (std::max)(0, config.chatBlockerWidth);
    const int blockerHeight = (std::max)(0, config.chatBlockerHeight);
    const int xMax = (std::max)(0, static_cast<int>(videoWidth) - blockerWidth);
    const int yMax = (std::max)(0, static_cast<int>(videoHeight) - blockerHeight);

    int x = 0;
    int y = yMax;
    switch (config.chatBlockerAnchor) {
    case RecordingConfig::ChatBlockerAnchor::BottomRight:
        x = xMax;
        y = yMax;
        break;
    case RecordingConfig::ChatBlockerAnchor::TopLeft:
        x = 0;
        y = 0;
        break;
    case RecordingConfig::ChatBlockerAnchor::TopRight:
        x = xMax;
        y = 0;
        break;
    case RecordingConfig::ChatBlockerAnchor::BottomLeft:
    default:
        x = 0;
        y = yMax;
        break;
    }

    return {static_cast<float>(x), static_cast<float>(y)};
}

std::vector<const char*> BuildVideoEncoderCandidates(const std::string& selectedEncoder)
{
    if (selectedEncoder == "qsv") {
        return {"obs_qsv11"};
    }
    if (selectedEncoder == "nvenc") {
        return {"jim_nvenc", "ffmpeg_nvenc"};
    }
    if (selectedEncoder == "amf") {
        return {"h264_texture_amf", "h264_ffmpeg_amf"};
    }
    if (selectedEncoder == "x264") {
        return {"obs_x264"};
    }

    // Default to hardware-first auto mode, preferring discrete GPU encoders first.
    return {"jim_nvenc", "ffmpeg_nvenc", "h264_texture_amf", "h264_ffmpeg_amf", "obs_qsv11", "obs_x264"};
}

enum video_format {
    VIDEO_FORMAT_NONE,
    VIDEO_FORMAT_I420,
    VIDEO_FORMAT_NV12,
    VIDEO_FORMAT_YVYU,
    VIDEO_FORMAT_YUY2,
    VIDEO_FORMAT_UYVY,
    VIDEO_FORMAT_RGBA,
    VIDEO_FORMAT_BGRA
};

enum video_colorspace {
    VIDEO_CS_DEFAULT,
    VIDEO_CS_601,
    VIDEO_CS_709,
    VIDEO_CS_SRGB
};

enum video_range_type {
    VIDEO_RANGE_DEFAULT,
    VIDEO_RANGE_PARTIAL,
    VIDEO_RANGE_FULL
};

enum obs_scale_type {
    OBS_SCALE_DISABLE,
    OBS_SCALE_POINT,
    OBS_SCALE_BICUBIC,
    OBS_SCALE_BILINEAR,
    OBS_SCALE_LANCZOS,
    OBS_SCALE_AREA
};

enum speaker_layout {
    SPEAKERS_UNKNOWN,
    SPEAKERS_MONO,
    SPEAKERS_STEREO
};

struct obs_video_info {
    const char* graphics_module;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t base_width;
    uint32_t base_height;
    uint32_t output_width;
    uint32_t output_height;
    video_format output_format;
    uint32_t adapter;
    bool gpu_conversion;
    video_colorspace colorspace;
    video_range_type range;
    obs_scale_type scale_type;
};

struct obs_audio_info {
    uint32_t samples_per_sec;
    speaker_layout speakers;
};

std::string ToUtf8(const std::filesystem::path& path)
{
    const auto wide = path.wstring();
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), size, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

struct WowWindowSearchState {
    HWND wowWindow = nullptr;
    DWORD wowProcessId = 0;
    int bestScore = -1;
    std::wstring processPath;
    std::wstring windowTitle;
    std::wstring className;
};

bool IsLikelyWowGameClientExecutableName(std::wstring exeName)
{
    std::transform(exeName.begin(), exeName.end(), exeName.begin(), towlower);
    if (exeName.size() < 7 || exeName.substr(exeName.size() - 4) != L".exe") {
        return false;
    }
    if (exeName.rfind(L"wow", 0) != 0) {
        return false;
    }

    // Explicitly exclude helper/sidecar processes that are not the main game audio emitter.
    if (exeName.find(L"voice") != std::wstring::npos
        || exeName.find(L"proxy") != std::wstring::npos
        || exeName.find(L"launcher") != std::wstring::npos
        || exeName.find(L"browser") != std::wstring::npos
        || exeName.find(L"update") != std::wstring::npos
        || exeName.find(L"crash") != std::wstring::npos) {
        return false;
    }

    const std::wstring stem = exeName.substr(0, exeName.size() - 4);
    // Common game-client binary names.
    return stem == L"wow"
        || stem == L"wowclassic";
}

std::string ToUtf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

int GetGdiPlusEncoderClsid(const wchar_t* mimeType, CLSID& outClsid)
{
    using namespace Gdiplus;
    UINT num = 0;
    UINT size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0) {
        return -1;
    }

    std::vector<BYTE> buffer(size);
    auto* codecs = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(num, size, codecs) != Ok) {
        return -1;
    }

    for (UINT i = 0; i < num; ++i) {
        if (codecs[i].MimeType && wcscmp(codecs[i].MimeType, mimeType) == 0) {
            outClsid = codecs[i].Clsid;
            return static_cast<int>(i);
        }
    }

    return -1;
}

bool EnsureBlackPng(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
        return true;
    }

    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "Failed to create blocker image directory: " + ec.message();
        return false;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) != Gdiplus::Ok) {
        error = "Failed to initialize GDI+ for blocker image generation.";
        return false;
    }

    bool success = false;
    do {
        Gdiplus::Bitmap bitmap(kBlockerImageSizePx, kBlockerImageSizePx, PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(&bitmap);
        graphics.Clear(Gdiplus::Color(255, 0, 0, 0));

        CLSID pngClsid{};
        if (GetGdiPlusEncoderClsid(L"image/png", pngClsid) < 0) {
            error = "Failed to locate PNG encoder for blocker image generation.";
            break;
        }

        if (bitmap.Save(path.wstring().c_str(), &pngClsid, nullptr) != Gdiplus::Ok) {
            error = "Failed to save blocker PNG file.";
            break;
        }
        success = true;
    } while (false);

    Gdiplus::GdiplusShutdown(token);
    return success;
}

BOOL CALLBACK FindWowWindowProc(HWND hwnd, LPARAM lParam)
{
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) {
        return TRUE;
    }

    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!processHandle) {
        return TRUE;
    }

    wchar_t processPath[MAX_PATH] = {};
    DWORD processPathSize = static_cast<DWORD>(std::size(processPath));
    bool wowProcess = false;
    if (QueryFullProcessImageNameW(processHandle, 0, processPath, &processPathSize)) {
        wowProcess = IsLikelyWowGameClientExecutableName(std::filesystem::path(processPath).filename().wstring());
    }
    CloseHandle(processHandle);
    if (!wowProcess) {
        return TRUE;
    }

    wchar_t title[256] = {};
    const int titleLen = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const bool titleLooksLikeWow = (titleLen > 0 && wcsstr(title, L"World of Warcraft") != nullptr);
    const bool classLooksLikeWow = (_wcsicmp(className, L"GxWindowClass") == 0);

    int score = 0;
    if (classLooksLikeWow) {
        score += 2;
    }
    if (titleLooksLikeWow) {
        score += 1;
    }

    auto* state = reinterpret_cast<WowWindowSearchState*>(lParam);
    if (score > state->bestScore) {
        state->bestScore = score;
        state->wowWindow = hwnd;
        state->wowProcessId = processId;
        state->processPath = processPath;
        state->windowTitle = (titleLen > 0) ? std::wstring(title, static_cast<size_t>(titleLen)) : std::wstring();
        state->className = className;
    }

    // Best possible match found; no need to keep scanning.
    if (score >= 3) {
        return FALSE;
    }

    return TRUE;
}

bool DetectWowClientSize(uint32_t& outWidth, uint32_t& outHeight)
{
    WowWindowSearchState state;
    EnumWindows(FindWowWindowProc, reinterpret_cast<LPARAM>(&state));
    if (!state.wowWindow) {
        return false;
    }

    RECT client{};
    if (!GetClientRect(state.wowWindow, &client)) {
        return false;
    }

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    outWidth = static_cast<uint32_t>(width);
    outHeight = static_cast<uint32_t>(height);
    return true;
}

struct AudioCaptureBinding {
    DWORD processId = 0;
    std::wstring executablePathWide;
    std::string executablePath;
    std::string windowTitle;
    std::string windowClass;
};

struct ProcessWindowSearchState {
    std::wstring targetExecutableNameLower;
    bool requireVisibleWindow = true;
    DWORD processId = 0;
    int bestScore = -1;
    std::wstring processPath;
    std::wstring windowTitle;
    std::wstring className;
};

BOOL CALLBACK FindWindowByExecutableProc(HWND hwnd, LPARAM lParam)
{
    auto* state = reinterpret_cast<ProcessWindowSearchState*>(lParam);
    if (!state) {
        return TRUE;
    }
    if (state->requireVisibleWindow && !IsWindowVisible(hwnd)) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) {
        return TRUE;
    }

    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!processHandle) {
        return TRUE;
    }

    wchar_t processPath[MAX_PATH] = {};
    DWORD processPathSize = static_cast<DWORD>(std::size(processPath));
    if (!QueryFullProcessImageNameW(processHandle, 0, processPath, &processPathSize)) {
        CloseHandle(processHandle);
        return TRUE;
    }
    CloseHandle(processHandle);

    std::wstring exeName = std::filesystem::path(processPath).filename().wstring();
    std::transform(exeName.begin(), exeName.end(), exeName.begin(), towlower);
    if (exeName != state->targetExecutableNameLower) {
        return TRUE;
    }

    wchar_t title[256] = {};
    const int titleLen = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    wchar_t className[128] = {};
    const int classLen = GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));

    int score = 0;
    if (titleLen > 0) {
        score += 2;
    }
    if (classLen > 0) {
        score += 1;
    }
    if (state->processId == 0 || processId == state->processId) {
        score += 1;
    }
    if (score > state->bestScore) {
        state->bestScore = score;
        state->processId = processId;
        state->processPath = processPath;
        state->windowTitle = (titleLen > 0) ? std::wstring(title, static_cast<size_t>(titleLen)) : std::wstring();
        state->className = (classLen > 0) ? std::wstring(className, static_cast<size_t>(classLen)) : std::wstring();
    }

    if (score >= 4) {
        return FALSE;
    }
    return TRUE;
}

bool DetectAudioCaptureBindingByExecutableName(const std::wstring& executableName, AudioCaptureBinding& outBinding)
{
    if (executableName.empty()) {
        return false;
    }
    ProcessWindowSearchState state;
    state.targetExecutableNameLower = executableName;
    std::transform(state.targetExecutableNameLower.begin(), state.targetExecutableNameLower.end(), state.targetExecutableNameLower.begin(), towlower);
    EnumWindows(FindWindowByExecutableProc, reinterpret_cast<LPARAM>(&state));
    if (state.processId == 0) {
        return false;
    }

    outBinding.processId = state.processId;
    outBinding.executablePathWide = state.processPath;
    outBinding.executablePath = ToUtf8(state.processPath);
    outBinding.windowTitle = ToUtf8(state.windowTitle);
    outBinding.windowClass = ToUtf8(state.className);
    return true;
}

bool DetectWowAudioCaptureBinding(AudioCaptureBinding& outBinding)
{
    WowWindowSearchState state;
    EnumWindows(FindWowWindowProc, reinterpret_cast<LPARAM>(&state));
    if (state.wowProcessId == 0) {
        return false;
    }

    outBinding.processId = state.wowProcessId;
    outBinding.executablePathWide = state.processPath;
    outBinding.executablePath = ToUtf8(state.processPath);
    outBinding.windowTitle = ToUtf8(state.windowTitle);
    outBinding.windowClass = ToUtf8(state.className);
    return true;
}

bool DetectDiscordAudioCaptureBinding(AudioCaptureBinding& outBinding)
{
    return DetectAudioCaptureBindingByExecutableName(L"discord.exe", outBinding);
}

std::string BuildObsWasapiProcessWindowSetting(
    const AudioCaptureBinding& binding,
    const std::string& fallbackTitle,
    const std::string& fallbackClassName,
    const std::string& fallbackExeName)
{
    std::string exeName;
    if (!binding.executablePathWide.empty()) {
        exeName = ToUtf8(std::filesystem::path(binding.executablePathWide).filename().wstring());
    }
    if (exeName.empty()) {
        exeName = fallbackExeName;
    }

    std::string title = binding.windowTitle.empty() ? fallbackTitle : binding.windowTitle;
    std::string className = binding.windowClass.empty() ? fallbackClassName : binding.windowClass;
    return title + ":" + className + ":" + exeName;
}

std::filesystem::path GetCurrentProcessDirectory()
{
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(modulePath).parent_path();
}

std::filesystem::path GetEnvPath(const char* name)
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

std::vector<std::filesystem::path> EnumerateDriveRootsStartingAtC()
{
    std::vector<std::filesystem::path> roots;
    const DWORD logicalDrives = GetLogicalDrives();
    if (logicalDrives == 0) {
        roots.emplace_back(R"(C:\)");
        return roots;
    }

    const auto addDriveIfPresent = [&](wchar_t driveLetter) {
        const DWORD bit = 1u << (driveLetter - L'A');
        if ((logicalDrives & bit) != 0) {
            std::wstring root;
            root.push_back(driveLetter);
            root += L":\\";
            roots.emplace_back(std::move(root));
        }
    };

    for (wchar_t driveLetter = L'C'; driveLetter <= L'Z'; ++driveLetter) {
        addDriveIfPresent(driveLetter);
    }
    for (wchar_t driveLetter = L'A'; driveLetter < L'C'; ++driveLetter) {
        addDriveIfPresent(driveLetter);
    }

    if (roots.empty()) {
        roots.emplace_back(R"(C:\)");
    }
    return roots;
}

template <typename T>
bool LoadProc(HMODULE module, const char* name, T& out, std::string& error)
{
    const auto proc = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!proc) {
        error = std::string("Missing required libobs export: ") + name;
        return false;
    }
    out = proc;
    return true;
}

} // namespace

struct LibObsRecorderEngine::ObsApi {
    bool (*obs_startup)(const char*, const char*, void*) = nullptr;
    void (*obs_shutdown)() = nullptr;
    void (*base_set_log_handler)(void (*)(int, const char*, va_list, void*), void*) = nullptr;
    void (*obs_add_data_path)(const char*) = nullptr;
    bool (*obs_reset_audio)(const obs_audio_info*) = nullptr;
    int (*obs_reset_video)(obs_video_info*) = nullptr;
    void (*obs_add_module_path)(const char*, const char*) = nullptr;
    int (*obs_open_module)(void**, const char*, const char*) = nullptr;
    bool (*obs_init_module)(void*) = nullptr;
    void (*obs_post_load_modules)() = nullptr;
    void (*obs_log_loaded_modules)() = nullptr;

    void* (*obs_data_create)() = nullptr;
    void (*obs_data_release)(void*) = nullptr;
    void (*obs_data_set_string)(void*, const char*, const char*) = nullptr;
    void (*obs_data_set_int)(void*, const char*, long long) = nullptr;
    void (*obs_data_set_bool)(void*, const char*, bool) = nullptr;

    void* (*obs_source_create)(const char*, const char*, void*, void*) = nullptr;
    void (*obs_source_release)(void*) = nullptr;
    bool (*obs_source_filter_add)(void*, void*) = nullptr;
    void (*obs_source_filter_remove)(void*, void*) = nullptr;
    void* (*obs_source_get_filter_by_name)(void*, const char*) = nullptr;
    void (*obs_source_update)(void*, void*) = nullptr;
    void (*obs_set_output_source)(uint32_t, void*) = nullptr;

    void* (*obs_scene_create)(const char*) = nullptr;
    void (*obs_scene_release)(void*) = nullptr;
    void* (*obs_scene_add)(void*, void*) = nullptr;
    void* (*obs_scene_get_source)(const void*) = nullptr;
    void (*obs_sceneitem_set_pos)(void*, const obs_vec2*) = nullptr;
    void (*obs_sceneitem_set_scale)(void*, const obs_vec2*) = nullptr;
    void (*obs_sceneitem_set_order)(void*, int) = nullptr;
    void (*obs_sceneitem_set_visible)(void*, bool) = nullptr;

    void* (*obs_output_create)(const char*, const char*, void*, void*) = nullptr;
    void (*obs_output_release)(void*) = nullptr;
    bool (*obs_output_start)(void*) = nullptr;
    void (*obs_output_stop)(void*) = nullptr;
    const char* (*obs_output_get_last_error)(void*) = nullptr;

    void* (*obs_video_encoder_create)(const char*, const char*, void*, void*) = nullptr;
    void* (*obs_audio_encoder_create)(const char*, const char*, void*, size_t, void*) = nullptr;
    void (*obs_encoder_release)(void*) = nullptr;
    void (*obs_encoder_set_video)(void*, void*) = nullptr;
    void (*obs_encoder_set_audio)(void*, void*) = nullptr;
    void* (*obs_get_video)() = nullptr;
    void* (*obs_get_audio)() = nullptr;

    void (*obs_output_set_video_encoder)(void*, void*) = nullptr;
    void (*obs_output_set_audio_encoder)(void*, void*, size_t) = nullptr;
};

LibObsRecorderEngine::~LibObsRecorderEngine()
{
    std::scoped_lock lock(mutex_);
    if (recording_ && output_ && api_) {
        api_->obs_output_stop(output_);
    }
    if (api_) {
        api_->obs_set_output_source(0, nullptr);
    }
    ReleaseObsObjects();
    ShutdownObsCore();
}

bool LibObsRecorderEngine::Initialize(const RecordingConfig& config, std::string& error)
{
    std::scoped_lock lock(mutex_);
    config_ = config;
    lastStartDiagnostics_.clear();
    error.clear();

    std::error_code ec;
    std::filesystem::create_directories(config_.outputDirectory, ec);
    if (ec) {
        initialized_ = false;
        error = "Failed to create output directory: " + ec.message();
        return false;
    }

    std::filesystem::path root;
    if (!ResolveObsInstallRoot(root)) {
        initialized_ = false;
        error = "Could not locate OBS installation. Set BEAN_OBS_ROOT or install OBS to <drive>:\\Program Files\\obs-studio.";
        return false;
    }

    const auto desiredBin = root / "bin" / "64bit";
    if (!obsModule_ || desiredBin != obsBinDir_) {
        ReleaseObsObjects();
        ShutdownObsCore();
        if (!LoadObsApi(desiredBin, error)) {
            initialized_ = false;
            return false;
        }
    }

    if (!InitializeObsCore(root, error)) {
        initialized_ = false;
        return false;
    }

    initialized_ = true;
    return true;
}

bool LibObsRecorderEngine::StartRecording(const std::string& fileStem, std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();
    lastStartDiagnostics_.clear();
    ClearLastObsLogLine();

    if (!initialized_) {
        error = "libobs recorder is not initialized.";
        return false;
    }
    if (recording_) {
        error = "libobs recorder is already recording.";
        return false;
    }

    ReleaseObsObjects();

    AudioCaptureBinding wowWindowBinding;
    if (!DetectWowAudioCaptureBinding(wowWindowBinding)) {
        error = "No World of Warcraft window was detected for game capture.";
        return false;
    }
    const std::string wowWindowSelector = BuildObsWasapiProcessWindowSetting(
        wowWindowBinding,
        config_.captureWindowTitle.empty() ? "World of Warcraft" : config_.captureWindowTitle,
        "GxWindowClass",
        "wow.exe");

    auto* gameSettings = api_->obs_data_create();
    if (!gameSettings) {
        error = "Failed to create game capture settings.";
        return false;
    }
    api_->obs_data_set_string(gameSettings, "capture_mode", "window");
    api_->obs_data_set_bool(gameSettings, "capture_cursor", false);
    api_->obs_data_set_string(gameSettings, "window", wowWindowSelector.c_str());

    gameCaptureSource_ = api_->obs_source_create("game_capture", "BeanGameCapture", gameSettings, nullptr);
    api_->obs_data_release(gameSettings);
    if (!gameCaptureSource_) {
        error = "Failed to create game capture source. Ensure win-capture plugin is available.";
        return false;
    }

    std::string audioSourceDebug;
    const auto createProcessAudioSource = [&](const AudioCaptureBinding& binding, const char* sourceName, void*& outSource, std::string& createError, const std::string& fallbackTitle, const std::string& fallbackClass, const std::string& fallbackExeName) {
        auto* audioSettings = api_->obs_data_create();
        if (!audioSettings) {
            createError = "Failed to create process audio settings.";
            return false;
        }
        const std::string windowSetting = BuildObsWasapiProcessWindowSetting(binding, fallbackTitle, fallbackClass, fallbackExeName);
        api_->obs_data_set_string(audioSettings, "window", windowSetting.c_str());
        api_->obs_data_set_int(audioSettings, "priority", 2); // Match by executable.
        api_->obs_data_set_bool(audioSettings, "exclude", false);
        outSource = api_->obs_source_create("wasapi_process_output_capture", sourceName, audioSettings, nullptr);
        api_->obs_data_release(audioSettings);
        if (!outSource) {
            createError = "Failed to create process audio capture source.";
            return false;
        }
        return true;
    };

    if (config_.audioCaptureScope == RecordingConfig::AudioCaptureScope::WowOnly
        || config_.audioCaptureScope == RecordingConfig::AudioCaptureScope::WowAndDiscord) {
        const AudioCaptureBinding& audioBinding = wowWindowBinding;
        std::string sourceError;
        if (!createProcessAudioSource(audioBinding, "BeanWowAudio", desktopAudioSource_, sourceError, "World of Warcraft", "GxWindowClass", "wow.exe")) {
            error = "Failed to create WoW-only audio capture source. Verify OBS supports Application Audio Capture and try updating OBS.";
            if (!sourceError.empty()) {
                error += " Internal: " + sourceError;
            }
            const auto logSnippet = GetRecentObsLogSnippet();
            if (!logSnippet.empty()) {
                error += " OBS: " + logSnippet;
            }
            ReleaseObsObjects();
            return false;
        }
        std::ostringstream os;
        os << "WoW process capture (pid=" << audioBinding.processId;
        if (!audioBinding.executablePath.empty()) {
            os << ", exe='" << audioBinding.executablePath << "'";
        }
        if (!audioBinding.windowClass.empty()) {
            os << ", class='" << audioBinding.windowClass << "'";
        }
        if (!audioBinding.windowTitle.empty()) {
            os << ", title='" << audioBinding.windowTitle << "'";
        }
        os << ", windowSelector='" << wowWindowSelector << "'";
        os << ")";
        if (config_.audioCaptureScope == RecordingConfig::AudioCaptureScope::WowAndDiscord) {
            AudioCaptureBinding discordBinding;
            if (!DetectDiscordAudioCaptureBinding(discordBinding)) {
                error = "WoW + Discord audio capture is enabled, but Discord was not detected. Start Discord and try again.";
                ReleaseObsObjects();
                return false;
            }
            std::string discordSourceError;
            if (!createProcessAudioSource(discordBinding, "BeanDiscordAudio", discordAudioSource_, discordSourceError, "Discord", "Chrome_WidgetWin_1", "discord.exe")) {
                error = "Failed to create Discord process audio capture source. Verify OBS supports Application Audio Capture and try updating OBS.";
                if (!discordSourceError.empty()) {
                    error += " Internal: " + discordSourceError;
                }
                const auto logSnippet = GetRecentObsLogSnippet();
                if (!logSnippet.empty()) {
                    error += " OBS: " + logSnippet;
                }
                ReleaseObsObjects();
                return false;
            }
            os << " + Discord process capture (pid=" << discordBinding.processId;
            if (!discordBinding.executablePath.empty()) {
                os << ", exe='" << discordBinding.executablePath << "'";
            }
            if (!discordBinding.windowClass.empty()) {
                os << ", class='" << discordBinding.windowClass << "'";
            }
            if (!discordBinding.windowTitle.empty()) {
                os << ", title='" << discordBinding.windowTitle << "'";
            }
            const std::string discordWindowSelector = BuildObsWasapiProcessWindowSetting(discordBinding, "Discord", "Chrome_WidgetWin_1", "discord.exe");
            os << ", windowSelector='" << discordWindowSelector << "'";
            os << ")";
        }
        audioSourceDebug = os.str();
    } else {
        auto* audioSettings = api_->obs_data_create();
        if (!audioSettings) {
            error = "Failed to create desktop audio settings.";
            return false;
        }
        api_->obs_data_set_string(audioSettings, "device_id", "default");
        desktopAudioSource_ = api_->obs_source_create("wasapi_output_capture", "BeanDesktopAudio", audioSettings, nullptr);
        api_->obs_data_release(audioSettings);
        audioSourceDebug = "Desktop output capture (device='default')";
    }

    std::string micSourceDebug = "disabled";
    if (config_.captureMicrophone) {
        auto* micSettings = api_->obs_data_create();
        if (!micSettings) {
            error = "Failed to create microphone settings.";
            ReleaseObsObjects();
            return false;
        }
        const std::string microphoneDeviceId = config_.microphoneDeviceId.empty() ? "default" : config_.microphoneDeviceId;
        api_->obs_data_set_string(micSettings, "device_id", microphoneDeviceId.c_str());
        microphoneAudioSource_ = api_->obs_source_create("wasapi_input_capture", "BeanMicrophone", micSettings, nullptr);
        api_->obs_data_release(micSettings);
        if (!microphoneAudioSource_) {
            error = "Failed to create microphone audio source.";
            const auto logSnippet = GetRecentObsLogSnippet();
            if (!logSnippet.empty()) {
                error += " OBS: " + logSnippet;
            }
            ReleaseObsObjects();
            return false;
        }
        std::string filterError;
        if (!ApplyMicrophoneNoiseSuppressionFilter(config_.microphoneNoiseSuppression, filterError)) {
            error = "Failed to apply microphone noise suppression filter: " + filterError;
            ReleaseObsObjects();
            return false;
        }
        micSourceDebug = "enabled (device='" + microphoneDeviceId + "')";
        micSourceDebug += config_.microphoneNoiseSuppression ? ", noise-suppression=enabled" : ", noise-suppression=disabled";
    }

    std::string blockerDebug = "disabled";
    void* blockerItem = nullptr;

    scene_ = api_->obs_scene_create("BeanScene");
    if (!scene_) {
        error = "Failed to create OBS scene.";
        ReleaseObsObjects();
        return false;
    }
    api_->obs_scene_add(scene_, gameCaptureSource_);
    if (config_.chatBlockerEnabled && config_.chatBlockerWidth > 0 && config_.chatBlockerHeight > 0) {
        auto* blockerSettings = api_->obs_data_create();
        if (!blockerSettings) {
            error = "Failed to create chat blocker settings.";
            ReleaseObsObjects();
            return false;
        }
        std::filesystem::path blockerImagePath;
        int sourceImageWidth = kBlockerImageSizePx;
        int sourceImageHeight = kBlockerImageSizePx;
        if (config_.chatBlockerUseCustomImage) {
            blockerImagePath = config_.chatBlockerCustomImagePath;
            sourceImageWidth = (std::max)(1, config_.chatBlockerCustomImageSourceWidth);
            sourceImageHeight = (std::max)(1, config_.chatBlockerCustomImageSourceHeight);
            std::error_code customImageEc;
            if (blockerImagePath.empty() || !std::filesystem::exists(blockerImagePath, customImageEc) || customImageEc) {
                api_->obs_data_release(blockerSettings);
                error = "Custom chat blocker image is missing. Pick a new image in Chat Privacy settings.";
                ReleaseObsObjects();
                return false;
            }
        } else {
            std::string blockerImageError;
            blockerImagePath = std::filesystem::temp_directory_path() / "bean-chat-blocker.png";
            if (!EnsureBlackPng(blockerImagePath, blockerImageError)) {
                api_->obs_data_release(blockerSettings);
                error = blockerImageError.empty() ? "Failed to create blocker image file." : blockerImageError;
                ReleaseObsObjects();
                return false;
            }
        }
        const std::string blockerImagePathUtf8 = ToUtf8(blockerImagePath);
        api_->obs_data_set_string(blockerSettings, "file", blockerImagePathUtf8.c_str());
        api_->obs_data_set_bool(blockerSettings, "unload", false);
        chatBlockerSource_ = api_->obs_source_create("image_source", "BeanChatBlocker", blockerSettings, nullptr);
        const std::string blockerSourceId = "image_source";
        api_->obs_data_release(blockerSettings);
        if (!chatBlockerSource_) {
            error = "Failed to create chat blocker image source.";
            ReleaseObsObjects();
            return false;
        }
        blockerItem = api_->obs_scene_add(scene_, chatBlockerSource_);
        if (blockerItem && api_->obs_sceneitem_set_pos) {
            const auto blockerPos = ResolveChatBlockerPosition(config_, videoWidth_, videoHeight_);
            api_->obs_sceneitem_set_pos(blockerItem, &blockerPos);
        }
        if (blockerItem && api_->obs_sceneitem_set_scale) {
            const obs_vec2 blockerScale{
                static_cast<float>(config_.chatBlockerWidth) / static_cast<float>((std::max)(1, sourceImageWidth)),
                static_cast<float>(config_.chatBlockerHeight) / static_cast<float>((std::max)(1, sourceImageHeight))};
            api_->obs_sceneitem_set_scale(blockerItem, &blockerScale);
        }
        if (blockerItem && api_->obs_sceneitem_set_visible) {
            api_->obs_sceneitem_set_visible(blockerItem, true);
        }
        std::ostringstream blockerOs;
        blockerOs << "enabled(" << blockerSourceId << ", " << config_.chatBlockerWidth << "x" << config_.chatBlockerHeight
                  << ", mode=" << (config_.chatBlockerUseCustomImage ? "custom" : "blank")
                  << ", anchor=";
        switch (config_.chatBlockerAnchor) {
        case RecordingConfig::ChatBlockerAnchor::BottomRight:
            blockerOs << "bottom-right";
            break;
        case RecordingConfig::ChatBlockerAnchor::TopLeft:
            blockerOs << "top-left";
            break;
        case RecordingConfig::ChatBlockerAnchor::TopRight:
            blockerOs << "top-right";
            break;
        case RecordingConfig::ChatBlockerAnchor::BottomLeft:
        default:
            blockerOs << "bottom-left";
            break;
        }
        blockerOs << ", item=" << (blockerItem ? "ok" : "null") << ")";
        blockerDebug = blockerOs.str();
    } else if (!config_.chatBlockerEnabled) {
        blockerDebug = "disabled(toggle)";
    } else {
        blockerDebug = "disabled(size<=0)";
    }
    if (desktopAudioSource_) {
        api_->obs_scene_add(scene_, desktopAudioSource_);
    }
    if (discordAudioSource_) {
        api_->obs_scene_add(scene_, discordAudioSource_);
    }
    if (microphoneAudioSource_) {
        api_->obs_scene_add(scene_, microphoneAudioSource_);
    }
    if (blockerItem && api_->obs_sceneitem_set_order) {
        api_->obs_sceneitem_set_order(blockerItem, kObsOrderMoveTop);
    }

    void* sceneSource = api_->obs_scene_get_source(scene_);
    if (!sceneSource) {
        error = "Failed to get source from scene.";
        ReleaseObsObjects();
        return false;
    }
    api_->obs_set_output_source(0, sceneSource);
    std::this_thread::sleep_for(kGameCaptureWarmupDelay);

    const bool useMp4 = (config_.containerFormat == "mp4");
    const auto extension = useMp4 ? ".mp4" : ".mkv";
    const auto outputPath = config_.outputDirectory / (fileStem + extension);
    auto* outputSettings = api_->obs_data_create();
    if (!outputSettings) {
        error = "Failed to allocate output settings.";
        ReleaseObsObjects();
        return false;
    }
    const auto outputPathUtf8 = ToUtf8(outputPath);
    api_->obs_data_set_string(outputSettings, "path", outputPathUtf8.c_str());
    api_->obs_data_set_string(outputSettings, "format_name", useMp4 ? "mp4" : "matroska");
    output_ = api_->obs_output_create("ffmpeg_muxer", "BeanRecordingOutput", outputSettings, nullptr);
    api_->obs_data_release(outputSettings);
    if (!output_) {
        error = "Failed to create ffmpeg_muxer output. Ensure obs-ffmpeg plugin is available.";
        const auto logSnippet = GetRecentObsLogSnippet();
        if (!logSnippet.empty()) {
            error += " OBS: " + logSnippet;
        }
        ReleaseObsObjects();
        return false;
    }

    const auto candidates = BuildVideoEncoderCandidates(config_.videoEncoder);
    std::string attemptedEncoders;
    std::string encoderFailures;

    auto* audioEncSettings = api_->obs_data_create();
    if (!audioEncSettings) {
        error = "Failed to allocate audio encoder settings.";
        ReleaseObsObjects();
        return false;
    }
    api_->obs_data_set_int(audioEncSettings, "bitrate", 160);
    audioEncoder_ = api_->obs_audio_encoder_create("ffmpeg_aac", "BeanAudioEncoder", audioEncSettings, 0, nullptr);
    api_->obs_data_release(audioEncSettings);
    if (!audioEncoder_) {
        error = "Failed to create ffmpeg_aac audio encoder.";
        const auto logSnippet = GetRecentObsLogSnippet();
        if (!logSnippet.empty()) {
            error += " OBS: " + logSnippet;
        }
        ReleaseObsObjects();
        return false;
    }

    api_->obs_encoder_set_audio(audioEncoder_, api_->obs_get_audio());
    api_->obs_output_set_audio_encoder(output_, audioEncoder_, 0);
    for (const char* encoderIdRaw : candidates) {
        if (!encoderIdRaw) {
            continue;
        }
        const std::string encoderId = encoderIdRaw;
        const bool supportsQualityTarget = SupportsQualityTarget(encoderId);
        const int attemptCount = supportsQualityTarget ? 2 : 1;
        for (int attemptIndex = 0; attemptIndex < attemptCount; ++attemptIndex) {
            const bool useCbrFallback = supportsQualityTarget && attemptIndex == 1;
            if (!attemptedEncoders.empty()) {
                attemptedEncoders += ", ";
            }
            attemptedEncoders += encoderId + (useCbrFallback ? " (cbr fallback)" : " (quality target)");

            if (videoEncoder_) {
                api_->obs_encoder_release(videoEncoder_);
                videoEncoder_ = nullptr;
            }

            auto* videoSettings = api_->obs_data_create();
            if (!videoSettings) {
                error = "Failed to allocate video encoder settings.";
                ReleaseObsObjects();
                return false;
            }
            ApplyVideoEncoderSettings(
                videoSettings,
                encoderId,
                config_,
                useCbrFallback,
                api_->obs_data_set_int,
                api_->obs_data_set_string);
            videoEncoder_ = api_->obs_video_encoder_create(encoderId.c_str(), "BeanVideoEncoder", videoSettings, nullptr);
            api_->obs_data_release(videoSettings);

            if (!videoEncoder_) {
                if (!encoderFailures.empty()) {
                    encoderFailures += " | ";
                }
                encoderFailures += encoderId + (useCbrFallback ? " (cbr fallback)" : " (quality target)") + ": create failed";
                continue;
            }

            api_->obs_encoder_set_video(videoEncoder_, api_->obs_get_video());
            api_->obs_output_set_video_encoder(output_, videoEncoder_);
            if (api_->obs_output_start(output_)) {
                recording_ = true;
                lastStartDiagnostics_ = "audio=" + audioSourceDebug
                    + ", microphone=" + micSourceDebug
                    + ", container=" + config_.containerFormat
                    + ", blocker=" + blockerDebug;
                return true;
            }

            std::string attemptError;
            if (api_->obs_output_get_last_error) {
                const char* outputError = api_->obs_output_get_last_error(output_);
                if (outputError && *outputError) {
                    attemptError = outputError;
                }
            }
            if (attemptError.empty()) {
                attemptError = GetLastObsLogLine();
            }
            if (attemptError.empty()) {
                attemptError = "start failed";
            }
            if (!encoderFailures.empty()) {
                encoderFailures += " | ";
            }
            encoderFailures += encoderId + (useCbrFallback ? " (cbr fallback)" : " (quality target)") + ": " + attemptError;
        }
    }

    error = "OBS failed to start output with encoder(s): " + attemptedEncoders + ".";
    if (!encoderFailures.empty()) {
        error += " Attempts: " + encoderFailures + ".";
    }
    const auto logSnippet = GetRecentObsLogSnippet();
    if (!logSnippet.empty()) {
        error += " OBS: " + logSnippet;
    }
    ReleaseObsObjects();
    return false;
}

bool LibObsRecorderEngine::StopRecording(std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();

    if (!recording_) {
        error = "libobs recorder is not currently recording.";
        return false;
    }

    if (output_) {
        api_->obs_output_stop(output_);
    }
    api_->obs_set_output_source(0, nullptr);
    ReleaseObsObjects();
    recording_ = false;
    return true;
}

bool LibObsRecorderEngine::SetMicrophoneNoiseSuppressionEnabled(bool enabled, std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();
    config_.microphoneNoiseSuppression = enabled;

    if (!recording_) {
        return true;
    }
    if (!microphoneAudioSource_) {
        error = "No active microphone source is available for live filter updates.";
        return false;
    }
    return ApplyMicrophoneNoiseSuppressionFilter(enabled, error);
}

bool LibObsRecorderEngine::IsRecording() const
{
    std::scoped_lock lock(mutex_);
    return recording_;
}

std::string LibObsRecorderEngine::GetLastStartDiagnostics() const
{
    std::scoped_lock lock(mutex_);
    return lastStartDiagnostics_;
}

bool LibObsRecorderEngine::ResolveObsInstallRoot(std::filesystem::path& root) const
{
    const auto envRoot = GetEnvPath("BEAN_OBS_ROOT");
    if (!envRoot.empty() && std::filesystem::exists(envRoot / "bin" / "64bit" / "obs.dll")) {
        root = envRoot;
        return true;
    }

    for (const auto& driveRoot : EnumerateDriveRootsStartingAtC()) {
        const std::filesystem::path candidates[] = {
            driveRoot / "Program Files" / "obs-studio",
            driveRoot / "Program Files (x86)" / "obs-studio"
        };
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate / "bin" / "64bit" / "obs.dll")) {
                root = candidate;
                return true;
            }
        }
    }
    return false;
}

bool LibObsRecorderEngine::LoadObsApi(const std::filesystem::path& obsBinDir, std::string& error)
{
    error.clear();
    obsBinDir_ = obsBinDir;
    const auto obsDllPath = obsBinDir_ / "obs.dll";
    obsModule_ = LoadLibraryExW(obsDllPath.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!obsModule_) {
        error = "Failed to load obs.dll from: " + obsDllPath.string();
        return false;
    }

    auto* api = new ObsApi();
    if (!LoadProc(obsModule_, "obs_startup", api->obs_startup, error) ||
        !LoadProc(obsModule_, "obs_shutdown", api->obs_shutdown, error) ||
        !LoadProc(obsModule_, "base_set_log_handler", api->base_set_log_handler, error) ||
        !LoadProc(obsModule_, "obs_add_data_path", api->obs_add_data_path, error) ||
        !LoadProc(obsModule_, "obs_reset_video", api->obs_reset_video, error) ||
        !LoadProc(obsModule_, "obs_reset_audio", api->obs_reset_audio, error) ||
        !LoadProc(obsModule_, "obs_add_module_path", api->obs_add_module_path, error) ||
        !LoadProc(obsModule_, "obs_open_module", api->obs_open_module, error) ||
        !LoadProc(obsModule_, "obs_init_module", api->obs_init_module, error) ||
        !LoadProc(obsModule_, "obs_post_load_modules", api->obs_post_load_modules, error) ||
        !LoadProc(obsModule_, "obs_log_loaded_modules", api->obs_log_loaded_modules, error) ||
        !LoadProc(obsModule_, "obs_data_create", api->obs_data_create, error) ||
        !LoadProc(obsModule_, "obs_data_release", api->obs_data_release, error) ||
        !LoadProc(obsModule_, "obs_data_set_string", api->obs_data_set_string, error) ||
        !LoadProc(obsModule_, "obs_data_set_int", api->obs_data_set_int, error) ||
        !LoadProc(obsModule_, "obs_data_set_bool", api->obs_data_set_bool, error) ||
        !LoadProc(obsModule_, "obs_source_create", api->obs_source_create, error) ||
        !LoadProc(obsModule_, "obs_source_release", api->obs_source_release, error) ||
        !LoadProc(obsModule_, "obs_set_output_source", api->obs_set_output_source, error) ||
        !LoadProc(obsModule_, "obs_scene_create", api->obs_scene_create, error) ||
        !LoadProc(obsModule_, "obs_scene_release", api->obs_scene_release, error) ||
        !LoadProc(obsModule_, "obs_scene_add", api->obs_scene_add, error) ||
        !LoadProc(obsModule_, "obs_scene_get_source", api->obs_scene_get_source, error) ||
        !LoadProc(obsModule_, "obs_output_create", api->obs_output_create, error) ||
        !LoadProc(obsModule_, "obs_output_release", api->obs_output_release, error) ||
        !LoadProc(obsModule_, "obs_output_start", api->obs_output_start, error) ||
        !LoadProc(obsModule_, "obs_output_stop", api->obs_output_stop, error) ||
        !LoadProc(obsModule_, "obs_output_get_last_error", api->obs_output_get_last_error, error) ||
        !LoadProc(obsModule_, "obs_video_encoder_create", api->obs_video_encoder_create, error) ||
        !LoadProc(obsModule_, "obs_audio_encoder_create", api->obs_audio_encoder_create, error) ||
        !LoadProc(obsModule_, "obs_encoder_release", api->obs_encoder_release, error) ||
        !LoadProc(obsModule_, "obs_encoder_set_video", api->obs_encoder_set_video, error) ||
        !LoadProc(obsModule_, "obs_encoder_set_audio", api->obs_encoder_set_audio, error) ||
        !LoadProc(obsModule_, "obs_get_video", api->obs_get_video, error) ||
        !LoadProc(obsModule_, "obs_get_audio", api->obs_get_audio, error) ||
        !LoadProc(obsModule_, "obs_output_set_video_encoder", api->obs_output_set_video_encoder, error) ||
        !LoadProc(obsModule_, "obs_output_set_audio_encoder", api->obs_output_set_audio_encoder, error)) {
        delete api;
        FreeLibrary(obsModule_);
        obsModule_ = nullptr;
        return false;
    }

    api->obs_source_filter_add = reinterpret_cast<decltype(api->obs_source_filter_add)>(GetProcAddress(obsModule_, "obs_source_filter_add"));
    api->obs_source_filter_remove = reinterpret_cast<decltype(api->obs_source_filter_remove)>(GetProcAddress(obsModule_, "obs_source_filter_remove"));
    api->obs_source_get_filter_by_name = reinterpret_cast<decltype(api->obs_source_get_filter_by_name)>(GetProcAddress(obsModule_, "obs_source_get_filter_by_name"));
    api->obs_source_update = reinterpret_cast<decltype(api->obs_source_update)>(GetProcAddress(obsModule_, "obs_source_update"));
    api->obs_sceneitem_set_pos = reinterpret_cast<decltype(api->obs_sceneitem_set_pos)>(GetProcAddress(obsModule_, "obs_sceneitem_set_pos"));
    api->obs_sceneitem_set_scale = reinterpret_cast<decltype(api->obs_sceneitem_set_scale)>(GetProcAddress(obsModule_, "obs_sceneitem_set_scale"));
    api->obs_sceneitem_set_order = reinterpret_cast<decltype(api->obs_sceneitem_set_order)>(GetProcAddress(obsModule_, "obs_sceneitem_set_order"));
    api->obs_sceneitem_set_visible = reinterpret_cast<decltype(api->obs_sceneitem_set_visible)>(GetProcAddress(obsModule_, "obs_sceneitem_set_visible"));
    api_ = api;
    return true;
}

bool LibObsRecorderEngine::InitializeObsCore(const std::filesystem::path& obsRoot, std::string& error)
{
    error.clear();
    if (initialized_) {
        return true;
    }

    ClearLastObsLogLine();
    obsRoot_ = obsRoot;
    SetDllDirectoryW(obsBinDir_.wstring().c_str());

    const auto pluginsBin = ToUtf8(obsRoot_ / "obs-plugins" / "64bit");
    const auto pluginsData = ToUtf8(obsRoot_ / "data" / "obs-plugins" / "%module%");
    std::string libobsData = ToUtf8(obsRoot_ / "data" / "libobs");
    if (!libobsData.empty() && libobsData.back() != '/' && libobsData.back() != '\\') {
        libobsData.push_back('/');
    }
    const auto moduleConfigPath = ToUtf8(std::filesystem::temp_directory_path() / "bean-obs-config");
    std::filesystem::create_directories(std::filesystem::temp_directory_path() / "bean-obs-config");

    api_->base_set_log_handler(ObsLogHandler, nullptr);
    if (!api_->obs_startup("en-US", moduleConfigPath.c_str(), nullptr)) {
        error = "obs_startup failed.";
        const auto logSnippet = GetRecentObsLogSnippet();
        if (!logSnippet.empty()) {
            error += " OBS: " + logSnippet;
        }
        return false;
    }

    api_->obs_add_data_path(libobsData.c_str());

    // Ensure plugin dependency DLLs (avcodec, Qt, etc) are discoverable when libobs loads modules.
    const auto obsBinUtf8 = ToUtf8(obsBinDir_);
    const auto currentPath = GetEnvPath("PATH").string();
    if (!obsBinUtf8.empty()) {
        std::string mergedPath = obsBinUtf8;
        if (!currentPath.empty()) {
            mergedPath += ";";
            mergedPath += currentPath;
        }
        SetEnvironmentVariableA("PATH", mergedPath.c_str());
    }

    // ffmpeg_muxer resolves helper path relative to host executable.
    // Ensure helper exists next to bean.exe.
    const auto helperSource = obsBinDir_ / "obs-ffmpeg-mux.exe";
    const auto helperTarget = GetCurrentProcessDirectory() / "obs-ffmpeg-mux.exe";
    if (std::filesystem::exists(helperSource)) {
        std::error_code copyEc;
        std::filesystem::copy_file(helperSource, helperTarget, std::filesystem::copy_options::overwrite_existing, copyEc);
        if (copyEc) {
            const auto logSnippet = GetRecentObsLogSnippet();
            std::ostringstream os;
            os << "Failed to stage ffmpeg helper executable: " << copyEc.message();
            if (!logSnippet.empty()) {
                os << " OBS: " << logSnippet;
            }
            error = os.str();
            api_->obs_shutdown();
            return false;
        }
    }

    api_->obs_add_module_path(pluginsBin.c_str(), pluginsData.c_str());

    const std::filesystem::path pluginBinDir = obsRoot_ / "obs-plugins" / "64bit";
    struct ModuleSpec {
        const char* dllName;
        const char* dataDirName;
    };
    const ModuleSpec requiredModules[] = {
        {"obs-outputs.dll", "obs-outputs"},
        {"obs-ffmpeg.dll", "obs-ffmpeg"},
        {"obs-x264.dll", "obs-x264"},
        {"image-source.dll", "image-source"},
        {"win-capture.dll", "win-capture"},
        {"win-wasapi.dll", "win-wasapi"}
    };

    for (const auto& module : requiredModules) {
        const auto modulePath = pluginBinDir / module.dllName;
        if (!std::filesystem::exists(modulePath)) {
            continue;
        }

        const auto modulePathUtf8 = ToUtf8(modulePath);
        const auto moduleDataPathUtf8 = ToUtf8(obsRoot_ / "data" / "obs-plugins" / module.dataDirName);
        void* moduleHandle = nullptr;
        const int openResult = api_->obs_open_module(&moduleHandle, modulePathUtf8.c_str(), moduleDataPathUtf8.c_str());
        if (openResult != 0 || !moduleHandle) {
            const auto logSnippet = GetRecentObsLogSnippet();
            std::ostringstream os;
            os << "Failed to open OBS module '" << module.dllName << "' (code " << openResult << ").";
            if (!logSnippet.empty()) {
                os << " OBS: " << logSnippet;
            }
            error = os.str();
            api_->obs_shutdown();
            return false;
        }
        if (!api_->obs_init_module(moduleHandle)) {
            const auto logSnippet = GetRecentObsLogSnippet();
            std::ostringstream os;
            os << "Failed to init OBS module '" << module.dllName << "'.";
            if (!logSnippet.empty()) {
                os << " OBS: " << logSnippet;
            }
            error = os.str();
            api_->obs_shutdown();
            return false;
        }
    }

    api_->obs_post_load_modules();
    api_->obs_log_loaded_modules();

    const auto d3d11Path = obsBinDir_ / "libobs-d3d11.dll";
    const auto openglPath = obsBinDir_ / "libobs-opengl.dll";

    uint32_t width = static_cast<uint32_t>(config_.width > 0 ? config_.width : 1920);
    uint32_t height = static_cast<uint32_t>(config_.height > 0 ? config_.height : 1080);
    DetectWowClientSize(width, height);
    videoWidth_ = width;
    videoHeight_ = height;
    const uint32_t fps = static_cast<uint32_t>(config_.fps > 0 ? config_.fps : 60);

    struct VideoAttempt {
        const char* moduleName;
        std::filesystem::path dllPath;
        video_format format;
        bool gpuConversion;
        video_colorspace colorspace;
        video_range_type range;
        obs_scale_type scaleType;
    };

    const VideoAttempt attempts[] = {
        {"libobs-d3d11", d3d11Path, VIDEO_FORMAT_NV12, true, VIDEO_CS_709, VIDEO_RANGE_PARTIAL, OBS_SCALE_BICUBIC},
        {"libobs-d3d11", d3d11Path, VIDEO_FORMAT_BGRA, false, VIDEO_CS_SRGB, VIDEO_RANGE_FULL, OBS_SCALE_BILINEAR},
        {"libobs-opengl", openglPath, VIDEO_FORMAT_BGRA, false, VIDEO_CS_SRGB, VIDEO_RANGE_FULL, OBS_SCALE_BILINEAR}
    };

    int videoResetResult = -9999;

    for (const auto& attempt : attempts) {
        if (!std::filesystem::exists(attempt.dllPath)) {
            continue;
        }

        HMODULE graphicsPreload = LoadLibraryExW(attempt.dllPath.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

        obs_video_info vi{};
        vi.graphics_module = attempt.moduleName;
        vi.fps_num = fps;
        vi.fps_den = 1;
        vi.base_width = width;
        vi.base_height = height;
        vi.output_width = width;
        vi.output_height = height;
        vi.output_format = attempt.format;
        vi.adapter = 0;
        vi.gpu_conversion = attempt.gpuConversion;
        vi.colorspace = attempt.colorspace;
        vi.range = attempt.range;
        vi.scale_type = attempt.scaleType;

        videoResetResult = api_->obs_reset_video(&vi);
        if (graphicsPreload) {
            FreeLibrary(graphicsPreload);
        }

        if (videoResetResult == 0) {
            break;
        }
    }

    if (videoResetResult != 0) {
        const auto logSnippet = GetRecentObsLogSnippet();
        api_->obs_shutdown();
        std::ostringstream os;
        os << "obs_reset_video failed (code " << videoResetResult
           << ", width " << width << ", height " << height << ", fps " << fps
           << "). Tried modules/formats: d3d11+NV12(709/partial), d3d11+BGRA(sRGB/full), opengl+BGRA(sRGB/full).";
        if (!logSnippet.empty()) {
            os << " OBS: " << logSnippet;
        }
        error = os.str();
        return false;
    }

    obs_audio_info ai{};
    ai.samples_per_sec = 48000;
    ai.speakers = SPEAKERS_STEREO;
    if (!api_->obs_reset_audio(&ai)) {
        const auto logSnippet = GetRecentObsLogSnippet();
        api_->obs_shutdown();
        error = "obs_reset_audio failed.";
        if (!logSnippet.empty()) {
            error += " OBS: " + logSnippet;
        }
        return false;
    }

    return true;
}

bool LibObsRecorderEngine::ApplyMicrophoneNoiseSuppressionFilter(bool enabled, std::string& error)
{
    error.clear();
    if (!api_) {
        error = "OBS API is unavailable.";
        return false;
    }
    if (!microphoneAudioSource_) {
        if (enabled) {
            error = "Microphone source is unavailable.";
            return false;
        }
        return true;
    }
    if (!api_->obs_source_get_filter_by_name || !api_->obs_source_filter_add || !api_->obs_source_filter_remove) {
        if (enabled) {
            error = "This OBS installation does not expose source-filter controls.";
            return false;
        }
        return true;
    }

    void* existingFilter = api_->obs_source_get_filter_by_name(microphoneAudioSource_, kMicrophoneNoiseFilterName);
    if (!enabled) {
        if (existingFilter) {
            api_->obs_source_filter_remove(microphoneAudioSource_, existingFilter);
            api_->obs_source_release(existingFilter);
        }
        return true;
    }

    auto* filterSettings = api_->obs_data_create();
    if (!filterSettings) {
        if (existingFilter) {
            api_->obs_source_release(existingFilter);
        }
        error = "Failed to create noise suppression filter settings.";
        return false;
    }
    // Prefer RNNoise when available; OBS falls back to the plugin default if unsupported.
    api_->obs_data_set_string(filterSettings, "method", "rnnoise");

    if (existingFilter) {
        if (api_->obs_source_update) {
            api_->obs_source_update(existingFilter, filterSettings);
        }
        api_->obs_data_release(filterSettings);
        api_->obs_source_release(existingFilter);
        return true;
    }

    void* filterSource = api_->obs_source_create("noise_suppress_filter", kMicrophoneNoiseFilterName, filterSettings, nullptr);
    api_->obs_data_release(filterSettings);
    if (!filterSource) {
        error = "Failed to create OBS noise suppression filter source.";
        return false;
    }
    const bool filterAdded = api_->obs_source_filter_add(microphoneAudioSource_, filterSource);
    api_->obs_source_release(filterSource);
    if (!filterAdded) {
        error = "Failed to attach OBS noise suppression filter to microphone source.";
        return false;
    }
    return true;
}

void LibObsRecorderEngine::ReleaseObsObjects()
{
    if (!api_) {
        return;
    }

    if (output_) {
        api_->obs_output_release(output_);
        output_ = nullptr;
    }
    if (videoEncoder_) {
        api_->obs_encoder_release(videoEncoder_);
        videoEncoder_ = nullptr;
    }
    if (audioEncoder_) {
        api_->obs_encoder_release(audioEncoder_);
        audioEncoder_ = nullptr;
    }
    if (gameCaptureSource_) {
        api_->obs_source_release(gameCaptureSource_);
        gameCaptureSource_ = nullptr;
    }
    if (chatBlockerSource_) {
        api_->obs_source_release(chatBlockerSource_);
        chatBlockerSource_ = nullptr;
    }
    if (desktopAudioSource_) {
        api_->obs_source_release(desktopAudioSource_);
        desktopAudioSource_ = nullptr;
    }
    if (discordAudioSource_) {
        api_->obs_source_release(discordAudioSource_);
        discordAudioSource_ = nullptr;
    }
    if (microphoneAudioSource_) {
        api_->obs_source_release(microphoneAudioSource_);
        microphoneAudioSource_ = nullptr;
    }
    if (scene_) {
        api_->obs_scene_release(scene_);
        scene_ = nullptr;
    }
}

void LibObsRecorderEngine::ShutdownObsCore()
{
    if (api_) {
        api_->obs_shutdown();
    }
    if (obsModule_) {
        FreeLibrary(obsModule_);
        obsModule_ = nullptr;
    }
    delete api_;
    api_ = nullptr;
    initialized_ = false;
    recording_ = false;
}

} // namespace bean::obs
