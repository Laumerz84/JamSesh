#include "ScopeView.h"
#include "../DspUtil.h"

namespace
{
    juce::Font font (float size, bool bold = false)
    {
        auto opts = juce::FontOptions (size);
        return juce::Font (bold ? opts.withStyle ("Bold") : opts);
    }

    juce::String dbString (float db)
    {
        return db <= -99.0f ? juce::String ("-inf") : juce::String (db, 1);
    }

}

ScopeView::ScopeView (ClodScopeProcessor& p)
    : ownFeed (std::make_unique<ProcessorFeed> (p)), ownParams (std::make_unique<ApvtsParams> (p.apvts)), proc (&p),
      feed (ownFeed.get()), params (ownParams.get()), slit (&p.doubleSlit), fps (60)
{
    init();
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

ScopeView::ScopeView (const ViewFeed& f, const ViewParams& ps, int framesPerSecond, const DoubleSlitEngine* slitEngine)
    : feed (&f), params (&ps), slit (slitEngine), fps (juce::jlimit (0, 120, framesPerSecond))
{
    init();
}

void ScopeView::init()
{
    const size_t n = (size_t) ScopeBuffer::size / 2;
    inL.assign (n, 0.0f); inR.assign (n, 0.0f);
    outL.assign (n, 0.0f); outR.assign (n, 0.0f);
    midScratch.assign (n, 0.0f);
    setOpaque (true);
    if (fps > 0)
        startTimerHz (fps);
}

ScopeView::~ScopeView() { stopTimer(); }

void ScopeView::setFramesPerSecond (int framesPerSecond)
{
    fps = juce::jlimit (0, 120, framesPerSecond);
    if (fps == 0)
        stopTimer();
    else if (isVisible())
        startTimerHz (fps);
}

void ScopeView::lookAndFeelChanged()
{
    // the grid and spectrum backdrops are cached pictures in theme colours
    layoutAreas();
    renderTraces();
    if (spectrumShown)
        renderSpectrum();
    repaint();
}

DoubleSlitEngine::Settings ScopeView::slitSettings() const
{
    if (slit != nullptr)
        return slit->currentSettings();

    // a friend's tile: the same geometry from their settings (the pitch is the one this view measures)
    DoubleSlitEngine::Settings s;
    s.sizing     = (int) param ("dsSizing", (float) s.sizing);
    s.multiple   = param ("dsMultiple", s.multiple);
    s.separation = param ("dsSeparation", s.separation);
    s.width      = param ("dsWidth", s.width);
    s.fringes    = (int) param ("dsFringes", (float) s.fringes);
    s.distance   = param ("dsDistance", s.distance);
    s.ear        = (int) param ("dsEar", (float) s.ear);
    s.sampleRate = feed->sampleRate();
    s.pitchHz    = freqHz > 0.0f ? freqHz : 220.0f;
    return s;
}

void ScopeView::resized() { layoutAreas(); }

void ScopeView::layoutAreas()
{
    spectrumShown = wantsSpectrum();
    slitShown = wantsSlit();
    auto area = getLocalBounds();

    // bottom strip holds the spectrum and/or the double-slit graphic side by side
    juce::Rectangle<int> strip;
    if (spectrumShown || slitShown)
    {
        strip = area.removeFromBottom (juce::jmax (120, getHeight() / (slitShown ? 3 : 4)));
        area.removeFromBottom (6);
    }
    spectrumArea = {};
    slitArea = {};
    if (spectrumShown && slitShown)
    {
        spectrumArea = strip.removeFromLeft (strip.getWidth() / 2 - 3);
        strip.removeFromLeft (6);
        slitArea = strip;
    }
    else if (spectrumShown)
        spectrumArea = strip;
    else if (slitShown)
        slitArea = strip;
    scopeArea = area;

    // CPU images: the window paints in software (UI/StreamSafe.h), so a GPU image would be copied back every frame
    auto picture = [this] (juce::Rectangle<int> r)
    {
        if (r.isEmpty())
            return juce::Image();
        return juce::Image (juce::Image::RGB, juce::jmax (1, juce::roundToInt ((float) r.getWidth() * pixelScale)),
                            juce::jmax (1, juce::roundToInt ((float) r.getHeight() * pixelScale)), true, juce::SoftwareImageType());
    };
    traces = picture (scopeArea);
    backdrop = picture (scopeArea);
    spectrum = picture (spectrumArea);
    spectrumBackdrop = picture (spectrumArea);
    spectrumRate = 0.0f;
    renderBackdrop();
    if (slitArea.getWidth() > 0 && slitArea.getHeight() > 0)
        slitField = juce::Image (juce::Image::RGB, juce::jmax (1, slitArea.getWidth() / 3), juce::jmax (1, slitArea.getHeight() / 3), true,
                                 juce::SoftwareImageType());
}

float ScopeView::sample (bool output, Chan c, int i) const noexcept
{
    const auto& l = output ? outL : inL;
    const auto& r = output ? outR : inR;
    switch (c)
    {
        case left:  return l[(size_t) i];
        case right: return r[(size_t) i];
        case mid:   return 0.5f * (l[(size_t) i] + r[(size_t) i]);
        case side:  return 0.5f * (l[(size_t) i] - r[(size_t) i]);
    }
    return 0.0f;
}

void ScopeView::timerCallback()
{
    if (wantsSpectrum() != spectrumShown || wantsSlit() != slitShown)
        layoutAreas();

    const double sr = feed->sampleRate();
    const int maxCopy = ScopeBuffer::size / 2;
    segLen = juce::jlimit (16, maxCopy / 2, (int) (param ("time", 20.0f) * 0.001 * sr));

    if (param ("freeze") < 0.5f)
    {
        copied = juce::jmin (maxCopy, juce::jmax (segLen * 2 + 2, 8192));
        feed->copyLatestPadded (false, inL.data(), inR.data(), copied);
        feed->copyLatestPadded (true, outL.data(), outR.data(), copied);
        updateReadouts();
        if (spectrumShown)
            updateSpectrum();
    }

    segLen = juce::jmin (segLen, juce::jmax (16, copied / 2));
    findTrigger();
    renderTraces();
    if (spectrumShown)
        renderSpectrum();
    if (slitShown)
        renderSlitField();
    repaint();
}

void ScopeView::findTrigger()
{
    const int mode = (int) param ("trigMode");
    const float level = param ("trigLevel");
    const int view = (int) param ("view");
    const bool output = (int) param ("source", 2.0f) != 0;
    const Chan c = view == 2 ? right : view == 3 ? mid : view == 4 ? side : left;

    const int pre = segLen / 10;   // trigger point sits 10% into the window, like a hardware scope
    segStart = copied - segLen;
    triggered = false;

    if (mode == 0 || view == 5 || copied < segLen + 2)
        return;

    for (int t = copied - (segLen - pre); t > pre + 1; --t)
    {
        const float a = sample (output, c, t - 1), b = sample (output, c, t);
        if (mode == 1 ? (a < level && b >= level) : (a > level && b <= level))
        {
            segStart = t - pre;
            triggered = true;
            return;
        }
    }
}

void ScopeView::updateReadouts()
{
    const bool output = (int) param ("source", 2.0f) != 0;
    const int start = copied - segLen;

    float peak = 0.0f;
    double sumSq = 0.0;
    for (int i = start; i < copied; ++i)
    {
        const float v = sample (output, mid, i);
        peak = juce::jmax (peak, std::abs (v));
        sumSq += (double) v * v;
    }
    const float rms = (float) std::sqrt (sumSq / juce::jmax (1, segLen));
    peakDb = juce::Decibels::gainToDecibels (peak, -100.0f);
    rmsDb = juce::Decibels::gainToDecibels (rms, -100.0f);

    for (int i = 0; i < copied; ++i)
        midScratch[(size_t) i] = sample (output, mid, i);
    freqHz = DspUtil::estimateFrequency (midScratch.data(), copied, feed->sampleRate());
}

void ScopeView::updateSpectrum()
{
    const bool output = (int) param ("source", 2.0f) != 0;
    const int start = copied - fftSize;
    if (start < 0)
        return;

    std::fill (fftData.begin(), fftData.end(), 0.0f);
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = sample (output, mid, start + i);

    window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    // Hann window has coherent gain 0.5, so a full-scale sine reads 0 dB
    const float norm = 4.0f / (float) fftSize;
    for (size_t b = 0; b < specDb.size(); ++b)
    {
        const float db = juce::Decibels::gainToDecibels (fftData[b] * norm, -120.0f);
        specDb[b] = juce::jmax (db, specDb[b] - 1.2f);
    }
}

// The scope's grid, drawn once per layout or grid change into `backdrop`
void ScopeView::renderBackdrop()
{
    const bool xy = (int) param ("view") == 5;
    backdropView = xy ? 1 : 0;
    if (! backdrop.isValid())
        return;

    {
        juce::Graphics g (backdrop);
        g.addTransform (juce::AffineTransform::scale ((float) backdrop.getWidth() / (float) scopeArea.getWidth(),
                                                      (float) backdrop.getHeight() / (float) scopeArea.getHeight()));
        paintGrid (g, scopeArea.withZeroOrigin().toFloat(), xy);
    }

    // the traces start again on the bare grid
    juce::Image::BitmapData dst (traces, juce::Image::BitmapData::writeOnly);
    const juce::Image::BitmapData grid (backdrop, juce::Image::BitmapData::readOnly);
    fasttrace::fadeToward (dst, grid, 0.0f);
}

void ScopeView::renderTraces()
{
    if (! traces.isValid())
        return;

    const int view = (int) param ("view");
    if (backdropView != (view == 5 ? 1 : 0))
        renderBackdrop();

    // afterglow: the last frame fades back into the grid; frozen or without persistence it starts clean
    const float persist = param ("persist", 35.0f) * 0.01f;
    {
        juce::Image::BitmapData dst (traces, juce::Image::BitmapData::readWrite);
        const juce::Image::BitmapData grid (backdrop, juce::Image::BitmapData::readOnly);
        fasttrace::fadeToward (dst, grid, param ("freeze") > 0.5f ? 0.0f : persist);
    }

    const int source = (int) param ("source", 2.0f);

    // 0 = input only, 1 = output only, 2 = input dimmed underneath output
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool output = pass == 1;
        if ((source == 0 && output) || (source == 1 && ! output))
            continue;

        const float alpha = (source == 2 && ! output) ? 0.4f : 1.0f;

        if (view == 5)
        {
            drawXY (output, output ? Palette::green : Palette::amber, alpha);
            continue;
        }

        const juce::Colour lc = (source == 2 && ! output) ? Palette::amber : Palette::green;
        const juce::Colour rc = (source == 2 && ! output) ? Palette::pink : Palette::blue;

        switch (view)
        {
            case 0: drawWave (output, left, lc, alpha); drawWave (output, right, rc, alpha); break;
            case 1: drawWave (output, left, lc, alpha); break;
            case 2: drawWave (output, right, rc, alpha); break;
            case 3: drawWave (output, mid, lc, alpha); break;
            case 4: drawWave (output, side, rc, alpha); break;
            default: break;
        }
    }
}

