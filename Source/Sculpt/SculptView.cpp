#include "SculptView.h"
#include "SculptTools.h"
#include "../Synth/ChordName.h"

namespace
{
    using namespace sculpt_tools;

    void styleButton (juce::TextButton& b, bool toggles)
    {
        b.setColour (juce::TextButton::buttonColourId, Palette::panel.brighter (0.06f));
        b.setColour (juce::TextButton::buttonOnColourId, Accent::sculpt.withAlpha (0.42f));
        b.setColour (juce::TextButton::textColourOffId, Palette::text);
        b.setColour (juce::TextButton::textColourOnId, Palette::bright);
        b.setClickingTogglesState (toggles);
    }

    // "MORPH [=====    35 %]" - the scMorph macro as a compact toolbar bar
    class MorphBox : public juce::Component
    {
    public:
        explicit MorphBox (juce::AudioProcessorValueTreeState& s)
        {
            label.setText ("MORPH", juce::dontSendNotification);
            label.setFont (Theme::font (10.5f, true));
            label.setColour (juce::Label::textColourId, Palette::dim);
            label.setJustificationType (juce::Justification::centredRight);
            label.setInterceptsMouseClicks (false, false);
            slider.setSliderStyle (juce::Slider::LinearBar);
            slider.setColour (juce::Slider::trackColourId, Accent::sculpt.withAlpha (0.45f));
            slider.setColour (juce::Slider::backgroundColourId, Palette::viewBg);
            slider.setColour (juce::Slider::textBoxTextColourId, Palette::text);
            slider.setColour (juce::Slider::textBoxOutlineColourId, Palette::panelEdge);
            if (s.getParameter ("scMorph") != nullptr)
                attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (s, "scMorph", slider);
            addAndMakeVisible (label);
            addAndMakeVisible (slider);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            label.setBounds (r.removeFromLeft (46));
            r.removeFromLeft (4);
            slider.setBounds (r);
        }

        juce::Label label;
        juce::Slider slider;

    private:
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    float valueAt (const Cycle& c, float frac)
    {
        float pos = frac * (float) cycleLen;
        pos -= std::floor (pos / (float) cycleLen) * (float) cycleLen;
        const int i0 = juce::jlimit (0, cycleLen - 1, (int) pos);
        const int i1 = (i0 + 1) % cycleLen;
        const float fr = pos - (float) i0;
        return c[(size_t) i0] + (c[(size_t) i1] - c[(size_t) i0]) * fr;
    }

    // DC-free, 64-harmonic version of a unit Gaussian bump of width sigma (cycles): cosine
    // coefficient of harmonic k and the bump's resulting height at its centre
    float bumpCoeff (float sigma, int k)
    {
        return 2.0f * sigma * std::sqrt (twoPi) * std::exp (-2.0f * pi * pi * sigma * sigma * (float) (k * k));
    }

    float bumpHeight (float sigma)
    {
        float r = 0.0f;
        for (int k = 1; k <= maxPartials; ++k)
            r += bumpCoeff (sigma, k);
        return juce::jmax (1.0e-4f, r);
    }

    juce::String noteText (const TrackerSnapshot& t)
    {
        if (t.chordMode)
            return t.numNotes > 0 ? "chord " + chordname::symbol (t.notes.data(), t.numNotes) + "  ("
                                        + chordname::noteList (t.notes.data(), t.numNotes) + ")"
                                  : juce::String ("no chord");
        if (t.f0Hz <= 0.0f || t.midiNote < 0)
            return "no pitch";
        const int c = (int) std::round (t.cents);
        return juce::MidiMessage::getMidiNoteName (t.midiNote, true, true, 4)
             + (c == 0 ? juce::String() : (c > 0 ? " +" : " ") + juce::String (c) + " ct")
             + "  " + juce::String (t.f0Hz, t.f0Hz < 100.0f ? 1 : 0) + " Hz";
    }
}

//==============================================================================
struct SculptView::Impl : private juce::Timer
{
    struct Canvas : public juce::Component
    {
        explicit Canvas (Impl& i) : o (i) { setWantsKeyboardFocus (true); }
        void paint (juce::Graphics& g) override                       { o.paintCanvas (g); }
        void resized() override                                       { o.layoutCanvas(); }
        void mouseMove (const juce::MouseEvent& e) override            { o.onMove (e); }
        void mouseExit (const juce::MouseEvent&) override              { o.onExit(); }
        void mouseDown (const juce::MouseEvent& e) override            { o.onDown (e); }
        void mouseDrag (const juce::MouseEvent& e) override            { o.onDrag (e); }
        void mouseUp (const juce::MouseEvent& e) override              { o.onUp (e); }
        void mouseDoubleClick (const juce::MouseEvent& e) override     { o.onDoubleClick (e); }
        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            if (! o.onWheel (e, w))
                juce::Component::mouseWheelMove (e, w);
        }
        bool keyPressed (const juce::KeyPress& k) override             { return o.onKey (k); }
        Impl& o;
    };

    enum class Region { none, banner, wave, bars, phases, info };
    enum class Drag { none, bend, draw, bars, phases };

