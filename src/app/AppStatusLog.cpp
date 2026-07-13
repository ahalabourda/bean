#include "app/AppStatusLog.h"

#include <algorithm>
#include <iomanip>
#include <regex>
#include <sstream>
#include <vector>

namespace {

std::wstring ToWide(const std::string& input)
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

std::string ToUtf8(const std::wstring& input)
{
    if (input.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, output.data(), size, nullptr, nullptr);
    if (!output.empty() && output.back() == '\0') {
        output.pop_back();
    }
    return output;
}

std::wstring GetWindowTextString(HWND hwnd)
{
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

std::wstring VersionText()
{
    constexpr wchar_t kAppVersion[] = L"0.1";
    return std::wstring(L"v") + kAppVersion;
}

std::wstring AudioCaptureScopeLabel(bean::core::AppSettings::AudioCaptureScope scope)
{
    switch (scope) {
    case bean::core::AppSettings::AudioCaptureScope::WowAndDiscord:
        return L"wow+discord";
    case bean::core::AppSettings::AudioCaptureScope::AllDesktop:
        return L"all-desktop";
    case bean::core::AppSettings::AudioCaptureScope::WowOnly:
    default:
        return L"wow-only";
    }
}

std::wstring ChatBlockerAnchorLabel(bean::core::AppSettings::ChatBlockerAnchor anchor)
{
    switch (anchor) {
    case bean::core::AppSettings::ChatBlockerAnchor::BottomRight:
        return L"bottom-right";
    case bean::core::AppSettings::ChatBlockerAnchor::TopLeft:
        return L"top-left";
    case bean::core::AppSettings::ChatBlockerAnchor::TopRight:
        return L"top-right";
    case bean::core::AppSettings::ChatBlockerAnchor::BottomLeft:
    default:
        return L"bottom-left";
    }
}

std::wstring TimestampNowForStatus()
{
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &tt);

    std::wostringstream os;
    os << std::put_time(&tm, L"%Y-%m-%d %H:%M:%S");
    return os.str();
}

std::wstring TimestampNowForStatusLogFile()
{
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &tt);

    std::wostringstream os;
    os << std::put_time(&tm, L"%Y-%m-%d-%H-%M-%S");
    return os.str();
}

std::wstring SanitizeStatusTextForPrivacy(const std::wstring& text)
{
    if (text.empty()) {
        return text;
    }

    // Redact the username in common Windows profile paths:
    // C:\Users\<name>\... and C:/Users/<name>/...
    static const std::wregex kWindowsUserPathBackslash(
        LR"(([A-Za-z]:\\Users\\)([^\\\/\r\n]+))",
        std::regex_constants::icase);
    static const std::wregex kWindowsUserPathForwardSlash(
        LR"(([A-Za-z]:/Users/)([^\\\/\r\n]+))",
        std::regex_constants::icase);

    std::wstring sanitized = std::regex_replace(text, kWindowsUserPathBackslash, L"$1[redacted]");
    sanitized = std::regex_replace(sanitized, kWindowsUserPathForwardSlash, L"$1[redacted]");
    return sanitized;
}

bool IsStatusLogFileName(const std::wstring& name)
{
    const std::wstring prefix = kStatusLogFilePrefix;
    const std::wstring suffix = kStatusLogFileExtension;
    if (name.size() <= (prefix.size() + suffix.size())) {
        return false;
    }
    if (name.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    return true;
}

void PruneOldStatusLogFiles(const std::filesystem::path& directory)
{
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            return;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto filename = entry.path().filename().wstring();
        if (IsStatusLogFileName(filename)) {
            candidates.push_back(entry.path());
        }
    }

    if (candidates.size() <= kStatusLogRetentionCount) {
        return;
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.filename().wstring() > right.filename().wstring();
    });
    for (size_t i = kStatusLogRetentionCount; i < candidates.size(); ++i) {
        std::filesystem::remove(candidates[i], ec);
    }
}

void AppendStatusLineToSessionLog(AppContext* ctx, const std::wstring& line)
{
    if (!ctx || !ctx->statusLogStream.is_open() || line.empty()) {
        return;
    }
    ctx->statusLogStream << ToUtf8(line) << "\n";
    ctx->statusLogStream.flush();
    if (!ctx->statusLogWriteFailed && !ctx->statusLogStream.good()) {
        ctx->statusLogWriteFailed = true;
    }
}

} // namespace

std::filesystem::path ResolveStatusLogDirectory(const AppContext* ctx)
{
    if (!ctx) {
        return {};
    }
    if (ctx->runRepository) {
        const auto dbPath = ctx->runRepository->GetDatabasePath();
        if (!dbPath.empty()) {
            return dbPath.parent_path();
        }
    }
    return ctx->settingsStore.GetConfigPath().parent_path();
}

