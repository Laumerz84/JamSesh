#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "../PluginProcessor.h"
#include "Theme.h"
#include "FastTrace.h"
#include "ViewFeed.h"

// The 2D scope main view: traces with afterglow, trigger, readouts, spectrum and the double-slit graphic.
// Draws from a ViewFeed (sound) and ViewParams (settings): the editor's own scope, or a friend's Jam Sesh tile.
class ScopeView : public juce::Component, private juce::Timer
{
public:
    // The editor's scope: this plugin's sound and settings, 60 fps, drag sets the trigger level
    explicit ScopeView (ClodScopeProcessor&);

    // Read-only (Jam Sesh tiles): any feed + settings, no mouse editing. Both must outlive the view.
    // slitEngine (optional) gives the double-slit graphic its live stats; without it they are left out.
    ScopeView (const ViewFeed&, const ViewParams&, int framesPerSecond = 30 /* 0 = owner calls tick() */, const DoubleSlitEngine* slitEngine = nullptr);
    ~ScopeView() override;

    void setFramesPerSecond (int fps);   // 0 = no timer: the owner calls tick()
    void tick() { timerCallback(); }      // one frame now (headless snapshots); the timer does this while visible
    void lookAndFeelChanged() override;   // the cached grid pictures carry theme colours

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void visibilityChanged() override;

private:
    enum Chan { left, right, mid, side };

    void timerCallback() override;
    void layoutAreas();
    void findTrigger();
    void updateReadouts();
    void updateSpectrum();
    void renderBackdrop();
    void renderTraces();
    void renderSpectrum();
    void drawWave (bool output, Chan, juce::Colour, float alpha);
    void drawXY (bool output, juce::Colour, float alpha);
    void paintGrid (juce::Graphics&, juce::Rectangle<float> area, bool xy) const;
    void paintSpectrumBackdrop (juce::Graphics&, juce::Rectangle<float> area) const;
    void paintSlit (juce::Graphics&);
    void renderSlitField();
    void setTriggerFromY (float y);

    float sample (bool output, Chan, int i) const noexcept;
    float param (const char* id, float fallback = 0.0f) const { return params->get (id, fallback); }
    float zoom() const { return param ("zoom", 1.0f); }
    float verticalScale() const { return (float) scopeArea.getHeight() * 0.45f * zoom(); }
    DoubleSlitEngine::Settings slitSettings() const;
    // the bottom strip (spectrum / double slit) only when there is room for it (small Jam Sesh tiles skip it)
    bool stripFits() const { return getHeight() >= 280; }
    bool wantsSpectrum() const { return param ("spectrum", 1.0f) > 0.5f && stripFits(); }
    bool wantsSlit() const { return param ("dsOn") > 0.5f && stripFits(); }

    void init();

    std::unique_ptr<ProcessorFeed> ownFeed;     // the editor's scope only
    std::unique_ptr<ApvtsParams> ownParams;
    ClodScopeProcessor* proc = nullptr;         // nullptr = read-only view
    const ViewFeed* feed = nullptr;
    const ViewParams* params = nullptr;
    const DoubleSlitEngine* slit = nullptr;
    int fps = 60;

    std::vector<float> inL, inR, outL, outR, midScratch;
    int copied = 0, segStart = 0, segLen = 0;
    bool triggered = false, spectrumShown = false, slitShown = false, dragging = false;

    juce::Rectangle<int> scopeArea, spectrumArea, slitArea;
    // The scope and spectrum pictures (grid + afterglowing traces, grid + curve), redrawn each frame at
    // the screen's pixel size (pixelScale) so that painting copies them 1:1
    juce::Image traces, backdrop, spectrum, spectrumBackdrop, slitField;
    float pixelScale = 1.0f, spectrumRate = 0.0f;
    int backdropView = -1;   // grid the backdrop shows: 0 = time, 1 = XY
    std::vector<juce::Point<float>> points;
    fasttrace::Columns columns;
    fasttrace::Stroker stroker;
    float wavePhase = 0.0f, waveLevel = 0.0f;

    // schematic geometry of the wave graphic, in pixels relative to slitArea
    struct SlitLayout { float lambda, sep, width, barrierX, screenX, earHalf; } slitLayout {};

    static constexpr int fftOrder = 11, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::vector<float> fftData = std::vector<float> ((size_t) fftSize * 2, 0.0f);
    std::vector<float> specDb = std::vector<float> ((size_t) fftSize / 2, -120.0f);

    float peakDb = -100.0f, rmsDb = -100.0f, freqHz = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScopeView)
};

