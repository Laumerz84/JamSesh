#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

// The theme's material (Theme::Style::material): how raised controls (buttons, chain chips, keys),
// sunken fields, section panels, the chain strip's group trays, the main view's frame and the window
// backdrop are drawn. Everything reads Palette:: and Theme::style() at paint time.
// The flat material is the original look: callers keep their own drawing for it (plain()).
// Shadows, glows and lips stay inside the bounds a caller passes: face() is the shape inside them.
namespace Surface
{
    struct State
    {
        bool over = false, down = false, on = false, enabled = true;
    };

    // Sides that join a neighbour (segmented buttons): square corners and no inset on that side
    struct Joins
    {
        bool left = false, right = false;
    };

    bool plain();                                               // flat material: draw the original way

    // Raised controls: buttons, chain chips, keys. on = toggled / selected.
    juce::Rectangle<float> face (juce::Rectangle<float> bounds, Joins = {});
    void raised (juce::Graphics&, juce::Rectangle<float> bounds, float corner, State, juce::Colour accent, Joins = {});
    juce::Colour ink (State, juce::Colour accent);              // a label's colour on a raised control
    juce::Rectangle<float> labelArea (juce::Rectangle<float> bounds, State, Joins = {});
    void label (juce::Graphics&, const juce::String& text, juce::Rectangle<float> area, const juce::Font&,
                State, juce::Colour accent, juce::Justification = juce::Justification::centred);

    // Fields and wells: combo boxes, text fields, check boxes, readouts
    void sunken (juce::Graphics&, juce::Rectangle<float> bounds, float corner, bool hot, juce::Colour accent);

    // Section panels (Theme::drawSection); sectionFace is the panel's shape inside its bounds
    juce::Rectangle<float> sectionFace (juce::Rectangle<int> bounds);
    void section (juce::Graphics&, juce::Rectangle<int> bounds, const juce::String& title, juce::Colour accent);

    // The chain strip's groups (SOURCE, FX RACK, MASTER, DISPLAY): materials with trays put a group's
    // chips on one slab whose label is a tab moulded onto it; the others draw brackets
    bool hasTrays();
    juce::Path trayShape (juce::Rectangle<float> body, juce::Rectangle<float> tab, float radius);
    void tray (juce::Graphics&, const juce::Path& shape, juce::Colour accent);

    void frame (juce::Graphics&, juce::Rectangle<float> view);  // around the main view, drawn outside it
    void backdrop (juce::Graphics&, juce::Rectangle<int> window);

    // A soft shadow outside a shape, as strokes of growing width (cheap in the software renderer);
    // draw the shape afterwards with an opaque fill to cover the inner half
    void softShadow (juce::Graphics&, const juce::Path&, float alpha, float spread, juce::Point<float> offset);
}
