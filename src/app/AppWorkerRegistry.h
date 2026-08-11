#pragma once

#include <mutex>
#include <thread>
#include <vector>

// UI-owned worker registry. Keeping the registry as a small composition unit
// makes the shutdown contract explicit: workers are admitted while the app is
// live and joined before the owning window/context is destroyed.
struct AppWorkerRegistry {
    std::mutex mutex;
    std::vector<std::thread> threads;
};
