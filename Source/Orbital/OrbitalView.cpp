#include "OrbitalView.h"
#include "Atoms.h"
#include <array>
#include <random>

namespace
{
    enum RenderMode { slices = 0, voxels = 1, pointCloud = 2 };
    enum ColourMode { heat = 0, phaseHue = 1 };

    constexpr int windowSamples = 8192;
    constexpr int tableRows = 8;

    float readParam (juce::AudioProcessorValueTreeState& s, const char* id, float fallback)
    {
        if (auto* p = s.getRawParameterValue (id))
            return p->load();
        return fallback;
    }

    juce::String pctText (float v, int) { return juce::String (v, v < 10.0f ? 1 : 0) + " %"; }

    juce::String thousands (int n)
    {
        if (n >= 1000)
            return juce::String (n / 1000.0, n >= 100000 ? 0 : 1) + "k";
        return juce::String (n);
    }

    // Heat map: dark blue -> violet -> pink -> amber -> near white
    void heatColour (float v, float& r, float& g, float& b)
    {
        struct Stop { float t, r, g, b; };
        static const Stop stops[] = { { 0.00f, 0.05f, 0.10f, 0.35f }, { 0.35f, 0.66f, 0.55f, 1.00f },
                                      { 0.65f, 1.00f, 0.36f, 0.54f }, { 0.85f, 1.00f, 0.70f, 0.28f },
                                      { 1.00f, 1.00f, 0.95f, 0.85f } };
        v = juce::jlimit (0.0f, 1.0f, v);
        for (int i = 1; i < 5; ++i)
        {
            if (v <= stops[i].t)
            {
                const auto& a = stops[i - 1];
                const auto& z = stops[i];
                const float t = (v - a.t) / (z.t - a.t);
                r = a.r + (z.r - a.r) * t;
                g = a.g + (z.g - a.g) * t;
                b = a.b + (z.b - a.b) * t;
                return;
            }
        }
        r = stops[4].r; g = stops[4].g; b = stops[4].b;
    }

    // v = exposure-scaled density / peak (0..1); psi is used by the phase colour map
    Vertex3 colourVertex (float x, float y, float z, float v, std::complex<float> psi, int colourMode, float alphaScale)
    {
        Vertex3 vx { x, y, z, 0.0f, 0.0f, 0.0f, 0.0f };
        if (colourMode == phaseHue)
        {
            const float hue = (std::arg (psi) + juce::MathConstants<float>::pi) / juce::MathConstants<float>::twoPi;
            const auto c = juce::Colour::fromHSV (hue, 0.85f, 1.0f, 1.0f);
            vx.r = c.getFloatRed(); vx.g = c.getFloatGreen(); vx.b = c.getFloatBlue();
        }
        else
        {
            heatColour (v, vx.r, vx.g, vx.b);
        }
        vx.a = juce::jlimit (0.0f, 1.0f, v * alphaScale);
        return vx;
    }
}

//==============================================================================
void OrbitalView::addParameters (ParamList& p)
{
    using namespace juce;
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "orbRMode", 1 }, "Orbital Render",
        StringArray { "Slices", "Voxel grid", "Point cloud" }, voxels));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "orbRRes", 1 }, "Voxel Resolution", 16, 64, 32,
        AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return String (v) + "^3"; })));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "orbRPoints", 1 }, "Cloud Points", 5000, 200000, 40000,
        AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return thousands (v); })));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "orbRBudget", 1 }, "Render Budget", 20000, 400000, 120000,
        AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return thousands (v) + "/frame"; })));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "orbRColour", 1 }, "Orbital Colour",
        StringArray { "Density (heat)", "Phase (hue)" }, heat));

    NormalisableRange<float> exposure (0.1f, 10.0f);
    exposure.setSkewForCentre (1.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "orbRExposure", 1 }, "Orbital Exposure", exposure, 1.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return "x" + String (v, 2); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "orbRContrast", 1 }, "Orbital Contrast",
        NormalisableRange<float> (0.15f, 1.0f, 0.01f), 0.6f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return "gamma " + String (v, 2); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "orbRThreshold", 1 }, "Orbital Threshold",
        NormalisableRange<float> (0.0f, 50.0f, 0.1f), 2.0f, AudioParameterFloatAttributes().withStringFromValueFunction (pctText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "orbRSlice", 1 }, "Slice Offset",
        NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 2); })));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "orbRFreeze", 1 }, "Orbital Pause", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "orbRSpin", 1 }, "Orbital Spin",
        NormalisableRange<float> (0.0f, 90.0f, 0.5f), 12.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v <= 0.0f ? String ("Off") : String (v, 1) + " deg/s"; })));
}

