#include "JamPanel.h"
#include "JamSession.h"
#include "JamPort.h"
#include "JamUiCommon.h"
#include "../PluginProcessor.h"

// The Jam Sesh page (owner: jam-ui soldier). Sections: SESSION (status, the code + Copy, your name,
// Host / Join / Leave), PLAYERS (volume, mute, latency with its breakdown), THIS PLUGIN (player or
// backing track), WHAT YOU SEND (finished sound or raw input), LATENCY (every jam::LatencyOptions
// switch with its trade-off and a live estimate of delay and upload) and ROUTERS & VPNS.
// All control on the message thread; a change listener + 15 Hz timer keep it in step with the session.
// layout() measures and places in one pass, so idealHeight() is exact; explanations are painted text
// measured with TextLayout so they wrap at any width.

namespace
{
    juce::Colour accent() { return Accent::jam; }   // read at paint time: themes can change it

    const juce::String hostHelp = "jam.host", joinHelp = "jam.join";

    // A row of mutually exclusive TextButtons (looks like ParamSegmented, but bound to a lambda)
    struct Segments
    {
        juce::OwnedArray<juce::TextButton> buttons;
        std::function<void (int)> onPick;

        void create (juce::Component& parent, std::initializer_list<const char*> names)
        {
            int i = 0;
            const int n = (int) names.size();
            for (auto* name : names)
            {
                auto* b = buttons.add (new juce::TextButton (name));
                b->setConnectedEdges ((i > 0 ? (int) juce::Button::ConnectedOnLeft : 0)
                                    | (i < n - 1 ? (int) juce::Button::ConnectedOnRight : 0));
                b->onClick = [this, i] { select (i); if (onPick) onPick (i); };
                parent.addAndMakeVisible (b);
                ++i;
            }
        }

        void select (int index)
        {
            for (int i = 0; i < buttons.size(); ++i)
                buttons[i]->setToggleState (i == index, juce::dontSendNotification);
        }

        void setBounds (juce::Rectangle<int> r)
        {
            const int n = buttons.size();
            for (int i = 0; i < n; ++i)
                buttons[i]->setBounds (i < n - 1 ? r.removeFromLeft (r.getWidth() / (n - i)) : r);
        }

        void colour (juce::Colour c)
        {
            for (auto* b : buttons)
                b->setColour (juce::TextButton::buttonOnColourId, c.withAlpha (0.35f));
        }

        void setTooltip (const juce::String& t)
        {
            for (auto* b : buttons)
                b->setTooltip (t);
        }

        void setEnabled (bool on)
        {
            for (auto* b : buttons)
                b->setEnabled (on);
        }
    };

    //==========================================================================
    // One player: colour, name, badges, latency (tooltip: breakdown), volume and mute
    class PlayerRow : public juce::Component
    {
    public:
        PlayerRow (jam::Session& s, HelpTarget* h)
            : session (s), hoverVol (volume, "jam.volume", h), hoverMute (mute, "jam.mute", h), hoverLat (latency, "jam.latency", h)
        {
            volume.setSliderStyle (juce::Slider::LinearHorizontal);
            volume.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            volume.setRange (0.0, 1.5, 0.0);
            volume.setSkewFactor (0.6);
            volume.setDoubleClickReturnValue (true, 1.0);
            volume.setPopupDisplayEnabled (true, false, nullptr);
            volume.textFromValueFunction = [] (double v)
            {
                return v <= 0.001 ? juce::String ("-inf dB") : juce::String (juce::Decibels::gainToDecibels (v), 1) + " dB";
            };
            volume.onValueChange = [this] { session.setPeerVolume (info.id, (float) volume.getValue()); };
            mute.setButtonText ("MUTE");
            mute.setClickingTogglesState (true);
            mute.onClick = [this] { session.setPeerMuted (info.id, mute.getToggleState()); };
            addAndMakeVisible (volume);
            addAndMakeVisible (mute);
            addAndMakeVisible (latency);
        }

        jam::PeerId id() const { return info.id; }

        void update (const jam::PeerInfo& p)
        {
            const bool changed = first || p.name != info.name || p.colour != info.colour || p.isHost != info.isHost
                              || p.connected != info.connected || p.isSelf != info.isSelf || p.hasTrack != info.hasTrack;
            first = false;
            info = p;
            latency.set (p);
            volume.setVisible (! p.isSelf);
            mute.setVisible (! p.isSelf);
            if (! volume.isMouseButtonDown())
                volume.setValue (p.volume, juce::dontSendNotification);
            mute.setToggleState (p.muted, juce::dontSendNotification);
            if (changed)
                repaint();
        }

        void colours()
        {
            mute.setColour (juce::TextButton::buttonOnColourId, Palette::pink.withAlpha (0.45f));
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            const auto c = info.colour.isTransparent() ? Accent::jam : info.colour;
            g.setColour (Palette::viewBg.withAlpha (0.6f));
            g.fillRoundedRectangle (r.toFloat(), 5.0f);
            g.setColour (c.withAlpha (info.connected || info.isSelf ? 0.9f : 0.35f));
            g.fillRoundedRectangle (r.toFloat().removeFromLeft (3.0f).reduced (0.0f, 4.0f), 1.5f);

            auto t = nameArea.withTrimmedLeft (10);
            g.setColour (Palette::text);
            g.setFont (Theme::font (12.5f, true));
            juce::String name = info.name.isNotEmpty() ? info.name : "Player " + juce::String ((int) info.id);
            juce::String tags;
            if (info.isSelf)   tags << " (you)";
            if (info.isHost)   tags << "  HOST";
            if (info.hasTrack) tags << "  TRACK";
            if (! info.connected && ! info.isSelf) tags << "  OFFLINE";
            g.drawFittedText (name, t.removeFromTop (t.getHeight()), juce::Justification::centredLeft, 1, 0.8f);
            const int nameW = juce::jmin (t.getWidth(), juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name) + 2);
            g.setFont (Theme::font (10.0f, true));
            g.setColour (! info.connected && ! info.isSelf ? Palette::pink : info.isHost ? Accent::jam : Palette::dim);
            g.drawFittedText (tags, nameArea.withTrimmedLeft (10 + nameW), juce::Justification::centredLeft, 1, 0.7f);
            if (info.isSelf)
            {
                g.setColour (Palette::dim);
                g.setFont (Theme::font (10.5f));
                g.drawText ("your sound goes to everyone", controlsArea, juce::Justification::centredLeft);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (6, 4);
            latency.setBounds (r.removeFromRight (64).withSizeKeepingCentre (64, 20));
            r.removeFromRight (6);
            const int ctrlW = juce::jmin (190, r.getWidth() / 2);
            controlsArea = r.removeFromRight (ctrlW);
            nameArea = r;
            auto cr = controlsArea;
            mute.setBounds (cr.removeFromRight (48).withSizeKeepingCentre (48, 20));
            cr.removeFromRight (6);
            volume.setBounds (cr);
        }

