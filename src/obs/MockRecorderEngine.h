#pragma once

#include "obs/IRecorderEngine.h"

#include <mutex>
#include <optional>
#include <string>

namespace bean::obs {

class MockRecorderEngine final : public IRecorderEngine {
public:
    bool Initialize(const RecordingConfig& config, std::string& error) override;
    bool StartRecording(const std::string& fileStem, std::string& error) override;
    bool StopRecording(std::string& error) override;
    bool SetMicrophoneNoiseSuppressionEnabled(bool enabled, std::string& error) override;
    bool IsRecording() const override;
    std::string GetLastStartDiagnostics() const override;
    void Shutdown() override;
    bool IsInitialized() const;

    // Test hooks: inject the failure modes the real engine enforces so
    // orchestrator error paths are exercisable without OBS.
    void SetFailNextInitialize(std::string errorMessage);
    void SetFailNextStart(std::string errorMessage);
    void SetRequireWowWindow(bool require);
    void SetWowWindowPresent(bool present);
    void ClearInjectedFailures();

private:
    mutable std::mutex mutex_;
    RecordingConfig config_{};
    bool initialized_ = false;
    bool recording_ = false;
    std::string activeFileStem_;
    std::string lastStartDiagnostics_;
    std::optional<std::string> failNextInitialize_;
    std::optional<std::string> failNextStart_;
    bool requireWowWindow_ = false;
    bool wowWindowPresent_ = true;
};

} // namespace bean::obs
