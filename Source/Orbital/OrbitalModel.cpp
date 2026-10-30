#include "OrbitalModel.h"
#include "OrbitalMath.h"
#include "Atoms.h"
#include "../DspUtil.h"
#include <juce_dsp/juce_dsp.h>
#include <map>
#include <numeric>
#include <tuple>

namespace orbital
{
namespace
{
    constexpr int maxEvalTerms = 32;
    constexpr double twoPi = 2.0 * orbmath::pi;

    juce::NormalisableRange<float> logRange (float lo, float hi)
    {
        return { lo, hi,
                 [] (float s, float e, float t) { return s * std::pow (e / s, t); },
                 [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); } };
    }

    juce::String hzText (double hz)
    {
        return hz >= 1000.0 ? juce::String (hz / 1000.0, 2) + " kHz" : juce::String (hz, 1) + " Hz";
    }

    juce::String centsText (double c)
    {
        return (c >= 0 ? "+" : "") + juce::String ((int) std::round (c)) + " c";
    }

    const char* seriesName (int lower)
    {
        static const char* names[] = { "Lyman", "Balmer", "Paschen", "Brackett", "Pfund", "Humphreys", "n=7", "n=8" };
        return names[juce::jlimit (0, 7, lower - 1)];
    }

    const char* greek (int step)
    {
        static const char* names[] = { "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta" };
        return names[juce::jlimit (0, 6, step - 1)];
    }

    // 4 (b^2 - a^2) / (3 a^2 b^2) as a reduced fraction: the hydrogen line frequency in units of f0
    juce::String lineFraction (int a, int b)
    {
        const long num = 4L * (b * b - a * a), den = 3L * a * a * b * b;
        const long g = std::gcd (num, den);
        return juce::String (num / g) + "/" + juce::String (den / g);
    }

    struct Peak
    {
        double hz = 0, amp = 0, phaseDiff = 0, width = 0;
    };

    // One desired (level, l, m) amplitude produced by the mapping for this frame
    struct Contribution
    {
        int level = 0, l = 0, m = 0;
        double power = 0, omega = 0, sourceHz = 0, detuneCents = 0;
        juce::String reason;
    };
}

struct OrbitalModel::Impl
{
    // owned: the editor's model reads its own APVTS; a Jam Sesh tile passes a friend's settings
    Impl (const ViewParams& p, std::unique_ptr<ViewParams> owned = {}) : ownParams (std::move (owned)), params (p) {}

    // one bridge setting, read from the view's settings (the fallbacks are the parameter defaults)
    struct Setting
    {
        const ViewParams& src;
        const char* id;
        float fallback;
        float load() const { return src.get (id, fallback); }
    };

    std::unique_ptr<ViewParams> ownParams;
    const ViewParams& params;
    Setting pPotential { params, "orbPotential", 2.0f }, pMapping { params, "orbMapping", 3.0f }, pAngular { params, "orbAngular", 0.0f };
    Setting pBasis { params, "orbBasis", 1.0f }, pFundMode { params, "orbFundMode", 1.0f }, pFundHz { params, "orbFundHz", 130.81f };
    Setting pTimeScale { params, "orbTimeScale", 0.005f }, pQuantize { params, "orbQuantize", 0.75f }, pMaxTerms { params, "orbMaxTerms", 6.0f };
    Setting pMaxN { params, "orbMaxN", 10.0f }, pL { params, "orbL", 1.0f }, pM { params, "orbM", 2.0f };
    Setting pSmooth { params, "orbSmooth", 120.0f }, pGate { params, "orbGate", -50.0f }, pZ { params, "orbZ", 11.0f };
    Setting pScreen { params, "orbScreen", 1.0f }, pSolo { params, "orbSolo", 0.0f };

    // settings snapshot for this frame
    int potential = 0, mapping = 0, angular = 0, maxN = 5, atomZ = 11;
    double screening = 1.0;
    bool realBasis = true;
    double timeScale = 0.002, quantize = 0.5;

    // analysis
    std::unique_ptr<juce::dsp::FFT> fft;
    int fftOrder = 0;
    std::vector<float> window, bufL, bufR, mono, magDb;
    std::vector<Peak> peaks;
    double f0 = 110.0, centroidHz = 0.0;
    juce::String f0Source = "manual";

    // per-state smoothing and continuous phase, keyed by (level index, l, m)
    struct KeyState
    {
        double amp = 0, target = 0, phase = 0, omega = 0, sourceHz = 0, detuneCents = 0;
        juce::String reason;
    };
    std::map<std::tuple<int, int, int>, KeyState> states;
    double lastDisplayT = -1.0, lastWallMs = 0.0;

    std::vector<ModeTerm> terms;
    std::map<std::tuple<int, int, int, int>, std::unique_ptr<orbmath::RadialTable>> tables;   // (potential, n, l, Zeff x 1000)
    std::vector<orbmath::EvalTerm> eval;
    float extent = 5.0f, peakDensity = 1.0f;

    // The ladder of energy levels for the current potential. Element levels are subshells of the valence
    // electron (Atoms.h); exactly degenerate subshells of one shell merge into one level, like hydrogen's n.
    struct LevelInfo
    {
        int q = 1;                  // n (Coulomb) or shell N (oscillator)
        std::vector<int> ls;        // allowed shapes in this level
        double energy = 0.0, extent = 5.0;
        double zByL[4] = { 1.0, 1.0, 1.0, 1.0 };   // element: Slater effective charge per l (orbital size)
        juce::String label;
    };
    std::vector<LevelInfo> ladder;
    atoms::Ladder atomLadder;
    juce::String ladderKey;

    // "The sound is the electron": how present it is follows the loudness; silence = no electron
    double presence = 0.0, levelDb = -120.0;
    std::vector<float> scan;

    //==========================================================================
    bool hydrogenPot() const { return potential == hydrogen; }
    bool coulombPot() const  { return potential != harmonicOscillator; }
    int levelCount() const   { return (int) ladder.size(); }
    int levelNumber (int i) const { return ladder[(size_t) i].q; }
    double energy (int i) const   { return ladder[(size_t) i].energy; }

    // excitation above the ground level, in units of the first excitation
    // (hydrogen dE1 = 3/8 hartree, oscillator dE1 = 1 hbar w, element: its own first gap)
    double excitation (int i) const
    {
        if (ladder.size() < 2)
            return 0.0;
        const double de1 = energy (1) - energy (0);
        return de1 > 0.0 ? (energy (i) - energy (0)) / de1 : (double) i;
    }

