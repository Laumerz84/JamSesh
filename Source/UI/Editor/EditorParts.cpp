// Non-header parts of the editor shell: the look-and-feel, the chain model, the chain strip,
// the module header, the split handle and the OUTPUT panel.
#include "LookAndFeel.h"
#include "Chain.h"
#include "../StreamSafe.h"
#include "../Surface.h"
#include "../Themes.h"

using namespace juce;

//==============================================================================
// Look and feel
//==============================================================================
namespace
{
    Colour accentFor (const Component& c) { return Theme::accentOf (&c, Palette::green); }

    // Corner radius of a control: the theme's, scaled from the original design value (5 px), never
    // more than a pill
    float ctlCorner (float height, float original = 5.0f, float maxFraction = 0.3f)
    {
        const float r = Theme::style().controlRadius * original / 5.0f;
        return jmin (r, height * (r > 8.0f ? 0.5f : maxFraction));
    }

    // A segmented button made by ParamSegmented carries its author's accent in buttonOnColourId
    Colour buttonAccent (const Button& b)
    {
        if (b.isColourSpecified (TextButton::buttonOnColourId))
            return b.findColour (TextButton::buttonOnColourId).withAlpha (1.0f);
        return accentFor (b);
    }

    Surface::Joins joinsOf (const Button& b) { return { b.isConnectedOnLeft(), b.isConnectedOnRight() }; }
}

ScopeLookAndFeel::ScopeLookAndFeel()
{
    Themes::current();   // loads the saved theme before anything reads the palette
    applyTheme();
}

// The colour table, from the current palette (UI/Themes.h): run again when the theme changes
void ScopeLookAndFeel::applyTheme()
{
    using namespace Palette;
    setColour (Slider::textBoxTextColourId, text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    setColour (Slider::textBoxHighlightColourId, green.withAlpha (0.3f));
    setColour (Label::textColourId, dim);
    setColour (Label::textWhenEditingColourId, text);
    setColour (ComboBox::backgroundColourId, bg);
    setColour (ComboBox::outlineColourId, panelEdge);
    setColour (ComboBox::textColourId, text);
    setColour (ComboBox::arrowColourId, green);
    setColour (PopupMenu::backgroundColourId, raised);
    setColour (PopupMenu::textColourId, text);
    setColour (PopupMenu::headerTextColourId, dim);
    setColour (PopupMenu::highlightedBackgroundColourId, green.withAlpha (0.2f));
    setColour (PopupMenu::highlightedTextColourId, bright);
    setColour (ToggleButton::textColourId, text);
    setColour (ToggleButton::tickColourId, green);
    setColour (ToggleButton::tickDisabledColourId, dim);
    setColour (TextButton::buttonColourId, bg);
    setColour (TextButton::textColourOffId, text);
    setColour (TextButton::textColourOnId, bright);
    setColour (TextEditor::backgroundColourId, viewBg);
    setColour (TextEditor::textColourId, text);
    setColour (TextEditor::outlineColourId, panelEdge);
    setColour (TextEditor::focusedOutlineColourId, green);
    setColour (TextEditor::highlightColourId, green.withAlpha (0.3f));
    setColour (TextEditor::highlightedTextColourId, bright);
    setColour (CaretComponent::caretColourId, green);
    setColour (ScrollBar::thumbColourId, panelEdge.brighter (0.3f));
    setColour (ScrollBar::trackColourId, Colours::transparentBlack);
    setColour (TabbedComponent::backgroundColourId, bg);
    setColour (TabbedComponent::outlineColourId, Colours::transparentBlack);
    setColour (TabbedButtonBar::tabOutlineColourId, Colours::transparentBlack);
    setColour (TooltipWindow::backgroundColourId, tooltip);
    setColour (TooltipWindow::textColourId, text);
    setColour (TooltipWindow::outlineColourId, panelEdge.brighter (0.2f));
    setColour (ListBox::backgroundColourId, viewBg);
    setColour (ListBox::outlineColourId, panelEdge);
    setColour (ListBox::textColourId, text);
    setColour (AlertWindow::backgroundColourId, panel);
    setColour (AlertWindow::textColourId, text);
    setColour (AlertWindow::outlineColourId, panelEdge);
    setColour (ResizableWindow::backgroundColourId, bg);
    setColour (Slider::rotarySliderFillColourId, green);
    setColour (Slider::rotarySliderOutlineColourId, panelEdge);
    setColour (Slider::trackColourId, green);
    setColour (Slider::backgroundColourId, panelEdge);
    setColour (Slider::thumbColourId, text);
}

//------------------------------------------------------------------------------
// Knobs: the theme picks the style (Theme::Knob; arc = the original look). Every style keeps the
// lit range, the snap ticks and a pointer readable.
void ScopeLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos,
                                         float startAngle, float endAngle, Slider& s)
{
    using namespace Palette;
    using Theme::Knob;
    const auto& st = Theme::style();
    const auto accent = s.isEnabled() ? accentFor (s) : dim.withAlpha (0.6f);
    const auto bounds = Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
    const float r = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const float cx = bounds.getCentreX(), cy = bounds.getCentreY();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const bool bipolar = s.getProperties().getWithDefault ("bipolar", false);
    const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    const float arcR = r - 2.0f;
    const bool hot = s.isMouseOverOrDragging() && s.isEnabled();
    const auto ks = st.knob;
    const float trackW = ks == Knob::flat ? 5.0f : ks == Knob::dial ? 2.0f : ks == Knob::dome ? 4.5f : 3.0f;
    const auto cap = ks == Knob::flat ? PathStrokeType::butt : PathStrokeType::rounded;
    const bool moved = std::abs (angle - from) > 0.001f;

    if (ks == Knob::ring)
    {
        // segmented LED ring, lit between the rest point and the value
        const int n = 21;
        const float d = jlimit (2.0f, 4.0f, arcR * 0.16f);
        const float lo = jmin (from, angle) - 0.01f, hi = jmax (from, angle) + 0.01f;
        for (int i = 0; i < n; ++i)
        {
            const float a = startAngle + (float) i / (float) (n - 1) * (endAngle - startAngle);
            const auto c = Point<float> (cx + arcR * std::sin (a), cy - arcR * std::cos (a));
            const bool lit = moved && a >= lo && a <= hi;
            if (lit && st.glow > 0.0f)
            {
                g.setColour (accent.withAlpha (st.glow));
                g.fillEllipse (Rectangle<float> (d * 2.2f, d * 2.2f).withCentre (c));
            }
            g.setColour (lit ? accent : panelEdge.brighter (0.2f));
            g.fillEllipse (Rectangle<float> (d, d).withCentre (c));
        }
    }
    else
    {
        Path track;
        track.addCentredArc (cx, cy, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (panelEdge);
        g.strokePath (track, PathStrokeType (trackW, PathStrokeType::curved, cap));
    }

    // ticks just outside the track at the snap points (default, zero); lit while the dial sits on one
    if (auto* marks = s.getProperties().getVarPointer ("snapMarks"); marks != nullptr && marks->isArray())
        for (const auto& m : *marks->getArray())
        {
            const float mp = (float) (double) m;
            const float a = startAngle + mp * (endAngle - startAngle);
            const bool on = std::abs (mp - pos) < 0.002f;
            const float tr = r + 1.0f;
            const float d = on ? 4.0f : 3.0f;
            g.setColour (on ? bright : dim.withAlpha (0.7f));
            g.fillEllipse (cx + tr * std::sin (a) - d * 0.5f, cy - tr * std::cos (a) - d * 0.5f, d, d);
        }

    if (ks != Knob::ring && moved)
    {
        Path value;
        value.addCentredArc (cx, cy, arcR, arcR, 0.0f, jmin (from, angle), jmax (from, angle), true);
        if (ks == Knob::glow && st.glow > 0.0f)
        {
            g.setColour (accent.withAlpha (st.glow * 0.4f));
            g.strokePath (value, PathStrokeType (11.0f, PathStrokeType::curved, PathStrokeType::rounded));
        }
        if ((ks == Knob::arc || ks == Knob::glow) && st.glow > 0.0f)
        {
            g.setColour (accent.withAlpha (st.glow));
            g.strokePath (value, PathStrokeType (7.0f, PathStrokeType::curved, PathStrokeType::rounded));
        }
        g.setColour (accent);
        g.strokePath (value, PathStrokeType (trackW, PathStrokeType::curved, cap));
    }

    const auto pointer = [&] (float r0, float r1, float width, Colour c)
    {
        g.setColour (s.isEnabled() ? c : dim);
        g.drawLine ({ { cx + r0 * std::sin (angle), cy - r0 * std::cos (angle) },
                      { cx + r1 * std::sin (angle), cy - r1 * std::cos (angle) } }, width);
    };

    switch (ks)
    {
        case Knob::arc:
        case Knob::glow:
        {
            // knob body with a soft top light (glow: a dark body with a coloured pointer)
            const float br = jmax (2.0f, r - 7.0f);
            if (ks == Knob::glow)
                g.setColour (hot ? knobHot : bg);
            else
                g.setGradientFill (ColourGradient (hot ? knobHot : knobTop, cx, cy - br, panel, cx, cy + br, false));
            g.fillEllipse (cx - br, cy - br, br * 2.0f, br * 2.0f);
            g.setColour (hot ? accent.withAlpha (0.5f) : knobRing);
            g.drawEllipse (cx - br, cy - br, br * 2.0f, br * 2.0f, 1.0f);
            pointer (br * 0.3f, br - 2.0f, 2.0f, ks == Knob::glow ? accent : text);
            break;
        }
        case Knob::dial:
        {
            // a bigger flat cap with a coloured pointer, like a hardware dial
            const float br = jmax (2.0f, r - 5.0f);
            g.setColour (hot ? knobHot : knobTop);
            g.fillEllipse (cx - br, cy - br, br * 2.0f, br * 2.0f);
            g.setColour (hot ? accent.withAlpha (0.6f) : knobRing);
            g.drawEllipse (cx - br, cy - br, br * 2.0f, br * 2.0f, 1.2f);
            pointer (br * 0.25f, br - 1.5f, 2.5f, accent);
            break;
        }
        case Knob::flat:
        {
            // no body: a dot rides inside the arc
            const float pr = arcR - 7.0f;
            g.setColour (s.isEnabled() ? (hot ? accent : text) : dim);
            g.fillEllipse (Rectangle<float> (5.0f, 5.0f).withCentre ({ cx + pr * std::sin (angle), cy - pr * std::cos (angle) }));
            break;
        }
        case Knob::ring:
        {
            const float br = jmax (2.0f, r - 8.0f);
            g.setColour (hot ? knobHot : bg);
            g.fillEllipse (cx - br, cy - br, br * 2.0f, br * 2.0f);
            g.setColour (hot ? accent : knobRing);
            g.drawEllipse (cx - br, cy - br, br * 2.0f, br * 2.0f, 1.0f);
            pointer (br * 0.2f, br - 2.0f, 2.0f, accent);
            break;
        }
        case Knob::dome:
        {
            // a glazed dome lit from the top left, with a coloured dimple pointing at the value
            const float br = jmax (2.0f, r - 8.0f);
            const auto dome = Rectangle<float> (cx - br, cy - br, br * 2.0f, br * 2.0f);
            Path outline;
            outline.addEllipse (dome);
            Surface::softShadow (g, outline, st.light ? 0.2f : 0.55f, 3.0f, { 0.0f, 1.5f });
            g.setGradientFill (ColourGradient (st.light ? panel.interpolatedWith (Colours::white, 0.75f) : panel.brighter (0.25f),
                                               cx - br * 0.35f, cy - br * 0.55f,
                                               panel.darker (st.light ? 0.06f : 0.2f), cx + br * 0.6f, cy + br * 0.9f, true));
            g.fillEllipse (dome);
            g.setColour (hot ? accent.withAlpha (0.45f) : Colours::white.withAlpha (st.light ? 0.6f : 0.08f));
            g.drawEllipse (dome.reduced (0.6f), hot ? 1.2f : 0.8f);
            const float pr = br * 0.62f;
            const auto dimple = Point<float> (cx + pr * std::sin (angle), cy - pr * std::cos (angle));
            g.setColour (s.isEnabled() ? accent : dim);
            g.fillEllipse (Rectangle<float> (hot ? 6.0f : 5.0f, hot ? 6.0f : 5.0f).withCentre (dimple));
            break;
        }
    }
}

void ScopeLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                                         Slider::SliderStyle style, Slider& s)
{
    if (style != Slider::LinearHorizontal && style != Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, minPos, maxPos, style, s);
        return;
    }

    using namespace Palette;
    const auto accent = s.isEnabled() ? accentFor (s) : dim;
    const bool horiz = style == Slider::LinearHorizontal;
    const auto r = Rectangle<int> (x, y, w, h).toFloat();

    if (! Surface::plain())
    {
        // a groove in the material with a raised thumb
        const float t = 6.0f;
        const auto groove = horiz ? r.withSizeKeepingCentre (r.getWidth(), t) : r.withSizeKeepingCentre (t, r.getHeight());
        Surface::sunken (g, groove, t * 0.5f, false, accent);
        const auto lit = (horiz ? groove.withRight (pos) : groove.withTop (pos)).reduced (1.5f);
        g.setColour (accent);
        g.fillRoundedRectangle (lit, lit.getHeight() * 0.5f);
        const auto c = horiz ? Point<float> (pos, r.getCentreY()) : Point<float> (r.getCentreX(), pos);
        Surface::raised (g, Rectangle<float> (20.0f, 20.0f).withCentre (c), 10.0f,
                         { s.isMouseOverOrDragging(), s.isMouseButtonDown(), false, s.isEnabled() }, accent);
        return;
    }

    const float t = 4.0f;
    auto trackR = horiz ? r.withSizeKeepingCentre (r.getWidth(), t) : r.withSizeKeepingCentre (t, r.getHeight());
    g.setColour (panelEdge);
    g.fillRoundedRectangle (trackR, t * 0.5f);

    auto fill = horiz ? trackR.withRight (pos) : trackR.withTop (pos);
    g.setColour (accent);
    g.fillRoundedRectangle (fill, t * 0.5f);

    const auto c = horiz ? Point<float> (pos, r.getCentreY()) : Point<float> (r.getCentreX(), pos);
    g.setColour (text);
    g.fillEllipse (Rectangle<float> (12.0f, 12.0f).withCentre (c));
    g.setColour (accent);
    g.drawEllipse (Rectangle<float> (12.0f, 12.0f).withCentre (c), 1.5f);
}

