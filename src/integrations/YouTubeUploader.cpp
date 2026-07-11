#include "integrations/YouTubeUploader.h"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace bean::integrations {
namespace {

constexpr const char* kTokenEndpoint = "https://oauth2.googleapis.com/token";
constexpr const char* kUploadInitEndpoint = "https://www.googleapis.com/upload/youtube/v3/videos?uploadType=resumable&part=snippet,status";
constexpr const char* kChannelIdentityEndpoint = "https://www.googleapis.com/youtube/v3/channels?part=snippet&mine=true";
constexpr DWORD kDefaultHttpTimeoutMs = 30000;

struct HttpResponse {
    DWORD statusCode = 0;
    std::string body;
    std::map<std::string, std::string> headersLower;
    std::string error;
};

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

std::string Trim(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string NormalizeBaseUrl(std::string value)
{
    while (value.size() > 8 && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string EscapeJson(std::string value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::string UnescapeJson(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    bool escape = false;
    for (char ch : value) {
        if (escape) {
            switch (ch) {
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case '\\':
            case '"':
            case '/':
                out.push_back(ch);
                break;
            default:
                out.push_back(ch);
                break;
            }
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        out.push_back(ch);
    }
    if (escape) {
        out.push_back('\\');
    }
    return out;
}

std::string ReadJsonString(const std::string& content, const std::string& key)
{
    const std::string marker = "\"" + key + "\"";
    const auto keyPos = content.find(marker);
    if (keyPos == std::string::npos) {
        return {};
    }
    const auto colonPos = content.find(':', keyPos + marker.size());
    if (colonPos == std::string::npos) {
        return {};
    }
    const auto quotePos = content.find('"', colonPos + 1);
    if (quotePos == std::string::npos) {
        return {};
    }

    bool escape = false;
    for (size_t i = quotePos + 1; i < content.size(); ++i) {
        const char ch = content[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            return UnescapeJson(content.substr(quotePos + 1, i - quotePos - 1));
        }
    }
    return {};
}

std::string UrlEncode(const std::string& input)
{
    std::ostringstream out;
    out.fill('0');
    out << std::hex << std::uppercase;
    for (unsigned char c : input) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
            continue;
        }
        out << '%' << std::setw(2) << static_cast<int>(c);
    }
    return out.str();
}

std::optional<std::vector<uint8_t>> ComputeSha256(const std::string& data)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD hashObjectSize = 0;
    DWORD dataSize = 0;
    DWORD hashSize = 0;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return std::nullopt;
    }
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjectSize), sizeof(hashObjectSize), &dataSize, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    if (BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &dataSize, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }

    std::vector<uint8_t> hashObject(hashObjectSize);
    std::vector<uint8_t> hashValue(hashSize);
    if (BCryptCreateHash(algorithm, &hash, hashObject.data(), hashObjectSize, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    if (BCryptFinishHash(hash, hashValue.data(), static_cast<ULONG>(hashValue.size()), 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return hashValue;
}

std::string Base64UrlNoPadding(const uint8_t* data, DWORD size)
{
    DWORD needed = 0;
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &needed)) {
        return {};
    }
    std::string out(needed, '\0');
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &needed)) {
        return {};
    }
    if (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    for (char& c : out) {
        if (c == '+') {
            c = '-';
        } else if (c == '/') {
            c = '_';
        }
    }
    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }
    return out;
}

std::optional<std::array<uint8_t, 32>> RandomBytes32()
{
    std::array<uint8_t, 32> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return std::nullopt;
    }
    return bytes;
}

std::string GenerateCodeVerifier()
{
    const auto bytes = RandomBytes32();
    if (!bytes.has_value()) {
        return {};
    }
    return Base64UrlNoPadding(bytes->data(), static_cast<DWORD>(bytes->size()));
}

std::string BuildCodeChallenge(const std::string& verifier)
{
    const auto hash = ComputeSha256(verifier);
    if (!hash.has_value()) {
        return {};
    }
    return Base64UrlNoPadding(hash->data(), static_cast<DWORD>(hash->size()));
}

std::string GenerateStateToken()
{
    const auto bytes = RandomBytes32();
    if (!bytes.has_value()) {
        return {};
    }
    return Base64UrlNoPadding(bytes->data(), static_cast<DWORD>(bytes->size()));
}

