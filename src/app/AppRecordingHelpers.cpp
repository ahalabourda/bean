#include "app/AppContext.h"
#include "app/AppRecordingHelpers.h"

#include "core/WowData.h"
#include "util/Strings.h"

#include <algorithm>
#include <climits>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using bean::util::ToUtf8;
using bean::util::ToWide;

std::wstring GetControlText(HWND control)
{
    if (!control) {
        return {};
    }
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

std::wstring ToWideUtf8(const std::string& input)
{
    return ToWide(input);
}

} // namespace

std::vector<std::filesystem::path> EnumerateRecordingMediaFiles(const std::filesystem::path& folder)
{
    std::vector<std::filesystem::path> files;
    if (folder.empty()) {
        return files;
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto ext = entry.path().extension().wstring();
        if (_wcsicmp(ext.c_str(), L".mp4") != 0 && _wcsicmp(ext.c_str(), L".mkv") != 0) {
            continue;
        }
        files.push_back(entry.path());
    }

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        std::error_code aEc;
        std::error_code bEc;
        const auto aTime = std::filesystem::last_write_time(a, aEc);
        const auto bTime = std::filesystem::last_write_time(b, bEc);
        if (!aEc && !bEc) {
            return aTime > bTime;
        }
        return a.filename().wstring() < b.filename().wstring();
    });
    return files;
}

std::filesystem::path ResolveRecordingsFolderPath(const AppContext* ctx)
{
    if (!ctx) {
        return {};
    }
    std::wstring folder = GetControlText(ctx->outputEdit);
    if (folder.empty()) {
        folder = ToWide(ctx->settings.outputDirectory.string());
    }
    if (folder.empty()) {
        return {};
    }
    return std::filesystem::path(folder);
}

void AddKnownRecordingFolder(
    std::vector<std::filesystem::path>& folders,
    const std::filesystem::path& folder)
{
    if (folder.empty()) {
        return;
    }
    const auto normalized = folder.lexically_normal();
    for (const auto& existing : folders) {
        if (_wcsicmp(existing.wstring().c_str(), normalized.wstring().c_str()) == 0) {
            return;
        }
    }
    folders.push_back(normalized);
}

std::string RecordingPathKey(const std::filesystem::path& path)
{
    auto value = path.lexically_normal().wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return ToUtf8(value);
}

std::string RecordingFileNameKey(const std::filesystem::path& path)
{
    auto value = path.filename().wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return ToUtf8(value);
}

std::vector<std::filesystem::path> CollectKnownRecordingFolders(const AppContext* ctx)
{
    std::vector<std::filesystem::path> folders;
    AddKnownRecordingFolder(folders, ResolveRecordingsFolderPath(ctx));
    if (!ctx || !ctx->runRepository) {
        return folders;
    }

    std::string dbError;
    for (const auto& run : ctx->runRepository->ListRuns(dbError)) {
        AddKnownRecordingFolder(folders, run.videoPath.parent_path());
        for (const auto& alias : run.pathAliases) {
            AddKnownRecordingFolder(folders, alias.parent_path());
        }
    }
    return folders;
}

std::filesystem::path ResolveClipsOutputFolderPath(const AppContext* ctx)
{
    const auto recordingsFolder = ResolveRecordingsFolderPath(ctx);
    if (recordingsFolder.empty()) {
        return {};
    }
    return recordingsFolder / "Clips";
}

std::vector<std::filesystem::path> EnumerateRecordingMediaFilesInFolders(
    const std::vector<std::filesystem::path>& folders)
{
    std::vector<std::filesystem::path> files;
    std::unordered_set<std::wstring> seen;
    for (const auto& folder : folders) {
        for (const auto& file : EnumerateRecordingMediaFiles(folder)) {
            const auto key = file.lexically_normal().wstring();
            if (seen.insert(key).second) {
                files.push_back(file);
            }
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        std::error_code aEc;
        std::error_code bEc;
        const auto aTime = std::filesystem::last_write_time(a, aEc);
        const auto bTime = std::filesystem::last_write_time(b, bEc);
        if (!aEc && !bEc && aTime != bTime) {
            return aTime > bTime;
        }
        return _wcsicmp(a.wstring().c_str(), b.wstring().c_str()) < 0;
    });
    return files;
}

