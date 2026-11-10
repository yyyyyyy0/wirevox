#pragma once

// MFX_PRODUCT_NAME is defined by CMakeLists.txt, which is the one place the name is written.
namespace mfx
{

constexpr const char* productName = MFX_PRODUCT_NAME;

// Folder the app used before it was renamed; only read, to carry the settings over.
constexpr const char* legacyProductName = "mic-fx-host";

} // namespace mfx
