#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../ScopeBuffer.h"
#include "../UI/Params.h"
#include "../UI/Help.h"
#include "../UI/Widgets.h"
#include "../Sculpt/SculptModel.h"

// The synth layer: makes any input playable as a synth. In Mono tracking a pitch/envelope tracker
// follows the input and one voice plays the Wave Sculptor's partials at the tracked pitch; in Chord
// tracking a polyphonic tracker hears up to 6 notes at once and each plays its own voice. The same
// engine can also be played polyphonically from MIDI notes sent to the plugin. The layer is mixed
// with the input according to the layer mode (blend / add overtones only / replace).
//
// Owner: Agent "Synth". The public API below is a CONTRACT (the processor, the editor and the Wave
// Sculptor code against it). Private state lives in Impl. Parameter ids start with "syn"; the
// layer's on/off switch is "synOn" (default off, so the plugin passes audio unchanged until a
// preset or the user turns it on).

struct TrackerSnapshot
{
    float f0Hz = 0.0f;         // tracked fundamental of the input (0 = none)
    float confidence = 0.0f;   // 0..1
    float levelDb = -120.0f;   // input envelope, dBFS
    bool voiced = false;       // a stable pitch is present right now
    int midiNote = -1;         // nearest MIDI note to f0 (-1 = none)
    float cents = 0.0f;        // f0 relative to that note
    float synthF0Hz = 0.0f;    // pitch the tracked voice is actually playing (after transpose, glide, scale)

    // Chord tracking mode: the notes heard right now (each plays its own voice), ascending
    bool chordMode = false;
    int numNotes = 0;
    std::array<int, 6> notes { -1, -1, -1, -1, -1, -1 };   // MIDI note numbers
    std::array<float, 6> noteLevels {};                      // RMS of each note (linear)
};

class SynthEngine
{
public:
    static void addParameters (ParamList&);
    static HelpList helpEntries();

    SynthEngine (juce::AudioProcessorValueTreeState&, SculptModel&, ScopeBuffer& layerScope);
    ~SynthEngine();

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    // Audio thread. `analysis` holds the raw plugin input (before any processing) for the tracker;
    // `io` holds the processed input and receives the mix of input and synth layer. The synth layer
    // alone is also pushed into layerScope (for the sculptor's live view). RT-safe. When synOn is off
    // and nothing is sounding, `io` is left untouched (the tracker keeps running cheaply because the
    // editor shows its pitch).
    void process (juce::AudioBuffer<float>& io, int numChannels,
                  const juce::AudioBuffer<float>& analysis, const juce::MidiBuffer& midi) noexcept;

    TrackerSnapshot tracker() const noexcept;   // any thread
    int activeVoices() const noexcept;          // any thread
    juce::String describeMapping() const;       // live plain-words explanation for the Inspector

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class SynthPanel : public ControlPanel
{
public:
    SynthPanel (juce::AudioProcessorValueTreeState&, SynthEngine&, HelpTarget*);
    ~SynthPanel() override;
    int idealHeight (int width) const override;
    juce::String title() const override { return "Synth"; }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
