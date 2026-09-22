#include "app/AppKeybinds.h"

#include "app/AppStatusLog.h"
#include "app/AppUtilities.h"
#include "util/Strings.h"

#include <array>
#include <string>

using bean::util::ToWide;

bean::core::Keybind* KeybindForIndex(AppContext* ctx, int index)
{
    if (!ctx || index < 0 || index >= 3) {
        return nullptr;
    }
    switch (index) {
    case 0: return &ctx->settings.clipKeybind;
    case 1: return &ctx->settings.manualStartKeybind;
    case 2: return &ctx->settings.manualStopKeybind;
    default: return nullptr;
    }
}

std::wstring VirtualKeyName(UINT virtualKey)
{
    if (virtualKey >= '0' && virtualKey <= '9') {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return L"F" + std::to_wstring(virtualKey - VK_F1 + 1);
    }
    switch (virtualKey) {
    case VK_SPACE: return L"Space";
    case VK_RETURN: return L"Enter";
    case VK_ESCAPE: return L"Esc";
    case VK_TAB: return L"Tab";
    case VK_BACK: return L"Backspace";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_NUMPAD0: case VK_NUMPAD1: case VK_NUMPAD2: case VK_NUMPAD3: case VK_NUMPAD4:
    case VK_NUMPAD5: case VK_NUMPAD6: case VK_NUMPAD7: case VK_NUMPAD8: case VK_NUMPAD9:
        return L"Num " + std::to_wstring(virtualKey - VK_NUMPAD0);
    default:
        break;
    }
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    wchar_t name[64] = {};
    if (scanCode != 0 && GetKeyNameTextW(static_cast<LONG>(scanCode << 16), name, static_cast<int>(std::size(name))) > 0) {
        return name;
    }
    return L"Key " + std::to_wstring(virtualKey);
}

std::wstring FormatKeybind(const bean::core::Keybind& keybind)
{
    if (!keybind.IsBound()) {
        return L"Unbound";
    }
    std::wstring result;
    const auto addModifier = [&result](const wchar_t* modifier) {
        if (!result.empty()) {
            result += L"+";
        }
        result += modifier;
    };
    if ((keybind.modifiers & MOD_CONTROL) != 0) addModifier(L"Ctrl");
    if ((keybind.modifiers & MOD_ALT) != 0) addModifier(L"Alt");
    if ((keybind.modifiers & MOD_SHIFT) != 0) addModifier(L"Shift");
    if ((keybind.modifiers & MOD_WIN) != 0) addModifier(L"Win");
    if (!result.empty()) {
        result += L"+";
    }
    result += VirtualKeyName(keybind.virtualKey);
    return result;
}

void PushKeybindsToUi(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    for (int index = 0; index < 3; ++index) {
        const auto* keybind = KeybindForIndex(ctx, index);
        if (keybind && ctx->keybindValueLabels[static_cast<size_t>(index)]) {
            const auto text = FormatKeybind(*keybind);
            UpdateTransparentStaticText(
                ctx->keybindValueLabels[static_cast<size_t>(index)],
                text.c_str());
        }
    }
}

void RegisterConfiguredHotkeys(AppContext* ctx)
{
    if (!ctx || !ctx->mainWindow) {
        return;
    }
    UnregisterHotKey(ctx->mainWindow, kClipHotkeyId);
    UnregisterHotKey(ctx->mainWindow, kManualStartHotkeyId);
    UnregisterHotKey(ctx->mainWindow, kManualStopHotkeyId);
    const int hotkeyIds[] = {kClipHotkeyId, kManualStartHotkeyId, kManualStopHotkeyId};
    for (int index = 0; index < 3; ++index) {
        const auto* keybind = KeybindForIndex(ctx, index);
        if (!keybind || !keybind->IsBound()) {
            continue;
        }
        if (!RegisterHotKey(
                ctx->mainWindow,
                hotkeyIds[index],
                static_cast<UINT>(keybind->modifiers) | MOD_NOREPEAT,
                static_cast<UINT>(keybind->virtualKey))) {
            SetStatus(ctx, std::wstring(L"Could not register ") + FormatKeybind(*keybind) + L"; it may already be in use.");
        }
    }
}

void SaveKeybindSettings(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    std::string error;
    if (!ctx->settingsStore.Save(ctx->settings, error)) {
        SetStatus(ctx, std::wstring(L"Keybind save failed: ") + ToWide(error));
        return;
    }
    ctx->orchestrator->ApplySettings(ctx->settings);
    PushKeybindsToUi(ctx);
    RegisterConfiguredHotkeys(ctx);
}

void BeginKeybindCapture(AppContext* ctx, int index)
{
    if (!ctx || !KeybindForIndex(ctx, index)) {
        return;
    }
    UnregisterHotKey(ctx->mainWindow, kClipHotkeyId);
    UnregisterHotKey(ctx->mainWindow, kManualStartHotkeyId);
    UnregisterHotKey(ctx->mainWindow, kManualStopHotkeyId);
    ctx->listeningKeybindIndex = index;
    const auto button = ctx->keybindRebindButtons[static_cast<size_t>(index)];
    if (button) {
        SetWindowTextW(button, L"Press a key...");
    }
    SetStatus(ctx, L"Listening for the next key combination...");
    SetFocus(ctx->mainWindow);
}

void UnbindKeybind(AppContext* ctx, int index)
{
    if (auto* keybind = KeybindForIndex(ctx, index)) {
        *keybind = {};
        if (ctx->listeningKeybindIndex == index) {
            ctx->listeningKeybindIndex.reset();
        }
        SaveKeybindSettings(ctx);
        SetStatus(ctx, L"Keybind unbound.");
    }
}

void ResetKeybind(AppContext* ctx, int index)
{
    if (!ctx) {
        return;
    }
    static constexpr bean::core::Keybind defaults[] = {
        {MOD_CONTROL | MOD_SHIFT, VK_F8},
        {MOD_CONTROL | MOD_SHIFT, VK_F9},
        {MOD_CONTROL | MOD_SHIFT, VK_F10}
    };
    if (auto* keybind = KeybindForIndex(ctx, index)) {
        *keybind = defaults[index];
        if (ctx->listeningKeybindIndex == index) {
            ctx->listeningKeybindIndex.reset();
        }
        SaveKeybindSettings(ctx);
        SetStatus(ctx, L"Keybind reset to its default.");
    }
}

bool CaptureKeybindKey(AppContext* ctx, WPARAM virtualKey)
{
    if (!ctx || !ctx->listeningKeybindIndex.has_value()) {
        return false;
    }
    switch (virtualKey) {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        break;
    }
    auto* keybind = KeybindForIndex(ctx, *ctx->listeningKeybindIndex);
    if (!keybind) {
        ctx->listeningKeybindIndex.reset();
        return true;
    }
    UINT modifiers = 0;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) modifiers |= MOD_CONTROL;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= MOD_ALT;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0) modifiers |= MOD_WIN;
    *keybind = {
        modifiers,
        static_cast<std::uint32_t>(virtualKey)
    };
    ctx->listeningKeybindIndex.reset();
    for (const auto button : ctx->keybindRebindButtons) {
        if (button) {
            SetWindowTextW(button, L"Rebind");
        }
    }
    SaveKeybindSettings(ctx);
    SetStatus(ctx, L"Keybind saved.");
    return true;
}

