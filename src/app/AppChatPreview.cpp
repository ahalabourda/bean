#include "app/AppChatPreview.h"

#include "app/AppChatPrivacy.h"
#include "app/AppPlatformUi.h"
#include "app/AppUtilities.h"

#include <algorithm>
#include <chrono>
#include <gdiplus.h>
#include <memory>
#include <filesystem>

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

