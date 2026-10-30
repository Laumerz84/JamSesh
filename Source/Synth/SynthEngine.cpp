#include "SynthEngine.h"
#include "SynthInternal.h"
#include "ChordName.h"
#include <cstdio>

// Synth layer engine: parameters, help, MIDI + tracked voice management, LFO, layer mix.
// The block is processed in chunks of <= 32 samples: tracker -> voice logic -> voices -> mix.
// MIDI events are applied at the start of the chunk they fall in (<= 0.7 ms timing error).

using namespace synth;

static_assert (std::tuple_size<decltype (TrackerSnapshot::notes)>::value == (size_t) maxChordNotes,
               "TrackerSnapshot publishes one entry per chord voice");

namespace
{
    const char* const scaleNames[] = { "Off (free pitch)", "Chromatic", "Major", "Minor", "Harmonic Minor",
                                       "Major Pentatonic", "Minor Pentatonic", "Blues", "Dorian", "Mixolydian" };
    const char* const keyNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    // allowed pitch classes per scale (bit i = i semitones above the key)
    constexpr uint16_t scaleMasks[] = {
        0xfff, 0xfff,
        (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
        (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
        (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),
        (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),
        (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),
        (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),
        (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),
        (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10) };

    juce::String noteName (int midi)
    {
        if (midi < 0)
            return "-";
        return juce::String (keyNames[midi % 12]) + juce::String (midi / 12 - 1);
    }

    juce::String dbOffText (float v, int)  { return v <= -59.9f ? juce::String ("Off") : juce::String (v, 1) + " dB"; }
    juce::String dbText (float v, int)     { return juce::String (v, 1) + " dB"; }
    juce::String pctText (float v, int)    { return juce::String ((int) std::round (v)) + " %"; }
    juce::String centsText (float v, int)  { return juce::String ((int) std::round (v)) + " cents"; }
    juce::String octText (float v, int)    { return (v > 0.0f ? "+" : "") + juce::String (v, 2) + " oct"; }
    juce::String hzText (float v, int)
    {
        if (v >= 1000.0f)
            return juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz";
        return juce::String (v, v < 10.0f ? 2 : (v < 100.0f ? 1 : 0)) + " Hz";
    }
    juce::String msText (float v, int)
    {
        if (v >= 1000.0f)
            return juce::String (v / 1000.0f, 2) + " s";
        return juce::String (v, v < 10.0f ? 1 : 0) + " ms";
    }

    juce::NormalisableRange<float> skewed (float lo, float hi, float centre, float step = 0.0f)
    {
        juce::NormalisableRange<float> r (lo, hi, step);
        r.setSkewForCentre (centre);
        return r;
    }

    float dbToGainOff (float db) { return db <= -59.9f ? 0.0f : juce::Decibels::decibelsToGain (db); }
}

//==============================================================================
void SynthEngine::addParameters (ParamList& p)
{
    using namespace juce;
    using FA = AudioParameterFloatAttributes;
    using IA = AudioParameterIntAttributes;
    auto addF = [&p] (const char* id, const char* name, NormalisableRange<float> r, float def,
                      juce::String (*text) (float, int))
    {
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def,
                                                            FA().withStringFromValueFunction (text)));
    };
    auto addC = [&p] (const char* id, const char* name, StringArray choices, int def)
    {
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, choices, def));
    };

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "synOn", 1 }, "Synth Layer", false));
    addC ("synSource", "Synth Source", { "Input", "MIDI", "Input + MIDI" }, 0);
    addC ("synLayer", "Synth Layer Mode", { "Blend", "Overtones", "Replace" }, 0);
    addF ("synInLevel", "Synth Input Level", NormalisableRange<float> (-60.0f, 6.0f, 0.1f), 0.0f, dbOffText);
    addF ("synLevel", "Synth Level", NormalisableRange<float> (-60.0f, 12.0f, 0.1f), -3.0f, dbOffText);
    addF ("synWidth", "Synth Width", NormalisableRange<float> (0.0f, 200.0f, 1.0f), 100.0f, pctText);

    addF ("synGate", "Tracker Gate", NormalisableRange<float> (-90.0f, -20.0f, 0.5f), -45.0f, dbText);
    addC ("synRange", "Tracker Range", { "Bass 32-450 Hz", "Guitar/Voice 55-1.4k", "High 150-3k Hz", "Full 32-3k Hz" }, 1);
    addF ("synFollow", "Follow Input Envelope", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f, pctText);
    addF ("synOnsetSens", "Onset Sensitivity", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f, pctText);

    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "synOctave", 1 }, "Synth Octave", -3, 3, 0,
        IA().withStringFromValueFunction ([] (int v, int) { return (v > 0 ? "+" : "") + juce::String (v) + " oct"; })));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "synSemi", 1 }, "Synth Semitone", -12, 12, 0,
        IA().withStringFromValueFunction ([] (int v, int) { return (v > 0 ? "+" : "") + juce::String (v) + " st"; })));
    addF ("synFine", "Synth Fine Tune", NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f, centsText);
    addF ("synGlide", "Synth Glide", skewed (0.0f, 2000.0f, 150.0f, 0.1f), 0.0f,
          [] (float v, int) { return v < 0.05f ? juce::String ("Off") : msText (v, 0); });
    addC ("synScale", "Synth Scale", StringArray (scaleNames, 10), 0);
    addC ("synKey", "Synth Key", StringArray (keyNames, 12), 0);

    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "synUnison", 1 }, "Synth Unison", 1, maxUnison, 1,
        IA().withStringFromValueFunction ([] (int v, int) { return v == 1 ? juce::String ("Off") : juce::String (v) + " voices"; })));
    addF ("synDetune", "Unison Detune", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 12.0f, centsText);
    addF ("synSpread", "Unison Spread", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 70.0f, pctText);
    addF ("synSub", "Sub Level", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f, pctText);
    addC ("synSubOct", "Sub Octave", { "-1 oct", "-2 oct" }, 0);
    addC ("synSubShape", "Sub Shape", { "Sine", "Square" }, 0);
    addF ("synNoise", "Noise Level", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f, pctText);
    addF ("synNoiseColour", "Noise Colour", NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f,
          [] (float v, int) { return std::abs (v) < 0.5f ? juce::String ("White") : (v < 0 ? "Dark " : "Bright ") + juce::String ((int) std::abs (v)) + " %"; });
    addF ("synPhaseRand", "Phase Randomness", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f, pctText);
    addF ("synWarmth", "Warmth", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f, pctText);

    addF ("synAmpA", "Amp Attack", skewed (0.5f, 5000.0f, 60.0f, 0.1f), 3.0f, msText);
    addF ("synAmpD", "Amp Decay", skewed (1.0f, 10000.0f, 400.0f, 0.1f), 300.0f, msText);
    addF ("synAmpS", "Amp Sustain", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, pctText);
    addF ("synAmpR", "Amp Release", skewed (2.0f, 10000.0f, 400.0f, 0.1f), 150.0f, msText);

    addC ("synFltType", "Filter Type", { "Ladder 24", "LP 12", "Band", "High", "Notch" }, 0);
    addF ("synCutoff", "Filter Cutoff", skewed (20.0f, 20000.0f, 1000.0f, 0.1f), 12000.0f, hzText);
    addF ("synReso", "Filter Resonance", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 10.0f, pctText);
    addF ("synFltDrive", "Filter Drive", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, pctText);
    addF ("synFltEnv", "Filter Env Amount", NormalisableRange<float> (-8.0f, 8.0f, 0.01f), 0.0f, octText);
    addF ("synKeyTrack", "Filter Key Track", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f, pctText);
    addF ("synFltA", "Filter Attack", skewed (0.5f, 5000.0f, 60.0f, 0.1f), 2.0f, msText);
    addF ("synFltD", "Filter Decay", skewed (1.0f, 10000.0f, 400.0f, 0.1f), 400.0f, msText);
    addF ("synFltS", "Filter Sustain", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, pctText);
    addF ("synFltR", "Filter Release", skewed (2.0f, 10000.0f, 400.0f, 0.1f), 200.0f, msText);

    addF ("synLfoRate", "LFO Rate", skewed (0.05f, 20.0f, 2.0f, 0.01f), 5.0f, hzText);
    addC ("synLfoShape", "LFO Shape", { "Sine", "Tri", "Saw Up", "Saw Dn", "Square", "S&H" }, 0);
    addF ("synLfoPitch", "LFO To Pitch", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, centsText);
    addF ("synLfoCutoff", "LFO To Cutoff", NormalisableRange<float> (0.0f, 4.0f, 0.01f), 0.0f,
          [] (float v, int) { return juce::String (v, 2) + " oct"; });
    addF ("synLfoAmp", "LFO To Amp", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, pctText);

    addF ("synVelSens", "Velocity Sensitivity", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 70.0f, pctText);
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "synBendRange", 1 }, "Pitch Bend Range", 0, 12, 2,
        IA().withStringFromValueFunction ([] (int v, int) { return "+-" + juce::String (v) + " st"; })));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "synVoices", 1 }, "MIDI Voices", 1, numMidiVoices, numMidiVoices,
        IA().withStringFromValueFunction ([] (int v, int) { return v == 1 ? juce::String ("Mono") : juce::String (v); })));

    // chord tracking and per-note drive (appended so existing parameters keep their order)
    addC ("synTrackMode", "Tracking Mode", { "Mono", "Chord" }, 0);
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "synChordNotes", 1 }, "Chord Notes", 1, maxChordNotes, maxChordNotes,
        IA().withStringFromValueFunction ([] (int v, int) { return juce::String (v) + (v == 1 ? " note" : " notes"); })));
    addF ("synChordSens", "Chord Sensitivity", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f, pctText);
    addF ("synPickAttack", "Pick Attack", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f,
          [] (float v, int) { return v < 0.5f ? juce::String ("Off") : pctText (v, 0); });
    addF ("synDrive", "Note Drive", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
          [] (float v, int) { return v < 0.05f ? juce::String ("Off") : pctText (v, 0); });
    addC ("synDriveType", "Note Drive Type", { "Overdrive", "Distortion", "Fuzz" }, 0);
    addF ("synDriveTone", "Note Drive Tone", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f, pctText);
}

