#include "SoundEquation.h"
#include "Expr/Expression.h"
#include "DspUtil.h"
#include "UI/Theme.h"

namespace
{
    // Order matters: this is the index order of the values passed to Expression::evaluate
    enum Var { vX, vL, vR, vM, vS, vCh, vT, vF, vP, vQ, vY1, vA, vB, vC, vD, vSr, numVars };

    const juce::StringArray& variableNames()
    {
        static const juce::StringArray names { "x", "l", "r", "m", "s", "ch", "t", "f", "p", "q", "y1", "a", "b", "c", "d", "sr" };
        return names;
    }

    const char* variableHelp[] = {
        "x  = this channel's input sample (after Drive), roughly -1..1",
        "l, r = left and right input samples (after Drive)",
        "",
        "m  = mid = (l + r) / 2",
        "s  = side = (l - r) / 2",
        "ch = channel being computed: 0 = left, 1 = right",
        "t  = seconds since the stage was switched on (restarts every 10 minutes)",
        "f  = tracked fundamental pitch of the input in Hz (0 when no clear pitch)",
        "p  = phase in radians that advances at f (pitch-locked oscillator; sin(p) follows the note)",
        "q  = phase in radians of a built-in oscillator at 20 * 500^a Hz (dial A sets 20 Hz .. 10 kHz)",
        "y1 = this channel's previous OUTPUT sample (feedback / recursion)",
        "a, b, c, d = the four dials, each 0..1",
        "", "", "",
        "sr = sample rate in Hz",
    };

    constexpr double phaseWrap = juce::MathConstants<double>::twoPi * 16.0;   // keeps sin(k*p) continuous for k in 1/16 steps
    constexpr double timeWrap = 600.0;

    struct Preset
    {
        const char* name;
        const char* formula;
        const char* dial[4];       // knob label in this preset ("" = unused)
        const char* meaning[4];    // what the dial does, for help/inspector
        float defaults[4];
        const char* about;
    };

    const Preset presets[] = {
        { "Harmonic adder (Chebyshev)",
          "x + a*2*x^2 + b*(4*x^3 - 3*x) + c*(8*x^4 - 8*x^2) + d*(16*x^5 - 20*x^3 + 5*x)",
          { "2ND HARM", "3RD HARM", "4TH HARM", "5TH HARM" },
          { "amount of 2nd harmonic (Chebyshev T2)", "amount of 3rd harmonic (T3)", "amount of 4th harmonic (T4)", "amount of 5th harmonic (T5)" },
          { 0.3f, 0.2f, 0.0f, 0.0f },
          "Chebyshev polynomial T_n turns a full-scale sine into exactly its n-th harmonic, so each dial adds one overtone (quieter input = weaker overtones)." },
        { "Wavefold",
          "sin(pi/2 * (x*(1 + 9*a) + 2*b - 1))",
          { "FOLDS", "SYMMETRY", "", "" },
          { "gain into the sine fold: 1x to 10x (more folds = brighter)", "offset before folding: 0.5 = symmetric, else even harmonics", "", "" },
          { 0.3f, 0.5f, 0.0f, 0.0f },
          "Pushes the wave through a sine so peaks fold back down instead of clipping." },
        { "Ring modulator",
          "x * (1 - b + b*sin(q))",
          { "CARRIER", "DEPTH", "", "" },
          { "carrier frequency 20 * 500^a Hz (20 Hz .. 10 kHz), drives q", "how much of the ring modulation (0 = dry, 1 = full ring)", "", "" },
          { 0.45f, 1.0f, 0.0f, 0.0f },
          "Multiplies the sound by a sine carrier: each partial splits into sum and difference frequencies." },
        { "Pitch-locked ring",
          "x * (1 - b + b*sin(p * round(1 + 15*a) / 4))",
          { "RATIO", "DEPTH", "", "" },
          { "carrier = ratio x tracked pitch, ratio = round(1 + 15a) / 4 (0.25 .. 4 in quarter steps)", "how much of the ring modulation", "", "" },
          { 0.2f, 1.0f, 0.0f, 0.0f },
          "Ring modulation whose carrier follows the played note, so it stays harmonic (ratio 1 = octave + DC, 0.5 = sub)." },
        { "Phase distortion",
          "sin(asin(clamp(x, -1, 1)) + a*pi*sin((1 + round(3*b)) * asin(clamp(x, -1, 1))))",
          { "AMOUNT", "HARMONIC", "", "" },
          { "how far the phase is bent (0 = clean)", "which harmonic bends the phase: 1 + round(3b) = 1..4", "", "" },
          { 0.3f, 0.0f, 0.0f, 0.0f },
          "Reads the wave's phase with asin, bends it, and rebuilds the wave with sin (Casio CZ-style)." },
        { "Self-FM feedback",
          "sin(pi/2 * x * (1 + 3*a) + 3*b*y1)",
          { "DRIVE", "FEEDBACK", "", "" },
          { "gain into the sine: 1x to 4x", "how much of the previous output sample is fed back into the phase (y1)", "", "" },
          { 0.2f, 0.3f, 0.0f, 0.0f },
          "The output feeds back into its own phase one sample later, giving FM-like grit and chaos at high feedback." },
        { "Stereo rotation",
          "ch < 0.5 ? l*cos(pi*(2*a - 1)) - r*sin(pi*(2*a - 1)) : l*sin(pi*(2*a - 1)) + r*cos(pi*(2*a - 1))",
          { "ANGLE", "", "", "" },
          { "rotation of the (L, R) pair: angle = (2a - 1) x 180 degrees; 0.5 = no change, 0.25 = -90", "", "", "" },
          { 0.5f, 0.0f, 0.0f, 0.0f },
          "Treats left/right as a 2D point and rotates it, which swaps, inverts or blends the channels (visible in XY view)." },
        { "Quantize steps",
          "lerp(round(x * (1 + 31*(1 - a))) / (1 + 31*(1 - a)), x, b)",
          { "COARSE", "BLEND", "", "" },
          { "step size: levels per unit = 1 + 31(1 - a) (0 = 32 fine steps, 1 = 1 step)", "blend back toward the smooth input", "", "" },
          { 0.6f, 0.0f, 0.0f, 0.0f },
          "Rounds the wave to a staircase of amplitude levels (lo-fi bit reduction you can see)." },
        { "Rectify + curve",
          "pow(abs(x), pow(4, 2*b - 1)) * lerp(sign(x), 1, a)",
          { "RECTIFY", "CURVE", "", "" },
          { "0 = keep sign, 1 = full-wave rectified (octave up)", "exponent 4^(2b - 1): below 0.5 compresses, above 0.5 expands; 0.5 = linear", "", "" },
          { 0.0f, 0.5f, 0.0f, 0.0f },
          "Power-law curve on the magnitude, with optional rectification that folds negative half-waves up." },
        { "Tube bias (asymmetric)",
          "tanh((x + 0.5*a) * (1 + 6*b)) - tanh(0.5*a * (1 + 6*b))",
          { "BIAS", "DRIVE", "", "" },
          { "DC bias before the tanh (0 .. 0.5): more = more even harmonics", "drive into the tanh: 1x to 7x", "", "" },
          { 0.3f, 0.3f, 0.0f, 0.0f },
          "Biased soft clipping like a tube stage; the bias makes the two half-waves clip differently." },
    };

