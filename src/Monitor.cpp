#include "Monitor.h"
#include "Product.h"
#include "Text.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <utility>

namespace mfx
{

const String MonitorDeviceType::aggregateNamePrefix = String (productName) + " monitor ";

bool isValidPair (const MonitorLayout& layout, int firstChannel)
{
    return firstChannel >= 0 && firstChannel + 1 < layout.monitorCount;
}

MonitorRouting planRouting (const BigInteger& wanted, const MonitorLayout& layout, int firstChannel)
{
    MonitorRouting routing;
    const auto hasPair = isValidPair (layout, firstChannel);
    const auto first = layout.monitorFirst + firstChannel;

    for (int i = 0; i < layout.mainCount; ++i)
    {
        const auto inner = layout.mainFirst + i;

        if (wanted[i] && ! (hasPair && (inner == first || inner == first + 1)))
        {
            routing.innerOutputs.setBit (inner);
            routing.visibleOutputs.setBit (i);
        }
    }

    if (hasPair)
    {
        routing.innerOutputs.setBit (first);
        routing.innerOutputs.setBit (first + 1);
        routing.monitorSlots[0] = routing.innerOutputs.getBitRange (0, first).countNumberOfSetBits();
        routing.monitorSlots[1] = routing.innerOutputs.getBitRange (0, first + 1).countNumberOfSetBits();
    }

    for (int i = routing.visibleOutputs.findNextSetBit (0); i >= 0; i = routing.visibleOutputs.findNextSetBit (i + 1))
        routing.mainSlots.push_back (routing.innerOutputs.getBitRange (0, layout.mainFirst + i).countNumberOfSetBits());

    return routing;
}

void copyToMonitor (float* const* output, const int* mainSlots, int numMain, const int* monitorSlots, int numSamples) noexcept
{
    for (int j = 0; j < 2; ++j)
    {
        if (monitorSlots[j] < 0)
            continue;

        auto* destination = output[monitorSlots[j]];

        if (numMain <= 0)
            FloatVectorOperations::clear (destination, numSamples);
        else
            FloatVectorOperations::copy (destination, output[mainSlots[jmin (j, numMain - 1)]], numSamples);
    }
}

//==============================================================================
namespace
{
    String pairText (int firstChannel)
    {
        return String (firstChannel + 1) + "/" + String (firstChannel + 2);
    }

    // A private aggregate of the main output (the clock) and the monitor device.
    class HalAggregate final : public MonitorBackend::Aggregate
    {
    public:
        HalAggregate (std::unique_ptr<hal::PrivateAggregate> aggregateIn, const String& nameIn, const String& uidIn, const MonitorLayout& layoutIn)
            : aggregate (std::move (aggregateIn)), name (nameIn), uid (uidIn), layout (layoutIn) {}

        String getName() const override             { return name; }
        MonitorLayout getLayout() const override    { return layout; }

        AudioObjectID getId() const noexcept        { return aggregate->getId(); }
        OSStatus destroy()                          { return aggregate->destroy(); }
        String getUid() const                       { return uid; }

    private:
        std::unique_ptr<hal::PrivateAggregate> aggregate;
        String name, uid;
        MonitorLayout layout;
    };

    // The same code runs in the app and in `--probe-monitor`.
    std::unique_ptr<HalAggregate> createHalAggregate (const String& mainName, const String& monitorName, String& error)
    {
        const auto main = hal::findDevice (mainName, false, true);
        const auto monitor = hal::findDevice (monitorName, false, true);

        if (main == kAudioObjectUnknown || monitor == kAudioObjectUnknown)
        {
            error = "'" + (main == kAudioObjectUnknown ? mainName : monitorName) + "' is not a (non-aggregate) output device";
            return nullptr;
        }

        if (main == monitor)
        {
            error = "both names are the same device";
            return nullptr;
        }

        // The HAL's device list follows a destroy a moment later. A new aggregate waits for the last one to be
        // gone and has its own name, so JUCE's scan never sees two of the same name (it would rename one).
        hal::PrivateAggregate::waitForLastDestroyed (200);

        static int counter = 0;
        const auto pid = String ((int) getpid());
        const auto name = MonitorDeviceType::aggregateNamePrefix + pid + "." + String (++counter);
        const auto uid = "local.nil." + String (productName).toLowerCase() + ".monitor." + pid + "." + String (counter) + "." + Uuid().toString();

        auto aggregate = hal::PrivateAggregate::create (name, uid, main, monitor, error);

        if (aggregate == nullptr)
            return nullptr;

        // The HAL fills the active sub-device list shortly after creation; JUCE numbers the channels from it.
        MonitorLayout layout;
        const auto mainChannels = hal::readNumbers (main).outputChannels, monitorChannels = hal::readNumbers (monitor).outputChannels;

        for (int attempt = 0; attempt < 10; ++attempt)
        {
            layout = {};

            for (const auto& sub : hal::outputChannelLayout (aggregate->getId()))
            {
                if (sub.id == main)       { layout.mainFirst = sub.firstChannel;    layout.mainCount = sub.numChannels; }
                if (sub.id == monitor)    { layout.monitorFirst = sub.firstChannel; layout.monitorCount = sub.numChannels; }
            }

            if (layout.mainCount == mainChannels && layout.monitorCount == monitorChannels)
                return std::make_unique<HalAggregate> (std::move (aggregate), name, uid, layout);

            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }

        error = "the aggregate's sub-devices do not show " + String (mainChannels) + " + " + String (monitorChannels) + " output channels";
        return nullptr;
    }

