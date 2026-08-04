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

struct YouTubeMediaFile {
    std::filesystem::path path;
    YouTubeMediaType type = YouTubeMediaType::Recording;
    std::filesystem::file_time_type modified{};
    uintmax_t size = 0;
};

// .mkv / .mp4 files in a folder, newest write-time first. Shared by the
// Recordings list and the Clips source combo.
std::vector<std::filesystem::path> EnumerateRecordingMediaFiles(const std::filesystem::path& folder);
std::vector<YouTubeMediaFile> EnumerateYouTubeMediaFiles(const std::filesystem::path& recordingsFolder);

std::wstring FormatElapsed(std::chrono::seconds elapsed);
bool ParseClipTime(const std::wstring& input, int& outSeconds);
std::wstring FormatBytes(uintmax_t bytes);
std::wstring FormatLocalDateTime(const std::chrono::system_clock::time_point& timePoint);
std::wstring FormatLocalDate(const std::chrono::system_clock::time_point& timePoint);
std::chrono::system_clock::time_point FileTimeToSystemClock(const std::filesystem::file_time_type& fileTime);
std::wstring SpecAbbreviationFromName(const std::optional<std::string>& specName);
bool IsLikelyInvalidParticipantName(const std::wstring& name);
COLORREF ClassColorForParticipant(const std::optional<std::string>& className);