    private:
        jam::Session& session;
        jam::PeerInfo info;
        bool first = true;
        juce::Slider volume;
        juce::TextButton mute;
        jamui::LatencyBadge latency;
        widgets_detail::HoverReporter hoverVol, hoverMute, hoverLat;
        juce::Rectangle<int> nameArea, controlsArea;
    };

    // Painted explanation text, wrapped to its width
    struct Note
    {
        juce::Rectangle<int> area;
        juce::String text;
        bool warn = false;
    };

    juce::Font noteFont() { return Theme::font (11.0f); }

    juce::TextLayout makeLayout (const juce::String& s, juce::Colour c, int width)
    {
        juce::AttributedString a;
        a.setWordWrap (juce::AttributedString::byWord);
        a.append (s, noteFont(), c);
        juce::TextLayout tl;
        tl.createLayout (a, (float) juce::jmax (40, width));
        return tl;
    }

    int noteHeight (const juce::String& s, int width)
    {
        return (int) std::ceil (makeLayout (s, Palette::dim, width).getHeight()) + 2;
    }
}

//==============================================================================
struct JamPanel::Impl : private juce::ChangeListener, private juce::Timer
{
    Impl (JamPanel& owner, ClodScopeProcessor& p, HelpTarget* h) : panel (owner), proc (p), help (h)
    {
        auto setup = [this] (juce::Component& c, const juce::String& id)
        {
            panel.addAndMakeVisible (c);
            hovers.push_back (std::make_unique<widgets_detail::HoverReporter> (c, id, help));
            if (auto* t = dynamic_cast<juce::SettableTooltipClient*> (&c))
                t->setTooltip (summaryOf (id));
        };

        // ---- session ----
        nameEd.setText (settings->name(), false);
        nameEd.setInputRestrictions (24);
        nameEd.setTextToShowWhenEmpty ("your name", Palette::dim);
        nameEd.onTextChange = [this] { settings->setName (nameEd.getText()); };
        setup (nameEd, "jam.name");

        portEd.setText (juce::String (settings->port()), false);
        portEd.setInputRestrictions (5, "0123456789");
        portEd.onTextChange = [this] { if (const int v = portEd.getText().getIntValue(); v >= 1024 && v <= 65535) settings->setPort (v); };
        setup (portEd, "jam.port");

        codeEd.setText (settings->lastCode(), false);
        codeEd.setTextToShowWhenEmpty ("paste the host's code", Palette::dim);
        codeEd.onReturnKey = [this] { join(); };
        setup (codeEd, "jam.code");

        hostBtn.setButtonText ("Host");
        hostBtn.onClick = [this] { host(); };
        setup (hostBtn, hostHelp);
        joinBtn.setButtonText ("Join");
        joinBtn.onClick = [this] { join(); };
        setup (joinBtn, joinHelp);
        leaveBtn.setButtonText ("Leave");
        leaveBtn.onClick = [this] { session->leave(); error.clear(); refresh(); };
        setup (leaveBtn, "jam.leave");
        copyBtn.setButtonText ("Copy");
        copyBtn.onClick = [this]
        {
            const auto current = session->sessionCode();
            if (current.isNotEmpty())
            {
                juce::SystemClipboard::copyTextToClipboard (current);
                copiedUntil = juce::Time::getMillisecondCounter() + 1500;
                copyBtn.setButtonText ("Copied");
            }
        };
        setup (copyBtn, "jam.copy");

        // ---- this plugin's role ----
        role.create (panel, { "Player", "Backing track" });
        role.onPick = [this] (int i)
        {
            if (auto* port = proc.getJamPort())
                port->setRole (i == 1 ? jam::Role::track : jam::Role::player);
            refresh();
        };
        for (auto* b : role.buttons) hovers.push_back (std::make_unique<widgets_detail::HoverReporter> (*b, "jam.role", help));
        role.setTooltip (summaryOf ("jam.role"));

        // ---- send mode ----
        sendMode.create (panel, { "Finished sound", "Raw input + settings" });
        sendMode.onPick = [this] (int i) { edit ([i] (jam::LatencyOptions& o) { o.sendMode = i == 1 ? jam::SendMode::rawInput : jam::SendMode::finishedSound; }); };
        for (auto* b : sendMode.buttons) hovers.push_back (std::make_unique<widgets_detail::HoverReporter> (*b, "jam.sendMode", help));
        sendMode.setTooltip (summaryOf ("jam.sendMode"));

        // ---- what your tile shows (ViewState::capture reads it) ----
        tileView.create (panel, { "Follow", "2D scope", "3D", "Orbital", "Sculptor" });
        tileView.onPick = [this] (int i) { proc.apvts.state.setProperty ("jamTileView", i, nullptr); };
        for (auto* b : tileView.buttons) hovers.push_back (std::make_unique<widgets_detail::HoverReporter> (*b, "jam.tileView", help));
        tileView.setTooltip (summaryOf ("jam.tileView"));
        tileView.select ((int) proc.apvts.state.getProperty ("jamTileView", 0));

        // ---- latency options ----
        auto toggle = [this, &setup] (juce::ToggleButton& t, const char* text, const char* id, std::function<void (jam::LatencyOptions&, bool)> fn)
        {
            t.setButtonText (text);
            t.onClick = [this, &t, fn] { const bool on = t.getToggleState(); edit ([fn, on] (jam::LatencyOptions& o) { fn (o, on); }); };
            setup (t, id);
        };
        toggle (compression, "Compression (Opus)", "jam.compression", [] (jam::LatencyOptions& o, bool on) { o.compression = on; });
        toggle (lossProtection, "Loss protection", "jam.lossProtection", [] (jam::LatencyOptions& o, bool on) { o.lossProtection = on; });
        toggle (drift, "Clock-drift correction", "jam.drift", [] (jam::LatencyOptions& o, bool on) { o.driftCorrection = on; });
        toggle (syncTrack, "Sync track", "jam.syncTrack", [] (jam::LatencyOptions& o, bool on) { o.syncTrack = on; });
        toggle (oneBar, "One-bar mode", "jam.oneBar", [] (jam::LatencyOptions& o, bool on) { o.oneBarMode = on; });

        for (int k : kbpsChoices)
            kbps.addItem (juce::String (k) + " kbit/s", k);
        kbps.onChange = [this] { const int v = kbps.getSelectedId(); if (v > 0) edit ([v] (jam::LatencyOptions& o) { o.opusKbps = v; }); };
        setup (kbps, "jam.opusKbps");

        packet.create (panel, { "Auto", "32", "64", "128", "256" });
        packet.onPick = [this] (int i) { edit ([i] (jam::LatencyOptions& o) { o.packetSamples = i == 0 ? 0 : 32 << (i - 1); }); };
        for (auto* b : packet.buttons) hovers.push_back (std::make_unique<widgets_detail::HoverReporter> (*b, "jam.packetSamples", help));
        packet.setTooltip (summaryOf ("jam.packetSamples"));

        jitterMode.create (panel, { "Auto", "Fixed" });
        jitterMode.onPick = [this] (int i) { edit ([i] (jam::LatencyOptions& o) { o.autoJitterBuffer = i == 0; }); };
        for (auto* b : jitterMode.buttons) hovers.push_back (std::make_unique<widgets_detail::HoverReporter> (*b, "jam.jitterBuffer", help));
        jitterMode.setTooltip (summaryOf ("jam.jitterBuffer"));

        jitterMs.setSliderStyle (juce::Slider::LinearHorizontal);
        jitterMs.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
        jitterMs.setRange (1.0, 60.0, 0.5);
        jitterMs.setSkewFactorFromMidPoint (12.0);
        jitterMs.setTextValueSuffix (" ms");
        jitterMs.setDoubleClickReturnValue (true, 10.0);
        jitterMs.onValueChange = [this] { const float v = (float) jitterMs.getValue(); edit ([v] (jam::LatencyOptions& o) { o.jitterMs = v; }); };
        setup (jitterMs, "jam.jitterMs");
        setup (estimateHover, "jam.estimate");   // transparent, over the painted estimate: Inspector help on hover

        // ---- notes (painted, wrapped) ----
        notes.resize (numNotes);
        notes[roleNote].text = "Player: this Clod Scope sends your sound and plays your friends to you. Backing track: it sends "
                               "what passes through it as the session's backing track. In FL Studio put a second Clod Scope on "
                               "a mixer insert that carries ONLY the beat (beat -> that insert -> Master), never on the Master "
                               "itself: there it would also send your guitar twice. The standalone app has its own track player.";
        notes[sendNote].text = "Finished sound: friends hear exactly what you hear (stereo). Raw input + settings: half the "
                               "data; friends rebuild your sound on their computer from your dry input and your settings, which "
                               "costs them CPU.";
        notes[tileNote].text = "Your tile in the Jam Sesh view, for you and your friends. Follow: the view you have open "
                               "(here on the Jam Sesh page, the one you had open last). Or pick one to always show it.";
        notes[compressionNote].text = "Off: uncompressed 16-bit audio, no codec delay, ~0.77 Mbit/s per channel. On: Opus, "
                                      "about 10x less data but ~5 ms more delay.";
        notes[lossNote].text = "Sends each packet's audio again in the next one, so a single lost packet is not heard. Doubles the "
                               "audio data and adds one packet of delay.";
        notes[packetNote].text = "Samples per packet at 48 kHz. Auto follows your sound card's block (64 to 128), which is "
                                 "quickest. 32 = 0.7 ms is quicker still but sends 1500 packets a second; 256 = 5.3 ms suits Wi-Fi.";
        notes[jitterNote].text = "The safety buffer that evens out packets arriving early or late. Auto adapts to each friend's "
                                 "connection; Fixed always waits this long. Too small = crackles, too big = delay.";
        notes[driftNote].text = "Stretches each friend's audio very slightly to your sound card's clock. Inaudible; without it a "
                                "long jam slowly drifts and drops out.";
        notes[syncNote].text = "The host hears the backing track as late as it reaches the friends, so everyone plays to the same "
                               "beat. Off: the host hears it at once and is ahead of everyone.";
        notes[oneBarNote].text = "For far-away friends: everyone hears everyone exactly one bar late, in time with the beat, "
                                 "instead of a little late and out of time. Needs a steady tempo.";
        notes[routerNote].text = "Over the internet the host's router must let friends in. When you host, Jam Sesh puts your "
                                 "internet address in the code (it asks a public STUN server, Google's or Cloudflare's, for it) "
                                 "and asks your router to forward UDP port " + juce::String (jam::defaultPort)
                               + " (or the port you host on) to this PC (UPnP); the line under the code says whether it worked. "
                                 "If not, forward the port in the router's settings, or let a friend host. Allow Jam Sesh (or "
                                 "your DAW) in the Windows firewall when it asks.\nNo luck? Everyone installs a VPN such as "
                                 "Tailscale or ZeroTier and joins the same network; the host then shares a code with the VPN "
                                 "address. Same house or same Wi-Fi: nothing to set up.";

        colours();
        session->addChangeListener (this);
        syncOptions();
        refresh();
        startTimerHz (15);
    }

