#include "app/AppChatPrivacy.h"

#include "app/AppUtilities.h"
#include "util/Strings.h"

#include <algorithm>
#include <cwctype>
#include <exception>
#include <gdiplus.h>
#include <memory>
#include <string>
#include <vector>

using bean::util::ToUtf8;
using bean::util::ToWide;

int ChatBlockerAnchorToComboIndex(bean::core::AppSettings::ChatBlockerAnchor anchor)
{
    switch (anchor) {
    case bean::core::AppSettings::ChatBlockerAnchor::BottomRight:
        return 1;
    case bean::core::AppSettings::ChatBlockerAnchor::TopLeft:
        return 2;
    case bean::core::AppSettings::ChatBlockerAnchor::TopRight:
        return 3;
    case bean::core::AppSettings::ChatBlockerAnchor::BottomLeft:
    default:
        return 0;
    }
}

bean::core::AppSettings::ChatBlockerAnchor ChatBlockerAnchorFromComboIndex(int index)
{
    switch (index) {
    case 1:
        return bean::core::AppSettings::ChatBlockerAnchor::BottomRight;
    case 2:
        return bean::core::AppSettings::ChatBlockerAnchor::TopLeft;
    case 3:
        return bean::core::AppSettings::ChatBlockerAnchor::TopRight;
    case 0:
    default:
        return bean::core::AppSettings::ChatBlockerAnchor::BottomLeft;
    }
}

std::filesystem::path ResolveChatBlockerImagesDirectory(const AppContext* ctx)
{
    if (!ctx) {
        return {};
    }
    const auto appDataDirectory = ctx->settingsStore.GetConfigPath().parent_path();
    if (appDataDirectory.empty()) {
        return {};
    }
    return appDataDirectory / "chat-blocker-images";
}

bool EnsureUiGdiplusInitialized()
{
    static const bool initialized = []() -> bool {
        Gdiplus::GdiplusStartupInput startupInput;
        ULONG_PTR token = 0;
        return Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) == Gdiplus::Ok;
    }();
    return initialized;
}

bool TryReadImageDimensions(const std::filesystem::path& imagePath, int& width, int& height)
{
    width = 0;
    height = 0;
    if (!EnsureUiGdiplusInitialized()) {
        return false;
    }
    bool success = false;
    {
        Gdiplus::Bitmap bitmap(imagePath.wstring().c_str());
        if (bitmap.GetLastStatus() == Gdiplus::Ok) {
            width = static_cast<int>(bitmap.GetWidth());
            height = static_cast<int>(bitmap.GetHeight());
            success = width > 0 && height > 0;
        }
    }
    return success;
}

std::wstring GetSelectedComboText(HWND combo)
{
    if (!combo) {
        return {};
    }
    const int selectedIndex = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (selectedIndex < 0) {
        return {};
    }
    const int textLength = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(selectedIndex), 0));
    if (textLength <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(textLength) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(selectedIndex), reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(textLength));
    return text;
}

std::wstring GetComboItemText(HWND combo, int index)
{
    if (!combo || index < 0) {
        return {};
    }
    const int textLength = static_cast<int>(SendMessageW(
        combo,
        CB_GETLBTEXTLEN,
        static_cast<WPARAM>(index),
        0));
    if (textLength < 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(textLength) + 1, L'\0');
    if (SendMessageW(
            combo,
            CB_GETLBTEXT,
            static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(text.data())) == CB_ERR) {
        return {};
    }
    text.resize(static_cast<size_t>(textLength));
    return text;
}

bool IsSupportedChatBlockerImageExtension(std::wstring extension)
{
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return extension == L".png"
        || extension == L".jpg"
        || extension == L".jpeg"
        || extension == L".bmp"
        || extension == L".gif"
        || extension == L".webp";
}

std::vector<std::wstring> EnumerateChatBlockerImageFileNames(const AppContext* ctx)
{
    std::vector<std::wstring> names;
    const auto imagesDirectory = ResolveChatBlockerImagesDirectory(ctx);
    if (imagesDirectory.empty()) {
        return names;
    }

    std::error_code ec;
    if (!std::filesystem::exists(imagesDirectory, ec) || ec) {
        return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator(imagesDirectory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::wstring extension = entry.path().extension().wstring();
        if (!IsSupportedChatBlockerImageExtension(extension)) {
            continue;
        }
        names.push_back(entry.path().filename().wstring());
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& lhs, const std::wstring& rhs) {
        std::wstring lhsLower = lhs;
        std::wstring rhsLower = rhs;
        std::transform(lhsLower.begin(), lhsLower.end(), lhsLower.begin(), towlower);
        std::transform(rhsLower.begin(), rhsLower.end(), rhsLower.begin(), towlower);
        return lhsLower < rhsLower;
    });
    return names;
}

