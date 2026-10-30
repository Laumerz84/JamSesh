#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ViewState.h"

struct ViewFeed;

// A Jam Sesh tile: draws one player's sound the way that player has their view set (their 2D scope,
// their 3D view - e.g. Phase Space - their orbital cloud or their sculpted wave), from their ViewFeed
// and ViewState. The Jam view lays the tiles out and draws the name / latency frame around them.
// Read-only: nothing in a tile changes anyone's settings (the 3D tiles can still be turned with the mouse).
// About 30 frames a second while showing. Owner: views soldier.
class ViewTile : public juce::Component
{
public:
    ViewTile();
    ~ViewTile() override;

    void setFeed (ViewFeed*);                     // nullptr = silence until the sound arrives; must outlive the tile
    void setViewState (const ViewState&);         // switches the kind of view when mainView changes

    // Settings a friend's older build does not send fall back to these parameter defaults (e.g. your own
    // processor's APVTS). Optional; must outlive the tile.
    void setDefaults (const juce::AudioProcessorValueTreeState*);

    void setFramesPerSecond (int);                // default 30

    // Advances one frame now (headless snapshots and timing); the tile's own timer does this while showing
    void tick();
    double lastFrameMs() const;                   // cost of the last frame's analysis + geometry (not painting)

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ViewTile)
};
