#include "Views3D.h"
#include "../DspUtil.h"
#include "../Expr/Expression.h"
#include <cmath>
#include <deque>
#include <functional>

namespace
{
    enum Mode       { helix = 0, phaseSpace, stereo3D, waterfall };
    enum ColourMode { byTime = 0, byAmplitude, byPhase, byVelocity };
    enum Var        { vS, vH, vL, vR, vM, vSd, vD1, vD2, vV, vA, vU, vT, vF, vP, vQ, numVars };

    const juce::StringArray& variableNames()
    {
        static const juce::StringArray names { "s", "h", "l", "r", "m", "sd", "d1", "d2", "v", "a", "u", "t", "f", "p", "q" };
        return names;
    }

    const char* formulaProperty[3] = { "v3FormulaX", "v3FormulaY", "v3FormulaZ" };
    const char* defaultFormula[3]  = { "s", "h", "u - 0.5" };
    const char* modeNames[4]       = { "Helix", "Phase space", "Stereo 3D", "Waterfall" };
    const char* modeBlurbs[4]      = { "Signal vs its 90-degree copy along time. A pure tone is a perfect corkscrew.",
                                       "Signal vs its own delayed or derivative copies. Tones loop, noise clouds.",
                                       "Left vs right, with time, mid or side as depth. 3D Lissajous shapes.",
                                       "Spectrum over time as a 3D landscape you can fly around." };

    juce::String sym (juce::juce_wchar c) { return juce::String::charToString (c); }
    juce::String tauS()   { return sym (0x03C4); }
    juce::String omegaS() { return sym (0x03C9); }
    juce::String degS()   { return sym (0x00B0); }
    juce::String piS()    { return sym (0x03C0); }

    juce::NormalisableRange<float> skewed (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    // The ramps' colours on the 3D screen (Scene3D::ink), refreshed when the theme changes: used per vertex
    struct Ramp { juce::Colour blue, green, amber, floor; };
    const Ramp& ramp()
    {
        static Ramp r;
        static juce::uint32 key[4] {};
        const juce::uint32 now[4] { Palette::blue.getARGB(), Palette::green.getARGB(), Palette::amber.getARGB(), Palette::screen.getARGB() };
        if (! std::equal (std::begin (now), std::end (now), std::begin (key)))
        {
            std::copy (std::begin (now), std::end (now), std::begin (key));
            r = { Scene3D::ink (Palette::blue), Scene3D::ink (Palette::green), Scene3D::ink (Palette::amber), Palette::screen.brighter (0.4f) };
        }
        return r;
    }

    juce::Colour gradient (float t)
    {
        const auto& k = ramp();
        t = juce::jlimit (0.0f, 1.0f, t);
        if (t < 0.5f)
            return k.blue.interpolatedWith (k.green, t * 2.0f);
        return k.green.interpolatedWith (k.amber, (t - 0.5f) * 2.0f);
    }

    juce::Colour terrain (float h)
    {
        const auto& k = ramp();
        h = juce::jlimit (0.0f, 1.0f, h);
        if (h < 0.35f) return k.floor.interpolatedWith (k.blue, h / 0.35f);
        if (h < 0.7f)  return k.blue.interpolatedWith (k.green, (h - 0.35f) / 0.35f);
        return k.green.interpolatedWith (k.amber, (h - 0.7f) / 0.3f);
    }

    Vertex3 vert (float x, float y, float z, juce::Colour c, float alpha)
    {
        return { x, y, z, c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue(), alpha };
    }

    // The formulas that reproduce each preset view (used by "Reset to view's formulas")
    void viewFormulas (int mode, int embed, int stereoZ, juce::String out[3])
    {
        out[0] = "s"; out[1] = "h"; out[2] = "u - 0.5";
        if (mode == phaseSpace)
        {
            out[0] = "s";
            out[1] = embed == 0 ? "d1" : "v";
            out[2] = embed == 0 ? "d2" : "a";
        }
        else if (mode == stereo3D)
        {
            out[0] = "l";
            out[1] = "r";
            out[2] = stereoZ == 0 ? "u - 0.5" : (stereoZ == 1 ? "m" : "sd");
        }
    }
}

//==============================================================================
void Views3D::addParameters (ParamList& p)
{
    using namespace juce;
    using FloatAttr = AudioParameterFloatAttributes;

    auto msText = [] (float v, int) { return String (v, v < 10.0f ? 2 : 0) + " ms"; };
    auto xText  = [] (float v, int) { return "x" + String (v, 2); };
    auto pctText = [] (float v, int) { return String ((int) std::round (v)) + " %"; };

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "v3Mode", 1 }, "3D View",
        StringArray { "Helix", "Phase space", "Stereo 3D", "Waterfall" }, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "v3Window", 1 }, "3D Window",
        skewed (2.0f, 500.0f, 40.0f), 20.0f, FloatAttr().withStringFromValueFunction (msText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "v3Scale", 1 }, "3D Scale",
        skewed (0.1f, 16.0f, 1.0f), 1.0f, FloatAttr().withStringFromValueFunction (xText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "v3Depth", 1 }, "3D Depth",
        skewed (0.1f, 4.0f, 1.0f), 1.0f, FloatAttr().withStringFromValueFunction (xText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "v3Twist", 1 }, "3D Twist",
        NormalisableRange<float> (-4.0f, 4.0f, 0.01f), 0.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, 2) + " turns"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "v3Tau", 1 }, "3D Delay Tau",
        skewed (0.05f, 50.0f, 2.0f), 1.0f, FloatAttr().withStringFromValueFunction (msText)));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "v3TauAuto", 1 }, "3D Auto Tau", true));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "v3Embed", 1 }, "3D Embedding",
        StringArray { "Delay (Takens)", "Derivatives" }, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "v3StereoZ", 1 }, "3D Stereo Depth",
        StringArray { "Depth = time", "Depth = mid", "Depth = side" }, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "v3Colour", 1 }, "3D Colour",
        StringArray { "By time", "By amplitude", "By phase", "By velocity" }, 2));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "v3Trail", 1 }, "3D Afterglow",
        NormalisableRange<float> (0.0f, 95.0f, 0.1f), 45.0f, FloatAttr().withStringFromValueFunction (pctText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "v3Thick", 1 }, "3D Line Width",
        NormalisableRange<float> (0.5f, 6.0f, 0.1f), 2.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " px"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "v3Spin", 1 }, "3D Auto Spin",
        NormalisableRange<float> (-90.0f, 90.0f, 0.5f), 10.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return std::abs (v) < 0.25f ? String ("Off") : String (v, 1) + " deg/s"; })));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "v3Rows", 1 }, "3D Waterfall Rows", 16, 128, 80));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "v3Map", 1 }, "3D Mapping",
        StringArray { "Preset mapping", "Custom formulas" }, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "v3P", 1 }, "3D Dial p",
        NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, 3); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "v3Q", 1 }, "3D Dial q",
        NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, 3); })));
}

