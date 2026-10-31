#include "FxInternal.h"
#include "FxHelp.h"

// Core of the rack: parameter layout, module switching (click-free crossfades), latency
// bookkeeping, meters and the live description. The stages live in the other Fx*.cpp files.

namespace
{
    using namespace juce;
    using FloatAttr = AudioParameterFloatAttributes;

    NormalisableRange<float> skewed (float lo, float hi, float centre, float step = 0.0f)
    {
        NormalisableRange<float> r (lo, hi, step);
        r.setSkewForCentre (centre);
        return r;
    }

    String hzText (float v)
    {
        return v < 1000.0f ? String (v, v < 100.0f ? 1 : 0) + " Hz" : String (v / 1000.0f, v < 10000.0f ? 2 : 1) + " kHz";
    }

    float hzFromText (const String& t)
    {
        const float v = t.getFloatValue();
        return t.containsIgnoreCase ("k") ? v * 1000.0f : v;
    }

    String msText (float v)
    {
        if (v >= 1000.0f)
            return String (v / 1000.0f, 2) + " s";
        return String (v, v < 10.0f ? 2 : (v < 100.0f ? 1 : 0)) + " ms";
    }

    String dbText (float v)   { return String (v, 1) + " dB"; }
    String pctText (float v)  { return String (juce::roundToInt (v)) + "%"; }
    String knobText (float v) { return String (v, 1); }

    FloatAttr hzAttr (float offBelow = -1.0f, float offAbove = 1.0e9f)
    {
        return FloatAttr()
            .withStringFromValueFunction ([offBelow, offAbove] (float v, int)
            {
                if (v <= offBelow || v >= offAbove)
                    return String ("Off");
                return hzText (v);
            })
            .withValueFromStringFunction ([offBelow, offAbove] (const String& t)
            {
                if (t.trim().equalsIgnoreCase ("off"))
                    return offBelow > 0.0f ? offBelow - 1.0f : offAbove + 1.0f;
                return hzFromText (t);
            });
    }

    FloatAttr msAttr()
    {
        return FloatAttr().withStringFromValueFunction ([] (float v, int) { return msText (v); })
                          .withValueFromStringFunction ([] (const String& t)
                          {
                              const float v = t.getFloatValue();
                              const auto s = t.trim().toLowerCase();
                              return (s.endsWith ("s") && ! s.endsWith ("ms")) ? v * 1000.0f : v;
                          });
    }

    FloatAttr dbAttr()   { return FloatAttr().withStringFromValueFunction ([] (float v, int) { return dbText (v); }); }
    FloatAttr pctAttr()  { return FloatAttr().withStringFromValueFunction ([] (float v, int) { return pctText (v); }); }
    FloatAttr knobAttr() { return FloatAttr().withStringFromValueFunction ([] (float v, int) { return knobText (v); }); }

    void addFloat (ParamList& p, const char* id, const char* name, NormalisableRange<float> r, float def, FloatAttr a)
    {
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def, a));
    }

    void addBool (ParamList& p, const char* id, const char* name, bool def)
    {
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def));
    }

    void addChoice (ParamList& p, const char* id, const char* name, const StringArray& c, int def)
    {
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, c, def));
    }

    NormalisableRange<float> lin (float lo, float hi, float step = 0.01f) { return NormalisableRange<float> (lo, hi, step); }
}

