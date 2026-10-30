#include "SynthEngine.h"
#include "SynthInternal.h"
#include "ChordName.h"
#include <complex>

// Synth layer control panel. Sections: header (power + live summary), SOURCE & LAYER, TRACKING
// (Mono / Chord switch, live tracker readout), PITCH, OSCILLATOR, FILTER (response curve), NOTE
// DRIVE, ENVELOPES (ADSR shapes), LFO and a collapsible ADVANCED section. layout() measures and
// places in one pass, so idealHeight() is always exact.

namespace
{
    const juce::Colour& accent = Accent::synth;   // a reference: follows theme switches

    float readParam (juce::AudioProcessorValueTreeState& s, const char* id, float fallback = 0.0f)
    {
        if (auto* p = s.getRawParameterValue (id))
            return p->load();
        return fallback;
    }

    const char* const noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    juce::String hzShort (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz" : juce::String (hz, hz < 100.0f ? 1 : 0) + " Hz";
    }

    //==========================================================================
    // Live tracker readout: note + cents and a confidence bar (Mono), or the chord symbol and a
    // meter per heard note (Chord); the input level meter with the gate tick
    class TrackerDisplay : public juce::Component
    {
    public:
        TrackerDisplay (juce::AudioProcessorValueTreeState& s, SynthEngine& e, HelpTarget* h)
            : state (s), engine (e), hover (*this, "syn.tracker", h) {}

        void update()
        {
            const auto t = engine.tracker();
            // gentle smoothing so the bars don't flicker at 15 Hz
            conf += (t.confidence - conf) * 0.5f;
            level += (juce::jmax (-90.0f, t.levelDb) - level) * 0.6f;
            snap = t;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (Palette::viewBg);
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (Palette::panelEdge);
            g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

            auto area = getLocalBounds().reduced (10, 6);
            auto left = area.removeFromLeft (juce::jmin (96, area.getWidth() / 3));
            if (readParam (state, "synTrackMode") > 0.5f)
            {
                paintChord (g, area, left);
                return;
            }
            const bool v = snap.voiced && snap.midiNote >= 0;

            g.setColour (v ? accent : Palette::dim);
            g.setFont (Theme::font (24.0f, true));
            const juce::String name = v ? juce::String (noteNames[snap.midiNote % 12]) + juce::String (snap.midiNote / 12 - 1)
                                        : juce::String ("--");
            g.drawText (name, left.removeFromTop (30), juce::Justification::centredLeft);
            g.setFont (Theme::font (11.0f));
            g.setColour (v ? Palette::text : Palette::dim);
            const juce::String cents = v ? (snap.cents >= 0.0f ? "+" : "") + juce::String ((int) std::round (snap.cents)) + " cents"
                                         : juce::String ("no pitch");
            g.drawText (cents, left, juce::Justification::topLeft);

            area.removeFromLeft (8);
            auto line = area.removeFromTop (16);
            g.setColour (Palette::text);
            g.setFont (Theme::font (11.0f));
            juce::String info = v ? "f0 " + juce::String (snap.f0Hz, 1) + " Hz" : juce::String ("listening...");
            if (snap.synthF0Hz > 0.0f)
                info << "  ->  synth " << juce::String (snap.synthF0Hz, 1) << " Hz";
            g.drawText (info, line, juce::Justification::centredLeft);

            auto bar = [&g] (juce::Rectangle<int> rr, const juce::String& label, float frac, juce::Colour c, float tick)
            {
                g.setColour (Palette::dim);
                g.setFont (Theme::font (10.0f, true));
                g.drawText (label, rr.removeFromLeft (38), juce::Justification::centredLeft);
                auto b = rr.withSizeKeepingCentre (rr.getWidth(), 6).toFloat();
                g.setColour (Palette::grid);
                g.fillRoundedRectangle (b, 3.0f);
                g.setColour (c);
                g.fillRoundedRectangle (b.withWidth (b.getWidth() * juce::jlimit (0.0f, 1.0f, frac)), 3.0f);
                if (tick >= 0.0f)
                {
                    g.setColour (Palette::text);
                    const float x = b.getX() + b.getWidth() * tick;
                    g.drawLine (x, b.getY() - 3.0f, x, b.getBottom() + 3.0f, 1.2f);
                }
            };
            area.removeFromTop (2);
            bar (area.removeFromTop (14), "CONF", conf, v ? accent : Palette::dim, -1.0f);
            area.removeFromTop (2);
            levelBar (g, area.removeFromTop (14));
        }

