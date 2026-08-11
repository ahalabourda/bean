#include "obs/MockRecorderEngine.h"

#include <filesystem>

namespace bean::obs {
namespace {

const char* MockAudioCaptureScopeLabel(RecordingConfig::AudioCaptureScope scope)
{
    switch (scope) {
    case RecordingConfig::AudioCaptureScope::WowAndDiscord:
        return "WoW + Discord (mock)";
    case RecordingConfig::AudioCaptureScope::AllDesktop:
        return "all desktop (mock)";
    case RecordingConfig::AudioCaptureScope::WowOnly:
    default:
        return "WoW-only (mock)";
    }
}

} // namespace

void MockRecorderEngine::SetFailNextInitialize(std::string errorMessage)
{
    std::scoped_lock lock(mutex_);
    failNextInitialize_ = std::move(errorMessage);
}

void MockRecorderEngine::SetFailNextStart(std::string errorMessage)
{
    std::scoped_lock lock(mutex_);
    failNextStart_ = std::move(errorMessage);
}

void MockRecorderEngine::SetRequireWowWindow(bool require)
{
    std::scoped_lock lock(mutex_);
    requireWowWindow_ = require;
}

void MockRecorderEngine::SetWowWindowPresent(bool present)
{
    std::scoped_lock lock(mutex_);
    wowWindowPresent_ = present;
}

void MockRecorderEngine::ClearInjectedFailures()
{
    std::scoped_lock lock(mutex_);
    failNextInitialize_.reset();
    failNextStart_.reset();
    requireWowWindow_ = false;
    wowWindowPresent_ = true;
}

bool MockRecorderEngine::Initialize(const RecordingConfig& config, std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();

    if (failNextInitialize_.has_value()) {
        error = *failNextInitialize_;
        failNextInitialize_.reset();
        initialized_ = false;
        return false;
    }

    if (config.outputDirectory.empty()) {
        error = "Output directory is not set.";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(config.outputDirectory, ec);
    if (ec) {
        error = "Failed to create output directory: " + ec.message();
        return false;
    }

    config_ = config;
    lastStartDiagnostics_.clear();
    initialized_ = true;
    return true;
}

bool MockRecorderEngine::StartRecording(const std::string& fileStem, std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();

    if (!initialized_) {
        error = "Recorder not initialized.";
        return false;
    }
    if (recording_) {
        error = "Recorder is already recording.";
        return false;
    }
    if (fileStem.empty()) {
        error = "File stem cannot be empty.";
        return false;
    }
    if (failNextStart_.has_value()) {
        error = *failNextStart_;
        failNextStart_.reset();
        return false;
    }
    if (requireWowWindow_ && !wowWindowPresent_) {
        error = "No World of Warcraft window was detected for game capture.";
        return false;
    }

    recording_ = true;
    activeFileStem_ = fileStem;
    lastStartDiagnostics_ = std::string("audio=")
        + MockAudioCaptureScopeLabel(config_.audioCaptureScope)
        + ", microphone=" + (config_.captureMicrophone ? "enabled" : "disabled")
        + ", noise-suppression=" + (config_.microphoneNoiseSuppression ? "enabled" : "disabled")
        + ", micDevice='" + (config_.microphoneDeviceId.empty() ? "default" : config_.microphoneDeviceId) + "'";
    return true;
}

bool MockRecorderEngine::StopRecording(std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();

    if (!recording_) {
        error = "Recorder is not currently recording.";
        return false;
    }

    recording_ = false;
    activeFileStem_.clear();
    return true;
}

bool MockRecorderEngine::SetMicrophoneNoiseSuppressionEnabled(bool enabled, std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();
    config_.microphoneNoiseSuppression = enabled;
    return true;
}

void MockRecorderEngine::Shutdown()
{
    std::scoped_lock lock(mutex_);
    recording_ = false;
    initialized_ = false;
    activeFileStem_.clear();
}

bool MockRecorderEngine::IsInitialized() const
{
    std::scoped_lock lock(mutex_);
    return initialized_;
}

bool MockRecorderEngine::IsRecording() const
{
    std::scoped_lock lock(mutex_);
    return recording_;
}

std::string MockRecorderEngine::GetLastStartDiagnostics() const
{
    std::scoped_lock lock(mutex_);
    return lastStartDiagnostics_;
}

} // namespace bean::obs
