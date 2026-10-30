#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_graphics/juce_graphics.h>
#include <vector>

// Jam Sesh: playing together over the network. Shared types for the session (Jam/JamSession.h), the
// network (Jam/JamNet.h), the audio engine (Jam/JamEngine.h, Jam/JamPort.h), the replicas that rebuild
// a friend's sound from their raw input (Jam/JamReplica.h) and the UI (Jam/JamView.h, Jam/JamPanel.h).
//
// Contract file (owner: lead). Changes go through the board (ROUND BOARD.md).
namespace jam
{
    using PeerId = juce::uint32;               // 0 = nobody; the host is always 1
    constexpr PeerId noPeer = 0, hostPeer = 1;
    constexpr int maxPlayers = 6;              // the host included
    constexpr int defaultPort = 47800;         // UDP; the host forwards it on the router, or everyone uses a VPN

    // What a plugin instance contributes. FL Studio: the instance on the guitar insert is the player,
    // an instance on the Master can be the backing track. The standalone app is a player that can also
    // play a backing track from a file.
    enum class Role { player, track };

    // What a player sends: the finished stereo sound, or the dry input (mono when the input is mono)
    // plus their settings, from which every receiver rebuilds the sound with a replica of the plugin.
    enum class SendMode { finishedSound, rawInput };

    // Every option that trades delay for safety or bandwidth is a switch ("make all latency options
    // optional"). Each player sets their own; values a sender decides (compression, packet size,
    // send mode) travel with the audio so receivers always know how to read it.
    struct LatencyOptions
    {
        bool compression = false;      // Opus; off = uncompressed 16-bit PCM (no codec delay, ~0.77 Mbit/s per mono channel)
        int  opusKbps = 128;           // per stream when compression is on
        bool lossProtection = false;   // repeat each packet's audio in the next one: survives single lost packets, doubles bandwidth
        int  packetSamples = 128;      // samples per packet at 48 kHz: 64 / 128 / 256 (1.3 / 2.7 / 5.3 ms)
        bool autoJitterBuffer = true;  // adapt the safety buffer to the connection; off = jitterMs fixed
        float jitterMs = 10.0f;
        bool driftCorrection = true;   // stretch each friend's audio to this sound card's clock (inaudible, prevents dropouts)
        bool syncTrack = true;         // play the backing track here as late as it reaches friends, so everyone hears it together
        bool oneBarMode = false;       // far-away mode: everyone hears everyone exactly one bar late, in time with the beat
        SendMode sendMode = SendMode::finishedSound;
    };

    // One player as the UI sees it (a snapshot, refreshed ~15 Hz)
    struct PeerInfo
    {
        PeerId id = noPeer;
        juce::String name;
        juce::Colour colour;
        bool isSelf = false, isHost = false, connected = false;
        bool relayed = false;                  // no direct path: their sound comes through the host (more delay)
        bool hasTrack = false;                 // streams a backing track
        SendMode sendMode = SendMode::finishedSound;
        bool compression = false;

        float pingMs = 0.0f;                   // round trip
        float networkMs = 0.0f;                // one way (half the ping, refined by the clock sync)
        float jitterMs = 0.0f;                 // spread of packet arrival times
        float bufferMs = 0.0f;                 // this side's safety buffer for them
        float totalMs = 0.0f;                  // their pick attack to your ears, everything included
        float lossPercent = 0.0f;              // packets lost or too late, last 10 s
        float driftPpm = 0.0f;                 // their sound card's clock relative to yours

        float levelDb = -100.0f;               // their sound, as it arrives
        juce::String chord;                    // what their tracker hears ("E", "C/G"), empty if nothing

        float volume = 1.0f;                   // your mix of them
        bool muted = false;
    };

    // Session state the UI shows
    enum class Status { idle, hosting, joining, connected, failed };

    // A backing track played by this instance (the standalone app's file player)
    struct TrackSource
    {
        virtual ~TrackSource() = default;
        // Audio thread: adds nothing if stopped; writes numSamples of the track (stereo) into out
        virtual void renderNextBlock (juce::AudioBuffer<float>& out, int numSamples) noexcept = 0;
        virtual bool isPlaying() const noexcept = 0;
    };
}
