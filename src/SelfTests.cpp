#include "SelfTests.h"
#include "Breakdown.h"
#include "Chain.h"
#include "Engine.h"
#include "FakeAudioDevice.h"
#include "LatencyTuner.h"
#include "Measure.h"
#include "ProcessTime.h"
#include "Settings.h"
#include "Text.h"

#include <CoreAudio/HostTime.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

namespace mfx
{
using namespace juce;

namespace
{
    void say (const String& text)
    {
        std::cout << text.toRawUTF8() << std::endl;
    }

    struct Checker
    {
        void operator() (bool condition, const String& what)
        {
            say (String (condition ? "ok:   " : "FAIL: ") + what);
            ok = ok && condition;
        }

        bool ok = true;
    };

    uint64_t toTicks (double ns)
    {
        return (uint64_t) AudioConvertNanosToHostTime ((UInt64) std::llround (ns));
    }
}

//==============================================================================
// A 48 kHz host with 64-sample cycles and a recording device with 128-sample buffers. The burst is
// injected in cycle 100 and arrives `trueLatencyNs` after that cycle's input was acquired; the
// analysis has to recover exactly that, plus the reading side's floor.
int selfTestTiming()
{
    Checker check;
    using Entry = hal::TimestampTap::Entry;

    constexpr double rate = 48000.0;
    constexpr double periodNs = 64.0 / rate * 1.0e9;
    constexpr double declaredNs = 3.106667e6;
    constexpr double baseNs = 5.0e9;
    constexpr int injectedCycle = 100;
    constexpr double trueLatencyNs = 20.4217e6;
    constexpr int captureFrames = 128;
    constexpr double recordingStartNs = baseNs - 30.0e6;
    constexpr double floorNs = 1.0e9 * 133.0 / rate;
    constexpr int chainSamples = 184;

    const auto inputNsOf = [] (int cycle) { return baseNs + cycle * periodNs; };

    std::vector<Entry> tap (300);

    for (size_t i = 0; i < tap.size(); ++i)
    {
        tap[i].inputTicks = toTicks (inputNsOf ((int) i));
        tap[i].outputTicks = toTicks (inputNsOf ((int) i) + declaredNs);
        tap[i].nowTicks = tap[i].inputTicks;
    }

    const auto burst = measure::makeTestSignal (rate);
    const auto numBuffers = (size_t) (1.5 * rate / captureFrames);
    std::vector<hal::Capture::Buffer> buffers (numBuffers);

    for (size_t j = 0; j < numBuffers; ++j)
        buffers[j] = { toTicks (recordingStartNs + (double) j * captureFrames / rate * 1.0e9), 0, (int64_t) j * captureFrames, captureFrames };

    const auto samplePositionOf = [&] (double arrivalNs) { return (arrivalNs - recordingStartNs) / 1.0e9 * rate; };

    std::vector<float> recording (numBuffers * (size_t) captureFrames, 0.0f);
    const auto arrival = samplePositionOf (inputNsOf (injectedCycle) + trueLatencyNs);

    // A burst twice as loud that arrived long before the search window must not be picked.
    const auto decoy = samplePositionOf (inputNsOf (20) + trueLatencyNs);

    for (size_t i = 0; i < recording.size(); ++i)
        recording[i] = (float) (measure::testSignalAt (rate, ((double) i - arrival) / rate)
                                + 2.0 * measure::testSignalAt (rate, ((double) i - decoy) / rate));

    const auto outputNsOf = [&] (int cycle) { return (uint64_t) std::llround (hal::hostTicksToNs (tap[(size_t) cycle].outputTicks)); };

    measure::BurstInputs inputs;
    inputs.tap = { tap.data(), tap.size() };
    inputs.buffers = { buffers.data(), buffers.size() };
    inputs.channels = { recording.data() };
    inputs.numSamples = (int) recording.size();
    inputs.burst = burst.data();
    inputs.burstLength = (int) burst.size();
    inputs.sampleRate = rate;
    inputs.readerFloorNs = floorNs;

    const auto expectedMs = (trueLatencyNs + floorNs) / 1.0e6;
    const auto run = measure::analyseBurst (inputs, outputNsOf (injectedCycle));

    check (run.trustworthy, "the burst is found and trusted (correlation " + String (run.coefficient, 3) + ")");
    check (std::abs (run.latencyMs - expectedMs) < 0.05, "latency " + String (run.latencyMs, 3) + " ms against the expected " + String (expectedMs, 3));
    check (std::abs (run.declaredMs - declaredNs / 1.0e6) < 0.001, "declared " + String (run.declaredMs, 4) + " ms is the output time minus the input time of the same cycle");
    check (std::abs (run.excessMs - (run.latencyMs - run.declaredMs)) < 1.0e-9, "excess is latency minus declared");

    {
        // The breakdown adds up the HAL's numbers; the cycle's own timestamps declare the same span. 64 + 64
        // samples of buffer with a safety offset of 5 on the input and 4 on the output is 137 samples at 44.1 kHz.
        hal::Numbers numbers;
        numbers.bufferSize = 64;
        numbers.inputSafety = 5;
        numbers.outputSafety = 4;
        const auto host = declareHost (numbers, 1.0);
        check (host.total() == 137, "the host declares " + String (host.total()) + " samples (64 + 64 + 5 + 4)");
        check (std::abs (samplesToMs (host.total(), 44100.0) - declaredNs / 1.0e6) < 0.001,
               "137 samples at 44.1 kHz (" + String (samplesToMs (host.total(), 44100.0), 4) + " ms) is the span the timestamps declare above");
        BreakdownInput input;
        input.sampleRate = 44100.0;
        input.aggregate = numbers;
        check (std::abs (computeBreakdown (input).declaredMs - declaredNs / 1.0e6) < 0.001, "and it is what the breakdown reports as declared");
    }

    // A sign error on the reading side's floor moves the result by twice the floor.
    auto noFloor = inputs;
    noFloor.readerFloorNs = 0.0;
    const auto withoutFloor = measure::analyseBurst (noFloor, outputNsOf (injectedCycle));
    check (std::abs ((run.latencyMs - withoutFloor.latencyMs) - floorNs / 1.0e6) < 0.01, "the reading side's latency is added, not subtracted");

    // Pairing with the neighbouring cycle moves the origin by exactly one host period.
    const auto later = measure::analyseBurst (inputs, outputNsOf (injectedCycle + 1));
    const auto earlier = measure::analyseBurst (inputs, outputNsOf (injectedCycle - 1));
    check (std::abs (later.latencyMs - (expectedMs - periodNs / 1.0e6)) < 0.05, "the next cycle's origin gives one period less (" + String (later.latencyMs, 3) + " ms)");
    check (std::abs (earlier.latencyMs - (expectedMs + periodNs / 1.0e6)) < 0.05, "the previous cycle's origin gives one period more (" + String (earlier.latencyMs, 3) + " ms)");

    // The plugins' latency lengthens what the host cycle declares and leaves the arrival alone.
    auto withChain = inputs;
    withChain.chainNs = 1.0e9 * chainSamples / rate;
    const auto chained = measure::analyseBurst (withChain, outputNsOf (injectedCycle));
    check (std::abs (chained.declaredMs - (declaredNs + withChain.chainNs) / 1.0e6) < 0.001 && std::abs (chained.latencyMs - run.latencyMs) < 1.0e-9,
           "chain latency raises the declared span only");

    check (measure::findCycle (inputs.tap, (double) outputNsOf (injectedCycle) + 90000.0) == injectedCycle, "a cycle is matched within 90 us");
    check (measure::findCycle (inputs.tap, (double) outputNsOf (injectedCycle) + 110000.0) == -1, "a time 110 us from every cycle matches none");
    check (measure::findCycle ({}, 1.0e9) == -1, "an empty tap matches nothing");

    {
        std::vector<float> noise (recording.size());
        Random random (7);

        for (auto& v : noise)
            v = 0.02f * (random.nextFloat() * 2.0f - 1.0f);

        auto noisy = inputs;
        noisy.channels = { noise.data() };
        check (! measure::analyseBurst (noisy, outputNsOf (injectedCycle)).trustworthy, "a recording without the burst is not trusted");
    }

    check (hal::findBuffer (inputs.buffers, 0) == 0 && hal::findBuffer (inputs.buffers, captureFrames - 1) == 0 && hal::findBuffer (inputs.buffers, captureFrames) == 1,
           "buffer lookup at the edges of the first two buffers");
    check (hal::findBuffer (inputs.buffers, -1) == -1 && hal::findBuffer (inputs.buffers, (int64_t) recording.size()) == -1, "buffer lookup outside the recording finds nothing");
    check (std::abs (hal::timeOfSampleNs (inputs.buffers, captureFrames + 0.5, rate) - (hal::hostTicksToNs (buffers[1].inputTicks) + 0.5 / rate * 1.0e9)) < 1.0,
           "time of a fractional sample is anchored to its own buffer");

    //==========================================================================
    // The injection into the callback: the live microphone must never get through.
    {
        Engine engine;
        engine.openDevices (nullptr);
        engine.start();

        test::FakeWorld world;
        test::FakeDevice device (world, "FakeOut", "FakeIn");
        BigInteger in, out;
        in.setBit (0);
        out.setRange (0, 2, true);
        device.open (in, out, rate, 64);
        engine.callback.audioDeviceAboutToStart (&device);

        const auto blockOut = [&engine] (int numSamples, float micLevel, std::vector<float>& left)
        {
            std::vector<float> mic ((size_t) numSamples, micLevel), right ((size_t) numSamples);
            left.assign ((size_t) numSamples, 9.0f);
            const float* channels[] = { mic.data() };
            float* outputs[] = { left.data(), right.data() };
            engine.callback.audioDeviceIOCallbackWithContext (channels, 1, outputs, 2, numSamples, {});
        };

        const auto silent = [] (const std::vector<float>& block)
        {
            return std::all_of (block.begin(), block.end(), [] (float v) { return v == 0.0f; });
        };

        std::vector<float> block;
        blockOut (64, 0.5f, block);
        check (std::abs (block[0] - 0.5f) < 1.0e-6f && std::abs (block[63] - 0.5f) < 1.0e-6f, "not injecting: the microphone reaches the output");

        engine.callback.setInjecting (true);
        blockOut (64, 0.5f, block);
        check (silent (block), "injecting without a burst armed: silence, no microphone");

        engine.callback.armBurst();
        blockOut (64, 0.5f, block);
        check (std::abs (block[10] - engine.callback.getBurst()[10]) < 1.0e-6f && std::abs (block[63] - engine.callback.getBurst()[63]) < 1.0e-6f,
               "an armed burst replaces the microphone");
        blockOut (64, 0.5f, block);
        blockOut (64, 0.5f, block);

        engine.callback.setInjecting (false);
        engine.callback.setInjecting (true);
        blockOut (64, 0.5f, block);
        check (silent (block), "restarting an injection forgets the burst that was half played (" + String (block[0], 4) + ")");
        check (! engine.callback.injectionWasDisturbed(), "no block was disturbed so far");

        blockOut (128, 0.5f, block);
        check (silent (block), "a block longer than the scratch buffer is played as silence, not as the microphone");
        check (engine.callback.injectionWasDisturbed(), "and the run is flagged as disturbed");

        engine.callback.setInjecting (true);
        check (! engine.callback.injectionWasDisturbed(), "a new injection starts with the flag cleared");
        engine.callback.setInjecting (false);
    }

    say (check.ok ? "SELFTEST TIMING OK" : "SELFTEST TIMING FAILED");
    return check.ok ? 0 : 1;
}

//==============================================================================
namespace
{
    String settingsText (int buffer, const String& cycleUsage = {})
    {
        return "<MIC_FX_HOST theme=\"rack84\"" + (cycleUsage.isNotEmpty() ? " cycleUsage=\"" + cycleUsage + "\"" : String())
             + "><DEVICESETUP deviceType=\"Fake\" audioOutputDeviceName=\"FakeOut\" audioInputDeviceName=\"FakeIn\" audioDeviceRate=\"48000.0\""
               " audioDeviceBufferSize=\"" + String (buffer) + "\" audioDeviceInChans=\"1\" audioDeviceOutChans=\"11\"/><CHAIN/></MIC_FX_HOST>";
    }

