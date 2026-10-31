#include "PluginEditor.h"
#include "DspUtil.h"
#include "UI/Editor/CoreHelp.h"
#include "Synth/ChordName.h"
#include "UI/StreamSafe.h"
#include "UI/Surface.h"

namespace
{
    juce::Font font (float size, bool bold = false)
    {
        auto opts = juce::FontOptions (size);
        return juce::Font (bold ? opts.withStyle ("Bold") : opts);
    }

    juce::String dbString (float db)
    {
        return db <= -99.0f ? juce::String ("-inf") : juce::String (db, 1);
    }

}

//==============================================================================
ClodScopeEditor::ClodScopeEditor (ClodScopeProcessor& p)
    : AudioProcessorEditor (p), proc (p),
      orbitalModel (p.apvts), views3D (p), orbitalView (p, orbitalModel),
      inspector (p.apvts), scope (p),
      inputPanel (p.apvts, &inspector),
      synthPanel (p.apvts, p.synth, &inspector),
      sculptPanel (p, &inspector),
      equationPanel (p.apvts, p.soundEq, &inspector),
      slitPanel (p.apvts, &inspector),
      outputPanel (p.apvts, &inspector),
      scopePanel (p.apvts, &inspector),
      views3DPanel (p, views3D, &inspector),
      orbitalPanel (p, orbitalModel, orbitalView, &inspector),
      jamPanel (p, &inspector),
      strip (p.apvts, &inspector),
      header (p.apvts, &inspector),
      presetBar (p.presets, &inspector),
      presetBrowser (p.presets, &inspector)
{
    setLookAndFeel (&lnf);
    views3D.reloadFromState();

    for (const auto& list : { corehelp::entries(), SoundEquationStage::helpEntries(), Views3D::helpEntries(),
                              orbital::OrbitalModel::helpEntries(), OrbitalView::helpEntries(), SynthEngine::helpEntries(),
                              FxRack::helpEntries(), SculptModel::helpEntries(), PresetManager::helpEntries(),
                              chain::helpEntries(), JamPanel::helpEntries() })
        inspector.addEntries (list);
    inspector.setLiveSource ([this] { return currentPage >= 0 ? chain::info (currentPage).title : juce::String(); },
                             [this] { return liveMappingText(); });

    addAndMakeVisible (scope);

    // ---- pages: one panel per module / display, each in its own scrolling viewport ----
    for (int m = 0; m < FxRack::numModules; ++m)
    {
        fxPanels[m] = proc.fx.createPanel ((FxRack::Module) m, &inspector);
        if (fxPanels[m] == nullptr)
            fxPanels[m] = std::make_unique<PlaceholderPanel> (FxRack::moduleName ((FxRack::Module) m));
    }
    addPage (chain::input, inputPanel);
    addPage (chain::synth, synthPanel);
    addPage (chain::sculpt, sculptPanel);
    addPage (chain::equation, equationPanel);
    for (int page = chain::amp; page <= chain::limiter; ++page)
        if (const int m = chain::fxModule (page); m >= 0)
            addPage (page, *fxPanels[m]);
    addPage (chain::slit, slitPanel);
    addPage (chain::output, outputPanel);
    addPage (chain::viewScope, scopePanel);
    addPage (chain::view3D, views3DPanel);
    addPage (chain::viewOrbital, orbitalPanel);
    addPage (chain::viewJam, jamPanel);
    outputPanel.onSelect = [this] (int page) { selectPage (page); };

    // ---- chain strip, module header, Inspector + its resize handle ----
    strip.onSelect = [this] (int page) { selectPage (page >= chain::numPages ? pageForView (currentView) : page); };
    strip.statusText = [this] (int page) { return chipStatus (page); };
    addAndMakeVisible (strip);
    header.onStep = [this] (int delta) { stepPage (delta); };
    addAndMakeVisible (header);
    addAndMakeVisible (inspector);

    split.onDrag = [this] (int dy, bool finished)
    {
        if (! draggingSplit)
        {
            draggingSplit = true;
            dragStartH = inspector.getHeight();
        }
        setInspectorHeight (dragStartH - dy, finished);
        if (finished)
            draggingSplit = false;
    };
    split.onDoubleClick = [this]
    {
        if (inspector.getHeight() > inspectorCollapsedH + 8)
        {
            inspectorExpandedH = inspector.getHeight();
            setInspectorHeight (inspectorCollapsedH, true);
        }
        else
        {
            setInspectorHeight (inspectorExpandedH, true);
        }
    };
    addAndMakeVisible (split);

    // ---- top bar ----
    addAndMakeVisible (presetBar);
    applyTooltips (presetBar);
    presetBar.onBrowse = [this] { showPresetBrowser(); };
    // the browser draws its own dimmed backdrop over the whole window and closes itself on Esc / backdrop click
    presetBrowser.onClose = [this] { hidePresetBrowser(); };
    addChildComponent (presetBrowser);

    // segmented main-view switch, bound to the "mainView" parameter; each view in its own colour
    const char* names[] = { "2D Scope", "3D Views", "Orbital", "Wave Sculptor", "Jam Sesh" };
    const juce::Colour accents[] = { Accent::view, Accent::view, Accent::physics, Accent::sculpt, Accent::jam };
    const juce::String viewTip = inspector.find ("mainView") != nullptr ? inspector.find ("mainView")->summary : juce::String();
    for (int i = 0; i < numViews; ++i)
    {
        auto& b = viewButtons[i];
        b.setButtonText (names[i]);
        b.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < numViews - 1 ? juce::Button::ConnectedOnRight : 0));
        b.setTooltip (viewTip);
        Theme::setAccent (b, accents[i]);
        b.onClick = [this, i] { setMainView (i); };
        viewSwitch.addAndMakeVisible (b);
    }
    addAndMakeVisible (viewSwitch);
    viewSwitchHover = std::make_unique<widgets_detail::HoverReporter> (viewSwitch, "mainView", &inspector);

    guideButton.setTooltip ("Concept guide: how the plugin is organised, what each module does and how every mapping works");
    guideButton.onClick = [this] { showGuide(); };
    addAndMakeVisible (guideButton);
    addAndMakeVisible (themePicker);
    Themes::addListener (this);
    if (StandaloneBar::isStandaloneApp())
    {
        appBar = std::make_unique<StandaloneBar> (p);
        addAndMakeVisible (*appBar);
    }
    guide.onClose = [this] { hideGuide(); };
    addChildComponent (guide);

    readL.assign (2048, 0.0f);
    readR.assign (2048, 0.0f);

    // ---- restore the last session's layout ----
    inspectorH = (int) proc.apvts.state.getProperty ("uiInspectorH", -1);
    if (inspectorH > inspectorCollapsedH)
        inspectorExpandedH = inspectorH;
    int page = chain::pageForKey (proc.apvts.state.getProperty ("uiModule").toString());

    mainViewAttachment = std::make_unique<juce::ParameterAttachment> (*proc.apvts.getParameter ("mainView"),
        [this] (float v) { applyMainView ((int) std::round (v)); });

    setResizable (true, true);
    setResizeLimits (1200, 800, 3200, 2000);
    setSize (1500, 940);
    mainViewAttachment->sendInitialUpdate();

    if (page < 0)
        page = chain::input;
    if (chain::isViewPage (page))
        page = pageForView (currentView);
    selectPage (page);
    initialising = false;
    startTimerHz (60);
}

