#pragma once

#include "app/AppContext.h"

#include <filesystem>
#include <string>

std::filesystem::path GetExecutableDirectory();
std::filesystem::path SpecIconPathFromExe(const std::string& className, const std::string& specName);
HBITMAP LoadPngBitmapForImageList(const std::filesystem::path& pngPath, int iconSizePx, int canvasSizePx, int verticalOffsetPx);

std::wstring GetWindowTextString(HWND hwnd);
void UpdateTransparentStaticText(HWND control, const wchar_t* newText);
void InvalidateControlAndParentRegion(HWND control);
int ReadIntControl(HWND hwnd, int fallback);
std::wstring PickFolder(HWND owner);
std::wstring PickImageFile(HWND owner);
std::wstring FormatHresultHex(HRESULT hr);
std::wstring GetKnownFolderPath(REFKNOWNFOLDERID folderId);
bool DirectoryExists(const std::wstring& path);
bool EnsureOutputDirectoryReady(const std::filesystem::path& outputDirectory, std::string& error);
