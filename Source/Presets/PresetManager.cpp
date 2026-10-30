#include "PresetManager.h"
#include "../PluginProcessor.h"
#include <algorithm>
#include <map>

namespace
{
    const juce::Identifier propName     { "presetName" };
    const juce::Identifier propCategory { "presetCategory" };
    const juce::Identifier propFile     { "presetFile" };
    const juce::Identifier propModified { "presetModified" };
    const juce::Identifier propFormula  { "eqFormula" };

    const juce::String presetTag     { "ClodScopePreset" };
    const juce::String fileExtension { ".clodpreset" };
    constexpr int formatVersion = 1;

    juce::String factoryKey (const juce::String& name) { return "factory:" + name; }

    // Short, exact text for a real parameter value (no trailing zeros)
    juce::String formatValue (float v)
    {
        auto s = juce::String (v, 6);
        if (s.containsChar ('.'))
            s = s.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
        return s == "-0" ? juce::String ("0") : s;
    }

    bool sameTable (const sculpt::BaseTable& a, const sculpt::BaseTable& b)
    {
        return a.amp == b.amp && a.phase == b.phase;
    }
}

//==============================================================================
struct PresetManager::Impl : public juce::AudioProcessorValueTreeState::Listener,
                             public juce::ValueTree::Listener,
                             public juce::Timer
{
    Impl (PresetManager& o, ClodScopeProcessor& p) : owner (o), proc (p), state (p.apvts.state)
    {
        factory = makeFactoryPresets();

        for (auto* ap : proc.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (ap))
                if (isSoundParameter (rp->getParameterID()))
                {
                    soundParams.push_back (rp);
                    proc.apvts.addParameterListener (rp->getParameterID(), this);
                }

        // the processor is constructed with every module at its defaults, so this is "Init"
        for (int which = 0; which < 2; ++which)
        {
            defaultTable[which] = proc.sculpt.getTable (which);
            if (std::all_of (defaultTable[which].amp.begin(), defaultTable[which].amp.end(), [] (float a) { return a == 0.0f; }))
                defaultTable[which] = SculptModel::makeWave (0);   // never let "Init" mean silence
        }
        defaultFormula = proc.soundEq.getCustomFormula();

        if (! factory.empty())
        {
            currentKey = factoryKey (factory.front().name);
            currentName = factory.front().name;
            currentCategory = factory.front().category;
        }

        state.addListener (this);
        loadSettings();
        scanUser();
        startTimerHz (15);
    }

    ~Impl() override
    {
        stopTimer();
        state.removeListener (this);
        for (auto* rp : soundParams)
            proc.apvts.removeParameterListener (rp->getParameterID(), this);
    }

    //==============================================================================
    // Edits can arrive on any thread (host automation runs on the audio thread), so listeners
    // only raise a flag and the timer handles it on the message thread.
    void parameterChanged (const juce::String&, float) override
    {
        if (! loading.load())
            dirtyPending = true;
    }

    static bool insideSculpt (juce::ValueTree t)
    {
        for (; t.isValid(); t = t.getParent())
            if (t.hasType ("Sculpt"))
                return true;
        return false;
    }

    void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& prop) override
    {
        if (loading.load())
            return;
        if ((t == state && prop == propFormula) || (t != state && insideSculpt (t)))
            dirtyPending = true;
    }

    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        if (! loading.load() && (insideSculpt (parent) || child.hasType ("Sculpt")))
            dirtyPending = true;
    }

    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int) override
    {
        if (! loading.load() && insideSculpt (parent))
            dirtyPending = true;
    }

    void timerCallback() override
    {
        const int program = pendingProgram.exchange (-1);
        if (program >= 0)
            owner.loadFactory (program);

        if (notifyPending.exchange (false))
            notifyChanged();

        if (dirtyPending.exchange (false) && juce::Time::getMillisecondCounterHiRes() > ignoreUntil && ! modified)
        {
            modified = true;
            state.setProperty (propModified, true, nullptr);
            notifyChanged();
        }
    }

    void notifyChanged()     { listeners.call ([] (PresetManager::Listener& l) { l.presetChanged(); }); }
    void notifyListChanged() { listeners.call ([] (PresetManager::Listener& l) { l.presetListChanged(); }); }

    //==============================================================================
    // Documents/Clod Scope/Presets; only created when something is written there
    juce::File folder (bool create = false) const
    {
        auto f = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                     .getChildFile ("Clod Scope").getChildFile ("Presets");
        if (create && ! f.isDirectory())
            f.createDirectory();
        return f;
    }

    juce::File settingsFile() const { return folder().getParentDirectory().getChildFile ("PresetSettings.xml"); }

    juce::String keyForFile (const juce::File& f) const
    {
        const auto dir = folder();
        if (f.isAChildOf (dir))
            return "user:" + f.getRelativePathFrom (dir).replaceCharacter ('\\', '/');
        return "file:" + f.getFullPathName();
    }

    juce::File fileForKey (const juce::String& key) const
    {
        if (key.startsWith ("user:"))
            return folder().getChildFile (key.fromFirstOccurrenceOf ("user:", false, false));
        if (key.startsWith ("file:"))
            return juce::File (key.fromFirstOccurrenceOf ("file:", false, false));
        return {};
    }

    void loadSettings()
    {
        favourites.clear();
        if (auto xml = juce::XmlDocument::parse (settingsFile()))
        {
            closeOnLoad = xml->getBoolAttribute ("closeOnLoad", false);
            lastAuthor = xml->getStringAttribute ("author");
            for (auto* e : xml->getChildWithTagNameIterator ("Favourite"))
                favourites.addIfNotAlreadyThere (e->getStringAttribute ("key"));
        }
    }

    void saveSettings() const
    {
        juce::XmlElement xml ("PresetSettings");
        xml.setAttribute ("closeOnLoad", closeOnLoad);
        xml.setAttribute ("author", lastAuthor);
        for (auto& k : favourites)
            xml.createNewChildElement ("Favourite")->setAttribute ("key", k);
        const auto file = settingsFile();
        file.getParentDirectory().createDirectory();
        xml.writeTo (file);
    }

    static Entry entryFromXml (const juce::XmlElement& xml, const juce::File& file, const juce::String& key)
    {
        Entry e;
        e.key = key;
        e.factory = false;
        e.file = file;
        e.name = xml.getStringAttribute ("name", file.getFileNameWithoutExtension());
        e.category = xml.getStringAttribute ("category", "Experimental & Physics");
        e.description = xml.getStringAttribute ("description");
        e.author = xml.getStringAttribute ("author");
        e.genres.addTokens (xml.getStringAttribute ("genres"), ";", "");
        e.genres.trim();
        e.genres.removeEmptyStrings();
        return e;
    }

    void scanUser()
    {
        list.clear();
        for (int i = 0; i < (int) factory.size(); ++i)
        {
            const auto& f = factory[(size_t) i];
            Entry e;
            e.key = factoryKey (f.name);
            e.name = f.name;
            e.category = f.category;
            e.description = f.description;
            e.genres = f.genres;
            e.author = "Clod Scope";
            e.factory = true;
            e.factoryIndex = i;
            list.push_back (e);
        }

        std::vector<Entry> user;
        if (folder().isDirectory())
            for (const auto& file : folder().findChildFiles (juce::File::findFiles, true, "*" + fileExtension))
                if (auto xml = juce::XmlDocument::parse (file))
                    if (xml->hasTagName (presetTag))
                        user.push_back (entryFromXml (*xml, file, keyForFile (file)));

        std::sort (user.begin(), user.end(), [] (const Entry& a, const Entry& b)
                   { return a.name.compareNatural (b.name) < 0; });
        for (auto& e : user)
            list.push_back (std::move (e));
    }

    //==============================================================================
    sculpt::BaseTable resolveTable (const juce::String& text, int which) const
    {
        if (text.startsWith ("wave:"))
        {
            const int idx = SculptModel::waveNames().indexOf (text.substring (5).trim(), true);
            if (idx >= 0)
                return SculptModel::makeWave (idx);
        }
        else if (text.isNotEmpty())
        {
            sculpt::BaseTable t;
            if (SculptModel::tableFromString (text, t))
                return t;
        }
        return defaultTable[which];
    }

    struct Sound
    {
        std::map<juce::String, float> values;          // real values by id
        std::map<juce::String, juce::String> choices;  // choice text by id (wins over values when it resolves)
        juce::String tableA, tableB, formula;
    };

    // Resets every sound parameter to its default, then applies the preset. Message thread.
    void applySound (const Sound& s, const juce::String& key, const juce::String& name, const juce::String& category)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        loading = true;

        for (auto* p : soundParams)
        {
            const auto id = p->getParameterID();
            float target = p->getDefaultValue();

            if (auto it = s.values.find (id); it != s.values.end())
                target = p->convertTo0to1 (it->second);

            if (auto it = s.choices.find (id); it != s.choices.end())
                if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (p))
                {
                    const int idx = cp->choices.indexOf (it->second, true);
                    if (idx >= 0)
                        target = cp->convertTo0to1 ((float) idx);
                }

            if (std::abs (p->getValue() - target) > 1.0e-6f)
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (target);
                p->endChangeGesture();
            }
        }

        for (int which = 0; which < 2; ++which)
        {
            const auto table = resolveTable (which == 0 ? s.tableA : s.tableB, which);
            if (! sameTable (table, proc.sculpt.getTable (which)))
                proc.sculpt.setTable (which, table, false);
        }

        const auto formula = s.formula.isNotEmpty() ? s.formula : defaultFormula;
        if (formula != proc.soundEq.getCustomFormula())
            proc.soundEq.setCustomFormula (formula);

        currentKey = key;
        currentName = name;
        currentCategory = category;
        modified = false;
        state.setProperty (propName, name, nullptr);
        state.setProperty (propCategory, category, nullptr);
        state.setProperty (propFile, key, nullptr);
        state.setProperty (propModified, false, nullptr);

        loading = false;
        dirtyPending = false;
        ignoreUntil = juce::Time::getMillisecondCounterHiRes() + 300.0;   // modules may publish a moment later
        notifyChanged();
    }

    static Sound soundFromFactory (const FactoryPreset& f)
    {
        Sound s;
        for (auto& [id, v] : f.values)
            s.values[id] = v;
        for (auto& [id, text] : f.choices)
            s.choices[id] = text;
        s.tableA = f.sculptTableA;
        s.tableB = f.sculptTableB;
        s.formula = f.equationFormula;
        return s;
    }

    static Sound soundFromXml (const juce::XmlElement& xml)
    {
        Sound s;
        if (auto* params = xml.getChildByName ("Params"))
            for (auto* e : params->getChildWithTagNameIterator ("Param"))
            {
                const auto id = e->getStringAttribute ("id");
                if (id.isEmpty())
                    continue;
                if (e->hasAttribute ("value"))
                    s.values[id] = (float) e->getDoubleAttribute ("value");
                if (e->hasAttribute ("choice"))
                    s.choices[id] = e->getStringAttribute ("choice");
            }
        if (auto* sc = xml.getChildByName ("Sculpt"))
        {
            s.tableA = sc->getStringAttribute ("tableA");
            s.tableB = sc->getStringAttribute ("tableB");
        }
        if (auto* eq = xml.getChildByName ("Equation"))
            s.formula = eq->getStringAttribute ("formula");
        return s;
    }

    //==============================================================================
    PresetManager& owner;
    ClodScopeProcessor& proc;
    juce::ValueTree& state;   // the APVTS member itself: replaceState() redirects its listeners

    std::vector<FactoryPreset> factory;     // immutable after construction (read by host threads)
    std::vector<Entry> list;
    std::vector<juce::RangedAudioParameter*> soundParams;
    sculpt::BaseTable defaultTable[2];
    juce::String defaultFormula;

    Filter filter;
    juce::StringArray favourites;
    bool closeOnLoad = false;
    juce::String lastAuthor;

    juce::String currentKey, currentName { "Init" }, currentCategory { "Init" };
    bool modified = false;

    std::atomic<int> hostProgram { 0 }, pendingProgram { -1 };
    std::atomic<bool> loading { false }, dirtyPending { false }, notifyPending { false };
    double ignoreUntil = 0.0;

    juce::ListenerList<PresetManager::Listener> listeners;
};