//==============================================================================
HelpList SynthEngine::helpEntries()
{
    HelpList h;
    auto add = [&h] (const char* id, const char* title, const char* summary, const char* mapping, const char* tips = "")
    {
        h.push_back ({ id, title, summary, mapping, tips });
    };

    add ("synOn", "Synth layer",
         "Turns the synth layer on: your input (or MIDI) plays the Wave Sculptor's wave as a synth.",
         "Off -> the input passes through this stage untouched (the tracker still runs for the pitch display). "
         "On -> output = input x Input level + synth x Synth level (per Layer mode). Switching fades over 20 ms.",
         "Start with a clean single-note guitar, bass or voice line: the defaults blend a tracked synth under it.");
    add ("synSource", "Source",
         "Who plays the synth: the tracked input, MIDI notes sent to the plugin, or both.",
         "Input -> the input plays the synth: one voice follows its pitch, attacks and loudness (Tracking mode Mono), or up to "
         "6 voices follow the notes of its chords (Tracking mode Chord). "
         "MIDI -> up to 8 voices played by MIDI notes (velocity, pitch bend, sustain pedal CC64). "
         "Input + MIDI -> both at once (the tracked voices plus the MIDI voices).",
         "In FL Studio route a MIDI-out / piano roll to the plugin to play it from MIDI.");
    add ("synLayer", "Layer mode",
         "How the synth is combined with the input signal.",
         "Blend -> input x Input level + synth x Synth level. "
         "Overtones -> the synth plays only partials with ratio >= 1.5 (harmonics 2..64, no fundamental, no sub) "
         "and is phase-locked to the input's fundamental, then added to the input: an exciter that adds sculpted overtones. "
         "Replace -> synth only (the input is muted; Input level is ignored).",
         "Overtones works best with Follow input at 100 % and a bright wave (saw, organ).");
    add ("synInLevel", "Input level",
         "Level of the (processed) input signal in the layer mix.",
         "gain = 10^(dB / 20); -60 dB = Off (input muted). Ignored in Replace mode.");
    add ("synLevel", "Synth level",
         "Level of the synth layer in the mix.",
         "gain = 10^(dB / 20); -60 dB = Off. With Follow input at 100 % the synth at 0 dB has the same RMS level as the input.",
         "Default -3 dB keeps Blend about as loud as the dry input.");
    add ("synWidth", "Width",
         "Stereo width of the synth layer.",
         "Mid/side: side x (Width / 100). 0 % = mono, 100 % = as rendered (unison spread), 200 % = side doubled.");
    add ("synGate", "Tracker gate",
         "Input level below which the tracker treats the input as silence, and above which the synth fades in.",
         "Input RMS (dBFS) < gate -> unvoiced: the tracked voices release after 60 ms (immediately at gate - 3 dB). "
         "Onsets are also ignored below the gate. Over the 15 dB above the gate the tracked voices fade in: "
         "gain x x^2, x = (level dB - gate) / 15, by the input level in Mono and by each note's own level in Chord "
         "(a chord note must reach the gate on its own), so hum, hiss and string noise just above the gate can't play loudly. "
         "Default -45 dB sits above the hum and hiss of a typical guitar rig.",
         "Raise it if string noise or hum still triggers notes; lower it for quiet sources (a soft voice, a low-output pickup).");
    add ("synRange", "Tracker range",
         "Pitch range the tracker searches. Narrower ranges track faster and make fewer octave errors.",
         "Bass -> 32..450 Hz; Guitar/Voice -> 55..1400 Hz; High -> 150..3000 Hz (whistle, flute); Full -> 32..3000 Hz. "
         "The analysis window is 2 periods of the lowest note (Bass ~63 ms, Guitar/Voice ~36 ms, High ~13 ms) "
         "and shrinks to 2.6 periods of the held note once it is stable.",
         "Latency to a new note is roughly half the window.");
    add ("synFollow", "Follow input",
         "How much the tracked voice's loudness follows the input's own dynamics.",
         "voice gain = (1 - F) + F x (input RMS / synth RMS), F = Follow / 100. Mono: the input RMS is measured over exactly one "
         "tracked period (ripple-free). Chord: each voice follows its own note's RMS (its partials in the analysis spectrum), so a "
         "ringing string fades on its own. 100 % -> the synth swells and decays exactly like your playing; 0 % -> constant level "
         "(the amp envelope alone shapes it). Tracked voices only.",
         "Keep at 100 % for guitar and voice; lower it for pad-like, compressed synth lines.");
    add ("synOnsetSens", "Onset sensitivity",
         "How easily a new attack (pluck, picked note) retriggers the envelopes.",
         "An onset fires when the pre-emphasised input energy rises (12 - 9 x S) dB above its 50 ms average, S = sens / 100 "
         "(50 % -> 7.5 dB). Minimum 50 ms between onsets. Re-plucks at the same pitch are detected too.",
         "Retriggers only matter when the amp/filter envelopes have decay (sustain < 100 %).");
    add ("synOctave", "Octave",
         "Transposes the synth in octaves.",
         "pitch = tracked or MIDI note + 12 x octave + semitone + fine / 100 (semitones).");
    add ("synSemi", "Semitone",
         "Transposes the synth in semitones.",
         "pitch offset = semitone value (-12..+12). +7 = a fifth above, +12 = an octave.");
    add ("synFine", "Fine tune",
         "Fine tuning of the synth.",
         "pitch offset = cents / 100 semitones (-100..+100 cents).");
    add ("synGlide", "Glide",
         "Portamento: how long the synth slides to a new pitch.",
         "One-pole slide in semitones; the voice covers ~95 % of the interval in the glide time. Off -> instant "
         "(a 0.5 ms minimum smoothing remains). Tracked voice: smooths the followed pitch; MIDI: slides from the last note.");
    add ("synScale", "Scale",
         "Snaps the tracked pitch to a scale (like pitch correction).",
         "Off -> follows the input exactly (bends, vibrato). Chromatic -> nearest semitone. Others -> nearest note of the "
         "scale built on Key (Major 0 2 4 5 7 9 11, Minor 0 2 3 5 7 8 10, Harmonic Minor 0 2 3 5 7 8 11, Major Pent 0 2 4 7 9, "
         "Minor Pent 0 3 5 7 10, Blues 0 3 5 6 7 10, Dorian 0 2 3 5 7 9 10, Mixolydian 0 2 4 5 7 9 10). "
         "A snapped note is held until the input passes 0.15 st beyond the midpoint to the next note. Tracked voices only "
         "(in Chord mode every note snaps on its own).",
         "Add Glide for a smooth, auto-tune-like slide.");
    add ("synKey", "Key",
         "Root note of the scale used by Scale.",
         "The scale's degrees are counted in semitones above this key. Used only when Scale is a scale (not Off / Chromatic).");
    add ("synUnison", "Unison",
         "Number of detuned copies of the wave per voice.",
         "N copies, each gain 1 / sqrt(N) (constant loudness). Copy i is detuned by Detune x (2i / (N-1) - 1) cents and panned "
         "by Spread x the same factor. Off (1) = a single centred copy.",
         "CPU grows linearly with N.");
    add ("synDetune", "Detune",
         "Detune of the outermost unison copies.",
         "copies spread evenly from -Detune to +Detune cents. Needs Unison >= 2.");
    add ("synSpread", "Spread",
         "Stereo spread of the unison copies.",
         "pan of copy i = Spread / 100 x (2i / (N-1) - 1), equal-power panning. Needs Unison >= 2.");
    add ("synSub", "Sub level",
         "Level of a sub oscillator under the wave.",
         "sub peak = 0.7 x Sub / 100. Muted in Overtones mode.");
    add ("synSubOct", "Sub octave",
         "Pitch of the sub oscillator.",
         "-1 oct -> f0 / 2; -2 oct -> f0 / 4.");
    add ("synSubShape", "Sub shape",
         "Waveform of the sub oscillator.",
         "Sine -> pure; Square -> band-limited (polyBLEP) square, scaled 0.7 for similar loudness.");
    add ("synNoise", "Noise",
         "Adds noise (breath, pick, air) to every voice, shaped by the envelopes and the filter.",
         "noise RMS = 0.2 x Noise / 100 before the filter.");
    add ("synNoiseColour", "Noise colour",
         "Tone of the noise.",
         "0 = white. Dark x % -> 1-pole low-pass at 16 kHz x (150/16000)^(x/100) (down to 150 Hz). "
         "Bright x % -> 1-pole high-pass at 30 Hz x 200^(x/100) (up to 6 kHz). Level is compensated.");
    add ("synPhaseRand", "Phase randomness",
         "Randomises the start phase of every partial at each new note.",
         "phase_k = sculpted phase_k + R x pi x random(-1..1), R = value / 100. 0 % = every note starts identically (punchy); "
         "higher = less phasey unison, softer attacks.");
    add ("synWarmth", "Warmth",
         "Analog-style saturation of the oscillator mix, before the filter.",
         "y = clip(g x) / min(1.5 g, 1), g = 0.05 + 2.45 x W^1.5, clip = cubic soft clipper with antiderivative "
         "anti-aliasing. Small signals keep unity gain; peaks stay at or below 1.");
    add ("synAmpA", "Amp attack",
         "Time for the voice to rise to full level.",
         "Analog-style curve reaching 1.0 after A ms (0.5 ms .. 5 s). A retrigger starts from the current level (no click).");
    add ("synAmpD", "Amp decay",
         "Time to fall from full level to the sustain level.",
         "Exponential; falls 60 dB of the distance to sustain in D ms.");
    add ("synAmpS", "Amp sustain",
         "Level held while the note (or tracked input) continues.",
         "level = S / 100 of full. 100 % = no decay stage.");
    add ("synAmpR", "Amp release",
         "Fade-out after the note ends (MIDI note off, or the tracked input goes silent / unvoiced).",
         "Exponential; falls 60 dB in R ms (minimum 2 ms).");
    add ("synFltType", "Filter type",
         "Character of the voice filter.",
         "Ladder 24 -> 4-pole (24 dB/oct) zero-delay-feedback ladder low-pass, self-oscillates near 100 % resonance. "
         "LP 12 -> 2-pole state-variable low-pass. Band -> band-pass (0 dB at the peak). High -> 2-pole high-pass. "
         "Notch -> band-reject.");
    add ("synCutoff", "Cutoff",
         "Filter cutoff frequency.",
         "fc = Cutoff x 2^(EnvAmt x filter env + KeyTrack x (note - 60) / 12 + LFO x LFO-to-cutoff) Hz, "
         "clamped to 16 Hz .. min(20 kHz, 0.45 x sample rate). Smoothed over ~20 ms.");
    add ("synReso", "Resonance",
         "Emphasis at the cutoff.",
         "Ladder: feedback k = 4.1 x R^0.8 (R = value / 100), self-oscillation from ~97 %. SVF: Q = 0.6 x 40^R (0.6 .. 24).");
    add ("synFltDrive", "Filter drive",
         "Pushes the filter input into soft saturation.",
         "pre-gain d = 1 + 5 x D^2 (D = value / 100), soft clip tanh(0.3 d x) / 0.3, then x 1 / (1 + 0.5 (d - 1)).");
    add ("synFltEnv", "Env amount",
         "How far the filter envelope moves the cutoff.",
         "cutoff shift = amount (octaves, -8..+8) x filter envelope (0..1).");
    add ("synKeyTrack", "Key track",
         "Makes the cutoff follow the played pitch, so high notes are as bright as low ones.",
         "cutoff shift = K / 100 x (note - 60) / 12 octaves (100 % = cutoff moves 1:1 with pitch, relative to C4).");
    add ("synFltA", "Filter attack", "Filter envelope attack.", "Same curve as the amp attack: reaches 1.0 after A ms.");
    add ("synFltD", "Filter decay", "Filter envelope decay.", "Exponential, 60 dB of the distance to sustain in D ms.");
    add ("synFltS", "Filter sustain", "Filter envelope sustain.", "level = S / 100 (applied to Env amount).");
    add ("synFltR", "Filter release", "Filter envelope release.", "Exponential, falls 60 dB in R ms.");
    add ("synLfoRate", "LFO rate",
         "Speed of the low-frequency oscillator (one LFO shared by all voices).",
         "cycles per second, 0.05 .. 20 Hz.");
    add ("synLfoShape", "LFO shape",
         "Waveform of the LFO.",
         "Sine, Tri (triangle), Saw Up (rising ramp), Saw Dn (falling ramp), Square, S&H (a new random value each cycle). "
         "All shapes are smoothed over 3 ms so square and S&H don't click.");
    add ("synLfoPitch", "LFO to pitch",
         "Vibrato depth.",
         "pitch offset = LFO (-1..+1) x value cents.");
    add ("synLfoCutoff", "LFO to cutoff",
         "Filter wobble depth.",
         "cutoff shift = LFO (-1..+1) x value octaves.");
    add ("synLfoAmp", "LFO to amp",
         "Tremolo depth.",
         "gain = 1 - D x (1 - LFO) / 2, D = value / 100: swings between 1 and 1 - D.");
    add ("synVelSens", "Velocity",
         "How much MIDI velocity changes the loudness.",
         "gain = 1 - S + S x (velocity / 127)^2, S = value / 100. MIDI voices only (the tracked voice follows the input level).");
    add ("synBendRange", "Bend range",
         "Pitch-bend wheel range.",
         "full wheel = +-value semitones (default 2). MIDI voices only.");
    add ("synVoices", "MIDI voices",
         "Maximum number of MIDI notes at once.",
         "1 = mono (each new note takes over the voice, legato with Glide), up to 8. When all are busy, the quietest "
         "releasing voice (or else the oldest) is reused without a click.");
    add ("synTrackMode", "Tracking mode",
         "Whether the input is heard as one note at a time or as chords.",
         "Mono -> one voice follows the input's pitch (McLeod pitch method, follows every bend). A new note starts once its "
         "pitch has held steady for 4 analysis frames (3 right after a strong pick attack): ~15 ms at A4, ~33 ms at low E, and "
         "a finger squeak, a pick scrape or a muted strum doesn't play. "
         "Chord -> up to 6 notes (one per guitar string) are heard at once and each plays its own voice. Every 5.3 ms an 85 ms spectrum (Bass / Full "
         "100 ms, High 43 ms) is searched note by note: harmonic sum, strongest first, each found note's partials removed "
         "before the next search. After a strum only the audio since the attack is analysed and the chord is decided 32 ms in "
         "(up to 52 ms when two low notes a fifth apart still blur): those notes start together, re-struck notes retrigger, "
         "the others stop. Later notes start after 2 agreeing frames; a note stops 60 ms after it is no longer heard.",
         "Chord mode is best on clean guitar, piano, organ and pads. Octave doublings (an open E chord has three E's) become one "
         "voice. Notes that share most harmonics (a fifth, a major third) are the hardest; raise Sensitivity if one goes missing. "
         "Scale = Chromatic keeps every chord voice exactly on its note.");
    add ("synChordNotes", "Chord notes",
         "The most notes Chord mode plays at once.",
         "1..6 voices. When more notes are heard, the strongest win (salience = weighted sum of their harmonics). "
         "Lowering it while notes ring releases the weakest at once. Chord mode only.",
         "Set 3 for clean triads from busy strums, 6 for full open chords (one voice per string).");
    add ("synChordSens", "Chord sensitivity",
         "How quiet a note may be, compared with the strongest note, and still be heard as part of the chord.",
         "A new note needs salience >= (0.40 - 0.28 x S) x the strongest note's (S = value / 100: 50 % -> 0.26); a playing note "
         "keeps going down to 60 % of that. Notes 30 dB under the loudest note or 12 dB under the gate are ignored. Chord mode only.",
         "Raise it when a soft inner note is missing; lower it when ghost notes appear (often an octave or a twelfth above a real note).");
    add ("synPickAttack", "Pick attack",
         "Lets the input's own attack through at every new note or strum while the tracker locks on.",
         "On each detected onset the dry input gain rises to P = value / 100, holds 25 ms, then fades with a 30 ms time constant "
         "(gone after ~100 ms). dry gain = max(layer mode's dry gain, P). Works in Mono and Chord mode.",
         "Use 30-60 % in Replace mode: the real pick or hammer stays and the synth takes over, so the tracker's lock time is never heard.");
    add ("synDrive", "Note drive",
         "Distortion applied to every note on its own, before the notes are mixed.",
         "Per voice, after its filter and amp envelope: low cut + mid bump -> gain -> 2x oversampled clipper (antiderivative "
         "anti-aliased) -> DC blocker -> tone low-pass. gain = -6 dB + value / 100 x 36 / 44 / 52 dB (Overdrive / Distortion / "
         "Fuzz). Level makeup keeps a voice at its nominal level equally loud at any setting. 0-5 % fades the stage in. "
         "Because each note is clipped alone, a chord gets no intermodulation: a full major chord stays clear under heavy gain, "
         "which one distorted amp on the whole chord cannot do. The input-following level (Follow, gate fade) is applied "
         "after the drive, so a faint note stays faint instead of being clipped up to full level. Applies to tracked and MIDI voices.",
         "Pair it with the FX rack's Cab (and the Amp switched off or on Clean) for a poly-distorted guitar.");
    add ("synDriveType", "Drive type",
         "The character of the per-note drive.",
         "Overdrive -> asymmetric soft sigmoid s(x + 0.2) - s(0.2), s(x) = x / sqrt(1 + x^2): warm, tube-like even harmonics; "
         "low cut 140 Hz, mid bump +6 dB above 720 Hz. Distortion -> cubic soft clip with a hard knee: tighter, more gain; "
         "low cut 100 Hz, bump +4 dB. Fuzz -> hard clip at -1 / +0.6: square, buzzy, strongly asymmetric; low cut 50 Hz.");
    add ("synDriveTone", "Drive tone",
         "Brightness after the per-note drive.",
         "2-pole low-pass (Q 0.71) after the clipper at fc = 1.2 kHz x 10^(value / 100): 0 % -> 1.2 kHz (dark), "
         "50 % -> 3.8 kHz, 100 % -> 12 kHz (open). Needs Note drive above 0 %.");
    add ("syn.tracker", "Tracker readout",
         "What the tracker hears on the raw plugin input right now.",
         "Mono: note name and cents = nearest equal-tempered note to f0 (A4 = 440 Hz). The bar shows confidence (NSDF clarity "
         "mapped 0.5..0.95 -> 0..100 %). Chord: the chord symbol (root from the lowest note when it fits, slash chords otherwise) "
         "and every note heard, with its level. The meter shows the input RMS in dBFS with the gate as a tick.");
    add ("syn.advanced", "Advanced",
         "Shows or hides rarely used controls.",
         "Click to expand: onset sensitivity, phase randomness, MIDI velocity, bend range and voice count.");
    return h;
}

