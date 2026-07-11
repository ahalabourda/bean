#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bean::core {

struct RunRecord {
    struct Participant {
        std::string guid;
        std::optional<std::string> name;
        std::optional<std::string> realm;
        std::optional<std::string> region;
        std::optional<int> specId;
        std::optional<std::string> specName;
        std::optional<std::string> className;
    };

    std::filesystem::path videoPath;
    std::string videoFileName;
    std::string triggerReason;
    std::string stopReason;
    std::string result;
    std::chrono::system_clock::time_point recordingStartedAt{};
    std::chrono::system_clock::time_point recordingEndedAt{};
    std::optional<std::chrono::system_clock::time_point> mythicRunStartedAt;
    std::optional<std::chrono::system_clock::time_point> mythicRunEndedAt;
    std::optional<int> challengeMapId;
    std::optional<int> keystoneLevel;
    std::optional<std::string> dungeonName;
    std::vector<Participant> participants;
};

class RunRepository {
public:
    RunRepository();
    explicit RunRepository(std::filesystem::path dbPath);

    bool Initialize(std::string& error);
    std::optional<RunRecord> GetRunByVideoPath(const std::filesystem::path& videoPath, std::string& error);
    bool UpsertRun(const RunRecord& record, std::string& error);
    std::filesystem::path GetDatabasePath() const;

private:
    bool EnsureInitialized(std::string& error);

    std::filesystem::path dbPath_;
    bool initialized_ = false;
    std::mutex mutex_;
};

} // namespace bean::core
