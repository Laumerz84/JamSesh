#pragma once

#include "../Jam/JamTypes.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>

// The standalone app's backing track: an audio file (WAV, MP3, FLAC, OGG) to play along with, looped
// or not. It is a jam::TrackSource, so in a Jam Sesh it streams to friends like the FL Master would.
// A background thread reads and resamples the file into a lock-free FIFO; the audio thread only copies.
// Owner: standalone soldier.
class TrackPlayer : public jam::TrackSource
{
public:
    TrackPlayer();
    ~TrackPlayer() override;

    // Audio thread: OVERWRITES the first numSamples of channels 0..1 of out with the track at the
    // device rate, volume applied; zeros when stopped (a 1-channel out gets the L+R mix).
    void renderNextBlock (juce::AudioBuffer<float>& out, int numSamples) noexcept override;
    bool isPlaying() const noexcept override;

    // ---- message thread ----
    bool load (const juce::File&, juce::String& error);   // stops; keeps the loop and volume settings
    void play();
    void stop();                                          // stops and rewinds
    void setLooping (bool);
    bool isLooping() const noexcept;
    void setVolume (float gain);                          // linear, 0..2
    float getVolume() const noexcept;
    void setSampleRate (double deviceRate);               // the rate renderNextBlock plays at

    bool hasFile() const noexcept;
    juce::File getFile() const;
    double getLengthSeconds() const noexcept;
    double getPositionSeconds() const noexcept;           // approximate, for the display
    double getBufferedSeconds() const noexcept;           // read ahead, waiting in the FIFO
    int getUnderruns() const noexcept;                    // blocks the reader didn't fill in time

    bool canLoad (const juce::File&) const;               // by extension
    juce::String wildcard() const;                        // "*.wav;*.mp3;..." for the file chooser

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackPlayer)
};
