#include "JamEngine.h"
#include "JamSession.h"
#include "JamStream.h"
#include "JamReplica.h"
#include "JamPort.h"
#include "../UI/ViewFeed.h"
#include <array>

// Owner: stream soldier.
namespace jam
{
    namespace
    {
        constexpr int maxChunk = 1024;      // friends are mixed in chunks of at most this many samples
        constexpr int feedSize = 16384;     // per view-tile ring

        // A friend's sound for their view tile: what arrives here (after their replica in raw mode)
        struct PeerFeed : public ViewFeed
        {
            int copyLatest (bool output, float* left, float* right, int numSamples) const override
            {
                if (! output && hasRaw.load (std::memory_order_relaxed))
                    return raw.copyLatest (left, right, numSamples);
                return out.copyLatest (left, right, numSamples);
            }
            double sampleRate() const override { return rate.load (std::memory_order_relaxed); }

            FeedRing out, raw;
            std::atomic<double> rate { 48000.0 };
            std::atomic<bool> hasRaw { false };
        };

        struct PeerSlot
        {
            PeerSlot()
            {
                for (auto& s : streams)
                    s.allocate();
                feed.out.prepare (feedSize);
                feed.raw.prepare (feedSize);
                scratch.setSize (2, maxChunk);
                silence.setSize (2, maxChunk);
                silence.clear();
                midi.ensureSize (4096);
                merged.ensureSize (4096);
                for (int ch = 1; ch <= 16; ++ch)
                    notesOff.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
            }

            std::atomic<PeerId> id { noPeer };
            std::atomic<bool> active { false };
            ReceiveStream streams[2];               // 0: their sound (finished or raw), 1: their backing track
            std::atomic<float> volume { 1.0f }, networkMs { 0.0f }, levelDb { -100.0f };
            std::atomic<bool> muted { false };
            PeerFeed feed;

            // audio thread only
            juce::AudioBuffer<float> scratch, silence;
            juce::MidiBuffer midi, merged, notesOff;
            bool replicaFed = false;        // their replica may hold notes
            float env = 0.0f;
            float gainNow = 1.0f;
        };
    }

    struct Engine::Impl
    {
        explicit Impl (Session& s) : session (s) {}

        PeerSlot* find (PeerId id) const noexcept
        {
            for (auto& s : slots)
                if (auto* p = s.load (std::memory_order_acquire))
                    if (p->id.load (std::memory_order_relaxed) == id && p->active.load (std::memory_order_acquire))
                        return p;
            return nullptr;
        }

        Session& session;
        std::array<std::atomic<PeerSlot*>, maxPlayers> slots {};
        std::vector<std::unique_ptr<PeerSlot>> owned;      // message thread; freed with the engine

        std::atomic<bool> compression { false }, lossProtection { false }, autoJitter { true },
                          driftCorrection { true }, syncTrack { true }, oneBarMode { false }, rawMode { false };
        std::atomic<int> opusKbps { 128 }, packetSamples { 128 };
        std::atomic<float> jitterMs { 10.0f };

        std::atomic<double> sessionTempo { 0.0 }, syncDelay { 0.0 }, lastBlockMs { 0.0 };
        std::atomic<int> numActive { 0 };
        std::atomic_flag mixing = ATOMIC_FLAG_INIT;

        std::vector<std::pair<ClodScopeProcessor*, std::unique_ptr<ProcessorFeed>>> selfFeeds;
    };

    Engine::Engine (Session& s) : impl (std::make_unique<Impl> (s)) {}

    Engine::~Engine()
    {
        for (auto& s : impl->slots)
            s.store (nullptr);
    }

    void Engine::audioPacketArrived (PeerId from, const juce::uint8* data, int size, double arrivalSeconds)
    {
        PacketView v;
        if (! parsePacket (data, size, v))
            return;
        if (auto* p = impl->find (from))
            p->streams[v.h.kind == StreamKind::track ? 1 : 0].push (v, data, size, arrivalSeconds);
    }

