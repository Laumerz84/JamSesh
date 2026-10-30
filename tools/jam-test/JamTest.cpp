// Headless Jam Sesh end-to-end tests. jam::Session is one per process, so every player is a hidden
// child process of this same tool (CREATE_NO_WINDOW, no console, no audio device, no window). Each
// child runs a ClodScopeProcessor on a simulated sound card: a thread that calls processBlock with
// N-sample blocks paced by its own clock, optionally running fast or slow by some ppm (clock drift).
// All sockets bind to 127.0.0.1 (CLOD_JAM_BIND); network trouble comes from CLOD_JAM_NETSIM.
// Every process on this PC shares one clock (QueryPerformanceCounter behind
// Time::getMillisecondCounterHiRes), so latency is measured directly: click in at the sender's input,
// click detected in the receiver's output.
// Owner: test soldier.
//
//   ClodJamTest <outDir> [--quick] [case ...]        run cases (default: all), summary in <outDir>\summary.txt
//      cases: local connect2 connect4 latency quality jitter drift raw rawmidi track onebar cpu
//   ClodJamTest --player host|join --out <file> [options]   one player (spawned by the above, or by ClodScopeSnap)
//      --port N  --code C  --name S  --expect N (players incl. self)  --seconds S (after everyone is in)
//      --signal silence|sine:HZ:AMP|clicks:PERIOD_MS|guitar   --ppm P   --block N
//      --opt comp=1,kbps=128,prot=1,packet=128,autojb=1,jb=10,drift=1,sync=1,onebar=0,raw=0
//      --track fl:SIGNAL | file:SIGNAL   (a second instance with the track role / a jam::TrackSource)
//      --patch drive   (a coloured sound for raw-input checks)   --view MAINVIEW[,V3MODE]
//      --knob ID:VALUE:SECONDS   (moves a parameter mid-run)
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <iostream>
#include <cstdlib>
#include <map>
#include "../../Source/PluginProcessor.h"
#include "../../Source/Jam/JamSession.h"
#include "../../Source/Jam/JamPort.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

// a few Win32 calls, declared here so windows.h stays out of the JUCE headers
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod (unsigned int);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod (unsigned int);
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess();
extern "C" __declspec(dllimport) int __stdcall GetProcessTimes (void*, void*, void*, void*, void*);
#pragma comment (lib, "winmm.lib")

namespace
{
    constexpr double sampleRate = 48000.0;

    double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

    double processCpuMs()
    {
        juce::uint64 c[4] {};
        GetProcessTimes (GetCurrentProcess(), &c[0], &c[1], &c[2], &c[3]);
        return (double) (c[2] + c[3]) / 10000.0;   // kernel + user, 100 ns units
    }

    void setEnv (const char* name, const juce::String& value)
    {
        _putenv_s (name, value.toRawUTF8());   // also sets the Win32 environment that child processes inherit
    }

    double median (std::vector<double> v)
    {
        if (v.empty()) return 0.0;
        std::sort (v.begin(), v.end());
        return v[v.size() / 2];
    }

    juce::String fmt (double v, int dp = 1) { return juce::String (v, dp); }

    juce::String joinNums (const std::vector<double>& v, int dp = 2)
    {
        juce::StringArray s;
        for (auto x : v) s.add (juce::String (x, dp));
        return s.joinIntoString (",");
    }

    std::vector<double> parseNums (const juce::String& s)
    {
        std::vector<double> v;
        for (auto& t : juce::StringArray::fromTokens (s, ",", ""))
            if (t.isNotEmpty())
                v.push_back (t.getDoubleValue());
        return v;
    }

    //==============================================================================================
    // Test signals

    // Karplus-Strong strings (as in ClodScopeSnap), strummed again every two seconds
    struct Guitar
    {
        struct Str { std::vector<float> line; size_t pos = 0; float prev = 0.0f; long start = 0; };
        std::vector<Str> strings;
        juce::Random rng { 5 };
        long t = 0;

        void strum (std::initializer_list<int> notes, double spreadMs)
        {
            strings.clear();
            long k = 0;
            for (int note : notes)
            {
                const double hz = 440.0 * std::pow (2.0, (note - 69) / 12.0);
                Str s;
                s.line.resize ((size_t) juce::jmax (2, (int) std::round (sampleRate / hz)));
                const float pos = 0.12f + 0.18f * rng.nextFloat();
                for (size_t i = 0; i < s.line.size(); ++i)
                {
                    const float x = (float) i / (float) s.line.size();
                    const float tri = x < pos ? x / pos : (1.0f - x) / (1.0f - pos);
                    s.line[i] = 1.6f * (tri - 0.5f) + 0.1f * (rng.nextFloat() * 2.0f - 1.0f);
                }
                s.start = t + (long) (k++ * spreadMs * 0.001 * sampleRate);
                strings.push_back (std::move (s));
            }
        }

        float next()
        {
            float out = 0.0f;
            for (auto& s : strings)
                if (t >= s.start)
                {
                    auto& v = s.line[s.pos];
                    const float o = v;
                    v = 0.997f * 0.5f * (v + s.prev);
                    s.prev = o;
                    s.pos = (s.pos + 1) % s.line.size();
                    out += 0.22f * o;
                }
            ++t;
            return out;
        }
    };

    struct Signal
    {
        enum Kind { silence, sine, clicks, guitar } kind = silence;
        double hz = 1000.0, amp = 0.5, periodMs = 500.0;
        long n = 0;
        Guitar g;
        static constexpr int burstLen = 96;   // 2 ms of 3 kHz under a Hann window

        static Signal parse (const juce::String& spec)
        {
            Signal s;
            auto p = juce::StringArray::fromTokens (spec, ":", "");
            if (p[0] == "sine")   { s.kind = sine; s.hz = p.size() > 1 ? p[1].getDoubleValue() : 1000.0; s.amp = p.size() > 2 ? p[2].getDoubleValue() : 0.5; }
            if (p[0] == "clicks") { s.kind = clicks; s.periodMs = p.size() > 1 ? p[1].getDoubleValue() : 500.0; }
            if (p[0] == "guitar") { s.kind = guitar; }
            return s;
        }

