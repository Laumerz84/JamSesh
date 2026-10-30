#pragma once

#include <atomic>
#include <vector>

// Single-writer ring buffer: the audio thread pushes stereo samples, the
// editor copies out the most recent N samples for drawing. Torn reads only
// ever affect a few displayed samples, which is fine for a scope.
class ScopeBuffer
{
public:
    static constexpr int size = 1 << 18;
    static constexpr int mask = size - 1;

    void push (const float* l, const float* r, int n) noexcept
    {
        int w = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            left[(size_t) ((w + i) & mask)]  = l[i];
            right[(size_t) ((w + i) & mask)] = r[i];
        }
        writePos.store ((w + n) & mask, std::memory_order_release);
    }

    void copyLatest (float* l, float* r, int n) const noexcept
    {
        const int start = (writePos.load (std::memory_order_acquire) - n) & mask;
        for (int i = 0; i < n; ++i)
        {
            l[i] = left[(size_t) ((start + i) & mask)];
            r[i] = right[(size_t) ((start + i) & mask)];
        }
    }

private:
    std::vector<float> left  = std::vector<float> ((size_t) size, 0.0f);
    std::vector<float> right = std::vector<float> ((size_t) size, 0.0f);
    std::atomic<int> writePos { 0 };
};
