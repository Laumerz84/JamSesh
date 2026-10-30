#include "ViewTile.h"
#include "../UI/ViewFeed.h"
#include "../UI/Theme.h"
#include "../UI/FastTrace.h"
#include "../UI/ScopeView.h"
#include "../Render3D/Scene3D.h"
#include "../Render3D/Views3D.h"
#include "../Orbital/OrbitalModel.h"
#include "../Orbital/OrbitalView.h"

// Owner: views soldier.
namespace
{
    enum Kind { scope2D = 0, views3D = 1, orbitalCloud = 2, sculptWave = 3 };

    // Forwards to whichever feed the tile has now, so the renderers keep one reference; silence without one
    struct ForwardFeed : public ViewFeed
    {
        int copyLatest (bool output, float* left, float* right, int numSamples) const override
        {
            if (target != nullptr)
                return target->copyLatest (output, left, right, numSamples);
            std::fill (left, left + numSamples, 0.0f);
            std::fill (right, right + numSamples, 0.0f);
            return numSamples;
        }

        double sampleRate() const override { return target != nullptr ? target->sampleRate() : 48000.0; }

        const ViewFeed* target = nullptr;
    };

    // A tile is small: the orbital cloud's sample budget is capped so a friend's "ultra" setting
    // costs a tile no more than a small view needs
    struct TileParams : public ViewParams
    {
        explicit TileParams (const ViewParams& b) : base (b) {}

        float get (juce::StringRef id, float fallback) const override
        {
            const float v = base.get (id, fallback);
            if (id == juce::StringRef ("orbRBudget")) return juce::jmin (v, 30000.0f);
            if (id == juce::StringRef ("orbRPoints")) return juce::jmin (v, 12000.0f);
            if (id == juce::StringRef ("orbRRes"))    return juce::jmin (v, 24.0f);
            return v;
        }

        juce::String getText (juce::StringRef id, const juce::String& fallback) const override { return base.getText (id, fallback); }

        const ViewParams& base;
    };

    // The wave sculptor, read-only: two cycles of their sculpted wave over its partials
    class SculptTile : public juce::Component
    {
    public:
        explicit SculptTile (const StateParams& p) : params (p) { setOpaque (true); }

        void stateChanged()
        {
            SculptWave w;
            if (SculptWave::fromBytes (params.state.extra, w))
                wave = w;
            picture = {};
            repaint();
        }

        void resized() override { picture = {}; }
        void lookAndFeelChanged() override { picture = {}; repaint(); }

        void paint (juce::Graphics& g) override
        {
            if (! picture.isValid() || picture.getWidth() != getWidth() || picture.getHeight() != getHeight())
                render();
            g.drawImageAt (picture, 0, 0);

            using namespace Palette;
            g.setFont (Theme::font (11.0f, true));
            g.setColour (Accent::sculpt);
            g.drawText ("WAVE SCULPTOR", getLocalBounds().reduced (8, 5), juce::Justification::topLeft);
            int used = 0;
            for (int k = 0; k < wave.count; ++k)
                used += wave.amp[k] > 1.0e-4f ? 1 : 0;
            juce::String info;
            info << used << " partials" << (wave.harmonic ? "" : "  (inharmonic)")
                 << "   morph " << juce::roundToInt (params.get ("scMorph", 0.0f) * 100.0f) << " %";
            g.setFont (Theme::font (10.5f));
            g.setColour (dim);
            g.drawText (info, getLocalBounds().reduced (8, 5), juce::Justification::topRight);
        }

    private:
        void render()
        {
            using namespace Palette;
            const int w = juce::jmax (1, getWidth()), h = juce::jmax (1, getHeight());
            picture = juce::Image (juce::Image::RGB, w, h, true, juce::SoftwareImageType());
            const auto all = juce::Rectangle<int> (0, 0, w, h);
            auto waveArea = all.withTrimmedTop (22).withTrimmedBottom (h / 3).reduced (6, 2).toFloat();
            auto barArea = all.withTop (all.getBottom() - h / 3).reduced (8, 6).toFloat();

            {
                juce::Graphics g (picture);
                g.fillAll (viewBg);
                g.setColour (grid);
                g.drawHorizontalLine ((int) waveArea.getCentreY(), waveArea.getX(), waveArea.getRight());
                g.drawVerticalLine ((int) waveArea.getCentreX(), waveArea.getY(), waveArea.getBottom());
                g.drawHorizontalLine ((int) barArea.getBottom(), barArea.getX(), barArea.getRight());

                // partial bars, height ~ sqrt(amp) so quiet overtones still show
                const int shown = juce::jmin (wave.count, 32);
                float maxAmp = 1.0e-6f;
                for (int k = 0; k < shown; ++k)
                    maxAmp = juce::jmax (maxAmp, wave.amp[k]);
                const float slot = barArea.getWidth() / (float) juce::jmax (1, shown);
                for (int k = 0; k < shown; ++k)
                {
                    const float bh = barArea.getHeight() * std::sqrt (wave.amp[k] / maxAmp);
                    g.setColour (Accent::sculpt.withAlpha (0.35f + 0.5f * wave.amp[k] / maxAmp));
                    g.fillRect (barArea.getX() + slot * (float) k + 1.0f, barArea.getBottom() - bh, juce::jmax (1.0f, slot - 2.0f), bh);
                }
            }

            // two cycles of the wave
            float peak = 1.0e-4f;
            const int cols = juce::jmax (2, (int) waveArea.getWidth());
            std::vector<float> ys ((size_t) cols);
            for (int i = 0; i < cols; ++i)
            {
                ys[(size_t) i] = wave.sampleCycle (2.0f * (float) i / (float) (cols - 1));
                peak = juce::jmax (peak, std::abs (ys[(size_t) i]));
            }
            points.clear();
            for (int i = 0; i < cols; ++i)
                points.push_back ({ waveArea.getX() + (float) i,
                                    waveArea.getCentreY() - ys[(size_t) i] / peak * waveArea.getHeight() * 0.45f });
            juce::Image::BitmapData px (picture, juce::Image::BitmapData::readWrite);
            stroker.stroke (px, points, 2.5f, Accent::sculpt.withAlpha (0.2f));
            stroker.stroke (px, points, 0.8f, Accent::sculpt);
        }

