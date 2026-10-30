#include "Surface.h"

// Drawing in the theme's material. Gradients cost per pixel in the software renderer, so they stay on
// small shapes or narrow bands; shadows and rims are strokes along an outline.
using namespace juce;
using Theme::Material;

namespace
{
    Material material() { return Theme::style().material; }
    bool light()        { return Theme::style().light; }

    Path roundedPath (Rectangle<float> r, float corner, Surface::Joins j)
    {
        corner = jmax (0.0f, jmin (corner, jmin (r.getWidth(), r.getHeight()) * 0.5f));
        Path p;
        p.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), corner, corner,
                               ! j.left, ! j.right, ! j.left, ! j.right);
        return p;
    }

    // A line along the inside of a shape's outline, moved by offset: a rim under the top edge (offset
    // down) or a shade above the bottom edge (offset up). The rest of the moved outline is clipped away.
    void innerLine (Graphics& g, const Path& shape, Colour c, float width, Point<float> offset)
    {
        Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (shape);
        g.setColour (c);
        g.strokePath (shape, PathStrokeType (width), AffineTransform::translation (offset));
    }

    // Shade falling from the top edge into a sunken shape
    void topShade (Graphics& g, const Path& shape, Rectangle<float> r, float alpha, float depth = 5.0f)
    {
        Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (shape);
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (alpha), 0.0f, r.getY(),
                                           Colours::black.withAlpha (0.0f), 0.0f, r.getY() + depth, false));
        g.fillRect (r.withHeight (depth));
    }

    void verticalFill (Graphics& g, const Path& p, Rectangle<float> r, Colour top, Colour bottom)
    {
        g.setGradientFill (ColourGradient (top, 0.0f, r.getY(), bottom, 0.0f, r.getBottom(), false));
        g.fillPath (p);
    }

    // Light band over the top of a shape that fades out (glass, chrome sky, plate sheen)
    void band (Graphics& g, const Path& shape, Rectangle<float> r, Colour c, float height)
    {
        Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (shape);
        const float h = jmin (r.getHeight(), height);
        g.setGradientFill (ColourGradient (c, 0.0f, r.getY(), c.withAlpha (0.0f), 0.0f, r.getY() + h, false));
        g.fillRect (r.withHeight (h));
    }

    // Glazed ceramic: lit from above, a bright rim under the top edge; pressed = shaded from the top
    void glaze (Graphics& g, const Path& p, Rectangle<float> r, bool pressed, float fade)
    {
        using namespace Palette;
        const bool lt = light();
        const auto top = pressed ? panel.darker (lt ? 0.07f : 0.32f)
                                 : (lt ? panel.interpolatedWith (Colours::white, 0.6f) : panel.brighter (0.10f));
        const auto bottom = pressed ? panel.darker (lt ? 0.03f : 0.2f) : panel.darker (lt ? 0.025f : 0.06f);
        verticalFill (g, p, r, top.withMultipliedAlpha (fade), bottom.withMultipliedAlpha (fade));
        if (pressed)
            topShade (g, p, r, lt ? 0.13f : 0.45f);
        else
            innerLine (g, p, Colours::white.withAlpha (lt ? 0.95f : 0.12f), 1.2f, { 0.0f, 1.0f });
    }

    void rivet (Graphics& g, Point<float> c)
    {
        const auto r = Rectangle<float> (5.0f, 5.0f).withCentre (c);
        g.setColour (Colours::black.withAlpha (0.6f));
        g.fillEllipse (r.translated (0.0f, 0.8f));
        g.setColour (Palette::panelEdge.brighter (0.35f));
        g.fillEllipse (r);
        g.setColour (Colours::black.withAlpha (0.55f));
        g.drawLine (c.x - 1.6f, c.y + 1.6f, c.x + 1.6f, c.y - 1.6f, 0.9f);
    }

    // Fine speckle, tiled over the background (software image: never a GPU image, UI/StreamSafe.h)
    const Image& grainTile()
    {
        static Image tile;
        static int generation = -1;
        if (generation != Theme::generation)
        {
            generation = Theme::generation;
            tile = Image (Image::ARGB, 96, 96, true, SoftwareImageType());
            Random rng (1234);
            const auto c = light() ? Colours::black : Colours::white;
            for (int y = 0; y < tile.getHeight(); ++y)
                for (int x = 0; x < tile.getWidth(); ++x)
                    if (rng.nextFloat() < 0.4f)
                        tile.setPixelAt (x, y, c.withAlpha (rng.nextFloat() * (light() ? 0.045f : 0.055f)));
        }
        return tile;
    }

    // Polka dots in staggered rows, as a tile
    const Image& dotsTile()
    {
        static Image tile;
        static int generation = -1;
        if (generation != Theme::generation)
        {
            generation = Theme::generation;
            tile = Image (Image::ARGB, 22, 44, true, SoftwareImageType());
            Graphics t (tile);
            t.setColour (Palette::text.withAlpha (0.07f));
            t.fillEllipse (9.5f, 9.5f, 3.0f, 3.0f);
            t.fillEllipse (-1.5f, 31.5f, 3.0f, 3.0f);   // the staggered row, split across the tile's edge
            t.fillEllipse (20.5f, 31.5f, 3.0f, 3.0f);
        }
        return tile;
    }

    // A segmented switch in the soft materials: the row is one sunken track, the chosen option a raised
    // pill inside it
    void segment (Graphics& g, Rectangle<float> r, const Path& track, float corner, Surface::State s,
                  Colour accent, Surface::Joins j, float fade)
    {
        using namespace Palette;
        const bool lt = light();
        const auto m = material();
        if (m == Material::ceramic)
            g.setColour (panel.darker (lt ? 0.09f : 0.3f));
        else if (m == Material::paper)
            g.setColour (lt ? bg.interpolatedWith (panel, 0.3f) : bg.interpolatedWith (panel, 0.35f));
        else
            g.setColour (lt ? bg.interpolatedWith (panel, 0.4f) : bg.interpolatedWith (panel, 0.5f));
        g.fillPath (track);
        topShade (g, track, r, m == Material::glass ? (lt ? 0.05f : 0.25f) : (lt ? 0.16f : 0.45f), 5.0f);
        if (m == Material::ceramic)
        {
            // pressed into the glaze: a light rim along the bottom edge, a faint cut all round
            innerLine (g, track, Colours::white.withAlpha (lt ? 0.9f : 0.06f), 1.0f, { 0.0f, -1.0f });
            g.setColour (Colours::black.withAlpha (lt ? 0.07f : 0.3f));
            g.strokePath (track, PathStrokeType (1.0f));
        }
        else
        {
            g.setColour (lt ? panelEdge : Colours::white.withAlpha (0.1f));
            g.strokePath (track, PathStrokeType (1.0f));
        }
        if (j.left && ! s.on)
        {
            g.setColour (panelEdge.withAlpha (lt ? 0.9f : 0.5f));
            g.fillRect (r.getX(), r.getY() + 6.0f, 1.0f, r.getHeight() - 12.0f);
        }
        if (! s.on && ! s.over)
            return;

        const auto pr = r.reduced (2.5f, 2.5f);
        const auto pill = roundedPath (pr, jmax (2.0f, corner - 2.5f), {});
        if (! s.on)
        {
            g.setColour (Colours::white.withAlpha (lt ? 0.35f : 0.05f));
            g.fillPath (pill);
            return;
        }
        if (m == Material::ceramic)
        {
            Surface::softShadow (g, pill, lt ? 0.2f : 0.5f, 1.5f, { 0.0f, 1.0f });
            glaze (g, pill, pr, s.down, fade);
            g.setColour (accent.withAlpha (lt ? 0.07f : 0.12f));
            g.fillPath (pill);
        }
        else if (m == Material::paper)
        {
            Surface::softShadow (g, pill, lt ? 0.14f : 0.45f, 1.5f, { 0.0f, 0.8f });
            const bool solid = Theme::style().buttons == Theme::Buttons::solid;
            g.setColour ((solid ? accent : lt ? panel : panel.interpolatedWith (accent, 0.16f).brighter (0.1f)).withMultipliedAlpha (fade));
            g.fillPath (pill);
        }
        else
        {
            g.setColour (panel.interpolatedWith (accent, lt ? 0.2f : 0.3f).withMultipliedAlpha (fade));
            g.fillPath (pill);
            band (g, pill, pr, Colours::white.withAlpha (lt ? 0.5f : 0.15f), pr.getHeight() * 0.55f);
            g.setColour (accent.withAlpha (0.8f));
            g.strokePath (pill, PathStrokeType (1.0f));
        }
    }
}

