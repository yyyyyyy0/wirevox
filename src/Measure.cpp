#include "Measure.h"

#include <cmath>

namespace mfx::measure
{

void wait (int milliseconds)
{
    MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
}

LoadResult observeLoad (Engine& engine, int milliseconds)
{
    engine.refreshHal();
    engine.resetOverloads();
    engine.callback.resetLoad();
    wait (milliseconds);

    LoadResult result;
    result.overloads = engine.getOverloads();
    result.load = engine.callback.getLoad();
    return result;
}

bool isFaster (const Candidate& a, const Candidate& b)
{
    if (a.measured != b.measured)
        return a.measured;

    return a.measured && a.latencyMs < b.latencyMs;
}

int pickBest (const std::vector<Candidate>& rows)
{
    int best = -1;

    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].measured && rows[i].overloads == 0 && rows[i].lateCallbacks == 0
            && (best < 0 || rows[i].latencyMs < rows[(size_t) best].latencyMs))
            best = (int) i;

    return best;
}

namespace
{
    constexpr double searchBeforeMs = 20.0, searchAfterMs = 470.0;
    constexpr double matchToleranceNs = 100000.0;

    // Index of the tap entry whose output time is the one JUCE passed to the callback that started
    // the burst, or -1. Cycles are at least 0.3 ms apart, so 100 us cannot match the wrong one.
    int findCycle (const hal::TimestampTap& tap, double outputNs)
    {
        for (size_t i = 0; i < tap.size(); ++i)
            if (std::abs (hal::hostTicksToNs (tap[i].outputTicks) - outputNs) < matchToleranceNs)
                return (int) i;

        return -1;
    }
}

Result runSession (Engine& engine, const Options& options)
{
    Result r;
    auto* device = engine.deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        r.problem = "no audio device is open";
        return r;
    }

    engine.refreshHal();
    const auto setup = engine.deviceManager.getAudioDeviceSetup();
    r.sampleRate = device->getCurrentSampleRate();
    r.blockSize = engine.callback.getBlockSize();
    r.captureName = setup.outputDeviceName;
    r.aggregate = engine.getAggregateNumbers();

    if (engine.getAggregate() == kAudioObjectUnknown)
    {
        r.problem = "the aggregate device JUCE built for this device pair was not found";
        return r;
    }

    const auto captureId = hal::findDevice (r.captureName, true, false);

    if (captureId == kAudioObjectUnknown)
    {
        r.problem = "output device '" + r.captureName + "' has no input side to record from";
        return r;
    }

    r.capture = hal::readNumbers (captureId);

    if (std::abs (r.capture.nominalRate - r.sampleRate) > 1.0)
    {
        r.problem = "'" + r.captureName + "' runs at " + String (r.capture.nominalRate, 0) + " Hz but the app at "
                  + String (r.sampleRate, 0) + " Hz";
        return r;
    }

    if (options.captureBuffer > 0)
        hal::setBufferSize (captureId, options.captureBuffer);

    r.chainLatencySamples = options.withChain ? engine.chain.getGraph().getLatencySamples() : 0;
    engine.chain.setForceBypassAll (! options.withChain);

    const auto seconds = (double) options.runs * options.spacingMs / 1000.0 + 1.5;
    hal::Capture capture (captureId, seconds, r.sampleRate);
    hal::TimestampTap tap (engine.getAggregate(), 200000);

    if (capture.numChannels() == 0)
    {
        engine.chain.setForceBypassAll (false);
        r.problem = "could not open '" + r.captureName + "' for recording";
        return r;
    }

    engine.resetOverloads();
    engine.callback.clearInjections();
    engine.callback.resetLoad();

    const auto started = capture.start() && tap.start();

    if (started)
    {
        engine.callback.setInjecting (true);
        wait (300);

        for (int i = 0; i < options.runs; ++i)
        {
            engine.callback.armBurst();
            wait ((int) options.spacingMs);
        }

        wait (200);
        engine.callback.setInjecting (false);
    }

    capture.stop();
    tap.stop();
    engine.chain.setForceBypassAll (false);

    r.captureBufferUsed = hal::readNumbers (captureId).bufferSize;

    if (options.captureBuffer > 0)
        hal::setBufferSize (captureId, r.capture.bufferSize);

    if (! started)
    {
        r.problem = "could not start the recording or the timestamp tap";
        return r;
    }

    r.overloads = engine.getOverloads();
    r.load = engine.callback.getLoad();
    r.captureGaps = capture.gaps();

    std::vector<double> wake;

    for (size_t i = 0; i < tap.size(); i += 4)
        wake.push_back ((hal::hostTicksToNs (tap[i].nowTicks) - hal::hostTicksToNs (tap[i].inputTicks)) / 1.0e6);

    r.wakeMs = summarise (wake).median;

    const auto& burst = engine.callback.getBurst();
    const auto rate = r.sampleRate;
    const auto chainNs = 1.0e9 * r.chainLatencySamples / rate;
    const auto readerFloorNs = 1.0e9 * (r.capture.inputLatency + r.capture.inputSafety + r.capture.inputStreamLatency) / rate;

    for (int i = 0; i < engine.callback.numInjections(); ++i)
    {
        Run run;
        const auto injection = engine.callback.getInjection (i);
        const auto cycle = findCycle (tap, (double) injection.outputHostNs);

        if (cycle < 0)
        {
            r.runs.push_back (run);
            continue;
        }

        // The burst's first sample is the first sample of the input buffer of this cycle.
        const auto inputNs = hal::hostTicksToNs (tap[(size_t) cycle].inputTicks);
        const auto outputNs = hal::hostTicksToNs (tap[(size_t) cycle].outputTicks) + chainNs;

        int firstBuffer = -1;

        for (size_t b = 0; b < capture.numBuffers(); ++b)
            if (hal::hostTicksToNs (capture.buffer (b).inputTicks) >= inputNs - searchBeforeMs * 1.0e6)
            {
                firstBuffer = (int) b;
                break;
            }

        if (firstBuffer < 0)
        {
            r.runs.push_back (run);
            continue;
        }

        const auto from = (int) capture.buffer ((size_t) firstBuffer).firstSample;
        const auto to = from + (int) (rate * (searchBeforeMs + searchAfterMs) / 1000.0);

        DelayEstimate best;

        for (int c = 0; c < capture.numChannels(); ++c)
        {
            const auto estimate = estimateDelay (burst.data(), (int) burst.size(), capture.data (c), (int) capture.numSamples(), from, to);

            if (estimate.valid && (! best.valid || estimate.coefficient > best.coefficient))
                best = estimate;
        }

        const auto arrivalNs = best.valid ? capture.timeOfSampleNs (best.delaySamples, rate) : -1.0;

        if (arrivalNs > 0.0)
        {
            run.trustworthy = isTrustworthy (best);
            run.latencyMs = (arrivalNs + readerFloorNs - inputNs) / 1.0e6;
            run.declaredMs = (outputNs - inputNs) / 1.0e6;
            run.excessMs = run.latencyMs - run.declaredMs;
            run.coefficient = best.coefficient;
            run.sidelobeDb = best.sidelobeDb;
        }

        r.runs.push_back (run);
    }

    std::vector<double> latency, declared, excess;

    for (auto& run : r.runs)
    {
        if (! run.trustworthy)
            continue;

        latency.push_back (run.latencyMs);
        declared.push_back (run.declaredMs);
        excess.push_back (run.excessMs);
    }

    r.trustworthyRuns = (int) latency.size();
    r.latency = summarise (latency);
    r.declared = summarise (declared);
    r.excess = summarise (excess);
    return r;
}

} // namespace mfx::measure
