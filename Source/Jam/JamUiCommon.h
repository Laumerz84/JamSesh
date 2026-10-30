#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "BinaryData.h"
#include "JamTypes.h"
#include "../UI/Theme.h"

// Shared by the Jam Sesh view and panel (owner: jam-ui soldier): the title font, the latency and
// bandwidth estimate of a set of LatencyOptions, the latency breakdown text of a player, and the
// settings this computer remembers for every Clod Scope (your name, the last port and code).
namespace jamui
{
    //==========================================================================
    // New Rocker (SIL OFL, compiled in) for the JAM SESH title; falls back to the UI font
    inline juce::Font titleFont (float height)
    {
        static juce::Typeface::Ptr face = juce::Typeface::createSystemTypefaceFor (BinaryData::NewRockerRegular_ttf,
                                                                                  (size_t) BinaryData::NewRockerRegular_ttfSize);
        if (face == nullptr)
            return Theme::font (height, true);
        return juce::Font (juce::FontOptions (face).withHeight (height));
    }

    //==========================================================================
    // What one set of options costs. Everything at 48 kHz (the session rate), one friend, one way.
    struct Estimate
    {
        float packetMs = 0.0f;      // waiting for a packet to fill
        float codecMs = 0.0f;       // Opus look-ahead + frame (0 when uncompressed)
        float bufferMs = 0.0f;      // the receiver's safety buffer (jitter buffer, + one packet with loss protection)
        float cardMs = 0.0f;        // this sound card's block (passed in)
        float totalMs = 0.0f;       // all of the above, without the network itself
        float oneBarMs = 0.0f;      // one-bar mode: what everyone hears instead (one bar at the session tempo)
        float mbitPerFriend = 0.0f; // upload to one friend, packet headers included
        int packetsPerSecond = 0;
    };

    constexpr float sessionRate = 48000.0f;
    constexpr int headerBytes = 28 + 28 + 4; // IPv4 + UDP, the audio header (stream), the network framing (approx.)
    constexpr float opusDelayMs = 2.5f;     // look-ahead (the 2.5 ms frame is the packet, counted there)

    inline float packetMs (int samples) { return 1000.0f * (float) samples / sessionRate; }

    // channels: 2 for the finished sound, 1 for raw input from a mono guitar (raw input is mono when the input is)
    inline Estimate estimate (const jam::LatencyOptions& o, float cardMs, double bpm, int channels)
    {
        Estimate e;
        e.packetMs = packetMs (o.packetSamples);
        e.codecMs = o.compression ? opusDelayMs : 0.0f;
        const float autoBuffer = juce::jmax (2.0f * e.packetMs, 4.0f);   // typical for a calm connection
        e.bufferMs = (o.autoJitterBuffer ? autoBuffer : o.jitterMs) + (o.lossProtection ? e.packetMs : 0.0f);
        e.cardMs = cardMs;
        e.totalMs = e.packetMs + e.codecMs + e.bufferMs + e.cardMs;
        e.oneBarMs = (float) (4.0 * 60000.0 / juce::jlimit (30.0, 300.0, bpm));

        e.packetsPerSecond = (int) std::round (sessionRate / (float) juce::jmax (16, o.packetSamples));
        const float audioBps = o.compression ? (float) o.opusKbps * 1000.0f      // per stream, whatever the channels
                                             : 16.0f * sessionRate * (float) channels;
        const float payload = audioBps * (o.lossProtection ? 2.0f : 1.0f);
        const float headers = (float) (headerBytes * 8 * e.packetsPerSecond);
        e.mbitPerFriend = (payload + headers) / 1.0e6f;
        return e;
    }

    // (juce::String (v, 0) would print every digit, "26.2171")
    inline juce::String ms (float v)   { return (v < 10.0f ? juce::String (v, 1) : juce::String (juce::roundToInt (v))) + " ms"; }
    inline juce::String mbit (float v) { return juce::String (v, v < 1.0f ? 2 : 1) + " Mbit/s"; }

