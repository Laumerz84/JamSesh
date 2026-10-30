#include "JamStream.h"
#include <cstring>
#include <cmath>

// Owner: stream soldier.
namespace jam
{
    namespace
    {
        constexpr double wireRate = (double) wire::sampleRate;
        constexpr int ringMask = ReceiveStream::ringSize - 1;

        // Drift: the rate ratio comes from the two RateTrackers (sender's clock vs this card's), limited to
        // +-300 ppm; a gentle proportional trim on the buffer level (50 samples off -> 100 ppm, at most
        // +-100) removes what is left. Big level errors are fixed by a crossfaded skip or rewind instead.
        constexpr double kp = 2.0e-6;
        constexpr double maxDrift = 300.0e-6, maxProportional = 100.0e-6;
        constexpr int fadeLength = 64;
    }

    //==============================================================================
    void PushResampler::prepare (int cap)
    {
        capacity = juce::jmax (16, cap);
        bufL.assign ((size_t) capacity, 0.0f);
        bufR.assign ((size_t) capacity, 0.0f);
        reset();
    }

    void PushResampler::reset() noexcept
    {
        std::fill (bufL.begin(), bufL.end(), 0.0f);
        std::fill (bufR.begin(), bufR.end(), 0.0f);
        count = 2;      // two zeros of history: the interpolator looks two samples back
        pos = 2.0;
    }

    void PushResampler::write (const float* left, const float* right, int n) noexcept
    {
        n = juce::jmin (n, capacity - count);
        if (n <= 0)
            return;
        std::memcpy (bufL.data() + count, left, sizeof (float) * (size_t) n);
        std::memcpy (bufR.data() + count, right, sizeof (float) * (size_t) n);
        count += n;
    }

    int PushResampler::read (float* left, float* right, int maxOut, double step) noexcept
    {
        int made = 0;
        const float* l = bufL.data();
        const float* r = bufR.data();
        while (made < maxOut)
        {
            const int i = (int) pos;
            if (i + 3 >= count)
                break;
            const float t = (float) (pos - i);
            left[made]  = lagrange6 (l + i - 2, t);
            right[made] = lagrange6 (r + i - 2, t);
            ++made;
            pos += step;
        }

        // keep two samples of history before the read position
        const int drop = (int) pos - 2;
        if (drop > 0)
        {
            const int keep = juce::jmax (0, count - drop);
            std::memmove (bufL.data(), bufL.data() + drop, sizeof (float) * (size_t) keep);
            std::memmove (bufR.data(), bufR.data() + drop, sizeof (float) * (size_t) keep);
            count = keep;
            pos -= drop;
        }
        return made;
    }

    //==============================================================================
    void FeedRing::prepare (int size)
    {
        size = juce::nextPowerOfTwo (juce::jmax (1024, size));
        bufL.assign ((size_t) size, 0.0f);
        bufR.assign ((size_t) size, 0.0f);
        mask = size - 1;
        written = 0;
    }

