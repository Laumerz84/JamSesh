#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PresetManager.h"
#include "../UI/Theme.h"
#include "../UI/Surface.h"

// Self-drawn building blocks for the preset bar, browser and dialogs. Header-only, used by
// PresetUI.cpp only. Everything paints itself so it looks the same under any LookAndFeel.
namespace presetui
{
    namespace colours
    {
        inline const juce::Colour& loaded   = Palette::green;   // the preset that is playing now
        inline const juce::Colour& select   = Palette::blue;    // keyboard / mouse selection
        inline const juce::Colour& star     = Palette::amber;   // favourites
        inline const juce::Colour& modified = Palette::amber;
    }

    //==============================================================================
    inline void drawStar (juce::Graphics& g, juce::Rectangle<float> r, bool filled, juce::Colour c)
    {
        const float outer = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
        juce::Path p;
        p.addStar (r.getCentre().translated (0.0f, outer * 0.06f), 5, outer * 0.44f, outer, 0.0f);
        g.setColour (c);
        if (filled)
            g.fillPath (p);
        else
            g.strokePath (p, juce::PathStrokeType (1.25f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    inline void drawChevron (juce::Graphics& g, juce::Rectangle<float> r, int direction, juce::Colour c, float thick = 1.7f)
    {
        // direction: -1 left, 1 right, 2 down
        const auto ctr = r.getCentre();
        const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.22f;
        juce::Path p;
        if (direction == 2)
        {
            p.startNewSubPath (ctr.x - s, ctr.y - s * 0.5f);
            p.lineTo (ctr.x, ctr.y + s * 0.5f);
            p.lineTo (ctr.x + s, ctr.y - s * 0.5f);
        }
        else
        {
            const float d = (float) direction;
            p.startNewSubPath (ctr.x - s * 0.5f * d, ctr.y - s);
            p.lineTo (ctr.x + s * 0.5f * d, ctr.y);
            p.lineTo (ctr.x - s * 0.5f * d, ctr.y + s);
        }
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    inline float textWidth (const juce::Font& f, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth (f, text);
    }

    inline float pillWidth (const juce::String& text, float fontSize = 10.5f)
    {
        return textWidth (Theme::font (fontSize, true), text) + 14.0f;
    }

    inline void drawPill (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, juce::Colour c,
                          bool on = false, float fontSize = 10.5f)
    {
        const float rad = r.getHeight() * 0.5f;
        g.setColour (c.withAlpha (on ? 0.24f : 0.08f));
        g.fillRoundedRectangle (r, rad);
        g.setColour (c.withAlpha (on ? 0.85f : 0.35f));
        g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.0f);
        g.setColour (on ? Palette::text : c.interpolatedWith (Palette::text, 0.35f));
        g.setFont (Theme::font (fontSize, true));
        g.drawText (text, r, juce::Justification::centred, false);
    }

    // Draws genre pills right-aligned (or left-aligned) inside r until they no longer fit; returns
    // the x where the drawn pills begin (right-aligned) or end (left-aligned).
    inline float drawPillRow (juce::Graphics& g, juce::Rectangle<float> r, const juce::StringArray& tags,
                              juce::Colour c, bool rightAligned)
    {
        float x = rightAligned ? r.getRight() : r.getX();
        for (int i = rightAligned ? tags.size() - 1 : 0; rightAligned ? i >= 0 : i < tags.size(); i += rightAligned ? -1 : 1)
        {
            const float w = pillWidth (tags[i]);
            if ((rightAligned ? x - w < r.getX() : x + w > r.getRight()))
                break;
            const auto pr = juce::Rectangle<float> (rightAligned ? x - w : x, r.getY(), w, r.getHeight());
            drawPill (g, pr, tags[i], c);
            x = rightAligned ? x - w - 4.0f : x + w + 4.0f;
        }
        return x;
    }

    inline void styleEditor (juce::TextEditor& e, bool multiLine = false)
    {
        e.setColour (juce::TextEditor::backgroundColourId, Palette::viewBg);
        e.setColour (juce::TextEditor::outlineColourId, Palette::panelEdge);
        e.setColour (juce::TextEditor::focusedOutlineColourId, colours::select.withAlpha (0.8f));
        e.setColour (juce::TextEditor::textColourId, Palette::text);
        e.setColour (juce::TextEditor::highlightColourId, colours::select.withAlpha (0.35f));
        e.setColour (juce::TextEditor::highlightedTextColourId, Palette::text);
        e.setColour (juce::CaretComponent::caretColourId, Palette::text);
        e.setMultiLine (multiLine, true);
        e.setReturnKeyStartsNewLine (multiLine);
        e.setScrollbarsShown (multiLine);
        e.setFont (Theme::font (13.0f));
        e.setIndents (8, multiLine ? 6 : 5);
        if (! multiLine)
            e.setJustification (juce::Justification::centredLeft);
    }

    inline void styleCombo (juce::ComboBox& c)
    {
        c.setColour (juce::ComboBox::backgroundColourId, Palette::viewBg);
        c.setColour (juce::ComboBox::outlineColourId, Palette::panelEdge);
        c.setColour (juce::ComboBox::textColourId, Palette::text);
        c.setColour (juce::ComboBox::arrowColourId, Palette::dim);
        c.setColour (juce::ComboBox::focusedOutlineColourId, colours::select);
    }

    //==============================================================================
    class FlatButton : public juce::Button
    {
    public:
        enum Style { normal, primary, danger, ghost };

        FlatButton (const juce::String& text, Style s = normal) : juce::Button (text), style (s)
        {
            setButtonText (text);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            const bool on = getToggleState();
            const auto tint = style == primary ? colours::loaded : style == danger ? Palette::pink : colours::select;
            const float a = isEnabled() ? 1.0f : 0.4f;

            if (! Surface::plain())
            {
                // a key in the theme's material; primary buttons are lit in their tint
                const Surface::State s { over, down, on || style == primary, isEnabled() };
                if (style != ghost || over || down)
                    Surface::raised (g, getLocalBounds().toFloat(), juce::jmin (Theme::style().controlRadius * 1.2f, 14.0f), s, tint);
                auto ink = style == danger ? Palette::pink : Surface::ink (s, tint);
                if (style == ghost && ! over)
                    ink = Palette::dim;
                g.setColour (ink.withMultipliedAlpha (a));
                g.setFont (Theme::font (12.0f, true));
                g.drawText (getButtonText(), Surface::labelArea (getLocalBounds().toFloat(), s).reduced (6.0f, 0.0f),
                            juce::Justification::centred, true);
                return;
            }

            if (style == primary || on)
            {
                g.setColour (tint.withAlpha ((down ? 0.42f : over ? 0.32f : 0.22f) * a));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (tint.withAlpha (0.8f * a));
                g.drawRoundedRectangle (r, 5.0f, 1.0f);
            }
            else if (style != ghost || over)
            {
                g.setColour ((over ? Palette::panelEdge : Palette::viewBg).withAlpha (a).brighter (down ? 0.15f : 0.0f));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour ((style == danger ? Palette::pink.withAlpha (over ? 0.8f : 0.45f)
                                              : Palette::panelEdge.brighter (over ? 0.4f : 0.1f)).withMultipliedAlpha (a));
                g.drawRoundedRectangle (r, 5.0f, 1.0f);
            }

            auto ink = style == danger ? Palette::pink : (style == primary || on ? Palette::text : (over ? Palette::text : Palette::text.withAlpha (0.85f)));
            if (style == ghost && ! over)
                ink = Palette::dim;
            g.setColour (ink.withMultipliedAlpha (a));
            g.setFont (Theme::font (12.0f, true));
            g.drawText (getButtonText(), getLocalBounds().reduced (6, 0), juce::Justification::centred, true);
        }

        Style style;
    };

    // Small square glyph buttons for the bar
    class IconButton : public juce::Button
    {
    public:
        enum Kind { prev, next, star, menu, close };

        explicit IconButton (Kind k) : juce::Button ("icon"), kind (k)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            if (! Surface::plain())
            {
                const Surface::State s { over, down, false, isEnabled() };
                if (over || down || framed)
                    Surface::raised (g, getLocalBounds().toFloat(), juce::jmin (Theme::style().controlRadius * 1.2f, 14.0f), s, colours::select);
                r = Surface::labelArea (getLocalBounds().toFloat(), s);
            }
            else
            {
                if (over || down || framed)
                {
                    g.setColour (down ? Palette::panelEdge.brighter (0.2f) : over ? Palette::panelEdge : Palette::viewBg);
                    g.fillRoundedRectangle (r, 5.0f);
                }
                if (framed)
                {
                    g.setColour (Palette::panelEdge.brighter (over ? 0.4f : 0.1f));
                    g.drawRoundedRectangle (r, 5.0f, 1.0f);
                }
            }

            const auto ink = over ? Palette::text : Palette::text.withAlpha (0.75f);
            const auto glyph = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()),
                                                        juce::jmin (r.getWidth(), r.getHeight()));
            switch (kind)
            {
                case prev:  drawChevron (g, glyph, -1, ink); break;
                case next:  drawChevron (g, glyph, 1, ink); break;
                case star:  drawStar (g, glyph.reduced (glyph.getWidth() * 0.22f), getToggleState(),
                                      getToggleState() ? colours::star : ink); break;
                case menu:
                {
                    g.setColour (ink);
                    const auto c = glyph.getCentre();
                    for (int i = -1; i <= 1; ++i)
                        g.fillEllipse (c.x - 1.7f, c.y + (float) i * 4.6f - 1.7f, 3.4f, 3.4f);
                    break;
                }
                case close:
                {
                    const auto c = glyph.getCentre();
                    const float s = glyph.getWidth() * 0.18f;
                    g.setColour (ink);
                    g.drawLine (c.x - s, c.y - s, c.x + s, c.y + s, 1.6f);
                    g.drawLine (c.x - s, c.y + s, c.x + s, c.y - s, 1.6f);
                    break;
                }
            }
        }

        Kind kind;
        bool framed = true;
    };