    // An engine on the fake device layer, opened from settings the way the window does.
    struct Rig
    {
        explicit Rig (const XmlElement& settings, const Array<int>& bufferSizes = {})
        {
            if (! bufferSizes.isEmpty())
                world.bufferSizes = bufferSizes;

            engine.deviceManager.addAudioDeviceType (std::make_unique<test::FakeDeviceType> (world));
            engine.openFromSettings (&settings);
            engine.start();
        }

        explicit Rig (const String& settings, const Array<int>& bufferSizes = {}) : Rig (*XmlDocument::parse (settings), bufferSizes) {}

        int runningBuffer() const
        {
            auto* device = engine.deviceManager.getCurrentAudioDevice();
            return device != nullptr ? device->getCurrentBufferSizeSamples() : 0;
        }

        // What the window's persist() writes.
        bool persist() const { return saveSettings (*buildSettingsXml ("rack84", engine)); }

        test::FakeWorld world;
        Engine engine;
    };

    String readFile() { return getSettingsFile().loadFileAsString(); }

    // The attributes that matter, from a settings file on disk.
    struct OnDisk
    {
        int buffer = 0;
        String cycleUsage;
    };

    OnDisk parseOnDisk()
    {
        OnDisk result;

        if (auto xml = loadSettings())
        {
            if (auto* device = xml->getChildByName ("DEVICESETUP"))
                result.buffer = device->getIntAttribute ("audioDeviceBufferSize");

            result.cycleUsage = xml->getStringAttribute ("cycleUsage");
        }

        return result;
    }

    void setReaderUncleanAtOrBelow (LatencyTuner& tuner, Rig& rig, int buffer)
    {
        tuner.setReader ([&rig, buffer]
        {
            return LatencyTuner::Reading { rig.runningBuffer() <= buffer ? 1 : 0, 0, 0.0 };
        });
    }

