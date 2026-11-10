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
#include <functional>
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
        explicit Rig (const XmlElement& settings, const Array<int>& bufferSizes = {}, const std::function<void (test::FakeWorld&)>& prepare = {})
        {
            if (! bufferSizes.isEmpty())
                world.bufferSizes = bufferSizes;

            if (prepare)
                prepare (world);

            engine.useDeviceType (std::make_unique<test::FakeDeviceType> (world), std::make_unique<test::FakeMonitorBackend> (world));
            engine.openFromSettings (&settings);
            engine.start();
        }

        explicit Rig (const String& settings, const Array<int>& bufferSizes = {}, const std::function<void (test::FakeWorld&)>& prepare = {})
            : Rig (*XmlDocument::parse (settings), bufferSizes, prepare) {}

        int runningBuffer() const
        {
            auto* device = engine.deviceManager.getCurrentAudioDevice();
            return device != nullptr ? device->getCurrentBufferSizeSamples() : 0;
        }

        // What the window's persist() writes.
        bool persist() const { return saveSettings (*buildSettingsXml ("rack84", engine)); }

        test::FakeWorld world;
        Engine engine { false };
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

    const auto folder = File::getSpecialLocation (File::tempDirectory).getNonexistentChildFile ("wirevox-engine-selftest", {}, false);
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

    const auto folder = File::getSpecialLocation (File::tempDirectory).getNonexistentChildFile ("wirevox-breakdown-selftest", {}, false);
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

        // The readout's three lines come from the same rows, so they follow the cycle usage too.
        const auto sides = sumSides (unmeasured);
        check (sides.input == 69 && sides.plugins == 1024 && sides.output == 68, "sides: input 5 + 64, plugins 1024 (the bypassed one counts 0), output 64 + 4");
        check (sides.input + sides.plugins + sides.output == unmeasured.declaredSamples, "the sides add up to the declared total");
        const auto lowSides = sumSides (low);
        check (lowSides.output == 16 + 4 && lowSides.input == 69 && lowSides.plugins == 0, "at cycle usage 0.25 the output side is 16 + 4, and the measured excess is in no side");

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
        result.excess.median = -3.0;
        check (! measure::makeExcessRecord (result, "fxhost out", now).has_value(), "a clearly negative median (wrong timing) makes none");
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

//==============================================================================
namespace
{
    // The device setup of the fake rig (FakeIn mono, FakeOut at 48 kHz, 64 samples) with a MONITOR element of the test's choosing.
    String monitorSettings (const String& monitorElement, const String& outputChannels = "11")
    {
        return "<MIC_FX_HOST theme=\"rack84\"><DEVICESETUP deviceType=\"Fake\" audioOutputDeviceName=\"FakeOut\" audioInputDeviceName=\"FakeIn\""
               " audioDeviceRate=\"48000.0\" audioDeviceBufferSize=\"64\" audioDeviceInChans=\"1\" audioDeviceOutChans=\"" + outputChannels + "\"/>"
             + monitorElement + "<CHAIN/></MIC_FX_HOST>";
    }

    // FakeOut (the main output, 2 channels) and FakeMon (the monitor device, 8 channels: 7/8 is the last pair).
    void monitorWorld (test::FakeWorld& world)
    {
        world.outputNames = { "FakeOut", "FakeMon" };
        world.outputChannels["FakeMon"] = 8;
    }

    // Halves what passes through, so the output of a processed chain differs from the microphone.
    class HalvingProcessor final : public AudioProcessor
    {
    public:
        HalvingProcessor()
            : AudioProcessor (BusesProperties().withInput ("In", AudioChannelSet::stereo()).withOutput ("Out", AudioChannelSet::stereo())) {}

        void processBlock (juce::AudioBuffer<float>& buffer, MidiBuffer&) override  { buffer.applyGain (0.5f); }
        using AudioProcessor::processBlock;

        const String getName() const override           { return "Halving"; }
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
    };

    bool allEqual (const std::vector<float>& block, float value)
    {
        return ! block.empty() && std::all_of (block.begin(), block.end(), [value] (float v) { return std::abs (v - value) < 1.0e-6f; });
    }

    bool sameBlock (const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::equal (a.begin(), a.end(), b.begin());
    }

    MonitorPlan planOf (const String& element)
    {
        return readMonitor (*XmlDocument::parse ("<MIC_FX_HOST>" + element + "</MIC_FX_HOST>"));
    }
}