    constexpr int numPresets = (int) (sizeof (presets) / sizeof (presets[0]));
    constexpr int customIndex = numPresets;

    // true if `name` appears as an identifier in the formula text
    bool usesName (const juce::String& text, const juce::String& name)
    {
        const int len = text.length();
        int i = 0;
        while (i < len)
        {
            const auto ch = text[i];
            if (juce::CharacterFunctions::isLetter (ch) || ch == '_')
            {
                int j = i;
                while (j < len && (juce::CharacterFunctions::isLetterOrDigit (text[j]) || text[j] == '_')) ++j;
                if (text.substring (i, j) == name)
                    return true;
                i = j;
            }
            else if (juce::CharacterFunctions::isDigit (ch))
            {
                while (i < len && (juce::CharacterFunctions::isLetterOrDigit (text[i]) || text[i] == '.')) ++i;
            }
            else
                ++i;
        }
        return false;
    }

    juce::String dialText (float v, int) { return juce::String (v, 2); }
    juce::String dbText (float v, int)   { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }

    const char* dialIds[] = { "eqA", "eqB", "eqC", "eqD" };
}

//==============================================================================
struct SoundEquationStage::Impl
{
    explicit Impl (juce::AudioProcessorValueTreeState& s) : apvts (s)
    {
        pOn = apvts.getRawParameterValue ("eqOn");
        pPreset = apvts.getRawParameterValue ("eqPreset");
        for (int i = 0; i < 4; ++i)
            pDial[i] = apvts.getRawParameterValue (dialIds[i]);
        pDrive = apvts.getRawParameterValue ("eqDrive");
        pMix = apvts.getRawParameterValue ("eqMix");
        pOut = apvts.getRawParameterValue ("eqOut");
        pSafety = apvts.getRawParameterValue ("eqSafety");

        for (auto& p : presets)
        {
            juce::String err;
            presetPrograms.push_back (expr::Expression::compile (p.formula, variableNames(), err));
            jassert (presetPrograms.back() != nullptr);   // a preset formula failed to compile
        }

        juce::String err;
        customOwner = expr::Expression::compile ("x", variableNames(), err);
        customRaw.store (customOwner.get());
    }

    const expr::Expression* activeProgram() const noexcept
    {
        const int idx = (int) pPreset->load();
        if (idx >= 0 && idx < numPresets)
            return presetPrograms[(size_t) idx].get();
        return customRaw.load (std::memory_order_acquire);
    }

    void purgeRetired()
    {
        const auto now = juce::Time::getMillisecondCounter();
        retired.erase (std::remove_if (retired.begin(), retired.end(),
                                       [now] (const Retired& r) { return now - r.when > 2000; }),
                       retired.end());
    }

    juce::AudioProcessorValueTreeState& apvts;
    std::atomic<float>* pOn;
    std::atomic<float>* pPreset;
    std::atomic<float>* pDial[4];
    std::atomic<float>* pDrive;
    std::atomic<float>* pMix;
    std::atomic<float>* pOut;
    std::atomic<float>* pSafety;

    std::vector<std::shared_ptr<const expr::Expression>> presetPrograms;

    // The audio thread only ever sees the raw pointer; owners live on the message thread and
    // replaced programs are kept for a while so a block still using one never sees it freed.
    std::shared_ptr<const expr::Expression> customOwner;
    std::atomic<const expr::Expression*> customRaw { nullptr };
    struct Retired { std::shared_ptr<const expr::Expression> program; juce::uint32 when; };
    std::vector<Retired> retired;
    juce::String customError;

    double sampleRate = 44100.0;
    juce::SmoothedValue<float> dialSm[4], driveSm, mixSm, outSm;
    float y1[2] {}, dcX[2] {}, dcY[2] {};
    float dcR = 0.999f;
    double time = 0.0, pPhase = 0.0, qPhase = 0.0;
    bool wasOn = false;

