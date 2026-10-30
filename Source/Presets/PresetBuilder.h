#pragma once

#include "FactoryPresets.h"
#include <initializer_list>

// Fluent builder for the factory library, so a preset reads as one statement:
//
//   lib << P ("Warm Tape Drive", "Guitar", { "Blues", "Rock" }, "Soft tape saturation ...")
//              .set ("drive", 38.0f).choice ("shape", "Soft (tanh)").set ("lpf", 7500.0f);
//
// Values are real parameter values (dB, Hz, %, choice index or 0/1 for switches). Anything not set
// loads at its default. PresetManager::validateFactoryPresets() reports unknown ids, out-of-range
// values, unknown choice texts, categories and genres (also printed with DBG in debug builds).
namespace presetbuilder
{
    class P
    {
    public:
        P (const juce::String& name, const juce::String& category,
           std::initializer_list<const char*> genres, const juce::String& description)
        {
            p.name = name;
            p.category = category;
            for (auto* g : genres)
                p.genres.add (g);
            p.description = description;
        }

        P& set (const juce::String& id, float realValue)            { p.values.emplace_back (id, realValue); return *this; }
        P& on (const juce::String& id)                              { return set (id, 1.0f); }
        P& off (const juce::String& id)                             { return set (id, 0.0f); }
        P& choice (const juce::String& id, const juce::String& text) { p.choices.emplace_back (id, text); return *this; }

        // Wave Sculptor tables: a factory wave by name (SculptModel::waveNames()) or a serialised table
        P& waveA (const juce::String& waveName)                     { p.sculptTableA = "wave:" + waveName; return *this; }
        P& waveB (const juce::String& waveName)                     { p.sculptTableB = "wave:" + waveName; return *this; }
        P& tableA (const juce::String& serialised)                  { p.sculptTableA = serialised; return *this; }
        P& tableB (const juce::String& serialised)                  { p.sculptTableB = serialised; return *this; }

        // Equation stage with a custom formula (also selects "Custom formula" and switches the stage on)
        P& formula (const juce::String& text)
        {
            p.equationFormula = text;
            choice ("eqPreset", "Custom formula");
            return on ("eqOn");
        }

        //==============================================================================
        // Shorthands for the common blocks. Later set() calls override what these set.

        // Synth layer on. source: "Input", "MIDI", "Input + MIDI"; layer: "Blend", "Overtones", "Replace"
        // Replace follows the input's loudness 85 %: the synth keeps your picking dynamics, and string
        // noise (squeaks, thumps, a muted strum) stays as quiet against your notes as it was on the
        // guitar. Pads that should hold a steady level set a lower Follow after this.
        P& synth (const juce::String& source, const juce::String& layer, float levelDb = 0.0f)
        {
            on ("synOn");
            choice ("synSource", source);
            choice ("synLayer", layer);
            if (layer == "Replace")
                set ("synFollow", 85.0f);
            return set ("synLevel", levelDb);
        }

        // Chord tracking: up to `notes` input notes at once, each on its own voice; Scale = Chromatic keeps
        // every voice exactly on its note. pickPct lets the input's own attack through at each strum.
        P& chord (float pickPct = 0.0f, int notes = 6, float sensPct = 50.0f)
        {
            choice ("synTrackMode", "Chord");
            choice ("synScale", "Chromatic");
            set ("synChordNotes", (float) notes).set ("synChordSens", sensPct);
            return pickPct > 0.0f ? set ("synPickAttack", pickPct) : *this;
        }

        // Per-note drive (each voice distorted on its own). type: "Overdrive", "Distortion", "Fuzz"
        P& noteDrive (const juce::String& type, float amountPct, float tonePct = 50.0f)
        {
            choice ("synDriveType", type);
            return set ("synDrive", amountPct).set ("synDriveTone", tonePct);
        }

        P& wave (const juce::String& a, const juce::String& b = {}, float morph = 0.0f)
        {
            waveA (a);
            if (b.isNotEmpty())
                waveB (b);
            return set ("scMorph", morph);
        }

        P& env (float attackMs, float decayMs, float sustainPct, float releaseMs)
        {
            return set ("synAmpA", attackMs).set ("synAmpD", decayMs).set ("synAmpS", sustainPct).set ("synAmpR", releaseMs);
        }

        // type: "Ladder 24", "LP 12", "Band", "High", "Notch"
        P& filter (const juce::String& type, float cutoffHz, float resoPct, float envOct = 0.0f)
        {
            choice ("synFltType", type);
            return set ("synCutoff", cutoffHz).set ("synReso", resoPct).set ("synFltEnv", envOct);
        }

        P& fenv (float attackMs, float decayMs, float sustainPct, float releaseMs)
        {
            return set ("synFltA", attackMs).set ("synFltD", decayMs).set ("synFltS", sustainPct).set ("synFltR", releaseMs);
        }

        P& unison (int voices, float detuneCents, float spreadPct = 70.0f)
        {
            return set ("synUnison", (float) voices).set ("synDetune", detuneCents).set ("synSpread", spreadPct);
        }

        P& vibrato (float rateHz, float cents)
        {
            return set ("synLfoRate", rateHz).set ("synLfoPitch", cents);
        }

