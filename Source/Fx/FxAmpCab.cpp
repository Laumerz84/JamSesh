#include "FxInternal.h"

// Amp: gate -> input tightening + pre-emphasis -> drive -> [4x oversampled: cascaded asymmetric
// triode stages with coupling/Miller filters -> tone stack -> push-pull power amp with sag]
// -> presence -> level. Cab: procedurally designed speaker + mic responses turned into
// minimum-phase IRs (plus a stereo room mic) on the worker thread, run through juce Convolution.

namespace fx
{
//==============================================================================
// Amp
//==============================================================================
static const AmpStage::Voice ampVoices[] = {
    //  name         blurb
    //  hpf  emphF emphDb emphQ  drvMin drvMax st  bias  stgDb interLp interHp  bassF midF trebF scoop  power sag  trim  auto  cleanLow
    { "Clean", "Glassy clean (blackface style): one stage, lots of headroom, breaks up only when pushed.",
      40.0f, 2500.0f,  2.0f, 0.7f,  -6.0f, 18.0f, 1, 0.12f,  0.0f, 9500.0f,  30.0f, 110.0f, 500.0f, 2600.0f, -3.0f, 0.6f, 0.20f,  0.0f, 0.80f, 0.0f },
    { "Crunch", "British crunch (plexi style): two stages, chords stay clear, singles bark.",
      60.0f, 1000.0f,  3.0f, 0.6f,   0.0f, 30.0f, 2, 0.25f,  6.0f, 7500.0f,  60.0f, 110.0f, 650.0f, 2400.0f, -1.0f, 1.0f, 0.30f, -7.5f, 0.45f, 0.0f },
    { "Lead", "Singing lead: mid-hump boost into three stages, smooth compressed sustain.",
      90.0f,  720.0f,  7.0f, 0.8f,  10.0f, 40.0f, 3, 0.30f, 10.0f, 6500.0f, 100.0f, 110.0f, 700.0f, 2400.0f,  0.0f, 1.2f, 0.35f, -14.5f, 0.15f, 0.0f },
    { "High Gain", "Modern high gain: tight lows, scooped mids, three hot stages for riffs and chugs.",
     110.0f, 1300.0f,  5.0f, 0.7f,  15.0f, 48.0f, 3, 0.35f, 12.0f, 6000.0f, 120.0f, 100.0f, 600.0f, 2800.0f, -4.0f, 1.5f, 0.25f, -14.0f, 0.10f, 0.0f },
    { "Fuzz", "Vintage fuzz into a small amp: hard asymmetric clipping, woolly and huge.",
      30.0f,  400.0f,  3.0f, 0.5f,  15.0f, 50.0f, 2, 0.60f, 14.0f, 4500.0f,  40.0f, 110.0f, 900.0f, 2200.0f, -6.0f, 0.8f, 0.50f, -14.5f, 0.10f, 0.0f },
    { "Bass", "Bass amp: lows stay clean and solid while the mids and highs get grit.",
      30.0f,  800.0f,  3.0f, 0.7f,  -3.0f, 30.0f, 2, 0.20f,  6.0f, 5000.0f,  50.0f,  80.0f, 500.0f, 2400.0f, -2.0f, 0.9f, 0.30f, -7.5f, 0.40f, 1.0f },
    { "Tube/Tape", "Warm tube/tape colour for synths, drums and mixes: gentle, smooth saturation.",
      20.0f, 3000.0f, -1.5f, 0.5f,  -6.0f, 24.0f, 1, 0.10f,  0.0f, 14000.0f, 20.0f, 110.0f, 500.0f, 3000.0f,  0.0f, 0.7f, 0.15f,  0.0f, 0.75f, 0.0f },
};

const AmpStage::Voice& AmpStage::voice (int m) noexcept
{
    return ampVoices[juce::jlimit (0, (int) std::size (ampVoices) - 1, m)];
}

juce::StringArray AmpStage::modelNames()
{
    juce::StringArray s;
    for (auto& v : ampVoices)
        s.add (v.name);
    return s;
}

void AmpStage::bind (juce::AudioProcessorValueTreeState& s)
{
    pModel = raw (s, "fxAmpModel");
    pGain = raw (s, "fxAmpGain");
    pBass = raw (s, "fxAmpBass");
    pMid = raw (s, "fxAmpMid");
    pTreble = raw (s, "fxAmpTreble");
    pPresence = raw (s, "fxAmpPresence");
    pLevel = raw (s, "fxAmpLevel");
    pGate = raw (s, "fxAmpGate");
    pBright = raw (s, "fxAmpBright");
}

void AmpStage::prepare (double sampleRate, int, int numChannels)
{
    sr = sampleRate;
    prepCh = juce::jlimit (1, maxCh, numChannels);
    os = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) prepCh, 2,
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, false, true);
    os->initProcessing ((size_t) chunk);
    osr = sr * 4.0;
    lat = (int) std::round (os->getLatencyInSamples());

    const double ctl = sr / chunk;
    for (auto* s : { &driveSm, &levelSm, &bassSm, &midSm, &trebleSm, &presSm })
        s->setTime (0.03f, ctl);

    sagAtt = onePoleCoef (0.004f, osr);
    sagRel = onePoleCoef (0.150f, osr);
    gateEnvRel = std::exp (-1.0f / (0.010f * (float) sr));
    gateOpen = onePoleCoef (0.0007f, sr);
    gateClose = onePoleCoef (0.070f, sr);
    gateHoldMax = (int) (0.045 * sr);
    reset();
}

