#pragma once

#include "core/SettingsStore.h"

#include <string>

bool ApplyReasonableDefaults(bean::core::AppSettings& settings, std::string& warning);
