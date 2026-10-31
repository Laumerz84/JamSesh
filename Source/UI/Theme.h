#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Shared colours and fonts. Every panel and view uses these so the plugin reads as one design.
// They are variables: Themes::apply (UI/Themes.h) reassigns them, so read them at paint time and
// never copy them into members (use Theme::setAccent / accentOf, or a const reference).
// The values below are theme 0 ("Midnight").
namespace Palette
{
    inline juce::Colour bg        { 0xff0b0f14 };
    inline juce::Colour panel     { 0xff121922 };
    inline juce::Colour panelEdge { 0xff1f2a36 };
    inline juce::Colour grid      { 0xff1a2430 };
    inline juce::Colour text      { 0xffd6e2ee };
    inline juce::Colour dim       { 0xff7c8b9a };
    inline juce::Colour green     { 0xff36e0a0 };
    inline juce::Colour blue      { 0xff4aa8ff };
    inline juce::Colour amber     { 0xffffb347 };
    inline juce::Colour pink      { 0xffff5c8a };
    inline juce::Colour violet    { 0xffa98bff };
    inline juce::Colour viewBg    { 0xff070a0e };   // background of scope / 3D / graphic areas
    inline juce::Colour raised    { 0xff161f2a };   // popup menus, tooltips, hovered fields
    inline juce::Colour bright    { 0xffffffff };   // strongest text and highlights (white on dark themes)
    // background + text for views that build their picture with additive light (clouds, glows):
    // equal to viewBg / text in dark themes, a dark "screen" in light themes
    inline juce::Colour screen     { 0xff070a0e };
    inline juce::Colour screenText { 0xffd6e2ee };

    // details used by the look-and-feel (derived from the colours above by Themes::apply)
    inline juce::Colour hover     { 0xff17212c };   // hovered chip / item
    inline juce::Colour field     { 0xff111821 };   // hovered combo box
    inline juce::Colour tooltip   { 0xff18212c };
    inline juce::Colour knobTop   { 0xff1e2935 };   // knob body gradient, top
    inline juce::Colour knobHot   { 0xff243142 };   // ... while hovered
    inline juce::Colour knobRing  { 0xff2a3644 };
}

// One accent colour per part of the signal chain, so a module's panel, chain chip, power button
// and section titles all share a colour and the user always knows which module they are in.
// input / synth / sculpt / fx / physics / master always equal green / blue / violet / amber / pink / text.
namespace Accent
{
    inline juce::Colour input   { 0xff36e0a0 };   // input / basic shaping (green)
    inline juce::Colour synth   { 0xff4aa8ff };   // synth layer (blue)
    inline juce::Colour sculpt  { 0xffa98bff };   // wave sculptor (violet)
    inline juce::Colour fx      { 0xffffb347 };   // effects rack (amber)
    inline juce::Colour physics { 0xffff5c8a };   // equation, double slit, orbital (pink)
    inline juce::Colour view    { 0xff5ad1e6 };   // display-only views and their panels (cyan)
    inline juce::Colour master  { 0xffd6e2ee };   // output stage (neutral)
    inline juce::Colour jam     { 0xffff7a3d };   // Jam Sesh (hot orange)
}

namespace Theme
{
    // Style changes that come with a theme (read by the look-and-feel and drawSection)
    enum class Knob    { arc, dial, flat, glow, ring, dome };   // ring = segmented LED ring, dome = glazed cap
    enum class Buttons { soft, solid, outline };
    enum class Panels  { flat, gradient, glass, outline };
    enum class Titles  { plain, underline, bar };