ClodScopeEditor::~ClodScopeEditor()
{
    Themes::removeListener (this);
    stopTimer();
    mainViewAttachment.reset();
    inspector.setLiveSource (nullptr, nullptr);
    scene.reset();
    sculptView.reset();
    jamView.reset();
    setLookAndFeel (nullptr);
}

void ClodScopeEditor::addPage (int page, ControlPanel& panel)
{
    const auto accent = chain::info (page).accent;
    if (! panel.getProperties().contains ("accent"))
        Theme::setAccent (panel, accent);
    auto vp = std::make_unique<PanelViewport> (panel);
    Theme::setAccent (*vp, accent);
    addChildComponent (*vp);
    viewports[(size_t) page] = std::move (vp);
}

//==============================================================================
int ClodScopeEditor::pageForView (int view)
{
    switch (view)
    {
        case 1:  return chain::view3D;
        case 2:  return chain::viewOrbital;
        case 3:  return chain::sculpt;
        case 4:  return chain::viewJam;
        default: return chain::viewScope;
    }
}

void ClodScopeEditor::setMainView (int view)
{
    mainViewAttachment->setValueAsCompleteGesture ((float) view);
    applyMainView (view);
}

void ClodScopeEditor::applyMainView (int view)
{
    view = juce::jlimit (0, numViews - 1, view);
    if (view <= 3)   // the last real view: a "Follow my view" Jam Sesh tile shows it while you're on the Jam page
        proc.apvts.state.setProperty ("jamLastView", view, nullptr);
    for (int i = 0; i < numViews; ++i)
        viewButtons[i].setToggleState (i == view, juce::dontSendNotification);
    if (view == currentView)
        return;

    currentView = view;
    scope.setVisible (view == 0);
    if (view == 1 || view == 2)
        ensureScene();
    if (view == 3)
        ensureSculptView();
    if (sculptView != nullptr)
        sculptView->setVisible (view == 3);
    if (view == 4)
        ensureJamView();
    if (jamView != nullptr)
        jamView->setVisible (view == 4);
    updateSceneVisibility();

    const char* chipNames[] = { "2D SCOPE", "3D", "ORBITAL", "SCULPTOR", "JAM SESH" };
    strip.setViewName (chipNames[view]);

    // open the controls that belong to the view (the user can still pick any module)
    if (! initialising)
    {
        if (view != 0)
            selectPage (pageForView (view));
        else if (chain::isViewPage (currentPage))
            selectPage (chain::viewScope);
    }
    updateHeaderAction();
    inspector.repaint();
}