    class HalMonitorBackend final : public MonitorBackend
    {
    public:
        int countOutputChannels (const String& deviceName) override
        {
            const auto id = hal::findDevice (deviceName, false, true);
            return id != kAudioObjectUnknown ? hal::readNumbers (id).outputChannels : 0;
        }

        std::unique_ptr<Aggregate> createAggregate (const String& mainName, const String& monitorName, String& error) override
        {
            return createHalAggregate (mainName, monitorName, error);
        }
    };
}

std::unique_ptr<MonitorBackend> makeHalMonitorBackend()
{
    return std::make_unique<HalMonitorBackend>();
}

//==============================================================================
// Shows JUCE the main output only. The device underneath has the main channels and the monitor
// device's, and the callback copies the processed main output to the monitor pair on the way out.
class MonitorDevice final : public AudioIODevice,
                            private AudioIODeviceCallback
{
public:
    MonitorDevice (MonitorDeviceType& typeIn, const String& mainName, std::unique_ptr<AudioIODevice> innerIn,
                   std::unique_ptr<MonitorBackend::Aggregate> aggregateIn, const MonitorLayout& layoutIn, int firstChannelIn, bool sameDevice)
        : AudioIODevice (mainName, innerIn->getTypeName()), type (typeIn), inner (std::move (innerIn)),
          aggregate (std::move (aggregateIn)), layout (layoutIn), firstChannel (firstChannelIn), sharesDevice (sameDevice)
    {
        type.active = this;
    }

    ~MonitorDevice() override
    {
        stop();
        inner.reset();          // JUCE's own aggregate is built from ours, so ours goes last
        aggregate.reset();

        if (type.active == this)
            type.active = nullptr;
    }

    StringArray getOutputChannelNames() override
    {
        const auto innerNames = inner->getOutputChannelNames();
        const auto suffix = " (" + getName() + ")";
        StringArray names;

        for (int i = 0; i < layout.mainCount; ++i)
        {
            auto channel = innerNames[layout.mainFirst + i];

            if (channel.endsWith (suffix))
                channel = channel.dropLastCharacters (suffix.length());

            // On one device the pair cannot also carry the main output; it is marked so the list says why.
            if (sharesDevice && (i == layout.monitorFirst + firstChannel || i == layout.monitorFirst + firstChannel + 1))
                channel += ja (" [モニター]");

            names.add (channel);
        }

        return names;
    }

    StringArray getInputChannelNames() override        { return inner->getInputChannelNames(); }
    Array<double> getAvailableSampleRates() override   { return inner->getAvailableSampleRates(); }
    Array<int> getAvailableBufferSizes() override      { return inner->getAvailableBufferSizes(); }
    int getDefaultBufferSize() override                { return inner->getDefaultBufferSize(); }

    String open (const BigInteger& inputs, const BigInteger& outputs, double rate, int bufferSize) override
    {
        // On one device a channel carries one signal. Narrowing the user's channels here would be written
        // back as their choice by JUCE, so a pair that overlaps them is refused instead.
        const auto first = layout.monitorFirst + firstChannel;

        if (sharesDevice && (outputs[first] || outputs[first + 1]))
        {
            const auto error = ja ("モニターの組 ") + String (firstChannel + 1) + "/" + String (firstChannel + 2) + ja (" が、メイン出力のチャンネルと重なっています。メイン出力のチャンネルから外してください");
            type.noteOpenFailure (error);
            return error;
        }

        routing = planRouting (outputs, layout, firstChannel);
        expectedOutputs = routing.innerOutputs.countNumberOfSetBits();
        mainPointers.assign (routing.mainSlots.size(), nullptr);

        const auto error = inner->open (inputs, routing.innerOutputs, rate, bufferSize);

        if (error.isNotEmpty())
            type.noteOpenFailure (error);

        return error;
    }

    void close() override                              { inner->close(); }
    bool isOpen() override                             { return inner->isOpen(); }

    void start (AudioIODeviceCallback* callback) override
    {
        user = callback;
        inner->start (callback != nullptr ? this : nullptr);

        if (callback != nullptr)
        {
            // A failure noted earlier belongs to a device that is gone; this one is playing.
            if (inner->isPlaying())
                type.takeOpenFailure();
            else
                type.noteOpenFailure (inner->getLastError());
        }
    }

    void stop() override
    {
        inner->stop();          // flushes the callback; the stop notification still reaches `user`
        user = nullptr;
    }

    bool isPlaying() override                          { return inner->isPlaying(); }
    String getLastError() override                     { return inner->getLastError(); }
    int getCurrentBufferSizeSamples() override         { return inner->getCurrentBufferSizeSamples(); }
    double getCurrentSampleRate() override             { return inner->getCurrentSampleRate(); }
    int getCurrentBitDepth() override                  { return inner->getCurrentBitDepth(); }
    BigInteger getActiveOutputChannels() const override { return routing.visibleOutputs; }
    BigInteger getActiveInputChannels() const override { return inner->getActiveInputChannels(); }
    int getOutputLatencyInSamples() override           { return inner->getOutputLatencyInSamples(); }
    int getInputLatencyInSamples() override            { return inner->getInputLatencyInSamples(); }
    AudioWorkgroup getWorkgroup() const override       { return inner->getWorkgroup(); }
    int getXRunCount() const noexcept override         { return inner->getXRunCount(); }

private:
    void audioDeviceIOCallbackWithContext (const float* const* input, int numInputs, float* const* output, int numOutputs,
                                           int numSamples, const AudioIODeviceCallbackContext& context) override
    {
        auto* callback = user.load();

        if (callback == nullptr || numOutputs != expectedOutputs)
        {
            for (int c = 0; c < numOutputs; ++c)
                if (output[c] != nullptr)
                    FloatVectorOperations::clear (output[c], numSamples);

            return;
        }

        const auto numMain = (int) routing.mainSlots.size();

        for (int i = 0; i < numMain; ++i)
            mainPointers[(size_t) i] = output[routing.mainSlots[(size_t) i]];

        callback->audioDeviceIOCallbackWithContext (input, numInputs, mainPointers.data(), numMain, numSamples, context);
        copyToMonitor (output, routing.mainSlots.data(), numMain, routing.monitorSlots, numSamples);
    }

    void audioDeviceAboutToStart (AudioIODevice*) override
    {
        if (auto* callback = user.load())
            callback->audioDeviceAboutToStart (this);
    }

    void audioDeviceStopped() override
    {
        if (auto* callback = user.load())
            callback->audioDeviceStopped();
    }

    void audioDeviceError (const String& message) override
    {
        if (auto* callback = user.load())
            callback->audioDeviceError (message);
    }

    MonitorDeviceType& type;
    std::unique_ptr<AudioIODevice> inner;
    std::unique_ptr<MonitorBackend::Aggregate> aggregate;
    MonitorLayout layout;
    int firstChannel;
    bool sharesDevice;

    MonitorRouting routing;
    int expectedOutputs = 0;
    std::vector<float*> mainPointers;
    std::atomic<AudioIODeviceCallback*> user { nullptr };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonitorDevice)
};

