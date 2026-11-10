#pragma once

#include <juce_core/juce_core.h>

namespace mfx
{

bool isCommandLineMode (const juce::StringArray& args);

// Runs --selftest / --list-devices without creating a window or opening an audio device.
int runCommandLine (const juce::StringArray& args);

} // namespace mfx
