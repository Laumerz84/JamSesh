#include "Themes.h"

// The theme table, switching, follow() for stale colours, the tree refresh and the shared setting.
namespace Themes
{
    namespace
    {
        using Theme::Knob;
        using Theme::Buttons;
        using Theme::Panels;
        using Theme::Titles;
        using Theme::Material;
        using Theme::Backdrop;

        // Colour roles, in this order everywhere (follow() searches them in this order, so a value
        // that two roles share in one theme maps to the earlier role)
        enum Role { rBg, rPanel, rEdge, rGrid, rText, rDim, rGreen, rBlue, rAmber, rPink, rViolet, rViewBg,
                    rCyan, rOrange, rRaised, rBright, numRoles };

        struct Def
        {
            const char* name;
            juce::uint32 c[rRaised];   // rBg .. rOrange; raised and bright are derived
            Theme::Style style;
            const char* tagline;       // its character in a few words (theme picker)
        };

        Theme::Style style (float radius, float controlRadius, float border, Knob k, Buttons b, Panels p, Titles t,
                            float glow, const char* font, float fontScale = 1.0f, bool light = false,
                            Material m = Material::flat, Backdrop backdrop = Backdrop::none)
        {
            Theme::Style s;
            s.radius = radius;
            s.controlRadius = controlRadius;
            s.border = border;
            s.knob = k;
            s.buttons = b;
            s.panels = p;
            s.titles = t;
            s.glow = glow;
            s.fontName = Theme::systemFontName (font);   // macOS: a Mac look-alike of the Windows font
            s.fontScale = fontScale;
            s.light = light;
            s.material = m;
            s.backdrop = backdrop;
            return s;
        }