//==============================================================================
void FxRack::addParameters (ParamList& p)
{
    // ---- Amp ----
    addBool   (p, "fxAmpOn", "Amp On", false);
    addChoice (p, "fxAmpModel", "Amp Model", fx::AmpStage::modelNames(), 1);
    addFloat  (p, "fxAmpGain", "Amp Gain", lin (0.0f, 10.0f), 5.0f, knobAttr());
    addFloat  (p, "fxAmpBass", "Amp Bass", lin (0.0f, 10.0f), 5.0f, knobAttr());
    addFloat  (p, "fxAmpMid", "Amp Mid", lin (0.0f, 10.0f), 5.0f, knobAttr());
    addFloat  (p, "fxAmpTreble", "Amp Treble", lin (0.0f, 10.0f), 5.0f, knobAttr());
    addFloat  (p, "fxAmpPresence", "Amp Presence", lin (0.0f, 10.0f), 5.0f, knobAttr());
    addFloat  (p, "fxAmpLevel", "Amp Level", lin (-24.0f, 12.0f, 0.1f), 0.0f, dbAttr());
    addFloat  (p, "fxAmpMix", "Amp Mix", lin (0.0f, 100.0f, 0.1f), 100.0f, pctAttr());
    addFloat  (p, "fxAmpGate", "Amp Gate", lin (-100.0f, -30.0f, 0.1f), -100.0f,
               FloatAttr().withStringFromValueFunction ([] (float v, int) { return v <= -99.9f ? String ("Off") : dbText (v); }));
    addBool   (p, "fxAmpBright", "Amp Bright", false);

    // ---- Cab ----
    addBool   (p, "fxCabOn", "Cab On", false);
    addChoice (p, "fxCabModel", "Cab Model", fx::CabStage::modelNames(), 1);
    addFloat  (p, "fxCabMic", "Cab Mic Position", lin (0.0f, 100.0f, 0.1f), 30.0f, pctAttr());
    addFloat  (p, "fxCabRoom", "Cab Room Mic", lin (0.0f, 100.0f, 0.1f), 15.0f, pctAttr());
    addFloat  (p, "fxCabLevel", "Cab Level", lin (-12.0f, 12.0f, 0.1f), 0.0f, dbAttr());
    addFloat  (p, "fxCabLowCut", "Cab Low Cut", skewed (20.0f, 400.0f, 80.0f, 0.1f), 40.0f, hzAttr (20.05f));
    addFloat  (p, "fxCabHighCut", "Cab High Cut", skewed (2000.0f, 20000.0f, 8000.0f, 1.0f), 16000.0f, hzAttr (-1.0f, 19990.0f));

    // ---- EQ ----
    addBool   (p, "fxEqOn", "EQ On", false);
    addFloat  (p, "fxEqLowCut", "EQ Low Cut", skewed (20.0f, 1000.0f, 120.0f, 0.1f), 20.0f, hzAttr (20.5f));
    addFloat  (p, "fxEqLowFreq", "EQ Low Shelf Freq", skewed (30.0f, 500.0f, 120.0f, 0.1f), 100.0f, hzAttr());
    addFloat  (p, "fxEqLowGain", "EQ Low Shelf Gain", lin (-15.0f, 15.0f, 0.1f), 0.0f, dbAttr());
    addFloat  (p, "fxEqMid1Freq", "EQ Mid 1 Freq", skewed (80.0f, 5000.0f, 600.0f, 0.1f), 400.0f, hzAttr());
    addFloat  (p, "fxEqMid1Gain", "EQ Mid 1 Gain", lin (-15.0f, 15.0f, 0.1f), 0.0f, dbAttr());
    addFloat  (p, "fxEqMid1Q", "EQ Mid 1 Q", skewed (0.3f, 8.0f, 1.2f, 0.01f), 1.0f, knobAttr());
    addFloat  (p, "fxEqMid2Freq", "EQ Mid 2 Freq", skewed (300.0f, 15000.0f, 2500.0f, 1.0f), 2500.0f, hzAttr());
    addFloat  (p, "fxEqMid2Gain", "EQ Mid 2 Gain", lin (-15.0f, 15.0f, 0.1f), 0.0f, dbAttr());
    addFloat  (p, "fxEqMid2Q", "EQ Mid 2 Q", skewed (0.3f, 8.0f, 1.2f, 0.01f), 1.0f, knobAttr());
    addFloat  (p, "fxEqHighFreq", "EQ High Shelf Freq", skewed (1500.0f, 16000.0f, 6000.0f, 1.0f), 8000.0f, hzAttr());
    addFloat  (p, "fxEqHighGain", "EQ High Shelf Gain", lin (-15.0f, 15.0f, 0.1f), 0.0f, dbAttr());
    addFloat  (p, "fxEqHighCut", "EQ High Cut", skewed (1000.0f, 20000.0f, 6000.0f, 1.0f), 20000.0f, hzAttr (-1.0f, 19900.0f));
    addFloat  (p, "fxEqOutput", "EQ Output", lin (-12.0f, 12.0f, 0.1f), 0.0f, dbAttr());

    // ---- Comp ----
    addBool   (p, "fxCompOn", "Comp On", false);
    addFloat  (p, "fxCompThresh", "Comp Threshold", lin (-60.0f, 0.0f, 0.1f), -18.0f, dbAttr());
    addFloat  (p, "fxCompRatio", "Comp Ratio", skewed (1.0f, 20.0f, 4.0f, 0.01f), 4.0f,
               FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 1 : 0) + ":1"; }));
    addFloat  (p, "fxCompAttack", "Comp Attack", skewed (0.1f, 100.0f, 10.0f, 0.01f), 10.0f, msAttr());
    addFloat  (p, "fxCompRelease", "Comp Release", skewed (10.0f, 1000.0f, 150.0f, 0.1f), 120.0f, msAttr());
    addFloat  (p, "fxCompKnee", "Comp Knee", lin (0.0f, 24.0f, 0.1f), 6.0f, dbAttr());
    addFloat  (p, "fxCompMakeup", "Comp Makeup", lin (-12.0f, 24.0f, 0.1f), 0.0f, dbAttr());
    addBool   (p, "fxCompAuto", "Comp Auto Makeup", true);
    addFloat  (p, "fxCompMix", "Comp Mix", lin (0.0f, 100.0f, 0.1f), 100.0f, pctAttr());
    addFloat  (p, "fxCompScHpf", "Comp Sidechain HPF", skewed (20.0f, 300.0f, 80.0f, 0.1f), 20.0f, hzAttr (20.5f));

    // ---- Mod ----
    addBool   (p, "fxModOn", "Mod On", false);
    addChoice (p, "fxModType", "Mod Type", fx::ModStage::typeNames(), 0);
    addFloat  (p, "fxModRate", "Mod Rate", skewed (0.02f, 10.0f, 1.0f, 0.001f), 0.8f,
               FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, v < 1.0f ? 2 : 1) + " Hz"; }));
    addBool   (p, "fxModSync", "Mod Sync", false);
    addChoice (p, "fxModDiv", "Mod Sync Rate", fx::syncNames (true), 16);
    addFloat  (p, "fxModDepth", "Mod Depth", lin (0.0f, 100.0f, 0.1f), 50.0f, pctAttr());
    addFloat  (p, "fxModFeedback", "Mod Feedback", lin (-95.0f, 95.0f, 0.1f), 0.0f, pctAttr());
    addFloat  (p, "fxModMix", "Mod Mix", lin (0.0f, 100.0f, 0.1f), 50.0f, pctAttr());
    addFloat  (p, "fxModWidth", "Mod Width", lin (0.0f, 100.0f, 0.1f), 100.0f, pctAttr());
    addChoice (p, "fxModShape", "Mod LFO Shape", StringArray { "Sine", "Triangle", "Square" }, 0);
    addFloat  (p, "fxModTone", "Mod Tone", skewed (1000.0f, 20000.0f, 6000.0f, 1.0f), 16000.0f, hzAttr (-1.0f, 19900.0f));

    // ---- Delay ----
    addBool   (p, "fxDelayOn", "Delay On", false);
    addChoice (p, "fxDelayType", "Delay Type", fx::DelayStage::typeNames(), 0);
    addBool   (p, "fxDelaySync", "Delay Sync", true);
    addChoice (p, "fxDelayDiv", "Delay Sync Time", fx::syncNames (false), 10);
    addFloat  (p, "fxDelayTime", "Delay Time", skewed (1.0f, 2000.0f, 300.0f, 0.01f), 375.0f, msAttr());
    addFloat  (p, "fxDelayFeedback", "Delay Feedback", lin (0.0f, 100.0f, 0.1f), 35.0f, pctAttr());
    addFloat  (p, "fxDelayMix", "Delay Mix", lin (0.0f, 100.0f, 0.1f), 25.0f, pctAttr());
    addFloat  (p, "fxDelayLowCut", "Delay Low Cut", skewed (20.0f, 1000.0f, 150.0f, 0.1f), 80.0f, hzAttr (20.5f));
    addFloat  (p, "fxDelayHighCut", "Delay High Cut", skewed (1000.0f, 20000.0f, 6000.0f, 1.0f), 8000.0f, hzAttr (-1.0f, 19900.0f));
    addFloat  (p, "fxDelayDuck", "Delay Ducking", lin (0.0f, 100.0f, 0.1f), 0.0f, pctAttr());
    addFloat  (p, "fxDelayWidth", "Delay Width", lin (0.0f, 100.0f, 0.1f), 100.0f, pctAttr());
    addFloat  (p, "fxDelayWow", "Delay Wow/Flutter", lin (0.0f, 100.0f, 0.1f), 30.0f, pctAttr());
    addFloat  (p, "fxDelaySat", "Delay Saturation", lin (0.0f, 100.0f, 0.1f), 30.0f, pctAttr());

    // ---- Reverb ----
    addBool   (p, "fxRevOn", "Reverb On", false);
    addChoice (p, "fxRevType", "Reverb Type", fx::ReverbStage::typeNames(), 1);
    addFloat  (p, "fxRevPreDelay", "Reverb Pre-delay", skewed (0.0f, 250.0f, 40.0f, 0.1f), 20.0f, msAttr());
    addFloat  (p, "fxRevSize", "Reverb Size", lin (0.0f, 100.0f, 0.1f), 60.0f, pctAttr());
    addFloat  (p, "fxRevDecay", "Reverb Decay", skewed (0.2f, 20.0f, 2.5f, 0.01f), 2.5f,
               FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 2 : 1) + " s"; }));
    addFloat  (p, "fxRevDamping", "Reverb Damping", lin (0.0f, 100.0f, 0.1f), 40.0f, pctAttr());
    addFloat  (p, "fxRevLowCut", "Reverb Low Cut", skewed (20.0f, 1000.0f, 150.0f, 0.1f), 100.0f, hzAttr (20.5f));
    addFloat  (p, "fxRevMod", "Reverb Modulation", lin (0.0f, 100.0f, 0.1f), 30.0f, pctAttr());
    addFloat  (p, "fxRevWidth", "Reverb Width", lin (0.0f, 100.0f, 0.1f), 100.0f, pctAttr());
    addFloat  (p, "fxRevMix", "Reverb Mix", lin (0.0f, 100.0f, 0.1f), 25.0f, pctAttr());
    addFloat  (p, "fxRevShimmer", "Reverb Shimmer", lin (0.0f, 100.0f, 0.1f), 50.0f, pctAttr());

    // ---- Limiter ----
    addBool   (p, "fxLimOn", "Limiter On", false);
    addFloat  (p, "fxLimInput", "Limiter Input", lin (0.0f, 24.0f, 0.1f), 0.0f, dbAttr());
    addFloat  (p, "fxLimCeiling", "Limiter Ceiling", lin (-12.0f, 0.0f, 0.1f), -0.3f, dbAttr());
    addFloat  (p, "fxLimRelease", "Limiter Release", skewed (10.0f, 1000.0f, 100.0f, 0.1f), 100.0f, msAttr());
}

