#include "JamSession.h"
#include "JamNet.h"
#include "JamEngine.h"
#include "JamReplica.h"
#include "JamPort.h"
#include "JamMetronome.h"
#include "ViewState.h"
#include "../UI/ViewFeed.h"
#include "../PluginProcessor.h"
#include "../Synth/ChordName.h"
#include <map>
#include <set>
#include <type_traits>

// Owner: net soldier.
//
// The facade the UI and the plugin instances use. A message-thread timer (30 Hz) does the session's
// message-thread work: tells the engine about players who came and went, sends this player's view
// state, metadata and (raw-input mode) plugin state and knob moves, hands friends' states to the
// replicas, and sends change messages. The audio thread only reads playerPort / trackPort (atomics).
namespace jam
{
namespace
{
    // Engine extras the stream soldier adds (network delay per friend, the host's tempo); called when present
    template <typename E, typename = void> struct HasNetworkMs : std::false_type {};
    template <typename E> struct HasNetworkMs<E, std::void_t<decltype (std::declval<E&>().setPeerNetworkMs (PeerId(), 0.0f))>> : std::true_type {};
    template <typename E, typename = void> struct HasSessionTempo : std::false_type {};
    template <typename E> struct HasSessionTempo<E, std::void_t<decltype (std::declval<E&>().setSessionTempo (0.0))>> : std::true_type {};

    template <typename E> void tellNetworkMs (E& e, PeerId p, float ms)
    {
        if constexpr (HasNetworkMs<E>::value) e.setPeerNetworkMs (p, ms);
        else juce::ignoreUnused (e, p, ms);
    }
    template <typename E> void tellSessionTempo (E& e, double bpm)
    {
        if constexpr (HasSessionTempo<E>::value) e.setSessionTempo (bpm);
        else juce::ignoreUnused (e, bpm);
    }

    float peakDb (const ScopeBuffer& scope, std::vector<float>& l, std::vector<float>& r)
    {
        scope.copyLatest (l.data(), r.data(), (int) l.size());
        float peak = 0.0f;
        for (size_t i = 0; i < l.size(); ++i)
            peak = juce::jmax (peak, std::abs (l[i]), std::abs (r[i]));
        return juce::jlimit (-100.0f, 24.0f, juce::Decibels::gainToDecibels (peak, -100.0f));
    }

    juce::String chordOf (ClodScopeProcessor& p)
    {
        const auto t = p.synth.tracker();
        if (t.chordMode && t.numNotes > 0)
            return chordname::symbol (t.notes.data(), t.numNotes);
        if (! t.chordMode && t.voiced && t.midiNote >= 0)
            return chordname::pitchClass (t.midiNote);
        return {};
    }

    // the plugin state without the parameter values (those travel as deltas): did anything else change?
    juce::MemoryBlock stateShape (ClodScopeProcessor& p)
    {
        auto tree = p.apvts.copyState();
        for (int i = tree.getNumChildren(); --i >= 0;)
            if (tree.getChild (i).hasType ("PARAM"))
                tree.removeChild (i, nullptr);
        juce::MemoryOutputStream out;
        tree.writeToStream (out);
        return out.getMemoryBlock();
    }
}

struct Session::Impl : private juce::Timer
{
    explicit Impl (Session& s) : owner (s), network (s), engine (s), replicas (s)
    {
        scopeL.resize (1024);
        scopeR.resize (1024);
        startTimerHz (30);
    }

    ~Impl() override { stopTimer(); }

    Session& owner;
    Network network;
    Engine engine;
    Replicas replicas;
    Metronome metronome;
    juce::Array<Port*> ports;
    std::atomic<Port*> player { nullptr }, track { nullptr };
    LatencyOptions options;
    std::map<PeerId, std::pair<float, bool>> mix;   // volume, muted (the UI reads them back)

    std::set<PeerId> known;                  // players the engine has been told about
    juce::uint32 lastChanges = 0xffffffffu;
    int tick = 0;
    std::vector<float> scopeL, scopeR;
    Network::LocalMeta meta;

    ViewState lastView;
    juce::MemoryBlock lastViewBytes;
    bool haveView = false;

    // raw-input mode, sender side
    ClodScopeProcessor* paramsOf = nullptr;
    std::vector<std::pair<juce::String, std::atomic<float>*>> paramPtrs;
    std::vector<float> lastSent;
    juce::MemoryBlock lastShape;
    bool rawSent = false;

    void refreshPorts()
    {
        Port* p = nullptr;
        Port* t = nullptr;
        for (auto* port : ports)
        {
            if (p == nullptr && port->role() == Role::player) p = port;
            if (t == nullptr && port->role() == Role::track)  t = port;
        }
        player.store (p, std::memory_order_release);
        track.store (t, std::memory_order_release);
    }