    Impl (SculptView& v, ClodScopeProcessor& p, HelpTarget* h)
        : view (v), proc (p), model (p.sculpt), help (h), canvas (*this), morphBox (p.apvts)
    {
        view.addAndMakeVisible (canvas);

        fillWaveMenu (waveBox);
        waveBox.setColour (juce::ComboBox::backgroundColourId, Palette::panel.brighter (0.06f));
        waveBox.setColour (juce::ComboBox::textColourId, Palette::text);
        waveBox.setColour (juce::ComboBox::outlineColourId, Palette::panelEdge);
        waveBox.onChange = [this]
        {
            const int idx = waveBox.getSelectedId() - 1;
            if (idx >= 0)
            {
                model.setTable (active, SculptModel::makeWave (idx), true);
                flash ("Loaded " + SculptModel::waveNames()[idx] + " into table " + tableName());
            }
            waveBox.setSelectedId (0, juce::dontSendNotification);
        };

        for (auto* b : { &aBtn, &bBtn })
        {
            styleButton (*b, true);
            b->setRadioGroupId (0x5c01);
        }
        aBtn.setConnectedEdges (juce::Button::ConnectedOnRight);
        bBtn.setConnectedEdges (juce::Button::ConnectedOnLeft);
        aBtn.onClick = [this] { if (aBtn.getToggleState()) setActiveTable (proc.apvts, 0); refresh(); };
        bBtn.onClick = [this] { if (bBtn.getToggleState()) setActiveTable (proc.apvts, 1); refresh(); };

        for (auto* b : { &bendBtn, &drawBtn })
        {
            styleButton (*b, true);
            b->setRadioGroupId (0x5c02);
        }
        bendBtn.setConnectedEdges (juce::Button::ConnectedOnRight);
        drawBtn.setConnectedEdges (juce::Button::ConnectedOnLeft);
        bendBtn.onClick = [this] { drawMode = drawBtn.getToggleState(); saveUi(); };
        drawBtn.onClick = [this] { drawMode = drawBtn.getToggleState(); saveUi(); };

        for (auto* b : { &copyBtn, &undoBtn, &redoBtn, &normBtn, &clearBtn, &randBtn, &smoothBtn, &oddBtn, &evenBtn, &invBtn, &captureBtn })
            styleButton (*b, false);
        captureBtn.setColour (juce::TextButton::buttonColourId, Accent::sculpt.withAlpha (0.22f));
        for (auto* b : { &compBtn, &synthOvBtn, &inputOvBtn, &dbBtn })
        {
            styleButton (*b, true);
            b->onClick = [this] { readToggles(); saveUi(); };
        }
        synthOvBtn.setColour (juce::TextButton::buttonOnColourId, Accent::synth.withAlpha (0.42f));
        inputOvBtn.setColour (juce::TextButton::buttonOnColourId, Accent::input.withAlpha (0.35f));

        copyBtn.onClick = [this]
        {
            model.setTable (1 - active, model.getTable (active), true);
            flash ("Copied table " + tableName() + " to table " + juce::String (active == 0 ? "B" : "A"));
        };
        undoBtn.onClick = [this] { flash (model.undo() ? "Undo" : "Nothing to undo"); };
        redoBtn.onClick = [this] { flash (model.redo() ? "Redo" : "Nothing to redo"); };
        normBtn.onClick   = [this] { applyTool (normalize (model.getTable (active)), "Normalized: loudest harmonic = 1"); };
        clearBtn.onClick  = [this] { applyTool (clearToSine(), "Cleared to a pure sine - add overtones with the bars"); };
        randBtn.onClick   = [this] { applyTool (randomize(), "Random spectrum"); };
        smoothBtn.onClick = [this] { applyTool (smooth (model.getTable (active)), "Smoothed (press again for more)"); };
        oddBtn.onClick    = [this] { applyTool (oddOnly (model.getTable (active)), "Even harmonics removed"); };
        evenBtn.onClick   = [this] { applyTool (evenOnly (model.getTable (active)), "Odd harmonics above the fundamental removed"); };
        invBtn.onClick    = [this] { applyTool (invertPhases (model.getTable (active)), "Phases inverted (cycle mirrored)"); };
        captureBtn.onClick = [this]
        {
            BaseTable t;
            juce::String msg;
            if (captureInput (proc, t, msg))
                model.setTable (active, t, true);
            flash (msg, 5000);
        };

        styleButton (turnOnBtn, false);
        turnOnBtn.setColour (juce::TextButton::buttonColourId, Accent::synth.withAlpha (0.5f));
        turnOnBtn.onClick = [this]
        {
            if (auto* prm = proc.apvts.getParameter ("synOn"))
            {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost (1.0f);
                prm->endChangeGesture();
            }
        };
        canvas.addChildComponent (turnOnBtn);
        hovers.add (new widgets_detail::HoverReporter (turnOnBtn, "sc.banner", help));

        // toolbar, in reading order; `true` = small gap before the item (a new group)
        const std::tuple<juce::Component*, int, bool, const char*> items[] = {
            { &waveBox, 150, false, "sc.factory" }, { &aBtn, 30, true, "sc.ab" }, { &bBtn, 30, false, "sc.ab" },
            { &copyBtn, 64, false, "sc.copy" }, { &morphBox, 170, true, "scMorph" },
            { &bendBtn, 50, true, "sc.mode" }, { &drawBtn, 50, false, "sc.mode" },
            { &undoBtn, 48, true, "sc.undo" }, { &redoBtn, 48, false, "sc.redo" },
            { &normBtn, 74, true, "sc.normalize" }, { &clearBtn, 48, false, "sc.clear" }, { &randBtn, 64, false, "sc.random" },
            { &smoothBtn, 60, false, "sc.smooth" }, { &oddBtn, 64, false, "sc.odd" }, { &evenBtn, 68, false, "sc.even" },
            { &invBtn, 78, false, "sc.invert" }, { &captureBtn, 104, true, "sc.capture" },
            { &compBtn, 84, true, "sc.components" }, { &synthOvBtn, 54, false, "sc.overlay" }, { &inputOvBtn, 54, false, "sc.overlay" },
            { &dbBtn, 40, false, "sc.dbbars" } };
        for (auto& [c, w, gap, id] : items)
        {
            toolItems.push_back ({ c, w, gap });
            view.addAndMakeVisible (*c);
            hovers.add (new widgets_detail::HoverReporter (*c, id, help));
        }

        loadUi();
        refresh();
        startTimerHz (30);
    }

    ~Impl() override { stopTimer(); }

    //==========================================================================
    juce::String tableName() const { return active == 0 ? "A" : "B"; }

    void flash (const juce::String& msg, int ms = 2500)
    {
        statusMsg = msg;
        statusUntil = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
        refresh();
    }

    void applyTool (const BaseTable& t, const juce::String& msg)
    {
        model.setTable (active, t, true);
        flash (msg + " (table " + tableName() + ")");
    }

    juce::ValueTree uiTree() { return proc.apvts.state.getOrCreateChildWithName ("SculptUI", nullptr); }

    void loadUi()
    {
        const auto t = proc.apvts.state.getChildWithName ("SculptUI");
        drawMode = (bool) t.getProperty ("draw", false);
        compBtn.setToggleState ((bool) t.getProperty ("components", false), juce::dontSendNotification);
        synthOvBtn.setToggleState ((bool) t.getProperty ("ovSynth", true), juce::dontSendNotification);
        inputOvBtn.setToggleState ((bool) t.getProperty ("ovInput", false), juce::dontSendNotification);
        dbBtn.setToggleState ((bool) t.getProperty ("dbBars", false), juce::dontSendNotification);
        bendWidth = juce::jlimit (0.01f, 0.25f, (float) t.getProperty ("bendWidth", 0.06f));
        (drawMode ? drawBtn : bendBtn).setToggleState (true, juce::dontSendNotification);
        readToggles();
    }

    void saveUi()
    {
        auto t = uiTree();
        t.setProperty ("draw", drawMode, nullptr);
        t.setProperty ("components", showComponents, nullptr);
        t.setProperty ("ovSynth", showSynth, nullptr);
        t.setProperty ("ovInput", showInput, nullptr);
        t.setProperty ("dbBars", dbBars, nullptr);
        t.setProperty ("bendWidth", bendWidth, nullptr);
    }