    // Writes the settings after every tick, the way the window's timer and device listener do, and
    // returns false as soon as a write differs from `expected` while the run is still going.
    bool tickToEnd (LatencyTuner& tuner, Rig& rig, const String& expected, int& writes, bool& everDiffered)
    {
        for (int guard = 0; guard < 5000 && tuner.isRunning(); ++guard)
        {
            tuner.tick();

            if (tuner.isRunning())
            {
                rig.persist();
                ++writes;
                everDiffered = everDiffered || readFile() != expected;
            }
        }

        return ! tuner.isRunning();
    }
}

int selfTestEngineChild (const String& settingsPath)
{
    useSettingsFile (File (settingsPath));
    const auto settings = loadSettings();

    if (settings == nullptr)
        return 2;

    Rig rig (*settings);
    LatencyTuner tuner (rig.engine);
    tuner.setReader ([] { return LatencyTuner::Reading {}; });
    tuner.start();

    // Run until both kinds of trial value are live: a smaller buffer on the device and a trial cycle usage.
    for (int guard = 0; guard < 5000 && tuner.isRunning(); ++guard)
    {
        tuner.tick();
        rig.persist();

        if (rig.engine.getExperiments().ioCycleUsage != settings->getDoubleAttribute ("cycleUsage", -1.0) && rig.runningBuffer() != 64)
            break;
    }

    say ("TUNING buffer " + String (rig.runningBuffer()) + " cycleUsage " + (rig.engine.getExperiments().ioCycleUsage.has_value()
                                                                              ? String (*rig.engine.getExperiments().ioCycleUsage, 2) : String ("default")));

    for (;;)
        Thread::sleep (100);
}

int selfTestEngine()
{
    Checker check;

    const auto folder = File::getSpecialLocation (File::tempDirectory).getNonexistentChildFile ("voxrack-engine-selftest", {}, false);
    folder.createDirectory();
    useSettingsFile (folder.getChildFile ("settings.xml"));

    //==========================================================================
    {
        check (! validCycleUsage (std::nan ("")).has_value() && ! validCycleUsage (0.0).has_value() && ! validCycleUsage (-0.5).has_value()
                   && ! validCycleUsage (1.5).has_value(),
               "a cycle usage of NaN, 0, a negative number or above 1 is rejected");
        check (validCycleUsage (0.25) == 0.25f && validCycleUsage (1.0) == 1.0f, "0.25 and 1.0 are accepted");

        for (const auto* bad : { "nan", "0", "-1", "2", "abc" })
        {
            Rig rig (settingsText (64, bad));
            check (! rig.engine.getExperiments().ioCycleUsage.has_value(), String ("a settings file with cycleUsage=\"") + bad + "\" leaves the device's default");
        }

        Rig valid (settingsText (64, "0.5"));
        check (valid.engine.getExperiments().ioCycleUsage == 0.5f, "a settings file with cycleUsage=\"0.5\" is applied");
    }

    {
        check (describeRateMismatch (44100.0, 44100.0, 44100.0).isEmpty(), "equal rates give no warning");
        check (describeRateMismatch (44100.0, 0.0, 0.0).isEmpty(), "unknown rates give no warning");
        const auto input = describeRateMismatch (44100.0, 192000.0, 44100.0);
        const auto output = describeRateMismatch (44100.0, 44100.0, 48000.0);
        const auto both = describeRateMismatch (44100.0, 192000.0, 48000.0);
        check (input.contains ("192000") && ! input.contains ("48000"), "an input interface at another rate is named");
        check (output.contains ("48000") && ! output.contains ("192000"), "an output device at another rate is named");
        check (both.contains ("192000") && both.contains ("48000"), "both ends are named when both differ");
    }

    //==========================================================================
    // A tune that completes: nothing is written while it runs, the settled pair afterwards.
    {
        Rig rig (settingsText (64));
        check (rig.runningBuffer() == 64, "the fake device opens at the saved buffer of 64");
        rig.persist();
        const auto before = readFile();

        LatencyTuner tuner (rig.engine);
        setReaderUncleanAtOrBelow (tuner, rig, 16);
        tuner.start();
        check (tuner.isRunning() && rig.engine.isPersistedAudioHeld(), "the tune is running and holds the persisted settings");

        int writes = 0;
        bool differed = false;
        const auto ended = tickToEnd (tuner, rig, before, writes, differed);

        check (ended, "the tune finishes");
        check (! differed, "all " + String (writes) + " settings written during the tune equal the pre-tune file");
        check (rig.world.openedBuffers.contains (16) && rig.world.openedBuffers.contains (32), "it really tried the smaller buffers (opened: " + String (rig.world.openedBuffers.size()) + " times)");
        check (rig.runningBuffer() == 32 && rig.engine.getExperiments().ioCycleUsage == 0.25f, "settled on buffer 32 and cycle usage 0.25 on the device");
        check (! rig.engine.isPersistedAudioHeld(), "the hold is released");

        rig.persist();
        const auto disk = parseOnDisk();
        check (disk.buffer == 32 && disk.cycleUsage.getDoubleValue() == 0.25, "the file now holds the settled pair: buffer " + String (disk.buffer) + ", cycleUsage " + disk.cycleUsage);

        Rig relaunch (*loadSettings());
        check (relaunch.runningBuffer() == 32 && relaunch.engine.getExperiments().ioCycleUsage == 0.25f, "a relaunch from that file runs buffer 32 with cycle usage 0.25");
    }

    // Cancelled in the middle, with a trial buffer and a trial cycle usage on the device.
    {
        Rig rig (settingsText (64, "0.5"));
        rig.persist();
        const auto before = readFile();

        LatencyTuner tuner (rig.engine);
        tuner.setReader ([] { return LatencyTuner::Reading {}; });
        tuner.start();

        int guard = 0;

        while (tuner.isRunning() && guard++ < 5000 && ! (rig.runningBuffer() != 64 && rig.engine.getExperiments().ioCycleUsage != 0.5f))
            tuner.tick();

        check (rig.runningBuffer() != 64 && rig.engine.getExperiments().ioCycleUsage != 0.5f, "mid-tune the device runs a trial buffer (" + String (rig.runningBuffer()) + ") and a trial cycle usage");
        rig.persist();
        check (readFile() == before, "writing the settings mid-tune leaves the file as it was");

        tuner.cancel();
        check (! tuner.isRunning() && rig.runningBuffer() == 64 && rig.engine.getExperiments().ioCycleUsage == 0.5f, "cancel puts buffer 64 and cycle usage 0.5 back on the device and in memory");
        rig.persist();
        check (readFile() == before, "after cancel the file is byte for byte what it was before the tune");
        check (tuner.getResult().isNotEmpty() && ! rig.engine.isPersistedAudioHeld(), "cancel leaves a message and releases the hold");
    }

    // Leaving while it runs: the tuner's destructor is the quit path of the window.
    {
        Rig rig (settingsText (64, "0.5"));
        rig.persist();
        const auto before = readFile();

        {
            LatencyTuner tuner (rig.engine);
            tuner.setReader ([] { return LatencyTuner::Reading {}; });
            tuner.start();

            for (int i = 0; i < 100; ++i)
                tuner.tick();
        }

        check (rig.runningBuffer() == 64 && rig.engine.getExperiments().ioCycleUsage == 0.5f, "destroying a running tuner restores the device and the cycle usage");
        rig.persist();
        check (readFile() == before, "and what is written afterwards is the pre-tune file");
    }

    // A step that cannot be opened: the run ends, says so, and the device is back at the original buffer.
    {
        Rig rig (settingsText (64, "0.5"));
        rig.persist();
        const auto before = readFile();
        rig.world.refusedBuffers = { 32 };

        LatencyTuner tuner (rig.engine);
        tuner.setReader ([] { return LatencyTuner::Reading {}; });
        tuner.start();

        bool finishedByTick = false;

        for (int guard = 0; guard < 5000 && tuner.isRunning(); ++guard)
            finishedByTick = tuner.tick();

        check (! tuner.isRunning() && finishedByTick, "when a step cannot be opened tick() reports that the run ended");
        check (tuner.getResult().isNotEmpty(), "and a result is available to show: " + tuner.getResult());
        check (rig.engine.deviceManager.getCurrentAudioDevice() != nullptr && rig.runningBuffer() == 64, "the device JUCE dropped is open again at the original buffer");
        check (rig.engine.getExperiments().ioCycleUsage == 0.5f && ! rig.engine.isPersistedAudioHeld(), "cycle usage is back and the hold is released");
        rig.persist();
        check (readFile() == before, "the file is unchanged after the failed run");
    }

    // Nothing to try.
    {
        Rig rig (settingsText (2048), { 2048, 4096 });
        check (rig.runningBuffer() == 2048, "the fake device opens at 2048");

        LatencyTuner tuner (rig.engine);
        tuner.start();
        check (! tuner.isRunning() && tuner.getResult().isNotEmpty() && ! rig.engine.isPersistedAudioHeld(), "a device without a usable buffer size does not start a run");
    }

    {
        Rig rig (settingsText (64));
        rig.engine.deviceManager.closeAudioDevice();
        LatencyTuner tuner (rig.engine);
        tuner.start();
        check (! tuner.isRunning() && tuner.getResult().isNotEmpty(), "no open device does not start a run");
    }

    // The device is changed underneath the run (an unplugged interface, a switch made elsewhere).
    {
        Rig rig (settingsText (64, "0.5"));
        LatencyTuner tuner (rig.engine);
        tuner.setReader ([] { return LatencyTuner::Reading {}; });
        tuner.start();

        for (int i = 0; i < 40; ++i)
            tuner.tick();

        auto setup = rig.engine.deviceManager.getAudioDeviceSetup();
        setup.inputDeviceName = "FakeIn2";
        rig.engine.deviceManager.setAudioDeviceSetup (setup, true);

        const auto ended = tuner.tick();
        check (ended && ! tuner.isRunning() && ! rig.engine.isPersistedAudioHeld(), "a device switched under the run ends it");
        check (rig.engine.deviceManager.getAudioDeviceSetup().inputDeviceName == "FakeIn2", "the new device is left alone");
        check (rig.engine.getExperiments().ioCycleUsage == 0.5f, "only the cycle usage is put back");
    }

    //==========================================================================
    // What the sweep and the measurement commands rely on to put the device back.
    {
        Rig rig (settingsText (64, "0.5"));

        {
            const ScopedSetup guard (rig.engine);
            rig.engine.setBufferSize (16);
            rig.engine.setExperiments ({ 0.25f });
            rig.engine.chain.setForceBypassAll (true);
            rig.engine.callback.setInjecting (true);
            check (rig.runningBuffer() == 16, "inside the scope the device runs the changed buffer");
        }

        check (rig.runningBuffer() == 64 && rig.engine.getExperiments().ioCycleUsage == 0.5f, "after the scope the buffer and the cycle usage are the ones from before");

        {
            const ScopedSetup guard (rig.engine);
            rig.world.refusedBuffers = { 16 };
            check (rig.engine.setBufferSize (16) == 0 && rig.engine.deviceManager.getCurrentAudioDevice() == nullptr, "a refused buffer leaves JUCE without a device");
            rig.world.refusedBuffers.clear();
        }

        check (rig.runningBuffer() == 64, "a device that a refused step closed is reopened at the original buffer by the scope");
    }

    {
        Engine engine;
        engine.openDevices (nullptr);
        engine.start();

        {
            const ScopedSetup guard (engine);
            engine.setExperiments ({ 0.25f });
        }

        check (! engine.getExperiments().ioCycleUsage.has_value() && engine.deviceManager.getCurrentAudioDevice() == nullptr,
               "without a device the scope restores the switches and opens nothing");
    }

    //==========================================================================
    // A process that dies in the middle of a tune leaves the pre-tune file.
    {
        Rig rig (settingsText (64, "0.5"));
        rig.persist();
        const auto before = readFile();

        ChildProcess child;
        const auto self = File::getSpecialLocation (File::currentExecutableFile).getFullPathName();
        const auto started = child.start (StringArray { self, "--selftest-engine-child", getSettingsFile().getFullPathName() });
        String output;
        char letter = 0;

        // One byte at a time: a larger read waits until the buffer is full or the child has exited.
        while (started && ! output.contains ("TUNING") && child.readProcessOutput (&letter, 1) == 1)
            output += String::charToString ((juce_wchar) (unsigned char) letter);

        check (started && output.contains ("TUNING"), "the child process is tuning (it reports trial values live)");
        check (child.kill() && child.waitForProcessToFinish (5000), "the child is killed (SIGKILL) in the middle of the tune");
        check (readFile() == before, "the settings file on disk is byte for byte the pre-tune file");

        Rig relaunch (*loadSettings());
        check (relaunch.runningBuffer() == 64 && relaunch.engine.getExperiments().ioCycleUsage == 0.5f, "a relaunch after the kill runs the pre-tune buffer 64 and cycle usage 0.5");
    }

    useSettingsFile ({});
    folder.deleteRecursively();

    say (check.ok ? "SELFTEST ENGINE OK" : "SELFTEST ENGINE FAILED");
    return check.ok ? 0 : 1;
}

//==============================================================================
namespace
{
    // A pass-through that takes a set time in processBlock, mono or stereo, and counts its calls.
    class BusyProcessor final : public AudioProcessor
    {
    public:
        explicit BusyProcessor (int64_t spinNanoseconds, bool mono = false, int latency = 0)
            : AudioProcessor (BusesProperties().withInput ("In", mono ? AudioChannelSet::mono() : AudioChannelSet::stereo())
                                               .withOutput ("Out", mono ? AudioChannelSet::mono() : AudioChannelSet::stereo())),
              spinNs (spinNanoseconds), onlyMono (mono)
        {
            setLatencySamples (latency);
        }

