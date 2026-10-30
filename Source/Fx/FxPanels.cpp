#include "FxInternal.h"
#include <map>
#include <set>

// One control panel per FX module, built from titled sections of rows (knob rows, choice rows,
// segmented selectors, displays). Controls that do not apply to the current type are greyed out,
// rarely used ones sit in a collapsible ADVANCED section. Also: the rack's live description.

namespace
{
    using namespace juce;

    float paramValue (AudioProcessorValueTreeState& s, const char* id)
    {
        auto* p = s.getRawParameterValue (id);
        return p != nullptr ? p->load() : 0.0f;
    }

    String paramText (AudioProcessorValueTreeState& s, const char* id)
    {
        if (auto* p = s.getParameter (id))
            return p->getCurrentValueAsText();
        return {};
    }

    void setParam (AudioProcessorValueTreeState& s, const char* id, float v)
    {
        if (auto* p = s.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    float freqToX (float f, Rectangle<float> r)
    {
        return r.getX() + r.getWidth() * (float) (std::log (f / 20.0) / std::log (1000.0));
    }

    float xToFreq (float x, Rectangle<float> r)
    {
        return 20.0f * std::pow (1000.0f, jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth()));
    }

    void drawFreqGrid (Graphics& g, Rectangle<float> r, float dbMin, float dbMax, float dbStep)
    {
        g.setColour (Palette::viewBg);
        g.fillRoundedRectangle (r, 5.0f);
        g.setFont (Theme::font (9.5f));
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            const float x = freqToX (f, r);
            g.setColour (Palette::grid);
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            g.setColour (Palette::dim.withAlpha (0.7f));
            g.drawText (f >= 1000.0f ? String ((int) (f / 1000.0f)) + "k" : String ((int) f), (int) x + 2, (int) r.getBottom() - 12, 28, 11,
                        Justification::left);
        }
        for (float d = dbMin + dbStep; d < dbMax; d += dbStep)
        {
            const float y = jmap (d, dbMin, dbMax, r.getBottom(), r.getY());
            g.setColour (d == 0.0f ? Palette::panelEdge.brighter (0.3f) : Palette::grid);
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
            g.setColour (Palette::dim.withAlpha (0.7f));
            g.drawText (String ((int) d), (int) r.getX() + 2, (int) y - 10, 24, 10, Justification::left);
        }
    }

    // ---------------------------------------------------------------- EQ curve (draggable)
    class EqCurve : public Component, private Timer
    {
    public:
        EqCurve (AudioProcessorValueTreeState& s, const double& rate) : state (s), sampleRate (rate) { startTimerHz (20); }

        void paint (Graphics& g) override
        {
            const auto r = plot();
            drawFreqGrid (g, r, -18.0f, 18.0f, 6.0f);
            const auto st = fx::EqStage::read (state);
            const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;

            Path p;
            for (int x = 0; x <= (int) r.getWidth(); x += 2)
            {
                const double f = xToFreq (r.getX() + (float) x, r);
                const float db = (float) fx::EqStage::responseDb (st, f, sr);
                const float y = jlimit (r.getY(), r.getBottom(), jmap (db, -18.0f, 18.0f, r.getBottom(), r.getY()));
                if (x == 0) p.startNewSubPath (r.getX(), y);
                else        p.lineTo (r.getX() + (float) x, y);
            }
            Path fill (p);
            const float zeroY = jmap (0.0f, -18.0f, 18.0f, r.getBottom(), r.getY());
            fill.lineTo (r.getRight(), zeroY);
            fill.lineTo (r.getX(), zeroY);
            fill.closeSubPath();
            g.setColour (Accent::fx.withAlpha (0.12f));
            g.fillPath (fill);
            g.setColour (Accent::fx);
            g.strokePath (p, PathStrokeType (1.8f));

            for (int i = 0; i < numHandles; ++i)
            {
                const auto c = handlePos (i, st, r);
                const bool active = i == dragging || i == hovered;
                const bool cutOff = (i == 0 && ! fx::EqStage::lowCutOn (st.lowCut)) || (i == 5 && ! fx::EqStage::highCutOn (st.highCut));
                g.setColour (Palette::bg);
                g.fillEllipse (c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f);
                g.setColour (cutOff ? Palette::dim : (active ? Palette::text : Accent::fx));
                g.drawEllipse (c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f, active ? 2.0f : 1.4f);
                g.setFont (Theme::font (9.0f, true));
                g.drawText (labels[i], Rectangle<float> (c.x - 8.0f, c.y - 6.0f, 16.0f, 12.0f), Justification::centred);
            }
        }

        void mouseMove (const MouseEvent& e) override
        {
            const int h = hit (e.position);
            if (h != hovered) { hovered = h; repaint(); }
        }
        void mouseExit (const MouseEvent&) override { hovered = -1; repaint(); }

        void mouseDown (const MouseEvent& e) override
        {
            dragging = hit (e.position);
            if (dragging < 0)
                return;
            for (auto* id : { freqIds[dragging], gainIds[dragging] })
                if (id != nullptr)
                    if (auto* p = state.getParameter (id))
                        p->beginChangeGesture();
        }

        void mouseDrag (const MouseEvent& e) override
        {
            if (dragging < 0)
                return;
            const auto r = plot();
            setParam (state, freqIds[dragging], xToFreq (e.position.x, r));
            if (gainIds[dragging] != nullptr)
                setParam (state, gainIds[dragging], jlimit (-15.0f, 15.0f, jmap (e.position.y, r.getBottom(), r.getY(), -18.0f, 18.0f)));
            repaint();
        }

