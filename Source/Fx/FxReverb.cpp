#include "FxInternal.h"

// Algorithmic reverb: pre-delay -> early reflections (room types) + 4 series allpass diffusers
// -> 16-line feedback delay network (Hadamard mixing, slowly modulated line lengths, Jot-style
// per-line absorption filters so highs decay faster exactly as set by Damping) -> low cut ->
// stereo width. Shimmer feeds an octave-up pitch shifted copy of the tail back into the network.

namespace fx
{
namespace
{
    const ReverbStage::TypeVoice revVoices[] = {
        // sizeMs diffMs diffusion erLevel modRate inputLp
        {  45.0f, 3.0f, 0.60f, 0.60f, 0.70f, 11000.0f },   // Room
        { 110.0f, 6.0f, 0.70f, 0.25f, 0.45f,  9000.0f },   // Hall
        {  65.0f, 4.5f, 0.75f, 0.00f, 0.90f, 16000.0f },   // Plate
        {  70.0f, 4.0f, 0.65f, 0.45f, 0.60f, 10000.0f },   // Chamber
        { 140.0f, 7.0f, 0.72f, 0.10f, 0.35f,  9000.0f },   // Shimmer / ambient
    };

    const float erTimes[8] = { 3.1f, 5.3f, 7.9f, 11.2f, 13.7f, 17.3f, 21.1f, 26.9f };
    const float diffRatios[4] = { 1.0f, 0.77f, 0.59f, 0.43f };

    // Line lengths as fractions of the longest: log-spaced 0.3..1 with a little irregularity, so no
    // two lines share a common period (that is what makes cheap reverbs ring)
    float lineRatio (int i) noexcept
    {
        return 0.3f * std::pow (1.0f / 0.3f, (float) i / (ReverbStage::numLines - 1)) * (1.0f + 0.031f * std::sin ((float) i * 2.7f));
    }

    inline void hadamard16 (float* x) noexcept
    {
        for (int h = 1; h < 16; h <<= 1)
            for (int i = 0; i < 16; i += 2 * h)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = x[j], b = x[j + h];
                    x[j] = a + b;
                    x[j + h] = a - b;
                }
        for (int i = 0; i < 16; ++i)
            x[i] *= 0.25f;
    }

    // output tap signs: two mutually orthogonal patterns for L and R
    constexpr float signL[16] = { 1, -1, 1, -1, 1, 1, -1, -1, 1, -1, -1, 1, -1, 1, 1, -1 };
    constexpr float signR[16] = { 1, 1, -1, -1, -1, 1, 1, -1, 1, 1, -1, -1, 1, -1, 1, -1 };
}

const ReverbStage::TypeVoice& ReverbStage::typeVoice (int t) noexcept
{
    return revVoices[juce::jlimit (0, 4, t)];
}

juce::StringArray ReverbStage::typeNames()
{
    return { "Room", "Hall", "Plate", "Chamber", "Shimmer" };
}

void ReverbStage::bind (juce::AudioProcessorValueTreeState& s)
{
    pType = raw (s, "fxRevType");
    pPreDelay = raw (s, "fxRevPreDelay");
    pSize = raw (s, "fxRevSize");
    pDecay = raw (s, "fxRevDecay");
    pDamping = raw (s, "fxRevDamping");
    pLowCut = raw (s, "fxRevLowCut");
    pMod = raw (s, "fxRevMod");
    pWidth = raw (s, "fxRevWidth");
    pMix = raw (s, "fxRevMix");
    pShimmer = raw (s, "fxRevShimmer");
}

void ReverbStage::prepare (double sampleRate, int, int)
{
    sr = sampleRate;
    for (auto& p : pre)
        p.prepare ((int) (0.30 * sr));
    for (auto& l : lines)
        l.prepare ((int) (0.17 * sr));
    for (auto& chs : diff)
        for (auto& d : chs)
            d.prepare ((int) (0.012 * sr));
    shimmerLine.prepare ((int) (0.12 * sr));

    const double ctl = sr / chunk;
    preSm.setTime (0.15f, ctl);
    sizeSm.setTime (0.4f, ctl);
    for (auto* s : { &decaySm, &dampSm, &modSm, &widthSm, &mixSm, &shimSm })
        s->setTime (0.05f, ctl);
    feedStep = (float) (1.0 / (0.02 * sr));
    for (int i = 0; i < 8; ++i)
        erGain[i] = 0.8f * std::exp (-0.25f * (float) i) * ((i & 1) ? -1.0f : 1.0f);
    reset();
}

