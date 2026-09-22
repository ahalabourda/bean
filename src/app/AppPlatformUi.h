#pragma once

#include "app/AppContext.h"

#include <optional>
#include <vector>

struct WowWindowUiInfo {
    HWND window = nullptr;
    bean::core::WowEdition edition = bean::core::WowEdition::Unknown;
    bool retailWindowDetected = false;
    bool ptrWindowDetected = false;
};

struct WowClientInfo {
    int width = 0;
    int height = 0;
    bean::core::WowEdition edition = bean::core::WowEdition::Unknown;
    bool bothInstancesDetected = false;
};

std::vector<MicrophoneOption> EnumerateMicrophoneOptions();
void RefreshMicrophoneOptionsUi(AppContext* ctx);
void RefreshMicrophoneDeviceOptionsUi(AppContext* ctx);

LRESULT CALLBACK PanelMessageForwarder(
    HWND panel,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR refData);

WowWindowUiInfo FindWowWindowForUi();
std::optional<WowClientInfo> GetWowClientSizeForUi();
void RefreshRecordingResolutionOptions(AppContext* ctx);
