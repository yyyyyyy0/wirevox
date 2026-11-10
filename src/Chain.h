#pragma once

#include "ProcessTime.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

namespace mfx
{
using namespace juce;

// One entry of the effect chain. `description`, `bypassed` and `state` are what gets persisted;
// `node` is the live instance inside the graph and is null when the plugin failed to load.
struct Slot
{
    PluginDescription description;
    bool bypassed = false;
    MemoryBlock state;
    AudioProcessorGraph::Node::Ptr node;
    String error;

    // Time the plugin's processBlock takes. `timer` is shared with the two probe processors the chain
    // puts around the plugin while it is in the path (they are null while it is out of the path);
    // `load` is the message thread's own record of what was drained from it.
    std::shared_ptr<ProcessTimer> timer = std::make_shared<ProcessTimer>();
    AudioProcessorGraph::Node::Ptr probeBefore, probeAfter;
    ProcessWindows load;

    bool isLive() const noexcept { return node != nullptr; }
};

Array<PluginDescription> findPluginsInFile (AudioPluginFormatManager&, const File&);

// The ordered slot list is the single source of truth. The AudioProcessorGraph is derived from it
// by rewire(). Every method must be called on the message thread.
class Chain final : private AudioProcessorListener,
                    private AsyncUpdater
{
public:
    explicit Chain (AudioPluginFormatManager&);
    ~Chain() override;

    AudioProcessorGraph& getGraph() noexcept { return graph; }

    int size() const noexcept                   { return (int) slots.size(); }
    const Slot& operator[] (int index) const    { return slots[(size_t) index]; }

    bool add (const PluginDescription&, String& error);

    // For tests: puts an already built processor into the chain as if it had been loaded from `description`.
    void addProcessor (std::unique_ptr<AudioProcessor>, const PluginDescription&);
    void remove (int index);
    void move (int index, int newIndex);
    void setBypassed (int index, bool shouldBypass);

    // Takes every slot out of the signal path without touching the slots' own bypass flags. The latency
    // measurement uses it so that plugins do not alter the test burst.
    void setForceBypassAll (bool);

    // Moves what the audio thread timed into each slot's `load`. Call from the message thread on a timer.
    void pollProcessTimes();
    void resetProcessTimes();

    // Connects input node -> live slots in order -> output node. A bypassed slot is left out of the path
    // entirely and its processor is suspended, so it adds neither latency nor CPU; its instance and state
    // stay in the slot. (A soft bypass inside the plugin keeps its latency and its processing.)
    // A slot in the path is wrapped in two pass-through probes that read the clock, which is how its
    // processing time is timed; a slot out of the path has none, so it costs nothing at all.
    // Call again whenever the device's channel configuration changes.
    void rewire();

    std::unique_ptr<XmlElement> toXml() const;
    StringArray restore (const XmlElement&);

    bool isEquivalentTo (const Chain&) const;

private:
    void clear();
    bool instantiate (Slot&);
    void attach (Slot&, std::unique_ptr<AudioProcessor>);
    void addProbes (Slot&);
    void removeProbes (Slot&);

    void audioProcessorParameterChanged (AudioProcessor*, int, float) override {}
    void audioProcessorChanged (AudioProcessor*, const ChangeDetails&) override;
    void handleAsyncUpdate() override;

    AudioPluginFormatManager& formats;
    AudioProcessorGraph graph;
    AudioProcessorGraph::Node::Ptr inputNode, outputNode;
    std::vector<Slot> slots;
    bool forceBypassAll = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Chain)
};

} // namespace mfx
