#include "SculptModel.h"
#include <cmath>
#include <cstring>

namespace
{
    using sculpt::BaseTable;
    using sculpt::PartialSet;
    using sculpt::maxPartials;

    constexpr float pi     = juce::MathConstants<float>::pi;
    constexpr float twoPi  = juce::MathConstants<float>::twoPi;
    constexpr float halfPi = juce::MathConstants<float>::halfPi;
    constexpr int lutSize  = 1024;
    constexpr int crestPoints = 256;
    constexpr size_t maxUndo = 64;

    inline float wrapPhase (float p) noexcept
    {
        p = std::fmod (p + pi, twoPi);
        if (p < 0.0f)
            p += twoPi;
        return p - pi;
    }

    struct TablePair
    {
        BaseTable t[2];
    };

    // All macro values in one plain struct so "did anything change" is a single compare
    struct MacroValues
    {
        float morph = 0.0f, tilt = 0.0f, oddEven = 0.0f;
        float formantPos = 6.0f, formantGain = 0.0f, formantWidth = 1.0f;
        float stretch = 0.0f, partials = 64.0f, comb = 0.0f, combOffset = 25.0f;
        int phaseMode = 0;

        bool operator== (const MacroValues& o) const noexcept { return std::memcmp (this, &o, sizeof (MacroValues)) == 0; }
    };

    // scStretch (0..100 %) -> stiffness B in ratio_k = k * sqrt (1 + B k^2). Cubic so the first
    // half of the dial stays in piano territory and the top end reaches bells and metal bars.
    inline float stretchCoefficient (float percent) noexcept
    {
        const float s = juce::jlimit (0.0f, 1.0f, percent * 0.01f);
        return 0.01f * s * s * s;
    }

    BaseTable normalised (BaseTable t)
    {
        float mx = 0.0f;
        for (auto a : t.amp)
            mx = juce::jmax (mx, a);
        if (mx > 0.0f)
            for (auto& a : t.amp)
                a /= mx;
        return t;
    }

    // The heart of the sculptor: table(s) + macros -> what the synth plays. RT-safe.
    void computeFinal (const TablePair& tp, const MacroValues& m, const float* sinLut, PartialSet& out) noexcept
    {
        const auto& A = tp.t[0];
        const auto& B = tp.t[1];
        const float t = juce::jlimit (0.0f, 1.0f, m.morph * 0.01f);
        const float tiltExp = m.tilt / 6.0206f;                       // 10^(tilt*log2(k)/20) == k^(tilt/6.02)
        const float oe = juce::jlimit (-1.0f, 1.0f, m.oddEven * 0.01f);
        const bool formantOn = std::abs (m.formantGain) > 0.01f;
        const float formantSigma = juce::jmax (0.05f, m.formantWidth * 0.5f / 1.17741f);   // width = full width at half the dB gain
        const float formantLogPos = std::log2 (juce::jmax (1.0f, m.formantPos));
        const float comb = juce::jlimit (0.0f, 1.0f, m.comb * 0.01f);
        const float combD = juce::jlimit (0.0f, 0.5f, m.combOffset * 0.01f);
        const float stiff = stretchCoefficient (m.stretch);

        float amp[maxPartials], ph[maxPartials];
        int count = 0;
        for (int i = 0; i < maxPartials; ++i)
        {
            const float k = (float) (i + 1);
            const float a = A.amp[(size_t) i], b = B.amp[(size_t) i];
            float g = a + (b - a) * t;

            // amplitude-weighted phase blend along the shortest arc: a silent partial takes the
            // other table's phase, so morphing never sweeps a phase that nobody can hear
            const float den = (1.0f - t) * a + t * b;
            const float w = den > 1.0e-9f ? t * b / den : t;
            float p = A.phase[(size_t) i] + w * wrapPhase (B.phase[(size_t) i] - A.phase[(size_t) i]);

            if (tiltExp != 0.0f)
                g *= std::pow (k, tiltExp);

            const bool odd = (i % 2) == 0;
            if (oe < 0.0f && ! odd)
                g *= 1.0f + oe;
            else if (oe > 0.0f && odd && i > 0)
                g *= 1.0f - oe;

            if (formantOn)
            {
                const float d = (std::log2 (k) - formantLogPos) / formantSigma;
                g *= std::pow (10.0f, m.formantGain * std::exp (-0.5f * d * d) / 20.0f);
            }

            if (comb > 0.0f)
                g *= (1.0f - comb) + comb * std::abs (std::sin (pi * k * combD));

            g *= juce::jlimit (0.0f, 1.0f, m.partials - k + 1.0f);

            if (m.phaseMode == 1)
                p = halfPi;   // cosine phase: every partial peaks at t = 0

            amp[i] = g;
            ph[i] = p;
            if (g > 1.0e-6f)
                count = i + 1;
        }

        if (m.phaseMode == 2)   // Schroeder phases: the flattest crest for this partial count
            for (int i = 0; i < count; ++i)
                ph[i] = wrapPhase (pi * (float) ((i + 1) * (i + 1)) / (float) juce::jmax (1, count));

        // fast crest estimate: sample one cycle at 256 points from the sine table
        float peak = 0.0f, sumAmp = 0.0f;
        int pidx[maxPartials] {};
        for (int i = 0; i < count; ++i)
        {
            pidx[i] = (int) std::lround (ph[i] * (float) lutSize / twoPi);
            sumAmp += amp[i];
        }
        for (int n = 0; n < crestPoints; ++n)
        {
            float v = 0.0f;
            for (int i = 0; i < count; ++i)
                if (amp[i] > 1.0e-6f)
                    v += amp[i] * sinLut[((i + 1) * n * (lutSize / crestPoints) + pidx[i]) & (lutSize - 1)];
            peak = juce::jmax (peak, std::abs (v));
        }
        if (stiff > 0.0f)
            peak = juce::jmax (peak, 0.6f * sumAmp);   // drifting partials line up now and then

        const float norm = peak > 1.0e-6f ? 1.0f / peak : 0.0f;
        out.count = count;
        out.harmonic = stiff <= 0.0f;
        for (int i = 0; i < maxPartials; ++i)
        {
            const float k = (float) (i + 1);
            out.ratio[(size_t) i] = stiff > 0.0f ? k * std::sqrt (1.0f + stiff * k * k) : k;
            out.amp[(size_t) i] = i < count ? amp[i] * norm : 0.0f;
            out.phase[(size_t) i] = i < count ? wrapPhase (ph[i]) : 0.0f;
        }
    }

