#include "LatencyTuner.h"
#include "Text.h"

namespace mfx
{

namespace
{
    constexpr int watchTicks = 12;          // 3 s
    constexpr int confirmTicks = 48;        // 12 s
    constexpr int settleAfterReopen = 3;    // 0.75 s
    constexpr double maxLoadPercent = 85.0;
    constexpr int largestBuffer = 1024;
}

LatencyTuner::LatencyTuner (Engine& e) : engine (e) {}

LatencyTuner::~LatencyTuner()
{
    cancel();
}

void LatencyTuner::setReader (std::function<Reading()> newReader)
{
    reader = std::move (newReader);
}

bool LatencyTuner::isClean (int overloads, int64 late, double maxLoad, double allowedPercent)
{
    return overloads == 0 && late == 0 && maxLoad <= allowedPercent;
}

void LatencyTuner::start()
{
    result.clear();
    auto* device = engine.deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        result = ja ("デバイスが開いていません");
        return;
    }

    sizes.clear();

    for (auto size : device->getAvailableBufferSizes())
        if (size <= largestBuffer)
            sizes.add (size);

    if (sizes.isEmpty())
    {
        result = ja ("このデバイスには試せるバッファサイズがありません");
        return;
    }

    const auto current = device->getCurrentBufferSizeSamples();
    index = 0;

    for (int i = 0; i < sizes.size(); ++i)
        if (sizes[i] <= current)
            index = i;

    originalSetup = engine.deviceManager.getAudioDeviceSetup();
    original = engine.getExperiments();
    usages = { 0.75f, 0.5f, 0.35f, 0.25f };
    usageIndex = 0;
    lastCleanIndex = -1;
    lastCleanUsageIndex = -1;
    direction = 0;          // decided by the first step
    engine.holdPersistedAudio();
    phase = Phase::buffer;
    beginStep();
}

void LatencyTuner::cancel()
{
    if (phase != Phase::idle)
        finish (ja ("中止しました。元の設定に戻しました"), Outcome::restore);
}

void LatencyTuner::beginStep()
{
    const auto confirming = phase == Phase::confirmBuffer || phase == Phase::confirmUsage;
    const auto suffix = confirming ? ja (" を長めに確認中") : ja (" を確認中");

    if (phase == Phase::buffer || phase == Phase::confirmBuffer)
    {
        const auto current = engine.deviceManager.getCurrentAudioDevice() != nullptr
                           ? engine.deviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples() : 0;

        if (sizes[index] != current && engine.setBufferSize (sizes[index]) == 0)
        {
            finish (ja ("バッファサイズを変更できませんでした。元の設定に戻しました"), Outcome::restore);
            return;
        }

        progress = ja ("バッファ ") + String (sizes[index]) + suffix;
        settleTicks = settleAfterReopen;
    }
    else
    {
        const auto usage = phase == Phase::usage ? usages[usageIndex] : usages[lastCleanUsageIndex];
        engine.setExperiments ({ usage });
        progress = ja ("サイクル使用率 ") + String (usage, 2) + suffix;
        settleTicks = 2;
    }

    ticksLeft = confirming ? confirmTicks : watchTicks;
    engine.refreshHal();
}

bool LatencyTuner::stepIsClean() const
{
    Reading reading;

    if (reader)
    {
        reading = reader();
    }
    else
    {
        const auto load = engine.callback.getLoad();
        reading = { engine.getOverloads(), load.late, load.maxPercent };
    }

    const auto usagePhase = phase == Phase::usage || phase == Phase::confirmUsage;
    const auto usage = phase == Phase::usage ? usages[usageIndex] : phase == Phase::confirmUsage ? usages[lastCleanUsageIndex] : 1.0f;
    const auto allowed = usagePhase ? jmin (maxLoadPercent, 60.0 * (double) usage) : maxLoadPercent;
    return isClean (reading.overloads, reading.late, reading.maxPercent, allowed);
}

bool LatencyTuner::deviceIsUnchanged() const
{
    const auto setup = engine.deviceManager.getAudioDeviceSetup();

    return engine.deviceManager.getCurrentAudioDevice() != nullptr
        && setup.inputDeviceName == originalSetup.inputDeviceName
        && setup.outputDeviceName == originalSetup.outputDeviceName;
}

