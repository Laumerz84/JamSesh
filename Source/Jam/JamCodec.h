#pragma once

#include "JamTypes.h"

// Turning audio into packet payloads and back: uncompressed 16-bit PCM, or Opus (third_party/opus,
// linked as the "opus" library) when compression is ticked. Also the audio payload layout that
// travels inside Network::sendAudio. Owner: stream soldier.
//
// Payload v2 (little-endian): a 29-byte header, the main frame, the redundant previous frame (loss
// protection, optional), this packet's MIDI events, then the MIDI of the previous two packets again
// (raw-input mode: a lost packet's note-off still arrives).
//   u8 version, u8 kind, u8 channels, u8 codec, u16 frames, u8 flags, u8 midiCount,
//   u64 sender sample counter (48 kHz), f64 sessionTime at capture, u16 mainBytes, u16 redundantBytes,
//   u8 historyCount
//   MIDI event (6 B): u16 offset in this packet, u8 size, 3 data bytes
//   history event (8 B): i16 start of its packet relative to this one, u16 offset, u8 size, 3 data bytes
struct OpusEncoder;
struct OpusDecoder;

namespace jam
{
    enum class StreamKind : juce::uint8 { player = 0, track = 1, raw = 2 };
    enum class Codec : juce::uint8 { pcm16 = 0, opus = 1 };

    namespace wire
    {
        constexpr int sampleRate = 48000;                 // everything on the wire runs at 48 kHz
        constexpr juce::uint8 version = 2;
        constexpr int headerBytes = 29;
        constexpr int maxFrames = 480;                    // 10 ms
        constexpr int maxMidi = 32;
        constexpr int midiEventBytes = 6;                 // u16 offset, u8 size, 3 data bytes
        constexpr int maxHistory = 32;
        constexpr int historyEventBytes = 8;              // i16 packet start (relative), u16 offset, u8 size, 3 data
        constexpr int maxFrameBytes = maxFrames * 2 * 2;  // PCM16 stereo
        constexpr int maxPacketBytes = headerBytes + 2 * maxFrameBytes + maxMidi * midiEventBytes
                                     + maxHistory * historyEventBytes;
        constexpr int maxSafePayload = 1398;              // above this the IP layer fragments (Network::maxAudioPayload)
        constexpr juce::uint8 flagRedundant = 1;
        constexpr juce::uint8 flagResend = 2;     // a loss-protection copy sent on its own (too big to ride along)

        // Opus frames are 2.5 or 5 ms at 48 kHz; the packet size snaps to them when compression is on.
        // packetSamples 0 = auto: the host's block in 48 kHz samples, 64..128, so a packet leaves with every
        // block (32 stays a manual choice: 1500 packets/s per friend is too much for many home uplinks).
        inline int framesFor (bool compression, int packetSamples, int hostBlock48k = 128) noexcept
        {
            if (compression)
                return packetSamples <= 128 ? 120 : 240;
            if (packetSamples <= 0)
                return juce::jlimit (64, 128, hostBlock48k);
            return juce::jlimit (32, 256, packetSamples);
        }
    }

    struct MidiEvent
    {
        juce::uint16 offset = 0;    // frame inside the packet
        juce::uint8 size = 0;
        juce::uint8 data[3] {};
    };

    struct HistoryEvent
    {
        juce::int16 relStart = 0;   // its packet's start counter minus this packet's
        juce::uint16 offset = 0;
        juce::uint8 size = 0;
        juce::uint8 data[3] {};
    };

    struct PacketHeader
    {
        StreamKind kind = StreamKind::player;
        int channels = 2;
        Codec codec = Codec::pcm16;
        int frames = 0;
        int midiCount = 0, historyCount = 0;
        juce::int64 sampleCounter = 0;
        double sessionTime = 0.0;
        int mainBytes = 0, redundantBytes = 0;   // redundantBytes > 0 = the previous frame rides along
        bool resend = false;                     // this is a copy of an earlier packet
    };

    struct PacketView
    {
        PacketHeader h;
        const juce::uint8* main = nullptr;
        const juce::uint8* redundant = nullptr;
        const juce::uint8* midi = nullptr;
        const juce::uint8* history = nullptr;
        MidiEvent midiAt (int index) const noexcept;
        HistoryEvent historyAt (int index) const noexcept;
    };

    // Returns the payload size, 0 if it does not fit
    int writePacket (juce::uint8* dest, int capacity, const PacketHeader&, const juce::uint8* main,
                     const juce::uint8* redundant, const MidiEvent* midi, const HistoryEvent* history = nullptr) noexcept;

    // Validates every size; false = drop it
    bool parsePacket (const juce::uint8* data, int size, PacketView& out) noexcept;

    namespace pcm16
    {
        // channels 1 (left only) or 2; returns bytes written
        int encode (const float* left, const float* right, int channels, int frames, juce::uint8* dest) noexcept;
        // mono is copied to both outputs
        void decode (const juce::uint8* src, int channels, int frames, float* left, float* right) noexcept;
    }

    // Opus, restricted low delay (CELT only, 2.5 ms look-ahead). Create / destroy on the message thread;
    // encode / decode / conceal are allocation-free.
    class OpusEnc
    {
    public:
        OpusEnc() = default;
        ~OpusEnc();
        bool create (int channels);
        void setKbps (int kbps) noexcept;
        void reset() noexcept;
        // left / right: frames samples (right ignored for mono); returns bytes, 0 on error
        int encode (const float* left, const float* right, int frames, juce::uint8* dest, int capacity) noexcept;
        int channels() const noexcept { return numChannels; }
        bool valid() const noexcept { return enc != nullptr; }

    private:
        ::OpusEncoder* enc = nullptr;
        int numChannels = 0, kbps = 0;
        float interleaved[wire::maxFrames * 2] {};
        JUCE_DECLARE_NON_COPYABLE (OpusEnc)
    };

    class OpusDec
    {
    public:
        OpusDec() = default;
        ~OpusDec();
        bool create();      // always stereo out: mono streams come out on both channels
        void reset() noexcept;
        // data == nullptr: packet loss concealment for frames (a multiple of 120). Returns frames written.
        int decode (const juce::uint8* data, int size, int frames, float* left, float* right) noexcept;
        bool valid() const noexcept { return dec != nullptr; }

    private:
        ::OpusDecoder* dec = nullptr;
        float interleaved[wire::maxFrames * 2] {};
        JUCE_DECLARE_NON_COPYABLE (OpusDec)
    };
}