// Coordinates here are pixels of `traces` (the screen's pixel size)
void ScopeView::drawWave (bool output, Chan c, juce::Colour colour, float alpha)
{
    const float w = (float) traces.getWidth(), h = (float) traces.getHeight();
    const float centre = h * 0.5f, scale = h * 0.45f * zoom();   // verticalScale() in these pixels
    auto toY = [&] (float v) { return juce::jlimit (-h, h * 2.0f, centre - v * scale); };

    points.clear();
    if (segLen <= (int) w * 2)
    {
        for (int i = 0; i < segLen; ++i)
            points.push_back ({ (float) i * w / (float) (segLen - 1), toY (sample (output, c, segStart + i)) });
    }
    else
    {
        // dense window: min/max per pixel column so peaks are never skipped
        const int cols = (int) w;
        for (int px = 0; px < cols; ++px)
        {
            const int a = segStart + (int) ((int64_t) px * segLen / cols);
            const int b = juce::jmax (a + 1, segStart + (int) ((int64_t) (px + 1) * segLen / cols));
            float mn = sample (output, c, a), mx = mn;
            for (int i = a + 1; i < b; ++i)
            {
                const float v = sample (output, c, i);
                mn = juce::jmin (mn, v);
                mx = juce::jmax (mx, v);
            }
            points.push_back ({ (float) px, toY (mx) });
            points.push_back ({ (float) px + 0.5f, toY (mn) });
        }
    }

    juce::Image::BitmapData pixels (traces, juce::Image::BitmapData::readWrite);
    stroker.stroke (pixels, points, 2.5f * pixelScale, colour.withAlpha (0.18f * alpha));   // 5 px glow
    stroker.stroke (pixels, points, 0.75f * pixelScale, colour.withAlpha (alpha));          // 1.5 px line
}

