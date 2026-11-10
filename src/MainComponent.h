#pragma once

#include "BreakdownPanel.h"
#include "Chain.h"
#include "Engine.h"
#include "LatencyTuner.h"
#include "Measure.h"
#include "Text.h"
#include "Theme.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <map>

namespace mfx
{
using namespace juce;

class MainComponent final : public Component,
                            public FileDragAndDropTarget,
                            private Timer,
                            private ChangeListener
{
public:
    explicit MainComponent (ui::ThemeRegistry&);
    ~MainComponent() override;

    void resized() override;
    void paint (Graphics&) override;
    void lookAndFeelChanged() override;

    bool isInterestedInFileDrag (const StringArray& files) override;
    void filesDropped (const StringArray& files, int x, int y) override;

private:
    class ChainRow;
    class ChainListModel;
    class EditorWindow;
    class Overlay;

    void chooseFiles();
    void addPluginFiles (const Array<File>&);
    void addDescription (const PluginDescription&);

    void openEditor (int index);
    void closeEditor (AudioProcessor*);
    void closeAllEditors();
    void removeSlot (int index);
    void moveSlot (int index, int delta);
    void setSlotBypassed (int index, bool);
    void chainEdited();

    void setTheme (const String& id);
    void showStatus (const String&);
    void refreshStatus();
    void updateReadout();
    std::optional<Breakdown> updateBreakdown();
    void startTuning();
    void tuningEnded();
    void askToMeasure();
    void startMeasuring();
    void measuringEnded();
    bool isBusy() const noexcept        { return tuner.isRunning() || measureSession != nullptr; }
    void updateBusyState();
    void persist();
    void applyMonitor (bool deviceChanged = false);
    void refreshMonitorControls();
    void serviceMonitor();
    void fillMonitorPairs (const String& deviceName, int firstChannel);

    void timerCallback() override;
    void changeListenerCallback (ChangeBroadcaster*) override;

    ui::ThemeRegistry& themes;
    String themeId;

    Engine engine;
    Chain& chain = engine.chain;
    AudioDeviceManager& deviceManager = engine.deviceManager;
    AudioPluginFormatManager& formatManager = engine.formats;
    LatencyTuner tuner { engine };
    std::unique_ptr<measure::Session> measureSession;

    AudioDeviceSelectorComponent deviceSelector;
    Viewport deviceViewport;
    ToggleButton monitorToggle { ja ("モニター出力") };
    ComboBox monitorDevice, monitorPair;
    TextButton addButton { ja ("プラグインを追加...") };
    TextButton minLatencyButton { "MIN LATENCY" };
    Label dropHint { {}, ja (".vst3 をこのウィンドウにドロップしても追加できます") };
    std::unique_ptr<ChainListModel> listModel;
    ListBox chainList;
    ui::BreakdownPanel breakdown;
    ui::ReadoutPanel lcd;
    ui::StatusStrip status;
    ComboBox themeSelector;
    std::unique_ptr<Overlay> overlay;
    TooltipWindow tooltips { this };

    Rectangle<int> logoStrip, deviceTitle, deviceFrame, chainTitle, chainFrame, breakdownTitle, breakdownFrame;

    // Narrow windows put a row's buttons on a second line instead of squeezing the plugin name.
    bool stackedRows = false;
    int lastLcdHeight = 0, lastBreakdownHeight = 0;
    uint32 seenStartCount = 0;

    std::unique_ptr<FileChooser> chooser;
    std::map<AudioProcessor*, std::unique_ptr<EditorWindow>> editors;
    int ticksSinceSave = 0;
    String statusMessage, standingWarning;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace mfx