    void Engine::addPeer (PeerId id)
    {
        if (id == noPeer || impl->find (id) != nullptr)
            return;

        // reuse their old slot, else a free one, else make one
        PeerSlot* slot = nullptr;
        for (auto& s : impl->slots)
            if (auto* p = s.load())
                if (! p->active && p->id == id)
                    slot = p;
        if (slot == nullptr)
            for (auto& s : impl->slots)
                if (auto* p = s.load())
                    if (! p->active)
                    {
                        slot = p;
                        break;
                    }
        if (slot == nullptr)
            for (auto& s : impl->slots)
                if (s.load() == nullptr)
                {
                    impl->owned.push_back (std::make_unique<PeerSlot>());
                    slot = impl->owned.back().get();
                    s.store (slot, std::memory_order_release);
                    break;
                }
        if (slot == nullptr)
            return;     // full

        slot->id = id;
        slot->volume = 1.0f;
        slot->muted = false;
        slot->networkMs = 0.0f;
        slot->levelDb = -100.0f;
        for (auto& st : slot->streams)
            st.requestReset();
        slot->active.store (true, std::memory_order_release);
        ++impl->numActive;
    }

    void Engine::removePeer (PeerId id)
    {
        if (auto* p = impl->find (id))
        {
            p->active.store (false, std::memory_order_release);
            for (auto& st : p->streams)
                st.requestReset();
            --impl->numActive;
        }
    }

    void Engine::setOptions (const LatencyOptions& o)
    {
        impl->compression = o.compression;
        impl->opusKbps = o.opusKbps;
        impl->lossProtection = o.lossProtection;
        impl->packetSamples = o.packetSamples;
        impl->autoJitter = o.autoJitterBuffer;
        impl->jitterMs = o.jitterMs;
        impl->driftCorrection = o.driftCorrection;
        impl->syncTrack = o.syncTrack;
        impl->oneBarMode = o.oneBarMode;
        impl->rawMode = o.sendMode == SendMode::rawInput;
    }

    LatencyOptions Engine::currentOptions() const noexcept
    {
        LatencyOptions o;
        o.compression = impl->compression.load (std::memory_order_relaxed);
        o.opusKbps = impl->opusKbps.load (std::memory_order_relaxed);
        o.lossProtection = impl->lossProtection.load (std::memory_order_relaxed);
        o.packetSamples = impl->packetSamples.load (std::memory_order_relaxed);
        o.autoJitterBuffer = impl->autoJitter.load (std::memory_order_relaxed);
        o.jitterMs = impl->jitterMs.load (std::memory_order_relaxed);
        o.driftCorrection = impl->driftCorrection.load (std::memory_order_relaxed);
        o.syncTrack = impl->syncTrack.load (std::memory_order_relaxed);
        o.oneBarMode = impl->oneBarMode.load (std::memory_order_relaxed);
        o.sendMode = impl->rawMode.load (std::memory_order_relaxed) ? SendMode::rawInput : SendMode::finishedSound;
        return o;
    }

    void Engine::setPeerVolume (PeerId id, float g)
    {
        if (auto* p = impl->find (id))
            p->volume = juce::jmax (0.0f, g);
    }

    void Engine::setPeerMuted (PeerId id, bool m)
    {
        if (auto* p = impl->find (id))
            p->muted = m;
    }

    void Engine::setPeerNetworkMs (PeerId id, float ms)
    {
        if (auto* p = impl->find (id))
            p->networkMs.store (juce::jmax (0.0f, ms), std::memory_order_relaxed);
    }

    void Engine::setSessionTempo (double bpm)
    {
        impl->sessionTempo.store (bpm, std::memory_order_relaxed);
    }

    bool Engine::hasPeers() const noexcept
    {
        return impl->numActive.load (std::memory_order_relaxed) > 0;
    }

    double Engine::barSeconds (double localBpm) const noexcept
    {
        double bpm = impl->sessionTempo.load (std::memory_order_relaxed);
        if (! (bpm > 1.0))
            bpm = localBpm;
        if (! (bpm > 1.0))
            bpm = 120.0;
        return 4.0 * 60.0 / juce::jlimit (20.0, 400.0, bpm);   // 4/4: the host gives no time signature here
    }