    juce::String pctText (float v, int)  { return juce::String ((int) std::round (v)) + " %"; }
    juce::String dbText (float v, int)   { return juce::String (v, 1) + " dB"; }
}

//==============================================================================
struct SculptModel::Impl
{
    explicit Impl (juce::AudioProcessorValueTreeState& s) : apvts (s)
    {
        pMorph       = apvts.getRawParameterValue ("scMorph");
        pTilt        = apvts.getRawParameterValue ("scTilt");
        pOddEven     = apvts.getRawParameterValue ("scOddEven");
        pFormantPos  = apvts.getRawParameterValue ("scFormantPos");
        pFormantGain = apvts.getRawParameterValue ("scFormantGain");
        pFormantW    = apvts.getRawParameterValue ("scFormantWidth");
        pStretch     = apvts.getRawParameterValue ("scStretch");
        pPartials    = apvts.getRawParameterValue ("scPartials");
        pComb        = apvts.getRawParameterValue ("scComb");
        pCombOffset  = apvts.getRawParameterValue ("scCombOffset");
        pPhase       = apvts.getRawParameterValue ("scPhase");

        for (int i = 0; i < lutSize; ++i)
            sinLut[(size_t) i] = std::sin (twoPi * (float) i / (float) lutSize);

        msgTables.t[0] = SculptModel::makeWave (defaultA);
        msgTables.t[1] = SculptModel::makeWave (defaultB);
        for (auto& sl : slots)
            sl = msgTables;
    }

    static float load (std::atomic<float>* p, float fallback) noexcept { return p != nullptr ? p->load (std::memory_order_relaxed) : fallback; }

    MacroValues readMacros() const noexcept
    {
        MacroValues m;
        m.morph        = load (pMorph, 0.0f);
        m.tilt         = load (pTilt, 0.0f);
        m.oddEven      = load (pOddEven, 0.0f);
        m.formantPos   = load (pFormantPos, 6.0f);
        m.formantGain  = load (pFormantGain, 0.0f);
        m.formantWidth = load (pFormantW, 1.0f);
        m.stretch      = load (pStretch, 0.0f);
        m.partials     = load (pPartials, 64.0f);
        m.comb         = load (pComb, 0.0f);
        m.combOffset   = load (pCombOffset, 25.0f);
        m.phaseMode    = (int) std::round (load (pPhase, 0.0f));
        return m;
    }

    // message thread -> audio thread (triple buffer: never blocks either side)
    void publishTables()
    {
        slots[(size_t) back] = msgTables;
        back = middle.exchange (back | dirtyBit, std::memory_order_acq_rel) & 3;
    }

    void writeState()
    {
        auto child = apvts.state.getOrCreateChildWithName ("Sculpt", nullptr);
        child.setProperty ("tableA", SculptModel::tableToString (msgTables.t[0]), nullptr);
        child.setProperty ("tableB", SculptModel::tableToString (msgTables.t[1]), nullptr);
    }

    void pushUndo()
    {
        undoStack.push_back (msgTables);
        if (undoStack.size() > maxUndo)
            undoStack.erase (undoStack.begin());
        redoStack.clear();
    }

    static constexpr int defaultA = 17;   // "Soft Saw"
    static constexpr int defaultB = 13;   // "Hollow"
    static constexpr int dirtyBit = 4;

    juce::AudioProcessorValueTreeState& apvts;
    std::atomic<float>* pMorph = nullptr;
    std::atomic<float>* pTilt = nullptr;
    std::atomic<float>* pOddEven = nullptr;
    std::atomic<float>* pFormantPos = nullptr;
    std::atomic<float>* pFormantGain = nullptr;
    std::atomic<float>* pFormantW = nullptr;
    std::atomic<float>* pStretch = nullptr;
    std::atomic<float>* pPartials = nullptr;
    std::atomic<float>* pComb = nullptr;
    std::atomic<float>* pCombOffset = nullptr;
    std::atomic<float>* pPhase = nullptr;

