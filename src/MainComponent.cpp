#include "MainComponent.h"
#include "Product.h"
#include "Settings.h"

namespace mfx
{

namespace
{
    constexpr int minNameWidth = 150;
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

    void lookAndFeelChanged() override
    {
        setBackgroundColour (findColour (ResizableWindow::backgroundColourId));
    }

private:
    MainComponent& owner;
    AudioProcessor& processor;
};

//==============================================================================
class MainComponent::ChainRow final : public Component
{
public:
    explicit ChainRow (MainComponent& ownerIn)
        : owner (ownerIn)
    {
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

        info.number = row + 1;
        info.name = slot.description.name;
        info.failureTag = slot.isLive() ? String() : ja ("[読み込み失敗]");
        info.state = ! slot.isLive() ? ui::SlotState::failed : slot.bypassed ? ui::SlotState::bypassed : ui::SlotState::active;

        bypass.setToggleState (slot.bypassed, dontSendNotification);
        editor.setEnabled (slot.isLive());
        up.setEnabled (row > 0);
        down.setEnabled (row < owner.chain.size() - 1);
        repaint();
    }

    void paint (Graphics& g) override
    {
        if (auto* theme = ui::findTheme (*this))
            theme->paintSlotRow (g, info, geometry);
    }

    void resized() override
    {
        auto* theme = ui::findTheme (*this);

        if (theme == nullptr)
            return;

        const auto& m = theme->getMetrics();
        geometry.module = getLocalBounds().reduced (4, 3);

        auto inner = geometry.module.reduced (m.rowInsetX, 7);
        auto top = owner.stackedRows ? inner.removeFromTop (m.controlHeight + 2) : inner;

        geometry.number = top.removeFromLeft (m.numberWidth).withSizeKeepingCentre (m.numberWidth, m.controlHeight - 2);
        top.removeFromLeft (8);
        geometry.indicator = top.removeFromLeft (m.indicatorWidth).withSizeKeepingCentre (m.indicatorWidth, m.controlHeight - 2);
        top.removeFromLeft (10);

        Rectangle<int> buttons;

        if (owner.stackedRows)
        {
            geometry.name = top;
            buttons = inner.removeFromBottom (m.controlHeight).removeFromRight (m.buttons.total());
        }
        else
        {
            buttons = top.removeFromRight (m.buttons.total());
            top.removeFromRight (8);
            geometry.name = top;
        }

        auto place = [&buttons, &m] (Button& button, int width)
        {
            button.setBounds (buttons.removeFromRight (width).withSizeKeepingCentre (width, m.controlHeight));
            buttons.removeFromRight (4);
        };

        place (remove, m.buttons.remove);
        place (down, m.buttons.down);
        place (up, m.buttons.up);
        place (editor, m.buttons.edit);
        buttons.removeFromRight (4);
        place (bypass, m.buttons.bypass);
    }

    void lookAndFeelChanged() override
    {
        resized();
        repaint();
    }

private:
    MainComponent& owner;
    int row = 0;

    ui::SlotRowInfo info;
    ui::SlotRowGeometry geometry;

    ui::BypassToggle bypass { ja ("バイパス") };
    TextButton editor { ja ("編集") }, up { ja ("上") }, down { ja ("下") }, remove { ja ("削除") };
};

//==============================================================================
// Paints over the children for themes that need it (CRT scanlines); never takes mouse events.
class MainComponent::Overlay final : public Component
{
public:
    Overlay() { setInterceptsMouseClicks (false, false); }

    void paint (Graphics& g) override
    {
        if (auto* theme = ui::findTheme (*this))
            theme->paintOverlay (g, getLocalBounds());
    }
};

//==============================================================================
class MainComponent::ChainListModel final : public ListBoxModel
{
public:
    explicit ChainListModel (MainComponent& ownerIn) : owner (ownerIn) {}

    int getNumRows() override { return owner.chain.size(); }

