#include "app/AppDraw.h"

#include "app/AppUtilities.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cctype>

VisualTheme gTheme;

namespace {

HWND gHoveredStyledButton = nullptr;
HWND gHoveredHelpIcon = nullptr;

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

void DrawCheckOrXGlyph(HDC dc, const RECT& bounds, bool valid)
{
    const COLORREF indicatorColor = valid ? kColorSuccess : kColorFailure;
    HPEN pen = CreatePen(PS_SOLID, 2, indicatorColor);
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
    if (pen) {
        DeleteObject(pen);
    }
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
        return L"Technical details:\nUltra = 16 (CQP/CRF/ICQ)\nHigh = 20 (CQP/CRF/ICQ)\nMedium = 24 (CQP/CRF/ICQ)\nLow = 28 (CQP/CRF/ICQ)\nMinimum = 32 (CQP/CRF/ICQ)\n\nx264 preset: Ultra = medium, High/Medium = veryfast, Low/Minimum = superfast.";
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

    HPEN linePen = CreatePen(PS_SOLID, 1, kColorListGrid);
    HGDIOBJ oldPen = nullptr;
    if (linePen) {
        oldPen = SelectObject(hdc, linePen);
    }
    if (linePen) {
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
    if (linePen) {
        DeleteObject(linePen);
    }
    if (oldFont) {
        SelectObject(hdc, oldFont);
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
}

void DestroyThemeResources()
{
    if (gTheme.uiFont) { DeleteObject(gTheme.uiFont); gTheme.uiFont = nullptr; }
    if (gTheme.mutedHintFont) { DeleteObject(gTheme.mutedHintFont); gTheme.mutedHintFont = nullptr; }
    if (gTheme.statusIndicatorFont) { DeleteObject(gTheme.statusIndicatorFont); gTheme.statusIndicatorFont = nullptr; }
    if (gTheme.recordingsFont) { DeleteObject(gTheme.recordingsFont); gTheme.recordingsFont = nullptr; }
    if (gTheme.headingFont) { DeleteObject(gTheme.headingFont); gTheme.headingFont = nullptr; }
    if (gTheme.inputBrush) { DeleteObject(gTheme.inputBrush); gTheme.inputBrush = nullptr; }
    if (gTheme.buttonBrush) { DeleteObject(gTheme.buttonBrush); gTheme.buttonBrush = nullptr; }
    if (gTheme.panelSolidBrush) { DeleteObject(gTheme.panelSolidBrush); gTheme.panelSolidBrush = nullptr; }
    if (gTheme.panelBorderBrush) { DeleteObject(gTheme.panelBorderBrush); gTheme.panelBorderBrush = nullptr; }
    if (gTheme.tooltipBrush) { DeleteObject(gTheme.tooltipBrush); gTheme.tooltipBrush = nullptr; }
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
    case IDC_TAB_CLIPS:
    case IDC_TAB_ABOUT:
    case IDC_OUTPUT_BROWSE:
    case IDC_LOG_BROWSE:
    case IDC_MONITOR_START:
    case IDC_MONITOR_STOP:
    case IDC_RECORD_START:
    case IDC_RECORD_STOP:
    case IDC_STATUS_OPEN_LOG_FOLDER:
    case IDC_RECORDINGS_REFRESH:
    case IDC_RECORDINGS_OPEN_FOLDER:
    case IDC_RECORDINGS_OPEN_DB_FOLDER:
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
    case IDC_CLIPS_OPEN_FOLDER:
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
    const std::array<int, 6> mainButtons = {IDC_TAB_STATUS, IDC_TAB_CONFIGURATION, IDC_TAB_CHAT_PRIVACY, IDC_TAB_RECORDINGS, IDC_TAB_CLIPS, IDC_TAB_ABOUT};
    for (const int id : mainButtons) {
        EnableOwnerDrawButton(ctx->mainWindow, id);
    }
    const std::array<int, 5> statusButtons = {IDC_MONITOR_START, IDC_MONITOR_STOP, IDC_RECORD_START, IDC_RECORD_STOP, IDC_STATUS_OPEN_LOG_FOLDER};
    for (const int id : statusButtons) {
        EnableOwnerDrawButton(ctx->statusPanel, id);
    }
    const std::array<int, 2> recorderButtons = {IDC_OUTPUT_BROWSE, IDC_LOG_BROWSE};
    for (const int id : recorderButtons) {
        EnableOwnerDrawButton(ctx->recorderPanel, id);
    }
    const std::array<int, 9> recordingsButtons = {
        IDC_RECORDINGS_REFRESH, IDC_RECORDINGS_OPEN_FOLDER, IDC_RECORDINGS_OPEN_DB_FOLDER, IDC_YOUTUBE_LINK_BUTTON, IDC_YOUTUBE_UNLINK_BUTTON,
        IDC_YOUTUBE_UNLINK_YES_BUTTON, IDC_YOUTUBE_UNLINK_NO_BUTTON, IDC_YOUTUBE_ACCOUNT_LINK, IDC_YOUTUBE_UPLOAD_BUTTON};
    for (const int id : recordingsButtons) {
        EnableOwnerDrawButton(ctx->recordingsPanel, id);
    }
    const std::array<int, 6> clipsButtons = {IDC_CLIPS_REFRESH, IDC_CLIPS_PLAY_PAUSE, IDC_CLIPS_SET_START, IDC_CLIPS_SET_END, IDC_CLIPS_EXPORT, IDC_CLIPS_OPEN_FOLDER};
    for (const int id : clipsButtons) {
        EnableOwnerDrawButton(ctx->clipsPanel, id);
    }
    const std::array<int, 4> aboutButtons = {
        IDC_ABOUT_WEBSITE_BUTTON, IDC_ABOUT_EMAIL_BUTTON, IDC_ABOUT_DISCORD_BUTTON, IDC_ABOUT_CHECK_UPDATES_BUTTON};
    for (const int id : aboutButtons) {
        EnableOwnerDrawButton(ctx->aboutPanel, id);
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
        || drawInfo->CtlID == IDC_TAB_RECORDINGS || drawInfo->CtlID == IDC_TAB_CLIPS || drawInfo->CtlID == IDC_TAB_ABOUT;
    const bool isLinkDisplay = drawInfo->CtlID == IDC_YOUTUBE_ACCOUNT_LINK;
    const bool isStatusTab = drawInfo->CtlID == IDC_TAB_STATUS;
    const bool isConfigurationTab = drawInfo->CtlID == IDC_TAB_CONFIGURATION;
    const bool showValidityIndicator = isStatusTab || isConfigurationTab;
    const bool isActiveTab = ctx
        && ((drawInfo->CtlID == IDC_TAB_STATUS && ctx->activeTab == AppContext::MainTab::Status)
            || (drawInfo->CtlID == IDC_TAB_CONFIGURATION && ctx->activeTab == AppContext::MainTab::Configuration)
            || (drawInfo->CtlID == IDC_TAB_CHAT_PRIVACY && ctx->activeTab == AppContext::MainTab::ChatPrivacy)
            || (drawInfo->CtlID == IDC_TAB_RECORDINGS && ctx->activeTab == AppContext::MainTab::Recordings)
            || (drawInfo->CtlID == IDC_TAB_CLIPS && ctx->activeTab == AppContext::MainTab::Clips)
            || (drawInfo->CtlID == IDC_TAB_ABOUT && ctx->activeTab == AppContext::MainTab::About));
    COLORREF fill = isLinkDisplay ? kColorInputBg : RGB(47, 60, 89);
    COLORREF border = isLinkDisplay ? kColorInputBorder : RGB(91, 114, 167);
    COLORREF text = kColorButtonText;
    if (isDisabled) {
        if (isLinkDisplay) { fill = kColorInputBg; border = kColorInputBorder; text = kColorTextMuted; }
        else { fill = RGB(32, 38, 55); border = RGB(56, 67, 95); text = RGB(127, 139, 167); }
    } else if (isPressed) { fill = RGB(73, 103, 166); border = RGB(118, 148, 212); }
    else if (isActiveTab) { fill = RGB(89, 120, 186); border = RGB(145, 176, 232); }
    else if (isHovered && isTab) { fill = RGB(54, 67, 98); border = RGB(97, 122, 174); }
    else if (isHovered) { fill = RGB(58, 72, 104); border = RGB(104, 129, 183); }
    else if (isTab) { fill = RGB(42, 51, 75); border = RGB(76, 94, 136); }

    SetBkMode(drawInfo->hDC, TRANSPARENT);
    HBRUSH clearBrush = CreateSolidBrush(kColorPanelBottom);
    if (clearBrush) { FillRect(drawInfo->hDC, &rc, clearBrush); DeleteObject(clearBrush); }
    const int cornerRadius = isTab ? 12 : (isLinkDisplay ? 6 : 9);
    HPEN borderPen = CreatePen(PS_SOLID, 1, border);
    HBRUSH fillBrush = CreateSolidBrush(fill);
    HGDIOBJ oldPen = borderPen ? SelectObject(drawInfo->hDC, borderPen) : nullptr;
    HGDIOBJ oldBrush = fillBrush ? SelectObject(drawInfo->hDC, fillBrush) : nullptr;
    RoundRect(drawInfo->hDC, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1, cornerRadius, cornerRadius);
    if (oldBrush) SelectObject(drawInfo->hDC, oldBrush);
    if (oldPen) SelectObject(drawInfo->hDC, oldPen);
    if (fillBrush) DeleteObject(fillBrush);
    if (borderPen) DeleteObject(borderPen);

    if (isActiveTab) {
        RECT accent = rc;
        accent.top += 3;
        accent.bottom = accent.top + 4;
        accent.left += 10;
        accent.right -= 10;
        HBRUSH accentBrush = CreateSolidBrush(RGB(189, 214, 255));
        if (accentBrush) { FillRect(drawInfo->hDC, &accent, accentBrush); DeleteObject(accentBrush); }
    }
    wchar_t textBuffer[256] = {};
    GetWindowTextW(drawInfo->hwndItem, textBuffer, static_cast<int>(std::size(textBuffer)));
    RECT textRect = rc;
    if (isPressed) {
        OffsetRect(&textRect, 0, 1);
    }
    SetTextColor(drawInfo->hDC, text);
    if (isLinkDisplay) {
        const COLORREF indicatorColor = ctx && ctx->youtubeLinked ? kColorSuccess : kColorFailure;
        const int centerY = (rc.top + rc.bottom) / 2;
        const int indicatorLeft = rc.left + 12;
        const int indicatorSize = 11;
        const int half = indicatorSize / 2;
        const int left = indicatorLeft;
        const int top = centerY - half;
        const int right = left + indicatorSize;
        const int bottom = top + indicatorSize;
        HPEN iconPen = CreatePen(PS_SOLID, 2, indicatorColor);
        HGDIOBJ oldIconPen = iconPen ? SelectObject(drawInfo->hDC, iconPen) : nullptr;
        if (ctx && ctx->youtubeLinked) {
            MoveToEx(drawInfo->hDC, left + 1, top + half, nullptr);
            LineTo(drawInfo->hDC, left + half - 1, bottom - 1);
            LineTo(drawInfo->hDC, right - 1, top + 1);
        } else {
            MoveToEx(drawInfo->hDC, left, top, nullptr);
            LineTo(drawInfo->hDC, right, bottom);
            MoveToEx(drawInfo->hDC, left, bottom, nullptr);
            LineTo(drawInfo->hDC, right, top);
        }
        if (oldIconPen) SelectObject(drawInfo->hDC, oldIconPen);
        if (iconPen) DeleteObject(iconPen);
        textRect.left += 34;
        textRect.right -= 8;
        DrawTextW(drawInfo->hDC, textBuffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        if (isTab) {
            textRect.left += 12;
            textRect.right -= showValidityIndicator ? 28 : 10;
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
    RECT rc = drawInfo->rcItem;
    HBRUSH clearBrush = CreateSolidBrush(kColorInputBg);
    if (clearBrush) { FillRect(drawInfo->hDC, &rc, clearBrush); DeleteObject(clearBrush); }
    if (drawInfo->CtlID == IDC_WOW_WINDOW_ICON
        || drawInfo->CtlID == IDC_OBS_INSTALL_ICON
        || drawInfo->CtlID == IDC_FFMPEG_ICON
        || drawInfo->CtlID == IDC_WARCRAFT_RECORDER_ICON
        || drawInfo->CtlID == IDC_ADVANCED_LOGGING_ICON) {
        bool isValid = false;
        if (drawInfo->CtlID == IDC_WOW_WINDOW_ICON) {
            isValid = ctx->wowWindowDetected;
        } else if (drawInfo->CtlID == IDC_OBS_INSTALL_ICON) {
            isValid = ctx->obsInstallDetected;
        } else if (drawInfo->CtlID == IDC_FFMPEG_ICON) {
            isValid = ctx->ffmpegDetected;
        } else if (drawInfo->CtlID == IDC_WARCRAFT_RECORDER_ICON) {
            isValid = !ctx->warcraftRecorderDetected;
        } else {
            isValid = ctx->advancedCombatLoggingEnabled;
        }
        const COLORREF indicatorColor = isValid ? RGB(80, 214, 142) : RGB(241, 100, 125);
        const int centerX = (rc.left + rc.right) / 2;
        const int centerY = (rc.top + rc.bottom) / 2;
        const int indicatorSize = 11;
        const int half = indicatorSize / 2;
        const int left = centerX - half;
        const int top = centerY - half;
        const int right = centerX + half;
        const int bottom = centerY + half;
        HPEN pen = CreatePen(PS_SOLID, 2, indicatorColor);
        HGDIOBJ oldPen = pen ? SelectObject(drawInfo->hDC, pen) : nullptr;
        if (isValid) {
            MoveToEx(drawInfo->hDC, left + 1, top + half, nullptr);
            LineTo(drawInfo->hDC, left + half - 1, bottom - 1);
            LineTo(drawInfo->hDC, right - 1, top + 1);
        } else {
            MoveToEx(drawInfo->hDC, left, top, nullptr);
            LineTo(drawInfo->hDC, right, bottom);
            MoveToEx(drawInfo->hDC, left, bottom, nullptr);
            LineTo(drawInfo->hDC, right, top);
        }
        if (oldPen) SelectObject(drawInfo->hDC, oldPen);
        if (pen) DeleteObject(pen);
        return;
    }
    const bool active = (drawInfo->CtlID == IDC_MONITOR_ICON) ? ctx->isMonitoring : ctx->isRecording;
    const COLORREF dotColor = active ? ((drawInfo->CtlID == IDC_MONITOR_ICON) ? RGB(80, 214, 142) : RGB(255, 112, 130)) : RGB(138, 151, 183);
    const COLORREF fillColor = active ? dotColor : RGB(45, 53, 76);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int diameter = (std::max)(6, (std::min)(10, (std::min)(width, height) - 2));
    const int left = rc.left + (width - diameter) / 2;
    const int top = rc.top + (height - diameter) / 2;
    const int right = left + diameter;
    const int bottom = top + diameter;
    HPEN pen = CreatePen(PS_SOLID, 1, dotColor);
    HBRUSH brush = CreateSolidBrush(fillColor);
    HGDIOBJ oldPen = pen ? SelectObject(drawInfo->hDC, pen) : nullptr;
    HGDIOBJ oldBrush = brush ? SelectObject(drawInfo->hDC, brush) : nullptr;
    Ellipse(drawInfo->hDC, left, top, right, bottom);
    if (oldBrush) SelectObject(drawInfo->hDC, oldBrush);
    if (oldPen) SelectObject(drawInfo->hDC, oldPen);
    if (brush) DeleteObject(brush);
    if (pen) DeleteObject(pen);
}

void DrawLengthValue(const DRAWITEMSTRUCT* drawInfo)
{
    if (!drawInfo) {
        return;
    }
    RECT rc = drawInfo->rcItem;
    HBRUSH clearBrush = CreateSolidBrush(kColorPanelBottom);
    if (clearBrush) { FillRect(drawInfo->hDC, &rc, clearBrush); DeleteObject(clearBrush); }
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
    HBRUSH clearBrush = CreateSolidBrush(kColorPanelBottom);
    if (clearBrush) { FillRect(drawInfo->hDC, &rc, clearBrush); DeleteObject(clearBrush); }
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int diameter = (std::max)(10, (std::min)(14, (std::min)(width, height) - 2));
    const int left = rc.left + (width - diameter) / 2;
    const int top = rc.top + (height - diameter) / 2;
    const int right = left + diameter;
    const int bottom = top + diameter;
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(129, 147, 188));
    HBRUSH fillBrush = CreateSolidBrush(RGB(35, 44, 64));
    HGDIOBJ oldPen = borderPen ? SelectObject(drawInfo->hDC, borderPen) : nullptr;
    HGDIOBJ oldBrush = fillBrush ? SelectObject(drawInfo->hDC, fillBrush) : nullptr;
    Ellipse(drawInfo->hDC, left, top, right, bottom);
    if (oldBrush) SelectObject(drawInfo->hDC, oldBrush);
    if (oldPen) SelectObject(drawInfo->hDC, oldPen);
    if (fillBrush) DeleteObject(fillBrush);
    if (borderPen) DeleteObject(borderPen);
    RECT textRect{left, top, right, bottom};
    SetBkMode(drawInfo->hDC, TRANSPARENT);
    SetTextColor(drawInfo->hDC, RGB(225, 234, 252));
    if (gTheme.uiFont) {
        SelectObject(drawInfo->hDC, gTheme.uiFont);
    }
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
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(84, 97, 132));
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
    HPEN gridPen = CreatePen(PS_SOLID, 1, kColorListGrid);
    HGDIOBJ oldPen = gridPen ? SelectObject(customDraw->nmcd.hdc, gridPen) : nullptr;
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
    if (gridPen) DeleteObject(gridPen);
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
    HBRUSH clearBrush = CreateSolidBrush(kColorPanelBottom);
    if (clearBrush) { FillRect(drawInfo->hDC, &rc, clearBrush); DeleteObject(clearBrush); }
    const bool linked = ctx->youtubeLinked;
    const COLORREF indicatorColor = linked ? kColorSuccess : kColorFailure;
    const int centerX = (rc.left + rc.right) / 2;
    const int centerY = (rc.top + rc.bottom) / 2;
    const int indicatorSize = 11;
    const int half = indicatorSize / 2;
    const int left = centerX - half;
    const int top = centerY - half;
    const int right = centerX + half;
    const int bottom = centerY + half;
    HPEN pen = CreatePen(PS_SOLID, 2, indicatorColor);
    HGDIOBJ oldPen = pen ? SelectObject(drawInfo->hDC, pen) : nullptr;
    if (linked) {
        MoveToEx(drawInfo->hDC, left + 1, top + half, nullptr);
        LineTo(drawInfo->hDC, left + half - 1, bottom - 1);
        LineTo(drawInfo->hDC, right - 1, top + 1);
    } else {
        MoveToEx(drawInfo->hDC, left, top, nullptr);
        LineTo(drawInfo->hDC, right, bottom);
        MoveToEx(drawInfo->hDC, left, bottom, nullptr);
        LineTo(drawInfo->hDC, right, top);
    }
    if (oldPen) SelectObject(drawInfo->hDC, oldPen);
    if (pen) DeleteObject(pen);
}
