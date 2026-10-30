#pragma once

#include "../UI/Widgets.h"
#include <memory>

class ClodScopeProcessor;

// The Jam Sesh page in the right column: your name, host / join (session code), the players with
// volume, mute and their latency breakdown, this instance's role (player or backing track), every
// latency option as a switch, and help for routers / VPNs. Owner: jam-ui soldier.
class JamPanel : public ControlPanel
{
public:
    JamPanel (ClodScopeProcessor&, HelpTarget*);
    ~JamPanel() override;

    int idealHeight (int width) const override;
    juce::String title() const override { return "Jam Sesh"; }
    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;     // themes: re-applies the Jam accent to the controls

    static HelpList helpEntries();          // Inspector help for every Jam Sesh control ("jam.*" ids)

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JamPanel)
};