    // Toggle pill (genre filters, options)
    class Chip : public juce::Button
    {
    public:
        explicit Chip (const juce::String& text, juce::Colour c = colours::select) : juce::Button (text), tint (c)
        {
            setButtonText (text);
            setClickingTogglesState (true);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            const bool on = getToggleState();
            tint = Theme::follow (tint);   // a theme switch since it was set
            const float rad = r.getHeight() * 0.5f;
            g.setColour (on ? tint.withAlpha (down ? 0.4f : 0.26f) : (over ? Palette::panelEdge : Palette::viewBg));
            g.fillRoundedRectangle (r, rad);
            g.setColour (on ? tint.withAlpha (0.9f) : Palette::panelEdge.brighter (over ? 0.5f : 0.15f));
            g.drawRoundedRectangle (r, rad, 1.0f);
            g.setColour (on ? Palette::text : (over ? Palette::text : Palette::dim));
            g.setFont (Theme::font (11.5f, on));
            g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
        }

        int idealWidth() const { return (int) std::ceil (textWidth (Theme::font (11.5f, true), getButtonText()) + 22.0f); }

        juce::Colour tint;
    };

    // Search field that lets the owner handle navigation keys before the editor does
    class SearchBox : public juce::TextEditor
    {
    public:
        std::function<bool (const juce::KeyPress&)> onNavigationKey;

