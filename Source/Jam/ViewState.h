#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <utility>
#include <vector>
#include "../UI/ViewFeed.h"

// What a player has open on screen: the main view and every setting that view uses. Sent to friends
// (about twice a second, and at once when it changes) so the Jam Sesh tile for that player draws the
// same view. Owner: views soldier.
//
// Wire format (toBytes, about 0.5 KB): "VS", format version, mainView, the parameters as (index into an
// append-only id list, float), the text settings likewise, then `extra`. A receiver skips ids it does
// not know, so a newer build can add settings without breaking older ones.
struct ViewState
{
    static constexpr int formatVersion = 1;

    int mainView = 0;                                           // 0 2D scope, 1 3D views, 2 orbital, 3 wave sculptor
    std::vector<std::pair<juce::String, float>> params;         // parameter ids and real values the view needs
    std::vector<std::pair<juce::String, juce::String>> texts;   // text settings (the 3D views' custom formulas)
    juce::MemoryBlock extra;                                    // the sculpted wave (SculptWave::toBytes)

    // Message thread: what this player's Jam Sesh tile shows: the view chosen for it on the Jam Sesh page, or
    // the view they have open (on the Jam Sesh page itself: the one they had open last).
    // The processor version takes the sculpted wave exactly as the synth plays it (prefer it); the APVTS
    // version rebuilds it from the stored tables (A/B morph only, without the other macros).
    static ViewState capture (ClodScopeProcessor&);
    static ViewState capture (juce::AudioProcessorValueTreeState&);

    juce::MemoryBlock toBytes() const;
    static bool fromBytes (const void* data, size_t size, ViewState& out);

    bool sameAs (const ViewState& other) const;

    float get (juce::StringRef id, float fallback) const;
    juce::String getText (juce::StringRef id, const juce::String& fallback) const;
};

// The wave sculptor's final partials (what the synth plays), packed small for ViewState::extra
struct SculptWave
{
    static constexpr int maxPartials = 64;
    int count = 0;
    bool harmonic = true;
    float ratio[maxPartials] {}, amp[maxPartials] {}, phase[maxPartials] {};

    juce::MemoryBlock toBytes() const;
    static bool fromBytes (const juce::MemoryBlock&, SculptWave& out);
    float sampleCycle (float x) const;   // the wave over one cycle of the fundamental, x in 0..1
};

// A friend's settings as the views read them (ViewParams over a ViewState). A setting missing from the
// state (an older build) comes from `defaults` (this plugin's own parameter defaults) or the caller's
// fallback. Message thread.
struct StateParams : public ViewParams
{
    explicit StateParams (const juce::AudioProcessorValueTreeState* defaultsFrom = nullptr) : defaults (defaultsFrom) {}

    float get (juce::StringRef id, float fallback) const override;
    juce::String getText (juce::StringRef id, const juce::String& fallback) const override;

    ViewState state;
    const juce::AudioProcessorValueTreeState* defaults = nullptr;
};
