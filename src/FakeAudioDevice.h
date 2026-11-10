#pragma once

#include "Monitor.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <map>
#include <set>
#include <vector>

// A device layer without hardware, for the self tests. Handed to an AudioDeviceManager through
// addAudioDeviceType() before it scans, it replaces CoreAudio, so the engine, the tuner and the
// settings can be run through real reopen / failure / restore paths with nothing opened on the Mac.
namespace mfx::test
{
using namespace juce;

class FakeDevice;

struct FakeWorld
{
    Array<int> bufferSizes { 16, 32, 64, 128, 256, 512, 1024 };
    std::set<int> refusedBuffers;       // open() fails for these sizes; JUCE then drops the device
    int opens = 0;                      // successful opens
    Array<int> openedBuffers;           // the buffer size of each of them

    // The output devices the type lists, with their channel counts (2 when a name has no entry). The
    // aggregates the fake backend makes are added here for as long as they exist, like the HAL's.
    StringArray outputNames { "FakeOut" };
    std::map<String, int> outputChannels;
    std::map<String, StringArray> channelNames;     // set for aggregates, the way JUCE names a sub-device's channels
    bool aggregatesListed = true;                   // false: the device scan does not list the aggregate
    bool failAggregate = false;                     // createAggregate refuses
    bool monitorBeforeMain = false;                 // the aggregate puts the monitor device's channels first
    int aggregatesCreated = 0, aggregatesAlive = 0;
    String lastAggregateName;
    int maxOpenOutputs = 0;                         // open() fails for more output channels than this (0: no limit)
    int refuseExtraOutputsBelow = 0;                // open() fails for more than 2 output channels at buffers below this
    bool delayedRemoval = false;                    // a destroyed aggregate stays in the list until the next one is made
    StringArray pendingRemoval;
    int settledRemovals = 0;                        // pending removals that createAggregate waited for
    std::set<String> unresolvable;                  // listed, but the backend cannot resolve them (duplicate names, aggregates)
    int devicesCreated = 0;
    int devicesAlive = 0;                           // devices created and not yet destroyed
    FakeDevice* lastDevice = nullptr;         // the newest one that is alive: tests drive its callback
};

class FakeDevice final : public AudioIODevice
{
public:
    FakeDevice (FakeWorld& worldIn, const String& output, const String& input)
        : AudioIODevice (output.isNotEmpty() ? output : input, "Fake"), world (worldIn), outputName (output)
    {
        ++world.devicesAlive;
        ++world.devicesCreated;
        world.lastDevice = this;
    }

    ~FakeDevice() override
    {
        --world.devicesAlive;

        if (world.lastDevice == this)
            world.lastDevice = nullptr;
    }

    StringArray getOutputChannelNames() override
    {
        if (const auto names = world.channelNames.find (outputName); names != world.channelNames.end())
            return names->second;

        const auto count = world.outputChannels.count (outputName) > 0 ? world.outputChannels[outputName] : 2;

        if (count == 2)
            return { "out L", "out R" };

        StringArray names;

        for (int i = 0; i < count; ++i)
            names.add ("out " + String (i + 1));

        return names;
    }

    StringArray getInputChannelNames() override         { return { "in" }; }
    Array<double> getAvailableSampleRates() override    { return { 44100.0, 48000.0 }; }
    Array<int> getAvailableBufferSizes() override       { return world.bufferSizes; }
    int getDefaultBufferSize() override                 { return 128; }

    String open (const BigInteger& inputs, const BigInteger& outputs, double rate, int bufferSize) override
    {
        if (world.refusedBuffers.count (bufferSize) > 0)
        {
            lastError = "refused buffer size " + String (bufferSize);
            return lastError;
        }

        if (world.maxOpenOutputs > 0 && outputs.countNumberOfSetBits() > world.maxOpenOutputs)
        {
            lastError = "too many output channels: " + String (outputs.countNumberOfSetBits());
            return lastError;
        }

        if (world.refuseExtraOutputsBelow > 0 && outputs.countNumberOfSetBits() > 2 && bufferSize < world.refuseExtraOutputsBelow)
        {
            lastError = "the aggregate cannot run at buffer size " + String (bufferSize);
            return lastError;
        }

        inputChannels = inputs;
        outputChannels = outputs;
        currentRate = rate;
        currentBuffer = bufferSize;
        opened = true;
        ++world.opens;
        world.openedBuffers.add (bufferSize);
        return {};
    }

    void close() override                               { opened = false; }
    bool isOpen() override                              { return opened; }

    void start (AudioIODeviceCallback* newCallback) override
    {
        callback = newCallback;
        playing = true;

        if (callback != nullptr)
            callback->audioDeviceAboutToStart (this);
    }

    void stop() override
    {
        if (playing && callback != nullptr)
            callback->audioDeviceStopped();

        playing = false;
        callback = nullptr;
    }

    bool isPlaying() override                           { return playing; }
    String getLastError() override                      { return lastError; }
    int getCurrentBufferSizeSamples() override          { return currentBuffer; }
    double getCurrentSampleRate() override              { return currentRate; }
    int getCurrentBitDepth() override                   { return 32; }
    BigInteger getActiveOutputChannels() const override { return outputChannels; }
    BigInteger getActiveInputChannels() const override  { return inputChannels; }
    int getOutputLatencyInSamples() override            { return currentBuffer; }
    int getInputLatencyInSamples() override             { return currentBuffer; }

