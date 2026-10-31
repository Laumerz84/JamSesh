#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #include <mstcpip.h>
 #include <avrt.h>
 #pragma comment (lib, "avrt.lib")
 #ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW (IOC_VENDOR, 12)
 #endif
#else
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
 #include <fcntl.h>
#endif

#include "JamNet.h"
#include "JamSession.h"
#include "JamProtocol.h"
#include "JamClock.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

// Owner: net soldier. See JamNet.h for the shape and JamProtocol.h for the packets.
//
// Threads: the network thread receives and runs the service tick (pings, heartbeats, relay
// decisions, resends, join retries, metadata and tempo). Message-thread calls take `lock`; so does
// the network thread while it handles a packet. The audio thread never locks: sendAudio reads the
// routes (one atomic id + one atomic packed endpoint per player) and sends with sendto on a stack
// buffer; incoming audio goes to the sink before the lock is taken.
namespace jam
{
namespace
{
    using namespace protocol;

    struct Endpoint
    {
        juce::uint32 ip = 0;   // host byte order
        int port = 0;

        bool valid() const noexcept { return ip != 0 && port > 0; }
        bool operator== (const Endpoint& o) const noexcept { return ip == o.ip && port == o.port; }
        bool operator!= (const Endpoint& o) const noexcept { return ! operator== (o); }
        juce::uint64 pack (bool relayed) const noexcept
        {
            return ((juce::uint64) ip << 32) | ((juce::uint64) (port & 0xffff) << 16) | (relayed ? 1u : 0u);
        }
        static Endpoint unpack (juce::uint64 v) noexcept { return { (juce::uint32) (v >> 32), (int) ((v >> 16) & 0xffff) }; }
        juce::String toString() const { return ipToString (ip) + ":" + juce::String (port); }
    };

    const juce::uint32 playerColours[maxPlayers] = { 0xffff8a3d, 0xff3ddcff, 0xffff4fa8, 0xff9be15d, 0xffffd23d, 0xffa98bff };

    constexpr double peerTimeout = 3.5;      // nothing heard for this long: gone
    constexpr double directGrace = 3.0;      // a pair without a direct round trip for this long: relay
    constexpr double joinGiveUp = 10.0;

    struct NetSim
    {
        bool on = false, noDirect = false;
        double delay = 0, jitter = 0, loss = 0, reorder = 0;   // seconds, seconds, 0..1, 0..1

        static NetSim fromEnv()
        {
            NetSim s;
            const auto text = juce::SystemStats::getEnvironmentVariable ("CLOD_JAM_NETSIM", {});
            for (auto& item : juce::StringArray::fromTokens (text, ",; ", ""))
            {
                const auto k = item.upToFirstOccurrenceOf ("=", false, false).trim().toLowerCase();
                const double v = item.fromFirstOccurrenceOf ("=", false, false).trim().getDoubleValue();
                if (k == "delay")    s.delay = juce::jmax (0.0, v) * 0.001;
                if (k == "jitter")   s.jitter = juce::jmax (0.0, v) * 0.001;
                if (k == "loss")     s.loss = juce::jlimit (0.0, 100.0, v) * 0.01;
                if (k == "reorder")  s.reorder = juce::jlimit (0.0, 100.0, v) * 0.01;
                if (k == "nodirect") s.noDirect = v != 0.0;
            }
            s.on = s.delay > 0 || s.jitter > 0 || s.loss > 0 || s.reorder > 0;
            return s;
        }
    };

    // a big transfer (plugin state, view state) in reliable chunks
    struct Assembly
    {
        juce::uint32 id = 0, applied = 0, total = 0, extra = 0;
        int count = 0, got = 0;
        juce::MemoryBlock buf;
        std::vector<bool> have;
    };

    struct Peer
    {
        PeerId id = noPeer;
        juce::String name, chord;
        juce::Colour colour;
        Endpoint ep;                          // where this player is reached directly
        bool isHost = false, relayed = false, hasTrack = false, compression = false;
        SendMode sendMode = SendMode::finishedSound;
        float levelDb = -100.0f;
        float captureMs = 0.0f;               // their input jack -> packet sent (metadata; 0 = not sent: older version)
        double added = 0, lastHeard = 0, lastDirectPong = 0, lastPing = 0, lastRelayPing = 0;
        double directRtt = 0, relayRtt = 0;   // seconds
        int slot = -1;

        juce::uint32 seen[256] {};            // reliable sequence numbers already handled
        int seenPos = 0;

        Assembly state, view;
        juce::MemoryBlock viewBytes;
        std::map<juce::String, std::pair<juce::uint32, float>> params;   // newest batch + value per parameter

        bool alreadySeen (juce::uint32 seq) noexcept
        {
            for (auto s : seen)
                if (s == seq)
                    return true;
            seen[seenPos] = seq;
            seenPos = (seenPos + 1) % (int) std::size (seen);
            return false;
        }
    };

    struct Pending
    {
        PeerId to = noPeer;
        juce::uint32 seq = 0;
        std::vector<juce::uint8> bytes;
        double next = 0;
        int tries = 0;
    };

    struct Out
    {
        juce::uint8 data[maxPacket];
        Writer w { data, maxPacket };
        Header h;
    };

   #ifdef _WIN32
    using RawSocket = SOCKET;

    // The network thread joins MMCSS "Pro Audio" (as audio threads do), so games' and the GUI's boosted
    // threads don't delay its wake-up for an arrived packet (that delay would read as jitter -> buffer).
    // Silent where MMCSS is unavailable (service stopped): the thread keeps its normal high priority.
    struct ProAudioThread
    {
        ProAudioThread() noexcept { DWORD task = 0; h = AvSetMmThreadCharacteristicsW (L"Pro Audio", &task); }
        ~ProAudioThread() { if (h != nullptr) AvRevertMmThreadCharacteristics (h); }
        HANDLE h = nullptr;
        JUCE_DECLARE_NON_COPYABLE (ProAudioThread)
    };

    // qWAVE: the socket's traffic to each friend marked as voice (Wi-Fi WMM puts it in the AC_VO queue:
    // less airtime wait under load; DSCP where the network honours it). Nothing in the packets changes.
    // qwave.dll is loaded at run time (some Windows Server installs lack it); every failure is silent.
    // The qWAVE calls go to a service and can take hundreds of ms (measured: 0.3-1.3 s stalls when the
    // network thread made them), so they run on their own worker thread: request() never waits.
    // request(): network thread. close(): after the network thread stopped, before the socket closes.
    class VoiceMarking
    {
    public:
        ~VoiceMarking()
        {
            close();
            if (dll != nullptr)
                FreeLibrary (dll);
        }

        void request (SOCKET s, juce::uint32 ip, int port)
        {
            const auto key = ((juce::uint64) ip << 16) | (juce::uint64) (port & 0xffff);
            if (std::find (requested.begin(), requested.end(), key) != requested.end() || requested.size() >= 32)
                return;
            requested.push_back (key);
            {
                const std::lock_guard<std::mutex> lk (mutex);
                queue.push_back ({ s, ip, port });
            }
            if (! worker.joinable())
                worker = std::thread ([this] { work(); });
            wake.notify_one();
        }

        void close() noexcept
        {
            {
                const std::lock_guard<std::mutex> lk (mutex);
                stop = true;
                queue.clear();
            }
            wake.notify_one();
            if (worker.joinable())
                worker.join();
            stop = false;
            if (handle != nullptr && closeFn != nullptr)
                closeFn (handle);   // removes the flows
            handle = nullptr;
            requested.clear();
        }