//==============================================================================
bool Surface::plain() { return material() == Material::flat; }

void Surface::softShadow (Graphics& g, const Path& p, float alpha, float spread, Point<float> offset)
{
    const int n = jmax (1, roundToInt (spread * 1.5f));
    const auto t = AffineTransform::translation (offset);
    g.setColour (Colours::black.withAlpha (alpha / (float) n));
    for (int i = n; i >= 1; --i)
        g.strokePath (p, PathStrokeType (2.0f * spread * (float) i / (float) n), t);
}

Rectangle<float> Surface::face (Rectangle<float> b, Joins j)
{
    float l = 0.5f, r = 0.5f, t = 0.5f, bottom = 0.5f;
    switch (material())
    {
        case Material::ceramic: l = r = 1.5f; t = 1.0f; bottom = 3.0f; break;   // room for the shadow below
        case Material::paper:   l = r = 1.0f; t = 0.5f; bottom = 2.5f; break;
        case Material::neon:    l = r = t = bottom = 2.0f; break;                 // room for the glow
        default: break;
    }
    if (j.left)  l = 0.0f;
    if (j.right) r = 0.0f;
    return { b.getX() + l, b.getY() + t, jmax (0.0f, b.getWidth() - l - r), jmax (0.0f, b.getHeight() - t - bottom) };
}

