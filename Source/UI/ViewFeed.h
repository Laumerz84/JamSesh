#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include "../PluginProcessor.h"

// Where a view gets its sound and its settings. The 2D scope, the 3D views and the orbital view draw
// from a ViewFeed (sound) and ViewParams (settings), so the same code draws your own view and every
// friend's Jam Sesh tile. Owner: views soldier.

//==============================================================================
// A view's sound: this plugin's own scope buffers (ProcessorFeed) or a friend's sound arriving in a
// Jam Sesh (the stream soldier's feed).
struct ViewFeed
{
    virtual ~ViewFeed() = default;

    // Message thread. Copies the newest numSamples of the finished sound (output = true) or of the raw
    // input (output = false; a feed without the input copies the output), oldest first. Returns how
    // many it copied: when that is less than numSamples, the first `returned` entries hold the newest
    // `returned` samples (copyLatestPadded() moves them to the end and zeroes the rest).
    virtual int copyLatest (bool output, float* left, float* right, int numSamples) const = 0;

    virtual double sampleRate() const = 0;

    // The synth layer alone (the wave sculptor plays through it). 0 = this feed has none (friends).
    virtual int copyLatestSynth (float* /*left*/, float* /*right*/, int /*numSamples*/) const { return 0; }

    // copyLatest, but always fills all numSamples: the missing (older) samples are silence at the start
    int copyLatestPadded (bool output, float* left, float* right, int numSamples) const
    {
        const int got = juce::jlimit (0, numSamples, copyLatest (output, left, right, numSamples));
        if (got < numSamples)
        {
            const int gap = numSamples - got;
            std::move_backward (left, left + got, left + numSamples);
            std::move_backward (right, right + got, right + numSamples);
            std::fill (left, left + gap, 0.0f);
            std::fill (right, right + gap, 0.0f);
        }
        return got;
    }
};

// This plugin's own sound: an adapter over a processor's scope buffers (inputScope / outputScope /
// synthScope). Cheap: one per view, or one shared.
struct ProcessorFeed : public ViewFeed
{
    explicit ProcessorFeed (ClodScopeProcessor& p) : proc (p) {}

    int copyLatest (bool output, float* left, float* right, int numSamples) const override
    {
        numSamples = juce::jlimit (0, ScopeBuffer::size, numSamples);
        (output ? proc.outputScope : proc.inputScope).copyLatest (left, right, numSamples);
        return numSamples;
    }

    double sampleRate() const override { return proc.currentSampleRate.load(); }

    int copyLatestSynth (float* left, float* right, int numSamples) const override
    {
        numSamples = juce::jlimit (0, ScopeBuffer::size, numSamples);
        proc.synthScope.copyLatest (left, right, numSamples);
        return numSamples;
    }

    ClodScopeProcessor& proc;
};

//==============================================================================
// A view's settings: the real (not normalised) value of a parameter by id, plus the few text settings
// that live in the state tree (the 3D views' custom formulas "v3FormulaX/Y/Z").
// Local views read the APVTS (ApvtsParams); friend tiles read the ViewState the friend sent
// (StateParams in Jam/ViewState.h). Message thread.
struct ViewParams
{
    virtual ~ViewParams() = default;
    virtual float get (juce::StringRef id, float fallback) const = 0;
    virtual juce::String getText (juce::StringRef id, const juce::String& fallback) const = 0;
};

struct ApvtsParams : public ViewParams
{
    explicit ApvtsParams (juce::AudioProcessorValueTreeState& s) : apvts (s) {}

    float get (juce::StringRef id, float fallback) const override
    {
        if (auto* p = apvts.getRawParameterValue (id))
            return p->load();
        return fallback;
    }

    juce::String getText (juce::StringRef id, const juce::String& fallback) const override
    {
        return apvts.state.getProperty (juce::Identifier (juce::String (id.text)), fallback).toString();
    }

    juce::AudioProcessorValueTreeState& apvts;
};