    private:
        struct Job { SOCKET s; juce::uint32 ip; int port; };

        void work()
        {
            SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;)
            {
                Job j {};
                {
                    std::unique_lock<std::mutex> lk (mutex);
                    wake.wait (lk, [this] { return stop || ! queue.empty(); });
                    if (stop)
                        return;
                    j = queue.front();
                    queue.erase (queue.begin());
                }
                if (! ready())
                    continue;
                sockaddr_in a {};
                a.sin_family = AF_INET;
                a.sin_port = htons ((unsigned short) j.port);
                a.sin_addr.s_addr = htonl (j.ip);
                unsigned long flow = 0;
                addFn (handle, j.s, (sockaddr*) &a, trafficTypeVoice, nonAdaptiveFlow, &flow);   // per destination (unconnected UDP)
            }
        }

        struct Version { unsigned short major, minor; };
        using CreateFn = BOOL (WINAPI*) (Version*, HANDLE*);
        using AddFn = BOOL (WINAPI*) (HANDLE, SOCKET, sockaddr*, int, DWORD, unsigned long*);
        using CloseFn = BOOL (WINAPI*) (HANDLE);
        static constexpr int trafficTypeVoice = 4;          // QOSTrafficTypeVoice
        static constexpr DWORD nonAdaptiveFlow = 2;         // QOS_NON_ADAPTIVE_FLOW: no network probing

        bool ready() noexcept
        {
            if (handle != nullptr)
                return true;
            if (failed)
                return false;
            if (dll == nullptr)
                dll = LoadLibraryW (L"qwave.dll");
            if (dll != nullptr)
            {
                createFn = (CreateFn) (void*) GetProcAddress (dll, "QOSCreateHandle");
                addFn = (AddFn) (void*) GetProcAddress (dll, "QOSAddSocketToFlow");
                closeFn = (CloseFn) (void*) GetProcAddress (dll, "QOSCloseHandle");
            }
            Version v { 1, 0 };
            HANDLE h = nullptr;
            if (createFn == nullptr || addFn == nullptr || closeFn == nullptr || ! createFn (&v, &h) || h == nullptr)
            {
                failed = true;
                return false;
            }
            handle = h;
            return true;
        }

        HMODULE dll = nullptr;
        CreateFn createFn = nullptr;
        AddFn addFn = nullptr;
        CloseFn closeFn = nullptr;
        HANDLE handle = nullptr;                    // worker thread (close() after joining it)
        bool failed = false;
        std::vector<juce::uint64> requested;        // network thread
        std::vector<Job> queue;
        std::mutex mutex;
        std::condition_variable wake;
        bool stop = false;
        std::thread worker;
    };
   #else
    using RawSocket = int;
   #endif
}

struct Network::Impl : private juce::Thread
{
    explicit Impl (Session& s) : juce::Thread ("Jam Sesh network"), session (s)
    {
        juce::ignoreUnused (session);
        sim = NetSim::fromEnv();
    }

    ~Impl() override { close (false); }

    Session& session;
    std::atomic<AudioPacketSink*> sink { nullptr };
    Clock clock;

    // ---- read by the audio thread (atomics only) ----
    std::unique_ptr<juce::DatagramSocket> socket;
    std::atomic<juce::int64> rawHandle { -1 };
    std::atomic<PeerId> self { noPeer };
    std::atomic<juce::uint64> key { 0 };
    std::atomic<bool> hosting { false };
    std::atomic<juce::uint32> seqCounter { 1 };
    std::atomic<juce::uint64> hostRoute { 0 };
    struct Route { std::atomic<PeerId> id { noPeer }; std::atomic<juce::uint64> ep { 0 }; };
    Route routes[maxPlayers];

    std::atomic<juce::uint64> nSent { 0 }, nReceived { 0 }, nRelayed { 0 }, nResent { 0 }, nDropped { 0 }, nAudioIn { 0 }, nAudioOut { 0 };
    std::atomic<juce::uint32> changes { 0 };

    // ---- everything else: under lock ----
    mutable juce::CriticalSection lock;
    Status st = Status::idle;
    juce::String error, name;
    juce::Colour colour;
    CodeInfo code;                    // host: its own addresses; joiner: the code it joined with
    int boundPort = -1;
    Endpoint hostEp;
    juce::String lastJoinName;        // host: the last player let in, to spot the same join arriving twice
    Endpoint lastJoinEp;
    double lastJoinAt = -10.0;

    // host: the internet address and the router's port (own thread; started and stopped without the lock)
    Reach internetReach { reachConfig() };