//==============================================================================
juce::StringArray PresetManager::categories()
{
    return { "Init", "Guitar", "Bass", "Keys & Organ", "Synth Lead", "Synth Bass", "Pad", "Pluck & Arp",
             "Strings & Brass", "Winds", "Bells & Mallets", "Vocal", "Drums & Perc", "Lo-fi & Texture",
             "Experimental & Physics" };
}

juce::StringArray PresetManager::genres()
{
    return { "Rock", "Metal", "Blues", "Pop", "Hip-Hop & Trap", "R&B & Soul", "EDM", "House & Techno",
             "Synthwave", "Lo-fi", "Ambient", "Cinematic", "Jazz", "Funk", "Indie", "Experimental" };
}

bool PresetManager::isSoundParameter (const juce::String& id)
{
    static const juce::StringArray inputStage { "inGain", "gate", "dc", "invert", "drive", "shape", "bits",
                                                "downsample", "hpf", "lpf", "mix", "outGain" };
    if (inputStage.contains (id))
        return true;
    for (auto* prefix : { "eq", "syn", "sc", "fx", "ds" })
        if (id.startsWith (prefix))
            return true;
    return false;
}

PresetManager::PresetManager (ClodScopeProcessor& p) : impl (std::make_unique<Impl> (*this, p))
{
   #if JUCE_DEBUG
    for (auto& problem : validateFactoryPresets())
        DBG ("Factory preset: " << problem);
   #endif

    // Developer hook for the preset render tests: CLOD_PRESET_VALIDATE=<file> writes the report
    const auto reportPath = juce::SystemStats::getEnvironmentVariable ("CLOD_PRESET_VALIDATE", {});
    if (reportPath.isNotEmpty() && juce::File::isAbsolutePath (reportPath))
    {
        const auto problems = validateFactoryPresets();
        juce::File (reportPath).replaceWithText (juce::String (getNumPrograms()) + " factory presets, "
                                                 + juce::String (problems.size()) + " problems\n"
                                                 + problems.joinIntoString ("\n") + "\n");
    }
}

