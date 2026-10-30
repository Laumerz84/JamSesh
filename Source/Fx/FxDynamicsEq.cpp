#include "FxInternal.h"

// EQ (SVF bands, coefficients refreshed every 32 samples from smoothed controls), compressor
// (feed-forward, log-domain, stereo-linked) and the look-ahead brickwall limiter.

namespace fx
{
//==============================================================================
// EQ
//==============================================================================
namespace
{
    const char* const eqIds[13] = { "fxEqLowCut", "fxEqLowFreq", "fxEqLowGain", "fxEqMid1Freq", "fxEqMid1Gain", "fxEqMid1Q",
                                    "fxEqMid2Freq", "fxEqMid2Gain", "fxEqMid2Q", "fxEqHighFreq", "fxEqHighGain", "fxEqHighCut",
                                    "fxEqOutput" };
    constexpr bool eqIsFreq[13] = { true, true, false, true, false, false, true, false, false, true, false, true, false };

    EqStage::Settings fromValues (const float* v)
    {
        EqStage::Settings s;
        s.lowCut = v[0];  s.lowFreq = v[1];  s.lowGain = v[2];
        s.m1Freq = v[3];  s.m1Gain = v[4];   s.m1Q = v[5];
        s.m2Freq = v[6];  s.m2Gain = v[7];   s.m2Q = v[8];
        s.highFreq = v[9]; s.highGain = v[10]; s.highCut = v[11]; s.out = v[12];
        return s;
    }

