#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../UI/Params.h"
#include "../UI/Help.h"
#include "../UI/Widgets.h"

// Professional effects rack, processed in this fixed order:
//   Amp -> Cab -> EQ -> Comp -> Mod -> Delay -> Reverb -> Limiter
// Each module has its own on/off parameter and its own control panel.
//
// Owner: Agent "FX". The public API below is a CONTRACT (the processor and the editor code against
// it). Private state lives in Impl. Parameter ids start with "fx"; module switches are
// "fxAmpOn", "fxCabOn", "fxEqOn", "fxCompOn", "fxModOn", "fxDelayOn", "fxRevOn", "fxLimOn"
// (all default off, so the plugin passes audio unchanged until a preset or the user turns one on).

class FxRack
{
public:
    enum Module { amp, cab, eq, comp, mod, delay, reverb, limiter, numModules };

    static void addParameters (ParamList&);
    static HelpList helpEntries();
    static juce::String moduleName (Module);       // "Amp", "Cab", "EQ", "Comp", "Mod", "Delay", "Reverb", "Limiter"
    static juce::String moduleEnableId (Module);   // e.g. "fxAmpOn"
    static juce::String moduleSummary (Module);    // one line, for the chain strip tooltip / Inspector

    explicit FxRack (juce::AudioProcessorValueTreeState&);
    ~FxRack();

    // Plugin (default): the latency is fixed after prepare() - the amp's oversampling and the limiter's
    // look-ahead stay in the path even when those modules are off - so a host's delay compensation never
    // changes. App (live = true, set before prepare()): no host compensates anything, so only the modules
    // that are on add their delay; switching crossfades between the two timings, so it never clicks.
    void setLiveLatency (bool live) noexcept;
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    int latencySamples() const noexcept;           // plugin: fixed after prepare(). app: what is in the path now (audio thread)
    int maxLatencySamples() const noexcept;        // the plugin's fixed value = the app's worst case

    // Audio thread: delays the dry path of the global mix by latencySamples(), so it lines up with the rack's
    // output. When that changes (app) it crossfades from the old delay to the new one over 10 ms.
    void alignDry (juce::AudioBuffer<float>& dry, int numChannels, int numSamples) noexcept;

    struct Transport
    {
        double bpm = 120.0;
        bool playing = false;
        double ppqPosition = 0.0;
    };

    // Audio thread, RT-safe. Modules that are off cost (almost) nothing.
    void process (juce::AudioBuffer<float>&, int numChannels, const Transport&) noexcept;

    // Same, but runLimiter = false stops before the limiter, so another stage (the double slit) can
    // run between the reverb and the limiter; then call processLimiter(). latencySamples() always
    // includes the limiter's look-ahead.
    void process (juce::AudioBuffer<float>&, int numChannels, const Transport&, bool runLimiter) noexcept;
    void processLimiter (juce::AudioBuffer<float>&, int numChannels) noexcept;   // honours fxLimOn

    float meterDb (Module) const noexcept;         // gain reduction (comp, limiter) or output level, for panels
    juce::String describeMapping() const;          // live plain-words explanation for the Inspector

    // One control panel per module (title() == moduleName). Created and owned by the editor.
    std::unique_ptr<ControlPanel> createPanel (Module, HelpTarget*);

private:
    struct Impl;
    struct DryAlign;
    std::unique_ptr<Impl> impl;
    std::unique_ptr<DryAlign> dryAlign;
};