void ClodScopeEditor::selectPage (int page)
{
    page = juce::jlimit (0, (int) chain::numPages - 1, page);
    if (page != currentPage)
    {
        if (currentPage >= 0)
            viewports[(size_t) currentPage]->setVisible (false);
        currentPage = page;

        auto& vp = *viewports[(size_t) page];
        if (! tooltipsApplied[(size_t) page])
        {
            applyTooltips (vp.panel);
            tooltipsApplied[(size_t) page] = true;
        }
        header.setPage (page);
        strip.setSelected (page, chain::isViewPage (page));
        proc.apvts.state.setProperty ("uiModule", chain::info (page).key, nullptr);
        layoutRightColumn();
        vp.setVisible (true);
        vp.fitPanel();
        updateHeaderAction();
        inspector.repaint();
    }

    // the sculptor's panel and its main view belong together
    if (page == chain::sculpt && currentView != 3 && ! initialising)
        setMainView (3);
}

void ClodScopeEditor::stepPage (int delta)
{
    if (currentPage >= 0 && currentPage < chain::numChain)
        selectPage (juce::jlimit (0, (int) chain::numChain - 1, currentPage + delta));
}

void ClodScopeEditor::updateHeaderAction()
{
    if (currentPage == chain::sculpt && currentView != 3)
        header.setAction ("Show Wave Sculptor", [this] { setMainView (3); });
    else if (currentPage == chain::synth)
        header.setAction ("Edit the wave", [this] { selectPage (chain::sculpt); });
    else if (currentPage == chain::output)
        header.setAction ("Presets...", [this] { showPresetBrowser(); });
    else
        header.setAction ({}, nullptr);
}

void ClodScopeEditor::ensureScene()
{
    if (scene != nullptr)
        return;
    scene = std::make_unique<Scene3D>();
    addChildComponent (*scene, 0);   // behind everything else
    scene->setBounds (mainArea);
}

void ClodScopeEditor::ensureJamView()
{
    if (jamView != nullptr)
        return;
    jamView = std::make_unique<JamView> (proc);
    addChildComponent (*jamView, 0);
    jamView->setBounds (mainArea);
}

void ClodScopeEditor::ensureSculptView()
{
    if (sculptView != nullptr)
        return;
    sculptView = std::make_unique<SculptView> (proc, &inspector);
    addChildComponent (*sculptView, 0);
    sculptView->setBounds (mainArea);
    applyTooltips (*sculptView);
}

void ClodScopeEditor::updateSceneVisibility()
{
    // the 3D view hides while an overlay covers it, rather than rendering every frame underneath
    const bool overlay = guide.isVisible() || presetBrowser.isVisible();
    if (scene != nullptr)
        scene->setVisible ((currentView == 1 || currentView == 2) && ! overlay);
}

void ClodScopeEditor::showPresetBrowser()
{
    hideGuide();
    presetBrowser.setBounds (getLocalBounds());
    presetBrowser.refresh();
    presetBrowser.setVisible (true);
    presetBrowser.toFront (true);
    updateSceneVisibility();
}

void ClodScopeEditor::hidePresetBrowser()
{
    if (! presetBrowser.isVisible())
        return;
    presetBrowser.setVisible (false);
    updateSceneVisibility();
}