Label* ScopeLookAndFeel::createSliderTextBox (Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (Theme::font (11.5f));
    l->setColour (Label::outlineWhenEditingColourId, accentFor (s));
    l->setColour (Label::backgroundWhenEditingColourId, Palette::viewBg);
    return l;
}

//------------------------------------------------------------------------------
int ScopeLookAndFeel::getTabButtonBestWidth (TabBarButton& b, int)
{
    return GlyphArrangement::getStringWidthInt (Theme::font (12.0f, true), b.getButtonText()) + 20;
}

void ScopeLookAndFeel::drawTabButton (TabBarButton& b, Graphics& g, bool isMouseOver, bool)
{
    using namespace Palette;
    const auto accent = accentFor (b);
    const auto r = b.getActiveArea().toFloat().reduced (2.0f, 3.0f);
    const bool front = b.isFrontTab();
    if (! Surface::plain())
    {
        const Surface::State st { isMouseOver, false, front, b.isEnabled() };
        Surface::raised (g, r, ctlCorner (r.getHeight(), 5.0f, 0.5f), st, accent);
        Surface::label (g, b.getButtonText(), Surface::labelArea (r, st), Theme::font (12.0f, true), st, accent);
        return;
    }
    g.setColour (front ? accent.withAlpha (0.16f) : (isMouseOver ? panelEdge : panel));
    g.fillRoundedRectangle (r, ctlCorner (r.getHeight(), 5.0f, 0.5f));
    if (front)
    {
        g.setColour (accent);
        g.fillRect (r.getX() + 6.0f, r.getBottom() - 2.0f, r.getWidth() - 12.0f, 2.0f);
    }
    g.setColour (front ? text : dim);
    g.setFont (Theme::font (12.0f, true));
    g.drawText (b.getButtonText(), r, Justification::centred);
}

void ScopeLookAndFeel::drawTabbedButtonBarBackground (TabbedButtonBar&, Graphics&) {}

void ScopeLookAndFeel::drawTabAreaBehindFrontButton (TabbedButtonBar&, Graphics& g, int w, int h)
{
    g.setColour (Palette::panelEdge);
    g.fillRect (0, h - 1, w, 1);
}

//------------------------------------------------------------------------------
void ScopeLookAndFeel::drawButtonBackground (Graphics& g, Button& b, const Colour&, bool isMouseOver, bool isMouseDown)
{
    using namespace Palette;
    const auto accent = buttonAccent (b);
    const bool on = b.getToggleState();
    if (! Surface::plain())
    {
        const auto bounds = b.getLocalBounds().toFloat();
        Surface::raised (g, bounds, ctlCorner (bounds.getHeight()), { isMouseOver, isMouseDown, on, b.isEnabled() }, accent, joinsOf (b));
        return;
    }
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool l = b.isConnectedOnLeft(), rt = b.isConnectedOnRight();
    const float corner = ctlCorner (r.getHeight());
    const auto style = Theme::style().buttons;

    Path p;
    p.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), corner, corner, ! l, ! rt, ! l, ! rt);

    // soft: tinted fill when on; solid: filled with the accent when on; outline: lines only
    Colour fill = bg;
    if (on)
    {
        if (style == Theme::Buttons::solid)        fill = accent.withAlpha (isMouseDown ? 0.75f : isMouseOver ? 0.9f : 1.0f);
        else if (style == Theme::Buttons::outline) fill = accent.withAlpha (isMouseDown ? 0.2f : isMouseOver ? 0.14f : 0.08f);
        else                                       fill = accent.withAlpha (isMouseDown ? 0.32f : isMouseOver ? 0.27f : 0.22f);
    }
    else if (isMouseDown)  fill = panelEdge.brighter (0.25f);
    else if (isMouseOver)  fill = panelEdge.brighter (0.05f);
    if (! b.isEnabled())
        fill = fill.withMultipliedAlpha (0.5f);
    g.setColour (fill);
    g.fillPath (p);

    g.setColour (on ? accent.withAlpha (0.9f) : isMouseOver ? panelEdge.brighter (0.45f) : panelEdge.brighter (0.15f));
    g.strokePath (p, PathStrokeType (style == Theme::Buttons::outline && on ? 1.6f : 1.0f));
}