    // test hook: CLOD_JAM_REACH="stun=HOST:PORT,router=URL" points jam::Reach at a fake STUN server and a
    // fake router (tools/jam-test --reach); jam::Reach then runs even with CLOD_JAM_BIND
    static Reach::Config reachConfig()
    {
        Reach::Config c;
        for (auto& part : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("CLOD_JAM_REACH", {}), ",", ""))
        {
            const auto k = part.upToFirstOccurrenceOf ("=", false, false).trim();
            const auto v = part.fromFirstOccurrenceOf ("=", false, false).trim();
            if (k == "stun")   c.stunServers = juce::StringArray (v);
            if (k == "router") c.routerDescription = v;
        }
        return c;
    }
    double joinStarted = 0, lastJoinSend = 0;
    PeerId nextId = 2;
    juce::uint32 listVersion = 0, blobCounter = 0, paramBatch = 0;
    std::map<PeerId, Peer> peers;
    std::vector<Pending> pending;
    std::deque<std::pair<PeerId, juce::MemoryBlock>> inStates;
    std::deque<std::pair<PeerId, std::vector<std::pair<juce::String, float>>>> inParams;
    LocalMeta meta;
    double localBpm = 120.0, localPpq = 0.0;
    bool localPlaying = false;
    double hostBpm = 120.0, hostPpq = 0.0, hostTempoAt = 0.0;
    bool hostPlaying = false, haveTempo = false;
    BeatGrid localGrid, hostGrid;    // the shared metronome: what the host sends / what a joiner got
    double lastService = 0, lastMeta = 0, lastTempo = 0, lastMarkFlows = 0;
   #ifdef _WIN32
    VoiceMarking voice;
   #endif

    // ---- network simulator (test only; network thread) ----
    NetSim sim;
    juce::Random rng;
    std::multimap<double, std::pair<Endpoint, std::vector<juce::uint8>>> simQueue;

    //==============================================================================
    void changed() noexcept { changes.fetch_add (1, std::memory_order_relaxed); }

    bool rawSend (const Endpoint& to, const juce::uint8* d, int n) noexcept
    {
        const auto h = rawHandle.load (std::memory_order_acquire);
        if (h < 0 || ! to.valid() || n <= 0)
            return false;
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons ((unsigned short) to.port);
        a.sin_addr.s_addr = htonl (to.ip);
        const bool ok = ::sendto ((RawSocket) h, (const char*) d, n, 0, (const sockaddr*) &a, (int) sizeof (a)) == n;
        nSent.fetch_add (1, std::memory_order_relaxed);
        return ok;
    }

    // to the host, which forwards the packet to `target` (for pairs that can't reach each other)
    void relaySend (PeerId target, const juce::uint8* d, int n) noexcept
    {
        juce::uint8 buf[maxDatagram];
        Writer w (buf, (int) sizeof (buf));
        Header h;
        h.type = Type::relay;
        h.key = key.load (std::memory_order_relaxed);
        h.sender = self.load (std::memory_order_relaxed);
        h.seq = seqCounter.fetch_add (1, std::memory_order_relaxed);
        w.header (h);
        w.u32 (target);
        w.raw (d, n);
        if (w.ok)
            rawSend (Endpoint::unpack (hostRoute.load (std::memory_order_relaxed)), buf, w.pos);
    }

    void sendToPeer (const Peer& p, const juce::uint8* d, int n) noexcept
    {
        if (p.relayed && ! hosting.load() && ! p.isHost && hostEp.valid())
            relaySend (p.id, d, n);
        else
            rawSend (p.ep, d, n);
    }

    // answers go back the way the question came: straight to its address, or through the host
    void replyTo (const Peer& p, const Endpoint& src, const juce::uint8* d, int n) noexcept
    {
        if (! hosting.load() && ! p.isHost && src == hostEp)
            relaySend (p.id, d, n);
        else
            rawSend (src, d, n);
    }

    void begin (Out& o, Type t, juce::uint8 flags = 0) noexcept
    {
        o.h.type = t;
        o.h.flags = flags;
        o.h.key = key.load();
        o.h.sender = self.load();
        o.h.seq = seqCounter.fetch_add (1);
        o.w.header (o.h);
    }

    // lock held
    void sendReliable (Out& o, PeerId target, double now)
    {
        if (! o.w.ok)
            return;
        for (auto& [id, p] : peers)
        {
            if (target != noPeer && id != target)
                continue;
            Pending pe;
            pe.to = id;
            pe.seq = o.h.seq;
            pe.bytes.assign (o.data, o.data + o.w.pos);
            pe.next = now + resendInterval (p);
            pending.push_back (std::move (pe));
            sendToPeer (p, o.data, o.w.pos);
        }
    }

    void sendUnreliable (Out& o, PeerId target)
    {
        if (! o.w.ok)
            return;
        for (auto& [id, p] : peers)
            if (target == noPeer || id == target)
                sendToPeer (p, o.data, o.w.pos);
    }

    static double resendInterval (const Peer& p) noexcept
    {
        const double rtt = p.relayed ? p.relayRtt : p.directRtt;
        return juce::jlimit (0.08, 0.5, rtt * 2.0 + 0.03);
    }

    //==============================================================================
    void setRoute (Peer& p)
    {
        if (p.slot < 0)
            for (int i = 0; i < maxPlayers; ++i)
                if (routes[i].id.load() == noPeer)
                {
                    p.slot = i;
                    break;
                }
        if (p.slot < 0)
            return;
        routes[p.slot].ep.store (p.ep.pack (p.relayed && ! hosting.load() && ! p.isHost), std::memory_order_release);
        routes[p.slot].id.store (p.id, std::memory_order_release);
    }

    Peer& addPeer (PeerId id, const Endpoint& ep, double now)
    {
        auto& p = peers[id];
        p.id = id;
        p.ep = ep;
        p.added = p.lastHeard = now;
        p.isHost = id == hostPeer;
        setRoute (p);
        changed();
        return p;
    }

    void removePeer (PeerId id)
    {
        auto it = peers.find (id);
        if (it == peers.end())
            return;
        if (it->second.slot >= 0)
        {
            routes[it->second.slot].id.store (noPeer, std::memory_order_release);
            routes[it->second.slot].ep.store (0);
        }
        pending.erase (std::remove_if (pending.begin(), pending.end(), [id] (const Pending& pe) { return pe.to == id; }), pending.end());
        peers.erase (it);
        changed();
    }

    bool knownRoute (PeerId id) const noexcept
    {
        for (auto& r : routes)
            if (r.id.load (std::memory_order_acquire) == id)
                return true;
        return false;
    }

    Endpoint routeOf (PeerId id) const noexcept
    {
        for (auto& r : routes)
            if (r.id.load (std::memory_order_acquire) == id)
                return Endpoint::unpack (r.ep.load (std::memory_order_acquire));
        return {};
    }

    //==============================================================================
    // host: the player list with the endpoints the host sees, to everyone, on every change
    void broadcastPeerList (double now)
    {
        ++listVersion;
        Out o;
        begin (o, Type::peerList, reliable);
        o.w.u32 (listVersion);
        o.w.u8 ((juce::uint8) (peers.size() + 1));
        o.w.u32 (hostPeer); o.w.str (name, 40); o.w.u32 (colour.getARGB()); o.w.u32 (0); o.w.u16 (0);
        for (auto& [id, p] : peers)
        {
            o.w.u32 (id); o.w.str (p.name, 40); o.w.u32 (p.colour.getARGB()); o.w.u32 (p.ep.ip); o.w.u16 ((juce::uint16) p.ep.port);
        }
        sendReliable (o, noPeer, now);
    }

    void sendWelcome (const Peer& p)
    {
        Out o;
        begin (o, Type::welcome);
        o.w.u32 (p.id);
        o.w.u32 (p.colour.getARGB());
        o.w.str (name, 40);
        o.w.u32 (colour.getARGB());
        o.w.u32 (p.ep.ip);
        o.w.u16 ((juce::uint16) p.ep.port);
        rawSend (p.ep, o.data, o.w.pos);
    }

    void sendPing (Peer& p, bool viaRelay, double now)
    {
        Out o;
        begin (o, Type::ping, viaRelay ? (juce::uint8) protocol::viaRelay : (juce::uint8) 0);
        o.w.f64 (localSeconds());
        if (viaRelay)
        {
            relaySend (p.id, o.data, o.w.pos);
            p.lastRelayPing = now;
        }
        else
        {
            rawSend (p.ep, o.data, o.w.pos);
            p.lastPing = now;
        }
    }

    //==============================================================================
    // network thread: a datagram arrived (after the simulator, if on)
    void onDatagram (const juce::uint8* d, int n, const Endpoint& src, double now)
    {
        if (n < headerSize || n > maxDatagram)
        {
            nDropped.fetch_add (1);
            return;
        }
        Reader r (d, n);
        Header h;
        if (! r.header (h) || h.key != key.load() || h.sender == self.load()
             || (h.sender == noPeer) != (h.type == Type::joinRequest))   // only a joiner has no id yet
        {
            nDropped.fetch_add (1);
            return;
        }
        nReceived.fetch_add (1, std::memory_order_relaxed);

        if (sim.noDirect && ! hosting.load() && hostRoute.load() != 0 && src != Endpoint::unpack (hostRoute.load()))
            return;   // test: only the host's address gets through

        // the hot paths, without the lock
        if (h.type == Type::audio)
        {
            if (knownRoute (h.sender))
                if (auto* s = sink.load (std::memory_order_acquire))
                {
                    nAudioIn.fetch_add (1, std::memory_order_relaxed);
                    s->audioPacketArrived (h.sender, d + headerSize, n - headerSize, now);
                }
            return;
        }
        if (h.type == Type::relay)
        {
            if (! hosting.load() || ! knownRoute (h.sender))
                return;
            const PeerId target = r.u32();
            const int innerSize = r.remaining();
            const auto* inner = r.take (innerSize);
            if (inner == nullptr || innerSize < headerSize || innerSize > maxDatagram - headerSize - relayPrefix || target == self.load())
                return;
            Reader ir (inner, innerSize);
            Header ih;
            if (! ir.header (ih) || ih.key != h.key || ih.sender != h.sender)
                return;
            const auto to = routeOf (target);
            if (to.valid())
            {
                rawSend (to, inner, innerSize);
                nRelayed.fetch_add (1, std::memory_order_relaxed);
            }
            return;
        }

        const juce::ScopedLock sl (lock);
        handle (h, r, src, now);
    }

    // lock held
    void handle (const Header& h, Reader& r, const Endpoint& src, double now)
    {
        if (h.type == Type::joinRequest)
        {
            if (hosting.load())
                onJoinRequest (r, src, now);
            return;
        }

        if (st == Status::joining)
        {
            if (h.sender == hostPeer && h.type == Type::welcome)
                onWelcome (r, src, now);
            else if (h.sender == hostPeer && h.type == Type::reject)
            {
                error = r.str();
                st = Status::failed;
                changed();
            }
            return;   // anything reliable from the host is resent after the welcome
        }

        auto it = peers.find (h.sender);
        if (it == peers.end())
            return;
        auto& p = it->second;
        p.lastHeard = now;

        if ((h.flags & reliable) != 0)
        {
            Out o;
            begin (o, Type::ack);
            o.w.u32 (h.seq);
            replyTo (p, src, o.data, o.w.pos);
            if (p.alreadySeen (h.seq))
                return;
        }

        switch (h.type)
        {
            case Type::ping:
            {
                const double t0 = r.f64();
                if (! r.ok)
                    return;
                Out o;
                begin (o, Type::pong, h.flags & protocol::viaRelay);
                o.w.f64 (t0);
                o.w.f64 (now);
                o.w.f64 (localSeconds());
                replyTo (p, src, o.data, o.w.pos);
                break;
            }
            case Type::pong:
            {
                const double t0 = r.f64(), t1 = r.f64(), t2 = r.f64();
                if (! r.ok)
                    return;
                const double rtt = (now - t0) - (t2 - t1);
                if (rtt < 0.0 || rtt > 10.0)
                    return;
                if ((h.flags & protocol::viaRelay) != 0)
                    p.relayRtt = rtt;
                else
                {
                    p.directRtt = rtt;
                    p.lastDirectPong = now;
                }
                if (p.isHost && ! hosting.load() && (h.flags & protocol::viaRelay) == 0)
                    clock.addExchange (t0, t1, t2, now);
                break;
            }
            case Type::ack:
            {
                const auto seq = r.u32();
                pending.erase (std::remove_if (pending.begin(), pending.end(),
                                               [&] (const Pending& pe) { return pe.to == p.id && pe.seq == seq; }),
                               pending.end());
                break;
            }
            case Type::peerList:  if (p.isHost) onPeerList (r, now); break;
            case Type::bye:       onBye (p.id); break;
            case Type::metadata:
            {
                auto n = r.str();
                const auto c = r.u32();
                const float level = r.f32();
                auto chord = r.str();
                const auto mode = r.u8();
                const auto comp = r.u8();
                const auto track = r.u8();
                if (! r.ok)
                    return;
                // trailing fields (1.2.4+); older senders end here. Readers ignore what follows what they know.
                const float capture = r.remaining() >= 4 ? r.f32() : 0.0f;
                p.captureMs = juce::jlimit (0.0f, 1000.0f, capture);
                if (n.isNotEmpty() && n != p.name) { p.name = n; changed(); }
                if (c != 0) p.colour = juce::Colour (c);
                p.levelDb = juce::jlimit (-100.0f, 24.0f, level);
                p.chord = chord;
                p.sendMode = mode == 1 ? SendMode::rawInput : SendMode::finishedSound;
                p.compression = comp != 0;
                p.hasTrack = track != 0;
                break;
            }
            case Type::tempo:
            {
                const double bpm = r.f64(), ppq = r.f64();
                const bool playing = r.u8() != 0;
                const double at = r.f64();
                if (! r.ok || ! p.isHost || bpm < 20.0 || bpm > 999.0)
                    return;
                hostBpm = bpm; hostPpq = ppq; hostPlaying = playing; hostTempoAt = at;
                haveTempo = true;
                // trailing (1.4.0+): the metronome's beat grid; an older host sends none = off
                hostGrid = {};
                if (r.remaining() >= 18)
                {
                    const auto flags = r.u8();
                    const int beats = r.u8();
                    const double gridBpm = r.f64(), anchor = r.f64();
                    if (r.ok && beats >= 1 && beats <= 16 && gridBpm >= 20.0 && gridBpm <= 999.0)
                        hostGrid = { (flags & 1) != 0, beats, gridBpm, anchor };
                }
                break;
            }
            case Type::pluginState:
            case Type::viewState:
                onChunk (p, h.type, r);
                break;
            case Type::params:
            {
                const auto batch = r.u32();
                const int count = r.u16();
                std::vector<std::pair<juce::String, float>> list;
                for (int i = 0; i < count && r.ok; ++i)
                {
                    auto id = r.str();
                    const float v = r.f32();
                    if (! r.ok || id.isEmpty())
                        break;
                    auto& slot = p.params[id];
                    if (batch >= slot.first)
                    {
                        slot = { batch, v };
                        list.emplace_back (id, v);
                    }
                }
                if (! list.empty())
                    inParams.emplace_back (p.id, std::move (list));
                break;
            }
            default: break;
        }
    }

    void onJoinRequest (Reader& r, const Endpoint& src, double now)
    {
        auto who = r.str().trim().substring (0, 40);
        if (! r.ok)
            return;

        for (auto& [id, p] : peers)
            if (p.ep == src)
            {
                sendWelcome (p);   // our welcome got lost: again
                return;
            }

        // the same request coming in a second way (a player in the host's home also reaches the internet
        // address, and the router loops it back from another port): the first copy is already in
        if (who == lastJoinName && now - lastJoinAt < 1.0 && src != lastJoinEp)
            return;

        if ((int) peers.size() + 1 >= maxPlayers)
        {
            Out o;
            begin (o, Type::reject);
            o.w.str ("The sesh is full (" + juce::String (maxPlayers) + " players)");
            rawSend (src, o.data, o.w.pos);
            return;
        }

        const PeerId id = nextId++;
        auto& p = addPeer (id, src, now);
        p.name = who.isNotEmpty() ? who : "Player " + juce::String (id);
        for (auto c : playerColours)   // the first colour nobody has
        {
            bool used = colour.getARGB() == c;
            for (auto& [oid, op] : peers)
                used = used || (oid != id && op.colour.getARGB() == c);
            if (! used)
            {
                p.colour = juce::Colour (c);
                break;
            }
        }
        sendWelcome (p);
        broadcastPeerList (now);
        lastJoinName = who;
        lastJoinEp = src;
        lastJoinAt = now;
    }

    void onWelcome (Reader& r, const Endpoint& src, double now)
    {
        const PeerId id = r.u32();
        const auto myColour = r.u32();
        auto hostName = r.str();
        const auto hostColour = r.u32();
        r.u32(); r.u16();   // how the host sees us (for the UI later)
        if (! r.ok || id <= hostPeer)
            return;

        self.store (id);
        colour = juce::Colour (myColour);
        hostEp = src;
        hostRoute.store (src.pack (false));
        clock.reset();
        auto& h = addPeer (hostPeer, src, now);
        h.name = hostName;
        h.colour = juce::Colour (hostColour);
        code.addresses.removeFirstMatchingValue (src.ip);
        code.addresses.insert (0, src.ip);   // the address that worked goes first in our copy of the code
        st = Status::connected;
        changed();
    }

    void onPeerList (Reader& r, double now)
    {
        const auto listVer = r.u32();
        const int count = r.u8();
        if (! r.ok || listVer <= listVersion || count > maxPlayers)
            return;

        struct Entry { PeerId id; juce::String name; juce::uint32 colour; Endpoint ep; };
        std::vector<Entry> list;
        for (int i = 0; i < count; ++i)
        {
            Entry e;
            e.id = r.u32();
            e.name = r.str();
            e.colour = r.u32();
            e.ep.ip = r.u32();
            e.ep.port = r.u16();
            if (! r.ok)
                return;
            list.push_back (e);
        }
        listVersion = listVer;

        const auto me = self.load();
        for (auto& e : list)
        {
            if (e.id == me || e.id == noPeer)
                continue;
            auto it = peers.find (e.id);
            if (it == peers.end())
            {
                if (e.id == hostPeer || ! e.ep.valid())
                    continue;
                auto& p = addPeer (e.id, e.ep, now);
                p.name = e.name;
                p.colour = juce::Colour (e.colour);
                p.lastPing = 0;
                continue;
            }
            auto& p = it->second;
            if (p.name != e.name) { p.name = e.name; changed(); }
            p.colour = juce::Colour (e.colour);
            if (! p.isHost && e.ep.valid() && e.ep != p.ep && now - p.lastDirectPong > directGrace)
            {
                p.ep = e.ep;
                setRoute (p);
            }
        }

        std::vector<PeerId> gone;
        for (auto& [id, p] : peers)
        {
            if (p.isHost)
                continue;
            bool listed = false;
            for (auto& e : list)
                listed = listed || e.id == id;
            if (! listed)
                gone.push_back (id);
        }
        for (auto id : gone)
            removePeer (id);
    }

    void onBye (PeerId id)
    {
        if (id == hostPeer && ! hosting.load())
        {
            error = "The host ended the sesh";
            st = Status::failed;
            removePeer (id);
            return;
        }
        removePeer (id);
        if (hosting.load())
            broadcastPeerList (localSeconds());
    }

    void onChunk (Peer& p, Type type, Reader& r)
    {
        const auto id = r.u32();
        const int idx = r.u16();
        const int count = r.u16();
        const auto total = r.u32();
        const auto extra = r.u32();
        const int len = r.u16();
        const auto* bytes = r.take (len);
        if (bytes == nullptr || id == 0 || total > (juce::uint32) maxStateSize || count < 1
             || count != juce::jmax (1, (int) ((total + stateChunk - 1) / stateChunk)) || idx >= count)
            return;
        const int expected = juce::jmin (stateChunk, (int) total - idx * stateChunk);
        if (len != juce::jmax (0, expected))
            return;

        auto& a = type == Type::pluginState ? p.state : p.view;
        if (id <= a.applied || id < a.id)
            return;   // older than what we have
        if (id > a.id)
        {
            a.id = id;
            a.total = total;
            a.count = count;
            a.got = 0;
            a.extra = extra;
            a.buf.setSize (total, true);
            a.have.assign ((size_t) count, false);
        }
        if (a.have[(size_t) idx])
            return;
        a.have[(size_t) idx] = true;
        ++a.got;
        if (len > 0)
            a.buf.copyFrom (bytes, (size_t) idx * (size_t) stateChunk, (size_t) len);
        if (a.got < a.count)
            return;

        a.applied = a.id;
        if (type == Type::viewState)
        {
            p.viewBytes = a.buf;
            changed();
            return;
        }
        inStates.emplace_back (p.id, a.buf);
        // knobs moved after the sender captured this state go back on top of it
        std::vector<std::pair<juce::String, float>> newer;
        for (auto& [pid, bv] : p.params)
            if (bv.first > a.extra)
                newer.emplace_back (pid, bv.second);
        if (! newer.empty())
            inParams.emplace_back (p.id, std::move (newer));
    }

    void sendBlob (Type type, const juce::MemoryBlock& data, juce::uint32 extra, PeerId target, double now)
    {
        if (data.getSize() > (size_t) maxStateSize || peers.empty())
            return;
        const auto total = (juce::uint32) data.getSize();
        const int count = juce::jmax (1, (int) ((total + stateChunk - 1) / stateChunk));
        const auto id = ++blobCounter;
        for (int i = 0; i < count; ++i)
        {
            const int len = juce::jmax (0, juce::jmin (stateChunk, (int) total - i * stateChunk));
            Out o;
            begin (o, type, reliable);
            o.w.u32 (id);
            o.w.u16 ((juce::uint16) i);
            o.w.u16 ((juce::uint16) count);
            o.w.u32 (total);
            o.w.u32 (extra);
            o.w.u16 ((juce::uint16) len);
            o.w.raw (static_cast<const juce::uint8*> (data.getData()) + (size_t) i * stateChunk, len);
            sendReliable (o, target, now);
        }
    }

    //==============================================================================
    // network thread, every ~10 ms
    void service (double now)
    {
        const juce::ScopedLock sl (lock);

        if (st == Status::joining)
        {
            if (now - lastJoinSend > 0.25)
            {
                lastJoinSend = now;
                Out o;
                begin (o, Type::joinRequest);
                o.w.str (name, 40);
                for (auto ip : code.addresses)
                    rawSend ({ ip, code.port }, o.data, o.w.pos);
            }
            if (now - joinStarted > joinGiveUp)
            {
                juce::StringArray tried;
                for (auto ip : code.addresses)
                    tried.add (Endpoint { ip, code.port }.toString());
                error = "No answer from " + tried.joinIntoString (", ")
                      + ". The host's Jam Sesh page says whether friends over the internet can reach them; if not, "
                        "they forward the port, someone else hosts, or you both join the same VPN.";
                st = Status::failed;
                changed();
            }
            return;
        }
        if (st != Status::hosting && st != Status::connected)
            return;

        const bool amHost = hosting.load();
        std::vector<PeerId> timedOut;
        for (auto& [id, p] : peers)
        {
            // pings: clock + heartbeat with the host, round trips (and hole punching) with everyone
            double interval = 0.5;
            if (p.isHost)
                interval = clock.synced() && now - p.added > 2.0 ? 0.25 : 0.1;
            else if (! amHost && now - p.added < directGrace + 1.0)
                interval = 0.2;
            if (now - p.lastPing >= interval)
                sendPing (p, false, now);
            if (p.relayed && now - p.lastRelayPing >= 0.5)
                sendPing (p, true, now);

            // direct or through the host
            if (! amHost && ! p.isHost)
            {
                const bool directOk = p.lastDirectPong > 0 && now - p.lastDirectPong < directGrace;
                if (! p.relayed && ! directOk && now - p.added > directGrace)
                {
                    p.relayed = true;
                    setRoute (p);
                    changed();
                }
                else if (p.relayed && directOk)
                {
                    p.relayed = false;
                    setRoute (p);
                    changed();
                }
            }

            if (now - p.lastHeard > peerTimeout && (amHost || p.isHost))
                timedOut.push_back (id);
        }

        for (auto id : timedOut)
        {
            if (id == hostPeer && ! amHost)
            {
                error = "Lost the connection to the host";
                st = Status::failed;
                removePeer (id);
                return;
            }
            removePeer (id);
        }
        if (amHost && ! timedOut.empty())
            broadcastPeerList (now);

        // resends
        for (auto& pe : pending)
        {
            if (now < pe.next)
                continue;
            auto it = peers.find (pe.to);
            if (it == peers.end() || ++pe.tries > 40)
            {
                pe.to = noPeer;   // give up (removed below)
                continue;
            }
            sendToPeer (it->second, pe.bytes.data(), (int) pe.bytes.size());
            pe.next = now + resendInterval (it->second) * (pe.tries > 5 ? 2.0 : 1.0);
            nResent.fetch_add (1, std::memory_order_relaxed);
        }
        pending.erase (std::remove_if (pending.begin(), pending.end(), [] (const Pending& pe) { return pe.to == noPeer; }), pending.end());

        // metadata ~10 Hz
        if (now - lastMeta >= 0.1)
        {
            lastMeta = now;
            Out o;
            begin (o, Type::metadata);
            o.w.str (name, 40);
            o.w.u32 (colour.getARGB());
            o.w.f32 (meta.levelDb);
            o.w.str (meta.chord, 24);
            o.w.u8 (meta.sendMode == SendMode::rawInput ? 1 : 0);
            o.w.u8 (meta.compression ? 1 : 0);
            o.w.u8 (meta.hasTrack ? 1 : 0);
            o.w.f32 (meta.captureMs);    // trailing: 1.2.1-1.2.3 readers stop before it
            sendUnreliable (o, noPeer);
        }

        // the session tempo, from the host ~10 Hz
        if (amHost && now - lastTempo >= 0.1)
        {
            lastTempo = now;
            Out o;
            begin (o, Type::tempo);
            o.w.f64 (localBpm);
            o.w.f64 (localPpq);
            o.w.u8 (localPlaying ? 1 : 0);
            o.w.f64 (clock.toSession (now));
            o.w.u8 (localGrid.on ? 1 : 0);         // trailing: the metronome (1.3.0 readers stop before it)
            o.w.u8 ((juce::uint8) localGrid.beatsPerBar);
            o.w.f64 (localGrid.bpm);
            o.w.f64 (localGrid.anchor);
            sendUnreliable (o, noPeer);
        }
    }

    //==============================================================================
    void run() override
    {
       #ifdef _WIN32
        const ProAudioThread proAudio;
       #endif
        juce::uint8 buf[maxDatagram + 64];
        while (! threadShouldExit())
        {
            int wait = 5;
            if (! simQueue.empty())
                wait = juce::jlimit (0, 5, (int) ((simQueue.begin()->first - localSeconds()) * 1000.0));

            auto* s = socket.get();
            const int ready = s != nullptr ? s->waitUntilReady (true, wait) : -1;
            if (ready < 0)
                juce::Thread::sleep (2);

            if (ready > 0)
            {
                for (int i = 0; i < 256; ++i)
                {
                    sockaddr_in from {};
                   #ifdef _WIN32
                    int fromLen = (int) sizeof (from);
                   #else
                    socklen_t fromLen = sizeof (from);
                   #endif
                    const auto h = rawHandle.load();
                    if (h < 0)
                        break;
                    const int n = (int) ::recvfrom ((RawSocket) h, (char*) buf, (int) sizeof (buf), 0, (sockaddr*) &from, &fromLen);
                    if (n <= 0)
                        break;
                    const Endpoint src { ntohl (from.sin_addr.s_addr), (int) ntohs (from.sin_port) };
                    const double now = localSeconds();

                    if (! sim.on)
                    {
                        onDatagram (buf, n, src, now);
                        continue;
                    }
                    if (rng.nextDouble() < sim.loss)
                        continue;
                    double due = now + sim.delay + (rng.nextDouble() * 2.0 - 1.0) * sim.jitter;
                    if (rng.nextDouble() < sim.reorder)
                        due += 2.0 * sim.jitter + 0.004;   // overtaken by the next few packets
                    simQueue.emplace (juce::jmax (now, due), std::make_pair (src, std::vector<juce::uint8> (buf, buf + n)));
                }
            }

            const double now = localSeconds();
            while (! simQueue.empty() && simQueue.begin()->first <= now)
            {
                auto node = simQueue.extract (simQueue.begin());
                auto& [src, bytes] = node.mapped();
                onDatagram (bytes.data(), (int) bytes.size(), src, now);
            }

            if (now - lastMarkFlows >= 0.5)
            {
                lastMarkFlows = now;
                markVoiceFlows();
            }

            if (now - lastService >= 0.01)
            {
                lastService = now;
                service (now);
            }
        }
    }

    // qWAVE voice marking for every destination this socket sends to (network thread; the qWAVE calls
    // themselves run on VoiceMarking's worker)
    void markVoiceFlows()
    {
       #ifdef _WIN32
        const auto h = rawHandle.load();
        if (h < 0)
            return;
        Endpoint eps[16];
        int count = 0;
        {
            const juce::ScopedLock sl (lock);
            if (hostEp.valid())
                eps[count++] = hostEp;
            for (auto& [id, p] : peers)
                if (p.ep.valid() && count < (int) std::size (eps))
                    eps[count++] = p.ep;
        }
        for (int i = 0; i < count; ++i)
            voice.request ((SOCKET) h, eps[i].ip, eps[i].port);
       #endif
    }

    //==============================================================================
    bool open (int port, bool exclusive, juce::String& err)
    {
        close (false);
        socket = std::make_unique<juce::DatagramSocket> (false);
        const auto h = (RawSocket) socket->getRawSocketHandle();
        if (socket->getRawSocketHandle() < 0)
        {
            err = "Could not create a network socket";
            socket.reset();
            return false;
        }

       #ifdef _WIN32
        if (exclusive)   // two hosts on one port would steal each other's packets
        {
            BOOL no = FALSE, yes = TRUE;
            setsockopt (h, SOL_SOCKET, SO_REUSEADDR, (const char*) &no, sizeof (no));
            setsockopt (h, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*) &yes, sizeof (yes));
        }
       #else
        if (exclusive)   // the same on macOS: JUCE's SO_REUSEADDR off, so a second bind to the port fails
        {
            int no = 0;
            setsockopt (h, SOL_SOCKET, SO_REUSEADDR, &no, (socklen_t) sizeof (no));
        }
       #endif

        const auto bindAddr = juce::SystemStats::getEnvironmentVariable ("CLOD_JAM_BIND", {}).trim();
        if (! socket->bindToPort (port, bindAddr))
        {
            err = port > 0 ? "Port " + juce::String (port) + " is already in use (another sesh open?)"
                           : juce::String ("Could not open a network port");
            socket.reset();
            return false;
        }

        int bufSize = 1 << 20;
        setsockopt (h, SOL_SOCKET, SO_RCVBUF, (const char*) &bufSize, sizeof (bufSize));
        setsockopt (h, SOL_SOCKET, SO_SNDBUF, (const char*) &bufSize, sizeof (bufSize));
       #ifdef _WIN32
        u_long nonBlocking = 1;
        ioctlsocket (h, FIONBIO, &nonBlocking);
        BOOL report = FALSE;   // an ICMP "port unreachable" must not make the next recvfrom fail
        DWORD bytes = 0;
        WSAIoctl (h, SIO_UDP_CONNRESET, &report, sizeof (report), nullptr, 0, &bytes, nullptr, nullptr);
       #else
        fcntl (h, F_SETFL, fcntl (h, F_GETFL, 0) | O_NONBLOCK);
       #endif

        boundPort = socket->getBoundPort();
        rawHandle.store ((juce::int64) h);
        return true;
    }

    // jam::Reach found something (its own thread): the internet address joins the code, the UI updates
    void reachChanged()
    {
        const auto r = internetReach.info();
        const juce::ScopedLock sl (lock);
        if (st == Status::hosting && r.publicIp != 0 && ! code.addresses.contains (r.publicIp))
            code.addresses.add (r.publicIp);
        changed();
    }

    void close (bool sayBye)
    {
        internetReach.stop();   // removes the router's port mapping; never under the lock (its callback takes it)

        if (socket != nullptr && sayBye && self.load() != noPeer)
        {
            const juce::ScopedLock sl (lock);
            Out o;
            begin (o, Type::bye);
            for (int i = 0; i < 2; ++i)
                sendUnreliable (o, noPeer);
        }

        rawHandle.store (-1);
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            stopThread (2000);
        }
       #ifdef _WIN32
        voice.close();
       #endif
        if (socket != nullptr)
        {
            socket->shutdown();
            socket.reset();
        }

        const juce::ScopedLock sl (lock);
        for (auto& rt : routes)
        {
            rt.id.store (noPeer);
            rt.ep.store (0);
        }
        self.store (noPeer);
        key.store (0);
        hosting.store (false);
        hostRoute.store (0);
        peers.clear();
        pending.clear();
        inStates.clear();
        inParams.clear();
        simQueue.clear();
        hostEp = {};
        code = {};
        lastJoinName.clear();
        lastJoinAt = -10.0;
        boundPort = -1;
        nextId = 2;
        listVersion = 0;
        haveTempo = false;
        hostGrid = {};
        clock.reset();
        if (st != Status::failed || sayBye)
            st = Status::idle;
        changed();
    }

    void start()
    {
        lastService = lastMeta = lastTempo = lastMarkFlows = 0;
        startThread (juce::Thread::Priority::high);
    }

    static juce::Array<juce::uint32> codeAddresses()
    {
        const auto bindAddr = juce::SystemStats::getEnvironmentVariable ("CLOD_JAM_BIND", {}).trim();
        if (bindAddr.isNotEmpty() && bindAddr != "0.0.0.0")
            if (auto ip = parseIPv4 (bindAddr))
                return { ip };

        // LAN first, then VPNs (Tailscale's 100.64/10 and the like), then anything else
        juce::Array<std::pair<int, juce::uint32>> ranked;
        for (auto& a : juce::IPAddress::getAllAddresses (false))
        {
            const auto ip = parseIPv4 (a.toString());
            const auto b0 = ip >> 24, b1 = (ip >> 16) & 255;
            if (ip == 0 || b0 == 127 || (b0 == 169 && b1 == 254) || b0 >= 224)
                continue;
            int rank = 2;
            if (b0 == 192 && b1 == 168) rank = 0;
            else if (b0 == 10 || (b0 == 172 && b1 >= 16 && b1 < 32)) rank = 0;
            else if (b0 == 100 && b1 >= 64 && b1 < 128) rank = 1;
            ranked.add ({ rank, ip });
        }
        std::stable_sort (ranked.begin(), ranked.end(), [] (auto& x, auto& y) { return x.first < y.first; });
        juce::Array<juce::uint32> out;
        for (auto& [rank, ip] : ranked)
            if (out.size() < 4 && ! out.contains (ip))
                out.add (ip);
        if (out.isEmpty())
            out.add (0x7f000001);
        return out;
    }
};