void ScopeView::drawXY (bool output, juce::Colour colour, float alpha)
{
    const float w = (float) traces.getWidth(), h = (float) traces.getHeight();
    const float cx = w * 0.5f, cy = h * 0.5f;
    const float scale = juce::jmin (w, h) * 0.45f * zoom();
    const int n = juce::jmin (segLen, 8192);
    const int start = copied - n;

    points.clear();
    for (int i = 0; i < n; ++i)
        points.push_back ({ juce::jlimit (-w, w * 2.0f, cx + sample (output, left, start + i) * scale),
                            juce::jlimit (-h, h * 2.0f, cy - sample (output, right, start + i) * scale) });

    juce::Image::BitmapData pixels (traces, juce::Image::BitmapData::readWrite);
    stroker.stroke (pixels, points, 0.5f * pixelScale, colour.withAlpha (0.8f * alpha));
}

void ScopeView::paintGrid (juce::Graphics& g, juce::Rectangle<float> a, bool xy) const
{
    using namespace Palette;
    g.fillAll (bg);   // the corners outside the rounded panel (the picture is copied as an opaque block)
    g.setColour (viewBg);
    g.fillRoundedRectangle (a, 6.0f);

    g.setColour (grid);
    if (xy)
    {
        const float s = juce::jmin (a.getWidth(), a.getHeight()) * 0.45f;
        g.drawEllipse (a.getCentreX() - s, a.getCentreY() - s, s * 2.0f, s * 2.0f, 1.0f);
        g.drawLine (a.getCentreX() - s, a.getCentreY() + s, a.getCentreX() + s, a.getCentreY() - s, 1.0f);
        g.drawLine (a.getCentreX() - s, a.getCentreY() - s, a.getCentreX() + s, a.getCentreY() + s, 1.0f);
    }
    else
    {
        for (int i = 1; i < 10; ++i)
        {
            const float x = a.getX() + a.getWidth() * (float) i / 10.0f;
            g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
        }
        for (int i = 1; i < 8; ++i)
        {
            const float y = a.getY() + a.getHeight() * (float) i / 8.0f;
            g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
        }
    }

    g.setColour (panelEdge.brighter (0.2f));
    g.drawHorizontalLine ((int) a.getCentreY(), a.getX(), a.getRight());
    g.drawVerticalLine ((int) a.getCentreX(), a.getY(), a.getBottom());
}