//==============================================================================
struct SynthEngine::Impl
{
    Impl (juce::AudioProcessorValueTreeState& s, SculptModel& m, ScopeBuffer& scope)
        : state (s), sculptModel (m), layerScope (scope)
    {
        auto get = [&s] (const char* id) { auto* p = s.getRawParameterValue (id); jassert (p != nullptr); return p; };
        pOn = get ("synOn"); pSource = get ("synSource"); pLayer = get ("synLayer");
        pInLevel = get ("synInLevel"); pLevel = get ("synLevel"); pWidth = get ("synWidth");
        pGate = get ("synGate"); pRange = get ("synRange"); pFollow = get ("synFollow"); pOnset = get ("synOnsetSens");
        pOct = get ("synOctave"); pSemi = get ("synSemi"); pFine = get ("synFine"); pGlide = get ("synGlide");
        pScale = get ("synScale"); pKey = get ("synKey");
        pUnison = get ("synUnison"); pDetune = get ("synDetune"); pSpread = get ("synSpread");
        pSub = get ("synSub"); pSubOct = get ("synSubOct"); pSubShape = get ("synSubShape");
        pNoise = get ("synNoise"); pNoiseCol = get ("synNoiseColour"); pPhaseRand = get ("synPhaseRand");
        pWarmth = get ("synWarmth");
        pAmpA = get ("synAmpA"); pAmpD = get ("synAmpD"); pAmpS = get ("synAmpS"); pAmpR = get ("synAmpR");
        pFltType = get ("synFltType"); pCutoff = get ("synCutoff"); pReso = get ("synReso");
        pFltDrive = get ("synFltDrive"); pFltEnv = get ("synFltEnv"); pKeyTrack = get ("synKeyTrack");
        pFltA = get ("synFltA"); pFltD = get ("synFltD"); pFltS = get ("synFltS"); pFltR = get ("synFltR");
        pLfoRate = get ("synLfoRate"); pLfoShape = get ("synLfoShape"); pLfoPitch = get ("synLfoPitch");
        pLfoCut = get ("synLfoCutoff"); pLfoAmp = get ("synLfoAmp");
        pVelSens = get ("synVelSens"); pBendRange = get ("synBendRange"); pVoices = get ("synVoices");
        pTrackMode = get ("synTrackMode"); pChordNotes = get ("synChordNotes"); pChordSens = get ("synChordSens");
        pPick = get ("synPickAttack"); pDrive = get ("synDrive"); pDriveType = get ("synDriveType"); pDriveTone = get ("synDriveTone");
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        debug = juce::SystemStats::getEnvironmentVariable ("CLOD_CHORD_DEBUG", {}).isNotEmpty();
        tracker.prepare (sr);
        chord.prepare (sr);
        for (auto& v : voices)
            v.prepare (sr);
        pickDecay = 1.0f - onePole (0.030f, sr);
        reset();
    }