HelpList FxRack::helpEntries()
{
    return fx::helpList();
}

juce::String FxRack::moduleName (Module m)
{
    static const char* names[] = { "Amp", "Cab", "EQ", "Comp", "Mod", "Delay", "Reverb", "Limiter" };
    return (m >= 0 && m < numModules) ? names[m] : "";
}

juce::String FxRack::moduleEnableId (Module m)
{
    static const char* ids[] = { "fxAmpOn", "fxCabOn", "fxEqOn", "fxCompOn", "fxModOn", "fxDelayOn", "fxRevOn", "fxLimOn" };
    return (m >= 0 && m < numModules) ? ids[m] : "";
}

juce::String FxRack::moduleSummary (Module m)
{
    switch (m)
    {
        case amp:     return "Amp: 7 tube-style amp models (clean to fuzz, bass, tape), tone stack + presence, 4x oversampled";
        case cab:     return "Cab: speaker cabinet + microphone simulation (6 cabs, mic position, room mic), convolution";
        case eq:      return "EQ: low cut, low shelf, two parametric mids, high shelf, high cut, with live curve";
        case comp:    return "Comp: compressor with soft knee, auto makeup, parallel mix and gain-reduction meter";
        case mod:     return "Mod: chorus, ensemble, flanger, phaser, vibrato, tremolo, auto-pan; free or tempo-synced";
        case delay:   return "Delay: digital, tape (wow, flutter, saturation) or ping-pong; synced or in ms";
        case reverb:  return "Reverb: 16-line modulated FDN - room, hall, plate, chamber, shimmer (octave-up)";
        case limiter: return "Limiter: look-ahead brickwall limiter with ceiling and release, last in the chain";
        case numModules:
        default:      break;
    }
    return {};
}

