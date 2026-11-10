#include "Breakdown.h"
#include "Text.h"

#include <cmath>

namespace mfx
{

HostDeclared declareHost (const hal::Numbers& n, double cycleUsage)
{
    const auto usage = cycleUsage > 0.0 && cycleUsage <= 1.0 ? cycleUsage : 1.0;

    HostDeclared d;
    d.inputDevice = n.inputLatency + n.inputSafety + n.inputStreamLatency;
    d.inputBuffer = n.bufferSize;
    d.outputBuffer = (int) std::lround ((double) n.bufferSize * usage);
    d.outputSafety = n.outputSafety;
    d.outputDevice = n.outputLatency + n.outputStreamLatency;
    return d;
}

double samplesToMs (double samples, double sampleRate)
{
    return sampleRate > 0.0 ? 1000.0 * samples / sampleRate : 0.0;
}

bool excessApplies (const ExcessRecord& record, const String& outputDevice, double sampleRate)
{
    return record.outputDevice == outputDevice && std::abs (record.sampleRate - sampleRate) < 0.5;
}

bool isUsable (const ExcessRecord& record)
{
    return record.outputDevice.isNotEmpty() && std::isfinite (record.sampleRate) && record.sampleRate > 0.0 && record.sampleRate <= 1.0e6
        && std::isfinite (record.excessMs) && record.excessMs >= 0.0 && record.excessMs <= 10000.0 && record.runs > 0;
}

int findLargestRow (const std::vector<BreakdownRow>& rows)
{
    int best = -1;

    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].known && rows[i].ms > 0.0 && (best < 0 || rows[i].ms > rows[(size_t) best].ms))
            best = (int) i;

    return best;
}

Breakdown computeBreakdown (const BreakdownInput& in)
{
    Breakdown result;
    const auto host = declareHost (in.aggregate, in.cycleUsage);

    const auto add = [&] (RowKind kind, const String& label, int samples)
    {
        BreakdownRow row;
        row.kind = kind;
        row.label = label;
        row.samples = samples;
        row.ms = samplesToMs (samples, in.sampleRate);
        result.rows.push_back (std::move (row));
    };

    add (RowKind::inputDevice, ja ("入力機器の申告値"), host.inputDevice);
    add (RowKind::inputBuffer, ja ("入力バッファ"), host.inputBuffer);

    for (const auto& plugin : in.plugins)
    {
        add (RowKind::plugin, ja ("プラグイン ") + String (plugin.slot + 1) + ": " + plugin.name + (plugin.bypassed ? ja ("（バイパス中）") : String()),
             plugin.bypassed ? 0 : plugin.samples);
        result.rows.back().slot = plugin.slot;
        result.rows.back().bypassed = plugin.bypassed;
    }

    add (RowKind::outputBuffer, ja ("出力バッファ + safety"), host.outputBuffer + host.outputSafety);
    add (RowKind::outputDevice, ja ("出力デバイスの申告値"), host.outputDevice);

    BreakdownRow excess;
    excess.kind = RowKind::outputExcess;
    excess.label = ja ("出力デバイスの実測超過分");
    excess.measured = true;
    excess.known = in.excessMs.has_value();

    if (excess.known)
    {
        excess.ms = *in.excessMs;
        excess.samples = (int) std::lround (*in.excessMs * in.sampleRate / 1000.0);
    }

    result.rows.push_back (std::move (excess));

    for (const auto& row : result.rows)
    {
        if (row.measured)
            continue;

        result.declaredSamples += row.samples;
        result.declaredMs += row.ms;
    }

    result.totalMs = result.declaredMs + (in.excessMs.has_value() ? *in.excessMs : 0.0);
    result.largest = findLargestRow (result.rows);

    if (result.largest >= 0)
        result.rows[(size_t) result.largest].isLargest = true;

    return result;
}

SideSums sumSides (const Breakdown& breakdown)
{
    SideSums sums;

    for (const auto& row : breakdown.rows)
    {
        switch (row.kind)
        {
            case RowKind::inputDevice:
            case RowKind::inputBuffer:   sums.input += row.samples; break;
            case RowKind::plugin:        sums.plugins += row.samples; break;
            case RowKind::outputBuffer:
            case RowKind::outputDevice:  sums.output += row.samples; break;
            case RowKind::outputExcess:  break;
        }
    }

    return sums;
}

} // namespace mfx
