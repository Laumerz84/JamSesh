#include "JamView.h"
#include "JamSession.h"
#include "JamUiCommon.h"
#include "ViewTile.h"
#include "ViewState.h"
#include "../PluginProcessor.h"
#include "../UI/Theme.h"
#include "../UI/ViewFeed.h"

// The Jam Sesh main view (owner: jam-ui soldier). A header with the JAM SESH title (New Rocker, a
// gradient fill, outline and glow in the Jam accent) and one line of session status; below it one
// tile per player (1..6, laid out for the biggest tiles): a frame in the player's colour with name,
// host / you badges, connection, their chord, level and total latency (tooltip: the breakdown), and
// inside it their own view (ViewTile). Before a session: what Jam Sesh is and the three steps.
// Everything reads the session on the message thread (change messages + a 15 Hz timer).

namespace
{
    //==========================================================================
    // One player's tile: frame + header + footer painted here, their view (ViewTile) inside.
    // Opaque, so the view's frequent repaints never repaint the Jam view behind it.
    class PlayerTile : public juce::Component
    {
    public:
        explicit PlayerTile (ClodScopeProcessor& p)
        {
            setOpaque (true);
            view.setDefaults (&p.apvts);
            addAndMakeVisible (view);
            addAndMakeVisible (latency);
        }

        jam::PeerId id() const { return info.id; }

        // selfFeed: this plugin's own sound, used for your tile when the session has no feed for it
        void update (const jam::PeerInfo& p, jam::Session& session, ClodScopeProcessor& proc, ProcessorFeed& selfFeed)
        {
            const bool frameChanged = first || p.name != info.name || p.colour != info.colour || p.isHost != info.isHost
                                   || p.connected != info.connected || p.chord != info.chord || p.isSelf != info.isSelf
                                   || p.hasTrack != info.hasTrack || p.sendMode != info.sendMode || p.relayed != info.relayed
                                   || std::abs (p.lossPercent - info.lossPercent) > 0.5f;
            first = false;
            info = p;
            latency.set (p);

            // the level meter moves smoothly: fast attack, slower fall
            const float db = p.isSelf ? juce::jmax (p.levelDb, levelOf (selfFeed)) : p.levelDb;
            const float target = juce::jlimit (-60.0f, 0.0f, p.connected || p.isSelf ? db : -100.0f);
            const float prev = level;
            level = target > level ? target : level + (target - level) * 0.35f;

            ViewFeed* f = p.id != jam::noPeer ? session.feedFor (p.id) : nullptr;
            if (f == nullptr && p.isSelf)
                f = &selfFeed;
            if (f != feed)
            {
                feed = f;
                view.setFeed (f);
            }
            ViewState vs;
            const bool got = p.id != jam::noPeer && session.viewStateFor (p.id, vs);
            if (! got && p.isSelf)
                vs = ViewState::capture (proc);   // your own tile shows your own view
            if ((got || p.isSelf) && (! haveState || ! vs.sameAs (state)))
            {
                state = vs;
                haveState = true;
                view.setViewState (vs);
            }

            if (frameChanged)
                repaint();
            else if (std::abs (prev - level) > 0.2f)
                repaint (footer);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Palette::viewBg);
            auto r = getLocalBounds().toFloat().reduced (3.0f);
            const auto c = colourOf();
            const bool live = info.connected || info.isSelf;

            // glow and frame in the player's colour
            for (int i = 3; i >= 1; --i)
            {
                g.setColour (c.withAlpha (live ? 0.06f * (float) (4 - i) : 0.02f));
                g.drawRoundedRectangle (r.expanded ((float) i * 0.9f), 9.0f, 1.6f);
            }
            g.setColour (Palette::panel);
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (live ? c : c.withAlpha (0.35f));
            g.drawRoundedRectangle (r, 8.0f, 2.0f);

