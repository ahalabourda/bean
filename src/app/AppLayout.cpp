#include "app/AppLayout.h"

#include <commctrl.h>

#include <algorithm>

namespace {

void MoveControl(HWND parent, int controlId, int x, int y, int width, int height)
{
    if (!parent) {
        return;
    }
    HWND control = GetDlgItem(parent, controlId);
    if (!control) {
        return;
    }
    MoveWindow(control, x, y, (std::max)(width, 10), (std::max)(height, 10), TRUE);
}

void LayoutConfigurationPanel(AppContext* ctx, int panelWidth, int)
{
    if (!ctx || !ctx->recorderPanel) {
        return;
    }

    const int rowHeight = 24;
    const int rowSpacing = 40;
    const int sectionSpacing = 48;
    const int xLabel = 20;
    const int xEdit = 150;
    const int xStatus = panelWidth - 58;
    const int xButton = xStatus - 108;
    const int editWidth = (std::max)(160, xButton - xEdit - 8);
    int y = 20;

    MoveControl(ctx->recorderPanel, IDC_OUTPUT_LABEL, xLabel, y, 120, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_OUTPUT_EDIT, xEdit, y, editWidth, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_OUTPUT_BROWSE, xButton, y, 100, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_OUTPUT_STATUS, xStatus, y, 40, rowHeight);
    y += rowSpacing;

    MoveControl(ctx->recorderPanel, IDC_LOG_LABEL, xLabel, y, 120, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_LOG_EDIT, xEdit, y, editWidth, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_LOG_BROWSE, xButton, y, 100, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_LOG_STATUS, xStatus, y, 40, rowHeight);
    y += rowSpacing;

    const int comboWidth = (std::max)(190, panelWidth - xEdit - 30);
    MoveControl(ctx->recorderPanel, IDC_ENCODER_LABEL, xLabel, y, 120, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_ENCODER_COMBO, xEdit, y, comboWidth, 150);
    y += rowSpacing;

    const int rightContentStart = panelWidth - 220;
    const int containerX = (std::max)(380, rightContentStart - 90);
    const int presetHelpIconSize = 16;
    const int presetHelpGap = 6;
    const int presetWidth = (std::max)(130, containerX - xEdit - (presetHelpIconSize + presetHelpGap + 10));
    MoveControl(ctx->recorderPanel, IDC_PRESET_LABEL, xLabel, y, 120, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_PRESET_COMBO, xEdit, y, presetWidth, 180);
    MoveControl(ctx->recorderPanel, IDC_PRESET_HELP, xEdit + presetWidth + presetHelpGap, y + 4, presetHelpIconSize, presetHelpIconSize);
    MoveControl(ctx->recorderPanel, IDC_CONTAINER_LABEL, containerX, y, 80, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_CONTAINER_COMBO, containerX + 86, y, 120, 140);
    y += rowSpacing;

    MoveControl(ctx->recorderPanel, IDC_AUDIO_SCOPE_LABEL, xLabel, y, 120, rowHeight);
    const int audioScopeGap = 14;
    const int audioScopeWidth = (std::max)(130, (panelWidth - xEdit - 30 - (audioScopeGap * 2)) / 3);
    MoveControl(ctx->recorderPanel, IDC_AUDIO_SCOPE_CHECK, xEdit, y, audioScopeWidth, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_AUDIO_SCOPE_WOW_DISCORD_RADIO, xEdit + audioScopeWidth + audioScopeGap, y, audioScopeWidth, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_AUDIO_SCOPE_ALL_RADIO, xEdit + (audioScopeWidth + audioScopeGap) * 2, y, audioScopeWidth, rowHeight);
    y += rowSpacing;
    MoveControl(ctx->recorderPanel, IDC_MICROPHONE_CHECK, xEdit, y, 220, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_MICROPHONE_NOISE_SUPPRESSION_CHECK, xEdit + 226, y, 220, rowHeight);
    y += rowSpacing;
    MoveControl(ctx->recorderPanel, IDC_MICROPHONE_COMBO, xEdit, y, panelWidth - xEdit - 30, 160);
    y += rowSpacing;

    MoveControl(ctx->recorderPanel, IDC_WIDTH_LABEL, xLabel, y, 60, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_WIDTH_EDIT, xLabel + 60, y, 80, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_HEIGHT_LABEL, xLabel + 160, y, 60, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_HEIGHT_EDIT, xLabel + 220, y, 80, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_FPS_LABEL, xLabel + 320, y, 40, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_FPS_EDIT, xLabel + 360, y, 60, rowHeight);
    y += rowSpacing;

    MoveControl(ctx->recorderPanel, IDC_POST_RUN_DELAY_LABEL, xLabel, y, 104, rowHeight);
    MoveControl(ctx->recorderPanel, IDC_POST_RUN_DELAY_HELP, xLabel + 104, y + 4, 16, 16);
    MoveControl(ctx->recorderPanel, IDC_POST_RUN_DELAY_EDIT, xLabel + 120, y, 70, rowHeight);
    y += sectionSpacing;
    MoveControl(ctx->recorderPanel, IDC_CONFIGURATION_AUTOSAVE_HINT, xLabel, y, panelWidth - (xLabel * 2), rowHeight);
}

void LayoutStatusPanel(AppContext* ctx, int panelWidth, int panelHeight)
{
    if (!ctx || !ctx->statusPanel) {
        return;
    }

    const int rowHeight = 24;
    const int xLabel = 20;
    int y = 20;

    const int commandButtonWidth = (std::max)(110, (panelWidth - 2 * xLabel - 24) / 4);
    MoveControl(ctx->statusPanel, IDC_MONITOR_START, xLabel, y, commandButtonWidth, rowHeight + 4);
    MoveControl(ctx->statusPanel, IDC_MONITOR_STOP, xLabel + commandButtonWidth + 8, y, commandButtonWidth, rowHeight + 4);
    MoveControl(ctx->statusPanel, IDC_RECORD_START, xLabel + (commandButtonWidth + 8) * 2, y, commandButtonWidth, rowHeight + 4);
    MoveControl(ctx->statusPanel, IDC_RECORD_STOP, xLabel + (commandButtonWidth + 8) * 3, y, commandButtonWidth, rowHeight + 4);
    y += 46;

    MoveControl(ctx->statusPanel, IDC_LIVE_LABEL, xLabel, y, 90, rowHeight);
    MoveControl(ctx->statusPanel, IDC_MONITOR_ICON, xLabel + 98, y + 6, 12, 12);
    MoveControl(ctx->statusPanel, IDC_MONITOR_TEXT, xLabel + 115, y, 90, rowHeight);
    MoveControl(ctx->statusPanel, IDC_RECORD_ICON, xLabel + 218, y + 6, 12, 12);
    MoveControl(ctx->statusPanel, IDC_RECORD_TEXT, xLabel + 235, y, 90, rowHeight);
    MoveControl(ctx->statusPanel, IDC_LENGTH_LABEL, xLabel + 335, y, 60, rowHeight);
    MoveControl(ctx->statusPanel, IDC_LENGTH_VALUE, xLabel + 400, y, 110, rowHeight);
    y += 36;

    MoveControl(ctx->statusPanel, IDC_WOW_WINDOW_LABEL, xLabel, y, 100, rowHeight);
    MoveControl(ctx->statusPanel, IDC_WOW_WINDOW_ICON, xLabel + 102, y + 3, 20, rowHeight);
    const int wowTextX = xLabel + 126;
    const int wowTextWidth = (std::max)(250, panelWidth - wowTextX - 20);
    MoveControl(ctx->statusPanel, IDC_WOW_WINDOW_TEXT, wowTextX, y, wowTextWidth, rowHeight);
    y += 40;

    MoveControl(ctx->statusPanel, IDC_OBS_INSTALL_LABEL, xLabel, y, 100, rowHeight);
    MoveControl(ctx->statusPanel, IDC_OBS_INSTALL_ICON, xLabel + 102, y + 3, 20, rowHeight);
    MoveControl(ctx->statusPanel, IDC_OBS_INSTALL_TEXT, wowTextX, y, wowTextWidth, rowHeight);
    y += 40;

    MoveControl(ctx->statusPanel, IDC_FFMPEG_LABEL, xLabel, y, 100, rowHeight);
    MoveControl(ctx->statusPanel, IDC_FFMPEG_ICON, xLabel + 102, y + 3, 20, rowHeight);
    MoveControl(ctx->statusPanel, IDC_FFMPEG_TEXT, wowTextX, y, wowTextWidth, rowHeight);
    y += 40;

    if (ctx->warcraftRecorderDetected) {
        MoveControl(ctx->statusPanel, IDC_WARCRAFT_RECORDER_LABEL, xLabel, y, 126, rowHeight);
        MoveControl(ctx->statusPanel, IDC_WARCRAFT_RECORDER_ICON, xLabel + 128, y + 3, 20, rowHeight);
        MoveControl(ctx->statusPanel, IDC_WARCRAFT_RECORDER_TEXT, wowTextX + 26, y, (std::max)(220, wowTextWidth - 26), rowHeight);
        y += 40;
    }

    MoveControl(ctx->statusPanel, IDC_ADVANCED_LOGGING_LABEL, xLabel, y, 160, rowHeight);
    MoveControl(ctx->statusPanel, IDC_ADVANCED_LOGGING_ICON, xLabel + 162, y + 3, 20, rowHeight);
    const int advancedTextX = wowTextX + 60;
    const int advancedHelpIconSize = 16;
    const int advancedHelpGap = 6;
    const int advancedTextWidth = 90;
    MoveControl(ctx->statusPanel, IDC_ADVANCED_LOGGING_TEXT, advancedTextX, y, advancedTextWidth, rowHeight);
    MoveControl(
        ctx->statusPanel,
        IDC_ADVANCED_LOGGING_HELP,
        advancedTextX + advancedTextWidth + advancedHelpGap,
        y + 4,
        advancedHelpIconSize,
        advancedHelpIconSize);
    y += 40;

    MoveControl(ctx->statusPanel, IDC_STATUS_LABEL, xLabel, y, 60, rowHeight);
    const int statusX = xLabel + 72;
    const int statusWidth = (std::max)(260, panelWidth - statusX - 20);
    const int statusButtonHeight = rowHeight + 4;
    const int statusBottomPadding = 12;
    const int statusGap = 8;
    const int statusHeight = (std::max)(90, panelHeight - y - statusBottomPadding - statusButtonHeight - statusGap);
    MoveControl(ctx->statusPanel, IDC_STATUS_TEXT, statusX, y, statusWidth, statusHeight);
    const int openLogButtonWidth = (std::min)(200, (std::max)(140, statusWidth));
    const int openLogButtonX = statusX + statusWidth - openLogButtonWidth;
    const int openLogButtonY = y + statusHeight + statusGap;
    MoveControl(ctx->statusPanel, IDC_STATUS_OPEN_LOG_FOLDER, openLogButtonX, openLogButtonY, openLogButtonWidth, statusButtonHeight);
}

void LayoutRecordingsPanel(AppContext* ctx, int panelWidth, int panelHeight)
{
    if (!ctx || !ctx->recordingsPanel) {
        return;
    }

    const int rowHeight = 24;
    const int left = 20;
    const int right = panelWidth - 20;
    const int listTop = 90;
    const int lowerAreaTop = panelHeight - 140;
    const int listHeight = (std::max)(140, lowerAreaTop - listTop);

    const int participantsGap = 12;
    const int participantsWidth = (std::max)(170, (std::min)(240, panelWidth / 4));
    const int listWidth = (std::max)(240, right - left - participantsWidth - participantsGap);
    const int participantsLeft = left + listWidth + participantsGap;

    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_LABEL, left, 20, right - left, rowHeight);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_REFRESH, left, 52, 100, rowHeight + 4);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_OPEN_FOLDER, left + 110, 52, 120, rowHeight + 4);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_OPEN_DB_FOLDER, left + 240, 52, 130, rowHeight + 4);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_LIST, left, listTop, listWidth, listHeight);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_INFO_LABEL, participantsLeft, listTop, participantsWidth, rowHeight);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_INFO_TEXT, participantsLeft, listTop + rowHeight, participantsWidth, (std::max)(100, listHeight - rowHeight));
    if (ctx->recordingsList) {
        const int dungeonWidth = (std::max)(120, listWidth * 40 / 100);
        const int keyWidth = (std::max)(56, listWidth * 11 / 100);
        const int lengthWidth = (std::max)(88, listWidth * 17 / 100);
        const int reservedRightPadding = GetSystemMetrics(SM_CXVSCROLL) + 12;
        const int dateWidth = (std::max)(94, listWidth - dungeonWidth - keyWidth - lengthWidth - reservedRightPadding);
        ListView_SetColumnWidth(ctx->recordingsList, 0, dungeonWidth);
        ListView_SetColumnWidth(ctx->recordingsList, 1, keyWidth);
        ListView_SetColumnWidth(ctx->recordingsList, 2, lengthWidth);
        ListView_SetColumnWidth(ctx->recordingsList, 3, dateWidth);
    }
    if (ctx->recordingsInfoText) {
        ListView_SetColumnWidth(ctx->recordingsInfoText, 0, (std::max)(80, participantsWidth - 8));
    }

    const int accountTop = listTop + listHeight + 12;
    const int rightColumnWidth = (std::max)(220, (std::min)(320, (right - left) / 3));
    const int rightColumnX = right - rightColumnWidth;
    const int leftColumnWidth = (std::max)(240, rightColumnX - left - 12);
    const int statusIconWidth = 24;
    const int actionX = rightColumnX + statusIconWidth + 8;
    const int actionWidth = (std::max)(120, rightColumnWidth - statusIconWidth - 8);
    const int confirmLabelWidth = (std::max)(80, (std::min)(110, actionWidth / 2));
    const int confirmButtonWidth = (std::max)(44, (actionWidth - confirmLabelWidth - 8) / 2);
    constexpr int accountLabelWidth = 154;
    const int accountFieldX = left + accountLabelWidth + 10;
    const int accountFieldWidth = leftColumnWidth - accountLabelWidth - 10;
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_ACCOUNT_LABEL, left, accountTop, accountLabelWidth, rowHeight);
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_ACCOUNT_LINK, accountFieldX, accountTop - 2, accountFieldWidth, rowHeight + 8);
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_LINK_STATUS, rightColumnX, accountTop, statusIconWidth, rowHeight);
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_LINK_BUTTON, actionX, accountTop - 2, actionWidth, rowHeight + 4);
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_UNLINK_BUTTON, actionX, accountTop - 2, actionWidth, rowHeight + 4);
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_UNLINK_CONFIRM_LABEL, actionX, accountTop, confirmLabelWidth, rowHeight);
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_UNLINK_YES_BUTTON, actionX + confirmLabelWidth + 8, accountTop - 2, confirmButtonWidth, rowHeight + 4);
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_UNLINK_NO_BUTTON, actionX + confirmLabelWidth + 8 + confirmButtonWidth + 6, accountTop - 2, confirmButtonWidth, rowHeight + 4);

    const int uploadControlsTop = accountTop + 36;
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_TITLE_LABEL, left, uploadControlsTop, 120, rowHeight);
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_TITLE_EDIT, left + 130, uploadControlsTop, leftColumnWidth - 130, rowHeight);
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_PRIVACY_LABEL, rightColumnX, uploadControlsTop, 70, rowHeight);
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_PRIVACY_COMBO, rightColumnX + 74, uploadControlsTop, rightColumnWidth - 74, 140);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_UPLOAD_PROGRESS, left, uploadControlsTop + 34, leftColumnWidth, 20);
    MoveControl(ctx->recordingsPanel, IDC_YOUTUBE_UPLOAD_BUTTON, rightColumnX, uploadControlsTop + 32, rightColumnWidth, rowHeight + 4);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_UPLOAD_STATUS, left, uploadControlsTop + 58, right - left, rowHeight);
}