void ClodScopeEditor::showGuide()
{
    hidePresetBrowser();
    guide.setVisible (true);
    guide.toFront (true);
    guide.grabKeyboardFocus();
    updateSceneVisibility();
}

void ClodScopeEditor::hideGuide()
{
    if (! guide.isVisible())
        return;
    guide.setVisible (false);
    updateSceneVisibility();
}

void ClodScopeEditor::themeChanged()
{
    lnf.applyTheme();
    Themes::refreshTree (*this);
    sendLookAndFeelChange();   // components that cache colours rebuild them in lookAndFeelChanged()
    repaint();
}

void ClodScopeEditor::parentHierarchyChanged()
{
    juce::AudioProcessorEditor::parentHierarchyChanged();
    // called as the host's window gets its peer, before the first paint
    streamsafe::useSoftwareRenderer (*this);
}

bool ClodScopeEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        if (presetBrowser.isVisible())  { hidePresetBrowser(); return true; }
        if (guide.isVisible())          { hideGuide(); return true; }
    }
    return false;
}

void ClodScopeEditor::applyTooltips (juce::Component& c)
{
    auto tipFor = [this] (const juce::String& id)
    {
        auto* h = inspector.find (id);
        return h != nullptr ? h->summary : juce::String();
    };
    auto setIfEmpty = [] (juce::SettableTooltipClient& t, const juce::String& tip)
    {
        if (t.getTooltip().isEmpty() && tip.isNotEmpty())
            t.setTooltip (tip);
    };

    for (auto* child : c.getChildren())
    {
        if (auto* k = dynamic_cast<ParamKnob*> (child))               setIfEmpty (k->slider, tipFor (k->id));
        else if (auto* ch = dynamic_cast<ParamChoice*> (child))       setIfEmpty (ch->box, tipFor (ch->id));
        else if (auto* t = dynamic_cast<ParamToggle*> (child))        setIfEmpty (t->button, tipFor (t->id));
        else if (auto* pb = dynamic_cast<ParamPowerButton*> (child))  setIfEmpty (pb->button, tipFor (pb->id));
        else if (auto* sg = dynamic_cast<ParamSegmented*> (child))
        {
            for (auto* b : sg->buttons)
                setIfEmpty (*b, tipFor (sg->id));
        }
        else
            applyTooltips (*child);
    }
}

//==============================================================================
void ClodScopeEditor::timerCallback()
{
    streamsafe::useSoftwareRendererEverywhere();   // windows JUCE opened by itself

    if ((currentView == 1 || currentView == 2) && scene != nullptr && scene->isVisible() && isShowing())
    {
        if (currentView == 1)
            views3D.produce (*scene);
        else
            orbitalView.produce (*scene);
    }

    if (++tick % 4 == 0)   // 15 Hz: meters and chip states
    {
        updateMeters();
        strip.refresh();
        repaint (readoutArea);
    }

    if (tick % 12 == 0)    // 5 Hz housekeeping
    {
        slitPanel.updateEnablement();
        if (currentPage >= 0)
            viewports[(size_t) currentPage]->fitPanel();
        header.repaint();
        updateHeaderAction();
        if (currentPage == chain::output)
            outputPanel.repaint();
    }
}

void ClodScopeEditor::updateMeters()
{
    auto peakOf = [this] (const ScopeBuffer& b)
    {
        const int n = (int) readL.size();
        b.copyLatest (readL.data(), readR.data(), n);
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
            peak = juce::jmax (peak, std::abs (readL[(size_t) i]), std::abs (readR[(size_t) i]));
        return juce::Decibels::gainToDecibels (peak, -100.0f);
    };
    // fast attack, ~20 dB/s fall so peaks stay readable
    inPeakDb = juce::jmax (peakOf (proc.inputScope), inPeakDb - 1.4f);
    outPeakDb = juce::jmax (peakOf (proc.outputScope), outPeakDb - 1.4f);
}

float ClodScopeEditor::param (const char* id) const
{
    auto* v = proc.apvts.getRawParameterValue (id);
    return v != nullptr ? v->load() : 0.0f;
}

juce::String ClodScopeEditor::paramText (const char* id) const
{
    auto* p = proc.apvts.getParameter (id);
    return p != nullptr ? p->getCurrentValueAsText() : juce::String();
}