            // header: colour chip, name, badges, connection
            auto h = header.reduced (2, 0);
            g.setColour (c);
            g.fillEllipse (h.removeFromLeft (12).withSizeKeepingCentre (10, 10).toFloat());
            h.removeFromLeft (6);
            g.setColour (Palette::text);
            g.setFont (Theme::font (14.0f, true));
            const juce::String name = info.name.isNotEmpty() ? info.name : juce::String ("Player ") + juce::String ((int) info.id);
            const int nameW = juce::jmin (h.getWidth() / 2, juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name) + 4);
            g.drawFittedText (name, h.removeFromLeft (nameW), juce::Justification::centredLeft, 1, 0.8f);

            auto badge = [&g, &h] (const juce::String& t, juce::Colour col, bool filled)
            {
                const auto f = Theme::font (10.0f, true);
                const int w = juce::GlyphArrangement::getStringWidthInt (f, t) + 12;
                if (w > h.getWidth() - 70)
                    return;
                h.removeFromLeft (6);
                auto b = h.removeFromLeft (w).withSizeKeepingCentre (w, 16).toFloat();
                g.setColour (filled ? col : col.withAlpha (0.15f));
                g.fillRoundedRectangle (b, 8.0f);
                g.setColour (filled ? Palette::viewBg : col);
                g.setFont (f);
                g.drawText (t, b, juce::Justification::centred);
            };
            if (info.isHost)   badge ("HOST", Accent::jam, true);
            if (info.isSelf)   badge ("YOU", Palette::text, false);
            if (info.hasTrack) badge ("TRACK", Palette::violet, false);
            if (info.sendMode == jam::SendMode::rawInput && ! info.isSelf)
                badge ("RAW", Palette::blue, false);
            if (info.relayed && ! info.isSelf)
                badge ("RELAY", Palette::amber, false);   // no direct path: through the host, more delay

            // connection state, right aligned
            {
                const bool lossy = live && ! info.isSelf && info.lossPercent >= 1.0f;
                const juce::String t = ! live ? juce::String ("OFFLINE")
                                     : lossy ? juce::String (info.lossPercent, 0) + "% LOST" : juce::String();
                if (t.isNotEmpty())
                {
                    g.setFont (Theme::font (10.0f, true));
                    g.setColour (! live ? Palette::pink : Palette::amber);
                    g.drawText (t, header.reduced (4, 0), juce::Justification::centredRight);
                }
            }

            // footer: chord (big, in their colour) and the level meter; the latency badge is a child
            auto f = footer;
            const bool hasChord = info.chord.isNotEmpty();
            g.setFont (Theme::font (18.0f, true));
            g.setColour (hasChord ? c : Palette::dim.withAlpha (0.6f));
            g.drawFittedText (hasChord ? info.chord : juce::String ("--"), f.removeFromLeft (juce::jmin (70, f.getWidth() / 4)),
                              juce::Justification::centredLeft, 1, 0.6f);
            f.removeFromRight (latency.getWidth() + 10);
            auto m = f.withSizeKeepingCentre (f.getWidth(), 6).toFloat();
            g.setColour (Palette::grid);
            g.fillRoundedRectangle (m, 3.0f);
            const float frac = juce::jlimit (0.0f, 1.0f, (level + 60.0f) / 60.0f);
            if (frac > 0.0f)
            {
                juce::ColourGradient grad (c.withAlpha (0.6f), m.getX(), 0.0f, level > -6.0f ? Palette::pink : c, m.getRight(), 0.0f, false);
                g.setGradientFill (grad);
                g.fillRoundedRectangle (m.withWidth (juce::jmax (6.0f, m.getWidth() * frac)), 3.0f);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (10, 6);
            header = r.removeFromTop (24);
            footer = r.removeFromBottom (28);
            r.removeFromBottom (4);
            r.removeFromTop (2);
            view.setBounds (r);
            latency.setBounds (footer.withTrimmedLeft (footer.getWidth() - 70).withSizeKeepingCentre (70, 20));
        }

        void lookAndFeelChanged() override { repaint(); }

    private:
        static float levelOf (const ViewFeed& f)
        {
            float l[512], r[512];
            const int n = f.copyLatest (true, l, r, 512);
            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
                peak = juce::jmax (peak, std::abs (l[i]), std::abs (r[i]));
            return juce::Decibels::gainToDecibels (peak, -100.0f);
        }

