#include "MainComponent.h"
#include "Settings.h"

namespace mfx
{

namespace
{
    constexpr int rowHeight = 38;
    constexpr int timerMs = 250;
    constexpr int ticksPerSave = 120;

    String samplesAndMs (int samples, double sampleRate)
    {
        const auto ms = sampleRate > 0 ? 1000.0 * samples / sampleRate : 0.0;
        return String (samples) + " samples (" + String (ms, 2) + " ms)";
    }
}

//==============================================================================
class MainComponent::EditorWindow final : public DocumentWindow
{
public:
    EditorWindow (MainComponent& ownerIn, AudioProcessor& processorIn)
        : DocumentWindow (processorIn.getName(), Colours::darkgrey, DocumentWindow::closeButton),
          owner (ownerIn), processor (processorIn)
    {
        setUsingNativeTitleBar (true);

        AudioProcessorEditor* editor = processor.hasEditor() ? processor.createEditorAndMakeActive() : nullptr;
        setContentOwned (editor != nullptr ? editor : new GenericAudioProcessorEditor (processor), true);
        setResizable (getContentComponent()->getWidth() > 0 && editor != nullptr && editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        owner.closeEditor (&processor);
    }

private:
    MainComponent& owner;
    AudioProcessor& processor;
};

//==============================================================================
// A Label sizes every line from the first font, so Japanese glyphs from the fallback font overlap.
// AttributedString lays text out through CoreText, which accounts for the fallback font's height.
class MainComponent::Readout final : public Component
{
public:
    void setText (const String& newText)
    {
        if (newText == text)
            return;

        text = newText;
        repaint();
    }

    void paint (Graphics& g) override
    {
        AttributedString attributed;
        attributed.setLineSpacing (2.0f);
        attributed.append (text, FontOptions (13.0f), Colours::white);
        attributed.draw (g, getLocalBounds().toFloat());
    }

private:
    String text;
};

//==============================================================================
class MainComponent::ChainRow final : public Component
{
public:
    explicit ChainRow (MainComponent& ownerIn)
        : owner (ownerIn)
    {
        addAndMakeVisible (name);
        addAndMakeVisible (bypass);
        addAndMakeVisible (editor);
        addAndMakeVisible (up);
        addAndMakeVisible (down);
        addAndMakeVisible (remove);

        bypass.onClick = [this] { owner.setSlotBypassed (row, bypass.getToggleState()); };
        editor.onClick = [this] { owner.openEditor (row); };
        up.onClick     = [this] { owner.moveSlot (row, -1); };
        down.onClick   = [this] { owner.moveSlot (row, 1); };
        remove.onClick = [this] { owner.removeSlot (row); };
    }

    void update (int newRow)
    {
        row = newRow;
        auto& slot = owner.chain[row];

        auto text = String (row + 1) + ".  " + slot.description.name;

        if (! slot.isLive())
            text << ja ("   [読み込み失敗]");

        name.setText (text, dontSendNotification);
        name.setColour (Label::textColourId, slot.isLive() ? Colours::white : Colours::salmon);
        bypass.setToggleState (slot.bypassed, dontSendNotification);
        editor.setEnabled (slot.isLive());
        up.setEnabled (row > 0);
        down.setEnabled (row < owner.chain.size() - 1);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (4, 5);

        remove.setBounds (area.removeFromRight (56));
        area.removeFromRight (4);
        down.setBounds (area.removeFromRight (34));
        area.removeFromRight (4);
        up.setBounds (area.removeFromRight (34));
        area.removeFromRight (4);
        editor.setBounds (area.removeFromRight (70));
        area.removeFromRight (6);
        bypass.setBounds (area.removeFromRight (84));
        name.setBounds (area);
    }

private:
    MainComponent& owner;
    int row = 0;

    Label name;
    ToggleButton bypass { ja ("バイパス") };
    TextButton editor { ja ("編集") }, up { ja ("上") }, down { ja ("下") }, remove { ja ("削除") };
};

//==============================================================================
class MainComponent::ChainListModel final : public ListBoxModel
{
public:
    explicit ChainListModel (MainComponent& ownerIn) : owner (ownerIn) {}

    int getNumRows() override { return owner.chain.size(); }

    void paintListBoxItem (int row, Graphics& g, int width, int height, bool) override
    {
        g.setColour (Colours::white.withAlpha (row % 2 == 0 ? 0.04f : 0.0f));
        g.fillRect (0, 0, width, height);
    }