void Surface::raised (Graphics& g, Rectangle<float> bounds, float corner, State s, Colour accent, Joins j)
{
    using namespace Palette;
    const bool lt = light();
    const auto r = face (bounds, j);
    if (r.isEmpty())
        return;
    const auto p = roundedPath (r, corner, j);
    const float fade = s.enabled ? 1.0f : 0.5f;
    const bool pressed = s.down || s.on;
    const auto m = material();

    if ((j.left || j.right) && (m == Material::ceramic || m == Material::paper || m == Material::glass))
    {
        segment (g, r, p, corner, s, accent, j, fade);
        return;
    }

    switch (m)
    {
        case Material::flat:
        {
            g.setColour (s.on ? accent.withAlpha (0.25f) : s.down ? panelEdge.brighter (0.25f) : s.over ? panelEdge.brighter (0.05f) : bg);
            g.fillPath (p);
            g.setColour (s.on ? accent.withAlpha (0.9f) : panelEdge.brighter (s.over ? 0.45f : 0.15f));
            g.strokePath (p, PathStrokeType (1.0f));
            break;
        }

        case Material::ceramic:
        {
            if (! pressed)
                softShadow (g, p, lt ? 0.18f : 0.5f, 2.5f, { 0.0f, 1.2f });
            glaze (g, p, r, pressed, fade);
            if (s.over && ! pressed)
            {
                g.setColour (Colours::white.withAlpha (lt ? 0.3f : 0.04f));
                g.fillPath (p);
            }
            if (s.on)
            {
                g.setColour (accent.withAlpha (lt ? 0.1f : 0.16f));
                g.fillPath (p);
            }
            break;
        }

        case Material::paper:
        {
            if (! s.down)
                softShadow (g, p, lt ? 0.12f : 0.4f, 2.0f, { 0.0f, 1.0f });
            const bool solid = Theme::style().buttons == Theme::Buttons::solid;
            g.setColour ((s.on ? (solid ? accent : panel.interpolatedWith (accent, lt ? 0.12f : 0.2f))
                               : s.down ? panelEdge : s.over ? hover : panel).withMultipliedAlpha (fade));
            g.fillPath (p);
            g.setColour (s.on ? accent.withAlpha (0.9f) : panelEdge.withAlpha (lt ? 1.0f : 0.6f));
            g.strokePath (p, PathStrokeType (1.0f));
            break;
        }

        case Material::glass:
        {
            g.setColour ((s.on ? panel.interpolatedWith (accent, lt ? 0.16f : 0.28f)
                               : s.over ? hover : panel.interpolatedWith (panelEdge, 0.3f)).withMultipliedAlpha (fade));
            g.fillPath (p);
            band (g, p, r, Colours::white.withAlpha (lt ? 0.6f : 0.16f), r.getHeight() * 0.55f);
            if (s.down)
            {
                g.setColour (Colours::black.withAlpha (0.12f));
                g.fillPath (p);
            }
            g.setColour (s.on ? accent.withAlpha (0.85f) : (lt ? panelEdge.darker (0.05f) : Colours::white.withAlpha (s.over ? 0.32f : 0.18f)));
            g.strokePath (p, PathStrokeType (1.0f));
            innerLine (g, p, Colours::white.withAlpha (lt ? 0.9f : 0.3f), 1.0f, { 0.0f, 1.0f });
            break;
        }

        case Material::neon:
        {
            const auto c = s.on ? accent : (s.over ? accent.withAlpha (0.8f) : panelEdge.brighter (0.6f));
            g.setColour (bg);
            g.fillPath (p);
            if (pressed)
            {
                g.setColour (accent.withAlpha (s.down ? 0.3f : 0.15f));
                g.fillPath (p);
            }
            if (s.on || s.over)
            {
                g.setColour (c.withAlpha (0.2f));
                g.strokePath (p, PathStrokeType (4.0f));
            }
            g.setColour (c.withMultipliedAlpha (fade));
            g.strokePath (p, PathStrokeType (s.on ? 1.6f : 1.0f));
            break;
        }

        case Material::terminal:
        {
            // [ brackets ] at the outer ends, a bar between options; the chosen one in inverse video
            if (s.on)
            {
                g.setColour (accent.withMultipliedAlpha (fade));
                g.fillRect (r);
            }
            else if (s.down || s.over)
            {
                g.setColour (accent.withAlpha (s.down ? 0.3f : 0.12f));
                g.fillRect (r);
            }
            g.setColour ((s.on || s.over ? accent : dim).withMultipliedAlpha (fade));
            const float tick = jmin (5.0f, r.getWidth() * 0.2f);
            const float x0 = r.getX() + 0.5f, x1 = r.getRight() - 0.5f, y0 = r.getY() + 0.5f, y1 = r.getBottom() - 0.5f;
            if (j.left)
                g.drawLine (x0, y0 + 4.0f, x0, y1 - 4.0f, 1.0f);
            else
            {
                g.drawLine (x0, y0, x0, y1, 1.0f);
                g.drawLine (x0, y0, x0 + tick, y0, 1.0f);
                g.drawLine (x0, y1, x0 + tick, y1, 1.0f);
            }
            if (! j.right)
            {
                g.drawLine (x1, y0, x1, y1, 1.0f);
                g.drawLine (x1, y0, x1 - tick, y0, 1.0f);
                g.drawLine (x1, y1, x1 - tick, y1, 1.0f);
            }
            break;
        }

        case Material::candy:
        {
            // a plastic key: a darker lip under the face, the face sinks into it while pressed
            const float lip = jmin (3.5f, r.getHeight() * 0.14f);
            const float sink = s.down ? lip - 1.0f : 0.0f;
            const auto colour = (s.on ? accent : panelEdge.brighter (s.over ? 0.3f : 0.12f)).withMultipliedAlpha (fade);
            g.setColour (colour.darker (0.7f));
            g.fillPath (roundedPath (r.withTrimmedTop (sink), corner, j));
            const auto top = r.withTrimmedBottom (lip).translated (0.0f, sink);
            g.setColour (colour);
            g.fillPath (roundedPath (top, corner, j));
            const auto gloss = top.reduced (jmin (corner * 0.6f, top.getWidth() * 0.2f), 2.0f).withHeight (top.getHeight() * 0.38f);
            g.setColour (Colours::white.withAlpha (s.on ? 0.32f : 0.12f));
            g.fillRoundedRectangle (gloss, gloss.getHeight() * 0.5f);
            break;
        }

        case Material::metal:
        {
            const auto hi = panelEdge.brighter (0.3f), lo = panel.darker (0.4f);
            verticalFill (g, p, r, (pressed ? lo : hi).withMultipliedAlpha (fade), (pressed ? hi.darker (0.35f) : lo).withMultipliedAlpha (fade));
            if (s.on)
            {
                g.setColour (accent.withAlpha (0.18f));
                g.fillPath (p);
            }
            if (s.over && ! pressed)
            {
                g.setColour (Colours::white.withAlpha (0.06f));
                g.fillPath (p);
            }
            innerLine (g, p, Colours::white.withAlpha (pressed ? 0.05f : 0.2f), 1.0f, { 0.0f, 1.0f });
            if (! pressed)
                innerLine (g, p, Colours::black.withAlpha (0.5f), 1.0f, { 0.0f, -1.0f });
            g.setColour (Colours::black.withAlpha (0.75f));
            g.strokePath (p, PathStrokeType (1.0f));
            if (s.on)
            {
                g.setColour (accent);
                g.fillRect (r.getX() + 5.0f, r.getBottom() - 3.0f, r.getWidth() - 10.0f, 1.5f);
            }
            break;
        }

        case Material::chrome:
        {
            // kept dark enough that white labels (with their shadow) read on every part of it
            const auto tint = s.on ? accent.darker (0.15f) : panelEdge.brighter (0.3f);
            const float mid = r.getY() + r.getHeight() * 0.5f;
            {
                Graphics::ScopedSaveState save (g);
                g.reduceClipRegion (p);
                g.setGradientFill (ColourGradient (tint.brighter (0.3f).withMultipliedAlpha (fade), 0.0f, r.getY(),
                                                   tint.darker (0.3f).withMultipliedAlpha (fade), 0.0f, mid, false));
                g.fillRect (r.withBottom (mid));
                g.setGradientFill (ColourGradient (bg.withMultipliedAlpha (fade), 0.0f, mid,
                                                   tint.darker (0.1f).withMultipliedAlpha (fade), 0.0f, r.getBottom(), false));
                g.fillRect (r.withTop (mid));
                g.setColour (Colours::white.withAlpha (0.5f));
                g.fillRect (r.getX(), mid - 0.5f, r.getWidth(), 1.0f);
                if (s.down)
                {
                    g.setColour (Colours::black.withAlpha (0.2f));
                    g.fillRect (r);
                }
            }
            g.setColour (s.on ? accent : (s.over ? tint.brighter (0.4f) : tint.withAlpha (0.7f)));
            g.strokePath (p, PathStrokeType (1.0f));
            break;
        }
    }
}

