#include "core/RecordingPath.h"

#include <algorithm>
#include <cctype>

namespace bean::core {

std::filesystem::path BuildRecordingPath(
    const std::filesystem::path& outputDirectory,
    const std::string& fileStem,
    std::string_view containerFormat)
{
    std::string normalized(containerFormat);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    const char* extension = (normalized == "mp4") ? ".mp4" : ".mkv";
    return outputDirectory / (fileStem + extension);
}

} // namespace bean::core
