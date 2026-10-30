#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include "../Sculpt/SculptModel.h"

// Private building blocks of the synth layer (shared by SynthEngine.cpp, Tracker.cpp and
// SynthVoice.cpp). Nothing here allocates on the audio thread.
namespace synth
{
    constexpr int chunkSize = 32;                    // engine works in chunks of <= 32 samples
    constexpr int maxUnison = 7;
    constexpr int numMidiVoices = 8;
    constexpr int maxChordNotes = 6;                 // tracked voices in Chord mode, one per guitar string (Mono uses the first)
    constexpr int numPartials = sculpt::maxPartials;
    constexpr int numGroups = numPartials / 4;       // partials are rendered 4 at a time (SIMD)
    constexpr float twoPi = juce::MathConstants<float>::twoPi;

    using Vec = juce::dsp::SIMDRegister<float>;
    static_assert (Vec::SIMDNumElements == 4, "the additive bank assumes 4-lane SIMD");

    inline float midiToHz (float note) noexcept   { return 440.0f * std::exp2 ((note - 69.0f) * (1.0f / 12.0f)); }
    inline float hzToMidi (float hz) noexcept     { return 69.0f + 12.0f * std::log2 (juce::jmax (1.0e-3f, hz) / 440.0f); }

    // One-pole coefficient for a time constant in seconds, updated every `step` samples
    inline float onePole (float seconds, double sr, int step = 1) noexcept
    {
        if (seconds <= 0.0f)
            return 1.0f;
        return 1.0f - (float) std::exp (-(double) step / (seconds * sr));
    }

