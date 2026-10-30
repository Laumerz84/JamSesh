#include "Recorder.h"

// The writer is swapped with an atomic pointer. stop() clears it, then waits for a tap that may still
// be using the old one (they last microseconds) before deleting it on the message thread.
struct Recorder::Impl
{
    Impl() { thread.startThread(); }
    ~Impl() { thread.stopThread (2000); }

    juce::TimeSliceThread thread { "Clod Scope recorder" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;   // message thread owns it
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> active { nullptr };
    std::atomic<int> inTap { 0 };
    std::atomic<juce::int64> samples { 0 };
    std::atomic<int> dropped { 0 };
    std::atomic<double> rate { 48000.0 };
    juce::File file;
};

Recorder::Recorder() : impl (std::make_unique<Impl>()) {}

Recorder::~Recorder()
{
    stop();
}

void Recorder::tap (const juce::AudioBuffer<float>& block, int numChannels, int numSamples) noexcept
{
    ++impl->inTap;
    if (auto* w = impl->active.load())
    {
        if (numChannels >= 1 && numSamples > 0)
        {
            const float* chans[2] = { block.getReadPointer (0), block.getReadPointer (numChannels >= 2 ? 1 : 0) };
            if (w->write (chans, numSamples))
                impl->samples += numSamples;
            else
                ++impl->dropped;
        }
    }
    --impl->inTap;
}

bool Recorder::start (const juce::File& f, double sampleRate, juce::String& error)
{
    stop();

    if (sampleRate <= 0)
        sampleRate = 48000.0;
    if (! f.getParentDirectory().createDirectory())
    {
        error = "Can't create " + f.getParentDirectory().getFullPathName();
        return false;
    }

    f.deleteFile();
    auto stream = f.createOutputStream();
    if (stream == nullptr)
    {
        error = "Can't write " + f.getFullPathName();
        return false;
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (stream.get(), sampleRate, 2, 24, {}, 0));
    if (w == nullptr)
    {
        error = "Can't create a WAV writer";
        return false;
    }
    stream.release();   // the writer owns it now

    impl->file = f;
    impl->rate = sampleRate;
    impl->samples = 0;
    impl->dropped = 0;
    // ~3 s of buffer: the disk may stall for a moment without losing audio
    impl->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (w.release(), impl->thread,
                                                                              (int) (sampleRate * 3.0));
    impl->active = impl->writer.get();
    return true;
}

void Recorder::stop()
{
    impl->active = nullptr;
    while (impl->inTap.load() != 0)
        juce::Thread::yield();
    impl->writer = nullptr;                // flushes what is left and closes the file
}

bool Recorder::isRecording() const noexcept      { return impl->active.load() != nullptr; }
double Recorder::getElapsedSeconds() const noexcept { return (double) impl->samples.load() / impl->rate.load(); }
juce::File Recorder::getFile() const               { return impl->file; }
int Recorder::getDroppedBlocks() const noexcept    { return impl->dropped.load(); }

juce::File Recorder::recordingsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("Clod Scope").getChildFile ("Recordings");
}

juce::File Recorder::newRecordingFile()
{
    const auto name = CLOD_APP_NAME " " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    return recordingsFolder().getNonexistentChildFile (name, ".wav", false);
}