    ~Impl() override
    {
        stopTimer();
        session->removeChangeListener (this);
    }

    static juce::String summaryOf (const juce::String& id)
    {
        static const HelpList list = JamPanel::helpEntries();
        for (auto& e : list)
            if (e.id == id)
                return e.summary;
        return {};
    }

    void colours()
    {
        Theme::setAccent (panel, accent());
        for (auto* s : { &role, &sendMode, &tileView, &packet, &jitterMode })
            s->colour (accent());
        for (auto* e : { &nameEd, &portEd, &codeEd })
            e->setColour (juce::TextEditor::focusedOutlineColourId, accent());
        hostBtn.setColour (juce::TextButton::buttonColourId, accent().withAlpha (0.28f));
        joinBtn.setColour (juce::TextButton::buttonColourId, accent().withAlpha (0.28f));
        for (auto* r : rows)
            r->colours();
    }

    //==========================================================================
    void edit (std::function<void (jam::LatencyOptions&)> fn)
    {
        if (syncing)
            return;
        auto o = session->options();
        fn (o);
        session->setOptions (o);
        syncOptions();
        updateEstimate();
    }

    // controls follow the session's options (another instance or the session may change them)
    void syncOptions()
    {
        const juce::ScopedValueSetter<bool> guard (syncing, true);
        const auto o = session->options();
        compression.setToggleState (o.compression, juce::dontSendNotification);
        lossProtection.setToggleState (o.lossProtection, juce::dontSendNotification);
        drift.setToggleState (o.driftCorrection, juce::dontSendNotification);
        syncTrack.setToggleState (o.syncTrack, juce::dontSendNotification);
        oneBar.setToggleState (o.oneBarMode, juce::dontSendNotification);
        int k = kbpsChoices[0];
        for (int c : kbpsChoices)
            if (std::abs (c - o.opusKbps) < std::abs (k - o.opusKbps))
                k = c;
        kbps.setSelectedId (k, juce::dontSendNotification);
        kbps.setEnabled (o.compression);
        kbps.setAlpha (o.compression ? 1.0f : 0.4f);
        packet.select (o.packetSamples <= 0 ? 0 : o.packetSamples <= 32 ? 1 : o.packetSamples <= 64 ? 2 : o.packetSamples <= 128 ? 3 : 4);
        jitterMode.select (o.autoJitterBuffer ? 0 : 1);
        if (! jitterMs.isMouseButtonDown())
            jitterMs.setValue (o.jitterMs, juce::dontSendNotification);
        jitterMs.setEnabled (! o.autoJitterBuffer);
        jitterMs.setAlpha (o.autoJitterBuffer ? 0.4f : 1.0f);
        sendMode.select (o.sendMode == jam::SendMode::rawInput ? 1 : 0);
    }

