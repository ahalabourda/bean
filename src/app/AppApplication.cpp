#include "app/AppApplication.h"

#include "app/AppContext.h"
#include "app/AppIconsTaskbar.h"
#include "app/AppProbeController.h"
#include "app/AppStartupSettings.h"
#include "app/AppStatusLog.h"
#include "app/AppTheme.h"
#include "app/AppYouTubeController.h"
#include "app/BeanUpdater.h"
#include "bean_version.h"
#include "core/RunRepository.h"
#include "core/RecordingOrchestrator.h"
#include "obs/IRecorderEngine.h"
#if defined(BEAN_ENABLE_LIBOBS) && BEAN_ENABLE_LIBOBS
#include "obs/LibObsRecorderEngine.h"
#else
#include "obs/MockRecorderEngine.h"
#endif
#include "util/Strings.h"

#include <commctrl.h>
#include <string>

using bean::util::ToWide;

namespace {
std::wstring MainWindowTitleText()
{
    return std::wstring(kWindowTitleBase) + L" - v" + BEAN_APP_VERSION_W;
}
}

int RunApplication(
    HINSTANCE instance,
    int cmdShow,
    WNDPROC windowProc,
    void (*refreshAboutUpdate)(AppContext*))
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

#if defined(BEAN_ENABLE_LIBOBS) && BEAN_ENABLE_LIBOBS
    auto recorderEngine = std::unique_ptr<bean::obs::IRecorderEngine>(
        std::make_unique<bean::obs::LibObsRecorderEngine>());
#else
    auto recorderEngine = std::unique_ptr<bean::obs::IRecorderEngine>(
        std::make_unique<bean::obs::MockRecorderEngine>());
#endif
    auto orchestrator = std::make_unique<bean::core::RecordingOrchestrator>(std::move(recorderEngine));
    orchestrator->SetRunRepository(runRepository);
    orchestrator->ApplySettings(settings);

    AppContext context;
    context.settingsStore = settingsStore;
    context.runRepository = runRepository;
    context.settings = settings;
    context.orchestrator = std::move(orchestrator);
    InitializeAppIcons(&context);

    context.orchestrator->SetStatusCallback([&context](const std::string& status) {
        PostStatus(&context, ToWide(status));
        if (status.rfind("Clip created:", 0) == 0) {
            PostBeanAppMessage(&context, WM_BEAN_CLIPS_UI_REFRESH);
        }
    });

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
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
        settings.windowWidth,
        settings.windowHeight,
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
    // Check in the background as soon as the UI is ready so update notices do
    // not depend on the user opening the About tab.
    if (refreshAboutUpdate) {
        refreshAboutUpdate(&context);
    }
    // Taskbar overlay icons are ignored until the window has a taskbar button.
    ApplyTaskbarOverlayState(&context, true);

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
