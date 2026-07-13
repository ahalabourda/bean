#pragma once

#include "core/RecordingOrchestrator.h"
#include "core/RunRepository.h"
#include "core/SettingsStore.h"

#include <windows.h>
#include <shobjidl.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct IGraphBuilder;
struct IMediaControl;
struct IMediaSeeking;
struct IVideoWindow;
struct IBasicAudio;
struct IBasicVideo;

inline constexpr wchar_t kWindowClassName[] = L"BeanMainWindow";
inline constexpr wchar_t kWindowTitleBase[] = L"Battle Encounter Archival Nexus - WoW Recorder";
inline constexpr wchar_t kAboutTitleText[] = L"Battle Encounter Archival Nexus";
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
inline constexpr UINT WM_BEAN_CLIPS_PREVIEW_READY = WM_APP + 106;
inline constexpr UINT WM_BEAN_CLIPS_EXPORT_COMPLETE = WM_APP + 107;
inline constexpr wchar_t kStatusLogFilePrefix[] = L"bean-status-log-";
inline constexpr wchar_t kStatusLogFileExtension[] = L".log";
inline constexpr size_t kStatusLogRetentionCount = 5;
inline constexpr wchar_t kYouTubeOAuthCredentialsMissingMessage[] =
    L"YouTube auth server is unavailable. Please try again later.";
inline constexpr UINT_PTR kLiveStatusTimerId = 1;
inline constexpr UINT_PTR kClipsExportStatusTimerId = 2;
inline constexpr UINT kLiveStatusIntervalMs = 500;
inline constexpr auto kChatPreviewCaptureInterval = std::chrono::milliseconds(1000);
inline constexpr auto kChatPreviewInvalidateInterval = std::chrono::milliseconds(1000);
inline constexpr auto kWowWindowPollInterval = std::chrono::seconds(2);
inline constexpr auto kObsInstallPollInterval = std::chrono::seconds(2);
inline constexpr auto kWarcraftRecorderPollInterval = std::chrono::seconds(30);
inline constexpr size_t kStatusMaxLines = 300;
inline constexpr COLORREF kColorWindowTop = RGB(11, 14, 23);
inline constexpr COLORREF kColorWindowBottom = RGB(5, 7, 14);
inline constexpr COLORREF kColorPanelTop = RGB(31, 36, 52);
inline constexpr COLORREF kColorPanelBottom = RGB(18, 21, 34);
inline constexpr COLORREF kColorPanelBorder = RGB(72, 86, 122);
inline constexpr COLORREF kColorTextPrimary = RGB(228, 234, 246);
inline constexpr COLORREF kColorTextMuted = RGB(165, 176, 203);
inline constexpr COLORREF kColorInputBg = RGB(17, 21, 33);
inline constexpr COLORREF kColorInputBorder = RGB(66, 79, 113);
inline constexpr COLORREF kColorButtonBg = RGB(48, 59, 86);
inline constexpr COLORREF kColorButtonText = RGB(235, 241, 255);
inline constexpr COLORREF kColorTooltipBg = RGB(22, 27, 40);
inline constexpr COLORREF kColorTooltipText = RGB(233, 239, 251);
inline constexpr COLORREF kColorListSelection = RGB(55, 76, 124);
inline constexpr COLORREF kColorListRow = RGB(12, 16, 25);
inline constexpr COLORREF kColorListRowAlt = RGB(16, 21, 32);
inline constexpr COLORREF kColorListGrid = RGB(41, 50, 71);
inline constexpr COLORREF kColorSuccess = RGB(80, 214, 142);
inline constexpr COLORREF kColorFailure = RGB(241, 100, 125);
inline constexpr COLORREF kColorWarning = RGB(241, 204, 96);
inline constexpr int kMinClientWidth = 930;
inline constexpr int kMinClientHeight = 560;
inline constexpr int kSpecIconSizePx = 16;
inline constexpr int kSpecIconCanvasSizePx = 18;
inline constexpr int kSpecIconVerticalOffsetPx = 1;

struct VisualTheme {
    HFONT uiFont = nullptr;
    HFONT mutedHintFont = nullptr;
    HFONT statusIndicatorFont = nullptr;
    HFONT recordingsFont = nullptr;
    HFONT headingFont = nullptr;
    HBRUSH inputBrush = nullptr;
    HBRUSH buttonBrush = nullptr;
    HBRUSH panelSolidBrush = nullptr;
    HBRUSH panelBorderBrush = nullptr;
    HBRUSH tooltipBrush = nullptr;
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
    IDC_WIDTH_LABEL,
    IDC_WIDTH_EDIT,
    IDC_HEIGHT_LABEL,
    IDC_HEIGHT_EDIT,
    IDC_FPS_LABEL,
    IDC_FPS_EDIT,
    IDC_POST_RUN_DELAY_LABEL,
    IDC_POST_RUN_DELAY_HELP,
    IDC_ADVANCED_LOGGING_HELP,
    IDC_POST_RUN_DELAY_EDIT,
    IDC_CONFIGURATION_AUTOSAVE_HINT,
    IDC_MONITOR_START,
    IDC_MONITOR_STOP,
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
    IDC_TAB_STATUS,
    IDC_TAB_CONFIGURATION,
    IDC_TAB_CHAT_PRIVACY,
    IDC_TAB_RECORDINGS,
    IDC_TAB_CLIPS,
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
    IDC_CHAT_PREVIEW_STATUS,
    IDC_CHAT_PREVIEW,
    IDC_CHAT_SAVE_SETTINGS,
    IDC_RECORDINGS_LABEL,
    IDC_RECORDINGS_LIST,
    IDC_RECORDINGS_REFRESH,
    IDC_RECORDINGS_OPEN_FOLDER,
    IDC_RECORDINGS_OPEN_DB_FOLDER,
    IDC_RECORDINGS_INFO_LABEL,
    IDC_RECORDINGS_INFO_TEXT,
    IDC_RECORDINGS_UPLOAD_PROGRESS,
    IDC_RECORDINGS_UPLOAD_STATUS,
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
    IDC_CLIPS_OPEN_FOLDER,
    IDC_CLIPS_FFMPEG_WARNING
};

