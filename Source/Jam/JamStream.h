#pragma once

#include "JamTypes.h"
#include "JamCodec.h"
#include <atomic>
#include <vector>

// One friend's incoming sound: jitter buffer, loss concealment, clock-drift resampling, level and
// latency statistics, and the ring their view tile reads. Owner: stream soldier.
//
// The receive side keeps a timeline ring at 48 kHz indexed by the sender's sample counter: packets
// are decoded into it in order (reordered ones wait in a small store, lost ones are concealed when
// the reader needs them), and a fractional reader plays it out at this sound card's rate, its speed
// trimmed by a slow PI loop that holds the buffer level (clock drift) or, in one-bar mode, the
// distance to "captured one bar ago".
namespace jam
{
    //==============================================================================
    // 6-point Lagrange resampler fed in pushes (send side: device rate -> 48 kHz). Allocation-free after
    // prepare(). One instance handles any ratio; step = input samples per output sample.
    class PushResampler
    {
    public:
        void prepare (int capacity);
        void reset() noexcept;
        void write (const float* left, const float* right, int numSamples) noexcept;
        int read (float* left, float* right, int maxOut, double step) noexcept;
        double pendingInput() const noexcept { return (double) count - pos; }

    private:
        std::vector<float> bufL, bufR;
        int count = 2, capacity = 0;
        double pos = 2.0;
    };

    // 6-point, 5th-order Lagrange interpolation between y[2] and y[3] (t in [0, 1)): clean to ~-100 dB
    // at 1 kHz, where a 4-point Hermite only reaches ~-70 dB
    inline float lagrange6 (const float* y, float t) noexcept
    {
        const float tp2 = t + 2.0f, tp1 = t + 1.0f, tm1 = t - 1.0f, tm2 = t - 2.0f, tm3 = t - 3.0f;
        const float a = tp2 * tp1, b = tm2 * tm3, c = t * tm1;
        return y[0] * (-tp1 * c * b / 120.0f)
             + y[1] * (tp2 * c * b / 24.0f)
             + y[2] * (-a * tm1 * b / 12.0f)
             + y[3] * (a * t * b / 12.0f)
             + y[4] * (-a * c * tm3 / 24.0f)
             + y[5] * (a * c * tm2 / 120.0f);
    }

    //==============================================================================
    // Newest-samples ring a view tile reads (one writer: the audio thread; readers: message thread)
    class FeedRing
    {
    public:
        void prepare (int sizePowerOfTwo);
        void push (const float* left, const float* right, int numSamples) noexcept;
        int copyLatest (float* left, float* right, int numSamples) const noexcept;
        bool hasData() const noexcept { return written.load (std::memory_order_acquire) > 0; }

    private:
        std::vector<float> bufL, bufR;
        int mask = 0;
        std::atomic<juce::int64> written { 0 };
    };

    //==============================================================================
    // A clock's rate against the wall clock, from a "lag" that grows or shrinks with it (packet transit for
    // a sender, callback lag for this sound card): the minimum of each 1 s window (the fastest packet /
    // the earliest callback, so jitter, late and reordered packets drop out), then a least-squares line
    // through the last 30 windows. ppm > 0: that clock runs fast.
    class RateTracker
    {
    public:
        void reset() noexcept { count = head = 0; windowStart = -1.0; }
        void add (double t, double lag) noexcept;
        bool ready() const noexcept { return count >= 6; }
        double ppm() const noexcept { return ppmNow; }

    private:
        static constexpr int maxWindows = 30;
        double ts[maxWindows] {}, vs[maxWindows] {};
        int count = 0, head = 0;
        double windowStart = -1.0, windowMin = 0.0, windowMinT = 0.0, lastLag = 0.0, ppmNow = 0.0;
    };

    //==============================================================================
    struct RenderContext
    {
        double deviceRate = 48000.0;
        int numSamples = 0;
        double sessionNow = 0.0;      // Network::sessionTime() for this block
        double wallNow = 0.0;         // getMillisecondCounterHiRes() * 0.001 for this block
        double barSeconds = 0.0;      // one-bar mode: play everything this long after capture; 0 = off
        bool autoJitter = true;
        float jitterMs = 10.0f;
        bool driftCorrection = true;
    };

    class ReceiveStream
    {
    public:
        static constexpr int ringBits = 18;                  // 5.4 s at 48 kHz (one bar at >= 45 BPM)
        static constexpr int ringSize = 1 << ringBits;
        static constexpr int fifoSlots = 128;
        static constexpr int storeSlots = 32;
        static constexpr int maxMidiPending = 256;

        ReceiveStream() { std::fill (std::begin (seenStarts), std::end (seenStarts), (juce::int64) -1); }
        ~ReceiveStream() = default;

        void allocate();                                      // message thread, once

        // Network thread: copies the packet into the hand-over queue, updates the arrival statistics
        void push (const PacketView&, const juce::uint8* data, int size, double arrivalSeconds) noexcept;

