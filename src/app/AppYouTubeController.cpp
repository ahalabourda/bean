#include "app/AppYouTubeController.h"

#include "app/AppDraw.h"
#include "app/AppLiveStatus.h"
#include "app/AppProbeController.h"
#include "app/AppRecordingHelpers.h"
#include "app/AppStatusLog.h"
#include "app/AppUtilities.h"
#include "app/AppYouTube.h"
#include "integrations/YouTubeUploader.h"
#include "util/Strings.h"

#include <algorithm>
#include <cstdint>
#include <string>

using bean::util::ToUtf8;
using bean::util::ToWide;

constexpr char kYouTubeAuthServerUrl[] = "https://andrew.gg/bean/youtube-auth/";

std::string GetYouTubeAuthServerUrl()
{
    return kYouTubeAuthServerUrl;
}

void PostYouTubeUploadProgress(
    AppContext* ctx,
    int percent,
    const std::wstring& text,
    const std::wstring& videoUrl = {})
{
    if (!ctx) {
        return;
    }
    auto* payload = new YouTubeUploadProgressPayload();
    payload->percent = std::clamp(percent, 0, 100);
    payload->text = text;
    payload->videoUrl = videoUrl;
    PostOwnedAppMessage(ctx, WM_BEAN_YOUTUBE_UPLOAD_PROGRESS, payload);
}

