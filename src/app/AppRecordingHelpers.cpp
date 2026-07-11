#include "app/AppContext.h"
#include "app/AppRecordingHelpers.h"

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace {

std::wstring ToWideUtf8(const std::string& input)
{
    if (input.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, output.data(), size);
    if (!output.empty() && output.back() == L'\0') {
        output.pop_back();
    }
    return output;
}

} // namespace

std::wstring FormatElapsed(std::chrono::seconds elapsed)
{
    const auto total = elapsed.count();
    const int hours = static_cast<int>(total / 3600);
    const int minutes = static_cast<int>((total % 3600) / 60);
    const int seconds = static_cast<int>(total % 60);

    wchar_t buffer[32] = {};
    if (hours > 0) {
        swprintf_s(buffer, L"%02d:%02d:%02d", hours, minutes, seconds);
    } else {
        swprintf_s(buffer, L"%02d:%02d", minutes, seconds);
    }
    return buffer;
}

std::string DungeonNameForChallengeMap(int challengeMapId)
{
    switch (challengeMapId) {
    case 161: return "Skyreach";
    case 239: return "Seat of the Triumvirate";
    case 402: return "Algeth'ar Academy";
    case 556: return "Pit of Saron";
    case 557: return "Windrunner Spire";
    case 558: return "Magisters' Terrace";
    case 559: return "Nexus-Point Xenas";
    case 560: return "Maisara Caverns";
    default: return {};
    }
}

std::wstring FormatBytes(uintmax_t bytes)
{
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

    std::wostringstream os;
    os << std::fixed << std::setprecision(1);
    if (bytes >= static_cast<uintmax_t>(kGiB)) {
        os << (static_cast<double>(bytes) / kGiB) << L" GB";
    } else if (bytes >= static_cast<uintmax_t>(kMiB)) {
        os << (static_cast<double>(bytes) / kMiB) << L" MB";
    } else if (bytes >= static_cast<uintmax_t>(kKiB)) {
        os << (static_cast<double>(bytes) / kKiB) << L" KB";
    } else {
        os << bytes << L" B";
    }
    return os.str();
}

std::wstring FormatLocalDateTime(const std::chrono::system_clock::time_point& timePoint)
{
    const auto tt = std::chrono::system_clock::to_time_t(timePoint);
    std::tm tm{};
    localtime_s(&tm, &tt);

    std::wostringstream os;
    os << std::put_time(&tm, L"%Y-%m-%d %H:%M:%S");
    return os.str();
}

std::wstring FormatLocalDate(const std::chrono::system_clock::time_point& timePoint)
{
    const auto tt = std::chrono::system_clock::to_time_t(timePoint);
    std::tm tm{};
    localtime_s(&tm, &tt);

    static constexpr const wchar_t* kMonthNames[12] = {
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"
    };

    const int monthIndex = (std::clamp)(tm.tm_mon, 0, 11);
    std::wostringstream os;
    os << kMonthNames[monthIndex] << L" " << tm.tm_mday << L", " << (tm.tm_year + 1900);
    return os.str();
}

std::chrono::system_clock::time_point FileTimeToSystemClock(const std::filesystem::file_time_type& fileTime)
{
    using namespace std::chrono;
    const auto adjusted = fileTime - std::filesystem::file_time_type::clock::now() + system_clock::now();
    return time_point_cast<system_clock::duration>(adjusted);
}

std::wstring SpecAbbreviationFromName(const std::optional<std::string>& specName)
{
    if (!specName.has_value() || specName->empty()) {
        return {};
    }
    const std::wstring wideSpecName = ToWideUtf8(*specName);
    if (wideSpecName.empty()) {
        return {};
    }

    std::wstring initials;
    bool atWordStart = true;
    for (wchar_t ch : wideSpecName) {
        if (std::iswalpha(ch)) {
            if (atWordStart) {
                initials.push_back(static_cast<wchar_t>(std::towupper(ch)));
            }
            atWordStart = false;
        } else {
            atWordStart = true;
        }
    }

    if (initials.size() >= 2) {
        return initials.substr(0, 3);
    }

    std::wstring compact;
    for (wchar_t ch : wideSpecName) {
        if (std::iswalpha(ch)) {
            compact.push_back(static_cast<wchar_t>(std::towupper(ch)));
        }
        if (compact.size() >= 3) {
            break;
        }
    }
    return compact;
}

bool IsLikelyInvalidParticipantName(const std::wstring& name)
{
    if (name.empty()) {
        return true;
    }
    bool allDigits = true;
    for (wchar_t ch : name) {
        if (!std::iswdigit(ch)) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        return true;
    }
    if (name.size() > 2 && name[0] == L'0' && (name[1] == L'x' || name[1] == L'X')) {
        return true;
    }
    return false;
}

COLORREF ClassColorForParticipant(const std::optional<std::string>& className)
{
    if (!className.has_value() || className->empty()) {
        return kColorTextMuted;
    }
    const char* value = className->c_str();
    if (_stricmp(value, "Death Knight") == 0) {
        return RGB(196, 31, 59);
    }
    if (_stricmp(value, "Demon Hunter") == 0) {
        return RGB(163, 48, 201);
    }
    if (_stricmp(value, "Druid") == 0) {
        return RGB(255, 124, 10);
    }
    if (_stricmp(value, "Evoker") == 0) {
        return RGB(51, 147, 127);
    }
    if (_stricmp(value, "Hunter") == 0) {
        return RGB(170, 211, 114);
    }
    if (_stricmp(value, "Mage") == 0) {
        return RGB(63, 199, 235);
    }
    if (_stricmp(value, "Monk") == 0) {
        return RGB(0, 255, 150);
    }
    if (_stricmp(value, "Paladin") == 0) {
        return RGB(244, 140, 186);
    }
    if (_stricmp(value, "Priest") == 0) {
        return RGB(240, 240, 240);
    }
    if (_stricmp(value, "Rogue") == 0) {
        return RGB(255, 244, 104);
    }
    if (_stricmp(value, "Shaman") == 0) {
        return RGB(0, 112, 221);
    }
    if (_stricmp(value, "Warlock") == 0) {
        return RGB(135, 136, 238);
    }
    if (_stricmp(value, "Warrior") == 0) {
        return RGB(198, 155, 109);
    }
    return kColorTextPrimary;
}