        void mouseUp (const MouseEvent&) override
        {
            if (dragging >= 0)
                for (auto* id : { freqIds[dragging], gainIds[dragging] })
                    if (id != nullptr)
                        if (auto* p = state.getParameter (id))
                            p->endChangeGesture();
            dragging = -1;
        }

        void mouseDoubleClick (const MouseEvent& e) override
        {
            const int h = hit (e.position);
            if (h >= 0 && gainIds[h] != nullptr)
                if (auto* p = state.getParameter (gainIds[h]))
                    p->setValueNotifyingHost (p->getDefaultValue());
        }

        void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
        {
            const int h = hit (e.position);
            const char* qId = h == 2 ? "fxEqMid1Q" : (h == 3 ? "fxEqMid2Q" : nullptr);
            if (qId == nullptr)
                return;
            const float q = paramValue (state, qId) * std::pow (1.15f, w.deltaY > 0 ? 1.0f : -1.0f);
            setParam (state, qId, jlimit (0.3f, 8.0f, q));
        }

    private:
        static constexpr int numHandles = 6;
        const char* freqIds[numHandles] = { "fxEqLowCut", "fxEqLowFreq", "fxEqMid1Freq", "fxEqMid2Freq", "fxEqHighFreq", "fxEqHighCut" };
        const char* gainIds[numHandles] = { nullptr, "fxEqLowGain", "fxEqMid1Gain", "fxEqMid2Gain", "fxEqHighGain", nullptr };
        const char* labels[numHandles] = { "C", "L", "1", "2", "H", "C" };

        Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (1.0f); }

        Point<float> handlePos (int i, const fx::EqStage::Settings& st, Rectangle<float> r) const
        {
            const float freqs[numHandles] = { st.lowCut, st.lowFreq, st.m1Freq, st.m2Freq, st.highFreq, st.highCut };
            const float gains[numHandles] = { 0.0f, st.lowGain, st.m1Gain, st.m2Gain, st.highGain, 0.0f };
            const float x = jlimit (r.getX() + 7.0f, r.getRight() - 7.0f, freqToX (freqs[i], r));
            const float y = (i == 0 || i == 5) ? r.getBottom() - 22.0f : jmap (gains[i], -18.0f, 18.0f, r.getBottom(), r.getY());
            return { x, y };
        }

        int hit (Point<float> pos) const
        {
            const auto st = fx::EqStage::read (state);
            const auto r = plot();
            int best = -1;
            float bestD = 14.0f;
            for (int i = 0; i < numHandles; ++i)
            {
                const float d = handlePos (i, st, r).getDistanceFrom (pos);
                if (d < bestD) { bestD = d; best = i; }
            }
            return best;
        }

        void timerCallback() override
        {
            const auto st = fx::EqStage::read (state);
            const float key = st.lowCut + st.lowFreq * 3.1f + st.lowGain * 7.0f + st.m1Freq * 1.7f + st.m1Gain * 11.0f + st.m1Q * 13.0f
                            + st.m2Freq * 0.7f + st.m2Gain * 17.0f + st.m2Q * 19.0f + st.highFreq * 0.3f + st.highGain * 23.0f
                            + st.highCut * 0.1f + st.out * 29.0f;
            if (key != lastKey) { lastKey = key; repaint(); }
        }

