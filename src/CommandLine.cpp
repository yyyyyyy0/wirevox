#include "CommandLine.h"
#include "Chain.h"
#include "Product.h"
#include "Settings.h"
#include "Theme.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
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

    constexpr double selfTestSampleRate = 48000.0;
    constexpr int selfTestBlockSize = 64;
    constexpr double selfTestSeconds = 3.0;

    void printUsage()
    {
        const String name (productName);

        say ("usage: " + name + " [--settings <file>]");
        say ("       " + name + " --selftest <path to .vst3>");
        say ("       " + name + " --selftest-settings");
        say ("       " + name + " --list-devices");
        say ("");
        say ("  --settings <file>    use this settings file instead of the default one (no migration)");
        say ("  --selftest           load a plugin through the chain and process audio without a device");
        say ("  --selftest-settings  check the settings carry-over from the pre-rename folder and the theme id");
        say ("  --list-devices       print input and output device names");
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
        AudioBuffer<float> buffer (2, selfTestBlockSize);
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

        check (defaultSettingsFile (root).getFullPathName().contains (String ("/") + productName + "/settings.xml"),
               "the new location is named after the product");

        root.deleteRecursively();
        say (ok ? "SELFTEST SETTINGS OK" : "SELFTEST SETTINGS FAILED");
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

        const auto success = activePeak > 1.0e-6f && bypassedOk && roundTripOk;
        say (success ? "SELFTEST OK" : "SELFTEST FAILED");
        return success ? 0 : 1;
    }
}

bool isCommandLineMode (const StringArray& args)
{
    return args.contains ("--selftest") || args.contains ("--selftest-settings")
        || args.contains ("--list-devices") || args.contains ("--help");
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

    if (args.contains ("--selftest-settings"))
        return selfTestSettings();

    const auto index = args.indexOf ("--selftest");

    if (index + 1 >= args.size())
    {
        printUsage();
        return 64;
    }

    return selfTest (args[index + 1]);
}

} // namespace mfx
