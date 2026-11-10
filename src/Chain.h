#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

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
    void remove (int index);
    void move (int index, int newIndex);
    void setBypassed (int index, bool shouldBypass);

    // Connects input node -> live slots in order -> output node. A bypassed slot stays wired in and
    // is bypassed by the graph, so toggling it does not change the reported latency.
    // Call again whenever the device's channel configuration changes.
    void rewire();

    std::unique_ptr<XmlElement> toXml() const;
    StringArray restore (const XmlElement&);

    bool isEquivalentTo (const Chain&) const;

private:
    void clear();
    bool instantiate (Slot&);

    void audioProcessorParameterChanged (AudioProcessor*, int, float) override {}
    void audioProcessorChanged (AudioProcessor*, const ChangeDetails&) override;
    void handleAsyncUpdate() override;

    AudioPluginFormatManager& formats;
    AudioProcessorGraph graph;
    AudioProcessorGraph::Node::Ptr inputNode, outputNode;
    std::vector<Slot> slots;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Chain)
};

} // namespace mfx