void AmpStage::reset() noexcept
{
    if (os != nullptr)
        os->reset();
    for (auto* f : { &inHpf, &brightSh, &emph, &presenceSh, &dcBlock, &bassSh, &midBell, &trebleSh, &lowSplit })
        f->reset();
    for (int i = 0; i < 3; ++i)
    {
        stageHp[i].reset();
        stageLp[i].reset();
    }
    for (auto& e : sagEnv)
        e = 0.0f;
    gateEnv = 0.0f;
    gateGain = 1.0f;
    gateHold = 0;
    model = pendingModel = juce::jlimit (0, 6, (int) get (pModel));
    switchGain = 1.0f;

    const auto& v = voice (model);
    const float g01 = get (pGain) * 0.1f;
    driveSm.snap (v.driveMinDb + g01 * (v.driveMaxDb - v.driveMinDb));
    levelSm.snap (get (pLevel));
    bassSm.snap (get (pBass));
    midSm.snap (get (pMid));
    trebleSm.snap (get (pTreble));
    presSm.snap (get (pPresence));
    driveGain = dbToGain (driveSm.value);
    outGain = dbToGain (levelSm.value + v.trimDb - v.autoLevel * driveSm.value);
    updateFilters (v, bassSm.value, midSm.value, trebleSm.value, presSm.value, get (pBright) > 0.5f, g01);
}

void AmpStage::updateFilters (const Voice& v, float bass, float mid, float treble, float presence, bool bright, float drive01) noexcept
{
    inHpf.set (Svf::highpass, v.hpf, 0.707f, 0.0f, sr);
    // a bright cap matters less the further the gain pot is turned up, as on a real amp
    brightSh.set (Svf::highShelf, 1800.0f, 0.7f, bright ? 6.0f * (1.0f - 0.6f * drive01) : 0.0f, sr);
    emph.set (Svf::bell, v.emphF, v.emphQ, v.emphDb, sr);
    for (int i = 0; i < 3; ++i)
    {
        stageHp[i].set (Svf::highpass, v.interHpf, 0.707f, 0.0f, osr);
        stageLp[i].set (Svf::lowpass, v.interLpf, 0.6f, 0.0f, osr);
    }
    bassSh.set (Svf::lowShelf, v.bassF, 0.7f, (bass - 5.0f) * 2.4f, osr);
    const float midDb = v.scoopDb + (mid < 5.0f ? (mid - 5.0f) * 3.0f : (mid - 5.0f) * 1.6f);
    midBell.set (Svf::bell, v.midF, 0.8f, midDb, osr);
    trebleSh.set (Svf::highShelf, v.trebleF, 0.7f, (treble - 5.0f) * 2.4f, osr);
    lowSplit.set (Svf::lowpass, 220.0f, 0.707f, 0.0f, osr);
    presenceSh.set (Svf::highShelf, 4000.0f, 0.7f, (presence - 5.0f) * 1.6f, sr);
    dcBlock.set (Svf::highpass, 12.0f, 0.707f, 0.0f, sr);
}

