#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include "PluginProcessor.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"
#include "UI/FastTrace.h"
#include "UI/ScopeView.h"
#include "UI/Editor/Inspector.h"
#include "UI/Editor/ConceptGuide.h"
#include "UI/Editor/CorePanels.h"
#include "UI/Editor/Chain.h"
#include "UI/Editor/LookAndFeel.h"
#include "Sculpt/SculptView.h"
#include "Presets/PresetUI.h"
#include "Render3D/Scene3D.h"
#include "Render3D/Views3D.h"
#include "Orbital/OrbitalModel.h"
#include "Orbital/OrbitalView.h"
#include "Jam/JamView.h"
#include "Jam/JamPanel.h"
#include "UI/Themes.h"
#include "UI/Editor/ThemePicker.h"
#include "App/StandaloneBar.h"

// Scrolling host for one ControlPanel: keeps the panel as wide as the viewport and as tall as it asks.
class PanelViewport : public juce::Viewport
{
public:
    explicit PanelViewport (ControlPanel& p) : panel (p)
    {
        setViewedComponent (&panel, false);
        setScrollBarsShown (true, false);
        setScrollBarThickness (8);
    }

    void resized() override
    {
        juce::Viewport::resized();
        fitPanel();
    }

    // Also called periodically: panels may change their ideal height (e.g. showing extra controls)
    void fitPanel()
    {
        const int w = juce::jmax (100, getWidth() - getScrollBarThickness() - 2);
        const int h = panel.idealHeight (w);
        if (panel.getWidth() != w || panel.getHeight() != h)
            panel.setSize (w, h);
    }

    ControlPanel& panel;
};


// The editor shell:
//   top bar      : logo, preset bar, main-view switch, concept guide, theme, level / tracked-note readouts
//   app row      : standalone app only - audio settings, input mute, tempo, backing track, record
//   chain strip  : one chip per module in signal order (the main navigation) + the VIEW chip
//   main area    : 2D scope, 3D views / orbital (one shared Scene3D), the Wave Sculptor or Jam Sesh
//   right column : module header, the selected page's panel (scrolling), resizable Inspector
// Overlays: the preset browser and the concept guide (Esc or a click on the backdrop closes them;
// the 3D view pauses, hidden, while one is open).
// Every window paints in software so screen sharing shows the whole host window (UI/StreamSafe.h).
// Themes: colours are read from Palette / Accent at paint time; on a theme change the editor re-applies
// the look-and-feel and repaints (themeChanged).
class ClodScopeEditor : public juce::AudioProcessorEditor, private juce::Timer, private Themes::Listener
{
public:
    explicit ClodScopeEditor (ClodScopeProcessor&);
    ~ClodScopeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void parentHierarchyChanged() override;

private:
    void themeChanged() override;
    void timerCallback() override;
    void applyMainView (int view);
    void setMainView (int view);
    void selectPage (int page);
    void stepPage (int delta);
    static int pageForView (int view);
    void updateHeaderAction();
    void ensureScene();
    void ensureSculptView();
    void ensureJamView();
    void updateSceneVisibility();
    void showPresetBrowser();
    void hidePresetBrowser();
    void showGuide();
    void hideGuide();
    void layoutRightColumn();
    void setInspectorHeight (int h, bool store);
    void addPage (int page, ControlPanel&);
    void applyTooltips (juce::Component&);
    void updateMeters();
    void paintReadouts (juce::Graphics&);
    void paintLogo (juce::Graphics&, juce::Rectangle<int>);

    juce::String chipStatus (int page) const;
    juce::String liveMappingText() const;
    juce::String describeScope() const;
    juce::String describeInput() const;
    juce::String describeSlit() const;
    juce::String describeOutput() const;
    juce::String paramText (const char* id) const;
    float param (const char* id) const;
    bool isOn (const char* id) const { return param (id) > 0.5f; }

    ScopeLookAndFeel lnf;
    ClodScopeProcessor& proc;

    // models first: panels and views refer to them
    orbital::OrbitalModel orbitalModel;
    Views3D views3D;
    OrbitalView orbitalView;

    Inspector inspector;
    ScopeView scope;
    std::unique_ptr<Scene3D> scene;            // created on first use; one GL context for both 3D views
    std::unique_ptr<SculptView> sculptView;    // created on first use
    std::unique_ptr<JamView> jamView;          // created on first use

    // one control panel per page (chain::Page)
    SoundPanel inputPanel;
    SynthPanel synthPanel;
    SculptPanel sculptPanel;
    SoundEquationPanel equationPanel;
    std::unique_ptr<ControlPanel> fxPanels[FxRack::numModules];
    SlitPanel slitPanel;
    OutputPanel outputPanel;
    ScopePanel scopePanel;
    Views3DPanel views3DPanel;
    OrbitalPanel orbitalPanel;
    JamPanel jamPanel;
    std::array<std::unique_ptr<PanelViewport>, chain::numPages> viewports;
    std::array<bool, chain::numPages> tooltipsApplied {};

    ChainStrip strip;
    ModuleHeader header;
    SplitHandle split;

    PresetBar presetBar;
    PresetBrowser presetBrowser;            // full-window overlay with its own backdrop

    static constexpr int numViews = 5;      // 2D Scope, 3D Views, Orbital, Wave Sculptor, Jam Sesh
    juce::Component viewSwitch;
    juce::TextButton viewButtons[numViews];
    std::unique_ptr<widgets_detail::HoverReporter> viewSwitchHover;
    juce::TextButton guideButton { "?" };
    ThemePicker themePicker;
    std::unique_ptr<StandaloneBar> appBar;  // standalone app only
    ConceptGuide guide;
    juce::TooltipWindow tooltipWindow { this, 600 };

    std::unique_ptr<juce::ParameterAttachment> mainViewAttachment;
    int currentView = -1, currentPage = -1;
    bool initialising = true;
    int tick = 0;

    // Inspector height: -1 = automatic; remembered in the state ("uiInspectorH")
    int inspectorH = -1, inspectorExpandedH = 260, dragStartH = 0;
    bool draggingSplit = false;
    static constexpr int inspectorCollapsedH = 34;

    juce::Rectangle<int> topBar, appBarArea, stripArea, logoArea, readoutArea, mainArea, rightArea, panelArea;
    std::vector<float> readL, readR;
    float inPeakDb = -100.0f, outPeakDb = -100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClodScopeEditor)
};