        // audio thread: guitar strums allocate a little every two seconds (test signal only)
        float next() noexcept
        {
            const long i = n++;
            switch (kind)
            {
                case sine: return (float) (amp * std::sin (juce::MathConstants<double>::twoPi * hz * (double) i / sampleRate));
                case clicks:
                {
                    const long period = (long) (periodMs * 0.001 * sampleRate);
                    const long k = i % period;
                    if (k >= burstLen) return 0.0f;
                    const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) k / burstLen);
                    return (float) (0.8 * w * std::sin (juce::MathConstants<double>::twoPi * 3000.0 * (double) k / sampleRate));
                }
                case guitar:
                    if (i % (long) (2.0 * sampleRate) == 0)
                        g.strum ({ 40, 47, 52, 56, 59, 64 }, 12.0);
                    return g.next();
                case silence: break;
            }
            return 0.0f;
        }
    };

    // rawmidi: ~40 notes for a MIDI-driven synth, the same list in the sender and the orchestrator.
    // Pitches that sound at the same time (release tails included) are a tritone, major 7th, minor 9th or
    // augmented 11th apart, so no harmonic of one lands on another's fundamental (per-note detection by pitch).
    struct Note { int pitch; double startMs, lenMs; int vel; };
    constexpr double noteRelease = 150.0;   // synAmpR in the rawmidi patch

    std::vector<Note> makeNotes()
    {
        juce::Random r (7);
        std::vector<Note> v;
        double t = 1000.0;
        while (v.size() < 40)
        {
            const int count = (v.size() % 6 == 5 && v.size() < 39) ? 2 : 1;   // every sixth: a two-note chord
            for (int c = 0; c < count; ++c)
            {
                const double len = 150.0 + r.nextInt (251);
                int pitch = 0;
                for (int tries = 0; tries < 300 && pitch == 0; ++tries)
                {
                    const int cand = 55 + r.nextInt (25);
                    bool ok = true;
                    for (auto& o : v)
                        if (o.startMs < t + len + noteRelease + 100.0 && o.startMs + o.lenMs + noteRelease + 100.0 > t)
                        {
                            const int d = std::abs (cand - o.pitch);
                            ok = ok && (d == 6 || d == 11 || d == 13 || d == 18);
                        }
                    if (ok) pitch = cand;
                }
                if (pitch != 0)
                    v.push_back ({ pitch, t, len, 70 + r.nextInt (50) });
            }
            t += 130.0 + r.nextInt (200);   // usually shorter than the notes: they overlap
        }
        return v;
    }

    double noteHz (int pitch) { return 440.0 * std::pow (2.0, (pitch - 69) / 12.0); }

    // The standalone app's file player, simulated: plays a test signal as the backing track
    struct SignalTrack : jam::TrackSource
    {
        Signal sig;
        std::atomic<bool> playing { false };
        void renderNextBlock (juce::AudioBuffer<float>& out, int numSamples) noexcept override
        {
            if (! playing)
                return;
            for (int i = 0; i < numSamples; ++i)
            {
                const float v = sig.next();
                for (int ch = 0; ch < juce::jmin (2, out.getNumChannels()); ++ch)
                    out.setSample (ch, i, v);
            }
        }
        bool isPlaying() const noexcept override { return playing; }
    };

    // A host transport at 120 BPM, playing, 4/4
    struct SimPlayHead : juce::AudioPlayHead
    {
        std::atomic<juce::int64> sample { 0 };
        double bpm = 120.0;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            const auto s = sample.load();
            const double ppq = (double) s / sampleRate * bpm / 60.0;
            p.setBpm (bpm);
            p.setTimeSignature (TimeSignature { 4, 4 });
            p.setTimeInSamples (s);
            p.setTimeInSeconds ((double) s / sampleRate);
            p.setPpqPosition (ppq);
            p.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
            p.setIsPlaying (true);
            return p;
        }
    };

    //==============================================================================================
    // Analysis

    // Onsets of click bursts after a quiet hold-off. Level-independent (gain along the way must not look
    // like delay): a coarse threshold finds the burst, its peak sets the level, and the onset is the first
    // sample above 10 % of that peak
    std::vector<juce::int64> detectClicks (const std::vector<float>& x, juce::int64 from, juce::int64 to, double holdMs)
    {
        std::vector<juce::int64> at;
        const auto hold = (juce::int64) (holdMs * 0.001 * sampleRate);
        const auto burst = (juce::int64) Signal::burstLen;
        juce::int64 last = -hold - 1;
        for (auto i = from; i < to; ++i)
            if (std::abs (x[(size_t) i]) > 0.05f)
            {
                if (i - last > hold)
                {
                    float peak = 0.0f;
                    for (auto k = i; k < juce::jmin (to, i + 2 * burst); ++k)
                        peak = juce::jmax (peak, std::abs (x[(size_t) k]));
                    auto onset = juce::jmax (from, i - burst);
                    while (onset < i && std::abs (x[(size_t) onset]) <= 0.1f * peak)
                        ++onset;
                    at.push_back (onset);
                }
                last = i;   // keep holding while it stays loud
            }
        return at;
    }

    struct Quality { double hz = 0, ampDb = -200, snrDb = 0, thdDb = 0; };

    // SNR and THD of a sine: 4-term Blackman-Harris window, 65536-point FFT
    Quality sineQuality (const std::vector<float>& x, juce::int64 from)
    {
        Quality q;
        constexpr int order = 16, N = 1 << order;
        if ((juce::int64) x.size() < from + N)
            return q;
        juce::dsp::FFT fft (order);
        std::vector<float> d ((size_t) N * 2, 0.0f);
        double wsum = 0;
        for (int i = 0; i < N; ++i)
        {
            const double a = juce::MathConstants<double>::twoPi * i / (N - 1);
            const double w = 0.35875 - 0.48829 * std::cos (a) + 0.14128 * std::cos (2 * a) - 0.01168 * std::cos (3 * a);
            wsum += w;
            d[(size_t) i] = (float) (x[(size_t) (from + i)] * w);
        }
        fft.performRealOnlyForwardTransform (d.data(), true);
        std::vector<double> p ((size_t) N / 2);
        for (int k = 0; k < N / 2; ++k)
            p[(size_t) k] = (double) d[(size_t) (2 * k)] * d[(size_t) (2 * k)] + (double) d[(size_t) (2 * k + 1)] * d[(size_t) (2 * k + 1)];
        const double binHz = sampleRate / N;
        const int lo = (int) (20.0 / binHz), hi = (int) (20000.0 / binHz);
        int peak = lo;
        for (int k = lo; k < hi; ++k)
            if (p[(size_t) k] > p[(size_t) peak]) peak = k;
        auto around = [&] (int c) { double s = 0; for (int k = juce::jmax (lo, c - 6); k <= juce::jmin (hi - 1, c + 6); ++k) s += p[(size_t) k]; return s; };
        double total = 0;
        for (int k = lo; k < hi; ++k) total += p[(size_t) k];
        const double fund = juce::jmax (1e-30, around (peak));
        double harm = 0;
        for (int h = 2; h <= 9 && h * peak < hi - 6; ++h) harm += around (h * peak);
        const double noise = juce::jmax (1e-30, total - fund - harm);
        q.hz = peak * binHz;
        // a sine of amplitude A gives |X| ~ A * sum(w) / 2 at its peak; the main lobe spreads that over the bins summed
        q.ampDb = 10.0 * std::log10 (fund / (wsum * wsum / 4.0) / 1.98);
        q.snrDb = 10.0 * std::log10 (fund / noise);
        q.thdDb = 10.0 * std::log10 (juce::jmax (1e-30, harm) / fund);
        return q;
    }

    // Dropouts in a steady sine: amplitude and phase per 1 ms window (I/Q against the nominal frequency),
    // an event wherever either jumps; events closer than 20 ms count once. Returns the event times (s).
    std::vector<double> sineGlitches (const std::vector<float>& x, juce::int64 from, juce::int64 to, double hz, double& ampOut)
    {
        const int W = 48;
        std::vector<double> amp, ph;
        for (auto s = from; s + W <= to; s += W)
        {
            double I = 0, Q = 0;
            for (int i = 0; i < W; ++i)
            {
                const double a = juce::MathConstants<double>::twoPi * hz * (double) (s + i) / sampleRate;
                I += x[(size_t) (s + i)] * std::cos (a);
                Q += x[(size_t) (s + i)] * std::sin (a);
            }
            amp.push_back (2.0 * std::sqrt (I * I + Q * Q) / W);
            ph.push_back (std::atan2 (Q, I));
        }
        std::vector<double> events;
        ampOut = median (amp);
        if (amp.size() < 4 || ampOut < 1e-4)
            return events;
        std::vector<double> dph;
        for (size_t k = 1; k < ph.size(); ++k)
            dph.push_back (std::remainder (ph[k] - ph[k - 1], juce::MathConstants<double>::twoPi));
        const double typical = median (dph);
        size_t lastEvent = 0;
        bool any = false;
        for (size_t k = 1; k < amp.size(); ++k)
        {
            const bool bad = std::abs (amp[k] - ampOut) > 0.25 * ampOut
                          || std::abs (std::remainder (dph[k - 1] - typical, juce::MathConstants<double>::twoPi)) > 0.35;
            if (bad)
            {
                if (! any || k - lastEvent > 20)
                    events.push_back ((double) (from + (juce::int64) k * W) / sampleRate);
                lastEvent = k;
                any = true;
            }
        }
        return events;
    }

    // 1/3-octave band levels (dB), averaged over Hann-windowed 4096-sample frames
    std::vector<double> bandLevels (const std::vector<float>& x, juce::int64 from, juce::int64 to)
    {
        constexpr int order = 12, N = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<double> acc ((size_t) N / 2, 0.0);
        std::vector<float> d ((size_t) N * 2);
        int frames = 0;
        for (auto s = from; s + N <= to; s += N / 2, ++frames)
        {
            std::fill (d.begin(), d.end(), 0.0f);
            for (int i = 0; i < N; ++i)
                d[(size_t) i] = x[(size_t) (s + i)] * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / N));
            fft.performFrequencyOnlyForwardTransform (d.data(), true);
            for (int k = 0; k < N / 2; ++k)
                acc[(size_t) k] += (double) d[(size_t) k] * d[(size_t) k];
        }
        std::vector<double> bands;
        for (int b = -12; b <= 12; ++b)   // 63 Hz .. 16 kHz around 1 kHz
        {
            const double fc = 1000.0 * std::pow (2.0, b / 3.0);
            const double f1 = fc / std::pow (2.0, 1.0 / 6.0), f2 = fc * std::pow (2.0, 1.0 / 6.0);
            double e = 1e-20;
            for (int k = (int) (f1 * N / sampleRate); k <= (int) (f2 * N / sampleRate) && k < N / 2; ++k)
                e += acc[(size_t) k];
            bands.push_back (10.0 * std::log10 (e / juce::jmax (1, frames)));
        }
        return bands;
    }

    //==============================================================================================
    // One player (child process)

    struct PlayerArgs
    {
        bool isHost = true, local = false;   // local: no session at all (checks the harness itself)
        juce::File out;
        int port = jam::defaultPort, expect = 2, block = 128;
        double seconds = 10.0, ppm = 0.0;
        juce::String code, name = "player", signal = "silence", opt, track, patch, view, knob;
        bool notes = false, dump = false;   // --notes: play makeNotes() over MIDI; --dump: write the output (raw float32) next to --out
    };

    jam::LatencyOptions parseOptions (const juce::String& spec)
    {
        jam::LatencyOptions o;
        for (auto& kv : juce::StringArray::fromTokens (spec, ",", ""))
        {
            const auto k = kv.upToFirstOccurrenceOf ("=", false, false).trim();
            const auto v = kv.fromFirstOccurrenceOf ("=", false, false).trim();
            if (k == "comp")   o.compression = v.getIntValue() != 0;
            if (k == "kbps")   o.opusKbps = v.getIntValue();
            if (k == "prot")   o.lossProtection = v.getIntValue() != 0;
            if (k == "packet") o.packetSamples = v.getIntValue();
            if (k == "autojb") o.autoJitterBuffer = v.getIntValue() != 0;
            if (k == "jb")     o.jitterMs = v.getFloatValue();
            if (k == "drift")  o.driftCorrection = v.getIntValue() != 0;
            if (k == "sync")   o.syncTrack = v.getIntValue() != 0;
            if (k == "onebar") o.oneBarMode = v.getIntValue() != 0;
            if (k == "raw")    o.sendMode = v.getIntValue() != 0 ? jam::SendMode::rawInput : jam::SendMode::finishedSound;
        }
        return o;
    }

    // The simulated sound card: blocks paced by this process's clock, running ppm fast or slow
    struct SoundCard : juce::Thread
    {
        ClodScopeProcessor& player;
        ClodScopeProcessor* trackInstance = nullptr;   // FL-style: an instance on the Master
        SignalTrack* trackSource = nullptr;            // standalone-style: the file player
        Signal input, trackSignal;
        SimPlayHead head;
        int block;
        double ppm;
        std::atomic<bool> go { false };
        std::atomic<juce::int64> goSample { -1 };
        std::vector<std::pair<juce::int64, juce::MidiMessage>> midiEvents;   // sample after go, sorted
        size_t nextMidi = 0;

        // what this player hears (preallocated); the input is regenerated after the run
        std::vector<float> in, out;
        std::atomic<juce::int64> written { 0 };
        double startMs = 0.0, blockMs = 0.0;
        std::atomic<double> busyMs { 0.0 };   // inside processBlock
        std::atomic<int> overruns { 0 };

        SoundCard (ClodScopeProcessor& p, int blockSize, double ppmOffset, double captureSeconds)
            : juce::Thread ("sim sound card"), player (p), block (blockSize), ppm (ppmOffset)
        {
            const auto n = (size_t) ((captureSeconds + 2.0) * sampleRate);
            in.assign (n, 0.0f);
            out.assign (n, 0.0f);
            blockMs = block * 1000.0 / (sampleRate * (1.0 + ppm * 1e-6));
        }

        void run() override
        {
            timeBeginPeriod (1);
            juce::AudioBuffer<float> buf (2, block), tbuf (2, block);
            juce::MidiBuffer midi;
            midi.ensureSize (4096);
            startMs = nowMs() + 20.0;
            juce::int64 n = 0;
            for (juce::int64 k = 0; ! threadShouldExit(); ++k)
            {
                const double deadline = startMs + (double) k * blockMs;
                for (double t = nowMs(); t < deadline; t = nowMs())
                {
                    if (deadline - t > 1.5) juce::Thread::sleep (1);
                    else juce::Thread::yield();
                }
                if (go && goSample < 0)
                    goSample = n;
                const bool on = go;
                for (int i = 0; i < block; ++i)
                {
                    const float v = on ? input.next() : 0.0f;
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }
                if (trackSource != nullptr)
                    trackSource->playing = on;
                const double t0 = nowMs();
                head.sample = n;
                midi.clear();
                if (on)
                    for (const auto rel = n - goSample.load(); nextMidi < midiEvents.size() && midiEvents[nextMidi].first < rel + block; ++nextMidi)
                        midi.addEvent (midiEvents[nextMidi].second, (int) juce::jmax ((juce::int64) 0, midiEvents[nextMidi].first - rel));
                player.processBlock (buf, midi);
                if (trackInstance != nullptr)
                {
                    // FL: the track-role instance sits on the insert that carries only the beat; the Master
                    // sums that insert and the guitar insert (this player's instance)
                    for (int i = 0; i < block; ++i)
                    {
                        const float v = on ? trackSignal.next() : 0.0f;
                        tbuf.setSample (0, i, v);
                        tbuf.setSample (1, i, v);
                    }
                    midi.clear();
                    trackInstance->processBlock (tbuf, midi);
                    buf.addFrom (0, 0, tbuf, 0, 0, block);
                    buf.addFrom (1, 0, tbuf, 1, 0, block);
                }
                const double t1 = nowMs();
                busyMs = busyMs + (t1 - t0);
                if (t1 > deadline + blockMs)
                    ++overruns;
                for (int i = 0; i < block && (size_t) (n + i) < out.size(); ++i)
                    out[(size_t) (n + i)] = buf.getSample (0, i);
                n += block;
                written = n;
            }
            timeEndPeriod (1);
        }

        double sampleToMs (juce::int64 s) const { return startMs + (double) s * 1000.0 / (sampleRate * (1.0 + ppm * 1e-6)); }
    };

    struct PeerSeries { std::vector<double> ping, net, jit, buf, total, loss, drift, level; bool connected = false; };

    int runPlayer (const PlayerArgs& a)
    {
        if (juce::SystemStats::getEnvironmentVariable ("CLOD_JAM_BIND", {}).isEmpty())
            setEnv ("CLOD_JAM_BIND", "127.0.0.1");   // never all interfaces: that makes Windows Firewall ask

        juce::StringArray result, log;
        auto writeResult = [&]
        {
            a.out.withFileExtension ("log").replaceWithText (log.joinIntoString ("\n") + "\n");
            a.out.replaceWithText (result.joinIntoString ("\n") + "\n");
        };
        auto put = [&] (const juce::String& k, const juce::String& v) { result.add (k + "=" + v); };
        put ("name", a.name);
        put ("role", a.isHost ? "host" : "join");

        std::unique_ptr<juce::AudioProcessor> base (createPluginFilter());
        auto& proc = dynamic_cast<ClodScopeProcessor&> (*base);
        proc.setPlayConfigDetails (2, 2, sampleRate, a.block);

        std::unique_ptr<juce::AudioProcessor> trackBase;
        SignalTrack fileTrack;
        const auto trackKind = a.track.upToFirstOccurrenceOf (":", false, false);
        const auto trackSig = a.track.fromFirstOccurrenceOf (":", false, false);
        if (trackKind == "fl")
        {
            trackBase.reset (createPluginFilter());
            trackBase->setPlayConfigDetails (2, 2, sampleRate, a.block);
            if (auto* port = dynamic_cast<ClodScopeProcessor&> (*trackBase).getJamPort())
                port->setRole (jam::Role::track);
        }
        else if (trackKind == "file")
        {
            fileTrack.sig = Signal::parse (trackSig);
            if (auto* port = proc.getJamPort())
                port->setTrackSource (&fileTrack);
        }

        auto set = [] (ClodScopeProcessor& p, const char* id, float realValue)
        {
            if (auto* prm = p.apvts.getParameter (id))
                prm->setValueNotifyingHost (prm->convertTo0to1 (realValue));
        };
        if (a.patch == "midisynth")
        {
            set (proc, "synOn", 1.0f);
            set (proc, "synSource", 1.0f);   // MIDI
            set (proc, "synLayer", 2.0f);    // replace: only the synth is heard
            set (proc, "synAmpR", (float) noteRelease);
        }
        if (a.patch == "drive")
        {
            set (proc, "drive", 60.0f);
            set (proc, "shape", 4.0f);    // asymmetric: even harmonics too
            set (proc, "hpf", 120.0f);
            set (proc, "lpf", 5000.0f);
        }
        if (a.view.isNotEmpty())
        {
            auto v = juce::StringArray::fromTokens (a.view, ",", "");
            set (proc, "mainView", v[0].getFloatValue());
            if (v.size() > 1)
                set (proc, "v3Mode", v[1].getFloatValue());
        }

        juce::SharedResourcePointer<jam::Session> session;
        session->setOptions (parseOptions (a.opt));

        SoundCard card (proc, a.block, a.ppm, a.seconds + 25.0);
        card.input = Signal::parse (a.signal);
        if (a.notes)
        {
            for (auto& nt : makeNotes())
            {
                card.midiEvents.push_back ({ (juce::int64) (nt.startMs * 0.001 * sampleRate), juce::MidiMessage::noteOn (1, nt.pitch, (juce::uint8) nt.vel) });
                card.midiEvents.push_back ({ (juce::int64) ((nt.startMs + nt.lenMs) * 0.001 * sampleRate), juce::MidiMessage::noteOff (1, nt.pitch) });
            }
            std::stable_sort (card.midiEvents.begin(), card.midiEvents.end(), [] (auto& x, auto& y) { return x.first < y.first; });
        }
        if (trackBase != nullptr)
        {
            card.trackInstance = dynamic_cast<ClodScopeProcessor*> (trackBase.get());
            card.trackSignal = Signal::parse (trackSig);
            card.trackInstance->setPlayHead (&card.head);
        }
        if (trackKind == "file")
            card.trackSource = &fileTrack;
        proc.setPlayHead (&card.head);
        proc.prepareToPlay (sampleRate, a.block);
        if (trackBase != nullptr)
            trackBase->prepareToPlay (sampleRate, a.block);
        card.startThread (juce::Thread::Priority::highest);

        auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
        pump (200);

        juce::String error;
        const double joinStart = nowMs();
        const bool ok = a.local || (a.isHost ? session->host (a.name, a.port, error) : session->join (a.code, a.name, error));
        log.add ("start: " + juce::String (ok ? "ok" : "failed: " + error) + " | " + session->statusText());
        if (! ok)
        {
            put ("error", (a.isHost ? "host: " : "join: ") + error);
            card.stopThread (2000);
            writeResult();
            return 0;
        }
        if (a.isHost && ! a.local)
        {
            auto code = session->sessionCode();
            if (code.isEmpty())
                code = "127.0.0.1:" + juce::String (a.port);
            put ("code", code);
            a.out.withFileExtension ("code").replaceWithText (code);
        }

        // wait for everyone
        double joinMs = -1.0;
        std::vector<jam::PeerInfo> ps;
        for (double t = nowMs(); t - joinStart < 20000.0; t = nowMs())
        {
            pump (5);
            ps = session->players();
            int in = 0;
            for (auto& p : ps)
                if (p.connected || p.isSelf) ++in;
            const auto st = session->status();
            if (a.local || in >= a.expect && (st == jam::Status::connected || (a.isHost && st == jam::Status::hosting)))
            {
                joinMs = t - joinStart;
                break;
            }
            if (st == jam::Status::failed)
                break;
        }
        log.add ("after join: " + session->statusText());
        put ("joinMs", fmt (joinMs));
        put ("status", session->statusText());
        {
            juce::StringArray names;
            for (auto& p : ps)
                names.add (p.name + (p.isSelf ? "*" : "") + (p.isHost ? "(host)" : "") + (p.connected || p.isSelf ? "" : "(not connected)"));
            put ("players", names.joinIntoString (" "));
            put ("playerCount", juce::String ((int) ps.size()));
        }
        if (joinMs < 0.0)
        {
            put ("error", "not everyone joined within 20 s (" + juce::String ((int) ps.size()) + " of " + juce::String (a.expect) + " players): "
                          + session->statusText());
            session->leave();
            card.stopThread (2000);
            writeResult();
            return 0;
        }

        // play and measure
        card.go = true;
        const double c0 = processCpuMs(), t0 = nowMs(), busy0 = card.busyMs;
        std::map<juce::String, PeerSeries> series;
        // --knob id:value:seconds moves a parameter mid-run (exercises parameter updates for raw-input replicas)
        auto knob = juce::StringArray::fromTokens (a.knob, ":", "");
        bool knobDone = a.knob.isEmpty();
        while (nowMs() - t0 < a.seconds * 1000.0)
        {
            pump (knobDone ? 250 : 20);
            if (! knobDone && nowMs() - t0 >= knob[2].getDoubleValue() * 1000.0)
            {
                set (proc, knob[0].toRawUTF8(), knob[1].getFloatValue());
                knobDone = true;
                log.add ("knob " + a.knob + " moved");
            }
            if (nowMs() - t0 < 1500.0)
                continue;   // let buffers settle
            for (auto& p : session->players())
            {
                if (p.isSelf) continue;
                auto& s = series[p.name];
                s.connected = p.connected;
                s.ping.push_back (p.pingMs);   s.net.push_back (p.networkMs);  s.jit.push_back (p.jitterMs);
                s.buf.push_back (p.bufferMs);  s.total.push_back (p.totalMs);  s.loss.push_back (p.lossPercent);
                s.drift.push_back (p.driftPpm); s.level.push_back (p.levelDb);
            }
        }
        const double wall = nowMs() - t0;
        put ("cpuPercent", fmt ((processCpuMs() - c0) / wall * 100.0, 2));
        put ("audioLoadPercent", fmt ((card.busyMs - busy0) / wall * 100.0, 2));
        put ("overruns", juce::String (card.overruns.load()));
        put ("finalStatus", session->statusText());
        card.stopThread (3000);   // stop the sound before leaving, so the capture ends inside the session
        session->leave();
        pump (100);

        for (auto& [name, s] : series)
        {
            auto mm = [] (const std::vector<double>& v, bool lo) { return v.empty() ? 0.0 : lo ? *std::min_element (v.begin(), v.end()) : *std::max_element (v.begin(), v.end()); };
            put ("peer." + name, "connected:" + juce::String (s.connected ? 1 : 0)
                 + ",ping:" + fmt (median (s.ping), 2) + ",net:" + fmt (median (s.net), 2) + ",jitter:" + fmt (median (s.jit), 2)
                 + ",buffer:" + fmt (median (s.buf), 2) + ",bufferMin:" + fmt (mm (s.buf, true), 2) + ",bufferMax:" + fmt (mm (s.buf, false), 2)
                 + ",bufferFirst:" + fmt (s.buf.empty() ? 0.0 : s.buf.front(), 2) + ",bufferLast:" + fmt (s.buf.empty() ? 0.0 : s.buf.back(), 2)
                 + ",total:" + fmt (median (s.total), 2) + ",loss:" + fmt (s.loss.empty() ? 0.0 : s.loss.back(), 2)
                 + ",drift:" + fmt (median (s.drift)) + ",level:" + fmt (median (s.level)));
        }

        // ---- analysis of the captures ----
        const auto n = juce::jmin ((juce::int64) card.out.size(), card.written.load());
        const auto go0 = juce::jmax ((juce::int64) 0, card.goSample.load());
        {
            // the input exactly as it was played (the card thread is stopped)
            Signal again = Signal::parse (a.signal);
            for (auto s = go0; s < n; ++s)
                card.in[(size_t) s] = again.next();
        }
        const auto settle = go0 + (juce::int64) (2.0 * sampleRate);
        put ("startMs", juce::String (card.sampleToMs (0), 3));
        put ("goMs", juce::String (card.sampleToMs (go0), 3));
        put ("measuredSeconds", fmt ((double) (n - go0) / sampleRate, 2));

        const double holdMs = 200.0;
        {
            std::vector<double> ti, to;
            if (card.input.kind == Signal::clicks)
                for (auto s : detectClicks (card.in, go0, n, holdMs)) ti.push_back (card.sampleToMs (s));
            for (auto s : detectClicks (card.out, go0, n, holdMs)) to.push_back (card.sampleToMs (s));
            put ("clicksIn", joinNums (ti));
            put ("clicksOut", joinNums (to));
        }
        if (settle + 70000 < n)
        {
            const auto q = sineQuality (card.out, n - 70000);
            put ("outSine", "hz:" + fmt (q.hz, 1) + ",ampDb:" + fmt (q.ampDb) + ",snrDb:" + fmt (q.snrDb) + ",thdDb:" + fmt (q.thdDb));
            double amp = 0.0;
            const auto sig = Signal::parse (a.signal);
            const auto ev = sineGlitches (card.out, settle, n, sig.kind == Signal::sine ? sig.hz : 1000.0, amp);
            std::vector<double> firstFew (ev.begin(), ev.begin() + (std::ptrdiff_t) juce::jmin ((size_t) 12, ev.size()));
            put ("glitches", juce::String ((int) ev.size()));
            put ("glitchAt", joinNums (firstFew, 3));
            put ("sineAmp", fmt (amp, 4));
        }
        if (a.dump && go0 < n)
        {
            juce::FileOutputStream f (a.out.withFileExtension ("f32"));
            if (f.openedOk())
            {
                f.setPosition (0);
                f.truncate();
                f.write (card.out.data() + go0, (size_t) (n - go0) * sizeof (float));
            }
            put ("dumpStartMs", juce::String (card.sampleToMs (go0), 3));
        }
        if (settle < n)
            put ("bands", joinNums (bandLevels (card.out, settle, n), 2));

        writeResult();
        return 0;
    }

    //==============================================================================================
    // Orchestrator

    juce::StringPairArray readResult (const juce::File& f)
    {
        juce::StringPairArray r;
        juce::StringArray lines;
        lines.addLines (f.loadFileAsString());
        for (auto& l : lines)
            if (l.containsChar ('='))
                r.set (l.upToFirstOccurrenceOf ("=", false, false), l.fromFirstOccurrenceOf ("=", false, false));
        return r;
    }

    // "a:1,b:2" -> value of key
    double sub (const juce::String& s, const juce::String& key)
    {
        for (auto& kv : juce::StringArray::fromTokens (s, ",", ""))
            if (kv.upToFirstOccurrenceOf (":", false, false) == key)
                return kv.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
        return 0.0;
    }

    struct Run
    {
        struct P { juce::String name; juce::StringArray args; juce::File out; std::unique_ptr<juce::ChildProcess> proc; };
        juce::String name, netsim;
        int port = 0;
        double seconds = 10.0;
        std::vector<P> players;   // [0] hosts
        std::vector<juce::StringPairArray> results;
        juce::String error;       // the host could not start / somebody could not join
        bool notImplemented = false;
        juce::String repro;

        void add (const juce::String& n, juce::StringArray a) { players.push_back ({ n, std::move (a), {}, nullptr }); }
        juce::StringPairArray& r (int i) { return results[(size_t) i]; }
    };

    struct Orchestrator
    {
        juce::File outDir, exe;
        juce::StringArray summary;
        // others may run ClodJamTest at the same time: start somewhere random, and move on when a port is taken
        int nextPort = 40000 + 2 * juce::Random::getSystemRandom().nextInt (10000);

        void line (const juce::String& s)
        {
            std::cout << s << std::endl;
            summary.add (s);
        }

        juce::StringArray argsFor (Run& run, Run::P& p, bool host, const juce::String& code)
        {
            juce::StringArray a;
            a.add (exe.getFullPathName());
            a.add ("--player"); a.add (host ? "host" : "join");
            a.add ("--out"); a.add (p.out.getFullPathName());
            a.add ("--name"); a.add (p.name);
            a.add ("--port"); a.add (juce::String (run.port));
            a.add ("--expect"); a.add (juce::String ((int) run.players.size()));
            a.add ("--seconds"); a.add (juce::String (run.seconds + (host ? 3.0 : 0.0)));
            if (! host) { a.add ("--code"); a.add (code); }
            a.addArray (p.args);
            return a;
        }

        static juce::String quoteArgs (const juce::StringArray& a)
        {
            juce::StringArray q;
            for (auto& s : a) q.add (s.containsChar (' ') ? s.quoted() : s);
            return q.joinIntoString (" ");
        }

        void start (Run& run)
        {
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                run.error = {};
                startOnce (run);
                if (! run.error.containsIgnoreCase ("in use"))
                    return;
                if (run.players[0].proc != nullptr)
                    run.players[0].proc->waitForProcessToFinish (5000);
            }
        }

        void startOnce (Run& run)
        {
            run.port = nextPort;
            nextPort += 2;
            setEnv ("CLOD_JAM_BIND", "127.0.0.1");
            setEnv ("CLOD_JAM_NETSIM", run.netsim);
            for (auto& p : run.players)
            {
                p.out = outDir.getChildFile (run.name + "_" + p.name + ".txt");
                p.out.deleteFile();
                p.out.withFileExtension ("code").deleteFile();
                p.out.withFileExtension ("log").deleteFile();
            }
            run.repro = "(cmd) set CLOD_JAM_BIND=127.0.0.1& set CLOD_JAM_NETSIM=" + run.netsim + "& ";
            auto& host = run.players[0];
            auto hostArgs = argsFor (run, host, true, {});
            run.repro += quoteArgs (hostArgs);
            host.proc = std::make_unique<juce::ChildProcess>();
            if (! host.proc->start (hostArgs, 0))
            {
                run.error = "could not start a child process";
                return;
            }
            const auto codeFile = host.out.withFileExtension ("code");
            const double t0 = nowMs();
            while (! codeFile.existsAsFile() && ! host.out.existsAsFile() && nowMs() - t0 < 20000.0)
                juce::Thread::sleep (20);
            juce::Thread::sleep (30);
            if (! codeFile.existsAsFile())
            {
                auto r = readResult (host.out);
                run.error = r["error"].isNotEmpty() ? r["error"] : juce::String ("the host wrote no session code within 20 s");
                run.notImplemented = run.error.containsIgnoreCase ("not built yet") || run.error.containsIgnoreCase ("not implemented");
                return;
            }
            const auto code = codeFile.loadFileAsString().trim();
            for (size_t i = 1; i < run.players.size(); ++i)
            {
                auto& p = run.players[i];
                auto a = argsFor (run, p, false, code);
                if (i == 1)
                    run.repro += "  +  " + quoteArgs (a);
                p.proc = std::make_unique<juce::ChildProcess>();
                p.proc->start (a, 0);
            }
        }

        void finish (Run& run)
        {
            const double limit = (run.seconds + 60.0) * 1000.0, t0 = nowMs();
            for (auto& p : run.players)
                if (p.proc != nullptr)
                {
                    const double left = limit - (nowMs() - t0);
                    if (! p.proc->waitForProcessToFinish ((int) juce::jmax (1000.0, left)))
                        p.proc->kill();
                }
            for (auto& p : run.players)
                run.results.push_back (readResult (p.out));
            if (run.error.isEmpty())
                for (auto& r : run.results)
                {
                    if (r["name"].isEmpty())
                    {
                        run.error = "a player wrote no result (crashed or hung?)";
                        break;
                    }
                    if (r["error"].isNotEmpty())
                    {
                        run.error = r["name"] + ": " + r["error"];
                        run.notImplemented = r["error"].containsIgnoreCase ("not built yet");
                        break;
                    }
                }
        }

        void exec (Run& run) { start (run); finish (run); }

        // a case that could not run: one line, and the repro
        bool failed (Run& run, const juce::String& caseName)
        {
            if (run.error.isEmpty())
                return false;
            line (caseName + ": " + (run.notImplemented ? "NOT IMPLEMENTED" : "FAIL") + " (" + run.error + ")");
            if (! run.notImplemented)
                line ("    repro: " + run.repro);
            return true;
        }

        // latencies: each detected click out -> the latest click in before it
        static std::vector<double> matchClicks (const std::vector<double>& in, const std::vector<double>& out, double maxMs)
        {
            std::vector<double> lat;
            for (auto t : out)
            {
                double best = -1.0;
                for (auto s : in)
                    if (s <= t + 0.5) best = s;
                if (best >= 0.0 && t - best < maxMs)
                    lat.push_back (t - best);
            }
            return lat;
        }

        juce::StringArray common (const juce::String& signal, const juce::String& opt, int block = 128, double ppm = 0.0)
        {
            juce::StringArray a;
            a.add ("--signal"); a.add (signal);
            if (opt.isNotEmpty()) { a.add ("--opt"); a.add (opt); }
            a.add ("--block"); a.add (juce::String (block));
            if (ppm != 0.0) { a.add ("--ppm"); a.add (juce::String (ppm)); }
            return a;
        }

        // ---- cases ----

        // the harness itself, no session: one player hears its own sound (plugin latency only)
        void local()
        {
            for (const char* sig : { "sine:997:0.5", "clicks:500" })
            {
                Run run;
                run.name = juce::String ("local_") + (juce::String (sig).startsWith ("sine") ? "sine" : "clicks");
                run.seconds = 8.0;
                Run::P p { "solo", common (sig, {}), outDir.getChildFile (run.name + "_solo.txt"), nullptr };
                auto a = argsFor (run, p, true, {});
                a.set (a.indexOf ("host"), "local");
                p.out.deleteFile();
                juce::ChildProcess c;
                c.start (a, 0);
                c.waitForProcessToFinish (60000);
                auto r = readResult (p.out);
                const auto lat = matchClicks (parseNums (r["clicksIn"]), parseNums (r["clicksOut"]), 500.0);
                line (run.name + ": INFO - own output: " + r["outSine"] + ", dropouts " + r["glitches"] + ", clicks in/out "
                      + juce::String ((int) parseNums (r["clicksIn"]).size()) + "/" + juce::String ((int) parseNums (r["clicksOut"]).size())
                      + " latency " + fmt (median (lat), 2) + " ms, overruns " + r["overruns"] + ", audio thread " + r["audioLoadPercent"]
                      + " %, process CPU " + r["cpuPercent"] + " %");
            }
        }

        double cpu2 = -1, cpu4 = -1, cpuRaw4 = -1;

        void connect (int players, double seconds)
        {
            const auto caseName = "connect" + juce::String (players);
            Run run;
            run.name = caseName;
            run.seconds = seconds;
            for (int i = 0; i < players; ++i)
                run.add (i == 0 ? juce::String ("host") : "p" + juce::String (i + 1), common ("sine:" + juce::String (440 + 110 * i) + ":0.3", {}));
            exec (run);
            if (failed (run, caseName))
                return;
            double worstJoin = 0;
            std::vector<double> pings;
            bool allSeeAll = true;
            double load = 0, cpu = 0;
            for (int i = 0; i < players; ++i)
            {
                auto& r = run.r (i);
                if (i > 0) worstJoin = juce::jmax (worstJoin, r["joinMs"].getDoubleValue());
                if (r["playerCount"].getIntValue() != players) allSeeAll = false;
                for (auto& k : r.getAllKeys())
                    if (k.startsWith ("peer."))
                    {
                        pings.push_back (sub (r[k], "ping"));
                        if (sub (r[k], "connected") < 0.5) allSeeAll = false;
                    }
                load += r["audioLoadPercent"].getDoubleValue();
                cpu += r["cpuPercent"].getDoubleValue();
            }
            (players == 2 ? cpu2 : cpu4) = load / players;
            const double ping = median (pings);
            line (caseName + ": " + (allSeeAll && ping > 0.0 ? "PASS" : "FAIL") + " - join " + fmt (worstJoin, 0) + " ms (slowest), ping "
                  + fmt (ping, 2) + " ms (median), players seen by host: " + run.r (0)["players"]
                  + (ping <= 0.0 ? " [ping is 0: PeerInfo::pingMs not filled]" : "")
                  + ", audio thread " + fmt (load / players, 2) + " %, process CPU " + fmt (cpu / players, 1) + " % (per player)");
            if (! allSeeAll || ping <= 0.0)
                line ("    repro: " + run.repro);
        }

        void latency (const juce::String& caseName, const juce::String& opt, const juce::String& netsim, double seconds, double clickMs, double expectMs)
        {
            Run run;
            run.name = caseName;
            run.netsim = netsim;
            run.seconds = seconds;
            run.add ("host", common ("clicks:" + juce::String (clickMs), opt));
            run.add ("p2", common ("silence", opt));
            exec (run);
            if (failed (run, caseName))
                return;
            const auto in = parseNums (run.r (0)["clicksIn"]), out = parseNums (run.r (1)["clicksOut"]);
            const auto lat = matchClicks (in, out, clickMs);
            const double total = sub (run.r (1)["peer.host"], "total");
            const double med = median (lat);
            double lo = 1e9, hi = 0;
            for (auto l : lat) { lo = juce::jmin (lo, l); hi = juce::jmax (hi, l); }
            // clicks the receiver could hear: sent while it was listening, with time to arrive before it stopped
            const double rxGo = run.r (1)["goMs"].getDoubleValue();
            const double rxEnd = rxGo + run.r (1)["measuredSeconds"].getDoubleValue() * 1000.0;
            int sent = 0;
            for (auto t : in)
                if (t >= rxGo && t + juce::jmax (expectMs, 0.0) + 300.0 <= rxEnd)
                    ++sent;
            const bool heard = sent > 0 && (double) lat.size() >= 0.9 * sent;
            const int heardCount = juce::jmin ((int) lat.size(), sent);
            juce::String verdict;
            if (! heard) verdict = "FAIL";
            else if (expectMs > 0) verdict = std::abs (med - expectMs) <= 5.0 ? "PASS" : "FAIL";
            else verdict = std::abs (med - total) <= 5.0 ? "PASS" : "MISMATCH";
            line (caseName + ": " + verdict + " - " + juce::String (heardCount) + "/" + juce::String (sent) + " clicks heard, click in -> out "
                  + fmt (med, 2) + " ms median (" + fmt (lat.empty() ? 0 : lo, 2) + " .. " + fmt (hi, 2) + "), UI totalMs "
                  + fmt (total, 2) + (expectMs > 0 ? ", expected " + fmt (expectMs, 1) : juce::String())
                  + " | " + run.r (1)["peer.host"]);
            if (verdict != "PASS")
                line ("    repro: " + run.repro);
        }

        void quality (const juce::String& caseName, const juce::String& opt, double minSnr)
        {
            Run run;
            run.name = caseName;
            run.seconds = 8.0;
            run.add ("host", common ("sine:997:0.5", opt));
            run.add ("p2", common ("silence", opt));
            exec (run);
            if (failed (run, caseName))
                return;
            const auto& rx = run.r (1)["outSine"];
            const auto& tx = run.r (0)["outSine"];
            const bool ok = sub (rx, "snrDb") >= minSnr && std::abs (sub (rx, "hz") - 997.0) < 2.0;
            line (caseName + ": " + (ok ? "PASS" : "FAIL") + " - received 997 Hz sine: SNR " + fmt (sub (rx, "snrDb")) + " dB, THD "
                  + fmt (sub (rx, "thdDb")) + " dB, level " + fmt (sub (rx, "ampDb")) + " dBFS (sender's own output: SNR "
                  + fmt (sub (tx, "snrDb")) + " dB, THD " + fmt (sub (tx, "thdDb")) + " dB), dropouts " + run.r (1)["glitches"]
                  + ", need SNR >= " + fmt (minSnr, 0));
            if (! ok)
                line ("    repro: " + run.repro);
        }

        // dropouts in a steady sine through a bad network
        void dropouts (const juce::String& caseName, const juce::String& opt, const juce::String& netsim, double seconds, int maxGlitches)
        {
            Run run;
            run.name = caseName;
            run.netsim = netsim;
            run.seconds = seconds;
            run.add ("host", common ("sine:1000:0.5", opt));
            run.add ("p2", common ("silence", opt));
            exec (run);
            if (failed (run, caseName))
                return;
            auto& r = run.r (1);
            const int g = r["glitches"].getIntValue();
            const auto verdict = maxGlitches < 0 ? juce::String ("INFO") : g <= maxGlitches ? juce::String ("PASS") : juce::String ("FAIL");
            line (caseName + ": " + verdict + " - " + juce::String (g) + " dropouts in " + fmt (seconds - 2.0, 0) + " s (netsim " + netsim
                  + (opt.isNotEmpty() ? ", " + opt : juce::String()) + "), amp " + r["sineAmp"] + " | " + r["peer.host"]
                  + (g > 0 ? " | at s: " + r["glitchAt"] : juce::String()));
            if (verdict == "FAIL")
                line ("    repro: " + run.repro);
        }

        void drift (double seconds)
        {
            Run on, off;
            on.name = "drift_on";   on.seconds = seconds;
            off.name = "drift_off"; off.seconds = seconds;
            on.add ("host", common ("sine:1000:0.5", "drift=1", 128, 100.0));
            on.add ("p2", common ("silence", "drift=1", 128, -100.0));
            off.add ("host", common ("sine:1000:0.5", "drift=0", 128, 100.0));
            off.add ("p2", common ("silence", "drift=0", 128, -100.0));
            start (on);
            start (off);
            finish (on);
            finish (off);
            for (auto* run : { &on, &off })
            {
                const auto caseName = juce::String (run == &on ? "drift (correction on)" : "drift (correction off)");
                if (failed (*run, caseName))
                    continue;
                auto& r = run->r (1);
                const auto& p = r["peer.host"];
                const int g = r["glitches"].getIntValue();
                const double range = sub (p, "bufferMax") - sub (p, "bufferMin");
                const bool ok = g == 0 && range < 10.0;
                const auto verdict = run == &on ? juce::String (ok ? "PASS" : "FAIL") : juce::String ("INFO");
                line (caseName + ": " + verdict + " - sender +100 ppm, receiver -100 ppm, " + fmt (seconds, 0) + " s: " + juce::String (g)
                      + " dropouts, buffer " + fmt (sub (p, "bufferFirst")) + " -> " + fmt (sub (p, "bufferLast")) + " ms (range "
                      + fmt (range) + "), drift seen " + fmt (sub (p, "drift")) + " ppm, total " + fmt (sub (p, "total")) + " ms"
                      + (g > 0 ? " | dropouts at s: " + r["glitchAt"] : juce::String()));
                if (verdict == "FAIL")
                    line ("    repro: " + run->repro);
            }
        }

        void raw (double seconds)
        {
            // the sender's finished sound (drive, asymmetric shape, filters) vs what the receiver rebuilds
            for (int players : { 2, 4 })
            {
                const auto caseName = juce::String (players == 2 ? "raw" : "raw4");
                Run run;
                run.name = caseName;
                run.seconds = seconds;
                auto hostArgs = common ("guitar", "raw=1");
                hostArgs.add ("--patch"); hostArgs.add ("drive");
                hostArgs.add ("--knob"); hostArgs.add ("lpf:2500:1.0");   // before the analysis window (from 2 s)
                run.add ("host", hostArgs);
                for (int i = 1; i < players; ++i)
                    run.add ("p" + juce::String (i + 1), common (players == 2 ? juce::String ("silence") : "sine:" + juce::String (330 * i) + ":0.2", "raw=1"));
                exec (run);
                if (failed (run, caseName))
                    continue;
                if (players == 4)
                {
                    double load = 0;
                    for (auto& r : run.results) load += r["audioLoadPercent"].getDoubleValue();
                    cpuRaw4 = load / players;
                    continue;
                }
                const auto tx = parseNums (run.r (0)["bands"]), rx = parseNums (run.r (1)["bands"]);
                if (tx.size() != rx.size() || tx.empty())
                {
                    line (caseName + ": FAIL - no spectra");
                    continue;
                }
                const double top = *std::max_element (tx.begin(), tx.end());
                double sum = 0, worst = 0;
                int used = 0;
                for (size_t b = 0; b < tx.size(); ++b)
                    if (tx[b] > top - 40.0)
                    {
                        const double d = std::abs (tx[b] - rx[b]);
                        sum += d; worst = juce::jmax (worst, d); ++used;
                    }
                const double mean = used > 0 ? sum / used : 99.0;
                const bool ok = mean < 1.0 && worst < 3.0;
                line (caseName + ": " + (ok ? "PASS" : "FAIL") + " - rebuilt sound vs sender's finished sound, 1/3-octave bands within 40 dB of the peak ("
                      + juce::String (used) + "): mean diff " + fmt (mean, 2) + " dB, worst " + fmt (worst, 2) + " dB"
                      + " | " + run.r (1)["peer.host"]);
                if (! ok)
                {
                    line ("    sender bands:   " + run.r (0)["bands"]);
                    line ("    receiver bands: " + run.r (1)["bands"]);
                    line ("    repro: " + run.repro);
                }
            }
        }

        static std::vector<float> loadF32 (const juce::File& f)
        {
            juce::MemoryBlock mb;
            f.loadFileAsData (mb);
            std::vector<float> v (mb.getSize() / sizeof (float));
            if (! v.empty())
                std::memcpy (v.data(), mb.getData(), v.size() * sizeof (float));
            return v;
        }

        // magnitude at hz over [from, from + len), Hann window (Goertzel)
        static double tone (const std::vector<float>& x, juce::int64 from, int len, double hz)
        {
            if (from < 0 || len < 16 || from + len > (juce::int64) x.size())
                return 0.0;
            const double w = juce::MathConstants<double>::twoPi * hz / sampleRate, c = 2.0 * std::cos (w);
            double s1 = 0, s2 = 0;
            for (int i = 0; i < len; ++i)
            {
                const double win = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (len - 1));
                const double s0 = x[(size_t) (from + i)] * win + c * s1 - s2;
                s2 = s1; s1 = s0;
            }
            return std::sqrt (juce::jmax (0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / len;
        }

        // first time (ms on the shared clock) the note's pitch rises above `level` within [fromMs, toMs]
        static double onsetMs (const std::vector<float>& x, double startMs, double fromMs, double toMs, double hz, double level)
        {
            constexpr int W = 2048, hop = 64;
            for (double t = fromMs; t <= toMs; t += hop * 1000.0 / sampleRate)
            {
                const auto centre = (juce::int64) ((t - startMs) * 0.001 * sampleRate);
                if (tone (x, centre - W / 2, W, hz) > level)
                    return t;
            }
            return -1.0;
        }

        // A MIDI-played synth in raw-input mode: every note must sound at the friend, none may hang
        void rawMidi (const juce::String& caseName, const juce::String& opt, const juce::String& netsim)
        {
            Run run;
            run.name = caseName;
            run.netsim = netsim;
            run.seconds = 20.0;
            auto tx = common ("silence", opt);
            tx.addArray ({ "--patch", "midisynth", "--notes", "--dump" });
            auto rx = common ("silence", opt);
            rx.add ("--dump");
            run.add ("host", tx);
            run.add ("p2", rx);
            exec (run);
            if (failed (run, caseName))
                return;
            const auto ref = loadF32 (run.players[0].out.withFileExtension ("f32"));
            const auto got = loadF32 (run.players[1].out.withFileExtension ("f32"));
            const double ts = run.r (0)["dumpStartMs"].getDoubleValue(), tr = run.r (1)["dumpStartMs"].getDoubleValue();
            const auto notes = makeNotes();
            if (ref.empty() || got.empty())
            {
                line (caseName + ": FAIL - no audio dumps");
                line ("    repro: " + run.repro);
                return;
            }

            // onsets in the sender's own output, then where the same note starts at the friend
            std::vector<double> refOn (notes.size(), -1.0), peak (notes.size(), 0.0), diffs;
            for (size_t k = 0; k < notes.size(); ++k)
            {
                const auto& nt = notes[k];
                const double hz = noteHz (nt.pitch), t0 = ts + nt.startMs;
                for (double t = t0; t < t0 + nt.lenMs; t += 5.0)
                    peak[k] = juce::jmax (peak[k], tone (ref, (juce::int64) ((t - ts) * 48.0) - 1024, 2048, hz));
                refOn[k] = onsetMs (ref, ts, t0 - 60.0, t0 + nt.lenMs, hz, 0.3 * peak[k]);
            }
            for (size_t k = 0; k < notes.size(); ++k)
                if (refOn[k] > 0.0)
                {
                    const double o = onsetMs (got, tr, refOn[k] - 20.0, refOn[k] + 400.0, noteHz (notes[k].pitch), 0.3 * peak[k]);
                    if (o > 0.0) diffs.push_back (o - refOn[k]);
                }
            const double lat = median (diffs);

            int sounded = 0, late = 0;
            juce::StringArray missing, lateList;
            double lastOff = 0.0;
            for (size_t k = 0; k < notes.size(); ++k)
            {
                const auto& nt = notes[k];
                const double hz = noteHz (nt.pitch);
                lastOff = juce::jmax (lastOff, nt.startMs + nt.lenMs);
                // the note's middle, a long window (fine pitch resolution), at the friend vs at the sender
                const double a = nt.startMs + 25.0, len = nt.lenMs - 35.0;
                const double eRef = tone (ref, (juce::int64) ((a) * 48.0), (int) (len * 48.0), hz);
                const double eGot = tone (got, (juce::int64) ((ts + a + lat - tr) * 48.0), (int) (len * 48.0), hz);
                const auto label = juce::MidiMessage::getMidiNoteName (nt.pitch, true, true, 4) + "@" + fmt (nt.startMs / 1000.0, 2) + "s";
                if (eRef <= 0.0 || eGot < eRef * 0.3)   // more than 10 dB down: not there
                {
                    missing.add (label + " (" + fmt (20.0 * std::log10 (juce::jmax (1e-9, eGot) / juce::jmax (1e-9, eRef)), 1) + " dB)");
                    continue;
                }
                ++sounded;
                const double o = refOn[k] > 0.0 ? onsetMs (got, tr, refOn[k] + lat - 50.0, refOn[k] + lat + 300.0, hz, 0.3 * peak[k]) : -1.0;
                if (o > 0.0 && o - refOn[k] - lat > 5.0)
                {
                    ++late;
                    lateList.add (label + " +" + fmt (o - refOn[k] - lat, 1) + " ms");
                }
            }

            // after the last note-off: release + 0.5 s, then the friend must be silent (< -60 dBFS RMS in every 50 ms)
            const double quietFrom = ts + lastOff + lat + noteRelease + 500.0;
            auto j0 = (juce::int64) ((quietFrom - tr) * 48.0);
            double worstDb = -200.0;
            int windows = 0;
            for (auto j = j0; j + 2400 <= (juce::int64) got.size(); j += 2400, ++windows)
            {
                double e = 0;
                for (int i = 0; i < 2400; ++i) e += (double) got[(size_t) (j + i)] * got[(size_t) (j + i)];
                worstDb = juce::jmax (worstDb, 10.0 * std::log10 (e / 2400.0 + 1e-30));
            }
            juce::StringArray ringing;
            if (worstDb > -60.0 && j0 >= 0)
                for (auto& nt : notes)
                    if (20.0 * std::log10 (tone (got, j0, 4800, noteHz (nt.pitch)) + 1e-12) > -70.0)
                        ringing.addIfNotAlreadyThere (juce::MidiMessage::getMidiNoteName (nt.pitch, true, true, 4));
            const bool stuck = worstDb > -60.0;
            const bool enoughTail = windows >= 4;
            const bool ok = enoughTail && ! stuck && sounded == (int) notes.size() && late == 0;
            line (caseName + ": " + (ok ? "PASS" : "FAIL") + " - notes sent " + juce::String ((int) notes.size()) + ", sounded "
                  + juce::String (sounded) + ", stuck " + juce::String (stuck ? juce::jmax (1, ringing.size()) : 0) + ", late " + juce::String (late)
                  + "; latency " + fmt (lat, 1) + " ms; after the last note-off + " + fmt (noteRelease + 500.0, 0) + " ms the friend is at "
                  + fmt (worstDb, 1) + " dBFS (need < -60" + (enoughTail ? juce::String() : juce::String (", TAIL TOO SHORT")) + ")"
                  + " | " + run.r (1)["peer.host"]);
            if (missing.size() > 0) line ("    missing: " + missing.joinIntoString (", "));
            if (lateList.size() > 0) line ("    late: " + lateList.joinIntoString (", "));
            if (ringing.size() > 0) line ("    still ringing: " + ringing.joinIntoString (" "));
            line ("    repro: " + run.repro);
        }

        void track (const juce::String& kind, double seconds)
        {
            // three players; the host plays a click track (FL Master instance, or the standalone file player);
            // everyone should hear each click at the same moment
            const auto caseName = "track_" + kind;
            Run run;
            run.name = caseName;
            run.seconds = seconds;
            auto hostArgs = common ("silence", "sync=1");
            hostArgs.add ("--track"); hostArgs.add (kind + ":clicks:1000");
            run.add ("host", hostArgs);
            run.add ("p2", common ("silence", "sync=1"));
            run.add ("p3", common ("silence", "sync=1"));
            exec (run);
            if (failed (run, caseName))
                return;
            const auto ref = parseNums (run.r (0)["clicksOut"]);
            std::vector<double> diffs;
            int matched = 0, expected = 0;
            for (int i = 1; i < 3; ++i)
            {
                const auto other = parseNums (run.r (i)["clicksOut"]);
                const double go = run.r (i)["goMs"].getDoubleValue();
                const double end = go + run.r (i)["measuredSeconds"].getDoubleValue() * 1000.0;
                for (auto t : ref)
                {
                    if (t < go + 500.0 || t > end - 500.0)
                        continue;   // that friend was not listening yet / any more
                    ++expected;
                    for (auto u : other)
                        if (std::abs (u - t) < 400.0) { diffs.push_back (u - t); ++matched; break; }
                }
            }
            double worst = 0;
            for (auto d : diffs) worst = juce::jmax (worst, std::abs (d));
            const bool ok = (double) ref.size() >= seconds - 4.0 && matched >= (int) (0.9 * expected) && worst <= 5.0;
            line (caseName + ": " + (ok ? "PASS" : "FAIL") + " - host heard " + juce::String ((int) ref.size()) + " clicks; friends matched "
                  + juce::String (matched) + "/" + juce::String (expected) + ", friend minus host " + fmt (median (diffs), 2)
                  + " ms median, worst |diff| " + fmt (worst, 2) + " ms (need <= 5); friends heard "
                  + juce::String ((int) parseNums (run.r (1)["clicksOut"]).size()) + " / " + juce::String ((int) parseNums (run.r (2)["clicksOut"]).size()));
            if (! ok)
                line ("    repro: " + run.repro);
        }

        void cpuReport()
        {
            if (cpu2 < 0 || cpu4 < 0)
            {
                line ("cpu: NOT MEASURED (connect2 / connect4 did not run)");
                return;
            }
            line ("cpu: INFO - audio thread per player: 2 players " + fmt (cpu2, 2) + " %, 4 players " + fmt (cpu4, 2) + " % -> "
                  + fmt ((cpu4 - cpu2) / 2.0, 2) + " % of one core per extra friend (finished sound, PCM)"
                  + (cpuRaw4 >= 0 ? ", raw-input mode 4 players " + fmt (cpuRaw4, 2) + " % -> " + fmt ((cpuRaw4 - cpu2) / 2.0, 2)
                                       + " % per extra friend (replicas)" : juce::String()));
        }
    };

    int runOrchestrator (const juce::File& outDir, juce::StringArray cases, bool quick)
    {
        outDir.createDirectory();
        Orchestrator o;
        o.outDir = outDir;
        o.exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        if (cases.isEmpty())
            cases = { "local", "connect2", "connect4", "latency", "quality", "jitter", "drift", "raw", "rawmidi", "track", "onebar", "cpu" };
        const double q = quick ? 0.4 : 1.0;
        o.line ("Clod Jam Test " + juce::Time::getCurrentTime().toString (true, true) + (quick ? " (quick)" : ""));
        for (auto& c : cases)
        {
            if (c == "local") o.local();
            if (c == "connect2") o.connect (2, 6.0);
            if (c == "connect4") o.connect (4, 6.0);
            if (c == "latency")
            {
                o.latency ("latency", {}, {}, 12.0 * q + 4.0, 500.0, 0.0);
                o.latency ("latency_net30", {}, "delay=30,jitter=2", 12.0 * q + 4.0, 500.0, 0.0);
                o.latency ("latency_opus", "comp=1", {}, 12.0 * q + 4.0, 500.0, 0.0);
            }
            if (c == "quality")
            {
                o.quality ("quality_pcm", "comp=0", 80.0);
                o.quality ("quality_opus", "comp=1,kbps=128", 25.0);
            }
            if (c == "jitter")
            {
                const double s = 20.0 * q + 4.0;
                o.dropouts ("jitter5", {}, "jitter=5", s, 0);
                o.dropouts ("loss1", "prot=0", "loss=1", s, -1);
                o.dropouts ("loss1_protected", "prot=1", "loss=1", s, -1);
                o.dropouts ("loss5", "prot=0", "loss=5", s, -1);
                o.dropouts ("loss5_protected", "prot=1", "loss=5", s, -1);
                o.dropouts ("reorder", {}, "delay=10,jitter=3,reorder=2", s, 0);
            }
            if (c == "drift") o.drift (quick ? 40.0 : 180.0);
            if (c == "raw") o.raw (12.0 * q + 6.0);
            if (c == "track")
            {
                o.track ("fl", 12.0 * q + 6.0);
                o.track ("file", 12.0 * q + 6.0);
            }
            if (c == "rawmidi")
            {
                o.rawMidi ("rawmidi_clean", "raw=1,prot=0", {});   // the harness itself: must pass
                o.rawMidi ("rawmidi_loss5", "raw=1,prot=0", "loss=5");
                o.rawMidi ("rawmidi_loss5_protected", "raw=1,prot=1", "loss=5");
            }
            if (c == "onebar") o.latency ("onebar", "onebar=1", {}, 16.0 * q + 8.0, 3000.0, 2000.0);
            if (c == "cpu") o.cpuReport();
        }
        outDir.getChildFile ("summary.txt").replaceWithText (o.summary.joinIntoString ("\n") + "\n");
        std::cout << "summary: " << outDir.getChildFile ("summary.txt").getFullPathName() << std::endl;
        return 0;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    auto value = [&] (const char* key, const juce::String& def) -> juce::String
    {
        const int i = args.indexOf (key);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : def;
    };

    if (args.contains ("--player"))
    {
        PlayerArgs a;
        a.isHost = value ("--player", "host") != "join";
        a.local = value ("--player", "host") == "local";
        a.out = juce::File (value ("--out", juce::File::getCurrentWorkingDirectory().getChildFile ("player.txt").getFullPathName()));
        a.port = value ("--port", juce::String (jam::defaultPort)).getIntValue();
        a.code = value ("--code", {});
        a.name = value ("--name", a.isHost ? "host" : "friend");
        a.expect = value ("--expect", "2").getIntValue();
        a.seconds = value ("--seconds", "10").getDoubleValue();
        a.signal = value ("--signal", "silence");
        a.ppm = value ("--ppm", "0").getDoubleValue();
        a.block = value ("--block", "128").getIntValue();
        a.opt = value ("--opt", {});
        a.track = value ("--track", {});
        a.patch = value ("--patch", {});
        a.view = value ("--view", {});
        a.knob = value ("--knob", {});
        a.notes = args.contains ("--notes");
        a.dump = args.contains ("--dump");
        return runPlayer (a);
    }

    if (args.isEmpty())
    {
        std::cout << "usage: ClodJamTest <outDir> [--quick] [case ...]\n";
        return 1;
    }
    const juce::File outDir (juce::File::getCurrentWorkingDirectory().getChildFile (args[0]));
    const bool quick = args.contains ("--quick");
    juce::StringArray cases;
    for (int i = 1; i < args.size(); ++i)
        if (! args[i].startsWith ("--"))
            cases.add (args[i]);
    return runOrchestrator (outDir, cases, quick);
}
