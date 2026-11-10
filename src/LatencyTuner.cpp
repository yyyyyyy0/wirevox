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

bool LatencyTuner::isClean (int overloads, int64 late, double maxLoad, double allowedPercent)
{
    return overloads == 0 && late == 0 && maxLoad <= allowedPercent;
}

void LatencyTuner::start()
{
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

    const auto current = device->getCurrentBufferSizeSamples();
    index = 0;

    for (int i = 0; i < sizes.size(); ++i)
        if (sizes[i] <= current)
            index = i;

    original = engine.getExperiments();
    originalBuffer = current;
    usages = { 0.75f, 0.5f, 0.35f, 0.25f };
    usageIndex = 0;
    lastCleanIndex = -1;
    lastCleanUsageIndex = -1;
    direction = 0;          // decided by the first step
    result.clear();
    phase = Phase::buffer;
    beginStep();
}

void LatencyTuner::cancel()
{
    if (phase == Phase::idle)
        return;

    lastCleanIndex = -1;
    engine.setBufferSize (originalBuffer);
    finish (ja ("中止しました。元の設定に戻しました"));
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
            finish (ja ("バッファサイズを変更できませんでした"));
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
    const auto load = engine.callback.getLoad();
    const auto usagePhase = phase == Phase::usage || phase == Phase::confirmUsage;
    const auto usage = phase == Phase::usage ? usages[usageIndex] : phase == Phase::confirmUsage ? usages[lastCleanUsageIndex] : 1.0f;
    const auto allowed = usagePhase ? jmin (maxLoadPercent, 60.0 * (double) usage) : maxLoadPercent;
    return isClean (engine.getOverloads(), load.late, load.maxPercent, allowed);
}

bool LatencyTuner::tick()
{
    if (phase == Phase::idle)
        return false;

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
                return false;
            }

            if (lastCleanIndex < 0)
            {
                engine.setBufferSize (originalBuffer);
                finish (ja ("どのバッファサイズでも安定しませんでした。元に戻しました"));
                return true;
            }

            index = lastCleanIndex;
            phase = Phase::confirmBuffer;
            beginStep();
            return ! isRunning();
        }

        case Phase::confirmBuffer:
        {
            if (clean)
            {
                phase = Phase::usage;
                usageIndex = 0;
                beginStep();
                return false;
            }

            // The short check missed a burst: take the next larger size.
            if (lastCleanIndex + 1 < sizes.size())
            {
                index = ++lastCleanIndex;
                beginStep();
                return ! isRunning();
            }

            engine.setBufferSize (originalBuffer);
            lastCleanIndex = -1;
            finish (ja ("長く見ると、どのバッファサイズでも音切れが出ました。元に戻しました"));
            return true;
        }

        case Phase::usage:
        {
            if (clean)
            {
                lastCleanUsageIndex = usageIndex;

                if (++usageIndex < usages.size())
                {
                    beginStep();
                    return false;
                }
            }

            if (lastCleanUsageIndex < 0)
            {
                finish ({});
                return true;
            }

            phase = Phase::confirmUsage;
            beginStep();
            return false;
        }

        case Phase::confirmUsage:
        {
            if (clean)
            {
                finish ({});
                return true;
            }

            --lastCleanUsageIndex;

            if (lastCleanUsageIndex >= 0)
            {
                beginStep();
                return false;
            }

            finish ({});
            return true;
        }

        case Phase::idle:
            break;
    }

    return false;
}

void LatencyTuner::finish (const String& reason)
{
    if (lastCleanIndex >= 0 && lastCleanUsageIndex >= 0)
        engine.setExperiments ({ usages[lastCleanUsageIndex] });
    else if (lastCleanIndex >= 0)
        engine.setExperiments ({});
    else
        engine.setExperiments (original);

    String text = reason;

    if (text.isEmpty() && lastCleanIndex >= 0)
    {
        text = ja ("最小遅延: バッファ ") + String (sizes[lastCleanIndex]);
        text << (lastCleanUsageIndex >= 0 ? ja (" / サイクル使用率 ") + String (usages[lastCleanUsageIndex], 2) : ja (" / サイクル使用率は既定のまま"));
    }

    result = text;
    phase = Phase::idle;
    progress.clear();
}

String LatencyTuner::getProgress() const
{
    return progress;
}

} // namespace mfx
