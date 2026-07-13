#include "app/AppContext.h"
#include "app/AppDraw.h"
#include "app/AppIconsTaskbar.h"
#include "app/AppLayout.h"
#include "app/AppRecordingHelpers.h"
#include "app/AppStatusLog.h"
#include "app/AppUtilities.h"
#include "app/BeanUpdater.h"
#include "integrations/YouTubeUploader.h"
#include "obs/LibObsRecorderEngine.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mfmediaengine.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "Gdiplus.lib")

namespace {

constexpr char kYouTubeAuthServerUrl[] = "https://andrew.gg/bean/youtube-auth/";

std::wstring VersionText()
{
    constexpr wchar_t kAppVersion[] = L"0.2.3";
    return std::wstring(L"v") + kAppVersion;
}

std::wstring MainWindowTitleText()
{
    return std::wstring(kWindowTitleBase) + std::wstring(L" - ") + VersionText();
}

std::wstring ToWide(const std::string& input)
{
    if (input.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, output.data(), size);
    if (!output.empty() && output.back() == L'\0') {
        output.pop_back();
    }
    return output;
}

std::string ToUtf8(const std::wstring& input)
{
    if (input.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, output.data(), size, nullptr, nullptr);
    if (!output.empty() && output.back() == '\0') {
        output.pop_back();
    }
    return output;
}

std::wstring GetWindowTextString(HWND hwnd)
{
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

bool DirectoryExists(const std::wstring& path);
std::string GetEnvString(const char* name);
std::string Trim(std::string value);
std::vector<std::filesystem::path> EnumerateDriveRootsStartingAtC();
bool ResolveObsInstallRootForUi(std::filesystem::path& root);

void UpdateTransparentStaticText(HWND control, const wchar_t* newText)
{
    if (!control || !newText) {
        return;
    }
    if (GetWindowTextString(control) == newText) {
        return;
    }

    SetWindowTextW(control, newText);

    // Transparent STATIC controls rely on parent repaint; redraw both control and
    // parent region so stale glyphs are erased before the new text is drawn.
    RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    HWND parent = GetParent(control);
    if (parent) {
        RECT rect{};
        if (GetWindowRect(control, &rect)) {
            MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rect), 2);
            RedrawWindow(parent, &rect, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        }
    }
}

void InvalidateControlAndParentRegion(HWND control)
{
    if (!control) {
        return;
    }
    // Keep redraw local to avoid full-panel flash/flicker on every interaction.
    InvalidateRect(control, nullptr, FALSE);
}

void UpdateClipsPositionLabel(AppContext* ctx);
constexpr int kClipsTimelineMax = 1000;
bool ParseClipSeconds(const std::wstring& input, int& outSeconds);
constexpr int kClipsSliderInsetPx = 6;
constexpr int kClipsTimelineThumbWidthPx = 10;
constexpr int kClipsVolumeThumbWidthPx = 8;

int SliderValueFromPointX(const RECT& rc, int x, int minValue, int maxValue, int thumbWidth)
{
    const int width = (std::max)(1, static_cast<int>(rc.right - rc.left));
    const int trackLeft = kClipsSliderInsetPx;
    const int trackRight = (std::max)(trackLeft + 1, width - kClipsSliderInsetPx);
    const int minCenter = trackLeft + thumbWidth / 2;
    const int maxCenter = (std::max)(minCenter + 1, trackRight - (thumbWidth - thumbWidth / 2));
    const int clampedX = (std::clamp)(x, minCenter, maxCenter);
    const double ratio = static_cast<double>(clampedX - minCenter) / static_cast<double>((std::max)(1, maxCenter - minCenter));
    const int value = minValue + static_cast<int>(std::lround(ratio * static_cast<double>(maxValue - minValue)));
    return (std::clamp)(value, minValue, maxValue);
}

void ApplyClipVolumePercent(AppContext* ctx, int percent)
{
    if (!ctx) {
        return;
    }
    ctx->clipsVolumePercent = (std::clamp)(percent, 0, 100);
    if (ctx->clipsPreviewEngine) {
        ctx->clipsPreviewEngine->SetVolumePercent(ctx->clipsVolumePercent);
    }
}

void SeekClipTimelineToSliderPosition(AppContext* ctx)
{
    if (!ctx || !ctx->clipsLoaded || !ctx->clipsPreviewEngine) {
        return;
    }
    const int targetMs = static_cast<int>((static_cast<long long>(ctx->clipsTimelinePosition) * (std::max)(1, ctx->clipsDurationMs)) / kClipsTimelineMax);
    ctx->clipsPreviewEngine->SeekMilliseconds(targetMs);
    if (ctx->clipsIsPlaying) {
        ctx->clipsPreviewEngine->Play();
    }
}

void UpdateClipsTimelineFromMouse(AppContext* ctx, HWND control, LPARAM lParam)
{
    if (!ctx || !control || !ctx->clipsLoaded) {
        return;
    }
    RECT rc{};
    GetClientRect(control, &rc);
    const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
    ctx->clipsTimelinePosition = SliderValueFromPointX(rc, x, 0, kClipsTimelineMax, kClipsTimelineThumbWidthPx);
    SeekClipTimelineToSliderPosition(ctx);
    InvalidateControlAndParentRegion(control);
}

void UpdateClipsVolumeFromMouse(AppContext* ctx, HWND control, LPARAM lParam)
{
    if (!ctx || !control || !ctx->clipsLoaded) {
        return;
    }
    RECT rc{};
    GetClientRect(control, &rc);
    const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
    ApplyClipVolumePercent(ctx, SliderValueFromPointX(rc, x, 0, 100, kClipsVolumeThumbWidthPx));
    InvalidateControlAndParentRegion(control);
}

void DrawClipsSlider(const DRAWITEMSTRUCT* drawInfo, const AppContext* ctx, bool isTimeline)
{
    if (!drawInfo || !ctx) {
        return;
    }
    RECT rc = drawInfo->rcItem;
    HBRUSH panelBrush = CreateSolidBrush(kColorPanelBottom);
    if (panelBrush) {
        FillRect(drawInfo->hDC, &rc, panelBrush);
        DeleteObject(panelBrush);
    }

    RECT track = rc;
    track.left += kClipsSliderInsetPx;
    track.right -= kClipsSliderInsetPx;
    track.top += 8;
    track.bottom -= 8;
    if (track.right <= track.left) {
        return;
    }

    HBRUSH trackBrush = CreateSolidBrush(RGB(13, 18, 29));
    if (trackBrush) {
        FillRect(drawInfo->hDC, &track, trackBrush);
        DeleteObject(trackBrush);
    }
    HPEN borderPen = CreatePen(PS_SOLID, 1, kColorInputBorder);
    HGDIOBJ oldPen = borderPen ? SelectObject(drawInfo->hDC, borderPen) : nullptr;
    HGDIOBJ oldBrush = SelectObject(drawInfo->hDC, GetStockObject(HOLLOW_BRUSH));
    Rectangle(drawInfo->hDC, track.left, track.top, track.right, track.bottom);
    if (oldBrush) {
        SelectObject(drawInfo->hDC, oldBrush);
    }

    if (isTimeline && ctx->clipsLoaded && ctx->clipsDurationMs > 0) {
        int startSeconds = 0;
        int endSeconds = 0;
        if (ParseClipSeconds(GetWindowTextString(ctx->clipsStartEdit), startSeconds)
            && ParseClipSeconds(GetWindowTextString(ctx->clipsEndEdit), endSeconds)
            && endSeconds > startSeconds) {
            const int durationSeconds = (std::max)(1, ctx->clipsDurationMs / 1000);
            startSeconds = (std::clamp)(startSeconds, 0, durationSeconds);
            endSeconds = (std::clamp)(endSeconds, 0, durationSeconds);
            if (endSeconds > startSeconds) {
                const int trackLeft = static_cast<int>(track.left);
                const int trackRight = static_cast<int>(track.right);
                const int trackWidth = (std::max)(1, trackRight - trackLeft);
                const int selectedLeft = trackLeft + static_cast<int>((static_cast<long long>(startSeconds) * trackWidth) / durationSeconds);
                const int selectedRight = trackLeft + static_cast<int>((static_cast<long long>(endSeconds) * trackWidth) / durationSeconds);

                RECT selectedRect{
                    (std::clamp)(selectedLeft, trackLeft, trackRight),
                    track.top + 1,
                    (std::clamp)(selectedRight, trackLeft, trackRight),
                    track.bottom - 1};
                if (selectedRect.right > selectedRect.left) {
                    HBRUSH selectedBrush = CreateSolidBrush(RGB(73, 103, 166));
                    if (selectedBrush) {
                        FillRect(drawInfo->hDC, &selectedRect, selectedBrush);
                        DeleteObject(selectedBrush);
                    }
                }

                HPEN markerPen = CreatePen(PS_SOLID, 1, RGB(189, 214, 255));
                HGDIOBJ oldMarkerPen = markerPen ? SelectObject(drawInfo->hDC, markerPen) : nullptr;
                const int markerTop = track.top - 2;
                const int markerBottom = track.bottom + 2;
                MoveToEx(drawInfo->hDC, selectedRect.left, markerTop, nullptr);
                LineTo(drawInfo->hDC, selectedRect.left, markerBottom);
                MoveToEx(drawInfo->hDC, selectedRect.right - 1, markerTop, nullptr);
                LineTo(drawInfo->hDC, selectedRect.right - 1, markerBottom);
                if (oldMarkerPen) {
                    SelectObject(drawInfo->hDC, oldMarkerPen);
                }
                if (markerPen) {
                    DeleteObject(markerPen);
                }
            }
        }
    }

    const int sliderValue = isTimeline ? ctx->clipsTimelinePosition : ctx->clipsVolumePercent;
    const int sliderMax = isTimeline ? kClipsTimelineMax : 100;
    const int thumbWidth = isTimeline ? kClipsTimelineThumbWidthPx : kClipsVolumeThumbWidthPx;
    const int trackLeft = static_cast<int>(track.left);
    const int trackRight = static_cast<int>(track.right);
    const int minCenter = trackLeft + thumbWidth / 2;
    const int maxCenter = (std::max)(minCenter + 1, trackRight - (thumbWidth - thumbWidth / 2));
    const int thumbCenter = minCenter + static_cast<int>((static_cast<long long>(sliderValue) * (maxCenter - minCenter)) / (std::max)(1, sliderMax));
    const int thumbLeft = thumbCenter - (thumbWidth / 2);
    RECT thumb{thumbLeft, track.top - 2, thumbLeft + thumbWidth, track.bottom + 2};
    HBRUSH thumbBrush = CreateSolidBrush(RGB(233, 239, 251));
    if (thumbBrush) {
        FillRect(drawInfo->hDC, &thumb, thumbBrush);
        DeleteObject(thumbBrush);
    }
    if (borderPen && oldPen) {
        SelectObject(drawInfo->hDC, oldPen);
    }
    if (borderPen) {
        DeleteObject(borderPen);
    }
}

LRESULT CALLBACK ClipsSliderSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData)
{
    auto* ctx = reinterpret_cast<AppContext*>(refData);
    if (!ctx) {
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }
    const int controlId = GetDlgCtrlID(hwnd);
    const bool isTimeline = controlId == IDC_CLIPS_TIMELINE;
    const bool isVolume = controlId == IDC_CLIPS_VOLUME_SLIDER;
    if (!isTimeline && !isVolume) {
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        if (isTimeline) {
            ctx->clipsTimelineDragActive = true;
            ctx->clipsTimelineScrubbing = true;
            UpdateClipsTimelineFromMouse(ctx, hwnd, lParam);
            UpdateClipsPositionLabel(ctx);
        } else {
            ctx->clipsVolumeDragActive = true;
            UpdateClipsVolumeFromMouse(ctx, hwnd, lParam);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd) {
            if (isTimeline && ctx->clipsTimelineDragActive) {
                UpdateClipsTimelineFromMouse(ctx, hwnd, lParam);
                UpdateClipsPositionLabel(ctx);
            } else if (isVolume && ctx->clipsVolumeDragActive) {
                UpdateClipsVolumeFromMouse(ctx, hwnd, lParam);
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) {
            if (isTimeline && ctx->clipsTimelineDragActive) {
                UpdateClipsTimelineFromMouse(ctx, hwnd, lParam);
                UpdateClipsPositionLabel(ctx);
            } else if (isVolume && ctx->clipsVolumeDragActive) {
                UpdateClipsVolumeFromMouse(ctx, hwnd, lParam);
            }
            ReleaseCapture();
            ctx->clipsTimelineDragActive = false;
            ctx->clipsVolumeDragActive = false;
            ctx->clipsTimelineScrubbing = false;
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        ctx->clipsTimelineDragActive = false;
        ctx->clipsVolumeDragActive = false;
        ctx->clipsTimelineScrubbing = false;
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ClipsSliderSubclassProc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

int ReadIntControl(HWND hwnd, int fallback)
{
    const auto text = GetWindowTextString(hwnd);
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

int ChatBlockerAnchorToComboIndex(bean::core::AppSettings::ChatBlockerAnchor anchor)
{
    switch (anchor) {
    case bean::core::AppSettings::ChatBlockerAnchor::BottomRight:
        return 1;
    case bean::core::AppSettings::ChatBlockerAnchor::TopLeft:
        return 2;
    case bean::core::AppSettings::ChatBlockerAnchor::TopRight:
        return 3;
    case bean::core::AppSettings::ChatBlockerAnchor::BottomLeft:
    default:
        return 0;
    }
}

bean::core::AppSettings::ChatBlockerAnchor ChatBlockerAnchorFromComboIndex(int index)
{
    switch (index) {
    case 1:
        return bean::core::AppSettings::ChatBlockerAnchor::BottomRight;
    case 2:
        return bean::core::AppSettings::ChatBlockerAnchor::TopLeft;
    case 3:
        return bean::core::AppSettings::ChatBlockerAnchor::TopRight;
    case 0:
    default:
        return bean::core::AppSettings::ChatBlockerAnchor::BottomLeft;
    }
}

std::wstring PickFolder(HWND owner)
{
    BROWSEINFOW browseInfo{};
    browseInfo.hwndOwner = owner;
    browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    browseInfo.lpszTitle = L"Select folder";

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&browseInfo);
    if (!pidl) {
        return {};
    }

    wchar_t pathBuffer[MAX_PATH] = {};
    std::wstring selected;
    if (SHGetPathFromIDListW(pidl, pathBuffer)) {
        selected = pathBuffer;
    }
    CoTaskMemFree(pidl);
    return selected;
}

std::wstring PickImageFile(HWND owner)
{
    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW openFile{};
    openFile.lStructSize = sizeof(openFile);
    openFile.hwndOwner = owner;
    openFile.lpstrFilter =
        L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0"
        L"All Files (*.*)\0*.*\0";
    openFile.lpstrFile = filePath;
    openFile.nMaxFile = static_cast<DWORD>(std::size(filePath));
    openFile.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    openFile.lpstrTitle = L"Choose chat blocker image";
    if (!GetOpenFileNameW(&openFile)) {
        return {};
    }
    return filePath;
}

std::wstring FormatClipTimeMs(int milliseconds)
{
    const int clamped = (std::max)(0, milliseconds);
    const int totalSeconds = clamped / 1000;
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds / 60) % 60;
    const int seconds = totalSeconds % 60;
    wchar_t buffer[24] = {};
    swprintf_s(buffer, L"%02d:%02d:%02d", hours, minutes, seconds);
    return buffer;
}

std::wstring BuildClipPositionText(int currentMs, int totalMs)
{
    return FormatClipTimeMs(currentMs) + L" / " + FormatClipTimeMs(totalMs);
}

std::filesystem::path ResolveRecordingsFolderPath(const AppContext* ctx)
{
    if (!ctx) {
        return {};
    }
    std::wstring folder = GetWindowTextString(ctx->outputEdit);
    if (folder.empty()) {
        folder = ToWide(ctx->settings.outputDirectory.string());
    }
    if (folder.empty()) {
        return {};
    }
    return std::filesystem::path(folder);
}

std::filesystem::path ResolveClipsOutputFolderPath(const AppContext* ctx)
{
    const auto recordingsFolder = ResolveRecordingsFolderPath(ctx);
    if (recordingsFolder.empty()) {
        return {};
    }
    return recordingsFolder / "Clips";
}

std::wstring FormatHresultHex(HRESULT hr)
{
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

struct ClipExportCompletePayload {
    bool success = false;
    std::wstring message;
};

void SetClipsExportStatus(AppContext* ctx, AppContext::ClipExportStatus status, const std::wstring& text)
{
    if (!ctx) {
        return;
    }
    ctx->clipsExportStatus = status;
    if (ctx->mainWindow) {
        KillTimer(ctx->mainWindow, kClipsExportStatusTimerId);
    }
    if (ctx->clipsFfmpegWarning) {
        UpdateTransparentStaticText(ctx->clipsFfmpegWarning, text.c_str());
        ShowWindow(ctx->clipsFfmpegWarning, text.empty() ? SW_HIDE : SW_SHOW);
    }
    if (status == AppContext::ClipExportStatus::Success && ctx->mainWindow) {
        SetTimer(ctx->mainWindow, kClipsExportStatusTimerId, 10'000, nullptr);
    }
}

void ClearClipsExportStatus(AppContext* ctx)
{
    SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Idle, L"");
}

bool ParseClipSeconds(const std::wstring& input, int& outSeconds)
{
    if (input.empty()) {
        return false;
    }
    try {
        size_t consumed = 0;
        const int parsed = std::stoi(input, &consumed);
        if (consumed != input.size()) {
            return false;
        }
        outSeconds = (std::max)(0, parsed);
        return true;
    } catch (...) {
        return false;
    }
}

int QueryClipPositionMs(const AppContext* ctx, int fallback = 0)
{
    if (!ctx || !ctx->clipsPreviewEngine) {
        return fallback;
    }
    return ctx->clipsPreviewEngine->PositionMilliseconds();
}

void CloseClipMedia(AppContext* ctx);

void ApplyClipVideoWindowBounds(AppContext* ctx)
{
    if (!ctx || !ctx->clipsVideoSurface) {
        return;
    }
    InvalidateRect(ctx->clipsVideoSurface, nullptr, FALSE);
}

void CloseClipMedia(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->clipsPreviewEngine) {
        ctx->clipsPreviewEngine->Close();
    }
    ctx->clipsLoaded = false;
    ctx->clipsIsPlaying = false;
    ctx->clipsDurationMs = 0;
    ctx->clipsVideoSourceWidth = 0;
    ctx->clipsVideoSourceHeight = 0;
    ctx->clipsLoadedPath.clear();
    ctx->clipsTimelineScrubbing = false;
}

void UpdateClipsPositionLabel(AppContext* ctx)
{
    if (!ctx || !ctx->clipsPositionText) {
        return;
    }
    int currentMs = 0;
    if (ctx->clipsLoaded) {
        currentMs = QueryClipPositionMs(ctx, 0);
    }
    const auto text = BuildClipPositionText(currentMs, ctx->clipsDurationMs);
    UpdateTransparentStaticText(ctx->clipsPositionText, text.c_str());
}

void RefreshClipsPlaybackControls(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    const bool previewBusy = false;
    const bool ffmpegAvailable = ctx->ffmpegDetected;
    if (ctx->clipsFfmpegWarning) {
        if (ctx->clipsExportStatus == AppContext::ClipExportStatus::Idle) {
            const wchar_t* warningText = ffmpegAvailable
                ? L""
                : L"FFmpeg is required to export clips.";
            UpdateTransparentStaticText(ctx->clipsFfmpegWarning, warningText);
            ShowWindow(ctx->clipsFfmpegWarning, ffmpegAvailable ? SW_HIDE : SW_SHOW);
        } else {
            ShowWindow(ctx->clipsFfmpegWarning, SW_SHOW);
        }
    }
    if (ctx->clipsSourceCombo) {
        EnableWindow(ctx->clipsSourceCombo, TRUE);
    }
    EnableWindow(GetDlgItem(ctx->clipsPanel, IDC_CLIPS_REFRESH), previewBusy ? FALSE : TRUE);
    if (ctx->clipsPlayPauseButton) {
        SetWindowTextW(ctx->clipsPlayPauseButton, ctx->clipsIsPlaying ? L"Pause" : L"Play");
        EnableWindow(ctx->clipsPlayPauseButton, (ctx->clipsLoaded && !previewBusy) ? TRUE : FALSE);
    }
    if (ctx->clipsTimeline) {
        EnableWindow(ctx->clipsTimeline, (ctx->clipsLoaded && !previewBusy) ? TRUE : FALSE);
        InvalidateControlAndParentRegion(ctx->clipsTimeline);
    }
    if (ctx->clipsVolumeSlider) {
        EnableWindow(ctx->clipsVolumeSlider, (ctx->clipsLoaded && !previewBusy) ? TRUE : FALSE);
        InvalidateControlAndParentRegion(ctx->clipsVolumeSlider);
    }
    if (ctx->clipsStartEdit) {
        EnableWindow(ctx->clipsStartEdit, (ctx->clipsLoaded && !previewBusy) ? TRUE : FALSE);
    }
    if (ctx->clipsEndEdit) {
        EnableWindow(ctx->clipsEndEdit, (ctx->clipsLoaded && !previewBusy) ? TRUE : FALSE);
    }
    EnableWindow(GetDlgItem(ctx->clipsPanel, IDC_CLIPS_SET_START), (ctx->clipsLoaded && !previewBusy) ? TRUE : FALSE);
    EnableWindow(GetDlgItem(ctx->clipsPanel, IDC_CLIPS_SET_END), (ctx->clipsLoaded && !previewBusy) ? TRUE : FALSE);
    EnableWindow(
        GetDlgItem(ctx->clipsPanel, IDC_CLIPS_EXPORT),
        (ctx->clipsLoaded && ffmpegAvailable && !previewBusy && !ctx->clipsExportInProgress.load()) ? TRUE : FALSE);
    UpdateClipsPositionLabel(ctx);
}

std::optional<std::filesystem::path> GetSelectedClipSourcePath(const AppContext* ctx)
{
    if (!ctx || !ctx->clipsSourceCombo) {
        return std::nullopt;
    }
    const int selected = static_cast<int>(SendMessageW(ctx->clipsSourceCombo, CB_GETCURSEL, 0, 0));
    if (selected < 0 || static_cast<size_t>(selected) >= ctx->clipSourceItems.size()) {
        return std::nullopt;
    }
    return ctx->clipSourceItems[static_cast<size_t>(selected)];
}

std::optional<std::filesystem::path> ResolveFfmpegExecutablePath(AppContext* ctx)
{
    if (ctx && ctx->ffmpegExecutablePath.has_value()) {
        std::error_code cachedPathEc;
        if (std::filesystem::exists(*ctx->ffmpegExecutablePath, cachedPathEc) && !cachedPathEc) {
            return ctx->ffmpegExecutablePath;
        }
        ctx->ffmpegExecutablePath.reset();
    }

    std::optional<std::filesystem::path> resolvedPath;
    if (const auto explicitPath = Trim(GetEnvString("BEAN_FFMPEG_PATH")); !explicitPath.empty()) {
        const auto candidate = std::filesystem::path(ToWide(explicitPath));
        if (std::filesystem::exists(candidate)) {
            resolvedPath = candidate;
        }
    }

    if (!resolvedPath.has_value()) {
        const auto obsRootText = Trim(GetEnvString("BEAN_OBS_ROOT"));
        if (!obsRootText.empty()) {
        const auto candidate = std::filesystem::path(ToWide(obsRootText)) / "bin" / "64bit" / "ffmpeg.exe";
        if (std::filesystem::exists(candidate)) {
                resolvedPath = candidate;
            }
        }
    }

    std::filesystem::path obsRoot;
    if (!resolvedPath.has_value() && ResolveObsInstallRootForUi(obsRoot)) {
        const auto candidate = obsRoot / "bin" / "64bit" / "ffmpeg.exe";
        if (std::filesystem::exists(candidate)) {
            resolvedPath = candidate;
        }
    }

    if (!resolvedPath.has_value()) {
        for (const auto& driveRoot : EnumerateDriveRootsStartingAtC()) {
            const std::filesystem::path commonCandidates[] = {
                driveRoot / "ffmpeg" / "bin" / "ffmpeg.exe",
                driveRoot / "Program Files" / "ffmpeg" / "bin" / "ffmpeg.exe",
                driveRoot / "Program Files (x86)" / "ffmpeg" / "bin" / "ffmpeg.exe",
                driveRoot / "ProgramData" / "chocolatey" / "bin" / "ffmpeg.exe"
            };
            for (const auto& candidate : commonCandidates) {
                if (std::filesystem::exists(candidate)) {
                    resolvedPath = candidate;
                    break;
                }
            }
            if (resolvedPath.has_value()) {
                break;
            }
        }
    }

    if (!resolvedPath.has_value()) {
        wchar_t searchPath[MAX_PATH] = {};
        const DWORD resolvedLen = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(std::size(searchPath)), searchPath, nullptr);
        if (resolvedLen > 0 && resolvedLen < std::size(searchPath)) {
            resolvedPath = std::filesystem::path(searchPath);
        }
    }

    if (!resolvedPath.has_value()) {
        wchar_t modulePath[MAX_PATH] = {};
        const DWORD moduleLen = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        if (moduleLen > 0) {
            const auto localCandidate = std::filesystem::path(modulePath).parent_path() / "ffmpeg.exe";
            if (std::filesystem::exists(localCandidate)) {
                resolvedPath = localCandidate;
            }
        }
    }

    if (ctx) {
        ctx->ffmpegExecutablePath = resolvedPath;
    }
    return resolvedPath;
}

bool LoadClipFromSelection(AppContext* ctx, bool reportStatus = true)
{
    if (!ctx || !ctx->clipsSourceCombo) {
        return false;
    }
    const int selected = static_cast<int>(SendMessageW(ctx->clipsSourceCombo, CB_GETCURSEL, 0, 0));
    if (selected < 0 || static_cast<size_t>(selected) >= ctx->clipSourceItems.size()) {
        CloseClipMedia(ctx);
        ctx->clipsTimelinePosition = 0;
        if (ctx->clipsStartEdit) {
            SetWindowTextW(ctx->clipsStartEdit, L"0");
        }
        if (ctx->clipsEndEdit) {
            SetWindowTextW(ctx->clipsEndEdit, L"0");
        }
        RefreshClipsPlaybackControls(ctx);
        return false;
    }

    const auto selectedPath = ctx->clipSourceItems[static_cast<size_t>(selected)];
    if (!std::filesystem::exists(selectedPath)) {
        CloseClipMedia(ctx);
        RefreshClipsPlaybackControls(ctx);
        if (reportStatus) {
            SetStatus(ctx, L"Selected clip file is unavailable.");
        }
        return false;
    }
    if (ctx->clipsLoaded && ctx->clipsLoadedPath.lexically_normal() == selectedPath.lexically_normal()) {
        RefreshClipsPlaybackControls(ctx);
        return true;
    }

    if (!ctx->clipsPreviewEngine) {
        ctx->clipsPreviewEngine = std::make_unique<ClipPreviewEngine>(ctx->mainWindow, ctx->clipsVideoSurface);
        const HRESULT initializeHr = ctx->clipsPreviewEngine->Initialize();
        if (FAILED(initializeHr)) {
            if (reportStatus) {
                SetStatus(ctx, L"Could not initialize Media Foundation clip preview (" + FormatHresultHex(initializeHr) + L").");
            }
            RefreshClipsPlaybackControls(ctx);
            return false;
        }
    }

    if (!ctx->clipsPreviewEngine->IsReady()) {
        if (!ctx->clipsLoaded || ctx->clipsLoadedPath.lexically_normal() != selectedPath.lexically_normal()) {
            CloseClipMedia(ctx);
            ctx->clipsLoadedPath = selectedPath;
            const HRESULT openHr = ctx->clipsPreviewEngine->Open(selectedPath);
            if (FAILED(openHr)) {
                ctx->clipsLoadedPath.clear();
                if (reportStatus) {
                    SetStatus(ctx, L"Could not load clip preview with Media Foundation (" + FormatHresultHex(openHr) + L").");
                }
                RefreshClipsPlaybackControls(ctx);
                return false;
            }
            if (reportStatus) {
                SetStatus(ctx, L"Loading clip preview...");
            }
        }
        ctx->clipsLoaded = false;
        RefreshClipsPlaybackControls(ctx);
        return false;
    }

    ApplyClipVideoWindowBounds(ctx);
    const auto nativeSize = ctx->clipsPreviewEngine->NativeVideoSize();
    ctx->clipsVideoSourceWidth = nativeSize.first;
    ctx->clipsVideoSourceHeight = nativeSize.second;
    ctx->clipsDurationMs = ctx->clipsPreviewEngine->DurationMilliseconds();
    if (ctx->clipsDurationMs <= 0) {
        // Some graph combinations fail duration probe until playback advances;
        // keep controls usable and let live position updates refine state.
        ctx->clipsDurationMs = 1000;
    }
    ctx->clipsLoaded = true;
    ctx->clipsIsPlaying = false;
    ctx->clipsLoadedPath = selectedPath;

    ctx->clipsTimelinePosition = 0;
    if (ctx->clipsStartEdit) {
        SetWindowTextW(ctx->clipsStartEdit, L"0");
    }
    if (ctx->clipsEndEdit) {
        SetWindowTextW(ctx->clipsEndEdit, std::to_wstring(ctx->clipsDurationMs / 1000).c_str());
    }
    ApplyClipVolumePercent(ctx, ctx->clipsVolumePercent);
    RefreshClipsPlaybackControls(ctx);
    if (reportStatus) {
        SetStatus(ctx, std::wstring(L"Loaded clip: ") + selectedPath.filename().wstring());
    }
    return true;
}

void RefreshClipsSourceList(AppContext* ctx)
{
    if (!ctx || !ctx->clipsSourceCombo) {
        return;
    }

    const auto folderPath = ResolveRecordingsFolderPath(ctx);
    const std::wstring existingSelection = GetWindowTextString(ctx->clipsSourceCombo);
    ctx->clipSourceItems.clear();
    SendMessageW(ctx->clipsSourceCombo, CB_RESETCONTENT, 0, 0);

    if (folderPath.empty() || !std::filesystem::exists(folderPath)) {
        CloseClipMedia(ctx);
        RefreshClipsPlaybackControls(ctx);
        SetStatus(ctx, L"Recordings folder unavailable for clips.");
        return;
    }

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folderPath, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto ext = entry.path().extension().wstring();
        if (_wcsicmp(ext.c_str(), L".mp4") != 0 && _wcsicmp(ext.c_str(), L".mkv") != 0) {
            continue;
        }
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        std::error_code aEc;
        std::error_code bEc;
        const auto aTime = std::filesystem::last_write_time(a, aEc);
        const auto bTime = std::filesystem::last_write_time(b, bEc);
        if (!aEc && !bEc) {
            return aTime > bTime;
        }
        return a.filename().wstring() < b.filename().wstring();
    });

    int restoreSelectionIndex = -1;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        ctx->clipSourceItems.push_back(file);
        const auto display = file.filename().wstring();
        const LRESULT row = SendMessageW(ctx->clipsSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        if (row >= 0 && _wcsicmp(existingSelection.c_str(), display.c_str()) == 0) {
            restoreSelectionIndex = static_cast<int>(i);
        }
    }

    if (!ctx->clipSourceItems.empty()) {
        if (restoreSelectionIndex < 0) {
            restoreSelectionIndex = 0;
        }
        SendMessageW(ctx->clipsSourceCombo, CB_SETCURSEL, static_cast<WPARAM>(restoreSelectionIndex), 0);
        const auto selectedPath = ctx->clipSourceItems[static_cast<size_t>(restoreSelectionIndex)];
        bool alreadyLoadedSelection = false;
        if (ctx->clipsLoaded && !ctx->clipsLoadedPath.empty()) {
            alreadyLoadedSelection = (ctx->clipsLoadedPath.lexically_normal() == selectedPath.lexically_normal());
        }
        if (!alreadyLoadedSelection) {
            LoadClipFromSelection(ctx, true);
        } else {
            UpdateClipsPositionLabel(ctx);
            RefreshClipsPlaybackControls(ctx);
        }
    } else {
        CloseClipMedia(ctx);
        RefreshClipsPlaybackControls(ctx);
    }
}

