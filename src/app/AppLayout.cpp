#include "app/AppLayout.h"
#include "app/AppPrerequisites.h"

#include <commctrl.h>

#include <algorithm>

namespace {

namespace LayoutMetrics {
constexpr int kPanelInset = 20;
constexpr int kRowHeight = 24;
constexpr int kButtonHeight = kRowHeight + 4;
constexpr int kRowSpacing = 40;
constexpr int kHelpIconSize = 16;
constexpr int kHelpIconGap = 6;
}

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

    constexpr int sectionSpacing = 48;
    constexpr int xEdit = 150;
    const int xStatus = panelWidth - 58;
    const int xButton = xStatus - 108;
    const int editWidth = (std::max)(160, xButton - xEdit - 8);
    int y = 20;

    MoveControl(ctx->recorderPanel, IDC_OUTPUT_LABEL, LayoutMetrics::kPanelInset, y, 120, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_OUTPUT_EDIT, xEdit, y, editWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_OUTPUT_BROWSE, xButton, y, 100, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_OUTPUT_STATUS, xStatus, y, 40, LayoutMetrics::kRowHeight);
    y += LayoutMetrics::kRowSpacing;

    MoveControl(ctx->recorderPanel, IDC_LOG_LABEL, LayoutMetrics::kPanelInset, y, 120, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_LOG_EDIT, xEdit, y, editWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_LOG_BROWSE, xButton, y, 100, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_LOG_STATUS, xStatus, y, 40, LayoutMetrics::kRowHeight);
    y += LayoutMetrics::kRowSpacing;

    const int comboWidth = (std::max)(190, panelWidth - xEdit - 30);
    MoveControl(ctx->recorderPanel, IDC_ENCODER_LABEL, LayoutMetrics::kPanelInset, y, 120, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_ENCODER_COMBO, xEdit, y, comboWidth, 150);
    y += LayoutMetrics::kRowSpacing;

    const int rightContentStart = panelWidth - 220;
    const int containerX = (std::max)(380, rightContentStart - 90);
    const int presetWidth = (std::max)(130, containerX - xEdit - (LayoutMetrics::kHelpIconSize + LayoutMetrics::kHelpIconGap + 10));
    MoveControl(ctx->recorderPanel, IDC_PRESET_LABEL, LayoutMetrics::kPanelInset, y, 120, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_PRESET_COMBO, xEdit, y, presetWidth, 180);
    MoveControl(ctx->recorderPanel, IDC_PRESET_HELP, xEdit + presetWidth + LayoutMetrics::kHelpIconGap, y + 4, LayoutMetrics::kHelpIconSize, LayoutMetrics::kHelpIconSize);
    MoveControl(ctx->recorderPanel, IDC_CONTAINER_LABEL, containerX, y, 80, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_CONTAINER_COMBO, containerX + 86, y, 120, 140);
    y += LayoutMetrics::kRowSpacing;

    MoveControl(ctx->recorderPanel, IDC_AUDIO_SCOPE_LABEL, LayoutMetrics::kPanelInset, y, 120, LayoutMetrics::kRowHeight);
    constexpr int audioScopeGap = 6;
    constexpr int audioScopeWowOnlyWidth = 120;
    constexpr int audioScopeWowDiscordWidth = 175;
    const int audioScopeAvailableWidth = (std::max)(0, panelWidth - xEdit - 30);
    const int audioScopeAllDesktopWidth = (std::max)(
        190,
        audioScopeAvailableWidth
            - audioScopeWowOnlyWidth
            - audioScopeWowDiscordWidth
            - (audioScopeGap * 2));
    const int audioScopeWowDiscordX = xEdit + audioScopeWowOnlyWidth + audioScopeGap;
    const int audioScopeAllDesktopX = audioScopeWowDiscordX + audioScopeWowDiscordWidth + audioScopeGap;
    MoveControl(ctx->recorderPanel, IDC_AUDIO_SCOPE_CHECK, xEdit, y, audioScopeWowOnlyWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_AUDIO_SCOPE_WOW_DISCORD_RADIO, audioScopeWowDiscordX, y, audioScopeWowDiscordWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_AUDIO_SCOPE_ALL_RADIO, audioScopeAllDesktopX, y, audioScopeAllDesktopWidth, LayoutMetrics::kRowHeight);
    y += LayoutMetrics::kRowSpacing;
    MoveControl(ctx->recorderPanel, IDC_MICROPHONE_CHECK, xEdit, y, 220, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_MICROPHONE_NOISE_SUPPRESSION_CHECK, xEdit + 226, y, 220, LayoutMetrics::kRowHeight);
    y += LayoutMetrics::kRowSpacing;
    MoveControl(ctx->recorderPanel, IDC_MICROPHONE_COMBO, xEdit, y, panelWidth - xEdit - 30, 160);
    y += LayoutMetrics::kRowSpacing;