    private:
        void levelBar (juce::Graphics& g, juce::Rectangle<int> rr) const
        {
            const float gate = readParam (state, "synGate", -55.0f);
            g.setColour (Palette::dim);
            g.setFont (Theme::font (10.0f, true));
            g.drawText ("LEVEL", rr.removeFromLeft (38), juce::Justification::centredLeft);
            auto b = rr.withSizeKeepingCentre (rr.getWidth(), 6).toFloat();
            g.setColour (Palette::grid);
            g.fillRoundedRectangle (b, 3.0f);
            g.setColour (level > gate ? Palette::green : Palette::dim);
            g.fillRoundedRectangle (b.withWidth (b.getWidth() * juce::jlimit (0.0f, 1.0f, (level + 90.0f) / 90.0f)), 3.0f);
            g.setColour (Palette::text);
            const float x = b.getX() + b.getWidth() * (gate + 90.0f) / 90.0f;
            g.drawLine (x, b.getY() - 3.0f, x, b.getBottom() + 3.0f, 1.2f);
        }

        // Chord mode: symbol + count on the left; the notes, one small meter each, and the input level
        void paintChord (juce::Graphics& g, juce::Rectangle<int> area, juce::Rectangle<int> left) const
        {
            const int n = snap.numNotes;
            const bool any = n > 0;
            g.setColour (any ? accent : Palette::dim);
            g.setFont (Theme::font (22.0f, true));
            g.drawFittedText (any ? chordname::symbol (snap.notes.data(), n) : juce::String ("--"),
                              left.removeFromTop (30), juce::Justification::centredLeft, 1, 0.6f);
            g.setFont (Theme::font (11.0f));
            g.setColour (any ? Palette::text : Palette::dim);
            const int maxN = (int) readParam (state, "synChordNotes", 5.0f);
            g.drawText (any ? juce::String (n) + " of " + juce::String (maxN) + (maxN == 1 ? " note" : " notes")
                            : juce::String ("no chord"),
                        left, juce::Justification::topLeft);

            area.removeFromLeft (8);
            auto row = area.removeFromTop (30);
            if (! any)
            {
                g.setColour (Palette::dim);
                g.setFont (Theme::font (11.0f));
                g.drawText ("listening for a chord...", row, juce::Justification::centredLeft);
            }
            const int cellW = juce::jmax (30, row.getWidth() / juce::jmax (1, maxN));
            for (int i = 0; i < n; ++i)
            {
                auto cell = row.removeFromLeft (cellW).reduced (2, 0);
                g.setColour (Palette::text);
                g.setFont (Theme::font (11.0f, true));
                g.drawText (chordname::noteName (snap.notes[(size_t) i]), cell.removeFromTop (16), juce::Justification::centredLeft);
                const float db = juce::Decibels::gainToDecibels (snap.noteLevels[(size_t) i], -100.0f);
                auto b = cell.withSizeKeepingCentre (cell.getWidth(), 5).toFloat();
                g.setColour (Palette::grid);
                g.fillRoundedRectangle (b, 2.5f);
                g.setColour (accent);
                g.fillRoundedRectangle (b.withWidth (b.getWidth() * juce::jlimit (0.05f, 1.0f, (db + 66.0f) / 60.0f)), 2.5f);
            }
            area.removeFromTop (2);
            levelBar (g, area.removeFromTop (14));
        }

        juce::AudioProcessorValueTreeState& state;
        SynthEngine& engine;
        widgets_detail::HoverReporter hover;
        TrackerSnapshot snap;
        float conf = 0.0f, level = -90.0f;
    };

    //==========================================================================
    // Filter magnitude response (analog prototypes of the voice filters), 20 Hz .. 20 kHz
    class FilterCurve : public juce::Component
    {
    public:
        FilterCurve (juce::AudioProcessorValueTreeState& s, HelpTarget* h) : state (s), hover (*this, "synCutoff", h) {}