Font ScopeLookAndFeel::getTextButtonFont (TextButton&, int buttonHeight)
{
    return Theme::font (jlimit (10.0f, 13.0f, (float) buttonHeight * 0.5f), true);
}

void ScopeLookAndFeel::drawButtonText (Graphics& g, TextButton& b, bool isMouseOver, bool isMouseDown)
{
    const bool on = b.getToggleState();
    if (! Surface::plain())
    {
        const Surface::State st { isMouseOver, isMouseDown, on, b.isEnabled() };
        const auto area = Surface::labelArea (b.getLocalBounds().toFloat(), st, joinsOf (b)).reduced (5.0f, 0.0f);
        Surface::label (g, b.getButtonText(), area, getTextButtonFont (b, b.getHeight()), st, buttonAccent (b));
        return;
    }
    const auto style = Theme::style().buttons;
    auto colour = isMouseOver ? Palette::text : Palette::text.withAlpha (0.85f);
    if (on)
        colour = style == Theme::Buttons::solid   ? Palette::bg
               : style == Theme::Buttons::outline ? buttonAccent (b)
                                                  : b.findColour (TextButton::textColourOnId);
    if (! b.isEnabled())
        colour = Palette::dim.withAlpha (0.6f);
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (colour);
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (6, 2), Justification::centred, 1, 0.8f);
}

void ScopeLookAndFeel::drawToggleButton (Graphics& g, ToggleButton& b, bool isMouseOver, bool isMouseDown)
{
    using namespace Palette;
    const auto accent = accentFor (b);
    const bool on = b.getToggleState(), enabled = b.isEnabled();
    const float size = jmin (16.0f, (float) b.getHeight() - 4.0f);
    auto box = Rectangle<float> (2.0f, ((float) b.getHeight() - size) * 0.5f, size, size);

    if (! Surface::plain())
    {
        // a well in the material; switched on, it holds a bead of the accent colour with a tick
        const float corner = ctlCorner (size, 4.0f, 0.5f);
        Surface::sunken (g, box, corner, isMouseOver && enabled, accent);
        if (on)
        {
            const auto bead = box.reduced (2.5f);
            g.setColour (accent.withAlpha (enabled ? (isMouseDown ? 0.7f : 1.0f) : 0.4f));
            g.fillRoundedRectangle (bead, jmax (0.0f, corner - 2.5f));
            Path tick;
            tick.startNewSubPath (bead.getX() + bead.getWidth() * 0.22f, bead.getCentreY());
            tick.lineTo (bead.getX() + bead.getWidth() * 0.43f, bead.getY() + bead.getHeight() * 0.72f);
            tick.lineTo (bead.getX() + bead.getWidth() * 0.8f, bead.getY() + bead.getHeight() * 0.28f);
            g.setColour (Theme::style().light ? panel : bg);
            g.strokePath (tick, PathStrokeType (1.8f, PathStrokeType::curved, PathStrokeType::rounded));
        }
        g.setColour (enabled ? (on || isMouseOver ? text : text.withAlpha (0.8f)) : dim.withAlpha (0.6f));
        g.setFont (Theme::font (12.0f, on));
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft ((int) box.getRight() + 7).withTrimmedRight (2),
                          Justification::centredLeft, 1, 0.8f);
        return;
    }

    g.setColour (on ? accent.withAlpha (enabled ? (isMouseDown ? 0.75f : 0.9f) : 0.35f)
                    : (isMouseOver ? panelEdge.brighter (0.1f) : bg));
    const float corner = ctlCorner (size, 4.0f, 0.5f);
    g.fillRoundedRectangle (box, corner);
    g.setColour (on ? accent : (isMouseOver ? panelEdge.brighter (0.6f) : panelEdge.brighter (0.3f)));
    g.drawRoundedRectangle (box.reduced (0.5f), corner, 1.0f);

    if (on)
    {
        Path tick;
        tick.startNewSubPath (box.getX() + size * 0.25f, box.getCentreY());
        tick.lineTo (box.getX() + size * 0.43f, box.getY() + size * 0.7f);
        tick.lineTo (box.getX() + size * 0.76f, box.getY() + size * 0.3f);
        g.setColour (bg);
        g.strokePath (tick, PathStrokeType (2.0f, PathStrokeType::curved, PathStrokeType::rounded));
    }

    g.setColour (enabled ? (on || isMouseOver ? text : text.withAlpha (0.8f)) : dim.withAlpha (0.6f));
    g.setFont (Theme::font (12.0f, on));
    g.drawFittedText (b.getButtonText(),
                      b.getLocalBounds().withTrimmedLeft ((int) box.getRight() + 7).withTrimmedRight (2),
                      Justification::centredLeft, 1, 0.8f);
}

//------------------------------------------------------------------------------
void ScopeLookAndFeel::drawComboBox (Graphics& g, int width, int height, bool isButtonDown,
                                     int, int, int, int, ComboBox& box)
{
    using namespace Palette;
    const auto accent = box.isEnabled() ? accentFor (box) : dim;
    const bool hot = box.isEnabled() && (box.isMouseOver (true) || box.hasKeyboardFocus (true));
    const auto r = Rectangle<int> (width, height).toFloat().reduced (0.5f);

    const float corner = ctlCorner (r.getHeight(), 5.0f, 0.5f);
    if (! Surface::plain())
    {
        Surface::sunken (g, Rectangle<int> (width, height).toFloat(), corner, hot || isButtonDown, accent);
    }
    else
    {
        g.setColour (isButtonDown ? panelEdge : (hot ? field : bg));
        g.fillRoundedRectangle (r, corner);
        g.setColour (hot ? accent.withAlpha (0.7f) : panelEdge.brighter (0.15f));
        g.drawRoundedRectangle (r, corner, 1.0f);
    }

    // chevron in the module colour
    const float cx = (float) width - 14.0f, cy = (float) height * 0.5f;
    Path arrow;
    arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
    arrow.lineTo (cx, cy + 2.0f);
    arrow.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (accent);
    g.strokePath (arrow, PathStrokeType (1.6f, PathStrokeType::curved, PathStrokeType::rounded));
}

Font ScopeLookAndFeel::getComboBoxFont (ComboBox& box)
{
    return Theme::font (jlimit (11.0f, 13.0f, (float) box.getHeight() * 0.52f));
}

void ScopeLookAndFeel::positionComboBoxText (ComboBox& box, Label& label)
{
    label.setBounds (4, 1, box.getWidth() - 26, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
    label.setMinimumHorizontalScale (0.75f);
}

Font ScopeLookAndFeel::getPopupMenuFont()
{
    return Theme::font (13.0f);
}

void ScopeLookAndFeel::preparePopupMenuWindow (Component& menuWindow)
{
    // the menu window is on the desktop but not painted yet (UI/StreamSafe.h)
    streamsafe::useSoftwareRenderer (menuWindow);
}

void ScopeLookAndFeel::drawPopupMenuBackground (Graphics& g, int width, int height)
{
    g.fillAll (findColour (PopupMenu::backgroundColourId));
    g.setColour (Palette::panelEdge.brighter (0.25f));
    g.drawRect (0, 0, width, height, 1);
}

void ScopeLookAndFeel::drawPopupMenuItemWithOptions (Graphics& g, const Rectangle<int>& area, bool isHighlighted,
                                                     const PopupMenu::Item& item, const PopupMenu::Options& options)
{
    using namespace Palette;
    const auto accent = options.getTargetComponent() != nullptr ? accentFor (*options.getTargetComponent()) : green;

    if (item.isSeparator)
    {
        g.setColour (panelEdge.brighter (0.2f));
        g.fillRect (area.withSizeKeepingCentre (area.getWidth() - 16, 1));
        return;
    }

    auto r = area.reduced (3, 1);
    const bool active = item.isEnabled;
    if (isHighlighted && active)
    {
        g.setColour (accent.withAlpha (0.2f));
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (r.toFloat().withWidth (3.0f), 1.5f);
    }

    auto textColour = item.colour != Colour() ? item.colour : text;
    if (! active)
        textColour = dim.withAlpha (0.6f);
    else if (isHighlighted)
        textColour = bright;

    r.removeFromLeft (8);
    auto iconArea = r.removeFromLeft (14).toFloat();
    if (item.image != nullptr)
        item.image->drawWithin (g, iconArea, RectanglePlacement::centred | RectanglePlacement::onlyReduceInSize, 1.0f);
    else if (item.isTicked)
    {
        g.setColour (accent);
        g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre (iconArea.getCentre()));
    }
    r.removeFromLeft (4);

    const bool hasSubMenu = item.subMenu != nullptr && (item.itemID == 0 || item.subMenu->getNumItems() > 0);
    if (hasSubMenu)
    {
        auto a = r.removeFromRight (14).toFloat();
        Path p;
        p.startNewSubPath (a.getCentreX() - 2.0f, a.getCentreY() - 4.0f);
        p.lineTo (a.getCentreX() + 2.0f, a.getCentreY());
        p.lineTo (a.getCentreX() - 2.0f, a.getCentreY() + 4.0f);
        g.setColour (isHighlighted ? accent : dim);
        g.strokePath (p, PathStrokeType (1.5f, PathStrokeType::curved, PathStrokeType::rounded));
    }

    g.setFont (Theme::font (13.0f, item.isTicked));
    g.setColour (item.isTicked && ! isHighlighted && active ? accent.interpolatedWith (bright, 0.35f) : textColour);
    g.drawFittedText (item.text, r.withTrimmedRight (4), Justification::centredLeft, 1, 0.85f);

    if (item.shortcutKeyDescription.isNotEmpty())
    {
        g.setFont (Theme::font (11.0f));
        g.setColour (dim);
        g.drawText (item.shortcutKeyDescription, r.withTrimmedRight (4), Justification::centredRight, true);
    }
}