Colour Surface::ink (State s, Colour accent)
{
    using namespace Palette;
    if (! s.enabled)
        return dim.withAlpha (0.6f);
    const auto& st = Theme::style();
    const auto plainInk = s.over ? text : text.withAlpha (0.88f);
    switch (st.material)
    {
        case Material::flat:     return s.on ? (st.buttons == Theme::Buttons::solid ? bg : st.buttons == Theme::Buttons::outline ? accent : bright) : plainInk;
        case Material::ceramic:  return s.on ? accent : plainInk;
        case Material::paper:    return s.on ? (st.buttons == Theme::Buttons::solid ? bg : accent) : plainInk;
        case Material::glass:    return s.on ? bright : plainInk;
        case Material::neon:     return s.on ? accent.brighter (0.35f) : plainInk;
        case Material::terminal: return s.on ? bg : (s.over ? accent : text);
        case Material::candy:    return s.on ? bg : plainInk;
        case Material::metal:    return s.on ? accent.brighter (0.2f) : plainInk;
        case Material::chrome:   return bright;
    }
    return plainInk;
}

Rectangle<float> Surface::labelArea (Rectangle<float> bounds, State s, Joins j)
{
    auto r = face (bounds, j);
    if (material() == Material::candy)
    {
        const float lip = jmin (3.5f, r.getHeight() * 0.14f);
        r = r.withTrimmedBottom (lip).translated (0.0f, s.down ? lip - 1.0f : 0.0f);
    }
    return r;
}

void Surface::label (Graphics& g, const String& text, Rectangle<float> area, const Font& f, State s, Colour accent,
                     Justification just)
{
    const auto box = area.toNearestInt();
    g.setFont (f);
    switch (material())
    {
        case Material::chrome:
        case Material::metal:
            g.setColour (Colours::black.withAlpha (0.6f));   // stamped into the surface
            g.drawFittedText (text, box.translated (material() == Material::chrome ? 1 : 0, 1), just, 1, 0.8f);
            break;
        case Material::neon:
            if (s.on)
            {
                g.setColour (accent.withAlpha (0.35f));
                g.drawFittedText (text, box.translated (0, 1), just, 1, 0.8f);
                g.drawFittedText (text, box.translated (0, -1), just, 1, 0.8f);
            }
            break;
        default:
            break;
    }
    g.setColour (ink (s, accent));
    g.drawFittedText (text, box, just, 1, 0.8f);
}

