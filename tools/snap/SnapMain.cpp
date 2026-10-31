// Headless snapshots of the Clod Scope editor. No window is ever shown and nothing takes focus:
// the processor and the editor run in-process, a strummed guitar is fed block by block while the
// message loop runs the editor's timers, then components are painted into images (PNG).
// Also checks capture-safe drawing (UI/StreamSafe.h): the 3D views render off-screen, the editor's
// window (created hidden) is on the software renderer, and what painting in software costs.
//   ClodScopeSnap <outDir> [base] [themes] [jam] [jamfriend] [bar] [all]      (no mode = base)
//     base       the editor in chord / mono / min size / XY / 3D / orbital, paint costs, window renderers
//     themes     editor_theme<NN>_<name>.png for every theme (Themes::apply on the live editor) + paint ms
//     jam        the Jam Sesh view and panel before any session
//     jamfriend  this process hosts a Jam Sesh on 127.0.0.1; a hidden ClodJamTest child joins playing a guitar
//                with its 3D view on Phase Space, so the friend's tile shows their view; tile costs
//                (--jamtest <path to ClodJamTest.exe>; default: ClodJamTest in this build folder)
//     bar        the standalone app's StandaloneBar, constructed directly
// The theme is forced (CLOD_THEME, default 0) so snapshots are reproducible and never save the owner's choice.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <iostream>
#include <cstdlib>
#include <functional>
#include "../../Source/PluginProcessor.h"
#include "../../Source/PluginEditor.h"
#include "../../Source/UI/StreamSafe.h"
#include "../../Source/UI/Themes.h"
#include "../../Source/Jam/JamSession.h"
#include "../../Source/Jam/JamView.h"
#include "../../Source/Jam/JamPanel.h"
#include "../../Source/Jam/ViewTile.h"
#include "../../Source/Jam/ViewState.h"
#include "../../Source/App/StandaloneBar.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
double threadCpuMs();   // CpuTime.cpp

namespace
{
    void setEnv (const char* name, const char* value)
    {
       #ifdef _WIN32
        _putenv_s (name, value);
       #else
        setenv (name, value, 1);
       #endif
    }

    template <typename T>
    T* findChild (juce::Component& root)
    {
        if (auto* t = dynamic_cast<T*> (&root))
            return t;
        for (auto* c : root.getChildren())
            if (auto* t = findChild<T> (*c))
                return t;
        return nullptr;
    }

    void save (juce::Component& c, const juce::File& file)
    {
        auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (img, out);
        std::cout << "  wrote " << file.getFileName() << " (" << img.getWidth() << "x" << img.getHeight() << ")\n";
    }

    // One paint of the component with JUCE's software renderer, which is what its window now uses (ms)
    double paintSoftwareMs (juce::Component& c)
    {
        juce::Image img (juce::Image::RGB, juce::jmax (1, c.getWidth()), juce::jmax (1, c.getHeight()), true, juce::SoftwareImageType());
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        {
            juce::Graphics g (img);
            c.paintEntireComponent (g, true);
        }
        return juce::Time::getMillisecondCounterHiRes() - t0;
    }

    juce::String rendererOf (juce::Component& c)
    {
        if (auto* peer = c.getPeer())
            return peer->getAvailableRenderingEngines()[peer->getCurrentRenderingEngine()];
        return "no window";
    }

    // Karplus-Strong strings plucked at 12-30 % of their length
    struct Guitar
    {
        struct String { std::vector<float> line; size_t pos = 0; float prev = 0.0f; long start = 0; };
        std::vector<String> strings;
        juce::Random rng { 5 };
        long t = 0;