void ScopeLookAndFeel::drawPopupMenuSectionHeader (Graphics& g, const Rectangle<int>& area, const String& name)
{
    g.setFont (Theme::font (11.0f, true));
    g.setColour (Palette::dim);
    g.drawFittedText (name.toUpperCase(), area.reduced (12, 0).withTrimmedTop (4), Justification::centredLeft, 1);
}

void ScopeLookAndFeel::getIdealPopupMenuItemSize (const String& text, bool isSeparator, int standardMenuItemHeight,
                                                  int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = 50;
        idealHeight = 9;
        return;
    }
    idealHeight = jmax (standardMenuItemHeight, 24);
    idealWidth = GlyphArrangement::getStringWidthInt (Theme::font (13.0f, true), text) + 56;
}

//------------------------------------------------------------------------------
void ScopeLookAndFeel::fillTextEditorBackground (Graphics& g, int width, int height, TextEditor& te)
{
    if (! Surface::plain())
    {
        Surface::sunken (g, Rectangle<int> (width, height).toFloat(), ctlCorner ((float) height, 4.0f), false, accentFor (te));
        return;
    }
    g.setColour (te.findColour (TextEditor::backgroundColourId));
    g.fillRoundedRectangle (Rectangle<int> (width, height).toFloat(), ctlCorner ((float) height, 4.0f));
}

void ScopeLookAndFeel::drawTextEditorOutline (Graphics& g, int width, int height, TextEditor& te)
{
    if (! te.isEnabled())
        return;
    const auto r = Rectangle<int> (width, height).toFloat().reduced (0.5f);
    if (! Surface::plain())
    {
        // the well has its own edges; focus adds the accent ring
        if (te.hasKeyboardFocus (true) && ! te.isReadOnly())
        {
            g.setColour (accentFor (te));
            g.drawRoundedRectangle (r, ctlCorner ((float) height, 4.0f), 1.5f);
        }
        return;
    }
    if (te.hasKeyboardFocus (true) && ! te.isReadOnly())
    {
        g.setColour (accentFor (te));
        g.drawRoundedRectangle (r, ctlCorner ((float) height, 4.0f), 1.5f);
    }
    else
    {
        g.setColour (te.findColour (TextEditor::outlineColourId));
        g.drawRoundedRectangle (r, ctlCorner ((float) height, 4.0f), 1.0f);
    }
}

void ScopeLookAndFeel::drawScrollbar (Graphics& g, ScrollBar& bar, int x, int y, int width, int height,
                                      bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                                      bool isMouseOver, bool isMouseDown)
{
    Rectangle<float> thumb;
    if (isScrollbarVertical)
        thumb = Rectangle<int> (x, thumbStartPosition, width, thumbSize).toFloat();
    else
        thumb = Rectangle<int> (thumbStartPosition, y, thumbSize, height).toFloat();

    const float thick = (isMouseOver || isMouseDown) ? 6.0f : 4.0f;
    thumb = isScrollbarVertical ? thumb.withSizeKeepingCentre (thick, thumb.getHeight() - 2.0f)
                                : thumb.withSizeKeepingCentre (thumb.getWidth() - 2.0f, thick);
    auto c = bar.findColour (ScrollBar::thumbColourId);
    g.setColour (isMouseDown ? c.brighter (0.5f) : isMouseOver ? c.brighter (0.25f) : c);
    g.fillRoundedRectangle (thumb, thick * 0.5f);
}

TextLayout ScopeLookAndFeel::tooltipLayout (const String& text)
{
    AttributedString s;
    s.setJustification (Justification::centredLeft);
    s.append (text, Theme::font (12.5f), Palette::text);
    TextLayout tl;
    tl.createLayoutWithBalancedLineLengths (s, 340.0f);
    return tl;
}

Rectangle<int> ScopeLookAndFeel::getTooltipBounds (const String& tipText, Point<int> screenPos, Rectangle<int> parentArea)
{
    const auto tl = tooltipLayout (tipText);
    const int w = (int) (tl.getWidth() + 22.0f), h = (int) (tl.getHeight() + 14.0f);
    return Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                           screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
               .constrainedWithin (parentArea);
}

void ScopeLookAndFeel::drawTooltip (Graphics& g, const String& text, int width, int height)
{
    const auto r = Rectangle<int> (width, height).toFloat();
    g.setColour (findColour (TooltipWindow::backgroundColourId));
    const float corner = ctlCorner (r.getHeight(), 5.0f, 0.5f);
    g.fillRoundedRectangle (r, corner);
    g.setColour (findColour (TooltipWindow::outlineColourId));
    g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
    tooltipLayout (text).draw (g, r.reduced (11.0f, 7.0f));
}

//==============================================================================
// Chain model
//==============================================================================
namespace chain
{
    namespace
    {
        const int fxPages[] = { amp, cab, eq, comp, mod, delay, reverb, limiter };
        const FxRack::Module fxModules[] = { FxRack::amp, FxRack::cab, FxRack::eq, FxRack::comp,
                                             FxRack::mod, FxRack::delay, FxRack::reverb, FxRack::limiter };

        std::vector<Info> buildTable()
        {
            std::vector<Info> t ((size_t) numPages);
            auto set = [&t] (int page, const char* key, const char* name, const char* shortName, const juce::String& title,
                             const juce::String& desc, const juce::String& enableId, juce::Colour accent, int group)
            {
                t[(size_t) page] = { key, name, shortName, title, desc, enableId, accent, group };
            };

            set (input, "input", "INPUT", "IN", "Input",
                 "Gain, gate, drive, bit crush and filters for the incoming audio - the first stage of the chain.",
                 {}, Accent::input, 0);
            set (synth, "synth", "SYNTH", "SYN", "Synth layer",
                 "Turns any input into a synth: follows its pitch and plays the sculpted wave. MIDI notes play it too.",
                 "synOn", Accent::synth, 0);
            set (sculpt, "sculpt", "SCULPT", "SCULPT", "Wave Sculptor",
                 "The wave the synth plays: drag the wave, set its overtones, morph between two shapes.",
                 {}, Accent::sculpt, 0);
            set (equation, "equation", "EQUATION", "EQN", "Wave equation",
                 "Reshapes every sample with a formula y = f(x, ...) - presets or your own.",
                 "eqOn", Accent::physics, -1);

            const char* fxKeys[]  = { "amp", "cab", "eq", "comp", "mod", "delay", "reverb", "limiter" };
            const char* fxNames[] = { "AMP", "CAB", "EQ", "COMP", "MOD", "DELAY", "REVERB", "LIMITER" };
            const char* fxShort[] = { "AMP", "CAB", "EQ", "COMP", "MOD", "DLY", "VERB", "LIM" };
            for (int i = 0; i < FxRack::numModules; ++i)
            {
                const auto m = fxModules[i];
                set (fxPages[i], fxKeys[i], fxNames[i], fxShort[i], FxRack::moduleName (m), FxRack::moduleSummary (m),
                     FxRack::moduleEnableId (m), Accent::fx, fxPages[i] == limiter ? 2 : 1);
            }

            set (slit, "slit", "SLIT", "SLIT", "Double slit",
                 "Two slits and a huge ear: delayed copies interfere into comb filters and smeared tails.",
                 "dsOn", Accent::physics, -1);
            set (output, "output", "OUTPUT", "OUT", "Output",
                 "Global dry/wet mix and output level, plus the whole chain at a glance.",
                 {}, Accent::master, 2);

            set (viewScope, "view.scope", "VIEW", "VIEW", "2D Scope display",
                 "Picture only: time window, zoom, trigger and spectrum of the 2D scope.", {}, Accent::view, -1);
            set (view3D, "view.3d", "VIEW", "VIEW", "3D Views display",
                 "Picture only: which 3D view is drawn and how sound is mapped to x, y, z and colour.", {}, Accent::view, -1);
            set (viewOrbital, "view.orbital", "VIEW", "VIEW", "Orbital display",
                 "Picture only: how the sound is turned into a quantum orbital and drawn as a cloud.", {}, Accent::physics, -1);
            set (viewJam, "view.jam", "VIEW", "VIEW", "Jam Sesh",
                 "Play together over the internet: host or join a sesh, hear your friends and see their views.",
                 {}, Accent::jam, -1);
            return t;
        }
    }