bool CrackUrl(const std::wstring& url, URL_COMPONENTSW& components, std::wstring& host, std::wstring& pathAndQuery, bool& isHttps, INTERNET_PORT& port, std::string& error)
{
    std::vector<wchar_t> hostBuffer(512);
    std::vector<wchar_t> pathBuffer(4096);
    ZeroMemory(&components, sizeof(components));
    components.dwStructSize = sizeof(components);
    components.lpszHostName = hostBuffer.data();
    components.dwHostNameLength = static_cast<DWORD>(hostBuffer.size());
    components.lpszUrlPath = pathBuffer.data();
    components.dwUrlPathLength = static_cast<DWORD>(pathBuffer.size());

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
        error = "Failed to parse URL for HTTP request.";
        return false;
    }
    host.assign(components.lpszHostName, components.dwHostNameLength);
    pathAndQuery.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0 && components.lpszExtraInfo) {
        pathAndQuery.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    isHttps = (components.nScheme == INTERNET_SCHEME_HTTPS);
    port = components.nPort;
    return true;
}

void ParseHeaders(const std::string& headerBlob, std::map<std::string, std::string>& headers)
{
    std::istringstream stream(headerBlob);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto sep = line.find(':');
        if (sep == std::string::npos) {
            continue;
        }
        const std::string name = ToLower(Trim(line.substr(0, sep)));
        const std::string value = Trim(line.substr(sep + 1));
        if (!name.empty()) {
            headers[name] = value;
        }
    }
}

HttpResponse SendRequest(const std::wstring& method, const std::string& urlUtf8, const std::vector<std::wstring>& headers, const std::string& body)
{
    HttpResponse response;
    const std::wstring url = ToWide(urlUtf8);
    if (url.empty()) {
        response.error = "Invalid request URL.";
        return response;
    }

    URL_COMPONENTSW parts{};
    std::wstring host;
    std::wstring pathAndQuery;
    bool isHttps = true;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::string crackError;
    if (!CrackUrl(url, parts, host, pathAndQuery, isHttps, port, crackError)) {
        response.error = crackError;
        return response;
    }

    HINTERNET session = WinHttpOpen(L"Bean/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        response.error = "WinHTTP initialization failed.";
        return response;
    }
    WinHttpSetTimeouts(session, kDefaultHttpTimeoutMs, kDefaultHttpTimeoutMs, kDefaultHttpTimeoutMs, kDefaultHttpTimeoutMs);

    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connection) {
        response.error = "Failed to connect to remote host.";
        WinHttpCloseHandle(session);
        return response;
    }

    const DWORD requestFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, method.c_str(), pathAndQuery.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags);
    if (!request) {
        response.error = "Failed to create HTTP request.";
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    std::wstring headersBlob;
    for (const auto& header : headers) {
        headersBlob += header;
        headersBlob += L"\r\n";
    }

    if (!WinHttpSendRequest(
            request,
            headersBlob.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headersBlob.c_str(),
            headersBlob.empty() ? 0 : static_cast<DWORD>(headersBlob.size()),
            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()),
            0)) {
        response.error = "Failed to send HTTP request.";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        response.error = "Failed to receive HTTP response.";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.statusCode = statusCode;

    DWORD rawHeaderSize = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &rawHeaderSize, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && rawHeaderSize > sizeof(wchar_t)) {
        std::wstring headerWide(static_cast<size_t>(rawHeaderSize / sizeof(wchar_t)), L'\0');
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, headerWide.data(), &rawHeaderSize, WINHTTP_NO_HEADER_INDEX)) {
            ParseHeaders(ToUtf8(headerWide), response.headersLower);
        }
    }

    std::string responseBody;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            response.error = "Failed while reading HTTP response.";
            break;
        }
        if (available == 0) {
            break;
        }
        std::string chunk(static_cast<size_t>(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            response.error = "Failed while receiving HTTP response body.";
            break;
        }
        chunk.resize(static_cast<size_t>(read));
        responseBody += chunk;
    }
    response.body = std::move(responseBody);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}