    void FeedRing::push (const float* left, const float* right, int n) noexcept
    {
        if (mask == 0)
            return;
        auto w = written.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            const auto idx = (size_t) ((w + i) & mask);
            bufL[idx] = left[i];
            bufR[idx] = right[i];
        }
        written.store (w + n, std::memory_order_release);
    }

    int FeedRing::copyLatest (float* left, float* right, int n) const noexcept
    {
        if (mask == 0)
            return 0;
        const auto w = written.load (std::memory_order_acquire);
        n = (int) juce::jmin ((juce::int64) n, w, (juce::int64) mask + 1);
        const auto start = w - n;
        for (int i = 0; i < n; ++i)
        {
            const auto idx = (size_t) ((start + i) & mask);
            left[i] = bufL[idx];
            right[i] = bufR[idx];
        }
        return n;
    }

    //==============================================================================
    void RateTracker::add (double t, double lag) noexcept
    {
        if (windowStart >= 0.0 && std::abs (lag - lastLag) > 0.5)
            reset();    // a jump (pause, restart): start over
        lastLag = lag;

        if (windowStart < 0.0)
        {
            windowStart = windowMinT = t;
            windowMin = lag;
            return;
        }
        if (lag < windowMin)
        {
            windowMin = lag;
            windowMinT = t;
        }
        if (t - windowStart < 1.0)
            return;

        ts[head] = windowMinT;
        vs[head] = windowMin;
        head = (head + 1) % maxWindows;
        count = juce::jmin (count + 1, maxWindows);
        windowStart = windowMinT = t;
        windowMin = lag;

        if (count >= 2)
        {
            double mt = 0.0, mv = 0.0;
            for (int i = 0; i < count; ++i) { mt += ts[i]; mv += vs[i]; }
            mt /= count;
            mv /= count;
            double num = 0.0, den = 0.0;
            for (int i = 0; i < count; ++i)
            {
                num += (ts[i] - mt) * (vs[i] - mv);
                den += (ts[i] - mt) * (ts[i] - mt);
            }
            if (den > 0.0)
                ppmNow = -num / den * 1.0e6;
        }
    }

    //==============================================================================
    void ReceiveStream::allocate()
    {
        slots.resize ((size_t) fifoSlots);
        store.resize ((size_t) storeSlots);
        ringL.assign ((size_t) ringSize, 0.0f);
        ringR.assign ((size_t) ringSize, 0.0f);
        decoder.create();
    }

    void ReceiveStream::push (const PacketView& v, const juce::uint8* data, int size, double arrival) noexcept
    {
        if (slots.empty() || size > wire::maxPacketBytes)
            return;

        const juce::SpinLock::ScopedLockType sl (pushLock);

        if (! v.h.resend)
            updateArrivalStats (v, arrival);
        lastArrivalMs.store ((juce::int64) (arrival * 1000.0), std::memory_order_relaxed);

        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 + n2 == 0)
            return;     // the audio thread is not draining (no player instance running): drop
        auto& slot = slots[(size_t) (n1 > 0 ? s1 : s2)];
        std::memcpy (slot.bytes, data, (size_t) size);
        slot.size = size;
        slot.arrival = arrival;
        fifo.finishedWrite (1);
    }

    void ReceiveStream::updateArrivalStats (const PacketView& v, double arrival) noexcept
    {
        // Arrival spread: transit = arrival - capture position. Its floor (the fastest packets) creeps up
        // slowly so a sender with a slower clock does not look like growing jitter.
        const double transit = arrival - (double) v.h.sampleCounter / wireRate;
        if (! haveTransit || std::abs (transit - minTransit) > 2.0)
        {
            haveTransit = true;
            minTransit = prevTransit = transit;
            jitterJ = peakSpread = 0.0;
            prevArrival = arrival;
            senderRate.reset();
        }
        else
        {
            const double dt = juce::jmax (0.0, arrival - prevArrival);
            jitterJ += (std::abs (transit - prevTransit) - jitterJ) / 16.0;   // RFC 3550
            minTransit = juce::jmin (minTransit + dt * 3.0e-4, transit);
            const double spread = transit - minTransit;
            // the late tail: jumps up at once on a late packet, holds 5 s, then sinks slowly (~30 s)
            if (spread >= peakSpread)
            {
                peakSpread = spread;
                peakHoldUntil = arrival + 5.0;
            }
            else if (arrival > peakHoldUntil)
            {
                peakSpread = juce::jmax (spread, peakSpread - peakSpread * juce::jmin (1.0, dt / 30.0));
            }
            prevTransit = transit;
            prevArrival = arrival;
        }
        senderRate.add (arrival, transit);
        senderPpm.store (senderRate.ppm(), std::memory_order_relaxed);
        senderRateReady.store (senderRate.ready(), std::memory_order_relaxed);
        jitterMs.store ((float) (peakSpread * 1000.0), std::memory_order_relaxed);
        // safety margin: the worst recent lateness plus half a packet and 0.5 ms (arrival-time granularity)
        jitterMarginSec.store (juce::jmax (2.5 * jitterJ, peakSpread) + 0.5 * v.h.frames / wireRate + 0.0005,
                               std::memory_order_relaxed);
    }

    void ReceiveStream::reset() noexcept
    {
        // consumer side: discard what is queued (it belonged to the previous peer); the producer may be
        // writing at this moment, so the fifo itself is never reset
        for (int ready = fifo.getNumReady(); ready > 0; --ready)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            fifo.finishedRead (n1 + n2);
        }
        notesOffPending = true;
        for (auto& st : seenStarts)
            st = -1;
        state = State::empty;
        for (auto& s : store)
            s.used = false;
        midiCount = 0;
        decoder.reset();
        rateCorrection = 0.0;
        cardRate.reset();
        consumedSeconds = 0.0;
        haveLevel = haveOffset = false;
        lossEma = 0.0;
        prevWall = 0.0;
        kind = -1;
        active = false;
        bufferMs = 0.0f;
        driftPpm = 0.0f;
        lossPercent = 0.0f;
    }

    void ReceiveStream::restartAt (juce::int64 counter) noexcept
    {
        notesOffPending = notesOffPending || midiCount > 0 || state == State::playing;
        state = State::prebuffer;
        decodeHead = writeHeadMax = counter;
        readPos = (double) counter;
        for (auto& s : store)
            s.used = false;
        midiCount = 0;
        decoder.reset();
        haveLevel = false;
        levelSmoothed = lateSmoothed = 0.0;
        fadeIn = fadeLength;
        concealRun = starveSamples = 0;
        prevWall = 0.0;
    }

    void ReceiveStream::writeRing (juce::int64 at, const float* l, const float* r, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const auto idx = (size_t) ((at + i) & ringMask);
            ringL[idx] = l[i];
            ringR[idx] = r[i];
        }
    }

    void ReceiveStream::readRing (double pos, double step, float* l, float* r, int n) const noexcept
    {
        const float* rl = ringL.data();
        const float* rr = ringR.data();
        for (int i = 0; i < n; ++i)
        {
            const double p = pos + i * step;
            const auto ip = (juce::int64) std::floor (p);
            const float t = (float) (p - (double) ip);
            float yl[6], yr[6];
            for (int k = 0; k < 6; ++k)
            {
                const auto idx = (size_t) ((ip - 2 + k) & ringMask);
                yl[k] = rl[idx];
                yr[k] = rr[idx];
            }
            l[i] = lagrange6 (yl, t);
            r[i] = lagrange6 (yr, t);
        }
    }

    // A skip or rewind lands where the waveform matches the one it replaces (best correlation of the
    // crossfade region within +-search samples of the wanted jump), so the crossfade does not dip.
    double ReceiveStream::alignedJump (double from, double wanted, double search) const noexcept
    {
        if (search < 1.0)
            return wanted;
        const auto base = (juce::int64) std::floor (from);
        const int range = (int) search;
        double best = -1.0e30, bestJump = wanted;
        for (int d = -range; d <= range; d += 2)
        {
            const auto j = (juce::int64) std::llround (wanted) + d;
            double dot = 0.0, energy = 1.0e-9;
            for (int i = 0; i < fadeLength; i += 2)
            {
                const auto a = (size_t) ((base + i) & ringMask), b = (size_t) ((base + j + i) & ringMask);
                dot += (double) ringL[a] * ringL[b] + (double) ringR[a] * ringR[b];
                energy += (double) ringL[b] * ringL[b] + (double) ringR[b] * ringR[b];
            }
            const double score = dot / std::sqrt (energy);
            if (score > best)
            {
                best = score;
                bestJump = (double) j + (from - (double) base);
            }
        }
        return bestJump;
    }

    static inline void noteLoss (double& ema, double samples, double lost) noexcept
    {
        const double a = juce::jmin (1.0, samples / (wireRate * 10.0));   // ~ the last 10 s
        ema += (lost - ema) * a;
    }

    void ReceiveStream::decodeFrame (const PacketHeader& h, const juce::uint8* bytes, int size, juce::int64 at) noexcept
    {
        const int f = h.frames;
        if (h.codec == Codec::pcm16)
        {
            pcm16::decode (bytes, h.channels, f, tmpL, tmpR);
        }
        else
        {
            if (lastCodec != Codec::opus)
                decoder.reset();
            decoder.decode (bytes, size, f, tmpL, tmpR);
        }
        lastCodec = h.codec;
        lastFrames = f;
        lastChannels = h.channels;

        if (fadeIn > 0)
        {
            const int len = juce::jmin (fadeIn, f);
            for (int i = 0; i < len; ++i)
            {
                const float g = (float) i / (float) fadeIn;
                tmpL[i] *= g;
                tmpR[i] *= g;
            }
            fadeIn = 0;
        }
        writeRing (at, tmpL, tmpR, f);
        decodeHead = at + f;
        concealRun = 0;
        noteLoss (lossEma, f, 0.0);
    }

    void ReceiveStream::decodePacket (const PacketView& v) noexcept
    {
        decodeFrame (v.h, v.main, v.h.mainBytes, v.h.sampleCounter);   // its MIDI was queued on arrival
    }

    bool ReceiveStream::midiSeen (juce::int64 start) const noexcept
    {
        for (auto s : seenStarts)
            if (s == start)
                return true;
        return false;
    }

    void ReceiveStream::queueMidi (juce::int64 pos, const juce::uint8* data, int size) noexcept
    {
        if (midiCount >= maxMidiPending)
            return;
        auto& p = midiPending[midiCount++];
        p.pos = pos;
        p.size = (juce::uint8) juce::jlimit (1, 3, size);
        std::memcpy (p.data, data, 3);
    }

    // Raw-input MIDI: this packet's own events, plus the copies of the previous two packets' events for
    // packets whose own MIDI never arrived. Each packet's MIDI is queued once (a ring of the packet starts
    // seen); events whose time has passed play at the start of the next block - a late note-off beats none.
    void ReceiveStream::takeMidi (const PacketView& v) noexcept
    {
        const auto c = v.h.sampleCounter;
        if (v.h.midiCount > 0 && ! midiSeen (c))
        {
            for (int i = 0; i < v.h.midiCount; ++i)
            {
                const auto e = v.midiAt (i);
                queueMidi (c + e.offset, e.data, e.size);
            }
            seenStarts[seenHead] = c;
            seenHead = (seenHead + 1) % numSeen;
        }

        juce::int64 newStarts[2];
        int numNew = 0;
        for (int i = 0; i < v.h.historyCount; ++i)
        {
            const auto e = v.historyAt (i);
            const auto start = c + e.relStart;
            if (midiSeen (start))
                continue;
            queueMidi (start + e.offset, e.data, e.size);
            ++recoveredMidi;
            if ((numNew == 0 || newStarts[numNew - 1] != start) && numNew < 2)
                newStarts[numNew++] = start;
        }
        for (int i = 0; i < numNew; ++i)
        {
            seenStarts[seenHead] = newStarts[i];
            seenHead = (seenHead + 1) % numSeen;
        }
    }

    void ReceiveStream::flushStore() noexcept
    {
        for (bool found = true; found;)
        {
            found = false;
            for (auto& s : store)
            {
                if (! s.used)
                    continue;
                PacketView v;
                if (! parsePacket (s.bytes, s.size, v) || v.h.sampleCounter + v.h.frames <= decodeHead)
                {
                    s.used = false;     // late now
                    continue;
                }
                if (v.h.sampleCounter == decodeHead)
                {
                    decodePacket (v);
                }
                else if (v.redundant != nullptr && v.h.sampleCounter - v.h.frames == decodeHead)
                {
                    decodeFrame (v.h, v.redundant, v.h.redundantBytes, decodeHead);
                    decodePacket (v);
                }
                else
                {
                    continue;
                }
                s.used = false;
                found = true;
            }
        }
    }

    bool ReceiveStream::storeHasAny (juce::int64& first) const noexcept
    {
        bool any = false;
        for (auto& s : store)
            if (s.used && (! any || s.counter < first))
            {
                first = s.counter;
                any = true;
            }
        return any;
    }

    void ReceiveStream::concealUntil (juce::int64 end) noexcept
    {
        auto gap = end - decodeHead;
        if (gap <= 0)
            return;
        if (gap > ringSize / 4)
        {
            restartAt (end);
            return;
        }
        noteLoss (lossEma, (double) gap, 1.0);

        if (lastCodec == Codec::opus)
        {
            while (gap > 0)
            {
                const int chunk = (int) juce::jmin ((juce::int64) wire::maxFrames, gap);
                const int frames = juce::jmin (wire::maxFrames, ((chunk + 119) / 120) * 120);
                decoder.decode (nullptr, 0, frames, tmpL, tmpR);   // Opus packet loss concealment
                writeRing (decodeHead, tmpL, tmpR, chunk);
                decodeHead += chunk;
                concealRun += chunk;
                gap -= chunk;
            }
            return;
        }

        // PCM: repeat the last frame, fading out over 5 ms; the next real frame fades back in
        const int period = juce::jmax (1, lastFrames);
        const auto base = decodeHead - period;
        for (juce::int64 i = 0; i < gap; ++i)
        {
            const auto src = (size_t) ((base + (i % period)) & ringMask);
            const auto dst = (size_t) ((decodeHead + i) & ringMask);
            const float g = juce::jmax (0.0f, 1.0f - (float) (concealRun + i) / 240.0f);
            ringL[dst] = ringL[src] * g;
            ringR[dst] = ringR[src] * g;
        }
        concealRun += (int) gap;
        decodeHead += gap;
        fadeIn = fadeLength;
    }

    void ReceiveStream::handle (const PacketView& v, const juce::uint8* data, int size, double arrival) noexcept
    {
        const auto& h = v.h;
        const auto c = h.sampleCounter;
        const int f = h.frames;

        kind.store ((int) h.kind, std::memory_order_relaxed);
        compression.store (h.codec == Codec::opus, std::memory_order_relaxed);
        packetMs.store ((float) f * 1000.0f / (float) wireRate, std::memory_order_relaxed);
        codecMs.store (h.codec == Codec::opus ? 2.5f : 0.0f, std::memory_order_relaxed);
        active.store (true, std::memory_order_relaxed);
        // loss protection in use: redundant copies inside packets, or sent alone (they show up as repeats)
        sinceRedundant = (v.redundant != nullptr || h.resend) ? 0 : sinceRedundant + 1;
        protectedStream = sinceRedundant < 16;

        // capture clock: sessionTime of sample 0 of their stream, smoothed against capture-time jitter
        const double off = h.sessionTime - (double) c / wireRate;
        if (! haveOffset || std::abs (off - captureOffset) > 0.25)
            captureOffset = off;
        else
            captureOffset += (off - captureOffset) * 0.01;
        haveOffset = true;

        if (state == State::empty
             || c + f - (juce::int64) readPos > ringSize - 4 * wire::maxFrames
             || c < (juce::int64) readPos - ringSize / 2)
            restartAt (c);      // first packet, or the sender restarted its counter

        if (! h.resend)
            takeMidi (v);

        // buffer level just after this arrival, against the reader position extrapolated to the arrival
        const auto end = c + f;
        if (state == State::playing && prevWall > 0.0)
        {
            const double readAt = prevReadStart + (arrival - prevWall) * prevSpeed;
            const double level = (double) juce::jmax (writeHeadMax, end) - readAt;
            if (! haveLevel)
                levelSmoothed = level;
            else
                levelSmoothed += (level - levelSmoothed) * (1.0 - std::exp (-(double) f / wireRate));
            haveLevel = true;
        }
        writeHeadMax = juce::jmax (writeHeadMax, end);

        if (end <= decodeHead || c < decodeHead)
            return;     // too late: its place was concealed (and counted) already

        if (c == decodeHead)
        {
            decodePacket (v);
            flushStore();
            return;
        }
        if (v.redundant != nullptr && c - f == decodeHead)
        {
            decodeFrame (h, v.redundant, h.redundantBytes, decodeHead);   // loss protection saved it
            decodePacket (v);
            flushStore();
            return;
        }

        // early: wait in the store for the ones before it
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            for (auto& s : store)
            {
                if (s.used && s.counter == c)
                    return;
                if (! s.used)
                {
                    s.used = true;
                    s.counter = c;
                    s.size = size;
                    std::memcpy (s.bytes, data, (size_t) size);
                    return;
                }
            }
            // store full: give up on the gap
            juce::int64 first = 0;
            if (storeHasAny (first))
            {
                concealUntil (first);
                flushStore();
            }
        }
    }

    bool ReceiveStream::render (float* left, float* right, juce::MidiBuffer* midiOut, const RenderContext& ctx) noexcept
    {
        const int n = ctx.numSamples;
        if (resetRequested.exchange (false))
            reset();
        if (ringL.empty() || n <= 0)
            return false;
        deviceRate = ctx.deviceRate;

        // hand-over queue -> timeline
        {
            int ready = fifo.getNumReady();
            while (ready-- > 0)
            {
                int s1, n1, s2, n2;
                fifo.prepareToRead (1, s1, n1, s2, n2);
                if (n1 + n2 == 0)
                    break;
                auto& slot = slots[(size_t) (n1 > 0 ? s1 : s2)];
                PacketView v;
                if (parsePacket (slot.bytes, slot.size, v))
                    handle (v, slot.bytes, slot.size, slot.arrival);
                fifo.finishedRead (1);
            }
        }

        // this card's clock against the wall clock (the sender's is measured on arrival)
        cardRate.add (ctx.wallNow, ctx.wallNow - consumedSeconds);
        consumedSeconds += n / ctx.deviceRate;

        auto silence = [&]
        {
            std::fill (left, left + n, 0.0f);
            std::fill (right, right + n, 0.0f);
        };
        auto fadeOutLast = [&]
        {
            for (int i = 0; i < n; ++i)
            {
                const float g = juce::jmax (0.0f, 1.0f - (float) i / (float) fadeLength);
                left[i] = lastOutL * g;
                right[i] = lastOutR * g;
            }
            lastOutL = lastOutR = 0.0f;
        };
        auto paused = [&]
        {
            prevReadStart = readPos;
            prevWall = ctx.wallNow;
            prevSpeed = 0.0;
        };

        if (state == State::empty)
        {
            silence();
            return false;
        }

        const double step0 = wireRate / ctx.deviceRate;
        const int P = juce::jmax (1, lastFrames);
        const int need = (int) std::ceil (n * step0 * 1.002) + 5;
        const double margin = (ctx.autoJitter ? juce::jlimit (0.001, 0.3, jitterMarginSec.load (std::memory_order_relaxed))
                                              : juce::jlimit (0.0, 1.0, (double) ctx.jitterMs * 0.001)) * wireRate;
        // loss protection only helps if we can wait for the next packet (it carries the lost one)
        const double safety = protectedStream ? juce::jmax (margin, P + 0.001 * wireRate) : margin;
        const double target = need + P + safety;     // level measured just after an arrival
        const bool oneBar = ctx.barSeconds > 0.0 && haveOffset;
        const double barSamples = juce::jmin (ctx.barSeconds * wireRate, (double) (ringSize - 9600));   // ring limit ~5.2 s
        auto lateness = [&]    // >0: playing later than "captured one bar ago"
        {
            return (ctx.sessionNow - captureOffset) * wireRate - readPos - barSamples;
        };

        auto publish = [&]
        {
            const double avail = (double) decodeHead - readPos;
            const double level = oneBar ? avail : (haveLevel ? levelSmoothed - 0.5 * P : avail);
            bufferMs.store ((float) (juce::jmax (0.0, level) * 1000.0 / wireRate), std::memory_order_relaxed);
            driftPpm.store ((float) (rateCorrection * 1.0e6), std::memory_order_relaxed);
            lossPercent.store ((float) (lossEma * 100.0), std::memory_order_relaxed);
        };

        if (state == State::prebuffer)
        {
            bool ready;
            if (oneBar)
            {
                const double late = lateness();
                ready = late >= 0.0;
                if (ready && late > 0.0)
                    readPos = juce::jmax (readPos, juce::jmin (readPos + late, (double) (decodeHead - need)));
            }
            else
            {
                ready = (double) (decodeHead - readPos) >= target - 0.5 * P;
            }

            if (! ready || (double) decodeHead - readPos < need)
            {
                silence();
                paused();
                publish();
                return false;
            }
            state = State::playing;
            haveLevel = false;
            lateSmoothed = oneBar ? lateness() : 0.0;
            fadeIn = 0;
            starveSamples = 0;
            lastOutL = lastOutR = 0.0f;
        }

        // a gap the reader has reached: conceal it if later packets are here
        if ((double) decodeHead - readPos < need)
        {
            juce::int64 first = 0;
            if (storeHasAny (first))
            {
                concealUntil (first);
                flushStore();
            }
        }

        // Nothing to conceal with yet, but packets are still flowing: treat the missing one as lost and
        // conceal it in its place (latency stays put; it is dropped if it turns up late). At most three
        // packets in a row - after that the sender has paused, and we hold instead.
        if ((double) decodeHead - readPos < need && ctx.driftCorrection
             && ctx.wallNow - (double) lastArrivalMs.load (std::memory_order_relaxed) * 0.001 < 0.05)
        {
            while ((double) decodeHead - readPos < need && concealRun < 3 * P)
                concealUntil (decodeHead + P);
        }

        if ((double) decodeHead - readPos < need)
        {
            // starving: nothing left to conceal with; hold the position (insert) so the buffer grows back
            fadeOutLast();
            noteLoss (lossEma, n * step0, 1.0);
            starveSamples += n;
            if (starveSamples > (int) (ctx.deviceRate * 0.5))
                state = State::prebuffer;       // gone quiet: buffer up again before playing
            paused();
            publish();
            return true;
        }
        const bool resuming = starveSamples > 0;
        starveSamples = 0;

        double excess;
        if (oneBar)
        {
            const double a = juce::jmin (1.0, (n / ctx.deviceRate) / 0.5);
            lateSmoothed += (lateness() - lateSmoothed) * a;
            excess = lateSmoothed;
        }
        else
        {
            excess = haveLevel ? levelSmoothed - target : 0.0;
        }

        // far too much buffered (or too late in one-bar mode): drop, with a short crossfade
        const double threshold = ctx.driftCorrection ? juce::jmax (120.0, (double) P, 0.5 * margin) : juce::jmax (96.0, (double) P);
        bool crossfade = false;
        if (excess > threshold)
        {
            const double skip = juce::jmin (excess, (double) decodeHead - readPos - need - 96.0);
            if (skip >= 16.0)
            {
                const double jump = alignedJump (readPos, skip, juce::jmin (skip - 16.0, 96.0));
                readRing (readPos, step0, xfadeL, xfadeR, fadeLength);
                readPos += jump;
                levelSmoothed -= jump;
                lateSmoothed -= jump;
                excess -= jump;
                crossfade = true;
            }
        }
        else if (! oneBar && haveLevel && ctx.driftCorrection && excess < -juce::jmax (72.0, 0.15 * margin))
        {
            // too little buffered for the jitter we now expect: replay a little (crossfaded) to grow it
            const double rewind = juce::jmin (-excess + 24.0, 0.02 * wireRate);
            const double back = -alignedJump (readPos, -rewind, juce::jmin (rewind - 16.0, 96.0));
            readRing (readPos, step0, xfadeL, xfadeR, fadeLength);
            readPos -= back;
            levelSmoothed += back;
            excess += back;
            crossfade = true;
        }
        else if (oneBar && excess < -threshold)
        {
            // too early (the tempo got slower): wait for the bar
            fadeOutLast();
            paused();
            lateSmoothed += n * step0;
            publish();
            return true;
        }

        if (senderRateReady.load (std::memory_order_relaxed) && cardRate.ready())
            rateCorrection = juce::jlimit (-maxDrift, maxDrift, (senderPpm.load (std::memory_order_relaxed) - cardRate.ppm()) * 1.0e-6);

        double correction = 0.0;
        if (ctx.driftCorrection)
            correction = rateCorrection + juce::jlimit (-maxProportional, maxProportional, kp * excess);
        double step = step0 * (1.0 + correction);
        const double room = (double) decodeHead - 4.0 - readPos;
        if (readPos + n * step > (double) decodeHead - 4.0)
            step = juce::jmax (0.0, room / n);

        readRing (readPos, step, left, right, n);

        if (crossfade)
            for (int i = 0; i < juce::jmin (n, fadeLength); ++i)
            {
                const float w = (float) i / (float) fadeLength;
                left[i] = xfadeL[i] + (left[i] - xfadeL[i]) * w;
                right[i] = xfadeR[i] + (right[i] - xfadeR[i]) * w;
            }
        if (resuming)
            for (int i = 0; i < juce::jmin (n, fadeLength); ++i)
            {
                const float g = (float) i / (float) fadeLength;
                left[i] *= g;
                right[i] *= g;
            }

        // their MIDI (raw-input mode), placed where its sample lands in this block
        if (midiCount > 0)
        {
            const double endPos = readPos + n * step;
            int keep = 0;
            for (int i = 0; i < midiCount; ++i)
            {
                const auto& e = midiPending[i];
                if ((double) e.pos < endPos)
                {
                    if (midiOut != nullptr)
                    {
                        const int offset = juce::jlimit (0, n - 1, (int) (((double) e.pos - readPos) / juce::jmax (1.0e-6, step)));
                        midiOut->addEvent (e.data, e.size, offset);
                    }
                }
                else
                {
                    midiPending[keep++] = e;
                }
            }
            midiCount = keep;
        }

        prevReadStart = readPos;
        prevWall = ctx.wallNow;
        prevSpeed = step * ctx.deviceRate;
        readPos += n * step;
        lastOutL = left[n - 1];
        lastOutR = right[n - 1];
        publish();
        return true;
    }
}