    void readToggles()
    {
        showComponents = compBtn.getToggleState();
        showSynth = synthOvBtn.getToggleState();
        showInput = inputOvBtn.getToggleState();
        dbBars = dbBtn.getToggleState();
        canvas.repaint();
    }

    float param (const char* id, float fallback) const
    {
        if (auto* v = proc.apvts.getRawParameterValue (id))
            return v->load();
        return fallback;
    }

    //==========================================================================
    void timerCallback() override
    {
        if (! view.isShowing())
            return;
        refresh();
    }

    // pulls the latest model + live data and repaints
    void refresh()
    {
        active = activeTable (proc.apvts);
        aBtn.setToggleState (active == 0, juce::dontSendNotification);
        bBtn.setToggleState (active == 1, juce::dontSendNotification);
        copyBtn.setButtonText (active == 0 ? "A -> B" : "B -> A");
        if (drawMode != drawBtn.getToggleState())
            (drawMode ? drawBtn : bendBtn).setToggleState (true, juce::dontSendNotification);

        model.copyDisplayPartials (finalSet);
        finalPeak = renderPartials (finalSet, finalWave);

        if (drag == Drag::bend)
            tableWave = gestureWave;          // frozen scale while the point is held
        else if (drag == Drag::draw)
        {
            tableWave = gestureWave;
            tablePeakShown = 1.0f;
        }
        else
        {
            table = model.getTable (active);
            tablePeak = renderTable (table, tableWave);
            tablePeakShown = tablePeak > 1.0e-6f ? tablePeak : 1.0f;
        }

        // show the table on its own only when the macros make it differ from what is heard
        float diff = 0.0f;
        if (finalPeak > 1.0e-6f)
            for (size_t i = 0; i < (size_t) cycleLen; i += 4)
                diff = juce::jmax (diff, std::abs (finalWave[i] / finalPeak - tableWave[i] / tablePeakShown));
        showTableLine = drag == Drag::bend || drag == Drag::draw || diff > 0.03f;

        // ghost ticks: final levels rescaled to the table's loudest harmonic
        float maxT = 0.0f, maxF = 0.0f;
        for (int i = 0; i < maxPartials; ++i)
        {
            maxT = juce::jmax (maxT, table.amp[(size_t) i]);
            maxF = juce::jmax (maxF, finalSet.amp[(size_t) i]);
        }
        ghostVisible = false;
        for (int i = 0; i < maxPartials; ++i)
        {
            ghost[(size_t) i] = maxF > 0.0f ? finalSet.amp[(size_t) i] / maxF * maxT : 0.0f;
            if (std::abs (ghost[(size_t) i] - table.amp[(size_t) i]) > 0.01f)
                ghostVisible = true;
        }

        // readouts
        trackerSnap = proc.synth.tracker();
        activePartials = 0;
        double e = 0.0, ek = 0.0;
        for (int i = 0; i < finalSet.count; ++i)
        {
            const double a = finalSet.amp[(size_t) i];
            if (a > 1.0e-4)
                ++activePartials;
            e += a * a;
            ek += a * a * (i + 1);
        }
        brightness = e > 0.0 ? (float) (ek / e) : 0.0f;
        double ms = 0.0;
        for (auto v : finalWave)
            ms += (double) v * v;
        const double rms = std::sqrt (ms / cycleLen);
        crestDb = rms > 1.0e-9 ? (float) (20.0 * std::log10 (finalPeak / rms)) : 0.0f;

        // live overlays, aligned to the sculpted wave's fundamental
        const double sr = proc.currentSampleRate.load();
        const float ref = finalSet.count > 0 ? finalSet.phase[0] : 0.0f;
        haveSynth = showSynth && liveCycle (proc.synthScope, sr, trackerSnap.synthF0Hz, ref, synthCycle, scratchL, scratchR);
        haveInput = showInput && liveCycle (proc.inputScope, sr, trackerSnap.f0Hz, ref, inputCycle, scratchL, scratchR);

        // banner
        const auto* synOn = proc.apvts.getRawParameterValue ("synOn");
        const bool off = synOn != nullptr && synOn->load() < 0.5f;
        if (off != bannerShown)
        {
            bannerShown = off;
            layoutCanvas();
        }
        canvas.repaint();
    }

    //==========================================================================
    int layoutToolbar (int width, bool apply)
    {
        const int rowH = 26, pad = 8;
        int x = pad, y = 6;
        for (auto& it : toolItems)
        {
            int w = juce::jmin (it.width, width - 2 * pad);
            if (it.gap && x > pad)
                x += 10;
            if (x + w > width - pad && x > pad)
            {
                x = pad;
                y += rowH + 4;
            }
            if (apply)
                it.comp->setBounds (x, y, w, rowH);
            x += w + 4;
        }
        return y + rowH + 6;
    }

    void layoutView()
    {
        auto r = view.getLocalBounds();
        toolbarArea = r.removeFromTop (layoutToolbar (r.getWidth(), true));
        canvas.setBounds (r);
        layoutCanvas();
    }

    void layoutCanvas()
    {
        auto r = canvas.getLocalBounds().reduced (8, 6);
        bannerArea = {};
        if (bannerShown)
        {
            bannerArea = r.removeFromTop (30);
            r.removeFromTop (6);
            turnOnBtn.setBounds (bannerArea.withTrimmedLeft (bannerArea.getWidth() - 92).reduced (4, 4));
        }
        turnOnBtn.setVisible (bannerShown);

        infoArea = r.removeFromBottom (20);
        r.removeFromBottom (4);
        phaseArea = r.removeFromBottom (juce::jlimit (24, 40, r.getHeight() / 12));
        r.removeFromBottom (4);
        barsArea = r.removeFromBottom (juce::jlimit (96, 280, (int) ((float) r.getHeight() * 0.38f)));
        r.removeFromBottom (6);
        waveArea = r;

        wavePlot = waveArea.reduced (14, 0).withTrimmedTop (26).withTrimmedBottom (18);
        barsPlot = barsArea.withTrimmedLeft (44).withTrimmedRight (10).withTrimmedTop (22).withTrimmedBottom (14);
        phasePlot = phaseArea.withTrimmedLeft (44).withTrimmedRight (10).reduced (0, 4);
        canvas.repaint();
    }

