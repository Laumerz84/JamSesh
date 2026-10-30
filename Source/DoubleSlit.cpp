#include "DoubleSlit.h"
#include "DspUtil.h"

namespace
{
    constexpr int subSources = 7;          // points across each slit's width
    constexpr int numBands = 24;           // for per-frequency geometry
    constexpr int maxImpulseLength = 1 << 17;
    constexpr int maxEarPoints = 262144;

    // Raised-cosine crossfade between log-spaced bands; weights sum to 1
    float bandWeight (double f, int band, const std::vector<double>& centres)
    {
        const int count = (int) centres.size();
        const double lc0 = std::log (centres.front());
        const double p = (std::log (std::max (f, 1.0)) - lc0) / (std::log (centres.back()) - lc0) * (count - 1);
        if (p <= 0.0)          return band == 0 ? 1.0f : 0.0f;
        if (p >= count - 1)    return band == count - 1 ? 1.0f : 0.0f;
        const int i = (int) p;
        const float lower = 0.5f * (1.0f + (float) std::cos (juce::MathConstants<double>::pi * (p - i)));
        if (band == i)     return lower;
        if (band == i + 1) return 1.0f - lower;
        return 0.0f;
    }
}

DoubleSlitEngine::DoubleSlitEngine (juce::AudioProcessorValueTreeState& state, const ScopeBuffer& input)
    : juce::Thread ("Double slit IR builder"), apvts (state), inputScope (input)
{
    pOn         = apvts.getRawParameterValue ("dsOn");
    pSizing     = apvts.getRawParameterValue ("dsSizing");
    pMultiple   = apvts.getRawParameterValue ("dsMultiple");
    pSeparation = apvts.getRawParameterValue ("dsSeparation");
    pWidth      = apvts.getRawParameterValue ("dsWidth");
    pFringes    = apvts.getRawParameterValue ("dsFringes");
    pDistance   = apvts.getRawParameterValue ("dsDistance");
    pEar        = apvts.getRawParameterValue ("dsEar");
    pMix        = apvts.getRawParameterValue ("dsMix");

    startThread (juce::Thread::Priority::low);
}

DoubleSlitEngine::~DoubleSlitEngine()
{
    stopThread (4000);
}

void DoubleSlitEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    maxBlock = (int) spec.maximumBlockSize;
    convolution.prepare (spec);
    dryCopy.setSize ((int) spec.numChannels, maxBlock);
    mixSm.reset (spec.sampleRate, 0.02);
    mixSm.setCurrentAndTargetValue (pMix->load() * 0.01f);
    wasOn = false;
}

DoubleSlitEngine::Settings DoubleSlitEngine::currentSettings() const
{
    Settings s;
    s.sizing     = (int) pSizing->load();
    s.multiple   = pMultiple->load();
    s.separation = pSeparation->load();
    s.width      = pWidth->load();
    s.fringes    = (int) pFringes->load();
    s.distance   = pDistance->load();
    s.ear        = (int) pEar->load();
    s.sampleRate = sampleRate.load();
    s.pitchHz    = trackedPitch.load();
    return s;
}

float DoubleSlitEngine::earSinTheta (const Settings& s)
{
    // wavelength / separation that defines where the fringes fall on the ear
    const double ratio = s.sizing == fixedMetres ? (speedOfSound / fixedReferenceHz) / s.separation
                                                 : 1.0 / s.multiple;
    return (float) std::min (0.98, 0.5 * s.fringes * ratio);
}

float DoubleSlitEngine::displayRatio (const Settings& s)
{
    if (s.sizing != fixedMetres)
        return s.multiple;
    const float f = s.pitchHz > 0.0f ? s.pitchHz : fixedReferenceHz;
    return (float) (s.separation * f / speedOfSound);
}

