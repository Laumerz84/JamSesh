#include "SculptView.h"
#include "SculptTools.h"

namespace
{
    using namespace sculpt_tools;

    void styleButton (juce::TextButton& b, bool toggles)
    {
        b.setColour (juce::TextButton::buttonColourId, Palette::bg);
        b.setColour (juce::TextButton::buttonOnColourId, Accent::sculpt.withAlpha (0.42f));
        b.setColour (juce::TextButton::textColourOffId, Palette::text);
        b.setColour (juce::TextButton::textColourOnId, Palette::bright);
        b.setClickingTogglesState (toggles);
    }

    const char* const howTo =
        "1. Turn the synth layer on and feed a sustained note (or play MIDI).\n"
        "2. Capture input to start from your own sound, or load a wave.\n"
        "3. On the wave: drag a point to bend it (mouse wheel = width), or switch to Draw and paint a cycle.\n"
        "4. Overtone bars: drag = level, shift-drag = paint, right-drag = phase, double-click = zero. "
        "Hover a bar to see its sine inside the wave.\n"
        "5. Edit table B and sweep MORPH. TILT, ODD/EVEN, FORMANT, STRETCH and COMB reshape the whole spectrum live and can be automated.";

    juce::String phaseDescription (int mode)
    {
        switch (mode)
        {
            case 1:  return "Aligned: every partial peaks at the same instant - sharp, buzzy, maximum punch.";
            case 2:  return "Spread: phases scattered (Schroeder) - the smoothest, lowest-crest wave.";
            default: return "Keep: each harmonic uses the phase in its table - the shape you drew.";
        }
    }
}

//==============================================================================
struct SculptPanel::Impl : private juce::Timer
{
    Impl (SculptPanel& owner, ClodScopeProcessor& p, HelpTarget* h)
        : panel (owner), proc (p), model (p.sculpt), help (h)
    {
        fillWaveMenu (waveBox);
        waveBox.onChange = [this]
        {
            const int idx = waveBox.getSelectedId() - 1;
            if (idx >= 0)
            {
                model.setTable (activeTable (proc.apvts), SculptModel::makeWave (idx), true);
                flash ("Loaded " + SculptModel::waveNames()[idx] + " into table " + juce::String (activeTable (proc.apvts) == 0 ? "A" : "B"));
            }
            waveBox.setSelectedId (0, juce::dontSendNotification);
        };
        waveLabel.setText ("LOAD WAVE INTO THE ACTIVE TABLE", juce::dontSendNotification);
        waveLabel.setFont (Theme::font (11.0f, true));
        waveLabel.setInterceptsMouseClicks (false, false);

        for (auto* b : { &aBtn, &bBtn })
        {
            styleButton (*b, true);
            b->setRadioGroupId (0x5c11);
        }
        aBtn.setConnectedEdges (juce::Button::ConnectedOnRight);
        bBtn.setConnectedEdges (juce::Button::ConnectedOnLeft);
        aBtn.onClick = [this] { if (aBtn.getToggleState()) setActiveTable (proc.apvts, 0); refresh(); };
        bBtn.onClick = [this] { if (bBtn.getToggleState()) setActiveTable (proc.apvts, 1); refresh(); };

        for (auto* b : { &copyBtn, &captureBtn, &undoBtn, &redoBtn })
            styleButton (*b, false);
        captureBtn.setColour (juce::TextButton::buttonColourId, Accent::sculpt.withAlpha (0.22f));
        copyBtn.onClick = [this]
        {
            const int a = activeTable (proc.apvts);
            model.setTable (1 - a, model.getTable (a), true);
            flash (a == 0 ? "Copied table A to table B" : "Copied table B to table A");
        };
        captureBtn.onClick = [this]
        {
            BaseTable t;
            juce::String msg;
            if (captureInput (proc, t, msg))
                model.setTable (activeTable (proc.apvts), t, true);
            flash (msg, 6000);
        };
        undoBtn.onClick = [this] { flash (model.undo() ? "Undo" : "Nothing to undo"); };
        redoBtn.onClick = [this] { flash (model.redo() ? "Redo" : "Nothing to redo"); };

        status.setFont (Theme::font (11.0f));
        status.setColour (juce::Label::textColourId, Palette::dim);
        status.setInterceptsMouseClicks (false, false);
        status.setMinimumHorizontalScale (1.0f);

        const std::pair<juce::Component*, const char*> tools[] = {
            { &waveBox, "sc.factory" }, { &aBtn, "sc.ab" }, { &bBtn, "sc.ab" }, { &copyBtn, "sc.copy" },
            { &captureBtn, "sc.capture" }, { &undoBtn, "sc.undo" }, { &redoBtn, "sc.redo" } };
        for (auto [c, id] : tools)
        {
            panel.addAndMakeVisible (*c);
            hovers.add (new widgets_detail::HoverReporter (*c, id, help));
        }
        panel.addAndMakeVisible (waveLabel);
        panel.addAndMakeVisible (status);

        spectrum = { knob ("scMorph", "MORPH A -> B"), knob ("scTilt", "TILT", true),
                     knob ("scOddEven", "ODD / EVEN", true), knob ("scPartials", "PARTIALS") };
        formant  = { knob ("scFormantPos", "POSITION"), knob ("scFormantGain", "GAIN", true), knob ("scFormantWidth", "WIDTH") };
        character = { knob ("scStretch", "STRETCH"), knob ("scComb", "COMB"), knob ("scCombOffset", "COMB OFFSET") };

        for (auto* l : { &formantNote, &characterNote, &phaseNote })
        {
            l->setFont (Theme::font (10.5f));
            l->setColour (juce::Label::textColourId, Palette::dim);
            l->setJustificationType (juce::Justification::topLeft);
            l->setMinimumHorizontalScale (1.0f);
            l->setInterceptsMouseClicks (false, false);
            panel.addAndMakeVisible (*l);
        }
        formantNote.setText ("A resonant bump over the harmonics (vowel colour). 0 dB = off.", juce::dontSendNotification);
        characterNote.setText ("Stretch = inharmonic (piano, bell). Comb = PWM-like notches.", juce::dontSendNotification);

        if (proc.apvts.getParameter ("scPhase") != nullptr)
        {
            phase = std::make_unique<ParamSegmented> (proc.apvts, "scPhase", help, Accent::sculpt);
            panel.addAndMakeVisible (*phase);
        }

        refresh();
        startTimerHz (10);
    }

