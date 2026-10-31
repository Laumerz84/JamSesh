// The standalone app (Jam Sesh.exe, CLOD_APP_NAME from CMakeLists.txt): a window holding the same editor as
// the plugin, playing through the computer's audio interface (ASIO or Windows audio) and MIDI inputs.
// JUCE's StandalonePluginHolder runs the audio device; the window is our own (no "Options" title-bar
// button: the editor's StandaloneBar has the audio settings, input, tempo, backing track and recorder).
//   Jam Sesh.exe --selftest <outDir>   headless check: no window, no audio device; writes selftest.txt (the
//                                      data folders it uses are in <outDir>\selftest-data, never the user's)
// Owner: standalone soldier.
#include <juce_core/system/juce_TargetPlatform.h>

#if JucePlugin_Build_Standalone && JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "AppServices.h"
#include "LowDelay.h"
#include "StandaloneBar.h"
#include "../AppPaths.h"
#include "../PluginProcessor.h"
#include "../UI/Theme.h"
#include "../UI/Themes.h"
#include "../UI/StreamSafe.h"
#include "../UI/Editor/Chain.h"
#include "../Jam/JamUiCommon.h"
#include "../Jam/JamView.h"

namespace
{
    //==============================================================================
    // Windows power throttling (EcoQoS) off for this process: Windows 11 may otherwise run a background or
    // minimized app on slow cores and ignore its 1 ms timer request, and the audio callback misses its deadline.
    // The app only, never the plugin (inside a host the process is the host's). Each part on its own: the
    // timer-resolution part needs Windows 11 22H2, the execution-speed part Windows 10 1709; older Windows just
    // says no. Returns bit 0 = execution speed, bit 1 = timer resolution, for the self-test's log.
    int turnOffPowerThrottling()
    {
       #if JUCE_WINDOWS
        struct ThrottlingState { unsigned long version, controlMask, stateMask; };   // PROCESS_POWER_THROTTLING_STATE
        using SetInfo = int (__stdcall*) (void*, int, void*, unsigned long);         // SetProcessInformation
        constexpr int processPowerThrottling = 4;                                     // PROCESS_INFORMATION_CLASS
        constexpr unsigned long executionSpeed = 0x1, ignoreTimerResolution = 0x4;

        static juce::DynamicLibrary kernel ("kernel32.dll");
        auto setInfo = (SetInfo) kernel.getFunction ("SetProcessInformation");
        if (setInfo == nullptr)
            return 0;
        void* const thisProcess = reinterpret_cast<void*> ((juce::pointer_sized_int) -1);   // GetCurrentProcess()
        int done = 0;
        for (auto [mask, bit] : { std::pair<unsigned long, int> { executionSpeed, 1 }, { ignoreTimerResolution, 2 } })
        {
            ThrottlingState st { 1, mask, 0 };   // version 1; control this part; state 0 = not throttled
            if (setInfo (thisProcess, processPowerThrottling, &st, (unsigned long) sizeof (st)) != 0)
                done |= bit;
        }
        return done;
       #else
        return 0;
       #endif
    }

    //==============================================================================
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (juce::StandalonePluginHolder& h, juce::PropertySet* s)
            : DocumentWindow (CLOD_APP_NAME, Palette::bg, DocumentWindow::allButtons), holder (h), settings (s)
        {
            setUsingNativeTitleBar (true);
            streamsafe::useSoftwareRenderer (*this);

            if (auto* ed = holder.processor->createEditorIfNeeded())
            {
                setContentOwned (ed, true);
                setResizable (ed->isResizable(), false);
                if (auto* c = ed->getConstrainer())
                    setResizeLimits (c->getMinimumWidth(), c->getMinimumHeight(), c->getMaximumWidth(), c->getMaximumHeight());
            }

            const auto saved = settings != nullptr ? settings->getValue ("clodWindow") : juce::String();
            if (saved.isEmpty() || ! restoreWindowStateFromString (saved) || ! isOnScreen())
                centreWithSize (getWidth(), getHeight());
        }

        ~MainWindow() override
        {
            saveState();
            clearContentComponent();          // the editor goes before the processor
        }

        void saveState()
        {
            if (settings != nullptr)
                settings->setValue ("clodWindow", getWindowStateAsString());
        }

        void closeButtonPressed() override
        {
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                app->systemRequestedQuit();
        }

        void lookAndFeelChanged() override
        {
            DocumentWindow::lookAndFeelChanged();
            setBackgroundColour (Palette::bg);
        }

    private:
        bool isOnScreen() const
        {
            const auto& displays = juce::Desktop::getInstance().getDisplays();
            for (auto& d : displays.displays)
                if (d.userArea.intersects (getBounds().withHeight (40)))   // the title bar is reachable
                    return true;
            return false;
        }

