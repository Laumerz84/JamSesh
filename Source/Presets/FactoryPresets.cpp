#include "FactoryPresets.h"
#include "PresetBuilder.h"

// The factory library. Each function below is one category; keep "Init" first (it is the host's
// program 0 and the "Init" menu item). Values are real units; anything not set loads at its default.
// Every preset except the two Init ones ends with level(): a loudness trim tuned from test renders
// (guitar phrase at about -18 dB RMS, tools/agentPresets/trim.py) plus the -1.2 dBFS safety limiter.
//
// Reference: input stage inGain/outGain (dB), gate (dB, -100 off), drive (%), shape, bits (24 off),
// downsample (1 off), hpf/lpf (Hz), mix (%). Synth "syn*", sculptor "sc*", FX "fx*", equation "eq*",
// double slit "ds*" - see the builder shorthands in PresetBuilder.h for the choice texts.

namespace
{
    using presetbuilder::P;
    using presetbuilder::Library;

    // Guitar -> synth presets play from the tracked input and from MIDI notes alike
    const juce::String both = "Input + MIDI";

    //==============================================================================
    void addInit (Library& lib)
    {
        lib << P ("Init", "Init", {},
                  "Every sound control at its default: the input passes through unchanged, ready to build on.");

        lib << P ("Clean Pass-through", "Init", {},
                  "Transparent, with only a gentle 30 Hz rumble filter - a clean starting point for any source.")
                   .set ("hpf", 30.0f);
    }

    //==============================================================================
    void addGuitar (Library& lib)
    {
        lib << P ("Glass Clean", "Guitar", { "Pop", "Indie", "R&B & Soul" },
                  "Sparkling blackface-style clean with light compression, a touch of chorus and a plate; for clean electric guitar.")
                   .amp ("Clean", 3.0f, 5.0f, 5.0f, 6.5f, 6.0f).on ("fxAmpBright")
                   .cab ("2x12 Combo", 35.0f, 20.0f)
                   .comp (-22.0f, 3.0f, 15.0f, 150.0f)
                   .mod ("Chorus", 0.6f, 30.0f, 30.0f)
                   .reverb ("Plate", 18.0f, 1.8f, 50.0f)
                   .level (2.5f);

        lib << P ("Funk Strut", "Guitar", { "Funk", "R&B & Soul" },
                  "Tight, glassy funk rhythm tone: squashed attack, honky mids, dry and upfront; for single-coil chops.")
                   .amp ("Clean", 2.5f, 3.5f, 6.5f, 7.0f, 6.0f).on ("fxAmpBright")
                   .cab ("1x12 Open Back", 25.0f, 10.0f)
                   .comp (-26.0f, 6.0f, 3.0f, 90.0f)
                   .on ("fxEqOn").set ("fxEqLowCut", 110.0f).set ("fxEqMid1Freq", 900.0f).set ("fxEqMid1Gain", 3.0f)
                   .reverb ("Room", 10.0f, 0.6f, 30.0f)
                   .level (4.0f);

        lib << P ("Jazz Box", "Guitar", { "Jazz", "Blues" },
                  "Round, dark archtop tone with soft highs and a small room; for neck pickup and fingerstyle.")
                   .amp ("Clean", 3.0f, 6.5f, 5.5f, 3.0f, 3.5f)
                   .cab ("1x12 Open Back", 55.0f, 25.0f)
                   .comp (-24.0f, 2.5f, 20.0f, 200.0f)
                   .reverb ("Room", 16.0f, 1.0f, 40.0f)
                   .level (5.5f);

        lib << P ("Surf Spring", "Guitar", { "Rock", "Indie" },
                  "Bright clean twang drenched in splashy reverb with a slow tremolo; for single notes and surf licks.")
                   .amp ("Clean", 4.0f, 4.5f, 5.0f, 7.5f, 6.5f).on ("fxAmpBright")
                   .cab ("2x12 Combo", 30.0f, 15.0f)
                   .mod ("Tremolo", 5.5f, 35.0f)
                   .reverb ("Chamber", 38.0f, 2.8f, 55.0f).set ("fxRevDamping", 25.0f)
                   .level (0.0f);

        lib << P ("Country Snap", "Guitar", { "Pop", "Blues" },
                  "Snappy, compressed twang with a quick slapback echo; for chicken-picking and bright single coils.")
                   .amp ("Clean", 3.5f, 4.0f, 5.5f, 7.0f, 7.0f).on ("fxAmpBright")
                   .cab ("2x12 Combo", 20.0f, 10.0f)
                   .comp (-24.0f, 5.0f, 2.0f, 100.0f)
                   .on ("fxDelayOn").choice ("fxDelayType", "Tape").off ("fxDelaySync")
                   .set ("fxDelayTime", 110.0f).set ("fxDelayFeedback", 12.0f).set ("fxDelayMix", 22.0f)
                   .level (3.5f);

        lib << P ("Plexi Crunch", "Guitar", { "Rock", "Blues" },
                  "Classic British crunch: chords ring clear, single notes bark; for humbuckers and power chords.")
                   .amp ("Crunch", 6.0f, 5.5f, 6.0f, 6.0f, 5.5f)
                   .cab ("4x12 Closed (V30)", 30.0f, 15.0f)
                   .reverb ("Room", 12.0f, 1.0f, 45.0f)
                   .level (5.0f);

        lib << P ("Blues Breaker", "Guitar", { "Blues", "Rock" },
                  "Warm, vocal edge-of-breakup with a fat midrange and tape slapback; for expressive lead and rhythm.")
                   .amp ("Crunch", 4.5f, 5.0f, 7.0f, 5.0f, 5.0f)
                   .cab ("2x12 Combo", 40.0f, 20.0f)
                   .delay ("Tape", "1/8", 18.0f, 14.0f)
                   .reverb ("Plate", 14.0f, 1.6f, 45.0f)
                   .level (4.5f);

        lib << P ("Singing Lead", "Guitar", { "Rock", "Metal", "Cinematic" },
                  "Smooth, compressed, endlessly sustaining lead with dotted-eighth echoes and a wide hall; for solos.")
                   .amp ("Lead", 7.0f, 5.0f, 6.5f, 5.5f, 5.5f)
                   .cab ("4x12 Closed (V30)", 35.0f, 15.0f)
                   .delay ("Ping-pong", "1/8D", 35.0f, 20.0f).set ("fxDelayDuck", 40.0f)
                   .reverb ("Hall", 18.0f, 2.8f, 65.0f)
                   .level (7.0f);

        lib << P ("Modern Chug", "Guitar", { "Metal" },
                  "Tight, scooped high-gain rhythm with a noise gate; for palm-muted riffs and drop tunings.")
                   .amp ("High Gain", 7.0f, 6.0f, 4.0f, 6.0f, 6.0f).set ("fxAmpGate", -62.0f)
                   .cab ("4x12 Closed (V30)", 25.0f, 8.0f)
                   .on ("fxEqOn").set ("fxEqLowCut", 80.0f).set ("fxEqHighCut", 11000.0f)
                   .level (7.0f);

        lib << P ("Djent Razor", "Guitar", { "Metal", "Experimental" },
                  "Ultra-tight, bright and compressed modern metal tone with a hard gate; for fast staccato riffing.")
                   .amp ("High Gain", 8.0f, 4.5f, 6.0f, 6.5f, 7.0f).set ("fxAmpGate", -55.0f)
                   .cab ("4x12 Closed (V30)", 20.0f, 5.0f)
                   .comp (-20.0f, 4.0f, 5.0f, 80.0f)
                   .on ("fxEqOn").set ("fxEqLowCut", 110.0f).set ("fxEqMid1Freq", 500.0f).set ("fxEqMid1Gain", -3.0f)
                   .level (3.0f);

        lib << P ("Fuzz Factory", "Guitar", { "Rock", "Indie" },
                  "Thick, woolly vintage fuzz that blooms on sustained notes; for riffs and big chords.")
                   .amp ("Fuzz", 7.0f, 6.0f, 5.0f, 5.0f, 5.0f)
                   .cab ("2x12 Combo", 35.0f, 20.0f)
                   .reverb ("Room", 10.0f, 1.0f, 40.0f)
                   .level (7.0f);

        lib << P ("Formula Fuzz", "Guitar", { "Rock", "Metal", "Funk" },
                  "Biased tube-style fuzz from the equation stage: thick even harmonics, tight lows; for riffs.")
                   .equation ("Tube bias (asymmetric)", 0.35f, 0.8f)
                   .set ("eqOut", -6.0f)
                   .set ("hpf", 80.0f)
                   .cab ("2x12 Combo", 35.0f, 15.0f)
                   .level (4.5f);

        lib << P ("Octave Up Fuzz", "Guitar", { "Rock", "Indie", "Experimental" },
                  "Full-wave rectified fuzz that adds a ringing octave above single notes; play above the 12th fret.")
                   .equation ("Rectify + curve", 1.0f, 0.4f)
                   .set ("eqOut", -3.0f)
                   .set ("hpf", 120.0f)
                   .amp ("Fuzz", 4.0f, 4.5f, 6.0f, 5.0f, 5.0f)
                   .cab ("1x12 Open Back", 35.0f, 15.0f)
                   .level (6.0f);

        lib << P ("Warm Tape Drive", "Guitar", { "Blues", "Rock", "Lo-fi" },
                  "Soft tape-style saturation with a rolled-off top; thickens clean guitar, bass or vocals.")
                   .set ("inGain", 2.0f)
                   .set ("drive", 32.0f).choice ("shape", "Soft (tanh)")
                   .set ("hpf", 40.0f).set ("lpf", 7000.0f)
                   .amp ("Tube/Tape", 4.0f)
                   .level (-4.5f);

        lib << P ("Shoegaze Wall", "Guitar", { "Indie", "Ambient", "Rock" },
                  "A wall of fuzz washed in ensemble chorus and shimmering reverb; for strummed chords and slow melodies.")
                   .amp ("Fuzz", 6.0f, 5.5f, 5.5f, 4.5f, 4.5f)
                   .cab ("2x12 Combo", 45.0f, 30.0f)
                   .mod ("Ensemble", 0.4f, 60.0f, 50.0f)
                   .reverb ("Shimmer", 45.0f, 6.0f, 85.0f).set ("fxRevShimmer", 35.0f)
                   .level (5.5f);

        lib << P ("Twelve-String Shine", "Guitar", { "Pop", "Indie", "Rock" },
                  "Adds a tracked octave-up shimmer under your notes like a 12-string, with chorus; for clean arpeggios.")
                   .synth ("Input", "Blend", -9.0f).set ("synOctave", 1.0f)
                   .wave ("Soft Saw").set ("synCutoff", 7000.0f)
                   .env (2.0f, 300.0f, 100.0f, 200.0f)
                   .amp ("Clean", 3.0f, 4.5f, 5.0f, 6.5f, 6.0f).on ("fxAmpBright")
                   .cab ("1x12 Open Back", 30.0f, 15.0f)
                   .mod ("Chorus", 0.8f, 40.0f, 40.0f)
                   .reverb ("Plate", 20.0f, 2.0f, 55.0f)
                   .level (3.0f);

        lib << P ("Ambient Swell", "Guitar", { "Ambient", "Cinematic" },
                  "Notes bloom softly and dissolve into long echoes and octave shimmer; for slow clean melodies.")
                   .amp ("Clean", 3.0f, 5.0f, 5.0f, 5.5f, 5.0f)
                   .cab ("1x12 Open Back", 40.0f, 25.0f)
                   .comp (-30.0f, 3.0f, 30.0f, 300.0f)
                   .delay ("Tape", "1/4D", 55.0f, 30.0f)
                   .reverb ("Shimmer", 50.0f, 8.0f, 90.0f).set ("fxRevShimmer", 45.0f)
                   .level (0.0f);
    }

