#pragma once

#include "Correlate.h"
#include "Engine.h"

namespace mfx::measure
{

struct Options
{
    int runs = 10;
    double spacingMs = 500.0;
    int captureBuffer = 0;      // proof tool: request this IO buffer size on the capture device (0 leaves it alone)
    bool withChain = false;     // false takes every plugin out of the path so the burst arrives unaltered
};

struct Run
{
    bool trustworthy = false;
    double latencyMs = 0.0;     // input acquisition -> available on the output device (see Measure.cpp)
    double declaredMs = 0.0;    // what the HAL timestamps of the host's own cycle promise for the same span
    double excessMs = 0.0;      // latency - declared: what the output device adds on top
    double coefficient = 0.0, sidelobeDb = 0.0;
};

struct Result
{
    String problem;             // non-empty when nothing could be measured
    std::vector<Run> runs;
    double sampleRate = 0.0;
    int blockSize = 0;
    double wakeMs = 0.0;        // median time from a cycle's first input frame to the IOProc actually running
    int captureBufferUsed = 0;  // IO buffer of the capture device during the recording
    int chainLatencySamples = 0;
    String captureName;
    hal::Numbers aggregate, capture;
    int overloads = 0;
    int64 captureGaps = 0;
    HostCallback::Load load;
    Summary latency, declared, excess;
    int trustworthyRuns = 0;
};

// Everything the analysis of one burst reads, as plain data, so the timing arithmetic can be run on
// made-up timestamps. `tap` and `buffers` are in recording order.
struct BurstInputs
{
    Span<const hal::TimestampTap::Entry> tap;       // input and output HAL time of every cycle of the host
    Span<const hal::Capture::Buffer> buffers;       // HAL time of every buffer of the recording
    std::vector<const float*> channels;
    int numSamples = 0;
    const float* burst = nullptr;
    int burstLength = 0;
    double sampleRate = 0.0;
    double chainNs = 0.0;           // plugin latency, added to the output time the host cycle declares
    double readerFloorNs = 0.0;     // the recording side's own latency and safety offset
};

// Index of the tap entry whose output time is `outputHostNs` (what JUCE passed to the callback that
// started the burst), or -1. Cycles are at least 0.3 ms apart, so the 100 us tolerance cannot match
// a neighbour.
int findCycle (Span<const hal::TimestampTap::Entry> tap, double outputHostNs);

// Analyses the burst that was injected in the cycle whose output time was `outputHostNs`. A run that
// could not be found or analysed has coefficient 0 and is not trustworthy.
Run analyseBurst (const BurstInputs&, uint64_t outputHostNs);

// Injects the test burst into the graph input `options.runs` times, records what arrives on the
// output device's input side, and works out the delay of every burst. Message thread only; spins the
// message loop while it waits.
Result runSession (Engine&, const Options&);

// Runs the load without injecting anything, for the overload and CPU columns of the sweep.
struct LoadResult { int overloads = 0; HostCallback::Load load; };
LoadResult observeLoad (Engine&, int milliseconds);

void wait (int milliseconds);

// One row of the sweep, reduced to what the choice of the fastest setting depends on.
struct Candidate
{
    bool measured = false;
    double latencyMs = 0.0;
    int overloads = 0;
    int64 lateCallbacks = 0;
};

// Rows that could not be measured sort last.
bool isFaster (const Candidate& a, const Candidate& b);

// Index of the lowest-latency measured row that had no overload and no late callback, or -1.
int pickBest (const std::vector<Candidate>&);

} // namespace mfx::measure
