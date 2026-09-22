#include "app/AppContext.h"
#include "app/AppClips.h"
#include "app/AppChatPrivacy.h"
#include "app/AppDraw.h"
#include "app/AppKeybinds.h"
#include "app/AppIconsTaskbar.h"
#include "app/AppLayout.h"
#include "app/AppLiveStatus.h"
#include "app/AppPanelFactory.h"
#include "app/AppRecordings.h"
#include "app/AppYouTube.h"
#include "app/AppRecordingHelpers.h"
#include "app/AppStatusLog.h"
#include "app/AppUtilities.h"
#include "app/BeanUpdater.h"
#include "bean_version.h"
#include "core/FileHash.h"
#include "core/GameEnvironment.h"
#include "core/RecordingSizeEstimate.h"
#include "core/WowData.h"
#include "integrations/YouTubeUploader.h"
#include "obs/IRecorderEngine.h"
#if defined(BEAN_ENABLE_LIBOBS) && BEAN_ENABLE_LIBOBS
#include "obs/LibObsRecorderEngine.h"
#else
#include "obs/MockRecorderEngine.h"
#endif
#include "util/Json.h"
#include "util/Strings.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <gdiplus.h>
#include <mfmediaengine.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "Gdiplus.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

namespace {

constexpr char kYouTubeAuthServerUrl[] = "https://andrew.gg/bean/youtube-auth/";

void ApplyDarkTitleBar(HWND hwnd)
{
    // Win10 20H1+: dark system chrome. Win11+: exact caption/text/border colors.
    const BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

    const COLORREF captionColor = kColorWindowTop;
    const COLORREF textColor = kColorTextPrimary;
    const COLORREF borderColor = kColorWindowTop;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
}

std::wstring VersionText()
{
    return std::wstring(L"v") + BEAN_APP_VERSION_W;
}

std::wstring MainWindowTitleText()
{
    return std::wstring(kWindowTitleBase) + std::wstring(L" - ") + VersionText();
}

using bean::util::ToUtf8;
using bean::util::ToWide;
using bean::util::Trim;

std::string GetEnvString(const char* name);

std::vector<MicrophoneOption> EnumerateMicrophoneOptions()
{
    std::vector<MicrophoneOption> options;
    options.push_back({L"Default microphone", "default"});

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return options;
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        return options;
    }

    UINT count = 0;
    if (SUCCEEDED(collection->GetCount(&count))) {
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device) {
                continue;
            }

            LPWSTR deviceId = nullptr;
            if (FAILED(device->GetId(&deviceId)) || !deviceId) {
                device->Release();
                continue;
            }

            std::wstring friendlyName = L"Microphone";
            IPropertyStore* propertyStore = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &propertyStore)) && propertyStore) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(propertyStore->GetValue(PKEY_Device_FriendlyName, &value))
                    && value.vt == VT_LPWSTR
                    && value.pwszVal
                    && value.pwszVal[0] != L'\0') {
                    friendlyName = value.pwszVal;
                }
                PropVariantClear(&value);
                propertyStore->Release();
            }

            options.push_back({friendlyName, ToUtf8(deviceId)});
            CoTaskMemFree(deviceId);
            device->Release();
        }
    }

    collection->Release();
    enumerator->Release();
    return options;
}

void RefreshMicrophoneOptionsUi(AppContext* ctx);
void RefreshMicrophoneDeviceOptionsUi(AppContext* ctx);

std::string ReadQuotedJson(const std::string& content, const std::string& key)
{
    // Now unescapes, which the local copy this replaced did not. Values with a
    // backslash previously came back with the escape sequence still in them.
    return bean::util::ReadJsonString(content, key);
}

std::string GetEnvString(const char* name)
{
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr || len == 0) {
        if (value) {
            free(value);
        }
        return {};
    }
    std::string out(value);
    free(value);
    return out;
}

std::string GetYouTubeAuthServerUrl()
{
    return kYouTubeAuthServerUrl;
}