//==============================================================================
void Surface::sunken (Graphics& g, Rectangle<float> bounds, float corner, bool hot, Colour accent)
{
    using namespace Palette;
    const bool lt = light();
    const auto r = bounds.reduced (0.5f);
    const auto p = roundedPath (r, corner, {});
    switch (material())
    {
        case Material::flat:
            g.setColour (hot ? field : bg);
            g.fillPath (p);
            g.setColour (hot ? accent.withAlpha (0.7f) : panelEdge.brighter (0.15f));
            g.strokePath (p, PathStrokeType (1.0f));
            break;

        case Material::ceramic:
            g.setColour (panel.darker (lt ? 0.06f : 0.28f));
            g.fillPath (p);
            topShade (g, p, r, lt ? 0.12f : 0.45f);
            innerLine (g, p, Colours::white.withAlpha (lt ? 0.7f : 0.05f), 1.0f, { 0.0f, -1.0f });
            if (hot)
            {
                g.setColour (accent.withAlpha (0.55f));
                g.strokePath (p, PathStrokeType (1.2f));
            }
            break;

        case Material::paper:
            g.setColour (lt ? panel.interpolatedWith (Colours::white, 0.5f) : bg.interpolatedWith (panel, 0.4f));
            g.fillPath (p);
            topShade (g, p, r, lt ? 0.05f : 0.25f, 3.0f);
            g.setColour (hot ? accent.withAlpha (0.8f) : panelEdge);
            g.strokePath (p, PathStrokeType (1.0f));
            break;

        case Material::glass:
            g.setColour (bg.interpolatedWith (panel, 0.45f));
            g.fillPath (p);
            topShade (g, p, r, lt ? 0.06f : 0.3f);
            g.setColour (hot ? accent.withAlpha (0.8f) : (lt ? panelEdge : Colours::white.withAlpha (0.14f)));
            g.strokePath (p, PathStrokeType (1.0f));
            break;

        case Material::neon:
            g.setColour (bg);
            g.fillPath (p);
            if (hot)
            {
                g.setColour (accent.withAlpha (0.18f));
                g.strokePath (p, PathStrokeType (3.5f));
            }
            g.setColour (hot ? accent : panelEdge.brighter (0.35f));
            g.strokePath (p, PathStrokeType (1.0f));
            break;

        case Material::terminal:
            g.setColour (bg);
            g.fillRect (r);
            g.setColour (hot ? accent : dim.withAlpha (0.7f));
            g.drawRect (r, 1.0f);
            break;

        case Material::candy:
            g.setColour (bg.interpolatedWith (panel, 0.5f));
            g.fillPath (p);
            topShade (g, p, r, 0.35f);
            if (hot)
            {
                g.setColour (accent.withAlpha (0.7f));
                g.strokePath (p, PathStrokeType (1.5f));
            }
            break;

        case Material::metal:
        case Material::chrome:
            verticalFill (g, p, r, bg.darker (0.3f), bg.interpolatedWith (panel, 0.5f));
            innerLine (g, p, Colours::black.withAlpha (0.6f), 1.0f, { 0.0f, 1.0f });
            g.setColour (hot ? accent.withAlpha (0.8f) : Colours::white.withAlpha (0.12f));
            g.strokePath (p, PathStrokeType (1.0f));
            break;
    }
}

//==============================================================================
Rectangle<float> Surface::sectionFace (Rectangle<int> bounds)
{
    const auto r = bounds.toFloat();
    switch (material())
    {
        case Material::ceramic: return { r.getX() + 1.0f, r.getY() + 1.0f, r.getWidth() - 3.0f, r.getHeight() - 4.5f };
        case Material::paper:   return { r.getX() + 1.0f, r.getY() + 0.5f, r.getWidth() - 2.5f, r.getHeight() - 4.0f };
        default:                return r;
    }
}