std::filesystem::path ResolveSelectedChatBlockerImagePath(const AppContext* ctx)
{
    if (!ctx || !ctx->chatBlockerImageCombo) {
        return {};
    }
    const std::wstring fileName = GetSelectedComboText(ctx->chatBlockerImageCombo);
    if (fileName.empty() || _wcsicmp(fileName.c_str(), L"No images imported") == 0) {
        return {};
    }
    const auto imagesDirectory = ResolveChatBlockerImagesDirectory(ctx);
    if (imagesDirectory.empty()) {
        return {};
    }
    return imagesDirectory / fileName;
}

std::string ResolveSelectedChatBlockerImageFileNameKey(const AppContext* ctx)
{
    const auto imagePath = ResolveSelectedChatBlockerImagePath(ctx);
    if (imagePath.empty()) {
        return {};
    }
    return ToUtf8(imagePath.filename().wstring());
}

void RememberChatBlockerSizeForSelectedImage(AppContext* ctx)
{
    if (!ctx || !ctx->chatBlockerWidthEdit || !ctx->chatBlockerHeightEdit) {
        return;
    }
    const bool customSelected = ctx->chatBlockerImageCustomRadio
        && SendMessageW(ctx->chatBlockerImageCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!customSelected) {
        return;
    }

    const std::string imageFileNameKey = ResolveSelectedChatBlockerImageFileNameKey(ctx);
    if (imageFileNameKey.empty()) {
        return;
    }

    const int width = (std::max)(0, ReadIntControl(ctx->chatBlockerWidthEdit, ctx->settings.chatBlockerWidth));
    const int height = (std::max)(0, ReadIntControl(ctx->chatBlockerHeightEdit, ctx->settings.chatBlockerHeight));
    if (width > 0 && height > 0) {
        ctx->settings.chatBlockerCustomImageSizesByFileName[imageFileNameKey] = std::make_pair(width, height);
    }
}

bool SyncChatBlockerSelectionToImageMetadata(AppContext* ctx, bool resetBlockerSizeToImage)
{
    if (!ctx) {
        return false;
    }
    const auto imagePath = ResolveSelectedChatBlockerImagePath(ctx);
    if (imagePath.empty()) {
        ctx->chatBlockerCustomSourceWidth = 0;
        ctx->chatBlockerCustomSourceHeight = 0;
        return false;
    }

    int sourceWidth = 0;
    int sourceHeight = 0;
    if (!TryReadImageDimensions(imagePath, sourceWidth, sourceHeight)) {
        return false;
    }
    ctx->chatBlockerCustomSourceWidth = sourceWidth;
    ctx->chatBlockerCustomSourceHeight = sourceHeight;
    if (resetBlockerSizeToImage && ctx->chatBlockerWidthEdit && ctx->chatBlockerHeightEdit) {
        int widthToApply = sourceWidth;
        int heightToApply = sourceHeight;
        const std::string imageFileNameKey = ToUtf8(imagePath.filename().wstring());
        const auto it = ctx->settings.chatBlockerCustomImageSizesByFileName.find(imageFileNameKey);
        if (it != ctx->settings.chatBlockerCustomImageSizesByFileName.end()
            && it->second.first > 0
            && it->second.second > 0) {
            widthToApply = it->second.first;
            heightToApply = it->second.second;
        }
        SetWindowTextW(ctx->chatBlockerWidthEdit, ToWide(std::to_string(widthToApply)).c_str());
        SetWindowTextW(ctx->chatBlockerHeightEdit, ToWide(std::to_string(heightToApply)).c_str());
    }
    return true;
}