    const Info& info (int page)
    {
        static std::vector<Info> table = buildTable();
        static int generation = Theme::generation;
        if (generation != Theme::generation)
        {
            // the accents come from the palette: refresh them in place (callers may hold references)
            generation = Theme::generation;
            const auto fresh = buildTable();
            for (size_t i = 0; i < table.size(); ++i)
                table[i].accent = fresh[i].accent;
        }
        return table[(size_t) jlimit (0, (int) numPages - 1, page)];
    }

    int pageForKey (const juce::String& key)
    {
        for (int i = 0; i < numPages; ++i)
            if (info (i).key == key)
                return i;
        return -1;
    }

    bool isViewPage (int page) { return page >= viewScope && page < numPages; }

    int fxModule (int page)
    {
        for (int i = 0; i < FxRack::numModules; ++i)
            if (fxPages[i] == page)
                return (int) fxModules[i];
        return -1;
    }

    juce::String groupName (int group)
    {
        switch (group)
        {
            case 0:  return "SOURCE";
            case 1:  return "FX RACK";
            case 2:  return "MASTER";
            default: return {};
        }
    }

    bool isActive (AudioProcessorValueTreeState& s, int page)
    {
        auto on = [&s] (const juce::String& id)
        {
            auto* v = s.getRawParameterValue (id);
            return v != nullptr && v->load() > 0.5f;
        };
        if (page == sculpt)
            return on ("synOn");
        const auto& id = info (page).enableId;
        return id.isEmpty() || on (id);
    }

    HelpList helpEntries()
    {
        HelpList h;
        for (int p = 0; p < numChain; ++p)
        {
            const auto& i = info (p);
            juce::String mapping;
            mapping << "Position in the chain: ";
            if (p == input)            mapping << "first - it receives the plugin's audio input";
            else if (p == sculpt)      mapping << "inside the synth layer - it is the wave the synth plays (heard only while SYNTH is on)";
            else if (p == output)      mapping << "last - after " << info (p - 1).name;
            else                       mapping << "after " << info (p == equation ? synth : p - 1).name << ", before " << info (p + 1).name;
            mapping << ".\nClick the chip: opens its controls in the right column.";
            if (i.enableId.isNotEmpty())
                mapping << "\nLED on the chip: switches the module on or off (parameter " << i.enableId
                        << "). A dim chip = the module is off and passes audio unchanged.";
            else if (p == sculpt)
                mapping << "\nSelecting it also switches the main view to the Wave Sculptor.";
            mapping << "\nThe small line under the name shows the module's live state.";
            h.push_back ({ "chain." + i.key, i.title + " (signal chain)", i.description, mapping,
                           p >= amp && p <= limiter ? "Effects that are off cost no CPU. The strip runs in processing order, left to right."
                                                    : "" });
        }
        h.push_back ({ "chain.view", "Display panel",
                       "Opens the controls of the current main view (2D Scope, 3D Views, Orbital or Wave Sculptor).",
                       "Display controls only change the picture, never the sound (the Wave Sculptor is the exception: "
                       "it edits the synth's wave). Switch the main view with the buttons at the top.",
                       "" });
        return h;
    }
}

//==============================================================================
// Chain strip
//==============================================================================
namespace
{
    // Small round LED that switches a module on/off
    class ChipLed : public Button
    {
    public:
        explicit ChipLed (Colour c) : Button ("on/off"), accent (c) { setClickingTogglesState (true); }

        void paintButton (Graphics& g, bool isOver, bool) override
        {
            accent = Theme::follow (accent);
            const auto r = getLocalBounds().toFloat().withSizeKeepingCentre (10.0f, 10.0f);
            if (getToggleState())
            {
                g.setColour (accent.withAlpha (0.3f));
                g.fillEllipse (r.expanded (3.0f));
                g.setColour (accent);
                g.fillEllipse (r);
                g.setColour (Colours::white.withAlpha (0.5f));
                g.fillEllipse (r.reduced (3.0f).translated (-1.0f, -1.0f));
            }
            else
            {
                g.setColour (Palette::viewBg);
                g.fillEllipse (r);
                g.setColour (isOver ? accent : Palette::dim.withAlpha (0.8f));
                g.drawEllipse (r.reduced (0.5f), 1.2f);
            }
        }

        Colour accent;
    };
}

class ChainStrip::Chip : public Component, public SettableTooltipClient
{
public:
    Chip (AudioProcessorValueTreeState& s, int pageIndex, HelpTarget* help)
        : page (pageIndex),
          hover (*this, page < chain::numChain ? "chain." + chain::info (page).key : juce::String ("chain.view"), help)
    {
        const auto& i = chain::info (page < chain::numChain ? page : (int) chain::viewScope);
        accent = i.accent;
        name = i.name;
        shortName = i.shortName;
        setTooltip (page < chain::numChain ? i.description + " (click to edit)"
                                           : juce::String ("Controls of the current main view (picture only)"));
        setMouseCursor (MouseCursor::PointingHandCursor);

        if (page < chain::numChain && i.enableId.isNotEmpty())
            if (auto* p = s.getParameter (i.enableId))
            {
                led = std::make_unique<ChipLed> (accent);
                led->setTooltip ("Switch " + i.title + " on or off");
                addAndMakeVisible (*led);
                attachment = std::make_unique<ButtonParameterAttachment> (*p, *led);
            }
    }

    int naturalWidth (bool shortNames) const
    {
        // the status line changes all the time, so a fixed room is reserved for it (the strip never jumps)
        const int label = GlyphArrangement::getStringWidthInt (Theme::font (11.5f, true), shortNames ? shortName : name);
        return jmax (label, 44) + (led != nullptr ? 22 : 10) + 10;
    }

    void setShort (bool s) { if (s != useShort) { useShort = s; repaint(); } }

    void update (const juce::String& newStatus, bool isOn)
    {
        if (newStatus != status || isOn != active)
        {
            status = newStatus;
            active = isOn;
            repaint();
        }
    }

    void setSelected (bool s) { if (s != selected) { selected = s; repaint(); } }
    void setName (const juce::String& n) { if (n != name) { name = n; shortName = n; repaint(); } }

    void resized() override
    {
        if (led != nullptr)
            led->setBounds (3, 4 + ((getHeight() - 8) / 2 + 1 - 18) / 2, 18, 18);
    }

    void paint (Graphics& g) override
    {
        using namespace Palette;
        accent = Theme::follow (accent);
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool over = isMouseOver (true);
        const float corner = ctlCorner (r.getHeight(), 6.0f, 0.5f);

        if (! Surface::plain())
        {
            paintInMaterial (g, over, corner);
            return;
        }

        g.setColour (selected ? accent.withAlpha (0.2f) : (over ? Palette::hover : panel));
        g.fillRoundedRectangle (r, corner);
        g.setColour (selected ? accent : (over ? accent.withAlpha (0.55f) : panelEdge.brighter (active ? 0.15f : 0.0f)));
        g.drawRoundedRectangle (r, corner, selected ? 1.5f : 1.0f);
        if (selected)
        {
            g.setColour (accent);
            g.fillRoundedRectangle (r.getX() + 8.0f, r.getBottom() - 3.0f, r.getWidth() - 16.0f, 3.0f, 1.5f);
        }

        // an accent tab on the left edge identifies the module family even when the chip is dim
        g.setColour (accent.withAlpha (active ? 0.9f : 0.3f));
        g.fillRoundedRectangle (r.getX() + 1.0f, r.getY() + 7.0f, 2.0f, r.getHeight() - 14.0f, 1.0f);

        auto inner = getLocalBounds().reduced (6, 4);
        if (led != nullptr)
            inner.removeFromLeft (16);
        auto top = inner.removeFromTop (inner.getHeight() / 2 + 1);
        const auto just = led != nullptr ? Justification::centredLeft : Justification::centred;

        g.setColour (! active ? dim : (selected ? bright : text));
        g.setFont (Theme::font (11.5f, true));
        g.drawFittedText (useShort ? shortName : name, top, just, 1, 0.8f);

        g.setFont (Theme::font (10.0f));
        g.setColour (active ? accent.withAlpha (0.85f) : dim.withAlpha (0.7f));
        g.drawFittedText (status, inner, just, 1, 0.7f);
    }

    void mouseEnter (const MouseEvent&) override { repaint(); }
    void mouseExit (const MouseEvent&) override  { repaint(); }

    void mouseUp (const MouseEvent& e) override
    {
        if (e.mouseWasClicked() && onClick)
            onClick();
    }