    Component* refreshComponentForRow (int row, bool, Component* existing) override
    {
        if (! isPositiveAndBelow (row, owner.chain.size()))
        {
            delete existing;
            return nullptr;
        }

        auto* component = existing != nullptr ? static_cast<ChainRow*> (existing) : new ChainRow (owner);
        component->update (row);
        return component;
    }

private:
    MainComponent& owner;
};

//==============================================================================
MainComponent::MainComponent()
    : chain (formatManager),
      deviceSelector (deviceManager, 0, 256, 0, 256, false, false, false, false)
{
    addDefaultFormatsToManager (formatManager);

    migrateLegacySettingsIfNeeded();
    const auto settings = loadSettings();
    StringArray restoreProblems;

    // Without saved settings, initialise() would open the system default devices. An empty setup
    // keeps every device closed until the user picks one: a mic wired straight to the speakers
    // on first launch would feed back.
    const XmlElement noDevice ("DEVICESETUP");
    auto* savedDevice = settings != nullptr ? settings->getChildByName ("DEVICESETUP") : nullptr;
    deviceManager.initialise (1, 2, savedDevice != nullptr ? savedDevice : &noDevice, false);

    bool skippedChainAfterCrash = false;

    if (settings != nullptr)
    {
        if (auto* chainXml = settings->getChildByName ("CHAIN"))
        {
            // A plugin that segfaults while loading takes the whole process down. The marker it
            // leaves behind makes the next launch skip the chain instead of crashing again.
            const auto marker = getSettingsFile().getSiblingFile ("restoring");

            if (marker.existsAsFile())
            {
                getSettingsFile().copyFileTo (getSettingsFile().getSiblingFile ("settings.crashed.xml"));
                skippedChainAfterCrash = true;
            }
            else
            {
                marker.create();
                restoreProblems = chain.restore (*chainXml);
            }

            marker.deleteFile();
        }
    }

    player.setProcessor (&chain.getGraph());
    deviceManager.addAudioCallback (&player);
    chain.rewire();
    deviceManager.addChangeListener (this);

    deviceViewport.setViewedComponent (&deviceSelector, false);
    deviceViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (deviceViewport);

    listModel = std::make_unique<ChainListModel> (*this);
    chainList.setModel (listModel.get());
    chainList.setRowHeight (rowHeight);
    chainList.setColour (ListBox::backgroundColourId, Colours::black.withAlpha (0.25f));
    addAndMakeVisible (chainList);

    addButton.onClick = [this] { chooseFiles(); };
    addAndMakeVisible (addButton);
    dropHint.setFont (FontOptions (12.0f));
    dropHint.setColour (Label::textColourId, Colours::grey);
    addAndMakeVisible (dropHint);

    readout = std::make_unique<Readout>();
    addAndMakeVisible (*readout);

    status.setJustificationType (Justification::centredLeft);
    status.setColour (Label::textColourId, Colours::orange);
    addAndMakeVisible (status);

    if (skippedChainAfterCrash)
        showStatus (ja ("前回は読み込み中に終了したため、チェーンを読み込んでいません（元の設定: settings.crashed.xml）"));
    else if (! restoreProblems.isEmpty())
        showStatus (ja ("復元できなかったプラグイン: ") + restoreProblems.joinIntoString (" / "));

    setSize (1000, 720);
    updateReadout();
    startTimer (timerMs);
}

MainComponent::~MainComponent()
{
    stopTimer();
    persist();
    closeAllEditors();
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);
    deviceManager.closeAudioDevice();
}

void MainComponent::paint (Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto left = area.removeFromLeft (jlimit (380, 520, area.getWidth() * 2 / 5));
    deviceViewport.setBounds (left);
    deviceSelector.setSize (deviceViewport.getMaximumVisibleWidth(), deviceSelector.getHeight());

    area.removeFromLeft (12);

    status.setBounds (area.removeFromBottom (24));

    auto top = area.removeFromTop (28);
    addButton.setBounds (top.removeFromLeft (150));
    dropHint.setBounds (top.withTrimmedLeft (8));
    area.removeFromTop (6);

    readout->setBounds (area.removeFromBottom (190));
    area.removeFromBottom (6);

    chainList.setBounds (area);
}

//==============================================================================
bool MainComponent::isInterestedInFileDrag (const StringArray& files)
{
    for (auto& path : files)
        if (File (path).hasFileExtension ("vst3"))
            return true;

    return false;
}

void MainComponent::filesDropped (const StringArray& files, int, int)
{
    Array<File> plugins;

    for (auto& path : files)
        if (File (path).hasFileExtension ("vst3"))
            plugins.add (File (path));

    addPluginFiles (plugins);
}

void MainComponent::chooseFiles()
{
    chooser = std::make_unique<FileChooser> (ja ("VST3 プラグインを選択"),
                                             File ("/Library/Audio/Plug-Ins/VST3"),
                                             "*.vst3");

    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles
                              | FileBrowserComponent::canSelectMultipleItems,
                          [this] (const FileChooser& fc) { addPluginFiles (fc.getResults()); });
}

void MainComponent::addPluginFiles (const Array<File>& files)
{
    for (auto& file : files)
    {
        const auto found = findPluginsInFile (formatManager, file);

        if (found.isEmpty())
        {
            showStatus (file.getFileName() + ja (" にプラグインが見つかりません"));
        }
        else if (found.size() == 1)
        {
            addDescription (found.getFirst());
        }
        else
        {
            PopupMenu menu;

            for (int i = 0; i < found.size(); ++i)
                menu.addItem (i + 1, found[i].name);

            menu.showMenuAsync (PopupMenu::Options(), [safe = SafePointer<MainComponent> (this), found] (int result)
            {
                if (safe != nullptr && result > 0)
                    safe->addDescription (found[result - 1]);
            });
        }
    }
}