void RefreshChatBlockerImageCombo(AppContext* ctx, const std::wstring& preferredFileName)
{
    if (!ctx || !ctx->chatBlockerImageCombo) {
        return;
    }

    const std::wstring requestedSelection = preferredFileName.empty()
        ? GetSelectedComboText(ctx->chatBlockerImageCombo)
        : preferredFileName;

    const auto imageFileNames = EnumerateChatBlockerImageFileNames(ctx);
    const int existingItemCount = static_cast<int>(SendMessageW(
        ctx->chatBlockerImageCombo,
        CB_GETCOUNT,
        0,
        0));
    const int expectedItemCount = imageFileNames.empty()
        ? 1
        : static_cast<int>(imageFileNames.size());
    bool contentsMatch = existingItemCount == expectedItemCount;
    if (contentsMatch && imageFileNames.empty()) {
        contentsMatch = _wcsicmp(
                GetComboItemText(ctx->chatBlockerImageCombo, 0).c_str(),
                L"No images imported") == 0;
    } else if (contentsMatch) {
        for (size_t index = 0; index < imageFileNames.size(); ++index) {
            if (_wcsicmp(
                    GetComboItemText(ctx->chatBlockerImageCombo, static_cast<int>(index)).c_str(),
                    imageFileNames[index].c_str()) != 0) {
                contentsMatch = false;
                break;
            }
        }
    }
    if (contentsMatch) {
        const std::wstring currentSelection = GetSelectedComboText(ctx->chatBlockerImageCombo);
        const std::wstring desiredSelection = preferredFileName.empty()
            ? currentSelection
            : preferredFileName;
        int desiredIndex = 0;
        if (!imageFileNames.empty() && !desiredSelection.empty()) {
            for (size_t index = 0; index < imageFileNames.size(); ++index) {
                if (_wcsicmp(imageFileNames[index].c_str(), desiredSelection.c_str()) == 0) {
                    desiredIndex = static_cast<int>(index);
                    break;
                }
            }
        }
        const int currentIndex = static_cast<int>(SendMessageW(
            ctx->chatBlockerImageCombo,
            CB_GETCURSEL,
            0,
            0));
        if (currentIndex != desiredIndex) {
            SendMessageW(
                ctx->chatBlockerImageCombo,
                CB_SETCURSEL,
                static_cast<WPARAM>(desiredIndex),
                0);
        }
        return;
    }

    SendMessageW(ctx->chatBlockerImageCombo, CB_RESETCONTENT, 0, 0);

    if (imageFileNames.empty()) {
        SendMessageW(
            ctx->chatBlockerImageCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(L"No images imported"));
        SendMessageW(ctx->chatBlockerImageCombo, CB_SETCURSEL, 0, 0);
        return;
    }

    int selectedIndex = -1;
    for (size_t index = 0; index < imageFileNames.size(); ++index) {
        const int comboIndex = static_cast<int>(SendMessageW(
            ctx->chatBlockerImageCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(imageFileNames[index].c_str())));
        if (comboIndex >= 0 && _wcsicmp(imageFileNames[index].c_str(), requestedSelection.c_str()) == 0) {
            selectedIndex = comboIndex;
        }
    }

    if (selectedIndex < 0 && !imageFileNames.empty()) {
        selectedIndex = 0;
    }
    SendMessageW(
        ctx->chatBlockerImageCombo,
        CB_SETCURSEL,
        selectedIndex >= 0 ? static_cast<WPARAM>(selectedIndex) : static_cast<WPARAM>(-1),
        0);
}

int ScaleByAspectRatio(int value, int numerator, int denominator)
{
    if (value <= 0 || numerator <= 0 || denominator <= 0) {
        return 0;
    }
    const double scaled = static_cast<double>(value) * static_cast<double>(numerator) / static_cast<double>(denominator);
    return (std::max)(1, static_cast<int>(std::lround(scaled)));
}

void RefreshChatBlockerImageControls(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    const bool customSelected = ctx->chatBlockerImageCustomRadio
        && SendMessageW(ctx->chatBlockerImageCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ctx->chatBlockerImageCombo) {
        const BOOL shouldEnable = customSelected ? TRUE : FALSE;
        if (IsWindowEnabled(ctx->chatBlockerImageCombo) != shouldEnable) {
            EnableWindow(ctx->chatBlockerImageCombo, shouldEnable);
        }
    }
    if (ctx->chatBlockerImageImportButton) {
        if (!IsWindowEnabled(ctx->chatBlockerImageImportButton)) {
            EnableWindow(ctx->chatBlockerImageImportButton, TRUE);
        }
    }
    if (ctx->chatBlockerImageOpenFolderButton) {
        if (!IsWindowEnabled(ctx->chatBlockerImageOpenFolderButton)) {
            EnableWindow(ctx->chatBlockerImageOpenFolderButton, TRUE);
        }
    }
}