void AmpStage::process (float* const* ch, int numCh, int n) noexcept
{
    if (os == nullptr)
        return;
    numCh = juce::jmin (numCh, prepCh);

    const float gateDb = get (pGate);
    const bool gateOn = gateDb > -99.9f;
    const float gateThr = dbToGain (gateDb);
    const float gateThrClose = gateThr * 0.5f;   // 6 dB hysteresis
    const bool bright = get (pBright) > 0.5f;

    for (int off = 0; off < n; off += chunk)
    {
        const int len = juce::jmin (chunk, n - off);

        // model change: dip the amp for ~10 ms, swap the voice at silence, come back
        const int wanted = juce::jlimit (0, 6, (int) get (pModel));
        if (wanted != model)
            pendingModel = wanted;
        const float switchTarget = (pendingModel != model) ? 0.0f : 1.0f;
        const float sgStart = switchGain;
        const float sgEnd = switchTarget > switchGain ? juce::jmin (1.0f, switchGain + 0.1f) : juce::jmax (0.0f, switchGain - 0.1f);
        switchGain = (switchTarget == switchGain) ? switchGain : sgEnd;

        const auto& v = voice (model);
        const float g01 = get (pGain) * 0.1f;
        const float drivePrev = driveGain, outPrev = outGain;
        driveSm.step (v.driveMinDb + g01 * (v.driveMaxDb - v.driveMinDb));
        levelSm.step (get (pLevel));
        driveGain = dbToGain (driveSm.value);
        outGain = dbToGain (levelSm.value + v.trimDb - v.autoLevel * driveSm.value);
        updateFilters (v, bassSm.step (get (pBass)), midSm.step (get (pMid)), trebleSm.step (get (pTreble)),
                       presSm.step (get (pPresence)), bright, g01);

        // --- base rate: gate, input filters, drive ---
        const float inv = 1.0f / (float) len;
        for (int i = 0; i < len; ++i)
        {
            if (gateOn)
            {
                float pk = 0.0f;
                for (int c = 0; c < numCh; ++c)
                    pk = std::max (pk, std::abs (ch[c][off + i]));
                gateEnv = std::max (pk, gateEnv * gateEnvRel);
                if (gateEnv > gateThr)
                    gateHold = gateHoldMax;
                else if (gateEnv < gateThrClose && gateHold > 0)
                    --gateHold;
                const float target = (gateHold > 0 || gateEnv > gateThrClose) ? 1.0f : 0.0f;
                gateGain += (target > gateGain ? gateOpen : gateClose) * (target - gateGain);
            }
            else
            {
                gateGain = 1.0f;
            }

            const float t = (float) (i + 1) * inv;
            const float drive = (drivePrev + t * (driveGain - drivePrev)) * (sgStart + t * (switchGain - sgStart)) * gateGain;
            for (int c = 0; c < numCh; ++c)
            {
                float x = ch[c][off + i];
                x = inHpf.process (x, c);
                x = brightSh.process (x, c);
                x = emph.process (x, c);
                ch[c][off + i] = x * drive;
            }
        }

        // --- 4x oversampled nonlinear core ---
        juce::dsp::AudioBlock<float> block (ch, (size_t) numCh, (size_t) off, (size_t) len);
        auto up = os->processSamplesUp (block);
        const int upLen = (int) up.getNumSamples();
        const float bias = v.bias;
        const float tb = std::tanh (bias);
        const float norm = 1.0f / (1.0f - tb * tb);
        const float stageGain = dbToGain (v.stageGainDb);
        const float pd = v.powerDrive;
        const float invPd = 1.0f / pd;
        const float cleanLowGain = v.cleanLow * 2.0f / juce::jmax (1.0e-3f, driveGain);

        for (int c = 0; c < numCh; ++c)
        {
            float* x = up.getChannelPointer ((size_t) c);
            float env = sagEnv[c];
            for (int i = 0; i < upLen; ++i)
            {
                float s = x[i];
                const float low = v.cleanLow > 0.0f ? lowSplit.process (s, c) : 0.0f;
                for (int st = 0; st < v.stages; ++st)
                {
                    // cascaded stages invert, so the asymmetry alternates like real triodes
                    const float b = (st & 1) ? -bias : bias;
                    const float tbs = (st & 1) ? -tb : tb;
                    s = (fastTanh ((st == 0 ? s : s * stageGain) + b) - tbs) * norm;
                    s = stageHp[st].process (s, c);
                    s = stageLp[st].process (s, c);
                }
                s = bassSh.process (s, c);
                s = midBell.process (s, c);
                s = trebleSh.process (s, c);

                const float a = std::abs (s);
                env += (a > env ? sagAtt : sagRel) * (a - env);
                const float sagGain = 1.0f / (1.0f + v.sag * env);
                s = fastTanh (pd * s * sagGain) * invPd;
                x[i] = s + low * cleanLowGain;
            }
            sagEnv[c] = env;
        }
        os->processSamplesDown (block);

        // --- base rate: presence, DC, level ---
        for (int i = 0; i < len; ++i)
        {
            const float t = (float) (i + 1) * inv;
            const float g = outPrev + t * (outGain - outPrev);
            for (int c = 0; c < numCh; ++c)
            {
                float x = ch[c][off + i];
                x = presenceSh.process (x, c);
                x = dcBlock.process (x, c);
                ch[c][off + i] = x * g;
            }
        }

        if (switchGain <= 0.0f && pendingModel != model)
        {
            model = pendingModel;
            for (int i = 0; i < 3; ++i)
            {
                stageHp[i].reset();
                stageLp[i].reset();
            }
            const auto& nv = voice (model);
            driveSm.snap (nv.driveMinDb + g01 * (nv.driveMaxDb - nv.driveMinDb));
            driveGain = dbToGain (driveSm.value);
            outGain = dbToGain (levelSm.value + nv.trimDb - nv.autoLevel * driveSm.value);
        }
    }
}

