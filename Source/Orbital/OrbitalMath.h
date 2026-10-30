#pragma once

// Pure math for bound-state wavefunctions (no JUCE), so it can be unit-tested standalone.
// Hydrogen in Bohr radii, oscillator in its natural length sqrt(hbar / m omega).

#include <cmath>
#include <complex>
#include <vector>

namespace orbmath
{
    constexpr double pi = 3.14159265358979323846;

    // Generalised Laguerre polynomial L_n^(alpha)(x) by the standard three-term recurrence
    inline double genLaguerre (int n, double alpha, double x)
    {
        if (n <= 0) return 1.0;
        double l0 = 1.0, l1 = 1.0 + alpha - x;
        for (int k = 1; k < n; ++k)
        {
            const double l2 = ((2.0 * k + 1.0 + alpha - x) * l1 - (k + alpha) * l0) / (k + 1.0);
            l0 = l1;
            l1 = l2;
        }
        return l1;
    }

    inline double logFactorial (int n) { return std::lgamma (n + 1.0); }

    // Normalised hydrogen radial function R_nl(r): integral of R^2 r^2 dr = 1
    inline double hydrogenRadial (int n, int l, double r)
    {
        const double rho = 2.0 * r / n;
        const double logNorm = 0.5 * (3.0 * std::log (2.0 / n) + logFactorial (n - l - 1)
                                      - std::log (2.0 * n) - logFactorial (n + l));
        return std::exp (logNorm - 0.5 * rho) * std::pow (rho, l) * genLaguerre (n - l - 1, 2.0 * l + 1.0, rho);
    }

    // Normalised 3D isotropic oscillator radial function for shell N (= 2 n_r + l)
    inline double oscillatorRadial (int N, int l, double r)
    {
        const int nr = (N - l) / 2;
        const double logNorm = 0.5 * (std::log (2.0) + logFactorial (nr) - std::lgamma (nr + l + 1.5));
        return std::exp (logNorm - 0.5 * r * r) * std::pow (r, l) * genLaguerre (nr, l + 0.5, r * r);
    }

    // sqrt((2l+1)/(4 pi) * (l-m)!/(l+m)!), m >= 0
    inline double ylmNorm (int l, int am)
    {
        return std::sqrt ((2.0 * l + 1.0) / (4.0 * pi) * std::exp (logFactorial (l - am) - logFactorial (l + am)));
    }

    inline double hydrogenExtent (int n)   { return 2.5 * n * n + 4.0; }
    inline double oscillatorExtent (int N) { return std::sqrt (2.0 * N + 3.0) + 2.0; }

    // Radial function sampled on [0, rMax] for fast linear-interpolated lookup
    struct RadialTable
    {
        std::vector<float> v;
        float rMax = 1.0f, scale = 0.0f;

        template <typename Fn>
        void build (Fn&& radial, double maxRadius, int size = 2048)
        {
            rMax = (float) maxRadius;
            v.resize ((size_t) size);
            for (int i = 0; i < size; ++i)
                v[(size_t) i] = (float) radial (maxRadius * i / (size - 1));
            scale = (float) (size - 1) / rMax;
        }

        float at (float r) const noexcept
        {
            const float p = r * scale;
            const int i = (int) p;
            if (i >= (int) v.size() - 1)
                return 0.0f;
            return v[(size_t) i] + (v[(size_t) i + 1] - v[(size_t) i]) * (p - (float) i);
        }
    };

    // One term of a superposition, ready for evaluation: c * R(r) * Y(theta, phi)
    struct EvalTerm
    {
        const RadialTable* radial = nullptr;
        int l = 0, m = 0;
        bool realBasis = false;
        float k = 0.0f;                 // ylmNorm(l, |m|), times sqrt(2) for real m != 0
        std::complex<float> c;
    };

    constexpr int maxL = 8;

    // Associated Legendre P_l^m(ct) without the Condon-Shortley phase (a consistent basis choice)
    inline float legendre (int l, int am, float ct, float st) noexcept
    {
        float pmm = 1.0f;
        for (int i = 1; i <= am; ++i)
            pmm *= (2.0f * i - 1.0f) * st;
        if (l == am)
            return pmm;
        float pm1 = ct * (2.0f * am + 1.0f) * pmm;
        for (int ll = am + 2; ll <= l; ++ll)
        {
            const float p = (ct * (2.0f * ll - 1.0f) * pm1 - (ll + am - 1.0f) * pmm) / (float) (ll - am);
            pmm = pm1;
            pm1 = p;
        }
        return pm1;
    }

    // psi(x, y, z) = sum over terms of c * R(r) * Y_lm. Complex basis: e^{i m phi}, with
    // Y_{l,-m} = conj(Y_{l,m}). Real basis: m > 0 -> cos(m phi), m < 0 -> sin(|m| phi).
    inline std::complex<float> evalPsi (const EvalTerm* terms, int count, float x, float y, float z) noexcept
    {
        const float rxy2 = x * x + y * y;
        const float r = std::sqrt (rxy2 + z * z);
        float ct = 1.0f, st = 0.0f, cphi = 1.0f, sphi = 0.0f;
        if (r > 1.0e-7f)
        {
            ct = z / r;
            const float rxy = std::sqrt (rxy2);
            st = rxy / r;
            if (rxy > 1.0e-7f) { cphi = x / rxy; sphi = y / rxy; }
        }

        float cm[maxL + 1], sm[maxL + 1];   // cos(m phi), sin(m phi)
        cm[0] = 1.0f; sm[0] = 0.0f;
        for (int m = 1; m <= maxL; ++m)
        {
            cm[m] = cm[m - 1] * cphi - sm[m - 1] * sphi;
            sm[m] = sm[m - 1] * cphi + cm[m - 1] * sphi;
        }

        std::complex<float> sum (0.0f, 0.0f);
        for (int i = 0; i < count; ++i)
        {
            const auto& t = terms[i];
            const float rad = t.radial->at (r);
            if (rad == 0.0f)
                continue;
            const int am = t.m < 0 ? -t.m : t.m;
            const float base = rad * t.k * legendre (t.l, am, ct, st);
            std::complex<float> ang;
            if (t.realBasis)
                ang = { t.m == 0 ? base : (t.m > 0 ? base * cm[am] : base * sm[am]), 0.0f };
            else
                ang = { base * cm[am], t.m >= 0 ? base * sm[am] : -base * sm[am] };
            sum += t.c * ang;
        }
        return sum;
    }
}
