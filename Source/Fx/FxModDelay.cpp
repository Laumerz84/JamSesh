#include "FxInternal.h"

// Modulation (delay-line effects, allpass phaser, amplitude effects) and the delay
// (digital with crossfaded time changes, tape with gliding time + wow/flutter + saturation,
// ping-pong). Both are written against raw channel pointers, mono or stereo.

namespace fx
{
//==============================================================================
// Mod
//==============================================================================
juce::StringArray ModStage::typeNames()
{
    return { "Chorus", "Ensemble", "Flanger", "Phaser", "Vibrato", "Tremolo", "Auto-pan" };
}

float ModStage::rateHz (float hz, bool sync, int div, double bpm) noexcept
{
    if (! sync)
        return hz;
    return (float) (juce::jmax (20.0, bpm) / 60.0 / syncBeats (div));
}

void ModStage::bind (juce::AudioProcessorValueTreeState& s)
{
    pType = raw (s, "fxModType");
    pRate = raw (s, "fxModRate");
    pSync = raw (s, "fxModSync");
    pDiv = raw (s, "fxModDiv");
    pDepth = raw (s, "fxModDepth");
    pFeedback = raw (s, "fxModFeedback");
    pMix = raw (s, "fxModMix");
    pWidth = raw (s, "fxModWidth");
    pShape = raw (s, "fxModShape");
    pTone = raw (s, "fxModTone");
}

void ModStage::prepare (double sampleRate, int, int)
{
    sr = sampleRate;
    for (auto& l : line)
        l.prepare ((int) (0.06 * sr));
    for (auto* s : { &depthSm, &fbSm, &mixSm, &widthSm, &toneSm })
        s->setTime (0.03f, sr / chunk);
    reset();
}

void ModStage::reset() noexcept
{
    for (auto& l : line)
        l.clear();
    for (auto& chs : apStage)
        for (auto& a : chs)
            a.reset();
    toneLp.reset();
    for (auto& f : fb)
        f = 0.0f;
    depthSm.snap (get (pDepth) * 0.01f);
    fbSm.snap (get (pFeedback) * 0.01f);
    mixSm.snap (get (pMix) * 0.01f);
    widthSm.snap (get (pWidth) * 0.01f);
    toneSm.snap (std::log (get (pTone)));
    type = pendingType = juce::jlimit (0, 6, (int) get (pType));
    switchGain = 1.0f;
}

float ModStage::lfo (double ph, int shape) const noexcept
{
    const double p = ph - std::floor (ph);
    switch (shape)
    {
        case 1:  return (float) (4.0 * std::abs (p - std::floor (p + 0.5)) - 1.0) * -1.0f;   // triangle
        case 2:  return fastTanh (5.0f * std::sin ((float) (p * juce::MathConstants<double>::twoPi))) / 0.99991f;   // rounded square
        default: return std::sin ((float) (p * juce::MathConstants<double>::twoPi));
    }
}

void ModStage::process (float* const* ch, int numCh, int n, const FxRack::Transport& t) noexcept
{
    const bool sync = get (pSync) > 0.5f;
    const int div = (int) get (pDiv);
    const float rate = rateHz (get (pRate), sync, div, t.bpm);
    if (sync && t.playing)
        phase = t.ppqPosition / syncBeats (div);   // lock the LFO to the song position
    const double inc = rate / sr;
    const int shape = (int) get (pShape);
    const float msToS = (float) sr * 0.001f;

    for (int off = 0; off < n; off += chunk)
    {
        const int len = juce::jmin (chunk, n - off);

        const int wanted = juce::jlimit (0, 6, (int) get (pType));
        if (wanted != type)
            pendingType = wanted;
        const float sg0 = switchGain;
        if (pendingType != type)
            switchGain = juce::jmax (0.0f, switchGain - 0.125f);
        else
            switchGain = juce::jmin (1.0f, switchGain + 0.125f);

        const float depth = depthSm.step (get (pDepth) * 0.01f);
        const float fbk = fbSm.step (get (pFeedback) * 0.01f);
        const float mix = mixSm.step (get (pMix) * 0.01f);
        const float width = widthSm.step (get (pWidth) * 0.01f);
        const float toneHz = std::exp (toneSm.step (std::log (get (pTone))));
        const bool toneOn = toneHz < 19900.0f;
        // dry + detuned copy average out to ~-3 dB; give back ~2 dB at 50% mix (chorus, ensemble)
        const float blendComp = 1.0f + 0.26f * std::sin (pi * mix);
        if (toneOn)
            toneLp.set (Svf::lowpass, toneHz, 0.707f, 0.0f, sr);

        if (type == phaser)
        {
            // sweep +-2.5 octaves around 900 Hz; coefficients refreshed per 32-sample chunk
            for (int c = 0; c < numCh; ++c)
            {
                const float l = lfo (phase + inc * len * 0.5 + c * 0.25 * width, shape);
                const float f = 900.0f * std::exp2 (l * depth * 2.5f);
                for (int k = 0; k < 4; ++k)
                    apStage[c][k].set (Svf::allpass, f * (1.0f + 0.35f * (float) k), 0.5f, 0.0f, sr);
            }
        }

        for (int i = 0; i < len; ++i)
        {
            const float sg = sg0 + (switchGain - sg0) * (float) (i + 1) / (float) len;
            const double ph = phase;
            phase += inc;

            if (type == autopan)
            {
                const float l = lfo (ph, shape) * depth;
                const float a = (l + 1.0f) * pi * 0.25f;
                const float gl = 1.41421356f * std::cos (a), gr = 1.41421356f * std::sin (a);
                ch[0][off + i] *= 1.0f + sg * (gl - 1.0f);
                if (numCh > 1)
                    ch[1][off + i] *= 1.0f + sg * (gr - 1.0f);
                continue;
            }

            for (int c = 0; c < numCh; ++c)
            {
                const float x = ch[c][off + i];
                float eff = x;
                switch (type)
                {
                    case chorus:
                    {
                        const float l = lfo (ph + c * 0.25 * width, shape);
                        const float d = (7.0f + 4.0f * depth * (0.5f + 0.5f * l)) * msToS;
                        float wet = line[c].read (d);
                        line[c].push (x + 0.5f * fbk * wet);
                        if (toneOn) wet = toneLp.process (wet, c);
                        eff = (x + mix * (wet - x)) * blendComp;
                        break;
                    }
                    case ensemble:
                    {
                        float wet = 0.0f;
                        for (int k = 0; k < 3; ++k)
                        {
                            const double o = k / 3.0 + c * width / 6.0;
                            const float slow = lfo (ph + o, 0);
                            const float fast = lfo (ph * 7.3 + o, 0);
                            const float d = (10.0f + depth * (3.0f * (0.5f + 0.5f * slow) + 0.4f * fast)) * msToS;
                            wet += line[c].read (d);
                        }
                        line[c].push (x);
                        wet *= 0.47f;
                        if (toneOn) wet = toneLp.process (wet, c);
                        eff = (x + mix * (wet - x)) * blendComp;
                        break;
                    }
                    case flanger:
                    {
                        const float l = lfo (ph + c * 0.25 * width, shape);
                        const float d = (0.3f + 4.7f * depth * (0.5f + 0.5f * l)) * msToS;
                        float wet = line[c].read (d);
                        line[c].push (fastTanh (x + fbk * wet));
                        if (toneOn) wet = toneLp.process (wet, c);
                        eff = x + mix * (wet - x);
                        break;
                    }
                    case phaser:
                    {
                        float s = x + fbk * 0.9f * fb[c];
                        for (int k = 0; k < 4; ++k)
                            s = apStage[c][k].process (s, c);
                        fb[c] = s;
                        eff = x + mix * (s - x);
                        break;
                    }
                    case vibrato:
                    {
                        const float l = lfo (ph + c * 0.25 * width, shape);
                        const float d = (3.0f + 2.5f * depth * l) * msToS;
                        float wet = line[c].read (d);
                        line[c].push (x);
                        if (toneOn) wet = toneLp.process (wet, c);
                        eff = wet;
                        break;
                    }
                    case tremolo:
                    {
                        const float l = lfo (ph + c * 0.5 * width, shape);
                        eff = x * (1.0f - depth * (0.5f - 0.5f * l));
                        break;
                    }
                    default: break;
                }
                ch[c][off + i] = x + sg * (eff - x);
            }
        }

        if (switchGain <= 0.0f && pendingType != type)
        {
            type = pendingType;
            for (auto& l : line)
                l.clear();
            for (auto& f : fb)
                f = 0.0f;
        }
    }
    phase -= std::floor (phase);
}

//==============================================================================
// Delay
//==============================================================================
juce::StringArray DelayStage::typeNames()
{
    return { "Digital", "Tape", "Ping-pong" };
}

double DelayStage::timeMs (float ms, bool sync, int div, double bpm) noexcept
{
    if (! sync)
        return ms;
    return syncBeats (div) * 60000.0 / juce::jmax (20.0, bpm);
}

void DelayStage::bind (juce::AudioProcessorValueTreeState& s)
{
    pType = raw (s, "fxDelayType");
    pSync = raw (s, "fxDelaySync");
    pDiv = raw (s, "fxDelayDiv");
    pTime = raw (s, "fxDelayTime");
    pFeedback = raw (s, "fxDelayFeedback");
    pMix = raw (s, "fxDelayMix");
    pLowCut = raw (s, "fxDelayLowCut");
    pHighCut = raw (s, "fxDelayHighCut");
    pDuck = raw (s, "fxDelayDuck");
    pWidth = raw (s, "fxDelayWidth");
    pWow = raw (s, "fxDelayWow");
    pSat = raw (s, "fxDelaySat");
}

void DelayStage::prepare (double sampleRate, int, int)
{
    sr = sampleRate;
    for (auto& l : line)
        l.prepare ((int) (4.1 * sr));   // 1/1 dotted at 90 BPM; longer settings are clamped to 4 s
    for (auto* s : { &fbSm, &mixSm, &widthSm, &duckSm, &wowSm, &satSm })
        s->setTime (0.03f, sr / chunk);
    duckAtt = onePoleCoef (0.002f, sr);
    duckRel = onePoleCoef (0.250f, sr);
    feedStep = (float) (1.0 / (0.02 * sr));
    reset();
}

void DelayStage::reset() noexcept
{
    for (auto& l : line)
        l.clear();
    loopHp.reset();
    loopLp.reset();
    const float target = (float) juce::jlimit (1.0, 4.0 * sr,
        timeMs (get (pTime), get (pSync) > 0.5f, (int) get (pDiv), 120.0) * 0.001 * sr);
    curDelay = oldDelay = tapeDelay = target;
    xfade = 1.0f;
    wowPhase = flutterPhase = 0.0;
    duckEnv = 0.0f;
    feed = 0.0f;
    silentSamples = 0;
    fbSm.snap (get (pFeedback) * 0.01f);
    mixSm.snap (get (pMix) * 0.01f);
    widthSm.snap (get (pWidth) * 0.01f);
    duckSm.snap (get (pDuck) * 0.01f);
    wowSm.snap (get (pWow) * 0.01f);
    satSm.snap (get (pSat) * 0.01f);
    type = pendingType = juce::jlimit (0, 2, (int) get (pType));
    switchGain = 1.0f;
}

bool DelayStage::process (float* const* ch, int numCh, int n, const FxRack::Transport& t, bool on) noexcept
{
    const double ms = timeMs (get (pTime), get (pSync) > 0.5f, (int) get (pDiv), t.bpm);
    const float target = (float) juce::jlimit (1.0, 4.0 * sr, ms * 0.001 * sr);
    const float lowHz = get (pLowCut), highHz = get (pHighCut);
    const bool lowOn = lowHz > 20.5f;
    const float lpHz = type == tape ? juce::jmin (highHz, 6500.0f) : highHz;
    const bool highOn = lpHz < 19900.0f;
    if (lowOn)  loopHp.set (Svf::highpass, lowHz, 0.707f, 0.0f, sr);
    if (highOn) loopLp.set (Svf::lowpass, lpHz, 0.707f, 0.0f, sr);
    const float tapeGlide = onePoleCoef (0.25f, sr);
    const double wowInc = 0.55 / sr, flutInc = 7.0 / sr;
    const float msToS = (float) sr * 0.001f;
    const float feedTarget = on ? 1.0f : 0.0f;
    float wetPeak = 0.0f;

    for (int off = 0; off < n; off += chunk)
    {
        const int len = juce::jmin (chunk, n - off);

        const int wanted = juce::jlimit (0, 2, (int) get (pType));
        if (wanted != type)
            pendingType = wanted;
        const float sg0 = switchGain;
        switchGain = pendingType != type ? juce::jmax (0.0f, switchGain - 0.125f) : juce::jmin (1.0f, switchGain + 0.125f);

        const float fbk = fbSm.step (get (pFeedback) * 0.01f);
        const float mix = mixSm.step (get (pMix) * 0.01f);
        const float width = widthSm.step (get (pWidth) * 0.01f);
        const float duck = duckSm.step (get (pDuck) * 0.01f);
        const float wow = wowSm.step (get (pWow) * 0.01f);
        const float sat = satSm.step (get (pSat) * 0.01f);
        const float dryG = juce::jmin (1.0f, 2.0f * (1.0f - mix));
        const float wetG = juce::jmin (1.0f, 2.0f * mix);
        const float drive = 1.0f + 4.0f * sat;
        const float satNorm = 2.0f / drive;   // unity small-signal gain, repeats compress as they get hot

        for (int i = 0; i < len; ++i)
        {
            if (feed != feedTarget)
                feed = feed < feedTarget ? juce::jmin (feedTarget, feed + feedStep) : juce::jmax (feedTarget, feed - feedStep);
            const float sg = sg0 + (switchGain - sg0) * (float) (i + 1) / (float) len;

            float inL = ch[0][off + i];
            float inR = numCh > 1 ? ch[1][off + i] : inL;

            // read positions
            float yL, yR;
            if (type == tape)
            {
                tapeDelay += tapeGlide * (target - tapeDelay);
                wowPhase += wowInc;
                flutterPhase += flutInc;
                rng = rng * 1664525u + 1013904223u;
                noiseState += 0.002f * (((float) (rng >> 9) * (1.0f / 8388608.0f) - 1.0f) - noiseState);
                const float mod = wow * (1.6f * std::sin ((float) (wowPhase * juce::MathConstants<double>::twoPi))
                                       + 0.22f * std::sin ((float) (flutterPhase * juce::MathConstants<double>::twoPi))
                                       + 6.0f * noiseState) * msToS;
                const float d = juce::jmax (1.0f, tapeDelay + mod);
                yL = line[0].read (d);
                yR = line[1].read (d);
            }
            else
            {
                if (xfade >= 1.0f && std::abs (target - curDelay) > 0.5f)
                {
                    oldDelay = curDelay;
                    curDelay = target;
                    xfade = 0.0f;
                    xfadeStep = (float) (1.0 / (0.05 * sr));   // 50 ms crossfade between old and new time
                }
                if (xfade < 1.0f)
                {
                    xfade = juce::jmin (1.0f, xfade + xfadeStep);
                    const float a = std::sin (xfade * pi * 0.5f), b = std::cos (xfade * pi * 0.5f);
                    yL = a * line[0].read (curDelay) + b * line[0].read (oldDelay);
                    yR = a * line[1].read (curDelay) + b * line[1].read (oldDelay);
                }
                else
                {
                    yL = line[0].read (curDelay);
                    yR = line[1].read (curDelay);
                }
            }

            // feedback path: filters (+ tape saturation)
            auto shapeLoop = [&] (float y, int c)
            {
                if (lowOn)  y = loopHp.process (y, c);
                if (highOn) y = loopLp.process (y, c);
                if (type == tape)
                    y = fastTanh (drive * y * 0.5f) * satNorm;
                else
                    y = juce::jlimit (-4.0f, 4.0f, y);
                return y;
            };
            const float fL = shapeLoop (yL, 0) * fbk;
            const float fR = shapeLoop (yR, 1) * fbk;

            if (type == pingpong)
            {
                const float mono = 0.5f * (inL + inR) * feed;
                line[0].push (mono + fR);
                line[1].push (fL);
            }
            else
            {
                line[0].push (inL * feed + fL);
                line[1].push (inR * feed + fR);
            }

            // ducking: repeats drop while the input is loud
            const float lvl = std::max (std::abs (inL), std::abs (inR)) * feed;
            duckEnv += (lvl > duckEnv ? duckAtt : duckRel) * (lvl - duckEnv);
            float wg = wetG * sg;
            if (duck > 0.0f)
                wg *= dbToGain (-duck * 24.0f * juce::jlimit (0.0f, 1.0f, (gainToDb (duckEnv) + 46.0f) / 40.0f));

            // width: mid/side on the repeats
            const float mid = 0.5f * (yL + yR), side = 0.5f * (yL - yR) * width;
            const float wL = (mid + side) * wg, wR = (mid - side) * wg;
            wetPeak = std::max (wetPeak, std::max (std::abs (wL), std::abs (wR)));

            const float dg = 1.0f + feed * (dryG - 1.0f);
            if (numCh > 1)
            {
                ch[0][off + i] = inL * dg + wL;
                ch[1][off + i] = inR * dg + wR;
            }
            else
            {
                ch[0][off + i] = inL * dg + 0.5f * (wL + wR);
            }
        }

        if (switchGain <= 0.0f && pendingType != type)
        {
            type = pendingType;
            loopHp.reset();
            loopLp.reset();
            curDelay = oldDelay = tapeDelay = target;
            xfade = 1.0f;
        }
    }
    wowPhase -= std::floor (wowPhase);
    flutterPhase -= std::floor (flutterPhase);

    // switched off: keep running until the repeats are inaudible (-100 dB for 100 ms)
    if (on || feed > 0.0f || wetPeak > 1.0e-5f)
        silentSamples = 0;
    else
        silentSamples += n;
    return silentSamples < (int) (0.1 * sr);
}
} // namespace fx
