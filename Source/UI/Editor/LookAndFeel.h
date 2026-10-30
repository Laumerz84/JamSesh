#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Theme.h"

// The plugin-wide look. Controls take their accent colour from the nearest ancestor that has one
// (Theme::setAccent), so every knob, combo, toggle and segmented button matches its module.
class ScopeLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ScopeLookAndFeel();
    void applyTheme();   // re-reads the palette into the colour table (theme changes)

    // knobs
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;

    // tabs (kept for panels that use their own tab bars)
    int getTabButtonBestWidth (juce::TabBarButton&, int tabDepth) override;
    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool isMouseOver, bool isMouseDown) override;
    void drawTabbedButtonBarBackground (juce::TabbedButtonBar&, juce::Graphics&) override;
    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int w, int h) override;

    // buttons (segmented switches, view switch, guide pages)
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& background,
                               bool isMouseOver, bool isMouseDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isMouseOver, bool isMouseDown) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isMouseOver, bool isMouseDown) override;

    // combo boxes and menus
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    void preparePopupMenuWindow (juce::Component&) override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItemWithOptions (juce::Graphics&, const juce::Rectangle<int>& area, bool isHighlighted,
                                       const juce::PopupMenu::Item&, const juce::PopupMenu::Options&) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>&, const juce::String&) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;

    // text editors, scrollbars, tooltips
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                        int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;
    int getMinimumScrollbarThumbSize (juce::ScrollBar&) override { return 24; }
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

private:
    static juce::TextLayout tooltipLayout (const juce::String& text);
};