    //==============================================================================
    void addBass (Library& lib)
    {
        lib << P ("Octave Bass", "Bass", { "Funk", "Pop", "R&B & Soul" },
                  "Turns guitar into a round, solid electric bass an octave down (MIDI plays it too); for single-note lines.")
                   .synth (both, "Replace").set ("synOctave", -1.0f)
                   .wave ("Sub Heavy", "Soft Saw", 35.0f)
                   .filter ("LP 12", 1400.0f, 10.0f, 1.0f).fenv (2.0f, 250.0f, 30.0f, 200.0f)
                   .env (2.0f, 800.0f, 80.0f, 120.0f)
                   .comp (-20.0f, 4.0f, 8.0f, 120.0f)
                   .amp ("Bass", 3.0f, 6.0f, 5.0f, 4.0f, 4.0f)
                   .cab ("4x10 Bass", 30.0f, 5.0f)
                   .level (16.5f);

        lib << P ("Fingerstyle P", "Bass", { "Funk", "Rock", "R&B & Soul" },
                  "Warm fingered bass with a soft thump on every note; for guitar played an octave down or for MIDI.")
                   .synth (both, "Replace").set ("synOctave", -1.0f)
                   .wave ("Soft Saw").set ("scTilt", -2.0f)
                   .filter ("Ladder 24", 700.0f, 15.0f, 2.0f).fenv (1.0f, 300.0f, 20.0f, 150.0f)
                   .env (1.0f, 1200.0f, 60.0f, 100.0f)
                   .comp (-22.0f, 4.0f, 10.0f, 150.0f)
                   .cab ("4x10 Bass", 40.0f, 5.0f)
                   .level (17.0f);

        lib << P ("Upright Thump", "Bass", { "Jazz", "Blues" },
                  "Woody acoustic upright: quick bloom, short decay and a little room; for walking lines.")
                   .synth (both, "Replace").set ("synOctave", -1.0f)
                   .wave ("Triangle", "Sine", 40.0f)
                   .set ("scFormantPos", 3.0f).set ("scFormantGain", 6.0f).set ("scFormantWidth", 1.2f)
                   .filter ("LP 12", 900.0f, 5.0f, 1.5f).fenv (5.0f, 200.0f, 0.0f, 150.0f)
                   .env (6.0f, 700.0f, 20.0f, 150.0f)
                   .reverb ("Room", 10.0f, 0.8f, 35.0f)
                   .level (14.5f);

        lib << P ("Fretless Mwah", "Bass", { "Jazz", "R&B & Soul", "Funk" },
                  "Singing fretless bass with a vocal 'mwah' filter sweep and glide; for legato guitar lines.")
                   .synth (both, "Replace").set ("synOctave", -1.0f).set ("synGlide", 70.0f)
                   .wave ("Soft Saw", "Vowel Oo", 30.0f)
                   .filter ("LP 12", 500.0f, 30.0f, 2.5f).fenv (60.0f, 500.0f, 30.0f, 300.0f)
                   .env (8.0f, 1500.0f, 70.0f, 200.0f)
                   .mod ("Chorus", 0.5f, 25.0f, 25.0f)
                   .level (11.0f);

        lib << P ("Grit Bass", "Bass", { "Rock", "Metal", "Indie" },
                  "Your guitar plus a gritty octave-down growl through a driven bass amp; for riffs that need weight.")
                   .synth ("Input", "Blend", -3.0f).set ("synOctave", -1.0f).set ("synInLevel", -4.0f)
                   .wave ("Saw").set ("synCutoff", 2500.0f)
                   .amp ("Bass", 6.5f, 6.0f, 6.0f, 5.0f, 5.0f)
                   .cab ("4x10 Bass", 30.0f, 10.0f)
                   .level (6.0f);

        lib << P ("Dub Pressure", "Bass", { "Hip-Hop & Trap", "Lo-fi", "R&B & Soul" },
                  "Deep, soft sine bass with a sub octave and a dark echo; for slow bass lines from guitar or MIDI.")
                   .synth (both, "Replace").set ("synOctave", -1.0f)
                   .wave ("Sine", "Triangle", 25.0f)
                   .set ("synSub", 35.0f).set ("synWarmth", 30.0f)
                   .filter ("LP 12", 600.0f, 0.0f)
                   .env (5.0f, 1500.0f, 80.0f, 250.0f)
                   .delay ("Tape", "1/4D", 30.0f, 12.0f).set ("fxDelayLowCut", 200.0f)
                   .level (8.0f);
    }

    //==============================================================================
    void addKeys (Library& lib)
    {
        lib << P ("Tonewheel Drawbars", "Keys & Organ", { "Jazz", "Funk", "Blues" },
                  "Full drawbar organ with a spinning-speaker chorus and a little tube growl; for chords and MIDI.")
                   .synth (both, "Replace").chord ().set ("synFollow", 40.0f)
                   .wave ("Organ (drawbars)")
                   .env (4.0f, 300.0f, 100.0f, 70.0f)
                   .mod ("Vibrato", 6.2f, 35.0f)
                   .amp ("Tube/Tape", 5.0f)
                   .reverb ("Room", 14.0f, 1.0f, 40.0f)
                   .level (2.5f);

        lib << P ("Gospel Leslie", "Keys & Organ", { "R&B & Soul", "Funk" },
                  "Bright, percussive organ through a fast rotary swirl and a breaking-up amp; for soulful stabs.")
                   .synth (both, "Replace").chord ().set ("synFollow", 50.0f)
                   .wave ("Organ (drawbars)", "Hollow", 20.0f).set ("scTilt", 1.5f)
                   .env (2.0f, 250.0f, 85.0f, 60.0f)
                   .mod ("Auto-pan", 6.5f, 45.0f)
                   .amp ("Crunch", 3.5f, 5.0f, 6.0f, 6.0f, 5.0f)
                   .cab ("1x12 Open Back", 40.0f, 30.0f)
                   .level (7.0f);

        lib << P ("Cathedral Pipes", "Keys & Organ", { "Cinematic", "Ambient" },
                  "Majestic pipe organ with a slow speak and a huge stone hall; for sustained chords and MIDI.")
                   .synth (both, "Replace").chord ().set ("synFollow", 30.0f)
                   .wave ("Organ (drawbars)", "Hollow", 45.0f)
                   .unison (2, 6.0f, 50.0f)
                   .env (90.0f, 500.0f, 100.0f, 1500.0f)
                   .reverb ("Hall", 42.0f, 6.5f, 95.0f).set ("fxRevPreDelay", 40.0f)
                   .level (-2.0f);

        lib << P ("Suitcase EP", "Keys & Organ", { "R&B & Soul", "Jazz", "Lo-fi" },
                  "Mellow tine electric piano with a bell-like attack and stereo tremolo; plays every note of your guitar chords, or MIDI.")
                   .synth (both, "Replace").chord (20.0f)
                   .wave ("Sine", "Bell", 22.0f).set ("synWarmth", 25.0f)
                   .env (2.0f, 2200.0f, 25.0f, 400.0f)
                   .filter ("LP 12", 3500.0f, 5.0f, 1.5f).fenv (1.0f, 600.0f, 0.0f, 300.0f)
                   .mod ("Auto-pan", 4.0f, 40.0f)
                   .amp ("Tube/Tape", 4.0f)
                   .reverb ("Plate", 16.0f, 1.6f, 45.0f)
                   .level (8.5f);

        lib << P ("Wurly Bark", "Keys & Organ", { "Pop", "Indie", "Funk" },
                  "Reedy electric piano that barks when you dig in, with amp tremolo; for rhythm parts.")
                   .synth (both, "Replace")
                   .wave ("Soft Square", "Clarinet", 30.0f)
                   .env (2.0f, 1500.0f, 30.0f, 250.0f)
                   .filter ("LP 12", 2200.0f, 10.0f, 2.0f).fenv (1.0f, 400.0f, 10.0f, 200.0f)
                   .amp ("Crunch", 3.0f, 5.0f, 6.0f, 5.0f, 5.0f)
                   .mod ("Tremolo", 5.0f, 30.0f)
                   .level (12.5f);

        lib << P ("Clav Funk", "Keys & Organ", { "Funk", "R&B & Soul" },
                  "Snappy, nasal clavinet with an auto-wah-style envelope and phaser; for staccato funk riffs.")
                   .synth (both, "Replace").set ("synOnsetSens", 70.0f)
                   .wave ("Pulse 10%")
                   .filter ("LP 12", 1400.0f, 45.0f, 2.5f).fenv (1.0f, 250.0f, 10.0f, 100.0f)
                   .env (1.0f, 600.0f, 25.0f, 90.0f)
                   .mod ("Phaser", 0.7f, 50.0f, 40.0f)
                   .level (20.0f);

        lib << P ("Harmonium Drone", "Keys & Organ", { "Ambient", "Indie", "Cinematic" },
                  "Breathy reed organ with a slow bellows swell and gentle chorus; for drones and held chords.")
                   .synth (both, "Replace").chord ().set ("synFollow", 35.0f)
                   .wave ("Clarinet", "Hollow", 40.0f)
                   .set ("synNoise", 6.0f).set ("synNoiseColour", -40.0f)
                   .env (220.0f, 500.0f, 100.0f, 700.0f)
                   .set ("synLfoAmp", 10.0f).set ("synLfoRate", 0.8f)
                   .mod ("Chorus", 0.4f, 35.0f, 35.0f)
                   .reverb ("Chamber", 22.0f, 2.2f, 55.0f)
                   .level (0.0f);
    }

