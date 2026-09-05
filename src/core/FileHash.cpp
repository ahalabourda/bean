#include "core/FileHash.h"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <vector>

namespace bean::core {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

class HashCleanup {
public:
    HashCleanup(BCRYPT_ALG_HANDLE algorithm, BCRYPT_HASH_HANDLE hash)
        : algorithm_(algorithm)
        , hash_(hash)
    {
    }

    ~HashCleanup()
    {
        if (hash_) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    HashCleanup(const HashCleanup&) = delete;
    HashCleanup& operator=(const HashCleanup&) = delete;

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

std::string ToHex(const std::vector<unsigned char>& bytes)
{
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(kHexDigits[(byte >> 4) & 0x0F]);
        result.push_back(kHexDigits[byte & 0x0F]);
    }
    return result;
}

} // namespace

std::optional<std::string> ComputeFileSha256(
    const std::filesystem::path& path,
    std::string& error)
{
    error.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        error = "Unable to open file for hashing.";
        return std::nullopt;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        error = "Unable to open SHA-256 provider.";
        return std::nullopt;
    }

    DWORD objectLength = 0;
    DWORD resultLength = 0;
    if (BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength),
            &resultLength,
            0)
        != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "Unable to determine SHA-256 object size.";
        return std::nullopt;
    }

    DWORD hashLength = 0;
    if (BCryptGetProperty(
            algorithm,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength),
            sizeof(hashLength),
            &resultLength,
            0)
        != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "Unable to determine SHA-256 hash size.";
        return std::nullopt;
    }

    std::vector<unsigned char> hashObject(objectLength);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(
            algorithm,
            &hash,
            hashObject.data(),
            static_cast<ULONG>(hashObject.size()),
            nullptr,
            0,
            0)
        != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "Unable to create SHA-256 hash.";
        return std::nullopt;
    }
    HashCleanup cleanup(algorithm, hash);

    std::vector<char> buffer(1024 * 1024);
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytesRead = stream.gcount();
        if (bytesRead <= 0) {
            break;
        }
        if (BCryptHashData(
                hash,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(bytesRead),
                0)
            != 0) {
            error = "Unable to hash recording file.";
            return std::nullopt;
        }
    }
    if (stream.bad()) {
        error = "Unable to read recording file for hashing.";
        return std::nullopt;
    }

    std::vector<unsigned char> hashValue(hashLength);
    if (BCryptFinishHash(
            hash,
            hashValue.data(),
            static_cast<ULONG>(hashValue.size()),
            0)
        != 0) {
        error = "Unable to finalize SHA-256 hash.";
        return std::nullopt;
    }
    return ToHex(hashValue);
}

} // namespace bean::core