        // Audio thread
        void reset() noexcept;
        void requestReset() noexcept { resetRequested = true; }   // any thread: reset on the next render
        // Audio thread: true once after the stream restarted or reset (MIDI may have been dropped: the
        // receiver should release whatever their replica still holds)
        bool takeNotesOff() noexcept { const bool r = notesOffPending; notesOffPending = false; return r; }
        int recoveredMidiEvents() const noexcept { return recoveredMidi; }
        // Writes numSamples of their sound (stereo) at the device rate. midiOut (raw mode) receives their
        // MIDI for this block. Returns false when there was nothing to play (zeros written).
        bool render (float* left, float* right, juce::MidiBuffer* midiOut, const RenderContext&) noexcept;

        // Statistics (any thread)
        std::atomic<float> jitterMs { 0.0f }, bufferMs { 0.0f }, driftPpm { 0.0f }, lossPercent { 0.0f };
        std::atomic<float> packetMs { 0.0f }, codecMs { 0.0f };
        std::atomic<int> kind { -1 };                        // StreamKind of the last packet, -1 = none yet
        std::atomic<bool> compression { false }, active { false };
        std::atomic<juce::int64> lastArrivalMs { 0 };

    private:
        struct Slot
        {
            int size = 0;
            double arrival = 0.0;
            juce::uint8 bytes[wire::maxPacketBytes];
        };
        struct Stored
        {
            bool used = false;
            juce::int64 counter = 0;
            int size = 0;
            juce::uint8 bytes[wire::maxPacketBytes];
        };
        struct PendingMidi
        {
            juce::int64 pos = 0;
            juce::uint8 size = 0, data[3] {};
        };
        enum class State { empty, prebuffer, playing };

        void handle (const PacketView&, const juce::uint8* data, int size, double arrival) noexcept;
        void updateArrivalStats (const PacketView&, double arrival) noexcept;
        void restartAt (juce::int64 counter) noexcept;
        void decodeFrame (const PacketHeader&, const juce::uint8* bytes, int size, juce::int64 at) noexcept;
        void decodePacket (const PacketView&) noexcept;
        void takeMidi (const PacketView&) noexcept;
        bool midiSeen (juce::int64 start) const noexcept;
        void queueMidi (juce::int64 pos, const juce::uint8* data, int size) noexcept;
        void flushStore() noexcept;
        bool storeHasAny (juce::int64& firstCounter) const noexcept;
        void concealUntil (juce::int64 end) noexcept;
        void writeRing (juce::int64 at, const float* l, const float* r, int n) noexcept;
        void readRing (double pos, double step, float* l, float* r, int n) const noexcept;
        double alignedJump (double from, double wanted, double search) const noexcept;

        // producer side (network thread)
        juce::SpinLock pushLock;
        juce::AbstractFifo fifo { fifoSlots };
        std::vector<Slot> slots;
        double prevTransit = 0.0, minTransit = 0.0, prevArrival = 0.0, jitterJ = 0.0, peakSpread = 0.0, peakHoldUntil = 0.0;
        bool haveTransit = false;
        std::atomic<double> jitterMarginSec { 0.0 };

        // consumer side (audio thread)
        std::vector<float> ringL, ringR;
        std::vector<Stored> store;
        PendingMidi midiPending[maxMidiPending];
        int midiCount = 0;
        static constexpr int numSeen = 64;
        juce::int64 seenStarts[numSeen];
        int seenHead = 0, recoveredMidi = 0;
        bool notesOffPending = false;
        OpusDec decoder;
        State state = State::empty;
        juce::int64 decodeHead = 0, writeHeadMax = 0;
        double readPos = 0.0;
        int lastFrames = 128, lastChannels = 2;
        Codec lastCodec = Codec::pcm16;
        bool decoderFresh = true, protectedStream = false;
        int sinceRedundant = 1000;
        int fadeIn = 0, concealRun = 0, starveSamples = 0;
        float lastOutL = 0.0f, lastOutR = 0.0f;
        double levelSmoothed = 0.0, lateSmoothed = 0.0, rateCorrection = 0.0;
        RateTracker senderRate;                                      // producer side
        RateTracker cardRate;                                        // consumer side
        double consumedSeconds = 0.0;
        static constexpr int numRenderWindows = 5;                   // render-side headroom (consumer side)
        double renderSpreads[numRenderWindows] {};
        int renderSpreadCount = 0, renderSpreadHead = 0;
        double renderWinStart = -1.0, renderWinMin = 0.0, renderWinMax = 0.0, renderHeadroom = 0.0;
        std::atomic<double> senderPpm { 0.0 };
        std::atomic<bool> senderRateReady { false };
        bool haveLevel = false;
        double prevReadStart = 0.0, prevWall = 0.0, prevSpeed = 0.0;   // prevSpeed: 48 kHz samples read per second
        double captureOffset = 0.0;                                  // sessionTime - counter / 48000
        bool haveOffset = false;
        double lossEma = 0.0;
        float xfadeL[64] {}, xfadeR[64] {};
        float tmpL[wire::maxFrames] {}, tmpR[wire::maxFrames] {};
        double deviceRate = 48000.0;
        std::atomic<bool> resetRequested { false };
    };
}
