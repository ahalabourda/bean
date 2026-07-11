#pragma once

#include "app/AppContext.h"
#include <filesystem>
#include <string>

std::filesystem::path GetExecutableDirectory();
std::filesystem::path SpecIconPathFromExe(const std::string& className, const std::string& specName);
HBITMAP LoadPngBitmapForImageList(const std::filesystem::path& pngPath, int iconSizePx, int canvasSizePx, int verticalOffsetPx);