        bool refresh()
        {
            const float t = readParam (state, "synFltType"), c = readParam (state, "synCutoff", 12000.0f),
                        q = readParam (state, "synReso", 10.0f);
            if (t == type && c == cutoff && q == reso)
                return false;
            type = t; cutoff = c; reso = q;
            repaint();
            return true;
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (Palette::viewBg);
            g.fillRoundedRectangle (r, 6.0f);
            const auto plot = r.reduced (4.0f);

            // grid: 100 Hz, 1 kHz, 10 kHz and 0 dB
            auto xOf = [&plot] (float hz) { return plot.getX() + plot.getWidth() * std::log (hz / 20.0f) / std::log (1000.0f); };
            auto yOf = [&plot] (float db) { return plot.getY() + plot.getHeight() * (18.0f - db) / 54.0f; };
            g.setColour (Palette::grid);
            for (float f : { 100.0f, 1000.0f, 10000.0f })
                g.drawVerticalLine ((int) xOf (f), plot.getY(), plot.getBottom());
            g.drawHorizontalLine ((int) yOf (0.0f), plot.getX(), plot.getRight());

            const int mode = (int) type;
            const float res = reso * 0.01f;
            const float k = 4.1f * std::pow (res, 0.8f);
            const float svfK = 1.0f / (0.6f * std::pow (40.0f, res));
            juce::Path p;
            const int steps = juce::jmax (32, (int) plot.getWidth());
            for (int i = 0; i <= steps; ++i)
            {
                const float hz = 20.0f * std::pow (1000.0f, (float) i / (float) steps);
                const std::complex<float> s (0.0f, hz / juce::jmax (16.0f, cutoff));
                std::complex<float> h;
                if (mode == 0)
                {
                    const auto p1 = 1.0f + s;
                    h = (1.0f + 0.5f * k) / (p1 * p1 * p1 * p1 + k);
                }
                else
                {
                    const auto d = s * s + svfK * s + 1.0f;
                    switch (mode)
                    {
                        case 1:  h = 1.0f / d; break;
                        case 2:  h = svfK * s / d; break;
                        case 3:  h = s * s / d; break;
                        default: h = (s * s + 1.0f) / d; break;
                    }
                }
                const float db = juce::jlimit (-36.0f, 18.0f, 20.0f * std::log10 (std::abs (h) + 1.0e-6f));
                const float x = plot.getX() + plot.getWidth() * (float) i / (float) steps;
                if (i == 0) p.startNewSubPath (x, yOf (db)); else p.lineTo (x, yOf (db));
            }
            juce::Path fill (p);
            fill.lineTo (plot.getRight(), plot.getBottom());
            fill.lineTo (plot.getX(), plot.getBottom());
            fill.closeSubPath();
            g.setColour (accent.withAlpha (0.15f));
            g.fillPath (fill);
            g.setColour (accent);
            g.strokePath (p, juce::PathStrokeType (1.6f));

            g.setColour (Palette::dim);
            g.setFont (Theme::font (10.0f));
            g.drawText (hzShort (cutoff), plot.reduced (2.0f, 1.0f), juce::Justification::topRight);
        }

    private:
        juce::AudioProcessorValueTreeState& state;
        widgets_detail::HoverReporter hover;
        float type = -1.0f, cutoff = 0.0f, reso = 0.0f;
    };

    //==========================================================================
    // Mini ADSR shape: segment widths ~ sqrt(time) so short and long times both read well
    class AdsrView : public juce::Component
    {
    public:
        AdsrView (juce::AudioProcessorValueTreeState& s, juce::StringArray paramIds) : state (s), ids (paramIds)
        {
            setInterceptsMouseClicks (false, false);
        }

        bool refresh()
        {
            std::array<float, 4> v {};
            for (int i = 0; i < 4; ++i)
                v[(size_t) i] = readParam (state, ids[i].toRawUTF8());
            if (v == values)
                return false;
            values = v;
            repaint();
            return true;
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (Palette::viewBg);
            g.fillRoundedRectangle (r, 5.0f);
            const auto plot = r.reduced (6.0f, 4.0f);

            const float a = std::sqrt (values[0]), d = std::sqrt (values[1]), rel = std::sqrt (values[3]);
            const float sus = values[2] * 0.01f;
            const float hold = 0.25f * (a + d + rel) + 1.0f;
            const float total = a + d + hold + rel;
            const float sx = plot.getWidth() / juce::jmax (1.0f, total);
            auto X = [&] (float t) { return plot.getX() + t * sx; };
            auto Y = [&] (float l) { return plot.getBottom() - l * plot.getHeight(); };

            juce::Path p;
            p.startNewSubPath (X (0), Y (0));
            const int n = 16;
            for (int i = 1; i <= n; ++i)   // convex attack
            {
                const float u = (float) i / n;
                p.lineTo (X (a * u), Y (1.2f * (1.0f - std::pow (6.0f, -u))));   // reaches 1 at u = 1
            }
            for (int i = 1; i <= n; ++i)   // exponential decay
            {
                const float u = (float) i / n;
                p.lineTo (X (a + d * u), Y (sus + (1.0f - sus) * std::pow (0.001f, u)));
            }
            p.lineTo (X (a + d + hold), Y (sus));
            for (int i = 1; i <= n; ++i)
            {
                const float u = (float) i / n;
                p.lineTo (X (a + d + hold + rel * u), Y (sus * std::pow (0.001f, u)));
            }
            g.setColour (accent.withAlpha (isEnabled() ? 0.9f : 0.35f));
            g.strokePath (p, juce::PathStrokeType (1.5f));
        }