void SyncClipTimelineFromPlayback(AppContext* ctx)
{
    if (!ctx || !ctx->clipsLoaded || !ctx->clipsTimeline || ctx->clipsTimelineScrubbing || ctx->clipsDurationMs <= 0) {
        return;
    }
    const int currentMs = QueryClipPositionMs(ctx, 0);
    if (!ctx->clipsIsPlaying && currentMs == 0 && ctx->clipsTimelinePosition > 0) {
        // Media Foundation can briefly report zero while a paused seek is
        // being applied. Do not overwrite the user's seek with that transient
        // value; the next media event/timer tick will provide the real time.
        return;
    }
    ctx->clipsTimelinePosition = (std::clamp)(static_cast<int>((static_cast<long long>(currentMs) * kClipsTimelineMax) / (std::max)(1, ctx->clipsDurationMs)), 0, kClipsTimelineMax);
    InvalidateControlAndParentRegion(ctx->clipsTimeline);
    UpdateClipsPositionLabel(ctx);
}

std::filesystem::path ResolveChatBlockerImagesDirectory(const AppContext* ctx)
{
    if (!ctx) {
        return {};
    }
    const auto appDataDirectory = ctx->settingsStore.GetConfigPath().parent_path();
    if (appDataDirectory.empty()) {
        return {};
    }
    return appDataDirectory / "chat-blocker-images";
}

bool EnsureUiGdiplusInitialized()
{
    static const bool initialized = []() -> bool {
        Gdiplus::GdiplusStartupInput startupInput;
        ULONG_PTR token = 0;
        return Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) == Gdiplus::Ok;
    }();
    return initialized;
}

bool TryReadImageDimensions(const std::filesystem::path& imagePath, int& width, int& height)
{
    width = 0;
    height = 0;
    if (!EnsureUiGdiplusInitialized()) {
        return false;
    }
    bool success = false;
    {
        Gdiplus::Bitmap bitmap(imagePath.wstring().c_str());
        if (bitmap.GetLastStatus() == Gdiplus::Ok) {
            width = static_cast<int>(bitmap.GetWidth());
            height = static_cast<int>(bitmap.GetHeight());
            success = width > 0 && height > 0;
        }
    }
    return success;
}

std::wstring GetSelectedComboText(HWND combo)
{
    if (!combo) {
        return {};
    }
    const int selectedIndex = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (selectedIndex < 0) {
        return {};
    }
    const int textLength = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(selectedIndex), 0));
    if (textLength <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(textLength) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(selectedIndex), reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(textLength));
    return text;
}

bool IsSupportedChatBlockerImageExtension(std::wstring extension)
{
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return extension == L".png"
        || extension == L".jpg"
        || extension == L".jpeg"
        || extension == L".bmp"
        || extension == L".gif"
        || extension == L".webp";
}

std::vector<std::wstring> EnumerateChatBlockerImageFileNames(const AppContext* ctx)
{
    std::vector<std::wstring> names;
    const auto imagesDirectory = ResolveChatBlockerImagesDirectory(ctx);
    if (imagesDirectory.empty()) {
        return names;
    }

    std::error_code ec;
    if (!std::filesystem::exists(imagesDirectory, ec) || ec) {
        return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator(imagesDirectory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::wstring extension = entry.path().extension().wstring();
        if (!IsSupportedChatBlockerImageExtension(extension)) {
            continue;
        }
        names.push_back(entry.path().filename().wstring());
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& lhs, const std::wstring& rhs) {
        std::wstring lhsLower = lhs;
        std::wstring rhsLower = rhs;
        std::transform(lhsLower.begin(), lhsLower.end(), lhsLower.begin(), towlower);
        std::transform(rhsLower.begin(), rhsLower.end(), rhsLower.begin(), towlower);
        return lhsLower < rhsLower;
    });
    return names;
}

std::filesystem::path ResolveSelectedChatBlockerImagePath(const AppContext* ctx)
{
    if (!ctx || !ctx->chatBlockerImageCombo) {
        return {};
    }
    const std::wstring fileName = GetSelectedComboText(ctx->chatBlockerImageCombo);
    if (fileName.empty() || _wcsicmp(fileName.c_str(), L"No images imported") == 0) {
        return {};
    }
    const auto imagesDirectory = ResolveChatBlockerImagesDirectory(ctx);
    if (imagesDirectory.empty()) {
        return {};
    }
    return imagesDirectory / fileName;
}

std::string ResolveSelectedChatBlockerImageFileNameKey(const AppContext* ctx)
{
    const auto imagePath = ResolveSelectedChatBlockerImagePath(ctx);
    if (imagePath.empty()) {
        return {};
    }
    return ToUtf8(imagePath.filename().wstring());
}

void RememberChatBlockerSizeForSelectedImage(AppContext* ctx)
{
    if (!ctx || !ctx->chatBlockerWidthEdit || !ctx->chatBlockerHeightEdit) {
        return;
    }
    const bool customSelected = ctx->chatBlockerImageCustomRadio
        && SendMessageW(ctx->chatBlockerImageCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!customSelected) {
        return;
    }

    const std::string imageFileNameKey = ResolveSelectedChatBlockerImageFileNameKey(ctx);
    if (imageFileNameKey.empty()) {
        return;
    }

    const int width = (std::max)(0, ReadIntControl(ctx->chatBlockerWidthEdit, ctx->settings.chatBlockerWidth));
    const int height = (std::max)(0, ReadIntControl(ctx->chatBlockerHeightEdit, ctx->settings.chatBlockerHeight));
    if (width > 0 && height > 0) {
        ctx->settings.chatBlockerCustomImageSizesByFileName[imageFileNameKey] = std::make_pair(width, height);
    }
}

bool SyncChatBlockerSelectionToImageMetadata(AppContext* ctx, bool resetBlockerSizeToImage)
{
    if (!ctx) {
        return false;
    }
    const auto imagePath = ResolveSelectedChatBlockerImagePath(ctx);
    if (imagePath.empty()) {
        ctx->chatBlockerCustomSourceWidth = 0;
        ctx->chatBlockerCustomSourceHeight = 0;
        return false;
    }

    int sourceWidth = 0;
    int sourceHeight = 0;
    if (!TryReadImageDimensions(imagePath, sourceWidth, sourceHeight)) {
        return false;
    }
    ctx->chatBlockerCustomSourceWidth = sourceWidth;
    ctx->chatBlockerCustomSourceHeight = sourceHeight;
    if (resetBlockerSizeToImage && ctx->chatBlockerWidthEdit && ctx->chatBlockerHeightEdit) {
        int widthToApply = sourceWidth;
        int heightToApply = sourceHeight;
        const std::string imageFileNameKey = ToUtf8(imagePath.filename().wstring());
        const auto it = ctx->settings.chatBlockerCustomImageSizesByFileName.find(imageFileNameKey);
        if (it != ctx->settings.chatBlockerCustomImageSizesByFileName.end()
            && it->second.first > 0
            && it->second.second > 0) {
            widthToApply = it->second.first;
            heightToApply = it->second.second;
        }
        SetWindowTextW(ctx->chatBlockerWidthEdit, ToWide(std::to_string(widthToApply)).c_str());
        SetWindowTextW(ctx->chatBlockerHeightEdit, ToWide(std::to_string(heightToApply)).c_str());
    }
    return true;
}

void RefreshChatBlockerImageCombo(AppContext* ctx, const std::wstring& preferredFileName)
{
    if (!ctx || !ctx->chatBlockerImageCombo) {
        return;
    }

    const std::wstring requestedSelection = preferredFileName.empty()
        ? GetSelectedComboText(ctx->chatBlockerImageCombo)
        : preferredFileName;

    const auto imageFileNames = EnumerateChatBlockerImageFileNames(ctx);
    SendMessageW(ctx->chatBlockerImageCombo, CB_RESETCONTENT, 0, 0);

    if (imageFileNames.empty()) {
        SendMessageW(
            ctx->chatBlockerImageCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(L"No images imported"));
        SendMessageW(ctx->chatBlockerImageCombo, CB_SETCURSEL, 0, 0);
        return;
    }

    int selectedIndex = -1;
    for (size_t index = 0; index < imageFileNames.size(); ++index) {
        const int comboIndex = static_cast<int>(SendMessageW(
            ctx->chatBlockerImageCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(imageFileNames[index].c_str())));
        if (comboIndex >= 0 && _wcsicmp(imageFileNames[index].c_str(), requestedSelection.c_str()) == 0) {
            selectedIndex = comboIndex;
        }
    }

    if (selectedIndex < 0 && !imageFileNames.empty()) {
        selectedIndex = 0;
    }
    SendMessageW(
        ctx->chatBlockerImageCombo,
        CB_SETCURSEL,
        selectedIndex >= 0 ? static_cast<WPARAM>(selectedIndex) : static_cast<WPARAM>(-1),
        0);
}

int ScaleByAspectRatio(int value, int numerator, int denominator)
{
    if (value <= 0 || numerator <= 0 || denominator <= 0) {
        return 0;
    }
    const double scaled = static_cast<double>(value) * static_cast<double>(numerator) / static_cast<double>(denominator);
    return (std::max)(1, static_cast<int>(std::lround(scaled)));
}

void RefreshChatBlockerImageControls(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    const bool customSelected = ctx->chatBlockerImageCustomRadio
        && SendMessageW(ctx->chatBlockerImageCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ctx->chatBlockerImageCombo) {
        EnableWindow(ctx->chatBlockerImageCombo, customSelected ? TRUE : FALSE);
    }
    if (ctx->chatBlockerImageImportButton) {
        EnableWindow(ctx->chatBlockerImageImportButton, TRUE);
    }
    if (ctx->chatBlockerImageOpenFolderButton) {
        EnableWindow(ctx->chatBlockerImageOpenFolderButton, TRUE);
    }
}

void ApplyChatBlockerAspectForEdit(AppContext* ctx, int editedControlId)
{
    if (!ctx || ctx->chatBlockerAspectAdjusting) {
        return;
    }
    const bool customSelected = ctx->chatBlockerImageCustomRadio
        && SendMessageW(ctx->chatBlockerImageCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!customSelected || ctx->chatBlockerCustomSourceWidth <= 0 || ctx->chatBlockerCustomSourceHeight <= 0) {
        return;
    }

    ctx->chatBlockerAspectAdjusting = true;
    if (editedControlId == IDC_CHAT_BLOCKER_WIDTH_EDIT && ctx->chatBlockerWidthEdit && ctx->chatBlockerHeightEdit) {
        const int width = (std::max)(0, ReadIntControl(ctx->chatBlockerWidthEdit, ctx->settings.chatBlockerWidth));
        const int height = ScaleByAspectRatio(width, ctx->chatBlockerCustomSourceHeight, ctx->chatBlockerCustomSourceWidth);
        ctx->chatBlockerIgnoreNextHeightChange = true;
        SetWindowTextW(ctx->chatBlockerHeightEdit, ToWide(std::to_string(height)).c_str());
    } else if (editedControlId == IDC_CHAT_BLOCKER_HEIGHT_EDIT && ctx->chatBlockerHeightEdit && ctx->chatBlockerWidthEdit) {
        const int height = (std::max)(0, ReadIntControl(ctx->chatBlockerHeightEdit, ctx->settings.chatBlockerHeight));
        const int width = ScaleByAspectRatio(height, ctx->chatBlockerCustomSourceWidth, ctx->chatBlockerCustomSourceHeight);
        ctx->chatBlockerIgnoreNextWidthChange = true;
        SetWindowTextW(ctx->chatBlockerWidthEdit, ToWide(std::to_string(width)).c_str());
    }
    ctx->chatBlockerAspectAdjusting = false;
}

bool DrawChatBlockerImageOverlay(HDC targetDc, const RECT& targetRect, const std::filesystem::path& imagePath)
{
    if (!targetDc || imagePath.empty() || targetRect.right <= targetRect.left || targetRect.bottom <= targetRect.top) {
        return false;
    }
    if (!EnsureUiGdiplusInitialized()) {
        return false;
    }

    static std::filesystem::path cachedImagePath;
    static std::unique_ptr<Gdiplus::Bitmap> cachedImage;
    if (!cachedImage || cachedImagePath != imagePath) {
        auto loadedImage = std::make_unique<Gdiplus::Bitmap>(imagePath.wstring().c_str());
        if (!loadedImage || loadedImage->GetLastStatus() != Gdiplus::Ok) {
            cachedImage.reset();
            cachedImagePath.clear();
            return false;
        }
        cachedImage = std::move(loadedImage);
        cachedImagePath = imagePath;
    }

    bool drewImage = false;
    {
        Gdiplus::Graphics graphics(targetDc);
        if (cachedImage) {
            const Gdiplus::Rect drawRect(
                targetRect.left,
                targetRect.top,
                targetRect.right - targetRect.left,
                targetRect.bottom - targetRect.top);
            drewImage = (graphics.DrawImage(cachedImage.get(), drawRect) == Gdiplus::Ok);
        }
    }
    return drewImage;
}

bool SaveCustomChatBlockerImage(AppContext* ctx, const std::filesystem::path& sourcePath, std::wstring& error)
{
    error.clear();
    if (!ctx || sourcePath.empty()) {
        error = L"No image selected.";
        return false;
    }
    try {
        int sourceWidth = 0;
        int sourceHeight = 0;
        if (!TryReadImageDimensions(sourcePath, sourceWidth, sourceHeight)) {
            error = L"Unable to read image dimensions from selected file.";
            return false;
        }

        const auto imagesDirectory = ResolveChatBlockerImagesDirectory(ctx);
        if (imagesDirectory.empty()) {
            error = L"Could not resolve chat blocker images folder.";
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(imagesDirectory, ec);
        if (ec) {
            error = std::wstring(L"Failed to create chat blocker images folder: ") + ToWide(ec.message());
            return false;
        }

        const std::filesystem::path targetPath = imagesDirectory / sourcePath.filename();
        const bool targetAlreadyExists = std::filesystem::exists(targetPath, ec) && !ec;
        if (targetAlreadyExists) {
            const int overwriteChoice = MessageBoxW(
                ctx->mainWindow,
                (std::wstring(L"An image named '") + targetPath.filename().wstring()
                    + L"' already exists.\n\nOverwrite existing file?").c_str(),
                L"Image Already Imported",
                MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
            if (overwriteChoice != IDYES) {
                error = L"Import canceled.";
                return false;
            }
        }
        std::filesystem::copy_file(sourcePath, targetPath, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = std::wstring(L"Failed to copy image into chat blocker folder: ") + ToWide(ec.message());
            return false;
        }

        RefreshChatBlockerImageCombo(ctx, targetPath.filename().wstring());
        ctx->chatBlockerCustomSourceWidth = sourceWidth;
        ctx->chatBlockerCustomSourceHeight = sourceHeight;
        SetWindowTextW(ctx->chatBlockerWidthEdit, ToWide(std::to_string(sourceWidth)).c_str());
        SetWindowTextW(ctx->chatBlockerHeightEdit, ToWide(std::to_string(sourceHeight)).c_str());
        return true;
    } catch (const std::exception& ex) {
        error = std::wstring(L"Unexpected error importing image: ") + ToWide(ex.what());
        return false;
    } catch (...) {
        error = L"Unexpected unknown error importing image.";
        return false;
    }
}

std::wstring GetKnownFolderPath(REFKNOWNFOLDERID folderId)
{
    PWSTR rawPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(folderId, 0, nullptr, &rawPath)) || rawPath == nullptr) {
        if (rawPath) {
            CoTaskMemFree(rawPath);
        }
        return {};
    }

    std::wstring path(rawPath);
    CoTaskMemFree(rawPath);
    return path;
}

std::vector<MicrophoneOption> EnumerateMicrophoneOptions()
{
    std::vector<MicrophoneOption> options;
    options.push_back({L"Default microphone", "default"});

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return options;
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        return options;
    }

    UINT count = 0;
    if (SUCCEEDED(collection->GetCount(&count))) {
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device) {
                continue;
            }

            LPWSTR deviceId = nullptr;
            if (FAILED(device->GetId(&deviceId)) || !deviceId) {
                device->Release();
                continue;
            }

            std::wstring friendlyName = L"Microphone";
            IPropertyStore* propertyStore = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &propertyStore)) && propertyStore) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(propertyStore->GetValue(PKEY_Device_FriendlyName, &value))
                    && value.vt == VT_LPWSTR
                    && value.pwszVal
                    && value.pwszVal[0] != L'\0') {
                    friendlyName = value.pwszVal;
                }
                PropVariantClear(&value);
                propertyStore->Release();
            }

            options.push_back({friendlyName, ToUtf8(deviceId)});
            CoTaskMemFree(deviceId);
            device->Release();
        }
    }

    collection->Release();
    enumerator->Release();
    return options;
}

void RefreshMicrophoneOptionsUi(AppContext* ctx);
void RefreshMicrophoneDeviceOptionsUi(AppContext* ctx);

std::string Trim(std::string value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string ReadQuotedJson(const std::string& content, const std::string& key)
{
    const std::string marker = "\"" + key + "\"";
    const auto keyPos = content.find(marker);
    if (keyPos == std::string::npos) {
        return {};
    }
    const auto colonPos = content.find(':', keyPos + marker.size());
    if (colonPos == std::string::npos) {
        return {};
    }
    const auto firstQuote = content.find('"', colonPos + 1);
    if (firstQuote == std::string::npos) {
        return {};
    }
    bool escape = false;
    for (size_t i = firstQuote + 1; i < content.size(); ++i) {
        const char ch = content[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            return content.substr(firstQuote + 1, i - firstQuote - 1);
        }
    }
    return {};
}

std::string GetEnvString(const char* name)
{
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr || len == 0) {
        if (value) {
            free(value);
        }
        return {};
    }
    std::string out(value);
    free(value);
    return out;
}

std::string GetYouTubeAuthServerUrl()
{
    return kYouTubeAuthServerUrl;
}

