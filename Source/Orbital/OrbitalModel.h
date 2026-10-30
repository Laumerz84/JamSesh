#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <complex>
#include <vector>
#include "../UI/Params.h"
#include "../UI/Help.h"
#include "../UI/ViewFeed.h"

// Physics + sound-to-quantum "bridge": turns the current audio into a superposition of bound
// states and evaluates the wavefunction psi(x, y, z) at the current display time.
//
// Owner: Agent C1 (implements OrbitalModel.cpp; may add private members here). Public API is a contract.
// Parameter ids must start with "orb" and are the BRIDGE parameters only (potential, level
// mapping, angular source, hbar, quantize, fundamental, max terms, dial l/m, max n...).
// Rendering parameters belong to OrbitalView.
//
// Units: hydrogen in Bohr radii (a0 = 1), energies in hartree; oscillator in its natural length
// sqrt(hbar/m omega) = 1, energies in units of hbar*omega.
namespace orbital
{
    enum Potential     { harmonicOscillator = 0, hydrogen = 1, element = 2 };   // element = valence electron of atom Z (Atoms.h)
    enum LevelMapping  { physicalLinear = 0, rydbergHarmonic = 1, emissionLines = 2, equalTemperament = 3 };
    enum AngularSource { angularDials = 0, angularStereo = 1, angularTimbre = 2, angularSpread = 3 };

    struct ModeTerm
    {
        int n = 1, l = 0, m = 0;        // hydrogen: principal n. oscillator: shell N (= 2*n_r + l) stored in n
        std::complex<double> c;         // amplitude; all terms normalised so sum |c|^2 = 1
        double energy = 0.0;            // model units (see above)
        double displayOmega = 0.0;      // phase speed on screen in rad per second of display time (after hbar)
        double sourceHz = 0.0;          // audio frequency that produced this term (0 if none)
        double detuneCents = 0.0;       // how far the source was from the exact level before quantize
        juce::String reason;            // short: why this term exists, e.g. "440 Hz = 2 x f0 -> N=1"
    };

    class OrbitalModel
    {
    public:
        static void addParameters (ParamList&);
        static HelpList helpEntries();

        explicit OrbitalModel (juce::AudioProcessorValueTreeState&);   // the editor's: this plugin's settings
        explicit OrbitalModel (const ViewParams&);                     // any settings (Jam Sesh tiles); must outlive it
        ~OrbitalModel();

        // Message thread, ~30-60 Hz. Analyses the latest audio window (stereo) and advances the
        // superposition to `displayTimeSeconds` (a clock the view controls; it can pause).
        void update (const float* left, const float* right, int numSamples,
                     double sampleRate, double displayTimeSeconds);

        const std::vector<ModeTerm>& terms() const noexcept;

        // Wavefunction / density at the time given to the last update(). Must be fast: the view
        // calls it up to ~250k times per frame. Thread-safe for concurrent const calls.
        std::complex<float> psi (float x, float y, float z) const noexcept;
        float density (float x, float y, float z) const noexcept { return std::norm (psi (x, y, z)); }

        float extent() const noexcept;            // radius holding ~99% of the probability (for framing)
        float peakDensityEstimate() const noexcept;

        juce::String describeMapping() const;     // live multi-line explanation for the Inspector
        juce::StringArray overlayLines() const;   // short lines for the 3D overlay, one per term

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