juce::String ClodScopeEditor::chipStatus (int page) const
{
    auto& s = proc.apvts;
    if (page == chain::input)   return dbString (inPeakDb) + " dB";
    if (page == chain::output)  return dbString (outPeakDb) + " dB";
    if (page == chain::viewScope)
        return currentView == 3 ? "edits sound" : "picture";
    if (page == chain::sculpt)  return chain::isActive (s, chain::sculpt) ? "playing" : "synth off";

    if (! chain::isActive (s, page))
        return "off";

    if (page == chain::synth)
    {
        const auto t = proc.synth.tracker();
        if (t.chordMode && t.numNotes > 0)
            return chordname::symbol (t.notes.data(), t.numNotes);
        if (! t.chordMode && t.voiced && t.midiNote >= 0)
            return juce::MidiMessage::getMidiNoteName (t.midiNote, true, true, 4);
        if (const int v = proc.synth.activeVoices(); v > 0)
            return juce::String (v) + (v == 1 ? " voice" : " voices");
        return "listening";
    }
    if (page == chain::comp || page == chain::limiter)
    {
        const float gr = juce::jlimit (0.0f, 60.0f, std::abs (proc.fx.meterDb ((FxRack::Module) chain::fxModule (page))));
        return "GR " + juce::String (gr, 1);
    }
    return "on";
}

//==============================================================================
juce::String ClodScopeEditor::describeScope() const
{
    juce::String s;
    s << "## 2D scope (picture only)\n";
    s << "Showing the " << paramText ("source") << " signal, channels: " << paramText ("view") << "\n";
    const int view = (int) param ("view");
    if (view == 5)
    {
        s << "XY: x = left sample, y = right sample, newest " << paramText ("time") << " (max 8192 samples)\n";
    }
    else
    {
        s << "Window " << paramText ("time") << " across 10 divisions = "
          << juce::String (param ("time") / 10.0f, 2) << " ms/div\n";
        const int trig = (int) param ("trigMode");
        if (trig == 0)
            s << "Trigger: free run (newest audio at the right edge)\n";
        else
            s << "Trigger: " << paramText ("trigMode") << " through level " << paramText ("trigLevel")
              << ", placed 10% into the window\n";
    }
    s << "Vertical: full scale x " << paramText ("zoom") << " (x1 = 90% of the height)\n";
    s << "Persistence " << paramText ("persist") << " of the previous frame kept each frame";
    if (isOn ("freeze"))
        s << "\nFROZEN: no new audio is captured";
    if (isOn ("spectrum"))
        s << "\nSpectrum: 2048-point FFT of the mid signal, 0 dB = full-scale sine";
    return s;
}

juce::String ClodScopeEditor::describeInput() const
{
    juce::StringArray active;
    if (std::abs (param ("inGain")) > 0.05f)   active.add ("Input gain = " + paramText ("inGain"));
    if (param ("gate") > -99.9f)               active.add ("Noise gate threshold = " + paramText ("gate"));
    if (std::abs (param ("dc")) > 0.0005f)     active.add ("DC offset = " + paramText ("dc"));
    if (isOn ("invert"))                       active.add ("Phase inverted (y = -x)");
    if (param ("drive") > 0.0f)                active.add ("Drive " + paramText ("drive") + " into " + paramText ("shape")
                                                           + " (k = " + juce::String (1.0f + 30.0f * std::pow (param ("drive") * 0.01f, 2.0f), 2) + ")");
    if (param ("bits") < 23.5f)                active.add ("Bit depth = " + paramText ("bits"));
    if (param ("downsample") > 1.5f)           active.add ("Downsample = " + paramText ("downsample"));
    if (param ("hpf") > 20.5f)                 active.add ("High-pass at " + paramText ("hpf"));
    if (param ("lpf") < 19990.0f)              active.add ("Low-pass at " + paramText ("lpf"));

    juce::String s ("## Input stage - active controls\n");
    if (active.isEmpty())
        s << "Everything is neutral: the input passes this stage unchanged.";
    else
        for (auto& a : active)
            s << "- " << a << "\n";
    s << "\nInput peak (raw, before gain): " << dbString (inPeakDb) << " dB";
    return s.trimEnd();
}

juce::String ClodScopeEditor::describeSlit() const
{
    juce::String s ("## Double slit\n");
    if (! isOn ("dsOn"))
        return s + "Off - the sound passes unchanged. Switch it on with the power button.";
    s << paramText ("dsSizing") << ", ear model " << paramText ("dsEar") << "\n"
      << "Ear " << juce::String (proc.doubleSlit.earWidthM.load(), 1) << " m wide, arrivals spread over "
      << juce::String (proc.doubleSlit.spreadMs.load(), 1) << " ms\n"
      << "Slit mix = " << paramText ("dsMix");
    return s;
}

