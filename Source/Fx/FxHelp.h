#pragma once

#include "../UI/Help.h"

// Inspector text for every FX rack parameter (and the panels' non-parameter displays).
// Mappings describe the DSP exactly as implemented in the Fx*.cpp files.

namespace fx
{
inline HelpList helpList()
{
    HelpList h;
    auto add = [&h] (const char* id, const char* title, const char* summary, const char* mapping, const char* tips = "")
    {
        h.push_back ({ id, title, summary, mapping, tips });
    };

    // ---------------- module switches ----------------
    const char* switchMap = "Off -> the module is bypassed and costs (almost) no CPU. Switching crossfades over 20 ms, so it never clicks.";
    add ("fxAmpOn", "Amp on/off", "Switches the amp simulator in or out of the chain.", switchMap,
         "In the plugin the amp's small 4x-oversampling latency (0.1 ms) stays even when off, so the timing never jumps; "
         "in the Jam Sesh app it is only there while the amp is on.");
    add ("fxCabOn", "Cab on/off", "Switches the speaker cabinet simulation in or out.", switchMap,
         "Almost every distorted guitar sound needs a cab after the amp - without it, distortion sounds fizzy.");
    add ("fxEqOn", "EQ on/off", "Switches the 6-band EQ in or out.", switchMap);
    add ("fxCompOn", "Comp on/off", "Switches the compressor in or out.", switchMap);
    add ("fxModOn", "Mod on/off", "Switches the modulation effect in or out.", switchMap);
    add ("fxDelayOn", "Delay on/off", "Switches the delay in or out.",
         "Off -> no new sound enters the delay (20 ms fade), but repeats already in it ring out naturally; "
         "the module then sleeps once they fall below -100 dB.");
    add ("fxRevOn", "Reverb on/off", "Switches the reverb in or out.",
         "Off -> no new sound enters the reverb (20 ms fade), but the tail rings out naturally; the module then sleeps once it falls below -100 dB.");
    add ("fxLimOn", "Limiter on/off", "Switches the final brickwall limiter in or out.", switchMap,
         "In the plugin the 0.75 ms look-ahead latency is always reported to the host, so switching never shifts timing; "
         "in the Jam Sesh app it is only there while the limiter is on.");

    // ---------------- Amp ----------------
    add ("fxAmpModel", "Amp model", "The amp character: how many gain stages, where it clips and how it is voiced.",
         "Clean -> 1 stage, drive -6..+18 dB, mild scoop at 500 Hz. Crunch -> 2 stages, drive 0..+30 dB, 650 Hz mids. "
         "Lead -> mid-hump (+7 dB at 720 Hz) into 3 stages, drive +10..+40 dB. High Gain -> 3 stages, drive +15..+48 dB, "
         "tight 110 Hz low cut, -4 dB scoop at 600 Hz. Fuzz -> 2 hard asymmetric stages, drive +15..+50 dB, dark. "
         "Bass -> 2 stages, everything below 220 Hz bypasses the clipping and is added back clean. "
         "Tube/Tape -> 1 gentle stage, flat and warm, for synths and mixes. Changing model dips the amp for ~10 ms (no click).",
         "Follow the amp with the Cab for guitar. Tube/Tape at low gain is a nice warmth/glue saturator.");
    add ("fxAmpGain", "Gain", "How hard the preamp is driven.",
         "0..10 -> drive in dB linearly between the model's min and max (e.g. Crunch: 0 -> 0 dB, 5 -> +15 dB, 10 -> +30 dB). "
         "The output is partly auto-levelled: output trim = model trim - k x drive dB, with k = 0.8 (Clean) .. 0.1 (High Gain), so turning up gain adds mostly distortion, not volume.",
         "Below 3 most models clean up; play dynamics then control the breakup.");
    add ("fxAmpBass", "Bass", "Tone stack bass control (between preamp and power amp).",
         "0..10 -> low shelf at the model's bass frequency (80-110 Hz): gain = (knob - 5) x 2.4 dB, i.e. -12..+12 dB, 5 = flat.");
    add ("fxAmpMid", "Mid", "Tone stack mid control.",
         "0..10 -> bell (Q 0.8) at the model's mid frequency (500-900 Hz). Gain = model scoop + (knob - 5) x 3 dB below 5 "
         "(down to -15 dB) or + (knob - 5) x 1.6 dB above 5 (up to +8 dB) - more cut than boost, like a passive stack.",
         "Low mids = classic scooped metal; high mids = cutting lead that sits in a band mix.");
    add ("fxAmpTreble", "Treble", "Tone stack treble control.",
         "0..10 -> high shelf at the model's treble frequency (2.2-3 kHz): (knob - 5) x 2.4 dB, i.e. -12..+12 dB.");
    add ("fxAmpPresence", "Presence", "Upper-treble sparkle after the power amp.",
         "0..10 -> high shelf at 4 kHz: (knob - 5) x 1.6 dB, i.e. -8..+8 dB.");
    add ("fxAmpLevel", "Level", "Output volume of the amp.", "-24..+12 dB of gain after the amp, on top of the model's automatic trim.");
    add ("fxAmpMix", "Amp mix", "Blend of clean (dry) signal and amp signal.",
         "0% -> only the dry signal, 100% -> only the amp. Linear blend; the dry path is delayed by the amp's latency so there is no comb filtering.",
         "40-70% keeps the attack of a bass or synth while adding grit.");
    add ("fxAmpGate", "Noise gate", "Built-in noise gate before the amp, for high-gain hiss and hum.",
         "Off at -100 dB. Otherwise the gate opens (0.7 ms) when the input peak exceeds the threshold, stays open 45 ms, "
         "and closes (70 ms) after the level drops 6 dB below the threshold.",
         "Set it just above the noise floor you hear between notes (often -60..-45 dB).");
    add ("fxAmpBright", "Bright", "Bright switch: adds sparkle before the gain stage.",
         "On -> high shelf at 1.8 kHz before the drive: +6 dB at Gain 0, fading to +2.4 dB at Gain 10 (like a bright cap).");

    // ---------------- Cab ----------------
    add ("fxCabModel", "Cabinet", "Which speaker cabinet is simulated.",
         "1x12 Open Back -> loose lows (open back cancels below ~110 Hz), sweet 2.3 kHz peak. 2x12 Combo -> balanced, chimey 2.7 kHz. "
         "4x12 Closed (V30) -> tight low thump around 95 Hz, aggressive 1.4 + 2.5 kHz peaks, notch at 4.2 kHz. "
         "4x10 Bass -> extended lows to ~50 Hz, punchy 800 Hz. 1x8 Lo-Fi -> small, nasal radio-like box (no lows below ~250 Hz). "
         "Off (DI) -> no speaker, only the low/high cut. Each cab is an impulse response designed from these curves (minimum phase), "
         "rebuilt off the audio thread and crossfaded in when you change something.");
    add ("fxCabMic", "Mic position", "Where the close microphone points at the speaker.",
         "0% -> on-axis at the dust cap (brightest, strongest presence peak). 100% -> off-axis at the cone edge: "
         "extra 4th-order roll-off moves from 14 kHz down to 3.5 kHz (14k x 0.25^pos), peaks above 1 kHz shrink by up to 60%, +2 dB body at 250 Hz.",
         "20-40% is the classic 'just off the cap' sound.");
    add ("fxCabRoom", "Room mic", "Blend of a second microphone ~1.5 m away in a small room.",
         "0% -> close mic only. Room level r: close mic x (1 - 0.7 r) + room mic x 0.85 r (room mic is energy-matched to the close mic). "
         "The room mic hears the cab off-axis with +3 dB more lows, ~4.5 ms later, plus 10 early reflections and a 0.3 s diffuse tail - different on left and right, so it adds width.",
         "10-25% adds air and depth without sounding like reverb.");
    add ("fxCabLevel", "Cab level", "Output volume of the cab.",
         "-12..+12 dB after the cab. Each cab is normalised so its average response between 150 Hz and 4 kHz is 0 dB.");
    add ("fxCabLowCut", "Cab low cut", "Removes rumble and boom below this frequency.",
         "12 dB/oct high-pass at the set frequency (20 Hz = Off), built into the impulse response.");
    add ("fxCabHighCut", "Cab high cut", "Removes fizz above this frequency.",
         "12 dB/oct low-pass at the set frequency (20 kHz = Off), built into the impulse response.");
    add ("fx.cabCurve", "Cab response", "The designed frequency response of the close mic on the selected cab.",
         "x: 20 Hz..20 kHz (log), y: -30..+12 dB. Mic position, low cut and high cut update the curve; the room mic is not drawn.");

    // ---------------- EQ ----------------
    add ("fxEqLowCut", "Low cut", "High-pass filter: removes everything below.",
         "24 dB/oct Butterworth high-pass at the set frequency. 20 Hz = Off.");
    add ("fxEqLowFreq", "Low shelf freq", "Corner frequency of the low shelf.", "30..500 Hz; the shelf is half-way (in dB) at this frequency (Q 0.71).");
    add ("fxEqLowGain", "Low shelf gain", "Boost or cut of everything below the low shelf frequency.", "-15..+15 dB.");
    add ("fxEqMid1Freq", "Mid 1 freq", "Centre of the first bell band.", "80 Hz..5 kHz.");
    add ("fxEqMid1Gain", "Mid 1 gain", "Boost or cut at Mid 1.", "-15..+15 dB at the centre frequency.");
    add ("fxEqMid1Q", "Mid 1 Q", "Width of Mid 1.", "Q 0.3 (about 3 octaves wide) .. 8 (a narrow notch/peak). Q 1 is about 1.4 octaves.");
    add ("fxEqMid2Freq", "Mid 2 freq", "Centre of the second bell band.", "300 Hz..15 kHz.");
    add ("fxEqMid2Gain", "Mid 2 gain", "Boost or cut at Mid 2.", "-15..+15 dB at the centre frequency.");
    add ("fxEqMid2Q", "Mid 2 Q", "Width of Mid 2.", "Q 0.3 (wide) .. 8 (narrow).");
    add ("fxEqHighFreq", "High shelf freq", "Corner frequency of the high shelf.", "1.5..16 kHz (Q 0.71).");
    add ("fxEqHighGain", "High shelf gain", "Boost or cut of everything above the high shelf frequency.", "-15..+15 dB.");
    add ("fxEqHighCut", "High cut", "Low-pass filter: removes everything above.", "24 dB/oct Butterworth low-pass. 20 kHz = Off.");
    add ("fxEqOutput", "EQ output", "Output gain of the EQ.", "-12..+12 dB.", "Use it to level-match after big boosts, so you judge tone, not loudness.");
    add ("fx.eqCurve", "EQ curve", "Live response of all EQ bands together.",
         "x: 20 Hz..20 kHz (log), y: -18..+18 dB. Drag a dot: left/right = frequency, up/down = gain (cuts: frequency only). "
         "Mouse wheel over a mid dot changes its Q. Double-click a dot to reset its gain.");

    // ---------------- Comp ----------------
    add ("fxCompThresh", "Threshold", "Level above which compression starts.", "-60..0 dBFS, measured on the louder channel (stereo-linked peak detector).");
    add ("fxCompRatio", "Ratio", "How strongly levels above the threshold are reduced.",
         "1:1 (none) .. 20:1 (limiting). Above the threshold, every R dB more input gives 1 dB more output.");
    add ("fxCompAttack", "Attack", "How fast the gain comes down when the level rises.", "0.1..100 ms time constant (63% of the way).",
         "Slower attack (10-30 ms) lets transients through: punchier drums and picking.");
    add ("fxCompRelease", "Release", "How fast the gain recovers after the level falls.", "10..1000 ms time constant (63% of the way).");
    add ("fxCompKnee", "Knee", "How gently compression starts around the threshold.",
         "0 dB = hard knee. With knee W, compression fades in over threshold - W/2 .. threshold + W/2 (quadratic curve).");
    add ("fxCompMakeup", "Makeup", "Gain added after compression.",
         "-12..+24 dB, added on top of the automatic makeup when Auto is on.");
    add ("fxCompAuto", "Auto makeup", "Automatically restores the level lost to compression.",
         "On -> adds 0.3 x |threshold| x (1 - 1/ratio) dB (about the average reduction on typical material). Threshold -18, 4:1 -> +4.1 dB.");
    add ("fxCompMix", "Comp mix", "Parallel ('New York') compression.",
         "0% -> dry only, 100% -> compressed only; linear blend of dry and compressed (incl. makeup).",
         "Heavy settings (8:1, fast attack) at 30-50% mix add density while keeping natural transients.");
    add ("fxCompScHpf", "Sidechain HPF", "Stops the bass from pumping the compressor.",
         "12 dB/oct high-pass on the detector signal only (the audio is not filtered). 20 Hz = Off.");
    add ("fx.compMeter", "Comp display", "Transfer curve and gain reduction.",
         "Curve: input dB (x, -60..0) -> output dB (y) with the current threshold/ratio/knee; the dot is the current input peak. "
         "Bar: current gain reduction, 0..-24 dB.");

    // ---------------- Mod ----------------
    add ("fxModType", "Mod type", "Which modulation effect.",
         "Chorus -> one voice per side, delay 7..11 ms swept by the LFO. Ensemble -> 3 voices per side, 10..13 ms slow sweep + 7.3x faster shimmer (string-machine). "
         "Flanger -> 0.3..5 ms sweep with feedback. Phaser -> 4 two-pole allpass stages swept +-2.5 octaves around 900 Hz. "
         "Vibrato -> pitch wobble only (100% wet, 3 +- 2.5 ms). Tremolo -> volume wobble. Auto-pan -> equal-power left/right movement.");
    add ("fxModRate", "Rate", "LFO speed in Hz (when Sync is off).", "0.02..10 Hz, one full LFO cycle per 1/rate seconds.");
    add ("fxModSync", "Sync", "Locks the LFO to the host tempo.",
         "On -> rate = (BPM / 60) / beats of the chosen note value; while the host plays, the LFO phase also follows the song position.");
    add ("fxModDiv", "Sync rate", "Note value of one LFO cycle when synced.",
         "1/4 = one quarter note, T = triplet (x 2/3), D = dotted (x 1.5), 1/1 = one 4/4 bar, 2 and 4 bars for slow sweeps.");
    add ("fxModDepth", "Depth", "How far the LFO moves the effect.",
         "Chorus: 0..4 ms of delay sweep. Ensemble: 0..3 ms + 0.4 ms. Flanger: 0..4.7 ms. Phaser: 0..+-2.5 octaves. Vibrato: 0..+-2.5 ms. "
         "Tremolo: volume dips to (1 - depth). Auto-pan: 0..100% of a full left/right swing.");
    add ("fxModFeedback", "Feedback", "Resonance of flanger and phaser (and a little for chorus).",
         "-95..+95%: delayed/allpassed signal fed back into the effect (Phaser x 0.9, Chorus x 0.5). Negative values invert it (hollower tone). No effect on Ensemble, Vibrato, Tremolo, Auto-pan.");
    add ("fxModMix", "Mix", "Blend of dry and effect signal.",
         "Linear: 0% dry .. 100% wet; 50% gives the deepest chorus/flanger/phaser notches. Chorus and Ensemble add up to +2 dB (x (1 + 0.26 sin(pi x mix))) to make up for the dry/wet averaging. Ignored by Vibrato (always 100%), Tremolo and Auto-pan.");
    add ("fxModWidth", "Width", "Stereo spread: LFO phase difference between left and right.",
         "0% = both sides move together (mono). 100% = 90 deg offset (chorus, flanger, phaser, vibrato), 60 deg between ensemble voices, 180 deg for tremolo (ping-pong tremolo). Auto-pan is always opposite.");
    add ("fxModShape", "LFO shape", "Waveform of the LFO.",
         "Sine = smooth, Triangle = constant-speed sweep, Square = rounded on/off (choppy tremolo, two-pitch vibrato). Ensemble always uses sine.");
    add ("fxModTone", "Tone", "Darkens the modulated (wet) voices.",
         "12 dB/oct low-pass on the wet signal of chorus, ensemble, flanger and vibrato. 20 kHz = Off.");

    // ---------------- Delay ----------------
    add ("fxDelayType", "Delay type", "Character of the delay.",
         "Digital -> clean repeats; time changes crossfade over 50 ms (no pitch bend). "
         "Tape -> time changes glide (pitch bends like a real tape machine, 0.25 s), wow/flutter, saturation in the loop and repeats darkened to at most 6.5 kHz. "
         "Ping-pong -> the input (summed to mono) bounces left -> right -> left.");
    add ("fxDelaySync", "Sync", "Delay time follows the host tempo.", "On -> time = beats of the note value x 60000 / BPM ms. Off -> the Time knob in ms.");
    add ("fxDelayDiv", "Sync time", "Note value of the delay when synced.",
         "1/32 .. 1/1 bar, T = triplet (x 2/3), D = dotted (x 1.5). At 120 BPM: 1/4 = 500 ms, 1/8D = 375 ms. Capped at 4 s.");
    add ("fxDelayTime", "Time", "Delay time in milliseconds (Sync off).", "1..2000 ms between the input and the first repeat.");
    add ("fxDelayFeedback", "Feedback", "How much of each repeat comes back again.",
         "0..100% loop gain: at 50% each repeat is 6 dB quieter (before the loop filters). 100% repeats forever (Tape saturates, Digital is clamped at +-4).");
    add ("fxDelayMix", "Mix", "Level of the repeats against the dry signal.",
         "Dry gain = min(1, 2 x (1 - mix)), wet gain = min(1, 2 x mix): the dry stays at full level up to 50%, 25% -> repeats at -6 dB.");
    add ("fxDelayLowCut", "Low cut", "Thins out the repeats.", "12 dB/oct high-pass inside the feedback loop, so each repeat gets thinner. 20 Hz = Off.");
    add ("fxDelayHighCut", "High cut", "Darkens the repeats.", "12 dB/oct low-pass inside the feedback loop, so each repeat gets darker. 20 kHz = Off.");
    add ("fxDelayDuck", "Ducking", "Repeats get quieter while you play and swell up in the gaps.",
         "Input peak follower (2 ms attack, 250 ms release): repeats are reduced by duck x 24 dB x clamp((level dB + 46) / 40), "
         "i.e. no ducking below -46 dBFS, full amount at -6 dBFS and above.");
    add ("fxDelayWidth", "Width", "Stereo spread of the repeats.",
         "Mid/side on the repeats: side x width. 0% = mono repeats, 100% = full stereo (ping-pong fully left/right).");
    add ("fxDelayWow", "Wow/flutter", "Tape speed wobble (Tape type only).",
         "0..100% -> up to +-1.6 ms slow wow (0.55 Hz) + +-0.22 ms flutter (7 Hz) + random drift added to the delay time.");
    add ("fxDelaySat", "Saturation", "Tape saturation of the repeats (Tape type only).",
         "Soft clipper in the loop with drive 1 + 4 x sat (unity for quiet repeats); loud repeats compress and grit up, so high feedback stays musical.");
    add ("fx.delayInfo", "Delay time readout", "The delay time actually used.", "Shows ms and the note value at the host tempo.");

    // ---------------- Reverb ----------------
    add ("fxRevType", "Reverb type", "The space.",
         "Room -> small (45 ms network), strong early reflections. Hall -> large (110 ms), smooth, slow modulation. "
         "Plate -> dense, bright, no early reflections. Chamber -> mid-size with reflections. "
         "Shimmer -> huge ambient space whose tail is pitched up an octave and fed back (see Shimmer).");
    add ("fxRevPreDelay", "Pre-delay", "Gap between the dry sound and the reverb.", "0..250 ms delay before the reverb input.",
         "20-40 ms keeps vocals and leads clear in front of the reverb.");
    add ("fxRevSize", "Size", "How big the space is.",
         "Scales all delay lines by 0.35 + 0.65 x size (0% = 35% of the type's size, 100% = full) and the diffusion and reflection times with it. Changes glide over ~0.4 s.");
    add ("fxRevDecay", "Decay", "Reverb time.", "0.2..20 s RT60: time for the tail (low/mid frequencies) to fall by 60 dB.");
    add ("fxRevDamping", "Damping", "How much faster the highs die away than the lows.",
         "High-frequency RT60 = Decay x 0.1^damping (0% = same as lows, 40% -> 0.4 x, 100% -> 0.1 x), exact per delay line (Jot absorption filters).");
    add ("fxRevLowCut", "Low cut", "Keeps the reverb from muddying the low end.", "12 dB/oct high-pass on the reverb output only. 20 Hz = Off.");
    add ("fxRevMod", "Modulation", "Slow movement inside the reverb that removes metallic ringing.",
         "0..100% -> each of the 16 lines wobbles by up to 1.2 ms at its own slow rate (type rate x 0.6..1.4).",
         "Lower it for pianos and sustained tones if you hear chorusing.");
    add ("fxRevWidth", "Width", "Stereo spread of the reverb.", "Mid/side: side x width. 0% = mono reverb, 100% = full stereo.");
    add ("fxRevMix", "Mix", "Reverb level against the dry signal.",
         "Dry gain = min(1, 2 x (1 - mix)), wet gain = min(1, 2 x mix): dry stays full up to 50%; 25% -> reverb at -6 dB. 100% = reverb only (for sends).");
    add ("fxRevShimmer", "Shimmer", "Octave-up feedback (Shimmer type only).",
         "0..100% -> amount of the tail that is pitched up one octave (two-head 50 ms shifter, band-limited 300 Hz..7 kHz) and fed back in; soft-limited so it cannot run away.");

    // ---------------- Limiter ----------------
    add ("fxLimInput", "Input", "Drive into the limiter for loudness.", "0..+24 dB of gain before limiting.",
         "Watch the GR meter: more than ~6 dB of constant reduction starts to flatten the sound.");
    add ("fxLimCeiling", "Ceiling", "Maximum output peak.",
         "-12..0 dBFS. Look-ahead (0.75 ms) brings the gain down smoothly before each peak, then a hard clip at the ceiling catches rounding errors. Sample peaks, not true peaks: leave -0.3 dB or lower.");
    add ("fxLimRelease", "Release", "How fast the gain recovers after a peak.", "10..1000 ms time constant.",
         "Short = louder but can distort bass; 100-200 ms is transparent.");
    add ("fx.limMeter", "Limiter meter", "Gain reduction of the limiter.", "Bar: 0..-24 dB of gain reduction right now.");

    return h;
}
} // namespace fx