std::vector<YouTubeMediaFile> EnumerateYouTubeMediaFiles(const std::filesystem::path& recordingsFolder)
{
    std::vector<YouTubeMediaFile> files;
    if (recordingsFolder.empty()) {
        return files;
    }

    const auto appendFiles = [&files](const std::filesystem::path& folder, YouTubeMediaType type) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto ext = entry.path().extension().wstring();
            if (_wcsicmp(ext.c_str(), L".mp4") != 0 && _wcsicmp(ext.c_str(), L".mkv") != 0) {
                continue;
            }

            YouTubeMediaFile file;
            file.path = entry.path();
            file.type = type;
            std::error_code timeEc;
            file.modified = std::filesystem::last_write_time(file.path, timeEc);
            if (timeEc) {
                file.modified = std::filesystem::file_time_type{};
            }
            std::error_code sizeEc;
            file.size = std::filesystem::file_size(file.path, sizeEc);
            if (sizeEc) {
                file.size = 0;
            }
            files.push_back(std::move(file));
        }
    };

    appendFiles(recordingsFolder, YouTubeMediaType::Recording);
    appendFiles(recordingsFolder / "Clips", YouTubeMediaType::Clip);
    std::sort(files.begin(), files.end(), [](const YouTubeMediaFile& a, const YouTubeMediaFile& b) {
        if (a.modified != b.modified) {
            return a.modified > b.modified;
        }
        return a.path.filename().wstring() < b.path.filename().wstring();
    });
    return files;
}

std::vector<YouTubeMediaFile> EnumerateYouTubeMediaFilesInFolders(
    const std::vector<std::filesystem::path>& folders)
{
    std::vector<YouTubeMediaFile> files;
    std::unordered_set<std::wstring> seen;
    for (const auto& folder : folders) {
        for (auto& file : EnumerateYouTubeMediaFiles(folder)) {
            const auto key = file.path.lexically_normal().wstring();
            if (seen.insert(key).second) {
                files.push_back(std::move(file));
            }
        }
    }
    std::sort(files.begin(), files.end(), [](const YouTubeMediaFile& a, const YouTubeMediaFile& b) {
        if (a.modified != b.modified) {
            return a.modified > b.modified;
        }
        return _wcsicmp(a.path.wstring().c_str(), b.path.wstring().c_str()) < 0;
    });
    return files;
}

void SortYouTubeMediaFiles(
    std::vector<YouTubeMediaFile>& files,
    YouTubeMediaSortColumn column,
    bool ascending)
{
    std::stable_sort(
        files.begin(),
        files.end(),
        [column, ascending](const YouTubeMediaFile& left, const YouTubeMediaFile& right) {
            int comparison = 0;
            switch (column) {
            case YouTubeMediaSortColumn::Type: {
                const int leftType = left.type == YouTubeMediaType::Clip ? 1 : 0;
                const int rightType = right.type == YouTubeMediaType::Clip ? 1 : 0;
                comparison = leftType < rightType ? -1 : (leftType > rightType ? 1 : 0);
                break;
            }
            case YouTubeMediaSortColumn::Name: {
                const auto leftName = left.path.filename().wstring();
                const auto rightName = right.path.filename().wstring();
                comparison = _wcsicmp(leftName.c_str(), rightName.c_str());
                break;
            }
            case YouTubeMediaSortColumn::Date:
                comparison = left.modified < right.modified ? -1 : (left.modified > right.modified ? 1 : 0);
                break;
            }
            if (comparison == 0) {
                const auto leftPath = left.path.wstring();
                const auto rightPath = right.path.wstring();
                comparison = _wcsicmp(leftPath.c_str(), rightPath.c_str());
            }
            return ascending ? comparison < 0 : comparison > 0;
        });
}