        AudioProcessorValueTreeState& state;
        const double& sampleRate;
        int dragging = -1, hovered = -1;
        float lastKey = 0.0f;
    };

    // ---------------------------------------------------------------- Cab response
    class CabCurve : public Component, private Timer
    {
    public:
        explicit CabCurve (AudioProcessorValueTreeState& s) : state (s) { startTimerHz (10); }

        void paint (Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat().reduced (1.0f);
            drawFreqGrid (g, r, -30.0f, 12.0f, 6.0f);
            g.setColour (Accent::fx.withAlpha (0.14f));
            g.fillPath (fillPath);
            g.setColour (Accent::fx);
            g.strokePath (curve, PathStrokeType (1.8f));
        }

        void resized() override { rebuild(); }

    private:
        void timerCallback() override
        {
            const float k[4] = { paramValue (state, "fxCabModel"), paramValue (state, "fxCabMic"),
                                 paramValue (state, "fxCabLowCut"), paramValue (state, "fxCabHighCut") };
            if (std::equal (std::begin (k), std::end (k), std::begin (key)))
                return;
            std::copy (std::begin (k), std::end (k), std::begin (key));
            rebuild();
        }

        void rebuild()
        {
            const auto r = getLocalBounds().toFloat().reduced (1.0f);
            curve.clear();
            const int model = (int) key[0];
            const float mic = key[1] * 0.01f;
            for (int x = 0; x <= (int) r.getWidth(); x += 3)
            {
                const double f = xToFreq (r.getX() + (float) x, r);
                const float db = fx::CabStage::designDb (model, mic, key[2], key[3], f);
                const float y = jlimit (r.getY(), r.getBottom(), jmap (db, -30.0f, 12.0f, r.getBottom(), r.getY()));
                if (x == 0) curve.startNewSubPath (r.getX(), y);
                else        curve.lineTo (r.getX() + (float) x, y);
            }
            fillPath = curve;
            fillPath.lineTo (curve.getCurrentPosition().x, r.getBottom());
            fillPath.lineTo (r.getX(), r.getBottom());
            fillPath.closeSubPath();
            repaint();
        }

        AudioProcessorValueTreeState& state;
        float key[4] { -1.0f, -1.0f, -1.0f, -1.0f };
        Path curve, fillPath;
    };

    // ---------------------------------------------------------------- Compressor display
    class CompView : public Component, private Timer
    {
    public:
        CompView (AudioProcessorValueTreeState& s, fx::CompStage& c) : state (s), comp (c) { startTimerHz (30); }

        void paint (Graphics& g) override
        {
            auto b = getLocalBounds().toFloat();
            auto meter = b.removeFromRight (34.0f);
            b.removeFromRight (10.0f);
            const float side = jmin (b.getHeight(), b.getWidth());
            auto r = b.removeFromLeft (side).reduced (1.0f);

            g.setColour (Palette::viewBg);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (Palette::grid);
            for (float d = -48.0f; d < 0.0f; d += 12.0f)
            {
                const float p = jmap (d, -60.0f, 0.0f, 0.0f, 1.0f);
                g.drawVerticalLine ((int) (r.getX() + p * r.getWidth()), r.getY(), r.getBottom());
                g.drawHorizontalLine ((int) (r.getBottom() - p * r.getHeight()), r.getX(), r.getRight());
            }
            g.setColour (Palette::panelEdge);
            g.drawLine (r.getX(), r.getBottom(), r.getRight(), r.getY(), 1.0f);

            const float t = paramValue (state, "fxCompThresh"), ratio = paramValue (state, "fxCompRatio"), knee = paramValue (state, "fxCompKnee");
            Path p;
            for (int i = 0; i <= 60; ++i)
            {
                const float in = -60.0f + (float) i;
                const float out = fx::CompStage::curveDb (in, t, ratio, knee);
                const float x = r.getX() + (float) i / 60.0f * r.getWidth();
                const float y = r.getBottom() - jlimit (0.0f, 1.0f, (out + 60.0f) / 60.0f) * r.getHeight();
                if (i == 0) p.startNewSubPath (x, y);
                else        p.lineTo (x, y);
            }
            g.setColour (Accent::fx);
            g.strokePath (p, PathStrokeType (1.8f));

            if (inDb > -60.0f)
            {
                const float out = fx::CompStage::curveDb (inDb, t, ratio, knee);
                const float x = r.getX() + (inDb + 60.0f) / 60.0f * r.getWidth();
                const float y = r.getBottom() - jlimit (0.0f, 1.0f, (out + 60.0f) / 60.0f) * r.getHeight();
                g.setColour (Palette::text);
                g.fillEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f);
            }

            // gain reduction bar (grows downward from 0 dB)
            g.setColour (Palette::viewBg);
            g.fillRoundedRectangle (meter, 4.0f);
            const float frac = jlimit (0.0f, 1.0f, -gr / 24.0f);
            auto bar = meter.reduced (5.0f, 16.0f);
            g.setColour (Accent::fx.withAlpha (0.85f));
            g.fillRect (bar.withHeight (bar.getHeight() * frac));
            g.setColour (Palette::dim);
            g.setFont (Theme::font (9.5f, true));
            g.drawText ("GR", meter.removeFromTop (14.0f), Justification::centred);
            g.setColour (Palette::text);
            g.drawText (String (gr, 1), meter.removeFromBottom (14.0f), Justification::centred);
        }

    private:
        void timerCallback() override
        {
            const float g = comp.grDb.load();
            gr = g < gr ? g : gr + 0.25f * (g - gr);
            inDb = comp.inDb.load();
            repaint();
        }

        AudioProcessorValueTreeState& state;
        fx::CompStage& comp;
        float gr = 0.0f, inDb = -100.0f;
    };

    // ---------------------------------------------------------------- Limiter meter
    class LimMeter : public Component, private Timer
    {
    public:
        LimMeter (fx::LimiterStage& l, std::atomic<float>& outPeak) : lim (l), out (outPeak) { startTimerHz (30); }

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            auto row = [&] (Rectangle<float> b, const String& name, float frac, const String& value, Colour c)
            {
                g.setColour (Palette::dim);
                g.setFont (Theme::font (10.0f, true));
                g.drawText (name, b.removeFromLeft (44.0f), Justification::centredLeft);
                auto v = b.removeFromRight (64.0f);
                g.setColour (Palette::viewBg);
                g.fillRoundedRectangle (b.reduced (0.0f, 3.0f), 3.0f);
                g.setColour (c);
                g.fillRoundedRectangle (b.reduced (0.0f, 3.0f).withWidth (b.getWidth() * jlimit (0.0f, 1.0f, frac)), 3.0f);
                g.setColour (Palette::text);
                g.drawText (value, v, Justification::centredRight);
            };
            const float h = r.getHeight() * 0.5f;
            row (r.removeFromTop (h), "GR", -gr / 24.0f, String (gr, 1) + " dB", Accent::fx);
            row (r, "OUT", (outDb + 48.0f) / 48.0f, outDb <= -99.0f ? String ("-inf") : String (outDb, 1) + " dB", Palette::green);
        }

    private:
        void timerCallback() override
        {
            const float g = lim.grDb.load();
            gr = g < gr ? g : gr + 0.25f * (g - gr);
            outDb = fx::gainToDb (out.load());
            repaint();
        }

        fx::LimiterStage& lim;
        std::atomic<float>& out;
        float gr = 0.0f, outDb = -100.0f;
    };

    // ---------------------------------------------------------------- ADVANCED disclosure header
    class Disclosure : public Component
    {
    public:
        std::function<void()> onToggle;
        bool open = false;

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            const auto c = isMouseOver() ? Palette::text : Accent::fx;
            Path tri;
            const float cy = r.getCentreY(), x = r.getX() + 2.0f;
            if (open) tri.addTriangle (x, cy - 3.0f, x + 8.0f, cy - 3.0f, x + 4.0f, cy + 3.0f);
            else      tri.addTriangle (x + 1.0f, cy - 4.5f, x + 1.0f, cy + 4.5f, x + 7.0f, cy);
            g.setColour (c);
            g.fillPath (tri);
            g.setFont (Theme::font (12.0f, true));
            g.drawText ("ADVANCED", r.withTrimmedLeft (16.0f), Justification::centredLeft);
            g.setColour (Palette::dim);
            g.setFont (Theme::font (10.5f));
            g.drawText (open ? "click to hide" : "click to show", r, Justification::centredRight);
        }
        void mouseEnter (const MouseEvent&) override { repaint(); }
        void mouseExit (const MouseEvent&) override  { repaint(); }
        void mouseUp (const MouseEvent& e) override
        {
            if (e.mouseWasClicked())
            {
                open = ! open;
                repaint();
                if (onToggle) onToggle();
            }
        }
    };
}

