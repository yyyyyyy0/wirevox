#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace mfx::measure
{

// Test burst for the latency measurement: a 50 ms linear sweep from 300 Hz to 0.4 x the sample rate
// at -20 dBFS, with 2 ms raised-cosine ends. A sweep keeps all its energy inside the band that the
// converters and sample-rate conversion in the path pass, so the correlation peak stays narrow
// (about 1 / bandwidth, 30 us) and sits 30 dB above the noise from the time-bandwidth product. An
// MLS of the same length would need its upper band-limited, which smears the peak the same way.
std::vector<float> makeTestSignal (double sampleRate);

// The same burst evaluated at any real time since its start (seconds); zero outside it. The tests use
// it to build a signal with an exact fractional delay.
double testSignalAt (double sampleRate, double seconds);

struct DelayEstimate
{
    bool valid = false;
    double delaySamples = 0.0;      // position of the reference's first sample inside the recording
    double coefficient = 0.0;       // normalised correlation at the peak, 1 = identical shape
    double sidelobeDb = 0.0;        // peak against the largest correlation more than 32 samples away
};

// Where `reference` sits inside `recording`, to a fraction of a sample. Only positions in
// [searchFrom, searchTo] are considered (searchTo < 0 means up to the end).
DelayEstimate estimateDelay (const float* reference, int numReference,
                             const float* recording, int numRecording,
                             int searchFrom = 0, int searchTo = -1);

// A run is trustworthy when the shape matches and the peak is unambiguous.
bool isTrustworthy (const DelayEstimate&);

struct Summary
{
    int count = 0;
    double median = 0.0, minimum = 0.0, maximum = 0.0, mean = 0.0, stddev = 0.0;
};

Summary summarise (std::vector<double> values);

} // namespace mfx::measure