void LayoutChatPrivacyPanel(AppContext* ctx, int panelWidth, int panelHeight)
{
    if (!ctx || !ctx->chatPrivacyPanel) {
        return;
    }

    const int rowHeight = 24;
    const int left = 20;
    const int right = panelWidth - 20;
    int y = 20;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_ENABLED_CHECK, left, y, 240, rowHeight);
    y += 32;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_BLANK_RADIO, left + 104, y, 104, rowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_CUSTOM_RADIO, left + 214, y, 104, rowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_IMPORT_BUTTON, left + 324, y, 78, rowHeight + 2);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_OPEN_FOLDER_BUTTON, left + 408, y, 108, rowHeight + 2);
    y += 36;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_LIBRARY_LABEL, left, y, 96, rowHeight);
    const int comboWidth = (std::max)(160, right - (left + 114));
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_COMBO, left + 114, y, comboWidth, 180);
    y += 34;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_WIDTH_LABEL, left, y, 110, rowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_WIDTH_EDIT, left + 114, y, 90, rowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_HEIGHT_LABEL, left + 218, y, 110, rowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_HEIGHT_EDIT, left + 328, y, 90, rowHeight);
    y += 34;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_ANCHOR_LABEL, left, y, 110, rowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_ANCHOR_COMBO, left + 114, y, 180, 140);
    y += 38;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_PREVIEW_LABEL, left, y, right - left, rowHeight);
    y += 28;

    const int previewHeight = (std::max)(220, panelHeight - y - 42);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_PREVIEW, left, y, right - left, previewHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_PREVIEW_STATUS, left, y + previewHeight + 8, right - left, rowHeight);
}

