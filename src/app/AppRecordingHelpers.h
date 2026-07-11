#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#include <windows.h>

std::wstring FormatElapsed(std::chrono::seconds elapsed);
std::string DungeonNameForChallengeMap(int challengeMapId);
std::wstring FormatBytes(uintmax_t bytes);
std::wstring FormatLocalDateTime(const std::chrono::system_clock::time_point& timePoint);
std::wstring FormatLocalDate(const std::chrono::system_clock::time_point& timePoint);
std::chrono::system_clock::time_point FileTimeToSystemClock(const std::filesystem::file_time_type& fileTime);
std::wstring SpecAbbreviationFromName(const std::optional<std::string>& specName);
bool IsLikelyInvalidParticipantName(const std::wstring& name);
COLORREF ClassColorForParticipant(const std::optional<std::string>& className);
