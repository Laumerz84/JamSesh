#pragma once

#include "JamTypes.h"
#include <memory>

// Raw-input mode (SendMode::rawInput): a friend sends only their dry input and their settings, and
// every receiver rebuilds their sound with a hidden replica of the plugin (a ClodScopeProcessor
// created with jamReplica = true: no Jam Sesh, no editor) kept in sync with the friend's state.
//
// Owner: net soldier. process() is called by the audio engine (stream soldier).
namespace jam
{
    class Session;

    class Replicas
    {
    public:
        explicit Replicas (Session&);
        ~Replicas();

        // message thread
        void prepare (double sampleRate, int maxBlockSize);
        void stateArrived (PeerId, const juce::MemoryBlock& pluginState);       // their whole getStateInformation()
        void parameterArrived (PeerId, const juce::String& paramId, float value);   // a knob they moved (real value)
        void removePeer (PeerId);

        // CONTRACT (audio thread, called by the engine): turns their raw input in io into their finished
        // sound, in place, with their MIDI. Returns false (io untouched) if there is no replica for them yet.
        bool process (PeerId, juce::AudioBuffer<float>& io, int numSamples, juce::MidiBuffer& midi) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Replicas)
    };
}