    std::function<void()> onClick;
    const int page;

private:
    // A key in the theme's material; selected = the material's "on" (pressed in, lit, inverted...)
    void paintInMaterial (Graphics& g, bool over, float corner)
    {
        using namespace Palette;
        const Surface::State st { over, false, selected, true };
        const auto bounds = getLocalBounds().toFloat();
        Surface::raised (g, bounds, corner, st, accent);
        const auto face = Surface::labelArea (bounds, st);
        const auto ink = Surface::ink (st, accent);
        const bool inverse = selected && ink == bg;   // a solid selection: everything on it in the ink colour

        // the family mark on the left edge (not in the terminal: its brackets frame the chip)
        if (Theme::style().material != Theme::Material::terminal)
        {
            g.setColour ((inverse ? ink : accent).withAlpha (active ? 0.9f : 0.3f));
            g.fillRoundedRectangle (face.getX() + 1.5f, face.getY() + 7.0f, 2.0f, jmax (0.0f, face.getHeight() - 14.0f), 1.0f);
        }

        auto inner = face.toNearestInt().reduced (5, 3);
        if (led != nullptr)
            inner.removeFromLeft (16);
        auto top = inner.removeFromTop (inner.getHeight() / 2 + 1);
        const auto just = led != nullptr ? Justification::centredLeft : Justification::centred;
        const auto nameFont = Theme::font (11.5f, true);
        // an off module's chip keeps its name readable on bright faces (chrome, a solid selection)
        const auto offInk = inverse ? ink.withAlpha (0.6f)
                          : Theme::style().material == Theme::Material::chrome ? Colours::white.withAlpha (0.55f) : dim;
        if (active)
            Surface::label (g, useShort ? shortName : name, top.toFloat(), nameFont, st, accent, just);
        else
        {
            g.setFont (nameFont);
            g.setColour (offInk);
            g.drawFittedText (useShort ? shortName : name, top, just, 1, 0.8f);
        }

        g.setFont (Theme::font (10.0f));
        g.setColour (! active ? offInk.withMultipliedAlpha (0.8f) : inverse ? ink.withAlpha (0.8f) : accent.withAlpha (0.85f));
        g.drawFittedText (status, inner, just, 1, 0.7f);
    }

    widgets_detail::HoverReporter hover;
    std::unique_ptr<ChipLed> led;
    std::unique_ptr<ButtonParameterAttachment> attachment;
    Colour accent;
    juce::String name, shortName, status;
    bool selected = false, active = true, useShort = false;
};

ChainStrip::ChainStrip (AudioProcessorValueTreeState& s, HelpTarget* help) : state (s)
{
    for (int p = 0; p <= chain::numChain; ++p)   // the extra one is the VIEW chip
    {
        auto* c = chips.add (new Chip (s, p, help));
        c->onClick = [this, p] { if (onSelect) onSelect (p < chain::numChain ? p : (int) chain::numPages); };
        addAndMakeVisible (c);
    }
}

ChainStrip::~ChainStrip() = default;

void ChainStrip::setSelected (int page, bool viewChipSelected)
{
    selected = page;
    for (auto* c : chips)
        c->setSelected (c->page < chain::numChain ? (! viewChipSelected && c->page == page) : viewChipSelected);
}

void ChainStrip::setViewName (const juce::String& n)
{
    chips.getLast()->setName (n);
    resized();
}

void ChainStrip::refresh()
{
    for (auto* c : chips)
    {
        const juce::String st = statusText ? statusText (c->page) : juce::String();
        c->update (st, c->page < chain::numChain ? chain::isActive (state, c->page) : true);
    }
}

void ChainStrip::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (16);                // group brackets
    r.removeFromBottom (4);
    const int n = chips.size();
    const int arrowW = 11, viewGap = 22;
    const int avail = r.getWidth() - arrowW * (chain::numChain - 1) - viewGap;

    int naturalSum = 0;
    for (auto* c : chips)
        naturalSum += c->naturalWidth (false);
    const bool useShort = naturalSum > avail;

    std::vector<int> widths;
    int sum = 0;
    for (auto* c : chips)
    {
        c->setShort (useShort);
        widths.push_back (c->naturalWidth (useShort));
        sum += widths.back();
    }

    // share out the spare width evenly (capped), or squeeze proportionally if it still does not fit
    const int spare = avail - sum;
    if (spare >= 0)
    {
        const int extra = jmin (40, spare / n);
        for (auto& w : widths)
            w += extra;
    }
    else
    {
        for (auto& w : widths)
            w = jmax (30, w * avail / jmax (1, sum));
    }

    int total = arrowW * (chain::numChain - 1) + viewGap;
    for (auto w : widths)
        total += w;
    int x = r.getX() + jmax (0, (r.getWidth() - total) / 2);

    connectors.clear();
    for (int i = 0; i < n; ++i)
    {
        if (i == n - 1)
            x += viewGap - arrowW;
        chips[i]->setBounds (x, r.getY(), widths[(size_t) i], r.getHeight());
        x += widths[(size_t) i];
        if (i < n - 2)
            connectors.push_back ({ x, r.getY(), arrowW, r.getHeight() });
        x += arrowW;
    }
    repaint();
}

std::vector<ChainStrip::Group> ChainStrip::groups() const
{
    std::vector<Group> out;
    const bool trays = Surface::hasTrays();
    const auto font = Theme::font (9.5f, true);
    for (int group = 0; group < 4; ++group)   // SOURCE, FX RACK, MASTER, DISPLAY (the VIEW chip)
    {
        int x0 = std::numeric_limits<int>::max(), x1 = std::numeric_limits<int>::min(), y0 = 0, y1 = 0, first = -1;
        for (auto* c : chips)
        {
            const bool member = group == 3 ? c->page >= chain::numChain
                                           : (c->page < chain::numChain && chain::info (c->page).group == group);
            if (! member)
                continue;
            if (first < 0)
                first = c->page;
            x0 = jmin (x0, c->getX());
            x1 = jmax (x1, c->getRight());
            y0 = c->getY();
            y1 = c->getBottom();
        }
        if (x0 > x1)
            continue;

        Group gr;
        gr.label = group == 3 ? juce::String ("DISPLAY") : chain::groupName (group);
        gr.about = group == 0 ? "where the sound comes from: your input, the synth layer and its wave"
                 : group == 1 ? "the effects, in the order the sound goes through them"
                 : group == 2 ? "the end of the chain: limiter and output level"
                              : "the controls of the main view (picture only)";
        gr.accent = group == 0 ? Accent::input : group == 1 ? Accent::fx : group == 2 ? Accent::master : Accent::view;
        gr.firstPage = group == 3 ? (int) chain::numPages : first;
        const float tw = (float) GlyphArrangement::getStringWidthInt (font, gr.label) + (trays ? 18.0f : 12.0f);
        const float mid = (float) (x0 + x1) * 0.5f;
        if (trays)
        {
            gr.body = { (float) x0 - 5.0f, (float) y0 - 3.0f, (float) (x1 - x0) + 10.0f, (float) (y1 - y0) + 5.0f };
            gr.tab = { mid - tw * 0.5f, 0.5f, tw, gr.body.getY() - 0.5f };
        }
        else
        {
            gr.body = Rectangle<int> (x0, y0, x1 - x0, y1 - y0).toFloat();
            gr.tab = { mid - tw * 0.5f, 0.0f, tw, 14.0f };
        }
        out.push_back (gr);
    }
    return out;
}

int ChainStrip::groupAt (Point<float> p) const
{
    const auto gs = groups();
    for (size_t i = 0; i < gs.size(); ++i)
        if (gs[i].tab.expanded (2.0f, 1.0f).contains (p))
            return (int) i;
    return -1;
}

