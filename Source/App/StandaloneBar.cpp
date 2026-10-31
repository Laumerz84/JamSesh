#include "StandaloneBar.h"
#include "AppServices.h"
#include "LowDelay.h"
#include "../PluginProcessor.h"
#include "../UI/Theme.h"
#include "../Jam/JamSession.h"
#include "../AppPaths.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace
{
    const char* const asioNotice = "ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries.";
    const char* const licenceNotice = "Copyright (C) 2026 Clod Codes. " CLOD_APP_NAME " is free software: you may share "
                                      "and change it under the GNU General Public License version 3. It comes with NO WARRANTY. "
                                      "The licence (LICENSE.txt) and the source code come with the download.";

    juce::String clock (double seconds)
    {
        const int s = (int) seconds;
        return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
    }

    // the editor hosts every pop-up (CallOutBox parented to it), so nothing opens a window of its own
    juce::Component* popupHost (juce::Component& c)
    {
        if (auto* ed = c.findParentComponentOfClass<juce::AudioProcessorEditor>())
            return ed;
        return c.getTopLevelComponent();
    }

    // Audio interface settings: JUCE's device selector inside a call-out in the editor, a "Lowest delay" button
    // (LowDelay.h: a real ASIO driver at 64 samples, else Windows Audio (Low Latency Mode) at its smallest
    // buffer) and a live readout of the driver's own latency figures, dropouts and load
    class AudioSettingsView : public juce::Component,
                              private juce::Timer
    {
    public:
        AudioSettingsView (AppServices& s, std::function<void()> closed)
            : services (s), dm (*s.deviceManager),
              selector (dm, 0, 2, 0, 2, true, false, false, false), onClosed (std::move (closed))   // inputs one by one
        {
            addAndMakeVisible (selector);
            lowestBtn.setTooltip ("Your interface's own ASIO driver at 64 samples; without one, Windows Audio (Low Latency Mode) "
                                  "at its smallest buffer. Never exclusive mode. Crackles? Raise the buffer a step.");
            lowestBtn.onClick = [this] { lowestDelay(); };
            Theme::setAccent (lowestBtn, Accent::jam);
            addAndMakeVisible (lowestBtn);
            setSize (560, 520);
            timerCallback();
            startTimerHz (4);
        }

        ~AudioSettingsView() override
        {
            if (onClosed)
                onClosed();
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Palette::text);
            g.setFont (Theme::font (14.0f, true));
            g.drawText ("Audio settings", getLocalBounds().removeFromTop (28).reduced (8, 0), juce::Justification::centredLeft);

            g.setFont (Theme::font (11.5f));
            g.setColour (Palette::text);
            g.drawFittedText (readoutText, readoutArea, juce::Justification::centredLeft, 2);
            g.setColour (result.isNotEmpty() ? (resultOk ? Palette::green : Palette::pink) : Palette::dim);
            g.drawFittedText (result.isNotEmpty() ? result
                                                  : juce::String ("The delay figures are the driver's own (input + output). No dropouts for a "
                                                                  "minute? Try the next smaller buffer. Crackles? Go one up."),
                              hintArea, juce::Justification::centredLeft, 2);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            r.removeFromTop (28);
            hintArea = r.removeFromBottom (34).reduced (8, 0);
            auto row = r.removeFromBottom (40).reduced (8, 4);
            lowestBtn.setBounds (row.removeFromLeft (104));
            row.removeFromLeft (10);
            readoutArea = row;
            selector.setBounds (r);
        }

    private:
        void timerCallback() override
        {
            auto* d = dm.getCurrentAudioDevice();
            const bool open = d != nullptr && d->isOpen();
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (open)
                dropouts.add (now, dm.getXRunCount());
            else
                dropouts.samples.clear();
            const auto text = open ? lowdelay::readout (d->getInputLatencyInSamples(), d->getOutputLatencyInSamples(),
                                                        d->getCurrentSampleRate(), dropouts.dropouts(), dropouts.seconds(),
                                                        dm.getCpuUsage())
                                   : lowdelay::readout (0, 0, 0.0, 0, 0.0, 0.0);
            if (text != readoutText)
            {
                readoutText = text;
                repaint (readoutArea);
            }
        }

        void lowestDelay()
        {
            juce::MouseCursor::showWaitCursor();
            const auto r = lowdelay::apply (dm, services.inputsNeeded, services.outputsNeeded, 64, true);
            juce::MouseCursor::hideWaitCursor();
            resultOk = r.changed;
            result = r.text.isNotEmpty() ? r.text : juce::String ("Nothing lower to pick here.");
            dropouts.samples.clear();
            if (services.saveAudioState)
                services.saveAudioState();
            timerCallback();
            repaint();
        }

        AppServices& services;
        juce::AudioDeviceManager& dm;
        juce::AudioDeviceSelectorComponent selector;
        juce::TextButton lowestBtn { "Lowest delay" };
        std::function<void()> onClosed;
        lowdelay::DropoutWindow dropouts;
        juce::String readoutText, result;
        bool resultOk = false;
        juce::Rectangle<int> readoutArea, hintArea;
    };

    // The input picker (the bar's In button): one or two of the interface's inputs, and how two reach the
    // sound. One input always sits in the middle.
    class InputView : public juce::Component
    {
    public:
        InputView (const juce::StringArray& names, int first, int second, bool mixed, StandaloneBar::InputChange change)
            : onChange (std::move (change)), empty (names.isEmpty())
        {
            secondBox.addItem ("None", 1);
            for (int i = 0; i < names.size(); ++i)
            {
                const auto item = juce::String (i + 1) + "   " + names[i];
                firstBox.addItem (item, i + 1);
                secondBox.addItem (item, i + 2);
            }
            modeBox.addItem ("Mixed in the middle (a mic and a guitar)", 1);
            modeBox.addItem ("Left and right (one stereo source)", 2);
            firstBox.setSelectedId (first >= 0 ? first + 1 : 0, juce::dontSendNotification);
            secondBox.setSelectedId (second >= 0 ? second + 2 : 1, juce::dontSendNotification);
            modeBox.setSelectedId (mixed ? 1 : 2, juce::dontSendNotification);
            modeBox.setEnabled (second >= 0);
            for (auto* box : { &firstBox, &secondBox, &modeBox })
            {
                box->onChange = [this] { changed(); };
                addChildComponent (box);
                box->setVisible (! empty);
            }
            setSize (440, 196);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().reduced (10, 0);
            g.setColour (Palette::text);
            g.setFont (Theme::font (14.0f, true));
            g.drawText ("Inputs", r.removeFromTop (30), juce::Justification::centredLeft);
            g.setColour (Palette::dim);
            g.setFont (Theme::font (11.5f));
            if (empty)
            {
                g.drawFittedText ("No audio interface is open. Pick one under Audio first.", r.removeFromTop (40),
                                  juce::Justification::topLeft, 2);
                return;
            }
            for (auto* label : { "Input", "Second input", "Two inputs" })
                g.drawText (label, r.removeFromTop (32).withWidth (100), juce::Justification::centredLeft);
            g.drawFittedText ("One instrument or mic? Pick only its input. A mic and a guitar? Pick both and mix them "
                              "in the middle. Changing inputs restarts the sound for a moment.",
                              r.reduced (0, 6), juce::Justification::topLeft, 3);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (10, 0).withTrimmedTop (30);
            for (auto* box : { &firstBox, &secondBox, &modeBox })
                box->setBounds (r.removeFromTop (32).withTrimmedLeft (104).reduced (0, 3));
        }

    private:
        void changed()
        {
            const int first = firstBox.getSelectedId() - 1;
            int second = secondBox.getSelectedId() - 2;
            if (second == first)
            {
                second = -1;
                secondBox.setSelectedId (1, juce::dontSendNotification);
            }
            modeBox.setEnabled (second >= 0);
            if (onChange && first >= 0)
                onChange (first, second, modeBox.getSelectedId() != 2);
        }

        juce::ComboBox firstBox, secondBox, modeBox;
        StandaloneBar::InputChange onChange;
        bool empty;
    };

    class InfoView : public juce::Component
    {
    public:
        InfoView()
        {
            text << CLOD_APP_NAME " " << JucePlugin_VersionString << " - play together online, live\n\n"
                 << "Audio: pick your audio interface; its own ASIO driver gives the lowest delay (Lowest delay picks it).\n"
                 << "  An amber Audio button: Windows Audio or DirectSound, about 30 ms more delay.\n"
                 << "Input: plays your guitar or mic through " CLOD_APP_NAME ". Use headphones, or the speakers feed back.\n"
                 << "In: which inputs of your interface; one input sits in the middle, two can be mixed (a mic and a guitar).\n"
                 << "Tempo: type a BPM or tap it; the delays and one-bar mode follow it. In a sesh you follow the host.\n"
                 << "Track: load or drop a WAV, MP3, FLAC or OGG file to play along with; it streams to your friends.\n"
                 << "Record: saves what you hear to " << apppaths::shown (Recorder::recordingsFolder()) << ".\n\n"
                 << licenceNotice << "\n\n"
                 << asioNotice;
            setSize (560, 310);
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Palette::text);
            g.setFont (Theme::font (12.5f));
            g.drawFittedText (text, getLocalBounds().reduced (10), juce::Justification::topLeft, 20);
        }

    private:
        juce::String text;
    };
}