void Surface::section (Graphics& g, Rectangle<int> bounds, const String& title, Colour accent)
{
    using namespace Palette;
    using Theme::Panels;
    using Theme::Titles;
    const auto& s = Theme::style();
    const bool lt = s.light;
    accent = Theme::follow (accent);
    const auto r = sectionFace (bounds);
    const auto p = roundedPath (r, s.radius, {});
    const auto titleFont = Theme::font (12.0f, true);
    const auto titleArea = bounds.reduced (14, 8).removeFromTop (16);

    switch (s.material)
    {
        case Material::flat:
        {
            g.setColour (s.panels == Panels::outline ? bg.interpolatedWith (panel, 0.55f) : panel);
            g.fillRoundedRectangle (r, s.radius);
            if (s.panels == Panels::gradient || s.panels == Panels::glass)
            {
                const auto b = r.withHeight (jmin (r.getHeight(), 56.0f));
                g.setGradientFill (ColourGradient (panelEdge.withAlpha (lt ? 0.5f : 0.45f), 0.0f, b.getY(),
                                                   panelEdge.withAlpha (0.0f), 0.0f, b.getBottom(), false));
                g.fillRoundedRectangle (b, s.radius);
            }
            if (s.border > 0.0f)
            {
                g.setColour (s.panels == Panels::outline ? accent.withAlpha (0.45f) : panelEdge);
                g.drawRoundedRectangle (r.reduced (s.border * 0.5f), s.radius, s.border);
            }
            if (s.panels == Panels::glass)
            {
                g.setColour (bright.withAlpha (lt ? 0.7f : 0.08f));
                g.drawHorizontalLine (bounds.getY() + 1, r.getX() + s.radius, r.getRight() - s.radius);
            }
            break;
        }

        case Material::ceramic:
            softShadow (g, p, lt ? 0.15f : 0.5f, 3.0f, { 0.0f, 1.5f });
            g.setColour (panel);
            g.fillPath (p);
            innerLine (g, p, Colours::white.withAlpha (lt ? 1.0f : 0.08f), 1.5f, { 0.0f, 1.2f });
            break;

        case Material::paper:
            softShadow (g, p, lt ? 0.1f : 0.45f, 3.0f, { 0.0f, 1.5f });
            g.setColour (panel);
            g.fillPath (p);
            if (! lt)
            {
                g.setColour (panelEdge.withAlpha (0.5f));
                g.strokePath (p, PathStrokeType (1.0f));
            }
            break;

        case Material::glass:
            g.setColour (panel);
            g.fillPath (p);
            band (g, p, r, Colours::white.withAlpha (lt ? 0.7f : 0.1f), 60.0f);
            g.setColour (lt ? panelEdge : Colours::white.withAlpha (0.13f));
            g.strokePath (p, PathStrokeType (1.0f));
            innerLine (g, p, Colours::white.withAlpha (lt ? 1.0f : 0.25f), 1.0f, { 0.0f, 1.0f });
            break;

        case Material::neon:
        {
            g.setColour (panel);
            g.fillPath (p);
            const auto tube = roundedPath (r.reduced (2.0f), s.radius, {});
            g.setColour (accent.withAlpha (0.1f));
            g.strokePath (tube, PathStrokeType (4.0f));
            g.setColour (accent.withAlpha (0.55f));
            g.strokePath (tube, PathStrokeType (1.0f));
            break;
        }

        case Material::terminal:
        {
            // a box whose top line runs through the title:  +-- TITLE ---------+
            g.setColour (panel);
            g.fillRect (r);
            const float y = title.isEmpty() ? r.getY() + 0.5f : (float) titleArea.getCentreY();
            const float x0 = r.getX() + 0.5f, x1 = r.getRight() - 0.5f, y1 = r.getBottom() - 0.5f;
            const float tx = (float) titleArea.getX() - 6.0f;
            const float tw = title.isEmpty() ? 0.0f : GlyphArrangement::getStringWidth (titleFont, title) + 12.0f;
            g.setColour (accent.withAlpha (0.5f));
            g.drawLine (x0, y, x0, y1);
            g.drawLine (x1, y, x1, y1);
            g.drawLine (x0, y1, x1, y1);
            if (tw > 0.0f)
            {
                g.drawLine (x0, y, tx, y);
                g.drawLine (jmin (x1, tx + tw), y, x1, y);
                g.setColour (accent);
                g.setFont (titleFont);
                g.drawText (title, Rectangle<float> (tx + 6.0f, y - 8.0f, jmax (0.0f, jmin (tw, x1 - tx) - 6.0f), 16.0f),
                            Justification::centredLeft);
            }
            else
            {
                g.drawLine (x0, y, x1, y);
            }
            return;
        }

        case Material::candy:
        {
            g.setColour (panel.darker (0.45f));
            g.fillPath (p);
            const auto top = roundedPath (r.withTrimmedBottom (4.0f), s.radius, {});
            g.setColour (panel);
            g.fillPath (top);
            innerLine (g, top, Colours::white.withAlpha (0.1f), 1.5f, { 0.0f, 1.2f });
            break;
        }

        case Material::metal:
        {
            g.setColour (panel);
            g.fillPath (p);
            band (g, p, r, panelEdge.withAlpha (0.5f), 56.0f);
            g.setColour (Colours::black.withAlpha (0.75f));
            g.strokePath (p, PathStrokeType (1.0f));
            g.setColour (Colours::white.withAlpha (0.07f));
            g.strokePath (roundedPath (r.reduced (1.5f), jmax (0.0f, s.radius - 1.5f), {}), PathStrokeType (1.0f));
            if (r.getWidth() > 80.0f && r.getHeight() > 50.0f)
                for (auto c : { r.getTopLeft() + Point<float> (6.5f, 6.5f), r.getTopRight() + Point<float> (-6.5f, 6.5f),
                                r.getBottomLeft() + Point<float> (6.5f, -6.5f), r.getBottomRight() + Point<float> (-6.5f, -6.5f) })
                    rivet (g, c);
            break;
        }

        case Material::chrome:
        {
            g.setColour (panel);
            g.fillPath (p);
            band (g, p, r, panelEdge.withAlpha (0.45f), 56.0f);
            g.setColour (panelEdge);
            g.strokePath (p, PathStrokeType (1.0f));
            Graphics::ScopedSaveState save (g);   // a laser line along the top edge
            g.reduceClipRegion (p);
            g.setColour (accent.withAlpha (0.22f));
            g.fillRect (r.getX(), r.getY(), r.getWidth(), 4.0f);
            g.setColour (accent.withAlpha (0.9f));
            g.fillRect (r.getX(), r.getY(), r.getWidth(), 1.5f);
            break;
        }
    }

    if (title.isEmpty())
        return;
    g.setFont (titleFont);
    if (s.material == Material::metal)
    {
        g.setColour (Colours::black.withAlpha (0.6f));
        g.drawText (title, titleArea.translated (0, 1), Justification::centredLeft);
    }
    else if (s.material == Material::neon)
    {
        g.setColour (accent.withAlpha (0.28f));
        g.drawText (title, titleArea.translated (0, 1), Justification::centredLeft);
        g.drawText (title, titleArea.translated (1, 0), Justification::centredLeft);
    }
    g.setColour (accent);
    g.drawText (title, titleArea, Justification::centredLeft);
    if (s.titles == Titles::underline)
    {
        const float tw = (float) GlyphArrangement::getStringWidthInt (titleFont, title);
        g.setColour (accent.withAlpha (0.6f));
        g.fillRect ((float) titleArea.getX(), (float) titleArea.getBottom() + 1.0f, jmin (tw, (float) titleArea.getWidth()), 1.0f);
    }
    else if (s.titles == Titles::bar)
    {
        g.fillRect ((float) bounds.getX() + 5.0f, (float) titleArea.getY() + 2.0f, 3.0f, (float) titleArea.getHeight() - 4.0f);
    }
}

