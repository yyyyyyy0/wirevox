#include "MainComponent.h"
#include "Product.h"
#include "Settings.h"

namespace mfx
{

namespace
{
    constexpr int minNameWidth = 150;
    constexpr int minChainListHeight = 130;
    constexpr int minBreakdownHeight = 110;
    constexpr int measureRuns = 6;
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

        const auto asIcon = [] (Button& button, ui::KeyIcon icon, const String& tooltip)
        {
            ui::setKeyIcon (button, icon);
            button.setTooltip (tooltip);
        };

        asIcon (editor, ui::KeyIcon::edit, ja ("プラグインの画面を開く"));
        asIcon (up, ui::KeyIcon::up, ja ("上へ移動"));
        asIcon (down, ui::KeyIcon::down, ja ("下へ移動"));
        asIcon (remove, ui::KeyIcon::remove, ja ("削除"));
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
      deviceSelector (deviceManager, 0, 256, 0, 256, false, false, false, false)
{
    const auto migration = migrateLegacySettingsIfNeeded();
    const auto settings = loadSettings();
    StringArray restoreProblems;

    themeId = ui::ThemeRegistry::sanitise (settings != nullptr ? readThemeId (*settings) : String());
    Desktop::getInstance().setDefaultLookAndFeel (&themes.get (themeId));

    // Without saved settings, initialise() would open the system default devices. An empty setup
    // keeps every device closed until the user picks one: a mic wired straight to the speakers
    // on first launch would feed back.
    engine.openFromSettings (settings.get());
    const auto monitorProblem = engine.takeMonitorProblem();

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

    engine.start();
    deviceManager.addChangeListener (this);

    deviceViewport.setViewedComponent (&deviceSelector, false);
    deviceViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (deviceViewport);

    monitorToggle.setTooltip (ja ("処理後の音を、メイン出力とは別のデバイスにも出します。通話の相手に届く音は変わりません"));
    monitorToggle.onClick = [this] { applyMonitor(); };
    monitorDevice.setTextWhenNothingSelected (ja ("モニター機器"));
    monitorDevice.setTooltip (ja ("モニターの出力先のデバイス"));
    monitorDevice.onChange = [this] { applyMonitor (true); };
    monitorPair.setTextWhenNothingSelected (ja ("ch"));
    monitorPair.setTooltip (ja ("モニターに使うチャンネルの組（左右）"));
    monitorPair.onChange = [this] { applyMonitor(); };
    addAndMakeVisible (monitorToggle);
    addAndMakeVisible (monitorDevice);
    addAndMakeVisible (monitorPair);
    refreshMonitorControls();

    listModel = std::make_unique<ChainListModel> (*this);
    chainList.setModel (listModel.get());
    addAndMakeVisible (chainList);

    breakdown.onMeasure = [this] { askToMeasure(); };
    addAndMakeVisible (breakdown);

    addButton.onClick = [this] { chooseFiles(); };
    addAndMakeVisible (addButton);

    minLatencyButton.onClick = [this] { startTuning(); };
    minLatencyButton.setTooltip (ja ("バッファサイズとIOサイクル使用率を下げながら、音切れ（オーバーロード）が出ない最小の遅延を探します。数十秒かかり、その間は音が途切れます"));
    addAndMakeVisible (minLatencyButton);
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
    {
        File source;

        for (auto& candidate : legacySettingsFiles (File::getSpecialLocation (File::userHomeDirectory)))
        {
            if (candidate.existsAsFile())
            {
                source = candidate;
                break;
            }
        }

        showStatus (ja ("以前の設定を引き継げませんでした: ") + source.getFullPathName());
    }
    else if (skippedChainAfterCrash)
        showStatus (ja ("前回は読み込み中に終了したため、チェーンを読み込んでいません（元の設定: settings.crashed.xml）"));
    else if (! restoreProblems.isEmpty())
        showStatus (ja ("復元できなかったプラグイン: ") + restoreProblems.joinIntoString (" / "));
    else if (monitorProblem.isNotEmpty())
        showStatus (monitorProblem);
    else if (engine.getMonitorPlan().enabled && deviceManager.getCurrentAudioDevice() == nullptr)
        showStatus (ja ("モニターは待機中です（メイン出力のデバイスが開いていません）"));

    setSize (1000, 720);
    lookAndFeelChanged();
    updateReadout();
    startTimer (timerMs);
}

MainComponent::~MainComponent()
{
    stopTimer();

    // A run that is cut short puts the original buffer and cycle usage back before anything is written.
    tuner.cancel();
    measureSession.reset();
    persist();
    closeAllEditors();
    deviceManager.removeChangeListener (this);
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

    if (! breakdownFrame.isEmpty())
        theme->paintPanel (g, breakdownTitle, breakdownFrame, "LATENCY BREAKDOWN", ui::PanelKind::list);
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

    // The monitor controls sit under the device list: the toggle, then the device and the channel pair.
    auto deviceArea = left.reduced (4);
    auto monitorArea = deviceArea.removeFromBottom (2 * m.controlHeight + 12);
    monitorArea.removeFromTop (8);
    monitorToggle.setBounds (monitorArea.removeFromTop (m.controlHeight));
    monitorArea.removeFromTop (4);
    auto monitorRow = monitorArea.removeFromTop (m.controlHeight);
    monitorPair.setBounds (monitorRow.removeFromRight (jmin (96, monitorRow.getWidth() / 3)));
    monitorRow.removeFromRight (6);
    monitorDevice.setBounds (monitorRow);
    deviceViewport.setBounds (deviceArea);
    deviceSelector.setSize (deviceViewport.getMaximumVisibleWidth(), deviceSelector.getHeight());

    area.removeFromLeft (14);

    chainTitle = area.removeFromTop (m.panelTitleHeight);
    area.removeFromTop (4);

    auto top = area.removeFromTop (28);
    addButton.setBounds (top.removeFromLeft (170));
    top.removeFromLeft (8);
    minLatencyButton.setBounds (top.removeFromLeft (132));
    dropHint.setBounds (top.withTrimmedLeft (10));
    area.removeFromTop (8);

    // The breakdown sits under the chain. It takes the height its rows need, but the chain keeps room
    // for a few rows; when there is less than that the breakdown scrolls.
    const auto breakdownWanted = m.panelTitleHeight + 4 + breakdown.getPreferredHeight (area.getWidth()) + 8;
    const auto breakdownRoom = jmax (m.panelTitleHeight + 4 + minBreakdownHeight, area.getHeight() - minChainListHeight - 10);
    const auto breakdownHeight = jmin (breakdownWanted, breakdownRoom);

    auto breakdownArea = area.removeFromBottom (breakdownHeight);
    area.removeFromBottom (10);
    breakdownTitle = breakdownArea.removeFromTop (m.panelTitleHeight);
    breakdownArea.removeFromTop (4);
    breakdownFrame = breakdownArea;
    breakdown.setBounds (breakdownArea.reduced (4));

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
    lastBreakdownHeight = 0;
    breakdown.lookAndFeelChanged();

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
    if (isBusy())
        return false;

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
    // A file chooser or menu that was already open can still deliver a plugin while MIN LATENCY or a measurement runs.
    if (isBusy())
    {
        showStatus (tuner.isRunning() ? ja ("MIN LATENCY の実行中はプラグインを追加できません") : ja ("計測の実行中はプラグインを追加できません"));
        return;
    }

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
    statusMessage = text;
    refreshStatus();
}

void MainComponent::refreshStatus()
{
    const auto progress = tuner.getProgress().isNotEmpty() ? tuner.getProgress()
                        : measureSession != nullptr ? measureSession->getProgress() : String();
    status.setText (progress.isNotEmpty() ? progress : statusMessage.isNotEmpty() ? statusMessage : standingWarning);
}

void MainComponent::startTuning()
{
    if (tuner.isRunning())
    {
        tuner.cancel();
        tuningEnded();
        return;
    }

    if (measureSession != nullptr)
        return;

    showStatus ({});
    tuner.start();

    if (tuner.isRunning())
        updateBusyState();
    else
        tuningEnded();
}

// MIN LATENCY and the measurement of the output device each change the device or the signal path
// underneath the window, so what they find is only valid for the setup they started with: the device
// selector and everything that edits the chain are off until the run ends, and the two never overlap.
void MainComponent::updateBusyState()
{
    const auto tuning = tuner.isRunning();
    const auto measuring = measureSession != nullptr;
    const auto busy = tuning || measuring;

    minLatencyButton.setButtonText (tuning ? ja ("中止") : "MIN LATENCY");
    minLatencyButton.setEnabled (! measuring);
    deviceSelector.setEnabled (! busy);
    monitorToggle.setEnabled (! busy);
    monitorDevice.setEnabled (! busy);
    monitorPair.setEnabled (! busy);
    addButton.setEnabled (! busy);
    chainList.setEnabled (! busy);
    breakdown.setMeasuring (measuring, ! tuning && deviceManager.getCurrentAudioDevice() != nullptr);
    refreshStatus();
}

void MainComponent::tuningEnded()
{
    updateBusyState();
    showStatus (tuner.getResult());
    serviceMonitor();
    persist();
}

//==============================================================================
void MainComponent::askToMeasure()
{
    if (measureSession != nullptr)
    {
        measureSession->cancel();
        measuringEnded();
        return;
    }

    if (isBusy() || deviceManager.getCurrentAudioDevice() == nullptr)
        return;

    // The test tone goes to the output device, which is what a call hears as the microphone.
    const auto options = MessageBoxOptions()
        .withIconType (MessageBoxIconType::WarningIcon)
        .withTitle (ja ("仮想デバイスを計測します"))
        .withMessage (ja ("計測の間（数秒）、出力デバイス「") + deviceManager.getAudioDeviceSetup().outputDeviceName
                      + ja ("」に試験音が流れ、マイクの音は止まります。通話中は、通話の相手に試験音が聞こえます。")
                      + (engine.getMonitorPlan().enabled ? ja ("モニター出力は、計測の間だけ止まります。") : String())
                      + ja ("\n\n計測を始めますか？"))
        .withButton (ja ("計測する"), 1)
        .withButton (ja ("やめる"), 0)
        .withAssociatedComponent (this);

    AlertWindow::showAsync (options, [safe = SafePointer<MainComponent> (this)] (int result)
    {
        if (safe != nullptr && result == 1)
            safe->startMeasuring();
    });
}

void MainComponent::startMeasuring()
{
    // The dialog can outlive a MIN LATENCY that was started behind it, or a device that was unplugged.
    if (isBusy() || deviceManager.getCurrentAudioDevice() == nullptr)
        return;

    measure::Options options;
    options.runs = measureRuns;

    showStatus ({});
    measureSession = std::make_unique<measure::Session> (engine, options);

    if (! measureSession->start())
    {
        const auto problem = measureSession->getResult().problemJa;
        measureSession.reset();
        updateBusyState();
        showStatus (ja ("仮想デバイスを計測できませんでした: ") + problem);
        return;
    }

    updateBusyState();
}

void MainComponent::measuringEnded()
{
    String message;

    if (measureSession != nullptr)
    {
        const auto& result = measureSession->getResult();
        const auto record = measure::makeExcessRecord (result, deviceManager.getAudioDeviceSetup().outputDeviceName, Time::getCurrentTime(), 3);

        if (result.problem.isNotEmpty())
        {
            message = ja ("仮想デバイスの計測: ") + result.problemJa + ja ("。元の状態に戻しました");
        }
        else if (record.has_value())
        {
            engine.setExcess (*record);
            message = ja ("仮想デバイスが足している遅延: ") + String (record->excessMs, 2) + ja (" ms（") + String (record->runs) + ja (" 回の中央値）");
        }
        else
        {
            message = ja ("試験音を安定して検出できなかったため、結果を採用しませんでした（信頼できた回数 ")
                    + String (result.trustworthyRuns) + "/" + String ((int) result.runs.size()) + ja ("）");
        }
    }

    measureSession.reset();
    updateBusyState();
    showStatus (message);
    serviceMonitor();
    persist();
    updateReadout();
}

//==============================================================================
// The device and the pair only say what the user wants; the engine decides whether it can be done, and
// what it reports back (it can leave the monitor off with a reason) is what the controls then show.
void MainComponent::applyMonitor (bool deviceChanged)
{
    if (isBusy())
        return;

    auto plan = engine.getMonitorPlan();
    plan.enabled = monitorToggle.getToggleState();

    if (monitorDevice.getSelectedId() > 0)
    {
        const auto chosen = monitorDevice.getText();

        if (deviceChanged && chosen != plan.device)
            fillMonitorPairs (chosen, plan.firstChannel);

        plan.device = chosen;
    }

    if (monitorPair.getSelectedId() > 0)
        plan.firstChannel = (monitorPair.getSelectedId() - 1) * 2;

    String message;

    if (plan.enabled && (monitorDevice.getSelectedId() <= 0 || monitorPair.getSelectedId() <= 0))
    {
        plan.enabled = false;
        message = ja ("モニター機器とチャンネルを選んでください");
    }

    engine.setMonitorPlan (plan);
    chain.rewire();

    if (const auto problem = engine.takeMonitorProblem(); problem.isNotEmpty())
        message = problem;

    refreshMonitorControls();
    showStatus (message);
    persist();
    updateReadout();
}

void MainComponent::serviceMonitor()
{
    if (! engine.serviceMonitor())
        return;

    chain.rewire();
    refreshMonitorControls();

    if (const auto problem = engine.takeMonitorProblem(); problem.isNotEmpty())
        showStatus (problem);

    persist();
}

void MainComponent::refreshMonitorControls()
{
    const auto plan = engine.getMonitorPlan();
    const auto names = engine.getMonitorCandidates();

    monitorDevice.clear (dontSendNotification);
    monitorDevice.addItemList (names, 1);

    if (const auto index = names.indexOf (plan.device); index >= 0)
        monitorDevice.setSelectedId (index + 1, dontSendNotification);

    fillMonitorPairs (plan.device, plan.firstChannel);
    monitorToggle.setToggleState (plan.enabled, dontSendNotification);
}

// Pairs start on odd channel numbers (1/2, 3/4, ... 31/32); one that does not line up is left unselected.
void MainComponent::fillMonitorPairs (const String& deviceName, int firstChannel)
{
    const auto pairs = deviceName.isNotEmpty() ? engine.countMonitorChannels (deviceName) / 2 : 0;

    monitorPair.clear (dontSendNotification);

    for (int i = 0; i < pairs; ++i)
        monitorPair.addItem (String (i * 2 + 1) + " / " + String (i * 2 + 2), i + 1);

    if (firstChannel >= 0 && firstChannel % 2 == 0 && firstChannel / 2 < pairs)
        monitorPair.setSelectedId (firstChannel / 2 + 1, dontSendNotification);
    else if (pairs > 0)
        monitorPair.setSelectedId (1, dontSendNotification);
}

void MainComponent::persist()
{
    if (! saveSettings (*buildSettingsXml (themeId, engine)))
        showStatus (ja ("設定ファイルを保存できませんでした: ") + getSettingsFile().getFullPathName());

    ticksSinceSave = 0;
}

void MainComponent::updateReadout()
{
    engine.refreshHal();

    // The readout prints the panel's own numbers, so the two totals cannot disagree.
    const auto latency = updateBreakdown();

    String text, bigValue = "--.--";
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr || ! latency.has_value())
    {
        standingWarning.clear();
        text << ja ("オーディオデバイスが開いていません。\n左のパネルで入力・出力デバイスを選んでください。\n");
    }
    else
    {
        const auto sampleRate = device->getCurrentSampleRate();
        const auto sides = sumSides (*latency);
        const auto excess = engine.getExcess();
        const auto load = engine.callback.getLoad();
        const auto rates = engine.getDeviceRates();
        const auto setup = deviceManager.getAudioDeviceSetup();

        if (sampleRate > 0)
            bigValue = String (latency->totalMs, 2);

        text << ja ("入力デバイス: ") << setup.inputDeviceName << "  ("
             << device->getActiveInputChannels().countNumberOfSetBits() << " ch)\n"
             << ja ("出力デバイス: ") << setup.outputDeviceName << "  ("
             << device->getActiveOutputChannels().countNumberOfSetBits() << " ch)\n"
             << ja ("サンプルレート: ") << String (sampleRate, 0) << " Hz\n"
             << ja ("バッファサイズ: ") << samplesAndMs (device->getCurrentBufferSizeSamples(), sampleRate) << "\n"
             << ja ("入力デバイス遅延（バッファ込み）: ") << samplesAndMs (sides.input, sampleRate) << "\n"
             << ja ("出力デバイス遅延（バッファ込み）: ") << samplesAndMs (sides.output, sampleRate) << "\n"
             << ja ("プラグイン遅延合計: ") << samplesAndMs (sides.plugins, sampleRate) << "\n";

        if (excess.has_value())
            text << ja ("出力デバイスの実測超過分: ") << String (excess->excessMs, 2) << " ms\n"
                 << ja ("合計: ") << String (latency->totalMs, 2) << " ms\n";
        else
            text << ja ("合計（出力デバイスの超過分を除く）: ") << samplesAndMs (latency->declaredSamples, sampleRate) << "\n";

        if (engine.isMonitorActive())
            text << ja ("モニター経路（申告値。デバイス間のドリフト補正は含みません）: ")
                 << samplesAndMs (declareMonitorPath (*latency, engine.getMonitorNumbers(), device->getCurrentBufferSizeSamples(), engine.getCycleUsage()), sampleRate) << "\n";

        text << ja ("負荷（コールバックの所要時間 / バッファ時間）: 平均 ") << String (load.meanPercent, 1) << " % / "
             << ja ("最大 ") << String (load.maxPercent, 1) << " %\n"
             << ja ("オーバーロード: ") << engine.getOverloads() << ja ("  遅れたコールバック: ") << (int) load.late;

        if (const auto usage = engine.getExperiments().ioCycleUsage)
            text << ja ("  IOサイクル使用率: ") << String (*usage, 2);

        text << "\n";

        standingWarning = describeRateMismatch (sampleRate, rates.input, rates.output, rates.monitor);

        if (device->getActiveInputChannels().isZero())
            text << ja ("入力チャンネルが選ばれていません。\n");
    }