void ChainStrip::mouseMove (const MouseEvent& e)
{
    const int i = groupAt (e.position);
    if (i != hoverGroup)
    {
        hoverGroup = i;
        setMouseCursor (i >= 0 ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
        repaint();
    }
}

void ChainStrip::mouseExit (const MouseEvent&)
{
    if (hoverGroup >= 0)
    {
        hoverGroup = -1;
        setMouseCursor (MouseCursor::NormalCursor);
        repaint();
    }
}

void ChainStrip::mouseUp (const MouseEvent& e)
{
    const int i = groupAt (e.position);
    if (i >= 0 && e.mouseWasClicked() && onSelect)
        onSelect (groups()[(size_t) i].firstPage);
}

juce::String ChainStrip::getTooltip()
{
    const auto gs = groups();
    if (hoverGroup < 0 || hoverGroup >= (int) gs.size())
        return {};
    const auto& gr = gs[(size_t) hoverGroup];
    return gr.label + ": " + gr.about + " (click to open "
         + (gr.firstPage >= chain::numPages ? juce::String ("them") : chain::info (gr.firstPage).name) + ")";
}

void ChainStrip::paint (Graphics& g)
{
    if (Surface::plain())
    {
        paintPlain (g);
        return;
    }

    using namespace Palette;
    using Theme::Material;
    const auto& st = Theme::style();
    const auto m = st.material;
    const auto groupFont = Theme::font (9.5f, true);
    const auto gs = groups();

    for (size_t i = 0; i < gs.size(); ++i)
    {
        const auto& gr = gs[i];
        const auto col = (int) i == hoverGroup ? gr.accent.interpolatedWith (bright, 0.35f) : gr.accent;
        if (Surface::hasTrays())
        {
            // the group's chips sit on one slab; its label is a tab moulded onto the slab
            Surface::tray (g, Surface::trayShape (gr.body, gr.tab, jmin (st.radius + 2.0f, 12.0f)), gr.accent);
            g.setFont (groupFont);
            g.setColour (col);
            g.drawFittedText (gr.label, gr.tab.withTrimmedTop (1.0f).toNearestInt(), Justification::centred, 1, 0.7f);
            continue;
        }

        // brackets: glowing tubes (neon), square terminal brackets, or plain lines
        const float y = 7.0f, x0 = gr.body.getX() + 4.0f, x1 = gr.body.getRight() - 4.0f;
        Path bracket;
        bracket.startNewSubPath (x0, y + 5.0f);
        bracket.lineTo (x0, y);
        bracket.lineTo (jmax (x0, gr.tab.getX()), y);
        bracket.startNewSubPath (jmin (x1, gr.tab.getRight()), y);
        bracket.lineTo (x1, y);
        bracket.lineTo (x1, y + 5.0f);
        if (m == Material::neon)
        {
            g.setColour (col.withAlpha (0.2f));
            g.strokePath (bracket, PathStrokeType (4.0f));
        }
        g.setColour (col.withAlpha (m == Material::neon ? 0.85f : 0.5f));
        g.strokePath (bracket, PathStrokeType (1.0f));
        g.setFont (groupFont);
        g.setColour (col);
        g.drawFittedText (m == Material::terminal ? "[" + gr.label + "]" : gr.label, gr.tab.toNearestInt(), Justification::centred, 1, 0.7f);
    }

    // arrows between the chips (SYNTH -> SCULPT is a link, not a signal step: the sculptor is the synth's wave)
    for (size_t i = 0; i < connectors.size(); ++i)
    {
        const auto c = connectors[i].toFloat();
        const float cy = c.getCentreY(), x0 = c.getX() + 2.0f, x1 = c.getRight() - 2.0f;
        if ((int) i == chain::synth)
        {
            g.setColour (Accent::sculpt.withAlpha (0.7f));
            for (float x = x0 + 1.0f; x < x1; x += 3.5f)
                g.fillEllipse (x - 1.0f, cy - 1.0f, 2.0f, 2.0f);
            continue;
        }
        Path head;
        head.addTriangle (x1, cy, x1 - 4.0f, cy - 3.0f, x1 - 4.0f, cy + 3.0f);
        if (m == Material::ceramic)
        {
            // engraved into the glaze: a light line under a dark one
            g.setColour (Colours::white.withAlpha (st.light ? 0.85f : 0.06f));
            g.drawLine (x0, cy + 1.0f, x1 - 1.0f, cy + 1.0f, 1.2f);
            g.fillPath (head, AffineTransform::translation (0.0f, 1.0f));
        }
        g.setColour (m == Material::ceramic || m == Material::terminal ? dim : text.withAlpha (0.5f));
        g.drawLine (x0, cy, x1 - 1.0f, cy, 1.2f);
        g.fillPath (head);
    }

    if (! Surface::hasTrays())
    {
        // the VIEW chip is not part of the audio chain
        auto* v = chips.getLast();
        g.setColour (panelEdge);
        g.drawVerticalLine (v->getX() - 11, (float) v->getY() + 6.0f, (float) v->getBottom() - 6.0f);
    }
}

void ChainStrip::paintPlain (Graphics& g)
{
    using namespace Palette;

    // arrows between the chips (SYNTH -> SCULPT is a link, not a signal step: the sculptor is the synth's wave)
    for (size_t i = 0; i < connectors.size(); ++i)
    {
        const auto c = connectors[i].toFloat();
        const float cy = c.getCentreY(), x0 = c.getX() + 2.0f, x1 = c.getRight() - 2.0f;
        if ((int) i == chain::synth)
        {
            g.setColour (Accent::sculpt.withAlpha (0.7f));
            for (float x = x0 + 1.0f; x < x1; x += 3.5f)
                g.fillEllipse (x - 1.0f, cy - 1.0f, 2.0f, 2.0f);
            continue;
        }
        g.setColour (text.withAlpha (0.4f));
        g.drawLine (x0, cy, x1 - 1.0f, cy, 1.2f);
        Path head;
        head.addTriangle (x1, cy, x1 - 4.0f, cy - 3.0f, x1 - 4.0f, cy + 3.0f);
        g.fillPath (head);
    }

    // brackets over the module groups
    const auto groupFont = Theme::font (9.5f, true);
    g.setFont (groupFont);
    for (int group = 0; group < 3; ++group)
    {
        int x0 = std::numeric_limits<int>::max(), x1 = std::numeric_limits<int>::min();
        for (int p = 0; p < chain::numChain; ++p)
            if (chain::info (p).group == group)
            {
                x0 = jmin (x0, chips[p]->getX());
                x1 = jmax (x1, chips[p]->getRight());
            }
        if (x0 > x1)
            continue;
        const auto col = hoverGroup == group ? text : group == 1 ? Accent::fx.withAlpha (0.85f) : dim;   // labels are clickable
        const auto label = chain::groupName (group);
        const float tw = (float) GlyphArrangement::getStringWidthInt (groupFont, label) + 12.0f;
        const float y = 7.0f, mid = (float) (x0 + x1) * 0.5f;
        g.setColour (col.withAlpha (0.45f));
        g.drawLine ((float) x0 + 4.0f, y, mid - tw * 0.5f, y, 1.0f);
        g.drawLine (mid + tw * 0.5f, y, (float) x1 - 4.0f, y, 1.0f);
        g.drawLine ((float) x0 + 4.0f, y, (float) x0 + 4.0f, y + 5.0f, 1.0f);
        g.drawLine ((float) x1 - 4.0f, y, (float) x1 - 4.0f, y + 5.0f, 1.0f);
        g.setColour (col);
        g.drawText (label, Rectangle<float> (mid - tw * 0.5f, 0.0f, tw, 14.0f), Justification::centred);
    }

    // the VIEW chip is not part of the audio chain
    auto* v = chips.getLast();
    g.setColour (hoverGroup == 3 ? text : dim);
    g.drawText ("DISPLAY", Rectangle<int> (v->getX(), 0, v->getWidth(), 14), Justification::centred);
    g.setColour (panelEdge);
    g.drawVerticalLine (v->getX() - 11, (float) v->getY() + 6.0f, (float) v->getBottom() - 6.0f);
}

//==============================================================================
// Module header
//==============================================================================
ModuleHeader::ModuleHeader (AudioProcessorValueTreeState& s, HelpTarget* h) : state (s), help (h)
{
    prevButton.setTooltip ("Previous module in the chain");
    nextButton.setTooltip ("Next module in the chain");
    prevButton.setConnectedEdges (Button::ConnectedOnRight);
    nextButton.setConnectedEdges (Button::ConnectedOnLeft);
    prevButton.onClick = [this] { if (onStep) onStep (-1); };
    nextButton.onClick = [this] { if (onStep) onStep (1); };
    actionButton.onClick = [this] { if (action) action(); };
    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);
    addChildComponent (actionButton);
}

void ModuleHeader::setPage (int newPage)
{
    if (newPage == page)
        return;
    page = newPage;
    const auto& i = chain::info (page);
    Theme::setAccent (*this, i.accent);

    if (power != nullptr && help != nullptr)
        help->clearHelp();
    power.reset();
    if (i.enableId.isNotEmpty() && state.getParameter (i.enableId) != nullptr)
    {
        power = std::make_unique<ParamPowerButton> (state, i.enableId, help, i.accent);
        power->button.setTooltip ("Switch " + i.title + " on or off");
        addAndMakeVisible (*power);
    }
    prevButton.setVisible (page < chain::numChain);
    nextButton.setVisible (page < chain::numChain);
    prevButton.setEnabled (page > 0);
    nextButton.setEnabled (page < chain::numChain - 1);
    resized();
    repaint();
}

void ModuleHeader::setAction (const juce::String& text, std::function<void()> fn)
{
    action = std::move (fn);
    if (text != actionButton.getButtonText() || text.isNotEmpty() != actionButton.isVisible())
    {
        actionButton.setButtonText (text);
        actionButton.setVisible (text.isNotEmpty());
        resized();
    }
}

void ModuleHeader::resized()
{
    auto r = getLocalBounds().reduced (14, 8);
    auto top = r.removeFromTop (26);
    if (power != nullptr)
    {
        power->setBounds (top.removeFromLeft (26));
        top.removeFromLeft (8);
    }
    if (nextButton.isVisible())
    {
        nextButton.setBounds (top.removeFromRight (26).reduced (0, 1));
        prevButton.setBounds (top.removeFromRight (26).reduced (0, 1));
        top.removeFromRight (8);
    }
    if (actionButton.isVisible())
    {
        const int w = GlyphArrangement::getStringWidthInt (Theme::font (12.0f, true), actionButton.getButtonText()) + 24;
        actionButton.setBounds (top.removeFromRight (jmin (w, top.getWidth() / 2)).reduced (0, 1));
        top.removeFromRight (8);
    }
    titleArea = top;
    r.removeFromTop (4);
    descArea = r;
}