    // One cycle the way a device runs it: the active channels compacted into one array each, the
    // microphone a constant `micLevel`, the output pre-filled with a marker that no processing produces.
    // Returns what the callback left in each active output channel, in order.
    std::vector<std::vector<float>> pump (int numSamples, float micLevel)
    {
        const auto numIn = inputChannels.countNumberOfSetBits(), numOut = outputChannels.countNumberOfSetBits();
        std::vector<std::vector<float>> in ((size_t) numIn, std::vector<float> ((size_t) numSamples, micLevel));
        std::vector<std::vector<float>> out ((size_t) numOut, std::vector<float> ((size_t) numSamples, 7.0f));
        std::vector<const float*> inPointers;
        std::vector<float*> outPointers;

        for (auto& channel : in)    inPointers.push_back (channel.data());
        for (auto& channel : out)   outPointers.push_back (channel.data());

        if (callback != nullptr)
            callback->audioDeviceIOCallbackWithContext (inPointers.data(), numIn, outPointers.data(), numOut, numSamples, {});

        return out;
    }

private:
    FakeWorld& world;
    String outputName;
    BigInteger inputChannels, outputChannels;
    double currentRate = 0.0;
    int currentBuffer = 0;
    bool opened = false, playing = false;
    AudioIODeviceCallback* callback = nullptr;
    String lastError;
};

class FakeDeviceType final : public AudioIODeviceType
{
public:
    explicit FakeDeviceType (FakeWorld& worldIn) : AudioIODeviceType ("Fake"), world (worldIn) {}

    void scanForDevices() override {}
    StringArray getDeviceNames (bool wantInputNames) const override
    {
        if (wantInputNames)
            return { "FakeIn", "FakeIn2" };

        StringArray names;

        for (auto& name : world.outputNames)
            if (world.aggregatesListed || ! name.startsWith (MonitorDeviceType::aggregateNamePrefix))
                names.add (name);

        return names;
    }
    int getDefaultDeviceIndex (bool) const override                     { return 0; }
    int getIndexOfDevice (AudioIODevice* device, bool asInput) const override
    {
        return device != nullptr ? getDeviceNames (asInput).indexOf (device->getName()) : -1;
    }

    bool hasSeparateInputsAndOutputs() const override                   { return true; }

    AudioIODevice* createDevice (const String& output, const String& input) override
    {
        return new FakeDevice (world, output, input);
    }

private:
    FakeWorld& world;
};

// The monitor layer's HAL, made of the same world: an aggregate is one more output device whose channels
// are the main device's and the monitor device's, in the order JUCE would number them.
class FakeMonitorBackend final : public MonitorBackend
{
public:
    explicit FakeMonitorBackend (FakeWorld& worldIn) : world (worldIn) {}

    int countOutputChannels (const String& deviceName) override
    {
        if (! world.outputNames.contains (deviceName) || deviceName.startsWith (MonitorDeviceType::aggregateNamePrefix) || world.unresolvable.count (deviceName) > 0)
            return 0;

        return world.outputChannels.count (deviceName) > 0 ? world.outputChannels[deviceName] : 2;
    }

    std::unique_ptr<Aggregate> createAggregate (const String& mainName, const String& monitorName, String& error) override
    {
        // The wait for the last aggregate to leave the list.
        for (auto& name : world.pendingRemoval)
        {
            world.outputNames.removeString (name);
            ++world.settledRemovals;
        }

        world.pendingRemoval.clear();

        const auto mainChannels = countOutputChannels (mainName), monitorChannels = countOutputChannels (monitorName);

        if (world.failAggregate || mainChannels == 0 || monitorChannels == 0)
        {
            error = "the fake HAL refuses";
            return nullptr;
        }

        const auto name = MonitorDeviceType::aggregateNamePrefix + "fake" + String (++world.aggregatesCreated);
        MonitorLayout layout { 0, mainChannels, mainChannels, monitorChannels };
        StringArray names;

        if (world.monitorBeforeMain)
            layout = { monitorChannels, mainChannels, 0, monitorChannels };

        const auto addChannels = [&names] (const String& device, int count)
        {
            for (int i = 0; i < count; ++i)
                names.add ("out " + String (i + 1) + " (" + device + ")");
        };

        if (world.monitorBeforeMain)
        {
            addChannels (monitorName, monitorChannels);
            addChannels (mainName, mainChannels);
        }
        else
        {
            addChannels (mainName, mainChannels);
            addChannels (monitorName, monitorChannels);
        }

        world.outputNames.add (name);
        world.outputChannels[name] = mainChannels + monitorChannels;
        world.channelNames[name] = names;
        world.lastAggregateName = name;
        ++world.aggregatesAlive;
        return std::make_unique<FakeAggregate> (world, name, layout);
    }

private:
    class FakeAggregate final : public Aggregate
    {
    public:
        FakeAggregate (FakeWorld& worldIn, const String& nameIn, const MonitorLayout& layoutIn) : world (worldIn), name (nameIn), layout (layoutIn) {}

        ~FakeAggregate() override
        {
            if (world.delayedRemoval)
                world.pendingRemoval.add (name);
            else
                world.outputNames.removeString (name);

            world.outputChannels.erase (name);
            world.channelNames.erase (name);
            --world.aggregatesAlive;
        }

        String getName() const override             { return name; }
        MonitorLayout getLayout() const override    { return layout; }

    private:
        FakeWorld& world;
        String name;
        MonitorLayout layout;
    };

    FakeWorld& world;
};

} // namespace mfx::test