    MoveControl(ctx->recorderPanel, IDC_RECORDING_RESOLUTION_LABEL, LayoutMetrics::kPanelInset, y, 120, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_RECORDING_RESOLUTION_COMBO, xEdit, y, 290, 180);
    y += LayoutMetrics::kRowSpacing;
    MoveControl(ctx->recorderPanel, IDC_FPS_LABEL, LayoutMetrics::kPanelInset, y, 40, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_FPS_EDIT, LayoutMetrics::kPanelInset + 46, y, 60, LayoutMetrics::kRowHeight);
    y += LayoutMetrics::kRowSpacing;

    MoveControl(ctx->recorderPanel, IDC_POST_RUN_DELAY_LABEL, LayoutMetrics::kPanelInset, y, 104, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_POST_RUN_DELAY_HELP, LayoutMetrics::kPanelInset + 104, y + 4, LayoutMetrics::kHelpIconSize, LayoutMetrics::kHelpIconSize);
    MoveControl(ctx->recorderPanel, IDC_POST_RUN_DELAY_EDIT, LayoutMetrics::kPanelInset + 120, y, 70, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_CLIP_DURATION_LABEL, LayoutMetrics::kPanelInset + 230, y, 116, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recorderPanel, IDC_CLIP_DURATION_EDIT, LayoutMetrics::kPanelInset + 350, y, 70, LayoutMetrics::kRowHeight);
    y += sectionSpacing;
    MoveControl(ctx->recorderPanel, IDC_CONFIGURATION_AUTOSAVE_HINT, LayoutMetrics::kPanelInset, y, panelWidth - (LayoutMetrics::kPanelInset * 2), LayoutMetrics::kRowHeight);
}