void LayoutAboutPanel(AppContext* ctx, int panelWidth, int)
{
    if (!ctx || !ctx->aboutPanel) {
        return;
    }
    const int rowHeight = 24;
    const int left = 20;
    const int rightButtonX = panelWidth - 170;
    const int valueWidth = (std::max)(180, rightButtonX - 150 - 14);

    MoveControl(ctx->aboutPanel, IDC_ABOUT_TITLE_LABEL, left, 24, panelWidth - 40, 34);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_BUILD_TEXT, left, 58, panelWidth - 40, rowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_WEBSITE_LABEL, left, 96, 120, rowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_WEBSITE_TEXT, 150, 96, valueWidth, rowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_WEBSITE_BUTTON, rightButtonX, 94, 150, rowHeight + 4);

    MoveControl(ctx->aboutPanel, IDC_ABOUT_EMAIL_LABEL, left, 134, 120, rowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_EMAIL_TEXT, 150, 134, valueWidth, rowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_EMAIL_BUTTON, rightButtonX, 132, 150, rowHeight + 4);

    MoveControl(ctx->aboutPanel, IDC_ABOUT_DISCORD_LABEL, left, 172, 120, rowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_DISCORD_TEXT, 150, 172, valueWidth, rowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_DISCORD_BUTTON, rightButtonX, 170, 150, rowHeight + 4);

    MoveControl(ctx->aboutPanel, IDC_ABOUT_UPDATE_LABEL, left, 210, 120, rowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_UPDATE_TEXT, 150, 210, valueWidth, rowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_CHECK_UPDATES_BUTTON, rightButtonX, 208, 150, rowHeight + 4);
}

