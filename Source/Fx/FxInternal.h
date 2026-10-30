#pragma once

#include "FxRack.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <vector>

// Private to the FX rack: shared DSP building blocks, the eight stages and FxRack::Impl.
// Every stage works on raw channel pointers (1 or 2 channels), in place, RT-safe.

namespace fx
{
constexpr float pi = juce::MathConstants<float>::pi;
constexpr int maxCh = 2;
constexpr int chunk = 32;   // control-rate interval: coefficients and smoothers update every 32 samples

inline float dbToGain (float db) noexcept   { return std::exp (db * 0.115129255f); }
inline float gainToDb (float g) noexcept    { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }

// Rational tanh, error < 1e-5 inside +-4.97 where it reaches 1
inline float fastTanh (float x) noexcept
{
    if (x > 4.97f)  return 1.0f;
    if (x < -4.97f) return -1.0f;
    const float x2 = x * x;
    const float num = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    const float den = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + 28.0f * x2));
    return juce::jlimit (-1.0f, 1.0f, num / den);
}

inline float onePoleCoef (float seconds, double rate) noexcept
{
    return seconds <= 0.0f ? 1.0f : 1.0f - (float) std::exp (-1.0 / (seconds * rate));
}

// One-pole parameter smoother, stepped at control rate (once per chunk)
struct Smooth
{
    float value = 0.0f, coef = 1.0f;
    void setTime (float seconds, double controlRate) noexcept { coef = onePoleCoef (seconds, controlRate); }
    void snap (float v) noexcept { value = v; }
    float step (float target) noexcept
    {
        value += coef * (target - value);
        if (std::abs (target - value) < 1.0e-6f * (1.0f + std::abs (target)))
            value = target;
        return value;
    }
};

// Cytomic (Simper) trapezoidal state-variable filter: modulation-safe, exact analog magnitude
// after frequency warping. Two channels of state.
struct Svf
{
    enum Type { lowpass, highpass, bandpass, bell, lowShelf, highShelf, allpass };

    struct Coefs
    {
        float g = 0.0f, k = 1.0f, a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, m0 = 1.0f, m1 = 0.0f, m2 = 0.0f;

        static Coefs make (Type t, float freq, float q, float gainDb, double sr) noexcept
        {
            Coefs c;
            const float f = juce::jlimit (5.0f, (float) (sr * 0.49), freq);
            float g = (float) std::tan (juce::MathConstants<double>::pi * f / sr);
            const float A = std::pow (10.0f, gainDb / 40.0f);
            float k = 1.0f / juce::jmax (0.05f, q);
            switch (t)
            {
                case lowpass:   c.m0 = 0.0f; c.m1 = 0.0f; c.m2 = 1.0f; break;
                case highpass:  c.m0 = 1.0f; c.m1 = -k;   c.m2 = -1.0f; break;
                case bandpass:  c.m0 = 0.0f; c.m1 = 1.0f; c.m2 = 0.0f; break;
                case allpass:   c.m0 = 1.0f; c.m1 = -2.0f * k; c.m2 = 0.0f; break;
                case bell:      k = 1.0f / (juce::jmax (0.05f, q) * A); c.m0 = 1.0f; c.m1 = k * (A * A - 1.0f); c.m2 = 0.0f; break;
                case lowShelf:  g /= std::sqrt (A); c.m0 = 1.0f; c.m1 = k * (A - 1.0f); c.m2 = A * A - 1.0f; break;
                case highShelf: g *= std::sqrt (A); c.m0 = A * A; c.m1 = k * (1.0f - A) * A; c.m2 = 1.0f - A * A; break;
            }
            c.g = g;
            c.k = k;
            c.a1 = 1.0f / (1.0f + g * (g + k));
            c.a2 = g * c.a1;
            c.a3 = g * c.a2;
            return c;
        }

        // Magnitude at frequency f (Hz), from the analog prototype at the warped frequency
        double magnitude (double f, double sr) const noexcept
        {
            const double w = std::tan (juce::MathConstants<double>::pi * juce::jmin (f, sr * 0.4999) / sr) / g;
            const std::complex<double> s (0.0, w);
            const auto d = s * s + (double) k * s + 1.0;
            const auto h = (double) m0 + (double) m1 * s / d + (double) m2 / d;
            return std::abs (h);
        }
    };