void ReverbStage::reset() noexcept
{
    for (auto& p : pre) p.clear();
    for (auto& l : lines) l.clear();
    for (auto& chs : diff)
        for (auto& d : chs)
            d.clear();
    shimmerLine.clear();
    for (int i = 0; i < numLines; ++i)
    {
        lineState[i] = 0.0f;
        const float ph = (float) i * 2.39996f;   // golden-angle spread of LFO phases
        lfoC[i] = std::cos (ph);
        lfoS[i] = std::sin (ph);
    }
    outHp.reset();
    inLp.reset();
    shimHp.reset();
    shimLp.reset();
    preSm.snap (get (pPreDelay) * 0.001f * (float) sr);
    sizeSm.snap (get (pSize) * 0.01f);
    decaySm.snap (get (pDecay));
    dampSm.snap (get (pDamping) * 0.01f);
    modSm.snap (get (pMod) * 0.01f);
    widthSm.snap (get (pWidth) * 0.01f);
    mixSm.snap (get (pMix) * 0.01f);
    shimSm.snap (get (pShimmer) * 0.01f);
    shimmerPhase = 0.0f;
    shimmerFb = 0.0f;
    feed = 0.0f;
    silentSamples = 0;
    type = pendingType = juce::jlimit (0, 4, (int) get (pType));
    switchGain = 1.0f;
}