// The spectrum's panel, grid and labels, drawn once per layout or sample-rate change
void ScopeView::paintSpectrumBackdrop (juce::Graphics& g, juce::Rectangle<float> a) const
{
    using namespace Palette;
    g.fillAll (bg);
    g.setColour (viewBg);
    g.fillRoundedRectangle (a, 6.0f);

    const float sr = (float) feed->sampleRate();
    const float maxF = juce::jmin (20000.0f, sr * 0.5f);
    auto freqToX = [&] (float f) { return a.getX() + a.getWidth() * std::log (f / 20.0f) / std::log (maxF / 20.0f); };
    auto dbToY = [&] (float db) { return juce::jmap (juce::jlimit (-100.0f, 0.0f, db), -100.0f, 0.0f, a.getBottom(), a.getY() + 4.0f); };

    g.setFont (font (10.0f));
    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = freqToX (f);
        g.setColour (grid);
        g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
        g.setColour (dim);
        g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k" : juce::String ((int) f),
                    (int) x + 3, (int) a.getBottom() - 14, 40, 12, juce::Justification::left);
    }
    for (float db : { -80.0f, -60.0f, -40.0f, -20.0f })
    {
        g.setColour (grid);
        g.drawHorizontalLine ((int) dbToY (db), a.getX(), a.getRight());
    }
}

