#include "CommandLine.h"
#include "MainComponent.h"
#include "Product.h"
#include "Theme.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace
{
using namespace juce;

class MainWindow final : public DocumentWindow
{
public:
    explicit MainWindow (mfx::ui::ThemeRegistry& themes)
        : DocumentWindow (JUCEApplication::getInstance()->getApplicationName(),
                          Desktop::getInstance().getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                          DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new mfx::MainComponent (themes), true);
        setResizable (true, true);
        setResizeLimits (760, 520, 2400, 1600);
        centreWithSize (1000, 720);
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class MicFxHostApplication final : public JUCEApplication
{
public:
    const String getApplicationName() override       { return mfx::productName; }
    const String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    // An instance on its own --settings file is a separate rack (used for visual checks while the
    // real one is live); two on the default settings would open the same devices twice.
    bool moreThanOneInstanceAllowed() override       { return getCommandLineParameterArray().contains ("--settings"); }

    void initialise (const String&) override
    {
        Desktop::getInstance().setDefaultLookAndFeel (&themes.get (mfx::ui::ThemeRegistry::getDefaultId()));
        window = std::make_unique<MainWindow> (themes);
    }

    void shutdown() override
    {
        window.reset();
        Desktop::getInstance().setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override              { quit(); }

private:
    mfx::ui::ThemeRegistry themes;
    std::unique_ptr<MainWindow> window;
};

JUCEApplicationBase* createApplication()
{
    return new MicFxHostApplication();
}
}

int main (int argc, char* argv[])
{
    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    if (mfx::isCommandLineMode (args))
        return mfx::runCommandLine (args);

    if (! mfx::applySettingsOption (args))
    {
        mfx::runCommandLine ({ "--help" });
        return 64;
    }

    juce::JUCEApplicationBase::createInstance = &createApplication;
    return juce::JUCEApplicationBase::main (argc, (const char**) argv);
}
