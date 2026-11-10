#pragma once

#include <juce_core/juce_core.h>

namespace mfx
{

juce::File getSettingsFile();

std::unique_ptr<juce::XmlElement> loadSettings();
bool saveSettings (const juce::XmlElement&);

} // namespace mfx