    //==========================================================================
    // coordinate helpers
    float waveScale() const   { return (float) wavePlot.getHeight() * 0.5f * 0.9f; }
    float waveCentre() const  { return (float) wavePlot.getCentreY(); }
    float fracFromX (float x) const { return juce::jlimit (0.0f, 0.9999f, (x - (float) wavePlot.getX()) / (float) juce::jmax (1, wavePlot.getWidth())); }
    float barWidth() const    { return (float) barsPlot.getWidth() / (float) maxPartials; }
    int barFromX (float x) const { return juce::jlimit (0, maxPartials - 1, (int) ((x - (float) barsPlot.getX()) / barWidth())); }

    float barHeightFrac (float amp) const
    {
        if (! dbBars)
            return juce::jlimit (0.0f, 1.0f, amp);
        if (amp <= 1.0e-6f)
            return 0.0f;
        return juce::jlimit (0.0f, 1.0f, (20.0f * std::log10 (amp) + 60.0f) / 60.0f);
    }

    float ampFromBarY (float y) const
    {
        const float v = juce::jlimit (0.0f, 1.0f, ((float) barsPlot.getBottom() - y) / (float) juce::jmax (1, barsPlot.getHeight()));
        if (! dbBars)
            return v < 0.004f ? 0.0f : v;
        return v < 0.01f ? 0.0f : std::pow (10.0f, (v * 60.0f - 60.0f) / 20.0f);
    }

    float phaseFromY (float y) const
    {
        const float half = (float) phasePlot.getHeight() * 0.5f;
        return juce::jlimit (-pi, pi, ((float) phasePlot.getCentreY() - y) / juce::jmax (1.0f, half) * pi);
    }

    Region regionAt (juce::Point<int> p) const
    {
        if (bannerArea.contains (p)) return Region::banner;
        if (waveArea.contains (p))   return Region::wave;
        if (barsArea.contains (p))   return Region::bars;
        if (phaseArea.contains (p))  return Region::phases;
        if (infoArea.contains (p))   return Region::info;
        return Region::none;
    }

    void setHelp (const juce::String& id)
    {
        if (id == helpId || help == nullptr)
            return;
        helpId = id;
        if (id.isEmpty())
            help->clearHelp();
        else
            help->showHelp (id);
    }

    //==========================================================================
    // mouse
    void onMove (const juce::MouseEvent& e)
    {
        hoverPos = e.position;
        const auto reg = regionAt (e.getPosition());
        hoverWave = reg == Region::wave;
        hoverBar = (reg == Region::bars || reg == Region::phases) && e.position.x >= (float) barsPlot.getX()
                       ? barFromX (e.position.x) : -1;
        switch (reg)
        {
            case Region::wave:   setHelp (drawMode ? "sc.draw" : "sc.bend"); break;
            case Region::bars:   setHelp ("sc.bars"); break;
            case Region::phases: setHelp ("sc.phases"); break;
            case Region::info:   setHelp ("sc.readout"); break;
            case Region::banner: setHelp ("sc.banner"); break;
            case Region::none:   default: setHelp ({}); break;
        }
        canvas.repaint();
    }

    void onExit()
    {
        hoverWave = false;
        hoverBar = -1;
        setHelp ({});
        canvas.repaint();
    }

    void onDown (const juce::MouseEvent& e)
    {
        canvas.grabKeyboardFocus();
        gestureFirst = true;
        gestureMoved = false;
        const auto reg = regionAt (e.getPosition());

        if (reg == Region::wave)
        {
            table = model.getTable (active);
            tablePeak = renderTable (table, tableWave);
            const float p0 = tablePeak > 1.0e-6f ? tablePeak : 1.0f;
            if (drawMode)
            {
                drag = Drag::draw;
                for (size_t i = 0; i < (size_t) cycleLen; ++i)
                    painted[i] = tableWave[i] / p0;
                lastPaintFrac = fracFromX (e.position.x);
                lastPaintVal = (waveCentre() - e.position.y) / waveScale();
                paintSegment (lastPaintFrac, lastPaintVal, lastPaintFrac, lastPaintVal);
                applyDraw();
            }
            else
            {
                drag = Drag::bend;
                gestureZ0 = toCoeffs (table);
                gestureP0 = p0;
                tablePeakShown = p0;
                gestureX0 = fracFromX (e.position.x);
                gestureDownY = e.position.y;
                gestureWave = tableWave;
            }
            return;
        }

        if (reg == Region::bars || reg == Region::phases)
        {
            if (e.getNumberOfClicks() > 1 || e.position.x < (float) barsPlot.getX())
                return;   // the double-click handler does the work
            drag = reg == Region::bars ? Drag::bars : Drag::phases;
            phaseEdit = drag == Drag::phases || e.mods.isPopupMenu() || e.mods.isAltDown();
            gestureTable = model.getTable (active);
            downBar = lastBar = barFromX (e.position.x);
            lastBarValue = barValue (e.position.y);
            setBar (downBar, lastBarValue);
            commitBars();
        }
    }

    float barValue (float y) const
    {
        if (phaseEdit)
            return drag == Drag::phases ? phaseFromY (y) : juce::jlimit (-pi, pi, (1.0f - 2.0f * ((y - (float) barsPlot.getY()) / (float) juce::jmax (1, barsPlot.getHeight()))) * pi);
        return ampFromBarY (y);
    }

    void setBar (int i, float v)
    {
        if (phaseEdit)
            gestureTable.phase[(size_t) i] = v;
        else
            gestureTable.amp[(size_t) i] = v;
    }

    void commitBars()
    {
        model.setTable (active, gestureTable, gestureFirst);
        gestureFirst = false;
        table = gestureTable;
        tablePeak = renderTable (table, tableWave);
        canvas.repaint();
    }

    void onDrag (const juce::MouseEvent& e)
    {
        hoverPos = e.position;
        if (e.getDistanceFromDragStart() > 2)
            gestureMoved = true;

        switch (drag)
        {
            case Drag::bend:
            {
                // relative pull: the grabbed point moves exactly by the mouse's vertical travel
                const float dv = -(e.position.y - gestureDownY) / waveScale() * gestureP0;
                const float D = dv / bumpHeight (bendWidth);
                Coeffs z = gestureZ0;
                for (int k = 1; k <= maxPartials; ++k)
                    z[(size_t) (k - 1)] += std::polar (D * bumpCoeff (bendWidth, k), pi * 0.5f - twoPi * (float) k * gestureX0);
                renderCoeffs (z, gestureWave);
                model.setTable (active, fromCoeffs (z), gestureFirst);
                gestureFirst = false;
                refresh();
                break;
            }
            case Drag::draw:
            {
                const float f = fracFromX (e.position.x);
                const float v = juce::jlimit (-1.1f, 1.1f, (waveCentre() - e.position.y) / waveScale());
                paintSegment (lastPaintFrac, lastPaintVal, f, v);
                lastPaintFrac = f;
                lastPaintVal = v;
                applyDraw();
                break;
            }
            case Drag::bars:
            case Drag::phases:
            {
                const int b = barFromX (e.position.x);
                const float v = barValue (e.position.y);
                if (e.mods.isShiftDown())
                {
                    const int lo = juce::jmin (lastBar, b), hi = juce::jmax (lastBar, b);
                    for (int i = lo; i <= hi; ++i)
                    {
                        const float t = hi == lo ? 1.0f : (float) (i - lastBar) / (float) (b - lastBar);
                        setBar (i, lastBarValue + (v - lastBarValue) * t);
                    }
                    lastBar = b;
                }
                else
                    setBar (downBar, v);
                lastBarValue = v;
                hoverBar = e.mods.isShiftDown() ? b : downBar;
                commitBars();
                break;
            }
            case Drag::none:
            default:
                break;
        }
    }

