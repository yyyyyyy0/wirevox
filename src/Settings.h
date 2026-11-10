#pragma once

#include "Breakdown.h"
#include "Modes.h"
#include "Monitor.h"

#include <juce_core/juce_core.h>
#include <optional>

namespace mfx
{

class Engine;

juce::File defaultSettingsFile (const juce::File& home);

// The settings files of the earlier product names, newest first (VOXRACK, then mic-fx-host).
juce::Array<juce::File> legacySettingsFiles (const juce::File& home);

enum class Migration { notNeeded, copied, failed };

// Copies the newest existing pre-rename settings file to the new location when the new one is missing.
// The old files are left in place. `home` is a parameter so tests can use a temporary directory.
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

// How MIN LATENCY was last run (`tuneMode`: "headroom" or "fastest") and which view the window shows
// (`view`: "simple" or "detail"), both attributes of the root. A missing or unknown tuneMode reads as
// headroom. A missing or unknown view reads as detail: a settings file from before the attribute existed
// belongs to someone who has only seen the detailed window. A first run, with no file at all, starts
// in the simple view (Preferences{}).
TuneMode readTuneMode (const juce::XmlElement& root);
ViewMode readViewMode (const juce::XmlElement& root);
void writePreferences (juce::XmlElement& root, const Preferences&);

// The measured excess of the output device is a LATENCY_EXCESS child of the root with the device name
// and rate it was taken at and when. Settings files from before it existed have none, which reads as
// not measured; a damaged one reads the same.
std::optional<ExcessRecord> readExcess (const juce::XmlElement& root);
void writeExcess (juce::XmlElement& root, const ExcessRecord&);

// The monitor output is a MONITOR child of the root: enabled, the device's name and the zero-based first
// channel of the pair. Settings files from before it existed have none, which reads as off; so does any
// value that cannot be right (an empty device name, a negative or unreadable channel). Whether the device
// exists and has the channels is only known when it is opened, and is checked there.
MonitorPlan readMonitor (const juce::XmlElement& root);
void writeMonitor (juce::XmlElement& root, const MonitorPlan&);

// The whole document the app writes: theme, the preferences, the audio settings the engine says are persistable
// (see Engine::getPersistedAudio), the measured excess that still applies to the open device, the
// monitor choice and the chain.
std::unique_ptr<juce::XmlElement> buildSettingsXml (const juce::String& themeId, const Engine&, const Preferences& = {});

std::unique_ptr<juce::XmlElement> loadSettings();
bool saveSettings (const juce::XmlElement&);

} // namespace mfx