    ~Impl() override { stopTimer(); }

    std::unique_ptr<ParamKnob> knob (const char* id, const char* label, bool bipolar = false)
    {
        if (proc.apvts.getParameter (id) == nullptr)
            return nullptr;
        auto k = std::make_unique<ParamKnob> (proc.apvts, id, label, help, bipolar);
        panel.addAndMakeVisible (*k);
        return k;
    }

    float param (const char* id, float fallback) const
    {
        if (auto* v = proc.apvts.getRawParameterValue (id))
            return v->load();
        return fallback;
    }

    void flash (const juce::String& msg, int ms = 3000)
    {
        statusMsg = msg;
        statusUntil = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
        refresh();
    }

    void timerCallback() override
    {
        if (panel.isShowing())
            refresh();
    }

    void refresh()
    {
        const int a = activeTable (proc.apvts);
        aBtn.setToggleState (a == 0, juce::dontSendNotification);
        bBtn.setToggleState (a == 1, juce::dontSendNotification);
        copyBtn.setButtonText (a == 0 ? "Copy A -> B" : "Copy B -> A");

        juce::String s;
        auto col = Palette::dim;
        if (juce::Time::getMillisecondCounter() < statusUntil)
        {
            s = statusMsg;
            col = Accent::sculpt.brighter (0.3f);
        }
        else
        {
            const int morph = (int) std::round (param ("scMorph", 0.0f));
            const int heard = a == 0 ? 100 - morph : morph;
            s = "Editing " + juce::String (a == 0 ? "A" : "B") + " - heard " + juce::String (heard) + " % (morph " + juce::String (morph) + " %)";
            if (heard == 0)
            {
                s << ". Raise MORPH to hear it.";
                col = Palette::amber;
            }
        }
        status.setText (s, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, col);

        auto enable = [] (ParamKnob* k, bool on)
        {
            if (k != nullptr)
            {
                k->setEnabled (on);
                k->setAlpha (on ? 1.0f : 0.4f);
            }
        };
        const bool formantOn = std::abs (param ("scFormantGain", 0.0f)) > 0.01f;
        enable (formant[0].get(), formantOn);
        enable (formant[2].get(), formantOn);
        enable (character[2].get(), param ("scComb", 0.0f) > 0.05f);
        phaseNote.setText (phaseDescription ((int) std::round (param ("scPhase", 0.0f))), juce::dontSendNotification);
    }

    juce::TextLayout hintLayout (int width) const
    {
        juce::AttributedString as;
        as.setWordWrap (juce::AttributedString::byWord);
        as.setLineSpacing (3.0f);
        as.append (howTo, Theme::font (11.0f), Palette::dim);
        juce::TextLayout tl;
        tl.createLayout (as, (float) juce::jmax (40, width));
        return tl;
    }

