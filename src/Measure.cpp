#include "Measure.h"
#include "Text.h"

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
}

int findCycle (Span<const hal::TimestampTap::Entry> tap, double outputHostNs)
{
    for (size_t i = 0; i < (size_t) tap.size(); ++i)
        if (std::abs (hal::hostTicksToNs (tap[i].outputTicks) - outputHostNs) < matchToleranceNs)
            return (int) i;

    return -1;
}

Run analyseBurst (const BurstInputs& in, uint64_t outputHostNs)
{
    Run run;
    const auto cycle = findCycle (in.tap, (double) outputHostNs);

    if (cycle < 0)
        return run;

    // The burst's first sample is the first sample of the input buffer of this cycle.
    const auto inputNs = hal::hostTicksToNs (in.tap[(size_t) cycle].inputTicks);
    const auto outputNs = hal::hostTicksToNs (in.tap[(size_t) cycle].outputTicks) + in.chainNs;

    int firstBuffer = -1;

    for (size_t b = 0; b < (size_t) in.buffers.size(); ++b)
        if (hal::hostTicksToNs (in.buffers[b].inputTicks) >= inputNs - searchBeforeMs * 1.0e6)
        {
            firstBuffer = (int) b;
            break;
        }

    if (firstBuffer < 0)
        return run;

    const auto from = (int) in.buffers[(size_t) firstBuffer].firstSample;
    const auto to = from + (int) (in.sampleRate * (searchBeforeMs + searchAfterMs) / 1000.0);

    DelayEstimate best;

    for (auto* channel : in.channels)
    {
        const auto estimate = estimateDelay (in.burst, in.burstLength, channel, in.numSamples, from, to);

        if (estimate.valid && (! best.valid || estimate.coefficient > best.coefficient))
            best = estimate;
    }

    const auto arrivalNs = best.valid ? hal::timeOfSampleNs (in.buffers, best.delaySamples, in.sampleRate) : -1.0;

    if (arrivalNs > 0.0)
    {
        run.trustworthy = isTrustworthy (best);
        run.latencyMs = (arrivalNs + in.readerFloorNs - inputNs) / 1.0e6;
        run.declaredMs = (outputNs - inputNs) / 1.0e6;
        run.excessMs = run.latencyMs - run.declaredMs;
        run.coefficient = best.coefficient;
        run.sidelobeDb = best.sidelobeDb;
    }

    return run;
}

// Proof tool: the capture device keeps the requested IO buffer only while the session runs.
struct Session::CaptureBufferScope
{
    CaptureBufferScope (AudioObjectID idIn, int previousIn, int requested)
        : id (idIn), previous (previousIn), active (requested > 0)
    {
        if (active)
            hal::setBufferSize (id, requested);
    }

    ~CaptureBufferScope()
    {
        if (active)
            hal::setBufferSize (id, previous);
    }

    AudioObjectID id;
    int previous;
    bool active;
};

namespace
{
    constexpr int settleMs = 300, tailMs = 200;
}

Session::Session (Engine& e, const Options& o)
    : engine (e), options (o)
{
}

Session::~Session()
{
    cancel();

    if (worker.joinable())
        worker.join();
}

bool Session::fail (const String& english, const String& japanese)
{
    result.problem = english;
    result.problemJa = japanese;
    releaseEverything();
    return false;
}

// Order matters: the capture device's own buffer first, then the engine's, and the recording last.
void Session::releaseEverything()
{
    captureScope.reset();
    restore.reset();
    capture.reset();
    tap.reset();
    phase = Phase::idle;
}