        // Every text role (text, dim, accents) is >= 4.5:1 against panel and viewBg (checked numerically)
        const Def& def (int i)
        {
            //                bg          panel       edge        grid        text        dim         green       blue        amber       pink        violet      viewBg      cyan        orange
            static const Def defs[] = {
                { "Midnight",
                  { 0xff0b0f14, 0xff121922, 0xff1f2a36, 0xff1a2430, 0xffd6e2ee, 0xff7c8b9a, 0xff36e0a0, 0xff4aa8ff, 0xffffb347, 0xffff5c8a, 0xffa98bff, 0xff070a0e, 0xff5ad1e6, 0xffff7a3d },
                  style (8.0f, 5.0f, 1.0f, Knob::arc, Buttons::soft, Panels::flat, Titles::plain, 0.18f, ""),
                  "the original: calm and flat" },
                { "Neon Arcade",
                  { 0xff07040e, 0xff110a1d, 0xff3a2266, 0xff1d1233, 0xfff2eaff, 0xffa794cc, 0xff39ff9c, 0xff5c8dff, 0xffffe14d, 0xffff3fb4, 0xffc77dff, 0xff040208, 0xff00f0ff, 0xffff8a2b },
                  style (3.0f, 2.0f, 1.0f, Knob::glow, Buttons::outline, Panels::outline, Titles::underline, 0.35f, "Consolas", 1.05f, false,
                         Material::neon, Backdrop::grid),
                  "glowing tubes on a black grid" },
                { "Synthwave Sunset",
                  { 0xff1a0b2e, 0xff241140, 0xff42216a, 0xff2e1650, 0xffffe9f4, 0xffbc9ad0, 0xff2ef2c4, 0xff52bbff, 0xffffb454, 0xffff5c9f, 0xffbf95ff, 0xff12071f, 0xff6ee9ff, 0xffff8159 },
                  style (10.0f, 6.0f, 1.0f, Knob::arc, Buttons::soft, Panels::gradient, Titles::bar, 0.3f, "Bahnschrift", 1.06f, false,
                         Material::chrome, Backdrop::sunset),
                  "80s chrome under a sunset" },
                { "Studio Light",
                  { 0xffe6eaef, 0xfffbfcfd, 0xffc5ced8, 0xffdbe1e8, 0xff151c25, 0xff535e6a, 0xff07734b, 0xff1d5fbf, 0xff9a5200, 0xffb8174f, 0xff6237c4, 0xfff4f6f8, 0xff006b7c, 0xffb3460f },
                  style (6.0f, 4.0f, 1.0f, Knob::flat, Buttons::solid, Panels::flat, Titles::plain, 0.0f, "Segoe UI", 1.15f, true,
                         Material::paper),
                  "clean paper cards" },
                { "Forest",
                  { 0xff0c1310, 0xff142019, 0xff26392c, 0xff1b2a20, 0xffe4efdc, 0xff94a893, 0xff8fd16a, 0xff62b8ad, 0xffe2b45c, 0xffe8836a, 0xffbca8dd, 0xff070c09, 0xffa3dbe3, 0xffeb8a42 },
                  style (12.0f, 8.0f, 1.0f, Knob::dial, Buttons::soft, Panels::gradient, Titles::plain, 0.1f, "Segoe UI", 1.15f, false,
                         Material::paper, Backdrop::grain),
                  "soft cards on mossy ground" },
                { "Deep Ocean",
                  { 0xff021a22, 0xff06262f, 0xff0f4552, 0xff0a3340, 0xffd4fbff, 0xff80b6c0, 0xff2ff5c8, 0xff3fbaff, 0xffffd27a, 0xffff7aa8, 0xffa3b8ff, 0xff01121a, 0xff5cf2ff, 0xffff9a6a },
                  style (14.0f, 8.0f, 1.0f, Knob::arc, Buttons::soft, Panels::glass, Titles::underline, 0.26f, "", 1.0f, false,
                         Material::glass, Backdrop::rays),
                  "frosted glass, light from above" },
                { "Blood Moon",
                  { 0xff0e0606, 0xff1a0c0c, 0xff3e1c1c, 0xff281313, 0xfff5e0dc, 0xffb08a86, 0xffe3c477, 0xffe08aa6, 0xffff9248, 0xffff4a4a, 0xffc995de, 0xff090303, 0xffe8b0a8, 0xffff6a2a },
                  style (2.0f, 2.0f, 1.0f, Knob::dial, Buttons::solid, Panels::flat, Titles::bar, 0.2f, "Georgia", 1.04f, false,
                         Material::metal),
                  "riveted iron plates" },
                { "Phosphor",
                  { 0xff020902, 0xff061206, 0xff17401e, 0xff0b220f, 0xff9dffad, 0xff58b866, 0xff39ff6a, 0xff66ffcc, 0xffffd24a, 0xffff8a66, 0xffc2ff66, 0xff010501, 0xff66eeff, 0xffffa64d },
                  style (0.0f, 0.0f, 1.0f, Knob::ring, Buttons::outline, Panels::outline, Titles::underline, 0.3f, "Consolas", 1.06f, false,
                         Material::terminal, Backdrop::scanlines),
                  "a green text terminal" },
                { "Candy Pop",
                  { 0xff1d1027, 0xff2a1838, 0xff4d2f63, 0xff38214a, 0xfffff0fa, 0xffcfaadb, 0xff6dffb3, 0xff86ccff, 0xffffd166, 0xffff75c9, 0xffcdabff, 0xff150b1d, 0xff7af5ff, 0xffffa375 },
                  style (16.0f, 11.0f, 0.0f, Knob::flat, Buttons::solid, Panels::flat, Titles::plain, 0.0f, "Trebuchet MS", 1.04f, false,
                         Material::candy, Backdrop::dots),
                  "glossy plastic keys" },
                { "Desert Dusk",
                  { 0xff1c1410, 0xff281d17, 0xff4a372b, 0xff33261e, 0xfff5e6d3, 0xffb9a28b, 0xffaecd85, 0xff86b5cc, 0xfff2a65a, 0xffee8064, 0xffcfa6cf, 0xff120d0a, 0xff94d3c3, 0xffff8f45 },
                  style (10.0f, 8.0f, 1.0f, Knob::dial, Buttons::soft, Panels::gradient, Titles::bar, 0.08f, "Cambria", 1.04f, false,
                         Material::ceramic, Backdrop::grain),
                  "moulded terracotta clay" },
                { "Ice",
                  { 0xffdce8f2, 0xfff2f8fc, 0xffb0c6d8, 0xffcddcea, 0xff0e2132, 0xff445a6f, 0xff00705f, 0xff1458b0, 0xff8a5100, 0xffad1f53, 0xff5640b8, 0xffe9f2f9, 0xff006585, 0xffa6420e },
                  style (10.0f, 6.0f, 1.0f, Knob::arc, Buttons::outline, Panels::glass, Titles::underline, 0.0f, "Bahnschrift", 1.08f, true,
                         Material::glass, Backdrop::grain),
                  "frosted glass on snow" },
                { "High Contrast",
                  { 0xff000000, 0xff0a0a0a, 0xff8c8c8c, 0xff333333, 0xfffafafa, 0xffcccccc, 0xff00ff7f, 0xff4db8ff, 0xffffd000, 0xffff5c93, 0xffc9a3ff, 0xff020202, 0xff00e5ff, 0xffff8c1a },
                  style (0.0f, 0.0f, 2.0f, Knob::flat, Buttons::outline, Panels::flat, Titles::underline, 0.0f, "", 1.1f),
                  "bold lines, easy to read" },
                { "Ceramic",
                  { 0xffdcd4c8, 0xfff6f2ec, 0xffd6cdc1, 0xffe3dcd1, 0xff2a2521, 0xff675d53, 0xff2a6c54, 0xff2552a3, 0xff8a4f0e, 0xffa63558, 0xff6646ad, 0xffede7de, 0xff1e6977, 0xff9a4414 },
                  style (14.0f, 11.0f, 0.0f, Knob::dome, Buttons::soft, Panels::flat, Titles::plain, 0.0f, "Calibri", 1.12f, true,
                         Material::ceramic),
                  "glazed porcelain, smooth and soft" },
            };
            return defs[juce::jlimit (0, (int) std::size (defs) - 1, i)];
        }