    std::array<float, lutSize> sinLut {};

    // message thread
    TablePair msgTables;
    std::vector<TablePair> undoStack, redoStack;
    int back = 2;

    // shared
    std::array<TablePair, 3> slots;
    std::atomic<int> middle { 1 };

    // audio thread
    int front = 0;
    bool first = true;
    MacroValues lastMacros;
    PartialSet audio;
    uint32_t versionCounter = 0;

    // display copy (audio thread writes, seqlock readers)
    std::atomic<uint32_t> seq { 0 };
    PartialSet display;
    std::atomic<uint32_t> lastProcessMs { 0 };
};

//==============================================================================
void SculptModel::addParameters (ParamList& p)
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "scMorph", 1 }, "Sculpt Morph A-B",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, Attr().withStringFromValueFunction (pctText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "scTilt", 1 }, "Sculpt Tilt",
        NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
        Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB/oct"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "scOddEven", 1 }, "Sculpt Odd/Even",
        NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f,
        Attr().withStringFromValueFunction ([] (float v, int)
        {
            const int r = (int) std::round (v);
            return r == 0 ? String ("Balanced") : r < 0 ? String (-r) + " % odd" : String (r) + " % even";
        })));

    NormalisableRange<float> posRange (1.0f, 48.0f, 0.01f);
    posRange.setSkewForCentre (8.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "scFormantPos", 1 }, "Sculpt Formant Position",
        posRange, 6.0f, Attr().withStringFromValueFunction ([] (float v, int) { return "H " + String (v, v < 10.0f ? 1 : 0); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "scFormantGain", 1 }, "Sculpt Formant Gain",
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, Attr().withStringFromValueFunction (dbText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "scFormantWidth", 1 }, "Sculpt Formant Width",
        NormalisableRange<float> (0.2f, 4.0f, 0.01f), 1.0f,
        Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 2) + " oct"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "scStretch", 1 }, "Sculpt Stretch",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, Attr().withStringFromValueFunction (pctText)));

    NormalisableRange<float> partRange (1.0f, 64.0f, 0.0f);
    partRange.setSkewForCentre (16.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "scPartials", 1 }, "Sculpt Partials",
        partRange, 64.0f, Attr().withStringFromValueFunction ([] (float v, int) { return String ((int) std::round (v)) + " partials"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "scComb", 1 }, "Sculpt Comb",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, Attr().withStringFromValueFunction (pctText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "scCombOffset", 1 }, "Sculpt Comb Offset",
        NormalisableRange<float> (1.0f, 50.0f, 0.1f), 25.0f,
        Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " % cyc"; })));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "scPhase", 1 }, "Sculpt Phase",
        StringArray { "Keep", "Aligned", "Spread" }, 0));
}

//==============================================================================
juce::StringArray SculptModel::waveNames()
{
    return { "Sine", "Saw", "Square", "Triangle", "Pulse 25%", "Pulse 10%", "Organ (drawbars)", "Clarinet",
             "Brass", "Strings", "Vowel Ah", "Vowel Oo", "Bell", "Hollow", "Bright Saw", "Soft Square",
             "Sub Heavy", "Soft Saw", "Vowel Ee" };
}

