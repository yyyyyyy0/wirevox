#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <set>

// A device layer without hardware, for the self tests. Handed to an AudioDeviceManager through
// addAudioDeviceType() before it scans, it replaces CoreAudio, so the engine, the tuner and the
// settings can be run through real reopen / failure / restore paths with nothing opened on the Mac.
namespace mfx::test
{
using namespace juce;

struct FakeWorld
{
    Array<int> bufferSizes { 16, 32, 64, 128, 256, 512, 1024 };
    std::set<int> refusedBuffers;       // open() fails for these sizes; JUCE then drops the device
    int opens = 0;                      // successful opens
    Array<int> openedBuffers;           // the buffer size of each of them
};

class FakeDevice final : public AudioIODevice
{
public:
    FakeDevice (FakeWorld& worldIn, const String& output, const String& input)
        : AudioIODevice (output.isNotEmpty() ? output : input, "Fake"), world (worldIn) {}

    StringArray getOutputChannelNames() override        { return { "out L", "out R" }; }
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

private:
    FakeWorld& world;
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
    StringArray getDeviceNames (bool wantInputNames) const override     { return wantInputNames ? StringArray { "FakeIn", "FakeIn2" } : StringArray { "FakeOut" }; }
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

} // namespace mfx::test