std::wstring InitializeStatusLogFile(AppContext* ctx)
{
    if (!ctx) {
        return L"App context missing.";
    }

    const auto logDirectory = ResolveStatusLogDirectory(ctx);
    if (logDirectory.empty()) {
        return L"Status log directory path is unavailable.";
    }

    std::error_code ec;
    std::filesystem::create_directories(logDirectory, ec);
    if (ec) {
        return std::wstring(L"Failed to create status log directory: ") + ToWide(ec.message());
    }

    ctx->statusLogPath = logDirectory / (std::wstring(kStatusLogFilePrefix) + TimestampNowForStatusLogFile() + std::wstring(kStatusLogFileExtension));
    ctx->statusLogStream.open(ctx->statusLogPath, std::ios::out | std::ios::trunc);
    if (!ctx->statusLogStream.is_open()) {
        return std::wstring(L"Could not open status log file: ") + ctx->statusLogPath.wstring();
    }
    ctx->statusLogWriteFailed = false;
    PruneOldStatusLogFiles(logDirectory);
    return {};
}

void LogSessionDiagnostics(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    SetStatus(ctx, std::wstring(L"Session started: ") + VersionText());
    SetStatus(ctx, std::wstring(L"Config file: ") + ctx->settingsStore.GetConfigPath().wstring());
    if (ctx->runRepository) {
        SetStatus(ctx, std::wstring(L"Run metadata DB: ") + ctx->runRepository->GetDatabasePath().wstring());
    }
    if (!ctx->statusLogPath.empty()) {
        SetStatus(ctx, std::wstring(L"Status session log: ") + ctx->statusLogPath.wstring());
    }

    std::wostringstream capture;
    capture
        << L"Capture settings: encoder=" << ToWide(ctx->settings.videoEncoder)
        << L", quality=" << ToWide(ctx->settings.encoderPreset)
        << L", container=" << ToWide(ctx->settings.videoContainer)
        << L", resolution=" << ctx->settings.width << L"x" << ctx->settings.height
        << L"@" << ctx->settings.fps << L"fps"
        << L", post-run tail=" << ctx->settings.postRunStopDelaySeconds << L"s"
        << L", audio-scope=" << AudioCaptureScopeLabel(ctx->settings.audioCaptureScope)
        << L", microphone=" << (ctx->settings.captureMicrophone ? L"yes" : L"no")
        << L", mic-noise-suppression=" << (ctx->settings.microphoneNoiseSuppression ? L"yes" : L"no");
    SetStatus(ctx, capture.str());

    SetStatus(ctx, std::wstring(L"Paths: output=") + ctx->settings.outputDirectory.wstring() + L", wow-log=" + ctx->settings.wowLogDirectory.wstring());

    std::wostringstream chatBlocker;
    chatBlocker
        << L"Chat blocker: enabled=" << (ctx->settings.chatBlockerEnabled ? L"yes" : L"no")
        << L", image=" << (ctx->settings.chatBlockerUseCustomImage ? L"custom" : L"blank")
        << L", size=" << ctx->settings.chatBlockerWidth << L"x" << ctx->settings.chatBlockerHeight
        << L", anchor=" << ChatBlockerAnchorLabel(ctx->settings.chatBlockerAnchor);
    if (ctx->settings.chatBlockerUseCustomImage && !ctx->settings.chatBlockerCustomImagePath.empty()) {
        chatBlocker << L", file=" << ctx->settings.chatBlockerCustomImagePath.wstring();
    }
    SetStatus(ctx, chatBlocker.str());

    const bool youtubeLinked = !ctx->settings.youtubeRefreshToken.empty();
    SetStatus(
        ctx,
        std::wstring(L"YouTube setup: oauth-configured=")
            + (ctx->youtubeOAuthConfigured ? L"yes" : L"no")
            + L", linked="
            + (youtubeLinked ? L"yes" : L"no"));
}

void SetStatus(AppContext* ctx, const std::wstring& text)
{
    if (!ctx || !ctx->statusText || text.empty()) {
        return;
    }

    const auto existing = GetWindowTextString(ctx->statusText);
    const std::wstring line = L"[" + TimestampNowForStatus() + L"] " + SanitizeStatusTextForPrivacy(text);
    AppendStatusLineToSessionLog(ctx, line);
    std::wstring combined;
    if (existing.empty()) {
        combined = line;
    } else {
        combined = existing + L"\r\n" + line;
    }

    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start < combined.size()) {
        size_t end = combined.find(L'\n', start);
        if (end == std::wstring::npos) {
            end = combined.size();
        }
        std::wstring entry = combined.substr(start, end - start);
        if (!entry.empty() && entry.back() == L'\r') {
            entry.pop_back();
        }
        lines.push_back(std::move(entry));
        start = end + 1;
    }

    if (lines.size() > kStatusMaxLines) {
        const size_t trimCount = lines.size() - kStatusMaxLines;
        lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(trimCount));
    }

    std::wostringstream rebuilt;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            rebuilt << L"\r\n";
        }
        rebuilt << lines[i];
    }
    combined = rebuilt.str();

    SetWindowTextW(ctx->statusText, combined.c_str());
    SendMessageW(ctx->statusText, EM_SETSEL, static_cast<WPARAM>(combined.size()), static_cast<LPARAM>(combined.size()));
    SendMessageW(ctx->statusText, EM_SCROLLCARET, 0, 0);
}

void PostStatus(AppContext* ctx, const std::wstring& text)
{
    if (!ctx || !ctx->mainWindow || text.empty()) {
        return;
    }
    auto* payload = new std::wstring(text);
    PostMessageW(ctx->mainWindow, WM_BEAN_STATUS, 0, reinterpret_cast<LPARAM>(payload));
}