//==============================================================================
FxRack::Impl::Impl (juce::AudioProcessorValueTreeState& s) : apvts (s), worker (*this)
{
    amp.bind (s);
    cab.bind (s);
    eq.bind (s);
    comp.bind (s);
    mod.bind (s);
    delay.bind (s);
    reverb.bind (s);
    limiter.bind (s);

    for (int m = 0; m < numModules; ++m)
    {
        slots[(size_t) m].on = fx::raw (s, moduleEnableId ((Module) m).toRawUTF8());
        peak[(size_t) m].store (0.0f);
    }
    slots[FxRack::amp].mix = fx::raw (s, "fxAmpMix");
    slots[FxRack::comp].mix = fx::raw (s, "fxCompMix");
}

FxRack::Impl::~Impl()
{
    worker.stopThread (4000);
}

void FxRack::Impl::processChunk (float* const* ch, int numCh, int n, const Transport& t, bool runRack, bool runLimiter) noexcept
{
    float* d[fx::maxCh] = { dry.getWritePointer (0), dry.getWritePointer (1) };

    auto measure = [&] (Module m)
    {
        float pk = 0.0f;
        for (int c = 0; c < numCh; ++c)
            for (int i = 0; i < n; ++i)
                pk = std::max (pk, std::abs (ch[c][i]));
        // peak hold with ~300 ms release, so panels can poll at any rate
        auto& a = peak[(size_t) m];
        const float decayed = a.load (std::memory_order_relaxed) * std::exp (-(float) n / (0.3f * (float) sampleRate));
        a.store (std::max (pk, decayed), std::memory_order_relaxed);
    };

    // Modules without tails: crossfade between the (time-aligned) dry signal and the module output.
    // The parallel mix of Amp and Comp rides on the same crossfade.
    // App (liveLatency): a module with latency (bypass != nullptr) adds no delay while it is off. When it
    // comes on, the output fades from the undelayed input u to the time-aligned result (slot.align 0 -> 1,
    // alongside the wet fade), and back when it goes off, so the timing change never clicks.
    float* u[fx::maxCh] = { dry.getWritePointer (fx::maxCh), dry.getWritePointer (fx::maxCh + 1) };
    auto runFaded = [&] (Module m, auto&& resetFn, auto&& processFn, fx::FixedDelay* bypass)
    {
        auto& s = slots[(size_t) m];
        const bool on = fx::get (s.on) > 0.5f;
        const bool live = liveLatency && bypass != nullptr;
        if (! s.active && ! on)
        {
            if (live)
            {
                // the app adds no delay here, but keeps the delay's history current (on a copy) so the
                // module can come in time-aligned
                for (int c = 0; c < numCh; ++c)
                    juce::FloatVectorOperations::copy (d[c], ch[c], n);
                bypass->process (d, numCh, n);
            }
            else if (bypass != nullptr)
            {
                bypass->process (ch, numCh, n);
            }
            peak[(size_t) m].store (0.0f, std::memory_order_relaxed);
            return;
        }
        if (! s.active)
        {
            resetFn();
            s.active = true;
            // the limiter is a safety stage: it takes hold at once instead of fading in (it can only
            // lower peaks, so switching it on abruptly cannot click; in the app the align fade below
            // covers the timing step)
            s.wet = (m == FxRack::limiter && on) ? 1.0f : 0.0f;
            if (live)
                s.align = 0.0f;
            if (m == FxRack::limiter && s.wet >= 1.0f && bypass != nullptr && bypass->len > 0)
            {
                // feed the limiter the samples its bypass delay still holds (oldest first), so its own
                // look-ahead delay holds the same history: taking hold at once then leaves no gap
                for (int done = 0; done < bypass->len;)
                {
                    const int k = std::min (bypass->len - done, maxBlock);
                    for (int c = 0; c < numCh; ++c)
                        for (int i = 0; i < k; ++i)
                            d[c][i] = bypass->buf[c][(size_t) ((bypass->pos + done + i) % bypass->len)];
                    this->limiter.process (d, numCh, k);
                    done += k;
                }
            }
        }

        const float target = on ? (s.mix != nullptr ? juce::jlimit (0.0f, 1.0f, fx::get (s.mix) * 0.01f) : 1.0f) : 0.0f;
        const bool steady = s.wet == target && target >= 1.0f;
        const bool aligning = live && (! on || s.align < 1.0f);
        if (steady && bypass == nullptr)
        {
            processFn();
            measure (m);
            return;
        }

        if (aligning)
            for (int c = 0; c < numCh; ++c)
                juce::FloatVectorOperations::copy (u[c], ch[c], n);
        for (int c = 0; c < numCh; ++c)
            juce::FloatVectorOperations::copy (d[c], ch[c], n);
        if (bypass != nullptr)
            bypass->process (d, numCh, n);
        processFn();

        if (! steady)
        {
            float w = s.wet;
            for (int i = 0; i < n; ++i)
            {
                if (w < target)      w = std::min (target, w + fadeStep);
                else if (w > target) w = std::max (target, w - fadeStep);
                for (int c = 0; c < numCh; ++c)
                    ch[c][i] = d[c][i] + w * (ch[c][i] - d[c][i]);
            }
            s.wet = w;
        }
        if (aligning)
        {
            const float aTarget = on ? 1.0f : 0.0f;
            float a = s.align;
            for (int i = 0; i < n; ++i)
            {
                if (a < aTarget)      a = std::min (aTarget, a + fadeStep);
                else if (a > aTarget) a = std::max (aTarget, a - fadeStep);
                for (int c = 0; c < numCh; ++c)
                    ch[c][i] = u[c][i] + a * (ch[c][i] - u[c][i]);
            }
            s.align = a;
        }
        if (! on && s.wet <= 0.0f && (! live || s.align <= 0.0f))
            s.active = false;
        measure (m);
    };

    // Delay and reverb fade their input instead, so repeats and tails ring out after switching off
    auto runTail = [&] (Module m, auto&& processFn, auto&& resetFn)
    {
        auto& s = slots[(size_t) m];
        const bool on = fx::get (s.on) > 0.5f;
        if (! s.active && ! on)
        {
            peak[(size_t) m].store (0.0f, std::memory_order_relaxed);
            return;
        }
        if (! s.active)
        {
            resetFn();
            s.active = true;
        }
        s.active = processFn (on);
        measure (m);
    };

    if (runRack)
    {
        runFaded (FxRack::amp,  [this] { this->amp.reset(); },  [&, this] { this->amp.process (ch, numCh, n); }, &ampBypass);
        runFaded (FxRack::cab,  [this] { this->cab.reset(); },  [&, this] { this->cab.process (ch, numCh, n); }, nullptr);
        runFaded (FxRack::eq,   [this] { this->eq.reset(); },   [&, this] { this->eq.process (ch, numCh, n); }, nullptr);
        runFaded (FxRack::comp, [this] { this->comp.reset(); }, [&, this] { this->comp.process (ch, numCh, n); }, nullptr);
        runFaded (FxRack::mod,  [this] { this->mod.reset(); },  [&, this] { this->mod.process (ch, numCh, n, t); }, nullptr);
        runTail (FxRack::delay,  [&, this] (bool on) { return this->delay.process (ch, numCh, n, t, on); }, [this] { this->delay.reset(); });
        runTail (FxRack::reverb, [&, this] (bool on) { return this->reverb.process (ch, numCh, n, on); },   [this] { this->reverb.reset(); });
    }
    if (runLimiter)
        runFaded (FxRack::limiter, [this] { this->limiter.reset(); }, [&, this] { this->limiter.process (ch, numCh, n); }, &limBypass);

    if (! slots[FxRack::comp].active)
        comp.grDb.store (0.0f, std::memory_order_relaxed);
    if (! slots[FxRack::limiter].active)
        limiter.grDb.store (0.0f, std::memory_order_relaxed);
}

