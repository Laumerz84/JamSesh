// Loads a VST3 the way a host would: scan, instantiate, process, open editor.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <iostream>
#include <cmath>
#include <set>
bool captureWindow (void* nativeHandle, int& w, int& h, std::vector<unsigned char>& bgra);
void clickWindow (void* nativeHandle, int x, int y, bool dbl);
void dragWindow (void* nativeHandle, int x0, int y0, int x1, int y1, int steps);
void keyWindow (void* nativeHandle, int virtualKey);
void resizeWindow (void* nativeHandle, int w, int h);
void setTopmost (void* nativeHandle, bool topmost);

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 2) { std::cout << "usage: PluginTest <path.vst3>\n"; return 2; }

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, juce::String (argv[1]));
    std::cout << "types found: " << found.size() << "\n";
    if (found.isEmpty()) return 1;
    for (auto* d : found)
        std::cout << "  " << d->name << " | " << d->manufacturerName << " | cat=" << d->category
                  << " | instrument=" << d->isInstrument << " | in=" << d->numInputChannels << " out=" << d->numOutputChannels << "\n";

    juce::String err;
    auto inst = format.createInstanceFromDescription (*found[0], 48000.0, 512, err);
    if (inst == nullptr) { std::cout << "instantiate FAILED: " << err << "\n"; return 1; }
    std::cout << "instantiated ok\n";
    if (std::getenv ("LIST_PARAMS"))
        for (auto* prm : inst->getParameters())
            std::cout << "  param: " << prm->getName (64) << " = " << prm->getCurrentValueAsText() << " (" << prm->getNumSteps() << " steps)\n";

    // ---- PRESET_TEST=<folder>: render every factory program with a plucked-guitar phrase + MIDI notes ----
    if (const char* presetDir = std::getenv ("PRESET_TEST"))
    {
        const double sr = 48000.0;
        const int block = 512;
        inst->prepareToPlay (sr, block);
        const juce::File outDir { juce::String (presetDir) };
        outDir.createDirectory();

        // Karplus-Strong plucks: E2 A2 D3 G3 B3 E4 then an E-major chord, 0.45 s apart (a guitar-like input)
        const double plucks[] = { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 };
        const int phraseLen = (int) (sr * 4.5);
        std::vector<float> input ((size_t) phraseLen, 0.0f);
        juce::Random rng (7);
        auto addPluck = [&] (double hz, int start, float gain)
        {
            const int period = juce::jmax (2, (int) std::round (sr / hz));
            std::vector<float> line ((size_t) period);
            for (auto& v : line) v = rng.nextFloat() * 2.0f - 1.0f;
            float prev = 0.0f;
            for (int i = start; i < phraseLen; ++i)
            {
                auto& v = line[(size_t) ((i - start) % period)];
                const float out = v;
                v = 0.996f * 0.5f * (v + prev);   // averaging = string damping
                prev = out;
                input[(size_t) i] += gain * out;
            }
        };
        for (int k = 0; k < 6; ++k)
            addPluck (plucks[k], (int) (k * 0.45 * sr), 0.35f);
        for (double hz : { 82.41, 123.47, 164.81, 207.65, 246.94 })
            addPluck (hz, (int) (2.9 * sr), 0.16f);

        const int midiStart = phraseLen, total = phraseLen + (int) (sr * 2.0);
        const int numPrograms = inst->getNumPrograms();
        std::cout << "programs: " << numPrograms << "\n";
        for (int p = 0; p < numPrograms; ++p)
        {
            inst->setCurrentProgram (p);
            inst->reset();
            juce::AudioBuffer<float> out (2, total);
            juce::AudioBuffer<float> io (2, block);
            double peak = 0.0, sumSq = 0.0, worstMs = 0.0, totalMs = 0.0;
            bool finite = true;
            int blocks = 0;
            for (int pos = 0; pos < total; pos += block)
            {
                const int n = juce::jmin (block, total - pos);
                io.setSize (2, n, false, false, true);
                for (int i = 0; i < n; ++i)
                {
                    const float v = pos + i < phraseLen ? input[(size_t) (pos + i)] : 0.0f;
                    io.setSample (0, i, v);
                    io.setSample (1, i, v);
                }
                juce::MidiBuffer m;
                // MIDI: C4 E4 G4 chord for 1 s after the guitar phrase (exercises MIDI-driven presets)
                auto at = [&] (int sample) { return sample >= pos && sample < pos + n; };
                if (at (midiStart))
                    for (int note : { 60, 64, 67 })
                        m.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), midiStart - pos);
                if (at (midiStart + (int) sr))
                    for (int note : { 60, 64, 67 })
                        m.addEvent (juce::MidiMessage::noteOff (1, note), midiStart + (int) sr - pos);

                const double t0 = juce::Time::getMillisecondCounterHiRes();
                inst->processBlock (io, m);
                const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
                worstMs = juce::jmax (worstMs, ms);
                totalMs += ms;
                ++blocks;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                    {
                        const float v = io.getSample (ch, i);
                        if (! std::isfinite (v)) finite = false;
                        out.setSample (ch, pos + i, std::isfinite (v) ? v : 0.0f);
                        peak = juce::jmax (peak, (double) std::abs (v));
                        sumSq += (double) v * v;
                    }
            }
            const double rmsDb = juce::Decibels::gainToDecibels (std::sqrt (sumSq / (2.0 * total)), -120.0);
            const double cpu = 100.0 * (totalMs / blocks) / (1000.0 * block / sr);
            const auto name = inst->getProgramName (p);
            std::cout << "  [" << p << "] " << name << ": peak " << juce::String (juce::Decibels::gainToDecibels (peak, -120.0), 1)
                      << " dBFS, rms " << juce::String (rmsDb, 1) << " dB, cpu avg " << juce::String (cpu, 1)
                      << "% worst block " << juce::String (worstMs, 2) << " ms, finite=" << finite
                      << (peak > 1.0 ? "  !!CLIPS" : "") << (rmsDb < -60.0 ? "  !!SILENT?" : "") << "\n";

            auto safe = name.replaceCharacters ("\\/:*?\"<>|", "_________");
            auto file = outDir.getChildFile (juce::String (p).paddedLeft ('0', 3) + " " + safe + ".wav");
            file.deleteFile();
            if (auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream()))
            {
                juce::WavAudioFormat wav;
                if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (stream.get(), sr, 2, 24, {}, 0)))
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer (out, 0, total);
                }
            }
        }
        inst->releaseResources();
        std::cout << "done\n";
        return 0;
    }

    // ---- CHORD_TEST=<folder>: Chord tracking (which notes, how fast) and per-note drive intermodulation ----
    if (const char* chordDir = std::getenv ("CHORD_TEST"))
    {
        const double sr = 48000.0;
        const int block = 512;
        inst->prepareToPlay (sr, block);
        const juce::File outDir { juce::String (chordDir) };
        outDir.createDirectory();

        auto prm = [&] (const juce::String& name) -> juce::AudioProcessorParameter*
        {
            for (auto* p : inst->getParameters())
                if (p->getName (64) == name)
                    return p;
            std::cout << "  !! missing param " << name << "\n";
            return nullptr;
        };
        auto setText = [&] (const juce::String& name, const juce::String& text)
        {
            if (auto* p = prm (name))
                p->setValue (p->getValueForText (text));
        };
        auto setNorm = [&] (const juce::String& name, float v) { if (auto* p = prm (name)) p->setValue (v); };
        auto show = [&] (std::initializer_list<const char*> names)
        {
            for (auto* n : names)
                if (auto* p = prm (n))
                    std::cout << "    " << n << " = " << p->getCurrentValueAsText() << "\n";
        };
        auto hzOf = [] (double note) { return 440.0 * std::pow (2.0, (note - 69.0) / 12.0); };
        auto nameOf = [] (double note)
        {
            static const char* const nm[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            const int n = (int) std::round (note);
            const int c = (int) std::round ((note - n) * 100.0);
            return juce::String (nm[((n % 12) + 12) % 12]) + juce::String (n / 12 - 1) + (c != 0 ? (c > 0 ? "+" : "") + juce::String (c) : juce::String());
        };

        // ---- test signals ----
        juce::Random rng (3);
        bool noisePluck = false;   // false: a pick at 12-30 % of the string (+ a little noise); true: pure noise (stress test)
        auto strum = [&] (std::vector<float>& b, const std::vector<int>& notes, int start, float gain, double spreadMs)
        {
            int k = 0;
            for (int note : notes)
            {
                const int period = juce::jmax (2, (int) std::round (sr / hzOf (note)));
                std::vector<float> line ((size_t) period);
                float lp = 0.0f;
                const float pos = 0.12f + 0.18f * rng.nextFloat();
                for (size_t i = 0; i < line.size(); ++i)
                {
                    lp += 0.6f * ((rng.nextFloat() * 2.0f - 1.0f) - lp);
                    const float x = (float) i / (float) period;
                    const float tri = x < pos ? x / pos : (1.0f - x) / (1.0f - pos);
                    line[i] = noisePluck ? lp : 1.6f * (tri - 0.5f) + 0.25f * lp;
                }
                float prev = 0.0f;
                const int s0 = start + (int) (k++ * spreadMs * 0.001 * sr);
                for (int i = s0; i < (int) b.size(); ++i)
                {
                    auto& v = line[(size_t) ((i - s0) % period)];
                    const float o = v;
                    v = 0.997f * 0.5f * (v + prev);
                    prev = o;
                    b[(size_t) i] += gain * o;
                }
            }
        };
        auto additive = [&] (std::vector<float>& b, const std::vector<int>& notes, int start, float gain, bool organ)
        {
            static const int organK[] = { 1, 2, 3, 4, 6, 8 };
            static const float organA[] = { 1.0f, 0.8f, 0.6f, 0.5f, 0.35f, 0.3f };
            for (int note : notes)
            {
                const double f0 = hzOf (note);
                const int numK = organ ? 6 : 16;
                for (int j = 0; j < numK; ++j)
                {
                    const int k = organ ? organK[j] : j + 1;
                    const double f = k * f0 * (organ ? 1.0 : std::sqrt (1.0 + 1.0e-4 * k * k));
                    if (f > 0.45 * sr)
                        break;
                    const double a = organ ? organA[j] : 1.0 / std::pow ((double) k, 1.1);
                    const double tau = organ ? 1.0e9 : 1.6 / (1.0 + 0.25 * k);
                    const double ph = rng.nextDouble() * 6.283185307179586;
                    for (int i = start; i < (int) b.size(); ++i)
                    {
                        const double t = (i - start) / sr;
                        const double env = std::min (1.0, t / (organ ? 0.015 : 0.002)) * std::exp (-t / tau);
                        b[(size_t) i] += (float) (gain * a * env * std::sin (6.283185307179586 * f * t + ph));
                    }
                }
            }
        };

        // ---- render: input in, mono output back (0.3 s of silence first so the tracker starts empty) ----
        auto render = [&] (const std::vector<float>& input, juce::MidiBuffer* midiAt0)
        {
            inst->reset();
            const int pre = (int) (0.3 * sr);
            const int total = pre + (int) input.size();
            std::vector<float> out ((size_t) total, 0.0f);
            juce::AudioBuffer<float> io (2, block);
            for (int pos = 0; pos < total; pos += block)
            {
                const int n = juce::jmin (block, total - pos);
                io.setSize (2, n, false, false, true);
                for (int i = 0; i < n; ++i)
                {
                    const int j = pos + i - pre;
                    const float v = j >= 0 ? input[(size_t) j] : 0.0f;
                    io.setSample (0, i, v);
                    io.setSample (1, i, v);
                }
                juce::MidiBuffer m;
                if (midiAt0 != nullptr && pos <= pre && pre < pos + n)
                    for (const auto meta : *midiAt0)
                        m.addEvent (meta.getMessage(), pre - pos);
                inst->processBlock (io, m);
                for (int i = 0; i < n; ++i)
                    out[(size_t) (pos + i)] = 0.5f * (io.getSample (0, i) + io.getSample (1, i));
            }
            out.erase (out.begin(), out.begin() + pre);
            return out;
        };

        // magnitude at hz over [from, from + len) with a Hann window (Goertzel)
        auto level = [&] (const std::vector<float>& x, double hz, int from, int len)
        {
            const double w = 6.283185307179586 * hz / sr, c = 2.0 * std::cos (w);
            double s1 = 0.0, s2 = 0.0, wsum = 0.0;
            for (int i = 0; i < len && from + i < (int) x.size(); ++i)
            {
                const double win = 0.5 - 0.5 * std::cos (6.283185307179586 * (i + 0.5) / len);
                const double s0 = x[(size_t) (from + i)] * win + c * s1 - s2;
                s2 = s1;
                s1 = s0;
                wsum += win;
            }
            return std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) * 2.0 / std::max (1.0, wsum);
        };

        // the notes present in out[from .. from + len): peaks of a 1/10-semitone pitch spectrum
        auto notesIn = [&] (const std::vector<float>& x, int from, int len)
        {
            std::vector<double> pitch, mag;
            for (double m = 28.0; m <= 100.0; m += 0.1)
            {
                pitch.push_back (m);
                mag.push_back (level (x, hzOf (m), from, len));
            }
            double mx = 0.0;
            for (auto v : mag) mx = std::max (mx, v);
            std::vector<std::pair<double, double>> found;   // pitch, dB re strongest
            for (size_t i = 1; i + 1 < mag.size(); ++i)
                if (mag[i] > mx * 0.06 && mag[i] > mag[i - 1] && mag[i] >= mag[i + 1])
                {
                    const double a = mag[i - 1], b = mag[i], c = mag[i + 1];
                    const double d = a - 2.0 * b + c;
                    const double p = std::abs (d) > 1.0e-12 ? 0.5 * (a - c) / d : 0.0;
                    found.push_back ({ pitch[i] + 0.1 * p, 20.0 * std::log10 (b / mx) });
                }
            return found;
        };

        struct Case { const char* name; std::vector<int> notes; int kind; };   // kind: 0 strum, 1 piano, 2 organ
        const Case cases[] = {
            { "C major (piano)",            { 60, 64, 67 }, 1 },
            { "A minor (piano)",            { 57, 60, 64 }, 1 },
            { "G7 (piano)",                 { 55, 59, 62, 65 }, 1 },
            { "Cmaj7 (organ)",              { 60, 64, 67, 71 }, 2 },
            { "C-G fifth (piano)",          { 48, 55 }, 1 },
            { "C3-C4 octave (piano)",       { 48, 60 }, 1 },
            { "C-D-E cluster (piano)",      { 60, 62, 64 }, 1 },
            { "E2 single (guitar)",         { 40 }, 0 },
            { "E5 power chord (guitar)",    { 40, 47, 52 }, 0 },
            { "Open E major (guitar)",      { 40, 47, 52, 56, 59, 64 }, 0 },
            { "Open C major (guitar)",      { 48, 52, 55, 60, 64 }, 0 },
            { "Open G major (guitar)",      { 43, 47, 50, 55, 59, 67 }, 0 },
            { "D minor xx0231 (guitar)",    { 50, 57, 62, 65 }, 0 },
            { "Am7 x02010 (guitar)",        { 45, 52, 55, 60, 64 }, 0 },
            { "Fmaj7 xx3210 (guitar)",      { 53, 57, 60, 64 }, 0 },
        };

        auto setupTracking = [&]
        {
            setNorm ("Synth Layer", 1.0f);
            setText ("Synth Source", "Input");
            setText ("Synth Layer Mode", "Replace");
            setText ("Tracking Mode", "Chord");
            setText ("Sculpt Partials", "1");
            setText ("Synth Unison", "1");
            setText ("Filter Cutoff", "20000");
            setText ("Filter Resonance", "0");
            setText ("Follow Input Envelope", "0");
            setText ("Amp Attack", "1");
            setText ("Amp Release", "30");
            setText ("Note Drive", "0");
        };
        setupTracking();
        show ({ "Synth Layer", "Synth Source", "Synth Layer Mode", "Tracking Mode", "Sculpt Partials", "Synth Unison",
                "Filter Cutoff", "Follow Input Envelope", "Amp Release", "Chord Notes", "Chord Sensitivity" });

        std::cout << "\n== chord tracking: voices played as pure sines, steady state 0.4-0.9 s ==\n";
        const juce::String only = std::getenv ("CHORD_CASE") != nullptr ? juce::String (std::getenv ("CHORD_CASE")) : juce::String();
        // per case (strums: 4 random string excitations): the voices still playing at >= -10 dB in the
        // steady window are the result; score bass, ghosts (not a played note), pitch classes, lock time
        int totalRuns = 0, bassOkRuns = 0, cleanRuns = 0, stressRuns = 0, stressBass = 0, stressClean = 0;
        for (const auto& cs : cases)
        {
            if (only.isNotEmpty() && ! juce::String (cs.name).containsIgnoreCase (only))
                continue;
            juce::String exp;
            for (int nn : cs.notes) exp << nameOf (nn) << " ";
            std::cout << "  " << cs.name << "   played: " << exp << "\n";
            const int variants = cs.kind == 0 ? 6 : 1;   // strums: 4 picked, 2 noise-plucked (stress)
            for (int v = 0; v < variants; ++v)
            {
                noisePluck = v >= 4;
                std::vector<float> input ((size_t) (sr * 1.2), 0.0f);
                if (cs.kind == 0)
                    strum (input, cs.notes, 0, 0.22f, 12.0);
                else
                    additive (input, cs.notes, 0, cs.kind == 2 ? 0.08f : 0.12f, cs.kind == 2);
                const auto out = render (input, nullptr);
                const auto heard = notesIn (out, (int) (0.4 * sr), (int) (0.5 * sr));

                juce::String got;
                int ghosts = 0, lockMax = 0;
                double lowest = 1000.0;
                std::set<int> classes, played;
                for (int nn : cs.notes) played.insert (nn % 12);
                for (auto& [p, db] : heard)
                {
                    if (db < -10.0)
                        continue;
                    bool valid = false;
                    for (int nn : cs.notes) valid = valid || std::abs (p - nn) < 0.5;
                    got << nameOf (p) << (valid ? "" : "?") << " ";
                    lowest = std::min (lowest, p);
                    if (! valid) { ++ghosts; continue; }
                    classes.insert (((int) std::round (p)) % 12);
                    // lock: 30 ms windows every 2.5 ms; first centre time at >= -6 dB of steady
                    const int win = (int) (0.030 * sr);
                    const double steady = level (out, hzOf (p), (int) (0.5 * sr), win);
                    for (int s = 0; s + win < (int) (0.5 * sr); s += (int) (0.0025 * sr))
                        if (level (out, hzOf (p), s, win) >= 0.5 * steady)
                        {
                            lockMax = std::max (lockMax, (int) ((s + win / 2) / sr * 1000.0));
                            break;
                        }
                }
                int lowPlayed = 1000;
                for (int nn : cs.notes) lowPlayed = std::min (lowPlayed, nn);
                const bool bassOk = std::abs (lowest - lowPlayed) < 0.5;
                if (noisePluck)
                {
                    ++stressRuns;
                    stressBass += bassOk ? 1 : 0;
                    stressClean += ghosts == 0 ? 1 : 0;
                }
                else
                {
                    ++totalRuns;
                    bassOkRuns += bassOk ? 1 : 0;
                    cleanRuns += ghosts == 0 ? 1 : 0;
                }
                std::cout << "     " << (noisePluck ? "noise " : "") << got.paddedRight (' ', noisePluck ? 28 : 34) << " bass " << (bassOk ? "ok " : "NO ")
                          << " ghosts " << ghosts << "  classes " << (int) classes.size() << "/" << (int) played.size() << "  lock " << lockMax << " ms\n";
            }
        }
        noisePluck = false;
        std::cout << "  SUMMARY: bass right in " << bassOkRuns << "/" << totalRuns << ", no ghost in " << cleanRuns << "/" << totalRuns
                  << "   (noise-plucked stress: bass " << stressBass << "/" << stressRuns << ", no ghost " << stressClean << "/" << stressRuns << ")\n";

        // Mono tracking lock time on single picked notes (a new phrase waits for a steady pitch)
        {
            setText ("Tracking Mode", "Mono");
            juce::String row;
            for (int note : { 69, 55, 40 })
            {
                std::vector<float> input ((size_t) (sr * 0.8), 0.0f);
                strum (input, { note }, 0, 0.3f, 0.0);
                const auto out = render (input, nullptr);
                const int win = (int) (0.020 * sr);
                const double steady = level (out, hzOf (note), (int) (0.4 * sr), win);
                int t = -1;
                for (int s = 0; s + win < (int) (0.4 * sr); s += (int) (0.001 * sr))
                    if (level (out, hzOf (note), s, win) >= 0.5 * steady)
                    {
                        t = (int) ((s + win / 2) / sr * 1000.0);
                        break;
                    }
                row << nameOf (note) << " " << (t < 0 ? juce::String ("--") : juce::String (t) + " ms") << "   ";
            }
            std::cout << "  Mono lock time (synth at -6 dB of its steady level): " << row << "\n";
            setText ("Tracking Mode", "Chord");
        }

        // chord change: C major strum, then G major 0.8 s later: old notes must stop, new ones start
        // (4 runs, each with its own random string excitations)
        for (int run = 0; run < 4; ++run)
        {
            std::vector<float> input ((size_t) (sr * 1.8), 0.0f);
            strum (input, { 48, 52, 55, 60, 64 }, 0, 0.22f, 12.0);
            // damp the first chord at the change (like a guitarist's hand), then strum the next
            const int change = (int) (0.8 * sr);
            for (int i = change; i < (int) input.size(); ++i)
                input[(size_t) i] *= (float) std::exp (-(i - change) / (0.01 * sr));
            strum (input, { 43, 47, 50, 55, 59, 67 }, change, 0.22f, 12.0);
            const auto out = render (input, nullptr);
            juce::String a, b2, tail;
            std::vector<double> beforeNotes;
            for (auto& [p, db] : notesIn (out, (int) (0.35 * sr), (int) (0.4 * sr)))
                if (db >= -10.0) { a << nameOf (p) << " "; beforeNotes.push_back (p); }
            for (auto& [p, db] : notesIn (out, change + (int) (0.35 * sr), (int) (0.4 * sr)))
                if (db >= -10.0) b2 << nameOf (p) << " ";
            // voices of the C chord that are not in G major: their level 0.15-0.55 s after the change
            // relative to 0.4 s before it (long windows: a new note a few Hz away doesn't leak in)
            const int longWin = (int) (0.4 * sr);
            for (double p : beforeNotes)
            {
                bool inG = false;
                for (int nn : { 43, 47, 50, 55, 59, 67 }) inG = inG || std::abs (p - nn) < 0.5;
                if (inG)
                    continue;
                const double before = level (out, hzOf (p), change - longWin, longWin);
                const double after = level (out, hzOf (p), change + (int) (0.15 * sr), longWin);
                tail << nameOf (p) << " " << juce::String (20.0 * std::log10 (std::max (1.0e-9, after) / std::max (1.0e-9, before)), 0) << " dB  ";
            }
            std::cout << "  chord change C -> G (run " << run + 1 << ")   before: " << a.paddedRight (' ', 16) << " after: " << b2.paddedRight (' ', 22)
                      << " old notes after vs before: " << tail << "\n";
        }

        // ---- per-note drive vs one amp on the whole chord: energy away from every note's harmonics ----
        std::cout << "\n== intermodulation: MIDI C4 E4 G4, saw wave ==\n";
        setText ("Synth Source", "MIDI");
        setText ("Sculpt Partials", "64");
        setText ("Amp Release", "200");
        auto imd = [&] (const juce::String& label)
        {
            juce::MidiBuffer m;
            for (int nn : { 60, 64, 67 })
                m.addEvent (juce::MidiMessage::noteOn (1, nn, (juce::uint8) 100), 0);
            std::vector<float> silence ((size_t) (sr * 1.2), 0.0f);
            const auto out = render (silence, &m);
            for (int nn : { 60, 64, 67 })
            {
                juce::MidiBuffer off;
                off.addEvent (juce::MidiMessage::noteOff (1, nn), 0);
                juce::AudioBuffer<float> io (2, 64);
                io.clear();
                inst->processBlock (io, off);
            }
            const int from = (int) (0.5 * sr), len = (int) (0.5 * sr);
            double harm = 0.0, inharm = 0.0, rms = 0.0;
            for (int i = from; i < from + len; ++i) rms += (double) out[(size_t) i] * out[(size_t) i];
            for (double f = 40.0; f < 6000.0; f += 2.0)
            {
                const double e = std::pow (level (out, f, from, len), 2.0);
                bool isHarm = false;
                for (int nn : { 60, 64, 67 })
                {
                    const double f0 = hzOf (nn), k = std::round (f / f0);
                    if (k >= 1.0 && std::abs (f - k * f0) <= std::max (6.0, 0.012 * f))
                        isHarm = true;
                }
                (isHarm ? harm : inharm) += e;
            }
            std::cout << "  " << label << ": energy off the notes' harmonics " << juce::String (10.0 * std::log10 (inharm / std::max (1.0e-20, harm + inharm)), 1)
                      << " dB, output rms " << juce::String (10.0 * std::log10 (rms / len + 1.0e-20), 1) << " dBFS\n";
        };
        setText ("Note Drive", "0");
        imd ("clean (no drive)        ");
        setText ("Note Drive Type", "Overdrive");  setText ("Note Drive", "60"); imd ("note drive Overdrive 60 %");
        setText ("Note Drive Type", "Distortion"); setText ("Note Drive", "80"); imd ("note drive Distortion 80%");
        setText ("Note Drive Type", "Fuzz");       setText ("Note Drive", "80"); imd ("note drive Fuzz 80 %      ");
        setText ("Note Drive", "0");
        setNorm ("Amp On", 1.0f);
        setText ("Amp Model", "High Gain");
        setText ("Amp Gain", "8");
        imd ("one amp High Gain 8      ");
        setNorm ("Amp On", 0.0f);

        // drive loudness vs amount (makeup): output rms of the triad at 0 / 25 / 50 / 75 / 100 % per type
        std::cout << "\n== note drive loudness (rms dBFS) at 0/25/50/75/100 % ==\n";
        for (auto* type : { "Overdrive", "Distortion", "Fuzz" })
        {
            setText ("Note Drive Type", type);
            juce::String row;
            for (int amt : { 0, 25, 50, 75, 100 })
            {
                setText ("Note Drive", juce::String (amt));
                juce::MidiBuffer m;
                for (int nn : { 60, 64, 67 })
                    m.addEvent (juce::MidiMessage::noteOn (1, nn, (juce::uint8) 100), 0);
                std::vector<float> silence ((size_t) (sr * 0.8), 0.0f);
                const auto out = render (silence, &m);
                juce::AudioBuffer<float> io (2, 64);
                juce::MidiBuffer off;
                for (int nn : { 60, 64, 67 }) off.addEvent (juce::MidiMessage::noteOff (1, nn), 0);
                io.clear();
                inst->processBlock (io, off);
                double rms = 0.0;
                for (int i = (int) (0.3 * sr); i < (int) (0.7 * sr); ++i) rms += (double) out[(size_t) i] * out[(size_t) i];
                row << juce::String (10.0 * std::log10 (rms / (0.4 * sr) + 1.0e-20), 1) << "  ";
            }
            std::cout << "  " << type << ": " << row << "\n";
        }
        inst->releaseResources();
        std::cout << "done\n";
        return 0;
    }

    // ---- NOISE_TEST=<folder>: how every preset reacts to guitar string noise (no notes at all: hum,
    //      hiss, handling thumps, finger squeaks, a pick scrape, muted strums, an open string plinked by
    //      a lifting finger, faint sympathetic ringing) compared with how it plays real notes ----
    if (const char* noiseDir = std::getenv ("NOISE_TEST"))
    {
        const double sr = 48000.0;
        const int block = 512;
        inst->prepareToPlay (sr, block);
        const juce::File outDir { juce::String (noiseDir) };
        outDir.createDirectory();
        const double pi2 = 6.283185307179586;
        auto hzOf = [] (double note) { return 440.0 * std::pow (2.0, (note - 69.0) / 12.0); };
        juce::Random rng (21);
        auto T = [sr] (double s) { return (int) std::round (s * sr); };

        // Karplus-Strong string picked at 12-30 % of its length; loop gain `keep` per period
        // (0.997 rings, 0.6 = palm-muted thud); `muteAt` (s, < 0 none) damps it from then on
        auto pluck = [&] (std::vector<float>& b, double note, double at, float gain, float keep, double muteAt)
        {
            const int period = juce::jmax (2, (int) std::round (sr / hzOf (note)));
            std::vector<float> line ((size_t) period);
            const float pos = 0.12f + 0.18f * rng.nextFloat();
            for (size_t i = 0; i < line.size(); ++i)
            {
                const float x = (float) i / (float) period;
                const float tri = x < pos ? x / pos : (1.0f - x) / (1.0f - pos);
                line[i] = 1.6f * (tri - 0.5f) + 0.1f * (rng.nextFloat() * 2.0f - 1.0f);
            }
            float prev = 0.0f;
            const int s0 = T (at), mute = muteAt >= 0.0 ? T (muteAt) : 1 << 30;
            for (int i = s0; i < (int) b.size(); ++i)
            {
                auto& v = line[(size_t) ((i - s0) % period)];
                const float o = v;
                v = (i >= mute ? 0.75f : keep) * 0.5f * (v + prev);
                prev = o;
                b[(size_t) i] += gain * o;
            }
        };

        // the notes: a single-note line (each note stopped as the next starts), then an E major strum
        std::vector<float> notes ((size_t) T (4.5), 0.0f);
        {
            const double ns[] = { 40, 45, 50, 55, 59, 64 };
            for (int k = 0; k < 6; ++k)
                pluck (notes, ns[k], 0.1 + 0.45 * k, 0.3f, 0.997f, 0.1 + 0.45 * (k + 1));
            int k = 0;
            for (double n : { 40.0, 47.0, 52.0, 56.0, 59.0, 64.0 })
                pluck (notes, n, 2.9 + 0.012 * k++, 0.16f, 0.997f, -1.0);
        }

        // the noise: 7 s, no intended note anywhere
        std::vector<float> noise ((size_t) T (7.0), 0.0f);
        for (size_t i = 0; i < noise.size(); ++i)
        {
            const double t = (double) i / sr;
            const double hum = std::sin (pi2 * 60 * t) + 0.5 * std::sin (pi2 * 120 * t + 1.0) + 0.45 * std::sin (pi2 * 180 * t + 2.0)
                             + 0.25 * std::sin (pi2 * 240 * t + 0.5) + 0.2 * std::sin (pi2 * 300 * t + 1.5);
            noise[i] = (float) (0.0021 * hum) + 0.0009f * (rng.nextFloat() * 2.0f - 1.0f);   // -54 dBFS hum, -66 dBFS hiss
        }
        auto thump = [&] (double at, float gain)   // knocking the body / handling the strings
        {
            for (int i = T (at); i < juce::jmin ((int) noise.size(), T (at + 0.25)); ++i)
            {
                const double t = (i - T (at)) / sr;
                noise[(size_t) i] += gain * (float) (std::exp (-t / 0.045) * std::sin (pi2 * 95.0 * t)
                                                     + (t < 0.004 ? (rng.nextFloat() * 2.0f - 1.0f) * 0.5 : 0.0));
            }
        };
        auto squeak = [&] (double at, double dur, double f0, double f1, float gain)   // a finger sliding on a wound string
        {
            double ph = 0.0;
            float hpZ = 0.0f;
            const float hpA = 1.0f - (float) std::exp (-pi2 * 600.0 / sr);
            for (int i = T (at); i < juce::jmin ((int) noise.size(), T (at + dur)); ++i)
            {
                const double u = (i - T (at)) / (dur * sr);
                ph += f0 * std::pow (f1 / f0, u) / sr;
                ph -= std::floor (ph);
                const float pulse = (ph < 0.12 ? 1.0f : 0.0f) * (0.6f + 0.4f * rng.nextFloat());
                hpZ += (pulse - hpZ) * hpA;
                noise[(size_t) i] += gain * (float) std::pow (std::sin (juce::MathConstants<double>::pi * u), 0.7) * (pulse - hpZ);
            }
        };
        auto scrape = [&] (double at, double dur, float gain)   // the pick dragged along a wound string
        {
            float ic1 = 0.0f, ic2 = 0.0f;
            for (int i = T (at); i < juce::jmin ((int) noise.size(), T (at + dur)); ++i)
            {
                const double u = (i - T (at)) / (dur * sr);
                const double fc = 3500.0 * std::pow (500.0 / 3500.0, u);
                const float g = (float) std::tan (juce::MathConstants<double>::pi * fc / sr), k = 0.25f;
                const float x = rng.nextFloat() * 2.0f - 1.0f;
                const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
                const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                const double env = juce::jmin (1.0, u / 0.08) * juce::jmin (1.0, (1.0 - u) / 0.2);
                noise[(size_t) i] += gain * (float) env * k * v1;
            }
        };
        auto chuck = [&] (double at, float gain)   // a palm-muted strum: all six strings thud
        {
            int k = 0;
            for (double n : { 40.0, 45.0, 50.0, 55.0, 59.0, 64.0 })
                pluck (noise, n, at + 0.006 * k++, gain, 0.6f, -1.0);
        };
        thump (0.5, 0.06f);
        squeak (1.0, 0.22, 700.0, 1300.0, 0.05f);
        pluck (noise, 45, 1.6, 0.012f, 0.997f, 2.0);          // a lifting finger plinks the open A string
        scrape (2.3, 0.45, 0.25f);
        chuck (3.2, 0.2f);
        chuck (3.45, 0.2f);
        chuck (3.7, 0.2f);
        squeak (4.4, 0.18, 1500.0, 800.0, 0.07f);
        pluck (noise, 40, 5.0, 0.004f, 0.998f, -1.0);         // faint sympathetic ringing (-48 dBFS)
        pluck (noise, 47, 5.0, 0.004f, 0.998f, -1.0);
        thump (6.1, 0.05f);

        auto render = [&] (const std::vector<float>& input)
        {
            inst->reset();
            std::vector<float> out (input.size(), 0.0f);
            juce::AudioBuffer<float> io (2, block);
            juce::MidiBuffer none;
            for (int pos = 0; pos < (int) input.size(); pos += block)
            {
                const int n = juce::jmin (block, (int) input.size() - pos);
                io.setSize (2, n, false, false, true);
                for (int i = 0; i < n; ++i)
                {
                    io.setSample (0, i, input[(size_t) (pos + i)]);
                    io.setSample (1, i, input[(size_t) (pos + i)]);
                }
                inst->processBlock (io, none);
                for (int i = 0; i < n; ++i)
                    out[(size_t) (pos + i)] = 0.5f * (io.getSample (0, i) + io.getSample (1, i));
            }
            return out;
        };
        // 50 ms RMS windows (dBFS): median over the note render, loudest over the noise render
        auto windows = [&] (const std::vector<float>& x)
        {
            std::vector<double> w;
            const int len = T (0.05);
            for (int s = 0; s + len <= (int) x.size(); s += len)
            {
                double e = 0.0;
                for (int i = s; i < s + len; ++i)
                    e += (double) x[(size_t) i] * x[(size_t) i];
                w.push_back (10.0 * std::log10 (e / len + 1.0e-20));
            }
            return w;
        };
        auto percentile = [] (std::vector<double> v, double q)
        {
            std::sort (v.begin(), v.end());
            return v.empty() ? -200.0 : v[(size_t) juce::jlimit (0, (int) v.size() - 1, (int) (q * (double) v.size()))];
        };
        auto median = [&] (const std::vector<double>& v) { return percentile (v, 0.5); };
        auto maxOf = [] (const std::vector<double>& v) { double m = -200.0; for (double d : v) m = std::max (m, d); return m; };
        // the loudest 50 ms in each noise event's 0.6 s (from its start, so release tails count)
        struct Event { const char* tag; double at; };
        const Event events[] = { { "thump", 0.5 }, { "squeak", 1.0 }, { "plink", 1.6 }, { "scrape", 2.3 },
                                 { "chucks", 3.2 }, { "squeak", 4.4 }, { "ringing", 5.0 }, { "thump", 6.1 } };
        auto eventMax = [&] (const std::vector<double>& w, const Event& e)
        {
            double m = -200.0;
            for (int i = (int) (e.at / 0.05); i < juce::jmin ((int) w.size(), (int) ((e.at + (e.at == 3.2 ? 1.1 : 0.6)) / 0.05)); ++i)
                m = std::max (m, w[(size_t) i]);
            return m;
        };
        auto perEvent = [&] (const std::vector<double>& w)
        {
            juce::String s;
            for (auto& e : events)
                s << e.tag << " " << juce::String (eventMax (w, e), 0) << "  ";
            return s;
        };

        juce::AudioProcessorParameter* synthOn = nullptr;
        for (auto* p : inst->getParameters())
            if (p->getName (64) == "Synth Layer")
                synthOn = p;

        const auto inNoise = windows (noise);
        std::cout << "noise input: loudest 50 ms " << juce::String (maxOf (inNoise), 1) << " dBFS, median "
                  << juce::String (median (inNoise), 1) << " dBFS;  " << perEvent (inNoise) << "\n";
        std::cout << "preset: loudest note | loudest noise (muted strums apart) | margin | muted strums vs loudest note | synth adds  [per event]\n"
                  << "  flagged (!!) when noise comes within 12 dB of the loudest note, or muted strums come out louder than it\n";
        int bad = 0;
        const int numPrograms = inst->getNumPrograms();
        const juce::String onlyPreset = std::getenv ("NOISE_PRESET") != nullptr ? juce::String (std::getenv ("NOISE_PRESET")) : juce::String();
        for (int p = 0; p < numPrograms; ++p)
        {
            if (onlyPreset.isNotEmpty() && ! inst->getProgramName (p).containsIgnoreCase (onlyPreset))
                continue;
            inst->setCurrentProgram (p);
            const auto outNotes = render (notes);
            inst->setCurrentProgram (p);
            const auto outNoise = render (noise);
            inst->setCurrentProgram (p);
            if (synthOn != nullptr)
                synthOn->setValue (0.0f);
            const auto outNoiseOff = render (noise);

            const double sig = maxOf (windows (outNotes));
            const auto wNoise = windows (outNoise);
            double worst = -200.0, chucks = -200.0;
            for (auto& e : events)
            {
                const double m = eventMax (wNoise, e);
                if (juce::String (e.tag) == "chucks")
                    chucks = std::max (chucks, m);
                else
                    worst = std::max (worst, m);
            }
            const double worstOff = maxOf (windows (outNoiseOff));
            const auto name = inst->getProgramName (p);
            const bool flagged = worst > sig - 12.0 || chucks > sig;
            bad += flagged ? 1 : 0;
            std::cout << "  [" << p << "] " << name.paddedRight (' ', 26) << juce::String (sig, 1).paddedLeft (' ', 7) << " | "
                      << juce::String (worst, 1).paddedLeft (' ', 7) << " | " << juce::String (sig - worst, 1).paddedLeft (' ', 6) << " | "
                      << juce::String (chucks - sig, 1).paddedLeft (' ', 6) << " | " << juce::String (maxOf (wNoise) - worstOff, 1).paddedLeft (' ', 6)
                      << (flagged ? " !! " : "    ") << perEvent (wNoise) << "\n";

            juce::AudioBuffer<float> wavBuf (1, (int) outNoise.size());
            for (size_t i = 0; i < outNoise.size(); ++i)
                wavBuf.setSample (0, (int) i, outNoise[i]);
            auto file = outDir.getChildFile (juce::String (p).paddedLeft ('0', 3) + " "
                                             + name.replaceCharacters ("\\/:*?\"<>|", "_________") + " (noise).wav");
            file.deleteFile();
            if (auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream()))
            {
                juce::WavAudioFormat wav;
                if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (stream.get(), sr, 1, 24, {}, 0)))
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer (wavBuf, 0, wavBuf.getNumSamples());
                }
            }
        }
        std::cout << "SUMMARY: " << bad << " of " << numPrograms << " presets let string noise come within 12 dB of their loudest note"
                     " (or muted strums above it)\n";
        inst->releaseResources();
        std::cout << "done\n";
        return 0;
    }

    inst->prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    for (int b = 0; b < 200; ++b)
    {
        for (int i = 0; i < 512; ++i)
            buf.setSample (0, i, std::sin ((b * 512 + i) * 0.05f)), buf.setSample (1, i, std::sin ((b * 512 + i) * 0.05f));
        inst->processBlock (buf, midi);
    }
    std::cout << "processed ok, last sample " << buf.getSample (0, 511) << "\n";

    // Double slit: each sizing mode x ear model, let the IR build, check output stays finite and audible
    auto findParam = [&] (const juce::String& name) -> juce::AudioProcessorParameter*
    {
        for (auto* prm : inst->getParameters())
            if (prm->getName (64) == name) return prm;
        return nullptr;
    };
    auto* on = findParam ("Double Slit");
    auto* sizing = findParam ("Slit Sizing");
    auto* ear = findParam ("Ear Model");
    std::cout << "double slit params: " << (on && sizing && ear ? "found" : "MISSING") << "\n";
    if (on && sizing && ear)
    {
        on->setValue (1.0f);
        const char* sizingNames[] = { "per-freq", "pitch", "fixed" };
        const char* earNames[] = { "physical", "bright", "weighted" };
        for (int sz = 0; sz < 3; ++sz)
            for (int e = 0; e < 3; ++e)
            {
                sizing->setValue (sz / 2.0f);
                ear->setValue (e / 2.0f);
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                double peak = 0.0, sumSq = 0.0; bool finite = true; long count = 0;
                for (int b = 0; b < 300; ++b)
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        const float v = 0.5f * std::sin ((b * 512 + i) * 2.0f * 3.14159265f * 220.0f / 48000.0f);
                        buf.setSample (0, i, v); buf.setSample (1, i, v);
                    }
                    inst->processBlock (buf, midi);
                    if (b > 200)
                        for (int i = 0; i < 512; ++i)
                        {
                            const float v = buf.getSample (0, i);
                            if (! std::isfinite (v)) finite = false;
                            peak = std::max (peak, (double) std::abs (v)); sumSq += v * v; ++count;
                        }
                    juce::Thread::sleep (b < 100 ? 10 : 0);
                }
                std::cout << "  " << sizingNames[sz] << " / " << earNames[e] << ": finite=" << finite
                          << " peak=" << peak << " rms=" << std::sqrt (sumSq / count)
                          << " (" << (int) (juce::Time::getMillisecondCounterHiRes() - t0) << " ms)\n";
            }
    }

    std::unique_ptr<juce::AudioProcessorEditor> ed (inst->createEditorIfNeeded());
    std::cout << "editor: " << (ed ? "ok" : "none") << "\n";
    if (ed && std::getenv ("GUI_TEST") == nullptr)
    {
        ed->setVisible (true); ed->addToDesktop (0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (500);
    }
    else if (ed)
    {
        // Drive every main view / render mode with a stereo chord, time the message thread, screenshot each
        const juce::File shotDir (juce::String (std::getenv ("GUI_TEST")));
        shotDir.createDirectory();
        ed->setTopLeftPosition (20, 20);
        ed->addToDesktop (juce::ComponentPeer::windowHasTitleBar);
        ed->setVisible (true);
        ed->toFront (true);

        auto setChoice = [&] (const juce::String& name, int index)
        {
            if (auto* prm = findParam (name))
                prm->setValue (prm->getNumSteps() > 1 ? (float) index / (float) (prm->getNumSteps() - 1) : (float) index);
            else
                std::cout << "  !! missing param " << name << "\n";
        };

        // hosted int params report a huge step count, so map the value into 0..1 directly
        auto setInt = [&] (const juce::String& name, int value, int lo, int hi)
        {
            if (auto* prm = findParam (name))
                prm->setValue ((float) (value - lo) / (float) (hi - lo));
            else
                std::cout << "  !! missing param " << name << "\n";
        };

        long sampleIndex = 0;
        float level = 1.0f;   // 0 = silence
        double toneHz = 0.0;  // > 0: a single pure tone instead of the chord
        bool plucking = false;   // loop a Karplus-Strong guitar phrase instead
        std::vector<float> pluckLoop;
        {
            const double sr = 48000.0;
            const int len = (int) (sr * 3.0);
            pluckLoop.assign ((size_t) len, 0.0f);
            juce::Random rng (11);
            const double notes[] = { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 };
            for (int k = 0; k < 6; ++k)
            {
                const int period = (int) std::round (sr / notes[k]), start = (int) (k * 0.5 * sr);
                std::vector<float> line ((size_t) period);
                for (auto& v : line) v = rng.nextFloat() * 2.0f - 1.0f;
                float prev = 0.0f;
                for (int i = start; i < len; ++i)
                {
                    auto& v = line[(size_t) ((i - start) % period)];
                    const float o = v;
                    v = 0.996f * 0.5f * (v + prev);
                    prev = o;
                    pluckLoop[(size_t) i] += 0.4f * o;
                }
            }
        }
        juce::MidiBuffer pendingMidi;
        auto pump = [&] (int ms, const juce::String& label)
        {
            double worst = 0.0, total = 0.0, peak = 0.0;
            int count = 0;
            bool finite = true;
            const double end = juce::Time::getMillisecondCounterHiRes() + ms;
            while (juce::Time::getMillisecondCounterHiRes() < end)
            {
                midi.clear();
                midi.swapWith (pendingMidi);
                for (int i = 0; i < 512; ++i, ++sampleIndex)
                {
                    const double t = sampleIndex / 48000.0, tau = 6.283185307179586;
                    if (plucking)
                    {
                        const float v = level * pluckLoop[(size_t) (sampleIndex % (long) pluckLoop.size())];
                        buf.setSample (0, i, v);
                        buf.setSample (1, i, v);
                        continue;
                    }
                    if (toneHz > 0.0)
                    {
                        const float v = level * 0.4f * (float) std::sin (tau * toneHz * t);
                        buf.setSample (0, i, v);
                        buf.setSample (1, i, v);
                        continue;
                    }
                    buf.setSample (0, i, level * (float) (0.3 * std::sin (tau * 220 * t) + 0.2 * std::sin (tau * 330 * t) + 0.1 * std::sin (tau * 440 * t)));
                    buf.setSample (1, i, level * (float) (0.3 * std::sin (tau * 220 * t + 0.6) + 0.2 * std::sin (tau * 330 * t) + 0.1 * std::sin (tau * 440 * t + 1.2)));
                }
                inst->processBlock (buf, midi);
                for (int i = 0; i < 512; ++i)
                {
                    const float v = buf.getSample (0, i);
                    if (! std::isfinite (v)) finite = false;
                    peak = std::max (peak, (double) std::abs (v));
                }
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
                const double busy = juce::Time::getMillisecondCounterHiRes() - t0 - 10.0;
                worst = std::max (worst, busy);
                total += std::max (0.0, busy);
                ++count;
            }
            std::cout << "  " << label << ": msg-thread overrun avg " << juce::String (total / juce::jmax (1, count), 1)
                      << " ms, worst " << juce::String (worst, 1) << " ms; audio finite=" << finite
                      << " peak=" << juce::String (peak, 3) << "\n";
        };

        auto shot = [&] (const juce::String& name)
        {
            int w = 0, h = 0;
            std::vector<unsigned char> px;
            if (auto* peer = ed->getPeer())
                if (captureWindow (peer->getNativeHandle(), w, h, px) && w > 0 && h > 0)
                {
                    juce::Image img (juce::Image::RGB, w, h, false);
                    juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                        {
                            const auto* p = &px[((size_t) y * (size_t) w + (size_t) x) * 4];
                            bd.setPixelColour (x, y, juce::Colour (p[2], p[1], p[0]));
                        }
                    auto f = shotDir.getChildFile (name + ".png");
                    f.deleteFile();
                    juce::FileOutputStream out (f);
                    juce::PNGImageFormat().writeImageToStream (img, out);
                }
        };

        // GUI_SCRIPT=<file>: one command per line (coordinates are screenshot pixels):
        //   wait <ms> | shot <name> | click <x> <y> | dblclick <x> <y> | drag <x0> <y0> <x1> <y1>
        //   key <virtualKeyCode> | size <w> <h> | program <index>
        //   param <Name> = <normalised 0..1> | choice <Name> = <index> | int <Name> = <value> <lo> <hi>
        //   tone <hz> | chord | plucks | silence | level <gain> | note <midiNote> on/off
        if (const char* scriptPath = std::getenv ("GUI_SCRIPT"))
        {
            juce::StringArray lines;
            juce::File (juce::String (scriptPath)).readLines (lines);
            void* hwnd = ed->getPeer() != nullptr ? ed->getPeer()->getNativeHandle() : nullptr;
            setTopmost (hwnd, true);   // clicks only land on a window that is really visible
            for (auto line : lines)
            {
                line = line.trim();
                if (line.isEmpty() || line.startsWith ("#"))
                    continue;
                auto tok = juce::StringArray::fromTokens (line, " ", "");
                const auto cmd = tok[0];
                auto rest = line.fromFirstOccurrenceOf (" ", false, false).trim();
                std::cout << "> " << line << "\n";
                if (cmd == "wait")          pump (tok[1].getIntValue(), "wait");
                else if (cmd == "shot")     shot (rest);
                else if (cmd == "click")    clickWindow (hwnd, tok[1].getIntValue(), tok[2].getIntValue(), false);
                else if (cmd == "dblclick") clickWindow (hwnd, tok[1].getIntValue(), tok[2].getIntValue(), true);
                else if (cmd == "drag")     dragWindow (hwnd, tok[1].getIntValue(), tok[2].getIntValue(), tok[3].getIntValue(), tok[4].getIntValue(), 12);
                else if (cmd == "key")      keyWindow (hwnd, tok[1].getIntValue());
                else if (cmd == "size")     resizeWindow (hwnd, tok[1].getIntValue(), tok[2].getIntValue());
                else if (cmd == "program")  inst->setCurrentProgram (tok[1].getIntValue());
                else if (cmd == "tone")     { toneHz = tok[1].getDoubleValue(); plucking = false; level = 1.0f; }
                else if (cmd == "chord")    { toneHz = 0.0; plucking = false; level = 1.0f; }
                else if (cmd == "plucks")   { plucking = true; level = 1.0f; }
                else if (cmd == "silence")  level = 0.0f;
                else if (cmd == "level")    level = (float) tok[1].getDoubleValue();
                else if (cmd == "note")
                {
                    const int nn = tok[1].getIntValue();
                    pendingMidi.addEvent (tok[2] == "on" ? juce::MidiMessage::noteOn (1, nn, (juce::uint8) 100)
                                                         : juce::MidiMessage::noteOff (1, nn), 0);
                }
                else if (cmd == "param" || cmd == "choice" || cmd == "int")
                {
                    const auto name = rest.upToFirstOccurrenceOf ("=", false, false).trim();
                    auto vals = juce::StringArray::fromTokens (rest.fromFirstOccurrenceOf ("=", false, false).trim(), " ", "");
                    if (cmd == "param")       { if (auto* prm = findParam (name)) prm->setValue ((float) vals[0].getDoubleValue()); else std::cout << "  !! missing " << name << "\n"; }
                    else if (cmd == "choice") setChoice (name, vals[0].getIntValue());
                    else                      setInt (name, vals[0].getIntValue(), vals[1].getIntValue(), vals[2].getIntValue());
                }
                else
                    std::cout << "  ?? unknown command\n";
            }
            setTopmost (hwnd, false);
            ed.reset();
            inst->releaseResources();
            std::cout << "done\n";
            return 0;
        }

        if (std::getenv ("SHAPES_TEST") != nullptr)
        {
            // single notes up from C3 on sodium: each semitone should be the next subshell's shape
            setChoice ("Main View", 2);
            setChoice ("Orbital Render", 1);
            const std::pair<const char*, double> notes[] = { { "C3_3s", 130.81 }, { "Cs3_3p", 138.59 }, { "Ds3_3d", 155.56 },
                                                             { "Fs3_4d", 185.00 }, { "G3_4f", 196.00 } };
            for (auto& [name, hz] : notes)
            {
                toneHz = hz;
                pump (2500, juce::String ("note ") + name);
                shot (juce::String ("s_") + name);
            }
            toneHz = 138.59;
            setChoice ("Orbital Colour", 1);
            pump (2000, "3p phase colour");          shot ("s_Cs3_3p_phase");
            toneHz = 0.0;
            setChoice ("Orbital Colour", 0);
            if (auto* solo = findParam ("Strongest State Only")) solo->setValue (1.0f);
            pump (2500, "chord, strongest only");    shot ("s_chord_solo");
            ed.reset();
            inst->releaseResources();
            std::cout << "done\n";
            return 0;
        }

        if (std::getenv ("ORBITAL_TEST") != nullptr)
        {
            // the sound is the electron: silence must be empty, sound must appear fast and fade out after
            setChoice ("Main View", 2);
            setChoice ("Orbital Render", 1);
            level = 0.0f;  pump (2500, "Atom Na, silence");          shot ("a_na_silence");
            level = 1.0f;  pump (300, "Atom Na, chord first 300 ms"); shot ("b_na_attack_300ms");
            pump (2200, "Atom Na, chord");                            shot ("c_na_chord");
            level = 0.15f; pump (1500, "Atom Na, quiet chord");       shot ("d_na_quiet");
            level = 1.0f;
            setInt ("Element Z", 19, 1, 86);  pump (2500, "Atom K");      shot ("e_k_chord");
            setInt ("Element Z", 26, 1, 86);  pump (2500, "Atom Fe");     shot ("f_fe_chord");
            setInt ("Element Z", 11, 1, 86);
            setChoice ("Level Mapping", 2); setChoice ("Orbital Colour", 1);
            pump (2500, "Atom Na emission phase");                    shot ("g_na_emission_phase");
            setChoice ("Level Mapping", 0); setChoice ("Orbital Colour", 0);
            if (auto* sc = findParam ("Screening")) sc->setValue (0.0f);
            pump (2500, "Na screening 0 (bare ion)");                 shot ("h_na_bare_ion");
            if (auto* sc = findParam ("Screening")) sc->setValue (1.0f / 1.5f);
            level = 0.0f;  pump (400, "silence 400 ms after");        shot ("i_fade_400ms");
            pump (1500, "silence 1.9 s after");                       shot ("j_fade_1900ms");
            ed.reset();
            inst->releaseResources();
            std::cout << "done\n";
            return 0;
        }

        pump (2500, "2D scope");                   shot ("1_scope2d");
        setChoice ("Main View", 1);
        const char* views[] = { "helix", "phase", "stereo", "waterfall" };
        for (int v = 0; v < 4; ++v)
        {
            setChoice ("3D View", v);
            pump (2500, juce::String ("3D ") + views[v]);
            shot (juce::String ("2_3d_") + juce::String (v) + "_" + views[v]);
        }
        setChoice ("Main View", 2);
        const char* modes[] = { "slices", "voxels", "points" };
        for (int m = 0; m < 3; ++m)
        {
            setChoice ("Orbital Render", m);
            pump (3000, juce::String ("Orbital ") + modes[m]);
            shot (juce::String ("3_orb_") + juce::String (m) + "_" + modes[m]);
        }
        setChoice ("Orbital Render", 1);
        setChoice ("Potential", 1);
        setChoice ("Level Mapping", 2);
        setChoice ("Orbital Colour", 1);
        pump (3500, "Orbital hydrogen emission phase");  shot ("4_orb_hydrogen_emission_phase");
        setChoice ("Level Mapping", 1);
        setChoice ("Angular Source", 1);
        pump (3000, "Orbital rydberg stereo");           shot ("5_orb_rydberg_stereo");

        setChoice ("Main View", 0);
        setChoice ("Wave Equation", 1);
        for (int p = 0; p < 11; ++p)
        {
            setChoice ("Equation Family", p);
            pump (600, juce::String ("Equation preset ") + juce::String (p));
        }
        shot ("6_equation_last");
    }
    ed.reset();
    inst->releaseResources();
    inst.reset();
    std::cout << "done\n";
    return 0;
}