    // What a theme is made of: how panels, buttons, chips, fields and the chain strip's groups are
    // drawn (UI/Surface.h). flat is the original look; the others give a theme its character.
    enum class Material
    {
        flat,       // flat fills with thin outlines
        ceramic,    // glazed porcelain / clay: soft raised shapes and shadows, selections pressed in
        glass,      // frosted glass: light bands and bright rims
        neon,       // glowing tubes on black
        terminal,   // a text terminal: square boxes, [ bracket ] buttons, inverse video
        candy,      // glossy plastic with a thick bottom lip
        metal,      // machined plates: bevels, black cut edges, rivets
        chrome,     // 80s chrome: bright sky over a hard horizon
        paper       // cards on a desk with soft shadows
    };
    enum class Backdrop { none, grid, scanlines, dots, sunset, rays, grain };   // texture on the window background

    struct Style
    {
        float radius = 8.0f;          // panels and sections
        float controlRadius = 5.0f;   // buttons, combos, fields, chips
        float border = 1.0f;          // panel outline width (0 = none)
        Knob knob = Knob::arc;
        Buttons buttons = Buttons::soft;
        Panels panels = Panels::flat;
        Titles titles = Titles::plain;
        float glow = 0.18f;           // halo strength around lit arcs and selected items (0 = none)
        const char* fontName = "";    // empty = JUCE's default sans
        float fontScale = 1.0f;
        bool light = false;           // light background: shadows instead of glows
        Material material = Material::flat;
        Backdrop backdrop = Backdrop::none;
    };

    // A font named by a theme (UI/Themes.cpp) on this computer: the themes name Windows fonts, and on macOS
    // each becomes a look-alike that every Mac has. Other names pass unchanged.
    inline const char* systemFontName (const char* name)
    {
       #if JUCE_MAC
        static const char* const macFonts[][2] = {
            { "Consolas",     "Menlo" },
            { "Segoe UI",     "Helvetica Neue" },
            { "Calibri",      "Avenir Next" },
            { "Candara",      "Avenir Next" },
            { "Corbel",       "Avenir Next" },
            { "Bahnschrift",  "Avenir Next Condensed" },
            { "Cambria",      "Palatino" },
        };
        for (auto& f : macFonts)
            if (juce::String (name) == f[0])
                return f[1];
       #endif
        return name;
    }

    inline Style currentStyle;
    inline const Style& style() { return currentStyle; }
    inline int generation = 0;        // counts theme switches (for anything that caches colours)

    // A colour cached before the last theme switch (a member copied from Palette:: / Accent::)
    // brought over to the current theme; other colours pass unchanged (Themes.cpp)
    juce::Colour follow (juce::Colour c);

    inline juce::Font font (float size, bool bold = false)
    {
        const auto& s = style();
        auto opts = juce::FontOptions (size * s.fontScale);
        if (*s.fontName != 0)
            opts = opts.withName (s.fontName);
        return juce::Font (bold ? opts.withStyle ("Bold") : opts);
    }

    // Panel with a section title (in the module's accent colour), as used by every control panel; drawn
    // in the theme's material (UI/Surface.cpp). Sections never fill their whole bounds in every material:
    // a component that draws one must not be opaque.
    void drawSection (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title,
                      juce::Colour accent = Palette::green);

    // Accent inheritance: a panel (or any component) stores its accent as a property and every
    // control inside it is drawn in that colour by the editor's look-and-feel (knob arcs, combo
    // arrows, toggles, segmented buttons, focus outlines). The nearest ancestor with one wins.
    // Themes::refreshTree brings these properties over to a new palette.
    inline void setAccent (juce::Component& c, juce::Colour accent)
    {
        c.getProperties().set ("accent", (juce::int64) accent.getARGB());
    }

    inline juce::Colour accentOf (const juce::Component* c, juce::Colour fallback = Palette::green)
    {
        for (; c != nullptr; c = c->getParentComponent())
            if (auto* v = c->getProperties().getVarPointer ("accent"))
                return juce::Colour ((juce::uint32) (juce::int64) *v);
        return fallback;
    }

    constexpr int knobCellHeight = 88;    // label 14 + rotary + value box 16
    constexpr int choiceHeight   = 44;    // label 16 + combo 24 + gap
    constexpr int sectionTitle   = 26;    // space reserved for the drawSection title
}
