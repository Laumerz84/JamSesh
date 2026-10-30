// The standalone app (Jam Sesh.exe, CLOD_APP_NAME from CMakeLists.txt): a window holding the same editor as
// the plugin, playing through the computer's audio interface (ASIO or Windows audio) and MIDI inputs.
// JUCE's StandalonePluginHolder runs the audio device; the window is our own (no "Options" title-bar
// button: the editor's StandaloneBar has the audio settings, input, tempo, backing track and recorder).
//   Jam Sesh.exe --selftest <outDir>   headless check: no window, no audio device; writes selftest.txt
// Owner: standalone soldier.
#include <juce_core/system/juce_TargetPlatform.h>

#if JucePlugin_Build_Standalone && JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "AppServices.h"
#include "StandaloneBar.h"
#include "../PluginProcessor.h"
#include "../UI/Theme.h"
#include "../UI/StreamSafe.h"

namespace
{
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

        bool run (const juce::File& outDir)
        {
            outDir.createDirectory();
            const double sr = 48000.0;
            const int bs = 256;
            line (CLOD_APP_NAME " " + juce::String (JucePlugin_VersionString) + " standalone self-test, "
                  + juce::Time::getCurrentTime().toString (true, true));
            line ("rate " + juce::String (sr, 0) + " Hz, block " + juce::String (bs));

            AppServices services;   // declared first: outlives the processor, as in the app
            auto proc = juce::createPluginFilterOfType (juce::AudioProcessor::wrapperType_Standalone);
            auto* cs = dynamic_cast<ClodScopeProcessor*> (proc.get());
            check (cs != nullptr, "processor created (wrapper type Standalone)");
            if (cs == nullptr)
                return false;

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

            // ---- 5. the editor with its StandaloneBar, painted off-screen (never added to the desktop) ----
            {
                AppServices::instance() = &services;
                std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditorIfNeeded());
                if (ed != nullptr)
                {
                    ed->setSize (1500, 940);
                    ed->setVisible (true);
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
        ClodStandaloneApp()
        {
            // the settings keep the plugin's name (%APPDATA%\Clod Scope\Clod Scope.settings), from before the
            // app was called Jam Sesh
            juce::PropertiesFile::Options options;
            options.applicationName = juce::CharPointer_UTF8 (JucePlugin_Name);
            options.filenameSuffix = ".settings";
            options.osxLibrarySubFolder = "Application Support";
            options.folderName = "";
            appProperties.setStorageParameters (options);
        }

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
                SelfTest test;
                const bool passed = test.run (outDir);
                outDir.getChildFile ("selftest.txt").replaceWithText (test.log);
                setApplicationReturnValue (passed ? 0 : 1);
                quit();
                return;
            }

            services = std::make_unique<AppServices>();
            AppServices::instance() = services.get();

            auto* settings = appProperties.getUserSettings();
            const juce::Array<juce::StandalonePluginHolder::PluginInOuts> channels;
            holder = std::make_unique<juce::StandalonePluginHolder> (settings, false, juce::String(), nullptr, channels, false);

            // a guitar app: input on unless the user turned it off in an earlier run
            // (JUCE mutes it by default against feedback; the bar's Input button says "use headphones")
            if (! settings->getBoolValue ("clodInputChoice", false))
                holder->getMuteInputValue() = false;

            services->deviceManager = &holder->deviceManager;
            services->inputMuted.referTo (holder->getMuteInputValue());
            services->settings = settings;
            services->saveAudioState = [h = holder.get()] { h->saveAudioDeviceState(); };

            if (auto* proc = dynamic_cast<ClodScopeProcessor*> (holder->processor.get()))
            {
                proc->outputTap = &services->recorder;
                if (auto* port = proc->getJamPort())
                    port->setTrackSource (&services->player);
                services->player.setSampleRate (proc->getSampleRate());
            }

            mainWindow = std::make_unique<MainWindow> (*holder, settings);
            mainWindow->setVisible (true);
        }

        void shutdown() override
        {
            if (services != nullptr)
                services->recorder.stop();
            mainWindow = nullptr;     // the editor
            holder = nullptr;         // stops the audio device, saves its setup, deletes the processor
            AppServices::instance() = nullptr;
            services = nullptr;       // after the processor: it held the track source and the output tap
            appProperties.saveIfNeeded();
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
        juce::ApplicationProperties appProperties;
        std::unique_ptr<AppServices> services;
        std::unique_ptr<juce::StandalonePluginHolder> holder;
        std::unique_ptr<MainWindow> mainWindow;
    };
}

JUCE_CREATE_APPLICATION_DEFINE (ClodStandaloneApp)

#endif
