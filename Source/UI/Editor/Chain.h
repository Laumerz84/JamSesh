#pragma once

#include "../Widgets.h"
#include "../../PluginProcessor.h"

// The editor's navigation model: every module of the signal chain (and the display views) is a
// "page" with a chip in the chain strip and a control panel in the right column. The table in
// EditorParts.cpp is the one place that lists them in signal order.
namespace chain
{
    enum Page
    {
        input, synth, sculpt, equation,
        amp, cab, eq, comp, mod, delay, reverb,
        slit, limiter, output,
        numChain,                                 // pages above are the audio chain, in order
        viewScope = numChain, view3D, viewOrbital, viewJam,
        numPages
    };

    struct Info
    {
        juce::String key;          // stable id: saved in the state ("uiModule") and used for help ids "chain.<key>"
        juce::String name;         // chip / header label, e.g. "REVERB"
        juce::String shortName;    // used when the strip is narrow
        juce::String title;        // header title, e.g. "Reverb"
        juce::String description;  // one line, for the header and the Inspector
        juce::String enableId;     // on/off parameter, empty if the module is always on
        juce::Colour accent;
        int group = -1;            // bracket in the strip: 0 = SOURCE, 1 = FX RACK, 2 = MASTER, -1 = none
    };

    const Info& info (int page);
    int pageForKey (const juce::String& key);         // -1 if unknown
    bool isViewPage (int page);
    int fxModule (int page);                          // FxRack::Module for FX pages, -1 otherwise
    HelpList helpEntries();                           // "chain.<key>" for every chip
    juce::String groupName (int group);

    // true if the page's module currently affects the sound (its switch is on; SCULPT follows the synth)
    bool isActive (juce::AudioProcessorValueTreeState&, int page);
}

//==============================================================================
// Full-width strip of clickable chips in signal order. A chip's LED switches its module on/off,
// clicking the chip selects its page. The last chip (VIEW) opens the panel of the current main view.
// The chips are grouped under SOURCE, FX RACK, MASTER and DISPLAY: brackets, or (in the materials that
// have them, UI/Surface.h) a tray per group with its label on a tab moulded onto it. Clicking a group's
// label opens the group's first module.
class ChainStrip : public juce::Component, public juce::TooltipClient
{
public:
    ChainStrip (juce::AudioProcessorValueTreeState&, HelpTarget*);
    ~ChainStrip() override;

    std::function<void (int page)> onSelect;                 // page, or chain::numPages for the VIEW chip
    std::function<juce::String (int page)> statusText;       // short live status line under each label

    void setSelected (int page, bool viewChipSelected);
    void setViewName (const juce::String&);
    void refresh();                                          // re-reads statuses and on/off states (call ~10 Hz)

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

    static constexpr int idealHeight = 64;

private:
    class Chip;

    // One group of chips: where its chips are (body) and where its label goes (tab)
    struct Group
    {
        juce::String label, about;
        juce::Colour accent;
        juce::Rectangle<float> body, tab;
        int firstPage = 0;                                   // opened by a click on the label (numPages = VIEW chip)
    };
    std::vector<Group> groups() const;
    int groupAt (juce::Point<float>) const;
    void paintPlain (juce::Graphics&);                       // the flat material: the original brackets

    juce::AudioProcessorValueTreeState& state;
    juce::OwnedArray<Chip> chips;                            // chain::numChain chips + the VIEW chip
    std::vector<juce::Rectangle<int>> connectors;            // between chip i and i + 1
    int selected = -1;
    int hoverGroup = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChainStrip)
};

//==============================================================================
// Header above the right column's panel: module name in its accent colour, power button,
// one-line description, an optional action button and previous / next module buttons.
class ModuleHeader : public juce::Component
{
public:
    ModuleHeader (juce::AudioProcessorValueTreeState&, HelpTarget*);

    void setPage (int page);
    void setAction (const juce::String& text, std::function<void()> fn);   // empty text hides the button
    std::function<void (int delta)> onStep;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int idealHeight = 66;

private:
    juce::AudioProcessorValueTreeState& state;
    HelpTarget* help;
    int page = -1;
    std::unique_ptr<ParamPowerButton> power;
    juce::TextButton actionButton, prevButton { "<" }, nextButton { ">" };
    std::function<void()> action;
    juce::Rectangle<int> titleArea, descArea;
};

//==============================================================================
// Drag handle between the panel and the Inspector: drag to resize, double-click to collapse.
class SplitHandle : public juce::Component, public juce::SettableTooltipClient
{
public:
    SplitHandle();
    std::function<void (int dy, bool finished)> onDrag;      // dy relative to the drag start
    std::function<void()> onDoubleClick;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
};

//==============================================================================
// OUTPUT page: global dry/wet and output gain, plus the whole chain at a glance (click a row to go there).
class OutputPanel : public ControlPanel
{
public:
    OutputPanel (juce::AudioProcessorValueTreeState&, HelpTarget*);

    std::function<void (int page)> onSelect;

    int idealHeight (int width) const override;
    juce::String title() const override { return "Output"; }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    int rowAt (juce::Point<int>) const;
    juce::Rectangle<int> rowBounds (int row) const;

    juce::AudioProcessorValueTreeState& state;
    ParamKnob mix, outGain;
    int hoverRow = -1;
    static constexpr int knobTop = 30, listTop = 30 + Theme::knobCellHeight + 30, rowH = 30;
};

// Shown if a module could not create its panel (keeps the editor usable)
class PlaceholderPanel : public ControlPanel
{
public:
    explicit PlaceholderPanel (juce::String name) : label (std::move (name)) {}
    int idealHeight (int) const override { return 80; }
    juce::String title() const override { return label; }
    void paint (juce::Graphics& g) override
    {
        Theme::drawSection (g, getLocalBounds(), label.toUpperCase());
        g.setColour (Palette::dim);
        g.setFont (Theme::font (12.0f));
        g.drawText ("No controls available.", getLocalBounds().reduced (14, 8).withTrimmedTop (24), juce::Justification::topLeft);
    }

private:
    juce::String label;
};