LRESULT CALLBACK PanelMessageForwarder(HWND panel, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_ERASEBKGND || message == WM_PAINT) {
        RECT rect{};
        GetClientRect(panel, &rect);
        if (message == WM_ERASEBKGND) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            TRIVERTEX vertices[2] = {
                {rect.left, rect.top, static_cast<COLOR16>(GetRValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetGValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetBValue(kColorPanelTop) << 8), 0xFF00},
                {rect.right, rect.bottom, static_cast<COLOR16>(GetRValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetGValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetBValue(kColorPanelBottom) << 8), 0xFF00},
            };
            GRADIENT_RECT gradientRect{0, 1};
            if (!GradientFill(dc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V)) {
                FillRect(dc, &rect, gTheme.panelSolidBrush ? gTheme.panelSolidBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            if (gTheme.panelBorderBrush) {
                FrameRect(dc, &rect, gTheme.panelBorderBrush);
            }
            return 1;
        }

        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(panel, &paint);
        if (dc) {
            TRIVERTEX vertices[2] = {
                {rect.left, rect.top, static_cast<COLOR16>(GetRValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetGValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetBValue(kColorPanelTop) << 8), 0xFF00},
                {rect.right, rect.bottom, static_cast<COLOR16>(GetRValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetGValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetBValue(kColorPanelBottom) << 8), 0xFF00},
            };
            GRADIENT_RECT gradientRect{0, 1};
            if (!GradientFill(dc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V)) {
                FillRect(dc, &rect, gTheme.panelSolidBrush ? gTheme.panelSolidBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            if (gTheme.panelBorderBrush) {
                FrameRect(dc, &rect, gTheme.panelBorderBrush);
            }
            EndPaint(panel, &paint);
            return 0;
        }
    }
    if (message == WM_COMMAND
        || message == WM_NOTIFY
        || message == WM_HSCROLL
        || message == WM_CTLCOLORSTATIC
        || message == WM_CTLCOLOREDIT
        || message == WM_CTLCOLORBTN
        || message == WM_CTLCOLORLISTBOX
        || message == WM_DRAWITEM
        || message == WM_MEASUREITEM) {
        HWND parent = GetParent(panel);
        if (parent) {
            return SendMessageW(parent, message, wParam, lParam);
        }
    }
    return DefSubclassProc(panel, message, wParam, lParam);
}

std::wstring ChatBlockerAnchorLabel(bean::core::AppSettings::ChatBlockerAnchor anchor)
{
    switch (anchor) {
    case bean::core::AppSettings::ChatBlockerAnchor::BottomRight:
        return L"bottom-right";
    case bean::core::AppSettings::ChatBlockerAnchor::TopLeft:
        return L"top-left";
    case bean::core::AppSettings::ChatBlockerAnchor::TopRight:
        return L"top-right";
    case bean::core::AppSettings::ChatBlockerAnchor::BottomLeft:
    default:
        return L"bottom-left";
    }
}

void RefreshMicrophoneOptionsUi(AppContext* ctx)
{
    if (!ctx || !ctx->microphoneCombo || !ctx->microphoneCheck) {
        return;
    }

    const bool micEnabled = (SendMessageW(ctx->microphoneCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const bool comboWasEnabled = IsWindowEnabled(ctx->microphoneCombo) != FALSE;
    EnableWindow(ctx->microphoneCombo, micEnabled ? TRUE : FALSE);
    if (comboWasEnabled != micEnabled) {
        RedrawWindow(ctx->microphoneCombo, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
    if (ctx->microphoneNoiseSuppressionCheck) {
        const bool noiseWasEnabled = IsWindowEnabled(ctx->microphoneNoiseSuppressionCheck) != FALSE;
        EnableWindow(ctx->microphoneNoiseSuppressionCheck, micEnabled ? TRUE : FALSE);
        if (noiseWasEnabled != micEnabled) {
            RedrawWindow(ctx->microphoneNoiseSuppressionCheck, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        }
    }
}

void RefreshMicrophoneDeviceOptionsUi(AppContext* ctx)
{
    if (!ctx || !ctx->microphoneCombo) {
        return;
    }

    ctx->microphoneOptions = EnumerateMicrophoneOptions();
    SendMessageW(ctx->microphoneCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& option : ctx->microphoneOptions) {
        SendMessageW(ctx->microphoneCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.displayName.c_str()));
    }

    int selectedIndex = 0;
    for (size_t i = 0; i < ctx->microphoneOptions.size(); ++i) {
        if (ctx->microphoneOptions[i].deviceId == ctx->settings.microphoneDeviceId) {
            selectedIndex = static_cast<int>(i);
            break;
        }
    }
    SendMessageW(ctx->microphoneCombo, CB_SETCURSEL, static_cast<WPARAM>(selectedIndex), 0);
    RefreshMicrophoneOptionsUi(ctx);
}

struct WowWindowUiInfo {
    HWND window = nullptr;
    bean::core::WowEdition edition = bean::core::WowEdition::Unknown;
    bool retailWindowDetected = false;
    bool ptrWindowDetected = false;
};

bean::core::WowEdition WowEditionForExecutableName(std::wstring executableName)
{
    std::transform(executableName.begin(), executableName.end(), executableName.begin(), towlower);
    if (executableName == L"wowt.exe") {
        return bean::core::WowEdition::Ptr;
    }
    if (executableName == L"wow.exe") {
        return bean::core::WowEdition::Retail;
    }
    return bean::core::WowEdition::Unknown;
}

BOOL CALLBACK FindWowWindowForUiProc(HWND hwnd, LPARAM lParam)
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
    bean::core::WowEdition edition = bean::core::WowEdition::Unknown;
    if (QueryFullProcessImageNameW(processHandle, 0, processPath, &processPathSize)) {
        edition = WowEditionForExecutableName(
            std::filesystem::path(processPath).filename().wstring());
    }
    CloseHandle(processHandle);
    if (edition == bean::core::WowEdition::Unknown) {
        return TRUE;
    }

    wchar_t title[256] = {};
    const int len = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    if (len <= 0) {
        return TRUE;
    }

    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const bool titleLooksLikeWow = (wcsstr(title, L"World of Warcraft") != nullptr);
    const bool classLooksLikeWow = (_wcsicmp(className, L"GxWindowClass") == 0);
    if (titleLooksLikeWow || classLooksLikeWow) {
        auto* found = reinterpret_cast<WowWindowUiInfo*>(lParam);
        found->retailWindowDetected =
            found->retailWindowDetected || edition == bean::core::WowEdition::Retail;
        found->ptrWindowDetected =
            found->ptrWindowDetected || edition == bean::core::WowEdition::Ptr;
        if (!found->window
            || (edition == bean::core::WowEdition::Ptr
                && found->edition != bean::core::WowEdition::Ptr)) {
            found->window = hwnd;
            found->edition = edition;
        }
    }

    return TRUE;
}

WowWindowUiInfo FindWowWindowForUi()
{
    WowWindowUiInfo found;
    EnumWindows(FindWowWindowForUiProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

struct WowClientInfo {
    int width = 0;
    int height = 0;
    bean::core::WowEdition edition = bean::core::WowEdition::Unknown;
    bool bothInstancesDetected = false;
};

std::optional<WowClientInfo> GetWowClientSizeForUi()
{
    const WowWindowUiInfo wow = FindWowWindowForUi();
    if (!wow.window) {
        return std::nullopt;
    }

    RECT client{};
    if (!GetClientRect(wow.window, &client)) {
        return std::nullopt;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return WowClientInfo{
        width,
        height,
        wow.edition,
        wow.retailWindowDetected && wow.ptrWindowDetected};
}

std::pair<int, int> ScaleResolutionToHeight(int sourceWidth, int sourceHeight, int targetHeight)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 || targetHeight <= 0 || targetHeight >= sourceHeight) {
        return {sourceWidth, sourceHeight};
    }
    int width = static_cast<int>(
        (static_cast<long long>(sourceWidth) * targetHeight + sourceHeight / 2) / sourceHeight);
    width = (std::max)(2, width & ~1);
    return {width, (std::max)(2, targetHeight & ~1)};
}

void RefreshRecordingResolutionOptions(AppContext* ctx)
{
    if (!ctx || !ctx->recordingResolutionCombo) {
        return;
    }

    const int previousHeight = ctx->settings.recordingResolutionHeight;
    SendMessageW(ctx->recordingResolutionCombo, CB_RESETCONTENT, 0, 0);
    if (ctx->detectedWowClientWidth <= 0 || ctx->detectedWowClientHeight <= 0) {
        SendMessageW(
            ctx->recordingResolutionCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(L"Waiting for WoW..."));
        SendMessageW(ctx->recordingResolutionCombo, CB_SETCURSEL, 0, 0);
        EnableWindow(ctx->recordingResolutionCombo, FALSE);
        return;
    }

    EnableWindow(ctx->recordingResolutionCombo, !ctx->isRecording);
    const auto addOption = [&](const std::wstring& label, int targetHeight) {
        const LRESULT index = SendMessageW(
            ctx->recordingResolutionCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(ctx->recordingResolutionCombo, CB_SETITEMDATA, static_cast<WPARAM>(index), targetHeight);
    };

    addOption(
        L"Full resolution (" + std::to_wstring(ctx->detectedWowClientWidth)
            + L" x " + std::to_wstring(ctx->detectedWowClientHeight) + L")",
        0);
    constexpr int commonHeights[] = {2160, 1440, 1080, 720};
    for (const int height : commonHeights) {
        if (height >= ctx->detectedWowClientHeight) {
            continue;
        }
        const auto [width, scaledHeight] = ScaleResolutionToHeight(
            ctx->detectedWowClientWidth,
            ctx->detectedWowClientHeight,
            height);
        addOption(
            L"Downscale to " + std::to_wstring(width)
                + L" x " + std::to_wstring(scaledHeight),
            height);
    }

    LRESULT selectedIndex = 0;
    const LRESULT itemCount = SendMessageW(ctx->recordingResolutionCombo, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < itemCount; ++i) {
        if (SendMessageW(ctx->recordingResolutionCombo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0)
            == previousHeight) {
            selectedIndex = i;
            break;
        }
    }
    SendMessageW(ctx->recordingResolutionCombo, CB_SETCURSEL, static_cast<WPARAM>(selectedIndex), 0);
}

std::wstring NormalizeProcessOrWindowToken(std::wstring value)
{
    std::wstring normalized;
    normalized.reserve(value.size());
    for (wchar_t ch : value) {
        if (std::iswalnum(ch)) {
            normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }
    }
    return normalized;
}

bool StartsWith(const std::wstring& value, const std::wstring& prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

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

void ApplyFolderAvailabilityResult(AppContext* ctx, const FolderAvailabilityResult& result);
void ApplyDiskSpaceProbeResult(AppContext* ctx, const DiskSpaceProbeResult& result);
void BeginDiskSpaceProbe(AppContext* ctx);

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

void ApplyDiskSpaceProbeResult(AppContext* ctx, const DiskSpaceProbeResult& result)
{
    if (!ctx) {
        return;
    }

    const bool wasLow = ctx->diskSpaceLow;
    ctx->diskSpaceLow = result.status == bean::core::DiskSpaceStatus::Warning;
    ctx->diskSpaceQueryFailed = result.status == bean::core::DiskSpaceStatus::Unknown;
    ctx->diskSpaceAvailableBytes = result.availableBytes;
    ctx->diskSpaceEstimatedRecordingBytes = result.estimatedRecordingBytes;
    ctx->diskSpaceWarningThresholdBytes = result.warningThresholdBytes;

    std::wstring statusText = L"Checking...";
    if (ctx->diskSpaceQueryFailed) {
        statusText = L"Unable to check";
    } else if (ctx->diskSpaceLow) {
        statusText = std::wstring(L"Low (") + FormatBytes(result.availableBytes) + L" free)";
    } else {
        statusText = FormatBytes(result.availableBytes) + L" free";
    }
    if (ctx->diskSpaceText) {
        UpdateTransparentStaticText(ctx->diskSpaceText, statusText.c_str());
    }
    if (ctx->diskSpaceIcon) {
        InvalidateRect(ctx->diskSpaceIcon, nullptr, FALSE);
    }
    if (ctx->statusTabButton && wasLow != ctx->diskSpaceLow) {
        InvalidateRect(ctx->statusTabButton, nullptr, FALSE);
    }
    ApplyTaskbarOverlayState(ctx);

    if (ctx->diskSpaceLow) {
        if (!ctx->diskSpaceWarningLogged) {
            ctx->diskSpaceWarningLogged = true;
            SetStatus(
                ctx,
                std::wstring(L"Warning: recordings drive is low on space (")
                    + FormatBytes(result.availableBytes)
                    + L" free). A typical 35-minute recording at your current settings is about "
                    + FormatBytes(result.estimatedRecordingBytes)
                    + L". Bean warns below "
                    + FormatBytes(result.warningThresholdBytes)
                    + L" free (3 recordings).");
        }
    } else if (wasLow && ctx->diskSpaceWarningLogged) {
        ctx->diskSpaceWarningLogged = false;
        if (!ctx->diskSpaceQueryFailed) {
            SetStatus(ctx, L"Recordings drive has enough free space again.");
        }
    }
}

// Launches ffmpeg to confirm it actually runs, so this must never happen on the
// UI thread. The result comes back via WM_BEAN_FFMPEG_PROBE_COMPLETE.
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

bool DetectAdvancedCombatLoggingForUi(const AppContext* ctx)
{
    return bean::core::IsAdvancedCombatLoggingEnabled(
        ctx ? ctx->settings.wowInstallDirectory : std::filesystem::path{},
        ctx ? ctx->detectedWowEdition : bean::core::WowEdition::Unknown);
}

bool EnsureChatPreviewFrameBitmap(AppContext* ctx, HDC referenceDc, int width, int height)
{
    if (!ctx || !referenceDc || width <= 0 || height <= 0) {
        return false;
    }
    if (ctx->chatPreviewFrameBitmap
        && ctx->chatPreviewFrameWidth == width
        && ctx->chatPreviewFrameHeight == height) {
        return true;
    }

    if (ctx->chatPreviewFrameBitmap) {
        DeleteObject(ctx->chatPreviewFrameBitmap);
        ctx->chatPreviewFrameBitmap = nullptr;
    }
    ctx->chatPreviewFrameWidth = 0;
    ctx->chatPreviewFrameHeight = 0;
    ctx->chatPreviewFrameValid = false;

    ctx->chatPreviewFrameBitmap = CreateCompatibleBitmap(referenceDc, width, height);
    if (!ctx->chatPreviewFrameBitmap) {
        return false;
    }
    ctx->chatPreviewFrameWidth = width;
    ctx->chatPreviewFrameHeight = height;
    return true;
}

void DrawChatPrivacyPreview(const DRAWITEMSTRUCT* drawInfo, AppContext* ctx)
{
    if (!drawInfo || !ctx) {
        return;
    }

    RECT rc = drawInfo->rcItem;
    const int surfaceWidth = rc.right - rc.left;
    const int surfaceHeight = rc.bottom - rc.top;
    if (surfaceWidth <= 0 || surfaceHeight <= 0) {
        return;
    }

    HDC paintDc = drawInfo->hDC;
    HDC bufferedDc = CreateCompatibleDC(drawInfo->hDC);
    HBITMAP bufferedBitmap = nullptr;
    HGDIOBJ oldBufferedBitmap = nullptr;
    bool useBackBuffer = false;
    if (bufferedDc) {
        bufferedBitmap = CreateCompatibleBitmap(drawInfo->hDC, surfaceWidth, surfaceHeight);
        if (bufferedBitmap) {
            oldBufferedBitmap = SelectObject(bufferedDc, bufferedBitmap);
            // Keep existing absolute-coordinate drawing math intact.
            SetWindowOrgEx(bufferedDc, rc.left, rc.top, nullptr);
            paintDc = bufferedDc;
            useBackBuffer = true;
        }
    }
    auto presentAndCleanup = [&]() {
        if (useBackBuffer) {
            BitBlt(drawInfo->hDC, rc.left, rc.top, surfaceWidth, surfaceHeight, bufferedDc, rc.left, rc.top, SRCCOPY);
        }
        if (oldBufferedBitmap) {
            SelectObject(bufferedDc, oldBufferedBitmap);
        }
        if (bufferedBitmap) {
            DeleteObject(bufferedBitmap);
        }
        if (bufferedDc) {
            DeleteDC(bufferedDc);
        }
    };

    HBRUSH backgroundBrush = CreateSolidBrush(kColorInputBg);
    if (backgroundBrush) {
        FillRect(paintDc, &rc, backgroundBrush);
        DeleteObject(backgroundBrush);
    }

    HPEN borderPen = CreatePen(PS_SOLID, 1, kColorInputBorder);
    HGDIOBJ oldPen = nullptr;
    HGDIOBJ oldBrush = nullptr;
    if (borderPen) {
        oldPen = SelectObject(paintDc, borderPen);
    }
    oldBrush = SelectObject(paintDc, GetStockObject(NULL_BRUSH));
    Rectangle(paintDc, rc.left, rc.top, rc.right, rc.bottom);
    if (oldBrush) {
        SelectObject(paintDc, oldBrush);
    }
    if (oldPen) {
        SelectObject(paintDc, oldPen);
    }
    if (borderPen) {
        DeleteObject(borderPen);
    }

    RECT content = rc;
    InflateRect(&content, -8, -8);
    const int contentWidth = content.right - content.left;
    const int contentHeight = content.bottom - content.top;
    if (contentWidth <= 0 || contentHeight <= 0) {
        presentAndCleanup();
        return;
    }

    const bool previewPausedDuringRecording = ctx->isRecording;
    if (previewPausedDuringRecording) {
        if (ctx->chatPreviewFrameBitmap) {
            DeleteObject(ctx->chatPreviewFrameBitmap);
            ctx->chatPreviewFrameBitmap = nullptr;
        }
        ctx->chatPreviewFrameWidth = 0;
        ctx->chatPreviewFrameHeight = 0;
        ctx->chatPreviewFrameValid = false;
        ctx->chatPreviewLastCaptureAt.reset();
    }

    int sourceWidth = ctx->chatPreviewSourceWidth > 0 ? ctx->chatPreviewSourceWidth : 1920;
    int sourceHeight = ctx->chatPreviewSourceHeight > 0 ? ctx->chatPreviewSourceHeight : 1080;
    if (!previewPausedDuringRecording) {
        if (const WowWindowUiInfo wow = FindWowWindowForUi(); wow.window) {
            RECT wowRect{};
            if (GetClientRect(wow.window, &wowRect)) {
                const int wowWidth = static_cast<int>(wowRect.right - wowRect.left);
                const int wowHeight = static_cast<int>(wowRect.bottom - wowRect.top);
                sourceWidth = (std::max)(1, wowWidth);
                sourceHeight = (std::max)(1, wowHeight);
                ctx->chatPreviewSourceWidth = sourceWidth;
                ctx->chatPreviewSourceHeight = sourceHeight;
            }

            const int safeSourceWidth = (std::max)(1, sourceWidth);
            const int safeSourceHeight = (std::max)(1, sourceHeight);
            int previewWidth = contentWidth;
            int previewHeight = (std::max)(1, static_cast<int>((static_cast<long long>(previewWidth) * safeSourceHeight) / safeSourceWidth));
            if (previewHeight > contentHeight) {
                previewHeight = contentHeight;
                previewWidth = (std::max)(1, static_cast<int>((static_cast<long long>(previewHeight) * safeSourceWidth) / safeSourceHeight));
            }
            RECT previewRect{
                content.left + (contentWidth - previewWidth) / 2,
                content.top + (contentHeight - previewHeight) / 2,
                content.left + (contentWidth - previewWidth) / 2 + previewWidth,
                content.top + (contentHeight - previewHeight) / 2 + previewHeight};

            const auto now = std::chrono::steady_clock::now();
            const bool shouldCapture = !ctx->chatPreviewFrameValid
                || !ctx->chatPreviewLastCaptureAt.has_value()
                || (now - *ctx->chatPreviewLastCaptureAt) >= kChatPreviewCaptureInterval;
            if (shouldCapture && !IsIconic(wow.window)
                && EnsureChatPreviewFrameBitmap(ctx, paintDc, previewWidth, previewHeight)) {
                HDC frameDc = CreateCompatibleDC(paintDc);
                HGDIOBJ oldBitmap = nullptr;
                if (frameDc) {
                    oldBitmap = SelectObject(frameDc, ctx->chatPreviewFrameBitmap);
                    HDC scratchDc = CreateCompatibleDC(paintDc);
                    HBITMAP scratchBitmap = nullptr;
                    HGDIOBJ oldScratchBitmap = nullptr;
                    if (scratchDc) {
                        scratchBitmap = CreateCompatibleBitmap(paintDc, sourceWidth, sourceHeight);
                        if (scratchBitmap) {
                            oldScratchBitmap = SelectObject(scratchDc, scratchBitmap);
                            constexpr UINT kPrintWindowRenderFullContent = 0x00000002;
                            const BOOL printed = PrintWindow(wow.window, scratchDc, PW_CLIENTONLY | kPrintWindowRenderFullContent);
                            if (printed) {
                                SetStretchBltMode(frameDc, COLORONCOLOR);
                                const BOOL copied = StretchBlt(
                                    frameDc,
                                    0,
                                    0,
                                    previewWidth,
                                    previewHeight,
                                    scratchDc,
                                    0,
                                    0,
                                    sourceWidth,
                                    sourceHeight,
                                    SRCCOPY);
                                if (copied) {
                                    ctx->chatPreviewFrameValid = true;
                                    ctx->chatPreviewLastCaptureAt = now;
                                }
                            }
                        }
                    }
                    if (oldScratchBitmap) {
                        SelectObject(scratchDc, oldScratchBitmap);
                    }
                    if (scratchBitmap) {
                        DeleteObject(scratchBitmap);
                    }
                    if (scratchDc) {
                        DeleteDC(scratchDc);
                    }
                }
                if (oldBitmap) {
                    SelectObject(frameDc, oldBitmap);
                }
                if (frameDc) {
                    DeleteDC(frameDc);
                }
            }
        }
    }

    bool drewPreview = false;
    const int safeSourceWidth = (std::max)(1, sourceWidth);
    const int safeSourceHeight = (std::max)(1, sourceHeight);
    int previewWidth = contentWidth;
    int previewHeight = (std::max)(1, static_cast<int>((static_cast<long long>(previewWidth) * safeSourceHeight) / safeSourceWidth));
    if (previewHeight > contentHeight) {
        previewHeight = contentHeight;
        previewWidth = (std::max)(1, static_cast<int>((static_cast<long long>(previewHeight) * safeSourceWidth) / safeSourceHeight));
    }
    RECT previewRect{
        content.left + (contentWidth - previewWidth) / 2,
        content.top + (contentHeight - previewHeight) / 2,
        content.left + (contentWidth - previewWidth) / 2 + previewWidth,
        content.top + (contentHeight - previewHeight) / 2 + previewHeight};

    if (previewPausedDuringRecording) {
        HBRUSH pausedBrush = CreateSolidBrush(kThemeColors.controlDisabledBackground);
        if (pausedBrush) {
            FillRect(paintDc, &previewRect, pausedBrush);
            DeleteObject(pausedBrush);
        }
        SetBkMode(paintDc, TRANSPARENT);
        SetTextColor(paintDc, kColorTextMuted);
        DrawTextW(paintDc, L"Preview paused during recording", -1, &previewRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        presentAndCleanup();
        return;
    }

    if (ctx->chatPreviewFrameValid && ctx->chatPreviewFrameBitmap) {
        HDC frameDc = CreateCompatibleDC(paintDc);
        HGDIOBJ oldBitmap = nullptr;
        if (frameDc) {
            oldBitmap = SelectObject(frameDc, ctx->chatPreviewFrameBitmap);
            drewPreview = (BitBlt(
                paintDc,
                previewRect.left,
                previewRect.top,
                previewWidth,
                previewHeight,
                frameDc,
                0,
                0,
                SRCCOPY) != 0);
        }
        if (oldBitmap) {
            SelectObject(frameDc, oldBitmap);
        }
        if (frameDc) {
            DeleteDC(frameDc);
        }
    }

    if (!drewPreview) {
        HBRUSH fallbackBrush = CreateSolidBrush(kThemeColors.controlDisabledBackground);
        if (fallbackBrush) {
            FillRect(paintDc, &previewRect, fallbackBrush);
            DeleteObject(fallbackBrush);
        }
        SetBkMode(paintDc, TRANSPARENT);
        SetTextColor(paintDc, kColorTextMuted);
        DrawTextW(paintDc, L"WoW preview unavailable", -1, &previewRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    const bool blockerEnabled = ctx->chatBlockerEnabledCheck
        ? (SendMessageW(ctx->chatBlockerEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED)
        : ctx->settings.chatBlockerEnabled;
    const int blockerWidth = (std::max)(0, ctx->chatBlockerWidthEdit ? ReadIntControl(ctx->chatBlockerWidthEdit, ctx->settings.chatBlockerWidth) : ctx->settings.chatBlockerWidth);
    const int blockerHeight = (std::max)(0, ctx->chatBlockerHeightEdit ? ReadIntControl(ctx->chatBlockerHeightEdit, ctx->settings.chatBlockerHeight) : ctx->settings.chatBlockerHeight);
    if (!blockerEnabled || blockerWidth <= 0 || blockerHeight <= 0) {
        presentAndCleanup();
        return;
    }

    const int scaledWidth = (std::max)(1, blockerWidth * previewWidth / safeSourceWidth);
    const int scaledHeight = (std::max)(1, blockerHeight * previewHeight / safeSourceHeight);
    int comboIndex = 0;
    if (ctx->chatBlockerAnchorCombo) {
        comboIndex = static_cast<int>(SendMessageW(ctx->chatBlockerAnchorCombo, CB_GETCURSEL, 0, 0));
    }
    const auto anchor = ChatBlockerAnchorFromComboIndex(comboIndex);

    RECT blockerRect{};
    switch (anchor) {
    case bean::core::AppSettings::ChatBlockerAnchor::BottomRight:
        blockerRect.right = previewRect.right;
        blockerRect.left = blockerRect.right - scaledWidth;
        blockerRect.bottom = previewRect.bottom;
        blockerRect.top = blockerRect.bottom - scaledHeight;
        break;
    case bean::core::AppSettings::ChatBlockerAnchor::TopLeft:
        blockerRect.left = previewRect.left;
        blockerRect.right = blockerRect.left + scaledWidth;
        blockerRect.top = previewRect.top;
        blockerRect.bottom = blockerRect.top + scaledHeight;
        break;
    case bean::core::AppSettings::ChatBlockerAnchor::TopRight:
        blockerRect.right = previewRect.right;
        blockerRect.left = blockerRect.right - scaledWidth;
        blockerRect.top = previewRect.top;
        blockerRect.bottom = blockerRect.top + scaledHeight;
        break;
    case bean::core::AppSettings::ChatBlockerAnchor::BottomLeft:
    default:
        blockerRect.left = previewRect.left;
        blockerRect.right = blockerRect.left + scaledWidth;
        blockerRect.bottom = previewRect.bottom;
        blockerRect.top = blockerRect.bottom - scaledHeight;
        break;
    }

    blockerRect.left = (std::max)(previewRect.left, blockerRect.left);
    blockerRect.top = (std::max)(previewRect.top, blockerRect.top);
    blockerRect.right = (std::min)(previewRect.right, blockerRect.right);
    blockerRect.bottom = (std::min)(previewRect.bottom, blockerRect.bottom);

    if (blockerRect.right <= blockerRect.left || blockerRect.bottom <= blockerRect.top) {
        presentAndCleanup();
        return;
    }

    const bool useCustomBlockerImage = ctx->chatBlockerImageCustomRadio
        ? (SendMessageW(ctx->chatBlockerImageCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED)
        : ctx->settings.chatBlockerUseCustomImage;

    bool drewCustomImage = false;
    if (useCustomBlockerImage) {
        std::filesystem::path customImagePath = ResolveSelectedChatBlockerImagePath(ctx);
        if (customImagePath.empty() && !ctx->settings.chatBlockerCustomImagePath.empty()) {
            customImagePath = ctx->settings.chatBlockerCustomImagePath;
        }
        drewCustomImage = DrawChatBlockerImageOverlay(paintDc, blockerRect, customImagePath);
    }

    if (!drewCustomImage) {
        HBRUSH blockerBrush = CreateSolidBrush(RGB(0, 0, 0));
        if (blockerBrush) {
            FillRect(paintDc, &blockerRect, blockerBrush);
            DeleteObject(blockerBrush);
        }
    }

    presentAndCleanup();
}

struct YouTubeAuthCompletionPayload {
    bool success = false;
    std::string clientId;
    std::string refreshToken;
    std::string channelId;
    std::string channelTitle;
    std::string error;
};

struct YouTubeUploadProgressPayload {
    int percent = 0;
    std::wstring text;
    std::wstring videoUrl;
};

struct YouTubeIdentityResolvedPayload {
    bool success = false;
    std::string channelId;
    std::string channelTitle;
    std::string error;
};

struct UpdateAvailabilityPayload {
    std::uint64_t requestId = 0;
    bean::app::UpdateAvailability availability = bean::app::UpdateAvailability::Failed;
    std::wstring statusMessage;
};

void DiscardQueuedAppMessages(HWND targetWindow)
{
    if (!targetWindow) {
        return;
    }
    MSG message{};
    while (PeekMessageW(
        &message,
        targetWindow,
        WM_APP + 100,
        WM_APP + 115,
        PM_REMOVE)) {
        switch (message.message) {
        case WM_BEAN_STATUS:
            delete reinterpret_cast<std::wstring*>(message.lParam);
            break;
        case WM_BEAN_YOUTUBE_AUTH_COMPLETE:
            delete reinterpret_cast<YouTubeAuthCompletionPayload*>(message.lParam);
            break;
        case WM_BEAN_YOUTUBE_UPLOAD_PROGRESS:
            delete reinterpret_cast<YouTubeUploadProgressPayload*>(message.lParam);
            break;
        case WM_BEAN_YOUTUBE_IDENTITY_RESOLVED:
            delete reinterpret_cast<YouTubeIdentityResolvedPayload*>(message.lParam);
            break;
        case WM_BEAN_CLIPS_EXPORT_COMPLETE:
            delete reinterpret_cast<ClipExportCompletePayload*>(message.lParam);
            break;
        case WM_BEAN_UPDATE_AVAILABILITY_READY:
            delete reinterpret_cast<UpdateAvailabilityPayload*>(message.lParam);
            break;
        case WM_BEAN_FFMPEG_PROBE_COMPLETE:
            delete reinterpret_cast<FfmpegProbeResult*>(message.lParam);
            break;
        case WM_BEAN_FOLDER_AVAILABILITY_COMPLETE:
            delete reinterpret_cast<FolderAvailabilityResult*>(message.lParam);
            break;
        case WM_BEAN_DISK_SPACE_COMPLETE:
            delete reinterpret_cast<DiskSpaceProbeResult*>(message.lParam);
            break;
        default:
            break;
        }
    }
}

void PostYouTubeUploadProgress(
    AppContext* ctx,
    int percent,
    const std::wstring& text,
    const std::wstring& videoUrl = {})
{
    if (!ctx) {
        return;
    }
    auto* payload = new YouTubeUploadProgressPayload();
    payload->percent = std::clamp(percent, 0, 100);
    payload->text = text;
    payload->videoUrl = videoUrl;
    PostOwnedAppMessage(ctx, WM_BEAN_YOUTUBE_UPLOAD_PROGRESS, payload);
}

LRESULT CALLBACK YouTubeUploadStatusSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR refData)
{
    auto* ctx = reinterpret_cast<AppContext*>(refData);
    if (message == WM_SETCURSOR && ctx && !ctx->youtubeLastVideoUrl.empty()) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        if (PtInRect(&ctx->youtubeUploadLinkBounds, point)) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
            return TRUE;
        }
    }
    if (message == WM_LBUTTONUP && ctx && !ctx->youtubeLastVideoUrl.empty()) {
        const POINT point{
            static_cast<short>(LOWORD(lParam)),
            static_cast<short>(HIWORD(lParam))};
        if (PtInRect(&ctx->youtubeUploadLinkBounds, point)) {
            const auto result = reinterpret_cast<intptr_t>(
                ShellExecuteW(hwnd, L"open", ctx->youtubeLastVideoUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32) {
                SetStatus(ctx, L"Failed to open the uploaded YouTube video.");
            }
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, YouTubeUploadStatusSubclassProc, 4);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void RequestYouTubeUiRefresh(AppContext* ctx)
{
    PostBeanAppMessage(ctx, WM_BEAN_YOUTUBE_UI_REFRESH);
}

void ResolveLinkedYouTubeIdentityAsync(AppContext* ctx, bool postErrorToStatus)
{
    if (!ctx
        || !ctx->mainWindow
        || ctx->shuttingDown.load(std::memory_order_acquire)
        || ctx->settings.youtubeRefreshToken.empty()
        || ctx->settings.youtubeClientId.empty()) {
        return;
    }
    bean::integrations::YouTubeCredentials creds;
    creds.clientId = ctx->settings.youtubeClientId;
    creds.refreshToken = ctx->settings.youtubeRefreshToken;
    creds.authServerUrl = GetYouTubeAuthServerUrl();
    LaunchAppWorker(ctx, [ctx, creds, postErrorToStatus]() {
        const auto identity = bean::integrations::YouTubeUploader::GetLinkedChannelIdentity(creds);
        auto* payload = new YouTubeIdentityResolvedPayload();
        payload->success = identity.success;
        payload->channelId = identity.channelId;
        payload->channelTitle = identity.channelTitle;
        payload->error = postErrorToStatus ? identity.error : std::string{};
        PostOwnedAppMessage(ctx, WM_BEAN_YOUTUBE_IDENTITY_RESOLVED, payload);
    });
}

void SetYouTubeUploadUi(AppContext* ctx, int percent, const std::wstring& text)
{
    if (!ctx) {
        return;
    }
    const int clampedPercent = std::clamp(percent, 0, 100);
    if (ctx->youtubeUploadProgress) {
        if (ctx->youtubeUploadPercent != clampedPercent) {
            ctx->youtubeUploadPercent = clampedPercent;
            SendMessageW(ctx->youtubeUploadProgress, PBM_SETPOS, static_cast<WPARAM>(clampedPercent), 0);
        }
    }
    if (ctx->youtubeUploadStatus) {
        if (ctx->youtubeUploadStatusText != text) {
            ctx->youtubeUploadStatusText = text;
            UpdateTransparentStaticText(ctx->youtubeUploadStatus, text.c_str());
        }
    }
}

void RefreshYouTubeUiState(AppContext* ctx);

void UpdateYouTubeMediaSelection(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    const int selectedIndex = GetSelectedYouTubeMediaIndex(ctx);
    if (selectedIndex >= 0 && static_cast<size_t>(selectedIndex) < ctx->youtubeMediaItems.size()) {
        if (ctx->youtubeTitleEdit) {
            const std::wstring title = DefaultYouTubeTitle(
                ctx->youtubeMediaItems[static_cast<size_t>(selectedIndex)].path);
            if (GetWindowTextString(ctx->youtubeTitleEdit) != title) {
                SetWindowTextW(ctx->youtubeTitleEdit, title.c_str());
            }
        }
    } else if (ctx->youtubeTitleEdit) {
        if (!GetWindowTextString(ctx->youtubeTitleEdit).empty()) {
            SetWindowTextW(ctx->youtubeTitleEdit, L"");
        }
    }
    RefreshYouTubeUiState(ctx);
}

void RefreshYouTubeMediaList(AppContext* ctx, bool startReconciliation = true)
{
    if (!ctx || !ctx->youtubeMediaList || !ctx->youtubeLabel) {
        return;
    }

    const auto folders = CollectKnownRecordingFolders(ctx);
    const bool anyFolderAvailable = std::any_of(
        folders.begin(),
        folders.end(),
        [](const auto& folder) { return DirectoryExists(folder.wstring()); });
    if (!anyFolderAvailable) {
        if (!ctx->youtubeMediaItems.empty() || ctx->youtubeMediaSelectedIndex != -1) {
            ctx->youtubeMediaItems.clear();
            RepopulateYouTubeMediaList(ctx);
            UpdateYouTubeMediaSelection(ctx);
        }
        UpdateTransparentStaticText(ctx->youtubeLabel, L"Recordings folder is unavailable.");
        if (startReconciliation) {
            BeginRecordingReconciliation(ctx);
        }
        return;
    }

    const auto previousItems = ctx->youtubeMediaItems;
    ctx->youtubeMediaItems = EnumerateYouTubeMediaFilesInFolders(folders);
    if (ctx->runRepository) {
        std::string dbError;
        std::unordered_map<std::string, std::string> triggerReasonsByPath;
        for (const auto& run : ctx->runRepository->ListRuns(dbError)) {
            triggerReasonsByPath[RecordingPathKey(run.videoPath)] = run.triggerReason;
            for (const auto& alias : run.pathAliases) {
                triggerReasonsByPath[RecordingPathKey(alias)] = run.triggerReason;
            }
        }
        for (auto& item : ctx->youtubeMediaItems) {
            if (item.type != YouTubeMediaType::Recording) {
                continue;
            }
            const auto triggerIt = triggerReasonsByPath.find(RecordingPathKey(item.path));
            if (triggerIt != triggerReasonsByPath.end()) {
                item.triggerReason = triggerIt->second;
            }
        }
    }
    SortYouTubeMediaItems(ctx);
    const bool mediaListChanged = !YouTubeMediaItemsEqual(previousItems, ctx->youtubeMediaItems);
    if (mediaListChanged) {
        RepopulateYouTubeMediaList(ctx);
        UpdateYouTubeMediaSelection(ctx);
    } else {
        RefreshYouTubeUiState(ctx);
    }

    size_t recordingCount = 0;
    size_t clipCount = 0;
    for (const auto& item : ctx->youtubeMediaItems) {
        if (item.type == YouTubeMediaType::Clip) {
            ++clipCount;
        } else {
            ++recordingCount;
        }
    }
    std::wostringstream summary;
    if (folders.size() == 1) {
        summary << folders.front().wstring();
    } else {
        summary << L"Known recording folders";
    }
    summary << L" (" << recordingCount << L" recording";
    if (recordingCount != 1) {
        summary << L"s";
    }
    summary << L", " << clipCount << L" clip";
    if (clipCount != 1) {
        summary << L"s";
    }
    summary << L")";
    UpdateTransparentStaticText(ctx->youtubeLabel, summary.str().c_str());
    if (startReconciliation) {
        BeginRecordingReconciliation(ctx);
    }
}

void RefreshYouTubeUiState(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    const auto setTextIfChanged = [](HWND control, const std::wstring& text) {
        if (control && GetWindowTextString(control) != text) {
            SetWindowTextW(control, text.c_str());
        }
    };
    const auto setVisibleIfChanged = [](HWND control, bool visible) {
        if (control && (IsWindowVisible(control) != FALSE) != visible) {
            ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
        }
    };
    const auto setEnabledIfChanged = [](HWND control, BOOL enabled) {
        if (control && IsWindowEnabled(control) != enabled) {
            EnableWindow(control, enabled);
        }
    };
    const bool wasOauthConfigured = ctx->youtubeOAuthConfigured;
    const bool wasLinked = ctx->youtubeLinked;
    const bool oauthConfigured = !GetYouTubeAuthServerUrl().empty();
    const bool linked = !ctx->settings.youtubeRefreshToken.empty();
    ctx->youtubeOAuthConfigured = oauthConfigured;
    ctx->youtubeLinked = linked;
    if (!linked) {
        ctx->youtubeUnlinkConfirmPending = false;
    }
    const int selectedIndex = GetSelectedYouTubeMediaIndex(ctx);
    const bool canUpload = oauthConfigured && linked && !ctx->youtubeBusy.load() && selectedIndex >= 0 && static_cast<size_t>(selectedIndex) < ctx->youtubeMediaItems.size();

    if (ctx->youtubeLinkStatus) {
        const std::wstring statusText = !oauthConfigured
            ? L"OAuth not configured"
            : (linked ? L"Linked" : L"Not linked");
        setTextIfChanged(ctx->youtubeLinkStatus, statusText);
        setVisibleIfChanged(ctx->youtubeLinkStatus, false);
        if (wasOauthConfigured != oauthConfigured || wasLinked != linked) {
            InvalidateRect(ctx->youtubeLinkStatus, nullptr, FALSE);
        }
    }
    if (ctx->youtubeLinkButton) {
        setVisibleIfChanged(ctx->youtubeLinkButton, !linked && oauthConfigured);
        setEnabledIfChanged(ctx->youtubeLinkButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    const bool showUnlinkConfirm = linked && ctx->youtubeUnlinkConfirmPending;
    if (ctx->youtubeUnlinkButton) {
        setTextIfChanged(ctx->youtubeUnlinkButton, L"Unlink Account");
        setVisibleIfChanged(ctx->youtubeUnlinkButton, linked && !showUnlinkConfirm);
        setEnabledIfChanged(ctx->youtubeUnlinkButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    if (ctx->youtubeUnlinkConfirmLabel) {
        setVisibleIfChanged(ctx->youtubeUnlinkConfirmLabel, showUnlinkConfirm);
    }
    if (ctx->youtubeUnlinkYesButton) {
        setVisibleIfChanged(ctx->youtubeUnlinkYesButton, showUnlinkConfirm);
        setEnabledIfChanged(ctx->youtubeUnlinkYesButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    if (ctx->youtubeUnlinkNoButton) {
        setVisibleIfChanged(ctx->youtubeUnlinkNoButton, showUnlinkConfirm);
        setEnabledIfChanged(ctx->youtubeUnlinkNoButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    if (ctx->youtubeAccountLabel) {
        // Transparent STATIC: use UpdateTransparentStaticText so repeated refreshes
        // don't stack glyphs (SetWindowText alone doesn't erase under NULL_BRUSH).
        UpdateTransparentStaticText(ctx->youtubeAccountLabel, L"YouTube Account:");
        if (!linked) {
            if (ctx->youtubeAccountLink) {
                setTextIfChanged(ctx->youtubeAccountLink, L"Not linked");
                setEnabledIfChanged(ctx->youtubeAccountLink, FALSE);
                setVisibleIfChanged(ctx->youtubeAccountLink, true);
            }
        } else if (!ctx->settings.youtubeChannelId.empty()) {
            if (ctx->youtubeAccountLink) {
                const std::wstring text = ToWide(
                    ctx->settings.youtubeChannelTitle.empty()
                        ? ctx->settings.youtubeChannelId
                        : ctx->settings.youtubeChannelTitle);
                setTextIfChanged(ctx->youtubeAccountLink, text);
                setEnabledIfChanged(ctx->youtubeAccountLink, TRUE);
                setVisibleIfChanged(ctx->youtubeAccountLink, true);
            }
        } else {
            if (ctx->youtubeAccountLink) {
                setTextIfChanged(ctx->youtubeAccountLink, ctx->youtubeBusy.load() ? L"Resolving..." : L"Linked");
                setEnabledIfChanged(ctx->youtubeAccountLink, FALSE);
                setVisibleIfChanged(ctx->youtubeAccountLink, true);
            }
        }
    }
    if (ctx->youtubeUploadButton) {
        setEnabledIfChanged(ctx->youtubeUploadButton, canUpload ? TRUE : FALSE);
    }
}

void UnlinkYouTubeAccount(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    ctx->settings.youtubeRefreshToken.clear();
    ctx->settings.youtubeChannelId.clear();
    ctx->settings.youtubeChannelTitle.clear();
    ctx->youtubeLastVideoUrl.clear();
    std::string saveError;
    if (!ctx->settingsStore.Save(ctx->settings, saveError)) {
        SetStatus(ctx, std::wstring(L"Failed to unlink YouTube account: ") + ToWide(saveError));
    } else {
        SetStatus(ctx, L"YouTube account unlinked.");
        SetYouTubeUploadUi(ctx, 0, L"No upload in progress.");
    }
}

void RefreshRecordingsList(AppContext* ctx, bool startReconciliation = true)
{
    if (!ctx || !ctx->recordingsList || !ctx->recordingsLabel) {
        return;
    }

    const auto folders = CollectKnownRecordingFolders(ctx);
    const bool anyFolderAvailable = std::any_of(
        folders.begin(),
        folders.end(),
        [](const auto& folder) { return DirectoryExists(folder.wstring()); });
    if (!anyFolderAvailable) {
        if (!ctx->allRecordingItems.empty() || !ctx->recordingItems.empty()) {
            ctx->allRecordingItems.clear();
            ctx->recordingItems.clear();
            ctx->recordingsSelectedIndex = -1;
            RefreshBeanFileList(ctx->recordingsList);
            UpdateRecordingInfoPane(ctx, -1);
        }
        UpdateTransparentStaticText(ctx->recordingsLabel, L"Recordings folder is unavailable.");
        if (startReconciliation) {
            BeginRecordingReconciliation(ctx);
        }
        return;
    }

    // One query for the whole table instead of one per video file. A user with
    // a few hundred recordings previously triggered that many separate queries
    // on every refresh, and refresh happens on tab switches and path edits.
    std::unordered_map<std::string, bean::core::RunRecord> runsByVideoPath;
    if (ctx->runRepository) {
        std::string dbError;
        for (const auto& run : ctx->runRepository->ListRuns(dbError)) {
            runsByVideoPath.emplace(RecordingPathKey(run.videoPath), run);
            for (const auto& alias : run.pathAliases) {
                runsByVideoPath.emplace(RecordingPathKey(alias), run);
            }
        }
    }

    // Reuse metadata for files whose mtime has not changed, so a tab switch
    // does not rebuild participant rows for every historical recording.
    std::unordered_map<std::wstring, const AppContext::RecordingItem*> previousByPath;
    previousByPath.reserve(ctx->allRecordingItems.size());
    for (const auto& item : ctx->allRecordingItems) {
        previousByPath.emplace(item.path.wstring(), &item);
    }

    std::vector<AppContext::RecordingItem> nextItems;
    for (const auto& mediaPath : EnumerateRecordingMediaFilesInFolders(folders)) {
        std::error_code timeEc;
        const auto modified = std::filesystem::last_write_time(mediaPath, timeEc);
        const auto writeTime = timeEc ? std::filesystem::file_time_type::clock::now() : modified;

        const auto previousIt = previousByPath.find(mediaPath.wstring());
        if (previousIt != previousByPath.end() && previousIt->second->modified == writeTime) {
            nextItems.push_back(*previousIt->second);
            continue;
        }

        AppContext::RecordingItem row;
        row.path = mediaPath;
        row.fileName = row.path.filename().wstring();
        row.dungeonName.clear();
        row.modified = writeTime;
        row.dateText = FormatLocalDate(FileTimeToSystemClock(row.modified));

        {
            const auto runIt = runsByVideoPath.find(RecordingPathKey(row.path));
            const std::optional<bean::core::RunRecord> run = runIt == runsByVideoPath.end()
                ? std::nullopt
                : std::optional<bean::core::RunRecord>(runIt->second);
            if (run.has_value()) {
                if (run->dungeonName.has_value() && !run->dungeonName->empty()) {
                    row.dungeonName = ToWide(*run->dungeonName);
                } else if (run->challengeMapId.has_value()) {
                    const auto inferredDungeonName = bean::core::DungeonNameForChallengeMap(*run->challengeMapId);
                    if (!inferredDungeonName.empty()) {
                        row.dungeonName = ToWide(inferredDungeonName);
                    }
                } else if (_stricmp(run->triggerReason.c_str(), "manual") == 0) {
                    row.dungeonName = L"Manual Recording";
                }
                if (run->keystoneLevel.has_value()) {
                    row.keystoneLevel = *run->keystoneLevel;
                    row.keystoneText = L"+" + std::to_wstring(*run->keystoneLevel);
                }
                if (run->recordingEndedAt > run->recordingStartedAt) {
                    row.duration = std::chrono::duration_cast<std::chrono::seconds>(run->recordingEndedAt - run->recordingStartedAt);
                    row.durationText = FormatElapsed(row.duration);
                }
                if (_stricmp(run->result.c_str(), "success") == 0
                    || _stricmp(run->result.c_str(), "timed") == 0
                    || _stricmp(run->stopReason.c_str(), "mythic-success") == 0) {
                    row.outcome = AppContext::RecordingItem::Outcome::Success;
                } else if (_stricmp(run->result.c_str(), "failure") == 0
                    || _stricmp(run->result.c_str(), "depleted") == 0
                    || _stricmp(run->result.c_str(), "overtime") == 0
                    || _stricmp(run->stopReason.c_str(), "mythic-failure") == 0) {
                    row.outcome = AppContext::RecordingItem::Outcome::Failure;
                }
                for (const auto& participant : run->participants) {
                    AppContext::RecordingItem::ParticipantUi participantUi;
                    participantUi.guid = participant.guid;
                    if (participant.name.has_value()) {
                        participantUi.name = ToWide(*participant.name);
                    }
                    participantUi.specAbbrev = SpecAbbreviationFromName(participant.specName);
                    participantUi.specId = participant.specId;
                    participantUi.specName = participant.specName;
                    participantUi.className = participant.className;
                    participantUi.classColor = ClassColorForParticipant(participant.className);
                    row.participants.push_back(std::move(participantUi));
                }
                row.kind = ClassifyRecordingKind(
                    run->triggerReason,
                    run->keystoneLevel.has_value() || run->challengeMapId.has_value());
            }
        }

        if (row.dungeonName.empty()) {
            row.dungeonName = row.path.stem().wstring();
        }
        if (row.keystoneLevel < 0) {
            row.keystoneText = L"-";
        }
        nextItems.push_back(std::move(row));
    }

    ctx->allRecordingItems = std::move(nextItems);
    BackfillRecordingParticipantsFromKnownGuids(ctx);
    SortRecordingItems(ctx);
    ApplyRecordingFilters(ctx);
    if (startReconciliation) {
        BeginRecordingReconciliation(ctx);
    }
}

void RefreshVisibleRecordingFileLists(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->activeTab == AppContext::MainTab::Recordings) {
        RefreshRecordingsList(ctx);
    } else if (ctx->activeTab == AppContext::MainTab::YouTube) {
        RefreshYouTubeMediaList(ctx);
    }
}

void RefreshLiveStatus(AppContext* ctx);

void SetActiveTab(AppContext* ctx, AppContext::MainTab tab)
{
    if (!ctx || !ctx->statusTabButton || !ctx->configurationTabButton || !ctx->chatPrivacyTabButton || !ctx->recordingsTabButton || !ctx->youtubeTabButton || !ctx->clipsTabButton || !ctx->keybindsTabButton || !ctx->aboutTabButton
        || !ctx->statusPanel || !ctx->recorderPanel || !ctx->chatPrivacyPanel || !ctx->recordingsPanel || !ctx->youtubePanel || !ctx->clipsPanel || !ctx->keybindsPanel || !ctx->aboutPanel) {
        return;
    }

    DismissCustomComboPopup();
    if (tab == AppContext::MainTab::Status || tab == AppContext::MainTab::Clips) {
        ctx->ffmpegCheckRequested = true;
    }
    ClearClipsExportStatus(ctx);
    ctx->activeTab = tab;
    const bool showStatus = (tab == AppContext::MainTab::Status);
    const bool showConfiguration = (tab == AppContext::MainTab::Configuration);
    const bool showChatPrivacy = (tab == AppContext::MainTab::ChatPrivacy);
    const bool showRecordings = (tab == AppContext::MainTab::Recordings);
    const bool showYouTube = (tab == AppContext::MainTab::YouTube);
    const bool showClips = (tab == AppContext::MainTab::Clips);
    const bool showKeybinds = (tab == AppContext::MainTab::Keybinds);
    const bool showAbout = (tab == AppContext::MainTab::About);

    ShowWindow(ctx->statusPanel, showStatus ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->recorderPanel, showConfiguration ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->chatPrivacyPanel, showChatPrivacy ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->recordingsPanel, showRecordings ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->youtubePanel, showYouTube ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->clipsPanel, showClips ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->keybindsPanel, showKeybinds ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->aboutPanel, showAbout ? SW_SHOW : SW_HIDE);
    if (!showConfiguration && ctx->configurationTooltip && IsWindow(ctx->configurationTooltip)) {
        ShowWindow(ctx->configurationTooltip, SW_HIDE);
    }
    EnableWindow(ctx->statusTabButton, !showStatus);
    EnableWindow(ctx->configurationTabButton, !showConfiguration);
    EnableWindow(ctx->chatPrivacyTabButton, !showChatPrivacy);
    EnableWindow(ctx->recordingsTabButton, !showRecordings);
    EnableWindow(ctx->youtubeTabButton, !showYouTube);
    EnableWindow(ctx->clipsTabButton, !showClips);
    EnableWindow(ctx->keybindsTabButton, !showKeybinds);
    EnableWindow(ctx->aboutTabButton, !showAbout);

    if (showRecordings) {
        RefreshRecordingsList(ctx);
    }
    if (showYouTube) {
        RefreshYouTubeMediaList(ctx);
    }
    if (showClips) {
        RefreshClipsSourceList(ctx);
        SyncClipTimelineFromPlayback(ctx);
    }
    if (showChatPrivacy) {
        RefreshChatBlockerImageCombo(ctx, {});
        SyncChatBlockerSelectionToImageMetadata(ctx, false);
        RefreshChatBlockerImageControls(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
    }
    if (tab == AppContext::MainTab::Status || tab == AppContext::MainTab::Clips) {
        RefreshLiveStatus(ctx);
    }
}

void OpenRecordingInClipmaker(AppContext* ctx, int recordingIndex)
{
    if (!ctx
        || recordingIndex < 0
        || static_cast<size_t>(recordingIndex) >= ctx->recordingItems.size()) {
        return;
    }

    const auto requestedPath = ctx->recordingItems[static_cast<size_t>(recordingIndex)].path.lexically_normal();
    SetActiveTab(ctx, AppContext::MainTab::Clips);
    if (!ctx->clipsSourceCombo) {
        return;
    }

    int matchingSourceIndex = -1;
    for (size_t index = 0; index < ctx->clipSourceItems.size(); ++index) {
        if (ctx->clipSourceItems[index].lexically_normal() == requestedPath) {
            matchingSourceIndex = static_cast<int>(index);
            break;
        }
    }
    if (matchingSourceIndex < 0) {
        std::error_code pathEc;
        if (std::filesystem::exists(requestedPath, pathEc) && !pathEc) {
            ctx->clipSourceItems.push_back(requestedPath);
            const auto displayName = requestedPath.filename().wstring();
            SendMessageW(ctx->clipsSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(displayName.c_str()));
            matchingSourceIndex = static_cast<int>(ctx->clipSourceItems.size() - 1);
        }
    }
    if (matchingSourceIndex < 0) {
        SetStatus(ctx, L"Selected recording is unavailable in Clipmaker.");
        return;
    }

    SendMessageW(ctx->clipsSourceCombo, CB_SETCURSEL, static_cast<WPARAM>(matchingSourceIndex), 0);
    LoadClipFromSelection(ctx, true);
}

void ApplyAboutUpdateAvailabilityResult(AppContext* ctx, const UpdateAvailabilityPayload& payload)
{
    if (!ctx) {
        return;
    }

    if (payload.requestId != ctx->aboutUpdateCheckRequestId.load()) {
        return;
    }
    ctx->aboutUpdateCheckInProgress.store(false);

    const bool wasUpdateAvailable = ctx->aboutUpdateAvailable;
#ifdef _DEBUG
    // Temporary visual-test override; keep the indicator visible regardless
    // of whether the Debug build can reach the update server.
    ctx->aboutUpdateAvailable = true;
#else
    if (payload.availability == bean::app::UpdateAvailability::UpdateAvailable) {
        ctx->aboutUpdateAvailable = true;
    } else if (payload.availability == bean::app::UpdateAvailability::UpToDate) {
        ctx->aboutUpdateAvailable = false;
    }
#endif
    if (wasUpdateAvailable != ctx->aboutUpdateAvailable && ctx->aboutTabButton) {
        InvalidateRect(ctx->aboutTabButton, nullptr, TRUE);
    }

    HWND updateButton = ctx->aboutPanel ? GetDlgItem(ctx->aboutPanel, IDC_ABOUT_CHECK_UPDATES_BUTTON) : nullptr;
    HWND updateText = ctx->aboutPanel ? GetDlgItem(ctx->aboutPanel, IDC_ABOUT_UPDATE_TEXT) : nullptr;
    if (!updateButton || !updateText) {
        return;
    }
    std::wstring updateTextValue;
    const wchar_t* updateButtonText = L"Check for updates";
    bool reportStatus = false;
    switch (payload.availability) {
    case bean::app::UpdateAvailability::UpdateAvailable:
        updateTextValue = payload.statusMessage;
        updateButtonText = L"Update now";
        reportStatus = true;
        break;
    case bean::app::UpdateAvailability::UpToDate:
        updateTextValue = L"Up to date.";
        break;
    case bean::app::UpdateAvailability::NotConfigured:
        updateTextValue = L"Failed to check for updates";
        reportStatus = true;
        break;
    case bean::app::UpdateAvailability::Failed:
        updateTextValue = L"Failed to check for updates";
        reportStatus = true;
        break;
    }
    UpdateTransparentStaticText(updateText, updateTextValue.c_str());
    if (GetWindowTextString(updateButton) != updateButtonText) {
        SetWindowTextW(updateButton, updateButtonText);
    }
    if (!IsWindowEnabled(updateButton)) {
        EnableWindow(updateButton, TRUE);
    }
    if (reportStatus) {
        SetStatus(ctx, payload.statusMessage);
    }
}

void RefreshAboutUpdateButtonState(AppContext* ctx)
{
    if (!ctx || !ctx->aboutPanel) {
        return;
    }

    HWND updateButton = GetDlgItem(ctx->aboutPanel, IDC_ABOUT_CHECK_UPDATES_BUTTON);
    if (!updateButton) {
        return;
    }
    HWND updateText = GetDlgItem(ctx->aboutPanel, IDC_ABOUT_UPDATE_TEXT);

    if (ctx->aboutUpdateCheckInProgress.exchange(true)) {
        return;
    }

    const std::uint64_t requestId = ctx->aboutUpdateCheckRequestId.fetch_add(1) + 1;
    EnableWindow(updateButton, FALSE);
    SetWindowTextW(updateButton, L"Checking...");
    UpdateTransparentStaticText(updateText, L"Checking for updates...");

    if (!LaunchAppWorker(ctx, [ctx, requestId]() {
        auto* payload = new UpdateAvailabilityPayload();
        payload->requestId = requestId;
        payload->availability = bean::app::GetUpdateAvailability(payload->statusMessage);
        PostOwnedAppMessage(ctx, WM_BEAN_UPDATE_AVAILABILITY_READY, payload);
    })) {
        ctx->aboutUpdateCheckInProgress.store(false, std::memory_order_release);
    }
}

void RefreshStatusCommandButtons(AppContext* ctx);

void RefreshLiveStatus(AppContext* ctx)
{
    if (!ctx || !ctx->orchestrator) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool wasMonitoring = ctx->isMonitoring;
    const bool wasRecording = ctx->isRecording;
    const bool warcraftRecorderWasDetected = ctx->warcraftRecorderDetected;
    bool warcraftRecorderStatusRefreshed = false;
    const auto warcraftRecorderPollInterval = ctx->warcraftRecorderDetected
        ? kWowWindowPollInterval
        : kWarcraftRecorderPollInterval;
    if (!ctx->warcraftRecorderLastCheckedAt.has_value()
        || (now - *ctx->warcraftRecorderLastCheckedAt) >= warcraftRecorderPollInterval) {
        ctx->warcraftRecorderDetected = bean::core::IsWarcraftRecorderRunning();
        ctx->warcraftRecorderLastCheckedAt = now;
        warcraftRecorderStatusRefreshed = true;
    }
    bool monitoring = ctx->orchestrator->IsMonitoring();
    if (ctx->alwaysOnMonitoring
        && !ctx->warcraftRecorderDetected
        && !monitoring
        && (!ctx->monitoringLastStartAttemptAt.has_value()
            || (now - *ctx->monitoringLastStartAttemptAt) >= kMonitoringRetryInterval)) {
        ctx->monitoringLastStartAttemptAt = now;
        ctx->orchestrator->ApplySettings(ctx->settings);
        std::string monitoringError;
        ctx->orchestrator->StartMonitoring(monitoringError);
        monitoring = ctx->orchestrator->IsMonitoring();
    } else if (monitoring) {
        ctx->monitoringLastStartAttemptAt.reset();
    }
    const bool recording = (ctx->orchestrator->GetState() == bean::core::OrchestratorState::Recording);
    const auto recordingSessionId = ctx->orchestrator->GetRecordingSessionId();
    const auto previousRecordingSessionId = ctx->activeRecordingSessionId;

    if (recording && (!ctx->isRecording || ctx->activeRecordingSessionId != recordingSessionId)) {
        ctx->recordingStartedAt = std::chrono::steady_clock::now();
        ctx->activeRecordingSessionId = recordingSessionId;
    } else if (!recording) {
        ctx->recordingStartedAt.reset();
        ctx->activeRecordingSessionId = 0;
    }
    ctx->isRecording = recording;
    if (!monitoring) {
        ctx->autoRecordFailed = false;
    }
    const bool recordingStateChanged = (wasRecording != ctx->isRecording);
    // Tab switch already rebuilds these lists. If Recordings or YouTube is
    // already open, a just-finished file would otherwise stay invisible until
    // the user hits Refresh. Mythic restarts also finish a file without a
    // recording->idle transition, so treat a session-id change the same way.
    const bool finishedRecordingShouldAppear =
        (wasRecording && !recording)
        || (wasRecording && recording && previousRecordingSessionId != 0
            && previousRecordingSessionId != recordingSessionId);
    if (finishedRecordingShouldAppear) {
        RefreshVisibleRecordingFileLists(ctx);
        BeginDiskSpaceProbe(ctx);
    }
    const bool wowWasDetected = ctx->wowWindowDetected;
    const auto wowWasEdition = ctx->detectedWowEdition;
    const int wowWasWidth = ctx->detectedWowClientWidth;
    const int wowWasHeight = ctx->detectedWowClientHeight;
    bool wowStatusRefreshed = false;
    if (!ctx->isRecording
        && (!ctx->wowWindowLastCheckedAt.has_value()
            || (now - *ctx->wowWindowLastCheckedAt) >= kWowWindowPollInterval)) {
        const auto wowSize = GetWowClientSizeForUi();
        ctx->wowWindowDetected = wowSize.has_value();
        ctx->wowBothInstancesDetected =
            wowSize.has_value() && wowSize->bothInstancesDetected;
        ctx->detectedWowEdition = wowSize.has_value()
            ? wowSize->edition
            : bean::core::WowEdition::Unknown;
        ctx->detectedWowClientWidth = wowSize.has_value() ? wowSize->width : 0;
        ctx->detectedWowClientHeight = wowSize.has_value() ? wowSize->height : 0;
        ctx->settings.detectedWowClientWidth = ctx->detectedWowClientWidth;
        ctx->settings.detectedWowClientHeight = ctx->detectedWowClientHeight;
        ctx->settings.detectedWowEdition = ctx->detectedWowEdition;
        ctx->orchestrator->ApplySettings(ctx->settings);
        ctx->wowWindowLastCheckedAt = now;
        wowStatusRefreshed = true;
    }
    if (wowStatusRefreshed
        && wowWasEdition != ctx->detectedWowEdition
        && monitoring
        && !ctx->isRecording) {
        // The watcher owns an open handle to the selected flavor's directory.
        // Restart it when the player switches between Retail and PTR.
        ctx->orchestrator->StopMonitoring();
        monitoring = false;
        if (ctx->alwaysOnMonitoring) {
            std::string monitoringError;
            ctx->orchestrator->ApplySettings(ctx->settings);
            ctx->orchestrator->StartMonitoring(monitoringError);
            monitoring = ctx->orchestrator->IsMonitoring();
        }
    }
    ctx->isMonitoring = monitoring;
    const bool monitoringStateChanged = (wasMonitoring != ctx->isMonitoring);
    const bool obsWasDetected = ctx->obsInstallDetected;
    if (!ctx->obsInstallLastCheckedAt.has_value()
        || (now - *ctx->obsInstallLastCheckedAt) >= kObsInstallPollInterval) {
        ctx->obsInstallDetected = bean::core::IsUsableObsInstallPresent();
        ctx->obsInstallLastCheckedAt = now;
    }
    const bool ffmpegWasDetected = ctx->ffmpegDetected;
    // ffmpegStatusRefreshed stays false here: the probe is asynchronous, and the
    // UI is repainted when WM_BEAN_FFMPEG_PROBE_COMPLETE arrives instead.
    const bool ffmpegStatusRefreshed = false;
    if (ctx->ffmpegCheckRequested
        || (!ctx->ffmpegDetected
            && (!ctx->ffmpegLastCheckedAt.has_value()
                || (now - *ctx->ffmpegLastCheckedAt) >= kObsInstallPollInterval))) {
        ctx->ffmpegLastCheckedAt = now;
        ctx->ffmpegCheckRequested = false;
        BeginFfmpegProbe(ctx);
    }
    const bool advancedCombatLoggingWasEnabled = ctx->advancedCombatLoggingEnabled;
    if (!ctx->advancedCombatLoggingLastCheckedAt.has_value()
        || (now - *ctx->advancedCombatLoggingLastCheckedAt) >= kWowWindowPollInterval) {
        ctx->advancedCombatLoggingEnabled = DetectAdvancedCombatLoggingForUi(ctx);
        ctx->advancedCombatLoggingLastCheckedAt = now;
    }

    if (ctx->monitorIcon && monitoringStateChanged) {
        InvalidateRect(ctx->monitorIcon, nullptr, FALSE);
    }
    if (ctx->recordIcon && recordingStateChanged) {
        InvalidateRect(ctx->recordIcon, nullptr, FALSE);
    }
    if (ctx->wowWindowIcon && wowWasDetected != ctx->wowWindowDetected) {
        InvalidateRect(ctx->wowWindowIcon, nullptr, FALSE);
    }
    if (ctx->wowWindowText) {
        std::wstring wowStatusText;
        if (!ctx->wowWindowDetected) {
            wowStatusText = L"WoW window not detected";
        } else if (ctx->wowBothInstancesDetected) {
            wowStatusText = L"Warning: Retail and PTR windows detected; using PTR";
        } else {
            wowStatusText =
                L"WoW (" + ToWide(bean::core::WowEditionLabel(ctx->detectedWowEdition))
                + L") window detected";
        }
        UpdateTransparentStaticText(ctx->wowWindowText, wowStatusText.c_str());
    }
    if (ctx->gameResolutionText) {
        const std::wstring gameResolutionText = ctx->wowWindowDetected
            ? std::to_wstring(ctx->detectedWowClientWidth) + L" \u00D7 "
                + std::to_wstring(ctx->detectedWowClientHeight)
            : L"Unavailable - WoW not detected";
        UpdateTransparentStaticText(ctx->gameResolutionText, gameResolutionText.c_str());
    }
    if (recordingStateChanged
        || wowWasWidth != ctx->detectedWowClientWidth
        || wowWasHeight != ctx->detectedWowClientHeight) {
        RefreshRecordingResolutionOptions(ctx);
    }
    if (ctx->obsInstallIcon && obsWasDetected != ctx->obsInstallDetected) {
        InvalidateRect(ctx->obsInstallIcon, nullptr, FALSE);
    }
    if (ctx->obsInstallText) {
        const wchar_t* obsStatusText = ctx->obsInstallDetected ? L"OBS install detected" : L"OBS install not detected";
        UpdateTransparentStaticText(ctx->obsInstallText, obsStatusText);
    }
    if (ctx->ffmpegIcon && ffmpegWasDetected != ctx->ffmpegDetected) {
        InvalidateRect(ctx->ffmpegIcon, nullptr, FALSE);
    }
    if (ctx->ffmpegText) {
        const wchar_t* ffmpegStatusText = ctx->ffmpegDetected ? L"FFmpeg available for trim" : L"FFmpeg not found for trim";
        UpdateTransparentStaticText(ctx->ffmpegText, ffmpegStatusText);
    }
    if (ffmpegStatusRefreshed || ffmpegWasDetected != ctx->ffmpegDetected) {
        RefreshClipsPlaybackControls(ctx);
    }
    bool warcraftRecorderRowVisibilityChanged = false;
    HWND warcraftRecorderLabel = GetDlgItem(ctx->statusPanel, IDC_WARCRAFT_RECORDER_LABEL);
    if (warcraftRecorderLabel) {
        const bool shouldShow = ctx->warcraftRecorderDetected;
        const bool currentlyVisible = IsWindowVisible(warcraftRecorderLabel) != FALSE;
        if (shouldShow != currentlyVisible) {
            ShowWindow(warcraftRecorderLabel, shouldShow ? SW_SHOW : SW_HIDE);
            warcraftRecorderRowVisibilityChanged = true;
        }
    }
    const bool warcraftRecorderStateChanged =
        warcraftRecorderWasDetected != ctx->warcraftRecorderDetected;
    if (ctx->warcraftRecorderIcon && warcraftRecorderStateChanged) {
        const bool shouldShow = ctx->warcraftRecorderDetected;
        const bool currentlyVisible = IsWindowVisible(ctx->warcraftRecorderIcon) != FALSE;
        if (shouldShow != currentlyVisible) {
            ShowWindow(ctx->warcraftRecorderIcon, shouldShow ? SW_SHOW : SW_HIDE);
            warcraftRecorderRowVisibilityChanged = true;
        }
        InvalidateRect(ctx->warcraftRecorderIcon, nullptr, FALSE);
    }
    if (ctx->warcraftRecorderText && warcraftRecorderStateChanged) {
        const bool shouldShow = ctx->warcraftRecorderDetected;
        const bool currentlyVisible = IsWindowVisible(ctx->warcraftRecorderText) != FALSE;
        if (shouldShow != currentlyVisible) {
            ShowWindow(ctx->warcraftRecorderText, shouldShow ? SW_SHOW : SW_HIDE);
            warcraftRecorderRowVisibilityChanged = true;
        }
        if (ctx->warcraftRecorderDetected) {
            UpdateTransparentStaticText(ctx->warcraftRecorderText, L"Detected - close Warcraft Recorder to avoid recording conflicts");
        }
    }
    if (warcraftRecorderRowVisibilityChanged && ctx->mainWindow) {
        RECT clientRect{};
        if (GetClientRect(ctx->mainWindow, &clientRect)) {
            LayoutMainUi(ctx, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
        }
    }
    if (ctx->advancedLoggingIcon
        && advancedCombatLoggingWasEnabled != ctx->advancedCombatLoggingEnabled) {
        InvalidateRect(ctx->advancedLoggingIcon, nullptr, FALSE);
    }
    if (ctx->advancedLoggingText) {
        const wchar_t* advancedLoggingText = ctx->advancedCombatLoggingEnabled
            ? L"Enabled"
            : L"Disabled";
        UpdateTransparentStaticText(ctx->advancedLoggingText, advancedLoggingText);
        if (ctx->advancedLoggingHelpIcon) {
            RECT textRect{};
            if (GetWindowRect(ctx->advancedLoggingText, &textRect)) {
                HWND parent = GetParent(ctx->advancedLoggingText);
                if (parent) {
                    MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&textRect), 2);
                    HDC dc = GetDC(ctx->advancedLoggingText);
                    if (dc) {
                        HGDIOBJ oldFont = nullptr;
                        HFONT font = reinterpret_cast<HFONT>(SendMessageW(ctx->advancedLoggingText, WM_GETFONT, 0, 0));
                        if (font) {
                            oldFont = SelectObject(dc, font);
                        }
                        SIZE textSize{};
                        const int textLength = lstrlenW(advancedLoggingText);
                        if (GetTextExtentPoint32W(dc, advancedLoggingText, textLength, &textSize)) {
                            const int helpX = textRect.left + textSize.cx + 6;
                            MoveWindow(ctx->advancedLoggingHelpIcon, helpX, textRect.top + 4, 20, 20, TRUE);
                        }
                        if (oldFont) {
                            SelectObject(dc, oldFont);
                        }
                        ReleaseDC(ctx->advancedLoggingText, dc);
                    }
                }
            }
        }
    }
    if (ctx->statusTabButton
        && (wowWasDetected != ctx->wowWindowDetected
            || wowWasEdition != ctx->detectedWowEdition
            || obsWasDetected != ctx->obsInstallDetected
            || ffmpegWasDetected != ctx->ffmpegDetected
            || warcraftRecorderWasDetected != ctx->warcraftRecorderDetected
            || advancedCombatLoggingWasEnabled != ctx->advancedCombatLoggingEnabled)) {
        InvalidateRect(ctx->statusTabButton, nullptr, FALSE);
    }
    if (ctx->warcraftRecorderDetected) {
        // Set the guard before appending because the custom text box emits
        // EN_CHANGE for programmatic EM_REPLACESEL updates.
        if (!ctx->warcraftRecorderWarningLogged && ctx->statusText) {
            ctx->warcraftRecorderWarningLogged = true;
            SetStatus(ctx, L"Warning: Warcraft Recorder is running. Close it while using Bean to avoid recording conflicts.");
        }
    } else {
        if (warcraftRecorderStatusRefreshed && warcraftRecorderWasDetected) {
            SetStatus(ctx, L"Warcraft Recorder no longer detected.");
        }
        ctx->warcraftRecorderWarningLogged = false;
    }
    if (ctx->lengthValue) {
        const std::wstring displayedLength = ctx->recordingStartedAt.has_value()
            ? FormatElapsed(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - *ctx->recordingStartedAt))
            : L"00:00:00";
        if (ctx->displayedRecordingLength != displayedLength) {
            ctx->displayedRecordingLength = displayedLength;
            SetWindowTextW(ctx->lengthValue, displayedLength.c_str());
            InvalidateRect(ctx->lengthValue, nullptr, FALSE);
        }
    }
    if (recordingStateChanged && ctx->chatPreview) {
        ctx->chatPreviewLastInvalidateAt.reset();
        InvalidateRect(ctx->chatPreview, nullptr, FALSE);
    }
    if (ctx->chatPreview && ctx->activeTab == AppContext::MainTab::ChatPrivacy && !ctx->isRecording) {
        if (!ctx->chatPreviewLastInvalidateAt.has_value()
            || (now - *ctx->chatPreviewLastInvalidateAt) >= kChatPreviewInvalidateInterval) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
            ctx->chatPreviewLastInvalidateAt = now;
        }
    } else {
        ctx->chatPreviewLastInvalidateAt.reset();
    }

    ApplyTaskbarOverlayState(ctx);
    RefreshStatusCommandButtons(ctx);
}

void RefreshStatusCommandButtons(AppContext* ctx)
{
    if (!ctx || !ctx->statusPanel) {
        return;
    }

    HWND recordStart = GetDlgItem(ctx->statusPanel, IDC_RECORD_START);
    HWND recordStop = GetDlgItem(ctx->statusPanel, IDC_RECORD_STOP);

    if (recordStart) {
        const BOOL shouldEnable = ctx->isRecording ? FALSE : TRUE;
        if (IsWindowEnabled(recordStart) != shouldEnable) {
            EnableWindow(recordStart, shouldEnable);
        }
    }
    if (recordStop) {
        const BOOL shouldEnable = ctx->isRecording ? TRUE : FALSE;
        if (IsWindowEnabled(recordStop) != shouldEnable) {
            EnableWindow(recordStop, shouldEnable);
        }
    }
}

void PullSettingsFromUi(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    ctx->settings.outputDirectory = ToUtf8(GetWindowTextString(ctx->outputEdit));
    ctx->settings.wowInstallDirectory = ToUtf8(GetWindowTextString(ctx->wowLogEdit));
    if (ctx->recordingResolutionCombo) {
        const LRESULT selectedIndex = SendMessageW(ctx->recordingResolutionCombo, CB_GETCURSEL, 0, 0);
        if (selectedIndex >= 0) {
            const LRESULT selectedHeight = SendMessageW(
                ctx->recordingResolutionCombo,
                CB_GETITEMDATA,
                static_cast<WPARAM>(selectedIndex),
                0);
            if (selectedHeight != CB_ERR) {
                ctx->settings.recordingResolutionHeight = static_cast<int>(selectedHeight);
            }
        }
    }
    ctx->settings.fps = ReadIntControl(ctx->fpsEdit, 60);
    ctx->settings.postRunStopDelaySeconds = (std::max)(0, ReadIntControl(ctx->postRunDelayEdit, 30));
    ctx->settings.clipDurationSeconds = (std::clamp)(ReadIntControl(ctx->clipDurationEdit, 30), 1, 3600);
    const bool customBlockerImage = ctx->chatBlockerImageCustomRadio
        && SendMessageW(ctx->chatBlockerImageCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ctx->settings.chatBlockerCustomImagePath = customBlockerImage ? ResolveSelectedChatBlockerImagePath(ctx) : std::filesystem::path();
    ctx->settings.chatBlockerUseCustomImage = customBlockerImage && !ctx->settings.chatBlockerCustomImagePath.empty();
    ctx->settings.chatBlockerCustomImageSourceWidth = customBlockerImage ? (std::max)(0, ctx->chatBlockerCustomSourceWidth) : 0;
    ctx->settings.chatBlockerCustomImageSourceHeight = customBlockerImage ? (std::max)(0, ctx->chatBlockerCustomSourceHeight) : 0;
    ctx->settings.chatBlockerWidth = (std::max)(0, ReadIntControl(ctx->chatBlockerWidthEdit, ctx->settings.chatBlockerWidth));
    ctx->settings.chatBlockerHeight = (std::max)(0, ReadIntControl(ctx->chatBlockerHeightEdit, ctx->settings.chatBlockerHeight));
    RememberChatBlockerSizeForSelectedImage(ctx);
    ctx->settings.chatBlockerAnchor = ChatBlockerAnchorFromComboIndex(static_cast<int>(SendMessageW(ctx->chatBlockerAnchorCombo, CB_GETCURSEL, 0, 0)));
    ctx->settings.chatBlockerEnabled = (SendMessageW(ctx->chatBlockerEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);

    const int encoderIndex = static_cast<int>(SendMessageW(ctx->encoderCombo, CB_GETCURSEL, 0, 0));
    if (encoderIndex == 1) {
        ctx->settings.videoEncoder = "nvenc";
    } else if (encoderIndex == 2) {
        ctx->settings.videoEncoder = "amf";
    } else if (encoderIndex == 3) {
        ctx->settings.videoEncoder = "qsv";
    } else if (encoderIndex == 4) {
        ctx->settings.videoEncoder = "x264";
    } else {
        ctx->settings.videoEncoder = "gpu_auto";
    }

    const int presetIndex = static_cast<int>(SendMessageW(ctx->presetCombo, CB_GETCURSEL, 0, 0));
    if (presetIndex == 0) {
        ctx->settings.encoderPreset = "ultra";
    } else if (presetIndex == 1) {
        ctx->settings.encoderPreset = "high";
    } else if (presetIndex == 2) {
        ctx->settings.encoderPreset = "medium";
    } else if (presetIndex == 3) {
        ctx->settings.encoderPreset = "low";
    } else {
        ctx->settings.encoderPreset = "minimum";
    }

    const int containerIndex = static_cast<int>(SendMessageW(ctx->containerCombo, CB_GETCURSEL, 0, 0));
    ctx->settings.videoContainer = (containerIndex == 1) ? "mp4" : "mkv";
    if (SendMessageW(ctx->audioScopeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        ctx->settings.audioCaptureScope = bean::core::AppSettings::AudioCaptureScope::WowOnly;
    } else if (ctx->audioScopeWowDiscordRadio && SendMessageW(ctx->audioScopeWowDiscordRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        ctx->settings.audioCaptureScope = bean::core::AppSettings::AudioCaptureScope::WowAndDiscord;
    } else {
        ctx->settings.audioCaptureScope = bean::core::AppSettings::AudioCaptureScope::AllDesktop;
    }
    ctx->settings.captureMicrophone = (SendMessageW(ctx->microphoneCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ctx->settings.microphoneNoiseSuppression =
        (ctx->microphoneNoiseSuppressionCheck
            && SendMessageW(ctx->microphoneNoiseSuppressionCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const int microphoneIndex = static_cast<int>(SendMessageW(ctx->microphoneCombo, CB_GETCURSEL, 0, 0));
    if (microphoneIndex >= 0 && static_cast<size_t>(microphoneIndex) < ctx->microphoneOptions.size()) {
        ctx->settings.microphoneDeviceId = ctx->microphoneOptions[static_cast<size_t>(microphoneIndex)].deviceId;
    } else if (ctx->settings.microphoneDeviceId.empty()) {
        ctx->settings.microphoneDeviceId = "default";
    }
}

void CommitChatBlockerSettings(AppContext* ctx)
{
    if (!ctx || !ctx->orchestrator || !ctx->chatBlockerAutoSaveArmed) {
        return;
    }

    const bool customBlockerImage = ctx->chatBlockerImageCustomRadio
        && SendMessageW(ctx->chatBlockerImageCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ctx->settings.chatBlockerCustomImagePath = customBlockerImage ? ResolveSelectedChatBlockerImagePath(ctx) : std::filesystem::path();
    ctx->settings.chatBlockerUseCustomImage = customBlockerImage && !ctx->settings.chatBlockerCustomImagePath.empty();
    ctx->settings.chatBlockerCustomImageSourceWidth = customBlockerImage ? (std::max)(0, ctx->chatBlockerCustomSourceWidth) : 0;
    ctx->settings.chatBlockerCustomImageSourceHeight = customBlockerImage ? (std::max)(0, ctx->chatBlockerCustomSourceHeight) : 0;
    ctx->settings.chatBlockerWidth = (std::max)(0, ReadIntControl(ctx->chatBlockerWidthEdit, ctx->settings.chatBlockerWidth));
    ctx->settings.chatBlockerHeight = (std::max)(0, ReadIntControl(ctx->chatBlockerHeightEdit, ctx->settings.chatBlockerHeight));
    RememberChatBlockerSizeForSelectedImage(ctx);
    const int anchorIndex = static_cast<int>(SendMessageW(ctx->chatBlockerAnchorCombo, CB_GETCURSEL, 0, 0));
    ctx->settings.chatBlockerAnchor = ChatBlockerAnchorFromComboIndex(anchorIndex);
    ctx->settings.chatBlockerEnabled = (SendMessageW(ctx->chatBlockerEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);

    std::string error;
    if (!ctx->settingsStore.Save(ctx->settings, error)) {
        SetStatus(ctx, std::wstring(L"Auto-save failed: ") + ToWide(error));
        return;
    }
    ctx->chatBlockerSettingsDirty = false;
    ctx->orchestrator->ApplySettings(ctx->settings);
}

void CommitConfigurationSettings(AppContext* ctx)
{
    if (!ctx || !ctx->orchestrator || !ctx->configurationAutoSaveArmed) {
        return;
    }

    const auto previousWowInstallDirectory = ctx->settings.wowInstallDirectory;
    PullSettingsFromUi(ctx);

    std::string error;
    if (!ctx->settingsStore.Save(ctx->settings, error)) {
        SetStatus(ctx, std::wstring(L"Auto-save failed: ") + ToWide(error));
        return;
    }
    ctx->configurationSettingsDirty = false;
    ctx->orchestrator->ApplySettings(ctx->settings);
    BeginDiskSpaceProbe(ctx);
    if (ctx->orchestrator->IsMonitoring()
        && !ctx->isRecording
        && previousWowInstallDirectory != ctx->settings.wowInstallDirectory) {
        ctx->orchestrator->StopMonitoring();
        std::string monitoringError;
        ctx->orchestrator->StartMonitoring(monitoringError);
    }
}

// Restarting an existing timer resets its countdown, so repeated calls while the
// user is still typing collapse into a single save once they pause.
void ScheduleAutoSave(AppContext* ctx, UINT_PTR timerId)
{
    if (!ctx
        || ctx->shuttingDown.load(std::memory_order_acquire)
        || !ctx->mainWindow) {
        return;
    }
    if (timerId == kChatBlockerAutoSaveTimerId) {
        ctx->chatBlockerSettingsDirty = true;
    } else if (timerId == kConfigurationAutoSaveTimerId) {
        ctx->configurationSettingsDirty = true;
    }
    SetTimer(ctx->mainWindow, timerId, kAutoSaveDebounceMs, nullptr);
}

void AutoSaveChatBlockerSettings(AppContext* ctx)
{
    ScheduleAutoSave(ctx, kChatBlockerAutoSaveTimerId);
}

void AutoSaveConfigurationSettings(AppContext* ctx)
{
    ScheduleAutoSave(ctx, kConfigurationAutoSaveTimerId);
}

// Runs any save still sitting in the debounce window. Called before the window
// goes away so a quick edit-then-close cannot lose the change.
void FlushPendingAutoSaves(AppContext* ctx)
{
    if (!ctx || !ctx->mainWindow) {
        return;
    }
    if (KillTimer(ctx->mainWindow, kConfigurationAutoSaveTimerId)
        || ctx->configurationSettingsDirty) {
        CommitConfigurationSettings(ctx);
    }
    if (KillTimer(ctx->mainWindow, kChatBlockerAutoSaveTimerId)
        || ctx->chatBlockerSettingsDirty) {
        CommitChatBlockerSettings(ctx);
    }
}

void ApplyFolderAvailabilityResult(AppContext* ctx, const FolderAvailabilityResult& result)
{
    if (!ctx) {
        return;
    }

    const bool wasOutputAvailable = ctx->outputAvailable;
    const bool wasWowLogAvailable = ctx->wowLogAvailable;
    const bool wasOutputFolderWillBeCreated = ctx->outputFolderWillBeCreatedOnRecordStart;
    const bool wasConfigurationValid =
        (wasOutputAvailable || wasOutputFolderWillBeCreated) && wasWowLogAvailable;

    ctx->outputAvailable = result.outputAvailable;
    ctx->wowLogAvailable = result.wowLogAvailable;
    ctx->outputFolderWillBeCreatedOnRecordStart = result.outputFolderWillBeCreatedOnRecordStart;
    const bool outputStatusChanged =
        wasOutputAvailable != ctx->outputAvailable
        || wasOutputFolderWillBeCreated != ctx->outputFolderWillBeCreatedOnRecordStart;
    const bool wowLogStatusChanged = wasWowLogAvailable != ctx->wowLogAvailable;
    const bool configurationValid =
        (ctx->outputAvailable || ctx->outputFolderWillBeCreatedOnRecordStart)
        && ctx->wowLogAvailable;

    if (ctx->outputStatus) {
        const wchar_t* outputStatusText = ctx->outputAvailable
            ? L"\x2713"
            : (ctx->outputFolderWillBeCreatedOnRecordStart ? L"(!)" : L"X");
        if (outputStatusChanged || GetWindowTextString(ctx->outputStatus) != outputStatusText) {
            SetWindowTextW(ctx->outputStatus, outputStatusText);
            InvalidateRect(ctx->outputStatus, nullptr, FALSE);
        }
    }
    if (ctx->wowLogStatus) {
        const wchar_t* wowLogStatusText = ctx->wowLogAvailable ? L"\x2713" : L"X";
        if (wowLogStatusChanged || GetWindowTextString(ctx->wowLogStatus) != wowLogStatusText) {
            SetWindowTextW(ctx->wowLogStatus, wowLogStatusText);
            InvalidateRect(ctx->wowLogStatus, nullptr, FALSE);
        }
    }
    if (ctx->configurationTabButton && wasConfigurationValid != configurationValid) {
        InvalidateRect(ctx->configurationTabButton, nullptr, FALSE);
    }
    if (ctx->activeTab == AppContext::MainTab::Recordings) {
        RefreshRecordingsList(ctx);
    } else if (ctx->activeTab == AppContext::MainTab::YouTube) {
        RefreshYouTubeMediaList(ctx);
    } else if (ctx->activeTab == AppContext::MainTab::Clips) {
        RefreshClipsSourceList(ctx);
    }
}

bool ApplyReasonableDefaults(bean::core::AppSettings& settings, std::string& warning)
{
    warning.clear();
    bool changed = false;

    if (settings.outputDirectory.empty()) {
        const auto videosPath = GetKnownFolderPath(FOLDERID_Videos);
        if (!videosPath.empty()) {
            const auto output = std::filesystem::path(videosPath) / "Bean";
            std::error_code ec;
            std::filesystem::create_directories(output, ec);
            if (!ec) {
                settings.outputDirectory = output;
                changed = true;
            } else {
                warning = "Could not create default output folder in Videos.";
            }
        } else {
            warning = "Could not resolve Videos folder for default output path.";
        }
    }

    if (settings.wowInstallDirectory.empty()) {
        settings.wowInstallDirectory = bean::core::ResolveDefaultWowInstallDirectory();
        changed = true;
    }
    if (settings.videoEncoder.empty()) {
        settings.videoEncoder = "gpu_auto";
        changed = true;
    }
    if (settings.encoderPreset == "quality") {
        settings.encoderPreset = "high";
        changed = true;
    } else if (settings.encoderPreset == "balanced") {
        settings.encoderPreset = "medium";
        changed = true;
    } else if (settings.encoderPreset == "speed") {
        settings.encoderPreset = "low";
        changed = true;
    } else if (settings.encoderPreset != "ultra"
        && settings.encoderPreset != "high"
        && settings.encoderPreset != "medium"
        && settings.encoderPreset != "low"
        && settings.encoderPreset != "minimum") {
        settings.encoderPreset = "high";
        changed = true;
    }
    if (settings.videoContainer.empty()) {
        settings.videoContainer = "mp4";
        changed = true;
    }
    if (settings.postRunStopDelaySeconds < 0) {
        settings.postRunStopDelaySeconds = 30;
        changed = true;
    }
    if (settings.clipDurationSeconds < 1 || settings.clipDurationSeconds > 3600) {
        settings.clipDurationSeconds = 30;
        changed = true;
    }
    if (settings.chatBlockerWidth < 0) {
        settings.chatBlockerWidth = 0;
        changed = true;
    }
    if (settings.chatBlockerHeight < 0) {
        settings.chatBlockerHeight = 0;
        changed = true;
    }
    if (settings.chatBlockerCustomImageSourceWidth < 0) {
        settings.chatBlockerCustomImageSourceWidth = 0;
        changed = true;
    }
    if (settings.chatBlockerCustomImageSourceHeight < 0) {
        settings.chatBlockerCustomImageSourceHeight = 0;
        changed = true;
    }
    if (!settings.chatBlockerUseCustomImage) {
        if (settings.chatBlockerCustomImageSourceWidth != 0 || settings.chatBlockerCustomImageSourceHeight != 0) {
            settings.chatBlockerCustomImageSourceWidth = 0;
            settings.chatBlockerCustomImageSourceHeight = 0;
            changed = true;
        }
    } else if (settings.chatBlockerCustomImagePath.empty()) {
        settings.chatBlockerUseCustomImage = false;
        settings.chatBlockerCustomImageSourceWidth = 0;
        settings.chatBlockerCustomImageSourceHeight = 0;
        changed = true;
    } else {
        std::error_code customImageEc;
        if (!std::filesystem::exists(settings.chatBlockerCustomImagePath, customImageEc) || customImageEc) {
            settings.chatBlockerUseCustomImage = false;
            settings.chatBlockerCustomImagePath.clear();
            settings.chatBlockerCustomImageSourceWidth = 0;
            settings.chatBlockerCustomImageSourceHeight = 0;
            changed = true;
        }
    }
    if (settings.microphoneDeviceId.empty()) {
        settings.microphoneDeviceId = "default";
        changed = true;
    }
    const auto* theme = FindThemeDefinition(settings.theme);
    if (!theme || settings.theme != theme->id) {
        settings.theme = bean::core::kDefaultTheme;
        changed = true;
    }

    return changed;
}

void ApplySelectedTheme(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    const auto* theme = FindThemeDefinition(ctx->settings.theme);
    if (!theme) {
        theme = &kThemeDefinitions.front();
    }
    ctx->settings.theme = theme->id;
    kThemeColors = theme->colors;
    RebuildThemeColorResources();

    if (ctx->mainWindow) {
        ApplyDarkTitleBar(ctx->mainWindow);
    }
    if (ctx->recordingsInfoText) {
        InvalidateRect(ctx->recordingsInfoText, nullptr, FALSE);
    }
    if (ctx->youtubeUploadProgress) {
        SendMessageW(ctx->youtubeUploadProgress, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(kColorListSelection));
        SendMessageW(ctx->youtubeUploadProgress, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(kColorInputBg));
    }
    if (ctx->clipsPreviewEngine) {
        ApplyClipVideoWindowBounds(ctx);
    }
    if (ctx->customizeThemeCombo) {
        SendMessageW(
            ctx->customizeThemeCombo,
            CB_SETCURSEL,
            static_cast<WPARAM>(ThemeIndexForId(ctx->settings.theme)),
            0);
    }
    if (ctx->mainWindow) {
        RedrawWindow(
            ctx->mainWindow,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        UpdateWindow(ctx->mainWindow);
    }
}

void SaveThemeSettings(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    std::string error;
    if (!ctx->settingsStore.Save(ctx->settings, error)) {
        SetStatus(ctx, std::wstring(L"Theme save failed: ") + ToWide(error));
    }
}

void PushSettingsToUi(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    SetWindowTextW(ctx->outputEdit, ToWide(ctx->settings.outputDirectory.string()).c_str());
    SetWindowTextW(ctx->wowLogEdit, ToWide(ctx->settings.wowInstallDirectory.string()).c_str());
    ctx->detectedWowClientWidth = ctx->settings.detectedWowClientWidth;
    ctx->detectedWowClientHeight = ctx->settings.detectedWowClientHeight;
    ctx->detectedWowEdition = ctx->settings.detectedWowEdition;
    ctx->wowWindowDetected = ctx->detectedWowClientWidth > 0 && ctx->detectedWowClientHeight > 0;
    RefreshRecordingResolutionOptions(ctx);
    SetWindowTextW(ctx->fpsEdit, ToWide(std::to_string(ctx->settings.fps)).c_str());
    SetWindowTextW(ctx->postRunDelayEdit, ToWide(std::to_string(ctx->settings.postRunStopDelaySeconds)).c_str());
    SetWindowTextW(ctx->clipDurationEdit, ToWide(std::to_string(ctx->settings.clipDurationSeconds)).c_str());
    PushKeybindsToUi(ctx);
    if (ctx->customizeThemeCombo) {
        SendMessageW(
            ctx->customizeThemeCombo,
            CB_SETCURSEL,
            static_cast<WPARAM>(ThemeIndexForId(ctx->settings.theme)),
            0);
    }
    const std::wstring selectedCustomImageFileName = ctx->settings.chatBlockerCustomImagePath.filename().wstring();
    RefreshChatBlockerImageCombo(ctx, selectedCustomImageFileName);
    SendMessageW(ctx->chatBlockerImageBlankRadio, BM_SETCHECK, ctx->settings.chatBlockerUseCustomImage ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(ctx->chatBlockerImageCustomRadio, BM_SETCHECK, ctx->settings.chatBlockerUseCustomImage ? BST_CHECKED : BST_UNCHECKED, 0);
    ctx->chatBlockerCustomSourceWidth = (std::max)(0, ctx->settings.chatBlockerCustomImageSourceWidth);
    ctx->chatBlockerCustomSourceHeight = (std::max)(0, ctx->settings.chatBlockerCustomImageSourceHeight);
    SyncChatBlockerSelectionToImageMetadata(ctx, false);
    SetWindowTextW(ctx->chatBlockerWidthEdit, ToWide(std::to_string(ctx->settings.chatBlockerWidth)).c_str());
    SetWindowTextW(ctx->chatBlockerHeightEdit, ToWide(std::to_string(ctx->settings.chatBlockerHeight)).c_str());
    SendMessageW(ctx->chatBlockerAnchorCombo, CB_SETCURSEL, static_cast<WPARAM>(ChatBlockerAnchorToComboIndex(ctx->settings.chatBlockerAnchor)), 0);
    SendMessageW(ctx->chatBlockerEnabledCheck, BM_SETCHECK, ctx->settings.chatBlockerEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    RefreshChatBlockerImageControls(ctx);

    int encoderIndex = 0;
    if (ctx->settings.videoEncoder == "nvenc") {
        encoderIndex = 1;
    } else if (ctx->settings.videoEncoder == "amf") {
        encoderIndex = 2;
    } else if (ctx->settings.videoEncoder == "qsv") {
        encoderIndex = 3;
    } else if (ctx->settings.videoEncoder == "x264") {
        encoderIndex = 4;
    }
    SendMessageW(ctx->encoderCombo, CB_SETCURSEL, static_cast<WPARAM>(encoderIndex), 0);

    int presetIndex = 1;
    if (ctx->settings.encoderPreset == "ultra") {
        presetIndex = 0;
    } else if (ctx->settings.encoderPreset == "medium") {
        presetIndex = 2;
    } else if (ctx->settings.encoderPreset == "low") {
        presetIndex = 3;
    } else if (ctx->settings.encoderPreset == "minimum") {
        presetIndex = 4;
    } else if (ctx->settings.encoderPreset == "balanced") {
        presetIndex = 2;
    } else if (ctx->settings.encoderPreset == "speed") {
        presetIndex = 3;
    } else if (ctx->settings.encoderPreset == "quality") {
        presetIndex = 1;
    }
    SendMessageW(ctx->presetCombo, CB_SETCURSEL, static_cast<WPARAM>(presetIndex), 0);

    int containerIndex = 0;
    if (ctx->settings.videoContainer == "mp4") {
        containerIndex = 1;
    }
    SendMessageW(ctx->containerCombo, CB_SETCURSEL, static_cast<WPARAM>(containerIndex), 0);
    SendMessageW(
        ctx->audioScopeCheck,
        BM_SETCHECK,
        ctx->settings.audioCaptureScope == bean::core::AppSettings::AudioCaptureScope::WowOnly ? BST_CHECKED : BST_UNCHECKED,
        0);
    if (ctx->audioScopeWowDiscordRadio) {
        SendMessageW(
            ctx->audioScopeWowDiscordRadio,
            BM_SETCHECK,
            ctx->settings.audioCaptureScope == bean::core::AppSettings::AudioCaptureScope::WowAndDiscord ? BST_CHECKED : BST_UNCHECKED,
            0);
    }
    SendMessageW(
        ctx->audioScopeAllRadio,
        BM_SETCHECK,
        ctx->settings.audioCaptureScope == bean::core::AppSettings::AudioCaptureScope::AllDesktop ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(ctx->microphoneCheck, BM_SETCHECK, ctx->settings.captureMicrophone ? BST_CHECKED : BST_UNCHECKED, 0);
    if (ctx->microphoneNoiseSuppressionCheck) {
        SendMessageW(
            ctx->microphoneNoiseSuppressionCheck,
            BM_SETCHECK,
            ctx->settings.microphoneNoiseSuppression ? BST_CHECKED : BST_UNCHECKED,
            0);
    }
    RefreshMicrophoneDeviceOptionsUi(ctx);
    if (ctx->youtubePrivacyCombo) {
        SendMessageW(ctx->youtubePrivacyCombo, CB_SETCURSEL, 0, 0);
    }
    RequestFolderAvailabilityRefresh(ctx);
    RefreshYouTubeUiState(ctx);
}

void HandleCommand(HWND hwnd, AppContext* ctx, int controlId)
{
    if (!ctx) {
        return;
    }

    switch (controlId) {
    case IDC_TAB_STATUS:
        SetActiveTab(ctx, AppContext::MainTab::Status);
        break;
    case IDC_TAB_CONFIGURATION:
        SetActiveTab(ctx, AppContext::MainTab::Configuration);
        break;
    case IDC_TAB_CHAT_PRIVACY:
        SetActiveTab(ctx, AppContext::MainTab::ChatPrivacy);
        break;
    case IDC_TAB_RECORDINGS:
        SetActiveTab(ctx, AppContext::MainTab::Recordings);
        break;
    case IDC_TAB_YOUTUBE:
        SetActiveTab(ctx, AppContext::MainTab::YouTube);
        break;
    case IDC_TAB_CLIPS:
        SetActiveTab(ctx, AppContext::MainTab::Clips);
        break;
    case IDC_TAB_KEYBINDS:
        SetActiveTab(ctx, AppContext::MainTab::Keybinds);
        break;
    case IDC_TAB_ABOUT:
        SetActiveTab(ctx, AppContext::MainTab::About);
        RefreshAboutUpdateButtonState(ctx);
        break;
    case IDC_OUTPUT_BROWSE: {
        const auto folder = PickFolder(hwnd);
        if (!folder.empty()) {
            SetWindowTextW(ctx->outputEdit, folder.c_str());
            RequestFolderAvailabilityRefresh(ctx);
            AutoSaveConfigurationSettings(ctx);
        }
        break;
    }
    case IDC_LOG_BROWSE: {
        const auto folder = PickFolder(hwnd);
        if (!folder.empty()) {
            SetWindowTextW(ctx->wowLogEdit, folder.c_str());
            RequestFolderAvailabilityRefresh(ctx);
            AutoSaveConfigurationSettings(ctx);
        }
        break;
    }
    case IDC_MICROPHONE_CHECK: {
        RefreshMicrophoneOptionsUi(ctx);
        break;
    }
    case IDC_CHAT_BLOCKER_ANCHOR_COMBO:
        AutoSaveChatBlockerSettings(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    case IDC_CHAT_BLOCKER_IMAGE_BLANK_RADIO:
        RefreshChatBlockerImageControls(ctx);
        AutoSaveChatBlockerSettings(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    case IDC_CHAT_BLOCKER_IMAGE_CUSTOM_RADIO:
        SyncChatBlockerSelectionToImageMetadata(ctx, true);
        RefreshChatBlockerImageControls(ctx);
        AutoSaveChatBlockerSettings(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    case IDC_CHAT_BLOCKER_IMAGE_IMPORT_BUTTON: {
        const auto selectedFile = PickImageFile(hwnd);
        if (selectedFile.empty()) {
            break;
        }
        std::wstring saveError;
        if (!SaveCustomChatBlockerImage(ctx, selectedFile, saveError)) {
            SetStatus(ctx, std::wstring(L"Custom chat blocker image error: ") + saveError);
            break;
        }
        SetStatus(ctx, L"Custom chat blocker image imported successfully.");
        SendMessageW(ctx->chatBlockerImageCustomRadio, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(ctx->chatBlockerImageBlankRadio, BM_SETCHECK, BST_UNCHECKED, 0);
        SyncChatBlockerSelectionToImageMetadata(ctx, true);
        RefreshChatBlockerImageControls(ctx);
        AutoSaveChatBlockerSettings(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    }
    case IDC_CHAT_BLOCKER_IMAGE_OPEN_FOLDER_BUTTON: {
        const auto imagesDirectory = ResolveChatBlockerImagesDirectory(ctx);
        if (imagesDirectory.empty()) {
            SetStatus(ctx, L"Custom image folder is unavailable.");
            break;
        }
        std::error_code ec;
        std::filesystem::create_directories(imagesDirectory, ec);
        if (ec) {
            SetStatus(ctx, std::wstring(L"Failed to open image folder: ") + ToWide(ec.message()));
            break;
        }
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", imagesDirectory.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open custom image folder.");
        }
        RefreshChatBlockerImageCombo(ctx, {});
        RefreshChatBlockerImageControls(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    }
    case IDC_CHAT_BLOCKER_ENABLED_CHECK:
        AutoSaveChatBlockerSettings(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    case IDC_RECORDINGS_REFRESH:
        RefreshRecordingsList(ctx);
        break;
    case IDC_YOUTUBE_REFRESH:
        RefreshYouTubeMediaList(ctx);
        SetStatus(ctx, L"YouTube media list refreshed.");
        break;
    case IDC_YOUTUBE_MEDIA_LIST:
        UpdateYouTubeMediaSelection(ctx);
        break;
    case IDC_CLIPS_REFRESH:
        RefreshClipsSourceList(ctx);
        SetStatus(ctx, L"Clip source list refreshed.");
        break;
    case IDC_CLIPS_SOURCE_COMBO:
        LoadClipFromSelection(ctx, true);
        break;
    case IDC_CLIPS_PLAY_PAUSE: {
        if (!ctx->clipsLoaded || !ctx->clipsPreviewEngine) {
            break;
        }
        if (ctx->clipsIsPlaying) {
            const HRESULT pauseHr = ctx->clipsPreviewEngine->Pause();
            if (SUCCEEDED(pauseHr)) {
                ctx->clipsIsPlaying = false;
                SetStatus(
                    ctx,
                    L"Clip pause requested (position-ms="
                        + std::to_wstring(ctx->clipsPreviewEngine->PositionMilliseconds())
                        + L").");
            } else {
                SetStatus(ctx, L"Clip pause failed (" + FormatHresultHex(pauseHr) + L").");
            }
        } else {
            const HRESULT playHr = ctx->clipsPreviewEngine->Play();
            if (SUCCEEDED(playHr)) {
                ctx->clipsIsPlaying = true;
                SetStatus(
                    ctx,
                    std::wstring(L"Clip play requested (playing=")
                        + (ctx->clipsPreviewEngine->IsPlaying() ? L"yes" : L"no")
                        + L", position-ms="
                        + std::to_wstring(ctx->clipsPreviewEngine->PositionMilliseconds())
                        + L").");
            } else {
                SetStatus(ctx, L"Clip play failed (" + FormatHresultHex(playHr) + L").");
            }
        }
        RefreshClipsPlaybackControls(ctx);
        break;
    }
    case IDC_CLIPS_SET_START: {
        if (!ctx->clipsLoaded) {
            break;
        }
        const int currentSeconds = QueryClipPositionMs(ctx, 0) / 1000;
        if (ctx->clipsStartEdit) {
            SetWindowTextW(
                ctx->clipsStartEdit,
                FormatElapsed(std::chrono::seconds(currentSeconds)).c_str());
        }
        if (ctx->clipsTimeline) {
            InvalidateControlAndParentRegion(ctx->clipsTimeline);
        }
        break;
    }
    case IDC_CLIPS_SET_END: {
        if (!ctx->clipsLoaded) {
            break;
        }
        const int currentSeconds = QueryClipPositionMs(ctx, 0) / 1000;
        if (ctx->clipsEndEdit) {
            SetWindowTextW(
                ctx->clipsEndEdit,
                FormatElapsed(std::chrono::seconds(currentSeconds)).c_str());
        }
        if (ctx->clipsTimeline) {
            InvalidateControlAndParentRegion(ctx->clipsTimeline);
        }
        break;
    }
    case IDC_CLIPS_EXPORT: {
        BeginClipExport(ctx, false);
        break;
    }
    case IDC_CLIPS_EXPORT_PRECISE: {
        BeginClipExport(ctx, true);
        break;
    }
    case IDC_CLIPS_OPEN_FOLDER: {
        const auto clipsFolder = ResolveClipsOutputFolderPath(ctx);
        if (clipsFolder.empty()) {
            SetStatus(ctx, L"Clips folder is unavailable.");
            break;
        }
        std::error_code ec;
        std::filesystem::create_directories(clipsFolder, ec);
        if (ec) {
            SetStatus(ctx, std::wstring(L"Could not open Clips folder: ") + ToWide(ec.message()));
            break;
        }
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", clipsFolder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open Clips folder.");
        }
        break;
    }
    case IDC_RECORDINGS_OPEN_FOLDER: {
        std::wstring folder = GetWindowTextString(ctx->outputEdit);
        if (folder.empty()) {
            folder = ToWide(ctx->settings.outputDirectory.string());
        }
        if (!DirectoryExists(folder)) {
            SetStatus(ctx, L"Cannot open recordings folder: path is unavailable.");
            break;
        }
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open recordings folder.");
        }
        break;
    }
    case IDC_RECORDINGS_OPEN_DB_FOLDER: {
        if (!ctx->runRepository) {
            SetStatus(ctx, L"Run metadata repository is unavailable.");
            break;
        }
        const auto dbFolder = ctx->runRepository->GetDatabasePath().parent_path();
        if (dbFolder.empty() || !std::filesystem::exists(dbFolder)) {
            SetStatus(ctx, L"Run metadata database folder is unavailable.");
            break;
        }
        const auto folderWide = dbFolder.wstring();
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", folderWide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open run metadata database folder.");
        }
        break;
    }
    case IDC_STATUS_OPEN_LOG_FOLDER: {
        const auto folder = ResolveStatusLogDirectory(ctx);
        if (folder.empty()) {
            SetStatus(ctx, L"Status log folder is unavailable.");
            break;
        }
        std::error_code ec;
        if (!std::filesystem::exists(folder, ec)) {
            std::filesystem::create_directories(folder, ec);
            if (ec) {
                SetStatus(ctx, std::wstring(L"Failed to prepare status log folder: ") + ToWide(ec.message()));
                break;
            }
        }
        const std::wstring folderWide = folder.wstring();
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", folderWide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open status log folder.");
        }
        break;
    }
    case IDC_STATUS_COPY_LOG_TEXT:
        if (!CopyBeanTextBoxText(ctx->statusText)) {
            SetStatus(ctx, L"Failed to copy status log text.");
        }
        break;
    case IDC_YOUTUBE_LINK_BUTTON: {
        PullSettingsFromUi(ctx);
        const std::string authServerUrl = GetYouTubeAuthServerUrl();
        if (authServerUrl.empty()) {
            SetStatus(ctx, kYouTubeOAuthCredentialsMissingMessage);
            break;
        }
        if (ctx->youtubeBusy.load()) {
            SetStatus(ctx, L"YouTube action already in progress.");
            break;
        }
        ctx->youtubeBusy.store(true);
        RefreshYouTubeUiState(ctx);
        SetStatus(ctx, L"Opening browser for YouTube authorization...");
        if (!LaunchAppWorker(ctx, [ctx, hwnd, authServerUrl]() {
            const auto auth = bean::integrations::YouTubeUploader::AuthorizeDesktop(hwnd, authServerUrl);
            auto* payload = new YouTubeAuthCompletionPayload();
            payload->success = auth.success;
            payload->clientId = auth.clientId;
            payload->refreshToken = auth.refreshToken;
            payload->channelId = auth.channelId;
            payload->channelTitle = auth.channelTitle;
            payload->error = auth.error;
            PostOwnedAppMessage(ctx, WM_BEAN_YOUTUBE_AUTH_COMPLETE, payload);
        })) {
            ctx->youtubeBusy.store(false);
            RefreshYouTubeUiState(ctx);
        }
        break;
    }
    case IDC_YOUTUBE_UNLINK_BUTTON: {
        if (ctx->youtubeBusy.load()) {
            SetStatus(ctx, L"YouTube action already in progress.");
            break;
        }
        ctx->youtubeUnlinkConfirmPending = true;
        RefreshYouTubeUiState(ctx);
        break;
    }
    case IDC_YOUTUBE_UNLINK_YES_BUTTON: {
        if (ctx->youtubeBusy.load()) {
            SetStatus(ctx, L"YouTube action already in progress.");
            break;
        }
        ctx->youtubeUnlinkConfirmPending = false;
        UnlinkYouTubeAccount(ctx);
        RefreshYouTubeUiState(ctx);
        break;
    }
    case IDC_YOUTUBE_UNLINK_NO_BUTTON:
        ctx->youtubeUnlinkConfirmPending = false;
        RefreshYouTubeUiState(ctx);
        break;
    case IDC_YOUTUBE_ACCOUNT_LINK: {
        if (ctx->settings.youtubeChannelId.empty()) {
            SetStatus(ctx, L"No linked YouTube channel URL is available.");
            break;
        }
        std::wstring url = L"https://www.youtube.com/channel/";
        url += ToWide(ctx->settings.youtubeChannelId);
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open linked YouTube channel.");
        }
        break;
    }
    case IDC_YOUTUBE_UPLOAD_BUTTON: {
        if (ctx->youtubeBusy.load()) {
            SetStatus(ctx, L"YouTube action already in progress.");
            break;
        }
        PullSettingsFromUi(ctx);
        const int selected = GetSelectedYouTubeMediaIndex(ctx);
        if (selected < 0 || static_cast<size_t>(selected) >= ctx->youtubeMediaItems.size()) {
            SetStatus(ctx, L"Select a recording or clip before uploading.");
            break;
        }
        if (ctx->settings.youtubeClientId.empty() || ctx->settings.youtubeRefreshToken.empty()) {
            SetStatus(ctx, L"Link your YouTube account first.");
            break;
        }
        const std::wstring titleWide = GetWindowTextString(ctx->youtubeTitleEdit);
        if (titleWide.empty()) {
            SetStatus(ctx, L"Enter a title for the upload.");
            break;
        }
        bean::integrations::YouTubePrivacy privacy = bean::integrations::YouTubePrivacy::Private;
        const int privacyIndex = static_cast<int>(SendMessageW(ctx->youtubePrivacyCombo, CB_GETCURSEL, 0, 0));
        if (privacyIndex == 1) {
            privacy = bean::integrations::YouTubePrivacy::Unlisted;
        } else if (privacyIndex == 2) {
            privacy = bean::integrations::YouTubePrivacy::Public;
        }

        const auto path = ctx->youtubeMediaItems[static_cast<size_t>(selected)].path;
        const auto title = ToUtf8(titleWide);
        bean::integrations::YouTubeCredentials creds;
        creds.clientId = ctx->settings.youtubeClientId;
        creds.refreshToken = ctx->settings.youtubeRefreshToken;
        creds.authServerUrl = GetYouTubeAuthServerUrl();
        ctx->youtubeLastVideoUrl.clear();
        ctx->youtubeBusy.store(true);
        RefreshYouTubeUiState(ctx);
        SetYouTubeUploadUi(ctx, 0, std::wstring(L"Uploading: ") + path.filename().wstring());
        SetStatus(ctx, std::wstring(L"Uploading to YouTube: ") + path.filename().wstring());
        if (!LaunchAppWorker(ctx, [ctx, path, title, privacy, creds]() {
            bean::integrations::YouTubeUploadRequest req;
            req.videoPath = path;
            req.title = title;
            req.privacy = privacy;
            int lastPercent = -1;
            const auto upload = bean::integrations::YouTubeUploader::UploadVideo(
                creds,
                req,
                [&lastPercent, ctx](uint64_t bytesSent, uint64_t totalBytes, const std::string& phase) {
                    if (phase == "auth") {
                        PostYouTubeUploadProgress(ctx, 0, L"Preparing YouTube authorization...");
                        return;
                    }
                    if (phase == "session") {
                        PostYouTubeUploadProgress(ctx, 0, L"Starting YouTube upload session...");
                        return;
                    }
                    if (phase == "complete") {
                        PostYouTubeUploadProgress(ctx, 100, L"Upload complete.");
                        return;
                    }
                    if (phase == "uploading") {
                        int percent = 0;
                        if (totalBytes > 0) {
                            percent = static_cast<int>((bytesSent * 100ULL) / totalBytes);
                        }
                        percent = std::clamp(percent, 0, 100);
                        if (percent == lastPercent && percent != 100) {
                            return;
                        }
                        lastPercent = percent;
                        std::wostringstream text;
                        text << L"Uploading to YouTube... " << percent << L"%";
                        PostYouTubeUploadProgress(ctx, percent, text.str());
                    }
                });
            if (!upload.success) {
                PostStatus(ctx, std::wstring(L"YouTube upload failed: ") + ToWide(upload.error));
                PostYouTubeUploadProgress(ctx, 0, std::wstring(L"Upload failed: ") + ToWide(upload.error));
                ctx->youtubeBusy.store(false);
                RequestYouTubeUiRefresh(ctx);
                return;
            }

            std::wstring message = L"YouTube upload complete.";
            std::wstring videoUrl;
            if (!upload.videoUrl.empty()) {
                videoUrl = ToWide(upload.videoUrl);
            }
            PostStatus(ctx, message);
            PostYouTubeUploadProgress(ctx, 100, message, videoUrl);
            ctx->youtubeBusy.store(false);
            RequestYouTubeUiRefresh(ctx);
        })) {
            ctx->youtubeBusy.store(false);
            RefreshYouTubeUiState(ctx);
        }
        break;
    }
    case IDC_ABOUT_WEBSITE_BUTTON: {
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", L"https://andrew.gg/bean", nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open website.");
        }
        break;
    }
    case IDC_ABOUT_EMAIL_BUTTON: {
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", L"mailto:goatrope@gmail.com?subject=BEAN%20Inquiry", nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open default email app.");
        }
        break;
    }
    case IDC_ABOUT_DISCORD_BUTTON: {
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", L"https://discord.gg/57JGRw6x3D", nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open Discord support invite.");
        }
        break;
    }
    case IDC_ABOUT_CHECK_UPDATES_BUTTON: {
        const HWND updateButton = GetDlgItem(ctx->aboutPanel, IDC_ABOUT_CHECK_UPDATES_BUTTON);
        const std::wstring buttonText = updateButton ? GetWindowTextString(updateButton) : std::wstring();
        if (buttonText != L"Update now") {
            RefreshAboutUpdateButtonState(ctx);
            break;
        }

        EnableWindow(updateButton, FALSE);
        SetWindowTextW(updateButton, L"Updating...");
        UpdateWindow(updateButton);
        std::wstring updateStatus;
        const auto result = bean::app::ApplyUpdate(updateStatus);
        SetStatus(ctx, updateStatus);
        if (result == bean::app::UpdateApplyResult::UpdateReadyAndExitRequested) {
            ctx->closeConfirmationBypassRequested = true;
            if (!PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
                ctx->closeConfirmationBypassRequested = false;
            }
        } else {
            RefreshAboutUpdateButtonState(ctx);
        }
        break;
    }
    case IDC_RECORD_START: {
        PullSettingsFromUi(ctx);
        std::string prepareError;
        if (!EnsureOutputDirectoryReady(ctx->settings.outputDirectory, prepareError)) {
            SetStatus(ctx, std::wstring(L"Recording start failed: ") + ToWide(prepareError));
            break;
        }
        RequestFolderAvailabilityRefresh(ctx);
        ctx->orchestrator->ApplySettings(ctx->settings);
        std::string error;
        ctx->orchestrator->StartManualRecording(error);
        break;
    }
    case IDC_RECORD_STOP: {
        std::string error;
        ctx->orchestrator->StopManualRecording(error);
        break;
    }
    case IDC_KEYBINDS_CREATE_CLIP_REBIND:
        BeginKeybindCapture(ctx, 0);
        break;
    case IDC_KEYBINDS_MANUAL_START_REBIND:
        BeginKeybindCapture(ctx, 1);
        break;
    case IDC_KEYBINDS_MANUAL_STOP_REBIND:
        BeginKeybindCapture(ctx, 2);
        break;
    case IDC_KEYBINDS_CREATE_CLIP_UNBIND:
        UnbindKeybind(ctx, 0);
        break;
    case IDC_KEYBINDS_MANUAL_START_UNBIND:
        UnbindKeybind(ctx, 1);
        break;
    case IDC_KEYBINDS_MANUAL_STOP_UNBIND:
        UnbindKeybind(ctx, 2);
        break;
    case IDC_KEYBINDS_CREATE_CLIP_RESET:
        ResetKeybind(ctx, 0);
        break;
    case IDC_KEYBINDS_MANUAL_START_RESET:
        ResetKeybind(ctx, 1);
        break;
    case IDC_KEYBINDS_MANUAL_STOP_RESET:
        ResetKeybind(ctx, 2);
        break;
    default:
        break;
    }

    RefreshLiveStatus(ctx);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    AppContext* ctx = reinterpret_cast<AppContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* appCtx = reinterpret_cast<AppContext*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(appCtx));
        ctx = appCtx;
        // Main-window controls are configured during WM_CREATE, so store it immediately.
        ctx->mainWindow = hwnd;
        ApplySelectedTheme(ctx);

        CreateAppPanels(ctx, hwnd);
        SetWindowSubclass(ctx->statusPanel, PanelMessageForwarder, 1, 0);
        SetWindowSubclass(ctx->recorderPanel, PanelMessageForwarder, 2, 0);
        SetWindowSubclass(ctx->chatPrivacyPanel, PanelMessageForwarder, 3, 0);
        SetWindowSubclass(ctx->recordingsPanel, PanelMessageForwarder, 4, 0);
        SetWindowSubclass(ctx->youtubePanel, PanelMessageForwarder, 5, 0);
        SetWindowSubclass(ctx->clipsPanel, PanelMessageForwarder, 6, 0);
        SetWindowSubclass(ctx->keybindsPanel, PanelMessageForwarder, 8, 0);
        SetWindowSubclass(ctx->aboutPanel, PanelMessageForwarder, 7, 0);
        SetWindowSubclass(ctx->presetHelpIcon, HoverTooltipSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        SetWindowSubclass(ctx->postRunDelayHelpIcon, HoverTooltipSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        SetWindowSubclass(ctx->outputStatus, HoverTooltipSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        SetWindowSubclass(ctx->advancedLoggingHelpIcon, HoverTooltipSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        SetWindowSubclass(ctx->diskSpaceHelpIcon, HoverTooltipSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        SetWindowSubclass(ctx->youtubeUploadStatus, YouTubeUploadStatusSubclassProc, 4, reinterpret_cast<DWORD_PTR>(ctx));
        if (ctx->clipsTimeline) {
            SetWindowSubclass(ctx->clipsTimeline, ClipsSliderSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        }
        if (ctx->clipsVolumeSlider) {
            SetWindowSubclass(ctx->clipsVolumeSlider, ClipsSliderSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        }
        RefreshChatBlockerImageCombo(ctx, {});
        RefreshChatBlockerImageControls(ctx);
        EnsureParticipantSpecIconList(ctx);
        RefreshClipsPlaybackControls(ctx);
        ConfigureStyledButtons(ctx);
        ConfigureModernControls(ctx);
        ApplyUiFonts(hwnd);
        ApplyRecordingsFonts(ctx);
        if (gTheme.mutedHintFont) {
            const std::array<int, 4> filterHintLabels = {
                IDC_RECORDINGS_FILTER_TYPE_LABEL,
                IDC_RECORDINGS_FILTER_TIMED_LABEL,
                IDC_RECORDINGS_FILTER_KEY_LABEL,
                IDC_RECORDINGS_FILTER_CHARS_LABEL};
            for (const int id : filterHintLabels) {
                HWND label = GetDlgItem(ctx->recordingsPanel, id);
                if (label) {
                    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.mutedHintFont), TRUE);
                }
            }
        }
        HWND autoSaveHint = GetDlgItem(ctx->recorderPanel, IDC_CONFIGURATION_AUTOSAVE_HINT);
        if (autoSaveHint && gTheme.mutedHintFont) {
            SendMessageW(autoSaveHint, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.mutedHintFont), TRUE);
        }
        HWND keybindAutoSaveHint = GetDlgItem(ctx->keybindsPanel, IDC_KEYBINDS_AUTOSAVE_HINT);
        if (keybindAutoSaveHint && gTheme.mutedHintFont) {
            SendMessageW(keybindAutoSaveHint, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.mutedHintFont), TRUE);
        }
        if (ctx->outputStatus && gTheme.statusIndicatorFont) {
            SendMessageW(ctx->outputStatus, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.statusIndicatorFont), TRUE);
        }
        ConfigureConfigurationTooltips(ctx);
        if (gTheme.headingFont) {
            HWND titleLabel = GetDlgItem(ctx->aboutPanel, IDC_ABOUT_TITLE_LABEL);
            if (titleLabel) {
                SendMessageW(titleLabel, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.headingFont), TRUE);
            }
        }
        HWND aboutFlavorText = GetDlgItem(ctx->aboutPanel, IDC_ABOUT_FLAVOR_TEXT);
        if (aboutFlavorText && gTheme.mutedItalicHintFont) {
            SendMessageW(aboutFlavorText, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.mutedItalicHintFont), TRUE);
        }
        SendMessageW(ctx->youtubeUploadProgress, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(kColorListSelection));
        SendMessageW(ctx->youtubeUploadProgress, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(kColorInputBg));

        const std::wstring statusLogInitError = InitializeStatusLogFile(ctx);
        if (!statusLogInitError.empty()) {
            SetStatus(ctx, std::wstring(L"Status log unavailable: ") + statusLogInitError);
        }
        PushSettingsToUi(ctx);
        RefreshLiveStatus(ctx);
        SetActiveTab(ctx, AppContext::MainTab::Status);
        if (!ctx->settings.youtubeRefreshToken.empty() && ctx->settings.youtubeChannelId.empty() && ctx->settings.youtubeChannelTitle.empty()) {
            ResolveLinkedYouTubeIdentityAsync(ctx, false);
        }
        LogSessionDiagnostics(ctx);
        SetStatus(ctx, L"Ready.");
        PullSettingsFromUi(ctx);
        ctx->orchestrator->ApplySettings(ctx->settings);
        ctx->alwaysOnMonitoring = true;
        if (!ctx->warcraftRecorderDetected) {
            std::string autoStartError;
            ctx->orchestrator->StartMonitoring(autoStartError);
        } else {
            SetStatus(
                ctx,
                L"Monitoring not started because Warcraft Recorder is running. "
                L"Close it and Bean will start monitoring automatically.");
        }
        RefreshLiveStatus(ctx);
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        LayoutMainUi(ctx, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
        BeginDiskSpaceProbe(ctx);
        ctx->chatBlockerAutoSaveArmed = true;
        ctx->configurationAutoSaveArmed = true;
        SetTimer(hwnd, kLiveStatusTimerId, kLiveStatusIntervalMs, nullptr);
        RegisterConfiguredHotkeys(ctx);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            DismissCustomComboPopup();
        }
        if (ctx) {
            if (ctx->activeTab == AppContext::MainTab::Recordings) {
                if (ctx->recordingsList) {
                    InvalidateRect(ctx->recordingsList, nullptr, FALSE);
                }
                if (ctx->recordingsInfoText) {
                    InvalidateRect(ctx->recordingsInfoText, nullptr, FALSE);
                }
            }
            if (ctx->activeTab == AppContext::MainTab::YouTube && ctx->youtubeMediaList) {
                InvalidateRect(ctx->youtubeMediaList, nullptr, FALSE);
            }
        }
        break;
    case WM_BEAN_FILE_LIST_SELECTION:
        if (ctx && reinterpret_cast<HWND>(wParam) == ctx->recordingsList) {
            UpdateRecordingInfoPane(ctx, static_cast<int>(lParam));
        } else if (ctx && reinterpret_cast<HWND>(wParam) == ctx->youtubeMediaList) {
            UpdateYouTubeMediaSelection(ctx);
        }
        return 0;
    case WM_BEAN_FILE_LIST_COLUMN_CLICK:
        if (ctx && reinterpret_cast<HWND>(wParam) == ctx->recordingsList) {
            const int column = static_cast<int>(lParam);
            if (column >= 0 && column <= 3) {
                const auto clickedColumn = static_cast<AppContext::RecordingSortColumn>(column);
                if (ctx->recordingSortColumn == clickedColumn) {
                    ctx->recordingSortAscending = !ctx->recordingSortAscending;
                } else {
                    ctx->recordingSortColumn = clickedColumn;
                    ctx->recordingSortAscending = true;
                }
                SortRecordingItems(ctx);
                ApplyRecordingFilters(ctx);
            }
        } else if (ctx && reinterpret_cast<HWND>(wParam) == ctx->youtubeMediaList) {
            const int column = static_cast<int>(lParam);
            if (column >= 0 && column <= 2) {
                const auto clickedColumn = static_cast<AppContext::YouTubeSortColumn>(column);
                if (ctx->youtubeSortColumn == clickedColumn) {
                    ctx->youtubeSortAscending = !ctx->youtubeSortAscending;
                } else {
                    ctx->youtubeSortColumn = clickedColumn;
                    ctx->youtubeSortAscending = true;
                }
                SortYouTubeMediaItems(ctx);
                ctx->youtubeMediaSelectedIndex = -1;
                RefreshBeanFileList(ctx->youtubeMediaList);
                UpdateYouTubeMediaSelection(ctx);
            }
        }
        return 0;
    case WM_BEAN_FILE_LIST_ACTION:
        if (ctx && reinterpret_cast<HWND>(wParam) == ctx->recordingsList) {
            OpenRecordingInClipmaker(ctx, static_cast<int>(lParam));
        }
        return 0;
    case WM_BEAN_FILE_LIST_DOUBLE_CLICK:
        if (ctx && reinterpret_cast<HWND>(wParam) == ctx->recordingsList) {
            const int selected = static_cast<int>(lParam);
            if (selected >= 0 && static_cast<size_t>(selected) < ctx->recordingItems.size()) {
                const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(
                    hwnd,
                    L"open",
                    ctx->recordingItems[static_cast<size_t>(selected)].path.wstring().c_str(),
                    nullptr,
                    nullptr,
                    SW_SHOWNORMAL));
                if (result <= 32) {
                    SetStatus(ctx, L"Failed to open selected recording.");
                }
            }
        } else if (ctx && reinterpret_cast<HWND>(wParam) == ctx->youtubeMediaList) {
            const int selected = static_cast<int>(lParam);
            if (selected >= 0 && static_cast<size_t>(selected) < ctx->youtubeMediaItems.size()) {
                const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(
                    hwnd,
                    L"open",
                    ctx->youtubeMediaItems[static_cast<size_t>(selected)].path.wstring().c_str(),
                    nullptr,
                    nullptr,
                    SW_SHOWNORMAL));
                if (result <= 32) {
                    SetStatus(ctx, L"Failed to open selected YouTube media file.");
                }
            }
        }
        return 0;
    case WM_HOTKEY:
        if (ctx && ctx->orchestrator && wParam == kClipHotkeyId) {
            std::string clipError;
            if (!ctx->orchestrator->RequestClip(clipError)) {
                SetStatus(ctx, std::wstring(L"Clip request failed: ") + ToWide(clipError));
            }
            return 0;
        }
        if (ctx && ctx->orchestrator && wParam == kManualStartHotkeyId) {
            HandleCommand(hwnd, ctx, IDC_RECORD_START);
            return 0;
        }
        if (ctx && ctx->orchestrator && wParam == kManualStopHotkeyId) {
            HandleCommand(hwnd, ctx, IDC_RECORD_STOP);
            return 0;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (CaptureKeybindKey(ctx, wParam)) {
            return 0;
        }
        break;
    case WM_COMMAND:
        if (ctx
            && (HIWORD(wParam) == CBN_SELCHANGE || HIWORD(wParam) == CBN_CLOSEUP)
            && IsStyledComboId(static_cast<int>(LOWORD(wParam)))) {
            // Let the native combo finish its selection transaction first,
            // then repaint the custom closed-state item from the queue.
            ScheduleModernComboRedraw(reinterpret_cast<HWND>(lParam));
        }
        if (ctx
            && HIWORD(wParam) == CBN_SELCHANGE
            && LOWORD(wParam) == IDC_CUSTOMIZE_THEME_COMBO) {
            const LRESULT selectedIndex = SendMessageW(ctx->customizeThemeCombo, CB_GETCURSEL, 0, 0);
            if (selectedIndex >= 0 && static_cast<size_t>(selectedIndex) < kThemeDefinitions.size()) {
                ctx->settings.theme = kThemeDefinitions[static_cast<size_t>(selectedIndex)].id;
                ApplySelectedTheme(ctx);
                SaveThemeSettings(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == IDC_STATUS_TEXT) {
            // The read-only custom status log is updated programmatically.
            // Do not feed its EN_CHANGE notifications back into the live
            // status refresh loop.
            return 0;
        }
        if (HIWORD(wParam) == EN_CHANGE && (LOWORD(wParam) == IDC_OUTPUT_EDIT || LOWORD(wParam) == IDC_LOG_EDIT)) {
            RequestFolderAvailabilityRefresh(ctx);
            if (ctx) {
                AutoSaveConfigurationSettings(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == EN_CHANGE
            && (LOWORD(wParam) == IDC_FPS_EDIT
                || LOWORD(wParam) == IDC_POST_RUN_DELAY_EDIT
                || LOWORD(wParam) == IDC_CLIP_DURATION_EDIT)) {
            if (ctx) {
                AutoSaveConfigurationSettings(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == EN_CHANGE
            && (LOWORD(wParam) == IDC_CLIPS_START_EDIT || LOWORD(wParam) == IDC_CLIPS_END_EDIT)) {
            if (ctx && ctx->clipsTimeline) {
                InvalidateControlAndParentRegion(ctx->clipsTimeline);
            }
            return 0;
        }
        if (HIWORD(wParam) == EN_CHANGE
            && (LOWORD(wParam) == IDC_RECORDINGS_FILTER_KEY_EDIT
                || LOWORD(wParam) == IDC_RECORDINGS_FILTER_CHARS_EDIT)) {
            if (ctx) {
                ApplyRecordingFilters(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_RECORDINGS_FILTER_TIMED_COMBO) {
            if (ctx) {
                ApplyRecordingFilters(ctx);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDC_RECORDINGS_FILTER_TIMED_COMBO) {
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED
            && (LOWORD(wParam) == IDC_RECORDINGS_FILTER_TYPE_MANUAL
                || LOWORD(wParam) == IDC_RECORDINGS_FILTER_TYPE_MYTHIC
                || LOWORD(wParam) == IDC_RECORDINGS_FILTER_TYPE_RAID
                || LOWORD(wParam) == IDC_RECORDINGS_FILTER_TYPE_PVP)) {
            if (ctx) {
                ApplyRecordingFilters(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE
            && (LOWORD(wParam) == IDC_ENCODER_COMBO
                || LOWORD(wParam) == IDC_PRESET_COMBO
                || LOWORD(wParam) == IDC_CONTAINER_COMBO
                || LOWORD(wParam) == IDC_MICROPHONE_COMBO
                || LOWORD(wParam) == IDC_RECORDING_RESOLUTION_COMBO)) {
            if (ctx) {
                AutoSaveConfigurationSettings(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_CLIPS_SOURCE_COMBO) {
            HandleCommand(hwnd, ctx, LOWORD(wParam));
            return 0;
        }
        if (LOWORD(wParam) == IDC_CLIPS_SOURCE_COMBO) {
            // Ignore non-selection combo notifications (dropdown open/close/focus),
            // otherwise we can accidentally trigger clip load work while the user
            // is just interacting with the combo.
            return 0;
        }
        if (HIWORD(wParam) == CBN_DROPDOWN && LOWORD(wParam) == IDC_CHAT_BLOCKER_IMAGE_COMBO) {
            if (ctx) {
                RefreshChatBlockerImageCombo(ctx, {});
                RefreshChatBlockerImageControls(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_CHAT_BLOCKER_IMAGE_COMBO) {
            if (ctx) {
                SyncChatBlockerSelectionToImageMetadata(ctx, true);
                AutoSaveChatBlockerSettings(ctx);
            }
            if (ctx && ctx->chatPreview) {
                InvalidateRect(ctx->chatPreview, nullptr, FALSE);
            }
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED
            && (LOWORD(wParam) == IDC_MICROPHONE_CHECK
                || LOWORD(wParam) == IDC_MICROPHONE_NOISE_SUPPRESSION_CHECK
                || LOWORD(wParam) == IDC_AUDIO_SCOPE_CHECK
                || LOWORD(wParam) == IDC_AUDIO_SCOPE_WOW_DISCORD_RADIO
                || LOWORD(wParam) == IDC_AUDIO_SCOPE_ALL_RADIO)) {
            HandleCommand(hwnd, ctx, LOWORD(wParam));
            if (ctx) {
                AutoSaveConfigurationSettings(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == EN_CHANGE && (LOWORD(wParam) == IDC_CHAT_BLOCKER_WIDTH_EDIT || LOWORD(wParam) == IDC_CHAT_BLOCKER_HEIGHT_EDIT)) {
            if (ctx) {
                if (LOWORD(wParam) == IDC_CHAT_BLOCKER_WIDTH_EDIT && ctx->chatBlockerIgnoreNextWidthChange) {
                    ctx->chatBlockerIgnoreNextWidthChange = false;
                    return 0;
                }
                if (LOWORD(wParam) == IDC_CHAT_BLOCKER_HEIGHT_EDIT && ctx->chatBlockerIgnoreNextHeightChange) {
                    ctx->chatBlockerIgnoreNextHeightChange = false;
                    return 0;
                }
                ApplyChatBlockerAspectForEdit(ctx, LOWORD(wParam));
                AutoSaveChatBlockerSettings(ctx);
            }
            if (ctx && ctx->chatPreview) {
                InvalidateRect(ctx->chatPreview, nullptr, FALSE);
            }
            return 0;
        }
        HandleCommand(hwnd, ctx, LOWORD(wParam));
        return 0;
    case WM_HSCROLL: {
        if (!ctx) {
            break;
        }
        break;
    }
    case WM_DRAWITEM: {
        auto* drawInfo = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!drawInfo) {
            break;
        }
        if (drawInfo->CtlType == ODT_COMBOBOX && IsStyledComboId(static_cast<int>(drawInfo->CtlID))) {
            DrawStyledComboItem(drawInfo);
            return TRUE;
        }
        if (drawInfo->CtlType == ODT_BUTTON && IsStyledButtonId(static_cast<int>(drawInfo->CtlID))) {
            DrawStyledButton(drawInfo, ctx);
            return TRUE;
        }
        if (drawInfo->CtlType == ODT_STATIC
            && (IsOwnerDrawStaticId(static_cast<int>(drawInfo->CtlID))
                || (ctx && drawInfo->hwndItem == ctx->configurationTooltip))) {
            if (IsStatusLightId(static_cast<int>(drawInfo->CtlID))) {
                DrawStatusLight(drawInfo, ctx);
            } else if (drawInfo->CtlID == IDC_LENGTH_VALUE) {
                DrawLengthValue(drawInfo);
            } else if (drawInfo->CtlID == IDC_YOUTUBE_LINK_STATUS) {
                DrawYouTubeLinkStatus(drawInfo, ctx);
            } else if (drawInfo->CtlID == IDC_YOUTUBE_UPLOAD_STATUS) {
                DrawYouTubeUploadStatus(drawInfo, ctx);
            } else if (drawInfo->CtlID == IDC_CONFIGURATION_TOOLTIP || (ctx && drawInfo->hwndItem == ctx->configurationTooltip)) {
                DrawConfigurationTooltip(drawInfo);
            } else if (drawInfo->CtlID == IDC_PRESET_HELP || drawInfo->CtlID == IDC_POST_RUN_DELAY_HELP || drawInfo->CtlID == IDC_ADVANCED_LOGGING_HELP || drawInfo->CtlID == IDC_DISK_SPACE_HELP) {
                DrawHelpIcon(drawInfo);
            } else if (drawInfo->CtlID == IDC_CHAT_PREVIEW) {
                DrawChatPrivacyPreview(drawInfo, ctx);
            } else if (drawInfo->CtlID == IDC_CLIPS_TIMELINE) {
                DrawClipsSlider(drawInfo, ctx, true);
            } else if (drawInfo->CtlID == IDC_CLIPS_VOLUME_SLIDER) {
                DrawClipsSlider(drawInfo, ctx, false);
            }
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        if (!ctx) {
            break;
        }
        const auto control = reinterpret_cast<HWND>(lParam);
        HDC dc = reinterpret_cast<HDC>(wParam);
        if (control == ctx->clipsVideoSurface) {
            SetBkMode(dc, OPAQUE);
            SetTextColor(dc, kColorTextPrimary);
            SetBkColor(dc, kColorInputBg);
            return reinterpret_cast<LRESULT>(gTheme.inputBrush ? gTheme.inputBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        if (control == ctx->statusText) {
            SetBkMode(dc, OPAQUE);
            SetTextColor(dc, kColorTextPrimary);
            SetBkColor(dc, kColorInputBg);
            return reinterpret_cast<LRESULT>(gTheme.inputBrush ? gTheme.inputBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        if (control == ctx->outputStatus || control == ctx->wowLogStatus) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, kColorPanelBottom);
            if (control == ctx->outputStatus) {
                if (ctx->outputAvailable) {
                    SetTextColor(dc, kColorSuccess);
                } else if (ctx->outputFolderWillBeCreatedOnRecordStart) {
                    SetTextColor(dc, kColorWarning);
                } else {
                    SetTextColor(dc, kColorFailure);
                }
            } else {
                SetTextColor(dc, ctx->wowLogAvailable ? kColorSuccess : kColorFailure);
            }
            return reinterpret_cast<LRESULT>(gTheme.panelSolidBrush ? gTheme.panelSolidBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        if (control == ctx->clipsFfmpegWarning) {
            SetBkMode(dc, TRANSPARENT);
            if (ctx->clipsExportStatus == AppContext::ClipExportStatus::Success) {
                SetTextColor(dc, kColorSuccess);
            } else if (ctx->clipsExportStatus == AppContext::ClipExportStatus::Failure || !ctx->ffmpegDetected) {
                SetTextColor(dc, kColorFailure);
            } else {
                SetTextColor(dc, kColorTextMuted);
            }
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kColorTextPrimary);
        if (control != ctx->outputStatus && control != ctx->wowLogStatus) {
            const int id = GetDlgCtrlID(control);
            if (id == IDC_RECORDINGS_LABEL || id == IDC_YOUTUBE_LABEL || id == IDC_YOUTUBE_UPLOAD_STATUS || id == IDC_ABOUT_BUILD_TEXT || id == IDC_ABOUT_FLAVOR_TEXT || id == IDC_YOUTUBE_UNLINK_CONFIRM_LABEL || id == IDC_CONFIGURATION_AUTOSAVE_HINT || id == IDC_KEYBINDS_AUTOSAVE_HINT
                || id == IDC_RECORDINGS_FILTER_TYPE_LABEL
                || id == IDC_RECORDINGS_FILTER_TIMED_LABEL
                || id == IDC_RECORDINGS_FILTER_KEY_LABEL
                || id == IDC_RECORDINGS_FILTER_CHARS_LABEL) {
                SetTextColor(dc, kColorTextMuted);
            }
        }
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kColorTextPrimary);
        if (ctx && reinterpret_cast<HWND>(lParam) == ctx->youtubeTitleEdit) {
            SetBkColor(dc, kColorYouTubeInputBg);
            return reinterpret_cast<LRESULT>(gTheme.youtubeInputBrush ? gTheme.youtubeInputBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        SetBkColor(dc, kColorInputBg);
        return reinterpret_cast<LRESULT>(gTheme.inputBrush ? gTheme.inputBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    case WM_CTLCOLORBTN: {
        HWND control = reinterpret_cast<HWND>(lParam);
        HDC dc = reinterpret_cast<HDC>(wParam);
        const int id = control ? GetDlgCtrlID(control) : 0;
        const bool isCheckOrRadioControl =
            id == IDC_AUDIO_SCOPE_CHECK
            || id == IDC_AUDIO_SCOPE_WOW_DISCORD_RADIO
            || id == IDC_AUDIO_SCOPE_ALL_RADIO
            || id == IDC_MICROPHONE_CHECK
            || id == IDC_MICROPHONE_NOISE_SUPPRESSION_CHECK
            || id == IDC_CHAT_BLOCKER_ENABLED_CHECK
            || id == IDC_CHAT_BLOCKER_IMAGE_BLANK_RADIO
            || id == IDC_CHAT_BLOCKER_IMAGE_CUSTOM_RADIO
            || id == IDC_RECORDINGS_FILTER_TYPE_MANUAL
            || id == IDC_RECORDINGS_FILTER_TYPE_MYTHIC
            || id == IDC_RECORDINGS_FILTER_TYPE_RAID
            || id == IDC_RECORDINGS_FILTER_TYPE_PVP;
        if (isCheckOrRadioControl) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, kColorPanelBottom);
            SetTextColor(dc, (control && !IsWindowEnabled(control)) ? kColorTextMuted : kColorTextPrimary);
            return reinterpret_cast<LRESULT>(gTheme.panelSolidBrush ? gTheme.panelSolidBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        // Owner-draw styled buttons paint their own background from the parent
        // gradient; avoid a solid buttonBrush flash behind the rectangular button.
        if (IsStyledButtonId(id)) {
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }

        SetTextColor(dc, kColorButtonText);
        SetBkColor(dc, kColorButtonBg);
        return reinterpret_cast<LRESULT>(gTheme.buttonBrush ? gTheme.buttonBrush : reinterpret_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
    }
    case WM_GETMINMAXINFO: {
        auto* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
        minMax->ptMinTrackSize.x = kMinClientWidth;
        minMax->ptMinTrackSize.y = kMinClientHeight;
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        if (ctx) {
            ctx->clipsResizeInProgress = true;
        }
        return 0;
    case WM_EXITSIZEMOVE:
        if (ctx) {
            ctx->clipsResizeInProgress = false;
            ApplyClipVideoWindowBounds(ctx);
            if (!IsZoomed(hwnd) && !IsIconic(hwnd)) {
                RECT windowRect{};
                if (GetWindowRect(hwnd, &windowRect)) {
                    ctx->settings.windowWidth = windowRect.right - windowRect.left;
                    ctx->settings.windowHeight = windowRect.bottom - windowRect.top;
                    std::string saveError;
                    ctx->settingsStore.Save(ctx->settings, saveError);
                }
            }
        }
        return 0;
    case WM_SIZE:
        if (ctx && wParam != SIZE_MINIMIZED) {
            LayoutMainUi(ctx, LOWORD(lParam), HIWORD(lParam));
            ApplyClipVideoWindowBounds(ctx);
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        HDC dc = reinterpret_cast<HDC>(wParam);
        TRIVERTEX vertices[2] = {
            {rect.left, rect.top, static_cast<COLOR16>(GetRValue(kColorWindowTop) << 8), static_cast<COLOR16>(GetGValue(kColorWindowTop) << 8), static_cast<COLOR16>(GetBValue(kColorWindowTop) << 8), 0xFF00},
            {rect.right, rect.bottom, static_cast<COLOR16>(GetRValue(kColorWindowBottom) << 8), static_cast<COLOR16>(GetGValue(kColorWindowBottom) << 8), static_cast<COLOR16>(GetBValue(kColorWindowBottom) << 8), 0xFF00},
        };
        GRADIENT_RECT gradientRect{0, 1};
        if (!GradientFill(dc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V)) {
            FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        TRIVERTEX vertices[2] = {
            {rect.left, rect.top, static_cast<COLOR16>(GetRValue(kColorWindowTop) << 8), static_cast<COLOR16>(GetGValue(kColorWindowTop) << 8), static_cast<COLOR16>(GetBValue(kColorWindowTop) << 8), 0xFF00},
            {rect.right, rect.bottom, static_cast<COLOR16>(GetRValue(kColorWindowBottom) << 8), static_cast<COLOR16>(GetGValue(kColorWindowBottom) << 8), static_cast<COLOR16>(GetBValue(kColorWindowBottom) << 8), 0xFF00},
        };
        GRADIENT_RECT gradientRect{0, 1};
        if (!GradientFill(dc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V)) {
            FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kClipsExportStatusTimerId) {
            if (ctx) {
                ClearClipsExportStatus(ctx);
            }
            return 0;
        }
        if (wParam == kFolderAvailabilityTimerId) {
            KillTimer(hwnd, kFolderAvailabilityTimerId);
            BeginFolderAvailabilityProbe(ctx);
            return 0;
        }
        if (wParam == kConfigurationAutoSaveTimerId) {
            KillTimer(hwnd, kConfigurationAutoSaveTimerId);
            CommitConfigurationSettings(ctx);
            return 0;
        }
        if (wParam == kChatBlockerAutoSaveTimerId) {
            KillTimer(hwnd, kChatBlockerAutoSaveTimerId);
            CommitChatBlockerSettings(ctx);
            return 0;
        }
        if (wParam == kLiveStatusTimerId) {
            if (ctx && ctx->orchestrator) {
                ctx->orchestrator->Tick();
            }
            RefreshLiveStatus(ctx);
            if (ctx
                && ctx->clipsLoaded
                && ctx->activeTab == AppContext::MainTab::Clips) {
                if (ctx->clipsPreviewEngine) {
                    const bool isRunning = ctx->clipsPreviewEngine->IsPlaying();
                    if (ctx->clipsIsPlaying != isRunning) {
                        ctx->clipsIsPlaying = isRunning;
                        RefreshClipsPlaybackControls(ctx);
                    }
                }
                SyncClipTimelineFromPlayback(ctx);
            }
            return 0;
        }
        break;
    case WM_BEAN_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lParam);
        if (ctx && text) {
            if (text->find(L"AUTO-RECORD FAILED") != std::wstring::npos) {
                ctx->autoRecordFailed = true;
            } else if (text->find(L"Recording started") != std::wstring::npos) {
                ctx->autoRecordFailed = false;
            }
            SetStatus(ctx, *text);
            ApplyTaskbarOverlayState(ctx);
        }
        delete text;
        return 0;
    }
    case WM_BEAN_YOUTUBE_UI_REFRESH:
        if (ctx) {
            RefreshYouTubeUiState(ctx);
            if (ctx->activeTab == AppContext::MainTab::YouTube) {
                RefreshYouTubeMediaList(ctx);
            }
        }
        return 0;
    case WM_BEAN_CLIPS_UI_REFRESH:
        if (ctx) {
            RefreshClipsPlaybackControls(ctx);
            RefreshClipsSourceList(ctx);
        }
        return 0;
    case WM_BEAN_CLIPS_MEDIA_EVENT:
        if (ctx && ctx->clipsPreviewEngine) {
            const DWORD eventCode = static_cast<DWORD>(wParam);
            if (eventCode == MF_MEDIA_ENGINE_EVENT_CANPLAY
                || eventCode == MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH) {
                LoadClipFromSelection(ctx, true);
            } else if (eventCode == MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE && ctx->clipsLoaded) {
                ctx->clipsDurationMs = ctx->clipsPreviewEngine->DurationMilliseconds();
                RefreshClipsPlaybackControls(ctx);
            } else if (eventCode == MF_MEDIA_ENGINE_EVENT_ERROR) {
                ctx->clipsLoaded = false;
                SetStatus(ctx, L"Could not load clip preview with Media Foundation.");
                RefreshClipsPlaybackControls(ctx);
            } else if (eventCode == MF_MEDIA_ENGINE_EVENT_PLAY
                || eventCode == MF_MEDIA_ENGINE_EVENT_PLAYING
                || eventCode == MF_MEDIA_ENGINE_EVENT_PAUSE
                || eventCode == MF_MEDIA_ENGINE_EVENT_ENDED
                || eventCode == MF_MEDIA_ENGINE_EVENT_TIMEUPDATE) {
                ctx->clipsIsPlaying = ctx->clipsPreviewEngine->IsPlaying();
                if (eventCode == MF_MEDIA_ENGINE_EVENT_PLAYING) {
                    SetStatus(ctx, L"Clip playback active.");
                } else if (eventCode == MF_MEDIA_ENGINE_EVENT_ENDED) {
                    SetStatus(ctx, L"Clip playback ended.");
                }
                InvalidateRect(ctx->clipsVideoSurface, nullptr, FALSE);
                RefreshClipsPlaybackControls(ctx);
            }
        }
        return 0;
    case WM_BEAN_CLIPS_EXPORT_COMPLETE: {
        auto* payload = reinterpret_cast<ClipExportCompletePayload*>(lParam);
        if (ctx && payload) {
            SetClipsExportStatus(
                ctx,
                payload->success ? AppContext::ClipExportStatus::Success : AppContext::ClipExportStatus::Failure,
                payload->message);
        }
        delete payload;
        return 0;
    }
    case WM_BEAN_YOUTUBE_AUTH_COMPLETE: {
        auto* payload = reinterpret_cast<YouTubeAuthCompletionPayload*>(lParam);
        if (ctx && payload) {
            ctx->youtubeBusy.store(false);
            if (!payload->success) {
                SetStatus(ctx, std::wstring(L"YouTube link failed: ") + ToWide(payload->error));
            } else {
                ctx->settings.youtubeClientId = payload->clientId;
                ctx->settings.youtubeRefreshToken = payload->refreshToken;
                ctx->settings.youtubeChannelId = payload->channelId;
                ctx->settings.youtubeChannelTitle = payload->channelTitle;
                std::string saveError;
                if (!ctx->settingsStore.Save(ctx->settings, saveError)) {
                    SetStatus(ctx, std::wstring(L"YouTube linked but saving settings failed: ") + ToWide(saveError));
                } else {
                    if (!payload->channelTitle.empty()) {
                        SetStatus(ctx, std::wstring(L"YouTube account linked: ") + ToWide(payload->channelTitle));
                    } else {
                        SetStatus(ctx, L"YouTube account linked successfully.");
                    }
                }
                if (payload->channelId.empty() && payload->channelTitle.empty()) {
                    ResolveLinkedYouTubeIdentityAsync(ctx, true);
                }
            }
            RefreshYouTubeUiState(ctx);
        }
        delete payload;
        return 0;
    }
    case WM_BEAN_YOUTUBE_UPLOAD_PROGRESS: {
        auto* payload = reinterpret_cast<YouTubeUploadProgressPayload*>(lParam);
        if (ctx && payload) {
            if (!payload->videoUrl.empty()) {
                ctx->youtubeLastVideoUrl = payload->videoUrl;
            }
            SetYouTubeUploadUi(ctx, payload->percent, payload->text);
            RefreshYouTubeUiState(ctx);
        }
        delete payload;
        return 0;
    }
    case WM_BEAN_YOUTUBE_IDENTITY_RESOLVED: {
        auto* payload = reinterpret_cast<YouTubeIdentityResolvedPayload*>(lParam);
        if (ctx && payload) {
            if (payload->success) {
                ctx->settings.youtubeChannelId = payload->channelId;
                ctx->settings.youtubeChannelTitle = payload->channelTitle;
                std::string saveError;
                if (!ctx->settingsStore.Save(ctx->settings, saveError)) {
                    SetStatus(ctx, std::wstring(L"Failed to save linked YouTube account details: ") + ToWide(saveError));
                }
            } else if (!payload->error.empty()) {
                if (ctx->settings.youtubeChannelTitle.empty()) {
                    ctx->settings.youtubeChannelTitle = "details unavailable";
                }
                SetStatus(ctx, std::wstring(L"Could not resolve linked YouTube account: ") + ToWide(payload->error));
            }
            RefreshYouTubeUiState(ctx);
        }
        delete payload;
        return 0;
    }
    case WM_BEAN_UPDATE_AVAILABILITY_READY: {
        auto* payload = reinterpret_cast<UpdateAvailabilityPayload*>(lParam);
        if (ctx && payload) {
            ApplyAboutUpdateAvailabilityResult(ctx, *payload);
        }
        delete payload;
        return 0;
    }
    case WM_BEAN_FOLDER_AVAILABILITY_COMPLETE: {
        auto* result = reinterpret_cast<FolderAvailabilityResult*>(lParam);
        if (ctx && result) {
            ctx->folderAvailabilityProbeInFlight.store(false, std::memory_order_release);
            const bool shouldProbeAgain = result->requestId != ctx->folderAvailabilityRequestId;
            if (result->requestId == ctx->folderAvailabilityRequestId) {
                ApplyFolderAvailabilityResult(ctx, *result);
            }
            if (shouldProbeAgain) {
                BeginFolderAvailabilityProbe(ctx);
            }
        }
        delete result;
        return 0;
    }
    case WM_BEAN_DISK_SPACE_COMPLETE: {
        auto* result = reinterpret_cast<DiskSpaceProbeResult*>(lParam);
        if (ctx && result) {
            ctx->diskSpaceProbeInFlight.store(false, std::memory_order_release);
            const bool shouldProbeAgain = result->requestId != ctx->diskSpaceRequestId;
            if (result->requestId == ctx->diskSpaceRequestId) {
                ApplyDiskSpaceProbeResult(ctx, *result);
            }
            if (shouldProbeAgain) {
                BeginDiskSpaceProbe(ctx);
            }
        } else if (ctx) {
            ctx->diskSpaceProbeInFlight.store(false, std::memory_order_release);
        }
        delete result;
        return 0;
    }
    case WM_BEAN_RECORDING_RECONCILIATION_COMPLETE: {
        auto* result = reinterpret_cast<RecordingReconciliationResult*>(lParam);
        if (ctx && result) {
            ctx->recordingReconciliationInFlight.store(false, std::memory_order_release);
            const bool shouldReconcileAgain =
                result->requestId != ctx->recordingReconciliationRequestId;
            if (result->relocatedCount > 0) {
                SetStatus(
                    ctx,
                    L"Rediscovered "
                    + std::to_wstring(result->relocatedCount)
                    + L" relocated recording"
                    + (result->relocatedCount == 1 ? L"." : L"s."));
            }
            if (!result->error.empty()) {
                SetStatus(ctx, L"Recording reconciliation warning: " + result->error);
            }
            RefreshRecordingsList(ctx, false);
            RefreshYouTubeMediaList(ctx, false);
            if (shouldReconcileAgain) {
                BeginRecordingReconciliation(ctx);
            }
        } else if (ctx) {
            ctx->recordingReconciliationInFlight.store(false, std::memory_order_release);
        }
        delete result;
        return 0;
    }
    case WM_BEAN_FFMPEG_PROBE_COMPLETE: {
        auto* result = reinterpret_cast<FfmpegProbeResult*>(lParam);
        if (ctx && result) {
            const bool wasDetected = ctx->ffmpegDetected;
            ctx->ffmpegDetected = result->runnable;
            ctx->ffmpegExecutablePath = result->executablePath;
            ctx->ffmpegProbeInFlight = false;
            if (ctx->ffmpegIcon) {
                InvalidateRect(ctx->ffmpegIcon, nullptr, TRUE);
            }
            if (ctx->ffmpegText) {
                UpdateTransparentStaticText(
                    ctx->ffmpegText,
                    ctx->ffmpegDetected ? L"FFmpeg available for trim" : L"FFmpeg not found for trim");
            }
            RefreshClipsPlaybackControls(ctx);
            if (wasDetected != ctx->ffmpegDetected) {
                if (ctx->statusTabButton) {
                    InvalidateRect(ctx->statusTabButton, nullptr, TRUE);
                }
                ApplyTaskbarOverlayState(ctx);
            }
        } else if (ctx) {
            ctx->ffmpegProbeInFlight = false;
        }
        delete result;
        return 0;
    }
    case WM_QUERYENDSESSION:
        // Do not veto or prompt for an operating-system shutdown/restart.
        return TRUE;
    case WM_ENDSESSION:
        if (wParam && ctx && ctx->orchestrator) {
            // Only stop after Windows has confirmed that the session is ending.
            // Stopping during WM_QUERYENDSESSION would lose a recording if the
            // shutdown were subsequently canceled.
            ctx->alwaysOnMonitoring = false;
            ctx->orchestrator->SetStatusCallback({});
            std::string stopError;
            ctx->orchestrator->StopForShutdown(stopError);
            ctx->orchestrator->StopMonitoring();
        }
        return 0;
    case WM_CLOSE:
        if (ctx && ctx->closeConfirmationBypassRequested) {
            ctx->closeConfirmationBypassRequested = false;
            DestroyWindow(hwnd);
        } else if (MessageBoxW(
                       hwnd,
                       L"Are you sure you want to close Bean?\nYour runs will no longer be recorded.",
                       L"Exit Bean",
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        if (ctx && ctx->shuttingDown.exchange(true, std::memory_order_acq_rel)) {
            return 0;
        }
        DismissCustomComboPopup();
        KillTimer(hwnd, kFolderAvailabilityTimerId);
        if (ctx) {
            ctx->alwaysOnMonitoring = false;
            if (ctx->orchestrator) {
                ctx->orchestrator->SetStatusCallback({});
            }
        }
        FlushPendingAutoSaves(ctx);
        if (ctx) {
            ctx->configurationAutoSaveArmed = false;
            ctx->chatBlockerAutoSaveArmed = false;
        }
        bean::integrations::YouTubeUploader::RequestCancel();
        UnregisterHotKey(hwnd, kClipHotkeyId);
        UnregisterHotKey(hwnd, kManualStartHotkeyId);
        UnregisterHotKey(hwnd, kManualStopHotkeyId);
        if (ctx && ctx->orchestrator) {
            std::string stopError;
            ctx->orchestrator->StopForShutdown(stopError);
            ctx->orchestrator->StopMonitoring();
        }
        JoinAppWorkers(ctx);
        if (ctx) {
            ctx->clipsExportInProgress.store(false);
            ctx->youtubeBusy.store(false);
            ctx->ffmpegProbeInFlight.store(false);
            ctx->folderAvailabilityProbeInFlight.store(false);
        }
        CloseClipMedia(ctx);
        ShutdownTaskbarOverlay(ctx);
        if (ctx && ctx->configurationTooltip && IsWindow(ctx->configurationTooltip)) {
            DestroyWindow(ctx->configurationTooltip);
            ctx->configurationTooltip = nullptr;
        }
        if (ctx && ctx->chatPreviewFrameBitmap) {
            DeleteObject(ctx->chatPreviewFrameBitmap);
            ctx->chatPreviewFrameBitmap = nullptr;
            ctx->chatPreviewFrameValid = false;
        }
        if (ctx && ctx->statusLogStream.is_open()) {
            ctx->statusLogStream.flush();
            ctx->statusLogStream.close();
        }
        KillTimer(hwnd, kLiveStatusTimerId);
        KillTimer(hwnd, kClipsExportStatusTimerId);
        KillTimer(hwnd, kConfigurationAutoSaveTimerId);
        KillTimer(hwnd, kChatBlockerAutoSaveTimerId);
        DiscardQueuedAppMessages(hwnd);
        DestroyParticipantSpecIcons(ctx);
        DestroyThemeResources();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int cmdShow)
{
    std::wstring updaterInitWarning;
    bean::app::InitializeVelopackRuntime(updaterInitWarning);

    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(comInit);

    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_LINK_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    bean::core::AppSettings settings;
    bean::core::SettingsStore settingsStore;
    std::string loadError;
    settingsStore.Load(settings, loadError);
    std::string defaultsWarning;
    const bool defaultsApplied = ApplyReasonableDefaults(settings, defaultsWarning);
    std::string youtubeOAuthWarning;
    if (GetYouTubeAuthServerUrl().empty()) {
        youtubeOAuthWarning = "YouTube auth server is not configured. Set BEAN_YOUTUBE_AUTH_SERVER_URL to an HTTPS URL.";
    }
    if (defaultsApplied) {
        std::string saveError;
        settingsStore.Save(settings, saveError);
        if (!saveError.empty() && loadError.empty()) {
            loadError = "Defaults applied but saving failed: " + saveError;
        }
    }

    auto runRepository = std::make_shared<bean::core::RunRepository>();
    std::string runRepoError;
    if (!runRepository->Initialize(runRepoError)) {
        if (!loadError.empty()) {
            loadError += " ";
        }
        loadError += "Run metadata DB init failed: " + runRepoError;
    }

#if defined(BEAN_ENABLE_LIBOBS) && BEAN_ENABLE_LIBOBS
    auto recorderEngine = std::unique_ptr<bean::obs::IRecorderEngine>(
        std::make_unique<bean::obs::LibObsRecorderEngine>());
#else
    auto recorderEngine = std::unique_ptr<bean::obs::IRecorderEngine>(
        std::make_unique<bean::obs::MockRecorderEngine>());
#endif
    auto orchestrator = std::make_unique<bean::core::RecordingOrchestrator>(std::move(recorderEngine));
    orchestrator->SetRunRepository(runRepository);
    orchestrator->ApplySettings(settings);

    AppContext context;
    context.settingsStore = settingsStore;
    context.runRepository = runRepository;
    context.settings = settings;
    context.orchestrator = std::move(orchestrator);
    InitializeAppIcons(&context);

    context.orchestrator->SetStatusCallback([&context](const std::string& status) {
        PostStatus(&context, ToWide(status));
        if (status.rfind("Clip created:", 0) == 0) {
            PostBeanAppMessage(&context, WM_BEAN_CLIPS_UI_REFRESH);
        }
    });

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = context.idleIcon.largeIcon;

    if (!RegisterClassW(&wc)) {
        return 1;
    }

    const std::wstring windowTitle = MainWindowTitleText();
    HWND hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        windowTitle.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        settings.windowWidth,
        settings.windowHeight,
        nullptr,
        nullptr,
        instance,
        &context);

    if (!hwnd) {
        DestroyAppIcons(&context);
        return 1;
    }
    context.mainWindow = hwnd;
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(context.idleIcon.smallIcon));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(context.idleIcon.largeIcon));
    InitializeTaskbarOverlay(&context);

    if (!loadError.empty()) {
        SetStatus(&context, std::wstring(L"Load settings warning: ") + ToWide(loadError));
    }
    if (!defaultsWarning.empty()) {
        SetStatus(&context, std::wstring(L"Defaults warning: ") + ToWide(defaultsWarning));
    }
    if (!youtubeOAuthWarning.empty()) {
        SetStatus(&context, std::wstring(L"YouTube OAuth warning: ") + ToWide(youtubeOAuthWarning));
    }
    if (!updaterInitWarning.empty()) {
        SetStatus(&context, updaterInitWarning);
    }

    ShowWindow(hwnd, cmdShow);
    UpdateWindow(hwnd);
    // Check in the background as soon as the UI is ready so update notices do
    // not depend on the user opening the About tab.
    RefreshAboutUpdateButtonState(&context);
    // Taskbar overlay icons are ignored until the window has a taskbar button.
    ApplyTaskbarOverlayState(&context, true);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(hwnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (shouldUninitializeCom) {
        CoUninitialize();
    }
    ShutdownTaskbarOverlay(&context);
    DestroyAppIcons(&context);

    return 0;
}