    // pitch tracking for f / p
    std::vector<float> pitchRing, pitchLinear;
    int ringPos = 0, sinceEstimate = 0, silentSamples = 0;
    float trackedF = 0.0f;
    std::atomic<float> shownF { 0.0f };
};

//==============================================================================
void SoundEquationStage::addParameters (ParamList& p)
{
    using namespace juce;
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "eqOn", 1 }, "Wave Equation", false));

    StringArray names;
    for (auto& pr : presets)
        names.add (pr.name);
    names.add ("Custom formula");
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "eqPreset", 1 }, "Equation Family", names, 0));

    const char* dialNames[] = { "Equation A", "Equation B", "Equation C", "Equation D" };
    for (int i = 0; i < 4; ++i)
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { dialIds[i], 1 }, dialNames[i],
            NormalisableRange<float> (0.0f, 1.0f, 0.001f), presets[0].defaults[i],
            AudioParameterFloatAttributes().withStringFromValueFunction (dialText)));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "eqDrive", 1 }, "Equation Drive",
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withStringFromValueFunction (dbText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "eqMix", 1 }, "Equation Mix",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String ((int) std::round (v)) + " %"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "eqOut", 1 }, "Equation Output",
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withStringFromValueFunction (dbText)));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "eqSafety", 1 }, "Equation Safety", true));
}

juce::StringArray SoundEquationStage::presetNames()
{
    juce::StringArray names;
    for (auto& pr : presets)
        names.add (pr.name);
    names.add ("Custom formula");
    return names;
}

juce::String SoundEquationStage::presetFormula (int index)
{
    return index >= 0 && index < numPresets ? juce::String (presets[index].formula) : juce::String();
}

SoundEquationStage::SoundEquationStage (juce::AudioProcessorValueTreeState& state)
    : impl (std::make_unique<Impl> (state))
{
    reloadFromState();
}

SoundEquationStage::~SoundEquationStage() = default;

void SoundEquationStage::prepare (double sampleRate, int, int)
{
    auto& s = *impl;
    s.sampleRate = sampleRate;
    for (int i = 0; i < 4; ++i)
    {
        s.dialSm[i].reset (sampleRate, 0.03);
        s.dialSm[i].setCurrentAndTargetValue (s.pDial[i]->load());
    }
    s.driveSm.reset (sampleRate, 0.03);
    s.mixSm.reset (sampleRate, 0.03);
    s.outSm.reset (sampleRate, 0.03);
    s.driveSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (s.pDrive->load()));
    s.mixSm.setCurrentAndTargetValue (s.pMix->load() * 0.01f);
    s.outSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (s.pOut->load()));

    s.dcR = (float) (1.0 - juce::MathConstants<double>::twoPi * 10.0 / sampleRate);   // ~10 Hz DC blocker
    s.pitchRing.assign (4096, 0.0f);
    s.pitchLinear.assign (4096, 0.0f);
    s.ringPos = s.sinceEstimate = s.silentSamples = 0;
    s.trackedF = 0.0f;
    s.wasOn = false;
}