juce::String ClodScopeEditor::describeOutput() const
{
    juce::String s ("## Signal chain - modules switched on\n");
    juce::StringArray on;
    for (int p = 0; p < chain::numChain; ++p)
        if (chain::info (p).enableId.isNotEmpty() && chain::isActive (proc.apvts, p))
            on.add (chain::info (p).title);
    s << (on.isEmpty() ? juce::String ("None - only the input stage and the output controls shape the sound.")
                       : on.joinIntoString (" -> "));
    s << "\n## Output\n";
    s << "Dry/Wet = " << paramText ("mix") << " (dry = raw input, delayed to match the processed path)\n";
    s << "Output gain = " << paramText ("outGain") << "\n";
    s << "Output peak = " << dbString (outPeakDb) << " dB";
    return s;
}

juce::String ClodScopeEditor::liveMappingText() const
{
    const int page = currentPage;
    juce::String s;

    if (page >= 0 && page < chain::numChain && ! chain::isActive (proc.apvts, page))
        s << (page == chain::sculpt ? "The SYNTH layer is off, so this wave is not heard. Switch SYNTH on to play it.\n"
                                    : "This module is OFF and passes audio unchanged. Click its power button to hear it.\n");

    switch (page)
    {
        case chain::input:       s << describeInput(); break;
        case chain::synth:       s << "## Synth layer\n" << proc.synth.describeMapping(); break;
        case chain::sculpt:      s << "## Wave Sculptor\n" << proc.sculpt.describeMapping(); break;
        case chain::equation:
        {
            const auto eqText = proc.soundEq.describeMapping();
            s << "## Wave equation\n" << (eqText.isNotEmpty() ? eqText : juce::String ("(neutral)"));
            break;
        }
        case chain::slit:        s << describeSlit(); break;
        case chain::output:      s << describeOutput(); break;
        case chain::viewScope:   s << describeScope(); break;
        case chain::view3D:      s << "## 3D view (picture only)\n" << views3D.describeMapping(); break;
        case chain::viewOrbital: s << "## Orbital view (picture only)\n" << orbitalView.describeMapping(); break;
        default:
            if (chain::fxModule (page) >= 0)
                s << "## FX rack\n" << proc.fx.describeMapping();
            break;
    }
    return s.trimEnd();
}