int selfTestMonitor()
{
    Checker check;

    //==========================================================================
    // Settings: round trip, files from before the monitor, damaged values.
    {
        const XmlElement none ("MIC_FX_HOST");
        const auto absent = readMonitor (none);
        check (! absent.enabled && absent.device.isEmpty() && absent.firstChannel == 0, "a settings file without a MONITOR element loads as monitor off");

        for (const auto& plan : { MonitorPlan { true, "Discrete4_Pro_SC", 30 }, MonitorPlan { false, "FakeMon", 4 }, MonitorPlan { true, "Dev \"quoted\" & <x>", 0 } })
        {
            XmlElement root ("MIC_FX_HOST");
            writeMonitor (root, plan);
            const auto reparsed = XmlDocument::parse (root.toString());
            check (reparsed != nullptr && readMonitor (*reparsed) == plan, "a monitor of '" + plan.device + "' at channel " + String (plan.firstChannel) + " round-trips through the XML text");
        }

        check (planOf ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>") == MonitorPlan { true, "FakeMon", 6 }, "a good element is read as written");
        const auto keptOff = planOf ("<MONITOR enabled=\"0\" device=\"FakeMon\" firstChannel=\"4\"/>");
        check (! keptOff.enabled && keptOff.device == "FakeMon" && keptOff.firstChannel == 4, "an element that is off keeps its device and channel");

        for (const auto* attributes : { "enabled=\"1\" device=\"\" firstChannel=\"2\"", "enabled=\"1\" firstChannel=\"2\"",
                                        "enabled=\"1\" device=\"FakeMon\" firstChannel=\"-2\"", "enabled=\"1\" device=\"FakeMon\" firstChannel=\"abc\"",
                                        "enabled=\"1\" device=\"FakeMon\" firstChannel=\"2.5\"", "enabled=\"1\" device=\"FakeMon\" firstChannel=\"\"",
                                        "enabled=\"1\" device=\"FakeMon\"", "enabled=\"1\" device=\"FakeMon\" firstChannel=\"99999999999\"",
                                        "enabled=\"nope\" device=\"FakeMon\" firstChannel=\"2\"", "device=\"FakeMon\" firstChannel=\"2\"" })
        {
            const auto plan = planOf (String ("<MONITOR ") + attributes + "/>");
            check (! plan.enabled, String ("a damaged element (") + attributes + ") loads as off");
        }
    }

    //==========================================================================
    // Pairs and routing: pure functions of the channel layout.
    {
        const MonitorLayout two { 0, 2, 0, 8 };
        check (isValidPair (two, 0) && isValidPair (two, 6) && ! isValidPair (two, 7) && ! isValidPair (two, 8) && ! isValidPair (two, -1),
               "a pair is valid from channel 0 to the second to last, never at the last, past the end or below 0");
        check (! isValidPair ({ 0, 2, 0, 1 }, 0) && ! isValidPair ({ 0, 2, 0, 0 }, 0) && isValidPair ({ 0, 2, 0, 2 }, 0),
               "a monitor device with 0 or 1 output channels has no pair; one with 2 has exactly one");

        BigInteger both, only1, none;
        both.setRange (0, 2, true);
        only1.setBit (1);

        auto routing = planRouting (both, { 0, 2, 2, 4 }, 2);
        check (routing.innerOutputs.toInteger() == 0b110011 && routing.visibleOutputs.toInteger() == 0b11, "main 1/2 and monitor 3/4 of FakeMon: the device opens channels 0, 1, 4, 5 and the callback sees 0, 1");
        check (routing.mainSlots == std::vector<int> { 0, 1 } && routing.monitorSlots[0] == 2 && routing.monitorSlots[1] == 3, "the callback finds the main pair in slots 0 and 1 and the monitor pair in 2 and 3");

        routing = planRouting (only1, { 0, 2, 2, 4 }, 0);
        check (routing.mainSlots == std::vector<int> { 0 } && routing.monitorSlots[0] == 1 && routing.monitorSlots[1] == 2 && routing.visibleOutputs.toInteger() == 0b10,
               "only main channel 2 active: one main slot, then the monitor pair");

        routing = planRouting (none, { 0, 2, 2, 4 }, 0);
        check (routing.mainSlots.empty() && routing.monitorSlots[0] == 0 && routing.monitorSlots[1] == 1 && routing.visibleOutputs.isZero(), "no main channel active: the monitor pair is all the device carries");

        routing = planRouting (both, { 4, 2, 0, 4 }, 2);
        check (routing.innerOutputs.toInteger() == 0b111100 && routing.monitorSlots[0] == 0 && routing.monitorSlots[1] == 1 && routing.mainSlots == std::vector<int> { 2, 3 },
               "the aggregate with the monitor device's channels first: the monitor pair is found in slots 0 and 1, the main pair after it");

        BigInteger eight, five, bad;
        eight.setRange (0, 8, true);
        five.setBit (5);
        five.setBit (6);
        routing = planRouting (eight, { 0, 8, 0, 8 }, 6);
        check (routing.visibleOutputs.toInteger() == 0b00111111 && routing.mainSlots.size() == 6 && routing.monitorSlots[0] == 6 && routing.monitorSlots[1] == 7,
               "on one device, a main channel that is also in the monitor pair is dropped from the main output");
        routing = planRouting (five, { 0, 8, 0, 8 }, 6);
        check (routing.visibleOutputs.toInteger() == 0b100000 && routing.mainSlots == std::vector<int> { 0 } && routing.monitorSlots[0] == 1 && routing.monitorSlots[1] == 2,
               "on one device with main channels 6 and 7 wanted and the pair at 7/8: only 6 stays");

        bad.setRange (0, 64, true);
        routing = planRouting (bad, { 0, 2, 2, 4 }, 2);
        check (routing.visibleOutputs.toInteger() == 0b11 && routing.innerOutputs.countNumberOfSetBits() == 4, "main channels beyond the main device's own are ignored");

        routing = planRouting (both, { 0, 2, 2, 4 }, 3);
        check (routing.monitorSlots[0] == -1 && routing.monitorSlots[1] == -1 && routing.innerOutputs.toInteger() == 0b11, "a pair that does not fit adds no channel at all");
        routing = planRouting (both, { 0, 2, 2, 4 }, -1);
        check (routing.monitorSlots[0] == -1 && routing.innerOutputs.toInteger() == 0b11, "a negative first channel adds no channel and touches no bit");
    }

    //==========================================================================
    // The copy.
    {
        const int mainSlots[] = { 0, 1 }, monitorSlots[] = { 2, 3 }, mono[] = { 0 };
        std::vector<std::vector<float>> channels (4, std::vector<float> (8, 7.0f));
        float* pointers[] = { channels[0].data(), channels[1].data(), channels[2].data(), channels[3].data() };

        std::fill (channels[0].begin(), channels[0].end(), 0.25f);
        std::fill (channels[1].begin(), channels[1].end(), -0.5f);
        copyToMonitor (pointers, mainSlots, 2, monitorSlots, 8);
        check (allEqual (channels[2], 0.25f) && allEqual (channels[3], -0.5f) && allEqual (channels[0], 0.25f) && allEqual (channels[1], -0.5f), "the monitor pair gets the main pair, left to left and right to right, and the main pair is not touched");

        std::fill (channels[2].begin(), channels[2].end(), 7.0f);
        std::fill (channels[3].begin(), channels[3].end(), 7.0f);
        copyToMonitor (pointers, mono, 1, monitorSlots, 8);
        check (allEqual (channels[2], 0.25f) && allEqual (channels[3], 0.25f), "a mono main output goes to both monitor channels");

        copyToMonitor (pointers, mainSlots, 0, monitorSlots, 8);
        check (allEqual (channels[2], 0.0f) && allEqual (channels[3], 0.0f), "no main channel: the monitor is silent, not left with whatever the buffer held");

        std::fill (channels[2].begin(), channels[2].end(), 7.0f);
        const int absent[] = { -1, -1 };
        copyToMonitor (pointers, mainSlots, 2, absent, 8);
        copyToMonitor (pointers, mainSlots, 2, monitorSlots, 0);
        check (allEqual (channels[2], 7.0f), "no monitor slot, or no samples: nothing is written");
    }

    {
        const auto text = describeRateMismatch (48000.0, 48000.0, 48000.0, 44100.0);
        check (text.contains ("44100") && text.contains (ja ("モニター")) && describeRateMismatch (48000.0, 48000.0, 48000.0, 48000.0).isEmpty() && describeRateMismatch (48000.0, 0.0, 0.0, 0.0).isEmpty(),
               "a monitor device at another rate is named in the rate warning; equal or unknown rates give none");
    }

    //==========================================================================
    // The layer is the engine's only device type from the moment it exists, so a device panel built before the
    // first open (which asks the manager for its types) cannot make JUCE add a plain CoreAudio type next to it.
    // This reads the system's device list (names only); nothing is opened.
    {
        Engine engine;
        const auto& types = engine.deviceManager.getAvailableDeviceTypes();
        check (types.size() == 1 && dynamic_cast<MonitorDeviceType*> (types[0]) != nullptr && types[0]->getTypeName() == "CoreAudio",
               "a default Engine has exactly one device type, the monitor layer in front of CoreAudio, under CoreAudio's name (" + String (types.size()) + " type(s))");
    }

    //==========================================================================
    // Off by default: nothing is made, and the device is CoreAudio's own kind of object, untouched.
    {
        Rig rig (monitorSettings ({}), {}, monitorWorld);
        auto* device = rig.engine.deviceManager.getCurrentAudioDevice();
        check (device != nullptr && dynamic_cast<test::FakeDevice*> (device) != nullptr, "with no MONITOR element the device is the layer's own, not wrapped");
        check (rig.world.aggregatesCreated == 0 && rig.world.devicesAlive == 1 && ! rig.engine.isMonitorActive() && ! rig.engine.getMonitorPlan().enabled,
               "no aggregate is created and one device is open");
        check (rig.engine.takeMonitorProblem().isEmpty(), "and there is nothing to report");

        const auto saved = buildSettingsXml ("rack84", rig.engine);
        check (saved->getChildByName ("MONITOR") == nullptr, "a monitor that was never chosen is not written");
    }

    // Same, for a plan that is off in the file but names a device.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"0\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, monitorWorld);
        check (rig.world.aggregatesCreated == 0 && ! rig.engine.isMonitorActive(), "a monitor that is saved as off creates nothing at launch");
        const auto saved = buildSettingsXml ("rack84", rig.engine);
        const auto* element = saved->getChildByName ("MONITOR");
        check (element != nullptr && element->getStringAttribute ("device") == "FakeMon" && element->getIntAttribute ("firstChannel") == 6 && element->getIntAttribute ("enabled") == 0,
               "and the choice is written back as it was");
    }

    //==========================================================================
    // On: the aggregate exists, JUCE sees only the main output, everything keyed by the output name keeps the main name.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, monitorWorld);
        auto& engine = rig.engine;
        auto& manager = engine.deviceManager;
        auto* device = manager.getCurrentAudioDevice();

        check (engine.isMonitorActive() && rig.world.aggregatesCreated == 1 && rig.world.aggregatesAlive == 1, "a monitor saved as on is running after launch, on one aggregate");
        check (device != nullptr && device->getName() == "FakeOut" && device->getTypeName() == "Fake", "the open device is called by the main output's name, and keeps the type name");
        check (manager.getAudioDeviceSetup().outputDeviceName == "FakeOut" && manager.getAudioDeviceSetup().inputDeviceName == "FakeIn", "the device setup holds the main output's name");

        const auto names = device->getOutputChannelNames();
        check (names.size() == 2 && names[0] == "out 1" && names[1] == "out 2", "the channel list is the main device's two channels, without the aggregate's sub-device suffix");
        check (device->getActiveOutputChannels().toInteger() == 0b11 && manager.getAudioDeviceSetup().outputChannels.toInteger() == 0b11, "the active output channels are the main output's");
        check (device->getCurrentSampleRate() == 48000.0 && device->getCurrentBufferSizeSamples() == 64, "rate and buffer size pass through");

        auto* type = manager.getCurrentDeviceTypeObject();
        check (type != nullptr && type->getDeviceNames (false) == StringArray ("FakeOut", "FakeMon") && type->getDeviceNames (false).indexOf (device->getName()) == 0,
               "the device list does not show the aggregate, though it exists (" + String (rig.world.outputNames.size()) + " output names in the layer)");
        check (type != nullptr && type->getIndexOfDevice (device, false) == 0, "the type finds the open device in its list by the main name");

        const auto audio = engine.getPersistedAudio();
        check (audio.device != nullptr && audio.device->getStringAttribute ("audioOutputDeviceName") == "FakeOut" && audio.device->getStringAttribute ("audioDeviceOutChans") == "11",
               "createStateXml (what is saved as DEVICESETUP) holds the main output's name and channels");

        engine.setExcess ({ "FakeOut", 48000.0, 16.0, 6, Time::getCurrentTime() });
        check (engine.getExcess().has_value() && engine.getExcess()->outputDevice == "FakeOut", "the measured excess, keyed by the output name and rate, still applies with the monitor on");

        const auto saved = buildSettingsXml ("rack84", engine);
        const auto* monitorElement = saved->getChildByName ("MONITOR");
        check (monitorElement != nullptr && monitorElement->getIntAttribute ("enabled") == 1 && monitorElement->getStringAttribute ("device") == "FakeMon" && monitorElement->getIntAttribute ("firstChannel") == 6,
               "the settings file holds the monitor choice");
        check (saved->getChildByName ("LATENCY_EXCESS") != nullptr && saved->getChildByName ("LATENCY_EXCESS")->getStringAttribute ("outputDevice") == "FakeOut", "and the excess record under the main output's name");
        check (! saved->toString().contains (MonitorDeviceType::aggregateNamePrefix), "the aggregate's name is nowhere in the saved settings");

        // The tuner's own "device unchanged" check compares the setup's names; a whole run must not be cut short by it.
        LatencyTuner tuner (engine);
        setReaderUncleanAtOrBelow (tuner, rig, 32);
        tuner.start();
        int writes = 0;
        bool differed = false;
        const auto before = String();
        const auto ended = tickToEnd (tuner, rig, before, writes, differed);
        check (ended && tuner.getResult().startsWith (ja ("最小遅延")), "MIN LATENCY runs to its end with the monitor on (" + tuner.getResult() + ")");
        check (rig.runningBuffer() == 64 && engine.isMonitorActive() && rig.world.devicesAlive == 1 && rig.world.aggregatesAlive == 1,
               "and the monitor is still running on the same aggregate afterwards (buffer " + String (rig.runningBuffer()) + ", " + String (rig.world.devicesAlive) + " device, " + String (rig.world.aggregatesAlive) + " aggregate)");
        check (rig.world.aggregatesCreated == 1, "buffer changes reopen the device, not the aggregate");
    }

    //==========================================================================
    // The callback: the monitor carries what the main output carries.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, monitorWorld);
        auto& engine = rig.engine;
        auto* inner = rig.world.lastDevice;
        check (inner != nullptr && engine.isMonitorActive(), "the underlying device is running");

        const auto cycle = [&inner] (float mic) { return inner->pump (64, mic); };

        auto out = cycle (0.25f);
        check (out.size() == 4 && allEqual (out[0], 0.25f) && allEqual (out[1], 0.25f), "an empty chain passes the microphone to both main channels");
        check (allEqual (out[2], 0.25f) && allEqual (out[3], 0.25f) && sameBlock (out[2], out[0]) && sameBlock (out[3], out[1]), "and the monitor pair carries the same");

        engine.chain.addProcessor (std::make_unique<HalvingProcessor>(), describe ("Halving"));
        engine.chain.rewire();
        out = cycle (0.25f);
        check (allEqual (out[0], 0.125f) && allEqual (out[1], 0.125f) && sameBlock (out[2], out[0]) && sameBlock (out[3], out[1]), "through a plugin the monitor carries the processed signal (0.125), not the microphone");

        engine.chain.setBypassed (0, true);
        out = cycle (0.25f);
        check (allEqual (out[0], 0.25f) && sameBlock (out[2], out[0]) && sameBlock (out[3], out[1]), "with the slot bypassed the monitor carries what the main output carries (0.25)");

        engine.chain.setBypassed (0, false);
        engine.chain.setForceBypassAll (true);
        out = cycle (0.25f);
        check (allEqual (out[0], 0.25f) && allEqual (out[1], 0.25f) && sameBlock (out[2], out[0]) && sameBlock (out[3], out[1]), "with the hard bypass of a measurement the monitor carries what the main output carries");
        engine.chain.setForceBypassAll (false);

        out = cycle (0.0f);
        check (allEqual (out[2], 0.0f) && allEqual (out[3], 0.0f), "a silent main output is a silent monitor");
        check (std::none_of (out.begin(), out.end(), [] (const std::vector<float>& channel) { return std::any_of (channel.begin(), channel.end(), [] (float v) { return v == 7.0f; }); }),
               "no output channel is left as the device handed it over");
    }

    // The monitor device's channels first in the aggregate.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"2\"/>"), {}, [] (test::FakeWorld& world)
        {
            monitorWorld (world);
            world.outputChannels["FakeMon"] = 4;
            world.monitorBeforeMain = true;
        });
        auto out = rig.world.lastDevice->pump (64, 0.5f);
        check (rig.engine.isMonitorActive() && out.size() == 4 && allEqual (out[0], 0.5f) && allEqual (out[1], 0.5f) && allEqual (out[2], 0.5f) && allEqual (out[3], 0.5f),
               "when the aggregate lists the monitor device first, the copy still lands on the monitor pair and the main pair is the one processed");
    }

    // Channel counts: mono main, no main channel, one device.
    {
        Rig mono (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"0\"/>", "1"), {}, monitorWorld);
        auto out = mono.world.lastDevice->pump (64, 0.25f);
        check (mono.engine.isMonitorActive() && out.size() == 3 && allEqual (out[0], 0.25f) && allEqual (out[1], 0.25f) && allEqual (out[2], 0.25f),
               "one main channel active: it is copied to both monitor channels");

        Rig silent (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"0\"/>", "0"), {}, monitorWorld);
        out = silent.world.lastDevice->pump (64, 0.25f);
        check (silent.engine.isMonitorActive() && out.size() == 2 && allEqual (out[0], 0.0f) && allEqual (out[1], 0.0f), "no main channel active: the monitor is silent");

        Rig same (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeOut\" firstChannel=\"6\"/>", "111111"), {}, [] (test::FakeWorld& world)
        {
            world.outputChannels["FakeOut"] = 8;
        });
        auto* device = same.engine.deviceManager.getCurrentAudioDevice();
        out = same.world.lastDevice->pump (64, 0.25f);
        check (same.engine.isMonitorActive() && same.world.aggregatesCreated == 0 && same.world.devicesAlive == 1, "the monitor on the main device itself makes no aggregate");
        check (out.size() == 8 && allEqual (out[0], 0.25f) && allEqual (out[6], 0.25f) && allEqual (out[7], 0.25f),
               "and the pair 7/8 of that device carries the main output");
        check (device->getActiveOutputChannels().toInteger() == 0b00111111 && same.engine.deviceManager.getAudioDeviceSetup().outputChannels.toInteger() == 0b00111111,
               "the main channels are the user's (" + String (device->getActiveOutputChannels().toInteger()) + ")");
        check (device->getOutputChannelNames()[6].endsWith (ja ("[モニター]")) && device->getOutputChannelNames()[7].endsWith (ja ("[モニター]")) && device->getOutputChannelNames().size() == 8,
               "the channel list says which two channels are the monitor's");

        // A pair that overlaps the user's main channels is refused; their channel choice is never narrowed.
        Rig overlap (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeOut\" firstChannel=\"6\"/>", "11111111"), {}, [] (test::FakeWorld& world)
        {
            world.outputChannels["FakeOut"] = 8;
        });
        const auto problem = overlap.engine.takeMonitorProblem();
        check (! overlap.engine.isMonitorActive() && ! overlap.engine.getMonitorPlan().enabled && problem.contains (ja ("重なって")) && overlap.engine.takeMonitorProblem().isEmpty(),
               "an overlapping pair on one device leaves the monitor off, with the reason once (" + problem + ")");
        const auto kept = overlap.engine.getPersistedAudio();
        check (overlap.engine.deviceManager.getCurrentAudioDevice() != nullptr && kept.device != nullptr && kept.device->getStringAttribute ("audioDeviceOutChans") == "11111111",
               "and the main output opens with all eight channels, as saved");
    }

    //==========================================================================
    // Turning it on and off at run time, and what is created and destroyed.
    {
        Rig rig (monitorSettings ({}), {}, monitorWorld);
        auto& engine = rig.engine;

        engine.setMonitorPlan ({ true, "FakeMon", 6 });
        check (engine.isMonitorActive() && rig.world.aggregatesCreated == 1 && rig.world.aggregatesAlive == 1 && rig.world.devicesAlive == 1,
               "turning it on creates one aggregate and reopens the device on it (one device is open, the old one is gone)");
        check (engine.getMonitorPlan() == MonitorPlan { true, "FakeMon", 6 }, "the plan reads back as set");

        const auto opens = rig.world.opens;
        engine.setMonitorPlan ({ true, "FakeMon", 6 });
        check (rig.world.opens == opens && rig.world.aggregatesCreated == 1, "setting the same plan again changes nothing");

        engine.setMonitorPlan ({ true, "FakeMon", 4 });
        check (engine.isMonitorActive() && rig.world.aggregatesCreated == 2 && rig.world.aggregatesAlive == 1, "another pair replaces the aggregate rather than adding one");

        engine.setMonitorPlan ({ false, "FakeMon", 4 });
        check (! engine.isMonitorActive() && rig.world.aggregatesAlive == 0 && rig.world.devicesAlive == 1 && dynamic_cast<test::FakeDevice*> (engine.deviceManager.getCurrentAudioDevice()) != nullptr,
               "turning it off destroys the aggregate and leaves the plain device");
        check (engine.getMonitorPlan() == MonitorPlan { false, "FakeMon", 4 }, "and the choice is kept");

        engine.setMonitorPlan ({ true, "FakeMon", 6 });
        check (rig.world.aggregatesAlive == 1, "on again");
        engine.deviceManager.closeAudioDevice();
        check (rig.world.aggregatesAlive == 0 && rig.world.devicesAlive == 0, "closing the device, which is what quitting does, destroys the aggregate");
    }

    // A device that is changed in the panel: the aggregate follows the new main output.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, [] (test::FakeWorld& world)
        {
            monitorWorld (world);
            world.outputNames.add ("FakeOut2");
        });
        auto setup = rig.engine.deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName = "FakeOut2";
        const auto error = rig.engine.deviceManager.setAudioDeviceSetup (setup, true);
        check (error.isEmpty() && rig.engine.isMonitorActive() && rig.world.aggregatesCreated == 2 && rig.world.aggregatesAlive == 1, "choosing another main output builds a new aggregate and destroys the old one");
        check (rig.engine.deviceManager.getAudioDeviceSetup().outputDeviceName == "FakeOut2" && rig.engine.deviceManager.getCurrentAudioDevice()->getName() == "FakeOut2", "and the name is the new main output's");
    }

    //==========================================================================
    // Failures: the main output carries on alone, the monitor is off, there is a reason, and no aggregate is left.
    {
        const auto fallsBack = [] (const String& description, const String& element, const std::function<void (test::FakeWorld&)>& prepare, bool keepsIntent = false)
        {
            Rig rig (monitorSettings (element), {}, [&prepare] (test::FakeWorld& world) { monitorWorld (world); if (prepare) prepare (world); });
            auto& engine = rig.engine;
            const auto plan = engine.getMonitorPlan();
            const auto problem = engine.takeMonitorProblem();
            auto* device = engine.deviceManager.getCurrentAudioDevice();

            Checker local;
            local (device != nullptr && device->getName() == "FakeOut" && rig.world.devicesAlive == 1, description + ": the main output is open");
            local (! engine.isMonitorActive() && plan.enabled == keepsIntent, description + (keepsIntent ? ": the monitor waits and the choice stays on" : ": the monitor is off"));
            local (problem.isNotEmpty() && engine.takeMonitorProblem().isEmpty(), description + ": a reason is reported, once (" + problem + ")");
            local (rig.world.aggregatesAlive == 0, description + ": no aggregate is left");
            local (engine.deviceManager.getAudioDeviceSetup().outputChannels.toInteger() == 0b11 && rig.runningBuffer() == 64, description + ": the device setup is as it was saved");

            const auto saved = buildSettingsXml ("rack84", engine);
            local (saved->getChildByName ("DEVICESETUP") != nullptr && saved->getChildByName ("DEVICESETUP")->getStringAttribute ("audioOutputDeviceName") == "FakeOut"
                       && saved->getChildByName ("MONITOR") != nullptr && saved->getChildByName ("MONITOR")->getIntAttribute ("enabled") == (keepsIntent ? 1 : 0),
                   description + (keepsIntent ? ": the next save still says on" : ": the next save says off") + " and keeps the main output");

            auto out = rig.world.lastDevice != nullptr ? rig.world.lastDevice->pump (64, 0.25f) : std::vector<std::vector<float>>();
            local (out.size() == 2 && allEqual (out[0], 0.25f) && allEqual (out[1], 0.25f), description + ": the main output still plays");
            return local.ok;
        };

        check (fallsBack ("an unknown device", "<MONITOR enabled=\"1\" device=\"Nope\" firstChannel=\"0\"/>", {}, true), "a saved monitor device that is not there waits, and the saved choice stays on");
        check (fallsBack ("a channel past the end", "<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"8\"/>", {}), "a first channel past the device's last falls back to off");
        check (fallsBack ("the last channel alone", "<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"7\"/>", {}), "a pair that would start on the device's last channel falls back to off");
        check (fallsBack ("a one-channel device", "<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"0\"/>", [] (test::FakeWorld& world) { world.outputChannels["FakeMon"] = 1; }),
               "a monitor device with a single channel falls back to off");
        check (fallsBack ("a refused aggregate", "<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>", [] (test::FakeWorld& world) { world.failAggregate = true; }), "an aggregate the HAL refuses falls back to off");
        check (fallsBack ("an unlisted aggregate", "<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>", [] (test::FakeWorld& world) { world.aggregatesListed = false; }),
               "an aggregate that JUCE's device scan does not list falls back to off, and is destroyed");

        Rig damaged (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"-3\"/>"), {}, monitorWorld);
        check (! damaged.engine.isMonitorActive() && damaged.world.aggregatesCreated == 0 && damaged.world.devicesAlive == 1
                   && damaged.engine.takeMonitorProblem().isEmpty() && damaged.engine.deviceManager.getAudioDeviceSetup().outputDeviceName == "FakeOut",
               "a damaged element turns nothing on and creates nothing");
    }

    // The monitor device goes away while running.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, monitorWorld);
        auto& engine = rig.engine;
        check (engine.isMonitorActive() && ! engine.checkMonitor(), "with the device there, checking leaves the monitor running");

        rig.world.outputNames.removeString ("FakeMon");
        check (engine.checkMonitor(), "the monitor device is gone: the monitor is dropped");
        check (! engine.isMonitorActive() && engine.getMonitorPlan() == MonitorPlan { true, "FakeMon", 6 }, "it is not running and the choice is kept as on");
        check (rig.world.aggregatesAlive == 0 && rig.world.devicesAlive == 1 && engine.deviceManager.getCurrentAudioDevice() != nullptr && engine.deviceManager.getCurrentAudioDevice()->getName() == "FakeOut",
               "the aggregate is destroyed and the main output is open");
        check (engine.takeMonitorProblem().isNotEmpty() && ! engine.checkMonitor(), "the reason is reported once, and a second check finds nothing to do");

        check (! engine.retryMonitor() && rig.world.aggregatesCreated == 1, "a device list change with the device still away reopens nothing");

        // Back again: a device list change brings the monitor back by itself.
        rig.world.outputNames.add ("FakeMon");
        check (engine.retryMonitor() && engine.isMonitorActive() && rig.world.aggregatesAlive == 1, "and when the device comes back the monitor is tried again and runs");
        check (! engine.retryMonitor(), "a running monitor is left alone");
    }

    //==========================================================================
    // The open fails after the device was made: back to the main output alone, with the error as the reason.
    {
        const auto limit = [] (test::FakeWorld& world) { monitorWorld (world); world.maxOpenOutputs = 2; };

        Rig launch (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, limit);
        const auto problem = launch.engine.takeMonitorProblem();
        check (launch.engine.deviceManager.getCurrentAudioDevice() != nullptr && launch.engine.deviceManager.getCurrentAudioDevice()->getName() == "FakeOut" && launch.world.devicesAlive == 1,
               "a monitor whose aggregate refuses to open at launch: the main output is open");
        check (! launch.engine.isMonitorActive() && ! launch.engine.getMonitorPlan().enabled && launch.world.aggregatesAlive == 0, "the monitor is off and no aggregate is left");
        check (problem.contains ("too many output channels") && launch.engine.takeMonitorProblem().isEmpty(), "the reason holds the error and is reported once (" + problem + ")");
        check (launch.engine.deviceManager.getAudioDeviceSetup().outputChannels.toInteger() == 0b11 && launch.runningBuffer() == 64, "the device setup is as it was saved");

        Rig live (monitorSettings ({}), {}, limit);
        live.engine.setMonitorPlan ({ true, "FakeMon", 6 });
        const auto liveProblem = live.engine.takeMonitorProblem();
        auto out = live.world.lastDevice != nullptr ? live.world.lastDevice->pump (64, 0.25f) : std::vector<std::vector<float>>();
        check (! live.engine.isMonitorActive() && ! live.engine.getMonitorPlan().enabled && liveProblem.contains ("too many output channels") && live.world.aggregatesAlive == 0,
               "turning the monitor on at run time, with the same refusal: off, with the reason, no aggregate");
        check (live.engine.deviceManager.getCurrentAudioDevice() != nullptr && out.size() == 2 && allEqual (out[0], 0.25f), "and the main output still plays");

        // The restore of a measurement goes through the same path.
        live.world.maxOpenOutputs = 0;
        live.engine.setMonitorPlan ({ true, "FakeMon", 6 });
        check (live.engine.isMonitorActive(), "the monitor is on again once the device accepts it");
        {
            ScopedSetup scope (live.engine);
            live.engine.setMonitorPlan ({ false, "FakeMon", 6 });
            live.world.maxOpenOutputs = 2;
        }
        check (live.engine.deviceManager.getCurrentAudioDevice() != nullptr && ! live.engine.isMonitorActive() && live.engine.takeMonitorProblem().isNotEmpty() && live.world.aggregatesAlive == 0,
               "a refusal when a ScopedSetup puts the monitor back also ends on the main output");
    }

    //==========================================================================
    // A device that is only absent for now does not cost the saved choice.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, [] (test::FakeWorld& world)
        {
            monitorWorld (world);
            world.outputNames.removeString ("FakeOut");      // Loopback is not running yet
        });
        auto& engine = rig.engine;
        check (engine.deviceManager.getCurrentAudioDevice() == nullptr && ! engine.isMonitorActive(), "the main output is missing at launch: nothing is open");
        const auto written = buildSettingsXml ("rack84", engine);
        check (written->getChildByName ("MONITOR") != nullptr && written->getChildByName ("MONITOR")->getIntAttribute ("enabled") == 1
                   && written->getChildByName ("MONITOR")->getStringAttribute ("device") == "FakeMon" && written->getChildByName ("MONITOR")->getIntAttribute ("firstChannel") == 6,
               "the settings written meanwhile still say the monitor is on");

        rig.world.outputNames.insert (0, "FakeOut");
        AudioDeviceManager::AudioDeviceSetup setup;
        setup.outputDeviceName = "FakeOut";
        setup.inputDeviceName = "FakeIn";
        setup.sampleRate = 48000.0;
        setup.bufferSize = 64;
        setup.inputChannels.setBit (0);
        setup.outputChannels.setRange (0, 2, true);
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
        engine.openWithSetup (setup);
        check (engine.isMonitorActive() && rig.world.aggregatesAlive == 1 && engine.getMonitorPlan() == MonitorPlan { true, "FakeMon", 6 }, "when the device appears and opens, the monitor runs");
    }

    // The monitor device missing at launch: the main output opens, the choice waits, a device list change starts it.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, [] (test::FakeWorld& world)
        {
            monitorWorld (world);
            world.outputNames.removeString ("FakeMon");
        });
        auto& engine = rig.engine;
        check (engine.getMonitorPlan().enabled && engine.takeMonitorProblem().isNotEmpty() && rig.world.devicesAlive == 1 && ! engine.isMonitorActive(), "an absent monitor device: main output open, choice kept, reason given");
        const auto written = buildSettingsXml ("rack84", engine);
        check (written->getChildByName ("MONITOR")->getIntAttribute ("enabled") == 1, "and the next save still says on");
        const auto opens = rig.world.opens;
        check (! engine.retryMonitor() && rig.world.opens == opens, "no retry until the device is there");
        rig.world.outputNames.add ("FakeMon");
        check (engine.retryMonitor() && engine.isMonitorActive() && ! engine.retryMonitor(), "once it is, one retry brings the monitor up and the next does nothing");
    }

    //==========================================================================
    // A device the list shows but the backend cannot resolve (a duplicate name, an aggregate) is not "waiting".
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, [] (test::FakeWorld& world)
        {
            monitorWorld (world);
            world.unresolvable.insert ("FakeMon");
        });
        auto& engine = rig.engine;
        const auto problem = engine.takeMonitorProblem();
        check (! engine.isMonitorActive() && ! engine.getMonitorPlan().enabled && problem.isNotEmpty() && engine.deviceManager.getCurrentAudioDevice() != nullptr,
               "a listed device the backend cannot resolve turns the choice off with a reason, and the main output opens (" + problem + ")");
        check (engine.getMonitorCandidates() == StringArray ("FakeOut"), "and it is not offered as a monitor device");

        const auto opens = rig.world.opens;

        for (int i = 0; i < 20; ++i)
            engine.serviceMonitor();

        check (rig.world.opens == opens && rig.world.aggregatesCreated == 0, "pumping 20 change messages reopens nothing (" + String (rig.world.opens - opens) + " reopens)");
    }

    // Waiting is bounded too: a list that did not change is not looked at twice.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, [] (test::FakeWorld& world)
        {
            monitorWorld (world);
            world.outputNames.removeString ("FakeMon");
        });
        auto& engine = rig.engine;
        const auto opens = rig.world.opens;

        for (int i = 0; i < 20; ++i)
            engine.serviceMonitor();

        rig.world.outputNames.add ("Other");
        for (int i = 0; i < 20; ++i)
            engine.serviceMonitor();

        check (rig.world.opens == opens && engine.getMonitorPlan().enabled, "20 change messages with the list unchanged, and 20 after an unrelated device came, reopen nothing");

        rig.world.outputNames.add ("FakeMon");
        check (engine.serviceMonitor() && engine.isMonitorActive(), "the device itself coming is the one reopen");

        const auto after = rig.world.opens;

        for (int i = 0; i < 20; ++i)
            engine.serviceMonitor();

        check (rig.world.opens == after && rig.world.aggregatesCreated == 1, "and then nothing more (" + String (rig.world.opens - after) + " reopens)");
    }

    //==========================================================================
    // The HAL's list follows a destroy late: the next aggregate waits for it and has a name of its own.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, [] (test::FakeWorld& world)
        {
            monitorWorld (world);
            world.delayedRemoval = true;
        });
        auto& engine = rig.engine;
        const auto aggregatesListed = [&rig]
        {
            int n = 0;

            for (auto& name : rig.world.outputNames)
                n += name.startsWith (MonitorDeviceType::aggregateNamePrefix) ? 1 : 0;

            return n;
        };

        const auto first = rig.world.lastAggregateName;
        engine.setMonitorPlan ({ true, "FakeMon", 4 });
        check (engine.isMonitorActive() && rig.world.lastAggregateName != first && rig.world.settledRemovals == 1 && aggregatesListed() == 1,
               "a pair change makes a new, differently named aggregate after the old one left the list");

        engine.setMonitorPlan ({ false, "FakeMon", 4 });
        engine.setMonitorPlan ({ true, "FakeMon", 4 });
        check (engine.isMonitorActive() && rig.world.settledRemovals == 2 && aggregatesListed() == 1 && rig.world.aggregatesAlive == 1, "so does off and on again");
    }

    //==========================================================================
    // MIN LATENCY with a size the aggregate cannot open: that size is not clean, and the monitor is back after.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, [] (test::FakeWorld& world)
        {
            monitorWorld (world);
            world.refuseExtraOutputsBelow = 64;
        });
        auto& engine = rig.engine;
        check (engine.isMonitorActive(), "the monitor runs at 64");

        LatencyTuner tuner (engine);
        setReaderUncleanAtOrBelow (tuner, rig, 16);
        tuner.start();
        bool intentKept = true, droppedSeen = false;

        for (int guard = 0; guard < 5000 && tuner.isRunning(); ++guard)
        {
            tuner.tick();
            droppedSeen = droppedSeen || ! engine.isMonitorActive();
            intentKept = intentKept && buildSettingsXml ("rack84", engine)->getChildByName ("MONITOR")->getIntAttribute ("enabled") == 1;
        }

        check (! tuner.isRunning() && tuner.getResult().startsWith (ja ("最小遅延")) && tuner.getResult().contains ("64"), "the run ends on buffer 64, the smallest size the monitor can run at (" + tuner.getResult() + ")");
        check (droppedSeen && intentKept, "the monitor was dropped at the sizes it could not open, and the saved choice stayed on at every tick");
        check (engine.isMonitorActive() && engine.getMonitorPlan() == MonitorPlan { true, "FakeMon", 6 } && rig.runningBuffer() == 64 && rig.world.aggregatesAlive == 1,
               "afterwards the monitor is running again at the final size");
    }

    //==========================================================================
    // A failure that happens while the UI is busy is picked up afterwards.
    {
        const auto setupOf = []
        {
            AudioDeviceManager::AudioDeviceSetup setup;
            setup.outputDeviceName = "FakeOut";
            setup.inputDeviceName = "FakeIn";
            setup.sampleRate = 48000.0;
            setup.bufferSize = 64;
            setup.inputChannels.setBit (0);
            setup.outputChannels.setRange (0, 2, true);
            setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
            return setup;
        };

        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, monitorWorld);
        auto& engine = rig.engine;

        // JUCE reopens by itself (a device list change) and the open fails with the monitor in use.
        rig.world.maxOpenOutputs = 2;
        engine.deviceManager.closeAudioDevice();
        const auto error = engine.deviceManager.setAudioDeviceSetup (setupOf(), true);
        check (error.isNotEmpty() && engine.deviceManager.getCurrentAudioDevice() == nullptr && engine.getMonitorPlan().enabled, "the device JUCE reopened failed: nothing is open and nothing has reacted yet");
        check (engine.serviceMonitor() && engine.deviceManager.getCurrentAudioDevice() != nullptr && ! engine.isMonitorActive() && engine.takeMonitorProblem().isNotEmpty(),
               "the service step that runs when the busy period ends opens the main output alone, with a reason");
        check (! engine.serviceMonitor(), "and then has nothing to do");
    }

    // A failure recorded for a device that is gone does not cost a monitor that works.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, monitorWorld);
        auto& engine = rig.engine;
        AudioDeviceManager::AudioDeviceSetup setup;
        setup.outputDeviceName = "FakeOut";
        setup.inputDeviceName = "FakeIn";
        setup.sampleRate = 48000.0;
        setup.bufferSize = 64;
        setup.inputChannels.setBit (0);
        setup.outputChannels.setRange (0, 2, true);
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;

        rig.world.maxOpenOutputs = 2;
        engine.deviceManager.closeAudioDevice();
        check (engine.deviceManager.setAudioDeviceSetup (setup, true).isNotEmpty(), "the first open fails and is recorded");
        rig.world.maxOpenOutputs = 0;
        check (engine.deviceManager.setAudioDeviceSetup (setup, true).isEmpty() && engine.isMonitorActive(), "JUCE's next open, on a device that works, runs the monitor");
        check (! engine.serviceMonitor() && engine.isMonitorActive() && engine.getMonitorPlan().enabled && engine.takeMonitorProblem().isEmpty(),
               "the earlier failure is not held against it");
    }

    // The restore of a scope reopens once.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, monitorWorld);
        auto& engine = rig.engine;
        const auto created = rig.world.devicesCreated;
        {
            ScopedSetup scope (engine);
            engine.setMonitorPlan ({ false, "FakeMon", 6 });
        }

        check (engine.isMonitorActive() && rig.world.devicesCreated == created + 2, "turning the monitor off for a scope and restoring it makes two devices, not three (" + String (rig.world.devicesCreated - created) + ")");
    }

    //==========================================================================
    // A measurement is of the main output path: it takes the monitor out and puts it back.
    {
        Rig rig (monitorSettings ("<MONITOR enabled=\"1\" device=\"FakeMon\" firstChannel=\"6\"/>"), {}, monitorWorld);
        {
            measure::Session session (rig.engine, {});
            const auto started = session.start();       // stops at the HAL lookup, which the fake layer has no aggregate for
            check (! started && session.getResult().problem.isNotEmpty(), "a session on the fake layer cannot start (no HAL aggregate)");
        }

        check (rig.engine.isMonitorActive() && rig.engine.getMonitorPlan() == MonitorPlan { true, "FakeMon", 6 } && rig.world.aggregatesAlive == 1,
               "the monitor is back on after the session is gone");
        check (rig.world.aggregatesCreated == 2, "after it was out of the way for the session (" + String (rig.world.aggregatesCreated) + " aggregates made in all)");
        check (rig.engine.deviceManager.getAudioDeviceSetup().outputDeviceName == "FakeOut", "and the output device is still the main one");
    }

    //==========================================================================
    // The one monitor-path figure, and the main rows staying as they are.
    {
        hal::Numbers aggregate;
        aggregate.bufferSize = 64;
        aggregate.inputSafety = 5;
        aggregate.outputSafety = 4;

        BreakdownInput input;
        input.sampleRate = 44100.0;
        input.aggregate = aggregate;
        input.plugins.push_back ({ 0, "P", 100, false });
        const auto breakdown = computeBreakdown (input);

        hal::Numbers monitor;
        monitor.outputSafety = 10;
        monitor.outputLatency = 20;
        monitor.outputStreamLatency = 3;

        check (breakdown.declaredSamples == 5 + 64 + 100 + 64 + 4, "the main breakdown does not know about the monitor (" + String (breakdown.declaredSamples) + " samples)");
        check (declareMonitorPath (breakdown, monitor, 64, 1.0) == 5 + 64 + 100 + 64 + 33, "the monitor path is the input side, the plugins and the monitor device's own output side");
        check (declareMonitorPath (breakdown, monitor, 64, 0.5) == 5 + 64 + 100 + 32 + 33, "at a cycle usage of 0.5 the output buffer counts half");
    }

    say (check.ok ? "SELFTEST MONITOR OK" : "SELFTEST MONITOR FAILED");
    return check.ok ? 0 : 1;
}

} // namespace mfx