HelpList OrbitalView::helpEntries()
{
    return {
        { "orbRMode", "Render mode",
          "How the probability cloud |psi|^2 is drawn. Pick by how much your computer can handle.",
          "Slices (cheap): three flat cross-sections (xy, xz, yz) through the Slice offset; each square shows |psi|^2 at that spot. "
          "Voxel grid (balanced, default): space is split into Res^3 cubes over +/- extent; each cube is one soft blob showing |psi|^2 "
          "at its centre, recomputed section by section (a few z-slabs per frame, within the Budget). "
          "Point cloud (heavy): Points dots scattered so that their density follows |psi|^2 (Metropolis sampling), moved a few steps per frame.",
          "Start with Voxel grid. Use Slices to read nodal planes exactly; Point cloud for the prettiest picture." },
        { "orbRRes", "Voxel resolution",
          "Number of cubes along each axis in Voxel grid mode.",
          "The cube [-extent, +extent]^3 is divided into Res x Res x Res cells (16^3 = 4k cells, 32^3 = 33k, 64^3 = 262k). "
          "Each cell is sampled once at its centre; a full refresh costs Res^3 density evaluations, spread over several frames by the Budget.",
          "Higher Res shows finer shells but a full refresh takes more frames (see 'sweep n frames' in the overlay)." },
        { "orbRPoints", "Cloud points",
          "Number of sample dots in Point cloud mode.",
          "Each dot is a random walker that moves to a nearby spot with probability min(1, |psi(new)|^2 / |psi(old)|^2) "
          "(Metropolis), so dots crowd together where the cloud is dense. The dots' opacity scales with Exposure.",
          "More points = smoother cloud, but each dot gets fewer steps per frame (Budget / 2 / Points), so it reacts more slowly." },
        { "orbRBudget", "Render budget",
          "Maximum density evaluations per frame: the cost cap that keeps the view smooth.",
          "Slices: each plane gets sqrt(Budget / 3) cells per side (24..96). Voxel grid: floor(Budget / Res^2) z-slabs are recomputed "
          "per frame, so a full sweep takes Res / slabs frames. Point cloud: Budget / 2 Metropolis steps per frame (each step evaluates "
          "the current and the proposed spot).",
          "Lower it if FL Studio stutters with the plugin window open; raise it for faster refresh." },
        { "orbRColour", "Colour map",
          "What the colours mean.",
          "Density (heat): colour = |psi|^2 / peak through dark blue -> violet -> pink -> amber -> white. "
          "Phase (hue): hue = phase angle arg(psi) of the wavefunction (once round the colour wheel per 2 pi), brightness = density. "
          "In phase mode you can see the sign flip across nodes and complex m states swirl as they rotate.",
          "Use Phase to see why two lobes cancel or reinforce when states mix." },
        { "orbRExposure", "Exposure",
          "Overall brightness of the cloud.",
          "Each sample's brightness = clamp(Exposure x |psi|^2 / peak, 0, 1). The peak follows what is currently drawn (jumps up "
          "instantly, eases down slowly). In Point cloud mode Exposure scales the dots' opacity.",
          "Turn it up to reveal faint outer shells; down if the core blows out to white." },
        { "orbRContrast", "Contrast (gamma)",
          "Compresses the huge brightness range of the cloud so faint outer shells stay visible next to a bright core.",
          "Every sample's relative density is reshaped before Threshold and Exposure: shown = (|psi|^2 / peak)^gamma. "
          "gamma 1 = linear (true proportions); gamma 0.6 (default) lifts faint regions; lower values lift faint regions further. "
          "Threshold is compared against the reshaped value, so lowering gamma also reveals more of the faint halo.",
          "Mix a high level (e.g. n = 5) with a low one and lower gamma to see both the tiny inner state and the big outer shell." },
        { "orbRThreshold", "Threshold",
          "Hide samples weaker than this fraction of the peak density.",
          "Cells / squares / dots with |psi|^2 / peak below Threshold % are not drawn at all (which also saves drawing time).",
          "1-5 % keeps the shape clean; 0 % shows the full faint halo." },
        { "orbRSlice", "Slice offset",
          "Where the three cross-section planes cut through the cloud (Slices mode).",
          "Each plane sits at Offset x extent along its own axis: the xy plane at z = Offset x extent, xz at y = Offset x extent, "
          "yz at x = Offset x extent. 0 = through the centre (the nucleus).",
          "Sweep it to find hidden inner shells and nodal surfaces." },
        { "orbRFreeze", "Pause clock",
          "Stops the display clock: the cloud stops evolving in time but still follows the sound.",
          "Each term's phase advances as exp(-i x displayOmega x t); Pause holds t. Changing the sound still changes which states are "
          "present. The scope's global Freeze also stops the audio window.",
          "Use it to inspect one instant of a sloshing mix." },
        { "orbRSpin", "Auto-spin",
          "Slowly rotates the camera around the cloud.",
          "Camera yaw advances by Spin degrees per second (paused while you drag). Purely visual.",
          "Drag to rotate, scroll to zoom, double-click to reset the camera." },
        { "orb.terms", "Live mapping table",
          "The states currently in the superposition and where they came from.",
          "One row per term: quantum numbers n l m, amplitude |c| (bar; all |c|^2 add up to 1), the audio frequency that produced it, "
          "how far that frequency was from the exact level in cents before Quantize, and the rule that created it.",
          "A pure tone should show one row; a chord several; noise many weak ones." },
    };
}

//==============================================================================
struct OrbitalView::Impl
{
    Impl (const ViewFeed& f, const ViewParams& p, orbital::OrbitalModel& m,
          std::unique_ptr<ViewFeed> ownedFeed = {}, std::unique_ptr<ViewParams> ownedParams = {})
        : ownFeed (std::move (ownedFeed)), ownParams (std::move (ownedParams)), feed (f), params (p), model (m)
    {
        left.assign ((size_t) windowSamples, 0.0f);
        right.assign ((size_t) windowSamples, 0.0f);
    }

    float param (const char* id, float fallback) const { return params.get (id, fallback); }

    struct Walker
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float dens = 0.0f;
        std::complex<float> psi;
    };

    void buildSlices (std::vector<DrawBatch>&, float extent, int budget, float exposure, float threshold, int colourMode);
    void buildVoxels (std::vector<DrawBatch>&, float extent, int budget, float exposure, float threshold, int colourMode);
    void buildPoints (std::vector<DrawBatch>&, float extent, int budget, float exposure, float threshold, int colourMode);
    float trackPeak (float peakNow);

    std::unique_ptr<ViewFeed> ownFeed;
    std::unique_ptr<ViewParams> ownParams;
    const ViewFeed& feed;
    const ViewParams& params;
    orbital::OrbitalModel& model;

    std::vector<float> left, right;
    double displayTime = 0.0, lastWall = 0.0;
    float displayPeak = 0.0f;
    float gamma = 0.6f;   // contrast: shown = (density / peak)^gamma

    // voxel cache, refreshed a few z-slabs per frame
    int gridRes = 0, nextSlab = 0;
    float gridExtent = 0.0f;
    std::vector<std::complex<float>> grid;

    // point cloud walkers
    std::vector<Walker> walkers;
    size_t cursor = 0;
    std::mt19937 rng { 12345u };

    juce::String statsLine;
    int lastMode = voxels;
};