void LayoutClipsPanel(AppContext* ctx, int panelWidth, int panelHeight)
{
    if (!ctx || !ctx->clipsPanel) {
        return;
    }

    const int rowHeight = 24;
    const int left = 20;
    const int right = panelWidth - 20;
    const int top = 20;
    const int bottomPadding = 20;

    MoveControl(ctx->clipsPanel, IDC_CLIPS_SOURCE_LABEL, left, top, 110, rowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_SOURCE_COMBO, left + 114, top, (std::max)(260, right - left - 230), 240);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_REFRESH, right - 90, top - 1, 90, rowHeight + 4);

    const int actionsTop = panelHeight - bottomPadding - (rowHeight + 4) + 1;
    const int clippingTop = actionsTop - 34;
    const int volumeTop = clippingTop - 34;
    const int playbackTop = volumeTop - 34;
    const int videoTop = top + 36;
    const int videoBottom = playbackTop - 10;
    const int videoHeight = (std::max)(120, videoBottom - videoTop);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_VIDEO_SURFACE, left, videoTop, right - left, videoHeight);

    MoveControl(ctx->clipsPanel, IDC_CLIPS_PLAY_PAUSE, left, playbackTop, 90, rowHeight + 4);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_TIMELINE, left + 96, playbackTop + 1, (std::max)(140, right - left - 330), rowHeight + 2);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_POSITION_TEXT, right - 228, playbackTop + 2, 228, rowHeight);

    MoveControl(ctx->clipsPanel, IDC_CLIPS_VOLUME_LABEL, left, volumeTop, 60, rowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_VOLUME_SLIDER, left + 62, volumeTop, 170, rowHeight);

    MoveControl(ctx->clipsPanel, IDC_CLIPS_START_LABEL, left, clippingTop, 88, rowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_START_EDIT, left + 92, clippingTop, 74, rowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_SET_START, left + 170, clippingTop - 1, 90, rowHeight + 4);

    MoveControl(ctx->clipsPanel, IDC_CLIPS_END_LABEL, left + 274, clippingTop, 82, rowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_END_EDIT, left + 360, clippingTop, 74, rowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_SET_END, left + 438, clippingTop - 1, 90, rowHeight + 4);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_EXPORT, left, actionsTop - 1, 110, rowHeight + 4);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_OPEN_FOLDER, left + 120, actionsTop - 1, 110, rowHeight + 4);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_FFMPEG_WARNING, left + 240, actionsTop, right - left - 240, rowHeight + 4);
}

} // namespace