//==============================================================================
// Cab
//==============================================================================
namespace
{
    struct CabVoice
    {
        float resF, resQ, openHp, hiF;
        int hiOrder;
        float bells[4][3];   // freq, dB, Q
        float ripple;
        float seed;
    };

    const CabVoice cabVoices[] = {
        { 90.0f, 0.8f, 110.0f, 5500.0f, 4, { { 2300.0f, 3.5f, 1.4f }, { 550.0f, -2.5f, 0.9f }, { 1100.0f, 1.0f, 1.5f }, { 3800.0f, -2.0f, 3.0f } }, 2.5f, 0.3f },
        { 85.0f, 1.0f,  70.0f, 5000.0f, 4, { { 2700.0f, 4.0f, 1.6f }, { 1200.0f, 2.0f, 1.2f }, { 420.0f, -2.0f, 1.0f },  { 4300.0f, -3.0f, 3.0f } }, 3.0f, 1.1f },
        { 95.0f, 1.5f,   0.0f, 5200.0f, 5, { { 2500.0f, 5.0f, 1.8f }, { 1450.0f, 3.0f, 1.4f }, { 650.0f, -3.0f, 0.9f },  { 4200.0f, -5.0f, 3.5f } }, 3.5f, 2.3f },
        { 50.0f, 1.1f,   0.0f, 4200.0f, 4, { { 800.0f, 2.5f, 0.7f },  { 2000.0f, 2.0f, 1.4f }, { 250.0f, -1.5f, 1.0f },  { 3200.0f, -2.0f, 2.0f } }, 2.0f, 3.7f },
        { 220.0f, 1.3f, 300.0f, 3200.0f, 3, { { 1300.0f, 6.0f, 1.0f }, { 2600.0f, 3.0f, 2.0f }, { 500.0f, -3.0f, 1.0f },  { 1000.0f, 0.0f, 1.0f } }, 4.0f, 4.9f },
        { 0.0f, 0.7f, 0.0f, 0.0f, 0, { { 1000.0f, 0.0f, 1.0f }, { 1000.0f, 0.0f, 1.0f }, { 1000.0f, 0.0f, 1.0f }, { 1000.0f, 0.0f, 1.0f } }, 0.0f, 0.0f },
    };
    constexpr int numCabs = (int) std::size (cabVoices);

    double bellMag2 (double f, double f0, double db, double q)
    {
        const double A = std::pow (10.0, db / 40.0);
        const double w = f / f0;
        const double re = 1.0 - w * w;
        const double num = re * re + (w * A / q) * (w * A / q);
        const double den = re * re + (w / (A * q)) * (w / (A * q));
        return num / den;
    }