float OrbitalView::Impl::trackPeak (float peakNow)
{
    // jump up instantly, ease down so the brightness doesn't pump
    if (! std::isfinite (peakNow) || peakNow <= 0.0f)
        return displayPeak > 0.0f ? displayPeak : 1.0f;
    displayPeak = peakNow > displayPeak ? peakNow : displayPeak + (peakNow - displayPeak) * 0.08f;
    return juce::jmax (displayPeak, 1.0e-30f);
}

void OrbitalView::Impl::buildSlices (std::vector<DrawBatch>& out, float extent, int budget, float exposure,
                                     float threshold, int colourMode)
{
    const int side = juce::jlimit (24, 96, (int) std::sqrt ((double) budget / 3.0));
    const float offset = param ("orbRSlice", 0.0f) * extent;
    const float cell = 2.0f * extent / (float) side;

    // plane 0 = xy (at z = offset), 1 = xz (at y = offset), 2 = yz (at x = offset)
    auto toWorld = [offset] (int plane, float u, float v, float& x, float& y, float& z)
    {
        x = plane == 2 ? offset : u;
        y = plane == 0 ? v : (plane == 1 ? offset : u);
        z = plane == 0 ? offset : v;
    };

    std::vector<std::complex<float>> samples ((size_t) side * (size_t) side * 3);
    float peakNow = 0.0f;
    for (int plane = 0; plane < 3; ++plane)
        for (int j = 0; j < side; ++j)
            for (int i = 0; i < side; ++i)
            {
                float x, y, z;
                toWorld (plane, -extent + ((float) i + 0.5f) * cell, -extent + ((float) j + 0.5f) * cell, x, y, z);
                const auto psi = model.psi (x, y, z);
                samples[((size_t) plane * (size_t) side + (size_t) j) * (size_t) side + (size_t) i] = psi;
                peakNow = juce::jmax (peakNow, std::norm (psi));
            }

    juce::ignoreUnused (peakNow);
    const float peak = trackPeak (model.peakDensityEstimate());   // absolute: a quiet or absent electron looks faint or empty
    DrawBatch quads;
    quads.type = DrawBatch::triangles;
    quads.additive = true;

    int drawn = 0;
    for (int plane = 0; plane < 3; ++plane)
        for (int j = 0; j < side; ++j)
            for (int i = 0; i < side; ++i)
            {
                const auto psi = samples[((size_t) plane * (size_t) side + (size_t) j) * (size_t) side + (size_t) i];
                const float rel = std::pow (std::norm (psi) / peak, gamma);
                if (rel < threshold)
                    continue;
                const float u0 = -extent + (float) i * cell, v0 = -extent + (float) j * cell;
                const float level = juce::jmin (1.0f, rel * exposure);
                auto corner = [&] (float u, float v)
                {
                    float x, y, z;
                    toWorld (plane, u, v, x, y, z);
                    return colourVertex (x, y, z, level, psi, colourMode, 0.9f);
                };
                const auto a = corner (u0, v0), b = corner (u0 + cell, v0);
                const auto c = corner (u0 + cell, v0 + cell), d = corner (u0, v0 + cell);
                quads.vertices.insert (quads.vertices.end(), { a, b, c, a, c, d });
                ++drawn;
            }
    out.push_back (std::move (quads));

    statsLine = "Slices " + juce::String (side) + " x " + juce::String (side) + " x 3 planes  |  "
              + thousands (drawn) + " squares drawn  |  " + thousands (side * side * 3) + " evals/frame";
}

void OrbitalView::Impl::buildVoxels (std::vector<DrawBatch>& out, float extent, int budget, float exposure,
                                     float threshold, int colourMode)
{
    const int res = juce::jlimit (16, 64, (int) param ("orbRRes", 32.0f));
    if (res != gridRes)
    {
        gridRes = res;
        grid.assign ((size_t) res * (size_t) res * (size_t) res, {});
        nextSlab = 0;
    }
    // re-frame only on big extent changes so the cached slabs stay meaningful
    if (gridExtent <= 0.0f || std::abs (extent / gridExtent - 1.0f) > 0.15f)
        gridExtent = extent;

    const float cell = 2.0f * gridExtent / (float) res;
    const int slabsPerFrame = juce::jlimit (1, res, budget / (res * res));
    auto index = [res] (int i, int j, int k) { return ((size_t) k * (size_t) res + (size_t) j) * (size_t) res + (size_t) i; };
    auto centre = [this, cell] (int i) { return -gridExtent + ((float) i + 0.5f) * cell; };

    // section by section: recompute a few z-slabs, keep the rest from the cache
    for (int s = 0; s < slabsPerFrame; ++s)
    {
        const int k = nextSlab;
        const float z = centre (k);
        for (int j = 0; j < res; ++j)
            for (int i = 0; i < res; ++i)
                grid[index (i, j, k)] = model.psi (centre (i), centre (j), z);
        nextSlab = (nextSlab + 1) % res;
    }

    const float peak = trackPeak (model.peakDensityEstimate());   // absolute, see buildSlices

    DrawBatch blobs;
    blobs.type = DrawBatch::points;
    blobs.additive = true;
    blobs.pointWorldSize = cell * 1.3f;

    for (int k = 0; k < res; ++k)
        for (int j = 0; j < res; ++j)
            for (int i = 0; i < res; ++i)
            {
                const auto psi = grid[index (i, j, k)];
                const float rel = std::pow (std::norm (psi) / peak, gamma);
                if (rel < threshold)
                    continue;
                // fixed per-cell jitter breaks up the grid's moire stripes without making the shape shimmer
                auto jitter = [cell] (int a, int b, int c, int axis)
                {
                    unsigned h = (unsigned) (a * 73856093) ^ (unsigned) (b * 19349663) ^ (unsigned) (c * 83492791) ^ (unsigned) (axis * 2654435761u);
                    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
                    return ((float) (h & 0xffff) / 65535.0f - 0.5f) * 0.7f * cell;
                };
                blobs.vertices.push_back (colourVertex (centre (i) + jitter (i, j, k, 0), centre (j) + jitter (i, j, k, 1),
                                                        centre (k) + jitter (i, j, k, 2),
                                                        juce::jmin (1.0f, rel * exposure), psi, colourMode, 0.55f));
            }

    const int drawn = (int) blobs.vertices.size();
    out.push_back (std::move (blobs));

    const int sweepFrames = (res + slabsPerFrame - 1) / slabsPerFrame;
    statsLine = "Voxel grid " + juce::String (res) + "^3  |  " + thousands (drawn) + " cells drawn  |  "
              + thousands (slabsPerFrame * res * res) + " evals/frame  |  refresh "
              + juce::String ((int) std::round (100.0 * slabsPerFrame / res)) + "% / sweep " + juce::String (sweepFrames) + " frames";
}