        void strum (std::initializer_list<int> notes, double spreadMs, double sr)
        {
            strings.clear();
            long k = 0;
            for (int note : notes)
            {
                const double hz = 440.0 * std::pow (2.0, (note - 69) / 12.0);
                String s;
                s.line.resize ((size_t) juce::jmax (2, (int) std::round (sr / hz)));
                const float pos = 0.12f + 0.18f * rng.nextFloat();
                for (size_t i = 0; i < s.line.size(); ++i)
                {
                    const float x = (float) i / (float) s.line.size();
                    const float tri = x < pos ? x / pos : (1.0f - x) / (1.0f - pos);
                    s.line[i] = 1.6f * (tri - 0.5f) + 0.1f * (rng.nextFloat() * 2.0f - 1.0f);
                }
                s.start = t + (long) (k++ * spreadMs * 0.001 * sr);
                strings.push_back (std::move (s));
            }
        }

        float next()
        {
            float out = 0.0f;
            for (auto& s : strings)
                if (t >= s.start)
                {
                    auto& v = s.line[s.pos];
                    const float o = v;
                    v = 0.997f * 0.5f * (v + s.prev);
                    s.prev = o;
                    s.pos = (s.pos + 1) % s.line.size();
                    out += 0.22f * o;
                }
            ++t;
            return out;
        }
    };
}