void LayoutStatusPanel(AppContext* ctx, int panelWidth, int panelHeight)
{
    if (!ctx || !ctx->statusPanel) {
        return;
    }

    int y = 20;

    const int commandButtonWidth = (std::max)(110, (panelWidth - 2 * LayoutMetrics::kPanelInset - 8) / 2);
    MoveControl(ctx->statusPanel, IDC_RECORD_START, LayoutMetrics::kPanelInset, y, commandButtonWidth, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->statusPanel, IDC_RECORD_STOP, LayoutMetrics::kPanelInset + commandButtonWidth + 8, y, commandButtonWidth, LayoutMetrics::kButtonHeight);
    y += 46;

    MoveControl(ctx->statusPanel, IDC_LIVE_LABEL, LayoutMetrics::kPanelInset, y, 90, LayoutMetrics::kRowHeight);
    MoveControl(ctx->statusPanel, IDC_MONITOR_ICON, LayoutMetrics::kPanelInset + 98, y + 6, 12, 12);
    MoveControl(ctx->statusPanel, IDC_MONITOR_TEXT, LayoutMetrics::kPanelInset + 115, y, 90, LayoutMetrics::kRowHeight);
    MoveControl(ctx->statusPanel, IDC_RECORD_ICON, LayoutMetrics::kPanelInset + 218, y + 6, 12, 12);
    MoveControl(ctx->statusPanel, IDC_RECORD_TEXT, LayoutMetrics::kPanelInset + 235, y, 90, LayoutMetrics::kRowHeight);
    MoveControl(ctx->statusPanel, IDC_LENGTH_LABEL, LayoutMetrics::kPanelInset + 335, y, 60, LayoutMetrics::kRowHeight);
    MoveControl(ctx->statusPanel, IDC_LENGTH_VALUE, LayoutMetrics::kPanelInset + 400, y, 110, LayoutMetrics::kRowHeight);
    y += 36;

    constexpr int wowTextX = LayoutMetrics::kPanelInset + 126;
    const int wowTextWidth = (std::max)(250, panelWidth - wowTextX - 20);
    for (const auto& row : kPrerequisiteRows) {
        if (row.visibleOnlyWhenUnhealthy && !ctx->warcraftRecorderDetected) {
            continue;
        }
        const int labelWidth = row.iconId == IDC_WARCRAFT_RECORDER_ICON ? 126
            : (row.iconId == IDC_ADVANCED_LOGGING_ICON ? 160 : 100);
        const int iconX = LayoutMetrics::kPanelInset + labelWidth + 2;
        const int textX = row.iconId == IDC_WARCRAFT_RECORDER_ICON ? (wowTextX + 26)
            : (row.iconId == IDC_ADVANCED_LOGGING_ICON ? (wowTextX + 60) : wowTextX);
        int textWidth = row.iconId == IDC_WARCRAFT_RECORDER_ICON
            ? (std::max)(220, wowTextWidth - 26)
            : wowTextWidth;
        if (row.iconId == IDC_ADVANCED_LOGGING_ICON) {
            textWidth = 90;
        }
        MoveControl(ctx->statusPanel, row.labelId, LayoutMetrics::kPanelInset, y, labelWidth, LayoutMetrics::kRowHeight);
        MoveControl(ctx->statusPanel, row.iconId, iconX, y + 3, 20, LayoutMetrics::kRowHeight);
        MoveControl(ctx->statusPanel, row.textId, textX, y, textWidth, LayoutMetrics::kRowHeight);
        if (row.iconId == IDC_ADVANCED_LOGGING_ICON) {
            MoveControl(
                ctx->statusPanel,
                IDC_ADVANCED_LOGGING_HELP,
                textX + textWidth + LayoutMetrics::kHelpIconGap,
                y + 4,
                LayoutMetrics::kHelpIconSize,
                LayoutMetrics::kHelpIconSize);
        }
        y += LayoutMetrics::kRowSpacing;
    }

    MoveControl(ctx->statusPanel, IDC_STATUS_LABEL, LayoutMetrics::kPanelInset, y, 60, LayoutMetrics::kRowHeight);
    constexpr int statusX = LayoutMetrics::kPanelInset + 72;
    const int statusWidth = (std::max)(260, panelWidth - statusX - 20);
    constexpr int statusBottomPadding = 12;
    constexpr int statusGap = 8;
    const int statusHeight = (std::max)(90, panelHeight - y - statusBottomPadding - LayoutMetrics::kButtonHeight - statusGap);
    MoveControl(ctx->statusPanel, IDC_STATUS_TEXT, statusX, y, statusWidth, statusHeight);
    const int openLogButtonWidth = (std::min)(200, (std::max)(140, statusWidth));
    const int openLogButtonX = statusX + statusWidth - openLogButtonWidth;
    const int openLogButtonY = y + statusHeight + statusGap;
    MoveControl(ctx->statusPanel, IDC_STATUS_OPEN_LOG_FOLDER, openLogButtonX, openLogButtonY, openLogButtonWidth, LayoutMetrics::kButtonHeight);
}