    void buildLadder()
    {
        ladder.clear();
        if (potential == harmonicOscillator)
        {
            for (int i = 0; i < maxN; ++i)
            {
                LevelInfo v;
                v.q = i;
                for (int l = i % 2; l <= i; l += 2) v.ls.push_back (l);
                v.energy = i + 1.5;
                v.extent = orbmath::oscillatorExtent (i);
                v.label = "N=" + juce::String (i);
                ladder.push_back (v);
            }
            return;
        }
        if (potential == hydrogen)
        {
            for (int i = 0; i < maxN; ++i)
            {
                LevelInfo v;
                v.q = i + 1;
                for (int l = 0; l <= i; ++l) v.ls.push_back (l);
                v.energy = -0.5 / (v.q * v.q);
                v.extent = orbmath::hydrogenExtent (v.q);
                v.label = "n=" + juce::String (v.q);
                ladder.push_back (v);
            }
            return;
        }

        atomLadder = atoms::valenceLadder (atomZ, screening, maxN * 4);
        for (auto& a : atomLadder.levels)
        {
            const bool degenerate = ! ladder.empty() && ladder.back().q == a.n
                                    && std::abs (a.energy - ladder.back().energy) <= 1.0e-9 * std::abs (a.energy);
            if (degenerate)
            {
                auto& v = ladder.back();
                v.ls.push_back (a.l);
                v.zByL[a.l] = a.zShape;
                v.extent = std::max (v.extent, atoms::extent (a.n, a.zShape));
                v.label << juce::String::charToString ((juce::juce_wchar) atoms::letter (a.l));
                continue;
            }
            if ((int) ladder.size() == maxN)
                break;
            LevelInfo v;
            v.q = a.n;
            v.ls = { a.l };
            v.energy = a.energy;
            v.zByL[a.l] = a.zShape;
            v.extent = atoms::extent (a.n, a.zShape);
            v.label = juce::String (atoms::label (a.n, a.l));
            ladder.push_back (v);
        }
    }

    // audio-frequency equivalent of each level's TRUE energy: ground = f0, first excited = 2 f0
    double energyFreq (int i) const { return f0 * (1.0 + excitation (i)); }

    // audio frequency that lands exactly on level i, per mapping
    double levelFreq (int i) const
    {
        switch (mapping)
        {
            case rydbergHarmonic:  return f0 * (i + 1);
            case equalTemperament: return f0 * std::pow (2.0, i / 12.0);
            default:               return energyFreq (i);
        }
    }

    double levelExtent (int i) const               { return ladder[(size_t) i].extent; }
    const std::vector<int>& allowedL (int i) const { return ladder[(size_t) i].ls; }
    juce::String levelLabel (int i) const          { return ladder[(size_t) i].label; }

    juce::String stateLabel (int n, int l, int m) const
    {
        if (potential == element)
            return juce::String (atoms::label (n, l)) + " m=" + juce::String (m);
        return (hydrogenPot() ? "n=" : "N=") + juce::String (n) + " l=" + juce::String (l) + " m=" + juce::String (m);
    }

    static int closestAllowed (const std::vector<int>& ls, int want)
    {
        int best = ls.front();
        for (int l : ls)
            if (std::abs (l - want) < std::abs (best - want))
                best = l;
        return best;
    }

    // Angular source: choose (l, m) inside level i for one spectral peak
    std::pair<int, int> pickAngular (int i, const Peak& p, juce::String& why) const
    {
        const auto& ls = allowedL (i);
        int l = ls.front(), m = 0;
        switch (angular)
        {
            case angularStereo:
            {
                const auto idx = (size_t) std::round (juce::jlimit (0.0, 1.0, p.width) * (double) (ls.size() - 1));
                l = ls[idx];
                m = (int) std::round (p.phaseDiff / orbmath::pi * l);
                why = "stereo width " + juce::String ((int) std::round (p.width * 100.0)) + "% -> l, L/R phase "
                    + juce::String ((int) std::round (p.phaseDiff * 180.0 / orbmath::pi)) + " deg -> m";
                break;
            }
            case angularTimbre:
            {
                const double bright = juce::jlimit (0.0, 1.0, std::log2 (std::max (1.0, centroidHz / f0)) / 4.0);
                l = ls[(size_t) std::round (bright * (double) (ls.size() - 1))];
                const int h = std::max (1, (int) std::round (p.hz / f0));
                m = ((h - 1) % (2 * l + 1)) - l;
                why = "centroid " + juce::String (centroidHz / f0, 1) + " x f0 -> l, harmonic " + juce::String (h) + " -> m";
                break;
            }
            default:
            {
                l = closestAllowed (ls, (int) pL.load());
                m = (int) pM.load();
                why = "dials";
                break;
            }
        }
        m = juce::jlimit (-l, l, m);
        return { l, m };
    }

    // Adds the (l, m) choice for level i, expanding Spread into every allowed state
    void addLevel (std::vector<Contribution>& out, int i, const Peak& p, double power, double omega,
                   double detune, const juce::String& reason) const
    {
        if (angular == angularSpread)
        {
            std::vector<std::pair<int, int>> lm;
            for (int l : allowedL (i))
                for (int m = -l; m <= l; ++m)
                    lm.push_back ({ l, m });
            for (auto [l, m] : lm)
                out.push_back ({ i, l, m, power / (double) lm.size(), omega, p.hz, detune,
                                 reason + ", spread over " + juce::String ((int) lm.size()) + " states" });
            return;
        }
        juce::String why;
        auto [l, m] = pickAngular (i, p, why);
        out.push_back ({ i, l, m, power, omega, p.hz, detune, reason + ", " + why });
    }

    // some l in level i and some l in level j differ by exactly one (electric dipole rule)
    bool dipoleAllowed (int i, int j) const
    {
        for (int a : allowedL (i))
            for (int b : allowedL (j))
                if (std::abs (a - b) == 1)
                    return true;
        return false;
    }

