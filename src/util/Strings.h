#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace bean::util {

// UTF-8 <-> UTF-16 conversion. These were previously copy-pasted into five
// translation units; keep the single definition here.
std::wstring ToWide(std::string_view utf8);

// Overloaded on concrete types rather than a single wstring_view parameter:
// std::wstring converts to both wstring_view and std::filesystem::path, and a
// lone view overload alongside the path one is ambiguous for the common case.
std::string ToUtf8(const std::wstring& wide);
std::string ToUtf8(const wchar_t* wide);
std::string ToUtf8(const std::filesystem::path& path);

// Removes leading and trailing spaces, tabs, carriage returns and newlines.
std::string Trim(std::string_view value);

} // namespace bean::util
