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

// What the window's latency breakdown adds up (and that it matches the numbers `--measure` reports),
// which row is the largest, and the measured excess's save and restore with a changed device or rate.
int selfTestBreakdown();

// How long each plugin's processBlock takes: the audio-to-message hand-over, the windows and the marker
// of the heaviest slot, and that the probes leave the graph (latency, bypass, routing) as it was.
int selfTestProcessTime();

// The child process of the kill test: tunes against the fake device and waits to be killed.
int selfTestEngineChild (const juce::String& settingsPath);

} // namespace mfx
