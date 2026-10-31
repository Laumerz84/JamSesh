#include "JamMetronome.h"
#include "JamSession.h"
#include "JamNet.h"
#include "../UI/Theme.h"
#include "../UI/Widgets.h"
#include <cmath>

// Owner: metro soldier. See JamMetronome.h.
namespace jam
{
    namespace
    {
        constexpr double leadSeconds = 0.35;     // switched on: the first bar starts this much later, so friends hear it too
        constexpr double clickSeconds = 0.07;

        // A short wooden tick: a sine with a fast attack and a quick decay, plus a brighter partial that dies faster.
        // The bar's first beat is higher and louder.
        inline float clickSample (int pos, bool accent, double sampleRate) noexcept
        {
            const double t = pos / sampleRate;
            const double f = accent ? 1760.0 : 1320.0;
            const double a = accent ? 0.62 : 0.42;
            const double env = juce::jmin (1.0, t / 0.0004) * std::exp (-t / 0.011);
            const double w = juce::MathConstants<double>::twoPi * f * t;
            return (float) (a * env * (std::sin (w) + 0.3 * std::sin (2.71 * w) * std::exp (-t / 0.003)));
        }

        // visible with every parent (as the Jam UI's timers check; also true off screen, e.g. in snapshots)
        bool visibleInTree (const juce::Component& c)
        {
            for (auto* p = &c; p != nullptr; p = p->getParentComponent())
                if (! p->isVisible())
                    return false;
            return true;
        }
    }

    //==============================================================================================
    BeatGrid Metronome::ownGrid (double bpm, double now) noexcept
    {
        bpm = juce::jlimit (20.0, 999.0, bpm > 0.0 ? bpm : 120.0);
        if (! switchedOn)
        {
            own = { false, beats, bpm, now };
            return own;
        }
        if (! own.on)
            own = { true, beats, bpm, now + leadSeconds };      // a bar starts a moment from now, for everyone

        if (bpm != own.bpm)
        {
            const double counted = (now - own.anchor) * own.bpm / 60.0;   // the beats keep counting through a tempo change
            own.anchor = now - counted * 60.0 / bpm;
            own.bpm = bpm;
        }
        if (beats != own.beatsPerBar)
        {
            // the next beat starts a bar (the beats stay where they are)
            own.anchor += std::ceil ((now - own.anchor) * own.bpm / 60.0 - 1.0e-9) * 60.0 / own.bpm;
            own.beatsPerBar = beats;
        }

        // the host program's transport plays: its beats and bars (beat 0 = its bar 1)
        if (dawLive.load (std::memory_order_acquire) && std::abs (dawBpmSeen.load (std::memory_order_relaxed) - bpm) < 1.0e-6)
        {
            const double bar = own.beatsPerBar * 60.0 / own.bpm;
            const double d = std::remainder (dawAnchor.load (std::memory_order_relaxed) - own.anchor, bar);
            if (std::abs (d) > 0.0015)
            {
                own.anchor += d;
                if (own.anchor > now)
                    own.anchor -= std::ceil ((own.anchor - now) / bar) * bar;   // no beat waits for it
            }
        }
        return own;
    }

    void Metronome::publish (const BeatGrid& g) noexcept
    {
        const auto c = current();
        if (c.on == g.on && c.beatsPerBar == g.beatsPerBar && c.bpm == g.bpm && c.anchor == g.anchor)
            return;
        version.fetch_add (1, std::memory_order_acq_rel);   // odd: writing
        pubOn.store (g.on, std::memory_order_relaxed);
        pubBeats.store (juce::jlimit (1, 16, g.beatsPerBar), std::memory_order_relaxed);
        pubBpm.store (juce::jlimit (20.0, 999.0, g.bpm), std::memory_order_relaxed);
        pubAnchor.store (g.anchor, std::memory_order_relaxed);
        version.fetch_add (1, std::memory_order_release);   // even: done
    }

