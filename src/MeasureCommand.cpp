#include "MeasureCommand.h"
#include "LatencyTuner.h"
#include "Measure.h"
#include "Product.h"
#include "Settings.h"

#include <algorithm>
#include <iostream>
#include <mach/mach_time.h>
#include <unistd.h>

namespace mfx
{
using namespace juce;

namespace
{
    struct Report
    {
        void line (const String& text)
        {
            std::cout << text.toRawUTF8() << std::endl;
            lines.add (text);
        }

        StringArray lines;
    };

    String fixed (double value, int digits = 3)
    {
        return String (value, digits);
    }

    String getOption (const StringArray& args, const String& name, const String& fallback = {})
    {
        const auto index = args.indexOf (name);
        return index >= 0 && index + 1 < args.size() ? args[index + 1] : fallback;
    }

    bool anotherInstanceRunning()
    {
        ChildProcess pgrep;

        if (! pgrep.start (StringArray { "/usr/bin/pgrep", "-x", String (productName) }))
            return false;

        const auto output = pgrep.readAllProcessOutput();
        const auto self = String ((int) getpid());

        for (auto& pid : StringArray::fromLines (output))
            if (pid.trim().isNotEmpty() && pid.trim() != self)
                return true;

        return false;
    }

    String timestamp()
    {
        return Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    }

    void describeNumbers (Report& report, const String& label, const hal::Numbers& n)
    {
        report.line (label + ": " + String (n.nominalRate, 0) + " Hz, buffer " + String (n.bufferSize) + " (range " + String (n.bufferMin) + "-" + String (n.bufferMax)
                     + "), in latency/safety/stream " + String (n.inputLatency) + "/" + String (n.inputSafety) + "/" + String (n.inputStreamLatency)
                     + ", out " + String (n.outputLatency) + "/" + String (n.outputSafety) + "/" + String (n.outputStreamLatency) + " samples");
    }

    void describeRun (Report& report, const measure::Result& r, bool withChain)
    {
        for (size_t i = 0; i < r.runs.size(); ++i)
        {
            const auto& run = r.runs[i];

            if (run.coefficient == 0.0 && ! run.trustworthy)
            {
                report.line ("run " + String ((int) i + 1) + ": no result (cycle or recording not found)");
                continue;
            }

            report.line ("run " + String ((int) i + 1) + ": latency " + fixed (run.latencyMs) + " ms  declared " + fixed (run.declaredMs)
                         + " ms  excess " + fixed (run.excessMs) + " ms  correlation " + fixed (run.coefficient, 2) + "  sidelobe "
                         + fixed (run.sidelobeDb, 1) + " dB  " + (run.trustworthy ? "ok" : "REJECTED (bad peak)"));
        }

        const auto line = [&report] (const String& name, const measure::Summary& s)
        {
            report.line (name + ": median " + fixed (s.median) + "  min " + fixed (s.minimum) + "  max " + fixed (s.maximum)
                         + "  stddev " + fixed (s.stddev) + " ms  (n=" + String (s.count) + ")");
        };

        line ("latency   (input acquired -> available on output device)", r.latency);
        line ("declared  (HAL timestamps of the host cycle alone)      ", r.declared);
        line ("excess    (latency - declared, added by the output device)", r.excess);

        if (withChain)
        {
            const auto pluginMs = 1000.0 * r.chainLatencySamples / r.sampleRate;
            report.line ("plugin latency reported by the chain: " + String (r.chainLatencySamples) + " samples = " + fixed (pluginMs) + " ms");
            report.line ("host + virtual device path (latency - plugins): median " + fixed (r.latency.median - pluginMs) + " ms");
        }

        report.line ("host callback wakes " + fixed (r.wakeMs) + " ms after the first input frame of its cycle (median)");
        report.line ("overloads (HAL) " + String (r.overloads) + ", late callbacks " + String (r.load.late) + ", callback load mean "
                     + fixed (r.load.meanPercent, 1) + " % max " + fixed (r.load.maxPercent, 1) + " %, capture gaps " + String (r.captureGaps));
    }

    //==============================================================================
    struct Session
    {
        Engine engine;
        std::unique_ptr<ScopedSetup> restore;     // after `engine`: it is gone before the engine is
        File settingsFile;
        Engine::DeviceRates ratesBefore;
        String inputName, outputName;
        bool chainLoaded = false;
    };