        void processBlock (juce::AudioBuffer<float>&, MidiBuffer&) override
        {
            ++blocks;

            const auto begin = std::chrono::steady_clock::now();

            while (std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now() - begin).count() < spinNs.load())
            {}
        }

        using AudioProcessor::processBlock;

        bool isBusesLayoutSupported (const BusesLayout& layouts) const override
        {
            const auto expected = onlyMono ? AudioChannelSet::mono() : AudioChannelSet::stereo();
            return layouts.getMainInputChannelSet() == expected && layouts.getMainOutputChannelSet() == expected;
        }

        const String getName() const override           { return "Busy"; }
        double getTailLengthSeconds() const override    { return 0.0; }
        bool acceptsMidi() const override               { return false; }
        bool producesMidi() const override              { return false; }
        bool hasEditor() const override                 { return false; }
        AudioProcessorEditor* createEditor() override   { return nullptr; }
        int getNumPrograms() override                   { return 1; }
        int getCurrentProgram() override                { return 0; }
        void setCurrentProgram (int) override {}
        const String getProgramName (int) override      { return {}; }
        void changeProgramName (int, const String&) override {}
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        void getStateInformation (MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

        std::atomic<int64_t> spinNs;
        std::atomic<int> blocks { 0 };

    private:
        bool onlyMono;
    };

