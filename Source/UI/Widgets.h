#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Theme.h"
#include "Surface.h"
#include "Help.h"

// Self-contained parameter controls (label + control + APVTS attachment) that report hover
// to the Inspector. Use these in every control panel instead of raw sliders/combos.

namespace widgets_detail
{
    // Reports hover of a widget or any of its children to a HelpTarget
    class HoverReporter : public juce::MouseListener
    {
    public:
        HoverReporter (juce::Component& owner, const juce::String& helpId, HelpTarget* target)
            : comp (owner), id (helpId), help (target)
        {
            comp.addMouseListener (this, true);
        }
        ~HoverReporter() override { comp.removeMouseListener (this); }

        void mouseEnter (const juce::MouseEvent&) override { if (help != nullptr) help->showHelp (id); }
        void mouseExit (const juce::MouseEvent&) override  { if (help != nullptr) help->clearHelp(); }

    private:
        juce::Component& comp;
        juce::String id;
        HelpTarget* help;
    };
}

// Rotary slider that catches on significant values (the default, and 0) while it is dragged.
// Typed-in values are never snapped.
class SnapSlider : public juce::Slider
{
public:
    std::vector<double> snapPoints;
    double snapWindow = 0.03;   // fraction of the dial's travel on either side of a snap point

    double snapValue (double attempted, DragMode mode) override
    {
        if (mode == notDragging)
            return attempted;
        const double p = valueToProportionOfLength (attempted);
        for (double s : snapPoints)
            if (std::abs (valueToProportionOfLength (s) - p) < snapWindow)
                return s;
        return attempted;
    }
};

class ParamKnob : public juce::Component
{
public:
    ParamKnob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId,
               const juce::String& labelText, HelpTarget* helpTarget = nullptr, bool bipolar = false)
        : id (paramId), hover (*this, paramId, helpTarget)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 16);
        slider.getProperties().set ("bipolar", bipolar);
        label.setText (labelText, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (Theme::font (11.0f, true));
        label.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (slider);
        addAndMakeVisible (label);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, paramId, slider);

        // snap points: the default and zero; double-click returns to the default. The look-and-feel
        // draws a tick at each point (stored as 0..1 positions along the dial).
        if (auto* p = state.getParameter (paramId))
        {
            const auto range = p->getNormalisableRange();
            const double def = range.convertFrom0to1 (p->getDefaultValue());
            slider.snapPoints.push_back (def);
            if (range.start <= 0.0f && range.end >= 0.0f && std::abs (def) > 1.0e-9)
                slider.snapPoints.push_back (0.0);
            slider.setDoubleClickReturnValue (true, def);

            juce::Array<juce::var> marks;
            for (double s : slider.snapPoints)
                marks.add (range.convertTo0to1 ((float) s));
            slider.getProperties().set ("snapMarks", marks);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (14));
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jmin (80, r.getWidth()), 16);
        slider.setBounds (r);
    }

    const juce::String id;
    SnapSlider slider;
    juce::Label label;

private:
    widgets_detail::HoverReporter hover;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

class ParamChoice : public juce::Component
{
public:
    ParamChoice (juce::AudioProcessorValueTreeState& state, const juce::String& paramId,
                 const juce::String& labelText, HelpTarget* helpTarget = nullptr)
        : id (paramId), hover (*this, paramId, helpTarget)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (paramId)))
            box.addItemList (p->choices, 1);
        label.setText (labelText, juce::dontSendNotification);
        label.setFont (Theme::font (11.0f, true));
        label.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (box);
        addAndMakeVisible (label);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, paramId, box);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (16));
        box.setBounds (r.removeFromTop (24));
    }

    const juce::String id;
    juce::ComboBox box;
    juce::Label label;

private:
    widgets_detail::HoverReporter hover;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
};

class ParamToggle : public juce::Component
{
public:
    ParamToggle (juce::AudioProcessorValueTreeState& state, const juce::String& paramId,
                 const juce::String& buttonText, HelpTarget* helpTarget = nullptr)
        : id (paramId), hover (*this, paramId, helpTarget)
    {
        button.setButtonText (buttonText);
        addAndMakeVisible (button);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, paramId, button);
    }

    void resized() override { button.setBounds (getLocalBounds()); }

    const juce::String id;
    juce::ToggleButton button;

private:
    widgets_detail::HoverReporter hover;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
};