HttpResponse UploadFileToResumableSession(
    const std::string& sessionUrlUtf8,
    const std::filesystem::path& videoPath,
    const YouTubeUploader::UploadProgressCallback& progressCallback)
{
    HttpResponse response;
    std::error_code fileSizeError;
    const auto fileSize = std::filesystem::file_size(videoPath, fileSizeError);
    if (fileSizeError) {
        response.error = "Unable to determine video file size.";
        return response;
    }
    if (fileSize > static_cast<uintmax_t>((std::numeric_limits<DWORD>::max)())) {
        response.error = "Video file is too large for this uploader implementation (>4GB).";
        return response;
    }

    std::ifstream stream(videoPath, std::ios::binary);
    if (!stream.is_open()) {
        response.error = "Unable to open video file for upload.";
        return response;
    }

    const std::wstring url = ToWide(sessionUrlUtf8);
    if (url.empty()) {
        response.error = "Invalid resumable upload URL.";
        return response;
    }

    URL_COMPONENTSW parts{};
    std::wstring host;
    std::wstring pathAndQuery;
    bool isHttps = true;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::string crackError;
    if (!CrackUrl(url, parts, host, pathAndQuery, isHttps, port, crackError)) {
        response.error = crackError;
        return response;
    }

    HINTERNET session = WinHttpOpen(L"Bean/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        response.error = "WinHTTP initialization failed.";
        return response;
    }
    WinHttpSetTimeouts(session, 60000, 60000, 60000, 60000);

    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connection) {
        response.error = "Failed to connect to upload host.";
        WinHttpCloseHandle(session);
        return response;
    }

    const DWORD requestFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"PUT", pathAndQuery.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags);
    if (!request) {
        response.error = "Failed to start upload request.";
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    const std::wstring headers = L"Content-Type: application/octet-stream\r\n";
    const DWORD totalLength = static_cast<DWORD>(fileSize);
    if (!WinHttpSendRequest(
            request,
            headers.c_str(),
            static_cast<DWORD>(headers.size()),
            WINHTTP_NO_REQUEST_DATA,
            0,
            totalLength,
            0)) {
        response.error = "Failed to begin upload request.";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    uint64_t bytesSent = 0;
    const uint64_t totalBytes = static_cast<uint64_t>(fileSize);
    if (progressCallback) {
        progressCallback(0, totalBytes, "uploading");
    }

    std::array<char, 1024 * 256> buffer{};
    while (stream.good()) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = stream.gcount();
        if (got <= 0) {
            break;
        }
        DWORD written = 0;
        if (!WinHttpWriteData(request, buffer.data(), static_cast<DWORD>(got), &written) || written != static_cast<DWORD>(got)) {
            response.error = "Failed while streaming video bytes to YouTube.";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return response;
        }
        bytesSent += static_cast<uint64_t>(written);
        if (progressCallback) {
            progressCallback(bytesSent, totalBytes, "uploading");
        }
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        response.error = "Upload request completed without a valid response.";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.statusCode = statusCode;

    std::string body;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            response.error = "Failed while reading upload response.";
            break;
        }
        if (available == 0) {
            break;
        }
        std::string chunk(static_cast<size_t>(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            response.error = "Failed while receiving upload response.";
            break;
        }
        chunk.resize(static_cast<size_t>(read));
        body += chunk;
    }
    response.body = std::move(body);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}

std::string BuildTokenRequestBody(const std::map<std::string, std::string>& values)
{
    std::ostringstream body;
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) {
            body << "&";
        }
        first = false;
        body << UrlEncode(key) << "=" << UrlEncode(value);
    }
    return body.str();
}

std::optional<std::string> ExchangeRefreshTokenForAccessToken(const YouTubeCredentials& credentials, std::string& error)
{
    error.clear();
    if (credentials.clientId.empty() || credentials.refreshToken.empty()) {
        error = "Missing YouTube client ID or refresh token.";
        return std::nullopt;
    }

    std::map<std::string, std::string> form;
    form["client_id"] = credentials.clientId;
    form["refresh_token"] = credentials.refreshToken;
    form["grant_type"] = "refresh_token";

    const auto response = SendRequest(
        L"POST",
        kTokenEndpoint,
        {L"Content-Type: application/x-www-form-urlencoded"},
        BuildTokenRequestBody(form));
    if (!response.error.empty()) {
        error = response.error;
        return std::nullopt;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
        std::string details = ReadJsonString(response.body, "error_description");
        if (details.empty()) {
            details = ReadJsonString(response.body, "error");
        }
        if (details.empty()) {
            details = "HTTP " + std::to_string(response.statusCode);
        }
        error = "Failed to refresh YouTube access token: " + details;
        return std::nullopt;
    }

    const std::string accessToken = ReadJsonString(response.body, "access_token");
    if (accessToken.empty()) {
        error = "Token response did not include an access token.";
        return std::nullopt;
    }
    return accessToken;
}

std::string PrivacyToString(YouTubePrivacy privacy)
{
    switch (privacy) {
    case YouTubePrivacy::Public:
        return "public";
    case YouTubePrivacy::Unlisted:
        return "unlisted";
    case YouTubePrivacy::Private:
    default:
        return "private";
    }
}

} // namespace