        constexpr int numThemes = 13;

        // Every role of a theme, the derived ones included
        struct Colours
        {
            juce::Colour c[numRoles];
            juce::Colour hover, field, tooltip, knobTop, knobHot, knobRing;
        };

        Colours coloursOf (int index)
        {
            const auto& d = def (index);
            Colours k;
            for (int r = 0; r < rRaised; ++r)
                k.c[r] = juce::Colour (d.c[r]);
            const auto bg = k.c[rBg], panel = k.c[rPanel], edge = k.c[rEdge];
            if (index == 0)
            {
                // today's look, exactly
                k.c[rRaised] = juce::Colour (0xff161f2a);
                k.c[rBright] = juce::Colour (0xffffffff);
                k.hover = juce::Colour (0xff17212c);
                k.field = juce::Colour (0xff111821);
                k.tooltip = juce::Colour (0xff18212c);
                k.knobTop = juce::Colour (0xff1e2935);
                k.knobHot = juce::Colour (0xff243142);
                k.knobRing = juce::Colour (0xff2a3644);
                return k;
            }
            const bool light = d.style.light;
            k.c[rRaised] = light ? panel : panel.interpolatedWith (edge, 0.4f);
            k.c[rBright] = light ? juce::Colour (0xff05080c) : juce::Colour (0xffffffff);
            k.hover   = panel.interpolatedWith (edge, light ? 0.3f : 0.45f);
            k.field   = light ? panel : bg.interpolatedWith (panel, 0.8f);
            k.tooltip = light ? panel : panel.interpolatedWith (edge, 0.5f);
            k.knobTop = light ? panel : panel.interpolatedWith (edge, 0.9f);
            k.knobHot = light ? bg.interpolatedWith (edge, 0.3f) : edge.brighter (0.15f);
            k.knobRing = light ? edge : edge.brighter (0.25f);
            return k;
        }

        juce::Colour* liveRole (int r)
        {
            static juce::Colour* const roles[numRoles] = {
                &Palette::bg, &Palette::panel, &Palette::panelEdge, &Palette::grid, &Palette::text, &Palette::dim,
                &Palette::green, &Palette::blue, &Palette::amber, &Palette::pink, &Palette::violet, &Palette::viewBg,
                &Accent::view, &Accent::jam, &Palette::raised, &Palette::bright
            };
            return roles[r];
        }

