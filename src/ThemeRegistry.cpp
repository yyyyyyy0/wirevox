#include "Themes.h"

namespace mfx::ui
{

// To add a theme: write its file with one factory function, declare the factory in Themes.h and add a row here.
const std::vector<ThemeEntry>& ThemeRegistry::entries()
{
    static const std::vector<ThemeEntry> table {
        { "rack84",    "RACK '84",    &createRack84Theme },
    };

    return table;
}

String ThemeRegistry::getDefaultId()
{
    return entries().front().id;
}

String ThemeRegistry::sanitise (const String& id)
{
    for (auto& entry : entries())
        if (id == entry.id)
            return id;

    return getDefaultId();
}

Theme& ThemeRegistry::get (const String& id)
{
    const auto wanted = sanitise (id);

    for (auto& [name, theme] : instances)
        if (name == wanted)
            return *theme;

    for (auto& entry : entries())
        if (wanted == entry.id)
        {
            instances.emplace_back (wanted, entry.create());
            return *instances.back().second;
        }

    jassertfalse;
    return *instances.front().second;
}

} // namespace mfx::ui
