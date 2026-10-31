#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <map>
#include "../Theme.h"
#include "../Help.h"

// Rich text made of blocks; boxed blocks get a tinted background with an accent bar.
// Shared by the Inspector and the Concept Guide.
class RichTextView : public juce::Component
{
public:
    struct Block
    {
        juce::AttributedString text;
        bool boxed = false;
        juce::Colour accent = Palette::green;
    };

    static juce::Font bodyFont (float size = 12.5f)  { return Theme::font (size); }
    static juce::Font monoFont (float size = 12.0f)  { return juce::Font (juce::FontOptions().withName (Theme::systemFontName ("Consolas")).withHeight (size)); }

    static Block heading (const juce::String& s, juce::Colour c = Palette::green)
    {
        Block b;
        b.text.append (s, Theme::font (11.0f, true), c);
        return b;
    }

    static Block body (const juce::String& s, juce::Colour c = Palette::text, float size = 12.5f)
    {
        Block b;
        b.text.append (s, bodyFont (size), c);
        return b;
    }

    // Lines containing '=' (formulas) are set in monospace; "## " lines become inline headings
    static Block mixed (const juce::String& s, juce::Colour c = Palette::text, bool boxed = false)
    {
        Block b;
        b.boxed = boxed;
        auto lines = juce::StringArray::fromLines (s);
        for (int i = 0; i < lines.size(); ++i)
        {
            const auto& line = lines[i];
            const juce::String nl = i < lines.size() - 1 ? "\n" : "";
            if (line.startsWith ("## "))
                b.text.append ((i > 0 ? "\n" : "") + line.substring (3).toUpperCase() + nl, Theme::font (11.0f, true), Palette::green);
            else if (line.containsChar ('='))
                b.text.append (line + nl, monoFont(), c);
            else
                b.text.append (line + nl, bodyFont(), c);
        }
        return b;
    }

    void setBlocks (std::vector<Block> newBlocks)
    {
        blocks = std::move (newBlocks);
        relayout();
    }

    // Lays out for the current width and resizes height to fit the content
    void relayout()
    {
        const float w = (float) juce::jmax (40, getWidth());
        layouts.clear();
        float y = 0.0f;
        for (auto& b : blocks)
        {
            const float pad = b.boxed ? 8.0f : 0.0f;
            juce::TextLayout tl;
            b.text.setWordWrap (juce::AttributedString::byWord);
            tl.createLayout (b.text, w - pad * 2.0f - (b.boxed ? 4.0f : 0.0f));
            const float h = tl.getHeight() + pad * 2.0f;
            layouts.push_back ({ std::move (tl), juce::Rectangle<float> (0.0f, y, w, h), b.boxed, b.accent, pad });
            y += h + 6.0f;
        }
        setSize (getWidth(), juce::jmax (10, (int) std::ceil (y)));
        repaint();
    }

    void resized() override
    {
        if (getWidth() != lastWidth)
        {
            lastWidth = getWidth();
            relayout();
        }
    }

    void paint (juce::Graphics& g) override
    {
        for (auto& l : layouts)
        {
            auto r = l.bounds;
            if (l.boxed)
            {
                g.setColour (l.accent.withAlpha (0.08f));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (l.accent.withAlpha (0.8f));
                g.fillRoundedRectangle (r.getX(), r.getY(), 3.0f, r.getHeight(), 1.5f);
                r = r.reduced (l.pad).withTrimmedLeft (4.0f);
            }
            l.layout.draw (g, r);
        }
    }

private:
    struct Laid { juce::TextLayout layout; juce::Rectangle<float> bounds; bool boxed; juce::Colour accent; float pad; };
    std::vector<Block> blocks;
    std::vector<Laid> layouts;
    int lastWidth = -1;
};

// Explains whatever control the mouse is over; otherwise shows the live mapping of the active view.
class Inspector : public juce::Component, public HelpTarget, private juce::Timer
{
public:
    explicit Inspector (juce::AudioProcessorValueTreeState& s) : state (s)
    {
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (8);
        addAndMakeVisible (viewport);
        startTimer (50);
    }

    ~Inspector() override { stopTimer(); }

    void addEntries (const HelpList& list)
    {
        for (auto& e : list)
            if (e.id.isNotEmpty())
                index[e.id] = e;
    }