    BeatGrid Metronome::current() const noexcept
    {
        BeatGrid g;
        for (int tries = 0; tries < 8; ++tries)
        {
            const auto v0 = version.load (std::memory_order_acquire);
            if ((v0 & 1) != 0)
                continue;
            g.on = pubOn.load (std::memory_order_relaxed);
            g.beatsPerBar = pubBeats.load (std::memory_order_relaxed);
            g.bpm = pubBpm.load (std::memory_order_relaxed);
            g.anchor = pubAnchor.load (std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_acquire);
            if (version.load (std::memory_order_relaxed) == v0)
                return g;
        }
        g.on = false;   // being rewritten right now: no click this block
        return g;
    }

    int Metronome::beatAt (const BeatGrid& g, double now, double* phase) noexcept
    {
        if (! g.on || ! (g.bpm > 0.0))
            return -1;
        const double b = (now - g.anchor) * g.bpm / 60.0;
        if (b < 0.0)
            return -1;   // the first bar has not started
        const double k = std::floor (b);
        if (phase != nullptr)
            *phase = b - k;
        return (int) std::fmod (k, (double) juce::jmax (1, g.beatsPerBar));
    }

    void Metronome::render (float* left, float* right, int n, double sr, double blockSession, double outSeconds,
                            double dawBpm, double dawPpq, bool dawPlaying) noexcept
    {
        if (n <= 0 || ! (sr > 1000.0))
            return;

        // When this block's first sample leaves the speakers, on the session clock. Callbacks start a little early or
        // late; the sound card's clock doesn't: follow the callback times smoothly. A big step is a new start (the
        // device restarted, or the session clock was replaced when joining).
        if (! timeValid || sr != lastRate || std::abs (blockSession - predicted) > 0.02)
        {
            predicted = blockSession;
            timeValid = true;
            lastRate = sr;
            lastClick = -1.0e9;
        }
        else
        {
            predicted += 0.02 * (blockSession - predicted);
        }
        const double t0 = predicted + juce::jmax (0.0, outSeconds);
        const double t1 = t0 + n / sr;
        predicted += n / sr;

        if (dawPlaying && dawBpm >= 20.0 && dawBpm <= 999.0)
        {
            dawAnchor.store (t0 - dawPpq * 60.0 / dawBpm, std::memory_order_relaxed);
            dawBpmSeen.store (dawBpm, std::memory_order_relaxed);
            dawLive.store (true, std::memory_order_release);
        }
        else
        {
            dawLive.store (false, std::memory_order_relaxed);
        }

        // the beats that sound in this block (beats before the grid's first are never played)
        int starts[4];
        bool accents[4];
        int numStarts = 0;
        const auto g = current();
        if (lastClick > t1)
            lastClick = -1.0e9;
        if (g.on)
        {
            const double beat = 60.0 / g.bpm;
            double k = juce::jmax (0.0, std::ceil ((t0 - 0.002 - g.anchor) / beat));
            for (int guard = 0; guard < 8; ++guard, k += 1.0)
            {
                const double tb = g.anchor + k * beat;
                if (tb >= t1)
                    break;
                if (tb <= lastClick + 0.5 * beat)
                    continue;   // played already: consecutive blocks overlap by a hair
                lastClick = tb;
                if (numStarts < 4)
                {
                    starts[numStarts] = juce::jlimit (0, n - 1, (int) std::lround ((tb - t0) * sr));
                    accents[numStarts] = std::fmod (k, (double) juce::jmax (1, g.beatsPerBar)) == 0.0;
                    ++numStarts;
                }
            }
        }

        if (numStarts == 0 && voice.pos < 0)
            return;

        const float vol = mute.load (std::memory_order_relaxed) ? 0.0f : gain.load (std::memory_order_relaxed);
        const int len = (int) (clickSeconds * sr);
        int next = 0;
        for (int i = 0; i < n; ++i)
        {
            while (next < numStarts && starts[next] <= i)
            {
                voice.pos = 0;
                voice.accent = accents[next++];
            }
            if (voice.pos < 0)
                continue;
            const float s = clickSample (voice.pos, voice.accent, sr) * vol;
            if (++voice.pos >= len)
                voice.pos = -1;
            left[i] += s;
            if (right != left)
                right[i] += s;
        }
    }