    void mapPeak (const Peak& p, std::vector<Contribution>& out) const
    {
        const int count = levelCount();
        const double k = p.hz / f0;
        const double power = p.amp * p.amp;

        if (mapping == emissionLines)
        {
            if (count < 2)
            {
                addLevel (out, 0, p, power, twoPi * energyFreq (0) * timeScale, 0.0, "only one level: no lines");
                return;
            }

            int a = 0, b = 1;
            double lineHz = f0;
            if (coulombPot())
            {
                double best = 1.0e9;
                for (int i = 0; i < count; ++i)
                    for (int j = i + 1; j < count; ++j)
                    {
                        if (potential == element && ! dipoleAllowed (i, j))
                            continue;
                        const double t = energyFreq (j) - energyFreq (i);
                        const double c = std::abs (1200.0 * std::log2 (p.hz / t));
                        if (c < best) { best = c; a = i; b = j; lineHz = t; }
                    }
            }
            else
            {
                // dipole rule dN = 1: every oscillator line is exactly f0, so the harmonic number picks the pair
                a = juce::jlimit (0, count - 2, (int) std::round (k) - 1);
                b = a + 1;
                lineHz = energyFreq (b) - energyFreq (a);
            }

            const double detune = 1200.0 * std::log2 (p.hz / lineHz);
            const double rel = p.hz + (lineHz - p.hz) * quantize;   // slosh speed: tone at 0, exact line at 1
            const double omegaA = twoPi * energyFreq (a) * timeScale;
            const double omegaB = omegaA + twoPi * rel * timeScale;

            juce::String line;
            if (potential == element)
                line = levelLabel (a) + "->" + levelLabel (b);
            else if (hydrogenPot())
                line = juce::String (seriesName (a + 1)) + "-" + greek (b - a) + " " + juce::String (a + 1) + "->"
                     + juce::String (b + 1) + " (" + lineFraction (a + 1, b + 1) + " x f0)";
            else
                line = "N=" + juce::String (a) + "->" + juce::String (b) + " (dN=1 line = f0)";
            const juce::String reason = hzText (p.hz) + " ~ line " + line + ", " + centsText (detune);

            // upper state from the angular source, lower state one l step away (dipole-like dl = +-1)
            juce::String why;
            auto [lb, mb] = angular == angularSpread ? std::pair<int, int> { allowedL (b).back(), 0 } : pickAngular (b, p, why);
            const auto la = allowedL (a);
            int lA = closestAllowed (la, lb - 1);
            if (std::abs (lA - lb) != 1)
                lA = closestAllowed (la, lb + 1);
            const int mA = juce::jlimit (-lA, lA, mb);
            out.push_back ({ a, lA, mA, power * 0.5, omegaA, p.hz, detune, reason + " (lower)" });
            out.push_back ({ b, lb, mb, power * 0.5, omegaB, p.hz, detune,
                             reason + " (upper" + (why.isEmpty() ? juce::String() : ", " + why) + ")" });
            return;
        }

        // bracket the tone between the two levels around it
        int lo = 0, hi = 0;
        if (p.hz >= levelFreq (count - 1))
            lo = hi = count - 1;
        else if (p.hz > levelFreq (0))
        {
            while (lo + 1 < count && levelFreq (lo + 1) <= p.hz) ++lo;
            hi = juce::jmin (lo + 1, count - 1);
        }

        double t = 0.0;
        if (hi != lo && levelFreq (hi) > levelFreq (lo) * 1.000001)
            t = std::log2 (p.hz / levelFreq (lo)) / std::log2 (levelFreq (hi) / levelFreq (lo));
        const int nearest = t < 0.5 ? lo : hi;
        const double detune = 1200.0 * std::log2 (p.hz / levelFreq (nearest));
        const double detuneHz = p.hz - levelFreq (nearest);
        const double tq = t + (std::round (t) - t) * quantize;

        const char* rule = mapping == physicalLinear ? "linear" : mapping == rydbergHarmonic ? "Rydberg->harmonic" : "equal temp.";
        for (int side = 0; side < 2; ++side)
        {
            const int lvl = side == 0 ? lo : hi;
            const double w = side == 0 ? 1.0 - tq : tq;
            if (w < 1.0e-4 || (side == 1 && hi == lo))
                continue;
            const juce::String share = w < 0.999 ? ", " + juce::String ((int) std::round (w * 100)) + "% share" : juce::String();
            const juce::String reason = hzText (p.hz) + " = " + juce::String (k, 2) + " x f0 -> " + levelLabel (lvl)
                                      + " (" + rule + ", " + centsText (detune) + share + ")";
            addLevel (out, lvl, p, power * w, twoPi * (energyFreq (lvl) + detuneHz) * timeScale, detune, reason);
        }
    }

    //==========================================================================
    void analyse (const float* left, const float* right, int numSamples, double sr)
    {
        peaks.clear();
        if (left == nullptr || numSamples < 512 || sr <= 0.0)
        {
            levelDb = -120.0;
            return;
        }

        int order = 9;
        while (order < 14 && (1 << (order + 1)) <= numSamples) ++order;
        const int N = 1 << order;
        if (order != fftOrder)
        {
            fftOrder = order;
            fft = std::make_unique<juce::dsp::FFT> (order);
            window.resize ((size_t) N);
            for (int i = 0; i < N; ++i)
                window[(size_t) i] = 0.5f - 0.5f * (float) std::cos (twoPi * i / N);
            bufL.assign ((size_t) N * 2, 0.0f);
            bufR.assign ((size_t) N * 2, 0.0f);
            mono.assign ((size_t) N, 0.0f);
            magDb.assign ((size_t) N / 2 + 1, -140.0f);
        }

        const int start = numSamples - N;
        std::fill (bufL.begin(), bufL.end(), 0.0f);
        std::fill (bufR.begin(), bufR.end(), 0.0f);
        for (int i = 0; i < N; ++i)
        {
            const float l = left[start + i], r = right != nullptr ? right[start + i] : l;
            mono[(size_t) i] = 0.5f * (l + r);
            bufL[(size_t) i] = l * window[(size_t) i];
            bufR[(size_t) i] = r * window[(size_t) i];
        }
        fft->performRealOnlyForwardTransform (bufL.data(), true);
        fft->performRealOnlyForwardTransform (bufR.data(), true);

        // loudness of the newest ~40 ms sets how present the electron is (fast, unlike the FFT window)
        const int recent = std::min (N, std::max (256, (int) (0.04 * sr)));
        double sq = 0.0;
        for (int i = N - recent; i < N; ++i)
            sq += (double) mono[(size_t) i] * mono[(size_t) i];
        levelDb = juce::Decibels::gainToDecibels (std::sqrt (sq / recent), -120.0);

        auto bin = [] (const std::vector<float>& b, int k) { return std::complex<float> (b[(size_t) k * 2], b[(size_t) k * 2 + 1]); };

        // Hann coherent gain 0.5: a sine of amplitude A reads A * N / 4
        const float scale = 4.0f / (float) N;
        const int bins = N / 2;
        double cNum = 0.0, cDen = 0.0;
        for (int k = 0; k <= bins; ++k)
        {
            const float m = std::abs (bin (bufL, k) + bin (bufR, k)) * 0.5f * scale;
            magDb[(size_t) k] = juce::Decibels::gainToDecibels (m, -140.0f);
            const double f = k * sr / N;
            if (f >= 20.0) { cNum += f * m; cDen += m; }
        }
        centroidHz = cDen > 1.0e-9 ? cNum / cDen : 0.0;

        const float gateDb = pGate.load();
        const int kMin = std::max (2, (int) std::ceil (20.0 * N / sr));
        const int kMax = std::min (bins - 2, (int) (std::min (12000.0, sr * 0.45) * N / sr));
        float strongest = -140.0f;
        for (int k = kMin; k <= kMax; ++k)
            strongest = std::max (strongest, magDb[(size_t) k]);

        for (int k = kMin; k <= kMax; ++k)
        {
            const float b = magDb[(size_t) k];
            if (b <= magDb[(size_t) k - 1] || b < magDb[(size_t) k + 1] || b < gateDb || b < strongest - 36.0f)
                continue;
            const float a = magDb[(size_t) k - 1], c = magDb[(size_t) k + 1];
            const float denom = a - 2.0f * b + c;
            const float d = std::abs (denom) > 1.0e-9f ? 0.5f * (a - c) / denom : 0.0f;
            Peak p;
            p.hz = (k + d) * sr / N;
            p.amp = juce::Decibels::decibelsToGain (b - 0.25f * (a - c) * d);
            const auto L = bin (bufL, k), R = bin (bufR, k);
            p.phaseDiff = std::arg (R * std::conj (L));
            const double midE = std::norm (L + R), sideE = std::norm (L - R);
            p.width = sideE / (midE + sideE + 1.0e-20);
            peaks.push_back (p);
        }

        std::sort (peaks.begin(), peaks.end(), [] (const Peak& x, const Peak& y) { return x.amp > y.amp; });
        const auto maxPeaks = (size_t) juce::jlimit (1, 12, (int) pMaxTerms.load());
        if (peaks.size() > maxPeaks)
            peaks.resize (maxPeaks);

        // fundamental
        if ((int) pFundMode.load() == 1)
        {
            f0 = pFundHz.load();
            f0Source = "manual";
            return;
        }

        const int n = std::min (N, 8192);
        const float est = DspUtil::estimateFrequency (mono.data() + (N - n), n, sr);
        double candidate = 0.0;
        if (est >= 20.0f && est <= 2000.0f)
        {
            candidate = est;
            f0Source = "auto: tracked pitch";
        }
        else if (! peaks.empty())
        {
            double lowest = 1.0e9;
            for (auto& p : peaks)
                if (p.amp >= peaks.front().amp * 0.25 && p.hz < lowest) lowest = p.hz;
            if (lowest >= 20.0 && lowest <= 2000.0)
            {
                candidate = lowest;
                f0Source = "auto: lowest strong peak";
            }
        }
        if (candidate > 0.0 && std::abs (candidate / f0 - 1.0) > 0.01)
            f0 = candidate;
    }