    void paintSegment (float f1, float v1, float f2, float v2)
    {
        int i1 = (int) std::round (f1 * (float) cycleLen), i2 = (int) std::round (f2 * (float) cycleLen);
        if (i1 > i2)
        {
            std::swap (i1, i2);
            std::swap (v1, v2);
        }
        for (int i = i1; i <= i2; ++i)
        {
            const float t = i2 == i1 ? 1.0f : (float) (i - i1) / (float) (i2 - i1);
            painted[(size_t) juce::jlimit (0, cycleLen - 1, i)] = v1 + (v2 - v1) * t;
        }
    }

    void applyDraw()
    {
        const auto z = analyse (painted);
        renderCoeffs (z, gestureWave);
        model.setTable (active, fromCoeffs (z), gestureFirst);
        gestureFirst = false;
        refresh();
    }

    void onUp (const juce::MouseEvent& e)
    {
        lastClickBar = (drag == Drag::bars || drag == Drag::phases) && ! gestureMoved ? downBar : -1;
        lastClickWasPhaseLane = drag == Drag::phases;
        drag = Drag::none;
        refresh();
        onMove (e);
    }

    void onDoubleClick (const juce::MouseEvent& e)
    {
        const auto reg = regionAt (e.getPosition());
        if ((reg != Region::bars && reg != Region::phases) || e.position.x < (float) barsPlot.getX())
            return;
        const int b = barFromX (e.position.x);
        const bool lane = reg == Region::phases;
        if (lastClickBar == b && lastClickWasPhaseLane == lane)
            model.undo();   // fold the first click's edit into this one undo step
        auto t = model.getTable (active);
        if (lane)
            t.phase[(size_t) b] = 0.0f;
        else
            t.amp[(size_t) b] = 0.0f;
        model.setTable (active, t, true);
        lastClickBar = -1;
        flash (lane ? "Phase of H" + juce::String (b + 1) + " reset to 0 deg" : "H" + juce::String (b + 1) + " zeroed");
    }

