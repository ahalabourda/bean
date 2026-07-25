#pragma once

#include <string>

namespace bean::util {

// Minimal JSON string helpers shared by the settings store, the YouTube client
// and the UI. Three subtly different copies of these used to exist, and one of
// them silently skipped unescaping.
//
// This is deliberately not a JSON parser: it locates a top-level "key" and
// reads the value that follows. It is adequate for the flat documents bean
// reads and writes, and nothing more.
std::string EscapeJson(std::string_view value);
std::string UnescapeJson(std::string_view value);

std::string ReadJsonString(const std::string& content, const std::string& key);
int ReadJsonInt(const std::string& content, const std::string& key, int fallback);
bool ReadJsonBool(const std::string& content, const std::string& key, bool fallback);

} // namespace bean::util
