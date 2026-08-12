#pragma once

#include "app/ClipPreviewEngine.h"
#include "app/AppRecordingHelpers.h"
#include "app/AppWorkerRegistry.h"
#include "core/RecordingOrchestrator.h"
#include "core/RunRepository.h"
#include "core/SettingsStore.h"

#include <windows.h>
#include <shobjidl.h>

#include <atomic>
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

inline constexpr wchar_t kWindowClassName[] = L"BeanMainWindow";
inline constexpr wchar_t kWindowTitleBase[] = L"Battle Encounter Archival Nexus - WoW Recorder";
inline constexpr wchar_t kAboutTitleText[] = L"Battle Encounter Archival Nexus";
inline constexpr wchar_t kAboutFlavorText[] = L"\"Thanks for using Bean!\"";
inline constexpr wchar_t kIconFile16[] = L"bean-16.ico";
inline constexpr wchar_t kIconFile32[] = L"bean-32.ico";
inline constexpr wchar_t kIconFile48[] = L"bean-48.ico";
inline constexpr wchar_t kIconFile256[] = L"bean-256.ico";
inline constexpr int kEmbeddedAppIconResourceId = 101;
inline constexpr UINT WM_BEAN_STATUS = WM_APP + 100;
inline constexpr UINT WM_BEAN_YOUTUBE_UI_REFRESH = WM_APP + 101;
inline constexpr UINT WM_BEAN_YOUTUBE_AUTH_COMPLETE = WM_APP + 102;
inline constexpr UINT WM_BEAN_YOUTUBE_UPLOAD_PROGRESS = WM_APP + 103;
inline constexpr UINT WM_BEAN_YOUTUBE_IDENTITY_RESOLVED = WM_APP + 104;
inline constexpr UINT WM_BEAN_CLIPS_UI_REFRESH = WM_APP + 105;
inline constexpr UINT WM_BEAN_CLIPS_EXPORT_COMPLETE = WM_APP + 107;
inline constexpr UINT WM_BEAN_UPDATE_AVAILABILITY_READY = WM_APP + 108;
inline constexpr UINT WM_BEAN_CLIPS_MEDIA_EVENT = WM_APP + 109;
inline constexpr UINT WM_BEAN_FFMPEG_PROBE_COMPLETE = WM_APP + 110;
inline constexpr UINT WM_BEAN_FILE_LIST_SELECTION = WM_APP + 111;
inline constexpr UINT WM_BEAN_FILE_LIST_COLUMN_CLICK = WM_APP + 112;
inline constexpr UINT WM_BEAN_FILE_LIST_DOUBLE_CLICK = WM_APP + 113;
inline constexpr wchar_t kStatusLogFilePrefix[] = L"bean-status-log-";
inline constexpr wchar_t kStatusLogFileExtension[] = L".txt";
inline constexpr size_t kStatusLogRetentionCount = 5;
inline constexpr wchar_t kYouTubeOAuthCredentialsMissingMessage[] =
    L"YouTube auth server is unavailable. Please try again later.";
inline constexpr UINT_PTR kLiveStatusTimerId = 1;
inline constexpr UINT_PTR kClipsExportStatusTimerId = 2;
inline constexpr UINT_PTR kConfigurationAutoSaveTimerId = 3;
inline constexpr UINT_PTR kChatBlockerAutoSaveTimerId = 4;
// Edit controls raise EN_CHANGE per keystroke. Coalesce them so typing a path
// does not rewrite config.json and reconfigure the orchestrator per character.
inline constexpr UINT kAutoSaveDebounceMs = 400;
inline constexpr int kClipHotkeyId = 1;
inline constexpr int kManualStartHotkeyId = 2;
inline constexpr int kManualStopHotkeyId = 3;
inline constexpr UINT kLiveStatusIntervalMs = 500;
inline constexpr auto kChatPreviewCaptureInterval = std::chrono::milliseconds(1000);
inline constexpr auto kChatPreviewInvalidateInterval = std::chrono::milliseconds(1000);
inline constexpr auto kWowWindowPollInterval = std::chrono::seconds(2);
inline constexpr auto kObsInstallPollInterval = std::chrono::seconds(2);
inline constexpr auto kMonitoringRetryInterval = std::chrono::seconds(5);
inline constexpr auto kWarcraftRecorderPollInterval = std::chrono::seconds(30);
inline constexpr size_t kStatusMaxLines = 300;
// Keep the visual palette in one place. Layout code should only depend on
// geometry constants, while drawing code should consume these named tokens.
// This makes a future theme/settings editor a palette change rather than a
// control-by-control rewrite.
struct ThemeColors {
    COLORREF windowTop;
    COLORREF windowBottom;
    COLORREF panelTop;
    COLORREF panelBottom;
    COLORREF panelBorder;
    COLORREF textPrimary;
    COLORREF textMuted;
    COLORREF inputBackground;
    COLORREF youtubeInputBackground;
    COLORREF inputBorder;
    COLORREF buttonBackground;
    COLORREF buttonText;
    COLORREF tooltipBackground;
    COLORREF tooltipText;
    COLORREF listSelection;
    COLORREF listRow;
    COLORREF listRowAlternate;
    COLORREF listGrid;
    COLORREF success;
    COLORREF failure;
    COLORREF warning;
    COLORREF accent;
    COLORREF accentBright;
    COLORREF controlHoverBackground;
    COLORREF controlHoverBorder;
    COLORREF controlPressedBackground;
    COLORREF controlPressedBorder;
    COLORREF controlActiveBackground;
    COLORREF controlActiveBorder;
    COLORREF controlTabBackground;
    COLORREF controlTabBorder;
    COLORREF controlDisabledBackground;
    COLORREF controlDisabledBorder;
    COLORREF controlDisabledText;
    COLORREF dropdownHoverBackground;
    COLORREF sliderTrack;
    COLORREF sliderSelection;
    COLORREF sliderMarker;
    COLORREF sliderThumb;
    COLORREF mutedDot;
    COLORREF recordingDot;
    COLORREF listSelectionInactive;
    COLORREF scrollbarTrack;
    COLORREF scrollbarThumb;
    COLORREF scrollbarThumbHover;
};