    private:
        juce::AudioProcessorValueTreeState& state;
        juce::StringArray ids;
        std::array<float, 4> values { -1.0f, -1.0f, -1.0f, -1.0f };
    };

    //==========================================================================
    // Clickable header strip of the Advanced section (the title itself is drawn by drawSection)
    class SectionToggle : public juce::Component
    {
    public:
        explicit SectionToggle (HelpTarget* h) : hover (*this, "syn.advanced", h) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

        void paint (juce::Graphics& g) override
        {
            g.setColour (isMouseOver() ? Palette::text : Palette::dim);
            g.setFont (Theme::font (11.0f, true));
            g.drawText (open ? "HIDE  -" : "SHOW  +", getLocalBounds().reduced (14, 0), juce::Justification::centredRight);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (getLocalBounds().contains (e.getPosition()))
            {
                open = ! open;
                repaint();
                if (onToggle)
                    onToggle();
            }
        }
        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override  { repaint(); }

        bool open = false;
        std::function<void()> onToggle;

    private:
        widgets_detail::HoverReporter hover;
    };
}

//==============================================================================
struct SynthPanel::Impl : private juce::Timer
{
    Impl (SynthPanel& owner, juce::AudioProcessorValueTreeState& s, SynthEngine& e, HelpTarget* h)
        : panel (owner), state (s), engine (e), help (h),
          power (s, "synOn", h, accent),
          tracker (s, e, h), curve (s, h),
          ampView (s, { "synAmpA", "synAmpD", "synAmpS", "synAmpR" }),
          fltView (s, { "synFltA", "synFltD", "synFltS", "synFltR" }),
          advToggle (h)
    {
        panel.addAndMakeVisible (power);
        summary.setFont (Theme::font (11.5f));
        summary.setColour (juce::Label::textColourId, Palette::text);
        summary.setMinimumHorizontalScale (0.85f);
        summary.setInterceptsMouseClicks (false, false);
        panel.addAndMakeVisible (summary);

        source = segmented ("synSource");
        layer = segmented ("synLayer");
        sourceLabel = label ("SOURCE");
        layerLabel = label ("LAYER MODE");
        layerDesc = label ("");
        layerDesc->setFont (Theme::font (10.5f));
        inLevel = knob ("synInLevel", "INPUT");
        synLevel = knob ("synLevel", "SYNTH");
        width = knob ("synWidth", "WIDTH");

        trackMode = segmented ("synTrackMode");
        panel.addAndMakeVisible (tracker);
        range = choice ("synRange", "RANGE");
        gate = knob ("synGate", "GATE");
        follow = knob ("synFollow", "FOLLOW INPUT");
        chordNotes = knob ("synChordNotes", "NOTES");
        chordSens = knob ("synChordSens", "SENSITIVITY");
        pickAttack = knob ("synPickAttack", "PICK ATTACK");
        trackDesc = label ("");
        trackDesc->setFont (Theme::font (10.5f));
        trackDesc->setJustificationType (juce::Justification::centredLeft);

        octave = knob ("synOctave", "OCTAVE", true);
        semi = knob ("synSemi", "SEMITONE", true);
        fine = knob ("synFine", "FINE", true);
        glide = knob ("synGlide", "GLIDE");
        scale = choice ("synScale", "SCALE");
        key = choice ("synKey", "KEY");

        unison = knob ("synUnison", "UNISON");
        detune = knob ("synDetune", "DETUNE");
        spread = knob ("synSpread", "SPREAD");
        warmth = knob ("synWarmth", "WARMTH");
        sub = knob ("synSub", "SUB");
        subOct = segmented ("synSubOct");
        subShape = segmented ("synSubShape");
        noise = knob ("synNoise", "NOISE");
        noiseCol = knob ("synNoiseColour", "COLOUR", true);

        fltType = segmented ("synFltType");
        panel.addAndMakeVisible (curve);
        fltDrive = knob ("synFltDrive", "DRIVE");
        cutoff = knob ("synCutoff", "CUTOFF");
        reso = knob ("synReso", "RESONANCE");
        fltEnv = knob ("synFltEnv", "ENV AMOUNT", true);
        keyTrack = knob ("synKeyTrack", "KEY TRACK");

        driveType = segmented ("synDriveType");
        drive = knob ("synDrive", "DRIVE");
        driveTone = knob ("synDriveTone", "TONE");
        driveDesc = label ("Distorts every note on its own before the notes are mixed, "
                           "so full chords stay clear even at high gain.");
        driveDesc->setFont (Theme::font (10.5f));
        driveDesc->setJustificationType (juce::Justification::centredLeft);

        ampLabel = label ("AMP ENVELOPE");
        fltLabel = label ("FILTER ENVELOPE");
        panel.addAndMakeVisible (ampView);
        panel.addAndMakeVisible (fltView);
        ampKnobs = { knob ("synAmpA", "ATTACK"), knob ("synAmpD", "DECAY"), knob ("synAmpS", "SUSTAIN"), knob ("synAmpR", "RELEASE") };
        fltKnobs = { knob ("synFltA", "ATTACK"), knob ("synFltD", "DECAY"), knob ("synFltS", "SUSTAIN"), knob ("synFltR", "RELEASE") };

        lfoShape = segmented ("synLfoShape");
        lfoKnobs = { knob ("synLfoRate", "RATE"), knob ("synLfoPitch", "TO PITCH"),
                     knob ("synLfoCutoff", "TO CUTOFF"), knob ("synLfoAmp", "TO AMP") };

        panel.addAndMakeVisible (advToggle);
        advToggle.onToggle = [this] { relayout(); };
        advKnobs = { knob ("synOnsetSens", "ONSET SENS"), knob ("synPhaseRand", "PHASE RAND"),
                     knob ("synVelSens", "VELOCITY"), knob ("synBendRange", "BEND RANGE"), knob ("synVoices", "VOICES") };
        midiNote = label ("Velocity, bend range and voices apply to MIDI notes.");
        midiNote->setFont (Theme::font (10.5f));

        refresh();
        startTimerHz (15);
    }

