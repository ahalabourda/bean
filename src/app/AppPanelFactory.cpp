#include "app/AppPanelFactory.h"

#include "app/AppDraw.h"
#include "app/ClipPreviewEngine.h"
#include "bean_version.h"

void CreateAppPanels(AppContext* ctx, HWND mainWindow)
{
        constexpr int navWidth = 120;
        constexpr int navX = 12;
        constexpr int panelX = navX + navWidth + 18;
        constexpr int panelY = 16;
        constexpr int panelWidth = 780;
        constexpr int panelHeight = 460;
        constexpr int labelWidth = 120;
        constexpr int editWidth = 360;
        constexpr int buttonWidth = 100;
        constexpr int rowHeight = 24;
        constexpr int rowSpacing = 40;
        constexpr int sectionSpacing = 48;
        constexpr int xLabel = 20;
        constexpr int xEdit = 150;
        constexpr int xButton = 520;
        constexpr int xStatus = 630;
        int y = 20;

        ctx->statusTabButton = CreateWindowW(L"BUTTON", L"Status", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 20, navWidth, 32, mainWindow, reinterpret_cast<HMENU>(IDC_TAB_STATUS), nullptr, nullptr);
        ctx->configurationTabButton = CreateWindowW(L"BUTTON", L"Config", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 58, navWidth, 32, mainWindow, reinterpret_cast<HMENU>(IDC_TAB_CONFIGURATION), nullptr, nullptr);
        ctx->chatPrivacyTabButton = CreateWindowW(L"BUTTON", L"Chat Blocker", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 96, navWidth, 32, mainWindow, reinterpret_cast<HMENU>(IDC_TAB_CHAT_PRIVACY), nullptr, nullptr);
        ctx->recordingsTabButton = CreateWindowW(L"BUTTON", L"Recordings", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 134, navWidth, 32, mainWindow, reinterpret_cast<HMENU>(IDC_TAB_RECORDINGS), nullptr, nullptr);
        ctx->clipsTabButton = CreateWindowW(L"BUTTON", L"Clipmaker", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 172, navWidth, 32, mainWindow, reinterpret_cast<HMENU>(IDC_TAB_CLIPS), nullptr, nullptr);
        ctx->youtubeTabButton = CreateWindowW(L"BUTTON", L"YouTube", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 210, navWidth, 32, mainWindow, reinterpret_cast<HMENU>(IDC_TAB_YOUTUBE), nullptr, nullptr);
        ctx->keybindsTabButton = CreateWindowW(L"BUTTON", L"Customize", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 248, navWidth, 32, mainWindow, reinterpret_cast<HMENU>(IDC_TAB_KEYBINDS), nullptr, nullptr);
        ctx->aboutTabButton = CreateWindowW(L"BUTTON", L"About", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, navX, 286, navWidth, 32, mainWindow, reinterpret_cast<HMENU>(IDC_TAB_ABOUT), nullptr, nullptr);

        EnsureThemeResources();
        ctx->statusPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_VISIBLE | WS_CHILD, panelX, panelY, panelWidth, panelHeight, mainWindow, nullptr, nullptr, nullptr);
        ctx->recorderPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, mainWindow, nullptr, nullptr, nullptr);
        ctx->chatPrivacyPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, mainWindow, nullptr, nullptr, nullptr);
        ctx->recordingsPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, mainWindow, nullptr, nullptr, nullptr);
        ctx->youtubePanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, mainWindow, nullptr, nullptr, nullptr);
        ctx->clipsPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, mainWindow, nullptr, nullptr, nullptr);
        ctx->keybindsPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, mainWindow, nullptr, nullptr, nullptr);
        ctx->aboutPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD, panelX, panelY, panelWidth, panelHeight, mainWindow, nullptr, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Output Folder:", WS_VISIBLE | WS_CHILD, xLabel, y, labelWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_OUTPUT_LABEL), nullptr, nullptr);
        ctx->outputEdit = CreateBeanTextBox(ctx->recorderPanel, IDC_OUTPUT_EDIT, L"", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP, ctx);
        CreateWindowW(L"BUTTON", L"Browse", WS_VISIBLE | WS_CHILD | WS_TABSTOP, xButton, y, buttonWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_OUTPUT_BROWSE), nullptr, nullptr);
        ctx->outputStatus = CreateWindowW(L"STATIC", L"X", WS_VISIBLE | WS_CHILD | SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE, xStatus, y, 40, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_OUTPUT_STATUS), nullptr, nullptr);
        y += rowSpacing;

        CreateWindowW(L"STATIC", L"WoW Install:", WS_VISIBLE | WS_CHILD, xLabel, y, labelWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_LOG_LABEL), nullptr, nullptr);
        ctx->wowLogEdit = CreateBeanTextBox(ctx->recorderPanel, IDC_LOG_EDIT, L"", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP, ctx);
        CreateWindowW(L"BUTTON", L"Browse", WS_VISIBLE | WS_CHILD | WS_TABSTOP, xButton, y, buttonWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_LOG_BROWSE), nullptr, nullptr);
        ctx->wowLogStatus = CreateWindowW(L"STATIC", L"X", WS_VISIBLE | WS_CHILD | SS_CENTER | SS_CENTERIMAGE, xStatus, y, 40, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_LOG_STATUS), nullptr, nullptr);
        y += rowSpacing;

        CreateWindowW(L"STATIC", L"Video Encoder:", WS_VISIBLE | WS_CHILD, xLabel, y, labelWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_ENCODER_LABEL), nullptr, nullptr);
        ctx->encoderCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, xEdit, y, 230, 180, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_ENCODER_COMBO), nullptr, nullptr);
        SendMessageW(ctx->encoderCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GPU (Auto)"));
        SendMessageW(ctx->encoderCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"NVIDIA NVENC"));
        SendMessageW(ctx->encoderCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"AMD AMF"));
        SendMessageW(ctx->encoderCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Intel Quick Sync (QSV)"));
        SendMessageW(ctx->encoderCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"CPU x264"));
        y += rowSpacing;

        CreateWindowW(L"STATIC", L"Video Quality:", WS_VISIBLE | WS_CHILD, xLabel, y, labelWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_PRESET_LABEL), nullptr, nullptr);
        ctx->presetCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, xEdit, y, 180, 180, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_PRESET_COMBO), nullptr, nullptr);
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
            20,
            20,
            ctx->recorderPanel,
            reinterpret_cast<HMENU>(IDC_PRESET_HELP),
            nullptr,
            nullptr);
        CreateWindowW(L"STATIC", L"Container:", WS_VISIBLE | WS_CHILD, xLabel + 260, y, 80, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_CONTAINER_LABEL), nullptr, nullptr);
        ctx->containerCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, xLabel + 346, y, 120, 120, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_CONTAINER_COMBO), nullptr, nullptr);
        SendMessageW(ctx->containerCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"mkv"));
        SendMessageW(ctx->containerCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"mp4"));
        y += rowSpacing;

        CreateWindowW(L"STATIC", L"Audio Capture:", WS_VISIBLE | WS_CHILD, xLabel, y, labelWidth, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_AUDIO_SCOPE_LABEL), nullptr, nullptr);
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
            L"Record local mic",
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
            WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP,
            xEdit,
            y,
            486,
            180,
            ctx->recorderPanel,
            reinterpret_cast<HMENU>(IDC_MICROPHONE_COMBO),
            nullptr,
            nullptr);
        y += rowSpacing;
        CreateWindowW(L"STATIC", L"Output Scale:", WS_VISIBLE | WS_CHILD, xLabel, y, 120, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_RECORDING_RESOLUTION_LABEL), nullptr, nullptr);
        ctx->recordingResolutionCombo = CreateWindowW(
            L"COMBOBOX",
            L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP,
            xLabel + 130,
            y,
            290,
            180,
            ctx->recorderPanel,
            reinterpret_cast<HMENU>(IDC_RECORDING_RESOLUTION_COMBO),
            nullptr,
            nullptr);
        y += rowSpacing;
        CreateWindowW(L"STATIC", L"FPS:", WS_VISIBLE | WS_CHILD, xLabel, y, 40, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_FPS_LABEL), nullptr, nullptr);
        ctx->fpsEdit = CreateBeanTextBox(ctx->recorderPanel, IDC_FPS_EDIT, L"60", WS_VISIBLE | WS_CHILD | ES_NUMBER | WS_TABSTOP, ctx);
        y += rowSpacing;

        CreateWindowW(L"STATIC", L"Post-run tail (s):", WS_VISIBLE | WS_CHILD, xLabel, y, 104, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_POST_RUN_DELAY_LABEL), nullptr, nullptr);
        ctx->postRunDelayEdit = CreateBeanTextBox(ctx->recorderPanel, IDC_POST_RUN_DELAY_EDIT, L"30", WS_VISIBLE | WS_CHILD | ES_NUMBER | WS_TABSTOP, ctx);
        ctx->postRunDelayHelpIcon = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW | SS_NOTIFY, xLabel + 180, y + 2, 20, 20, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_POST_RUN_DELAY_HELP), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Clip duration (s):", WS_VISIBLE | WS_CHILD, xLabel + 230, y, 116, rowHeight, ctx->recorderPanel, reinterpret_cast<HMENU>(IDC_CLIP_DURATION_LABEL), nullptr, nullptr);
        ctx->clipDurationEdit = CreateBeanTextBox(ctx->recorderPanel, IDC_CLIP_DURATION_EDIT, L"30", WS_VISIBLE | WS_CHILD | ES_NUMBER | WS_TABSTOP, ctx);
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

        CreateWindowW(
            L"STATIC",
            L"Customize global hotkeys and the visual appearance of Bean.",
            WS_VISIBLE | WS_CHILD,
            20, 20, 700, rowHeight,
            ctx->keybindsPanel,
            reinterpret_cast<HMENU>(IDC_KEYBINDS_INFO),
            nullptr,
            nullptr);
        CreateWindowW(
            L"STATIC",
            L"Theme:",
            WS_VISIBLE | WS_CHILD,
            20, 54, 120, rowHeight,
            ctx->keybindsPanel,
            reinterpret_cast<HMENU>(IDC_CUSTOMIZE_THEME_LABEL),
            nullptr,
            nullptr);
        ctx->customizeThemeCombo = CreateWindowW(
            L"COMBOBOX",
            L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS
                | CBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
            150, 52, 280, 320,
            ctx->keybindsPanel,
            reinterpret_cast<HMENU>(IDC_CUSTOMIZE_THEME_COMBO),
            nullptr,
            nullptr);
        for (const auto& theme : kThemeDefinitions) {
            SendMessageW(ctx->customizeThemeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(theme.displayName));
        }
        CreateWindowW(
            L"STATIC",
            L"Settings auto-save as you make changes.",
            WS_VISIBLE | WS_CHILD | SS_CENTER,
            20, 220, panelWidth - 40, rowHeight,
            ctx->keybindsPanel,
            reinterpret_cast<HMENU>(IDC_KEYBINDS_AUTOSAVE_HINT),
            nullptr,
            nullptr);
        const wchar_t* keybindLabels[] = {L"Create clip", L"Start recording", L"Stop recording"};
        const int labelIds[] = {IDC_KEYBINDS_CREATE_CLIP_LABEL, IDC_KEYBINDS_MANUAL_START_LABEL, IDC_KEYBINDS_MANUAL_STOP_LABEL};
        const int valueIds[] = {IDC_KEYBINDS_CREATE_CLIP_VALUE, IDC_KEYBINDS_MANUAL_START_VALUE, IDC_KEYBINDS_MANUAL_STOP_VALUE};
        const int rebindIds[] = {IDC_KEYBINDS_CREATE_CLIP_REBIND, IDC_KEYBINDS_MANUAL_START_REBIND, IDC_KEYBINDS_MANUAL_STOP_REBIND};
        const int unbindIds[] = {IDC_KEYBINDS_CREATE_CLIP_UNBIND, IDC_KEYBINDS_MANUAL_START_UNBIND, IDC_KEYBINDS_MANUAL_STOP_UNBIND};
        const int resetIds[] = {IDC_KEYBINDS_CREATE_CLIP_RESET, IDC_KEYBINDS_MANUAL_START_RESET, IDC_KEYBINDS_MANUAL_STOP_RESET};
        for (int index = 0; index < 3; ++index) {
            const int rowY = 104 + index * 44;
            CreateWindowW(L"STATIC", keybindLabels[index], WS_VISIBLE | WS_CHILD,
                20, rowY, 160, rowHeight, ctx->keybindsPanel,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(labelIds[index])), nullptr, nullptr);
            ctx->keybindValueLabels[static_cast<size_t>(index)] = CreateWindowW(
                L"STATIC", L"Unbound", WS_VISIBLE | WS_CHILD | SS_CENTER | SS_CENTERIMAGE,
                190, rowY, 180, rowHeight, ctx->keybindsPanel,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(valueIds[index])), nullptr, nullptr);
            ctx->keybindRebindButtons[static_cast<size_t>(index)] = CreateWindowW(
                L"BUTTON", L"Rebind", WS_VISIBLE | WS_CHILD | WS_TABSTOP,
                390, rowY, 100, rowHeight + 2, ctx->keybindsPanel,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(rebindIds[index])), nullptr, nullptr);
            ctx->keybindUnbindButtons[static_cast<size_t>(index)] = CreateWindowW(
                L"BUTTON", L"Unbind", WS_VISIBLE | WS_CHILD | WS_TABSTOP,
                500, rowY, 100, rowHeight + 2, ctx->keybindsPanel,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(unbindIds[index])), nullptr, nullptr);
            CreateWindowW(
                L"BUTTON", L"Reset", WS_VISIBLE | WS_CHILD | WS_TABSTOP,
                610, rowY, 100, rowHeight + 2, ctx->keybindsPanel,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(resetIds[index])), nullptr, nullptr);
        }
        y += rowSpacing;

        CreateWindowW(L"BUTTON", L"Start Recording", WS_VISIBLE | WS_CHILD, xLabel, y, 140, rowHeight + 4, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_RECORD_START), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Stop Recording", WS_VISIBLE | WS_CHILD, xLabel + 150, y, 140, rowHeight + 4, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_RECORD_STOP), nullptr, nullptr);
        y += 44;

        CreateWindowW(L"STATIC", L"Live Status:", WS_VISIBLE | WS_CHILD, xLabel, y, 90, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_LIVE_LABEL), nullptr, nullptr);
        ctx->monitorIcon = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, xLabel + 105, y + 5, 14, 14, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_MONITOR_ICON), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Monitoring", WS_VISIBLE | WS_CHILD, xLabel + 125, y, 92, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_MONITOR_TEXT), nullptr, nullptr);
        ctx->recordIcon = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, xLabel + 225, y + 5, 14, 14, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_RECORD_ICON), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Recording", WS_VISIBLE | WS_CHILD, xLabel + 245, y, 80, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_RECORD_TEXT), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Length:", WS_VISIBLE | WS_CHILD, xLabel + 345, y, 60, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_LENGTH_LABEL), nullptr, nullptr);
        ctx->lengthValue = CreateWindowW(L"STATIC", L"00:00:00", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, xLabel + 410, y, 110, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_LENGTH_VALUE), nullptr, nullptr);
        y += 34;

        CreateWindowW(L"STATIC", L"WoW:", WS_VISIBLE | WS_CHILD, xLabel, y, 90, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_WOW_WINDOW_LABEL), nullptr, nullptr);
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
            20,
            20,
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
        y += 36;

        CreateWindowW(
            L"STATIC",
            L"Disk Space:",
            WS_VISIBLE | WS_CHILD,
            420,
            118,
            100,
            rowHeight,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_DISK_SPACE_LABEL),
            nullptr,
            nullptr);
        ctx->diskSpaceIcon = CreateWindowW(
            L"STATIC",
            L"X",
            WS_VISIBLE | WS_CHILD | SS_OWNERDRAW,
            522,
            118,
            20,
            rowHeight,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_DISK_SPACE_ICON),
            nullptr,
            nullptr);
        ctx->diskSpaceText = CreateWindowW(
            L"STATIC",
            L"Checking...",
            WS_VISIBLE | WS_CHILD,
            546,
            118,
            220,
            rowHeight,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_DISK_SPACE_TEXT),
            nullptr,
            nullptr);
        ctx->diskSpaceHelpIcon = CreateWindowW(
            L"STATIC",
            L"",
            WS_VISIBLE | WS_CHILD | SS_OWNERDRAW | SS_NOTIFY,
            770,
            122,
            20,
            20,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_DISK_SPACE_HELP),
            nullptr,
            nullptr);

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
            rowHeight + 4,
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
            rowHeight + 4,
            ctx->chatPrivacyPanel,
            reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_IMAGE_OPEN_FOLDER_BUTTON),
            nullptr,
            nullptr);
        ctx->chatBlockerImageCombo = CreateWindowW(
            L"COMBOBOX",
            L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP,
            134,
            88,
            464,
            180,
            ctx->chatPrivacyPanel,
            reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_IMAGE_COMBO),
            nullptr,
            nullptr);
        CreateWindowW(L"STATIC", L"Width:", WS_VISIBLE | WS_CHILD, 20, 122, 120, rowHeight, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_WIDTH_LABEL), nullptr, nullptr);
        ctx->chatBlockerWidthEdit = CreateBeanTextBox(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_WIDTH_EDIT, L"500", WS_VISIBLE | WS_CHILD | ES_NUMBER | WS_TABSTOP, ctx);
        CreateWindowW(L"STATIC", L"Height:", WS_VISIBLE | WS_CHILD, 238, 122, 120, rowHeight, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_HEIGHT_LABEL), nullptr, nullptr);
        ctx->chatBlockerHeightEdit = CreateBeanTextBox(ctx->chatPrivacyPanel, IDC_CHAT_BLOCKER_HEIGHT_EDIT, L"300", WS_VISIBLE | WS_CHILD | ES_NUMBER | WS_TABSTOP, ctx);
        CreateWindowW(L"STATIC", L"Anchor Corner:", WS_VISIBLE | WS_CHILD, 20, 156, 110, rowHeight, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_ANCHOR_LABEL), nullptr, nullptr);
        ctx->chatBlockerAnchorCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, 134, 156, 180, 120, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_BLOCKER_ANCHOR_COMBO), nullptr, nullptr);
        SendMessageW(ctx->chatBlockerAnchorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom Left"));
        SendMessageW(ctx->chatBlockerAnchorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom Right"));
        SendMessageW(ctx->chatBlockerAnchorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top Left"));
        SendMessageW(ctx->chatBlockerAnchorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top Right"));
        SendMessageW(ctx->chatBlockerAnchorCombo, CB_SETCURSEL, 0, 0);
        CreateWindowW(L"STATIC", L"WoW Live Preview (overlay area marks chat blocker):", WS_VISIBLE | WS_CHILD, 20, 194, 720, rowHeight, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_PREVIEW_LABEL), nullptr, nullptr);
        ctx->chatPreview = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | SS_OWNERDRAW, 20, 222, 740, 222, ctx->chatPrivacyPanel, reinterpret_cast<HMENU>(IDC_CHAT_PREVIEW), nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Status:", WS_VISIBLE | WS_CHILD, xLabel, y, 60, rowHeight, ctx->statusPanel, reinterpret_cast<HMENU>(IDC_STATUS_LABEL), nullptr, nullptr);
        ctx->statusText = CreateBeanTextBox(
            ctx->statusPanel,
            IDC_STATUS_TEXT,
            L"",
            WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            ctx);
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
        CreateWindowW(
            L"BUTTON",
            L"Copy Log to Clipboard",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP,
            300,
            y + rowHeight + 44,
            200,
            rowHeight + 4,
            ctx->statusPanel,
            reinterpret_cast<HMENU>(IDC_STATUS_COPY_LOG_TEXT),
            nullptr,
            nullptr);

        ctx->recordingsLabel = CreateWindowW(L"STATIC", L"Folder:", WS_VISIBLE | WS_CHILD, 20, 20, 740, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_LABEL), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Refresh", WS_VISIBLE | WS_CHILD, 20, 52, 100, rowHeight + 4, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_REFRESH), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open Folder", WS_VISIBLE | WS_CHILD, 130, 52, 120, rowHeight + 4, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_OPEN_FOLDER), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open DB Folder", WS_VISIBLE | WS_CHILD, 260, 52, 130, rowHeight + 4, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_OPEN_DB_FOLDER), nullptr, nullptr);
        ctx->recordingsList = CreateBeanFileList(
            ctx->recordingsPanel,
            IDC_RECORDINGS_LIST,
            ctx,
            BeanFileListKind::Recordings);
        ctx->recordingsInfoLabel = CreateWindowW(L"STATIC", L"Characters", WS_VISIBLE | WS_CHILD, 532, 90, 228, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_INFO_LABEL), nullptr, nullptr);
        ctx->recordingsInfoText = CreateBeanFileList(
            ctx->recordingsPanel,
            IDC_RECORDINGS_INFO_TEXT,
            ctx,
            BeanFileListKind::Participants);

        CreateWindowW(L"STATIC", L"Type", WS_VISIBLE | WS_CHILD, 532, 344, 228, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_FILTER_TYPE_LABEL), nullptr, nullptr);
        ctx->recordingsFilterTypeManualCheck = CreateWindowW(
            L"BUTTON",
            L"Manual",
            WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
            532,
            368,
            108,
            rowHeight,
            ctx->recordingsPanel,
            reinterpret_cast<HMENU>(IDC_RECORDINGS_FILTER_TYPE_MANUAL),
            nullptr,
            nullptr);
        ctx->recordingsFilterTypeMythicCheck = CreateWindowW(
            L"BUTTON",
            L"M+",
            WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
            646,
            368,
            108,
            rowHeight,
            ctx->recordingsPanel,
            reinterpret_cast<HMENU>(IDC_RECORDINGS_FILTER_TYPE_MYTHIC),
            nullptr,
            nullptr);
        ctx->recordingsFilterTypeRaidCheck = CreateWindowW(
            L"BUTTON",
            L"Raid",
            WS_VISIBLE | WS_CHILD | WS_DISABLED | BS_AUTOCHECKBOX | WS_TABSTOP,
            532,
            394,
            108,
            rowHeight,
            ctx->recordingsPanel,
            reinterpret_cast<HMENU>(IDC_RECORDINGS_FILTER_TYPE_RAID),
            nullptr,
            nullptr);
        ctx->recordingsFilterTypePvpCheck = CreateWindowW(
            L"BUTTON",
            L"PvP",
            WS_VISIBLE | WS_CHILD | WS_DISABLED | BS_AUTOCHECKBOX | WS_TABSTOP,
            646,
            394,
            108,
            rowHeight,
            ctx->recordingsPanel,
            reinterpret_cast<HMENU>(IDC_RECORDINGS_FILTER_TYPE_PVP),
            nullptr,
            nullptr);
        SendMessageW(ctx->recordingsFilterTypeManualCheck, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(ctx->recordingsFilterTypeMythicCheck, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(ctx->recordingsFilterTypeRaidCheck, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(ctx->recordingsFilterTypePvpCheck, BM_SETCHECK, BST_CHECKED, 0);
        CreateWindowW(L"STATIC", L"Timed", WS_VISIBLE | WS_CHILD, 532, 428, 228, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_FILTER_TIMED_LABEL), nullptr, nullptr);
        ctx->recordingsFilterTimedCombo = CreateWindowW(
            L"COMBOBOX",
            L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP,
            532,
            452,
            228,
            120,
            ctx->recordingsPanel,
            reinterpret_cast<HMENU>(IDC_RECORDINGS_FILTER_TIMED_COMBO),
            nullptr,
            nullptr);
        SendMessageW(ctx->recordingsFilterTimedCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Any"));
        SendMessageW(ctx->recordingsFilterTimedCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Timed"));
        SendMessageW(ctx->recordingsFilterTimedCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Depleted"));
        SendMessageW(ctx->recordingsFilterTimedCombo, CB_SETCURSEL, 0, 0);
        CreateWindowW(L"STATIC", L"Key level", WS_VISIBLE | WS_CHILD, 532, 486, 228, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_FILTER_KEY_LABEL), nullptr, nullptr);
        ctx->recordingsFilterKeyEdit = CreateBeanTextBox(
            ctx->recordingsPanel,
            IDC_RECORDINGS_FILTER_KEY_EDIT,
            L"",
            WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
            ctx);
        CreateWindowW(L"STATIC", L"Names", WS_VISIBLE | WS_CHILD, 532, 534, 228, rowHeight, ctx->recordingsPanel, reinterpret_cast<HMENU>(IDC_RECORDINGS_FILTER_CHARS_LABEL), nullptr, nullptr);
        ctx->recordingsFilterCharsEdit = CreateBeanTextBox(
            ctx->recordingsPanel,
            IDC_RECORDINGS_FILTER_CHARS_EDIT,
            L"",
            WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
            ctx);

        ctx->youtubeLabel = CreateWindowW(L"STATIC", L"Recordings and clips:", WS_VISIBLE | WS_CHILD, 20, 58, 740, rowHeight, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_LABEL), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Refresh", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 664, 57, 96, rowHeight + 4, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_REFRESH), nullptr, nullptr);
        ctx->youtubeMediaList = CreateBeanFileList(
            ctx->youtubePanel,
            IDC_YOUTUBE_MEDIA_LIST,
            ctx,
            BeanFileListKind::YouTube);

        ctx->youtubeLinkButton = CreateWindowW(L"BUTTON", L"Link YouTube", WS_VISIBLE | WS_CHILD, 540, 20, 110, rowHeight + 4, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_LINK_BUTTON), nullptr, nullptr);
        ctx->youtubeUnlinkButton = CreateWindowW(L"BUTTON", L"Unlink Account", WS_CHILD, 652, 20, 108, rowHeight + 4, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_UNLINK_BUTTON), nullptr, nullptr);
        ctx->youtubeUnlinkConfirmLabel = CreateWindowW(L"STATIC", L"You sure?", WS_CHILD, 540, 50, 100, rowHeight, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_UNLINK_CONFIRM_LABEL), nullptr, nullptr);
        ctx->youtubeUnlinkYesButton = CreateWindowW(L"BUTTON", L"Yes", WS_CHILD, 644, 48, 54, rowHeight + 4, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_UNLINK_YES_BUTTON), nullptr, nullptr);
        ctx->youtubeUnlinkNoButton = CreateWindowW(L"BUTTON", L"No", WS_CHILD, 702, 48, 54, rowHeight + 4, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_UNLINK_NO_BUTTON), nullptr, nullptr);
        ctx->youtubeLinkStatus = CreateWindowW(L"STATIC", L"Not linked", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW, 540, 50, 24, rowHeight, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_LINK_STATUS), nullptr, nullptr);
        ctx->youtubeAccountLabel = CreateWindowW(L"STATIC", L"YouTube Account:", WS_VISIBLE | WS_CHILD, 20, 20, 500, rowHeight, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_ACCOUNT_LABEL), nullptr, nullptr);
        ctx->youtubeAccountLink = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 170, 16, 350, rowHeight + 8, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_ACCOUNT_LINK), nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Title:", WS_VISIBLE | WS_CHILD, 20, 20, 120, rowHeight, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_TITLE_LABEL), nullptr, nullptr);
        ctx->youtubeTitleEdit = CreateBeanTextBox(ctx->youtubePanel, IDC_YOUTUBE_TITLE_EDIT, L"", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, ctx);
        CreateWindowW(L"STATIC", L"Visibility:", WS_VISIBLE | WS_CHILD, 540, 20, 70, rowHeight, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_PRIVACY_LABEL), nullptr, nullptr);
        ctx->youtubePrivacyCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, 612, 20, 148, 120, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_PRIVACY_COMBO), nullptr, nullptr);
        SendMessageW(ctx->youtubePrivacyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"private"));
        SendMessageW(ctx->youtubePrivacyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"unlisted"));
        SendMessageW(ctx->youtubePrivacyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"public"));
        SendMessageW(ctx->youtubePrivacyCombo, CB_SETCURSEL, 0, 0);
        SendMessageW(ctx->youtubePrivacyCombo, CB_SETMINVISIBLE, 8, 0);
        ctx->youtubeUploadButton = CreateWindowW(L"BUTTON", L"Upload to YouTube", WS_VISIBLE | WS_CHILD, 540, 424, 220, rowHeight + 4, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_UPLOAD_BUTTON), nullptr, nullptr);
        ctx->youtubeUploadProgress = CreateWindowW(PROGRESS_CLASSW, L"", WS_VISIBLE | WS_CHILD, 20, 426, 500, 20, ctx->youtubePanel, reinterpret_cast<HMENU>(IDC_YOUTUBE_UPLOAD_PROGRESS), nullptr, nullptr);
        SendMessageW(ctx->youtubeUploadProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(ctx->youtubeUploadProgress, PBM_SETPOS, 0, 0);
        ctx->youtubeUploadStatus = CreateWindowW(
            L"STATIC",
            L"No upload in progress.",
            WS_VISIBLE | WS_CHILD | SS_OWNERDRAW | SS_NOTIFY,
            20,
            448,
            740,
            rowHeight,
            ctx->youtubePanel,
            reinterpret_cast<HMENU>(IDC_YOUTUBE_UPLOAD_STATUS),
            nullptr,
            nullptr);

        CreateWindowW(L"STATIC", L"Source Recording:", WS_VISIBLE | WS_CHILD, 20, 20, 110, rowHeight, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_SOURCE_LABEL), nullptr, nullptr);
        ctx->clipsSourceCombo = CreateWindowW(
            L"COMBOBOX",
            L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP,
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
            ClipPreviewEngine::VideoHostWindowClass(),
            L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPCHILDREN,
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
        }
        CreateWindowW(L"STATIC", L"Start:", WS_VISIBLE | WS_CHILD, 20, 398, 50, rowHeight, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_START_LABEL), nullptr, nullptr);
        ctx->clipsStartEdit = CreateBeanTextBox(
            ctx->clipsPanel,
            IDC_CLIPS_START_EDIT,
            L"00:00",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP,
            ctx);
        CreateWindowW(L"BUTTON", L"Set Start", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 156, 397, 94, rowHeight + 4, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_SET_START), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"End:", WS_VISIBLE | WS_CHILD, 268, 398, 40, rowHeight, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_END_LABEL), nullptr, nullptr);
        ctx->clipsEndEdit = CreateBeanTextBox(
            ctx->clipsPanel,
            IDC_CLIPS_END_EDIT,
            L"00:00",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP,
            ctx);
        CreateWindowW(L"BUTTON", L"Set End", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 394, 397, 94, rowHeight + 4, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_SET_END), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Export Clip (Fast)", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 20, 431, 150, rowHeight + 4, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_EXPORT), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Export Clip (Precise)", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 180, 431, 170, rowHeight + 4, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_EXPORT_PRECISE), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open Folder", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 360, 431, 110, rowHeight + 4, ctx->clipsPanel, reinterpret_cast<HMENU>(IDC_CLIPS_OPEN_FOLDER), nullptr, nullptr);
        ctx->clipsFfmpegWarning = CreateWindowW(
            L"STATIC",
            L"FFmpeg is required to export clips.",
            WS_CHILD | SS_LEFT,
            480,
            431,
            280,
            rowHeight + 4,
            ctx->clipsPanel,
            reinterpret_cast<HMENU>(IDC_CLIPS_FFMPEG_WARNING),
            nullptr,
            nullptr);
        ctx->clipsTimelinePosition = 0;
        ctx->clipsVolumePercent = 100;

        const std::wstring versionText = std::wstring(L"v") + BEAN_APP_VERSION_W;
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
        CreateWindowW(L"STATIC", kAboutFlavorText, WS_VISIBLE | WS_CHILD | SS_CENTER, 20, 290, 740, rowHeight, ctx->aboutPanel, reinterpret_cast<HMENU>(IDC_ABOUT_FLAVOR_TEXT), nullptr, nullptr);

}