//==============================================================================
// The global dry path's delay: a ring that is always written, read at the current latency. When the
// latency changes (app: a module with latency switched on or off) it crossfades from the old read tap
// to the new one, so the dry signal never jumps.
struct FxRack::DryAlign
{
    juce::AudioBuffer<float> ring;
    int mask = 0, pos = 0, cur = 0, prev = 0;
    float fade = 1.0f, step = 0.001f;

    void prepare (int maxLatency, double sampleRate, int latency)
    {
        ring.setSize (fx::maxCh, juce::nextPowerOfTwo (juce::jmax (1, maxLatency) + 1));
        mask = ring.getNumSamples() - 1;
        step = (float) (1.0 / (0.01 * sampleRate));   // 10 ms
        reset (latency);
    }

    void reset (int latency) noexcept
    {
        ring.clear();
        pos = 0;
        cur = prev = juce::jlimit (0, mask, latency);
        fade = 1.0f;
    }

    void process (float* const* ch, int numCh, int n, int target) noexcept
    {
        target = juce::jlimit (0, mask, target);
        if (fade >= 1.0f && target != cur)   // a change during a fade waits for it to finish
        {
            prev = cur;
            cur = target;
            fade = 0.0f;
        }
        float f = fade;
        for (int c = 0; c < numCh; ++c)
        {
            float* r = ring.getWritePointer (c);
            float* x = ch[c];
            int p = pos;
            f = fade;
            for (int i = 0; i < n; ++i)
            {
                r[p] = x[i];
                const float a = r[(p - cur) & mask];
                if (f < 1.0f)
                {
                    const float b = r[(p - prev) & mask];
                    x[i] = b + f * (a - b);
                    f = std::min (1.0f, f + step);
                }
                else
                {
                    x[i] = a;
                }
                p = (p + 1) & mask;
            }
        }
        if (numCh > 0)
        {
            pos = (pos + n) & mask;
            fade = f;
        }
    }
};

