#pragma once

#include "Breakdown.h"
#include "Chain.h"
#include "Hal.h"
#include "HostCallback.h"
#include "Monitor.h"

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
String describeRateMismatch (double runningRate, double inputRate, double outputRate, double monitorRate = 0.0);

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
    // The monitor layer (Monitor.h) goes in front of the CoreAudio device type right here, before anything
    // can ask the device manager for its types: a window's device panel does, and would otherwise make
    // JUCE add a plain CoreAudio type that the layer cannot get in front of. With the monitor off, which
    // is how an engine starts, the layer hands out CoreAudio's own devices untouched. Tests pass false and
    // give a fake layer to useDeviceType().
    explicit Engine (bool useCoreAudio = true);
    ~Engine();

    // `deviceSetup` is the DEVICESETUP element of the settings, or null to keep every device closed
    // (a mic wired straight to the speakers on first launch would feed back).
    void openDevices (const XmlElement* deviceSetup);

    // For tests, on an Engine made with `false`: the device layer to put the monitor layer in front of,
    // instead of CoreAudio. Call once, before anything opens.
    void useDeviceType (std::unique_ptr<AudioIODeviceType> inner, std::unique_ptr<MonitorBackend> backend);

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

    // The aggregate's numbers. With the monitor on, the aggregate also holds the monitor device, whose
    // own latency and safety offset would raise the aggregate's output side; the output side here is
    // always the main output device's own, so the breakdown keeps describing the main path.
    hal::Numbers getAggregateNumbers() const;

    // The monitor device's own numbers; empty when the monitor is not running.
    hal::Numbers getMonitorNumbers() const;

    // Nominal rates of the two devices as they were right after the device was opened (0 when unknown).
    // JUCE writes its rate into the devices it opens, so the output device (a virtual one in particular)
    // usually follows the app, while an input interface that cannot run at the app's rate keeps its own
    // and the aggregate converts.
    // `monitor` is the monitor device's, only while the monitor runs.
    struct DeviceRates { double input = 0.0, output = 0.0, monitor = 0.0; };
    DeviceRates getDeviceRates() const noexcept     { return deviceRates; }

    void setExperiments (const Experiments&);
    const Experiments& getExperiments() const noexcept { return experiments; }

    // The IO cycle usage the aggregate runs at now: read from the device when it can be, otherwise the
    // experiment's value, otherwise the device's default of 1.0.
    double getCycleUsage() const;

    //==============================================================================
    // What the output device adds beyond what it declares, as last measured (Measure.h). The record is
    // only ever returned for the output device and the rate that are open now: it is dropped the first
    // time refreshHal() sees another device or rate, and never written for one.
    void setExcess (const ExcessRecord&);
    std::optional<ExcessRecord> getExcess() const;

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

    //==============================================================================
    // The monitor output: the processed voice also on a second device. The plan is the user's choice and
    // is what the settings file holds; whether it is running is a separate question (isMonitorActive),
    // because it is left off, with a reason, when the devices cannot be combined. All on the message thread.
    MonitorPlan getMonitorPlan() const;
    bool isMonitorActive() const;

    // Applies a plan. An open device is reopened when what it carries changes; nothing at all is created
    // or opened for a plan that is off. Afterwards getMonitorPlan() says what took effect.
    // `reopen` false only for a caller that opens the device itself right after (ScopedSetup).
    void setMonitorPlan (const MonitorPlan&, bool reopen = true);

    // The reason the monitor was left off, once; empty when there is none.
    String takeMonitorProblem();

    // Stops using the monitor while its device is away, keeping the choice. True when it did.
    bool checkMonitor();

    // Tries a waiting monitor again, for when the device list has changed. True when the device was reopened.
    bool retryMonitor();

    // All of the above in the order they matter, for the places that cannot tell which one is due: after a
    // device list change, when a MIN LATENCY run or a measurement ends, and on the timer. True when it
    // changed anything.
    bool serviceMonitor();

    // For a device that JUCE reopened by itself (a device list change) and that failed to open with the
    // monitor in use: turns the monitor off with the reason and opens the main output alone. True when it did.
    bool recoverMonitorFailure();

    StringArray getMonitorCandidates() const;
    int countMonitorChannels (const String& deviceName) const;

private:
    void applyExperiments();
    void reopenDevice();
    bool fallBackFromOpenFailure();
    bool monitorNeedsReopen() const;

    AudioObjectID aggregate = kAudioObjectUnknown;
    std::unique_ptr<hal::OverloadListener> overloads;
    uint32 seenStartCount = 0;
    DeviceRates deviceRates;
    Experiments experiments;
    bool started = false;
    MonitorDeviceType* monitorType = nullptr;       // owned by deviceManager
    AudioObjectID mainOutput = kAudioObjectUnknown, monitorOutput = kAudioObjectUnknown;     // while the monitor runs

    std::optional<ExcessRecord> excess;

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
    MonitorPlan monitor;
};

} // namespace mfx
