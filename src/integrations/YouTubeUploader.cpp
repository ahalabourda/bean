#include "integrations/YouTubeUploader.h"

#include "util/Json.h"
#include "util/Strings.h"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
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

constexpr const char* kUploadInitEndpoint = "https://www.googleapis.com/upload/youtube/v3/videos?uploadType=resumable&part=snippet,status";
constexpr const char* kChannelIdentityEndpoint = "https://www.googleapis.com/youtube/v3/channels?part=snippet&mine=true";
constexpr DWORD kDefaultHttpTimeoutMs = 30000;

std::atomic<bool> g_cancelRequested{false};

struct HttpResponse {
    DWORD statusCode = 0;
    std::string body;
    std::map<std::string, std::string> headersLower;
    std::string error;
};

using bean::util::EscapeJson;
using bean::util::ReadJsonString;
using bean::util::ToUtf8;
using bean::util::ToWide;
using bean::util::Trim;
using bean::util::UnescapeJson;

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

bool IsTransientHttpStatus(DWORD statusCode)
{
    return statusCode == 429 || statusCode == 500 || statusCode == 502 || statusCode == 503 || statusCode == 504;
}

// RAII wrapper for WinHTTP session + connection + request handles.
class WinHttpRequest {
public:
    WinHttpRequest() = default;
    WinHttpRequest(const WinHttpRequest&) = delete;
    WinHttpRequest& operator=(const WinHttpRequest&) = delete;

    ~WinHttpRequest()
    {
        if (request_) {
            WinHttpCloseHandle(request_);
            request_ = nullptr;
        }
        if (connection_) {
            WinHttpCloseHandle(connection_);
            connection_ = nullptr;
        }
        if (session_) {
            WinHttpCloseHandle(session_);
            session_ = nullptr;
        }
    }

    bool Open(const std::wstring& method, const std::string& urlUtf8, DWORD timeoutsMs, std::string& error)
    {
        const std::wstring url = ToWide(urlUtf8);
        if (url.empty()) {
            error = "Invalid request URL.";
            return false;
        }

        URL_COMPONENTSW parts{};
        std::wstring host;
        std::wstring pathAndQuery;
        bool isHttps = true;
        INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
        if (!CrackUrl(url, parts, host, pathAndQuery, isHttps, port, error)) {
            return false;
        }

        session_ = WinHttpOpen(L"Bean/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) {
            error = "WinHTTP initialization failed.";
            return false;
        }
        WinHttpSetTimeouts(session_, timeoutsMs, timeoutsMs, timeoutsMs, timeoutsMs);

        connection_ = WinHttpConnect(session_, host.c_str(), port, 0);
        if (!connection_) {
            error = "Failed to connect to remote host.";
            return false;
        }

        const DWORD requestFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
        request_ = WinHttpOpenRequest(
            connection_,
            method.c_str(),
            pathAndQuery.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            requestFlags);
        if (!request_) {
            error = "Failed to create HTTP request.";
            return false;
        }
        return true;
    }

    HINTERNET Handle() const { return request_; }

private:
    HINTERNET session_ = nullptr;
    HINTERNET connection_ = nullptr;
    HINTERNET request_ = nullptr;
};

