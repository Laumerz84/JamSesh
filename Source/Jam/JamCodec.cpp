#include "JamCodec.h"
#include <opus.h>
#include <cstring>

// Owner: stream soldier.
namespace jam
{
    namespace
    {
        void put16 (juce::uint8* p, int v) noexcept    { p[0] = (juce::uint8) v; p[1] = (juce::uint8) (v >> 8); }
        int get16 (const juce::uint8* p) noexcept      { return (int) p[0] | ((int) p[1] << 8); }

        void put64 (juce::uint8* p, juce::uint64 v) noexcept
        {
            for (int i = 0; i < 8; ++i)
                p[i] = (juce::uint8) (v >> (8 * i));
        }

        juce::uint64 get64 (const juce::uint8* p) noexcept
        {
            juce::uint64 v = 0;
            for (int i = 0; i < 8; ++i)
                v |= (juce::uint64) p[i] << (8 * i);
            return v;
        }
    }

    MidiEvent PacketView::midiAt (int index) const noexcept
    {
        MidiEvent e;
        const auto* p = midi + index * wire::midiEventBytes;
        e.offset = (juce::uint16) get16 (p);
        e.size = p[2];
        e.data[0] = p[3]; e.data[1] = p[4]; e.data[2] = p[5];
        return e;
    }

    HistoryEvent PacketView::historyAt (int index) const noexcept
    {
        HistoryEvent e;
        const auto* p = history + index * wire::historyEventBytes;
        e.relStart = (juce::int16) (juce::uint16) get16 (p);
        e.offset = (juce::uint16) get16 (p + 2);
        e.size = p[4];
        e.data[0] = p[5]; e.data[1] = p[6]; e.data[2] = p[7];
        return e;
    }

    int writePacket (juce::uint8* dest, int capacity, const PacketHeader& h, const juce::uint8* main,
                     const juce::uint8* redundant, const MidiEvent* midi, const HistoryEvent* history) noexcept
    {
        const int redundantBytes = redundant != nullptr ? h.redundantBytes : 0;
        const int midiCount = juce::jlimit (0, wire::maxMidi, h.midiCount);
        const int historyCount = history != nullptr ? juce::jlimit (0, wire::maxHistory, h.historyCount) : 0;
        const int total = wire::headerBytes + h.mainBytes + redundantBytes + midiCount * wire::midiEventBytes
                        + historyCount * wire::historyEventBytes;
        if (total > capacity || h.mainBytes > 0xffff || redundantBytes > 0xffff)
            return 0;

        auto* p = dest;
        p[0] = wire::version;
        p[1] = (juce::uint8) h.kind;
        p[2] = (juce::uint8) h.channels;
        p[3] = (juce::uint8) h.codec;
        put16 (p + 4, h.frames);
        p[6] = (juce::uint8) ((redundantBytes > 0 ? wire::flagRedundant : 0) | (h.resend ? wire::flagResend : 0));
        p[7] = (juce::uint8) midiCount;
        put64 (p + 8, (juce::uint64) h.sampleCounter);
        juce::uint64 t;
        std::memcpy (&t, &h.sessionTime, 8);
        put64 (p + 16, t);
        put16 (p + 24, h.mainBytes);
        put16 (p + 26, redundantBytes);
        p[28] = (juce::uint8) historyCount;
        p += wire::headerBytes;

        std::memcpy (p, main, (size_t) h.mainBytes);
        p += h.mainBytes;
        if (redundantBytes > 0)
        {
            std::memcpy (p, redundant, (size_t) redundantBytes);
            p += redundantBytes;
        }
        for (int i = 0; i < midiCount; ++i)
        {
            put16 (p, midi[i].offset);
            p[2] = midi[i].size;
            p[3] = midi[i].data[0]; p[4] = midi[i].data[1]; p[5] = midi[i].data[2];
            p += wire::midiEventBytes;
        }
        for (int i = 0; i < historyCount; ++i)
        {
            put16 (p, (juce::uint16) history[i].relStart);
            put16 (p + 2, history[i].offset);
            p[4] = history[i].size;
            p[5] = history[i].data[0]; p[6] = history[i].data[1]; p[7] = history[i].data[2];
            p += wire::historyEventBytes;
        }
        return total;
    }