//==============================================================================
struct FxRack::Impl::Panel : public ControlPanel, private juce::Timer
{
    enum RowKind { knobRow, choiceRow, fullRow };
    struct Row { RowKind kind; std::vector<juce::Component*> comps; int height = 0; };
    struct Section { juce::String title; std::vector<Row> rows; bool advanced = false; juce::Rectangle<int> rect; };

    Panel (FxRack::Impl& r, FxRack::Module m, HelpTarget* h)
        : rack (r), state (r.apvts), module (m), help (h),
          power (r.apvts, FxRack::moduleEnableId (m), h, Accent::fx)
    {
        addAndMakeVisible (power);
        status.setFont (Theme::font (10.5f));
        status.setColour (juce::Label::textColourId, Palette::dim);
        status.setJustificationType (juce::Justification::centredRight);
        status.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (status);
        disclosure.onToggle = [this]
        {
            advancedOpen = disclosure.open;
            applyAdvancedVisibility();
            if (getWidth() > 0)
                setSize (getWidth(), idealHeight (getWidth()));
            resized();
        };
        build();
        applyAdvancedVisibility();
        refresh();
        startTimerHz (10);
    }

    ~Panel() override { stopTimer(); }

    juce::String title() const override { return FxRack::moduleName (module); }
    int idealHeight (int width) const override { return const_cast<Panel*> (this)->layout (width, false); }
    void resized() override { layout (getWidth(), true); }

    void paint (juce::Graphics& g) override
    {
        for (auto& s : sections)
            Theme::drawSection (g, s.rect, s.advanced ? juce::String() : s.title, Accent::fx);
    }

    //------------------------------------------------------------------ building blocks
    template <typename T, typename... Args>
    T* make (Args&&... args)
    {
        auto c = std::make_unique<T> (std::forward<Args> (args)...);
        auto* raw = c.get();
        addAndMakeVisible (*raw);
        owned.push_back (std::move (c));
        return raw;
    }

    ParamKnob* knob (const char* id, const char* label, bool bipolar = false)
    {
        auto* k = make<ParamKnob> (state, id, label, help, bipolar);
        byId[id] = k;
        return k;
    }

    ParamChoice* choice (const char* id, const char* label)
    {
        auto* c = make<ParamChoice> (state, id, label, help);
        byId[id] = c;
        return c;
    }

    ParamToggle* toggle (const char* id, const char* text)
    {
        auto* t = make<ParamToggle> (state, id, text, help);
        byId[id] = t;
        return t;
    }

    juce::Label* info (const char* helpId = nullptr)
    {
        auto* l = make<juce::Label>();
        l->setFont (Theme::font (11.0f));
        l->setColour (juce::Label::textColourId, Palette::dim);
        l->setJustificationType (juce::Justification::topLeft);
        l->setMinimumHorizontalScale (1.0f);
        if (helpId != nullptr)
            hovers.push_back (std::make_unique<widgets_detail::HoverReporter> (*l, helpId, help));
        else
            l->setInterceptsMouseClicks (false, false);
        return l;
    }

    juce::Component* segmented (const char* id, const char* label)
    {
        auto* holder = make<juce::Component>();
        auto* l = new juce::Label ({}, label);
        l->setFont (Theme::font (11.0f, true));
        l->setInterceptsMouseClicks (false, false);
        auto* seg = new ParamSegmented (state, id, help, Accent::fx);
        holder->addAndMakeVisible (l);
        holder->addAndMakeVisible (seg);
        segParts.push_back (std::unique_ptr<juce::Component> (l));
        segParts.push_back (std::unique_ptr<juce::Component> (seg));
        segLayouts.push_back ({ holder, l, seg });
        byId[id] = holder;
        return holder;
    }

    template <typename T>
    T* display (std::unique_ptr<T> c, const char* helpId)
    {
        auto* raw = c.get();
        addAndMakeVisible (*raw);
        hovers.push_back (std::make_unique<widgets_detail::HoverReporter> (*raw, helpId, help));
        owned.push_back (std::move (c));
        return raw;
    }

    Section& section (const juce::String& t, bool advanced = false)
    {
        sections.push_back ({ t, {}, advanced, {} });
        return sections.back();
    }

    static void knobs (Section& s, std::initializer_list<juce::Component*> c) { s.rows.push_back ({ knobRow, c, Theme::knobCellHeight }); }
    static void choices (Section& s, std::initializer_list<juce::Component*> c) { s.rows.push_back ({ choiceRow, c, Theme::choiceHeight }); }
    static void full (Section& s, juce::Component* c, int h) { s.rows.push_back ({ fullRow, { c }, h }); }