    // Returns an empty string on success, otherwise what went wrong.
    String openSession (Session& s, const StringArray& args, bool loadChain, Report& report)
    {
        const auto settings = loadSettings();

        if (settings == nullptr)
            return "cannot read the settings file " + getSettingsFile().getFullPathName();

        s.settingsFile = getSettingsFile();
        const auto* deviceXml = settings->getChildByName ("DEVICESETUP");

        if (deviceXml == nullptr)
            return "the settings file has no DEVICESETUP; choose devices in the app first";

        s.inputName = deviceXml->getStringAttribute ("audioInputDeviceName");
        s.outputName = deviceXml->getStringAttribute ("audioOutputDeviceName");

        // Read from the HAL before anything opens the devices: JUCE writes its rate into every device it
        // opens, which can retune a virtual output device, and the run has to say so.
        s.ratesBefore = { Engine::nominalRateOf (s.inputName, true), Engine::nominalRateOf (s.outputName, false) };
        report.line ("input device " + s.inputName + " nominal rate before: " + String (s.ratesBefore.input, 0) + " Hz");
        report.line ("output device " + s.outputName + " nominal rate before: " + String (s.ratesBefore.output, 0) + " Hz");

        if (loadChain)
            if (auto* chainXml = settings->getChildByName ("CHAIN"))
            {
                for (auto& problem : s.engine.chain.restore (*chainXml))
                    report.line ("chain problem: " + problem);

                s.chainLoaded = true;
            }

        s.engine.openDevices (deviceXml);

        if (s.engine.deviceManager.getCurrentAudioDevice() == nullptr)
            return "the devices in the settings did not open";

        s.engine.start();

        // Whatever the options below change on the device is put back when the session ends.
        s.restore = std::make_unique<ScopedSetup> (s.engine);

        if (const auto size = getOption (args, "--buffer"); size.isNotEmpty())
            if (s.engine.setBufferSize (size.getIntValue()) == 0)
                return "could not open the device at buffer size " + size;

        Experiments experiments;

        if (const auto usage = getOption (args, "--io-cycle-usage"); usage.isNotEmpty())
        {
            experiments.ioCycleUsage = validCycleUsage (usage.getDoubleValue());

            if (! experiments.ioCycleUsage.has_value())
                return "--io-cycle-usage needs a number above 0 and at most 1, not '" + usage + "'";
        }

        s.engine.setExperiments (experiments);
        measure::wait (500);
        s.engine.refreshHal();
        return {};
    }

    // Reads both nominal rates again. True when neither moved while the devices were open.
    bool verifyRates (Session& s, Report& report)
    {
        bool unchanged = true;

        const auto check = [&] (const String& label, const String& name, bool input, double before)
        {
            const auto after = Engine::nominalRateOf (name, input);
            const auto same = std::abs (after - before) < 0.5;
            unchanged = unchanged && same;
            report.line (label + " " + name + " nominal rate after: " + String (after, 0) + " Hz"
                         + (same ? " (unchanged)" : " (CHANGED, was " + String (before, 0) + " Hz)"));
        };

        check ("input device", s.inputName, true, s.ratesBefore.input);
        check ("output device", s.outputName, false, s.ratesBefore.output);

        if (! unchanged)
            report.line ("opening the devices changed a nominal rate: JUCE writes the rate of the settings file into the devices it opens");

        return unchanged;
    }

    void describeSetup (Report& report, Session& s)
    {
        auto* device = s.engine.deviceManager.getCurrentAudioDevice();
        const auto setup = s.engine.deviceManager.getAudioDeviceSetup();

        mach_timebase_info_data_t timebase {};
        mach_timebase_info (&timebase);

        report.line ("settings: " + s.settingsFile.getFullPathName());
        report.line ("input: " + setup.inputDeviceName + "  output: " + setup.outputDeviceName);
        report.line ("app rate " + String (device->getCurrentSampleRate(), 0) + " Hz, buffer " + String (device->getCurrentBufferSizeSamples())
                     + " samples (callback receives " + String (s.engine.callback.getBlockSize()) + ")");
        describeNumbers (report, "aggregate (JUCE private)", s.engine.getAggregateNumbers());

        const auto rates = s.engine.getDeviceRates();
        const auto mismatch = describeRateMismatch (device->getCurrentSampleRate(), rates.input, rates.output);
        report.line ("device rates after opening: input " + String (rates.input, 0) + " Hz, output " + String (rates.output, 0) + " Hz"
                     + (mismatch.isEmpty() ? String() : " (differ from the app's rate: the system converts)"));

        float usage = 0.0f;
        report.line (hal::readIoCycleUsage (s.engine.getAggregate(), usage) ? "aggregate IOCycleUsage " + fixed (usage, 2)
                                                                            : String ("aggregate has no IOCycleUsage property"));
        report.line ("JUCE reports: input " + String (device->getInputLatencyInSamples()) + " + output " + String (device->getOutputLatencyInSamples())
                     + " samples");
        report.line ("clock: mach tick " + fixed (1.0e9 * timebase.numer / timebase.denom, 2) + " ns, HAL host times in the same domain");
    }

