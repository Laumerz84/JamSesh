#include "SynthInternal.h"
#include <cstdio>
#include <algorithm>

// Chord tracker: hears up to 6 simultaneous notes in one mixed signal (a strummed guitar, a
// keyboard, an organ).
//
// Spectrum. Every ~5.3 ms the newest 85 ms of the input (Bass / Full range 100 ms, High 43 ms),
// decimated to ~12 kHz, is Hann-windowed, zero-padded to 2048 points and transformed: long enough
// to separate two low notes a major third apart. Right after an attack only the audio since the
// attack (from 8 ms before the detected onset) is analysed, so a new strike is never mixed with
// what rang before. Peaks are picked on the raw magnitude (log-parabolic interpolation) and
// weighted by a whitening gain sigma^(nu - 1), nu = 0.33, over triangular bands reaching two ERBs
// to either side, so a loud low string can't drown a quiet high one.
//
// Notes, one at a time (iterative harmonic-sum estimation after Klapuri, 2006):
//   salience(f0) = sum over every harmonic m up to 3.4 kHz of w(f0, m) x the strongest residual
//                  peak within tolerance of m f0,   w(f0, m) = (f0 + 27 Hz) / (m f0 + 320 Hz)
// The winner (quarter-semitone grid, refined to 1/40 semitone; notes playing now get a 15 % edge
// so close calls don't swap) gives way to a candidate an octave, twelfth or two octaves below
// with its own clear fundamental and >= 85 % of its salience (an open E chord sits entirely on
// E2's harmonics). A note needs a peak at its fundamental (no common sub-octave of a fifth) and a
// second low partial (a lone peak is as often noise as a note), and its own level (first 8
// partials) must reach the gate (mains hum is a perfect harmonic series). Its pitch is the median of
// f_m / m over its clear low partials. Its partials
// are then removed from the residual, each only up to 2 x the mean of its neighbours in the
// original spectrum, so a partial shared with another note keeps the excess that belongs to that
// note (in C-E-G the G is also C's 3rd harmonic, and E its 5th). The search repeats until the next
// salience falls under (0.40 - 0.28 x sensitivity) of the first note's. A note on a harmonic of a
// lower note found in the same frame must be 2.2 x stronger and carry >= 50 % of its salience
// above its fundamental (a lone strong partial of a string is not a note).
//
// Tracking. A note starts playing after 2 agreeing frames (4 if it sits on a playing note's 2nd to
// 6th harmonic), keeps playing while heard, and stops 60 ms after it was last heard (at once below
// the gate - 3 dB). After an attack every note is held until 32 ms of the strike are in (or up to
// 52 ms while strong low energy is unexplained: two low notes a fifth apart blur until then); that
// frame decides the chord: its notes start at once, playing notes heard again >= 3 dB louder
// retrigger, the others stop. A note younger than 150 ms that a strong lower note explains as one
// of its harmonics (a ghost born in an attack) stops within 15 ms.
//
// CLOD_CHORD_DEBUG=1 in the environment prints every frame to stderr (for the test host).
// Cost at 48 kHz: one 2048-point FFT and ~0.1-0.3 M simple operations per hop, about 1 % of a core.

namespace synth
{
namespace
{
    constexpr int fftOrder = 11;
    constexpr int fftSize = 1 << fftOrder;
    constexpr int numBins = fftSize / 2 + 1;
    constexpr int maxWindow = 1280;
    constexpr int ringLen = 2048;                 // decimated history (power of two, > maxWindow)
    constexpr float topHz = 3400.0f;              // analysis band (the decimation filter passes ~3.6 kHz)
    constexpr float coarseStep = 0.25f;           // semitones between salience candidates
    constexpr int maxHarm = 64;                   // every candidate sums its harmonics up to the band top
    constexpr float nu = 0.33f;

    inline float weight (float f0, int m) noexcept { return (f0 + 27.0f) / ((float) m * f0 + 320.0f); }
    inline float erbRate (float hz) noexcept       { return 21.4f * std::log10 (1.0f + 0.00437f * hz); }
    inline float erbToHz (float e) noexcept        { return (std::pow (10.0f, e / 21.4f) - 1.0f) / 0.00437f; }

