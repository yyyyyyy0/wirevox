#include "HostCallback.h"
#include "Correlate.h"

#include <mach/mach_time.h>

namespace mfx
{

namespace
{
    // Ticks of mach_absolute_time to nanoseconds.
    double ticksToNs (uint64_t ticks)
    {
        static const auto factor = []
        {
            mach_timebase_info_data_t info {};
            mach_timebase_info (&info);
            return (double) info.numer / (double) info.denom;
        }();

        return (double) ticks * factor;
    }
}

HostCallback::HostCallback (AudioProcessorPlayer& playerToUse)
    : player (playerToUse)
{
    ticksToNs (1);      // fills the cached timebase on a thread that may allocate
}

void HostCallback::audioDeviceAboutToStart (AudioIODevice* device)
{
    const auto rate = device->getCurrentSampleRate();
    const auto block = device->getCurrentBufferSizeSamples();

    periodNs = rate > 0.0 ? 1.0e9 * block / rate : 0.0;
    lastStartTicks = 0;
    callbackCounter = 0;
    burstPosition = -1;
    seenEpoch = injectionEpoch.load();
    burst = measure::makeTestSignal (rate);

    scratch.setSize (jmax (1, device->getActiveInputChannels().countNumberOfSetBits()), jmax (1, block));
    scratchPointers.assign ((size_t) scratch.getNumChannels(), nullptr);

    for (int c = 0; c < scratch.getNumChannels(); ++c)
        scratchPointers[(size_t) c] = scratch.getReadPointer (c);

    lastBlockSize = block;
    resetLoad();
    ++startCount;
    player.audioDeviceAboutToStart (device);
}

void HostCallback::audioDeviceStopped()
{
    player.audioDeviceStopped();
}

void HostCallback::audioDeviceError (const String& message)
{
    player.audioDeviceError (message);
}

//==============================================================================
void HostCallback::resetLoad()
{
    callbacks = 0;
    late = 0;
    busyNs = 0;
    maxLoadPermille = 0;
}

HostCallback::Load HostCallback::getLoad() const
{
    Load load;
    load.callbacks = callbacks.load();
    load.late = late.load();

    if (load.callbacks > 0 && periodNs > 0.0)
    {
        load.meanPercent = 100.0 * (double) busyNs.load() / ((double) load.callbacks * periodNs);
        load.maxPercent = (double) maxLoadPermille.load() / 10.0;
    }

    return load;
}

void HostCallback::setInjecting (bool shouldInject) noexcept
{
    armed = false;
    disturbed = false;
    ++injectionEpoch;
    injecting = shouldInject;
}

//==============================================================================
const float* const* HostCallback::applyInjection (const float* const* input, int numInputs, int numSamples,
                                                  const AudioIODeviceCallbackContext& context)
{
    if (const auto epoch = injectionEpoch.load (std::memory_order_acquire); epoch != seenEpoch)
    {
        seenEpoch = epoch;
        burstPosition = -1;
    }

    if (! injecting.load (std::memory_order_acquire))
        return input;

    // The live microphone must never reach the output during a measurement, so a block the scratch
    // buffer cannot hold is dropped instead of passed through.
    if (numInputs > scratch.getNumChannels() || numSamples > scratch.getNumSamples() || burst.empty())
    {
        disturbed.store (true, std::memory_order_release);
        return nullptr;
    }

    if (numInputs <= 0)
    {
        disturbed.store (true, std::memory_order_release);
        return input;
    }

    if (burstPosition < 0 && armed.exchange (false))
    {
        const auto index = injectionCount.load (std::memory_order_relaxed);

        if (index < injections.size())
            injections[index] = { context.hostTimeNs != nullptr ? *context.hostTimeNs : 0, callbackCounter };

        injectionCount.store (index + 1, std::memory_order_release);
        burstPosition = 0;
    }

    const auto toCopy = burstPosition < 0 ? 0 : jmin (numSamples, (int) burst.size() - burstPosition);

    for (int c = 0; c < numInputs; ++c)
    {
        auto* out = scratch.getWritePointer (c);
        FloatVectorOperations::clear (out, numSamples);

        if (toCopy > 0)
            FloatVectorOperations::copy (out, burst.data() + burstPosition, toCopy);
    }

    if (burstPosition >= 0)
    {
        burstPosition += numSamples;

        if (burstPosition >= (int) burst.size())
            burstPosition = -1;
    }

    return scratchPointers.data();
}

void HostCallback::audioDeviceIOCallbackWithContext (const float* const* input, int numInputs, float* const* output,
                                                     int numOutputs, int numSamples, const AudioIODeviceCallbackContext& context)
{
    const auto start = mach_absolute_time();

    // A callback that starts far later than one period after the previous one was held up by the
    // scheduler, whatever the HAL reports.
    if (lastStartTicks != 0 && periodNs > 0.0 && ticksToNs (start - lastStartTicks) > 1.5 * periodNs)
        late.fetch_add (1, std::memory_order_relaxed);

    lastStartTicks = start;
    ++callbackCounter;
    lastBlockSize.store (numSamples, std::memory_order_relaxed);

    const auto* usedInput = applyInjection (input, numInputs, numSamples, context);

    if (usedInput == nullptr && numInputs > 0)
    {
        for (int c = 0; c < numOutputs; ++c)
            if (output[c] != nullptr)
                FloatVectorOperations::clear (output[c], numSamples);
    }
    else
    {
        player.audioDeviceIOCallbackWithContext (usedInput, numInputs, output, numOutputs, numSamples, context);
    }

    const auto busy = (int64) ticksToNs (mach_absolute_time() - start);
    callbacks.fetch_add (1, std::memory_order_relaxed);
    busyNs.fetch_add (busy, std::memory_order_relaxed);

    if (periodNs > 0.0)
    {
        const auto permille = (int64) (1000.0 * (double) busy / periodNs);
        auto current = maxLoadPermille.load (std::memory_order_relaxed);

        while (permille > current && ! maxLoadPermille.compare_exchange_weak (current, permille, std::memory_order_relaxed))
        {}
    }
}

} // namespace mfx