// The spectrum picture for this frame: the backdrop with the curve and its gradient fill on top
void ScopeView::renderSpectrum()
{
    const float sr = (float) feed->sampleRate();
    if (! spectrum.isValid() || sr <= 0.0f)
        return;

    const float toPixels = (float) spectrum.getWidth() / (float) spectrumArea.getWidth();
    if (sr != spectrumRate)   // the frequency labels depend on the sample rate
    {
        spectrumRate = sr;
        juce::Graphics g (spectrumBackdrop);
        g.addTransform (juce::AffineTransform::scale (toPixels));
        paintSpectrumBackdrop (g, spectrumArea.withZeroOrigin().toFloat());
    }

    const float h = (float) spectrum.getHeight();
    const float maxF = juce::jmin (20000.0f, sr * 0.5f);
    auto dbToY = [&] (float db) { return juce::jmap (juce::jlimit (-100.0f, 0.0f, db), -100.0f, 0.0f, h, 4.0f * toPixels); };

    points.clear();
    const int cols = spectrum.getWidth();
    for (int px = 0; px <= cols; ++px)
    {
        const float f = 20.0f * std::pow (maxF / 20.0f, (float) px / (float) cols);
        const float bin = f * (float) fftSize / sr;
        const int b0 = juce::jlimit (0, (int) specDb.size() - 2, (int) bin);
        const float frac = juce::jlimit (0.0f, 1.0f, bin - (float) b0);
        const float db = specDb[(size_t) b0] + (specDb[(size_t) b0 + 1] - specDb[(size_t) b0]) * frac;
        points.push_back ({ (float) px, dbToY (db) });
    }
    columns.build (points, cols);

    juce::Image::BitmapData pixels (spectrum, juce::Image::BitmapData::readWrite);
    {
        const juce::Image::BitmapData base (spectrumBackdrop, juce::Image::BitmapData::readOnly);
        fasttrace::fadeToward (pixels, base, 0.0f);
    }
    fasttrace::fillBelow (pixels, columns, Palette::blue, 0.35f, 0.02f);
    stroker.stroke (pixels, points, 0.6f * toPixels, Palette::blue);
}

