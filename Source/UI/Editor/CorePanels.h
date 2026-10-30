#pragma once

#include "../Widgets.h"
#include "../../PluginProcessor.h"

// A ControlPanel made of titled sections. Each section lays out a row of toggles (optional,
// above or below), a grid of knobs and rows of choice boxes, and adapts its column count to the
// available width. idealHeight() runs the same layout without applying it.
class SectionedPanel : public ControlPanel
{
public:
    int idealHeight (int width) const override { return layout (width, false); }

    void paint (juce::Graphics& g) override
    {
        for (auto& s : drawn)
            Theme::drawSection (g, s.first, s.second, accent);
    }

    void resized() override { layout (getWidth(), true); }

protected:
    // Section titles and (through the look-and-feel) every control inside use this colour
    void setPanelAccent (juce::Colour c)
    {
        accent = c;
        Theme::setAccent (*this, c);
    }

    juce::Colour accent = Palette::green;

    struct Section
    {
        juce::String title;
        std::vector<juce::Component*> knobs, choices, toggles;
        bool togglesFirst = false;
    };

    // Returns the index of the new section; fill it via sections[i] right away
    Section& addSection (const juce::String& title)
    {
        sections.push_back ({ title, {}, {}, {}, false });
        return sections.back();
    }

    template <typename T, typename... Args>
    T* make (Args&&... args)
    {
        auto c = std::make_unique<T> (std::forward<Args> (args)...);
        auto* raw = c.get();
        addAndMakeVisible (*raw);
        owned.push_back (std::move (c));
        return raw;
    }

    std::vector<Section> sections;

private:
    int layout (int width, bool apply) const
    {
        if (apply)
            drawn.clear();
        int y = 0;
        for (auto& s : sections)
        {
            const int top = y;
            auto inner = juce::Rectangle<int> (12, y + 8, juce::jmax (100, width - 24), 0);
            int cy = inner.getY() + 22;

            auto placeToggles = [&]
            {
                if (s.toggles.empty())
                    return;
                int x = inner.getX();
                for (auto* t : s.toggles)
                {
                    const int w = dynamic_cast<juce::TextButton*> (t) != nullptr ? 90 : 128;
                    if (x + w > inner.getRight() && x > inner.getX())
                    {
                        x = inner.getX();
                        cy += 28;
                    }
                    if (apply)
                        t->setBounds (x, cy, w, 24);
                    x += w + 6;
                }
                cy += 30;
            };

            if (s.togglesFirst)
                placeToggles();

            if (! s.knobs.empty())
            {
                const int cols = juce::jlimit (3, 6, inner.getWidth() / 84);
                const int cellW = inner.getWidth() / cols;
                for (size_t i = 0; i < s.knobs.size(); ++i)
                {
                    const int col = (int) (i % (size_t) cols), row = (int) (i / (size_t) cols);
                    if (apply)
                        s.knobs[i]->setBounds (inner.getX() + col * cellW + 2, cy + row * Theme::knobCellHeight + 2,
                                               cellW - 4, Theme::knobCellHeight - 4);
                }
                cy += (int) ((s.knobs.size() + (size_t) cols - 1) / (size_t) cols) * Theme::knobCellHeight + 4;
            }

            if (! s.choices.empty())
            {
                const int perRow = inner.getWidth() >= 380 && s.choices.size() >= 3 ? 3 : 2;
                const int cw = (inner.getWidth() - 8 * (perRow - 1)) / perRow;
                for (size_t i = 0; i < s.choices.size(); ++i)
                {
                    const int col = (int) (i % (size_t) perRow), row = (int) (i / (size_t) perRow);
                    if (apply)
                        s.choices[i]->setBounds (inner.getX() + col * (cw + 8), cy + row * Theme::choiceHeight, cw, 40);
                }
                cy += (int) ((s.choices.size() + (size_t) perRow - 1) / (size_t) perRow) * Theme::choiceHeight;
            }

            if (! s.togglesFirst)
                placeToggles();

            y = cy + 8;
            if (apply)
                drawn.push_back ({ juce::Rectangle<int> (0, top, width, y - top), s.title });
            y += 10;
        }
        return y;
    }

    std::vector<std::unique_ptr<juce::Component>> owned;
    mutable std::vector<std::pair<juce::Rectangle<int>, juce::String>> drawn;
};

