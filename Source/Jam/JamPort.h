#pragma once

#include "JamSession.h"

class ClodScopeProcessor;

// A plugin instance's connection to the Jam Sesh. Every ClodScopeProcessor owns one, except the
// replicas that rebuild friends' sound. Being the player instance or the backing-track instance is
// this port's role; the session holds the list of ports.
//
// Contract (owner: lead; changes through the board). JamPort.cpp: stream soldier.
namespace jam
{
    class Port
    {
    public:
        explicit Port (ClodScopeProcessor&);
        ~Port();

        ClodScopeProcessor& processor() noexcept { return owner; }

        // message thread (prepareToPlay)
        void prepare (double sampleRate, int maxBlockSize);

        // Audio thread, at the very end of processBlock. rawInput: this block's input before any
        // processing (2 channels, numSamples valid). io: the finished sound; the friends (if this is
        // the player instance) and the backing track (if any) are mixed into it here, and this player's
        // sound or raw input is sent from here. The host's tempo and position come with every block.
        void process (juce::AudioBuffer<float>& io, int numChannels, const juce::AudioBuffer<float>& rawInput,
                      const juce::MidiBuffer& midi, double bpm, double ppqPosition, bool playing) noexcept;

        // message thread
        void setRole (Role);
        Role role() const noexcept;
        void setTrackSource (TrackSource*);   // the standalone app's file player; nullptr = none

        juce::SharedResourcePointer<Session>& session() noexcept { return sessionRef; }

        // The host's tempo and position as of the last processed block (any thread, lock-free).
        // process() stores them every block; the session sends the host's as the session tempo.
        struct Transport { double bpm = 120.0, ppq = 0.0; bool playing = false; };
        Transport lastTransport() const noexcept { return { lastBpm.load(), lastPpq.load(), lastPlaying.load() }; }

    private:
        std::atomic<double> lastBpm { 120.0 }, lastPpq { 0.0 };
        std::atomic<bool> lastPlaying { false };

        ClodScopeProcessor& owner;
        juce::SharedResourcePointer<Session> sessionRef;
        struct Impl;
        std::unique_ptr<Impl> impl;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Port)
    };
}