LRESULT CALLBACK PanelMessageForwarder(HWND panel, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_ERASEBKGND || message == WM_PAINT) {
        RECT rect{};
        GetClientRect(panel, &rect);
        if (message == WM_ERASEBKGND) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            TRIVERTEX vertices[2] = {
                {rect.left, rect.top, static_cast<COLOR16>(GetRValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetGValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetBValue(kColorPanelTop) << 8), 0xFF00},
                {rect.right, rect.bottom, static_cast<COLOR16>(GetRValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetGValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetBValue(kColorPanelBottom) << 8), 0xFF00},
            };
            GRADIENT_RECT gradientRect{0, 1};
            if (!GradientFill(dc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V)) {
                FillRect(dc, &rect, gTheme.panelSolidBrush ? gTheme.panelSolidBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            if (gTheme.panelBorderBrush) {
                FrameRect(dc, &rect, gTheme.panelBorderBrush);
            }
            return 1;
        }

        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(panel, &paint);
        if (dc) {
            TRIVERTEX vertices[2] = {
                {rect.left, rect.top, static_cast<COLOR16>(GetRValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetGValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetBValue(kColorPanelTop) << 8), 0xFF00},
                {rect.right, rect.bottom, static_cast<COLOR16>(GetRValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetGValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetBValue(kColorPanelBottom) << 8), 0xFF00},
            };
            GRADIENT_RECT gradientRect{0, 1};
            if (!GradientFill(dc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V)) {
                FillRect(dc, &rect, gTheme.panelSolidBrush ? gTheme.panelSolidBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            if (gTheme.panelBorderBrush) {
                FrameRect(dc, &rect, gTheme.panelBorderBrush);
            }
            EndPaint(panel, &paint);
            return 0;
        }
    }
    if (message == WM_COMMAND
        || message == WM_NOTIFY
        || message == WM_HSCROLL
        || message == WM_CTLCOLORSTATIC
        || message == WM_CTLCOLOREDIT
        || message == WM_CTLCOLORBTN
        || message == WM_CTLCOLORLISTBOX
        || message == WM_DRAWITEM
        || message == WM_MEASUREITEM) {
        HWND parent = GetParent(panel);
        if (parent) {
            return SendMessageW(parent, message, wParam, lParam);
        }
    }
    return DefSubclassProc(panel, message, wParam, lParam);
}

LRESULT CALLBACK EditSelectAllSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_KEYDOWN && wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    if (message == WM_CHAR && wParam == 1) {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void EnableCtrlASelectAll(HWND editControl)
{
    if (!editControl) {
        return;
    }
    SetWindowSubclass(editControl, EditSelectAllSubclassProc, 1, 0);
}

std::wstring ChatBlockerAnchorLabel(bean::core::AppSettings::ChatBlockerAnchor anchor)
{
    switch (anchor) {
    case bean::core::AppSettings::ChatBlockerAnchor::BottomRight:
        return L"bottom-right";
    case bean::core::AppSettings::ChatBlockerAnchor::TopLeft:
        return L"top-left";
    case bean::core::AppSettings::ChatBlockerAnchor::TopRight:
        return L"top-right";
    case bean::core::AppSettings::ChatBlockerAnchor::BottomLeft:
    default:
        return L"bottom-left";
    }
}

void RefreshMicrophoneOptionsUi(AppContext* ctx)
{
    if (!ctx || !ctx->microphoneCombo || !ctx->microphoneCheck) {
        return;
    }

    const bool micEnabled = (SendMessageW(ctx->microphoneCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const bool comboWasEnabled = IsWindowEnabled(ctx->microphoneCombo) != FALSE;
    EnableWindow(ctx->microphoneCombo, micEnabled ? TRUE : FALSE);
    if (comboWasEnabled != micEnabled) {
        RedrawWindow(ctx->microphoneCombo, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
    if (ctx->microphoneNoiseSuppressionCheck) {
        const bool noiseWasEnabled = IsWindowEnabled(ctx->microphoneNoiseSuppressionCheck) != FALSE;
        EnableWindow(ctx->microphoneNoiseSuppressionCheck, micEnabled ? TRUE : FALSE);
        if (noiseWasEnabled != micEnabled) {
            RedrawWindow(ctx->microphoneNoiseSuppressionCheck, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
            if (ctx->recorderPanel) {
                RedrawWindow(ctx->recorderPanel, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
            }
        }
    }
}

void RefreshMicrophoneDeviceOptionsUi(AppContext* ctx)
{
    if (!ctx || !ctx->microphoneCombo) {
        return;
    }

    ctx->microphoneOptions = EnumerateMicrophoneOptions();
    SendMessageW(ctx->microphoneCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& option : ctx->microphoneOptions) {
        SendMessageW(ctx->microphoneCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.displayName.c_str()));
    }

    int selectedIndex = 0;
    for (size_t i = 0; i < ctx->microphoneOptions.size(); ++i) {
        if (ctx->microphoneOptions[i].deviceId == ctx->settings.microphoneDeviceId) {
            selectedIndex = static_cast<int>(i);
            break;
        }
    }
    SendMessageW(ctx->microphoneCombo, CB_SETCURSEL, static_cast<WPARAM>(selectedIndex), 0);
    RefreshMicrophoneOptionsUi(ctx);
}

BOOL CALLBACK FindWowWindowForUiProc(HWND hwnd, LPARAM lParam)
{
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) {
        return TRUE;
    }

    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!processHandle) {
        return TRUE;
    }

    wchar_t processPath[MAX_PATH] = {};
    DWORD processPathSize = static_cast<DWORD>(std::size(processPath));
    bool wowProcess = false;
    if (QueryFullProcessImageNameW(processHandle, 0, processPath, &processPathSize)) {
        std::wstring exeName = std::filesystem::path(processPath).filename().wstring();
        std::transform(exeName.begin(), exeName.end(), exeName.begin(), towlower);
        wowProcess = (exeName.size() >= 7
            && exeName.rfind(L"wow", 0) == 0
            && exeName.substr(exeName.size() - 4) == L".exe");
    }
    CloseHandle(processHandle);
    if (!wowProcess) {
        return TRUE;
    }

    wchar_t title[256] = {};
    const int len = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    if (len <= 0) {
        return TRUE;
    }

    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const bool titleLooksLikeWow = (wcsstr(title, L"World of Warcraft") != nullptr);
    const bool classLooksLikeWow = (_wcsicmp(className, L"GxWindowClass") == 0);
    if (titleLooksLikeWow || classLooksLikeWow) {
        auto* found = reinterpret_cast<HWND*>(lParam);
        *found = hwnd;
        return FALSE;
    }

    return TRUE;
}

HWND FindWowWindowForUi()
{
    HWND found = nullptr;
    EnumWindows(FindWowWindowForUiProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool DetectWowWindowForUi()
{
    return FindWowWindowForUi() != nullptr;
}

std::wstring NormalizeProcessOrWindowToken(std::wstring value)
{
    std::wstring normalized;
    normalized.reserve(value.size());
    for (wchar_t ch : value) {
        if (std::iswalnum(ch)) {
            normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }
    }
    return normalized;
}

bool StartsWith(const std::wstring& value, const std::wstring& prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool IsWarcraftRecorderProcessToken(const std::wstring& value)
{
    if (value.empty()) {
        return false;
    }
    const std::wstring normalized = NormalizeProcessOrWindowToken(value);
    return StartsWith(normalized, L"warcraftrecorder")
        || StartsWith(normalized, L"warccraftrecorder");
}

bool IsWarcraftRecorderWindowToken(const std::wstring& value)
{
    if (value.empty()) {
        return false;
    }
    const std::wstring normalized = NormalizeProcessOrWindowToken(value);
    return StartsWith(normalized, L"warcraftrecorder")
        || StartsWith(normalized, L"warccraftrecorder");
}

bool IsWarcraftRecorderProcessId(DWORD processId)
{
    if (processId == 0) {
        return false;
    }
    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!processHandle) {
        return false;
    }
    wchar_t processPath[MAX_PATH] = {};
    DWORD processPathSize = static_cast<DWORD>(std::size(processPath));
    bool isWarcraftRecorder = false;
    if (QueryFullProcessImageNameW(processHandle, 0, processPath, &processPathSize)) {
        const std::wstring exeName = std::filesystem::path(processPath).filename().wstring();
        isWarcraftRecorder = IsWarcraftRecorderProcessToken(exeName);
    }
    CloseHandle(processHandle);
    return isWarcraftRecorder;
}

bool DetectWarcraftRecorderByProcessSnapshot()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W processEntry{};
    processEntry.dwSize = sizeof(processEntry);
    if (!Process32FirstW(snapshot, &processEntry)) {
        CloseHandle(snapshot);
        return false;
    }

    do {
        if (IsWarcraftRecorderProcessToken(processEntry.szExeFile)) {
            CloseHandle(snapshot);
            return true;
        }
    } while (Process32NextW(snapshot, &processEntry));

    CloseHandle(snapshot);
    return false;
}

BOOL CALLBACK DetectWarcraftRecorderWindowProc(HWND hwnd, LPARAM lParam)
{
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return TRUE;
    }

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), length + 1);
    title.resize(static_cast<size_t>(length));
    if (!IsWarcraftRecorderWindowToken(title)) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (!IsWarcraftRecorderProcessId(processId)) {
        return TRUE;
    }

    auto* found = reinterpret_cast<bool*>(lParam);
    *found = true;
    return FALSE;
}

bool DetectWarcraftRecorderByWindowTitle()
{
    bool found = false;
    EnumWindows(DetectWarcraftRecorderWindowProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool DetectWarcraftRecorderForUi()
{
    return DetectWarcraftRecorderByProcessSnapshot() || DetectWarcraftRecorderByWindowTitle();
}

std::vector<std::filesystem::path> EnumerateDriveRootsStartingAtC()
{
    std::vector<std::filesystem::path> roots;
    const DWORD logicalDrives = GetLogicalDrives();
    if (logicalDrives == 0) {
        roots.emplace_back(R"(C:\)");
        return roots;
    }

    const auto addDriveIfPresent = [&](wchar_t driveLetter) {
        const DWORD bit = 1u << (driveLetter - L'A');
        if ((logicalDrives & bit) != 0) {
            std::wstring root;
            root.push_back(driveLetter);
            root += L":\\";
            roots.emplace_back(std::move(root));
        }
    };

    for (wchar_t driveLetter = L'C'; driveLetter <= L'Z'; ++driveLetter) {
        addDriveIfPresent(driveLetter);
    }
    for (wchar_t driveLetter = L'A'; driveLetter < L'C'; ++driveLetter) {
        addDriveIfPresent(driveLetter);
    }

    if (roots.empty()) {
        roots.emplace_back(R"(C:\)");
    }
    return roots;
}

bool ResolveObsInstallRootForUi(std::filesystem::path& root)
{
    const auto envRootText = Trim(GetEnvString("BEAN_OBS_ROOT"));
    if (!envRootText.empty()) {
        const std::filesystem::path envRoot = ToWide(envRootText);
        if (std::filesystem::exists(envRoot / "bin" / "64bit" / "obs.dll")) {
            root = envRoot;
            return true;
        }
    }

    for (const auto& driveRoot : EnumerateDriveRootsStartingAtC()) {
        const std::filesystem::path candidates[] = {
            driveRoot / "Program Files" / "obs-studio",
            driveRoot / "Program Files (x86)" / "obs-studio"
        };
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate / "bin" / "64bit" / "obs.dll")) {
                root = candidate;
                return true;
            }
        }
    }
    return false;
}

bool DetectUsableObsInstallForUi()
{
    std::filesystem::path root;
    if (!ResolveObsInstallRootForUi(root)) {
        return false;
    }

    const auto bin64 = root / "bin" / "64bit";
    if (!std::filesystem::exists(bin64 / "obs.dll")) {
        return false;
    }
    if (!std::filesystem::exists(bin64 / "obs-ffmpeg-mux.exe")) {
        return false;
    }
    if (!std::filesystem::exists(root / "obs-plugins" / "64bit")) {
        return false;
    }
    if (!std::filesystem::exists(root / "data" / "libobs")) {
        return false;
    }
    const bool hasGraphicsBackend = std::filesystem::exists(bin64 / "libobs-d3d11.dll")
        || std::filesystem::exists(bin64 / "libobs-opengl.dll");
    return hasGraphicsBackend;
}

bool DetectFfmpegForUi(AppContext* ctx)
{
    return ResolveFfmpegExecutablePath(ctx).has_value();
}

std::filesystem::path ResolveDefaultWowLogDirectory()
{
    for (const auto& driveRoot : EnumerateDriveRootsStartingAtC()) {
        const std::filesystem::path candidates[] = {
            driveRoot / "Program Files (x86)" / "World of Warcraft" / "_retail_" / "Logs",
            driveRoot / "Program Files" / "World of Warcraft" / "_retail_" / "Logs"
        };
        for (const auto& candidate : candidates) {
            if (DirectoryExists(candidate.wstring())) {
                return candidate;
            }
        }
    }

    // Preserve historical fallback when no install is auto-detected.
    return R"(C:\Program Files (x86)\World of Warcraft\_retail_\Logs)";
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::optional<std::filesystem::path> ResolveWowInstallDirectoryFromLogDirectory(const std::filesystem::path& logDirectory)
{
    if (logDirectory.empty()) {
        return std::nullopt;
    }
    std::filesystem::path normalized = logDirectory.lexically_normal();
    if (normalized.filename() == L"Logs") {
        normalized = normalized.parent_path();
    }
    if (normalized.filename() == L"_retail_") {
        const auto installDirectory = normalized.parent_path();
        if (!installDirectory.empty()) {
            return installDirectory;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolveWowInstallDirectoryForUi(const AppContext* ctx)
{
    if (ctx) {
        if (const auto fromSettings = ResolveWowInstallDirectoryFromLogDirectory(ctx->settings.wowLogDirectory)) {
            return fromSettings;
        }
    }

    if (const auto fromAutoDetection = ResolveWowInstallDirectoryFromLogDirectory(ResolveDefaultWowLogDirectory())) {
        return fromAutoDetection;
    }
    return std::nullopt;
}

bool DetectAdvancedCombatLoggingForUi(const AppContext* ctx)
{
    const auto installDirectory = ResolveWowInstallDirectoryForUi(ctx);
    if (!installDirectory.has_value()) {
        return false;
    }
    const auto configPath = *installDirectory / "_retail_" / "WTF" / "Config.wtf";

    std::ifstream stream(configPath);
    if (!stream.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty()) {
            continue;
        }

        std::istringstream parser(trimmed);
        std::string setToken;
        std::string keyToken;
        std::string valueToken;
        if (!(parser >> setToken >> keyToken >> valueToken)) {
            continue;
        }
        if (ToLowerAscii(setToken) != "set" || ToLowerAscii(keyToken) != "advancedcombatlogging") {
            continue;
        }
        return valueToken == "\"1\"";
    }

    return false;
}

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
        if (HWND wowWindow = FindWowWindowForUi()) {
            RECT wowRect{};
            if (GetClientRect(wowWindow, &wowRect)) {
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
            if (shouldCapture && !IsIconic(wowWindow)
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
                            const BOOL printed = PrintWindow(wowWindow, scratchDc, PW_CLIENTONLY | kPrintWindowRenderFullContent);
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
        HBRUSH pausedBrush = CreateSolidBrush(RGB(24, 30, 44));
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
        HBRUSH fallbackBrush = CreateSolidBrush(RGB(24, 30, 44));
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

struct YouTubeAuthCompletionPayload {
    bool success = false;
    std::string clientId;
    std::string refreshToken;
    std::string channelId;
    std::string channelTitle;
    std::string error;
};

struct YouTubeUploadProgressPayload {
    int percent = 0;
    std::wstring text;
};

struct YouTubeIdentityResolvedPayload {
    bool success = false;
    std::string channelId;
    std::string channelTitle;
    std::string error;
};

struct UpdateAvailabilityPayload {
    std::uint64_t requestId = 0;
    bean::app::UpdateAvailability availability = bean::app::UpdateAvailability::Failed;
    std::wstring statusMessage;
};

void PostYouTubeUploadProgress(AppContext* ctx, int percent, const std::wstring& text)
{
    if (!ctx || !ctx->mainWindow) {
        return;
    }
    auto* payload = new YouTubeUploadProgressPayload();
    payload->percent = std::clamp(percent, 0, 100);
    payload->text = text;
    PostMessageW(ctx->mainWindow, WM_BEAN_YOUTUBE_UPLOAD_PROGRESS, 0, reinterpret_cast<LPARAM>(payload));
}

void RequestYouTubeUiRefresh(AppContext* ctx)
{
    if (!ctx || !ctx->mainWindow) {
        return;
    }
    PostMessageW(ctx->mainWindow, WM_BEAN_YOUTUBE_UI_REFRESH, 0, 0);
}

void ResolveLinkedYouTubeIdentityAsync(AppContext* ctx, bool postErrorToStatus)
{
    if (!ctx || !ctx->mainWindow || ctx->settings.youtubeRefreshToken.empty() || ctx->settings.youtubeClientId.empty()) {
        return;
    }
    bean::integrations::YouTubeCredentials creds;
    creds.clientId = ctx->settings.youtubeClientId;
    creds.refreshToken = ctx->settings.youtubeRefreshToken;
    std::thread([ctx, creds, postErrorToStatus]() {
        const auto identity = bean::integrations::YouTubeUploader::GetLinkedChannelIdentity(creds);
        auto* payload = new YouTubeIdentityResolvedPayload();
        payload->success = identity.success;
        payload->channelId = identity.channelId;
        payload->channelTitle = identity.channelTitle;
        payload->error = postErrorToStatus ? identity.error : std::string{};
        PostMessageW(ctx->mainWindow, WM_BEAN_YOUTUBE_IDENTITY_RESOLVED, 0, reinterpret_cast<LPARAM>(payload));
    }).detach();
}

void SetRecordingsUploadUi(AppContext* ctx, int percent, const std::wstring& text)
{
    if (!ctx) {
        return;
    }
    if (ctx->recordingsUploadProgress) {
        SendMessageW(ctx->recordingsUploadProgress, PBM_SETPOS, static_cast<WPARAM>(std::clamp(percent, 0, 100)), 0);
    }
    if (ctx->recordingsUploadStatus) {
        UpdateTransparentStaticText(ctx->recordingsUploadStatus, text.c_str());
    }
}

int GetSelectedRecordingIndex(AppContext* ctx)
{
    if (!ctx || !ctx->recordingsList) {
        return -1;
    }
    return ListView_GetNextItem(ctx->recordingsList, -1, LVNI_SELECTED);
}

std::wstring DefaultYouTubeTitle(const std::filesystem::path& path)
{
    auto title = path.stem().wstring();
    if (title.empty()) {
        title = path.filename().wstring();
    }
    std::replace(title.begin(), title.end(), L'_', L' ');
    return title;
}

void RefreshYouTubeUiState(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    const bool oauthConfigured = !GetYouTubeAuthServerUrl().empty();
    const bool linked = !ctx->settings.youtubeRefreshToken.empty();
    ctx->youtubeOAuthConfigured = oauthConfigured;
    ctx->youtubeLinked = linked;
    if (!linked) {
        ctx->youtubeUnlinkConfirmPending = false;
    }
    const int selectedIndex = GetSelectedRecordingIndex(ctx);
    const bool canUpload = oauthConfigured && linked && !ctx->youtubeBusy.load() && selectedIndex >= 0 && static_cast<size_t>(selectedIndex) < ctx->recordingItems.size();

    if (ctx->youtubeLinkStatus) {
        if (!oauthConfigured) {
            SetWindowTextW(ctx->youtubeLinkStatus, L"OAuth not configured");
        } else {
            SetWindowTextW(ctx->youtubeLinkStatus, linked ? L"Linked" : L"Not linked");
        }
        ShowWindow(ctx->youtubeLinkStatus, SW_HIDE);
        InvalidateRect(ctx->youtubeLinkStatus, nullptr, TRUE);
    }
    if (ctx->youtubeLinkButton) {
        ShowWindow(ctx->youtubeLinkButton, (!linked && oauthConfigured) ? SW_SHOW : SW_HIDE);
        EnableWindow(ctx->youtubeLinkButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    const bool showUnlinkConfirm = linked && ctx->youtubeUnlinkConfirmPending;
    if (ctx->youtubeUnlinkButton) {
        SetWindowTextW(ctx->youtubeUnlinkButton, L"Unlink Account");
        ShowWindow(ctx->youtubeUnlinkButton, (linked && !showUnlinkConfirm) ? SW_SHOW : SW_HIDE);
        EnableWindow(ctx->youtubeUnlinkButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    if (ctx->youtubeUnlinkConfirmLabel) {
        ShowWindow(ctx->youtubeUnlinkConfirmLabel, showUnlinkConfirm ? SW_SHOW : SW_HIDE);
    }
    if (ctx->youtubeUnlinkYesButton) {
        ShowWindow(ctx->youtubeUnlinkYesButton, showUnlinkConfirm ? SW_SHOW : SW_HIDE);
        EnableWindow(ctx->youtubeUnlinkYesButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    if (ctx->youtubeUnlinkNoButton) {
        ShowWindow(ctx->youtubeUnlinkNoButton, showUnlinkConfirm ? SW_SHOW : SW_HIDE);
        EnableWindow(ctx->youtubeUnlinkNoButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    if (ctx->youtubeAccountLabel) {
        if (!linked) {
            SetWindowTextW(ctx->youtubeAccountLabel, L"YouTube Account:");
            if (ctx->youtubeAccountLink) {
                SetWindowTextW(ctx->youtubeAccountLink, L"Not linked");
                EnableWindow(ctx->youtubeAccountLink, FALSE);
                ShowWindow(ctx->youtubeAccountLink, SW_SHOW);
            }
        } else if (!ctx->settings.youtubeChannelId.empty()) {
            SetWindowTextW(ctx->youtubeAccountLabel, L"YouTube Account:");
            if (ctx->youtubeAccountLink) {
                std::wstring text = ToWide(ctx->settings.youtubeChannelTitle.empty() ? ctx->settings.youtubeChannelId : ctx->settings.youtubeChannelTitle);
                SetWindowTextW(ctx->youtubeAccountLink, text.c_str());
                EnableWindow(ctx->youtubeAccountLink, TRUE);
                ShowWindow(ctx->youtubeAccountLink, SW_SHOW);
            }
        } else {
            SetWindowTextW(ctx->youtubeAccountLabel, L"YouTube Account:");
            if (ctx->youtubeAccountLink) {
                SetWindowTextW(ctx->youtubeAccountLink, ctx->youtubeBusy.load() ? L"Resolving..." : L"Linked");
                EnableWindow(ctx->youtubeAccountLink, FALSE);
                ShowWindow(ctx->youtubeAccountLink, SW_SHOW);
            }
        }
    }
    if (ctx->youtubeUploadButton) {
        EnableWindow(ctx->youtubeUploadButton, canUpload ? TRUE : FALSE);
    }
}

void UnlinkYouTubeAccount(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    ctx->settings.youtubeRefreshToken.clear();
    ctx->settings.youtubeChannelId.clear();
    ctx->settings.youtubeChannelTitle.clear();
    std::string saveError;
    if (!ctx->settingsStore.Save(ctx->settings, saveError)) {
        SetStatus(ctx, std::wstring(L"Failed to unlink YouTube account: ") + ToWide(saveError));
    } else {
        SetStatus(ctx, L"YouTube account unlinked.");
        SetRecordingsUploadUi(ctx, 0, L"No upload in progress.");
    }
}

void BackfillRecordingParticipantsFromKnownGuids(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    struct GuidProfile {
        std::wstring bestName;
        std::wstring bestSpecAbbrev;
        std::optional<std::string> bestSpecName;
        std::optional<std::string> bestClassName;
        COLORREF bestColor = kColorTextMuted;
    };

    std::unordered_map<std::string, GuidProfile> profiles;
    for (const auto& recording : ctx->recordingItems) {
        for (const auto& participant : recording.participants) {
            if (participant.guid.empty()) {
                continue;
            }
            auto& profile = profiles[participant.guid];
            if (profile.bestName.empty() && !participant.name.empty() && !IsLikelyInvalidParticipantName(participant.name)) {
                profile.bestName = participant.name;
            }
            if (profile.bestSpecAbbrev.empty() && !participant.specAbbrev.empty()) {
                profile.bestSpecAbbrev = participant.specAbbrev;
            }
            if (!profile.bestSpecName.has_value() && participant.specName.has_value() && !participant.specName->empty()) {
                profile.bestSpecName = participant.specName;
            }
            if (!profile.bestClassName.has_value() && participant.className.has_value() && !participant.className->empty()) {
                profile.bestClassName = participant.className;
            }
            if (profile.bestColor == kColorTextMuted && participant.classColor != kColorTextMuted) {
                profile.bestColor = participant.classColor;
            }
        }
    }

    for (auto& recording : ctx->recordingItems) {
        for (auto& participant : recording.participants) {
            if (participant.guid.empty()) {
                continue;
            }
            const auto profileIt = profiles.find(participant.guid);
            if (profileIt == profiles.end()) {
                continue;
            }
            const auto& profile = profileIt->second;
            if ((participant.name.empty() || IsLikelyInvalidParticipantName(participant.name)) && !profile.bestName.empty()) {
                participant.name = profile.bestName;
            }
            if (participant.specAbbrev.empty() && !profile.bestSpecAbbrev.empty()) {
                participant.specAbbrev = profile.bestSpecAbbrev;
            }
            if ((!participant.specName.has_value() || participant.specName->empty()) && profile.bestSpecName.has_value()) {
                participant.specName = profile.bestSpecName;
            }
            if ((!participant.className.has_value() || participant.className->empty()) && profile.bestClassName.has_value()) {
                participant.className = profile.bestClassName;
            }
            if (participant.classColor == kColorTextMuted && profile.bestColor != kColorTextMuted) {
                participant.classColor = profile.bestColor;
            }
        }
    }
}

void UpdateRecordingParticipantsPane(AppContext* ctx, int selectedIndex)
{
    if (!ctx || !ctx->recordingsInfoText) {
        return;
    }

    ListView_DeleteAllItems(ctx->recordingsInfoText);
    ctx->visibleParticipantRowColors.clear();

    if (selectedIndex < 0 || static_cast<size_t>(selectedIndex) >= ctx->recordingItems.size()) {
        return;
    }

    const auto& recording = ctx->recordingItems[static_cast<size_t>(selectedIndex)];
    int rowIndex = 0;
    for (const auto& participant : recording.participants) {
        std::wstring displayText = participant.name;
        if (displayText.empty() || IsLikelyInvalidParticipantName(displayText)) {
            displayText = L"(unknown)";
        }
        const int iconIndex = ResolveParticipantSpecIconIndex(ctx, participant.className, participant.specName);
        if (iconIndex == I_IMAGENONE && !participant.specAbbrev.empty()) {
            displayText += std::wstring(L" [") + participant.specAbbrev + L"]";
        }
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = rowIndex;
        item.iSubItem = 0;
        item.iImage = iconIndex;
        item.pszText = const_cast<wchar_t*>(displayText.c_str());
        SendMessageW(ctx->recordingsInfoText, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        ctx->visibleParticipantRowColors.push_back(participant.classColor);
        ++rowIndex;
    }
}

void UpdateRecordingInfoPane(AppContext* ctx, int selectedIndex)
{
    if (!ctx) {
        return;
    }
    UpdateRecordingParticipantsPane(ctx, selectedIndex);
    if (selectedIndex < 0 || static_cast<size_t>(selectedIndex) >= ctx->recordingItems.size()) {
        RefreshYouTubeUiState(ctx);
        return;
    }

    const auto& item = ctx->recordingItems[static_cast<size_t>(selectedIndex)];
    if (ctx->youtubeTitleEdit) {
        SetWindowTextW(ctx->youtubeTitleEdit, DefaultYouTubeTitle(item.path).c_str());
    }
    RefreshYouTubeUiState(ctx);
}

void SortRecordingItems(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    const auto column = ctx->recordingSortColumn;
    const bool asc = ctx->recordingSortAscending;
    std::sort(ctx->recordingItems.begin(), ctx->recordingItems.end(), [column, asc](const AppContext::RecordingItem& a, const AppContext::RecordingItem& b) {
        int cmp = 0;
        switch (column) {
        case AppContext::RecordingSortColumn::Dungeon:
            cmp = _wcsicmp(a.dungeonName.c_str(), b.dungeonName.c_str());
            break;
        case AppContext::RecordingSortColumn::Keystone:
            if (a.keystoneLevel < b.keystoneLevel) {
                cmp = -1;
            } else if (a.keystoneLevel > b.keystoneLevel) {
                cmp = 1;
            } else {
                cmp = 0;
            }
            break;
        case AppContext::RecordingSortColumn::Duration:
            if (a.duration < b.duration) {
                cmp = -1;
            } else if (a.duration > b.duration) {
                cmp = 1;
            } else {
                cmp = 0;
            }
            break;
        case AppContext::RecordingSortColumn::Date:
            if (a.modified < b.modified) {
                cmp = -1;
            } else if (a.modified > b.modified) {
                cmp = 1;
            } else {
                cmp = 0;
            }
            break;
        }
        return asc ? (cmp < 0) : (cmp > 0);
    });
}

void RepopulateRecordingsListControl(AppContext* ctx)
{
    if (!ctx || !ctx->recordingsList) {
        return;
    }

    ListView_DeleteAllItems(ctx->recordingsList);
    int itemIndex = 0;
    for (const auto& recording : ctx->recordingItems) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = itemIndex;
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(recording.dungeonName.c_str());
        SendMessageW(ctx->recordingsList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));

        LVITEMW subItem{};
        subItem.mask = LVIF_TEXT;
        subItem.iItem = itemIndex;

        subItem.iSubItem = 1;
        subItem.pszText = const_cast<wchar_t*>(recording.keystoneText.c_str());
        SendMessageW(ctx->recordingsList, LVM_SETITEMTEXTW, static_cast<WPARAM>(itemIndex), reinterpret_cast<LPARAM>(&subItem));

        subItem.iSubItem = 2;
        subItem.pszText = const_cast<wchar_t*>(recording.durationText.c_str());
        SendMessageW(ctx->recordingsList, LVM_SETITEMTEXTW, static_cast<WPARAM>(itemIndex), reinterpret_cast<LPARAM>(&subItem));

        subItem.iSubItem = 3;
        subItem.pszText = const_cast<wchar_t*>(recording.dateText.c_str());
        SendMessageW(ctx->recordingsList, LVM_SETITEMTEXTW, static_cast<WPARAM>(itemIndex), reinterpret_cast<LPARAM>(&subItem));
        ++itemIndex;
    }

    UpdateRecordingInfoPane(ctx, ListView_GetNextItem(ctx->recordingsList, -1, LVNI_SELECTED));
}

void RefreshRecordingsList(AppContext* ctx)
{
    if (!ctx || !ctx->recordingsList || !ctx->recordingsLabel) {
        return;
    }

    std::wstring folder = GetWindowTextString(ctx->outputEdit);
    if (folder.empty()) {
        folder = ToWide(ctx->settings.outputDirectory.string());
    }

    ListView_DeleteAllItems(ctx->recordingsList);
    ctx->recordingItems.clear();
    UpdateRecordingInfoPane(ctx, -1);

    if (!DirectoryExists(folder)) {
        SetWindowTextW(ctx->recordingsLabel, L"Recordings folder is unavailable.");
        return;
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }

        const auto extension = entry.path().extension().wstring();
        if (_wcsicmp(extension.c_str(), L".mkv") != 0 && _wcsicmp(extension.c_str(), L".mp4") != 0) {
            continue;
        }

        std::error_code timeEc;
        const auto modified = std::filesystem::last_write_time(entry.path(), timeEc);

        AppContext::RecordingItem row;
        row.path = entry.path();
        row.fileName = row.path.filename().wstring();
        row.dungeonName.clear();
        row.modified = timeEc ? std::filesystem::file_time_type::clock::now() : modified;
        row.dateText = FormatLocalDate(FileTimeToSystemClock(row.modified));

        if (ctx->runRepository) {
            std::string dbError;
            const auto run = ctx->runRepository->GetRunByVideoPath(row.path, dbError);
            if (run.has_value()) {
                if (run->dungeonName.has_value() && !run->dungeonName->empty()) {
                    row.dungeonName = ToWide(*run->dungeonName);
                } else if (run->challengeMapId.has_value()) {
                    const auto inferredDungeonName = DungeonNameForChallengeMap(*run->challengeMapId);
                    if (!inferredDungeonName.empty()) {
                        row.dungeonName = ToWide(inferredDungeonName);
                    }
                } else if (_stricmp(run->triggerReason.c_str(), "manual") == 0) {
                    row.dungeonName = L"Manual Recording";
                }
                if (run->keystoneLevel.has_value()) {
                    row.keystoneLevel = *run->keystoneLevel;
                    row.keystoneText = L"+" + std::to_wstring(*run->keystoneLevel);
                }
                if (run->recordingEndedAt > run->recordingStartedAt) {
                    row.duration = std::chrono::duration_cast<std::chrono::seconds>(run->recordingEndedAt - run->recordingStartedAt);
                    row.durationText = FormatElapsed(row.duration);
                }
                if (_stricmp(run->result.c_str(), "success") == 0
                    || _stricmp(run->result.c_str(), "timed") == 0
                    || _stricmp(run->stopReason.c_str(), "mythic-success") == 0) {
                    row.outcome = AppContext::RecordingItem::Outcome::Success;
                } else if (_stricmp(run->result.c_str(), "failure") == 0
                    || _stricmp(run->result.c_str(), "depleted") == 0
                    || _stricmp(run->result.c_str(), "overtime") == 0
                    || _stricmp(run->stopReason.c_str(), "mythic-failure") == 0) {
                    row.outcome = AppContext::RecordingItem::Outcome::Failure;
                }
                for (const auto& participant : run->participants) {
                    AppContext::RecordingItem::ParticipantUi participantUi;
                    participantUi.guid = participant.guid;
                    if (participant.name.has_value()) {
                        participantUi.name = ToWide(*participant.name);
                    }
                    participantUi.specAbbrev = SpecAbbreviationFromName(participant.specName);
                    participantUi.specId = participant.specId;
                    participantUi.specName = participant.specName;
                    participantUi.className = participant.className;
                    participantUi.classColor = ClassColorForParticipant(participant.className);
                    row.participants.push_back(std::move(participantUi));
                }
            }
        }

        if (row.dungeonName.empty()) {
            row.dungeonName = row.path.stem().wstring();
        }
        if (row.keystoneLevel < 0) {
            row.keystoneText = L"-";
        }
        ctx->recordingItems.push_back(std::move(row));
    }

    BackfillRecordingParticipantsFromKnownGuids(ctx);
    SortRecordingItems(ctx);
    RepopulateRecordingsListControl(ctx);

    std::wostringstream summary;
    summary << L"Folder: " << folder << L" (" << ctx->recordingItems.size() << L" file";
    if (ctx->recordingItems.size() != 1) {
        summary << L"s";
    }
    summary << L")";
    SetWindowTextW(ctx->recordingsLabel, summary.str().c_str());
}

void RefreshLiveStatus(AppContext* ctx);

void SetActiveTab(AppContext* ctx, AppContext::MainTab tab)
{
    if (!ctx || !ctx->statusTabButton || !ctx->configurationTabButton || !ctx->chatPrivacyTabButton || !ctx->recordingsTabButton || !ctx->clipsTabButton || !ctx->aboutTabButton
        || !ctx->statusPanel || !ctx->recorderPanel || !ctx->chatPrivacyPanel || !ctx->recordingsPanel || !ctx->clipsPanel || !ctx->aboutPanel) {
        return;
    }

    if (tab == AppContext::MainTab::Status || tab == AppContext::MainTab::Clips) {
        ctx->ffmpegCheckRequested = true;
    }
    ClearClipsExportStatus(ctx);
    ctx->activeTab = tab;
    const bool showStatus = (tab == AppContext::MainTab::Status);
    const bool showConfiguration = (tab == AppContext::MainTab::Configuration);
    const bool showChatPrivacy = (tab == AppContext::MainTab::ChatPrivacy);
    const bool showRecordings = (tab == AppContext::MainTab::Recordings);
    const bool showClips = (tab == AppContext::MainTab::Clips);
    const bool showAbout = (tab == AppContext::MainTab::About);

    ShowWindow(ctx->statusPanel, showStatus ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->recorderPanel, showConfiguration ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->chatPrivacyPanel, showChatPrivacy ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->recordingsPanel, showRecordings ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->clipsPanel, showClips ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->aboutPanel, showAbout ? SW_SHOW : SW_HIDE);
    if (!showConfiguration && ctx->configurationTooltip && IsWindow(ctx->configurationTooltip)) {
        ShowWindow(ctx->configurationTooltip, SW_HIDE);
    }
    EnableWindow(ctx->statusTabButton, !showStatus);
    EnableWindow(ctx->configurationTabButton, !showConfiguration);
    EnableWindow(ctx->chatPrivacyTabButton, !showChatPrivacy);
    EnableWindow(ctx->recordingsTabButton, !showRecordings);
    EnableWindow(ctx->clipsTabButton, !showClips);
    EnableWindow(ctx->aboutTabButton, !showAbout);

    if (showRecordings) {
        RefreshRecordingsList(ctx);
    }
    if (showClips) {
        RefreshClipsSourceList(ctx);
    }
    if (showChatPrivacy) {
        RefreshChatBlockerImageCombo(ctx, {});
        SyncChatBlockerSelectionToImageMetadata(ctx, false);
        RefreshChatBlockerImageControls(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
    }
    if (tab == AppContext::MainTab::Status || tab == AppContext::MainTab::Clips) {
        RefreshLiveStatus(ctx);
    }
}

void ApplyAboutUpdateAvailabilityResult(AppContext* ctx, const UpdateAvailabilityPayload& payload)
{
    if (!ctx || !ctx->aboutPanel) {
        return;
    }

    if (payload.requestId != ctx->aboutUpdateCheckRequestId.load()) {
        return;
    }
    ctx->aboutUpdateCheckInProgress.store(false);

    HWND updateButton = GetDlgItem(ctx->aboutPanel, IDC_ABOUT_CHECK_UPDATES_BUTTON);
    if (!updateButton) {
        return;
    }

    HWND updateText = GetDlgItem(ctx->aboutPanel, IDC_ABOUT_UPDATE_TEXT);
    switch (payload.availability) {
    case bean::app::UpdateAvailability::UpdateAvailable:
        UpdateTransparentStaticText(updateText, payload.statusMessage.c_str());
        SetWindowTextW(updateButton, L"Update now");
        EnableWindow(updateButton, TRUE);
        SetStatus(ctx, payload.statusMessage);
        break;
    case bean::app::UpdateAvailability::UpToDate:
        UpdateTransparentStaticText(updateText, L"Up to date.");
        SetWindowTextW(updateButton, L"Check for updates");
        EnableWindow(updateButton, TRUE);
        break;
    case bean::app::UpdateAvailability::NotConfigured:
        UpdateTransparentStaticText(updateText, L"Failed to check for updates");
        SetWindowTextW(updateButton, L"Check for updates");
        EnableWindow(updateButton, TRUE);
        SetStatus(ctx, payload.statusMessage);
        break;
    case bean::app::UpdateAvailability::Failed:
        UpdateTransparentStaticText(updateText, L"Failed to check for updates");
        SetWindowTextW(updateButton, L"Check for updates");
        EnableWindow(updateButton, TRUE);
        SetStatus(ctx, payload.statusMessage);
        break;
    }
}

void RefreshAboutUpdateButtonState(AppContext* ctx)
{
    if (!ctx || !ctx->aboutPanel) {
        return;
    }

    HWND updateButton = GetDlgItem(ctx->aboutPanel, IDC_ABOUT_CHECK_UPDATES_BUTTON);
    if (!updateButton) {
        return;
    }
    HWND updateText = GetDlgItem(ctx->aboutPanel, IDC_ABOUT_UPDATE_TEXT);

    if (ctx->aboutUpdateCheckInProgress.exchange(true)) {
        return;
    }

    const std::uint64_t requestId = ctx->aboutUpdateCheckRequestId.fetch_add(1) + 1;
    EnableWindow(updateButton, FALSE);
    SetWindowTextW(updateButton, L"Checking...");
    UpdateTransparentStaticText(updateText, L"Checking for updates...");

    std::thread([ctx, requestId]() {
        auto* payload = new UpdateAvailabilityPayload();
        payload->requestId = requestId;
        payload->availability = bean::app::GetUpdateAvailability(payload->statusMessage);
        if (!ctx->mainWindow) {
            delete payload;
            return;
        }
        PostMessageW(ctx->mainWindow, WM_BEAN_UPDATE_AVAILABILITY_READY, 0, reinterpret_cast<LPARAM>(payload));
    }).detach();
}

void RefreshStatusCommandButtons(AppContext* ctx);

void RefreshLiveStatus(AppContext* ctx)
{
    if (!ctx || !ctx->orchestrator) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool wasRecording = ctx->isRecording;
    const bool monitoring = ctx->orchestrator->IsMonitoring();
    const bool recording = (ctx->orchestrator->GetState() == bean::core::OrchestratorState::Recording);
    const auto recordingSessionId = ctx->orchestrator->GetRecordingSessionId();

    if (recording && (!ctx->isRecording || ctx->activeRecordingSessionId != recordingSessionId)) {
        ctx->recordingStartedAt = std::chrono::steady_clock::now();
        ctx->activeRecordingSessionId = recordingSessionId;
    } else if (!recording) {
        ctx->recordingStartedAt.reset();
        ctx->activeRecordingSessionId = 0;
    }
    ctx->isMonitoring = monitoring;
    ctx->isRecording = recording;
    const bool recordingStateChanged = (wasRecording != ctx->isRecording);
    const bool wowWasDetected = ctx->wowWindowDetected;
    bool wowStatusRefreshed = false;
    if (!ctx->wowWindowLastCheckedAt.has_value()
        || (now - *ctx->wowWindowLastCheckedAt) >= kWowWindowPollInterval) {
        ctx->wowWindowDetected = DetectWowWindowForUi();
        ctx->wowWindowLastCheckedAt = now;
        wowStatusRefreshed = true;
    }
    const bool obsWasDetected = ctx->obsInstallDetected;
    bool obsStatusRefreshed = false;
    if (!ctx->obsInstallLastCheckedAt.has_value()
        || (now - *ctx->obsInstallLastCheckedAt) >= kObsInstallPollInterval) {
        ctx->obsInstallDetected = DetectUsableObsInstallForUi();
        ctx->obsInstallLastCheckedAt = now;
        obsStatusRefreshed = true;
    }
    const bool ffmpegWasDetected = ctx->ffmpegDetected;
    bool ffmpegStatusRefreshed = false;
    if (ctx->ffmpegCheckRequested
        || (!ctx->ffmpegDetected
            && (!ctx->ffmpegLastCheckedAt.has_value()
                || (now - *ctx->ffmpegLastCheckedAt) >= kObsInstallPollInterval))) {
        ctx->ffmpegDetected = DetectFfmpegForUi(ctx);
        ctx->ffmpegLastCheckedAt = now;
        ctx->ffmpegCheckRequested = false;
        ffmpegStatusRefreshed = true;
    }
    const bool warcraftRecorderWasDetected = ctx->warcraftRecorderDetected;
    bool warcraftRecorderStatusRefreshed = false;
    const auto warcraftRecorderPollInterval = ctx->warcraftRecorderDetected
        ? kWowWindowPollInterval
        : kWarcraftRecorderPollInterval;
    if (!ctx->warcraftRecorderLastCheckedAt.has_value()
        || (now - *ctx->warcraftRecorderLastCheckedAt) >= warcraftRecorderPollInterval) {
        ctx->warcraftRecorderDetected = DetectWarcraftRecorderForUi();
        ctx->warcraftRecorderLastCheckedAt = now;
        warcraftRecorderStatusRefreshed = true;
    }
    const bool advancedCombatLoggingWasEnabled = ctx->advancedCombatLoggingEnabled;
    bool advancedCombatLoggingStatusRefreshed = false;
    if (!ctx->advancedCombatLoggingLastCheckedAt.has_value()
        || (now - *ctx->advancedCombatLoggingLastCheckedAt) >= kWowWindowPollInterval) {
        ctx->advancedCombatLoggingEnabled = DetectAdvancedCombatLoggingForUi(ctx);
        ctx->advancedCombatLoggingLastCheckedAt = now;
        advancedCombatLoggingStatusRefreshed = true;
    }

    if (ctx->monitorIcon) {
        InvalidateRect(ctx->monitorIcon, nullptr, TRUE);
    }
    if (ctx->recordIcon) {
        InvalidateRect(ctx->recordIcon, nullptr, TRUE);
    }
    if (ctx->wowWindowIcon && (wowStatusRefreshed || wowWasDetected != ctx->wowWindowDetected)) {
        InvalidateRect(ctx->wowWindowIcon, nullptr, TRUE);
    }
    if (ctx->wowWindowText) {
        const wchar_t* wowStatusText = ctx->wowWindowDetected ? L"WoW window detected" : L"WoW window not detected";
        UpdateTransparentStaticText(ctx->wowWindowText, wowStatusText);
    }
    if (ctx->chatPreviewStatus) {
        const wchar_t* previewText = ctx->wowWindowDetected
            ? L"Preview targets WoW's client area (exclusive fullscreen may limit capture)."
            : L"WoW not detected; preview uses a placeholder.";
        UpdateTransparentStaticText(ctx->chatPreviewStatus, previewText);
    }
    if (ctx->obsInstallIcon && (obsStatusRefreshed || obsWasDetected != ctx->obsInstallDetected)) {
        InvalidateRect(ctx->obsInstallIcon, nullptr, TRUE);
    }
    if (ctx->obsInstallText) {
        const wchar_t* obsStatusText = ctx->obsInstallDetected ? L"OBS install detected" : L"OBS install not detected";
        UpdateTransparentStaticText(ctx->obsInstallText, obsStatusText);
    }
    if (ctx->ffmpegIcon && (ffmpegStatusRefreshed || ffmpegWasDetected != ctx->ffmpegDetected)) {
        InvalidateRect(ctx->ffmpegIcon, nullptr, TRUE);
    }
    if (ctx->ffmpegText) {
        const wchar_t* ffmpegStatusText = ctx->ffmpegDetected ? L"FFmpeg available for trim" : L"FFmpeg not found for trim";
        UpdateTransparentStaticText(ctx->ffmpegText, ffmpegStatusText);
    }
    if (ffmpegStatusRefreshed || ffmpegWasDetected != ctx->ffmpegDetected) {
        RefreshClipsPlaybackControls(ctx);
    }
    bool warcraftRecorderRowVisibilityChanged = false;
    HWND warcraftRecorderLabel = GetDlgItem(ctx->statusPanel, IDC_WARCRAFT_RECORDER_LABEL);
    if (warcraftRecorderLabel) {
        const bool shouldShow = ctx->warcraftRecorderDetected;
        const bool currentlyVisible = IsWindowVisible(warcraftRecorderLabel) != FALSE;
        if (shouldShow != currentlyVisible) {
            ShowWindow(warcraftRecorderLabel, shouldShow ? SW_SHOW : SW_HIDE);
            warcraftRecorderRowVisibilityChanged = true;
        }
    }
    if (ctx->warcraftRecorderIcon && (warcraftRecorderStatusRefreshed || warcraftRecorderWasDetected != ctx->warcraftRecorderDetected)) {
        const bool shouldShow = ctx->warcraftRecorderDetected;
        const bool currentlyVisible = IsWindowVisible(ctx->warcraftRecorderIcon) != FALSE;
        if (shouldShow != currentlyVisible) {
            ShowWindow(ctx->warcraftRecorderIcon, shouldShow ? SW_SHOW : SW_HIDE);
            warcraftRecorderRowVisibilityChanged = true;
        }
        InvalidateRect(ctx->warcraftRecorderIcon, nullptr, TRUE);
    }
    if (ctx->warcraftRecorderText && (warcraftRecorderStatusRefreshed || warcraftRecorderWasDetected != ctx->warcraftRecorderDetected)) {
        const bool shouldShow = ctx->warcraftRecorderDetected;
        const bool currentlyVisible = IsWindowVisible(ctx->warcraftRecorderText) != FALSE;
        if (shouldShow != currentlyVisible) {
            ShowWindow(ctx->warcraftRecorderText, shouldShow ? SW_SHOW : SW_HIDE);
            warcraftRecorderRowVisibilityChanged = true;
        }
        if (ctx->warcraftRecorderDetected) {
            UpdateTransparentStaticText(ctx->warcraftRecorderText, L"Detected - close Warcraft Recorder to avoid recording conflicts");
        }
    }
    if (warcraftRecorderRowVisibilityChanged && ctx->mainWindow) {
        RECT clientRect{};
        if (GetClientRect(ctx->mainWindow, &clientRect)) {
            LayoutMainUi(ctx, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
        }
    }
    if (ctx->advancedLoggingIcon
        && (advancedCombatLoggingStatusRefreshed || advancedCombatLoggingWasEnabled != ctx->advancedCombatLoggingEnabled)) {
        InvalidateRect(ctx->advancedLoggingIcon, nullptr, TRUE);
    }
    if (ctx->advancedLoggingText) {
        const wchar_t* advancedLoggingText = ctx->advancedCombatLoggingEnabled
            ? L"Enabled"
            : L"Disabled";
        UpdateTransparentStaticText(ctx->advancedLoggingText, advancedLoggingText);
        if (ctx->advancedLoggingHelpIcon) {
            RECT textRect{};
            if (GetWindowRect(ctx->advancedLoggingText, &textRect)) {
                HWND parent = GetParent(ctx->advancedLoggingText);
                if (parent) {
                    MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&textRect), 2);
                    HDC dc = GetDC(ctx->advancedLoggingText);
                    if (dc) {
                        HGDIOBJ oldFont = nullptr;
                        HFONT font = reinterpret_cast<HFONT>(SendMessageW(ctx->advancedLoggingText, WM_GETFONT, 0, 0));
                        if (font) {
                            oldFont = SelectObject(dc, font);
                        }
                        SIZE textSize{};
                        const int textLength = lstrlenW(advancedLoggingText);
                        if (GetTextExtentPoint32W(dc, advancedLoggingText, textLength, &textSize)) {
                            const int helpX = textRect.left + textSize.cx + 6;
                            MoveWindow(ctx->advancedLoggingHelpIcon, helpX, textRect.top + 4, 16, 16, TRUE);
                        }
                        if (oldFont) {
                            SelectObject(dc, oldFont);
                        }
                        ReleaseDC(ctx->advancedLoggingText, dc);
                    }
                }
            }
        }
    }
    if (ctx->statusTabButton
        && (wowWasDetected != ctx->wowWindowDetected
            || obsWasDetected != ctx->obsInstallDetected
            || ffmpegWasDetected != ctx->ffmpegDetected
            || warcraftRecorderWasDetected != ctx->warcraftRecorderDetected
            || advancedCombatLoggingWasEnabled != ctx->advancedCombatLoggingEnabled)) {
        InvalidateRect(ctx->statusTabButton, nullptr, TRUE);
    }
    if (ctx->warcraftRecorderDetected) {
        // If detection happened before status controls were ready (startup), log once
        // as soon as we can write to the status text/log.
        if (!ctx->warcraftRecorderWarningLogged && ctx->statusText) {
            SetStatus(ctx, L"Warning: Warcraft Recorder is running. Close it while using Bean to avoid recording conflicts.");
            ctx->warcraftRecorderWarningLogged = true;
        }
    } else {
        if (warcraftRecorderStatusRefreshed && warcraftRecorderWasDetected) {
            SetStatus(ctx, L"Warcraft Recorder no longer detected.");
        }
        ctx->warcraftRecorderWarningLogged = false;
    }
    if (ctx->lengthValue) {
        if (ctx->recordingStartedAt.has_value()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - *ctx->recordingStartedAt);
            SetWindowTextW(ctx->lengthValue, FormatElapsed(elapsed).c_str());
        } else {
            SetWindowTextW(ctx->lengthValue, L"00:00:00");
        }
        InvalidateRect(ctx->lengthValue, nullptr, TRUE);
    }
    if (recordingStateChanged && ctx->chatPreview) {
        ctx->chatPreviewLastInvalidateAt.reset();
        InvalidateRect(ctx->chatPreview, nullptr, FALSE);
    }
    if (ctx->chatPreview && ctx->activeTab == AppContext::MainTab::ChatPrivacy && !ctx->isRecording) {
        if (!ctx->chatPreviewLastInvalidateAt.has_value()
            || (now - *ctx->chatPreviewLastInvalidateAt) >= kChatPreviewInvalidateInterval) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
            ctx->chatPreviewLastInvalidateAt = now;
        }
    } else {
        ctx->chatPreviewLastInvalidateAt.reset();
    }

    ApplyTaskbarOverlayState(ctx);
    RefreshStatusCommandButtons(ctx);
}

void RefreshStatusCommandButtons(AppContext* ctx)
{
    if (!ctx || !ctx->statusPanel) {
        return;
    }

    HWND monitorStart = GetDlgItem(ctx->statusPanel, IDC_MONITOR_START);
    HWND monitorStop = GetDlgItem(ctx->statusPanel, IDC_MONITOR_STOP);
    HWND recordStart = GetDlgItem(ctx->statusPanel, IDC_RECORD_START);
    HWND recordStop = GetDlgItem(ctx->statusPanel, IDC_RECORD_STOP);

    if (monitorStart) {
        EnableWindow(monitorStart, ctx->isMonitoring ? FALSE : TRUE);
    }
    if (monitorStop) {
        EnableWindow(monitorStop, ctx->isMonitoring ? TRUE : FALSE);
    }
    if (recordStart) {
        EnableWindow(recordStart, ctx->isRecording ? FALSE : TRUE);
    }
    if (recordStop) {
        EnableWindow(recordStop, ctx->isRecording ? TRUE : FALSE);
    }
}

void PullSettingsFromUi(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    ctx->settings.outputDirectory = ToUtf8(GetWindowTextString(ctx->outputEdit));
    ctx->settings.wowLogDirectory = ToUtf8(GetWindowTextString(ctx->wowLogEdit));
    ctx->settings.width = ReadIntControl(ctx->widthEdit, 1920);
    ctx->settings.height = ReadIntControl(ctx->heightEdit, 1080);
    ctx->settings.fps = ReadIntControl(ctx->fpsEdit, 60);
    ctx->settings.postRunStopDelaySeconds = (std::max)(0, ReadIntControl(ctx->postRunDelayEdit, 30));
    const bool customBlockerImage = ctx->chatBlockerImageCustomRadio
        && SendMessageW(ctx->chatBlockerImageCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ctx->settings.chatBlockerCustomImagePath = customBlockerImage ? ResolveSelectedChatBlockerImagePath(ctx) : std::filesystem::path();
    ctx->settings.chatBlockerUseCustomImage = customBlockerImage && !ctx->settings.chatBlockerCustomImagePath.empty();
    ctx->settings.chatBlockerCustomImageSourceWidth = customBlockerImage ? (std::max)(0, ctx->chatBlockerCustomSourceWidth) : 0;
    ctx->settings.chatBlockerCustomImageSourceHeight = customBlockerImage ? (std::max)(0, ctx->chatBlockerCustomSourceHeight) : 0;
    ctx->settings.chatBlockerWidth = (std::max)(0, ReadIntControl(ctx->chatBlockerWidthEdit, ctx->settings.chatBlockerWidth));
    ctx->settings.chatBlockerHeight = (std::max)(0, ReadIntControl(ctx->chatBlockerHeightEdit, ctx->settings.chatBlockerHeight));
    RememberChatBlockerSizeForSelectedImage(ctx);
    ctx->settings.chatBlockerAnchor = ChatBlockerAnchorFromComboIndex(static_cast<int>(SendMessageW(ctx->chatBlockerAnchorCombo, CB_GETCURSEL, 0, 0)));
    ctx->settings.chatBlockerEnabled = (SendMessageW(ctx->chatBlockerEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);

    const int encoderIndex = static_cast<int>(SendMessageW(ctx->encoderCombo, CB_GETCURSEL, 0, 0));
    if (encoderIndex == 1) {
        ctx->settings.videoEncoder = "nvenc";
    } else if (encoderIndex == 2) {
        ctx->settings.videoEncoder = "amf";
    } else if (encoderIndex == 3) {
        ctx->settings.videoEncoder = "qsv";
    } else if (encoderIndex == 4) {
        ctx->settings.videoEncoder = "x264";
    } else {
        ctx->settings.videoEncoder = "gpu_auto";
    }

    const int presetIndex = static_cast<int>(SendMessageW(ctx->presetCombo, CB_GETCURSEL, 0, 0));
    if (presetIndex == 0) {
        ctx->settings.encoderPreset = "ultra";
    } else if (presetIndex == 1) {
        ctx->settings.encoderPreset = "high";
    } else if (presetIndex == 2) {
        ctx->settings.encoderPreset = "medium";
    } else if (presetIndex == 3) {
        ctx->settings.encoderPreset = "low";
    } else {
        ctx->settings.encoderPreset = "minimum";
    }

    const int containerIndex = static_cast<int>(SendMessageW(ctx->containerCombo, CB_GETCURSEL, 0, 0));
    ctx->settings.videoContainer = (containerIndex == 1) ? "mp4" : "mkv";
    if (SendMessageW(ctx->audioScopeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        ctx->settings.audioCaptureScope = bean::core::AppSettings::AudioCaptureScope::WowOnly;
    } else if (ctx->audioScopeWowDiscordRadio && SendMessageW(ctx->audioScopeWowDiscordRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        ctx->settings.audioCaptureScope = bean::core::AppSettings::AudioCaptureScope::WowAndDiscord;
    } else {
        ctx->settings.audioCaptureScope = bean::core::AppSettings::AudioCaptureScope::AllDesktop;
    }
    ctx->settings.captureMicrophone = (SendMessageW(ctx->microphoneCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ctx->settings.microphoneNoiseSuppression =
        (ctx->microphoneNoiseSuppressionCheck
            && SendMessageW(ctx->microphoneNoiseSuppressionCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const int microphoneIndex = static_cast<int>(SendMessageW(ctx->microphoneCombo, CB_GETCURSEL, 0, 0));
    if (microphoneIndex >= 0 && static_cast<size_t>(microphoneIndex) < ctx->microphoneOptions.size()) {
        ctx->settings.microphoneDeviceId = ctx->microphoneOptions[static_cast<size_t>(microphoneIndex)].deviceId;
    } else if (ctx->settings.microphoneDeviceId.empty()) {
        ctx->settings.microphoneDeviceId = "default";
    }
}

void AutoSaveChatBlockerSettings(AppContext* ctx)
{
    if (!ctx || !ctx->orchestrator || !ctx->chatBlockerAutoSaveArmed) {
        return;
    }

    const bool customBlockerImage = ctx->chatBlockerImageCustomRadio
        && SendMessageW(ctx->chatBlockerImageCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ctx->settings.chatBlockerCustomImagePath = customBlockerImage ? ResolveSelectedChatBlockerImagePath(ctx) : std::filesystem::path();
    ctx->settings.chatBlockerUseCustomImage = customBlockerImage && !ctx->settings.chatBlockerCustomImagePath.empty();
    ctx->settings.chatBlockerCustomImageSourceWidth = customBlockerImage ? (std::max)(0, ctx->chatBlockerCustomSourceWidth) : 0;
    ctx->settings.chatBlockerCustomImageSourceHeight = customBlockerImage ? (std::max)(0, ctx->chatBlockerCustomSourceHeight) : 0;
    ctx->settings.chatBlockerWidth = (std::max)(0, ReadIntControl(ctx->chatBlockerWidthEdit, ctx->settings.chatBlockerWidth));
    ctx->settings.chatBlockerHeight = (std::max)(0, ReadIntControl(ctx->chatBlockerHeightEdit, ctx->settings.chatBlockerHeight));
    RememberChatBlockerSizeForSelectedImage(ctx);
    const int anchorIndex = static_cast<int>(SendMessageW(ctx->chatBlockerAnchorCombo, CB_GETCURSEL, 0, 0));
    ctx->settings.chatBlockerAnchor = ChatBlockerAnchorFromComboIndex(anchorIndex);
    ctx->settings.chatBlockerEnabled = (SendMessageW(ctx->chatBlockerEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);

    std::string error;
    if (!ctx->settingsStore.Save(ctx->settings, error)) {
        SetStatus(ctx, std::wstring(L"Auto-save failed: ") + ToWide(error));
        return;
    }
    ctx->orchestrator->ApplySettings(ctx->settings);
}

void AutoSaveConfigurationSettings(AppContext* ctx)
{
    if (!ctx || !ctx->orchestrator || !ctx->configurationAutoSaveArmed) {
        return;
    }

    PullSettingsFromUi(ctx);

    std::string error;
    if (!ctx->settingsStore.Save(ctx->settings, error)) {
        SetStatus(ctx, std::wstring(L"Auto-save failed: ") + ToWide(error));
        return;
    }
    ctx->orchestrator->ApplySettings(ctx->settings);
}

bool DirectoryExists(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec);
}

bool EnsureOutputDirectoryReady(const std::filesystem::path& outputDirectory, std::string& error)
{
    error.clear();
    if (outputDirectory.empty()) {
        error = "Output folder is empty.";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec) {
        error = "Could not create output folder: " + ec.message();
        return false;
    }
    return true;
}

void RefreshFolderAvailability(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    const auto outputPath = GetWindowTextString(ctx->outputEdit);
    const auto wowLogPath = GetWindowTextString(ctx->wowLogEdit);

    ctx->outputAvailable = DirectoryExists(outputPath);
    ctx->wowLogAvailable = DirectoryExists(wowLogPath);
    ctx->outputFolderWillBeCreatedOnRecordStart = !ctx->outputAvailable && !outputPath.empty();

    if (ctx->outputStatus) {
        const wchar_t* outputStatusText = ctx->outputAvailable
            ? L"\x2713"
            : (ctx->outputFolderWillBeCreatedOnRecordStart ? L"(!)" : L"X");
        SetWindowTextW(ctx->outputStatus, outputStatusText);
        InvalidateRect(ctx->outputStatus, nullptr, TRUE);
    }
    if (ctx->wowLogStatus) {
        SetWindowTextW(ctx->wowLogStatus, ctx->wowLogAvailable ? L"\x2713" : L"X");
        InvalidateRect(ctx->wowLogStatus, nullptr, TRUE);
    }
    if (ctx->configurationTabButton) {
        InvalidateRect(ctx->configurationTabButton, nullptr, TRUE);
    }
    if (ctx->activeTab == AppContext::MainTab::Recordings) {
        RefreshRecordingsList(ctx);
    } else if (ctx->activeTab == AppContext::MainTab::Clips) {
        RefreshClipsSourceList(ctx);
    }
}

bool ApplyReasonableDefaults(bean::core::AppSettings& settings, std::string& warning)
{
    warning.clear();
    bool changed = false;

    if (settings.outputDirectory.empty()) {
        const auto videosPath = GetKnownFolderPath(FOLDERID_Videos);
        if (!videosPath.empty()) {
            const auto output = std::filesystem::path(videosPath) / "Bean";
            std::error_code ec;
            std::filesystem::create_directories(output, ec);
            if (!ec) {
                settings.outputDirectory = output;
                changed = true;
            } else {
                warning = "Could not create default output folder in Videos.";
            }
        } else {
            warning = "Could not resolve Videos folder for default output path.";
        }
    }

    if (settings.wowLogDirectory.empty()) {
        settings.wowLogDirectory = ResolveDefaultWowLogDirectory();
        changed = true;
    }
    if (settings.videoEncoder.empty()) {
        settings.videoEncoder = "gpu_auto";
        changed = true;
    }
    if (settings.encoderPreset == "quality") {
        settings.encoderPreset = "high";
        changed = true;
    } else if (settings.encoderPreset == "balanced") {
        settings.encoderPreset = "medium";
        changed = true;
    } else if (settings.encoderPreset == "speed") {
        settings.encoderPreset = "low";
        changed = true;
    } else if (settings.encoderPreset != "ultra"
        && settings.encoderPreset != "high"
        && settings.encoderPreset != "medium"
        && settings.encoderPreset != "low"
        && settings.encoderPreset != "minimum") {
        settings.encoderPreset = "high";
        changed = true;
    }
    if (settings.videoContainer.empty()) {
        settings.videoContainer = "mp4";
        changed = true;
    }
    if (settings.postRunStopDelaySeconds < 0) {
        settings.postRunStopDelaySeconds = 30;
        changed = true;
    }
    if (settings.chatBlockerWidth < 0) {
        settings.chatBlockerWidth = 0;
        changed = true;
    }
    if (settings.chatBlockerHeight < 0) {
        settings.chatBlockerHeight = 0;
        changed = true;
    }
    if (settings.chatBlockerCustomImageSourceWidth < 0) {
        settings.chatBlockerCustomImageSourceWidth = 0;
        changed = true;
    }
    if (settings.chatBlockerCustomImageSourceHeight < 0) {
        settings.chatBlockerCustomImageSourceHeight = 0;
        changed = true;
    }
    if (!settings.chatBlockerUseCustomImage) {
        if (settings.chatBlockerCustomImageSourceWidth != 0 || settings.chatBlockerCustomImageSourceHeight != 0) {
            settings.chatBlockerCustomImageSourceWidth = 0;
            settings.chatBlockerCustomImageSourceHeight = 0;
            changed = true;
        }
    } else if (settings.chatBlockerCustomImagePath.empty()) {
        settings.chatBlockerUseCustomImage = false;
        settings.chatBlockerCustomImageSourceWidth = 0;
        settings.chatBlockerCustomImageSourceHeight = 0;
        changed = true;
    } else {
        std::error_code customImageEc;
        if (!std::filesystem::exists(settings.chatBlockerCustomImagePath, customImageEc) || customImageEc) {
            settings.chatBlockerUseCustomImage = false;
            settings.chatBlockerCustomImagePath.clear();
            settings.chatBlockerCustomImageSourceWidth = 0;
            settings.chatBlockerCustomImageSourceHeight = 0;
            changed = true;
        }
    }
    if (settings.microphoneDeviceId.empty()) {
        settings.microphoneDeviceId = "default";
        changed = true;
    }

    return changed;
}

void PushSettingsToUi(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    SetWindowTextW(ctx->outputEdit, ToWide(ctx->settings.outputDirectory.string()).c_str());
    SetWindowTextW(ctx->wowLogEdit, ToWide(ctx->settings.wowLogDirectory.string()).c_str());
    SetWindowTextW(ctx->widthEdit, ToWide(std::to_string(ctx->settings.width)).c_str());
    SetWindowTextW(ctx->heightEdit, ToWide(std::to_string(ctx->settings.height)).c_str());
    SetWindowTextW(ctx->fpsEdit, ToWide(std::to_string(ctx->settings.fps)).c_str());
    SetWindowTextW(ctx->postRunDelayEdit, ToWide(std::to_string(ctx->settings.postRunStopDelaySeconds)).c_str());
    const std::wstring selectedCustomImageFileName = ctx->settings.chatBlockerCustomImagePath.filename().wstring();
    RefreshChatBlockerImageCombo(ctx, selectedCustomImageFileName);
    SendMessageW(ctx->chatBlockerImageBlankRadio, BM_SETCHECK, ctx->settings.chatBlockerUseCustomImage ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(ctx->chatBlockerImageCustomRadio, BM_SETCHECK, ctx->settings.chatBlockerUseCustomImage ? BST_CHECKED : BST_UNCHECKED, 0);
    ctx->chatBlockerCustomSourceWidth = (std::max)(0, ctx->settings.chatBlockerCustomImageSourceWidth);
    ctx->chatBlockerCustomSourceHeight = (std::max)(0, ctx->settings.chatBlockerCustomImageSourceHeight);
    SyncChatBlockerSelectionToImageMetadata(ctx, false);
    SetWindowTextW(ctx->chatBlockerWidthEdit, ToWide(std::to_string(ctx->settings.chatBlockerWidth)).c_str());
    SetWindowTextW(ctx->chatBlockerHeightEdit, ToWide(std::to_string(ctx->settings.chatBlockerHeight)).c_str());
    SendMessageW(ctx->chatBlockerAnchorCombo, CB_SETCURSEL, static_cast<WPARAM>(ChatBlockerAnchorToComboIndex(ctx->settings.chatBlockerAnchor)), 0);
    SendMessageW(ctx->chatBlockerEnabledCheck, BM_SETCHECK, ctx->settings.chatBlockerEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    RefreshChatBlockerImageControls(ctx);

    int encoderIndex = 0;
    if (ctx->settings.videoEncoder == "nvenc") {
        encoderIndex = 1;
    } else if (ctx->settings.videoEncoder == "amf") {
        encoderIndex = 2;
    } else if (ctx->settings.videoEncoder == "qsv") {
        encoderIndex = 3;
    } else if (ctx->settings.videoEncoder == "x264") {
        encoderIndex = 4;
    }
    SendMessageW(ctx->encoderCombo, CB_SETCURSEL, static_cast<WPARAM>(encoderIndex), 0);

    int presetIndex = 1;
    if (ctx->settings.encoderPreset == "ultra") {
        presetIndex = 0;
    } else if (ctx->settings.encoderPreset == "medium") {
        presetIndex = 2;
    } else if (ctx->settings.encoderPreset == "low") {
        presetIndex = 3;
    } else if (ctx->settings.encoderPreset == "minimum") {
        presetIndex = 4;
    } else if (ctx->settings.encoderPreset == "balanced") {
        presetIndex = 2;
    } else if (ctx->settings.encoderPreset == "speed") {
        presetIndex = 3;
    } else if (ctx->settings.encoderPreset == "quality") {
        presetIndex = 1;
    }
    SendMessageW(ctx->presetCombo, CB_SETCURSEL, static_cast<WPARAM>(presetIndex), 0);

    int containerIndex = 0;
    if (ctx->settings.videoContainer == "mp4") {
        containerIndex = 1;
    }
    SendMessageW(ctx->containerCombo, CB_SETCURSEL, static_cast<WPARAM>(containerIndex), 0);
    SendMessageW(
        ctx->audioScopeCheck,
        BM_SETCHECK,
        ctx->settings.audioCaptureScope == bean::core::AppSettings::AudioCaptureScope::WowOnly ? BST_CHECKED : BST_UNCHECKED,
        0);
    if (ctx->audioScopeWowDiscordRadio) {
        SendMessageW(
            ctx->audioScopeWowDiscordRadio,
            BM_SETCHECK,
            ctx->settings.audioCaptureScope == bean::core::AppSettings::AudioCaptureScope::WowAndDiscord ? BST_CHECKED : BST_UNCHECKED,
            0);
    }
    SendMessageW(
        ctx->audioScopeAllRadio,
        BM_SETCHECK,
        ctx->settings.audioCaptureScope == bean::core::AppSettings::AudioCaptureScope::AllDesktop ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(ctx->microphoneCheck, BM_SETCHECK, ctx->settings.captureMicrophone ? BST_CHECKED : BST_UNCHECKED, 0);
    if (ctx->microphoneNoiseSuppressionCheck) {
        SendMessageW(
            ctx->microphoneNoiseSuppressionCheck,
            BM_SETCHECK,
            ctx->settings.microphoneNoiseSuppression ? BST_CHECKED : BST_UNCHECKED,
            0);
    }
    RefreshMicrophoneDeviceOptionsUi(ctx);
    if (ctx->youtubePrivacyCombo) {
        SendMessageW(ctx->youtubePrivacyCombo, CB_SETCURSEL, 0, 0);
    }
    RefreshFolderAvailability(ctx);
    RefreshYouTubeUiState(ctx);
}

void HandleCommand(HWND hwnd, AppContext* ctx, int controlId)
{
    if (!ctx) {
        return;
    }

    switch (controlId) {
    case IDC_TAB_STATUS:
        SetActiveTab(ctx, AppContext::MainTab::Status);
        break;
    case IDC_TAB_CONFIGURATION:
        SetActiveTab(ctx, AppContext::MainTab::Configuration);
        break;
    case IDC_TAB_CHAT_PRIVACY:
        SetActiveTab(ctx, AppContext::MainTab::ChatPrivacy);
        break;
    case IDC_TAB_RECORDINGS:
        SetActiveTab(ctx, AppContext::MainTab::Recordings);
        break;
    case IDC_TAB_CLIPS:
        SetActiveTab(ctx, AppContext::MainTab::Clips);
        break;
    case IDC_TAB_ABOUT:
        SetActiveTab(ctx, AppContext::MainTab::About);
        RefreshAboutUpdateButtonState(ctx);
        break;
    case IDC_OUTPUT_BROWSE: {
        const auto folder = PickFolder(hwnd);
        if (!folder.empty()) {
            SetWindowTextW(ctx->outputEdit, folder.c_str());
            RefreshFolderAvailability(ctx);
        }
        break;
    }
    case IDC_LOG_BROWSE: {
        const auto folder = PickFolder(hwnd);
        if (!folder.empty()) {
            SetWindowTextW(ctx->wowLogEdit, folder.c_str());
            RefreshFolderAvailability(ctx);
        }
        break;
    }
    case IDC_MICROPHONE_CHECK: {
        RefreshMicrophoneOptionsUi(ctx);
        break;
    }
    case IDC_CHAT_BLOCKER_ANCHOR_COMBO:
        AutoSaveChatBlockerSettings(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    case IDC_CHAT_BLOCKER_IMAGE_BLANK_RADIO:
        RefreshChatBlockerImageControls(ctx);
        AutoSaveChatBlockerSettings(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    case IDC_CHAT_BLOCKER_IMAGE_CUSTOM_RADIO:
        SyncChatBlockerSelectionToImageMetadata(ctx, true);
        RefreshChatBlockerImageControls(ctx);
        AutoSaveChatBlockerSettings(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    case IDC_CHAT_BLOCKER_IMAGE_IMPORT_BUTTON: {
        const auto selectedFile = PickImageFile(hwnd);
        if (selectedFile.empty()) {
            break;
        }
        std::wstring saveError;
        if (!SaveCustomChatBlockerImage(ctx, selectedFile, saveError)) {
            SetStatus(ctx, std::wstring(L"Custom chat blocker image error: ") + saveError);
            break;
        }
        SetStatus(ctx, L"Custom chat blocker image imported successfully.");
        SendMessageW(ctx->chatBlockerImageCustomRadio, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(ctx->chatBlockerImageBlankRadio, BM_SETCHECK, BST_UNCHECKED, 0);
        SyncChatBlockerSelectionToImageMetadata(ctx, true);
        RefreshChatBlockerImageControls(ctx);
        AutoSaveChatBlockerSettings(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    }
    case IDC_CHAT_BLOCKER_IMAGE_OPEN_FOLDER_BUTTON: {
        const auto imagesDirectory = ResolveChatBlockerImagesDirectory(ctx);
        if (imagesDirectory.empty()) {
            SetStatus(ctx, L"Custom image folder is unavailable.");
            break;
        }
        std::error_code ec;
        std::filesystem::create_directories(imagesDirectory, ec);
        if (ec) {
            SetStatus(ctx, std::wstring(L"Failed to open image folder: ") + ToWide(ec.message()));
            break;
        }
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", imagesDirectory.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open custom image folder.");
        }
        RefreshChatBlockerImageCombo(ctx, {});
        RefreshChatBlockerImageControls(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    }
    case IDC_CHAT_BLOCKER_ENABLED_CHECK:
        AutoSaveChatBlockerSettings(ctx);
        if (ctx->chatPreview) {
            InvalidateRect(ctx->chatPreview, nullptr, FALSE);
        }
        break;
    case IDC_RECORDINGS_REFRESH:
        RefreshRecordingsList(ctx);
        break;
    case IDC_CLIPS_REFRESH:
        RefreshClipsSourceList(ctx);
        SetStatus(ctx, L"Clip source list refreshed.");
        break;
    case IDC_CLIPS_SOURCE_COMBO:
        LoadClipFromSelection(ctx, true);
        break;
    case IDC_CLIPS_PLAY_PAUSE: {
        if (!ctx->clipsLoaded || !ctx->clipsPreviewEngine) {
            break;
        }
        if (ctx->clipsIsPlaying) {
            const HRESULT pauseHr = ctx->clipsPreviewEngine->Pause();
            if (SUCCEEDED(pauseHr)) {
                ctx->clipsIsPlaying = false;
                SetStatus(
                    ctx,
                    L"Clip pause requested (position-ms="
                        + std::to_wstring(ctx->clipsPreviewEngine->PositionMilliseconds())
                        + L").");
            } else {
                SetStatus(ctx, L"Clip pause failed (" + FormatHresultHex(pauseHr) + L").");
            }
        } else {
            const HRESULT playHr = ctx->clipsPreviewEngine->Play();
            if (SUCCEEDED(playHr)) {
                ctx->clipsIsPlaying = true;
                SetStatus(
                    ctx,
                    std::wstring(L"Clip play requested (playing=")
                        + (ctx->clipsPreviewEngine->IsPlaying() ? L"yes" : L"no")
                        + L", position-ms="
                        + std::to_wstring(ctx->clipsPreviewEngine->PositionMilliseconds())
                        + L").");
            } else {
                SetStatus(ctx, L"Clip play failed (" + FormatHresultHex(playHr) + L").");
            }
        }
        RefreshClipsPlaybackControls(ctx);
        break;
    }
    case IDC_CLIPS_SET_START: {
        if (!ctx->clipsLoaded) {
            break;
        }
        const int currentSeconds = QueryClipPositionMs(ctx, 0) / 1000;
        if (ctx->clipsStartEdit) {
            SetWindowTextW(ctx->clipsStartEdit, std::to_wstring(currentSeconds).c_str());
        }
        if (ctx->clipsTimeline) {
            InvalidateControlAndParentRegion(ctx->clipsTimeline);
        }
        break;
    }
    case IDC_CLIPS_SET_END: {
        if (!ctx->clipsLoaded) {
            break;
        }
        const int currentSeconds = QueryClipPositionMs(ctx, 0) / 1000;
        if (ctx->clipsEndEdit) {
            SetWindowTextW(ctx->clipsEndEdit, std::to_wstring(currentSeconds).c_str());
        }
        if (ctx->clipsTimeline) {
            InvalidateControlAndParentRegion(ctx->clipsTimeline);
        }
        break;
    }
    case IDC_CLIPS_EXPORT: {
        if (ctx->clipsExportInProgress.load()) {
            SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Exporting, L"Exporting...");
            break;
        }
        if (!ctx->clipsLoaded || ctx->clipsLoadedPath.empty()) {
            SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"Select and load a recording first.");
            break;
        }
        if (!std::filesystem::exists(ctx->clipsLoadedPath)) {
            SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"Selected recording file no longer exists.");
            break;
        }
        int startSeconds = 0;
        int endSeconds = 0;
        if (!ParseClipSeconds(GetWindowTextString(ctx->clipsStartEdit), startSeconds)
            || !ParseClipSeconds(GetWindowTextString(ctx->clipsEndEdit), endSeconds)) {
            SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"Start and end must be valid whole seconds.");
            break;
        }
        if (endSeconds <= startSeconds) {
            SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"End must be greater than start.");
            break;
        }
        const auto ffmpegPath = ResolveFfmpegExecutablePath(ctx);
        if (!ffmpegPath.has_value()) {
            SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"FFmpeg executable could not be found.");
            break;
        }
        const auto recordingsFolder = ResolveRecordingsFolderPath(ctx);
        if (recordingsFolder.empty()) {
            SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"Recordings folder is unavailable.");
            break;
        }
        const auto clipsFolder = recordingsFolder / "Clips";
        std::error_code clipsCreateEc;
        std::filesystem::create_directories(clipsFolder, clipsCreateEc);
        if (clipsCreateEc) {
            SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, std::wstring(L"Could not create Clips folder: ") + ToWide(clipsCreateEc.message()));
            break;
        }

        const std::wstring sourceStem = ctx->clipsLoadedPath.stem().wstring();
        const std::wstring extension = ctx->clipsLoadedPath.extension().wstring().empty() ? L".mp4" : ctx->clipsLoadedPath.extension().wstring();
        std::wstringstream outputName;
        outputName << sourceStem << L"_clip_" << startSeconds << L"_" << endSeconds << extension;
        const auto outputPath = clipsFolder / outputName.str();

        ctx->clipsExportInProgress.store(true);
        RefreshClipsPlaybackControls(ctx);
        SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Exporting, L"Exporting...");
        SetStatus(ctx, std::wstring(L"Exporting clip to ") + outputPath.filename().wstring() + L"...");
        const std::filesystem::path inputPath = ctx->clipsLoadedPath;
        const std::filesystem::path ffmpegExe = *ffmpegPath;
        std::thread([ctx, ffmpegExe, inputPath, outputPath, startSeconds, endSeconds]() {
            std::wstringstream command;
            command << L"\"" << ffmpegExe.wstring() << L"\""
                    << L" -y -ss " << startSeconds
                    << L" -to " << endSeconds
                    << L" -i \"" << inputPath.wstring() << L"\""
                    << L" -c copy \"" << outputPath.wstring() << L"\"";

            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            PROCESS_INFORMATION processInfo{};
            std::wstring commandLine = command.str();
            BOOL created = CreateProcessW(
                nullptr,
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startupInfo,
                &processInfo);

            if (!created) {
                const DWORD errorCode = GetLastError();
                auto* payload = new ClipExportCompletePayload();
                payload->message = std::wstring(L"Export failed to start (error ") + std::to_wstring(errorCode) + L").";
                PostMessageW(ctx->mainWindow, WM_BEAN_CLIPS_EXPORT_COMPLETE, 0, reinterpret_cast<LPARAM>(payload));
                PostStatus(ctx, std::wstring(L"Clip export failed to start (error ") + std::to_wstring(errorCode) + L").");
                ctx->clipsExportInProgress.store(false);
                PostMessageW(ctx->mainWindow, WM_BEAN_CLIPS_UI_REFRESH, 0, 0);
                return;
            }

            WaitForSingleObject(processInfo.hProcess, INFINITE);
            DWORD exitCode = 1;
            GetExitCodeProcess(processInfo.hProcess, &exitCode);
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);

            if (exitCode == 0) {
                auto* payload = new ClipExportCompletePayload();
                payload->success = true;
                payload->message = L"Export complete!";
                PostMessageW(ctx->mainWindow, WM_BEAN_CLIPS_EXPORT_COMPLETE, 0, reinterpret_cast<LPARAM>(payload));
                PostStatus(ctx, std::wstring(L"Clip export complete: ") + outputPath.wstring());
            } else {
                auto* payload = new ClipExportCompletePayload();
                payload->message = std::wstring(L"Export failed (ffmpeg exit code ") + std::to_wstring(exitCode) + L").";
                PostMessageW(ctx->mainWindow, WM_BEAN_CLIPS_EXPORT_COMPLETE, 0, reinterpret_cast<LPARAM>(payload));
                PostStatus(ctx, std::wstring(L"Clip export failed (ffmpeg exit code ") + std::to_wstring(exitCode) + L").");
            }
            ctx->clipsExportInProgress.store(false);
            PostMessageW(ctx->mainWindow, WM_BEAN_CLIPS_UI_REFRESH, 0, 0);
        }).detach();
        break;
    }
    case IDC_CLIPS_OPEN_FOLDER: {
        const auto clipsFolder = ResolveClipsOutputFolderPath(ctx);
        if (clipsFolder.empty()) {
            SetStatus(ctx, L"Clips folder is unavailable.");
            break;
        }
        std::error_code ec;
        std::filesystem::create_directories(clipsFolder, ec);
        if (ec) {
            SetStatus(ctx, std::wstring(L"Could not open Clips folder: ") + ToWide(ec.message()));
            break;
        }
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", clipsFolder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open Clips folder.");
        }
        break;
    }
    case IDC_RECORDINGS_OPEN_FOLDER: {
        std::wstring folder = GetWindowTextString(ctx->outputEdit);
        if (folder.empty()) {
            folder = ToWide(ctx->settings.outputDirectory.string());
        }
        if (!DirectoryExists(folder)) {
            SetStatus(ctx, L"Cannot open recordings folder: path is unavailable.");
            break;
        }
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open recordings folder.");
        }
        break;
    }
    case IDC_RECORDINGS_OPEN_DB_FOLDER: {
        if (!ctx->runRepository) {
            SetStatus(ctx, L"Run metadata repository is unavailable.");
            break;
        }
        const auto dbFolder = ctx->runRepository->GetDatabasePath().parent_path();
        if (dbFolder.empty() || !std::filesystem::exists(dbFolder)) {
            SetStatus(ctx, L"Run metadata database folder is unavailable.");
            break;
        }
        const auto folderWide = dbFolder.wstring();
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", folderWide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open run metadata database folder.");
        }
        break;
    }
    case IDC_STATUS_OPEN_LOG_FOLDER: {
        const auto folder = ResolveStatusLogDirectory(ctx);
        if (folder.empty()) {
            SetStatus(ctx, L"Status log folder is unavailable.");
            break;
        }
        std::error_code ec;
        if (!std::filesystem::exists(folder, ec)) {
            std::filesystem::create_directories(folder, ec);
            if (ec) {
                SetStatus(ctx, std::wstring(L"Failed to prepare status log folder: ") + ToWide(ec.message()));
                break;
            }
        }
        const std::wstring folderWide = folder.wstring();
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", folderWide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open status log folder.");
        }
        break;
    }
    case IDC_YOUTUBE_LINK_BUTTON: {
        PullSettingsFromUi(ctx);
        const std::string authServerUrl = GetYouTubeAuthServerUrl();
        if (authServerUrl.empty()) {
            SetStatus(ctx, kYouTubeOAuthCredentialsMissingMessage);
            break;
        }
        if (ctx->youtubeBusy.load()) {
            SetStatus(ctx, L"YouTube action already in progress.");
            break;
        }
        ctx->youtubeBusy.store(true);
        RefreshYouTubeUiState(ctx);
        SetStatus(ctx, L"Opening browser for YouTube authorization...");
        std::thread([ctx, hwnd, authServerUrl]() {
            const auto auth = bean::integrations::YouTubeUploader::AuthorizeDesktop(hwnd, authServerUrl);
            auto* payload = new YouTubeAuthCompletionPayload();
            payload->success = auth.success;
            payload->clientId = auth.clientId;
            payload->refreshToken = auth.refreshToken;
            payload->channelId = auth.channelId;
            payload->channelTitle = auth.channelTitle;
            payload->error = auth.error;
            PostMessageW(ctx->mainWindow, WM_BEAN_YOUTUBE_AUTH_COMPLETE, 0, reinterpret_cast<LPARAM>(payload));
        }).detach();
        break;
    }
    case IDC_YOUTUBE_UNLINK_BUTTON: {
        if (ctx->youtubeBusy.load()) {
            SetStatus(ctx, L"YouTube action already in progress.");
            break;
        }
        ctx->youtubeUnlinkConfirmPending = true;
        RefreshYouTubeUiState(ctx);
        break;
    }
    case IDC_YOUTUBE_UNLINK_YES_BUTTON: {
        if (ctx->youtubeBusy.load()) {
            SetStatus(ctx, L"YouTube action already in progress.");
            break;
        }
        ctx->youtubeUnlinkConfirmPending = false;
        UnlinkYouTubeAccount(ctx);
        RefreshYouTubeUiState(ctx);
        break;
    }
    case IDC_YOUTUBE_UNLINK_NO_BUTTON:
        ctx->youtubeUnlinkConfirmPending = false;
        RefreshYouTubeUiState(ctx);
        break;
    case IDC_YOUTUBE_ACCOUNT_LINK: {
        if (ctx->settings.youtubeChannelId.empty()) {
            SetStatus(ctx, L"No linked YouTube channel URL is available.");
            break;
        }
        std::wstring url = L"https://www.youtube.com/channel/";
        url += ToWide(ctx->settings.youtubeChannelId);
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open linked YouTube channel.");
        }
        break;
    }
    case IDC_YOUTUBE_UPLOAD_BUTTON: {
        if (ctx->youtubeBusy.load()) {
            SetStatus(ctx, L"YouTube action already in progress.");
            break;
        }
        PullSettingsFromUi(ctx);
        const int selected = GetSelectedRecordingIndex(ctx);
        if (selected < 0 || static_cast<size_t>(selected) >= ctx->recordingItems.size()) {
            SetStatus(ctx, L"Select a recording before uploading.");
            break;
        }
        if (ctx->settings.youtubeClientId.empty() || ctx->settings.youtubeRefreshToken.empty()) {
            SetStatus(ctx, L"Link your YouTube account first.");
            break;
        }
        const std::wstring titleWide = GetWindowTextString(ctx->youtubeTitleEdit);
        if (titleWide.empty()) {
            SetStatus(ctx, L"Enter a title for the upload.");
            break;
        }
        bean::integrations::YouTubePrivacy privacy = bean::integrations::YouTubePrivacy::Private;
        const int privacyIndex = static_cast<int>(SendMessageW(ctx->youtubePrivacyCombo, CB_GETCURSEL, 0, 0));
        if (privacyIndex == 1) {
            privacy = bean::integrations::YouTubePrivacy::Unlisted;
        } else if (privacyIndex == 2) {
            privacy = bean::integrations::YouTubePrivacy::Public;
        }

        const auto path = ctx->recordingItems[static_cast<size_t>(selected)].path;
        const auto title = ToUtf8(titleWide);
        bean::integrations::YouTubeCredentials creds;
        creds.clientId = ctx->settings.youtubeClientId;
        creds.refreshToken = ctx->settings.youtubeRefreshToken;
        ctx->youtubeBusy.store(true);
        RefreshYouTubeUiState(ctx);
        SetRecordingsUploadUi(ctx, 0, std::wstring(L"Uploading: ") + path.filename().wstring());
        SetStatus(ctx, std::wstring(L"Uploading to YouTube: ") + path.filename().wstring());
        std::thread([ctx, path, title, privacy, creds]() {
            bean::integrations::YouTubeUploadRequest req;
            req.videoPath = path;
            req.title = title;
            req.privacy = privacy;
            int lastPercent = -1;
            const auto upload = bean::integrations::YouTubeUploader::UploadVideo(
                creds,
                req,
                [&lastPercent, ctx](uint64_t bytesSent, uint64_t totalBytes, const std::string& phase) {
                    if (phase == "auth") {
                        PostYouTubeUploadProgress(ctx, 0, L"Preparing YouTube authorization...");
                        return;
                    }
                    if (phase == "session") {
                        PostYouTubeUploadProgress(ctx, 0, L"Starting YouTube upload session...");
                        return;
                    }
                    if (phase == "complete") {
                        PostYouTubeUploadProgress(ctx, 100, L"Upload complete.");
                        return;
                    }
                    if (phase == "uploading") {
                        int percent = 0;
                        if (totalBytes > 0) {
                            percent = static_cast<int>((bytesSent * 100ULL) / totalBytes);
                        }
                        percent = std::clamp(percent, 0, 100);
                        if (percent == lastPercent && percent != 100) {
                            return;
                        }
                        lastPercent = percent;
                        std::wostringstream text;
                        text << L"Uploading to YouTube... " << percent << L"%";
                        PostYouTubeUploadProgress(ctx, percent, text.str());
                    }
                });
            if (!upload.success) {
                PostStatus(ctx, std::wstring(L"YouTube upload failed: ") + ToWide(upload.error));
                PostYouTubeUploadProgress(ctx, 0, std::wstring(L"Upload failed: ") + ToWide(upload.error));
                ctx->youtubeBusy.store(false);
                RequestYouTubeUiRefresh(ctx);
                return;
            }

            std::wstring message = L"YouTube upload complete.";
            if (!upload.videoUrl.empty()) {
                message += L" " + ToWide(upload.videoUrl);
            }
            PostStatus(ctx, message);
            ctx->youtubeBusy.store(false);
            RequestYouTubeUiRefresh(ctx);
        }).detach();
        break;
    }
    case IDC_ABOUT_WEBSITE_BUTTON: {
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", L"https://andrew.gg/bean", nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open website.");
        }
        break;
    }
    case IDC_ABOUT_EMAIL_BUTTON: {
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", L"mailto:goatrope@gmail.com", nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open default email app.");
        }
        break;
    }
    case IDC_ABOUT_DISCORD_BUTTON: {
        const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", L"https://discord.gg/57JGRw6x3D", nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatus(ctx, L"Failed to open Discord support invite.");
        }
        break;
    }
    case IDC_ABOUT_CHECK_UPDATES_BUTTON: {
        const HWND updateButton = GetDlgItem(ctx->aboutPanel, IDC_ABOUT_CHECK_UPDATES_BUTTON);
        const std::wstring buttonText = updateButton ? GetWindowTextString(updateButton) : std::wstring();
        if (buttonText != L"Update now") {
            RefreshAboutUpdateButtonState(ctx);
            break;
        }

        std::wstring updateStatus;
        const auto result = bean::app::ApplyUpdate(updateStatus);
        SetStatus(ctx, updateStatus);
        if (result == bean::app::UpdateApplyResult::UpdateReadyAndExitRequested) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else {
            RefreshAboutUpdateButtonState(ctx);
        }
        break;
    }
    case IDC_MONITOR_START: {
        PullSettingsFromUi(ctx);
        ctx->orchestrator->ApplySettings(ctx->settings);
        std::string error;
        ctx->orchestrator->StartMonitoring(error);
        break;
    }
    case IDC_MONITOR_STOP:
        ctx->orchestrator->StopMonitoring();
        break;
    case IDC_RECORD_START: {
        PullSettingsFromUi(ctx);
        std::string prepareError;
        if (!EnsureOutputDirectoryReady(ctx->settings.outputDirectory, prepareError)) {
            SetStatus(ctx, std::wstring(L"Recording start failed: ") + ToWide(prepareError));
            break;
        }
        RefreshFolderAvailability(ctx);
        ctx->orchestrator->ApplySettings(ctx->settings);
        std::string error;
        ctx->orchestrator->StartManualRecording(error);
        break;
    }
    case IDC_RECORD_STOP: {
        std::string error;
        ctx->orchestrator->StopManualRecording(error);
        break;
    }
    default:
        break;
    }

    RefreshLiveStatus(ctx);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    AppContext* ctx = reinterpret_cast<AppContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* appCtx = reinterpret_cast<AppContext*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(appCtx));
        ctx = appCtx;
        // Main-window controls are configured during WM_CREATE, so store it immediately.
        ctx->mainWindow = hwnd;

        const int navWidth = 120;
        const int navX = 12;
        const int panelX = navX + navWidth + 18;
        const int panelY = 16;
        const int panelWidth = 780;
        const int panelHeight = 460;
        const int labelWidth = 120;
        const int editWidth = 360;
        const int buttonWidth = 100;
        const int rowHeight = 24;
        const int rowSpacing = 40;
        const int sectionSpacing = 48;
        const int xLabel = 20;
        const int xEdit = 150;
        const int xButton = 520;
        const int xStatus = 630;
        int y = 20;

        ctx->statusTabButton = CreateWindowW(L"BUTTON", L"Status", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 20, navWidth, 32, hwnd, reinterpret_cast<HMENU>(IDC_TAB_STATUS), nullptr, nullptr);
        ctx->configurationTabButton = CreateWindowW(L"BUTTON", L"Config", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 58, navWidth, 32, hwnd, reinterpret_cast<HMENU>(IDC_TAB_CONFIGURATION), nullptr, nullptr);
        ctx->chatPrivacyTabButton = CreateWindowW(L"BUTTON", L"Chat Blocker", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 96, navWidth, 32, hwnd, reinterpret_cast<HMENU>(IDC_TAB_CHAT_PRIVACY), nullptr, nullptr);
        ctx->recordingsTabButton = CreateWindowW(L"BUTTON", L"Recordings", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 134, navWidth, 32, hwnd, reinterpret_cast<HMENU>(IDC_TAB_RECORDINGS), nullptr, nullptr);
        ctx->clipsTabButton = CreateWindowW(L"BUTTON", L"Clips", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 172, navWidth, 32, hwnd, reinterpret_cast<HMENU>(IDC_TAB_CLIPS), nullptr, nullptr);
        ctx->aboutTabButton = CreateWindowW(L"BUTTON", L"About", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 210, navWidth, 32, hwnd, reinterpret_cast<HMENU>(IDC_TAB_ABOUT), nullptr, nullptr);

        EnsureThemeResources();
        ctx->statusPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_VISIBLE | WS_CHILD, panelX, panelY, panelWidth, panelHeight, hwnd, nullptr, nullptr, nullptr);
        ctx->recorderPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, hwnd, nullptr, nullptr, nullptr);
        ctx->chatPrivacyPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, hwnd, nullptr, nullptr, nullptr);
        ctx->recordingsPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, hwnd, nullptr, nullptr, nullptr);
        ctx->clipsPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, hwnd, nullptr, nullptr, nullptr);
        ctx->aboutPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, hwnd, nullptr, nullptr, nullptr);
        SetWindowSubclass(ctx->statusPanel, PanelMessageForwarder, 1, 0);
        SetWindowSubclass(ctx->recorderPanel, PanelMessageForwarder, 2, 0);
        SetWindowSubclass(ctx->chatPrivacyPanel, PanelMessageForwarder, 3, 0);
        SetWindowSubclass(ctx->recordingsPanel, PanelMessageForwarder, 4, 0);
        SetWindowSubclass(ctx->clipsPanel, PanelMessageForwarder, 5, 0);
        SetWindowSubclass(ctx->aboutPanel, PanelMessageForwarder, 6, 0);

        CreateWindowW(L"STATIC", L"Output Folder:", WS_VISIBLE | WS_CHILD, xLabel, y, labelWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_OUTPUT_LABEL), nullptr, nullptr);
        ctx->outputEdit = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, xEdit, y, editWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_OUTPUT_EDIT), nullptr, nullptr);
        EnableCtrlASelectAll(ctx->outputEdit);
        CreateWindowW(L"BUTTON", L"Browse", WS_VISIBLE | WS_CHILD | WS_TABSTOP, xButton, y, buttonWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_OUTPUT_BROWSE), nullptr, nullptr);
        ctx->outputStatus = CreateWindowW(L"STATIC", L"X", WS_VISIBLE | WS_CHILD | SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE, xStatus, y, 40, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_OUTPUT_STATUS), nullptr, nullptr);
        y += rowSpacing;

        CreateWindowW(L"STATIC", L"WoW Log Folder:", WS_VISIBLE | WS_CHILD, xLabel, y, labelWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_LOG_LABEL), nullptr, nullptr);
        ctx->wowLogEdit = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, xEdit, y, editWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_LOG_EDIT), nullptr, nullptr);
        EnableCtrlASelectAll(ctx->wowLogEdit);
        CreateWindowW(L"BUTTON", L"Browse", WS_VISIBLE | WS_CHILD | WS_TABSTOP, xButton, y, buttonWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_LOG_BROWSE), nullptr, nullptr);
        ctx->wowLogStatus = CreateWindowW(L"STATIC", L"X", WS_VISIBLE | WS_CHILD | SS_CENTER | SS_CENTERIMAGE, xStatus, y, 40, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_LOG_STATUS), nullptr, nullptr);
        y += rowSpacing;

        CreateWindowW(L"STATIC", L"Video Encoder:", WS_VISIBLE | WS_CHILD, xLabel, y, labelWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_ENCODER_LABEL), nullptr, nullptr);
        ctx->encoderCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | WS_TABSTOP, xEdit, y, 230, 140, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_ENCODER_COMBO), nullptr, nullptr);
        SendMessageW(ctx->encoderCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GPU (Auto)"));
        SendMessageW(ctx->encoderCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"NVIDIA NVENC"));
        SendMessageW(ctx->encoderCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"AMD AMF"));
        SendMessageW(ctx->encoderCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Intel Quick Sync (QSV)"));
        SendMessageW(ctx->encoderCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"CPU x264"));
        y += rowSpacing;

        CreateWindowW(L"STATIC", L"Video Quality:", WS_VISIBLE | WS_CHILD, xLabel, y, labelWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_PRESET_LABEL), nullptr, nullptr);
        ctx->presetCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | WS_TABSTOP, xEdit, y, 180, 180, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_PRESET_COMBO), nullptr, nullptr);
        SendMessageW(ctx->presetCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ultra"));
        SendMessageW(ctx->presetCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"High"));
        SendMessageW(ctx->presetCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Medium"));
        SendMessageW(ctx->presetCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Low"));
        SendMessageW(ctx->presetCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Minimum"));
        ctx->presetHelpIcon = CreateWindowW(
            L"STATIC",
            L"",
            WS_VISIBLE | WS_CHILD | SS_OWNERDRAW | SS_NOTIFY,
            xEdit + 186,
            y + 4,
            16,
            16,
            ctx->recorderPanel,
            reinterpret_cast<HMENU>(IDC_PRESET_HELP),
            nullptr,
            nullptr);
        SetWindowSubclass(ctx->presetHelpIcon, HoverTooltipSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        CreateWindowW(L"STATIC", L"Container:", WS_VISIBLE | WS_CHILD, xLabel + 260, y, 80, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_CONTAINER_LABEL), nullptr, nullptr);
        ctx->containerCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | WS_TABSTOP, xLabel + 346, y, 120, 120, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_CONTAINER_COMBO), nullptr, nullptr);
        SendMessageW(ctx->containerCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"mkv"));
        SendMessageW(ctx->containerCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"mp4"));
        y += rowSpacing;

        CreateWindowW(L"STATIC", L"Audio capture:", WS_VISIBLE | WS_CHILD, xLabel, y, labelWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_AUDIO_SCOPE_LABEL), nullptr, nullptr);
        ctx->audioScopeCheck = CreateWindowW(
            L"BUTTON",
            L"WoW only",
            WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
            xEdit,
            y,
            150,
            rowHeight,
            ctx->recorderPanel,
            reinterpret_cast<HMENU>(IDC_AUDIO_SCOPE_CHECK),
            nullptr,
            nullptr);
        ctx->audioScopeWowDiscordRadio = CreateWindowW(
            L"BUTTON",
            L"WoW + Discord",
            WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON | WS_TABSTOP,
            xEdit + 160,
            y,
            150,
            rowHeight,
            ctx->recorderPanel,
            reinterpret_cast<HMENU>(IDC_AUDIO_SCOPE_WOW_DISCORD_RADIO),
            nullptr,
            nullptr);
        ctx->audioScopeAllRadio = CreateWindowW(
            L"BUTTON",
            L"All desktop audio",
            WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON | WS_TABSTOP,
            xEdit + 320,
            y,
            160,
            rowHeight,
            ctx->recorderPanel,
            reinterpret_cast<HMENU>(IDC_AUDIO_SCOPE_ALL_RADIO),
            nullptr,
            nullptr);
        SendMessageW(ctx->audioScopeCheck, BM_SETCHECK, BST_CHECKED, 0);
        y += rowSpacing;

        ctx->microphoneCheck = CreateWindowW(
            L"BUTTON",
            L"Record local microphone",
            WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
            xEdit,
            y,
            210,
            rowHeight,
            ctx->recorderPanel,
            reinterpret_cast<HMENU>(IDC_MICROPHONE_CHECK),
            nullptr,
            nullptr);
        ctx->microphoneNoiseSuppressionCheck = CreateWindowW(
            L"BUTTON",
            L"Noise suppression",
            WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
            xEdit + 216,
            y,
            180,
            rowHeight,
            ctx->recorderPanel,
            reinterpret_cast<HMENU>(IDC_MICROPHONE_NOISE_SUPPRESSION_CHECK),
            nullptr,
            nullptr);
        y += rowSpacing;
        ctx->microphoneCombo = CreateWindowW(
            L"COMBOBOX",
            L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | WS_TABSTOP,
            xEdit,
            y,
            486,
            180,
            ctx->recorderPanel,
            reinterpret_cast<HMENU>(IDC_MICROPHONE_COMBO),
            nullptr,
            nullptr);
        y += rowSpacing;

        CreateWindowW(L"STATIC", L"Width:", WS_VISIBLE | WS_CHILD, xLabel, y, 60, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_WIDTH_LABEL), nullptr, nullptr);
        ctx->widthEdit = CreateWindowW(L"EDIT", L"1920", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER | WS_TABSTOP, xLabel + 60, y, 80, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_WIDTH_EDIT), nullptr, nullptr);
        EnableCtrlASelectAll(ctx->widthEdit);
        CreateWindowW(L"STATIC", L"Height:", WS_VISIBLE | WS_CHILD, xLabel + 160, y, 60, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_HEIGHT_LABEL), nullptr, nullptr);
        ctx->heightEdit = CreateWindowW(L"EDIT", L"1080", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER | WS_TABSTOP, xLabel + 220, y, 80, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_HEIGHT_EDIT), nullptr, nullptr);
        EnableCtrlASelectAll(ctx->heightEdit);
        CreateWindowW(L"STATIC", L"FPS:", WS_VISIBLE | WS_CHILD, xLabel + 320, y, 40, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_FPS_LABEL), nullptr, nullptr);
        ctx->fpsEdit = CreateWindowW(L"EDIT", L"60", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER | WS_TABSTOP, xLabel + 360, y, 60, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_FPS_EDIT), nullptr, nullptr);
        EnableCtrlASelectAll(ctx->fpsEdit);
        y += rowSpacing;

        CreateWindowW(L"STATIC", L"Post-run tail (s):", WS_VISIBLE | WS_CHILD, xLabel, y, 104, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_POST_RUN_DELAY_LABEL), nullptr, nullptr);
        ctx->postRunDelayHelpIcon = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW | SS_NOTIFY, xLabel + 104, y + 4, 16, 16, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_POST_RUN_DELAY_HELP), nullptr, nullptr);
        ctx->postRunDelayEdit = CreateWindowW(L"EDIT", L"30", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER | WS_TABSTOP, xLabel + 120, y, 70, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_POST_RUN_DELAY_EDIT), nullptr, nullptr);
        EnableCtrlASelectAll(ctx->postRunDelayEdit);
        SetWindowSubclass(ctx->postRunDelayHelpIcon, HoverTooltipSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        SetWindowSubclass(ctx->outputStatus, HoverTooltipSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        y += sectionSpacing;
        CreateWindowW(
            L"STATIC",
            L"Settings auto-save as you make changes.",
            WS_VISIBLE | WS_CHILD | SS_CENTER,
            xLabel,
            y,
            panelWidth - (xLabel * 2),
            rowHeight,
            ctx->recorderPanel,
            reinterpret_cast<HMENU>(IDC_CONFIGURATION_AUTOSAVE_HINT),
            nullptr,
            nullptr);
        y += rowSpacing;

        CreateWindowW(L"BUTTON", L"Start Monitoring", WS_VISIBLE | WS_CHILD, xLabel, y, 140, rowHeight + 4, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_MONITOR_START), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Stop Monitoring", WS_VISIBLE | WS_CHILD, xLabel + 150, y, 140, rowHeight + 4, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_MONITOR_STOP), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Start Recording", WS_VISIBLE | WS_CHILD, xLabel + 320, y, 140, rowHeight + 4, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_RECORD_START), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Stop Recording", WS_VISIBLE | WS_CHILD, xLabel + 470, y, 120, rowHeight + 4, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_RECORD_STOP), nullptr, nullptr);
        y += 44;

        CreateWindowW(L"STATIC", L"Live Status:", WS_VISIBLE | WS_CHILD, xLabel, y, 90, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_LIVE_LABEL), nullptr, nullptr);
        ctx->monitorIcon = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, xLabel + 95, y, 18, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_MONITOR_ICON), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Monitoring", WS_VISIBLE | WS_CHILD, xLabel + 115, y, 80, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_MONITOR_TEXT), nullptr, nullptr);
        ctx->recordIcon = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, xLabel + 215, y, 18, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_RECORD_ICON), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Recording", WS_VISIBLE | WS_CHILD, xLabel + 235, y, 70, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_RECORD_TEXT), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Length:", WS_VISIBLE | WS_CHILD, xLabel + 330, y, 50, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_LENGTH_LABEL), nullptr, nullptr);
        ctx->lengthValue = CreateWindowW(L"STATIC", L"00:00:00", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, xLabel + 385, y, 85, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_LENGTH_VALUE), nullptr, nullptr);
        y += 34;

        CreateWindowW(L"STATIC", L"WoW Window:", WS_VISIBLE | WS_CHILD, xLabel, y, 90, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_WOW_WINDOW_LABEL), nullptr, nullptr);
        ctx->wowWindowIcon = CreateWindowW(L"STATIC", L"X", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, xLabel + 95, y, 20, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_WOW_WINDOW_ICON), nullptr, nullptr);
        ctx->wowWindowText = CreateWindowW(L"STATIC", L"WoW window not detected", WS_VISIBLE | WS_CHILD, xLabel + 118, y, 220, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_WOW_WINDOW_TEXT), nullptr, nullptr);
        y += 36;

        CreateWindowW(L"STATIC", L"OBS Install:", WS_VISIBLE | WS_CHILD, xLabel, y, 90, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_OBS_INSTALL_LABEL), nullptr, nullptr);
        ctx->obsInstallIcon = CreateWindowW(L"STATIC", L"X", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, xLabel + 95, y, 20, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_OBS_INSTALL_ICON), nullptr, nullptr);
        ctx->obsInstallText = CreateWindowW(L"STATIC", L"OBS install not detected", WS_VISIBLE | WS_CHILD, xLabel + 118, y, 220, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_OBS_INSTALL_TEXT), nullptr, nullptr);
        y += 36;

        CreateWindowW(L"STATIC", L"FFmpeg:", WS_VISIBLE | WS_CHILD, xLabel, y, 90, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_FFMPEG_LABEL), nullptr, nullptr);
        ctx->ffmpegIcon = CreateWindowW(L"STATIC", L"X", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, xLabel + 95, y, 20, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_FFMPEG_ICON), nullptr, nullptr);
        ctx->ffmpegText = CreateWindowW(L"STATIC", L"FFmpeg not found for trim", WS_VISIBLE | WS_CHILD, xLabel + 118, y, 240, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_FFMPEG_TEXT), nullptr, nullptr);
        y += 36;

        CreateWindowW(
            L"STATIC",
            L"WCR Conflict:",
            WS_VISIBLE | WS_CHILD,
            xLabel,
            y,
            126,
            rowHeight,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_WARCRAFT_RECORDER_LABEL),
            nullptr,
            nullptr);
        HWND warcraftRecorderLabel = GetDlgItem(ctx->statusPanel, IDC_WARCRAFT_RECORDER_LABEL);
        ctx->warcraftRecorderIcon = CreateWindowW(
            L"STATIC",
            L"X",
            WS_VISIBLE | WS_CHILD | SS_OWNERDRAW,
            xLabel + 128,
            y,
            20,
            rowHeight,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_WARCRAFT_RECORDER_ICON),
            nullptr,
            nullptr);
        ctx->warcraftRecorderText = CreateWindowW(
            L"STATIC",
            L"Not detected",
            WS_VISIBLE | WS_CHILD,
            xLabel + 152,
            y,
            380,
            rowHeight,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_WARCRAFT_RECORDER_TEXT),
            nullptr,
            nullptr);
        if (warcraftRecorderLabel) {
            ShowWindow(warcraftRecorderLabel, SW_HIDE);
        }
        ShowWindow(ctx->warcraftRecorderIcon, SW_HIDE);
        ShowWindow(ctx->warcraftRecorderText, SW_HIDE);
        y += 36;

        CreateWindowW(L"STATIC", L"Advanced Logging:", WS_VISIBLE | WS_CHILD, xLabel, y, 120, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_ADVANCED_LOGGING_LABEL), nullptr, nullptr);
        ctx->advancedLoggingHelpIcon = CreateWindowW(
            L"STATIC",
            L"",
            WS_VISIBLE | WS_CHILD | SS_OWNERDRAW | SS_NOTIFY,
            xLabel + 282,
            y + 4,
            16,
            16,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_ADVANCED_LOGGING_HELP),
            nullptr,
            nullptr);
        ctx->advancedLoggingIcon = CreateWindowW(L"STATIC", L"X", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, xLabel + 162, y, 20, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_ADVANCED_LOGGING_ICON), nullptr, nullptr);
        ctx->advancedLoggingText = CreateWindowW(
            L"STATIC",
            L"Advanced Combat Logging disabled",
            WS_VISIBLE | WS_CHILD,
            xLabel + 186,
            y,
            260,
            rowHeight,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_ADVANCED_LOGGING_TEXT),
            nullptr,
            nullptr);
        SetWindowSubclass(ctx->advancedLoggingHelpIcon, HoverTooltipSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        y += 36;

        ctx->chatBlockerEnabledCheck = CreateWindowW(
            L"BUTTON",
            L"Enable Chat Blocker",
            WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
            20,
            20,
            240,
            rowHeight,
            ctx->chatPrivacyPanel,
            reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_ENABLED_CHECK),
            nullptr,
            nullptr);
        CreateWindowW(L"STATIC", L"Image:", WS_VISIBLE | WS_CHILD, 20, 52, 96, rowHeight, ctx->chatPrivacyPanel, nullptr, nullptr, nullptr);
        ctx->chatBlockerImageBlankRadio = CreateWindowW(
            L"BUTTON",
            L"Blank",
            WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON | WS_TABSTOP,
            124,
            52,
            104,
            rowHeight,
            ctx->chatPrivacyPanel,
            reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_IMAGE_BLANK_RADIO),
            nullptr,
            nullptr);
        ctx->chatBlockerImageCustomRadio = CreateWindowW(
            L"BUTTON",
            L"Custom",
            WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON | WS_TABSTOP,
            234,
            52,
            104,
            rowHeight,
            ctx->chatPrivacyPanel,
            reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_IMAGE_CUSTOM_RADIO),
            nullptr,
            nullptr);
        SendMessageW(ctx->chatBlockerImageBlankRadio, BM_SETCHECK, BST_CHECKED, 0);
        CreateWindowW(
            L"STATIC",
            L"Library:",
            WS_VISIBLE | WS_CHILD,
            20,
            88,
            96,
            rowHeight,
            ctx->chatPrivacyPanel,
            reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_IMAGE_LIBRARY_LABEL),
            nullptr,
            nullptr);
        ctx->chatBlockerImageImportButton = CreateWindowW(
            L"BUTTON",
            L"Import",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP,
            344,
            52,
            78,
            rowHeight + 2,
            ctx->chatPrivacyPanel,
            reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_IMAGE_IMPORT_BUTTON),
            nullptr,
            nullptr);
        ctx->chatBlockerImageOpenFolderButton = CreateWindowW(
            L"BUTTON",
            L"Open Folder",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP,
            428,
            52,
            108,
            rowHeight + 2,
            ctx->chatPrivacyPanel,
            reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_IMAGE_OPEN_FOLDER_BUTTON),
            nullptr,
            nullptr);
        ctx->chatBlockerImageCombo = CreateWindowW(
            L"COMBOBOX",
            L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | WS_TABSTOP,
            134,
            88,
            464,
            180,
            ctx->chatPrivacyPanel,
            reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_IMAGE_COMBO),
            nullptr,
            nullptr);
        CreateWindowW(L"STATIC", L"Width:", WS_VISIBLE | WS_CHILD, 20, 122, 120, rowHeight, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_WIDTH_LABEL), nullptr, nullptr);
        ctx->chatBlockerWidthEdit = CreateWindowW(L"EDIT", L"500", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER | WS_TABSTOP, 134, 122, 90, rowHeight, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_WIDTH_EDIT), nullptr, nullptr);
        EnableCtrlASelectAll(ctx->chatBlockerWidthEdit);
        CreateWindowW(L"STATIC", L"Height:", WS_VISIBLE | WS_CHILD, 238, 122, 120, rowHeight, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_HEIGHT_LABEL), nullptr, nullptr);
        ctx->chatBlockerHeightEdit = CreateWindowW(L"EDIT", L"300", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER | WS_TABSTOP, 352, 122, 90, rowHeight, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_HEIGHT_EDIT), nullptr, nullptr);
        EnableCtrlASelectAll(ctx->chatBlockerHeightEdit);
        CreateWindowW(L"STATIC", L"Anchor Corner:", WS_VISIBLE | WS_CHILD, 20, 156, 110, rowHeight, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_ANCHOR_LABEL), nullptr, nullptr);
        ctx->chatBlockerAnchorCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | WS_TABSTOP, 134, 156, 180, 120, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_ANCHOR_COMBO), nullptr, nullptr);
        SendMessageW(ctx->chatBlockerAnchorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom Left"));
        SendMessageW(ctx->chatBlockerAnchorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom Right"));
        SendMessageW(ctx->chatBlockerAnchorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top Left"));
        SendMessageW(ctx->chatBlockerAnchorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top Right"));
        SendMessageW(ctx->chatBlockerAnchorCombo, CB_SETCURSEL, 0, 0);
        CreateWindowW(L"STATIC", L"WoW Live Preview (overlay area marks chat blocker):", WS_VISIBLE | WS_CHILD, 20, 194, 720, rowHeight, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_PREVIEW_LABEL), nullptr, nullptr);
        ctx->chatPreview = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | SS_OWNERDRAW, 20, 222, 740, 222, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_PREVIEW), nullptr, nullptr);
        ctx->chatPreviewStatus = CreateWindowW(L"STATIC", L"WoW not detected; preview uses a placeholder.", WS_VISIBLE | WS_CHILD, 20, 454, 740, rowHeight, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_PREVIEW_STATUS), nullptr, nullptr);
        RefreshChatBlockerImageCombo(ctx, {});
        RefreshChatBlockerImageControls(ctx);

        CreateWindowW(L"STATIC", L"Status:", WS_VISIBLE | WS_CHILD, xLabel, y, 60, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_STATUS_LABEL), nullptr, nullptr);
        ctx->statusText = CreateWindowW(
            L"EDIT",
            L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            xEdit,
            y,
            470,
            rowHeight + 40,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_STATUS_TEXT),
            nullptr,
            nullptr);
        EnableCtrlASelectAll(ctx->statusText);
        CreateWindowW(
            L"BUTTON",
            L"Open Status Log Folder",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP,
            520,
            y + rowHeight + 44,
            200,
            rowHeight + 4,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_STATUS_OPEN_LOG_FOLDER),
            nullptr,
            nullptr);

        ctx->recordingsLabel = CreateWindowW(L"STATIC", L"Folder:", WS_VISIBLE | WS_CHILD, 20, 20, 740, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_LABEL), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Refresh", WS_VISIBLE | WS_CHILD, 20, 52, 100, rowHeight + 4, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_REFRESH), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open Folder", WS_VISIBLE | WS_CHILD, 130, 52, 120, rowHeight + 4, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_OPEN_FOLDER), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open DB Folder", WS_VISIBLE | WS_CHILD, 260, 52, 130, rowHeight + 4, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_OPEN_DB_FOLDER), nullptr, nullptr);
        ctx->recordingsList = CreateWindowW(WC_LISTVIEWW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 20, 90, 500, 220, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_LIST), nullptr, nullptr);
        SetWindowTheme(ctx->recordingsList, L"", L"");
        ListView_SetExtendedListViewStyle(ctx->recordingsList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(ctx->recordingsList, kColorListRow);
        ListView_SetTextBkColor(ctx->recordingsList, kColorListRow);
        ListView_SetTextColor(ctx->recordingsList, kColorTextPrimary);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        column.cx = 240;
        column.fmt = LVCFMT_LEFT;
        column.pszText = const_cast<wchar_t*>(L"Dungeon");
        SendMessageW(ctx->recordingsList, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
        column.cx = 64;
        column.fmt = LVCFMT_CENTER;
        column.pszText = const_cast<wchar_t*>(L"Level");
        SendMessageW(ctx->recordingsList, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
        column.cx = 110;
        column.fmt = LVCFMT_CENTER;
        column.pszText = const_cast<wchar_t*>(L"Duration");
        SendMessageW(ctx->recordingsList, LVM_INSERTCOLUMNW, 2, reinterpret_cast<LPARAM>(&column));
        column.cx = 128;
        column.fmt = LVCFMT_CENTER;
        column.pszText = const_cast<wchar_t*>(L"Date");
        SendMessageW(ctx->recordingsList, LVM_INSERTCOLUMNW, 3, reinterpret_cast<LPARAM>(&column));
        ctx->recordingsListHeader = ListView_GetHeader(ctx->recordingsList);
        if (ctx->recordingsListHeader) {
            SetWindowTheme(ctx->recordingsListHeader, L"", L"");
            SetWindowSubclass(ctx->recordingsListHeader, RecordingsHeaderSubclassProc, 2, reinterpret_cast<DWORD_PTR>(ctx));
            InvalidateRect(ctx->recordingsListHeader, nullptr, TRUE);
        }
        ctx->recordingsInfoLabel = CreateWindowW(L"STATIC", L"Characters", WS_VISIBLE | WS_CHILD, 532, 90, 228, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_INFO_LABEL), nullptr, nullptr);
        ctx->recordingsInfoText = CreateWindowW(WC_LISTVIEWW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER, 532, 114, 228, 196, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_INFO_TEXT), nullptr, nullptr);
        SetWindowTheme(ctx->recordingsInfoText, L"", L"");
        ListView_SetExtendedListViewStyle(ctx->recordingsInfoText, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        EnsureParticipantSpecIconList(ctx);
        ListView_SetBkColor(ctx->recordingsInfoText, kColorListRow);
        ListView_SetTextBkColor(ctx->recordingsInfoText, kColorListRow);
        ListView_SetTextColor(ctx->recordingsInfoText, kColorTextPrimary);
        LVCOLUMNW partyColumn{};
        partyColumn.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        partyColumn.cx = 220;
        partyColumn.fmt = LVCFMT_LEFT;
        partyColumn.pszText = const_cast<wchar_t*>(L"");
        SendMessageW(ctx->recordingsInfoText, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&partyColumn));

        ctx->youtubeLinkButton = CreateWindowW(L"BUTTON", L"Link YouTube", WS_VISIBLE | WS_CHILD, 540, 322, 110, rowHeight + 4, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_LINK_BUTTON), nullptr, nullptr);
        ctx->youtubeUnlinkButton = CreateWindowW(L"BUTTON", L"Unlink Account", WS_CHILD, 652, 322, 108, rowHeight + 4, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_UNLINK_BUTTON), nullptr, nullptr);
        ctx->youtubeUnlinkConfirmLabel = CreateWindowW(L"STATIC", L"You sure?", WS_CHILD, 540, 352, 100, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_UNLINK_CONFIRM_LABEL), nullptr, nullptr);
        ctx->youtubeUnlinkYesButton = CreateWindowW(L"BUTTON", L"Yes", WS_CHILD, 644, 350, 54, rowHeight + 4, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_UNLINK_YES_BUTTON), nullptr, nullptr);
        ctx->youtubeUnlinkNoButton = CreateWindowW(L"BUTTON", L"No", WS_CHILD, 702, 350, 54, rowHeight + 4, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_UNLINK_NO_BUTTON), nullptr, nullptr);
        ctx->youtubeLinkStatus = CreateWindowW(L"STATIC", L"Not linked", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, 540, 352, 24, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_LINK_STATUS), nullptr, nullptr);
        ctx->youtubeAccountLabel = CreateWindowW(L"STATIC", L"YouTube Account:", WS_VISIBLE | WS_CHILD, 20, 352, 500, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_ACCOUNT_LABEL), nullptr, nullptr);
        ctx->youtubeAccountLink = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 90, 348, 430, rowHeight + 8, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_ACCOUNT_LINK), nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Video Title:", WS_VISIBLE | WS_CHILD, 20, 392, 120, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_TITLE_LABEL), nullptr, nullptr);
        ctx->youtubeTitleEdit = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 150, 392, 370, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_TITLE_EDIT), nullptr, nullptr);
        EnableCtrlASelectAll(ctx->youtubeTitleEdit);
        CreateWindowW(L"STATIC", L"Visibility:", WS_VISIBLE | WS_CHILD, 540, 392, 70, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_PRIVACY_LABEL), nullptr, nullptr);
        ctx->youtubePrivacyCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST, 612, 392, 148, 120, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_PRIVACY_COMBO), nullptr, nullptr);
        SendMessageW(ctx->youtubePrivacyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"private"));
        SendMessageW(ctx->youtubePrivacyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"unlisted"));
        SendMessageW(ctx->youtubePrivacyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"public"));
        SendMessageW(ctx->youtubePrivacyCombo, CB_SETCURSEL, 0, 0);
        ctx->youtubeUploadButton = CreateWindowW(L"BUTTON", L"Upload to YouTube", WS_VISIBLE | WS_CHILD, 540, 424, 220, rowHeight + 4, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_UPLOAD_BUTTON), nullptr, nullptr);
        ctx->recordingsUploadProgress = CreateWindowW(PROGRESS_CLASSW, L"", WS_VISIBLE | WS_CHILD, 20, 426, 500, 20, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_UPLOAD_PROGRESS), nullptr, nullptr);
        SendMessageW(ctx->recordingsUploadProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(ctx->recordingsUploadProgress, PBM_SETPOS, 0, 0);
        ctx->recordingsUploadStatus = CreateWindowW(L"STATIC", L"No upload in progress.", WS_VISIBLE | WS_CHILD, 20, 448, 740, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_UPLOAD_STATUS), nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Source Recording:", WS_VISIBLE | WS_CHILD, 20, 20, 110, rowHeight, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_SOURCE_LABEL), nullptr, nullptr);
        ctx->clipsSourceCombo = CreateWindowW(
            L"COMBOBOX",
            L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
            134,
            20,
            520,
            360,
            ctx->clipsPanel,
            reinterpret_cast<HMENU>(IDC_CLIPS_SOURCE_COMBO),
            nullptr,
            nullptr);
        CreateWindowW(L"BUTTON", L"Refresh", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 664, 19, 96, rowHeight + 4, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_REFRESH), nullptr, nullptr);
        ctx->clipsVideoSurface = CreateWindowW(
            L"STATIC",
            L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER,
            20,
            56,
            740,
            264,
            ctx->clipsPanel,
            reinterpret_cast<HMENU>(IDC_CLIPS_VIDEO_SURFACE),
            nullptr,
            nullptr);
        ctx->clipsPlayPauseButton = CreateWindowW(
            L"BUTTON",
            L"Play",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP,
            20,
            330,
            90,
            rowHeight + 4,
            ctx->clipsPanel,
            reinterpret_cast<HMENU>(IDC_CLIPS_PLAY_PAUSE),
            nullptr,
            nullptr);
        ctx->clipsTimeline = CreateWindowW(
            L"STATIC",
            L"",
            WS_VISIBLE | WS_CHILD | SS_OWNERDRAW | SS_NOTIFY,
            116,
            332,
            496,
            rowHeight,
            ctx->clipsPanel,
            reinterpret_cast<HMENU>(IDC_CLIPS_TIMELINE),
            nullptr,
            nullptr);
        if (ctx->clipsTimeline) {
            SetWindowSubclass(ctx->clipsTimeline, ClipsSliderSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        }
        ctx->clipsPositionText = CreateWindowW(
            L"STATIC",
            L"00:00:00 / 00:00:00",
            WS_VISIBLE | WS_CHILD | SS_LEFTNOWORDWRAP,
            620,
            332,
            228,
            rowHeight,
            ctx->clipsPanel,
            reinterpret_cast<HMENU>(IDC_CLIPS_POSITION_TEXT),
            nullptr,
            nullptr);
        CreateWindowW(L"STATIC", L"Volume:", WS_VISIBLE | WS_CHILD, 20, 364, 60, rowHeight, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_VOLUME_LABEL), nullptr, nullptr);
        ctx->clipsVolumeSlider = CreateWindowW(
            L"STATIC",
            L"",
            WS_VISIBLE | WS_CHILD | SS_OWNERDRAW | SS_NOTIFY,
            84,
            364,
            168,
            rowHeight,
            ctx->clipsPanel,
            reinterpret_cast<HMENU>(IDC_CLIPS_VOLUME_SLIDER),
            nullptr,
            nullptr);
        if (ctx->clipsVolumeSlider) {
            SetWindowSubclass(ctx->clipsVolumeSlider, ClipsSliderSubclassProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        }
        CreateWindowW(L"STATIC", L"Start (s):", WS_VISIBLE | WS_CHILD, 20, 398, 80, rowHeight, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_START_LABEL), nullptr, nullptr);
        ctx->clipsStartEdit = CreateWindowW(
            L"EDIT",
            L"0",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER | WS_TABSTOP,
            104,
            398,
            74,
            rowHeight,
            ctx->clipsPanel,
            reinterpret_cast<HMENU>(IDC_CLIPS_START_EDIT),
            nullptr,
            nullptr);
        EnableCtrlASelectAll(ctx->clipsStartEdit);
        CreateWindowW(L"BUTTON", L"Set Start", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 182, 397, 94, rowHeight + 4, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_SET_START), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"End (s):", WS_VISIBLE | WS_CHILD, 288, 398, 70, rowHeight, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_END_LABEL), nullptr, nullptr);
        ctx->clipsEndEdit = CreateWindowW(
            L"EDIT",
            L"0",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER | WS_TABSTOP,
            360,
            398,
            74,
            rowHeight,
            ctx->clipsPanel,
            reinterpret_cast<HMENU>(IDC_CLIPS_END_EDIT),
            nullptr,
            nullptr);
        EnableCtrlASelectAll(ctx->clipsEndEdit);
        CreateWindowW(L"BUTTON", L"Set End", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 438, 397, 94, rowHeight + 4, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_SET_END), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Export Clip", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 20, 431, 112, rowHeight + 4, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_EXPORT), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open Folder", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 140, 431, 112, rowHeight + 4, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_OPEN_FOLDER), nullptr, nullptr);
        ctx->clipsFfmpegWarning = CreateWindowW(
            L"STATIC",
            L"FFmpeg is required to export clips.",
            WS_CHILD | SS_LEFT,
            260,
            431,
            500,
            rowHeight + 4,
            ctx->clipsPanel,
            reinterpret_cast<HMENU>(IDC_CLIPS_FFMPEG_WARNING),
            nullptr,
            nullptr);
        ctx->clipsTimelinePosition = 0;
        ctx->clipsVolumePercent = 100;
        RefreshClipsPlaybackControls(ctx);

        const std::wstring versionText = VersionText();
        CreateWindowW(L"STATIC", kAboutTitleText, WS_VISIBLE | WS_CHILD | SS_CENTER, 20, 24, 740, 28, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_TITLE_LABEL), nullptr, nullptr);
        CreateWindowW(L"STATIC", versionText.c_str(), WS_VISIBLE | WS_CHILD | SS_CENTER, 20, 58, 740, rowHeight, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_BUILD_TEXT), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Website:", WS_VISIBLE | WS_CHILD, 20, 96, 120, rowHeight, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_WEBSITE_LABEL), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"https://andrew.gg/bean", WS_VISIBLE | WS_CHILD, 150, 96, 360, rowHeight, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_WEBSITE_TEXT), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open Website", WS_VISIBLE | WS_CHILD, 540, 94, 150, rowHeight + 4, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_WEBSITE_BUTTON), nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Email:", WS_VISIBLE | WS_CHILD, 20, 134, 120, rowHeight, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_EMAIL_LABEL), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"goatrope@gmail.com", WS_VISIBLE | WS_CHILD, 150, 134, 360, rowHeight, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_EMAIL_TEXT), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Send Email", WS_VISIBLE | WS_CHILD, 540, 132, 150, rowHeight + 4, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_EMAIL_BUTTON), nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Discord:", WS_VISIBLE | WS_CHILD, 20, 172, 120, rowHeight, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_DISCORD_LABEL), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"https://discord.gg/57JGRw6x3D", WS_VISIBLE | WS_CHILD, 150, 172, 360, rowHeight, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_DISCORD_TEXT), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Join Discord", WS_VISIBLE | WS_CHILD, 540, 170, 150, rowHeight + 4, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_DISCORD_BUTTON), nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Updates:", WS_VISIBLE | WS_CHILD, 20, 210, 120, rowHeight, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_UPDATE_LABEL), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Checking for updates...", WS_VISIBLE | WS_CHILD, 150, 210, 360, rowHeight, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_UPDATE_TEXT), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Check for updates", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 540, 208, 150, rowHeight + 4, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_CHECK_UPDATES_BUTTON), nullptr, nullptr);

        ConfigureStyledButtons(ctx);
        ApplyUiFonts(hwnd);
        ApplyRecordingsFonts(ctx);
        HWND autoSaveHint = GetDlgItem(ctx->recorderPanel, IDC_CONFIGURATION_AUTOSAVE_HINT);
        if (autoSaveHint && gTheme.mutedHintFont) {
            SendMessageW(autoSaveHint, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.mutedHintFont), TRUE);
        }
        if (ctx->outputStatus && gTheme.statusIndicatorFont) {
            SendMessageW(ctx->outputStatus, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.statusIndicatorFont), TRUE);
        }
        ConfigureConfigurationTooltips(ctx);
        if (gTheme.headingFont) {
            HWND titleLabel = GetDlgItem(ctx->aboutPanel, IDC_ABOUT_TITLE_LABEL);
            if (titleLabel) {
                SendMessageW(titleLabel, WM_SETFONT, reinterpret_cast<WPARAM>(gTheme.headingFont), TRUE);
            }
        }
        SendMessageW(ctx->recordingsUploadProgress, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(kColorListSelection));
        SendMessageW(ctx->recordingsUploadProgress, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(kColorInputBg));

        const std::wstring statusLogInitError = InitializeStatusLogFile(ctx);
        if (!statusLogInitError.empty()) {
            SetStatus(ctx, std::wstring(L"Status log unavailable: ") + statusLogInitError);
        }
        PushSettingsToUi(ctx);
        RefreshLiveStatus(ctx);
        SetActiveTab(ctx, AppContext::MainTab::Status);
        if (!ctx->settings.youtubeRefreshToken.empty() && ctx->settings.youtubeChannelId.empty() && ctx->settings.youtubeChannelTitle.empty()) {
            ResolveLinkedYouTubeIdentityAsync(ctx, false);
        }
        LogSessionDiagnostics(ctx);
        SetStatus(ctx, L"Ready.");
        PullSettingsFromUi(ctx);
        ctx->orchestrator->ApplySettings(ctx->settings);
        std::string autoStartError;
        ctx->orchestrator->StartMonitoring(autoStartError);
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        LayoutMainUi(ctx, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
        ctx->chatBlockerAutoSaveArmed = true;
        ctx->configurationAutoSaveArmed = true;
        SetTimer(hwnd, kLiveStatusTimerId, kLiveStatusIntervalMs, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && (LOWORD(wParam) == IDC_OUTPUT_EDIT || LOWORD(wParam) == IDC_LOG_EDIT)) {
            RefreshFolderAvailability(ctx);
            if (ctx) {
                AutoSaveConfigurationSettings(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == EN_CHANGE
            && (LOWORD(wParam) == IDC_WIDTH_EDIT
                || LOWORD(wParam) == IDC_HEIGHT_EDIT
                || LOWORD(wParam) == IDC_FPS_EDIT
                || LOWORD(wParam) == IDC_POST_RUN_DELAY_EDIT)) {
            if (ctx) {
                AutoSaveConfigurationSettings(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == EN_CHANGE
            && (LOWORD(wParam) == IDC_CLIPS_START_EDIT || LOWORD(wParam) == IDC_CLIPS_END_EDIT)) {
            if (ctx && ctx->clipsTimeline) {
                InvalidateControlAndParentRegion(ctx->clipsTimeline);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE
            && (LOWORD(wParam) == IDC_ENCODER_COMBO
                || LOWORD(wParam) == IDC_PRESET_COMBO
                || LOWORD(wParam) == IDC_CONTAINER_COMBO
                || LOWORD(wParam) == IDC_MICROPHONE_COMBO)) {
            if (ctx) {
                AutoSaveConfigurationSettings(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_CLIPS_SOURCE_COMBO) {
            HandleCommand(hwnd, ctx, LOWORD(wParam));
            return 0;
        }
        if (LOWORD(wParam) == IDC_CLIPS_SOURCE_COMBO) {
            // Ignore non-selection combo notifications (dropdown open/close/focus),
            // otherwise we can accidentally trigger clip load work while the user
            // is just interacting with the combo.
            return 0;
        }
        if (HIWORD(wParam) == CBN_DROPDOWN && LOWORD(wParam) == IDC_CHAT_BLOCKER_IMAGE_COMBO) {
            if (ctx) {
                RefreshChatBlockerImageCombo(ctx, {});
                RefreshChatBlockerImageControls(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_CHAT_BLOCKER_IMAGE_COMBO) {
            if (ctx && ctx->chatBlockerImageCombo
                && SendMessageW(ctx->chatBlockerImageCombo, CB_GETDROPPEDSTATE, 0, 0) != 0) {
                // Ignore hover/navigation changes while dropdown is open.
                return 0;
            }
            if (ctx) {
                SyncChatBlockerSelectionToImageMetadata(ctx, true);
                AutoSaveChatBlockerSettings(ctx);
            }
            if (ctx && ctx->chatPreview) {
                InvalidateRect(ctx->chatPreview, nullptr, FALSE);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_CLOSEUP && LOWORD(wParam) == IDC_CHAT_BLOCKER_IMAGE_COMBO) {
            if (ctx) {
                SyncChatBlockerSelectionToImageMetadata(ctx, true);
                AutoSaveChatBlockerSettings(ctx);
            }
            if (ctx && ctx->chatPreview) {
                InvalidateRect(ctx->chatPreview, nullptr, FALSE);
            }
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED
            && (LOWORD(wParam) == IDC_MICROPHONE_CHECK
                || LOWORD(wParam) == IDC_MICROPHONE_NOISE_SUPPRESSION_CHECK
                || LOWORD(wParam) == IDC_AUDIO_SCOPE_CHECK
                || LOWORD(wParam) == IDC_AUDIO_SCOPE_WOW_DISCORD_RADIO
                || LOWORD(wParam) == IDC_AUDIO_SCOPE_ALL_RADIO)) {
            HandleCommand(hwnd, ctx, LOWORD(wParam));
            if (ctx) {
                AutoSaveConfigurationSettings(ctx);
            }
            return 0;
        }
        if (HIWORD(wParam) == EN_CHANGE && (LOWORD(wParam) == IDC_CHAT_BLOCKER_WIDTH_EDIT || LOWORD(wParam) == IDC_CHAT_BLOCKER_HEIGHT_EDIT)) {
            if (ctx) {
                if (LOWORD(wParam) == IDC_CHAT_BLOCKER_WIDTH_EDIT && ctx->chatBlockerIgnoreNextWidthChange) {
                    ctx->chatBlockerIgnoreNextWidthChange = false;
                    return 0;
                }
                if (LOWORD(wParam) == IDC_CHAT_BLOCKER_HEIGHT_EDIT && ctx->chatBlockerIgnoreNextHeightChange) {
                    ctx->chatBlockerIgnoreNextHeightChange = false;
                    return 0;
                }
                ApplyChatBlockerAspectForEdit(ctx, LOWORD(wParam));
                AutoSaveChatBlockerSettings(ctx);
            }
            if (ctx && ctx->chatPreview) {
                InvalidateRect(ctx->chatPreview, nullptr, FALSE);
            }
            return 0;
        }
        HandleCommand(hwnd, ctx, LOWORD(wParam));
        return 0;
    case WM_HSCROLL: {
        if (!ctx) {
            break;
        }
        break;
    }
    case WM_NOTIFY: {
        if (!ctx) {
            break;
        }
        auto* hdr = reinterpret_cast<LPNMHDR>(lParam);
        if (!ctx->recordingsList) {
            break;
        }
        if (hdr->idFrom == IDC_RECORDINGS_INFO_TEXT && hdr->code == NM_CUSTOMDRAW) {
            auto* customDraw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            if (customDraw->nmcd.dwDrawStage == CDDS_PREPAINT) {
                return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
            }
            if (customDraw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                const size_t itemIndex = customDraw->nmcd.dwItemSpec;
                COLORREF participantColor = kColorTextPrimary;
                if (itemIndex < ctx->visibleParticipantRowColors.size()) {
                    participantColor = ctx->visibleParticipantRowColors[itemIndex];
                }
                customDraw->clrText = participantColor;
                customDraw->clrTextBk = ((itemIndex % 2) == 0) ? kColorListRow : kColorListRowAlt;
                return CDRF_NEWFONT;
            }
            if (customDraw->nmcd.dwDrawStage == CDDS_POSTPAINT) {
                return CDRF_DODEFAULT;
            }
        }
        if (hdr->idFrom == IDC_RECORDINGS_LIST && hdr->code == NM_CUSTOMDRAW) {
            auto* customDraw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            if (customDraw->nmcd.dwDrawStage == CDDS_PREPAINT) {
                return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
            }
            if (customDraw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                customDraw->clrText = kColorTextPrimary;
                customDraw->clrTextBk = (customDraw->nmcd.uItemState & CDIS_SELECTED)
                    ? kColorListSelection
                    : ((customDraw->nmcd.dwItemSpec % 2 == 0) ? kColorListRow : kColorListRowAlt);
                return CDRF_NOTIFYSUBITEMDRAW;
            }
            if (customDraw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                customDraw->clrText = kColorTextPrimary;
                customDraw->clrTextBk = (customDraw->nmcd.uItemState & CDIS_SELECTED)
                    ? kColorListSelection
                    : ((customDraw->nmcd.dwItemSpec % 2 == 0) ? kColorListRow : kColorListRowAlt);
                if ((customDraw->nmcd.uItemState & CDIS_SELECTED) == 0
                    && customDraw->iSubItem == 1
                    && customDraw->nmcd.dwItemSpec < ctx->recordingItems.size()) {
                    const auto& item = ctx->recordingItems[customDraw->nmcd.dwItemSpec];
                    if (item.outcome == AppContext::RecordingItem::Outcome::Success) {
                        customDraw->clrText = kColorSuccess;
                    } else if (item.outcome == AppContext::RecordingItem::Outcome::Failure) {
                        customDraw->clrText = kColorFailure;
                    }
                }
                return CDRF_NEWFONT;
            }
            if (customDraw->nmcd.dwDrawStage == CDDS_POSTPAINT) {
                DrawRecordingsGridLines(customDraw, ctx);
                return CDRF_DODEFAULT;
            }
        }
        if (hdr->idFrom == IDC_RECORDINGS_LIST && hdr->code == LVN_COLUMNCLICK) {
            auto* nmlv = reinterpret_cast<LPNMLISTVIEW>(lParam);
            if (nmlv->iSubItem < 0 || nmlv->iSubItem > 3) {
                return 0;
            }
            const auto clickedColumn = static_cast<AppContext::RecordingSortColumn>(nmlv->iSubItem);
            if (ctx->recordingSortColumn == clickedColumn) {
                ctx->recordingSortAscending = !ctx->recordingSortAscending;
            } else {
                ctx->recordingSortColumn = clickedColumn;
                ctx->recordingSortAscending = true;
            }
            SortRecordingItems(ctx);
            RepopulateRecordingsListControl(ctx);
            return 0;
        }
        if (hdr->idFrom == IDC_RECORDINGS_LIST && hdr->code == LVN_ITEMCHANGED) {
            const int selected = ListView_GetNextItem(ctx->recordingsList, -1, LVNI_SELECTED);
            UpdateRecordingInfoPane(ctx, selected);
            return 0;
        }
        if (hdr->idFrom == IDC_RECORDINGS_LIST && hdr->code == NM_DBLCLK) {
            const int selected = ListView_GetNextItem(ctx->recordingsList, -1, LVNI_SELECTED);
            if (selected >= 0 && static_cast<size_t>(selected) < ctx->recordingItems.size()) {
                const auto path = ctx->recordingItems[static_cast<size_t>(selected)].path;
                const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(hwnd, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                if (result <= 32) {
                    SetStatus(ctx, L"Failed to open selected recording.");
                }
            }
            return 0;
        }
        break;
    }
    case WM_DRAWITEM: {
        auto* drawInfo = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!drawInfo) {
            break;
        }
        if (drawInfo->CtlType == ODT_BUTTON && IsStyledButtonId(static_cast<int>(drawInfo->CtlID))) {
            DrawStyledButton(drawInfo, ctx);
            return TRUE;
        }
        if (drawInfo->CtlType == ODT_STATIC
            && (IsOwnerDrawStaticId(static_cast<int>(drawInfo->CtlID))
                || (ctx && drawInfo->hwndItem == ctx->configurationTooltip))) {
            if (IsStatusLightId(static_cast<int>(drawInfo->CtlID))) {
                DrawStatusLight(drawInfo, ctx);
            } else if (drawInfo->CtlID == IDC_LENGTH_VALUE) {
                DrawLengthValue(drawInfo);
            } else if (drawInfo->CtlID == IDC_YOUTUBE_LINK_STATUS) {
                DrawYouTubeLinkStatus(drawInfo, ctx);
            } else if (drawInfo->CtlID == IDC_CONFIGURATION_TOOLTIP || (ctx && drawInfo->hwndItem == ctx->configurationTooltip)) {
                DrawConfigurationTooltip(drawInfo);
            } else if (drawInfo->CtlID == IDC_PRESET_HELP || drawInfo->CtlID == IDC_POST_RUN_DELAY_HELP || drawInfo->CtlID == IDC_ADVANCED_LOGGING_HELP) {
                DrawHelpIcon(drawInfo);
            } else if (drawInfo->CtlID == IDC_CHAT_PREVIEW) {
                DrawChatPrivacyPreview(drawInfo, ctx);
            } else if (drawInfo->CtlID == IDC_CLIPS_TIMELINE) {
                DrawClipsSlider(drawInfo, ctx, true);
            } else if (drawInfo->CtlID == IDC_CLIPS_VOLUME_SLIDER) {
                DrawClipsSlider(drawInfo, ctx, false);
            }
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        if (!ctx) {
            break;
        }
        const auto control = reinterpret_cast<HWND>(lParam);
        HDC dc = reinterpret_cast<HDC>(wParam);
        if (control == ctx->clipsVideoSurface) {
            SetBkMode(dc, OPAQUE);
            SetTextColor(dc, kColorTextPrimary);
            SetBkColor(dc, kColorInputBg);
            return reinterpret_cast<LRESULT>(gTheme.inputBrush ? gTheme.inputBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        if (control == ctx->statusText) {
            SetBkMode(dc, OPAQUE);
            SetTextColor(dc, kColorTextPrimary);
            SetBkColor(dc, kColorInputBg);
            return reinterpret_cast<LRESULT>(gTheme.inputBrush ? gTheme.inputBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        if (control == ctx->outputStatus || control == ctx->wowLogStatus) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, kColorPanelBottom);
            if (control == ctx->outputStatus) {
                if (ctx->outputAvailable) {
                    SetTextColor(dc, kColorSuccess);
                } else if (ctx->outputFolderWillBeCreatedOnRecordStart) {
                    SetTextColor(dc, kColorWarning);
                } else {
                    SetTextColor(dc, kColorFailure);
                }
            } else {
                SetTextColor(dc, ctx->wowLogAvailable ? kColorSuccess : kColorFailure);
            }
            return reinterpret_cast<LRESULT>(gTheme.panelSolidBrush ? gTheme.panelSolidBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        if (control == ctx->clipsFfmpegWarning) {
            SetBkMode(dc, TRANSPARENT);
            if (ctx->clipsExportStatus == AppContext::ClipExportStatus::Success) {
                SetTextColor(dc, kColorSuccess);
            } else if (ctx->clipsExportStatus == AppContext::ClipExportStatus::Failure || !ctx->ffmpegDetected) {
                SetTextColor(dc, kColorFailure);
            } else {
                SetTextColor(dc, kColorTextMuted);
            }
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kColorTextPrimary);
        if (control != ctx->outputStatus && control != ctx->wowLogStatus) {
            const int id = GetDlgCtrlID(control);
            if (id == IDC_RECORDINGS_LABEL || id == IDC_RECORDINGS_UPLOAD_STATUS || id == IDC_ABOUT_BUILD_TEXT || id == IDC_YOUTUBE_UNLINK_CONFIRM_LABEL || id == IDC_CHAT_PREVIEW_STATUS || id == IDC_CONFIGURATION_AUTOSAVE_HINT) {
                SetTextColor(dc, kColorTextMuted);
            }
        }
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kColorTextPrimary);
        SetBkColor(dc, kColorInputBg);
        return reinterpret_cast<LRESULT>(gTheme.inputBrush ? gTheme.inputBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    case WM_CTLCOLORBTN: {
        HWND control = reinterpret_cast<HWND>(lParam);
        HDC dc = reinterpret_cast<HDC>(wParam);
        const int id = control ? GetDlgCtrlID(control) : 0;
        const bool isCheckOrRadioControl =
            id == IDC_AUDIO_SCOPE_CHECK
            || id == IDC_AUDIO_SCOPE_WOW_DISCORD_RADIO
            || id == IDC_AUDIO_SCOPE_ALL_RADIO
            || id == IDC_MICROPHONE_CHECK
            || id == IDC_MICROPHONE_NOISE_SUPPRESSION_CHECK
            || id == IDC_CHAT_BLOCKER_ENABLED_CHECK
            || id == IDC_CHAT_BLOCKER_IMAGE_BLANK_RADIO
            || id == IDC_CHAT_BLOCKER_IMAGE_CUSTOM_RADIO;
        if (isCheckOrRadioControl) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, kColorPanelBottom);
            SetTextColor(dc, (control && !IsWindowEnabled(control)) ? kColorTextMuted : kColorTextPrimary);
            return reinterpret_cast<LRESULT>(gTheme.panelSolidBrush ? gTheme.panelSolidBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }

        SetTextColor(dc, kColorButtonText);
        SetBkColor(dc, kColorButtonBg);
        return reinterpret_cast<LRESULT>(gTheme.buttonBrush ? gTheme.buttonBrush : reinterpret_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
    }
    case WM_GETMINMAXINFO: {
        auto* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
        minMax->ptMinTrackSize.x = kMinClientWidth;
        minMax->ptMinTrackSize.y = kMinClientHeight;
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        if (ctx) {
            ctx->clipsResizeInProgress = true;
        }
        return 0;
    case WM_EXITSIZEMOVE:
        if (ctx) {
            ctx->clipsResizeInProgress = false;
            ApplyClipVideoWindowBounds(ctx);
        }
        return 0;
    case WM_SIZE:
        if (ctx && wParam != SIZE_MINIMIZED) {
            LayoutMainUi(ctx, LOWORD(lParam), HIWORD(lParam));
            if (!ctx->clipsResizeInProgress) {
                ApplyClipVideoWindowBounds(ctx);
            }
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        HDC dc = reinterpret_cast<HDC>(wParam);
        TRIVERTEX vertices[2] = {
            {rect.left, rect.top, static_cast<COLOR16>(GetRValue(kColorWindowTop) << 8), static_cast<COLOR16>(GetGValue(kColorWindowTop) << 8), static_cast<COLOR16>(GetBValue(kColorWindowTop) << 8), 0xFF00},
            {rect.right, rect.bottom, static_cast<COLOR16>(GetRValue(kColorWindowBottom) << 8), static_cast<COLOR16>(GetGValue(kColorWindowBottom) << 8), static_cast<COLOR16>(GetBValue(kColorWindowBottom) << 8), 0xFF00},
        };
        GRADIENT_RECT gradientRect{0, 1};
        if (!GradientFill(dc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V)) {
            FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        TRIVERTEX vertices[2] = {
            {rect.left, rect.top, static_cast<COLOR16>(GetRValue(kColorWindowTop) << 8), static_cast<COLOR16>(GetGValue(kColorWindowTop) << 8), static_cast<COLOR16>(GetBValue(kColorWindowTop) << 8), 0xFF00},
            {rect.right, rect.bottom, static_cast<COLOR16>(GetRValue(kColorWindowBottom) << 8), static_cast<COLOR16>(GetGValue(kColorWindowBottom) << 8), static_cast<COLOR16>(GetBValue(kColorWindowBottom) << 8), 0xFF00},
        };
        GRADIENT_RECT gradientRect{0, 1};
        if (!GradientFill(dc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V)) {
            FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kClipsExportStatusTimerId) {
            if (ctx) {
                ClearClipsExportStatus(ctx);
            }
            return 0;
        }
        if (wParam == kLiveStatusTimerId) {
            if (ctx && ctx->orchestrator) {
                ctx->orchestrator->Tick();
            }
            RefreshLiveStatus(ctx);
            if (ctx && ctx->clipsLoaded) {
                if (ctx->clipsPreviewEngine) {
                    const bool isRunning = ctx->clipsPreviewEngine->IsPlaying();
                    if (ctx->clipsIsPlaying != isRunning) {
                        ctx->clipsIsPlaying = isRunning;
                        RefreshClipsPlaybackControls(ctx);
                    }
                }
                SyncClipTimelineFromPlayback(ctx);
            }
            return 0;
        }
        break;
    case WM_BEAN_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lParam);
        if (ctx && text) {
            SetStatus(ctx, *text);
        }
        delete text;
        return 0;
    }
    case WM_BEAN_YOUTUBE_UI_REFRESH:
        if (ctx) {
            RefreshYouTubeUiState(ctx);
        }
        return 0;
    case WM_BEAN_CLIPS_UI_REFRESH:
        if (ctx) {
            RefreshClipsPlaybackControls(ctx);
            RefreshClipsSourceList(ctx);
        }
        return 0;
    case WM_BEAN_CLIPS_MEDIA_EVENT:
        if (ctx && ctx->clipsPreviewEngine) {
            const DWORD eventCode = static_cast<DWORD>(wParam);
            if (eventCode == MF_MEDIA_ENGINE_EVENT_CANPLAY
                || eventCode == MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH) {
                LoadClipFromSelection(ctx, true);
            } else if (eventCode == MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE && ctx->clipsLoaded) {
                ctx->clipsDurationMs = ctx->clipsPreviewEngine->DurationMilliseconds();
                RefreshClipsPlaybackControls(ctx);
            } else if (eventCode == MF_MEDIA_ENGINE_EVENT_ERROR) {
                ctx->clipsLoaded = false;
                SetStatus(ctx, L"Could not load clip preview with Media Foundation.");
                RefreshClipsPlaybackControls(ctx);
            } else if (eventCode == MF_MEDIA_ENGINE_EVENT_PLAY
                || eventCode == MF_MEDIA_ENGINE_EVENT_PLAYING
                || eventCode == MF_MEDIA_ENGINE_EVENT_PAUSE
                || eventCode == MF_MEDIA_ENGINE_EVENT_ENDED
                || eventCode == MF_MEDIA_ENGINE_EVENT_TIMEUPDATE) {
                ctx->clipsIsPlaying = ctx->clipsPreviewEngine->IsPlaying();
                if (eventCode == MF_MEDIA_ENGINE_EVENT_PLAYING) {
                    SetStatus(ctx, L"Clip playback active.");
                } else if (eventCode == MF_MEDIA_ENGINE_EVENT_ENDED) {
                    SetStatus(ctx, L"Clip playback ended.");
                }
                InvalidateRect(ctx->clipsVideoSurface, nullptr, FALSE);
                RefreshClipsPlaybackControls(ctx);
            }
        }
        return 0;
    case WM_BEAN_CLIPS_EXPORT_COMPLETE: {
        auto* payload = reinterpret_cast<ClipExportCompletePayload*>(lParam);
        if (ctx && payload) {
            SetClipsExportStatus(
                ctx,
                payload->success ? AppContext::ClipExportStatus::Success : AppContext::ClipExportStatus::Failure,
                payload->message);
        }
        delete payload;
        return 0;
    }
    case WM_BEAN_YOUTUBE_AUTH_COMPLETE: {
        auto* payload = reinterpret_cast<YouTubeAuthCompletionPayload*>(lParam);
        if (ctx && payload) {
            ctx->youtubeBusy.store(false);
            if (!payload->success) {
                SetStatus(ctx, std::wstring(L"YouTube link failed: ") + ToWide(payload->error));
            } else {
                ctx->settings.youtubeClientId = payload->clientId;
                ctx->settings.youtubeRefreshToken = payload->refreshToken;
                ctx->settings.youtubeChannelId = payload->channelId;
                ctx->settings.youtubeChannelTitle = payload->channelTitle;
                std::string saveError;
                if (!ctx->settingsStore.Save(ctx->settings, saveError)) {
                    SetStatus(ctx, std::wstring(L"YouTube linked but saving settings failed: ") + ToWide(saveError));
                } else {
                    if (!payload->channelTitle.empty()) {
                        SetStatus(ctx, std::wstring(L"YouTube account linked: ") + ToWide(payload->channelTitle));
                    } else {
                        SetStatus(ctx, L"YouTube account linked successfully.");
                    }
                }
                if (payload->channelId.empty() && payload->channelTitle.empty()) {
                    ResolveLinkedYouTubeIdentityAsync(ctx, true);
                }
            }
            RefreshYouTubeUiState(ctx);
        }
        delete payload;
        return 0;
    }
    case WM_BEAN_YOUTUBE_UPLOAD_PROGRESS: {
        auto* payload = reinterpret_cast<YouTubeUploadProgressPayload*>(lParam);
        if (ctx && payload) {
            SetRecordingsUploadUi(ctx, payload->percent, payload->text);
        }
        delete payload;
        return 0;
    }
    case WM_BEAN_YOUTUBE_IDENTITY_RESOLVED: {
        auto* payload = reinterpret_cast<YouTubeIdentityResolvedPayload*>(lParam);
        if (ctx && payload) {
            if (payload->success) {
                ctx->settings.youtubeChannelId = payload->channelId;
                ctx->settings.youtubeChannelTitle = payload->channelTitle;
                std::string saveError;
                if (!ctx->settingsStore.Save(ctx->settings, saveError)) {
                    SetStatus(ctx, std::wstring(L"Failed to save linked YouTube account details: ") + ToWide(saveError));
                }
            } else if (!payload->error.empty()) {
                if (ctx->settings.youtubeChannelTitle.empty()) {
                    ctx->settings.youtubeChannelTitle = "details unavailable";
                }
                SetStatus(ctx, std::wstring(L"Could not resolve linked YouTube account: ") + ToWide(payload->error));
            }
            RefreshYouTubeUiState(ctx);
        }
        delete payload;
        return 0;
    }
    case WM_BEAN_UPDATE_AVAILABILITY_READY: {
        auto* payload = reinterpret_cast<UpdateAvailabilityPayload*>(lParam);
        if (ctx && payload) {
            ApplyAboutUpdateAvailabilityResult(ctx, *payload);
        }
        delete payload;
        return 0;
    }
    case WM_DESTROY:
        if (ctx && ctx->orchestrator) {
            ctx->orchestrator->StopMonitoring();
            std::string stopError;
            ctx->orchestrator->StopManualRecording(stopError);
        }
        CloseClipMedia(ctx);
        ShutdownTaskbarOverlay(ctx);
        if (ctx && ctx->configurationTooltip && IsWindow(ctx->configurationTooltip)) {
            DestroyWindow(ctx->configurationTooltip);
            ctx->configurationTooltip = nullptr;
        }
        if (ctx && ctx->chatPreviewFrameBitmap) {
            DeleteObject(ctx->chatPreviewFrameBitmap);
            ctx->chatPreviewFrameBitmap = nullptr;
            ctx->chatPreviewFrameValid = false;
        }
        if (ctx && ctx->statusLogStream.is_open()) {
            ctx->statusLogStream.flush();
            ctx->statusLogStream.close();
        }
        KillTimer(hwnd, kLiveStatusTimerId);
        KillTimer(hwnd, kClipsExportStatusTimerId);
        DestroyParticipantSpecIcons(ctx);
        DestroyThemeResources();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int cmdShow)
{
    std::wstring updaterInitWarning;
    bean::app::InitializeVelopackRuntime(updaterInitWarning);

    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(comInit);

    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_LINK_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    bean::core::AppSettings settings;
    bean::core::SettingsStore settingsStore;
    std::string loadError;
    settingsStore.Load(settings, loadError);
    std::string defaultsWarning;
    const bool defaultsApplied = ApplyReasonableDefaults(settings, defaultsWarning);
    std::string youtubeOAuthWarning;
    if (GetYouTubeAuthServerUrl().empty()) {
        youtubeOAuthWarning = "YouTube auth server is not configured. Set BEAN_YOUTUBE_AUTH_SERVER_URL to an HTTPS URL.";
    }
    if (defaultsApplied) {
        std::string saveError;
        settingsStore.Save(settings, saveError);
        if (!saveError.empty() && loadError.empty()) {
            loadError = "Defaults applied but saving failed: " + saveError;
        }
    }

    auto runRepository = std::make_shared<bean::core::RunRepository>();
    std::string runRepoError;
    if (!runRepository->Initialize(runRepoError)) {
        if (!loadError.empty()) {
            loadError += " ";
        }
        loadError += "Run metadata DB init failed: " + runRepoError;
    }

    auto orchestrator = std::make_unique<bean::core::RecordingOrchestrator>(std::make_unique<bean::obs::LibObsRecorderEngine>());
    orchestrator->SetRunRepository(runRepository);
    orchestrator->ApplySettings(settings);

    AppContext context;
    context.settingsStore = settingsStore;
    context.runRepository = runRepository;
    context.settings = settings;
    context.orchestrator = std::move(orchestrator);
    InitializeAppIcons(&context);

    context.orchestrator->SetStatusCallback([&context](const std::string& status) {
        if (!context.mainWindow) {
            return;
        }
        auto* text = new std::wstring(ToWide(status));
        PostMessageW(context.mainWindow, WM_BEAN_STATUS, 0, reinterpret_cast<LPARAM>(text));
    });

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = context.idleIcon.largeIcon;

    if (!RegisterClassW(&wc)) {
        return 1;
    }

    const std::wstring windowTitle = MainWindowTitleText();
    HWND hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        windowTitle.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        960,
        560,
        nullptr,
        nullptr,
        instance,
        &context);

    if (!hwnd) {
        DestroyAppIcons(&context);
        return 1;
    }
    context.mainWindow = hwnd;
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(context.idleIcon.smallIcon));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(context.idleIcon.largeIcon));
    InitializeTaskbarOverlay(&context);
    ApplyTaskbarOverlayState(&context, true);

    if (!loadError.empty()) {
        SetStatus(&context, std::wstring(L"Load settings warning: ") + ToWide(loadError));
    }
    if (!defaultsWarning.empty()) {
        SetStatus(&context, std::wstring(L"Defaults warning: ") + ToWide(defaultsWarning));
    }
    if (!youtubeOAuthWarning.empty()) {
        SetStatus(&context, std::wstring(L"YouTube OAuth warning: ") + ToWide(youtubeOAuthWarning));
    }
    if (!updaterInitWarning.empty()) {
        SetStatus(&context, updaterInitWarning);
    }

    ShowWindow(hwnd, cmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(hwnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (shouldUninitializeCom) {
        CoUninitialize();
    }
    ShutdownTaskbarOverlay(&context);
    DestroyAppIcons(&context);

    return 0;
}