void LayoutMainUi(AppContext* ctx, int clientWidth, int clientHeight)
{
    if (!ctx || !ctx->statusPanel || !ctx->recorderPanel || !ctx->chatPrivacyPanel || !ctx->recordingsPanel || !ctx->clipsPanel || !ctx->aboutPanel) {
        return;
    }

    const int outer = 12;
    const int navWidth = (std::max)(120, (std::min)(160, clientWidth / 6));
    const int navY = 20;
    const int navHeight = 34;
    const int panelX = outer + navWidth + 16;
    const int panelY = 14;
    const int panelWidth = (std::max)(320, clientWidth - panelX - outer);
    const int panelHeight = (std::max)(240, clientHeight - panelY - outer);

    MoveWindow(ctx->statusTabButton, outer, navY, navWidth, navHeight, TRUE);
    MoveWindow(ctx->configurationTabButton, outer, navY + 40, navWidth, navHeight, TRUE);
    MoveWindow(ctx->chatPrivacyTabButton, outer, navY + 80, navWidth, navHeight, TRUE);
    MoveWindow(ctx->recordingsTabButton, outer, navY + 120, navWidth, navHeight, TRUE);
    MoveWindow(ctx->clipsTabButton, outer, navY + 160, navWidth, navHeight, TRUE);
    MoveWindow(ctx->aboutTabButton, outer, navY + 200, navWidth, navHeight, TRUE);

    MoveWindow(ctx->statusPanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->recorderPanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->chatPrivacyPanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->recordingsPanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->clipsPanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->aboutPanel, panelX, panelY, panelWidth, panelHeight, TRUE);

    LayoutStatusPanel(ctx, panelWidth, panelHeight);
    LayoutConfigurationPanel(ctx, panelWidth, panelHeight);
    LayoutChatPrivacyPanel(ctx, panelWidth, panelHeight);
    LayoutRecordingsPanel(ctx, panelWidth, panelHeight);
    LayoutClipsPanel(ctx, panelWidth, panelHeight);
    LayoutAboutPanel(ctx, panelWidth, panelHeight);

    InvalidateRect(ctx->mainWindow, nullptr, TRUE);
}
