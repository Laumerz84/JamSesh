#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <deque>

// Picking the audio device for the lowest delay (the standalone app only). Used twice:
//   - the first run (no saved device setup): a real ASIO driver at 128 samples, else Windows Audio (Low Latency
//     Mode) at its smallest buffer, else JUCE's default (Windows Audio shared). Saved setups are never touched.
//   - the Audio settings' "Lowest delay" button: the same rules, aiming for 64 samples on ASIO.
// On macOS there is only CoreAudio, which has no shared-mode penalty: the devices open now (else the system's
// defaults) at 128 samples on the first run, and 64 for "Lowest delay". warningFor() never warns about it.
// Never exclusive mode (it silences every other app on that device). Generic ASIO wrappers (FL Studio ASIO,
// ASIO4ALL, Generic Low Latency, Realtek ASIO, ...) are skipped: Low Latency Mode is usually as good and keeps
// other apps' sound. The choice itself is a pure function (candidates / choose / pickBuffer), checked by the
// app's --selftest with made-up device lists; apply() is the part that opens devices.
// Owner: app soldier.
namespace lowdelay
{
    inline const char* const asioType = "ASIO";
    inline const char* const lowLatencyType = "Windows Audio (Low Latency Mode)";
    inline const char* const sharedType = "Windows Audio";
    inline const char* const directSoundType = "DirectSound";
    inline const char* const coreAudioType = "CoreAudio";   // macOS

    // one device type and its devices, as AudioIODeviceType lists them
    struct TypeInfo
    {
        juce::String type;
        juce::StringArray outputs, inputs;
        int defaultOutput = 0, defaultInput = 0;
    };

    // what is open now (empty = nothing)
    struct Current
    {
        juce::String type, output, input;
    };

    struct Choice
    {
        enum class Kind { keep, asio, lowLatencyMode, coreAudio };
        Kind kind = Kind::keep;
        juce::String type, output, input;
        int bufferTarget = 0;   // the buffer to aim for (pickBuffer); 0 = the smallest available
    };

    // ASIO drivers that wrap Windows audio or another app instead of driving an interface
    inline bool isWrapperAsio (const juce::String& driverName)
    {
        const auto n = driverName.toLowerCase();
        for (auto* w : { "fl studio asio", "asio4all", "generic low latency", "realtek", "built-in asio", "asio2wasapi",
                         "flexasio", "voicemeeter", "asio link", "rearoute", "magix low latency", "directx",
                         "asio multimedia", "jackrouter" })
            if (n.contains (w))
                return true;
        return false;
    }

    // In order of preference: a real ASIO driver (the one open now if it is one, else the first in the list),
    // then Low Latency Mode on the devices open now (or Windows' defaults). Empty: nothing better than today.
    // macOS: CoreAudio on the devices open now (or the system's defaults) at asioBuffer.
    inline juce::Array<Choice> candidates (const juce::Array<TypeInfo>& types, const Current& current, int asioBuffer)
    {
        auto byDefault = [] (const juce::StringArray& names, int index)
        {
            return names.isEmpty() ? juce::String() : names[juce::jlimit (0, names.size() - 1, index)];
        };
        juce::Array<Choice> out;
        for (auto& t : types)
        {
            if (t.type != coreAudioType || t.outputs.isEmpty())
                continue;
            const bool inUse = current.type == coreAudioType;
            const auto output = inUse && t.outputs.contains (current.output) ? current.output : byDefault (t.outputs, t.defaultOutput);
            const auto input = inUse && (current.input.isEmpty() || t.inputs.contains (current.input)) ? current.input
                                                                                                     : byDefault (t.inputs, t.defaultInput);
            out.add ({ Choice::Kind::coreAudio, t.type, output, input, asioBuffer });
        }
        for (auto& t : types)
        {
            if (t.type != asioType)
                continue;
            juce::String pick;
            if (current.type == asioType && t.outputs.contains (current.output) && ! isWrapperAsio (current.output))
                pick = current.output;
            else
                for (auto& n : t.outputs)
                    if (! isWrapperAsio (n))
                    {
                        pick = n;
                        break;
                    }
            if (pick.isNotEmpty())
                out.add ({ Choice::Kind::asio, t.type, pick, t.inputs.contains (pick) ? pick : juce::String(), asioBuffer });
        }
        for (auto& t : types)
        {
            if (t.type != lowLatencyType || t.outputs.isEmpty())
                continue;
            const auto output = t.outputs.contains (current.output) ? current.output : byDefault (t.outputs, t.defaultOutput);
            const auto input = t.inputs.contains (current.input) ? current.input : byDefault (t.inputs, t.defaultInput);
            out.add ({ Choice::Kind::lowLatencyMode, t.type, output, input, 0 });
        }
        return out;
    }

