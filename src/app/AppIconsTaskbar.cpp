#include "app/AppIconsTaskbar.h"

#include "app/AppUtilities.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {

std::filesystem::path IconPathFromExe(const wchar_t* fileName)
{
    return GetExecutableDirectory() / "assets" / "icons" / fileName;
}

HICON TryLoadIconFile(const std::filesystem::path& iconPath, int iconSize)
{
    if (iconPath.empty()) {
        return nullptr;
    }
    return reinterpret_cast<HICON>(LoadImageW(
        nullptr,
        iconPath.c_str(),
        IMAGE_ICON,
        iconSize,
        iconSize,
        LR_LOADFROMFILE | LR_CREATEDIBSECTION));
}

void DestroyIconSet(AppIconSet& iconSet)
{
    if (iconSet.smallIcon) {
        DestroyIcon(iconSet.smallIcon);
        iconSet.smallIcon = nullptr;
    }
    if (iconSet.largeIcon) {
        DestroyIcon(iconSet.largeIcon);
        iconSet.largeIcon = nullptr;
    }
}

bool IconSetLoaded(const AppIconSet& iconSet)
{
    return iconSet.smallIcon && iconSet.largeIcon;
}

HICON LoadBestIdleIconForSize(int iconSize, const std::array<const wchar_t*, 5>& candidates)
{
    for (const wchar_t* fileName : candidates) {
        HICON icon = TryLoadIconFile(IconPathFromExe(fileName), iconSize);
        if (icon) {
            return icon;
        }
    }
    return nullptr;
}

AppIconSet LoadIdleIconSetFromAvailableFiles()
{
    AppIconSet iconSet;
    const int smallSize = GetSystemMetrics(SM_CXSMICON);
    const int largeSize = GetSystemMetrics(SM_CXICON);

    iconSet.smallIcon = LoadBestIdleIconForSize(smallSize, {kIconFile16, kIconFile32, kIconFile48, kIconFile256});
    iconSet.largeIcon = LoadBestIdleIconForSize(largeSize, {kIconFile48, kIconFile32, kIconFile256, kIconFile16});
    if (!IconSetLoaded(iconSet)) {
        DestroyIconSet(iconSet);
    }
    return iconSet;
}

