#include "app/AppUtilities.h"

#include "util/Strings.h"

#include <commdlg.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::wstring ToWideUtf8(const std::string& input)
{
    return bean::util::ToWide(input);
}

std::string BuildSpecIconKey(const std::string& className, const std::string& specName)
{
    std::string classPart;
    classPart.reserve(className.size());
    for (unsigned char ch : className) {
        if (std::isalnum(ch)) {
            classPart.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    std::string specPart;
    specPart.reserve(specName.size());
    for (unsigned char ch : specName) {
        if (std::isalnum(ch)) {
            specPart.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    if (classPart.empty() || specPart.empty()) {
        return {};
    }
    return classPart + "-" + specPart;
}

} // namespace

std::filesystem::path GetExecutableDirectory()
{
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD moduleLen = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (moduleLen == 0 || moduleLen >= std::size(modulePath)) {
        return {};
    }
    return std::filesystem::path(modulePath).parent_path();
}

std::filesystem::path SpecIconPathFromExe(const std::string& className, const std::string& specName)
{
    const std::string key = BuildSpecIconKey(className, specName);
    if (key.empty()) {
        return {};
    }

    const auto iconDirectory = GetExecutableDirectory() / "assets" / "spec-icons";
    const auto canonicalJpgPath = iconDirectory / (ToWideUtf8(key) + L".jpg");
    if (std::filesystem::exists(canonicalJpgPath)) {
        return canonicalJpgPath;
    }

    if (key == "shaman-enhancement") {
        const auto aliasPath = iconDirectory / L"spell_shaman_improvedstormstrike.jpg";
        if (std::filesystem::exists(aliasPath)) {
            return aliasPath;
        }
    }

    const auto canonicalPngPath = iconDirectory / (ToWideUtf8(key) + L".png");
    if (std::filesystem::exists(canonicalPngPath)) {
        return canonicalPngPath;
    }

    return canonicalJpgPath;
}

HBITMAP LoadPngBitmapForImageList(const std::filesystem::path& pngPath, int iconSizePx, int canvasSizePx, int verticalOffsetPx)
{
    if (pngPath.empty() || iconSizePx <= 0 || canvasSizePx <= 0 || !std::filesystem::exists(pngPath)) {
        return nullptr;
    }

    IWICImagingFactory* factory = nullptr;
    const HRESULT factoryHr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(factoryHr) || !factory) {
        return nullptr;
    }

    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = factory->CreateDecoderFromFilename(
        pngPath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &decoder);
    if (FAILED(hr) || !decoder) {
        factory->Release();
        return nullptr;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        decoder->Release();
        factory->Release();
        return nullptr;
    }

    IWICBitmapSource* source = frame;
    IWICBitmapScaler* scaler = nullptr;
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (SUCCEEDED(frame->GetSize(&sourceWidth, &sourceHeight))
        && (sourceWidth != static_cast<UINT>(iconSizePx) || sourceHeight != static_cast<UINT>(iconSizePx))) {
        hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr) && scaler) {
            hr = scaler->Initialize(frame, iconSizePx, iconSizePx, WICBitmapInterpolationModeFant);
            if (SUCCEEDED(hr)) {
                source = scaler;
            }
        }
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) {
        if (scaler) {
            scaler->Release();
        }
        frame->Release();
        decoder->Release();
        factory->Release();
        return nullptr;
    }

    hr = converter->Initialize(
        source,
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        converter->Release();
        if (scaler) {
            scaler->Release();
        }
        frame->Release();
        decoder->Release();
        factory->Release();
        return nullptr;
    }

    const int iconStride = iconSizePx * 4;
    std::vector<BYTE> iconPixels(static_cast<size_t>(iconStride * iconSizePx));
    hr = converter->CopyPixels(nullptr, iconStride, static_cast<UINT>(iconPixels.size()), iconPixels.data());
    if (FAILED(hr)) {
        converter->Release();
        if (scaler) {
            scaler->Release();
        }
        frame->Release();
        decoder->Release();
        factory->Release();
        return nullptr;
    }

    const int canvasStride = canvasSizePx * 4;
    std::vector<BYTE> canvasPixels(static_cast<size_t>(canvasStride * canvasSizePx), 0);
    const int xOffset = (std::max)(0, (canvasSizePx - iconSizePx) / 2);
    const int unclampedY = (std::max)(0, (canvasSizePx - iconSizePx) / 2) + verticalOffsetPx;
    const int yOffset = (std::clamp)(unclampedY, 0, (std::max)(0, canvasSizePx - iconSizePx));
    for (int y = 0; y < iconSizePx; ++y) {
        BYTE* dst = canvasPixels.data() + static_cast<size_t>((y + yOffset) * canvasStride + xOffset * 4);
        const BYTE* src = iconPixels.data() + static_cast<size_t>(y * iconStride);
        std::memcpy(dst, src, static_cast<size_t>(iconStride));
    }

    BITMAPV5HEADER bitmapHeader{};
    bitmapHeader.bV5Size = sizeof(bitmapHeader);
    bitmapHeader.bV5Width = canvasSizePx;
    bitmapHeader.bV5Height = -canvasSizePx;
    bitmapHeader.bV5Planes = 1;
    bitmapHeader.bV5BitCount = 32;
    bitmapHeader.bV5Compression = BI_BITFIELDS;
    bitmapHeader.bV5RedMask = 0x00FF0000;
    bitmapHeader.bV5GreenMask = 0x0000FF00;
    bitmapHeader.bV5BlueMask = 0x000000FF;
    bitmapHeader.bV5AlphaMask = 0xFF000000;

    HDC screenDc = GetDC(nullptr);
    void* dibPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        screenDc,
        reinterpret_cast<BITMAPINFO*>(&bitmapHeader),
        DIB_RGB_COLORS,
        &dibPixels,
        nullptr,
        0);
    if (screenDc) {
        ReleaseDC(nullptr, screenDc);
    }

    if (bitmap && dibPixels) {
        std::memcpy(dibPixels, canvasPixels.data(), canvasPixels.size());
    } else if (bitmap) {
        DeleteObject(bitmap);
        bitmap = nullptr;
    }

    converter->Release();
    if (scaler) {
        scaler->Release();
    }
    frame->Release();
    decoder->Release();
    factory->Release();
    return bitmap;
}