bool LatencyTuner::tick()
{
    if (phase == Phase::idle)
        return false;

    // The device is only ever changed by this run's own steps, which keep its names. Anything else
    // (an unplugged interface, a switch made elsewhere) makes the plan this run started with invalid.
    if (! deviceIsUnchanged())
    {
        finish (ja ("デバイスが変わったため中止しました"), Outcome::abandon);
        return true;
    }

    if (settleTicks > 0)
    {
        if (--settleTicks == 0)
        {
            engine.refreshHal();
            engine.resetOverloads();
            engine.callback.resetLoad();
        }

        return false;
    }

    if (--ticksLeft > 0)
        return false;

    advance();
    return phase == Phase::idle;
}

void LatencyTuner::advance()
{
    const auto clean = stepIsClean();

    switch (phase)
    {
        case Phase::buffer:
        {
            if (clean)
                lastCleanIndex = index;

            // The first step decides which way to walk: down while it is clean, up while it is not.
            if (direction == 0)
                direction = clean ? -1 : 1;

            const auto next = index + direction;

            if ((direction < 0 && clean && next >= 0) || (direction > 0 && ! clean && next < sizes.size()))
            {
                index = next;
                beginStep();
                return;
            }

            if (lastCleanIndex < 0)
            {
                finish (ja ("どのバッファサイズでも安定しませんでした。元に戻しました"), Outcome::restore);
                return;
            }

            index = lastCleanIndex;
            phase = Phase::confirmBuffer;
            beginStep();
            return;
        }

        case Phase::confirmBuffer:
        {
            if (clean)
            {
                phase = Phase::usage;
                usageIndex = 0;
                beginStep();
                return;
            }

            // The short check missed a burst: take the next larger size.
            if (lastCleanIndex + 1 < sizes.size())
            {
                index = ++lastCleanIndex;
                beginStep();
                return;
            }

            finish (ja ("長く見ると、どのバッファサイズでも音切れが出ました。元に戻しました"), Outcome::restore);
            return;
        }

        case Phase::usage:
        {
            if (clean)
            {
                lastCleanUsageIndex = usageIndex;

                if (++usageIndex < usages.size())
                {
                    beginStep();
                    return;
                }
            }

            if (lastCleanUsageIndex < 0)
            {
                finish ({}, Outcome::settle);
                return;
            }

            phase = Phase::confirmUsage;
            beginStep();
            return;
        }

        case Phase::confirmUsage:
        {
            if (clean)
            {
                finish ({}, Outcome::settle);
                return;
            }

            if (--lastCleanUsageIndex >= 0)
            {
                beginStep();
                return;
            }

            finish ({}, Outcome::settle);
            return;
        }

        case Phase::idle:
            break;
    }
}

void LatencyTuner::finish (const String& reason, Outcome outcome)
{
    String text = reason;

    if (outcome == Outcome::settle)
    {
        auto setup = originalSetup;
        setup.bufferSize = sizes[lastCleanIndex];

        if (engine.openWithSetup (setup) != 0)
        {
            engine.setExperiments (lastCleanUsageIndex >= 0 ? Experiments { usages[lastCleanUsageIndex] } : Experiments {});
        }
        else
        {
            outcome = Outcome::restore;
            text = ja ("見つけた設定でデバイスを開けませんでした。元の設定に戻しました");
        }
    }

    if (outcome == Outcome::restore)
    {
        engine.setExperiments (original);

        if (engine.openWithSetup (originalSetup) == 0)
            text << ja ("（元のバッファサイズでデバイスを開き直せませんでした）");
    }
    else if (outcome == Outcome::abandon)
    {
        engine.setExperiments (original);
    }

    if (text.isEmpty())
    {
        text = ja ("最小遅延: バッファ ") + String (sizes[lastCleanIndex]);
        text << (lastCleanUsageIndex >= 0 ? ja (" / サイクル使用率 ") + String (usages[lastCleanUsageIndex], 2) : ja (" / サイクル使用率は既定のまま"));
    }

    engine.releasePersistedAudio();
    result = text;
    phase = Phase::idle;
    progress.clear();
}

String LatencyTuner::getProgress() const
{
    return progress;
}

} // namespace mfx
