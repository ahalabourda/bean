#include "app/AppLiveStatus.h"

#include <utility>
#include <vector>

bool LaunchAppWorker(AppContext* ctx, std::function<void()> work)
{
    if (!ctx || !work || ctx->shuttingDown.load(std::memory_order_acquire)) {
        return false;
    }
    std::scoped_lock lock(ctx->backgroundWorkers.mutex);
    if (ctx->shuttingDown.load(std::memory_order_acquire)) {
        return false;
    }
    ctx->backgroundWorkers.threads.emplace_back(std::move(work));
    return true;
}

void JoinAppWorkers(AppContext* ctx)
{
    if (!ctx) {
        return;
    }
    std::vector<std::thread> threads;
    {
        std::scoped_lock lock(ctx->backgroundWorkers.mutex);
        threads.swap(ctx->backgroundWorkers.threads);
    }
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}
