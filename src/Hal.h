#pragma once

#include <CoreAudio/CoreAudio.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

// Thin CoreAudio HAL helpers: device lookup, the numbers JUCE does not expose, and the raw IOProcs
// the latency measurement needs. Everything that touches the HAL from a realtime thread is here and
// allocates nothing after construction.
namespace mfx::hal
{
using juce::String;

struct Numbers
{
    double nominalRate = 0.0;
    int bufferSize = 0, bufferMin = 0, bufferMax = 0;
    int inputLatency = 0, inputSafety = 0, inputStreamLatency = 0, inputChannels = 0;
    int outputLatency = 0, outputSafety = 0, outputStreamLatency = 0, outputChannels = 0;
};

String nameOf (AudioObjectID);
std::vector<AudioObjectID> allDevices();

// Exact name match. Aggregate devices are skipped unless asked for.
AudioObjectID findDevice (const String& name, bool wantInput, bool wantOutput);

// The private aggregate device this process's JUCE built for the open device pair.
AudioObjectID findJuceAggregate();

std::vector<AudioObjectID> subDevices (AudioObjectID aggregate);
Numbers readNumbers (AudioObjectID);

bool setBufferSize (AudioObjectID, int frames);

// kAudioDevicePropertyIOCycleUsage; false when the device has no such property.
bool readIoCycleUsage (AudioObjectID, float& usage);
bool writeIoCycleUsage (AudioObjectID, float usage);

// Time conversion shared by every timestamp in this file.
double hostTicksToNs (uint64_t ticks);

//==============================================================================
// Counts kAudioDeviceProcessorOverload notifications on a set of devices.
class OverloadListener
{
public:
    explicit OverloadListener (std::vector<AudioObjectID> devices);
    ~OverloadListener();

    int getCount() const noexcept { return count.load(); }
    void reset() noexcept { count = 0; }

private:
    static OSStatus notify (AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*);

    std::vector<AudioObjectID> watched;
    std::atomic<int> count { 0 };
};

//==============================================================================
// A second IOProc on the aggregate that only writes down the input and output timestamps of every
// cycle. JUCE hands its callback the output time only; the input time is the one the measurement
// starts from.
class TimestampTap
{
public:
    struct Entry { uint64_t inputTicks = 0, outputTicks = 0, nowTicks = 0; };

    TimestampTap (AudioObjectID device, size_t capacity);
    ~TimestampTap();

    bool start();
    void stop();

    size_t size() const noexcept { return std::min (written.load(), entries.size()); }
    const Entry& operator[] (size_t i) const noexcept { return entries[i]; }
    juce::Span<const Entry> span() const noexcept { return { entries.data(), size() }; }

private:
    static OSStatus ioProc (AudioObjectID, const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
                            AudioBufferList*, const AudioTimeStamp*, void*);

    AudioObjectID device;
    AudioDeviceIOProcID proc = nullptr;
    std::vector<Entry> entries;
    std::atomic<size_t> written { 0 };
    bool running = false;
};

//==============================================================================
// Records the first two input channels of a device together with the host time of every buffer.
class Capture
{
public:
    struct Buffer
    {
        uint64_t inputTicks = 0;     // HAL time at which the first frame was acquired
        uint64_t nowTicks = 0;       // when the IOProc actually ran
        int64_t firstSample = 0;     // index of the first frame in `samples`
        int frames = 0;
    };

    Capture (AudioObjectID device, double seconds, double sampleRate);
    ~Capture();

    bool start();
    void stop();

    int numChannels() const noexcept { return channels; }
    int64_t numSamples() const noexcept { return std::min (recorded.load(), capacity); }
    const float* data (int channel) const noexcept { return samples[(size_t) channel].data(); }
    size_t numBuffers() const noexcept { return std::min (bufferCount.load(), buffers.size()); }
    const Buffer& buffer (size_t i) const noexcept { return buffers[i]; }
    int64_t gaps() const noexcept { return discontinuities.load(); }

    juce::Span<const Buffer> bufferSpan() const noexcept { return { buffers.data(), numBuffers() }; }

    // Index of the buffer that holds `sample`, or -1.
    int findBuffer (int64_t sample) const;

    // HAL time in nanoseconds of a (fractional) sample index, anchored to the buffer that holds it.
    double timeOfSampleNs (double sample, double sampleRate) const;

private:
    static OSStatus ioProc (AudioObjectID, const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
                            AudioBufferList*, const AudioTimeStamp*, void*);

    AudioObjectID device;
    AudioDeviceIOProcID proc = nullptr;
    int channels = 0;
    int64_t capacity = 0;
    std::vector<float> samples[2];
    std::vector<Buffer> buffers;
    std::atomic<int64_t> recorded { 0 };
    std::atomic<size_t> bufferCount { 0 };
    std::atomic<int64_t> discontinuities { 0 };
    double lastSampleTime = -1.0;
    int lastFrames = 0;
    bool running = false;
};

// The same lookups on any run of buffers in recording order, so the timing arithmetic can be tested
// with made-up timestamps.
int findBuffer (juce::Span<const Capture::Buffer> buffers, int64_t sample);
double timeOfSampleNs (juce::Span<const Capture::Buffer> buffers, double sample, double sampleRate);

} // namespace mfx::hal