    void host()
    {
        settings->setName (nameEd.getText());
        const int port = juce::jlimit (1024, 65535, portEd.getText().getIntValue() > 0 ? portEd.getText().getIntValue() : jam::defaultPort);
        portEd.setText (juce::String (port), false);
        settings->setPort (port);
        juce::String err;
        error = session->host (settings->name(), port, err) ? juce::String() : (err.isNotEmpty() ? err : juce::String ("Could not host."));
        refresh();
    }

    void join()
    {
        const auto typed = codeEd.getText().trim();
        if (typed.isEmpty())
        {
            error = "Paste the host's code first.";
            refresh();
            return;
        }
        settings->setName (nameEd.getText());
        settings->setLastCode (typed);
        juce::String err;
        error = session->join (typed, settings->name(), err) ? juce::String() : (err.isNotEmpty() ? err : juce::String ("Could not join."));
        refresh();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        syncOptions();
        refresh();
    }

    void timerCallback() override
    {
        if (copiedUntil != 0 && juce::Time::getMillisecondCounter() > copiedUntil)
        {
            copiedUntil = 0;
            copyBtn.setButtonText ("Copy");
        }
        if (jamui::visibleInTree (panel))
            refresh();
    }

    void refresh()
    {
        auto& s = *session;
        const auto st = s.status();
        const bool in = jamui::inSession (st);
        auto line = s.statusText();
        const auto c = s.sessionCode();
        const auto reachNow = s.reachInfo();

        hostBtn.setEnabled (! in);
        joinBtn.setEnabled (! in);
        portEd.setEnabled (! in);
        codeEd.setEnabled (! in);
        leaveBtn.setEnabled (in || st == jam::Status::failed);
        copyBtn.setEnabled (c.isNotEmpty());
        if (in)
            error.clear();

        const auto* port = proc.getJamPort();
        role.setEnabled (port != nullptr);
        role.select (port != nullptr && port->role() == jam::Role::track ? 1 : 0);
        tileView.select ((int) proc.apvts.state.getProperty ("jamTileView", 0));   // also after a session is reloaded

        // player rows
        // outside a session the list may still hold yourself: show the explainer instead of tiles
        auto players = jamui::inSession (s.status()) ? s.players() : jamui::fakePlayers();
        bool rebuild = players.size() != (size_t) rows.size();
        for (size_t i = 0; ! rebuild && i < players.size(); ++i)
            rebuild = rows[(int) i]->id() != players[i].id;
        if (rebuild)
        {
            rows.clear();
            for (size_t i = 0; i < players.size(); ++i)
            {
                auto* r = rows.add (new PlayerRow (s, help));
                r->colours();
                panel.addAndMakeVisible (r);
            }
        }
        for (size_t i = 0; i < players.size(); ++i)
            rows[(int) i]->update (players[i]);
        friends = juce::jmax (0, (int) players.size() - 1);

        const bool textChanged = st != status || line != statusLine || c != code || error != shownError
                              || reachNow.state != reach.state || reachNow.text != reach.text;
        status = st;
        statusLine = line;
        code = c;
        shownError = error;
        reach = reachNow;
        updateEstimate();
        if (rebuild || textChanged)
            relayout();
    }