    bool inSession() const
    {
        const auto s = network.status();
        return s == Status::hosting || s == Status::connected;
    }

    void timerCallback() override
    {
        ++tick;
        refreshPorts();
        auto* pp = player.load();
        auto* proc = pp != nullptr ? &pp->processor() : nullptr;

        // players who came and went
        std::set<PeerId> now;
        const auto peers = network.peers();
        if (inSession())
            for (auto& p : peers)
                now.insert (p.id);
        for (auto id : known)
            if (now.count (id) == 0)
            {
                engine.removePeer (id);
                replicas.removePeer (id);
                mix.erase (id);
            }
        bool newcomers = false;
        for (auto id : now)
            if (known.count (id) == 0)
            {
                engine.addPeer (id);
                if (auto it = mix.find (id); it != mix.end())
                {
                    engine.setPeerVolume (id, it->second.first);
                    engine.setPeerMuted (id, it->second.second);
                }
                if (haveView)
                    network.sendViewState (lastViewBytes, id);
                if (options.sendMode == SendMode::rawInput && proc != nullptr && rawSent)
                {
                    juce::MemoryBlock state;
                    proc->getStateInformation (state);
                    network.sendPluginState (state, id);
                }
                newcomers = true;
            }
        known = now;

        if (inSession())
        {
            for (auto& p : peers)
            {
                tellNetworkMs (engine, p.id, p.rttMs * 0.5f);
                engine.setPeerCaptureMs (p.id, p.captureMs);
            }

            double bpm = 0, ppq = 0;
            bool playing = false;
            if (network.isHost())
            {
                auto* src = track.load() != nullptr ? track.load() : pp;
                if (src != nullptr)
                {
                    const auto t = src->lastTransport();
                    network.setLocalTransport (t.bpm, t.ppq, t.playing);
                }
            }
            tellSessionTempo (engine, network.sessionTransport (bpm, ppq, playing) ? bpm : 0.0);

            if (proc != nullptr)
            {
                // metadata ~10 Hz
                if (tick % 3 == 0)
                {
                    meta.levelDb = peakDb (proc->outputScope, scopeL, scopeR);
                    meta.chord = chordOf (*proc);
                    meta.sendMode = options.sendMode;
                    meta.compression = options.compression;
                    meta.hasTrack = track.load() != nullptr;
                    // input jack -> packet sent (sound card in + chain + send hold), follows device, chain and
                    // block changes within 100 ms
                    meta.captureMs = latency::captureMs (engine.localLatency (*proc), options);
                    network.setLocalMeta (meta);
                }

                // the view this player has open, twice a second when it changed
                if (tick % 15 == 0 || ! haveView)
                {
                    auto v = ViewState::capture (*proc);
                    if (! haveView || ! v.sameAs (lastView))
                    {
                        lastView = v;
                        lastViewBytes = v.toBytes();
                        haveView = true;
                        network.sendViewState (lastViewBytes);
                    }
                }

                sendRawState (*proc);
            }

            // friends' settings for their replicas
            PeerId from = noPeer;
            juce::MemoryBlock state;
            while (network.popPluginState (from, state))
            {
                replicas.stateArrived (from, state);
                ++statesIn;
            }
            std::vector<std::pair<juce::String, float>> params;
            while (network.popParams (from, params))
                for (auto& [id, v] : params)
                {
                    replicas.parameterArrived (from, id, v);
                    ++paramsIn;
                }
        }
        else
        {
            haveView = false;
            rawSent = false;
        }

        updateMetronome (pp);

        if (tick % 30 == 0)
            writeNetLog();

        // change messages: on every status / player-list change, and ~7 Hz in a session for the numbers
        const auto c = network.changeCount();
        if (c != lastChanges || newcomers || (inSession() && tick % 4 == 0))
        {
            lastChanges = c;
            owner.sendChangeMessage();
        }
    }

    // The shared metronome (Jam/JamMetronome.h): a friend follows the host's grid (none from an older host: off);
    // the host leads it and sends it with the tempo; outside a sesh this computer runs its own on its tempo.
    void updateMetronome (Port* pp)
    {
        const auto st = network.status();
        BeatGrid g;
        if (st == Status::joining || st == Status::connected)
        {
            if (! network.sessionMetronome (g))
                g = {};
        }
        else
        {
            double bpm = 0.0, ppq = 0.0;
            bool playing = false;
            if (st != Status::hosting || ! network.sessionTransport (bpm, ppq, playing))   // host: the tempo it sends
            {
                auto* src = track.load() != nullptr ? track.load() : pp;
                bpm = src != nullptr ? src->lastTransport().bpm : 120.0;
            }
            g = metronome.ownGrid (bpm, network.sessionTime());
            if (st == Status::hosting)
                network.setLocalMetronome (g);
        }
        metronome.publish (g);
    }