    //------------------------------------------------------------------ per-module content
    void build()
    {
        const auto upper = FxRack::moduleName (module).toUpperCase();
        sections.reserve (8);   // sections are filled through references while new ones are added
        switch (module)
        {
            case FxRack::amp:
            {
                auto& s = section (upper);
                choices (s, { choice ("fxAmpModel", "MODEL") });
                modelInfo = info();
                full (s, modelInfo, 30);
                knobs (s, { knob ("fxAmpGain", "GAIN"), knob ("fxAmpBass", "BASS"), knob ("fxAmpMid", "MID"), knob ("fxAmpTreble", "TREBLE") });
                knobs (s, { knob ("fxAmpPresence", "PRESENCE"), knob ("fxAmpLevel", "LEVEL"), toggle ("fxAmpBright", "Bright") });
                auto& a = section ("ADVANCED", true);
                knobs (a, { knob ("fxAmpGate", "NOISE GATE"), knob ("fxAmpMix", "MIX") });
                break;
            }
            case FxRack::cab:
            {
                auto& s = section (upper);
                choices (s, { choice ("fxCabModel", "CABINET") });
                full (s, display (std::make_unique<CabCurve> (state), "fx.cabCurve"), 110);
                knobs (s, { knob ("fxCabMic", "MIC POSITION"), knob ("fxCabRoom", "ROOM MIC"), knob ("fxCabLevel", "LEVEL", true) });
                auto& a = section ("ADVANCED", true);
                knobs (a, { knob ("fxCabLowCut", "LOW CUT"), knob ("fxCabHighCut", "HIGH CUT") });
                break;
            }
            case FxRack::eq:
            {
                auto& s = section (upper);
                full (s, display (std::make_unique<EqCurve> (state, rack.sampleRate), "fx.eqCurve"), 170);
                knobs (s, { knob ("fxEqLowCut", "LOW CUT"), knob ("fxEqLowFreq", "LOW FREQ"), knob ("fxEqLowGain", "LOW GAIN", true),
                            knob ("fxEqOutput", "OUTPUT", true) });
                auto& m = section ("MIDS");
                knobs (m, { knob ("fxEqMid1Freq", "MID 1 FREQ"), knob ("fxEqMid1Gain", "MID 1 GAIN", true), knob ("fxEqMid1Q", "MID 1 Q") });
                knobs (m, { knob ("fxEqMid2Freq", "MID 2 FREQ"), knob ("fxEqMid2Gain", "MID 2 GAIN", true), knob ("fxEqMid2Q", "MID 2 Q") });
                auto& h = section ("HIGHS");
                knobs (h, { knob ("fxEqHighFreq", "HIGH FREQ"), knob ("fxEqHighGain", "HIGH GAIN", true), knob ("fxEqHighCut", "HIGH CUT") });
                break;
            }
            case FxRack::comp:
            {
                auto& s = section (upper);
                full (s, display (std::make_unique<CompView> (state, rack.comp), "fx.compMeter"), 120);
                knobs (s, { knob ("fxCompThresh", "THRESHOLD"), knob ("fxCompRatio", "RATIO"), knob ("fxCompAttack", "ATTACK"),
                            knob ("fxCompRelease", "RELEASE") });
                knobs (s, { knob ("fxCompKnee", "KNEE"), knob ("fxCompMakeup", "MAKEUP"), knob ("fxCompMix", "MIX"),
                            toggle ("fxCompAuto", "Auto makeup") });
                auto& a = section ("ADVANCED", true);
                knobs (a, { knob ("fxCompScHpf", "SIDECHAIN HPF") });
                break;
            }
            case FxRack::mod:
            {
                auto& s = section (upper);
                choices (s, { choice ("fxModType", "TYPE") });
                modelInfo = info();
                full (s, modelInfo, 30);
                rateKnob = knob ("fxModRate", "RATE");
                divChoice = choice ("fxModDiv", "SYNC RATE");
                knobs (s, { toggle ("fxModSync", "Tempo sync"), rateKnob, knob ("fxModDepth", "DEPTH"), knob ("fxModMix", "MIX") });
                knobs (s, { knob ("fxModFeedback", "FEEDBACK", true), knob ("fxModWidth", "WIDTH") });
                auto& a = section ("ADVANCED", true);
                knobs (a, { choice ("fxModShape", "LFO SHAPE"), knob ("fxModTone", "TONE") });
                break;
            }
            case FxRack::delay:
            {
                auto& s = section (upper);
                full (s, segmented ("fxDelayType", "TYPE"), 46);
                modelInfo = info ("fx.delayInfo");
                full (s, modelInfo, 18);
                rateKnob = knob ("fxDelayTime", "TIME");
                divChoice = choice ("fxDelayDiv", "NOTE");
                knobs (s, { toggle ("fxDelaySync", "Tempo sync"), rateKnob, knob ("fxDelayFeedback", "FEEDBACK"), knob ("fxDelayMix", "MIX") });
                knobs (s, { knob ("fxDelayLowCut", "LOW CUT"), knob ("fxDelayHighCut", "HIGH CUT"), knob ("fxDelayDuck", "DUCKING"),
                            knob ("fxDelayWidth", "WIDTH") });
                auto& t = section ("TAPE");
                knobs (t, { knob ("fxDelayWow", "WOW/FLUTTER"), knob ("fxDelaySat", "SATURATION") });
                break;
            }
            case FxRack::reverb:
            {
                auto& s = section (upper);
                choices (s, { choice ("fxRevType", "TYPE") });
                modelInfo = info();
                full (s, modelInfo, 30);
                knobs (s, { knob ("fxRevPreDelay", "PRE-DELAY"), knob ("fxRevSize", "SIZE"), knob ("fxRevDecay", "DECAY"), knob ("fxRevMix", "MIX") });
                knobs (s, { knob ("fxRevDamping", "DAMPING"), knob ("fxRevLowCut", "LOW CUT"), knob ("fxRevWidth", "WIDTH"),
                            knob ("fxRevShimmer", "SHIMMER") });
                auto& a = section ("ADVANCED", true);
                knobs (a, { knob ("fxRevMod", "MODULATION") });
                break;
            }
            case FxRack::limiter:
            {
                auto& s = section (upper);
                full (s, display (std::make_unique<LimMeter> (rack.limiter, rack.peak[FxRack::limiter]), "fx.limMeter"), 40);
                knobs (s, { knob ("fxLimInput", "INPUT"), knob ("fxLimCeiling", "CEILING"), knob ("fxLimRelease", "RELEASE") });
                break;
            }
            case FxRack::numModules:
            default: break;
        }
        for (auto& s : sections)
            if (s.advanced)
                addAndMakeVisible (disclosure);
    }