    void reset()
    {
        tracker.reset();
        chord.reset();
        for (auto& v : voices)
            v.reset();
        trackHeld = false;
        pendingRetrig = false;
        snapped = -1.0f;
        lastTrackerFrame = -1;
        recentCount = 0;
        chordSlot.fill (0);
        chordSnap.fill (-1.0f);
        chordGain.fill (1.0f);
        chordTarget.fill (1.0f);
        lastChordFrame = -1;
        pickEnv = 0.0f;
        pickHold = 0;
        lastMidiPitch = -1.0f;
        sustainPedal = false;
        bend = bendTarget = 0.0f;
        onMix = 0.0f;
        lfoPhase = 0.0;
        lfoSmoothed = 0.0f;
        smoothInit = false;
    }

    static float load (std::atomic<float>* p) noexcept { return p->load (std::memory_order_relaxed); }

    // ---- per-block parameter snapshot ----
    struct Block
    {
        bool on = false, inputSrc = true, midiSrc = false, chordMode = false;
        int layer = 0, scale = 0, key = 0, maxVoices = 8, lfoShape = 0, chordNotes = maxChordNotes;
        float chordSens = 0.5f, pick = 0.0f;
        float inGain = 1, synGain = 0.7f, width = 1, follow = 1, transpose = 0, gateDb = -55;
        float lfoRate = 5, lfoPitch = 0, lfoCut = 0, lfoAmp = 0, bendRange = 2;
        float cutoffOct = 13, reso = 0.1f, warmth = 0, sub = 0, noise = 0, fltDrive = 0;
    };

    void readBlock (Block& b) noexcept
    {
        b.on = load (pOn) > 0.5f;
        const int src = (int) load (pSource);
        b.inputSrc = src != 1;
        b.midiSrc = src != 0;
        b.layer = (int) load (pLayer);
        b.inGain = b.layer == 2 ? 0.0f : dbToGainOff (load (pInLevel));
        b.synGain = dbToGainOff (load (pLevel));
        b.width = load (pWidth) * 0.01f;
        b.follow = load (pFollow) * 0.01f;
        b.transpose = 12.0f * load (pOct) + load (pSemi) + load (pFine) * 0.01f;
        b.scale = (int) load (pScale);
        b.key = (int) load (pKey);
        b.gateDb = load (pGate);
        b.maxVoices = juce::jlimit (1, numMidiVoices, (int) load (pVoices));
        b.lfoRate = load (pLfoRate);
        b.lfoShape = (int) load (pLfoShape);
        b.lfoPitch = load (pLfoPitch) * 0.01f;
        b.lfoCut = load (pLfoCut);
        b.lfoAmp = load (pLfoAmp) * 0.01f;
        b.bendRange = load (pBendRange);
        b.cutoffOct = std::log2 (juce::jmax (16.0f, load (pCutoff)));
        b.reso = load (pReso) * 0.01f;
        b.warmth = load (pWarmth) * 0.01f;
        b.sub = load (pSub) * 0.01f;
        b.noise = load (pNoise) * 0.01f;
        b.fltDrive = load (pFltDrive) * 0.01f;
        b.chordMode = (int) load (pTrackMode) == 1;
        b.chordNotes = juce::jlimit (1, maxChordNotes, (int) load (pChordNotes));
        b.chordSens = load (pChordSens) * 0.01f;
        b.pick = load (pPick) * 0.01f;

        auto& v = vp;
        v.driveAmount = load (pDrive) * 0.01f;
        v.driveType = (int) load (pDriveType);
        v.driveTone = load (pDriveTone) * 0.01f;
        v.unison = juce::jlimit (1, maxUnison, (int) load (pUnison));
        v.detuneCents = load (pDetune);
        v.spread = load (pSpread) * 0.01f;
        v.subOct = (int) load (pSubOct) + 1;
        v.subSquare = (int) load (pSubShape) == 1;
        v.noiseColour = load (pNoiseCol) * 0.01f;
        v.phaseRand = load (pPhaseRand) * 0.01f;
        v.filterType = (int) load (pFltType);
        v.envOct = load (pFltEnv);
        v.keyTrack = load (pKeyTrack) * 0.01f;
        v.glideMs = load (pGlide);
        v.velSens = load (pVelSens) * 0.01f;
        v.overtonesOnly = b.layer == 1;
        v.ampA = load (pAmpA); v.ampD = load (pAmpD); v.ampS = load (pAmpS) * 0.01f; v.ampR = load (pAmpR);
        v.fltA = load (pFltA); v.fltD = load (pFltD); v.fltS = load (pFltS) * 0.01f; v.fltR = load (pFltR);
    }

    // ---- scale quantiser with hysteresis (one snap state per tracked voice) ----
    float quantize (float note, int scale, int key) noexcept { return quantize (note, scale, key, snapped); }

