#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

enum class YouTubeMediaType {
    Recording,
    Clip
};

enum class YouTubeMediaSortColumn {
    Type,
    Name,
    Date
};

enum class RecordingKind {
    Manual,
    MythicPlus,
    Raid,
    Pvp
};

enum class RecordingOutcomeFilter {
    Any,
    Timed,
    Depleted
};

struct RecordingKeyLevelFilter {
    bool active = false;
    int minLevel = 0;
    int maxLevel = 0;
};

struct RecordingFilterCriteria {
    bool includeManual = true;
    bool includeMythicPlus = true;
    bool includeRaid = true;
    bool includePvp = true;
    RecordingOutcomeFilter outcome = RecordingOutcomeFilter::Any;
    RecordingKeyLevelFilter keyLevel;
    std::vector<std::wstring> characterNames;
};

struct RecordingFilterItem {
    RecordingKind kind = RecordingKind::Manual;
    bool timed = false;
    bool depleted = false;
    int keystoneLevel = -1;
    std::vector<std::wstring> participantNames;
};

struct YouTubeMediaFile {
    std::filesystem::path path;
    YouTubeMediaType type = YouTubeMediaType::Recording;
    std::filesystem::file_time_type modified{};
    uintmax_t size = 0;
    std::string triggerReason;
};

// .mkv / .mp4 files in a folder, newest write-time first. Shared by the
// Recordings list and the Clips source combo.
std::vector<std::filesystem::path> EnumerateRecordingMediaFiles(const std::filesystem::path& folder);
std::vector<YouTubeMediaFile> EnumerateYouTubeMediaFiles(const std::filesystem::path& recordingsFolder);
void SortYouTubeMediaFiles(
    std::vector<YouTubeMediaFile>& files,
    YouTubeMediaSortColumn column,
    bool ascending);

std::wstring FormatElapsed(std::chrono::seconds elapsed);
bool ParseClipTime(const std::wstring& input, int& outSeconds);
std::wstring FormatBytes(uintmax_t bytes);
std::wstring FormatLocalDateTime(const std::chrono::system_clock::time_point& timePoint);
std::wstring FormatLocalDate(const std::chrono::system_clock::time_point& timePoint);
std::chrono::system_clock::time_point FileTimeToSystemClock(const std::filesystem::file_time_type& fileTime);
std::wstring SpecAbbreviationFromName(const std::optional<std::string>& specName);
bool IsLikelyInvalidParticipantName(const std::wstring& name);
COLORREF ClassColorForParticipant(const std::optional<std::string>& className);

RecordingKind ClassifyRecordingKind(const std::string& triggerReason, bool hasMythicMetadata);
bool ParseRecordingKeyLevelFilter(const std::wstring& input, RecordingKeyLevelFilter& outFilter);
std::vector<std::wstring> ParseRecordingCharacterNameFilter(const std::wstring& input);
bool RecordingFilterIsRestricting(const RecordingFilterCriteria& criteria);
bool RecordingMatchesFilter(const RecordingFilterItem& item, const RecordingFilterCriteria& criteria);