    // xorshift white noise in -1..1
    struct Rng
    {
        uint32_t s = 0x9e3779b9u;
        float next() noexcept
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            return (float) (int32_t) s * 4.6566129e-10f;
        }
    };

    // ADSR with an analog-style convex attack and exponential decay/release.
    // Times are "to the end of the stage": attack reaches 1.0 after A ms, decay and release fall
    // 60 dB (towards sustain / silence) in D / R ms.
    struct Adsr
    {
        enum Stage { idle, attack, decay, sustain, release };

        void set (float aMs, float dMs, float sus, float rMs, double sr) noexcept
        {
            aCoef = onePole (juce::jmax (0.5f, aMs) * 0.001f / 1.7918f, sr);    // 1.2 * (1 - e^-t) hits 1 at t = ln 6
            dCoef = onePole (juce::jmax (1.0f, dMs) * 0.001f / 6.9078f, sr);    // ln 1000: -60 dB
            rCoef = onePole (juce::jmax (2.0f, rMs) * 0.001f / 6.9078f, sr);
            sustainLevel = sus;
        }

        void gateOn() noexcept   { stage = attack; }   // restarts from the current level: no click
        void gateOff() noexcept  { if (stage != idle) stage = release; }
        void kill() noexcept     { stage = idle; level = 0.0f; }
        bool isIdle() const noexcept { return stage == idle; }

        float next() noexcept
        {
            switch (stage)
            {
                case attack:
                    level += (1.2f - level) * aCoef;
                    if (level >= 1.0f) { level = 1.0f; stage = decay; }
                    break;
                case decay:
                    level = sustainLevel + (level - sustainLevel) * (1.0f - dCoef);
                    if (std::abs (level - sustainLevel) < 1.0e-4f) { level = sustainLevel; stage = sustain; }
                    break;
                case sustain:
                    level += (sustainLevel - level) * dCoef;   // follows sustain changes smoothly
                    break;
                case release:
                    level *= 1.0f - rCoef;
                    if (level < 1.0e-4f) { level = 0.0f; stage = idle; }
                    break;
                case idle:
                default:
                    break;
            }
            return level;
        }

        Stage stage = idle;
        float level = 0.0f, aCoef = 1.0f, dCoef = 1.0f, rCoef = 1.0f, sustainLevel = 1.0f;
    };

    // Cubic soft clipper with first-order antiderivative anti-aliasing (ADAA): the drive stage
    // adds harmonics without the fold-back aliasing a plain waveshaper would produce.
    struct AdaaClip
    {
        static float clip (float x) noexcept
        {
            if (x >= 1.0f)  return 1.0f;
            if (x <= -1.0f) return -1.0f;
            return 1.5f * x - 0.5f * x * x * x;
        }
        static double integral (double x) noexcept
        {
            const double a = std::abs (x);
            if (a >= 1.0)
                return a - 0.375;
            const double x2 = x * x;
            return 0.75 * x2 - 0.125 * x2 * x2;
        }
        float process (float x) noexcept
        {
            const double F = integral (x);
            const double d = (double) x - x1;
            const float y = std::abs (d) > 1.0e-5 ? (float) ((F - F1) / d) : clip (0.5f * (x + (float) x1));
            x1 = x;
            F1 = F;
            return y;
        }
        void reset() noexcept { x1 = 0.0; F1 = 0.0; }
        double x1 = 0.0, F1 = 0.0;
    };

    inline float tanhApprox (float x) noexcept
    {
        x = juce::jlimit (-3.0f, 3.0f, x);
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    // Soft saturation used at the filter inputs: nearly clean at drive 1 (3 % compression at full
    // scale), bounded so resonance can never run away.
    inline float saturate (float x, float drive, float norm) noexcept
    {
        constexpr float k = 0.3f;
        return tanhApprox (k * drive * x) * (norm / k);
    }

    // Zero-delay-feedback (TPT) 4-pole ladder low-pass with a soft limiter on the feedback sum, so
    // high resonance stays bounded and self-oscillates musically.
    struct Ladder
    {
        float s[4] {};
        void reset() noexcept { s[0] = s[1] = s[2] = s[3] = 0.0f; }

        float process (float x, float G, float k, float drive, float driveNorm) noexcept
        {
            const float beta = 1.0f - G;
            const float G2 = G * G;
            const float sigma = beta * (G2 * G * s[0] + G2 * s[1] + G * s[2] + s[3]);
            float u = (x * (1.0f + 0.5f * k) - k * sigma) / (1.0f + k * G2 * G2);
            u = saturate (u, drive, driveNorm);
            float y = u;
            for (auto& st : s)
            {
                const float v = (y - st) * G;
                const float out = v + st;
                st = out + v;
                y = out;
            }
            return y;
        }
    };

    // Simper/Zavalishin state-variable filter (TPT): LP12, band-pass, high-pass and notch
    struct Svf
    {
        float ic1 = 0.0f, ic2 = 0.0f;
        void reset() noexcept { ic1 = ic2 = 0.0f; }

        float process (float x, float g, float k, int mode) noexcept
        {
            const float a1 = 1.0f / (1.0f + g * (g + k));
            const float a2 = g * a1;
            const float a3 = g * a2;
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            switch (mode)
            {
                case 1:  return v2;                    // LP 12
                case 2:  return k * v1;                // band-pass, 0 dB at the peak
                case 3:  return x - k * v1 - v2;       // high-pass
                default: return x - k * v1;            // notch
            }
        }
    };

    // sin and cos of x in [0, pi] (Taylor around pi/2, error < 4e-7); the bank renormalises its
    // rotators every chunk so the tiny magnitude error never accumulates.
    inline void sinCos (Vec x, Vec& sOut, Vec& cOut) noexcept
    {
        const Vec y = x - Vec::expand (juce::MathConstants<float>::halfPi);
        const Vec y2 = y * y;
        // cos y and sin y (y in [-pi/2, pi/2])
        Vec cy = Vec::expand (1.0f) + y2 * (Vec::expand (-0.5f) + y2 * (Vec::expand (1.0f / 24.0f)
                 + y2 * (Vec::expand (-1.0f / 720.0f) + y2 * (Vec::expand (1.0f / 40320.0f)
                 + y2 * Vec::expand (-1.0f / 3628800.0f)))));
        Vec sy = y * (Vec::expand (1.0f) + y2 * (Vec::expand (-1.0f / 6.0f) + y2 * (Vec::expand (1.0f / 120.0f)
                 + y2 * (Vec::expand (-1.0f / 5040.0f) + y2 * (Vec::expand (1.0f / 362880.0f)
                 + y2 * Vec::expand (-1.0f / 39916800.0f))))));
        sOut = cy;                         // sin x = cos (x - pi/2)
        cOut = Vec::expand (0.0f) - sy;    // cos x = -sin (x - pi/2)
    }

    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

        void lowPass (double sr, double fc, double q) noexcept
        {
            const double w = juce::MathConstants<double>::twoPi * fc / sr;
            const double alpha = std::sin (w) / (2.0 * q);
            const double cw = std::cos (w);
            const double a0 = 1.0 + alpha;
            b0 = (float) ((1.0 - cw) * 0.5 / a0);
            b1 = (float) ((1.0 - cw) / a0);
            b2 = b0;
            a1 = (float) (-2.0 * cw / a0);
            a2 = (float) ((1.0 - alpha) / a0);
        }

        float process (float x) noexcept
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    //==============================================================================
    // Monophonic pitch + envelope tracker (McLeod pitch method on a decimated signal).
    class PitchTracker
    {
    public:
        void prepare (double sampleRate);
        void reset() noexcept;
        void setRange (int rangeIndex) noexcept;
        void setGateDb (float db) noexcept           { gateDb = db; }
        void setOnsetSensitivity (float s) noexcept  { onsetThresholdDb = 12.0f - 9.0f * juce::jlimit (0.0f, 1.0f, s); }

        void process (const float* mono, int n) noexcept;   // full sample rate

        // results (audio thread)
        float f0 = 0.0f;            // Hz, 0 = never had a pitch; holds the last pitch while unvoiced
        float clarity = 0.0f;       // NSDF peak of the last frame (0..1)
        float confidence = 0.0f;    // smoothed 0..1
        bool voiced = false;
        float envelope = 0.0f;      // follow envelope: RMS over one tracked period (linear)
        float levelDb = -120.0f;
        int onsets = 0;             // increments on every detected attack
        int frames = 0;             // increments on every analysis frame
        int framesSinceOnset = 1000;
        float candidate = 0.0f;     // this frame's own pitch estimate (Hz; 0 = no clean estimate), even
                                    // when f0 keeps the last accepted pitch

        static constexpr float rangeLo[4] = { 32.0f, 55.0f, 150.0f, 32.0f };
        static constexpr float rangeHi[4] = { 450.0f, 1400.0f, 3000.0f, 3000.0f };

    private:
        void analyse() noexcept;
        float pickPeriod (int tauMin, int tauMax, float& clarityOut) noexcept;
        void setEnvelopeWindow (int samples) noexcept;

        double sr = 48000.0, srD = 12000.0;
        int decim = 4, decimPhase = 0, hop = 32, hopCounter = 0;
        float fMin = 55.0f, fMax = 1400.0f;
        float gateDb = -55.0f, onsetThresholdDb = 9.0f;

        // decimation: DC blocker + 4th-order Butterworth low-pass
        float dcX1 = 0.0f, dcY1 = 0.0f, dcR = 0.998f;
        Biquad decimLp[2];

        static constexpr int ringSize = 4096;
        std::array<float, ringSize> ring {};
        int ringPos = 0;

        std::array<std::unique_ptr<juce::dsp::FFT>, 12> ffts;
        std::vector<float> fftBuf, frame, nsdf;

        // envelope: running mean of x^2 over a window of `envLen` samples
        std::vector<float> sqRing;
        int sqPos = 0, envLen = 480;
        double sqSum = 0.0;
        float envAtt = 1.0f, envRel = 1.0f;
        int recalcCounter = 0;

        // onset detection
        float preX1 = 0.0f, onsetEnv = 0.0f, onsetCoef = 0.0f, slowDb = -120.0f, slowCoef = 0.0f;
        bool onsetArmed = true;
        int refractory = 0;

        float period = 0.0f;        // decimated samples
        float pendingF = 0.0f;
        int pendingCount = 0;
        float lastClarity = 0.0f;
        int badFrames = 0;
    };

    //==============================================================================
    // Polyphonic tracker for the Chord tracking mode: up to 6 notes at once in one mixed signal
    // (iterative harmonic-sum estimation with spectral-smoothness cancellation), followed by
    // note tracking with hysteresis so every heard note keeps a stable identity (slot id).
    class ChordTracker
    {
    public:
        struct Note
        {
            uint32_t id = 0;          // stable while the note lasts; 0 = slot unused
            float pitch = 0.0f;       // MIDI note number, fractional (as heard, no transpose)
            float level = 0.0f;       // RMS of the note's partials (linear, like PitchTracker::envelope)
            float salience = 0.0f;
            bool active = false;      // confirmed: plays a voice
            bool retrigger = false;   // re-struck at the last attack (the engine clears it)
            bool matched = false;     // heard in the latest frame
            int hits = 0, misses = 0;
            int activatedAt = 0;      // frame number when it started playing
            float levelAtOnset = 0.0f;
        };
        static constexpr int maxSlots = 8;

        void prepare (double sampleRate);
        void reset() noexcept;
        void clearNotes() noexcept;
        void setRange (int rangeIndex) noexcept;
        void setGateDb (float db) noexcept           { gateDb = db; }
        void setMaxNotes (int n) noexcept            { maxNotes = juce::jlimit (1, maxChordNotes, n); }
        void setSensitivity (float s) noexcept       { sens = juce::jlimit (0.0f, 1.0f, s); }
        void setEnabled (bool on) noexcept;          // off: keeps the input history warm, skips the analysis
        void onset() noexcept;                       // an attack was detected (PitchTracker::onsets)

        void process (const float* mono, int n) noexcept;   // full sample rate

        std::array<Note, maxSlots> slots {};
        int frames = 0;             // increments on every analysed frame

    private:
        struct Peak { float hz, amp, raw; };   // whitened (residual) and raw amplitude
        struct FrameNote { float pitch, salience, level, upper; bool strong, onHarmonic; };

        void analyse() noexcept;
        void buildWindow() noexcept;
        float salience (float f0, bool fine, int* match) const noexcept;
        int strongestPeak (float lo, float hi) const noexcept;
        int fundamentalPeak (float hz) const noexcept;
        void trackNotes (const FrameNote* notes, int count, bool silent, bool decision) noexcept;
        bool harmonicOfActive (float pitch) const noexcept;

        double sr = 48000.0, srD = 12000.0;
        int decim = 4, decimPhase = 0, hop = 64, hopCounter = 0;
        int rangeIndex = 1, winLen = 1020, builtWinLen = 0, decideLen = 384, decideMaxLen = 624;
        float fLo = 55.0f, fHi = 1400.0f;
        float gateDb = -55.0f, sens = 0.5f;
        int maxNotes = maxChordNotes;
        bool enabled = false, debug = false;

        float dcX1 = 0.0f, dcY1 = 0.0f, dcR = 0.998f;
        Biquad decimLp[2];
        std::vector<float> ring;
        int ringPos = 0;

        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, attackWindow, fftBuf, mag, white, bandGain;
        float ampNorm = 1.0f, binHz = 5.86f;
        int topBin = 500, frameLen = 768;

        static constexpr int maxBands = 32;
        int numBands = 0;
        float bandCentre[maxBands] {};      // in bins

        static constexpr int maxPeaks = 192;
        std::array<Peak, maxPeaks> peaks {};
        std::array<float, maxPeaks> resid {}, residRaw {};
        int numPeaks = 0;

        static constexpr int maxCands = 320;
        std::array<float, maxCands> candHz {}, candMidi {};
        int numCands = 0;

        uint32_t nextId = 1;
        int sinceOnset = 1 << 12, onsetFrame = 0;   // decimated samples since the last attack
        bool decisionPending = false;
    };

    //==============================================================================
    // Per-note distortion: each voice is driven on its own before the voices are summed, so
    // chords stay free of the intermodulation one distorted amp adds to a whole chord.
    // Pre-emphasis into the clipper, 2x oversampled shapers with first-order antiderivative
    // anti-aliasing, DC blocker, tone low-pass and a level makeup that keeps loudness steady.
    class NoteDrive
    {
    public:
        void prepare (double sampleRate);
        void reset() noexcept;
        bool isRunning() const noexcept { return wetNow > 0.0f; }

        // in place, n <= chunkSize. amount / tone 0..1, type 0 overdrive, 1 distortion, 2 fuzz;
        // refLevel = the voice's nominal RMS x 2 (the input is normalised by it)
        void process (float* L, float* R, int n, float amount, int type, float tone, float refLevel) noexcept;

    private:
        struct Shaper
        {
            double x1 = 0.0, F1 = 0.0;
            float process (float x, int type) noexcept;
            void reset() noexcept { x1 = 0.0; F1 = 0.0; }
        };
        struct Channel
        {
            float lowZ = 0.0f, bumpZ = 0.0f, preLpZ = 0.0f, dcX = 0.0f, dcY = 0.0f;
            Biquad tone;
            Shaper shaper;
        };

        double sr = 48000.0;
        std::unique_ptr<juce::dsp::Oversampling<float>> os;
        Channel ch[2];
        int lastType = -1;
        float gainNow = 0.0f, wetNow = 0.0f, toneNow = -1.0f;
        float aLow = 0.0f, aBump = 0.0f, aPreLp = 0.0f, aDc = 0.0f;
    };

    //==============================================================================
    // Values the engine prepares once per chunk for every voice
    struct VoiceParams
    {
        int unison = 1;
        float detuneCents = 0.0f, spread = 0.0f;
        float subLevel = 0.0f; int subOct = 1; bool subSquare = false;
        float noiseLevel = 0.0f, noiseColour = 0.0f;
        float phaseRand = 0.0f, warmth = 0.0f;
        int filterType = 0;                 // 0 ladder, 1 LP12, 2 BP, 3 HP, 4 notch
        float cutoffOct = 13.0f, reso = 0.0f, filterDrive = 0.0f, envOct = 0.0f, keyTrack = 0.0f;
        float lfoPitchSemis = 0.0f, lfoCutOct = 0.0f;
        float lfoAmpStart = 1.0f, lfoAmpEnd = 1.0f;
        float glideMs = 0.0f;
        float bendSemis = 0.0f;             // MIDI voices only
        float velSens = 0.7f;
        bool overtonesOnly = false;
        float baseGain = 0.35f;
        float oscRms = 0.5f;                // RMS of the partial set at amplitude 1 (drive normalisation)
        float driveAmount = 0.0f, driveTone = 0.5f;
        int driveType = 0;                  // 0 overdrive, 1 distortion, 2 fuzz
        float ampA = 3, ampD = 300, ampS = 1, ampR = 120;
        float fltA = 1, fltD = 400, fltS = 0, fltR = 200;
    };

    class SynthVoice
    {
    public:
        void prepare (double sampleRate);
        void reset() noexcept;

        // pitch in MIDI note numbers (fractional). A fresh voice glides from `glideFrom` when it
        // is > 0 and glide is on; a voice that is still sounding continues from where it is.
        void noteOn (float pitch, float velocity, float glideFrom,
                     const sculpt::PartialSet&, const VoiceParams&) noexcept;
        void retrigger() noexcept { amp.gateOn(); flt.gateOn(); }
        void noteOff() noexcept   { amp.gateOff(); flt.gateOff(); held = false; }
        void setTargetPitch (float pitch) noexcept { targetPitch = pitch; }
        void setExternalGain (float g) noexcept { extGainTarget = g; }
        void enablePll (bool on) noexcept;

        // adds this voice into L/R for n <= chunkSize samples; `pllInput` (mono input) is only
        // used by the tracked voice in Overtones mode to phase-lock to the input's fundamental
        void render (float* L, float* R, int n, const VoiceParams&, const sculpt::PartialSet&,
                     const float* pllInput) noexcept;

        bool isActive() const noexcept { return ! amp.isIdle(); }
        float currentPitch() const noexcept { return pitchNow; }
        float currentHz() const noexcept { return hzNow; }
        float level() const noexcept { return amp.level; }

        int note = -1;
        bool held = false, sustained = false;
        uint32_t age = 0;
        float velocity = 1.0f;

    private:
        void initPhases (const sculpt::PartialSet&, float phaseRand) noexcept;

        struct alignas (16) Bank
        {
            float re[numPartials], im[numPartials], c[numPartials], s[numPartials], a[numPartials];
        };

        double sr = 48000.0;
        float pitchNow = 60.0f, targetPitch = 60.0f, hzNow = 261.6f;
        Adsr amp, flt;
        std::array<Bank, maxUnison> banks;
        int activeGroups = numGroups;
        float extGain = 1.0f, extGainTarget = 1.0f, gainNow = 0.0f;
        float subPhase = 0.0f;
        Rng rng;
        float noiseLp = 0.0f;
        AdaaClip clip[2];
        Ladder ladder[2];
        Svf svf[2];
        NoteDrive noteDrive;
        int lastFilterType = 0;
        float subGainNow = 0.0f, noiseGainNow = 0.0f;

        // phase-locked loop (Overtones mode)
        bool pll = false;
        float pllI = 0.0f, pllQ = 0.0f, pllInteg = 0.0f, pllDw = 0.0f;
    };
}
