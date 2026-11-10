#include "Settings.h"
#include "Engine.h"
#include "Product.h"

#include <climits>
#include <cstdlib>
#include <sys/stat.h>

namespace mfx
{
using namespace juce;

namespace
{
    File settingsFileOverride;

    File home()
    {
        return File::getSpecialLocation (File::userHomeDirectory);
    }

    File settingsFileIn (const File& homeDirectory, const String& folder)
    {
        return homeDirectory.getChildFile ("Library/Application Support").getChildFile (folder).getChildFile ("settings.xml");
    }
}

bool isSameFile (const File& a, const File& b)
{
    struct stat first {}, second {};

    if (::stat (a.getFullPathName().toRawUTF8(), &first) == 0 && ::stat (b.getFullPathName().toRawUTF8(), &second) == 0)
        return first.st_dev == second.st_dev && first.st_ino == second.st_ino;

    // At least one of them is not there yet; resolve the folders (which must exist) and compare the names.
    const auto resolved = [] (const File& file) -> File
    {
        char buffer[PATH_MAX];

        if (::realpath (file.getParentDirectory().getFullPathName().toRawUTF8(), buffer) == nullptr)
            return file;

        return File (String::fromUTF8 (buffer)).getChildFile (file.getFileName());
    };

    return resolved (a) == resolved (b);
}

File defaultSettingsFile (const File& homeDirectory)
{
    return settingsFileIn (homeDirectory, productName);
}

Array<File> legacySettingsFiles (const File& homeDirectory)
{
    Array<File> files;

    for (auto* name : legacyProductNames)
        files.add (settingsFileIn (homeDirectory, name));

    return files;
}

Migration migrateLegacySettings (const File& homeDirectory)
{
    const auto target = defaultSettingsFile (homeDirectory);

    if (target.exists())
        return Migration::notNeeded;

    File legacy;

    for (auto& candidate : legacySettingsFiles (homeDirectory))
    {
        if (candidate.existsAsFile())
        {
            legacy = candidate;
            break;
        }
    }

    if (legacy == File())
        return Migration::notNeeded;

    if (! target.getParentDirectory().createDirectory())
        return Migration::failed;

    // A half-copied file would be taken for the real settings on the next launch.
    TemporaryFile temp (target);

    if (! legacy.copyFileTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
        return Migration::failed;

    return Migration::copied;
}

void useSettingsFile (const File& file)
{
    settingsFileOverride = file;
}

Migration migrateLegacySettingsIfNeeded()
{
    return settingsFileOverride == File() ? migrateLegacySettings (home()) : Migration::notNeeded;
}

File getSettingsFile()
{
    return settingsFileOverride != File() ? settingsFileOverride : defaultSettingsFile (home());
}

String readThemeId (const XmlElement& root)
{
    return root.getStringAttribute ("theme");
}

void writeThemeId (XmlElement& root, const String& id)
{
    root.setAttribute ("theme", id);
}

std::optional<ExcessRecord> readExcess (const XmlElement& root)
{
    const auto* element = root.getChildByName ("LATENCY_EXCESS");

    if (element == nullptr)
        return std::nullopt;

    ExcessRecord record;
    record.outputDevice = element->getStringAttribute ("outputDevice");
    record.sampleRate = element->getDoubleAttribute ("sampleRate", 0.0);
    record.excessMs = element->getDoubleAttribute ("excessMs", -1.0);
    record.runs = element->getIntAttribute ("runs", 0);
    record.measuredAt = Time::fromISO8601 (element->getStringAttribute ("measuredAt"));

    if (! isUsable (record))
        return std::nullopt;

    return record;
}

void writeExcess (XmlElement& root, const ExcessRecord& record)
{
    auto* element = root.createNewChildElement ("LATENCY_EXCESS");
    element->setAttribute ("outputDevice", record.outputDevice);
    element->setAttribute ("sampleRate", record.sampleRate);
    element->setAttribute ("excessMs", record.excessMs);
    element->setAttribute ("runs", record.runs);
    element->setAttribute ("measuredAt", record.measuredAt.toISO8601 (true));
}

MonitorPlan readMonitor (const XmlElement& root)
{
    MonitorPlan plan;
    const auto* element = root.getChildByName ("MONITOR");

    if (element == nullptr)
        return plan;

    plan.device = element->getStringAttribute ("device");

    // Digits only, so a negative, fractional or unreadable channel reads as damaged rather than as 0.
    const auto channel = element->getStringAttribute ("firstChannel").trim();
    const auto readable = channel.isNotEmpty() && channel.length() <= 4 && channel.containsOnly ("0123456789");

    if (readable)
        plan.firstChannel = channel.getIntValue();

    plan.enabled = element->getBoolAttribute ("enabled", false) && plan.device.isNotEmpty() && readable;
    return plan;
}

void writeMonitor (XmlElement& root, const MonitorPlan& plan)
{
    auto* element = root.createNewChildElement ("MONITOR");
    element->setAttribute ("enabled", plan.enabled ? 1 : 0);
    element->setAttribute ("device", plan.device);
    element->setAttribute ("firstChannel", plan.firstChannel);
}

std::unique_ptr<XmlElement> buildSettingsXml (const String& themeId, const Engine& engine)
{
    auto root = std::make_unique<XmlElement> ("MIC_FX_HOST");
    writeThemeId (*root, themeId);

    auto audio = engine.getPersistedAudio();

    if (audio.device != nullptr)
        root->addChildElement (audio.device.release());

    if (audio.cycleUsage.has_value())
        root->setAttribute ("cycleUsage", (double) *audio.cycleUsage);

    if (const auto excess = engine.getExcess())
        writeExcess (*root, *excess);

    // Nothing is written for a monitor that was never chosen, so a file stays as old tools expect it.
    if (const auto monitor = engine.getMonitorPlan(); monitor.enabled || monitor.device.isNotEmpty())
        writeMonitor (*root, monitor);

    root->addChildElement (engine.chain.toXml().release());
    return root;
}

std::unique_ptr<XmlElement> loadSettings()
{
    const auto file = getSettingsFile();
    auto xml = XmlDocument::parse (file);

    // The next save would replace an unparseable file with an empty chain and lose every plugin state.
    if (xml == nullptr && file.existsAsFile())
        file.copyFileTo (file.getSiblingFile ("settings.unreadable.xml"));

    return xml;
}

bool saveSettings (const XmlElement& xml)
{
    auto file = getSettingsFile();

    if (! file.getParentDirectory().createDirectory())
        return false;

    // A crash while writing must not leave a half-written settings file behind.
    TemporaryFile temp (file);
    return xml.writeTo (temp.getFile()) && temp.overwriteTargetFileWithTemporary();
}

} // namespace mfx