    //==============================================================================
    void addLeads (Library& lib)
    {
        lib << P ("Saw Hero", "Synth Lead", { "EDM", "Synthwave", "Rock" },
                  "Fat, detuned saw lead that follows every bend of your guitar, with echoes and hall; for solos and MIDI.")
                   .synth (both, "Replace").set ("synGlide", 30.0f)
                   .wave ("Bright Saw").unison (3, 10.0f, 60.0f)
                   .filter ("Ladder 24", 3500.0f, 20.0f, 1.0f).fenv (2.0f, 400.0f, 50.0f, 300.0f)
                   .env (3.0f, 400.0f, 90.0f, 250.0f)
                   .delay ("Ping-pong", "1/8D", 35.0f, 18.0f)
                   .reverb ("Hall", 16.0f, 2.4f, 60.0f)
                   .level (15.5f);

        lib << P ("Synthwave Lead", "Synth Lead", { "Synthwave", "Pop" },
                  "Wide, glossy 80s lead with ensemble chorus and dotted delays; for melodies on guitar or keys.")
                   .synth (both, "Replace").set ("synGlide", 20.0f)
                   .wave ("Saw", "Square", 25.0f).unison (5, 16.0f, 80.0f)
                   .filter ("LP 12", 4000.0f, 15.0f)
                   .env (5.0f, 400.0f, 90.0f, 300.0f)
                   .mod ("Ensemble", 0.5f, 50.0f, 35.0f)
                   .delay ("Ping-pong", "1/4D", 40.0f, 20.0f)
                   .reverb ("Plate", 20.0f, 2.5f, 60.0f)
                   .level (7.5f);

        lib << P ("Chip Hero", "Synth Lead", { "Experimental", "Lo-fi", "EDM" },
                  "Square-wave 8-bit lead snapped to semitones with a crisp echo; for game-style melodies.")
                   .synth (both, "Replace").choice ("synScale", "Chromatic")
                   .wave ("Square").set ("scPartials", 24.0f)
                   .env (1.0f, 300.0f, 85.0f, 80.0f)
                   .vibrato (6.0f, 8.0f)
                   .set ("bits", 10.0f)
                   .delay ("Digital", "1/8", 30.0f, 18.0f)
                   .level (9.5f);

        lib << P ("Sync Scream", "Synth Lead", { "Rock", "EDM", "Synthwave" },
                  "Aggressive, vocal sync-style lead with a resonant formant peak pushed into an amp; for screaming solos.")
                   .synth (both, "Replace").set ("synGlide", 25.0f)
                   .wave ("Bright Saw").set ("scFormantPos", 12.0f).set ("scFormantGain", 10.0f).set ("scFormantWidth", 0.8f)
                   .filter ("LP 12", 5000.0f, 30.0f)
                   .env (2.0f, 300.0f, 90.0f, 200.0f)
                   .amp ("Lead", 3.5f, 4.5f, 6.0f, 5.0f, 5.0f)
                   .cab ("4x12 Closed (V30)", 40.0f, 15.0f)
                   .delay ("Digital", "1/8D", 30.0f, 15.0f)
                   .level (11.0f);

        lib << P ("Snap Tune Lead", "Synth Lead", { "Pop", "Hip-Hop & Trap", "R&B & Soul" },
                  "Pitch-snapped, gliding lead with that hard-tuned vocal feel; for melodies on guitar or voice.")
                   .synth (both, "Replace").choice ("synScale", "Chromatic").set ("synGlide", 60.0f)
                   .wave ("Soft Saw", "Vowel Ee", 35.0f)
                   .filter ("LP 12", 4500.0f, 10.0f)
                   .env (3.0f, 400.0f, 90.0f, 200.0f)
                   .mod ("Chorus", 0.7f, 25.0f, 25.0f)
                   .reverb ("Plate", 16.0f, 1.8f, 50.0f)
                   .level (10.0f);

        lib << P ("Theremin", "Synth Lead", { "Cinematic", "Experimental" },
                  "Eerie, gliding pure tone with a singing vibrato in a dark hall; for slow, sliding melodies.")
                   .synth (both, "Replace").set ("synGlide", 220.0f).set ("synFollow", 45.0f)
                   .wave ("Sine", "Triangle", 15.0f)
                   .env (60.0f, 500.0f, 100.0f, 500.0f)
                   .vibrato (5.5f, 22.0f)
                   .reverb ("Hall", 28.0f, 3.5f, 75.0f)
                   .level (1.0f);

        lib << P ("Whistle Lead", "Synth Lead", { "Pop", "Indie", "Lo-fi" },
                  "Airy whistled melody an octave up with breath noise and a soft room; for simple tunes.")
                   .synth (both, "Replace").set ("synOctave", 1.0f).set ("synGlide", 40.0f)
                   .wave ("Sine")
                   .set ("synNoise", 10.0f).set ("synNoiseColour", 50.0f)
                   .env (25.0f, 300.0f, 90.0f, 200.0f)
                   .vibrato (5.0f, 10.0f)
                   .reverb ("Room", 18.0f, 1.2f, 45.0f)
                   .level (10.0f);

        lib << P ("Mono Brass Lead", "Synth Lead", { "Funk", "Synthwave" },
                  "Punchy analog brass lead with a filter swell on every note; for funky lines and hooks.")
                   .synth (both, "Replace").set ("synGlide", 35.0f).set ("synVoices", 1.0f)
                   .wave ("Saw", "Brass", 40.0f)
                   .filter ("Ladder 24", 900.0f, 25.0f, 3.0f).fenv (40.0f, 400.0f, 45.0f, 200.0f)
                   .env (10.0f, 400.0f, 85.0f, 150.0f)
                   .set ("synWarmth", 35.0f)
                   .delay ("Digital", "1/8", 20.0f, 12.0f)
                   .level (15.0f);

        lib << P ("Overtone Exciter", "Synth Lead", { "Pop", "Indie", "Rock" },
                  "Adds phase-locked, organ-like overtones on top of your own guitar tone; an exciter for lead lines.")
                   .synth ("Input", "Overtones", -4.0f)
                   .wave ("Organ (drawbars)").set ("scTilt", -1.5f)
                   .amp ("Crunch", 4.0f, 5.0f, 6.0f, 5.5f, 5.0f)
                   .cab ("2x12 Combo", 35.0f, 15.0f)
                   .reverb ("Plate", 14.0f, 1.6f, 45.0f)
                   .level (4.5f);
    }

    //==============================================================================
    void addSynthBass (Library& lib)
    {
        lib << P ("Reese Rumble", "Synth Bass", { "EDM", "House & Techno", "Hip-Hop & Trap" },
                  "Dark, phasing detuned-saw bass with a warm growl; for low guitar lines or MIDI.")
                   .synth (both, "Replace").set ("synOctave", -1.0f)
                   .wave ("Saw").unison (4, 25.0f, 40.0f)
                   .filter ("Ladder 24", 600.0f, 20.0f).set ("synWarmth", 40.0f)
                   .env (3.0f, 500.0f, 95.0f, 150.0f)
                   .level (12.0f);

        lib << P ("808 Glide", "Synth Bass", { "Hip-Hop & Trap" },
                  "Booming 808-style sine bass that slides between notes and saturates softly; for trap lines.")
                   .synth (both, "Replace").set ("synOctave", -1.0f).set ("synGlide", 110.0f).set ("synVoices", 1.0f)
                   .wave ("Sine", "Sub Heavy", 30.0f).set ("synSub", 20.0f).set ("synWarmth", 45.0f)
                   .filter ("LP 12", 1200.0f, 0.0f)
                   .env (2.0f, 1800.0f, 40.0f, 300.0f)
                   .level (9.0f);

        lib << P ("Acid Squelch", "Synth Bass", { "House & Techno", "EDM" },
                  "Resonant, squelchy acid line with a fast filter snap on every note; for picked guitar or MIDI.")
                   .synth (both, "Replace").set ("synOnsetSens", 70.0f).set ("synGlide", 40.0f)
                   .wave ("Saw")
                   .filter ("Ladder 24", 350.0f, 72.0f, 4.0f).fenv (1.0f, 220.0f, 0.0f, 120.0f)
                   .set ("synFltDrive", 40.0f).set ("synKeyTrack", 30.0f)
                   .env (2.0f, 400.0f, 80.0f, 80.0f)
                   .delay ("Digital", "1/16D", 30.0f, 12.0f)
                   .level (11.5f);

        lib << P ("Fat Ladder Bass", "Synth Bass", { "Funk", "Synthwave", "R&B & Soul" },
                  "Thick classic ladder bass with a sub and a punchy filter pluck; for funky low lines.")
                   .synth (both, "Replace").set ("synOctave", -1.0f)
                   .wave ("Soft Saw", "Square", 30.0f).set ("synSub", 40.0f)
                   .filter ("Ladder 24", 500.0f, 25.0f, 2.5f).fenv (1.0f, 280.0f, 15.0f, 150.0f)
                   .env (1.0f, 700.0f, 70.0f, 100.0f)
                   .set ("synWarmth", 30.0f)
                   .level (11.5f);

        lib << P ("Pulse Runner", "Synth Bass", { "Synthwave", "EDM" },
                  "Tight pulse-wave bass for driving 16th-note lines; for muted guitar picking or MIDI.")
                   .synth (both, "Replace").set ("synOctave", -1.0f)
                   .wave ("Pulse 25%")
                   .filter ("LP 12", 1300.0f, 15.0f, 2.0f).fenv (1.0f, 180.0f, 10.0f, 100.0f)
                   .env (1.0f, 300.0f, 45.0f, 60.0f)
                   .level (14.5f);

        lib << P ("Wobble Monster", "Synth Bass", { "EDM", "Experimental" },
                  "Growling bass with a rhythmic filter wobble and drive; for held low notes.")
                   .synth (both, "Replace").set ("synOctave", -1.0f)
                   .wave ("Saw", "Square", 40.0f).unison (2, 8.0f, 30.0f)
                   .filter ("Ladder 24", 450.0f, 45.0f).set ("synFltDrive", 45.0f)
                   .set ("synLfoRate", 3.0f).set ("synLfoCutoff", 2.5f).choice ("synLfoShape", "Tri")
                   .env (3.0f, 500.0f, 100.0f, 150.0f)
                   .level (10.5f);

        lib << P ("Deep House Organ Bass", "Synth Bass", { "House & Techno" },
                  "Short, round organ bass with a soft click; for offbeat house lines from guitar or MIDI.")
                   .synth (both, "Replace").set ("synOctave", -1.0f)
                   .wave ("Organ (drawbars)", "Sine", 45.0f)
                   .env (1.0f, 450.0f, 10.0f, 90.0f)
                   .filter ("LP 12", 1800.0f, 5.0f)
                   .level (19.5f);

        lib << P ("Sub Underneath", "Synth Bass", { "Hip-Hop & Trap", "Pop", "EDM" },
                  "Keeps your guitar as it is and adds a clean sine sub an octave below; for weight on riffs and bass.")
                   .synth ("Input", "Blend", -5.0f).set ("synOctave", -1.0f)
                   .wave ("Sine")
                   .filter ("LP 12", 300.0f, 0.0f)
                   .level (5.0f);
    }

