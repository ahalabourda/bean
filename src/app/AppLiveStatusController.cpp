#include "app/AppLiveStatusController.h"

#include "app/AppClips.h"
#include "app/AppDraw.h"
#include "app/AppIconsTaskbar.h"
#include "app/AppLayout.h"
#include "app/AppPlatformUi.h"
#include "app/AppProbeController.h"
#include "app/AppRecordingHelpers.h"
#include "app/AppStatusLog.h"
#include "app/AppStatusUi.h"
#include "app/AppUtilities.h"
#include "core/GameEnvironment.h"
#include "util/Strings.h"

#include <algorithm>
#include <chrono>

using bean::util::ToWide;

bool DetectAdvancedCombatLoggingForUi(const AppContext* ctx)
{
    return bean::core::IsAdvancedCombatLoggingEnabled(
        ctx ? ctx->settings.wowInstallDirectory : std::filesystem::path{},
        ctx ? ctx->detectedWowEdition : bean::core::WowEdition::Unknown);
}

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
