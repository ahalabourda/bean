#pragma once

#include <string>

namespace bean::app {

enum class UpdateAvailability {
    NotConfigured,
    UpdateAvailable,
    UpToDate,
    Failed
};

enum class UpdateApplyResult {
    NotConfigured,
    NoUpdateAvailable,
    UpdateReadyAndExitRequested,
    Failed
};

bool InitializeVelopackRuntime(std::wstring& warning);
UpdateAvailability GetUpdateAvailability(std::wstring& statusMessage);
UpdateApplyResult ApplyUpdate(std::wstring& statusMessage);

} // namespace bean::app