HelpList Views3D::helpEntries()
{
    const auto T = tauS(), W = omegaS(), D = degS(), P = piS();
    const juce::String pictureOnly = " Picture only: the sound is never changed by this control.";

    return {
        { "v3Mode", "3D view",
          "Chooses which 3D picture of the wave is drawn." + pictureOnly,
          "s = mono signal (L+R)/2 of the shown source, u = 0 (oldest) .. 1 (newest) along the window.\n"
          "Helix: x = s*scale, y = H[s]*scale, z = (u-0.5)*2*depth, then (x,y) rotated by 2" + P + "*twist*u. H = Hilbert transform (every frequency shifted 90" + D + ").\n"
          "Phase space (Delay): x = s(t), y = s(t-" + T + "), z = s(t-2" + T + "), all *scale.  (Derivatives): x = s, y = (ds/dt)/" + W + ", z = (d2s/dt2)/" + W + "^2, " + W + " = 2" + P + "*pitch.\n"
          "Stereo 3D: x = L*scale, y = R*scale, z = time (u-0.5)*2*depth, or mid (L+R)/2 *scale, or side (L-R)/2 *scale.\n"
          "Waterfall: x = log frequency 30 Hz..20 kHz, y = level in dB (-90..0) *scale, z = age of the spectrum row *depth.",
          "Start with Helix on a pure tone: one perfect corkscrew. Add a second tone and the corkscrew wobbles at the beat rate." },
        { "v3Window", "Window",
          "How much recent audio the curve views draw." + pictureOnly,
          "The last window milliseconds of audio become the curve; u runs 0..1 across it. Longer = more cycles in the helix / more laps of the loop.",
          "About 4-10 periods of the note looks best: 20 ms for ~220 Hz." },
        { "v3Scale", "Scale",
          "Amplitude zoom of the signal axes." + pictureOnly,
          "Every signal-valued coordinate is multiplied by scale (helix x/y, phase-space x/y/z, stereo x/y and mid/side depth, waterfall height). Full-scale audio (1.0) reaches the edge of the frame at x1.",
          "Quiet external input: turn this up instead of the input gain so the sound stays untouched." },
        { "v3Depth", "Depth",
          "Stretch of the time axis." + pictureOnly,
          "z = (u - 0.5) * 2 * depth whenever z is time (helix, stereo with Depth = time, all custom Z formulas). Waterfall: row spacing along z. Ignored when z is a signal (phase space).",
          "Smaller depth packs more turns of the corkscrew together." },
        { "v3Twist", "Twist",
          "Extra rotation of the x-y plane along the time axis." + pictureOnly,
          "(x, y) are rotated by angle 2" + P + " * twist * u, so the oldest point is unrotated and the newest is turned 'twist' full turns. A tone whose cycles match the twist untwists into a straight line, i.e. twist acts like a frequency shift of the picture.",
          "Negative values twist the other way. Tuning twist to the number of cycles in the window freezes the helix." },
        { "v3Tau", "Delay " + T,
          "Embedding delay for Phase space (Delay mode), used when Auto " + T + " is off." + pictureOnly,
          "y = s(t - " + T + "), z = s(t - 2" + T + "). " + T + " of a quarter period turns a sine into a circle; other values give ellipses.",
          "" },
        { "v3TauAuto", "Auto " + T,
          "Sets " + T + " automatically from the detected pitch." + pictureOnly,
          T + " = 1 / (4 * pitch) - a quarter period, so a pure tone draws a round loop. Falls back to 1 ms when no pitch is found (noise, silence).",
          "" },
        { "v3Embed", "Embedding",
          "How Phase space builds its 3 coordinates from one signal." + pictureOnly,
          "Delay (Takens): (s(t), s(t-" + T + "), s(t-2" + T + ")).  Derivatives: (s, ds/dt / " + W + ", d2s/dt2 / " + W + "^2) with " + W + " = 2" + P + " * pitch (440 Hz if none), so a sine of amplitude A gives a circle of radius A.",
          "Periodic sounds draw closed loops or knots; harmonics add lobes; noise becomes a fuzzy cloud." },
        { "v3StereoZ", "Stereo depth",
          "What the depth axis shows in Stereo 3D." + pictureOnly,
          "x = left, y = right always. Depth = time: z = (u-0.5)*2*depth (Lissajous stretched along time). Depth = mid: z = (L+R)/2 * scale. Depth = side: z = (L-R)/2 * scale.",
          "Mono material lies on the x = y diagonal; wide stereo spreads off it." },
        { "v3Colour", "Colour",
          "What the colour of the curve encodes." + pictureOnly,
          "By time: blue (oldest) -> green -> amber (newest). By amplitude: envelope |s + iH[s]| / peak on the same scale. By phase: hue = instantaneous phase atan2(H[s], s) (one full colour wheel per cycle). By velocity: |ds/dt| / " + W + " relative to the peak.",
          "Phase colouring makes the corkscrew look like a rainbow ribbon with one cycle of hue per turn." },
        { "v3Trail", "Afterglow",
          "Keeps fading copies of previous frames, like phosphor." + pictureOnly,
          "Each older frame is drawn with brightness afterglow^age (age in frames), up to 16 frames.",
          "" },
        { "v3Thick", "Line width",
          "Thickness of the drawn curve in pixels." + pictureOnly,
          "Core line = width; a soft glow 4x wider at 12% brightness is drawn under it.",
          "" },
        { "v3Spin", "Auto spin",
          "Slowly rotates the camera around the vertical axis." + pictureOnly,
          "Camera yaw changes by this many degrees per second (paused while you drag). Drag to rotate by hand, wheel to zoom, double-click to reset.",
          "" },
        { "v3Rows", "Waterfall rows",
          "How much spectrum history the Waterfall keeps." + pictureOnly,
          "One row (a 2048-point FFT, log-binned into 96 columns) is added per frame, about 60 per second; the oldest row is dropped beyond this count.",
          "" },
        { "v3Map", "Mapping",
          "Use the view's built-in equations or your own x/y/z formulas." + pictureOnly,
          "Preset mapping: the equations listed under 3D view. Custom formulas: x = X*scale, y = Y*scale, z = Z*2*depth, then twist is applied, with X/Y/Z typed below. Colour, window, afterglow still apply. Waterfall is a spectrum surface and has no custom formulas.",
          "Press 'Reset to view's formulas' to load the current view's equations, then edit them." },
        { "v3P", "Dial p",
          "A free dial (-1..1) you can use in custom formulas as p." + pictureOnly,
          "Only appears inside your formulas as the variable p. Example: X = s*cos(p*pi) - h*sin(p*pi) rotates the helix.",
          "" },
        { "v3Q", "Dial q",
          "A free dial (-1..1) you can use in custom formulas as q." + pictureOnly,
          "Only appears inside your formulas as the variable q. Example: Z = (u - 0.5) + q*s pushes loud parts forward.",
          "" },
        { "v3.formulaX", "Formula X",
          "Your equation for the horizontal axis (custom mapping)." + pictureOnly,
          "x = X * scale, then rotated by twist. Evaluated once per drawn point with the variables of that moment.",
          "Compiles 0.4 s after you stop typing or when you press Enter. Errors show in red and the preset x is used meanwhile." },
        { "v3.formulaY", "Formula Y",
          "Your equation for the vertical axis (custom mapping)." + pictureOnly,
          "y = Y * scale, then rotated by twist.", "" },
        { "v3.formulaZ", "Formula Z",
          "Your equation for the depth axis (custom mapping)." + pictureOnly,
          "z = Z * 2 * depth (so Z = u - 0.5 spreads time over the depth).", "" },
        { "v3.vars", "Formula variables",
          "What you can use inside the X/Y/Z formulas.",
          "s signal (L+R)/2, h Hilbert of s (90" + D + " copy), l / r left / right, m mid, sd side, d1 / d2 s delayed by " + T + " / 2" + T + ", v = (ds/dt)/" + W + ", a = (d2s/dt2)/" + W + "^2, u = 0..1 along the window, t = seconds, f = detected pitch in Hz, p / q dials.\n"
          "Functions: sin cos tan asin acos atan atan2 exp log sqrt abs pow min max clamp sat tri saw sqr hypot, constants pi tau e.",
          "Try X = s, Y = h, Z = sin(2*pi*u*3)*0.3 for a corkscrew wound around a coil." },
    };
}