void ScopeView::paint (juce::Graphics& g)
{
    using namespace Palette;

    // the pictures follow the screen's pixel size (e.g. 150 % scaling, or a move to another monitor)
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (std::abs (scale - pixelScale) > 0.01f)
    {
        pixelScale = scale;
        layoutAreas();
        renderTraces();
        if (spectrumShown)
            renderSpectrum();
    }

    // pixel for pixel, so this is a plain copy
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    auto blit = [&g] (const juce::Image& picture, juce::Rectangle<int> area)
    {
        g.drawImageTransformed (picture, juce::AffineTransform::scale ((float) area.getWidth() / (float) picture.getWidth(),
                                                                       (float) area.getHeight() / (float) picture.getHeight())
                                             .translated ((float) area.getX(), (float) area.getY()));
    };

    // opaque: whatever the pictures leave uncovered is the editor's background
    juce::RectangleList<int> uncovered (getLocalBounds());
    if (traces.isValid())
        uncovered.subtract (scopeArea);
    if (spectrumShown && spectrum.isValid())
        uncovered.subtract (spectrumArea);
    g.setColour (bg);
    g.fillRectList (uncovered);

    if (traces.isValid())
        blit (traces, scopeArea);

    const int view = (int) param ("view");
    const int trigMode = (int) param ("trigMode");
    const auto a = scopeArea.toFloat();

    if (view != 5 && trigMode != 0)
    {
        const float y = a.getCentreY() - param ("trigLevel") * verticalScale();
        if (y >= a.getY() && y <= a.getBottom())
        {
            const float dashes[] = { 4.0f, 4.0f };
            g.setColour (amber.withAlpha (0.7f));
            g.drawDashedLine ({ a.getX(), y, a.getRight(), y }, dashes, 2, 1.0f);
            g.fillRect (a.getX(), y - 4.0f, 6.0f, 8.0f);
        }
    }

    // top-left: timebase / status
    g.setFont (font (12.0f));
    const float timeMs = param ("time", 20.0f);
    juce::String info = view == 5 ? juce::String ("XY  ") + juce::String (timeMs, 1) + " ms"
                                  : juce::String (timeMs / 10.0f, timeMs < 10.0f ? 2 : 1) + " ms/div";
    info << "   x" << juce::String (zoom(), 2);
    g.setColour (dim);
    g.drawText (info, scopeArea.reduced (10, 8), juce::Justification::topLeft);

    juce::String status;
    juce::Colour statusColour = dim;
    if (param ("freeze") > 0.5f)          { status = "FROZEN";  statusColour = blue; }
    else if (view == 5 || trigMode == 0)  { status = "FREE RUN"; }
    else if (triggered)                   { status = "TRIG'D";  statusColour = green; }
    else                                  { status = "NO TRIG"; statusColour = amber; }
    g.setColour (statusColour);
    g.drawText (status, scopeArea.reduced (10, 8).translated (0, 16), juce::Justification::topLeft);

    // top-right: measurements
    juce::String meas;
    meas << "PEAK " << dbString (peakDb) << " dB    RMS " << dbString (rmsDb) << " dB    ";
    meas << (freqHz > 0.0f ? (freqHz >= 1000.0f ? juce::String (freqHz / 1000.0f, 2) + " kHz"
                                                 : juce::String (freqHz, 1) + " Hz")
                           : juce::String ("-- Hz"));
    g.setColour (text);
    g.drawText (meas, scopeArea.reduced (10, 8), juce::Justification::topRight);

    // bottom-left legend
    const int source = (int) param ("source", 2.0f);
    juce::String legend;
    if (source == 0)      legend = "showing: INPUT";
    else if (source == 1) legend = "showing: OUTPUT";
    else                  legend = "dim = input   bright = output";
    if (view == 0)        legend << "   |   green/amber = L   blue/pink = R";
    g.setColour (dim);
    g.setFont (font (11.0f));
    g.drawText (legend, scopeArea.reduced (10, 6), juce::Justification::bottomLeft);
    if (view != 5 && proc != nullptr)
        g.drawText ("drag to set trigger  -  double-click resets", scopeArea.reduced (10, 6), juce::Justification::bottomRight);

    if (spectrumShown && spectrum.isValid())
    {
        blit (spectrum, spectrumArea);
        g.setFont (font (10.0f));
        g.setColour (dim);
        g.drawText ("SPECTRUM", spectrumArea.reduced (8, 4), juce::Justification::topLeft);
    }
    if (slitShown)
        paintSlit (g);
}