    // per tracker range (Bass, Guitar/Voice, High, Full): lowest and highest note (a note needs >= 3
    // harmonics in the band), the steady window (long: separates low notes a third apart), and when an
    // attack's chord is decided (soonest / latest, while low energy is still unexplained)
    constexpr float chordLo[4]   = { 32.0f, 55.0f, 150.0f, 32.0f };
    constexpr float chordHi[4]   = { 450.0f, 1100.0f, 1700.0f, 1700.0f };
    constexpr float windowMs[4]  = { 100.0f, 85.0f, 43.0f, 100.0f };
    constexpr float decideMs[4]  = { 45.0f, 32.0f, 20.0f, 40.0f };
    constexpr float decideMaxMs[4] = { 70.0f, 52.0f, 32.0f, 60.0f };
}

void ChordTracker::prepare (double sampleRate)
{
    sr = sampleRate;
    decim = juce::jmax (1, (int) std::round (sr / 12000.0));
    srD = sr / decim;
    hop = juce::jmax (16, (int) std::round (srD * 0.0053));

    const double fc = juce::jmin (3600.0, 0.3 * srD);
    decimLp[0].lowPass (sr, fc, 0.5412);
    decimLp[1].lowPass (sr, fc, 1.3066);
    dcR = (float) (1.0 - juce::MathConstants<double>::twoPi * 20.0 / sr);

    if (fft == nullptr)
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);
    ring.assign ((size_t) ringLen, 0.0f);
    window.assign ((size_t) maxWindow, 0.0f);
    attackWindow.assign ((size_t) maxWindow, 0.0f);
    fftBuf.assign ((size_t) (2 * fftSize), 0.0f);
    mag.assign ((size_t) numBins, 0.0f);
    white.assign ((size_t) numBins, 0.0f);
    bandGain.assign ((size_t) maxBands, 1.0f);

    binHz = (float) (srD / fftSize);
    topBin = juce::jmin (numBins - 2, (int) (juce::jmin ((double) topHz, 0.42 * srD) / binHz));

    // whitening bands: one per ERB from 50 Hz to the top of the analysis band
    numBands = 0;
    const float e1 = erbRate ((float) topBin * binHz);
    for (float e = erbRate (50.0f); e <= e1 + 0.01f && numBands < maxBands; e += 1.0f)
        bandCentre[numBands++] = erbToHz (e) / binHz;

    debug = juce::SystemStats::getEnvironmentVariable ("CLOD_CHORD_DEBUG", {}).isNotEmpty();
    builtWinLen = 0;
    numCands = 0;
    const int range = rangeIndex;
    rangeIndex = -1;
    setRange (range);
    reset();
}

void ChordTracker::setRange (int index) noexcept
{
    index = juce::jlimit (0, 3, index);
    if (index == rangeIndex && numCands > 0)
        return;
    rangeIndex = index;
    fLo = chordLo[index];
    fHi = chordHi[index];
    winLen = juce::jlimit (256, maxWindow, (int) std::round (windowMs[index] * 0.001 * srD));
    decideLen = juce::jmin (winLen, (int) std::round (decideMs[index] * 0.001 * srD));
    decideMaxLen = juce::jmin (winLen, (int) std::round (decideMaxMs[index] * 0.001 * srD));
    if (winLen != builtWinLen)
        buildWindow();

    numCands = 0;
    const float m1 = hzToMidi (fHi);
    for (float m = hzToMidi (fLo); m <= m1 && numCands < maxCands; m += coarseStep)
    {
        candMidi[(size_t) numCands] = m;
        candHz[(size_t) numCands++] = midiToHz (m);
    }
}

void ChordTracker::buildWindow() noexcept
{
    double sum = 0.0;
    for (int i = 0; i < winLen; ++i)
    {
        const float w = 0.5f - 0.5f * std::cos (twoPi * ((float) i + 0.5f) / (float) winLen);
        window[(size_t) i] = w;
        sum += w;
    }
    ampNorm = (float) (2.0 / juce::jmax (1.0, sum));   // |X| x ampNorm = amplitude of a sinusoid
    builtWinLen = winLen;
}

void ChordTracker::reset() noexcept
{
    for (auto& b : decimLp)
        b.z1 = b.z2 = 0.0f;
    dcX1 = dcY1 = 0.0f;
    std::fill (ring.begin(), ring.end(), 0.0f);
    ringPos = 0;
    decimPhase = 0;
    hopCounter = 0;
    clearNotes();
}

