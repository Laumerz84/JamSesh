#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

class ClodScopeProcessor;

// A row under the top bar, shown only in the standalone app: audio interface settings, input mute,
// tempo (BPM box + tap), the backing-track file player and recording to WAV. Drop an audio file on
// it to load it as the backing track. Owner: standalone soldier.
class StandaloneBar : public juce::Component,
                      public juce::FileDragAndDropTarget
{
public:
    explicit StandaloneBar (ClodScopeProcessor&);
    ~StandaloneBar() override;

    static bool isStandaloneApp();           // true when running as the app (Jam Sesh.exe)
    static constexpr int idealHeight = 34;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StandaloneBar)
};