inline constexpr ThemeColors kBeanAlphaThemeColors{
    RGB(11, 14, 23),
    RGB(5, 7, 14),
    RGB(31, 36, 52),
    RGB(18, 21, 34),
    RGB(72, 86, 122),
    RGB(228, 234, 246),
    RGB(165, 176, 203),
    RGB(17, 21, 33),
    RGB(31, 39, 59),
    RGB(66, 79, 113),
    RGB(48, 59, 86),
    RGB(235, 241, 255),
    RGB(22, 27, 40),
    RGB(233, 239, 251),
    RGB(55, 76, 124),
    RGB(12, 16, 25),
    RGB(16, 21, 32),
    RGB(41, 50, 71),
    RGB(80, 214, 142),
    RGB(241, 100, 125),
    RGB(241, 204, 96),
    RGB(91, 148, 255),
    RGB(190, 216, 255),
    RGB(58, 72, 104),
    RGB(104, 129, 183),
    RGB(73, 103, 166),
    RGB(118, 148, 212),
    RGB(59, 77, 119),
    RGB(190, 216, 255),
    RGB(32, 38, 56),
    RGB(76, 94, 136),
    RGB(32, 38, 55),
    RGB(56, 67, 95),
    RGB(127, 139, 167),
    RGB(55, 76, 124),
    RGB(13, 18, 29),
    RGB(73, 103, 166),
    RGB(189, 214, 255),
    RGB(233, 239, 251),
    RGB(138, 151, 183),
    RGB(255, 112, 130),
    RGB(34, 42, 61),
    RGB(12, 16, 25),
    RGB(72, 86, 122),
    RGB(104, 129, 183),
};

// UI-thread-owned active palette. It starts with Bean Alpha and is replaced
// when the user selects another preset in Customize.
inline ThemeColors kThemeColors = kBeanAlphaThemeColors;

constexpr COLORREF MixThemeColors(COLORREF first, COLORREF second, int secondWeightPercent)
{
    const int weight = secondWeightPercent < 0 ? 0 : (secondWeightPercent > 100 ? 100 : secondWeightPercent);
    const int firstWeight = 100 - weight;
    const auto mixChannel = [firstWeight, weight](int firstChannel, int secondChannel) {
        return (firstChannel * firstWeight + secondChannel * weight + 50) / 100;
    };
    return RGB(
        mixChannel(GetRValue(first), GetRValue(second)),
        mixChannel(GetGValue(first), GetGValue(second)),
        mixChannel(GetBValue(first), GetBValue(second)));
}

inline ThemeColors MakeThemePalette(
    COLORREF windowTop,
    COLORREF windowBottom,
    COLORREF panelTop,
    COLORREF panelBottom,
    COLORREF panelBorder,
    COLORREF textPrimary,
    COLORREF textMuted,
    COLORREF inputBackground,
    COLORREF buttonBackground,
    COLORREF accent,
    COLORREF accentBright,
    COLORREF success,
    COLORREF failure,
    COLORREF warning)
{
    const COLORREF youtubeInputBackground = MixThemeColors(inputBackground, panelTop, 35);
    const COLORREF inputBorder = MixThemeColors(panelBorder, textMuted, 35);
    const COLORREF tooltipBackground = MixThemeColors(windowTop, windowBottom, 60);
    const COLORREF listSelection = MixThemeColors(accent, panelBottom, 55);
    const COLORREF listRow = MixThemeColors(inputBackground, panelBottom, 25);
    const COLORREF listRowAlternate = MixThemeColors(panelBottom, inputBackground, 45);
    const COLORREF listGrid = MixThemeColors(panelBorder, panelBottom, 50);
    const COLORREF controlHoverBackground = MixThemeColors(buttonBackground, accent, 35);
    const COLORREF controlHoverBorder = MixThemeColors(accentBright, panelBorder, 35);
    const COLORREF controlPressedBackground = MixThemeColors(buttonBackground, accent, 60);
    const COLORREF controlPressedBorder = accent;
    const COLORREF controlActiveBackground = MixThemeColors(accent, panelBottom, 35);
    const COLORREF controlActiveBorder = accentBright;
    const COLORREF controlTabBackground = MixThemeColors(buttonBackground, windowTop, 30);
    const COLORREF controlTabBorder = MixThemeColors(panelBorder, windowTop, 40);
    const COLORREF controlDisabledBackground = MixThemeColors(panelBottom, windowTop, 35);
    const COLORREF controlDisabledBorder = MixThemeColors(panelBorder, windowTop, 45);
    const COLORREF controlDisabledText = MixThemeColors(textMuted, windowTop, 35);
    const COLORREF dropdownHoverBackground = listSelection;
    const COLORREF sliderTrack = panelBorder;
    const COLORREF sliderSelection = accent;
    const COLORREF sliderMarker = accentBright;
    const COLORREF sliderThumb = textPrimary;
    const COLORREF mutedDot = controlTabBorder;
    const COLORREF recordingDot = failure;
    const COLORREF listSelectionInactive = MixThemeColors(listSelection, panelBottom, 45);
    const COLORREF scrollbarTrack = MixThemeColors(panelBottom, windowTop, 35);
    const COLORREF scrollbarThumb = MixThemeColors(panelBorder, textMuted, 25);
    const COLORREF scrollbarThumbHover = MixThemeColors(accent, panelBorder, 45);
    return {
        windowTop,
        windowBottom,
        panelTop,
        panelBottom,
        panelBorder,
        textPrimary,
        textMuted,
        inputBackground,
        youtubeInputBackground,
        inputBorder,
        buttonBackground,
        textPrimary,
        tooltipBackground,
        textPrimary,
        listSelection,
        listRow,
        listRowAlternate,
        listGrid,
        success,
        failure,
        warning,
        accent,
        accentBright,
        controlHoverBackground,
        controlHoverBorder,
        controlPressedBackground,
        controlPressedBorder,
        controlActiveBackground,
        controlActiveBorder,
        controlTabBackground,
        controlTabBorder,
        controlDisabledBackground,
        controlDisabledBorder,
        controlDisabledText,
        dropdownHoverBackground,
        sliderTrack,
        sliderSelection,
        sliderMarker,
        sliderThumb,
        mutedDot,
        recordingDot,
        listSelectionInactive,
        scrollbarTrack,
        scrollbarThumb,
        scrollbarThumbHover
    };
}