    ~Impl() override { stopTimer(); }

    std::unique_ptr<ParamKnob> knob (const char* id, const char* text, bool bipolar = false)
    {
        auto k = std::make_unique<ParamKnob> (state, id, text, help, bipolar);
        panel.addAndMakeVisible (*k);
        return k;
    }

    std::unique_ptr<ParamChoice> choice (const char* id, const char* text)
    {
        auto c = std::make_unique<ParamChoice> (state, id, text, help);
        panel.addAndMakeVisible (*c);
        return c;
    }

    std::unique_ptr<ParamSegmented> segmented (const char* id)
    {
        auto c = std::make_unique<ParamSegmented> (state, id, help, accent);
        panel.addAndMakeVisible (*c);
        return c;
    }

    std::unique_ptr<juce::Label> label (const juce::String& text)
    {
        auto l = std::make_unique<juce::Label> (juce::String(), text);
        l->setFont (Theme::font (11.0f, true));
        l->setColour (juce::Label::textColourId, Palette::dim);
        l->setInterceptsMouseClicks (false, false);
        l->setBorderSize ({ 0, 0, 0, 0 });
        panel.addAndMakeVisible (*l);
        return l;
    }

    void relayout()
    {
        panel.resized();
        panel.repaint();
        if (auto* vp = panel.findParentComponentOfClass<juce::Viewport>())
            vp->resized();   // the editor's viewport refits the panel to idealHeight()
    }

    float param (const char* id, float fallback = 0.0f) const { return readParam (state, id, fallback); }

    static void enable (juce::Component* c, bool on)
    {
        if (c == nullptr)
            return;
        c->setEnabled (on);
        c->setAlpha (on ? 1.0f : 0.4f);
    }

