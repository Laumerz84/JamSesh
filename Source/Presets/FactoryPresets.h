#pragma once

#include <juce_core/juce_core.h>
#include <utility>
#include <vector>

// One factory preset. Values are REAL (denormalised) parameter values keyed by parameter id; every
// sound parameter not listed is reset to its default when the preset loads (see
// PresetManager::isSoundParameter), so a preset always sounds the same regardless of what was
// loaded before it. Write presets with the builder in PresetBuilder.h rather than by hand.
struct FactoryPreset
{
    juce::String name;
    juce::String category;        // one of PresetManager::categories()
    juce::StringArray genres;     // tags from PresetManager::genres()
    juce::String description;     // one sentence: what it sounds like and what input it suits
    std::vector<std::pair<juce::String, float>> values;
    juce::String sculptTableA;    // optional SculptModel::tableToString(...) for table A (empty = default),
                                  // or "wave:<name>" for one of SculptModel::waveNames()
    juce::String sculptTableB;    // optional table B (morph target), same forms as A
    juce::String equationFormula; // optional custom formula (used when eqPreset = "Custom formula")

    // Choice parameters given by option text (e.g. { "shape", "Wavefold" }), resolved against the
    // parameter's choices when the preset loads, so presets survive reordered choice lists.
    // Applied after `values`.
    std::vector<std::pair<juce::String, juce::String>> choices;
};

// The factory library, in browser order. Owner: Agent "Presets" (FactoryPresets.cpp).
std::vector<FactoryPreset> makeFactoryPresets();
