#include "app/AppDraw.h"

#include "app/AppPrerequisites.h"
#include "app/AppRecordingHelpers.h"
#include "app/AppUtilities.h"
#include "obs/IRecorderEngine.h"

#include <commctrl.h>
#include <gdiplus.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <utility>
#include <unordered_map>

#pragma comment(lib, "Gdiplus.lib")

VisualTheme gTheme;

namespace {

HWND gHoveredStyledButton = nullptr;
HWND gHoveredModernToggle = nullptr;
HWND gHoveredModernCombo = nullptr;
HWND gHoveredHelpIcon = nullptr;
HWND gHoveredThemedScrollbar = nullptr;
HWND gDraggingThemedScrollbar = nullptr;
int gThemedScrollbarDragOffset = 0;
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

struct BeanTextBoxState {
    AppContext* ctx = nullptr;
    std::wstring text;
    size_t caret = 0;
    size_t anchor = 0;
    std::wstring undoText;
    size_t undoCaret = 0;
    size_t undoAnchor = 0;
    bool hasUndo = false;
    bool numberOnly = false;
    bool multiline = false;
    bool readOnly = false;
    bool selecting = false;
    bool caretVisible = true;
    int scrollX = 0;
    int scrollY = 0;
    bool draggingVerticalScrollbar = false;
    int verticalScrollDragOffset = 0;
    HFONT font = nullptr;
    DWORD lastClickTime = 0;
    POINT lastClickPoint{};
    int clickCount = 0;
};

std::unordered_map<HWND, BeanTextBoxState> gBeanTextBoxes;
constexpr wchar_t kBeanTextBoxClassName[] = L"Bean.TextBox";
constexpr UINT_PTR kBeanTextBoxCaretTimerId = 1;

BeanTextBoxState* GetBeanTextBoxState(HWND hwnd)
{
    const auto it = gBeanTextBoxes.find(hwnd);
    return it == gBeanTextBoxes.end() ? nullptr : &it->second;
}

size_t BeanTextBoxSelectionStart(const BeanTextBoxState& state)
{
    return (std::min)(state.caret, state.anchor);
}

size_t BeanTextBoxSelectionEnd(const BeanTextBoxState& state)
{
    return (std::max)(state.caret, state.anchor);
}

void NotifyBeanTextBoxChanged(HWND hwnd)
{
    HWND parent = GetParent(hwnd);
    if (parent) {
        SendMessageW(
            parent,
            WM_COMMAND,
            MAKEWPARAM(static_cast<WORD>(GetDlgCtrlID(hwnd)), EN_CHANGE),
            reinterpret_cast<LPARAM>(hwnd));
    }
}

void BeanTextBoxSaveUndo(BeanTextBoxState& state)
{
    state.undoText = state.text;
    state.undoCaret = state.caret;
    state.undoAnchor = state.anchor;
    state.hasUndo = true;
}

bool BeanTextBoxReplaceSelection(BeanTextBoxState& state, std::wstring replacement)
{
    if (state.numberOnly) {
        replacement.erase(
            std::remove_if(
                replacement.begin(),
                replacement.end(),
                [](wchar_t character) { return character < L'0' || character > L'9'; }),
            replacement.end());
    }
    const size_t start = BeanTextBoxSelectionStart(state);
    const size_t end = BeanTextBoxSelectionEnd(state);
    if (start == end && replacement.empty()) {
        return false;
    }
    BeanTextBoxSaveUndo(state);
    state.text.replace(start, end - start, replacement);
    state.caret = start + replacement.size();
    state.anchor = state.caret;
    return true;
}

void BeanTextBoxCopySelection(HWND hwnd, const BeanTextBoxState& state)
{
    const size_t start = BeanTextBoxSelectionStart(state);
    const size_t end = BeanTextBoxSelectionEnd(state);
    if (start == end || !OpenClipboard(hwnd)) {
        return;
    }
    EmptyClipboard();
    const size_t byteCount = (end - start + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (data) {
        void* destination = GlobalLock(data);
        if (destination) {
            std::memcpy(destination, state.text.data() + start, (end - start) * sizeof(wchar_t));
            static_cast<wchar_t*>(destination)[end - start] = L'\0';
            GlobalUnlock(data);
            SetClipboardData(CF_UNICODETEXT, data);
            data = nullptr;
        }
    }
    if (data) {
        GlobalFree(data);
    }
    CloseClipboard();
}

std::wstring BeanTextBoxClipboardText()
{
    std::wstring value;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr)) {
        return value;
    }
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const auto* source = static_cast<const wchar_t*>(GlobalLock(data));
        if (source) {
            value = source;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return value;
}

int BeanTextBoxMeasureWidth(HDC dc, const std::wstring& text, size_t length)
{
    if (!dc || length == 0) {
        return 0;
    }
    SIZE size{};
    GetTextExtentPoint32W(dc, text.data(), static_cast<int>(length), &size);
    return size.cx;
}

size_t BeanTextBoxHitTest(HWND hwnd, BeanTextBoxState& state, int x)
{
    HDC dc = GetDC(hwnd);
    if (!dc) {
        return state.text.size();
    }
    HFONT font = state.font ? state.font : gTheme.uiFont;
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    const int contentLeft = 8;
    const int contentRight = (std::max)(contentLeft, static_cast<int>(clientRect.right) - 8);
    const int contentX = (std::clamp)(x, contentLeft, contentRight) - contentLeft + state.scrollX;
    size_t result = state.text.size();
    for (size_t index = 0; index < state.text.size(); ++index) {
        const int left = BeanTextBoxMeasureWidth(dc, state.text, index);
        const int right = BeanTextBoxMeasureWidth(dc, state.text, index + 1);
        if (contentX < (left + right) / 2) {
            result = index;
            break;
        }
    }
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
    ReleaseDC(hwnd, dc);
    return result;
}

int BeanTextBoxRegisterClick(BeanTextBoxState& state, int x, int y)
{
    const DWORD now = GetTickCount();
    const bool sameSequence = state.clickCount > 0
        && now - state.lastClickTime <= GetDoubleClickTime()
        && std::abs(x - state.lastClickPoint.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
        && std::abs(y - state.lastClickPoint.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
    state.clickCount = sameSequence && state.clickCount < 3 ? state.clickCount + 1 : 1;
    state.lastClickTime = now;
    state.lastClickPoint = POINT{x, y};
    return state.clickCount;
}

bool BeanTextBoxIsWordCharacter(wchar_t character)
{
    return (character >= L'A' && character <= L'Z')
        || (character >= L'a' && character <= L'z')
        || (character >= L'0' && character <= L'9')
        || character == L'_';
}

void BeanTextBoxSelectWord(BeanTextBoxState& state, size_t hit)
{
    if (state.text.empty()) {
        state.anchor = state.caret = 0;
        return;
    }
    size_t position = (std::min)(hit, state.text.size() - 1);
    const bool wordCharacter = BeanTextBoxIsWordCharacter(state.text[position]);
    size_t start = position;
    size_t end = position + 1;
    while (start > 0 && BeanTextBoxIsWordCharacter(state.text[start - 1]) == wordCharacter) {
        --start;
    }
    while (end < state.text.size() && BeanTextBoxIsWordCharacter(state.text[end]) == wordCharacter) {
        ++end;
    }
    state.anchor = start;
    state.caret = end;
}

void BeanTextBoxEnsureCaretVisible(HWND hwnd, BeanTextBoxState& state, HDC dc)
{
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    const int contentLeft = 8;
    const int contentRight = (std::max)(contentLeft, static_cast<int>(clientRect.right) - 8);
    const int caretX = contentLeft + BeanTextBoxMeasureWidth(dc, state.text, state.caret) - state.scrollX;
    if (caretX < contentLeft) {
        state.scrollX -= contentLeft - caretX;
    } else if (caretX > contentRight) {
        state.scrollX += caretX - contentRight;
    }
    const int textWidth = BeanTextBoxMeasureWidth(dc, state.text, state.text.size());
    const int maxScroll = (std::max)(0, textWidth - (contentRight - contentLeft));
    state.scrollX = (std::clamp)(state.scrollX, 0, maxScroll);
}

struct BeanTextBoxMultilineMetrics {
    int trackTop = 1;
    int trackBottom = 1;
    int thumbHeight = 0;
    int thumbTravel = 0;
    int thumbTop = 1;
    int maxScroll = 0;
};

BeanTextBoxMultilineMetrics GetBeanTextBoxMultilineMetrics(HWND hwnd, BeanTextBoxState& state)
{
    BeanTextBoxMultilineMetrics result;
    HDC dc = GetDC(hwnd);
    if (!dc) {
        return result;
    }
    const HFONT font = state.font ? state.font : gTheme.uiFont;
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int textLeft = 8;
    const int textRight = (std::max)(textLeft, static_cast<int>(clientRect.right) - 14 - 2);
    const int textTop = 4;
    const int textBottom = (std::max)(textTop, static_cast<int>(clientRect.bottom) - 4);
    RECT measuredRect{textLeft, textTop, textRight, textTop};
    DrawTextW(
        dc,
        state.text.c_str(),
        static_cast<int>(state.text.size()),
        &measuredRect,
        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_CALCRECT);
    const int textHeight = (std::max)(metrics.tmHeight, measuredRect.bottom - measuredRect.top);
    const int viewportHeight = (std::max)(1, textBottom - textTop);
    result.maxScroll = (std::max)(0, textHeight - viewportHeight);
    result.trackBottom = (std::max)(result.trackTop + 1, static_cast<int>(clientRect.bottom) - 1);
    const int trackHeight = result.trackBottom - result.trackTop;
    result.thumbHeight = (std::max)(18, trackHeight * viewportHeight / textHeight);
    result.thumbHeight = (std::min)(result.thumbHeight, trackHeight);
    result.thumbTravel = (std::max)(0, trackHeight - result.thumbHeight);
    result.thumbTop = result.trackTop
        + (result.maxScroll > 0 ? result.thumbTravel * state.scrollY / result.maxScroll : 0);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
    ReleaseDC(hwnd, dc);
    return result;
}

void DrawBeanTextBox(HWND hwnd, HDC dc)
{
    BeanTextBoxState* state = GetBeanTextBoxState(hwnd);
    if (!state || !dc) {
        return;
    }
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    HBRUSH backgroundBrush = CreateSolidBrush(
        IsWindowEnabled(hwnd) ? kColorInputBg : kThemeColors.controlDisabledBackground);
    if (backgroundBrush) {
        FillRect(dc, &clientRect, backgroundBrush);
        DeleteObject(backgroundBrush);
    }

    const COLORREF borderColor = !IsWindowEnabled(hwnd)
        ? kThemeColors.controlDisabledBorder
        : (GetFocus() == hwnd ? kThemeColors.accentBright : kColorInputBorder);
    HBRUSH borderBrush = CreateSolidBrush(borderColor);
    if (borderBrush) {
        RECT top{0, 0, clientRect.right, 1};
        RECT bottom{0, clientRect.bottom - 1, clientRect.right, clientRect.bottom};
        RECT left{0, 0, 1, clientRect.bottom};
        RECT right{clientRect.right - 1, 0, clientRect.right, clientRect.bottom};
        FillRect(dc, &top, borderBrush);
        FillRect(dc, &bottom, borderBrush);
        FillRect(dc, &left, borderBrush);
        FillRect(dc, &right, borderBrush);
        DeleteObject(borderBrush);
    }

    const int contentLeft = 8;
    const int contentRight = (std::max)(contentLeft, static_cast<int>(clientRect.right) - 8);
    const int contentTop = 1;
    const int contentBottom = (std::max)(contentTop, static_cast<int>(clientRect.bottom) - 1);
    const HFONT font = state->font ? state->font : gTheme.uiFont;
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    if (state->multiline) {
        const int scrollbarWidth = 14;
        const int textLeft = 8;
        const int textRight = (std::max)(textLeft, static_cast<int>(clientRect.right) - scrollbarWidth - 2);
        const int textTop = 4;
        const int textBottom = (std::max)(textTop, static_cast<int>(clientRect.bottom) - 4);
        RECT measuredRect{textLeft, textTop, textRight, textTop};
        DrawTextW(
            dc,
            state->text.c_str(),
            static_cast<int>(state->text.size()),
            &measuredRect,
            DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_CALCRECT);
        const int textHeight = (std::max)(metrics.tmHeight, measuredRect.bottom - measuredRect.top);
        const int viewportHeight = (std::max)(1, textBottom - textTop);
        const int maxScroll = (std::max)(0, textHeight - viewportHeight);
        state->scrollY = (std::clamp)(state->scrollY, 0, maxScroll);

        const int savedDc = SaveDC(dc);
        IntersectClipRect(dc, textLeft, textTop, textRight, textBottom);
        SetWindowOrgEx(dc, 0, state->scrollY, nullptr);
        RECT textRect{textLeft, textTop, textRight, textTop + textHeight};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(
            dc,
            IsWindowEnabled(hwnd) ? kColorTextPrimary : kThemeColors.controlDisabledText);
        DrawTextW(
            dc,
            state->text.c_str(),
            static_cast<int>(state->text.size()),
            &textRect,
            DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        RestoreDC(dc, savedDc);

        if (maxScroll > 0) {
            const int trackLeft = static_cast<int>(clientRect.right) - scrollbarWidth;
            const int trackTop = 1;
            const int trackBottom = (std::max)(trackTop + 1, static_cast<int>(clientRect.bottom) - 1);
            HBRUSH trackBrush = CreateSolidBrush(kThemeColors.scrollbarTrack);
            if (trackBrush) {
                RECT trackRect{trackLeft, trackTop, static_cast<int>(clientRect.right), trackBottom};
                FillRect(dc, &trackRect, trackBrush);
                DeleteObject(trackBrush);
            }
            const int trackHeight = trackBottom - trackTop;
            const int thumbHeight = (std::max)(18, trackHeight * viewportHeight / textHeight);
            const int thumbTravel = (std::max)(0, trackHeight - thumbHeight);
            const int thumbTop = trackTop + (maxScroll > 0 ? thumbTravel * state->scrollY / maxScroll : 0);
            HBRUSH thumbBrush = CreateSolidBrush(kThemeColors.scrollbarThumb);
            if (thumbBrush) {
                RECT thumbRect{trackLeft + 2, thumbTop, static_cast<int>(clientRect.right) - 2, thumbTop + thumbHeight};
                FillRect(dc, &thumbRect, thumbBrush);
                DeleteObject(thumbBrush);
            }
        }
        if (oldFont) {
            SelectObject(dc, oldFont);
        }
        return;
    }
    const int textTop = contentTop + (contentBottom - contentTop - metrics.tmHeight) / 2;
    BeanTextBoxEnsureCaretVisible(hwnd, *state, dc);

    const int savedDc = SaveDC(dc);
    IntersectClipRect(dc, contentLeft, contentTop, contentRight, contentBottom);
    const size_t selectionStart = BeanTextBoxSelectionStart(*state);
    const size_t selectionEnd = BeanTextBoxSelectionEnd(*state);
    if (selectionStart != selectionEnd) {
        const int selectionLeft = contentLeft
            + BeanTextBoxMeasureWidth(dc, state->text, selectionStart)
            - state->scrollX;
        const int selectionRight = contentLeft
            + BeanTextBoxMeasureWidth(dc, state->text, selectionEnd)
            - state->scrollX;
        HBRUSH selectionBrush = CreateSolidBrush(kColorListSelection);
        if (selectionBrush) {
            RECT selectionRect{selectionLeft, textTop, selectionRight, textTop + metrics.tmHeight};
            FillRect(dc, &selectionRect, selectionBrush);
            DeleteObject(selectionBrush);
        }
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(
        dc,
        IsWindowEnabled(hwnd) ? kColorTextPrimary : kThemeColors.controlDisabledText);
    const int textX = contentLeft - state->scrollX;
    if (!state->text.empty()) {
        TextOutW(dc, textX, textTop, state->text.c_str(), static_cast<int>(state->text.size()));
    }
    if (GetFocus() == hwnd && state->caretVisible) {
        const int caretX = contentLeft
            + BeanTextBoxMeasureWidth(dc, state->text, state->caret)
            - state->scrollX;
        HPEN caretPen = CreatePen(PS_SOLID, 1, kColorTextPrimary);
        HGDIOBJ oldPen = caretPen ? SelectObject(dc, caretPen) : nullptr;
        MoveToEx(dc, caretX, textTop, nullptr);
        LineTo(dc, caretX, textTop + metrics.tmHeight);
        if (oldPen) {
            SelectObject(dc, oldPen);
        }
        if (caretPen) {
            DeleteObject(caretPen);
        }
    }
    RestoreDC(dc, savedDc);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
}

void PaintBeanTextBoxBuffered(HWND hwnd, HDC target, const PAINTSTRUCT& paint)
{
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    const int width = static_cast<int>(clientRect.right);
    const int height = static_cast<int>(clientRect.bottom);
    HDC buffer = (width > 0 && height > 0) ? CreateCompatibleDC(target) : nullptr;
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, width, height) : nullptr;
    if (!buffer || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        DrawBeanTextBox(hwnd, target);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
    DrawBeanTextBox(hwnd, buffer);
    const RECT& dirty = paint.rcPaint;
    BitBlt(
        target,
        dirty.left,
        dirty.top,
        dirty.right - dirty.left,
        dirty.bottom - dirty.top,
        buffer,
        dirty.left,
        dirty.top,
        SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

void BeanTextBoxInvalidate(HWND hwnd)
{
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK BeanTextBoxWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    BeanTextBoxState* state = GetBeanTextBoxState(hwnd);
    if (!state) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintBeanTextBoxBuffered(hwnd, dc, paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        DrawBeanTextBox(hwnd, reinterpret_cast<HDC>(wParam));
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_WANTARROWS;
    case WM_GETTEXTLENGTH:
        return static_cast<LRESULT>(state->text.size());
    case WM_GETTEXT: {
        const size_t capacity = static_cast<size_t>(wParam);
        if (capacity == 0 || !lParam) {
            return 0;
        }
        const size_t length = (std::min)(state->text.size(), capacity - 1);
        std::memcpy(reinterpret_cast<void*>(lParam), state->text.data(), length * sizeof(wchar_t));
        reinterpret_cast<wchar_t*>(lParam)[length] = L'\0';
        return static_cast<LRESULT>(length);
    }
    case WM_SETTEXT:
        state->text = lParam ? reinterpret_cast<const wchar_t*>(lParam) : L"";
        state->caret = state->anchor = state->text.size();
        state->scrollX = 0;
        state->scrollY = 0;
        state->hasUndo = false;
        BeanTextBoxInvalidate(hwnd);
        return TRUE;
    case WM_SETFONT:
        state->font = reinterpret_cast<HFONT>(wParam);
        if (lParam) {
            BeanTextBoxInvalidate(hwnd);
            UpdateWindow(hwnd);
        }
        return 0;
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(state->font);
    case EM_SETSEL: {
        const size_t length = state->text.size();
        const size_t start = wParam == static_cast<WPARAM>(-1)
            ? length
            : (std::min)(static_cast<size_t>(wParam), length);
        const size_t end = lParam == static_cast<LPARAM>(-1)
            ? length
            : (std::min)(static_cast<size_t>(lParam), length);
        state->anchor = start;
        state->caret = end;
        state->caretVisible = true;
        BeanTextBoxInvalidate(hwnd);
        return 0;
    }
    case EM_GETSEL: {
        if (wParam) *reinterpret_cast<DWORD*>(wParam) = static_cast<DWORD>(state->anchor);
        if (lParam) *reinterpret_cast<DWORD*>(lParam) = static_cast<DWORD>(state->caret);
        return 0;
    }
    case EM_REPLACESEL: {
        const wchar_t* replacement = lParam ? reinterpret_cast<const wchar_t*>(lParam) : L"";
        if (BeanTextBoxReplaceSelection(*state, replacement)) {
            NotifyBeanTextBoxChanged(hwnd);
            BeanTextBoxInvalidate(hwnd);
        }
        return 0;
    }
    case EM_SCROLLCARET:
        if (state->multiline) {
            state->scrollY = GetBeanTextBoxMultilineMetrics(hwnd, *state).maxScroll;
            BeanTextBoxInvalidate(hwnd);
        }
        return 0;
    case WM_COPY:
        BeanTextBoxCopySelection(hwnd, *state);
        return 0;
    case WM_CUT:
        if (state->readOnly) {
            return 0;
        }
        BeanTextBoxCopySelection(hwnd, *state);
        if (BeanTextBoxReplaceSelection(*state, L"")) {
            NotifyBeanTextBoxChanged(hwnd);
            BeanTextBoxInvalidate(hwnd);
        }
        return 0;
    case WM_CLEAR:
        if (state->readOnly) {
            return 0;
        }
        if (BeanTextBoxReplaceSelection(*state, L"")) {
            NotifyBeanTextBoxChanged(hwnd);
            BeanTextBoxInvalidate(hwnd);
        }
        return 0;
    case WM_PASTE: {
        if (state->readOnly) {
            return 0;
        }
        if (BeanTextBoxReplaceSelection(*state, BeanTextBoxClipboardText())) {
            NotifyBeanTextBoxChanged(hwnd);
            BeanTextBoxInvalidate(hwnd);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (state->multiline) {
            const int wheelDelta = static_cast<short>(HIWORD(wParam));
            const BeanTextBoxMultilineMetrics metrics = GetBeanTextBoxMultilineMetrics(hwnd, *state);
            state->scrollY = (std::clamp)(
                state->scrollY - wheelDelta / 2,
                0,
                metrics.maxScroll);
            BeanTextBoxInvalidate(hwnd);
            return 0;
        }
        break;
    case WM_SETFOCUS:
        state->caretVisible = true;
        SetTimer(hwnd, kBeanTextBoxCaretTimerId, 500, nullptr);
        BeanTextBoxInvalidate(hwnd);
        return 0;
    case WM_KILLFOCUS:
        KillTimer(hwnd, kBeanTextBoxCaretTimerId);
        state->selecting = false;
        BeanTextBoxInvalidate(hwnd);
        return 0;
    case WM_TIMER:
        if (wParam == kBeanTextBoxCaretTimerId) {
            state->caretVisible = !state->caretVisible;
            BeanTextBoxInvalidate(hwnd);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: {
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        if (state->multiline) {
            RECT clientRect{};
            GetClientRect(hwnd, &clientRect);
            const BeanTextBoxMultilineMetrics metrics = GetBeanTextBoxMultilineMetrics(hwnd, *state);
            const int scrollbarLeft = static_cast<int>(clientRect.right) - 14;
            if (x >= scrollbarLeft && metrics.maxScroll > 0) {
                if (y >= metrics.thumbTop && y < metrics.thumbTop + metrics.thumbHeight) {
                    state->draggingVerticalScrollbar = true;
                    state->verticalScrollDragOffset = y - metrics.thumbTop;
                    SetCapture(hwnd);
                } else {
                    const int page = (std::max)(1, static_cast<int>(clientRect.bottom) - 8);
                    state->scrollY += y < metrics.thumbTop ? -page : page;
                    state->scrollY = (std::clamp)(state->scrollY, 0, metrics.maxScroll);
                    BeanTextBoxInvalidate(hwnd);
                }
                return 0;
            }
        }
        const int clickCount = BeanTextBoxRegisterClick(*state, x, y);
        SetFocus(hwnd);
        const size_t hit = BeanTextBoxHitTest(hwnd, *state, x);
        if (clickCount == 2) {
            BeanTextBoxSelectWord(*state, hit);
            state->selecting = false;
            state->caretVisible = true;
            BeanTextBoxInvalidate(hwnd);
            return 0;
        }
        if (clickCount >= 3) {
            state->anchor = 0;
            state->caret = state->text.size();
            state->selecting = false;
            state->caretVisible = true;
            BeanTextBoxInvalidate(hwnd);
            return 0;
        }
        if ((GetKeyState(VK_SHIFT) & 0x8000) == 0) {
            state->anchor = hit;
        }
        state->caret = hit;
        state->selecting = true;
        state->caretVisible = true;
        SetCapture(hwnd);
        BeanTextBoxInvalidate(hwnd);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        const int clickCount = BeanTextBoxRegisterClick(*state, x, y);
        SetFocus(hwnd);
        const size_t hit = BeanTextBoxHitTest(hwnd, *state, x);
        if (clickCount >= 3) {
            state->anchor = 0;
            state->caret = state->text.size();
        } else {
            BeanTextBoxSelectWord(*state, hit);
        }
        state->selecting = false;
        state->caretVisible = true;
        BeanTextBoxInvalidate(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (state->draggingVerticalScrollbar && GetCapture() == hwnd) {
            const BeanTextBoxMultilineMetrics metrics = GetBeanTextBoxMultilineMetrics(hwnd, *state);
            const int trackPosition = static_cast<int>(static_cast<short>(HIWORD(lParam)))
                - state->verticalScrollDragOffset
                - metrics.trackTop;
            const int clampedPosition = (std::clamp)(trackPosition, 0, metrics.thumbTravel);
            state->scrollY = metrics.thumbTravel > 0
                ? clampedPosition * metrics.maxScroll / metrics.thumbTravel
                : 0;
            BeanTextBoxInvalidate(hwnd);
            return 0;
        }
        if (state->selecting && GetCapture() == hwnd) {
            const size_t hit = BeanTextBoxHitTest(
                hwnd,
                *state,
                static_cast<int>(static_cast<short>(LOWORD(lParam))));
            state->caret = hit;
            state->caretVisible = true;
            BeanTextBoxInvalidate(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state->draggingVerticalScrollbar) {
            state->draggingVerticalScrollbar = false;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            BeanTextBoxInvalidate(hwnd);
            return 0;
        }
        if (state->selecting) {
            state->selecting = false;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            BeanTextBoxInvalidate(hwnd);
        }
        return 0;
    case WM_CAPTURECHANGED:
        state->draggingVerticalScrollbar = false;
        state->selecting = false;
        BeanTextBoxInvalidate(hwnd);
        return 0;
    case WM_KEYDOWN: {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (control) {
            if (wParam == 'A') {
                state->anchor = 0;
                state->caret = state->text.size();
                BeanTextBoxInvalidate(hwnd);
                return 0;
            }
            if (wParam == 'C') {
                BeanTextBoxCopySelection(hwnd, *state);
                return 0;
            }
            if (wParam == 'X') {
                SendMessageW(hwnd, WM_CUT, 0, 0);
                return 0;
            }
            if (wParam == 'V') {
                SendMessageW(hwnd, WM_PASTE, 0, 0);
                return 0;
            }
            if (wParam == 'Z') {
                if (state->readOnly) {
                    return 0;
                }
                if (state->hasUndo) {
                    std::swap(state->text, state->undoText);
                    std::swap(state->caret, state->undoCaret);
                    std::swap(state->anchor, state->undoAnchor);
                    state->hasUndo = false;
                    NotifyBeanTextBoxChanged(hwnd);
                    BeanTextBoxInvalidate(hwnd);
                }
                return 0;
            }
        }
        if (state->readOnly) {
            return 0;
        }
        if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_HOME || wParam == VK_END) {
            const size_t selectionStart = BeanTextBoxSelectionStart(*state);
            const size_t selectionEnd = BeanTextBoxSelectionEnd(*state);
            if (!shift && selectionStart != selectionEnd) {
                state->caret = wParam == VK_LEFT || wParam == VK_HOME ? selectionStart : selectionEnd;
                state->anchor = state->caret;
            } else if (wParam == VK_LEFT) {
                if (state->caret > 0) --state->caret;
            } else if (wParam == VK_RIGHT) {
                if (state->caret < state->text.size()) ++state->caret;
            } else if (wParam == VK_HOME) {
                state->caret = 0;
            } else {
                state->caret = state->text.size();
            }
            if (!shift) {
                state->anchor = state->caret;
            }
            state->caretVisible = true;
            BeanTextBoxInvalidate(hwnd);
            return 0;
        }
        if (wParam == VK_BACK || wParam == VK_DELETE) {
            bool changed = false;
            if (BeanTextBoxSelectionStart(*state) != BeanTextBoxSelectionEnd(*state)) {
                changed = BeanTextBoxReplaceSelection(*state, L"");
            } else if (wParam == VK_BACK && state->caret > 0) {
                state->anchor = state->caret - 1;
                changed = BeanTextBoxReplaceSelection(*state, L"");
            } else if (wParam == VK_DELETE && state->caret < state->text.size()) {
                state->anchor = state->caret + 1;
                changed = BeanTextBoxReplaceSelection(*state, L"");
            }
            state->caretVisible = true;
            if (changed) {
                NotifyBeanTextBoxChanged(hwnd);
            }
            BeanTextBoxInvalidate(hwnd);
            return 0;
        }
        break;
    }
    case WM_CHAR:
        if (state->readOnly) {
            return 0;
        }
        if (wParam == 1) {
            state->anchor = 0;
            state->caret = state->text.size();
            BeanTextBoxInvalidate(hwnd);
            return 0;
        }
        if (wParam >= 32 && (!state->numberOnly || (wParam >= L'0' && wParam <= L'9'))) {
            if (BeanTextBoxReplaceSelection(*state, std::wstring(1, static_cast<wchar_t>(wParam)))) {
                state->caretVisible = true;
                NotifyBeanTextBoxChanged(hwnd);
                BeanTextBoxInvalidate(hwnd);
            }
        }
        return 0;
    case WM_NCDESTROY:
        KillTimer(hwnd, kBeanTextBoxCaretTimerId);
        gBeanTextBoxes.erase(hwnd);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureBeanTextBoxClass()
{
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpfnWndProc = BeanTextBoxWndProc;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_IBEAM));
    windowClass.lpszClassName = kBeanTextBoxClassName;
    if (!RegisterClassExW(&windowClass)) {
        return false;
    }
    registered = true;
    return true;
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
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const POINT point{
            static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
            static_cast<LONG>(static_cast<short>(HIWORD(lParam)))};
        const bool inside = PtInRect(&clientRect, point) != FALSE;
        const bool hoverChanged = inside
            ? gHoveredModernToggle != hwnd
            : gHoveredModernToggle == hwnd;
        if (inside && gHoveredModernToggle != hwnd) {
            HWND previous = gHoveredModernToggle;
            gHoveredModernToggle = hwnd;
            if (previous && IsWindow(previous)) {
                InvalidateRect(previous, nullptr, FALSE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (!inside && gHoveredModernToggle == hwnd) {
            gHoveredModernToggle = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (hoverChanged) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        UpdateWindow(hwnd);
        return result;
    }
    case WM_MOUSELEAVE:
        if (gHoveredModernToggle == hwnd) {
            gHoveredModernToggle = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        {
            LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
            return result;
        }
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

constexpr int kCustomComboPopupScrollbarWidth = 14;
constexpr int kCustomComboPopupRowHeight = 28;
constexpr UINT kCustomComboPopupDragTimerId = 1;
constexpr DWORD kCustomComboPopupTypeAheadTimeoutMs = 1000;

enum class CustomComboPopupCloseReason {
    Commit,
    Cancel,
    Dismiss,
    Destroy,
};

struct CustomComboPopupState {
    HWND popup = nullptr;
    HWND combo = nullptr;
    int originalIndex = -1;
    int highlightedIndex = -1;
    int scrollOffset = 0;
    int visibleRowLimit = 8;
    bool draggingScrollbar = false;
    bool releasingCapture = false;
    bool closing = false;
    int scrollbarDragOffset = 0;
    std::wstring typeAhead;
    DWORD typeAheadTick = 0;
};

std::unique_ptr<CustomComboPopupState> gCustomComboPopup;

CustomComboPopupState* GetCustomComboPopupState(HWND popup)
{
    if (!popup || !gCustomComboPopup || gCustomComboPopup->popup != popup) {
        return nullptr;
    }
    return gCustomComboPopup.get();
}

HWND GetCustomComboForPopup(HWND popup)
{
    const CustomComboPopupState* state = GetCustomComboPopupState(popup);
    return state ? state->combo : nullptr;
}

bool IsStyledComboIdInternal(int controlId)
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

bool IsCustomComboControl(HWND combo)
{
    return combo && IsStyledComboIdInternal(GetDlgCtrlID(combo));
}

int CustomComboPopupRowLimit(HWND combo, int itemCount)
{
    int rowLimit = 8;
    switch (GetDlgCtrlID(combo)) {
    case IDC_ENCODER_COMBO:
    case IDC_PRESET_COMBO:
    case IDC_MICROPHONE_COMBO:
    case IDC_RECORDING_RESOLUTION_COMBO:
        rowLimit = 5;
        break;
    case IDC_CONTAINER_COMBO:
        rowLimit = 2;
        break;
    case IDC_CHAT_BLOCKER_ANCHOR_COMBO:
        rowLimit = 4;
        break;
    case IDC_CHAT_BLOCKER_IMAGE_COMBO:
        rowLimit = 6;
        break;
    case IDC_YOUTUBE_PRIVACY_COMBO:
        rowLimit = 3;
        break;
    case IDC_CUSTOMIZE_THEME_COMBO:
        rowLimit = static_cast<int>(kThemeDefinitions.size());
        break;
    case IDC_CLIPS_SOURCE_COMBO:
    default:
        break;
    }
    return (std::max)(1, (std::min)(rowLimit, (std::max)(1, itemCount)));
}

struct CustomComboPopupMetrics {
    RECT clientRect{};
    int itemCount = 0;
    int visibleRows = 0;
    int maxOffset = 0;
    int contentRight = 0;
    int thumbHeight = 0;
    int travel = 0;
    int thumbTop = 0;
    bool hasScrollbar = false;
};

bool GetCustomComboPopupMetrics(HWND popup, CustomComboPopupMetrics& metrics)
{
    const CustomComboPopupState* state = GetCustomComboPopupState(popup);
    if (!state || !state->combo || !GetClientRect(popup, &metrics.clientRect)) {
        return false;
    }
    metrics.itemCount = (std::max)(
        0,
        static_cast<int>(SendMessageW(state->combo, CB_GETCOUNT, 0, 0)));
    const int contentHeight = (std::max)(
        1,
        static_cast<int>(metrics.clientRect.bottom) - 2);
    metrics.visibleRows = (std::max)(
        1,
        (std::min)(
            state->visibleRowLimit,
            (contentHeight + kCustomComboPopupRowHeight - 1) / kCustomComboPopupRowHeight));
    metrics.maxOffset = (std::max)(0, metrics.itemCount - metrics.visibleRows);
    metrics.hasScrollbar = metrics.maxOffset > 0;
    metrics.contentRight = metrics.hasScrollbar
        ? (std::max)(1, static_cast<int>(metrics.clientRect.right) - kCustomComboPopupScrollbarWidth)
        : static_cast<int>(metrics.clientRect.right);
    if (!metrics.hasScrollbar) {
        return true;
    }
    const int trackHeight = (std::max)(1, static_cast<int>(metrics.clientRect.bottom));
    metrics.thumbHeight = (std::max)(
        24,
        (std::min)(
            trackHeight,
            trackHeight * metrics.visibleRows / (std::max)(1, metrics.itemCount)));
    metrics.travel = (std::max)(1, trackHeight - metrics.thumbHeight);
    metrics.thumbTop = metrics.maxOffset > 0
        ? metrics.travel * state->scrollOffset / metrics.maxOffset
        : 0;
    return true;
}

int CustomComboPopupVisibleRows(HWND popup)
{
    CustomComboPopupMetrics metrics{};
    return GetCustomComboPopupMetrics(popup, metrics) ? metrics.visibleRows : 1;
}

int CustomComboPopupMaxOffset(HWND popup)
{
    CustomComboPopupMetrics metrics{};
    return GetCustomComboPopupMetrics(popup, metrics) ? metrics.maxOffset : 0;
}

int CustomComboPopupRowAtPoint(
    const CustomComboPopupMetrics& metrics,
    const CustomComboPopupState& state,
    int x,
    int y)
{
    if (x < 1 || x >= metrics.contentRight || y < 1) {
        return -1;
    }
    const int row = (y - 1) / kCustomComboPopupRowHeight;
    if (row < 0 || row >= metrics.visibleRows) {
        return -1;
    }
    const int index = state.scrollOffset + row;
    return index >= 0 && index < metrics.itemCount ? index : -1;
}

std::wstring GetComboItemText(HWND combo, int index)
{
    const int length = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, index, 0));
    if (length < 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(length));
    return text;
}

void NotifyCustomCombo(HWND combo, WORD notification)
{
    if (!combo) {
        return;
    }
    HWND parent = GetParent(combo);
    if (parent) {
        SendMessageW(
            parent,
            WM_COMMAND,
            MAKEWPARAM(static_cast<WORD>(GetDlgCtrlID(combo)), notification),
            reinterpret_cast<LPARAM>(combo));
    }
}

void EnsureCustomComboPopupClass();
void CloseCustomComboPopup(HWND popup, CustomComboPopupCloseReason reason);
void CommitCustomComboPopupSelection(HWND popup, int index);
void InvalidateCustomComboPopup(HWND popup);
void StopCustomComboPopupDrag(HWND popup)
{
    CustomComboPopupState* state = GetCustomComboPopupState(popup);
    if (!state) {
        return;
    }
    state->draggingScrollbar = false;
    state->scrollbarDragOffset = 0;
    KillTimer(popup, kCustomComboPopupDragTimerId);
    if (GetCapture() == popup) {
        state->releasingCapture = true;
        ReleaseCapture();
        state->releasingCapture = false;
    }
    InvalidateCustomComboPopup(popup);
}

void UpdateCustomComboPopupDrag(HWND popup)
{
    CustomComboPopupState* state = GetCustomComboPopupState(popup);
    if (!state || !state->draggingScrollbar) {
        return;
    }
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
        StopCustomComboPopupDrag(popup);
        return;
    }
    CustomComboPopupMetrics metrics{};
    if (!GetCustomComboPopupMetrics(popup, metrics) || metrics.maxOffset <= 0) {
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(popup, &cursor);
    state->scrollOffset = (std::clamp)(
        (static_cast<int>(cursor.y) - state->scrollbarDragOffset)
            * metrics.maxOffset / metrics.travel,
        0,
        metrics.maxOffset);
    InvalidateCustomComboPopup(popup);
}

void EnsureCustomComboPopupHighlightVisible(HWND popup)
{
    CustomComboPopupState* state = GetCustomComboPopupState(popup);
    if (!state) {
        return;
    }
    CustomComboPopupMetrics metrics{};
    if (!GetCustomComboPopupMetrics(popup, metrics)) {
        return;
    }
    const int visibleRows = metrics.visibleRows;
    const int maxOffset = metrics.maxOffset;
    if (state->highlightedIndex >= 0) {
        if (state->highlightedIndex < state->scrollOffset) {
            state->scrollOffset = state->highlightedIndex;
        } else if (state->highlightedIndex >= state->scrollOffset + visibleRows) {
            state->scrollOffset = state->highlightedIndex - visibleRows + 1;
        }
    }
    state->scrollOffset = (std::clamp)(state->scrollOffset, 0, maxOffset);
}

void DrawCustomComboPopup(HWND popup, HDC dc)
{
    CustomComboPopupState* state = GetCustomComboPopupState(popup);
    if (!state || !dc) {
        return;
    }
    CustomComboPopupMetrics metrics{};
    if (!GetCustomComboPopupMetrics(popup, metrics)) {
        return;
    }
    state->scrollOffset = (std::clamp)(state->scrollOffset, 0, metrics.maxOffset);

    HBRUSH backgroundBrush = CreateSolidBrush(kColorInputBg);
    if (backgroundBrush) {
        FillRect(dc, &metrics.clientRect, backgroundBrush);
        DeleteObject(backgroundBrush);
    }

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = gTheme.uiFont ? SelectObject(dc, gTheme.uiFont) : nullptr;
    for (int row = 0; row < metrics.visibleRows; ++row) {
        const int itemIndex = state->scrollOffset + row;
        if (itemIndex >= metrics.itemCount) {
            break;
        }
        RECT rowRect{
            1,
            1 + row * kCustomComboPopupRowHeight,
            metrics.contentRight,
            1 + (row + 1) * kCustomComboPopupRowHeight};
        const bool highlighted = itemIndex == state->highlightedIndex;
        HBRUSH rowBrush = CreateSolidBrush(
            highlighted ? kThemeColors.dropdownHoverBackground : kColorInputBg);
        if (rowBrush) {
            FillRect(dc, &rowRect, rowBrush);
            DeleteObject(rowBrush);
        }
        SetTextColor(dc, kColorTextPrimary);
        RECT textRect = rowRect;
        textRect.left += 10;
        textRect.right -= 10;
        DrawTextW(
            dc,
            GetComboItemText(state->combo, itemIndex).c_str(),
            -1,
            &textRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (oldFont) {
        SelectObject(dc, oldFont);
    }

    if (metrics.hasScrollbar) {
        HBRUSH trackBrush = CreateSolidBrush(kThemeColors.scrollbarTrack);
        if (trackBrush) {
            RECT trackRect{
                metrics.contentRight,
                0,
                metrics.clientRect.right,
                metrics.clientRect.bottom};
            FillRect(dc, &trackRect, trackBrush);
            DeleteObject(trackBrush);
        }
        HBRUSH thumbBrush = CreateSolidBrush(
            state->draggingScrollbar
                ? kThemeColors.scrollbarThumbHover
                : kThemeColors.scrollbarThumb);
        if (thumbBrush) {
            RECT thumbRect{
                metrics.contentRight + 2,
                metrics.thumbTop + 2,
                metrics.clientRect.right - 2,
                metrics.thumbTop + metrics.thumbHeight - 2};
            FillRect(dc, &thumbRect, thumbBrush);
            DeleteObject(thumbBrush);
        }
    }

    HBRUSH borderBrush = CreateSolidBrush(kColorInputBorder);
    if (borderBrush) {
        RECT top{0, 0, metrics.clientRect.right, 1};
        RECT bottom{
            0,
            metrics.clientRect.bottom - 1,
            metrics.clientRect.right,
            metrics.clientRect.bottom};
        RECT left{0, 0, 1, metrics.clientRect.bottom};
        RECT right{
            metrics.clientRect.right - 1,
            0,
            metrics.clientRect.right,
            metrics.clientRect.bottom};
        FillRect(dc, &top, borderBrush);
        FillRect(dc, &bottom, borderBrush);
        FillRect(dc, &left, borderBrush);
        FillRect(dc, &right, borderBrush);
        DeleteObject(borderBrush);
    }
}

void PaintCustomComboPopupBuffered(HWND popup, HDC target, const PAINTSTRUCT& paint)
{
    RECT clientRect{};
    GetClientRect(popup, &clientRect);
    const int width = static_cast<int>(clientRect.right);
    const int height = static_cast<int>(clientRect.bottom);
    HDC buffer = (width > 0 && height > 0) ? CreateCompatibleDC(target) : nullptr;
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, width, height) : nullptr;
    if (!buffer || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        DrawCustomComboPopup(popup, target);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
    DrawCustomComboPopup(popup, buffer);
    const RECT& dirty = paint.rcPaint;
    BitBlt(
        target,
        dirty.left,
        dirty.top,
        dirty.right - dirty.left,
        dirty.bottom - dirty.top,
        buffer,
        dirty.left,
        dirty.top,
        SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

void SetCustomComboPopupHighlight(HWND popup, int index)
{
    CustomComboPopupState* state = GetCustomComboPopupState(popup);
    if (!state) {
        return;
    }
    const int count = static_cast<int>(SendMessageW(state->combo, CB_GETCOUNT, 0, 0));
    state->highlightedIndex = count > 0 ? (std::clamp)(index, 0, count - 1) : -1;
    EnsureCustomComboPopupHighlightVisible(popup);
    InvalidateCustomComboPopup(popup);
}

void CommitCustomComboPopupSelection(HWND popup, int index)
{
    CustomComboPopupState* state = GetCustomComboPopupState(popup);
    if (!state) {
        return;
    }
    const HWND combo = state->combo;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    const int previousIndex = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    const bool validIndex = index >= 0 && index < count;
    const bool selectionChanged = validIndex && index != previousIndex;
    if (selectionChanged) {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    }
    CloseCustomComboPopup(
        popup,
        validIndex
            ? CustomComboPopupCloseReason::Commit
            : CustomComboPopupCloseReason::Dismiss);
}

int FindCustomComboPopupPrefixMatch(HWND combo, int startIndex, const std::wstring& prefix)
{
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    if (count <= 0 || prefix.empty()) {
        return -1;
    }
    const int first = (std::max)(0, (std::min)(startIndex, count - 1));
    for (int offset = 0; offset < count; ++offset) {
        const int index = (first + offset) % count;
        const std::wstring text = GetComboItemText(combo, index);
        if (text.size() < prefix.size()) {
            continue;
        }
        bool matches = true;
        for (size_t character = 0; character < prefix.size(); ++character) {
            if (std::towlower(text[character]) != std::towlower(prefix[character])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return index;
        }
    }
    return -1;
}

bool MoveClosedCustomComboSelection(HWND combo, int direction)
{
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    if (count <= 0 || direction == 0) {
        return true;
    }
    const int current = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    const int next = current < 0
        ? (direction < 0 ? count - 1 : 0)
        : (std::clamp)(current + direction, 0, count - 1);
    if (next != current) {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(next), 0);
        NotifyCustomCombo(combo, CBN_SELCHANGE);
        RefreshModernCombo(combo);
    }
    return true;
}

void UpdateCustomComboPopupHighlightFromPoint(HWND popup, int x, int y)
{
    CustomComboPopupState* state = GetCustomComboPopupState(popup);
    CustomComboPopupMetrics metrics{};
    if (!state || !GetCustomComboPopupMetrics(popup, metrics)) {
        return;
    }
    const int index = CustomComboPopupRowAtPoint(metrics, *state, x, y);
    if (index >= 0) {
        SetCustomComboPopupHighlight(popup, index);
    } else if (state->highlightedIndex != -1) {
        state->highlightedIndex = -1;
        InvalidateCustomComboPopup(popup);
    }
}

bool HandleCustomComboPopupKey(HWND combo, WPARAM key)
{
    CustomComboPopupState* state = gCustomComboPopup
        && gCustomComboPopup->combo == combo
        ? gCustomComboPopup.get()
        : nullptr;
    if (!state) {
        return false;
    }
    const HWND popup = state->popup;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    if (key == VK_ESCAPE) {
        CloseCustomComboPopup(popup, CustomComboPopupCloseReason::Cancel);
        return true;
    }
    if (key == VK_F4 || (key == VK_UP && (GetKeyState(VK_MENU) & 0x8000) != 0)) {
        CloseCustomComboPopup(popup, CustomComboPopupCloseReason::Dismiss);
        return true;
    }
    if (key == VK_RETURN || key == VK_SPACE) {
        CommitCustomComboPopupSelection(popup, state->highlightedIndex);
        return true;
    }
    if (count <= 0) {
        return true;
    }
    int next = state->highlightedIndex < 0
        ? static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0))
        : state->highlightedIndex;
    switch (key) {
    case VK_UP: --next; break;
    case VK_DOWN: ++next; break;
    case VK_PRIOR: next -= CustomComboPopupVisibleRows(popup); break;
    case VK_NEXT: next += CustomComboPopupVisibleRows(popup); break;
    case VK_HOME: next = 0; break;
    case VK_END: next = count - 1; break;
    default: return false;
    }
    SetCustomComboPopupHighlight(popup, next);
    return true;
}

LRESULT CALLBACK CustomComboPopupWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    CustomComboPopupState* state = GetCustomComboPopupState(hwnd);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintCustomComboPopupBuffered(hwnd, dc, paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!state) return 0;
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        if (state->draggingScrollbar) {
            UpdateCustomComboPopupDrag(hwnd);
            return 0;
        }
        CustomComboPopupMetrics metrics{};
        if (!GetCustomComboPopupMetrics(hwnd, metrics)) {
            return 0;
        }
        if (metrics.hasScrollbar && x >= metrics.contentRight) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            if (state->highlightedIndex != -1) {
                state->highlightedIndex = -1;
                InvalidateCustomComboPopup(hwnd);
            }
            return 0;
        }
        UpdateCustomComboPopupHighlightFromPoint(hwnd, x, y);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (state && !state->draggingScrollbar) {
            state->highlightedIndex = -1;
            InvalidateCustomComboPopup(hwnd);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (state) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int lines = (std::max)(1, std::abs(delta) / WHEEL_DELTA);
            const int maxOffset = CustomComboPopupMaxOffset(hwnd);
            state->scrollOffset = (std::clamp)(
                state->scrollOffset + (delta > 0 ? -lines : lines),
                0,
                maxOffset);
            InvalidateCustomComboPopup(hwnd);
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(hwnd, &cursor);
            UpdateCustomComboPopupHighlightFromPoint(
                hwnd,
                static_cast<int>(cursor.x),
                static_cast<int>(cursor.y));
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (!state) return 0;
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        CustomComboPopupMetrics metrics{};
        if (!GetCustomComboPopupMetrics(hwnd, metrics)) {
            CloseCustomComboPopup(hwnd, CustomComboPopupCloseReason::Dismiss);
            return 0;
        }
        if (x < 0 || y < 0 || x >= metrics.clientRect.right || y >= metrics.clientRect.bottom) {
            CloseCustomComboPopup(hwnd, CustomComboPopupCloseReason::Dismiss);
            return 0;
        }
        if (metrics.hasScrollbar && x >= metrics.contentRight) {
            if (y >= metrics.thumbTop && y < metrics.thumbTop + metrics.thumbHeight) {
                state->draggingScrollbar = true;
                state->scrollbarDragOffset = y - metrics.thumbTop;
                SetCapture(hwnd);
                SetTimer(hwnd, kCustomComboPopupDragTimerId, 10, nullptr);
            } else {
                state->scrollOffset = (std::clamp)(
                    state->scrollOffset
                        + (y < metrics.thumbTop ? -metrics.visibleRows : metrics.visibleRows),
                    0,
                    metrics.maxOffset);
                InvalidateCustomComboPopup(hwnd);
            }
            return 0;
        }
        const int index = CustomComboPopupRowAtPoint(metrics, *state, x, y);
        if (index >= 0) {
            CommitCustomComboPopupSelection(hwnd, index);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (state && state->draggingScrollbar) {
            StopCustomComboPopupDrag(hwnd);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (state) {
            if (!state->releasingCapture) {
                if (state->draggingScrollbar
                    && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
                    SetCapture(hwnd);
                    SetTimer(hwnd, kCustomComboPopupDragTimerId, 10, nullptr);
                    InvalidateCustomComboPopup(hwnd);
                    return 0;
                }
                state->draggingScrollbar = false;
                state->scrollbarDragOffset = 0;
                KillTimer(hwnd, kCustomComboPopupDragTimerId);
                if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                    CloseCustomComboPopup(hwnd, CustomComboPopupCloseReason::Dismiss);
                } else {
                    InvalidateCustomComboPopup(hwnd);
                }
            }
        }
        return 0;
    case WM_TIMER:
        if (wParam == kCustomComboPopupDragTimerId) {
            UpdateCustomComboPopupDrag(hwnd);
            return 0;
        }
        break;
    case WM_ACTIVATEAPP:
        if (!wParam && state) {
            CloseCustomComboPopup(hwnd, CustomComboPopupCloseReason::Dismiss);
        }
        return 0;
    case WM_CANCELMODE:
        CloseCustomComboPopup(hwnd, CustomComboPopupCloseReason::Cancel);
        return 0;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
        CloseCustomComboPopup(hwnd, CustomComboPopupCloseReason::Dismiss);
        return 0;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCDESTROY:
        KillTimer(hwnd, kCustomComboPopupDragTimerId);
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void EnsureCustomComboPopupClass()
{
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpfnWndProc = CustomComboPopupWndProc;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
    windowClass.lpszClassName = L"BeanCustomComboPopup";
    windowClass.hbrBackground = nullptr;
    if (RegisterClassExW(&windowClass)) {
        registered = true;
    }
}

void OpenCustomComboPopup(HWND combo)
{
    if (!IsCustomComboControl(combo)) {
        return;
    }
    if (gCustomComboPopup) {
        CloseCustomComboPopup(
            gCustomComboPopup->popup,
            CustomComboPopupCloseReason::Dismiss);
    }
    EnsureCustomComboPopupClass();
    RECT comboRect{};
    if (!GetWindowRect(combo, &comboRect)) return;
    const int itemCount = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    if (itemCount <= 0) return;
    const int visibleRowLimit = CustomComboPopupRowLimit(combo, itemCount);
    const int popupHeight = kCustomComboPopupRowHeight * visibleRowLimit + 2;
    HMONITOR monitor = MonitorFromWindow(combo, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) {
        return;
    }
    const int workLeft = static_cast<int>(monitorInfo.rcWork.left);
    const int workRight = static_cast<int>(monitorInfo.rcWork.right);
    const int workTop = static_cast<int>(monitorInfo.rcWork.top);
    const int workBottom = static_cast<int>(monitorInfo.rcWork.bottom);
    const int popupWidth = (std::min)(
        (std::max)(120, static_cast<int>(comboRect.right - comboRect.left)),
        (std::max)(120, workRight - workLeft));
    int x = comboRect.left;
    int y = comboRect.bottom;
    if (y + popupHeight > monitorInfo.rcWork.bottom && comboRect.top - popupHeight >= monitorInfo.rcWork.top) {
        y = comboRect.top - popupHeight;
    }
    x = (std::max)(workLeft, (std::min)(x, workRight - popupWidth));
    y = (std::max)(workTop, (std::min)(y, workBottom - popupHeight));
    HWND owner = GetAncestor(combo, GA_ROOT);
    HWND popup = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"BeanCustomComboPopup",
        L"",
        WS_POPUP,
        x,
        y,
        popupWidth,
        popupHeight,
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (!popup) return;
    auto state = std::make_unique<CustomComboPopupState>();
    state->popup = popup;
    state->combo = combo;
    state->originalIndex = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    state->highlightedIndex = state->originalIndex;
    state->visibleRowLimit = visibleRowLimit;
    gCustomComboPopup = std::move(state);
    SetWindowLongPtrW(
        popup,
        GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(gCustomComboPopup.get()));
    NotifyCustomCombo(combo, CBN_DROPDOWN);
    if (!GetCustomComboPopupState(popup) || !IsWindow(popup)) {
        return;
    }
    CustomComboPopupState* currentState = GetCustomComboPopupState(popup);
    currentState->originalIndex = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    currentState->highlightedIndex = currentState->originalIndex;
    EnsureCustomComboPopupHighlightVisible(popup);
    SetWindowPos(
        popup,
        HWND_TOP,
        x,
        y,
        popupWidth,
        popupHeight,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetCapture(popup);
}

void CloseCustomComboPopup(HWND popup, CustomComboPopupCloseReason reason)
{
    CustomComboPopupState* state = GetCustomComboPopupState(popup);
    if (!state || state->closing) {
        return;
    }
    const HWND combo = state->combo;
    const int originalIndex = state->originalIndex;
    const bool commit = reason == CustomComboPopupCloseReason::Commit;
    const bool destroy = reason == CustomComboPopupCloseReason::Destroy;
    state->closing = true;
    KillTimer(popup, kCustomComboPopupDragTimerId);
    if (GetCapture() == popup) {
        state->releasingCapture = true;
        ReleaseCapture();
        state->releasingCapture = false;
    }
    DestroyWindow(popup);
    if (gCustomComboPopup && gCustomComboPopup->popup == popup) {
        gCustomComboPopup.reset();
    }
    if (!destroy && combo && IsWindow(combo)) {
        if (reason == CustomComboPopupCloseReason::Cancel
            && static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)) != originalIndex) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(originalIndex), 0);
        }
        NotifyCustomCombo(
            combo,
            commit ? CBN_SELENDOK : CBN_SELENDCANCEL);
        if (commit) {
            const int currentIndex = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
            if (currentIndex != originalIndex) {
                NotifyCustomCombo(combo, CBN_SELCHANGE);
            }
        }
        NotifyCustomCombo(combo, CBN_CLOSEUP);
    }
}

void InvalidateCustomComboPopup(HWND popup)
{
    if (popup) {
        InvalidateRect(popup, nullptr, FALSE);
    }
}

LRESULT CALLBACK ModernComboSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    const bool isCustomCombo = IsCustomComboControl(hwnd);
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
        if (message == WM_KILLFOCUS
            && gCustomComboPopup
            && gCustomComboPopup->combo == hwnd) {
            CloseCustomComboPopup(
                gCustomComboPopup->popup,
                CustomComboPopupCloseReason::Dismiss);
        }
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (message == CB_SETCURSEL
            && gCustomComboPopup
            && gCustomComboPopup->combo == hwnd) {
            CustomComboPopupState* state = gCustomComboPopup.get();
            if (state) {
                state->highlightedIndex = static_cast<int>(wParam);
                EnsureCustomComboPopupHighlightVisible(state->popup);
                InvalidateCustomComboPopup(state->popup);
            }
        }
        RefreshModernCombo(hwnd);
        return result;
    }
    case CB_GETDROPPEDSTATE:
        if (isCustomCombo) {
            return gCustomComboPopup && gCustomComboPopup->combo == hwnd;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        if (isCustomCombo) {
            if (IsWindowEnabled(hwnd) && GetFocus() != hwnd) {
                SetFocus(hwnd);
            }
            if (gCustomComboPopup && gCustomComboPopup->combo == hwnd) {
                CloseCustomComboPopup(
                    gCustomComboPopup->popup,
                    CustomComboPopupCloseReason::Dismiss);
            } else {
                OpenCustomComboPopup(hwnd);
            }
            return 0;
        }
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        RefreshModernCombo(hwnd);
        return result;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (isCustomCombo) {
            if (gCustomComboPopup && HandleCustomComboPopupKey(hwnd, wParam)) {
                return 0;
            }
            const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
            if (!gCustomComboPopup
                && !altDown
                && (wParam == VK_UP || wParam == VK_DOWN)) {
                MoveClosedCustomComboSelection(hwnd, wParam == VK_UP ? -1 : 1);
                return 0;
            }
            if (!gCustomComboPopup
                && (wParam == VK_SPACE
                    || wParam == VK_RETURN
                    || wParam == VK_F4
                    || (wParam == VK_DOWN && altDown))) {
                OpenCustomComboPopup(hwnd);
                return 0;
            }
        }
        break;
    case WM_CHAR:
        if (isCustomCombo && gCustomComboPopup && gCustomComboPopup->combo == hwnd
            && wParam >= 0x20 && wParam != 0x7F) {
            CustomComboPopupState* state = gCustomComboPopup.get();
            const DWORD now = GetTickCount();
            if (now - state->typeAheadTick > kCustomComboPopupTypeAheadTimeoutMs) {
                state->typeAhead.clear();
            }
            state->typeAhead.push_back(static_cast<wchar_t>(wParam));
            state->typeAheadTick = now;
            const int current = state->highlightedIndex >= 0
                ? state->highlightedIndex + 1
                : static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
            int match = FindCustomComboPopupPrefixMatch(hwnd, current, state->typeAhead);
            if (match < 0 && state->typeAhead.size() > 1) {
                state->typeAhead.assign(1, state->typeAhead.back());
                match = FindCustomComboPopupPrefixMatch(hwnd, current, state->typeAhead);
            }
            if (match >= 0) {
                SetCustomComboPopupHighlight(state->popup, match);
            }
            return 0;
        }
        break;
    case CB_SHOWDROPDOWN:
        if (isCustomCombo) {
            if (wParam) {
                OpenCustomComboPopup(hwnd);
            } else {
                if (gCustomComboPopup && gCustomComboPopup->combo == hwnd) {
                    CloseCustomComboPopup(
                        gCustomComboPopup->popup,
                        CustomComboPopupCloseReason::Dismiss);
                }
            }
            return gCustomComboPopup && gCustomComboPopup->combo == hwnd;
        }
        break;
    case WM_LBUTTONUP: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        RefreshModernCombo(hwnd);
        return result;
    }
    case WM_NCDESTROY:
        if (gCustomComboPopup && gCustomComboPopup->combo == hwnd) {
            CloseCustomComboPopup(
                gCustomComboPopup->popup,
                CustomComboPopupCloseReason::Destroy);
        }
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

std::unordered_map<HWND, HWND> gThemedScrollbarByHost;
constexpr int kThemedScrollbarWidth = 14;

bool IsListViewHost(HWND hwnd)
{
    wchar_t className[64] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    return lstrcmpiW(className, WC_LISTVIEWW) == 0;
}

bool IsListBoxHost(HWND hwnd)
{
    wchar_t className[64] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    return lstrcmpiW(className, L"ListBox") == 0
        || lstrcmpiW(className, L"ComboLBox") == 0;
}

int ThemedScrollHostItemCount(HWND hwnd)
{
    if (IsListViewHost(hwnd)) {
        return ListView_GetItemCount(hwnd);
    }
    if (IsListBoxHost(hwnd)) {
        return static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0));
    }
    return 0;
}

int ThemedScrollHostPageSize(HWND hwnd)
{
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    const int clientHeight = clientRect.bottom - clientRect.top;
    if (clientHeight <= 0) {
        return 1;
    }
    if (IsListViewHost(hwnd)) {
        return (std::max)(1, ListView_GetCountPerPage(hwnd));
    }
    if (IsListBoxHost(hwnd)) {
        const int itemHeight = static_cast<int>(SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0));
        return itemHeight > 0 ? (std::max)(1, clientHeight / itemHeight) : 1;
    }
    return 1;
}

int ThemedScrollHostPosition(HWND hwnd)
{
    if (IsListViewHost(hwnd)) {
        return (std::max)(0, ListView_GetTopIndex(hwnd));
    }
    if (IsListBoxHost(hwnd)) {
        return (std::max)(0, static_cast<int>(SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0)));
    }
    return 0;
}

void HideNativeScrollbars(HWND hwnd)
{
    if (!hwnd) {
        return;
    }
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR desiredStyle = style & ~(WS_VSCROLL | WS_HSCROLL);
    if (IsListViewHost(hwnd)) {
        desiredStyle |= LVS_NOSCROLL;
    }
    if (desiredStyle != style) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, desiredStyle);
        SetWindowPos(
            hwnd,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    ShowScrollBar(hwnd, SB_BOTH, FALSE);
}

void SetThemedScrollHostPosition(HWND hwnd, int position)
{
    const int itemCount = ThemedScrollHostItemCount(hwnd);
    const int pageSize = ThemedScrollHostPageSize(hwnd);
    const int maxPosition = (std::max)(0, itemCount - pageSize);
    position = (std::clamp)(position, 0, maxPosition);
    if (IsListViewHost(hwnd)) {
        const int currentPosition = ThemedScrollHostPosition(hwnd);
        RECT itemRect{};
        const int itemHeight = ListView_GetItemRect(hwnd, currentPosition, &itemRect, LVIR_BOUNDS)
            ? itemRect.bottom - itemRect.top
            : 0;
        if (itemHeight > 0 && position != currentPosition) {
            SendMessageW(
                hwnd,
                LVM_SCROLL,
                0,
                static_cast<LPARAM>((position - currentPosition) * itemHeight));
        }
        if (position < itemCount) {
            ListView_EnsureVisible(hwnd, position, FALSE);
        }
    } else if (IsListBoxHost(hwnd)) {
        SendMessageW(hwnd, LB_SETTOPINDEX, static_cast<WPARAM>(position), 0);
    }
}

void UpdateThemedScrollbar(HWND host)
{
    HideNativeScrollbars(host);
    const auto scrollbarIt = gThemedScrollbarByHost.find(host);
    if (scrollbarIt == gThemedScrollbarByHost.end() || !IsWindow(scrollbarIt->second)) {
        return;
    }
    HWND scrollbar = scrollbarIt->second;
    RECT clientRect{};
    GetClientRect(host, &clientRect);
    const int scrollbarX = (std::max)(0, static_cast<int>(clientRect.right) - kThemedScrollbarWidth);
    const int hostHeight = (std::max)(0, static_cast<int>(clientRect.bottom));
    const int itemCount = ThemedScrollHostItemCount(host);
    const int pageSize = ThemedScrollHostPageSize(host);
    const int maxPosition = (std::max)(0, itemCount - pageSize);
    ::SetWindowPos(
        scrollbar,
        HWND_TOP,
        scrollbarX,
        0,
        kThemedScrollbarWidth,
        hostHeight,
        SWP_NOACTIVATE | (maxPosition > 0 ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));

    InvalidateRect(scrollbar, nullptr, FALSE);
}

struct ThemedScrollbarMetrics {
    RECT clientRect{};
    int thumbTop = 0;
    int thumbHeight = 0;
    int maxPosition = 0;
};

bool GetThemedScrollbarMetrics(HWND scrollbar, ThemedScrollbarMetrics& metrics)
{
    if (!scrollbar) {
        return false;
    }
    HWND host = GetParent(scrollbar);
    if (!host) {
        return false;
    }
    GetClientRect(scrollbar, &metrics.clientRect);
    const int trackHeight = metrics.clientRect.bottom - metrics.clientRect.top;
    const int itemCount = ThemedScrollHostItemCount(host);
    const int pageSize = ThemedScrollHostPageSize(host);
    const int range = (std::max)(1, itemCount);
    metrics.maxPosition = (std::max)(0, itemCount - pageSize);
    if (trackHeight <= 0 || metrics.maxPosition <= 0) {
        return false;
    }
    metrics.thumbHeight = (std::max)(
        24,
        (std::min)(trackHeight, static_cast<int>(
            (static_cast<long long>(trackHeight) * pageSize) / range)));
    const int travel = (std::max)(0, trackHeight - metrics.thumbHeight);
    const int position = (std::min)(ThemedScrollHostPosition(host), metrics.maxPosition);
    metrics.thumbTop = metrics.maxPosition > 0
        ? static_cast<int>((static_cast<long long>(travel) * position) / metrics.maxPosition)
        : 0;
    return true;
}

void DrawThemedScrollbarControl(HWND hwnd, HDC dc)
{
    if (!hwnd || !dc) {
        return;
    }
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    HBRUSH trackBrush = CreateSolidBrush(kThemeColors.scrollbarTrack);
    if (trackBrush) {
        FillRect(dc, &clientRect, trackBrush);
        DeleteObject(trackBrush);
    }

    ThemedScrollbarMetrics metrics{};
    if (!GetThemedScrollbarMetrics(hwnd, metrics)) {
        return;
    }

    RECT thumbRect{
        clientRect.left + 2,
        metrics.thumbTop + 2,
        clientRect.right - 2,
        metrics.thumbTop + metrics.thumbHeight - 2};
    const COLORREF thumbColor = gHoveredThemedScrollbar == hwnd
        ? kThemeColors.scrollbarThumbHover
        : kThemeColors.scrollbarThumb;
    HBRUSH thumbBrush = CreateSolidBrush(thumbColor);
    if (thumbBrush) {
        FillRect(dc, &thumbRect, thumbBrush);
        DeleteObject(thumbBrush);
    }
}

LRESULT CALLBACK ThemedScrollbarControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        DrawThemedScrollbarControl(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (gDraggingThemedScrollbar == hwnd) {
            ThemedScrollbarMetrics metrics{};
            if (GetThemedScrollbarMetrics(hwnd, metrics)) {
                const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
                const int travel = (std::max)(
                    1,
                    static_cast<int>(metrics.clientRect.bottom - metrics.clientRect.top) - metrics.thumbHeight);
                const int desiredTop = y - gThemedScrollbarDragOffset;
                const int clampedTop = (std::clamp)(desiredTop, 0, travel);
                const int position = static_cast<int>(
                    (static_cast<long long>(clampedTop) * metrics.maxPosition) / travel);
                HWND host = GetParent(hwnd);
                SendMessageW(
                    host,
                    WM_VSCROLL,
                    MAKEWPARAM(SB_THUMBTRACK, position),
                    reinterpret_cast<LPARAM>(hwnd));
            }
            return 0;
        }
        if (gHoveredThemedScrollbar != hwnd) {
            HWND previous = gHoveredThemedScrollbar;
            gHoveredThemedScrollbar = hwnd;
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
        return 0;
    }
    case WM_MOUSELEAVE:
        if (gHoveredThemedScrollbar == hwnd) {
            gHoveredThemedScrollbar = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        ThemedScrollbarMetrics metrics{};
        if (!GetThemedScrollbarMetrics(hwnd, metrics)) {
            return 0;
        }
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        if (y >= metrics.thumbTop && y < metrics.thumbTop + metrics.thumbHeight) {
            gDraggingThemedScrollbar = hwnd;
            gThemedScrollbarDragOffset = y - metrics.thumbTop;
            SetCapture(hwnd);
        } else {
            HWND host = GetParent(hwnd);
            const int command = y < metrics.thumbTop ? SB_PAGEUP : SB_PAGEDOWN;
            SendMessageW(host, WM_VSCROLL, MAKEWPARAM(command, 0), reinterpret_cast<LPARAM>(hwnd));
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (gDraggingThemedScrollbar == hwnd) {
            ThemedScrollbarMetrics metrics{};
            if (GetThemedScrollbarMetrics(hwnd, metrics)) {
                const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
                const int travel = (std::max)(
                    1,
                    static_cast<int>(metrics.clientRect.bottom - metrics.clientRect.top) - metrics.thumbHeight);
                const int desiredTop = y - gThemedScrollbarDragOffset;
                const int clampedTop = (std::clamp)(desiredTop, 0, travel);
                const int position = static_cast<int>(
                    (static_cast<long long>(clampedTop) * metrics.maxPosition) / travel);
                HWND host = GetParent(hwnd);
                SendMessageW(
                    host,
                    WM_VSCROLL,
                    MAKEWPARAM(SB_THUMBPOSITION, position),
                    reinterpret_cast<LPARAM>(hwnd));
            }
            ReleaseCapture();
            gDraggingThemedScrollbar = nullptr;
            gThemedScrollbarDragOffset = 0;
        }
        return 0;
    case WM_CAPTURECHANGED:

        if (gDraggingThemedScrollbar == hwnd) {
            gDraggingThemedScrollbar = nullptr;
            gThemedScrollbarDragOffset = 0;
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        return 0;
    case WM_NCDESTROY:
        if (gHoveredThemedScrollbar == hwnd) {
            gHoveredThemedScrollbar = nullptr;
        }
        if (gDraggingThemedScrollbar == hwnd) {
            gDraggingThemedScrollbar = nullptr;
            gThemedScrollbarDragOffset = 0;
        }
        RemoveWindowSubclass(hwnd, ThemedScrollbarControlProc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK ThemedScrollbarHostSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (message) {
    case WM_NCPAINT: {
        HideNativeScrollbars(hwnd);
        UpdateThemedScrollbar(hwnd);
        return 0;
    }
    case WM_PAINT: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        UpdateThemedScrollbar(hwnd);
        return result;
    }
    case WM_SIZE: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        UpdateThemedScrollbar(hwnd);
        return result;
    }
    case WM_SHOWWINDOW:
    case WM_WINDOWPOSCHANGED: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        UpdateThemedScrollbar(hwnd);
        return result;
    }
    case WM_MOUSEWHEEL: {
        const int itemCount = ThemedScrollHostItemCount(hwnd);
        if (itemCount <= ThemedScrollHostPageSize(hwnd)) {
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }
        const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        const int lines = (std::max)(1, std::abs(wheelDelta) / WHEEL_DELTA);
        const int direction = wheelDelta > 0 ? -1 : 1;
        SetThemedScrollHostPosition(hwnd, ThemedScrollHostPosition(hwnd) + direction * lines);
        UpdateThemedScrollbar(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        UpdateThemedScrollbar(hwnd);
        return result;
    }
    case WM_VSCROLL:
        if (reinterpret_cast<HWND>(lParam) == gThemedScrollbarByHost[hwnd]) {
            const int pageSize = ThemedScrollHostPageSize(hwnd);
            const int currentPosition = ThemedScrollHostPosition(hwnd);
            const int maxPosition = (std::max)(0, ThemedScrollHostItemCount(hwnd) - pageSize);
            int nextPosition = currentPosition;
            switch (LOWORD(wParam)) {
            case SB_LINEUP: nextPosition -= 1; break;
            case SB_LINEDOWN: nextPosition += 1; break;
            case SB_PAGEUP: nextPosition -= pageSize; break;
            case SB_PAGEDOWN: nextPosition += pageSize; break;
            case SB_THUMBPOSITION:
            case SB_THUMBTRACK: nextPosition = HIWORD(wParam); break;
            case SB_TOP: nextPosition = 0; break;
            case SB_BOTTOM: nextPosition = maxPosition; break;
            default: break;
            }
            SetThemedScrollHostPosition(hwnd, nextPosition);
            UpdateThemedScrollbar(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        gThemedScrollbarByHost.erase(hwnd);
        RemoveWindowSubclass(hwnd, ThemedScrollbarHostSubclassProc, 1);
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

} // namespace

void ScheduleModernComboRedraw(HWND combo)
{
    if (combo) {
        PostMessageW(combo, kRefreshModernComboMessage, 0, 0);
    }
}

void DismissCustomComboPopup()
{
    if (gCustomComboPopup) {
        CloseCustomComboPopup(
            gCustomComboPopup->popup,
            CustomComboPopupCloseReason::Dismiss);
    }
}

HWND CreateBeanTextBox(
    HWND parent,
    int controlId,
    const wchar_t* initialText,
    LONG_PTR style,
    AppContext* ctx)
{
    if (!parent || !EnsureBeanTextBoxClass()) {
        return nullptr;
    }
    const bool numberOnly = (style & ES_NUMBER) != 0;
    const bool multiline = (style & ES_MULTILINE) != 0;
    const bool readOnly = (style & ES_READONLY) != 0;
    style &= ~(WS_BORDER | WS_VSCROLL | WS_HSCROLL);
    HWND textBox = CreateWindowExW(
        0,
        kBeanTextBoxClassName,
        L"",
        static_cast<DWORD>(style),
        0,
        0,
        10,
        10,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
        GetModuleHandleW(nullptr),
        nullptr);
    if (!textBox) {
        return nullptr;
    }
    BeanTextBoxState state;
    state.ctx = ctx;
    state.text = initialText ? initialText : L"";
    state.caret = state.anchor = state.text.size();
    state.numberOnly = numberOnly;
    state.multiline = multiline;
    state.readOnly = readOnly;
    state.font = gTheme.uiFont;
    gBeanTextBoxes.emplace(textBox, std::move(state));
    BeanTextBoxInvalidate(textBox);
    return textBox;
}

bool CopyBeanTextBoxText(HWND hwnd)
{
    const BeanTextBoxState* state = GetBeanTextBoxState(hwnd);
    if (!state || !OpenClipboard(hwnd)) {
        return false;
    }
    EmptyClipboard();
    const size_t byteCount = (state->text.size() + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    bool copied = false;
    if (data) {
        void* destination = GlobalLock(data);
        if (destination) {
            std::memcpy(destination, state->text.c_str(), byteCount);
            GlobalUnlock(data);
            if (SetClipboardData(CF_UNICODETEXT, data)) {
                data = nullptr;
                copied = true;
            }
        }
    }
    if (data) {
        GlobalFree(data);
    }
    CloseClipboard();
    return copied;
}

#if 0
void ConfigureThemedScrollbars(HWND hwnd)
{
    if (!hwnd) {
        return;
    }

    const auto existing = gThemedScrollbarByHost.find(hwnd);
    if (existing != gThemedScrollbarByHost.end() && IsWindow(existing->second)) {
        UpdateThemedScrollbar(hwnd);
        return;
    }

    const LONG_PTR originalStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR style = originalStyle;
    style &= ~(WS_VSCROLL | WS_HSCROLL);
    style |= WS_CLIPCHILDREN;
    if (IsListViewHost(hwnd)) {
        style |= LVS_NOSCROLL;
    }
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    HWND scrollbar = CreateWindowExW(
        0,
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE | SS_NOTIFY,
        0,
        0,
        kThemedScrollbarWidth,
        0,
        hwnd,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (!scrollbar) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, originalStyle);
        SetWindowPos(
            hwnd,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        return;
    }

    gThemedScrollbarByHost[hwnd] = scrollbar;
    SetWindowSubclass(scrollbar, ThemedScrollbarControlProc, 1, 0);
    SetWindowTheme(hwnd, L"", L"");
    SetWindowSubclass(hwnd, ThemedScrollbarHostSubclassProc, 1, 0);
    UpdateThemedScrollbar(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}
#endif

namespace {

struct BeanFileListState {
    AppContext* ctx = nullptr;
    BeanFileListKind kind = BeanFileListKind::Recordings;
    int scrollOffset = 0;
    bool draggingScrollbar = false;
    int dragOffset = 0;
};

std::unordered_map<HWND, BeanFileListState> gBeanFileLists;
constexpr int kBeanFileListHeaderHeight = 28;
constexpr int kBeanFileListRowHeight = 26;
constexpr int kBeanFileListScrollbarWidth = 14;

BeanFileListState* GetBeanFileListState(HWND hwnd)
{
    const auto it = gBeanFileLists.find(hwnd);
    return it == gBeanFileLists.end() ? nullptr : &it->second;
}

int& BeanFileListSelection(BeanFileListState& state)
{
    return state.kind == BeanFileListKind::Recordings
        ? state.ctx->recordingsSelectedIndex
        : state.ctx->youtubeMediaSelectedIndex;
}

size_t BeanFileListItemCount(const BeanFileListState& state)
{
    return state.kind == BeanFileListKind::Recordings
        ? state.ctx->recordingItems.size()
        : state.ctx->youtubeMediaItems.size();
}

COLORREF BeanFileListSelectionBackground(const BeanFileListState& state)
{
    const bool appActive = state.ctx
        && state.ctx->mainWindow
        && GetForegroundWindow() == state.ctx->mainWindow;
    return appActive ? kColorListSelection : kThemeColors.listSelectionInactive;
}

std::vector<int> BeanFileListColumnWidths(HWND hwnd, BeanFileListKind kind)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int contentWidth = (std::max)(100, static_cast<int>(rc.right) - kBeanFileListScrollbarWidth);
    std::vector<int> widths;
    if (kind == BeanFileListKind::Recordings) {
        const int dungeonWidth = (std::max)(120, contentWidth * 40 / 100);
        const int keyWidth = (std::max)(56, contentWidth * 11 / 100);
        const int lengthWidth = (std::max)(88, contentWidth * 17 / 100);
        const int dateWidth = (std::max)(94, contentWidth - dungeonWidth - keyWidth - lengthWidth);
        widths = {dungeonWidth, keyWidth, lengthWidth, dateWidth};
    } else {
        const int typeWidth = (std::max)(86, contentWidth * 12 / 100);
        const int dateWidth = (std::max)(130, contentWidth * 23 / 100);
        const int nameWidth = (std::max)(220, contentWidth - typeWidth - dateWidth);
        widths = {typeWidth, nameWidth, dateWidth};
    }
    int totalWidth = 0;
    for (const int width : widths) {
        totalWidth += width;
    }
    if (totalWidth > contentWidth) {
        int assignedWidth = 0;
        for (size_t index = 0; index + 1 < widths.size(); ++index) {
            widths[index] = (std::max)(1, widths[index] * contentWidth / totalWidth);
            assignedWidth += widths[index];
        }
        widths.back() = (std::max)(1, contentWidth - assignedWidth);
    }
    return widths;
}

std::vector<std::wstring> BeanFileListHeaders(BeanFileListKind kind)
{
    return kind == BeanFileListKind::Recordings
        ? std::vector<std::wstring>{L"Dungeon", L"Level", L"Duration", L"Date"}
        : std::vector<std::wstring>{L"Type", L"Name", L"Date"};
}

int BeanFileListVisibleRows(int clientHeight)
{
    const int contentHeight = (std::max)(0, clientHeight - kBeanFileListHeaderHeight);
    return (std::max)(1, (contentHeight + kBeanFileListRowHeight - 1) / kBeanFileListRowHeight);
}

int BeanFileListScrollRows(int clientHeight)
{
    const int contentHeight = (std::max)(0, clientHeight - kBeanFileListHeaderHeight);
    return (std::max)(1, contentHeight / kBeanFileListRowHeight);
}

bool BeanFileListColumnCentered(BeanFileListKind kind, size_t column)
{
    return kind == BeanFileListKind::YouTube ? column == 2 : column > 0;
}

std::vector<std::wstring> BeanFileListRow(const BeanFileListState& state, size_t index)
{
    if (state.kind == BeanFileListKind::Recordings) {
        const auto& item = state.ctx->recordingItems[index];
        return {item.dungeonName, item.keystoneText, item.durationText, item.dateText};
    }
    const auto& item = state.ctx->youtubeMediaItems[index];
    return {
        item.type == YouTubeMediaType::Clip ? L"Clip" : L"Recording",
        item.path.filename().wstring(),
        FormatLocalDate(FileTimeToSystemClock(item.modified))};
}

void DrawBeanFileListText(HDC dc, const RECT& rect, const std::wstring& text, bool centered, COLORREF color, HFONT font)
{
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT textRect = rect;
    textRect.left += centered ? 3 : 8;
    textRect.right -= centered ? 3 : 6;
    DrawTextW(
        dc,
        text.c_str(),
        -1,
        &textRect,
        (centered ? DT_CENTER : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
}

void DrawBeanFileList(HWND hwnd, HDC dc)
{
    BeanFileListState* state = GetBeanFileListState(hwnd);
    if (!state || !state->ctx || !dc) {
        return;
    }
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    HBRUSH backgroundBrush = CreateSolidBrush(kColorListRow);
    if (backgroundBrush) {
        FillRect(dc, &clientRect, backgroundBrush);
        DeleteObject(backgroundBrush);
    }

    const auto widths = BeanFileListColumnWidths(hwnd, state->kind);
    const auto headers = BeanFileListHeaders(state->kind);
    const int contentWidth = clientRect.right - kBeanFileListScrollbarWidth;
    HBRUSH headerBrush = CreateSolidBrush(kColorInputBg);
    if (headerBrush) {
        RECT headerRect{0, 0, contentWidth, kBeanFileListHeaderHeight};
        FillRect(dc, &headerRect, headerBrush);
        DeleteObject(headerBrush);
    }

    const HFONT font = gTheme.recordingsFont ? gTheme.recordingsFont : gTheme.uiFont;
    int columnLeft = 0;
    for (size_t column = 0; column < headers.size(); ++column) {
        RECT cell{columnLeft, 0, columnLeft + widths[column], kBeanFileListHeaderHeight};
        DrawBeanFileListText(
            dc,
            cell,
            headers[column],
            BeanFileListColumnCentered(state->kind, column),
            kColorTextMuted,
            font);
        HBRUSH lineBrush = CreateSolidBrush(kColorListGrid);
        if (lineBrush) {
            RECT line{cell.right - 1, 0, cell.right, clientRect.bottom};
            FillRect(dc, &line, lineBrush);
            DeleteObject(lineBrush);
        }
        columnLeft += widths[column];
    }

    const size_t itemCount = BeanFileListItemCount(*state);
    const int clientHeight = static_cast<int>(clientRect.bottom);
    const int visibleRows = BeanFileListVisibleRows(clientHeight);
    const int maxOffset = (std::max)(
        0,
        static_cast<int>(itemCount) - BeanFileListScrollRows(clientHeight));
    state->scrollOffset = (std::clamp)(state->scrollOffset, 0, maxOffset);
    const int selectedIndex = BeanFileListSelection(*state);
    for (int row = 0; row < visibleRows; ++row) {
        const size_t itemIndex = static_cast<size_t>(state->scrollOffset + row);
        if (itemIndex >= itemCount) {
            break;
        }
        const int top = kBeanFileListHeaderHeight + row * kBeanFileListRowHeight;
        const bool selected = static_cast<int>(itemIndex) == selectedIndex;
        const COLORREF rowColor = selected
            ? BeanFileListSelectionBackground(*state)
            : ((itemIndex % 2 == 0) ? kColorListRow : kColorListRowAlt);
        HBRUSH rowBrush = CreateSolidBrush(rowColor);
        if (rowBrush) {
            RECT rowRect{0, top, contentWidth, top + kBeanFileListRowHeight};
            FillRect(dc, &rowRect, rowBrush);
            DeleteObject(rowBrush);
        }

        const auto cells = BeanFileListRow(*state, itemIndex);
        columnLeft = 0;
        for (size_t column = 0; column < cells.size(); ++column) {
            RECT cell{columnLeft, top, columnLeft + widths[column], top + kBeanFileListRowHeight};
            COLORREF textColor = kColorTextPrimary;
            if (state->kind == BeanFileListKind::Recordings
                && column == 1
                && !selected
                && itemIndex < state->ctx->recordingItems.size()) {
                const auto outcome = state->ctx->recordingItems[itemIndex].outcome;
                if (outcome == AppContext::RecordingItem::Outcome::Success) {
                    textColor = kColorSuccess;
                } else if (outcome == AppContext::RecordingItem::Outcome::Failure) {
                    textColor = kColorFailure;
                }
            }
            DrawBeanFileListText(
                dc,
                cell,
                cells[column],
                BeanFileListColumnCentered(state->kind, column),
                textColor,
                font);
            columnLeft += widths[column];
        }
        HBRUSH gridBrush = CreateSolidBrush(kColorListGrid);
        if (gridBrush) {
            RECT line{0, top + kBeanFileListRowHeight - 1, contentWidth, top + kBeanFileListRowHeight};
            FillRect(dc, &line, gridBrush);
            DeleteObject(gridBrush);
        }
    }

    HBRUSH scrollbarTrack = CreateSolidBrush(kThemeColors.scrollbarTrack);
    if (scrollbarTrack) {
        RECT track{contentWidth, 0, clientRect.right, clientRect.bottom};
        FillRect(dc, &track, scrollbarTrack);
        DeleteObject(scrollbarTrack);
    }
    if (maxOffset > 0) {
        const int trackHeight = clientHeight;
        const int scrollRows = BeanFileListScrollRows(clientHeight);
        const int thumbHeight = (std::max)(
            24,
            (std::min)(trackHeight, trackHeight * scrollRows / (std::max)(1, static_cast<int>(itemCount))));
        const int travel = (std::max)(1, trackHeight - thumbHeight);
        const int thumbTop = travel * state->scrollOffset / maxOffset;
        const COLORREF thumbColor = state->draggingScrollbar
            ? kThemeColors.scrollbarThumbHover
            : kThemeColors.scrollbarThumb;
        HBRUSH thumbBrush = CreateSolidBrush(thumbColor);
        if (thumbBrush) {
            RECT thumb{contentWidth + 2, thumbTop + 2, clientRect.right - 2, thumbTop + thumbHeight - 2};
            FillRect(dc, &thumb, thumbBrush);
            DeleteObject(thumbBrush);
        }
    }
    HBRUSH borderBrush = CreateSolidBrush(kColorInputBorder);
    if (borderBrush) {
        RECT topBorder{0, 0, clientRect.right, 1};
        RECT bottomBorder{0, clientRect.bottom - 1, clientRect.right, clientRect.bottom};
        RECT leftBorder{0, 0, 1, clientRect.bottom};
        RECT rightBorder{clientRect.right - 1, 0, clientRect.right, clientRect.bottom};
        FillRect(dc, &topBorder, borderBrush);
        FillRect(dc, &bottomBorder, borderBrush);
        FillRect(dc, &leftBorder, borderBrush);
        FillRect(dc, &rightBorder, borderBrush);
        DeleteObject(borderBrush);
    }
}

void NotifyBeanFileList(HWND hwnd, UINT message, LPARAM value)
{
    const BeanFileListState* state = GetBeanFileListState(hwnd);
    HWND target = state && state->ctx ? state->ctx->mainWindow : GetParent(hwnd);
    if (target) {
        SendMessageW(target, message, reinterpret_cast<WPARAM>(hwnd), value);
    }
}

void EnsureBeanFileListSelectionVisible(HWND hwnd, BeanFileListState& state)
{
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    const int clientHeight = static_cast<int>(clientRect.bottom);
    const int visibleRows = BeanFileListScrollRows(clientHeight);
    const int selected = BeanFileListSelection(state);
    if (selected >= 0) {
        if (selected < state.scrollOffset) {
            state.scrollOffset = selected;
        } else if (selected >= state.scrollOffset + visibleRows) {
            state.scrollOffset = selected - visibleRows + 1;
        }
    }
    const int maxOffset = (std::max)(
        0,
        static_cast<int>(BeanFileListItemCount(state)) - visibleRows);
    state.scrollOffset = (std::clamp)(state.scrollOffset, 0, maxOffset);
}

void PaintBeanFileListBuffered(HWND hwnd, HDC target, const PAINTSTRUCT& paint)
{
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    const int width = static_cast<int>(clientRect.right);
    const int height = static_cast<int>(clientRect.bottom);
    if (width <= 0 || height <= 0) {
        return;
    }

    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, width, height) : nullptr;
    if (!buffer || !bitmap) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        if (buffer) {
            DeleteDC(buffer);
        }
        DrawBeanFileList(hwnd, target);
        return;
    }

    const HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
    DrawBeanFileList(hwnd, buffer);
    const RECT& dirty = paint.rcPaint;
    BitBlt(
        target,
        dirty.left,
        dirty.top,
        dirty.right - dirty.left,
        dirty.bottom - dirty.top,
        buffer,
        dirty.left,
        dirty.top,
        SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

LRESULT CALLBACK BeanFileListSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    BeanFileListState* state = GetBeanFileListState(hwnd);
    if (!state) {
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintBeanFileListBuffered(hwnd, dc, paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_SIZE:
        EnsureBeanFileListSelectionVisible(hwnd, *state);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_GETDLGCODE: {
        LRESULT code = DefSubclassProc(hwnd, message, wParam, lParam) | DLGC_WANTARROWS;
        const MSG* keyMessage = reinterpret_cast<const MSG*>(lParam);
        if (keyMessage
            && keyMessage->message == WM_KEYDOWN
            && keyMessage->wParam == VK_RETURN) {
            code |= DLGC_WANTMESSAGE;
        }
        return code;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const int lines = (std::max)(1, std::abs(delta) / WHEEL_DELTA);
        state->scrollOffset += delta > 0 ? -lines : lines;
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int maxOffset = (std::max)(
            0,
            static_cast<int>(BeanFileListItemCount(*state))
                - BeanFileListScrollRows(static_cast<int>(clientRect.bottom)));
        state->scrollOffset = (std::clamp)(state->scrollOffset, 0, maxOffset);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int clientHeight = static_cast<int>(rc.bottom);
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        const int contentWidth = rc.right - kBeanFileListScrollbarWidth;
        const size_t itemCount = BeanFileListItemCount(*state);
        const int visibleRows = BeanFileListScrollRows(clientHeight);
        const int maxOffset = (std::max)(0, static_cast<int>(itemCount) - visibleRows);
        if (x >= contentWidth && maxOffset > 0) {
            const int trackHeight = clientHeight;
            const int thumbHeight = (std::max)(
                24,
                (std::min)(trackHeight, trackHeight * visibleRows / (std::max)(1, static_cast<int>(itemCount))));
            const int travel = (std::max)(1, trackHeight - thumbHeight);
            const int thumbTop = travel * state->scrollOffset / maxOffset;
            if (y >= thumbTop && y < thumbTop + thumbHeight) {
                state->draggingScrollbar = true;
                state->dragOffset = y - thumbTop;
                SetCapture(hwnd);
            } else {
                state->scrollOffset += y < thumbTop ? -visibleRows : visibleRows;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (y >= kBeanFileListHeaderHeight && x < contentWidth) {
            const int row = (y - kBeanFileListHeaderHeight) / kBeanFileListRowHeight;
            const int index = state->scrollOffset + row;
            if (row >= 0 && index >= 0 && static_cast<size_t>(index) < itemCount) {
                BeanFileListSelection(*state) = index;
                NotifyBeanFileList(hwnd, WM_BEAN_FILE_LIST_SELECTION, index);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (y < kBeanFileListHeaderHeight && x < contentWidth) {
            const auto widths = BeanFileListColumnWidths(hwnd, state->kind);
            int left = 0;
            for (size_t column = 0; column < widths.size(); ++column) {
                if (x >= left && x < left + widths[column]) {
                    NotifyBeanFileList(hwnd, WM_BEAN_FILE_LIST_COLUMN_CLICK, static_cast<LPARAM>(column));
                    break;
                }
                left += widths[column];
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (state->draggingScrollbar) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int clientHeight = static_cast<int>(rc.bottom);
            const int itemCount = static_cast<int>(BeanFileListItemCount(*state));
            const int visibleRows = BeanFileListScrollRows(clientHeight);
            const int maxOffset = (std::max)(0, itemCount - visibleRows);
            const int thumbHeight = (std::max)(
                24,
                (std::min)(clientHeight, clientHeight * visibleRows / (std::max)(1, itemCount)));
            const int travel = (std::max)(1, clientHeight - thumbHeight);
            const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
            state->scrollOffset = (std::clamp)((y - state->dragOffset) * maxOffset / travel, 0, maxOffset);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        return 0;
    case WM_LBUTTONUP:
        if (state->draggingScrollbar) {
            state->draggingScrollbar = false;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_KEYDOWN: {
        const int itemCount = static_cast<int>(BeanFileListItemCount(*state));
        if (itemCount <= 0) {
            return 0;
        }
        int& selected = BeanFileListSelection(*state);
        if (wParam == VK_RETURN) {
            if (selected >= 0 && selected < itemCount) {
                NotifyBeanFileList(hwnd, WM_BEAN_FILE_LIST_DOUBLE_CLICK, selected);
            }
            return 0;
        }
        int next = selected < 0 ? 0 : selected;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int visibleRows = BeanFileListScrollRows(static_cast<int>(rc.bottom));
        if (wParam == VK_UP) next -= 1;
        else if (wParam == VK_DOWN) next += 1;
        else if (wParam == VK_PRIOR) next -= visibleRows;
        else if (wParam == VK_NEXT) next += visibleRows;
        else if (wParam == VK_HOME) next = 0;
        else if (wParam == VK_END) next = itemCount - 1;
        else break;
        selected = (std::clamp)(next, 0, itemCount - 1);
        EnsureBeanFileListSelectionVisible(hwnd, *state);
        NotifyBeanFileList(hwnd, WM_BEAN_FILE_LIST_SELECTION, selected);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        if (y >= kBeanFileListHeaderHeight) {
            const int row = (y - kBeanFileListHeaderHeight) / kBeanFileListRowHeight;
            const int index = state->scrollOffset + row;
            if (index >= 0 && static_cast<size_t>(index) < BeanFileListItemCount(*state)) {
                NotifyBeanFileList(hwnd, WM_BEAN_FILE_LIST_DOUBLE_CLICK, index);
            }
        }
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        gBeanFileLists.erase(hwnd);
        RemoveWindowSubclass(hwnd, BeanFileListSubclassProc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

} // namespace

HWND CreateBeanFileList(HWND parent, int controlId, AppContext* ctx, BeanFileListKind kind)
{
    if (!parent || !ctx) {
        return nullptr;
    }
    HWND list = CreateWindowExW(
        0,
        L"STATIC",
        L"",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | SS_NOTIFY,
        0,
        0,
        10,
        10,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
        nullptr,
        nullptr);
    if (!list) {
        return nullptr;
    }
    gBeanFileLists[list] = BeanFileListState{ctx, kind, 0, false, 0};
    SetWindowSubclass(list, BeanFileListSubclassProc, 1, 0);
    return list;
}

void RefreshBeanFileList(HWND list)
{
    if (list) {
        InvalidateRect(list, nullptr, FALSE);
    }
}

int GetBeanFileListSelectedIndex(HWND list)
{
    const BeanFileListState* state = GetBeanFileListState(list);
    return state && state->ctx ? BeanFileListSelection(*const_cast<BeanFileListState*>(state)) : -1;
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
    const Gdiplus::REAL iconSize = static_cast<Gdiplus::REAL>((std::min)(width, height));
    const Gdiplus::REAL circleDiameter = iconSize - 2.5f;
    if (circleDiameter <= 0.0f) {
        return;
    }
    const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>(bounds.left + width / 2.0f);
    const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>(bounds.top + height / 2.0f);
    const Gdiplus::REAL circleLeft = centerX - circleDiameter / 2.0f;
    const Gdiplus::REAL circleTop = centerY - circleDiameter / 2.0f;

    if (EnsureAlertGdiplus()) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

        const Gdiplus::Color warningColor(
            255,
            GetRValue(kThemeColors.accent),
            GetGValue(kThemeColors.accent),
            GetBValue(kThemeColors.accent));
        const Gdiplus::Color glyphColor(
            255,
            GetRValue(kColorWindowTop),
            GetGValue(kColorWindowTop),
            GetBValue(kColorWindowTop));
        Gdiplus::SolidBrush warningBrush(warningColor);
        Gdiplus::Pen warningPen(warningColor, 1.25f);
        const Gdiplus::RectF circleRect(circleLeft, circleTop, circleDiameter, circleDiameter);
        graphics.FillEllipse(&warningBrush, circleRect);
        graphics.DrawEllipse(&warningPen, circleRect);

        const Gdiplus::REAL scale = circleDiameter / 13.5f;
        Gdiplus::Pen glyphPen(glyphColor, 2.2f);
        glyphPen.SetStartCap(Gdiplus::LineCapRound);
        glyphPen.SetEndCap(Gdiplus::LineCapRound);
        glyphPen.SetLineJoin(Gdiplus::LineJoinRound);
        graphics.DrawLine(
            &glyphPen,
            centerX,
            centerY - 4.5f * scale,
            centerX,
            centerY + 1.8f * scale);
        Gdiplus::SolidBrush glyphBrush(glyphColor);
        const Gdiplus::REAL dotDiameter = 1.8f * scale;
        graphics.FillEllipse(
            &glyphBrush,
            centerX - dotDiameter / 2.0f,
            centerY + 4.0f * scale - dotDiameter / 2.0f,
            dotDiameter,
            dotDiameter);
        return;
    }

    const int diameter = (std::max)(1, (std::min)(width, height) - 2);
    const int left = bounds.left + (width - diameter) / 2;
    const int top = bounds.top + (height - diameter) / 2;
    const int right = left + diameter;
    const int bottom = top + diameter;
    HPEN borderPen = CreatePen(PS_SOLID, 1, kThemeColors.accent);
    HBRUSH fillBrush = CreateSolidBrush(kThemeColors.accent);
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

    const int centerXInt = (left + right) / 2;
    HPEN exclamationPen = CreatePen(PS_SOLID, 2, kColorWindowTop);
    HGDIOBJ oldExclamationPen = exclamationPen ? SelectObject(dc, exclamationPen) : nullptr;
    MoveToEx(dc, centerXInt, top + 3, nullptr);
    LineTo(dc, centerXInt, bottom - 5);
    if (oldExclamationPen) {
        SelectObject(dc, oldExclamationPen);
    }
    if (exclamationPen) {
        DeleteObject(exclamationPen);
    }
    HBRUSH dotBrush = CreateSolidBrush(kColorWindowTop);
    HGDIOBJ oldDotBrush = dotBrush ? SelectObject(dc, dotBrush) : nullptr;
    Ellipse(dc, centerXInt - 1, bottom - 4, centerXInt + 2, bottom - 1);
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
    case IDC_STATUS_COPY_LOG_TEXT:
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
    return IsStyledComboIdInternal(controlId);
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
    const std::array<int, 4> statusButtons = {
        IDC_RECORD_START,
        IDC_RECORD_STOP,
        IDC_STATUS_COPY_LOG_TEXT,
        IDC_STATUS_OPEN_LOG_FOLDER};
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
    const int borderInset = isTab && isActiveTab ? 2 : 1;
    Rectangle(drawInfo->hDC, rc.left + borderInset, rc.top + borderInset, rc.right - borderInset, rc.bottom - borderInset);
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
                iconRect.right -= 8;
                const int targetSize = 18;
                iconRect.left = iconRect.right - targetSize;
                const int height = iconRect.bottom - iconRect.top;
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
