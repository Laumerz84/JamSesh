#pragma once

#include "JamTypes.h"
#include "JamReach.h"
#include <memory>
#include <vector>

// The network side of Jam Sesh: UDP sockets, joining, the player list, ping and the shared session
// clock, and every non-audio message (names, view states, chords, settings for raw-input replicas).
//
// Owner: net soldier. The three members marked CONTRACT are used by the audio engine; everything
// else is yours to shape.
//
// One UDP socket (IPv4) and one network thread (receive, pings, heartbeats, resends, join retries).
// Topology: peer-to-peer mesh. The host admits players and tells everyone the others' endpoints;
// everyone pings everyone directly (which also opens most home routers); a pair that cannot reach
// each other within ~3 s sends through the host instead (relay), and goes back to direct when it can.
//
// A host also runs jam::Reach (JamReach.h), which adds the internet address to the session code and
// asks the router to forward the port.
//
// Test hooks (environment): CLOD_JAM_BIND=127.0.0.1 binds the socket to that address only (and puts
// only it in the session code, without jam::Reach). CLOD_JAM_REACH="stun=HOST:PORT,router=URL" points
// jam::Reach at a fake STUN server and router (and runs it even with CLOD_JAM_BIND). CLOD_JAM_NETSIM="delay=30,jitter=5,loss=1,reorder=1,nodirect=1"
// simulates a network on the receive path: one-way delay ms, +-jitter ms, loss %, reorder %, and
// nodirect=1 drops every packet that does not come from the host's address (forces the relay).
namespace jam
{
    class Session;

    // CONTRACT: receives audio packets (implemented by jam::Engine)
    struct AudioPacketSink
    {
        virtual ~AudioPacketSink() = default;
        // Network receive thread. data/size = exactly what the sender passed to Network::sendAudio.
        // arrivalSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001 when it arrived.
        virtual void audioPacketArrived (PeerId from, const juce::uint8* data, int size, double arrivalSeconds) = 0;
    };

    class Network
    {
    public:
        explicit Network (Session&);
        ~Network();

        // CONTRACT: where incoming audio packets go
        void setAudioSink (AudioPacketSink*);

        // CONTRACT: any thread, including the audio thread; non-blocking, no allocation. Sends the payload
        // to every connected player (target = noPeer) or to one player. At most maxAudioPayload bytes.
        void sendAudio (const void* payload, int size, PeerId target = noPeer) noexcept;
        static int maxAudioPayload() noexcept;   // 4200: larger payloads are dropped (keep it under ~1400: fragments)

        // CONTRACT: seconds on the host's clock, the same on every computer in the session (to ~1 ms).
        // Before a session it is this computer's clock.
        double sessionTime() const noexcept;

        // ---- session control (message thread; jam::Session calls these) ----
        bool startHost (const juce::String& name, int port, juce::String& error);
        bool startJoin (const juce::String& code, const juce::String& name, juce::String& error);
        void stop();                                  // says bye, closes the socket

        Status status() const;
        juce::String statusText() const;
        juce::String sessionCode() const;            // "JAM-XXXX-..." (host: right away; joiner: once in)
        juce::String plainCode() const;              // "ip,ip:port#key"
        ReachInfo reachInfo() const;                 // host: can friends over the internet join? (JamReach.h)
        bool isHost() const;
        PeerId selfId() const;
        juce::Colour selfColour() const;
        juce::String selfName() const;
        int localPort() const;                       // the bound UDP port (-1 when not in a session)
        juce::uint32 changeCount() const noexcept;   // bumps when status or the player list changes

        struct PeerSnapshot
        {
            PeerId id = noPeer;
            juce::String name, chord;
            juce::Colour colour;
            bool isHost = false, connected = false, relayed = false;
            bool hasTrack = false, compression = false;
            SendMode sendMode = SendMode::finishedSound;
            float rttMs = 0.0f;                      // round trip on the path in use (direct or via the host)
            float levelDb = -100.0f;
        };
        std::vector<PeerSnapshot> peers() const;     // everyone but self, by id

        // clock (any thread)
        bool clockSynced() const noexcept;           // true for the host; joiners after a few pings
        float clockOffsetMs() const noexcept;        // host clock minus this computer's, now
        float clockDriftPpm() const noexcept;

        // ---- what this player sends (message thread) ----
        struct LocalMeta
        {
            float levelDb = -100.0f;
            juce::String chord;
            SendMode sendMode = SendMode::finishedSound;
            bool compression = false, hasTrack = false;
        };
        void setLocalMeta (const LocalMeta&);                         // sent ~10 Hz by the network thread
        void setLocalTransport (double bpm, double ppq, bool playing); // the host sends it as the session tempo
        void sendViewState (const juce::MemoryBlock&, PeerId target = noPeer);     // reliable
        void sendPluginState (const juce::MemoryBlock&, PeerId target = noPeer);   // reliable, chunked
        void sendParams (const std::vector<std::pair<juce::String, float>>&, PeerId target = noPeer);   // reliable

        // ---- what arrived (message thread) ----
        bool viewStateFor (PeerId, juce::MemoryBlock& out) const;
        bool popPluginState (PeerId& from, juce::MemoryBlock& state);                      // completed transfers, in order
        bool popParams (PeerId& from, std::vector<std::pair<juce::String, float>>& params);
        // the host's tempo now (joiners: extrapolated on the session clock); false before any arrived
        bool sessionTransport (double& bpm, double& ppqNow, bool& playing) const;

        // ---- numbers for tests / the UI ----
        struct Counters { juce::uint64 sent = 0, received = 0, relayed = 0, resent = 0, dropped = 0, audioIn = 0, audioOut = 0; };
        Counters counters() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Network)
    };
}