void ChordTracker::clearNotes() noexcept
{
    for (auto& s : slots)
        s = Note();
    numPeaks = 0;
    sinceOnset = ringLen;
    decisionPending = false;
}

void ChordTracker::setEnabled (bool on) noexcept
{
    if (on == enabled)
        return;
    enabled = on;
    if (! on)
        clearNotes();
}

void ChordTracker::onset() noexcept
{
    sinceOnset = 0;
    onsetFrame = frames;
    decisionPending = true;
    for (auto& s : slots)
        s.levelAtOnset = s.active ? s.level : 0.0f;
}

void ChordTracker::process (const float* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float s = x[i];
        float y = s - dcX1 + dcR * dcY1;
        dcX1 = s;
        dcY1 = y;
        y = decimLp[1].process (decimLp[0].process (y));

        if (++decimPhase >= decim)
        {
            decimPhase = 0;
            ring[(size_t) ringPos] = y;
            ringPos = (ringPos + 1) & (ringLen - 1);
            if (sinceOnset < ringLen)
                ++sinceOnset;
            if (++hopCounter >= hop)
            {
                hopCounter = 0;
                if (enabled)
                    analyse();
            }
        }
    }

    // denormal guards for silent input
    for (auto& b : decimLp)
    {
        if (std::abs (b.z1) < 1.0e-15f) b.z1 = 0.0f;
        if (std::abs (b.z2) < 1.0e-15f) b.z2 = 0.0f;
    }
    if (std::abs (dcY1) < 1.0e-15f) dcY1 = 0.0f;
}

// the residual peak that can be this candidate's fundamental: within 0.8 % + 1.5 Hz, or within
// 0.6 of a frame bin (two low notes a third apart pull each other's peak by a few Hz)
int ChordTracker::fundamentalPeak (float hz) const noexcept
{
    const float tol = juce::jmax (0.008f * hz + 1.5f, 0.6f * (float) (srD / frameLen));
    return strongestPeak (hz - tol, hz + tol);
}

int ChordTracker::strongestPeak (float lo, float hi) const noexcept
{
    int a = 0, b = numPeaks;
    while (a < b)
    {
        const int mid = (a + b) / 2;
        if (peaks[(size_t) mid].hz < lo)
            a = mid + 1;
        else
            b = mid;
    }
    int best = -1;
    float bestA = 0.0f;
    for (int i = a; i < numPeaks && peaks[(size_t) i].hz <= hi; ++i)
        if (resid[(size_t) i] > bestA)
        {
            bestA = resid[(size_t) i];
            best = i;
        }
    return best;
}

// Harmonic sum over the residual peaks. Coarse candidates (a quarter semitone apart) use a wider
// tolerance than the fine refinement; higher harmonics get extra room for string inharmonicity.
float ChordTracker::salience (float f0, bool fine, int* match) const noexcept
{
    const int M = juce::jmin (maxHarm, (int) ((float) topBin * binHz / f0));
    const float rel = fine ? 0.003f : 0.0075f;
    const float minTol = 0.6f * (float) (srD / frameLen);   // two close low notes pull each other's peak a little
    float s = 0.0f;
    for (int m = 1; m <= maxHarm; ++m)
    {
        int i = -1;
        if (m <= M)
        {
            const float fm = (float) m * f0;
            // never wider than 15 % of f0: a low note's upper harmonics are so close together that a wider
            // window would claim any noise peak as one of them
            const float tol = juce::jmin (0.15f * f0, juce::jmax (minTol, fm * (rel + juce::jmin (0.025f, 0.00005f * (float) (m * m)))));
            i = strongestPeak (fm - tol, fm + tol);
            if (i >= 0)
                s += weight (f0, m) * resid[(size_t) i];
        }
        if (match != nullptr)
            match[m - 1] = i;
    }
    return s;
}