void LayoutRecordingsPanel(AppContext* ctx, int panelWidth, int panelHeight)
{
    if (!ctx || !ctx->recordingsPanel) {
        return;
    }

    const int right = panelWidth - LayoutMetrics::kPanelInset;
    constexpr int listTop = 90;
    const int listHeight = (std::max)(180, panelHeight - listTop - 20);

    constexpr int participantsGap = 12;
    const int participantsWidth = (std::max)(170, (std::min)(240, panelWidth / 4));
    const int listWidth = (std::max)(240, right - LayoutMetrics::kPanelInset - participantsWidth - participantsGap);
    const int participantsLeft = LayoutMetrics::kPanelInset + listWidth + participantsGap;

    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_LABEL, LayoutMetrics::kPanelInset, LayoutMetrics::kPanelInset, right - LayoutMetrics::kPanelInset, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_REFRESH, LayoutMetrics::kPanelInset, 52, 100, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_OPEN_FOLDER, LayoutMetrics::kPanelInset + 110, 52, 120, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_OPEN_DB_FOLDER, LayoutMetrics::kPanelInset + 240, 52, 130, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_LIST, LayoutMetrics::kPanelInset, listTop, listWidth, listHeight);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_INFO_LABEL, participantsLeft, listTop, participantsWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->recordingsPanel, IDC_RECORDINGS_INFO_TEXT, participantsLeft, listTop + LayoutMetrics::kRowHeight, participantsWidth, (std::max)(100, listHeight - LayoutMetrics::kRowHeight));
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

}

void LayoutYouTubePanel(AppContext* ctx, int panelWidth, int panelHeight)
{
    if (!ctx || !ctx->youtubePanel) {
        return;
    }

    constexpr int listTop = 96;
    const int right = panelWidth - LayoutMetrics::kPanelInset;
    const int accountRightWidth = (std::max)(250, (std::min)(340, (right - LayoutMetrics::kPanelInset) / 3));
    const int accountRightX = right - accountRightWidth;
    const int accountLeftWidth = (std::max)(240, accountRightX - LayoutMetrics::kPanelInset - 12);
    constexpr int statusIconWidth = 24;
    const int actionX = accountRightX + statusIconWidth + 8;
    const int actionWidth = (std::max)(120, accountRightWidth - statusIconWidth - 8);
    const int confirmLabelWidth = (std::max)(80, (std::min)(110, actionWidth / 2));
    const int confirmButtonWidth = (std::max)(44, (actionWidth - confirmLabelWidth - 8) / 2);
    constexpr int accountLabelWidth = 154;
    const int accountFieldX = LayoutMetrics::kPanelInset + accountLabelWidth + 10;
    const int accountFieldWidth = accountLeftWidth - accountLabelWidth - 10;

    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_ACCOUNT_LABEL, LayoutMetrics::kPanelInset, LayoutMetrics::kPanelInset, accountLabelWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_ACCOUNT_LINK, accountFieldX, LayoutMetrics::kPanelInset - 2, accountFieldWidth, LayoutMetrics::kRowHeight + 8);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_LINK_STATUS, accountRightX, LayoutMetrics::kPanelInset, statusIconWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_LINK_BUTTON, actionX, LayoutMetrics::kPanelInset - 2, actionWidth, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_UNLINK_BUTTON, actionX, LayoutMetrics::kPanelInset - 2, actionWidth, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_UNLINK_CONFIRM_LABEL, actionX, LayoutMetrics::kPanelInset, confirmLabelWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_UNLINK_YES_BUTTON, actionX + confirmLabelWidth + 8, LayoutMetrics::kPanelInset - 2, confirmButtonWidth, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_UNLINK_NO_BUTTON, actionX + confirmLabelWidth + 8 + confirmButtonWidth + 6, LayoutMetrics::kPanelInset - 2, confirmButtonWidth, LayoutMetrics::kButtonHeight);

    constexpr int refreshWidth = 96;
    constexpr int refreshGap = 10;
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_LABEL, LayoutMetrics::kPanelInset, 58, right - LayoutMetrics::kPanelInset - refreshWidth - refreshGap, LayoutMetrics::kRowHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_REFRESH, right - refreshWidth, 57, refreshWidth, LayoutMetrics::kButtonHeight);
    const int preferredListHeight = (std::max)(140, panelHeight - listTop - 114);
    const int maxListHeight = (std::max)(0, panelHeight - listTop - 114);
    const int listHeight = (std::min)(preferredListHeight, maxListHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_MEDIA_LIST, LayoutMetrics::kPanelInset, listTop, right - LayoutMetrics::kPanelInset, listHeight);
    if (ctx->youtubeMediaList) {
        const int typeWidth = (std::max)(86, (right - LayoutMetrics::kPanelInset) * 12 / 100);
        const int dateWidth = (std::max)(130, (right - LayoutMetrics::kPanelInset) * 23 / 100);
        const int reservedRightPadding = GetSystemMetrics(SM_CXVSCROLL) + 12;
        const int nameWidth = (std::max)(220, right - LayoutMetrics::kPanelInset - typeWidth - dateWidth - reservedRightPadding);
        ListView_SetColumnWidth(ctx->youtubeMediaList, 0, typeWidth);
        ListView_SetColumnWidth(ctx->youtubeMediaList, 1, nameWidth);
        ListView_SetColumnWidth(ctx->youtubeMediaList, 2, dateWidth);
    }

    const int controlsTop = listTop + listHeight + 12;
    const int rightColumnWidth = (std::max)(220, (std::min)(320, (right - LayoutMetrics::kPanelInset) / 3));
    const int rightColumnX = right - rightColumnWidth;
    const int leftColumnWidth = (std::max)(240, rightColumnX - LayoutMetrics::kPanelInset - 12);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_TITLE_LABEL, LayoutMetrics::kPanelInset, controlsTop, 45, LayoutMetrics::kRowHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_TITLE_EDIT, LayoutMetrics::kPanelInset + 50, controlsTop, leftColumnWidth - 50, LayoutMetrics::kRowHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_PRIVACY_LABEL, rightColumnX, controlsTop, 70, LayoutMetrics::kRowHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_PRIVACY_COMBO, rightColumnX + 74, controlsTop, rightColumnWidth - 74, LayoutMetrics::kRowHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_UPLOAD_PROGRESS, LayoutMetrics::kPanelInset, controlsTop + 46, leftColumnWidth, 20);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_UPLOAD_BUTTON, rightColumnX, controlsTop + 44, rightColumnWidth, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->youtubePanel, IDC_YOUTUBE_UPLOAD_STATUS, LayoutMetrics::kPanelInset, controlsTop + 70, leftColumnWidth, LayoutMetrics::kRowHeight);
}

