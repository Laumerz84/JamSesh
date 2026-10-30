#pragma once

#include "JamTypes.h"
#include <cmath>
#include <cstring>

// Jam Sesh packet layout (the outer header every UDP packet carries, message types, the session code
// format). Owner: net soldier. The audio payload inside an audio packet is the stream soldier's
// (Jam/JamEngine.cpp); the network only routes it.
//
// Every packet: Header (24 bytes, little endian) + body. Packets with the wrong magic, version or
// session key are dropped before anything else looks at them. Control, state, parameters and view
// state are sent reliably (the receiver acks the sequence number, the sender resends until acked);
// audio, metadata, pings and tempo are not.
namespace jam::protocol
{
    constexpr juce::uint32 magic = 0x4d414a43;      // "CJAM"
    constexpr juce::uint8 version = 1;
    constexpr int headerSize = 24;
    constexpr int maxPacket = 1450;                  // control packets: under a 1500-byte MTU with IP/UDP/VPN overhead
    constexpr int relayPrefix = 4;                   // target PeerId in front of a packet the host forwards
    // Audio may be bigger (PCM with long packets + loss protection). Above ~1400 bytes the IP layer
    // splits it into fragments, and losing any fragment loses the packet, so the engine keeps the
    // usual sizes under that.
    constexpr int maxAudioPayload = 4200;
    constexpr int maxDatagram = maxAudioPayload + 2 * headerSize + relayPrefix;   // the largest packet anyone sends (relayed audio)
    constexpr int stateChunk = 1100;                 // plugin state travels in chunks of this many bytes
    constexpr int maxStateSize = 4 * 1024 * 1024;

    enum class Type : juce::uint8
    {
        joinRequest = 1,   // joiner -> host: name; repeated until welcome
        welcome,           // host -> joiner: your id + colour, host name + colour, how the host sees you
        reject,            // host -> joiner: reason (session full)
        peerList,          // host -> all (reliable, versioned): every player + the endpoint the host sees; on every join / leave
        ping,              // anyone -> anyone: t0 (sender's clock); doubles as heartbeat and hole punch
        pong,              // reply: t0 echoed, t1 receive time, t2 send time (replier's clock)
        bye,               // leaving
        metadata,          // ~10 Hz: name, colour, level, chord, send mode, compression, has-track
        viewState,         // reliable, versioned: ViewState::toBytes
        pluginState,       // reliable chunks of getStateInformation (raw-input mode)
        params,            // reliable, coalesced parameter changes (raw-input mode)
        tempo,             // host -> all ~10 Hz: bpm, bar position, playing, session time it was sampled
        audio,             // opaque payload for the engine
        relay,             // to the host: target PeerId + a whole packet to forward
        ack                // a reliable sequence number arrived
    };

    enum Flags : juce::uint8
    {
        reliable = 1,      // ack me
        viaRelay = 2       // ping / pong through the host relay (measures the path in use)
    };

    struct Header
    {
        juce::uint32 magicValue = magic;
        juce::uint8 versionValue = version;
        Type type = Type::ping;
        juce::uint8 flags = 0;
        juce::uint8 reserved = 0;
        juce::uint64 key = 0;
        PeerId sender = noPeer;
        juce::uint32 seq = 0;
    };

    // ---- little-endian writer / reader with bounds checks (no allocation except str) ----
    struct Writer
    {
        Writer (juce::uint8* d, int cap) noexcept : data (d), capacity (cap) {}
        juce::uint8* data;
        int capacity, pos = 0;
        bool ok = true;