    constexpr double loadRate = 48000.0;
    constexpr int loadBlock = 64;
    constexpr double loadPeriodNs = 1.0e9 * loadBlock / loadRate;

    void prepare (Chain& chain)
    {
        chain.getGraph().setPlayConfigDetails (1, 2, loadRate, loadBlock);
        chain.getGraph().prepareToPlay (loadRate, loadBlock);
    }

    // A constant mono signal in, so a processor that passes it on is audible as the same constant on both sides.
    void run (Chain& chain, int numBlocks, float* leftOut = nullptr, float* rightOut = nullptr)
    {
        juce::AudioBuffer<float> buffer (2, loadBlock);
        MidiBuffer midi;

        for (int block = 0; block < numBlocks; ++block)
        {
            buffer.clear();
            FloatVectorOperations::fill (buffer.getWritePointer (0), 0.25f, loadBlock);
            chain.getGraph().processBlock (buffer, midi);
        }

        if (leftOut != nullptr)
            *leftOut = buffer.getSample (0, loadBlock - 1);

        if (rightOut != nullptr)
            *rightOut = buffer.getSample (1, loadBlock - 1);
    }

    PluginDescription describe (const String& name)
    {
        PluginDescription description;
        description.name = name;
        return description;
    }
}

int selfTestProcessTime()
{
    Checker check;

    //==========================================================================
    // The hand-over from the audio thread: nothing is lost and every window is whole.
    {
        ProcessTimer timer;
        timer.record (1000);
        timer.record (1000);
        timer.record (1000);
        timer.record (5000);
        const auto first = timer.drain();
        const auto second = timer.drain();
        check (first.count == 4 && first.sumNs == 8000 && first.maxNs == 5000, "a window holds the count, the sum and the maximum (" + String (first.count) + ", " + String (first.sumNs) + ", " + String (first.maxNs) + ")");
        check (second.count == 0 && second.sumNs == 0 && second.maxNs == 0, "draining empties it");

        timer.record (-50);
        check (timer.drain().sumNs == 0, "a negative time (a clock step) counts as 0");

        // One thread records while another drains, the way the audio thread and the window do.
        ProcessTimer shared;
        constexpr int records = 400000;
        std::atomic<bool> finished { false };
        int64_t count = 0, sum = 0, worst = 0;

        std::thread audio ([&]
        {
            for (int i = 0; i < records; ++i)
                shared.record (1 + i % 100);

            finished = true;
        });

        while (! finished.load())
        {
            const auto window = shared.drain();
            count += window.count;
            sum += window.sumNs;
            worst = std::max (worst, window.maxNs);
            std::this_thread::yield();
        }

        audio.join();
        const auto last = shared.drain();
        count += last.count;
        sum += last.sumNs;
        worst = std::max (worst, last.maxNs);

        int64_t expectedSum = 0;

        for (int i = 0; i < records; ++i)
            expectedSum += 1 + i % 100;

        check (count == records && sum == expectedSum && worst == 100,
               "draining while another thread records loses nothing (" + String (count) + " of " + String (records) + " blocks, sum " + String (sum) + " of " + String (expectedSum) + ")");
    }

    //==========================================================================
    // Windows and percentages: 50 us per block on a 64-sample block at 44.1 kHz.
    {
        constexpr double period = 1.0e9 * 64.0 / 44100.0;
        ProcessWindows windows;
        check (! windows.stats (period).valid, "no block timed: no reading");

        windows.push ({ 100, 100 * 50000, 200000 });
        windows.push ({ 100, 100 * 50000, 80000 });
        const auto stats = windows.stats (period);
        check (stats.valid && stats.blocks == 200, "two windows of 100 blocks");
        check (std::abs (stats.meanPercent - 100.0 * 50000.0 / period) < 1.0e-9 && std::abs (stats.meanPercent - 3.445) < 0.001, "the mean is " + String (stats.meanPercent, 3) + " % of one buffer (50 us of 1451 us)");
        check (std::abs (stats.maxPercent - 100.0 * 200000.0 / period) < 1.0e-9 && std::abs (stats.maxPercent - 13.781) < 0.001, "the maximum is the worst block: " + String (stats.maxPercent, 3) + " %");
        check (! windows.stats (0.0).valid, "a zero buffer time gives no reading");

        for (size_t i = 0; i < ProcessWindows::numWindows; ++i)
            windows.push ({ 10, 10 * 30000, 40000 });

        const auto recent = windows.stats (period);
        check (std::abs (recent.maxPercent - 100.0 * 40000.0 / period) < 1.0e-9, "only the recent windows count: the old 200 us block has aged out");

        windows.clear();
        check (! windows.stats (period).valid, "clearing forgets everything");

        auto heavy = ProcessStats { true, 10, 5.0, 20.0 }, light = ProcessStats { true, 10, 9.0, 12.0 }, none = ProcessStats {};
        check (findHeaviest ({ light, heavy, none }) == 1, "the slot with the largest worst block is the heaviest, not the one with the larger mean");
        check (findHeaviest ({ none, none }) == -1 && findHeaviest ({}) == -1, "no reading, no heaviest");
        light.maxPercent = 20.0;
        check (findHeaviest ({ heavy, light }) == 1 && findHeaviest ({ light, heavy }) == 0, "equal worst blocks: the larger mean wins");
        check (findHeaviest ({ none, heavy, heavy }) == 1, "and a complete tie goes to the first");
    }

    //==========================================================================
    // Through the real chain and graph.
    {
        AudioPluginFormatManager formats;
        Chain chain (formats);
        prepare (chain);
        auto& graph = chain.getGraph();

        auto heavy = std::make_unique<BusyProcessor> (400000), light = std::make_unique<BusyProcessor> (30000);
        auto* heavyPlugin = heavy.get();
        auto* lightPlugin = light.get();
        chain.addProcessor (std::move (light), describe ("Light"));
        chain.addProcessor (std::move (heavy), describe ("Heavy"));

        check (graph.getNodes().size() == 2 + 2 + 4, "two slots in the path: the I/O nodes, two plugins and four probes (" + String (graph.getNodes().size()) + " nodes)");
        check (graph.getLatencySamples() == 0, "the probes add no latency");

        float left = 0.0f, right = 0.0f;
        run (chain, 300, &left, &right);
        check (std::abs (left - 0.25f) < 1.0e-6f && std::abs (right - 0.25f) < 1.0e-6f, "a mono signal still reaches both sides unchanged through the probes");

        chain.pollProcessTimes();
        const auto lightStats = chain[0].load.stats (loadPeriodNs), heavyStats = chain[1].load.stats (loadPeriodNs);
        check (lightStats.valid && heavyStats.valid && lightStats.blocks >= 290, "both slots were timed (" + String (lightStats.blocks) + " and " + String (heavyStats.blocks) + " blocks)");
        check (heavyStats.meanPercent > 100.0 * 400000.0 / loadPeriodNs - 0.01 && heavyStats.meanPercent < 100.0 * 400000.0 / loadPeriodNs * 2.0,
              "the heavy plugin's mean is its 400 us as a share of 1333 us: " + String (heavyStats.meanPercent, 2) + " % (30 % expected)");
        check (lightStats.meanPercent > 100.0 * 30000.0 / loadPeriodNs - 0.01 && lightStats.meanPercent < 12.0,
               "the light plugin's mean is its 30 us: " + String (lightStats.meanPercent, 2) + " % (2.25 % expected)");
        check (heavyStats.maxPercent >= heavyStats.meanPercent && lightStats.maxPercent >= lightStats.meanPercent, "the maximum is never below the mean");
        check (findHeaviest ({ lightStats, heavyStats }) == 1, "the slot marked heaviest is the 400 us one");

        // The time of one plugin does not leak into its neighbour's.
        check (lightStats.maxPercent < 100.0 * 400000.0 / loadPeriodNs * 0.6, "the light plugin does not carry the heavy one's time (max " + String (lightStats.maxPercent, 2) + " %)");

        // Moving a slot takes its history with it.
        chain.move (1, 0);
        check (chain[0].load.stats (loadPeriodNs).meanPercent > chain[1].load.stats (loadPeriodNs).meanPercent, "a moved slot keeps its own readings");
        chain.move (0, 1);

        // Bypass takes the plugin and its probes out of the graph.
        chain.setBypassed (1, true);
        check (graph.getNodes().size() == 2 + 2 + 2 && chain[1].probeBefore == nullptr && chain[1].probeAfter == nullptr, "a bypassed slot has no probes in the graph");
        chain.resetProcessTimes();
        const auto heavyBlocks = heavyPlugin->blocks.load(), lightBlocks = lightPlugin->blocks.load();
        run (chain, 100, &left, &right);
        chain.pollProcessTimes();
        check (! chain[1].load.stats (loadPeriodNs).valid && heavyPlugin->blocks == heavyBlocks, "a bypassed slot is neither timed nor processed");
        check (chain[0].load.stats (loadPeriodNs).valid && lightPlugin->blocks == lightBlocks + 100, "the slot in the path is still timed");
        check (std::abs (left - 0.25f) < 1.0e-6f && std::abs (right - 0.25f) < 1.0e-6f, "the signal passes with a slot out of the path");

        chain.setBypassed (1, false);
        check (graph.getNodes().size() == 8, "un-bypassed: the probes are back");

        chain.setForceBypassAll (true);
        check (graph.getNodes().size() == 4, "forced bypass of everything takes every probe out (" + String (graph.getNodes().size()) + " nodes)");
        run (chain, 10, &left, &right);
        check (std::abs (left - 0.25f) < 1.0e-6f && std::abs (right - 0.25f) < 1.0e-6f, "with everything forced out the microphone path is the input straight to the output");
        chain.setForceBypassAll (false);
        check (graph.getNodes().size() == 8, "forced bypass released: the probes are back");

        chain.remove (0);
        check (graph.getNodes().size() == 2 + 1 + 2 && chain.size() == 1, "removing a slot removes its probes");
        chain.remove (0);
        check (graph.getNodes().size() == 2, "and an empty chain is the two I/O nodes");
    }

    // A mono plugin between stereo ones, and a plugin that reports latency: routing and latency as before.
    {
        AudioPluginFormatManager formats;
        Chain chain (formats);
        prepare (chain);
        chain.addProcessor (std::make_unique<BusyProcessor> (1000), describe ("Stereo"));
        chain.addProcessor (std::make_unique<BusyProcessor> (1000, true, 0), describe ("Mono"));
        chain.addProcessor (std::make_unique<BusyProcessor> (1000, false, 184), describe ("Latent"));

        float left = 0.0f, right = 0.0f;
        run (chain, 50, &left, &right);
        check (std::abs (left - 0.25f) < 1.0e-6f && std::abs (right - 0.25f) < 1.0e-6f, "a mono plugin in the middle: the signal reaches both sides");
        check (chain.getGraph().getLatencySamples() == 184, "the plugin's own latency is the graph's latency (" + String (chain.getGraph().getLatencySamples()) + ")");

        chain.pollProcessTimes();
        check (chain[0].load.stats (loadPeriodNs).valid && chain[1].load.stats (loadPeriodNs).valid && chain[2].load.stats (loadPeriodNs).valid, "every slot, mono or not, is timed");
    }

    // A plugin with no input (a generator) cannot be wrapped: the graph would not order the probes around it.
    {
        AudioPluginFormatManager formats;
        Chain chain (formats);
        prepare (chain);
        chain.addProcessor (std::make_unique<BusyProcessor> (1000), describe ("Stereo"));
        check (chain[0].probeBefore != nullptr, "a stereo plugin with a signal is wrapped");

        chain.getGraph().setPlayConfigDetails (0, 2, loadRate, loadBlock);
        chain.getGraph().prepareToPlay (loadRate, loadBlock);
        chain.rewire();
        check (chain[0].probeBefore == nullptr && chain[0].probeAfter == nullptr, "with no input channels there is no signal to wrap: no probes, so no wrong reading");
        run (chain, 20);
        chain.pollProcessTimes();
        check (! chain[0].load.stats (loadPeriodNs).valid, "and the slot reads as not timed");
    }

    // What the probes cost: one slot of a plugin that does nothing against no slot at all.
    {
        AudioPluginFormatManager formats;
        Chain chain (formats);
        prepare (chain);
        chain.addProcessor (std::make_unique<BusyProcessor> (0), describe ("Nothing"));

        const auto timeBlocks = [&chain] (int numBlocks)
        {
            const auto begin = std::chrono::steady_clock::now();
            run (chain, numBlocks);
            return (double) std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now() - begin).count() / numBlocks;
        };

        timeBlocks (2000);
        auto with = timeBlocks (20000);

        for (int i = 0; i < 4; ++i)
            with = std::min (with, timeBlocks (20000));

        chain.setForceBypassAll (true);
        timeBlocks (2000);
        auto without = timeBlocks (20000);

        for (int i = 0; i < 4; ++i)
            without = std::min (without, timeBlocks (20000));

        const auto extra = with - without;
        say ("      one empty slot with its probes: " + String (with, 0) + " ns per block, no slot: " + String (without, 0) + " ns, difference " + String (extra, 0) + " ns ("
             + String (100.0 * extra / loadPeriodNs, 3) + " % of a 64-sample buffer)");
        check (extra < 0.01 * loadPeriodNs, "the plugin, its two probes and their graph nodes together cost under 1 % of a buffer");
    }