    //------------------------------------------------------------------ layout
    int layout (int width, bool apply)
    {
        const int pad = 12;
        const int inner = juce::jmax (100, width - pad * 2);
        const int cw = inner / 4;
        int y = 0;
        auto place = [apply] (juce::Component* c, juce::Rectangle<int> r) { if (apply && c != nullptr) c->setBounds (r); };

        for (size_t si = 0; si < sections.size(); ++si)
        {
            auto& s = sections[si];
            const int top = y;
            if (si == 0)
            {
                place (&power, { width - pad - 22, top + 6, 22, 22 });
                place (&status, { width / 2 - 20, top + 8, width / 2 - pad - 12, 16 });
            }
            if (s.advanced)
            {
                place (&disclosure, { pad, top + 4, inner, 20 });
                y += Theme::sectionTitle;
                if (! advancedOpen)
                {
                    s.rect = { 0, top, width, Theme::sectionTitle + 2 };
                    y = top + s.rect.getHeight() + 10;
                    continue;
                }
            }
            else
            {
                y += Theme::sectionTitle;
            }

            for (auto& row : s.rows)
            {
                switch (row.kind)
                {
                    case knobRow:
                    {
                        int i = 0;
                        for (auto* c : row.comps)
                        {
                            const juce::Rectangle<int> cell (pad + (i++) * cw, y, cw, Theme::knobCellHeight - 4);
                            if (dynamic_cast<ParamKnob*> (c) != nullptr)
                                place (c, cell);
                            else if (dynamic_cast<ParamToggle*> (c) != nullptr)
                                place (c, cell.reduced (4, 0).withSizeKeepingCentre (cell.getWidth() - 8, 24));
                            else
                                place (c, cell.reduced (4, 0).withSizeKeepingCentre (cell.getWidth() - 8, 40));
                            if (c == rateKnob && divChoice != nullptr)   // the synced note choice shares the time/rate cell
                                place (divChoice, cell.reduced (4, 0).withSizeKeepingCentre (cell.getWidth() - 8, 40));
                        }
                        break;
                    }
                    case choiceRow:
                    {
                        const int n = (int) row.comps.size();
                        const int w = (inner - 8 * (n - 1)) / juce::jmax (1, n);
                        int i = 0;
                        for (auto* c : row.comps)
                            place (c, { pad + (i++) * (w + 8), y, w, 40 });
                        break;
                    }
                    case fullRow:
                        place (row.comps[0], { pad, y, inner, row.height });
                        break;
                }
                y += row.height + (row.kind == fullRow ? 6 : 0);
            }
            y += 6;
            s.rect = { 0, top, width, y - top };
            y += 10;
        }

        if (apply)
            for (auto& sl : segLayouts)
            {
                auto r = sl.holder->getLocalBounds();
                sl.label->setBounds (r.removeFromTop (16));
                sl.seg->setBounds (r.removeFromTop (26));
            }
        return juce::jmax (0, y - 10);
    }

    void applyAdvancedVisibility()
    {
        for (auto& s : sections)
            if (s.advanced)
                for (auto& row : s.rows)
                    for (auto* c : row.comps)
                        c->setVisible (advancedOpen);
    }

    //------------------------------------------------------------------ live state
    void timerCallback() override { refresh(); }

    void enable (const char* id, bool on)
    {
        auto it = byId.find (id);
        if (it != byId.end() && ! on)
            disabled.insert (it->second);
    }

