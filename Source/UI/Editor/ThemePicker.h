#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The theme button in the top bar: opens the theme list with a preview of each palette.
// Applying is instant and global (UI/Themes.h). Owner: themes soldier.
class ThemePicker : public juce::Component, public juce::SettableTooltipClient
{
public:
    ThemePicker();
    ~ThemePicker() override;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    static constexpr int idealWidth = 32;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemePicker)
};
