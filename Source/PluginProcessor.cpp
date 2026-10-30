#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Render3D/Views3D.h"
#include "Orbital/OrbitalView.h"

namespace
{
    juce::String dbText (float v, int)      { return juce::String (v, 1) + " dB"; }
    juce::String hzText (float v, int)      { return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String ((int) v) + " Hz"; }
    juce::String pctText (float v, int)     { return juce::String ((int) std::round (v)) + " %"; }

    juce::NormalisableRange<float> skewed (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout ClodScopeProcessor::createLayout()
{
    using namespace juce;
    using FloatAttr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    // ---- signal processing ----
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "inGain", 1 }, "Input Gain",
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, FloatAttr().withStringFromValueFunction (dbText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "gate", 1 }, "Noise Gate",
        NormalisableRange<float> (-100.0f, 0.0f, 0.5f), -100.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return v <= -99.9f ? String ("Off") : dbText (v, 0); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "dc", 1 }, "DC Offset",
        NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, 3); })));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "invert", 1 }, "Phase Invert", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "drive", 1 }, "Drive",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, FloatAttr().withStringFromValueFunction (pctText)));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "shape", 1 }, "Shape",
        StringArray { "Soft (tanh)", "Hard Clip", "Wavefold", "Sine", "Asymmetric" }, 0));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "bits", 1 }, "Bit Depth", 1, 24, 24,
        AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return v >= 24 ? String ("Off") : String (v) + " bit"; })));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "downsample", 1 }, "Downsample", 1, 32, 1,
        AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return v <= 1 ? String ("Off") : String (v) + "x"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "hpf", 1 }, "High-Pass",
        skewed (20.0f, 2000.0f, 200.0f), 20.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return v <= 20.5f ? String ("Off") : hzText (v, 0); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "lpf", 1 }, "Low-Pass",
        skewed (200.0f, 20000.0f, 2000.0f), 20000.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return v >= 19990.0f ? String ("Off") : hzText (v, 0); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Dry/Wet",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, FloatAttr().withStringFromValueFunction (pctText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "outGain", 1 }, "Output Gain",
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, FloatAttr().withStringFromValueFunction (dbText)));

    // ---- scope display ----
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "time", 1 }, "Time Window",
        skewed (1.0f, 500.0f, 30.0f), 20.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 1 : 0) + " ms"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "zoom", 1 }, "Vertical Zoom",
        skewed (0.25f, 32.0f, 2.0f), 1.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return "x" + String (v, 2); })));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "trigMode", 1 }, "Trigger",
        StringArray { "Free Run", "Rising Edge", "Falling Edge" }, 1));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "trigLevel", 1 }, "Trigger Level",
        NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, 3); })));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "view", 1 }, "Channel View",
        StringArray { "Left + Right", "Left", "Right", "Mid (L+R)", "Side (L-R)", "XY / Lissajous" }, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "source", 1 }, "Show Signal",
        StringArray { "Input (raw)", "Output (modified)", "Both (compare)" }, 2));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "persist", 1 }, "Persistence",
        NormalisableRange<float> (0.0f, 95.0f, 0.1f), 35.0f, FloatAttr().withStringFromValueFunction (pctText)));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "freeze", 1 }, "Freeze", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "spectrum", 1 }, "Spectrum", true));

    // ---- double slit ----
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "dsOn", 1 }, "Double Slit", false));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "dsSizing", 1 }, "Slit Sizing",
        StringArray { "Per frequency", "Follow pitch", "Fixed (metres)" }, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "dsMultiple", 1 }, "Slit Multiple",
        skewed (1.0f, 50.0f, 8.0f), 8.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " wl"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "dsSeparation", 1 }, "Slit Separation",
        skewed (0.05f, 20.0f, 1.0f), 1.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return v < 1.0f ? String ((int) std::round (v * 100.0f)) + " cm" : String (v, 2) + " m"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "dsWidth", 1 }, "Slit Width",
        NormalisableRange<float> (0.01f, 0.9f, 0.01f), 0.2f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, 2) + " d"; })));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "dsFringes", 1 }, "Fringes", 1, 40, 5));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "dsDistance", 1 }, "Screen Distance",
        skewed (1.0f, 500.0f, 50.0f), 50.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 1 : 0) + " m"; })));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "dsEar", 1 }, "Ear Model",
        StringArray { "Physical", "Bright fringes only", "Fringe-weighted" }, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "dsMix", 1 }, "Slit Mix",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, FloatAttr().withStringFromValueFunction (pctText)));

    // ---- main view + modules (each module owns its own parameter definitions) ----
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "mainView", 1 }, "Main View",
        StringArray { "2D Scope", "3D Views", "Orbital", "Wave Sculptor", "Jam Sesh" }, 0));
    SoundEquationStage::addParameters (p);
    SculptModel::addParameters (p);
    SynthEngine::addParameters (p);
    FxRack::addParameters (p);
    Views3D::addParameters (p);
    orbital::OrbitalModel::addParameters (p);
    OrbitalView::addParameters (p);

    return { p.begin(), p.end() };
}

