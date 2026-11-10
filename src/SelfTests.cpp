#include "SelfTests.h"
#include "Engine.h"
#include "FakeAudioDevice.h"
#include "LatencyTuner.h"
#include "Measure.h"
#include "Settings.h"
#include "Text.h"

#include <CoreAudio/HostTime.h>
#include <algorithm>
#include <cmath>
#include <iostream>

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

} // namespace mfx