//==============================================================================
class SoundPanel : public SectionedPanel
{
public:
    SoundPanel (juce::AudioProcessorValueTreeState& state, HelpTarget* help) : apvts (state)
    {
        setPanelAccent (Accent::input);
        auto knob = [&] (const char* id, const char* name, bool bipolar = false) { return make<ParamKnob> (state, id, name, help, bipolar); };

        auto& in = addSection ("INPUT");
        in.knobs = { knob ("inGain", "IN GAIN", true), knob ("gate", "NOISE GATE"), knob ("dc", "DC OFFSET", true) };
        in.toggles = { make<ParamToggle> (state, "invert", "Phase invert", help) };

        auto& sh = addSection ("SHAPE");
        sh.knobs = { knob ("drive", "DRIVE"), knob ("bits", "BIT DEPTH"), knob ("downsample", "DOWNSAMPLE") };
        sh.choices = { make<ParamChoice> (state, "shape", "SHAPE", help) };

        auto& tone = addSection ("TONE");
        tone.knobs = { knob ("hpf", "HIGH-PASS"), knob ("lpf", "LOW-PASS") };

        auto* reset = make<juce::TextButton> ("Reset input");
        reset->setTooltip ("Puts every input-stage control (gain, gate, shape, filters) back to its neutral default");
        reset->onClick = [this] { resetProcessing(); };
        tone.toggles = { reset };
    }

    juce::String title() const override { return "Input"; }

private:
    void resetProcessing()
    {
        for (auto* id : { "inGain", "gate", "dc", "invert", "drive", "shape", "bits", "downsample", "hpf", "lpf" })
            if (auto* p = apvts.getParameter (id))
                p->setValueNotifyingHost (p->getDefaultValue());
    }

    juce::AudioProcessorValueTreeState& apvts;
};

//==============================================================================
class ScopePanel : public SectionedPanel
{
public:
    ScopePanel (juce::AudioProcessorValueTreeState& state, HelpTarget* help)
    {
        setPanelAccent (Accent::view);
        auto knob = [&] (const char* id, const char* name, bool bipolar = false) { return make<ParamKnob> (state, id, name, help, bipolar); };

        auto& view = addSection ("2D SCOPE DISPLAY");
        view.knobs = { knob ("time", "TIME"), knob ("zoom", "ZOOM"), knob ("persist", "PERSIST") };
        view.choices = { make<ParamChoice> (state, "view", "CHANNELS", help), make<ParamChoice> (state, "source", "SHOW", help) };
        view.toggles = { make<ParamToggle> (state, "freeze", "Freeze", help), make<ParamToggle> (state, "spectrum", "Spectrum", help) };

        auto& trig = addSection ("TRIGGER");
        trig.knobs = { knob ("trigLevel", "TRIG LEVEL", true) };
        trig.choices = { make<ParamChoice> (state, "trigMode", "TRIGGER", help) };
    }

    juce::String title() const override { return "Scope"; }
};

//==============================================================================
class SlitPanel : public SectionedPanel
{
public:
    SlitPanel (juce::AudioProcessorValueTreeState& state, HelpTarget* help) : apvts (state)
    {
        setPanelAccent (Accent::physics);
        auto knob = [&] (const char* id, const char* name) { return make<ParamKnob> (state, id, name, help); };

        auto& geo = addSection ("DOUBLE SLIT - GEOMETRY");
        geo.togglesFirst = true;
        geo.toggles = { make<ParamToggle> (state, "dsOn", "Enable", help) };
        multiple = knob ("dsMultiple", "MULTIPLE");
        separation = knob ("dsSeparation", "SEPARATION");
        geo.knobs = { multiple, separation, knob ("dsWidth", "SLIT WIDTH"), knob ("dsFringes", "FRINGES"), knob ("dsDistance", "DISTANCE") };
        geo.choices = { make<ParamChoice> (state, "dsSizing", "SLIT SIZING", help) };

        auto& ear = addSection ("DOUBLE SLIT - EAR & MIX");
        ear.knobs = { knob ("dsMix", "SLIT MIX") };
        ear.choices = { make<ParamChoice> (state, "dsEar", "EAR MODEL", help) };

        for (auto& s : sections)
        {
            for (auto* c : s.knobs)   dependents.push_back (c);
            for (auto* c : s.choices) dependents.push_back (c);
        }
        updateEnablement();
    }

    juce::String title() const override { return "Double Slit"; }

    // Greys out everything while the slit is off, and the knob the current sizing mode ignores
    void updateEnablement()
    {
        const bool on = apvts.getRawParameterValue ("dsOn")->load() > 0.5f;
        const bool fixed = (int) apvts.getRawParameterValue ("dsSizing")->load() == DoubleSlitEngine::fixedMetres;
        for (auto* c : dependents)
            c->setEnabled (on);
        multiple->setEnabled (on && ! fixed);
        separation->setEnabled (on && fixed);
    }

private:
    juce::AudioProcessorValueTreeState& apvts;
    ParamKnob* multiple = nullptr;
    ParamKnob* separation = nullptr;
    std::vector<juce::Component*> dependents;
};
