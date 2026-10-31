#include "JamPort.h"
#include "JamEngine.h"
#include "JamCodec.h"
#include "JamStream.h"
#include "JamReplica.h"
#include "JamClock.h"
#include "JamMetronome.h"
#include "../PluginProcessor.h"
#include <cstring>

// Owner: stream soldier.
namespace jam
{
    namespace
    {
        constexpr int chunkSize = 1024;     // the port works in chunks of at most this many samples

        // One outgoing stream: device rate -> 48 kHz, cut into packets, PCM16 or Opus, sent
        struct Sender
        {
            void prepare (StreamKind k)
            {
                kind = k;
                resampler.prepare (chunkSize * 3 + 64);
                encMono.create (1);
                encStereo.create (2);
                reset();
            }

            void reset() noexcept
            {
                resampler.reset();
                acc = 0;
                prevBytes = 0;
                prevCounter = -1;
                numPending = 0;
                monoRun = 0;
                hist[0].count = hist[1].count = 0;
            }

            // midi: events of the host block; only those in [midiStart, midiStart + n) belong to this chunk.
            // hostBlock48k: the host's block in 48 kHz samples (the auto packet size follows it)
            void send (const float* left, const float* right, int n, double sampleRate, const juce::MidiBuffer* midi,
                       int midiStart, const LatencyOptions& opts, Network& net, double sessionNow, bool allowMono,
                       int hostBlock48k) noexcept
            {
                const double step = sampleRate / (double) wire::sampleRate;
                const bool direct = sampleRate == (double) wire::sampleRate;
                if (direct != wasDirect)
                {
                    // the device rate changed (prepare resets too): start the resampler from scratch
                    wasDirect = direct;
                    resampler.reset();
                }

                if (midi != nullptr)
                {
                    const double base = (double) (counter + acc) + resampler.pendingInput() / step;
                    for (const auto meta : *midi)
                    {
                        const int s = meta.samplePosition - midiStart;
                        if (s < 0 || s >= n || meta.numBytes < 1 || meta.numBytes > 3 || numPending >= maxPending)
                            continue;
                        auto& p = pending[numPending++];
                        p.pos = (juce::int64) (base + s / step);
                        p.e.size = (juce::uint8) meta.numBytes;
                        std::memcpy (p.e.data, meta.data, (size_t) meta.numBytes);
                    }
                }

                if (direct)
                {
                    // 48 kHz device: straight into the packet. The resampler would only delay here: its 3-sample
                    // lookahead left every packet short by 3 until the next host block (a whole block late).
                    const double endCounter = (double) (counter + acc + n);
                    int done = 0;
                    while (done < n)
                    {
                        if (acc == 0)
                            frames = wire::framesFor (opts.compression, opts.packetSamples, hostBlock48k);
                        const int k = juce::jmin (frames - acc, n - done);
                        std::memcpy (accL + acc, left + done, sizeof (float) * (size_t) k);
                        std::memcpy (accR + acc, right + done, sizeof (float) * (size_t) k);
                        acc += k;
                        done += k;
                        if (acc < frames)
                            break;
                        emit (opts, net, sessionNow - (endCounter - (double) counter) / wire::sampleRate, allowMono);
                        acc = 0;
                    }
                    return;
                }

                resampler.write (left, right, n);
                const double endCounter = (double) (counter + acc) + resampler.pendingInput() / step;

                for (;;)
                {
                    if (acc == 0)
                        frames = wire::framesFor (opts.compression, opts.packetSamples, hostBlock48k);
                    acc += resampler.read (accL + acc, accR + acc, frames - acc, step);
                    if (acc < frames)
                        break;
                    emit (opts, net, sessionNow - (endCounter - (double) counter) / wire::sampleRate, allowMono);
                    acc = 0;
                }
            }

