#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

struct HWND__;
using HWND = HWND__*;

namespace bean::integrations {

enum class YouTubePrivacy {
    Private,
    Unlisted,
    Public
};

struct YouTubeCredentials {
    std::string clientId;
    std::string refreshToken;
};

struct YouTubeAuthResult {
    bool success = false;
    std::string clientId;
    std::string refreshToken;
    std::string channelId;
    std::string channelTitle;
    std::string error;
};

struct YouTubeUploadRequest {
    std::filesystem::path videoPath;
    std::string title;
    YouTubePrivacy privacy = YouTubePrivacy::Private;
};

struct YouTubeUploadResult {
    bool success = false;
    std::string videoId;
    std::string videoUrl;
    std::string error;
};

struct YouTubeChannelIdentityResult {
    bool success = false;
    std::string channelId;
    std::string channelTitle;
    std::string error;
};

class YouTubeUploader {
public:
    using UploadProgressCallback = std::function<void(uint64_t bytesSent, uint64_t totalBytes, const std::string& phase)>;

    static YouTubeAuthResult AuthorizeDesktop(HWND owner, const std::string& authServerUrl);
    static YouTubeUploadResult UploadVideo(
        const YouTubeCredentials& credentials,
        const YouTubeUploadRequest& request,
        const UploadProgressCallback& progressCallback = {});
    static YouTubeChannelIdentityResult GetLinkedChannelIdentity(const YouTubeCredentials& credentials);
};

} // namespace bean::integrations
