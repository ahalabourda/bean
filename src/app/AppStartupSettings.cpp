#include "app/AppStartupSettings.h"

#include "app/AppContext.h"
#include "app/AppUtilities.h"
#include "core/GameEnvironment.h"

#include <algorithm>
#include <filesystem>
#include <shlobj.h>

bool ApplyReasonableDefaults(bean::core::AppSettings& settings, std::string& warning)
{
    warning.clear();
    bool changed = false;

    if (settings.outputDirectory.empty()) {
        const auto videosPath = GetKnownFolderPath(FOLDERID_Videos);
        if (!videosPath.empty()) {
            const auto output = std::filesystem::path(videosPath) / "Bean";
            std::error_code ec;
            std::filesystem::create_directories(output, ec);
            if (!ec) {
                settings.outputDirectory = output;
                changed = true;
            } else {
                warning = "Could not create default output folder in Videos.";
            }
        } else {
            warning = "Could not resolve Videos folder for default output path.";
        }
    }

    if (settings.wowInstallDirectory.empty()) {
        settings.wowInstallDirectory = bean::core::ResolveDefaultWowInstallDirectory();
        changed = true;
    }
    if (settings.videoEncoder.empty()) {
        settings.videoEncoder = "gpu_auto";
        changed = true;
    }
    if (settings.encoderPreset == "quality") {
        settings.encoderPreset = "high";
        changed = true;
    } else if (settings.encoderPreset == "balanced") {
        settings.encoderPreset = "medium";
        changed = true;
    } else if (settings.encoderPreset == "speed") {
        settings.encoderPreset = "low";
        changed = true;
    } else if (settings.encoderPreset != "ultra"
        && settings.encoderPreset != "high"
        && settings.encoderPreset != "medium"
        && settings.encoderPreset != "low"
        && settings.encoderPreset != "minimum") {
        settings.encoderPreset = "high";
        changed = true;
    }
    if (settings.videoContainer.empty()) {
        settings.videoContainer = "mp4";
        changed = true;
    }
    if (settings.postRunStopDelaySeconds < 0) {
        settings.postRunStopDelaySeconds = 30;
        changed = true;
    }
    if (settings.clipDurationSeconds < 1 || settings.clipDurationSeconds > 3600) {
        settings.clipDurationSeconds = 30;
        changed = true;
    }
    if (settings.chatBlockerWidth < 0) {
        settings.chatBlockerWidth = 0;
        changed = true;
    }
    if (settings.chatBlockerHeight < 0) {
        settings.chatBlockerHeight = 0;
        changed = true;
    }
    if (settings.chatBlockerCustomImageSourceWidth < 0) {
        settings.chatBlockerCustomImageSourceWidth = 0;
        changed = true;
    }
    if (settings.chatBlockerCustomImageSourceHeight < 0) {
        settings.chatBlockerCustomImageSourceHeight = 0;
        changed = true;
    }
    if (!settings.chatBlockerUseCustomImage) {
        if (settings.chatBlockerCustomImageSourceWidth != 0
            || settings.chatBlockerCustomImageSourceHeight != 0) {
            settings.chatBlockerCustomImageSourceWidth = 0;
            settings.chatBlockerCustomImageSourceHeight = 0;
            changed = true;
        }
    } else if (settings.chatBlockerCustomImagePath.empty()) {
        settings.chatBlockerUseCustomImage = false;
        settings.chatBlockerCustomImageSourceWidth = 0;
        settings.chatBlockerCustomImageSourceHeight = 0;
        changed = true;
    } else {
        std::error_code customImageEc;
        if (!std::filesystem::exists(settings.chatBlockerCustomImagePath, customImageEc)
            || customImageEc) {
            settings.chatBlockerUseCustomImage = false;
            settings.chatBlockerCustomImagePath.clear();
            settings.chatBlockerCustomImageSourceWidth = 0;
            settings.chatBlockerCustomImageSourceHeight = 0;
            changed = true;
        }
    }
    if (settings.microphoneDeviceId.empty()) {
        settings.microphoneDeviceId = "default";
        changed = true;
    }
    const auto* theme = FindThemeDefinition(settings.theme);
    if (!theme || settings.theme != theme->id) {
        settings.theme = bean::core::kDefaultTheme;
        changed = true;
    }

    return changed;
}