struct ThemeDefinition {
    const char* id;
    const wchar_t* displayName;
    ThemeColors colors;
};

inline const std::array<ThemeDefinition, 10> kThemeDefinitions{{
    {"bean_alpha", L"Bean Alpha", kBeanAlphaThemeColors},
    {"midnight_roast", L"Midnight Roast", MakeThemePalette(
        RGB(10, 12, 20), RGB(4, 5, 10), RGB(34, 31, 46), RGB(19, 17, 29), RGB(91, 72, 113),
        RGB(244, 237, 255), RGB(186, 171, 204), RGB(22, 19, 34), RGB(53, 43, 67),
        RGB(177, 112, 255), RGB(218, 177, 255), RGB(100, 224, 158), RGB(255, 108, 146), RGB(255, 207, 103))},
    {"mocha", L"Mocha", MakeThemePalette(
        RGB(29, 18, 15), RGB(14, 8, 7), RGB(65, 42, 32), RGB(36, 22, 18), RGB(126, 83, 61),
        RGB(255, 242, 225), RGB(207, 168, 137), RGB(35, 21, 17), RGB(82, 48, 36),
        RGB(222, 139, 83), RGB(255, 190, 120), RGB(111, 214, 135), RGB(239, 108, 100), RGB(255, 211, 111))},
    {"matcha", L"Matcha", MakeThemePalette(
        RGB(13, 23, 18), RGB(6, 12, 9), RGB(31, 53, 42), RGB(16, 31, 24), RGB(70, 112, 88),
        RGB(232, 248, 235), RGB(162, 197, 173), RGB(15, 29, 22), RGB(37, 67, 48),
        RGB(102, 194, 126), RGB(170, 235, 157), RGB(102, 221, 145), RGB(246, 111, 118), RGB(244, 211, 101))},
    {"ocean", L"Ocean", MakeThemePalette(
        RGB(8, 20, 31), RGB(3, 9, 16), RGB(24, 53, 70), RGB(12, 28, 40), RGB(54, 115, 143),
        RGB(226, 246, 255), RGB(154, 194, 213), RGB(10, 27, 39), RGB(25, 65, 88),
        RGB(64, 173, 225), RGB(139, 221, 255), RGB(85, 218, 173), RGB(245, 108, 127), RGB(245, 204, 92))},
    {"berry", L"Berry", MakeThemePalette(
        RGB(27, 10, 23), RGB(13, 4, 12), RGB(61, 25, 52), RGB(33, 12, 29), RGB(125, 57, 105),
        RGB(255, 234, 249), RGB(211, 159, 194), RGB(32, 12, 28), RGB(79, 31, 67),
        RGB(226, 91, 167), RGB(255, 157, 211), RGB(103, 220, 155), RGB(255, 101, 137), RGB(255, 211, 104))},
    {"sunset", L"Sunset", MakeThemePalette(
        RGB(31, 15, 12), RGB(15, 6, 7), RGB(72, 35, 30), RGB(39, 17, 19), RGB(137, 69, 58),
        RGB(255, 238, 224), RGB(211, 164, 146), RGB(39, 17, 18), RGB(92, 41, 34),
        RGB(239, 126, 78), RGB(255, 181, 107), RGB(101, 219, 157), RGB(255, 103, 115), RGB(255, 213, 104))},
    {"lavender", L"Lavender", MakeThemePalette(
        RGB(18, 15, 32), RGB(8, 6, 18), RGB(45, 37, 68), RGB(24, 18, 42), RGB(95, 80, 142),
        RGB(241, 238, 255), RGB(181, 171, 211), RGB(24, 19, 43), RGB(59, 47, 91),
        RGB(145, 121, 235), RGB(198, 177, 255), RGB(104, 224, 169), RGB(248, 105, 135), RGB(246, 210, 102))},
    {"ember", L"Ember", MakeThemePalette(
        RGB(25, 17, 10), RGB(12, 7, 4), RGB(58, 39, 24), RGB(31, 19, 10), RGB(125, 82, 43),
        RGB(255, 244, 222), RGB(210, 178, 128), RGB(31, 19, 10), RGB(79, 47, 23),
        RGB(232, 143, 53), RGB(255, 195, 102), RGB(105, 222, 145), RGB(248, 102, 100), RGB(255, 216, 100))},
    {"high_contrast", L"High Contrast", MakeThemePalette(
        RGB(0, 0, 0), RGB(0, 0, 0), RGB(20, 20, 20), RGB(5, 5, 5), RGB(130, 130, 130),
        RGB(255, 255, 255), RGB(220, 220, 220), RGB(15, 15, 15), RGB(42, 42, 42),
        RGB(0, 170, 255), RGB(100, 220, 255), RGB(0, 255, 120), RGB(255, 70, 90), RGB(255, 220, 0))}
}};