    void save (Report& report, Session& s, const String& prefix)
    {
        const auto file = s.settingsFile.getSiblingFile (prefix + "-" + timestamp() + ".txt");

        if (file.replaceWithText (report.lines.joinIntoString ("\n") + "\n"))
            std::cout << ("report written to " + file.getFullPathName()).toRawUTF8() << std::endl;
        else
            std::cout << "could not write the report next to the settings" << std::endl;
    }

    //==============================================================================
    int measureOnce (const StringArray& args)
    {
        Report report;
        Session s;
        const auto withChain = args.contains ("--with-chain");

        if (const auto problem = openSession (s, args, withChain, report); problem.isNotEmpty())
        {
            report.line ("FAIL: " + problem);
            return 2;
        }

        describeSetup (report, s);
        report.line (withChain ? "chain: real chain in the path (" + String (s.engine.chain.size()) + " slots), burst may be altered by the plugins"
                               : "chain: empty path (plugins not loaded), host and device path only");

        measure::Options options;
        options.runs = jmax (1, getOption (args, "--runs", "10").getIntValue());
        options.withChain = withChain;
        options.captureBuffer = getOption (args, "--capture-buffer", "0").getIntValue();

        const auto result = measure::runSession (s.engine, options);

        if (result.problem.isNotEmpty())
        {
            report.line ("FAIL: " + result.problem);
            verifyRates (s, report);
            return 2;
        }

        report.line ("capture IO buffer during the recording: " + String (result.captureBufferUsed) + " frames");
        describeNumbers (report, "capture device '" + result.captureName + "'", result.capture);
        describeRun (report, result, withChain);
        const auto rateOk = verifyRates (s, report);
        save (report, s, "measure");
        return rateOk && result.trustworthyRuns > 0 ? 0 : 1;
    }

    //==============================================================================
    // The MIN LATENCY button without a window: same tuner, same timing, nothing is saved.
    int tune (const StringArray& args)
    {
        Report report;
        Session s;

        if (const auto problem = openSession (s, args, true, report); problem.isNotEmpty())
        {
            report.line ("FAIL: " + problem);
            return 2;
        }

        describeSetup (report, s);

        {
            const ScopedSetup guard (s.engine);
            LatencyTuner tuner (s.engine);
            tuner.start();
            String last;

            while (tuner.isRunning())
            {
                measure::wait (LatencyTuner::tickMs);

                if (tuner.getProgress() != last)
                {
                    last = tuner.getProgress();
                    report.line ("  " + last);
                }

                tuner.tick();
            }

            report.line (tuner.getResult());

            const auto audio = s.engine.getPersistedAudio();
            report.line ("the settings file would now hold: buffer " + String (audio.device != nullptr ? audio.device->getIntAttribute ("audioDeviceBufferSize") : 0)
                         + ", cycleUsage " + (audio.cycleUsage.has_value() ? fixed (*audio.cycleUsage, 2) : String ("default")) + " (not written)");
        }

        const auto rateOk = verifyRates (s, report);
        save (report, s, "measure-tune");
        return rateOk ? 0 : 1;
    }

    //==============================================================================
    struct SweepRow
    {
        int requested = 0, actual = 0;
        Experiments experiments;
        measure::Result result;
        int loadOverloads = 0;
        HostCallback::Load load;
        bool fastestClean = false, winner = false;
        bool soaked = false;
        int soakOverloads = 0;
        int64 soakLate = 0;
        double soakMaxLoad = 0.0;

        int overloads() const { return loadOverloads + result.overloads; }
        int64 late() const { return load.late + result.load.late; }
    };

    String experimentLabel (const Experiments& e)
    {
        return e.ioCycleUsage.has_value() ? "cycle=" + String (*e.ioCycleUsage, 2) : String ("cycle=default");
    }

