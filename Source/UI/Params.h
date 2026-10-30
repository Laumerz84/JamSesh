#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>

// Each module adds its own parameters to the processor's layout through a static
// addParameters (ParamList&) so modules never edit each other's files.
using ParamList = std::vector<std::unique_ptr<juce::RangedAudioParameter>>;
