#pragma once

#include "Engine.h"

#include <functional>

namespace mfx
{

// MIN LATENCY: lowers the buffer size and then the IO cycle usage while watching the overload
// counter, and settles on the smallest setting that stayed clean. Each step is short; the setting it
// ends on is then watched for much longer, because a plugin that works in bursts overloads only now
// and then and a few seconds can miss it. Driven by a timer on the message
// thread; it never touches the sample rate.
//
// Every step is a trial. From start() to the end the engine's persisted audio settings are held at
// what they were before, and the run ends in exactly one of three ways: settled (the pair of buffer
// size and cycle usage that stayed clean is on the device and becomes what is persisted), restored
// (cancelled, failed or nothing was stable: the original buffer and cycle usage are back) or
// abandoned (the device itself was changed or lost underneath the run, so only the cycle usage is put
// back and the device is left as it now is).
class LatencyTuner
{
public:
    explicit LatencyTuner (Engine&);
    ~LatencyTuner();

    void start();
    void cancel();
    bool isRunning() const noexcept     { return phase != Phase::idle; }

    // Call every `tickMs`. Returns true when this call ended the run, whatever the outcome.
    bool tick();

    // What one step is judged by.
    struct Reading { int overloads = 0; int64 late = 0; double maxPercent = 0.0; };

    // Replaces the engine as the source of the readings; for tests, which have no HAL to overload.
    void setReader (std::function<Reading()>);

    String getProgress() const;
    String getResult() const            { return result; }

    // A setting is clean when nothing overloaded, no callback started late and the callback used
    // clearly less than the slice it was given.
    static bool isClean (int overloads, int64 lateCallbacks, double maxLoadPercent, double allowedPercent);

    static constexpr int tickMs = 250;

private:
    enum class Phase { idle, buffer, confirmBuffer, usage, confirmUsage };
    enum class Outcome { settle, restore, abandon };

    void advance();
    void beginStep();
    void finish (const String& reason, Outcome);
    bool stepIsClean() const;
    bool deviceIsUnchanged() const;

    Engine& engine;
    Phase phase = Phase::idle;
    Array<int> sizes;
    int index = 0, lastCleanIndex = -1, direction = -1;
    Array<float> usages;
    int usageIndex = 0, lastCleanUsageIndex = -1;
    AudioDeviceManager::AudioDeviceSetup originalSetup;
    int ticksLeft = 0, settleTicks = 0;
    String progress, result;
    Experiments original;
    std::function<Reading()> reader;
};

} // namespace mfx
