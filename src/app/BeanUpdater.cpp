#include "app/BeanUpdater.h"

#include <windows.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#ifdef BEAN_ENABLE_VELOPACK
#include "Velopack.h"
#endif

namespace bean::app {
namespace {

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }

    const int charsNeeded = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (charsNeeded <= 0) {
        return {};
    }

    std::wstring converted(static_cast<size_t>(charsNeeded), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, converted.data(), charsNeeded);
    if (!converted.empty() && converted.back() == L'\0') {
        converted.pop_back();
    }
    return converted;
}

std::optional<std::string> ResolveUpdateSourceUrl()
{
    if (const char* envValue = std::getenv("BEAN_UPDATE_URL"); envValue && envValue[0] != '\0') {
        return std::string(envValue);
    }
#ifdef BEAN_VELOPACK_UPDATE_URL
    return std::string(BEAN_VELOPACK_UPDATE_URL);
#else
    return std::nullopt;
#endif
}

std::wstring GetLastVelopackError()
{
#ifdef BEAN_ENABLE_VELOPACK
    std::vector<char> errorBuffer(1024, '\0');
    size_t written = vpkc_get_last_error(errorBuffer.data(), errorBuffer.size());
    if (written == 0) {
        return L"Unknown Velopack error.";
    }
    if (written > errorBuffer.size()) {
        errorBuffer.assign(written, '\0');
        written = vpkc_get_last_error(errorBuffer.data(), errorBuffer.size());
        if (written == 0) {
            return L"Unknown Velopack error.";
        }
    }
    return Utf8ToWide(std::string(errorBuffer.data()));
#else
    return L"Unknown Velopack error.";
#endif
}

#ifdef BEAN_ENABLE_VELOPACK
std::string NormalizeGithubRepositoryUrl(const std::string& value)
{
    constexpr char kGithubPrefix[] = "https://github.com/";
    if (value.rfind(kGithubPrefix, 0) != 0) {
        return {};
    }

    std::string repository = value.substr(sizeof(kGithubPrefix) - 1);
    while (!repository.empty() && repository.back() == '/') {
        repository.pop_back();
    }
    constexpr char kReleasesSuffix[] = "/releases";
    if (repository.size() >= sizeof(kReleasesSuffix) - 1
        && repository.compare(
            repository.size() - (sizeof(kReleasesSuffix) - 1),
            sizeof(kReleasesSuffix) - 1,
            kReleasesSuffix)
            == 0) {
        repository.erase(repository.size() - (sizeof(kReleasesSuffix) - 1));
    }
    return std::string(kGithubPrefix) + repository;
}

bool CreateUpdateManager(const std::string& sourceUrl, vpkc_update_manager_t** manager)
{
    const std::string githubRepositoryUrl = NormalizeGithubRepositoryUrl(sourceUrl);
    if (!githubRepositoryUrl.empty()) {
        vpkc_update_source_t* source = vpkc_new_source_github(githubRepositoryUrl.c_str(), nullptr, false);
        if (!source) {
            return false;
        }
        if (vpkc_new_update_manager_with_source(source, nullptr, nullptr, manager)) {
            return true;
        }
        vpkc_free_source(source);
        return false;
    }

    return vpkc_new_update_manager(sourceUrl.c_str(), nullptr, nullptr, manager);
}
#endif

} // namespace

bool InitializeVelopackRuntime(std::wstring& warning)
{
#ifdef BEAN_ENABLE_VELOPACK
    warning.clear();
    vpkc_app_run(nullptr);
    return true;
#else
    warning.clear();
    return true;
#endif
}

