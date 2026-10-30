#pragma once

#include <juce_core/juce_core.h>
#include <vector>

// Help text for one control. Every parameter (and every non-parameter control such as a
// formula box) gets one of these so the Inspector can explain it when hovered.
struct ParamHelp
{
    juce::String id;        // APVTS parameter id, or a pseudo id for non-parameter controls (e.g. "eq.formula")
    juce::String title;     // display name, e.g. "Level mapping"
    juce::String summary;   // one sentence: what the control does
    juce::String mapping;   // REQUIRED: what is mapped to what, and how - the rule or formula in plain words,
                            // with units. Per choice option where the parameter is a choice ("Rydberg -> harmonic: ...").
    juce::String tips;      // optional: useful ranges, interactions with other controls, what to listen/look for
};

using HelpList = std::vector<ParamHelp>;

// Implemented by the editor's Inspector panel. Widgets call showHelp on mouse enter and
// clearHelp on mouse exit; the implementation should debounce clearHelp (~250 ms) so moving
// between a knob's label and its slider doesn't flicker.
class HelpTarget
{
public:
    virtual ~HelpTarget() = default;
    virtual void showHelp (const juce::String& id) = 0;
    virtual void clearHelp() = 0;
};