void LayoutChatPrivacyPanel(AppContext* ctx, int panelWidth, int panelHeight)
{
    if (!ctx || !ctx->chatPrivacyPanel) {
        return;
    }

    const int right = panelWidth - LayoutMetrics::kPanelInset;
    int y = LayoutMetrics::kPanelInset;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_ENABLED_CHECK, LayoutMetrics::kPanelInset, y, 240, LayoutMetrics::kRowHeight);
    y += 32;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_BLANK_RADIO, LayoutMetrics::kPanelInset + 104, y, 104, LayoutMetrics::kRowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_CUSTOM_RADIO, LayoutMetrics::kPanelInset + 214, y, 104, LayoutMetrics::kRowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_IMPORT_BUTTON, LayoutMetrics::kPanelInset + 324, y, 78, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_OPEN_FOLDER_BUTTON, LayoutMetrics::kPanelInset + 408, y, 108, LayoutMetrics::kButtonHeight);
    y += 36;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_LIBRARY_LABEL, LayoutMetrics::kPanelInset, y, 96, LayoutMetrics::kRowHeight);
    const int comboWidth = (std::max)(160, right - (LayoutMetrics::kPanelInset + 114));
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_COMBO, LayoutMetrics::kPanelInset + 114, y, comboWidth, 180);
    y += 34;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_WIDTH_LABEL, LayoutMetrics::kPanelInset, y, 110, LayoutMetrics::kRowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_WIDTH_EDIT, LayoutMetrics::kPanelInset + 114, y, 90, LayoutMetrics::kRowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_HEIGHT_LABEL, LayoutMetrics::kPanelInset + 218, y, 110, LayoutMetrics::kRowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_HEIGHT_EDIT, LayoutMetrics::kPanelInset + 328, y, 90, LayoutMetrics::kRowHeight);
    y += 34;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_ANCHOR_LABEL, LayoutMetrics::kPanelInset, y, 110, LayoutMetrics::kRowHeight);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_ANCHOR_COMBO, LayoutMetrics::kPanelInset + 114, y, 180, 140);
    y += 38;

    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_PREVIEW_LABEL, LayoutMetrics::kPanelInset, y, right - LayoutMetrics::kPanelInset, LayoutMetrics::kRowHeight);
    y += 28;

    const int previewHeight = (std::max)(220, panelHeight - y - 12);
    MoveControl(ctx->chatPrivacyPanel, IDC_CHAT_PREVIEW, LayoutMetrics::kPanelInset, y, right - LayoutMetrics::kPanelInset, previewHeight);
}