        juce::StandalonePluginHolder& holder;
        juce::PropertySet* settings;
    };

    //==============================================================================
    // --selftest: the processor, the backing-track player, the recorder and tap tempo, with no window
    // and no audio device. Everything is driven from here as an audio callback would.
    struct SelfTest
    {
        juce::String log;
        bool ok = true;

        void line (const juce::String& s) { log << s << "\n"; }
        void check (bool cond, const juce::String& what)
        {
            line ((cond ? "PASS  " : "FAIL  ") + what);
            ok = ok && cond;
        }

        static double rms (const std::vector<float>& v, size_t from, size_t to)
        {
            double s = 0.0;
            to = juce::jmin (to, v.size());
            for (auto i = from; i < to; ++i)
                s += (double) v[i] * v[i];
            return to > from ? std::sqrt (s / (double) (to - from)) : 0.0;
        }

        static double frequency (const std::vector<float>& v, size_t from, size_t to, double sr)
        {
            int crossings = 0;
            size_t first = 0, last = 0;
            to = juce::jmin (to, v.size());
            for (auto i = from + 1; i < to; ++i)
                if (v[i - 1] < 0.0f && v[i] >= 0.0f)
                {
                    if (crossings == 0)
                        first = i;
                    last = i;
                    ++crossings;
                }
            return crossings > 1 ? (crossings - 1) * sr / (double) (last - first) : 0.0;
        }

        // a plucked string (Karplus-Strong): E2 A2 D3 G3, one second each, then again
        struct Guitar
        {
            explicit Guitar (double sampleRate) : sr (sampleRate) {}
            float next()
            {
                if (n % (int) sr == 0)
                    pluck (notes[(n / (int) sr) % 4]);
                ++n;
                const int len = (int) line.size();
                const float y = 0.498f * (line[(size_t) pos] + line[(size_t) ((pos + 1) % len)]);
                line[(size_t) pos] = y;
                pos = (pos + 1) % len;
                return y;
            }
            void pluck (double hz)
            {
                line.assign ((size_t) juce::jmax (2, (int) (sr / hz)), 0.0f);
                for (auto& x : line)
                    x = (rng.nextFloat() * 2.0f - 1.0f) * 0.35f;
                pos = 0;
            }
            double sr;
            const double notes[4] { 82.41, 110.0, 146.83, 196.0 };
            std::vector<float> line { 0.0f, 0.0f };
            int pos = 0, n = 0;
            juce::Random rng { 1234 };
        };

        // ---- 0. the data folders (AppPaths.h), all under dataRoot (the user's are never touched): an install
        //      from before 1.4.0 keeps its files in "Clod Scope" folders, which move to "Jam Sesh" once ----
        void dataFolders (const juce::File& dataRoot)
        {
            dataRoot.deleteRecursively();   // the last self-test's
            const auto oldSettings = dataRoot.getChildFile ("AppData").getChildFile ("Clod Scope");
            const auto oldDocs = dataRoot.getChildFile ("Documents").getChildFile ("Clod Scope");
            const auto newSettings = dataRoot.getChildFile ("AppData").getChildFile ("Jam Sesh");
            const auto newDocs = dataRoot.getChildFile ("Documents").getChildFile ("Jam Sesh");
            const juce::String theme = "theme=" + Themes::name (1) + "\n";
            oldSettings.getChildFile ("Theme.settings").create();
            oldSettings.getChildFile ("Theme.settings").replaceWithText (theme);
            {
                juce::PropertiesFile::Options o;   // as jamui::Settings and the app write them (XML)
                juce::PropertiesFile jam (oldSettings.getChildFile ("Jam Sesh.settings"), o);
                jam.setValue ("name", "Selftest player");
                juce::PropertiesFile app (oldSettings.getChildFile ("Clod Scope.settings"), o);
                app.setValue ("clodWindow", "selftest window");
            }
            oldDocs.getChildFile ("Presets").getChildFile ("Selftest.clodpreset").create();
            oldDocs.getChildFile ("Presets").getChildFile ("Selftest.clodpreset").replaceWithText ("<ClodScopePreset name=\"Selftest preset\"/>\n");
            oldDocs.getChildFile ("Recordings").getChildFile ("old take.wav").create();
            oldDocs.getChildFile ("Recordings").getChildFile ("old take.wav").replaceWithText ("not a real wav");

            const auto s = apppaths::settingsFolder();
            const auto d = apppaths::documentsFolder();
            const auto appFile = apppaths::appSettingsFile();
            line ("data folders: settings " + s.dir.getFullPathName() + ", documents " + d.dir.getFullPathName()
                  + ", app settings " + appFile.getFileName());
            check (s.dir == newSettings && ! s.old && ! oldSettings.exists()
                       && newSettings.getChildFile ("Theme.settings").loadFileAsString().trim() == theme.trim(),
                   "old settings folder moved to Jam Sesh, Theme.settings intact");
            const bool themeForced = juce::SystemStats::getEnvironmentVariable ("CLOD_THEME", {}).isNotEmpty();
            check (themeForced || Themes::current() == 1, "the theme is read from the new folder (" + Themes::name (Themes::current())
                                                              + (themeForced ? ", forced by CLOD_THEME" : "") + ")");
            {
                juce::PropertiesFile app (appFile, juce::PropertiesFile::Options());
                check (appFile == newSettings.getChildFile ("App.settings") && app.getValue ("clodWindow") == "selftest window"
                           && ! newSettings.getChildFile ("Clod Scope.settings").exists(),
                       "the app's Clod Scope.settings is now App.settings, content kept");
            }
            {
                juce::SharedResourcePointer<jamui::Settings> jam;
                check (jamui::Settings::location() == newSettings.getChildFile ("Jam Sesh.settings") && jam->name() == "Selftest player",
                       "Jam Sesh.settings (your name) read from the new folder");
            }
            check (d.dir == newDocs && ! d.old && ! oldDocs.exists()
                       && newDocs.getChildFile ("Presets").getChildFile ("Selftest.clodpreset").existsAsFile()
                       && newDocs.getChildFile ("Recordings").getChildFile ("old take.wav").loadFileAsString() == "not a real wav",
                   "old documents folder moved to Jam Sesh (presets, recordings intact)");
            check (Recorder::recordingsFolder() == newDocs.getChildFile ("Recordings") && apppaths::settings() == newSettings
                       && ! apppaths::allowMoves(),
                   "recordings go to Jam Sesh\\Recordings; the folders stay decided");

            // a processor alone (the VST3 build's moduleinfo step, a plugin scan, PluginTest) moves nothing: the
            // old folder until the window opens or the app starts
            {
                const auto base = dataRoot.getChildFile ("scan");
                base.getChildFile ("Clod Scope").getChildFile ("Presets").createDirectory();
                const auto f = apppaths::moveOnce (base, false);
                check (f.old && f.dir == base.getChildFile ("Clod Scope") && ! base.getChildFile ("Jam Sesh").exists(),
                       "without leave to move (no window, no app): the old folder is used, nothing moved");
            }
            // both there (an older version ran again after the move): the new one, the old one left as it is
            {
                const auto base = dataRoot.getChildFile ("both");
                base.getChildFile ("Clod Scope").getChildFile ("x.txt").create();
                base.getChildFile ("Jam Sesh").createDirectory();
                const auto f = apppaths::moveOnce (base);
                check (f.dir == base.getChildFile ("Jam Sesh") && ! f.old && base.getChildFile ("Clod Scope").getChildFile ("x.txt").existsAsFile(),
                       "both folders there: Jam Sesh is used, the old one is left alone");
            }
            // a new install: the new folder, created only when something is written there
            {
                const auto base = dataRoot.getChildFile ("fresh");
                const auto f = apppaths::moveOnce (base);
                check (f.dir == base.getChildFile ("Jam Sesh") && ! f.old && ! base.exists(), "a new install: Jam Sesh, nothing created");
            }
           #if JUCE_WINDOWS
            // a file in the old folder in use (an older version records into it): the old folder this time, the
            // move on the next start
            {
                const auto base = dataRoot.getChildFile ("busy");
                const auto take = base.getChildFile ("Clod Scope").getChildFile ("Recordings").getChildFile ("take.wav");
                take.create();
                apppaths::Folder busy;
                {
                    juce::FileOutputStream recording (take);   // held open, as an older version's recorder would
                    busy = apppaths::moveOnce (base);
                }
                const auto later = apppaths::moveOnce (base);
                line ("old folder with a file open: " + busy.dir.getFileName() + (busy.old ? " (old)" : "") + "; closed: "
                      + later.dir.getFileName());
                check (busy.old && busy.dir == base.getChildFile ("Clod Scope") && ! later.old
                           && later.dir.getChildFile ("Recordings").getChildFile ("take.wav").existsAsFile(),
                       "a file in use: the old folder is used, the move happens once it is closed");
            }
           #endif
        }

        bool run (const juce::File& outDir, const juce::File& dataRoot)
        {
            outDir.createDirectory();
            const double sr = 48000.0;
            const int bs = 256;
            line (CLOD_APP_NAME " " + juce::String (JucePlugin_VersionString) + " standalone self-test, "
                  + juce::Time::getCurrentTime().toString (true, true));
            line ("rate " + juce::String (sr, 0) + " Hz, block " + juce::String (bs));

            dataFolders (dataRoot);

            AppServices services;   // declared first: outlives the processor, as in the app
            auto proc = juce::createPluginFilterOfType (juce::AudioProcessor::wrapperType_Standalone);
            auto* cs = dynamic_cast<ClodScopeProcessor*> (proc.get());
            check (cs != nullptr, "processor created (wrapper type Standalone)");
            if (cs == nullptr)
                return false;
            {
                bool found = false;
                for (auto& e : cs->presets.entries())
                    found = found || (! e.factory && e.name == "Selftest preset");
                check (found, "the moved user preset is in the preset list");
            }

            proc->disableNonMainBuses();
            proc->setRateAndBufferSizeDetails (sr, bs);
            proc->prepareToPlay (sr, bs);
            if (auto* port = cs->getJamPort())
                port->setTrackSource (&services.player);
            check (cs->getJamPort() != nullptr, "Jam Sesh port present, track source wired");

            juce::AudioBuffer<float> io (2, bs);
            juce::MidiBuffer midi;
            Guitar guitar (sr);

            auto processSeconds = [&] (double seconds, bool withInput, std::vector<float>* outL,
                                       double& inRms, double& outPeak, int& bad, double& cpuMs)
            {
                const int blocks = (int) (seconds * sr / bs);
                double inSum = 0.0;
                outPeak = 0.0;
                bad = 0;
                cpuMs = 0.0;
                for (int b = 0; b < blocks; ++b)
                {
                    for (int i = 0; i < bs; ++i)
                    {
                        const float x = withInput ? guitar.next() : 0.0f;
                        io.setSample (0, i, x);
                        io.setSample (1, i, x);
                        inSum += (double) x * x;
                    }
                    midi.clear();
                    const double t0 = juce::Time::getMillisecondCounterHiRes();
                    proc->processBlock (io, midi);
                    cpuMs += juce::Time::getMillisecondCounterHiRes() - t0;
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < bs; ++i)
                        {
                            const float y = io.getSample (ch, i);
                            if (! std::isfinite (y))
                                ++bad;
                            else
                                outPeak = juce::jmax (outPeak, (double) std::abs (y));
                            if (ch == 0 && outL != nullptr)
                                outL->push_back (y);
                        }
                }
                inRms = std::sqrt (inSum / juce::jmax (1.0, (double) blocks * bs));
            };

            // ---- 1. the processor with a guitar-like input ----
            {
                std::vector<float> outL;
                double inRms, peak, cpuMs;
                int bad;
                processSeconds (4.0, true, &outL, inRms, peak, bad, cpuMs);
                const double outRms = rms (outL, 0, outL.size());
                line ("processor: 4.0 s guitar in rms " + juce::String (inRms, 4) + ", out rms " + juce::String (outRms, 4)
                      + ", out peak " + juce::String (peak, 4) + ", " + juce::String (cpuMs, 1) + " ms CPU ("
                      + juce::String (cpuMs / 40.0, 2) + "% of real time)");
                check (bad == 0, "no NaN/Inf in the output (" + juce::String (bad) + ")");
                check (outRms > 1.0e-4, "the guitar comes out");
            }

            // ---- 1b. the input choice (StandaloneBar's In button): the guitar on the first input only, and a
            //      second source (a 330 Hz tone) on the other ----
            {
                auto run = [&] (int routing, bool secondSource, double& outL, double& outR)
                {
                    cs->inputRouting = routing;
                    double sumL = 0.0, sumR = 0.0, phase = 0.0;
                    long count = 0;
                    for (int b = 0; b < (int) (sr / bs); ++b)
                    {
                        for (int i = 0; i < bs; ++i)
                        {
                            io.setSample (0, i, guitar.next());
                            io.setSample (1, i, secondSource ? 0.2f * (float) std::sin (phase) : 0.0f);
                            phase += juce::MathConstants<double>::twoPi * 330.0 / sr;
                        }
                        midi.clear();
                        proc->processBlock (io, midi);
                        if (b >= 10)   // once the filters have settled
                            for (int i = 0; i < bs; ++i)
                            {
                                sumL += (double) io.getSample (0, i) * io.getSample (0, i);
                                sumR += (double) io.getSample (1, i) * io.getSample (1, i);
                                ++count;
                            }
                    }
                    outL = std::sqrt (sumL / juce::jmax (1L, count));
                    outR = std::sqrt (sumR / juce::jmax (1L, count));
                };
                double stereoL, stereoR, oneL, oneR, mixL, mixR;
                run (ClodScopeProcessor::inputStereo, false, stereoL, stereoR);
                run (ClodScopeProcessor::inputFirstCentred, false, oneL, oneR);
                run (ClodScopeProcessor::inputMixCentred, true, mixL, mixR);
                cs->inputRouting = ClodScopeProcessor::inputStereo;
                line ("inputs: as they come L " + juce::String (stereoL, 4) + " R " + juce::String (stereoR, 4)
                      + "; one in the middle L " + juce::String (oneL, 4) + " R " + juce::String (oneR, 4)
                      + "; two mixed L " + juce::String (mixL, 4) + " R " + juce::String (mixR, 4));
                check (stereoR < stereoL * 0.25, "as they come: the first input stays on the left (the old behaviour)");
                check (oneL > 1.0e-4 && std::abs (oneL - oneR) < oneL * 0.05, "one input: in the middle (both sides equal)");
                check (mixL > 1.0e-4 && std::abs (mixL - mixR) < mixL * 0.05, "two inputs mixed: in the middle (both sides equal)");
            }

            // ---- 2. tempo ----
            {
                TapTempo tapper;
                double bpm = 0.0;
                for (double t : { 1000.0, 1500.0, 2000.0, 2500.0 })
                    bpm = tapper.tap (t);
                line ("tap tempo: taps 500 ms apart -> " + juce::String (bpm, 2) + " BPM");
                check (std::abs (bpm - 120.0) < 0.01, "tap tempo gives 120 BPM");
                cs->fallbackBpm = 97.0;
                double inRms, peak, cpuMs;
                int bad;
                processSeconds (0.5, true, nullptr, inRms, peak, bad, cpuMs);
                check (std::abs (cs->fallbackBpm.load() - 97.0) < 1.0e-9 && bad == 0, "fallbackBpm 97 set and processed");
                cs->fallbackBpm = 120.0;
            }

            // ---- 3. the backing-track player ----
            const auto trackFile = outDir.getChildFile ("selftest-track.wav");
            {
                const double fileRate = 44100.0;
                const int len = (int) (2.0 * fileRate);
                juce::AudioBuffer<float> tone (2, len);
                for (int i = 0; i < len; ++i)
                {
                    tone.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / fileRate));
                    tone.setSample (1, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 660.0 * i / fileRate));
                }
                trackFile.deleteFile();
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> os (trackFile.createOutputStream().release());
                std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), fileRate, 2, 16, {}, 0));
                if (w != nullptr)
                {
                    os.release();
                    w->writeFromAudioSampleBuffer (tone, 0, len);
                }
            }

            auto& player = services.player;
            juce::String err;
            const bool loaded = player.load (trackFile, err);
            check (loaded, "track loads (2.0 s, 44.1 kHz WAV, 440 Hz left / 660 Hz right) " + err);
            line ("track length " + juce::String (player.getLengthSeconds(), 3) + " s, formats " + player.wildcard());
            player.setSampleRate (sr);
            player.setVolume (1.0f);

            juce::AudioBuffer<float> tb (2, bs);
            // renders like an audio callback, waiting for the reader thread instead of real time
            auto renderTrack = [&] (int maxBlocks, std::vector<float>& L, std::vector<float>& R)
            {
                int blocks = 0;
                while (blocks < maxBlocks && player.isPlaying())
                {
                    const double until = juce::Time::getMillisecondCounterHiRes() + 300.0;
                    while (player.getBufferedSeconds() * sr < bs && juce::Time::getMillisecondCounterHiRes() < until)
                        juce::Thread::sleep (1);
                    player.renderNextBlock (tb, bs);
                    L.insert (L.end(), tb.getReadPointer (0), tb.getReadPointer (0) + bs);
                    R.insert (R.end(), tb.getReadPointer (1), tb.getReadPointer (1) + bs);
                    ++blocks;
                }
                return blocks;
            };

            if (loaded)
            {
                player.setLooping (false);
                player.play();
                std::vector<float> L, R;
                const int underruns0 = player.getUnderruns();
                renderTrack (2000, L, R);
                // the first block after play() is silent (the FIFO flush handshake), so measure sound to sound
                size_t firstSound = L.size(), lastSound = 0;
                for (size_t i = 0; i < L.size(); ++i)
                    if (std::abs (L[i]) > 1.0e-4f)
                    {
                        firstSound = juce::jmin (firstSound, i);
                        lastSound = i;
                    }
                const double seconds = (double) (lastSound + 1 - juce::jmin (firstSound, lastSound)) / sr;
                const double fL = frequency (L, firstSound + 4800, firstSound + 91200, sr), fR = frequency (R, firstSound + 4800, firstSound + 91200, sr);
                const double rL = rms (L, firstSound + 4800, firstSound + 91200);
                line ("track once: " + juce::String ((int) (lastSound + 1 - firstSound)) + " samples (starting after "
                      + juce::String ((int) firstSound) + ") = " + juce::String (seconds, 4)
                      + " s at 48 kHz, left " + juce::String (fL, 2) + " Hz, right " + juce::String (fR, 2)
                      + " Hz, left rms " + juce::String (rL, 4) + " (expect 0.3536), underruns "
                      + juce::String (player.getUnderruns() - underruns0) + ", playing after end: "
                      + (player.isPlaying() ? "yes" : "no"));
                check (std::abs (seconds - 2.0) < 0.005, "track plays its full length once, resampled 44.1 -> 48 kHz");
                check (std::abs (fL - 440.0) < 1.0 && std::abs (fR - 660.0) < 1.0, "pitch kept (440 / 660 Hz)");
                check (std::abs (rL - 0.3536) < 0.01, "level kept at volume 1.0");
                check (! player.isPlaying(), "stops at the end when not looping");

                // looping: 3 s of a 2 s file keeps playing without a gap
                player.setLooping (true);
                player.setVolume (0.5f);
                player.stop();
                player.play();
                L.clear();
                R.clear();
                const int underruns1 = player.getUnderruns();
                renderTrack ((int) (3.0 * sr / bs), L, R);
                int silentRuns = 0, run = 0;
                bool started = false;
                for (auto x : L)
                {
                    started = started || std::abs (x) >= 1.0e-4f;
                    if (! started)
                        continue;
                    run = std::abs (x) < 1.0e-4f ? run + 1 : 0;
                    if (run == 64)
                        ++silentRuns;
                }
                line ("track looped: " + juce::String ((double) L.size() / sr, 3) + " s rendered, playing "
                      + (player.isPlaying() ? "yes" : "no") + ", gaps " + juce::String (silentRuns) + ", rms "
                      + juce::String (rms (L, 0, L.size()), 4) + " (volume 0.5, expect ~0.177), underruns "
                      + juce::String (player.getUnderruns() - underruns1) + ", position "
                      + juce::String (player.getPositionSeconds(), 2) + " s");
                check (player.isPlaying() && silentRuns == 0, "loops without a gap");

                // through the processor: the Port decides whether the track is mixed locally. Silence first
                // (track stopped) so the guitar's delay/reverb tails die down, then the track
                double inRms, peak, cpuMs;
                int bad;
                std::vector<float> quietL, trackL;
                player.stop();
                processSeconds (3.0, false, &quietL, inRms, peak, bad, cpuMs);
                player.play();
                processSeconds (1.0, false, &trackL, inRms, peak, bad, cpuMs);
                const double quiet = rms (quietL, quietL.size() - 24000, quietL.size()), withTrack = rms (trackL, 0, trackL.size());
                line ("processor, no input: out rms " + juce::String (quiet, 4) + " with the track stopped, "
                      + juce::String (withTrack, 4) + " playing (no change = jam::Port doesn't mix the track yet)");
                check (bad == 0, "processing with a track source is clean");
                player.stop();
            }

            // ---- 4. the recorder ----
            {
                auto& rec = services.recorder;
                const auto recFile = outDir.getChildFile ("selftest-rec.wav");
                const bool started = rec.start (recFile, sr, err);
                check (started, "recorder starts " + err);
                cs->outputTap = &rec;
                std::vector<float> outL;
                double inRms, peak, cpuMs;
                int bad;
                processSeconds (2.0, true, &outL, inRms, peak, bad, cpuMs);
                cs->outputTap = nullptr;
                const double elapsed = rec.getElapsedSeconds();
                const int dropped = rec.getDroppedBlocks();
                rec.stop();

                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (recFile));
                check (r != nullptr, "recording reads back");
                if (r != nullptr)
                {
                    juce::AudioBuffer<float> back (2, (int) r->lengthInSamples);
                    r->read (&back, 0, (int) r->lengthInSamples, 0, true, true);
                    std::vector<float> backL (back.getReadPointer (0), back.getReadPointer (0) + back.getNumSamples());
                    double maxDiff = 0.0;
                    for (size_t i = 0; i < juce::jmin (backL.size(), outL.size()); ++i)
                        maxDiff = juce::jmax (maxDiff, (double) std::abs (backL[i] - outL[i]));
                    line ("recording: " + juce::String ((int) r->lengthInSamples) + " samples, "
                          + juce::String ((int) r->numChannels) + " ch, " + juce::String (r->sampleRate, 0) + " Hz, "
                          + juce::String ((int) r->bitsPerSample) + "-bit, elapsed shown " + juce::String (elapsed, 3)
                          + " s, dropped blocks " + juce::String (dropped) + ", rms " + juce::String (rms (backL, 0, backL.size()), 4)
                          + " vs live " + juce::String (rms (outL, 0, outL.size()), 4) + ", max sample diff "
                          + juce::String (maxDiff, 6));
                    check (r->lengthInSamples == (juce::int64) outL.size() && r->numChannels == 2 && dropped == 0,
                           "recording has every sample (2.0 s stereo)");
                    check (maxDiff < 1.0e-4, "recording matches the output (24-bit)");
                }
                line ("recordings go to " + Recorder::newRecordingFile().getFullPathName() + " (not created)");
            }

            // ---- 4b. the lowest-delay device choice (LowDelay.h), with made-up device lists: no device is opened ----
            {
                using lowdelay::Choice;
                const juce::StringArray speakers { "Speakers (Realtek(R) Audio)", "Headphones (USB Audio)" },
                                        mics { "Microphone (Realtek(R) Audio)", "Microphone (USB Audio)" };
                auto types = [&] (juce::StringArray asio, bool lowLatency)
                {
                    juce::Array<lowdelay::TypeInfo> t;
                    t.add ({ "Windows Audio", speakers, mics, 0, 0 });
                    t.add ({ "Windows Audio (Exclusive Mode)", speakers, mics, 0, 0 });
                    if (lowLatency)
                        t.add ({ "Windows Audio (Low Latency Mode)", speakers, mics, 1, 1 });
                    t.add ({ "DirectSound", speakers, mics, 0, 0 });
                    if (! asio.isEmpty())
                        t.add ({ "ASIO", asio, asio, 0, 0 });
                    return t;
                };
                const juce::StringArray wrappers { "FL Studio ASIO", "ASIO4ALL v2", "Generic Low Latency ASIO Driver", "Realtek ASIO" };
                auto withReal = wrappers;
                withReal.add ("Focusrite USB ASIO");
                withReal.add ("UMC ASIO Driver");
                const lowdelay::Current shared { "Windows Audio", "Speakers (Realtek(R) Audio)", "Microphone (Realtek(R) Audio)" };

                const auto a = lowdelay::choose (types (withReal, true), shared, 128);
                check (a.kind == Choice::Kind::asio && a.type == "ASIO" && a.output == "Focusrite USB ASIO" && a.input == a.output
                           && a.bufferTarget == 128,
                       "first run: the first real ASIO driver (wrappers skipped) at 128 -> " + a.output);
                const auto b = lowdelay::choose (types (withReal, true), { "ASIO", "UMC ASIO Driver", "UMC ASIO Driver" }, 64);
                check (b.output == "UMC ASIO Driver" && b.bufferTarget == 64, "Lowest delay keeps the real ASIO driver in use, aims for 64");
                const auto c = lowdelay::choose (types (wrappers, true), shared, 128);
                check (c.kind == Choice::Kind::lowLatencyMode && c.type == "Windows Audio (Low Latency Mode)"
                           && c.output == shared.output && c.input == shared.input && c.bufferTarget == 0,
                       "only wrappers: Low Latency Mode on the same devices, smallest buffer");
                const auto d = lowdelay::choose (types (wrappers, true), {}, 128);
                check (d.output == "Headphones (USB Audio)" && d.input == "Microphone (USB Audio)",
                       "Low Latency Mode with nothing open: Windows' default devices");
                const auto e = lowdelay::choose (types ({}, false), shared, 128);
                check (e.kind == Choice::Kind::keep && e.type.isEmpty(), "no ASIO and no Low Latency Mode: keep today's default");
                bool exclusive = false;
                for (auto& list : { withReal, wrappers, juce::StringArray() })
                    for (bool ll : { true, false })
                        for (auto& cand : lowdelay::candidates (types (list, ll), shared, 128))
                            exclusive = exclusive || cand.type.contains ("Exclusive");
                check (! exclusive, "never exclusive mode");
                const auto order = lowdelay::candidates (types (withReal, true), shared, 128);
                check (order.size() == 2 && order[0].kind == Choice::Kind::asio && order[1].kind == Choice::Kind::lowLatencyMode,
                       "fallback order: ASIO, then Low Latency Mode (if the driver is busy)");

                // macOS: only CoreAudio, at 128 on the first run and 64 for Lowest delay, on the devices in use
                const juce::Array<lowdelay::TypeInfo> mac { { "CoreAudio", { "MacBook Pro Speakers", "USB Audio CODEC" },
                                                              { "MacBook Pro Microphone", "USB Audio CODEC" }, 0, 0 } };
                const auto m1 = lowdelay::choose (mac, { "CoreAudio", "USB Audio CODEC", "USB Audio CODEC" }, 128);
                const auto m2 = lowdelay::choose (mac, {}, 64);
                check (m1.kind == Choice::Kind::coreAudio && m1.output == "USB Audio CODEC" && m1.input == "USB Audio CODEC"
                           && m1.bufferTarget == 128 && m2.kind == Choice::Kind::coreAudio && m2.output == "MacBook Pro Speakers"
                           && m2.input == "MacBook Pro Microphone" && m2.bufferTarget == 64 && lowdelay::warningFor ("CoreAudio").isEmpty(),
                       "macOS: CoreAudio on the devices in use (else the defaults) at 128 / 64 samples, no amber warning");

                check (lowdelay::pickBuffer ({ 32, 64, 128, 256 }, 128) == 128 && lowdelay::pickBuffer ({ 32, 64, 128, 256 }, 64) == 64
                           && lowdelay::pickBuffer ({ 96, 192, 288 }, 128) == 192 && lowdelay::pickBuffer ({ 16, 32 }, 64) == 32
                           && lowdelay::pickBuffer ({ 480, 144, 192, 240 }, 0) == 144 && lowdelay::pickBuffer ({}, 128) == 0,
                       "buffer: the target, else the next larger, else the largest; 0 = the smallest");

                check (lowdelay::warningFor ("Windows Audio").contains ("30 ms") && lowdelay::warningFor ("DirectSound").isNotEmpty()
                           && lowdelay::warningFor ("Windows Audio (Low Latency Mode)").isEmpty()
                           && lowdelay::warningFor ("Windows Audio (Exclusive Mode)").isEmpty() && lowdelay::warningFor ("ASIO").isEmpty(),
                       "amber warning only for Windows Audio shared mode and DirectSound");

                const auto ro = lowdelay::readout (139, 307, 48000.0, 2, 60.0, 0.123);
                line ("readout: " + ro);
                check (ro.contains ("in 2.9 + out 6.4 = 9.3 ms") && ro.contains ("dropouts: 2 in the last 60 s") && ro.contains ("load 12 %"),
                       "settings readout: driver in + out = round trip, dropouts, load");
                lowdelay::DropoutWindow w;
                w.add (0.0, 5);
                w.add (30000.0, 7);
                w.add (70000.0, 9);   // the first sample falls out of the minute
                const int inMinute = w.dropouts();
                w.add (71000.0, 0);   // device restarted
                check (inMinute == 2 && w.dropouts() == 0, "dropouts: counted over the last minute, restart clears");

                // the processor's device latency: 0 with no device (a manager that never opened one)
                cs->deviceInputLatency = 123;
                cs->deviceOutputLatency = 456;
                {
                    juce::AudioDeviceManager unopened;
                    lowdelay::LatencyPublisher pub (unopened, cs->deviceInputLatency, cs->deviceOutputLatency);
                }
                check (cs->deviceInputLatency.load() == 0 && cs->deviceOutputLatency.load() == 0, "device latency published as 0 with no device");

                const int throttle = turnOffPowerThrottling();
                line (juce::String ("power throttling off (this process): execution speed ") + ((throttle & 1) ? "yes" : "no")
                      + ", timer resolution " + ((throttle & 2) ? "yes" : "no (needs Windows 11 22H2)"));
            }

            // ---- 5. the editor with its StandaloneBar, painted off-screen (never added to the desktop) ----
            {
                AppServices::instance() = &services;
                // as if the 2D scope and the input page were open last time: the app opens on Jam Sesh anyway
                if (auto* mv = cs->apvts.getParameter ("mainView"))
                    mv->setValueNotifyingHost (mv->convertTo0to1 (0.0f));
                cs->apvts.state.setProperty ("uiModule", chain::info (chain::input).key, nullptr);
                std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditorIfNeeded());
                if (ed != nullptr)
                {
                    ed->setSize (1500, 940);
                    ed->setVisible (true);

                    // Jam Sesh's view and page, and its button the leftmost of the view switch
                    std::function<juce::TextButton* (juce::Component&)> findJamButton = [&] (juce::Component& c) -> juce::TextButton*
                    {
                        for (auto* child : c.getChildren())
                        {
                            if (auto* b = dynamic_cast<juce::TextButton*> (child); b != nullptr && b->getButtonText() == "Jam Sesh")
                                return b;
                            if (auto* b = findJamButton (*child))
                                return b;
                        }
                        return nullptr;
                    };
                    const int view = juce::roundToInt (cs->apvts.getRawParameterValue ("mainView")->load());
                    const auto page = cs->apvts.state.getProperty ("uiModule").toString();
                    bool jamShown = false;
                    for (auto* c : ed->getChildren())
                        if (auto* jv = dynamic_cast<JamView*> (c))
                            jamShown = jv->isVisible() && jv->getWidth() > 0;
                    auto* jamButton = findJamButton (*ed);
                    bool jamFirst = jamButton != nullptr && jamButton->getToggleState();
                    if (jamButton != nullptr)
                        for (auto* sibling : jamButton->getParentComponent()->getChildren())
                            jamFirst = jamFirst && (sibling == jamButton || sibling->getX() > jamButton->getX());
                    line ("start screen: view " + juce::String (view) + ", page " + page + ", Jam Sesh view "
                          + (jamShown ? "shown" : "hidden") + ", Jam Sesh button " + (jamFirst ? "first" : "not first"));
                    check (view == 4 && page == chain::info (chain::viewJam).key && jamShown,
                           "the app opens on Jam Sesh (view and page), whatever was open last");
                    check (jamFirst, "Jam Sesh is the first button of the view switch (and on)");
                    {
                        juce::Image whole (juce::Image::RGB, ed->getWidth(), ed->getHeight(), true, juce::SoftwareImageType());
                        {
                            juce::Graphics g (whole);
                            ed->paintEntireComponent (g, true);
                        }
                        const auto png = outDir.getChildFile ("selftest-editor.png");
                        png.deleteFile();
                        juce::FileOutputStream os (png);
                        juce::PNGImageFormat().writeImageToStream (whole, os);
                        line ("the app's start screen -> " + png.getFileName());
                    }
                    StandaloneBar* bar = nullptr;
                    for (auto* c : ed->getChildren())
                        if (auto* b = dynamic_cast<StandaloneBar*> (c))
                            bar = b;
                    check (bar != nullptr && bar->isVisible() && bar->getHeight() == StandaloneBar::idealHeight,
                           "the editor shows the StandaloneBar");
                    if (bar != nullptr)
                    {
                        juce::Image img (juce::Image::RGB, bar->getWidth(), bar->getHeight(), true, juce::SoftwareImageType());
                        const double t0 = juce::Time::getMillisecondCounterHiRes();
                        {
                            juce::Graphics g (img);
                            bar->paintEntireComponent (g, true);
                        }
                        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
                        const auto png = outDir.getChildFile ("selftest-bar.png");
                        png.deleteFile();
                        juce::FileOutputStream os (png);
                        juce::PNGImageFormat().writeImageToStream (img, os);
                        line ("bar painted in software: " + juce::String (bar->getWidth()) + "x" + juce::String (bar->getHeight())
                              + ", " + juce::String (ms, 2) + " ms -> " + png.getFileName());

                        // the amber Audio button: the bar acts as if Windows Audio (shared mode) were open
                        juce::TextButton* audio = nullptr;
                        for (auto* c : bar->getChildren())
                            if (auto* b = dynamic_cast<juce::TextButton*> (c); b != nullptr && b->getButtonText() == "Audio")
                                audio = b;
                        const bool plain = audio != nullptr && ! audio->getToggleState();
                        services.testDeviceType = "Windows Audio";
                        bar->refreshNow();   // what the bar's 10 Hz timer does
                        const bool amber = audio != nullptr && audio->getToggleState() && audio->getTooltip().contains ("30 ms");
                        juce::Image img2 (juce::Image::RGB, bar->getWidth(), bar->getHeight(), true, juce::SoftwareImageType());
                        {
                            juce::Graphics g (img2);
                            bar->paintEntireComponent (g, true);
                        }
                        const auto png2 = outDir.getChildFile ("selftest-bar-amber.png");
                        png2.deleteFile();
                        {
                            juce::FileOutputStream os2 (png2);
                            juce::PNGImageFormat().writeImageToStream (img2, os2);
                        }
                        line ("Audio button tooltip in shared mode: " + (audio != nullptr ? audio->getTooltip() : juce::String ("-")));
                        services.testDeviceType = "Windows Audio (Low Latency Mode)";
                        bar->refreshNow();
                        const bool cleared = audio != nullptr && ! audio->getToggleState();
                        services.testDeviceType.clear();
                        check (plain && amber && cleared, "Audio button amber on Windows Audio shared mode (tooltip says why), plain on Low "
                                                          "Latency Mode -> " + png2.getFileName());
                    }
                    ed.reset();
                }
                else
                {
                    check (false, "editor created");
                }
                AppServices::instance() = nullptr;
            }

            proc->releaseResources();
            line (ok ? "RESULT: PASS" : "RESULT: FAIL");
            return ok;
        }
    };

    //==============================================================================
    class ClodStandaloneApp final : public juce::JUCEApplication
    {
    public:
        const juce::String getApplicationName() override           { return juce::CharPointer_UTF8 (CLOD_APP_NAME); }
        const juce::String getApplicationVersion() override        { return JucePlugin_VersionString; }
        bool moreThanOneInstanceAllowed() override                 { return true; }
        void anotherInstanceStarted (const juce::String&) override {}

        void initialise (const juce::String& commandLine) override
        {
            // headless self-test: decided before anything opens a window or an audio device
            const auto args = juce::StringArray::fromTokens (commandLine, true);
            const int st = args.indexOf ("--selftest");
            if (st >= 0)
            {
                const auto dirArg = args[st + 1].unquoted();
                const auto outDir = dirArg.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile (dirArg)
                                                        : juce::File::getCurrentWorkingDirectory();
                const auto dataRoot = outDir.getChildFile ("selftest-data");
                apppaths::useTestRoot (dataRoot);   // before anything asks for a folder: the user's are never touched
                SelfTest test;
                const bool passed = test.run (outDir, dataRoot);
                outDir.getChildFile ("selftest.txt").replaceWithText (test.log);
                setApplicationReturnValue (passed ? 0 : 1);
                quit();
                return;
            }

            turnOffPowerThrottling();

            services = std::make_unique<AppServices>();
            AppServices::instance() = services.get();

            // the app's settings (audio setup, window, the last state): %APPDATA%\Jam Sesh\App.settings, moved
            // there from %APPDATA%\Clod Scope\Clod Scope.settings once (AppPaths.h: the app moves the old folders)
            apppaths::allowMoves();
            appSettings = std::make_unique<juce::PropertiesFile> (apppaths::appSettingsFile(), juce::PropertiesFile::Options());
            auto* settings = appSettings.get();
            const bool firstRun = settings->getXmlValue ("audioSetup") == nullptr;   // decided before the holder runs
            const juce::Array<juce::StandalonePluginHolder::PluginInOuts> channels;
            holder = std::make_unique<juce::StandalonePluginHolder> (settings, false, juce::String(), nullptr, channels, false);
            // the counts the holder opened the device with (its getNumInput/OutputChannels: the main buses)
            const int inputsNeeded = holder->processor->getMainBusNumInputChannels();
            const int outputsNeeded = holder->processor->getMainBusNumOutputChannels();

            // a guitar app: input on unless the user turned it off in an earlier run
            // (JUCE mutes it by default against feedback; the bar's Input button says "use headphones")
            if (! settings->getBoolValue ("clodInputChoice", false))
                holder->getMuteInputValue() = false;

            // first run (no saved device setup): the lowest-delay device instead of JUCE's first type (Windows Audio
            // shared, ~30 ms more): a real ASIO driver at 128, else Low Latency Mode at its smallest buffer
            if (firstRun)
            {
                const auto r = lowdelay::apply (holder->deviceManager, inputsNeeded, outputsNeeded, 128, false);
                if (r.changed)
                {
                    services->startupNote = "Audio: " + lowdelay::describe (holder->deviceManager) + " (picked for the lowest delay)";
                    holder->saveAudioDeviceState();
                }
            }

            services->deviceManager = &holder->deviceManager;
            services->inputsNeeded = inputsNeeded;
            services->outputsNeeded = outputsNeeded;
            services->inputMuted.referTo (holder->getMuteInputValue());
            services->settings = settings;
            services->saveAudioState = [h = holder.get()] { h->saveAudioDeviceState(); };

            if (auto* proc = dynamic_cast<ClodScopeProcessor*> (holder->processor.get()))
            {
                proc->outputTap = &services->recorder;
                if (auto* port = proc->getJamPort())
                    port->setTrackSource (&services->player);
                services->player.setSampleRate (proc->getSampleRate());
                // the device's own latency for the Jam panel and the per-player totals (0 = no device)
                latencyPublisher = std::make_unique<lowdelay::LatencyPublisher> (holder->deviceManager, proc->deviceInputLatency,
                                                                                 proc->deviceOutputLatency);
            }

            mainWindow = std::make_unique<MainWindow> (*holder, settings);
            mainWindow->setVisible (true);
        }

        void shutdown() override
        {
            if (services != nullptr)
                services->recorder.stop();
            mainWindow = nullptr;     // the editor
            latencyPublisher = nullptr;
            holder = nullptr;         // stops the audio device, saves its setup, deletes the processor
            AppServices::instance() = nullptr;
            services = nullptr;       // after the processor: it held the track source and the output tap
            if (appSettings != nullptr)
                appSettings->saveIfNeeded();
        }

        void systemRequestedQuit() override
        {
            if (holder != nullptr)
                holder->savePluginState();
            if (mainWindow != nullptr)
                mainWindow->saveState();

            if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
                juce::Timer::callAfterDelay (100, [] { if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit(); });
            else
                quit();
        }

    private:
        std::unique_ptr<juce::PropertiesFile> appSettings;   // first: the holder refers to it
        std::unique_ptr<AppServices> services;
        std::unique_ptr<juce::StandalonePluginHolder> holder;
        std::unique_ptr<lowdelay::LatencyPublisher> latencyPublisher;
        std::unique_ptr<MainWindow> mainWindow;
    };
}

JUCE_CREATE_APPLICATION_DEFINE (ClodStandaloneApp)

#endif