    HelpList Metronome::helpEntries()
    {
        return {
            { "jam.metro", "Metronome",
              "One click for everyone in the sesh, at the same moment, so you can play in time together.",
              "The host switches it on or off for everyone (friends see the host's choice); the tempo is the sesh tempo "
              "(the host's FL Studio tempo, or the app's BPM box). The click is not sent over the network: every "
              "computer makes its own on the shared beat grid, early by its own sound card's output delay, so it "
              "leaves every speaker together (within about a millisecond). You hear it, nobody else hears yours, and "
              "it is not in what you send or record. Outside a sesh it clicks for you alone, on your own tempo.",
              "While the host's FL Studio plays, the clicks fall on its beats and bars." },
            { "jam.metroBeats", "Beats per bar",
              "How many beats make a bar; the first one clicks higher and louder.",
              "2, 3, 4 or 6. The host sets it for everyone; a change makes the next beat a bar's first.",
              {} },
            { "jam.metroVolume", "Metronome volume",
              "How loud the click is for you (only you hear the change).",
              "Gain from 0 to 1.5; 0.7 is the default. Double-click for 0.7.",
              {} },
            { "jam.metroMute", "Mute metronome",
              "Silences the click for you only; everyone else still hears theirs.",
              "The beat lights keep going.",
              {} },
        };
    }