        //----------------------------------------------------------------------
        struct State
        {
            int current = 0;
            int previous = 0;
            juce::uint32 seen = 1;       // themes applied in this process (their colours may be cached somewhere)
            bool loaded = false;
            bool forced = false;         // CLOD_THEME: no saving, no watching
            juce::Time fileTime;
        };

        State& state()
        {
            static State s;
            return s;
        }

        juce::ListenerList<Listener>& listeners()
        {
            static juce::ListenerList<Listener> l;
            return l;
        }

        juce::File settingsFile()
        {
            return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("Clod Scope").getChildFile ("Theme.settings");
        }

        int indexOf (const juce::String& s)
        {
            for (int i = 0; i < numThemes; ++i)
                if (s.equalsIgnoreCase (def (i).name))
                    return i;
            if (s.containsOnly ("0123456789") && s.isNotEmpty())
                return juce::jlimit (0, numThemes - 1, s.getIntValue());
            return -1;
        }

        // The settings file holds one line, "theme=<name>"
        int readSaved()
        {
            const auto f = settingsFile();
            state().fileTime = f.getLastModificationTime();
            for (auto& line : juce::StringArray::fromLines (f.loadFileAsString()))
                if (line.trim().startsWithIgnoreCase ("theme="))
                    return indexOf (line.fromFirstOccurrenceOf ("=", false, false).trim());
            return -1;
        }

        void save (int index)
        {
            auto f = settingsFile();
            f.getParentDirectory().createDirectory();
            f.replaceWithText ("theme=" + juce::String (def (index).name) + "\n");
            state().fileTime = f.getLastModificationTime();
        }

        // Sets Palette::, Accent:: and the style; no listeners, no saving
        void set (int index)
        {
            auto& st = state();
            const auto k = coloursOf (index);
            for (int r = 0; r < numRoles; ++r)
                *liveRole (r) = k.c[r];
            Palette::hover = k.hover;
            Palette::field = k.field;
            Palette::tooltip = k.tooltip;
            Palette::knobTop = k.knobTop;
            Palette::knobHot = k.knobHot;
            Palette::knobRing = k.knobRing;
            const bool light = def (index).style.light;
            Palette::screen = light ? juce::Colour (0xff141a22) : Palette::viewBg;
            Palette::screenText = light ? juce::Colour (0xffe2e9f0) : Palette::text;
            Accent::input = Palette::green;
            Accent::synth = Palette::blue;
            Accent::sculpt = Palette::violet;
            Accent::fx = Palette::amber;
            Accent::physics = Palette::pink;
            Accent::master = Palette::text;
            Theme::currentStyle = def (index).style;
            ++Theme::generation;
            st.previous = st.current;
            st.current = index;
            st.seen |= (1u << index);
        }

        void load()
        {
            auto& st = state();
            if (st.loaded)
                return;
            st.loaded = true;
            const auto forced = juce::SystemStats::getEnvironmentVariable ("CLOD_THEME", {});
            int index = -1;
            if (forced.isNotEmpty())
            {
                st.forced = true;
                index = indexOf (forced);
            }
            else
            {
                index = readSaved();
            }
            if (index > 0)
            {
                set (index);
                st.previous = index;
                st.seen = 1u << index;   // nothing was drawn in theme 0 yet
            }
        }

        // Another process (the standalone app, another DAW) changed the setting
        struct Watcher : public juce::Timer
        {
            void timerCallback() override
            {
                auto& st = state();
                if (st.forced || settingsFile().getLastModificationTime() == st.fileTime)
                    return;
                const int i = readSaved();
                if (i >= 0 && i != st.current)
                {
                    set (i);
                    listeners().call ([] (Listener& l) { l.themeChanged(); });
                }
            }
        };

        std::unique_ptr<Watcher>& watcher()
        {
            static std::unique_ptr<Watcher> w;
            return w;
        }

        // The role of a colour in one theme (alpha ignored), or -1
        int roleIn (const Colours& k, juce::uint32 rgb)
        {
            for (int r = 0; r < numRoles; ++r)
                if ((k.c[r].getARGB() & 0xffffffu) == rgb)
                    return r;
            return -1;
        }