sculpt::BaseTable SculptModel::makeWave (int index)
{
    BaseTable t;
    auto set = [&t] (int k, float a, float ph = 0.0f)
    {
        if (k < 1 || k > maxPartials)
            return;
        if (a < 0.0f) { a = -a; ph += pi; }
        t.amp[(size_t) (k - 1)] = a;
        t.phase[(size_t) (k - 1)] = wrapPhase (ph);
    };
    // smooth spectral envelope of vowel formants (centres in harmonic numbers for f0 ~ 130 Hz)
    auto formants = [] (float k, std::initializer_list<std::pair<float, float>> fs)
    {
        float e = 0.0f;
        for (auto [c, g] : fs)
        {
            const float d = std::log2 (k / c) / 0.22f;
            e += g * std::exp (-0.5f * d * d);
        }
        return e;
    };
    auto pulse = [&set] (float width)
    {
        for (int k = 1; k <= maxPartials; ++k)
            set (k, std::sin (pi * (float) k * width) / (float) k, halfPi);
    };

    switch (index)
    {
        case 0:  set (1, 1.0f); break;
        case 1:  for (int k = 1; k <= maxPartials; ++k) set (k, 1.0f / (float) k); break;
        case 2:  for (int k = 1; k <= maxPartials; k += 2) set (k, 1.0f / (float) k); break;
        case 3:  for (int k = 1; k <= maxPartials; k += 2) set (k, ((k / 2) % 2 == 0 ? 1.0f : -1.0f) / (float) (k * k)); break;
        case 4:  pulse (0.25f); break;
        case 5:  pulse (0.10f); break;
        case 6:  // drawbars 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1' (no 16': the table cannot go below the fundamental)
        {
            const std::pair<int, float> bars[] = { { 1, 1.0f }, { 2, 0.8f }, { 3, 0.7f }, { 4, 0.45f }, { 5, 0.2f }, { 6, 0.3f }, { 8, 0.35f } };
            for (auto [k, a] : bars)
                set (k, a, 0.3f * (float) k);
            break;
        }
        case 7:  // clarinet: strong odd harmonics, faint evens, falling off above ~H11
            for (int k = 1; k <= maxPartials; ++k)
                set (k, (k % 2 == 1 ? 1.0f / std::pow ((float) k, 0.7f) : 0.04f / (float) k)
                            / (1.0f + std::pow ((float) k / 12.0f, 3.0f)));
            break;
        case 8:  // brass: rising to a peak around H3-H5, long bright tail
            for (int k = 1; k <= maxPartials; ++k)
                set (k, (float) k * std::exp (-(float) k / 3.5f) + 0.15f / (float) k);
            break;
        case 9:  // bowed string: saw with body resonances
            for (int k = 1; k <= maxPartials; ++k)
            {
                const float kf = (float) k;
                set (k, (1.0f / kf) * (0.6f + formants (kf, { { 4.0f, 0.7f }, { 11.0f, 0.5f }, { 24.0f, 0.3f } }))
                           / (1.0f + std::pow (kf / 30.0f, 2.0f)));
            }
            break;
        case 10: case 11: case 18:   // vowels Ah / Oo / Ee
        {
            const bool ah = index == 10, oo = index == 11;
            for (int k = 1; k <= maxPartials; ++k)
            {
                const float kf = (float) k;
                const float env = ah ? formants (kf, { { 5.6f, 1.0f }, { 8.4f, 0.7f }, { 18.8f, 0.25f } })
                                : oo ? formants (kf, { { 2.3f, 1.0f }, { 6.7f, 0.3f }, { 17.0f, 0.08f } })
                                     : formants (kf, { { 2.1f, 1.0f }, { 17.5f, 0.45f }, { 23.0f, 0.3f } });
                set (k, (0.05f + env) / std::sqrt (kf));
            }
            break;
        }
        case 12: // bell: sparse partials (add STRETCH for the inharmonic ring)
        {
            const std::pair<int, float> ps[] = { { 1, 0.55f }, { 2, 1.0f }, { 3, 0.45f }, { 4, 0.3f }, { 5, 0.5f },
                                                 { 7, 0.22f }, { 8, 0.3f }, { 11, 0.2f }, { 13, 0.14f }, { 17, 0.08f } };
            for (auto [k, a] : ps)
                set (k, a, 0.9f * (float) k);
            break;
        }
        case 13: // hollow: odd-only with a strong 3rd, flute/organ-like
            for (int k = 1; k <= maxPartials; k += 2)
                set (k, (k == 3 ? 1.8f : 1.0f) / std::pow ((float) k, 1.25f));
            break;
        case 14: for (int k = 1; k <= maxPartials; ++k) set (k, 1.0f / std::pow ((float) k, 0.7f)); break;
        case 15: for (int k = 1; k <= maxPartials; k += 2) set (k, std::exp (-(float) k / 10.0f) / (float) k); break;
        case 16: // sub heavy: a big fundamental with a short, warm overtone tail
            set (1, 1.0f);
            for (int k = 2; k <= maxPartials; ++k)
                set (k, 0.35f / std::pow ((float) k - 0.5f, 1.6f));
            break;
        case 17: // soft saw: 1/k with a gentle rolloff above H16
        default:
            for (int k = 1; k <= maxPartials; ++k)
                set (k, (1.0f / (float) k) / std::sqrt (1.0f + std::pow ((float) k / 16.0f, 2.0f)));
            break;
    }
    return normalised (t);
}

//==============================================================================
juce::String SculptModel::tableToString (const sculpt::BaseTable& t)
{
    int last = 0;
    for (int i = 0; i < maxPartials; ++i)
        if (t.amp[(size_t) i] > 0.00005f)
            last = i + 1;

    juce::String s ("S1");
    for (int i = 0; i < last; ++i)
    {
        const float a = t.amp[(size_t) i];
        s << ' ';
        if (a <= 0.00005f)
            s << '0';
        else
            s << juce::String (a, 4) << ',' << juce::String (t.phase[(size_t) i], 3);
    }
    return s;
}

bool SculptModel::tableFromString (const juce::String& text, sculpt::BaseTable& out)
{
    juce::StringArray tokens;
    tokens.addTokens (text.trim(), " ", "");
    tokens.removeEmptyStrings();
    if (tokens.isEmpty() || tokens[0] != "S1")
        return false;

    BaseTable t;
    for (int i = 1; i < tokens.size() && i <= maxPartials; ++i)
    {
        const auto& tok = tokens[i];
        const int comma = tok.indexOfChar (',');
        const float a = (comma < 0 ? tok : tok.substring (0, comma)).getFloatValue();
        const float ph = comma < 0 ? 0.0f : tok.substring (comma + 1).getFloatValue();
        if (! std::isfinite (a) || ! std::isfinite (ph))
            return false;
        t.amp[(size_t) (i - 1)] = juce::jlimit (0.0f, 1.0f, a);
        t.phase[(size_t) (i - 1)] = wrapPhase (ph);
    }
    out = t;
    return true;
}

