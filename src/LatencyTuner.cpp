#include "LatencyTuner.h"
#include "Text.h"

namespace mfx
{

namespace
{
    constexpr int watchTicks = 12;          // 3 s
    constexpr int confirmTicks = 48;        // 12 s
    constexpr int settleAfterReopen = 3;    // 0.75 s
    constexpr int largestBuffer = 1024;
}

LatencyTuner::LatencyTuner (Engine& e) : engine (e) {}

LatencyTuner::~LatencyTuner()
{
    cancel();

    if (backgroundLoad != nullptr)
        backgroundLoad->stop();
}

void LatencyTuner::setBackgroundLoad (std::unique_ptr<BackgroundLoad> newLoad)
{
    if (backgroundLoad != nullptr)
        backgroundLoad->stop();

    backgroundLoad = std::move (newLoad);
}

double LatencyTuner::bufferLoadLimit() const noexcept
{
    return mode == TuneMode::headroom ? headroomMaxLoadPercent : fastestMaxLoadPercent;
}

void LatencyTuner::setReader (std::function<Reading()> newReader)
{
    reader = std::move (newReader);
}

bool LatencyTuner::isClean (int overloads, int64 late, double maxLoad, double allowedPercent)
{
    return overloads == 0 && late == 0 && maxLoad <= allowedPercent;
}

void LatencyTuner::start (TuneMode newMode)
{
    result.clear();
    mode = newMode;
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
    originalMonitor = engine.getMonitorPlan();
    monitorWasActive = engine.isMonitorActive();
    monitorDropped = false;
    usages = { 0.75f, 0.5f, 0.35f, 0.25f };
    usageIndex = 0;
    lastCleanIndex = -1;
    lastCleanUsageIndex = -1;
    direction = 0;          // decided by the first step
    engine.holdPersistedAudio();

    // A lower cycle usage shrinks the time the callback may take, which is what headroom is for.
    if (mode == TuneMode::headroom)
    {
        if (backgroundLoad == nullptr)
            backgroundLoad = makeThreadedBackgroundLoad();

        engine.setExperiments ({});
    }

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
    monitorDropped = false;

    // The device is reopened below; the load comes back once it has settled (see tick()).
    if (backgroundLoad != nullptr)
        backgroundLoad->stop();

    const auto confirming = phase == Phase::confirmBuffer || phase == Phase::confirmUsage;
    const auto suffix = String (confirming ? ja (" を長めに確認中") : ja (" を確認中"))
                      + (mode == TuneMode::headroom ? ja ("（背景の負荷を掛けています）") : String());

    if (phase == Phase::buffer || phase == Phase::confirmBuffer)
    {
        const auto current = engine.deviceManager.getCurrentAudioDevice() != nullptr
                           ? engine.deviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples() : 0;

        if (sizes[index] != current && engine.setBufferSize (sizes[index]) == 0)
        {
            finish (ja ("バッファサイズを変更できませんでした。元の設定に戻しました"), Outcome::restore);
            return;
        }

        // A size the monitor's aggregate cannot open makes the engine drop the monitor. That size is not clean
        // for this setup, and the saved choice is put back so the next step tries the monitor again.
        if (monitorWasActive && ! engine.isMonitorActive())
        {
            monitorDropped = true;
            engine.setMonitorPlan (originalMonitor, false);
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
    if (monitorDropped)
        return false;

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
    const auto allowed = usagePhase ? jmin (bufferLoadLimit(), 60.0 * (double) usage) : bufferLoadLimit();
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

            if (mode == TuneMode::headroom && backgroundLoad != nullptr)
                backgroundLoad->start();
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
                // Headroom keeps the device's default cycle usage, so the search ends with the buffer.
                if (mode == TuneMode::headroom)
                {
                    finish ({}, Outcome::settle);
                    return;
                }

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

    // Whatever ends the run, the synthetic load ends with it, before the device is opened for the last time.
    if (backgroundLoad != nullptr)
        backgroundLoad->stop();

    // The saved monitor choice is back before the final open, which makes a new device for it if it is needed.
    if (monitorWasActive && outcome != Outcome::abandon)
        engine.setMonitorPlan (originalMonitor, false);

    if (outcome == Outcome::settle)
    {
        auto setup = originalSetup;
        setup.bufferSize = sizes[lastCleanIndex];

        if (engine.openWithSetup (setup, true) != 0)
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
        text = ja (mode == TuneMode::headroom ? "最小遅延（余裕あり）: バッファ " : "最小遅延（最速）: バッファ ") + String (sizes[lastCleanIndex]);
        text << (lastCleanUsageIndex >= 0 ? ja (" / サイクル使用率 ") + String (usages[lastCleanUsageIndex], 2) : ja (" / サイクル使用率は既定のまま"));

        if (mode == TuneMode::headroom)
            text << ja ("。ゲームを動かしたまま押すと、より実際に近い結果になります");
    }

    if (monitorWasActive && outcome != Outcome::abandon && ! engine.isMonitorActive())
    {
        const auto problem = engine.takeMonitorProblem();
        text << " / " << (problem.isNotEmpty() ? problem : ja ("モニターは止まったままです"));
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