inline const ThemeDefinition* FindThemeDefinition(const std::string& id)
{
    for (const auto& theme : kThemeDefinitions) {
        if (id == theme.id) {
            return &theme;
        }
    }
    return &kThemeDefinitions.front();
}

inline size_t ThemeIndexForId(const std::string& id)
{
    for (size_t index = 0; index < kThemeDefinitions.size(); ++index) {
        if (id == kThemeDefinitions[index].id) {
            return index;
        }
    }
    return 0;
}

// Compatibility aliases keep existing drawing code readable while ensuring
// the actual values above remain centralized.
inline COLORREF& kColorWindowTop = kThemeColors.windowTop;
inline COLORREF& kColorWindowBottom = kThemeColors.windowBottom;
inline COLORREF& kColorPanelTop = kThemeColors.panelTop;
inline COLORREF& kColorPanelBottom = kThemeColors.panelBottom;
inline COLORREF& kColorPanelBorder = kThemeColors.panelBorder;
inline COLORREF& kColorTextPrimary = kThemeColors.textPrimary;
inline COLORREF& kColorTextMuted = kThemeColors.textMuted;
inline COLORREF& kColorInputBg = kThemeColors.inputBackground;
inline COLORREF& kColorYouTubeInputBg = kThemeColors.youtubeInputBackground;
inline COLORREF& kColorInputBorder = kThemeColors.inputBorder;
inline COLORREF& kColorButtonBg = kThemeColors.buttonBackground;
inline COLORREF& kColorButtonText = kThemeColors.buttonText;
inline COLORREF& kColorTooltipBg = kThemeColors.tooltipBackground;
inline COLORREF& kColorTooltipText = kThemeColors.tooltipText;
inline COLORREF& kColorListSelection = kThemeColors.listSelection;
inline COLORREF& kColorListRow = kThemeColors.listRow;
inline COLORREF& kColorListRowAlt = kThemeColors.listRowAlternate;
inline COLORREF& kColorListGrid = kThemeColors.listGrid;
inline COLORREF& kColorSuccess = kThemeColors.success;
inline COLORREF& kColorFailure = kThemeColors.failure;
inline COLORREF& kColorWarning = kThemeColors.warning;
inline constexpr int kMinClientWidth = 930;
inline constexpr int kMinClientHeight = 560;
inline constexpr int kSpecIconSizePx = 16;
inline constexpr int kSpecIconCanvasSizePx = 18;
inline constexpr int kSpecIconVerticalOffsetPx = 1;

struct VisualTheme {
    HFONT uiFont = nullptr;
    HFONT mutedHintFont = nullptr;
    HFONT mutedItalicHintFont = nullptr;
    HFONT statusIndicatorFont = nullptr;
    HFONT recordingsFont = nullptr;
    HFONT headingFont = nullptr;
    HBRUSH inputBrush = nullptr;
    HBRUSH youtubeInputBrush = nullptr;
    HBRUSH buttonBrush = nullptr;
    HBRUSH panelSolidBrush = nullptr;
    HBRUSH panelBorderBrush = nullptr;
    HBRUSH tooltipBrush = nullptr;
    // Cached for owner-draw hot paths (status glyphs, list grids, dots).
    HPEN successPen = nullptr;
    HPEN failurePen = nullptr;
    HPEN listGridPen = nullptr;
    HPEN mutedDotPen = nullptr;
    HPEN recordingDotPen = nullptr;
    HBRUSH successBrush = nullptr;
    HBRUSH failureBrush = nullptr;
    HBRUSH mutedDotBrush = nullptr;
    HBRUSH recordingDotBrush = nullptr;
};

struct MicrophoneOption {
    std::wstring displayName;
    std::string deviceId;
};

enum class TaskbarOverlayState {
    Idle,
    MonitoringReady,
    Recording,
    Warning
};

// Set when mythic auto-start fails so the taskbar overlay shows Warning without
// requiring the Status tab to be open. Cleared on a successful recording start
// or when monitoring is not active.
inline constexpr char kAutoRecordFailedStatusPrefix[] = "AUTO-RECORD FAILED";

struct AppIconSet {
    HICON smallIcon = nullptr;
    HICON largeIcon = nullptr;
};

struct TaskbarOverlayIconSet {
    HICON readyIcon = nullptr;
    HICON recordingIcon = nullptr;
    HICON warningIcon = nullptr;
};