//==============================================================================
MonitorDeviceType::MonitorDeviceType (std::unique_ptr<AudioIODeviceType> innerIn, std::unique_ptr<MonitorBackend> backendIn)
    : AudioIODeviceType (innerIn->getTypeName()), inner (std::move (innerIn)), backend (std::move (backendIn))
{
    inner->addListener (this);
}

MonitorDeviceType::~MonitorDeviceType()
{
    inner->removeListener (this);
}

void MonitorDeviceType::setPlan (const MonitorPlan& plan)
{
    wanted = plan;
    problem.clear();
    openFailure.clear();
    triedSnapshot.clear();
    cacheSnapshot.clear();
    waiting = false;
}

void MonitorDeviceType::noteOpenFailure (const String& error)
{
    openFailure = error.isNotEmpty() ? error : ja ("原因不明");
}

String MonitorDeviceType::takeOpenFailure()
{
    return std::exchange (openFailure, String());
}

void MonitorDeviceType::suspendMonitor (const String& reason)
{
    waiting = wanted.enabled;
    problem = reason;
}

String MonitorDeviceType::takeProblem()
{
    return std::exchange (problem, String());
}

String MonitorDeviceType::deviceSnapshot() const
{
    return getDeviceNames (false).joinIntoString ("\n") + "\n--\n" + getDeviceNames (true).joinIntoString ("\n");
}

