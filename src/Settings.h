#pragma once

#include <juce_core/juce_core.h>

namespace mfx
{

juce::File defaultSettingsFile (const juce::File& home);
juce::File legacySettingsFile (const juce::File& home);

enum class Migration { notNeeded, copied, failed };

// Copies the pre-rename settings file to the new location when only the old one exists.
// The old file is left in place. `home` is a parameter so tests can use a temporary directory.
Migration migrateLegacySettings (const juce::File& home);

// True when both paths lead to the same file, whatever `..` segments, symbolic links (also in the
// folders) or hard links they go through. A path that does not exist yet is compared by its folder.
bool isSameFile (const juce::File&, const juce::File&);

// --settings <file>: use this file instead of the default location, and skip migration.
void useSettingsFile (const juce::File&);

// Runs the migration against the real home directory unless --settings was given.
Migration migrateLegacySettingsIfNeeded();

juce::File getSettingsFile();

// Id of the selected theme; empty when the settings do not name one. Validation is the caller's job.
juce::String readThemeId (const juce::XmlElement& root);
void writeThemeId (juce::XmlElement& root, const juce::String& id);

std::unique_ptr<juce::XmlElement> loadSettings();
bool saveSettings (const juce::XmlElement&);

} // namespace mfx