        juce::Colour colourOf() const
        {
            return info.colour.isTransparent() ? Accent::jam : info.colour;
        }

        jam::PeerInfo info;
        ViewTile view;
        jamui::LatencyBadge latency;
        ViewFeed* feed = nullptr;
        ViewState state;
        bool haveState = false, first = true;
        float level = -60.0f;
        juce::Rectangle<int> header, footer;
    };

    //==========================================================================
    // Before a session: what Jam Sesh is, and the three steps
    constexpr int introHeight = 84, cardHeight = 128, failedHeight = 38;

    void paintExplainer (juce::Graphics& g, juce::Rectangle<int> area, jam::Status status, const juce::String& statusText)
    {
        const bool compact = area.getHeight() < 300;
        auto intro = area.removeFromTop (introHeight);
        g.setColour (Palette::text);
        g.setFont (Theme::font (compact ? 15.0f : 17.0f, true));
        g.drawText ("Play together online, live.", intro.removeFromTop (28), juce::Justification::centredLeft);
        g.setColour (Palette::dim);
        g.setFont (Theme::font (12.5f));
        g.drawFittedText ("One of you hosts, the others join with a code. Everyone hears everyone as they play: "
                          "about 20-35 ms in the same city, like standing across a big room; more the further apart you are. "
                          "Far away? Switch on one-bar mode and everyone plays exactly one bar behind, in time.",
                          intro, juce::Justification::topLeft, 3, 1.0f);

        if (status == jam::Status::failed)
        {
            auto e = area.removeFromTop (30);
            g.setColour (Palette::pink.withAlpha (0.12f));
            g.fillRoundedRectangle (e.toFloat(), 6.0f);
            g.setColour (Palette::pink);
            g.setFont (Theme::font (12.0f, true));
            g.drawFittedText (statusText, e.reduced (10, 0), juce::Justification::centredLeft, 1, 0.8f);
            area.removeFromTop (8);
        }

        struct Step { const char* title; const char* body; };
        const Step steps[] = {
            { "HOST OR JOIN", "Open the Jam Sesh page on the right. One player presses Host. Everyone else pastes "
                              "the host's code and presses Join. Pick your name first: friends see it on your tile." },
            { "SHARE THE CODE", "The host presses Copy next to the session code and sends it by chat. The code holds "
                                "the host's address and port, nothing else." },
            { "ROUTER OR VPN", "Same house: nothing to do. Over the internet the host forwards UDP port 47800 on the "
                               "router to their PC, or everyone joins one Tailscale / ZeroTier network and the host "
                               "shares the VPN address." } };

        const int gap = 10, n = 3;
        const bool row = area.getWidth() >= 620;
        const int cardH = row ? juce::jmin (area.getHeight(), cardHeight)
                              : juce::jmin ((area.getHeight() - 2 * gap) / n, 110);
        auto cards = area.removeFromTop (row ? cardH : n * cardH + 2 * gap);
        const int cardW = row ? (cards.getWidth() - 2 * gap) / n : cards.getWidth();
        for (int i = 0; i < n; ++i)
        {
            auto c = row ? cards.removeFromLeft (cardW) : cards.removeFromTop (cardH);
            if (row) cards.removeFromLeft (gap); else cards.removeFromTop (gap);

            g.setColour (Palette::panel);
            g.fillRoundedRectangle (c.toFloat(), 8.0f);
            g.setColour (Palette::panelEdge);
            g.drawRoundedRectangle (c.toFloat().reduced (0.5f), 8.0f, 1.0f);
            g.setColour (Accent::jam);
            g.fillRoundedRectangle (c.toFloat().removeFromLeft (3.0f).reduced (0.0f, 10.0f), 1.5f);

            auto in = c.reduced (14, 10);
            auto top = in.removeFromTop (32);
            g.setFont (jamui::titleFont (28.0f));
            g.setColour (Accent::jam);
            g.drawText (juce::String (i + 1), top.removeFromLeft (26), juce::Justification::centredLeft);
            g.setColour (Palette::text);
            g.setFont (Theme::font (12.5f, true));
            g.drawText (steps[i].title, top, juce::Justification::centredLeft);
            g.setColour (Palette::dim);
            g.setFont (Theme::font (12.0f));
            g.drawFittedText (steps[i].body, in, juce::Justification::topLeft, juce::jmax (1, in.getHeight() / 15), 1.0f);
        }
    }
}