            void emit (const LatencyOptions& opts, Network& net, double captureTime, bool allowMono) noexcept
            {
                // raw input: mono when both channels are the same (for half a second; stereo at once)
                bool mono = false;
                if (allowMono)
                {
                    bool same = true;
                    for (int i = 0; i < frames && same; ++i)
                        same = accL[i] == accR[i];
                    monoRun = same ? monoRun + frames : 0;
                    mono = monoRun >= wire::sampleRate / 2;
                }
                const int ch = mono ? 1 : 2;
                const Codec codec = opts.compression ? Codec::opus : Codec::pcm16;

                int bytes = 0;
                if (codec == Codec::pcm16)
                {
                    bytes = pcm16::encode (accL, accR, ch, frames, frame);
                }
                else
                {
                    auto& enc = ch == 1 ? encMono : encStereo;
                    if (prevCodec != Codec::opus || prevChannels != ch)
                        enc.reset();
                    enc.setKbps (ch == 1 ? juce::jmax (16, opts.opusKbps / 2) : opts.opusKbps);
                    bytes = enc.encode (accL, accR, frames, frame, wire::maxFrameBytes);
                }

                if (bytes > 0)
                {
                    PacketHeader h;
                    h.kind = allowMono ? StreamKind::raw : kind;     // allowMono = raw input
                    h.channels = ch;
                    h.codec = codec;
                    h.frames = frames;
                    h.sampleCounter = counter;
                    h.sessionTime = captureTime;
                    h.mainBytes = bytes;

                    // MIDI that lands in this packet (later ones wait; at most wire::maxMidi per packet)
                    MidiEvent events[wire::maxMidi];
                    int keep = 0;
                    for (int i = 0; i < numPending; ++i)
                    {
                        if (pending[i].pos < counter + frames && h.midiCount < wire::maxMidi)
                        {
                            auto e = pending[i].e;
                            e.offset = (juce::uint16) juce::jlimit ((juce::int64) 0, (juce::int64) frames - 1, pending[i].pos - counter);
                            events[h.midiCount++] = e;
                        }
                        else
                        {
                            pending[keep++] = pending[i];
                        }
                    }
                    numPending = keep;

                    // the MIDI of the previous two packets rides along (newest first): a lost note-off still lands
                    HistoryEvent history[wire::maxHistory];
                    for (int k = 0; k < 2; ++k)
                    {
                        const auto& hp = hist[(histHead + 2 - 1 - k) % 2];
                        const auto rel = hp.start - counter;
                        if (hp.count == 0 || rel < -32768 || rel >= 0)
                            continue;
                        for (int i = 0; i < hp.count && h.historyCount < wire::maxHistory; ++i)
                        {
                            auto& e = history[h.historyCount++];
                            e.relStart = (juce::int16) rel;
                            e.offset = hp.events[i].offset;
                            e.size = hp.events[i].size;
                            std::memcpy (e.data, hp.events[i].data, 3);
                        }
                    }

                    // Never let IP fragment a packet (<= wire::maxSafePayload): if the audio copy does not fit it
                    // goes alone right after; the copied MIDI shrinks to what is left; this packet's MIDI always stays.
                    const juce::uint8* redundant = nullptr;
                    bool resendSeparately = false;
                    const int base = wire::headerBytes + bytes + h.midiCount * wire::midiEventBytes;
                    if (opts.lossProtection && prevBytes > 0 && prevCounter + prevFrames == counter
                         && prevFrames == frames && prevChannels == ch && prevCodec == codec)
                    {
                        if (base + prevBytes <= wire::maxSafePayload)
                        {
                            redundant = prev;
                            h.redundantBytes = prevBytes;
                        }
                        else
                        {
                            resendSeparately = true;
                        }
                    }
                    const int room = wire::maxSafePayload - base - h.redundantBytes;
                    h.historyCount = juce::jlimit (0, h.historyCount, room / wire::historyEventBytes);

                    const int size = writePacket (packet, (int) sizeof (packet), h, frame, redundant, events, history);
                    if (size > 0)
                        net.sendAudio (packet, size);

                    // remember this packet's MIDI for the next two
                    auto& mine = hist[histHead];
                    mine.start = counter;
                    mine.count = h.midiCount;
                    std::copy (events, events + h.midiCount, mine.events);
                    histHead = (histHead + 1) % 2;

                    if (resendSeparately)
                    {
                        PacketHeader again = h;
                        again.sampleCounter = prevCounter;
                        again.sessionTime = captureTime - frames / (double) wire::sampleRate;
                        again.mainBytes = prevBytes;
                        again.redundantBytes = 0;
                        again.midiCount = 0;
                        again.historyCount = 0;
                        again.resend = true;
                        const int size2 = writePacket (packet, (int) sizeof (packet), again, prev, nullptr, nullptr);
                        if (size2 > 0)
                            net.sendAudio (packet, size2);
                    }

                    std::memcpy (prev, frame, (size_t) bytes);
                    prevBytes = bytes;
                }
                else
                {
                    prevBytes = 0;
                }
                prevCounter = counter;
                prevFrames = frames;
                prevChannels = ch;
                prevCodec = codec;
                counter += frames;
            }