    constexpr float butterQ[2] = { 0.5411961f, 1.3065630f };   // 4th-order Butterworth as two SVF sections
}

EqStage::Settings EqStage::read (juce::AudioProcessorValueTreeState& state)
{
    float v[13];
    for (int i = 0; i < 13; ++i)
        v[i] = get (raw (state, eqIds[i]));
    return fromValues (v);
}

double EqStage::responseDb (const Settings& s, double f, double rate) noexcept
{
    double mag = 1.0;
    if (lowCutOn (s.lowCut))
        for (auto q : butterQ)
            mag *= Svf::Coefs::make (Svf::highpass, s.lowCut, q, 0.0f, rate).magnitude (f, rate);
    mag *= Svf::Coefs::make (Svf::lowShelf, s.lowFreq, 0.707f, s.lowGain, rate).magnitude (f, rate);
    mag *= Svf::Coefs::make (Svf::bell, s.m1Freq, s.m1Q, s.m1Gain, rate).magnitude (f, rate);
    mag *= Svf::Coefs::make (Svf::bell, s.m2Freq, s.m2Q, s.m2Gain, rate).magnitude (f, rate);
    mag *= Svf::Coefs::make (Svf::highShelf, s.highFreq, 0.707f, s.highGain, rate).magnitude (f, rate);
    if (highCutOn (s.highCut))
        for (auto q : butterQ)
            mag *= Svf::Coefs::make (Svf::lowpass, s.highCut, q, 0.0f, rate).magnitude (f, rate);
    return 20.0 * std::log10 (juce::jmax (1.0e-9, mag)) + s.out;
}

void EqStage::bind (juce::AudioProcessorValueTreeState& state)
{
    for (int i = 0; i < 13; ++i)
        p[(size_t) i] = raw (state, eqIds[i]);
}

void EqStage::prepare (double sampleRate, int, int)
{
    sr = sampleRate;
    for (auto& s : sm)
        s.setTime (0.025f, sr / chunk);
    reset();
}

void EqStage::reset() noexcept
{
    for (int i = 0; i < 13; ++i)
    {
        const float v = get (p[(size_t) i]);
        sm[(size_t) i].snap (eqIsFreq[i] ? std::log (v) : v);
    }
    for (auto* f : { &lc[0], &lc[1], &ls, &b1, &b2, &hs, &hc[0], &hc[1] })
        f->reset();
    outGain = dbToGain (sm[12].value);
}

void EqStage::process (float* const* ch, int numCh, int n) noexcept
{
    for (int off = 0; off < n; off += chunk)
    {
        const int len = juce::jmin (chunk, n - off);
        float v[13];
        for (int i = 0; i < 13; ++i)
        {
            const float target = get (p[(size_t) i]);
            const float s = sm[(size_t) i].step (eqIsFreq[i] ? std::log (target) : target);
            v[i] = eqIsFreq[i] ? std::exp (s) : s;
        }
        const auto st = fromValues (v);
        const bool lowOn = lowCutOn (st.lowCut), highOn = highCutOn (st.highCut);
        if (lowOn)
            for (int k = 0; k < 2; ++k)
                lc[k].set (Svf::highpass, st.lowCut, butterQ[k], 0.0f, sr);
        ls.set (Svf::lowShelf, st.lowFreq, 0.707f, st.lowGain, sr);
        b1.set (Svf::bell, st.m1Freq, st.m1Q, st.m1Gain, sr);
        b2.set (Svf::bell, st.m2Freq, st.m2Q, st.m2Gain, sr);
        hs.set (Svf::highShelf, st.highFreq, 0.707f, st.highGain, sr);
        if (highOn)
            for (int k = 0; k < 2; ++k)
                hc[k].set (Svf::lowpass, st.highCut, butterQ[k], 0.0f, sr);

        const float g0 = outGain;
        outGain = dbToGain (st.out);
        for (int c = 0; c < numCh; ++c)
        {
            float* x = ch[c] + off;
            for (int i = 0; i < len; ++i)
            {
                float s = x[i];
                if (lowOn)
                    s = lc[1].process (lc[0].process (s, c), c);
                s = ls.process (s, c);
                s = b1.process (s, c);
                s = b2.process (s, c);
                s = hs.process (s, c);
                if (highOn)
                    s = hc[1].process (hc[0].process (s, c), c);
                x[i] = s * (g0 + (outGain - g0) * (float) (i + 1) / (float) len);
            }
        }
        if (! lowOn)  { lc[0].reset(); lc[1].reset(); }
        if (! highOn) { hc[0].reset(); hc[1].reset(); }
    }
}

//==============================================================================
// Compressor
//==============================================================================
float CompStage::curveDb (float in, float t, float r, float w) noexcept
{
    const float over = in - t;
    if (2.0f * over < -w)
        return in;
    if (w > 0.0f && 2.0f * std::abs (over) <= w)
    {
        const float k = over + 0.5f * w;
        return in + (1.0f / r - 1.0f) * k * k / (2.0f * w);
    }
    return t + over / r;
}

void CompStage::bind (juce::AudioProcessorValueTreeState& s)
{
    pThresh = raw (s, "fxCompThresh");
    pRatio = raw (s, "fxCompRatio");
    pAttack = raw (s, "fxCompAttack");
    pRelease = raw (s, "fxCompRelease");
    pKnee = raw (s, "fxCompKnee");
    pMakeup = raw (s, "fxCompMakeup");
    pAuto = raw (s, "fxCompAuto");
    pScHpf = raw (s, "fxCompScHpf");
}

void CompStage::prepare (double sampleRate, int, int)
{
    sr = sampleRate;
    makeupSm.setTime (0.03f, sr / chunk);
    reset();
}

void CompStage::reset() noexcept
{
    env = 0.0f;
    scHp.reset();
    const float t = get (pThresh), r = get (pRatio);
    makeupSm.snap (get (pMakeup) + (get (pAuto) > 0.5f ? autoMakeupDb (t, r) : 0.0f));
    makeupGain = dbToGain (makeupSm.value);
    grDb.store (0.0f);
}

void CompStage::process (float* const* ch, int numCh, int n) noexcept
{
    const float t = get (pThresh), r = juce::jmax (1.0f, get (pRatio)), w = get (pKnee);
    const float att = onePoleCoef (get (pAttack) * 0.001f, sr);
    const float rel = onePoleCoef (get (pRelease) * 0.001f, sr);
    const float scHz = get (pScHpf);
    const bool scOn = scHz > 20.5f;
    if (scOn)
        scHp.set (Svf::highpass, scHz, 0.707f, 0.0f, sr);
    const float makeupTarget = get (pMakeup) + (get (pAuto) > 0.5f ? autoMakeupDb (t, r) : 0.0f);

    float minGr = 0.0f, maxIn = 0.0f;
    for (int off = 0; off < n; off += chunk)
    {
        const int len = juce::jmin (chunk, n - off);
        const float m0 = makeupGain;
        makeupGain = dbToGain (makeupSm.step (makeupTarget));
        for (int i = 0; i < len; ++i)
        {
            float lvl = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                const float x = ch[c][off + i];
                lvl = std::max (lvl, std::abs (scOn ? scHp.process (x, c) : x));
            }
            maxIn = std::max (maxIn, lvl);
            const float inLvl = gainToDb (lvl);
            const float target = curveDb (inLvl, t, r, w) - inLvl;
            env += (target < env ? att : rel) * (target - env);
            minGr = std::min (minGr, env);
            const float g = dbToGain (env) * (m0 + (makeupGain - m0) * (float) (i + 1) / (float) len);
            for (int c = 0; c < numCh; ++c)
                ch[c][off + i] *= g;
        }
    }
    grDb.store (minGr, std::memory_order_relaxed);
    inDb.store (gainToDb (maxIn), std::memory_order_relaxed);
}