void OrbitalView::Impl::buildPoints (std::vector<DrawBatch>& out, float extent, int budget, float exposure,
                                     float threshold, int colourMode)
{
    const int count = juce::jlimit (5000, 200000, (int) param ("orbRPoints", 40000.0f));
    std::uniform_real_distribution<float> uni (-1.0f, 1.0f);
    std::uniform_real_distribution<float> unit (0.0f, 1.0f);
    std::normal_distribution<float> gauss (0.0f, 1.0f);
    auto randomWalker = [&] { return Walker { uni (rng) * extent, uni (rng) * extent, uni (rng) * extent, 0.0f, {} }; };

    if ((int) walkers.size() != count)
    {
        const size_t old = walkers.size();
        walkers.resize ((size_t) count);
        for (size_t i = old; i < walkers.size(); ++i)
            walkers[i] = randomWalker();
        cursor = 0;
    }

    // Metropolis: each step re-evaluates the walker's current spot (the cloud evolves in time)
    // and a proposal, so the dots follow the live |psi|^2
    const float stepSize = 0.12f * extent;
    const int steps = juce::jmax (1, budget / 2);
    for (int s = 0; s < steps; ++s)
    {
        auto& w = walkers[cursor];
        cursor = (cursor + 1) % walkers.size();

        if (std::abs (w.x) > 1.5f * extent || std::abs (w.y) > 1.5f * extent || std::abs (w.z) > 1.5f * extent)
            w = randomWalker();

        w.psi = model.psi (w.x, w.y, w.z);
        w.dens = std::norm (w.psi);

        const float nx = w.x + gauss (rng) * stepSize;
        const float ny = w.y + gauss (rng) * stepSize;
        const float nz = w.z + gauss (rng) * stepSize;
        const auto np = model.psi (nx, ny, nz);
        const float nd = std::norm (np);
        if (nd >= w.dens || unit (rng) * w.dens < nd)
            w = Walker { nx, ny, nz, nd, np };
    }

    const float peak = trackPeak (model.peakDensityEstimate());

    DrawBatch dots;
    dots.type = DrawBatch::points;
    dots.additive = true;
    dots.pointWorldSize = extent * 0.022f;
    dots.vertices.reserve (walkers.size());
    const float alpha = juce::jlimit (0.05f, 1.0f, 0.35f * exposure);
    for (const auto& w : walkers)
    {
        const float rel = std::pow (w.dens / peak, gamma);
        if (rel < threshold)
            continue;
        auto v = colourVertex (w.x, w.y, w.z, juce::jlimit (0.25f, 1.0f, rel * exposure), w.psi, colourMode, 1.0f);
        v.a = alpha;
        dots.vertices.push_back (v);
    }
    const int drawn = (int) dots.vertices.size();
    out.push_back (std::move (dots));

    statsLine = "Point cloud " + thousands (count) + " walkers  |  " + thousands (drawn) + " dots drawn  |  "
              + thousands (steps * 2) + " evals/frame  |  " + juce::String ((double) steps / count, 2) + " steps/dot/frame";
}

//==============================================================================
OrbitalView::OrbitalView (ClodScopeProcessor& p, orbital::OrbitalModel& m)
{
    auto f = std::make_unique<ProcessorFeed> (p);
    auto ps = std::make_unique<ApvtsParams> (p.apvts);
    const ViewFeed& feedRef = *f;
    const ViewParams& paramsRef = *ps;
    impl = std::make_unique<Impl> (feedRef, paramsRef, m, std::move (f), std::move (ps));
}

OrbitalView::OrbitalView (const ViewFeed& f, const ViewParams& p, orbital::OrbitalModel& m)
    : impl (std::make_unique<Impl> (f, p, m)) {}

OrbitalView::~OrbitalView() = default;

void OrbitalView::produce (Scene3D& scene)
{
    auto& d = *impl;

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double dt = d.lastWall > 0.0 ? juce::jlimit (0.0, 0.1, now - d.lastWall) : 0.0;
    d.lastWall = now;

    const bool globalFreeze = d.param ("freeze", 0.0f) > 0.5f;
    const bool paused = globalFreeze || d.param ("orbRFreeze", 0.0f) > 0.5f;
    if (! paused)
        d.displayTime += dt;

    if (! globalFreeze)
    {
        d.feed.copyLatestPadded ((int) d.param ("source", 2.0f) != 0, d.left.data(), d.right.data(), windowSamples);
    }
    d.model.update (d.left.data(), d.right.data(), windowSamples, d.feed.sampleRate(), d.displayTime);

    float extent = d.model.extent();
    if (! std::isfinite (extent) || extent <= 0.0f)
        extent = 10.0f;

    const int mode = juce::jlimit (0, 2, (int) d.param ("orbRMode", (float) voxels));
    const int budget = juce::jlimit (20000, 400000, (int) d.param ("orbRBudget", 120000.0f));
    const float exposure = d.param ("orbRExposure", 1.0f);
    d.gamma = juce::jlimit (0.15f, 1.0f, d.param ("orbRContrast", 0.6f));
    const float threshold = d.param ("orbRThreshold", 2.0f) * 0.01f;
    const int colourMode = (int) d.param ("orbRColour", 0.0f);

    if (mode != d.lastMode)
    {
        d.lastMode = mode;
        d.displayPeak = 0.0f;
    }

    std::vector<DrawBatch> batches;
    switch (mode)
    {
        case slices:     d.buildSlices (batches, extent, budget, exposure, threshold, colourMode); break;
        case pointCloud: d.buildPoints (batches, extent, budget, exposure, threshold, colourMode); break;
        default:         d.buildVoxels (batches, extent, budget, exposure, threshold, colourMode); break;
    }
    scene.submitFrame (std::move (batches));
    scene.setWorldRadius (mode == voxels && d.gridExtent > 0.0f ? d.gridExtent : extent);

    const bool coulomb = (int) d.param ("orbPotential", 2.0f) != orbital::harmonicOscillator;
    const juce::String unit = coulomb ? " (a0)" : " (osc. length)";
    scene.setAxes (true, "x" + unit, "y" + unit, "z" + unit);
    scene.setAutoSpin (d.param ("orbRSpin", 12.0f));

    juce::StringArray bottom;
    bottom.add (d.statsLine);
    bottom.add (colourMode == phaseHue ? "Colour: hue = phase of psi, brightness = |psi|^2"
                                       : "Colour: |psi|^2 / peak   dark blue -> violet -> amber -> white");
    if (paused)
        bottom.add (globalFreeze ? "FROZEN (scope Freeze)" : "PAUSED (display clock held)");
    scene.setOverlay (d.model.overlayLines(), bottom);
}