    Coefs c;
    float ic1[maxCh] {}, ic2[maxCh] {};

    void set (Type t, float freq, float q, float gainDb, double sr) noexcept { c = Coefs::make (t, freq, q, gainDb, sr); }
    void reset() noexcept { for (int i = 0; i < maxCh; ++i) ic1[i] = ic2[i] = 0.0f; }

    inline float process (float v0, int ch) noexcept
    {
        const float v3 = v0 - ic2[ch];
        const float v1 = c.a1 * ic1[ch] + c.a2 * v3;
        const float v2 = ic2[ch] + c.a2 * ic1[ch] + c.a3 * v3;
        ic1[ch] = 2.0f * v1 - ic1[ch];
        ic2[ch] = 2.0f * v2 - ic2[ch];
        return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
    }
};

// Circular delay line (power-of-two size) with cubic Hermite reads. Delay 0 = newest sample.
struct DelayLine
{
    std::vector<float> buf;
    int mask = 0, w = 0;

    void prepare (int maxDelaySamples)
    {
        int size = 4;
        while (size < maxDelaySamples + 4)
            size <<= 1;
        buf.assign ((size_t) size, 0.0f);
        mask = size - 1;
        w = 0;
    }
    void clear() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }
    int maxDelay() const noexcept { return mask - 3; }

    inline void push (float x) noexcept { buf[(size_t) w] = x; w = (w + 1) & mask; }
    inline float tap (int d) const noexcept { return buf[(size_t) ((w - 1 - d) & mask)]; }

    inline float read (float d) const noexcept
    {
        d = juce::jlimit (1.0f, (float) maxDelay(), d);
        const float rp = (float) (w - 1) - d;
        const float fl = std::floor (rp);
        const int i = (int) fl;
        const float t = rp - fl;
        const float x0 = buf[(size_t) ((i - 1) & mask)], x1 = buf[(size_t) (i & mask)];
        const float x2 = buf[(size_t) ((i + 1) & mask)], x3 = buf[(size_t) ((i + 2) & mask)];
        const float c1 = 0.5f * (x2 - x0);
        const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
        const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
        return ((c3 * t + c2) * t + c1) * t + x1;
    }

    inline float readLinear (float d) const noexcept
    {
        d = juce::jlimit (0.0f, (float) maxDelay(), d);
        const float rp = (float) (w - 1) - d;
        const float fl = std::floor (rp);
        const int i = (int) fl;
        const float t = rp - fl;
        const float a = buf[(size_t) (i & mask)], b = buf[(size_t) ((i + 1) & mask)];
        return a + t * (b - a);
    }
};

// Fixed integer delay for keeping a bypassed latency-bearing module time-aligned
struct FixedDelay
{
    std::vector<float> buf[maxCh];
    int len = 0, pos = 0;

    void prepare (int samples)
    {
        len = juce::jmax (0, samples);
        for (auto& b : buf)
            b.assign ((size_t) juce::jmax (1, len), 0.0f);
        pos = 0;
    }
    void clear() noexcept { for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f); pos = 0; }

    void process (float* const* ch, int numCh, int n) noexcept
    {
        if (len <= 0)
            return;
        int p = pos;
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < numCh; ++c)
            {
                const float d = buf[c][(size_t) p];
                buf[c][(size_t) p] = ch[c][i];
                ch[c][i] = d;
            }
            p = (p + 1 == len) ? 0 : p + 1;
        }
        pos = p;
    }
};

// Tempo divisions shared by Mod and Delay. Beats are quarter notes.
inline juce::StringArray syncNames (bool withBars)
{
    juce::StringArray s;
    for (auto* base : { "1/32", "1/16", "1/8", "1/4", "1/2", "1/1" })
    {
        s.add (juce::String (base) + "T");
        s.add (base);
        s.add (juce::String (base) + "D");
    }
    if (withBars)
    {
        s.add ("2 bars");
        s.add ("4 bars");
    }
    return s;
}

inline double syncBeats (int index) noexcept
{
    if (index >= 18)
        return index == 18 ? 8.0 : 16.0;
    static const double bases[] = { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 };
    const double b = bases[juce::jlimit (0, 5, index / 3)];
    const int v = index % 3;
    return v == 0 ? b * 2.0 / 3.0 : (v == 2 ? b * 1.5 : b);
}