// Round power button (on/off) drawn as a power glyph in the module's accent colour. Use it in
// section headers and chain chips for every module or effect that can be switched on and off.
class PowerButton : public juce::Button
{
public:
    explicit PowerButton (juce::Colour accent = Palette::green) : juce::Button ("power"), accentColour (accent)
    {
        setClickingTogglesState (true);
    }

    void paintButton (juce::Graphics& g, bool isOver, bool isDown) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.5f);
        const float d = juce::jmin (r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre (d, d);
        const bool on = getToggleState();
        accentColour = Theme::follow (accentColour);   // a theme switch since construction
        auto ink = on ? accentColour : (isOver ? Palette::text : Palette::dim);

        if (Surface::plain())
        {
            g.setColour (on ? accentColour.withAlpha (isDown ? 0.32f : 0.18f) : Palette::bg.withAlpha (isDown ? 0.6f : 1.0f));
            g.fillEllipse (r);
            g.setColour (ink.withAlpha (on ? 0.9f : 0.7f));
            g.drawEllipse (r.reduced (0.5f), 1.2f);
        }
        else
        {
            // a round key in the material; the glyph lights up in the accent colour when on
            const Surface::State s { isOver, isDown, on, isEnabled() };
            Surface::raised (g, r.expanded (1.0f), d, s, accentColour);
            const auto face = Surface::labelArea (r.expanded (1.0f), s);
            r = face.withSizeKeepingCentre (juce::jmin (face.getWidth(), face.getHeight()), juce::jmin (face.getWidth(), face.getHeight()));
            ink = on ? Surface::ink (s, accentColour) : ink;
        }

        const auto c = r.getCentre();
        const float rad = d * 0.26f;
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, 0.7f, juce::MathConstants<float>::twoPi - 0.7f, true);
        g.setColour (ink);
        g.strokePath (arc, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.drawLine (c.x, c.y - rad * 1.3f, c.x, c.y - rad * 0.2f, 1.6f);
    }

    juce::Colour accentColour;
};

class ParamPowerButton : public juce::Component
{
public:
    ParamPowerButton (juce::AudioProcessorValueTreeState& state, const juce::String& paramId,
                      HelpTarget* helpTarget = nullptr, juce::Colour accent = Palette::green)
        : id (paramId), button (accent), hover (*this, paramId, helpTarget)
    {
        addAndMakeVisible (button);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, paramId, button);
    }

    void resized() override { button.setBounds (getLocalBounds()); }

    const juce::String id;
    PowerButton button;

private:
    widgets_detail::HoverReporter hover;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
};

// A choice parameter shown as a row of segmented buttons (best for 2-5 short options such as
// filter types or modes; use ParamChoice for long lists).
class ParamSegmented : public juce::Component
{
public:
    ParamSegmented (juce::AudioProcessorValueTreeState& state, const juce::String& paramId,
                    HelpTarget* helpTarget = nullptr, juce::Colour accent = Palette::green)
        : id (paramId), hover (*this, paramId, helpTarget)
    {
        auto* p = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (paramId));
        jassert (p != nullptr);   // ParamSegmented needs an AudioParameterChoice
        if (p == nullptr)
            return;

        for (int i = 0; i < p->choices.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (p->choices[i]));
            b->setClickingTogglesState (false);
            b->setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.35f));
            b->setConnectedEdges ((i > 0 ? (int) juce::Button::ConnectedOnLeft : 0)
                                | (i < p->choices.size() - 1 ? (int) juce::Button::ConnectedOnRight : 0));
            b->onClick = [this, i] { if (attachment != nullptr) attachment->setValueAsCompleteGesture ((float) i); };
            addAndMakeVisible (b);
        }
        attachment = std::make_unique<juce::ParameterAttachment> (*p, [this] (float v) { select ((int) std::round (v)); });
        attachment->sendInitialUpdate();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int n = buttons.size();
        for (int i = 0; i < n; ++i)
            buttons[i]->setBounds (i < n - 1 ? r.removeFromLeft (r.getWidth() / (n - i)) : r);
    }

    const juce::String id;
    juce::OwnedArray<juce::TextButton> buttons;

private:
    void select (int index)
    {
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == index, juce::dontSendNotification);
    }

    widgets_detail::HoverReporter hover;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

// Base class for every tab's control panel. The editor places panels in a scrolling
// viewport, so a panel only needs to say how tall it wants to be for a given width.
class ControlPanel : public juce::Component
{
public:
    virtual int idealHeight (int width) const = 0;
    virtual juce::String title() const = 0;      // tab name, e.g. "Orbital"
};
