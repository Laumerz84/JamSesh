#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../Theme.h"
#include "Inspector.h"

// In-window overlay with short explanatory pages. Lines starting with "## " are headings and
// lines containing '=' are shown in monospace (see RichTextView::mixed).
class ConceptGuide : public juce::Component
{
public:
    std::function<void()> onClose;

    ConceptGuide()
    {
        for (int i = 0; i < (int) pages().size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (pages()[(size_t) i].first));
            b->setClickingTogglesState (true);
            b->setRadioGroupId (4711);
            b->onClick = [this, i] { showPage (i); };
            addAndMakeVisible (b);
        }
        closeButton.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeButton);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (8);
        addAndMakeVisible (viewport);
        setWantsKeyboardFocus (true);
        showPage (0);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey && onClose)
        {
            onClose();
            return true;
        }
        return false;
    }

    void showPage (int i)
    {
        page = juce::jlimit (0, (int) pages().size() - 1, i);
        buttons[page]->setToggleState (true, juce::dontSendNotification);
        const auto& p = pages()[(size_t) page];
        std::vector<RichTextView::Block> blocks;
        RichTextView::Block title;
        title.text.append (p.first, Theme::font (18.0f, true), Palette::text);
        blocks.push_back (title);
        // paragraphs are separated by blank lines; formula-only paragraphs get a box
        for (auto& para : juce::StringArray::fromTokens (p.second, "\f", ""))
            blocks.push_back (RichTextView::mixed (para.trim(), Palette::text, para.startsWith ("  ")));
        content.setBlocks (std::move (blocks));
        viewport.setViewPosition (0, 0);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        g.setColour (Palette::panel);
        g.fillRoundedRectangle (card.toFloat(), 10.0f);
        g.setColour (Palette::panelEdge);
        g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 10.0f, 1.0f);
        g.setColour (Palette::green);
        g.setFont (Theme::font (13.0f, true));
        g.drawText ("CONCEPT GUIDE", card.reduced (18, 14).removeFromTop (20), juce::Justification::centredLeft);
        g.setColour (Palette::panelEdge);
        g.drawVerticalLine (card.getX() + 210, (float) card.getY() + 50.0f, (float) card.getBottom() - 16.0f);
    }

    void resized() override
    {
        card = getLocalBounds().reduced (juce::jmax (30, getWidth() / 10), juce::jmax (24, getHeight() / 12));
        auto inner = card.reduced (18, 14);
        auto top = inner.removeFromTop (24);
        closeButton.setBounds (top.removeFromRight (80));
        inner.removeFromTop (12);
        auto list = inner.removeFromLeft (180);
        inner.removeFromLeft (30);
        const int step = juce::jlimit (22, 36, list.getHeight() / juce::jmax (1, buttons.size()));
        for (auto* b : buttons)
        {
            b->setBounds (list.removeFromTop (step - 5));
            list.removeFromTop (5);
        }
        viewport.setBounds (inner);
        content.setSize (inner.getWidth() - 12, content.getHeight());
        showPage (page);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! card.contains (e.getPosition()) && onClose)
            onClose();
    }