juce::String OrbitalView::describeMapping() const
{
    const auto& d = *impl;
    const int mode = juce::jlimit (0, 2, (int) d.param ("orbRMode", (float) voxels));
    const int colourMode = (int) d.param ("orbRColour", 0.0f);

    juce::String s;
    s << "RENDER\n";
    if (mode == slices)
        s << "Slices: three planes through offset " << juce::String (d.param ("orbRSlice", 0.0f), 2)
          << " x extent. Each square shows |psi|^2 at that point of the plane.\n";
    else if (mode == voxels)
        s << "Voxel grid: space split into " << (int) d.param ("orbRRes", 32.0f) << "^3 cubes over +/- extent. "
          << "Each blob shows |psi|^2 at its cube's centre; a few z-slabs are recomputed per frame (section by section).\n";
    else
        s << "Point cloud: " << (int) d.param ("orbRPoints", 40000.0f)
          << " dots random-walk so their density follows |psi|^2 (Metropolis).\n";

    s << (colourMode == phaseHue ? "Colour: hue = phase arg(psi), brightness = |psi|^2.\n"
                                 : "Colour: heat map of |psi|^2 / peak (blue -> violet -> amber -> white).\n");
    s << "Brightness = Exposure x (|psi|^2 / peak)^gamma (Exposure x" << juce::String (d.param ("orbRExposure", 1.0f), 2)
      << ", gamma " << juce::String (d.param ("orbRContrast", 0.6f), 2)
      << "); samples below " << juce::String (d.param ("orbRThreshold", 2.0f), 1) << "% of peak are hidden.\n";
    if (d.statsLine.isNotEmpty())
        s << d.statsLine << "\n";
    s << "\n" << d.model.describeMapping();
    return s;
}

//==============================================================================
namespace
{
    juce::String potentialText (int i)
    {
        switch (i)
        {
            case 1:  return "Textbook hydrogen: E_n = -1/(2 n^2) hartree. Levels bunch together going up; every shape l < n is allowed.";
            case 2:  return "The sound is the outer electron of element Z. Each level is a real free subshell (3s, 3p, 4s, 3d...) with its own fixed shape.";
            default: return "Evenly spaced levels E = (N + 3/2) hbar w, so a note's harmonics land exactly on shells N = 0, 1, 2...";
        }
    }

    juce::String atomText (int z, float screening)
    {
        const auto lad = atoms::valenceLadder (z, screening, 7);
        juce::String t;
        t << z << " " << atoms::symbol (z) << " - " << atoms::name (z) << "\n";
        t << "Electron's ladder: ";
        for (size_t i = 0; i < lad.levels.size(); ++i)
            t << (i > 0 ? "  " : "") << atoms::label (lad.levels[i].n, lad.levels[i].l);
        t << "\n" << (z - 1) << " other electrons fill the core";
        if (screening < 0.995f || screening > 1.005f)
            t << " (screening " << (int) std::round (screening * 100.0f) << "%)";
        return t;
    }

    juce::String mappingText (int i)
    {
        switch (i)
        {
            case 1:  return "f0, 2f0, 3f0... sit exactly on n = 1, 2, 3. The cloud still moves with the true hydrogen energies.";
            case 2:  return "Each tone picks the transition n1 -> n2 whose spectral line matches it; the cloud is that two-state mix sloshing at the tone's frequency.";
            case 3:  return "One level per semitone above f0: play a scale to climb the ladder and see each subshell's shape.";
            default: return "Tone frequency maps linearly to energy (scaled by f0), so beats in the cloud match the beats you hear.";
        }
    }

    juce::String angularText (int i)
    {
        switch (i)
        {
            case 1:  return "Left/right phase difference sets m (orientation); stereo width sets l (shape).";
            case 2:  return "Brightness of the sound sets l (shape); harmonic number sets m (orientation).";
            case 3:  return "Energy shared evenly over every shape of each level: round, symmetric shells.";
            default: return "You choose l (shape) and m (orientation) with the l and m dials below.";
        }
    }

    juce::String basisText (int i)
    {
        return i == 1 ? "cos/sin combinations of +m and -m: the familiar p and d lobes, standing still."
                      : "exp(i m phi) states: m != 0 gives rotating rings (current loops); the phase colours swirl.";
    }

    // Live table of the terms in the superposition
    class TermsTable : public juce::Component
    {
    public:
        void setTerms (std::vector<orbital::ModeTerm> t, juce::String f0)
        {
            terms = std::move (t);
            f0Line = std::move (f0);
            repaint();
        }

        static int heightFor() { return 20 + tableRows * 15 + 20; }

