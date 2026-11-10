#pragma once

#include "Hal.h"

#include <juce_core/juce_core.h>
#include <optional>
#include <vector>

namespace mfx
{
using juce::String;

// What the HAL says the host's own cycle adds, in samples at the app's rate. The numbers are the
// aggregate device's (Engine::getAggregateNumbers), the same ones JUCE adds up for its input and output
// latency, and, at the default IO cycle usage, the span between the input and output timestamps that
// `--measure` calls `declared`. This is the one place that arithmetic is written.
//
// The IO cycle usage (kAudioDevicePropertyIOCycleUsage) pulls the output time forward: the output
// buffer counts `usage x buffer`, and 1.0 is the device's default (README: "IO サイクル使用率").
struct HostDeclared
{
    int inputDevice = 0;        // latency + safety offset + stream latency of the input side
    int inputBuffer = 0;
    int outputBuffer = 0;       // usage x buffer
    int outputSafety = 0;
    int outputDevice = 0;       // latency + stream latency of the output side

    int total() const noexcept  { return inputDevice + inputBuffer + outputBuffer + outputSafety + outputDevice; }
};

HostDeclared declareHost (const hal::Numbers&, double cycleUsage);

double samplesToMs (double samples, double sampleRate);

//==============================================================================
// One plugin of the chain as the breakdown needs it. A bypassed plugin is out of the path and adds 0.
struct PluginLatency
{
    int slot = 0;
    String name;
    int samples = 0;            // 0 when bypassed
    bool bypassed = false;
};

// The result of measuring what the output device adds beyond what it declares (Measure.h). It is only
// valid for the output device and the sample rate it was taken at.
struct ExcessRecord
{
    String outputDevice;
    double sampleRate = 0.0;
    double excessMs = 0.0;
    int runs = 0;               // trustworthy runs the median was taken from
    juce::Time measuredAt;
};

// True when a result taken on `outputDevice` at `sampleRate` still describes the device that is open now.
bool excessApplies (const ExcessRecord&, const String& outputDevice, double sampleRate);

// A record is valid when it names a device, a positive rate up to 1 MHz, a finite excess from 0 to
// 10 s and at least one run; anything else (a damaged settings file) is ignored.
bool isUsable (const ExcessRecord&);

struct BreakdownInput
{
    double sampleRate = 0.0;
    hal::Numbers aggregate;
    double cycleUsage = 1.0;
    std::vector<PluginLatency> plugins;
    std::optional<double> excessMs;     // the measured excess for the open device, when there is one
};

enum class RowKind { inputDevice, inputBuffer, plugin, outputBuffer, outputDevice, outputExcess };

struct BreakdownRow
{
    RowKind kind = RowKind::inputDevice;
    int slot = -1;              // plugin rows only
    String label;
    int samples = 0;
    double ms = 0.0;
    bool measured = false;      // true only for the output excess: everything else is declared
    bool known = true;          // false for the output excess until it has been measured
    bool bypassed = false;
    bool isLargest = false;
};

struct Breakdown
{
    std::vector<BreakdownRow> rows;
    int declaredSamples = 0;    // every row except the measured one
    double declaredMs = 0.0;
    double totalMs = 0.0;       // declared plus the measured excess when there is one
    int largest = -1;           // index into `rows` of the row with the most ms, -1 when all are 0
};

Breakdown computeBreakdown (const BreakdownInput&);

// The declared rows added up per side, for the readout that prints one line each for input, plugins and
// output. The three add up to `declaredSamples`.
struct SideSums
{
    int input = 0;              // input device + input buffer
    int plugins = 0;
    int output = 0;             // output buffer + safety + output device, without the measured excess
};

SideSums sumSides (const Breakdown&);

// Index of the row with the most milliseconds among the known ones (the first of equals), or -1 when
// none is above zero.
int findLargestRow (const std::vector<BreakdownRow>&);

} // namespace mfx
