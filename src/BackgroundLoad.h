#pragma once

#include <juce_core/juce_core.h>
#include <memory>

namespace mfx
{

// Synthetic work that competes with the audio callback for CPU and memory bandwidth, the way a game or
// a plugin editor does. MIN LATENCY in its headroom mode runs it while it watches a setting, so that the
// setting it settles on survives a machine that is busy. start() and stop() may be called in any order
// and any number of times; stop() returns only after the work has ended, and destruction stops it.
class BackgroundLoad
{
public:
    virtual ~BackgroundLoad() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
};

// One worker per physical core but one (at least one), each at normal thread priority (never realtime,
// so the audio thread is what suffers and not the system) looping over a buffer of a few MB.
std::unique_ptr<BackgroundLoad> makeThreadedBackgroundLoad();

// The number of workers makeThreadedBackgroundLoad() uses for this many physical cores.
int backgroundLoadWorkers (int physicalCores) noexcept;

} // namespace mfx