        // model: "Clean", "Crunch", "Lead", "High Gain", "Fuzz", "Bass", "Tube/Tape"; knobs 0..10
        P& amp (const juce::String& model, float gain, float bass = 5.0f, float mid = 5.0f, float treble = 5.0f, float presence = 5.0f)
        {
            on ("fxAmpOn");
            choice ("fxAmpModel", model);
            return set ("fxAmpGain", gain).set ("fxAmpBass", bass).set ("fxAmpMid", mid)
                     .set ("fxAmpTreble", treble).set ("fxAmpPresence", presence);
        }

        // model: "1x12 Open Back", "2x12 Combo", "4x12 Closed (V30)", "4x10 Bass", "1x8 Lo-Fi", "Off (DI)"
        P& cab (const juce::String& model, float micPct = 30.0f, float roomPct = 15.0f)
        {
            on ("fxCabOn");
            choice ("fxCabModel", model);
            return set ("fxCabMic", micPct).set ("fxCabRoom", roomPct);
        }

        P& comp (float threshDb, float ratio, float attackMs = 10.0f, float releaseMs = 120.0f)
        {
            on ("fxCompOn");
            return set ("fxCompThresh", threshDb).set ("fxCompRatio", ratio).set ("fxCompAttack", attackMs).set ("fxCompRelease", releaseMs);
        }

        // type: "Chorus", "Ensemble", "Flanger", "Phaser", "Vibrato", "Tremolo", "Auto-pan"
        P& mod (const juce::String& type, float rateHz, float depthPct, float mixPct = 50.0f)
        {
            on ("fxModOn");
            choice ("fxModType", type);
            return set ("fxModRate", rateHz).set ("fxModDepth", depthPct).set ("fxModMix", mixPct);
        }

        // type: "Digital", "Tape", "Ping-pong"; division e.g. "1/8D", "1/4", "1/16T"
        P& delay (const juce::String& type, const juce::String& division, float feedbackPct, float mixPct)
        {
            on ("fxDelayOn");
            choice ("fxDelayType", type);
            on ("fxDelaySync");
            choice ("fxDelayDiv", division);
            return set ("fxDelayFeedback", feedbackPct).set ("fxDelayMix", mixPct);
        }

        // type: "Room", "Hall", "Plate", "Chamber", "Shimmer"
        P& reverb (const juce::String& type, float mixPct, float decayS = 2.5f, float sizePct = 60.0f)
        {
            on ("fxRevOn");
            choice ("fxRevType", type);
            return set ("fxRevMix", mixPct).set ("fxRevDecay", decayS).set ("fxRevSize", sizePct);
        }

        P& eqBand (float lowCutHz, float highCutHz)
        {
            on ("fxEqOn");
            return set ("fxEqLowCut", lowCutHz).set ("fxEqHighCut", highCutHz);
        }

        // Equation stage with one of SoundEquationStage::presetNames() and its dials a..d (0..1)
        P& equation (const juce::String& family, float a, float b = 0.5f, float c = 0.5f, float d = 0.5f)
        {
            on ("eqOn");
            choice ("eqPreset", family);
            return set ("eqA", a).set ("eqB", b).set ("eqC", c).set ("eqD", d);
        }

        // Loudness trim (tuned from test renders) plus the safety limiter every preset ends with:
        // limiter on at a -1.2 dBFS ceiling; negative dB lower the output. Positive dB are taken as
        // CLEAN gain first - the synth level (Replace mode without an amp, up to +12 dB), the amp's
        // output level (up to +12 dB), the equation output (up to +18 dB) - and only what is left
        // drives the limiter, never by more than 6 dB: keeping attacks and dynamics intact matters
        // more than matching loudness exactly, so very quiet percussive patches stay a little quieter.
        P& level (float db)
        {
            on ("fxLimOn");
            set ("fxLimCeiling", -1.2f).set ("fxLimRelease", 120.0f);
            if (db <= 0.0f)
                return set ("outGain", db);

            auto raise = [this, &db] (const char* id, float current, float maxValue)
            {
                const float raised = juce::jmin (maxValue, current + db);
                if (raised > current)
                {
                    set (id, raised);
                    db -= raised - current;
                }
            };

            const bool ampOn = valueOf ("fxAmpOn", 0.0f) > 0.5f;
            if (choiceOf ("synLayer") == "Replace" && ! ampOn)
                raise ("synLevel", valueOf ("synLevel", -3.0f), 12.0f);
            if (db > 0.0f && ampOn)
                raise ("fxAmpLevel", valueOf ("fxAmpLevel", 0.0f), 12.0f);
            if (db > 0.0f && valueOf ("eqOn", 0.0f) > 0.5f)
                raise ("eqOut", valueOf ("eqOut", 0.0f), 18.0f);

            return db > 0.0f ? set ("fxLimInput", juce::jmin (6.0f, db)) : *this;
        }

        float valueOf (const juce::String& id, float fallback) const
        {
            for (auto it = p.values.rbegin(); it != p.values.rend(); ++it)
                if (it->first == id)
                    return it->second;
            return fallback;
        }

        juce::String choiceOf (const juce::String& id) const
        {
            for (auto it = p.choices.rbegin(); it != p.choices.rend(); ++it)
                if (it->first == id)
                    return it->second;
            return {};
        }

        operator FactoryPreset() const { return p; }
        const FactoryPreset& get() const { return p; }

    private:
        FactoryPreset p;
    };

    class Library
    {
    public:
        Library& operator<< (const P& preset) { list.push_back (preset.get()); return *this; }
        std::vector<FactoryPreset> take() { return std::move (list); }

    private:
        std::vector<FactoryPreset> list;
    };
}