//==============================================================================
struct Views3D::Impl
{
    // ownedFeed / ownedParams: the editor's own view (this plugin); formulaState: where setFormula saves (nullptr = read-only)
    Impl (const ViewFeed& f, const ViewParams& p, juce::ValueTree formulaState,
          std::unique_ptr<ViewFeed> ownedFeed = {}, std::unique_ptr<ViewParams> ownedParams = {})
        : ownFeed (std::move (ownedFeed)), ownParams (std::move (ownedParams)), feed (f), params (p), state (formulaState)
    {
        startMs = juce::Time::getMillisecondCounterHiRes();
    }

    float param (const char* id) const { return params.get (id, 0.0f); }

    void pullAudio (int mode, bool frozen)
    {
        sampleRate = juce::jmax (8000.0, feed.sampleRate());
        if (frozen && total > 0)
            return;

        const int maxCopy = ScopeBuffer::size / 2;
        count = juce::jlimit (64, 32768, (int) (param ("v3Window") * 0.001 * sampleRate));
        if (mode == waterfall)
            count = juce::jmax (count, 2048);

        if (param ("v3TauAuto") > 0.5f)
            tauSamples = pitchHz > 0.0f ? (int) std::round (sampleRate / (4.0 * pitchHz)) : (int) std::round (sampleRate * 0.001);
        else
            tauSamples = (int) std::round (param ("v3Tau") * 0.001 * sampleRate);
        tauSamples = juce::jlimit (1, 16384, tauSamples);

        const int margin = juce::jmax (256, count / 8);
        const int back = 2 * tauSamples + 2;
        count = juce::jmin (count, maxCopy - margin - back);
        total = back + count + margin;
        start = back;

        rawL.resize ((size_t) total);
        rawR.resize ((size_t) total);
        sig.resize ((size_t) total);
        hil.resize ((size_t) total);

        feed.copyLatestPadded ((int) params.get ("source", 2.0f) != 0, rawL.data(), rawR.data(), total);
        for (int i = 0; i < total; ++i)
            sig[(size_t) i] = 0.5f * (rawL[(size_t) i] + rawR[(size_t) i]);

        pitchHz = DspUtil::estimateFrequency (sig.data(), total, sampleRate);
        computeHilbert();

        peak = 1.0e-4f;
        for (int i = start; i < start + count; ++i)
            peak = juce::jmax (peak, std::hypot (sig[(size_t) i], hil[(size_t) i]));
    }

