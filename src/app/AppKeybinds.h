#pragma once

#include "app/AppContext.h"

void PushKeybindsToUi(AppContext* ctx);
void RegisterConfiguredHotkeys(AppContext* ctx);
void BeginKeybindCapture(AppContext* ctx, int index);
void UnbindKeybind(AppContext* ctx, int index);
void ResetKeybind(AppContext* ctx, int index);
bool CaptureKeybindKey(AppContext* ctx, WPARAM virtualKey);