LRESULT CALLBACK YouTubeUploadStatusSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR refData)
{
    auto* ctx = reinterpret_cast<AppContext*>(refData);
    if (message == WM_SETCURSOR && ctx && !ctx->youtubeLastVideoUrl.empty()) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        if (PtInRect(&ctx->youtubeUploadLinkBounds, point)) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
            return TRUE;
        }
    }
    if (message == WM_LBUTTONUP && ctx && !ctx->youtubeLastVideoUrl.empty()) {
        const POINT point{
            static_cast<short>(LOWORD(lParam)),
            static_cast<short>(HIWORD(lParam))};
        if (PtInRect(&ctx->youtubeUploadLinkBounds, point)) {
            const auto result = reinterpret_cast<intptr_t>(
                ShellExecuteW(hwnd, L"open", ctx->youtubeLastVideoUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32) {
                SetStatus(ctx, L"Failed to open the uploaded YouTube video.");
            }
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, YouTubeUploadStatusSubclassProc, 4);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void RequestYouTubeUiRefresh(AppContext* ctx)
{
    PostBeanAppMessage(ctx, WM_BEAN_YOUTUBE_UI_REFRESH);
}

void ResolveLinkedYouTubeIdentityAsync(AppContext* ctx, bool postErrorToStatus)
{
    if (!ctx
        || !ctx->mainWindow
        || ctx->shuttingDown.load(std::memory_order_acquire)
        || ctx->settings.youtubeRefreshToken.empty()
        || ctx->settings.youtubeClientId.empty()) {
        return;
    }
    bean::integrations::YouTubeCredentials creds;
    creds.clientId = ctx->settings.youtubeClientId;
    creds.refreshToken = ctx->settings.youtubeRefreshToken;
    creds.authServerUrl = GetYouTubeAuthServerUrl();
    LaunchAppWorker(ctx, [ctx, creds, postErrorToStatus]() {
        const auto identity = bean::integrations::YouTubeUploader::GetLinkedChannelIdentity(creds);
        auto* payload = new YouTubeIdentityResolvedPayload();
        payload->success = identity.success;
        payload->channelId = identity.channelId;
        payload->channelTitle = identity.channelTitle;
        payload->error = postErrorToStatus ? identity.error : std::string{};
        PostOwnedAppMessage(ctx, WM_BEAN_YOUTUBE_IDENTITY_RESOLVED, payload);
    });
}

void SetYouTubeUploadUi(AppContext* ctx, int percent, const std::wstring& text)
{
    if (!ctx) {
        return;
    }
    const int clampedPercent = std::clamp(percent, 0, 100);
    if (ctx->youtubeUploadProgress) {
        if (ctx->youtubeUploadPercent != clampedPercent) {
            ctx->youtubeUploadPercent = clampedPercent;
            SendMessageW(ctx->youtubeUploadProgress, PBM_SETPOS, static_cast<WPARAM>(clampedPercent), 0);
        }
    }
    if (ctx->youtubeUploadStatus) {
        if (ctx->youtubeUploadStatusText != text) {
            ctx->youtubeUploadStatusText = text;
            UpdateTransparentStaticText(ctx->youtubeUploadStatus, text.c_str());
        }
    }
}


void UpdateYouTubeMediaSelection(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    const int selectedIndex = GetSelectedYouTubeMediaIndex(ctx);
    if (selectedIndex >= 0 && static_cast<size_t>(selectedIndex) < ctx->youtubeMediaItems.size()) {
        if (ctx->youtubeTitleEdit) {
            const std::wstring title = DefaultYouTubeTitle(
                ctx->youtubeMediaItems[static_cast<size_t>(selectedIndex)].path);
            if (GetWindowTextString(ctx->youtubeTitleEdit) != title) {
                SetWindowTextW(ctx->youtubeTitleEdit, title.c_str());
            }
        }
    } else if (ctx->youtubeTitleEdit) {
        if (!GetWindowTextString(ctx->youtubeTitleEdit).empty()) {
            SetWindowTextW(ctx->youtubeTitleEdit, L"");
        }
    }
    RefreshYouTubeUiState(ctx);
}


void RefreshYouTubeUiState(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    const auto setTextIfChanged = [](HWND control, const std::wstring& text) {
        if (control && GetWindowTextString(control) != text) {
            SetWindowTextW(control, text.c_str());
        }
    };
    const auto setVisibleIfChanged = [](HWND control, bool visible) {
        if (control && (IsWindowVisible(control) != FALSE) != visible) {
            ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
        }
    };
    const auto setEnabledIfChanged = [](HWND control, BOOL enabled) {
        if (control && IsWindowEnabled(control) != enabled) {
            EnableWindow(control, enabled);
        }
    };
    const bool wasOauthConfigured = ctx->youtubeOAuthConfigured;
    const bool wasLinked = ctx->youtubeLinked;
    const bool oauthConfigured = !GetYouTubeAuthServerUrl().empty();
    const bool linked = !ctx->settings.youtubeRefreshToken.empty();
    ctx->youtubeOAuthConfigured = oauthConfigured;
    ctx->youtubeLinked = linked;
    if (!linked) {
        ctx->youtubeUnlinkConfirmPending = false;
    }
    const int selectedIndex = GetSelectedYouTubeMediaIndex(ctx);
    const bool canUpload = oauthConfigured && linked && !ctx->youtubeBusy.load() && selectedIndex >= 0 && static_cast<size_t>(selectedIndex) < ctx->youtubeMediaItems.size();

    if (ctx->youtubeLinkStatus) {
        const std::wstring statusText = !oauthConfigured
            ? L"OAuth not configured"
            : (linked ? L"Linked" : L"Not linked");
        setTextIfChanged(ctx->youtubeLinkStatus, statusText);
        setVisibleIfChanged(ctx->youtubeLinkStatus, false);
        if (wasOauthConfigured != oauthConfigured || wasLinked != linked) {
            InvalidateRect(ctx->youtubeLinkStatus, nullptr, FALSE);
        }
    }
    if (ctx->youtubeLinkButton) {
        setVisibleIfChanged(ctx->youtubeLinkButton, !linked && oauthConfigured);
        setEnabledIfChanged(ctx->youtubeLinkButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    const bool showUnlinkConfirm = linked && ctx->youtubeUnlinkConfirmPending;
    if (ctx->youtubeUnlinkButton) {
        setTextIfChanged(ctx->youtubeUnlinkButton, L"Unlink Account");
        setVisibleIfChanged(ctx->youtubeUnlinkButton, linked && !showUnlinkConfirm);
        setEnabledIfChanged(ctx->youtubeUnlinkButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    if (ctx->youtubeUnlinkConfirmLabel) {
        setVisibleIfChanged(ctx->youtubeUnlinkConfirmLabel, showUnlinkConfirm);
    }
    if (ctx->youtubeUnlinkYesButton) {
        setVisibleIfChanged(ctx->youtubeUnlinkYesButton, showUnlinkConfirm);
        setEnabledIfChanged(ctx->youtubeUnlinkYesButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    if (ctx->youtubeUnlinkNoButton) {
        setVisibleIfChanged(ctx->youtubeUnlinkNoButton, showUnlinkConfirm);
        setEnabledIfChanged(ctx->youtubeUnlinkNoButton, ctx->youtubeBusy.load() ? FALSE : TRUE);
    }
    if (ctx->youtubeAccountLabel) {
        // Transparent STATIC: use UpdateTransparentStaticText so repeated refreshes
        // don't stack glyphs (SetWindowText alone doesn't erase under NULL_BRUSH).
        UpdateTransparentStaticText(ctx->youtubeAccountLabel, L"YouTube Account:");
        if (!linked) {
            if (ctx->youtubeAccountLink) {
                setTextIfChanged(ctx->youtubeAccountLink, L"Not linked");
                setEnabledIfChanged(ctx->youtubeAccountLink, FALSE);
                setVisibleIfChanged(ctx->youtubeAccountLink, true);
            }
        } else if (!ctx->settings.youtubeChannelId.empty()) {
            if (ctx->youtubeAccountLink) {
                const std::wstring text = ToWide(
                    ctx->settings.youtubeChannelTitle.empty()
                        ? ctx->settings.youtubeChannelId
                        : ctx->settings.youtubeChannelTitle);
                setTextIfChanged(ctx->youtubeAccountLink, text);
                setEnabledIfChanged(ctx->youtubeAccountLink, TRUE);
                setVisibleIfChanged(ctx->youtubeAccountLink, true);
            }
        } else {
            if (ctx->youtubeAccountLink) {
                setTextIfChanged(ctx->youtubeAccountLink, ctx->youtubeBusy.load() ? L"Resolving..." : L"Linked");
                setEnabledIfChanged(ctx->youtubeAccountLink, FALSE);
                setVisibleIfChanged(ctx->youtubeAccountLink, true);
            }
        }
    }
    if (ctx->youtubeUploadButton) {
        setEnabledIfChanged(ctx->youtubeUploadButton, canUpload ? TRUE : FALSE);
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
    ctx->youtubeLastVideoUrl.clear();
    std::string saveError;
    if (!ctx->settingsStore.Save(ctx->settings, saveError)) {
        SetStatus(ctx, std::wstring(L"Failed to unlink YouTube account: ") + ToWide(saveError));
    } else {
        SetStatus(ctx, L"YouTube account unlinked.");
        SetYouTubeUploadUi(ctx, 0, L"No upload in progress.");
    }
}



void BeginYouTubeAuthorization(AppContext* ctx, HWND hwnd)
{
const std::string authServerUrl = GetYouTubeAuthServerUrl();
if (authServerUrl.empty()) {
    SetStatus(ctx, kYouTubeOAuthCredentialsMissingMessage);
    return;
}
if (ctx->youtubeBusy.load()) {
    SetStatus(ctx, L"YouTube action already in progress.");
    return;
}
ctx->youtubeBusy.store(true);
RefreshYouTubeUiState(ctx);
SetStatus(ctx, L"Opening browser for YouTube authorization...");
if (!LaunchAppWorker(ctx, [ctx, hwnd, authServerUrl]() {
    const auto auth = bean::integrations::YouTubeUploader::AuthorizeDesktop(hwnd, authServerUrl);
    auto* payload = new YouTubeAuthCompletionPayload();
    payload->success = auth.success;
    payload->clientId = auth.clientId;
    payload->refreshToken = auth.refreshToken;
    payload->channelId = auth.channelId;
    payload->channelTitle = auth.channelTitle;
    payload->error = auth.error;
    PostOwnedAppMessage(ctx, WM_BEAN_YOUTUBE_AUTH_COMPLETE, payload);
})) {
    ctx->youtubeBusy.store(false);
    RefreshYouTubeUiState(ctx);
}
}

void BeginYouTubeUpload(AppContext* ctx)
{
if (ctx->youtubeBusy.load()) {
    SetStatus(ctx, L"YouTube action already in progress.");
    return;
}
const int selected = GetSelectedYouTubeMediaIndex(ctx);
if (selected < 0 || static_cast<size_t>(selected) >= ctx->youtubeMediaItems.size()) {
    SetStatus(ctx, L"Select a recording or clip before uploading.");
    return;
}
if (ctx->settings.youtubeClientId.empty() || ctx->settings.youtubeRefreshToken.empty()) {
    SetStatus(ctx, L"Link your YouTube account first.");
    return;
}
const std::wstring titleWide = GetWindowTextString(ctx->youtubeTitleEdit);
if (titleWide.empty()) {
    SetStatus(ctx, L"Enter a title for the upload.");
    return;
}
bean::integrations::YouTubePrivacy privacy = bean::integrations::YouTubePrivacy::Private;
const int privacyIndex = static_cast<int>(SendMessageW(ctx->youtubePrivacyCombo, CB_GETCURSEL, 0, 0));
if (privacyIndex == 1) {
    privacy = bean::integrations::YouTubePrivacy::Unlisted;
} else if (privacyIndex == 2) {
    privacy = bean::integrations::YouTubePrivacy::Public;
}

const auto path = ctx->youtubeMediaItems[static_cast<size_t>(selected)].path;
const auto title = ToUtf8(titleWide);
bean::integrations::YouTubeCredentials creds;
creds.clientId = ctx->settings.youtubeClientId;
creds.refreshToken = ctx->settings.youtubeRefreshToken;
creds.authServerUrl = GetYouTubeAuthServerUrl();
ctx->youtubeLastVideoUrl.clear();
ctx->youtubeBusy.store(true);
RefreshYouTubeUiState(ctx);
SetYouTubeUploadUi(ctx, 0, std::wstring(L"Uploading: ") + path.filename().wstring());
SetStatus(ctx, std::wstring(L"Uploading to YouTube: ") + path.filename().wstring());
if (!LaunchAppWorker(ctx, [ctx, path, title, privacy, creds]() {
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
    std::wstring videoUrl;
    if (!upload.videoUrl.empty()) {
        videoUrl = ToWide(upload.videoUrl);
    }
    PostStatus(ctx, message);
    PostYouTubeUploadProgress(ctx, 100, message, videoUrl);
    ctx->youtubeBusy.store(false);
    RequestYouTubeUiRefresh(ctx);
})) {
    ctx->youtubeBusy.store(false);
    RefreshYouTubeUiState(ctx);
}
}

void RefreshYouTubeMediaList(AppContext* ctx, bool startReconciliation)
{
    if (!ctx || !ctx->youtubeMediaList || !ctx->youtubeLabel) {
        return;
    }

    const auto folders = CollectKnownRecordingFolders(ctx);
    const bool anyFolderAvailable = std::any_of(
        folders.begin(),
        folders.end(),
        [](const auto& folder) { return DirectoryExists(folder.wstring()); });
    if (!anyFolderAvailable) {
        if (!ctx->youtubeMediaItems.empty() || ctx->youtubeMediaSelectedIndex != -1) {
            ctx->youtubeMediaItems.clear();
            RepopulateYouTubeMediaList(ctx);
            UpdateYouTubeMediaSelection(ctx);
        }
        UpdateTransparentStaticText(ctx->youtubeLabel, L"Recordings folder is unavailable.");
        if (startReconciliation) {
            BeginRecordingReconciliation(ctx);
        }
        return;
    }

    const auto previousItems = ctx->youtubeMediaItems;
    ctx->youtubeMediaItems = EnumerateYouTubeMediaFilesInFolders(folders);
    if (ctx->runRepository) {
        std::string dbError;
        std::unordered_map<std::string, std::string> triggerReasonsByPath;
        for (const auto& run : ctx->runRepository->ListRuns(dbError)) {
            triggerReasonsByPath[RecordingPathKey(run.videoPath)] = run.triggerReason;
            for (const auto& alias : run.pathAliases) {
                triggerReasonsByPath[RecordingPathKey(alias)] = run.triggerReason;
            }
        }
        for (auto& item : ctx->youtubeMediaItems) {
            if (item.type != YouTubeMediaType::Recording) {
                continue;
            }
            const auto triggerIt = triggerReasonsByPath.find(RecordingPathKey(item.path));
            if (triggerIt != triggerReasonsByPath.end()) {
                item.triggerReason = triggerIt->second;
            }
        }
    }
    SortYouTubeMediaItems(ctx);
    const bool mediaListChanged = !YouTubeMediaItemsEqual(previousItems, ctx->youtubeMediaItems);
    if (mediaListChanged) {
        RepopulateYouTubeMediaList(ctx);
        UpdateYouTubeMediaSelection(ctx);
    } else {
        RefreshYouTubeUiState(ctx);
    }

    size_t recordingCount = 0;
    size_t clipCount = 0;
    for (const auto& item : ctx->youtubeMediaItems) {
        if (item.type == YouTubeMediaType::Clip) {
            ++clipCount;
        } else {
            ++recordingCount;
        }
    }
    std::wostringstream summary;
    if (folders.size() == 1) {
        summary << folders.front().wstring();
    } else {
        summary << L"Known recording folders";
    }
    summary << L" (" << recordingCount << L" recording";
    if (recordingCount != 1) {
        summary << L"s";
    }
    summary << L", " << clipCount << L" clip";
    if (clipCount != 1) {
        summary << L"s";
    }
    summary << L")";
    UpdateTransparentStaticText(ctx->youtubeLabel, summary.str().c_str());
    if (startReconciliation) {
        BeginRecordingReconciliation(ctx);
    }
}

