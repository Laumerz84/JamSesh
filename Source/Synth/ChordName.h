#pragma once

#include <juce_core/juce_core.h>

// Names the notes the Chord tracker hears ("C", "Am7", "G/B", "E5"), for the displays. The root
// is the lowest note when the notes form a known chord on it; otherwise any note that does, written
// as a slash chord over the lowest note. Unknown sets are listed as pitch names from the bass up.
namespace chordname
{
    inline juce::String pitchClass (int midi)
    {
        static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return names[((midi % 12) + 12) % 12];
    }

    // note name with octave, middle C = C4 (A4 = 440 Hz)
    inline juce::String noteName (int midi)
    {
        return midi < 0 ? juce::String ("-") : pitchClass (midi) + juce::String (midi / 12 - 1);
    }

    inline juce::String symbol (const int* notes, int count)
    {
        if (count <= 0)
            return {};
        int bass = notes[0];
        uint16_t set = 0;
        for (int i = 0; i < count; ++i)
        {
            bass = juce::jmin (bass, notes[i]);
            set = (uint16_t) (set | (1u << (((notes[i] % 12) + 12) % 12)));
        }
        const int bassPc = ((bass % 12) + 12) % 12;
        if ((set & (set - 1)) == 0)
            return pitchClass (bass);

        struct Shape { uint16_t mask; const char* suffix; };   // bit i = i semitones above the root
        static const Shape shapes[] = {
            { 0x091, "" },      { 0x089, "m" },     { 0x081, "5" },     { 0x085, "sus2" },  { 0x0A1, "sus4" },
            { 0x049, "dim" },   { 0x111, "aug" },   { 0x491, "7" },     { 0x891, "maj7" },  { 0x489, "m7" },
            { 0x889, "mMaj7" }, { 0x449, "m7b5" },  { 0x249, "dim7" },  { 0x291, "6" },     { 0x289, "m6" },
            { 0x095, "add9" },  { 0x08D, "madd9" }, { 0x495, "9" },     { 0x895, "maj9" },  { 0x48D, "m9" },
            { 0x4A1, "7sus4" }, { 0x295, "6/9" },   { 0x411, "7" },     { 0x409, "m7" },    { 0x811, "maj7" },
            { 0x011, "" },      { 0x009, "m" } };

        for (int pass = 0; pass < 2; ++pass)    // the bass as root first
            for (int r = 0; r < 12; ++r)
            {
                if (((set >> r) & 1) == 0 || (pass == 0) != (r == bassPc))
                    continue;
                const uint16_t rel = (uint16_t) (((set >> r) | (set << (12 - r))) & 0xFFF);
                for (auto& s : shapes)
                    if (rel == s.mask)
                    {
                        juce::String name = pitchClass (r) + s.suffix;
                        if (r != bassPc)
                            name << "/" << pitchClass (bass);
                        return name;
                    }
            }

        juce::StringArray names;
        for (int i = 0; i < 12; ++i)
        {
            const int pc = (bassPc + i) % 12;
            if ((set >> pc) & 1)
                names.add (pitchClass (pc));
        }
        return names.joinIntoString (" ");
    }

    // "C3 E3 G3"
    inline juce::String noteList (const int* notes, int count)
    {
        juce::StringArray s;
        for (int i = 0; i < count; ++i)
            s.add (noteName (notes[i]));
        return s.joinIntoString (" ");
    }
}