        const StateParams& params;
        SculptWave wave;
        juce::Image picture;
        std::vector<juce::Point<float>> points;
        fasttrace::Stroker stroker;
    };
}

//==============================================================================
struct ViewTile::Impl : public juce::Timer
{
    explicit Impl (ViewTile& o) : owner (o) {}
    ~Impl() override { stopTimer(); }

    void build (int newKind)
    {
        stopTimer();
        scope.reset();
        orbital.reset();
        model.reset();
        views.reset();
        scene.reset();
        sculpt.reset();
        kind = newKind;

        switch (kind)
        {
            case scope2D:
                scope = std::make_unique<ScopeView> (feed, params, 0);   // driven by this tile's timer
                owner.addAndMakeVisible (*scope);
                break;
            case views3D:
                scene = std::make_unique<Scene3D>();
                views = std::make_unique<Views3D> (feed, tileParams);
                owner.addAndMakeVisible (*scene);
                break;
            case orbitalCloud:
                scene = std::make_unique<Scene3D>();
                model = std::make_unique<orbital::OrbitalModel> (tileParams);
                orbital = std::make_unique<OrbitalView> (feed, tileParams, *model);
                owner.addAndMakeVisible (*scene);
                break;
            default:
                sculpt = std::make_unique<SculptTile> (params);
                sculpt->stateChanged();
                owner.addAndMakeVisible (*sculpt);
                break;
        }
        owner.resized();
        if (kind != sculptWave)
            startTimerHz (fps);
    }

    void frame()
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        if (scope != nullptr)
            scope->tick();
        else if (scene != nullptr && views != nullptr)
            views->produce (*scene);
        else if (scene != nullptr && orbital != nullptr)
            orbital->produce (*scene);
        frameMs = juce::Time::getMillisecondCounterHiRes() - t0;
    }

    void timerCallback() override
    {
        if (owner.isShowing())
            frame();
    }

    juce::Component* content() const
    {
        if (scope != nullptr) return scope.get();
        if (scene != nullptr) return scene.get();
        return sculpt.get();
    }

    ViewTile& owner;
    ForwardFeed feed;
    StateParams params;
    TileParams tileParams { params };
    int kind = -1, fps = 30;
    double frameMs = 0.0;

    std::unique_ptr<ScopeView> scope;
    std::unique_ptr<Scene3D> scene;          // declared before its producers, so it outlives them
    std::unique_ptr<Views3D> views;
    std::unique_ptr<orbital::OrbitalModel> model;
    std::unique_ptr<OrbitalView> orbital;
    std::unique_ptr<SculptTile> sculpt;
};

ViewTile::ViewTile() : impl (std::make_unique<Impl> (*this))
{
    setOpaque (true);
    impl->build (scope2D);
}

ViewTile::~ViewTile() = default;

void ViewTile::setFeed (ViewFeed* f) { impl->feed.target = f; }

void ViewTile::setViewState (const ViewState& s)
{
    auto& d = *impl;
    const bool textsChanged = s.texts != d.params.state.texts;
    d.params.state = s;
    if (s.mainView != d.kind)
        d.build (s.mainView);
    else if (d.views != nullptr && textsChanged)
        d.views->reloadFromState();   // their custom formulas
    else if (d.sculpt != nullptr)
        d.sculpt->stateChanged();
}

void ViewTile::setDefaults (const juce::AudioProcessorValueTreeState* s) { impl->params.defaults = s; }

void ViewTile::setFramesPerSecond (int f)
{
    impl->fps = juce::jlimit (1, 60, f);
    if (impl->kind != sculptWave)
        impl->startTimerHz (impl->fps);
}

void ViewTile::tick() { impl->frame(); }
double ViewTile::lastFrameMs() const { return impl->frameMs; }

void ViewTile::paint (juce::Graphics& g)
{
    if (impl->content() == nullptr)
        g.fillAll (Palette::viewBg);
}

void ViewTile::resized()
{
    if (auto* c = impl->content())
        c->setBounds (getLocalBounds());
}
