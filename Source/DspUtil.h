#pragma once

#include <cmath>

namespace DspUtil
{
    // Fundamental estimate from rising zero crossings, with hysteresis so noise
    // doesn't add false crossings. Returns 0 when there's no clear periodic signal.
    inline float estimateFrequency (const float* x, int n, double sampleRate)
    {
        if (n < 4)
            return 0.0f;

        double mean = 0.0;
        for (int i = 0; i < n; ++i)
            mean += x[i];
        mean /= n;

        double sq = 0.0;
        for (int i = 0; i < n; ++i)
            sq += (x[i] - mean) * (x[i] - mean);
        const float hyst = 0.3f * (float) std::sqrt (sq / n);
        if (hyst < 1.0e-5f)
            return 0.0f;

        double first = -1.0, last = -1.0;
        int crossings = 0;
        bool armed = false;
        for (int i = 1; i < n; ++i)
        {
            const float a = x[i - 1] - (float) mean;
            const float b = x[i] - (float) mean;
            if (b < -hyst)
                armed = true;
            if (armed && a < 0.0f && b >= 0.0f)
            {
                const double pos = (i - 1) + a / (a - b);
                if (first < 0.0) first = pos;
                last = pos;
                ++crossings;
                armed = false;
            }
        }
        return (crossings >= 3 && last > first) ? (float) ((crossings - 1) * sampleRate / (last - first)) : 0.0f;
    }
}
