#pragma once

#include "app/AppContext.h"

#include <filesystem>
#include <optional>

struct ClipExportCompletePayload {
    bool success = false;
    std::wstring message;
};

void CloseClipMedia(AppContext* ctx);
void ApplyClipVideoWindowBounds(AppContext* ctx);
int QueryClipPositionMs(const AppContext* ctx, int fallback = 0);
void RefreshClipsPlaybackControls(AppContext* ctx);
void BeginClipExport(AppContext* ctx, bool precise);
bool LoadClipFromSelection(AppContext* ctx, bool reportStatus = true);
void RefreshClipsSourceList(AppContext* ctx);
void SyncClipTimelineFromPlayback(AppContext* ctx);
void SetClipsExportStatus(AppContext* ctx, AppContext::ClipExportStatus status, const std::wstring& text);
void ClearClipsExportStatus(AppContext* ctx);

std::optional<std::filesystem::path> ResolveFfmpegExecutablePath(AppContext* ctx);
bool IsFfmpegExecutableRunnable(const std::filesystem::path& executablePath);

void DrawClipsSlider(const DRAWITEMSTRUCT* drawInfo, const AppContext* ctx, bool isTimeline);
LRESULT CALLBACK ClipsSliderSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR refData);
