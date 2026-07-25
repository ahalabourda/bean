#include "util/Json.h"

#include "util/Strings.h"

#include <optional>
#include <string_view>

namespace bean::util {
namespace {

// Locates the raw, unquoted token following "key": - used for numbers and
// booleans, which are not quoted.
std::optional<std::string> FindScalarToken(const std::string& content, const std::string& key)
{
    const std::string marker = "\"" + key + "\"";
    const auto keyPos = content.find(marker);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }
    const auto colonPos = content.find(':', keyPos + marker.size());
    if (colonPos == std::string::npos) {
        return std::nullopt;
    }
    const auto valueStart = content.find_first_not_of(" \t\r\n", colonPos + 1);
    if (valueStart == std::string::npos) {
        return std::nullopt;
    }
    auto valueEnd = content.find_first_of(",}\r\n", valueStart);
    if (valueEnd == std::string::npos) {
        valueEnd = content.size();
    }
    return Trim(std::string_view(content).substr(valueStart, valueEnd - valueStart));
}

} // namespace

std::string EscapeJson(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char ch : value) {
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

std::string UnescapeJson(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    bool escape = false;
    for (const char ch : value) {
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
            default:
                // Covers \\ , \" and \/ , and passes anything else through.
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
    const auto openQuote = content.find('"', colonPos + 1);
    if (openQuote == std::string::npos) {
        return {};
    }

    bool escape = false;
    for (std::size_t index = openQuote + 1; index < content.size(); ++index) {
        const char ch = content[index];
        if (escape) {
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            return UnescapeJson(std::string_view(content).substr(openQuote + 1, index - openQuote - 1));
        }
    }
    return {};
}

int ReadJsonInt(const std::string& content, const std::string& key, int fallback)
{
    const auto token = FindScalarToken(content, key);
    if (!token.has_value()) {
        return fallback;
    }
    try {
        return std::stoi(*token);
    } catch (...) {
        return fallback;
    }
}

bool ReadJsonBool(const std::string& content, const std::string& key, bool fallback)
{
    const auto token = FindScalarToken(content, key);
    if (!token.has_value()) {
        return fallback;
    }
    if (*token == "true") {
        return true;
    }
    if (*token == "false") {
        return false;
    }
    return fallback;
}

} // namespace bean::util