void LayoutAboutPanel(AppContext* ctx, int panelWidth, int panelHeight)
{
    if (!ctx || !ctx->aboutPanel) {
        return;
    }
    const int rightButtonX = panelWidth - 170;
    const int valueWidth = (std::max)(180, rightButtonX - 150 - 14);
    constexpr int updatesRowBottom = 210 + LayoutMetrics::kRowHeight;
    const int flavorY = updatesRowBottom + (std::max)(0, panelHeight - updatesRowBottom - LayoutMetrics::kRowHeight) / 2;

    MoveControl(ctx->aboutPanel, IDC_ABOUT_TITLE_LABEL, LayoutMetrics::kPanelInset, 24, panelWidth - (2 * LayoutMetrics::kPanelInset), 34);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_BUILD_TEXT, LayoutMetrics::kPanelInset, 58, panelWidth - (2 * LayoutMetrics::kPanelInset), LayoutMetrics::kRowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_WEBSITE_LABEL, LayoutMetrics::kPanelInset, 96, 120, LayoutMetrics::kRowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_WEBSITE_TEXT, 150, 96, valueWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_WEBSITE_BUTTON, rightButtonX, 94, 150, LayoutMetrics::kButtonHeight);

    MoveControl(ctx->aboutPanel, IDC_ABOUT_EMAIL_LABEL, LayoutMetrics::kPanelInset, 134, 120, LayoutMetrics::kRowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_EMAIL_TEXT, 150, 134, valueWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_EMAIL_BUTTON, rightButtonX, 132, 150, LayoutMetrics::kButtonHeight);

    MoveControl(ctx->aboutPanel, IDC_ABOUT_DISCORD_LABEL, LayoutMetrics::kPanelInset, 172, 120, LayoutMetrics::kRowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_DISCORD_TEXT, 150, 172, valueWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_DISCORD_BUTTON, rightButtonX, 170, 150, LayoutMetrics::kButtonHeight);

    MoveControl(ctx->aboutPanel, IDC_ABOUT_UPDATE_LABEL, LayoutMetrics::kPanelInset, 210, 120, LayoutMetrics::kRowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_UPDATE_TEXT, 150, 210, valueWidth, LayoutMetrics::kRowHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_CHECK_UPDATES_BUTTON, rightButtonX, 208, 150, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->aboutPanel, IDC_ABOUT_FLAVOR_TEXT, LayoutMetrics::kPanelInset, flavorY, panelWidth - (2 * LayoutMetrics::kPanelInset), LayoutMetrics::kRowHeight);
}