ClodScopeProcessor::ClodScopeProcessor (bool jamReplica)
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ClodScope", createLayout()),
      soundEq (apvts),
      sculpt (apvts),
      synth (apvts, sculpt, synthScope),
      fx (apvts),
      doubleSlit (apvts, inputScope),
      presets (*this)
{
    pInGain     = apvts.getRawParameterValue ("inGain");
    pGate       = apvts.getRawParameterValue ("gate");
    pDc         = apvts.getRawParameterValue ("dc");
    pInvert     = apvts.getRawParameterValue ("invert");
    pDrive      = apvts.getRawParameterValue ("drive");
    pShape      = apvts.getRawParameterValue ("shape");
    pBits       = apvts.getRawParameterValue ("bits");
    pDownsample = apvts.getRawParameterValue ("downsample");
    pHpf        = apvts.getRawParameterValue ("hpf");
    pLpf        = apvts.getRawParameterValue ("lpf");
    pMix        = apvts.getRawParameterValue ("mix");
    pOutGain    = apvts.getRawParameterValue ("outGain");

    if (! jamReplica)
        jamPort = std::make_unique<jam::Port> (*this);
}

bool ClodScopeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void ClodScopeProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
    hpf.prepare (spec);
    lpf.prepare (spec);
    hpf.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    lpf.setType (juce::dsp::StateVariableTPTFilterType::lowpass);

    dryBuffer.setSize (2, samplesPerBlock);
    const int channels = juce::jmax (1, getTotalNumOutputChannels());
    soundEq.prepare (sampleRate, samplesPerBlock, channels);
    synth.prepare (sampleRate, samplesPerBlock, channels);
    fx.prepare (sampleRate, samplesPerBlock, channels);

    latency = juce::jmax (0, fx.latencySamples());
    setLatencySamples (latency);
    dryDelay.setSize (2, juce::jmax (1, latency));
    dryDelay.clear();
    dryDelayPos = 0;
    doubleSlit.prepare ({ sampleRate, (juce::uint32) samplesPerBlock,
                          (juce::uint32) juce::jmax (1, getTotalNumOutputChannels()) });

    inGainSm.reset (sampleRate, 0.02);
    outGainSm.reset (sampleRate, 0.02);
    mixSm.reset (sampleRate, 0.02);
    inGainSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pInGain->load()));
    outGainSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pOutGain->load()));
    mixSm.setCurrentAndTargetValue (pMix->load() * 0.01f);

    // gate: 1 ms open, 80 ms close, 50 ms envelope release
    const auto sr = (float) sampleRate;
    gateAttack  = 1.0f - std::exp (-1.0f / (0.001f * sr));
    gateRelease = 1.0f - std::exp (-1.0f / (0.080f * sr));
    envRelease  = std::exp (-1.0f / (0.050f * sr));
    gateEnv = 0.0f;
    gateGain = 1.0f;

    holdSample[0] = holdSample[1] = 0.0f;
    holdCounter[0] = holdCounter[1] = 0;

    jamRaw.setSize (2, samplesPerBlock);
    if (jamPort != nullptr)
        jamPort->prepare (sampleRate, samplesPerBlock);
}

float ClodScopeProcessor::shape (float x, int mode, float k) const noexcept
{
    switch (mode)
    {
        case 0:  return std::tanh (k * x) / std::tanh (k);
        case 1:  return juce::jlimit (-1.0f, 1.0f, k * x);
        case 2:
        {
            // triangle fold: values past +-1 reflect back into range
            const float t = 0.25f * k * x + 0.25f;
            return 4.0f * std::abs (t - std::round (t)) - 1.0f;
        }
        case 3:  return std::sin (juce::MathConstants<float>::halfPi * k * x);
        case 4:  return x >= 0.0f ? std::tanh (k * x) / std::tanh (k)
                                  : std::tanh (0.4f * k * x) / std::tanh (k);
        default: return x;
    }
}

void ClodScopeProcessor::reset()
{
    synth.reset();
    fx.reset();
    dryDelay.clear();
    dryDelayPos = 0;
}

void ClodScopeProcessor::delayDry (int numChannels, int numSamples) noexcept
{
    if (latency <= 0)
        return;
    int pos = dryDelayPos;
    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* d = dryDelay.getWritePointer (ch);
            const float delayed = d[pos];
            d[pos] = dryBuffer.getSample (ch, i);
            dryBuffer.setSample (ch, i, delayed);
        }
        pos = (pos + 1) % latency;
    }
    dryDelayPos = pos;
}

void ClodScopeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int n = buffer.getNumSamples();
    if (numCh == 0 || n == 0)
        return;

    for (int ch = getTotalNumInputChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, n);

    auto* l = buffer.getWritePointer (0);
    auto* r = numCh > 1 ? buffer.getWritePointer (1) : l;
    inputScope.push (l, r, n);

    dryBuffer.setSize (2, n, false, false, true);
    for (int ch = 0; ch < numCh; ++ch)
        dryBuffer.copyFrom (ch, 0, buffer, ch, 0, n);
    if (numCh == 1)
        dryBuffer.copyFrom (1, 0, buffer, 0, 0, n);   // mono hosts: both analysis channels valid

    if (jamPort != nullptr)
    {
        jamRaw.setSize (2, n, false, false, true);
        jamRaw.copyFrom (0, 0, dryBuffer, 0, 0, n);
        jamRaw.copyFrom (1, 0, dryBuffer, 1, 0, n);
    }

    inGainSm.setTargetValue (juce::Decibels::decibelsToGain (pInGain->load()));
    outGainSm.setTargetValue (juce::Decibels::decibelsToGain (pOutGain->load()));
    mixSm.setTargetValue (pMix->load() * 0.01f);

    const float gateDb   = pGate->load();
    const bool gateOn    = gateDb > -99.9f;
    const float gateThr  = juce::Decibels::decibelsToGain (gateDb);
    const float dc       = pDc->load();
    const float polarity = pInvert->load() > 0.5f ? -1.0f : 1.0f;
    const float drive    = pDrive->load() * 0.01f;
    const float k        = 1.0f + drive * drive * 30.0f;
    const int shapeMode  = (int) pShape->load();
    const int bits       = (int) pBits->load();
    const float steps    = (float) (1 << juce::jlimit (0, 23, bits - 1));
    const int dsFactor   = juce::jmax (1, (int) pDownsample->load());
    const float hpfHz    = pHpf->load();
    const float lpfHz    = juce::jmin (pLpf->load(), (float) currentSampleRate.load() * 0.45f);
    const bool hpfOn     = hpfHz > 20.5f;
    const bool lpfOn     = pLpf->load() < 19990.0f;

    hpf.setCutoffFrequency (hpfHz);
    lpf.setCutoffFrequency (lpfHz);

    for (int i = 0; i < n; ++i)
    {
        const float inG = inGainSm.getNextValue();
        float s[2] = { l[i] * inG, r[i] * inG };

        if (gateOn)
        {
            const float level = juce::jmax (std::abs (s[0]), std::abs (s[1]));
            gateEnv = juce::jmax (level, gateEnv * envRelease);
            const float target = gateEnv > gateThr ? 1.0f : 0.0f;
            gateGain += (target - gateGain) * (target > gateGain ? gateAttack : gateRelease);
        }
        else
        {
            gateGain = 1.0f;
        }

        for (int ch = 0; ch < numCh; ++ch)
        {
            float x = s[ch] * gateGain;
            x = (x + dc) * polarity;

            // fade the shaper in over the first 10% of the knob so there's no level jump
            if (drive > 0.0f)
                x += (shape (x, shapeMode, k) - x) * juce::jmin (1.0f, drive * 10.0f);

            if (bits < 24)
                x = std::round (x * steps) / steps;

            if (dsFactor > 1)
            {
                if (--holdCounter[ch] <= 0)
                {
                    holdSample[ch] = x;
                    holdCounter[ch] = dsFactor;
                }
                x = holdSample[ch];
            }

            if (hpfOn) x = hpf.processSample (ch, x);
            if (lpfOn) x = lpf.processSample (ch, x);

            buffer.setSample (ch, i, x);
        }
    }

    sculpt.process();
    synth.process (buffer, numCh, dryBuffer, midi);
    soundEq.process (buffer, numCh);

    FxRack::Transport transport;
    transport.bpm = fallbackBpm.load();
    if (auto* head = getPlayHead())
        if (auto pos = head->getPosition())
        {
            if (auto bpm = pos->getBpm())
                transport.bpm = *bpm;
            if (auto ppq = pos->getPpqPosition())
                transport.ppqPosition = *ppq;
            transport.playing = pos->getIsPlaying();
        }
    // amp .. reverb, then the double slit, then the limiter as the very last safety stage
    fx.process (buffer, numCh, transport, false);
    doubleSlit.process (buffer, numCh);
    fx.processLimiter (buffer, numCh);

    delayDry (numCh, n);

    for (int i = 0; i < n; ++i)
    {
        const float outG = outGainSm.getNextValue();
        const float mix = mixSm.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float dry = dryBuffer.getSample (ch, i);
            buffer.setSample (ch, i, (dry + (buffer.getSample (ch, i) - dry) * mix) * outG);
        }
    }

    outputScope.push (l, r, n);

    if (jamPort != nullptr)
        jamPort->process (buffer, numCh, jamRaw, midi, transport.bpm, transport.ppqPosition, transport.playing);

    if (auto* t = outputTap.load())
        t->tap (buffer, numCh, n);
}

int ClodScopeProcessor::getNumPrograms()                       { return juce::jmax (1, presets.getNumPrograms()); }
int ClodScopeProcessor::getCurrentProgram()                    { return presets.getCurrentProgram(); }
void ClodScopeProcessor::setCurrentProgram (int index)         { presets.setCurrentProgram (index); }
const juce::String ClodScopeProcessor::getProgramName (int i)  { return presets.getProgramName (i); }

juce::AudioProcessorEditor* ClodScopeProcessor::createEditor()
{
    return new ClodScopeEditor (*this);
}

void ClodScopeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void ClodScopeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            soundEq.reloadFromState();
            sculpt.reloadFromState();
            presets.onStateRestored();
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ClodScopeProcessor();
}
