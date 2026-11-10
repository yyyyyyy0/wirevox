#pragma once

#include "Chain.h"
#include "Hal.h"
#include "HostCallback.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <optional>

namespace mfx
{
using namespace juce;

// Switches the measurement sweep flips one at a time. An unset value means the device's own default.
struct Experiments
{
    std::optional<float> ioCycleUsage;
};

// Everything between the settings file and the sound: plugin formats, the chain, the device manager,
// the callback that times and feeds the graph, and the HAL listeners that count overloads. The window
// and the command-line modes both build one of these.
class Engine
{
public:
    Engine();
    ~Engine();

    // `deviceSetup` is the DEVICESETUP element of the settings, or null to keep every device closed
    // (a mic wired straight to the speakers on first launch would feed back).
    void openDevices (const XmlElement* deviceSetup);

    // Connects the chain to the device manager. Call once, after the chain has been restored.
    void start();

    AudioPluginFormatManager formats;
    Chain chain { formats };
    AudioDeviceManager deviceManager;
    AudioProcessorPlayer player;
    HostCallback callback { player };

    //==============================================================================
    // Message thread. Finds the private aggregate device once per device start and listens on it
    // and its sub-devices; also re-applies the experiment switches to it.
    void refreshHal();

    AudioObjectID getAggregate() const noexcept     { return aggregate; }
    int getOverloads() const noexcept               { return overloads != nullptr ? overloads->getCount() : 0; }
    void resetOverloads() noexcept                  { if (overloads != nullptr) overloads->reset(); }
    hal::Numbers getAggregateNumbers() const        { return aggregate != kAudioObjectUnknown ? hal::readNumbers (aggregate) : hal::Numbers {}; }

    // Nominal rate of the input device as the hardware itself runs it (0 when unknown).
    double getInputHardwareRate() const noexcept    { return inputHardwareRate; }

    void setExperiments (const Experiments&);
    const Experiments& getExperiments() const noexcept { return experiments; }

    // Reopens the device with this buffer size (JUCE rounds it to a power of two). Returns the size the
    // callback actually receives, or 0 when the device does not open.
    int setBufferSize (int frames);

    // Nominal sample rate of a named device straight from the HAL (0 when it does not exist).
    static double nominalRateOf (const String& deviceName, bool input);

private:
    void applyExperiments();

    AudioObjectID aggregate = kAudioObjectUnknown;
    std::unique_ptr<hal::OverloadListener> overloads;
    uint32 seenStartCount = 0;
    double inputHardwareRate = 0.0;
    Experiments experiments;
    bool started = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

// Puts the device setup, the experiment switches and the chain's forced bypass back as they were when
// it was created, however the scope is left. The sweep wraps its changes in one.
class ScopedSetup
{
public:
    explicit ScopedSetup (Engine&);
    ~ScopedSetup();

private:
    Engine& engine;
    std::unique_ptr<XmlElement> saved;
    Experiments experiments;
};

} // namespace mfx