    //==========================================================================
    // A player's total latency split into its parts (tooltip of a tile / player row)
    inline juce::String breakdown (const jam::PeerInfo& p)
    {
        if (p.isSelf)
            return "This is you. Your own sound reaches you with no network delay.";
        if (! p.connected)
            return p.name + " is not connected right now.";
        const float codec = p.compression ? opusDelayMs : 0.0f;
        const float rest = juce::jmax (0.0f, p.totalMs - p.networkMs - p.bufferMs - codec);
        juce::String s;
        s << p.name << ": " << ms (p.totalMs) << " from their pick to your ears\n"
          << "  network   " << ms (p.networkMs) << "  (ping " << ms (p.pingMs) << ", jitter " << ms (p.jitterMs)
                            << (p.relayed ? ", relayed by the host" : "") << ")\n"
          << "  buffer    " << ms (p.bufferMs) << "  (your safety buffer for them)\n"
          << "  codec     " << (p.compression ? ms (codec) + "  (Opus)" : juce::String ("0 ms  (uncompressed)")) << "\n"
          << "  packet    " << ms (rest) << "  (packet size + both sound cards)\n"
          << "  lost      " << juce::String (p.lossPercent, 1) << " % of packets, last 10 s";
        if (std::abs (p.driftPpm) > 0.05f)
            s << "\n  clock     " << juce::String (p.driftPpm, 1) << " ppm against yours";
        return s;
    }

    // Green under 20 ms (feels like playing in one room), amber under 40 ms, pink above
    inline juce::Colour latencyColour (float totalMs)
    {
        return totalMs < 20.0f ? Palette::green : totalMs < 40.0f ? Palette::amber : Palette::pink;
    }

    //==========================================================================
    // Total latency of one player, coloured by how it feels; the tooltip is the breakdown
    class LatencyBadge : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        void set (const jam::PeerInfo& p)
        {
            const juce::String t = p.isSelf ? juce::String ("YOU") : p.connected ? ms (p.totalMs) : juce::String ("--");
            const int k = p.isSelf ? 0 : ! p.connected ? 1 : 2;
            if (t != text || k != kind || (k == 2 && latencyColour (p.totalMs) != latencyColour (total)))
            {
                text = t;
                kind = k;
                total = p.totalMs;
                repaint();
            }
            const auto tip = breakdown (p);
            if (tip != getTooltip())
                setTooltip (tip);
        }