    const orbmath::RadialTable* tableFor (int level, int l)
    {
        const int q = levelNumber (level);
        const int zKey = potential == element ? (int) std::round (ladder[(size_t) level].zByL[juce::jlimit (0, 3, l)] * 1000.0) : 0;
        const auto key = std::make_tuple (potential, q, l, zKey);
        auto it = tables.find (key);
        if (it != tables.end())
            return it->second.get();
        auto t = std::make_unique<orbmath::RadialTable>();
        if (potential == element)
        {
            // hydrogen-like orbital of effective charge Z: R(r) = Z^(3/2) R_nl(Z r)
            const double zs = ladder[(size_t) level].zByL[juce::jlimit (0, 3, l)];
            const double k = std::pow (zs, 1.5);
            t->build ([q, l, zs, k] (double r) { return k * orbmath::hydrogenRadial (q, l, zs * r); }, levelExtent (level) * 1.6);
        }
        else if (hydrogenPot())
            t->build ([q, l] (double r) { return orbmath::hydrogenRadial (q, l, r); }, levelExtent (level) * 1.6);
        else
            t->build ([q, l] (double r) { return orbmath::oscillatorRadial (q, l, r); }, levelExtent (level) * 1.6);
        return (tables[key] = std::move (t)).get();
    }

    void step (double displayT)
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double wallDt = lastWallMs > 0.0 ? juce::jlimit (0.0, 0.5, (nowMs - lastWallMs) * 0.001) : 0.0;
        lastWallMs = nowMs;
        double dt = lastDisplayT >= 0.0 ? displayT - lastDisplayT : 0.0;
        if (dt < 0.0 || dt > 1.0) dt = 0.0;
        lastDisplayT = displayT;

        std::vector<Contribution> contribs;
        for (auto& p : peaks)
            mapPeak (p, contribs);

        // the sound IS the electron: no sound, no electron (it fades out with the response time)
        const double gate = pGate.load();
        const double presenceTarget = contribs.empty() ? 0.0
                                    : juce::jlimit (0.0, 1.0, (levelDb - gate) / std::max (1.0, -6.0 - gate));
        const double tauP = presenceTarget > presence ? 0.012 : std::max (0.02, pSmooth.load() * 0.001);
        presence += (presenceTarget - presence) * (wallDt > 0.0 ? 1.0 - std::exp (-wallDt / tauP) : 1.0);

        for (auto& kv : states)
            kv.second.target = 0.0;

        // merge contributions for the same state: powers add, the strongest one names it
        std::map<std::tuple<int, int, int>, double> strongest;
        for (auto& c : contribs)
        {
            const auto key = std::make_tuple (c.level, c.l, c.m);
            auto& s = states[key];
            s.target = std::sqrt (s.target * s.target + c.power);
            if (c.power > strongest[key])
            {
                strongest[key] = c.power;
                s.omega = c.omega;
                s.sourceHz = c.sourceHz;
                s.detuneCents = c.detuneCents;
                s.reason = c.reason;
            }
        }

        const double tau = std::max (0.01, pSmooth.load() * 0.001);
        const double alphaDown = wallDt > 0.0 ? 1.0 - std::exp (-wallDt / tau) : 1.0;
        const double alphaUp = wallDt > 0.0 ? 1.0 - std::exp (-wallDt / (tau * 0.25)) : 1.0;   // attacks 4x faster
        double loudest = 0.0;
        for (auto& kv : states)
            loudest = std::max (loudest, kv.second.target);

        for (auto it = states.begin(); it != states.end();)
        {
            auto& s = it->second;
            const double target = loudest > 0.0 ? s.target / loudest : 0.0;
            s.amp += (target - s.amp) * (target > s.amp ? alphaUp : alphaDown);
            s.phase = std::fmod (s.phase + s.omega * dt, twoPi);
            if (s.target == 0.0 && s.amp < 0.003)
                it = states.erase (it);
            else
                ++it;
        }

        // the strongest states become the superposition
        std::vector<std::pair<double, std::tuple<int, int, int>>> order;
        for (auto& kv : states)
            order.push_back ({ kv.second.amp, kv.first });
        std::sort (order.begin(), order.end(), [] (const auto& x, const auto& y) { return x.first > y.first; });
        const size_t keep = pSolo.load() > 0.5f ? 1 : (size_t) maxEvalTerms;   // solo: one clean textbook orbital
        if (order.size() > keep)
            order.resize (keep);

