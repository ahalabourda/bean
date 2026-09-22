#include "app/AppTheme.h"

#include "app/AppClips.h"
#include "app/AppDraw.h"
#include "app/AppStatusLog.h"
#include "app/AppUtilities.h"
#include "bean_version.h"
#include "util/Strings.h"

#include <dwmapi.h>
#include <string>

using bean::util::ToWide;

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

