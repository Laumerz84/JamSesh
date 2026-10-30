#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "../UI/Params.h"
#include "../UI/Help.h"

// The Wave Sculptor's data: the single-cycle wave the synth layer plays, described as a
// superposition of up to 64 partials, plus "superposition" macro parameters (morph between two
// waves, spectral tilt, odd/even balance, formant, inharmonic stretch, ...).
//
// Owner: Agent "Sculpt" (implements SculptModel.cpp). The public API below is a CONTRACT: the
// synth engine, the preset system and the editor code against it. Private state lives in Impl.
// Parameter ids start with "sc". The editable tables are stored in the APVTS state tree (child
// "Sculpt") so they are saved with projects and presets.

namespace sculpt
{
    constexpr int maxPartials = 64;

    // One editable single-cycle wave: amplitude (0..1) and phase (radians) of harmonics 1..64.
    struct BaseTable
    {
        std::array<float, maxPartials> amp {};
        std::array<float, maxPartials> phase {};
    };

    // What the synth plays: the table(s) after the macro parameters. Plain data so it can be
    // copied without allocation. Partials whose amp is 0 may be skipped by the synth.
    struct PartialSet
    {
        int count = 0;                              // partials in use (<= maxPartials)
        std::array<float, maxPartials> ratio {};    // frequency / fundamental (exactly k + 1 when harmonic)
        std::array<float, maxPartials> amp {};      // linear; scaled so the summed wave peaks near 1.0
        std::array<float, maxPartials> phase {};    // radians at the start of a cycle
        bool harmonic = true;                       // every ratio is an exact integer (wavetable-friendly)
        uint32_t version = 0;                       // changes whenever any value above changes
    };
}

class SculptModel
{
public:
    static void addParameters (ParamList&);
    static HelpList helpEntries();

    // Factory single-cycle waves (Sine, Saw, Square, Triangle, Pulse, Organ, Bell, Vowel...) that the
    // sculptor's quick buttons and the preset library can start from.
    static juce::StringArray waveNames();
    static sculpt::BaseTable makeWave (int index);

    // Compact ASCII serialisation of a table (used in the state tree and in presets)
    static juce::String tableToString (const sculpt::BaseTable&);
    static bool tableFromString (const juce::String&, sculpt::BaseTable& out);

    explicit SculptModel (juce::AudioProcessorValueTreeState&);
    ~SculptModel();

    // ---- audio thread ----
    // Called by the processor at the start of every block: applies the macro parameters to the
    // table(s) and refreshes audioPartials() when anything changed (parameter automation included).
    // RT-safe: no allocation, never blocks.
    void process() noexcept;
    const sculpt::PartialSet& audioPartials() const noexcept;

    // ---- any thread ----
    void copyDisplayPartials (sculpt::PartialSet& out) const noexcept;   // latest final set, for drawing

    // ---- message thread ----
    // Two editable tables, A (0) and B (1). The morph macro blends A -> B; with morph at 0 only A plays.
    sculpt::BaseTable getTable (int which) const;
    void setTable (int which, const sculpt::BaseTable&, bool undoable = true);   // writes the state tree + publishes
    bool undo();
    bool redo();
    void reloadFromState();                 // after a host state restore or a preset load
    juce::String describeMapping() const;   // live, plain words, for the Inspector

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