    // Analytic signal via FFT: keep DC, double positive frequencies, drop negative ones
    void computeHilbert()
    {
        int order = 6;
        while ((1 << order) < total)
            ++order;
        const int M = 1 << order;
        if (order != hilbertOrder)
        {
            hilbertFft = std::make_unique<juce::dsp::FFT> (order);
            hilbertOrder = order;
            fftIn.assign ((size_t) M, {});
            fftOut.assign ((size_t) M, {});
        }

        double mean = 0.0;
        for (int i = 0; i < total; ++i)
            mean += sig[(size_t) i];
        mean /= juce::jmax (1, total);

        for (int i = 0; i < M; ++i)
            fftIn[(size_t) i] = { i < total ? sig[(size_t) i] - (float) mean : 0.0f, 0.0f };

        hilbertFft->perform (fftIn.data(), fftOut.data(), false);
        for (int k = 1; k < M / 2; ++k)
            fftOut[(size_t) k] *= 2.0f;
        for (int k = M / 2 + 1; k < M; ++k)
            fftOut[(size_t) k] = {};
        hilbertFft->perform (fftOut.data(), fftIn.data(), true);

        // the inverse transform's scaling is implementation-defined; measure it from the real part
        double num = 0.0, den = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const double x = sig[(size_t) i] - mean;
            num += fftIn[(size_t) i].real() * x;
            den += x * x;
        }
        const double s = (den > 1.0e-12 && std::abs (num) > 1.0e-12) ? num / den : 1.0;
        for (int i = 0; i < total; ++i)
            hil[(size_t) i] = (float) (fftIn[(size_t) i].imag() / s);
    }

    juce::String setFormula (int axis, const juce::String& text)
    {
        axis = juce::jlimit (0, 2, axis);
        formulaText[axis] = text;
        if (state.isValid())
            state.setProperty (formulaProperty[axis], text, nullptr);

        juce::String err;
        formula[axis] = expr::Expression::compile (text, variableNames(), err);
        if (formula[axis] == nullptr && err.isEmpty())
            err = "Could not compile";
        formulaError[axis] = formula[axis] != nullptr ? juce::String() : err;
        return formulaError[axis];
    }

    void reloadFromState()
    {
        for (int k = 0; k < 3; ++k)
            setFormula (k, params.getText (formulaProperty[k], defaultFormula[k]));
    }

    void buildCurve (std::vector<Vertex3>& out, int mode)
    {
        out.clear();
        if (count < 2 || total < start + count + 1)
            return;

        const int embed = (int) param ("v3Embed");
        const int stereoZ = (int) param ("v3StereoZ");
        const int colourMode = (int) param ("v3Colour");
        const bool custom = (int) param ("v3Map") == 1;
        const float scale = param ("v3Scale"), depth = param ("v3Depth"), twist = param ("v3Twist");
        const double omega = juce::MathConstants<double>::twoPi * (pitchHz > 0.0f ? pitchHz : 440.0);
        const float dScale = (float) (sampleRate / omega);
        const bool zIsTime = mode == helix || (mode == stereo3D && stereoZ == 0);
        const int step = juce::jmax (1, count / 4000);
        const double nowSec = (juce::Time::getMillisecondCounterHiRes() - startMs) * 0.001;

        float vars[numVars] {};
        vars[vF] = pitchHz;
        vars[vP] = param ("v3P");
        vars[vQ] = param ("v3Q");

        out.reserve ((size_t) (count / step + 2));
        for (int i = start; i < start + count; i += step)
        {
            const float u = (float) (i - start) / (float) (count - 1);
            const float s = sig[(size_t) i], h = hil[(size_t) i];
            const float l = rawL[(size_t) i], r = rawR[(size_t) i];
            const float v = (sig[(size_t) i + 1] - sig[(size_t) i - 1]) * 0.5f * dScale;
            const float a = (sig[(size_t) i + 1] - 2.0f * s + sig[(size_t) i - 1]) * dScale * dScale;
            const float d1 = sig[(size_t) (i - tauSamples)], d2 = sig[(size_t) (i - 2 * tauSamples)];

            float x = s, y = h, z = u - 0.5f;
            if (mode == phaseSpace)
            {
                y = embed == 0 ? d1 : v;
                z = embed == 0 ? d2 : a;
            }
            else if (mode == stereo3D)
            {
                x = l;
                y = r;
                z = stereoZ == 0 ? u - 0.5f : (stereoZ == 1 ? 0.5f * (l + r) : 0.5f * (l - r));
            }

            float X = x * scale, Y = y * scale, Z = zIsTime ? z * 2.0f * depth : z * scale;

            if (custom)
            {
                vars[vS] = s;  vars[vH] = h;  vars[vL] = l;  vars[vR] = r;
                vars[vM] = 0.5f * (l + r);  vars[vSd] = 0.5f * (l - r);
                vars[vD1] = d1;  vars[vD2] = d2;  vars[vV] = v;  vars[vA] = a;
                vars[vU] = u;
                vars[vT] = (float) (nowSec - (double) (start + count - i) / sampleRate);
                if (formula[0] != nullptr) X = formula[0]->evaluate (vars) * scale;
                if (formula[1] != nullptr) Y = formula[1]->evaluate (vars) * scale;
                if (formula[2] != nullptr) Z = formula[2]->evaluate (vars) * 2.0f * depth;
            }

            if (twist != 0.0f)
            {
                const float ang = juce::MathConstants<float>::twoPi * twist * u;
                const float c = std::cos (ang), sn = std::sin (ang);
                const float rx = X * c - Y * sn;
                Y = X * sn + Y * c;
                X = rx;
            }

            juce::Colour col;
            switch (colourMode)
            {
                case byTime:      col = gradient (u); break;
                case byAmplitude: col = gradient (std::hypot (s, h) / peak); break;
                case byVelocity:  col = gradient (std::abs (v) / peak); break;
                default:
                {
                    const float hue = std::atan2 (h, s) / juce::MathConstants<float>::twoPi + 0.5f;
                    col = juce::Colour::fromHSV (hue, 0.7f, 1.0f, 1.0f);
                    break;
                }
            }

            out.push_back (vert (juce::jlimit (-8.0f, 8.0f, X), juce::jlimit (-8.0f, 8.0f, Y),
                                 juce::jlimit (-8.0f, 8.0f, Z), col, 0.3f + 0.7f * u));
        }
    }

    void buildWaterfall (std::vector<DrawBatch>& out, bool frozen)
    {
        static constexpr int cols = 96, n = 2048;
        const int maxRows = juce::jlimit (16, 128, (int) param ("v3Rows"));
        const float scale = param ("v3Scale"), depth = param ("v3Depth");

        if ((! frozen || rows.empty()) && start + count >= n)
        {
            std::fill (specData.begin(), specData.end(), 0.0f);
            const int base = start + count - n;
            for (int i = 0; i < n; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (n - 1));
                specData[(size_t) i] = sig[(size_t) (base + i)] * w;
            }
            spectrumFft.performFrequencyOnlyForwardTransform (specData.data());

            std::vector<float> row ((size_t) cols);
            const double fLo = 30.0, fHi = juce::jmin (20000.0, sampleRate * 0.5);
            for (int c = 0; c < cols; ++c)
            {
                const double f0 = fLo * std::pow (fHi / fLo, (double) c / cols);
                const double f1 = fLo * std::pow (fHi / fLo, (double) (c + 1) / cols);
                const int b0 = juce::jlimit (1, n / 2 - 1, (int) (f0 * n / sampleRate));
                const int b1 = juce::jlimit (b0 + 1, n / 2, (int) (f1 * n / sampleRate) + 1);
                float mag = 0.0f;
                for (int b = b0; b < b1; ++b)
                    mag = juce::jmax (mag, specData[(size_t) b]);
                row[(size_t) c] = juce::jlimit (-100.0f, 0.0f, juce::Decibels::gainToDecibels (mag * 4.0f / (float) n, -100.0f));
            }
            rows.push_front (std::move (row));
        }
        while ((int) rows.size() > maxRows)
            rows.pop_back();
        if (rows.size() < 2)
            return;

        auto px = [] (int c) { return ((float) c / (float) (cols - 1) * 2.0f - 1.0f) * 1.2f; };
        auto pz = [&] (int r) { return (1.0f - 2.0f * (float) r / (float) (maxRows - 1)) * depth; };
        auto h01 = [] (float db) { return juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f); };
        auto py = [&] (float h) { return (h * 1.6f - 0.8f) * scale; };

        DrawBatch surf;
        surf.type = DrawBatch::triangles;
        surf.additive = false;
        surf.depthTest = true;
        surf.vertices.reserve ((size_t) (rows.size() * cols * 6));
        for (int r = 0; r + 1 < (int) rows.size(); ++r)
        {
            const float fade0 = 0.9f * (1.0f - 0.75f * (float) r / (float) maxRows);
            const float fade1 = 0.9f * (1.0f - 0.75f * (float) (r + 1) / (float) maxRows);
            for (int c = 0; c + 1 < cols; ++c)
            {
                const float ha = h01 (rows[(size_t) r][(size_t) c]),       hb = h01 (rows[(size_t) r][(size_t) c + 1]);
                const float hc = h01 (rows[(size_t) r + 1][(size_t) c]),   hd = h01 (rows[(size_t) r + 1][(size_t) c + 1]);
                const Vertex3 A = vert (px (c),     py (ha), pz (r),     terrain (ha), fade0);
                const Vertex3 B = vert (px (c + 1), py (hb), pz (r),     terrain (hb), fade0);
                const Vertex3 C = vert (px (c),     py (hc), pz (r + 1), terrain (hc), fade1);
                const Vertex3 E = vert (px (c + 1), py (hd), pz (r + 1), terrain (hd), fade1);
                surf.vertices.insert (surf.vertices.end(), { A, B, C, B, E, C });
            }
        }
        out.push_back (std::move (surf));

        DrawBatch front;
        front.type = DrawBatch::lineStrip;
        front.lineWidth = 2.0f;
        for (int c = 0; c < cols; ++c)
        {
            const float h = h01 (rows[0][(size_t) c]);
            front.vertices.push_back (vert (px (c), py (h) + 0.01f, pz (0), ramp().green.brighter (0.3f), 0.95f));
        }
        out.push_back (std::move (front));
    }

    juce::String axisLabel (int axis, int mode) const
    {
        const bool custom = (int) param ("v3Map") == 1 && mode != waterfall;
        if (custom)
        {
            const char* names[3] = { "x: X = ", "y: Y = ", "z: Z = " };
            auto t = formulaText[axis].trim();
            if (t.length() > 28) t = t.substring (0, 26) + "...";
            return names[axis] + t + (formula[axis] == nullptr ? " (error)" : "");
        }

        const auto T = tauS(), W = omegaS();
        switch (mode)
        {
            case helix:
            {
                const juce::String l[3] = { "x: signal s", "y: Hilbert H[s] (90" + degS() + " copy)", "z: time (newest at +z)" };
                return l[axis];
            }
            case phaseSpace:
            {
                if ((int) param ("v3Embed") == 0)
                {
                    const juce::String l[3] = { "x: s(t)", "y: s(t - " + T + ")", "z: s(t - 2" + T + ")" };
                    return l[axis];
                }
                const juce::String l[3] = { "x: s", "y: ds/dt / " + W, "z: d2s/dt2 / " + W + "^2" };
                return l[axis];
            }
            case stereo3D:
            {
                const int sz = (int) param ("v3StereoZ");
                const juce::String l[3] = { "x: left", "y: right", sz == 0 ? "z: time" : (sz == 1 ? "z: mid (L+R)/2" : "z: side (L-R)/2") };
                return l[axis];
            }
            default:
            {
                const juce::String l[3] = { "x: frequency (log)", "y: level (dB)", "z: time (newest at front)" };
                return l[axis];
            }
        }
    }

    juce::String describe() const
    {
        const int mode = (int) param ("v3Mode");
        const bool custom = (int) param ("v3Map") == 1 && mode != waterfall;
        const auto T = tauS(), W = omegaS(), P = piS();
        const juce::String scale = juce::String (param ("v3Scale"), 2), depth = juce::String (param ("v3Depth"), 2),
                           twist = juce::String (param ("v3Twist"), 2);
        const juce::String src = (int) param ("source") == 0 ? "INPUT (raw)" : "OUTPUT (modified)";

        juce::String d;
        d << "3D view: " << modeNames[mode] << (custom ? " with custom formulas" : "") << "  -  picture only, the sound is unchanged.\n";

        if (mode == waterfall)
        {
            d << "Each frame a 2048-point FFT of the " << src << " mono signal (L+R)/2 is binned into 96 log-spaced columns (30 Hz .. "
              << juce::String (juce::jmin (20000.0, sampleRate * 0.5) / 1000.0, 1) << " kHz).\n"
              << "x = log frequency, y = (dB + 90)/90 * 1.6 - 0.8, times scale " << scale << ".\n"
              << "z = row age: newest row at the front, older rows step back by depth " << depth << "; "
              << (int) param ("v3Rows") << " rows kept (~" << juce::String (param ("v3Rows") / 60.0f, 1) << " s).\n"
              << "Colour = height: dark -> blue -> green -> amber as the level rises.";
            return d;
        }

        d << "s = mono (L+R)/2 of the " << src << " signal over the last " << juce::String (param ("v3Window"), 1)
          << " ms; u = 0 (oldest) .. 1 (newest). Pitch " << (pitchHz > 0.0f ? juce::String (pitchHz, 1) + " Hz" : juce::String ("not detected")) << ".\n";

        if (custom)
        {
            const char* n[3] = { "X", "Y", "Z" };
            for (int k = 0; k < 3; ++k)
                d << n[k] << " = " << formulaText[k] << (formula[k] == nullptr ? "   (error: preset used)" : "") << "\n";
            d << "x = X * " << scale << ", y = Y * " << scale << ", z = Z * 2 * " << depth << ".\n";
        }
        else if (mode == helix)
        {
            d << "x = s(t) * " << scale << "\ny = H[s](t) * " << scale << "   (H = Hilbert transform: every frequency shifted 90 degrees)\n"
              << "z = (u - 0.5) * 2 * " << depth << "\n"
              << "A pure tone traces a circle in x-y, so along z it becomes a corkscrew: radius = amplitude, turns = cycles in the window.\n";
        }
        else if (mode == phaseSpace)
        {
            const juce::String tauMs = juce::String (tauSamples * 1000.0 / sampleRate, 3);
            if ((int) param ("v3Embed") == 0)
                d << "x = s(t), y = s(t - " << T << "), z = s(t - 2" << T << "), all * " << scale << "\n"
                  << T << " = " << tauMs << " ms" << (param ("v3TauAuto") > 0.5f ? " (auto: quarter period of the pitch)" : " (manual)") << ".\n";
            else
                d << "x = s, y = (ds/dt) / " << W << ", z = (d2s/dt2) / " << W << "^2, all * " << scale << "; "
                  << W << " = 2" << P << " * " << juce::String (pitchHz > 0.0f ? pitchHz : 440.0f, 1) << " Hz.\n";
            d << "Periodic sound -> closed loop / knot. Noise -> cloud.\n";
        }
        else
        {
            const int sz = (int) param ("v3StereoZ");
            d << "x = L * " << scale << ", y = R * " << scale << ", z = "
              << (sz == 0 ? "(u - 0.5) * 2 * " + depth + " (time)" : (sz == 1 ? "(L+R)/2 * " + scale + " (mid)" : "(L-R)/2 * " + scale + " (side)")) << "\n"
              << "Mono sound lies on the x = y diagonal; stereo width spreads it out.\n";
        }

        if (param ("v3Twist") != 0.0f)
            d << "Then (x, y) are rotated by 2" << P << " * " << twist << " * u.\n";

        const char* colours[4] = { "time (blue = oldest, amber = newest)", "amplitude envelope |s + iH[s]|",
                                   "instantaneous phase atan2(H[s], s) as hue", "speed |ds/dt| / " };
        d << "Colour = " << colours[(int) param ("v3Colour")] << ((int) param ("v3Colour") == byVelocity ? W : juce::String()) << ".";
        return d;
    }

    std::unique_ptr<ViewFeed> ownFeed;
    std::unique_ptr<ViewParams> ownParams;
    const ViewFeed& feed;
    const ViewParams& params;
    juce::ValueTree state;

    std::vector<float> rawL, rawR, sig, hil;
    int total = 0, start = 0, count = 0, tauSamples = 1;
    float pitchHz = 0.0f, peak = 1.0f;
    double sampleRate = 44100.0, startMs = 0.0;

    std::unique_ptr<juce::dsp::FFT> hilbertFft;
    int hilbertOrder = 0;
    std::vector<juce::dsp::Complex<float>> fftIn, fftOut;

    juce::dsp::FFT spectrumFft { 11 };
    std::vector<float> specData = std::vector<float> (4096, 0.0f);
    std::deque<std::vector<float>> rows;
    std::deque<std::vector<Vertex3>> trail;

    std::shared_ptr<const expr::Expression> formula[3];
    juce::String formulaText[3], formulaError[3];
};