PresetManager::~PresetManager() = default;

//==============================================================================
int PresetManager::getNumPrograms() const { return (int) impl->factory.size(); }

int PresetManager::getCurrentProgram() const
{
    return juce::jlimit (0, juce::jmax (0, getNumPrograms() - 1), impl->hostProgram.load());
}

void PresetManager::setCurrentProgram (int index)
{
    if (index < 0 || index >= getNumPrograms() || index == impl->hostProgram.load())
        return;   // some hosts re-send the current program on load; that must not wipe a restored state

    impl->hostProgram = index;
    if (juce::MessageManager::existsAndIsCurrentThread())
        loadFactory (index);
    else
        impl->pendingProgram = index;   // the timer loads it on the message thread
}

juce::String PresetManager::getProgramName (int index) const
{
    return index >= 0 && index < getNumPrograms() ? impl->factory[(size_t) index].name : juce::String();
}

void PresetManager::onStateRestored()
{
    auto& s = *impl;
    const auto& st = s.state;
    if (st.hasProperty (propName))
    {
        s.currentName = st.getProperty (propName).toString();
        s.currentCategory = st.getProperty (propCategory).toString();
        s.currentKey = st.getProperty (propFile).toString();
        s.modified = (bool) st.getProperty (propModified, false);
    }

    for (int i = 0; i < getNumPrograms(); ++i)
        if (s.currentKey == factoryKey (s.factory[(size_t) i].name))
            s.hostProgram = i;

    // the restore itself fired every parameter listener; none of that is a user edit
    s.dirtyPending = false;
    s.ignoreUntil = juce::Time::getMillisecondCounterHiRes() + 500.0;
    s.notifyPending = true;
}

