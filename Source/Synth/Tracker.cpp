#include "SynthInternal.h"

// Monophonic pitch tracker: McLeod pitch method (normalised square difference function, NSDF)
// computed with an FFT autocorrelation on the input decimated to ~12 kHz, every ~2.7 ms.
// The analysis window adapts: while a note is held cleanly it shrinks to ~2.6 periods of that
// note (low latency, octave-down errors impossible); otherwise it covers 2 periods of the
// lowest note of the selected range. Octave-up errors are rejected with a stickiness rule, and
// pitch jumps need two agreeing frames unless they follow an attack (onset).
//
// Cost at 48 kHz: ~25 flops per input sample plus one 128..2048-point real FFT pair per hop
// (silent input skips the FFT) -> well under 1 % of one core.

namespace synth
{
void PitchTracker::prepare (double sampleRate)
{
    sr = sampleRate;
    decim = juce::jmax (1, (int) std::round (sr / 12000.0));
    srD = sr / decim;
    hop = juce::jmax (8, (int) std::round (srD * 0.0027));

    const double fc = juce::jmin (3600.0, 0.3 * srD);
    decimLp[0].lowPass (sr, fc, 0.5412);
    decimLp[1].lowPass (sr, fc, 1.3066);
    dcR = (float) (1.0 - juce::MathConstants<double>::twoPi * 15.0 / sr);

    for (int o = 6; o <= 11; ++o)
        if (ffts[(size_t) o] == nullptr)
            ffts[(size_t) o] = std::make_unique<juce::dsp::FFT> (o);
    fftBuf.assign (2 * 2048 + 16, 0.0f);
    frame.assign (1100, 0.0f);
    nsdf.assign (1100, 0.0f);

    sqRing.assign ((size_t) juce::nextPowerOfTwo ((int) (sr / 25.0) + 16), 0.0f);

    envAtt = onePole (0.001f, sr);
    envRel = onePole (0.020f, sr);
    onsetCoef = onePole (0.003f, sr);
    slowCoef = onePole (0.050f, srD, hop);
    reset();
}

void PitchTracker::reset() noexcept
{
    f0 = 0.0f; clarity = 0.0f; confidence = 0.0f; voiced = false;
    envelope = 0.0f; levelDb = -120.0f;
    framesSinceOnset = 1000;
    for (auto& b : decimLp)
        b.z1 = b.z2 = 0.0f;
    dcX1 = dcY1 = 0.0f;
    std::fill (ring.begin(), ring.end(), 0.0f);
    std::fill (sqRing.begin(), sqRing.end(), 0.0f);
    sqSum = 0.0;
    sqPos = 0;
    envLen = juce::jmax (16, (int) (sr * 0.010));
    preX1 = 0.0f; onsetEnv = 0.0f; slowDb = -120.0f; onsetArmed = true; refractory = 0;
    period = 0.0f; pendingF = 0.0f; pendingCount = 0; lastClarity = 0.0f;
    badFrames = 0;
}

void PitchTracker::setRange (int rangeIndex) noexcept
{
    const int i = juce::jlimit (0, 3, rangeIndex);
    fMin = rangeLo[i];
    fMax = rangeHi[i];
}

void PitchTracker::setEnvelopeWindow (int samples) noexcept
{
    const int mask = (int) sqRing.size() - 1;
    samples = juce::jlimit (16, mask - 1, samples);
    // grow or shrink the running window one sample at a time so the sum stays exact
    while (envLen < samples)
    {
        sqSum += sqRing[(size_t) ((sqPos - 1 - envLen) & mask)];
        ++envLen;
    }
    while (envLen > samples)
    {
        --envLen;
        sqSum -= sqRing[(size_t) ((sqPos - 1 - envLen) & mask)];
    }
}

void PitchTracker::process (const float* x, int n) noexcept
{
    const int sqMask = (int) sqRing.size() - 1;

    for (int i = 0; i < n; ++i)
    {
        const float s = x[i];

        // envelope: RMS over one tracked period (ripple-free for periodic input)
        const float s2 = s * s;
        sqRing[(size_t) sqPos] = s2;
        sqSum += s2 - sqRing[(size_t) ((sqPos - envLen) & sqMask)];
        sqPos = (sqPos + 1) & sqMask;
        const float rms = std::sqrt ((float) juce::jmax (0.0, sqSum / envLen));
        envelope += (rms - envelope) * (rms > envelope ? envAtt : envRel);

        // onset detector input: pre-emphasised energy (attacks are broadband)
        const float pe = s - 0.97f * preX1;
        preX1 = s;
        onsetEnv += (pe * pe - onsetEnv) * onsetCoef;

        // DC block + anti-alias low-pass + decimate
        float y = s - dcX1 + dcR * dcY1;
        dcX1 = s;
        dcY1 = y;
        y = decimLp[1].process (decimLp[0].process (y));

        if (++decimPhase >= decim)
        {
            decimPhase = 0;
            ring[(size_t) ringPos] = y;
            ringPos = (ringPos + 1) & (ringSize - 1);
            if (++hopCounter >= hop)
            {
                hopCounter = 0;
                analyse();
            }
        }
    }

    // keep the running sum exact (float error would slowly accumulate)
    recalcCounter += n;
    if (recalcCounter > (int) sr)
    {
        recalcCounter = 0;
        double sum = 0.0;
        for (int k = 1; k <= envLen; ++k)
            sum += sqRing[(size_t) ((sqPos - k) & sqMask)];
        sqSum = sum;
    }

    // denormal guards for silent input
    if (envelope < 1.0e-9f) envelope = 0.0f;
    if (onsetEnv < 1.0e-15f) onsetEnv = 0.0f;
    for (auto& b : decimLp)
    {
        if (std::abs (b.z1) < 1.0e-15f) b.z1 = 0.0f;
        if (std::abs (b.z2) < 1.0e-15f) b.z2 = 0.0f;
    }
    if (std::abs (dcY1) < 1.0e-15f) dcY1 = 0.0f;
}

void PitchTracker::analyse() noexcept
{
    ++frames;
    if (framesSinceOnset < 100000)
        ++framesSinceOnset;

    levelDb = juce::Decibels::gainToDecibels (envelope, -120.0f);

    // ---- onsets ----
    const float onDb = 10.0f * std::log10 (onsetEnv + 1.0e-12f);
    if (refractory > 0)
        --refractory;
    if (onsetArmed && refractory == 0 && onDb > slowDb + onsetThresholdDb && levelDb > gateDb)
    {
        ++onsets;
        onsetArmed = false;
        refractory = (int) (0.050 * srD / hop);
        framesSinceOnset = 0;
    }
    if (onDb < slowDb + 0.4f * onsetThresholdDb)
        onsetArmed = true;
    slowDb += (onDb - slowDb) * slowCoef;

    // ---- pitch ----
    auto unvoicedFrame = [this]
    {
        candidate = 0.0f;
        clarity = 0.0f;
        lastClarity = 0.0f;
        confidence += (0.0f - confidence) * 0.25f;
        if (++badFrames >= 3)
            voiced = false;
    };

    if (levelDb < gateDb)
    {
        unvoicedFrame();
        if (levelDb < gateDb - 6.0f)
            voiced = false;
        return;
    }

    const int wMax = juce::jmin (1024, (int) (2.0 * srD / fMin));
    int W = wMax;
    if (voiced && lastClarity > 0.8f && period > 0.0f)
        W = juce::jlimit (64, wMax, (int) (2.6f * period) + 2);

    // newest W samples, mean removed
    double mean = 0.0;
    for (int k = 0; k < W; ++k)
    {
        frame[(size_t) k] = ring[(size_t) ((ringPos - W + k) & (ringSize - 1))];
        mean += frame[(size_t) k];
    }
    mean /= W;
    double energy = 0.0;
    for (int k = 0; k < W; ++k)
    {
        frame[(size_t) k] -= (float) mean;
        energy += (double) frame[(size_t) k] * frame[(size_t) k];
    }
    if (energy < 1.0e-10)
    {
        unvoicedFrame();
        return;
    }

    // autocorrelation via FFT (zero-padded to >= 2W so it is linear, not circular)
    int order = 6;
    while ((1 << order) < 2 * W && order < 11)
        ++order;
    const int N = 1 << order;
    std::fill (fftBuf.begin(), fftBuf.begin() + 2 * N, 0.0f);
    std::copy (frame.begin(), frame.begin() + W, fftBuf.begin());
    auto& fft = *ffts[(size_t) order];
    fft.performRealOnlyForwardTransform (fftBuf.data());
    for (int k = 0; k < N; ++k)
    {
        const float re = fftBuf[(size_t) (2 * k)], im = fftBuf[(size_t) (2 * k + 1)];
        fftBuf[(size_t) (2 * k)] = re * re + im * im;
        fftBuf[(size_t) (2 * k + 1)] = 0.0f;
    }
    fft.performRealOnlyInverseTransform (fftBuf.data());
    if (fftBuf[0] <= 0.0f)
    {
        unvoicedFrame();
        return;
    }
    const double scale = energy / fftBuf[0];   // independent of the FFT engine's normalisation

    const int tauMin = juce::jmax (2, (int) (srD / fMax));
    const int tauMax = juce::jmin ((int) (srD / fMin) + 2, W * 2 / 3);
    if (tauMax <= tauMin + 2)
    {
        unvoicedFrame();
        return;
    }

    double m = 2.0 * energy;
    nsdf[0] = 1.0f;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        const double a = frame[(size_t) (tau - 1)], b = frame[(size_t) (W - tau)];
        m -= a * a + b * b;
        nsdf[(size_t) tau] = m > 1.0e-12 ? (float) (2.0 * fftBuf[(size_t) tau] * scale / m) : 0.0f;
    }