enum ControlId {
    IDC_OUTPUT_LABEL = 1000,
    IDC_OUTPUT_EDIT,
    IDC_OUTPUT_BROWSE,
    IDC_OUTPUT_STATUS,
    IDC_LOG_LABEL,
    IDC_LOG_EDIT,
    IDC_LOG_BROWSE,
    IDC_LOG_STATUS,
    IDC_ENCODER_LABEL,
    IDC_ENCODER_COMBO,
    IDC_PRESET_LABEL,
    IDC_PRESET_COMBO,
    IDC_PRESET_HELP,
    IDC_CONTAINER_LABEL,
    IDC_CONTAINER_COMBO,
    IDC_AUDIO_SCOPE_LABEL,
    IDC_AUDIO_SCOPE_CHECK,
    IDC_AUDIO_SCOPE_WOW_DISCORD_RADIO,
    IDC_AUDIO_SCOPE_ALL_RADIO,
    IDC_MICROPHONE_CHECK,
    IDC_MICROPHONE_NOISE_SUPPRESSION_CHECK,
    IDC_MICROPHONE_COMBO,
    IDC_GAME_RESOLUTION_LABEL,
    IDC_GAME_RESOLUTION_TEXT,
    IDC_RECORDING_RESOLUTION_LABEL,
    IDC_RECORDING_RESOLUTION_COMBO,
    IDC_FPS_LABEL,
    IDC_FPS_EDIT,
    IDC_POST_RUN_DELAY_LABEL,
    IDC_POST_RUN_DELAY_HELP,
    IDC_ADVANCED_LOGGING_HELP,
    IDC_POST_RUN_DELAY_EDIT,
    IDC_CLIP_DURATION_LABEL,
    IDC_CLIP_DURATION_EDIT,
    IDC_CONFIGURATION_AUTOSAVE_HINT,
    IDC_CUSTOMIZE_THEME_LABEL,
    IDC_CUSTOMIZE_THEME_COMBO,
    IDC_KEYBINDS_INFO,
    IDC_KEYBINDS_AUTOSAVE_HINT,
    IDC_KEYBINDS_CREATE_CLIP_LABEL,
    IDC_KEYBINDS_MANUAL_START_LABEL,
    IDC_KEYBINDS_MANUAL_STOP_LABEL,
    IDC_KEYBINDS_CREATE_CLIP_VALUE,
    IDC_KEYBINDS_MANUAL_START_VALUE,
    IDC_KEYBINDS_MANUAL_STOP_VALUE,
    IDC_KEYBINDS_CREATE_CLIP_REBIND,
    IDC_KEYBINDS_MANUAL_START_REBIND,
    IDC_KEYBINDS_MANUAL_STOP_REBIND,
    IDC_KEYBINDS_CREATE_CLIP_UNBIND,
    IDC_KEYBINDS_MANUAL_START_UNBIND,
    IDC_KEYBINDS_MANUAL_STOP_UNBIND,
    IDC_KEYBINDS_CREATE_CLIP_RESET,
    IDC_KEYBINDS_MANUAL_START_RESET,
    IDC_KEYBINDS_MANUAL_STOP_RESET,
    IDC_RECORD_START,
    IDC_RECORD_STOP,
    IDC_LIVE_LABEL,
    IDC_MONITOR_ICON,
    IDC_MONITOR_TEXT,
    IDC_RECORD_ICON,
    IDC_RECORD_TEXT,
    IDC_LENGTH_LABEL,
    IDC_LENGTH_VALUE,
    IDC_SAVE_SETTINGS,
    IDC_WOW_WINDOW_LABEL,
    IDC_WOW_WINDOW_ICON,
    IDC_WOW_WINDOW_TEXT,
    IDC_OBS_INSTALL_LABEL,
    IDC_OBS_INSTALL_ICON,
    IDC_OBS_INSTALL_TEXT,
    IDC_FFMPEG_LABEL,
    IDC_FFMPEG_ICON,
    IDC_FFMPEG_TEXT,
    IDC_WARCRAFT_RECORDER_LABEL,
    IDC_WARCRAFT_RECORDER_ICON,
    IDC_WARCRAFT_RECORDER_TEXT,
    IDC_ADVANCED_LOGGING_LABEL,
    IDC_ADVANCED_LOGGING_ICON,
    IDC_ADVANCED_LOGGING_TEXT,
    IDC_STATUS_LABEL,
    IDC_STATUS_TEXT,
    IDC_STATUS_OPEN_LOG_FOLDER,
    IDC_STATUS_COPY_LOG_TEXT,
    IDC_TAB_STATUS,
    IDC_TAB_CONFIGURATION,
    IDC_TAB_CHAT_PRIVACY,
    IDC_TAB_RECORDINGS,
    IDC_TAB_YOUTUBE,
    IDC_TAB_CLIPS,
    IDC_TAB_KEYBINDS,
    IDC_TAB_ABOUT,
    IDC_CHAT_BLOCKER_ENABLED_CHECK,
    IDC_CHAT_BLOCKER_IMAGE_BLANK_RADIO,
    IDC_CHAT_BLOCKER_IMAGE_CUSTOM_RADIO,
    IDC_CHAT_BLOCKER_IMAGE_LIBRARY_LABEL,
    IDC_CHAT_BLOCKER_IMAGE_IMPORT_BUTTON,
    IDC_CHAT_BLOCKER_IMAGE_OPEN_FOLDER_BUTTON,
    IDC_CHAT_BLOCKER_IMAGE_COMBO,
    IDC_CHAT_BLOCKER_WIDTH_LABEL,
    IDC_CHAT_BLOCKER_WIDTH_EDIT,
    IDC_CHAT_BLOCKER_HEIGHT_LABEL,
    IDC_CHAT_BLOCKER_HEIGHT_EDIT,
    IDC_CHAT_BLOCKER_ANCHOR_LABEL,
    IDC_CHAT_BLOCKER_ANCHOR_COMBO,
    IDC_CHAT_PREVIEW_LABEL,
    IDC_CHAT_PREVIEW,
    IDC_CHAT_SAVE_SETTINGS,
    IDC_RECORDINGS_LABEL,
    IDC_RECORDINGS_LIST,
    IDC_RECORDINGS_REFRESH,
    IDC_RECORDINGS_OPEN_FOLDER,
    IDC_RECORDINGS_OPEN_DB_FOLDER,
    IDC_RECORDINGS_INFO_LABEL,
    IDC_RECORDINGS_INFO_TEXT,
    IDC_YOUTUBE_LABEL,
    IDC_YOUTUBE_MEDIA_LIST,
    IDC_YOUTUBE_REFRESH,
    IDC_YOUTUBE_UPLOAD_PROGRESS,
    IDC_YOUTUBE_UPLOAD_STATUS,
    IDC_YOUTUBE_LINK_BUTTON,
    IDC_YOUTUBE_UNLINK_BUTTON,
    IDC_YOUTUBE_UNLINK_CONFIRM_LABEL,
    IDC_YOUTUBE_UNLINK_YES_BUTTON,
    IDC_YOUTUBE_UNLINK_NO_BUTTON,
    IDC_YOUTUBE_LINK_STATUS,
    IDC_YOUTUBE_ACCOUNT_LABEL,
    IDC_YOUTUBE_ACCOUNT_LINK,
    IDC_YOUTUBE_TITLE_LABEL,
    IDC_YOUTUBE_TITLE_EDIT,
    IDC_YOUTUBE_PRIVACY_LABEL,
    IDC_YOUTUBE_PRIVACY_COMBO,
    IDC_YOUTUBE_UPLOAD_BUTTON,
    IDC_CONFIGURATION_TOOLTIP,
    IDC_ABOUT_WEBSITE_BUTTON,
    IDC_ABOUT_EMAIL_BUTTON,
    IDC_ABOUT_DISCORD_BUTTON,
    IDC_ABOUT_TITLE_LABEL,
    IDC_ABOUT_WEBSITE_LABEL,
    IDC_ABOUT_WEBSITE_TEXT,
    IDC_ABOUT_EMAIL_LABEL,
    IDC_ABOUT_EMAIL_TEXT,
    IDC_ABOUT_DISCORD_LABEL,
    IDC_ABOUT_DISCORD_TEXT,
    IDC_ABOUT_BUILD_LABEL,
    IDC_ABOUT_BUILD_TEXT,
    IDC_ABOUT_UPDATE_LABEL,
    IDC_ABOUT_UPDATE_TEXT,
    IDC_ABOUT_CHECK_UPDATES_BUTTON,
    IDC_ABOUT_FLAVOR_TEXT,
    IDC_CLIPS_SOURCE_LABEL,
    IDC_CLIPS_SOURCE_COMBO,
    IDC_CLIPS_REFRESH,
    IDC_CLIPS_VIDEO_SURFACE,
    IDC_CLIPS_PLAY_PAUSE,
    IDC_CLIPS_TIMELINE,
    IDC_CLIPS_POSITION_TEXT,
    IDC_CLIPS_VOLUME_LABEL,
    IDC_CLIPS_VOLUME_SLIDER,
    IDC_CLIPS_START_LABEL,
    IDC_CLIPS_START_EDIT,
    IDC_CLIPS_SET_START,
    IDC_CLIPS_END_LABEL,
    IDC_CLIPS_END_EDIT,
    IDC_CLIPS_SET_END,
    IDC_CLIPS_EXPORT,
    IDC_CLIPS_EXPORT_PRECISE,
    IDC_CLIPS_OPEN_FOLDER,
    IDC_CLIPS_FFMPEG_WARNING
};

