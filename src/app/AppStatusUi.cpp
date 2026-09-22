#include "app/AppStatusUi.h"

#include "app/AppRecordings.h"
#include "app/AppYouTubeController.h"

void RefreshVisibleRecordingFileLists(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->activeTab == AppContext::MainTab::Recordings) {
        RefreshRecordingsList(ctx);
    } else if (ctx->activeTab == AppContext::MainTab::YouTube) {
        RefreshYouTubeMediaList(ctx);
    }
}

void RefreshLiveStatus(AppContext* ctx);


void RefreshStatusCommandButtons(AppContext* ctx);

void RefreshStatusCommandButtons(AppContext* ctx)
{
    if (!ctx || !ctx->statusPanel) {
        return;
    }

    HWND recordStart = GetDlgItem(ctx->statusPanel, IDC_RECORD_START);
    HWND recordStop = GetDlgItem(ctx->statusPanel, IDC_RECORD_STOP);

    if (recordStart) {
        const BOOL shouldEnable = ctx->isRecording ? FALSE : TRUE;
        if (IsWindowEnabled(recordStart) != shouldEnable) {
            EnableWindow(recordStart, shouldEnable);
        }
    }
    if (recordStop) {
        const BOOL shouldEnable = ctx->isRecording ? TRUE : FALSE;
        if (IsWindowEnabled(recordStop) != shouldEnable) {
            EnableWindow(recordStop, shouldEnable);
        }
    }
}

