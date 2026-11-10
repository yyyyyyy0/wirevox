#include "Correlate.h"

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <complex>

namespace mfx::measure
{
using namespace juce;

namespace
{
    constexpr double burstSeconds = 0.050;
    constexpr double endSeconds = 0.002;
    constexpr double burstAmplitude = 0.1;      // -20 dBFS
    constexpr double sweepStartHz = 300.0;
    constexpr int sidelobeGuard = 32;
    constexpr int sincHalfWidth = 16;

    double sweepStopHz (double sampleRate) { return 0.4 * sampleRate; }

    double sinc (double x)
    {
        if (std::abs (x) < 1.0e-9)
            return 1.0;

        const auto px = MathConstants<double>::pi * x;
        return std::sin (px) / px;
    }

    // Weights of a Hann-windowed sinc (16 taps each side) that read a signal at `fraction` of a sample
    // after the index: value(index + fraction) = sum over m of weight[m] * signal[index - m].
    struct Kernel
    {
        explicit Kernel (double fraction)
        {
            for (int m = -sincHalfWidth + 1; m <= sincHalfWidth; ++m)
            {
                const auto x = fraction + (double) m;
                const auto window = 0.5 * (1.0 + std::cos (MathConstants<double>::pi * x / (double) sincHalfWidth));
                weight[(size_t) (m + sincHalfWidth - 1)] = sinc (x) * window;
            }
        }

        double weight[2 * sincHalfWidth] {};
    };
}

double testSignalAt (double sampleRate, double t)
{
    if (t < 0.0 || t > burstSeconds)
        return 0.0;

    const auto rate = (sweepStopHz (sampleRate) - sweepStartHz) / burstSeconds;
    const auto phase = 2.0 * MathConstants<double>::pi * (sweepStartHz * t + 0.5 * rate * t * t);

    double envelope = 1.0;

    if (t < endSeconds)
        envelope = 0.5 * (1.0 - std::cos (MathConstants<double>::pi * t / endSeconds));
    else if (t > burstSeconds - endSeconds)
        envelope = 0.5 * (1.0 - std::cos (MathConstants<double>::pi * (burstSeconds - t) / endSeconds));

    return burstAmplitude * envelope * std::sin (phase);
}

std::vector<float> makeTestSignal (double sampleRate)
{
    std::vector<float> signal ((size_t) std::ceil (burstSeconds * sampleRate));

    for (size_t i = 0; i < signal.size(); ++i)
        signal[i] = (float) testSignalAt (sampleRate, (double) i / sampleRate);

    return signal;
}

DelayEstimate estimateDelay (const float* reference, int numReference,
                             const float* recording, int numRecording,
                             int searchFrom, int searchTo)
{
    DelayEstimate result;

    if (numReference <= 0 || numRecording < numReference)
        return result;

    const auto lastLag = numRecording - numReference;
    searchFrom = jlimit (0, lastLag, searchFrom);
    searchTo = searchTo < 0 ? lastLag : jlimit (searchFrom, lastLag, searchTo);

    // Only the part of the recording that can matter goes through the FFT.
    const auto segmentStart = searchFrom;
    const auto segmentLength = searchTo - searchFrom + numReference;
    const auto order = (int) std::ceil (std::log2 ((double) (segmentLength + numReference)));
    dsp::FFT fft (order);
    const auto size = (size_t) fft.getSize();

    std::vector<std::complex<float>> a (size), b (size), spectrumA (size), spectrumB (size);

    for (int i = 0; i < segmentLength; ++i)
        a[(size_t) i] = recording[segmentStart + i];

    for (int i = 0; i < numReference; ++i)
        b[(size_t) i] = reference[i];

    fft.perform (a.data(), spectrumA.data(), false);
    fft.perform (b.data(), spectrumB.data(), false);

    for (size_t i = 0; i < size; ++i)
        spectrumA[i] *= std::conj (spectrumB[i]);

    fft.perform (spectrumA.data(), a.data(), true);

    const auto numLags = searchTo - searchFrom + 1;
    std::vector<double> raw ((size_t) numLags), coefficient ((size_t) numLags);

    double referenceEnergy = 0.0;

    for (int i = 0; i < numReference; ++i)
        referenceEnergy += (double) reference[i] * (double) reference[i];

    // Energy of every window of the recording through a running sum.
    double windowEnergy = 0.0;

    for (int i = 0; i < numReference; ++i)
        windowEnergy += (double) recording[searchFrom + i] * (double) recording[searchFrom + i];

    int best = 0;

    for (int lag = 0; lag < numLags; ++lag)
    {
        if (lag > 0)
        {
            const double leaving = recording[searchFrom + lag - 1], entering = recording[searchFrom + lag + numReference - 1];
            windowEnergy = jmax (0.0, windowEnergy - leaving * leaving + entering * entering);
        }

        raw[(size_t) lag] = (double) a[(size_t) lag].real();
        coefficient[(size_t) lag] = raw[(size_t) lag] / std::sqrt (referenceEnergy * windowEnergy + 1.0e-12);

        if (raw[(size_t) lag] > raw[(size_t) best])
            best = lag;
    }

    double otherPeak = 1.0e-12;

    for (int lag = 0; lag < numLags; ++lag)
        if (std::abs (lag - best) > sidelobeGuard)
            otherPeak = jmax (otherPeak, std::abs (raw[(size_t) lag]));

    // Refine between the samples: slide a band-limited copy of the reference over the recording and
    // keep the shift that correlates best. The correlation is unimodal within one sample of the peak.
    const auto coarse = searchFrom + best;

    const auto score = [&] (double shift)
    {
        const auto u = -shift;
        const auto base = (int) std::floor (u);
        const Kernel kernel (u - (double) base);
        double sum = 0.0;

        for (int n = 0; n < numReference && coarse + n < numRecording; ++n)
        {
            double value = 0.0;

            for (int m = -sincHalfWidth + 1; m <= sincHalfWidth; ++m)
            {
                const auto k = n + base - m;

                if (k >= 0 && k < numReference)
                    value += kernel.weight[(size_t) (m + sincHalfWidth - 1)] * (double) reference[k];
            }

            sum += (double) recording[coarse + n] * value;
        }

        return sum;
    };

    double low = -0.75, high = 0.75;

    for (int i = 0; i < 40; ++i)
    {
        const auto m1 = low + (high - low) / 3.0, m2 = high - (high - low) / 3.0;

        if (score (m1) < score (m2))
            low = m1;
        else
            high = m2;
    }

    result.valid = true;
    result.delaySamples = (double) coarse + 0.5 * (low + high);
    result.coefficient = coefficient[(size_t) best];
    result.sidelobeDb = 20.0 * std::log10 (jmax (1.0e-12, std::abs (raw[(size_t) best])) / otherPeak);
    return result;
}

bool isTrustworthy (const DelayEstimate& estimate)
{
    return estimate.valid && estimate.coefficient >= 0.6 && estimate.sidelobeDb >= 6.0;
}

Summary summarise (std::vector<double> values)
{
    Summary s;
    s.count = (int) values.size();

    if (values.empty())
        return s;

    std::sort (values.begin(), values.end());
    s.minimum = values.front();
    s.maximum = values.back();
    s.median = values.size() % 2 == 1 ? values[values.size() / 2]
                                      : 0.5 * (values[values.size() / 2 - 1] + values[values.size() / 2]);

    for (auto v : values)
        s.mean += v;

    s.mean /= (double) values.size();

    for (auto v : values)
        s.stddev += (v - s.mean) * (v - s.mean);

    s.stddev = std::sqrt (s.stddev / (double) values.size());
    return s;
}

} // namespace mfx::measure