//==============================================================================
Views3D::Views3D (ClodScopeProcessor& p)
{
    auto f = std::make_unique<ProcessorFeed> (p);
    auto ps = std::make_unique<ApvtsParams> (p.apvts);
    const ViewFeed& feedRef = *f;
    const ViewParams& paramsRef = *ps;
    impl = std::make_unique<Impl> (feedRef, paramsRef, p.apvts.state, std::move (f), std::move (ps));
    impl->reloadFromState();
}

Views3D::Views3D (const ViewFeed& f, const ViewParams& p)
    : impl (std::make_unique<Impl> (f, p, juce::ValueTree()))
{
    impl->reloadFromState();
}

Views3D::~Views3D() = default;

void Views3D::produce (Scene3D& scene)
{
    auto& m = *impl;
    const int mode = juce::jlimit (0, 3, (int) m.param ("v3Mode"));
    const bool frozen = m.param ("freeze") > 0.5f;
    m.pullAudio (mode, frozen);

    std::vector<DrawBatch> batches;

    if (mode == waterfall)
    {
        m.trail.clear();
        m.buildWaterfall (batches, frozen);
    }
    else
    {
        std::vector<Vertex3> curve;
        m.buildCurve (curve, mode);
        const float thick = m.param ("v3Thick");
        const float glowAmount = m.param ("v3Trail") * 0.01f;
        const int keep = glowAmount > 0.01f ? juce::jlimit (1, 16, (int) std::ceil (std::log (0.05) / std::log ((double) glowAmount))) : 0;

        if (frozen)
            m.trail.clear();
        else
            m.trail.push_front (curve);
        while ((int) m.trail.size() > keep + 1)
            m.trail.pop_back();

        for (int age = (int) m.trail.size() - 1; age >= 1; --age)
        {
            DrawBatch b;
            b.type = DrawBatch::lineStrip;
            b.lineWidth = juce::jmax (1.0f, thick * 0.8f);
            b.vertices = m.trail[(size_t) age];
            const float fade = std::pow (glowAmount, (float) age) * 0.6f;
            for (auto& v : b.vertices)
                v.a *= fade;
            batches.push_back (std::move (b));
        }

        DrawBatch glow;
        glow.type = DrawBatch::lineStrip;
        glow.lineWidth = thick * 4.0f;
        glow.vertices = curve;
        for (auto& v : glow.vertices)
            v.a *= 0.12f;
        batches.push_back (std::move (glow));

        DrawBatch core;
        core.type = DrawBatch::lineStrip;
        core.lineWidth = thick;
        core.vertices = curve;
        batches.push_back (std::move (core));

        if (! curve.empty())
        {
            DrawBatch head;
            head.type = DrawBatch::points;
            head.pointSize = thick * 4.0f + 4.0f;
            auto v = curve.back();
            v.r = v.g = v.b = 1.0f;
            v.a = 0.9f;
            head.vertices.push_back (v);
            batches.push_back (std::move (head));
        }
    }

    scene.setWorldRadius (1.4f);
    scene.setAxes (true, m.axisLabel (0, mode), m.axisLabel (1, mode), m.axisLabel (2, mode));
    scene.setAutoSpin (m.param ("v3Spin"));

    const bool custom = (int) m.param ("v3Map") == 1 && mode != waterfall;
    juce::StringArray top;
    top.add (juce::String (modeNames[mode]) + (custom ? "  -  custom formulas" : ""));
    juce::String info;
    if (mode == waterfall)
        info << (int) m.param ("v3Rows") << " rows of spectrum   -   2048-pt FFT, 96 log bands";
    else
    {
        info << "window " << juce::String (m.param ("v3Window"), 1) << " ms   pitch "
             << (m.pitchHz > 0.0f ? juce::String (m.pitchHz, 1) + " Hz" : juce::String ("--"));
        if (mode == phaseSpace && (int) m.param ("v3Embed") == 0)
            info << "   " << tauS() << " " << juce::String (m.tauSamples * 1000.0 / m.sampleRate, 2) << " ms"
                 << (m.param ("v3TauAuto") > 0.5f ? " (auto)" : "");
    }
    top.add (info);

    juce::StringArray bottom;
    bottom.add (juce::String ("picture only - the sound is not changed") + (frozen ? "   -   FROZEN" : ""));

    scene.setOverlay (top, bottom);
    scene.submitFrame (std::move (batches));
}