//==============================================================================
const std::vector<PresetManager::Entry>& PresetManager::entries() const { return impl->list; }
const std::vector<FactoryPreset>& PresetManager::factoryPresets() const { return impl->factory; }

int PresetManager::indexOfKey (const juce::String& key) const
{
    for (size_t i = 0; i < impl->list.size(); ++i)
        if (impl->list[i].key == key)
            return (int) i;
    return -1;
}

const PresetManager::Entry* PresetManager::findEntry (const juce::String& key) const
{
    const int i = indexOfKey (key);
    return i >= 0 ? &impl->list[(size_t) i] : nullptr;
}

void PresetManager::rescanUserPresets()
{
    impl->loadSettings();
    impl->scanUser();
    impl->notifyListChanged();
}

//==============================================================================
void PresetManager::setFilter (const Filter& f) { impl->filter = f; }
const PresetManager::Filter& PresetManager::getFilter() const { return impl->filter; }

bool PresetManager::matches (const Entry& e, const Filter& f) const
{
    if (f.group == favouritesGroup())
    {
        if (! isFavourite (e.key))
            return false;
    }
    else if (f.group == userGroup())
    {
        if (e.factory)
            return false;
    }
    else if (f.group.isNotEmpty() && f.group != allGroup() && ! e.category.equalsIgnoreCase (f.group))
    {
        return false;
    }

    if (! f.genres.isEmpty())
    {
        bool any = false;
        for (auto& g : f.genres)
            any = any || e.genres.contains (g, true);
        if (! any)
            return false;
    }

    if (f.search.trim().isNotEmpty())
    {
        const auto hay = e.name + " " + e.category + " " + e.description + " "
                       + e.genres.joinIntoString (" ") + " " + e.author + (e.factory ? "" : " user");
        juce::StringArray words;
        words.addTokens (f.search, " ,", "\"");
        words.trim();
        words.removeEmptyStrings();
        for (auto& w : words)
            if (! hay.containsIgnoreCase (w))
                return false;
    }
    return true;
}