    void refresh()
    {
        disabled.clear();
        const bool moduleOn = paramValue (state, FxRack::moduleEnableId (module).toRawUTF8()) > 0.5f;
        const double bpm = rack.lastBpm.load();
        juce::String st;

        switch (module)
        {
            case FxRack::amp:
            {
                const auto& v = fx::AmpStage::voice ((int) paramValue (state, "fxAmpModel"));
                modelInfo->setText (v.blurb, juce::dontSendNotification);
                const float drive = v.driveMinDb + paramValue (state, "fxAmpGain") * 0.1f * (v.driveMaxDb - v.driveMinDb);
                st = "drive " + juce::String (drive, 1) + " dB  |  4x OS, " + juce::String (rack.latAmp) + " smp";
                break;
            }
            case FxRack::cab:
                st = "close + room mic IR";
                break;
            case FxRack::eq:
                st = "drag the dots";
                break;
            case FxRack::comp:
            {
                const bool autoOn = paramValue (state, "fxCompAuto") > 0.5f;
                st = autoOn ? "auto makeup +" + juce::String (fx::CompStage::autoMakeupDb (paramValue (state, "fxCompThresh"),
                                                                                              paramValue (state, "fxCompRatio")), 1) + " dB"
                            : juce::String ("manual makeup");
                break;
            }
            case FxRack::mod:
            {
                const int t = (int) paramValue (state, "fxModType");
                static const char* blurbs[] = {
                    "Chorus: a slightly detuned copy beside the original - wider, shimmering.",
                    "Ensemble: three moving voices per side, the lush string-machine sound.",
                    "Flanger: a short sweeping delay with feedback - the jet-plane whoosh.",
                    "Phaser: sweeping notches from allpass stages - swirly, vocal.",
                    "Vibrato: pitch wobble only, no dry signal.",
                    "Tremolo: volume wobble; Width = ping-pong between the sides.",
                    "Auto-pan: the sound moves between left and right." };
                modelInfo->setText (blurbs[juce::jlimit (0, 6, t)], juce::dontSendNotification);
                const bool sync = paramValue (state, "fxModSync") > 0.5f;
                rateKnob->setVisible (! sync);
                divChoice->setVisible (sync);
                enable ("fxModFeedback", t == fx::ModStage::chorus || t == fx::ModStage::flanger || t == fx::ModStage::phaser);
                enable ("fxModMix", t <= fx::ModStage::phaser);
                enable ("fxModWidth", t != fx::ModStage::autopan);
                enable ("fxModTone", t <= fx::ModStage::flanger || t == fx::ModStage::vibrato);
                enable ("fxModShape", t != fx::ModStage::ensemble);
                const float hz = fx::ModStage::rateHz (paramValue (state, "fxModRate"), sync, (int) paramValue (state, "fxModDiv"), bpm);
                st = juce::String (hz, hz < 1.0f ? 2 : 1) + " Hz" + (sync ? " @ " + juce::String (bpm, 0) + " BPM" : juce::String());
                break;
            }
            case FxRack::delay:
            {
                const bool sync = paramValue (state, "fxDelaySync") > 0.5f;
                rateKnob->setVisible (! sync);
                divChoice->setVisible (sync);
                const int t = (int) paramValue (state, "fxDelayType");
                enable ("fxDelayWow", t == fx::DelayStage::tape);
                enable ("fxDelaySat", t == fx::DelayStage::tape);
                const double ms = juce::jmin (4000.0, fx::DelayStage::timeMs (paramValue (state, "fxDelayTime"), sync,
                                                                              (int) paramValue (state, "fxDelayDiv"), bpm));
                modelInfo->setText ("Repeats every " + juce::String (ms, ms < 100.0 ? 1 : 0) + " ms"
                                        + (sync ? " (" + paramText (state, "fxDelayDiv") + " at " + juce::String (bpm, 1) + " BPM)" : juce::String()),
                                    juce::dontSendNotification);
                st = paramText (state, "fxDelayType");
                break;
            }
            case FxRack::reverb:
            {
                const int t = (int) paramValue (state, "fxRevType");
                static const char* blurbs[] = {
                    "Room: small space with clear early reflections - natural, close.",
                    "Hall: large, smooth concert-hall tail.",
                    "Plate: dense and bright, the classic vocal and snare sound.",
                    "Chamber: mid-size, warm, with audible reflections.",
                    "Shimmer: huge ambient space whose tail climbs an octave (Shimmer knob)." };
                modelInfo->setText (blurbs[juce::jlimit (0, 4, t)], juce::dontSendNotification);
                enable ("fxRevShimmer", t == fx::ReverbStage::shimmer);
                st = "RT60 " + paramText (state, "fxRevDecay");
                break;
            }
            case FxRack::limiter:
                st = "look-ahead " + juce::String (rack.latLim) + " smp";
                break;
            case FxRack::numModules:
            default: break;
        }
        status.setText (moduleOn ? st : juce::String ("off - click the power button"), juce::dontSendNotification);

        // grey out what does not apply; dim everything a little while the module is off
        for (auto& o : owned)
        {
            auto* c = o.get();
            if (c == &status)
                continue;
            const bool dis = disabled.count (c) > 0;
            c->setEnabled (! dis);
            c->setAlpha ((moduleOn ? 1.0f : 0.6f) * (dis ? 0.4f : 1.0f));
        }
    }

    struct SegLayout { juce::Component* holder; juce::Component* label; juce::Component* seg; };

    FxRack::Impl& rack;
    juce::AudioProcessorValueTreeState& state;
    FxRack::Module module;
    HelpTarget* help;
    ParamPowerButton power;
    juce::Label status;
    Disclosure disclosure;
    bool advancedOpen = false;

    std::vector<Section> sections;
    std::vector<std::unique_ptr<juce::Component>> owned, segParts;
    std::vector<SegLayout> segLayouts;
    std::vector<std::unique_ptr<widgets_detail::HoverReporter>> hovers;
    std::map<juce::String, juce::Component*> byId;
    std::set<juce::Component*> disabled;
    juce::Label* modelInfo = nullptr;
    ParamKnob* rateKnob = nullptr;
    ParamChoice* divChoice = nullptr;
};

