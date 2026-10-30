#pragma once

#include "TrackPlayer.h"
#include "Recorder.h"
#include <juce_data_structures/juce_data_structures.h>

namespace juce { class AudioDeviceManager; }

// The standalone app's own objects. StandaloneApp creates them before the processor and deletes them
// after it, so the processor's track source and output tap never dangle. nullptr outside Clod Scope.exe.
// The holder's parts are passed in here so that the shared plugin code never includes JUCE's
// standalone wrapper header. Owner: standalone soldier.
struct AppServices
{
    TrackPlayer player;
    Recorder recorder;

    juce::AudioDeviceManager* deviceManager = nullptr;   // StandalonePluginHolder's
    juce::Value inputMuted;                              // refers to the holder's "mute input" value
    juce::PropertySet* settings = nullptr;               // the app's settings file
    std::function<void()> saveAudioState;                // writes the device setup to the settings

    static AppServices*& instance() noexcept
    {
        static AppServices* current = nullptr;
        return current;
    }
};

// Tap tempo: the average of the last few intervals; a pause over 2 s starts a new count.
struct TapTempo
{
    // returns the tempo after this tap, or 0 when there aren't two taps yet
    double tap (double nowMs) noexcept
    {
        if (count > 0 && nowMs - times[(count - 1) % size] > 2000.0)
            count = 0;
        times[count % size] = nowMs;
        ++count;

        const int k = count < size ? count : size;
        if (k < 2)
            return 0.0;
        const double span = times[(count - 1) % size] - times[(count - k) % size];
        return span > 0.0 ? juce::jlimit (30.0, 300.0, 60000.0 * (k - 1) / span) : 0.0;
    }

    static constexpr int size = 5;
    double times[size] {};
    int count = 0;
};