std::vector<int> PresetManager::filteredIndices() const { return filteredIndices (impl->filter); }

std::vector<int> PresetManager::filteredIndices (const Filter& f) const
{
    std::vector<int> out;
    for (size_t i = 0; i < impl->list.size(); ++i)
        if (matches (impl->list[i], f))
            out.push_back ((int) i);
    return out;
}

int PresetManager::countInGroup (const juce::String& group) const
{
    Filter f;
    f.group = group;
    return (int) filteredIndices (f).size();
}

//==============================================================================
bool PresetManager::loadEntry (int entryIndex)
{
    if (entryIndex < 0 || entryIndex >= (int) impl->list.size())
        return false;
    const auto e = impl->list[(size_t) entryIndex];
    return e.factory ? loadFactory (e.factoryIndex) : loadFile (e.file);
}

bool PresetManager::loadKey (const juce::String& key)
{
    if (key.startsWith ("factory:"))
    {
        for (int i = 0; i < getNumPrograms(); ++i)
            if (key == factoryKey (impl->factory[(size_t) i].name))
                return loadFactory (i);
        return false;
    }
    const auto f = impl->fileForKey (key);
    return f.existsAsFile() && loadFile (f);
}

bool PresetManager::loadFactory (int index)
{
    if (index < 0 || index >= getNumPrograms())
        return false;
    const auto& f = impl->factory[(size_t) index];
    impl->hostProgram = index;   // before notifying the host, so its echo is a no-op
    impl->applySound (Impl::soundFromFactory (f), factoryKey (f.name), f.name, f.category);
    impl->proc.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withProgramChanged (true));
    return true;
}

bool PresetManager::loadFile (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    return xml != nullptr && applyPresetXml (*xml, impl->keyForFile (file));
}

void PresetManager::loadInit()
{
    if (getNumPrograms() > 0)
        loadFactory (0);
    else
        impl->applySound ({}, factoryKey ("Init"), "Init", "Init");
}

void PresetManager::loadNext()
{
    auto order = filteredIndices();
    if (order.empty())
        for (int i = 0; i < (int) impl->list.size(); ++i)
            order.push_back (i);
    if (order.empty())
        return;

    const int cur = indexOfKey (impl->currentKey);
    auto it = std::find (order.begin(), order.end(), cur);
    const size_t next = it == order.end() ? 0 : ((size_t) (it - order.begin()) + 1) % order.size();
    loadEntry (order[next]);
}

void PresetManager::loadPrevious()
{
    auto order = filteredIndices();
    if (order.empty())
        for (int i = 0; i < (int) impl->list.size(); ++i)
            order.push_back (i);
    if (order.empty())
        return;

    const int cur = indexOfKey (impl->currentKey);
    auto it = std::find (order.begin(), order.end(), cur);
    const size_t pos = it == order.end() ? 0 : (size_t) (it - order.begin());
    loadEntry (order[(pos + order.size() - 1) % order.size()]);
}

bool PresetManager::revert() { return loadKey (impl->currentKey); }

//==============================================================================
juce::String PresetManager::getCurrentName() const     { return impl->currentName; }
juce::String PresetManager::getCurrentCategory() const { return impl->currentCategory; }
juce::String PresetManager::getCurrentKey() const      { return impl->currentKey; }
const PresetManager::Entry* PresetManager::currentEntry() const { return findEntry (impl->currentKey); }
bool PresetManager::isModified() const                 { return impl->modified; }