    static float quantize (float note, int scale, int key, float& snapped) noexcept
    {
        if (scale <= 0)
        {
            snapped = -1.0f;
            return note;
        }
        const uint16_t mask = scaleMasks[juce::jlimit (0, 9, scale)];
        auto allowed = [mask, key] (int n) { return (mask >> (((n - key) % 12 + 12) % 12)) & 1; };
        const int base = (int) std::round (note);
        int best = base;
        float bestDist = 100.0f;
        for (int d = -6; d <= 6; ++d)
            if (allowed (base + d) && std::abs ((float) (base + d) - note) < bestDist)
            {
                bestDist = std::abs ((float) (base + d) - note);
                best = base + d;
            }
        if (snapped >= 0.0f && allowed ((int) snapped) && std::abs (note - snapped) < bestDist + 0.3f)
            return snapped;
        snapped = (float) best;
        return snapped;
    }

    // ---- MIDI ----
    void handleMidi (const juce::MidiMessage& m, const Block& b, const sculpt::PartialSet& set) noexcept
    {
        if (m.isNoteOn())
        {
            if (! b.midiSrc || ! b.on)
                return;
            const int note = m.getNoteNumber();
            int idx = -1;
            for (int i = 0; i < b.maxVoices && idx < 0; ++i)
                if (voices[(size_t) i].isActive() && voices[(size_t) i].note == note)
                    idx = i;
            for (int i = 0; i < b.maxVoices && idx < 0; ++i)
                if (! voices[(size_t) i].isActive())
                    idx = i;
            if (idx < 0)
            {
                float quietest = 2.0f;
                for (int i = 0; i < b.maxVoices; ++i)
                {
                    auto& v = voices[(size_t) i];
                    if (! v.held && ! v.sustained && v.level() < quietest)
                    {
                        quietest = v.level();
                        idx = i;
                    }
                }
            }
            if (idx < 0)
            {
                uint32_t oldest = 0xffffffffu;
                for (int i = 0; i < b.maxVoices; ++i)
                    if (voices[(size_t) i].age < oldest)
                    {
                        oldest = voices[(size_t) i].age;
                        idx = i;
                    }
            }
            auto& v = voices[(size_t) juce::jmax (0, idx)];
            VoiceParams p = vp;
            p.overtonesOnly = b.layer == 1;
            v.setExternalGain (1.0f);
            v.noteOn ((float) note + b.transpose, m.getFloatVelocity(), lastMidiPitch, set, p);
            v.note = note;
            v.age = ++ageCounter;
            lastMidiPitch = (float) note + b.transpose;
        }
        else if (m.isNoteOff())
        {
            for (int i = 0; i < numMidiVoices; ++i)
            {
                auto& v = voices[(size_t) i];
                if (v.held && v.note == m.getNoteNumber())
                {
                    if (sustainPedal)
                    {
                        v.held = false;
                        v.sustained = true;
                    }
                    else
                    {
                        v.noteOff();
                    }
                }
            }
        }
        else if (m.isSustainPedalOn())
        {
            sustainPedal = true;
        }
        else if (m.isSustainPedalOff())
        {
            sustainPedal = false;
            for (int i = 0; i < numMidiVoices; ++i)
                if (voices[(size_t) i].sustained)
                {
                    voices[(size_t) i].noteOff();
                    voices[(size_t) i].sustained = false;
                }
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            releaseMidiVoices();
            sustainPedal = false;
        }
        else if (m.isPitchWheel())
        {
            bendTarget = (float) (m.getPitchWheelValue() - 8192) / 8192.0f;
        }
    }

    void releaseMidiVoices() noexcept
    {
        for (int i = 0; i < numMidiVoices; ++i)
        {
            auto& v = voices[(size_t) i];
            if (v.held || v.sustained)
                v.noteOff();
            v.sustained = false;
        }
    }

    // ---- tracked voice ----
    void updateTracked (int len, const Block& b, const sculpt::PartialSet& set, const VoiceParams& p) noexcept
    {
        auto& v = voices[trackedIndex];
        const bool newOnset = tracker.onsets != lastOnsets;
        lastOnsets = tracker.onsets;

        if (! b.inputSrc || ! b.on)
        {
            if (trackHeld)
                v.noteOff();
            trackHeld = false;
            pendingRetrig = false;
            return;
        }

        if (newOnset && trackHeld)
        {
            pendingRetrig = true;
            pendingSamples = 0;
        }

        // a new phrase only starts on a steady pitch: 4 fresh analysis frames that drift no more than
        // 0.2 semitone first to last and spread no more than 0.25 (a finger sliding on a wound string
        // sweeps ~0.5 semitone in that time, a plucked note settles by ~0.1). Right after a strong attack
        // (an onset 18 dB or more above the gate: a pick, not a squeak or a scrape) 3 frames will do.
        if (tracker.frames != lastTrackerFrame)
        {
            lastTrackerFrame = tracker.frames;
            if (tracker.voiced && tracker.candidate > 0.0f)   // a fresh estimate this frame, not a held one
            {
                recentPitch[(size_t) (recentCount % 4)] = hzToMidi (tracker.candidate);
                ++recentCount;
            }
            else
            {
                recentCount = 0;
            }
            if (debug && tracker.voiced && ! trackHeld)
                std::fprintf (stderr, "[mono %d] estimate %.1f Hz level %.1f dB n %d\n", tracker.frames,
                              tracker.candidate, tracker.levelDb, recentCount);
        }
        const bool strongAttack = tracker.framesSinceOnset < 12 && tracker.levelDb >= b.gateDb + 18.0f;
        const int needed = strongAttack ? 3 : 4;
        bool steady = false;
        if (recentCount >= needed)
        {
            float hi = -1000.0f, lo = 1000.0f;
            for (int k = 1; k <= needed; ++k)
            {
                const float pk = recentPitch[(size_t) ((recentCount - k) % 4)];
                hi = juce::jmax (hi, pk);
                lo = juce::jmin (lo, pk);
            }
            const float newest = recentPitch[(size_t) ((recentCount - 1) % 4)];
            const float oldest = recentPitch[(size_t) ((recentCount - needed) % 4)];
            steady = std::abs (newest - oldest) <= 0.2f && hi - lo <= 0.25f;
        }

        const bool voicedNow = tracker.voiced && tracker.f0 > 0.0f && tracker.levelDb > b.gateDb
                               && (steady || (trackHeld && v.isActive()));
        if (voicedNow)
        {
            unvoicedSamples = 0;
            const float note = quantize (hzToMidi (tracker.f0) + b.transpose, b.scale, b.key);
            if (! trackHeld || ! v.isActive())
            {
                v.noteOn (note, 1.0f, -1.0f, set, p);   // a new phrase starts on pitch; glide acts within it
                v.note = -2;
                trackHeld = true;
                pendingRetrig = false;
            }
            else
            {
                v.setTargetPitch (note);
            }
        }
        else if (trackHeld)
        {
            unvoicedSamples += len;
            if (unvoicedSamples > (int) (0.06 * sr) || tracker.levelDb < b.gateDb - 3.0f)
            {
                v.noteOff();
                trackHeld = false;
                pendingRetrig = false;
            }
        }

        if (pendingRetrig)
        {
            pendingSamples += len;
            // wait for the first pitch frame after the attack (so the new note's pitch is in), max 30 ms
            if (tracker.framesSinceOnset >= 2 || pendingSamples > (int) (0.03 * sr))
            {
                if (trackHeld)
                    v.retrigger();
                pendingRetrig = false;
            }
        }

        // follow the input envelope: synth RMS = input RMS at 100 %; faded in over 15 dB above the gate
        if (trackHeld)
        {
            const float ratio = tracker.envelope / juce::jmax (0.02f, oscRms * trackedBaseGain);
            v.setExternalGain (((1.0f - b.follow) + b.follow * juce::jmin (ratio, 12.0f)) * gateKnee (tracker.levelDb, b.gateDb));
        }
    }

    // ---- chord voices (Tracking mode Chord): voice t of the tracked group plays slot chordSlot[t] ----
    SynthVoice& chordVoice (int t) noexcept { return voices[trackedIndex + (size_t) t]; }

    const ChordTracker::Note* findSlot (uint32_t id) const noexcept
    {
        for (auto& s : chord.slots)
            if (s.id == id && s.active)
                return &s;
        return nullptr;
    }

    void releaseChordVoices() noexcept
    {
        for (int t = 0; t < maxChordNotes; ++t)
            if (chordSlot[(size_t) t] != 0)
            {
                chordVoice (t).noteOff();
                chordSlot[(size_t) t] = 0;
            }
    }

    float chordFollowGain (float level, float follow) const noexcept
    {
        const float ratio = level / juce::jmax (0.02f, oscRms * chordBaseGain);
        return (1.0f - follow) + follow * juce::jmin (ratio, 12.0f);
    }

    // the tracked voices fade in over the 15 dB above the gate (Mono: by the input level; Chord: by
    // each note's own level), so hum, hiss and string noise that get past the gate can't play loudly
    static float gateKnee (float levelDb, float gateDb) noexcept
    {
        const float x = juce::jlimit (0.0f, 1.0f, (levelDb - gateDb) * (1.0f / 15.0f));
        return x * x;
    }

