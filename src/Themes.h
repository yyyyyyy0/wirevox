#pragma once

#include "Theme.h"

// One factory per theme, defined in the theme's own file and listed in the table in ThemeRegistry.cpp.
namespace mfx::ui
{

std::unique_ptr<Theme> createRack84Theme();

} // namespace mfx::ui