//==============================================================================
Network::Network (Session& s) : impl (std::make_unique<Impl> (s)) {}
Network::~Network() = default;

void Network::setAudioSink (AudioPacketSink* s) { impl->sink.store (s, std::memory_order_release); }

int Network::maxAudioPayload() noexcept { return protocol::maxAudioPayload; }

void Network::sendAudio (const void* payload, int size, PeerId target) noexcept
{
    auto& m = *impl;
    const auto me = m.self.load (std::memory_order_acquire);
    if (me == noPeer || payload == nullptr || size <= 0 || size > protocol::maxAudioPayload)
        return;

    juce::uint8 pkt[protocol::maxAudioPayload + protocol::headerSize];
    protocol::Writer w (pkt, (int) sizeof (pkt));
    protocol::Header h;
    h.type = protocol::Type::audio;
    h.key = m.key.load (std::memory_order_relaxed);
    h.sender = me;
    h.seq = m.seqCounter.fetch_add (1, std::memory_order_relaxed);
    w.header (h);
    w.raw (payload, size);
    if (! w.ok)
        return;

    const bool amHost = m.hosting.load (std::memory_order_relaxed);
    for (auto& r : m.routes)
    {
        const auto id = r.id.load (std::memory_order_acquire);
        if (id == noPeer || id == me || (target != noPeer && id != target))
            continue;
        const auto ep = r.ep.load (std::memory_order_acquire);
        if ((ep & 1) != 0 && ! amHost)
            m.relaySend (id, pkt, w.pos);
        else
            m.rawSend (Endpoint::unpack (ep), pkt, w.pos);
        m.nAudioOut.fetch_add (1, std::memory_order_relaxed);
    }
}

