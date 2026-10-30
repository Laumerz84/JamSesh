#pragma once

#include "../PluginProcessor.h"
#include <complex>
#include <vector>

// Message-thread helpers shared by SculptView and SculptPanel: rendering a cycle from a table,
// analysing a cycle back into harmonics, the one-click table tools, input capture, and the
// "which table is being edited" UI state (kept in apvts.state child "SculptUI").
namespace sculpt_tools
{
    using sculpt::BaseTable;
    using sculpt::PartialSet;
    using sculpt::maxPartials;

    constexpr int cycleLen = 512;   // points per displayed / analysed cycle (power of two)
    constexpr float pi    = juce::MathConstants<float>::pi;
    constexpr float twoPi = juce::MathConstants<float>::twoPi;
    using Cycle = std::array<float, (size_t) cycleLen>;

    inline float wrap (float p) noexcept
    {
        p = std::fmod (p + pi, twoPi);
        if (p < 0.0f)
            p += twoPi;
        return p - pi;
    }

    struct Trig
    {
        Trig()
        {
            for (int i = 0; i < cycleLen; ++i)
            {
                s[(size_t) i] = std::sin (twoPi * (float) i / (float) cycleLen);
                c[(size_t) i] = std::cos (twoPi * (float) i / (float) cycleLen);
            }
        }
        Cycle s {}, c {};
    };

    inline const Trig& trig() { static const Trig t; return t; }

    inline float peakOf (const Cycle& x)
    {
        float p = 0.0f;
        for (auto v : x)
            p = juce::jmax (p, std::abs (v));
        return p;
    }

    // adds a * sin(2 pi k t + ph) for integer k over one cycle (table lookup, exact)
    inline void addHarmonic (Cycle& out, int k, float a, float ph, int step = 1)
    {
        const auto& tr = trig();
        const float cp = std::cos (ph) * a, sp = std::sin (ph) * a;
        for (int n = 0; n < cycleLen; n += step)
        {
            const size_t idx = (size_t) ((k * n) & (cycleLen - 1));
            out[(size_t) n] += tr.s[idx] * cp + tr.c[idx] * sp;
        }
    }

    inline float renderTable (const BaseTable& t, Cycle& out)
    {
        out.fill (0.0f);
        for (int i = 0; i < maxPartials; ++i)
            if (t.amp[(size_t) i] > 1.0e-7f)
                addHarmonic (out, i + 1, t.amp[(size_t) i], t.phase[(size_t) i]);
        return peakOf (out);
    }

    // complex coefficients z_k = a e^(i phase) (k = 1..64) for edits that add shapes
    using Coeffs = std::array<std::complex<float>, (size_t) maxPartials>;

    inline Coeffs toCoeffs (const BaseTable& t)
    {
        Coeffs z;
        for (size_t i = 0; i < (size_t) maxPartials; ++i)
            z[i] = std::polar (t.amp[i], t.phase[i]);
        return z;
    }

    inline float renderCoeffs (const Coeffs& z, Cycle& out)
    {
        out.fill (0.0f);
        for (int i = 0; i < maxPartials; ++i)
        {
            const float a = std::abs (z[(size_t) i]);
            if (a > 1.0e-7f)
                addHarmonic (out, i + 1, a, std::arg (z[(size_t) i]));
        }
        return peakOf (out);
    }

    // scale-free conversion: the loudest harmonic becomes 1 (the synth normalises the level anyway)
    inline BaseTable fromCoeffs (const Coeffs& z)
    {
        BaseTable t;
        float mx = 0.0f;
        for (auto& c : z)
            mx = juce::jmax (mx, std::abs (c));
        const float g = mx > 1.0e-9f ? 1.0f / mx : 0.0f;
        for (size_t i = 0; i < (size_t) maxPartials; ++i)
        {
            t.amp[i] = std::abs (z[i]) * g;
            t.phase[i] = t.amp[i] > 1.0e-7f ? std::arg (z[i]) : 0.0f;
        }
        return t;
    }

    // DFT of one cycle -> harmonics 1..64 (DC dropped). x = sum a sin(2 pi k t + ph).
    inline Coeffs analyse (const Cycle& x)
    {
        const auto& tr = trig();
        Coeffs z;
        for (int k = 1; k <= maxPartials; ++k)
        {
            float re = 0.0f, im = 0.0f;
            for (int n = 0; n < cycleLen; ++n)
            {
                const size_t idx = (size_t) ((k * n) & (cycleLen - 1));
                re += x[(size_t) n] * tr.c[idx];
                im -= x[(size_t) n] * tr.s[idx];
            }
            const float a = 2.0f * std::sqrt (re * re + im * im) / (float) cycleLen;
            z[(size_t) (k - 1)] = std::polar (a, std::atan2 (im, re) + pi * 0.5f);
        }
        return z;
    }