        void paint (juce::Graphics& g) override
        {
            using namespace Palette;
            auto r = getLocalBounds();
            g.setColour (viewBg);
            g.fillRoundedRectangle (r.toFloat(), 5.0f);
            r.reduce (6, 2);

            const int cQ = 64, cBar = 64, cHz = 62, cCents = 48;
            auto take = [] (juce::Rectangle<int>& row, int width) { return row.removeFromLeft (width); };

            auto header = r.removeFromTop (20);
            g.setFont (Theme::font (10.0f, true));
            g.setColour (dim);
            g.drawText ("n  l  m", take (header, cQ), juce::Justification::centredLeft);
            g.drawText ("amp", take (header, cBar), juce::Justification::centredLeft);
            g.drawText ("from", take (header, cHz), juce::Justification::centredLeft);
            g.drawText ("detune", take (header, cCents), juce::Justification::centredLeft);
            g.drawText ("why", header, juce::Justification::centredLeft);

            g.setFont (Theme::font (10.5f));
            auto rows = r.removeFromTop (tableRows * 15);
            if (terms.empty())
            {
                g.setColour (dim);
                g.drawText ("Silence: no electron. The sound is the electron - play something.", rows, juce::Justification::centred);
            }
            else
            {
                double total = 0.0;
                for (const auto& t : terms)
                    total += std::norm (t.c);
                const int shown = juce::jmin ((int) terms.size(), tableRows);
                for (int i = 0; i < shown; ++i)
                {
                    auto row = rows.removeFromTop (15);
                    const auto& t = terms[(size_t) i];
                    g.setColour (text);
                    g.drawText (juce::String (t.n) + "  " + juce::String (t.l) + "  " + juce::String (t.m),
                                take (row, cQ), juce::Justification::centredLeft);

                    auto bar = take (row, cBar).reduced (0, 4).withTrimmedRight (8).toFloat();
                    g.setColour (panelEdge);
                    g.fillRect (bar);
                    const float amp = total > 0.0 ? (float) std::sqrt (std::norm (t.c) / total) : 0.0f;
                    g.setColour (green);
                    g.fillRect (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, amp)));

                    g.setColour (text);
                    g.drawText (t.sourceHz > 0.0 ? juce::String (t.sourceHz, t.sourceHz < 100.0 ? 1 : 0) + " Hz" : juce::String ("-"),
                                take (row, cHz), juce::Justification::centredLeft);
                    g.setColour (std::abs (t.detuneCents) < 10.0 ? dim : amber);
                    g.drawText ((t.detuneCents >= 0.0 ? "+" : "") + juce::String ((int) std::round (t.detuneCents)) + "c",
                                take (row, cCents), juce::Justification::centredLeft);
                    g.setColour (dim);
                    g.drawText (t.reason, row, juce::Justification::centredLeft, true);
                }
            }

            g.setColour (text);
            g.drawText (f0Line, r, juce::Justification::centredLeft, true);
            if ((int) terms.size() > tableRows)
            {
                g.setColour (dim);
                g.drawText ("+" + juce::String ((int) terms.size() - tableRows) + " more", r, juce::Justification::centredRight);
            }
        }

    private:
        std::vector<orbital::ModeTerm> terms;
        juce::String f0Line;
    };

    // Three segmented buttons for the render mode with a cost hint under each
    class ModeSelector : public juce::Component
    {
    public:
        explicit ModeSelector (juce::AudioProcessorValueTreeState& s) : state (s)
        {
            const char* names[] = { "Slices", "Voxel grid", "Point cloud" };
            for (int i = 0; i < 3; ++i)
            {
                auto& b = buttons[(size_t) i];
                b.setButtonText (names[i]);
                b.setClickingTogglesState (false);
                b.setColour (juce::TextButton::buttonOnColourId, Palette::green.withAlpha (0.35f));
                b.setConnectedEdges ((i > 0 ? (int) juce::Button::ConnectedOnLeft : 0)
                                   | (i < 2 ? (int) juce::Button::ConnectedOnRight : 0));
                b.onClick = [this, i]
                {
                    if (auto* p = state.getParameter ("orbRMode"))
                        p->setValueNotifyingHost (p->convertTo0to1 ((float) i));
                };
                addAndMakeVisible (b);
            }
        }

        void refresh()
        {
            const int mode = (int) readParam (state, "orbRMode", (float) voxels);
            for (int i = 0; i < 3; ++i)
                buttons[(size_t) i].setToggleState (i == mode, juce::dontSendNotification);
        }

        void paint (juce::Graphics& g) override
        {
            const char* hints[] = { "cheap", "balanced", "heavy" };
            const juce::Colour colours[] = { Palette::green, Palette::amber, Palette::pink };
            auto r = getLocalBounds().removeFromBottom (14);
            const int w = r.getWidth() / 3;
            g.setFont (Theme::font (10.0f));
            for (int i = 0; i < 3; ++i)
            {
                g.setColour (colours[i].withAlpha (0.8f));
                g.drawText (hints[i], i < 2 ? r.removeFromLeft (w) : r, juce::Justification::centred);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().removeFromTop (26);
            const int w = r.getWidth() / 3;
            for (int i = 0; i < 3; ++i)
                buttons[(size_t) i].setBounds (i < 2 ? r.removeFromLeft (w) : r);
        }

    private:
        juce::AudioProcessorValueTreeState& state;
        std::array<juce::TextButton, 3> buttons;
    };
}