    //==============================================================================
    void addPads (Library& lib)
    {
        lib << P ("Glacier Pad", "Pad", { "Ambient", "Cinematic" },
                  "Vast, slowly blooming detuned pad with an octave shimmer tail; turns sustained notes into scenery.")
                   .synth (both, "Replace").chord ().set ("synFollow", 30.0f)
                   .wave ("Soft Saw", "Hollow", 30.0f).unison (5, 18.0f, 90.0f)
                   .filter ("LP 12", 2500.0f, 10.0f)
                   .env (700.0f, 1000.0f, 100.0f, 2500.0f)
                   .reverb ("Shimmer", 45.0f, 7.0f, 90.0f).set ("fxRevShimmer", 40.0f)
                   .level (-4.5f);

        lib << P ("Warm Blanket", "Pad", { "Lo-fi", "R&B & Soul", "Ambient" },
                  "Soft, dark analog pad with ensemble chorus; wraps every note of your guitar chords (or MIDI) in warmth.")
                   .synth (both, "Replace").chord ().set ("synFollow", 35.0f)
                   .wave ("Soft Square", "Triangle", 40.0f).unison (3, 10.0f, 70.0f)
                   .filter ("LP 12", 1300.0f, 5.0f)
                   .env (400.0f, 800.0f, 100.0f, 1500.0f)
                   .mod ("Ensemble", 0.4f, 55.0f, 40.0f)
                   .reverb ("Hall", 25.0f, 3.0f, 70.0f)
                   .level (-4.0f);

        lib << P ("Guitar Halo", "Pad", { "Ambient", "Indie", "Pop" },
                  "Your guitar stays in front while a soft octave-up pad swells behind every note; for clean arpeggios.")
                   .synth ("Input", "Blend", -8.0f).set ("synOctave", 1.0f).set ("synFollow", 60.0f)
                   .wave ("Hollow", "Sine", 40.0f).unison (3, 12.0f, 90.0f)
                   .env (500.0f, 800.0f, 100.0f, 1800.0f)
                   .filter ("LP 12", 3500.0f, 5.0f)
                   .reverb ("Shimmer", 35.0f, 5.0f, 85.0f).set ("fxRevShimmer", 30.0f)
                   .level (1.0f);

        lib << P ("Dark Matter", "Pad", { "Cinematic", "Ambient", "Experimental" },
                  "Ominous low pad with a slow filter drift and a cavernous hall; for tension under films and games.")
                   .synth (both, "Replace").chord ().set ("synOctave", -1.0f).set ("synFollow", 30.0f)
                   .wave ("Hollow", "Strings", 40.0f).unison (3, 14.0f, 80.0f)
                   .filter ("Ladder 24", 700.0f, 30.0f)
                   .set ("synLfoRate", 0.12f).set ("synLfoCutoff", 1.2f)
                   .env (900.0f, 1000.0f, 100.0f, 3000.0f)
                   .reverb ("Hall", 40.0f, 8.0f, 95.0f)
                   .level (0.0f);

        lib << P ("Sunset Strings Pad", "Pad", { "Synthwave", "Pop" },
                  "Lush 80s poly pad with a wide detune, ensemble and plate; for big chorus chords.")
                   .synth (both, "Replace").chord ().set ("synFollow", 40.0f)
                   .wave ("Saw").unison (6, 15.0f, 90.0f)
                   .filter ("LP 12", 3000.0f, 10.0f, 1.0f).fenv (400.0f, 1500.0f, 50.0f, 1000.0f)
                   .env (300.0f, 1000.0f, 100.0f, 1200.0f)
                   .mod ("Ensemble", 0.5f, 50.0f, 40.0f)
                   .reverb ("Plate", 28.0f, 3.0f, 65.0f)
                   .level (0.5f);

        lib << P ("Breathing Pad", "Pad", { "Ambient", "Cinematic" },
                  "A soft vowel pad that slowly breathes in and out with airy noise; for long held notes.")
                   .synth (both, "Replace").chord ().set ("synFollow", 30.0f)
                   .wave ("Vowel Oo", "Vowel Ah", 30.0f).unison (3, 10.0f, 80.0f)
                   .set ("synNoise", 12.0f).set ("synNoiseColour", 30.0f)
                   .set ("synLfoRate", 0.25f).set ("synLfoAmp", 35.0f)
                   .env (800.0f, 1000.0f, 100.0f, 2500.0f)
                   .reverb ("Hall", 35.0f, 5.0f, 85.0f)
                   .level (4.5f);

        lib << P ("Frozen Air", "Pad", { "Ambient", "Experimental", "Cinematic" },
                  "Notes freeze into an endless, glittering reverb cloud; for sparse single notes and swells.")
                   .synth ("Input", "Blend", -6.0f).set ("synOctave", 1.0f).set ("synFollow", 50.0f)
                   .wave ("Sine", "Bell", 20.0f)
                   .env (300.0f, 1000.0f, 100.0f, 3000.0f)
                   .reverb ("Shimmer", 60.0f, 18.0f, 100.0f).set ("fxRevShimmer", 55.0f).set ("fxRevDamping", 30.0f)
                   .level (-2.0f);
    }

    //==============================================================================
    void addPlucks (Library& lib)
    {
        lib << P ("Crystal Pluck", "Pluck & Arp", { "EDM", "Pop" },
                  "Bright, glassy synth pluck with ping-pong echoes; every picked note becomes a sparkling pluck.")
                   .synth (both, "Replace").set ("synOnsetSens", 65.0f)
                   .wave ("Bright Saw", "Bell", 20.0f).unison (2, 8.0f, 60.0f)
                   .filter ("Ladder 24", 700.0f, 20.0f, 4.0f).fenv (1.0f, 230.0f, 0.0f, 200.0f)
                   .env (1.0f, 750.0f, 0.0f, 400.0f)
                   .delay ("Ping-pong", "1/8D", 40.0f, 22.0f)
                   .reverb ("Plate", 18.0f, 2.0f, 55.0f)
                   .level (20.0f);

        lib << P ("Kalimba Drops", "Pluck & Arp", { "Lo-fi", "Ambient", "Pop" },
                  "Soft thumb-piano plucks with a woody ring and a small room; for gentle picked patterns.")
                   .synth (both, "Replace").set ("synOnsetSens", 60.0f)
                   .wave ("Sine", "Bell", 40.0f).set ("scStretch", 6.0f)
                   .env (1.0f, 700.0f, 0.0f, 400.0f)
                   .filter ("LP 12", 3500.0f, 5.0f)
                   .reverb ("Room", 20.0f, 1.2f, 45.0f)
                   .level (17.5f);

        lib << P ("Future Pluck", "Pluck & Arp", { "EDM", "House & Techno", "Pop" },
                  "Wide, chorused supersaw pluck with tempo-synced 16th echoes; for chord stabs and arps.")
                   .synth (both, "Replace").chord (25.0f).set ("synOnsetSens", 65.0f)
                   .wave ("Saw").unison (4, 14.0f, 90.0f)
                   .filter ("LP 12", 900.0f, 25.0f, 3.5f).fenv (1.0f, 180.0f, 0.0f, 150.0f)
                   .env (1.0f, 600.0f, 0.0f, 300.0f)
                   .mod ("Chorus", 0.8f, 40.0f, 35.0f)
                   .delay ("Ping-pong", "1/16", 35.0f, 18.0f)
                   .level (19.5f);

        lib << P ("Synthwave Arp", "Pluck & Arp", { "Synthwave" },
                  "Retro pulse arp with a quick filter blip and a long digital echo; for picked arpeggios.")
                   .synth (both, "Replace").set ("synOnsetSens", 65.0f)
                   .wave ("Pulse 25%")
                   .filter ("LP 12", 1200.0f, 20.0f, 3.0f).fenv (1.0f, 200.0f, 0.0f, 150.0f)
                   .env (1.0f, 300.0f, 10.0f, 200.0f)
                   .delay ("Digital", "1/8D", 45.0f, 25.0f)
                   .reverb ("Plate", 15.0f, 1.8f, 50.0f)
                   .level (19.0f);

        lib << P ("Gated Sparkle", "Pluck & Arp", { "Pop", "Indie" },
                  "Your guitar plus a short square-wave blip an octave up on every pick; for arpeggios that cut through.")
                   .synth ("Input", "Blend", -6.0f).set ("synOctave", 1.0f).set ("synOnsetSens", 70.0f)
                   .wave ("Soft Square")
                   .env (1.0f, 180.0f, 0.0f, 120.0f)
                   .filter ("LP 12", 5000.0f, 10.0f)
                   .delay ("Ping-pong", "1/8", 30.0f, 15.0f)
                   .level (5.0f);

        lib << P ("Sitar Drone", "Pluck & Arp", { "Experimental", "Indie" },
                  "Buzzing, sympathetic sitar-like pluck with a bright jawari comb; for single-string melodies.")
                   .synth (both, "Replace").set ("synGlide", 40.0f)
                   .wave ("Bright Saw").set ("scComb", 45.0f).set ("scCombOffset", 9.0f).set ("scStretch", 3.0f)
                   .set ("scFormantPos", 14.0f).set ("scFormantGain", 6.0f)
                   .env (1.0f, 2500.0f, 15.0f, 600.0f)
                   .filter ("LP 12", 6000.0f, 10.0f)
                   .reverb ("Chamber", 20.0f, 2.0f, 55.0f)
                   .level (16.0f);

        lib << P ("Harp Glissando", "Pluck & Arp", { "Cinematic", "Ambient" },
                  "Soft concert-harp pluck with a gentle hall; for arpeggios and broken chords.")
                   .synth (both, "Replace").chord (15.0f)
                   .wave ("Triangle", "Strings", 25.0f)
                   .env (1.0f, 1600.0f, 0.0f, 900.0f)
                   .filter ("LP 12", 2800.0f, 5.0f, 1.5f).fenv (1.0f, 500.0f, 0.0f, 300.0f)
                   .reverb ("Hall", 28.0f, 3.0f, 70.0f)
                   .level (10.5f);

        lib << P ("Koto Pluck", "Pluck & Arp", { "Cinematic", "Experimental" },
                  "Bright, twangy koto-style pluck snapped to E minor pentatonic; for pentatonic picking.")
                   .synth (both, "Replace").set ("synGlide", 25.0f).choice ("synScale", "Minor Pentatonic").choice ("synKey", "E")
                   .wave ("Bright Saw", "Hollow", 50.0f).set ("scFormantPos", 9.0f).set ("scFormantGain", 5.0f)
                   .env (1.0f, 900.0f, 0.0f, 400.0f)
                   .filter ("LP 12", 2500.0f, 15.0f, 2.5f).fenv (1.0f, 250.0f, 0.0f, 200.0f)
                   .reverb ("Room", 18.0f, 1.2f, 45.0f)
                   .level (19.5f);
    }