struct StandaloneBar::Impl : private juce::Timer,
                             private juce::Value::Listener,
                             private juce::ChangeListener
{
    Impl (StandaloneBar& b, ClodScopeProcessor& p) : bar (b), proc (p), services (AppServices::instance())
    {
        for (auto* c : std::initializer_list<juce::Component*> { &audioBtn, &inputBtn, &inBtn, &bpmBox, &tapBtn, &loadBtn,
                                                                 &playBtn, &loopBtn, &volume, &recBtn, &infoBtn })
            bar.addAndMakeVisible (c);

        audioBtn.setTooltip (audioTip);
        audioBtn.onClick = [this] { showAudioSettings(); };

        inputBtn.setTooltip ("Play your input (guitar, mic) through " CLOD_APP_NAME ". Use headphones to avoid feedback.");
        inputBtn.onClick = [this] { toggleInput(); };

        inBtn.onClick = [this] { showInputPicker(); };

        bpmBox.setEditable (true, true, false);
        bpmBox.setJustificationType (juce::Justification::centred);
        bpmBox.setTooltip ("Tempo in BPM for the delays, the beat-synced effects and Jam Sesh. Click to type.");
        bpmBox.onTextChange = [this] { setBpm (bpmBox.getText().getDoubleValue()); };
        tapBtn.setTooltip ("Tap the tempo (4 taps or more)");
        tapBtn.onClick = [this]
        {
            const double bpm = tapper.tap (juce::Time::getMillisecondCounterHiRes());
            if (bpm > 0.0 && ! followingHost)
                setBpm (bpm);
        };

        loadBtn.setTooltip ("Load a backing track (WAV, MP3, FLAC, OGG). You can also drop a file on this bar.");
        loadBtn.onClick = [this] { chooseTrack(); };
        playBtn.setTooltip ("Play / stop the backing track (stop rewinds)");
        playBtn.onClick = [this]
        {
            if (services == nullptr)
                return;
            if (services->player.isPlaying())
                services->player.stop();
            else
                services->player.play();
            refresh();
        };
        loopBtn.setClickingTogglesState (true);
        loopBtn.setTooltip ("Loop the backing track");
        loopBtn.onClick = [this]
        {
            if (services != nullptr)
                services->player.setLooping (loopBtn.getToggleState());
            proc.apvts.state.setProperty ("appTrackLoop", loopBtn.getToggleState(), nullptr);
        };
        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volume.setRange (0.0, 1.5, 0.01);
        volume.setDoubleClickReturnValue (true, 0.8);
        volume.setTooltip ("Backing track volume");
        volume.onValueChange = [this]
        {
            if (services != nullptr)
                services->player.setVolume ((float) volume.getValue());
            proc.apvts.state.setProperty ("appTrackVol", volume.getValue(), nullptr);
        };

        recBtn.setTooltip ("Record what you hear (you, your friends and the track) to a WAV file in "
                           + apppaths::shown (Recorder::recordingsFolder()));
        recBtn.onClick = [this] { toggleRecord(); };
        infoBtn.setTooltip ("About " CLOD_APP_NAME);
        infoBtn.onClick = [this]
        {
            auto* host = popupHost (bar);
            juce::CallOutBox::launchAsynchronously (std::make_unique<InfoView>(), host->getLocalArea (&infoBtn, infoBtn.getLocalBounds()), host);
        };

        // restore this app's last tempo and track (stored with the plugin state)
        const auto& st = proc.apvts.state;
        const double bpm = (double) st.getProperty ("appBpm", 0.0);
        if (bpm >= 30.0 && bpm <= 300.0)
            proc.fallbackBpm = bpm;
        loopBtn.setToggleState ((bool) st.getProperty ("appTrackLoop", true), juce::dontSendNotification);
        volume.setValue ((double) st.getProperty ("appTrackVol", 0.8), juce::dontSendNotification);

        if (services != nullptr)
        {
            services->player.setLooping (loopBtn.getToggleState());
            services->player.setVolume ((float) volume.getValue());
            services->player.setSampleRate (proc.getSampleRate());
            const juce::File last (st.getProperty ("appTrack").toString());
            juce::String err;
            if (! services->player.hasFile() && last.existsAsFile())
                services->player.load (last, err);
            services->inputMuted.addListener (this);
            if (services->deviceManager != nullptr)
                services->deviceManager->addChangeListener (this);
            applyRouting();
            if (services->startupNote.isNotEmpty())   // e.g. the first run's device pick
            {
                message = services->startupNote;
                messageUntil = juce::Time::getMillisecondCounter() + 12000;
                services->startupNote.clear();
            }
        }
        else
        {
            for (auto* c : std::initializer_list<juce::Component*> { &audioBtn, &inputBtn, &inBtn, &loadBtn, &playBtn, &loopBtn, &volume, &recBtn })
                c->setEnabled (false);
        }

        applyColours();
        refresh();
        startTimerHz (10);
    }

    ~Impl() override
    {
        if (services != nullptr)
        {
            services->inputMuted.removeListener (this);
            if (services->deviceManager != nullptr)
                services->deviceManager->removeChangeListener (this);
        }
    }

    // ---- actions ----
    void showAudioSettings()
    {
        if (services == nullptr || services->deviceManager == nullptr)
            return;
        auto save = services->saveAudioState;
        auto view = std::make_unique<AudioSettingsView> (*services, [save] { if (save) save(); });
        auto* host = popupHost (bar);
        juce::CallOutBox::launchAsynchronously (std::move (view), host->getLocalArea (&audioBtn, audioBtn.getLocalBounds()), host);
    }

    void toggleInput()
    {
        if (services == nullptr)
            return;
        services->inputMuted = ! (bool) services->inputMuted.getValue();
        if (services->settings != nullptr)
            services->settings->setValue ("clodInputChoice", true);   // from now on the app keeps the user's choice
        if (services->saveAudioState)
            services->saveAudioState();
        refresh();
    }

    // ---- inputs: which of the interface's inputs, and how they reach the sound ----
    juce::AudioIODevice* device() const
    {
        return services != nullptr && services->deviceManager != nullptr ? services->deviceManager->getCurrentAudioDevice() : nullptr;
    }

    std::pair<int, int> activeInputs() const   // channel indices, lowest first; -1 = none
    {
        if (auto* d = device())
        {
            const auto bits = d->getActiveInputChannels();
            const int first = bits.findNextSetBit (0);
            return { first, first >= 0 ? bits.findNextSetBit (first + 1) : -1 };
        }
        return { -1, -1 };
    }

    bool inputsMixed() const   // two inputs: mixed in the middle (default) or left and right
    {
        return services == nullptr || services->settings == nullptr || services->settings->getBoolValue ("clodInputMix", true);
    }

    // one input in the middle; two mixed in the middle or left and right
    void applyRouting()
    {
        const auto [first, second] = activeInputs();
        proc.inputRouting = second >= 0 ? (inputsMixed() ? ClodScopeProcessor::inputMixCentred : ClodScopeProcessor::inputStereo)
                          : first >= 0  ? ClodScopeProcessor::inputFirstCentred
                                        : ClodScopeProcessor::inputStereo;
    }

    void showInputPicker()
    {
        auto* d = device();
        const auto [first, second] = activeInputs();
        auto view = StandaloneBar::createInputPicker (d != nullptr ? d->getInputChannelNames() : juce::StringArray(), first, second,
                                                      inputsMixed(), [this] (int a, int b, bool mixed) { setInputs (a, b, mixed); });
        auto* host = popupHost (bar);
        juce::CallOutBox::launchAsynchronously (std::move (view), host->getLocalArea (&inBtn, inBtn.getLocalBounds()), host);
    }

    void setInputs (int first, int second, bool mixed)
    {
        if (services == nullptr || services->deviceManager == nullptr)
            return;
        if (services->settings != nullptr)
            services->settings->setValue ("clodInputMix", mixed);
        auto& dm = *services->deviceManager;
        auto setup = dm.getAudioDeviceSetup();
        juce::BigInteger channels;
        channels.setBit (first);
        if (second >= 0)
            channels.setBit (second);
        if (setup.useDefaultInputChannels || setup.inputChannels != channels)
        {
            setup.useDefaultInputChannels = false;
            setup.inputChannels = channels;
            const auto err = dm.setAudioDeviceSetup (setup, true);
            if (err.isNotEmpty())
            {
                message = err;
                messageUntil = juce::Time::getMillisecondCounter() + 6000;
            }
        }
        applyRouting();
        if (services->saveAudioState)
            services->saveAudioState();
        refresh();
    }

    // the device or its inputs changed (also from the Audio settings)
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        applyRouting();
        refresh();
    }

    void setBpm (double bpm)
    {
        if (bpm >= 30.0 && bpm <= 300.0)
        {
            proc.fallbackBpm = bpm;
            proc.apvts.state.setProperty ("appBpm", bpm, nullptr);
        }
        refresh();
    }

    void chooseTrack()
    {
        if (services == nullptr)
            return;
        auto start = services->player.getFile();
        if (! start.existsAsFile())
            start = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        chooser = std::make_unique<juce::FileChooser> ("Load a backing track", start, services->player.wildcard());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult().existsAsFile())
                                      loadTrack (fc.getResult());
                              });
    }

    void loadTrack (const juce::File& f)
    {
        if (services == nullptr)
            return;
        juce::String err;
        if (services->player.load (f, err))
        {
            message.clear();
            proc.apvts.state.setProperty ("appTrack", f.getFullPathName(), nullptr);
        }
        else
        {
            message = err;
            messageUntil = juce::Time::getMillisecondCounter() + 5000;
        }
        refresh();
    }

    void toggleRecord()
    {
        if (services == nullptr)
            return;
        auto& rec = services->recorder;
        if (rec.isRecording())
        {
            rec.stop();
            message = "Saved " + rec.getFile().getFileName();
            messageUntil = juce::Time::getMillisecondCounter() + 6000;
        }
        else
        {
            juce::String err;
            if (! rec.start (Recorder::newRecordingFile(), proc.getSampleRate(), err))
            {
                message = err;
                messageUntil = juce::Time::getMillisecondCounter() + 6000;
            }
        }
        refresh();
    }

    // ---- state -> controls ----
    void refresh()
    {
        const bool muted = services == nullptr || (bool) services->inputMuted.getValue();
        // amber while the device type adds a lot of delay (Windows Audio shared mode, DirectSound)
        const auto warning = services != nullptr ? lowdelay::warningFor (services->currentDeviceType()) : juce::String();
        if (warning != audioWarning)
        {
            audioWarning = warning;
            audioBtn.setToggleState (warning.isNotEmpty(), juce::dontSendNotification);
            audioBtn.setTooltip (warning.isNotEmpty() ? warning : juce::String (audioTip));
        }

        inputBtn.setButtonText (muted ? "Input off" : "Input on");
        inputBtn.setToggleState (! muted, juce::dontSendNotification);

        const auto [first, second] = activeInputs();
        const auto names = device() != nullptr ? device()->getInputChannelNames() : juce::StringArray();
        auto inputName = [&names] (int i) { return juce::String (i + 1) + (names[i].isNotEmpty() ? " (" + names[i] + ")" : juce::String()); };
        inBtn.setButtonText (first < 0   ? juce::String ("In -")
                             : second < 0 ? "In " + juce::String (first + 1)
                                          : "In " + juce::String (first + 1) + (inputsMixed() ? "+" : "/") + juce::String (second + 1));
        inBtn.setTooltip (first < 0   ? juce::String ("Which inputs of your audio interface to use")
                          : second < 0 ? "Input " + inputName (first) + ", in the middle. Click to choose other inputs."
                                       : "Inputs " + inputName (first) + " and " + inputName (second)
                                         + (inputsMixed() ? ", mixed in the middle" : ", left and right") + ". Click to choose.");

        if (! bpmBox.isBeingEdited())
            bpmBox.setText (juce::String (proc.fallbackBpm.load(), 1), juce::dontSendNotification);
        bpmBox.setEditable (! followingHost, ! followingHost, false);
        tapBtn.setEnabled (! followingHost);

        if (services != nullptr)
        {
            const bool playing = services->player.isPlaying();
            playBtn.setButtonText (playing ? "Stop" : "Play");
            playBtn.setToggleState (playing, juce::dontSendNotification);
            playBtn.setEnabled (services->player.hasFile());
            const bool rec = services->recorder.isRecording();
            recBtn.setButtonText (rec ? "Stop rec" : "Record");
            recBtn.setToggleState (rec, juce::dontSendNotification);
        }
        bar.repaint();
    }

    void timerCallback() override
    {
        if (services != nullptr)
            services->player.setSampleRate (proc.getSampleRate());

        // in a Jam Sesh a friend follows the host's tempo
        bool follow = false;
        if (auto* port = proc.getJamPort())
        {
            auto& s = *port->session();
            if (s.status() == jam::Status::connected && ! s.isHost())
            {
                const double bpm = s.sessionBpm();
                if (bpm >= 30.0 && bpm <= 300.0)
                {
                    follow = true;
                    proc.fallbackBpm = bpm;
                }
            }
        }
        followingHost = follow;
        bpmBox.setTooltip (follow ? "Following the Jam Sesh host's tempo"
                                  : "Tempo in BPM for the delays, the beat-synced effects and Jam Sesh. Click to type.");

        if (message.isNotEmpty() && juce::Time::getMillisecondCounter() > messageUntil)
            message.clear();
        refresh();
    }

    void valueChanged (juce::Value&) override { refresh(); }

    void applyColours()
    {
        Theme::setAccent (audioBtn, Palette::amber);   // lit only while warning
        Theme::setAccent (inputBtn, Accent::input);
        Theme::setAccent (inBtn, Accent::input);
        Theme::setAccent (playBtn, Accent::jam);
        Theme::setAccent (loopBtn, Accent::jam);
        Theme::setAccent (recBtn, Palette::pink);
        bpmBox.setColour (juce::Label::textColourId, Palette::text);
        bpmBox.setColour (juce::Label::backgroundColourId, Palette::field);
        bpmBox.setColour (juce::Label::outlineColourId, Palette::panelEdge);
        bpmBox.setColour (juce::Label::textWhenEditingColourId, Palette::bright);
        bpmBox.setColour (juce::TextEditor::textColourId, Palette::bright);
        bpmBox.setColour (juce::TextEditor::backgroundColourId, Palette::field);
        bpmBox.setColour (juce::TextEditor::highlightColourId, Accent::jam.withAlpha (0.4f));
        bpmBox.setFont (Theme::font (13.0f, true));
    }

    void layout()
    {
        auto r = bar.getLocalBounds().reduced (8, 4);
        auto take = [&r] (int w) { auto a = r.removeFromLeft (w); r.removeFromLeft (4); return a; };
        audioBtn.setBounds (take (60));
        inputBtn.setBounds (take (76));
        inBtn.setBounds (take (62));
        r.removeFromLeft (12);
        bpmLabelArea = take (30);
        bpmBox.setBounds (take (52));
        tapBtn.setBounds (take (40));
        r.removeFromLeft (12);
        trackLabelArea = take (40);
        loadBtn.setBounds (take (50));
        playBtn.setBounds (take (50));
        loopBtn.setBounds (take (48));
        volume.setBounds (take (84).reduced (0, 3));

        infoBtn.setBounds (r.removeFromRight (26));
        r.removeFromRight (8);
        recTimeArea = r.removeFromRight (56);
        r.removeFromRight (4);
        recBtn.setBounds (r.removeFromRight (70));
        r.removeFromRight (12);
        trackInfoArea = r;
    }

    void paint (juce::Graphics& g)
    {
        g.setColour (Palette::panel);
        g.fillRect (bar.getLocalBounds());
        g.setColour (Palette::panelEdge);
        g.drawHorizontalLine (bar.getHeight() - 1, 0.0f, (float) bar.getWidth());

        if (dragOver)
        {
            g.setColour (Accent::jam.withAlpha (0.18f));
            g.fillRect (bar.getLocalBounds());
            g.setColour (Accent::jam);
            g.drawRect (bar.getLocalBounds(), 2);
        }

        g.setFont (Theme::font (11.5f, true));
        g.setColour (Palette::dim);
        g.drawText ("BPM", bpmLabelArea, juce::Justification::centredRight);
        g.drawText ("Track", trackLabelArea, juce::Justification::centredRight);

        // file name and position, or a message
        g.setFont (Theme::font (12.0f));
        if (message.isNotEmpty())
        {
            g.setColour (message.startsWith ("Saved") || message.startsWith ("Audio:") ? Palette::green : Palette::pink);
            g.drawFittedText (message, trackInfoArea, juce::Justification::centredLeft, 1);
        }
        else if (services != nullptr && services->player.hasFile())
        {
            auto& p = services->player;
            const auto pos = clock (p.getPositionSeconds()) + " / " + clock (p.getLengthSeconds());
            auto a = trackInfoArea;
            g.setColour (Palette::dim);
            g.drawText (pos, a.removeFromRight (80), juce::Justification::centredRight);
            g.setColour (p.isPlaying() ? Palette::text : Palette::dim);
            g.drawFittedText (p.getFile().getFileNameWithoutExtension(), a, juce::Justification::centredLeft, 1);
        }
        else
        {
            g.setColour (Palette::dim.withAlpha (0.7f));
            g.drawFittedText ("Drop an audio file here to play along", trackInfoArea, juce::Justification::centredLeft, 1);
        }

        if (services != nullptr && services->recorder.isRecording())
        {
            g.setColour (Palette::pink);
            g.fillEllipse (recTimeArea.withSizeKeepingCentre (8, 8).withX (recTimeArea.getX()).toFloat());
            g.setColour (Palette::text);
            g.drawText (clock (services->recorder.getElapsedSeconds()), recTimeArea.withTrimmedLeft (12),
                        juce::Justification::centredLeft);
        }
    }

    StandaloneBar& bar;
    ClodScopeProcessor& proc;
    AppServices* services;

    juce::TextButton audioBtn { "Audio" }, inputBtn { "Input on" }, inBtn { "In" }, tapBtn { "Tap" }, loadBtn { "Load" },
                     playBtn { "Play" }, loopBtn { "Loop" }, recBtn { "Record" }, infoBtn { "i" };
    juce::Label bpmBox;
    juce::Slider volume;
    std::unique_ptr<juce::FileChooser> chooser;
    TapTempo tapper;

    juce::Rectangle<int> bpmLabelArea, trackLabelArea, trackInfoArea, recTimeArea;
    juce::String message, audioWarning;
    juce::uint32 messageUntil = 0;
    static constexpr const char* audioTip = "Audio interface, inputs, outputs, sample rate and buffer size (ASIO for the lowest delay)";
    bool followingHost = false, dragOver = false;
};

