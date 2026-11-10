#include "Settings.h"

namespace mfx
{
using namespace juce;

File getSettingsFile()
{
    return File::getSpecialLocation (File::userHomeDirectory)
               .getChildFile ("Library/Application Support/mic-fx-host/settings.xml");
}

std::unique_ptr<XmlElement> loadSettings()
{
    return XmlDocument::parse (getSettingsFile());
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
