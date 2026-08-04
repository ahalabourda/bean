#include "core/GameEnvironment.h"

#include "util/Strings.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace bean::core {
namespace {

using util::ToWide;
using util::Trim;

std::string GetEnvString(const char* name)
{
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr || length == 0) {
        if (value != nullptr) {
            std::free(value);
        }
        return {};
    }
    std::string result(value);
    std::free(value);
    return result;
}

bool DirectoryExists(const std::filesystem::path& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec);
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

// Strips punctuation and case so "Warcraft Recorder", "WarcraftRecorder.exe"
// and "warcraft-recorder" all compare equal.
std::wstring NormalizeProcessOrWindowToken(const std::wstring& value)
{
    std::wstring normalized;
    normalized.reserve(value.size());
    for (const wchar_t ch : value) {
        if (std::iswalnum(ch)) {
            normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }
    }
    return normalized;
}

bool IsWarcraftRecorderToken(const std::wstring& value)
{
    if (value.empty()) {
        return false;
    }
    const std::wstring normalized = NormalizeProcessOrWindowToken(value);
    const auto startsWith = [&normalized](const std::wstring& prefix) {
        return normalized.size() >= prefix.size()
            && normalized.compare(0, prefix.size(), prefix) == 0;
    };
    // The misspelled variant matches a real build that shipped that way.
    return startsWith(L"warcraftrecorder") || startsWith(L"warccraftrecorder");
}

bool IsWarcraftRecorderProcessId(DWORD processId)
{
    if (processId == 0) {
        return false;
    }
    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (processHandle == nullptr) {
        return false;
    }
    wchar_t processPath[MAX_PATH] = {};
    DWORD processPathSize = static_cast<DWORD>(std::size(processPath));
    bool isWarcraftRecorder = false;
    if (QueryFullProcessImageNameW(processHandle, 0, processPath, &processPathSize)) {
        isWarcraftRecorder =
            IsWarcraftRecorderToken(std::filesystem::path(processPath).filename().wstring());
    }
    CloseHandle(processHandle);
    return isWarcraftRecorder;
}

bool DetectWarcraftRecorderByProcessSnapshot()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W processEntry{};
    processEntry.dwSize = sizeof(processEntry);
    if (!Process32FirstW(snapshot, &processEntry)) {
        CloseHandle(snapshot);
        return false;
    }

    do {
        if (IsWarcraftRecorderToken(processEntry.szExeFile)) {
            CloseHandle(snapshot);
            return true;
        }
    } while (Process32NextW(snapshot, &processEntry));

    CloseHandle(snapshot);
    return false;
}

BOOL CALLBACK DetectWarcraftRecorderWindowProc(HWND hwnd, LPARAM lParam)
{
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return TRUE;
    }

    std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), length + 1);
    title.resize(static_cast<std::size_t>(length));
    if (!IsWarcraftRecorderToken(title)) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (!IsWarcraftRecorderProcessId(processId)) {
        return TRUE;
    }

    *reinterpret_cast<bool*>(lParam) = true;
    return FALSE;
}

} // namespace

std::vector<std::filesystem::path> EnumerateDriveRoots()
{
    std::vector<std::filesystem::path> roots;
    const DWORD logicalDrives = GetLogicalDrives();
    if (logicalDrives == 0) {
        roots.emplace_back(R"(C:\)");
        return roots;
    }

    const auto addDriveIfPresent = [&](wchar_t driveLetter) {
        const DWORD bit = 1u << (driveLetter - L'A');
        if ((logicalDrives & bit) != 0) {
            std::wstring root;
            root.push_back(driveLetter);
            root += L":\\";
            roots.emplace_back(std::move(root));
        }
    };

    // C: first because that is overwhelmingly where installs live.
    for (wchar_t driveLetter = L'C'; driveLetter <= L'Z'; ++driveLetter) {
        addDriveIfPresent(driveLetter);
    }
    for (wchar_t driveLetter = L'A'; driveLetter < L'C'; ++driveLetter) {
        addDriveIfPresent(driveLetter);
    }

    if (roots.empty()) {
        roots.emplace_back(R"(C:\)");
    }
    return roots;
}

bool ResolveObsInstallRoot(std::filesystem::path& root)
{
    const auto envRootText = Trim(GetEnvString("BEAN_OBS_ROOT"));
    if (!envRootText.empty()) {
        const std::filesystem::path envRoot = ToWide(envRootText);
        if (std::filesystem::exists(envRoot / "bin" / "64bit" / "obs.dll")) {
            root = envRoot;
            return true;
        }
    }

    for (const auto& driveRoot : EnumerateDriveRoots()) {
        const std::filesystem::path candidates[] = {
            driveRoot / "Program Files" / "obs-studio",
            driveRoot / "Program Files (x86)" / "obs-studio"
        };
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate / "bin" / "64bit" / "obs.dll")) {
                root = candidate;
                return true;
            }
        }
    }
    return false;
}