        double sumSq = 0.0;
        for (auto& o : order)
            sumSq += o.first * o.first;
        const double normAmp = sumSq > 0.0 ? 1.0 / std::sqrt (sumSq) : 0.0;

        terms.clear();
        eval.clear();
        if (order.empty())
            return;   // silence: nothing to draw, keep the last framing so the camera doesn't jump
        extent = (float) levelExtent (0) * 0.5f;
        for (auto& o : order)
        {
            const auto [lvl, l, m] = o.second;
            const auto& s = states[o.second];
            ModeTerm t;
            t.n = levelNumber (lvl);
            t.l = l;
            t.m = m;
            t.c = std::polar (s.amp * normAmp, -s.phase);
            t.energy = energy (lvl);
            t.displayOmega = s.omega;
            t.sourceHz = s.sourceHz;
            t.detuneCents = s.detuneCents;
            t.reason = s.reason;
            terms.push_back (t);

            orbmath::EvalTerm e;
            e.radial = tableFor (lvl, l);
            e.l = l;
            e.m = m;
            e.realBasis = realBasis;
            e.k = (float) (orbmath::ylmNorm (l, std::abs (m)) * (realBasis && m != 0 ? std::sqrt (2.0) : 1.0));
            e.c = std::complex<float> (t.c);
            eval.push_back (e);

            if (std::norm (t.c) > 0.01)
                extent = std::max (extent, (float) levelExtent (lvl));
        }

        // Exposure reference from a coarse grid scan: the density level above which the brightest 3% of the
        // probability lies. The raw maximum would let the needle-thin spike that s orbitals have at the
        // nucleus (tiny volume, almost no probability) dim the whole cloud.
        const int g = 18;
        scan.resize ((size_t) g * g * g);
        double total = 0.0;
        size_t idx = 0;
        for (int i = 0; i < g; ++i)
            for (int j = 0; j < g; ++j)
                for (int k = 0; k < g; ++k)
                {
                    const float x = extent * (2.0f * ((float) i + 0.5f) / g - 1.0f);
                    const float y = extent * (2.0f * ((float) j + 0.5f) / g - 1.0f);
                    const float z = extent * (2.0f * ((float) k + 0.5f) / g - 1.0f);
                    scan[idx] = std::norm (orbmath::evalPsi (eval.data(), (int) eval.size(), x, y, z));
                    total += scan[idx++];
                }
        std::sort (scan.begin(), scan.end(), std::greater<float>());
        float reference = scan.front();
        double acc = 0.0;
        for (float v : scan)
        {
            acc += v;
            reference = v;
            if (acc >= 0.03 * total)
                break;
        }
        peakDensity = std::max (reference, 1.0e-9f);

        // peakDensity is for the normalised state; the drawn psi carries the electron's presence
        const float presenceAmp = (float) std::sqrt (juce::jlimit (0.0, 1.0, presence));
        for (auto& e : eval)
            e.c *= presenceAmp;
    }
};

//==============================================================================
OrbitalModel::OrbitalModel (juce::AudioProcessorValueTreeState& state)
{
    auto owned = std::make_unique<ApvtsParams> (state);
    const ViewParams& ref = *owned;
    impl = std::make_unique<Impl> (ref, std::move (owned));
}

OrbitalModel::OrbitalModel (const ViewParams& params) : impl (std::make_unique<Impl> (params)) {}
OrbitalModel::~OrbitalModel() = default;

void OrbitalModel::update (const float* left, const float* right, int numSamples, double sampleRate, double displayTimeSeconds)
{
    auto& d = *impl;
    d.potential = juce::jlimit (0, 2, (int) d.pPotential.load());
    d.mapping   = (int) d.pMapping.load();
    d.angular   = (int) d.pAngular.load();
    d.realBasis = (int) d.pBasis.load() == 1;
    d.timeScale = d.pTimeScale.load();
    d.quantize  = d.pQuantize.load();
    d.maxN      = juce::jlimit (1, 12, (int) d.pMaxN.load());
    d.atomZ     = juce::jlimit (1, atoms::maxZ, (int) d.pZ.load());
    d.screening = d.pScreen.load();

    // a different ladder means level indices name different states: start the superposition afresh
    const juce::String key = juce::String (d.potential) + "/" + juce::String (d.maxN) + "/"
                           + (d.potential == element ? juce::String (d.atomZ) + "/" + juce::String (d.screening, 3) : juce::String());
    if (key != d.ladderKey)
    {
        d.ladderKey = key;
        d.buildLadder();
        d.states.clear();
    }

    d.analyse (left, right, numSamples, sampleRate);
    d.step (displayTimeSeconds);
}

const std::vector<ModeTerm>& OrbitalModel::terms() const noexcept { return impl->terms; }

std::complex<float> OrbitalModel::psi (float x, float y, float z) const noexcept
{
    return orbmath::evalPsi (impl->eval.data(), (int) impl->eval.size(), x, y, z);
}

float OrbitalModel::extent() const noexcept { return impl->extent; }
float OrbitalModel::peakDensityEstimate() const noexcept { return impl->peakDensity; }