void ChordTracker::analyse() noexcept
{
    ++frames;

    // ---- the frame: the newest window, or right after an attack only what came after it (from 8 ms
    //      before the detected onset), so the new strike is never mixed with what rang before. Until
    //      that is half a window long every note is held as it is; then the chord is decided. ----
    int W = winLen;
    float norm = ampNorm;
    const float* win = window.data();
    bool decision = false;
    const int attackLen = sinceOnset + (int) (0.008 * srD);
    if (decisionPending || attackLen < winLen)
    {
        if (attackLen < decideLen)
            return;
        W = juce::jmin (winLen, attackLen);
        decision = decisionPending;
        decisionPending = false;
        if (W < winLen)
        {
            double sum = 0.0;
            for (int i = 0; i < W; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos (twoPi * ((float) i + 0.5f) / (float) W);
                attackWindow[(size_t) i] = w;
                sum += w;
            }
            norm = (float) (2.0 / juce::jmax (1.0, sum));
            win = attackWindow.data();
        }
    }
    frameLen = W;

    double mean = 0.0;
    for (int k = 0; k < W; ++k)
        mean += ring[(size_t) ((ringPos - W + k) & (ringLen - 1))];
    mean /= W;
    std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
    double energy = 0.0;
    for (int k = 0; k < W; ++k)
    {
        const float v = ring[(size_t) ((ringPos - W + k) & (ringLen - 1))] - (float) mean;
        energy += (double) v * v;
        fftBuf[(size_t) k] = v * win[k];
    }
    const float rmsDb = juce::Decibels::gainToDecibels ((float) std::sqrt (energy / W), -120.0f);
    if (rmsDb < gateDb)
    {
        trackNotes (nullptr, 0, rmsDb < gateDb - 3.0f, decision);
        return;
    }

    fft->performFrequencyOnlyForwardTransform (fftBuf.data(), true);
    for (int k = 0; k <= topBin + 1; ++k)
        mag[(size_t) k] = fftBuf[(size_t) k] * norm;

    // ---- whitening: triangular bands reaching two ERBs to either side (so even a low note's widely
    //      spaced harmonics always share a band), gain sigma^(nu - 1), interpolated between centres ----
    float sigMax = 0.0f;
    for (int b = 0; b < numBands; ++b)
    {
        const float c = bandCentre[b];
        const float lo = b >= 2 ? bandCentre[b - 2] : juce::jmax (1.0f, c - 2.0f * (bandCentre[juce::jmin (b + 1, numBands - 1)] - c));
        const float hi = b + 2 < numBands ? bandCentre[b + 2] : juce::jmin ((float) topBin, c + 2.0f * (c - bandCentre[juce::jmax (b - 1, 0)]));
        double e = 0.0, wsum = 0.0;
        for (int k = juce::jmax (1, (int) std::ceil (lo)); k <= juce::jmin (topBin, (int) hi); ++k)
        {
            const float t = (float) k < c ? ((float) k - lo) / juce::jmax (1.0e-3f, c - lo)
                                          : (hi - (float) k) / juce::jmax (1.0e-3f, hi - c);
            if (t <= 0.0f)
                continue;
            e += (double) t * mag[(size_t) k] * mag[(size_t) k];
            wsum += t;
        }
        bandGain[(size_t) b] = wsum > 0.0 ? (float) std::sqrt (e / wsum) : 0.0f;   // sigma for now
        sigMax = juce::jmax (sigMax, bandGain[(size_t) b]);
    }
    if (sigMax <= 1.0e-9f)
    {
        trackNotes (nullptr, 0, false, decision);
        return;
    }
    const float sigFloor = sigMax * 0.01f;
    for (int b = 0; b < numBands; ++b)
        bandGain[(size_t) b] = std::pow (juce::jmax (bandGain[(size_t) b], sigFloor), nu - 1.0f);

    int band = 0;
    for (int k = 0; k <= topBin + 1; ++k)
    {
        const float fk = (float) k;
        while (band + 1 < numBands && bandCentre[band + 1] <= fk)
            ++band;
        float g;
        if (fk <= bandCentre[0])
            g = bandGain[0];
        else if (band + 1 >= numBands)
            g = bandGain[(size_t) (numBands - 1)];
        else
        {
            const float t = (fk - bandCentre[band]) / (bandCentre[band + 1] - bandCentre[band]);
            g = bandGain[(size_t) band] + t * (bandGain[(size_t) (band + 1)] - bandGain[(size_t) band]);
        }
        white[(size_t) k] = g;   // the whitening gain of bin k
    }

    // ---- spectral peaks of the raw spectrum (log-parabolic interpolation: the whitening slope must
    //      not shift them), kept down to 30 dB under the strongest whitened peak ----
    const int kLo = juce::jmax (2, (int) (0.85f * fLo / binHz));
    float wMax = 0.0f;
    for (int k = kLo; k <= topBin; ++k)
        wMax = juce::jmax (wMax, mag[(size_t) k] * white[(size_t) k]);
    const float peakFloor = wMax * 0.0316f;
    numPeaks = 0;
    for (int k = kLo; k < topBin && numPeaks < maxPeaks; ++k)
    {
        const float c = mag[(size_t) k];
        if (c <= mag[(size_t) (k - 1)] || c < mag[(size_t) (k + 1)] || c * white[(size_t) k] <= peakFloor)
            continue;
        const float a = std::log (mag[(size_t) (k - 1)] + 1.0e-12f), b = std::log (c);
        const float g = std::log (mag[(size_t) (k + 1)] + 1.0e-12f);
        const float den = a - 2.0f * b + g;
        const float p = den < 0.0f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - g) / den) : 0.0f;
        const float gain = white[(size_t) k] + p * (white[(size_t) (p >= 0.0f ? k + 1 : k - 1)] - white[(size_t) k]) * (p >= 0.0f ? 1.0f : -1.0f);
        auto& pk = peaks[(size_t) numPeaks];
        pk.hz = ((float) k + p) * binHz;
        pk.raw = std::exp (b - 0.25f * (a - g) * p);
        pk.amp = pk.raw * gain;
        resid[(size_t) numPeaks] = pk.amp;
        residRaw[(size_t) numPeaks] = pk.raw;
        ++numPeaks;
    }
    if (numPeaks == 0)
    {
        trackNotes (nullptr, 0, false, decision);
        return;
    }

    // ---- notes, strongest first: estimate, check, cancel, repeat ----
    const float detectRel = 0.40f - 0.28f * sens;
    const float keepRel = 0.6f * detectRel;
    FrameNote found[maxChordNotes + 4];
    float taken[maxChordNotes + 10];
    int numFound = 0, numTaken = 0, rejects = 0;
    float s1 = 0.0f, maxLevel = 0.0f;
    int match[maxHarm];
    float a[maxHarm + 2];

    float foundHz[maxChordNotes + 4] {};
    float orig[maxHarm + 2];

    // notes playing now get a small edge, so close calls (an octave pair, a note at the threshold)
    // don't swap from frame to frame
    float playing[maxSlots];
    int numPlaying = 0;
    for (auto& sl : slots)
        if (sl.id != 0 && sl.active)
            playing[numPlaying++] = sl.pitch;
    auto continuity = [&] (float midi)
    {
        for (int p = 0; p < numPlaying; ++p)
            if (std::abs (midi - playing[p]) < 0.5f)
                return 1.15f;
        return 1.0f;
    };

    while (numFound < maxNotes + 4 && rejects < 4 && numTaken < maxChordNotes + 10)
    {
        // coarse search; a candidate needs a residual peak at its fundamental (also prunes the search)
        int best = -1;
        float bestS = 0.0f;
        for (int c = 0; c < numCands; ++c)
        {
            bool skip = false;
            for (int t = 0; t < numTaken && ! skip; ++t)
                skip = std::abs (candMidi[(size_t) c] - taken[t]) < 0.6f;
            const float hz = candHz[(size_t) c];
            if (skip || fundamentalPeak (hz) < 0)
                continue;
            const float s = salience (hz, false, nullptr) * continuity (candMidi[(size_t) c]);
            if (s > bestS)
            {
                bestS = s;
                best = c;
            }
        }
        if (best < 0)
            break;

        // prefer the lowest plausible fundamental: when the winner's partials are all harmonics of a
        // note an octave, a twelfth or two octaves below that has its own clear fundamental and almost
        // the same salience (an open E chord sits entirely on E2's harmonics), take the lower note
        for (int k = 4; k >= 2; --k)
        {
            const int c = best - (int) std::round (12.0f * std::log2 ((float) k) / coarseStep);
            if (c < 0)
                continue;
            bool skip = false;
            for (int t = 0; t < numTaken && ! skip; ++t)
                skip = std::abs (candMidi[(size_t) c] - taken[t]) < 0.6f;
            const float hz = candHz[(size_t) c];
            const int fund = skip ? -1 : fundamentalPeak (hz);
            const float sLow = fund >= 0 ? salience (hz, false, nullptr) * continuity (candMidi[(size_t) c]) : 0.0f;
            if (fund >= 0 && resid[(size_t) fund] >= 0.2f * wMax && sLow >= 0.85f * bestS)
            {
                best = c;
                bestS = sLow;
                break;
            }
        }

        float f0 = candHz[(size_t) best], s = -1.0f;
        for (int d = -10; d <= 10; ++d)
        {
            const float hz = candHz[(size_t) best] * std::exp2 ((float) d * (0.025f / 12.0f));
            const float sv = salience (hz, true, nullptr);
            if (sv > s)
            {
                s = sv;
                f0 = hz;
            }
        }
        if (s <= 0.0f || (s1 > 0.0f && s < keepRel * s1))
            break;

        salience (f0, true, match);
        const int M = juce::jmin (maxHarm, (int) ((float) topBin * binHz / f0));
        float strongest = 0.0f;
        int lowMatched = 0;
        for (int m = 1; m <= M; ++m)
        {
            const int i = match[m - 1];
            a[m] = i >= 0 ? resid[(size_t) i] : 0.0f;
            orig[m] = i >= 0 ? peaks[(size_t) i].amp : 0.0f;
            strongest = juce::jmax (strongest, a[m]);
            if (m <= 6 && i >= 0)
                ++lowMatched;
        }
        a[0] = orig[0] = 0.0f;
        a[M + 1] = orig[M + 1] = 0.0f;

        // a note needs its fundamental (this stops the common sub-octave of a fifth from passing
        // as a note) and a second low partial: a lone strong peak is noise (a pick scrape, a squeak)
        // as often as it is a pure tone, and pure tones are for Mono tracking
        const bool hasFund = a[1] > 0.0f && a[1] >= 0.12f * strongest;
        if (! hasFund || lowMatched < 2)
        {
            taken[numTaken++] = hzToMidi (f0);
            ++rejects;
            continue;
        }

        // pitch: the median of f_m / m over the clear low partials (one merged or shared peak can't pull
        // it), refined by averaging the estimates that agree with it within 0.4 %
        {
            float est[8], wts[8];
            int ne = 0;
            for (int m = 1; m <= juce::jmin (M, 8); ++m)
                if (match[m - 1] >= 0 && a[m] >= 0.2f * strongest)
                {
                    est[ne] = peaks[(size_t) match[m - 1]].hz / (float) m;
                    wts[ne++] = a[m] * weight (f0, m);
                }
            if (ne > 0)
            {
                float sorted[8];
                std::copy (est, est + ne, sorted);
                std::sort (sorted, sorted + ne);
                const float med = sorted[ne / 2];
                double num = 0.0, den = 0.0;
                for (int e = 0; e < ne; ++e)
                    if (std::abs (est[e] / med - 1.0f) < 0.004f)
                    {
                        num += (double) wts[e] * est[e];
                        den += wts[e];
                    }
                f0 = den > 0.0 ? (float) (num / den) : med;
            }
        }

        // how much of the salience comes from partials above the fundamental (a lone strong partial
        // of a lower note has almost none of its own)
        float upper = 0.0f;
        for (int m = 2; m <= M; ++m)
            upper += weight (f0, m) * a[m];
        upper /= juce::jmax (1.0e-9f, upper + weight (f0, 1) * a[1]);

        // cancel: each partial up to 2 x the mean of its neighbours in the original spectrum (the
        // note's own envelope); the excess of a partial shared with another note stays for that note
        double rawSq = 0.0;
        for (int m = 1; m <= M; ++m)
        {
            const int i = match[m - 1];
            if (i < 0 || a[m] <= 0.0f)
                continue;
            float take = a[m];
            if (m >= 2)
            {
                const int present = (orig[m - 1] > 0.0f ? 1 : 0) + (orig[m + 1] > 0.0f ? 1 : 0);
                if (present > 0)
                    take = juce::jmin (a[m], 2.0f * (orig[m - 1] + orig[m + 1]) / (float) present);
            }
            const float rawTake = residRaw[(size_t) i] * (take / a[m]);
            if (m <= 8)   // the level counts the low partials only (noise near high harmonics is not the note)
                rawSq += (double) rawTake * rawTake;
            resid[(size_t) i] = juce::jmax (0.0f, resid[(size_t) i] - take);
            residRaw[(size_t) i] = juce::jmax (0.0f, residRaw[(size_t) i] - rawTake);
        }

        if (s1 <= 0.0f)
            s1 = s;
        foundHz[numFound] = f0;
        auto& fn = found[numFound++];
        fn.pitch = hzToMidi (f0);
        fn.salience = s;
        fn.level = (float) std::sqrt (0.5 * rawSq);
        fn.upper = upper;
        fn.onHarmonic = false;
        fn.strong = true;
        maxLevel = juce::jmax (maxLevel, fn.level);
        taken[numTaken++] = fn.pitch;
    }

    // a note on a harmonic (x2 .. x16) of a lower note found here needs 2.2 x the threshold and real
    // partials of its own (>= 50 % of its salience above its fundamental) to count as new, and can't
    // keep a playing note alive on its own: cancellation leftovers live there (a partial shared by
    // several notes of a chord - F#5 in open E is E2 x9, B2 x6 and B3 x3 - or one strongly excited
    // partial of a plucked string)
    for (int i = 0; i < numFound; ++i)
    {
        for (int j = 0; j < numFound && ! found[i].onHarmonic; ++j)
        {
            if (j == i || foundHz[j] >= foundHz[i] || found[j].salience < 0.35f * found[i].salience)
                continue;
            const float ratio = foundHz[i] / foundHz[j], k = std::round (ratio);
            found[i].onHarmonic = k >= 2.0f && k <= 16.0f && std::abs (ratio / k - 1.0f) < 0.023f;
        }
        found[i].strong = found[i].onHarmonic ? found[i].salience >= 2.2f * detectRel * s1 && found[i].upper >= 0.5f
                                              : found[i].salience >= detectRel * s1;
    }

    // an attack's decision waits (up to 0.8 of a window) while strong low energy is still unexplained:
    // two low notes a fifth apart (E2-B2) blur into one peak until ~50 ms of the strike are in
    if (decision && W < decideMaxLen)
    {
        float lowest = 1.0e9f;
        for (int i = 0; i < numFound; ++i)
            if (found[i].strong)
                lowest = juce::jmin (lowest, foundHz[i]);
        for (int i = 0; i < numPeaks && peaks[(size_t) i].hz < 0.95f * lowest; ++i)
            if (resid[(size_t) i] >= 0.3f * wMax)
            {
                decisionPending = true;
                if (debug)
                    std::fprintf (stderr, "[chord %d] len %d: unexplained %.0f Hz, decision waits\n", frames, W, peaks[(size_t) i].hz);
                return;
            }
    }

    if (debug)
    {
        std::fprintf (stderr, "[chord %d]%s len %d rms %.1f dB peaks %d:", frames, decision ? " DECIDE" : "", W, rmsDb, numPeaks);
        for (int i = 0; i < numFound; ++i)
            std::fprintf (stderr, "  %.2f s%.2f %.0fdB%s%s", found[i].pitch, found[i].salience / juce::jmax (1.0e-9f, s1),
                          juce::Decibels::gainToDecibels (found[i].level), found[i].strong ? "" : "w", found[i].onHarmonic ? "h" : "");
        std::fprintf (stderr, "\n");
    }

    // drop notes under the gate on their own (mains hum is a perfect harmonic series that a loud noise
    // lifts past the gate with it; a peak or two of a scrape is far quieter than a played note) or 30 dB
    // under the loudest note of the frame
    const float minLevel = juce::jmax (juce::Decibels::decibelsToGain (gateDb), maxLevel * 0.0316f);
    int kept = 0;
    for (int i = 0; i < numFound; ++i)
        if (found[i].level >= minLevel)
            found[kept++] = found[i];
    trackNotes (found, kept, false, decision);
}

