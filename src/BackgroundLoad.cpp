#include "BackgroundLoad.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace mfx
{

namespace
{
    constexpr size_t bufferBytes = 4u * 1024u * 1024u;

    class Worker final : public juce::Thread
    {
    public:
        Worker() : Thread ("WIREVOX background load"), data (bufferBytes / sizeof (uint64_t), 1u) {}
        ~Worker() override { stopThread (5000); }

        void run() override
        {
            uint64_t carry = 1;

            while (! threadShouldExit())
            {
                // A pass over the whole buffer takes a few milliseconds, so the stop request is seen quickly.
                for (auto& word : data)
                {
                    carry = carry * 6364136223846793005ull + word + 1442695040888963407ull;
                    word = carry;
                }

                sink.store (carry, std::memory_order_relaxed);
            }
        }

    private:
        std::vector<uint64_t> data;
        std::atomic<uint64_t> sink { 0 };
    };

    class ThreadedBackgroundLoad final : public BackgroundLoad
    {
    public:
        ~ThreadedBackgroundLoad() override { stop(); }

        void start() override
        {
            if (! workers.empty())
                return;

            const auto count = backgroundLoadWorkers (juce::SystemStats::getNumPhysicalCpus());

            for (int i = 0; i < count; ++i)
            {
                workers.push_back (std::make_unique<Worker>());
                workers.back()->startThread (juce::Thread::Priority::normal);
            }
        }

        void stop() override
        {
            for (auto& worker : workers)
                worker->signalThreadShouldExit();

            workers.clear();    // each destructor joins
        }

    private:
        std::vector<std::unique_ptr<Worker>> workers;
    };
}

int backgroundLoadWorkers (int physicalCores) noexcept
{
    return std::max (1, physicalCores - 1);
}

std::unique_ptr<BackgroundLoad> makeThreadedBackgroundLoad()
{
    return std::make_unique<ThreadedBackgroundLoad>();
}

} // namespace mfx
