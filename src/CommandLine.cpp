#include "CommandLine.h"
#include "Chain.h"
#include "Correlate.h"
#include "Engine.h"
#include "Measure.h"
#include "MeasureCommand.h"
#include "Monitor.h"
#include "Product.h"
#include "SelfTests.h"
#include "Settings.h"
#include "Theme.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace mfx
{
using namespace juce;

namespace
{
    void say (const String& text)
    {
        std::cout << text.toRawUTF8() << std::endl;
    }

    // Overridden by --rate so plugin latency can be compared across sample rates.
    double selfTestSampleRate = 48000.0;
    constexpr int selfTestBlockSize = 64;
    constexpr double selfTestSeconds = 3.0;

    void printUsage()
    {
        const String name (productName);

        say ("usage: " + name + " [--settings <file>]");
        say ("       " + name + " --selftest <path to .vst3> [--rate <Hz>]");
        say ("       " + name + " --selftest-settings");
        say ("       " + name + " --selftest-correlate | --selftest-chain | --selftest-sweep | --selftest-timing | --selftest-engine");
        say ("       " + name + " --selftest-breakdown | --selftest-processtime | --selftest-monitor");
        say ("       " + name + " --probe-monitor \"<main output>\" \"<monitor device>\"");
        say ("       " + name + " --measure | --measure-sweep | --tune --settings <file> [--runs N] [--buffer N] [--io-cycle-usage X] [--with-chain] [--force]");
        say ("       " + name + " --list-devices");
        say ("");
        say ("  --settings <file>    use this settings file instead of the default one (no migration)");
        say ("  --selftest           load a plugin through the chain and process audio without a device");
        say ("  --selftest-settings  check the settings carry-over from the pre-rename folder and the theme id");
        say ("  --list-devices       print input and output device names");
        say ("  --selftest-correlate the delay estimator against signals with a known fractional delay");
        say ("  --selftest-chain     hard bypass: latency, CPU and state of a bypassed slot, with test processors");
        say ("  --selftest-sweep     the sweep's choice of the fastest clean setting");
        say ("  --selftest-timing    the latency arithmetic on made-up HAL timestamps, and the burst injection");
        say ("  --selftest-engine    persisted audio settings, MIN LATENCY and device restore on a fake device layer");
        say ("  --selftest-breakdown the latency breakdown's arithmetic, its largest row, and the measured excess saved and restored");
        say ("  --selftest-processtime per-plugin processing time: the hand-over, the windows, the heaviest slot, and the probes in the graph");
        say ("  --selftest-monitor   the monitor output: its settings, the channel routing and the copy to the monitor pair, on a fake device layer");
        say ("  --probe-monitor      make the monitor's aggregate device as the app would, print the devices' rates and the channel layout, destroy it (no IO)");
        say ("  --measure            inject a test burst, record it on the output device and print the latency");
        say ("  --measure-sweep      repeat that for buffer sizes and switches and print a table sorted by latency");
        say ("  --tune               run the MIN LATENCY search without a window, then restore the setup");
        say ("  --force              run a measurement although another " + name + " process is running (the devices are opened a second time)");
    }

    int listDevices()
    {
        AudioDeviceManager manager;

        for (auto* type : manager.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            say ("[" + type->getTypeName() + "]");

            for (auto& name : type->getDeviceNames (true))
                say ("  input:  " + name);

            for (auto& name : type->getDeviceNames (false))
                say ("  output: " + name);
        }

        return 0;
    }

    void prepareGraph (Chain& chain)
    {
        // Same shape the app gets from a mono mic and a stereo virtual device.
        auto& graph = chain.getGraph();
        graph.setPlayConfigDetails (1, 2, selfTestSampleRate, selfTestBlockSize);
        graph.prepareToPlay (selfTestSampleRate, selfTestBlockSize);
    }

    struct Peaks { float left = 0.0f, right = 0.0f; };

    // Feeds an impulse followed by quiet noise on the single input channel, the way
    // AudioProcessorPlayer hands a mono mic to a graph with two output channels.
    Peaks processSeconds (AudioProcessorGraph& graph, double seconds)
    {
        const auto numBlocks = (int) (seconds * selfTestSampleRate / selfTestBlockSize);
        juce::AudioBuffer<float> buffer (2, selfTestBlockSize);
        MidiBuffer midi;
        Random random (1234);
        Peaks peaks;

        for (int block = 0; block < numBlocks; ++block)
        {
            buffer.clear();

            for (int i = 0; i < selfTestBlockSize; ++i)
                buffer.setSample (0, i, 0.1f * (random.nextFloat() * 2.0f - 1.0f));

            if (block == 0)
                buffer.setSample (0, 0, 1.0f);

            graph.processBlock (buffer, midi);

            peaks.left  = jmax (peaks.left,  buffer.getMagnitude (0, 0, selfTestBlockSize));
            peaks.right = jmax (peaks.right, buffer.getMagnitude (1, 0, selfTestBlockSize));
        }

        return peaks;
    }

    // Runs the migration against a temporary home directory; the real one is never touched.
    int selfTestSettings()
    {
        bool ok = true;

        auto check = [&ok] (bool condition, const String& what)
        {
            say (String (condition ? "ok:   " : "FAIL: ") + what);
            ok = ok && condition;
        };

        const auto contents = String ("<MIC_FX_HOST><CHAIN/></MIC_FX_HOST>");
        const File root = File::getSpecialLocation (File::tempDirectory).getNonexistentChildFile ("voxrack-settings-selftest", {}, false);

        {
            const auto home = root.getChildFile ("carry-over");
            const auto legacy = legacySettingsFile (home);
            const auto target = defaultSettingsFile (home);
            legacy.create();
            legacy.replaceWithText (contents);

            check (migrateLegacySettings (home) == Migration::copied, "legacy settings are copied when the new file is missing");
            check (target.loadFileAsString() == contents, "the copy has the same contents");
            check (legacy.loadFileAsString() == contents, "the legacy file is left in place");
            check (target.getParentDirectory().getNumberOfChildFiles (File::findFiles) == 1, "no temporary file is left behind");
            check (migrateLegacySettings (home) == Migration::notNeeded, "a second run does nothing");
        }

        {
            const auto home = root.getChildFile ("keep-existing");
            const auto legacy = legacySettingsFile (home);
            const auto target = defaultSettingsFile (home);
            legacy.create();
            legacy.replaceWithText ("legacy");
            target.create();
            target.replaceWithText ("current");

            check (migrateLegacySettings (home) == Migration::notNeeded, "nothing happens when the new file already exists");
            check (target.loadFileAsString() == "current", "the existing new file is not overwritten");
        }

        {
            const auto home = root.getChildFile ("fresh");
            home.createDirectory();

            check (migrateLegacySettings (home) == Migration::notNeeded, "nothing happens without a legacy file");
            check (! defaultSettingsFile (home).getParentDirectory().exists(), "no settings folder is created without a legacy file");
        }

        {
            // Settings go to a temporary file only; the override is cleared again below.
            useSettingsFile (root.getChildFile ("theme/settings.xml"));

            for (auto& entry : ui::ThemeRegistry::entries())
            {
                XmlElement saved ("MIC_FX_HOST");
                writeThemeId (saved, entry.id);
                const auto loaded = saveSettings (saved) ? loadSettings() : nullptr;

                check (loaded != nullptr && ui::ThemeRegistry::sanitise (readThemeId (*loaded)) == entry.id,
                       String ("theme '") + entry.id + "' is saved and restored");
            }

            XmlElement unknown ("MIC_FX_HOST");
            writeThemeId (unknown, "no-such-theme");
            check (ui::ThemeRegistry::sanitise (readThemeId (unknown)) == ui::ThemeRegistry::getDefaultId(),
                   "an unknown theme id falls back to the default");

            const XmlElement unset ("MIC_FX_HOST");
            check (ui::ThemeRegistry::sanitise (readThemeId (unset)) == ui::ThemeRegistry::getDefaultId(),
                   "settings without a theme use the default");

            useSettingsFile ({});
        }

        {
            const auto real = root.getChildFile ("same/real folder/settings.xml");
            real.create();
            const auto linkFolder = root.getChildFile ("same/link");
            const auto linkFile = root.getChildFile ("same/file-link.xml");
            ::symlink (real.getParentDirectory().getFullPathName().toRawUTF8(), linkFolder.getFullPathName().toRawUTF8());
            ::symlink (real.getFullPathName().toRawUTF8(), linkFile.getFullPathName().toRawUTF8());
            const auto hardLink = root.getChildFile ("same/hard.xml");
            ::link (real.getFullPathName().toRawUTF8(), hardLink.getFullPathName().toRawUTF8());

            check (isSameFile (real, real), "a file is the same as itself");
            check (isSameFile (real, real.getParentDirectory().getChildFile ("../real folder/./settings.xml")), "relative segments are resolved");
            check (isSameFile (real, linkFile), "a symbolic link to the file is the same file");
            check (isSameFile (real, linkFolder.getChildFile ("settings.xml")), "a path through a linked folder is the same file");
            check (isSameFile (real, hardLink), "a hard link is the same file");
            check (! isSameFile (real, root.getChildFile ("same/other.xml")), "another file is not");

            const auto missing = real.getParentDirectory().getChildFile ("new.xml");
            check (isSameFile (missing, linkFolder.getChildFile ("new.xml")), "a file that does not exist yet is compared by its resolved folder");
            check (! isSameFile (missing, real), "and differs from an existing one");
        }

        check (defaultSettingsFile (root).getFullPathName().contains (String ("/") + productName + "/settings.xml"),
               "the new location is named after the product");

        root.deleteRecursively();
        say (ok ? "SELFTEST SETTINGS OK" : "SELFTEST SETTINGS FAILED");
        return ok ? 0 : 1;
    }


    //==============================================================================
    // Estimates the delay of the test burst at known fractional positions, with noise and a distractor.
    int selfTestCorrelate()
    {
        bool ok = true;

        auto check = [&ok] (bool condition, const String& what)
        {
            say (String (condition ? "ok:   " : "FAIL: ") + what);
            ok = ok && condition;
        };

        const auto toleranceMs = 0.05;
        double worstMs = 0.0;
        Random random (42);

        for (const auto rate : { 44100.0, 48000.0, 96000.0 })
            for (const auto delay : { 1234.37, 2000.5, 33.9, 9000.012 })
            {
                const auto reference = measure::makeTestSignal (rate);
                std::vector<float> recording ((size_t) (0.5 * rate), 0.0f);

                // The exact burst, shifted by a fractional delay, over a noise floor 40 dB down, with a
                // half-amplitude echo further on that must not be taken for the burst.
                for (size_t i = 0; i < recording.size(); ++i)
                {
                    const auto t = ((double) i - delay) / rate;
                    const auto echo = ((double) i - delay - 0.2 * rate) / rate;
                    recording[i] = (float) (measure::testSignalAt (rate, t) + 0.5 * measure::testSignalAt (rate, echo)
                                            + 0.001 * (random.nextDouble() * 2.0 - 1.0));
                }

                const auto estimate = measure::estimateDelay (reference.data(), (int) reference.size(), recording.data(), (int) recording.size());
                const auto errorMs = std::abs (estimate.delaySamples - delay) / rate * 1000.0;
                worstMs = jmax (worstMs, errorMs);

                if (errorMs >= toleranceMs || ! measure::isTrustworthy (estimate))
                    check (false, String (rate, 0) + " Hz, delay " + String (delay, 3) + " samples: estimated " + String (estimate.delaySamples, 3));
            }

        check (worstMs < toleranceMs, "all 12 delays recovered within " + String (toleranceMs) + " ms (worst " + String (worstMs, 5) + " ms)");

        {
            // Noise alone must not pass for a measurement.
            const auto reference = measure::makeTestSignal (48000.0);
            std::vector<float> noise (24000);

            for (auto& v : noise)
                v = 0.05f * (random.nextFloat() * 2.0f - 1.0f);

            const auto estimate = measure::estimateDelay (reference.data(), (int) reference.size(), noise.data(), (int) noise.size());
            check (! measure::isTrustworthy (estimate), "noise alone is rejected (correlation " + String (estimate.coefficient, 3) + ")");
        }

        {
            const auto summary = measure::summarise ({ 3.0, 1.0, 2.0, 10.0 });
            check (summary.median == 2.5 && summary.minimum == 1.0 && summary.maximum == 10.0, "median, minimum and maximum");
        }

        say (ok ? "SELFTEST CORRELATE OK" : "SELFTEST CORRELATE FAILED");
        return ok ? 0 : 1;
    }

    //==============================================================================
    // A stand-in for a plugin that reports latency, really delays by it, has a bypass parameter (as most
    // VST3 plugins do, so a soft bypass would keep the latency) and keeps one value as its state.
    class LatentProcessor final : public AudioProcessor
    {
    public:
        explicit LatentProcessor (int latency)
            : AudioProcessor (BusesProperties().withInput ("In", AudioChannelSet::stereo()).withOutput ("Out", AudioChannelSet::stereo())),
              delayLine ((size_t) latency + 1, 0.0f)
        {
            setLatencySamples (latency);
            addParameter (bypass = new AudioParameterBool ({ "bypass", 1 }, "Bypass", false));
        }

        void processBlock (juce::AudioBuffer<float>& buffer, MidiBuffer&) override
        {
            ++blocks;

            // Both channels carry the same signal in these tests, so one delay line is enough.
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                delayLine[(size_t) writeIndex] = buffer.getSample (0, i);
                writeIndex = (writeIndex + 1) % (int) delayLine.size();
                const auto out = delayLine[(size_t) writeIndex];

                for (int c = 0; c < buffer.getNumChannels(); ++c)
                    buffer.setSample (c, i, out);
            }
        }

        using AudioProcessor::processBlock;

        void reset() override                           { std::fill (delayLine.begin(), delayLine.end(), 0.0f); }
        AudioProcessorParameter* getBypassParameter() const override { return bypass; }

        const String getName() const override           { return "Latent"; }
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
        void getStateInformation (MemoryBlock& destination) override { destination.replaceAll (&marker, sizeof (marker)); }
        void setStateInformation (const void* data, int size) override { if (size == (int) sizeof (marker)) memcpy (&marker, data, sizeof (marker)); }

        int marker = 0;
        int blocks = 0;

    private:
        AudioParameterBool* bypass = nullptr;
        std::vector<float> delayLine;
        int writeIndex = 0;
    };

    // Position of the first output sample above half scale after an impulse at the start, or -1.
    int measureImpulseDelay (AudioProcessorGraph& graph)
    {
        juce::AudioBuffer<float> buffer (2, selfTestBlockSize);
        MidiBuffer midi;

        for (int block = 0; block < 40; ++block)
        {
            buffer.clear();

            if (block == 0)
                buffer.setSample (0, 0, 1.0f);

            graph.processBlock (buffer, midi);

            for (int i = 0; i < selfTestBlockSize; ++i)
                if (buffer.getSample (0, i) > 0.5f)
                    return block * selfTestBlockSize + i;
        }

        return -1;
    }

    int selfTestChain()
    {
        bool ok = true;

        auto check = [&ok] (bool condition, const String& what)
        {
            say (String (condition ? "ok:   " : "FAIL: ") + what);
            ok = ok && condition;
        };

        selfTestSampleRate = 48000.0;
        AudioPluginFormatManager formats;
        Chain chain (formats);
        prepareGraph (chain);

        constexpr int latency = 184;
        auto owned = std::make_unique<LatentProcessor> (latency);
        auto* plugin = owned.get();
        plugin->marker = 4711;
        PluginDescription description;
        description.name = "Latent";
        chain.addProcessor (std::move (owned), description);
        auto& graph = chain.getGraph();

        check (graph.getLatencySamples() == latency, "active slot: graph reports " + String (graph.getLatencySamples()) + " samples");
        check (measureImpulseDelay (graph) == latency, "active slot: the impulse really arrives " + String (latency) + " samples late");

        chain.setBypassed (0, true);
        check (graph.getLatencySamples() == 0, "bypassed slot: graph reports " + String (graph.getLatencySamples()) + " samples (was " + String (latency) + ")");
        check (plugin->isSuspended(), "bypassed slot: the plugin is suspended");

        const auto blocksBefore = plugin->blocks;
        check (measureImpulseDelay (graph) == 0, "bypassed slot: the impulse passes with no delay");
        check (plugin->blocks == blocksBefore, "bypassed slot: processBlock is no longer called");

        MemoryBlock state;
        plugin->getStateInformation (state);
        check (plugin->marker == 4711 && state.getSize() == sizeof (int), "bypassed slot: the plugin instance keeps its state");
        check (chain[0].bypassed && chain[0].isLive(), "bypassed slot: the chain still lists it as bypassed and live");

        chain.setBypassed (0, false);
        check (graph.getLatencySamples() == latency && ! plugin->isSuspended(), "un-bypassed: latency is back to " + String (graph.getLatencySamples()));
        check (measureImpulseDelay (graph) == latency, "un-bypassed: the impulse is delayed again, with no stale audio from before");

        chain.setForceBypassAll (true);
        check (graph.getLatencySamples() == 0 && ! chain[0].bypassed, "forced bypass of everything: latency 0 and the slot's own flag untouched");
        chain.setForceBypassAll (false);
        check (graph.getLatencySamples() == latency, "forced bypass released: latency " + String (graph.getLatencySamples()));

        // Toggling the bypass while another thread keeps processing: a node that is suspended before the graph
        // stops connecting it outputs silence for the blocks in between (thousands in this run). Suspending it
        // after the rebuild leaves only a block that was already running when the new sequence took over.
        {
            // A processor without latency, so that a silent block can only come from the graph: the
            // one above is reset on every un-bypass and is silent for its 184 samples by design.
            Chain stress (formats);
            prepareGraph (stress);
            stress.addProcessor (std::make_unique<LatentProcessor> (0), description);
            auto& stressGraph = stress.getGraph();

            std::atomic<bool> stop { false };
            std::atomic<int> silentBlocks { 0 }, blocks { 0 };

            std::thread audio ([&]
            {
                juce::AudioBuffer<float> buffer (2, selfTestBlockSize);
                MidiBuffer midi;

                while (! stop.load())
                {
                    buffer.clear();

                    for (int i = 0; i < selfTestBlockSize; ++i)
                        buffer.setSample (0, i, 1.0f);

                    stressGraph.processBlock (buffer, midi);

                    if (++blocks > 50 && buffer.getMagnitude (0, 0, selfTestBlockSize) == 0.0f)
                        ++silentBlocks;
                }
            });

            for (int i = 0; i < 400; ++i)
            {
                stress.setBypassed (0, i % 2 == 0);
                Thread::sleep (1);
            }

            stop = true;
            audio.join();
            check (silentBlocks <= 40, "toggling the bypass 400 times under load: " + String (silentBlocks.load()) + " of " + String (blocks.load()) + " blocks were silent");
        }

        say (ok ? "SELFTEST CHAIN OK" : "SELFTEST CHAIN FAILED");
        return ok ? 0 : 1;
    }

    //==============================================================================
    int selfTestSweep()
    {
        bool ok = true;

        auto check = [&ok] (bool condition, const String& what)
        {
            say (String (condition ? "ok:   " : "FAIL: ") + what);
            ok = ok && condition;
        };

        using measure::Candidate;
        const std::vector<Candidate> rows {
            { true, 6.0, 0, 0 }, { true, 3.0, 2, 0 }, { true, 4.0, 0, 3 }, { true, 5.0, 0, 0 }, { false, 0.0, 0, 0 }, { true, 4.5, 0, 0 } };

        check (measure::pickBest (rows) == 5, "the fastest row without overloads or late callbacks wins (not the 3.0 ms one with overloads)");
        check (measure::pickBest ({ { true, 3.0, 1, 0 }, { false, 0.0, 0, 0 } }) == -1, "no clean row means no winner");
        check (measure::isFaster ({ true, 9.0, 0, 0 }, { false, 0.0, 0, 0 }) && ! measure::isFaster ({ false, 0.0, 0, 0 }, { true, 9.0, 0, 0 }),
               "rows that could not be measured sort last");

        say (ok ? "SELFTEST SWEEP OK" : "SELFTEST SWEEP FAILED");
        return ok ? 0 : 1;
    }

    int selfTest (const String& path)
    {
        const File file (path);

        if (! file.exists())
        {
            say ("FAIL: no such file: " + path);
            return 2;
        }

        AudioPluginFormatManager formats;
        addDefaultFormatsToManager (formats);

        const auto descriptions = findPluginsInFile (formats, file);

        if (descriptions.isEmpty())
        {
            say ("FAIL: no plugin found in " + path);
            return 2;
        }

        Chain chain (formats);
        prepareGraph (chain);

        String error;

        if (! chain.add (descriptions.getFirst(), error))
        {
            say ("FAIL: could not load " + descriptions.getFirst().name + ": " + error);
            return 2;
        }

        MessageManager::getInstance()->runDispatchLoopUntil (200);

        auto& graph = chain.getGraph();
        auto& processor = *chain[0].node->getProcessor();

        say ("plugin:            " + processor.getName() + " (" + descriptions.getFirst().pluginFormatName
             + ", " + descriptions.getFirst().manufacturerName + ")");
        say ("channels:          " + String (processor.getMainBusNumInputChannels()) + " in / "
             + String (processor.getMainBusNumOutputChannels()) + " out");
        say ("plugin latency:    " + String (processor.getLatencySamples()) + " samples");
        say ("graph latency:     " + String (graph.getLatencySamples()) + " samples");

        const auto active = processSeconds (graph, selfTestSeconds);
        const auto activePeak = jmax (active.left, active.right);

        // Some plugins only report their real latency once audio has run through them.
        MessageManager::getInstance()->runDispatchLoopUntil (200);
        say ("latency after processing: plugin " + String (processor.getLatencySamples())
             + " / graph " + String (graph.getLatencySamples()) + " samples");

        say ("output peak:       L " + String (active.left, 4) + "  R " + String (active.right, 4));
        say ("output non-silent: " + String (activePeak > 1.0e-6f ? "yes" : "no"));

        chain.setBypassed (0, true);
        const auto bypassed = processSeconds (graph, 0.5);
        const auto bypassedOk = bypassed.left > 0.05f && bypassed.right > 0.05f;
        const auto bypassedLatency = graph.getLatencySamples();
        say ("latency while bypassed: graph " + String (bypassedLatency) + " samples (a hard bypass leaves the path)");
        say ("bypass pass-through: L " + String (bypassed.left, 4) + "  R " + String (bypassed.right, 4)
             + (bypassedOk ? "  (mono input reaches both sides)" : "  (NO SIGNAL)"));

        const auto xml = chain.toXml();
        Chain restored (formats);
        prepareGraph (restored);
        const auto problems = restored.restore (*xml);
        const auto roundTripOk = problems.isEmpty() && restored.isEquivalentTo (chain) && restored[0].bypassed;
        say ("xml round trip:    " + String (roundTripOk ? "ok" : "MISMATCH")
             + " (state " + String (xml->getChildElement (0)->getStringAttribute ("state").length()) + " base64 chars)");

        for (auto& problem : problems)
            say ("  restore problem: " + problem);

        const auto success = activePeak > 1.0e-6f && bypassedOk && roundTripOk && bypassedLatency == 0;
        say (success ? "SELFTEST OK" : "SELFTEST FAILED");
        return success ? 0 : 1;
    }
}

