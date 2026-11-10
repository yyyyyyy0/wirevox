#pragma once

#include <juce_core/juce_core.h>

namespace mfx
{

bool isCommandLineMode (const juce::StringArray& args);

// Runs --selftest / --selftest-settings / --list-devices / --help without creating a window or
// opening an audio device.
int runCommandLine (const juce::StringArray& args);

// Applies --settings <file> when present. Returns false when the option has no value.
bool applySettingsOption (const juce::StringArray& args);

} // namespace mfx