struct OrbitalPanel::Impl : private juce::Timer
{
    Impl (OrbitalPanel& owner, ClodScopeProcessor& p, orbital::OrbitalModel& m, HelpTarget* h)
        : panel (owner), proc (p), model (m), help (h), modeSelector (p.apvts),
          modeHover (modeSelector, "orbRMode", h), tableHover (table, "orb.terms", h)
    {
        potential = choice ("orbPotential", "POTENTIAL (the 'atom')");
        mapping   = choice ("orbMapping", "LEVEL MAPPING (frequency -> energy)");
        angular   = choice ("orbAngular", "ANGULAR SOURCE (picks l and m)");
        basis     = choice ("orbBasis", "BASIS");
        fundMode  = choice ("orbFundMode", "FUNDAMENTAL f0");

        atomKnobs = { knob ("orbZ", "ELEMENT Z"), knob ("orbScreen", "SCREENING") };
        atomInfo.setFont (Theme::font (11.0f, true));
        atomInfo.setColour (juce::Label::textColourId, Palette::text);
        atomInfo.setJustificationType (juce::Justification::centredLeft);
        atomInfo.setInterceptsMouseClicks (false, false);
        panel.addAndMakeVisible (atomInfo);

        for (auto* d : { &potentialDesc, &mappingDesc, &angularDesc, &basisDesc })
        {
            d->setFont (Theme::font (11.0f));
            d->setColour (juce::Label::textColourId, Palette::dim);
            d->setJustificationType (juce::Justification::topLeft);
            d->setMinimumHorizontalScale (1.0f);
            d->setInterceptsMouseClicks (false, false);
            panel.addAndMakeVisible (*d);
        }

        bridgeKnobs = { knob ("orbTimeScale", "TIME SCALE"), knob ("orbQuantize", "QUANTIZE"),
                        knob ("orbMaxTerms", "MAX TERMS"), knob ("orbMaxN", "MAX n"),
                        knob ("orbFundHz", "f0 (MANUAL)"), knob ("orbSmooth", "SMOOTH"),
                        knob ("orbGate", "GATE"), nullptr,
                        knob ("orbL", "l (SHAPE)"), knob ("orbM", "m (ORIENT.)") };

        dialsNote.setText ("l and m dials are used when Angular source = Dials.", juce::dontSendNotification);
        dialsNote.setFont (Theme::font (10.5f));
        dialsNote.setColour (juce::Label::textColourId, Palette::dim);
        panel.addAndMakeVisible (dialsNote);

        panel.addAndMakeVisible (table);
        panel.addAndMakeVisible (modeSelector);

        sliceKnob  = knob ("orbRSlice", "SLICE OFFSET", true);
        resKnob    = knob ("orbRRes", "RESOLUTION");
        pointsKnob = knob ("orbRPoints", "POINTS");
        renderKnobs = { knob ("orbRBudget", "BUDGET"), knob ("orbRExposure", "EXPOSURE"), knob ("orbRThreshold", "THRESHOLD") };
        spinKnob = knob ("orbRSpin", "AUTO-SPIN");
        contrastKnob = knob ("orbRContrast", "CONTRAST");
        colour = choice ("orbRColour", "COLOUR MAP");
        if (proc.apvts.getParameter ("orbRFreeze") != nullptr)
        {
            freeze = std::make_unique<ParamToggle> (proc.apvts, "orbRFreeze", "Pause clock", help);
            panel.addAndMakeVisible (*freeze);
        }
        if (proc.apvts.getParameter ("orbSolo") != nullptr)
        {
            solo = std::make_unique<ParamToggle> (proc.apvts, "orbSolo", "Strongest state only (clean shape)", help);
            panel.addAndMakeVisible (*solo);
        }

        refresh();
        startTimerHz (5);
    }

    ~Impl() override { stopTimer(); }

    // Widgets are only created for parameters that exist, so a missing id never crashes the panel
    std::unique_ptr<ParamKnob> knob (const char* id, const char* label, bool bipolar = false)
    {
        if (proc.apvts.getParameter (id) == nullptr)
            return nullptr;
        auto k = std::make_unique<ParamKnob> (proc.apvts, id, label, help, bipolar);
        panel.addAndMakeVisible (*k);
        return k;
    }

    std::unique_ptr<ParamChoice> choice (const char* id, const char* label)
    {
        if (proc.apvts.getParameter (id) == nullptr)
            return nullptr;
        auto c = std::make_unique<ParamChoice> (proc.apvts, id, label, help);
        panel.addAndMakeVisible (*c);
        return c;
    }

    float param (const char* id, float fallback) const { return readParam (proc.apvts, id, fallback); }

    void timerCallback() override { refresh(); }

    void refresh()
    {
        const int pot = (int) param ("orbPotential", 2.0f);
        potentialDesc.setText (potentialText (pot), juce::dontSendNotification);
        const bool atom = pot == orbital::element;
        atomInfo.setText (atom ? atomText ((int) param ("orbZ", 11.0f), param ("orbScreen", 1.0f))
                               : juce::String ("Element Z and Screening apply when Potential = Atom (element Z)."), juce::dontSendNotification);
        atomInfo.setColour (juce::Label::textColourId, atom ? Palette::text : Palette::dim);
        mappingDesc.setText (mappingText ((int) param ("orbMapping", 0.0f)), juce::dontSendNotification);
        angularDesc.setText (angularText ((int) param ("orbAngular", 0.0f)), juce::dontSendNotification);
        basisDesc.setText (basisText ((int) param ("orbBasis", 0.0f)), juce::dontSendNotification);

        const bool manual = (int) param ("orbFundMode", 0.0f) == 1;
        const bool dials = (int) param ("orbAngular", 0.0f) == orbital::angularDials;
        auto enable = [] (ParamKnob* k, bool on)
        {
            if (k != nullptr)
            {
                k->setEnabled (on);
                k->setAlpha (on ? 1.0f : 0.4f);
            }
        };
        enable (bridgeKnobs[4].get(), manual);
        enable (bridgeKnobs[8].get(), dials);
        enable (bridgeKnobs[9].get(), dials);
        enable (atomKnobs[0].get(), atom);
        enable (atomKnobs[1].get(), atom);
        dialsNote.setText (atom ? "Atom: each subshell fixes l (s p d f); the l dial only matters where subshells share a rung."
                                : "l and m dials are used when Angular source = Dials.", juce::dontSendNotification);
        dialsNote.setVisible (atom || ! dials);

        const int mode = (int) param ("orbRMode", (float) voxels);
        if (sliceKnob)  sliceKnob->setVisible (mode == slices);
        if (resKnob)    resKnob->setVisible (mode == voxels);
        if (pointsKnob) pointsKnob->setVisible (mode == pointCloud);
        modeSelector.refresh();

        const juce::String f0 = manual ? "f0 = " + juce::String (param ("orbFundHz", 220.0f), 1) + " Hz (manual)"
                                       : juce::String ("f0 = tracked from the audio (Auto)");
        table.setTerms (model.terms(), f0 + "   |   " + juce::String ((int) model.terms().size()) + " states in the mix");
    }

