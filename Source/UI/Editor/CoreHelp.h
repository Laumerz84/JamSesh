#pragma once

#include "../Help.h"

// Help for the original (pre-module) parameters: wave modification, scope display, double slit
// and the main view switch. Every `mapping` describes exactly what the DSP / drawing code does.
namespace corehelp
{
    inline HelpList entries()
    {
        HelpList h;
        auto add = [&h] (const char* id, const char* title, const char* summary, const char* mapping, const char* tips = "")
        {
            h.push_back ({ id, title, summary, mapping, tips });
        };

        // ---------------- main view ----------------
        add ("mainView", "Main view",
             "Chooses what the large display area shows.",
             "2D Scope: the classic oscilloscope (time across, amplitude up) with spectrum and double-slit graphic.\n"
             "3D Views: the four 3D wave views (helix, phase space, stereo 3D, waterfall), drawn on the GPU.\n"
             "Orbital: the sound mapped onto a quantum bound state and drawn as a probability cloud.\n"
             "Wave Sculptor: the synth's wave, its overtone bars and the A/B morph, edited directly with the mouse.\n"
             "Only the visible view does any work, so hidden views cost no CPU/GPU.",
             "Switching the view also opens its controls in the right column (the VIEW chip at the end of the chain "
             "strip opens them again later). Only the Wave Sculptor changes the sound - it edits the synth's wave.");

        // ---------------- wave modification (signal chain order) ----------------
        add ("inGain", "Input gain",
             "Boosts or cuts the signal before any processing.",
             "y = x * 10^(dB/20), range -24..+24 dB, smoothed over 20 ms. First stage of the chain; the gate, shaper "
             "and filters all see the gained signal. The 'Input' scope trace is taken BEFORE this gain.",
             "Raise it to drive the shaper harder; lower it to tame hot interface inputs.");
        add ("gate", "Noise gate",
             "Silences the signal while it is below a threshold - removes hiss and room noise from live input.",
             "Envelope = peak of max(|L|, |R|) with 50 ms release (stereo-linked). Gate gain moves towards 1 with a 1 ms "
             "attack when envelope > threshold, towards 0 with an 80 ms release otherwise. Applied right after input gain. "
             "-100 dB = Off (gate bypassed).",
             "Set it just above the noise floor you see on the scope when nothing is playing.");
        add ("dc", "DC offset",
             "Shifts the whole waveform up or down.",
             "y = x + offset (-1..+1, full scale = 1), added after the gate and before phase invert and the shaper. An offset "
             "makes the shaper treat positive and negative halves differently (adds even harmonics).",
             "Combine with Drive to get asymmetric, tube-like distortion. Large offsets reduce headroom.");
        add ("invert", "Phase invert",
             "Flips the waveform upside down.",
             "y = -(x + DC offset). Applied before the shaper, so with an asymmetric shape it changes the tone; on its own it "
             "only changes polarity (inaudible except against the dry signal).",
             "With Dry/Wet at 50% an inverted copy cancels the dry signal - a quick null test.");
        add ("drive", "Drive",
             "How hard the signal is pushed into the waveshaper selected by Shape.",
             "drive d = 0..1 (0..100%). Shaper gain k = 1 + 30 * d^2. Output = x + (shape(x, k) - x) * min(1, 10 * d), so the "
             "shaper fades in over the first 10% of the knob (no level jump). 0% = shaper bypassed.",
             "Watch 'Both (compare)' on the scope to see the dim input vs the bright shaped output.");
        add ("shape", "Shape",
             "The waveshaping curve used by Drive.",
             "Soft (tanh): y = tanh(k x) / tanh(k)  - smooth saturation, odd harmonics.\n"
             "Hard Clip: y = clamp(k x, -1, 1)  - flat tops, bright and buzzy.\n"
             "Wavefold: t = 0.25 k x + 0.25, y = 4 |t - round(t)| - 1  - peaks fold back down, rich metallic overtones.\n"
             "Sine: y = sin(pi/2 * k x)  - wraps the wave through a sine, very harmonic-dense at high drive.\n"
             "Asymmetric: x >= 0: tanh(k x)/tanh(k); x < 0: tanh(0.4 k x)/tanh(k)  - even harmonics, tube-like.",
             "k comes from the Drive knob (k = 1 + 30 d^2).");
        add ("bits", "Bit depth",
             "Reduces amplitude resolution (bitcrusher).",
             "steps = 2^(bits - 1); y = round(x * steps) / steps. Applied after the shaper. 24 bits = Off. "
             "1 bit = 2 levels (square-like), 4 bits = 16 levels.",
             "Low bit depths add quantisation noise that follows the signal - visible as stair steps on the scope.");
        add ("downsample", "Downsample",
             "Holds each sample for N samples (sample-rate reduction).",
             "Sample-and-hold per channel: every N-th sample is captured and repeated N times, giving an effective sample "
             "rate of host rate / N (with aliasing, deliberately). 1 = Off. Applied after the bitcrusher.",
             "Zoom the time window down to a few ms to see the steps.");
        add ("hpf", "High-pass",
             "Removes low frequencies below the cutoff.",
             "Topology-preserving state-variable high-pass, 12 dB/octave, Q = 0.707 (Butterworth), per channel, after "
             "downsampling. 20 Hz = Off (filter bypassed).",
             "Use 80-150 Hz to clean rumble from mic input.");
        add ("lpf", "Low-pass",
             "Removes high frequencies above the cutoff.",
             "State-variable low-pass, 12 dB/octave, Q = 0.707, after the high-pass. Cutoff is limited to 0.45 x sample rate. "
             "20 kHz = Off (filter bypassed).",
             "Tames the harsh aliasing from the bitcrusher, downsampler and hard clipping.");
        add ("mix", "Dry/Wet",
             "Blend between the untouched input and the fully processed signal.",
             "out = dry + (wet - dry) * mix. dry = the raw input (before input gain); wet = everything after the low-pass, "
             "the Equation stage and the Double Slit. 0% = dry only, 100% = wet only. Smoothed over 20 ms.",
             "Parallel processing: keep the dry attack and add a little heavily driven wet.");
        add ("outGain", "Output gain",
             "Final level after the dry/wet mix.",
             "y = mix output * 10^(dB/20), -24..+24 dB, smoothed over 20 ms. The 'Output' scope trace is taken after this.",
             "Use it to level-match the processed sound against bypass.");

        // ---------------- scope display (picture only) ----------------
        add ("time", "Time window",
             "How much time the 2D scope shows across its width.",
             "Window = 1..500 ms across 10 grid divisions (ms/div = window / 10). Windows wider than 2 samples per pixel are "
             "drawn as min/max per pixel column so no peaks are skipped. Display only.",
             "One period of a 100 Hz tone is 10 ms - set ~20 ms to see two cycles.");
        add ("zoom", "Vertical zoom",
             "Scales the waveform vertically.",
             "Pixel offset = sample * (scope height * 0.45) * zoom, so at x1 full scale (+-1.0) spans 90% of the height. "
             "Also scales the XY view and the trigger-level line. Display only.",
             "Raise it to inspect quiet noise from external inputs.");
        add ("trigMode", "Trigger",
             "Keeps a repeating wave still on screen.",
             "Rising Edge: searches back from the newest audio for the latest sample where signal crosses the trigger level "
             "going up; Falling Edge: going down; Free Run: no search, the newest audio is shown. The trigger point is placed "
             "10% into the window. The searched signal is the channel shown (Left for L+R, Right, Mid, or Side) of the input "
             "when Show = Input, otherwise of the output.",
             "If the status reads NO TRIG, the level is above or below the whole wave - drag it back onto the wave.");
        add ("trigLevel", "Trigger level",
             "Signal value the trigger looks for (-1..+1).",
             "Compared directly against sample values (not scaled by zoom). Shown as the dashed amber line. Drag up/down on the "
             "scope to set it, double-click to reset to 0.",
             "");
        add ("view", "Channel view",
             "Which channel combination the 2D scope draws.",
             "Left + Right: both channels overlaid (green = L, blue = R).\nLeft / Right: one channel.\n"
             "Mid (L+R): (L + R) / 2 - what a mono speaker plays.\nSide (L-R): (L - R) / 2 - the stereo difference.\n"
             "XY / Lissajous: x = L, y = R for the newest min(window, 8192) samples; mono = diagonal line, out of phase = "
             "other diagonal, wide stereo = cloud.",
             "Display only.");
        add ("source", "Show signal",
             "Whether the scope, readouts and spectrum show the input, the output or both.",
             "Input (raw): taken before any processing (before input gain).\nOutput (modified): after everything, including "
             "output gain.\nBoth (compare): input drawn dim (amber/pink) under the output (green/blue). Peak/RMS/frequency "
             "readouts and the spectrum use the mid (L+R)/2 of the output unless Input is selected.",
             "Both is the easiest way to see what each modification does.");
        add ("persist", "Persistence",
             "Phosphor-style trail of previous frames.",
             "Each frame (60 per second) the previous trace image is multiplied by persist (0..95%) before the new trace is "
             "drawn, so a trace fades to 1% after log(0.01)/log(p) frames (e.g. 35% -> ~4 frames). 0% = no trail.",
             "High persistence makes noise look like a glowing band and shows trigger jitter.");
        add ("freeze", "Freeze",
             "Stops capturing new audio so you can study the current picture.",
             "While on, the copied audio is not refreshed; view, zoom, time window and trigger changes still re-render the "
             "frozen buffer. Readouts and spectrum also hold.",
             "");
        add ("spectrum", "Spectrum",
             "Shows a frequency analyser strip under the 2D scope.",
             "2048-sample Hann-windowed FFT of the mid signal (L+R)/2, magnitude in dB = 20 log10(|X| * 4/N) so a full-scale "
             "sine reads 0 dB, peak-hold falling 1.2 dB per frame. Log frequency axis 20 Hz .. min(20 kHz, sample rate / 2), "
             "vertical -100..0 dB.",
             "Display only.");

        // ---------------- double slit ----------------
        add ("dsOn", "Double slit",
             "Plays the sound as if heard through a double slit by a very large ear.",
             "A plane wave hits a barrier with two slits (separation d, width a, each modelled as 7 point sources). The ear "
             "is a line at distance L covering n bright fringes. Every ear point receives every slit point's copy delayed by "
             "(path - shortest path) / 343 m/s - the shortest path is heard instantly - with amplitude sqrt(r_min / r) "
             "(cylindrical spreading). All copies form an impulse response (max 2^17 samples) that is convolved with the "
             "wet signal. The response is normalised so its average gain over 50 Hz-15 kHz is 1 (peaks capped at +6 dB) "
             "and is rebuilt on a background thread when a setting changes.",
             "Small setups sound like comb filtering; large distances and many fringes become a smeared, coloured tail.");
        add ("dsSizing", "Slit sizing",
             "How the slit separation relates to the sound's wavelength.",
             "Per frequency: the spectrum is split into 24 log-spaced bands (30 Hz-16 kHz, raised-cosine crossfades); each band "
             "uses d = Multiple x (343 / band frequency), so every frequency sees the same fringe pattern.\n"
             "Follow pitch: d = Multiple x (343 / f0), f0 detected from the input by zero crossings (8192 samples); rebuilt "
             "when f0 moves by more than 3%.\n"
             "Fixed (metres): d = Separation, the same for all frequencies (physically realistic: low notes diffract widely, "
             "high notes form tight fringes). With a non-Physical ear model it is computed in 24 bands so the fringe weighting "
             "uses each band's wavelength.",
             "Per frequency matches the 'slit size = multiple of the wavelength' idea best.");
        add ("dsMultiple", "Slit multiple",
             "Slit separation in wavelengths (Per frequency / Follow pitch).",
             "d = Multiple x wavelength (1..50). Fringe angles follow sin(theta_m) = m / Multiple, so a larger multiple packs "
             "the fringes closer together and narrows the ear for the same fringe count. Ignored (greyed) in Fixed mode.",
             "");
        add ("dsSeparation", "Slit separation",
             "Slit separation in metres (Fixed mode only).",
             "d = 0.05..20 m. The ear width is set from the fringe spacing at 440 Hz: sin(theta) = n x (343/440) / (2 d). "
             "Ignored (greyed) in Per frequency / Follow pitch.",
             "");
        add ("dsWidth", "Slit width",
             "Width of each slit as a fraction of the separation.",
             "a = width x d (0.01..0.9). Each slit is 7 point sources spread across a, which produces the single-slit "
             "diffraction envelope (bright fringes fade at wide angles) automatically.",
             "Wide slits dim the outer fringes; very narrow slits approach two ideal point sources.");
        add ("dsFringes", "Fringes",
             "How many bright fringes the ear spans.",
             "Ear half-angle: sin(theta) = min(0.98, n / (2 x d/wavelength)); ear half-width Y = L x tan(theta). More fringes = "
             "wider ear = longer arrival-time spread.",
             "Physically, summing pressure over many fringes cancels much of the pattern; try the Bright-only ear for a stronger effect.");
        add ("dsDistance", "Screen distance",
             "Distance from the slits to the ear (L).",
             "1..500 m. Scales the ear width (Y = L tan(theta)) and therefore all delays: spread = (longest path - shortest path) "
             "/ 343 m/s. E.g. L = 50 m, 5 fringes, multiple 8 -> ~4.7 ms.",
             "The double-slit graphic shows the resulting ear width and spread.");
        add ("dsEar", "Ear model",
             "How the large ear adds up the waves arriving across it.",
             "Physical: every ear point counts equally (coherent pressure sum).\n"
             "Bright fringes only: ear points count only where the two-slit intensity cos^2(pi x delta / wavelength) > 0.5 "
             "(delta = path difference between the slit centres).\n"
             "Fringe-weighted: each ear point is weighted by cos^2(pi x delta / wavelength).",
             "");
        add ("dsMix", "Slit mix",
             "Blend between the signal before and after the double slit.",
             "out = pre + (slit - pre) x mix, smoothed over 20 ms; 'pre' is the wet signal after the Equation stage.",
             "");

        return h;
    }
}