    void updateEstimate()
    {
        const auto o = session->options();
        const auto me = session->engine().localLatency (proc);
        const int ch = o.sendMode == jam::SendMode::rawInput ? 1 : 2;
        const auto e = jamui::estimate (o, me, session->sessionBpm(), ch);

        juce::String d, u;
        if (o.oneBarMode)
            d << "1 bar = " << jamui::ms (e.oneBarMs) << " at " << juce::String (session->sessionBpm(), 0) << " BPM (in time)";
        else
            d << (e.cardKnown ? "~" : "about ") << jamui::ms (e.totalMs) << " + the network";
        u << jamui::mbit (e.mbitPerFriend) << " per friend";
        if (friends > 0)
            u << ", " << jamui::mbit (e.mbitPerFriend * (float) friends) << " now";
        else
            u << ", " << jamui::mbit (e.mbitPerFriend * (float) (jam::maxPlayers - 1)) << " with 5";
        juce::String parts;
        if (e.cardKnown)
            parts << "sound card in " << juce::String (e.cardInMs, 1) << " + out " << jamui::ms (e.cardOutMs);
        else
            parts << "sound card ~" << jamui::ms (e.cardMs) << " (estimate: 2 x block + 1.5)";
        parts << "  +  sound chain " << jamui::ms (e.chainMs) << "  +  packets & buffer " << jamui::ms (e.bufferMs);
        if (o.compression)
            parts << "  +  codec " << jamui::ms (e.codecMs);
        parts << "   |   packet " << jamui::ms (e.packetMs) << ", " << e.packetsPerSecond << " packets/s each way";
        if (d != estDelay || u != estUpload || parts != estParts)
        {
            estDelay = d;
            estUpload = u;
            estParts = parts;
            panel.repaint (estimateRect);
        }
    }

    void relayout()
    {
        panel.resized();
        panel.repaint();
        if (auto* vp = panel.findParentComponentOfClass<juce::Viewport>())
            vp->resized();
    }

    //==========================================================================
    int layout (int widthPx, bool apply)
    {
        const int pad = 12, gap = 10;
        const int inner = juce::jmax (200, widthPx - pad * 2);
        int y = 0;
        auto place = [apply] (juce::Component* c, juce::Rectangle<int> r) { if (apply && c != nullptr) c->setBounds (r); };
        auto note = [&] (int which, int indent = 0)
        {
            const int h = noteHeight (notes[(size_t) which].text, inner - indent);
            if (apply)
                notes[(size_t) which].area = { pad + indent, y, inner - indent, h };
            y += h + 4;
        };
        auto label = [&] (juce::Rectangle<int>& slot) { slot = { pad, y, inner, 16 }; y += 17; };
        auto begin = [&] { const int top = y; y += Theme::sectionTitle; return top; };
        auto end = [&] (juce::Rectangle<int>& rect, int top) { y += 6; if (apply) rect = { 0, top, widthPx, y - top }; y += gap; };
        juce::Rectangle<int> scratch;

        // SESSION
        {
            const int top = begin();
            juce::Rectangle<int> st { pad, y, inner, 20 };
            if (apply) statusRect = st;
            y += 22;
            if (shownError.isNotEmpty())
            {
                const int h = noteHeight (shownError, inner - 12) + 6;
                if (apply) errorRect = { pad, y, inner, h };
                y += h + 4;
            }
            else if (apply)
            {
                errorRect = {};
            }

            // the code and Copy, then whether friends over the internet can reach the host
            if (apply) codeRect = { pad, y, inner - 76, 30 };
            place (&copyBtn, { pad + inner - 70, y + 3, 70, 24 });
            y += 36;
            if (reach.text.isNotEmpty())
            {
                const int h = noteHeight (reach.text, inner - 12) + 6;
                if (apply) reachRect = { pad, y, inner, h };
                y += h + 6;
            }
            else if (apply)
            {
                reachRect = {};
            }

            label (apply ? nameLabel : scratch);
            place (&nameEd, { pad, y, inner, 26 });
            y += 32;

            label (apply ? hostLabel : scratch);
            place (&portEd, { pad, y, 80, 26 });
            place (&hostBtn, { pad + 86, y, inner - 86, 26 });
            y += 32;

            label (apply ? joinLabel : scratch);
            place (&codeEd, { pad, y, inner - 86, 26 });
            place (&joinBtn, { pad + inner - 80, y, 80, 26 });
            y += 34;
            place (&leaveBtn, { pad, y, inner, 26 });
            y += 26;
            end (sessionRect, top);
        }

        // PLAYERS
        {
            const int top = begin();
            if (rows.isEmpty())
            {
                if (apply) emptyPlayersRect = { pad, y, inner, 22 };
                y += 24;
            }
            for (auto* r : rows)
            {
                place (r, { pad, y, inner, 32 });
                y += 36;
            }
            end (playersRect, top);
        }

        // THIS PLUGIN
        {
            const int top = begin();
            if (apply) role.setBounds ({ pad, y, inner, 26 });
            y += 32;
            note (roleNote);
            end (roleRect, top);
        }

        // WHAT YOU SEND
        {
            const int top = begin();
            if (apply) sendMode.setBounds ({ pad, y, inner, 26 });
            y += 32;
            note (sendNote);
            y += 4;
            if (apply) tileView.setBounds ({ pad, y, inner, 26 });
            y += 32;
            note (tileNote);
            end (sendRect, top);
        }

        // LATENCY
        {
            const int top = begin();
            if (apply) estimateRect = { pad, y, inner, 78 };
            place (&estimateHover, { pad, y, inner, 78 });
            y += 84;

            const int half = inner / 2;
            place (&compression, { pad, y, half + 30, 24 });
            place (&kbps, { pad + half + 34, y, inner - half - 34, 24 });
            y += 26;
            note (compressionNote, 24);

            place (&lossProtection, { pad, y, inner, 24 });
            y += 26;
            note (lossNote, 24);

            label (apply ? packetLabel : scratch);
            if (apply) packet.setBounds ({ pad, y, inner, 26 });
            y += 30;
            note (packetNote, 24);

            label (apply ? jitterLabel : scratch);
            if (apply) jitterMode.setBounds ({ pad, y, juce::jmin (160, half), 26 });
            place (&jitterMs, { pad + juce::jmin (160, half) + 10, y, inner - juce::jmin (160, half) - 10, 26 });
            y += 30;
            note (jitterNote, 24);

            place (&drift, { pad, y, inner, 24 });
            y += 26;
            note (driftNote, 24);

            place (&syncTrack, { pad, y, inner, 24 });
            y += 26;
            note (syncNote, 24);

            place (&oneBar, { pad, y, inner, 24 });
            y += 26;
            note (oneBarNote, 24);
            end (latencyRect, top);
        }

        // ROUTERS & VPNS
        {
            const int top = begin();
            note (routerNote);
            end (routerRect, top);
        }
        return y;
    }