//==============================================================================
void ClodScopeEditor::paintLogo (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto icon = area.removeFromLeft (30).withSizeKeepingCentre (28, 28).toFloat();
    if (Surface::plain())
    {
        g.setColour (Palette::viewBg);
        g.fillRoundedRectangle (icon, 6.0f);
        g.setColour (Palette::panelEdge.brighter (0.2f));
        g.drawRoundedRectangle (icon.reduced (0.5f), 6.0f, 1.0f);
    }
    else
    {
        Surface::sunken (g, icon, juce::jmin (6.0f * Theme::style().controlRadius / 5.0f, 14.0f), false, Accent::input);
    }

    // a little wave whose colour runs through the module accents
    juce::Path wave;
    for (int i = 0; i <= 40; ++i)
    {
        const float t = (float) i / 40.0f;
        const float x = icon.getX() + 4.0f + t * (icon.getWidth() - 8.0f);
        const float y = icon.getCentreY() - std::sin (t * juce::MathConstants<float>::twoPi * 1.5f) * 7.0f * (1.0f - 0.4f * t);
        if (i == 0) wave.startNewSubPath (x, y); else wave.lineTo (x, y);
    }
    juce::ColourGradient grad (Accent::input, icon.getX(), 0.0f, Accent::fx, icon.getRight(), 0.0f, false);
    grad.addColour (0.5, Accent::synth);
    g.setGradientFill (grad);
    g.strokePath (wave, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    area.removeFromLeft (10);
    g.setColour (Palette::text);
    g.setFont (Theme::font (17.0f, true));
    // fitted: some themes' fonts are wider than Consolas. The app shows its own name.
    const juce::String name = appBar != nullptr ? juce::String (CLOD_APP_NAME).toUpperCase() : juce::String ("CLOD SCOPE");
    g.drawFittedText (name, area.removeFromTop (area.getHeight() / 2 + 3), juce::Justification::bottomLeft, 1, 0.7f);
    g.setColour (Palette::dim);
    g.setFont (Theme::font (9.5f, true));
    g.drawFittedText ("SOUND KIT", area, juce::Justification::topLeft, 1, 0.7f);
}

void ClodScopeEditor::paintReadouts (juce::Graphics& g)
{
    using namespace Palette;
    auto r = readoutArea;
    auto noteArea = r.removeFromRight (84);
    r.removeFromRight (10);

    auto meterRow = [&g] (juce::Rectangle<int> row, const char* label, float db, juce::Colour c)
    {
        g.setFont (Theme::font (9.5f, true));
        g.setColour (dim);
        g.drawText (label, row.removeFromLeft (28), juce::Justification::centredLeft);
        g.setFont (Theme::font (10.5f));
        g.setColour (db > -0.3f ? pink : text);
        g.drawText (dbString (db), row.removeFromRight (40), juce::Justification::centredRight);
        row.removeFromRight (6);
        const auto bar = row.withSizeKeepingCentre (row.getWidth(), 5).toFloat();
        g.setColour (viewBg);
        g.fillRoundedRectangle (bar, 2.5f);
        const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        if (frac > 0.0f)
        {
            g.setColour (db > -0.3f ? pink : db > -6.0f ? amber : c);
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (2.0f, bar.getWidth() * frac)), 2.5f);
        }
    };
    meterRow (r.removeFromTop (r.getHeight() / 2), "IN", inPeakDb, Accent::input);
    meterRow (r, "OUT", outPeakDb, Accent::master);

    // tracked note of the input (what the synth layer would play)
    if (Surface::plain())
    {
        g.setColour (panel);
        g.fillRoundedRectangle (noteArea.toFloat(), 6.0f);
        g.setColour (panelEdge);
        g.drawRoundedRectangle (noteArea.toFloat().reduced (0.5f), 6.0f, 1.0f);
    }
    else
    {
        Surface::sunken (g, noteArea.toFloat(), juce::jmin (6.0f * Theme::style().controlRadius / 5.0f, 12.0f), false, Accent::synth);
    }
    auto inner = noteArea.reduced (8, 3);
    const auto t = proc.synth.tracker();
    g.setFont (Theme::font (9.0f, true));
    g.setColour (dim);
    g.drawText (t.chordMode ? "CHORD" : "NOTE", inner.removeFromTop (11), juce::Justification::centredLeft);

    if (t.chordMode)
    {
        const bool any = t.numNotes > 0;
        g.setColour (any ? Accent::synth : dim);
        g.setFont (Theme::font (any ? 14.0f : 13.0f, true));
        g.drawFittedText (any ? chordname::symbol (t.notes.data(), t.numNotes) : juce::String ("--"),
                          inner, juce::Justification::centredLeft, 1, 0.6f);
    }
    else if (t.voiced && t.midiNote >= 0)
    {
        g.setColour (Accent::synth);
        g.setFont (Theme::font (14.0f, true));
        const auto name = juce::MidiMessage::getMidiNoteName (t.midiNote, true, true, 4);
        g.drawText (name, inner, juce::Justification::centredLeft);
        g.setFont (Theme::font (10.0f));
        g.setColour (text.withAlpha (0.8f));
        const int c = juce::roundToInt (t.cents);
        g.drawText ((c >= 0 ? "+" : "") + juce::String (c) + "c", inner, juce::Justification::centredRight);
    }
    else
    {
        g.setColour (dim);
        g.setFont (Theme::font (13.0f, true));
        g.drawText ("--", inner, juce::Justification::centredLeft);
    }
}