    say (check.ok ? "SELFTEST PROCESSTIME OK" : "SELFTEST PROCESSTIME FAILED");
    return check.ok ? 0 : 1;
}

//==============================================================================
int selfTestBreakdown()
{
    Checker check;

    const auto folder = File::getSpecialLocation (File::tempDirectory).getNonexistentChildFile ("voxrack-breakdown-selftest", {}, false);
    folder.createDirectory();
    useSettingsFile (folder.getChildFile ("settings.xml"));

    hal::Numbers numbers;
    numbers.bufferSize = 64;
    numbers.inputSafety = 5;
    numbers.outputSafety = 4;

    //==========================================================================
    {
        const auto host = declareHost (numbers, 1.0);
        check (host.total() == 137 && host.inputDevice == 5 && host.inputBuffer == 64 && host.outputBuffer == 64 && host.outputSafety == 4 && host.outputDevice == 0,
               "the HAL numbers 64 + 5 | 64 + 4 add up to 137 samples");

        auto withDevices = numbers;
        withDevices.inputLatency = 10;
        withDevices.inputStreamLatency = 7;
        withDevices.outputLatency = 3;
        withDevices.outputStreamLatency = 2;
        const auto jucesNumbers = (withDevices.inputLatency + withDevices.inputSafety + withDevices.bufferSize + withDevices.inputStreamLatency)
                                + (withDevices.outputLatency + withDevices.outputSafety + withDevices.bufferSize + withDevices.outputStreamLatency);
        check (declareHost (withDevices, 1.0).total() == jucesNumbers, "at the default cycle usage it is what JUCE adds up for its input and output latency (" + String (jucesNumbers) + ")");

        check (declareHost (numbers, 0.25).total() == 5 + 64 + 16 + 4, "a cycle usage of 0.25 counts a quarter of the output buffer");
        check (declareHost (numbers, 0.0).total() == 137 && declareHost (numbers, 2.0).total() == 137 && declareHost (numbers, std::nan ("")).total() == 137,
               "a cycle usage that is not in (0, 1] is the default");
        check (std::abs (samplesToMs (137, 44100.0) - 3.107) < 0.0005 && samplesToMs (137, 0.0) == 0.0, "137 samples at 44.1 kHz are 3.107 ms; no rate gives 0");
    }

    //==========================================================================
    {
        BreakdownInput in;
        in.sampleRate = 44100.0;
        in.aggregate = numbers;
        in.plugins = { { 0, "RX 11", 0, false }, { 1, "MAutoPitch", 1024, false }, { 2, "TDR Nova", 184, true } };

        const auto unmeasured = computeBreakdown (in);
        check (unmeasured.rows.size() == 8, "the rows: input device, input buffer, three plugins, output buffer, output device, excess (" + String ((int) unmeasured.rows.size()) + ")");
        check (unmeasured.rows[0].kind == RowKind::inputDevice && unmeasured.rows[1].kind == RowKind::inputBuffer && unmeasured.rows[2].kind == RowKind::plugin
                   && unmeasured.rows[5].kind == RowKind::outputBuffer && unmeasured.rows[6].kind == RowKind::outputDevice && unmeasured.rows[7].kind == RowKind::outputExcess,
               "in signal order");
        check (unmeasured.rows[0].samples == 5 && unmeasured.rows[1].samples == 64 && unmeasured.rows[5].samples == 68 && unmeasured.rows[6].samples == 0, "input device 5, input buffer 64, output buffer + safety 68, output device 0");
        check (unmeasured.rows[3].samples == 1024 && std::abs (unmeasured.rows[3].ms - 23.220) < 0.001, "MAutoPitch: 1024 samples = 23.220 ms");
        check (unmeasured.rows[4].bypassed && unmeasured.rows[4].samples == 0 && unmeasured.rows[4].ms == 0.0, "a bypassed plugin counts 0 even when it reports latency");
        check (unmeasured.rows[4].label.contains (ja ("バイパス")) && unmeasured.rows[3].slot == 1 && unmeasured.rows[3].label.contains ("MAutoPitch"), "its label says so; the others name the plugin");
        check (! unmeasured.rows[7].known && unmeasured.rows[7].measured && unmeasured.rows[7].ms == 0.0, "the output excess is not known before it has been measured");
        check (unmeasured.declaredSamples == 137 + 1024 && std::abs (unmeasured.declaredMs - samplesToMs (137 + 1024, 44100.0)) < 1.0e-9, "declared adds the plugins to the host's 137 samples");
        check (std::abs (unmeasured.totalMs - unmeasured.declaredMs) < 1.0e-9, "without a measurement the total is the declared part");
        check (unmeasured.largest == 3 && unmeasured.rows[3].isLargest, "the largest row is the plugin with 1024 samples");

        int marked = 0;

        for (auto& row : unmeasured.rows)
            marked += row.isLargest ? 1 : 0;

        check (marked == 1, "exactly one row carries the mark");

        // The measured excess of this Mac's Loopback device (768 samples = 17.415 ms) next to the host's declared 3.107 ms.
        BreakdownInput loopback;
        loopback.sampleRate = 44100.0;
        loopback.aggregate = numbers;
        loopback.excessMs = 17.415;
        const auto measured = computeBreakdown (loopback);
        check (measured.rows.size() == 5 && measured.rows[4].known && measured.rows[4].measured && std::abs (measured.rows[4].ms - 17.415) < 1.0e-9, "with a measurement the excess row is known and measured");
        check (measured.rows[4].samples == 768, "17.415 ms at 44.1 kHz is 768 samples");
        check (measured.declaredSamples == 137 && std::abs (measured.declaredMs - 3.107) < 0.001, "the declared part is still 137 samples = 3.107 ms");
        check (std::abs (measured.totalMs - 20.522) < 0.001, "the total is 20.522 ms, what --measure found on this Mac");
        check (measured.largest == 4 && measured.rows[4].isLargest, "the largest row is the virtual device's excess");

        // Marks follow the milliseconds, and the first of equal rows wins.
        BreakdownInput tie;
        tie.sampleRate = 48000.0;
        tie.aggregate.bufferSize = 64;
        const auto tied = computeBreakdown (tie);
        check (tied.largest == 1, "a tie goes to the first row (input buffer, 64 samples, before the output buffer, 64 samples)");

        BreakdownInput silent;
        silent.sampleRate = 48000.0;
        const auto zero = computeBreakdown (silent);
        check (zero.largest == -1 && ! zero.rows[0].isLargest, "nothing is marked when every row is 0");

        std::vector<BreakdownRow> manual (3);
        manual[0].ms = 5.0;
        manual[1].ms = 9.0;
        manual[1].known = false;
        manual[2].ms = 7.0;
        check (findLargestRow (manual) == 2, "a row that is not known is never the largest");

        // Floor of the host at cycle usage -> 0: 0 + 5 + 64 + 4 + 0 = 73 samples = 1.66 ms.
        BreakdownInput quarter = loopback;
        quarter.cycleUsage = 0.25;
        const auto low = computeBreakdown (quarter);
        check (low.declaredSamples == 5 + 64 + 16 + 4, "cycle usage 0.25: " + String (low.declaredSamples) + " samples");

        check (unmeasured.rows[1].label == ja ("入力バッファ") && unmeasured.rows[1].label.length() == 6, "labels go through ja(): six characters, not eighteen bytes read as Latin-1");
    }

    //==========================================================================
    {
        ExcessRecord record { "fxhost out", 44100.0, 17.415, 6, Time::getCurrentTime() };
        check (isUsable (record), "a complete record is usable");
        check (excessApplies (record, "fxhost out", 44100.0) && excessApplies (record, "fxhost out", 44100.3), "it applies to the same device and rate");
        check (! excessApplies (record, "fxhost out", 48000.0) && ! excessApplies (record, "Discord out", 44100.0), "not to another rate or another device");

        auto broken = record;
        broken.excessMs = std::nan ("");
        check (! isUsable (broken), "NaN is not usable");
        broken = record;
        broken.excessMs = -1.0;
        check (! isUsable (broken), "a negative excess is not usable");
        broken = record;
        broken.excessMs = 1.0e12;
        check (! isUsable (broken), "an excess of 10^12 ms is not usable");
        broken = record;
        broken.runs = 0;
        check (! isUsable (broken), "no runs is not usable");
        broken = record;
        broken.outputDevice = {};
        check (! isUsable (broken), "no device name is not usable");

        measure::Result result;
        result.sampleRate = 44100.0;
        result.trustworthyRuns = 5;
        result.excess.median = 17.4;
        const auto now = Time::getCurrentTime();
        const auto made = measure::makeExcessRecord (result, "fxhost out", now);
        check (made.has_value() && made->runs == 5 && std::abs (made->excessMs - 17.4) < 1.0e-9 && made->outputDevice == "fxhost out" && made->measuredAt == now, "a finished measurement becomes a record");
        result.trustworthyRuns = 2;
        check (! measure::makeExcessRecord (result, "fxhost out", now).has_value(), "fewer than three trusted runs make none");
        result.trustworthyRuns = 5;
        result.problem = "x";
        check (! measure::makeExcessRecord (result, "fxhost out", now).has_value(), "a failed measurement makes none");
        result.problem = {};
        result.excess.median = -0.01;
        check (measure::makeExcessRecord (result, "fxhost out", now)->excessMs == 0.0, "a median a hair below zero (noise on a device that adds nothing) is kept as 0");
    }

    //==========================================================================
    // Save and restore through the settings file, on the fake device layer (FakeOut at 48 kHz).
    {
        const auto measuredAt = Time::getCurrentTime();
        const ExcessRecord record { "FakeOut", 48000.0, 16.0, 6, measuredAt };

        {
            Rig rig (settingsText (64));
            check (! rig.engine.getExcess().has_value(), "a settings file from before this feature (no LATENCY_EXCESS) reads as not measured");
            rig.engine.setExcess (record);
            check (rig.engine.getExcess().has_value(), "a result for the open device and rate is returned");
            check (rig.persist() && readFile().contains ("LATENCY_EXCESS"), "and written");
        }

        {
            Rig relaunch (*loadSettings());
            const auto restored = relaunch.engine.getExcess();
            check (restored.has_value() && restored->outputDevice == "FakeOut" && restored->sampleRate == 48000.0 && std::abs (restored->excessMs - 16.0) < 1.0e-9 && restored->runs == 6,
                   "a relaunch on the same device and rate shows it again");
            check (restored.has_value() && std::abs ((double) restored->measuredAt.toMilliseconds() - (double) measuredAt.toMilliseconds()) < 2.0, "with the time it was measured");

            // The rate changes: not measured any more, and it stays so when the rate comes back.
            auto setup = relaunch.engine.deviceManager.getAudioDeviceSetup();
            setup.sampleRate = 44100.0;
            relaunch.engine.openWithSetup (setup, true);
            check (relaunch.runningBuffer() > 0 && ! relaunch.engine.getExcess().has_value(), "at another sample rate it is not measured");
            check (relaunch.persist() && ! readFile().contains ("LATENCY_EXCESS"), "and the file no longer holds it");
            setup.sampleRate = 48000.0;
            relaunch.engine.openWithSetup (setup, true);
            check (! relaunch.engine.getExcess().has_value(), "switching back does not bring an old measurement back");
        }

        {
            Rig again (*loadSettings());
            check (! again.engine.getExcess().has_value(), "the next launch has none either");
        }

        // A result for another output device.
        {
            Rig rig (settingsText (64));
            auto other = record;
            other.outputDevice = "Discord out";
            rig.engine.setExcess (other);
            check (! rig.engine.getExcess().has_value(), "a result for another output device is not returned");
            check (rig.persist() && ! readFile().contains ("LATENCY_EXCESS"), "and never written");
        }

        // A device that is missing at launch must not cost the saved result.
        {
            Rig rig (settingsText (64));
            rig.engine.setExcess (record);
            rig.persist();
            auto xml = loadSettings();
            xml->getChildByName ("DEVICESETUP")->setAttribute ("audioOutputDeviceName", "NotThere");
            Rig missing (*xml);
            check (missing.engine.deviceManager.getCurrentAudioDevice() == nullptr, "the output device named in the file is not there: nothing is open");
            check (missing.engine.getExcess().has_value(), "the saved measurement is kept until a device can be judged");
            check (missing.persist() && readFile().contains ("LATENCY_EXCESS"), "and written again");
        }

        // Damaged values are ignored and the rest of the file still loads.
        for (const auto* attributes : { "outputDevice=\"FakeOut\" sampleRate=\"48000\" excessMs=\"nan\" runs=\"6\" measuredAt=\"\"",
                                        "outputDevice=\"FakeOut\" sampleRate=\"0\" excessMs=\"16\" runs=\"6\" measuredAt=\"\"",
                                        "outputDevice=\"FakeOut\" sampleRate=\"48000\" excessMs=\"-4\" runs=\"6\" measuredAt=\"\"",
                                        "outputDevice=\"\" sampleRate=\"48000\" excessMs=\"16\" runs=\"6\" measuredAt=\"\"",
                                        "outputDevice=\"FakeOut\" sampleRate=\"48000\" excessMs=\"16\" runs=\"0\" measuredAt=\"\"",
                                        "" })
        {
            auto text = settingsText (64);
            text = text.replace ("<CHAIN/>", "<LATENCY_EXCESS " + String (attributes) + "/><CHAIN/>");
            Rig rig (text);
            check (rig.runningBuffer() == 64 && ! rig.engine.getExcess().has_value(), String ("damaged LATENCY_EXCESS (") + attributes + ") reads as not measured and the file still loads");
        }

        // The measurement session on a device layer that has no HAL aggregate: it says why, in Japanese, and changes nothing.
        {
            Rig rig (settingsText (64));
            measure::Session session (rig.engine, {});
            check (! session.start() && ! session.isRunning(), "a session cannot start without the aggregate device");
            check (session.getResult().problem.isNotEmpty() && session.getResult().problemJa.isNotEmpty() && (size_t) session.getResult().problemJa.length() < session.getResult().problemJa.getNumBytesAsUTF8(),
                   "it names the problem in English and in Japanese: " + session.getResult().problemJa);
            check (rig.runningBuffer() == 64 && ! session.tick() && session.getProgress().isEmpty(), "the device is as it was and a tick does nothing");

            rig.engine.deviceManager.closeAudioDevice();
            measure::Session none (rig.engine, {});
            check (! none.start() && none.getResult().problem == "no audio device is open", "and not without any device");
        }
    }

    useSettingsFile ({});
    folder.deleteRecursively();

    say (check.ok ? "SELFTEST BREAKDOWN OK" : "SELFTEST BREAKDOWN FAILED");
    return check.ok ? 0 : 1;
}

} // namespace mfx