        bool keyPressed (const juce::KeyPress& k) override
        {
            if (onNavigationKey != nullptr && onNavigationKey (k))
                return true;
            return juce::TextEditor::keyPressed (k);
        }

        void paintOverChildren (juce::Graphics& g) override
        {
            // magnifier glyph in the left indent
            const auto r = juce::Rectangle<float> (8.0f, (float) getHeight() * 0.5f - 7.0f, 14.0f, 14.0f);
            g.setColour (hasKeyboardFocus (true) ? Palette::text : Palette::dim);
            g.drawEllipse (r.getX() + 1.0f, r.getY() + 1.0f, 8.5f, 8.5f, 1.4f);
            g.drawLine (r.getX() + 8.6f, r.getY() + 8.6f, r.getRight() - 1.0f, r.getBottom() - 1.0f, 1.6f);
        }
    };

    //==============================================================================
    // Dimmed full-window layer with one centred dialog. Esc or a click outside dismisses it.
    class ModalOverlay : public juce::Component, private juce::ComponentListener
    {
    public:
        ModalOverlay (juce::Component& hostComp, std::unique_ptr<juce::Component> c, std::function<void()> dismissFn)
            : host (&hostComp), content (std::move (c)), dismiss (std::move (dismissFn))
        {
            setWantsKeyboardFocus (true);
            addAndMakeVisible (*content);
            hostComp.addComponentListener (this);
            setBounds (hostComp.getLocalBounds());
            hostComp.addAndMakeVisible (this);
            toFront (false);
            if (isShowing())
                grabKeyboardFocus();
        }

