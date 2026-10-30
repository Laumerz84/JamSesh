#pragma once

#include <juce_graphics/juce_graphics.h>
#include <cstring>
#include <limits>
#include <vector>

// Fast drawing straight into RGB software images, for pictures redrawn every frame (the 2D scope's
// traces and spectrum). The editor paints in software (UI/StreamSafe.h), where JUCE's path stroking
// costs about 1 ms per curve; the scope draws up to eight curves 60 times a second.
namespace fasttrace
{
    namespace detail
    {
        inline void blend (juce::Image::BitmapData& px, int x, int y, juce::PixelARGB src, juce::uint32 amount256)
        {
            reinterpret_cast<juce::PixelRGB*> (px.getPixelPointer (x, y))->blend (src, amount256);
        }
    }

    // Anti-aliased lines of any width and direction. Each segment is walked along its longer axis and
    // marks how much of each pixel across it the line covers, in a scratch buffer where overlapping
    // segments keep the larger value (as one stroked path would). Then every touched pixel is blended
    // once.
    class Stroker
    {
    public:
        void stroke (juce::Image::BitmapData& px, const std::vector<juce::Point<float>>& pts, float halfWidth, juce::Colour colour)
        {
            jassert (px.pixelFormat == juce::Image::RGB);
            const int w = px.width, h = px.height;
            if (w <= 0 || h <= 0 || pts.size() < 2)
                return;
            if (cover.size() < (size_t) w * (size_t) h)
                cover.resize ((size_t) w * (size_t) h, 0);   // stays all zero between strokes
            top.assign ((size_t) w, h);
            bottom.assign ((size_t) w, -1);

            auto mark = [&] (int x, int y, float amount)
            {
                if (x < 0 || y < 0 || x >= w || y >= h || amount <= 0.0f)
                    return;
                auto& c = cover[(size_t) y * (size_t) w + (size_t) x];
                c = juce::jmax (c, (juce::uint8) juce::jmin (255.0f, amount * 255.0f + 0.5f));
                top[(size_t) x] = juce::jmin (top[(size_t) x], y);
                bottom[(size_t) x] = juce::jmax (bottom[(size_t) x], y);
            };

            for (size_t n = 0; n + 1 < pts.size(); ++n)
            {
                auto p = pts[n], q = pts[n + 1];
                const bool steep = std::abs (q.y - p.y) > std::abs (q.x - p.x);
                if (steep)   // walk rows instead of columns: x below is the longer axis
                {
                    std::swap (p.x, p.y);
                    std::swap (q.x, q.y);
                }
                if (p.x > q.x)
                    std::swap (p, q);

                const float slope = q.x > p.x ? (q.y - p.y) / (q.x - p.x) : 0.0f;
                const float half = halfWidth * std::sqrt (1.0f + slope * slope);   // the line's extent across this axis
                const int last = juce::jmin ((steep ? h : w) - 1, (int) std::floor (q.x));
                for (int i = juce::jmax (0, (int) std::floor (p.x)); i <= last; ++i)
                {
                    const float centre = p.y + slope * (juce::jlimit (p.x, q.x, (float) i + 0.5f) - p.x);
                    const float a = centre - half, b = centre + half;
                    for (int j = (int) std::floor (a); (float) j < b; ++j)
                    {
                        const float amount = juce::jmin ((float) (j + 1), b) - juce::jmax ((float) j, a);
                        if (steep) mark (j, i, amount);
                        else       mark (i, j, amount);
                    }
                }
            }

            const auto src = colour.getPixelARGB();
            for (int x = 0; x < w; ++x)
                for (int y = top[(size_t) x]; y <= bottom[(size_t) x]; ++y)
                {
                    auto& c = cover[(size_t) y * (size_t) w + (size_t) x];
                    if (c != 0)
                    {
                        detail::blend (px, x, y, src, ((juce::uint32) c * 256u + 127u) / 255u);
                        c = 0;
                    }
                }
        }

    private:
        std::vector<juce::uint8> cover;
        std::vector<int> top, bottom;
    };

