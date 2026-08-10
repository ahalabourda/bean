#include "app/AppDraw.h"

#include "app/AppPrerequisites.h"
#include "app/AppUtilities.h"
#include "obs/IRecorderEngine.h"

#include <commctrl.h>
#include <gdiplus.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <utility>

#pragma comment(lib, "Gdiplus.lib")

VisualTheme gTheme;

namespace {

HWND gHoveredStyledButton = nullptr;
HWND gHoveredModernToggle = nullptr;
HWND gHoveredModernCombo = nullptr;
HWND gHoveredHelpIcon = nullptr;
ULONG_PTR gAlertGdiplusToken = 0;
constexpr UINT kRefreshModernComboMessage = WM_APP + 120;

bool EnsureAlertGdiplus()
{
    if (gAlertGdiplusToken != 0) {
        return true;
    }
    Gdiplus::GdiplusStartupInput startupInput;
    return Gdiplus::GdiplusStartup(&gAlertGdiplusToken, &startupInput, nullptr) == Gdiplus::Ok;
}

bool IsStatusTabValid(const AppContext* ctx)
{
    return ctx
        && ctx->wowWindowDetected
        && ctx->obsInstallDetected
        && ctx->ffmpegDetected
        && !ctx->warcraftRecorderDetected
        && ctx->advancedCombatLoggingEnabled;
}

bool IsConfigurationTabValid(const AppContext* ctx)
{
    return ctx && (ctx->outputAvailable || ctx->outputFolderWillBeCreatedOnRecordStart) && ctx->wowLogAvailable;
}

std::string BuildSpecIconKey(const std::string& className, const std::string& specName)
{
    std::string classPart;
    classPart.reserve(className.size());
    for (unsigned char ch : className) {
        if (std::isalnum(ch)) {
            classPart.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    std::string specPart;
    specPart.reserve(specName.size());
    for (unsigned char ch : specName) {
        if (std::isalnum(ch)) {
            specPart.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    if (classPart.empty() || specPart.empty()) {
        return {};
    }
    return classPart + "-" + specPart;
}

BOOL CALLBACK ApplyUiFontEnumProc(HWND child, LPARAM)
{
    if (gTheme.uiFont) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.uiFont), TRUE);
    }
    return TRUE;
}

BOOL CALLBACK ApplyRecordingsFontEnumProc(HWND child, LPARAM)
{
    if (gTheme.recordingsFont) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.recordingsFont), TRUE);
    }
    return TRUE;
}