bool Session::start()
{
    if (isRunning())
        return false;

    result = {};
    armed = 0;
    analysed = false;

    device = engine.deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return fail ("no audio device is open", ja ("オーディオデバイスが開いていません"));

    restore = std::make_unique<ScopedSetup> (engine);

    // With the monitor on, the aggregate holds a second output device, which changes the HAL's timestamps
    // for the cycle and the path the burst takes. The measurement is of the main output path, so the
    // monitor is off for it; the ScopedSetup brings it back.
    if (engine.getMonitorPlan().enabled)
    {
        auto off = engine.getMonitorPlan();
        off.enabled = false;
        engine.setMonitorPlan (off);
        device = engine.deviceManager.getCurrentAudioDevice();

        if (device == nullptr)
            return fail ("the audio device could not be reopened without the monitor", ja ("モニターを外してデバイスを開き直せませんでした"));
    }

    engine.refreshHal();
    const auto setup = engine.deviceManager.getAudioDeviceSetup();
    result.sampleRate = device->getCurrentSampleRate();
    result.blockSize = engine.callback.getBlockSize();
    result.captureName = setup.outputDeviceName;
    result.aggregate = engine.getAggregateNumbers();

    if (engine.getAggregate() == kAudioObjectUnknown)
        return fail ("the aggregate device JUCE built for this device pair was not found",
                     ja ("JUCE が作った合成デバイスが見つかりませんでした"));

    captureId = hal::findDevice (result.captureName, true, false);

    if (captureId == kAudioObjectUnknown)
        return fail ("output device '" + result.captureName + "' has no input side to record from",
                     ja ("出力デバイス「") + result.captureName + ja ("」には録音できる入力側がありません"));

    result.capture = hal::readNumbers (captureId);

    if (std::abs (result.capture.nominalRate - result.sampleRate) > 1.0)
        return fail ("'" + result.captureName + "' runs at " + String (result.capture.nominalRate, 0) + " Hz but the app at "
                         + String (result.sampleRate, 0) + " Hz",
                     ja ("「") + result.captureName + ja ("」は ") + String (result.capture.nominalRate, 0) + ja (" Hz、アプリは ")
                         + String (result.sampleRate, 0) + ja (" Hz で動いているため測れません"));

    captureScope = std::make_unique<CaptureBufferScope> (captureId, result.capture.bufferSize, options.captureBuffer);

    result.chainLatencySamples = options.withChain ? engine.chain.getGraph().getLatencySamples() : 0;
    engine.chain.setForceBypassAll (! options.withChain);

    const auto seconds = (double) options.runs * options.spacingMs / 1000.0 + 1.5;
    capture = std::make_unique<hal::Capture> (captureId, seconds, result.sampleRate);
    tap = std::make_unique<hal::TimestampTap> (engine.getAggregate(), 200000);

    if (capture->numChannels() == 0)
        return fail ("could not open '" + result.captureName + "' for recording",
                     ja ("「") + result.captureName + ja ("」を録音用に開けませんでした"));

    engine.resetOverloads();
    engine.callback.clearInjections();
    engine.callback.resetLoad();

    if (! (capture->start() && tap->start()))
    {
        capture->stop();
        tap->stop();
        return fail ("could not start the recording or the timestamp tap", ja ("録音を開始できませんでした"));
    }

    engine.callback.setInjecting (true);
    startCount = engine.callback.getStartCount();
    startedMs = Time::getMillisecondCounterHiRes();
    phase = Phase::recording;
    return true;
}

bool Session::deviceIsUnchanged() const
{
    return engine.deviceManager.getCurrentAudioDevice() == device && engine.callback.getStartCount() == startCount;
}

String Session::getProgress() const
{
    if (phase == Phase::recording)
        return ja ("仮想デバイスを計測中（試験音 ") + String (armed) + "/" + String (options.runs) + ja ("）。この間マイクの音は止まり、出力に試験音が流れます");

    if (phase == Phase::analysing)
        return ja ("計測結果を解析中");

    return {};
}

void Session::cancel()
{
    if (phase == Phase::recording)
    {
        stopRecording();
        fail ("cancelled", ja ("中止しました"));
    }
    else if (phase == Phase::analysing)
    {
        worker.join();
        fail ("cancelled", ja ("中止しました"));
    }
}

bool Session::tick()
{
    if (phase == Phase::idle)
        return false;

    if (phase == Phase::analysing)
    {
        if (! analysed.load())
            return false;

        worker.join();
        capture.reset();
        tap.reset();
        phase = Phase::idle;
        return true;
    }

    // The plan this run started with is void when the device is replaced (an interface unplugged, a
    // choice made elsewhere): the burst would be injected into something else and the numbers are
    // for another device.
    if (! deviceIsUnchanged())
    {
        stopRecording();
        fail ("the device changed while measuring", ja ("計測中にデバイスが変わったため中止しました"));
        return true;
    }

    const auto elapsed = Time::getMillisecondCounterHiRes() - startedMs;

    if (armed < options.runs && elapsed >= settleMs + armed * options.spacingMs)
    {
        engine.callback.armBurst();
        ++armed;
    }

    if (armed < options.runs || elapsed < settleMs + options.runs * options.spacingMs + tailMs)
        return false;

    stopRecording();

    if (result.problem.isNotEmpty())
    {
        fail (result.problem, result.problemJa);
        return true;
    }

    phase = Phase::analysing;
    worker = std::thread ([this] { analyse(); });
    return false;
}