juce::String OrbitalModel::describeMapping() const
{
    const auto& d = *impl;
    juce::StringArray lines;
    const int count = d.levelCount();

    lines.add ("THE SOUND IS THE ELECTRON: its presence follows the loudness (now " + juce::String ((int) std::round (d.presence * 100.0))
               + "%, " + juce::String (d.levelDb, 1) + " dBFS; Peak gate " + juce::String (d.pGate.load(), 0)
               + " dB = absent, -6 dBFS = fully present). Silence = no electron, nothing is drawn.");
    if (d.potential == element)
    {
        const auto& lad = d.atomLadder;
        lines.add ("Potential: atom Z = " + juce::String (d.atomZ) + " " + atoms::symbol (d.atomZ) + " (" + atoms::name (d.atomZ) + "), screening "
                   + juce::String ((int) std::round (d.screening * 100.0)) + "%. The sound is the outermost electron; the other "
                   + juce::String (d.atomZ - 1) + " electrons fill the inner subshells (aufbau order), so those subshells are Pauli-blocked.");
        lines.add ("Energies: quantum-defect formula E = -Z_net^2 / (2 (n - delta_l)^2) hartree, Z_net = " + juce::String (lad.zNet, 2)
                   + "; defects s " + juce::String (d.screening * lad.defects[0], 2) + ", p " + juce::String (d.screening * lad.defects[1], 2)
                   + ", d " + juce::String (d.screening * lad.defects[2], 2) + ", f " + juce::String (d.screening * lad.defects[3], 2)
                   + " (low-l orbits dive into the " + juce::String (lad.coreShells) + " closed core shells and feel more of the nucleus).");
        lines.add ("Shapes: hydrogen-like orbitals with Slater effective charge Zeff (same nodes and lobes as hydrogen, sized like the real atom).");
        juce::String ladderText;
        for (int i = 0; i < count; ++i)
            ladderText << (i > 0 ? ", " : "") << d.levelLabel (i) << " " << juce::String (d.energy (i), 3) << " Ha (Zeff "
                       << juce::String (d.ladder[(size_t) i].zByL[d.ladder[(size_t) i].ls.front()], 2) << ")";
        lines.add ("Ladder (lowest first): " + ladderText + ".");
    }
    else if (d.hydrogenPot())
        lines.add ("Potential: hydrogen (Coulomb). Levels n = 1.." + juce::String (count) + ", E_n = -1/(2 n^2) hartree - they bunch together as n grows.");
    else
        lines.add ("Potential: 3D harmonic oscillator. Shells N = 0.." + juce::String (count - 1) + ", E = N + 3/2 (hbar w) - equally spaced, like a harmonic series.");

    lines.add ("Fundamental f0 = " + hzText (d.f0) + " (" + d.f0Source + ").");

    juce::String levels;
    for (int i = 0; i < count; ++i)
        levels << (i > 0 ? ", " : "") << d.levelLabel (i) << " " << hzText (d.levelFreq (i));

    switch (d.mapping)
    {
        case physicalLinear:
            lines.add ("Level mapping: Physical (linear). Energy is a straight-line function of frequency: f = f0 x (1 + excitation / first excitation). "
                       "Ground = f0, first excited = 2 f0. Frequency DIFFERENCES are kept, so on-screen beats match the beats you hear.");
            break;
        case rydbergHarmonic:
            lines.add ("Level mapping: Rydberg -> harmonic. Level number = f / f0 (n = sqrt(R/|E|) inverted), so f0, 2 f0, 3 f0... land exactly on successive levels. "
                       "RATIOS are kept; the cloud still moves with the true energies, so on-screen beats are not the audible beats.");
            break;
        case emissionLines:
            if (d.potential == element)
                lines.add ("Level mapping: Emission lines. Each tone is a transition between two subshells of this atom that obey the dipole rule (l changes by 1), "
                           "scaled so the first gap (" + (count > 1 ? d.levelLabel (0) + "->" + d.levelLabel (1) : juce::String ("-")) + ") = f0. "
                           "The pair is mixed equally and sloshes at the tone frequency x time scale.");
            else if (d.hydrogenPot())
                lines.add ("Level mapping: Emission lines. Each tone is a transition frequency f = f0 x (4/3)(1/n1^2 - 1/n2^2) (Lyman-alpha 1->2 = f0, Lyman-beta = 32/27 f0). "
                           "The two states are mixed equally, so the cloud sloshes at the tone frequency x time scale - the motion that really radiates that light.");
            else
                lines.add ("Level mapping: Emission lines. Oscillator dipole rule dN = 1: every line is exactly f0. The tone's harmonic number k picks the pair N=k-1 -> N=k; "
                           "the mix sloshes at the tone frequency x time scale.");
            break;
        default:
            lines.add ("Level mapping: Equal temperament. Level index = 12 x log2(f / f0) semitones, so each semitone above f0 is the next level. "
                       "The cloud moves with the true energies.");
            break;
    }
    if (d.mapping != emissionLines)
        lines.add ("Level frequencies: " + levels + ".");

    const double ts = d.timeScale;
    lines.add ("Time scale (hbar): 1 audio Hz -> " + juce::String (ts, ts < 0.01 ? 5 : 3) + " Hz on screen; a 1 Hz beat between two tones takes "
               + juce::String (ts > 0.0 ? 1.0 / ts : 0.0, 0) + " s per cycle.");
    lines.add ("Quantize " + juce::String ((int) std::round (d.quantize * 100.0)) + "%: "
               + (d.mapping == emissionLines ? juce::String ("0 = sloshes at the tone's own frequency, 100 = at the exact line frequency.")
                                             : juce::String ("0 = in-between tones split across neighbour levels (linear in cents), 100 = snap to the nearest level.")));

    static const char* angularText[] = {
        "Dials - l and m set by the L and M dials (clamped to the level's allowed values).",
        "Stereo - stereo width (side / total energy) picks l from low to high, the L/R phase difference picks m (sign = rotation direction).",
        "Timbre - spectral centroid / f0 (1x..16x, log) picks l, the harmonic number picks m.",
        "Spread - each tone's energy is shared evenly over every (l, m) of its level." };
    lines.add ("Angular source: " + juce::String (angularText[juce::jlimit (0, 3, d.angular)]));
    lines.add (juce::String ("Basis: ") + (d.realBasis ? "real (chemistry lobes px, py, dxy...)" : "complex (m = angular momentum about z, rings)") + ".");

    lines.add (d.terms.empty() ? juce::String ("Current terms: none (silence - no electron).") : juce::String ("Current terms:"));
    for (auto& t : d.terms)
        lines.add ("  " + d.stateLabel (t.n, t.l, t.m) + "  |c|^2 = " + juce::String (std::norm (t.c), 2) + "  - " + t.reason);
    return lines.joinIntoString ("\n");
}

juce::StringArray OrbitalModel::overlayLines() const
{
    juce::StringArray lines;
    const auto& d = *impl;
    juce::String head = d.potential == element ? juce::String (atoms::symbol (d.atomZ)) + " (Z=" + juce::String (d.atomZ) + ")  " : juce::String();
    if (d.terms.empty())
        lines.add (head + "silence - no electron. Play a sound.");
    else
        lines.add (head + "electron presence " + juce::String ((int) std::round (d.presence * 100.0)) + "%  (" + juce::String (d.levelDb, 1) + " dBFS)");
    for (auto& t : impl->terms)
        lines.add (d.stateLabel (t.n, t.l, t.m)
                   + "  " + juce::String ((int) std::round (std::norm (t.c) * 100.0)) + "%"
                   + (t.sourceHz > 0.0 ? "  " + hzText (t.sourceHz) + " (" + centsText (t.detuneCents) + ")" : juce::String()));
    return lines;
}