    void timerCallback() override { refresh(); }

    void refresh()
    {
        tracker.update();
        curve.refresh();
        ampView.refresh();
        fltView.refresh();

        const bool on = param ("synOn") > 0.5f;
        const int src = (int) param ("synSource");
        const int mode = (int) param ("synLayer");
        const bool inputSrc = src != 1, midiSrc = src != 0;
        const int sc = (int) param ("synScale");

        const bool chordMode = param ("synTrackMode") > 0.5f;
        enable (inLevel.get(), mode != 2);
        enable (trackMode.get(), inputSrc);
        enable (range.get(), inputSrc);
        enable (gate.get(), inputSrc);
        enable (follow.get(), inputSrc);
        enable (chordNotes.get(), inputSrc && chordMode);
        enable (chordSens.get(), inputSrc && chordMode);
        enable (pickAttack.get(), inputSrc);
        enable (scale.get(), inputSrc);
        const bool driveOn = param ("synDrive") > 0.05f;
        enable (driveTone.get(), driveOn);
        trackDesc->setText (! inputSrc ? juce::String ("Tracking is for the Input source.")
                                       : chordMode ? juce::String ("Up to 6 notes at once, one voice per string. Locks ~30-50 ms after a strum.")
                                                   : juce::String ("One note at a time. Reacts in a few ms and follows every bend."),
                            juce::dontSendNotification);
        enable (key.get(), inputSrc && sc >= 2);
        const bool uni = param ("synUnison", 1.0f) > 1.5f;
        enable (detune.get(), uni);
        enable (spread.get(), uni);
        const bool subOn = param ("synSub") > 0.0f && mode != 1;
        enable (sub.get(), mode != 1);
        enable (subOct.get(), subOn);
        enable (subShape.get(), subOn);
        enable (noiseCol.get(), param ("synNoise") > 0.0f);
        const bool envOn = std::abs (param ("synFltEnv")) > 0.005f;
        enable (&fltView, envOn);
        for (auto& k : fltKnobs)
            enable (k.get(), envOn);
        enable (advKnobs[0].get(), inputSrc);
        for (int i = 2; i < 5; ++i)
            enable (advKnobs[(size_t) i].get(), midiSrc);

        static const char* const layerText[] = {
            "Input and synth side by side.",
            "Adds the wave's harmonics 2..64 on top of the input (exciter).",
            "Synth only - the input is muted." };
        layerDesc->setText (layerText[juce::jlimit (0, 2, mode)], juce::dontSendNotification);

        // one-line live summary
        juce::String s;
        if (! on)
        {
            s = "Off - switch on to play your input as a synth.";
        }
        else
        {
            const auto t = engine.tracker();
            if (inputSrc && chordMode)
            {
                if (t.numNotes > 0)
                    s << "Chord " << chordname::symbol (t.notes.data(), t.numNotes) << " ("
                      << chordname::noteList (t.notes.data(), t.numNotes) << ")";
                else
                    s << "Waiting for a chord";
            }
            else if (inputSrc)
            {
                if (t.voiced && t.midiNote >= 0)
                    s << "Tracking " << noteNames[t.midiNote % 12] << (t.midiNote / 12 - 1);
                else
                    s << "Waiting for a note";
                if (t.synthF0Hz > 0.0f)
                    s << " -> " << juce::String (t.synthF0Hz, 1) << " Hz";
            }
            if (midiSrc)
                s << (inputSrc ? "  +  " : "") << "MIDI";
            static const char* const modeNames[] = { "Blend", "Overtones", "Replace" };
            const int voicesNow = engine.activeVoices();
            s << "  |  " << modeNames[juce::jlimit (0, 2, mode)] << "  |  " << voicesNow << (voicesNow == 1 ? " voice" : " voices");
        }
        summary.setText (s, juce::dontSendNotification);
        summary.setColour (juce::Label::textColourId, on ? Palette::text : Palette::dim);
    }