    void paintListBoxItem (int, Graphics&, int, int, bool) override {}

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
MainComponent::MainComponent (ui::ThemeRegistry& themeRegistry)
    : themes (themeRegistry),
      chain (formatManager),
      deviceSelector (deviceManager, 0, 256, 0, 256, false, false, false, false)
{
    addDefaultFormatsToManager (formatManager);

    const auto migration = migrateLegacySettingsIfNeeded();
    const auto settings = loadSettings();
    StringArray restoreProblems;

    themeId = ui::ThemeRegistry::sanitise (settings != nullptr ? readThemeId (*settings) : String());
    Desktop::getInstance().setDefaultLookAndFeel (&themes.get (themeId));

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
    addAndMakeVisible (chainList);

    addButton.onClick = [this] { chooseFiles(); };
    addAndMakeVisible (addButton);
    addAndMakeVisible (dropHint);

    addAndMakeVisible (lcd);
    addAndMakeVisible (status);

    const auto& entries = ui::ThemeRegistry::entries();

    for (size_t i = 0; i < entries.size(); ++i)
    {
        themeSelector.addItem (entries[i].displayName, (int) i + 1);

        if (themeId == entries[i].id)
            themeSelector.setSelectedId ((int) i + 1, dontSendNotification);
    }

    themeSelector.onChange = [this]
    {
        const auto& all = ui::ThemeRegistry::entries();
        const auto index = themeSelector.getSelectedId() - 1;

        if (isPositiveAndBelow (index, (int) all.size()))
            setTheme (all[(size_t) index].id);
    };
    addAndMakeVisible (themeSelector);

    overlay = std::make_unique<Overlay>();
    addChildComponent (*overlay);

    if (migration == Migration::failed)
        showStatus (ja ("以前の設定を引き継げませんでした: ") + legacySettingsFile (File::getSpecialLocation (File::userHomeDirectory)).getFullPathName());
    else if (skippedChainAfterCrash)
        showStatus (ja ("前回は読み込み中に終了したため、チェーンを読み込んでいません（元の設定: settings.crashed.xml）"));
    else if (! restoreProblems.isEmpty())
        showStatus (ja ("復元できなかったプラグイン: ") + restoreProblems.joinIntoString (" / "));

    setSize (1000, 720);
    lookAndFeelChanged();
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
    auto* theme = ui::findTheme (*this);

    if (theme == nullptr)
        return;

    theme->paintWindow (g, getLocalBounds());
    theme->paintLogo (g, logoStrip, productName, "VOICE EFFECT RACK");
    theme->paintPanel (g, deviceTitle, deviceFrame, "AUDIO I/O", ui::PanelKind::controls);
    theme->paintPanel (g, chainTitle, chainFrame, "EFFECT CHAIN", ui::PanelKind::list);
}

void MainComponent::resized()
{
    auto* theme = ui::findTheme (*this);

    if (theme == nullptr)
        return;

    const auto& m = theme->getMetrics();
    auto area = getLocalBounds().reduced (m.margin);

    logoStrip = area.removeFromTop (m.logoStripHeight);
    themeSelector.setBounds (theme->getSelectorBounds (logoStrip));
    area.removeFromTop (10);

    status.setBounds (area.removeFromBottom (m.statusHeight));
    area.removeFromBottom (8);

    // The readout grows with its text, but never takes more than 45% of the window.
    const auto lcdHeight = jmin (lcd.getPreferredHeight (area.getWidth()), area.getHeight() * 45 / 100);
    lcd.setBounds (area.removeFromBottom (lcdHeight));
    area.removeFromBottom (8);

    auto left = area.removeFromLeft (jlimit (320, 480, area.getWidth() * 36 / 100));
    deviceTitle = left.removeFromTop (m.panelTitleHeight);
    left.removeFromTop (4);
    deviceFrame = left;
    deviceViewport.setBounds (left.reduced (4));
    deviceSelector.setSize (deviceViewport.getMaximumVisibleWidth(), deviceSelector.getHeight());

    area.removeFromLeft (14);

    chainTitle = area.removeFromTop (m.panelTitleHeight);
    area.removeFromTop (4);

    auto top = area.removeFromTop (28);
    addButton.setBounds (top.removeFromLeft (170));
    dropHint.setBounds (top.withTrimmedLeft (10));
    area.removeFromTop (8);

    chainFrame = area;
    chainList.setBounds (area.reduced (4));

    const auto oneLineWidth = 2 * m.rowInsetX + 8 + m.numberWidth + 8 + m.indicatorWidth + 10
                                + m.buttons.total() + 8 + minNameWidth;
    stackedRows = chainList.getWidth() - m.scrollbarWidth < oneLineWidth;
    chainList.setRowHeight (stackedRows ? m.rowStacked : m.rowOneLine);

    if (overlay != nullptr)
        overlay->setBounds (getLocalBounds());
}

void MainComponent::lookAndFeelChanged()
{
    auto* theme = ui::findTheme (*this);

    if (theme == nullptr)
        return;

    dropHint.setFont (theme->getUiFont (11.5f, false));
    dropHint.setColour (Label::textColourId, findColour (Label::textColourId).withAlpha (0.8f));

    if (overlay != nullptr)
        overlay->setVisible (theme->hasOverlay());

    lastLcdHeight = 0;

    resized();
    repaint();
}

void MainComponent::setTheme (const String& id)
{
    // Only the look changes; the audio graph and the device are not touched.
    themeId = ui::ThemeRegistry::sanitise (id);
    Desktop::getInstance().setDefaultLookAndFeel (&themes.get (themeId));
    persist();
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
    status.setText (text);
}

void MainComponent::persist()
{
    XmlElement root ("MIC_FX_HOST");
    writeThemeId (root, themeId);

    if (auto device = deviceManager.createStateXml())
        root.addChildElement (device.release());

    root.addChildElement (chain.toXml().release());

    if (! saveSettings (root))
        showStatus (ja ("設定ファイルを保存できませんでした: ") + getSettingsFile().getFullPathName());

    ticksSinceSave = 0;
}

void MainComponent::updateReadout()
{
    String text, bigValue = "--.--";
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        text << ja ("オーディオデバイスが開いていません。\n左のパネルで入力・出力デバイスを選んでください。\n");
    }
    else
    {
        const auto sampleRate = device->getCurrentSampleRate();
        const auto inLatency = device->getInputLatencyInSamples();
        const auto outLatency = device->getOutputLatencyInSamples();
        const auto pluginLatency = chain.getGraph().getLatencySamples();
        const auto xruns = device->getXRunCount();
        const auto setup = deviceManager.getAudioDeviceSetup();
        const auto totalSamples = inLatency + outLatency + pluginLatency;

        if (sampleRate > 0)
            bigValue = String (1000.0 * totalSamples / sampleRate, 2);

        text << ja ("入力デバイス: ") << setup.inputDeviceName << "  ("
             << device->getActiveInputChannels().countNumberOfSetBits() << " ch)\n"
             << ja ("出力デバイス: ") << setup.outputDeviceName << "  ("
             << device->getActiveOutputChannels().countNumberOfSetBits() << " ch)\n"
             << ja ("サンプルレート: ") << String (sampleRate, 0) << " Hz\n"
             << ja ("バッファサイズ: ") << samplesAndMs (device->getCurrentBufferSizeSamples(), sampleRate) << "\n"
             << ja ("入力デバイス遅延（バッファ込み）: ") << samplesAndMs (inLatency, sampleRate) << "\n"
             << ja ("出力デバイス遅延（バッファ込み）: ") << samplesAndMs (outLatency, sampleRate) << "\n"
             << ja ("プラグイン遅延合計: ") << samplesAndMs (pluginLatency, sampleRate) << "\n"
             << ja ("合計: ") << samplesAndMs (totalSamples, sampleRate) << "\n"
             << "xrun: " << (xruns >= 0 ? String (xruns) : ja ("非対応")) << "\n";

        if (device->getActiveInputChannels().isZero())
            text << ja ("入力チャンネルが選ばれていません。\n");
    }

    for (int i = 0; i < chain.size(); ++i)
        if (chain[i].isLive())
            text << "  " << (i + 1) << ". " << chain[i].description.name << " : "
                 << chain[i].node->getProcessor()->getLatencySamples() << " samples\n";

    lcd.setContent ({ "TOTAL LATENCY", bigValue, "ms", text.trimEnd() });

    const auto preferred = lcd.getPreferredHeight (lcd.getWidth());

    if (preferred != lastLcdHeight)
    {
        lastLcdHeight = preferred;
        resized();
    }
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