bool IsUsableObsInstallPresent()
{
    std::filesystem::path root;
    if (!ResolveObsInstallRoot(root)) {
        return false;
    }

    const auto bin64 = root / "bin" / "64bit";
    if (!std::filesystem::exists(bin64 / "obs.dll")) {
        return false;
    }
    if (!std::filesystem::exists(bin64 / "obs-ffmpeg-mux.exe")) {
        return false;
    }
    if (!std::filesystem::exists(root / "obs-plugins" / "64bit")) {
        return false;
    }
    if (!std::filesystem::exists(root / "data" / "libobs")) {
        return false;
    }
    return std::filesystem::exists(bin64 / "libobs-d3d11.dll")
        || std::filesystem::exists(bin64 / "libobs-opengl.dll");
}

const char* WowEditionLabel(WowEdition edition)
{
    switch (edition) {
    case WowEdition::Retail:
        return "Retail";
    case WowEdition::Ptr:
        return "PTR";
    case WowEdition::Unknown:
    default:
        return "Unknown";
    }
}

std::filesystem::path ResolveDefaultWowInstallDirectory()
{
    for (const auto& driveRoot : EnumerateDriveRoots()) {
        const std::filesystem::path candidates[] = {
            driveRoot / "Program Files (x86)" / "World of Warcraft",
            driveRoot / "Program Files" / "World of Warcraft"
        };
        for (const auto& candidate : candidates) {
            if (DirectoryExists(candidate)) {
                return candidate;
            }
        }
    }

    return R"(C:\Program Files (x86)\World of Warcraft)";
}

std::filesystem::path ResolveWowLogDirectoryFromInstallDirectory(
    const std::filesystem::path& installDirectory,
    WowEdition edition)
{
    if (installDirectory.empty()) {
        return {};
    }
    const wchar_t* flavorDirectory = edition == WowEdition::Ptr ? L"_ptr_" : L"_retail_";
    return installDirectory / flavorDirectory / "Logs";
}

std::filesystem::path ResolveDefaultWowLogDirectory()
{
    return ResolveWowLogDirectoryFromInstallDirectory(
        ResolveDefaultWowInstallDirectory(),
        WowEdition::Retail);
}

std::optional<std::filesystem::path> ResolveWowInstallDirectoryFromLogDirectory(
    const std::filesystem::path& logDirectory)
{
    if (logDirectory.empty()) {
        return std::nullopt;
    }
    std::filesystem::path normalized = logDirectory.lexically_normal();
    if (normalized.filename() == L"Logs") {
        normalized = normalized.parent_path();
    }
    if (normalized.filename() == L"_retail_" || normalized.filename() == L"_ptr_") {
        auto installDirectory = normalized.parent_path();
        if (!installDirectory.empty()) {
            return installDirectory;
        }
    }
    return std::nullopt;
}

bool IsAdvancedCombatLoggingEnabled(
    const std::filesystem::path& installDirectory,
    WowEdition edition)
{
    auto resolvedInstallDirectory = installDirectory;
    if (resolvedInstallDirectory.empty()) {
        resolvedInstallDirectory = ResolveDefaultWowInstallDirectory();
    }
    if (resolvedInstallDirectory.empty()) {
        return false;
    }

    const auto configPath =
        resolvedInstallDirectory / (edition == WowEdition::Ptr ? L"_ptr_" : L"_retail_") / "WTF" / "Config.wtf";
    std::ifstream stream(configPath);
    if (!stream.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty()) {
            continue;
        }

        std::istringstream parser(trimmed);
        std::string setToken;
        std::string keyToken;
        std::string valueToken;
        if (!(parser >> setToken >> keyToken >> valueToken)) {
            continue;
        }
        if (ToLowerAscii(setToken) != "set" || ToLowerAscii(keyToken) != "advancedcombatlogging") {
            continue;
        }
        return valueToken == "\"1\"";
    }
    return false;
}

bool IsWarcraftRecorderRunning()
{
    if (DetectWarcraftRecorderByProcessSnapshot()) {
        return true;
    }
    bool found = false;
    EnumWindows(DetectWarcraftRecorderWindowProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

} // namespace bean::core
