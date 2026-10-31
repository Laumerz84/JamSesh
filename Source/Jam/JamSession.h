#pragma once

#include "JamTypes.h"
#include "JamReach.h"
#include <juce_events/juce_events.h>
#include <memory>

class ClodScopeProcessor;
struct ViewFeed;    // UI/ViewFeed.h
struct ViewState;   // Jam/ViewState.h

namespace jam
{
    class Network;   // Jam/JamNet.h     (owner: net soldier)
    class Engine;    // Jam/JamEngine.h  (owner: stream soldier)
    class Replicas;  // Jam/JamReplica.h (owner: net soldier)
    class Port;      // Jam/JamPort.h    (owner: stream soldier)

    // The one Jam Sesh per process: every Clod Scope instance in an FL project, and the standalone app,
    // share it. Hold it with juce::SharedResourcePointer<jam::Session>; it lives while anyone holds it.
    //
    // Threads: control calls (host / join / leave / options / volumes) on the message thread; the audio
    // engine on the audio thread; the network on its own threads. Change messages (status, players,
    // options) arrive on the message thread.
    //
    // Contract file (owner: lead; changes through the board). JamSession.cpp: net soldier.
    class Session : public juce::ChangeBroadcaster
    {
    public:
        Session();
        ~Session() override;

        // ---- control (message thread) ----
        bool host (const juce::String& playerName, int port, juce::String& error);
        bool join (const juce::String& code, const juce::String& playerName, juce::String& error);
        void leave();

        Status status() const;
        juce::String statusText() const;        // one line for the UI, e.g. "Hosting on port 47800 - 2 friends in"
        juce::String sessionCode() const;       // what friends type to join (empty unless in a session)
        ReachInfo reachInfo() const;            // host: can friends over the internet join? (JamReach.h)
        bool isHost() const;
        PeerId selfId() const;

        std::vector<PeerInfo> players() const;  // everyone, self first
        void setPeerVolume (PeerId, float gain);
        void setPeerMuted (PeerId, bool muted);

        LatencyOptions options() const;
        void setOptions (const LatencyOptions&);

        double sessionBpm() const;              // the tempo everyone shares (one-bar mode); the host's tempo

        // ---- what the tiles draw (message thread) ----
        ViewFeed* feedFor (PeerId);             // that player's sound (nullptr if unknown); self = this plugin's own
        bool viewStateFor (PeerId, ViewState& out) const;   // the view they have open, with its settings

        // ---- parts ----
        Network& network();
        Engine& engine();
        Replicas& replicas();

        // ---- plugin instances (called by jam::Port, message thread) ----
        void attach (Port&);
        void detach (Port&);
        Port* playerPort() const;               // the instance that sends this player's sound and plays the friends
        Port* trackPort() const;                // the instance (if any) that sends a backing track

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Session)
    };
}
