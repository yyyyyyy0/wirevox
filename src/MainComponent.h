#pragma once

#include "Chain.h"
#include "Theme.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <map>

namespace mfx
{
using namespace juce;

// JUCE reads plain char* literals as ASCII, so UTF-8 text has to be converted explicitly.
inline String ja (const char* utf8) { return String::fromUTF8 (utf8); }

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
    void updateReadout();
    void persist();

    void timerCallback() override;
    void changeListenerCallback (ChangeBroadcaster*) override;

    ui::ThemeRegistry& themes;
    String themeId;

    AudioPluginFormatManager formatManager;
    Chain chain;
    AudioDeviceManager deviceManager;
    AudioProcessorPlayer player;

    AudioDeviceSelectorComponent deviceSelector;
    Viewport deviceViewport;
    TextButton addButton { ja ("プラグインを追加...") };
    Label dropHint { {}, ja (".vst3 をこのウィンドウにドロップしても追加できます") };
    std::unique_ptr<ChainListModel> listModel;
    ListBox chainList;
    ui::ReadoutPanel lcd;
    ui::StatusStrip status;
    ComboBox themeSelector;
    std::unique_ptr<Overlay> overlay;

    Rectangle<int> logoStrip, deviceTitle, deviceFrame, chainTitle, chainFrame;

    // Narrow windows put a row's buttons on a second line instead of squeezing the plugin name.
    bool stackedRows = false;
    int lastLcdHeight = 0;

    std::unique_ptr<FileChooser> chooser;
    std::map<AudioProcessor*, std::unique_ptr<EditorWindow>> editors;
    int ticksSinceSave = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace mfx