void ModuleHeader::paint (Graphics& g)
{
    using namespace Palette;
    if (page < 0)
        return;
    const auto& i = chain::info (page);
    const auto& st = Theme::style();
    const float corner = st.radius;
    if (! Surface::plain())
    {
        // a section in the material, washed with the module's colour from the left
        Surface::section (g, getLocalBounds(), {}, i.accent);
        const auto face = Surface::sectionFace (getLocalBounds());
        {
            Graphics::ScopedSaveState save (g);
            Path shape;
            shape.addRoundedRectangle (face, corner);
            g.reduceClipRegion (shape);
            g.setGradientFill (ColourGradient (i.accent.withAlpha (0.12f), face.getX(), face.getY(),
                                               i.accent.withAlpha (0.0f), face.getX() + face.getWidth() * 0.7f, face.getY(), false));
            g.fillRect (face);
        }
        g.setColour (i.accent);
        g.fillRoundedRectangle (face.getX() + 1.0f, face.getY() + 10.0f, 3.0f, face.getHeight() - 20.0f, 1.5f);
    }
    else
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour (panel);
        g.fillRoundedRectangle (r, corner);
        g.setGradientFill (ColourGradient (i.accent.withAlpha (0.12f), r.getX(), r.getY(),
                                           i.accent.withAlpha (0.0f), r.getX() + r.getWidth() * 0.7f, r.getY(), false));
        g.fillRoundedRectangle (r, corner);
        if (st.border > 0.0f)
        {
            g.setColour (st.panels == Theme::Panels::outline ? i.accent.withAlpha (0.45f) : panelEdge);
            g.drawRoundedRectangle (r.reduced (st.border * 0.5f), corner, st.border);
        }
        g.setColour (i.accent);
        g.fillRoundedRectangle (r.getX() + 1.0f, r.getY() + 10.0f, 3.0f, r.getHeight() - 20.0f, 1.5f);
    }

    const auto titleFont = Theme::font (17.0f, true);
    const auto title = i.title.toUpperCase();
    g.setColour (i.accent);
    g.setFont (titleFont);
    g.drawFittedText (title, titleArea, Justification::centredLeft, 1, 0.8f);

    // badge after the title: position in the chain and on/off state, or "picture only" for display pages
    const auto badge = titleArea.withTrimmedLeft (GlyphArrangement::getStringWidthInt (titleFont, title) + 10);
    juce::String tag;
    bool warn = false;
    if (page < chain::numChain)
    {
        const bool on = chain::isActive (state, page);
        tag = juce::String (page + 1) + " / " + juce::String ((int) chain::numChain);
        if (i.enableId.isNotEmpty())   tag << (on ? "   ON" : "   OFF - passes audio unchanged");
        else if (page == chain::sculpt) tag << (on ? "   PLAYING" : "   SYNTH IS OFF");
        warn = ! on;
    }
    else
    {
        tag = page == chain::viewJam ? "PLAY TOGETHER" : "PICTURE ONLY";
    }
    g.setFont (Theme::font (10.5f, true));
    g.setColour (warn ? amber.withAlpha (0.9f) : dim);
    g.drawFittedText (tag, badge, Justification::centredLeft, 1, 0.8f);

    g.setColour (text.withAlpha (0.8f));
    g.setFont (Theme::font (12.0f));
    g.drawFittedText (i.description, descArea, Justification::topLeft, 2, 0.9f);
}

//==============================================================================
// Split handle
//==============================================================================
SplitHandle::SplitHandle()
{
    setMouseCursor (MouseCursor::UpDownResizeCursor);
    setTooltip ("Drag to resize the Inspector - double-click to collapse or expand it");
}

void SplitHandle::paint (Graphics& g)
{
    const bool over = isMouseOverOrDragging();
    const auto c = getLocalBounds().toFloat().getCentre();
    g.setColour (over ? Palette::text.withAlpha (0.7f) : Palette::panelEdge.brighter (0.4f));
    for (int i = -2; i <= 2; ++i)
        g.fillEllipse (c.x + (float) i * 7.0f - 1.5f, c.y - 1.5f, 3.0f, 3.0f);
}

void SplitHandle::mouseDown (const MouseEvent&) {}
void SplitHandle::mouseDrag (const MouseEvent& e) { if (onDrag) onDrag (e.getDistanceFromDragStartY(), false); }
void SplitHandle::mouseUp (const MouseEvent& e)   { if (onDrag && e.mouseWasDraggedSinceMouseDown()) onDrag (e.getDistanceFromDragStartY(), true); }
void SplitHandle::mouseDoubleClick (const MouseEvent&) { if (onDoubleClick) onDoubleClick(); }

//==============================================================================
// OUTPUT panel
//==============================================================================
OutputPanel::OutputPanel (AudioProcessorValueTreeState& s, HelpTarget* help)
    : state (s), mix (s, "mix", "DRY/WET", help), outGain (s, "outGain", "OUT GAIN", help, true)
{
    Theme::setAccent (*this, Accent::master);
    addAndMakeVisible (mix);
    addAndMakeVisible (outGain);
}

int OutputPanel::idealHeight (int) const
{
    return listTop + 8 + (int) chain::numChain * rowH + 14;
}

juce::Rectangle<int> OutputPanel::rowBounds (int row) const
{
    return { 12, listTop + 8 + row * rowH, getWidth() - 24, rowH };
}

int OutputPanel::rowAt (Point<int> p) const
{
    for (int i = 0; i < chain::numChain; ++i)
        if (rowBounds (i).contains (p))
            return i;
    return -1;
}

void OutputPanel::resized()
{
    const int cell = jmin (100, (getWidth() - 28) / 4);
    mix.setBounds (14, knobTop, cell, Theme::knobCellHeight - 4);
    outGain.setBounds (14 + cell, knobTop, cell, Theme::knobCellHeight - 4);
}

void OutputPanel::paint (Graphics& g)
{
    using namespace Palette;
    const auto top = Rectangle<int> (0, 0, getWidth(), listTop - 32);
    Theme::drawSection (g, top, "OUTPUT", Accent::master);

    auto note = Rectangle<int> (outGain.getRight() + 12, knobTop, getWidth() - outGain.getRight() - 26, Theme::knobCellHeight - 8);
    g.setColour (dim);
    g.setFont (Theme::font (11.5f));
    g.drawFittedText ("Dry/Wet blends the untouched input (delay-compensated) with the processed signal. "
                      "Out gain sets the final level. Double-click a dial to reset it.",
                      note, Justification::centredLeft, 5, 0.9f);

    const auto list = Rectangle<int> (0, listTop - 22, getWidth(), getHeight() - (listTop - 22));
    Theme::drawSection (g, list, "SIGNAL CHAIN  -  click a module to edit it", Accent::master);

    for (int i = 0; i < chain::numChain; ++i)
    {
        const auto& inf = chain::info (i);
        auto r = rowBounds (i);
        const bool on = chain::isActive (state, i);
        if (i == hoverRow)
        {
            g.setColour (inf.accent.withAlpha (0.12f));
            g.fillRoundedRectangle (r.toFloat(), ctlCorner ((float) r.getHeight(), 5.0f, 0.5f));
        }
        auto dot = r.removeFromLeft (20).toFloat().withSizeKeepingCentre (8.0f, 8.0f);
        if (on)
        {
            g.setColour (inf.accent);
            g.fillEllipse (dot);
        }
        else
        {
            g.setColour (dim);
            g.drawEllipse (dot, 1.2f);
        }

        g.setFont (Theme::font (12.0f, true));
        g.setColour (on ? inf.accent : dim);
        g.drawFittedText (inf.name, r.removeFromLeft (78), Justification::centredLeft, 1, 0.8f);

        juce::String stateText;
        if (inf.enableId.isNotEmpty())  stateText = on ? "ON" : "off";
        else if (i == chain::sculpt)    stateText = on ? "playing" : "synth off";
        else                            stateText = "always";
        g.setFont (Theme::font (11.0f, true));
        g.setColour (on ? text : dim);
        g.drawFittedText (stateText, r.removeFromLeft (62), Justification::centredLeft, 1, 0.8f);

        g.setFont (Theme::font (11.0f));
        g.setColour (on ? text.withAlpha (0.7f) : dim.withAlpha (0.7f));
        g.drawFittedText (inf.description, r, Justification::centredLeft, 1, 0.85f);
    }
}

void OutputPanel::mouseMove (const MouseEvent& e)
{
    const int r = rowAt (e.getPosition());
    if (r != hoverRow)
    {
        hoverRow = r;
        setMouseCursor (r >= 0 ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
        repaint();
    }
}

void OutputPanel::mouseExit (const MouseEvent&)
{
    hoverRow = -1;
    repaint();
}

void OutputPanel::mouseUp (const MouseEvent& e)
{
    const int r = rowAt (e.getPosition());
    if (r >= 0 && e.mouseWasClicked() && onSelect)
        onSelect (r);
}