AppIconSet LoadEmbeddedAppIconSet()
{
    AppIconSet iconSet;
#ifdef BEAN_HAS_EMBEDDED_ICON
    HINSTANCE moduleInstance = GetModuleHandleW(nullptr);
    if (!moduleInstance) {
        return iconSet;
    }

    iconSet.smallIcon = reinterpret_cast<HICON>(LoadImageW(
        moduleInstance,
        MAKEINTRESOURCEW(kEmbeddedAppIconResourceId),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    iconSet.largeIcon = reinterpret_cast<HICON>(LoadImageW(
        moduleInstance,
        MAKEINTRESOURCEW(kEmbeddedAppIconResourceId),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR));
    if (!IconSetLoaded(iconSet)) {
        DestroyIconSet(iconSet);
    }
#endif
    return iconSet;
}

AppIconSet FallbackSystemIconSet()
{
    AppIconSet iconSet = LoadEmbeddedAppIconSet();
    if (IconSetLoaded(iconSet)) {
        return iconSet;
    }

    iconSet.smallIcon = CopyIcon(LoadIcon(nullptr, IDI_APPLICATION));
    iconSet.largeIcon = CopyIcon(LoadIcon(nullptr, IDI_APPLICATION));
    return iconSet;
}

void EnsureFallbackIconSet(AppIconSet& iconSet)
{
    if (IconSetLoaded(iconSet)) {
        return;
    }
    DestroyIconSet(iconSet);
    iconSet = FallbackSystemIconSet();
}

void DestroyTaskbarOverlayIcons(TaskbarOverlayIconSet& icons)
{
    if (icons.readyIcon) {
        DestroyIcon(icons.readyIcon);
        icons.readyIcon = nullptr;
    }
    if (icons.recordingIcon) {
        DestroyIcon(icons.recordingIcon);
        icons.recordingIcon = nullptr;
    }
    if (icons.warningIcon) {
        DestroyIcon(icons.warningIcon);
        icons.warningIcon = nullptr;
    }
}

HICON CreateTaskbarOverlayDotIcon(COLORREF color)
{
    const int iconSize = (std::max)(16, GetSystemMetrics(SM_CXSMICON));
    const int radius = (std::max)(5, iconSize / 2 - 2);
    const int center = iconSize / 2;
    const float outerRadius = static_cast<float>(radius);
    const float borderThickness = (iconSize >= 20) ? 1.5f : 1.0f;
    const float fillRadius = (std::max)(0.0f, outerRadius - borderThickness);
    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        return nullptr;
    }

    BITMAPV5HEADER bitmapHeader{};
    bitmapHeader.bV5Size = sizeof(bitmapHeader);
    bitmapHeader.bV5Width = iconSize;
    bitmapHeader.bV5Height = -iconSize;
    bitmapHeader.bV5Planes = 1;
    bitmapHeader.bV5BitCount = 32;
    bitmapHeader.bV5Compression = BI_BITFIELDS;
    bitmapHeader.bV5RedMask = 0x00FF0000;
    bitmapHeader.bV5GreenMask = 0x0000FF00;
    bitmapHeader.bV5BlueMask = 0x000000FF;
    bitmapHeader.bV5AlphaMask = 0xFF000000;

    void* pixels = nullptr;
    HBITMAP colorBitmap = CreateDIBSection(
        screenDc,
        reinterpret_cast<BITMAPINFO*>(&bitmapHeader),
        DIB_RGB_COLORS,
        &pixels,
        nullptr,
        0);
    ReleaseDC(nullptr, screenDc);

    const int maskStride = ((iconSize + 31) / 32) * 4;
    std::vector<unsigned char> maskBits(static_cast<size_t>(maskStride * iconSize), 0x00);
    HBITMAP maskBitmap = CreateBitmap(iconSize, iconSize, 1, 1, maskBits.data());
    if (!colorBitmap || !maskBitmap || !pixels) {
        if (colorBitmap) {
            DeleteObject(colorBitmap);
        }
        if (maskBitmap) {
            DeleteObject(maskBitmap);
        }
        return nullptr;
    }

    auto* argbPixels = static_cast<unsigned int*>(pixels);
    const float red = static_cast<float>(GetRValue(color));
    const float green = static_cast<float>(GetGValue(color));
    const float blue = static_cast<float>(GetBValue(color));

    for (int y = 0; y < iconSize; ++y) {
        for (int x = 0; x < iconSize; ++x) {
            const float dx = static_cast<float>(x) - static_cast<float>(center) + 0.5f;
            const float dy = static_cast<float>(y) - static_cast<float>(center) + 0.5f;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const size_t pixelIndex = static_cast<size_t>(y * iconSize + x);

            const float outerCoverage = (std::clamp)(outerRadius + 0.5f - distance, 0.0f, 1.0f);
            const float fillCoverage = (std::clamp)(fillRadius + 0.5f - distance, 0.0f, 1.0f);
            const float borderCoverage = (std::max)(0.0f, outerCoverage - fillCoverage);

            const float finalAlpha = fillCoverage + borderCoverage * (1.0f - fillCoverage);
            if (finalAlpha <= 0.0f) {
                argbPixels[pixelIndex] = 0x00000000u;
                continue;
            }

            const float mixedRed = (red * fillCoverage) / finalAlpha;
            const float mixedGreen = (green * fillCoverage) / finalAlpha;
            const float mixedBlue = (blue * fillCoverage) / finalAlpha;

            const unsigned int alpha8 = static_cast<unsigned int>((std::clamp)(finalAlpha, 0.0f, 1.0f) * 255.0f + 0.5f);
            const unsigned int red8 = static_cast<unsigned int>((std::clamp)(mixedRed, 0.0f, 255.0f) + 0.5f);
            const unsigned int green8 = static_cast<unsigned int>((std::clamp)(mixedGreen, 0.0f, 255.0f) + 0.5f);
            const unsigned int blue8 = static_cast<unsigned int>((std::clamp)(mixedBlue, 0.0f, 255.0f) + 0.5f);
            argbPixels[pixelIndex] = (alpha8 << 24) | (red8 << 16) | (green8 << 8) | blue8;
        }
    }

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;
    HICON icon = CreateIconIndirect(&iconInfo);

    DeleteObject(colorBitmap);
    DeleteObject(maskBitmap);
    return icon;
}

TaskbarOverlayState ResolveTaskbarOverlayState(const AppContext* ctx)
{
    if (!ctx || !ctx->isMonitoring) {
        return TaskbarOverlayState::Idle;
    }
    if (ctx->isRecording) {
        return TaskbarOverlayState::Recording;
    }
    const bool prerequisitesHealthy = ctx->wowWindowDetected && ctx->obsInstallDetected && ctx->ffmpegDetected;
    return prerequisitesHealthy ? TaskbarOverlayState::MonitoringReady : TaskbarOverlayState::Warning;
}

} // namespace

