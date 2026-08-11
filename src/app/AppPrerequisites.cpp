#include "app/AppPrerequisites.h"

bool PrerequisiteRowIsHealthy(const AppContext* ctx, const PrerequisiteRow& row)
{
    if (!ctx) {
        return false;
    }
    const bool healthy = row.healthMember ? (ctx->*(row.healthMember)) : false;
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
