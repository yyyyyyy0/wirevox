#pragma once

#include <juce_core/juce_core.h>

namespace mfx
{

// JUCE reads plain char* literals as ASCII, so UTF-8 text has to be converted explicitly.
inline juce::String ja (const char* utf8) { return juce::String::fromUTF8 (utf8); }

} // namespace mfx