FxRack::FxRack (juce::AudioProcessorValueTreeState& s)
    : impl (std::make_unique<Impl> (s)), dryAlign (std::make_unique<DryAlign>()) {}
FxRack::~FxRack() = default;

void FxRack::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    auto& m = *impl;
    m.isPrepared = false;
    m.worker.stopThread (4000);

    m.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    m.maxBlock = juce::jmax (1, maxBlockSize);
    m.channels = juce::jlimit (1, fx::maxCh, numChannels);
    m.dry.setSize (2 * fx::maxCh, m.maxBlock);   // [0, maxCh): time-aligned dry, [maxCh, 2 maxCh): undelayed (app)
    m.dry.clear();

    m.amp.prepare (m.sampleRate, m.maxBlock, m.channels);
    m.cab.prepare (m.sampleRate, m.maxBlock, m.channels);
    m.eq.prepare (m.sampleRate, m.maxBlock, m.channels);
    m.comp.prepare (m.sampleRate, m.maxBlock, m.channels);
    m.mod.prepare (m.sampleRate, m.maxBlock, m.channels);
    m.delay.prepare (m.sampleRate, m.maxBlock, m.channels);
    m.reverb.prepare (m.sampleRate, m.maxBlock, m.channels);
    m.limiter.prepare (m.sampleRate, m.maxBlock, m.channels);

    m.latAmp = m.amp.latency();
    m.latLim = m.limiter.latency();
    m.ampBypass.prepare (m.latAmp);
    m.limBypass.prepare (m.latLim);
    m.fadeStep = (float) (1.0 / (0.02 * m.sampleRate));   // 20 ms switch crossfade

    for (auto& s : m.slots)
    {
        s.active = false;
        s.wet = 0.0f;
        s.align = m.liveLatency ? 0.0f : 1.0f;
    }
    dryAlign->prepare (m.latAmp + m.latLim, m.sampleRate, latencySamples());
    for (auto& p : m.peak)
        p.store (0.0f);

    m.isPrepared = true;
    m.worker.startThread (juce::Thread::Priority::low);
}