private:
    using Page = std::pair<juce::String, juce::String>;

    // "\f" separates paragraphs; a paragraph starting with two spaces is drawn boxed
    static const std::vector<Page>& pages()
    {
        static const std::vector<Page> p {
            { "Getting around",
              "Clod Scope is a sound kit: any input can be shaped, turned into a synth, sculpted, sent through a "
              "full effects rack - and watched on a scope, in 3D or as an atom.\f"
              "## The chain strip (the main navigation)\nThe row of chips under the top bar is the signal chain, left to "
              "right in processing order. Click a chip to open that module's controls in the right column. The LED on a "
              "chip switches the module on or off; a dim chip is off and passes audio unchanged. The small line under "
              "each name shows its live state (level, tracked note, gain reduction...). Use < and > in the module header "
              "to step through the chain.\f"
              "## Main view\nThe buttons at the top pick the big display: 2D Scope, 3D Views, Orbital or Wave Sculptor. "
              "The VIEW chip at the end of the strip opens the controls of the current view (picture only - they never "
              "change the sound, except the Wave Sculptor, which edits the synth's wave).\f"
              "## Inspector\nHover any knob, menu, switch or chip and the Inspector (bottom right) explains what it does "
              "and exactly what it maps to what. When nothing is hovered it describes the selected module live. Drag the "
              "dotted handle above it to resize it; double-click the handle to collapse it.\f"
              "## Dials\nEvery dial catches on its default and on 0 as you drag past (the small dots around the dial mark "
              "these points; the dot lights up while the dial sits on one). Double-click a dial to reset it; typed values "
              "are never snapped. Each module's dials are drawn in its colour.\f"
              "## Presets\nThe preset bar at the top loads, saves and steps through sounds; Browse opens the full library. "
              "Esc or a click outside closes any overlay." },

            { "Signal chain",
              "Audio flows through the plugin in this order (the same order as the chain strip):\f"
              "  INPUT -> SYNTH (+ SCULPT) -> EQUATION -> AMP -> CAB -> EQ -> COMP -> MOD -> DELAY -> REVERB\n"
              "  -> SLIT -> LIMITER -> OUTPUT (dry/wet + output gain)\f"
              "## Colours\nGreen = input, blue = synth, violet = wave sculptor, pink = physics (equation, double slit, "
              "orbital), amber = the FX rack, cyan = display-only controls.\f"
              "Everything starts neutral or off, so the plugin passes audio unchanged until you turn something on or load "
              "a preset. The dry signal used by Dry/Wet is the raw input, delayed to line up with the processed sound.\f"
              "## What the views show\n'Input' traces are taken before input gain (the raw signal); 'Output' traces after "
              "output gain. 'Show' on the 2D Scope display panel picks which one the scope and spectrum use.\f"
              "## External input\nIn FL Studio set a mixer insert's input to your audio interface and put Clod Scope on that "
              "insert." },

            { "Synth layer",
              "The SYNTH module turns ANY input into a synth. A tracker follows the pitch and loudness of the incoming "
              "audio (voice, guitar, bass, a sample...) and a voice plays the Wave Sculptor's wave at that pitch, "
              "following the input's envelope.\f"
              "## Layer modes\nThe synth can be blended with the input, add only new overtones on top of it, or replace "
              "it entirely - from subtle thickening to a full synth that you play with your voice.\f"
              "## MIDI\nNotes sent to the plugin play the same engine polyphonically, so Clod Scope also works as an "
              "instrument (route a MIDI track or the piano roll to it in FL Studio).\f"
              "## Tracked note\nThe top bar shows the note the tracker hears right now (and how many cents off it is); "
              "the SYNTH chip shows it too. Tracking works best on one note at a time - for chords, play the synth from MIDI." },

            { "Wave Sculptor",
              "The Wave Sculptor is the hands-on editor for the wave the synth plays. Pick it as the main view (or click "
              "the SCULPT chip) and edit the sound with the mouse.\f"
              "## Drag the wave\nDraw directly on the single-cycle wave; its overtones update to match.\f"
              "## Overtone bars\nEach bar is one harmonic (1 = the fundamental, 2 = an octave up, 3 = an octave and a "
              "fifth...). Drag a bar to set how loud that overtone is.\f"
              "## Superposition\nThe wave is literally the sum of its overtones - the same superposition idea as the "
              "quantum views. The macro dials (tilt, odd/even, formant, stretch...) reshape all overtones at once.\f"
              "## Morph\nThere are two waves, A and B. Morph blends from A to B, so one dial (or an automation lane) "
              "sweeps between two timbres. Undo and redo cover every edit.\f"
              "The sculpted wave is heard while the SYNTH module is on." },

            { "FX rack",
              "Eight studio effects in a fixed order: AMP (drive and tone) -> CAB (speaker cabinet) -> EQ -> COMP "
              "-> MOD (chorus, flanger, phaser...) -> DELAY -> REVERB -> LIMITER.\f"
              "Each effect has its own chip in the strip (under the FX RACK bracket) with an on/off LED, its own panel and "
              "amber dials. Effects that are off cost no CPU.\f"
              "## Tips\nStart from a preset and switch effects off one by one to hear what each adds. The LIMITER sits "
              "at the end (in the MASTER group) and keeps the output safe; the COMP and LIMITER chips show their gain reduction." },

            { "Presets & browser",
              "The preset bar at the top shows the current preset; its arrows step to the previous / next one, and an "
              "edited preset is marked as modified.\f"
              "## Browser\nBrowse opens the library over the window: filter by category (instrument / sound type) and "
              "genre, search by name, mark favourites and save your own. Esc or a click on the dimmed backdrop closes it.\f"
              "## What a preset stores\nOnly sound-shaping settings (input, synth, sculpt, equation, FX, double slit, mix "
              "and output). Display settings and the main view are never changed by loading a preset." },

            { "2D scope",
              "Time runs left to right, amplitude up and down. Time window sets how much time fits across (10 divisions); "
              "Zoom scales it vertically.\f"
              "## Trigger\nA repeating wave only stands still if every frame starts at the same point in its cycle. The "
              "trigger finds the newest place where the wave crosses the trigger level (rising or falling) and lines the "
              "picture up there. Drag on the scope to move the level. These controls open with the VIEW chip.\f"
              "## Persistence, freeze, XY\nPersistence leaves a fading trail like a phosphor tube. Freeze holds the current "
              "audio so you can zoom and inspect. XY plots left against right: mono is a diagonal line, stereo width "
              "spreads it into shapes (Lissajous figures)." },

            { "3D views",
              "The 3D Views draw the same audio in up to three dimensions. Drag to rotate, scroll to zoom, double-click to "
              "reset; auto-spin turns it slowly.\f"
              "## Helix (analytic signal)\nThe wave and a 90-degree shifted copy of itself (its Hilbert transform) are used as "
              "two axes, with time as the third. A pure sine becomes a perfect corkscrew; its radius is the loudness envelope "
              "and its twist rate is the frequency.\f"
              "## Phase space\nThe signal plotted against delayed copies (or derivatives) of itself. Periodic sounds trace "
              "closed loops and knots; noise becomes a cloud; chaotic sounds show attractor-like shapes.\f"
              "## Stereo 3D\nOscilloscope-music style: left = X, right = Y, and time (or mid/side) as depth.\f"
              "## Waterfall\nThe spectrum over time as a landscape: frequency x time x loudness.\f"
              "The 3D mapping dials and x/y/z formulas only change the picture, never the sound." },

            { "Sound -> atom bridge",
              "An electron bound in an atom is a standing wave in 3D. Each allowed shape (orbital) is labelled n, l, m and "
              "has its own energy. The Orbital view turns the sound into a mix of these shapes and draws the probability "
              "cloud (|psi|^2).\f"
              "## The sound is the electron\nNo sound, no electron: in silence nothing is drawn. The louder the sound, the "
              "more present the electron (it appears within ~12 ms and fades out over the Response time). Its frequencies "
              "decide WHICH orbitals it occupies.\f"
              "## How sound becomes a mix of orbitals\nThe audio is split into its frequencies. Each strong frequency becomes "
              "one term of the mix: its relative loudness sets how much of that orbital is present, and its frequency picks "
              "the energy level (via Level mapping). Phases run continuously so shapes never flicker.\f"
              "## Potential\nHarmonic oscillator: levels are evenly spaced, E = (N + 3/2) h-bar omega, exactly like a note's "
              "harmonic series - the most musical choice. Hydrogen: the textbook atom, E_n = -R / n^2, levels bunch together "
              "going up. Atom (element Z): the sound is the outer electron of a real element. The other electrons fill the "
              "inner subshells, so the electron climbs that element's own ladder (sodium: 3s, 3p, 4s, 3d, 4p...), and each "
              "rung has a fixed shape - s spheres, p dumbbells, d clovers, f flowers. Screening below 100% strips the core "
              "away to build custom, hydrogen-like ions.\f"
              "## h-bar (time scale)\nIn quantum mechanics h-bar converts frequency into energy and sets how fast a mix "
              "changes. A single orbital never moves (its cloud is stationary); a mix of two pulses at their energy "
              "difference. Lower the time scale to slow audio-rate beating down to something you can watch.\f"
              "## Quantize\n0 = tones between levels blend the neighbouring orbitals (shown as slow drift); 1 = each tone "
              "snaps to the nearest level.\f"
              "## Angular source (the missing dimension)\nA tone only gives an energy (n), but each energy level holds many "
              "shapes (l, m). This picks what chooses them: your Dials, the Stereo image (L/R phase -> orientation m, "
              "width -> shape l), the Timbre (brightness -> l, harmonic number -> m), or Spread (shared evenly, round shells)." },

            { "Level mappings",
              "Music cares about RATIOS between frequencies (intervals); quantum mechanics runs on DIFFERENCES between "
              "energies (beats). No single map keeps both for hydrogen - so you choose.\f"
              "## Physical (linear)\n  E = a x f + b\fKeeps differences, so beats in the cloud match beats you hear. Hydrogen "
              "levels are then not musical; the oscillator's are (its levels are already evenly spaced).\f"
              "## Rydberg -> harmonic\n  n = sqrt(R / |E|)   so   f = f0 x n\fHydrogen levels land exactly on your note's "
              "harmonic series (fundamental, octave, octave + fifth...). The cloud still moves with true hydrogen energies, "
              "so the visual pulse rate will not equal the audible beating.\f"
              "## Emission lines (most physical)\n  f = R x (1/n1^2 - 1/n2^2)\fAtoms emit light at the DIFFERENCE between two "
              "levels, and a mix of those two levels makes the cloud slosh at exactly that frequency. Here each tone picks "
              "the transition that matches it and the view shows that two-level mix sloshing at the tone's own frequency "
              "(slowed by h-bar). Line ratios are fractions, so they form real intervals: the first two Lyman lines are "
              "(8/9) / (3/4) = 32/27, a Pythagorean minor third.\f"
              "## Equal temperament\n  f = f0 x 2^(level / 12)\fOne level per semitone - for playing scales against the "
              "atom. Keeps only the order of the levels." },

            { "Double slit",
              "Imagine the sound coming through a wall with two small openings, heard by an enormous ear far away.\f"
              "Every point of the ear hears every point of both slits, each copy delayed by its extra distance / 343 m/s "
              "(the shortest path arrives instantly). Copies arriving in step add up (bright fringes); out of step they "
              "cancel (dark fringes). All those delayed copies together act like a filter + echo pattern on the sound.\f"
              "Small setups sound like a metallic comb filter; a long screen distance and many fringes give a smeared, "
              "coloured tail. The graphic under the 2D scope shows the waves, the slits, the ear and the delay spread." },

            { "Equations",
              "The EQUATION module reshapes the actual sound with a formula, y = f(...), evaluated for every sample. Pick a "
              "preset family and tune it with dials A-D, or type your own formula (it turns red with the error position if "
              "it does not parse).\f"
              "## Writing formulas\nOperators: + - * / ^ %, comparisons (< > <= >= == !=), && || !, a ? b : c.\n"
              "Functions: sin cos tan asin acos atan atan2 sinh cosh tanh exp log log2 log10 sqrt abs sign floor ceil "
              "round fract min max clamp pow mod lerp step smoothstep sat tri saw sqr hypot. Constants: pi tau e.\f"
              "The EQUATION panel lists the variables available (such as the current sample and time) and what each preset "
              "computes. The 3D Views display panel has its own picture-only x/y/z formulas." },
        };
        return p;
    }

    juce::OwnedArray<juce::TextButton> buttons;
    juce::TextButton closeButton { "Close" };
    juce::Viewport viewport;
    RichTextView content;
    juce::Rectangle<int> card;
    int page = 0;
};