    void updateChord (int len, const Block& b, const sculpt::PartialSet& set, const VoiceParams& p) noexcept
    {
        if (! b.inputSrc || ! b.on)
        {
            releaseChordVoices();
            return;
        }

        if (chord.frames != lastChordFrame)
        {
            lastChordFrame = chord.frames;

            // notes that ended release their voices
            for (int t = 0; t < maxChordNotes; ++t)
                if (chordSlot[(size_t) t] != 0 && findSlot (chordSlot[(size_t) t]) == nullptr)
                {
                    chordVoice (t).noteOff();
                    chordSlot[(size_t) t] = 0;
                }

            // new notes take a free voice: an idle one, else the quietest one still releasing
            for (auto& s : chord.slots)
            {
                if (s.id == 0 || ! s.active)
                    continue;
                bool hasVoice = false;
                for (auto id : chordSlot)
                    hasVoice = hasVoice || id == s.id;
                if (hasVoice)
                    continue;
                int best = -1;
                float quietest = 1.0e9f;
                for (int t = 0; t < maxChordNotes; ++t)
                {
                    if (chordSlot[(size_t) t] != 0)
                        continue;
                    const float lv = chordVoice (t).isActive() ? chordVoice (t).level() : -1.0f;
                    if (lv < quietest)
                    {
                        quietest = lv;
                        best = t;
                    }
                }
                if (best < 0)
                    break;
                auto& v = chordVoice (best);
                chordSnap[(size_t) best] = -1.0f;
                const float note = quantize (s.pitch + b.transpose, b.scale, b.key, chordSnap[(size_t) best]);
                chordGain[(size_t) best] = chordTarget[(size_t) best]
                    = chordFollowGain (s.level, b.follow) * gateKnee (juce::Decibels::gainToDecibels (s.level, -120.0f), b.gateDb);
                v.setExternalGain (chordGain[(size_t) best]);
                v.noteOn (note, 1.0f, -1.0f, set, p);   // a new note starts on pitch
                v.note = -2;
                chordSlot[(size_t) best] = s.id;
                s.retrigger = false;
            }

            // playing notes follow their pitch and loudness; re-struck notes retrigger
            for (int t = 0; t < maxChordNotes; ++t)
            {
                if (chordSlot[(size_t) t] == 0)
                    continue;
                for (auto& s : chord.slots)
                    if (s.id == chordSlot[(size_t) t])
                    {
                        auto& v = chordVoice (t);
                        v.setTargetPitch (quantize (s.pitch + b.transpose, b.scale, b.key, chordSnap[(size_t) t]));
                        if (s.retrigger)
                        {
                            v.retrigger();
                            s.retrigger = false;
                        }
                        chordTarget[(size_t) t] = chordFollowGain (s.level, b.follow)
                                                  * gateKnee (juce::Decibels::gainToDecibels (s.level, -120.0f), b.gateDb);
                    }
            }
        }

        // loudness: the frames come every 5.3 ms, so smooth (10 ms) between them
        const float c = onePole (0.010f, sr, len);
        for (int t = 0; t < maxChordNotes; ++t)
            if (chordSlot[(size_t) t] != 0)
            {
                chordGain[(size_t) t] += (chordTarget[(size_t) t] - chordGain[(size_t) t]) * c;
                chordVoice (t).setExternalGain (chordGain[(size_t) t]);
            }
    }

    float lfoValue (int shape, double ph) noexcept
    {
        const float f = (float) ph;
        switch (shape)
        {
            case 1:  return 4.0f * std::abs (f - 0.5f) - 1.0f;
            case 2:  return 2.0f * f - 1.0f;
            case 3:  return 1.0f - 2.0f * f;
            case 4:  return f < 0.5f ? 1.0f : -1.0f;
            case 5:  return shValue;
            default: return std::sin (twoPi * f);
        }
    }

    void process (juce::AudioBuffer<float>& io, int numChannels, const juce::AudioBuffer<float>& analysis,
                  const juce::MidiBuffer& midi) noexcept
    {
        const int n = io.getNumSamples();
        if (n <= 0)
            return;
        const int numCh = juce::jlimit (1, 2, juce::jmin (numChannels, io.getNumChannels()));
        const int nA = juce::jmin (n, analysis.getNumSamples());
        const float* a0 = analysis.getNumChannels() > 0 ? analysis.getReadPointer (0) : nullptr;
        const float* a1 = analysis.getNumChannels() > 1 ? analysis.getReadPointer (1) : a0;

        Block b;
        readBlock (b);
        tracker.setRange ((int) load (pRange));
        tracker.setGateDb (b.gateDb);
        tracker.setOnsetSensitivity (load (pOnset) * 0.01f);
        chord.setRange ((int) load (pRange));
        chord.setGateDb (b.gateDb);
        chord.setMaxNotes (b.chordNotes);
        chord.setSensitivity (b.chordSens);
        chord.setEnabled (b.chordMode && b.inputSrc);   // also while the layer is off: the displays show the chord

        // switching Mono <-> Chord: the tracked voices of the other mode release
        if (b.chordMode != chordModeNow)
        {
            if (trackHeld)
                voices[trackedIndex].noteOff();
            trackHeld = false;
            pendingRetrig = false;
            releaseChordVoices();
            chordModeNow = b.chordMode;
        }

        const auto& set = sculptModel.audioPartials();
        if (set.version != lastSetVersion || ! oscRmsValid)
        {
            float e = 0.0f;
            for (int k = 0; k < juce::jmin (set.count, numPartials); ++k)
                e += set.amp[(size_t) k] * set.amp[(size_t) k];
            oscRms = std::sqrt (0.5f * e);
            lastSetVersion = set.version;
            oscRmsValid = true;
        }
        vp.oscRms = oscRms;

        bool anyActive = false;
        for (auto& v : voices)
            anyActive = anyActive || v.isActive();

        alignas (16) float mono[chunkSize];

        // ---- bypass path: tracker only, layer scope decays ----
        if (! b.on && onMix <= 0.0f)
        {
            if (anyActive || trackHeld)
            {
                for (auto& v : voices)
                    v.reset();
                trackHeld = false;
            }
            chordSlot.fill (0);
            pickEnv = 0.0f;
            static const float zeros[chunkSize] {};
            for (int pos = 0; pos < n; pos += chunkSize)
            {
                const int len = juce::jmin (chunkSize, n - pos);
                for (int i = 0; i < len; ++i)
                    mono[i] = pos + i < nA && a0 != nullptr ? 0.5f * (a0[pos + i] + a1[pos + i]) : 0.0f;
                tracker.process (mono, len);
                if (tracker.onsets != lastOnsets)
                    chord.onset();
                lastOnsets = lastPickOnsets = tracker.onsets;
                chord.process (mono, len);
                layerScope.push (zeros, zeros, len);
            }
            publish (0);
            return;
        }

        if (! b.midiSrc)
            releaseMidiVoices();

        if (! smoothInit)
        {
            sm = b;
            smoothInit = true;
        }

        auto midiIt = midi.cbegin();
        const auto midiEnd = midi.cend();
        const float onTarget = b.on ? 1.0f : 0.0f;
        const float onStep = 1.0f / (0.02f * (float) sr);
        auto* outL = io.getWritePointer (0);
        auto* outR = numCh > 1 ? io.getWritePointer (1) : nullptr;

        for (int pos = 0; pos < n;)
        {
            const int len = juce::jmin (chunkSize, n - pos);

            for (; midiIt != midiEnd; ++midiIt)
            {
                const auto meta = *midiIt;
                if (meta.samplePosition >= pos + len)
                    break;
                handleMidi (meta.getMessage(), b, set);
            }

            for (int i = 0; i < len; ++i)
                mono[i] = pos + i < nA && a0 != nullptr ? 0.5f * (a0[pos + i] + a1[pos + i]) : 0.0f;
            tracker.process (mono, len);

            // attacks: re-check the chord, let the pick through (Pick attack)
            if (tracker.onsets != lastPickOnsets)
            {
                lastPickOnsets = tracker.onsets;
                if (b.chordMode)
                    chord.onset();
                if (b.inputSrc && b.pick > 0.0f)
                {
                    pickEnv = juce::jmax (pickEnv, b.pick);
                    pickHold = (int) (0.025 * sr);
                }
            }
            chord.process (mono, len);

            // ---- smoothing (per chunk) ----
            const float c = onePole (0.02f, sr, len);
            const Block prev = sm;
            sm.inGain += (b.inGain - sm.inGain) * c;
            sm.synGain += (b.synGain - sm.synGain) * c;
            sm.width += (b.width - sm.width) * c;
            sm.cutoffOct += (b.cutoffOct - sm.cutoffOct) * c;
            sm.reso += (b.reso - sm.reso) * c;
            sm.warmth += (b.warmth - sm.warmth) * c;
            sm.sub += (b.sub - sm.sub) * c;
            sm.noise += (b.noise - sm.noise) * c;
            sm.fltDrive += (b.fltDrive - sm.fltDrive) * c;
            bend += (bendTarget - bend) * onePole (0.005f, sr, len);

            // ---- LFO ----
            lfoPhase += (double) b.lfoRate * len / sr;
            if (lfoPhase >= 1.0)
            {
                lfoPhase -= std::floor (lfoPhase);
                shValue = lfoRng.next();
            }
            const float lfoPrev = lfoSmoothed;
            lfoSmoothed += (lfoValue (b.lfoShape, lfoPhase) - lfoSmoothed) * onePole (0.003f, sr, len);
            auto trem = [&b] (float l) { return 1.0f - b.lfoAmp * 0.5f * (1.0f - l); };

            vp.cutoffOct = sm.cutoffOct;
            vp.reso = sm.reso;
            vp.warmth = sm.warmth;
            vp.subLevel = sm.sub;
            vp.noiseLevel = sm.noise;
            vp.filterDrive = sm.fltDrive;
            vp.lfoPitchSemis = lfoSmoothed * b.lfoPitch;
            vp.lfoCutOct = lfoSmoothed * b.lfoCut;
            vp.lfoAmpStart = trem (lfoPrev);
            vp.lfoAmpEnd = trem (lfoSmoothed);
            vp.overtonesOnly = b.layer == 1;

            VoiceParams vt = vp;     // tracked voice(s)
            vt.baseGain = b.chordMode ? chordBaseGain : trackedBaseGain;
            vt.bendSemis = 0.0f;
            vt.velSens = 0.0f;
            // hides the tracker's pitch steps (Mono 2.7 ms, Chord 5.3 ms frames)
            vt.glideMs = juce::jmax (vp.glideMs, b.chordMode ? 8.0f : 4.0f);
            vp.baseGain = midiBaseGain;
            vp.bendSemis = bend * b.bendRange;

            if (b.chordMode)
            {
                lastOnsets = tracker.onsets;
                updateChord (len, b, set, vt);
            }
            else
            {
                updateTracked (len, b, set, vt);
            }
            for (int t = 0; t < maxChordNotes; ++t)
                chordVoice (t).enablePll (b.layer == 1);   // Overtones: each voice locks to its own note

            // ---- render ----
            alignas (16) float sL[chunkSize] {}, sR[chunkSize] {};
            for (int i = 0; i < numMidiVoices; ++i)
                voices[(size_t) i].render (sL, sR, len, vp, set, nullptr);
            for (int t = 0; t < maxChordNotes; ++t)
                chordVoice (t).render (sL, sR, len, vt, set, mono);

            // ---- layer mix ----
            const float invLen = 1.0f / (float) len;
            for (int i = 0; i < len; ++i)
            {
                const float t = (float) (i + 1) * invLen;
                onMix = onTarget > onMix ? juce::jmin (onTarget, onMix + onStep) : juce::jmax (onTarget, onMix - onStep);
                const float inG = prev.inGain + (sm.inGain - prev.inGain) * t;
                const float synG = (prev.synGain + (sm.synGain - prev.synGain) * t) * onMix;
                const float w = prev.width + (sm.width - prev.width) * t;
                const float mid = 0.5f * (sL[i] + sR[i]);
                const float side = 0.5f * (sL[i] - sR[i]) * w;
                sL[i] = (mid + side) * synG;
                sR[i] = (mid - side) * synG;
                float dryG = 1.0f - onMix + onMix * inG;
                if (pickEnv > 0.0f)
                {
                    dryG = juce::jmax (dryG, onMix * pickEnv);
                    if (pickHold > 0)
                        --pickHold;
                    else if ((pickEnv *= pickDecay) < 1.0e-4f)
                        pickEnv = 0.0f;
                }
                if (outR != nullptr)
                {
                    outL[pos + i] = outL[pos + i] * dryG + sL[i];
                    outR[pos + i] = outR[pos + i] * dryG + sR[i];
                }
                else
                {
                    outL[pos + i] = outL[pos + i] * dryG + 0.5f * (sL[i] + sR[i]);
                }
            }
            layerScope.push (sL, sR, len);
            pos += len;
        }

        // switched off and faded out: stop everything
        if (! b.on && onMix <= 0.0f)
        {
            for (auto& v : voices)
                v.reset();
            trackHeld = false;
            chordSlot.fill (0);
        }

        int active = 0;
        for (auto& v : voices)
            active += v.isActive() ? 1 : 0;
        publish (active);
    }

