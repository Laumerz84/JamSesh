#pragma once

#include "../PluginProcessor.h"
#include "../UI/Params.h"
#include "../UI/Help.h"
#include "../UI/Widgets.h"
#include "../Render3D/Scene3D.h"
#include "OrbitalModel.h"

// Renders the orbital probability density from OrbitalModel into the shared Scene3D, with three
// quality levels: Slices (3 cross-sections), Voxel grid (section-by-section cells at an adjustable
// lower resolution; default), Point cloud (full samples). Also hosts the Orbital control panel,
// which shows BOTH the bridge parameters (OrbitalModel) and the render parameters.
//
// Owner: Agent C2 (implements OrbitalView.cpp + the panel). Public API is a contract.
// Render parameter ids must start with "orbR".
class OrbitalView
{
public:
    static void addParameters (ParamList&);
    static HelpList helpEntries();

    OrbitalView (ClodScopeProcessor&, orbital::OrbitalModel&);   // the editor's: this plugin's sound and settings

    // Any feed + settings (Jam Sesh tiles; give the model the same settings). All must outlive the view.
    OrbitalView (const ViewFeed&, const ViewParams&, orbital::OrbitalModel&);
    ~OrbitalView();

    // Message thread, ~60 Hz while the orbital view is showing: pulls audio from its feed, calls model.update(), builds geometry for the current render mode, submits it.
    void produce (Scene3D& scene);

    juce::String describeMapping() const;   // render mode explanation + model.describeMapping()

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class OrbitalPanel : public ControlPanel
{
public:
    OrbitalPanel (ClodScopeProcessor&, orbital::OrbitalModel&, OrbitalView&, HelpTarget*);
    ~OrbitalPanel() override;
    int idealHeight (int width) const override;
    juce::String title() const override { return "Orbital"; }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
