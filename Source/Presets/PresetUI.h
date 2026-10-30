#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PresetManager.h"
#include "../UI/Widgets.h"

// Preset bar (top of the editor) and preset browser (in-window overlay).
//
// Owner: Agent "Presets". Public API is a CONTRACT (the editor places these). Private state in Impl.
// Both draw themselves completely (no reliance on the LookAndFeel). Dialogs (save, rename, delete)
// open as dimmed overlays over the editor's top-level component.

class PresetBar : public juce::Component
{
public:
    PresetBar (PresetManager&, HelpTarget*);
    ~PresetBar() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    int idealWidth() const;                 // the editor gives it this width (it can live with less)
    std::function<void()> onBrowse;         // the editor shows the browser overlay

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Fills whatever bounds the editor gives it (normally the whole editor): a dimmed backdrop with
// the browser panel inset. Clicking the backdrop or pressing Esc calls onClose.
class PresetBrowser : public juce::Component
{
public:
    PresetBrowser (PresetManager&, HelpTarget*);
    ~PresetBrowser() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override {}   // never scroll the editor behind
    void visibilityChanged() override;

    void refresh();                         // re-read the preset lists (called when shown)
    std::function<void()> onClose;          // the editor hides the overlay

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
