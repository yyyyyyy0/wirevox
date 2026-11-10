#pragma once

#include "Theme.h"

// One factory per theme, defined in the theme's own file and listed in the table in ThemeRegistry.cpp.
namespace mfx::ui
{

std::unique_ptr<Theme> createRack84Theme();
std::unique_ptr<Theme> createTerminalTheme();
std::unique_ptr<Theme> createDesktop95Theme();
std::unique_ptr<Theme> createNeonTheme();

} // namespace mfx::ui
