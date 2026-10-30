#pragma once

#include "../PluginProcessor.h"
#include "../UI/Widgets.h"

// The Wave Sculptor main view (mainView index 3, "Wave Sculptor") and its control panel.
// The view is the hands-on editor: the wave itself, its overtones and their superposition, edited
// directly with the mouse. It reads/writes proc.sculpt, shows proc.synth.tracker() and the live
// synth layer (proc.synthScope).
//
// Owner: Agent "Sculpt". Public API is a CONTRACT (the editor embeds these). Private state in Impl.

class SculptView : public juce::Component
{
public:
    SculptView (ClodScopeProcessor&, HelpTarget*);
    ~SculptView() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class SculptPanel : public ControlPanel
{
public:
    SculptPanel (ClodScopeProcessor&, HelpTarget*);
    ~SculptPanel() override;
    int idealHeight (int width) const override;
    juce::String title() const override { return "Sculpt"; }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