double Network::sessionTime() const noexcept { return impl->clock.sessionNow(); }

bool Network::startHost (const juce::String& playerName, int port, juce::String& error)
{
    auto& m = *impl;
    if (! m.open (port > 0 ? port : defaultPort, true, error))
        return false;

    {
        const juce::ScopedLock sl (m.lock);
        juce::uint64 k = 0;
        while (k == 0)
            k = ((juce::uint64) (juce::uint32) juce::Random::getSystemRandom().nextInt() << 32)
              ^ (juce::uint64) (juce::uint32) juce::Random::getSystemRandom().nextInt()
              ^ (juce::uint64) juce::Time::getHighResolutionTicks();
        m.key.store (k);
        m.self.store (hostPeer);
        m.hosting.store (true);
        m.name = playerName.trim().substring (0, 40);
        if (m.name.isEmpty())
            m.name = "Host";
        m.colour = juce::Colour (playerColours[0]);
        m.code.addresses = Impl::codeAddresses();
        m.code.port = m.boundPort;
        m.code.key = k;
        m.error.clear();
        m.st = Status::hosting;
        m.changed();
        m.start();
    }

    // friends over the internet: the internet address for the code, and the router's port (tests stay local)
    if (juce::SystemStats::getEnvironmentVariable ("CLOD_JAM_BIND", {}).trim().isEmpty()
         || juce::SystemStats::getEnvironmentVariable ("CLOD_JAM_REACH", {}).isNotEmpty())
        m.internetReach.start (m.boundPort, [&m] { m.reachChanged(); });
    return true;
}

