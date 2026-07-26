#include "app/AppPrerequisites.h"

bool PrerequisiteRowIsHealthy(const AppContext* ctx, const PrerequisiteRow& row)
{
    if (!ctx) {
        return false;
    }
    bool healthy = false;
    switch (row.iconId) {
    case IDC_WOW_WINDOW_ICON:
        healthy = ctx->wowWindowDetected;
        break;
    case IDC_OBS_INSTALL_ICON:
        healthy = ctx->obsInstallDetected;
        break;
    case IDC_FFMPEG_ICON:
        healthy = ctx->ffmpegDetected;
        break;
    case IDC_WARCRAFT_RECORDER_ICON:
        healthy = ctx->warcraftRecorderDetected;
        break;
    case IDC_ADVANCED_LOGGING_ICON:
        healthy = ctx->advancedCombatLoggingEnabled;
        break;
    default:
        break;
    }
    return row.invertHealthy ? !healthy : healthy;
}

HWND* PrerequisiteIconHwndSlot(AppContext* ctx, int iconId)
{
    if (!ctx) {
        return nullptr;
    }
    switch (iconId) {
    case IDC_WOW_WINDOW_ICON:
        return &ctx->wowWindowIcon;
    case IDC_OBS_INSTALL_ICON:
        return &ctx->obsInstallIcon;
    case IDC_FFMPEG_ICON:
        return &ctx->ffmpegIcon;
    case IDC_WARCRAFT_RECORDER_ICON:
        return &ctx->warcraftRecorderIcon;
    case IDC_ADVANCED_LOGGING_ICON:
        return &ctx->advancedLoggingIcon;
    default:
        return nullptr;
    }
}

HWND* PrerequisiteTextHwndSlot(AppContext* ctx, int textId)
{
    if (!ctx) {
        return nullptr;
    }
    switch (textId) {
    case IDC_WOW_WINDOW_TEXT:
        return &ctx->wowWindowText;
    case IDC_OBS_INSTALL_TEXT:
        return &ctx->obsInstallText;
    case IDC_FFMPEG_TEXT:
        return &ctx->ffmpegText;
    case IDC_WARCRAFT_RECORDER_TEXT:
        return &ctx->warcraftRecorderText;
    case IDC_ADVANCED_LOGGING_TEXT:
        return &ctx->advancedLoggingText;
    default:
        return nullptr;
    }
}