    // Raw (un-normalised) response in dB; mic 0 = on-axis at the cap, 1 = off-axis at the edge
    double rawDb (const CabVoice& v, double mic, double lowCut, double highCut, double f)
    {
        f = juce::jmax (1.0, f);
        double mag2 = 1.0;
        if (v.resF > 0.0f)
        {
            const double w = f / v.resF, q = v.resQ;
            mag2 *= (w * w * w * w) / ((1.0 - w * w) * (1.0 - w * w) + (w / q) * (w / q));
        }
        if (v.openHp > 0.0f)
        {
            const double w = f / v.openHp;   // open back: front/back cancellation thins the lows
            mag2 *= w * w / (1.0 + w * w);
        }
        if (v.hiF > 0.0f)
        {
            const double w = f / (v.hiF * (1.0 - 0.15 * mic));
            mag2 /= 1.0 + std::pow (w, 2.0 * v.hiOrder);
            const double fo = 14000.0 * std::pow (0.25, mic);   // off-axis beaming loss
            const double wo = f / fo;
            mag2 /= 1.0 + wo * wo * wo * wo;
        }
        for (auto& b : v.bells)
            if (b[1] != 0.0f)
                mag2 *= bellMag2 (f, b[0], b[1] * (b[0] > 1000.0f ? 1.0 - 0.6 * mic : 1.0), b[2]);
        if (v.resF > 0.0f)
        {
            mag2 *= bellMag2 (f, 250.0, 2.0 * mic, 0.8);   // the cone edge has more body
            mag2 *= bellMag2 (f, 120.0, 2.5, 0.6);         // proximity effect of a close mic
        }
        if (lowCut > 20.1)
        {
            const double w = f / lowCut;
            mag2 *= (w * w * w * w) / (1.0 + w * w * w * w);
        }
        if (highCut < 19990.0)
        {
            const double w = f / highCut;
            mag2 /= 1.0 + w * w * w * w;
        }
        double db = 10.0 * std::log10 (juce::jmax (1.0e-12, mag2));

        // cone break-up: a smooth, deterministic irregularity above ~1 kHz
        if (v.ripple > 0.0f && f > 700.0)
        {
            const double l2 = std::log2 (f);
            const double amt = juce::jlimit (0.0, 1.0, (f - 700.0) / 1800.0) * v.ripple * (1.0 - 0.4 * mic);
            db += amt * (0.6 * std::sin (juce::MathConstants<double>::twoPi * 1.7 * l2 + v.seed)
                       + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 3.1 * l2 + 2.0 * v.seed)
                       + 0.2 * std::sin (juce::MathConstants<double>::twoPi * 5.3 * l2 + 3.0 * v.seed));
        }
        return juce::jmax (-90.0, db);
    }

    double normDb (const CabVoice& v, double mic, double lowCut, double highCut)
    {
        double sum = 0.0;
        constexpr int pts = 40;
        for (int i = 0; i < pts; ++i)
            sum += rawDb (v, mic, lowCut, highCut, 150.0 * std::pow (4000.0 / 150.0, i / (double) (pts - 1)));
        return sum / pts;
    }

    using Complex = std::complex<float>;

    // Minimum-phase spectrum from a magnitude (dB per bin, 0..M/2) via the real cepstrum
    void minPhaseSpectrum (juce::dsp::FFT& fft, float invScale, const std::vector<double>& dB, std::vector<Complex>& out)
    {
        const int M = fft.getSize();
        std::vector<Complex> a ((size_t) M), b ((size_t) M);
        for (int k = 0; k <= M / 2; ++k)
        {
            const float lm = (float) (dB[(size_t) k] * 0.11512925464970228);   // ln(10)/20
            a[(size_t) k] = { lm, 0.0f };
            if (k > 0 && k < M / 2)
                a[(size_t) (M - k)] = { lm, 0.0f };
        }
        fft.perform (a.data(), b.data(), true);
        for (int n = 0; n < M; ++n)
        {
            float c = b[(size_t) n].real() * invScale;
            if (n > 0 && n < M / 2)  c *= 2.0f;
            else if (n > M / 2)      c = 0.0f;
            a[(size_t) n] = { c, 0.0f };
        }
        fft.perform (a.data(), b.data(), false);
        out.resize ((size_t) M);
        for (int k = 0; k < M; ++k)
            out[(size_t) k] = std::exp (b[(size_t) k]);
    }
}

juce::StringArray CabStage::modelNames()
{
    return { "1x12 Open Back", "2x12 Combo", "4x12 Closed (V30)", "4x10 Bass", "1x8 Lo-Fi", "Off (DI)" };
}

float CabStage::designDb (int model, float mic, float lowCut, float highCut, double f) noexcept
{
    const auto& v = cabVoices[juce::jlimit (0, numCabs - 1, model)];
    return (float) (rawDb (v, mic, lowCut, highCut, f) - normDb (v, mic, lowCut, highCut));
}

