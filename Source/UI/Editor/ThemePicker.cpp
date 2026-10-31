#include "ThemePicker.h"
#include "../Theme.h"
#include "../Themes.h"
#include "../Surface.h"

namespace
{
    // One row of the theme list: a small preview of the theme (its background, a panel, its text in
    // its own font and the module accents) next to the name
    class ThemeItem : public juce::PopupMenu::CustomComponent
    {
    public:
        explicit ThemeItem (int themeIndex) : index (themeIndex) {}

        void getIdealSize (int& w, int& h) override
        {
            w = 310;
            h = 38;
        }

        void paint (juce::Graphics& g) override
        {
            using namespace Palette;
            const auto p = Themes::preview (index);
            const auto& st = Themes::styleOf (index);
            const bool isCurrent = index == Themes::current();
            auto r = getLocalBounds().reduced (4, 2);

            if (isItemHighlighted())
            {
                g.setColour (green.withAlpha (0.2f));
                g.fillRoundedRectangle (r.toFloat(), 4.0f);
            }

            // preview card in the theme's own colours and corners
            auto card = r.removeFromLeft (118).reduced (2, 1).toFloat();
            const float rad = juce::jmin (st.radius * 0.6f, card.getHeight() * 0.5f);
            g.setColour (p.bg);
            g.fillRoundedRectangle (card, rad);
            auto inner = card.reduced (4.0f, 4.0f);
            g.setColour (p.panel);
            g.fillRoundedRectangle (inner, juce::jmax (0.0f, rad - 2.0f));
            if (st.border > 0.0f)
            {
                g.setColour (p.panelEdge);
                g.drawRoundedRectangle (inner, juce::jmax (0.0f, rad - 2.0f), 1.0f);
            }
            auto opts = juce::FontOptions (11.0f * st.fontScale).withStyle ("Bold");
            if (*st.fontName != 0)
                opts = opts.withName (st.fontName);
            g.setFont (juce::Font (opts));
            g.setColour (p.text);
            g.drawText ("Aa", inner.removeFromLeft (26.0f), juce::Justification::centred);
            const float d = 8.0f;
            for (int i = 0; i < 6; ++i)
            {
                g.setColour (p.accents[i]);
                g.fillEllipse (inner.getX() + (float) i * (d + 3.0f), inner.getCentreY() - d * 0.5f, d, d);
            }

            r.removeFromLeft (8);
            if (isCurrent)
            {
                g.setColour (green);
                g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ (float) r.getRight() - 8.0f, (float) r.getCentreY() }));
                r.removeFromRight (16);
            }
            auto nameRow = r.removeFromTop (r.getHeight() / 2 + 2);
            g.setFont (Theme::font (13.0f, isCurrent));
            g.setColour (isItemHighlighted() ? bright : (isCurrent ? green : text));
            g.drawFittedText (Themes::name (index), nameRow, juce::Justification::bottomLeft, 1, 0.85f);
            g.setFont (Theme::font (10.5f));
            g.setColour (isItemHighlighted() ? text : dim);
            g.drawFittedText (Themes::tagline (index), r, juce::Justification::topLeft, 1, 0.8f);
        }

    private:
        const int index;
    };
}

ThemePicker::ThemePicker()
{
    setTooltip ("Theme: colours and style of the whole plugin (shared by the app and every plugin instance)");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

ThemePicker::~ThemePicker() = default;

void ThemePicker::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const float rad = juce::jmin (Theme::style().controlRadius * 0.8f, r.getHeight() * 0.5f);
    const bool over = isMouseOver();
    if (Surface::plain())
    {
        g.setColour (over ? Palette::hover : Palette::panel);
        g.fillRoundedRectangle (r, rad);
        g.setColour (over ? Palette::panelEdge.brighter (0.4f) : Palette::panelEdge);
        g.drawRoundedRectangle (r, rad, 1.0f);
    }
    else
    {
        const Surface::State s { over, false, false, true };
        Surface::raised (g, getLocalBounds().toFloat(), rad, s, Palette::green);
        r = Surface::labelArea (getLocalBounds().toFloat(), s);
    }
    const float d = juce::jmin (r.getWidth(), r.getHeight()) * 0.22f;
    const juce::Colour dots[] = { Palette::green, Palette::blue, Palette::amber, Palette::pink };
    for (int i = 0; i < 4; ++i)
    {
        g.setColour (dots[i]);
        g.fillEllipse (r.getCentreX() + (i % 2 == 0 ? -d * 1.1f : d * 0.1f), r.getCentreY() + (i < 2 ? -d * 1.1f : d * 0.1f), d, d);
    }
}

void ThemePicker::mouseEnter (const juce::MouseEvent&) { repaint(); }
void ThemePicker::mouseExit (const juce::MouseEvent&)  { repaint(); }

void ThemePicker::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mouseWasClicked())
        return;

    juce::PopupMenu menu;
    menu.addSectionHeader ("Theme");
    for (int i = 0; i < Themes::count(); ++i)
        menu.addCustomItem (i + 1, std::make_unique<ThemeItem> (i), nullptr, Themes::name (i));

    juce::Component::SafePointer<ThemePicker> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (310),
                        [safe] (int result)
                        {
                            if (result > 0)
                                Themes::apply (result - 1);   // every open editor repaints (Themes::Listener)
                            if (safe != nullptr)
                                safe->repaint();
                        });
}