            StreamKind kind = StreamKind::player;
            PushResampler resampler;
            OpusEnc encMono, encStereo;
            float accL[wire::maxFrames] {}, accR[wire::maxFrames] {};
            int acc = 0, frames = 128, monoRun = 0;
            bool wasDirect = false;
            juce::int64 counter = 0, prevCounter = -1;
            juce::uint8 frame[wire::maxFrameBytes] {}, prev[wire::maxFrameBytes] {};
            juce::uint8 packet[wire::maxPacketBytes] {};
            int prevBytes = 0, prevFrames = 0, prevChannels = 0;
            Codec prevCodec = Codec::pcm16;

            struct Pending { juce::int64 pos = 0; MidiEvent e; };
            static constexpr int maxPending = 128;
            Pending pending[maxPending];
            int numPending = 0;

            struct SentMidi { juce::int64 start = 0; int count = 0; MidiEvent events[wire::maxMidi]; };
            SentMidi hist[2];
            int histHead = 0;
        };

        // The local backing-track delay (sync track). Jumps to a new delay with a 10 ms crossfade when it
        // moves by more than 2 ms, so the slowly wandering estimate never makes it warble.
        struct SyncDelay
        {
            void prepare (double sr)
            {
                sampleRate = sr;
                size = juce::nextPowerOfTwo ((int) (sr * 6.0) + chunkSize);
                bufL.assign ((size_t) size, 0.0f);
                bufR.assign ((size_t) size, 0.0f);
                write = 0;
                delay = oldDelay = 0;
                fadePos = fadeLen = juce::jmax (1, (int) (sr * 0.01));
            }

            void process (float* l, float* r, int n, double seconds) noexcept
            {
                if (size == 0)
                    return;
                const int wanted = juce::jlimit (0, size - chunkSize - 1, (int) std::round (seconds * sampleRate));
                if (fadePos >= fadeLen
                     && (std::abs (wanted - delay) > (int) (sampleRate * 0.002 + 0.5) || (wanted == 0 && delay != 0)))
                {
                    oldDelay = delay;
                    delay = wanted;
                    fadePos = 0;
                }
                if (delay == 0 && fadePos >= fadeLen)
                {
                    // keep the line filled so a later delay starts from real audio
                    for (int i = 0; i < n; ++i)
                    {
                        bufL[(size_t) ((write + i) & (size - 1))] = l[i];
                        bufR[(size_t) ((write + i) & (size - 1))] = r[i];
                    }
                    write = (write + n) & (size - 1);
                    return;
                }
                const int mask = size - 1;
                for (int i = 0; i < n; ++i)
                {
                    bufL[(size_t) write] = l[i];
                    bufR[(size_t) write] = r[i];
                    float outL = bufL[(size_t) ((write - delay) & mask)];
                    float outR = bufR[(size_t) ((write - delay) & mask)];
                    if (fadePos < fadeLen)
                    {
                        const float g = (float) fadePos / (float) fadeLen;
                        outL = bufL[(size_t) ((write - oldDelay) & mask)] + (outL - bufL[(size_t) ((write - oldDelay) & mask)]) * g;
                        outR = bufR[(size_t) ((write - oldDelay) & mask)] + (outR - bufR[(size_t) ((write - oldDelay) & mask)]) * g;
                        ++fadePos;
                    }
                    l[i] = outL;
                    r[i] = outR;
                    write = (write + 1) & mask;
                }
            }