    //==============================================================================================
    namespace
    {
        void paintLights (juce::Graphics& g, juce::Rectangle<float> area, const BeatGrid& grid, int beat,
                          juce::Colour accent, bool label)
        {
            if (! grid.on)
                return;
            if (label)
            {
                g.setFont (Theme::font (10.5f, true));
                g.setColour (Palette::dim);
                const juce::String t = "CLICK " + juce::String (grid.bpm, grid.bpm == std::round (grid.bpm) ? 0 : 1);
                const float w = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), t) + 8.0f;
                g.drawText (t, area.removeFromLeft (w), juce::Justification::centredLeft);
            }
            const int n = juce::jlimit (1, 16, grid.beatsPerBar);
            const float d = juce::jmin (area.getHeight() * 0.62f, (area.getWidth() - 4.0f * (float) (n - 1)) / (float) n);
            if (d < 3.0f)
                return;
            float x = area.getX();
            for (int i = 0; i < n; ++i, x += d + 4.0f)
            {
                auto r = juce::Rectangle<float> (x, area.getCentreY() - d * 0.5f, d, d);
                const bool lit = i == beat;
                g.setColour ((i == 0 ? accent : Palette::text).withAlpha (lit ? 0.95f : 0.16f));
                g.fillEllipse (r);
                g.setColour ((i == 0 ? accent : Palette::dim).withAlpha (0.7f));
                g.drawEllipse (r.reduced (0.5f), 1.0f);
            }
        }
    }

    struct BeatLights::Impl
    {
        juce::SharedResourcePointer<Session> session;
        int beat = -2;
        BeatGrid shown;
    };

    BeatLights::BeatLights() : impl (std::make_unique<Impl>())
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (40);
    }

    BeatLights::~BeatLights() { stopTimer(); }

    void BeatLights::timerCallback()
    {
        if (! visibleInTree (*this))
            return;
        auto& m = *impl;
        const auto g = m.session->metronome().current();
        // repaints once a beat (the view behind is costly to paint): the lit light is the beat heard now
        const int b = Metronome::beatAt (g, m.session->network().sessionTime());
        if (b != m.beat || g.on != m.shown.on || g.beatsPerBar != m.shown.beatsPerBar || g.bpm != m.shown.bpm)
        {
            m.beat = b;
            m.shown = g;
            repaint();
        }
    }

    void BeatLights::paint (juce::Graphics& g)
    {
        paintLights (g, getLocalBounds().toFloat(), impl->shown, impl->beat, Accent::jam, showLabel);
    }

    //==============================================================================================
    struct MetronomeRow::Impl
    {
        Impl (MetronomeRow& o, HelpTarget* h) : owner (o), help (h) {}

        MetronomeRow& owner;
        HelpTarget* help;
        juce::SharedResourcePointer<Session> session;

        juce::TextButton onBtn, muteBtn;
        juce::OwnedArray<juce::TextButton> beatBtns;
        juce::Slider volume;
        BeatLights lights;
        std::vector<std::unique_ptr<widgets_detail::HoverReporter>> hovers;
        juce::String text;
        juce::Rectangle<int> beatsLabel, volumeLabel, textArea;
        juce::Colour accent = Accent::jam;

        static constexpr int beatChoices[] = { 2, 3, 4, 6 };

        bool leads() const   // this computer's switch decides (host, or not in a sesh)
        {
            const auto st = session->status();
            return st != Status::joining && st != Status::connected;
        }

        void refresh()
        {
            auto& m = session->metronome();
            const auto g = m.current();
            const bool mine = leads();
            const bool on = mine ? m.isOn() : g.on;
            const int beats = mine ? m.beatsPerBar() : g.beatsPerBar;

            onBtn.setToggleState (on, juce::dontSendNotification);
            onBtn.setButtonText (on ? "ON" : "OFF");
            onBtn.setEnabled (mine);
            for (int i = 0; i < beatBtns.size(); ++i)
            {
                beatBtns[i]->setToggleState (beatChoices[i] == beats, juce::dontSendNotification);
                beatBtns[i]->setEnabled (mine);
            }
            if (! volume.isMouseButtonDown())
                volume.setValue (m.volume(), juce::dontSendNotification);
            muteBtn.setToggleState (m.isMuted(), juce::dontSendNotification);

            const auto st = session->status();
            const juce::String tempo = juce::String (g.bpm, g.bpm == std::round (g.bpm) ? 0 : 1) + " BPM, "
                                     + juce::String (g.beatsPerBar) + " beats per bar";
            juce::String t;
            if (st == Status::hosting)
                t = on ? "Everyone in the sesh hears it: " + tempo + ". Each computer makes its own click on the shared clock."
                       : "Off. Switch it on and everyone in the sesh hears the same click at the same moment.";
            else if (! mine)
                t = g.on ? "The host's metronome: " + tempo + ". Your volume and mute are yours alone."
                         : "Off. The host switches it on for everyone (with Jam Sesh 1.4 or newer).";
            else
                t = on ? "Clicking for you alone at " + tempo + " (not in a sesh)."
                       : "Off. In a sesh the host switches it on for everyone; outside one it clicks for you alone.";
            if (t != text)
            {
                text = t;
                owner.repaint (textArea);
            }
        }
    };

    MetronomeRow::MetronomeRow (HelpTarget* help) : impl (std::make_unique<Impl> (*this, help))
    {
        auto& d = *impl;
        auto hover = [&d] (juce::Component& c, const char* id)
        {
            d.hovers.push_back (std::make_unique<widgets_detail::HoverReporter> (c, id, d.help));
            for (auto& e : Metronome::helpEntries())
                if (e.id == id)
                    if (auto* t = dynamic_cast<juce::SettableTooltipClient*> (&c))
                        t->setTooltip (e.summary);
        };

        d.onBtn.setClickingTogglesState (true);
        d.onBtn.onClick = [this]
        {
            impl->session->metronome().setOn (impl->onBtn.getToggleState());
            impl->refresh();
        };
        addAndMakeVisible (d.onBtn);
        hover (d.onBtn, "jam.metro");

        for (int i = 0; i < (int) std::size (Impl::beatChoices); ++i)
        {
            auto* b = d.beatBtns.add (new juce::TextButton (juce::String (Impl::beatChoices[i])));
            const int n = (int) std::size (Impl::beatChoices);
            b->setConnectedEdges ((i > 0 ? (int) juce::Button::ConnectedOnLeft : 0) | (i < n - 1 ? (int) juce::Button::ConnectedOnRight : 0));
            b->onClick = [this, i]
            {
                impl->session->metronome().setBeatsPerBar (Impl::beatChoices[i]);
                impl->refresh();
            };
            addAndMakeVisible (b);
            hover (*b, "jam.metroBeats");
        }

        d.volume.setSliderStyle (juce::Slider::LinearHorizontal);
        d.volume.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        d.volume.setRange (0.0, 1.5, 0.0);
        d.volume.setSkewFactor (0.6);
        d.volume.setDoubleClickReturnValue (true, 0.7);
        d.volume.setPopupDisplayEnabled (true, false, nullptr);
        d.volume.textFromValueFunction = [] (double v)
        {
            return v <= 0.001 ? juce::String ("-inf dB") : juce::String (juce::Decibels::gainToDecibels (v), 1) + " dB";
        };
        d.volume.onValueChange = [this] { impl->session->metronome().setVolume ((float) impl->volume.getValue()); };
        addAndMakeVisible (d.volume);
        hover (d.volume, "jam.metroVolume");

        d.muteBtn.setButtonText ("MUTE");
        d.muteBtn.setClickingTogglesState (true);
        d.muteBtn.onClick = [this] { impl->session->metronome().setMuted (impl->muteBtn.getToggleState()); };
        addAndMakeVisible (d.muteBtn);
        hover (d.muteBtn, "jam.metroMute");

        addAndMakeVisible (d.lights);
        setAccent (Accent::jam);
        d.refresh();
        startTimerHz (10);
    }

    MetronomeRow::~MetronomeRow() { stopTimer(); }

    void MetronomeRow::setAccent (juce::Colour c)
    {
        auto& d = *impl;
        d.accent = c;
        Theme::setAccent (*this, c);
        d.onBtn.setColour (juce::TextButton::buttonOnColourId, c.withAlpha (0.45f));
        for (auto* b : d.beatBtns)
            b->setColour (juce::TextButton::buttonOnColourId, c.withAlpha (0.35f));
        d.muteBtn.setColour (juce::TextButton::buttonOnColourId, Palette::pink.withAlpha (0.45f));
        repaint();
    }

    void MetronomeRow::timerCallback()
    {
        if (visibleInTree (*this))
            impl->refresh();
    }

    int MetronomeRow::idealHeight (int) const
    {
        return 26 + 6 + 24 + 6 + 30;
    }

    void MetronomeRow::resized()
    {
        auto& d = *impl;
        auto r = getLocalBounds();
        auto row1 = r.removeFromTop (26);
        d.onBtn.setBounds (row1.removeFromLeft (64));
        row1.removeFromLeft (12);
        d.beatsLabel = row1.removeFromLeft (44);
        auto seg = row1.removeFromLeft (juce::jmin (150, juce::jmax (80, row1.getWidth() / 2)));
        const int n = d.beatBtns.size();
        for (int i = 0; i < n; ++i)
            d.beatBtns[i]->setBounds (i < n - 1 ? seg.removeFromLeft (seg.getWidth() / (n - i)) : seg);
        row1.removeFromLeft (12);
        d.lights.setBounds (row1.withSizeKeepingCentre (row1.getWidth(), 20));

        r.removeFromTop (6);
        auto row2 = r.removeFromTop (24);
        d.volumeLabel = row2.removeFromLeft (56);
        d.muteBtn.setBounds (row2.removeFromRight (52).withSizeKeepingCentre (52, 22));
        row2.removeFromRight (8);
        d.volume.setBounds (row2);

        r.removeFromTop (6);
        d.textArea = r;
    }

    void MetronomeRow::paint (juce::Graphics& g)
    {
        auto& d = *impl;
        g.setColour (Palette::dim);
        g.setFont (Theme::font (10.5f, true));
        g.drawText ("BEATS", d.beatsLabel, juce::Justification::centredLeft);
        g.drawText ("VOLUME", d.volumeLabel, juce::Justification::centredLeft);
        g.setFont (Theme::font (11.0f));
        g.drawFittedText (d.text, d.textArea, juce::Justification::topLeft, 2, 0.85f);
    }
}
