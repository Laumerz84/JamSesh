#include "SynthInternal.h"

// One synth voice: an additive bank that plays the Wave Sculptor's partial set (up to 64
// partials x up to 7 unison copies), plus sub oscillator and noise, an anti-aliased warmth stage,
// a ZDF ladder / state-variable filter with its own envelope, the amp envelope and the per-note
// drive (distortion of this voice alone, before the voices are summed).
//
// Why additive: the partial set may change every block (sculptor macros are automatable) and may
// be inharmonic. Each partial is a complex rotator (4 mul + 2 add per sample, 4 partials per SIMD
// lane group); targets are recomputed every chunk (<= 32 samples) and amplitudes ramp linearly
// across the chunk, so set changes, pitch glides and partials crossing the band limit are all
// click-free. A partial is faded out between 0.40 and 0.45 x sample rate (max 18 / 20.5 kHz), so
// nothing is ever rendered above Nyquist at any pitch.
//
// Cost (48 kHz, 64 partials): ~0.25 % of a core per unison copy -> mono tracked voice with unison 3
// ~0.9 %, eight MIDI voices with unison 3 ~7 % (scalar fallback without SIMD roughly 3x that).

namespace synth
{
namespace
{
    inline float polyBlep (float t, float dt) noexcept
    {
        if (t < dt)
        {
            t /= dt;
            return t + t - t * t - 1.0f;
        }
        if (t > 1.0f - dt)
        {
            t = (t - 1.0f) / dt;
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }

    // ---- per-note drive curves (index = type) ----
    // 0 overdrive: asymmetric algebraic sigmoid s(x + b) - s(b), s(x) = x / sqrt(1 + x^2) (tube-like even harmonics)
    // 1 distortion: cubic soft clip with a hard knee at |x| = 1, slight bias
    // 2 fuzz: hard clip at -1 / +0.6 (strongly asymmetric, square-ish)
    constexpr float driveBias[3]   = { 0.20f, 0.06f, 0.0f };
    constexpr float driveSpanDb[3] = { 36.0f, 44.0f, 52.0f };    // gain -6 dB at 0 % .. +30 / +38 / +46 dB at 100 %
    constexpr float driveLowHz[3]  = { 140.0f, 100.0f, 50.0f };  // pre high-pass: keeps the lows tight
    constexpr float driveBump[3]   = { 1.0f, 0.6f, 0.2f };       // + bump x high-pass(720 Hz): mids into the clipper
    constexpr float driveS0[3]     = { 0.943f, 1.49f, 1.0f };    // small-signal slope of each curve
    constexpr float driveSat[3]    = { 0.95f, 0.92f, 0.78f };    // RMS of each curve fully saturated
    constexpr double fuzzHi = 0.6;

    inline double sigmoid (double x) noexcept { return x / std::sqrt (1.0 + x * x); }

    inline double driveCurve (double x, int type) noexcept
    {
        switch (type)
        {
            case 0:
                return sigmoid (x + driveBias[0]) - sigmoid (driveBias[0]);
            case 1:
            {
                const double b = driveBias[1], u = x + b;
                const double c = u >= 1.0 ? 1.0 : (u <= -1.0 ? -1.0 : 1.5 * u - 0.5 * u * u * u);
                return c - (1.5 * b - 0.5 * b * b * b);
            }
            default:
                return juce::jlimit (-1.0, fuzzHi, x);
        }
    }

    inline double driveIntegral (double x, int type) noexcept
    {
        switch (type)
        {
            case 0:
            {
                const double b = driveBias[0], u = x + b;
                return std::sqrt (1.0 + u * u) - sigmoid (b) * x;
            }
            case 1:
            {
                const double b = driveBias[1], u = x + b, au = std::abs (u);
                const double F = au >= 1.0 ? au - 0.375 : 0.75 * u * u - 0.125 * u * u * u * u;
                return F - (1.5 * b - 0.5 * b * b * b) * x;
            }
            default:
                if (x > fuzzHi)  return fuzzHi * x - 0.5 * fuzzHi * fuzzHi;
                if (x < -1.0)    return -x - 0.5;
                return 0.5 * x * x;
        }
    }
}

//==============================================================================
float NoteDrive::Shaper::process (float in, int type) noexcept
{
    const double x = in;
    const double F = driveIntegral (x, type);
    const double d = x - x1;
    const double y = std::abs (d) > 1.0e-6 ? (F - F1) / d : driveCurve (0.5 * (x + x1), type);
    x1 = x;
    F1 = F;
    return (float) y;
}

void NoteDrive::prepare (double sampleRate)
{
    sr = sampleRate;
    if (os == nullptr)
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                                                               false, false);
    os->initProcessing ((size_t) chunkSize);
    aDc = onePole (1.0f / (twoPi * 12.0f), sr);
    lastType = -1;
    reset();
}

void NoteDrive::reset() noexcept
{
    if (os != nullptr)
        os->reset();
    for (auto& c : ch)
    {
        c.lowZ = c.bumpZ = c.preLpZ = c.dcX = c.dcY = 0.0f;
        c.tone.z1 = c.tone.z2 = 0.0f;
        c.shaper.reset();
    }
    wetNow = 0.0f;
    toneNow = -1.0f;
}

void NoteDrive::process (float* L, float* R, int n, float amount, int type, float tone, float refLevel) noexcept
{
    amount = juce::jlimit (0.0f, 1.0f, amount);
    if ((amount <= 0.0f && wetNow <= 0.0f) || os == nullptr || n <= 0)
        return;
    type = juce::jlimit (0, 2, type);

    if (type != lastType)
    {
        aLow = onePole (1.0f / (twoPi * driveLowHz[type]), sr);
        aBump = onePole (1.0f / (twoPi * 720.0f), sr);
        aPreLp = onePole (1.0f / (twoPi * juce::jmin (7000.0f, 0.4f * (float) sr)), sr);
        if (lastType >= 0)
            for (auto& c : ch)
                c.shaper.reset();
        lastType = type;
    }

    // drive gain in dB, smoothed per chunk (one-pole, 20 ms) and ramped across it
    const float gainDb = -6.0f + amount * driveSpanDb[type];
    const float prevDb = wetNow > 0.0f ? gainNow : gainDb;
    gainNow = prevDb + (gainDb - prevDb) * onePole (0.02f, sr, n);
    const float g0 = juce::Decibels::decibelsToGain (prevDb), g1 = juce::Decibels::decibelsToGain (gainNow);

    // fade in over the first 5 % of the dial, so switching it on never clicks
    const float wetTarget = juce::jmin (1.0f, amount * 20.0f);
    const float wet0 = wetNow;
    wetNow = wetTarget > wetNow ? juce::jmin (wetTarget, wetNow + 0.1f) : juce::jmax (wetTarget, wetNow - 0.1f);

    tone = juce::jlimit (0.0f, 1.0f, tone);
    if (std::abs (tone - toneNow) > 1.0e-4f)
    {
        toneNow = toneNow < 0.0f ? tone : toneNow + (tone - toneNow) * 0.3f;
        const double fc = juce::jmin (0.45 * sr, 1200.0 * std::pow (10.0, (double) toneNow));
        for (auto& c : ch)
        {
            const float z1 = c.tone.z1, z2 = c.tone.z2;
            c.tone.lowPass (sr, fc, 0.7071);
            c.tone.z1 = z1;
            c.tone.z2 = z2;
        }
    }

    // level makeup: a voice at its nominal level keeps its loudness at any drive setting
    auto makeup = [type] (float g)
    {
        const float lin = driveS0[type] * g * 0.5f;
        return 0.5f * std::sqrt (1.0f + (lin / driveSat[type]) * (lin / driveSat[type])) / juce::jmax (1.0e-6f, lin);
    };
    const float invRef = 1.0f / juce::jmax (1.0e-6f, refLevel);
    const float m0 = makeup (g0) * refLevel, m1 = makeup (g1) * refLevel;

    alignas (16) float dry[2][chunkSize];
    alignas (16) float pre[2][chunkSize];
    float* io[2] = { L, R };
    const float invN = 1.0f / (float) n;
    for (int c = 0; c < 2; ++c)
    {
        auto& s = ch[c];
        for (int i = 0; i < n; ++i)
        {
            const float x = io[c][i];
            dry[c][i] = x;
            s.lowZ += (x - s.lowZ) * aLow;
            const float hp = x - s.lowZ;
            s.bumpZ += (hp - s.bumpZ) * aBump;
            const float shaped = hp + driveBump[type] * (hp - s.bumpZ);
            s.preLpZ += (shaped - s.preLpZ) * aPreLp;
            const float t = (float) (i + 1) * invN;
            pre[c][i] = s.preLpZ * (g0 + (g1 - g0) * t) * invRef;
        }
    }

    // 2x oversampled shaping
    float* preCh[2] = { pre[0], pre[1] };
    juce::dsp::AudioBlock<float> block (preCh, 2, (size_t) n);
    auto up = os->processSamplesUp (block);
    for (size_t c = 0; c < 2; ++c)
    {
        auto* d = up.getChannelPointer (c);
        auto& sh = ch[c].shaper;
        for (size_t i = 0; i < up.getNumSamples(); ++i)
            d[i] = sh.process (d[i], type);
    }
    os->processSamplesDown (block);

    for (int c = 0; c < 2; ++c)
    {
        auto& s = ch[c];
        for (int i = 0; i < n; ++i)
        {
            float y = pre[c][i];
            const float hp = y - s.dcX + (1.0f - aDc) * s.dcY;   // DC blocker (the curves are asymmetric)
            s.dcX = y;
            s.dcY = hp;
            y = s.tone.process (hp);
            const float t = (float) (i + 1) * invN;
            const float w = wet0 + (wetNow - wet0) * t;
            io[c][i] = dry[c][i] + (y * (m0 + (m1 - m0) * t) - dry[c][i]) * w;
        }
        if (std::abs (s.dcY) < 1.0e-15f) s.dcY = 0.0f;
        if (std::abs (s.tone.z1) < 1.0e-15f) s.tone.z1 = 0.0f;
        if (std::abs (s.tone.z2) < 1.0e-15f) s.tone.z2 = 0.0f;
    }
}

//==============================================================================
void SynthVoice::prepare (double sampleRate)
{
    sr = sampleRate;
    rng.s = (uint32_t) (reinterpret_cast<uintptr_t> (this) >> 4) | 1u;
    noteDrive.prepare (sr);
    reset();
}

void SynthVoice::reset() noexcept
{
    amp.kill();
    flt.kill();
    for (auto& b : banks)
    {
        std::fill (std::begin (b.re), std::end (b.re), 1.0f);
        std::fill (std::begin (b.im), std::end (b.im), 0.0f);
        std::fill (std::begin (b.c), std::end (b.c), 1.0f);
        std::fill (std::begin (b.s), std::end (b.s), 0.0f);
        std::fill (std::begin (b.a), std::end (b.a), 0.0f);
    }
    for (auto& c : clip) c.reset();
    for (auto& l : ladder) l.reset();
    for (auto& s : svf) s.reset();
    noteDrive.reset();
    note = -1;
    held = sustained = false;
    gainNow = 0.0f;
    subGainNow = noiseGainNow = 0.0f;
    noiseLp = 0.0f;
    subPhase = 0.0f;
    pllI = pllQ = pllInteg = pllDw = 0.0f;
}

void SynthVoice::enablePll (bool on) noexcept
{
    if (on != pll)
    {
        pll = on;
        pllI = pllQ = pllInteg = pllDw = 0.0f;
    }
}

void SynthVoice::initPhases (const sculpt::PartialSet& set, float phaseRand) noexcept
{
    const float spreadRad = juce::jlimit (0.0f, 1.0f, phaseRand) * juce::MathConstants<float>::pi;
    for (size_t u = 0; u < banks.size(); ++u)
    {
        auto& b = banks[u];
        // unison copies start at random points of the cycle (a time shift keeps each copy's wave
        // shape) so they don't sum coherently into a loud, phasey attack
        const float shift = u == 0 ? 0.0f : juce::MathConstants<float>::pi * (1.0f + rng.next());
        for (int k = 0; k < numPartials; ++k)
        {
            const float base = (k < set.count ? set.phase[(size_t) k] : 0.0f) + shift * (float) (k + 1);
            const float ph = std::fmod (base, twoPi) + spreadRad * rng.next();
            b.re[k] = std::cos (ph);
            b.im[k] = std::sin (ph);
            b.a[k] = 0.0f;
        }
    }
}

void SynthVoice::noteOn (float pitch, float vel, float glideFrom, const sculpt::PartialSet& set,
                         const VoiceParams& p) noexcept
{
    const bool fresh = amp.isIdle();
    targetPitch = pitch;
    if (fresh)
    {
        pitchNow = (p.glideMs > 0.0f && glideFrom > 0.0f) ? glideFrom : pitch;
        initPhases (set, p.phaseRand);
        for (auto& c : clip) c.reset();
        for (auto& l : ladder) l.reset();
        for (auto& s : svf) s.reset();
        noteDrive.reset();
        extGain = extGainTarget;
        gainNow = 0.0f;
        subPhase = 0.0f;
    }
    else if (p.glideMs <= 0.0f)
    {
        pitchNow = pitch;   // stolen / re-attacked voice: phases stay continuous, pitch jumps
    }
    velocity = vel;
    held = true;
    sustained = false;
    pllInteg = 0.0f;
    pllDw = 0.0f;
    amp.set (p.ampA, p.ampD, p.ampS, p.ampR, sr);
    flt.set (p.fltA, p.fltD, p.fltS, p.fltR, sr);
    amp.gateOn();
    flt.gateOn();
}

void SynthVoice::render (float* L, float* R, int n, const VoiceParams& p, const sculpt::PartialSet& set,
                         const float* pllInput) noexcept
{
    if (amp.isIdle() || n <= 0)
        return;

    amp.set (p.ampA, p.ampD, p.ampS, p.ampR, sr);
    flt.set (p.fltA, p.fltD, p.fltS, p.fltR, sr);

    // ---- pitch: glide (time = ~95 % of the way), bend, LFO ----
    const float glideTau = juce::jmax (0.5f, p.glideMs) * 0.001f / 3.0f;
    pitchNow += (targetPitch - pitchNow) * onePole (glideTau, sr, n);
    const float pitch = pitchNow + p.bendSemis + p.lfoPitchSemis;
    const float w0 = twoPi * midiToHz (pitch) / (float) sr;

    const int U = juce::jlimit (1, maxUnison, p.unison);
    const int centre = (U - 1) / 2;
    const bool canLock = pll && pllInput != nullptr && set.harmonic && set.count > 0;

    // ---- phase-locked loop: keeps the fundamental in phase with the input's (Overtones mode) ----
    if (canLock)
    {
        const auto& b = banks[(size_t) centre];
        const float wRef = (w0 + pllDw) * set.ratio[0];
        const float cr = std::cos (wRef), ci = std::sin (wRef);
        float re = b.re[0], im = b.im[0], I = 0.0f, Q = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float nr = re * cr - im * ci;
            im = re * ci + im * cr;
            re = nr;
            I += pllInput[i] * re;
            Q += pllInput[i] * im;
        }
        const float f0 = w0 * (float) sr / twoPi;
        const float lpA = onePole (1.0f / (twoPi * juce::jlimit (15.0f, 200.0f, f0 * 0.25f)), sr, n);
        pllI += (I / (float) n - pllI) * lpA;
        pllQ += (Q / (float) n - pllQ) * lpA;
        if (pllI * pllI + pllQ * pllQ > 1.0e-10f)
        {
            const float err = std::atan2 (pllI, pllQ);
            const float wn = twoPi * 5.0f;
            const float kp = 2.0f * 0.8f * wn / (float) sr;
            const float ki = wn * wn * (float) n / (float) (sr * sr);
            const float lim = 0.02f * w0;
            pllInteg = juce::jlimit (-lim, lim, pllInteg + ki * err);
            pllDw = juce::jlimit (-lim, lim, kp * err + pllInteg);
        }
    }
    else
    {
        pllDw = 0.0f;
        pllInteg = 0.0f;
    }