            std::vector<float> bufL, bufR;
            int size = 0, write = 0, delay = 0, oldDelay = 0, fadePos = 0, fadeLen = 1;
            double sampleRate = 48000.0;
        };
    }

    struct Port::Impl
    {
        std::atomic<Role> role { Role::player };
        std::atomic<TrackSource*> track { nullptr };

        std::atomic<bool> prepared { false };
        double sampleRate = 48000.0;
        Sender sound, trackOut;
        SyncDelay trackDelay;
        juce::AudioBuffer<float> trackBuf, monoTmp;
        bool wasInSession = false;

        // the host's block for the auto packet size: the largest of the last half second (hosts such as
        // FL Studio vary their blocks; the packet size should not chase every short one)
        int autoBlock = 0, windowMax = 0, windowSamples = 0;
        int hostBlock48k (int n) noexcept
        {
            windowMax = juce::jmax (windowMax, n);
            windowSamples += n;
            if (autoBlock == 0 || windowSamples >= (int) (sampleRate * 0.5))
            {
                autoBlock = windowMax;
                windowMax = windowSamples = 0;
            }
            return (int) std::lround (autoBlock * (double) wire::sampleRate / sampleRate);
        }
    };

    Port::Port (ClodScopeProcessor& p) : owner (p), impl (std::make_unique<Impl>())
    {
        impl->sound.prepare (StreamKind::player);
        impl->trackOut.prepare (StreamKind::track);
        sessionRef->attach (*this);
    }

    Port::~Port()
    {
        sessionRef->detach (*this);
    }

    void Port::prepare (double sampleRate, int maxBlockSize)
    {
        impl->prepared = false;
        impl->sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        impl->sound.reset();
        impl->trackOut.reset();
        impl->trackDelay.prepare (impl->sampleRate);
        impl->autoBlock = impl->windowMax = impl->windowSamples = 0;
        impl->trackBuf.setSize (2, chunkSize);
        impl->monoTmp.setSize (2, chunkSize);
        if (role() == Role::player)
            sessionRef->replicas().prepare (impl->sampleRate, juce::jmax (maxBlockSize, chunkSize));
        impl->prepared = true;
    }

