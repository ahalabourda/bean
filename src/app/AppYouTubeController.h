#pragma once

#include "app/AppContext.h"

#include <string>

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
    std::wstring videoUrl;
};

struct YouTubeIdentityResolvedPayload {
    bool success = false;
    std::string channelId;
    std::string channelTitle;
    std::string error;
};

std::string GetYouTubeAuthServerUrl();
void RequestYouTubeUiRefresh(AppContext* ctx);
void ResolveLinkedYouTubeIdentityAsync(AppContext* ctx, bool postErrorToStatus);
void SetYouTubeUploadUi(AppContext* ctx, int percent, const std::wstring& text);
void RefreshYouTubeUiState(AppContext* ctx);
void UpdateYouTubeMediaSelection(AppContext* ctx);
void UnlinkYouTubeAccount(AppContext* ctx);
void BeginYouTubeAuthorization(AppContext* ctx, HWND hwnd);
void BeginYouTubeUpload(AppContext* ctx);

LRESULT CALLBACK YouTubeUploadStatusSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR refData);