    double Engine::syncDelaySeconds() const noexcept
    {
        return hasPeers() ? impl->syncDelay.load (std::memory_order_relaxed) : 0.0;
    }

    void Engine::mixFriends (float* left, float* right, int numChannels, int numSamples, double sampleRate,
                             double localBpm) noexcept
    {
        if (numSamples <= 0 || sampleRate <= 0.0 || impl->mixing.test_and_set (std::memory_order_acquire))
            return;

        RenderContext ctx;
        ctx.deviceRate = sampleRate;
        ctx.sessionNow = impl->session.network().sessionTime();
        ctx.wallNow = juce::Time::getMillisecondCounterHiRes() * 0.001;
        const bool oneBar = impl->oneBarMode.load (std::memory_order_relaxed);
        const double bar = barSeconds (localBpm);
        ctx.barSeconds = oneBar ? bar : 0.0;
        ctx.autoJitter = impl->autoJitter.load (std::memory_order_relaxed);
        ctx.jitterMs = impl->jitterMs.load (std::memory_order_relaxed);
        ctx.driftCorrection = impl->driftCorrection.load (std::memory_order_relaxed);
        impl->lastBlockMs.store (numSamples * 1000.0 / sampleRate, std::memory_order_relaxed);

        double slowest = 0.0;
        const float envDecay = std::exp (-(float) juce::jmin (numSamples, maxChunk) / (float) (sampleRate * 0.3));

        for (auto& s : impl->slots)
        {
            auto* p = s.load (std::memory_order_acquire);
            if (p == nullptr || ! p->active.load (std::memory_order_acquire))
                continue;
            const auto id = p->id.load (std::memory_order_relaxed);
            p->feed.rate.store (sampleRate, std::memory_order_relaxed);
            const float target = p->muted.load (std::memory_order_relaxed) ? 0.0f : p->volume.load (std::memory_order_relaxed);

            for (int start = 0; start < numSamples; start += maxChunk)
            {
                const int len = juce::jmin (maxChunk, numSamples - start);
                ctx.numSamples = len;
                auto* L = p->scratch.getWritePointer (0);
                auto* R = p->scratch.getWritePointer (1);

                for (int k = 0; k < 2; ++k)
                {
                    auto& st = p->streams[k];
                    p->midi.clear();
                    bool got = st.render (L, R, k == 0 ? &p->midi : nullptr, ctx);

                    if (k == 0)
                    {
                        const bool raw = st.kind.load (std::memory_order_relaxed) == (int) StreamKind::raw;
                        p->feed.hasRaw.store (raw, std::memory_order_relaxed);
                        const bool restarted = st.takeNotesOff();
                        if (raw && got)
                        {
                            // their dry input -> their replica -> their sound; no replica yet = nothing
                            p->feed.raw.push (L, R, len);
                            auto* midiIn = &p->midi;
                            if (restarted && p->replicaFed)
                            {
                                // MIDI may have been dropped with the old stream: release held notes first
                                p->merged.clear();
                                p->merged.addEvents (p->notesOff, 0, -1, 0);
                                p->merged.addEvents (p->midi, 0, -1, 0);
                                midiIn = &p->merged;
                            }
                            juce::AudioBuffer<float> io (p->scratch.getArrayOfWritePointers(), 2, len);
                            if (impl->session.replicas().process (id, io, len, *midiIn))
                            {
                                p->replicaFed = true;
                            }
                            else
                            {
                                std::fill (L, L + len, 0.0f);
                                std::fill (R, R + len, 0.0f);
                                got = false;
                            }
                        }
                        else
                        {
                            if (raw)
                            {
                                p->feed.raw.push (L, R, len);
                                std::fill (L, L + len, 0.0f);
                                std::fill (R, R + len, 0.0f);
                            }
                            if (p->replicaFed)
                            {
                                // their raw stream stopped or they switched to finished sound: release every
                                // note their replica holds, so it does not drone when it plays again
                                p->merged.clear();
                                p->merged.addEvents (p->notesOff, 0, -1, 0);
                                juce::AudioBuffer<float> quiet (p->silence.getArrayOfWritePointers(), 2, len);
                                impl->session.replicas().process (id, quiet, len, p->merged);
                                p->silence.clear();
                                p->replicaFed = false;
                            }
                        }
                        p->feed.out.push (L, R, len);

                        float peak = 0.0f;
                        for (int i = 0; i < len; ++i)
                            peak = juce::jmax (peak, std::abs (L[i]), std::abs (R[i]));
                        p->env = juce::jmax (peak, p->env * envDecay);
                        p->levelDb.store (juce::Decibels::gainToDecibels (p->env, -100.0f), std::memory_order_relaxed);
                    }
                    else if (! got)
                    {
                        continue;
                    }

                    // volume ramps over the chunk (no zipper)
                    float g = p->gainNow;
                    const float dg = (target - g) / (float) len;
                    if (numChannels > 1)
                    {
                        for (int i = 0; i < len; ++i, g += dg)
                        {
                            left[start + i] += L[i] * g;
                            right[start + i] += R[i] * g;
                        }
                    }
                    else
                    {
                        for (int i = 0; i < len; ++i, g += dg)
                            left[start + i] += 0.5f * (L[i] + R[i]) * g;
                    }
                }
                p->gainNow = target;
            }

            slowest = juce::jmax (slowest, (double) p->networkMs.load (std::memory_order_relaxed)
                                             + (double) p->streams[0].bufferMs.load (std::memory_order_relaxed));
        }

        // Backing track sync: a friend hears our track after the network and their buffer for it; their
        // buffer for us is not visible here, so ours for them stands in (the paths are near symmetric).
        impl->syncDelay.store (oneBar ? bar : slowest * 0.001, std::memory_order_relaxed);
        impl->mixing.clear (std::memory_order_release);
    }