void InitializeAppIcons(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    ctx->idleIcon = LoadIdleIconSetFromAvailableFiles();
    EnsureFallbackIconSet(ctx->idleIcon);
}

void DestroyAppIcons(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    DestroyIconSet(ctx->idleIcon);
}

void InitializeTaskbarOverlay(AppContext* ctx)
{
    if (!ctx || !ctx->mainWindow || ctx->taskbarList) {
        return;
    }

    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ctx->taskbarList))) || !ctx->taskbarList) {
        return;
    }
    if (FAILED(ctx->taskbarList->HrInit())) {
        ctx->taskbarList->Release();
        ctx->taskbarList = nullptr;
        return;
    }
    ctx->taskbarOverlayIcons.readyIcon = CreateTaskbarOverlayDotIcon(RGB(80, 214, 142));
    ctx->taskbarOverlayIcons.recordingIcon = CreateTaskbarOverlayDotIcon(RGB(241, 100, 125));
    ctx->taskbarOverlayIcons.warningIcon = CreateTaskbarOverlayDotIcon(RGB(255, 192, 68));
}

void ApplyTaskbarOverlayState(AppContext* ctx, bool forceUpdate)
{
    if (!ctx || !ctx->mainWindow || !ctx->taskbarList) {
        return;
    }
    const TaskbarOverlayState nextState = ResolveTaskbarOverlayState(ctx);
    if (!forceUpdate && nextState == ctx->activeTaskbarOverlayState) {
        return;
    }

    HICON overlayIcon = nullptr;
    const wchar_t* overlayDescription = L"";
    if (nextState == TaskbarOverlayState::MonitoringReady) {
        overlayIcon = ctx->taskbarOverlayIcons.readyIcon;
        overlayDescription = L"Monitoring ready";
    } else if (nextState == TaskbarOverlayState::Recording) {
        overlayIcon = ctx->taskbarOverlayIcons.recordingIcon;
        overlayDescription = L"Recording";
    } else if (nextState == TaskbarOverlayState::Warning) {
        overlayIcon = ctx->taskbarOverlayIcons.warningIcon;
        overlayDescription = L"Monitoring warning";
    }

    ctx->taskbarList->SetOverlayIcon(ctx->mainWindow, overlayIcon, overlayDescription);
    ctx->activeTaskbarOverlayState = nextState;
}

void ShutdownTaskbarOverlay(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->taskbarList && ctx->mainWindow) {
        ctx->taskbarList->SetOverlayIcon(ctx->mainWindow, nullptr, L"");
    }
    DestroyTaskbarOverlayIcons(ctx->taskbarOverlayIcons);
    if (ctx->taskbarList) {
        ctx->taskbarList->Release();
        ctx->taskbarList = nullptr;
    }
    ctx->activeTaskbarOverlayState = TaskbarOverlayState::Idle;
}
