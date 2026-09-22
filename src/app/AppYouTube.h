#pragma once

#include "app/AppContext.h"

int GetSelectedYouTubeMediaIndex(AppContext* ctx);
std::wstring DefaultYouTubeTitle(const std::filesystem::path& path);
void SortYouTubeMediaItems(AppContext* ctx);
bool YouTubeMediaItemsEqual(
    const std::vector<YouTubeMediaFile>& left,
    const std::vector<YouTubeMediaFile>& right);
void RepopulateYouTubeMediaList(AppContext* ctx);
