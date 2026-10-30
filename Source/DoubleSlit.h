#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "ScopeBuffer.h"

// Simulates hearing a sound through a double slit with a very large ear.
//
// A plane wave hits a barrier with two slits (separation d, width a). The "ear"
// is a line at distance L that spans n bright fringes. Every point on the ear
// receives a copy of the sound from every point of each slit, delayed by its
// path length over the speed of sound (the shortest path is heard instantly)
// and attenuated by cylindrical spreading. Summing all copies gives an impulse
// response, which a background thread rebuilds whenever the geometry changes
// and the audio thread applies with a zero-latency convolution.
class DoubleSlitEngine : private juce::Thread
{
public:
    enum Sizing { perFrequency, followPitch, fixedMetres };
    enum EarModel { physical, brightOnly, fringeWeighted };

    struct Settings
    {
        int sizing = perFrequency;
        float multiple = 8.0f;      // d = multiple * wavelength (per-frequency / follow-pitch)
        float separation = 1.0f;    // d in metres (fixed)
        float width = 0.2f;         // a as a fraction of d
        int fringes = 5;
        float distance = 50.0f;     // L in metres
        int ear = physical;
        double sampleRate = 44100.0;
        float pitchHz = 220.0f;
    };

    static constexpr double speedOfSound = 343.0;
    static constexpr float fixedReferenceHz = 440.0f;

    DoubleSlitEngine (juce::AudioProcessorValueTreeState&, const ScopeBuffer& input);
    ~DoubleSlitEngine() override;

    void prepare (const juce::dsp::ProcessSpec&);
    void process (juce::AudioBuffer<float>&, int numChannels);

    // Geometry shared by the impulse builder and the editor's wave graphic
    static float earSinTheta (const Settings&);
    static float displayRatio (const Settings&);   // d / wavelength for the graphic

    Settings currentSettings() const;

    std::atomic<float> spreadMs { 0.0f }, earWidthM { 0.0f }, pitchUsedHz { 0.0f };
    std::atomic<bool> truncated { false }, building { false }, ready { false };

private:
    void run() override;
    juce::AudioBuffer<float> buildImpulse (const Settings&);

    juce::AudioProcessorValueTreeState& apvts;
    const ScopeBuffer& inputScope;

    juce::dsp::Convolution convolution;
    juce::AudioBuffer<float> dryCopy;
    juce::SmoothedValue<float> mixSm;
    int maxBlock = 512;
    bool wasOn = false;

    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<float> trackedPitch { 220.0f };
    std::vector<float> pitchScratch;

    std::atomic<float>* pOn;
    std::atomic<float>* pSizing;
    std::atomic<float>* pMultiple;
    std::atomic<float>* pSeparation;
    std::atomic<float>* pWidth;
    std::atomic<float>* pFringes;
    std::atomic<float>* pDistance;
    std::atomic<float>* pEar;
    std::atomic<float>* pMix;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DoubleSlitEngine)
};