bool MonitorDeviceType::isMonitorDevicePresent() const
{
    // Resolving walks the HAL, so it is redone only when the list has changed.
    const auto snapshot = deviceSnapshot();

    if (snapshot != cacheSnapshot || wanted.device != cacheDevice)
    {
        cacheSnapshot = snapshot;
        cacheDevice = wanted.device;
        cachePresent = inner->getDeviceNames (false).contains (wanted.device) && backend->countOutputChannels (wanted.device) > 0;
    }

    return cachePresent;
}

StringArray MonitorDeviceType::getUsableOutputNames() const
{
    StringArray names;

    for (auto& name : getDeviceNames (false))
        if (backend->countOutputChannels (name) > 0)
            names.add (name);

    return names;
}

bool MonitorDeviceType::consumeRetry()
{
    if (! waiting || active != nullptr)
        return false;

    const auto snapshot = deviceSnapshot();

    if (snapshot == triedSnapshot)
        return false;

    triedSnapshot = snapshot;
    return isMonitorDevicePresent();
}

void MonitorDeviceType::abandonMonitor (const String& reason)
{
    wanted.enabled = false;
    waiting = false;
    problem = reason;
}

void MonitorDeviceType::scanForDevices()
{
    inner->scanForDevices();
}

StringArray MonitorDeviceType::getDeviceNames (bool wantInputNames) const
{
    StringArray names;

    for (auto& name : inner->getDeviceNames (wantInputNames))
        if (! name.startsWith (aggregateNamePrefix))
            names.add (name);

    return names;
}

int MonitorDeviceType::getDefaultDeviceIndex (bool forInput) const
{
    const auto innerNames = inner->getDeviceNames (forInput);
    const auto index = inner->getDefaultDeviceIndex (forInput);
    return isPositiveAndBelow (index, innerNames.size()) ? jmax (0, getDeviceNames (forInput).indexOf (innerNames[index])) : 0;
}

int MonitorDeviceType::getIndexOfDevice (AudioIODevice* device, bool asInput) const
{
    if (device == nullptr)
        return -1;

    if (dynamic_cast<MonitorDevice*> (device) != nullptr)
        return getDeviceNames (asInput).indexOf (device->getName());

    const auto index = inner->getIndexOfDevice (device, asInput);
    const auto innerNames = inner->getDeviceNames (asInput);
    return isPositiveAndBelow (index, innerNames.size()) ? getDeviceNames (asInput).indexOf (innerNames[index]) : -1;
}

bool MonitorDeviceType::hasSeparateInputsAndOutputs() const
{
    return inner->hasSeparateInputsAndOutputs();
}

void MonitorDeviceType::audioDeviceListChanged()
{
    callDeviceChangeListeners();
}

AudioIODevice* MonitorDeviceType::createDevice (const String& outputDeviceName, const String& inputDeviceName)
{
    if (wanted.enabled && outputDeviceName.isNotEmpty())
        return createWithMonitor (outputDeviceName, inputDeviceName);

    return inner->createDevice (outputDeviceName, inputDeviceName);
}

AudioIODevice* MonitorDeviceType::fail (const String& reason, bool temporary, const String& outputDeviceName, const String& inputDeviceName)
{
    if (temporary)
    {
        waiting = true;
        triedSnapshot = deviceSnapshot();
        problem = ja ("モニターは待機中です（") + reason + ja ("）。メイン出力だけで続けます。機器が現れたら自動で試します");
    }
    else
    {
        waiting = false;
        wanted.enabled = false;
        problem = ja ("モニターを使えません（") + reason + ja ("）。メイン出力だけで続けます");
    }

    return inner->createDevice (outputDeviceName, inputDeviceName);
}

