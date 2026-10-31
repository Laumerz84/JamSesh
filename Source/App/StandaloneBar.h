#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

class ClodScopeProcessor;

// A row under the top bar, shown only in the standalone app: audio interface settings, input mute, which
// of the interface's inputs to use (one in the middle, or two mixed or left and right), tempo (BPM box +
// tap), the backing-track file player and recording to WAV. Drop an audio file on
// it to load it as the backing track. Owner: standalone soldier.
class StandaloneBar : public juce::Component,
                      public juce::FileDragAndDropTarget
{
public:
    explicit StandaloneBar (ClodScopeProcessor&);
    ~StandaloneBar() override;

    static bool isStandaloneApp();           // true when running as the app (Jam Sesh.exe)
    static constexpr int idealHeight = 34;

    // The input picker behind the In button: the interface's input names, the chosen ones (channel
    // indices, -1 = none) and whether two are mixed in the middle; onChange gets the new choice.
    // Public so tools/snap can draw it with made-up input names.
    using InputChange = std::function<void (int first, int second, bool mixed)>;
    static std::unique_ptr<juce::Component> createInputPicker (const juce::StringArray& inputNames, int first, int second,
                                                               bool mixed, InputChange onChange);

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