bool ChordTracker::harmonicOfActive (float pitch) const noexcept
{
    static constexpr float steps[] = { 12.0f, 19.02f, 24.0f, 27.86f, 31.02f };   // harmonics 2..6
    for (auto& s : slots)
        if (s.id != 0 && s.active)
            for (float st : steps)
                if (std::abs (std::abs (pitch - s.pitch) - st) < 0.5f)
                    return true;
    return false;
}

void ChordTracker::trackNotes (const FrameNote* notes, int count, bool silent, bool decision) noexcept
{
    for (auto& s : slots)
        s.matched = false;
    const int releaseFrames = juce::jmax (2, (int) std::round (0.060 * srD / hop));
    const int fast = juce::jmax (1, (int) std::round (0.015 * srD / hop));

    // frame notes (strongest first) continue the nearest slot within 0.6 semitone; strong ones
    // that continue nothing start a candidate; weak ones on another note's harmonic are leftovers
    for (int i = 0; i < count; ++i)
    {
        const auto& fn = notes[i];
        if (! fn.strong && fn.onHarmonic)
            continue;
        Note* best = nullptr;
        float bestD = 0.6f;
        for (auto& s : slots)
            if (s.id != 0 && ! s.matched && std::abs (s.pitch - fn.pitch) < bestD)
            {
                bestD = std::abs (s.pitch - fn.pitch);
                best = &s;
            }
        if (best != nullptr)
        {
            auto& s = *best;
            const float d = fn.pitch - s.pitch;
            s.pitch += std::abs (d) > 0.3f ? d : 0.3f * d;   // follows bends, smooths jitter
            s.level += (fn.level - s.level) * 0.5f;
            s.salience = fn.salience;
            s.matched = true;
            if (fn.strong || s.active)
            {
                ++s.hits;
                s.misses = 0;
            }
            continue;
        }
        if (! fn.strong)
            continue;
        for (auto& s : slots)
            if (s.id == 0)
            {
                s = Note();
                s.id = nextId++;
                if (nextId == 0)
                    nextId = 1;
                s.pitch = fn.pitch;
                s.level = fn.level;
                s.salience = fn.salience;
                s.hits = 1;
                s.matched = true;
                break;
            }
    }

    // notes not heard in this frame: they stop after 60 ms; at once when the input went silent or an
    // attack's clean frame no longer has them; within 15 ms when a note younger than 150 ms is now
    // explained by a strong lower note as one of its harmonics (a ghost born in an attack)
    for (auto& s : slots)
    {
        if (s.id == 0 || s.matched)
            continue;
        if (s.active && frames - s.activatedAt < (int) (0.15 * srD / hop))   // young notes: attack ghosts
            for (int i = 0; i < count; ++i)
            {
                if (! notes[i].strong || notes[i].onHarmonic || notes[i].pitch > s.pitch - 11.5f)
                    continue;
                const float ratio = std::exp2 ((s.pitch - notes[i].pitch) / 12.0f), k = std::round (ratio);
                if (k >= 2.0f && k <= 12.0f && std::abs (ratio / k - 1.0f) < 0.023f)
                {
                    s.misses = juce::jmax (s.misses, releaseFrames - fast - 1);
                    break;
                }
            }
        s.misses = silent || (decision && s.active && s.activatedAt <= onsetFrame) ? releaseFrames : s.misses + 1;
        s.level *= 0.85f;
        if ((! s.active && s.misses >= 2) || s.misses >= releaseFrames)
        {
            if (debug && s.active)
                std::fprintf (stderr, "   - %.2f (id %u)\n", s.pitch, s.id);
            s = Note();
        }
    }

    // an attack's clean frame: notes that came back louder were struck again (retrigger)
    if (decision)
        for (auto& s : slots)
            if (s.id != 0 && s.active && s.matched && s.activatedAt <= onsetFrame)
                s.retrigger = s.levelAtOnset <= 0.0f || s.level > 1.41f * s.levelAtOnset;

    // too many playing notes (the Notes control was lowered): the weakest stop
    int numActive = 0;
    for (auto& s : slots)
        numActive += s.active ? 1 : 0;
    while (numActive > maxNotes)
    {
        Note* weakest = nullptr;
        for (auto& s : slots)
            if (s.active && (weakest == nullptr || s.salience < weakest->salience))
                weakest = &s;
        *weakest = Note();
        --numActive;
    }

    // confirm candidates: at once in an attack's clean frame, else 2 agreeing frames (4 when the
    // note could be a playing note's harmonic)
    for (auto& s : slots)
    {
        if (s.id == 0 || s.active || numActive >= maxNotes)
            continue;
        if (s.hits >= (decision ? 1 : (harmonicOfActive (s.pitch) ? 4 : 2)))
        {
            s.active = true;
            s.activatedAt = frames;
            ++numActive;
            if (debug)
                std::fprintf (stderr, "   + %.2f (id %u)\n", s.pitch, s.id);
        }
    }
}
}