void ApplyChatBlockerAspectForEdit(AppContext* ctx, int editedControlId)
{
    if (!ctx || ctx->chatBlockerAspectAdjusting) {
        return;
    }
    const bool customSelected = ctx->chatBlockerImageCustomRadio
        && SendMessageW(ctx->chatBlockerImageCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!customSelected || ctx->chatBlockerCustomSourceWidth <= 0 || ctx->chatBlockerCustomSourceHeight <= 0) {
        return;
    }

    ctx->chatBlockerAspectAdjusting = true;
    if (editedControlId == IDC_CHAT_BLOCKER_WIDTH_EDIT && ctx->chatBlockerWidthEdit && ctx->chatBlockerHeightEdit) {
        const int width = (std::max)(0, ReadIntControl(ctx->chatBlockerWidthEdit, ctx->settings.chatBlockerWidth));
        const int height = ScaleByAspectRatio(width, ctx->chatBlockerCustomSourceHeight, ctx->chatBlockerCustomSourceWidth);
        ctx->chatBlockerIgnoreNextHeightChange = true;
        SetWindowTextW(ctx->chatBlockerHeightEdit, ToWide(std::to_string(height)).c_str());
    } else if (editedControlId == IDC_CHAT_BLOCKER_HEIGHT_EDIT && ctx->chatBlockerHeightEdit && ctx->chatBlockerWidthEdit) {
        const int height = (std::max)(0, ReadIntControl(ctx->chatBlockerHeightEdit, ctx->settings.chatBlockerHeight));
        const int width = ScaleByAspectRatio(height, ctx->chatBlockerCustomSourceWidth, ctx->chatBlockerCustomSourceHeight);
        ctx->chatBlockerIgnoreNextWidthChange = true;
        SetWindowTextW(ctx->chatBlockerWidthEdit, ToWide(std::to_string(width)).c_str());
    }
    ctx->chatBlockerAspectAdjusting = false;
}

bool DrawChatBlockerImageOverlay(HDC targetDc, const RECT& targetRect, const std::filesystem::path& imagePath)
{
    if (!targetDc || imagePath.empty() || targetRect.right <= targetRect.left || targetRect.bottom <= targetRect.top) {
        return false;
    }
    if (!EnsureUiGdiplusInitialized()) {
        return false;
    }

    static std::filesystem::path cachedImagePath;
    static std::unique_ptr<Gdiplus::Bitmap> cachedImage;
    if (!cachedImage || cachedImagePath != imagePath) {
        auto loadedImage = std::make_unique<Gdiplus::Bitmap>(imagePath.wstring().c_str());
        if (!loadedImage || loadedImage->GetLastStatus() != Gdiplus::Ok) {
            cachedImage.reset();
            cachedImagePath.clear();
            return false;
        }
        cachedImage = std::move(loadedImage);
        cachedImagePath = imagePath;
    }

    bool drewImage = false;
    {
        Gdiplus::Graphics graphics(targetDc);
        if (cachedImage) {
            const Gdiplus::Rect drawRect(
                targetRect.left,
                targetRect.top,
                targetRect.right - targetRect.left,
                targetRect.bottom - targetRect.top);
            drewImage = (graphics.DrawImage(cachedImage.get(), drawRect) == Gdiplus::Ok);
        }
    }
    return drewImage;
}

bool SaveCustomChatBlockerImage(AppContext* ctx, const std::filesystem::path& sourcePath, std::wstring& error)
{
    error.clear();
    if (!ctx || sourcePath.empty()) {
        error = L"No image selected.";
        return false;
    }
    try {
        int sourceWidth = 0;
        int sourceHeight = 0;
        if (!TryReadImageDimensions(sourcePath, sourceWidth, sourceHeight)) {
            error = L"Unable to read image dimensions from selected file.";
            return false;
        }

        const auto imagesDirectory = ResolveChatBlockerImagesDirectory(ctx);
        if (imagesDirectory.empty()) {
            error = L"Could not resolve chat blocker images folder.";
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(imagesDirectory, ec);
        if (ec) {
            error = std::wstring(L"Failed to create chat blocker images folder: ") + ToWide(ec.message());
            return false;
        }

        const std::filesystem::path targetPath = imagesDirectory / sourcePath.filename();
        const bool targetAlreadyExists = std::filesystem::exists(targetPath, ec) && !ec;
        if (targetAlreadyExists) {
            const int overwriteChoice = MessageBoxW(
                ctx->mainWindow,
                (std::wstring(L"An image named '") + targetPath.filename().wstring()
                    + L"' already exists.\n\nOverwrite existing file?").c_str(),
                L"Image Already Imported",
                MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
            if (overwriteChoice != IDYES) {
                error = L"Import canceled.";
                return false;
            }
        }
        std::filesystem::copy_file(sourcePath, targetPath, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = std::wstring(L"Failed to copy image into chat blocker folder: ") + ToWide(ec.message());
            return false;
        }

        RefreshChatBlockerImageCombo(ctx, targetPath.filename().wstring());
        ctx->chatBlockerCustomSourceWidth = sourceWidth;
        ctx->chatBlockerCustomSourceHeight = sourceHeight;
        SetWindowTextW(ctx->chatBlockerWidthEdit, ToWide(std::to_string(sourceWidth)).c_str());
        SetWindowTextW(ctx->chatBlockerHeightEdit, ToWide(std::to_string(sourceHeight)).c_str());
        return true;
    } catch (const std::exception& ex) {
        error = std::wstring(L"Unexpected error importing image: ") + ToWide(ex.what());
        return false;
    } catch (...) {
        error = L"Unexpected unknown error importing image.";
        return false;
    }
}

