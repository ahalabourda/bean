#pragma once

#include "obs/IRecorderEngine.h"

#include <mutex>
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

private:
    mutable std::mutex mutex_;
    RecordingConfig config_{};
    bool initialized_ = false;
    bool recording_ = false;
    std::string activeFileStem_;
    std::string lastStartDiagnostics_;
};

} // namespace bean::obs