UpdateAvailability GetUpdateAvailability(std::wstring& statusMessage)
{
#ifndef BEAN_ENABLE_VELOPACK
    statusMessage = L"Updates are not enabled in this build.";
    return UpdateAvailability::NotConfigured;
#else
    const auto updateSource = ResolveUpdateSourceUrl();
    if (!updateSource.has_value()) {
        statusMessage = L"Set BEAN_UPDATE_URL (or BEAN_VELOPACK_UPDATE_URL at build time) to enable updates.";
        return UpdateAvailability::NotConfigured;
    }

    vpkc_update_manager_t* manager = nullptr;
    if (!CreateUpdateManager(*updateSource, &manager) || !manager) {
        statusMessage = L"Update manager init failed: " + GetLastVelopackError();
        return UpdateAvailability::Failed;
    }

    vpkc_asset_t* pendingAsset = nullptr;
    if (vpkc_update_pending_restart(manager, &pendingAsset) && pendingAsset) {
        vpkc_free_asset(pendingAsset);
        vpkc_free_update_manager(manager);
        statusMessage = L"An update is ready to install.";
        return UpdateAvailability::UpdateAvailable;
    }

    vpkc_update_info_t* updateInfo = nullptr;
    const vpkc_update_check_t checkResult = vpkc_check_for_updates(manager, &updateInfo);
    if (checkResult == NO_UPDATE_AVAILABLE || checkResult == REMOTE_IS_EMPTY) {
        vpkc_free_update_manager(manager);
        statusMessage = L"You're already on the latest version.";
        return UpdateAvailability::UpToDate;
    }
    if (checkResult != UPDATE_AVAILABLE || !updateInfo || !updateInfo->TargetFullRelease) {
        vpkc_free_update_info(updateInfo);
        vpkc_free_update_manager(manager);
        statusMessage = L"Update check failed: " + GetLastVelopackError();
        return UpdateAvailability::Failed;
    }
    const std::wstring availableVersion = updateInfo->TargetFullRelease->Version
        ? Utf8ToWide(updateInfo->TargetFullRelease->Version)
        : std::wstring();
    vpkc_free_update_info(updateInfo);
    vpkc_free_update_manager(manager);
    statusMessage = L"New update available";
    if (!availableVersion.empty()) {
        statusMessage += L": v" + availableVersion;
    }
    return UpdateAvailability::UpdateAvailable;
#endif
}

UpdateApplyResult ApplyUpdate(std::wstring& statusMessage)
{
#ifndef BEAN_ENABLE_VELOPACK
    statusMessage = L"Updates are not enabled in this build.";
    return UpdateApplyResult::NotConfigured;
#else
    const auto updateSource = ResolveUpdateSourceUrl();
    if (!updateSource.has_value()) {
        statusMessage = L"Set BEAN_UPDATE_URL (or BEAN_VELOPACK_UPDATE_URL at build time) to enable updates.";
        return UpdateApplyResult::NotConfigured;
    }

    vpkc_update_manager_t* manager = nullptr;
    if (!CreateUpdateManager(*updateSource, &manager) || !manager) {
        statusMessage = L"Update manager init failed: " + GetLastVelopackError();
        return UpdateApplyResult::Failed;
    }

    vpkc_asset_t* pendingAsset = nullptr;
    if (vpkc_update_pending_restart(manager, &pendingAsset) && pendingAsset) {
        const bool applyStarted = vpkc_wait_exit_then_apply_updates(manager, pendingAsset, false, true, nullptr, 0);
        vpkc_free_asset(pendingAsset);
        vpkc_free_update_manager(manager);
        if (!applyStarted) {
            statusMessage = L"Failed to apply pending update: " + GetLastVelopackError();
            return UpdateApplyResult::Failed;
        }
        statusMessage = L"Applying a previously downloaded update...";
        return UpdateApplyResult::UpdateReadyAndExitRequested;
    }

    vpkc_update_info_t* updateInfo = nullptr;
    const vpkc_update_check_t checkResult = vpkc_check_for_updates(manager, &updateInfo);
    if (checkResult == NO_UPDATE_AVAILABLE || checkResult == REMOTE_IS_EMPTY) {
        vpkc_free_update_manager(manager);
        statusMessage = L"You're already on the latest version.";
        return UpdateApplyResult::NoUpdateAvailable;
    }
    if (checkResult != UPDATE_AVAILABLE || !updateInfo || !updateInfo->TargetFullRelease) {
        vpkc_free_update_info(updateInfo);
        vpkc_free_update_manager(manager);
        statusMessage = L"Update check failed: " + GetLastVelopackError();
        return UpdateApplyResult::Failed;
    }

    if (!vpkc_download_updates(manager, updateInfo, nullptr, nullptr)) {
        vpkc_free_update_info(updateInfo);
        vpkc_free_update_manager(manager);
        statusMessage = L"Update download failed: " + GetLastVelopackError();
        return UpdateApplyResult::Failed;
    }

    const bool applyStarted = vpkc_wait_exit_then_apply_updates(
        manager,
        updateInfo->TargetFullRelease,
        false,
        true,
        nullptr,
        0);
    vpkc_free_update_info(updateInfo);
    vpkc_free_update_manager(manager);
    if (!applyStarted) {
        statusMessage = L"Failed to start update apply: " + GetLastVelopackError();
        return UpdateApplyResult::Failed;
    }

    statusMessage = L"Update downloaded. Restarting to apply...";
    return UpdateApplyResult::UpdateReadyAndExitRequested;
#endif
}

} // namespace bean::app