juce::String Views3D::describeMapping() const               { return impl->describe(); }
juce::String Views3D::setFormula (int axis, const juce::String& text) { return impl->setFormula (axis, text); }
juce::String Views3D::getFormula (int axis) const          { return impl->formulaText[juce::jlimit (0, 2, axis)]; }
void Views3D::reloadFromState()                            { impl->reloadFromState(); }

//==============================================================================
namespace
{
    class ViewCard : public juce::Component
    {
    public:
        ViewCard (int idx, std::function<void (int)> onPick, HelpTarget* help)
            : index (idx), pick (std::move (onPick)), hover (*this, "v3Mode", help)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void paint (juce::Graphics& g) override
        {
            using namespace Palette;
            const auto r = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (selected ? green.withAlpha (0.12f) : viewBg);
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (selected ? green : (isMouseOver (true) ? dim : panelEdge));
            g.drawRoundedRectangle (r, 6.0f, selected ? 1.6f : 1.0f);

            auto area = getLocalBounds().reduced (9, 6);
            g.setColour (selected ? green : text);
            g.setFont (Theme::font (13.0f, true));
            g.drawText (modeNames[index], area.removeFromTop (17), juce::Justification::centredLeft);
            g.setColour (dim);
            g.setFont (Theme::font (10.5f));
            g.drawFittedText (modeBlurbs[index], area, juce::Justification::topLeft, 2, 0.9f);
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override  { repaint(); }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (getLocalBounds().contains (e.getPosition()))
                pick (index);
        }

        const int index;
        bool selected = false;

    private:
        std::function<void (int)> pick;
        widgets_detail::HoverReporter hover;
    };

    void styleEditor (juce::TextEditor& ed)
    {
        ed.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain)));
        ed.setColour (juce::TextEditor::backgroundColourId, Palette::viewBg);
        ed.setColour (juce::TextEditor::textColourId, Palette::text);
        ed.setColour (juce::TextEditor::outlineColourId, Palette::panelEdge);
        ed.setColour (juce::TextEditor::focusedOutlineColourId, Palette::green);
        ed.setColour (juce::CaretComponent::caretColourId, Palette::green);
        ed.setMultiLine (false);
        ed.setReturnKeyStartsNewLine (false);
        ed.setIndents (6, 4);
    }
}