bool PresetManager::isCurrentUserPreset() const
{
    auto* e = currentEntry();
    return e != nullptr && ! e->factory;
}

PresetManager::Metadata PresetManager::currentMetadata() const
{
    Metadata m;
    m.name = impl->currentName;
    m.category = impl->currentCategory;
    m.author = impl->lastAuthor;
    if (auto* e = currentEntry())
    {
        m.description = e->description;
        m.genres = e->genres;
        if (! e->factory && e->author.isNotEmpty())
            m.author = e->author;
    }
    if (! categories().contains (m.category))
        m.category = "Experimental & Physics";
    return m;
}

//==============================================================================
juce::File PresetManager::getPresetsFolder() const { return impl->folder (true); }

juce::File PresetManager::userFileFor (const juce::String& name) const
{
    return impl->folder().getChildFile (juce::File::createLegalFileName (name.trim()) + fileExtension);
}

bool PresetManager::userPresetExists (const juce::String& name) const
{
    return name.trim().isNotEmpty() && userFileFor (name).existsAsFile();
}

juce::String PresetManager::uniqueUserName (const juce::String& base) const
{
    auto stem = base.trim().isNotEmpty() ? base.trim() : juce::String ("My Preset");
    if (! userPresetExists (stem))
        return stem;
    for (int n = 2; n < 1000; ++n)
        if (! userPresetExists (stem + " " + juce::String (n)))
            return stem + " " + juce::String (n);
    return stem + " " + juce::String (juce::Time::currentTimeMillis());
}

std::unique_ptr<juce::XmlElement> PresetManager::createPresetXml (const Metadata& m) const
{
    auto xml = std::make_unique<juce::XmlElement> (presetTag);
    xml->setAttribute ("version", formatVersion);
    xml->setAttribute ("plugin", JucePlugin_VersionString);
    xml->setAttribute ("name", m.name.trim());
    xml->setAttribute ("category", m.category);
    xml->setAttribute ("genres", m.genres.joinIntoString (";"));
    xml->setAttribute ("description", m.description.trim());
    xml->setAttribute ("author", m.author.trim());

    auto* params = xml->createNewChildElement ("Params");
    for (auto* p : impl->soundParams)
    {
        auto* e = params->createNewChildElement ("Param");
        e->setAttribute ("id", p->getParameterID());
        const float real = p->convertFrom0to1 (p->getValue());
        e->setAttribute ("value", formatValue (real));
        if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (p))
            e->setAttribute ("choice", cp->getCurrentChoiceName());
    }

    auto* sc = xml->createNewChildElement ("Sculpt");
    sc->setAttribute ("tableA", SculptModel::tableToString (impl->proc.sculpt.getTable (0)));
    sc->setAttribute ("tableB", SculptModel::tableToString (impl->proc.sculpt.getTable (1)));
    xml->createNewChildElement ("Equation")->setAttribute ("formula", impl->proc.soundEq.getCustomFormula());
    return xml;
}

bool PresetManager::applyPresetXml (const juce::XmlElement& xml, const juce::String& key)
{
    if (! xml.hasTagName (presetTag))
        return false;
    const auto name = xml.getStringAttribute ("name", impl->fileForKey (key).getFileNameWithoutExtension());
    impl->applySound (Impl::soundFromXml (xml), key, name, xml.getStringAttribute ("category"));
    return true;
}

juce::String PresetManager::saveUserPreset (const Metadata& m, bool allowOverwrite)
{
    if (m.name.trim().isEmpty())
        return "Please give the preset a name.";
    getPresetsFolder();   // creates it
    const auto file = userFileFor (m.name);
    if (file.existsAsFile() && ! allowOverwrite)
        return "A user preset called \"" + m.name.trim() + "\" already exists.";

    auto xml = createPresetXml (m);
    if (! xml->writeTo (file))
        return "Could not write " + file.getFullPathName();

    auto& s = *impl;
    s.lastAuthor = m.author.trim();
    s.saveSettings();
    s.scanUser();

    s.currentKey = s.keyForFile (file);
    s.currentName = m.name.trim();
    s.currentCategory = m.category;
    s.modified = false;
    s.state.setProperty (propName, s.currentName, nullptr);
    s.state.setProperty (propCategory, s.currentCategory, nullptr);
    s.state.setProperty (propFile, s.currentKey, nullptr);
    s.state.setProperty (propModified, false, nullptr);
    s.notifyListChanged();
    s.notifyChanged();
    return {};
}