    // Vertical extent of a curve (points in ascending x) in each pixel column [c, c + 1); columns it
    // misses have lo > hi
    struct Columns
    {
        std::vector<float> lo, hi;

        void build (const std::vector<juce::Point<float>>& pts, int width)
        {
            lo.assign ((size_t) juce::jmax (0, width), std::numeric_limits<float>::max());
            hi.assign ((size_t) juce::jmax (0, width), std::numeric_limits<float>::lowest());
            auto add = [this, width] (int c, float y)
            {
                if (c >= 0 && c < width)
                {
                    lo[(size_t) c] = juce::jmin (lo[(size_t) c], y);
                    hi[(size_t) c] = juce::jmax (hi[(size_t) c], y);
                }
            };

            if (pts.size() == 1)
                add ((int) std::floor (pts[0].x), pts[0].y);

            for (size_t i = 0; i + 1 < pts.size(); ++i)
            {
                const auto p = pts[i], q = pts[i + 1];
                const int c0 = (int) std::floor (p.x), c1 = (int) std::floor (q.x);
                if (c1 < 0 || c0 >= width)
                    continue;
                add (c0, p.y);
                add (c1, q.y);
                // where the segment crosses a column edge, that y belongs to the columns on both sides
                for (int c = juce::jmax (c0 + 1, 0); c <= juce::jmin (c1, width); ++c)
                {
                    const float y = p.y + (q.y - p.y) * ((float) c - p.x) / (q.x - p.x);
                    add (c - 1, y);
                    add (c, y);
                }
            }
        }
    };

    // Fills each column from the curve down to the image's bottom edge, with a vertical gradient of
    // opacity from alphaTop (top row) to alphaBottom (bottom row)
    inline void fillBelow (juce::Image::BitmapData& px, const Columns& cols, juce::Colour colour,
                           float alphaTop, float alphaBottom)
    {
        jassert (px.pixelFormat == juce::Image::RGB);
        const auto src = colour.withAlpha (1.0f).getPixelARGB();
        const int w = juce::jmin (px.width, (int) cols.lo.size()), h = px.height;
        std::vector<float> rowAlpha ((size_t) juce::jmax (0, h));
        for (int y = 0; y < h; ++y)
            rowAlpha[(size_t) y] = 256.0f * juce::jmap (((float) y + 0.5f) / (float) h, alphaTop, alphaBottom);

        for (int c = 0; c < w; ++c)
        {
            if (cols.lo[(size_t) c] > cols.hi[(size_t) c])
                continue;
            const float top = juce::jlimit (0.0f, (float) h, 0.5f * (cols.lo[(size_t) c] + cols.hi[(size_t) c]));
            int y = (int) top;
            if ((float) y < top && y < h)   // the edge pixel is only partly below the curve
            {
                detail::blend (px, c, y, src, (juce::uint32) (rowAlpha[(size_t) y] * ((float) (y + 1) - top)));
                ++y;
            }
            for (; y < h; ++y)
                detail::blend (px, c, y, src, (juce::uint32) rowAlpha[(size_t) y]);
        }
    }

    // Moves every byte of `image` toward the same byte of `target`, keeping `keep` (0..1) of the
    // difference: an afterglow that fades back into the target picture. keep = 0 copies the target.
    inline void fadeToward (juce::Image::BitmapData& image, const juce::Image::BitmapData& target, float keep)
    {
        jassert (image.width == target.width && image.height == target.height && image.pixelFormat == target.pixelFormat);
        const int k = juce::jlimit (0, 256, juce::roundToInt (keep * 256.0f));
        const int rowBytes = image.width * image.pixelStride;
        for (int y = 0; y < image.height; ++y)
        {
            auto* d = image.getLinePointer (y);
            const auto* s = target.getLinePointer (y);
            if (k == 0)
            {
                std::memcpy (d, s, (size_t) rowBytes);
                continue;
            }
            for (int i = 0; i < rowBytes; ++i)
                d[i] = (juce::uint8) (s[i] + ((int) d[i] - (int) s[i]) * k / 256);   // rounds toward the target
        }
    }
}