    inline Choice choose (const juce::Array<TypeInfo>& types, const Current& current, int asioBuffer)
    {
        const auto c = candidates (types, current, asioBuffer);
        return c.isEmpty() ? Choice() : c.getFirst();
    }

    // target > 0: that size if the device offers it, else the next larger, else the largest.
    // target 0: the smallest. Returns 0 when the device lists nothing (keep its default).
    inline int pickBuffer (const juce::Array<int>& available, int target)
    {
        if (available.isEmpty())
            return 0;
        int smallest = available.getFirst(), largest = smallest, above = 0;
        for (auto b : available)
        {
            smallest = juce::jmin (smallest, b);
            largest = juce::jmax (largest, b);
            if (b >= target && (above == 0 || b < above))
                above = b;
        }
        if (target <= 0)
            return smallest;
        return above > 0 ? above : largest;
    }

    // The bar's amber Audio button: the device types that add a lot of delay. Empty = fine.
    inline juce::String warningFor (const juce::String& deviceType)
    {
        if (deviceType == sharedType)
            return "Windows Audio (shared mode) adds about 30 ms of delay. Click Audio, then Lowest delay - or pick your "
                   "interface's ASIO driver, or Windows Audio (Low Latency Mode) at its smallest buffer.";
        if (deviceType == directSoundType)
            return "DirectSound adds a lot of delay (30 ms or more). Click Audio, then Lowest delay - or pick your "
                   "interface's ASIO driver, or Windows Audio (Low Latency Mode) at its smallest buffer.";
        return {};
    }

    // The Audio settings' readout line (the driver's own figures)
    inline juce::String readout (int inSamples, int outSamples, double sampleRate, int dropouts, double windowSeconds, double cpu)
    {
        if (sampleRate <= 0.0)
            return "No audio device is open.";
        auto ms = [sampleRate] (int samples) { return juce::String (1000.0 * juce::jmax (0, samples) / sampleRate, 1); };
        const auto card = inSamples + outSamples > 0
                            ? "Sound card: in " + ms (inSamples) + " + out " + ms (outSamples) + " = "
                                + ms (juce::jmax (0, inSamples) + juce::jmax (0, outSamples)) + " ms round trip"
                            : juce::String ("Sound card: the driver reports no delay figures");
        return card + "   |   dropouts: " + juce::String (dropouts) + " in the last " + juce::String (juce::roundToInt (windowSeconds))
               + " s   |   load " + juce::String (juce::roundToInt (juce::jlimit (0.0, 1.0, cpu) * 100.0)) + " %";
    }

    // Dropouts in the last minute from AudioDeviceManager::getXRunCount() (a running count that restarts with the device)
    struct DropoutWindow
    {
        void add (double nowMs, int count)
        {
            if (! samples.empty() && count < samples.back().second)
                samples.clear();   // the device restarted
            samples.push_back ({ nowMs, count });
            while (samples.size() > 1 && nowMs - samples.front().first > 60000.0)
                samples.pop_front();
        }
        int dropouts() const      { return samples.empty() ? 0 : samples.back().second - samples.front().second; }
        double seconds() const    { return samples.empty() ? 0.0 : (samples.back().first - samples.front().first) / 1000.0; }

        std::deque<std::pair<double, int>> samples;
    };

    //==============================================================================
    // The parts that touch the device manager (message thread)
    inline juce::Array<TypeInfo> scan (juce::AudioDeviceManager& dm)
    {
        juce::Array<TypeInfo> types;
        for (auto* t : dm.getAvailableDeviceTypes())   // scans once (ASIO: the registry; Windows: the endpoint list)
        {
            TypeInfo i;
            i.type = t->getTypeName();
            i.outputs = t->getDeviceNames (false);
            i.inputs = t->getDeviceNames (true);
            i.defaultOutput = t->getDefaultDeviceIndex (false);
            i.defaultInput = t->getDefaultDeviceIndex (true);
            types.add (i);
        }
        return types;
    }

    inline juce::String describe (juce::AudioDeviceManager& dm)
    {
        auto* d = dm.getCurrentAudioDevice();
        if (d == nullptr)
            return "no audio device is open";
        return dm.getCurrentAudioDeviceType() + ": " + d->getName() + ", " + juce::String (d->getCurrentBufferSizeSamples())
               + " samples at " + juce::String (d->getCurrentSampleRate() / 1000.0, 1) + " kHz";
    }

    // the open device's buffer -> pickBuffer (target); back to its default if that doesn't open
    inline void setBuffer (juce::AudioDeviceManager& dm, int target)
    {
        auto* d = dm.getCurrentAudioDevice();
        if (d == nullptr)
            return;
        const int b = pickBuffer (d->getAvailableBufferSizes(), target);
        if (b <= 0 || b == d->getCurrentBufferSizeSamples())
            return;
        auto s = dm.getAudioDeviceSetup();
        s.bufferSize = b;
        if (dm.setAudioDeviceSetup (s, true).isNotEmpty() || dm.getCurrentAudioDevice() == nullptr)
        {
            s.bufferSize = 0;
            dm.setAudioDeviceSetup (s, true);
        }
    }