        void raw (const void* src, int n) noexcept
        {
            if (n < 0 || pos + n > capacity) { ok = false; return; }
            if (n > 0) std::memcpy (data + pos, src, (size_t) n);
            pos += n;
        }
        void u8 (juce::uint8 v) noexcept   { raw (&v, 1); }
        void u16 (juce::uint16 v) noexcept { const juce::uint8 b[2] = { (juce::uint8) v, (juce::uint8) (v >> 8) }; raw (b, 2); }
        void u32 (juce::uint32 v) noexcept { u16 ((juce::uint16) v); u16 ((juce::uint16) (v >> 16)); }
        void u64 (juce::uint64 v) noexcept { u32 ((juce::uint32) v); u32 ((juce::uint32) (v >> 32)); }
        void f32 (float v) noexcept        { juce::uint32 u; std::memcpy (&u, &v, 4); u32 (u); }
        void f64 (double v) noexcept       { juce::uint64 u; std::memcpy (&u, &v, 8); u64 (u); }
        void str (const juce::String& s, int maxBytes = 255)
        {
            const char* utf8 = s.toRawUTF8();
            int n = juce::jmin ((int) std::strlen (utf8), maxBytes, 255);
            while (n > 0 && (((juce::uint8) utf8[n]) & 0xc0) == 0x80)   // don't cut a UTF-8 sequence in half
                --n;
            u8 ((juce::uint8) n);
            raw (utf8, n);
        }
        void header (const Header& h) noexcept
        {
            u32 (h.magicValue); u8 (h.versionValue); u8 ((juce::uint8) h.type); u8 (h.flags); u8 (h.reserved);
            u64 (h.key); u32 (h.sender); u32 (h.seq);
        }
    };

    struct Reader
    {
        Reader (const juce::uint8* d, int n) noexcept : data (d), size (n) {}
        const juce::uint8* data;
        int size, pos = 0;
        bool ok = true;

        int remaining() const noexcept { return size - pos; }
        const juce::uint8* take (int n) noexcept
        {
            if (n < 0 || pos + n > size) { ok = false; pos = size; return nullptr; }
            auto* p = data + pos;
            pos += n;
            return p;
        }
        juce::uint8 u8() noexcept   { auto* p = take (1); return p ? p[0] : (juce::uint8) 0; }
        juce::uint16 u16() noexcept { auto* p = take (2); return p ? (juce::uint16) (p[0] | (p[1] << 8)) : (juce::uint16) 0; }
        juce::uint32 u32() noexcept { const juce::uint32 lo = u16(); return lo | ((juce::uint32) u16() << 16); }
        juce::uint64 u64() noexcept { const juce::uint64 lo = u32(); return lo | ((juce::uint64) u32() << 32); }
        float f32() noexcept        { const auto u = u32(); float v; std::memcpy (&v, &u, 4); return std::isfinite (v) ? v : 0.0f; }
        double f64() noexcept       { const auto u = u64(); double v; std::memcpy (&v, &u, 8); return std::isfinite (v) ? v : 0.0; }
        juce::String str()
        {
            const int n = u8();
            auto* p = take (n);
            return p != nullptr ? juce::String::fromUTF8 ((const char*) p, n) : juce::String();
        }
        bool header (Header& h) noexcept
        {
            h.magicValue = u32(); h.versionValue = u8(); h.type = (Type) u8(); h.flags = u8(); h.reserved = u8();
            h.key = u64(); h.sender = u32(); h.seq = u32();
            return ok && h.magicValue == magic && h.versionValue == version;
        }
    };

    // ---- the session code friends type ----
    // "JAM-XXXX-XXXX-..." : Crockford base32 of [port u16][key u64][IPv4 x n][checksum u8]. Every host
    // address worth trying is in it (LAN, VPNs such as Tailscale 100.x / ZeroTier). Also accepted:
    // "ip:port#key" or "ip1,ip2:port#key" (key in hex); port optional (defaultPort).
    struct CodeInfo
    {
        juce::Array<juce::uint32> addresses;   // IPv4, host byte order
        int port = defaultPort;
        juce::uint64 key = 0;
    };

    inline juce::uint32 parseIPv4 (const juce::String& s)
    {
        auto parts = juce::StringArray::fromTokens (s.trim(), ".", "");
        if (parts.size() != 4)
            return 0;
        juce::uint32 v = 0;
        for (auto& p : parts)
        {
            if (p.isEmpty() || p.length() > 3 || ! p.containsOnly ("0123456789") || p.getIntValue() > 255)
                return 0;
            v = (v << 8) | (juce::uint32) p.getIntValue();
        }
        return v;
    }

    inline juce::String ipToString (juce::uint32 ip)
    {
        return juce::String ((ip >> 24) & 255) + "." + juce::String ((ip >> 16) & 255) + "."
             + juce::String ((ip >> 8) & 255) + "." + juce::String (ip & 255);
    }

