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