bool isCommandLineMode (const StringArray& args)
{
    return args.contains ("--selftest") || args.contains ("--selftest-correlate") || args.contains ("--selftest-chain")
        || args.contains ("--selftest-sweep") || args.contains ("--selftest-settings") || args.contains ("--selftest-timing")
        || args.contains ("--selftest-engine") || args.contains ("--selftest-engine-child")
        || args.contains ("--selftest-breakdown") || args.contains ("--selftest-processtime") || args.contains ("--selftest-monitor")
        || args.contains ("--probe-monitor")
        || args.contains ("--list-devices") || args.contains ("--help") || isMeasureMode (args);
}

bool applySettingsOption (const StringArray& args)
{
    const auto index = args.indexOf ("--settings");

    if (index < 0)
        return true;

    if (index + 1 >= args.size())
        return false;

    useSettingsFile (File::getCurrentWorkingDirectory().getChildFile (args[index + 1]));
    return true;
}

int runCommandLine (const StringArray& args)
{
    const ScopedJuceInitialiser_GUI initialiser;

    if (args.contains ("--help"))
    {
        printUsage();
        return 0;
    }

    if (args.contains ("--list-devices"))
        return listDevices();

    if (isMeasureMode (args))
    {
        if (! applySettingsOption (args))
        {
            printUsage();
            return 64;
        }

        return runMeasureCommand (args);
    }

    if (args.contains ("--selftest-settings"))
        return selfTestSettings();

    if (args.contains ("--selftest-correlate"))
        return selfTestCorrelate();

    if (args.contains ("--selftest-chain"))
        return selfTestChain();

    if (args.contains ("--selftest-sweep"))
        return selfTestSweep();

    if (args.contains ("--selftest-timing"))
        return selfTestTiming();

    if (args.contains ("--selftest-engine"))
        return selfTestEngine();

    if (args.contains ("--selftest-breakdown"))
        return selfTestBreakdown();

    if (args.contains ("--selftest-processtime"))
        return selfTestProcessTime();

    if (args.contains ("--selftest-monitor"))
        return selfTestMonitor();

    if (const auto probe = args.indexOf ("--probe-monitor"); probe >= 0)
    {
        if (probe + 2 >= args.size())
        {
            printUsage();
            return 64;
        }

        return runProbeMonitor (args[probe + 1], args[probe + 2]);
    }

    if (const auto child = args.indexOf ("--selftest-engine-child"); child >= 0 && child + 1 < args.size())
        return selfTestEngineChild (args[child + 1]);

    if (const auto rate = args.indexOf ("--rate"); rate >= 0 && rate + 1 < args.size() && args[rate + 1].getDoubleValue() > 0.0)
        selfTestSampleRate = args[rate + 1].getDoubleValue();

    const auto index = args.indexOf ("--selftest");

    if (index + 1 >= args.size())
    {
        printUsage();
        return 64;
    }

    return selfTest (args[index + 1]);
}

} // namespace mfx