void Theme::drawSection (Graphics& g, Rectangle<int> r, const String& title, Colour accent)
{
    Surface::section (g, r, title, accent);
}

//==============================================================================
bool Surface::hasTrays()
{
    switch (material())
    {
        case Material::ceramic:
        case Material::paper:
        case Material::glass:
        case Material::candy:
        case Material::metal:
            return true;
        default:
            return false;
    }
}

Path Surface::trayShape (Rectangle<float> body, Rectangle<float> tab, float radius)
{
    // one outline: the body, with the tab rising out of its top edge through two small fillets
    const float f = 4.0f, tr = jmin (5.0f, tab.getHeight() * 0.5f), top = body.getY();
    const float tx0 = jmax (tab.getX(), body.getX() + f + 1.0f), tx1 = jmin (tab.getRight(), body.getRight() - f - 1.0f);
    radius = jmax (0.0f, jmin (radius, body.getHeight() * 0.5f, tx0 - f - body.getX(), body.getRight() - tx1 - f));
    const float x0 = body.getX(), x1 = body.getRight(), y1 = body.getBottom(), ty = tab.getY();

    Path p;
    p.startNewSubPath (x0 + radius, top);
    p.lineTo (tx0 - f, top);
    p.quadraticTo (tx0, top, tx0, top - f);
    p.lineTo (tx0, ty + tr);
    p.quadraticTo (tx0, ty, tx0 + tr, ty);
    p.lineTo (tx1 - tr, ty);
    p.quadraticTo (tx1, ty, tx1, ty + tr);
    p.lineTo (tx1, top - f);
    p.quadraticTo (tx1, top, tx1 + f, top);
    p.lineTo (x1 - radius, top);
    p.quadraticTo (x1, top, x1, top + radius);
    p.lineTo (x1, y1 - radius);
    p.quadraticTo (x1, y1, x1 - radius, y1);
    p.lineTo (x0 + radius, y1);
    p.quadraticTo (x0, y1, x0, y1 - radius);
    p.lineTo (x0, top + radius);
    p.quadraticTo (x0, top, x0 + radius, top);
    p.closeSubPath();
    return p;
}

void Surface::tray (Graphics& g, const Path& shape, Colour accent)
{
    using namespace Palette;
    const bool lt = light();
    const auto r = shape.getBounds();
    switch (material())
    {
        case Material::ceramic:
            softShadow (g, shape, lt ? 0.16f : 0.5f, 2.5f, { 0.0f, 1.0f });
            g.setColour (panel.interpolatedWith (bg, lt ? 0.3f : 0.35f));   // a little darker than the glazed keys on it
            g.fillPath (shape);
            innerLine (g, shape, Colours::white.withAlpha (lt ? 0.9f : 0.08f), 1.3f, { 0.0f, 1.0f });
            break;

        case Material::paper:
            softShadow (g, shape, lt ? 0.08f : 0.4f, 2.5f, { 0.0f, 1.0f });
            g.setColour (lt ? bg.interpolatedWith (panel, 0.55f) : panel.interpolatedWith (bg, 0.45f));
            g.fillPath (shape);
            break;

        case Material::glass:
            g.setColour (Colours::white.withAlpha (lt ? 0.45f : 0.05f));
            g.fillPath (shape);
            g.setColour (lt ? panelEdge : Colours::white.withAlpha (0.14f));
            g.strokePath (shape, PathStrokeType (1.0f));
            innerLine (g, shape, Colours::white.withAlpha (lt ? 0.9f : 0.2f), 1.0f, { 0.0f, 1.0f });
            break;

        case Material::candy:
            g.setColour (panel.darker (0.45f));
            g.fillPath (shape, AffineTransform::translation (0.0f, 2.5f));
            g.setColour (panel.interpolatedWith (accent, 0.06f));
            g.fillPath (shape);
            break;

        case Material::metal:
            verticalFill (g, shape, r, panelEdge.brighter (0.05f), panel.darker (0.3f));
            g.setColour (Colours::black.withAlpha (0.75f));
            g.strokePath (shape, PathStrokeType (1.0f));
            innerLine (g, shape, Colours::white.withAlpha (0.12f), 1.0f, { 0.0f, 1.0f });
            break;

        default:
            break;
    }
}