        Colours& cachedColours (int index)
        {
            static Colours all[numThemes];
            static bool built = false;
            if (! built)
            {
                for (int i = 0; i < numThemes; ++i)
                    all[i] = coloursOf (i);
                built = true;
            }
            return all[index];
        }

        juce::Colour followColour (juce::Colour c)
        {
            auto& st = state();
            const juce::uint32 rgb = c.getARGB() & 0xffffffu;
            if (st.seen == (1u << st.current) || roleIn (cachedColours (st.current), rgb) >= 0)
                return c;
            int r = (st.seen >> st.previous) & 1u ? roleIn (cachedColours (st.previous), rgb) : -1;
            for (int i = 0; r < 0 && i < numThemes; ++i)
                if (i != st.current && ((st.seen >> i) & 1u) != 0)
                    r = roleIn (cachedColours (i), rgb);
            return r < 0 ? c : liveRole (r)->withAlpha (c.getAlpha());
        }

        juce::String fontFamily (const Theme::Style& s)
        {
            return *s.fontName != 0 ? juce::String (s.fontName) : juce::Font::getDefaultSansSerifFontName();
        }

        void refreshComponent (juce::Component& c, const juce::String& oldFamily, float oldScale)
        {
            auto& props = c.getProperties();
            for (int i = 0; i < props.size(); ++i)
            {
                const auto key = props.getName (i).toString();
                if (key == "accent" || key.startsWith ("jcclr_"))
                {
                    // setColour stores an int (key "jcclr_<hex id>"), Theme::setAccent an int64
                    auto& v = *props.getVarPointerAt (i);
                    const auto old = juce::Colour ((juce::uint32) (juce::int64) v);
                    const auto now = followColour (old);
                    if (now != old)
                    {
                        if (key == "accent")
                            v = (juce::int64) now.getARGB();
                        else
                            c.setColour (key.substring (6).getHexValue32(), now);   // colourChanged() for the component
                    }
                }
            }

            const auto& s = Theme::style();
            if (auto* label = dynamic_cast<juce::Label*> (&c))
            {
                auto f = label->getFont();
                if (f.getTypefaceName() == oldFamily || (oldFamily == juce::Font::getDefaultSansSerifFontName()
                                                         && f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName()))
                {
                    f.setTypefaceName (fontFamily (s));
                    f.setHeight (f.getHeight() / oldScale * s.fontScale);
                    label->setFont (f);
                }
            }

            for (auto* child : c.getChildren())
                refreshComponent (*child, oldFamily, oldScale);
        }
    }

    //==========================================================================
    int count() { return numThemes; }
    juce::String name (int index) { return def (index).name; }
    juce::String tagline (int index) { return def (index).tagline; }

    int current()
    {
        load();
        return state().current;
    }

    void apply (int index)
    {
        load();
        index = juce::jlimit (0, numThemes - 1, index);
        auto& st = state();
        if (index == st.current)
            return;
        set (index);
        if (! st.forced)
            save (index);
        listeners().call ([] (Listener& l) { l.themeChanged(); });
    }

    void addListener (Listener* l)
    {
        load();
        listeners().add (l);
        if (watcher() == nullptr && ! state().forced)
        {
            watcher() = std::make_unique<Watcher>();
            watcher()->startTimer (2000);
        }
    }

    void removeListener (Listener* l)
    {
        listeners().remove (l);
        if (listeners().isEmpty())
            watcher().reset();
    }

    void refreshTree (juce::Component& root)
    {
        const auto& old = def (state().previous).style;
        refreshComponent (root, fontFamily (old), old.fontScale);
    }

    Preview preview (int index)
    {
        const auto k = coloursOf (juce::jlimit (0, numThemes - 1, index));
        return { k.c[rBg], k.c[rPanel], k.c[rEdge], k.c[rText], k.c[rDim],
                 { k.c[rGreen], k.c[rBlue], k.c[rViolet], k.c[rAmber], k.c[rPink], k.c[rCyan] } };
    }

    const Theme::Style& styleOf (int index) { return def (index).style; }
}

juce::Colour Theme::follow (juce::Colour c)
{
    return Themes::followColour (c);
}