        void paint (juce::Graphics& g) override
        {
            // colour resolved here, so a theme switch shows at once
            const auto colour = kind == 0 ? Palette::dim : kind == 1 ? Palette::pink : latencyColour (total);
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (colour.withAlpha (0.14f));
            g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            g.setColour (colour.withAlpha (0.75f));
            g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);
            g.setColour (colour);
            g.setFont (Theme::font (11.5f, true));
            g.drawText (text, getLocalBounds(), juce::Justification::centred);
        }

    private:
        juce::String text;
        int kind = 0;
        float total = 0.0f;
    };

    // Headless snapshots only: CLOD_JAM_FAKEPLAYERS=<1..6> shows that many made-up players (self first) while the
    // session has none, so the tile grid and the player rows can be checked without a network. Never set in use.
    inline std::vector<jam::PeerInfo> fakePlayers()
    {
        const int n = juce::jlimit (0, jam::maxPlayers, juce::SystemStats::getEnvironmentVariable ("CLOD_JAM_FAKEPLAYERS", "0").getIntValue());
        static const char* const names[] = { "Jorn", "Mika", "Sanne", "Daan", "Lotte", "Ravi" };
        static const char* const chords[] = { "E", "C/G", "", "Am7", "D", "F#m" };
        const juce::uint32 colours[] = { 0xffff7a3d, 0xff4aa8ff, 0xff36e0a0, 0xffa98bff, 0xffffb347, 0xffff5c8a };
        std::vector<jam::PeerInfo> out;
        for (int i = 0; i < n; ++i)
        {
            jam::PeerInfo p;
            p.id = (jam::PeerId) (i + 1);
            p.name = names[i];
            p.colour = juce::Colour (colours[i]);
            p.isSelf = i == 0;
            p.isHost = i == 0;
            p.connected = i != 4;
            p.hasTrack = i == 3;
            p.sendMode = i == 2 ? jam::SendMode::rawInput : jam::SendMode::finishedSound;
            p.compression = i == 5;
            p.pingMs = 8.0f + 9.0f * (float) i;
            p.networkMs = p.pingMs * 0.5f;
            p.jitterMs = 0.6f + 0.5f * (float) i;
            p.bufferMs = 5.3f + (float) i;
            p.totalMs = p.isSelf ? 0.0f : p.networkMs + p.bufferMs + (p.compression ? 5.0f : 0.0f) + 2.7f + 6.0f;
            p.lossPercent = i == 5 ? 2.4f : 0.0f;
            p.driftPpm = 3.0f * (float) i - 6.0f;
            p.levelDb = -6.0f - 7.0f * (float) i;
            p.chord = chords[i];
            p.volume = i == 2 ? 0.7f : 1.0f;
            p.muted = i == 4;
            out.push_back (p);
        }
        return out;
    }

    inline juce::String statusName (jam::Status s)
    {
        switch (s)
        {
            case jam::Status::hosting:   return "HOSTING";
            case jam::Status::joining:   return "JOINING";
            case jam::Status::connected: return "CONNECTED";
            case jam::Status::failed:    return "FAILED";
            case jam::Status::idle:      break;
        }
        return "NOT IN A SESSION";
    }

    inline juce::Colour statusColour (jam::Status s)
    {
        switch (s)
        {
            case jam::Status::hosting:
            case jam::Status::connected: return Palette::green;
            case jam::Status::joining:   return Palette::amber;
            case jam::Status::failed:    return Palette::pink;
            case jam::Status::idle:      break;
        }
        return Palette::dim;
    }

    // Like isShowing() but without needing a window (headless snapshots): visible along the whole parent chain
    inline bool visibleInTree (const juce::Component& c)
    {
        for (auto* p = &c; p != nullptr; p = p->getParentComponent())
            if (! p->isVisible())
                return false;
        return true;
    }

    inline bool inSession (jam::Status s) { return s == jam::Status::hosting || s == jam::Status::connected || s == jam::Status::joining; }

    //==========================================================================
    // Remembered for every Clod Scope on this computer: %APPDATA%\Clod Scope\Jam Sesh.settings
    class Settings
    {
    public:
        Settings()
        {
            juce::PropertiesFile::Options o;
            o.applicationName = "Jam Sesh";
            o.folderName = "Clod Scope";
            o.filenameSuffix = ".settings";
            o.osxLibrarySubFolder = "Application Support";
            o.storageFormat = juce::PropertiesFile::storeAsXML;
            o.millisecondsBeforeSaving = 500;
            file = std::make_unique<juce::PropertiesFile> (o);
        }

        juce::String name() const
        {
            auto n = file->getValue ("name").trim();
            if (n.isEmpty())
                n = juce::SystemStats::getLogonName().trim();
            return n.isNotEmpty() ? n.substring (0, 24) : juce::String ("Player");
        }
        void setName (const juce::String& n)    { file->setValue ("name", n.trim().substring (0, 24)); }
        int port() const                        { return juce::jlimit (1024, 65535, file->getIntValue ("port", jam::defaultPort)); }
        void setPort (int p)                    { file->setValue ("port", p); }
        juce::String lastCode() const           { return file->getValue ("code"); }
        void setLastCode (const juce::String& c) { file->setValue ("code", c.trim()); }

    private:
        std::unique_ptr<juce::PropertiesFile> file;
    };
}
