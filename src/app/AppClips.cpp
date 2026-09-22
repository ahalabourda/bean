#include "app/AppClips.h"

#include "app/AppDraw.h"
#include "app/AppLiveStatus.h"
#include "app/AppRecordingHelpers.h"
#include "app/AppStatusLog.h"
#include "app/AppUtilities.h"
#include "app/ClipPreviewEngine.h"
#include "core/RecordingSizeEstimate.h"
#include "obs/IRecorderEngine.h"
#include "util/Strings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using bean::util::ToUtf8;
using bean::util::ToWide;

void UpdateClipsPositionLabel(AppContext* ctx);
constexpr int kClipsTimelineMax = 1000;
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

    HBRUSH trackBrush = CreateSolidBrush(kThemeColors.sliderTrack);
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
        if (ParseClipTime(GetWindowTextString(ctx->clipsStartEdit), startSeconds)
            && ParseClipTime(GetWindowTextString(ctx->clipsEndEdit), endSeconds)
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
                    HBRUSH selectedBrush = CreateSolidBrush(kThemeColors.sliderSelection);
                    if (selectedBrush) {
                        FillRect(drawInfo->hDC, &selectedRect, selectedBrush);
                        DeleteObject(selectedBrush);
                    }
                }

                HPEN markerPen = CreatePen(PS_SOLID, 1, kThemeColors.sliderMarker);
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
    HBRUSH thumbBrush = CreateSolidBrush(kThemeColors.sliderThumb);
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

std::wstring BuildClipPositionText(int currentMs, int totalMs)
{
    return FormatClipTimeMs(currentMs) + L" / " + FormatClipTimeMs(totalMs);
}

struct FfmpegProcessResult {
    bool launched = false;
    DWORD launchError = ERROR_SUCCESS;
    DWORD exitCode = 1;
    std::wstring output;
};

