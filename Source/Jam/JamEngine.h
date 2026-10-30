#pragma once

#include "JamNet.h"
#include <memory>

struct ViewFeed;

// The audio side of Jam Sesh: packing this player's sound (or raw input) into packets, uncompressed
// or Opus; for every friend a jitter buffer, loss concealment, clock-drift correction and the mix;
// the backing track and its sync delay; one-bar mode. Talks to plugin instances through jam::Port.
//
// Owner: stream soldier. Members marked CONTRACT are used by the session (net soldier) or the UI.
namespace jam
{
    class Session;

    class Engine : public AudioPacketSink
    {
    public:
        explicit Engine (Session&);
        ~Engine() override;

        void audioPacketArrived (PeerId from, const juce::uint8* data, int size, double arrivalSeconds) override;

        // CONTRACT (message thread): players come and go, options change
        void addPeer (PeerId);
        void removePeer (PeerId);
        void setOptions (const LatencyOptions&);
        void setPeerVolume (PeerId, float gain);
        void setPeerMuted (PeerId, bool muted);

        // CONTRACT (message thread): numbers for PeerInfo
        struct Stats
        {
            float networkMs = 0.0f, jitterMs = 0.0f, bufferMs = 0.0f, totalMs = 0.0f;
            float lossPercent = 0.0f, driftPpm = 0.0f, levelDb = -100.0f;
            bool hasTrack = false, compression = false;
            SendMode sendMode = SendMode::finishedSound;
        };
        Stats statsFor (PeerId) const;

        // CONTRACT (message thread): that player's sound as it arrives here, for their view tile.
        // Self (selfId() or noPeer) = the player instance's own scopes. Pointers stay valid while the
        // Engine lives (peer slots are reused, never freed).
        ViewFeed* feedFor (PeerId);

        // ---- for the net soldier (any thread, lock-free) ----
        void setPeerNetworkMs (PeerId, float oneWayMs);     // half the ping (or the clock sync's estimate)
        void setSessionTempo (double bpm);                  // the host's tempo; 0 = unknown

        // ---- for jam::Port (audio thread unless noted) ----
        LatencyOptions currentOptions() const noexcept;
        bool hasPeers() const noexcept;
        // Mixes every friend (their sound and their backing track) into left / right (left == right for
        // mono). Only one caller at a time mixes; a second concurrent caller is skipped.
        void mixFriends (float* left, float* right, int numChannels, int numSamples, double sampleRate,
                         double localBpm) noexcept;
        // How late a backing track played here should sound: the slowest friend's network + buffer, or
        // one bar in one-bar mode. 0 outside a session.
        double syncDelaySeconds() const noexcept;
        double barSeconds (double localBpm) const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
    };
}