    void publish (int active) noexcept
    {
        aF0.store (tracker.voiced ? tracker.f0 : 0.0f, std::memory_order_relaxed);
        aConf.store (tracker.voiced ? tracker.confidence : tracker.confidence * 0.5f, std::memory_order_relaxed);
        aLevel.store (tracker.levelDb, std::memory_order_relaxed);
        aVoiced.store (tracker.voiced, std::memory_order_relaxed);
        const auto& tv = voices[trackedIndex];
        aSynthF0.store (! chordModeNow && tv.isActive() && tv.note == -2 ? tv.currentHz() : 0.0f, std::memory_order_relaxed);
        aVoices.store (active, std::memory_order_relaxed);
        aCutoff.store (std::exp2 (sm.cutoffOct), std::memory_order_relaxed);

        // the chord as heard right now, ascending: count in bits 0..2, then 7 bits per note
        float pitch[maxChordNotes], level[maxChordNotes];
        int count = 0;
        if (chordModeNow)
            for (auto& s : chord.slots)
                if (s.id != 0 && s.active && count < maxChordNotes)
                {
                    int i = count++;
                    while (i > 0 && pitch[i - 1] > s.pitch)
                    {
                        pitch[i] = pitch[i - 1];
                        level[i] = level[i - 1];
                        --i;
                    }
                    pitch[i] = s.pitch;
                    level[i] = s.level;
                }
        uint64_t packed = (uint64_t) count;
        for (int i = 0; i < count; ++i)
        {
            packed |= (uint64_t) juce::jlimit (0, 127, (int) std::round (pitch[i])) << (3 + 7 * i);
            aNoteLevel[(size_t) i].store (level[i], std::memory_order_relaxed);
        }
        aChord.store (packed, std::memory_order_relaxed);
        aChordMode.store (chordModeNow, std::memory_order_relaxed);
    }

    juce::AudioProcessorValueTreeState& state;
    SculptModel& sculptModel;
    ScopeBuffer& layerScope;

    std::atomic<float>* pOn {}; std::atomic<float>* pSource {}; std::atomic<float>* pLayer {};
    std::atomic<float>* pInLevel {}; std::atomic<float>* pLevel {}; std::atomic<float>* pWidth {};
    std::atomic<float>* pGate {}; std::atomic<float>* pRange {}; std::atomic<float>* pFollow {}; std::atomic<float>* pOnset {};
    std::atomic<float>* pOct {}; std::atomic<float>* pSemi {}; std::atomic<float>* pFine {}; std::atomic<float>* pGlide {};
    std::atomic<float>* pScale {}; std::atomic<float>* pKey {};
    std::atomic<float>* pUnison {}; std::atomic<float>* pDetune {}; std::atomic<float>* pSpread {};
    std::atomic<float>* pSub {}; std::atomic<float>* pSubOct {}; std::atomic<float>* pSubShape {};
    std::atomic<float>* pNoise {}; std::atomic<float>* pNoiseCol {}; std::atomic<float>* pPhaseRand {};
    std::atomic<float>* pWarmth {};
    std::atomic<float>* pAmpA {}; std::atomic<float>* pAmpD {}; std::atomic<float>* pAmpS {}; std::atomic<float>* pAmpR {};
    std::atomic<float>* pFltType {}; std::atomic<float>* pCutoff {}; std::atomic<float>* pReso {};
    std::atomic<float>* pFltDrive {}; std::atomic<float>* pFltEnv {}; std::atomic<float>* pKeyTrack {};
    std::atomic<float>* pFltA {}; std::atomic<float>* pFltD {}; std::atomic<float>* pFltS {}; std::atomic<float>* pFltR {};
    std::atomic<float>* pLfoRate {}; std::atomic<float>* pLfoShape {}; std::atomic<float>* pLfoPitch {};
    std::atomic<float>* pLfoCut {}; std::atomic<float>* pLfoAmp {};
    std::atomic<float>* pVelSens {}; std::atomic<float>* pBendRange {}; std::atomic<float>* pVoices {};
    std::atomic<float>* pTrackMode {}; std::atomic<float>* pChordNotes {}; std::atomic<float>* pChordSens {};
    std::atomic<float>* pPick {}; std::atomic<float>* pDrive {}; std::atomic<float>* pDriveType {}; std::atomic<float>* pDriveTone {};

    // voices 0..7 play MIDI; 8..12 are the tracked group (Mono uses voice 8 only)
    static constexpr size_t trackedIndex = numMidiVoices;
    static constexpr float midiBaseGain = 0.35f;
    static constexpr float trackedBaseGain = 0.5f;
    static constexpr float chordBaseGain = 0.32f;   // 3 notes ~ the loudness of one Mono voice

    double sr = 48000.0;
    PitchTracker tracker;
    ChordTracker chord;
    std::array<SynthVoice, numMidiVoices + maxChordNotes> voices;
    VoiceParams vp;
    Block sm;
    bool smoothInit = false;

    float onMix = 0.0f;
    float oscRms = 0.5f;
    uint32_t lastSetVersion = 0;
    bool oscRmsValid = false;

    // tracked voice state
    bool trackHeld = false, pendingRetrig = false;
    int lastOnsets = 0, unvoicedSamples = 0, pendingSamples = 0;
    float snapped = -1.0f;
    int lastTrackerFrame = -1, recentCount = 0;
    std::array<float, 4> recentPitch {};
    bool debug = false;   // CLOD_CHORD_DEBUG: mono note-on candidates to stderr (test host)

    // chord voices: slot id each plays (0 = free), per-voice scale snap and follow gain
    bool chordModeNow = false;
    std::array<uint32_t, maxChordNotes> chordSlot {};
    std::array<float, maxChordNotes> chordSnap {}, chordGain {}, chordTarget {};
    int lastChordFrame = -1;

