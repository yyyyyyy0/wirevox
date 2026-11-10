#include "CommandLine.h"
#include "Chain.h"

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
    return args.contains ("--selftest") || args.contains ("--list-devices");
}

int runCommandLine (const StringArray& args)
{
    const ScopedJuceInitialiser_GUI initialiser;

    if (args.contains ("--list-devices"))
        return listDevices();

    const auto index = args.indexOf ("--selftest");

    if (index + 1 >= args.size())
    {
        say ("usage: mic-fx-host --selftest <path to .vst3>");
        return 64;
    }

    return selfTest (args[index + 1]);
}

} // namespace mfx
