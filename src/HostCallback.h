#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <vector>

namespace mfx
{
using namespace juce;

// Sits between the audio device and the AudioProcessorPlayer. On the audio thread it only reads the
// clock, bumps atomics and, while a measurement runs, overwrites the input with the test burst. It
// allocates, locks and logs nothing.
class HostCallback final : public AudioIODeviceCallback
{
public:
    explicit HostCallback (AudioProcessorPlayer&);

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int, const AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const String&) override;

    //==============================================================================
    struct Load
    {
        int64 callbacks = 0, late = 0;
        double meanPercent = 0.0, maxPercent = 0.0;     // callback duration against the buffer period
    };

    // Everything is relative to the last reset (the device start also resets).
    Load getLoad() const;
    void resetLoad();

    uint32 getStartCount() const noexcept       { return startCount.load(); }
    int getBlockSize() const noexcept           { return lastBlockSize.load(); }
    double getSampleRate() const noexcept       { return currentRate.load(); }

    //==============================================================================
    // Latency measurement. While injecting, the input is replaced by silence plus the burst.
    struct Injection
    {
        uint64 outputHostNs = 0;    // the HAL output time of the cycle that carried the first burst sample
        uint64 callbackIndex = 0;
    };

    void setInjecting (bool shouldInject) noexcept;
    void armBurst() noexcept                    { armed = true; }
    int numInjections() const noexcept          { return jmin ((int) injectionCount.load(), (int) injections.size()); }
    Injection getInjection (int i) const noexcept { return injections[(size_t) i]; }
    void clearInjections() noexcept             { injectionCount = 0; }
    int getBurstLength() const noexcept         { return (int) burst.size(); }
    const std::vector<float>& getBurst() const noexcept { return burst; }

private:
    const float* const* applyInjection (const float* const* input, int numInputs, int numSamples, const AudioIODeviceCallbackContext&);

    AudioProcessorPlayer& player;

    std::atomic<bool> injecting { false }, armed { false };
    std::atomic<uint32> startCount { 0 };
    std::atomic<int> lastBlockSize { 0 };
    std::atomic<double> currentRate { 0.0 };

    double periodNs = 0.0;
    uint64_t lastStartTicks = 0;
    uint64_t callbackCounter = 0;

    std::atomic<int64> callbacks { 0 }, late { 0 }, busyNs { 0 }, maxLoadPermille { 0 };

    std::vector<float> burst;
    juce::AudioBuffer<float> scratch;
    std::vector<const float*> scratchPointers;
    int burstPosition = -1;
    std::array<Injection, 512> injections {};
    std::atomic<size_t> injectionCount { 0 };
};

} // namespace mfx