StandaloneBar::StandaloneBar (ClodScopeProcessor& p) : impl (std::make_unique<Impl> (*this, p)) {}

std::unique_ptr<juce::Component> StandaloneBar::createInputPicker (const juce::StringArray& inputNames, int first, int second,
                                                                   bool mixed, InputChange onChange)
{
    return std::make_unique<InputView> (inputNames, first, second, mixed, std::move (onChange));
}
StandaloneBar::~StandaloneBar() = default;

bool StandaloneBar::isStandaloneApp()
{
    return juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_Standalone;
}

void StandaloneBar::refreshNow()               { impl->refresh(); }
void StandaloneBar::paint (juce::Graphics& g)  { impl->paint (g); }
void StandaloneBar::resized()                  { impl->layout(); }

void StandaloneBar::lookAndFeelChanged()
{
    impl->applyColours();
    repaint();
}

bool StandaloneBar::isInterestedInFileDrag (const juce::StringArray& files)
{
    auto* s = AppServices::instance();
    return s != nullptr && files.size() == 1 && s->player.canLoad (juce::File (files[0]));
}

void StandaloneBar::fileDragEnter (const juce::StringArray&, int, int) { impl->dragOver = true; repaint(); }
void StandaloneBar::fileDragExit (const juce::StringArray&)            { impl->dragOver = false; repaint(); }

void StandaloneBar::filesDropped (const juce::StringArray& files, int, int)
{
    impl->dragOver = false;
    if (! files.isEmpty())
        impl->loadTrack (juce::File (files[0]));
}