    // test only: CLOD_JAM_NETLOG=<dir> appends one line a second to <dir>/net-<pid>.log (status, clock,
    // every friend's path and round trip, counters, replica traffic)
    int statesIn = 0, paramsIn = 0;
    juce::File logFile;
    bool logChecked = false;

    void writeNetLog()
    {
        if (! logChecked)
        {
            logChecked = true;
            const auto dir = juce::SystemStats::getEnvironmentVariable ("CLOD_JAM_NETLOG", {});
            if (dir.isNotEmpty())
                logFile = juce::File (dir).getChildFile ("net-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt()) + ".log");
        }
        if (logFile == juce::File() || network.status() == Status::idle)
            return;

        const auto c = network.counters();
        juce::String line;
        line << juce::Time::getCurrentTime().toString (false, true, true, true) << " self=" << (int) network.selfId()
             << " status=\"" << network.statusText() << "\" clockOffsetMs=" << juce::String (network.clockOffsetMs(), 3)
             << " synced=" << (network.clockSynced() ? 1 : 0) << " driftPpm=" << juce::String (network.clockDriftPpm(), 1)
             << " sessionBpm=" << juce::String (owner.sessionBpm(), 1)
             << " sent=" << (juce::int64) c.sent << " recv=" << (juce::int64) c.received << " relayedByMe=" << (juce::int64) c.relayed
             << " resent=" << (juce::int64) c.resent << " dropped=" << (juce::int64) c.dropped
             << " audioOut=" << (juce::int64) c.audioOut << " audioIn=" << (juce::int64) c.audioIn
             << " statesIn=" << statesIn << " paramsIn=" << paramsIn;
        for (auto& p : network.peers())
        {
            juce::MemoryBlock vb;
            line << " | peer " << (int) p.id << " \"" << p.name << "\"" << (p.isHost ? " host" : "")
                 << (p.relayed ? " RELAYED" : " direct") << (p.connected ? "" : " LOST") << " rttMs=" << juce::String (p.rttMs, 2)
                 << " levelDb=" << juce::String (p.levelDb, 1) << " chord=" << (p.chord.isEmpty() ? "-" : p.chord)
                 << " mode=" << (p.sendMode == SendMode::rawInput ? "raw" : "finished")
                 << " view=" << (network.viewStateFor (p.id, vb) ? (int) vb.getSize() : 0) << "B";
        }
        logFile.appendText (line + "\n");
    }

    // raw-input mode: the full state when it starts, when something besides a knob changed (~300 ms
    // debounce) and for every newcomer; knob moves as coalesced deltas at 30 Hz
    void sendRawState (ClodScopeProcessor& proc)
    {
        if (options.sendMode != SendMode::rawInput)
        {
            rawSent = false;
            return;
        }

        if (paramsOf != &proc)
        {
            paramsOf = &proc;
            paramPtrs.clear();
            for (auto* p : proc.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                    if (auto* raw = proc.apvts.getRawParameterValue (rp->getParameterID()))
                        paramPtrs.emplace_back (rp->getParameterID(), raw);
            lastSent.assign (paramPtrs.size(), 0.0f);
            rawSent = false;
        }

        if (! rawSent || tick % 9 == 0)
        {
            auto shape = stateShape (proc);
            if (! rawSent || shape != lastShape)
            {
                lastShape = std::move (shape);
                juce::MemoryBlock state;
                proc.getStateInformation (state);
                for (size_t i = 0; i < paramPtrs.size(); ++i)
                    lastSent[i] = paramPtrs[i].second->load();
                network.sendPluginState (state);
                rawSent = true;
                return;
            }
        }

        std::vector<std::pair<juce::String, float>> moved;
        for (size_t i = 0; i < paramPtrs.size(); ++i)
        {
            const float v = paramPtrs[i].second->load();
            if (v != lastSent[i])
            {
                lastSent[i] = v;
                moved.emplace_back (paramPtrs[i].first, v);
            }
        }
        if (! moved.empty())
            network.sendParams (moved);
    }
};

Session::Session() : impl (std::make_unique<Impl> (*this))
{
    impl->network.setAudioSink (&impl->engine);
}

Session::~Session()
{
    impl->network.stop();
    impl->network.setAudioSink (nullptr);
}

bool Session::host (const juce::String& playerName, int port, juce::String& error)
{
    const bool ok = impl->network.startHost (playerName, port, error);
    sendChangeMessage();
    return ok;
}

bool Session::join (const juce::String& code, const juce::String& playerName, juce::String& error)
{
    const bool ok = impl->network.startJoin (code, playerName, error);
    sendChangeMessage();
    return ok;
}

void Session::leave()
{
    impl->network.stop();
    for (auto id : impl->known)
    {
        impl->engine.removePeer (id);
        impl->replicas.removePeer (id);
    }
    impl->known.clear();
    sendChangeMessage();
}

Status Session::status() const { return impl->network.status(); }
juce::String Session::statusText() const { return impl->network.statusText(); }
juce::String Session::sessionCode() const { return impl->network.sessionCode(); }
ReachInfo Session::reachInfo() const { return impl->network.reachInfo(); }
bool Session::isHost() const { return impl->network.isHost(); }
PeerId Session::selfId() const { return impl->network.selfId(); }

std::vector<PeerInfo> Session::players() const
{
    auto& m = *impl;
    std::vector<PeerInfo> out;

    PeerInfo me;
    me.id = m.network.selfId();
    me.name = m.network.selfName();
    if (me.name.isEmpty())
        me.name = "You";
    me.colour = me.id != noPeer ? m.network.selfColour() : juce::Colour (0xffff8a3d);
    me.isSelf = true;
    me.isHost = m.network.isHost();
    me.connected = true;
    me.hasTrack = m.track.load() != nullptr;
    me.sendMode = m.options.sendMode;
    me.compression = m.options.compression;
    me.levelDb = m.meta.levelDb;
    me.chord = m.meta.chord;
    out.push_back (me);

    if (! m.inSession())
        return out;

    for (auto& p : m.network.peers())
    {
        const auto s = m.engine.statsFor (p.id);
        PeerInfo i;
        i.id = p.id;
        i.name = p.name;
        i.colour = p.colour;
        i.isHost = p.isHost;
        i.connected = p.connected;
        i.relayed = p.relayed;
        i.hasTrack = p.hasTrack || s.hasTrack;
        i.sendMode = p.sendMode;
        i.compression = p.compression;
        i.pingMs = p.rttMs;
        i.captureMs = p.captureMs;
        i.networkMs = s.networkMs > 0.0f ? s.networkMs : p.rttMs * 0.5f;
        i.jitterMs = s.jitterMs;
        i.bufferMs = s.bufferMs;
        i.totalMs = s.totalMs;
        i.lossPercent = s.lossPercent;
        i.driftPpm = s.driftPpm;
        i.levelDb = s.levelDb > -100.0f ? s.levelDb : p.levelDb;
        i.chord = p.chord;
        if (auto it = m.mix.find (p.id); it != m.mix.end())
        {
            i.volume = it->second.first;
            i.muted = it->second.second;
        }
        out.push_back (i);
    }
    return out;
}

void Session::setPeerVolume (PeerId p, float g)
{
    auto it = impl->mix.find (p);
    impl->mix[p] = { g, it != impl->mix.end() && it->second.second };
    impl->engine.setPeerVolume (p, g);
    sendChangeMessage();
}

void Session::setPeerMuted (PeerId p, bool muted)
{
    auto it = impl->mix.find (p);
    impl->mix[p] = { it != impl->mix.end() ? it->second.first : 1.0f, muted };
    impl->engine.setPeerMuted (p, muted);
    sendChangeMessage();
}

LatencyOptions Session::options() const { return impl->options; }

void Session::setOptions (const LatencyOptions& o)
{
    impl->options = o;
    impl->engine.setOptions (o);
    sendChangeMessage();
}

double Session::sessionBpm() const
{
    double bpm = 0, ppq = 0;
    bool playing = false;
    if (impl->network.sessionTransport (bpm, ppq, playing) && bpm > 0)
        return bpm;
    if (auto* p = impl->player.load())
        return p->processor().fallbackBpm.load();
    return 120.0;
}

ViewFeed* Session::feedFor (PeerId p) { return impl->engine.feedFor (p); }

bool Session::viewStateFor (PeerId p, ViewState& out) const
{
    auto& m = *impl;
    if (p == m.network.selfId() || p == noPeer)
    {
        if (auto* port = m.player.load())
        {
            out = ViewState::capture (port->processor());
            return true;
        }
        return false;
    }
    juce::MemoryBlock bytes;
    return m.network.viewStateFor (p, bytes) && ViewState::fromBytes (bytes.getData(), bytes.getSize(), out);
}

Network& Session::network() { return impl->network; }
Engine& Session::engine() { return impl->engine; }
Replicas& Session::replicas() { return impl->replicas; }
Metronome& Session::metronome() { return impl->metronome; }

void Session::attach (Port& p)
{
    impl->ports.addIfNotAlreadyThere (&p);
    impl->refreshPorts();
}

void Session::detach (Port& p)
{
    impl->ports.removeFirstMatchingValue (&p);
    impl->refreshPorts();
    if (impl->paramsOf == &p.processor())
        impl->paramsOf = nullptr;
}

Port* Session::playerPort() const { return impl->player.load (std::memory_order_acquire); }
Port* Session::trackPort() const { return impl->track.load (std::memory_order_acquire); }
}