//==============================================================================
SculptModel::SculptModel (juce::AudioProcessorValueTreeState& state)
    : impl (std::make_unique<Impl> (state))
{
    reloadFromState();
}

SculptModel::~SculptModel() = default;

void SculptModel::process() noexcept
{
    auto& s = *impl;
    s.lastProcessMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);

    bool changed = s.first;
    if ((s.middle.load (std::memory_order_acquire) & Impl::dirtyBit) != 0)
    {
        s.front = s.middle.exchange (s.front, std::memory_order_acq_rel) & 3;
        changed = true;
    }
    const auto m = s.readMacros();
    if (! (m == s.lastMacros))
        changed = true;
    if (! changed)
        return;

    s.first = false;
    s.lastMacros = m;
    computeFinal (s.slots[(size_t) s.front], m, s.sinLut.data(), s.audio);
    s.audio.version = ++s.versionCounter;

    // seqlock publish of the display copy
    const auto q = s.seq.load (std::memory_order_relaxed);
    s.seq.store (q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);
    s.display = s.audio;
    s.seq.store (q + 2, std::memory_order_release);
}

const sculpt::PartialSet& SculptModel::audioPartials() const noexcept
{
    return impl->audio;
}

void SculptModel::copyDisplayPartials (sculpt::PartialSet& out) const noexcept
{
    auto& s = *impl;

    // with no audio running (some hosts stop processing) compute the display directly so the
    // editor still reacts to every edit
    const auto now = juce::Time::getMillisecondCounter();
    if (now - s.lastProcessMs.load (std::memory_order_relaxed) > 300 && juce::MessageManager::existsAndIsCurrentThread())
    {
        computeFinal (s.msgTables, s.readMacros(), s.sinLut.data(), out);
        uint32_t h = 2166136261u;
        for (int i = 0; i < out.count; ++i)
            h = (h ^ (uint32_t) std::lround (out.amp[(size_t) i] * 1.0e6f + out.phase[(size_t) i] * 1.0e3f + out.ratio[(size_t) i] * 7.0f)) * 16777619u;
        out.version = h | 0x80000000u;
        return;
    }

    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const auto q1 = s.seq.load (std::memory_order_acquire);
        if ((q1 & 1u) != 0)
            continue;
        out = s.display;
        std::atomic_thread_fence (std::memory_order_acquire);
        if (s.seq.load (std::memory_order_relaxed) == q1)
            return;
    }
}

sculpt::BaseTable SculptModel::getTable (int which) const
{
    return impl->msgTables.t[which == 1 ? 1 : 0];
}

void SculptModel::setTable (int which, const sculpt::BaseTable& table, bool undoable)
{
    auto& s = *impl;
    if (undoable)
        s.pushUndo();

    auto& dst = s.msgTables.t[which == 1 ? 1 : 0];
    for (int i = 0; i < maxPartials; ++i)
    {
        const float a = table.amp[(size_t) i], ph = table.phase[(size_t) i];
        dst.amp[(size_t) i] = std::isfinite (a) ? juce::jlimit (0.0f, 1.0f, a) : 0.0f;
        dst.phase[(size_t) i] = std::isfinite (ph) ? wrapPhase (ph) : 0.0f;
    }
    s.writeState();
    s.publishTables();
}

bool SculptModel::undo()
{
    auto& s = *impl;
    if (s.undoStack.empty())
        return false;
    s.redoStack.push_back (s.msgTables);
    s.msgTables = s.undoStack.back();
    s.undoStack.pop_back();
    s.writeState();
    s.publishTables();
    return true;
}

bool SculptModel::redo()
{
    auto& s = *impl;
    if (s.redoStack.empty())
        return false;
    s.undoStack.push_back (s.msgTables);
    s.msgTables = s.redoStack.back();
    s.redoStack.pop_back();
    s.writeState();
    s.publishTables();
    return true;
}

void SculptModel::reloadFromState()
{
    auto& s = *impl;
    const auto child = s.apvts.state.getChildWithName ("Sculpt");
    bool complete = child.isValid();
    const char* keys[] = { "tableA", "tableB" };
    const int defaults[] = { Impl::defaultA, Impl::defaultB };
    for (int i = 0; i < 2; ++i)
    {
        BaseTable t;
        if (child.isValid() && tableFromString (child.getProperty (keys[i]).toString(), t))
            s.msgTables.t[i] = t;
        else
        {
            s.msgTables.t[i] = makeWave (defaults[i]);
            complete = false;
        }
    }
    s.undoStack.clear();
    s.redoStack.clear();
    if (! complete)
        s.writeState();
    s.publishTables();
}