YouTubeAuthResult YouTubeUploader::AuthorizeDesktop(HWND owner, const std::string& authServerUrl)
{
    YouTubeAuthResult result;
    const std::string normalizedAuthServerUrl = NormalizeBaseUrl(authServerUrl);
    if (normalizedAuthServerUrl.empty()) {
        result.error = "YouTube auth server URL is not configured.";
        return result;
    }
    if (normalizedAuthServerUrl.rfind("https://", 0) != 0) {
        result.error = "YouTube auth server must use HTTPS.";
        return result;
    }

    const std::string sessionId = GenerateStateToken();
    const std::string pollToken = GenerateStateToken();
    if (sessionId.empty() || pollToken.empty()) {
        result.error = "Failed to initialize secure YouTube authorization session.";
        return result;
    }

    const auto startResponse = SendRequest(
        L"POST",
        normalizedAuthServerUrl + "/start",
        {L"Content-Type: application/x-www-form-urlencoded"},
        BuildTokenRequestBody({
            {"session_id", sessionId},
            {"poll_token", pollToken},
        }));
    if (!startResponse.error.empty()) {
        result.error = "Could not contact YouTube auth server: " + startResponse.error;
        return result;
    }
    if (startResponse.statusCode < 200 || startResponse.statusCode >= 300) {
        const std::string details = ReadJsonString(startResponse.body, "error");
        result.error = "YouTube auth server rejected the request: "
            + (details.empty() ? "HTTP " + std::to_string(startResponse.statusCode) : details);
        return result;
    }

    const std::string authorizationUrl = ReadJsonString(startResponse.body, "authorization_url");
    if (authorizationUrl.empty() || authorizationUrl.rfind("https://", 0) != 0) {
        result.error = "YouTube auth server returned an invalid authorization URL.";
        return result;
    }

    const std::wstring authUrlWide = ToWide(authorizationUrl);
    const auto shellResult = reinterpret_cast<intptr_t>(ShellExecuteW(owner, L"open", authUrlWide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (shellResult <= 32) {
        result.error = "Failed to open browser for YouTube authorization.";
        return result;
    }

    constexpr int kPollAttempts = 300;
    for (int attempt = 0; attempt < kPollAttempts; ++attempt) {
        const auto pollResponse = SendRequest(
            L"GET",
            normalizedAuthServerUrl + "/poll?session_id=" + UrlEncode(sessionId) + "&poll_token=" + UrlEncode(pollToken),
            {},
            {});
        if (!pollResponse.error.empty()) {
            result.error = "Could not poll YouTube auth server: " + pollResponse.error;
            return result;
        }
        if (pollResponse.statusCode < 200 || pollResponse.statusCode >= 300) {
            const std::string details = ReadJsonString(pollResponse.body, "error");
            result.error = "YouTube auth server poll failed: "
                + (details.empty() ? "HTTP " + std::to_string(pollResponse.statusCode) : details);
            return result;
        }

        const std::string status = ReadJsonString(pollResponse.body, "status");
        if (status == "pending") {
            Sleep(1000);
            continue;
        }
        if (status == "error") {
            result.error = ReadJsonString(pollResponse.body, "error");
            if (result.error.empty()) {
                result.error = "YouTube authorization failed on the auth server.";
            }
            return result;
        }
        if (status != "complete") {
            result.error = "YouTube auth server returned an unknown session status.";
            return result;
        }

        result.clientId = ReadJsonString(pollResponse.body, "client_id");
        result.refreshToken = ReadJsonString(pollResponse.body, "refresh_token");
        result.channelId = ReadJsonString(pollResponse.body, "channel_id");
        result.channelTitle = ReadJsonString(pollResponse.body, "channel_title");
        if (result.clientId.empty() || result.refreshToken.empty()) {
            result.error = "YouTube auth server returned incomplete credentials.";
            return result;
        }
        result.success = true;
        return result;
    }

    result.error = "Timed out waiting for YouTube authorization.";
    return result;
}

YouTubeUploadResult YouTubeUploader::UploadVideo(
    const YouTubeCredentials& credentials,
    const YouTubeUploadRequest& request,
    const UploadProgressCallback& progressCallback)
{
    YouTubeUploadResult result;
    if (credentials.clientId.empty()) {
        result.error = "YouTube client ID is required.";
        return result;
    }
    if (credentials.refreshToken.empty()) {
        result.error = "YouTube account is not linked yet.";
        return result;
    }
    if (request.videoPath.empty() || !std::filesystem::exists(request.videoPath)) {
        result.error = "Video file does not exist.";
        return result;
    }
    if (request.title.empty()) {
        result.error = "Video title is required.";
        return result;
    }

    const uint64_t totalBytes = static_cast<uint64_t>(std::filesystem::file_size(request.videoPath));
    if (progressCallback) {
        progressCallback(0, totalBytes, "auth");
    }

    std::string tokenError;
    const auto accessToken = ExchangeRefreshTokenForAccessToken(credentials, tokenError);
    if (!accessToken.has_value()) {
        result.error = tokenError;
        return result;
    }

    std::ostringstream metadata;
    metadata << "{"
             << "\"snippet\":{"
             << "\"title\":\"" << EscapeJson(request.title) << "\","
             << "\"categoryId\":\"20\""
             << "},"
             << "\"status\":{"
             << "\"privacyStatus\":\"" << EscapeJson(PrivacyToString(request.privacy)) << "\""
             << "}"
             << "}";

    std::vector<std::wstring> initHeaders;
    initHeaders.emplace_back(L"Authorization: Bearer " + ToWide(*accessToken));
    initHeaders.emplace_back(L"Content-Type: application/json; charset=UTF-8");
    initHeaders.emplace_back(L"X-Upload-Content-Type: application/octet-stream");
    const uintmax_t fileSize = std::filesystem::file_size(request.videoPath);
    initHeaders.emplace_back(L"X-Upload-Content-Length: " + std::to_wstring(fileSize));

    if (progressCallback) {
        progressCallback(0, totalBytes, "session");
    }
    const auto initResponse = SendRequest(L"POST", kUploadInitEndpoint, initHeaders, metadata.str());
    if (!initResponse.error.empty()) {
        result.error = initResponse.error;
        return result;
    }
    if (initResponse.statusCode < 200 || initResponse.statusCode >= 300) {
        std::string details = ReadJsonString(initResponse.body, "error_description");
        if (details.empty()) {
            details = ReadJsonString(initResponse.body, "message");
        }
        if (details.empty()) {
            details = "HTTP " + std::to_string(initResponse.statusCode);
        }
        result.error = "Failed to initialize YouTube upload session: " + details;
        return result;
    }

    const auto locationIt = initResponse.headersLower.find("location");
    if (locationIt == initResponse.headersLower.end() || locationIt->second.empty()) {
        result.error = "YouTube upload session URL was missing from response.";
        return result;
    }

    const auto uploadResponse = UploadFileToResumableSession(locationIt->second, request.videoPath, progressCallback);
    if (!uploadResponse.error.empty()) {
        result.error = uploadResponse.error;
        return result;
    }
    if (uploadResponse.statusCode < 200 || uploadResponse.statusCode >= 300) {
        std::string details = ReadJsonString(uploadResponse.body, "message");
        if (details.empty()) {
            details = ReadJsonString(uploadResponse.body, "error");
        }
        if (details.empty()) {
            details = "HTTP " + std::to_string(uploadResponse.statusCode);
        }
        result.error = "YouTube upload failed: " + details;
        return result;
    }

    result.videoId = ReadJsonString(uploadResponse.body, "id");
    if (!result.videoId.empty()) {
        result.videoUrl = "https://www.youtube.com/watch?v=" + result.videoId;
    }
    if (progressCallback) {
        progressCallback(totalBytes, totalBytes, "complete");
    }
    result.success = true;
    return result;
}

YouTubeChannelIdentityResult YouTubeUploader::GetLinkedChannelIdentity(const YouTubeCredentials& credentials)
{
    YouTubeChannelIdentityResult result;
    std::string tokenError;
    const auto accessToken = ExchangeRefreshTokenForAccessToken(credentials, tokenError);
    if (!accessToken.has_value()) {
        result.error = tokenError;
        return result;
    }

    std::vector<std::wstring> headers;
    headers.emplace_back(L"Authorization: Bearer " + ToWide(*accessToken));
    const auto response = SendRequest(L"GET", kChannelIdentityEndpoint, headers, "");
    if (!response.error.empty()) {
        result.error = response.error;
        return result;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
        std::string details = ReadJsonString(response.body, "message");
        if (details.empty()) {
            details = "HTTP " + std::to_string(response.statusCode);
        }
        result.error = "Failed to read linked YouTube channel: " + details;
        return result;
    }

    result.channelId = ReadJsonString(response.body, "id");
    result.channelTitle = ReadJsonString(response.body, "title");
    if (result.channelId.empty() && result.channelTitle.empty()) {
        result.error = "Could not parse linked channel details.";
        return result;
    }
    result.success = true;
    return result;
}

} // namespace bean::integrations