    // Lays the panel out for `width`; with apply = false it only measures (for idealHeight)
    int layout (int widthPx, bool apply)
    {
        const int pad = 12;
        const int inner = juce::jmax (200, widthPx - pad * 2);
        const int cw = inner / 4;
        const int kh = Theme::knobCellHeight - 4;
        const int gap = 10;
        int y = 0;
        auto place = [apply] (juce::Component* c, juce::Rectangle<int> r) { if (apply && c != nullptr) c->setBounds (r); };
        auto knobRow = [&] (std::initializer_list<juce::Component*> comps)
        {
            int i = 0;
            for (auto* c : comps)
                place (c, { pad + (i++) * cw, y, cw, kh });
            y += Theme::knobCellHeight;
        };
        auto begin = [&] { const int top = y; y += Theme::sectionTitle; return top; };
        auto end = [&] (juce::Rectangle<int>& rect, int top) { y += 4; rect = { 0, top, widthPx, y - top }; y += gap; };

        // header
        {
            const int top = y;
            place (&power, { widthPx - pad - 24, top + 5, 24, 24 });
            y += Theme::sectionTitle;
            place (&summary, { pad, y - 2, inner, 20 });
            y += 20;
            end (headerRect, top);
        }

        // source & layer
        {
            const int top = begin();
            place (sourceLabel.get(), { pad, y, inner, 16 });
            y += 16;
            place (source.get(), { pad, y, inner, 26 });
            y += 32;
            place (layerLabel.get(), { pad, y, inner, 16 });
            y += 16;
            place (layer.get(), { pad, y, inner, 26 });
            y += 28;
            place (layerDesc.get(), { pad, y, inner, 16 });
            y += 20;
            knobRow ({ inLevel.get(), synLevel.get(), width.get() });
            end (sourceRect, top);
        }

        // tracking
        {
            const int top = begin();
            place (trackMode.get(), { pad, y, inner, 26 });
            y += 32;
            place (&tracker, { pad, y, inner, 60 });
            y += 66;
            place (range.get(), { pad, y + 22, 2 * cw - 8, 40 });
            place (gate.get(), { pad + 2 * cw, y, cw, kh });
            place (follow.get(), { pad + 3 * cw, y, cw, kh });
            y += Theme::knobCellHeight;
            place (chordNotes.get(), { pad, y, cw, kh });
            place (chordSens.get(), { pad + cw, y, cw, kh });
            place (pickAttack.get(), { pad + 2 * cw, y, cw, kh });
            place (trackDesc.get(), { pad + 3 * cw + 6, y + 10, cw - 10, 60 });
            y += Theme::knobCellHeight;
            end (trackRect, top);
        }

        // pitch
        {
            const int top = begin();
            knobRow ({ octave.get(), semi.get(), fine.get(), glide.get() });
            place (scale.get(), { pad, y, 2 * cw - 8, 40 });
            place (key.get(), { pad + 2 * cw, y, 2 * cw - 8, 40 });
            y += Theme::choiceHeight;
            end (pitchRect, top);
        }

        // oscillator
        {
            const int top = begin();
            knobRow ({ unison.get(), detune.get(), spread.get(), warmth.get() });
            place (sub.get(), { pad, y, cw, kh });
            place (subOct.get(), { pad + cw + 2, y + 16, cw - 6, 24 });
            place (subShape.get(), { pad + cw + 2, y + 46, cw - 6, 24 });
            place (noise.get(), { pad + 2 * cw, y, cw, kh });
            place (noiseCol.get(), { pad + 3 * cw, y, cw, kh });
            y += Theme::knobCellHeight;
            end (oscRect, top);
        }

        // filter
        {
            const int top = begin();
            place (fltType.get(), { pad, y, inner, 26 });
            y += 32;
            place (&curve, { pad, y + 2, 3 * cw - 8, kh - 4 });
            place (fltDrive.get(), { pad + 3 * cw, y, cw, kh });
            y += Theme::knobCellHeight;
            knobRow ({ cutoff.get(), reso.get(), fltEnv.get(), keyTrack.get() });
            end (filterRect, top);
        }

        // per-note drive
        {
            const int top = begin();
            place (driveType.get(), { pad, y, inner, 26 });
            y += 32;
            place (drive.get(), { pad, y, cw, kh });
            place (driveTone.get(), { pad + cw, y, cw, kh });
            place (driveDesc.get(), { pad + 2 * cw + 6, y + 10, 2 * cw - 10, 56 });
            y += Theme::knobCellHeight;
            end (driveRect, top);
        }

        // envelopes
        {
            const int top = begin();
            place (ampLabel.get(), { pad, y, inner, 16 });
            y += 18;
            place (&ampView, { pad, y, inner, 34 });
            y += 40;
            knobRow ({ ampKnobs[0].get(), ampKnobs[1].get(), ampKnobs[2].get(), ampKnobs[3].get() });
            place (fltLabel.get(), { pad, y, inner, 16 });
            y += 18;
            place (&fltView, { pad, y, inner, 34 });
            y += 40;
            knobRow ({ fltKnobs[0].get(), fltKnobs[1].get(), fltKnobs[2].get(), fltKnobs[3].get() });
            end (envRect, top);
        }

        // lfo
        {
            const int top = begin();
            place (lfoShape.get(), { pad, y, inner, 26 });
            y += 32;
            knobRow ({ lfoKnobs[0].get(), lfoKnobs[1].get(), lfoKnobs[2].get(), lfoKnobs[3].get() });
            end (lfoRect, top);
        }

        // advanced (collapsible)
        {
            const int top = y;
            place (&advToggle, { 0, top, widthPx, Theme::sectionTitle + 4 });
            y += Theme::sectionTitle + 4;
            const bool open = advToggle.open;
            for (auto& k : advKnobs)
                if (apply && k != nullptr)
                    k->setVisible (open);
            if (apply)
                midiNote->setVisible (open);
            if (open)
            {
                knobRow ({ advKnobs[0].get(), advKnobs[1].get(), advKnobs[2].get(), advKnobs[3].get() });
                place (advKnobs[4].get(), { pad, y, cw, kh });
                place (midiNote.get(), { pad + cw + 6, y + 20, inner - cw - 6, 32 });
                y += Theme::knobCellHeight;
            }
            if (open)
                y += 4;
            advRect = { 0, top, widthPx, y - top };
        }
        return y;
    }