    float c = 0.0f;
    const float tau = pickPeriod (tauMin, tauMax, c);
    lastClarity = c;
    clarity = c;

    const float fCand = tau > 0.0f ? (float) (srD / tau) : 0.0f;
    // right after an attack the window is only partly filled with the new note, which lowers the
    // NSDF peak (~2 (L - P) / (2 L - P) for L samples of note): accept a lower clarity there
    const float enter = framesSinceOnset < 12 ? 0.68f : 0.8f;
    const bool good = fCand > 0.0f && c >= (voiced ? 0.6f : enter)
                      && fCand >= fMin * 0.97f && fCand <= fMax * 1.03f;
    if (! good)
    {
        unvoicedFrame();
        return;
    }

    badFrames = 0;
    candidate = fCand;
    bool accept = true;
    if (voiced && f0 > 0.0f && framesSinceOnset > 8
        && std::abs (12.0f * std::log2 (fCand / f0)) > 1.0f)
    {
        // a jump without an attack must be confirmed by a second frame
        if (pendingF > 0.0f && std::abs (fCand / pendingF - 1.0f) < 0.03f)
            ++pendingCount;
        else
            pendingCount = 1;
        pendingF = fCand;
        accept = pendingCount >= 2;
    }
    if (accept)
    {
        f0 = fCand;
        period = tau;
        pendingCount = 0;
        pendingF = 0.0f;
        voiced = true;
    }
    const float target = juce::jlimit (0.0f, 1.0f, (c - 0.5f) / 0.45f);
    confidence += (target - confidence) * 0.35f;