//==============================================================================
// Limiter: required gain -> running minimum over the look-ahead window -> box average of the
// same length (so the gain is already down when the peak arrives) -> release.
//==============================================================================
void LimiterStage::bind (juce::AudioProcessorValueTreeState& s)
{
    pInput = raw (s, "fxLimInput");
    pCeiling = raw (s, "fxLimCeiling");
    pRelease = raw (s, "fxLimRelease");
}

void LimiterStage::prepare (double sampleRate, int, int)
{
    sr = sampleRate;
    hold = juce::jmax (2, (int) std::round (0.0015 * sr));   // 1.5 ms look-ahead
    holdBuf.assign ((size_t) hold, 1.0f);
    dq.assign ((size_t) hold + 1, 0);
    boxBuf.assign ((size_t) hold, 1.0f);
    audioDelay.prepare (hold - 1);
    inSm.setTime (0.02f, sr / chunk);
    ceilSm.setTime (0.02f, sr / chunk);
    reset();
}

void LimiterStage::reset() noexcept
{
    std::fill (holdBuf.begin(), holdBuf.end(), 1.0f);
    std::fill (boxBuf.begin(), boxBuf.end(), 1.0f);
    boxSum = (double) hold;
    dqHead = dqTail = 0;
    counter = 0;
    idx = 0;
    env = 1.0f;
    audioDelay.clear();
    inSm.snap (get (pInput));
    ceilSm.snap (get (pCeiling));
    grDb.store (0.0f);
}

void LimiterStage::process (float* const* ch, int numCh, int n) noexcept
{
    const float rel = onePoleCoef (get (pRelease) * 0.001f, sr);
    const int dqSize = hold + 1;
    float minEnv = 1.0f;

    for (int off = 0; off < n; off += chunk)
    {
        const int len = juce::jmin (chunk, n - off);
        const float in0 = dbToGain (inSm.value), c0 = dbToGain (ceilSm.value);
        const float in1 = dbToGain (inSm.step (get (pInput))), c1 = dbToGain (ceilSm.step (get (pCeiling)));

        for (int i = 0; i < len; ++i)
        {
            const float t = (float) (i + 1) / (float) len;
            const float inG = in0 + (in1 - in0) * t;
            const float ceiling = c0 + (c1 - c0) * t;

            float pk = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                ch[c][off + i] *= inG;
                pk = std::max (pk, std::abs (ch[c][off + i]));
            }
            const float need = pk > ceiling ? ceiling / pk : 1.0f;

            // running minimum of the last `hold` targets (monotonic deque)
            holdBuf[(size_t) idx] = need;
            while (dqTail != dqHead)
            {
                const int back = (dqTail - 1 + dqSize) % dqSize;
                if (holdBuf[(size_t) (dq[(size_t) back] % hold)] >= need)
                    dqTail = back;
                else
                    break;
            }
            dq[(size_t) dqTail] = counter;
            dqTail = (dqTail + 1) % dqSize;
            while (dq[(size_t) dqHead] <= counter - hold)
                dqHead = (dqHead + 1) % dqSize;
            const float h = holdBuf[(size_t) (dq[(size_t) dqHead] % hold)];

            boxSum += (double) h - (double) boxBuf[(size_t) idx];
            boxBuf[(size_t) idx] = h;
            const float avg = (float) (boxSum / hold);

            env = avg < env ? avg : env + rel * (avg - env);
            minEnv = std::min (minEnv, env);

            idx = (idx + 1 == hold) ? 0 : idx + 1;
            ++counter;

            // the gain is applied after the audio delay below; store it in place for now
            gainScratch[i] = env;
            ceilScratch[i] = ceiling;
        }

        float* seg[maxCh] = { ch[0] + off, numCh > 1 ? ch[1] + off : nullptr };
        audioDelay.process (seg, numCh, len);
        for (int c = 0; c < numCh; ++c)
            for (int i = 0; i < len; ++i)
                seg[c][i] = juce::jlimit (-ceilScratch[i], ceilScratch[i], seg[c][i] * gainScratch[i]);
    }
    grDb.store (gainToDb (minEnv), std::memory_order_relaxed);
}
} // namespace fx
