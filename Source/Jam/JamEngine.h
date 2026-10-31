#pragma once

#include "JamNet.h"
#include <memory>

struct ViewFeed;
class ClodScopeProcessor;

// The audio side of Jam Sesh: packing this player's sound (or raw input) into packets, uncompressed
// or Opus; for every friend a jitter buffer, loss concealment, clock-drift correction and the mix;
// the backing track and its sync delay; one-bar mode. Talks to plugin instances through jam::Port.
//
// Owner: stream soldier. Members marked CONTRACT are used by the session (net soldier) or the UI.
namespace jam
{
    class Session;

    // The latency model behind every number the UI shows (Engine::statsFor, the session's captureMs, the panel's
    // estimate). One way, a player's input jack to a friend's ears, all in ms or 48 kHz session samples:
    //   their sound card in + their sound chain + send hold         = captureMs (the sender works it out and sends it)
    // + codec + network
    // + receive level - P                                          (bufferMs - P/2: bufferMs = level just after an
    //                                                               arrival - P/2, P = the packet's frames)
    // + your sound card out
    // "Receive level - P": a packet's last sample plays `level` after it arrives, its first P earlier; a sample
    // captured at the end of the sender's block leaves at once. With P = the sender's block that is the whole
    // story; a packet longer than the block holds the earlier block's audio P - B longer (send hold), and a block
    // carrying several packets sends its later ones (P - B) / 2 fresher on average (negative hold). When neither
    // divides the other (Opus 120-frame packets in 128-sample blocks) packets end anywhere in a block: P - B / 2.
    // A sound card's latency counts from the block's first sample (as drivers report it: buffer + converters).
    // Unknown (0: inside a plugin host, or an older friend) falls back to half a block per card.
    struct LocalLatency
    {
        float inMs = 0.0f, outMs = 0.0f;   // the sound card as its driver reports it; 0 = unknown (a plugin host)
        float chainMs = 0.0f;              // the sound chain (getLatencySamples)
        float blockMs = 0.0f;              // the audio block the host calls with (or prepared for)
        bool cardKnown() const noexcept    { return inMs > 0.0f && outMs > 0.0f; }
        float inOrGuess() const noexcept   { return inMs > 0.0f ? inMs : 0.5f * blockMs; }
        float outOrGuess() const noexcept  { return outMs > 0.0f ? outMs : 0.5f * blockMs; }
    };

    namespace latency
    {
        constexpr double rate = 48000.0;
        inline float msOf (double samples48) noexcept { return (float) (samples48 * 1000.0 / rate); }

        // frames per packet a sender uses: auto (0) follows its block clamped to 64..128; Opus 2.5 / 5 ms frames
        inline int packetFrames (const LatencyOptions& o, float blockMs) noexcept
        {
            const int p = o.packetSamples > 0 ? o.packetSamples
                                              : juce::jlimit (64, 128, (int) std::lround (blockMs * rate * 0.001));
            return o.compression ? (p <= 128 ? 120 : 240) : juce::jlimit (32, 256, p);    // as wire::framesFor
        }

        // ms a packet's audio waits on the sender beyond its block (see above; negative = fresher)
        inline float sendHoldMs (float packetMs, float blockMs) noexcept
        {
            const int p = juce::roundToInt (packetMs * rate * 0.001), b = juce::roundToInt (blockMs * rate * 0.001);
            if (p <= 0 || b <= 0)
                return 0.0f;
            if (p >= b && p % b == 0)
                return packetMs - blockMs;
            if (p < b && b % p == 0)
                return 0.5f * (packetMs - blockMs);
            return packetMs - 0.5f * blockMs;
        }

        // input jack -> packet sent, what this player sends as captureMs (never 0: 0 means "unknown")
        inline float captureMs (const LocalLatency& me, const LatencyOptions& o) noexcept
        {
            const float hold = sendHoldMs (msOf (packetFrames (o, me.blockMs)), me.blockMs);
            // raw input leaves before the chain: the friend's copy of it adds the chain there
            const float chain = o.sendMode == SendMode::rawInput ? 0.0f : me.chainMs;
            return juce::jmax (0.01f, me.inOrGuess() + hold + chain);
        }
    }

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
            float captureMs = 0.0f;     // their side as counted in totalMs (their figure, or the fallback for an older friend)
            float lossPercent = 0.0f, driftPpm = 0.0f, levelDb = -100.0f;
            bool hasTrack = false, compression = false;
            SendMode sendMode = SendMode::finishedSound;
        };
        Stats statsFor (PeerId) const;
        void setPeerCaptureMs (PeerId, float ms);           // their Network::PeerSnapshot::captureMs (0 = unknown)

        // This player's own latency parts (message thread): the processor's sound card figures, its chain, and
        // the block the audio thread last mixed with (the prepared block size until a session block ran).
        LocalLatency localLatency (ClodScopeProcessor&) const;
        float lastBlockMs() const noexcept;                 // 0 before the first block in a session

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