void FxRack::reset()
{
    auto& m = *impl;
    m.amp.reset();
    m.cab.reset();
    m.eq.reset();
    m.comp.reset();
    m.mod.reset();
    m.delay.reset();
    m.reverb.reset();
    m.limiter.reset();
    m.ampBypass.clear();
    m.limBypass.clear();
    // start in the state the parameters ask for, so nothing fades in after a reset (a preset loaded
    // before playback starts sounds complete - and limited - from the first sample)
    for (auto& s : m.slots)
    {
        const bool on = s.on != nullptr && fx::get (s.on) > 0.5f;
        s.active = on;
        s.wet = on ? (s.mix != nullptr ? juce::jlimit (0.0f, 1.0f, fx::get (s.mix) * 0.01f) : 1.0f) : 0.0f;
        s.align = (on || ! m.liveLatency) ? 1.0f : 0.0f;
    }
    dryAlign->reset (latencySamples());
}

void FxRack::setLiveLatency (bool live) noexcept
{
    impl->liveLatency = live;
}

int FxRack::maxLatencySamples() const noexcept
{
    return impl->latAmp + impl->latLim;
}

int FxRack::latencySamples() const noexcept
{
    auto& m = *impl;
    if (! m.liveLatency)
        return m.latAmp + m.latLim;
    // an active module is (fading) in the path; the dry path's own 10 ms crossfade follows it
    return (m.slots[FxRack::amp].active ? m.latAmp : 0) + (m.slots[FxRack::limiter].active ? m.latLim : 0);
}