    SynthPanel& panel;
    juce::AudioProcessorValueTreeState& state;
    SynthEngine& engine;
    HelpTarget* help;

    ParamPowerButton power;
    juce::Label summary;
    std::unique_ptr<ParamSegmented> source, layer, trackMode, subOct, subShape, fltType, driveType, lfoShape;
    std::unique_ptr<juce::Label> sourceLabel, layerLabel, layerDesc, trackDesc, driveDesc, ampLabel, fltLabel, midiNote;
    std::unique_ptr<ParamKnob> inLevel, synLevel, width, gate, follow, chordNotes, chordSens, pickAttack,
                               octave, semi, fine, glide,
                               unison, detune, spread, warmth, sub, noise, noiseCol,
                               fltDrive, cutoff, reso, fltEnv, keyTrack, drive, driveTone;
    std::unique_ptr<ParamChoice> range, scale, key;
    std::array<std::unique_ptr<ParamKnob>, 4> ampKnobs, fltKnobs, lfoKnobs;
    std::array<std::unique_ptr<ParamKnob>, 5> advKnobs;
    TrackerDisplay tracker;
    FilterCurve curve;
    AdsrView ampView, fltView;
    SectionToggle advToggle;

    juce::Rectangle<int> headerRect, sourceRect, trackRect, pitchRect, oscRect, filterRect, driveRect, envRect, lfoRect, advRect;
};

//==============================================================================
SynthPanel::SynthPanel (juce::AudioProcessorValueTreeState& s, SynthEngine& e, HelpTarget* h)
{
    impl = std::make_unique<Impl> (*this, s, e, h);
}

SynthPanel::~SynthPanel() = default;

int SynthPanel::idealHeight (int width) const
{
    return impl->layout (width, false);
}

void SynthPanel::paint (juce::Graphics& g)
{
    Theme::drawSection (g, impl->headerRect, "SYNTH LAYER", accent);
    Theme::drawSection (g, impl->sourceRect, "SOURCE & LAYER", accent);
    Theme::drawSection (g, impl->trackRect, "TRACKING", accent);
    Theme::drawSection (g, impl->pitchRect, "PITCH", accent);
    Theme::drawSection (g, impl->oscRect, "OSCILLATOR", accent);
    Theme::drawSection (g, impl->filterRect, "FILTER", accent);
    Theme::drawSection (g, impl->driveRect, "NOTE DRIVE", accent);
    Theme::drawSection (g, impl->envRect, "ENVELOPES", accent);
    Theme::drawSection (g, impl->lfoRect, "LFO", accent);
    Theme::drawSection (g, impl->advRect, "ADVANCED", accent);
}

void SynthPanel::resized()
{
    impl->layout (getWidth(), true);
}