    void paint (juce::Graphics& g)
    {
        const auto a = accent();
        Theme::drawSection (g, sessionRect, "JAM SESH", a);
        Theme::drawSection (g, playersRect, "PLAYERS", a);
        Theme::drawSection (g, roleRect, "THIS PLUGIN", a);
        Theme::drawSection (g, sendRect, "WHAT YOU SEND", a);
        Theme::drawSection (g, latencyRect, "LATENCY", a);
        Theme::drawSection (g, routerRect, "ROUTERS & VPNS", a);

        // status: dot + one line
        {
            auto r = statusRect;
            const auto c = jamui::statusColour (status);
            g.setColour (c);
            g.fillEllipse (r.removeFromLeft (10).withSizeKeepingCentre (8, 8).toFloat());
            r.removeFromLeft (6);
            g.setFont (Theme::font (11.0f, true));
            const juce::String name = jamui::statusName (status);
            const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name) + 8;
            g.drawText (name, r.removeFromLeft (w), juce::Justification::centredLeft);
            g.setColour (Palette::text);
            g.setFont (Theme::font (11.5f));
            g.drawFittedText (statusLine, r, juce::Justification::centredLeft, 1, 0.75f);
        }
        if (! errorRect.isEmpty())
        {
            g.setColour (Palette::pink.withAlpha (0.12f));
            g.fillRoundedRectangle (errorRect.toFloat(), 5.0f);
            makeLayout (shownError, Palette::pink, errorRect.getWidth() - 12).draw (g, errorRect.reduced (6, 3).toFloat());
        }

        // the session code
        {
            auto r = codeRect;
            g.setColour (Palette::viewBg);
            g.fillRoundedRectangle (r.toFloat(), 5.0f);
            g.setColour (code.isNotEmpty() ? a.withAlpha (0.7f) : Palette::panelEdge);
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 5.0f, 1.0f);
            r.reduce (10, 0);
            g.setColour (Palette::dim);
            g.setFont (Theme::font (10.0f, true));
            g.drawText ("CODE", r.removeFromLeft (40), juce::Justification::centredLeft);
            g.setColour (code.isNotEmpty() ? Palette::text : Palette::dim);
            g.setFont (code.isNotEmpty() ? juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::bold))
                                         : Theme::font (11.0f));
            g.drawFittedText (code.isNotEmpty() ? code : juce::String ("Host or join to get a code"), r, juce::Justification::centredLeft, 1, 0.6f);
        }
        if (! reachRect.isEmpty())
        {
            const auto c = reach.state == jam::ReachState::open        ? Palette::green
                         : reach.state == jam::ReachState::checking    ? Palette::blue
                         : reach.state == jam::ReachState::shared      ? Palette::pink
                                                                       : Palette::amber;
            g.setColour (c.withAlpha (0.12f));
            g.fillRoundedRectangle (reachRect.toFloat(), 5.0f);
            makeLayout (reach.text, c, reachRect.getWidth() - 12).draw (g, reachRect.reduced (6, 3).toFloat());
        }

        g.setColour (Palette::dim);
        g.setFont (Theme::font (11.0f, true));
        g.drawText ("YOUR NAME  (friends see it; remembered on this computer)", nameLabel, juce::Justification::centredLeft);
        g.drawText ("HOST  (port, UDP)", hostLabel, juce::Justification::centredLeft);
        g.drawText ("JOIN  (the host's code)", joinLabel, juce::Justification::centredLeft);
        g.drawText ("PACKET SIZE  (samples)", packetLabel, juce::Justification::centredLeft);
        g.drawText ("JITTER BUFFER", jitterLabel, juce::Justification::centredLeft);

        if (rows.isEmpty())
        {
            g.setColour (Palette::dim);
            g.setFont (Theme::font (11.5f));
            g.drawText ("Nobody here yet. Host or join a session above.", emptyPlayersRect, juce::Justification::centredLeft);
        }

        // live estimate
        {
            auto r = estimateRect;
            g.setColour (Palette::viewBg);
            g.fillRoundedRectangle (r.toFloat(), 6.0f);
            g.setColour (a.withAlpha (0.45f));
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 6.0f, 1.0f);
            r.reduce (10, 6);
            auto delayRow = r.removeFromTop (20);
            auto uploadRow = r.removeFromTop (20);
            g.setFont (Theme::font (10.0f, true));
            g.setColour (Palette::dim);
            g.drawText ("DELAY", delayRow.removeFromLeft (56), juce::Justification::centredLeft);
            g.drawText ("UPLOAD", uploadRow.removeFromLeft (56), juce::Justification::centredLeft);
            g.setFont (Theme::font (13.0f, true));
            g.setColour (a);
            g.drawFittedText (estDelay, delayRow, juce::Justification::centredLeft, 1, 0.7f);
            g.setColour (Palette::text);
            g.drawFittedText (estUpload, uploadRow, juce::Justification::centredLeft, 1, 0.7f);
            g.setColour (Palette::dim);
            g.setFont (Theme::font (10.5f));
            g.drawFittedText (estParts, r, juce::Justification::centredLeft, 2, 0.8f);
        }

        for (auto& n : notes)
            if (! n.area.isEmpty())
                makeLayout (n.text, Palette::dim, n.area.getWidth()).draw (g, n.area.toFloat());
    }

    enum { roleNote, sendNote, compressionNote, lossNote, packetNote, jitterNote, driftNote, syncNote, oneBarNote, routerNote, tileNote, numNotes };
    static constexpr int kbpsChoices[] = { 64, 96, 128, 192, 256 };

    JamPanel& panel;
    ClodScopeProcessor& proc;
    HelpTarget* help;
    juce::SharedResourcePointer<jam::Session> session;
    juce::SharedResourcePointer<jamui::Settings> settings;

    juce::TextEditor nameEd, portEd, codeEd;
    juce::TextButton hostBtn, joinBtn, leaveBtn, copyBtn;
    Segments role, sendMode, tileView, packet, jitterMode;
    juce::ToggleButton compression, lossProtection, drift, syncTrack, oneBar;
    juce::ComboBox kbps;
    juce::Slider jitterMs;
    juce::Component estimateHover;
    juce::OwnedArray<PlayerRow> rows;
    std::vector<std::unique_ptr<widgets_detail::HoverReporter>> hovers;
    std::vector<Note> notes;

    jam::Status status = jam::Status::idle;
    juce::String statusLine, code, error, shownError, estDelay, estUpload, estParts;
    jam::ReachInfo reach;
    juce::Rectangle<int> reachRect;
    int friends = 0;
    bool syncing = false;
    juce::uint32 copiedUntil = 0;

    juce::Rectangle<int> sessionRect, playersRect, roleRect, sendRect, latencyRect, routerRect;
    juce::Rectangle<int> statusRect, errorRect, codeRect, nameLabel, hostLabel, joinLabel, packetLabel, jitterLabel,
                         emptyPlayersRect, estimateRect;
};

