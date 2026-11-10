#include "Chain.h"

#include <optional>
#include <vector>

namespace mfx
{

namespace
{
    constexpr double defaultSampleRate = 48000.0;
    constexpr int defaultBlockSize = 512;

    // A stereo pass-through that reads the clock. One goes in front of a plugin and one behind it, and
    // the time between the two is the plugin's. The buffer is not touched, so the graph can run both in
    // place; the audio thread does a clock read and a few atomics in each.
    class TimingProbe final : public AudioProcessor
    {
    public:
        enum class Edge { before, after };

        TimingProbe (std::shared_ptr<ProcessTimer> timerToUse, Edge edgeToUse)
            : AudioProcessor (BusesProperties().withInput ("In", AudioChannelSet::stereo()).withOutput ("Out", AudioChannelSet::stereo())),
              timer (std::move (timerToUse)), edge (edgeToUse)
        {
        }

        void processBlock (juce::AudioBuffer<float>&, MidiBuffer&) override
        {
            if (edge == Edge::before)
                timer->begin();
            else
                timer->end();
        }

        using AudioProcessor::processBlock;

        const String getName() const override           { return "Timing probe"; }
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

    private:
        std::shared_ptr<ProcessTimer> timer;
        Edge edge;
    };

    // Hosts a stereo effect chain; fall back to the plugin's own layout when stereo is refused
    // (mono-only voice tools, for example).
    void preferStereoMainBuses (AudioProcessor& processor)
    {
        auto layout = processor.getBusesLayout();

        if (processor.getBusCount (true) > 0)
            layout.getChannelSet (true, 0) = AudioChannelSet::stereo();

        if (processor.getBusCount (false) > 0)
            layout.getChannelSet (false, 0) = AudioChannelSet::stereo();

        if (processor.checkBusesLayoutSupported (layout))
            processor.setBusesLayout (layout);
    }
}

Array<PluginDescription> findPluginsInFile (AudioPluginFormatManager& formats, const File& file)
{
    Array<PluginDescription> result;

    for (auto* format : formats.getFormats())
    {
        if (! format->fileMightContainThisPluginType (file.getFullPathName()))
            continue;

        OwnedArray<PluginDescription> found;
        format->findAllTypesForFile (found, file.getFullPathName());

        for (auto* description : found)
            result.add (*description);
    }

    return result;
}

//==============================================================================
Chain::Chain (AudioPluginFormatManager& formatManager)
    : formats (formatManager)
{
    // Mono mic in, stereo out until a real device reconfigures the graph.
    graph.setPlayConfigDetails (1, 2, defaultSampleRate, defaultBlockSize);

    using IO = AudioProcessorGraph::AudioGraphIOProcessor;
    inputNode  = graph.addNode (std::make_unique<IO> (IO::audioInputNode),  std::nullopt, AudioProcessorGraph::UpdateKind::none);
    outputNode = graph.addNode (std::make_unique<IO> (IO::audioOutputNode), std::nullopt, AudioProcessorGraph::UpdateKind::none);
}

Chain::~Chain()
{
    cancelPendingUpdate();
    clear();
}

bool Chain::instantiate (Slot& slot)
{
    const auto sampleRate = graph.getSampleRate() > 0 ? graph.getSampleRate() : defaultSampleRate;
    const auto blockSize = graph.getBlockSize() > 0 ? graph.getBlockSize() : defaultBlockSize;

    String error;
    auto instance = formats.createPluginInstance (slot.description, sampleRate, blockSize, error);

    if (instance == nullptr)
    {
        slot.error = error.isEmpty() ? String ("unknown error") : error;
        return false;
    }

    attach (slot, std::move (instance));
    return true;
}

void Chain::attach (Slot& slot, std::unique_ptr<AudioProcessor> instance)
{
    preferStereoMainBuses (*instance);

    if (slot.state.getSize() > 0)
        instance->setStateInformation (slot.state.getData(), (int) slot.state.getSize());

    instance->addListener (this);

    slot.node = graph.addNode (std::move (instance), std::nullopt, AudioProcessorGraph::UpdateKind::none);
    slot.error.clear();
}

void Chain::addProcessor (std::unique_ptr<AudioProcessor> processor, const PluginDescription& description)
{
    Slot slot;
    slot.description = description;
    attach (slot, std::move (processor));
    slots.push_back (std::move (slot));
    rewire();
}

bool Chain::add (const PluginDescription& description, String& error)
{
    Slot slot;
    slot.description = description;

    if (! instantiate (slot))
    {
        error = slot.error;
        return false;
    }

    slots.push_back (std::move (slot));
    rewire();
    return true;
}

void Chain::remove (int index)
{
    if (! isPositiveAndBelow (index, size()))
        return;

    removeProbes (slots[(size_t) index]);

    if (auto node = slots[(size_t) index].node)
    {
        node->getProcessor()->removeListener (this);
        graph.removeNode (node->nodeID, AudioProcessorGraph::UpdateKind::none);
    }

    slots.erase (slots.begin() + index);
    rewire();
}

void Chain::move (int index, int newIndex)
{
    if (! isPositiveAndBelow (index, size()) || ! isPositiveAndBelow (newIndex, size()) || index == newIndex)
        return;

    auto slot = std::move (slots[(size_t) index]);
    slots.erase (slots.begin() + index);
    slots.insert (slots.begin() + newIndex, std::move (slot));
    rewire();
}

void Chain::setBypassed (int index, bool shouldBypass)
{
    if (! isPositiveAndBelow (index, size()))
        return;

    slots[(size_t) index].bypassed = shouldBypass;
    rewire();
}

void Chain::setForceBypassAll (bool shouldForce)
{
    if (forceBypassAll == shouldForce)
        return;

    forceBypassAll = shouldForce;
    rewire();
}

void Chain::addProbes (Slot& slot)
{
    using Edge = TimingProbe::Edge;

    if (slot.probeBefore == nullptr)
        slot.probeBefore = graph.addNode (std::make_unique<TimingProbe> (slot.timer, Edge::before), std::nullopt, AudioProcessorGraph::UpdateKind::none);

    if (slot.probeAfter == nullptr)
        slot.probeAfter = graph.addNode (std::make_unique<TimingProbe> (slot.timer, Edge::after), std::nullopt, AudioProcessorGraph::UpdateKind::none);
}

// The running render sequence keeps the nodes it uses alive, so a probe can leave the graph while the
// audio thread is still passing signal through it.
void Chain::removeProbes (Slot& slot)
{
    for (auto* probe : { &slot.probeBefore, &slot.probeAfter })
        if (*probe != nullptr)
        {
            graph.removeNode ((*probe)->nodeID, AudioProcessorGraph::UpdateKind::none);
            probe->reset();
        }
}

void Chain::pollProcessTimes()
{
    for (auto& slot : slots)
        slot.load.push (slot.timer->drain());
}

void Chain::resetProcessTimes()
{
    for (auto& slot : slots)
    {
        slot.timer->drain();
        slot.load.clear();
    }
}

void Chain::clear()
{
    for (auto& slot : slots)
        if (slot.node != nullptr)
            slot.node->getProcessor()->removeListener (this);

    slots.clear();

    for (auto& connection : graph.getConnections())
        graph.removeConnection (connection, AudioProcessorGraph::UpdateKind::none);

    const auto nodes = graph.getNodes();

    for (auto* node : nodes)
        if (node != inputNode.get() && node != outputNode.get())
            graph.removeNode (node->nodeID, AudioProcessorGraph::UpdateKind::none);
}

void Chain::rewire()
{
    using Update = AudioProcessorGraph::UpdateKind;
    using NodeAndChannel = AudioProcessorGraph::NodeAndChannel;

    // The I/O nodes only learn the device's channel counts when the graph rebuilds.
    graph.rebuild();

    for (auto& connection : graph.getConnections())
        graph.removeConnection (connection, Update::none);

    const auto numIn  = inputNode->getProcessor()->getTotalNumOutputChannels();
    const auto numOut = outputNode->getProcessor()->getTotalNumInputChannels();

    // A mono mic is fanned out to both sides; two or more input channels map to L and R.
    struct Signal { std::optional<NodeAndChannel> left, right; };
    Signal signal;

    if (numIn > 0)
    {
        signal.left  = NodeAndChannel { inputNode->nodeID, 0 };
        signal.right = NodeAndChannel { inputNode->nodeID, numIn > 1 ? 1 : 0 };
    }

    // An unconnected node would still be processed, so it is also suspended (the graph then only
    // clears its buffer). Resuming starts from a clean state instead of stale delay lines.
    // The graph clears the buffer of a suspended node even while the running render sequence still
    // connects it, which would silence the signal for as long as this function takes. So a node that
    // leaves the path is suspended only after the rebuilt sequence has replaced the running one.
    std::vector<AudioProcessor*> toSuspend;

    for (auto& slot : slots)
    {
        if (slot.node == nullptr)
            continue;

        auto& processor = *slot.node->getProcessor();
        const auto inPath = ! slot.bypassed && ! forceBypassAll;

        if (inPath && processor.isSuspended())
        {
            const ScopedLock lock (processor.getCallbackLock());
            processor.reset();
            processor.suspendProcessing (false);
        }
        else if (! inPath && ! processor.isSuspended())
        {
            toSuspend.push_back (&processor);
        }

        if (! inPath)
        {
            removeProbes (slot);
            continue;
        }

        const auto id = slot.node->nodeID;
        const auto pluginIn  = processor.getMainBusNumInputChannels();
        const auto pluginOut = processor.getMainBusNumOutputChannels();

        // signal -> probe -> plugin -> probe -> signal. The probes are stereo, so what leaves a mono
        // plugin is fanned out to both of the second probe's inputs, as it is to the next plugin.
        // The graph only runs the first probe before the plugin and the second after it when the
        // connections force that order, so a plugin that takes no input or gives no output, or a chain
        // with no input signal, is left unwrapped and is not timed.
        const auto timed = signal.left && pluginIn >= 1 && pluginOut >= 1;

        if (timed)
            addProbes (slot);
        else
            removeProbes (slot);

        if (timed)
        {
            const auto before = slot.probeBefore->nodeID;
            graph.addConnection ({ *signal.left, { before, 0 } }, Update::none);
            graph.addConnection ({ *signal.right, { before, 1 } }, Update::none);
            signal.left  = NodeAndChannel { before, 0 };
            signal.right = NodeAndChannel { before, 1 };
        }

        if (pluginIn >= 1 && signal.left)
            graph.addConnection ({ *signal.left, { id, 0 } }, Update::none);

        if (pluginIn >= 2 && signal.right)
            graph.addConnection ({ *signal.right, { id, 1 } }, Update::none);

        if (pluginOut >= 2)
        {
            signal.left  = NodeAndChannel { id, 0 };
            signal.right = NodeAndChannel { id, 1 };
        }
        else if (pluginOut == 1)
        {
            signal.left = signal.right = NodeAndChannel { id, 0 };
        }

        if (timed)
        {
            const auto after = slot.probeAfter->nodeID;
            graph.addConnection ({ *signal.left, { after, 0 } }, Update::none);
            graph.addConnection ({ *signal.right, { after, 1 } }, Update::none);
            signal.left  = NodeAndChannel { after, 0 };
            signal.right = NodeAndChannel { after, 1 };
        }
    }

    if (numOut >= 1 && signal.left)
        graph.addConnection ({ *signal.left, { outputNode->nodeID, 0 } }, Update::none);

    if (numOut >= 2 && signal.right)
        graph.addConnection ({ *signal.right, { outputNode->nodeID, 1 } }, Update::none);

    graph.rebuild();

    for (auto* processor : toSuspend)
        processor->suspendProcessing (true);
}

void Chain::audioProcessorChanged (AudioProcessor*, const ChangeDetails& details)
{
    if (details.latencyChanged)
        triggerAsyncUpdate();
}

void Chain::handleAsyncUpdate()
{
    graph.rebuild();
}

//==============================================================================
std::unique_ptr<XmlElement> Chain::toXml() const
{
    auto root = std::make_unique<XmlElement> ("CHAIN");

    for (auto& slot : slots)
    {
        auto state = slot.state;

        if (slot.node != nullptr)
        {
            state.reset();
            slot.node->getProcessor()->getStateInformation (state);
        }

        auto* element = root->createNewChildElement ("SLOT");
        element->setAttribute ("bypassed", slot.bypassed);
        element->setAttribute ("state", state.toBase64Encoding());
        element->addChildElement (slot.description.createXml().release());
    }

    return root;
}

StringArray Chain::restore (const XmlElement& xml)
{
    clear();

    StringArray problems;

    for (auto* element : xml.getChildWithTagNameIterator ("SLOT"))
    {
        Slot slot;
        auto* pluginElement = element->getChildByName ("PLUGIN");

        if (pluginElement == nullptr || ! slot.description.loadFromXml (*pluginElement))
        {
            problems.add ("Unreadable slot in settings, dropped");
            continue;
        }

        slot.bypassed = element->getBoolAttribute ("bypassed");
        slot.state.fromBase64Encoding (element->getStringAttribute ("state"));

        slots.push_back (std::move (slot));

        if (! instantiate (slots.back()))
            problems.add (slots.back().description.name + ": " + slots.back().error);
    }

    rewire();
    return problems;
}

bool Chain::isEquivalentTo (const Chain& other) const
{
    if (size() != other.size())
        return false;

    for (int i = 0; i < size(); ++i)
    {
        auto& a = slots[(size_t) i];
        auto& b = other.slots[(size_t) i];

        if (! a.description.isDuplicateOf (b.description)
            || a.bypassed != b.bypassed
            || a.isLive() != b.isLive())
            return false;
    }

    return true;
}

} // namespace mfx