void LayoutClipsPanel(AppContext* ctx, int panelWidth, int panelHeight)
{
    if (!ctx || !ctx->clipsPanel) {
        return;
    }

    const int right = panelWidth - LayoutMetrics::kPanelInset;

    MoveControl(ctx->clipsPanel, IDC_CLIPS_SOURCE_LABEL, LayoutMetrics::kPanelInset, LayoutMetrics::kPanelInset, 110, LayoutMetrics::kRowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_SOURCE_COMBO, LayoutMetrics::kPanelInset + 114, LayoutMetrics::kPanelInset, (std::max)(260, right - LayoutMetrics::kPanelInset - 230), 240);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_REFRESH, right - 90, LayoutMetrics::kPanelInset - 1, 90, LayoutMetrics::kButtonHeight);

    const int actionsTop = panelHeight - LayoutMetrics::kPanelInset - LayoutMetrics::kButtonHeight + 1;
    const int clippingTop = actionsTop - 34;
    const int volumeTop = clippingTop - 34;
    const int playbackTop = volumeTop - 34;
    constexpr int videoTop = LayoutMetrics::kPanelInset + 36;
    const int videoBottom = playbackTop - 10;
    const int videoHeight = (std::max)(120, videoBottom - videoTop);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_VIDEO_SURFACE, LayoutMetrics::kPanelInset, videoTop, right - LayoutMetrics::kPanelInset, videoHeight);

    MoveControl(ctx->clipsPanel, IDC_CLIPS_PLAY_PAUSE, LayoutMetrics::kPanelInset, playbackTop, 90, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_TIMELINE, LayoutMetrics::kPanelInset + 96, playbackTop + 1, (std::max)(140, right - LayoutMetrics::kPanelInset - 330), LayoutMetrics::kRowHeight + 2);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_POSITION_TEXT, right - 228, playbackTop + 2, 228, LayoutMetrics::kRowHeight);

    MoveControl(ctx->clipsPanel, IDC_CLIPS_VOLUME_LABEL, LayoutMetrics::kPanelInset, volumeTop, 60, LayoutMetrics::kRowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_VOLUME_SLIDER, LayoutMetrics::kPanelInset + 62, volumeTop, 170, LayoutMetrics::kRowHeight);

    MoveControl(ctx->clipsPanel, IDC_CLIPS_START_LABEL, LayoutMetrics::kPanelInset, clippingTop, 50, LayoutMetrics::kRowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_START_EDIT, LayoutMetrics::kPanelInset + 54, clippingTop, 74, LayoutMetrics::kRowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_SET_START, LayoutMetrics::kPanelInset + 136, clippingTop - 1, 90, LayoutMetrics::kButtonHeight);

    MoveControl(ctx->clipsPanel, IDC_CLIPS_END_LABEL, LayoutMetrics::kPanelInset + 240, clippingTop, 40, LayoutMetrics::kRowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_END_EDIT, LayoutMetrics::kPanelInset + 284, clippingTop, 74, LayoutMetrics::kRowHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_SET_END, LayoutMetrics::kPanelInset + 366, clippingTop - 1, 90, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_EXPORT, LayoutMetrics::kPanelInset, actionsTop - 1, 150, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_EXPORT_PRECISE, LayoutMetrics::kPanelInset + 160, actionsTop - 1, 170, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_OPEN_FOLDER, LayoutMetrics::kPanelInset + 340, actionsTop - 1, 110, LayoutMetrics::kButtonHeight);
    MoveControl(ctx->clipsPanel, IDC_CLIPS_FFMPEG_WARNING, LayoutMetrics::kPanelInset + 460, actionsTop, (std::max)(0, right - LayoutMetrics::kPanelInset - 460), LayoutMetrics::kButtonHeight);
}