juce::String SculptModel::describeMapping() const
{
    auto& s = *impl;
    const auto m = s.readMacros();
    sculpt::PartialSet ps;
    copyDisplayPartials (ps);

    int active = 0;
    double e = 0.0, ek = 0.0;
    for (int i = 0; i < ps.count; ++i)
    {
        const double a = ps.amp[(size_t) i];
        if (a > 1.0e-4)
            ++active;
        e += a * a;
        ek += a * a * (i + 1);
    }
    const double rms = std::sqrt (e * 0.5);
    const double crestDb = rms > 1.0e-9 ? 20.0 * std::log10 (1.0 / rms) : 0.0;

    juce::String txt;
    const int morph = (int) std::round (m.morph);
    if (morph <= 0)        txt << "Playing table A only (morph 0 %). ";
    else if (morph >= 100) txt << "Playing table B only (morph 100 %). ";
    else                   txt << "Morphing " << morph << " % from table A toward B. ";

    if (std::abs (m.tilt) > 0.05f)
        txt << "Tilt " << juce::String (m.tilt, 1) << " dB/oct (H2 " << juce::String (m.tilt, 1) << " dB, H8 "
            << juce::String (m.tilt * 3.0f, 1) << " dB). ";
    if (std::abs (m.oddEven) > 0.5f)
        txt << (m.oddEven < 0 ? "Even harmonics x " + juce::String (1.0f + m.oddEven * 0.01f, 2)
                              : "Odd harmonics above H1 x " + juce::String (1.0f - m.oddEven * 0.01f, 2)) << ". ";
    if (std::abs (m.formantGain) > 0.01f)
        txt << "Formant " << juce::String (m.formantGain, 1) << " dB at harmonic " << juce::String (m.formantPos, 1)
            << " (" << juce::String (m.formantWidth, 2) << " oct wide). ";
    if (m.comb > 0.05f)
        txt << "Comb " << (int) std::round (m.comb) << " % at offset " << juce::String (m.combOffset, 1) << " % of a cycle. ";
    txt << active << " of 64 partials sound";
    if (m.partials < 63.5f)
        txt << " (limit " << (int) std::round (m.partials) << ")";
    txt << ". ";
    if (ps.harmonic)
        txt << "Harmonic: partial k sits exactly at k x f0. ";
    else
        txt << "Stretched (B = " << juce::String (stretchCoefficient (m.stretch), 5) << "): partial 8 at "
            << juce::String (ps.ratio[7], 2) << " x f0, partial 16 at " << juce::String (ps.ratio[15], 2) << " x f0. ";
    const char* phaseNames[] = { "Keep (table phases)", "Aligned (all peak together)", "Spread (smooth crest)" };
    txt << "Phase: " << phaseNames[juce::jlimit (0, 2, m.phaseMode)] << ". ";
    txt << "Crest " << juce::String (crestDb, 1) << " dB, brightness centroid H" << juce::String (e > 0.0 ? ek / e : 0.0, 1) << ".";
    return txt;
}