    //==============================================================================
    void addStringsBrass (Library& lib)
    {
        lib << P ("Solo Violin", "Strings & Brass", { "Cinematic", "Ambient" },
                  "Expressive bowed violin with a gentle attack, natural vibrato and concert hall; for slow guitar melodies.")
                   .synth (both, "Replace").set ("synGlide", 50.0f).set ("synFollow", 45.0f)
                   .wave ("Strings").set ("scFormantPos", 10.0f).set ("scFormantGain", 4.0f)
                   .env (110.0f, 600.0f, 95.0f, 400.0f)
                   .filter ("LP 12", 6000.0f, 5.0f)
                   .vibrato (5.6f, 14.0f)
                   .reverb ("Hall", 28.0f, 2.8f, 70.0f)
                   .level (4.0f);

        lib << P ("Cello Section", "Strings & Brass", { "Cinematic" },
                  "Warm, rich cello ensemble an octave down with a slow bow; for lines and held notes.")
                   .synth (both, "Replace").set ("synOctave", -1.0f).set ("synFollow", 45.0f)
                   .wave ("Strings", "Soft Saw", 20.0f).unison (3, 8.0f, 70.0f)
                   .env (180.0f, 800.0f, 100.0f, 700.0f)
                   .filter ("LP 12", 3000.0f, 5.0f)
                   .vibrato (5.0f, 8.0f)
                   .reverb ("Hall", 30.0f, 3.2f, 75.0f)
                   .level (3.5f);

        lib << P ("String Machine", "Strings & Brass", { "Synthwave", "Pop", "Indie" },
                  "Vintage 70s string ensemble: buzzy saws in a swirling chorus; every strummed note becomes a string voice.")
                   .synth (both, "Replace").chord ().set ("synFollow", 40.0f)
                   .wave ("Saw").unison (3, 9.0f, 80.0f)
                   .filter ("LP 12", 3800.0f, 5.0f)
                   .env (200.0f, 800.0f, 100.0f, 900.0f)
                   .mod ("Ensemble", 0.6f, 70.0f, 55.0f)
                   .reverb ("Plate", 20.0f, 2.2f, 55.0f)
                   .level (1.0f);

        lib << P ("Pizzicato Strings", "Strings & Brass", { "Cinematic", "Pop" },
                  "Short plucked orchestral strings in a warm hall; for playful picked lines.")
                   .synth (both, "Replace").set ("synOnsetSens", 65.0f)
                   .wave ("Strings", "Triangle", 30.0f).unison (2, 6.0f, 60.0f)
                   .env (2.0f, 350.0f, 0.0f, 250.0f)
                   .filter ("LP 12", 3000.0f, 5.0f, 1.0f).fenv (1.0f, 200.0f, 0.0f, 150.0f)
                   .reverb ("Hall", 25.0f, 2.2f, 65.0f)
                   .level (20.0f);

        lib << P ("Brass Section", "Strings & Brass", { "Funk", "Cinematic", "R&B & Soul" },
                  "Bold horn section with a brassy filter bloom on every note; for stabs and riffs.")
                   .synth (both, "Replace").set ("synFollow", 75.0f)
                   .wave ("Brass").unison (3, 9.0f, 70.0f)
                   .filter ("Ladder 24", 1500.0f, 10.0f, 2.0f).fenv (60.0f, 400.0f, 60.0f, 250.0f)
                   .env (35.0f, 500.0f, 90.0f, 250.0f)
                   .reverb ("Plate", 18.0f, 1.8f, 55.0f)
                   .level (11.0f);

        lib << P ("Muted Trumpet", "Strings & Brass", { "Jazz", "Lo-fi" },
                  "Soft, nasal muted trumpet with a slight vibrato in a small club; for jazzy guitar melodies.")
                   .synth (both, "Replace").set ("synGlide", 30.0f).set ("synFollow", 75.0f)
                   .wave ("Brass").set ("scFormantPos", 7.0f).set ("scFormantGain", 8.0f).set ("scFormantWidth", 0.7f)
                   .filter ("LP 12", 2200.0f, 15.0f, 1.0f).fenv (40.0f, 400.0f, 50.0f, 200.0f)
                   .env (30.0f, 500.0f, 90.0f, 200.0f)
                   .vibrato (5.2f, 9.0f)
                   .reverb ("Room", 20.0f, 1.2f, 45.0f)
                   .level (13.0f);

        lib << P ("French Horns", "Strings & Brass", { "Cinematic" },
                  "Round, noble horn section with a soft attack and a big hall; for heroic melodies.")
                   .synth (both, "Replace").set ("synFollow", 45.0f)
                   .wave ("Brass", "Hollow", 40.0f).unison (2, 6.0f, 60.0f)
                   .filter ("LP 12", 1100.0f, 5.0f, 1.0f).fenv (120.0f, 600.0f, 70.0f, 400.0f)
                   .env (110.0f, 600.0f, 100.0f, 600.0f)
                   .reverb ("Hall", 32.0f, 3.5f, 80.0f)
                   .level (5.0f);

        lib << P ("80s Synth Brass", "Strings & Brass", { "Synthwave", "Pop", "Funk" },
                  "Punchy poly-synth brass with a classic filter swell; for big chord stabs.")
                   .synth (both, "Replace").chord (15.0f)
                   .wave ("Saw").unison (2, 10.0f, 60.0f)
                   .filter ("Ladder 24", 1000.0f, 15.0f, 3.0f).fenv (30.0f, 350.0f, 40.0f, 200.0f)
                   .env (12.0f, 400.0f, 85.0f, 250.0f)
                   .mod ("Chorus", 0.5f, 30.0f, 30.0f)
                   .reverb ("Plate", 16.0f, 1.8f, 50.0f)
                   .level (11.0f);
    }

    //==============================================================================
    void addWinds (Library& lib)
    {
        lib << P ("Breathy Flute", "Winds", { "Cinematic", "Ambient", "Jazz" },
                  "Airy concert flute an octave up with breath noise and soft vibrato; for slow guitar melodies.")
                   .synth (both, "Replace").set ("synOctave", 1.0f).set ("synGlide", 35.0f).set ("synFollow", 75.0f)
                   .wave ("Hollow", "Sine", 45.0f).set ("scPartials", 12.0f)
                   .set ("synNoise", 12.0f).set ("synNoiseColour", 40.0f)
                   .env (60.0f, 500.0f, 95.0f, 250.0f)
                   .vibrato (5.0f, 9.0f)
                   .reverb ("Hall", 25.0f, 2.5f, 65.0f)
                   .level (5.0f);

        lib << P ("Woody Clarinet", "Winds", { "Jazz", "Cinematic" },
                  "Dark, hollow clarinet with a reedy midrange; for legato lines in the middle register.")
                   .synth (both, "Replace").set ("synGlide", 30.0f).set ("synFollow", 75.0f)
                   .wave ("Clarinet")
                   .set ("synNoise", 4.0f).set ("synNoiseColour", -20.0f)
                   .filter ("LP 12", 4000.0f, 5.0f)
                   .env (45.0f, 500.0f, 95.0f, 200.0f)
                   .vibrato (4.8f, 5.0f)
                   .reverb ("Room", 20.0f, 1.4f, 50.0f)
                   .level (3.5f);

        lib << P ("Smoky Tenor", "Winds", { "Jazz", "R&B & Soul", "Funk" },
                  "Growly, breathy saxophone-like tone with a soft amp edge; for bluesy guitar lines.")
                   .synth (both, "Replace").set ("synGlide", 40.0f).set ("synFollow", 75.0f)
                   .wave ("Brass", "Clarinet", 45.0f).set ("scFormantPos", 4.0f).set ("scFormantGain", 8.0f).set ("scFormantWidth", 1.4f)
                   .set ("synNoise", 8.0f).set ("synNoiseColour", 10.0f)
                   .filter ("LP 12", 3200.0f, 10.0f, 1.0f).fenv (30.0f, 400.0f, 60.0f, 200.0f)
                   .env (30.0f, 500.0f, 90.0f, 200.0f)
                   .vibrato (5.0f, 10.0f)
                   .amp ("Tube/Tape", 5.0f)
                   .reverb ("Plate", 20.0f, 1.8f, 50.0f)
                   .level (8.5f);

        lib << P ("Pan Flute", "Winds", { "Ambient", "Cinematic" },
                  "Hollow, chiffy pan pipes with lots of breath and a cave-like echo; for simple modal melodies.")
                   .synth (both, "Replace").set ("synOctave", 1.0f).set ("synFollow", 75.0f)
                   .wave ("Sine", "Hollow", 25.0f)
                   .set ("synNoise", 22.0f).set ("synNoiseColour", 20.0f)
                   .env (30.0f, 500.0f, 90.0f, 300.0f)
                   .vibrato (4.5f, 6.0f)
                   .delay ("Tape", "1/4", 25.0f, 12.0f)
                   .reverb ("Chamber", 30.0f, 2.8f, 70.0f)
                   .level (7.0f);

        lib << P ("Oboe Reed", "Winds", { "Cinematic", "Jazz" },
                  "Bright, nasal double-reed tone with a focused formant; for expressive solo lines.")
                   .synth (both, "Replace").set ("synGlide", 30.0f).set ("synFollow", 75.0f)
                   .wave ("Pulse 25%", "Clarinet", 40.0f).set ("scFormantPos", 10.0f).set ("scFormantGain", 8.0f).set ("scFormantWidth", 0.8f)
                   .filter ("LP 12", 3500.0f, 5.0f)
                   .env (40.0f, 500.0f, 95.0f, 200.0f)
                   .vibrato (5.5f, 8.0f)
                   .reverb ("Hall", 22.0f, 2.2f, 60.0f)
                   .level (8.0f);

        lib << P ("Clay Ocarina", "Winds", { "Indie", "Lo-fi", "Ambient" },
                  "Pure, round ocarina an octave up with a touch of air; for sweet, simple tunes.")
                   .synth (both, "Replace").set ("synOctave", 1.0f).set ("synFollow", 75.0f)
                   .wave ("Sine", "Triangle", 20.0f)
                   .set ("synNoise", 6.0f).set ("synNoiseColour", 30.0f)
                   .env (35.0f, 400.0f, 90.0f, 220.0f)
                   .vibrato (5.0f, 6.0f)
                   .reverb ("Room", 22.0f, 1.4f, 50.0f)
                   .level (7.0f);
    }