void CabStage::bind (juce::AudioProcessorValueTreeState& s)
{
    pModel = raw (s, "fxCabModel");
    pMic = raw (s, "fxCabMic");
    pRoom = raw (s, "fxCabRoom");
    pLevel = raw (s, "fxCabLevel");
    pLowCut = raw (s, "fxCabLowCut");
    pHighCut = raw (s, "fxCabHighCut");
}

void CabStage::prepare (double sampleRate, int maxBlock, int numChannels)
{
    sr = sampleRate;
    irChannels = juce::jlimit (1, maxCh, numChannels);
    conv.prepare ({ sampleRate, (juce::uint32) juce::jmax (1, maxBlock), (juce::uint32) irChannels.load() });
    levelSm.setTime (0.03f, sampleRate / chunk);
    for (auto& k : lastKey)
        k = -1.0f;
    prepared = true;
    rebuildIfNeeded();   // first IR right away (the worker is stopped while we are being prepared)
    reset();
}

void CabStage::reset() noexcept
{
    conv.reset();
    levelSm.snap (get (pLevel));
    gain = dbToGain (levelSm.value);
}

void CabStage::process (float* const* ch, int numCh, int n) noexcept
{
    juce::dsp::AudioBlock<float> block (ch, (size_t) numCh, (size_t) n);
    conv.process (juce::dsp::ProcessContextReplacing<float> (block));

    for (int off = 0; off < n; off += chunk)
    {
        const int len = juce::jmin (chunk, n - off);
        const float g0 = gain;
        gain = dbToGain (levelSm.step (get (pLevel)));
        for (int c = 0; c < numCh; ++c)
            for (int i = 0; i < len; ++i)
                ch[c][off + i] *= g0 + (gain - g0) * (float) (i + 1) / (float) len;
    }
}

void CabStage::rebuildIfNeeded()
{
    if (! prepared)
        return;
    const float key[5] = { get (pModel), get (pMic), get (pRoom), get (pLowCut), get (pHighCut) };
    bool changed = false;
    for (int i = 0; i < 5; ++i)
        if (std::abs (key[i] - lastKey[i]) > (i >= 3 ? 0.002f * key[i] : 0.25f))
            changed = true;
    if (! changed)
        return;
    for (int i = 0; i < 5; ++i)
        lastKey[i] = key[i];

    auto ir = buildIr ((int) key[0], key[1] * 0.01f, key[2] * 0.01f, key[3], key[4]);
    const auto stereo = ir.getNumChannels() > 1 ? juce::dsp::Convolution::Stereo::yes : juce::dsp::Convolution::Stereo::no;
    conv.loadImpulseResponse (std::move (ir), sr.load(), stereo, juce::dsp::Convolution::Trim::no,
                              juce::dsp::Convolution::Normalise::no);
}