void ClodScopeEditor::paint (juce::Graphics& g)
{
    using namespace Palette;
    g.fillAll (bg);

    if (Surface::plain())
    {
        g.setGradientFill (juce::ColourGradient (bg.interpolatedWith (panel, 0.85f), 0.0f, (float) topBar.getY(),
                                                 bg, 0.0f, (float) topBar.getBottom(), false));
        g.fillRect (topBar);
        g.setColour (panelEdge);
        g.drawHorizontalLine (topBar.getBottom() - 1, 0.0f, (float) getWidth());

        if (! appBarArea.isEmpty())
        {
            g.setColour (bg.interpolatedWith (panel, 0.5f));
            g.fillRect (appBarArea);
        }

        g.setColour (bg.interpolatedWith (panel, 0.38f));
        g.fillRect (stripArea);
        g.setColour (panelEdge);
        g.drawHorizontalLine (stripArea.getBottom() - 1, 0.0f, (float) getWidth());
    }
    else
    {
        // the material's own background: a texture, the main view set into a frame or well, and only a
        // faint line under the top bar (none on ceramic: the trays and wells give the shape)
        Surface::backdrop (g, getLocalBounds());
        if (! appBarArea.isEmpty())
        {
            g.setColour (bg.interpolatedWith (panel, 0.35f).withAlpha (0.8f));
            g.fillRect (appBarArea);
        }
        if (Theme::style().material != Theme::Material::ceramic)
        {
            g.setColour (panelEdge.withAlpha (0.7f));
            g.drawHorizontalLine (topBar.getBottom() - 1, 0.0f, (float) getWidth());
        }
        Surface::frame (g, mainArea.toFloat());
    }

    paintLogo (g, logoArea);
    paintReadouts (g);
}

void ClodScopeEditor::resized()
{
    auto r = getLocalBounds();
    topBar = r.removeFromTop (52);
    appBarArea = appBar != nullptr ? r.removeFromTop (StandaloneBar::idealHeight) : juce::Rectangle<int>();
    if (appBar != nullptr)
        appBar->setBounds (appBarArea);
    stripArea = r.removeFromTop (ChainStrip::idealHeight + 10);

    auto bar = topBar.reduced (16, 10);
    logoArea = bar.removeFromLeft (150);
    bar.removeFromLeft (14);
    readoutArea = bar.removeFromRight (236);
    bar.removeFromRight (14);
    themePicker.setBounds (bar.removeFromRight (ThemePicker::idealWidth));
    bar.removeFromRight (6);
    guideButton.setBounds (bar.removeFromRight (32));
    bar.removeFromRight (8);

    int widths[numViews], total = 0;
    for (int i = 0; i < numViews; ++i)
    {
        widths[i] = juce::GlyphArrangement::getStringWidthInt (Theme::font (12.5f, true), viewButtons[i].getButtonText()) + 22;
        total += widths[i];
    }
    viewSwitch.setBounds (bar.removeFromRight (total));
    for (int i = 0, x = 0; i < numViews; x += widths[i], ++i)
        viewButtons[i].setBounds (x, 0, widths[i], viewSwitch.getHeight());
    bar.removeFromRight (16);
    presetBar.setBounds (bar.removeFromLeft (juce::jmin (juce::jmax (0, presetBar.idealWidth()), bar.getWidth())));

    strip.setBounds (stripArea.reduced (12, 3));

    r = r.reduced (12, 0).withTrimmedTop (10).withTrimmedBottom (12);
    const int rightW = juce::jlimit (430, 560, (int) ((float) getWidth() * 0.34f));
    rightArea = r.removeFromRight (rightW);
    r.removeFromRight (12);

    mainArea = r;
    scope.setBounds (mainArea);
    if (scene != nullptr)
        scene->setBounds (mainArea);
    if (sculptView != nullptr)
        sculptView->setBounds (mainArea);
    if (jamView != nullptr)
        jamView->setBounds (mainArea);

    layoutRightColumn();

    guide.setBounds (getLocalBounds());
    presetBrowser.setBounds (getLocalBounds());
}

void ClodScopeEditor::layoutRightColumn()
{
    auto r = rightArea;
    header.setBounds (r.removeFromTop (ModuleHeader::idealHeight));
    r.removeFromTop (6);

    const int maxH = juce::jmax (inspectorCollapsedH, r.getHeight() - 170);
    const int autoH = juce::jlimit (210, 340, (int) ((float) r.getHeight() * 0.36f));
    const int h = juce::jlimit (inspectorCollapsedH, maxH, inspectorH < 0 ? autoH : inspectorH);
    inspector.setBounds (r.removeFromBottom (h));
    split.setBounds (r.removeFromBottom (12));
    panelArea = r;

    if (currentPage >= 0)
        viewports[(size_t) currentPage]->setBounds (panelArea);
}

void ClodScopeEditor::setInspectorHeight (int h, bool store)
{
    inspectorH = juce::jmax (inspectorCollapsedH, h);
    layoutRightColumn();
    inspectorH = inspector.getHeight();   // what actually fitted
    if (store)
        proc.apvts.state.setProperty ("uiInspectorH", inspectorH, nullptr);
}