std::wstring GetWindowTextString(HWND hwnd)
{
    if (!hwnd) {
        return {};
    }
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

void UpdateTransparentStaticText(HWND control, const wchar_t* newText)
{
    if (!control || !newText) {
        return;
    }
    if (GetWindowTextString(control) == newText) {
        return;
    }

    SetWindowTextW(control, newText);

    // Transparent STATIC controls rely on parent repaint; redraw both control and
    // parent region so stale glyphs are erased before the new text is drawn.
    RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    HWND parent = GetParent(control);
    if (parent) {
        RECT rect{};
        if (GetWindowRect(control, &rect)) {
            MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rect), 2);
            RedrawWindow(parent, &rect, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        }
    }
}

void InvalidateControlAndParentRegion(HWND control)
{
    if (!control) {
        return;
    }
    // Keep redraw local to avoid full-panel flash/flicker on every interaction.
    InvalidateRect(control, nullptr, FALSE);
}

int ReadIntControl(HWND hwnd, int fallback)
{
    const auto text = GetWindowTextString(hwnd);
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

std::wstring PickFolder(HWND owner)
{
    BROWSEINFOW browseInfo{};
    browseInfo.hwndOwner = owner;
    browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    browseInfo.lpszTitle = L"Select folder";

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&browseInfo);
    if (!pidl) {
        return {};
    }

    wchar_t pathBuffer[MAX_PATH] = {};
    std::wstring selected;
    if (SHGetPathFromIDListW(pidl, pathBuffer)) {
        selected = pathBuffer;
    }
    CoTaskMemFree(pidl);
    return selected;
}

std::wstring PickImageFile(HWND owner)
{
    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW openFile{};
    openFile.lStructSize = sizeof(openFile);
    openFile.hwndOwner = owner;
    openFile.lpstrFilter =
        L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0"
        L"All Files (*.*)\0*.*\0";
    openFile.lpstrFile = filePath;
    openFile.nMaxFile = static_cast<DWORD>(std::size(filePath));
    openFile.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    openFile.lpstrTitle = L"Choose chat blocker image";
    if (!GetOpenFileNameW(&openFile)) {
        return {};
    }
    return filePath;
}

std::wstring FormatHresultHex(HRESULT hr)
{
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

std::wstring GetKnownFolderPath(REFKNOWNFOLDERID folderId)
{
    PWSTR rawPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(folderId, 0, nullptr, &rawPath)) || rawPath == nullptr) {
        if (rawPath) {
            CoTaskMemFree(rawPath);
        }
        return {};
    }

    std::wstring path(rawPath);
    CoTaskMemFree(rawPath);
    return path;
}

bool DirectoryExists(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec);
}

bool EnsureOutputDirectoryReady(const std::filesystem::path& outputDirectory, std::string& error)
{
    error.clear();
    if (outputDirectory.empty()) {
        error = "Output folder is empty.";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec) {
        error = "Could not create output folder: " + ec.message();
        return false;
    }
    return true;
}