void DoubleSlitEngine::run()
{
    Settings last;
    bool have = false;
    std::vector<float> l (8192), r (8192), mid (8192);

    while (! threadShouldExit())
    {
        wait (80);
        if (pOn->load() < 0.5f)
            continue;

        if ((int) pSizing->load() != perFrequency)
        {
            inputScope.copyLatest (l.data(), r.data(), (int) l.size());
            for (size_t i = 0; i < mid.size(); ++i)
                mid[i] = 0.5f * (l[i] + r[i]);
            const float f = DspUtil::estimateFrequency (mid.data(), (int) mid.size(), sampleRate.load());
            if (f > 30.0f && f < 5000.0f)
                trackedPitch = f;
        }

        const auto s = currentSettings();
        const bool changed = ! have
            || s.sizing != last.sizing || s.multiple != last.multiple || s.separation != last.separation
            || s.width != last.width || s.fringes != last.fringes || s.distance != last.distance
            || s.ear != last.ear || s.sampleRate != last.sampleRate
            || (s.sizing == followPitch && std::abs (s.pitchHz / last.pitchHz - 1.0f) > 0.03f);
        if (! changed)
            continue;

        building = true;
        auto ir = buildImpulse (s);
        building = false;
        if (threadShouldExit())
            return;

        if (ir.getNumSamples() > 0)
        {
            convolution.loadImpulseResponse (std::move (ir), s.sampleRate, juce::dsp::Convolution::Stereo::no,
                                             juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
            ready = true;
        }
        last = s;
        have = true;
    }
}

juce::AudioBuffer<float> DoubleSlitEngine::buildImpulse (const Settings& s)
{
    const double c = speedOfSound, sr = s.sampleRate, L = s.distance;
    const double sinT = earSinTheta (s);
    const double Y = L * sinT / std::sqrt (1.0 - sinT * sinT);   // ear half-width
    earWidthM = (float) (2.0 * Y);
    pitchUsedHz = s.pitchHz;

    // Per-frequency geometry (or frequency-dependent fringe weighting) needs bands;
    // otherwise one geometry covers the whole spectrum.
    const bool multiBand = s.sizing == perFrequency || (s.sizing == fixedMetres && s.ear != physical);
    std::vector<double> centres;
    if (multiBand)
        for (int b = 0; b < numBands; ++b)
            centres.push_back (30.0 * std::pow (16000.0 / 30.0, b / (numBands - 1.0)));
    else
        centres.push_back (s.sizing == followPitch ? s.pitchHz : fixedReferenceHz);

    auto separationFor = [&] (double f)
    {
        return s.sizing == fixedMetres ? (double) s.separation
                                       : s.multiple * c / (s.sizing == followPitch ? s.pitchHz : f);
    };
    // nearest slit point to the ear sets the instantaneous (zero-delay) path
    auto shortestPath = [&] (double d, double a) { return std::hypot (L, std::max (0.0, 0.5 * (d - a) - Y)); };

    double maxDelay = 0.0;
    for (double f : centres)
    {
        const double d = separationFor (f), a = s.width * d;
        const double rMax = std::hypot (L, Y + 0.5 * (d + a));
        maxDelay = std::max (maxDelay, (rMax - shortestPath (d, a)) / c);
    }
    spreadMs = (float) (maxDelay * 1000.0);

    int len = (int) std::ceil (maxDelay * sr) + 4;
    truncated = len > maxImpulseLength;
    len = juce::jlimit (64, maxImpulseLength, len);

    int order = 6;
    while ((1 << order) < len * 2)
        ++order;
    const int N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> work ((size_t) N * 2, 0.0f);
    std::vector<float> spectrum (multiBand ? (size_t) N + 2 : 0, 0.0f);

    for (size_t b = 0; b < centres.size(); ++b)
    {
        if (threadShouldExit())
            return {};

        const double f = centres[b], lambda = c / f;
        const double d = separationFor (f), a = s.width * d;
        const double rMin = shortestPath (d, a);

        // ear sampling: fine enough that neighbouring points differ by < half a sample,
        // relaxed for low bands where only long wavelengths survive the band filter
        const double dyMin = 0.5 * c / sr;
        const double dyBand = multiBand ? std::max (dyMin, c / (f * 1.5 * 4.0)) : dyMin;
        const int M = juce::jlimit (64, maxEarPoints, (int) std::ceil (2.0 * Y / dyBand));
        const double dy = 2.0 * Y / M;

        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < M; ++i)
        {
            const double y = -Y + (i + 0.5) * dy;

            double earGain = 1.0;
            if (s.ear != physical)
            {
                const double diff = std::hypot (L, y + 0.5 * d) - std::hypot (L, y - 0.5 * d);
                const double c2 = std::cos (juce::MathConstants<double>::pi * diff / lambda);
                const double intensity = c2 * c2;
                earGain = s.ear == brightOnly ? (intensity > 0.5 ? 1.0 : 0.0) : intensity;
                if (earGain <= 0.0)
                    continue;
            }

            for (int side = -1; side <= 1; side += 2)
            {
                for (int j = 0; j < subSources; ++j)
                {
                    const double ys = side * 0.5 * d + (j / (subSources - 1.0) - 0.5) * a;
                    const double r = std::hypot (L, y - ys);
                    const double w = std::sqrt (rMin / r) * dy * earGain / subSources;
                    const double pos = (r - rMin) / c * sr;
                    const int i0 = (int) pos;
                    if (i0 + 1 >= len)
                        continue;
                    const double frac = pos - i0;
                    work[(size_t) i0]     += (float) (w * (1.0 - frac));
                    work[(size_t) i0 + 1] += (float) (w * frac);
                }
            }
        }

        if (! multiBand)
            break;

        fft.performRealOnlyForwardTransform (work.data(), true);
        for (int k = 0; k <= N / 2; ++k)
        {
            const float g = bandWeight (k * sr / N, (int) b, centres);
            if (g > 0.0f)
            {
                spectrum[(size_t) k * 2]     += g * work[(size_t) k * 2];
                spectrum[(size_t) k * 2 + 1] += g * work[(size_t) k * 2 + 1];
            }
        }
    }

    if (multiBand)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        std::copy (spectrum.begin(), spectrum.end(), work.begin());
        fft.performRealOnlyInverseTransform (work.data());
    }

    // Normalise so the average level across the audible band is unity, with peaks capped at +6 dB
    std::vector<float> check ((size_t) N * 2, 0.0f);
    std::copy (work.begin(), work.begin() + len, check.begin());
    fft.performRealOnlyForwardTransform (check.data(), true);
    double sumSq = 0.0, peak = 0.0;
    int count = 0;
    for (int k = 1; k <= N / 2; ++k)
    {
        const double fk = k * sr / N;
        if (fk < 50.0 || fk > std::min (15000.0, sr * 0.45))
            continue;
        const double m = std::hypot (check[(size_t) k * 2], check[(size_t) k * 2 + 1]);
        sumSq += m * m;
        peak = std::max (peak, m);
        ++count;
    }
    const double rms = count > 0 ? std::sqrt (sumSq / count) : 0.0;
    if (rms < 1.0e-12)
        return {};
    double gain = 1.0 / rms;
    if (peak * gain > 2.0)
        gain = 2.0 / peak;

    juce::AudioBuffer<float> ir (1, len);
    const int fade = std::min (256, len / 4);
    for (int i = 0; i < len; ++i)
    {
        const float tail = i >= len - fade ? (float) (len - i) / (float) fade : 1.0f;
        ir.setSample (0, i, (float) (work[(size_t) i] * gain) * tail);
    }
    return ir;
}