void ScopeView::renderSlitField()
{
    if (! slitField.isValid())
        return;

    const auto s = slitSettings();
    const float W = (float) slitArea.getWidth(), H = (float) slitArea.getHeight();
    const float twoPi = juce::MathConstants<float>::twoPi;

    // Schematic, not to scale: keep the real d/wavelength ratio while fitting the slits on screen
    auto& lay = slitLayout;
    const float ratio = DoubleSlitEngine::displayRatio (s);
    lay.lambda = 12.0f;
    const float maxSep = H * 0.6f;
    if (ratio * lay.lambda > maxSep)
        lay.lambda = juce::jmax (3.0f, maxSep / ratio);
    lay.sep = juce::jmin (ratio * lay.lambda, maxSep);
    lay.width = juce::jmax (2.0f, s.width * lay.sep);
    lay.barrierX = W * 0.22f;
    lay.screenX = W * 0.84f;
    const float sinT = DoubleSlitEngine::earSinTheta (s);
    lay.earHalf = (lay.screenX - lay.barrierX) * sinT / std::sqrt (1.0f - sinT * sinT);

    // brightness follows the signal level so the waves pulse with the audio
    const float target = 0.25f + 0.75f * juce::jlimit (0.0f, 1.0f, (rmsDb + 60.0f) / 60.0f);
    waveLevel += (target - waveLevel) * 0.15f;
    wavePhase = std::fmod (wavePhase + 0.12f, twoPi);

    const float k = twoPi / lay.lambda;
    const float cy = H * 0.5f;
    const float slitY[2] = { cy - lay.sep * 0.5f, cy + lay.sep * 0.5f };
    const float barrierPhase = k * lay.barrierX;
    const float cellW = W / (float) slitField.getWidth(), cellH = H / (float) slitField.getHeight();

    const juce::Colour base (Palette::viewBg);
    juce::Image::BitmapData px (slitField, juce::Image::BitmapData::writeOnly);
    for (int j = 0; j < slitField.getHeight(); ++j)
    {
        const float y = (j + 0.5f) * cellH;
        for (int i = 0; i < slitField.getWidth(); ++i)
        {
            const float x = (i + 0.5f) * cellW;
            float v = 0.0f;
            if (x < lay.barrierX)
                v = std::cos (k * x - wavePhase);
            else if (x < lay.screenX)
            {
                for (float sy : slitY)
                {
                    const float r = std::hypot (x - lay.barrierX, y - sy);
                    v += std::cos (k * r + barrierPhase - wavePhase) / std::sqrt (1.0f + r / lay.lambda);
                }
                v *= 1.3f;
            }

            const float a = juce::jlimit (0.0f, 1.0f, std::abs (v) * waveLevel);
            px.setPixelColour (i, j, base.interpolatedWith (v > 0.0f ? Palette::amber : Palette::blue, a * 0.85f));
        }
    }
}