std::unique_ptr<ControlPanel> FxRack::Impl::makePanel (Impl& impl, FxRack::Module m, HelpTarget* help)
{
    return std::make_unique<Panel> (impl, m, help);
}

//==============================================================================
juce::String FxRack::Impl::describe()
{
    auto& s = apvts;
    auto t = [&s] (const char* id) { return paramText (s, id); };
    auto on = [&s] (FxRack::Module m) { return paramValue (s, FxRack::moduleEnableId (m).toRawUTF8()) > 0.5f; };

    juce::StringArray lines;
    if (on (FxRack::amp))
    {
        const auto& v = fx::AmpStage::voice ((int) paramValue (s, "fxAmpModel"));
        const float drive = v.driveMinDb + paramValue (s, "fxAmpGain") * 0.1f * (v.driveMaxDb - v.driveMinDb);
        lines.add ("Amp: " + t ("fxAmpModel") + ", gain " + t ("fxAmpGain") + " -> drive " + juce::String (drive, 1)
                   + " dB into " + juce::String (v.stages) + " stage(s); bass/mid/treble " + t ("fxAmpBass") + "/" + t ("fxAmpMid") + "/"
                   + t ("fxAmpTreble") + ", presence " + t ("fxAmpPresence") + ", level " + t ("fxAmpLevel") + ", mix " + t ("fxAmpMix")
                   + (paramValue (s, "fxAmpGate") > -99.9f ? ", gate " + t ("fxAmpGate") : juce::String()));
    }
    if (on (FxRack::cab))
        lines.add ("Cab: " + t ("fxCabModel") + ", mic " + t ("fxCabMic") + " off-axis, room mic " + t ("fxCabRoom")
                   + ", cuts " + t ("fxCabLowCut") + " / " + t ("fxCabHighCut") + ", level " + t ("fxCabLevel"));
    if (on (FxRack::eq))
        lines.add ("EQ: low cut " + t ("fxEqLowCut") + ", low shelf " + t ("fxEqLowGain") + " @ " + t ("fxEqLowFreq")
                   + ", mid1 " + t ("fxEqMid1Gain") + " @ " + t ("fxEqMid1Freq") + " Q" + t ("fxEqMid1Q")
                   + ", mid2 " + t ("fxEqMid2Gain") + " @ " + t ("fxEqMid2Freq") + " Q" + t ("fxEqMid2Q")
                   + ", high shelf " + t ("fxEqHighGain") + " @ " + t ("fxEqHighFreq") + ", high cut " + t ("fxEqHighCut")
                   + ", output " + t ("fxEqOutput"));
    if (on (FxRack::comp))
        lines.add ("Comp: " + t ("fxCompRatio") + " above " + t ("fxCompThresh") + ", knee " + t ("fxCompKnee") + ", attack "
                   + t ("fxCompAttack") + ", release " + t ("fxCompRelease") + ", makeup " + t ("fxCompMakeup")
                   + (paramValue (s, "fxCompAuto") > 0.5f ? " + auto" : "") + ", mix " + t ("fxCompMix")
                   + "; now reducing " + juce::String (-comp.grDb.load(), 1) + " dB");
    if (on (FxRack::mod))
    {
        const bool sync = paramValue (s, "fxModSync") > 0.5f;
        lines.add ("Mod: " + t ("fxModType") + " at " + (sync ? t ("fxModDiv") + " (synced)" : t ("fxModRate")) + ", depth "
                   + t ("fxModDepth") + ", feedback " + t ("fxModFeedback") + ", mix " + t ("fxModMix") + ", width " + t ("fxModWidth"));
    }
    if (on (FxRack::delay))
    {
        const bool sync = paramValue (s, "fxDelaySync") > 0.5f;
        const double ms = juce::jmin (4000.0, fx::DelayStage::timeMs (paramValue (s, "fxDelayTime"), sync, (int) paramValue (s, "fxDelayDiv"), lastBpm.load()));
        lines.add ("Delay: " + t ("fxDelayType") + ", " + (sync ? t ("fxDelayDiv") + " = " : juce::String()) + juce::String (ms, 0)
                   + " ms, feedback " + t ("fxDelayFeedback") + ", mix " + t ("fxDelayMix") + ", loop filter " + t ("fxDelayLowCut")
                   + " .. " + t ("fxDelayHighCut") + (paramValue (s, "fxDelayDuck") > 0.0f ? ", ducking " + t ("fxDelayDuck") : juce::String()));
    }
    if (on (FxRack::reverb))
        lines.add ("Reverb: " + t ("fxRevType") + ", decay " + t ("fxRevDecay") + ", size " + t ("fxRevSize") + ", pre-delay "
                   + t ("fxRevPreDelay") + ", damping " + t ("fxRevDamping") + ", mix " + t ("fxRevMix")
                   + ((int) paramValue (s, "fxRevType") == fx::ReverbStage::shimmer ? ", shimmer " + t ("fxRevShimmer") : juce::String()));
    if (on (FxRack::limiter))
        lines.add ("Limiter: +" + t ("fxLimInput") + " in, ceiling " + t ("fxLimCeiling") + ", release " + t ("fxLimRelease")
                   + "; now reducing " + juce::String (-limiter.grDb.load(), 1) + " dB");

    const juce::String head = "FX rack (Amp -> Cab -> EQ -> Comp -> Mod -> Delay -> Reverb -> Limiter), latency "
                            + juce::String (latAmp + latLim) + " samples.";
    if (lines.isEmpty())
        return head + "\nAll modules are off: the rack passes audio unchanged.";
    return head + "\n" + lines.joinIntoString ("\n");
}
