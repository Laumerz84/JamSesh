#pragma once

// Atom model for the orbital view (pure math, no JUCE): the sound is the outermost (valence)
// electron of element Z, moving in the field of the nucleus plus the other Z - 1 electrons,
// which sit in their ground-state subshells (aufbau / Madelung filling).
//
//   Energies: quantum-defect (Rydberg) formula  E = -Z_net^2 / (2 (n - delta_l)^2)  hartree,
//             Z_net = charge seen from far away (1 for a neutral atom), delta_l grows with the
//             number of core shells because low-l orbits dive into the core and feel more of the
//             nucleus. The defects are fitted to the alkali atoms (Li, Na, K, Rb, Cs).
//   Shapes:   hydrogen-like orbitals with the effective charge from Slater's screening rules,
//             R(r) = Zeff^(3/2) R_nl^H(Zeff r): same nodes and lobes as hydrogen, sized like the real atom.
//
// "Screening" scales every core effect: 1 = the real neutral atom, 0 = no core electrons at all
// (a bare nucleus of charge Z with one electron, i.e. a hydrogen-like ion), above 1 = exaggerated.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace atoms
{
    constexpr int maxZ = 86;
    constexpr int maxShell = 8;

    inline const char* symbol (int z)
    {
        static const char* s[] = { "?",
            "H", "He", "Li", "Be", "B", "C", "N", "O", "F", "Ne", "Na", "Mg", "Al", "Si", "P", "S", "Cl", "Ar",
            "K", "Ca", "Sc", "Ti", "V", "Cr", "Mn", "Fe", "Co", "Ni", "Cu", "Zn", "Ga", "Ge", "As", "Se", "Br", "Kr",
            "Rb", "Sr", "Y", "Zr", "Nb", "Mo", "Tc", "Ru", "Rh", "Pd", "Ag", "Cd", "In", "Sn", "Sb", "Te", "I", "Xe",
            "Cs", "Ba", "La", "Ce", "Pr", "Nd", "Pm", "Sm", "Eu", "Gd", "Tb", "Dy", "Ho", "Er", "Tm", "Yb", "Lu",
            "Hf", "Ta", "W", "Re", "Os", "Ir", "Pt", "Au", "Hg", "Tl", "Pb", "Bi", "Po", "At", "Rn" };
        return s[std::clamp (z, 0, maxZ)];
    }

    inline const char* name (int z)
    {
        static const char* s[] = { "?",
            "hydrogen", "helium", "lithium", "beryllium", "boron", "carbon", "nitrogen", "oxygen", "fluorine", "neon",
            "sodium", "magnesium", "aluminium", "silicon", "phosphorus", "sulfur", "chlorine", "argon",
            "potassium", "calcium", "scandium", "titanium", "vanadium", "chromium", "manganese", "iron", "cobalt", "nickel",
            "copper", "zinc", "gallium", "germanium", "arsenic", "selenium", "bromine", "krypton",
            "rubidium", "strontium", "yttrium", "zirconium", "niobium", "molybdenum", "technetium", "ruthenium", "rhodium",
            "palladium", "silver", "cadmium", "indium", "tin", "antimony", "tellurium", "iodine", "xenon",
            "caesium", "barium", "lanthanum", "cerium", "praseodymium", "neodymium", "promethium", "samarium", "europium",
            "gadolinium", "terbium", "dysprosium", "holmium", "erbium", "thulium", "ytterbium", "lutetium",
            "hafnium", "tantalum", "tungsten", "rhenium", "osmium", "iridium", "platinum", "gold", "mercury",
            "thallium", "lead", "bismuth", "polonium", "astatine", "radon" };
        return s[std::clamp (z, 0, maxZ)];
    }

    inline char letter (int l) { return "spdf"[std::clamp (l, 0, 3)]; }
    inline int capacity (int l) { return 2 * (2 * l + 1); }

    struct Config
    {
        int occ[maxShell + 1][4] = {};   // electrons in subshell (n, l)
    };

    // Ground-state filling of `electrons` electrons in Madelung (n + l, then n) order.
    // Ignores the handful of real exceptions (Cr, Cu, ...), which don't change the picture.
    inline Config fill (int electrons)
    {
        static const int order[][2] = { {1,0},{2,0},{2,1},{3,0},{3,1},{4,0},{3,2},{4,1},{5,0},{4,2},{5,1},{6,0},
                                        {4,3},{5,2},{6,1},{7,0},{5,3},{6,2},{7,1},{8,0} };
        Config c;
        for (auto& s : order)
        {
            if (electrons <= 0)
                break;
            const int put = std::min (electrons, capacity (s[1]));
            c.occ[s[0]][s[1]] = put;
            electrons -= put;
        }
        return c;
    }

    // Slater's grouping order: (1s)(2s,2p)(3s,3p)(3d)(4s,4p)(4d)(4f)(5s,5p)(5d)...
    inline int slaterGroup (int n, int l)
    {
        static const int groups[][2] = { {1,0},{2,0},{3,0},{3,2},{4,0},{4,2},{4,3},{5,0},{5,2},{5,3},{6,0},{6,2},{6,3},{7,0},{7,2},{8,0} };
        const int key = l <= 1 ? 0 : l;
        for (int i = 0; i < (int) (sizeof (groups) / sizeof (groups[0])); ++i)
            if (groups[i][0] == n && groups[i][1] == key)
                return i;
        return 99;
    }

    // Slater's rules: how much of the nuclear charge the core electrons hide from an electron in (n, l)
    inline double slaterShielding (const Config& core, int n, int l)
    {
        double s = 0.0;
        for (int cn = 1; cn <= maxShell; ++cn)
            for (int cl = 0; cl < 4; ++cl)
            {
                const int e = core.occ[cn][cl];
                if (e == 0)
                    continue;
                if (l <= 1)
                {
                    if (cn == n && cl <= 1)  s += (n == 1 ? 0.30 : 0.35) * e;   // same (ns, np) group
                    else if (cn == n - 1)    s += 0.85 * e;                     // the shell just inside
                    else if (cn <= n - 2)    s += 1.00 * e;                     // deeper shells
                }
                else
                {
                    if (cn == n && cl == l)                            s += 0.35 * e;
                    else if (slaterGroup (cn, cl) < slaterGroup (n, l)) s += 1.00 * e;
                }
            }
        return s;
    }

    struct Level
    {
        int n = 1, l = 0;
        double energy = -0.5;   // hartree
        double nStar = 1.0;     // effective principal quantum number n - delta_l
        double zShape = 1.0;    // Slater effective charge used for the orbital's size
        double defect = 0.0;    // delta_l
        bool groundSubshell = false;
    };

    struct Ladder
    {
        std::vector<Level> levels;   // lowest energy first
        int coreShells = 0;
        double zNet = 1.0;
        double defects[4] = {};
    };

    // The valence electron's ladder of available subshells (not filled by the core), lowest energy first
    inline Ladder valenceLadder (int z, double screening, int count)
    {
        z = std::clamp (z, 1, maxZ);
        screening = std::max (0.0, screening);
        const Config core = fill (z - 1);

        // closed core shells (full 1s, full np) set how deep low-l orbits can penetrate
        Ladder lad;
        for (int n = 1; n <= maxShell; ++n)
            if (n == 1 ? core.occ[1][0] == 2 : core.occ[n][1] == 6)
                ++lad.coreShells;

        // quantum defects fitted to the alkalis: s ~ 0.9 k - 0.45 for k core shells (Li .40, Na 1.37, K 2.18,
        // Rb 3.13, Cs 4.05), p ~ s - 0.48, d ~ s - 1.9, f ~ s - 4.0 (never negative)
        const double ds = lad.coreShells == 0 ? 0.0 : 0.9 * lad.coreShells - 0.45;
        lad.defects[0] = ds;
        lad.defects[1] = std::max (0.0, ds - 0.48);
        // small floors for d and f keep them apart like in real atoms (Na: d 0.015, f 0.002), so every
        // subshell is its own rung instead of 4d/4f collapsing into one
        lad.defects[2] = std::max (0.01 * lad.coreShells, ds - 1.9);
        lad.defects[3] = std::max (0.002 * lad.coreShells, ds - 4.0);
        lad.zNet = z - std::min (screening, 1.0) * (z - 1);

        const Config ground = fill (z);
        for (int n = 1; n <= maxShell; ++n)
            for (int l = 0; l < std::min (n, 4); ++l)
            {
                if (core.occ[n][l] >= capacity (l))
                    continue;   // already full of core electrons: the Pauli principle forbids it
                Level v;
                v.n = n;
                v.l = l;
                v.defect = screening * lad.defects[l];
                v.nStar = std::max (l + 0.75, n - v.defect);
                v.energy = -0.5 * lad.zNet * lad.zNet / (v.nStar * v.nStar);
                v.zShape = std::max (1.0, z - screening * slaterShielding (core, n, l));
                v.groundSubshell = ground.occ[n][l] > core.occ[n][l];
                lad.levels.push_back (v);
            }

        std::sort (lad.levels.begin(), lad.levels.end(), [] (const Level& a, const Level& b) { return a.energy < b.energy; });

        // The one-electron defect model is weakest for open-shell atoms (C, Fe...), where it can rank an
        // excited subshell below the real ground one. The electron lives in its ground subshell, so that
        // stays level 0 and anything the model put below it is lifted to just above it.
        auto g = std::find_if (lad.levels.begin(), lad.levels.end(), [] (const Level& v) { return v.groundSubshell; });
        if (g != lad.levels.end() && g != lad.levels.begin())
        {
            const Level groundLevel = *g;
            lad.levels.erase (g);
            int lifted = 0;
            for (auto& v : lad.levels)
                if (v.energy <= groundLevel.energy)
                    v.energy = groundLevel.energy * (1.0 - 0.02 * ++lifted);   // small distinct steps above the ground
            lad.levels.insert (lad.levels.begin(), groundLevel);
            std::stable_sort (lad.levels.begin() + 1, lad.levels.end(), [] (const Level& a, const Level& b) { return a.energy < b.energy; });
        }

        if ((int) lad.levels.size() > count)
            lad.levels.resize ((size_t) std::max (1, count));
        return lad;
    }

    inline std::string label (int n, int l) { return std::to_string (n) + letter (l); }

    // Radius holding ~99% of a hydrogen-like orbital of charge zShape
    inline double extent (int n, double zShape) { return (2.5 * n * n + 4.0) / std::max (0.5, zShape); }
}