AudioIODevice* MonitorDeviceType::createWithMonitor (const String& outputDeviceName, const String& inputDeviceName)
{
    const auto listed = inner->getDeviceNames (false).contains (wanted.device);
    const auto channels = backend->countOutputChannels (wanted.device);

    if (! listed)
        return fail (ja ("モニター機器「") + wanted.device + ja ("」が見つかりません"), true, outputDeviceName, inputDeviceName);

    // Listed, but not a device that can be combined (a duplicate name, an aggregate): waiting would never end.
    if (channels == 0)
        return fail (ja ("モニター機器「") + wanted.device + ja ("」を扱えません（同じ名前の機器が複数ある、合成デバイスである、出力がない、など）"), false, outputDeviceName, inputDeviceName);

    if (wanted.firstChannel < 0 || wanted.firstChannel + 1 >= channels)
        return fail (ja ("モニター機器「") + wanted.device + ja ("」にチャンネル ") + pairText (wanted.firstChannel)
                         + ja (" がありません（出力は ") + String (channels) + " ch）", false, outputDeviceName, inputDeviceName);

    std::unique_ptr<MonitorBackend::Aggregate> aggregate;
    MonitorLayout layout { 0, channels, 0, channels };
    auto nameToOpen = outputDeviceName;
    const auto sameDevice = wanted.device == outputDeviceName;

    // On one device there is nothing to combine: the pair is just two more channels of the main output.
    if (! sameDevice)
    {
        String error;
        aggregate = backend->createAggregate (outputDeviceName, wanted.device, error);

        if (aggregate == nullptr)
            return fail (ja ("モニター用の合成デバイスを作れませんでした: ") + error, false, outputDeviceName, inputDeviceName);

        layout = aggregate->getLayout();

        if (layout.mainCount <= 0 || ! isValidPair (layout, wanted.firstChannel))
            return fail (ja ("合成デバイスのチャンネル配置が想定と違います"), false, outputDeviceName, inputDeviceName);

        // The device list was scanned before the aggregate existed. Opening by a name the list does not
        // hold would give JUCE an input-only device, so a missing name is a failure here.
        nameToOpen = aggregate->getName();
        inner->scanForDevices();

        if (! inner->getDeviceNames (false).contains (nameToOpen))
            return fail (ja ("JUCE のデバイス一覧に合成デバイスが現れませんでした"), false, outputDeviceName, inputDeviceName);
    }

    std::unique_ptr<AudioIODevice> device (inner->createDevice (nameToOpen, inputDeviceName));

    if (device == nullptr)
        return fail (ja ("合成デバイスを開けませんでした"), false, outputDeviceName, inputDeviceName);

    problem.clear();
    waiting = false;
    return new MonitorDevice (*this, outputDeviceName, std::move (device), std::move (aggregate), layout, wanted.firstChannel, sameDevice);
}

