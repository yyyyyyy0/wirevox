#pragma once

#include "Engine.h"

namespace mfx
{

// MIN LATENCY: lowers the buffer size and then the IO cycle usage while watching the overload
// counter, and settles on the smallest setting that stayed clean. Each step is short; the setting it
// ends on is then watched for much longer, because a plugin that works in bursts overloads only now
// and then and a few seconds can miss it. Driven by a timer on the message
// thread; it never touches the sample rate.
class LatencyTuner
{
public:
    explicit LatencyTuner (Engine&);

    void start();
    void cancel();
    bool isRunning() const noexcept     { return phase != Phase::idle; }

    // Call every `tickMs`. Returns true when this call finished the run.
    bool tick();

    String getProgress() const;
    String getResult() const            { return result; }

    // A setting is clean when nothing overloaded, no callback started late and the callback used
    // clearly less than the slice it was given.
    static bool isClean (int overloads, int64 lateCallbacks, double maxLoadPercent, double allowedPercent);

    static constexpr int tickMs = 250;

private:
    enum class Phase { idle, buffer, confirmBuffer, usage, confirmUsage };

    void beginStep();
    void finish (const String& reason);
    bool stepIsClean() const;

    Engine& engine;
    Phase phase = Phase::idle;
    Array<int> sizes;
    int index = 0, lastCleanIndex = -1, direction = -1;
    Array<float> usages;
    int usageIndex = 0, lastCleanUsageIndex = -1;
    int originalBuffer = 0;
    int ticksLeft = 0, settleTicks = 0;
    String progress, result;
    Experiments original;
};

} // namespace mfx