struct AppContext {
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
    HWND widthEdit = nullptr;
    HWND heightEdit = nullptr;
    HWND fpsEdit = nullptr;
    HWND postRunDelayEdit = nullptr;
    HWND postRunDelayHelpIcon = nullptr;
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
    HWND chatPrivacyTabButton = nullptr;
    HWND aboutTabButton = nullptr;
    HWND clipsTabButton = nullptr;
    HWND statusPanel = nullptr;
    HWND recorderPanel = nullptr;
    HWND recordingsPanel = nullptr;
    HWND chatPrivacyPanel = nullptr;
    HWND aboutPanel = nullptr;
    HWND clipsPanel = nullptr;
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
    HWND chatPreviewStatus = nullptr;
    HBITMAP chatPreviewFrameBitmap = nullptr;
    int chatPreviewFrameWidth = 0;
    int chatPreviewFrameHeight = 0;
    int chatPreviewSourceWidth = 1920;
    int chatPreviewSourceHeight = 1080;
    bool chatPreviewFrameValid = false;
    std::optional<std::chrono::steady_clock::time_point> chatPreviewLastCaptureAt;
    std::optional<std::chrono::steady_clock::time_point> chatPreviewLastInvalidateAt;
    HWND recordingsList = nullptr;
    HWND recordingsListHeader = nullptr;
    HWND recordingsInfoLabel = nullptr;
    HWND recordingsLabel = nullptr;
    HWND recordingsInfoText = nullptr;
    HWND recordingsUploadProgress = nullptr;
    HWND recordingsUploadStatus = nullptr;
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
    IGraphBuilder* clipsGraph = nullptr;
    IMediaControl* clipsMediaControl = nullptr;
    IMediaSeeking* clipsMediaSeeking = nullptr;
    IVideoWindow* clipsVideoWindow = nullptr;
    IBasicAudio* clipsBasicAudio = nullptr;
    IBasicVideo* clipsBasicVideo = nullptr;
    AppIconSet idleIcon;
    ITaskbarList3* taskbarList = nullptr;
    TaskbarOverlayIconSet taskbarOverlayIcons;
    TaskbarOverlayState activeTaskbarOverlayState = TaskbarOverlayState::Idle;
    bool outputAvailable = false;
    bool wowLogAvailable = false;
    bool isMonitoring = false;
    bool isRecording = false;
    bool wowWindowDetected = false;
    bool obsInstallDetected = false;
    bool ffmpegDetected = false;
    bool warcraftRecorderDetected = false;
    bool warcraftRecorderWarningLogged = false;
    bool advancedCombatLoggingEnabled = false;
    bool chatBlockerAutoSaveArmed = false;
    bool chatBlockerAspectAdjusting = false;
    bool chatBlockerIgnoreNextWidthChange = false;
    bool chatBlockerIgnoreNextHeightChange = false;
    int chatBlockerCustomSourceWidth = 0;
    int chatBlockerCustomSourceHeight = 0;
    bool configurationAutoSaveArmed = false;
    bool outputFolderWillBeCreatedOnRecordStart = false;
    std::optional<std::chrono::steady_clock::time_point> wowWindowLastCheckedAt;
    std::optional<std::chrono::steady_clock::time_point> obsInstallLastCheckedAt;
    std::optional<std::chrono::steady_clock::time_point> ffmpegLastCheckedAt;
    std::optional<std::filesystem::path> ffmpegExecutablePath;
    std::optional<std::chrono::steady_clock::time_point> warcraftRecorderLastCheckedAt;
    std::optional<std::chrono::steady_clock::time_point> advancedCombatLoggingLastCheckedAt;
    std::optional<std::chrono::steady_clock::time_point> recordingStartedAt;
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
    std::vector<COLORREF> visibleParticipantRowColors;
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
    std::atomic<bool> youtubeBusy{false};
    bool youtubeOAuthConfigured = false;
    bool youtubeLinked = false;
    bool youtubeUnlinkConfirmPending = false;
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
    std::atomic<bool> clipsPreviewBuildInProgress{false};
    std::atomic<std::uint64_t> clipsPreviewRequestId{0};
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
    enum class MainTab {
        Status,
        Configuration,
        ChatPrivacy,
        Recordings,
        Clips,
        About
    };
    MainTab activeTab = MainTab::Status;
    std::vector<MicrophoneOption> microphoneOptions;
};
