#pragma once

#include <juce_core/juce_core.h>

namespace mfx
{

// Self tests that need no audio hardware and no plugin. Each prints one line per check and returns
// 0 when everything held.

// The timing arithmetic of a measurement on made-up HAL timestamps, and the test burst injection.
int selfTestTiming();

// The engine's persisted audio settings, MIN LATENCY and the restore of a changed device setup, run
// against a fake device layer, including a child process that is killed in the middle of a tune.
int selfTestEngine();

// The child process of the kill test: tunes against the fake device and waits to be killed.
int selfTestEngineChild (const juce::String& settingsPath);

} // namespace mfx
