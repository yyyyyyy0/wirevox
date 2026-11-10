#pragma once

// MFX_PRODUCT_NAME is defined by CMakeLists.txt, which is the one place the name is written.
namespace mfx
{

constexpr const char* productName = MFX_PRODUCT_NAME;

// Folders the app used under its earlier names, newest first; only read, to carry the settings over.
// WIREVOX <- VOXRACK <- mic-fx-host.
inline constexpr const char* legacyProductNames[] = { "VOXRACK", "mic-fx-host" };

} // namespace mfx