    const float wc = w0 + pllDw;
    hzNow = wc * (float) sr / twoPi;

    // ---- additive bank ----
    alignas (16) float tmpL[chunkSize] {}, tmpR[chunkSize] {};
    alignas (16) float wk[numPartials], tgt[numPartials];
    const float fadeLo = twoPi * (float) juce::jmin (0.40 * sr, 18000.0) / (float) sr;
    const float fadeHi = twoPi * (float) juce::jmin (0.45 * sr, 20500.0) / (float) sr;
    const float invFade = 1.0f / (fadeHi - fadeLo);
    const int count = juce::jlimit (0, numPartials, set.count);
    const float uniGain = 1.0f / std::sqrt ((float) U);
    const float invN = 1.0f / (float) n;

    for (int u = 0; u < U; ++u)
    {
        auto& b = banks[(size_t) u];
        const float pos = U > 1 ? 2.0f * (float) u / (float) (U - 1) - 1.0f : 0.0f;
        const float wu = wc * std::exp2 (p.detuneCents * pos * (1.0f / 1200.0f));
        const float panAngle = (juce::jlimit (-1.0f, 1.0f, p.spread * pos) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        const float gL = juce::MathConstants<float>::sqrt2 * std::cos (panAngle) * uniGain;
        const float gR = juce::MathConstants<float>::sqrt2 * std::sin (panAngle) * uniGain;

        int highest = 0;
        for (int k = 0; k < numPartials; ++k)
        {
            float w = 0.0f, a = 0.0f;
            if (k < count)
            {
                w = set.ratio[(size_t) k] * wu;
                a = set.amp[(size_t) k] * juce::jlimit (0.0f, 1.0f, (fadeHi - w) * invFade);
                if (p.overtonesOnly && set.ratio[(size_t) k] < 1.5f)
                    a = 0.0f;
            }
            wk[k] = juce::jlimit (0.0f, juce::MathConstants<float>::pi, w);
            tgt[k] = a;
            if (a > 0.0f || b.a[k] > 0.0f)
                highest = k;
        }
        const int groups = highest / 4 + 1;   // group 0 always runs: the PLL reads partial 0

        Vec acc[chunkSize];
        for (int i = 0; i < n; ++i)
            acc[i] = Vec::expand (0.0f);

        for (int g = 0; g < groups; ++g)
        {
            const int o = g * 4;
            Vec s, c;
            sinCos (Vec::fromRawArray (wk + o), s, c);
            Vec re = Vec::fromRawArray (b.re + o);
            Vec im = Vec::fromRawArray (b.im + o);
            Vec a  = Vec::fromRawArray (b.a + o);
            const Vec t = Vec::fromRawArray (tgt + o);
            const Vec da = (t - a) * Vec::expand (invN);
            for (int i = 0; i < n; ++i)
            {
                const Vec nr = re * c - im * s;
                im = re * s + im * c;
                re = nr;
                a = a + da;
                acc[i] = acc[i] + a * im;
            }
            // renormalise the rotators (the polynomial sin/cos are not exactly unit length)
            const Vec f = Vec::expand (1.5f) - Vec::expand (0.5f) * (re * re + im * im);
            (re * f).copyToRawArray (b.re + o);
            (im * f).copyToRawArray (b.im + o);
            t.copyToRawArray (b.a + o);
        }

        for (int i = 0; i < n; ++i)
        {
            const float m = acc[i].sum();
            tmpL[i] += m * gL;
            tmpR[i] += m * gR;
        }
    }

    // ---- sub oscillator + noise (centred) ----
    const float subTarget = p.overtonesOnly ? 0.0f : p.subLevel * 0.7f;
    const float noiseTarget = p.noiseLevel * 0.35f;
    if (subTarget > 0.0f || subGainNow > 0.0f || noiseTarget > 0.0f || noiseGainNow > 0.0f)
    {
        const float subInc = (hzNow / (p.subOct >= 2 ? 4.0f : 2.0f)) / (float) sr;
        const float dSub = (subTarget - subGainNow) * invN;
        const float dNoise = (noiseTarget - noiseGainNow) * invN;

        const float col = juce::jlimit (-1.0f, 1.0f, p.noiseColour);
        const bool dark = col < 0.0f;
        const float fc = dark ? 16000.0f * std::pow (150.0f / 16000.0f, -col) : 30.0f * std::pow (6000.0f / 30.0f, col);
        const float a = onePole (1.0f / (twoPi * juce::jmin (fc, 0.45f * (float) sr)), sr);
        const float comp = dark ? std::sqrt ((2.0f - a) / a)
                                : std::sqrt ((2.0f - a) / juce::jmax (1.0e-6f, 2.0f * (1.0f - a) * (1.0f - a)));

        for (int i = 0; i < n; ++i)
        {
            subGainNow += dSub;
            noiseGainNow += dNoise;
            float v = 0.0f;
            if (subGainNow > 0.0f)
            {
                subPhase += subInc;
                if (subPhase >= 1.0f)
                    subPhase -= 1.0f;
                float sv;
                if (p.subSquare)
                {
                    sv = subPhase < 0.5f ? 1.0f : -1.0f;
                    sv += polyBlep (subPhase, subInc);
                    float t2 = subPhase + 0.5f;
                    if (t2 >= 1.0f) t2 -= 1.0f;
                    sv -= polyBlep (t2, subInc);
                    sv *= 0.7f;   // equal loudness with the sine
                }
                else
                {
                    sv = std::sin (twoPi * subPhase);
                }
                v += sv * subGainNow;
            }
            if (noiseGainNow > 0.0f)
            {
                const float x = rng.next();
                noiseLp += (x - noiseLp) * a;
                v += (dark ? noiseLp : x - noiseLp) * comp * noiseGainNow;
            }
            tmpL[i] += v;
            tmpR[i] += v;
        }
        subGainNow = subTarget;
        noiseGainNow = noiseTarget;
    }

    // ---- warmth: anti-aliased soft clip, unity small-signal gain up to g = 2/3, peak 1 above ----
    {
        const float wa = juce::jlimit (0.0f, 1.0f, p.warmth);
        const float g = 0.05f + 2.45f * wa * std::sqrt (wa);
        const float norm = 1.0f / juce::jmin (1.5f * g, 1.0f);
        for (int i = 0; i < n; ++i)
        {
            tmpL[i] = clip[0].process (g * tmpL[i]) * norm;
            tmpR[i] = clip[1].process (g * tmpR[i]) * norm;
        }
    }

    // ---- filter (coefficients every 8 samples) + amp ----
    const int type = juce::jlimit (0, 4, p.filterType);
    if (type != lastFilterType)
    {
        for (auto& l : ladder) l.reset();
        for (auto& s : svf) s.reset();
        lastFilterType = type;
    }
    const float reso = juce::jlimit (0.0f, 1.0f, p.reso);
    const float ladderK = 4.1f * std::pow (reso, 0.8f);
    const float svfK = 1.0f / (0.6f * std::pow (40.0f, reso));
    const float drive = 1.0f + 5.0f * p.filterDrive * p.filterDrive;
    const float driveNorm = 1.0f / (1.0f + 0.5f * (drive - 1.0f));
    const float maxFc = (float) juce::jmin (0.45 * sr, 20000.0);
    const float keyOct = p.keyTrack * (pitchNow - 60.0f) * (1.0f / 12.0f);

    const float velGain = 1.0f - p.velSens + p.velSens * velocity * velocity;
    const float base = p.baseGain * velGain;
    const float dExt = (extGainTarget - extGain) * invN;
    const float dTrem = (p.lfoAmpEnd - p.lfoAmpStart) * invN;
    float trem = p.lfoAmpStart;

    float G = 0.0f, g = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float fe = flt.next();
        if ((i & 7) == 0)
        {
            const float oct = p.cutoffOct + p.envOct * fe + keyOct + p.lfoCutOct;
            const float fc = juce::jlimit (16.0f, maxFc, std::exp2 (oct));
            g = std::tan (juce::MathConstants<float>::pi * fc / (float) sr);
            G = g / (1.0f + g);
        }

        float l = tmpL[i], r = tmpR[i];
        if (type == 0)
        {
            l = ladder[0].process (l, G, ladderK, drive, driveNorm);
            r = ladder[1].process (r, G, ladderK, drive, driveNorm);
        }
        else
        {
            l = svf[0].process (saturate (l, drive, driveNorm), g, svfK, type);
            r = svf[1].process (saturate (r, drive, driveNorm), g, svfK, type);
        }

        trem += dTrem;
        const float gain = amp.next() * base * trem;
        tmpL[i] = l * gain;
        tmpR[i] = r * gain;
    }

    // ---- per-note drive: this voice alone, after its envelope (decaying notes clean up) ----
    if (p.driveAmount > 0.0f || noteDrive.isRunning())
        noteDrive.process (tmpL, tmpR, n, p.driveAmount, p.driveType, p.driveTone,
                           2.0f * base * juce::jmax (0.05f, p.oscRms));   // includes velocity: loudness-neutral at any velocity

    // the input-following gain comes last, so the drive can't lift a faint (noise-triggered) note
    // to full level: every note keeps its own loudness after the distortion
    for (int i = 0; i < n; ++i)
    {
        extGain += dExt;
        L[i] += tmpL[i] * extGain;
        R[i] += tmpR[i] * extGain;
    }
    extGain = extGainTarget;

    if (amp.isIdle())
    {
        for (auto& c : clip) c.reset();
        for (auto& l : ladder) l.reset();
        for (auto& s : svf) s.reset();
        noteDrive.reset();
        flt.kill();
        held = sustained = false;
        note = -1;
    }
}
}