struct AppContext {
    // Thread ownership (f9):
    // - UI thread owns HWND fields, settings edits, and list/combo state.
    // - Background workers started via LaunchAppWorker may only touch atomics
    //   and must PostMessage results; JoinAppWorkers runs in WM_DESTROY.
    // - Orchestrator / combat-log / trim workers are owned by bean_core and
    //   never hold raw AppContext pointers.

    bean::core::SettingsStore settingsStore;
    std::shared_ptr<bean::core::RunRepository> runRepository;
    bean::core::AppSettings settings;
    std::unique_ptr<bean::core::RecordingOrchestrator> orchestrator;
    HWND mainWindow = nullptr;

    HWND outputEdit = nullptr;
    HWND outputStatus = nullptr;
    HWND wowLogEdit = nullptr;
    HWND wowLogStatus = nullptr;
    HWND encoderCombo = nullptr;
    HWND presetCombo = nullptr;
    HWND presetHelpIcon = nullptr;
    HWND containerCombo = nullptr;
    HWND audioScopeCheck = nullptr;
    HWND audioScopeWowDiscordRadio = nullptr;
    HWND audioScopeAllRadio = nullptr;
    HWND microphoneCheck = nullptr;
    HWND microphoneNoiseSuppressionCheck = nullptr;
    HWND microphoneCombo = nullptr;
    HWND gameResolutionText = nullptr;
    HWND recordingResolutionCombo = nullptr;
    HWND fpsEdit = nullptr;
    HWND postRunDelayEdit = nullptr;
    HWND postRunDelayHelpIcon = nullptr;
    HWND clipDurationEdit = nullptr;
    HWND advancedLoggingHelpIcon = nullptr;
    HWND configurationTooltip = nullptr;
    HWND monitorIcon = nullptr;
    HWND recordIcon = nullptr;
    HWND wowWindowIcon = nullptr;
    HWND wowWindowText = nullptr;
    HWND obsInstallIcon = nullptr;
    HWND obsInstallText = nullptr;
    HWND ffmpegIcon = nullptr;
    HWND ffmpegText = nullptr;
    HWND warcraftRecorderIcon = nullptr;
    HWND warcraftRecorderText = nullptr;
    HWND advancedLoggingIcon = nullptr;
    HWND advancedLoggingText = nullptr;
    HWND lengthValue = nullptr;
    HWND statusText = nullptr;
    HWND statusTabButton = nullptr;
    HWND configurationTabButton = nullptr;
    HWND recordingsTabButton = nullptr;
    HWND youtubeTabButton = nullptr;
    HWND chatPrivacyTabButton = nullptr;
    HWND aboutTabButton = nullptr;
    HWND clipsTabButton = nullptr;
    HWND keybindsTabButton = nullptr;
    HWND customizeThemeCombo = nullptr;
    HWND statusPanel = nullptr;
    HWND recorderPanel = nullptr;
    HWND recordingsPanel = nullptr;
    HWND youtubePanel = nullptr;
    HWND chatPrivacyPanel = nullptr;
    HWND aboutPanel = nullptr;
    HWND clipsPanel = nullptr;
    HWND keybindsPanel = nullptr;
    std::array<HWND, 3> keybindValueLabels{};
    std::array<HWND, 3> keybindRebindButtons{};
    std::array<HWND, 3> keybindUnbindButtons{};
    std::optional<int> listeningKeybindIndex;
    HWND chatBlockerWidthEdit = nullptr;
    HWND chatBlockerHeightEdit = nullptr;
    HWND chatBlockerAnchorCombo = nullptr;
    HWND chatBlockerEnabledCheck = nullptr;
    HWND chatBlockerImageBlankRadio = nullptr;
    HWND chatBlockerImageCustomRadio = nullptr;
    HWND chatBlockerImageImportButton = nullptr;
    HWND chatBlockerImageOpenFolderButton = nullptr;
    HWND chatBlockerImageCombo = nullptr;
    HWND chatPreview = nullptr;
    HBITMAP chatPreviewFrameBitmap = nullptr;
    int chatPreviewFrameWidth = 0;
    int chatPreviewFrameHeight = 0;
    int chatPreviewSourceWidth = 1920;
    int chatPreviewSourceHeight = 1080;
    bool chatPreviewFrameValid = false;
    std::optional<std::chrono::steady_clock::time_point> chatPreviewLastCaptureAt;
    std::optional<std::chrono::steady_clock::time_point> chatPreviewLastInvalidateAt;
    HWND recordingsList = nullptr;
    HWND recordingsInfoLabel = nullptr;
    HWND recordingsLabel = nullptr;
    HWND recordingsInfoText = nullptr;
    HWND youtubeLabel = nullptr;
    HWND youtubeMediaList = nullptr;
    HWND youtubeUploadProgress = nullptr;
    HWND youtubeUploadStatus = nullptr;
    HWND youtubeLinkButton = nullptr;
    HWND youtubeUnlinkButton = nullptr;
    HWND youtubeUnlinkConfirmLabel = nullptr;
    HWND youtubeUnlinkYesButton = nullptr;
    HWND youtubeUnlinkNoButton = nullptr;
    HWND youtubeLinkStatus = nullptr;
    HWND youtubeAccountLabel = nullptr;
    HWND youtubeAccountLink = nullptr;
    HWND youtubeTitleEdit = nullptr;
    HWND youtubePrivacyCombo = nullptr;
    HWND youtubeUploadButton = nullptr;
    HWND clipsSourceCombo = nullptr;
    HWND clipsVideoSurface = nullptr;
    HWND clipsPlayPauseButton = nullptr;
    HWND clipsTimeline = nullptr;
    HWND clipsPositionText = nullptr;
    HWND clipsVolumeSlider = nullptr;
    HWND clipsStartEdit = nullptr;
    HWND clipsEndEdit = nullptr;
    HWND clipsFfmpegWarning = nullptr;
    std::unique_ptr<ClipPreviewEngine> clipsPreviewEngine;
    AppIconSet idleIcon;
    ITaskbarList3* taskbarList = nullptr;
    TaskbarOverlayIconSet taskbarOverlayIcons;
    TaskbarOverlayState activeTaskbarOverlayState = TaskbarOverlayState::Idle;
    bool outputAvailable = false;
    bool wowLogAvailable = false;
    bool isMonitoring = false;
    bool isRecording = false;
    bool autoRecordFailed = false;
    // Once set, RefreshLiveStatus keeps StartMonitoring alive with no user toggle.
    bool alwaysOnMonitoring = false;
    std::optional<std::chrono::steady_clock::time_point> monitoringLastStartAttemptAt;
    bool wowWindowDetected = false;
    bool wowBothInstancesDetected = false;
    bean::core::WowEdition detectedWowEdition = bean::core::WowEdition::Unknown;
    bool obsInstallDetected = false;
    bool ffmpegDetected = false;
    bool warcraftRecorderDetected = false;
    bool warcraftRecorderWarningLogged = false;
    bool advancedCombatLoggingEnabled = false;
    bool chatBlockerAutoSaveArmed = false;
    bool chatBlockerSettingsDirty = false;
    bool chatBlockerAspectAdjusting = false;
    bool chatBlockerIgnoreNextWidthChange = false;
    bool chatBlockerIgnoreNextHeightChange = false;
    int chatBlockerCustomSourceWidth = 0;
    int chatBlockerCustomSourceHeight = 0;
    bool configurationAutoSaveArmed = false;
    bool configurationSettingsDirty = false;
    bool outputFolderWillBeCreatedOnRecordStart = false;
    bool ffmpegCheckRequested = false;
    // Probing ffmpeg means launching it, so it runs on a worker thread and
    // reports back with WM_BEAN_FFMPEG_PROBE_COMPLETE.
    std::atomic<bool> ffmpegProbeInFlight{false};
    std::optional<std::chrono::steady_clock::time_point> wowWindowLastCheckedAt;
    int detectedWowClientWidth = 0;
    int detectedWowClientHeight = 0;
    std::optional<std::chrono::steady_clock::time_point> obsInstallLastCheckedAt;
    std::optional<std::chrono::steady_clock::time_point> ffmpegLastCheckedAt;
    std::optional<std::filesystem::path> ffmpegExecutablePath;
    std::optional<std::chrono::steady_clock::time_point> warcraftRecorderLastCheckedAt;
    std::optional<std::chrono::steady_clock::time_point> advancedCombatLoggingLastCheckedAt;
    std::optional<std::chrono::steady_clock::time_point> recordingStartedAt;
    std::wstring displayedRecordingLength = L"00:00:00";
    std::uint64_t activeRecordingSessionId = 0;
    struct RecordingItem {
        struct ParticipantUi {
            std::string guid;
            std::wstring name;
            std::wstring specAbbrev;
            std::optional<int> specId;
            std::optional<std::string> specName;
            std::optional<std::string> className;
            COLORREF classColor = kColorTextPrimary;
        };

