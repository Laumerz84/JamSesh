// ClodLatencyProbe: opens an audio interface with every driver type the app offers (ASIO, Windows Audio
// shared / exclusive / low latency, DirectSound) and prints, for a few buffer sizes, the latency each
// driver reports and how steadily its audio callbacks arrive. Headless: no window, outputs silence.
//   ClodLatencyProbe.exe [device name part, default "Focusrite"] [seconds per test, default 2]
// The interface is taken over for a moment per test, so other sound on it briefly stops.
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
    // Records when every callback arrives; writes silence
    struct Recorder : juce::AudioIODeviceCallback
    {
        std::vector<juce::int64> ticks = std::vector<juce::int64> (100000);
        std::atomic<int> count { 0 };

        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            const int n = count.load (std::memory_order_relaxed);
            if (n < (int) ticks.size())
            {
                ticks[(size_t) n] = juce::Time::getHighResolutionTicks();
                count.store (n + 1, std::memory_order_relaxed);
            }
            for (int c = 0; c < numOut; ++c)
                if (out[c] != nullptr)
                    juce::FloatVectorOperations::clear (out[c], numSamples);
        }
        void audioDeviceAboutToStart (juce::AudioIODevice*) override { count = 0; }
        void audioDeviceStopped() override {}
    };

    juce::String pick (const juce::StringArray& names, const juce::String& part)
    {
        for (auto& n : names)
            if (n.containsIgnoreCase (part))
                return n;
        return {};
    }

    juce::String ms (double samples, double rate) { return juce::String (1000.0 * samples / rate, 2) + " ms"; }

    void test (juce::AudioIODeviceType& type, const juce::String& outName, const juce::String& inName, int buffer, double seconds)
    {
        std::unique_ptr<juce::AudioIODevice> dev (type.createDevice (outName, inName));
        if (dev == nullptr)
        {
            std::cout << "    could not create the device\n";
            return;
        }
        const auto rates = dev->getAvailableSampleRates();
        const double rate = rates.contains (48000.0) ? 48000.0 : dev->getCurrentSampleRate() > 0 ? dev->getCurrentSampleRate() : rates[0];
        juce::BigInteger ins, outs;
        ins.setRange (0, juce::jmin (2, dev->getInputChannelNames().size()), true);
        outs.setRange (0, juce::jmin (2, dev->getOutputChannelNames().size()), true);

        const auto err = dev->open (ins, outs, rate, buffer);
        if (err.isNotEmpty())
        {
            std::cout << "    buffer " << buffer << ": open failed: " << err << "\n";
            return;
        }
        Recorder rec;
        dev->start (&rec);
        juce::MessageManager::getInstance()->runDispatchLoopUntil ((int) (seconds * 1000.0));
        dev->stop();

        const double sr = dev->getCurrentSampleRate();
        const int bs = dev->getCurrentBufferSizeSamples();
        const int inLat = dev->getInputLatencyInSamples();
        const int outLat = dev->getOutputLatencyInSamples();
        const int xruns = dev->getXRunCount();
        dev->close();

        // callback spacing: how steady the driver is (a jam's safety buffer has to cover the worst gap)
        const int n = rec.count.load();
        double mean = 0, worst = 0, var = 0;
        std::vector<double> gaps;
        for (int i = 1; i < n; ++i)
            gaps.push_back (1000.0 * juce::Time::highResolutionTicksToSeconds (rec.ticks[(size_t) i] - rec.ticks[(size_t) i - 1]));
        for (auto g : gaps) { mean += g; worst = juce::jmax (worst, g); }
        if (! gaps.empty()) mean /= (double) gaps.size();
        for (auto g : gaps) var += (g - mean) * (g - mean);
        const double jitter = gaps.size() > 1 ? std::sqrt (var / (double) gaps.size()) : 0.0;

        std::cout << "    buffer " << bs << " @ " << (int) sr << " Hz (" << ms (bs, sr) << ")"
                  << ": input " << ms (inLat, sr) << ", output " << ms (outLat, sr)
                  << ", in+out " << ms (inLat + outLat, sr)
                  << " | " << n << " callbacks, mean gap " << juce::String (mean, 2) << " ms, worst " << juce::String (worst, 2)
                  << " ms, jitter " << juce::String (jitter, 3) << " ms, xruns " << (xruns < 0 ? juce::String ("n/a") : juce::String (xruns))
                  << "\n";
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juce;
    const juce::String part = argc > 1 ? juce::String (argv[1]) : juce::String ("Focusrite");
    const double seconds = argc > 2 ? juce::jlimit (0.5, 20.0, juce::String (argv[2]).getDoubleValue()) : 2.0;

    juce::AudioDeviceManager manager;
    for (auto* type : manager.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        const auto outs = type->getDeviceNames (false);
        const auto ins = type->getDeviceNames (true);
        const auto outName = pick (outs, part);
        const auto inName = pick (ins, part);
        std::cout << "\n== " << type->getTypeName() << "\n";
        if (outName.isEmpty() && inName.isEmpty())
        {
            std::cout << "    no device matching \"" << part << "\" (outputs: " << outs.joinIntoString (", ") << ")\n";
            continue;
        }
        std::cout << "    out: " << (outName.isEmpty() ? juce::String ("-") : outName)
                  << " | in: " << (inName.isEmpty() ? juce::String ("-") : inName) << "\n";

        std::unique_ptr<juce::AudioIODevice> probe (type->createDevice (outName, inName));
        if (probe == nullptr)
        {
            std::cout << "    could not create the device\n";
            continue;
        }
        const auto sizes = probe->getAvailableBufferSizes();
        std::cout << "    buffer sizes: " << [&] { juce::StringArray s; for (int b : sizes) s.add (juce::String (b)); return s.joinIntoString (" "); }()
                  << " (default " << probe->getDefaultBufferSize() << ")\n";
        probe.reset();

        juce::Array<int> wanted;
        if (! sizes.isEmpty())
            wanted.addIfNotAlreadyThere (sizes[0]);
        for (int b : { 32, 64, 128, 256 })
            if (sizes.contains (b))
                wanted.addIfNotAlreadyThere (b);
        wanted.sort();
        for (int b : wanted)
            test (*type, outName, inName, b, seconds);
    }
    std::cout << "\ndone\n";
    return 0;
}
