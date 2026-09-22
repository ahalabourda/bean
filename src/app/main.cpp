#include "app/AppContext.h"
#include "app/AppApplication.h"
#include "app/AppClips.h"
#include "app/AppChatPrivacy.h"
#include "app/AppChatPreview.h"
#include "app/AppDraw.h"
#include "app/AppKeybinds.h"
#include "app/AppLiveStatusController.h"
#include "app/AppIconsTaskbar.h"
#include "app/AppLayout.h"
#include "app/AppLiveStatus.h"
#include "app/AppPanelFactory.h"
#include "app/AppPlatformUi.h"
#include "app/AppProbeController.h"
#include "app/AppRecordings.h"
#include "app/AppYouTube.h"
#include "app/AppYouTubeController.h"
#include "app/AppRecordingHelpers.h"
#include "app/AppStatusLog.h"
#include "app/AppStartupSettings.h"
#include "app/AppStatusUi.h"
#include "app/AppTabs.h"
#include "app/AppTheme.h"
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
#include <dwmapi.h>
#include <gdiplus.h>
#include <mfmediaengine.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
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




void SetActiveTab(AppContext* ctx, AppContext::MainTab tab)
{
    ApplyActiveTab(ctx, tab, RefreshLiveStatus);
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

void ApplyFolderAvailabilityResult(AppContext* ctx, const FolderAvailabilityResult& result);

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
        WM_APP + 117,
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
        case WM_BEAN_RECORDING_RECONCILIATION_COMPLETE:
            delete reinterpret_cast<RecordingReconciliationResult*>(message.lParam);
            break;
        default:
            break;
        }
    }
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

    if (controlId == IDC_YOUTUBE_UPLOAD_BUTTON) {
        PullSettingsFromUi(ctx);
    }
    if (HandleYouTubeCommand(hwnd, ctx, controlId)) {
        RefreshLiveStatus(ctx);
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
    return RunApplication(instance, cmdShow, WindowProc, RefreshAboutUpdateButtonState);
}