juce::String PresetManager::renameUserPreset (const juce::String& key, const juce::String& newName)
{
    auto* e = findEntry (key);
    if (e == nullptr || e->factory)
        return "Only user presets can be renamed.";
    if (newName.trim().isEmpty())
        return "Please give the preset a name.";

    const auto oldFile = e->file;
    const auto newFile = userFileFor (newName);
    if (newFile != oldFile && newFile.existsAsFile())
        return "A user preset called \"" + newName.trim() + "\" already exists.";

    auto xml = juce::XmlDocument::parse (oldFile);
    if (xml == nullptr)
        return "Could not read " + oldFile.getFullPathName();
    xml->setAttribute ("name", newName.trim());
    if (! xml->writeTo (newFile))
        return "Could not write " + newFile.getFullPathName();
    if (newFile != oldFile)
        oldFile.deleteFile();

    auto& s = *impl;
    const auto newKey = s.keyForFile (newFile);
    if (s.favourites.contains (key))
    {
        s.favourites.removeString (key);
        s.favourites.add (newKey);
        s.saveSettings();
    }
    if (s.currentKey == key)
    {
        s.currentKey = newKey;
        s.currentName = newName.trim();
        s.state.setProperty (propName, s.currentName, nullptr);
        s.state.setProperty (propFile, s.currentKey, nullptr);
    }
    s.scanUser();
    s.notifyListChanged();
    s.notifyChanged();
    return {};
}

juce::String PresetManager::deleteUserPreset (const juce::String& key)
{
    auto* e = findEntry (key);
    if (e == nullptr || e->factory)
        return "Only user presets can be deleted.";

    const auto file = e->file;
    if (! file.moveToTrash() && ! file.deleteFile())
        return "Could not delete " + file.getFullPathName();

    auto& s = *impl;
    if (s.favourites.contains (key))
    {
        s.favourites.removeString (key);
        s.saveSettings();
    }
    // the sound stays as it is; it just no longer has a file behind it
    if (s.currentKey == key)
        s.modified = true;
    s.scanUser();
    s.notifyListChanged();
    s.notifyChanged();
    return {};
}

void PresetManager::revealPresetsFolder() const
{
    getPresetsFolder().startAsProcess();
}

//==============================================================================
bool PresetManager::isFavourite (const juce::String& key) const { return impl->favourites.contains (key); }

void PresetManager::setFavourite (const juce::String& key, bool fav)
{
    if (fav == isFavourite (key))
        return;
    if (fav)
        impl->favourites.add (key);
    else
        impl->favourites.removeString (key);
    impl->saveSettings();
    impl->notifyListChanged();
}

void PresetManager::toggleFavourite (const juce::String& key) { setFavourite (key, ! isFavourite (key)); }

bool PresetManager::getCloseBrowserOnLoad() const { return impl->closeOnLoad; }

void PresetManager::setCloseBrowserOnLoad (bool b)
{
    impl->closeOnLoad = b;
    impl->saveSettings();
}

//==============================================================================
juce::StringArray PresetManager::validateFactoryPresets() const
{
    juce::StringArray problems;
    const auto cats = categories();
    const auto gens = genres();
    const auto waves = SculptModel::waveNames();
    juce::StringArray names;
    auto& apvts = impl->proc.apvts;

    auto checkTable = [&] (const juce::String& where, const juce::String& t)
    {
        if (t.startsWith ("wave:"))
        {
            if (! waves.contains (t.substring (5).trim(), true))
                problems.add (where + ": unknown wave \"" + t.substring (5) + "\"");
        }
        else if (t.isNotEmpty())
        {
            sculpt::BaseTable tmp;
            if (! SculptModel::tableFromString (t, tmp))
                problems.add (where + ": table does not parse");
        }
    };

    for (auto& f : impl->factory)
    {
        const auto where = "\"" + f.name + "\"";
        if (names.contains (f.name))
            problems.add (where + ": duplicate name");
        names.add (f.name);
        if (! cats.contains (f.category))
            problems.add (where + ": unknown category \"" + f.category + "\"");
        for (auto& g : f.genres)
            if (! gens.contains (g))
                problems.add (where + ": unknown genre \"" + g + "\"");

        for (auto& [id, v] : f.values)
        {
            auto* p = apvts.getParameter (id);
            if (p == nullptr)
                problems.add (where + ": unknown parameter " + id);
            else if (! isSoundParameter (id))
                problems.add (where + ": " + id + " is not a sound parameter");
            else
            {
                const auto range = p->getNormalisableRange();
                if (v < range.start || v > range.end)
                    problems.add (where + ": " + id + " = " + formatValue (v) + " outside "
                                  + formatValue (range.start) + " .. " + formatValue (range.end));
            }
        }

        for (auto& [id, text] : f.choices)
        {
            auto* cp = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id));
            if (cp == nullptr)
                problems.add (where + ": " + id + " is not a choice parameter");
            else if (! cp->choices.contains (text, true))
                problems.add (where + ": " + id + " has no option \"" + text + "\"");
        }

        checkTable (where + " table A", f.sculptTableA);
        checkTable (where + " table B", f.sculptTableB);
    }
    return problems;
}

