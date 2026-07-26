#include "app/AppLiveStatus.h"

#include <utility>
#include <vector>

void LaunchAppWorker(AppContext* ctx, std::function<void()> work)
{
    if (!ctx || !work) {
        return;
    }
    std::scoped_lock lock(ctx->backgroundWorkers.mutex);
    ctx->backgroundWorkers.threads.emplace_back(std::move(work));
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
