#include "Settings.h"
#include "Product.h"

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

File defaultSettingsFile (const File& homeDirectory)
{
    return settingsFileIn (homeDirectory, productName);
}

File legacySettingsFile (const File& homeDirectory)
{
    return settingsFileIn (homeDirectory, legacyProductName);
}

Migration migrateLegacySettings (const File& homeDirectory)
{
    const auto target = defaultSettingsFile (homeDirectory);
    const auto legacy = legacySettingsFile (homeDirectory);

    if (target.exists() || ! legacy.existsAsFile())
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

void migrateLegacySettingsIfNeeded()
{
    if (settingsFileOverride == File())
        migrateLegacySettings (home());
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