    struct Result
    {
        bool changed = false;     // a candidate opened (or the buffer was lowered)
        juce::String text;        // for the user
    };

    // Tries the candidates in order; if none opens, goes back to the device that was open before.
    // inputs/outputs = the channel counts the holder initialised the manager with.
    // lowerBufferAnyway: with no better device type, still aim the open device at asioBuffer ("Lowest delay").
    inline Result apply (juce::AudioDeviceManager& dm, int inputs, int outputs, int asioBuffer, bool lowerBufferAnyway)
    {
        const auto prevType = dm.getCurrentAudioDeviceType();
        const auto prevSetup = dm.getAudioDeviceSetup();
        auto prevXml = dm.createStateXml();
        if (prevXml == nullptr)
        {
            prevXml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
            prevXml->setAttribute ("deviceType", prevType);
            prevXml->setAttribute ("audioOutputDeviceName", prevSetup.outputDeviceName);
            prevXml->setAttribute ("audioInputDeviceName", prevSetup.inputDeviceName);
        }

        const auto cands = candidates (scan (dm), { prevType, prevSetup.outputDeviceName, prevSetup.inputDeviceName }, asioBuffer);
        juce::StringArray failed;
        bool touched = false;
        for (auto& c : cands)
        {
            const bool same = ! touched && c.type == prevType && c.output == prevSetup.outputDeviceName
                              && c.input == prevSetup.inputDeviceName && dm.getCurrentAudioDevice() != nullptr;
            if (! same)
            {
                auto xml = std::make_unique<juce::XmlElement> (*prevXml);   // keeps the MIDI inputs
                xml->setAttribute ("deviceType", c.type);
                xml->setAttribute ("audioOutputDeviceName", c.output);
                xml->setAttribute ("audioInputDeviceName", c.input);
                for (auto* a : { "audioDeviceName", "audioDeviceInChans", "audioDeviceOutChans", "audioDeviceBufferSize" })
                    xml->removeAttribute (a);
                xml->setAttribute ("audioDeviceRate", 48000.0);   // the session's rate; the device's own if it can't
                dm.closeAudioDevice();   // a new device object even when the name is the same in another type
                if (! touched && c.type != prevType)
                    juce::Thread::sleep (500);   // as JUCE does when switching types (DirectSound/ASIO clashes)
                touched = true;
                const auto err = dm.initialise (inputs, outputs, xml.get(), false);
                if (err.isNotEmpty() || dm.getCurrentAudioDevice() == nullptr)
                {
                    failed.add (c.output + " didn't open" + (c.kind == Choice::Kind::asio ? " (is another app, e.g. FL Studio, using it?)" : ""));
                    continue;
                }
            }
            setBuffer (dm, c.kind == Choice::Kind::lowLatencyMode ? 0 : c.bufferTarget);   // ASIO, CoreAudio: the target
            auto text = "Now " + describe (dm) + ".";
            if (! failed.isEmpty())
                text << " " << failed.joinIntoString ("; ") << ".";
            return { true, text };
        }

        if (touched)   // nothing opened: back to what was open
        {
            dm.closeAudioDevice();
            dm.initialise (inputs, outputs, prevXml.get(), true);
            return { false, failed.joinIntoString ("; ") + ". Kept " + describe (dm) + "." };
        }
        if (lowerBufferAnyway && dm.getCurrentAudioDevice() != nullptr)
        {
            setBuffer (dm, asioBuffer);
            return { true, "No ASIO driver or Low Latency Mode found. Now " + describe (dm) + "." };
        }
        return { false, {} };
    }

    // Publishes the open device's own latency figures into the processor (samples at the device rate, 0 = no
    // device) whenever the device opens or changes, and once a second in case a driver reset changed them.
    struct LatencyPublisher : private juce::ChangeListener, private juce::Timer
    {
        LatencyPublisher (juce::AudioDeviceManager& d, std::atomic<int>& in, std::atomic<int>& out)
            : dm (d), inLatency (in), outLatency (out)
        {
            dm.addChangeListener (this);
            publish();
            startTimer (1000);
        }
        ~LatencyPublisher() override
        {
            stopTimer();
            dm.removeChangeListener (this);
        }

        void publish()
        {
            auto* d = dm.getCurrentAudioDevice();
            const bool open = d != nullptr && d->isOpen();
            inLatency = open ? juce::jmax (0, d->getInputLatencyInSamples()) : 0;
            outLatency = open ? juce::jmax (0, d->getOutputLatencyInSamples()) : 0;
        }

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override { publish(); }
        void timerCallback() override                                  { publish(); }

        juce::AudioDeviceManager& dm;
        std::atomic<int>& inLatency;
        std::atomic<int>& outLatency;
    };
}