void SoundEquationStage::process (juce::AudioBuffer<float>& buffer, int numChannels) noexcept
{
    auto& s = *impl;
    if (s.pOn->load() < 0.5f)
    {
        s.wasOn = false;
        return;
    }

    const auto* program = s.activeProgram();
    const int n = buffer.getNumSamples();
    if (program == nullptr || n == 0 || numChannels == 0 || s.pitchRing.empty())
        return;

    if (! s.wasOn)
    {
        for (int i = 0; i < 4; ++i)
            s.dialSm[i].setCurrentAndTargetValue (s.pDial[i]->load());
        s.driveSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (s.pDrive->load()));
        s.mixSm.setCurrentAndTargetValue (s.pMix->load() * 0.01f);
        s.outSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (s.pOut->load()));
        s.y1[0] = s.y1[1] = s.dcX[0] = s.dcX[1] = s.dcY[0] = s.dcY[1] = 0.0f;
        s.time = s.pPhase = s.qPhase = 0.0;
        s.wasOn = true;
    }

    for (int i = 0; i < 4; ++i)
        s.dialSm[i].setTargetValue (s.pDial[i]->load());
    s.driveSm.setTargetValue (juce::Decibels::decibelsToGain (s.pDrive->load()));
    s.mixSm.setTargetValue (s.pMix->load() * 0.01f);
    s.outSm.setTargetValue (juce::Decibels::decibelsToGain (s.pOut->load()));
    const bool safety = s.pSafety->load() > 0.5f;

    float* left = buffer.getWritePointer (0);
    float* right = numChannels > 1 ? buffer.getWritePointer (1) : left;
    const int chans = juce::jmin (numChannels, 2);
    const double dt = 1.0 / s.sampleRate;
    const double log500 = std::log (500.0);

    float vars[numVars] {};
    vars[vSr] = (float) s.sampleRate;

    for (int i = 0; i < n; ++i)
    {
        const float drive = s.driveSm.getNextValue();
        const float mix = s.mixSm.getNextValue();
        const float outG = s.outSm.getNextValue();
        for (int k = 0; k < 4; ++k)
            vars[vA + k] = s.dialSm[k].getNextValue();

        const float inL = left[i], inR = right[i];

        // pitch tracking on the raw input, re-estimated every 1024 samples
        s.pitchRing[(size_t) s.ringPos] = 0.5f * (inL + inR);
        s.ringPos = (s.ringPos + 1) & 4095;
        if (++s.sinceEstimate >= 1024)
        {
            s.sinceEstimate = 0;
            for (int k = 0; k < 4096; ++k)
                s.pitchLinear[(size_t) k] = s.pitchRing[(size_t) ((s.ringPos + k) & 4095)];
            const float est = DspUtil::estimateFrequency (s.pitchLinear.data(), 4096, s.sampleRate);
            if (est > 20.0f && est < 5000.0f)
            {
                const bool close = s.trackedF > 0.0f && std::abs (est / s.trackedF - 1.0f) < 0.06f;
                s.trackedF = close ? 0.7f * s.trackedF + 0.3f * est : est;
                s.silentSamples = 0;
            }
            else if ((s.silentSamples += 1024) > (int) (0.4 * s.sampleRate))
                s.trackedF = 0.0f;
        }

        const float l = inL * drive, r = inR * drive;
        vars[vL] = l;
        vars[vR] = r;
        vars[vM] = 0.5f * (l + r);
        vars[vS] = 0.5f * (l - r);
        vars[vT] = (float) s.time;
        vars[vF] = s.trackedF;
        vars[vP] = (float) s.pPhase;
        vars[vQ] = (float) s.qPhase;

        for (int ch = 0; ch < chans; ++ch)
        {
            vars[vX] = ch == 0 ? l : r;
            vars[vCh] = (float) ch;
            vars[vY1] = s.y1[ch];

            float y = program->evaluate (vars);
            if (safety)
            {
                // DC blocker, then a gentle tanh ceiling at about +3.5 dBFS
                const float blocked = y - s.dcX[ch] + s.dcR * s.dcY[ch];
                s.dcX[ch] = y;
                s.dcY[ch] = std::isfinite (blocked) ? blocked : 0.0f;
                y = 1.5f * std::tanh (s.dcY[ch] / 1.5f);
            }
            else
            {
                y = juce::jlimit (-8.0f, 8.0f, y);
            }
            s.y1[ch] = y;

            const float dry = ch == 0 ? inL : inR;
            const float out = (dry + (y - dry) * mix) * outG;
            if (ch == 0) left[i] = out; else right[i] = out;
        }

        s.time += dt;
        if (s.time >= timeWrap) s.time -= timeWrap;
        s.pPhase += juce::MathConstants<double>::twoPi * s.trackedF * dt;
        if (s.pPhase >= phaseWrap) s.pPhase -= phaseWrap;
        s.qPhase += juce::MathConstants<double>::twoPi * 20.0 * std::exp (vars[vA] * log500) * dt;
        if (s.qPhase >= phaseWrap) s.qPhase -= phaseWrap;
    }

    s.shownF = s.trackedF;
}

juce::String SoundEquationStage::setCustomFormula (const juce::String& text)
{
    auto& s = *impl;
    s.apvts.state.setProperty ("eqFormula", text, nullptr);

    juce::String err;
    auto compiled = expr::Expression::compile (text, variableNames(), err);
    s.purgeRetired();
    if (compiled == nullptr)
    {
        s.customError = err;
        return err;   // the previous working formula keeps playing
    }

    s.retired.push_back ({ s.customOwner, juce::Time::getMillisecondCounter() });
    s.customOwner = compiled;
    s.customRaw.store (compiled.get(), std::memory_order_release);
    s.customError.clear();
    return {};
}

juce::String SoundEquationStage::getCustomFormula() const
{
    const auto text = impl->apvts.state.getProperty ("eqFormula").toString();
    return text.isNotEmpty() ? text : juce::String ("x");
}

void SoundEquationStage::reloadFromState()
{
    setCustomFormula (getCustomFormula());
}

juce::String SoundEquationStage::describeMapping() const
{
    auto& s = *impl;
    const int idx = (int) s.pPreset->load();
    const bool custom = idx < 0 || idx >= numPresets;
    const juce::String formula = custom ? getCustomFormula() : juce::String (presets[idx].formula);

    juce::String out;
    out << "Wave equation stage: " << (s.pOn->load() > 0.5f ? "ON - this changes the sound" : "OFF (bypassed, no CPU)") << "\n";
    out << "Family: " << (custom ? juce::String ("Custom formula") : juce::String (presets[idx].name)) << "\n";
    out << "Applied per sample, per channel:  y = " << formula << "\n";
    if (custom && s.customError.isNotEmpty())
        out << "Your latest edit has an error (" << s.customError << "); the last working formula is still playing.\n";
    if (! custom)
        out << presets[idx].about << "\n";

    for (int k = 0; k < 4; ++k)
    {
        const char letter = (char) ('a' + k);
        const float v = s.pDial[k]->load();
        if (custom)
        {
            if (usesName (formula, juce::String::charToString (letter)))
                out << letter << " = " << juce::String (v, 2) << " -> used in your formula as '" << letter << "'\n";
        }
        else if (presets[idx].dial[k][0] != 0)
            out << letter << " = " << juce::String (v, 2) << " -> " << presets[idx].meaning[k] << "\n";
    }

    if (usesName (formula, "q"))
        out << "q runs at 20 * 500^a = " << juce::String (20.0 * std::pow (500.0, (double) s.pDial[0]->load()), 0) << " Hz\n";
    if (usesName (formula, "f") || usesName (formula, "p"))
    {
        const float f = s.shownF.load();
        out << "Tracked pitch f = " << (f > 0.0f ? juce::String (f, 1) + " Hz" : juce::String ("none (no clear pitch)")) << " -> p advances at f\n";
    }

    out << "Input -> x: Drive " << dbText (s.pDrive->load(), 0) << ". Output: " << juce::String ((int) std::round (s.pMix->load()))
        << "% wet, " << dbText (s.pOut->load(), 0) << ". Safety "
        << (s.pSafety->load() > 0.5f ? "ON (DC blocker + soft ceiling near +3.5 dBFS)" : "OFF (only a hard limit at +/-8)") << ".";
    return out;
}

