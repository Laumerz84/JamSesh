#include "TrackPlayer.h"

// Threads: the message thread loads / plays / stops (under readerLock, shared with the reader thread
// only); the reader thread ("Clod Scope track") reads the file, resamples it to the device rate and
// pushes it into an SPSC FIFO; the audio thread pops. A flush (new file, rewind) is a handshake: the
// message thread bumps flushGen, the reader stops writing, the audio thread empties the FIFO and acks.
struct TrackPlayer::Impl : public juce::TimeSliceClient
{
    static constexpr int fifoSize = 1 << 15;     // ~0.7 s at 48 kHz
    static constexpr int srcSize = 1 << 14;      // file samples read ahead of the resampler
    static constexpr int maxChunk = 4096;

    Impl()
    {
        formats.registerBasicFormats();
        ring.clear();
        thread.addTimeSliceClient (this);
        thread.startThread();
    }

    ~Impl() override
    {
        thread.removeTimeSliceClient (this);
        thread.stopThread (2000);
    }

    // ---- reader thread ----
    int useTimeSlice() override
    {
        const juce::ScopedLock sl (readerLock);

        if (flushGen.load() != ackGen.load())
            return 2;                                  // waiting for the audio thread to empty the FIFO
        if (reader == nullptr || ! playing.load() || producerDone.load())
            return 20;

        const int space = fifo.getFreeSpace();
        if (space < 256)
            return 5;

        // top up the source buffer, wrapping at the end when looping
        if (srcStart > 0)
        {
            const int keep = srcEnd - srcStart;
            for (int ch = 0; ch < 2; ++ch)
                std::memmove (src.getWritePointer (ch), src.getReadPointer (ch, srcStart), sizeof (float) * (size_t) keep);
            srcStart = 0;
            srcEnd = keep;
        }

        while (srcEnd < srcSize && ! readerEnded)
        {
            const auto remaining = fileLength - readPos;
            if (remaining <= 0)
            {
                if (looping.load() && fileLength > 0)
                    readPos = 0;
                else
                    readerEnded = true;
                continue;
            }

            const int n = (int) juce::jmin ((juce::int64) (srcSize - srcEnd), remaining, (juce::int64) 8192);
            reader->read (&src, srcEnd, n, readPos, true, true);   // a mono file fills both channels
            readPos += n;
            srcEnd += n;
        }

        const double ratio = fileRate / juce::jmax (8000.0, outRate.load());
        const int avail = srcEnd - srcStart;
        const int canMake = (int) ((avail - 6) / ratio);

        if (canMake <= 0)
        {
            if (readerEnded)
                producerDone = true;                   // the audio thread stops when the FIFO runs dry
            updatePosition();
            return 20;
        }

        const int n = juce::jmin (space, canMake, maxChunk);
        int s1, z1, s2, z2;
        fifo.prepareToWrite (n, s1, z1, s2, z2);

        int used = 0;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* in = src.getReadPointer (ch, srcStart);
            int u = interp[ch].process (ratio, in, ring.getWritePointer (ch, s1), z1);
            if (z2 > 0)
                u += interp[ch].process (ratio, in + u, ring.getWritePointer (ch, s2), z2);
            used = u;                                  // both channels advance alike
        }

        fifo.finishedWrite (z1 + z2);
        srcStart += used;
        updatePosition();
        return fifo.getFreeSpace() >= 1024 ? 0 : 5;
    }

    void updatePosition()
    {
        if (fileRate > 0)
            headSeconds = (double) (readPos - (srcEnd - srcStart)) / fileRate;
    }

    // message thread, under readerLock: back to the start of the file, FIFO flushed
    void rewindLocked()
    {
        readPos = 0;
        srcStart = srcEnd = 0;
        readerEnded = false;
        producerDone = false;
        headSeconds = 0.0;
        for (auto& i : interp)
            i.reset();
        ++flushGen;
    }

    juce::TimeSliceThread thread { "Clod Scope track" };
    juce::AudioFormatManager formats;

    juce::CriticalSection readerLock;                 // message thread <-> reader thread, never the audio thread
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::File file;
    double fileRate = 44100.0;
    juce::int64 fileLength = 0, readPos = 0;
    juce::AudioBuffer<float> src { 2, srcSize };
    int srcStart = 0, srcEnd = 0;
    bool readerEnded = false;
    juce::LagrangeInterpolator interp[2];

    juce::AbstractFifo fifo { fifoSize };
    juce::AudioBuffer<float> ring { 2, fifoSize };

    std::atomic<bool> playing { false }, looping { true }, producerDone { false }, loaded { false };
    std::atomic<float> gain { 0.8f };
    std::atomic<double> outRate { 48000.0 }, headSeconds { 0.0 }, lengthSeconds { 0.0 };
    std::atomic<int> flushGen { 0 }, ackGen { 0 }, underruns { 0 };
    float lastGain = 0.8f;                             // audio thread
};

TrackPlayer::TrackPlayer() : impl (std::make_unique<Impl>()) {}
TrackPlayer::~TrackPlayer() = default;