    inline const char* base32Alphabet() noexcept { return "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; }

    inline juce::uint8 codeChecksum (const juce::uint8* b, int n) noexcept
    {
        juce::uint8 sum = 0x5a;
        for (int i = 0; i < n; ++i)
            sum = (juce::uint8) ((sum * 31) + b[i]);
        return sum;
    }

    inline juce::String makeCode (const CodeInfo& info)
    {
        juce::uint8 bytes[64];
        Writer w (bytes, (int) sizeof (bytes) - 1);
        w.u16 ((juce::uint16) info.port);
        w.u64 (info.key);
        for (int i = 0; i < juce::jmin (8, info.addresses.size()); ++i)
        {
            const auto a = info.addresses[i];
            w.u8 ((juce::uint8) (a >> 24)); w.u8 ((juce::uint8) (a >> 16)); w.u8 ((juce::uint8) (a >> 8)); w.u8 ((juce::uint8) a);
        }
        bytes[w.pos] = codeChecksum (bytes, w.pos);
        const int n = w.pos + 1;

        juce::String chars;
        int acc = 0, bits = 0;
        for (int i = 0; i < n; ++i)
        {
            acc = ((acc << 8) | bytes[i]) & 0xffff;
            bits += 8;
            while (bits >= 5)
            {
                chars += base32Alphabet()[(acc >> (bits - 5)) & 31];
                bits -= 5;
            }
        }
        if (bits > 0)
            chars += base32Alphabet()[(acc << (5 - bits)) & 31];

        juce::String code ("JAM");
        for (int i = 0; i < chars.length(); i += 4)
            code << "-" << chars.substring (i, i + 4);
        return code;
    }

    inline bool parseCode (const juce::String& text, CodeInfo& out)
    {
        out = {};
        auto s = text.trim();

        if (s.containsChar ('#'))   // plain: ip[,ip]:port#key
        {
            auto hostPart = s.upToFirstOccurrenceOf ("#", false, false).trim();
            auto keyPart = s.fromFirstOccurrenceOf ("#", false, false).trim();
            if (keyPart.isEmpty() || keyPart.length() > 16 || ! keyPart.containsOnly ("0123456789abcdefABCDEF"))
                return false;
            out.key = (juce::uint64) keyPart.getHexValue64();

            if (hostPart.containsChar (':'))
            {
                out.port = hostPart.fromLastOccurrenceOf (":", false, false).trim().getIntValue();
                hostPart = hostPart.upToLastOccurrenceOf (":", false, false);
            }
            for (auto& h : juce::StringArray::fromTokens (hostPart, ",; ", ""))
                if (auto ip = parseIPv4 (h))
                    out.addresses.add (ip);
            return out.key != 0 && out.port > 0 && out.port < 65536 && ! out.addresses.isEmpty();
        }

        s = s.toUpperCase();
        if (s.startsWith ("JAM"))
            s = s.substring (3);

        juce::uint8 bytes[64];
        int n = 0, acc = 0, bits = 0;
        for (auto c : s)
        {
            if (c == '-' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
                continue;
            if (c == 'O') c = '0';
            if (c == 'I' || c == 'L') c = '1';
            if (c == 0 || c > 127)
                return false;
            const char* p = std::strchr (base32Alphabet(), (char) c);
            if (p == nullptr)
                return false;
            acc = ((acc << 5) | (int) (p - base32Alphabet())) & 0xffff;
            bits += 5;
            if (bits >= 8)
            {
                if (n >= (int) sizeof (bytes))
                    return false;
                bytes[n++] = (juce::uint8) (acc >> (bits - 8));
                bits -= 8;
            }
        }

        // port (2) + key (8) + at least one address (4) + checksum (1)
        if (n < 15 || (n - 11) % 4 != 0 || codeChecksum (bytes, n - 1) != bytes[n - 1])
            return false;

        Reader r (bytes, n - 1);
        out.port = r.u16();
        out.key = r.u64();
        while (r.remaining() >= 4)
        {
            auto* p = r.take (4);
            out.addresses.add (((juce::uint32) p[0] << 24) | ((juce::uint32) p[1] << 16) | ((juce::uint32) p[2] << 8) | (juce::uint32) p[3]);
        }
        return out.key != 0 && out.port > 0 && ! out.addresses.isEmpty();
    }
}