    inline float renderPartials (const PartialSet& p, Cycle& out, int step = 1)
    {
        out.fill (0.0f);
        for (int i = 0; i < juce::jmin (p.count, maxPartials); ++i)
        {
            const float a = p.amp[(size_t) i];
            if (a <= 1.0e-6f)
                continue;
            if (p.harmonic)
                addHarmonic (out, i + 1, a, p.phase[(size_t) i], step);
            else
                for (int n = 0; n < cycleLen; n += step)
                    out[(size_t) n] += a * std::sin (twoPi * p.ratio[(size_t) i] * (float) n / (float) cycleLen + p.phase[(size_t) i]);
        }
        return peakOf (out);
    }

    //==========================================================================
    // one-click table tools
    inline BaseTable normalize (BaseTable t)
    {
        float mx = 0.0f;
        for (auto a : t.amp) mx = juce::jmax (mx, a);
        if (mx > 0.0f)
            for (auto& a : t.amp) a /= mx;
        return t;
    }

    inline BaseTable clearToSine()
    {
        BaseTable t;
        t.amp[0] = 1.0f;
        return t;
    }

    inline BaseTable randomize()
    {
        auto& r = juce::Random::getSystemRandom();
        BaseTable t;
        const float p = 0.5f + 1.1f * r.nextFloat();
        for (int i = 0; i < maxPartials; ++i)
        {
            const bool drop = i > 0 && r.nextFloat() < 0.2f;
            t.amp[(size_t) i] = drop ? 0.0f : (0.3f + 0.7f * r.nextFloat()) / std::pow ((float) (i + 1), p);
            t.phase[(size_t) i] = wrap (twoPi * r.nextFloat());
        }
        t.amp[0] = juce::jmax (t.amp[0], 0.5f);
        return normalize (t);
    }

    inline BaseTable smooth (BaseTable t)
    {
        const float s = 1.0f / 200.0f;
        for (int i = 0; i < maxPartials; ++i)
        {
            const float k = (float) (i + 1);
            t.amp[(size_t) i] *= std::exp (-2.0f * pi * pi * s * s * k * k);
        }
        return t;
    }

    inline BaseTable oddOnly (BaseTable t)
    {
        for (int i = 1; i < maxPartials; i += 2)
            t.amp[(size_t) i] = 0.0f;
        return t;
    }

    inline BaseTable evenOnly (BaseTable t)
    {
        for (int i = 2; i < maxPartials; i += 2)
            t.amp[(size_t) i] = 0.0f;
        return t;
    }

    inline BaseTable invertPhases (BaseTable t)
    {
        for (auto& p : t.phase)
            p = wrap (-p);
        return t;
    }

    //==========================================================================
    // shared UI state: which table is being edited
    inline int activeTable (juce::AudioProcessorValueTreeState& s)
    {
        return (int) s.state.getChildWithName ("SculptUI").getProperty ("active", 0) == 1 ? 1 : 0;
    }

    inline void setActiveTable (juce::AudioProcessorValueTreeState& s, int which)
    {
        s.state.getOrCreateChildWithName ("SculptUI", nullptr).setProperty ("active", which == 1 ? 1 : 0, nullptr);
    }

    // factory wave menu with category headings (item id = wave index + 1)
    inline void fillWaveMenu (juce::ComboBox& box)
    {
        const auto names = SculptModel::waveNames();
        box.clear (juce::dontSendNotification);
        auto addGroup = [&] (const char* heading, std::initializer_list<int> ids)
        {
            box.addSectionHeading (heading);
            for (int i : ids)
                if (i < names.size())
                    box.addItem (names[i], i + 1);
        };
        addGroup ("Basic", { 0, 1, 2, 3, 4, 5 });
        addGroup ("Soft and character", { 17, 14, 15, 13, 16 });
        addGroup ("Instruments", { 6, 7, 8, 9, 12 });
        addGroup ("Vocal", { 10, 11, 18 });
        box.setTextWhenNothingSelected ("Load wave...");
    }

    //==========================================================================
    // Live single cycle of a scope buffer at frequency f0, shifted so its fundamental phase equals
    // refPhase and normalised to its own peak. Returns false when there is nothing to show.
    inline bool liveCycle (const ScopeBuffer& sb, double sr, float f0, float refPhase, Cycle& out,
                           std::vector<float>& l, std::vector<float>& r)
    {
        if (f0 < 15.0f || sr <= 0.0)
            return false;
        const double period = sr / (double) f0;
        if (period > 16000.0 || period < 2.0)
            return false;
        const int n = (int) std::ceil (period) + 2;
        l.resize ((size_t) n);
        r.resize ((size_t) n);
        sb.copyLatest (l.data(), r.data(), n);

        Cycle c;
        const double start = (double) (n - 1) - period;
        double e = 0.0;
        for (int j = 0; j < cycleLen; ++j)
        {
            const double pos = start + (double) j * period / (double) cycleLen;
            const int i0 = juce::jlimit (0, n - 2, (int) pos);
            const float fr = (float) (pos - (double) i0);
            const float a = 0.5f * (l[(size_t) i0] + r[(size_t) i0]);
            const float b = 0.5f * (l[(size_t) i0 + 1] + r[(size_t) i0 + 1]);
            c[(size_t) j] = a + (b - a) * fr;
            e += (double) c[(size_t) j] * c[(size_t) j];
        }
        if (e / cycleLen < 1.0e-8)
            return false;

        const auto& tr = trig();
        float re = 0.0f, im = 0.0f;
        for (int j = 0; j < cycleLen; ++j)
        {
            re += c[(size_t) j] * tr.c[(size_t) j];
            im -= c[(size_t) j] * tr.s[(size_t) j];
        }
        const float psi = std::atan2 (im, re) + pi * 0.5f;
        const float shift = wrap (refPhase - psi) / twoPi * (float) cycleLen;
        for (int j = 0; j < cycleLen; ++j)
        {
            float pos = (float) j + shift;
            pos -= std::floor (pos / (float) cycleLen) * (float) cycleLen;
            const int i0 = (int) pos % cycleLen;
            const int i1 = (i0 + 1) % cycleLen;
            const float fr = pos - std::floor (pos);
            out[(size_t) j] = c[(size_t) i0] + (c[(size_t) i1] - c[(size_t) i0]) * fr;
        }
        const float pk = peakOf (out);
        if (pk <= 1.0e-6f)
            return false;
        for (auto& v : out)
            v /= pk;
        return true;
    }