struct Views3DPanel::Impl : private juce::Timer
{
    Impl (Views3DPanel& o, ClodScopeProcessor& p, Views3D& v, HelpTarget* h)
        : owner (o), apvts (p.apvts), views (v), help (h)
    {
        for (int i = 0; i < 4; ++i)
        {
            auto* c = cards.add (new ViewCard (i, [this] (int idx) { pickMode (idx); }, help));
            owner.addAndMakeVisible (c);
        }

        auto knob = [&] (std::unique_ptr<ParamKnob>& k, const char* id, const char* label, bool bipolar = false)
        {
            k = std::make_unique<ParamKnob> (apvts, id, label, help, bipolar);
            owner.addChildComponent (*k);
        };
        knob (kWindow, "v3Window", "WINDOW");
        knob (kScale, "v3Scale", "SCALE");
        knob (kDepth, "v3Depth", "DEPTH");
        knob (kTwist, "v3Twist", "TWIST", true);
        knob (kTau, "v3Tau", "DELAY TAU");
        knob (kTrail, "v3Trail", "AFTERGLOW");
        knob (kThick, "v3Thick", "LINE WIDTH");
        knob (kSpin, "v3Spin", "AUTO SPIN", true);
        knob (kRows, "v3Rows", "ROWS");
        knob (kP, "v3P", "DIAL p", true);
        knob (kQ, "v3Q", "DIAL q", true);

        auto choice = [&] (std::unique_ptr<ParamChoice>& c, const char* id, const char* label)
        {
            c = std::make_unique<ParamChoice> (apvts, id, label, help);
            owner.addChildComponent (*c);
        };
        choice (cEmbed, "v3Embed", "EMBEDDING");
        choice (cStereoZ, "v3StereoZ", "STEREO DEPTH");
        choice (cColour, "v3Colour", "COLOUR");
        choice (cMap, "v3Map", "MAPPING");

        tTauAuto = std::make_unique<ParamToggle> (apvts, "v3TauAuto", "Auto tau (quarter period of the pitch)", help);
        owner.addChildComponent (*tTauAuto);

        const char* names[3] = { "X =", "Y =", "Z =" };
        const char* helpIds[3] = { "v3.formulaX", "v3.formulaY", "v3.formulaZ" };
        for (int k = 0; k < 3; ++k)
        {
            fName[k].setText (names[k], juce::dontSendNotification);
            fName[k].setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::bold)));
            fName[k].setColour (juce::Label::textColourId, Palette::text);
            fStatus[k].setFont (Theme::font (11.0f, true));
            fStatus[k].setJustificationType (juce::Justification::centredRight);
            styleEditor (fEdit[k]);
            fEdit[k].setText (views.getFormula (k), juce::dontSendNotification);
            fEdit[k].onTextChange = [this, k] { lastEditMs[k] = juce::Time::getMillisecondCounterHiRes(); pending[k] = true; };
            fEdit[k].onReturnKey = [this, k] { compile (k); };
            fEdit[k].onFocusLost = [this, k] { if (pending[k]) compile (k); };
            fHover[k] = std::make_unique<widgets_detail::HoverReporter> (fEdit[k], helpIds[k], help);
            owner.addChildComponent (fName[k]);
            owner.addChildComponent (fEdit[k]);
            owner.addChildComponent (fStatus[k]);
        }

        errorLine.setFont (Theme::font (11.0f));
        errorLine.setColour (juce::Label::textColourId, Palette::pink);
        errorLine.setJustificationType (juce::Justification::topLeft);
        owner.addChildComponent (errorLine);

        inactiveNote.setText ("Switch Mapping to 'Custom formulas' to draw with these.", juce::dontSendNotification);
        inactiveNote.setFont (Theme::font (11.0f));
        inactiveNote.setColour (juce::Label::textColourId, Palette::amber);
        owner.addChildComponent (inactiveNote);

        resetBtn.onClick = [this] { resetFormulas(); };
        varsBtn.setClickingTogglesState (true);
        varsBtn.setColour (juce::TextButton::buttonOnColourId, Palette::green.withAlpha (0.3f));
        varsBtn.onClick = [this] { showVars = varsBtn.getToggleState(); relayout (true); };
        varsHover = std::make_unique<widgets_detail::HoverReporter> (varsBtn, "v3.vars", help);
        owner.addChildComponent (resetBtn);
        owner.addChildComponent (varsBtn);

        const auto T = tauS(), W = omegaS();
        varsText.setText ("s    signal (L+R)/2           h    Hilbert of s (90 deg copy)\n"
                          "l r  left / right             m    mid (l+r)/2      sd  side (l-r)/2\n"
                          "d1   s delayed by " + T + "          d2   s delayed by 2" + T + "\n"
                          "v    (ds/dt) / " + W + "             a    (d2s/dt2) / " + W + "^2\n"
                          "u    0..1 along the window    t    seconds          f   pitch (Hz)\n"
                          "p q  your dials (-1..1)\n"
                          "fn   sin cos tan exp log sqrt abs pow min max clamp\n"
                          "     sat tri saw sqr atan2 hypot   const pi tau e\n"
                          "out  x = X*scale  y = Y*scale  z = Z*2*depth, then twist",
                          juce::dontSendNotification);
        varsText.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::plain)));
        varsText.setColour (juce::Label::textColourId, Palette::text);
        varsText.setColour (juce::Label::backgroundColourId, Palette::viewBg);
        varsText.setJustificationType (juce::Justification::topLeft);
        owner.addChildComponent (varsText);

        for (int k = 0; k < 3; ++k)
            showStatus (k, views.setFormula (k, views.getFormula (k)));

        startTimerHz (10);
    }

    ~Impl() override { stopTimer(); }

    float param (const char* id) const { return apvts.getRawParameterValue (id)->load(); }

    void pickMode (int idx)
    {
        if (auto* p = apvts.getParameter ("v3Mode"))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 ((float) idx));
            p->endChangeGesture();
        }
    }

    void showStatus (int k, const juce::String& err)
    {
        fStatus[k].setText (err.isEmpty() ? "OK" : "error", juce::dontSendNotification);
        fStatus[k].setColour (juce::Label::textColourId, err.isEmpty() ? Palette::green : Palette::pink);
        errors[k] = err;
        juce::String all;
        const char* n[3] = { "X", "Y", "Z" };
        for (int i = 0; i < 3; ++i)
            if (errors[i].isNotEmpty())
                all << n[i] << ": " << errors[i] << "\n";
        errorLine.setText (all.trimEnd(), juce::dontSendNotification);
    }

    void compile (int k)
    {
        pending[k] = false;
        showStatus (k, views.setFormula (k, fEdit[k].getText()));
    }

    void resetFormulas()
    {
        juce::String f[3];
        viewFormulas ((int) param ("v3Mode"), (int) param ("v3Embed"), (int) param ("v3StereoZ"), f);
        for (int k = 0; k < 3; ++k)
        {
            fEdit[k].setText (f[k], juce::dontSendNotification);
            compile (k);
        }
    }

    // Lays out (when apply) and returns the total height for this width
    int layout (int width, bool apply)
    {
        const int pad = 12, gap = 10;
        const int mode = juce::jlimit (0, 3, (int) param ("v3Mode"));
        const bool curveView = mode != waterfall;
        const bool phase = mode == phaseSpace, stereo = mode == stereo3D;
        const bool custom = (int) param ("v3Map") == 1;
        const bool zTime = mode == helix || (stereo && (int) param ("v3StereoZ") == 0) || (custom && curveView) || mode == waterfall;
        const int inner = juce::jmax (100, width - 2 * pad);

        // --- view cards
        int y = 0;
        const int cardH = 54, cardW = (inner - 8) / 2;
        if (apply)
            for (int i = 0; i < 4; ++i)
            {
                cards[i]->setBounds (pad + (i % 2) * (cardW + 8), y + 28 + (i / 2) * (cardH + 8), cardW, cardH);
                cards[i]->selected = i == mode;
                cards[i]->repaint();
            }
        const int viewH = 28 + 2 * cardH + 8 + 12;
        secView = { 0, y, width, viewH };
        y += viewH + gap;

        // --- shape controls for the active view
        std::vector<ParamKnob*> knobs;
        if (curveView)
        {
            knobs = { kWindow.get(), kScale.get() };
            if (zTime) knobs.push_back (kDepth.get());
            knobs.push_back (kTwist.get());
            if (phase) knobs.push_back (kTau.get());
            knobs.push_back (kTrail.get());
            knobs.push_back (kThick.get());
            knobs.push_back (kSpin.get());
        }
        else
            knobs = { kScale.get(), kDepth.get(), kRows.get(), kSpin.get() };

        std::vector<ParamChoice*> choices;
        if (phase) choices.push_back (cEmbed.get());
        if (stereo) choices.push_back (cStereoZ.get());
        if (curveView) choices.push_back (cColour.get());

        const int shapeTop = y;
        int sy = y + 28;
        const int cols = 4, cellW = inner / cols;
        for (size_t i = 0; i < knobs.size(); ++i)
            if (apply)
                knobs[i]->setBounds (pad + (int) (i % cols) * cellW, sy + (int) (i / cols) * Theme::knobCellHeight, cellW, Theme::knobCellHeight);
        sy += (int) ((knobs.size() + cols - 1) / cols) * Theme::knobCellHeight + 4;

        const int choiceW = (inner - 8) / 2;
        for (size_t i = 0; i < choices.size(); ++i)
            if (apply)
                choices[i]->setBounds (pad + (int) (i % 2) * (choiceW + 8), sy + (int) (i / 2) * Theme::choiceHeight, choiceW, 40);
        sy += (int) ((choices.size() + 1) / 2) * Theme::choiceHeight;

        if (phase)
        {
            if (apply) tTauAuto->setBounds (pad, sy, inner, 24);
            sy += 30;
        }
        secShape = { 0, shapeTop, width, sy - shapeTop + 6 };
        y = secShape.getBottom() + gap;

        if (apply)
        {
            for (auto* k : { kWindow.get(), kScale.get(), kDepth.get(), kTwist.get(), kTau.get(), kTrail.get(), kThick.get(), kSpin.get(), kRows.get() })
                k->setVisible (std::find (knobs.begin(), knobs.end(), k) != knobs.end());
            for (auto* c : { cEmbed.get(), cStereoZ.get(), cColour.get() })
                c->setVisible (std::find (choices.begin(), choices.end(), c) != choices.end());
            tTauAuto->setVisible (phase);
            kTau->slider.setEnabled (param ("v3TauAuto") < 0.5f);
        }

        // --- custom formulas (curve views only)
        secFormula = {};
        const bool showFormulas = curveView;
        if (showFormulas)
        {
            const int top = y;
            int fy = y + 28;
            if (apply)
            {
                cMap->setBounds (pad, fy, inner - 100, 40);
                varsBtn.setBounds (pad + inner - 92, fy + 16, 92, 24);
            }
            fy += Theme::choiceHeight;
            if (apply)
            {
                kP->setBounds (pad, fy, cellW, Theme::knobCellHeight);
                kQ->setBounds (pad + cellW, fy, cellW, Theme::knobCellHeight);
                kP->slider.setEnabled (custom);
                kQ->slider.setEnabled (custom);
            }
            fy += Theme::knobCellHeight;
            if (! custom)
            {
                if (apply) inactiveNote.setBounds (pad, fy, inner, 18);
                fy += 20;
            }
            for (int k = 0; k < 3; ++k)
            {
                if (apply)
                {
                    fName[k].setBounds (pad, fy, 34, 26);
                    fEdit[k].setBounds (pad + 36, fy, inner - 36 - 50, 26);
                    fStatus[k].setBounds (pad + inner - 48, fy, 48, 26);
                }
                fy += 30;
            }
            if (apply) errorLine.setBounds (pad, fy, inner, 30);
            fy += 32;
            if (apply) resetBtn.setBounds (pad, fy, juce::jmin (200, inner), 26);
            fy += 32;
            if (showVars)
            {
                if (apply) varsText.setBounds (pad, fy, inner, 150);
                fy += 156;
            }
            secFormula = { 0, top, width, fy - top + 4 };
            y = secFormula.getBottom();
        }
        else
            y -= gap;

        if (apply)
        {
            cMap->setVisible (showFormulas);
            kP->setVisible (showFormulas);
            kQ->setVisible (showFormulas);
            varsBtn.setVisible (showFormulas);
            resetBtn.setVisible (showFormulas);
            errorLine.setVisible (showFormulas);
            inactiveNote.setVisible (showFormulas && ! custom);
            varsText.setVisible (showFormulas && showVars);
            for (int k = 0; k < 3; ++k)
            {
                fName[k].setVisible (showFormulas);
                fEdit[k].setVisible (showFormulas);
                fStatus[k].setVisible (showFormulas);
                fEdit[k].setAlpha (custom ? 1.0f : 0.6f);
            }
        }
        return y;
    }

    void relayout (bool force)
    {
        const int w = owner.getWidth();
        if (w <= 0)
            return;
        const int h = layout (w, false);
        if (h != owner.getHeight())
            owner.setSize (w, h);   // resized() lays out
        else if (force)
            layout (w, true);
        owner.repaint();
    }

    void timerCallback() override
    {
        const int mode = (int) param ("v3Mode"), map = (int) param ("v3Map");
        const int embed = (int) param ("v3Embed"), stereoZ = (int) param ("v3StereoZ");
        const bool tauAuto = param ("v3TauAuto") > 0.5f;
        if (mode != lastMode || map != lastMap || embed != lastEmbed || stereoZ != lastStereoZ || tauAuto != lastTauAuto)
        {
            lastMode = mode; lastMap = map; lastEmbed = embed; lastStereoZ = stereoZ; lastTauAuto = tauAuto;
            relayout (true);
        }

        const double now = juce::Time::getMillisecondCounterHiRes();
        for (int k = 0; k < 3; ++k)
            if (pending[k] && now - lastEditMs[k] > 400.0)
                compile (k);
    }

    Views3DPanel& owner;
    juce::AudioProcessorValueTreeState& apvts;
    Views3D& views;
    HelpTarget* help;

    juce::OwnedArray<ViewCard> cards;
    std::unique_ptr<ParamKnob> kWindow, kScale, kDepth, kTwist, kTau, kTrail, kThick, kSpin, kRows, kP, kQ;
    std::unique_ptr<ParamChoice> cEmbed, cStereoZ, cColour, cMap;
    std::unique_ptr<ParamToggle> tTauAuto;

    juce::Label fName[3], fStatus[3], errorLine, inactiveNote, varsText;
    juce::TextEditor fEdit[3];
    std::unique_ptr<widgets_detail::HoverReporter> fHover[3], varsHover;
    juce::TextButton resetBtn { "Reset to view's formulas" }, varsBtn { "Variables" };
    bool showVars = false;
    juce::String errors[3];

    juce::Rectangle<int> secView, secShape, secFormula;
    int lastMode = -1, lastMap = -1, lastEmbed = -1, lastStereoZ = -1;
    bool lastTauAuto = false;
    double lastEditMs[3] {};
    bool pending[3] {};
};

Views3DPanel::Views3DPanel (ClodScopeProcessor& p, Views3D& v, HelpTarget* help)
{
    impl = std::make_unique<Impl> (*this, p, v, help);
}

Views3DPanel::~Views3DPanel()
{
    impl.reset();
}

int Views3DPanel::idealHeight (int width) const
{
    return impl->layout (width, false);
}

void Views3DPanel::paint (juce::Graphics& g)
{
    Theme::drawSection (g, impl->secView, "3D VIEW");
    Theme::drawSection (g, impl->secShape, "SHAPE   (picture only - sound unchanged)");
    if (! impl->secFormula.isEmpty())
        Theme::drawSection (g, impl->secFormula, "CUSTOM FORMULAS   (picture only)");
}

void Views3DPanel::resized()
{
    impl->layout (getWidth(), true);
}