//==============================================================================
HelpList SculptModel::helpEntries()
{
    HelpList h;
    auto add = [&h] (const char* id, const char* title, const char* summary, const char* mapping, const char* tips = "")
    {
        h.push_back ({ id, title, summary, mapping, tips });
    };

    // ---- parameters ----
    add ("scMorph", "Morph A -> B",
         "Blends the two editable wave tables: 0 % plays table A, 100 % plays table B.",
         "Per harmonic k: amp = ampA + m x (ampB - ampA) with m = MORPH/100. Phase moves along the shortest arc from phaseA to "
         "phaseB, weighted by loudness: w = m x ampB / ((1-m) x ampA + m x ampB), so a silent harmonic simply takes the other table's phase.",
         "Automate it for evolving pads. Edit table B with the A/B selector in the sculptor.");
    add ("scTilt", "Tilt",
         "Tilts the whole spectrum: negative = darker, positive = brighter.",
         "gain_k = 10^(TILT x log2(k) / 20): harmonic 2 changes by TILT dB, harmonic 4 by 2 x TILT, harmonic 8 by 3 x TILT, "
         "harmonic 64 by 6 x TILT. The fundamental never changes. Range -12..+12 dB per octave.",
         "-3 dB/oct softens a saw into a warm pad; +3 dB/oct adds bite.");
    add ("scOddEven", "Odd / Even",
         "Balances odd against even harmonics.",
         "Negative values scale the even harmonics (2, 4, 6...) by 1 + v/100: -100 % removes them (hollow, square/clarinet-like). "
         "Positive values scale the odd harmonics above the fundamental (3, 5, 7...) by 1 - v/100: +100 % leaves the fundamental "
         "plus the evens (octave-like, organ-ish). The fundamental is always kept so the pitch never jumps.",
         "Try -100 % on a saw for a square.");
    add ("scFormantPos", "Formant position",
         "Where the resonant bump sits, measured in harmonic number.",
         "Centre of the bump = harmonic POS (1..48). Because it is relative to the note, the timbre stays the same on every key "
         "(like a wavetable). In Hz the bump sits at POS x f0, e.g. POS 6 at 220 Hz = 1320 Hz.",
         "Sweep it with GAIN at +12 dB for a talking, wah-like vowel.");
    add ("scFormantGain", "Formant gain",
         "Boost (or cut) of the resonant bump. 0 dB = off.",
         "gain_k (dB) = GAIN x exp(-0.5 x (log2(k / POS) / sigma)^2), sigma = WIDTH / 2.355 octaves, so at WIDTH/2 octaves either "
         "side of POS the boost is half of GAIN. Range -24..+24 dB.",
         "Negative gain carves a notch for a thinner, phasey colour.");
    add ("scFormantWidth", "Formant width",
         "How wide the formant bump is, in octaves of harmonic number.",
         "Full width at half the dB gain, 0.2..4 octaves. 0.5 oct = narrow vowel-like peak; 2 oct = broad tone-control style tilt.",
         "Only active when FORMANT GAIN is not 0.");
    add ("scStretch", "Stretch (inharmonic)",
         "Pulls the overtones away from exact multiples: piano stiffness at low settings, bells and metal at high ones.",
         "ratio_k = k x sqrt(1 + B x k^2) with B = 0.01 x (STRETCH/100)^3. 10 % -> B = 0.00001 (H16 at 16.02 x f0, subtle piano); "
         "30 % -> B = 0.00027 (H16 at 16.5 x); 100 % -> B = 0.01 (H8 at 10.2 x, H16 at 30.2 x). Any value above 0 makes the set inharmonic.",
         "Load the Bell wave and add 40-70 % STRETCH. Stretched sets are no longer one repeating cycle; the display shows the first cycle.");
    add ("scPartials", "Partials",
         "How many harmonics play - a brightness limit that never aliases.",
         "Harmonic k gets weight clamp(PARTIALS - k + 1, 0, 1): with 12.5, harmonics 1..12 play fully and 13 at 50 %; "
         "fractional values fade the top partial in smoothly so automation is click-free. Range 1..64.",
         "Low values (3-8) give flute and soft-bass tones from any wave.");
    add ("scComb", "Comb",
         "A comb filter across the harmonics - like pulse-width modulation applied to any wave.",
         "gain_k = (1 - c) + c x |sin(pi x k x d)|, c = COMB/100, d = COMB OFFSET / 100 cycles. That is the spectrum of the wave minus "
         "a copy delayed by d of a cycle, so a saw with COMB 100 % turns into a pulse of width d.",
         "Automate COMB OFFSET slowly for classic PWM movement.");
    add ("scCombOffset", "Comb offset",
         "The comb's delay as a fraction of one cycle (the 'pulse width').",
         "d = OFFSET / 100 cycles (1..50 %). Notches fall on harmonics k = n / d: 50 % removes every even harmonic, 25 % every 4th, "
         "10 % every 10th.",
         "Only active when COMB is above 0.");
    add ("scPhase", "Phase",
         "How the partials are lined up in time. The spectrum is the same; the shape and the punch change.",
         "Keep -> each harmonic uses its table phase (the shape you drew). Aligned -> every phase = 90 deg (cosine), so all partials "
         "peak together at the start of the cycle: maximum crest, buzzy and peaky. Spread -> Schroeder phases phi_k = pi x k^2 / N "
         "(N = partials in use): the flattest wave, about the lowest crest factor, smooth and dense.",
         "Spread gets more level through the limiter; Aligned sounds sharper on plucks.");

    // ---- tools ----
    add ("sc.wave", "The wave",
         "One cycle of the final wave the synth plays (all macros applied), normalised to its peak.",
         "x = time across one cycle (0..360 deg), y = amplitude (peak = 1). In Bend mode drag a point up/down to pull it, in Draw mode "
         "paint a new shape. Every edit is analysed into harmonics 1..64 (DC removed) and written to the active table.",
         "The thin white line is the active table on its own; it only differs from the glow when macros or morph change the sound.");
    add ("sc.bend", "Bend (grab and pull)",
         "Grab any point of the wave and pull it up or down; the neighbourhood follows smoothly.",
         "Adds D x exp(-0.5 x (dx / W)^2) around the grabbed point (dx in cycles, wrapping), where W = bend width (mouse wheel, "
         "0.01..0.25 cycle). D is solved so the grabbed point lands exactly on the mouse after the 64-harmonic, DC-free resynthesis. "
         "One drag = one undo step.",
         "Wheel = width: narrow for a sharp spike (adds high overtones), wide for a soft swell (low overtones).");
    add ("sc.draw", "Draw (freehand)",
         "Paint a brand-new cycle with the mouse.",
         "Each drag segment overwrites the cycle between its end points (512 points per cycle); the painted shape is converted with a "
         "DFT: amp_k = 2|X_k|/N, phase_k = arg(X_k) + 90 deg, for k = 1..64, then scaled so the loudest harmonic = 1.",
         "Sharp corners need many harmonics; the result is the closest 64-harmonic wave, so steps show a little ripple.");
    add ("sc.mode", "Bend / Draw",
         "Chooses what a drag on the wave does.",
         "Bend -> smooth local pull of one point. Draw -> freehand painting.");
    add ("sc.bars", "Harmonic bars (overtones)",
         "Each bar is one harmonic of the active table: bar k = k x f0.",
         "Bar height = amplitude 0..1 (linear, or -60..0 dB with the dB toggle). Drag a bar to set it; shift-drag paints across bars; "
         "right-drag or alt-drag sets phases (top = +180 deg, bottom = -180 deg); double-click zeroes a bar. Hover a bar to see its "
         "sine drawn over the wave. Ghost ticks show the level after the macros.",
         "Bars past the PARTIALS limit are dimmed because they are not played.");
    add ("sc.phases", "Phase lane",
         "One dot per harmonic: its phase from -180 deg (bottom) to +180 deg (top).",
         "Drag to set phases (shift-drag paints across), double-click resets a phase to 0. Phase only changes the shape of the wave, "
         "not the spectrum; it matters when Phase = Keep.");
    add ("sc.components", "Show components",
         "Draws every partial as its own faint sine plus their sum - the superposition made visible.",
         "Each line = amp_k x sin(2 pi x ratio_k x t + phase_k) at the same scale as the wave; their sum is the bright wave.");
    add ("sc.overlay", "Live overlays",
         "Draws the live synth layer (blue) and/or the live input (green) over the sculpted wave.",
         "Synth: the last cycle of the synth layer at its playing pitch. Input: the last cycle of the input at the tracked f0. Each is "
         "shifted so its fundamental lines up with the sculpted wave and normalised to its own peak.",
         "If the synth overlay matches the glow, what you see is what you hear.");
    add ("sc.dbbars", "dB bars",
         "Switches the bar graph between linear and logarithmic height.",
         "Linear: height = amp. dB: height = (20 x log10(amp) + 60) / 60, so -60 dB sits at the floor.",
         "dB view shows quiet high harmonics; linear view matches what you see in the wave.");
    add ("sc.factory", "Load wave",
         "Loads a factory spectrum into the active table (undoable).",
         "Replaces amplitudes and phases of harmonics 1..64 of the active table with the chosen wave, scaled so its loudest harmonic = 1.",
         "Start from a wave close to your goal, then sculpt.");
    add ("sc.ab", "Table A / B",
         "Chooses which table the bars, bend and draw edit.",
         "A = the table heard at MORPH 0 %, B = the table heard at MORPH 100 %.",
         "When you edit B with MORPH at 0 % you will not hear the change - raise MORPH.");
    add ("sc.copy", "Copy table",
         "Copies the active table into the other one (undoable).",
         "Active A -> B or active B -> A, all 64 amplitudes and phases.",
         "Copy A to B, then reshape B for a morph that starts from the same sound.");
    add ("sc.morph", "Morph shortcut",
         "The MORPH macro (scMorph), right in the toolbar.",
         "0 % = table A, 100 % = table B (see MORPH in the panel).");
    add ("sc.normalize", "Normalize",
         "Scales the active table so its loudest harmonic is 1.",
         "amp_k = amp_k / max(amp). The sound's level is normalised anyway; this restores bar resolution.");
    add ("sc.clear", "Clear",
         "Resets the active table to a pure sine (only the fundamental).",
         "amp_1 = 1, all other amps = 0, all phases 0. Build overtones up again with the bars.");
    add ("sc.random", "Randomize",
         "Makes a new random but musical spectrum in the active table.",
         "amp_k = r_k / k^p with p random in 0.5..1.6, r_k random 0.3..1 and about one harmonic in five dropped; phases random 0..360 deg. "
         "The fundamental is always present.");
    add ("sc.smooth", "Smooth",
         "Rounds off the wave's corners (a gentle low-pass on the table). Press again for more.",
         "amp_k = amp_k x exp(-2 pi^2 s^2 k^2) with s = 1/200 cycle: H16 x 0.88, H32 x 0.60, H64 x 0.13 per press.");
    add ("sc.odd", "Odd only",
         "Removes the even harmonics from the active table.",
         "amp_k = 0 for k = 2, 4, 6, ...: a hollow, square/clarinet-like colour.");
    add ("sc.even", "Even only",
         "Removes the odd harmonics above the fundamental.",
         "amp_k = 0 for k = 3, 5, 7, ...; the fundamental stays so the pitch does not jump an octave.");
    add ("sc.invert", "Invert phases",
         "Negates every phase: the cycle plays mirrored in time and upside down.",
         "phase_k = -phase_k. The spectrum (and mostly the sound) stays the same; the shape and its attack change.");
    add ("sc.capture", "Capture input",
         "Grabs the input's current single cycle and turns it into the active table, so the synth starts out sounding like your sound.",
         "Takes up to 8 periods of the input at the tracked f0 (refined +/-1.5 %), Hann-windowed DFT at k x f0 for k = 1..64, phases "
         "shifted so the fundamental starts at 0 deg, loudest harmonic = 1. Needs a stable, voiced note.",
         "Hold a sustained note while you click. Then morph, tilt or bend it.");
    add ("sc.undo", "Undo", "Steps back one table edit (one drag, one button press).", "Up to 64 steps; loading a preset clears the history.");
    add ("sc.redo", "Redo", "Re-applies an undone table edit.", "Available until the next new edit.");
    add ("sc.banner", "Synth layer is off",
         "The sculpted wave is only heard through the synth layer.",
         "[Turn on] sets synOn = on. The input then drives the synth layer at its tracked pitch (or MIDI notes play it).");
    add ("sc.readout", "Readouts",
         "Live facts about the sound.",
         "Note = tracked pitch of the input (name, cents, Hz). Partials = harmonics louder than -80 dB. Crest = peak/RMS of the wave in dB "
         "(higher = spikier). Bright = loudness-weighted mean harmonic number sum(k a^2)/sum(a^2).");
    return h;
}
