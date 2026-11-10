#include "Hal.h"

#include <cmath>
#include <unistd.h>

namespace mfx::hal
{
using namespace juce;

namespace
{
    AudioObjectPropertyAddress address (AudioObjectPropertySelector selector,
                                        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
    {
        return { selector, scope, kAudioObjectPropertyElementMain };
    }

    template <typename T>
    bool read (AudioObjectID id, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope, T& value)
    {
        auto a = address (selector, scope);
        UInt32 size = sizeof (T);
        return AudioObjectHasProperty (id, &a) && AudioObjectGetPropertyData (id, &a, 0, nullptr, &size, &value) == noErr;
    }

    template <typename T>
    bool write (AudioObjectID id, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope, T value)
    {
        auto a = address (selector, scope);
        Boolean settable = false;
        return AudioObjectHasProperty (id, &a)
            && AudioObjectIsPropertySettable (id, &a, &settable) == noErr && settable
            && AudioObjectSetPropertyData (id, &a, 0, nullptr, sizeof (T), &value) == noErr;
    }

    std::vector<AudioObjectID> readIds (AudioObjectID id, AudioObjectPropertySelector selector,
                                        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
    {
        auto a = address (selector, scope);
        UInt32 size = 0;

        if (AudioObjectGetPropertyDataSize (id, &a, 0, nullptr, &size) != noErr || size == 0)
            return {};

        std::vector<AudioObjectID> ids (size / sizeof (AudioObjectID));

        if (AudioObjectGetPropertyData (id, &a, 0, nullptr, &size, ids.data()) != noErr)
            return {};

        return ids;
    }

    int channelCount (AudioObjectID id, AudioObjectPropertyScope scope)
    {
        auto a = address (kAudioDevicePropertyStreamConfiguration, scope);
        UInt32 size = 0;

        if (AudioObjectGetPropertyDataSize (id, &a, 0, nullptr, &size) != noErr || size == 0)
            return 0;

        HeapBlock<char> storage (size);
        auto* list = reinterpret_cast<AudioBufferList*> (storage.get());

        if (AudioObjectGetPropertyData (id, &a, 0, nullptr, &size, list) != noErr)
            return 0;

        int total = 0;

        for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
            total += (int) list->mBuffers[i].mNumberChannels;

        return total;
    }

    int streamLatency (AudioObjectID id, AudioObjectPropertyScope scope)
    {
        const auto streams = readIds (id, kAudioDevicePropertyStreams, scope);
        UInt32 latency = 0;

        if (! streams.empty())
            read (streams.front(), kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, latency);

        return (int) latency;
    }

    bool isAggregate (AudioObjectID id)
    {
        AudioClassID cls = 0;
        return read (id, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal, cls) && cls == kAudioAggregateDeviceClassID;
    }

    // An IOProc that only reads timestamps or input data must not also take part in the output mix.
    void disableOutputStreams (AudioObjectID device, AudioDeviceIOProcID proc)
    {
        const auto count = readIds (device, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput).size();

        if (count == 0)
            return;

        HeapBlock<char> storage (sizeof (AudioHardwareIOProcStreamUsage) + count * sizeof (UInt32), true);
        auto* usage = reinterpret_cast<AudioHardwareIOProcStreamUsage*> (storage.get());
        usage->mIOProc = reinterpret_cast<void*> (proc);
        usage->mNumberStreams = (UInt32) count;

        auto a = address (kAudioDevicePropertyIOProcStreamUsage, kAudioObjectPropertyScopeOutput);
        AudioObjectSetPropertyData (device, &a, 0, nullptr, (UInt32) (sizeof (AudioHardwareIOProcStreamUsage) + (count - 1) * sizeof (UInt32)), usage);
    }
}

String nameOf (AudioObjectID id)
{
    CFStringRef name = nullptr;
    UInt32 size = sizeof (name);
    auto a = address (kAudioObjectPropertyName);

    if (AudioObjectGetPropertyData (id, &a, 0, nullptr, &size, &name) != noErr || name == nullptr)
        return {};

    auto result = String::fromCFString (name);
    CFRelease (name);
    return result;
}

std::vector<AudioObjectID> allDevices()
{
    return readIds (kAudioObjectSystemObject, kAudioHardwarePropertyDevices);
}

AudioObjectID findDevice (const String& name, bool wantInput, bool wantOutput)
{
    for (auto id : allDevices())
    {
        if (nameOf (id) != name || isAggregate (id))
            continue;

        if ((wantInput && channelCount (id, kAudioObjectPropertyScopeInput) == 0)
            || (wantOutput && channelCount (id, kAudioObjectPropertyScopeOutput) == 0))
            continue;

        return id;
    }

    return kAudioObjectUnknown;
}

AudioObjectID findJuceAggregate()
{
    const auto wanted = "_JucePrivateAggregateDevice_" + String ((int) getpid());

    for (auto id : allDevices())
        if (isAggregate (id) && nameOf (id) == wanted)
            return id;

    return kAudioObjectUnknown;
}

std::vector<AudioObjectID> subDevices (AudioObjectID aggregate)
{
    return readIds (aggregate, kAudioAggregateDevicePropertyActiveSubDeviceList);
}

Numbers readNumbers (AudioObjectID id)
{
    Numbers n;
    UInt32 value = 0;
    Float64 rate = 0;
    AudioValueRange range {};

    if (read (id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate))
        n.nominalRate = rate;

    if (read (id, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, value))
        n.bufferSize = (int) value;

    if (read (id, kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal, range))
    {
        n.bufferMin = (int) range.mMinimum;
        n.bufferMax = (int) range.mMaximum;
    }

    const auto fill = [&] (AudioObjectPropertyScope scope, int& latency, int& safety, int& stream, int& channels)
    {
        value = 0;
        latency = read (id, kAudioDevicePropertyLatency, scope, value) ? (int) value : 0;
        value = 0;
        safety = read (id, kAudioDevicePropertySafetyOffset, scope, value) ? (int) value : 0;
        stream = streamLatency (id, scope);
        channels = channelCount (id, scope);
    };

    fill (kAudioObjectPropertyScopeInput, n.inputLatency, n.inputSafety, n.inputStreamLatency, n.inputChannels);
    fill (kAudioObjectPropertyScopeOutput, n.outputLatency, n.outputSafety, n.outputStreamLatency, n.outputChannels);
    return n;
}

bool setBufferSize (AudioObjectID id, int frames)
{
    return write (id, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, (UInt32) frames);
}

bool readIoCycleUsage (AudioObjectID id, float& usage)
{
    return read (id, kAudioDevicePropertyIOCycleUsage, kAudioObjectPropertyScopeGlobal, usage);
}

bool writeIoCycleUsage (AudioObjectID id, float usage)
{
    return write (id, kAudioDevicePropertyIOCycleUsage, kAudioObjectPropertyScopeGlobal, (Float32) usage);
}

double hostTicksToNs (uint64_t ticks)
{
    return (double) AudioConvertHostTimeToNanos (ticks);
}

//==============================================================================
OverloadListener::OverloadListener (std::vector<AudioObjectID> devices)
    : watched (std::move (devices))
{
    for (auto id : watched)
    {
        AudioObjectPropertyAddress a { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeWildcard, kAudioObjectPropertyElementWildcard };
        AudioObjectAddPropertyListener (id, &a, &notify, this);
    }
}

OverloadListener::~OverloadListener()
{
    for (auto id : watched)
    {
        AudioObjectPropertyAddress a { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeWildcard, kAudioObjectPropertyElementWildcard };
        AudioObjectRemovePropertyListener (id, &a, &notify, this);
    }
}

OSStatus OverloadListener::notify (AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* self)
{
    ++static_cast<OverloadListener*> (self)->count;
    return noErr;
}

//==============================================================================
TimestampTap::TimestampTap (AudioObjectID deviceIn, size_t capacity)
    : device (deviceIn), entries (capacity)
{
    if (AudioDeviceCreateIOProcID (device, &ioProc, this, &proc) != noErr)
        proc = nullptr;
    else
        disableOutputStreams (device, proc);
}

TimestampTap::~TimestampTap()
{
    stop();

    if (proc != nullptr)
        AudioDeviceDestroyIOProcID (device, proc);
}

bool TimestampTap::start()
{
    written = 0;
    running = proc != nullptr && AudioDeviceStart (device, proc) == noErr;
    return running;
}

void TimestampTap::stop()
{
    if (running)
        AudioDeviceStop (device, proc);

    running = false;
}

OSStatus TimestampTap::ioProc (AudioObjectID, const AudioTimeStamp* now, const AudioBufferList*, const AudioTimeStamp* inputTime,
                               AudioBufferList*, const AudioTimeStamp* outputTime, void* self)
{
    auto& tap = *static_cast<TimestampTap*> (self);
    const auto index = tap.written.load (std::memory_order_relaxed);

    if (index < tap.entries.size())
        tap.entries[index] = { inputTime != nullptr ? inputTime->mHostTime : 0, outputTime != nullptr ? outputTime->mHostTime : 0,
                               now != nullptr ? now->mHostTime : 0 };

    tap.written.store (index + 1, std::memory_order_release);
    return noErr;
}

//==============================================================================
Capture::Capture (AudioObjectID deviceIn, double seconds, double sampleRate)
    : device (deviceIn)
{
    channels = jmin (2, channelCount (device, kAudioObjectPropertyScopeInput));
    capacity = (int64_t) (seconds * sampleRate);

    for (int c = 0; c < channels; ++c)
        samples[c].assign ((size_t) capacity, 0.0f);

    buffers.resize ((size_t) jlimit (1024.0, 600000.0, seconds * sampleRate / 8.0));

    if (channels > 0)
    {
        if (AudioDeviceCreateIOProcID (device, &ioProc, this, &proc) != noErr)
            proc = nullptr;
        else
            disableOutputStreams (device, proc);
    }
}

Capture::~Capture()
{
    stop();

    if (proc != nullptr)
        AudioDeviceDestroyIOProcID (device, proc);
}

bool Capture::start()
{
    recorded = 0;
    bufferCount = 0;
    discontinuities = 0;
    lastSampleTime = -1.0;
    running = proc != nullptr && AudioDeviceStart (device, proc) == noErr;
    return running;
}

void Capture::stop()
{
    if (running)
        AudioDeviceStop (device, proc);

    running = false;
}

OSStatus Capture::ioProc (AudioObjectID, const AudioTimeStamp* now, const AudioBufferList* in, const AudioTimeStamp* inputTime,
                          AudioBufferList*, const AudioTimeStamp*, void* self)
{
    auto& cap = *static_cast<Capture*> (self);

    if (in == nullptr || in->mNumberBuffers == 0 || inputTime == nullptr)
        return noErr;

    const auto start = cap.recorded.load (std::memory_order_relaxed);
    const auto index = cap.bufferCount.load (std::memory_order_relaxed);
    const int frames = (int) (in->mBuffers[0].mDataByteSize / (in->mBuffers[0].mNumberChannels * sizeof (float)));

    if (frames <= 0 || start + frames > cap.capacity || index >= cap.buffers.size())
        return noErr;

    if (cap.lastSampleTime >= 0.0 && std::abs (inputTime->mSampleTime - (cap.lastSampleTime + cap.lastFrames)) > 0.5)
        ++cap.discontinuities;

    cap.lastSampleTime = inputTime->mSampleTime;
    cap.lastFrames = frames;

    for (int c = 0; c < cap.channels; ++c)
    {
        int offset = 0;

        for (UInt32 b = 0; b < in->mNumberBuffers; ++b)
        {
            const auto& buffer = in->mBuffers[b];
            const int n = (int) buffer.mNumberChannels;

            if (c < offset + n && buffer.mData != nullptr)
            {
                const auto* src = static_cast<const float*> (buffer.mData) + (c - offset);
                auto* dst = cap.samples[c].data() + start;

                for (int i = 0; i < frames; ++i)
                    dst[i] = src[(size_t) i * (size_t) n];

                break;
            }

            offset += n;
        }
    }

    cap.buffers[index] = { inputTime->mHostTime, now != nullptr ? now->mHostTime : 0, start, frames };
    cap.recorded.store (start + frames, std::memory_order_release);
    cap.bufferCount.store (index + 1, std::memory_order_release);
    return noErr;
}

int Capture::findBuffer (int64_t sample) const
{
    return hal::findBuffer (bufferSpan(), sample);
}

double Capture::timeOfSampleNs (double sample, double sampleRate) const
{
    return hal::timeOfSampleNs (bufferSpan(), sample, sampleRate);
}

int findBuffer (Span<const Capture::Buffer> buffers, int64_t sample)
{
    const auto n = (size_t) buffers.size();
    size_t low = 0, high = n;

    while (low < high)
    {
        const auto mid = (low + high) / 2;

        if (buffers[mid].firstSample + buffers[mid].frames <= sample)
            low = mid + 1;
        else
            high = mid;
    }

    return low < n && buffers[low].firstSample <= sample ? (int) low : -1;
}

double timeOfSampleNs (Span<const Capture::Buffer> buffers, double sample, double sampleRate)
{
    const auto index = findBuffer (buffers, (int64_t) std::floor (sample));

    if (index < 0)
        return -1.0;

    const auto& b = buffers[(size_t) index];
    return hostTicksToNs (b.inputTicks) + (sample - (double) b.firstSample) / sampleRate * 1.0e9;
}

} // namespace mfx::hal
