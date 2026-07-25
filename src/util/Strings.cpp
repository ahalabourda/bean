#include "util/Strings.h"

#include <windows.h>

namespace bean::util {

std::wstring ToWide(std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), output.data(), size);
    return output;
}

namespace {

std::string ToUtf8View(std::wstring_view wide)
{
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), output.data(), size, nullptr, nullptr);
    return output;
}

} // namespace

std::string ToUtf8(const std::wstring& wide)
{
    return ToUtf8View(wide);
}

std::string ToUtf8(const wchar_t* wide)
{
    return wide == nullptr ? std::string() : ToUtf8View(wide);
}

std::string ToUtf8(const std::filesystem::path& path)
{
    return ToUtf8View(path.native());
}

std::string Trim(std::string_view value)
{
    constexpr std::string_view kWhitespace = " \t\r\n";
    const auto begin = value.find_first_not_of(kWhitespace);
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(kWhitespace);
    return std::string(value.substr(begin, end - begin + 1));
}

} // namespace bean::util
