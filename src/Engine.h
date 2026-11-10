#pragma once

#include "Chain.h"
#include "Hal.h"
#include "HostCallback.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <optional>

namespace mfx
{
using namespace juce;

// Switches the measurement sweep and MIN LATENCY change. An unset value means the device's own default.
struct Experiments
{
    std::optional<float> ioCycleUsage;
};

// Text for the status line when a device's nominal rate is not the rate the app runs at, which means
// the system converts between the two; empty when all known rates agree. A rate of 0 is unknown.
String describeRateMismatch (double runningRate, double inputRate, double outputRate);

// The IO cycle usage is a fraction in (0, 1]. NaN, zero, negative and larger values (from a damaged
// settings file or a mistyped option) are rejected before they can reach the HAL.
std::optional<float> validCycleUsage (double);

// The audio part of the settings file: the chosen device setup and the cycle usage.
struct AudioSettings
{
    std::unique_ptr<XmlElement> device;
    std::optional<float> cycleUsage;
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

    // The same from a whole settings file: its DEVICESETUP and its cycleUsage attribute. Null keeps every device closed.
    void openFromSettings (const XmlElement* settingsRoot);

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

    // Nominal rates of the two devices as they were right after the device was opened (0 when unknown).
    // JUCE writes its rate into the devices it opens, so the output device (a virtual one in particular)
    // usually follows the app, while an input interface that cannot run at the app's rate keeps its own
    // and the aggregate converts.
    struct DeviceRates { double input = 0.0, output = 0.0; };
    DeviceRates getDeviceRates() const noexcept     { return deviceRates; }

    void setExperiments (const Experiments&);
    const Experiments& getExperiments() const noexcept { return experiments; }

    // Reopens the device with this buffer size (JUCE rounds it to a power of two). Returns the size the
    // callback actually receives, or 0 when the device does not open. JUCE's own record of the chosen
    // setup is not touched, so this is a trial until the caller persists it (see getPersistedAudio).
    int setBufferSize (int frames);

    // The same for a whole setup, which also reopens a device that a failed step left closed (JUCE
    // forgets the device names then, so the caller has to keep the setup it wants back).
    // `recordAsChosen` also makes it the setup JUCE reopens with after a device list change.
    int openWithSetup (const AudioDeviceManager::AudioDeviceSetup&, bool recordAsChosen = false);

    //==============================================================================
    // What the settings file should hold for the audio side: the chosen device setup with the buffer
    // size that is actually running, and the cycle usage in effect. (JUCE only records a buffer size
    // that was chosen through its selector; one set by setBufferSize would otherwise be lost.)
    // While a hold is active these stay at what they were when it began, so a trial that is cut short
    // can never be written down.
    AudioSettings getPersistedAudio() const;
    void holdPersistedAudio();
    void releasePersistedAudio();
    bool isPersistedAudioHeld() const noexcept { return held.has_value(); }

    // Nominal sample rate of a named device straight from the HAL (0 when it does not exist).
    static double nominalRateOf (const String& deviceName, bool input);

private:
    void applyExperiments();

    AudioObjectID aggregate = kAudioObjectUnknown;
    std::unique_ptr<hal::OverloadListener> overloads;
    uint32 seenStartCount = 0;
    DeviceRates deviceRates;
    Experiments experiments;
    bool started = false;

    struct Held { std::unique_ptr<XmlElement> device; std::optional<float> cycleUsage; };
    std::optional<Held> held;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

// Puts the device setup, the experiment switches and the chain's forced bypass back as they were when
// it was created, however the scope is left. The measurement commands wrap their changes in one.
class ScopedSetup
{
public:
    explicit ScopedSetup (Engine&);
    ~ScopedSetup();

private:
    Engine& engine;
    AudioDeviceManager::AudioDeviceSetup saved;
    bool hadDevice = false;
    Experiments experiments;
};

} // namespace mfx
