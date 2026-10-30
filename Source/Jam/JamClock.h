#pragma once

#include "JamTypes.h"
#include <atomic>

// The shared session clock: maps this computer's clock to the host's (offset and rate, refined by
// ping exchanges), so bar lines (one-bar mode) and backing-track timing land at the same moment on
// every computer. Owner: net soldier.
//
// NTP style: every ping/pong with the host gives an offset sample and its round trip. Only the
// quickest exchanges are trusted (queueing delay only ever adds time, and asymmetrically), the offset
// is smoothed so the session clock never jumps once it has settled, and the rate (the two crystals'
// drift) is fitted over the last minute of good samples.
namespace jam
{
    // seconds on this computer's clock
    inline double localSeconds() noexcept { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

    class Clock
    {
    public:
        // network thread: one exchange. t0 = our send, t1 = host receive, t2 = host send, t3 = our receive
        void addExchange (double t0, double t1, double t2, double t3) noexcept;
        void reset() noexcept;                      // back to "this computer is the session clock"

        // any thread, lock-free (audio thread included)
        double toSession (double local) const noexcept;
        double sessionNow() const noexcept { return toSession (localSeconds()); }

        bool synced() const noexcept { return numSamples.load (std::memory_order_relaxed) >= 3; }
        float offsetMs() const noexcept;            // session minus local, now
        float driftPpm() const noexcept;            // the host's clock rate relative to ours
        float bestRttMs() const noexcept { return bestRtt.load (std::memory_order_relaxed) * 1000.0f; }

    private:
        struct Sample { double local = 0, offset = 0, rtt = 1.0e9; };
        static constexpr int window = 32, longWindow = 64;
        Sample recent[window];                      // network thread only
        Sample good[longWindow];                    // best sample of every few exchanges, for the rate fit
        int recentCount = 0, recentPos = 0, goodCount = 0, goodPos = 0, sinceGood = 0;
        Sample bestOfBatch;

        // published model: session = local + offset + rate * (local - ref), behind a seqlock
        std::atomic<juce::uint32> version { 0 };
        std::atomic<double> pubOffset { 0.0 }, pubRate { 0.0 }, pubRef { 0.0 };
        std::atomic<int> numSamples { 0 };
        std::atomic<float> bestRtt { 0.0f };
        double smoothedOffset = 0.0, rate = 0.0;    // network thread
        bool haveOffset = false;

        void publish (double offset, double rateValue, double ref) noexcept;
    };
}
