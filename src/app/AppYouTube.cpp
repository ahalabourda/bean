#include "app/AppYouTube.h"

#include "app/AppDraw.h"
#include "app/AppRecordingHelpers.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

int GetSelectedYouTubeMediaIndex(AppContext* ctx)
{
    if (!ctx || !ctx->youtubeMediaList) {
        return -1;
    }
    return GetBeanFileListSelectedIndex(ctx->youtubeMediaList);
}

std::wstring DefaultYouTubeTitle(const std::filesystem::path& path)
{
    auto title = path.stem().wstring();
    if (title.empty()) {
        title = path.filename().wstring();
    }
    std::replace(title.begin(), title.end(), L'_', L' ');
    return title;
}

void SortYouTubeMediaItems(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    YouTubeMediaSortColumn sortColumn = YouTubeMediaSortColumn::Type;
    switch (ctx->youtubeSortColumn) {
    case AppContext::YouTubeSortColumn::Name:
        sortColumn = YouTubeMediaSortColumn::Name;
        break;
    case AppContext::YouTubeSortColumn::Date:
        sortColumn = YouTubeMediaSortColumn::Date;
        break;
    case AppContext::YouTubeSortColumn::Type:
        break;
    }
    SortYouTubeMediaFiles(
        ctx->youtubeMediaItems,
        sortColumn,
        ctx->youtubeSortAscending);
}

bool YouTubeMediaItemsEqual(
    const std::vector<YouTubeMediaFile>& left,
    const std::vector<YouTubeMediaFile>& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].path.lexically_normal() != right[index].path.lexically_normal()
            || left[index].type != right[index].type
            || left[index].triggerReason != right[index].triggerReason
            || left[index].modified != right[index].modified
            || left[index].size != right[index].size) {
            return false;
        }
    }
    return true;
}

void RepopulateYouTubeMediaList(AppContext* ctx)
{
    if (!ctx || !ctx->youtubeMediaList) {
        return;
    }

    ctx->youtubeMediaSelectedIndex = -1;
    RefreshBeanFileList(ctx->youtubeMediaList);
}