// Ends the injection and the recording and puts the engine back; what the recording holds stays for
// the analysis.
void Session::stopRecording()
{
    const auto disturbed = engine.callback.injectionWasDisturbed();
    engine.callback.setInjecting (false);
    capture->stop();
    tap->stop();

    result.captureBufferUsed = hal::readNumbers (captureId).bufferSize;
    result.overloads = engine.getOverloads();
    result.load = engine.callback.getLoad();
    result.captureGaps = capture->gaps();

    burst = engine.callback.getBurst();
    injectionOutputNs.clear();

    for (int i = 0; i < engine.callback.numInjections(); ++i)
        injectionOutputNs.push_back (engine.callback.getInjection (i).outputHostNs);

    captureScope.reset();
    restore.reset();

    if (disturbed)
    {
        result.problem = "a block arrived that the injection buffer could not hold (the device's channels or block size changed); "
                         "that block was played as silence and the run is invalid";
        result.problemJa = ja ("試験音の送出中にチャンネル数かブロックサイズが変わったため、この計測は無効です");
        return;
    }

    std::vector<double> wake;

    for (size_t i = 0; i < tap->size(); i += 4)
        wake.push_back ((hal::hostTicksToNs ((*tap)[i].nowTicks) - hal::hostTicksToNs ((*tap)[i].inputTicks)) / 1.0e6);

    result.wakeMs = summarise (wake).median;
}

// Worker thread: reads the recording and the burst copy, writes only into the result.
void Session::analyse()
{
    const auto rate = result.sampleRate;

    BurstInputs inputs;
    inputs.tap = tap->span();
    inputs.buffers = capture->bufferSpan();

    for (int c = 0; c < capture->numChannels(); ++c)
        inputs.channels.push_back (capture->data (c));

    inputs.numSamples = (int) capture->numSamples();
    inputs.burst = burst.data();
    inputs.burstLength = (int) burst.size();
    inputs.sampleRate = rate;
    inputs.chainNs = 1.0e9 * result.chainLatencySamples / rate;
    inputs.readerFloorNs = 1.0e9 * (result.capture.inputLatency + result.capture.inputSafety + result.capture.inputStreamLatency) / rate;

    for (auto outputNs : injectionOutputNs)
        result.runs.push_back (analyseBurst (inputs, outputNs));

    std::vector<double> latency, declared, excess;

    for (auto& run : result.runs)
    {
        if (! run.trustworthy)
            continue;

        latency.push_back (run.latencyMs);
        declared.push_back (run.declaredMs);
        excess.push_back (run.excessMs);
    }

    result.trustworthyRuns = (int) latency.size();
    result.latency = summarise (latency);
    result.declared = summarise (declared);
    result.excess = summarise (excess);
    analysed = true;
}

//==============================================================================
Result runSession (Engine& engine, const Options& options)
{
    Session session (engine, options);

    if (! session.start())
        return session.getResult();

    while (session.isRunning())
    {
        wait (50);
        session.tick();
    }

    return session.getResult();
}

std::optional<ExcessRecord> makeExcessRecord (const Result& r, const String& outputDevice, juce::Time now, int minRuns)
{
    if (r.problem.isNotEmpty() || r.trustworthyRuns < minRuns || outputDevice.isEmpty() || r.sampleRate <= 0.0)
        return std::nullopt;

    // A hair below zero is noise on a device that adds nothing; clearly negative means the timing was wrong.
    if (r.excess.median < -0.5)
        return std::nullopt;

    ExcessRecord record;
    record.outputDevice = outputDevice;
    record.sampleRate = r.sampleRate;
    record.excessMs = jmax (0.0, r.excess.median);
    record.runs = r.trustworthyRuns;
    record.measuredAt = now;
    return record;
}

} // namespace mfx::measure