    //==============================================================================
    void addBells (Library& lib)
    {
        lib << P ("Ring Bells", "Bells & Mallets", { "Experimental", "Cinematic", "Ambient" },
                  "Pitch-locked ring modulation turns plucked notes into glassy, bell-like tones; for single notes.")
                   .equation ("Pitch-locked ring", 0.6f, 0.7f)
                   .set ("hpf", 60.0f)
                   .reverb ("Plate", 22.0f, 2.5f, 60.0f)
                   .level (10.0f);

        lib << P ("Marimba", "Bells & Mallets", { "Jazz", "Pop", "Lo-fi" },
                  "Warm wooden marimba with a soft mallet and short decay; for picked melodies and MIDI.")
                   .synth (both, "Replace").set ("synOnsetSens", 65.0f)
                   .wave ("Sine", "Bell", 18.0f).set ("scTilt", -4.0f)
                   .env (1.0f, 600.0f, 0.0f, 300.0f)
                   .filter ("LP 12", 3000.0f, 5.0f)
                   .reverb ("Room", 16.0f, 1.0f, 40.0f)
                   .level (15.5f);

        lib << P ("Vibraphone", "Bells & Mallets", { "Jazz", "Lo-fi", "R&B & Soul" },
                  "Mellow vibes with a pulsing motor tremolo and long ring; for jazzy chords and lines.")
                   .synth (both, "Replace").chord (15.0f)
                   .wave ("Sine", "Bell", 30.0f)
                   .env (1.0f, 2600.0f, 0.0f, 1200.0f)
                   .set ("synLfoRate", 5.0f).set ("synLfoAmp", 35.0f)
                   .reverb ("Hall", 22.0f, 2.5f, 60.0f)
                   .level (14.0f);

        lib << P ("Glockenspiel", "Bells & Mallets", { "Cinematic", "Pop", "Indie" },
                  "Sparkling, high steel glockenspiel an octave up; for twinkling melodies.")
                   .synth (both, "Replace").set ("synOctave", 1.0f)
                   .wave ("Bell").set ("scStretch", 8.0f)
                   .env (1.0f, 1400.0f, 0.0f, 900.0f)
                   .reverb ("Hall", 25.0f, 2.5f, 65.0f)
                   .level (15.5f);

        lib << P ("Steel Pan", "Bells & Mallets", { "Pop", "Funk" },
                  "Bright, singing steel drum with a soft attack and short sustain; for upbeat melodies.")
                   .synth (both, "Replace").set ("synOnsetSens", 65.0f)
                   .wave ("Bell", "Hollow", 50.0f).set ("scStretch", 4.0f).set ("scFormantPos", 4.0f).set ("scFormantGain", 6.0f)
                   .env (4.0f, 800.0f, 10.0f, 400.0f)
                   .filter ("LP 12", 4500.0f, 10.0f, 1.0f).fenv (1.0f, 300.0f, 0.0f, 200.0f)
                   .reverb ("Room", 18.0f, 1.2f, 45.0f)
                   .level (16.5f);

        lib << P ("Tubular Bells", "Bells & Mallets", { "Cinematic", "Ambient" },
                  "Huge, inharmonic church bells ringing into a long hall; for slow single notes.")
                   .synth (both, "Replace")
                   .wave ("Bell").set ("scStretch", 30.0f)
                   .env (1.0f, 5000.0f, 0.0f, 3000.0f)
                   .reverb ("Hall", 32.0f, 5.0f, 85.0f)
                   .level (6.5f);

        lib << P ("Music Box", "Bells & Mallets", { "Lo-fi", "Cinematic", "Ambient" },
                  "Tiny, delicate music-box tines two octaves up; for lullaby melodies.")
                   .synth (both, "Replace").set ("synOctave", 2.0f).set ("synOnsetSens", 65.0f)
                   .wave ("Bell", "Sine", 30.0f).set ("scStretch", 5.0f)
                   .env (1.0f, 1100.0f, 0.0f, 700.0f)
                   .filter ("LP 12", 9000.0f, 5.0f)
                   .reverb ("Plate", 25.0f, 2.2f, 55.0f)
                   .level (12.5f);

        lib << P ("Gamelan Gong", "Bells & Mallets", { "Experimental", "Ambient", "Cinematic" },
                  "Shimmering, detuned metallic gongs with combed partials; for meditative single notes.")
                   .synth (both, "Replace")
                   .wave ("Bell", "Hollow", 30.0f).set ("scStretch", 50.0f).set ("scComb", 30.0f).set ("scCombOffset", 18.0f)
                   .unison (2, 12.0f, 70.0f)
                   .env (2.0f, 3000.0f, 0.0f, 2000.0f)
                   .reverb ("Hall", 30.0f, 4.5f, 80.0f)
                   .level (10.0f);
    }

    //==============================================================================
    void addVocal (Library& lib)
    {
        lib << P ("Choir Aahs", "Vocal", { "Cinematic", "Ambient" },
                  "A soft choir singing 'aah' with a slow swell in a cathedral hall; for held notes and chords.")
                   .synth (both, "Replace").chord ().set ("synFollow", 40.0f)
                   .wave ("Vowel Ah").unison (5, 14.0f, 90.0f)
                   .env (260.0f, 800.0f, 100.0f, 1500.0f)
                   .vibrato (4.8f, 6.0f)
                   .reverb ("Hall", 38.0f, 4.5f, 90.0f)
                   .level (3.5f);

        lib << P ("Ooh Section", "Vocal", { "R&B & Soul", "Pop" },
                  "Smooth backing 'ooh' vocals with ensemble width; for soulful pads behind a lead.")
                   .synth (both, "Replace").set ("synFollow", 45.0f)
                   .wave ("Vowel Oo", "Vowel Ah", 20.0f).unison (4, 10.0f, 85.0f)
                   .env (200.0f, 800.0f, 100.0f, 1000.0f)
                   .mod ("Ensemble", 0.4f, 45.0f, 35.0f)
                   .reverb ("Plate", 26.0f, 2.6f, 65.0f)
                   .level (7.5f);

        lib << P ("Robot Voice", "Vocal", { "Experimental", "EDM", "Hip-Hop & Trap" },
                  "Hard-snapped, buzzy vocoder-style robot tone; for voice or guitar lines with a sci-fi edge.")
                   .synth (both, "Replace").choice ("synScale", "Chromatic")
                   .wave ("Vowel Ee", "Square", 35.0f).set ("scPartials", 32.0f)
                   .env (2.0f, 300.0f, 95.0f, 100.0f)
                   .equation ("Quantize steps", 0.15f, 0.4f)
                   .mod ("Flanger", 0.3f, 30.0f, 25.0f)
                   .level (12.5f);

        lib << P ("Talk Box", "Vocal", { "Funk", "R&B & Soul", "Rock" },
                  "Talking, wah-like formant sweep on a gritty synth; for funky guitar licks.")
                   .synth (both, "Replace").set ("synGlide", 25.0f)
                   .wave ("Vowel Ah", "Vowel Ee", 50.0f)
                   .filter ("Band", 900.0f, 30.0f, 2.0f).fenv (40.0f, 500.0f, 30.0f, 200.0f)
                   .set ("synLfoRate", 1.5f).set ("synLfoCutoff", 0.6f)
                   .env (5.0f, 400.0f, 90.0f, 150.0f)
                   .amp ("Crunch", 4.0f, 5.0f, 6.5f, 5.0f, 5.0f)
                   .cab ("1x12 Open Back", 35.0f, 15.0f)
                   .level (18.0f);

        lib << P ("Octave Choir", "Vocal", { "Pop", "Cinematic", "R&B & Soul" },
                  "Your input plus a soft choir doubling it an octave up; for vocals or clean guitar melodies.")
                   .synth ("Input", "Blend", -7.0f).set ("synOctave", 1.0f).set ("synFollow", 70.0f)
                   .wave ("Vowel Ah", "Vowel Oo", 40.0f).unison (3, 10.0f, 90.0f)
                   .env (120.0f, 600.0f, 100.0f, 600.0f)
                   .reverb ("Hall", 25.0f, 2.8f, 70.0f)
                   .level (4.0f);

        lib << P ("Whisper Air", "Vocal", { "Ambient", "Experimental" },
                  "Ghostly, breathy whispers that follow your notes; for atmospheric textures from any input.")
                   .synth (both, "Replace").set ("synFollow", 45.0f)
                   .wave ("Vowel Oo", "Vowel Ee", 40.0f)
                   .set ("synNoise", 55.0f).set ("synNoiseColour", 20.0f)
                   .filter ("Band", 1800.0f, 20.0f).set ("synKeyTrack", 80.0f)
                   .env (150.0f, 600.0f, 90.0f, 900.0f)
                   .reverb ("Hall", 40.0f, 4.0f, 85.0f)
                   .level (11.5f);
    }