juce::AudioBuffer<float> CabStage::buildIr (int model, float mic, float room, float lowCut, float highCut) const
{
    const double rate = sr.load();
    const auto& v = cabVoices[juce::jlimit (0, numCabs - 1, model)];
    const bool di = v.resF <= 0.0f;
    if (di)
        room = 0.0f;
    const int nch = irChannels.load();

    int order = 10;
    while ((1 << order) < (int) (rate * 0.2) && order < 16)
        ++order;
    juce::dsp::FFT fft (order);
    const int M = fft.getSize();

    // the inverse transform's scaling differs between FFT engines; measure it once
    std::vector<Complex> a ((size_t) M, Complex (1.0f, 0.0f)), b ((size_t) M);
    fft.perform (a.data(), b.data(), true);
    const float invScale = 1.0f / juce::jmax (1.0e-9f, b[0].real());

    auto spectrum = [&] (double micPos, double lowBoostDb, std::vector<Complex>& out)
    {
        const double nrm = normDb (v, micPos, lowCut, highCut);
        std::vector<double> dB ((size_t) (M / 2 + 1));
        for (int k = 0; k <= M / 2; ++k)
        {
            const double f = juce::jmax (5.0, k * rate / M);
            double d = rawDb (v, micPos, lowCut, highCut, f) - nrm;
            if (lowBoostDb != 0.0)
                d += lowBoostDb / (1.0 + (f / 160.0) * (f / 160.0));
            dB[(size_t) k] = juce::jlimit (-90.0, 30.0, d);
        }
        minPhaseSpectrum (fft, invScale, dB, out);
    };

    std::vector<Complex> closeSpec, roomSpec;
    spectrum (mic, 0.0, closeSpec);

    const int closeLen = juce::jmin (M, (int) (rate * 0.045));
    const int roomLen = juce::jmin (M, (int) (rate * 0.19));
    const int total = room > 0.001f ? roomLen : closeLen;

    juce::AudioBuffer<float> ir (nch, total);
    ir.clear();

    // close mic: inverse transform of the minimum-phase spectrum, faded over its last quarter
    fft.perform (closeSpec.data(), b.data(), true);
    std::vector<float> closeIr ((size_t) closeLen);
    for (int i = 0; i < closeLen; ++i)
    {
        const float fadeStart = 0.75f * (float) closeLen;
        const float w = i < fadeStart ? 1.0f : 0.5f + 0.5f * std::cos (pi * ((float) i - fadeStart) / (closeLen - fadeStart));
        closeIr[(size_t) i] = b[(size_t) i].real() * invScale * w;
    }

    const float closeGain = 1.0f - 0.7f * room;
    for (int c = 0; c < nch; ++c)
        for (int i = 0; i < closeLen && i < total; ++i)
            ir.setSample (c, i, closeIr[(size_t) i] * closeGain);

    if (room > 0.001f)
    {
        // room mic: the cab heard off-axis from ~1.5 m (more low end, duller top), through a
        // sparse set of early reflections and a short diffuse tail - different per side
        spectrum (1.0, 3.0, roomSpec);
        double closeEnergy = 0.0;
        for (auto s : closeIr)
            closeEnergy += (double) s * s;

        for (int c = 0; c < nch; ++c)
        {
            juce::Random rng (c == 0 ? 0x5eed1 + model : 0x7a11 + model * 3);
            std::vector<Complex> e ((size_t) M, Complex (0.0f, 0.0f));
            const int direct = (int) (rate * (0.0045 + 0.0004 * c));
            e[(size_t) direct] = { 1.0f, 0.0f };
            for (int r = 0; r < 10; ++r)
            {
                const double t = 0.006 + 0.036 * std::pow ((r + rng.nextDouble()) / 10.0, 1.3);
                const int pos = juce::jmin (M - 1, (int) (t * rate));
                const float g = 0.75f * std::exp (-(float) t / 0.03f) * (rng.nextBool() ? 1.0f : -1.0f);
                e[(size_t) pos] += Complex (g, 0.0f);
            }
            const int tailStart = (int) (rate * 0.012);
            const int tailEnd = juce::jmin (M, roomLen);
            float lp = 0.0f;
            const float lpc = onePoleCoef (1.0f / (2.0f * pi * 5000.0f), rate);
            for (int i = tailStart; i < tailEnd; ++i)
            {
                const float t = (float) (i - tailStart) / (float) rate;
                const float env = 0.09f * std::exp (-6.91f * t / 0.32f) * juce::jmin (1.0f, t / 0.004f);
                lp += lpc * ((rng.nextFloat() * 2.0f - 1.0f) - lp);
                e[(size_t) i] += Complex (lp * env * 2.0f, 0.0f);
            }

            std::vector<Complex> E ((size_t) M);
            fft.perform (e.data(), E.data(), false);
            for (int k = 0; k < M; ++k)
                E[(size_t) k] *= roomSpec[(size_t) k];
            fft.perform (E.data(), b.data(), true);

            std::vector<float> roomIr ((size_t) roomLen);
            double roomEnergy = 0.0;
            for (int i = 0; i < roomLen; ++i)
            {
                const float fadeStart = 0.8f * (float) roomLen;
                const float w = i < fadeStart ? 1.0f : 0.5f + 0.5f * std::cos (pi * ((float) i - fadeStart) / (roomLen - fadeStart));
                roomIr[(size_t) i] = b[(size_t) i].real() * invScale * w;
                roomEnergy += (double) roomIr[(size_t) i] * roomIr[(size_t) i];
            }
            const float roomGain = room * 0.85f * (float) std::sqrt (closeEnergy / juce::jmax (1.0e-12, roomEnergy));
            for (int i = 0; i < roomLen; ++i)
                ir.addSample (c, i, roomIr[(size_t) i] * roomGain);
        }
    }
    return ir;
}
} // namespace fx