void ScopeView::paintSlit (juce::Graphics& g)
{
    using namespace Palette;
    const auto a = slitArea.toFloat();
    const auto& lay = slitLayout;
    const float cy = a.getCentreY();

    g.saveState();
    g.reduceClipRegion (slitArea);
    g.setColour (viewBg);
    g.fillRoundedRectangle (a, 6.0f);
    g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
    g.drawImage (slitField, a);

    // barrier with two openings
    const float bx = a.getX() + lay.barrierX;
    const float gaps[2][2] = { { cy - lay.sep * 0.5f - lay.width * 0.5f, cy - lay.sep * 0.5f + lay.width * 0.5f },
                               { cy + lay.sep * 0.5f - lay.width * 0.5f, cy + lay.sep * 0.5f + lay.width * 0.5f } };
    g.setColour (dim.brighter (0.12f));   // the barrier
    g.fillRect (bx - 2.0f, a.getY(), 4.0f, juce::jmax (0.0f, gaps[0][0] - a.getY()));
    g.fillRect (bx - 2.0f, gaps[0][1], 4.0f, juce::jmax (0.0f, gaps[1][0] - gaps[0][1]));
    g.fillRect (bx - 2.0f, gaps[1][1], 4.0f, juce::jmax (0.0f, a.getBottom() - gaps[1][1]));

    // screen + the part of it the ear covers
    const float sx = a.getX() + lay.screenX;
    g.setColour (dim);
    g.drawVerticalLine ((int) sx, a.getY(), a.getBottom());
    g.setColour (green);
    g.fillRect (sx - 1.5f, juce::jmax (a.getY(), cy - lay.earHalf), 3.0f,
                juce::jmin (a.getBottom(), cy + lay.earHalf) - juce::jmax (a.getY(), cy - lay.earHalf));

    // intensity across the screen (two slits with finite width)
    const float k = juce::MathConstants<float>::twoPi / lay.lambda;
    const float L = lay.screenX - lay.barrierX;
    const int rows = slitArea.getHeight();
    std::vector<float> intensity ((size_t) rows);
    float peak = 1.0e-6f;
    for (int row = 0; row < rows; ++row)
    {
        const float y = (float) row - (float) rows * 0.5f;
        float re = 0.0f, im = 0.0f;
        for (int side = -1; side <= 1; side += 2)
            for (int j = 0; j < 5; ++j)
            {
                const float ys = side * lay.sep * 0.5f + (j / 4.0f - 0.5f) * lay.width;
                const float r = std::hypot (L, y - ys);
                re += std::cos (k * r) / std::sqrt (r);
                im += std::sin (k * r) / std::sqrt (r);
            }
        intensity[(size_t) row] = re * re + im * im;
        peak = juce::jmax (peak, intensity[(size_t) row]);
    }
    juce::Path curve;
    const float maxW = a.getRight() - sx - 6.0f;
    for (int row = 0; row < rows; ++row)
    {
        const float x = sx + 3.0f + maxW * intensity[(size_t) row] / peak;
        const float y = a.getY() + (float) row;
        if (row == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
    }
    g.setColour (amber);
    g.strokePath (curve, juce::PathStrokeType (1.2f));

    // labels
    const auto s = slitSettings();
    juce::String geo;
    if (s.sizing == DoubleSlitEngine::perFrequency)
        geo << "d = " << juce::String (s.multiple, 1) << " wl (every frequency)";
    else if (s.sizing == DoubleSlitEngine::followPitch)
        geo << "d = " << juce::String (s.multiple, 1) << " wl of "
            << juce::String (slit != nullptr ? slit->pitchUsedHz.load() : s.pitchHz, 0) << " Hz";
    else
        geo << "d = " << juce::String (s.separation, 2) << " m";
    geo << "   a = " << juce::String (s.width, 2) << " d   n = " << s.fringes;

    juce::String path;
    path << "L = " << juce::String (s.distance, 0) << " m";
    if (slit != nullptr)   // the engine's live stats (a friend's tile has only their settings)
    {
        path << "   ear " << juce::String (slit->earWidthM.load(), 1) << " m   spread "
             << juce::String (slit->spreadMs.load(), 1) << " ms";
        if (slit->truncated.load()) path << " (cut at 2.7 s)";
        if (slit->building.load())  path << "   building...";
    }

    g.setFont (font (11.0f, true));
    g.setColour (text);
    g.drawText ("DOUBLE SLIT", slitArea.reduced (8, 5), juce::Justification::topLeft);
    g.setFont (font (10.5f));
    g.setColour (text.withAlpha (0.85f));
    g.drawText (geo, slitArea.reduced (8, 5).withTrimmedBottom (14), juce::Justification::bottomLeft);
    g.drawText (path, slitArea.reduced (8, 5), juce::Justification::bottomLeft);
    g.restoreState();
}

void ScopeView::setTriggerFromY (float y)
{
    if (auto* p = proc != nullptr ? proc->apvts.getParameter ("trigLevel") : nullptr)
    {
        const float level = juce::jlimit (-1.0f, 1.0f, ((float) scopeArea.getCentreY() - y) / verticalScale());
        p->setValueNotifyingHost (p->convertTo0to1 (level));
    }
}

void ScopeView::mouseDown (const juce::MouseEvent& e)
{
    dragging = proc != nullptr && scopeArea.contains (e.getPosition()) && (int) param ("view") != 5;
    if (dragging)
    {
        proc->apvts.getParameter ("trigLevel")->beginChangeGesture();
        setTriggerFromY ((float) e.y);
    }
}

void ScopeView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging)
        setTriggerFromY ((float) e.y);
}

void ScopeView::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
        proc->apvts.getParameter ("trigLevel")->endChangeGesture();
    dragging = false;
}

void ScopeView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (proc != nullptr && scopeArea.contains (e.getPosition()))
        if (auto* p = proc->apvts.getParameter ("trigLevel"))
            p->setValueNotifyingHost (p->getDefaultValue());
}

void ScopeView::visibilityChanged()
{
    // the 2D scope only does work while it is the visible main view
    if (isVisible() && fps > 0)
        startTimerHz (fps);
    else
        stopTimer();
}