    bool onWheel (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
    {
        if (regionAt (e.getPosition()) != Region::wave || drawMode)
            return false;
        bendWidth = juce::jlimit (0.01f, 0.25f, bendWidth * std::exp ((w.deltaY != 0.0f ? w.deltaY : w.deltaX) * (w.isReversed ? -1.5f : 1.5f)));
        saveUi();
        flash ("Bend width " + juce::String (bendWidth, 3) + " cycle (" + juce::String ((int) std::round (bendWidth * 360.0f)) + " deg)", 1200);
        return true;
    }

    bool onKey (const juce::KeyPress& k)
    {
        const auto mods = k.getModifiers();
        if (! mods.isCommandDown())
            return false;
        if (k.getKeyCode() == 'Z' && ! mods.isShiftDown()) { flash (model.undo() ? "Undo" : "Nothing to undo"); return true; }
        if (k.getKeyCode() == 'Y' || (k.getKeyCode() == 'Z' && mods.isShiftDown())) { flash (model.redo() ? "Redo" : "Nothing to redo"); return true; }
        return false;
    }

    //==========================================================================
    // painting
    juce::Path cyclePath (const Cycle& c, float scale) const
    {
        juce::Path p;
        const float x0 = (float) wavePlot.getX(), w = (float) wavePlot.getWidth(), cy = waveCentre();
        const float lim = (float) wavePlot.getHeight();
        for (int j = 0; j <= cycleLen; ++j)
        {
            const float x = x0 + w * (float) j / (float) cycleLen;
            const float y = juce::jlimit (cy - lim, cy + lim, cy - c[(size_t) (j % cycleLen)] * scale);
            if (j == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        return p;
    }

    juce::Path partialPath (float ratio, float amp, float phase, float scale) const
    {
        juce::Path p;
        const float x0 = (float) wavePlot.getX(), w = (float) wavePlot.getWidth(), cy = waveCentre();
        const int n = juce::jlimit (48, 512, (int) (ratio * 14.0f));
        for (int j = 0; j <= n; ++j)
        {
            const float t = (float) j / (float) n;
            const float y = cy - amp * scale * std::sin (twoPi * ratio * t + phase);
            if (j == 0) p.startNewSubPath (x0 + w * t, y); else p.lineTo (x0 + w * t, y);
        }
        return p;
    }

    static void glow (juce::Graphics& g, const juce::Path& p, juce::Colour c, float core)
    {
        g.setColour (c.withAlpha (0.06f));
        g.strokePath (p, juce::PathStrokeType (core + 8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c.withAlpha (0.18f));
        g.strokePath (p, juce::PathStrokeType (core + 3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (core, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    static void viewBox (juce::Graphics& g, juce::Rectangle<int> r)
    {
        g.setColour (Palette::viewBg);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (Palette::panelEdge);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    void paintCanvas (juce::Graphics& g)
    {
        if (bannerShown)
            paintBanner (g);
        paintWave (g);
        paintBars (g);
        paintPhases (g);
        paintInfo (g);
    }

    void paintBanner (juce::Graphics& g)
    {
        const auto r = bannerArea.toFloat();
        g.setColour (Accent::synth.withAlpha (0.14f));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Accent::synth.withAlpha (0.6f));
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (Palette::text);
        g.setFont (Theme::font (12.5f, true));
        g.drawText ("Synth layer is off - you won't hear the sculpted wave", bannerArea.withTrimmedLeft (12).withTrimmedRight (100),
                    juce::Justification::centredLeft, true);
    }

    void paintWave (juce::Graphics& g)
    {
        using namespace Palette;
        viewBox (g, waveArea);
        const auto plot = wavePlot.toFloat();
        const float cy = waveCentre(), sc = waveScale();

        // grid: amplitude lines and quarter cycles
        for (float v : { -1.0f, -0.5f, 0.5f, 1.0f })
        {
            g.setColour (grid.withAlpha (std::abs (v) == 1.0f ? 0.9f : 0.6f));
            g.drawHorizontalLine ((int) std::round (cy - v * sc), plot.getX(), plot.getRight());
        }
        g.setColour (panelEdge.brighter (0.25f));
        g.drawHorizontalLine ((int) std::round (cy), plot.getX(), plot.getRight());
        g.setFont (Theme::font (10.0f));
        const char* deg[] = { "0", "90", "180", "270", "360 deg" };
        for (int q = 0; q <= 4; ++q)
        {
            const float x = plot.getX() + plot.getWidth() * (float) q * 0.25f;
            g.setColour (grid);
            g.drawVerticalLine ((int) std::round (x), plot.getY(), plot.getBottom());
            g.setColour (dim.withAlpha (0.8f));
            const int tw = q == 4 ? 60 : 30;
            const int tx = q == 0 ? (int) x : q == 4 ? (int) x - tw : (int) x - tw / 2;
            g.drawText (deg[q], tx, (int) plot.getBottom() + 2, tw, 14,
                        q == 0 ? juce::Justification::centredLeft : q == 4 ? juce::Justification::centredRight : juce::Justification::centred);
        }

        // header: what we are looking at + tracked note
        auto head = waveArea.reduced (12, 0).removeFromTop (24);
        g.setFont (Theme::font (12.0f, true));
        g.setColour (Accent::sculpt);
        g.drawText ("WAVE", head, juce::Justification::centredLeft);
        g.setColour (dim);
        g.setFont (Theme::font (11.0f));
        const float morph = param ("scMorph", 0.0f);
        juce::String sub = "editing table " + tableName() + (drawMode ? "  -  Draw: paint a new cycle" : "  -  Bend: drag a point, wheel = width");
        if ((active == 1 && morph < 0.5f) || (active == 0 && morph > 99.5f))
            sub = "editing table " + tableName() + "  -  not heard at morph " + juce::String ((int) std::round (morph)) + " % (raise MORPH)";
        // legend on the right of the header, the description gets what is left
        {
            const auto lf = Theme::font (10.0f);
            g.setFont (lf);
            int lx = head.getRight();
            auto addLegend = [&] (const juce::String& s, juce::Colour c)
            {
                const int w = juce::GlyphArrangement::getStringWidthInt (lf, s) + 22;
                lx -= w;
                g.setColour (c);
                g.fillRoundedRectangle ((float) lx + 4.0f, (float) head.getCentreY() - 1.5f, 10.0f, 3.0f, 1.5f);
                g.setColour (dim);
                g.drawText (s, lx + 17, head.getY(), w - 17, head.getHeight(), juce::Justification::centredLeft);
            };
            if (haveInput)     addLegend ("input", Accent::input);
            if (haveSynth)     addLegend ("synth live", Accent::synth);
            if (showTableLine) addLegend ("table " + tableName(), Palette::bright);
            addLegend ("you hear", Accent::sculpt);
            g.setFont (Theme::font (11.0f));
            g.setColour (dim);
            g.drawText (sub, head.withTrimmedLeft (46).withRight (lx - 8), juce::Justification::centredLeft, true);
        }

        g.saveState();
        g.reduceClipRegion (waveArea.reduced (2));

        // superposition: every partial faintly
        if (showComponents && finalPeak > 1.0e-6f)
        {
            const float s = sc / finalPeak;
            for (int i = 0; i < finalSet.count; ++i)
            {
                const float a = finalSet.amp[(size_t) i];
                if (a < 0.004f)
                    continue;
                const auto c = juce::Colour::fromHSV (0.74f - 0.3f * (float) i / 63.0f, 0.55f, 1.0f, 1.0f);
                g.setColour (c.withAlpha (juce::jlimit (0.12f, 0.55f, 0.12f + a * 0.8f)));
                g.strokePath (partialPath (finalSet.ratio[(size_t) i], a, finalSet.phase[(size_t) i], s), juce::PathStrokeType (1.0f));
            }
        }

        if (haveInput)
        {
            g.setColour (Accent::input.withAlpha (0.5f));
            g.strokePath (cyclePath (inputCycle, sc), juce::PathStrokeType (1.2f));
        }
        if (haveSynth)
        {
            g.setColour (Accent::synth.withAlpha (0.55f));
            g.strokePath (cyclePath (synthCycle, sc), juce::PathStrokeType (1.3f));
        }

        // the hovered harmonic's own sine
        if (hoverBar >= 0 && hoverBar < finalSet.count && finalPeak > 1.0e-6f && finalSet.amp[(size_t) hoverBar] > 1.0e-5f)
        {
            const auto p = partialPath (finalSet.ratio[(size_t) hoverBar], finalSet.amp[(size_t) hoverBar],
                                        finalSet.phase[(size_t) hoverBar], sc / finalPeak);
            g.setColour (amber.withAlpha (0.25f));
            g.strokePath (p, juce::PathStrokeType (4.0f));
            g.setColour (amber.withAlpha (0.95f));
            g.strokePath (p, juce::PathStrokeType (1.4f));
        }

        // the final wave (what the synth plays): soft fill + glow
        if (finalPeak > 1.0e-6f)
        {
            const auto wave = cyclePath (finalWave, sc / finalPeak);
            juce::Path fill (wave);
            fill.lineTo (plot.getRight(), cy);
            fill.lineTo (plot.getX(), cy);
            fill.closeSubPath();
            // fill fades toward the zero line from both sides
            juce::ColourGradient grad (Accent::sculpt.withAlpha (0.22f), 0.0f, cy - sc, Accent::sculpt.withAlpha (0.22f), 0.0f, cy + sc, false);
            grad.addColour (0.5, Accent::sculpt.withAlpha (0.02f));
            g.setGradientFill (grad);
            g.fillPath (fill);
            glow (g, wave, Accent::sculpt, 1.8f);
        }
        else
        {
            g.setColour (dim);
            g.setFont (Theme::font (12.0f));
            g.drawText ("Silent table - load a wave, raise a bar or draw a shape", wavePlot, juce::Justification::centred);
        }

        // the active table on its own (when it differs, and always while editing)
        if (showTableLine)
        {
            const float s = sc / tablePeakShown;
            if (drag == Drag::draw)
            {
                g.setColour (text.withAlpha (0.25f));
                g.strokePath (cyclePath (painted, sc), juce::PathStrokeType (3.0f));
            }
            g.setColour (Palette::bright.withAlpha (drag != Drag::none ? 0.85f : 0.45f));
            g.strokePath (cyclePath (tableWave, s), juce::PathStrokeType (drag != Drag::none ? 1.4f : 1.0f));
        }

        // bend handle: influence zone + grab point
        if (! drawMode && (hoverWave || drag == Drag::bend) && drag != Drag::draw)
        {
            const float fx = drag == Drag::bend ? gestureX0 : fracFromX (hoverPos.x);
            const float s = sc / (drag == Drag::bend ? gestureP0 : tablePeakShown);
            const float yv = cy - valueAt (tableWave, fx) * s;
            const float xc = plot.getX() + fx * plot.getWidth();
            const float half = bendWidth * 2.0f * plot.getWidth();
            juce::ColourGradient zone (Accent::sculpt.withAlpha (0.0f), xc - half, 0.0f, Accent::sculpt.withAlpha (0.0f), xc + half, 0.0f, false);
            zone.addColour (0.5, Accent::sculpt.withAlpha (drag == Drag::bend ? 0.18f : 0.10f));
            g.setGradientFill (zone);
            g.fillRect (juce::Rectangle<float> (xc - half, plot.getY(), half * 2.0f, plot.getHeight()));
            g.setColour (Palette::bright);
            g.fillEllipse (xc - 4.0f, yv - 4.0f, 8.0f, 8.0f);
            g.setColour (Accent::sculpt);
            g.drawEllipse (xc - 6.5f, yv - 6.5f, 13.0f, 13.0f, 1.5f);
        }
        else if (drawMode && hoverWave && drag == Drag::none)
        {
            g.setColour (Palette::bright.withAlpha (0.8f));
            g.drawEllipse (hoverPos.x - 3.0f, hoverPos.y - 3.0f, 6.0f, 6.0f, 1.2f);
        }
        g.restoreState();
    }

    void paintBars (juce::Graphics& g)
    {
        using namespace Palette;
        viewBox (g, barsArea);
        const auto plot = barsPlot.toFloat();
        const float bw = barWidth();
        const float limit = param ("scPartials", 64.0f);

        auto head = barsArea.reduced (12, 0).removeFromTop (22);
        g.setFont (Theme::font (12.0f, true));
        g.setColour (Accent::sculpt);
        g.drawText ("OVERTONES", head, juce::Justification::centredLeft);
        g.setFont (Theme::font (10.5f));
        g.setColour (dim);
        const juce::String hint = barsArea.getWidth() > 640
            ? "drag = level   shift-drag = paint   right/alt-drag = phase   double-click = zero"
            : "drag = level   shift = paint   right = phase";
        g.drawText (hint, head.withTrimmedLeft (86), juce::Justification::centredRight, true);

        // y grid + labels
        g.setFont (Theme::font (9.5f));
        auto gridLine = [&] (float frac, const juce::String& label)
        {
            const float y = plot.getBottom() - frac * plot.getHeight();
            g.setColour (grid);
            g.drawHorizontalLine ((int) std::round (y), plot.getX(), plot.getRight());
            g.setColour (dim.withAlpha (0.8f));
            g.drawText (label, barsArea.getX() + 2, (int) y - 7, 38, 14, juce::Justification::centredRight);
        };
        if (dbBars)
            for (int d = 0; d >= -48; d -= 12)
                gridLine ((float) (d + 60) / 60.0f, juce::String (d) + " dB");
        else
            for (float f : { 0.25f, 0.5f, 0.75f, 1.0f })
                gridLine (f, juce::String (f, 2));

        const float gap = bw > 6.0f ? 1.0f : 0.4f;
        for (int i = 0; i < maxPartials; ++i)
        {
            const float x = plot.getX() + (float) i * bw;
            const float h = barHeightFrac (table.amp[(size_t) i]) * plot.getHeight();
            const bool played = (float) (i + 1) <= limit + 0.5f;
            const bool hot = i == hoverBar;
            auto c = Accent::sculpt;
            if (! played) c = dim.withAlpha (0.35f);
            if (hot) c = c.brighter (0.5f);
            if (hot)
            {
                g.setColour (Accent::sculpt.withAlpha (0.10f));
                g.fillRect (x, plot.getY(), bw, plot.getHeight());
            }
            if (h > 0.0f)
            {
                const juce::Rectangle<float> bar (x + gap, plot.getBottom() - h, juce::jmax (0.8f, bw - 2.0f * gap), h);
                g.setGradientFill (juce::ColourGradient (c.withAlpha (played ? 0.95f : 0.5f), 0.0f, bar.getY(),
                                                         c.withAlpha (played ? 0.35f : 0.2f), 0.0f, plot.getBottom(), false));
                g.fillRect (bar);
                g.setColour (c.brighter (0.4f));
                g.fillRect (bar.withHeight (juce::jmin (1.5f, h)));
            }
            if (ghostVisible && ghost[(size_t) i] > 1.0e-5f)
            {
                const float gy = plot.getBottom() - barHeightFrac (ghost[(size_t) i]) * plot.getHeight();
                g.setColour (text.withAlpha (0.75f));
                g.fillRect (x + gap, gy - 0.75f, juce::jmax (0.8f, bw - 2.0f * gap), 1.5f);
            }
        }

        // partial-limit marker
        if (limit < 63.5f)
        {
            const float lx = plot.getX() + juce::jlimit (0.0f, 64.0f, limit) * bw;
            g.setColour (amber.withAlpha (0.7f));
            g.drawVerticalLine ((int) std::round (lx), plot.getY(), plot.getBottom());
        }

        g.setFont (Theme::font (9.5f));
        g.setColour (dim);
        for (int k : { 1, 8, 16, 24, 32, 40, 48, 56, 64 })
        {
            const float xc = plot.getX() + ((float) k - 0.5f) * bw;
            g.drawText (juce::String (k), (int) xc - 12, (int) plot.getBottom() + 1, 24, 12, juce::Justification::centred);
        }
    }

    void paintPhases (juce::Graphics& g)
    {
        using namespace Palette;
        viewBox (g, phaseArea);
        const auto plot = phasePlot.toFloat();
        g.setFont (Theme::font (9.5f, true));
        g.setColour (dim);
        g.drawText ("PHASE", phaseArea.getX() + 4, phaseArea.getY(), 40, phaseArea.getHeight(), juce::Justification::centred);
        g.setColour (grid);
        g.drawHorizontalLine ((int) std::round (plot.getCentreY()), plot.getX(), plot.getRight());

        const float bw = barWidth();
        const float r = juce::jlimit (1.2f, 3.0f, bw * 0.3f);
        for (int i = 0; i < maxPartials; ++i)
        {
            const float a = table.amp[(size_t) i];
            if (a < 0.0005f)
                continue;
            const float x = plot.getX() + ((float) i + 0.5f) * bw;
            const float y = plot.getCentreY() - table.phase[(size_t) i] / pi * plot.getHeight() * 0.5f;
            g.setColour ((i == hoverBar ? Accent::sculpt.brighter (0.5f) : Accent::sculpt).withAlpha (0.3f + 0.7f * std::sqrt (a)));
            g.fillEllipse (x - r, y - r, r * 2.0f, r * 2.0f);
        }
    }

    void paintInfo (juce::Graphics& g)
    {
        using namespace Palette;
        auto r = infoArea;
        const auto noteFont = Theme::font (11.5f, true);
        const auto note = noteText (trackerSnap);
        const int nw = juce::GlyphArrangement::getStringWidthInt (noteFont, note) + 14;
        g.setFont (noteFont);
        g.setColour (trackerSnap.voiced ? Accent::synth : dim);
        g.drawText (note, r.removeFromRight (nw), juce::Justification::centredRight);

        const juce::String readout = "partials " + juce::String (activePartials)
                                   + "   crest " + juce::String (crestDb, 1) + " dB"
                                   + "   bright H" + juce::String (brightness, 1)
                                   + (finalSet.harmonic ? juce::String() : juce::String ("   stretched"));
        g.setFont (Theme::font (11.0f, true));
        const int rw = juce::GlyphArrangement::getStringWidthInt (Theme::font (11.0f, true), readout) + 8;
        g.setColour (text.withAlpha (0.85f));
        g.drawText (readout, r.removeFromRight (rw), juce::Justification::centredRight);

        juce::String status;
        juce::Colour col = dim;
        if (juce::Time::getMillisecondCounter() < statusUntil)
        {
            status = statusMsg;
            col = Accent::sculpt.brighter (0.3f);
        }
        else if (hoverBar >= 0)
        {
            const auto i = (size_t) hoverBar;
            const float a = table.amp[i];
            const float f0 = trackerSnap.synthF0Hz > 0.0f ? trackerSnap.synthF0Hz : trackerSnap.f0Hz;
            status = "H" + juce::String (hoverBar + 1) + "  " + juce::String (finalSet.ratio[i], 2) + " x f0"
                   + (f0 > 0.0f ? " (" + juce::String (f0 * finalSet.ratio[i], 0) + " Hz)" : juce::String())
                   + "   amp " + juce::String (a, 3)
                   + (a > 1.0e-6f ? " (" + juce::String (20.0f * std::log10 (a), 1) + " dB)" : juce::String (" (off)"))
                   + "   phase " + juce::String ((int) std::round (table.phase[i] * 180.0f / pi)) + " deg";
            col = text;
        }
        else if (hoverWave)
            status = drawMode ? "Draw: drag to paint a new cycle - one stroke = one undo step"
                              : "Bend: drag up/down to pull this point - wheel changes the width ("
                                + juce::String ((int) std::round (bendWidth * 360.0f)) + " deg)";
        else
            status = "Shape the wave above, its overtones below. Ctrl+Z / Ctrl+Y = undo / redo.";
        g.setFont (Theme::font (11.0f));
        g.setColour (col);
        g.drawText (status, r.withTrimmedRight (8), juce::Justification::centredLeft, true);
    }

    //==========================================================================
    struct ToolItem { juce::Component* comp; int width; bool gap; };

    SculptView& view;
    ClodScopeProcessor& proc;
    SculptModel& model;
    HelpTarget* help;
    Canvas canvas;

    juce::ComboBox waveBox;
    juce::TextButton aBtn { "A" }, bBtn { "B" }, copyBtn { "A -> B" };
    MorphBox morphBox;
    juce::TextButton bendBtn { "Bend" }, drawBtn { "Draw" }, undoBtn { "Undo" }, redoBtn { "Redo" };
    juce::TextButton normBtn { "Normalize" }, clearBtn { "Clear" }, randBtn { "Random" }, smoothBtn { "Smooth" };
    juce::TextButton oddBtn { "Odd only" }, evenBtn { "Even only" }, invBtn { "Inv. phase" }, captureBtn { "Capture input" };
    juce::TextButton compBtn { "Components" }, synthOvBtn { "Synth" }, inputOvBtn { "Input" }, dbBtn { "dB" };
    juce::TextButton turnOnBtn { "Turn on" };
    juce::OwnedArray<widgets_detail::HoverReporter> hovers;   // after the components: destroyed first
    std::vector<ToolItem> toolItems;

    juce::Rectangle<int> toolbarArea, bannerArea, waveArea, barsArea, phaseArea, infoArea;
    juce::Rectangle<int> wavePlot, barsPlot, phasePlot;

    // display state
    int active = 0;
    PartialSet finalSet;
    BaseTable table;
    Cycle finalWave {}, tableWave {}, synthCycle {}, inputCycle {}, painted {}, gestureWave {};
    std::array<float, (size_t) maxPartials> ghost {};
    float finalPeak = 0.0f, tablePeak = 0.0f, tablePeakShown = 1.0f;
    bool showTableLine = false, ghostVisible = false, haveSynth = false, haveInput = false, bannerShown = false;
    TrackerSnapshot trackerSnap;
    int activePartials = 0;
    float brightness = 0.0f, crestDb = 0.0f;
    std::vector<float> scratchL, scratchR;
    juce::String statusMsg, helpId;
    juce::uint32 statusUntil = 0;

    // options
    bool drawMode = false, showComponents = false, showSynth = true, showInput = false, dbBars = false;
    float bendWidth = 0.06f;

    // interaction
    Drag drag = Drag::none;
    juce::Point<float> hoverPos;
    bool hoverWave = false;
    int hoverBar = -1;
    bool gestureFirst = true, gestureMoved = false, phaseEdit = false;
    Coeffs gestureZ0 {};
    float gestureP0 = 1.0f, gestureX0 = 0.0f, gestureDownY = 0.0f;
    float lastPaintFrac = 0.0f, lastPaintVal = 0.0f;
    BaseTable gestureTable;
    int downBar = 0, lastBar = 0, lastClickBar = -1;
    bool lastClickWasPhaseLane = false;
    float lastBarValue = 0.0f;
};

//==============================================================================
SculptView::SculptView (ClodScopeProcessor& p, HelpTarget* h)
{
    impl = std::make_unique<Impl> (*this, p, h);
}

SculptView::~SculptView() = default;

void SculptView::paint (juce::Graphics& g)
{
    g.fillAll (Palette::bg);
    const auto bar = impl->toolbarArea;
    g.setColour (Palette::panel);
    g.fillRect (bar);
    g.setColour (Palette::panelEdge);
    g.drawHorizontalLine (bar.getBottom() - 1, (float) bar.getX(), (float) bar.getRight());
}

void SculptView::resized()
{
    impl->layoutView();
}
