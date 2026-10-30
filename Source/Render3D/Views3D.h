#pragma once

#include "../PluginProcessor.h"
#include "../UI/Params.h"
#include "../UI/Help.h"
#include "../UI/Widgets.h"
#include "../UI/ViewFeed.h"
#include "Scene3D.h"

// The four 3D wave views (Helix / analytic signal, Phase space, Stereo 3D, Waterfall terrain)
// plus picture-only mapping dials and picture-only custom x/y/z formulas. Nothing here changes
// the sound.
//
// Owner: Agent B (implements Views3D.cpp + the panel). Public API is a contract.
// Parameter ids must start with "v3". Custom formulas are stored in the APVTS state tree as
// properties "v3FormulaX", "v3FormulaY", "v3FormulaZ".
class Views3D
{
public:
    static void addParameters (ParamList&);
    static HelpList helpEntries();

    explicit Views3D (ClodScopeProcessor&);   // the editor's: this plugin's sound and settings

    // Read-only (Jam Sesh tiles): any feed + settings (formulas from params.getText). Both must outlive it.
    // setFormula then only changes this view, nothing is saved.
    Views3D (const ViewFeed&, const ViewParams&);
    ~Views3D();

    // Message thread, called ~60 times a second while the 3D view is showing.
    // Reads audio from its feed and submits this frame to the scene
    // (geometry, world radius, axes labels, overlay text).
    void produce (Scene3D& scene);

    juce::String describeMapping() const;   // live: what x, y, z and colour mean right now

    // Picture-only custom mapping (axis 0 = x, 1 = y, 2 = z). Returns "" or an error message.
    juce::String setFormula (int axis, const juce::String& text);
    juce::String getFormula (int axis) const;
    void reloadFromState();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class Views3DPanel : public ControlPanel
{
public:
    Views3DPanel (ClodScopeProcessor&, Views3D&, HelpTarget*);
    ~Views3DPanel() override;
    int idealHeight (int width) const override;
    juce::String title() const override { return "3D"; }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