std::wstring FormatElapsed(std::chrono::seconds elapsed)
{
    const auto total = elapsed.count();
    const int minutes = static_cast<int>(total / 60);
    const int seconds = static_cast<int>(total % 60);

    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%02d:%02d", minutes, seconds);
    return buffer;
}

std::wstring FormatClipTimeMs(int milliseconds)
{
    const int clamped = (std::max)(0, milliseconds);
    const int totalSeconds = clamped / 1000;
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds / 60) % 60;
    const int seconds = totalSeconds % 60;
    wchar_t buffer[24] = {};
    swprintf_s(buffer, L"%02d:%02d:%02d", hours, minutes, seconds);
    return buffer;
}

bool ParseClipTime(const std::wstring& input, int& outSeconds)
{
    outSeconds = 0;
    std::wstring trimmed;
    trimmed.reserve(input.size());
    for (const wchar_t ch : input) {
        if (!iswspace(ch)) {
            trimmed.push_back(ch);
        }
    }
    if (trimmed.empty()) {
        return false;
    }

    std::vector<int> parts;
    size_t index = 0;
    while (index < trimmed.size()) {
        if (trimmed[index] == L':') {
            return false;
        }
        if (!iswdigit(trimmed[index])) {
            return false;
        }
        int value = 0;
        const size_t start = index;
        while (index < trimmed.size() && iswdigit(trimmed[index])) {
            const int digit = trimmed[index] - L'0';
            if (value > (INT_MAX - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
            ++index;
        }
        if (index == start) {
            return false;
        }
        parts.push_back(value);
        if (index == trimmed.size()) {
            break;
        }
        if (trimmed[index] != L':') {
            return false;
        }
        ++index;
        if (index == trimmed.size()) {
            return false; // trailing colon
        }
    }

    if (parts.size() == 2) {
        // mm:ss — minutes may exceed 59 for long recordings.
        if (parts[1] > 59) {
            return false;
        }
        outSeconds = parts[0] * 60 + parts[1];
        return true;
    }
    if (parts.size() == 3) {
        // hh:mm:ss
        if (parts[1] > 59 || parts[2] > 59) {
            return false;
        }
        outSeconds = parts[0] * 3600 + parts[1] * 60 + parts[2];
        return true;
    }
    return false;
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

namespace {

std::wstring TrimWide(const std::wstring& input)
{
    size_t start = 0;
    while (start < input.size() && iswspace(input[start])) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && iswspace(input[end - 1])) {
        --end;
    }
    return input.substr(start, end - start);
}

bool WideContainsInsensitive(const std::wstring& haystack, const std::wstring& needle)
{
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    const auto equalsIgnoreCase = [](wchar_t left, wchar_t right) {
        return std::towlower(left) == std::towlower(right);
    };
    const auto found = std::search(
        haystack.begin(),
        haystack.end(),
        needle.begin(),
        needle.end(),
        equalsIgnoreCase);
    return found != haystack.end();
}

bool ParseKeyLevelNumber(const std::wstring& input, size_t& index, int& value)
{
    if (index < input.size() && input[index] == L'+') {
        ++index;
    }
    if (index >= input.size() || !iswdigit(input[index])) {
        return false;
    }
    long long accumulated = 0;
    while (index < input.size() && iswdigit(input[index])) {
        accumulated = accumulated * 10 + (input[index] - L'0');
        if (accumulated > 999) {
            return false;
        }
        ++index;
    }
    value = static_cast<int>(accumulated);
    return true;
}

} // namespace

RecordingKind ClassifyRecordingKind(const std::string& triggerReason, bool hasMythicMetadata)
{
    if (!triggerReason.empty()) {
        if (_stricmp(triggerReason.c_str(), "mythic-start") == 0) {
            return RecordingKind::MythicPlus;
        }
        if (_stricmp(triggerReason.c_str(), "raid") == 0
            || _stricmp(triggerReason.c_str(), "raid-start") == 0) {
            return RecordingKind::Raid;
        }
        if (_stricmp(triggerReason.c_str(), "pvp") == 0
            || _stricmp(triggerReason.c_str(), "pvp-start") == 0
            || _stricmp(triggerReason.c_str(), "arena") == 0
            || _stricmp(triggerReason.c_str(), "battleground") == 0) {
            return RecordingKind::Pvp;
        }
        if (_stricmp(triggerReason.c_str(), "manual") == 0) {
            return RecordingKind::Manual;
        }
    }
    return hasMythicMetadata ? RecordingKind::MythicPlus : RecordingKind::Manual;
}

bool ParseRecordingKeyLevelFilter(const std::wstring& input, RecordingKeyLevelFilter& outFilter)
{
    outFilter = {};
    std::wstring compact;
    compact.reserve(input.size());
    for (const wchar_t character : input) {
        if (!iswspace(character)) {
            compact.push_back(character);
        }
    }
    if (compact.empty()) {
        return true;
    }

    size_t index = 0;
    int first = 0;
    if (!ParseKeyLevelNumber(compact, index, first)) {
        return false;
    }
    if (index == compact.size()) {
        outFilter.active = true;
        outFilter.minLevel = first;
        outFilter.maxLevel = first;
        return true;
    }
    if (compact[index] != L'-') {
        return false;
    }
    ++index;
    int second = 0;
    if (!ParseKeyLevelNumber(compact, index, second) || index != compact.size()) {
        return false;
    }
    outFilter.active = true;
    outFilter.minLevel = (std::min)(first, second);
    outFilter.maxLevel = (std::max)(first, second);
    return true;
}

std::vector<std::wstring> ParseRecordingCharacterNameFilter(const std::wstring& input)
{
    std::vector<std::wstring> names;
    size_t start = 0;
    while (start <= input.size()) {
        const size_t comma = input.find(L',', start);
        const size_t end = comma == std::wstring::npos ? input.size() : comma;
        const std::wstring name = TrimWide(input.substr(start, end - start));
        if (!name.empty()) {
            names.push_back(name);
        }
        if (comma == std::wstring::npos) {
            break;
        }
        start = comma + 1;
    }
    return names;
}

bool RecordingFilterIsRestricting(const RecordingFilterCriteria& criteria)
{
    return !criteria.includeManual
        || !criteria.includeMythicPlus
        || !criteria.includeRaid
        || !criteria.includePvp
        || criteria.outcome != RecordingOutcomeFilter::Any
        || criteria.keyLevel.active
        || !criteria.characterNames.empty();
}

bool RecordingMatchesFilter(const RecordingFilterItem& item, const RecordingFilterCriteria& criteria)
{
    bool typeAllowed = false;
    switch (item.kind) {
    case RecordingKind::Manual:
        typeAllowed = criteria.includeManual;
        break;
    case RecordingKind::MythicPlus:
        typeAllowed = criteria.includeMythicPlus;
        break;
    case RecordingKind::Raid:
        typeAllowed = criteria.includeRaid;
        break;
    case RecordingKind::Pvp:
        typeAllowed = criteria.includePvp;
        break;
    }
    if (!typeAllowed) {
        return false;
    }

    switch (criteria.outcome) {
    case RecordingOutcomeFilter::Timed:
        if (!item.timed) {
            return false;
        }
        break;
    case RecordingOutcomeFilter::Depleted:
        if (!item.depleted) {
            return false;
        }
        break;
    case RecordingOutcomeFilter::Any:
        break;
    }

    if (criteria.keyLevel.active) {
        if (item.keystoneLevel < criteria.keyLevel.minLevel
            || item.keystoneLevel > criteria.keyLevel.maxLevel) {
            return false;
        }
    }

    for (const auto& needle : criteria.characterNames) {
        bool matchedName = false;
        for (const auto& name : item.participantNames) {
            if (WideContainsInsensitive(name, needle)) {
                matchedName = true;
                break;
            }
        }
        if (!matchedName) {
            return false;
        }
    }
    return true;
}