    int sweep (const StringArray& args)
    {
        Report report;
        Session s;

        if (const auto problem = openSession (s, args, true, report); problem.isNotEmpty())
        {
            report.line ("FAIL: " + problem);
            return 2;
        }

        describeSetup (report, s);
        report.line ("chain: " + String (s.engine.chain.size()) + " slots in the load phase, empty path in the latency phase");

        measure::Options options;
        options.runs = jmax (1, getOption (args, "--runs", "6").getIntValue());

        const auto original = s.engine.deviceManager.getAudioDeviceSetup();
        const auto maxSize = getOption (args, "--max-buffer", "256").getIntValue();

        Array<int> sizes;

        for (auto size : s.engine.deviceManager.getCurrentAudioDevice()->getAvailableBufferSizes())
            if (size <= maxSize)
                sizes.addIfNotAlreadyThere (size);

        std::vector<Experiments> switches { {} };

        float usage = 0.0f;

        if (s.engine.getAggregate() != kAudioObjectUnknown && hal::readIoCycleUsage (s.engine.getAggregate(), usage))
            for (const auto value : { 0.5f, 0.25f })
            {
                Experiments lowered;
                lowered.ioCycleUsage = value;
                switches.push_back (lowered);
            }

        std::vector<SweepRow> rows;
        const auto soakSeconds = getOption (args, "--soak", "20").getIntValue();
        int winner = -1;
        measure::LoadResult soaked;

        {
            const ScopedSetup guard (s.engine);

            for (auto size : sizes)
                for (const auto& e : switches)
                {
                    SweepRow row;
                    row.requested = size;
                    row.experiments = e;
                    s.engine.setExperiments (e);
                    row.actual = s.engine.setBufferSize (size);

                    if (row.actual > 0)
                    {
                        s.engine.setExperiments (e);
                        measure::wait (400);
                        const auto observed = measure::observeLoad (s.engine, 1500);
                        row.loadOverloads = observed.overloads;
                        row.load = observed.load;
                        row.result = measure::runSession (s.engine, options);
                    }
                    else
                    {
                        row.result.problem = "device did not open";
                    }

                    std::cout << ("  buffer " + String (size) + " " + experimentLabel (e) + " -> block " + String (row.actual) + ", latency "
                                  + (row.result.trustworthyRuns > 0 ? fixed (row.result.latency.median) + " ms" : String ("n/a")) + ", overloads "
                                  + String (row.overloads()) + ", late " + String (row.late())).toRawUTF8() << std::endl;
                    rows.push_back (std::move (row));
                }

            // A few seconds per row can miss an occasional burst (a denoiser working through a frame), so
            // the fastest clean rows are run again for longer with the real chain, and the first to stay
            // clean wins.
            std::vector<size_t> order;

            for (size_t i = 0; i < rows.size(); ++i)
                if (rows[i].result.trustworthyRuns > 0 && rows[i].overloads() == 0 && rows[i].late() == 0)
                    order.push_back (i);

            std::stable_sort (order.begin(), order.end(), [&rows] (size_t a, size_t b)
            {
                return rows[a].result.latency.median < rows[b].result.latency.median;
            });

            for (size_t tried = 0; tried < order.size() && tried < 8 && soakSeconds > 0 && winner < 0; ++tried)
            {
                auto& row = rows[order[tried]];
                std::cout << ("  soak " + String (soakSeconds) + " s: buffer " + String (row.requested) + " " + experimentLabel (row.experiments)).toRawUTF8() << std::endl;
                s.engine.setExperiments (row.experiments);

                if (s.engine.setBufferSize (row.requested) == 0)
                    continue;

                s.engine.setExperiments (row.experiments);
                measure::wait (500);
                const auto result = measure::observeLoad (s.engine, soakSeconds * 1000);
                row.soaked = true;
                row.soakOverloads = result.overloads;
                row.soakLate = result.load.late;
                row.soakMaxLoad = result.load.maxPercent;

                if (result.overloads == 0 && result.load.late == 0)
                {
                    winner = (int) order[tried];
                    soaked = result;
                }
            }
        }

        measure::wait (500);
        const auto restored = s.engine.deviceManager.getAudioDeviceSetup();
        report.line (restored.bufferSize == original.bufferSize && restored.inputDeviceName == original.inputDeviceName
                         && restored.outputDeviceName == original.outputDeviceName
                         ? "device setup restored (buffer " + String (restored.bufferSize) + ")"
                         : "WARNING: device setup differs after the sweep (buffer " + String (restored.bufferSize) + ", was " + String (original.bufferSize) + ")");

        if (winner >= 0)
            rows[(size_t) winner].winner = true;

        std::vector<measure::Candidate> candidates;

        for (auto& row : rows)
            candidates.push_back ({ row.result.trustworthyRuns > 0, row.result.latency.median, row.overloads(), row.late() });

        if (const auto fastestClean = measure::pickBest (candidates); fastestClean >= 0)
            rows[(size_t) fastestClean].fastestClean = true;


        std::stable_sort (rows.begin(), rows.end(), [] (const SweepRow& a, const SweepRow& b)
        {
            return measure::isFaster ({ a.result.trustworthyRuns > 0, a.result.latency.median, 0, 0 },
                                      { b.result.trustworthyRuns > 0, b.result.latency.median, 0, 0 });
        });

        const SweepRow* best = nullptr;

        for (auto& row : rows)
            if (row.winner)
                best = &row;

        report.line ("");
        report.line ("sorted by median latency (input acquired -> available on the output device, empty path)");
        report.line ("buffer  block  switches              latency ms  min      max      declared  overl  late  load%mean/max  corr  soak");

        for (auto& row : rows)
        {
            const auto& r = row.result;
            const auto text = String (row.requested).paddedRight (' ', 8) + String (row.actual).paddedRight (' ', 7)
                            + experimentLabel (row.experiments).paddedRight (' ', 22)
                            + (r.trustworthyRuns > 0 ? fixed (r.latency.median).paddedRight (' ', 12) + fixed (r.latency.minimum).paddedRight (' ', 9)
                                                       + fixed (r.latency.maximum).paddedRight (' ', 9) + fixed (r.declared.median).paddedRight (' ', 10)
                                                     : (r.problem.isNotEmpty() ? r.problem : String ("no trustworthy run")).paddedRight (' ', 40))
                            + String (row.overloads()).paddedRight (' ', 7) + String (row.late()).paddedRight (' ', 6)
                            + (fixed (row.load.meanPercent, 1) + "/" + fixed (row.load.maxPercent, 1)).paddedRight (' ', 15)
                            + (r.trustworthyRuns > 0 ? fixed (r.runs.empty() ? 0.0 : r.runs.front().coefficient, 2) : String()).paddedRight (' ', 6)
                            + (row.soaked ? String (soakSeconds) + " s: overl " + String (row.soakOverloads) + " late " + String (row.soakLate)
                                                + " max load " + fixed (row.soakMaxLoad, 1) + " %"
                                          : String());
            report.line (text + (row.winner ? "   <== WINNER: fastest that stayed clean for the soak"
                                            : row.fastestClean ? "   <== fastest with zero overloads in the short run" : ""));
        }

        report.line ("");

        if (best != nullptr)
        {
            report.line ("best: buffer " + String (best->actual) + " " + experimentLabel (best->experiments) + ", latency "
                         + fixed (best->result.latency.median) + " ms (declared " + fixed (best->result.declared.median) + " ms), "
                         + String (soakSeconds) + " s soak with the real chain: " + String (soaked.overloads) + " overloads, "
                         + String (soaked.load.late) + " late callbacks, callback load mean " + fixed (soaked.load.meanPercent, 1) + " % max "
                         + fixed (soaked.load.maxPercent, 1) + " %");
        }
        else
        {
            report.line ("no configuration was measurable, free of overloads and late callbacks, and stayed clean for the soak");
        }

        const auto rateOk = verifyRates (s, report);
        save (report, s, "measure-sweep");
        return rateOk ? 0 : 1;
    }
}

bool isMeasureMode (const StringArray& args)
{
    return args.contains ("--measure") || args.contains ("--measure-sweep") || args.contains ("--tune");
}

int runMeasureCommand (const StringArray& args)
{
    const auto explicitSettings = args.contains ("--settings");

    if (! explicitSettings && ! args.contains ("--use-default-settings"))
    {
        std::cout << "--measure needs --settings <file> (a copy of the settings is fine), or --use-default-settings "
                     "when no other instance is running" << std::endl;
        return 64;
    }

    // A settings file that names the same devices opens them again, so --settings does not make this safe.
    if (anotherInstanceRunning() && ! args.contains ("--force"))
    {
        std::cout << "another " << productName << " process is running and may hold the devices these settings name; a measurement "
                     "would open them again and interrupt its audio. Quit it, or pass --force if it is not using them" << std::endl;
        return 3;
    }

    if (args.contains ("--tune"))
        return tune (args);

    return args.contains ("--measure-sweep") ? sweep (args) : measureOnce (args);
}

} // namespace mfx