void FxRack::alignDry (juce::AudioBuffer<float>& buffer, int numChannels, int numSamples) noexcept
{
    const int numCh = juce::jlimit (0, juce::jmin (fx::maxCh, buffer.getNumChannels()), numChannels);
    float* ch[fx::maxCh] = { numCh > 0 ? buffer.getWritePointer (0) : nullptr, numCh > 1 ? buffer.getWritePointer (1) : nullptr };
    dryAlign->process (ch, numCh, juce::jmin (numSamples, buffer.getNumSamples()), latencySamples());
}

void FxRack::Impl::processBuffer (juce::AudioBuffer<float>& buffer, int numChannels, const Transport& t,
                                  bool runRack, bool runLimiter) noexcept
{
    if (! isPrepared.load (std::memory_order_acquire))
        return;

    juce::ScopedNoDenormals noDenormals;
    if (runRack)
        lastBpm.store (t.bpm, std::memory_order_relaxed);

    const int numCh = juce::jlimit (0, juce::jmin (channels, buffer.getNumChannels()), numChannels);
    if (numCh <= 0)
        return;

    const int total = buffer.getNumSamples();
    for (int off = 0; off < total; off += maxBlock)
    {
        const int n = juce::jmin (maxBlock, total - off);
        float* ch[fx::maxCh] = { buffer.getWritePointer (0, off), numCh > 1 ? buffer.getWritePointer (1, off) : nullptr };
        Transport tt = t;
        if (off > 0)
            tt.ppqPosition += (double) off / sampleRate * t.bpm / 60.0;
        processChunk (ch, numCh, n, tt, runRack, runLimiter);
    }
}

void FxRack::process (juce::AudioBuffer<float>& buffer, int numChannels, const Transport& t) noexcept
{
    impl->processBuffer (buffer, numChannels, t, true, true);
}

void FxRack::process (juce::AudioBuffer<float>& buffer, int numChannels, const Transport& t, bool runLimiter) noexcept
{
    impl->processBuffer (buffer, numChannels, t, true, runLimiter);
}

void FxRack::processLimiter (juce::AudioBuffer<float>& buffer, int numChannels) noexcept
{
    impl->processBuffer (buffer, numChannels, Transport {}, false, true);
}

float FxRack::meterDb (Module which) const noexcept
{
    auto& m = *impl;
    switch (which)
    {
        case comp:    return m.comp.grDb.load (std::memory_order_relaxed);
        case limiter: return m.limiter.grDb.load (std::memory_order_relaxed);
        case amp: case cab: case eq: case mod: case delay: case reverb:
            return fx::gainToDb (m.peak[(size_t) which].load (std::memory_order_relaxed));
        case numModules:
        default: break;
    }
    return -100.0f;
}

juce::String FxRack::describeMapping() const
{
    return impl->describe();
}

std::unique_ptr<ControlPanel> FxRack::createPanel (Module m, HelpTarget* help)
{
    return Impl::makePanel (*impl, m, help);
}