    // A bypassed slot is out of the path, so it counts 0 here like it does in the total above.
    for (int i = 0; i < chain.size(); ++i)
        if (chain[i].isLive())
            text << "  " << (i + 1) << ". " << chain[i].description.name << " : "
                 << (chain[i].bypassed ? 0 : chain[i].node->getProcessor()->getLatencySamples()) << " samples"
                 << (chain[i].bypassed ? ja (" (バイパス中)") : String()) << "\n";

    lcd.setContent ({ "TOTAL LATENCY", bigValue, "ms", text.trimEnd() });
    refreshStatus();

    const auto preferred = lcd.getPreferredHeight (lcd.getWidth());

    if (preferred != lastLcdHeight)
    {
        lastLcdHeight = preferred;
        resized();
    }
}

std::optional<Breakdown> MainComponent::updateBreakdown()
{
    std::optional<Breakdown> latency;
    ui::BreakdownView view;
    auto* device = deviceManager.getCurrentAudioDevice();
    view.deviceOpen = device != nullptr;

    if (device != nullptr)
    {
        const auto rate = device->getCurrentSampleRate();
        const auto buffer = device->getCurrentBufferSizeSamples();
        view.sampleRate = rate;
        view.cycleUsage = engine.getCycleUsage();
        view.bufferMs = samplesToMs (buffer, rate);
        view.excess = engine.getExcess();

        BreakdownInput input;
        input.sampleRate = rate;
        input.aggregate = engine.getAggregateNumbers();
        input.cycleUsage = view.cycleUsage;

        if (engine.getAggregate() == kAudioObjectUnknown)
            input.aggregate.bufferSize = buffer;

        if (view.excess.has_value())
            input.excessMs = view.excess->excessMs;

        // The time a plugin takes is a share of what one buffer lasts.
        const auto blockSize = engine.callback.getBlockSize() > 0 ? engine.callback.getBlockSize() : buffer;
        const auto periodNs = rate > 0.0 ? 1.0e9 * blockSize / rate : 0.0;
        std::vector<ProcessStats> stats;

        for (int i = 0; i < chain.size(); ++i)
        {
            if (! chain[i].isLive())
                continue;

            const auto bypassed = chain[i].bypassed;
            input.plugins.push_back ({ i, chain[i].description.name, bypassed ? 0 : chain[i].node->getProcessor()->getLatencySamples(), bypassed });

            ui::SlotLoadView load;
            load.slot = i;
            load.name = chain[i].description.name;
            load.bypassed = bypassed;
            load.stats = bypassed ? ProcessStats {} : chain[i].load.stats (periodNs);
            view.loads.push_back (std::move (load));
            stats.push_back (view.loads.back().stats);
        }

        if (const auto heaviest = findHeaviest (stats); heaviest >= 0)
            view.loads[(size_t) heaviest].heaviest = true;

        view.latency = computeBreakdown (input);
        latency = view.latency;
    }

    breakdown.setView (view);
    breakdown.setMeasuring (measureSession != nullptr, ! tuner.isRunning() && view.deviceOpen);

    // The panel grows and shrinks with the chain; the layout makes room for it.
    const auto preferred = breakdown.getPreferredHeight (breakdown.getWidth());

    if (preferred != lastBreakdownHeight)
    {
        lastBreakdownHeight = preferred;
        resized();
    }

    return latency;
}

void MainComponent::timerCallback()
{
    if (tuner.isRunning() && tuner.tick())
        tuningEnded();

    if (measureSession != nullptr && measureSession->tick())
        measuringEnded();

    // A monitor that was dropped, lost or waiting while something else was busy, or that the device list says
    // can run now (looked at once per list).
    if (! isBusy())
        serviceMonitor();

    // What the audio thread timed since the last tick. A new device start (another buffer size or
    // rate) changes what a share of a buffer means, so the old readings go.
    if (const auto startCount = engine.callback.getStartCount(); startCount != seenStartCount)
    {
        seenStartCount = startCount;
        chain.resetProcessTimes();
    }
    else
    {
        chain.pollProcessTimes();
    }

    updateReadout();

    if (++ticksSinceSave >= ticksPerSave)
        persist();
}

void MainComponent::changeListenerCallback (ChangeBroadcaster*)
{
    // A device list change is when a waiting monitor is tried again, and when a reopen that JUCE did by
    // itself and that failed with the monitor in use is retried without it.
    if (! isBusy())
        engine.serviceMonitor();

    chain.rewire();
    refreshMonitorControls();

    if (const auto problem = engine.takeMonitorProblem(); problem.isNotEmpty())
        showStatus (problem);

    persist();
    updateReadout();
}

} // namespace mfx
