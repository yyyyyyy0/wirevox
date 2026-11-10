#include "ProcessTime.h"

#include <mach/mach_time.h>

namespace mfx
{

ProcessTimer::ProcessTimer()
{
    mach_timebase_info_data_t info {};
    mach_timebase_info (&info);
    nsPerTick = info.denom != 0 ? (double) info.numer / (double) info.denom : 1.0;
}

void ProcessTimer::begin() noexcept
{
    startTicks.store (mach_absolute_time(), std::memory_order_relaxed);
}

void ProcessTimer::end() noexcept
{
    const auto started = startTicks.load (std::memory_order_relaxed);

    if (started == 0)
        return;

    record ((int64_t) ((double) (mach_absolute_time() - started) * nsPerTick));
}

void ProcessTimer::record (int64_t nanoseconds) noexcept
{
    if (nanoseconds < 0)
        nanoseconds = 0;

    auto current = maxNs.load (std::memory_order_relaxed);

    while (nanoseconds > current && ! maxNs.compare_exchange_weak (current, nanoseconds, std::memory_order_relaxed))
    {}

    sumNs.fetch_add (nanoseconds, std::memory_order_relaxed);
    count.fetch_add (1, std::memory_order_release);
}

ProcessTimer::Window ProcessTimer::drain() noexcept
{
    Window window;
    window.count = count.exchange (0, std::memory_order_acquire);
    window.sumNs = sumNs.exchange (0, std::memory_order_relaxed);
    window.maxNs = maxNs.exchange (0, std::memory_order_relaxed);
    return window;
}

//==============================================================================
void ProcessWindows::push (const ProcessTimer::Window& window) noexcept
{
    ring[next] = window;
    next = (next + 1) % ring.size();
}

void ProcessWindows::clear() noexcept
{
    ring.fill ({});
    next = 0;
}

ProcessStats ProcessWindows::stats (double periodNs) const noexcept
{
    int64_t blocks = 0, sum = 0, worst = 0;

    for (const auto& window : ring)
    {
        blocks += window.count;
        sum += window.sumNs;

        if (window.maxNs > worst)
            worst = window.maxNs;
    }

    ProcessStats result;

    if (blocks <= 0 || periodNs <= 0.0)
        return result;

    result.valid = true;
    result.blocks = blocks;
    result.meanPercent = 100.0 * ((double) sum / (double) blocks) / periodNs;
    result.maxPercent = 100.0 * (double) worst / periodNs;
    return result;
}

int findHeaviest (const std::vector<ProcessStats>& all) noexcept
{
    int best = -1;

    for (size_t i = 0; i < all.size(); ++i)
    {
        if (! all[i].valid)
            continue;

        if (best < 0)
        {
            best = (int) i;
            continue;
        }

        const auto& current = all[(size_t) best];

        if (all[i].maxPercent > current.maxPercent
            || (! (all[i].maxPercent < current.maxPercent) && all[i].meanPercent > current.meanPercent))
            best = (int) i;
    }

    return best;
}

} // namespace mfx