inline std::atomic<float>* raw (juce::AudioProcessorValueTreeState& s, const char* id)
{
    auto* p = s.getRawParameterValue (id);
    jassert (p != nullptr);
    static std::atomic<float> dummy { 0.0f };
    return p != nullptr ? p : &dummy;
}

inline float get (const std::atomic<float>* p) noexcept { return p->load (std::memory_order_relaxed); }

//==============================================================================
class AmpStage
{
public:
    struct Voice
    {
        const char* name;
        const char* blurb;
        float hpf, emphF, emphDb, emphQ;
        float driveMinDb, driveMaxDb;
        int stages;
        float bias, stageGainDb, interLpf, interHpf;
        float bassF, midF, trebleF, scoopDb;
        float powerDrive, sag, trimDb, autoLevel, cleanLow;
    };
    static const Voice& voice (int model) noexcept;
    static juce::StringArray modelNames();

    void bind (juce::AudioProcessorValueTreeState&);
    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset() noexcept;
    int latency() const noexcept { return lat; }
    void process (float* const* ch, int numCh, int n) noexcept;

private:
    void updateFilters (const Voice&, float bass, float mid, float treble, float presence, bool bright, float drive01) noexcept;

    std::atomic<float> *pModel = nullptr, *pGain = nullptr, *pBass = nullptr, *pMid = nullptr, *pTreble = nullptr,
                       *pPresence = nullptr, *pLevel = nullptr, *pGate = nullptr, *pBright = nullptr;
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    double sr = 44100.0, osr = 176400.0;
    int lat = 0, prepCh = 2;
    int model = 1, pendingModel = 1;
    float switchGain = 1.0f;

    Svf inHpf, brightSh, emph, presenceSh, dcBlock;
    Svf stageHp[3], stageLp[3], bassSh, midBell, trebleSh, lowSplit;
    Smooth driveSm, levelSm, bassSm, midSm, trebleSm, presSm;
    float driveGain = 1.0f, outGain = 1.0f;
    float sagEnv[maxCh] {};
    float sagAtt = 0.0f, sagRel = 0.0f;
    float gateEnv = 0.0f, gateGain = 1.0f, gateRel = 0.0f, gateOpen = 0.0f, gateClose = 0.0f, gateEnvRel = 0.0f;
    int gateHold = 0, gateHoldMax = 0;
};

class CabStage
{
public:
    static juce::StringArray modelNames();
    // Designed magnitude (dB) of the close-mic speaker response, also drawn by the panel
    static float designDb (int model, float mic, float lowCut, float highCut, double f) noexcept;

    void bind (juce::AudioProcessorValueTreeState&);
    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset() noexcept;
    void process (float* const* ch, int numCh, int n) noexcept;
    void rebuildIfNeeded();   // worker thread: synthesises and loads a new IR when the cab settings changed

private:
    juce::AudioBuffer<float> buildIr (int model, float mic, float room, float lowCut, float highCut) const;

    std::atomic<float> *pModel = nullptr, *pMic = nullptr, *pRoom = nullptr, *pLevel = nullptr, *pLowCut = nullptr, *pHighCut = nullptr;
    juce::dsp::Convolution conv;
    std::atomic<double> sr { 44100.0 };
    std::atomic<int> irChannels { 2 };
    std::atomic<bool> prepared { false };
    float lastKey[5] { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
    Smooth levelSm;
    float gain = 1.0f;
};

class EqStage
{
public:
    struct Settings
    {
        float lowCut = 20.0f, lowFreq = 100.0f, lowGain = 0.0f;
        float m1Freq = 400.0f, m1Gain = 0.0f, m1Q = 1.0f;
        float m2Freq = 2500.0f, m2Gain = 0.0f, m2Q = 1.0f;
        float highFreq = 8000.0f, highGain = 0.0f, highCut = 20000.0f, out = 0.0f;
    };
    static Settings read (juce::AudioProcessorValueTreeState&);
    static double responseDb (const Settings&, double f, double sr) noexcept;
    static bool lowCutOn (float hz) noexcept  { return hz > 20.5f; }
    static bool highCutOn (float hz) noexcept { return hz < 19900.0f; }