    bool parsePacket (const juce::uint8* data, int size, PacketView& out) noexcept
    {
        if (data == nullptr || size < wire::headerBytes || data[0] != wire::version)
            return false;

        auto& h = out.h;
        if (data[1] > 2 || data[3] > 1)
            return false;
        h.kind = (StreamKind) data[1];
        h.channels = data[2];
        h.codec = (Codec) data[3];
        h.frames = get16 (data + 4);
        const bool hasRedundant = (data[6] & wire::flagRedundant) != 0;
        h.resend = (data[6] & wire::flagResend) != 0;
        h.midiCount = data[7];
        h.sampleCounter = (juce::int64) get64 (data + 8);
        const auto t = get64 (data + 16);
        std::memcpy (&h.sessionTime, &t, 8);
        h.mainBytes = get16 (data + 24);
        h.redundantBytes = get16 (data + 26);
        h.historyCount = data[28];

        if (h.channels < 1 || h.channels > 2 || h.frames < 1 || h.frames > wire::maxFrames
             || h.midiCount > wire::maxMidi || h.historyCount > wire::maxHistory || hasRedundant != (h.redundantBytes > 0)
             || h.mainBytes > wire::maxFrameBytes || h.redundantBytes > wire::maxFrameBytes
             || ! std::isfinite (h.sessionTime))
            return false;

        if (h.codec == Codec::pcm16)
        {
            const int expected = h.frames * h.channels * 2;
            if (h.mainBytes != expected || (hasRedundant && h.redundantBytes != expected))
                return false;
        }
        else if (h.frames % 120 != 0 || h.mainBytes < 1)
        {
            return false;
        }

        if (wire::headerBytes + h.mainBytes + h.redundantBytes + h.midiCount * wire::midiEventBytes
              + h.historyCount * wire::historyEventBytes != size)
            return false;

        out.main = data + wire::headerBytes;
        out.redundant = hasRedundant ? out.main + h.mainBytes : nullptr;
        out.midi = out.main + h.mainBytes + h.redundantBytes;
        out.history = out.midi + h.midiCount * wire::midiEventBytes;
        return true;
    }

    namespace pcm16
    {
        static inline juce::int16 toPcm (float x) noexcept
        {
            return (juce::int16) juce::jlimit (-32767, 32767, juce::roundToInt (x * 32767.0f));
        }

        int encode (const float* left, const float* right, int channels, int frames, juce::uint8* dest) noexcept
        {
            auto* p = dest;
            for (int i = 0; i < frames; ++i)
            {
                put16 (p, (juce::uint16) toPcm (left[i]));
                p += 2;
                if (channels > 1)
                {
                    put16 (p, (juce::uint16) toPcm (right[i]));
                    p += 2;
                }
            }
            return (int) (p - dest);
        }

        void decode (const juce::uint8* src, int channels, int frames, float* left, float* right) noexcept
        {
            constexpr float scale = 1.0f / 32767.0f;
            for (int i = 0; i < frames; ++i)
            {
                const float l = (float) (juce::int16) get16 (src) * scale;
                src += 2;
                float r = l;
                if (channels > 1)
                {
                    r = (float) (juce::int16) get16 (src) * scale;
                    src += 2;
                }
                left[i] = l;
                right[i] = r;
            }
        }
    }

    //==============================================================================
    OpusEnc::~OpusEnc()
    {
        if (enc != nullptr)
            opus_encoder_destroy (enc);
    }

    bool OpusEnc::create (int channels)
    {
        int err = 0;
        enc = opus_encoder_create (wire::sampleRate, channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        if (err != OPUS_OK || enc == nullptr)
        {
            enc = nullptr;
            return false;
        }
        numChannels = channels;
        opus_encoder_ctl (enc, OPUS_SET_COMPLEXITY (6));
        opus_encoder_ctl (enc, OPUS_SET_SIGNAL (OPUS_SIGNAL_MUSIC));
        setKbps (128);
        return true;
    }

    void OpusEnc::setKbps (int k) noexcept
    {
        k = juce::jlimit (16, 510, k);
        if (enc != nullptr && k != kbps)
        {
            kbps = k;
            opus_encoder_ctl (enc, OPUS_SET_BITRATE (k * 1000));
        }
    }

    void OpusEnc::reset() noexcept
    {
        if (enc != nullptr)
            opus_encoder_ctl (enc, OPUS_RESET_STATE);
    }

    int OpusEnc::encode (const float* left, const float* right, int frames, juce::uint8* dest, int capacity) noexcept
    {
        if (enc == nullptr || frames > wire::maxFrames)
            return 0;
        const float* in = left;
        if (numChannels == 2)
        {
            for (int i = 0; i < frames; ++i)
            {
                interleaved[2 * i] = left[i];
                interleaved[2 * i + 1] = right[i];
            }
            in = interleaved;
        }
        const int n = opus_encode_float (enc, in, frames, dest, capacity);
        return n > 0 ? n : 0;
    }

    //==============================================================================
    OpusDec::~OpusDec()
    {
        if (dec != nullptr)
            opus_decoder_destroy (dec);
    }

    bool OpusDec::create()
    {
        int err = 0;
        dec = opus_decoder_create (wire::sampleRate, 2, &err);
        if (err != OPUS_OK)
            dec = nullptr;
        return dec != nullptr;
    }

    void OpusDec::reset() noexcept
    {
        if (dec != nullptr)
            opus_decoder_ctl (dec, OPUS_RESET_STATE);
    }

    int OpusDec::decode (const juce::uint8* data, int size, int frames, float* left, float* right) noexcept
    {
        frames = juce::jmin (frames, wire::maxFrames);
        int n = 0;
        if (dec != nullptr)
            n = opus_decode_float (dec, data, data != nullptr ? size : 0, interleaved, frames, 0);
        if (n <= 0)
        {
            std::memset (left, 0, sizeof (float) * (size_t) frames);
            std::memset (right, 0, sizeof (float) * (size_t) frames);
            return frames;
        }
        for (int i = 0; i < n; ++i)
        {
            left[i] = interleaved[2 * i];
            right[i] = interleaved[2 * i + 1];
        }
        for (int i = n; i < frames; ++i)
            left[i] = right[i] = 0.0f;
        return frames;
    }
}
