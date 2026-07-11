#pragma once

#include "app/AppContext.h"

#include <filesystem>
#include <string>

std::filesystem::path ResolveStatusLogDirectory(const AppContext* ctx);
std::wstring InitializeStatusLogFile(AppContext* ctx);
void LogSessionDiagnostics(AppContext* ctx);
void SetStatus(AppContext* ctx, const std::wstring& text);
void PostStatus(AppContext* ctx, const std::wstring& text);
