#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace bean::core {

// Computes a SHA-256 digest without retaining the file in memory. Callers
// should invoke this from a worker thread for large recording files.
std::optional<std::string> ComputeFileSha256(
    const std::filesystem::path& path,
    std::string& error);

} // namespace bean::core