void TrackPlayer::renderNextBlock (juce::AudioBuffer<float>& out, int numSamples) noexcept
{
    auto& d = *impl;
    const int numCh = juce::jmin (2, out.getNumChannels());
    numSamples = juce::jmin (numSamples, out.getNumSamples());
    if (numCh <= 0 || numSamples <= 0)
        return;

    const int gen = d.flushGen.load();
    const bool flushed = gen != d.ackGen.load();
    if (flushed)
    {
        d.fifo.finishedRead (d.fifo.getNumReady());
        d.ackGen = gen;
    }

    for (int ch = 0; ch < numCh; ++ch)
        out.clear (ch, 0, numSamples);

    const float g = d.gain.load();
    if (! d.playing.load())
    {
        d.lastGain = g;
        return;
    }

    const int ready = d.fifo.getNumReady();
    const int take = juce::jmin (ready, numSamples);
    int s1, z1, s2, z2;
    d.fifo.prepareToRead (take, s1, z1, s2, z2);

    auto copy = [&] (int ringStart, int count, int outStart)
    {
        if (count <= 0)
            return;
        if (numCh == 2)
        {
            out.copyFrom (0, outStart, d.ring, 0, ringStart, count);
            out.copyFrom (1, outStart, d.ring, 1, ringStart, count);
        }
        else
        {
            out.copyFrom (0, outStart, d.ring.getReadPointer (0, ringStart), count, 0.5f);
            out.addFrom (0, outStart, d.ring, 1, ringStart, count, 0.5f);
        }
    };
    copy (s1, z1, 0);
    copy (s2, z2, z1);
    d.fifo.finishedRead (z1 + z2);

    for (int ch = 0; ch < numCh; ++ch)
        out.applyGainRamp (ch, 0, numSamples, d.lastGain, g);
    d.lastGain = g;

    if (take < numSamples)
    {
        if (d.producerDone.load() && d.fifo.getNumReady() == 0)
            d.playing = false;                          // the end of a track that doesn't loop
        else if (! flushed)                             // right after a flush the reader starts filling
            ++d.underruns;
    }
}

bool TrackPlayer::isPlaying() const noexcept { return impl->playing.load(); }

bool TrackPlayer::load (const juce::File& f, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> r (impl->formats.createReaderFor (f));
    if (r == nullptr)
    {
        error = "Can't read " + f.getFileName() + " (WAV, AIFF, FLAC, OGG or MP3 only)";
        return false;
    }
    if (r->sampleRate <= 0 || r->lengthInSamples <= 0)
    {
        error = f.getFileName() + " is empty";
        return false;
    }

    impl->playing = false;
    {
        const juce::ScopedLock sl (impl->readerLock);
        std::swap (impl->reader, r);                // the old reader is deleted below, outside the lock
        impl->file = f;
        impl->fileRate = impl->reader->sampleRate;
        impl->fileLength = impl->reader->lengthInSamples;
        impl->lengthSeconds = (double) impl->fileLength / impl->fileRate;
        impl->rewindLocked();
    }
    impl->loaded = true;
    return true;
}

void TrackPlayer::play()
{
    if (! impl->loaded.load())
        return;
    if (impl->producerDone.load())                  // finished: start over
    {
        const juce::ScopedLock sl (impl->readerLock);
        impl->rewindLocked();
    }
    impl->playing = true;
    impl->thread.moveToFrontOfQueue (impl.get());
}

void TrackPlayer::stop()
{
    impl->playing = false;
    const juce::ScopedLock sl (impl->readerLock);
    impl->rewindLocked();
}

void TrackPlayer::setLooping (bool b)              { impl->looping = b; }
bool TrackPlayer::isLooping() const noexcept       { return impl->looping.load(); }
void TrackPlayer::setVolume (float g)              { impl->gain = juce::jlimit (0.0f, 2.0f, g); }
float TrackPlayer::getVolume() const noexcept      { return impl->gain.load(); }

void TrackPlayer::setSampleRate (double rate)
{
    if (rate > 0)
        impl->outRate = rate;
}

bool TrackPlayer::hasFile() const noexcept           { return impl->loaded.load(); }
double TrackPlayer::getLengthSeconds() const noexcept { return impl->lengthSeconds.load(); }
int TrackPlayer::getUnderruns() const noexcept       { return impl->underruns.load(); }

juce::File TrackPlayer::getFile() const
{
    const juce::ScopedLock sl (impl->readerLock);
    return impl->file;
}

double TrackPlayer::getBufferedSeconds() const noexcept
{
    return impl->fifo.getNumReady() / juce::jmax (8000.0, impl->outRate.load());
}

double TrackPlayer::getPositionSeconds() const noexcept
{
    const double len = impl->lengthSeconds.load();
    double pos = impl->headSeconds.load() - getBufferedSeconds();
    if (len > 0)
    {
        while (pos < 0.0)
            pos += len;
        pos = std::fmod (pos, len);
    }
    return juce::jmax (0.0, pos);
}

bool TrackPlayer::canLoad (const juce::File& f) const
{
    return impl->formats.findFormatForFileExtension (f.getFileExtension()) != nullptr;
}

juce::String TrackPlayer::wildcard() const
{
    return impl->formats.getWildcardForAllFormats();
}