FfmpegProcessResult RunFfmpegProcess(const std::filesystem::path& executablePath, const std::wstring& arguments)
{
    FfmpegProcessResult result;
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&outputRead, &outputWrite, &securityAttributes, 0)) {
        result.launchError = GetLastError();
        return result;
    }
    if (!SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0)) {
        result.launchError = GetLastError();
        CloseHandle(outputRead);
        CloseHandle(outputWrite);
        return result;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = outputWrite;
    startupInfo.hStdError = outputWrite;
    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = L"\"" + executablePath.wstring() + L"\" " + arguments;
    const BOOL created = CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        executablePath.parent_path().wstring().c_str(),
        &startupInfo,
        &processInfo);
    CloseHandle(outputWrite);

    if (!created) {
        result.launchError = GetLastError();
        CloseHandle(outputRead);
        return result;
    }
    result.launched = true;

    std::string output;
    std::thread outputReader([&output, outputRead]() {
        std::array<char, 4096> buffer{};
        DWORD bytesRead = 0;
        while (ReadFile(outputRead, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0) {
            if (output.size() < 64 * 1024) {
                output.append(buffer.data(), (std::min)(static_cast<size_t>(bytesRead), 64 * 1024 - output.size()));
            }
        }
        CloseHandle(outputRead);
    });

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    GetExitCodeProcess(processInfo.hProcess, &result.exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    outputReader.join();
    result.output = ToWide(output);
    return result;
}

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

void BeginClipExport(AppContext* ctx, bool precise);
std::optional<std::filesystem::path> ResolveFfmpegExecutablePath(AppContext* ctx);

std::filesystem::path AllocateUniqueClipOutputPath(const std::filesystem::path& desiredPath)
{
    std::error_code ec;
    if (!std::filesystem::exists(desiredPath, ec) && !ec) {
        return desiredPath;
    }

    const auto parent = desiredPath.parent_path();
    const std::wstring stem = desiredPath.stem().wstring();
    const std::wstring extension = desiredPath.extension().wstring();
    for (int suffix = 1; suffix < 10'000; ++suffix) {
        const auto candidate = parent / (stem + L"-" + std::to_wstring(suffix) + extension);
        ec.clear();
        if (!std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return desiredPath;
}

int QueryClipPositionMs(const AppContext* ctx, int fallback)
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
    if (ctx->clipsPreviewEngine && ctx->clipsPreviewEngine->IsReady()) {
        ctx->clipsPreviewEngine->UpdatePlaybackWindow();
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
    ctx->clipsTimelinePosition = 0;
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
    const BOOL clipControlsEnabled = (ctx->clipsLoaded && !previewBusy) ? TRUE : FALSE;
    const auto setEnabledIfChanged = [](HWND control, BOOL enabled) {
        if (!control || IsWindowEnabled(control) == enabled) {
            return false;
        }
        EnableWindow(control, enabled);
        return true;
    };
    if (ctx->clipsFfmpegWarning) {
        if (ctx->clipsExportStatus == AppContext::ClipExportStatus::Idle) {
            const wchar_t* warningText = ffmpegAvailable
                ? L""
                : L"FFmpeg is required to export clips.";
            UpdateTransparentStaticText(ctx->clipsFfmpegWarning, warningText);
            const bool shouldShow = !ffmpegAvailable;
            if ((IsWindowVisible(ctx->clipsFfmpegWarning) != FALSE) != shouldShow) {
                ShowWindow(ctx->clipsFfmpegWarning, shouldShow ? SW_SHOW : SW_HIDE);
            }
        } else {
            if (!IsWindowVisible(ctx->clipsFfmpegWarning)) {
                ShowWindow(ctx->clipsFfmpegWarning, SW_SHOW);
            }
        }
    }
    if (ctx->clipsSourceCombo) {
        setEnabledIfChanged(ctx->clipsSourceCombo, TRUE);
    }
    setEnabledIfChanged(GetDlgItem(ctx->clipsPanel, IDC_CLIPS_REFRESH), previewBusy ? FALSE : TRUE);
    if (ctx->clipsPlayPauseButton) {
        const wchar_t* playPauseText = ctx->clipsIsPlaying ? L"Pause" : L"Play";
        if (GetWindowTextString(ctx->clipsPlayPauseButton) != playPauseText) {
            SetWindowTextW(ctx->clipsPlayPauseButton, playPauseText);
        }
        setEnabledIfChanged(ctx->clipsPlayPauseButton, clipControlsEnabled);
    }
    if (ctx->clipsTimeline) {
        if (setEnabledIfChanged(ctx->clipsTimeline, clipControlsEnabled)) {
            InvalidateControlAndParentRegion(ctx->clipsTimeline);
        }
    }
    if (ctx->clipsVolumeSlider) {
        if (setEnabledIfChanged(ctx->clipsVolumeSlider, clipControlsEnabled)) {
            InvalidateControlAndParentRegion(ctx->clipsVolumeSlider);
        }
    }
    if (ctx->clipsStartEdit) {
        setEnabledIfChanged(ctx->clipsStartEdit, clipControlsEnabled);
    }
    if (ctx->clipsEndEdit) {
        setEnabledIfChanged(ctx->clipsEndEdit, clipControlsEnabled);
    }
    setEnabledIfChanged(GetDlgItem(ctx->clipsPanel, IDC_CLIPS_SET_START), clipControlsEnabled);
    setEnabledIfChanged(GetDlgItem(ctx->clipsPanel, IDC_CLIPS_SET_END), clipControlsEnabled);
    const BOOL exportEnabled =
        (ctx->clipsLoaded && ffmpegAvailable && !previewBusy && !ctx->clipsExportInProgress.load()) ? TRUE : FALSE;
    setEnabledIfChanged(GetDlgItem(ctx->clipsPanel, IDC_CLIPS_EXPORT), exportEnabled);
    setEnabledIfChanged(GetDlgItem(ctx->clipsPanel, IDC_CLIPS_EXPORT_PRECISE), exportEnabled);
    UpdateClipsPositionLabel(ctx);
}

void BeginClipExport(AppContext* ctx, bool precise)
{
    if (!ctx) {
        return;
    }
    if (ctx->clipsExportInProgress.load()) {
        SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Exporting, L"Exporting...");
        return;
    }
    if (!ctx->clipsLoaded || ctx->clipsLoadedPath.empty()) {
        SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"Select and load a recording first.");
        return;
    }
    if (!std::filesystem::exists(ctx->clipsLoadedPath)) {
        SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"Selected recording file no longer exists.");
        return;
    }
    int startSeconds = 0;
    int endSeconds = 0;
    if (!ParseClipTime(GetWindowTextString(ctx->clipsStartEdit), startSeconds)
        || !ParseClipTime(GetWindowTextString(ctx->clipsEndEdit), endSeconds)) {
        SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"Start and end must be valid times (mm:ss).");
        return;
    }
    if (endSeconds <= startSeconds) {
        SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"End must be greater than start.");
        return;
    }
    const auto ffmpegPath = ResolveFfmpegExecutablePath(ctx);
    if (!ffmpegPath.has_value()) {
        SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"FFmpeg executable could not be found.");
        return;
    }
    const auto recordingsFolder = ResolveRecordingsFolderPath(ctx);
    if (recordingsFolder.empty()) {
        SetClipsExportStatus(ctx, AppContext::ClipExportStatus::Failure, L"Recordings folder is unavailable.");
        return;
    }
    const auto clipsFolder = recordingsFolder / "Clips";
    std::error_code clipsCreateEc;
    std::filesystem::create_directories(clipsFolder, clipsCreateEc);
    if (clipsCreateEc) {
        SetClipsExportStatus(
            ctx,
            AppContext::ClipExportStatus::Failure,
            std::wstring(L"Could not create Clips folder: ") + ToWide(clipsCreateEc.message()));
        return;
    }

    const std::wstring sourceStem = ctx->clipsLoadedPath.stem().wstring();
    const std::wstring extension = ctx->clipsLoadedPath.extension().wstring().empty()
        ? L".mp4"
        : ctx->clipsLoadedPath.extension().wstring();
    std::wstringstream outputName;
    outputName << sourceStem << L"_clip_" << startSeconds << L"_" << endSeconds << extension;
    const auto outputPath = AllocateUniqueClipOutputPath(clipsFolder / outputName.str());

    // Precise export re-encodes; match the recording's quality tier when known.
    // Fallback CRF 24 == High (see ResolveConstantQualityValueForPreset).
    constexpr int kDefaultPreciseExportCrf = 24;
    int preciseCrf = kDefaultPreciseExportCrf;
    if (precise && ctx->runRepository) {
        std::string runLookupError;
        if (const auto run = ctx->runRepository->GetRunByVideoPath(ctx->clipsLoadedPath, runLookupError);
            run.has_value() && run->encoderPreset.has_value() && !run->encoderPreset->empty()) {
            preciseCrf = bean::obs::ResolveConstantQualityValueForPreset(*run->encoderPreset);
        }
    }

    ctx->clipsExportInProgress.store(true);
    RefreshClipsPlaybackControls(ctx);
    SetClipsExportStatus(
        ctx,
        AppContext::ClipExportStatus::Exporting,
        precise ? L"Exporting (precise)..." : L"Exporting (fast)...");
    SetStatus(
        ctx,
        std::wstring(precise ? L"Exporting precise clip to " : L"Exporting fast clip to ")
            + outputPath.filename().wstring() + L"...");
    const std::filesystem::path inputPath = ctx->clipsLoadedPath;
    const std::filesystem::path ffmpegExe = *ffmpegPath;
    LaunchAppWorker(ctx, [ctx, ffmpegExe, inputPath, outputPath, startSeconds, endSeconds, precise, preciseCrf]() {
        const int durationSeconds = endSeconds - startSeconds;
        std::wstringstream arguments;
        arguments << L"-y -hide_banner -v error"
                  << L" -ss " << startSeconds
                  << L" -i \"" << inputPath.wstring() << L"\""
                  << L" -t " << durationSeconds;
        if (precise) {
            // Re-encode so cuts land on exact timestamps instead of nearest keyframes.
            arguments << L" -c:v libx264 -crf " << preciseCrf << L" -preset veryfast"
                      << L" -c:a aac -b:a 160k";
            if (_wcsicmp(outputPath.extension().c_str(), L".mp4") == 0) {
                arguments << L" -movflags +faststart";
            }
        } else {
            arguments << L" -c copy -avoid_negative_ts make_zero";
        }
        arguments << L" \"" << outputPath.wstring() << L"\"";
        const FfmpegProcessResult process = RunFfmpegProcess(ffmpegExe, arguments.str());

        if (!process.launched) {
            auto* payload = new ClipExportCompletePayload();
            payload->message = std::wstring(L"Export failed to start (error ")
                + std::to_wstring(process.launchError) + L").";
            PostOwnedAppMessage(ctx, WM_BEAN_CLIPS_EXPORT_COMPLETE, payload);
            PostStatus(
                ctx,
                std::wstring(L"Clip export failed to start (error ")
                    + std::to_wstring(process.launchError) + L") using "
                    + ffmpegExe.wstring() + L".");
            ctx->clipsExportInProgress.store(false);
            PostBeanAppMessage(ctx, WM_BEAN_CLIPS_UI_REFRESH);
            return;
        }

        if (process.exitCode == 0) {
            auto* payload = new ClipExportCompletePayload();
            payload->success = true;
            payload->message = precise ? L"Precise export complete!" : L"Fast export complete!";
            PostOwnedAppMessage(ctx, WM_BEAN_CLIPS_EXPORT_COMPLETE, payload);
            PostStatus(ctx, std::wstring(L"Clip export complete: ") + outputPath.wstring());
        } else {
            auto* payload = new ClipExportCompletePayload();
            payload->message = std::wstring(L"Export failed (ffmpeg exit code ")
                + std::to_wstring(process.exitCode) + L").";
            PostOwnedAppMessage(ctx, WM_BEAN_CLIPS_EXPORT_COMPLETE, payload);
            std::wstring diagnostic = process.output;
            while (!diagnostic.empty() && (diagnostic.back() == L'\r' || diagnostic.back() == L'\n' || diagnostic.back() == L' ')) {
                diagnostic.pop_back();
            }
            PostStatus(
                ctx,
                std::wstring(L"Clip export failed (ffmpeg exit code ")
                    + std::to_wstring(process.exitCode)
                    + L") using "
                    + ffmpegExe.wstring()
                    + (diagnostic.empty() ? L"." : L"): " + diagnostic));
        }
        ctx->clipsExportInProgress.store(false);
        PostBeanAppMessage(ctx, WM_BEAN_CLIPS_UI_REFRESH);
    });
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
    const auto bundledCandidate = GetExecutableDirectory() / "ffmpeg.exe";
    if (std::filesystem::exists(bundledCandidate)) {
        resolvedPath = bundledCandidate;
    }

    if (ctx) {
        ctx->ffmpegExecutablePath = resolvedPath;
    }
    return resolvedPath;
}