    Engine::Stats Engine::statsFor (PeerId id) const
    {
        Stats st;
        auto* p = impl->find (id);
        if (p == nullptr)
            return st;

        const auto& s = p->streams[0];
        st.networkMs = p->networkMs.load();
        st.jitterMs = s.jitterMs.load();
        st.bufferMs = s.bufferMs.load();
        st.lossPercent = s.lossPercent.load();
        st.driftPpm = s.driftPpm.load();
        st.levelDb = p->levelDb.load();
        st.compression = s.compression.load();
        st.sendMode = s.kind.load() == (int) StreamKind::raw ? SendMode::rawInput : SendMode::finishedSound;
        st.hasTrack = p->streams[1].active.load()
                       &&(juce::int64) juce::Time::getMillisecondCounterHiRes() - p->streams[1].lastArrivalMs.load() < 2000;

        // their pick attack to your ears: they wait a packet (and the codec), the network, our buffer (which
        // already holds our block); plus on average half of their own audio block before it is sent
        st.totalMs = s.packetMs.load() + s.codecMs.load() + st.networkMs + st.bufferMs + 0.5f * (float) impl->lastBlockMs.load();
        if (impl->oneBarMode.load())
        {
            double bpm = impl->sessionTempo.load();
            if (! (bpm > 1.0))
                if (auto* port = impl->session.playerPort())
                    bpm = port->lastTransport().bpm;
            st.totalMs = (float) (barSeconds (bpm) * 1000.0);
        }
        return st;
    }

    ViewFeed* Engine::feedFor (PeerId id)
    {
        if (id == noPeer || id == impl->session.selfId())
        {
            auto* port = impl->session.playerPort();
            if (port == nullptr)
                return nullptr;
            auto& proc = port->processor();
            for (auto& f : impl->selfFeeds)
                if (f.first == &proc)
                    return f.second.get();
            impl->selfFeeds.emplace_back (&proc, std::make_unique<ProcessorFeed> (proc));
            return impl->selfFeeds.back().second.get();
        }

        for (auto& s : impl->slots)
            if (auto* p = s.load())
                if (p->id == id)
                    return &p->feed;
        return nullptr;
    }
}