//==============================================================================
int runProbeMonitor (const String& mainName, const String& monitorName)
{
    const auto say = [] (const String& text) { std::cout << text.toRawUTF8() << std::endl; };

    struct Involved { String role; AudioObjectID id; double before = 0.0; };
    std::vector<Involved> devices { { "main output", hal::findDevice (mainName, false, true) },
                                    { "monitor", hal::findDevice (monitorName, false, true) } };

    for (auto& d : devices)
    {
        if (d.id == kAudioObjectUnknown)
        {
            say ("FAIL: " + d.role + " device '" + (d.role == "monitor" ? monitorName : mainName) + "' is not an output device");
            return 2;
        }
    }

    if (devices[0].id == devices[1].id)
    {
        say ("the main output and the monitor are the same device: the app creates no aggregate for that, the pair is two more channels of the device");
        return 0;
    }

    const auto printRates = [&] (const String& when, bool remember)
    {
        bool unchanged = true;

        for (auto& d : devices)
        {
            const auto rate = hal::readNumbers (d.id).nominalRate;

            if (remember)
                d.before = rate;

            const auto same = std::abs (rate - d.before) < 0.5;
            unchanged = unchanged && same;
            say (d.role + " '" + hal::nameOf (d.id) + "' nominal rate " + when + ": " + String (rate, 0) + " Hz"
                 + (remember || same ? String() : " (CHANGED, was " + String (d.before, 0) + " Hz)"));
        }

        return unchanged;
    };

    for (auto& d : devices)
    {
        const auto n = hal::readNumbers (d.id);
        say (d.role + " '" + hal::nameOf (d.id) + "' uid " + hal::uidOf (d.id) + ": " + String (n.inputChannels) + " in, " + String (n.outputChannels) + " out");
    }

    printRates ("before", true);

    // JUCE's CoreAudio type is only asked what it lists; nothing is opened.
    std::unique_ptr<AudioIODeviceType> juceType (AudioIODeviceType::createAudioIODeviceType_CoreAudio());
    String error;
    auto aggregate = createHalAggregate (mainName, monitorName, error);

    if (aggregate == nullptr)
    {
        say ("FAIL: the aggregate could not be created: " + error);
        return 3;
    }

    const auto id = aggregate->getId();
    say ("created private aggregate '" + aggregate->getName() + "' uid " + aggregate->getUid() + " (object " + String ((int) id) + ")");
    say ("  clock device uid " + hal::clockUidOf (id) + " (main output uid " + hal::uidOf (devices[0].id) + ")");

    const auto all = hal::allDevices();
    const auto visible = std::find (all.begin(), all.end(), id) != all.end();
    say (String ("  listed in this process's HAL device list: ") + (visible ? "yes" : "NO"));

    bool listedByJuce = false;

    if (juceType != nullptr)
    {
        juceType->scanForDevices();
        listedByJuce = juceType->getDeviceNames (false).contains (aggregate->getName());
    }

    say (String ("  listed by JUCE's CoreAudio device scan (so AudioDeviceManager can open it by name): ") + (listedByJuce ? "yes" : "NO"));

    int channel = 0;

    for (const auto& sub : hal::outputChannelLayout (id))
    {
        const auto name = hal::nameOf (sub.id);
        const auto role = sub.id == devices[0].id ? "main output" : sub.id == devices[1].id ? "monitor" : "other";
        say ("  sub-device '" + name + "' (" + role + "): output channels " + String (sub.firstChannel + 1) + " to " + String (sub.firstChannel + sub.numChannels)
             + " of the aggregate (" + String (sub.numChannels) + " ch)");

        for (int c = 0; c < sub.numChannels; ++c, ++channel)
            say ("    aggregate output " + String (channel + 1) + " -> " + name + " channel " + String (c + 1));
    }

    const auto layout = aggregate->getLayout();
    say ("  layout the app uses: main " + String (layout.mainFirst + 1) + ".." + String (layout.mainFirst + layout.mainCount)
         + ", monitor " + String (layout.monitorFirst + 1) + ".." + String (layout.monitorFirst + layout.monitorCount));

    const auto unchangedWhileAlive = printRates ("with the aggregate", false);

    juceType.reset();
    const auto destroyStatus = aggregate->destroy();
    aggregate.reset();
    say ("destroyed the aggregate: OSStatus " + String ((int) destroyStatus));

    const auto unchangedAfter = printRates ("right after destroying it", false);

    // The HAL updates its device list after the call returns, so the list is polled before the aggregate is called a leftover.
    bool stillThere = true;
    int waitedMs = 0;

    for (; waitedMs <= 1000; waitedMs += 20)
    {
        const auto remaining = hal::allDevices();
        stillThere = std::find (remaining.begin(), remaining.end(), id) != remaining.end();

        if (! stillThere)
            break;

        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }

    say (String ("the aggregate is gone from the device list: ") + (stillThere ? "NO (after 1 s)" : "yes (after " + String (waitedMs) + " ms)"));

    // A change that comes after the destroy (a sub-device put back by the HAL) would be missed by the reads above.
    const auto unchangedLater = printRates ("after waiting", false);

    if (! visible || ! listedByJuce || stillThere)
    {
        say ("PROBE FAILED: the aggregate cannot be used the way the app needs");
        return 5;
    }

    if (! unchangedWhileAlive || ! unchangedAfter || ! unchangedLater || destroyStatus != noErr)
    {
        say ("PROBE: a nominal rate changed or the destroy failed (this code writes no rate; the HAL did)");
        return 4;
    }

    say ("PROBE OK: no rate changed");
    return 0;
}

} // namespace mfx