//==============================================================================
void OrbitalModel::addParameters (ParamList& p)
{
    using namespace juce;
    using FloatAttr = AudioParameterFloatAttributes;

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "orbPotential", 1 }, "Potential",
        StringArray { "Harmonic oscillator", "Hydrogen (Coulomb)", "Atom (element Z)" }, 2));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "orbMapping", 1 }, "Level Mapping",
        StringArray { "Physical (linear)", "Rydberg -> harmonic", "Emission lines", "Equal temperament" }, 3));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "orbAngular", 1 }, "Angular Source",
        StringArray { "Dials", "Stereo", "Timbre", "Spread" }, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "orbBasis", 1 }, "Orbital Basis",
        StringArray { "Complex (m states)", "Real (chemistry lobes)" }, 1));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "orbFundMode", 1 }, "Fundamental Mode",
        StringArray { "Auto (track pitch)", "Manual" }, 1));

    NormalisableRange<float> fund (20.0f, 2000.0f);
    fund.setSkewForCentre (200.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "orbFundHz", 1 }, "Fundamental", fund, 130.81f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return hzText (v); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "orbTimeScale", 1 }, "Time Scale (hbar)",
        logRange (0.00001f, 1.0f), 0.005f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return "x" + String (v, v < 0.01f ? 5 : 3); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "orbQuantize", 1 }, "Quantize",
        NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.75f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String ((int) std::round (v * 100.0f)) + " %"; })));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "orbMaxTerms", 1 }, "Max Terms", 1, 12, 6));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "orbMaxN", 1 }, "Max Level", 1, 12, 10));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "orbL", 1 }, "Dial l", 0, 7, 1));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "orbM", 1 }, "Dial m", -7, 7, 2));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "orbSmooth", 1 }, "Response Time",
        logRange (10.0f, 2000.0f), 120.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String ((int) std::round (v)) + " ms"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "orbGate", 1 }, "Peak Gate",
        NormalisableRange<float> (-90.0f, 0.0f, 0.5f), -50.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "orbZ", 1 }, "Element Z", 1, atoms::maxZ, 11,
        AudioParameterIntAttributes().withStringFromValueFunction ([] (int z, int)
            { return String (z) + " " + atoms::symbol (z) + " (" + atoms::name (z) + ")"; })));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "orbSolo", 1 }, "Strongest State Only", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "orbScreen", 1 }, "Screening",
        NormalisableRange<float> (0.0f, 1.5f, 0.01f), 1.0f,
        FloatAttr().withStringFromValueFunction ([] (float v, int) { return String ((int) std::round (v * 100.0f)) + " %"; })));
}