LRESULT CALLBACK StyledButtonHoverSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (message) {
    case WM_MOUSEMOVE: {
        if (gHoveredStyledButton != hwnd) {
            HWND previous = gHoveredStyledButton;
            gHoveredStyledButton = hwnd;
            if (previous && IsWindow(previous)) {
                InvalidateRect(previous, nullptr, TRUE);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);
        break;
    }
    case WM_MOUSELEAVE:
        if (gHoveredStyledButton == hwnd) {
            gHoveredStyledButton = nullptr;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        break;
    case WM_NCDESTROY:
        if (gHoveredStyledButton == hwnd) {
            gHoveredStyledButton = nullptr;
        }
        RemoveWindowSubclass(hwnd, StyledButtonHoverSubclassProc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void EnableOwnerDrawButton(HWND parent, int controlId)
{
    if (!parent) {
        return;
    }
    HWND button = GetDlgItem(parent, controlId);
    if (!button) {
        return;
    }
    LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
    if ((style & BS_OWNERDRAW) == 0) {
        SetWindowLongPtrW(button, GWL_STYLE, style | BS_OWNERDRAW);
        SetWindowPos(button, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetWindowSubclass(button, StyledButtonHoverSubclassProc, 1, 0);
}

COLORREF LerpColor(COLORREF a, COLORREF b, float t)
{
    t = (std::clamp)(t, 0.0f, 1.0f);
    const auto lerpChannel = [t](int from, int to) {
        return from + static_cast<int>((to - from) * t + 0.5f);
    };
    return RGB(
        lerpChannel(GetRValue(a), GetRValue(b)),
        lerpChannel(GetGValue(a), GetGValue(b)),
        lerpChannel(GetBValue(a), GetBValue(b)));
}

// Clears the square behind custom buttons using the same vertical gradient as the
// parent (window or panel), so the button background matches its surroundings.
void FillStyledButtonParentBackground(HDC hdc, HWND button, const RECT& buttonRc, HWND mainWindow)
{
    if (!hdc || !button) {
        return;
    }

    HWND parent = GetParent(button);
    RECT parentRc{};
    if (!parent || !GetClientRect(parent, &parentRc)) {
        if (gTheme.panelSolidBrush) {
            FillRect(hdc, &buttonRc, gTheme.panelSolidBrush);
        }
        return;
    }

    const int parentHeight = parentRc.bottom - parentRc.top;
    if (parentHeight <= 0) {
        if (gTheme.panelSolidBrush) {
            FillRect(hdc, &buttonRc, gTheme.panelSolidBrush);
        }
        return;
    }

    POINT topLeft{buttonRc.left, buttonRc.top};
    MapWindowPoints(button, parent, &topLeft, 1);

    const bool isMainWindow = (mainWindow != nullptr && parent == mainWindow);
    const COLORREF topColor = isMainWindow ? kColorWindowTop : kColorPanelTop;
    const COLORREF bottomColor = isMainWindow ? kColorWindowBottom : kColorPanelBottom;

    const int savedDc = SaveDC(hdc);
    IntersectClipRect(hdc, buttonRc.left, buttonRc.top, buttonRc.right, buttonRc.bottom);

    const LONG gradientTop = buttonRc.top - topLeft.y;
    const LONG gradientBottom = gradientTop + parentHeight;
    TRIVERTEX vertices[2] = {
        {buttonRc.left,
         gradientTop,
         static_cast<COLOR16>(GetRValue(topColor) << 8),
         static_cast<COLOR16>(GetGValue(topColor) << 8),
         static_cast<COLOR16>(GetBValue(topColor) << 8),
         0xFF00},
        {buttonRc.right,
         gradientBottom,
         static_cast<COLOR16>(GetRValue(bottomColor) << 8),
         static_cast<COLOR16>(GetGValue(bottomColor) << 8),
         static_cast<COLOR16>(GetBValue(bottomColor) << 8),
         0xFF00},
    };
    GRADIENT_RECT gradientRect{0, 1};
    const BOOL filled = GradientFill(hdc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V);
    if (!filled) {
        const float t = static_cast<float>(topLeft.y + (buttonRc.bottom - buttonRc.top) / 2) / static_cast<float>(parentHeight);
        HBRUSH brush = CreateSolidBrush(LerpColor(topColor, bottomColor, t));
        if (brush) {
            FillRect(hdc, &buttonRc, brush);
            DeleteObject(brush);
        } else if (gTheme.panelSolidBrush) {
            FillRect(hdc, &buttonRc, gTheme.panelSolidBrush);
        }
    }

    RestoreDC(hdc, savedDc);
}

void DrawModernRadioGlyph(
    HDC dc,
    const RECT& glyph,
    COLORREF borderColor,
    COLORREF fillColor,
    bool checked)
{
    const int glyphWidth = glyph.right - glyph.left;
    const int glyphHeight = glyph.bottom - glyph.top;
    if (glyphWidth <= 0 || glyphHeight <= 0) {
        return;
    }

    if (EnsureAlertGdiplus()) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        if (graphics.GetLastStatus() == Gdiplus::Ok) {
            const auto toGdiPlusColor = [](COLORREF color) {
                return Gdiplus::Color(
                    255,
                    GetRValue(color),
                    GetGValue(color),
                    GetBValue(color));
            };
            const Gdiplus::RectF outerRect(
                static_cast<Gdiplus::REAL>(glyph.left) + 0.5f,
                static_cast<Gdiplus::REAL>(glyph.top) + 0.5f,
                static_cast<Gdiplus::REAL>(glyphWidth - 1),
                static_cast<Gdiplus::REAL>(glyphHeight - 1));
            const Gdiplus::Color outerColor = toGdiPlusColor(fillColor);
            const Gdiplus::Color ringColor = toGdiPlusColor(borderColor);
            Gdiplus::SolidBrush outerBrush(outerColor);
            Gdiplus::Pen ringPen(ringColor, 1.0f);
            graphics.FillEllipse(&outerBrush, outerRect);
            graphics.DrawEllipse(&ringPen, outerRect);

            if (checked) {
                constexpr Gdiplus::REAL dotDiameter = 6.0f;
                const Gdiplus::RectF dotRect(
                    static_cast<Gdiplus::REAL>(glyph.left) + (glyphWidth - dotDiameter) / 2.0f,
                    static_cast<Gdiplus::REAL>(glyph.top) + (glyphHeight - dotDiameter) / 2.0f,
                    dotDiameter,
                    dotDiameter);
                Gdiplus::SolidBrush dotBrush(toGdiPlusColor(kColorButtonText));
                graphics.FillEllipse(&dotBrush, dotRect);
            }
            return;
        }
    }

    HPEN borderPen = CreatePen(PS_SOLID, 1, borderColor);
    HBRUSH fillBrush = CreateSolidBrush(fillColor);
    HGDIOBJ oldPen = borderPen ? SelectObject(dc, borderPen) : nullptr;
    HGDIOBJ oldBrush = fillBrush ? SelectObject(dc, fillBrush) : nullptr;
    Ellipse(dc, glyph.left, glyph.top, glyph.right, glyph.bottom);
    if (oldBrush) {
        SelectObject(dc, oldBrush);
    }
    if (oldPen) {
        SelectObject(dc, oldPen);
    }
    if (fillBrush) {
        DeleteObject(fillBrush);
    }
    if (borderPen) {
        DeleteObject(borderPen);
    }
    if (checked) {
        const int dotSize = 6;
        const int dotCenterX = (glyph.left + glyph.right) / 2;
        const int dotCenterY = (glyph.top + glyph.bottom) / 2;
        HBRUSH dotBrush = CreateSolidBrush(kColorButtonText);
        if (dotBrush) {
            HGDIOBJ oldDotBrush = SelectObject(dc, dotBrush);
            HPEN dotPen = CreatePen(PS_SOLID, 1, kColorButtonText);
            HGDIOBJ oldDotPen = dotPen ? SelectObject(dc, dotPen) : nullptr;
            Ellipse(
                dc,
                dotCenterX - dotSize / 2,
                dotCenterY - dotSize / 2,
                dotCenterX + dotSize / 2,
                dotCenterY + dotSize / 2);
            if (oldDotPen) {
                SelectObject(dc, oldDotPen);
            }
            if (dotPen) {
                DeleteObject(dotPen);
            }
            SelectObject(dc, oldDotBrush);
            DeleteObject(dotBrush);
        }
    }
}

void DrawModernCheckboxCheckmark(HDC dc, const RECT& glyph, int centerY)
{
    if (EnsureAlertGdiplus()) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        if (graphics.GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Pen checkPen(
                Gdiplus::Color(
                    255,
                    GetRValue(kColorButtonText),
                    GetGValue(kColorButtonText),
                    GetBValue(kColorButtonText)),
                1.8f);
            checkPen.SetStartCap(Gdiplus::LineCapRound);
            checkPen.SetEndCap(Gdiplus::LineCapRound);
            checkPen.SetLineJoin(Gdiplus::LineJoinRound);
            const Gdiplus::PointF points[] = {
                {static_cast<Gdiplus::REAL>(glyph.left + 3), static_cast<Gdiplus::REAL>(centerY)},
                {static_cast<Gdiplus::REAL>(glyph.left + 6), static_cast<Gdiplus::REAL>(glyph.bottom - 3)},
                {static_cast<Gdiplus::REAL>(glyph.right - 3), static_cast<Gdiplus::REAL>(glyph.top + 3)},
            };
            graphics.DrawLines(&checkPen, points, 3);
            return;
        }
    }

    HPEN checkPen = CreatePen(PS_SOLID, 2, kColorButtonText);
    HGDIOBJ oldCheckPen = checkPen ? SelectObject(dc, checkPen) : nullptr;
    MoveToEx(dc, glyph.left + 3, centerY, nullptr);
    LineTo(dc, glyph.left + 6, glyph.bottom - 3);
    LineTo(dc, glyph.right - 3, glyph.top + 3);
    if (oldCheckPen) {
        SelectObject(dc, oldCheckPen);
    }
    if (checkPen) {
        DeleteObject(checkPen);
    }
}

void DrawModernToggleContent(HWND hwnd, HDC dc, const AppContext* ctx)
{
    if (!hwnd || !dc) {
        return;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillStyledButtonParentBackground(dc, hwnd, rc, ctx ? ctx->mainWindow : nullptr);

    const int controlId = GetDlgCtrlID(hwnd);
    const bool isRadio = controlId == IDC_AUDIO_SCOPE_CHECK
        || controlId == IDC_AUDIO_SCOPE_WOW_DISCORD_RADIO
        || controlId == IDC_AUDIO_SCOPE_ALL_RADIO
        || controlId == IDC_CHAT_BLOCKER_IMAGE_BLANK_RADIO
        || controlId == IDC_CHAT_BLOCKER_IMAGE_CUSTOM_RADIO;
    const bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool pressed = (SendMessageW(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool hovered = gHoveredModernToggle == hwnd;
    const bool focused = GetFocus() == hwnd;

    const COLORREF borderColor = !enabled
        ? kThemeColors.controlDisabledBorder
        : (focused ? kThemeColors.accentBright : (hovered ? kThemeColors.controlHoverBorder : kColorInputBorder));
    const COLORREF fillColor = !enabled
        ? kThemeColors.controlDisabledBackground
        : (checked ? (pressed ? kThemeColors.controlPressedBackground : kThemeColors.accent)
                   : (pressed ? kThemeColors.controlHoverBackground : kColorInputBg));
    const COLORREF textColor = !enabled ? kThemeColors.controlDisabledText : kColorTextPrimary;

    const int centerY = (rc.top + rc.bottom) / 2;
    const int glyphSize = isRadio ? 16 : 15;
    const int glyphLeft = 7;
    const int glyphTop = centerY - glyphSize / 2;
    RECT glyph{glyphLeft, glyphTop, glyphLeft + glyphSize, glyphTop + glyphSize};

    if (isRadio) {
        DrawModernRadioGlyph(dc, glyph, borderColor, fillColor, checked);
    } else {
        HPEN borderPen = CreatePen(PS_SOLID, focused ? 2 : 1, borderColor);
        HBRUSH fillBrush = CreateSolidBrush(fillColor);
        HGDIOBJ oldPen = borderPen ? SelectObject(dc, borderPen) : nullptr;
        HGDIOBJ oldBrush = fillBrush ? SelectObject(dc, fillBrush) : nullptr;
        Rectangle(dc, glyph.left, glyph.top, glyph.right, glyph.bottom);
        if (oldBrush) {
            SelectObject(dc, oldBrush);
        }
        if (oldPen) {
            SelectObject(dc, oldPen);
        }
        if (fillBrush) {
            DeleteObject(fillBrush);
        }
        if (borderPen) {
            DeleteObject(borderPen);
        }
        if (checked) {
            DrawModernCheckboxCheckmark(dc, glyph, centerY);
        }
    }

    wchar_t textBuffer[256] = {};
    GetWindowTextW(hwnd, textBuffer, static_cast<int>(std::size(textBuffer)));
    RECT textRect = rc;
    textRect.left = glyph.right + 7;
    textRect.right -= 4;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, textColor);
    HGDIOBJ oldFont = gTheme.uiFont ? SelectObject(dc, gTheme.uiFont) : nullptr;
    DrawTextW(dc, textBuffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }

}

void DrawModernToggle(HWND hwnd, HDC dc, const AppContext* ctx)
{
    if (!hwnd || !dc) {
        return;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    HDC bufferDc = CreateCompatibleDC(dc);
    HBITMAP bufferBitmap = bufferDc ? CreateCompatibleBitmap(dc, width, height) : nullptr;
    if (!bufferDc || !bufferBitmap) {
        if (bufferDc) {
            DeleteDC(bufferDc);
        }
        DrawModernToggleContent(hwnd, dc, ctx);
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(bufferDc, bufferBitmap);
    DrawModernToggleContent(hwnd, bufferDc, ctx);
    BitBlt(dc, 0, 0, width, height, bufferDc, 0, 0, SRCCOPY);
    if (oldBitmap) {
        SelectObject(bufferDc, oldBitmap);
    }
    DeleteObject(bufferBitmap);
    DeleteDC(bufferDc);
}

void DrawModernComboChrome(HWND hwnd, HDC dc)
{
    if (!hwnd || !dc) {
        return;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool hovered = gHoveredModernCombo == hwnd;
    const bool focused = GetFocus() == hwnd;
    const int arrowWidth = 27;
    RECT arrowRect{(std::max)(rc.left, rc.right - arrowWidth), rc.top + 1, rc.right - 1, rc.bottom - 1};

    HBRUSH arrowBrush = CreateSolidBrush(enabled ? kColorInputBg : kThemeColors.controlDisabledBackground);
    if (arrowBrush) {
        FillRect(dc, &arrowRect, arrowBrush);
        DeleteObject(arrowBrush);
    }

    const COLORREF borderColor = !enabled
        ? kThemeColors.controlDisabledBorder
        : (focused ? kThemeColors.accentBright : (hovered ? kThemeColors.controlHoverBorder : kColorInputBorder));
    const int borderThickness = 1;
    HBRUSH borderBrush = CreateSolidBrush(borderColor);
    if (borderBrush) {
        RECT topBorder{rc.left, rc.top, rc.right, rc.top + borderThickness};
        RECT bottomBorder{rc.left, rc.bottom - borderThickness, rc.right, rc.bottom};
        RECT leftBorder{rc.left, rc.top + borderThickness, rc.left + borderThickness, rc.bottom - borderThickness};
        RECT rightBorder{rc.right - borderThickness, rc.top + borderThickness, rc.right, rc.bottom - borderThickness};
        FillRect(dc, &topBorder, borderBrush);
        FillRect(dc, &bottomBorder, borderBrush);
        FillRect(dc, &leftBorder, borderBrush);
        FillRect(dc, &rightBorder, borderBrush);
        DeleteObject(borderBrush);
    }

    const int centerX = (arrowRect.left + arrowRect.right) / 2;
    const int centerY = (arrowRect.top + arrowRect.bottom) / 2;
    HPEN arrowPen = CreatePen(PS_SOLID, 2, enabled ? kThemeColors.accentBright : kThemeColors.controlDisabledText);
    HGDIOBJ oldArrowPen = arrowPen ? SelectObject(dc, arrowPen) : nullptr;
    MoveToEx(dc, centerX - 5, centerY - 2, nullptr);
    LineTo(dc, centerX, centerY + 3);
    LineTo(dc, centerX + 5, centerY - 2);
    if (oldArrowPen) {
        SelectObject(dc, oldArrowPen);
    }
    if (arrowPen) {
        DeleteObject(arrowPen);
    }
}

void DrawModernCombo(HWND hwnd, HDC dc)
{
    if (!hwnd || !dc) {
        return;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    DRAWITEMSTRUCT drawInfo{};
    drawInfo.CtlType = ODT_COMBOBOX;
    drawInfo.hDC = dc;
    drawInfo.hwndItem = hwnd;
    drawInfo.itemID = static_cast<UINT>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
    drawInfo.itemState = ODS_COMBOBOXEDIT;
    drawInfo.rcItem = rc;
    DrawStyledComboItem(&drawInfo);
    DrawModernComboChrome(hwnd, dc);
}

void RefreshModernCombo(HWND hwnd)
{
    if (!hwnd) {
        return;
    }
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
}

LRESULT CALLBACK ModernToggleSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData)
{
    auto* ctx = reinterpret_cast<AppContext*>(refData);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        DrawModernToggle(hwnd, dc, ctx);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        DrawModernToggle(hwnd, reinterpret_cast<HDC>(wParam), ctx);
        return 0;
    case WM_MOUSEMOVE: {
        if (gHoveredModernToggle != hwnd) {
            HWND previous = gHoveredModernToggle;
            gHoveredModernToggle = hwnd;
            if (previous && IsWindow(previous)) {
                InvalidateRect(previous, nullptr, FALSE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);
        break;
    }
    case WM_MOUSELEAVE:
        if (gHoveredModernToggle == hwnd) {
            gHoveredModernToggle = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case BM_SETCHECK:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_CAPTURECHANGED: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        return result;
    }
    case WM_NCDESTROY:
        if (gHoveredModernToggle == hwnd) {
            gHoveredModernToggle = nullptr;
        }
        RemoveWindowSubclass(hwnd, ModernToggleSubclassProc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK ModernComboSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        DrawModernCombo(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        DrawModernCombo(hwnd, reinterpret_cast<HDC>(wParam));
        return 0;
    case WM_NCPAINT:
        return 0;
    case kRefreshModernComboMessage:
        RefreshModernCombo(hwnd);
        return 0;
    case WM_MOUSEMOVE: {
        if (gHoveredModernCombo != hwnd) {
            HWND previous = gHoveredModernCombo;
            gHoveredModernCombo = hwnd;
            if (previous && IsWindow(previous)) {
                InvalidateRect(previous, nullptr, FALSE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);
        break;
    }
    case WM_MOUSELEAVE:
        if (gHoveredModernCombo == hwnd) {
            gHoveredModernCombo = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case CB_SETCURSEL: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        RefreshModernCombo(hwnd);
        return result;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        RefreshModernCombo(hwnd);
        return result;
    }
    case WM_NCDESTROY:
        if (gHoveredModernCombo == hwnd) {
            gHoveredModernCombo = nullptr;
        }
        RemoveWindowSubclass(hwnd, ModernComboSubclassProc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

const wchar_t* GetHelpTooltipTextForControl(const AppContext* ctx, HWND control)
{
    if (!ctx || !control) {
        return nullptr;
    }
    if (control == ctx->outputStatus && ctx->outputFolderWillBeCreatedOnRecordStart) {
        return L"Folder does not exist but will be created upon recording start.";
    }
    if (control == ctx->postRunDelayHelpIcon) {
        return L"After your keystone ends, Bean keeps recording for this many seconds to capture wrap-up moments.";
    }
    if (control == ctx->presetHelpIcon) {
        static thread_local std::wstring presetTooltip;
        std::wostringstream tooltip;
        tooltip
            << L"Technical details:\n"
            << L"Ultra = " << bean::obs::ResolveConstantQualityValueForPreset("ultra") << L" (CQP/CRF/ICQ)\n"
            << L"High = " << bean::obs::ResolveConstantQualityValueForPreset("high") << L" (CQP/CRF/ICQ)\n"
            << L"Medium = " << bean::obs::ResolveConstantQualityValueForPreset("medium") << L" (CQP/CRF/ICQ)\n"
            << L"Low = " << bean::obs::ResolveConstantQualityValueForPreset("low") << L" (CQP/CRF/ICQ)\n"
            << L"Minimum = " << bean::obs::ResolveConstantQualityValueForPreset("minimum") << L" (CQP/CRF/ICQ)\n\n"
            << L"x264 preset: Ultra = medium, High/Medium = veryfast, Low/Minimum = superfast.";
        presetTooltip = tooltip.str();
        return presetTooltip.c_str();
    }
    if (control == ctx->advancedLoggingHelpIcon) {
        return L"If changed in-game, Bean can only detect the change after /reload, relog, or closing WoW.";
    }
    return nullptr;
}

void ShowConfigurationTooltip(AppContext* ctx, HWND anchor, const wchar_t* text)
{
    if (!ctx || !anchor || !text || !ctx->configurationTooltip || !IsWindow(ctx->configurationTooltip)) {
        return;
    }
    HWND tooltipParent = GetParent(ctx->configurationTooltip);
    if (!tooltipParent) {
        return;
    }

    SetWindowTextW(ctx->configurationTooltip, text);
    HDC dc = GetDC(ctx->configurationTooltip);
    if (!dc) {
        return;
    }

    HGDIOBJ oldFont = nullptr;
    if (gTheme.uiFont) {
        oldFont = SelectObject(dc, gTheme.uiFont);
    }
    RECT textRect{0, 0, 320, 0};
    DrawTextW(dc, text, -1, &textRect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
    ReleaseDC(ctx->configurationTooltip, dc);

    RECT anchorRect{};
    if (!GetWindowRect(anchor, &anchorRect)) {
        return;
    }
    MapWindowPoints(HWND_DESKTOP, tooltipParent, reinterpret_cast<POINT*>(&anchorRect), 2);
    const int width = (std::max)(180, static_cast<int>(textRect.right - textRect.left) + 20);
    const int height = (std::max)(32, static_cast<int>(textRect.bottom - textRect.top) + 14);
    RECT panelRect{};
    GetClientRect(tooltipParent, &panelRect);
    int x = anchorRect.right + 10;
    if (x + width > panelRect.right - 6) {
        x = anchorRect.left - width - 10;
    }
    x = (std::max)(6, x);
    const int y = anchorRect.top - 2;
    SetWindowPos(
        ctx->configurationTooltip,
        HWND_TOP,
        x,
        y,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(ctx->configurationTooltip, nullptr, TRUE);
}

void PaintRecordingsHeader(HWND header, HDC hdc)
{
    if (!header || !hdc) {
        return;
    }

    RECT rc{};
    GetClientRect(header, &rc);
    HBRUSH bgBrush = CreateSolidBrush(kColorInputBg);
    if (bgBrush) {
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);
    }

    HGDIOBJ oldFont = nullptr;
    if (gTheme.recordingsFont) {
        oldFont = SelectObject(hdc, gTheme.recordingsFont);
    } else if (gTheme.uiFont) {
        oldFont = SelectObject(hdc, gTheme.uiFont);
    }

    const int columnCount = static_cast<int>(SendMessageW(header, HDM_GETITEMCOUNT, 0, 0));
    for (int i = 0; i < columnCount; ++i) {
        RECT cell{};
        if (!SendMessageW(header, HDM_GETITEMRECT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&cell))) {
            continue;
        }

        HDITEMW item{};
        wchar_t textBuffer[128] = {};
        item.mask = HDI_TEXT | HDI_FORMAT;
        item.pszText = textBuffer;
        item.cchTextMax = static_cast<int>(std::size(textBuffer));
        SendMessageW(header, HDM_GETITEMW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&item));

        RECT textRect = cell;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kColorTextMuted);
        if ((item.fmt & HDF_CENTER) != 0) {
            DrawTextW(hdc, textBuffer, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            textRect.left += 8;
            DrawTextW(hdc, textBuffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    EnsureThemeResources();
    HGDIOBJ oldPen = gTheme.listGridPen ? SelectObject(hdc, gTheme.listGridPen) : nullptr;
    if (gTheme.listGridPen) {
        const int columnCountForLines = static_cast<int>(SendMessageW(header, HDM_GETITEMCOUNT, 0, 0));
        for (int i = 0; i < columnCountForLines; ++i) {
            RECT cell{};
            if (!SendMessageW(header, HDM_GETITEMRECT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&cell))) {
                continue;
            }
            MoveToEx(hdc, cell.right - 1, cell.top, nullptr);
            LineTo(hdc, cell.right - 1, cell.bottom);
        }
        MoveToEx(hdc, rc.left, rc.bottom - 1, nullptr);
        LineTo(hdc, rc.right, rc.bottom - 1);
    }
    if (oldPen) {
        SelectObject(hdc, oldPen);
    }
    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
}

} // namespace

void ScheduleModernComboRedraw(HWND combo)
{
    if (combo) {
        PostMessageW(combo, kRefreshModernComboMessage, 0, 0);
    }
}

namespace {

void DrawCheckOrXGlyph(HDC dc, const RECT& bounds, bool valid)
{
    if (!dc) {
        return;
    }
    EnsureThemeResources();

    if (EnsureAlertGdiplus()) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        if (graphics.GetLastStatus() == Gdiplus::Ok) {
            const Gdiplus::Color glyphColor(
                255,
                GetRValue(valid ? kColorSuccess : kColorFailure),
                GetGValue(valid ? kColorSuccess : kColorFailure),
                GetBValue(valid ? kColorSuccess : kColorFailure));
            Gdiplus::Pen glyphPen(glyphColor, 2.0f);
            glyphPen.SetStartCap(Gdiplus::LineCapRound);
            glyphPen.SetEndCap(Gdiplus::LineCapRound);
            glyphPen.SetLineJoin(Gdiplus::LineJoinRound);

            const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(bounds.left) + 1.5f;
            const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(bounds.top) + 1.5f;
            const Gdiplus::REAL right = static_cast<Gdiplus::REAL>(bounds.right) - 1.5f;
            const Gdiplus::REAL bottom = static_cast<Gdiplus::REAL>(bounds.bottom) - 1.5f;
            const Gdiplus::REAL centerY = (top + bottom) / 2.0f;

            if (valid) {
                const Gdiplus::PointF points[] = {
                    {left, centerY},
                    {left + (right - left) * 0.34f, bottom - 0.5f},
                    {right, top + 0.5f},
                };
                graphics.DrawLines(&glyphPen, points, 3);
            } else {
                graphics.DrawLine(&glyphPen, left, top, right, bottom);
                graphics.DrawLine(&glyphPen, left, bottom, right, top);
            }
            return;
        }
    }

    HPEN pen = valid ? gTheme.successPen : gTheme.failurePen;
    HGDIOBJ oldPen = pen ? SelectObject(dc, pen) : nullptr;
    const int half = (bounds.right - bounds.left) / 2;
    if (valid) {
        MoveToEx(dc, bounds.left + 1, bounds.top + half, nullptr);
        LineTo(dc, bounds.left + half - 1, bounds.bottom - 1);
        LineTo(dc, bounds.right - 1, bounds.top + 1);
    } else {
        MoveToEx(dc, bounds.left, bounds.top, nullptr);
        LineTo(dc, bounds.right, bounds.bottom);
        MoveToEx(dc, bounds.left, bounds.bottom, nullptr);
        LineTo(dc, bounds.right, bounds.top);
    }
    if (oldPen) {
        SelectObject(dc, oldPen);
    }
}

void DrawStatusDot(HDC dc, const RECT& bounds, COLORREF color)
{
    if (!dc) {
        return;
    }

    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    if (EnsureAlertGdiplus()) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        if (graphics.GetLastStatus() == Gdiplus::Ok) {
            const Gdiplus::Color dotColor(
                255,
                GetRValue(color),
                GetGValue(color),
                GetBValue(color));
            Gdiplus::SolidBrush dotBrush(dotColor);
            const Gdiplus::REAL diameter = static_cast<Gdiplus::REAL>((std::min)(width, height)) - 1.0f;
            const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(bounds.left) + 0.5f;
            const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(bounds.top) + 0.5f;
            graphics.FillEllipse(&dotBrush, left, top, diameter, diameter);
            return;
        }
    }

    EnsureThemeResources();
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = brush ? SelectObject(dc, brush) : nullptr;
    const int diameter = (std::min)(width, height);
    const int left = bounds.left + (width - diameter) / 2;
    const int top = bounds.top + (height - diameter) / 2;
    Ellipse(dc, left, top, left + diameter, top + diameter);
    if (oldBrush) {
        SelectObject(dc, oldBrush);
    }
    if (brush) {
        DeleteObject(brush);
    }
}

void DrawUpdateAlertGlyph(HDC dc, const RECT& bounds)
{
    if (!dc) {
        return;
    }
    EnsureThemeResources();

    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int diameter = (std::max)(10, (std::min)(14, (std::min)(width, height)));
    const int left = bounds.left + (width - diameter) / 2;
    const int top = bounds.top + (height - diameter) / 2;
    const int right = left + diameter;
    const int bottom = top + diameter;

    if (EnsureAlertGdiplus()) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        const Gdiplus::Color warningColor(
            255,
            GetRValue(kColorWarning),
            GetGValue(kColorWarning),
            GetBValue(kColorWarning));
        const Gdiplus::Color glyphColor(
            255,
            GetRValue(kColorWindowTop),
            GetGValue(kColorWindowTop),
            GetBValue(kColorWindowTop));
        Gdiplus::SolidBrush warningBrush(warningColor);
        Gdiplus::Pen warningPen(warningColor, 1.0f);
        graphics.FillEllipse(
            &warningBrush,
            static_cast<Gdiplus::REAL>(left),
            static_cast<Gdiplus::REAL>(top),
            static_cast<Gdiplus::REAL>(diameter - 1),
            static_cast<Gdiplus::REAL>(diameter - 1));
        graphics.DrawEllipse(
            &warningPen,
            static_cast<Gdiplus::REAL>(left) + 0.5f,
            static_cast<Gdiplus::REAL>(top) + 0.5f,
            static_cast<Gdiplus::REAL>(diameter - 2),
            static_cast<Gdiplus::REAL>(diameter - 2));

        Gdiplus::Pen glyphPen(glyphColor, 1.8f);
        const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>(left + right) / 2.0f;
        graphics.DrawLine(
            &glyphPen,
            centerX,
            static_cast<Gdiplus::REAL>(top) + 3.0f,
            centerX,
            static_cast<Gdiplus::REAL>(bottom) - 5.0f);
        Gdiplus::SolidBrush glyphBrush(glyphColor);
        graphics.FillEllipse(
            &glyphBrush,
            centerX - 1.0f,
            static_cast<Gdiplus::REAL>(bottom) - 4.0f,
            2.0f,
            2.0f);
        return;
    }

    HPEN borderPen = CreatePen(PS_SOLID, 1, kColorWarning);
    HBRUSH fillBrush = CreateSolidBrush(kColorWarning);
    HGDIOBJ oldPen = borderPen ? SelectObject(dc, borderPen) : nullptr;
    HGDIOBJ oldBrush = fillBrush ? SelectObject(dc, fillBrush) : nullptr;
    Ellipse(dc, left, top, right, bottom);
    if (oldBrush) {
        SelectObject(dc, oldBrush);
    }
    if (oldPen) {
        SelectObject(dc, oldPen);
    }
    if (fillBrush) {
        DeleteObject(fillBrush);
    }
    if (borderPen) {
        DeleteObject(borderPen);
    }

    const int centerX = (left + right) / 2;
    HPEN exclamationPen = CreatePen(PS_SOLID, 2, kColorWindowTop);
    HGDIOBJ oldExclamationPen = exclamationPen ? SelectObject(dc, exclamationPen) : nullptr;
    MoveToEx(dc, centerX, top + 3, nullptr);
    LineTo(dc, centerX, bottom - 5);
    if (oldExclamationPen) {
        SelectObject(dc, oldExclamationPen);
    }
    if (exclamationPen) {
        DeleteObject(exclamationPen);
    }
    HBRUSH dotBrush = CreateSolidBrush(kColorWindowTop);
    HGDIOBJ oldDotBrush = dotBrush ? SelectObject(dc, dotBrush) : nullptr;
    Ellipse(dc, centerX - 1, bottom - 4, centerX + 2, bottom - 1);
    if (oldDotBrush) {
        SelectObject(dc, oldDotBrush);
    }
    if (dotBrush) {
        DeleteObject(dotBrush);
    }
}

} // namespace

void EnsureThemeResources()
{
    if (!gTheme.uiFont) {
        gTheme.uiFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Segoe UI");
    }
    if (!gTheme.mutedHintFont) {
        gTheme.mutedHintFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Segoe UI");
    }
    if (!gTheme.mutedItalicHintFont) {
        gTheme.mutedItalicHintFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Segoe UI");
    }
    if (!gTheme.statusIndicatorFont) {
        gTheme.statusIndicatorFont = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Segoe UI");
    }
    if (!gTheme.recordingsFont) {
        gTheme.recordingsFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Segoe UI");
    }
    if (!gTheme.headingFont) {
        gTheme.headingFont = CreateFontW(-27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Segoe UI");
    }
    if (!gTheme.inputBrush) {
        gTheme.inputBrush = CreateSolidBrush(kColorInputBg);
    }
    if (!gTheme.youtubeInputBrush) {
        gTheme.youtubeInputBrush = CreateSolidBrush(kColorYouTubeInputBg);
    }
    if (!gTheme.buttonBrush) {
        gTheme.buttonBrush = CreateSolidBrush(kColorButtonBg);
    }
    if (!gTheme.panelSolidBrush) {
        gTheme.panelSolidBrush = CreateSolidBrush(kColorPanelBottom);
    }
    if (!gTheme.panelBorderBrush) {
        gTheme.panelBorderBrush = CreateSolidBrush(kColorPanelBorder);
    }
    if (!gTheme.tooltipBrush) {
        gTheme.tooltipBrush = CreateSolidBrush(kColorTooltipBg);
    }
    if (!gTheme.successPen) {
        gTheme.successPen = CreatePen(PS_SOLID, 2, kColorSuccess);
    }
    if (!gTheme.failurePen) {
        gTheme.failurePen = CreatePen(PS_SOLID, 2, kColorFailure);
    }
    if (!gTheme.listGridPen) {
        gTheme.listGridPen = CreatePen(PS_SOLID, 1, kColorListGrid);
    }
    if (!gTheme.mutedDotPen) {
        gTheme.mutedDotPen = CreatePen(PS_SOLID, 1, kThemeColors.mutedDot);
    }
    if (!gTheme.recordingDotPen) {
        gTheme.recordingDotPen = CreatePen(PS_SOLID, 1, kThemeColors.recordingDot);
    }
    if (!gTheme.successBrush) {
        gTheme.successBrush = CreateSolidBrush(kColorSuccess);
    }
    if (!gTheme.failureBrush) {
        gTheme.failureBrush = CreateSolidBrush(kColorFailure);
    }
    if (!gTheme.mutedDotBrush) {
        gTheme.mutedDotBrush = CreateSolidBrush(kThemeColors.controlTabBorder);
    }
    if (!gTheme.recordingDotBrush) {
        gTheme.recordingDotBrush = CreateSolidBrush(kThemeColors.recordingDot);
    }
}

void RebuildThemeColorResources()
{
    if (gTheme.inputBrush) { DeleteObject(gTheme.inputBrush); gTheme.inputBrush = nullptr; }
    if (gTheme.youtubeInputBrush) { DeleteObject(gTheme.youtubeInputBrush); gTheme.youtubeInputBrush = nullptr; }
    if (gTheme.buttonBrush) { DeleteObject(gTheme.buttonBrush); gTheme.buttonBrush = nullptr; }
    if (gTheme.panelSolidBrush) { DeleteObject(gTheme.panelSolidBrush); gTheme.panelSolidBrush = nullptr; }
    if (gTheme.panelBorderBrush) { DeleteObject(gTheme.panelBorderBrush); gTheme.panelBorderBrush = nullptr; }
    if (gTheme.tooltipBrush) { DeleteObject(gTheme.tooltipBrush); gTheme.tooltipBrush = nullptr; }
    if (gTheme.successPen) { DeleteObject(gTheme.successPen); gTheme.successPen = nullptr; }
    if (gTheme.failurePen) { DeleteObject(gTheme.failurePen); gTheme.failurePen = nullptr; }
    if (gTheme.listGridPen) { DeleteObject(gTheme.listGridPen); gTheme.listGridPen = nullptr; }
    if (gTheme.mutedDotPen) { DeleteObject(gTheme.mutedDotPen); gTheme.mutedDotPen = nullptr; }
    if (gTheme.recordingDotPen) { DeleteObject(gTheme.recordingDotPen); gTheme.recordingDotPen = nullptr; }
    if (gTheme.successBrush) { DeleteObject(gTheme.successBrush); gTheme.successBrush = nullptr; }
    if (gTheme.failureBrush) { DeleteObject(gTheme.failureBrush); gTheme.failureBrush = nullptr; }
    if (gTheme.mutedDotBrush) { DeleteObject(gTheme.mutedDotBrush); gTheme.mutedDotBrush = nullptr; }
    if (gTheme.recordingDotBrush) { DeleteObject(gTheme.recordingDotBrush); gTheme.recordingDotBrush = nullptr; }
    EnsureThemeResources();
}

void DestroyThemeResources()
{
    if (gTheme.uiFont) { DeleteObject(gTheme.uiFont); gTheme.uiFont = nullptr; }
    if (gTheme.mutedHintFont) { DeleteObject(gTheme.mutedHintFont); gTheme.mutedHintFont = nullptr; }
    if (gTheme.mutedItalicHintFont) { DeleteObject(gTheme.mutedItalicHintFont); gTheme.mutedItalicHintFont = nullptr; }
    if (gTheme.statusIndicatorFont) { DeleteObject(gTheme.statusIndicatorFont); gTheme.statusIndicatorFont = nullptr; }
    if (gTheme.recordingsFont) { DeleteObject(gTheme.recordingsFont); gTheme.recordingsFont = nullptr; }
    if (gTheme.headingFont) { DeleteObject(gTheme.headingFont); gTheme.headingFont = nullptr; }
    if (gTheme.inputBrush) { DeleteObject(gTheme.inputBrush); gTheme.inputBrush = nullptr; }
    if (gTheme.youtubeInputBrush) { DeleteObject(gTheme.youtubeInputBrush); gTheme.youtubeInputBrush = nullptr; }
    if (gTheme.buttonBrush) { DeleteObject(gTheme.buttonBrush); gTheme.buttonBrush = nullptr; }
    if (gTheme.panelSolidBrush) { DeleteObject(gTheme.panelSolidBrush); gTheme.panelSolidBrush = nullptr; }
    if (gTheme.panelBorderBrush) { DeleteObject(gTheme.panelBorderBrush); gTheme.panelBorderBrush = nullptr; }
    if (gTheme.tooltipBrush) { DeleteObject(gTheme.tooltipBrush); gTheme.tooltipBrush = nullptr; }
    if (gTheme.successPen) { DeleteObject(gTheme.successPen); gTheme.successPen = nullptr; }
    if (gTheme.failurePen) { DeleteObject(gTheme.failurePen); gTheme.failurePen = nullptr; }
    if (gTheme.listGridPen) { DeleteObject(gTheme.listGridPen); gTheme.listGridPen = nullptr; }
    if (gTheme.mutedDotPen) { DeleteObject(gTheme.mutedDotPen); gTheme.mutedDotPen = nullptr; }
    if (gTheme.recordingDotPen) { DeleteObject(gTheme.recordingDotPen); gTheme.recordingDotPen = nullptr; }
    if (gTheme.successBrush) { DeleteObject(gTheme.successBrush); gTheme.successBrush = nullptr; }
    if (gTheme.failureBrush) { DeleteObject(gTheme.failureBrush); gTheme.failureBrush = nullptr; }
    if (gTheme.mutedDotBrush) { DeleteObject(gTheme.mutedDotBrush); gTheme.mutedDotBrush = nullptr; }
    if (gTheme.recordingDotBrush) { DeleteObject(gTheme.recordingDotBrush); gTheme.recordingDotBrush = nullptr; }
    if (gAlertGdiplusToken != 0) {
        Gdiplus::GdiplusShutdown(gAlertGdiplusToken);
        gAlertGdiplusToken = 0;
    }
}

void DestroyParticipantSpecIcons(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->participantSpecIcons) {
        ImageList_Destroy(ctx->participantSpecIcons);
        ctx->participantSpecIcons = nullptr;
    }
    ctx->participantSpecIconIndexByKey.clear();
}

void EnsureParticipantSpecIconList(AppContext* ctx)
{
    if (!ctx || !ctx->recordingsInfoText || ctx->participantSpecIcons) {
        return;
    }
    ctx->participantSpecIcons = ImageList_Create(kSpecIconCanvasSizePx, kSpecIconCanvasSizePx, ILC_COLOR32 | ILC_MASK, 8, 8);
    if (!ctx->participantSpecIcons) {
        return;
    }
    ListView_SetImageList(ctx->recordingsInfoText, ctx->participantSpecIcons, LVSIL_SMALL);
}

int ResolveParticipantSpecIconIndex(
    AppContext* ctx,
    const std::optional<std::string>& className,
    const std::optional<std::string>& specName)
{
    if (!ctx || !className.has_value() || className->empty() || !specName.has_value() || specName->empty()) {
        return I_IMAGENONE;
    }
    EnsureParticipantSpecIconList(ctx);
    if (!ctx->participantSpecIcons) {
        return I_IMAGENONE;
    }

    const std::string key = BuildSpecIconKey(*className, *specName);
    if (key.empty()) {
        return I_IMAGENONE;
    }

    const auto cached = ctx->participantSpecIconIndexByKey.find(key);
    if (cached != ctx->participantSpecIconIndexByKey.end()) {
        return cached->second;
    }

    const auto imagePath = SpecIconPathFromExe(*className, *specName);
    HBITMAP bitmap = LoadPngBitmapForImageList(imagePath, kSpecIconSizePx, kSpecIconCanvasSizePx, kSpecIconVerticalOffsetPx);
    if (!bitmap) {
        return I_IMAGENONE;
    }
    const int imageIndex = ImageList_Add(ctx->participantSpecIcons, bitmap, nullptr);
    DeleteObject(bitmap);
    if (imageIndex < 0) {
        return I_IMAGENONE;
    }
    ctx->participantSpecIconIndexByKey[key] = imageIndex;
    return imageIndex;
}

void ApplyUiFonts(HWND root)
{
    if (!root || !gTheme.uiFont) {
        return;
    }
    EnumChildWindows(root, ApplyUiFontEnumProc, 0);
}

void ApplyRecordingsFonts(AppContext* ctx)
{
    if (!ctx || !ctx->recordingsPanel || !gTheme.recordingsFont) {
        return;
    }
    SendMessageW(ctx->recordingsPanel, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.recordingsFont), TRUE);
    EnumChildWindows(ctx->recordingsPanel, ApplyRecordingsFontEnumProc, 0);
}

bool IsStyledButtonId(int controlId)
{
    switch (controlId) {
    case IDC_TAB_STATUS:
    case IDC_TAB_CONFIGURATION:
    case IDC_TAB_CHAT_PRIVACY:
    case IDC_TAB_RECORDINGS:
    case IDC_TAB_YOUTUBE:
    case IDC_TAB_CLIPS:
    case IDC_TAB_KEYBINDS:
    case IDC_TAB_ABOUT:
    case IDC_OUTPUT_BROWSE:
    case IDC_LOG_BROWSE:
    case IDC_RECORD_START:
    case IDC_RECORD_STOP:
    case IDC_STATUS_OPEN_LOG_FOLDER:
    case IDC_CHAT_BLOCKER_IMAGE_IMPORT_BUTTON:
    case IDC_CHAT_BLOCKER_IMAGE_OPEN_FOLDER_BUTTON:
    case IDC_RECORDINGS_REFRESH:
    case IDC_RECORDINGS_OPEN_FOLDER:
    case IDC_RECORDINGS_OPEN_DB_FOLDER:
    case IDC_YOUTUBE_REFRESH:
    case IDC_YOUTUBE_LINK_BUTTON:
    case IDC_YOUTUBE_UNLINK_BUTTON:
    case IDC_YOUTUBE_UNLINK_YES_BUTTON:
    case IDC_YOUTUBE_UNLINK_NO_BUTTON:
    case IDC_YOUTUBE_ACCOUNT_LINK:
    case IDC_YOUTUBE_UPLOAD_BUTTON:
    case IDC_ABOUT_WEBSITE_BUTTON:
    case IDC_ABOUT_EMAIL_BUTTON:
    case IDC_ABOUT_DISCORD_BUTTON:
    case IDC_ABOUT_CHECK_UPDATES_BUTTON:
    case IDC_CLIPS_REFRESH:
    case IDC_CLIPS_PLAY_PAUSE:
    case IDC_CLIPS_SET_START:
    case IDC_CLIPS_SET_END:
    case IDC_CLIPS_EXPORT:
    case IDC_CLIPS_EXPORT_PRECISE:
    case IDC_CLIPS_OPEN_FOLDER:
    case IDC_KEYBINDS_CREATE_CLIP_REBIND:
    case IDC_KEYBINDS_MANUAL_START_REBIND:
    case IDC_KEYBINDS_MANUAL_STOP_REBIND:
    case IDC_KEYBINDS_CREATE_CLIP_UNBIND:
    case IDC_KEYBINDS_MANUAL_START_UNBIND:
    case IDC_KEYBINDS_MANUAL_STOP_UNBIND:
    case IDC_KEYBINDS_CREATE_CLIP_RESET:
    case IDC_KEYBINDS_MANUAL_START_RESET:
    case IDC_KEYBINDS_MANUAL_STOP_RESET:
        return true;
    default:
        return false;
    }
}

bool IsStyledComboId(int controlId)
{
    switch (controlId) {
    case IDC_ENCODER_COMBO:
    case IDC_PRESET_COMBO:
    case IDC_CONTAINER_COMBO:
    case IDC_MICROPHONE_COMBO:
    case IDC_RECORDING_RESOLUTION_COMBO:
    case IDC_CHAT_BLOCKER_IMAGE_COMBO:
    case IDC_CHAT_BLOCKER_ANCHOR_COMBO:
    case IDC_CUSTOMIZE_THEME_COMBO:
    case IDC_YOUTUBE_PRIVACY_COMBO:
    case IDC_CLIPS_SOURCE_COMBO:
        return true;
    default:
        return false;
    }
}

bool IsStatusLightId(int controlId)
{
    return controlId == IDC_MONITOR_ICON
        || controlId == IDC_RECORD_ICON
        || controlId == IDC_WOW_WINDOW_ICON
        || controlId == IDC_OBS_INSTALL_ICON
        || controlId == IDC_FFMPEG_ICON
        || controlId == IDC_WARCRAFT_RECORDER_ICON
        || controlId == IDC_ADVANCED_LOGGING_ICON;
}

bool IsOwnerDrawStaticId(int controlId)
{
    return IsStatusLightId(controlId)
        || controlId == IDC_LENGTH_VALUE
        || controlId == IDC_YOUTUBE_LINK_STATUS
        || controlId == IDC_YOUTUBE_UPLOAD_STATUS
        || controlId == IDC_CONFIGURATION_TOOLTIP
        || controlId == IDC_PRESET_HELP
        || controlId == IDC_POST_RUN_DELAY_HELP
        || controlId == IDC_ADVANCED_LOGGING_HELP
        || controlId == IDC_CHAT_PREVIEW
        || controlId == IDC_CLIPS_TIMELINE
        || controlId == IDC_CLIPS_VOLUME_SLIDER;
}

void ConfigureStyledButtons(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    const std::array<int, 8> mainButtons = {IDC_TAB_STATUS, IDC_TAB_CONFIGURATION, IDC_TAB_CHAT_PRIVACY, IDC_TAB_RECORDINGS, IDC_TAB_YOUTUBE, IDC_TAB_CLIPS, IDC_TAB_KEYBINDS, IDC_TAB_ABOUT};
    for (const int id : mainButtons) {
        EnableOwnerDrawButton(ctx->mainWindow, id);
    }
    const std::array<int, 3> statusButtons = {IDC_RECORD_START, IDC_RECORD_STOP, IDC_STATUS_OPEN_LOG_FOLDER};
    for (const int id : statusButtons) {
        EnableOwnerDrawButton(ctx->statusPanel, id);
    }
    const std::array<int, 2> recorderButtons = {IDC_OUTPUT_BROWSE, IDC_LOG_BROWSE};
    for (const int id : recorderButtons) {
        EnableOwnerDrawButton(ctx->recorderPanel, id);
    }
    const std::array<int, 2> chatPrivacyButtons = {
        IDC_CHAT_BLOCKER_IMAGE_IMPORT_BUTTON, IDC_CHAT_BLOCKER_IMAGE_OPEN_FOLDER_BUTTON};
    for (const int id : chatPrivacyButtons) {
        EnableOwnerDrawButton(ctx->chatPrivacyPanel, id);
    }
    const std::array<int, 3> recordingsButtons = {
        IDC_RECORDINGS_REFRESH, IDC_RECORDINGS_OPEN_FOLDER, IDC_RECORDINGS_OPEN_DB_FOLDER};
    for (const int id : recordingsButtons) {
        EnableOwnerDrawButton(ctx->recordingsPanel, id);
    }
    const std::array<int, 7> youtubeButtons = {
        IDC_YOUTUBE_REFRESH, IDC_YOUTUBE_LINK_BUTTON, IDC_YOUTUBE_UNLINK_BUTTON,
        IDC_YOUTUBE_UNLINK_YES_BUTTON, IDC_YOUTUBE_UNLINK_NO_BUTTON, IDC_YOUTUBE_ACCOUNT_LINK,
        IDC_YOUTUBE_UPLOAD_BUTTON};
    for (const int id : youtubeButtons) {
        EnableOwnerDrawButton(ctx->youtubePanel, id);
    }
    const std::array<int, 7> clipsButtons = {
        IDC_CLIPS_REFRESH, IDC_CLIPS_PLAY_PAUSE, IDC_CLIPS_SET_START, IDC_CLIPS_SET_END,
        IDC_CLIPS_EXPORT, IDC_CLIPS_EXPORT_PRECISE, IDC_CLIPS_OPEN_FOLDER};
    for (const int id : clipsButtons) {
        EnableOwnerDrawButton(ctx->clipsPanel, id);
    }
    const std::array<int, 9> keybindButtons = {
        IDC_KEYBINDS_CREATE_CLIP_REBIND, IDC_KEYBINDS_MANUAL_START_REBIND, IDC_KEYBINDS_MANUAL_STOP_REBIND,
        IDC_KEYBINDS_CREATE_CLIP_UNBIND, IDC_KEYBINDS_MANUAL_START_UNBIND, IDC_KEYBINDS_MANUAL_STOP_UNBIND,
        IDC_KEYBINDS_CREATE_CLIP_RESET, IDC_KEYBINDS_MANUAL_START_RESET, IDC_KEYBINDS_MANUAL_STOP_RESET};
    for (const int id : keybindButtons) {
        EnableOwnerDrawButton(ctx->keybindsPanel, id);
    }
    const std::array<int, 4> aboutButtons = {
        IDC_ABOUT_WEBSITE_BUTTON, IDC_ABOUT_EMAIL_BUTTON, IDC_ABOUT_DISCORD_BUTTON, IDC_ABOUT_CHECK_UPDATES_BUTTON};
    for (const int id : aboutButtons) {
        EnableOwnerDrawButton(ctx->aboutPanel, id);
    }
}

void ConfigureModernControls(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    const std::array<std::pair<HWND, int>, 8> toggleControls = {{
        {ctx->recorderPanel, IDC_AUDIO_SCOPE_CHECK},
        {ctx->recorderPanel, IDC_AUDIO_SCOPE_WOW_DISCORD_RADIO},
        {ctx->recorderPanel, IDC_AUDIO_SCOPE_ALL_RADIO},
        {ctx->recorderPanel, IDC_MICROPHONE_CHECK},
        {ctx->recorderPanel, IDC_MICROPHONE_NOISE_SUPPRESSION_CHECK},
        {ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_ENABLED_CHECK},
        {ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_BLANK_RADIO},
        {ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_CUSTOM_RADIO},
    }};
    for (const auto& [parent, controlId] : toggleControls) {
        if (parent) {
            HWND control = GetDlgItem(parent, controlId);
            if (control) {
                SetWindowSubclass(control, ModernToggleSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
            }
        }
    }

    const std::array<std::pair<HWND, int>, 10> comboControls = {{
        {ctx->recorderPanel, IDC_ENCODER_COMBO},
        {ctx->recorderPanel, IDC_PRESET_COMBO},
        {ctx->recorderPanel, IDC_CONTAINER_COMBO},
        {ctx->recorderPanel, IDC_MICROPHONE_COMBO},
        {ctx->recorderPanel, IDC_RECORDING_RESOLUTION_COMBO},
        {ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_IMAGE_COMBO},
        {ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_ANCHOR_COMBO},
        {ctx->keybindsPanel, IDC_CUSTOMIZE_THEME_COMBO},
        {ctx->youtubePanel, IDC_YOUTUBE_PRIVACY_COMBO},
        {ctx->clipsPanel, IDC_CLIPS_SOURCE_COMBO},
    }};
    for (const auto& [parent, controlId] : comboControls) {
        if (!parent) {
            continue;
        }
        HWND combo = GetDlgItem(parent, controlId);
        if (!combo) {
            continue;
        }
        LONG_PTR style = GetWindowLongPtrW(combo, GWL_STYLE);
        style = (style & ~WS_BORDER) | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
        SetWindowLongPtrW(combo, GWL_STYLE, style);
        SetWindowPos(combo, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 24);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, 28);
        if (controlId == IDC_CUSTOMIZE_THEME_COMBO) {
            SendMessageW(combo, CB_SETMINVISIBLE, static_cast<WPARAM>(kThemeDefinitions.size()), 0);
        }
        SetWindowTheme(combo, L"", L"");
        SetWindowSubclass(combo, ModernComboSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
    }
}

void DrawStyledComboItem(const DRAWITEMSTRUCT* drawInfo)
{
    if (!drawInfo || !drawInfo->hwndItem) {
        return;
    }

    const bool isEditItem = (drawInfo->itemState & ODS_COMBOBOXEDIT) != 0;
    const bool isSelected = !isEditItem && (drawInfo->itemState & ODS_SELECTED) != 0;
    const bool enabled = IsWindowEnabled(drawInfo->hwndItem) != FALSE;
    const COLORREF background = !enabled
        ? kThemeColors.controlDisabledBackground
        : (isSelected ? kThemeColors.dropdownHoverBackground : kColorInputBg);
    const COLORREF textColor = !enabled ? kThemeColors.controlDisabledText : kColorTextPrimary;

    HBRUSH backgroundBrush = CreateSolidBrush(background);
    if (backgroundBrush) {
        FillRect(drawInfo->hDC, &drawInfo->rcItem, backgroundBrush);
        DeleteObject(backgroundBrush);
    }

    std::wstring text;
    if (drawInfo->itemID == static_cast<UINT>(-1)) {
        const int textLength = GetWindowTextLengthW(drawInfo->hwndItem);
        text.resize(static_cast<size_t>((std::max)(0, textLength)) + 1);
        if (textLength > 0) {
            GetWindowTextW(drawInfo->hwndItem, text.data(), textLength + 1);
        }
        text.resize(static_cast<size_t>((std::max)(0, textLength)));
    } else {
        const int textLength = static_cast<int>(SendMessageW(
            drawInfo->hwndItem, CB_GETLBTEXTLEN, drawInfo->itemID, 0));
        if (textLength >= 0) {
            text.resize(static_cast<size_t>(textLength) + 1);
            if (textLength > 0) {
                SendMessageW(
                    drawInfo->hwndItem,
                    CB_GETLBTEXT,
                    drawInfo->itemID,
                    reinterpret_cast<LPARAM>(text.data()));
            }
            text.resize(static_cast<size_t>(textLength));
        }
    }

    RECT textRect = drawInfo->rcItem;
    textRect.left += 10;
    textRect.right -= isEditItem ? 32 : 10;
    SetBkMode(drawInfo->hDC, TRANSPARENT);
    SetTextColor(drawInfo->hDC, textColor);
    HGDIOBJ oldFont = gTheme.uiFont ? SelectObject(drawInfo->hDC, gTheme.uiFont) : nullptr;
    DrawTextW(drawInfo->hDC, text.c_str(), -1, &textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (oldFont) {
        SelectObject(drawInfo->hDC, oldFont);
    }
}

void DrawStyledButton(const DRAWITEMSTRUCT* drawInfo, const AppContext* ctx)
{
    if (!drawInfo) {
        return;
    }
    RECT rc = drawInfo->rcItem;
    const bool isDisabled = (drawInfo->itemState & ODS_DISABLED) != 0;
    const bool isPressed = (drawInfo->itemState & ODS_SELECTED) != 0;
    const bool isHovered = !isDisabled && (drawInfo->hwndItem == gHoveredStyledButton);
    const bool isTab = drawInfo->CtlID == IDC_TAB_STATUS || drawInfo->CtlID == IDC_TAB_CONFIGURATION || drawInfo->CtlID == IDC_TAB_CHAT_PRIVACY
        || drawInfo->CtlID == IDC_TAB_RECORDINGS || drawInfo->CtlID == IDC_TAB_YOUTUBE || drawInfo->CtlID == IDC_TAB_CLIPS || drawInfo->CtlID == IDC_TAB_KEYBINDS || drawInfo->CtlID == IDC_TAB_ABOUT;
    const bool isLinkDisplay = drawInfo->CtlID == IDC_YOUTUBE_ACCOUNT_LINK;
    const bool isStatusTab = drawInfo->CtlID == IDC_TAB_STATUS;
    const bool isConfigurationTab = drawInfo->CtlID == IDC_TAB_CONFIGURATION;
    const bool showValidityIndicator = isStatusTab || isConfigurationTab;
    const bool showAboutUpdateIndicator = drawInfo->CtlID == IDC_TAB_ABOUT && ctx && ctx->aboutUpdateAvailable;
    const bool showTabIndicator = showValidityIndicator || showAboutUpdateIndicator;
    const bool isActiveTab = ctx
        && ((drawInfo->CtlID == IDC_TAB_STATUS && ctx->activeTab == AppContext::MainTab::Status)
            || (drawInfo->CtlID == IDC_TAB_CONFIGURATION && ctx->activeTab == AppContext::MainTab::Configuration)
            || (drawInfo->CtlID == IDC_TAB_CHAT_PRIVACY && ctx->activeTab == AppContext::MainTab::ChatPrivacy)
            || (drawInfo->CtlID == IDC_TAB_RECORDINGS && ctx->activeTab == AppContext::MainTab::Recordings)
            || (drawInfo->CtlID == IDC_TAB_YOUTUBE && ctx->activeTab == AppContext::MainTab::YouTube)
            || (drawInfo->CtlID == IDC_TAB_CLIPS && ctx->activeTab == AppContext::MainTab::Clips)
            || (drawInfo->CtlID == IDC_TAB_KEYBINDS && ctx->activeTab == AppContext::MainTab::Keybinds)
            || (drawInfo->CtlID == IDC_TAB_ABOUT && ctx->activeTab == AppContext::MainTab::About));
    COLORREF fill = isLinkDisplay ? kColorInputBg : kThemeColors.buttonBackground;
    COLORREF border = isLinkDisplay ? kColorInputBorder : kThemeColors.controlHoverBorder;
    COLORREF text = isTab ? (isActiveTab ? kColorButtonText : kColorTextMuted) : kColorButtonText;
    // The active tab is disabled only to prevent a redundant click; it should
    // still receive the active visual treatment below.
    if (isDisabled && !isActiveTab) {
        if (isLinkDisplay) { fill = kColorInputBg; border = kColorInputBorder; text = kColorTextMuted; }
        else { fill = kThemeColors.controlDisabledBackground; border = kThemeColors.controlDisabledBorder; text = kThemeColors.controlDisabledText; }
    } else if (isPressed) { fill = kThemeColors.controlPressedBackground; border = kThemeColors.controlPressedBorder; }
    else if (isActiveTab) {
        // Let the selected tab stand out through clean, high-contrast text.
        fill = kThemeColors.controlActiveBackground;
        border = kThemeColors.controlActiveBorder;
    }
    else if (isHovered && isTab) { fill = kThemeColors.controlHoverBackground; border = kThemeColors.controlHoverBorder; }
    else if (isHovered) { fill = kThemeColors.controlHoverBackground; border = kThemeColors.controlHoverBorder; }
    else if (isTab) { fill = kThemeColors.controlTabBackground; border = kThemeColors.controlTabBorder; }

    SetBkMode(drawInfo->hDC, TRANSPARENT);
    FillStyledButtonParentBackground(drawInfo->hDC, drawInfo->hwndItem, rc, ctx ? ctx->mainWindow : nullptr);
    HPEN borderPen = CreatePen(PS_SOLID, isActiveTab ? 2 : 1, border);
    HBRUSH fillBrush = CreateSolidBrush(fill);
    HGDIOBJ oldPen = borderPen ? SelectObject(drawInfo->hDC, borderPen) : nullptr;
    HGDIOBJ oldBrush = fillBrush ? SelectObject(drawInfo->hDC, fillBrush) : nullptr;
    Rectangle(drawInfo->hDC, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1);
    if (oldBrush) SelectObject(drawInfo->hDC, oldBrush);
    if (oldPen) SelectObject(drawInfo->hDC, oldPen);
    if (fillBrush) DeleteObject(fillBrush);
    if (borderPen) DeleteObject(borderPen);

    wchar_t textBuffer[256] = {};
    GetWindowTextW(drawInfo->hwndItem, textBuffer, static_cast<int>(std::size(textBuffer)));
    RECT textRect = rc;
    if (isPressed) {
        OffsetRect(&textRect, 0, 1);
    }
    SetTextColor(drawInfo->hDC, text);
    if (isLinkDisplay) {
        const int centerY = (rc.top + rc.bottom) / 2;
        const int indicatorLeft = rc.left + 12;
        const int indicatorSize = 11;
        const int half = indicatorSize / 2;
        RECT glyphBounds{};
        glyphBounds.left = indicatorLeft;
        glyphBounds.top = centerY - half;
        glyphBounds.right = indicatorLeft + indicatorSize;
        glyphBounds.bottom = glyphBounds.top + indicatorSize;
        DrawCheckOrXGlyph(drawInfo->hDC, glyphBounds, ctx && ctx->youtubeLinked);
        textRect.left += 34;
        textRect.right -= 8;
        DrawTextW(drawInfo->hDC, textBuffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        if (isTab) {
            textRect.left += 12;
            textRect.right -= showTabIndicator ? 28 : 10;
            DrawTextW(drawInfo->hDC, textBuffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (showValidityIndicator && ctx) {
                const bool isValid = isStatusTab ? IsStatusTabValid(ctx) : IsConfigurationTabValid(ctx);
                RECT iconRect = rc;
                iconRect.left = iconRect.right - 20;
                iconRect.right -= 8;
                const int height = iconRect.bottom - iconRect.top;
                const int targetSize = 11;
                const int yInset = (std::max)(0, (height - targetSize) / 2);
                iconRect.top += yInset;
                iconRect.bottom = iconRect.top + targetSize;
                DrawCheckOrXGlyph(drawInfo->hDC, iconRect, isValid);
            }
            if (showAboutUpdateIndicator) {
                RECT iconRect = rc;
                iconRect.left = iconRect.right - 20;
                iconRect.right -= 8;
                const int height = iconRect.bottom - iconRect.top;
                const int targetSize = 14;
                const int yInset = (std::max)(0, (height - targetSize) / 2);
                iconRect.top += yInset;
                iconRect.bottom = iconRect.top + targetSize;
                DrawUpdateAlertGlyph(drawInfo->hDC, iconRect);
            }
        } else {
            DrawTextW(drawInfo->hDC, textBuffer, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
}

void DrawStatusLight(const DRAWITEMSTRUCT* drawInfo, const AppContext* ctx)
{
    if (!drawInfo || !ctx) {
        return;
    }
    EnsureThemeResources();
    RECT rc = drawInfo->rcItem;
    const bool isPrerequisiteIcon = drawInfo->CtlID == IDC_WOW_WINDOW_ICON
        || drawInfo->CtlID == IDC_OBS_INSTALL_ICON
        || drawInfo->CtlID == IDC_FFMPEG_ICON
        || drawInfo->CtlID == IDC_WARCRAFT_RECORDER_ICON
        || drawInfo->CtlID == IDC_ADVANCED_LOGGING_ICON;
    if (isPrerequisiteIcon) {
        if (gTheme.inputBrush) {
            FillRect(drawInfo->hDC, &rc, gTheme.inputBrush);
        }
        bool isValid = false;
        for (const auto& row : kPrerequisiteRows) {
            if (row.iconId == drawInfo->CtlID) {
                isValid = PrerequisiteRowIsHealthy(ctx, row);
                break;
            }
        }
        const int centerX = (rc.left + rc.right) / 2;
        const int centerY = (rc.top + rc.bottom) / 2;
        const int indicatorSize = 11;
        const int half = indicatorSize / 2;
        RECT glyphBounds{};
        glyphBounds.left = centerX - half;
        glyphBounds.top = centerY - half;
        glyphBounds.right = centerX + half;
        glyphBounds.bottom = centerY + half;
        DrawCheckOrXGlyph(drawInfo->hDC, glyphBounds, isValid);
        return;
    }

    FillStyledButtonParentBackground(drawInfo->hDC, drawInfo->hwndItem, rc, ctx->mainWindow);
    const bool active = (drawInfo->CtlID == IDC_MONITOR_ICON) ? ctx->isMonitoring : ctx->isRecording;
    const bool isMonitor = drawInfo->CtlID == IDC_MONITOR_ICON;
    COLORREF color = kThemeColors.mutedDot;
    if (active && isMonitor) {
        color = kColorSuccess;
    } else if (active) {
        color = kThemeColors.recordingDot;
    }
    DrawStatusDot(drawInfo->hDC, rc, color);
}

void DrawLengthValue(const DRAWITEMSTRUCT* drawInfo)
{
    if (!drawInfo) {
        return;
    }
    RECT rc = drawInfo->rcItem;
    FillStyledButtonParentBackground(drawInfo->hDC, drawInfo->hwndItem, rc, nullptr);
    wchar_t textBuffer[64] = {};
    GetWindowTextW(drawInfo->hwndItem, textBuffer, static_cast<int>(std::size(textBuffer)));
    SetBkMode(drawInfo->hDC, TRANSPARENT);
    SetTextColor(drawInfo->hDC, kColorTextPrimary);
    DrawTextW(drawInfo->hDC, textBuffer, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void DrawHelpIcon(const DRAWITEMSTRUCT* drawInfo)
{
    if (!drawInfo) {
        return;
    }
    RECT rc = drawInfo->rcItem;
    // Owner-draw controls do not have true alpha transparency. Repaint the
    // parent panel's gradient underneath the icon so the area outside the
    // circle still matches the current background.
    FillStyledButtonParentBackground(drawInfo->hDC, drawInfo->hwndItem, rc, nullptr);

    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    const Gdiplus::REAL iconSize = static_cast<Gdiplus::REAL>((std::min)(width, height));
    const Gdiplus::REAL circleDiameter = iconSize - 2.5f;
    if (circleDiameter <= 0.0f) {
        return;
    }
    const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>(rc.left + width / 2.0f);
    const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>(rc.top + height / 2.0f);
    const Gdiplus::REAL circleLeft = centerX - circleDiameter / 2.0f;
    const Gdiplus::REAL circleTop = centerY - circleDiameter / 2.0f;

    if (EnsureAlertGdiplus()) {
        Gdiplus::Graphics graphics(drawInfo->hDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        if (graphics.GetLastStatus() == Gdiplus::Ok) {
            const auto toGdiPlusColor = [](COLORREF color) {
                return Gdiplus::Color(
                    255,
                    GetRValue(color),
                    GetGValue(color),
                    GetBValue(color));
            };

            const Gdiplus::RectF circleRect(circleLeft, circleTop, circleDiameter, circleDiameter);
            Gdiplus::SolidBrush circleBrush(toGdiPlusColor(kThemeColors.controlTabBackground));
            Gdiplus::Pen borderPen(toGdiPlusColor(kThemeColors.controlHoverBorder), 1.25f);
            graphics.FillEllipse(&circleBrush, circleRect);
            graphics.DrawEllipse(&borderPen, circleRect);

            // A vector glyph keeps the question mark compact and centered at
            // this size, instead of allowing a font's descender to cross the
            // circle border.
            const Gdiplus::REAL scale = circleDiameter / 13.5f;
            Gdiplus::GraphicsPath questionPath;
            questionPath.StartFigure();
            questionPath.AddBezier(
                Gdiplus::PointF(centerX - 2.3f * scale, centerY - 2.0f * scale),
                Gdiplus::PointF(centerX - 2.3f * scale, centerY - 3.8f * scale),
                Gdiplus::PointF(centerX - 1.3f * scale, centerY - 4.6f * scale),
                Gdiplus::PointF(centerX, centerY - 4.6f * scale));
            questionPath.AddBezier(
                Gdiplus::PointF(centerX, centerY - 4.6f * scale),
                Gdiplus::PointF(centerX + 1.8f * scale, centerY - 4.6f * scale),
                Gdiplus::PointF(centerX + 2.6f * scale, centerY - 3.5f * scale),
                Gdiplus::PointF(centerX + 2.6f * scale, centerY - 2.0f * scale));
            questionPath.AddBezier(
                Gdiplus::PointF(centerX + 2.6f * scale, centerY - 2.0f * scale),
                Gdiplus::PointF(centerX + 2.6f * scale, centerY - 0.2f * scale),
                Gdiplus::PointF(centerX + 0.2f * scale, centerY),
                Gdiplus::PointF(centerX, centerY + 1.7f * scale));

            Gdiplus::Pen questionPen(toGdiPlusColor(kColorTooltipText), 1.65f);
            questionPen.SetStartCap(Gdiplus::LineCapRound);
            questionPen.SetEndCap(Gdiplus::LineCapRound);
            questionPen.SetLineJoin(Gdiplus::LineJoinRound);
            graphics.DrawPath(&questionPen, &questionPath);

            Gdiplus::SolidBrush dotBrush(toGdiPlusColor(kColorTooltipText));
            const Gdiplus::REAL dotDiameter = 1.8f * scale;
            graphics.FillEllipse(
                &dotBrush,
                centerX - dotDiameter / 2.0f,
                centerY + 4.0f * scale - dotDiameter / 2.0f,
                dotDiameter,
                dotDiameter);
            return;
        }
    }

    // GDI+ is available on supported Windows systems, but keep a safe
    // fallback for startup failures.
    const int diameter = (std::max)(10, (std::min)(14, (std::min)(width, height) - 2));
    const int left = rc.left + (width - diameter) / 2;
    const int top = rc.top + (height - diameter) / 2;
    const int right = left + diameter;
    const int bottom = top + diameter;
    HPEN borderPen = CreatePen(PS_SOLID, 1, kThemeColors.controlHoverBorder);
    HBRUSH fillBrush = CreateSolidBrush(kThemeColors.controlTabBackground);
    HGDIOBJ oldPen = borderPen ? SelectObject(drawInfo->hDC, borderPen) : nullptr;
    HGDIOBJ oldBrush = fillBrush ? SelectObject(drawInfo->hDC, fillBrush) : nullptr;
    Ellipse(drawInfo->hDC, left, top, right, bottom);
    if (oldBrush) SelectObject(drawInfo->hDC, oldBrush);
    if (oldPen) SelectObject(drawInfo->hDC, oldPen);
    if (fillBrush) DeleteObject(fillBrush);
    if (borderPen) DeleteObject(borderPen);

    RECT textRect{left, top, right, bottom};
    SetBkMode(drawInfo->hDC, TRANSPARENT);
    SetTextColor(drawInfo->hDC, kColorTooltipText);
    DrawTextW(drawInfo->hDC, L"?", 1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void DrawConfigurationTooltip(const DRAWITEMSTRUCT* drawInfo)
{
    if (!drawInfo) {
        return;
    }
    RECT rc = drawInfo->rcItem;
    HBRUSH bgBrush = gTheme.tooltipBrush ? gTheme.tooltipBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    FillRect(drawInfo->hDC, &rc, bgBrush);
    HPEN borderPen = CreatePen(PS_SOLID, 1, kThemeColors.controlTabBorder);
    HGDIOBJ oldPen = borderPen ? SelectObject(drawInfo->hDC, borderPen) : nullptr;
    HGDIOBJ oldBrush = SelectObject(drawInfo->hDC, GetStockObject(HOLLOW_BRUSH));
    Rectangle(drawInfo->hDC, rc.left, rc.top, rc.right, rc.bottom);
    if (oldBrush) SelectObject(drawInfo->hDC, oldBrush);
    if (oldPen) SelectObject(drawInfo->hDC, oldPen);
    if (borderPen) DeleteObject(borderPen);
    wchar_t textBuffer[512] = {};
    GetWindowTextW(drawInfo->hwndItem, textBuffer, static_cast<int>(std::size(textBuffer)));
    RECT textRect = rc;
    textRect.left += 10;
    textRect.right -= 10;
    textRect.top += 7;
    textRect.bottom -= 7;
    SetBkMode(drawInfo->hDC, TRANSPARENT);
    SetTextColor(drawInfo->hDC, kColorTooltipText);
    if (gTheme.uiFont) {
        SelectObject(drawInfo->hDC, gTheme.uiFont);
    }
    DrawTextW(drawInfo->hDC, textBuffer, -1, &textRect, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
}

void HideConfigurationTooltip(AppContext* ctx)
{
    if (!ctx || !ctx->configurationTooltip || !IsWindow(ctx->configurationTooltip)) {
        return;
    }
    ShowWindow(ctx->configurationTooltip, SW_HIDE);
}

LRESULT CALLBACK HoverTooltipSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData)
{
    auto* ctx = reinterpret_cast<AppContext*>(refData);
    if (!ctx) {
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }
    const bool isTooltipCapableControl =
        hwnd == ctx->presetHelpIcon
        || hwnd == ctx->postRunDelayHelpIcon
        || hwnd == ctx->advancedLoggingHelpIcon
        || hwnd == ctx->outputStatus;
    if (!isTooltipCapableControl) {
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_MOUSEMOVE: {
        const bool isHelpIcon = hwnd == ctx->presetHelpIcon || hwnd == ctx->postRunDelayHelpIcon || hwnd == ctx->advancedLoggingHelpIcon;
        if (isHelpIcon && gHoveredHelpIcon != hwnd) {
            gHoveredHelpIcon = hwnd;
        }
        if (ctx->configurationTooltip && IsWindow(ctx->configurationTooltip)) {
            if (const wchar_t* text = GetHelpTooltipTextForControl(ctx, hwnd)) {
                ShowConfigurationTooltip(ctx, hwnd, text);
            } else {
                HideConfigurationTooltip(ctx);
            }
        }
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);
        break;
    }
    case WM_MOUSELEAVE:
        if (gHoveredHelpIcon == hwnd) {
            gHoveredHelpIcon = nullptr;
        }
        HideConfigurationTooltip(ctx);
        break;
    case WM_NCDESTROY:
        if (hwnd == ctx->presetHelpIcon || hwnd == ctx->postRunDelayHelpIcon || hwnd == ctx->advancedLoggingHelpIcon) {
            gHoveredHelpIcon = nullptr;
        }
        RemoveWindowSubclass(hwnd, HoverTooltipSubclassProc, subclassId);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void ConfigureConfigurationTooltips(AppContext* ctx)
{
    if (!ctx || !ctx->mainWindow) {
        return;
    }
    ctx->configurationTooltip = CreateWindowExW(
        0, L"STATIC", nullptr, WS_CHILD | SS_OWNERDRAW, CW_USEDEFAULT, CW_USEDEFAULT, 260, 44,
        ctx->mainWindow, reinterpret_cast<HMENU>(IDC_CONFIGURATION_TOOLTIP), nullptr, nullptr);
    if (!ctx->configurationTooltip) {
        return;
    }
    if (gTheme.uiFont) {
        SendMessageW(ctx->configurationTooltip, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.uiFont), TRUE);
    }
    ShowWindow(ctx->configurationTooltip, SW_HIDE);
}

void DrawRecordingsGridLines(const NMLVCUSTOMDRAW* customDraw, const AppContext* ctx)
{
    if (!customDraw || !ctx || !ctx->recordingsList) {
        return;
    }
    const int itemCount = ListView_GetItemCount(ctx->recordingsList);
    if (itemCount <= 0) {
        return;
    }
    RECT client{};
    GetClientRect(ctx->recordingsList, &client);
    RECT firstRow{};
    if (!ListView_GetItemRect(ctx->recordingsList, 0, &firstRow, LVIR_BOUNDS)) {
        return;
    }
    EnsureThemeResources();
    HGDIOBJ oldPen = gTheme.listGridPen ? SelectObject(customDraw->nmcd.hdc, gTheme.listGridPen) : nullptr;
    for (int i = 0; i < itemCount; ++i) {
        RECT rowRect{};
        if (!ListView_GetItemRect(ctx->recordingsList, i, &rowRect, LVIR_BOUNDS)) {
            continue;
        }
        MoveToEx(customDraw->nmcd.hdc, client.left, rowRect.bottom - 1, nullptr);
        LineTo(customDraw->nmcd.hdc, client.right, rowRect.bottom - 1);
    }
    const int scrollX = GetScrollPos(ctx->recordingsList, SB_HORZ);
    HWND header = ListView_GetHeader(ctx->recordingsList);
    const int columnCount = header ? Header_GetItemCount(header) : 0;
    int x = -scrollX;
    for (int col = 0; col < columnCount - 1; ++col) {
        x += ListView_GetColumnWidth(ctx->recordingsList, col);
        if (x <= client.left || x >= client.right) {
            continue;
        }
        MoveToEx(customDraw->nmcd.hdc, x - 1, firstRow.top, nullptr);
        LineTo(customDraw->nmcd.hdc, x - 1, client.bottom);
    }
    if (oldPen) SelectObject(customDraw->nmcd.hdc, oldPen);
}

LRESULT CALLBACK RecordingsHeaderSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData)
{
    auto* ctx = reinterpret_cast<AppContext*>(refData);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintRecordingsHeader(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        if (ctx && ctx->recordingsListHeader == hwnd) {
            ctx->recordingsListHeader = nullptr;
        }
        if (ctx && ctx->youtubeMediaListHeader == hwnd) {
            ctx->youtubeMediaListHeader = nullptr;
        }
        RemoveWindowSubclass(hwnd, RecordingsHeaderSubclassProc, 2);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void DrawYouTubeLinkStatus(const DRAWITEMSTRUCT* drawInfo, const AppContext* ctx)
{
    if (!drawInfo || !ctx) {
        return;
    }
    RECT rc = drawInfo->rcItem;
    if (gTheme.panelSolidBrush) {
        FillRect(drawInfo->hDC, &rc, gTheme.panelSolidBrush);
    }
    const int centerX = (rc.left + rc.right) / 2;
    const int centerY = (rc.top + rc.bottom) / 2;
    const int indicatorSize = 11;
    const int half = indicatorSize / 2;
    RECT glyphBounds{};
    glyphBounds.left = centerX - half;
    glyphBounds.top = centerY - half;
    glyphBounds.right = centerX + half;
    glyphBounds.bottom = centerY + half;
    DrawCheckOrXGlyph(drawInfo->hDC, glyphBounds, ctx->youtubeLinked);
}

void DrawYouTubeUploadStatus(const DRAWITEMSTRUCT* drawInfo, AppContext* ctx)
{
    if (!drawInfo || !ctx) {
        return;
    }
    RECT rc = drawInfo->rcItem;
    if (gTheme.panelSolidBrush) {
        FillRect(drawInfo->hDC, &rc, gTheme.panelSolidBrush);
    }

    const int textLength = GetWindowTextLengthW(drawInfo->hwndItem);
    std::wstring statusText(static_cast<size_t>((std::max)(0, textLength)), L'\0');
    if (textLength > 0) {
        GetWindowTextW(drawInfo->hwndItem, statusText.data(), textLength + 1);
    }
    HFONT normalFont = gTheme.uiFont;
    const HGDIOBJ oldFont = normalFont ? SelectObject(drawInfo->hDC, normalFont) : nullptr;
    SetBkMode(drawInfo->hDC, TRANSPARENT);
    SetTextColor(drawInfo->hDC, kColorTextPrimary);
    RECT statusRect = rc;
    DrawTextW(drawInfo->hDC, statusText.c_str(), -1, &statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    ctx->youtubeUploadLinkBounds = {};
    if (!ctx->youtubeLastVideoUrl.empty()) {
        SIZE statusSize{};
        SIZE linkSize{};
        GetTextExtentPoint32W(drawInfo->hDC, statusText.c_str(), static_cast<int>(statusText.size()), &statusSize);
        GetTextExtentPoint32W(drawInfo->hDC, L" ", 1, &linkSize);
        const int linkX = rc.left + statusSize.cx + linkSize.cx;
        RECT linkRect{linkX, rc.top, rc.right, rc.bottom};
        LOGFONTW logFont{};
        HFONT linkFont = nullptr;
        if (gTheme.uiFont && GetObjectW(gTheme.uiFont, sizeof(logFont), &logFont) == sizeof(logFont)) {
            logFont.lfUnderline = TRUE;
            linkFont = CreateFontIndirectW(&logFont);
        }
        if (linkFont) {
            SelectObject(drawInfo->hDC, linkFont);
        }
        SetTextColor(drawInfo->hDC, kThemeColors.accentBright);
        SIZE linkTextSize{};
        GetTextExtentPoint32W(drawInfo->hDC, L"View.", 5, &linkTextSize);
        DrawTextW(drawInfo->hDC, L"View.", -1, &linkRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT clientRect{};
        GetClientRect(drawInfo->hwndItem, &clientRect);
        ctx->youtubeUploadLinkBounds = {
            linkX - rc.left,
            clientRect.top,
            linkX - rc.left + linkTextSize.cx,
            clientRect.bottom};
        if (linkFont) {
            SelectObject(drawInfo->hDC, normalFont ? normalFont : oldFont);
            DeleteObject(linkFont);
        }
    }
    if (oldFont) {
        SelectObject(drawInfo->hDC, oldFont);
    }
}