HelpList SoundEquationStage::helpEntries()
{
    HelpList h;

    h.push_back ({ "eqOn", "Wave equation",
        "Switches the wave-equation stage on. It reshapes the actual audio (you hear it).",
        "When on, every output sample is y = f(x, ...) computed from the formula shown in the panel, applied separately to the "
        "left and right channels after the filters and before the double slit. When off it is skipped entirely (no CPU).",
        "Watch the 2D scope with Show = Both to compare input (dim) and output (bright)." });

    juce::String presetMap ("Selects which formula f is applied. Each family is a real formula evaluated by the same engine:\n");
    for (auto& pr : presets)
    {
        presetMap << "- " << pr.name << ":  y = " << pr.formula << ".  ";
        for (int k = 0; k < 4; ++k)
            if (pr.dial[k][0] != 0)
                presetMap << (char) ('A' + k) << " = " << pr.meaning[k] << "; ";
        presetMap << "\n";
    }
    presetMap << "- Custom formula: y = whatever you type in the formula box (variables listed under Variables...). Dials A-D are a, b, c, d.";
    h.push_back ({ "eqPreset", "Equation family", "Chooses the formula that reshapes each sample.", presetMap,
        "Pick a family, then 'Copy preset into editor' to use it as a starting point for your own formula." });

    for (int k = 0; k < 4; ++k)
    {
        juce::String map;
        map << "Dial " << (char) ('A' + k) << " is the variable '" << (char) ('a' + k) << "' (0..1) in the formula; its meaning depends on the family:\n";
        for (auto& pr : presets)
            map << "- " << pr.name << ": " << (pr.dial[k][0] != 0 ? juce::String (pr.meaning[k]) : juce::String ("unused")) << "\n";
        map << "- Custom formula: whatever your formula does with '" << (char) ('a' + k) << "'.";
        if (k == 0)
            map << "\nDial A also always sets the speed of the built-in oscillator phase q: 20 * 500^a Hz.";
        h.push_back ({ dialIds[k], juce::String ("Dial ") + (char) ('A' + k),
            "A tuning dial for the current equation; its label shows what it controls.", map,
            "Changes are smoothed over 30 ms so turning a dial never clicks." });
    }

    h.push_back ({ "eqDrive", "Drive (into the formula)", "Gain applied to the input before the formula sees it.",
        "x, l, r, m and s = input sample x 10^(Drive/20). +6 dB doubles x, so shapers bite harder. The dry signal used by Mix is NOT driven.",
        "Most families are designed for x around -1..1." });
    h.push_back ({ "eqMix", "Mix", "Blend between the untouched input and the formula output.",
        "out = dry + (y - dry) x Mix/100, then x Output gain. 0% = input only, 100% = formula only.", {} });
    h.push_back ({ "eqOut", "Output", "Gain after the equation stage.", "out x 10^(Output/20).", "Use it to level-match when a family gets louder." });
    h.push_back ({ "eqSafety", "Safety", "Protects your ears and FL from wild formulas.",
        "ON: y passes through a ~10 Hz DC blocker, then 1.5 x tanh(y / 1.5) (a soft ceiling near +3.5 dBFS). "
        "OFF: y is only hard-limited to +/-8. Invalid results (divide by zero, sqrt of a negative) always become 0.",
        "Leave it on unless you need the exact, unprotected formula (e.g. DC-offset tricks)." });

    juce::String vars ("Variables you can use in a formula:\n");
    for (auto* line : variableHelp)
        if (line[0] != 0)
            vars << "- " << line << "\n";
    vars << "Constants: pi, tau, e.\nFunctions: sin cos tan asin acos atan atan2 sinh cosh tanh exp log log2 log10 sqrt abs sign floor ceil "
            "round fract min max clamp(x,lo,hi) pow mod lerp(a,b,t) step(edge,x) smoothstep(a,b,x) sat (soft clip) "
            "tri saw sqr (period 2*pi, -1..1) hypot.\nOperators: + - * / % ^, comparisons (give 1 or 0), && || !, and cond ? a : b.";
    h.push_back ({ "eq.formula", "Formula box", "Type your own wave equation y = f(...); it compiles 0.4 s after you stop typing (or on Enter).",
        "The text is the right-hand side of y = ... and is evaluated for every sample of each channel with the variables below. "
        "If it has an error, the status line says where, and the last working formula keeps playing.\n" + vars,
        "Example: sin(pi/2*x*(1+4*a)) + b*x*sin(p*2)  - a wavefold plus a pitch-locked octave ring." });
    h.push_back ({ "eq.vars", "Variables", "Reference of everything a formula can read.", vars, {} });
    return h;
}

