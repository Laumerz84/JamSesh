#pragma once

#include "../PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>

// Records what the standalone app plays (you, your friends and the backing track) to a WAV file in
// Documents\Jam Sesh\Recordings, written from a background thread. It is the processor's outputTap:
// the audio thread only pushes into AudioFormatWriter::ThreadedWriter's FIFO. Owner: standalone soldier.
class Recorder : public ClodScopeProcessor::AudioTap
{
public:
    Recorder();
    ~Recorder() override;

    // audio thread
    void tap (const juce::AudioBuffer<float>& block, int numChannels, int numSamples) noexcept override;

    // message thread
    bool start (const juce::File& wavFile, double sampleRate, juce::String& error);   // 24-bit stereo WAV
    void stop();                                       // flushes and closes the file
    bool isRecording() const noexcept;
    double getElapsedSeconds() const noexcept;
    juce::File getFile() const;                        // the current or last recording
    int getDroppedBlocks() const noexcept;             // the disk couldn't keep up

    static juce::File recordingsFolder();              // Documents\Jam Sesh\Recordings (AppPaths.h)
    static juce::File newRecordingFile();              // "Jam Sesh 2026-10-30 21-05-33.wav" in that folder

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Recorder)
};