bool Network::startJoin (const juce::String& codeText, const juce::String& playerName, juce::String& error)
{
    auto& m = *impl;
    protocol::CodeInfo info;
    if (! protocol::parseCode (codeText, info))
    {
        error = "That sesh code doesn't look right";
        return false;
    }
    if (! m.open (0, false, error))
        return false;

    const juce::ScopedLock sl (m.lock);
    m.key.store (info.key);
    m.code = info;
    m.name = playerName.trim().substring (0, 40);
    if (m.name.isEmpty())
        m.name = "Player";
    m.colour = juce::Colour (playerColours[1]);
    m.error.clear();
    m.st = Status::joining;
    m.joinStarted = localSeconds();
    m.lastJoinSend = 0;
    m.changed();
    m.start();
    return true;
}

void Network::stop() { impl->close (true); }

Status Network::status() const
{
    const juce::ScopedLock sl (impl->lock);
    return impl->st;
}

juce::String Network::statusText() const
{
    auto& m = *impl;
    const juce::ScopedLock sl (m.lock);
    const int others = (int) m.peers.size();
    switch (m.st)
    {
        case Status::idle:    return "Not in a sesh";
        case Status::hosting: return "Hosting on port " + juce::String (m.boundPort) + " - "
                                   + (others == 0 ? juce::String ("waiting for friends")
                                                  : juce::String (others) + (others == 1 ? " friend in" : " friends in"));
        case Status::joining: return "Joining " + Endpoint { m.code.addresses.isEmpty() ? 0u : m.code.addresses[0], m.code.port }.toString() + "...";
        case Status::connected:
        {
            juce::String hostName = "the host";
            int relayed = 0;
            for (auto& [id, p] : m.peers)
            {
                if (p.isHost) hostName = p.name;
                if (p.relayed) ++relayed;
            }
            auto t = "In " + hostName + "'s sesh - " + juce::String (others + 1) + " players";
            if (relayed > 0)
                t << " (" << relayed << " through the host)";
            return t;
        }
        case Status::failed:  return m.error.isNotEmpty() ? m.error : juce::String ("Connection failed");
    }
    return {};
}

