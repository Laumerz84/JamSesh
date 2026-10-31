#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../UI/Help.h"
#include "FactoryPresets.h"

class ClodScopeProcessor;

// Factory + user presets: load, save, browse by category / genre / search, favourites, next /
// previous, "modified" state, and the host's program list.
//
// Owner: Agent "Presets". The members marked CONTRACT are used by the processor and must keep
// these signatures; everything else in the public API is the owner's to design (the preset bar and
// browser in PresetUI.h are its only other users). Private state lives in Impl.
//
// Everything except the CONTRACT program calls is message-thread only.
//
// Files: user presets are XML files (*.clodpreset) in Documents/Jam Sesh/Presets; favourites and
// browser options live in Documents/Jam Sesh/PresetSettings.xml (AppPaths.h). The current preset's name,
// category, key and modified flag are stored in the APVTS state (properties presetName,
// presetCategory, presetFile, presetModified) so they come back with a project.
class PresetManager
{
public:
    static HelpList helpEntries();
    static juce::StringArray categories();   // instrument / sound-type categories, in browser order
    static juce::StringArray genres();       // genre tags
    // CONTRACT: which parameters a preset stores and resets. Sound-shaping parameters only (input
    // stage, equation, synth, sculpt, fx, double slit, mix, output); never display/view parameters.
    static bool isSoundParameter (const juce::String& paramId);

    explicit PresetManager (ClodScopeProcessor&);   // CONTRACT
    ~PresetManager();

    // CONTRACT: host program interface (factory presets). Safe from any thread.
    int getNumPrograms() const;
    int getCurrentProgram() const;
    void setCurrentProgram (int index);
    juce::String getProgramName (int index) const;

    // CONTRACT: called by the processor after the host restored a project's state
    void onStateRestored();

    //==============================================================================
    // One browsable preset (factory or user)
    struct Entry
    {
        juce::String key;           // "factory:<name>" or "user:<full path>" - stable identity
        juce::String name, category, description, author;
        juce::StringArray genres;
        bool factory = true;
        int factoryIndex = -1;      // index into the factory list (host program number)
        juce::File file;            // user presets only
    };

    // What the user types into the save dialog
    struct Metadata
    {
        juce::String name, category, description, author;
        juce::StringArray genres;
    };

    // Browser filter; next / previous step through the presets that match it
    struct Filter
    {
        juce::String group = "All";   // "All", "Favourites", "User" or one of categories()
        juce::StringArray genres;     // any of these (empty = no genre filter)
        juce::String search;          // every word must appear in name, description, category or genres
    };

    static juce::String allGroup()        { return "All"; }
    static juce::String favouritesGroup() { return "Favourites"; }
    static juce::String userGroup()       { return "User"; }

    // ---- library ----
    const std::vector<Entry>& entries() const;     // factory first (library order), then user (by name)
    int indexOfKey (const juce::String& key) const;
    const Entry* findEntry (const juce::String& key) const;
    void rescanUserPresets();                       // re-reads the user folder and the settings file
    const std::vector<FactoryPreset>& factoryPresets() const;

    // ---- filtering ----
    void setFilter (const Filter&);
    const Filter& getFilter() const;
    bool matches (const Entry&, const Filter&) const;
    std::vector<int> filteredIndices() const;                 // indices into entries() for the current filter
    std::vector<int> filteredIndices (const Filter&) const;
    int countInGroup (const juce::String& group) const;       // ignoring genres and search

    // ---- loading ----
    bool loadEntry (int entryIndex);
    bool loadKey (const juce::String& key);
    bool loadFactory (int factoryIndex);
    bool loadFile (const juce::File&);    // any .clodpreset file, also outside the user folder
    void loadInit();
    void loadNext();                      // within the current filter (all presets if it matches nothing)
    void loadPrevious();
    bool revert();                        // reload the current preset, discarding edits

    // ---- current preset ----
    juce::String getCurrentName() const;
    juce::String getCurrentCategory() const;
    juce::String getCurrentKey() const;
    const Entry* currentEntry() const;    // nullptr when the current preset is no longer in the library
    bool isModified() const;
    bool isCurrentUserPreset() const;
    Metadata currentMetadata() const;     // prefills the save dialog

    // ---- user presets ----
    juce::File getPresetsFolder() const;            // created on demand
    juce::File userFileFor (const juce::String& name) const;
    bool userPresetExists (const juce::String& name) const;
    juce::String uniqueUserName (const juce::String& base) const;
    // Writes the current sound under the metadata's name (overwriting a user preset of that name
    // when allowed) and makes it the current preset. Returns "" on success or an error message.
    juce::String saveUserPreset (const Metadata&, bool allowOverwrite);
    juce::String renameUserPreset (const juce::String& key, const juce::String& newName);
    juce::String deleteUserPreset (const juce::String& key);   // moves the file to the bin
    void revealPresetsFolder() const;

    // ---- favourites + options ----
    bool isFavourite (const juce::String& key) const;
    void setFavourite (const juce::String& key, bool);
    void toggleFavourite (const juce::String& key);
    bool getCloseBrowserOnLoad() const;
    void setCloseBrowserOnLoad (bool);

    // ---- presets as data ----
    std::unique_ptr<juce::XmlElement> createPresetXml (const Metadata&) const;   // the current sound
    bool applyPresetXml (const juce::XmlElement&, const juce::String& key);     // loads; false if not a preset

    // Problems in the factory library (unknown ids / choices / categories / genres, values out of
    // range). Printed with DBG on construction in debug builds.
    juce::StringArray validateFactoryPresets() const;

    // ---- change notifications (message thread) ----
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void presetChanged() {}        // another preset loaded, or the modified flag changed
        virtual void presetListChanged() {}    // presets added / removed / renamed, favourites changed
    };
    void addListener (Listener*);
    void removeListener (Listener*);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
