#include "CommandLine.h"
#include "MainComponent.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace
{
using namespace juce;

class MainWindow final : public DocumentWindow
{
public:
    MainWindow()
        : DocumentWindow (JUCEApplication::getInstance()->getApplicationName(),
                          Desktop::getInstance().getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                          DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new mfx::MainComponent(), true);
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
    const String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override       { return false; }

    void initialise (const String&) override         { window = std::make_unique<MainWindow>(); }
    void shutdown() override                         { window.reset(); }
    void systemRequestedQuit() override              { quit(); }

private:
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

    juce::JUCEApplicationBase::createInstance = &createApplication;
    return juce::JUCEApplicationBase::main (argc, (const char**) argv);
}