int main (int argc, char** argv)
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));
    if (std::getenv ("CLOD_THEME") == nullptr)
        setEnv ("CLOD_THEME", "0");
    setEnv ("CLOD_JAM_BIND", "127.0.0.1");   // never all interfaces (Windows Firewall would ask)

    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outDir (args.size() > 0 ? juce::File::getCurrentWorkingDirectory().getChildFile (args[0])
                                             : juce::File::getCurrentWorkingDirectory());
    outDir.createDirectory();
    auto want = [&] (const char* mode) { return args.contains (mode) || args.contains ("all"); };
    const bool anyMode = want ("base") || want ("themes") || want ("jam") || want ("jamfriend") || want ("bar");
    const bool wantBase = want ("base") || ! anyMode;
    juce::File jamTestExe = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                .getParentDirectory().getParentDirectory().getParentDirectory()
                                .getChildFile ("ClodJamTest_artefacts").getChildFile ("Release")
                               #ifdef _WIN32
                                .getChildFile ("ClodJamTest.exe");
                               #else
                                .getChildFile ("ClodJamTest");
                               #endif
    if (args.indexOf ("--jamtest") >= 0)
        jamTestExe = juce::File (args[args.indexOf ("--jamtest") + 1]);

    const double sr = 48000.0;
    const int block = 480;
    std::unique_ptr<juce::AudioProcessor> base (createPluginFilter());
    auto& proc = dynamic_cast<ClodScopeProcessor&> (*base);
    proc.setPlayConfigDetails (2, 2, sr, block);
    proc.prepareToPlay (sr, block);

    auto set = [&] (const char* id, float realValue)
    {
        if (auto* p = proc.apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (realValue));
        else
            std::cout << "  !! missing parameter " << id << "\n";
    };

    Guitar guitar;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    auto pump = [&] (int ms)
    {
        for (int done = 0; done < ms; done += 10)
        {
            for (int i = 0; i < block; ++i)
            {
                const float v = guitar.next();
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }
            proc.processBlock (buf, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        }
    };

    proc.apvts.state.setProperty ("uiModule", "synth", nullptr);
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditorIfNeeded());
    if (ed == nullptr)
    {
        std::cout << "no editor\n";
        return 1;
    }
    ed->setVisible (true);   // visible as a component; it is never added to the desktop
    pump (300);

    // ---- every theme on the live editor ----
    if (want ("themes"))
    {
        const int original = Themes::current();
        set ("mainView", 0.0f);
        guitar.strum ({ 40, 47, 52, 56, 59, 64 }, 12.0, sr);
        // paint costs are noisy while other work runs: 20 whole-editor paints per theme, back to back, min and median
        auto paintStats = [&]
        {
            std::vector<double> ms;
            paintSoftwareMs (*ed);   // warm-up after the switch
            for (int i = 0; i < 20; ++i)
                ms.push_back (paintSoftwareMs (*ed));
            std::sort (ms.begin(), ms.end());
            return "editor paint min " + juce::String (ms.front(), 2) + " ms, median " + juce::String (ms[ms.size() / 2], 2) + " ms";
        };
        for (int t = 0; t <= Themes::count(); ++t)
        {
            const int theme = t % Themes::count();   // theme 0 again at the end (drift of the machine's load)
            Themes::apply (theme);
            pump (250);
            const auto name = Themes::name (theme).retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
            std::cout << "  theme " << theme << " " << Themes::name (theme) << ": " << paintStats() << ";";
            if (t < Themes::count())
                save (*ed, outDir.getChildFile ("editor_theme" + juce::String (theme).paddedLeft ('0', 2) + "_" + name + ".png"));
            else
                std::cout << " (again)\n";
        }
        Themes::apply (original);
        pump (100);
    }

    auto savePanelOf = [&] (juce::Component& panel, const juce::File& file)
    {
        // the panel as the right column shows it (inside its PanelViewport)
        juce::Component* c = &panel;
        while (c->getParentComponent() != nullptr && dynamic_cast<PanelViewport*> (c) == nullptr)
            c = c->getParentComponent();
        save (dynamic_cast<PanelViewport*> (c) != nullptr ? *c : panel, file);
    };

    auto jamTiles = [&]
    {
        std::vector<ViewTile*> tiles;
        std::function<void (juce::Component&)> collect = [&] (juce::Component& c)
        {
            if (auto* t = dynamic_cast<ViewTile*> (&c)) tiles.push_back (t);
            for (auto* ch : c.getChildren()) collect (*ch);
        };
        if (auto* jv = findChild<JamView> (*ed))
            collect (*jv);
        return tiles;
    };

    // tiles only animate while on screen (never, here): advance them all together before a snapshot
    auto tickTiles = [&] (int frames)
    {
        for (int f = 0; f < frames; ++f)
        {
            pump (33);
            for (auto* t : jamTiles())
                t->tick();
        }
    };

    auto tileCosts = [&] (const char* what)
    {
        auto* jv = findChild<JamView> (*ed);
        if (jv == nullptr)
            return;
        const auto tiles = jamTiles();
        std::cout << "  Jam view " << what << " (" << jv->getWidth() << "x" << jv->getHeight() << "): paint "
                  << juce::String (paintSoftwareMs (*jv), 2) << " ms, " << tiles.size() << " tiles\n";
        for (size_t i = 0; i < tiles.size(); ++i)
        {
            double frame = 0.0, paint = 0.0;
            const int frames = 30;
            for (int f = 0; f < frames; ++f)
            {
                pump (10);
                tiles[i]->tick();
                frame += tiles[i]->lastFrameMs();
                paint += paintSoftwareMs (*tiles[i]);
            }
            std::cout << "    tile " << i << " (" << tiles[i]->getWidth() << "x" << tiles[i]->getHeight() << "): frame "
                      << juce::String (frame / frames, 2) << " ms + paint " << juce::String (paint / frames, 2) << " ms\n";
        }
    };

    // ---- Jam Sesh, no session ----
    if (want ("jam"))
    {
        set ("mainView", 4.0f);
        guitar.strum ({ 45, 52, 57 }, 10.0, sr);
        pump (200);
        tickTiles (15);
        if (auto* jv = findChild<JamView> (*ed))
            save (*jv, outDir.getChildFile ("jam_view_idle.png"));
        else
            std::cout << "  !! no JamView in the editor\n";
        if (auto* jp = findChild<JamPanel> (*ed))
            savePanelOf (*jp, outDir.getChildFile ("jam_panel_idle.png"));
        else
            std::cout << "  !! no JamPanel in the editor\n";
        save (*ed, outDir.getChildFile ("editor_jam_idle.png"));
        tileCosts ("idle");
        set ("mainView", 0.0f);
        pump (50);
    }

    // ---- Jam Sesh with a real friend (a hidden ClodJamTest child over 127.0.0.1) ----
    if (want ("jamfriend"))
    {
        juce::SharedResourcePointer<jam::Session> session;
        juce::String error;
        // others may run Jam tests at the same time: a random port, the next one if it is taken
        int port = 40000 + 2 * juce::Random::getSystemRandom().nextInt (10000);
        auto hostIt = [&]
        {
            for (int attempt = 0; attempt < 4; ++attempt, port += 2)
                if (session->host ("Snap", port, error) || ! error.containsIgnoreCase ("in use"))
                    break;
            return session->status() != jam::Status::idle && session->status() != jam::Status::failed;
        };
        if (! jamTestExe.existsAsFile())
        {
            std::cout << "  jam friend: no ClodJamTest at " << jamTestExe.getFullPathName() << " (build it, or pass --jamtest)\n";
        }
        else if (! hostIt())
        {
            std::cout << "  jam friend: FAIL (host: " << error << ")\n";
        }
        else
        {
            const auto code = session->sessionCode().isNotEmpty() ? session->sessionCode() : "127.0.0.1:" + juce::String (port);
            const auto friendOut = outDir.getChildFile ("jam_friend_player.txt");
            juce::ChildProcess child;   // CREATE_NO_WINDOW, no pipes
            child.start (juce::StringArray { jamTestExe.getFullPathName(), "--player", "join", "--code", code, "--name", "Friend",
                                             "--expect", "2", "--seconds", "60", "--signal", "guitar", "--view", "1,1",
                                             "--out", friendOut.getFullPathName() }, 0);
            bool in = false;
            for (int i = 0; i < 200 && ! in; ++i)   // up to ~20 s
            {
                if (i % 20 == 0)
                    guitar.strum ({ 40, 47, 52, 56, 59, 64 }, 12.0, sr);
                pump (100);
                for (auto& p : session->players())
                    if (p.connected && ! p.isSelf)
                        in = true;
            }
            std::cout << "  jam friend: " << (in ? "joined" : "DID NOT JOIN within 20 s") << " - " << session->statusText() << "\n";
            if (in)
            {
                set ("mainView", 4.0f);
                guitar.strum ({ 40, 47, 52, 56, 59, 64 }, 12.0, sr);
                pump (1500);   // the friend's view state and sound arrive
                tickTiles (30);
                for (auto& p : session->players())
                {
                    ViewState vs;
                    const bool has = session->viewStateFor (p.id, vs);
                    std::cout << "    player " << p.name << (p.isSelf ? " (self)" : "") << ": view state "
                              << (has ? "mainView " + juce::String (vs.mainView) : juce::String ("none"))
                              << ", feed " << (session->feedFor (p.id) != nullptr ? "yes" : "none")
                              << ", ping " << juce::String (p.pingMs, 2) << " ms, total " << juce::String (p.totalMs, 2)
                              << " ms, level " << juce::String (p.levelDb, 1) << " dB\n";
                }
                if (auto* jv = findChild<JamView> (*ed))
                    save (*jv, outDir.getChildFile ("jam_view_friend.png"));
                if (auto* jp = findChild<JamPanel> (*ed))
                    savePanelOf (*jp, outDir.getChildFile ("jam_panel_friend.png"));
                save (*ed, outDir.getChildFile ("editor_jam_friend.png"));
                tileCosts ("with a friend");

                // your tile with a view chosen for it (3D) while you're on the Jam Sesh page
                proc.apvts.state.setProperty ("jamTileView", 2, nullptr);
                pump (1200);
                tickTiles (30);
                for (auto& p : session->players())
                    if (p.isSelf)
                    {
                        ViewState vs;
                        session->viewStateFor (p.id, vs);
                        std::cout << "    your tile set to 3D on the Jam Sesh page: view state mainView " << vs.mainView << "\n";
                    }
                if (auto* jv = findChild<JamView> (*ed))
                    save (*jv, outDir.getChildFile ("jam_view_tile3d.png"));
                if (auto* jp = findChild<JamPanel> (*ed))
                    save (*jp, outDir.getChildFile ("jam_panel_tile3d.png"));   // the whole panel, not only what scrolls into view
                proc.apvts.state.setProperty ("jamTileView", 0, nullptr);
                set ("mainView", 0.0f);
                pump (50);
            }
            session->leave();
            pump (200);
            child.kill();
        }
    }

    // ---- the standalone app's bar, constructed directly ----
    if (want ("bar"))
    {
        StandaloneBar bar (proc);
        bar.setSize (1500, StandaloneBar::idealHeight);
        bar.setVisible (true);
        pump (200);
        paintSoftwareMs (bar);
        std::cout << "  standalone bar paint: " << juce::String (paintSoftwareMs (bar), 2) << " ms;";
        save (bar, outDir.getChildFile ("standalone_bar.png"));

        // the In button's input picker, with made-up input names (inputs 1 and 2 mixed)
        auto picker = StandaloneBar::createInputPicker ({ "Mic 1", "Inst 2", "Line 3", "Line 4" }, 0, 1, true, {});
        picker->setVisible (true);
        save (*picker, outDir.getChildFile ("input_picker.png"));
    }

    if (wantBase)
    {

    // Chord mode with per-note drive: an open E major strum
    set ("synOn", 1.0f);
    set ("synTrackMode", 1.0f);
    set ("synLayer", 2.0f);
    set ("synDriveType", 1.0f);
    set ("synDrive", 45.0f);
    set ("synPickAttack", 40.0f);
    guitar.strum ({ 40, 47, 52, 56, 59, 64 }, 12.0, sr);
    pump (1400);
    auto* panel = findChild<SynthPanel> (*ed);
    save (*ed, outDir.getChildFile ("editor_chord.png"));
    if (panel != nullptr)
        save (*panel, outDir.getChildFile ("synth_panel_chord.png"));

    // Mono mode, one note
    set ("synTrackMode", 0.0f);
    set ("synDrive", 0.0f);
    guitar.strum ({ 45 }, 0.0, sr);
    pump (900);
    save (*ed, outDir.getChildFile ("editor_mono.png"));
    if (panel != nullptr)
        save (*panel, outDir.getChildFile ("synth_panel_mono.png"));

    // the editor at its minimum size, chord mode (layout check)
    set ("synTrackMode", 1.0f);
    guitar.strum ({ 48, 52, 55, 60, 64 }, 10.0, sr);
    ed->setSize (1200, 800);
    pump (1200);
    save (*ed, outDir.getChildFile ("editor_min_chord.png"));

    // ---- capture-safe drawing ----
    ed->setSize (1500, 940);
    set ("synOn", 0.0f);
    set ("mainView", 0.0f);
    guitar.strum ({ 40, 47, 52, 56, 59, 64 }, 12.0, sr);
    pump (400);
    if (auto* scopeView = findChild<ScopeView> (*ed))
    {
        double ms = 0.0, worst = 0.0;
        const int frames = 60;
        for (int i = 0; i < frames; ++i)
        {
            pump (10);
            const double t = paintSoftwareMs (*scopeView);
            ms += t;
            worst = juce::jmax (worst, t);
        }
        std::cout << "  2D scope paint (" << scopeView->getWidth() << "x" << scopeView->getHeight() << "): "
                  << juce::String (ms / frames, 2) << " ms per frame, worst " << juce::String (worst, 2) << " ms\n";
    }
    const double firstPaint = paintSoftwareMs (*ed);
    std::cout << "  whole editor paint (1500x940, on open and resize): first " << juce::String (firstPaint, 2)
              << " ms, then " << juce::String (paintSoftwareMs (*ed), 2) << " ms\n";
    {
        // every page's panel as the right column shows it (FX pages dim their controls while the module is off)
        juce::Rectangle<int> column;
        std::vector<PanelViewport*> pages;
        for (auto* child : ed->getChildren())
            if (auto* vp = dynamic_cast<PanelViewport*> (child))
            {
                pages.push_back (vp);
                if (vp->isVisible())
                    column = vp->getBounds();
            }
        std::vector<std::pair<double, juce::String>> times;
        for (auto* vp : pages)
        {
            vp->setBounds (column);
            vp->fitPanel();
            paintSoftwareMs (*vp);   // warm-up
            times.push_back ({ paintSoftwareMs (*vp), vp->panel.title() });
        }
        std::sort (times.begin(), times.end(), [] (auto& a, auto& b) { return a.first > b.first; });
        std::cout << "  page panels (" << column.getWidth() << "x" << column.getHeight() << "), slowest:";
        for (size_t i = 0; i < juce::jmin ((size_t) 4, times.size()); ++i)
            std::cout << "  " << times[i].second << " " << juce::String (times[i].first, 2) << " ms";
        std::cout << "\n";
    }
    auto timerLoad = [] (const char* what)
    {
        // timers alone (meters, scope traces, panels); nothing is painted here since there is no window
        const double c0 = threadCpuMs(), t0 = juce::Time::getMillisecondCounterHiRes();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (3000);
        const double load = (threadCpuMs() - c0) / (juce::Time::getMillisecondCounterHiRes() - t0) * 100.0;
        std::cout << "  editor timers, " << what << ": " << juce::String (load, 1) << " % of one core\n";
    };
    timerLoad ("2D scope view");

    set ("view", 5.0f);   // XY
    pump (300);
    if (auto* scopeView = findChild<ScopeView> (*ed))
        std::cout << "  2D scope paint, XY: " << juce::String (paintSoftwareMs (*scopeView), 2) << " ms\n";
    timerLoad ("2D scope view, XY");
    save (*ed, outDir.getChildFile ("editor_xy.png"));
    set ("view", 0.0f);

    // The editor only produces 3D geometry while its window is showing, so drive its scene directly
    auto timeScene = [&] (Scene3D& scene, auto& producer, const char* name)
    {
        producer.produce (scene);
        paintSoftwareMs (scene);   // the first frame starts the GPU context
        double geometry = 0.0, drawing = 0.0, worst = 0.0;
        const int frames = 60;
        for (int i = 0; i < frames; ++i)
        {
            pump (10);
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            producer.produce (scene);
            const double t1 = juce::Time::getMillisecondCounterHiRes();
            paintSoftwareMs (scene);
            const double t2 = juce::Time::getMillisecondCounterHiRes();
            geometry += t1 - t0;
            drawing += t2 - t1;
            worst = juce::jmax (worst, t2 - t1);
        }
        std::cout << "  " << name << " (" << scene.getWidth() << "x" << scene.getHeight() << "): "
                  << (scene.isGpuAvailable() ? "rendered off-screen" : "GPU NOT AVAILABLE") << ", per frame: geometry "
                  << juce::String (geometry / frames, 2) << " ms, render + read back + paint " << juce::String (drawing / frames, 2)
                  << " ms (worst " << juce::String (worst, 2) << " ms)\n";
    };

    set ("mainView", 1.0f);
    pump (100);
    if (auto* scene = findChild<Scene3D> (*ed))
    {
        Views3D views (proc);
        views.reloadFromState();
        guitar.strum ({ 40, 47, 52, 56, 59, 64 }, 12.0, sr);
        timeScene (*scene, views, "3D view");
        save (*ed, outDir.getChildFile ("editor_3d.png"));

        set ("mainView", 2.0f);
        pump (100);
        orbital::OrbitalModel model (proc.apvts);
        OrbitalView orbitalView (proc, model);
        guitar.strum ({ 45, 52, 57 }, 10.0, sr);
        timeScene (*scene, orbitalView, "orbital view");
        save (*ed, outDir.getChildFile ("editor_orbital.png"));
    }
    else
    {
        std::cout << "  !! no 3D scene in the editor\n";
    }
    set ("mainView", 0.0f);
    pump (50);

    // Window renderers. Both windows are created hidden and never shown.
    {
        ed->setVisible (false);
        ed->addToDesktop (0);
        std::cout << "  editor window renderer: " << rendererOf (*ed) << "\n";
        juce::Component other;
        other.setSize (64, 64);
        other.addToDesktop (0);
        std::cout << "  other window renderer: " << rendererOf (other);
        streamsafe::useSoftwareRendererEverywhere();
        std::cout << ", after the sweep: " << rendererOf (other) << "\n";
        other.removeFromDesktop();
        ed->removeFromDesktop();
    }
    }   // wantBase

    ed.reset();
    proc.releaseResources();
    std::cout << "done\n";
    return 0;
}
