#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "ScopeBuffer.h"
#include "DoubleSlit.h"
#include "SoundEquation.h"
#include "Sculpt/SculptModel.h"
#include "Synth/SynthEngine.h"
#include "Fx/FxRack.h"
#include "Presets/PresetManager.h"
#include "Jam/JamPort.h"

// Signal chain (processBlock):
//   input -> input stage (gain, gate, DC, drive/shape, bits, downsample, HPF/LPF)
//         -> synth layer (tracks the raw input; mixes input and synth per layer mode)
//         -> equation stage -> FX rack (amp, cab, EQ, comp, mod, delay, reverb)
//         -> double slit -> limiter -> global dry/wet mix (dry delayed by the rack's latency) -> output gain
//         -> Jam Sesh (sends this player's sound, mixes in the friends and the backing track)
class ClodScopeProcessor : public juce::AudioProcessor
{
public:
    // jamReplica: a hidden copy that rebuilds a friend's sound from their raw input (Jam/JamReplica.h);
    // it has no Jam Sesh port of its own
    explicit ClodScopeProcessor (bool jamReplica = false);

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;   // host transport jumps / stops: clear voices, tails and delay lines
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }     // MIDI notes play the synth layer
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }   // delay and reverb tails

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioProcessorValueTreeState apvts;
    ScopeBuffer inputScope, outputScope;
    ScopeBuffer synthScope;                       // the synth layer alone (for the sculptor's live view)
    std::atomic<double> currentSampleRate { 44100.0 };
    SoundEquationStage soundEq;
    SculptModel sculpt;
    SynthEngine synth;
    FxRack fx;
    DoubleSlitEngine doubleSlit;
    PresetManager presets;                        // it reads and writes everything above

    jam::Port* getJamPort() noexcept { return jamPort.get(); }
    std::atomic<double> fallbackBpm { 120.0 };    // tempo when the host gives none (the standalone app sets it)

    // How the interface's inputs reach the sound (the standalone app's input choice, App/StandaloneBar.cpp):
    // as they come (stereo: first input left, second right), the first input in the middle, or the two
    // mixed into the middle (a mic and a guitar). The plugin leaves it at stereo; the host routes there.
    enum InputRouting { inputStereo = 0, inputFirstCentred = 1, inputMixCentred = 2 };
    std::atomic<int> inputRouting { inputStereo };

    // Sees every finished block, after Jam Sesh: everything you hear (the standalone app's recorder)
    struct AudioTap
    {
        virtual ~AudioTap() = default;
        virtual void tap (const juce::AudioBuffer<float>& block, int numChannels, int numSamples) noexcept = 0;   // audio thread
    };
    std::atomic<AudioTap*> outputTap { nullptr };

private:
    std::unique_ptr<jam::Port> jamPort;           // nullptr for replicas
    juce::AudioBuffer<float> jamRaw;              // this block's input before any processing
    float shape (float x, int mode, float k) const noexcept;
    void delayDry (int numChannels, int numSamples) noexcept;

    juce::dsp::StateVariableTPTFilter<float> hpf, lpf;
    juce::AudioBuffer<float> dryBuffer;
    juce::SmoothedValue<float> inGainSm, outGainSm, mixSm;

    // the FX rack may add latency; the dry path of the global mix is delayed to match
    juce::AudioBuffer<float> dryDelay;
    int dryDelayPos = 0, latency = 0;

    float holdSample[2] {};
    int holdCounter[2] {};
    float gateEnv = 0.0f, gateGain = 1.0f;
    float gateAttack = 0.0f, gateRelease = 0.0f, envRelease = 0.0f;

    std::atomic<float>* pInGain;
    std::atomic<float>* pGate;
    std::atomic<float>* pDc;
    std::atomic<float>* pInvert;
    std::atomic<float>* pDrive;
    std::atomic<float>* pShape;
    std::atomic<float>* pBits;
    std::atomic<float>* pDownsample;
    std::atomic<float>* pHpf;
    std::atomic<float>* pLpf;
    std::atomic<float>* pMix;
    std::atomic<float>* pOutGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClodScopeProcessor)
};