//==============================================================================
JamPanel::JamPanel (ClodScopeProcessor& p, HelpTarget* h)
{
    impl = std::make_unique<Impl> (*this, p, h);
}

JamPanel::~JamPanel() = default;

int JamPanel::idealHeight (int width) const
{
    return impl != nullptr ? impl->layout (width, false) : 100;
}

void JamPanel::paint (juce::Graphics& g)
{
    if (impl != nullptr)
        impl->paint (g);
}

void JamPanel::resized()
{
    if (impl != nullptr)   // null while Impl's constructor runs its first refresh
        impl->layout (getWidth(), true);
}

void JamPanel::lookAndFeelChanged()
{
    if (impl != nullptr)
        impl->colours();
    repaint();
}

//==============================================================================
HelpList JamPanel::helpEntries()
{
    const juce::String port (jam::defaultPort);
    return {
        { "jam.name", "Your name",
          "The name your friends see on your tile and in their player list.",
          "Sent to everyone when you host or join (up to 24 characters). Remembered for every Clod Scope on this computer "
          "in %APPDATA%\\Clod Scope\\Jam Sesh.settings. Changing it during a session takes effect the next time you join.",
          "Empty uses your Windows user name." },
        { "jam.port", "Host port",
          "The UDP port your computer listens on when you host.",
          "Any port from 1024 to 65535; default " + port + ". Friends never type it: it is part of the session code. "
          "Over the internet the host's router must forward this exact UDP port: Jam Sesh asks the router to (UPnP) "
          "and says under the code whether it worked.",
          "Change it only if another program already uses " + port + "." },
        { hostHelp, "Host",
          "Starts a session on this computer; friends join you with your code.",
          "Opens UDP on the port to the left, makes you player 1 (the host: you set the session tempo and coordinate "
          "everyone) and shows the session code above. Up to 6 players in total, you included.",
          "The host needs the best upload: every player sends to every other player directly." },
        { "jam.code", "Session code",
          "What a friend types to join: the host's addresses and port, packed into one line.",
          "Paste the code the host sent you. It holds the host's home network and internet addresses, the port and "
          "a random session key, no password or other personal data. The last code you used is remembered.",
          "Only give it to people you want in the jam. With a VPN (Tailscale / ZeroTier) it also holds the VPN address." },
        { joinHelp, "Join",
          "Joins the session whose code is in the box.",
          "Contacts the host at the address in the code, gets the list of players and connects to each of them "
          "directly (peer-to-peer audio). Status shows JOINING until the host answers, then CONNECTED, or FAILED with the reason.",
          "Fails if the host's router doesn't let friends in (the host's page says) and you are not on the same network or VPN." },
        { "jam.leave", "Leave",
          "Leaves the session (as host: ends it for everyone).",
          "Stops sending and receiving at once; friends' tiles disappear and their sound stops.",
          {} },
        { "jam.copy", "Copy code",
          "Copies the session code to the clipboard.",
          "Copies the exact text shown next to CODE, to paste into a chat.",
          {} },
        { "jam.volume", "Player volume",
          "How loud this friend is in your own mix (only you hear the change).",
          "Gain from 0 (-inf dB) to 1.5 (+3.5 dB); 1.0 = 0 dB = as they send it. Double-click for 0 dB.",
          {} },
        { "jam.mute", "Mute player",
          "Silences this friend for you only.",
          "Their sound is still received (their tile keeps moving), it is just not mixed into what you hear.",
          {} },
        { "jam.latency", "Total latency",
          "How long from this friend's pick attack to your ears, everything included. Hover for the breakdown.",
          "Sum in ms of: their side (their sound card's input + their sound chain + packet, as their Jam Sesh reports it; "
          "estimated for older versions), the network (one way), the codec (0 uncompressed, 2.5 ms Opus look-ahead) and your "
          "side (your safety buffer for them + your sound card's output as its driver reports it; inside a plugin host the "
          "sound card is estimated). Green under 20 ms (feels like one room), amber under 40 ms, pink above.",
          "Above ~40 ms tight playing gets hard: try smaller packets, compression off, a smaller sound-card buffer or one-bar mode." },
        { "jam.role", "This plugin's role",
          "Whether this Clod Scope is your player or the session's backing track.",
          "Player: sends your sound (or raw input) and plays your friends into its output. Backing track: sends what passes "
          "through it as the backing track and mixes nothing in. In FL Studio put a second Clod Scope on a mixer insert "
          "that carries only the beat (beat -> that insert -> Master) and set it to Backing track; not on the Master, where it "
          "would also send your own guitar. The standalone app has its own track player.",
          "One player instance per computer; the first instance set to Player is used." },
        { "jam.tileView", "Your tile",
          "What your tile in the Jam Sesh view shows, for you and every friend.",
          "Follow: the view you have open; while you're on the Jam Sesh page, the one you had open last. 2D scope, 3D, "
          "Orbital or Sculptor: always that view, with your own settings for it (the 3D mode, the orbital look ...). "
          "Friends' computers draw it from your sound and settings, so it costs them a little graphics work.",
          "Remembered with your session; presets don't change it." },
        { "jam.sendMode", "What you send",
          "Your finished sound, or your dry input plus your settings.",
          "Finished sound: stereo, exactly what you hear (2 x 0.77 Mbit/s uncompressed). Raw input + settings: your dry input "
          "(mono for a mono guitar, 0.77 Mbit/s) plus your settings; every friend rebuilds your sound with a hidden copy of "
          "Clod Scope, so it costs them CPU and they hear your settings changes about half a second late.",
          "Raw input halves your upload, which lets uncompressed audio reach more friends." },
        { "jam.compression", "Compression (Opus)",
          "Compresses your audio with Opus instead of sending raw 16-bit samples.",
          "Off (default): uncompressed 16-bit PCM at 48 kHz, 0.768 Mbit/s per channel, no codec delay. On: Opus at the "
          "bit rate to the right, about 10x less data, adds ~5 ms of delay (look-ahead + frame).",
          "Turn it on for slow uploads or many friends; keep it off on a good connection." },
        { "jam.opusKbps", "Opus bit rate",
          "Data rate of each compressed stream.",
          "64 / 96 / 128 / 192 / 256 kbit/s per stream (stereo or mono), only while compression is on. Higher sounds closer "
          "to the original; the delay does not change.",
          "128 kbit/s is transparent for most guitar tones." },
        { "jam.lossProtection", "Loss protection",
          "Repeats each packet's audio in the next packet.",
          "A single lost packet is rebuilt from the next one instead of being heard as a click. Doubles the audio data "
          "(headers stay the same) and adds one packet of delay to the safety buffer.",
          "Worth it on Wi-Fi or a busy connection (loss shown on the tiles above 1 %)." },
        { "jam.packetSamples", "Packet size",
          "How much audio goes into each network packet.",
          "Auto: one packet per block of your sound card (64 to 128 samples), so no audio waits for a packet to fill. "
          "32 / 64 / 128 / 256 samples at 48 kHz = 0.7 / 1.3 / 2.7 / 5.3 ms of audio per packet and 1500 / 750 / 375 / 188 "
          "packets per second to each friend. Every packet carries ~60 bytes of headers, so small packets cost upload.",
          "Auto is the default. 32 only on a fast wired upload; 256 for Wi-Fi." },
        { "jam.jitterBuffer", "Jitter buffer",
          "The safety buffer that evens out packets arriving early or late.",
          "Auto: adapts per friend to their connection's jitter (bigger when packets arrive unevenly, smaller when calm). "
          "Fixed: always waits the time on the slider for every friend.",
          "Crackles = buffer too small; everything late = buffer too big." },
        { "jam.jitterMs", "Fixed buffer",
          "The safety buffer in Fixed mode.",
          "1 to 60 ms, added to every friend's delay. Only used when the jitter buffer is set to Fixed. Double-click for 10 ms.",
          {} },
        { "jam.drift", "Clock-drift correction",
          "Keeps each friend's audio in step with your sound card's clock.",
          "Every sound card runs a little fast or slow (shown in ppm in the latency breakdown). On: their audio is stretched "
          "by that tiny amount, which is inaudible. Off: the buffer slowly empties or overflows and drops out every few minutes.",
          "Leave it on unless you are testing." },
        { "jam.syncTrack", "Sync track",
          "The host hears the backing track as late as it reaches the friends.",
          "On: the backing track is delayed here by the friends' latency (the largest one, in ms), so everyone hears the "
          "same beat at the same moment. Off: the host hears it at once and plays ahead of everyone.",
          "Only matters on the computer that plays the backing track." },
        { "jam.oneBar", "One-bar mode",
          "For far-away friends: everyone hears everyone exactly one bar late, in time.",
          "Each friend's audio is delayed to exactly one bar (4 beats at the session tempo: 2000 ms at 120 BPM) after it "
          "was played, so it lands on the beat of the next bar. Needs a steady tempo (the host's).",
          "Works for loops and jams over a repeating progression; not for call-and-response." },
        { "jam.estimate", "Latency estimate",
          "What your current options cost: delay and upload.",
          "Delay, one way to a friend with your sound card and options, without the network itself: your sound card's input "
          "+ output (as its driver reports them; inside a plugin host estimated as 2 x block + 1.5 ms), your sound chain, "
          "the packets and the friend's safety buffer on a calm connection, and the codec. Upload = audio + headers to one "
          "friend in Mbit/s (you send to every friend separately).",
          {} },
    };
}