    // Lays the panel out for `width`; with apply = false it only measures (for idealHeight)
    int layout (int width, bool apply)
    {
        const int pad = 12, gap = 10;
        const int inner = width - pad * 2;
        const int cw = inner / 4;
        const int kh = Theme::knobCellHeight - 4;
        int y = 0;
        auto place = [apply] (juce::Component* c, juce::Rectangle<int> r) { if (apply && c != nullptr) c->setBounds (r); };

        // --- tables ---
        int top = y;
        y += Theme::sectionTitle + 2;
        place (&waveLabel, { pad, y, inner, 16 });
        place (&waveBox, { pad, y + 16, inner, 24 });
        y += 46;
        {
            const int abW = 34;
            place (&aBtn, { pad, y, abW, 26 });
            place (&bBtn, { pad + abW, y, abW, 26 });
            const int rest = inner - 2 * abW - 8;
            const int copyW = rest * 45 / 100;
            place (&copyBtn, { pad + 2 * abW + 8, y, copyW, 26 });
            place (&captureBtn, { pad + 2 * abW + 8 + copyW + 6, y, rest - copyW - 6, 26 });
        }
        y += 32;
        place (&undoBtn, { pad, y + 3, 56, 24 });
        place (&redoBtn, { pad + 60, y + 3, 56, 24 });
        place (&status, { pad + 124, y, inner - 124, 30 });
        y += 38;
        tablesRect = { 0, top, width, y - top };
        y += gap;

        // --- spectrum ---
        top = y;
        y += Theme::sectionTitle;
        for (int i = 0; i < 4; ++i)
            place (spectrum[(size_t) i].get(), { pad + i * cw, y, cw, kh });
        y += Theme::knobCellHeight;
        spectrumRect = { 0, top, width, y - top };
        y += gap;

        // --- formant ---
        top = y;
        y += Theme::sectionTitle;
        for (int i = 0; i < 3; ++i)
            place (formant[(size_t) i].get(), { pad + i * cw, y, cw, kh });
        place (&formantNote, { pad + 3 * cw + 4, y + 14, inner - 3 * cw - 4, kh - 18 });
        y += Theme::knobCellHeight;
        formantRect = { 0, top, width, y - top };
        y += gap;

        // --- character ---
        top = y;
        y += Theme::sectionTitle;
        for (int i = 0; i < 3; ++i)
            place (character[(size_t) i].get(), { pad + i * cw, y, cw, kh });
        place (&characterNote, { pad + 3 * cw + 4, y + 14, inner - 3 * cw - 4, kh - 18 });
        y += Theme::knobCellHeight;
        characterRect = { 0, top, width, y - top };
        y += gap;

        // --- phase ---
        top = y;
        y += Theme::sectionTitle + 2;
        place (phase.get(), { pad, y, inner, 26 });
        y += 30;
        place (&phaseNote, { pad, y, inner, 30 });
        y += 34;
        phaseRect = { 0, top, width, y - top };
        y += gap;

        // --- how to ---
        top = y;
        y += Theme::sectionTitle;
        const int textW = inner;
        const int th = (int) std::ceil (hintLayout (textW).getHeight());
        hintRect = { pad, y, textW, th };
        y += th + 12;
        howRect = { 0, top, width, y - top };
        return y;
    }

    SculptPanel& panel;
    ClodScopeProcessor& proc;
    SculptModel& model;
    HelpTarget* help;

    juce::ComboBox waveBox;
    juce::Label waveLabel, status, formantNote, characterNote, phaseNote;
    juce::TextButton aBtn { "A" }, bBtn { "B" }, copyBtn { "Copy A -> B" }, captureBtn { "Capture input" };
    juce::TextButton undoBtn { "Undo" }, redoBtn { "Redo" };
    std::array<std::unique_ptr<ParamKnob>, 4> spectrum;
    std::array<std::unique_ptr<ParamKnob>, 3> formant, character;
    std::unique_ptr<ParamSegmented> phase;
    juce::OwnedArray<widgets_detail::HoverReporter> hovers;   // after the components: destroyed first

    juce::String statusMsg;
    juce::uint32 statusUntil = 0;
    juce::Rectangle<int> tablesRect, spectrumRect, formantRect, characterRect, phaseRect, howRect, hintRect;
};

//==============================================================================
SculptPanel::SculptPanel (ClodScopeProcessor& p, HelpTarget* h)
{
    impl = std::make_unique<Impl> (*this, p, h);
}

SculptPanel::~SculptPanel() = default;

int SculptPanel::idealHeight (int width) const
{
    return impl->layout (width, false);
}

void SculptPanel::paint (juce::Graphics& g)
{
    Theme::drawSection (g, impl->tablesRect, "WAVE TABLES", Accent::sculpt);
    Theme::drawSection (g, impl->spectrumRect, "SPECTRUM", Accent::sculpt);
    Theme::drawSection (g, impl->formantRect, "FORMANT", Accent::sculpt);
    Theme::drawSection (g, impl->characterRect, "CHARACTER", Accent::sculpt);
    Theme::drawSection (g, impl->phaseRect, "PHASE", Accent::sculpt);
    Theme::drawSection (g, impl->howRect, "HOW TO SCULPT", Accent::sculpt);
    impl->hintLayout (impl->hintRect.getWidth()).draw (g, impl->hintRect.toFloat());
}

void SculptPanel::resized()
{
    impl->layout (getWidth(), true);
}
