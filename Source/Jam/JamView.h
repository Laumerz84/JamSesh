#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

class ClodScopeProcessor;

// The Jam Sesh main view: the JAM SESH title (New Rocker font, Source/Resources/Fonts) and one tile
// per player (ViewTile) with their name, colour, chord and latency, each tile drawn the way that
// player has their own view set. Before a session: what Jam Sesh is and how to start one.
// Owner: jam-ui soldier.
class JamView : public juce::Component
{
public:
    explicit JamView (ClodScopeProcessor&);
    ~JamView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;     // themes: everything is painted from Palette / Accent, so a repaint is enough

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JamView)
};