void PresetManager::addListener (Listener* l)    { impl->listeners.add (l); }
void PresetManager::removeListener (Listener* l) { impl->listeners.remove (l); }

//==============================================================================
HelpList PresetManager::helpEntries()
{
    const juce::String loadRule =
        "Loading a preset first resets EVERY sound control (input stage, synth, wave sculptor tables, "
        "equation incl. its custom formula, FX rack, double slit, mix, output) to its default and then "
        "applies the preset's values, so a preset always sounds the same whatever was loaded before. "
        "View settings (scope, 3D, orbital display, zoom) are never changed.";

    return {
        { "preset.name", "Preset",
          "The current preset. Click to open the preset browser.",
          "Shows name and category; a dot and \"*\" mean you changed something since it was loaded or saved. "
          + loadRule,
          "Use the menu's \"Revert\" to go back to the stored version." },
        { "preset.prev", "Previous preset",
          "Loads the previous preset.",
          "Steps backwards through the presets that match the browser's current filter (category, genres, "
          "search), or through all presets when nothing matches; wraps around. " + loadRule,
          "Filter the browser to one category first to audition just, say, guitar tones." },
        { "preset.next", "Next preset",
          "Loads the next preset.",
          "Steps forwards through the presets that match the browser's current filter, or all presets; "
          "wraps around. " + loadRule,
          {} },
        { "preset.fav", "Favourite",
          "Marks the current preset as a favourite (filled star) or removes it.",
          "Favourites appear under \"Favourites\" in the browser. Stored in Documents/Clod Scope/"
          "PresetSettings.xml, shared by every instance of the plugin.",
          {} },
        { "preset.save", "Save preset",
          "Saves the current sound as a user preset.",
          "Stores every sound control as a real value, both wave sculptor tables and the custom formula, "
          "plus name, category, genre tags and description, in Documents/Clod Scope/Presets/<name>.clodpreset.",
          "Saving under the name of an existing user preset asks before overwriting it." },
        { "preset.menu", "Preset menu",
          "Save As, Rename, Delete, Revert, Init and the presets folder.",
          "Save As: a new user preset. Rename / Delete: user presets only (Delete moves the file to the "
          "recycle bin). Revert: reload the current preset. Init: every sound control at its default "
          "(the input passes through unchanged).",
          "Preset files can be shared: copy them into the presets folder and choose Refresh." },
        { "browser.search", "Search",
          "Filters the list as you type.",
          "Every word must appear in the preset's name, description, category, genres or author "
          "(not case sensitive). Up / Down move the selection, Enter loads, Esc clears or closes.",
          "Try \"warm\", \"fuzz\", \"bell\" or \"lo-fi drums\"." },
        { "browser.categories", "Categories",
          "Shows one kind of sound: All, your Favourites, your User presets, or one instrument category.",
          "The number next to each entry is how many presets it holds. Next / previous in the top bar "
          "follow the chosen category.",
          {} },
        { "browser.genres", "Genre tags",
          "Narrows the list to presets tagged with any of the selected genres.",
          "Click a tag to select or deselect it (several can be on: a preset matches when it has any of "
          "them). \"Clear\" removes the genre filter.",
          {} },
        { "browser.list", "Preset list",
          "Click to select, double-click or Enter to load, click the star to mark a favourite.",
          "The highlighted bar marks the preset that is loaded now. " + loadRule,
          "Loading keeps the browser open so you can audition; switch on \"Close on load\" to close it automatically." },
    };
}