        ~ModalOverlay() override
        {
            if (host != nullptr)
                host->removeComponentListener (this);
        }

        void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black.withAlpha (0.55f)); }
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override {}

        void resized() override
        {
            const int w = juce::jmin (content->getWidth(), getWidth() - 16);
            const int h = juce::jmin (content->getHeight(), getHeight() - 16);
            content->setBounds (getLocalBounds().withSizeKeepingCentre (w, h));
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (! content->getBounds().contains (e.getPosition()) && dismiss != nullptr)
                dismiss();
        }

        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress::escapeKey && dismiss != nullptr)
            {
                dismiss();
                return true;
            }
            return true;   // modal: keys never reach the editor behind
        }

        juce::Component* getContent() const { return content.get(); }

    private:
        void componentMovedOrResized (juce::Component& c, bool, bool wasResized) override
        {
            if (wasResized)
                setBounds (c.getLocalBounds());
        }

        juce::Component::SafePointer<juce::Component> host;
        std::unique_ptr<juce::Component> content;
        std::function<void()> dismiss;
    };

    // Owns at most one overlay. close() may be called from inside the dialog's own callbacks: the
    // overlay is hidden at once and deleted on the next message loop turn.
    struct ModalHost
    {
        ~ModalHost() { overlay.reset(); }

        template <typename DialogType>
        DialogType* open (juce::Component& hostComp, std::unique_ptr<DialogType> dialog)
        {
            close();
            auto* raw = dialog.get();
            overlay = std::make_unique<ModalOverlay> (hostComp, std::move (dialog), [this] { close(); });
            return raw;
        }

        void close()
        {
            if (overlay == nullptr)
                return;
            auto* o = overlay.release();
            o->setVisible (false);
            if (auto* p = o->getParentComponent())
                p->removeChildComponent (o);
            juce::MessageManager::callAsync ([o] { delete o; });
        }

        bool isOpen() const { return overlay != nullptr; }

        std::unique_ptr<ModalOverlay> overlay;
    };

    //==============================================================================
    // Vertical list with animated (smooth) wheel scrolling, a slim draggable scrollbar and hover
    // tracking. Subclasses paint rows and react to clicks.
    class SmoothList : public juce::Component, private juce::Timer
    {
    public:
        explicit SmoothList (int rowH) : rowHeight (rowH) {}

        virtual int getNumRows() const = 0;
        virtual void paintRow (juce::Graphics&, int row, juce::Rectangle<int> bounds, bool hovered) = 0;
        virtual void rowMouseDown (int /*row*/, const juce::MouseEvent&, juce::Rectangle<int> /*rowBounds*/) {}
        virtual void rowDoubleClicked (int /*row*/) {}
        virtual void paintEmpty (juce::Graphics&) {}

        void contentChanged()
        {
            target = juce::jlimit (0.0f, maxScroll(), target);
            scroll = juce::jlimit (0.0f, maxScroll(), scroll);
            repaint();
        }

        void scrollToRow (int row, bool animate = true)
        {
            if (row < 0)
                return;
            const float top = (float) (row * rowHeight);
            const float bottom = top + (float) rowHeight;
            float t = target;
            if (top < t)
                t = top;
            else if (bottom > t + (float) getHeight())
                t = bottom - (float) getHeight();
            setTarget (t, animate);
        }

        void centreRow (int row)
        {
            if (row >= 0)
                setTarget ((float) (row * rowHeight) - ((float) getHeight() - (float) rowHeight) * 0.5f, false);
        }

        void scrollToTop() { setTarget (0.0f, false); }

        int rowAt (int y) const
        {
            const int row = (int) std::floor (((float) y + scroll) / (float) rowHeight);
            return row >= 0 && row < getNumRows() ? row : -1;
        }

        juce::Rectangle<int> rowBounds (int row) const
        {
            return { 0, juce::roundToInt ((float) (row * rowHeight) - scroll), contentWidth(), rowHeight };
        }

        int visibleRows() const { return juce::jmax (1, getHeight() / rowHeight); }

        void paint (juce::Graphics& g) override
        {
            const int n = getNumRows();
            if (n == 0)
            {
                paintEmpty (g);
                return;
            }

            const int first = juce::jmax (0, (int) (scroll / (float) rowHeight));
            for (int row = first; row < n; ++row)
            {
                const auto b = rowBounds (row);
                if (b.getY() >= getHeight())
                    break;
                paintRow (g, row, b, row == hoverRow);
            }

            if (maxScroll() > 0.0f)
            {
                const auto track = barArea().toFloat();
                const float total = (float) (n * rowHeight);
                const float thumbH = juce::jmax (24.0f, track.getHeight() * (float) getHeight() / total);
                const float y = track.getY() + (track.getHeight() - thumbH) * (scroll / maxScroll());
                g.setColour (Palette::dim.withAlpha (draggingBar || barHover ? 0.55f : 0.28f));
                g.fillRoundedRectangle ({ track.getX() + 1.0f, y, track.getWidth() - 2.0f, thumbH }, 2.5f);
            }
        }

        void resized() override { contentChanged(); }

        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
        {
            const float dy = w.isReversed ? -w.deltaY : w.deltaY;
            setTarget (target - dy * (float) rowHeight * 6.0f, true);
        }

        void mouseMove (const juce::MouseEvent& e) override { updateHover (e.getPosition()); }

        void mouseExit (const juce::MouseEvent&) override
        {
            barHover = false;
            setHover (-1);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (maxScroll() > 0.0f && barArea().expanded (4, 0).contains (e.getPosition()))
            {
                draggingBar = true;
                dragStartScroll = scroll;
                repaint();
                return;
            }
            const int row = rowAt (e.y);
            if (row >= 0)
                rowMouseDown (row, e, rowBounds (row));
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! draggingBar)
                return;
            const float total = (float) (getNumRows() * rowHeight);
            const float perPixel = total / juce::jmax (1.0f, (float) getHeight());
            setTarget (dragStartScroll + (float) e.getDistanceFromDragStartY() * perPixel, false);
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            draggingBar = false;
            repaint();
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (draggingBar || barArea().contains (e.getPosition()))
                return;
            const int row = rowAt (e.y);
            if (row >= 0)
                rowDoubleClicked (row);
        }

    protected:
        int contentWidth() const { return getWidth() - (maxScroll() > 0.0f ? barWidth + 2 : 0); }
        int hoverRow = -1;
        const int rowHeight;

    private:
        static constexpr int barWidth = 7;

        float maxScroll() const { return juce::jmax (0.0f, (float) (getNumRows() * rowHeight - getHeight())); }
        juce::Rectangle<int> barArea() const { return { getWidth() - barWidth, 2, barWidth, juce::jmax (0, getHeight() - 4) }; }

        void setTarget (float t, bool animate)
        {
            target = juce::jlimit (0.0f, maxScroll(), t);
            if (animate)
            {
                startTimerHz (60);
            }
            else
            {
                scroll = target;
                stopTimer();
                repaint();
            }
        }

        void timerCallback() override
        {
            scroll += (target - scroll) * 0.32f;
            if (std::abs (target - scroll) < 0.5f)
            {
                scroll = target;
                stopTimer();
            }
            updateHover (getMouseXYRelative());
            repaint();
        }

        void updateHover (juce::Point<int> p)
        {
            const bool overBar = maxScroll() > 0.0f && barArea().expanded (4, 0).contains (p);
            if (overBar != barHover)
            {
                barHover = overBar;
                repaint();
            }
            setHover (isMouseOver() && ! overBar ? rowAt (p.y) : -1);
        }

        void setHover (int row)
        {
            if (row != hoverRow)
            {
                hoverRow = row;
                repaint();
            }
        }

        float scroll = 0.0f, target = 0.0f, dragStartScroll = 0.0f;
        bool draggingBar = false, barHover = false;
    };

    //==============================================================================
    class ConfirmDialog : public juce::Component
    {
    public:
        ConfirmDialog (const juce::String& titleText, const juce::String& messageText, const juce::String& confirmText,
                       std::function<void()> onConfirm, std::function<void()> onCancel)
            : title (titleText), message (messageText), confirm (confirmText, FlatButton::danger), cancel ("Cancel")
        {
            confirm.onClick = [onConfirm, onCancel]
            {
                if (onConfirm != nullptr) onConfirm();
                if (onCancel != nullptr) onCancel();   // closes the dialog
            };
            cancel.onClick = [onCancel] { if (onCancel != nullptr) onCancel(); };
            addAndMakeVisible (confirm);
            addAndMakeVisible (cancel);
            setSize (400, 150);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (Palette::panel);
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (Palette::panelEdge.brighter (0.2f));
            g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);
            g.setColour (Palette::text);
            g.setFont (Theme::font (15.0f, true));
            g.drawText (title, 20, 16, getWidth() - 40, 22, juce::Justification::centredLeft);
            g.setColour (Palette::dim);
            g.setFont (Theme::font (12.5f));
            g.drawFittedText (message, 20, 44, getWidth() - 40, 46, juce::Justification::topLeft, 3, 1.0f);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (20, 16).removeFromBottom (30);
            confirm.setBounds (r.removeFromRight (110));
            r.removeFromRight (8);
            cancel.setBounds (r.removeFromRight (90));
        }

    private:
        juce::String title, message;
        FlatButton confirm, cancel;
    };

    //==============================================================================
    // Save / Save As / Rename. Saves straight through the manager; onDone closes the overlay.
    class SaveDialog : public juce::Component
    {
    public:
        enum class Mode { save, saveAs, rename };

        SaveDialog (PresetManager& m, Mode md, std::function<void()> done)
            : manager (m), mode (md), onDone (std::move (done)), okButton ("Save", FlatButton::primary), cancelButton ("Cancel")
        {
            const auto meta = manager.currentMetadata();
            renameKey = manager.getCurrentKey();

            styleEditor (nameEditor);
            nameEditor.setText (mode == Mode::saveAs ? manager.uniqueUserName (meta.name) : meta.name, false);
            nameEditor.setTextToShowWhenEmpty ("Preset name", Palette::dim);
            nameEditor.onTextChange = [this] { updateStatus(); };
            nameEditor.onReturnKey = [this] { commit(); };
            nameEditor.onEscapeKey = [this] { finish(); };
            addAndMakeVisible (nameEditor);

            if (mode != Mode::rename)
            {
                styleCombo (categoryBox);
                categoryBox.addItemList (PresetManager::categories(), 1);
                categoryBox.setText (meta.category, juce::dontSendNotification);
                addAndMakeVisible (categoryBox);

                styleEditor (authorEditor);
                authorEditor.setText (meta.author, false);
                authorEditor.setTextToShowWhenEmpty ("Your name (optional)", Palette::dim);
                authorEditor.onEscapeKey = [this] { finish(); };
                addAndMakeVisible (authorEditor);

                styleEditor (descriptionEditor, true);
                descriptionEditor.setText (meta.description, false);
                descriptionEditor.setTextToShowWhenEmpty ("What it sounds like, and what input it suits", Palette::dim);
                descriptionEditor.onEscapeKey = [this] { finish(); };
                addAndMakeVisible (descriptionEditor);

                for (auto& gname : PresetManager::genres())
                {
                    auto* c = genreChips.add (new Chip (gname));
                    c->setToggleState (meta.genres.contains (gname), juce::dontSendNotification);
                    addAndMakeVisible (c);
                }
            }

            okButton.onClick = [this] { commit(); };
            cancelButton.onClick = [this] { finish(); };
            addAndMakeVisible (okButton);
            addAndMakeVisible (cancelButton);

            setSize (500, layout (500, false));
            updateStatus();
        }

        void focusName()
        {
            nameEditor.grabKeyboardFocus();
            nameEditor.selectAll();
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (Palette::panel);
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (Palette::panelEdge.brighter (0.2f));
            g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);

            g.setColour (Palette::text);
            g.setFont (Theme::font (15.0f, true));
            const auto titleText = mode == Mode::rename ? juce::String ("Rename preset")
                                 : mode == Mode::saveAs ? juce::String ("Save as new preset") : juce::String ("Save preset");
            g.drawText (titleText, pad, 14, getWidth() - 2 * pad, 22, juce::Justification::centredLeft);

            g.setFont (Theme::font (10.5f, true));
            g.setColour (Palette::dim);
            for (auto& [text, area] : labels)
                g.drawText (text, area, juce::Justification::centredLeft);

            g.setColour (Theme::follow (statusColour));
            g.setFont (Theme::font (11.5f));
            g.drawFittedText (status, statusArea, juce::Justification::centredLeft, 2, 1.0f);
        }

        void resized() override { layout (getWidth(), true); }

    private:
        // Lays the fields out top to bottom; returns the height needed
        int layout (int width, bool apply)
        {
            const int w = width - 2 * pad;
            int y = 46;
            if (apply)
                labels.clear();

            auto label = [&] (const juce::String& text)
            {
                if (apply)
                    labels.emplace_back (text, juce::Rectangle<int> (pad, y, w, 14));
                y += 17;
            };

            label ("NAME");
            if (apply) nameEditor.setBounds (pad, y, w, 30);
            y += 30 + 12;

            if (mode != Mode::rename)
            {
                const int half = (w - 12) / 2;
                if (apply)
                {
                    labels.emplace_back ("CATEGORY", juce::Rectangle<int> (pad, y, half, 14));
                    labels.emplace_back ("AUTHOR", juce::Rectangle<int> (pad + half + 12, y, w - half - 12, 14));
                }
                y += 17;
                if (apply)
                {
                    categoryBox.setBounds (pad, y, half, 28);
                    authorEditor.setBounds (pad + half + 12, y, w - half - 12, 28);
                }
                y += 28 + 12;

                label ("GENRE TAGS");
                int x = pad;
                for (auto* c : genreChips)
                {
                    const int cw = c->idealWidth();
                    if (x + cw > pad + w)
                    {
                        x = pad;
                        y += 30;
                    }
                    if (apply)
                        c->setBounds (x, y, cw, 24);
                    x += cw + 6;
                }
                y += 24 + 14;

                label ("DESCRIPTION");
                if (apply) descriptionEditor.setBounds (pad, y, w, 58);
                y += 58 + 8;
            }

            if (apply) statusArea = { pad, y, w, 30 };
            y += 30 + 8;
            if (apply)
            {
                okButton.setBounds (pad + w - 120, y, 120, 32);
                cancelButton.setBounds (pad + w - 120 - 8 - 96, y, 96, 32);
            }
            y += 32 + 18;
            return y;
        }

        void updateStatus()
        {
            const auto name = nameEditor.getText().trim();
            const bool exists = manager.userPresetExists (name);
            okButton.setEnabled (name.isNotEmpty());
            okButton.setButtonText (mode == Mode::rename ? "Rename" : "Save");
            statusColour = Palette::dim;

            if (name.isEmpty())
            {
                status = "Type a name for the preset.";
            }
            else if (mode == Mode::rename)
            {
                auto* e = manager.findEntry (renameKey);
                const bool sameFile = e != nullptr && manager.userFileFor (name) == e->file;
                if (exists && ! sameFile)
                {
                    status = "A user preset called \"" + name + "\" already exists.";
                    statusColour = Palette::pink;
                    okButton.setEnabled (false);
                }
                else
                {
                    status = "Renames the preset file in Documents/Clod Scope/Presets.";
                }
            }
            else if (exists)
            {
                status = "Replaces your existing preset \"" + name + "\".";
                statusColour = colours::modified;
                okButton.setButtonText ("Overwrite");
            }
            else
            {
                status = "Saved to Documents/Clod Scope/Presets as a user preset.";
            }
            repaint();
        }

        void commit()
        {
            const auto name = nameEditor.getText().trim();
            if (name.isEmpty() || ! okButton.isEnabled())
                return;

            juce::String err;
            if (mode == Mode::rename)
            {
                err = manager.renameUserPreset (renameKey, name);
            }
            else
            {
                PresetManager::Metadata md;
                md.name = name;
                md.category = categoryBox.getText().isNotEmpty() ? categoryBox.getText() : juce::String ("Experimental & Physics");
                md.description = descriptionEditor.getText().replaceCharacters ("\r\n", "  ").trim();
                md.author = authorEditor.getText().trim();
                for (auto* c : genreChips)
                    if (c->getToggleState())
                        md.genres.add (c->getButtonText());
                err = manager.saveUserPreset (md, true);   // the status line already warned about overwriting
            }

            if (err.isEmpty())
            {
                finish();
            }
            else
            {
                status = err;
                statusColour = Palette::pink;
                repaint();
            }
        }

        void finish()
        {
            if (onDone != nullptr)
                onDone();
        }

        static constexpr int pad = 20;
        PresetManager& manager;
        Mode mode;
        std::function<void()> onDone;
        juce::String renameKey;

        juce::TextEditor nameEditor, authorEditor, descriptionEditor;
        juce::ComboBox categoryBox;
        juce::OwnedArray<Chip> genreChips;
        FlatButton okButton, cancelButton;

        std::vector<std::pair<juce::String, juce::Rectangle<int>>> labels;
        juce::String status;
        juce::Colour statusColour { Palette::dim };
        juce::Rectangle<int> statusArea;
    };
}
