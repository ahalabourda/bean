#pragma once

#include "app/AppContext.h"

#include <functional>

// Joinable UI-thread-owned workers. Prefer this over std::thread(...).detach()
// so WM_DESTROY can wait for posts to a still-alive HWND.
void LaunchAppWorker(AppContext* ctx, std::function<void()> work);
void JoinAppWorkers(AppContext* ctx);