    // the follow envelope integrates exactly one period (several for very high notes)
    if (voiced && f0 > 0.0f)
    {
        const double p = sr / f0;
        setEnvelopeWindow ((int) std::round (p * std::ceil (0.004 * sr / p)));
    }
}

float PitchTracker::pickPeriod (int tauMin, int tauMax, float& clarityOut) noexcept
{
    constexpr int maxPeaks = 48;
    int pos[maxPeaks] {};
    float val[maxPeaks] {};
    int numPeaks = 0;

    // skip the lobe around lag 0, then take the maximum of every positive lobe ("key maxima")
    int t = 1;
    while (t < tauMax && nsdf[(size_t) t] > 0.0f)
        ++t;
    int bestPos = -1;
    float bestVal = 0.0f;
    bool inLobe = false;
    for (; t < tauMax; ++t)
    {
        const float v = nsdf[(size_t) t];
        if (v > 0.0f)
        {
            if (! inLobe)
            {
                inLobe = true;
                bestPos = -1;
                bestVal = 0.0f;
            }
            if (v > bestVal)
            {
                bestVal = v;
                bestPos = t;
            }
        }
        else if (inLobe)
        {
            inLobe = false;
            if (bestPos >= tauMin && numPeaks < maxPeaks)
            {
                pos[numPeaks] = bestPos;
                val[numPeaks++] = bestVal;
            }
        }
    }
    // an unterminated last lobe only counts if its maximum is a real peak, not the edge
    if (inLobe && bestPos >= tauMin && bestPos < tauMax - 1 && numPeaks < maxPeaks)
    {
        pos[numPeaks] = bestPos;
        val[numPeaks++] = bestVal;
    }

    clarityOut = 0.0f;
    if (numPeaks == 0)
        return 0.0f;

    float nMax = 0.0f;
    for (int i = 0; i < numPeaks; ++i)
        nMax = juce::jmax (nMax, val[i]);

    int chosen = 0;
    for (int i = 0; i < numPeaks; ++i)
        if (val[i] >= 0.88f * nMax)
        {
            chosen = i;
            break;
        }

    // octave-up protection: a strong 2nd harmonic can make the half period look periodic. If
    // the previous period is still clearly more periodic, stay there.
    if (voiced && period > 0.0f)
    {
        const float ratio = (float) pos[chosen] / period;
        if (ratio > 0.46f && ratio < 0.54f)
            for (int i = chosen + 1; i < numPeaks; ++i)
                if (std::abs ((float) pos[i] / period - 1.0f) < 0.05f && val[i] > val[chosen] + 0.03f)
                {
                    chosen = i;
                    break;
                }
    }

    // parabolic interpolation of the peak
    const int p = pos[chosen];
    const float a = nsdf[(size_t) (p - 1)], b = nsdf[(size_t) p], c = nsdf[(size_t) (p + 1)];
    const float den = a - 2.0f * b + c;
    float delta = std::abs (den) > 1.0e-9f ? 0.5f * (a - c) / den : 0.0f;
    delta = juce::jlimit (-0.5f, 0.5f, delta);
    clarityOut = juce::jlimit (0.0f, 1.0f, b - 0.25f * (a - c) * delta);
    return (float) p + delta;
}
}