    //==============================================================================
    void addDrums (Library& lib)
    {
        lib << P ("Body Kick", "Drums & Perc", { "Hip-Hop & Trap", "House & Techno" },
                  "Every pick becomes a deep, round kick-drum thump two octaves down; for muted single notes or MIDI.")
                   .synth (both, "Replace").set ("synOctave", -2.0f).set ("synOnsetSens", 80.0f).set ("synFollow", 100.0f)
                   .wave ("Sine").set ("synWarmth", 55.0f)
                   .filter ("LP 12", 250.0f, 5.0f, 3.0f).fenv (0.5f, 60.0f, 0.0f, 60.0f)
                   .env (0.5f, 220.0f, 0.0f, 90.0f)
                   .level (20.0f);

        lib << P ("Shaker Hits", "Drums & Perc", { "Hip-Hop & Trap", "House & Techno", "Pop" },
                  "Turns picked notes into crisp shaker and hat ticks; for rhythmic muted strumming.")
                   .synth (both, "Replace", 6.0f).set ("synOnsetSens", 85.0f)
                   .wave ("Sine").set ("synNoise", 100.0f).set ("synNoiseColour", 40.0f)
                   .filter ("High", 500.0f, 10.0f)
                   .env (0.5f, 600.0f, 0.0f, 150.0f)
                   .level (20.0f);

        lib << P ("Snare Crack", "Drums & Perc", { "Rock", "Pop", "Hip-Hop & Trap" },
                  "A snappy noise burst with a tonal body on every hit, plus a short plate; for percussive picking.")
                   .synth (both, "Replace").set ("synOnsetSens", 85.0f).set ("synOctave", -1.0f)
                   .wave ("Triangle").set ("synNoise", 70.0f).set ("synNoiseColour", 30.0f)
                   .filter ("High", 180.0f, 10.0f)
                   .env (0.5f, 260.0f, 0.0f, 120.0f)
                   .reverb ("Plate", 18.0f, 0.9f, 35.0f)
                   .level (20.0f);

        lib << P ("Woodblock", "Drums & Perc", { "Funk", "Experimental", "Pop" },
                  "Hollow, clicky woodblock pitched to your notes; for percussive melodies.")
                   .synth (both, "Replace").set ("synOctave", 1.0f).set ("synOnsetSens", 80.0f)
                   .wave ("Hollow")
                   .filter ("LP 12", 3000.0f, 30.0f).set ("synKeyTrack", 100.0f)
                   .env (0.5f, 140.0f, 0.0f, 70.0f)
                   .reverb ("Room", 12.0f, 0.6f, 30.0f)
                   .level (20.0f);

        lib << P ("Tuned Toms", "Drums & Perc", { "Cinematic", "Rock" },
                  "Big tuned toms with a tight skin and room; for tribal patterns on low strings.")
                   .synth (both, "Replace").set ("synOctave", -1.0f).set ("synOnsetSens", 80.0f)
                   .wave ("Sine", "Triangle", 30.0f).set ("synNoise", 10.0f).set ("synNoiseColour", -40.0f)
                   .filter ("LP 12", 600.0f, 10.0f, 2.0f).fenv (0.5f, 120.0f, 0.0f, 100.0f)
                   .env (0.5f, 450.0f, 0.0f, 200.0f)
                   .reverb ("Room", 22.0f, 1.2f, 55.0f)
                   .level (18.0f);

        lib << P ("Hand Drum", "Drums & Perc", { "Experimental", "Funk", "Ambient" },
                  "Warm, talking hand-drum tones with a skin formant; for rhythmic single notes.")
                   .synth (both, "Replace").set ("synOnsetSens", 80.0f).set ("synGlide", 30.0f)
                   .wave ("Sine", "Vowel Oo", 35.0f).set ("scFormantPos", 2.5f).set ("scFormantGain", 6.0f)
                   .filter ("LP 12", 1200.0f, 10.0f, 1.5f).fenv (0.5f, 150.0f, 0.0f, 100.0f)
                   .env (0.5f, 320.0f, 0.0f, 150.0f)
                   .reverb ("Room", 15.0f, 0.9f, 40.0f)
                   .level (20.0f);
    }

    //==============================================================================
    void addLofi (Library& lib)
    {
        lib << P ("Lo-fi Crunch", "Lo-fi & Texture", { "Lo-fi", "Hip-Hop & Trap" },
                  "8-bit, 4x downsampled grit with a dark top - instant sampler crunch for drums or keys.")
                   .set ("drive", 15.0f)
                   .set ("bits", 8.0f).set ("downsample", 4.0f)
                   .set ("lpf", 9000.0f)
                   .level (1.0f);

        lib << P ("Dusty Cassette", "Lo-fi & Texture", { "Lo-fi", "Hip-Hop & Trap", "Jazz" },
                  "Warbly, saturated cassette tone with a soft top and a wobbly echo; for chill guitar and keys.")
                   .amp ("Tube/Tape", 5.0f, 5.5f, 5.0f, 3.5f, 4.0f)
                   .mod ("Vibrato", 0.7f, 22.0f)
                   .eqBand (90.0f, 6500.0f)
                   .delay ("Tape", "1/4", 25.0f, 12.0f).set ("fxDelayWow", 60.0f)
                   .set ("bits", 12.0f)
                   .level (5.5f);

        lib << P ("Telephone Line", "Lo-fi & Texture", { "Lo-fi", "Indie", "Experimental" },
                  "Narrow, crunchy telephone band-pass; for vocals, intros and breakdowns.")
                   .set ("drive", 25.0f).choice ("shape", "Hard Clip")
                   .eqBand (450.0f, 3200.0f).set ("fxEqMid2Freq", 1500.0f).set ("fxEqMid2Gain", 4.0f)
                   .level (-3.0f);

        lib << P ("Lo-fi Keys", "Lo-fi & Texture", { "Lo-fi", "Hip-Hop & Trap", "Jazz" },
                  "Dusty electric piano with tape wobble and bit grit; turns guitar or MIDI chords into a beat-tape loop.")
                   .synth (both, "Replace").chord (15.0f)
                   .wave ("Sine", "Bell", 18.0f).set ("synWarmth", 30.0f)
                   .env (3.0f, 1800.0f, 25.0f, 400.0f)
                   .filter ("LP 12", 2500.0f, 5.0f)
                   .mod ("Vibrato", 0.6f, 25.0f)
                   .amp ("Tube/Tape", 5.0f)
                   .set ("bits", 11.0f).set ("downsample", 2.0f)
                   .reverb ("Room", 18.0f, 1.2f, 45.0f)
                   .level (8.5f);

        lib << P ("Arcade Crush", "Lo-fi & Texture", { "Experimental", "Lo-fi", "EDM" },
                  "Heavily bit-crushed, aliased square tones straight out of an old console; for riffs and leads.")
                   .synth (both, "Blend", -3.0f).set ("synInLevel", -10.0f).set ("synFollow", 75.0f)
                   .wave ("Square").set ("scPartials", 16.0f)
                   .env (1.0f, 250.0f, 70.0f, 80.0f)
                   .set ("bits", 6.0f).set ("downsample", 6.0f)
                   .set ("lpf", 8000.0f)
                   .level (14.0f);

        lib << P ("Underwater", "Lo-fi & Texture", { "Lo-fi", "Ambient", "Experimental" },
                  "Muffled, swirling sound from below the surface with a deep phaser; for any input.")
                   .set ("lpf", 650.0f)
                   .mod ("Phaser", 0.25f, 75.0f, 50.0f).set ("fxModFeedback", 50.0f)
                   .reverb ("Chamber", 35.0f, 3.0f, 70.0f).set ("fxRevDamping", 70.0f)
                   .level (10.5f);

        lib << P ("Bedroom Amp", "Lo-fi & Texture", { "Lo-fi", "Indie" },
                  "A small, boxy practice amp breaking up through a tiny speaker; for raw demo-style guitar.")
                   .amp ("Crunch", 5.0f, 4.0f, 6.0f, 5.0f, 4.0f)
                   .cab ("1x8 Lo-Fi", 40.0f, 20.0f)
                   .reverb ("Room", 12.0f, 0.7f, 30.0f)
                   .level (3.5f);

        lib << P ("Radio Ghost", "Lo-fi & Texture", { "Experimental", "Cinematic", "Lo-fi" },
                  "A distant AM-radio voice drifting through flanged echoes and reverb; for eerie intros on any input.")
                   .set ("drive", 20.0f)
                   .eqBand (500.0f, 3500.0f)
                   .mod ("Flanger", 0.15f, 60.0f, 40.0f).set ("fxModFeedback", 40.0f)
                   .delay ("Tape", "1/4D", 45.0f, 25.0f)
                   .reverb ("Hall", 30.0f, 4.0f, 80.0f)
                   .level (1.0f);

        lib << P ("Tape Haze", "Lo-fi & Texture", { "Ambient", "Lo-fi" },
                  "Notes smear into a warm cloud of saturated tape echoes; for slow ambient guitar.")
                   .delay ("Tape", "1/4", 70.0f, 38.0f).set ("fxDelaySat", 60.0f).set ("fxDelayWow", 55.0f).set ("fxDelayHighCut", 4000.0f)
                   .reverb ("Hall", 35.0f, 5.0f, 85.0f).set ("fxRevDamping", 60.0f)
                   .mod ("Chorus", 0.3f, 40.0f, 30.0f)
                   .level (0.0f);
    }

