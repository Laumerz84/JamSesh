#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

// UI themes: whole palettes (Palette:: and Accent:: in UI/Theme.h) and style changes (the material
// everything is made of - UI/Surface.h -, corners, knob style, fonts, backdrop). The choice is global: every instance and the standalone app share it, and
// it is remembered in %APPDATA%\Jam Sesh\Theme.settings (AppPaths.h; another process's change is picked up
// within ~2 s while an editor is open). The env var CLOD_THEME=<index or name> forces a theme without
// saving it (headless snapshots). Owner: themes soldier.
namespace Themes
{
    int count();
    juce::String name (int index);
    juce::String tagline (int index);       // its character in a few words
    int current();                          // the first call loads the saved theme (and applies it)
    void apply (int index);                 // message thread: switches palette and style, saves, tells the listeners

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void themeChanged() = 0;    // message thread
    };
    void addListener (Listener*);
    void removeListener (Listener*);

    // After a change: brings colours that components set on themselves (setColour, accent properties)
    // and label fonts over to the new theme, for the whole tree under root. The editor calls it from
    // themeChanged().
    void refreshTree (juce::Component& root);

    // For previews (ThemePicker): a theme's main colours and its style, without applying it
    struct Preview
    {
        juce::Colour bg, panel, panelEdge, text, dim;
        juce::Colour accents[6];            // input, synth, sculpt, fx, physics, view
    };
    Preview preview (int index);
    const Theme::Style& styleOf (int index);
}
