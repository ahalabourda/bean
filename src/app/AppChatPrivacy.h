#pragma once

#include "app/AppContext.h"

#include <filesystem>
#include <string>

std::filesystem::path ResolveChatBlockerImagesDirectory(const AppContext* ctx);
std::filesystem::path ResolveSelectedChatBlockerImagePath(const AppContext* ctx);
void RememberChatBlockerSizeForSelectedImage(AppContext* ctx);
bool SyncChatBlockerSelectionToImageMetadata(AppContext* ctx, bool resetBlockerSizeToImage);
void RefreshChatBlockerImageCombo(AppContext* ctx, const std::wstring& preferredFileName);
void RefreshChatBlockerImageControls(AppContext* ctx);
void ApplyChatBlockerAspectForEdit(AppContext* ctx, int editedControlId);
bool DrawChatBlockerImageOverlay(
    HDC targetDc,
    const RECT& targetRect,
    const std::filesystem::path& imagePath);
bool SaveCustomChatBlockerImage(
    AppContext* ctx,
    const std::filesystem::path& sourcePath,
    std::wstring& error);

int ChatBlockerAnchorToComboIndex(bean::core::AppSettings::ChatBlockerAnchor anchor);
bean::core::AppSettings::ChatBlockerAnchor ChatBlockerAnchorFromComboIndex(int index);