void DoubleSlitEngine::process (juce::AudioBuffer<float>& buffer, int numChannels)
{
    if (pOn->load() < 0.5f)
    {
        wasOn = false;
        return;
    }
    if (! wasOn)
    {
        convolution.reset();
        mixSm.setCurrentAndTargetValue (pMix->load() * 0.01f);
        wasOn = true;
    }
    if (! ready)
        return;

    const int n = buffer.getNumSamples();
    dryCopy.setSize (numChannels, n, false, false, true);
    for (int ch = 0; ch < numChannels; ++ch)
        dryCopy.copyFrom (ch, 0, buffer, ch, 0, n);

    auto block = juce::dsp::AudioBlock<float> (buffer).getSubsetChannelBlock (0, (size_t) numChannels);
    for (int start = 0; start < n; start += maxBlock)
    {
        auto sub = block.getSubBlock ((size_t) start, (size_t) std::min (maxBlock, n - start));
        convolution.process (juce::dsp::ProcessContextReplacing<float> (sub));
    }

    mixSm.setTargetValue (pMix->load() * 0.01f);
    for (int i = 0; i < n; ++i)
    {
        const float m = mixSm.getNextValue();
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dry = dryCopy.getSample (ch, i);
            buffer.setSample (ch, i, dry + (buffer.getSample (ch, i) - dry) * m);
        }
    }
}