HelpList OrbitalModel::helpEntries()
{
    return {
        { "orbPotential", "Potential",
          "Chooses the 'atom' the sound is bound in, which fixes the shapes and energies of the allowed standing waves.",
          "Harmonic oscillator: a 3D spring-like potential. Shells N = 0, 1, 2... with energy E = N + 3/2 (hbar w) - equally spaced, so a note's harmonic "
          "series (f0, 2 f0, 3 f0...) lines up with the levels perfectly and both ratios and beats survive. Allowed shapes in shell N: l = N, N-2, ... 0. "
          "Hydrogen (Coulomb): the real atom. Levels n = 1, 2, 3... with E_n = -1/(2 n^2) hartree, bunching together as n grows; shapes l = 0..n-1 (s, p, d, f...). "
          "Atom (element Z): the sound is the outermost electron of element Z (1..86). The other Z-1 electrons fill the inner subshells in aufbau order, "
          "so the electron can only use the subshells they leave free. Each level is one real subshell (3s, 3p, 4s, 3d...) with a FIXED shape l, ordered by "
          "energy E = -Z_net^2 / (2 (n - delta_l)^2) (quantum defects fitted to the alkali metals: sodium's 3s->3p gap comes out within 5% of the real D line). "
          "Orbitals are hydrogen-like, sized by Slater's effective charge, so d orbitals of transition metals are compact and outer s/p orbitals large.",
          "Oscillator is the musical one; hydrogen the textbook one; Atom gives each element its own ladder of shapes. Max level limits how many levels exist." },

        { "orbZ", "Element Z",
          "Which atom the electron belongs to (used when Potential = Atom).",
          "Z = number of protons (1 H .. 86 Rn). The ground subshell is where electron number Z goes in aufbau order (1s 2s 2p 3s 3p 4s 3d 4p 5s ...); "
          "inner subshells already full of the other Z-1 electrons are skipped (Pauli). The ladder is the free subshells sorted by energy. "
          "Simplified model: one active electron in the field of nucleus + core; real exceptions (Cr, Cu) and multi-electron coupling are ignored.",
          "Alkalis (Li 3, Na 11, K 19, Rb 37, Cs 55) are the most accurate. Na: 3s 3p 4s 3d 4p...; K puts 3d ABOVE 4s, as in the real atom." },

        { "orbScreen", "Screening",
          "How strongly the other electrons hide the nucleus - 100% is the real atom; change it to build custom atoms.",
          "Scales every core effect: the net charge seen far away Z_net = Z - s (Z - 1), the quantum defects delta_l x s, and Slater's shielding x s "
          "(orbital size Zeff = Z - s x S). 0% = no core electrons: a bare nucleus of charge Z with one electron (hydrogen-like ion, levels Z^2 deeper and "
          "orbitals Z times smaller). 100% = real neutral atom. Above 100% exaggerates penetration, spreading s, p and d further apart.",
          "Sweep it to watch an atom turn into a hydrogen-like ion." },

        { "orbMapping", "Level mapping",
          "How an audio frequency picks an energy level (f0 = the fundamental).",
          "Physical (linear): energy is a straight line in frequency, f = f0 x (1 + excitation / first excitation): ground = f0, first excited = 2 f0. "
          "Hydrogen's other levels then sit at 2.19 f0, 2.25 f0... below 2.33 f0 (ionisation). Keeps frequency DIFFERENCES: on-screen beats = audible beats. "
          "Rydberg -> harmonic: level number = f / f0 (the inverse of n = sqrt(R/|E|)), so f0, 2 f0, 3 f0 land exactly on levels 1, 2, 3. Keeps RATIOS (intervals) "
          "but not differences - the cloud moves with the true energies, so its beats differ from what you hear. "
          "Emission lines: the tone is a TRANSITION frequency f = f0 x (4/3)(1/n1^2 - 1/n2^2), scaled so Lyman-alpha (1->2) = f0 (Lyman-beta = 32/27 f0, a Pythagorean minor third). "
          "The nearest line in cents is picked and its two states are mixed equally, so the cloud sloshes at the tone frequency - the motion that really emits that light. "
          "Oscillator: the dipole rule dN = 1 makes every line exactly f0, so the harmonic number k picks the pair N=k-1 -> N=k. "
          "Equal temperament: level index = 12 x log2(f / f0) semitones - one level per semitone above f0; the cloud moves with the true energies.",
          "Physical keeps the physics, Rydberg -> harmonic keeps the music, Emission lines is the most literal light-matter analogy. "
          "Equal temperament (default) is the easiest way to SEE the shapes: with sodium and f0 = C3, C = 3s sphere, C# = 3p dumbbell, D = 4s, D# = 3d clover, "
          "E = 4p, F = 5s, F# = 4d, G = 4f flower..." },

        { "orbAngular", "Angular source",
          "Chooses the shape (l) and orientation (m) inside an energy level - the information a pure tone doesn't carry.",
          "Dials: l and m come from the Dial l / Dial m controls, clamped to what the level allows. "
          "Stereo: per tone, stereo width (side energy / total, 0..100%) picks l from the lowest to the highest allowed value, and the L/R phase difference "
          "(-180..180 deg) picks m = round(phase / 180 x l) - the sign gives the rotation direction. Mono = lowest l, m = 0. "
          "Timbre: the spectral centroid as a multiple of f0 (1x..16x, log scale) picks l (brighter = more lobes); the tone's harmonic number h picks m = ((h-1) mod (2l+1)) - l. "
          "Spread: the tone's energy is shared evenly over every (l, m) of its level with fixed, deterministic phases - lumpy full shells.",
          "In Emission lines mode the source picks the upper state; the lower state is one l step away (dipole-like dl = +-1)." },

        { "orbBasis", "Orbital basis",
          "Which family of angular shapes is used for each (l, m).",
          "Complex (m states): Y_lm ~ P_l^m(cos theta) x e^{i m phi} - definite angular momentum m about the vertical axis; the density alone looks like rings and "
          "doughnuts while the phase winds m times around. "
          "Real (chemistry lobes): cos(m phi) for m > 0, sin(|m| phi) for m < 0 - the px / py / dxy lobes from chemistry books. Same energies, different shapes; "
          "the sign of m then selects cos vs sin instead of a rotation direction.",
          "" },

        { "orbFundMode", "Fundamental mode",
          "Where f0 - the reference every mapping divides by - comes from.",
          "Manual (default): f0 = the Fundamental dial (C3 = 130.81 Hz), a fixed ground, so the PITCH you play picks the level - higher notes climb to more "
          "excited orbitals with new shapes. Auto (track pitch): f0 = the pitch tracked from the audio (zero-crossing detector; if there's no clear pitch, the lowest "
          "spectral peak at least 25% as strong as the strongest; updates when it moves > 1%). Then every note's fundamental IS the ground state (an s sphere for "
          "most atoms) and only its overtones reach p, d and f.",
          "Manual + Equal temperament = play a scale to walk through the shapes. Auto shows the timbre of one note instead." },

        { "orbSolo", "Strongest state only",
          "Shows only the single strongest state, as one clean textbook orbital.",
          "Off: psi = the whole superposition of every mapped state (mixes like s + p look like lopsided hybrids and slosh in time). "
          "On: psi = only the state with the largest amplitude, normalised - a pure s sphere, p dumbbell, d clover or f flower that stands still.",
          "Turn it on to learn the shapes, off to see how sounds mix them." },

        { "orbFundHz", "Fundamental",
          "The reference frequency f0 when Fundamental mode is Manual.",
          "Ground level = f0 in every mapping except Emission lines, where the Lyman-alpha line (1->2) = f0. All other levels and lines are placed relative to it.",
          "Set it to the root of the key you're playing in." },

        { "orbTimeScale", "Time scale (hbar)",
          "The bridge constant between audio frequency and how fast the cloud moves.",
          "On-screen phase speed = audio frequency x time scale (for the motion between tones: beat frequency x time scale). At x0.002, a 1 Hz beat takes "
          "500 s per cycle and a 220 Hz difference cycles at 0.44 Hz. A single pure state never changes shape at any setting - only mixes of states move.",
          "Raise it to see fast sloshing, lower it to freeze chords into slowly breathing shapes." },

        { "orbQuantize", "Quantize",
          "How strictly tones snap to energy levels.",
          "0%: a tone between two levels is split between them by its position in cents (linear), and its detune from the nearest level becomes a slow extra phase drift "
          "(detune Hz x time scale). 100%: snap to the nearest level. In between: the split is pulled toward the nearest level. "
          "Emission lines: 0% = the pair sloshes at the tone's own frequency, 100% = at the exact line frequency.",
          "" },

        { "orbMaxTerms", "Max terms",
          "How many spectral peaks (loudest first) become states.",
          "Each peak above the Peak gate maps to one state (two in Emission lines, every (l, m) of the level in Spread). At most 32 states are drawn in total.",
          "Fewer terms = cleaner shapes and faster rendering." },

        { "orbMaxN", "Max level",
          "How many energy levels exist.",
          "Hydrogen: n = 1..Max level. Oscillator: shells N = 0..Max level - 1. Atom: the lowest Max level free subshells of the element. "
          "Tones above the top level are clamped to it (the detune shows how far off they are).",
          "Higher levels are larger and have more nodes." },

        { "orbL", "Dial l",
          "Shape quantum number used when Angular source = Dials.",
          "l = number of angular nodal surfaces (0 = s sphere, 1 = p dumbbell, 2 = d clover, 3 = f...). Clamped to the closest value the level allows "
          "(hydrogen: 0..n-1, oscillator: N, N-2, ... 0).",
          "" },

        { "orbM", "Dial m",
          "Orientation quantum number used when Angular source = Dials.",
          "m = -l..l (clamped). Complex basis: angular momentum about the vertical axis. Real basis: m > 0 = cos lobes, m < 0 = sin lobes, m = 0 = aligned with the vertical axis. "
          "Default 2 shows the iconic shapes: p -> px dumbbell (clamped to 1), d -> d(x2-y2) four-leaf clover, f -> eight-lobed flower; m = 0 gives the 'z' versions (d_z2 = dumbbell + ring).",
          "" },

        { "orbSmooth", "Response time",
          "How quickly the state amplitudes follow the audio.",
          "Each state's amplitude moves toward its target with this time constant (exponential smoothing in real time; rises 4x faster than it falls). "
          "The electron's presence appears within ~12 ms of a sound and fades out over this time after it stops. Phases always run continuously, so slow settings never jump.",
          "Short = reacts to every note; long = calm, averaged shapes." },

        { "orbGate", "Peak gate",
          "Ignores quiet spectral peaks.",
          "Only peaks louder than this level (dBFS, as sine amplitude) and within 36 dB of the loudest peak become states. It is also the floor of the "
          "electron's presence: presence = (loudness - gate) / (-6 dBFS - gate), clamped 0..1, using the RMS of the newest 40 ms. "
          "Below the gate there is no electron and nothing is drawn.",
          "Raise it if room noise creates flickering extra states." },
    };
}
}