    void bind (juce::AudioProcessorValueTreeState&);
    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset() noexcept;
    void process (float* const* ch, int numCh, int n) noexcept;

private:
    std::array<std::atomic<float>*, 13> p {};
    std::array<Smooth, 13> sm;
    Svf lc[2], ls, b1, b2, hs, hc[2];
    double sr = 44100.0;
    float outGain = 1.0f;
};

class CompStage
{
public:
    void bind (juce::AudioProcessorValueTreeState&);
    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset() noexcept;
    void process (float* const* ch, int numCh, int n) noexcept;
    static float autoMakeupDb (float threshDb, float ratio) noexcept { return 0.3f * -threshDb * (1.0f - 1.0f / ratio); }
    static float curveDb (float inDb, float threshDb, float ratio, float kneeDb) noexcept;   // static transfer curve

    std::atomic<float> grDb { 0.0f }, inDb { -100.0f };

private:
    std::atomic<float> *pThresh = nullptr, *pRatio = nullptr, *pAttack = nullptr, *pRelease = nullptr, *pKnee = nullptr,
                       *pMakeup = nullptr, *pAuto = nullptr, *pScHpf = nullptr;
    double sr = 44100.0;
    Svf scHp;
    float env = 0.0f, makeupGain = 1.0f;
    Smooth makeupSm;
    float peakIn = 0.0f;
};

class ModStage
{
public:
    enum Type { chorus, ensemble, flanger, phaser, vibrato, tremolo, autopan };
    static juce::StringArray typeNames();

    void bind (juce::AudioProcessorValueTreeState&);
    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset() noexcept;
    void process (float* const* ch, int numCh, int n, const FxRack::Transport&) noexcept;
    static float rateHz (float hz, bool sync, int div, double bpm) noexcept;

private:
    float lfo (double phase, int shape) const noexcept;

    std::atomic<float> *pType = nullptr, *pRate = nullptr, *pSync = nullptr, *pDiv = nullptr, *pDepth = nullptr,
                       *pFeedback = nullptr, *pMix = nullptr, *pWidth = nullptr, *pShape = nullptr, *pTone = nullptr;
    double sr = 44100.0;
    double phase = 0.0;
    DelayLine line[maxCh];
    Svf apStage[maxCh][6];
    Svf toneLp;
    float fb[maxCh] {};
    Smooth depthSm, fbSm, mixSm, widthSm, toneSm;
    int type = 0, pendingType = 0;
    float switchGain = 1.0f;
};

class DelayStage
{
public:
    enum Type { digital, tape, pingpong };
    static juce::StringArray typeNames();

    void bind (juce::AudioProcessorValueTreeState&);
    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset() noexcept;
    // Returns false once the stage is switched off and its repeats have died away
    bool process (float* const* ch, int numCh, int n, const FxRack::Transport&, bool on) noexcept;
    static double timeMs (float ms, bool sync, int div, double bpm) noexcept;

private:
    std::atomic<float> *pType = nullptr, *pSync = nullptr, *pDiv = nullptr, *pTime = nullptr, *pFeedback = nullptr,
                       *pMix = nullptr, *pLowCut = nullptr, *pHighCut = nullptr, *pDuck = nullptr, *pWidth = nullptr,
                       *pWow = nullptr, *pSat = nullptr;
    double sr = 44100.0;
    DelayLine line[maxCh];
    Svf loopHp, loopLp;
    float curDelay = 0.0f, oldDelay = 0.0f, xfade = 1.0f, xfadeStep = 0.0f, tapeDelay = 0.0f;
    double wowPhase = 0.0, flutterPhase = 0.0;
    float noiseState = 0.0f;
    juce::uint32 rng = 12345u;
    float duckEnv = 0.0f, duckAtt = 0.0f, duckRel = 0.0f;
    float feed = 0.0f, feedStep = 0.0f;
    Smooth fbSm, mixSm, widthSm, duckSm, wowSm, satSm;
    int silentSamples = 0;
    int type = 0, pendingType = 0;
    float switchGain = 1.0f;
};

class ReverbStage
{
public:
    enum Type { room, hall, plate, chamber, shimmer };
    static constexpr int numLines = 16;
    static juce::StringArray typeNames();

    void bind (juce::AudioProcessorValueTreeState&);
    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset() noexcept;
    bool process (float* const* ch, int numCh, int n, bool on) noexcept;

