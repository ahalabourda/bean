#pragma once

#include "app/AppContext.h"

#include <cstddef>

// Data-driven status-tab prerequisite rows (f18). Creation, layout, polling and
// drawing iterate this table instead of five near-copies.
struct PrerequisiteRow {
    int labelId = 0;
    int iconId = 0;
    int textId = 0;
    const wchar_t* labelText = nullptr;
    bool AppContext::*healthMember = nullptr;
    // When true, the row is only shown while the probe reports unhealthy/conflict.
    bool visibleOnlyWhenUnhealthy = false;
    // When true, the green check means the probe returned false (e.g. WCR absent).
    bool invertHealthy = false;
};

inline constexpr PrerequisiteRow kPrerequisiteRows[] = {
    {IDC_WOW_WINDOW_LABEL, IDC_WOW_WINDOW_ICON, IDC_WOW_WINDOW_TEXT, L"WoW Window", &AppContext::wowWindowDetected, false, false},
    {IDC_OBS_INSTALL_LABEL, IDC_OBS_INSTALL_ICON, IDC_OBS_INSTALL_TEXT, L"OBS Install", &AppContext::obsInstallDetected, false, false},
    {IDC_FFMPEG_LABEL, IDC_FFMPEG_ICON, IDC_FFMPEG_TEXT, L"FFmpeg", &AppContext::ffmpegDetected, false, false},
    {IDC_WARCRAFT_RECORDER_LABEL, IDC_WARCRAFT_RECORDER_ICON, IDC_WARCRAFT_RECORDER_TEXT, L"WCR Conflict", &AppContext::warcraftRecorderDetected, true, true},
    {IDC_ADVANCED_LOGGING_LABEL, IDC_ADVANCED_LOGGING_ICON, IDC_ADVANCED_LOGGING_TEXT, L"Advanced Logging", &AppContext::advancedCombatLoggingEnabled, false, false},
};

inline constexpr std::size_t kPrerequisiteRowCount = sizeof(kPrerequisiteRows) / sizeof(kPrerequisiteRows[0]);

bool PrerequisiteRowIsHealthy(const AppContext* ctx, const PrerequisiteRow& row);
HWND* PrerequisiteIconHwndSlot(AppContext* ctx, int iconId);
HWND* PrerequisiteTextHwndSlot(AppContext* ctx, int textId);
