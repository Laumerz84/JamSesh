#include "StandaloneBar.h"
#include "AppServices.h"
#include "../PluginProcessor.h"
#include "../UI/Theme.h"
#include "../Jam/JamSession.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace
{
    const char* const asioNotice = "ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries.";
    const char* const licenceNotice = "Copyright (C) 2026 Clod Codes. " CLOD_APP_NAME " and Clod Scope are free software: you may share "
                                      "and change them under the GNU General Public License version 3. They come with NO WARRANTY. "
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

    // Audio interface settings: JUCE's device selector inside a call-out in the editor
    class AudioSettingsView : public juce::Component
    {
    public:
        AudioSettingsView (juce::AudioDeviceManager& dm, std::function<void()> closed)
            : selector (dm, 0, 2, 0, 2, true, false, true, false), onClosed (std::move (closed))
        {
            addAndMakeVisible (selector);
            setSize (540, 440);
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
            g.setColour (Palette::dim);
            g.setFont (Theme::font (11.5f));
            g.drawFittedText ("Lowest delay: an ASIO driver and a buffer of 64-256 samples. Crackles? Raise the buffer.",
                              getLocalBounds().removeFromBottom (24).reduced (8, 0), juce::Justification::centredLeft, 1);
        }

        void resized() override
        {
            selector.setBounds (getLocalBounds().withTrimmedTop (28).withTrimmedBottom (24));
        }

    private:
        juce::AudioDeviceSelectorComponent selector;
        std::function<void()> onClosed;
    };

    class InfoView : public juce::Component
    {
    public:
        InfoView()
        {
            text << CLOD_APP_NAME " " << JucePlugin_VersionString << " - the Clod Scope app\n\n"
                 << "Audio: pick your audio interface; ASIO gives the lowest delay.\n"
                 << "Input: plays your guitar or mic through Clod Scope. Use headphones, or the speakers feed back.\n"
                 << "Tempo: type a BPM or tap it; the delays and one-bar mode follow it. In a Jam Sesh you follow the host.\n"
                 << "Track: load or drop a WAV, MP3, FLAC or OGG file to play along with; it streams to your friends.\n"
                 << "Record: saves what you hear to Documents\\Clod Scope\\Recordings.\n\n"
                 << licenceNotice << "\n\n"
                 << asioNotice;
            setSize (560, 290);
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
                             private juce::Value::Listener
{
    Impl (StandaloneBar& b, ClodScopeProcessor& p) : bar (b), proc (p), services (AppServices::instance())
    {
        for (auto* c : std::initializer_list<juce::Component*> { &audioBtn, &inputBtn, &bpmBox, &tapBtn, &loadBtn,
                                                                 &playBtn, &loopBtn, &volume, &recBtn, &infoBtn })
            bar.addAndMakeVisible (c);

        audioBtn.setTooltip ("Audio interface, inputs, outputs, sample rate and buffer size (ASIO for the lowest delay)");
        audioBtn.onClick = [this] { showAudioSettings(); };

        inputBtn.setTooltip ("Play your input (guitar, mic) through Clod Scope. Use headphones to avoid feedback.");
        inputBtn.onClick = [this] { toggleInput(); };

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

        recBtn.setTooltip ("Record what you hear (you, your friends and the track) to a WAV file in Documents\\Clod Scope\\Recordings");
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
        }
        else
        {
            for (auto* c : std::initializer_list<juce::Component*> { &audioBtn, &inputBtn, &loadBtn, &playBtn, &loopBtn, &volume, &recBtn })
                c->setEnabled (false);
        }

        applyColours();
        refresh();
        startTimerHz (10);
    }

    ~Impl() override
    {
        if (services != nullptr)
            services->inputMuted.removeListener (this);
    }

    // ---- actions ----
    void showAudioSettings()
    {
        if (services == nullptr || services->deviceManager == nullptr)
            return;
        auto save = services->saveAudioState;
        auto view = std::make_unique<AudioSettingsView> (*services->deviceManager, [save] { if (save) save(); });
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
        inputBtn.setButtonText (muted ? "Input off" : "Input on");
        inputBtn.setToggleState (! muted, juce::dontSendNotification);

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
        Theme::setAccent (inputBtn, Accent::input);
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
            g.setColour (message.startsWith ("Saved") ? Palette::green : Palette::pink);
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

    juce::TextButton audioBtn { "Audio" }, inputBtn { "Input on" }, tapBtn { "Tap" }, loadBtn { "Load" },
                     playBtn { "Play" }, loopBtn { "Loop" }, recBtn { "Record" }, infoBtn { "i" };
    juce::Label bpmBox;
    juce::Slider volume;
    std::unique_ptr<juce::FileChooser> chooser;
    TapTempo tapper;

    juce::Rectangle<int> bpmLabelArea, trackLabelArea, trackInfoArea, recTimeArea;
    juce::String message;
    juce::uint32 messageUntil = 0;
    bool followingHost = false, dragOver = false;
};

StandaloneBar::StandaloneBar (ClodScopeProcessor& p) : impl (std::make_unique<Impl> (*this, p)) {}
StandaloneBar::~StandaloneBar() = default;

bool StandaloneBar::isStandaloneApp()
{
    return juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_Standalone;
}

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