    const ParamHelp* find (const juce::String& id) const
    {
        auto it = index.find (id);
        return it != index.end() ? &it->second : nullptr;
    }

    // title shown in the header + text producer for the "nothing hovered" state
    void setLiveSource (std::function<juce::String()> titleFn, std::function<juce::String()> textFn)
    {
        liveTitle = std::move (titleFn);
        liveText = std::move (textFn);
        rebuild (true);
    }

    void showHelp (const juce::String& id) override
    {
        clearCountdown = -1;
        if (id != current)
        {
            current = id;
            rebuild (true);
        }
    }

    void clearHelp() override { clearCountdown = 5; }   // 5 ticks x 50 ms = 250 ms debounce

    void paint (juce::Graphics& g) override
    {
        Theme::drawSection (g, getLocalBounds(), "INSPECTOR");
        g.setFont (Theme::font (11.0f));
        g.setColour (Palette::dim);
        const juce::String mode = current.isNotEmpty() ? "hovered control"
                                                       : "live mapping" + (liveTitle ? " - " + liveTitle() : juce::String());
        g.drawText (mode, getLocalBounds().reduced (14, 8).removeFromTop (16), juce::Justification::centredRight);
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds().reduced (12, 8).withTrimmedTop (22));
        content.setSize (viewport.getWidth() - viewport.getScrollBarThickness() - 2, content.getHeight());
        rebuild (true);
    }

private:
    void timerCallback() override
    {
        if (clearCountdown > 0 && --clearCountdown == 0)
        {
            clearCountdown = -1;
            current = {};
            rebuild (true);
        }
        if (++ticks % 4 == 0)   // 5 Hz: live values
            rebuild (false);
    }

    void rebuild (bool force)
    {
        std::vector<RichTextView::Block> b;
        juce::String signature;

        if (current.isNotEmpty())
        {
            auto* param = state.getParameter (current);
            const juce::String value = param != nullptr ? param->getCurrentValueAsText() : juce::String();
            signature << current << "|" << value;

            if (auto* e = find (current))
            {
                RichTextView::Block title;
                title.text.append (e->title, Theme::font (15.0f, true), Palette::text);
                if (value.isNotEmpty())
                    title.text.append ("   = " + value, Theme::font (13.0f, true), Palette::amber);
                b.push_back (title);
                if (e->summary.isNotEmpty())
                    b.push_back (RichTextView::body (e->summary));
                b.push_back (RichTextView::heading ("MAPPING - WHAT IS MAPPED TO WHAT, AND HOW"));
                b.push_back (RichTextView::mixed (e->mapping.isNotEmpty() ? e->mapping : "(not documented)", Palette::text, true));
                if (e->tips.isNotEmpty())
                {
                    b.push_back (RichTextView::heading ("TIPS", Palette::amber));
                    b.push_back (RichTextView::body (e->tips, Palette::dim));
                }
            }
            else
            {
                RichTextView::Block title;
                title.text.append (param != nullptr ? param->getName (64) : current, Theme::font (15.0f, true), Palette::text);
                if (value.isNotEmpty())
                    title.text.append ("   = " + value, Theme::font (13.0f, true), Palette::amber);
                b.push_back (title);
                b.push_back (RichTextView::body ("No detailed help has been written for this control yet.", Palette::dim));
            }
        }
        else
        {
            const juce::String text = liveText ? liveText() : juce::String();
            signature << "live|" << text;
            b.push_back (RichTextView::body ("Hover any control to see exactly what it maps and how. "
                                             "What the current view is doing right now:", Palette::dim, 11.5f));
            b.push_back (RichTextView::mixed (text.isNotEmpty() ? text : "(nothing to report)"));
        }

        if (! force && signature == lastSignature)
            return;
        lastSignature = signature;
        const bool modeChanged = (current.isEmpty()) != lastWasLive;
        lastWasLive = current.isEmpty();
        content.setBlocks (std::move (b));
        if (force || modeChanged)
            repaint();
    }

    juce::AudioProcessorValueTreeState& state;
    std::map<juce::String, ParamHelp> index;
    std::function<juce::String()> liveTitle, liveText;

    juce::Viewport viewport;
    RichTextView content;
    juce::String current, lastSignature;
    bool lastWasLive = true;
    int clearCountdown = -1, ticks = 0;
};
