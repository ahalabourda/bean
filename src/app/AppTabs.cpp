#include "app/AppTabs.h"

#include "app/AppClips.h"
#include "app/AppChatPrivacy.h"
#include "app/AppDraw.h"
#include "app/AppRecordings.h"
#include "app/AppYouTubeController.h"

void ApplyActiveTab(AppContext* ctx, AppContext::MainTab tab, RefreshLiveStatusCallback refreshLiveStatus)
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
        if (refreshLiveStatus) {
            refreshLiveStatus(ctx);
        }
    }
}