void LayoutKeybindsPanel(AppContext* ctx, int panelWidth, int panelHeight)
{
    if (!ctx || !ctx->keybindsPanel) {
        return;
    }
    MoveControl(ctx->keybindsPanel, IDC_KEYBINDS_INFO, LayoutMetrics::kPanelInset, LayoutMetrics::kPanelInset, panelWidth - (2 * LayoutMetrics::kPanelInset), LayoutMetrics::kRowHeight);
    MoveControl(ctx->keybindsPanel, IDC_KEYBINDS_AUTOSAVE_HINT, LayoutMetrics::kPanelInset, panelHeight - 44, panelWidth - (2 * LayoutMetrics::kPanelInset), LayoutMetrics::kRowHeight);
    for (int index = 0; index < 3; ++index) {
        const int y = 64 + index * 44;
        MoveControl(ctx->keybindsPanel, IDC_KEYBINDS_CREATE_CLIP_LABEL + index, LayoutMetrics::kPanelInset, y, 160, LayoutMetrics::kRowHeight);
        MoveControl(ctx->keybindsPanel, IDC_KEYBINDS_CREATE_CLIP_VALUE + index, LayoutMetrics::kPanelInset + 170, y, 180, LayoutMetrics::kRowHeight);
        MoveControl(ctx->keybindsPanel, IDC_KEYBINDS_CREATE_CLIP_REBIND + index, LayoutMetrics::kPanelInset + 370, y, 100, LayoutMetrics::kRowHeight + 2);
        MoveControl(ctx->keybindsPanel, IDC_KEYBINDS_CREATE_CLIP_UNBIND + index, LayoutMetrics::kPanelInset + 480, y, 100, LayoutMetrics::kRowHeight + 2);
        MoveControl(ctx->keybindsPanel, IDC_KEYBINDS_CREATE_CLIP_RESET + index, LayoutMetrics::kPanelInset + 590, y, 100, LayoutMetrics::kRowHeight + 2);
    }
}

} // namespace

void LayoutMainUi(AppContext* ctx, int clientWidth, int clientHeight)
{
    if (!ctx || !ctx->statusPanel || !ctx->recorderPanel || !ctx->chatPrivacyPanel || !ctx->recordingsPanel || !ctx->youtubePanel || !ctx->clipsPanel || !ctx->keybindsPanel || !ctx->aboutPanel) {
        return;
    }

    constexpr int outer = 12;
    const int navWidth = (std::max)(120, (std::min)(160, clientWidth / 6));
    constexpr int navY = 20;
    constexpr int navHeight = 34;
    const int panelX = outer + navWidth + 16;
    constexpr int panelY = 14;
    const int panelWidth = (std::max)(320, clientWidth - panelX - outer);
    const int panelHeight = (std::max)(240, clientHeight - panelY - outer);

    MoveWindow(ctx->statusTabButton, outer, navY, navWidth, navHeight, TRUE);
    MoveWindow(ctx->configurationTabButton, outer, navY + 40, navWidth, navHeight, TRUE);
    MoveWindow(ctx->chatPrivacyTabButton, outer, navY + 80, navWidth, navHeight, TRUE);
    MoveWindow(ctx->recordingsTabButton, outer, navY + 120, navWidth, navHeight, TRUE);
    MoveWindow(ctx->clipsTabButton, outer, navY + 160, navWidth, navHeight, TRUE);
    MoveWindow(ctx->youtubeTabButton, outer, navY + 200, navWidth, navHeight, TRUE);
    MoveWindow(ctx->keybindsTabButton, outer, navY + 240, navWidth, navHeight, TRUE);
    MoveWindow(ctx->aboutTabButton, outer, navY + 280, navWidth, navHeight, TRUE);

    MoveWindow(ctx->statusPanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->recorderPanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->chatPrivacyPanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->recordingsPanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->youtubePanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->clipsPanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->keybindsPanel, panelX, panelY, panelWidth, panelHeight, TRUE);
    MoveWindow(ctx->aboutPanel, panelX, panelY, panelWidth, panelHeight, TRUE);

    LayoutStatusPanel(ctx, panelWidth, panelHeight);
    LayoutConfigurationPanel(ctx, panelWidth, panelHeight);
    LayoutChatPrivacyPanel(ctx, panelWidth, panelHeight);
    LayoutRecordingsPanel(ctx, panelWidth, panelHeight);
    LayoutYouTubePanel(ctx, panelWidth, panelHeight);
    LayoutClipsPanel(ctx, panelWidth, panelHeight);
    LayoutKeybindsPanel(ctx, panelWidth, panelHeight);
    LayoutAboutPanel(ctx, panelWidth, panelHeight);

    InvalidateRect(ctx->mainWindow, nullptr, TRUE);
}