    void Port::process (juce::AudioBuffer<float>& io, int numChannels, const juce::AudioBuffer<float>& rawInput,
                        const juce::MidiBuffer& midi, double bpm, double ppqPosition, bool playing) noexcept
    {
        lastBpm.store (bpm, std::memory_order_relaxed);
        lastPpq.store (ppqPosition, std::memory_order_relaxed);
        lastPlaying.store (playing, std::memory_order_relaxed);

        auto& d = *impl;
        const int n = io.getNumSamples();
        if (! d.prepared.load (std::memory_order_acquire) || numChannels < 1 || n <= 0)
            return;

        auto& session = *sessionRef;
        auto& engine = session.engine();
        auto& net = session.network();
        const auto opts = engine.currentOptions();
        const bool inSession = engine.hasPeers();
        if (inSession != d.wasInSession)
        {
            // a fresh session starts the streams from scratch (their counters keep running: receivers resync)
            d.wasInSession = inSession;
            d.sound.reset();
            d.trackOut.reset();
        }
        const double now = inSession ? net.sessionTime() : 0.0;
        const double trackDelay = inSession && (opts.syncTrack || opts.oneBarMode) ? engine.syncDelaySeconds() : 0.0;
        const bool isPlayer = role() == Role::player;
        const bool raw = opts.sendMode == SendMode::rawInput;
        const int block48k = d.hostBlock48k (n);

        for (int start = 0; start < n; start += chunkSize)
        {
            const int len = juce::jmin (chunkSize, n - start);
            float* l = io.getWritePointer (0, start);
            float* r = numChannels > 1 ? io.getWritePointer (1, start) : nullptr;
            const float* sl = l;
            const float* sr = r;
            if (r == nullptr)
            {
                // mono host: send the one channel as both
                sr = l;
            }

            if (isPlayer)
            {
                if (inSession)
                {
                    if (raw && rawInput.getNumChannels() >= 2 && rawInput.getNumSamples() >= start + len)
                        d.sound.send (rawInput.getReadPointer (0, start), rawInput.getReadPointer (1, start), len,
                                      d.sampleRate, &midi, start, opts, net, now, true, block48k);
                    else
                        d.sound.send (sl, sr, len, d.sampleRate, nullptr, 0, opts, net, now, false, block48k);
                }

                // the standalone app's file player: send it, play it here (late by the sync delay)
                if (auto* ts = d.track.load (std::memory_order_acquire))
                {
                    juce::AudioBuffer<float> view (d.trackBuf.getArrayOfWritePointers(), 2, len);
                    ts->renderNextBlock (view, len);
                    auto* tl = d.trackBuf.getWritePointer (0);
                    auto* tr = d.trackBuf.getWritePointer (1);
                    if (inSession && ts->isPlaying())
                        d.trackOut.send (tl, tr, len, d.sampleRate, nullptr, 0, opts, net, now, false, block48k);
                    d.trackDelay.process (tl, tr, len, trackDelay);
                    if (r != nullptr)
                    {
                        juce::FloatVectorOperations::add (l, tl, len);
                        juce::FloatVectorOperations::add (r, tr, len);
                    }
                    else
                    {
                        for (int i = 0; i < len; ++i)
                            l[i] += 0.5f * (tl[i] + tr[i]);
                    }
                }

                if (inSession)
                    engine.mixFriends (l, r != nullptr ? r : l, numChannels > 1 ? 2 : 1, len, d.sampleRate, bpm);
            }
            else
            {
                // backing-track instance: its input is the track; send it, then play it here late
                if (inSession)
                    d.trackOut.send (sl, sr, len, d.sampleRate, nullptr, 0, opts, net, now, false, block48k);

                if (r != nullptr)
                {
                    d.trackDelay.process (l, r, len, trackDelay);
                }
                else
                {
                    auto* tmp = d.monoTmp.getWritePointer (0);
                    std::memcpy (tmp, l, sizeof (float) * (size_t) len);
                    d.trackDelay.process (l, tmp, len, trackDelay);
                }
            }
        }
    }

    void Port::mixMetronome (juce::AudioBuffer<float>& io, int numChannels, double blockStartSeconds) noexcept
    {
        auto& d = *impl;
        const int n = io.getNumSamples();
        auto& session = *sessionRef;
        if (numChannels < 1 || n <= 0 || ! d.prepared.load (std::memory_order_acquire) || session.playerPort() != this)
            return;   // one click per computer: the player instance's

        // the block's start on the session clock, and when its first sample leaves the speakers after that: the sound
        // card's output latency (unknown inside a plugin host: a block + 0.75 ms, as Engine's estimates assume)
        const double blockSession = session.network().sessionTime() - (localSeconds() - blockStartSeconds);
        const int outSamples = owner.deviceOutputLatency.load (std::memory_order_relaxed);
        const double outSeconds = outSamples > 0 ? outSamples / d.sampleRate : n / d.sampleRate + 0.00075;
        float* l = io.getWritePointer (0);
        float* r = numChannels > 1 ? io.getWritePointer (1) : l;
        session.metronome().render (l, r, n, d.sampleRate, blockSession, outSeconds, lastBpm.load (std::memory_order_relaxed),
                                    lastPpq.load (std::memory_order_relaxed), lastPlaying.load (std::memory_order_relaxed));
    }

    void Port::setRole (Role r) { impl->role = r; }

    Role Port::role() const noexcept { return impl->role.load(); }
    void Port::setTrackSource (TrackSource* t) { impl->track = t; }
}