    // Capture the input's current cycle into a table. Message thread.
    inline bool captureInput (ClodScopeProcessor& proc, BaseTable& result, juce::String& message)
    {
        const auto tr = proc.synth.tracker();
        const double sr = proc.currentSampleRate.load();
        double f0 = (double) tr.f0Hz;
        if (f0 < 20.0 || f0 > 5000.0 || sr <= 0.0)
        {
            message = "No stable pitch in the input - play and hold a note, then Capture.";
            return false;
        }
        const double period = sr / f0;
        const int periods = juce::jlimit (2, 8, (int) (16384.0 / period));
        const int len = (int) std::round (periods * period);
        if (len < 64)
        {
            message = "The input pitch is too high to capture.";
            return false;
        }

        std::vector<float> l ((size_t) len), r ((size_t) len), x ((size_t) len), w ((size_t) len);
        proc.inputScope.copyLatest (l.data(), r.data(), len);
        double e = 0.0;
        for (int i = 0; i < len; ++i)
        {
            x[(size_t) i] = 0.5f * (l[(size_t) i] + r[(size_t) i]);
            w[(size_t) i] = 0.5f - 0.5f * std::cos (twoPi * (float) i / (float) (len - 1));   // Hann
            e += (double) x[(size_t) i] * x[(size_t) i];
        }
        if (e / len < 1.0e-8)
        {
            message = "The input is silent - play a note, then Capture.";
            return false;
        }

        auto dft = [&] (double f) -> std::complex<double>
        {
            const std::complex<double> rot = std::polar (1.0, -juce::MathConstants<double>::twoPi * f / sr);
            std::complex<double> ph (1.0, 0.0), acc (0.0, 0.0);
            for (int i = 0; i < len; ++i)
            {
                acc += ph * (double) (x[(size_t) i] * w[(size_t) i]);
                ph *= rot;
                if ((i & 1023) == 1023)
                    ph /= std::abs (ph);   // keep the phasor on the unit circle
            }
            return acc;
        };

        // refine f0 (+/- 1.5 %) on the energy of the first 8 harmonics
        double bestF = f0, bestE = -1.0;
        for (int c = -15; c <= 15; ++c)
        {
            const double f = f0 * (1.0 + 0.001 * c);
            double en = 0.0;
            for (int k = 1; k <= 8 && k * f < sr * 0.5; ++k)
                en += std::norm (dft (k * f));
            if (en > bestE)
            {
                bestE = en;
                bestF = f;
            }
        }
        f0 = bestF;

        Coeffs z;
        int found = 0;
        for (int k = 1; k <= maxPartials; ++k)
        {
            if (k * f0 >= sr * 0.48)
            {
                z[(size_t) (k - 1)] = {};
                continue;
            }
            const auto X = dft (k * f0);
            z[(size_t) (k - 1)] = std::polar ((float) std::abs (X), (float) std::arg (X) + pi * 0.5f);
        }
        // shift time so the fundamental starts at phase 0: phase_k -= k * phase_1
        const float p1 = std::arg (z[0]);
        for (int k = 1; k <= maxPartials; ++k)
            z[(size_t) (k - 1)] *= std::polar (1.0f, -(float) k * p1);

        result = fromCoeffs (z);
        for (auto a : result.amp)
            if (a > 0.001f)
                ++found;
        // clean up the noise floor (below -60 dB of the loudest harmonic)
        for (size_t i = 0; i < (size_t) maxPartials; ++i)
            if (result.amp[i] < 0.001f)
                result.amp[i] = 0.0f;

        const int note = (int) std::round (69.0 + 12.0 * std::log2 (f0 / 440.0));
        message = "Captured " + juce::String (found) + " harmonics at " + juce::String (f0, 1) + " Hz ("
                + juce::MidiMessage::getMidiNoteName (note, true, true, 4) + ")"
                + (tr.voiced ? juce::String() : juce::String (" - pitch was not stable, result may be noisy"));
        return true;
    }
}