void MainComponent::addDescription (const PluginDescription& description)
{
    String error;

    if (chain.add (description, error))
    {
        showStatus ({});
        chainEdited();
    }
    else
    {
        showStatus (description.name + ja (" を読み込めませんでした: ") + error);
    }
}

//==============================================================================
void MainComponent::openEditor (int index)
{
    if (! isPositiveAndBelow (index, chain.size()) || ! chain[index].isLive())
        return;

    auto* processor = chain[index].node->getProcessor();

    if (auto it = editors.find (processor); it != editors.end())
        it->second->toFront (true);
    else
        editors[processor] = std::make_unique<EditorWindow> (*this, *processor);
}

void MainComponent::closeEditor (AudioProcessor* processor)
{
    // The window is deleted from inside its own close callback otherwise.
    MessageManager::callAsync ([safe = SafePointer<MainComponent> (this), processor]
    {
        if (safe != nullptr)
        {
            safe->editors.erase (processor);
            safe->persist();
        }
    });
}

void MainComponent::closeAllEditors()
{
    editors.clear();
}

void MainComponent::removeSlot (int index)
{
    if (! isPositiveAndBelow (index, chain.size()))
        return;

    if (chain[index].isLive())
        editors.erase (chain[index].node->getProcessor());

    chain.remove (index);
    chainEdited();
}

void MainComponent::moveSlot (int index, int delta)
{
    chain.move (index, index + delta);
    chainEdited();
}

void MainComponent::setSlotBypassed (int index, bool shouldBypass)
{
    chain.setBypassed (index, shouldBypass);
    persist();
}

void MainComponent::chainEdited()
{
    chainList.updateContent();
    chainList.repaint();
    persist();
    updateReadout();
}

//==============================================================================
void MainComponent::showStatus (const String& text)
{
    status.setText (text, dontSendNotification);
}

void MainComponent::persist()
{
    XmlElement root ("MIC_FX_HOST");

    if (auto device = deviceManager.createStateXml())
        root.addChildElement (device.release());

    root.addChildElement (chain.toXml().release());

    if (! saveSettings (root))
        showStatus (ja ("設定ファイルを保存できませんでした: ") + getSettingsFile().getFullPathName());

    ticksSinceSave = 0;
}

void MainComponent::updateReadout()
{
    String text;
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        text << "オーディオデバイスが開いていません。\n左のパネルで入力・出力デバイスを選んでください。\n";
    }
    else
    {
        const auto sampleRate = device->getCurrentSampleRate();
        const auto inLatency = device->getInputLatencyInSamples();
        const auto outLatency = device->getOutputLatencyInSamples();
        const auto pluginLatency = chain.getGraph().getLatencySamples();
        const auto xruns = device->getXRunCount();
        const auto setup = deviceManager.getAudioDeviceSetup();

        text << "入力デバイス: " << setup.inputDeviceName << "  ("
             << device->getActiveInputChannels().countNumberOfSetBits() << " ch)\n"
             << "出力デバイス: " << setup.outputDeviceName << "  ("
             << device->getActiveOutputChannels().countNumberOfSetBits() << " ch)\n"
             << "サンプルレート: " << String (sampleRate, 0) << " Hz\n"
             << "バッファサイズ: " << samplesAndMs (device->getCurrentBufferSizeSamples(), sampleRate) << "\n"
             << "入力デバイス遅延（バッファ込み）: " << samplesAndMs (inLatency, sampleRate) << "\n"
             << "出力デバイス遅延（バッファ込み）: " << samplesAndMs (outLatency, sampleRate) << "\n"
             << "プラグイン遅延合計: " << samplesAndMs (pluginLatency, sampleRate) << "\n"
             << "合計: " << samplesAndMs (inLatency + outLatency + pluginLatency, sampleRate) << "\n"
             << "xrun: " << (xruns >= 0 ? String (xruns) : ja ("非対応")) << "\n";

        if (device->getActiveInputChannels().isZero())
            text << "入力チャンネルが選ばれていません。\n";
    }

    for (int i = 0; i < chain.size(); ++i)
        if (chain[i].isLive())
            text << "  " << (i + 1) << ". " << chain[i].description.name << " : "
                 << chain[i].node->getProcessor()->getLatencySamples() << " samples\n";

    readout->setText (text);
}

void MainComponent::timerCallback()
{
    updateReadout();

    if (++ticksSinceSave >= ticksPerSave)
        persist();
}

void MainComponent::changeListenerCallback (ChangeBroadcaster*)
{
    chain.rewire();
    persist();
    updateReadout();
}

} // namespace mfx
