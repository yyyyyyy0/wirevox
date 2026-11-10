#pragma once

#include <juce_core/juce_core.h>

namespace mfx
{

bool isMeasureMode (const juce::StringArray& args);

// --measure and --measure-sweep: open the devices and chain from the settings file in use, measure,
// print and write a report next to the settings, and put the device setup back as it was.
int runMeasureCommand (const juce::StringArray& args);

} // namespace mfx