        enum class Outcome {
            Unknown,
            Success,
            Failure
        };

        std::filesystem::path path;
        std::wstring fileName;
        std::wstring dungeonName = L"Unknown dungeon";
        std::wstring keystoneText = L"-";
        std::wstring durationText = L"--:--:--";
        std::wstring dateText;
        int keystoneLevel = -1;
        Outcome outcome = Outcome::Unknown;
        std::chrono::seconds duration = std::chrono::seconds::zero();
        std::filesystem::file_time_type modified{};
        std::vector<ParticipantUi> participants;
    };
    std::vector<RecordingItem> recordingItems;
    std::vector<YouTubeMediaFile> youtubeMediaItems;
    std::vector<COLORREF> visibleParticipantRowColors;
    int recordingsSelectedIndex = -1;
    int youtubeMediaSelectedIndex = -1;
    HIMAGELIST participantSpecIcons = nullptr;
    std::unordered_map<std::string, int> participantSpecIconIndexByKey;
    enum class RecordingSortColumn {
        Dungeon = 0,
        Keystone = 1,
        Duration = 2,
        Date = 3
    };
    RecordingSortColumn recordingSortColumn = RecordingSortColumn::Date;
    bool recordingSortAscending = false;
    enum class YouTubeSortColumn {
        Type = 0,
        Name = 1,
        Date = 2
    };
    YouTubeSortColumn youtubeSortColumn = YouTubeSortColumn::Date;
    bool youtubeSortAscending = false;
    std::atomic<bool> youtubeBusy{false};
    std::atomic<bool> aboutUpdateCheckInProgress{false};
    std::atomic<std::uint64_t> aboutUpdateCheckRequestId{0};
    // Kept on the UI thread so the About tab can advertise an update even
    // while its panel has never been opened.
    // Temporary Debug-build override for visually testing the indicator.
#ifdef _DEBUG
    bool aboutUpdateAvailable = true;
#else
    bool aboutUpdateAvailable = false;
#endif
    bool youtubeOAuthConfigured = false;
    bool youtubeLinked = false;
    bool youtubeUnlinkConfirmPending = false;
    std::wstring youtubeLastVideoUrl;
    RECT youtubeUploadLinkBounds{};
    bool clipsLoaded = false;
    bool clipsIsPlaying = false;
    bool clipsTimelineScrubbing = false;
    bool clipsTimelineDragActive = false;
    bool clipsVolumeDragActive = false;
    bool clipsResizeInProgress = false;
    enum class ClipExportStatus {
        Idle,
        Exporting,
        Success,
        Failure
    };
    ClipExportStatus clipsExportStatus = ClipExportStatus::Idle;
    std::atomic<bool> clipsExportInProgress{false};
    std::filesystem::path statusLogPath;
    std::filesystem::path clipsLoadedPath;
    std::vector<std::filesystem::path> clipSourceItems;
    int clipsDurationMs = 0;
    int clipsTimelinePosition = 0;
    int clipsVolumePercent = 100;
    int clipsVideoSourceWidth = 0;
    int clipsVideoSourceHeight = 0;
    std::ofstream statusLogStream;
    bool statusLogWriteFailed = false;
    // Authoritative status tab lines. SetStatus appends here instead of
    // reading the edit control back out and re-parsing it on every message.
    std::deque<std::wstring> statusLines;
    enum class MainTab {
        Status,
        Configuration,
        ChatPrivacy,
        Recordings,
        YouTube,
        Clips,
        Keybinds,
        About
    };
    MainTab activeTab = MainTab::Status;
    std::vector<MicrophoneOption> microphoneOptions;
    std::atomic<bool> shuttingDown{false};

    AppWorkerRegistry backgroundWorkers;
};

template <typename Payload>
bool PostOwnedAppMessage(
    AppContext* ctx,
    UINT message,
    Payload* payload,
    WPARAM wParam = 0)
{
    if (!ctx
        || ctx->shuttingDown.load(std::memory_order_acquire)
        || !ctx->mainWindow
        || !PostMessageW(
            ctx->mainWindow,
            message,
            wParam,
            reinterpret_cast<LPARAM>(payload))) {
        delete payload;
        return false;
    }
    return true;
}

inline bool PostBeanAppMessage(
    AppContext* ctx,
    UINT message,
    WPARAM wParam = 0,
    LPARAM lParam = 0)
{
    return ctx
        && !ctx->shuttingDown.load(std::memory_order_acquire)
        && ctx->mainWindow
        && PostMessageW(ctx->mainWindow, message, wParam, lParam) != FALSE;
}