    //==============================================================================
    void addPhysics (Library& lib)
    {
        lib << P ("Distant Hall (Double Slit)", "Experimental & Physics", { "Ambient", "Cinematic" },
                  "The double-slit interference pattern heard from 200 m away: a far-off, combed hall; for any input.")
                   .on ("dsOn").choice ("dsSizing", "Fixed (metres)")
                   .set ("dsSeparation", 3.0f).set ("dsDistance", 200.0f)
                   .set ("dsMix", 60.0f)
                   .set ("lpf", 9000.0f)
                   .level (6.0f);

        lib << P ("Interference Choir", "Experimental & Physics", { "Experimental", "Ambient" },
                  "A vowel synth heard through pitch-following double slits: shifting, choral fringes; for held notes.")
                   .synth (both, "Replace").set ("synFollow", 45.0f)
                   .wave ("Vowel Ah", "Vowel Ee", 40.0f).unison (3, 10.0f, 80.0f)
                   .env (200.0f, 800.0f, 100.0f, 1200.0f)
                   .on ("dsOn").choice ("dsSizing", "Follow pitch").set ("dsMultiple", 6.0f).set ("dsFringes", 9.0f)
                   .choice ("dsEar", "Fringe-weighted").set ("dsMix", 70.0f)
                   .reverb ("Hall", 25.0f, 3.0f, 70.0f)
                   .level (10.5f);

        lib << P ("Fringe Comb", "Experimental & Physics", { "Experimental", "Indie" },
                  "Only the bright interference fringes pass, giving a hollow, metallic comb; for guitar and drums.")
                   .on ("dsOn").choice ("dsSizing", "Per frequency").set ("dsMultiple", 14.0f)
                   .choice ("dsEar", "Bright fringes only").set ("dsFringes", 12.0f).set ("dsMix", 80.0f)
                   .level (7.5f);

        lib << P ("Sine Fold Formula", "Experimental & Physics", { "Experimental", "EDM" },
                  "A custom formula folds the wave through a sine for buzzy, vowel-like overtones; dial A sets the folds.")
                   .formula ("sin(3*x*(1 + 4*a))")
                   .set ("eqA", 0.3f)
                   .set ("eqOut", -3.0f)
                   .level (-6.0f);

        lib << P ("Wavefolder Synth", "Experimental & Physics", { "EDM", "Experimental", "Synthwave" },
                  "A tracked sine synth through a West-Coast wavefolder: rich, rubbery, animated; for guitar lines or MIDI.")
                   .synth (both, "Replace")
                   .wave ("Sine", "Triangle", 30.0f)
                   .env (3.0f, 500.0f, 90.0f, 200.0f)
                   .equation ("Wavefold", 0.45f, 0.55f)
                   .set ("eqOut", -4.0f)
                   .reverb ("Plate", 15.0f, 1.6f, 50.0f)
                   .level (-2.0f);

        lib << P ("Harmonic Adder", "Experimental & Physics", { "Rock", "Experimental", "Pop" },
                  "Chebyshev polynomials add exact 2nd and 3rd harmonics: thicker, brighter, still clean; for any input.")
                   .equation ("Harmonic adder (Chebyshev)", 0.35f, 0.25f, 0.08f, 0.04f)
                   .level (12.5f);

        lib << P ("Phase Bender", "Experimental & Physics", { "Experimental", "EDM" },
                  "Phase distortion bends the waveform into hollow, resonant CZ-style tones; for picked notes.")
                   .equation ("Phase distortion", 0.6f, 0.34f)
                   .mod ("Phaser", 0.4f, 40.0f, 30.0f)
                   .level (-3.5f);

        lib << P ("Self-FM Chaos", "Experimental & Physics", { "Experimental" },
                  "Feedback FM snarls and squeals as you play harder; for noisy, unpredictable guitar.")
                   .equation ("Self-FM feedback", 0.3f, 0.35f).set ("eqMix", 60.0f)
                   .set ("hpf", 60.0f)
                   .delay ("Ping-pong", "1/8", 35.0f, 15.0f)
                   .level (-9.5f);

        lib << P ("Orbit Rotator", "Experimental & Physics", { "Ambient", "Experimental" },
                  "Rotates the stereo field and auto-pans it into a slow orbit around your head; for any stereo input.")
                   .equation ("Stereo rotation", 0.3f)
                   .mod ("Auto-pan", 0.2f, 70.0f)
                   .reverb ("Hall", 22.0f, 3.0f, 70.0f)
                   .level (5.5f);

        lib << P ("Quantum Steps", "Experimental & Physics", { "Experimental", "Lo-fi" },
                  "Quantised amplitude steps seen through a double slit: stepped, glitchy and spatial; for any input.")
                   .equation ("Quantize steps", 0.55f, 0.25f)
                   .on ("dsOn").choice ("dsSizing", "Per frequency").set ("dsMultiple", 5.0f).set ("dsMix", 50.0f)
                   .level (7.0f);

        lib << P ("Inharmonic Drift", "Experimental & Physics", { "Experimental", "Cinematic", "Ambient" },
                  "Stretched, bell-like partials drifting against each other with a slow wobble; for eerie sustained notes.")
                   .synth (both, "Replace").set ("synFollow", 50.0f)
                   .wave ("Saw", "Bell", 30.0f).set ("scStretch", 45.0f).set ("scComb", 25.0f)
                   .unison (2, 15.0f, 80.0f).set ("synLfoRate", 0.3f).set ("synLfoPitch", 15.0f)
                   .filter ("LP 12", 3000.0f, 10.0f)
                   .env (200.0f, 1000.0f, 100.0f, 1500.0f)
                   .reverb ("Hall", 32.0f, 4.0f, 80.0f)
                   .level (3.0f);

        lib << P ("Comb Resonator", "Experimental & Physics", { "Experimental", "Indie" },
                  "Your input plus a combed, metallic resonance that rings at your pitch; for plucked notes.")
                   .synth ("Input", "Blend", -6.0f)
                   .wave ("Bright Saw").set ("scComb", 75.0f).set ("scCombOffset", 12.0f).set ("scStretch", 10.0f)
                   .env (2.0f, 1200.0f, 20.0f, 600.0f)
                   .reverb ("Plate", 18.0f, 2.0f, 50.0f)
                   .level (4.5f);

        lib << P ("Spectral Tilt Odd", "Experimental & Physics", { "Experimental", "Synthwave" },
                  "Odd-only, upward-tilted spectrum: a hollow, glassy, almost clarinet-meets-laser tone; for leads.")
                   .synth (both, "Replace").set ("synGlide", 40.0f)
                   .wave ("Saw").set ("scOddEven", -90.0f).set ("scTilt", 2.0f).set ("scPartials", 40.0f)
                   .filter ("LP 12", 6000.0f, 20.0f)
                   .env (5.0f, 400.0f, 90.0f, 250.0f)
                   .delay ("Ping-pong", "1/8D", 35.0f, 18.0f)
                   .level (10.0f);
    }

    //==============================================================================
    // Chord tracking and per-note drive: every note of a strummed chord plays its own voice.
    // Added last so the host program numbers of the presets above stay where they were.
    void addChordKit (Library& lib)
    {
        lib << P ("Poly Drive Wall", "Guitar", { "Rock", "Metal", "Indie" },
                  "Every string distorted on its own: full major, 7th and add9 chords stay clear under heavy gain - "
                  "a sound one amp can't make. Strum open chords.")
                   .synth ("Input", "Replace").chord (35.0f)
                   .wave ("Bright Saw", "Square", 25.0f).unison (2, 6.0f, 60.0f)
                   .noteDrive ("Distortion", 70.0f, 45.0f)
                   .filter ("LP 12", 7000.0f, 5.0f)
                   .env (2.0f, 400.0f, 100.0f, 180.0f)
                   .cab ("4x12 Closed (V30)", 35.0f, 15.0f)
                   .reverb ("Room", 12.0f, 0.9f, 40.0f)
                   .level (7.5f);

        lib << P ("Velvet Fuzz Strum", "Guitar", { "Indie", "Rock", "Experimental" },
                  "Your clean strum with a per-note fuzz synth under it: thick and buzzy, yet every chord tone stays "
                  "in tune; for slow, ringing chords.")
                   .synth ("Input", "Blend", -6.0f).chord (0.0f, 4).set ("synInLevel", -3.0f)
                   .wave ("Soft Square", "Saw", 30.0f)
                   .noteDrive ("Fuzz", 55.0f, 35.0f)
                   .env (8.0f, 600.0f, 100.0f, 400.0f)
                   .mod ("Chorus", 0.5f, 35.0f, 30.0f)
                   .reverb ("Plate", 22.0f, 2.2f, 55.0f)
                   .level (8.0f);

        lib << P ("Strum Bloom Pad", "Pad", { "Ambient", "Pop", "Cinematic" },
                  "Strummed chords bloom into a wide, soft pad - every string its own detuned voice - with a "
                  "shimmering tail; for open chords and slow changes.")
                   .synth (both, "Replace").chord ().set ("synFollow", 35.0f)
                   .wave ("Soft Saw", "Hollow", 35.0f).unison (5, 16.0f, 90.0f)
                   .filter ("LP 12", 3000.0f, 8.0f)
                   .env (450.0f, 1000.0f, 100.0f, 1800.0f)
                   .reverb ("Shimmer", 38.0f, 5.5f, 85.0f).set ("fxRevShimmer", 30.0f)
                   .level (-3.0f);

        lib << P ("Poly Strum Synth", "Pluck & Arp", { "Synthwave", "Pop", "EDM" },
                  "Each strummed string becomes a bright analog pluck with ping-pong echoes: guitar chords turned into "
                  "a polysynth part.")
                   .synth (both, "Replace").chord (30.0f)
                   .wave ("Saw").unison (3, 12.0f, 80.0f)
                   .filter ("Ladder 24", 1100.0f, 25.0f, 3.0f).fenv (1.0f, 260.0f, 15.0f, 250.0f)
                   .env (1.0f, 900.0f, 30.0f, 350.0f)
                   .mod ("Chorus", 0.6f, 35.0f, 30.0f)
                   .delay ("Ping-pong", "1/8D", 30.0f, 20.0f)
                   .level (16.5f);

        lib << P ("Overdriven Chord Organ", "Keys & Organ", { "Rock", "Blues", "Funk" },
                  "A drawbar organ on every note of your chord, each pushed into its own tube overdrive: growling "
                  "but never muddy; for rock comping.")
                   .synth (both, "Replace").chord ().set ("synFollow", 45.0f)
                   .wave ("Organ (drawbars)")
                   .noteDrive ("Overdrive", 55.0f, 55.0f)
                   .env (4.0f, 300.0f, 100.0f, 80.0f)
                   .mod ("Vibrato", 6.0f, 30.0f)
                   .cab ("1x12 Open Back", 40.0f, 25.0f)
                   .reverb ("Room", 14.0f, 1.0f, 40.0f)
                   .level (3.5f);

        lib << P ("Chord Bells", "Bells & Mallets", { "Ambient", "Cinematic", "Indie" },
                  "Your guitar chord rings on, with sculpted bell overtones locked to every string (Overtones mode, "
                  "one voice per note); for slow arpeggios and open chords.")
                   .synth ("Input", "Overtones", -4.0f).chord ().set ("synFollow", 100.0f)
                   .wave ("Bell", "Sine", 20.0f)
                   .env (2.0f, 1500.0f, 60.0f, 1200.0f)
                   .reverb ("Hall", 30.0f, 3.5f, 75.0f)
                   .level (2.0f);
    }
}

std::vector<FactoryPreset> makeFactoryPresets()
{
    Library lib;
    addInit (lib);
    addGuitar (lib);
    addBass (lib);
    addKeys (lib);
    addLeads (lib);
    addSynthBass (lib);
    addPads (lib);
    addPlucks (lib);
    addStringsBrass (lib);
    addWinds (lib);
    addBells (lib);
    addVocal (lib);
    addDrums (lib);
    addLofi (lib);
    addPhysics (lib);
    addChordKit (lib);
    return lib.take();
}
