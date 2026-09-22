#include "app/AppPlatformUi.h"

#include "app/AppDraw.h"
#include "util/Strings.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <string>
#include <tlhelp32.h>
#include <vector>

using bean::util::ToUtf8;

LRESULT CALLBACK PanelMessageForwarder(HWND panel, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_ERASEBKGND || message == WM_PAINT) {
        RECT rect{};
        GetClientRect(panel, &rect);
        if (message == WM_ERASEBKGND) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            TRIVERTEX vertices[2] = {
                {rect.left, rect.top, static_cast<COLOR16>(GetRValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetGValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetBValue(kColorPanelTop) << 8), 0xFF00},
                {rect.right, rect.bottom, static_cast<COLOR16>(GetRValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetGValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetBValue(kColorPanelBottom) << 8), 0xFF00},
            };
            GRADIENT_RECT gradientRect{0, 1};
            if (!GradientFill(dc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V)) {
                FillRect(dc, &rect, gTheme.panelSolidBrush ? gTheme.panelSolidBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            if (gTheme.panelBorderBrush) {
                FrameRect(dc, &rect, gTheme.panelBorderBrush);
            }
            return 1;
        }

        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(panel, &paint);
        if (dc) {
            TRIVERTEX vertices[2] = {
                {rect.left, rect.top, static_cast<COLOR16>(GetRValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetGValue(kColorPanelTop) << 8), static_cast<COLOR16>(GetBValue(kColorPanelTop) << 8), 0xFF00},
                {rect.right, rect.bottom, static_cast<COLOR16>(GetRValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetGValue(kColorPanelBottom) << 8), static_cast<COLOR16>(GetBValue(kColorPanelBottom) << 8), 0xFF00},
            };
            GRADIENT_RECT gradientRect{0, 1};
            if (!GradientFill(dc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V)) {
                FillRect(dc, &rect, gTheme.panelSolidBrush ? gTheme.panelSolidBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            if (gTheme.panelBorderBrush) {
                FrameRect(dc, &rect, gTheme.panelBorderBrush);
            }
            EndPaint(panel, &paint);
            return 0;
        }
    }
    if (message == WM_COMMAND
        || message == WM_NOTIFY
        || message == WM_HSCROLL
        || message == WM_CTLCOLORSTATIC
        || message == WM_CTLCOLOREDIT
        || message == WM_CTLCOLORBTN
        || message == WM_CTLCOLORLISTBOX
        || message == WM_DRAWITEM
        || message == WM_MEASUREITEM) {
        HWND parent = GetParent(panel);
        if (parent) {
            return SendMessageW(parent, message, wParam, lParam);
        }
    }
    return DefSubclassProc(panel, message, wParam, lParam);
}

void RefreshMicrophoneOptionsUi(AppContext* ctx)
{
    if (!ctx || !ctx->microphoneCombo || !ctx->microphoneCheck) {
        return;
    }

    const bool micEnabled = (SendMessageW(ctx->microphoneCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const bool comboWasEnabled = IsWindowEnabled(ctx->microphoneCombo) != FALSE;
    EnableWindow(ctx->microphoneCombo, micEnabled ? TRUE : FALSE);
    if (comboWasEnabled != micEnabled) {
        RedrawWindow(ctx->microphoneCombo, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
    if (ctx->microphoneNoiseSuppressionCheck) {
        const bool noiseWasEnabled = IsWindowEnabled(ctx->microphoneNoiseSuppressionCheck) != FALSE;
        EnableWindow(ctx->microphoneNoiseSuppressionCheck, micEnabled ? TRUE : FALSE);
        if (noiseWasEnabled != micEnabled) {
            RedrawWindow(ctx->microphoneNoiseSuppressionCheck, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        }
    }
}

void RefreshMicrophoneDeviceOptionsUi(AppContext* ctx)
{
    if (!ctx || !ctx->microphoneCombo) {
        return;
    }

    ctx->microphoneOptions = EnumerateMicrophoneOptions();
    SendMessageW(ctx->microphoneCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& option : ctx->microphoneOptions) {
        SendMessageW(ctx->microphoneCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.displayName.c_str()));
    }

    int selectedIndex = 0;
    for (size_t i = 0; i < ctx->microphoneOptions.size(); ++i) {
        if (ctx->microphoneOptions[i].deviceId == ctx->settings.microphoneDeviceId) {
            selectedIndex = static_cast<int>(i);
            break;
        }
    }
    SendMessageW(ctx->microphoneCombo, CB_SETCURSEL, static_cast<WPARAM>(selectedIndex), 0);
    RefreshMicrophoneOptionsUi(ctx);
}


std::vector<MicrophoneOption> EnumerateMicrophoneOptions()
{
    std::vector<MicrophoneOption> options;
    options.push_back({L"Default microphone", "default"});

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return options;
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        return options;
    }

    UINT count = 0;
    if (SUCCEEDED(collection->GetCount(&count))) {
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device) {
                continue;
            }

            LPWSTR deviceId = nullptr;
            if (FAILED(device->GetId(&deviceId)) || !deviceId) {
                device->Release();
                continue;
            }

            std::wstring friendlyName = L"Microphone";
            IPropertyStore* propertyStore = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &propertyStore)) && propertyStore) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(propertyStore->GetValue(PKEY_Device_FriendlyName, &value))
                    && value.vt == VT_LPWSTR
                    && value.pwszVal
                    && value.pwszVal[0] != L'\0') {
                    friendlyName = value.pwszVal;
                }
                PropVariantClear(&value);
                propertyStore->Release();
            }

            options.push_back({friendlyName, ToUtf8(deviceId)});
            CoTaskMemFree(deviceId);
            device->Release();
        }
    }

    collection->Release();
    enumerator->Release();
    return options;
}

