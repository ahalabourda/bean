#pragma once

#include "app/AppContext.h"

void InitializeAppIcons(AppContext* ctx);
void DestroyAppIcons(AppContext* ctx);
void InitializeTaskbarOverlay(AppContext* ctx);
void ApplyTaskbarOverlayState(AppContext* ctx, bool forceUpdate = false);
void ShutdownTaskbarOverlay(AppContext* ctx);
