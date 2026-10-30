#include "JamClock.h"
#include <algorithm>
#include <cmath>

// Owner: net soldier.
namespace jam
{
    void Clock::reset() noexcept
    {
        recentCount = recentPos = goodCount = goodPos = sinceGood = 0;
        bestOfBatch = {};
        smoothedOffset = rate = 0.0;
        haveOffset = false;
        numSamples.store (0);
        bestRtt.store (0.0f);
        publish (0.0, 0.0, 0.0);
    }

    void Clock::publish (double offset, double rateValue, double ref) noexcept
    {
        version.fetch_add (1, std::memory_order_acq_rel);   // odd: writing
        pubOffset.store (offset, std::memory_order_relaxed);
        pubRate.store (rateValue, std::memory_order_relaxed);
        pubRef.store (ref, std::memory_order_relaxed);
        version.fetch_add (1, std::memory_order_release);   // even: done
    }

    double Clock::toSession (double local) const noexcept
    {
        for (int tries = 0; tries < 8; ++tries)
        {
            const auto v0 = version.load (std::memory_order_acquire);
            if ((v0 & 1) != 0)
                continue;
            const double o = pubOffset.load (std::memory_order_relaxed);
            const double r = pubRate.load (std::memory_order_relaxed);
            const double ref = pubRef.load (std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_acquire);
            if (version.load (std::memory_order_relaxed) == v0)
                return local + o + r * (local - ref);
        }
        return local + pubOffset.load (std::memory_order_relaxed);
    }

    float Clock::offsetMs() const noexcept
    {
        const double now = localSeconds();
        return (float) ((toSession (now) - now) * 1000.0);
    }

    float Clock::driftPpm() const noexcept
    {
        return (float) (pubRate.load (std::memory_order_relaxed) * 1.0e6);
    }

    void Clock::addExchange (double t0, double t1, double t2, double t3) noexcept
    {
        const double rtt = (t3 - t0) - (t2 - t1);
        if (! (rtt >= 0.0) || rtt > 5.0)
            return;
        const double offset = ((t1 - t0) + (t2 - t3)) * 0.5;
        const double local = (t0 + t3) * 0.5;

        recent[recentPos] = { local, offset, rtt };
        recentPos = (recentPos + 1) % window;
        recentCount = std::min (recentCount + 1, window);

        // the quickest quarter of the recent exchanges: their offsets are the least skewed by queueing
        Sample sorted[window];
        std::copy (recent, recent + recentCount, sorted);
        std::sort (sorted, sorted + recentCount, [] (const Sample& a, const Sample& b) { return a.rtt < b.rtt; });
        const int take = std::max (1, recentCount / 4);
        double sum = 0.0;
        for (int i = 0; i < take; ++i)
            sum += sorted[i].offset;
        const double target = sum / take;
        bestRtt.store ((float) sorted[0].rtt, std::memory_order_relaxed);

        // rate: the best sample of every 8 exchanges, least-squares slope of offset over local time
        if (rtt < bestOfBatch.rtt)
            bestOfBatch = { local, offset, rtt };
        if (++sinceGood >= 8)
        {
            good[goodPos] = bestOfBatch;
            goodPos = (goodPos + 1) % longWindow;
            goodCount = std::min (goodCount + 1, longWindow);
            bestOfBatch = {};
            sinceGood = 0;

            if (goodCount >= 6)
            {
                double mx = 0, my = 0;
                for (int i = 0; i < goodCount; ++i) { mx += good[i].local; my += good[i].offset; }
                mx /= goodCount; my /= goodCount;
                double sxy = 0, sxx = 0;
                for (int i = 0; i < goodCount; ++i)
                {
                    sxy += (good[i].local - mx) * (good[i].offset - my);
                    sxx += (good[i].local - mx) * (good[i].local - mx);
                }
                if (sxx > 1.0)   // at least a few seconds of spread
                    rate = juce::jlimit (-2.0e-4, 2.0e-4, sxy / sxx);   // real crystals: well under 200 ppm
            }
        }

        // smooth: settle fast at first, then glide (the session clock must not jump under the music)
        const double now = local;
        const double predicted = haveOffset ? smoothedOffset : target;
        if (! haveOffset || numSamples.load (std::memory_order_relaxed) < 4 || std::abs (target - predicted) > 0.05)
            smoothedOffset = target;   // first samples, or a big step (host clock jumped): take it
        else
            smoothedOffset += (target - smoothedOffset) * 0.1;
        haveOffset = true;
        numSamples.fetch_add (1, std::memory_order_relaxed);

        // the smoothed offset is valid "now"; the rate extrapolates from here
        publish (smoothedOffset, rate, now);
    }
}
