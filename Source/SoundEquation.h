#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "UI/Params.h"
#include "UI/Help.h"
#include "UI/Widgets.h"

// "Wave equation" stage: reshapes the actual audio with a formula y = f(...), chosen from
// preset equation families (tuned with dials A-D) or typed as a custom formula.
// Runs in the processor after the filters and before the double slit.
//
// Owner: Agent A (implements SoundEquation.cpp + the panel). Public API is a contract.
// Parameter ids must start with "eq". The custom formula text is stored in the APVTS state
// tree as property "eqFormula" so it is saved with the project.
class SoundEquationStage
{
public:
    static void addParameters (ParamList&);
    static HelpList helpEntries();

    static juce::StringArray presetNames();          // last entry is "Custom formula"
    static juce::String presetFormula (int index);   // the formula each preset evaluates, for display/editing

    explicit SoundEquationStage (juce::AudioProcessorValueTreeState&);
    ~SoundEquationStage();

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void process (juce::AudioBuffer<float>&, int numChannels) noexcept;   // audio thread

    // Message thread. Compiles and installs a custom formula; returns "" on success or an error.
    juce::String setCustomFormula (const juce::String& text);
    juce::String getCustomFormula() const;
    void reloadFromState();                          // called after the host restores state

    juce::String describeMapping() const;            // live, plain-words explanation for the Inspector

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class SoundEquationPanel : public ControlPanel
{
public:
    SoundEquationPanel (juce::AudioProcessorValueTreeState&, SoundEquationStage&, HelpTarget*);
    ~SoundEquationPanel() override;
    int idealHeight (int width) const override;
    juce::String title() const override { return "Equation"; }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