//==============================================================================
namespace
{
    class TransferCurve : public juce::Component
    {
    public:
        void setCurves (std::vector<float> now, std::vector<float> later, bool timeVarying)
        {
            curveNow = std::move (now);
            curveLater = std::move (later);
            showLater = timeVarying;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            using namespace Palette;
            const auto a = getLocalBounds().toFloat();
            g.setColour (viewBg);
            g.fillRoundedRectangle (a, 6.0f);

            const float yRange = 1.5f;
            auto toPoint = [&] (float x, float y)
            {
                return juce::Point<float> (a.getX() + (x + 1.0f) * 0.5f * a.getWidth(),
                                           a.getCentreY() - juce::jlimit (-yRange, yRange, y) / yRange * a.getHeight() * 0.5f);
            };

            g.setColour (grid);
            for (float v : { -1.0f, -0.5f, 0.5f, 1.0f })
            {
                g.drawVerticalLine ((int) toPoint (v, 0).x, a.getY(), a.getBottom());
                g.drawHorizontalLine ((int) toPoint (0, v).y, a.getX(), a.getRight());
            }
            g.setColour (panelEdge.brighter (0.3f));
            g.drawHorizontalLine ((int) a.getCentreY(), a.getX(), a.getRight());
            g.drawVerticalLine ((int) a.getCentreX(), a.getY(), a.getBottom());

            const float dashes[] = { 3.0f, 4.0f };
            g.setColour (dim.withAlpha (0.6f));
            g.drawDashedLine ({ toPoint (-1.0f, -1.0f), toPoint (1.0f, 1.0f) }, dashes, 2, 1.0f);

            auto drawCurve = [&] (const std::vector<float>& c, juce::Colour col, float width)
            {
                if (c.size() < 2) return;
                juce::Path p;
                for (size_t i = 0; i < c.size(); ++i)
                {
                    const float x = -1.0f + 2.0f * (float) i / (float) (c.size() - 1);
                    const auto pt = toPoint (x, c[i]);
                    if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
                }
                g.setColour (col);
                g.strokePath (p, juce::PathStrokeType (width));
            };

            if (showLater)
                drawCurve (curveLater, green.withAlpha (0.3f), 1.2f);
            drawCurve (curveNow, green, 1.8f);

            g.setFont (Theme::font (10.5f));
            g.setColour (dim);
            g.drawText ("transfer curve  y = f(x)   (dashed: y = x)", getLocalBounds().reduced (8, 4), juce::Justification::topLeft);
            if (showLater)
                g.drawText ("faint: same formula a moment later (it changes over time)", getLocalBounds().reduced (8, 4), juce::Justification::bottomLeft);
            g.drawText ("x: -1 .. 1   y: -1.5 .. 1.5", getLocalBounds().reduced (8, 4), juce::Justification::topRight);
        }

    private:
        std::vector<float> curveNow, curveLater;
        bool showLater = false;
    };

    class VariablesView : public juce::Component
    {
    public:
        explicit VariablesView (const juce::String& text)
        {
            editor.setMultiLine (true, true);
            editor.setReadOnly (true);
            editor.setScrollbarsShown (true);
            editor.setFont (Theme::font (12.5f));
            editor.setColour (juce::TextEditor::backgroundColourId, Palette::panel);
            editor.setColour (juce::TextEditor::textColourId, Palette::text);
            editor.setColour (juce::TextEditor::outlineColourId, Palette::panelEdge);
            editor.setText (text, false);
            addAndMakeVisible (editor);
            setSize (460, 340);
        }
        void resized() override { editor.setBounds (getLocalBounds()); }

    private:
        juce::TextEditor editor;
    };
}