bool ReverbStage::process (float* const* ch, int numCh, int n, bool on) noexcept
{
    const float feedTarget = on ? 1.0f : 0.0f;
    const float lowHz = get (pLowCut);
    const bool lowOn = lowHz > 20.5f;
    if (lowOn)
        outHp.set (Svf::highpass, lowHz, 0.707f, 0.0f, sr);
    float wetPeak = 0.0f;
    const float shimWin = (float) (0.05 * sr);

    for (int off = 0; off < n; off += chunk)
    {
        const int len = juce::jmin (chunk, n - off);

        const int wanted = juce::jlimit (0, 4, (int) get (pType));
        if (wanted != type)
            pendingType = wanted;
        const float sg0 = switchGain;
        switchGain = pendingType != type ? juce::jmax (0.0f, switchGain - 0.1f) : juce::jmin (1.0f, switchGain + 0.1f);

        const auto& v = typeVoice (type);
        const float preD = preSm.step (get (pPreDelay) * 0.001f * (float) sr);
        const float size = sizeSm.step (get (pSize) * 0.01f);
        const float t60 = juce::jmax (0.1f, decaySm.step (get (pDecay)));
        const float damp = dampSm.step (get (pDamping) * 0.01f);
        const float modAmt = modSm.step (get (pMod) * 0.01f);
        const float width = widthSm.step (get (pWidth) * 0.01f);
        const float mix = mixSm.step (get (pMix) * 0.01f);
        const float shim = type == shimmer ? shimSm.step (get (pShimmer) * 0.01f) : 0.0f;

        // network geometry and absorption for this chunk
        const float scale = 0.35f + 0.65f * size;
        const float longest = v.sizeMs * scale * 0.001f * (float) sr;
        const float t60hf = t60 * std::pow (0.1f, damp);
        const float modDepth = modAmt * 0.0006f * (float) sr;
        for (int i = 0; i < numLines; ++i)
        {
            lineLen[i] = longest * lineRatio (i);
            const float L = lineLen[i];
            const float gdc = std::pow (10.0f, -3.0f * L / (t60 * (float) sr));
            const float gny = std::pow (10.0f, -3.0f * L / (t60hf * (float) sr));
            const float b = (gdc - gny) / (gdc + gny);
            dampB[i] = b;
            dampA[i] = gdc * (1.0f - b);
            const float w = juce::MathConstants<float>::twoPi * v.modRate * (0.6f + 0.8f * (float) i / (numLines - 1)) / (float) sr;
            rotC[i] = std::cos (w);
            rotS[i] = std::sin (w);
            // renormalise the quadrature oscillators so rounding never grows them
            const float m = 1.0f / std::sqrt (lfoC[i] * lfoC[i] + lfoS[i] * lfoS[i]);
            lfoC[i] *= m;
            lfoS[i] *= m;
        }
        const float diffMs = v.diffMs * (0.6f + 0.4f * size);
        for (int c = 0; c < maxCh; ++c)
            for (int k = 0; k < 4; ++k)
                diffLen[c][k] = juce::jlimit (1, diff[c][k].maxDelay(), (int) (diffMs * diffRatios[k] * (c == 0 ? 1.0f : 1.07f) * 0.001f * (float) sr));
        const float erScale = (v.sizeMs / 70.0f) * scale * 0.001f * (float) sr;
        for (int c = 0; c < maxCh; ++c)
            for (int k = 0; k < 8; ++k)
                erTap[c][k] = juce::jmin ((float) pre[c].maxDelay(),
                                          preD + erTimes[k] * erScale * (c == 0 ? 1.0f : 1.07f) + (c == 0 ? 0.0f : 0.4f * 0.001f * (float) sr));
        inLp.set (Svf::lowpass, v.inputLp, 0.707f, 0.0f, sr);
        if (type == shimmer)
        {
            shimHp.set (Svf::highpass, 300.0f, 0.707f, 0.0f, sr);
            shimLp.set (Svf::lowpass, 7000.0f, 0.707f, 0.0f, sr);
        }
        const float dryG = juce::jmin (1.0f, 2.0f * (1.0f - mix));
        const float wetG = juce::jmin (1.0f, 2.0f * mix);
        const float erLevel = v.erLevel;
        const float diffG = v.diffusion;

        for (int i = 0; i < len; ++i)
        {
            if (feed != feedTarget)
                feed = feed < feedTarget ? juce::jmin (feedTarget, feed + feedStep) : juce::jmax (feedTarget, feed - feedStep);
            const float sg = sg0 + (switchGain - sg0) * (float) (i + 1) / (float) len;

            const float xL = ch[0][off + i];
            const float xR = numCh > 1 ? ch[1][off + i] : xL;

            // pre-delay, early reflections
            float in[maxCh] = { xL * feed, xR * feed };
            if (type == shimmer)
            {
                in[0] += shimmerFb;
                in[1] += shimmerFb;
            }
            float er[maxCh] = { 0.0f, 0.0f };
            float diffused[maxCh];
            for (int c = 0; c < maxCh; ++c)
            {
                pre[c].push (in[c]);
                float s = pre[c].readLinear (preD);
                if (erLevel > 0.0f)
                    for (int k = 0; k < 8; ++k)
                        er[c] += erGain[k] * pre[c].tap ((int) erTap[c][k]);
                s = inLp.process (s, c);
                for (int k = 0; k < 4; ++k)
                {
                    auto& d = diff[c][k];
                    const float delayed = d.tap (diffLen[c][k] - 1);
                    const float vv = s + diffG * delayed;
                    s = delayed - diffG * vv;
                    d.push (vv);
                }
                diffused[c] = s;
            }

            // FDN
            float o[numLines], x[numLines];
            for (int k = 0; k < numLines; ++k)
            {
                const float c0 = lfoC[k], s0 = lfoS[k];
                lfoC[k] = c0 * rotC[k] - s0 * rotS[k];
                lfoS[k] = c0 * rotS[k] + s0 * rotC[k];
                o[k] = lines[k].read (lineLen[k] + modDepth * (1.0f + lfoS[k]));
                lineState[k] = dampA[k] * o[k] + dampB[k] * lineState[k];
                x[k] = lineState[k];
            }
            hadamard16 (x);
            for (int k = 0; k < numLines; ++k)
                lines[k].push (x[k] + diffused[k & 1] * ((k & 2) ? -0.5f : 0.5f));

            float wl = 0.0f, wr = 0.0f;
            for (int k = 0; k < numLines; ++k)
            {
                wl += signL[k] * o[k];
                wr += signR[k] * o[k];
            }
            wl = wl * 0.3f + er[0] * erLevel * 0.5f;
            wr = wr * 0.3f + er[1] * erLevel * 0.5f;

            if (lowOn)
            {
                wl = outHp.process (wl, 0);
                wr = outHp.process (wr, 1);
            }

            if (type == shimmer)
            {
                // octave up: two read heads sweeping a 50 ms window at double speed, Hann crossfaded
                shimmerLine.push (0.5f * (wl + wr));
                shimmerPhase += 1.0f / shimWin;
                if (shimmerPhase >= 1.0f)
                    shimmerPhase -= 1.0f;
                float shifted = 0.0f;
                for (int h = 0; h < 2; ++h)
                {
                    float p = shimmerPhase + 0.5f * (float) h;
                    if (p >= 1.0f)
                        p -= 1.0f;
                    const float wnd = std::sin (pi * p);
                    shifted += wnd * wnd * shimmerLine.readLinear (1.0f + shimWin * (1.0f - p));
                }
                shifted = shimLp.process (shimHp.process (shifted, 0), 0);
                shimmerFb = fastTanh (shifted * shim * 0.55f);
            }

            const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width;
            const float g = wetG * sg;
            const float oL = (mid + side) * g, oR = (mid - side) * g;
            wetPeak = std::max (wetPeak, std::max (std::abs (oL), std::abs (oR)));

            const float dg = 1.0f + feed * (dryG - 1.0f);
            if (numCh > 1)
            {
                ch[0][off + i] = xL * dg + oL;
                ch[1][off + i] = xR * dg + oR;
            }
            else
            {
                ch[0][off + i] = xL * dg + 0.5f * (oL + oR);
            }
        }

        if (switchGain <= 0.0f && pendingType != type)
            type = pendingType;
    }

    if (on || feed > 0.0f || wetPeak > 1.0e-5f)
        silentSamples = 0;
    else
        silentSamples += n;
    return silentSamples < (int) (0.2 * sr);
}
} // namespace fx