//==============================================================================
void Surface::frame (Graphics& g, Rectangle<float> v)
{
    using namespace Palette;
    const bool lt = light();
    const auto& s = Theme::style();
    switch (material())
    {
        case Material::flat:
            break;

        case Material::ceramic:
        {
            // the view sits in a well pressed into the glaze
            const auto w = v.expanded (5.0f);
            const auto p = roundedPath (w, s.radius, {});
            g.setColour (bg.darker (lt ? 0.07f : 0.3f));
            g.fillPath (p);
            topShade (g, p, w, lt ? 0.16f : 0.5f, 6.0f);
            innerLine (g, p, Colours::white.withAlpha (lt ? 0.8f : 0.06f), 1.0f, { 0.0f, -1.0f });
            break;
        }

        case Material::paper:
            softShadow (g, roundedPath (v, 2.0f, {}), lt ? 0.14f : 0.5f, 4.0f, { 0.0f, 2.0f });
            break;

        case Material::glass:
            g.setColour (lt ? panelEdge : Colours::white.withAlpha (0.16f));
            g.drawRoundedRectangle (v.expanded (2.5f), s.radius * 0.5f, 1.0f);
            break;

        case Material::neon:
        {
            const auto p = roundedPath (v.expanded (2.5f), s.radius, {});
            g.setColour (Accent::view.withAlpha (0.14f));
            g.strokePath (p, PathStrokeType (4.0f));
            g.setColour (Accent::view.withAlpha (0.6f));
            g.strokePath (p, PathStrokeType (1.0f));
            break;
        }

        case Material::terminal:
            g.setColour (dim.withAlpha (0.6f));
            g.drawRect (v.expanded (3.0f), 1.0f);
            break;

        case Material::candy:
        {
            const auto w = v.expanded (5.0f);
            g.setColour (panelEdge.darker (0.5f));
            g.fillRoundedRectangle (w.translated (0.0f, 3.0f), s.radius);
            g.setColour (panelEdge);
            g.fillRoundedRectangle (w, s.radius);
            break;
        }

        case Material::metal:
        {
            const auto w = v.expanded (6.0f);
            const auto p = roundedPath (w, 3.0f, {});
            verticalFill (g, p, w, panelEdge.brighter (0.15f), panel.darker (0.4f));
            g.setColour (Colours::black.withAlpha (0.8f));
            g.strokePath (p, PathStrokeType (1.0f));
            g.drawRect (v.expanded (1.0f), 1.0f);
            for (auto c : { w.getTopLeft() + Point<float> (3.0f, 3.0f), w.getTopRight() + Point<float> (-3.0f, 3.0f),
                            w.getBottomLeft() + Point<float> (3.0f, -3.0f), w.getBottomRight() + Point<float> (-3.0f, -3.0f) })
                rivet (g, c);
            break;
        }

        case Material::chrome:
        {
            const auto w = v.expanded (3.0f);
            g.setGradientFill (ColourGradient (pink, w.getX(), 0.0f, Accent::view, w.getRight(), 0.0f, false));
            g.drawRoundedRectangle (w, s.radius * 0.6f, 1.5f);
            break;
        }
    }
}

void Surface::backdrop (Graphics& g, Rectangle<int> window)
{
    using namespace Palette;
    using Theme::Backdrop;
    const auto clip = g.getClipBounds().getIntersection (window);
    if (clip.isEmpty())
        return;

    switch (Theme::style().backdrop)
    {
        case Backdrop::none:
            break;

        case Backdrop::grid:
        {
            const int step = 24;
            g.setColour (panelEdge.withAlpha (0.45f));
            for (int x = (clip.getX() / step) * step; x < clip.getRight(); x += step)
                g.fillRect (x, clip.getY(), 1, clip.getHeight());
            for (int y = (clip.getY() / step) * step; y < clip.getBottom(); y += step)
                g.fillRect (clip.getX(), y, clip.getWidth(), 1);
            break;
        }

        case Backdrop::scanlines:
            g.setColour (text.withAlpha (0.035f));
            for (int y = (clip.getY() / 3) * 3; y < clip.getBottom(); y += 3)
                g.fillRect (clip.getX(), y, clip.getWidth(), 1);
            break;

        case Backdrop::dots:
            g.setTiledImageFill (dotsTile(), 0, 0, 1.0f);
            g.fillRect (clip);
            break;

        case Backdrop::sunset:
        {
            // a warm glow sinking from the top edge, and the sun's stripes along the bottom of the window
            const float h = (float) window.getHeight();
            g.setGradientFill (ColourGradient (amber.withAlpha (0.12f), 0.0f, 0.0f, pink.withAlpha (0.0f), 0.0f, h * 0.3f, false));
            g.fillRect (clip);
            g.setGradientFill (ColourGradient (pink.withAlpha (0.0f), 0.0f, h * 0.7f, pink.withAlpha (0.12f), 0.0f, h, false));
            g.fillRect (clip);
            break;
        }

        case Backdrop::rays:
        {
            // light falling in from above in a few slanted shafts
            const float w = (float) window.getWidth(), h = (float) window.getHeight();
            g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.05f), 0.0f, 0.0f, Colours::white.withAlpha (0.0f), 0.0f, h * 0.6f, false));
            for (int i = 0; i < 5; ++i)
            {
                const float x = w * (0.04f + 0.2f * (float) i), width = 40.0f + 25.0f * (float) (i % 3);
                Path shaft;
                shaft.addQuadrilateral (x, 0.0f, x + width, 0.0f, x + width + h * 0.35f, h, x + h * 0.35f, h);
                g.fillPath (shaft);
            }
            break;
        }

        case Backdrop::grain:
            g.setTiledImageFill (grainTile(), 0, 0, 1.0f);
            g.fillRect (clip);
            break;
    }
}