juce::String Network::sessionCode() const
{
    const juce::ScopedLock sl (impl->lock);
    if (impl->st != Status::hosting && impl->st != Status::connected)
        return {};
    return protocol::makeCode (impl->code);
}

juce::String Network::plainCode() const
{
    const juce::ScopedLock sl (impl->lock);
    if (impl->st != Status::hosting && impl->st != Status::connected)
        return {};
    juce::StringArray ips;
    for (auto ip : impl->code.addresses)
        ips.add (protocol::ipToString (ip));
    return ips.joinIntoString (",") + ":" + juce::String (impl->code.port) + "#"
         + juce::String::toHexString ((juce::int64) impl->code.key).paddedLeft ('0', 16);
}

ReachInfo Network::reachInfo() const { return impl->internetReach.info(); }

bool Network::isHost() const { return impl->hosting.load(); }
PeerId Network::selfId() const { return impl->self.load(); }
juce::Colour Network::selfColour() const { const juce::ScopedLock sl (impl->lock); return impl->colour; }
juce::String Network::selfName() const { const juce::ScopedLock sl (impl->lock); return impl->name; }
int Network::localPort() const { const juce::ScopedLock sl (impl->lock); return impl->boundPort; }
juce::uint32 Network::changeCount() const noexcept { return impl->changes.load (std::memory_order_relaxed); }