    // pick attack: dry input let through at each onset
    int lastPickOnsets = 0, pickHold = 0;
    float pickEnv = 0.0f, pickDecay = 0.999f;

    // MIDI state
    bool sustainPedal = false;
    float bend = 0.0f, bendTarget = 0.0f, lastMidiPitch = -1.0f;
    uint32_t ageCounter = 0;

    // LFO
    double lfoPhase = 0.0;
    float lfoSmoothed = 0.0f, shValue = 0.0f;
    Rng lfoRng;

    // published for the editor
    std::atomic<float> aF0 { 0.0f }, aConf { 0.0f }, aLevel { -120.0f }, aSynthF0 { 0.0f }, aCutoff { 12000.0f };
    std::atomic<bool> aVoiced { false }, aChordMode { false };
    std::atomic<int> aVoices { 0 };
    std::atomic<uint64_t> aChord { 0 };
    std::array<std::atomic<float>, maxChordNotes> aNoteLevel {};
};

//==============================================================================
SynthEngine::SynthEngine (juce::AudioProcessorValueTreeState& s, SculptModel& m, ScopeBuffer& layerScope)
    : impl (std::make_unique<Impl> (s, m, layerScope))
{
}

SynthEngine::~SynthEngine() = default;

void SynthEngine::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    juce::ignoreUnused (maxBlockSize, numChannels);
    impl->prepare (sampleRate);
}

void SynthEngine::reset()
{
    impl->reset();
}

void SynthEngine::process (juce::AudioBuffer<float>& io, int numChannels,
                           const juce::AudioBuffer<float>& analysis, const juce::MidiBuffer& midi) noexcept
{
    impl->process (io, numChannels, analysis, midi);
}

TrackerSnapshot SynthEngine::tracker() const noexcept
{
    TrackerSnapshot t;
    t.f0Hz = impl->aF0.load (std::memory_order_relaxed);
    t.confidence = impl->aConf.load (std::memory_order_relaxed);
    t.levelDb = impl->aLevel.load (std::memory_order_relaxed);
    t.voiced = impl->aVoiced.load (std::memory_order_relaxed) && t.f0Hz > 0.0f;
    t.synthF0Hz = impl->aSynthF0.load (std::memory_order_relaxed);
    if (t.f0Hz > 0.0f)
    {
        const float m = hzToMidi (t.f0Hz);
        t.midiNote = juce::jlimit (0, 127, (int) std::round (m));
        t.cents = (m - (float) t.midiNote) * 100.0f;
    }
    t.chordMode = impl->aChordMode.load (std::memory_order_relaxed);
    const uint64_t packed = impl->aChord.load (std::memory_order_relaxed);
    t.numNotes = juce::jlimit (0, maxChordNotes, (int) (packed & 7u));
    for (int i = 0; i < t.numNotes; ++i)
    {
        t.notes[(size_t) i] = (int) ((packed >> (3 + 7 * i)) & 127u);
        t.noteLevels[(size_t) i] = impl->aNoteLevel[(size_t) i].load (std::memory_order_relaxed);
    }
    return t;
}

int SynthEngine::activeVoices() const noexcept
{
    return impl->aVoices.load (std::memory_order_relaxed);
}

juce::String SynthEngine::describeMapping() const
{
    auto& s = impl->state;
    auto val = [&s] (const char* id) { auto* p = s.getRawParameterValue (id); return p != nullptr ? p->load() : 0.0f; };
    auto choiceText = [&s] (const char* id)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (s.getParameter (id)))
            return p->getCurrentChoiceName();
        return juce::String();
    };

    const auto t = tracker();
    const bool on = val ("synOn") > 0.5f;
    const int src = (int) val ("synSource");
    const int layer = (int) val ("synLayer");
    juce::String out;

    out << "Synth layer " << (on ? "ON" : "OFF (tracker still listening)") << ". Source: " << choiceText ("synSource") << ".\n";

    const bool chordMode = (int) val ("synTrackMode") == 1;
    if (chordMode && src != 1)
    {
        if (t.numNotes > 0)
            out << "Chord tracker hears " << chordname::symbol (t.notes.data(), t.numNotes) << " ("
                << chordname::noteList (t.notes.data(), t.numNotes) << ") - " << t.numNotes
                << (t.numNotes == 1 ? " note" : " notes") << " of max " << (int) val ("synChordNotes")
                << ", input " << juce::String (t.levelDb, 1) << " dBFS.\n";
        else
            out << "Chord tracker: no notes (input " << juce::String (t.levelDb, 1) << " dBFS, gate "
                << juce::String (val ("synGate"), 1) << " dB).\n";
    }
    else if (t.voiced)
        out << "Tracker: " << noteName (t.midiNote) << " " << (t.cents >= 0 ? "+" : "") << juce::String ((int) std::round (t.cents))
            << " cents (" << juce::String (t.f0Hz, 1) << " Hz), confidence " << juce::String ((int) std::round (t.confidence * 100.0f))
            << " %, input " << juce::String (t.levelDb, 1) << " dBFS.\n";
    else
        out << "Tracker: no stable pitch (input " << juce::String (t.levelDb, 1) << " dBFS, gate "
            << juce::String (val ("synGate"), 1) << " dB).\n";

    const float transpose = 12.0f * val ("synOctave") + val ("synSemi") + val ("synFine") * 0.01f;
    if (src != 1 && chordMode)
    {
        out << "Chord voices: each heard note plays its own voice at its pitch " << (transpose >= 0 ? "+" : "")
            << juce::String (transpose, 2) << " st";
        if ((int) val ("synScale") > 0)
            out << ", snapped to " << choiceText ("synKey") << " " << choiceText ("synScale");
        out << "; each voice's level follows its own note " << juce::String ((int) val ("synFollow")) << " %";
        if (val ("synPickAttack") > 0.5f)
            out << "; pick attack lets " << juce::String ((int) val ("synPickAttack")) << " % of the input through at each strum";
        out << ".\n";
    }
    else if (src != 1)
    {
        out << "Tracked voice: ";
        if (t.synthF0Hz > 0.0f)
            out << "plays " << juce::String (t.synthF0Hz, 1) << " Hz (" << noteName ((int) std::round (hzToMidi (t.synthF0Hz))) << ")";
        else
            out << "silent";
        out << " = input pitch " << (transpose >= 0 ? "+" : "") << juce::String (transpose, 2) << " st";
        if ((int) val ("synScale") > 0)
            out << ", snapped to " << choiceText ("synKey") << " " << choiceText ("synScale");
        out << "; level follows the input " << juce::String ((int) val ("synFollow")) << " %.\n";
    }

    sculpt::PartialSet ps;
    impl->sculptModel.copyDisplayPartials (ps);
    int audible = 0;
    for (int k = 0; k < ps.count; ++k)
        audible += ps.amp[(size_t) k] > 1.0e-4f ? 1 : 0;
    out << "Wave: " << audible << " sculpted partials (" << (ps.harmonic ? "harmonic" : "inharmonic") << ")";
    const int uni = (int) val ("synUnison");
    if (uni > 1)
        out << " x " << uni << " unison copies (+-" << juce::String (val ("synDetune"), 1) << " cents)";
    out << ".\n";

    const float inDb = val ("synInLevel"), synDb = val ("synLevel");
    auto db = [] (float v) { return v <= -59.9f ? juce::String ("Off") : juce::String (v, 1) + " dB"; };
    switch (layer)
    {
        case 1:  out << "Layer: Overtones - input at " << db (inDb) << " + harmonics 2..64 only at " << db (synDb)
                     << ", phase-locked to the input fundamental.\n"; break;
        case 2:  out << "Layer: Replace - synth only at " << db (synDb) << " (input muted).\n"; break;
        default: out << "Layer: Blend - input at " << db (inDb) << " + synth at " << db (synDb) << ".\n"; break;
    }

    out << "Filter: " << choiceText ("synFltType") << " at " << hzText (impl->aCutoff.load(), 0)
        << ", resonance " << juce::String ((int) val ("synReso")) << " %";
    const float envAmt = val ("synFltEnv");
    if (std::abs (envAmt) > 0.005f)
        out << ", envelope " << (envAmt > 0 ? "+" : "") << juce::String (envAmt, 2) << " oct";
    out << ".\n";
    if (const float drv = val ("synDrive"); drv > 0.05f)
    {
        static const float span[] = { 36.0f, 44.0f, 52.0f };
        const int type = juce::jlimit (0, 2, (int) val ("synDriveType"));
        out << "Note drive: " << choiceText ("synDriveType") << " " << juce::String ((int) std::round (drv)) << " % (gain "
            << juce::String (-6.0f + drv * 0.01f * span[type], 1) << " dB) on every voice separately, tone low-pass "
            << hzText ((float) (1200.0 * std::pow (10.0, val ("synDriveTone") * 0.01)), 0) << ".\n";
    }
    out << "Voices active: " << activeVoices();
    if (src == 0)
        out << (chordMode ? " (tracked, max " + juce::String ((int) val ("synChordNotes")) + ")" : juce::String());
    else
        out << " (MIDI max " << (int) val ("synVoices") << (src == 2 ? " + tracked)" : ")");
    return out;
}
