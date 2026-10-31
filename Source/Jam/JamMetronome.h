#pragma once

#include "JamTypes.h"
#include "../UI/Help.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <memory>

// The shared metronome (owner: metro soldier). One beat grid for the whole sesh, clicks made locally: every
// player's copy synthesises its own click on the host's grid (jam::BeatGrid on the session clock, which JamClock
// keeps within ~1 ms on every computer), early by this computer's own output latency, so the click leaves
// everyone's speakers at the same moment. It never travels as audio (that would arrive late): the grid rides on
// the host's tempo message (Network::sessionMetronome).
//
// The host switches it on and off for everyone and picks the beats per bar; the tempo is the sesh tempo (the host
// program's, or the app's BPM box). Every player has their own volume and mute. What it reaches: this player's
// ears only. jam::Port mixes it after the app's recorder tap (PluginProcessor::processBlock), so it is not sent
// to friends, not in "Finished sound" or raw input, not in recordings. Outside a sesh it runs on its own, on the
// app's BPM (or the host program's tempo), for practice.
//
// While the host program's transport plays (FL Studio), the host's grid lines up with its beats and bars.
namespace jam
{
    class Metronome
    {
    public:
        Metronome() = default;

        // ---- this computer's settings (message thread; the audio thread reads volume and mute) ----
        void setOn (bool on) noexcept                { switchedOn = on; }   // the host's / a solo player's switch
        bool isOn() const noexcept                   { return switchedOn; }
        void setBeatsPerBar (int n) noexcept         { beats = juce::jlimit (1, 16, n); }
        int beatsPerBar() const noexcept             { return beats; }
        void setVolume (float g) noexcept            { gain.store (juce::jlimit (0.0f, 1.5f, g), std::memory_order_relaxed); }
        float volume() const noexcept                { return gain.load (std::memory_order_relaxed); }
        void setMuted (bool m) noexcept              { mute.store (m, std::memory_order_relaxed); }
        bool isMuted() const noexcept                { return mute.load (std::memory_order_relaxed); }

        // ---- the grid (jam::Session, message thread, 30 Hz) ----
        // The grid this computer leads (host, or outside a sesh) at `bpm`: starts a bar shortly after it is switched
        // on, keeps the beat count running through tempo changes, makes the next beat a bar's first when the beats
        // per bar change, and follows the host program's beats while its transport plays.
        BeatGrid ownGrid (double bpm, double sessionNow) noexcept;
        void publish (const BeatGrid&) noexcept;     // the grid the clicks follow (the own one, or the host's)
        BeatGrid current() const noexcept;           // any thread
        // the beat heard now (0 = a bar's first beat, -1 = off); phase: 0..1 through that beat
        static int beatAt (const BeatGrid&, double sessionNow, double* phase = nullptr) noexcept;

        // ---- audio thread (jam::Port::mixMetronome, the player instance only) ----
        // Adds this block's clicks to left / right (right may equal left). blockSession: the session time when the
        // block's callback started; outSeconds: this sound card's output latency (the block's first sample leaves
        // the speakers that much later). The host program's transport for this block lines the grid up with it.
        void render (float* left, float* right, int numSamples, double sampleRate, double blockSession, double outSeconds,
                     double dawBpm, double dawPpq, bool dawPlaying) noexcept;

        static HelpList helpEntries();               // "jam.metro*" ids (JamPanel::helpEntries adds them)

    private:
        bool switchedOn = false;
        int beats = 4;
        std::atomic<float> gain { 0.7f };
        std::atomic<bool> mute { false };
        BeatGrid own;                                // message thread

        // the published grid, behind a seqlock (the audio thread reads it every block)
        std::atomic<juce::uint32> version { 0 };
        std::atomic<bool> pubOn { false };
        std::atomic<int> pubBeats { 4 };
        std::atomic<double> pubBpm { 120.0 }, pubAnchor { 0.0 };

        // the host program's beat 0 on the session clock while its transport plays (audio thread -> ownGrid)
        std::atomic<bool> dawLive { false };
        std::atomic<double> dawAnchor { 0.0 }, dawBpmSeen { 0.0 };

        // audio thread
        double predicted = 0.0, lastRate = 0.0, lastClick = -1.0e9;
        bool timeValid = false;
        struct Voice { int pos = -1; bool accent = false; };
        Voice voice;

        JUCE_DECLARE_NON_COPYABLE (Metronome)
    };

    // A few lights, one per beat of the bar, the beat heard now lit (the first one brighter). Paints nothing while
    // the metronome is off. Used in the panel's METRONOME row and the Jam Sesh view's header.
    class BeatLights : public juce::Component, private juce::Timer
    {
    public:
        BeatLights();
        ~BeatLights() override;
        void paint (juce::Graphics&) override;
        bool showLabel = false;        // "CLICK 120" before the lights (the view's header)

    private:
        void timerCallback() override;
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // The METRONOME row of the Jam Sesh panel: the switch (the host's, or yours outside a sesh; a friend sees the
    // host's choice), beats per bar, your volume and mute, the beat lights and one line on what it is doing.
    class MetronomeRow : public juce::Component, private juce::Timer
    {
    public:
        explicit MetronomeRow (HelpTarget*);
        ~MetronomeRow() override;
        int idealHeight (int width) const;
        void paint (juce::Graphics&) override;
        void resized() override;
        void setAccent (juce::Colour);

    private:
        void timerCallback() override;
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