    // Lays the panel out for `width`; with apply = false it only measures (for idealHeight)
    int layout (int width, bool apply)
    {
        const int pad = 12;
        const int inner = width - pad * 2;
        const int cw = inner / 4;
        int y = 0;
        auto place = [apply] (juce::Component* c, juce::Rectangle<int> r) { if (apply && c != nullptr) c->setBounds (r); };
        auto knobRow = [&] (std::initializer_list<juce::Component*> comps)
        {
            int i = 0;
            for (auto* c : comps)
                place (c, { pad + (i++) * cw, y, cw, Theme::knobCellHeight - 4 });
            y += Theme::knobCellHeight;
        };

        // --- bridge: sound -> atom ---
        const int bridgeTop = y;
        y += Theme::sectionTitle + 4;
        const std::pair<juce::Component*, juce::Component*> choiceRows[] = {
            { potential.get(), &potentialDesc }, { mapping.get(), &mappingDesc },
            { angular.get(), &angularDesc }, { basis.get(), &basisDesc } };
        for (auto [c, desc] : choiceRows)
        {
            place (c, { pad, y, inner, 40 });
            place (desc, { pad, y + 42, inner, 28 });
            y += 76;
            if (c == potential.get())
            {
                place (atomKnobs[0].get(), { pad, y, cw, Theme::knobCellHeight - 4 });
                place (atomKnobs[1].get(), { pad + cw, y, cw, Theme::knobCellHeight - 4 });
                place (&atomInfo, { pad + 2 * cw + 6, y, inner - 2 * cw - 6, Theme::knobCellHeight - 8 });
                y += Theme::knobCellHeight + 4;
            }
        }
        place (fundMode.get(), { pad, y, juce::jmin (inner, 200), 40 });
        place (solo.get(), { pad + juce::jmin (inner, 200) + 12, y + 16, inner - juce::jmin (inner, 200) - 12, 24 });
        y += 46;
        knobRow ({ bridgeKnobs[0].get(), bridgeKnobs[1].get(), bridgeKnobs[2].get(), bridgeKnobs[3].get() });
        knobRow ({ bridgeKnobs[4].get(), bridgeKnobs[5].get(), bridgeKnobs[6].get(), bridgeKnobs[7].get() });
        knobRow ({ bridgeKnobs[8].get(), bridgeKnobs[9].get() });
        place (&dialsNote, { pad, y - 4, inner, 16 });
        y += 16;
        bridgeRect = { 0, bridgeTop, width, y - bridgeTop };
        y += 12;

        // --- live mapping ---
        const int liveTop = y;
        y += Theme::sectionTitle;
        place (&table, { pad, y, inner, TermsTable::heightFor() });
        y += TermsTable::heightFor() + 10;
        liveRect = { 0, liveTop, width, y - liveTop };
        y += 12;

        // --- render ---
        const int renderTop = y;
        y += Theme::sectionTitle + 2;
        place (&modeSelector, { pad, y, inner, 42 });
        y += 48;
        for (auto* k : { sliceKnob.get(), resKnob.get(), pointsKnob.get() })   // only one is visible, for the current mode
            place (k, { pad, y, cw, Theme::knobCellHeight - 4 });
        for (int i = 0; i < 3; ++i)
            place (renderKnobs[(size_t) i].get(), { pad + (i + 1) * cw, y, cw, Theme::knobCellHeight - 4 });
        y += Theme::knobCellHeight;
        place (spinKnob.get(), { pad, y, cw, Theme::knobCellHeight - 4 });
        place (contrastKnob.get(), { pad + cw, y, cw, Theme::knobCellHeight - 4 });
        place (colour.get(), { pad + 2 * cw + 8, y + 4, inner - 2 * cw - 8, 40 });
        place (freeze.get(), { pad + 2 * cw + 8, y + 52, 140, 24 });
        y += Theme::knobCellHeight + 6;
        renderRect = { 0, renderTop, width, y - renderTop };
        return y;
    }

    OrbitalPanel& panel;
    ClodScopeProcessor& proc;
    orbital::OrbitalModel& model;
    HelpTarget* help;

    std::unique_ptr<ParamChoice> potential, mapping, angular, basis, fundMode, colour;
    juce::Label potentialDesc, mappingDesc, angularDesc, basisDesc, dialsNote, atomInfo;
    std::array<std::unique_ptr<ParamKnob>, 2> atomKnobs;
    std::array<std::unique_ptr<ParamKnob>, 10> bridgeKnobs;
    TermsTable table;
    ModeSelector modeSelector;
    widgets_detail::HoverReporter modeHover, tableHover;
    std::unique_ptr<ParamKnob> sliceKnob, resKnob, pointsKnob, spinKnob, contrastKnob;
    std::array<std::unique_ptr<ParamKnob>, 3> renderKnobs;
    std::unique_ptr<ParamToggle> freeze, solo;

    juce::Rectangle<int> bridgeRect, liveRect, renderRect;
};

OrbitalPanel::OrbitalPanel (ClodScopeProcessor& p, orbital::OrbitalModel& m, OrbitalView& v, HelpTarget* h)
{
    juce::ignoreUnused (v);
    impl = std::make_unique<Impl> (*this, p, m, h);
}

OrbitalPanel::~OrbitalPanel() = default;

int OrbitalPanel::idealHeight (int width) const
{
    return impl->layout (width, false);
}

void OrbitalPanel::paint (juce::Graphics& g)
{
    Theme::drawSection (g, impl->bridgeRect, "BRIDGE: SOUND -> ATOM");
    Theme::drawSection (g, impl->liveRect, "LIVE MAPPING");
    Theme::drawSection (g, impl->renderRect, "RENDER");
}

void OrbitalPanel::resized()
{
    impl->layout (getWidth(), true);
}