HttpResponse SendRequestOnce(const std::wstring& method, const std::string& urlUtf8, const std::vector<std::wstring>& headers, const std::string& body)
{
    HttpResponse response;
    WinHttpRequest http;
    if (!http.Open(method, urlUtf8, kDefaultHttpTimeoutMs, response.error)) {
        return response;
    }

    std::wstring headersBlob;
    for (const auto& header : headers) {
        headersBlob += header;
        headersBlob += L"\r\n";
    }

    if (!WinHttpSendRequest(
            http.Handle(),
            headersBlob.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headersBlob.c_str(),
            headersBlob.empty() ? 0 : static_cast<DWORD>(headersBlob.size()),
            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()),
            0)) {
        response.error = "Failed to send HTTP request.";
        return response;
    }

    if (!WinHttpReceiveResponse(http.Handle(), nullptr)) {
        response.error = "Failed to receive HTTP response.";
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(http.Handle(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.statusCode = statusCode;

    DWORD rawHeaderSize = 0;
    WinHttpQueryHeaders(http.Handle(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &rawHeaderSize, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && rawHeaderSize > sizeof(wchar_t)) {
        std::wstring headerWide(static_cast<size_t>(rawHeaderSize / sizeof(wchar_t)), L'\0');
        if (WinHttpQueryHeaders(http.Handle(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, headerWide.data(), &rawHeaderSize, WINHTTP_NO_HEADER_INDEX)) {
            ParseHeaders(ToUtf8(headerWide), response.headersLower);
        }
    }

    std::string responseBody;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(http.Handle(), &available)) {
            response.error = "Failed while reading HTTP response.";
            break;
        }
        if (available == 0) {
            break;
        }
        std::string chunk(static_cast<size_t>(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(http.Handle(), chunk.data(), available, &read)) {
            response.error = "Failed while receiving HTTP response body.";
            break;
        }
        chunk.resize(static_cast<size_t>(read));
        responseBody += chunk;
    }
    response.body = std::move(responseBody);
    return response;
}

HttpResponse SendRequest(const std::wstring& method, const std::string& urlUtf8, const std::vector<std::wstring>& headers, const std::string& body)
{
    HttpResponse response;
    constexpr int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (g_cancelRequested.load(std::memory_order_relaxed)) {
            response.error = "YouTube request was cancelled.";
            return response;
        }

        response = SendRequestOnce(method, urlUtf8, headers, body);
        if (!response.error.empty()) {
            return response;
        }
        if (!IsTransientHttpStatus(response.statusCode) || attempt == kMaxAttempts) {
            return response;
        }

        if (g_cancelRequested.load(std::memory_order_relaxed)) {
            response.error = "YouTube request was cancelled.";
            return response;
        }
        Sleep(static_cast<DWORD>(250 * attempt));
    }
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

    WinHttpRequest http;
    if (!http.Open(L"PUT", sessionUrlUtf8, 60000, response.error)) {
        return response;
    }

    const std::wstring headers = L"Content-Type: application/octet-stream\r\n";
    const DWORD totalLength = static_cast<DWORD>(fileSize);
    if (!WinHttpSendRequest(
            http.Handle(),
            headers.c_str(),
            static_cast<DWORD>(headers.size()),
            WINHTTP_NO_REQUEST_DATA,
            0,
            totalLength,
            0)) {
        response.error = "Failed to begin upload request.";
        return response;
    }

    uint64_t bytesSent = 0;
    const uint64_t totalBytes = static_cast<uint64_t>(fileSize);
    if (progressCallback) {
        progressCallback(0, totalBytes, "uploading");
    }

    std::array<char, 1024 * 256> buffer{};
    while (stream.good()) {
        if (g_cancelRequested.load(std::memory_order_relaxed)) {
            response.error = "YouTube upload was cancelled.";
            return response;
        }
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = stream.gcount();
        if (got <= 0) {
            break;
        }
        DWORD written = 0;
        if (!WinHttpWriteData(http.Handle(), buffer.data(), static_cast<DWORD>(got), &written) || written != static_cast<DWORD>(got)) {
            response.error = "Failed while streaming video bytes to YouTube.";
            return response;
        }
        bytesSent += static_cast<uint64_t>(written);
        if (progressCallback) {
            progressCallback(bytesSent, totalBytes, "uploading");
        }
    }

    if (!WinHttpReceiveResponse(http.Handle(), nullptr)) {
        response.error = "Upload request completed without a valid response.";
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(http.Handle(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.statusCode = statusCode;

    std::string body;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(http.Handle(), &available)) {
            response.error = "Failed while reading upload response.";
            break;
        }
        if (available == 0) {
            break;
        }
        std::string chunk(static_cast<size_t>(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(http.Handle(), chunk.data(), available, &read)) {
            response.error = "Failed while receiving upload response.";
            break;
        }
        chunk.resize(static_cast<size_t>(read));
        body += chunk;
    }
    response.body = std::move(body);
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
    if (credentials.clientId.empty() || credentials.refreshToken.empty() || credentials.authServerUrl.empty()) {
        error = "Missing YouTube client ID, refresh token, or auth server URL.";
        return std::nullopt;
    }

    const std::string tokenBrokerUrl = NormalizeBaseUrl(credentials.authServerUrl) + "/token";
    std::map<std::string, std::string> form;
    form["client_id"] = credentials.clientId;
    form["refresh_token"] = credentials.refreshToken;

    const auto response = SendRequest(
        L"POST",
        tokenBrokerUrl,
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
        error = "Failed to refresh YouTube access token through auth server: " + details;
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

void YouTubeUploader::RequestCancel()
{
    g_cancelRequested.store(true, std::memory_order_relaxed);
}

void YouTubeUploader::ClearCancel()
{
    g_cancelRequested.store(false, std::memory_order_relaxed);
}

bool YouTubeUploader::IsCancelRequested()
{
    return g_cancelRequested.load(std::memory_order_relaxed);
}

YouTubeAuthResult YouTubeUploader::AuthorizeDesktop(HWND owner, const std::string& authServerUrl)
{
    YouTubeAuthResult result;
    ClearCancel();
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
        if (IsCancelRequested()) {
            result.error = "YouTube authorization was cancelled.";
            return result;
        }
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
            // Sleep in short slices so cancel is noticed within ~100ms.
            for (int slice = 0; slice < 10; ++slice) {
                if (IsCancelRequested()) {
                    result.error = "YouTube authorization was cancelled.";
                    return result;
                }
                Sleep(100);
            }
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
    ClearCancel();
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
