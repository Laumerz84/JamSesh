#pragma once

#include <juce_core/juce_core.h>

// Where Jam Sesh keeps its files (owner: rebrand soldier). Until 1.4.0 the folders were called "Clod Scope":
// the old folder is moved (renamed) to the new name once, when the new one doesn't exist yet. If that fails (a
// file in it is in use, e.g. by an older version that still runs), the old folder is used, with its old file
// names, and the move is tried again next time. Nothing is ever deleted.
//   settings()   %APPDATA%\Jam Sesh (Mac: ~/Library/Application Support/Jam Sesh): Theme.settings,
//                Jam Sesh.settings (your name, port and last code) and the app's App.settings
//   documents()  Documents\Jam Sesh: Presets\, PresetSettings.xml, Recordings\
// Folders move only once someone uses Jam Sesh: the app at its start, the plugin when its window opens
// (allowMoves). A processor alone moves nothing: the VST3 build creates one for its moduleinfo.json, plugin
// scanners and test hosts (PluginTest) do too. The headless tools (ClodScopeSnap, ClodJamTest) never move.
// Tests: useTestRoot() or the env var CLOD_DATA_DIR=<folder> put every folder in <folder>\AppData and
// <folder>\Documents instead of the user's (moves allowed there). The app's --selftest does that.
namespace apppaths
{
    inline constexpr const char* newName = "Jam Sesh";
    inline constexpr const char* oldName = "Clod Scope";

    struct Folder
    {
        juce::File dir;
        bool old = false;   // the old folder, not moved (yet): everything in it keeps its old name
    };

    // The folder "Jam Sesh" in base, moved there from "Clod Scope" when only the old one exists (and mayMove)
    inline Folder moveOnce (const juce::File& base, bool mayMove = true)
    {
        const auto now = base.getChildFile (newName), before = base.getChildFile (oldName);
        if (now.exists() || ! before.isDirectory())
            return { now, false };
        // moveFileTo replaces nothing here: the new name doesn't exist (checked above)
        if (mayMove && (before.moveFileTo (now) || now.isDirectory()))   // or another process just moved it
            return { now, false };
        return { before, true };
    }

    namespace detail
    {
        inline juce::File& testRoot()
        {
            static juce::File root;
            return root;
        }

        // A folder in place of the user's: useTestRoot(), else CLOD_DATA_DIR
        inline juce::File sandbox()
        {
            if (testRoot() != juce::File())
                return testRoot();
            const auto env = juce::SystemStats::getEnvironmentVariable ("CLOD_DATA_DIR", {}).trim();
            return env.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile (env) : juce::File();
        }

        inline juce::File settingsBase()
        {
            if (const auto s = sandbox(); s != juce::File())
                return s.getChildFile ("AppData");
            const auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
           #if JUCE_MAC
            return d.getChildFile ("Application Support");   // JUCE's is ~/Library
           #else
            return d;
           #endif
        }

        inline juce::File documentsBase()
        {
            if (const auto s = sandbox(); s != juce::File())
                return s.getChildFile ("Documents");
            return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        }

        struct State
        {
            juce::CriticalSection lock;
            bool resolved = false, mayMove = false;
            Folder settings, documents;
        };

        inline State& state()
        {
            static State s;
            return s;
        }

        // lock held; decided on first use, each folder once per process (allowMoves can still move an old one)
        inline void resolve (State& s)
        {
            if (s.resolved)
                return;
            const bool mayMove = s.mayMove || sandbox() != juce::File();
            s.settings = moveOnce (settingsBase(), mayMove);
            s.documents = moveOnce (documentsBase(), mayMove);
            s.resolved = true;
        }
    }

    // Tests: every folder under root (root/AppData, root/Documents) instead of the user's. Call it before anything
    // asks for a folder.
    inline void useTestRoot (const juce::File& root)   { detail::testRoot() = root; }

    // The app at its start and the plugin's editor call this: from now on this process moves the old folders,
    // including one already in use as the old folder. True if a folder moved just now (the caller re-reads what
    // it loaded from there). The headless tools never move anything.
    inline bool allowMoves()
    {
       #if JucePlugin_Build_VST3 || JucePlugin_Build_Standalone
        auto& s = detail::state();
        const juce::ScopedLock sl (s.lock);
        if (s.mayMove)
            return false;
        s.mayMove = true;
        if (! s.resolved)
            return false;   // moved on first use
        bool moved = false;
        if (s.settings.old)
        {
            s.settings = moveOnce (detail::settingsBase(), true);
            moved = moved || ! s.settings.old;
        }
        if (s.documents.old)
        {
            s.documents = moveOnce (detail::documentsBase(), true);
            moved = moved || ! s.documents.old;
        }
        return moved;
       #else
        return false;
       #endif
    }

    inline Folder settingsFolder()
    {
        auto& s = detail::state();
        const juce::ScopedLock sl (s.lock);
        detail::resolve (s);
        return s.settings;
    }

    inline Folder documentsFolder()
    {
        auto& s = detail::state();
        const juce::ScopedLock sl (s.lock);
        detail::resolve (s);
        return s.documents;
    }

    inline juce::File settings()    { return settingsFolder().dir; }
    inline juce::File documents()   { return documentsFolder().dir; }

    // The app's own settings (audio setup, window, the last state): App.settings, renamed once from the
    // "Clod Scope.settings" of older versions (which named it after the plugin)
    inline juce::File appSettingsFile()
    {
        const auto f = settingsFolder();
        const auto before = f.dir.getChildFile ("Clod Scope.settings");
        if (f.old)
            return before;
        const auto now = f.dir.getChildFile ("App.settings");
        if (now.exists() || ! before.existsAsFile())
            return now;
        return before.moveFileTo (now) ? now : before;
    }

    // A file or folder as help texts show it: "Documents\Jam Sesh\Presets", "%APPDATA%\Jam Sesh\Theme.settings"
    // (Mac: "Documents/Jam Sesh/Presets", "~/Library/Application Support/Jam Sesh/Theme.settings")
    inline juce::String shown (const juce::File& f)
    {
        const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        if (f.isAChildOf (docs))
            return "Documents" + juce::String (juce::File::getSeparatorString()) + f.getRelativePathFrom (docs);
       #if JUCE_WINDOWS
        const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
        if (f.isAChildOf (appData))
            return "%APPDATA%\\" + f.getRelativePathFrom (appData);
       #else
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        if (f.isAChildOf (home))
            return "~/" + f.getRelativePathFrom (home);
       #endif
        return f.getFullPathName();
    }
}