//==============================================================================
struct JamView::Impl : private juce::ChangeListener, private juce::Timer
{
    Impl (JamView& v, ClodScopeProcessor& p) : view (v), proc (p), selfFeed (p)
    {
        preview = std::make_unique<PlayerTile> (p);
        view.addChildComponent (*preview);
        session->addChangeListener (this);
        refresh();
        startTimerHz (15);
    }

    ~Impl() override
    {
        stopTimer();
        session->removeChangeListener (this);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override
    {
        if (jamui::visibleInTree (view))
            refresh();
    }

    void refresh()
    {
        auto& s = *session;
        status = s.status();
        const auto text = s.statusText();
        const auto code = s.sessionCode();
        const auto opts = s.options();
        const double bpm = s.sessionBpm();
        // outside a session the list may still hold yourself: show the explainer instead of tiles
        auto players = jamui::inSession (s.status()) ? s.players() : jamui::fakePlayers();
        if (players.size() > (size_t) jam::maxPlayers)
            players.resize ((size_t) jam::maxPlayers);

        // tiles follow the player list (same order: self first)
        bool layoutChanged = players.size() != (size_t) tiles.size();
        for (size_t i = 0; ! layoutChanged && i < players.size(); ++i)
            layoutChanged = tiles[(int) i]->id() != players[i].id;
        if (layoutChanged)
        {
            tiles.clear();
            for (size_t i = 0; i < players.size(); ++i)
                view.addAndMakeVisible (tiles.add (new PlayerTile (proc)));
        }
        for (size_t i = 0; i < players.size(); ++i)
            tiles[(int) i]->update (players[i], s, proc, selfFeed);

        // before a session: a preview of your own tile (what friends will see of you)
        if (preview != nullptr)
        {
            preview->setVisible (tiles.isEmpty() && ! previewArea.isEmpty());
            if (preview->isVisible())
            {
                jam::PeerInfo me;
                me.name = settings->name();
                me.colour = Accent::jam;
                me.isSelf = true;
                preview->update (me, s, proc, selfFeed);
            }
        }

        // the header line
        juce::String line;
        if (jamui::inSession (status))
        {
            const int friends = juce::jmax (0, (int) players.size() - 1);
            line << jamui::statusName (status);
            if (code.isNotEmpty())
                line << "   |   CODE " << code;
            line << "   |   " << friends << (friends == 1 ? " FRIEND" : " FRIENDS")
                 << "   |   " << juce::String (bpm, 0) << " BPM";
            if (opts.oneBarMode)
                line << "   |   ONE-BAR MODE: everyone " << jamui::ms (jamui::estimate (opts, 0.0f, bpm, 2).oneBarMs) << " behind, in time";
        }
        else
        {
            line = status == jam::Status::failed ? juce::String ("COULD NOT START THE SESSION") : juce::String ("PLAY TOGETHER, LIVE");
        }

        const bool headChanged = line != headLine || text != statusText;
        headLine = line;
        statusText = text;
        if (status != lastStatus || layoutChanged)
        {
            lastStatus = status;
            view.resized();
            view.repaint();
        }
        else if (headChanged)
        {
            view.repaint (status == jam::Status::failed ? view.getLocalBounds() : headerArea);
        }
    }

    void layout()
    {
        auto r = view.getLocalBounds();
        const int headH = juce::jlimit (84, 120, r.getHeight() / 7);
        headerArea = r.removeFromTop (headH);
        bodyArea = r.reduced (14, 6).withTrimmedBottom (6);

        // explainer above, the preview of your tile below it when there is room
        const int used = 4 + introHeight + (status == jam::Status::failed ? failedHeight : 0) + cardHeight + 16;
        previewArea = bodyArea.withTrimmedTop (used + 22).reduced (6, 0);
        if (previewArea.getHeight() < 150 || bodyArea.getWidth() < 620)
            previewArea = {};
        else
            previewArea = previewArea.withSizeKeepingCentre (juce::jmin (previewArea.getWidth(), (int) ((float) previewArea.getHeight() * 2.2f)),
                                                             previewArea.getHeight());
        if (preview != nullptr)
        {
            preview->setBounds (previewArea);
            preview->setVisible (tiles.isEmpty() && ! previewArea.isEmpty());
        }

        const int n = tiles.size();
        if (n == 0)
            return;

        // the grid that gives the biggest tiles near a 3:2 shape
        int bestCols = 1;
        float best = -1.0f;
        for (int cols = 1; cols <= n; ++cols)
        {
            const int rows = (n + cols - 1) / cols;
            const float w = (float) bodyArea.getWidth() / (float) cols, h = (float) bodyArea.getHeight() / (float) rows;
            const float score = juce::jmin (w / 1.5f, h);
            if (score > best + 0.5f)
            {
                best = score;
                bestCols = cols;
            }
        }
        const int cols = bestCols, rows = (n + cols - 1) / cols;
        const int gap = 10;
        const int tw = (bodyArea.getWidth() - gap * (cols - 1)) / cols;
        const int th = (bodyArea.getHeight() - gap * (rows - 1)) / rows;
        for (int i = 0; i < n; ++i)
        {
            const int row = i / cols, col = i % cols;
            const int inRow = juce::jmin (cols, n - row * cols);
            const int x0 = bodyArea.getX() + (bodyArea.getWidth() - (inRow * tw + (inRow - 1) * gap)) / 2;   // centre a short last row
            tiles[i]->setBounds (x0 + col * (tw + gap), bodyArea.getY() + row * (th + gap), tw, th);
        }
    }

    // JAM SESH: gradient fill, light outline and an outer glow, slanted forward
    void paintTitle (juce::Graphics& g, juce::Rectangle<int> area)
    {
        const float height = juce::jlimit (40.0f, 78.0f, (float) area.getHeight() * 0.62f);
        if (std::abs (height - titleHeight) > 0.5f || titlePath.isEmpty())
        {
            titleHeight = height;
            juce::GlyphArrangement ga;
            ga.addLineOfText (jamui::titleFont (height), "JAM SESH", 0.0f, 0.0f);
            titlePath.clear();
            ga.createPath (titlePath);
            titlePath.applyTransform (juce::AffineTransform::shear (-0.12f, 0.0f));
        }

        const auto b = titlePath.getBounds();
        const float x = (float) area.getX() + 22.0f - b.getX();
        const float y = (float) area.getCentreY() - b.getCentreY() - (float) area.getHeight() * 0.10f;
        juce::Path p (titlePath);
        p.applyTransform (juce::AffineTransform::translation (x, y));
        const auto pb = p.getBounds();

        // stage light behind the title
        {
            juce::ColourGradient glow (Accent::jam.withAlpha (0.22f), pb.getCentreX(), pb.getCentreY(),
                                       Accent::jam.withAlpha (0.0f), pb.getCentreX() + pb.getWidth() * 0.9f, pb.getCentreY(), true);
            // circular gradient squashed to the title's shape, so it fades out on every side
            const float k = pb.getHeight() / juce::jmax (1.0f, pb.getWidth());
            g.setFillType (juce::FillType (glow).transformed (juce::AffineTransform::scale (1.0f, k * 1.6f, pb.getCentreX(), pb.getCentreY())));
            g.fillEllipse (pb.withSizeKeepingCentre (pb.getWidth() * 1.8f, pb.getHeight() * 1.8f * 1.6f));
        }
        for (float w : { 14.0f, 8.0f, 4.0f })
        {
            g.setColour (Accent::jam.withAlpha (w > 10.0f ? 0.07f : w > 6.0f ? 0.12f : 0.22f));
            g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        juce::ColourGradient fill (Palette::amber, 0.0f, pb.getY(), Accent::jam, 0.0f, pb.getBottom(), false);
        fill.addColour (0.85, Palette::pink.interpolatedWith (Accent::jam, 0.5f));
        g.setGradientFill (fill);
        g.fillPath (p);
        g.setColour (Palette::text.withAlpha (0.85f));
        g.strokePath (p, juce::PathStrokeType (1.2f));

        // speed stripes after the title
        const float sx = pb.getRight() + 16.0f;
        const float right = (float) area.getRight() - 18.0f;
        if (right - sx > 60.0f)
        {
            for (int i = 0; i < 3; ++i)
            {
                const float yy = pb.getY() + pb.getHeight() * (0.30f + 0.2f * (float) i);
                const float len = (right - sx) * (1.0f - 0.25f * (float) i);
                juce::ColourGradient sg (Accent::jam.withAlpha (0.55f - 0.15f * (float) i), sx, yy,
                                         Accent::jam.withAlpha (0.0f), sx + len, yy, false);
                g.setGradientFill (sg);
                g.fillRect (juce::Rectangle<float> (sx + 8.0f * (float) i, yy, len, 3.0f - (float) i * 0.6f));
            }
        }

        // the status line under the title
        auto line = area.withTop ((int) pb.getBottom() + 6).withHeight (18).withTrimmedLeft (22);
        g.setFont (Theme::font (11.5f, true));
        g.setColour (status == jam::Status::idle ? Palette::dim : jamui::statusColour (status));
        g.drawFittedText (headLine, line, juce::Justification::centredLeft, 1, 0.75f);
    }

    void paint (juce::Graphics& g)
    {
        g.fillAll (Palette::viewBg);
        const auto all = view.getLocalBounds().toFloat();

        // a fretboard highway fading in from the title
        {
            const float vx = all.getCentreX(), vy = (float) headerArea.getBottom() - 20.0f;
            for (int i = -6; i <= 6; ++i)
            {
                const float bx = vx + (float) i * all.getWidth() * 0.16f;
                juce::ColourGradient lg (Accent::jam.withAlpha (0.0f), vx, vy, Accent::jam.withAlpha (0.08f), bx, all.getBottom(), false);
                g.setGradientFill (lg);
                juce::Path l;
                l.startNewSubPath (vx + (float) i * 6.0f, vy);
                l.lineTo (bx, all.getBottom());
                g.strokePath (l, juce::PathStrokeType (1.2f));
            }
        }
        g.setColour (Accent::jam.withAlpha (0.25f));
        g.fillRect ((float) headerArea.getX() + 14.0f, (float) headerArea.getBottom() - 1.0f, (float) headerArea.getWidth() - 28.0f, 1.0f);

        paintTitle (g, headerArea);
        if (tiles.isEmpty())
        {
            paintExplainer (g, bodyArea.reduced (6, 4), status, statusText);
            if (! previewArea.isEmpty())
            {
                g.setColour (Palette::dim);
                g.setFont (Theme::font (11.0f, true));
                g.drawText ("YOUR TILE  -  what your friends will see of you (your view, your sound)",
                            previewArea.withY (previewArea.getY() - 22).withHeight (18), juce::Justification::centredLeft);
            }
        }
    }

    JamView& view;
    ClodScopeProcessor& proc;
    juce::SharedResourcePointer<jam::Session> session;
    juce::SharedResourcePointer<jamui::Settings> settings;
    ProcessorFeed selfFeed;
    juce::OwnedArray<PlayerTile> tiles;
    std::unique_ptr<PlayerTile> preview;
    jam::Status status = jam::Status::idle, lastStatus = jam::Status::idle;
    juce::String headLine, statusText;
    juce::Rectangle<int> headerArea, bodyArea, previewArea;
    juce::Path titlePath;
    float titleHeight = 0.0f;
};

//==============================================================================
JamView::JamView (ClodScopeProcessor& p)
{
    setOpaque (true);
    impl = std::make_unique<Impl> (*this, p);
}

JamView::~JamView() = default;

void JamView::paint (juce::Graphics& g)
{
    if (impl != nullptr)
        impl->paint (g);
}

void JamView::resized()
{
    if (impl != nullptr)
        impl->layout();
}

void JamView::lookAndFeelChanged()
{
    repaint();
}