struct SoundEquationPanel::Impl : private juce::Timer
{
    Impl (SoundEquationPanel& o, juce::AudioProcessorValueTreeState& s, SoundEquationStage& st, HelpTarget* h)
        : owner (o), apvts (s), stage (st), help (h),
          enable (s, "eqOn", "Enable", h),
          preset (s, "eqPreset", "EQUATION FAMILY", h),
          dialA (s, "eqA", "A", h), dialB (s, "eqB", "B", h), dialC (s, "eqC", "C", h), dialD (s, "eqD", "D", h),
          drive (s, "eqDrive", "DRIVE", h, true), mix (s, "eqMix", "MIX", h), out (s, "eqOut", "OUTPUT", h, true),
          safety (s, "eqSafety", "Safety", h),
          editorHover (editor, "eq.formula", h), copyHover (copyButton, "eq.formula", h),
          varsHover (varsButton, "eq.vars", h), resetHover (resetButton, "eqPreset", h)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &enable, &preset, &curve, &dialA, &dialB, &dialC, &dialD,
                                                                 &drive, &mix, &out, &safety, &resetButton, &copyButton,
                                                                 &varsButton, &formulaLabel, &editor, &status })
            owner.addAndMakeVisible (c);

        formulaLabel.setText ("FORMULA   y =", juce::dontSendNotification);
        formulaLabel.setFont (Theme::font (11.0f, true));
        formulaLabel.setColour (juce::Label::textColourId, Palette::dim);

        editor.setMultiLine (true, true);
        editor.setReturnKeyStartsNewLine (false);
        editor.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain)));
        editor.setColour (juce::TextEditor::backgroundColourId, Palette::viewBg);
        editor.setColour (juce::TextEditor::textColourId, Palette::text);
        editor.setColour (juce::TextEditor::outlineColourId, Palette::panelEdge);
        editor.setColour (juce::TextEditor::focusedOutlineColourId, Palette::green);
        editor.onTextChange = [this]
        {
            if (! editor.isReadOnly())
            {
                compileAt = juce::Time::getMillisecondCounter() + 400;
                setStatus ("...", Palette::dim);
            }
        };
        editor.onReturnKey = [this] { if (! editor.isReadOnly()) compileNow(); };

        status.setFont (Theme::font (11.0f));
        status.setJustificationType (juce::Justification::topLeft);
        status.setMinimumHorizontalScale (1.0f);

        resetButton.setButtonText ("Reset dials");
        resetButton.onClick = [this] { resetDials(); };
        copyButton.setButtonText ("Copy preset into editor");
        copyButton.onClick = [this] { copyPresetIntoEditor(); };
        varsButton.setButtonText ("Variables...");
        varsButton.onClick = [this] { showVariables(); };

        for (auto* b : { &resetButton, &copyButton, &varsButton })
            b->setColour (juce::TextButton::buttonColourId, Palette::bg);

        refreshMode (true);
        startTimerHz (10);
    }

    ~Impl() override { stopTimer(); }

    int presetIndex() const { return (int) apvts.getRawParameterValue ("eqPreset")->load(); }
    bool isCustom() const   { return presetIndex() >= numPresets; }

    void setStatus (const juce::String& text, juce::Colour colour)
    {
        status.setText (text, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, colour);
    }

    void okStatus()
    {
        const bool on = apvts.getRawParameterValue ("eqOn")->load() > 0.5f;
        setStatus (on ? "OK - compiled and running live" : "OK - compiled. Switch Enable on to hear it.", Palette::green);
    }

    void compileNow()
    {
        compileAt = 0;
        const auto err = stage.setCustomFormula (editor.getText());
        if (err.isEmpty())
            okStatus();
        else
            setStatus ("Error: " + err + "   (the last working formula keeps playing)", Palette::pink);
        previewKey.clear();
    }

    void refreshMode (bool force)
    {
        const int idx = presetIndex();
        if (! force && idx == lastIndex)
            return;
        lastIndex = idx;
        if (idx < numPresets)
            lastPresetIndex = idx;

        ParamKnob* dials[] = { &dialA, &dialB, &dialC, &dialD };
        for (int k = 0; k < 4; ++k)
        {
            const bool custom = idx >= numPresets;
            const juce::String label = custom ? juce::String::charToString ((juce::juce_wchar) ('A' + k)) + " (" + juce::String::charToString ((juce::juce_wchar) ('a' + k)) + ")"
                                              : juce::String (presets[idx].dial[k]);
            dials[k]->label.setText (label.isNotEmpty() ? label : juce::String ("-"), juce::dontSendNotification);
            dials[k]->slider.setEnabled (custom || label.isNotEmpty());
        }
        resetButton.setEnabled (idx < numPresets);

        if (idx >= numPresets)
        {
            editor.setReadOnly (false);
            editor.setAlpha (1.0f);
            editor.setText (stage.getCustomFormula(), false);
            juce::String err;
            if (expr::Expression::compile (editor.getText(), variableNames(), err) == nullptr)
                setStatus ("Error: " + err + "   (the last working formula keeps playing)", Palette::pink);
            else
                okStatus();
            copyButton.setButtonText ("Copy preset into editor");
        }
        else
        {
            editor.setReadOnly (true);
            editor.setAlpha (0.65f);
            editor.setText (presets[idx].formula, false);
            setStatus (juce::String (presets[idx].about) + "\nRead-only preset formula - choose 'Custom formula' or 'Copy preset into editor' to edit it.", Palette::dim);
        }
        previewKey.clear();
    }

    void resetDials()
    {
        const int idx = presetIndex();
        if (idx >= numPresets)
            return;
        for (int k = 0; k < 4; ++k)
            if (auto* p = apvts.getParameter (dialIds[k]))
                p->setValueNotifyingHost (p->convertTo0to1 (presets[idx].defaults[k]));
    }

    void copyPresetIntoEditor()
    {
        const int idx = isCustom() ? lastPresetIndex : presetIndex();
        stage.setCustomFormula (presets[juce::jlimit (0, numPresets - 1, idx)].formula);
        if (auto* p = apvts.getParameter ("eqPreset"))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) customIndex));
        refreshMode (true);
    }

    void showVariables()
    {
        juce::String text ("Write the right-hand side of  y = ...  It runs once per sample for each channel.\n\n");
        for (auto* line : variableHelp)
            if (line[0] != 0)
                text << line << "\n";
        text << "\nConstants: pi  tau  e\n\nFunctions:\n"
                "  sin cos tan asin acos atan atan2(y,x) sinh cosh tanh exp log log2 log10 sqrt abs sign\n"
                "  floor ceil round fract min(a,b) max(a,b) clamp(x,lo,hi) pow(a,b) mod(a,b) hypot(a,b)\n"
                "  lerp(a,b,t) step(edge,x) smoothstep(a,b,x) sat(x) = soft clip\n"
                "  tri(x) saw(x) sqr(x) = triangle / saw / square with period 2*pi, range -1..1\n\n"
                "Operators: + - * / % ^ (power)   < > <= >= == != (give 1 or 0)   && || !   cond ? a : b\n\n"
                "Examples:\n"
                "  sin(pi/2 * x * (1 + 4*a))            wavefold, A = amount\n"
                "  x + a*sin(p*2)*abs(x)                 adds an octave locked to the note\n"
                "  x * (1 - b + b*sin(q))                ring mod, A = carrier, B = depth\n"
                "  sat(x*(1 + 8*a)) * (ch < 0.5 ? 1 : -1) drive with the right channel inverted\n"
                "  lerp(x, y1, a)                        one-pole smoothing (feedback via y1)";
        // inside the editor rather than in a window of its own (UI/StreamSafe.h)
        auto view = std::make_unique<VariablesView> (text);
        juce::Component* host = owner.findParentComponentOfClass<juce::AudioProcessorEditor>();
        if (host == nullptr)
            host = owner.getTopLevelComponent();
        juce::CallOutBox::launchAsynchronously (std::move (view), host->getLocalArea (&varsButton, varsButton.getLocalBounds()), host);
    }

    void updatePreview()
    {
        const int idx = presetIndex();
        const juce::String text = idx < numPresets ? juce::String (presets[idx].formula) : stage.getCustomFormula();

        float dials[4];
        for (int k = 0; k < 4; ++k)
            dials[k] = apvts.getRawParameterValue (dialIds[k])->load();

        juce::String key = text;
        for (float d : dials)
            key << "|" << juce::String (d, 4);
        if (key == previewKey)
            return;
        previewKey = key;

        if (previewText != text || previewProgram == nullptr)
        {
            juce::String err;
            auto compiled = expr::Expression::compile (text, variableNames(), err);
            if (compiled != nullptr)
                previewProgram = compiled;
            previewText = text;
        }
        if (previewProgram == nullptr)
            return;

        constexpr int points = 161;
        std::vector<float> now ((size_t) points), later ((size_t) points);
        float vars[numVars] {};
        for (int k = 0; k < 4; ++k)
            vars[vA + k] = dials[k];
        vars[vSr] = 48000.0f;
        vars[vF] = 220.0f;
        float maxDiff = 0.0f;
        for (int i = 0; i < points; ++i)
        {
            const float x = -1.0f + 2.0f * (float) i / (float) (points - 1);
            vars[vX] = vars[vL] = vars[vR] = vars[vM] = x;
            vars[vS] = 0.0f;
            vars[vCh] = 0.0f;
            vars[vY1] = 0.0f;
            vars[vT] = vars[vP] = vars[vQ] = 0.0f;
            now[(size_t) i] = previewProgram->evaluate (vars);
            vars[vT] = 0.37f;
            vars[vP] = vars[vQ] = 1.3f;
            later[(size_t) i] = previewProgram->evaluate (vars);
            maxDiff = juce::jmax (maxDiff, std::abs (later[(size_t) i] - now[(size_t) i]));
        }
        curve.setCurves (std::move (now), std::move (later), maxDiff > 1.0e-3f);
    }

    void timerCallback() override
    {
        refreshMode (false);
        if (compileAt != 0 && juce::Time::getMillisecondCounter() >= compileAt)
            compileNow();
        updatePreview();
    }

    void layout (juce::Rectangle<int> area)
    {
        auto r = area.reduced (12, 8);
        enable.setBounds (r.removeFromTop (22).removeFromRight (90));
        r.removeFromTop (4);

        auto presetRow = r.removeFromTop (Theme::choiceHeight);
        resetButton.setBounds (presetRow.removeFromRight (96).withTrimmedTop (16).withHeight (24));
        presetRow.removeFromRight (8);
        preset.setBounds (presetRow);
        r.removeFromTop (4);

        curve.setBounds (r.removeFromTop (curveHeight));
        r.removeFromTop (6);

        auto placeRow = [&r] (std::initializer_list<juce::Component*> comps)
        {
            auto row = r.removeFromTop (Theme::knobCellHeight);
            const int w = row.getWidth() / 4;
            for (auto* c : comps)
                c->setBounds (row.removeFromLeft (w).reduced (2));
        };
        placeRow ({ &dialA, &dialB, &dialC, &dialD });
        placeRow ({ &drive, &mix, &out, &safety });
        // centre the safety toggle in its cell
        safety.setBounds (safety.getBounds().withSizeKeepingCentre (safety.getWidth(), 24));
        r.removeFromTop (6);

        auto labelRow = r.removeFromTop (24);
        varsButton.setBounds (labelRow.removeFromRight (90));
        labelRow.removeFromRight (6);
        copyButton.setBounds (labelRow.removeFromRight (160));
        formulaLabel.setBounds (labelRow);
        r.removeFromTop (4);

        editor.setBounds (r.removeFromTop (editorHeight));
        r.removeFromTop (2);
        status.setBounds (r.removeFromTop (statusHeight));
    }

    static constexpr int curveHeight = 130, editorHeight = 72, statusHeight = 34;

    static int heightNeeded()
    {
        return 8 + 22 + 4 + Theme::choiceHeight + 4 + curveHeight + 6 + 2 * Theme::knobCellHeight + 6 + 24 + 4
               + editorHeight + 2 + statusHeight + 8;
    }

    SoundEquationPanel& owner;
    juce::AudioProcessorValueTreeState& apvts;
    SoundEquationStage& stage;
    HelpTarget* help;

    ParamToggle enable;
    ParamChoice preset;
    TransferCurve curve;
    ParamKnob dialA, dialB, dialC, dialD, drive, mix, out;
    ParamToggle safety;
    juce::TextButton resetButton, copyButton, varsButton;
    juce::Label formulaLabel, status;
    juce::TextEditor editor;
    widgets_detail::HoverReporter editorHover, copyHover, varsHover, resetHover;

    int lastIndex = -1, lastPresetIndex = 0;
    juce::uint32 compileAt = 0;
    juce::String previewKey, previewText;
    std::shared_ptr<const expr::Expression> previewProgram;
};

SoundEquationPanel::SoundEquationPanel (juce::AudioProcessorValueTreeState& state, SoundEquationStage& stage, HelpTarget* help)
    : impl (std::make_unique<Impl> (*this, state, stage, help))
{
}

SoundEquationPanel::~SoundEquationPanel() = default;

int SoundEquationPanel::idealHeight (int) const
{
    return Impl::heightNeeded();
}

void SoundEquationPanel::paint (juce::Graphics& g)
{
    Theme::drawSection (g, getLocalBounds(), "WAVE EQUATION  (changes the sound)");
}

void SoundEquationPanel::resized()
{
    impl->layout (getLocalBounds());
}