std::vector<Network::PeerSnapshot> Network::peers() const
{
    auto& m = *impl;
    const double now = localSeconds();
    const juce::ScopedLock sl (m.lock);
    std::vector<PeerSnapshot> out;
    for (auto& [id, p] : m.peers)
    {
        PeerSnapshot s;
        s.id = id;
        s.name = p.name;
        s.chord = p.chord;
        s.colour = p.colour;
        s.isHost = p.isHost;
        s.connected = now - p.lastHeard < 2.0;
        s.relayed = p.relayed;
        s.hasTrack = p.hasTrack;
        s.compression = p.compression;
        s.sendMode = p.sendMode;
        s.levelDb = p.levelDb;
        s.captureMs = p.captureMs;
        s.rttMs = (float) ((p.relayed ? p.relayRtt : p.directRtt) * 1000.0);
        out.push_back (s);
    }
    return out;
}

bool Network::clockSynced() const noexcept { return impl->hosting.load() || impl->clock.synced(); }
float Network::clockOffsetMs() const noexcept { return impl->clock.offsetMs(); }
float Network::clockDriftPpm() const noexcept { return impl->clock.driftPpm(); }

void Network::setLocalMeta (const LocalMeta& meta)
{
    const juce::ScopedLock sl (impl->lock);
    impl->meta = meta;
}

void Network::setLocalTransport (double bpm, double ppq, bool playing)
{
    const juce::ScopedLock sl (impl->lock);
    impl->localBpm = juce::jlimit (20.0, 999.0, bpm > 0 ? bpm : 120.0);
    impl->localPpq = ppq;
    impl->localPlaying = playing;
}

void Network::sendViewState (const juce::MemoryBlock& bytes, PeerId target)
{
    const juce::ScopedLock sl (impl->lock);
    impl->sendBlob (protocol::Type::viewState, bytes, 0, target, localSeconds());
}

void Network::sendPluginState (const juce::MemoryBlock& state, PeerId target)
{
    const juce::ScopedLock sl (impl->lock);
    impl->sendBlob (protocol::Type::pluginState, state, impl->paramBatch, target, localSeconds());
}

void Network::sendParams (const std::vector<std::pair<juce::String, float>>& params, PeerId target)
{
    auto& m = *impl;
    const juce::ScopedLock sl (m.lock);
    if (params.empty() || m.peers.empty())
        return;
    const auto batch = ++m.paramBatch;
    size_t i = 0;
    while (i < params.size())
    {
        Out o;
        m.begin (o, protocol::Type::params, protocol::reliable);
        o.w.u32 (batch);
        const int countPos = o.w.pos;
        o.w.u16 (0);
        int count = 0;
        // as many as fit in one packet (ids <= 64 bytes + value)
        while (i < params.size() && o.w.pos + 70 < protocol::maxPacket)
        {
            o.w.str (params[i].first, 64);
            o.w.f32 (params[i].second);
            ++count;
            ++i;
        }
        o.data[countPos] = (juce::uint8) count;
        o.data[countPos + 1] = (juce::uint8) (count >> 8);
        m.sendReliable (o, target, localSeconds());
    }
}

bool Network::viewStateFor (PeerId id, juce::MemoryBlock& out) const
{
    const juce::ScopedLock sl (impl->lock);
    auto it = impl->peers.find (id);
    if (it == impl->peers.end() || it->second.viewBytes.isEmpty())
        return false;
    out = it->second.viewBytes;
    return true;
}

bool Network::popPluginState (PeerId& from, juce::MemoryBlock& state)
{
    const juce::ScopedLock sl (impl->lock);
    if (impl->inStates.empty())
        return false;
    from = impl->inStates.front().first;
    state = std::move (impl->inStates.front().second);
    impl->inStates.pop_front();
    return true;
}

bool Network::popParams (PeerId& from, std::vector<std::pair<juce::String, float>>& params)
{
    const juce::ScopedLock sl (impl->lock);
    if (impl->inParams.empty())
        return false;
    from = impl->inParams.front().first;
    params = std::move (impl->inParams.front().second);
    impl->inParams.pop_front();
    return true;
}

bool Network::sessionTransport (double& bpm, double& ppqNow, bool& playing) const
{
    auto& m = *impl;
    const juce::ScopedLock sl (m.lock);
    if (m.hosting.load())
    {
        bpm = m.localBpm; ppqNow = m.localPpq; playing = m.localPlaying;
        return true;
    }
    if (! m.haveTempo)
        return false;
    bpm = m.hostBpm;
    playing = m.hostPlaying;
    ppqNow = m.hostPpq + (playing ? (m.clock.sessionNow() - m.hostTempoAt) * bpm / 60.0 : 0.0);
    return true;
}

void Network::setLocalMetronome (const BeatGrid& g)
{
    auto& m = *impl;
    const juce::ScopedLock sl (m.lock);
    const auto& o = m.localGrid;
    if (g.on != o.on || g.beatsPerBar != o.beatsPerBar || g.bpm != o.bpm || g.anchor != o.anchor)
        m.lastTempo = 0;   // friends hear the change with the next tempo message, at once
    m.localGrid = g;
}

bool Network::sessionMetronome (BeatGrid& g) const
{
    auto& m = *impl;
    const juce::ScopedLock sl (m.lock);
    if (m.hosting.load())
    {
        g = m.localGrid;
        return true;
    }
    if (! m.haveTempo)
        return false;
    g = m.hostGrid;
    return true;
}

Network::Counters Network::counters() const noexcept
{
    auto& m = *impl;
    Counters c;
    c.sent = m.nSent.load(); c.received = m.nReceived.load(); c.relayed = m.nRelayed.load();
    c.resent = m.nResent.load(); c.dropped = m.nDropped.load(); c.audioIn = m.nAudioIn.load(); c.audioOut = m.nAudioOut.load();
    return c;
}
}