bool IsFfmpegExecutableRunnable(const std::filesystem::path& executablePath)
{
    if (executablePath.empty() || !std::filesystem::exists(executablePath)) {
        return false;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = L"\"" + executablePath.wstring() + L"\" -hide_banner -version";
    const BOOL created = CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        executablePath.parent_path().wstring().c_str(),
        &startupInfo,
        &processInfo);
    if (!created) {
        return false;
    }

    // Bounded wait: "ffmpeg -version" is near-instant, and a hung binary must
    // not pin the probe thread forever.
    constexpr DWORD kProbeTimeoutMs = 5000;
    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, kProbeTimeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateProcess(processInfo.hProcess, 1);
        WaitForSingleObject(processInfo.hProcess, 1000);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return false;
    }

    DWORD exitCode = 1;
    const BOOL gotExitCode = GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return gotExitCode && exitCode == 0;
}

bool LoadClipFromSelection(AppContext* ctx, bool reportStatus)
{
    if (!ctx || !ctx->clipsSourceCombo) {
        return false;
    }
    const int selected = static_cast<int>(SendMessageW(ctx->clipsSourceCombo, CB_GETCURSEL, 0, 0));
    if (selected < 0 || static_cast<size_t>(selected) >= ctx->clipSourceItems.size()) {
        CloseClipMedia(ctx);
        ctx->clipsTimelinePosition = 0;
        if (ctx->clipsStartEdit) {
            SetWindowTextW(ctx->clipsStartEdit, L"00:00");
        }
        if (ctx->clipsEndEdit) {
            SetWindowTextW(ctx->clipsEndEdit, L"00:00");
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

    const bool selectionChanged = ctx->clipsLoadedPath.empty()
        || ctx->clipsLoadedPath.lexically_normal() != selectedPath.lexically_normal();
    if (selectionChanged) {
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

    if (!ctx->clipsPreviewEngine->IsReady()) {
        ctx->clipsLoaded = false;
        RefreshClipsPlaybackControls(ctx);
        return false;
    }

    // Media Foundation may retain the previous source's current time while
    // the new source is loading. Force the newly selected clip to its start
    // before syncing the UI playhead from the engine.
    ctx->clipsPreviewEngine->SeekMilliseconds(0);
    const auto nativeSize = ctx->clipsPreviewEngine->NativeVideoSize();
    ctx->clipsVideoSourceWidth = nativeSize.first;
    ctx->clipsVideoSourceHeight = nativeSize.second;
    ApplyClipVideoWindowBounds(ctx);
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
        SetWindowTextW(ctx->clipsStartEdit, L"00:00");
    }
    if (ctx->clipsEndEdit) {
        SetWindowTextW(
            ctx->clipsEndEdit,
            FormatElapsed(std::chrono::seconds((std::max)(0, ctx->clipsDurationMs / 1000))).c_str());
    }
    ApplyClipVolumePercent(ctx, ctx->clipsVolumePercent);
    RefreshClipsPlaybackControls(ctx);
    if (reportStatus) {
        SetStatus(ctx, std::wstring(L"Loaded clip: ") + selectedPath.filename().wstring());
    }
    return true;
}

bool ClipSourceListsEqual(
    const std::vector<std::filesystem::path>& current,
    const std::vector<std::filesystem::path>& next)
{
    if (current.size() != next.size()) {
        return false;
    }
    for (size_t index = 0; index < current.size(); ++index) {
        if (current[index].lexically_normal() != next[index].lexically_normal()) {
            return false;
        }
    }
    return true;
}

void RefreshClipsSourceList(AppContext* ctx)
{
    if (!ctx || !ctx->clipsSourceCombo) {
        return;
    }

    const auto folderPath = ResolveRecordingsFolderPath(ctx);
    if (folderPath.empty() || !std::filesystem::exists(folderPath)) {
        const bool hasComboItems = SendMessageW(ctx->clipsSourceCombo, CB_GETCOUNT, 0, 0) > 0;
        if (!ctx->clipSourceItems.empty() || hasComboItems
            || ctx->clipsLoaded || !ctx->clipsLoadedPath.empty()) {
            ctx->clipSourceItems.clear();
            if (hasComboItems) {
                SendMessageW(ctx->clipsSourceCombo, CB_RESETCONTENT, 0, 0);
            }
            CloseClipMedia(ctx);
            RefreshClipsPlaybackControls(ctx);
        }
        SetStatus(ctx, L"Recordings folder unavailable for clips.");
        return;
    }

    const std::vector<std::filesystem::path> files = EnumerateRecordingMediaFiles(folderPath);
    const bool sourceListChanged = !ClipSourceListsEqual(ctx->clipSourceItems, files);
    const std::wstring existingSelection = GetWindowTextString(ctx->clipsSourceCombo);
    if (sourceListChanged) {
        ctx->clipSourceItems.clear();
        SendMessageW(ctx->clipsSourceCombo, CB_RESETCONTENT, 0, 0);
        ctx->clipSourceItems = files;
        for (const auto& file : files) {
            const auto display = file.filename().wstring();
            SendMessageW(ctx->clipsSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        }
    }
    int restoreSelectionIndex = -1;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto display = files[i].filename().wstring();
        if (_wcsicmp(existingSelection.c_str(), display.c_str()) == 0) {
            restoreSelectionIndex = static_cast<int>(i);
        }
    }

    if (!files.empty()) {
        if (restoreSelectionIndex < 0) {
            restoreSelectionIndex = 0;
        }
        const int currentSelection = static_cast<int>(SendMessageW(
            ctx->clipsSourceCombo,
            CB_GETCURSEL,
            0,
            0));
        if (currentSelection != restoreSelectionIndex) {
            SendMessageW(ctx->clipsSourceCombo, CB_SETCURSEL, static_cast<WPARAM>(restoreSelectionIndex), 0);
        }
        const auto selectedPath = files[static_cast<size_t>(restoreSelectionIndex)];
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
        if (ctx->clipsLoaded || !ctx->clipsLoadedPath.empty() || ctx->clipsDurationMs != 0) {
            CloseClipMedia(ctx);
            RefreshClipsPlaybackControls(ctx);
        }
    }
}

void SyncClipTimelineFromPlayback(AppContext* ctx)
{
    if (!ctx || !ctx->clipsLoaded || !ctx->clipsTimeline || ctx->clipsTimelineScrubbing || ctx->clipsDurationMs <= 0) {
        return;
    }
    // A newly loaded clip is paused at the start. Media Foundation can report
    // a tiny nonzero startup timestamp; converting that against a very short
    // duration makes the playhead visibly jump forward. Keep an explicitly
    // left-positioned paused clip at zero until playback or a user seek moves it.
    if (!ctx->clipsIsPlaying && ctx->clipsTimelinePosition == 0) {
        return;
    }
    const int currentMs = QueryClipPositionMs(ctx, 0);
    if (!ctx->clipsIsPlaying && currentMs == 0 && ctx->clipsTimelinePosition > 0) {
        // Media Foundation can briefly report zero while a paused seek is
        // being applied. Do not overwrite the user's seek with that transient
        // value; the next media event/timer tick will provide the real time.
        return;
    }
    const int nextTimelinePosition = (std::clamp)(
        static_cast<int>((static_cast<long long>(currentMs) * kClipsTimelineMax) / (std::max)(1, ctx->clipsDurationMs)),
        0,
        kClipsTimelineMax);
    if (ctx->clipsTimelinePosition != nextTimelinePosition) {
        ctx->clipsTimelinePosition = nextTimelinePosition;
        InvalidateControlAndParentRegion(ctx->clipsTimeline);
    }
    UpdateClipsPositionLabel(ctx);
}