    struct TypeVoice { float sizeMs, diffMs, diffusion, erLevel, modRate, inputLp; };
    static const TypeVoice& typeVoice (int t) noexcept;

private:
    std::atomic<float> *pType = nullptr, *pPreDelay = nullptr, *pSize = nullptr, *pDecay = nullptr, *pDamping = nullptr,
                       *pLowCut = nullptr, *pMod = nullptr, *pWidth = nullptr, *pMix = nullptr, *pShimmer = nullptr;
    double sr = 44100.0;
    DelayLine pre[maxCh];
    DelayLine lines[numLines];
    DelayLine diff[maxCh][4];
    DelayLine shimmerLine;
    float lineState[numLines] {};
    float lineLen[numLines] {};
    float dampA[numLines] {}, dampB[numLines] {};
    float lfoC[numLines] {}, lfoS[numLines] {}, rotC[numLines] {}, rotS[numLines] {};
    int diffLen[maxCh][4] {};
    float erTap[maxCh][8] {}, erGain[8] {};
    Svf outHp, inLp, shimHp, shimLp;
    Smooth preSm, sizeSm, decaySm, dampSm, modSm, widthSm, mixSm, shimSm;
    float shimmerPhase = 0.0f, shimmerFb = 0.0f;
    float feed = 0.0f, feedStep = 0.0f;
    int silentSamples = 0;
    int type = 1, pendingType = 1;
    float switchGain = 1.0f;
};

class LimiterStage
{
public:
    void bind (juce::AudioProcessorValueTreeState&);
    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset() noexcept;
    int latency() const noexcept { return hold - 1; }
    void process (float* const* ch, int numCh, int n) noexcept;

    std::atomic<float> grDb { 0.0f };

private:
    std::atomic<float> *pInput = nullptr, *pCeiling = nullptr, *pRelease = nullptr;
    double sr = 44100.0;
    int hold = 1;
    std::vector<float> holdBuf;        // gain targets of the last `hold` samples (ring)
    std::vector<long long> dq;         // monotonic deque of sample indices (ring) for the running minimum
    int dqHead = 0, dqTail = 0;
    std::vector<float> boxBuf;
    double boxSum = 0.0;
    int idx = 0;
    long long counter = 0;
    FixedDelay audioDelay;
    float gainScratch[chunk] {}, ceilScratch[chunk] {};
    float env = 1.0f;
    Smooth inSm, ceilSm;
};
} // namespace fx

//==============================================================================
struct FxRack::Impl
{
    explicit Impl (juce::AudioProcessorValueTreeState&);
    ~Impl();

    void processChunk (float* const* ch, int numCh, int n, const Transport&, bool runRack, bool runLimiter) noexcept;
    void processBuffer (juce::AudioBuffer<float>&, int numChannels, const Transport&, bool runRack, bool runLimiter) noexcept;

    // FxPanels.cpp
    struct Panel;
    static std::unique_ptr<ControlPanel> makePanel (Impl&, FxRack::Module, HelpTarget*);
    juce::String describe();

    struct Slot
    {
        std::atomic<float>* on = nullptr;
        std::atomic<float>* mix = nullptr;   // optional parallel mix (0..100), folded into the crossfade
        float wet = 0.0f;                    // current crossfade position 0..1 (x mix)
        bool active = false;
    };

    class Worker : public juce::Thread
    {
    public:
        explicit Worker (Impl& o) : juce::Thread ("Clod Scope FX worker"), owner (o) {}
        void run() override
        {
            while (! threadShouldExit())
            {
                owner.cab.rebuildIfNeeded();
                wait (60);
            }
        }
        Impl& owner;
    };

    juce::AudioProcessorValueTreeState& apvts;
    fx::AmpStage amp;
    fx::CabStage cab;
    fx::EqStage eq;
    fx::CompStage comp;
    fx::ModStage mod;
    fx::DelayStage delay;
    fx::ReverbStage reverb;
    fx::LimiterStage limiter;

    std::array<Slot, FxRack::numModules> slots;
    std::array<std::atomic<float>, FxRack::numModules> peak;   // output peak (linear) of each active module
    juce::AudioBuffer<float> dry;
    fx::FixedDelay ampBypass, limBypass;
    double sampleRate = 44100.0;
    int maxBlock = 512, channels = 2, latAmp = 0, latLim = 0;
    float fadeStep = 0.001f;
    std::atomic<double> lastBpm { 120.0 };
    std::atomic<bool> isPrepared { false };
    Worker worker;
};