bean::core::WowEdition WowEditionForExecutableName(std::wstring executableName)
{
    std::transform(executableName.begin(), executableName.end(), executableName.begin(), towlower);
    if (executableName == L"wowt.exe") {
        return bean::core::WowEdition::Ptr;
    }
    if (executableName == L"wow.exe") {
        return bean::core::WowEdition::Retail;
    }
    return bean::core::WowEdition::Unknown;
}

BOOL CALLBACK FindWowWindowForUiProc(HWND hwnd, LPARAM lParam)
{
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) {
        return TRUE;
    }

    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!processHandle) {
        return TRUE;
    }

    wchar_t processPath[MAX_PATH] = {};
    DWORD processPathSize = static_cast<DWORD>(std::size(processPath));
    bean::core::WowEdition edition = bean::core::WowEdition::Unknown;
    if (QueryFullProcessImageNameW(processHandle, 0, processPath, &processPathSize)) {
        edition = WowEditionForExecutableName(
            std::filesystem::path(processPath).filename().wstring());
    }
    CloseHandle(processHandle);
    if (edition == bean::core::WowEdition::Unknown) {
        return TRUE;
    }

    wchar_t title[256] = {};
    const int len = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    if (len <= 0) {
        return TRUE;
    }

    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const bool titleLooksLikeWow = (wcsstr(title, L"World of Warcraft") != nullptr);
    const bool classLooksLikeWow = (_wcsicmp(className, L"GxWindowClass") == 0);
    if (titleLooksLikeWow || classLooksLikeWow) {
        auto* found = reinterpret_cast<WowWindowUiInfo*>(lParam);
        found->retailWindowDetected =
            found->retailWindowDetected || edition == bean::core::WowEdition::Retail;
        found->ptrWindowDetected =
            found->ptrWindowDetected || edition == bean::core::WowEdition::Ptr;
        if (!found->window
            || (edition == bean::core::WowEdition::Ptr
                && found->edition != bean::core::WowEdition::Ptr)) {
            found->window = hwnd;
            found->edition = edition;
        }
    }

    return TRUE;
}

WowWindowUiInfo FindWowWindowForUi()
{
    WowWindowUiInfo found;
    EnumWindows(FindWowWindowForUiProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

std::optional<WowClientInfo> GetWowClientSizeForUi()
{
    const WowWindowUiInfo wow = FindWowWindowForUi();
    if (!wow.window) {
        return std::nullopt;
    }

    RECT client{};
    if (!GetClientRect(wow.window, &client)) {
        return std::nullopt;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return WowClientInfo{
        width,
        height,
        wow.edition,
        wow.retailWindowDetected && wow.ptrWindowDetected};
}

std::pair<int, int> ScaleResolutionToHeight(int sourceWidth, int sourceHeight, int targetHeight)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 || targetHeight <= 0 || targetHeight >= sourceHeight) {
        return {sourceWidth, sourceHeight};
    }
    int width = static_cast<int>(
        (static_cast<long long>(sourceWidth) * targetHeight + sourceHeight / 2) / sourceHeight);
    width = (std::max)(2, width & ~1);
    return {width, (std::max)(2, targetHeight & ~1)};
}

void RefreshRecordingResolutionOptions(AppContext* ctx)
{
    if (!ctx || !ctx->recordingResolutionCombo) {
        return;
    }

    const int previousHeight = ctx->settings.recordingResolutionHeight;
    SendMessageW(ctx->recordingResolutionCombo, CB_RESETCONTENT, 0, 0);
    if (ctx->detectedWowClientWidth <= 0 || ctx->detectedWowClientHeight <= 0) {
        SendMessageW(
            ctx->recordingResolutionCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(L"Waiting for WoW..."));
        SendMessageW(ctx->recordingResolutionCombo, CB_SETCURSEL, 0, 0);
        EnableWindow(ctx->recordingResolutionCombo, FALSE);
        return;
    }

    EnableWindow(ctx->recordingResolutionCombo, !ctx->isRecording);
    const auto addOption = [&](const std::wstring& label, int targetHeight) {
        const LRESULT index = SendMessageW(
            ctx->recordingResolutionCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(ctx->recordingResolutionCombo, CB_SETITEMDATA, static_cast<WPARAM>(index), targetHeight);
    };

    addOption(
        L"Full resolution (" + std::to_wstring(ctx->detectedWowClientWidth)
            + L" x " + std::to_wstring(ctx->detectedWowClientHeight) + L")",
        0);
    constexpr int commonHeights[] = {2160, 1440, 1080, 720};
    for (const int height : commonHeights) {
        if (height >= ctx->detectedWowClientHeight) {
            continue;
        }
        const auto [width, scaledHeight] = ScaleResolutionToHeight(
            ctx->detectedWowClientWidth,
            ctx->detectedWowClientHeight,
            height);
        addOption(
            L"Downscale to " + std::to_wstring(width)
                + L" x " + std::to_wstring(scaledHeight),
            height);
    }

    LRESULT selectedIndex = 0;
    const LRESULT itemCount = SendMessageW(ctx->recordingResolutionCombo, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < itemCount; ++i) {
        if (SendMessageW(ctx->recordingResolutionCombo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0)
            == previousHeight) {
            selectedIndex = i;
            break;
        }
    }
    SendMessageW(ctx->recordingResolutionCombo, CB_SETCURSEL, static_cast<WPARAM>(selectedIndex), 0);
}

