#pragma once

#include "Hal.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <memory>
#include <vector>

namespace mfx
{
using namespace juce;

// What the user chose for the monitor output: the processed voice on a second device, next to the main
// output. `firstChannel` is the zero-based first channel of the stereo pair on `device` (31/32 is 30).
// The plan is kept even while it is off, so the choice survives a device that is not there for a while.
struct MonitorPlan
{
    bool enabled = false;
    String device;
    int firstChannel = 0;

    bool operator== (const MonitorPlan& other) const noexcept { return enabled == other.enabled && device == other.device && firstChannel == other.firstChannel; }
    bool operator!= (const MonitorPlan& other) const noexcept { return ! (*this == other); }
};

// Where the main output's channels and the monitor device's channels sit in the channel numbering of the
// device JUCE opens (the aggregate, or the main device itself when the monitor is on the same device).
struct MonitorLayout
{
    int mainFirst = 0, mainCount = 0;
    int monitorFirst = 0, monitorCount = 0;
};

// True when `firstChannel` and the one after it are both channels of the monitor device.
bool isValidPair (const MonitorLayout&, int firstChannel);

//==============================================================================
// What the monitor layer asks of the system, so the self tests can run it without a HAL.
class MonitorBackend
{
public:
    // A device pair that is open to JUCE as one output device. Destroying it removes it.
    class Aggregate
    {
    public:
        virtual ~Aggregate() = default;
        virtual String getName() const = 0;                 // the name the CoreAudio device type lists it under
        virtual MonitorLayout getLayout() const = 0;
    };

    virtual ~MonitorBackend() = default;

    // Output channels of a device by name; 0 when there is no such device or it has no outputs.
    virtual int countOutputChannels (const String& deviceName) = 0;

    // Null with `error` set (English, for the log of the probe) when it cannot be made.
    virtual std::unique_ptr<Aggregate> createAggregate (const String& mainName, const String& monitorName, String& error) = 0;
};

// The HAL's: a private aggregate of the two devices, the main output as the clock.
std::unique_ptr<MonitorBackend> makeHalMonitorBackend();

//==============================================================================
// The channels the device is really opened with, and where the callback finds them. The user's callback
// only ever sees `visibleOutputs` (the main output's channels); the monitor pair is added underneath.
struct MonitorRouting
{
    BigInteger innerOutputs;            // what the underlying device is opened with
    BigInteger visibleOutputs;          // the main channels that are really active
    std::vector<int> mainSlots;         // index in the callback's output array of each visible channel
    int monitorSlots[2] = { -1, -1 };   // index of the monitor pair in that array
};

// `wanted` is the main channel selection as JUCE numbers it (0 is the main device's first channel).
// A main channel that is also one of the monitor pair is dropped: a channel carries one signal.
MonitorRouting planRouting (const BigInteger& wanted, const MonitorLayout&, int firstChannel);

// The monitor pair gets whatever the main output carries after processing: channel 0 and 1 of it (channel
// 0 twice for a mono main output), silence when the main output has no channel. No allocation, no lock.
void copyToMonitor (float* const* output, const int* mainSlots, int numMain, const int* monitorSlots, int numSamples) noexcept;

//==============================================================================
class MonitorDevice;

// Stands in for the CoreAudio device type. While the monitor is off it hands out the underlying
// device itself, untouched. While it is on it opens the main output together with the monitor device as
// one aggregate, and hands out a MonitorDevice that shows JUCE only the main output: the device name,
// the channel list and the active channels are the main device's, so the device panel, the saved
// DEVICESETUP and everything else keyed by the output device never see the aggregate.
class MonitorDeviceType final : public AudioIODeviceType,
                                private AudioIODeviceType::Listener
{
public:
    MonitorDeviceType (std::unique_ptr<AudioIODeviceType> inner, std::unique_ptr<MonitorBackend> backend);
    ~MonitorDeviceType() override;

    //==============================================================================
    // Message thread. The plan takes effect the next time a device is created.
    void setPlan (const MonitorPlan& plan);
    MonitorPlan getPlan() const noexcept        { return wanted; }
    bool isActive() const noexcept              { return active != nullptr; }

    // The reason the monitor was left off, once; empty when there is none.
    String takeProblem();

    // True when the monitor device is in the device list and the backend can resolve it to a device it can
    // combine. The one test for "is it there", used to start, to retry and to notice that it is gone.
    bool isMonitorDevicePresent() const;

    // The input and output device names as the list shows them, without the aggregate. It only changes
    // when a device comes or goes, which is what a retry waits for.
    String deviceSnapshot() const;

    // The output devices of the list that the backend can resolve: the ones the monitor can use.
    StringArray getUsableOutputNames() const;

    // For a waiting monitor: true when the device list changed since the last look and the device is there
    // now. Each list is looked at once, so a try that ends in waiting again is not repeated until it changes.
    bool consumeRetry();

    // Turns the choice off with a reason: it cannot work as chosen (the channels, the device refused to open).
    void abandonMonitor (const String& reason);

    // Keeps the choice but stops using it for now, with a reason: a device that is not there at the moment.
    // The monitor is tried again when the device list changes (Engine::retryMonitor).
    void suspendMonitor (const String& reason);
    bool isWaiting() const noexcept             { return waiting; }

    // The error of the last open that failed while the monitor was in use, once; empty when there is none.
    // A device that failed to open is deleted by JUCE, so this is how the engine learns it has to fall back.
    void noteOpenFailure (const String& error);
    String takeOpenFailure();

    int countOutputChannels (const String& deviceName) const    { return backend->countOutputChannels (deviceName); }

    //==============================================================================
    void scanForDevices() override;
    StringArray getDeviceNames (bool wantInputNames) const override;
    int getDefaultDeviceIndex (bool forInput) const override;
    int getIndexOfDevice (AudioIODevice*, bool asInput) const override;
    bool hasSeparateInputsAndOutputs() const override;
    AudioIODevice* createDevice (const String& outputDeviceName, const String& inputDeviceName) override;

    // The prefix of the aggregate's name. The device list never shows a name that starts with it.
    static const String aggregateNamePrefix;

private:
    friend class MonitorDevice;

    void audioDeviceListChanged() override;
    AudioIODevice* createWithMonitor (const String& outputDeviceName, const String& inputDeviceName);
    // `temporary`: a device is missing, so the choice stays and the monitor waits for it. Otherwise the
    // choice cannot work and is turned off.
    AudioIODevice* fail (const String& reason, bool temporary, const String& outputDeviceName, const String& inputDeviceName);

    std::unique_ptr<AudioIODeviceType> inner;
    std::unique_ptr<MonitorBackend> backend;
    MonitorPlan wanted;
    String problem, openFailure, triedSnapshot;
    mutable String cacheSnapshot, cacheDevice;
    mutable bool cachePresent = false;
    bool waiting = false;
    MonitorDevice* active = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonitorDeviceType)
};

//==============================================================================
// `--probe-monitor "<main output>" "<monitor device>"`: makes the aggregate exactly as the app would,
// prints the nominal rate of every device involved before and after, the aggregate's channel layout and
// whether JUCE's device scan lists it, then destroys it. No IO is started and no rate is written by this
// code. Returns 0 when everything held, otherwise a nonzero code.
int runProbeMonitor (const String& mainName, const String& monitorName);

} // namespace mfx
