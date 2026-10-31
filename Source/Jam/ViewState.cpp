#include "ViewState.h"
#include "../Sculpt/SculptModel.h"

// Owner: views soldier.
namespace
{
    // Every setting the four views read. APPEND ONLY: the index is what travels, so never reorder or
    // remove an entry (an unused one just stays). At most 255 entries.
    const char* const paramIds[] =
    {
        // 2D scope (and "source" / "freeze", which the 3D and orbital views read too)
        "time", "zoom", "trigMode", "trigLevel", "view", "source", "persist", "freeze", "spectrum",
        // double slit graphic in the 2D scope
        "dsOn", "dsSizing", "dsMultiple", "dsSeparation", "dsWidth", "dsFringes", "dsDistance", "dsEar",
        // 3D views
        "v3Mode", "v3Map", "v3Embed", "v3StereoZ", "v3Colour", "v3Window", "v3Tau", "v3TauAuto", "v3Scale",
        "v3Depth", "v3Twist", "v3P", "v3Q", "v3Rows", "v3Thick", "v3Trail", "v3Spin",
        // orbital: bridge (OrbitalModel) and render (OrbitalView)
        "orbPotential", "orbMapping", "orbAngular", "orbBasis", "orbFundMode", "orbFundHz", "orbTimeScale",
        "orbQuantize", "orbMaxTerms", "orbMaxN", "orbL", "orbM", "orbSmooth", "orbGate", "orbZ", "orbScreen", "orbSolo",
        "orbRMode", "orbRBudget", "orbRExposure", "orbRContrast", "orbRThreshold", "orbRColour", "orbRSpin",
        "orbRSlice", "orbRRes", "orbRPoints", "orbRFreeze",
        // wave sculptor (the tile draws the final partials from `extra`; these are shown as text)
        "scMorph", "scTilt", "scOddEven", "scFormantPos", "scFormantGain", "scFormantWidth", "scStretch",
        "scPartials", "scComb", "scCombOffset", "scPhase",
    };
    constexpr int numParamIds = (int) (sizeof (paramIds) / sizeof (paramIds[0]));
    static_assert (numParamIds <= 255, "the index travels as one byte");

    // Text settings in the state tree. APPEND ONLY, like paramIds.
    const char* const textIds[] = { "v3FormulaX", "v3FormulaY", "v3FormulaZ" };
    constexpr int numTextIds = (int) (sizeof (textIds) / sizeof (textIds[0]));

    constexpr int maxTextBytes = 512;   // a formula longer than this is cut (the tile then shows an error)

    int clampView (int v) { return v >= 0 && v <= 3 ? v : 0; }

    // What the player's Jam Sesh tile shows: the view chosen for it in the Jam Sesh page (state property
    // "jamTileView": 1..4 = 2D scope .. wave sculptor), or with 0 ("Follow my view") the view they have
    // open; on the Jam Sesh page itself (mainView 4) the one they had open last ("jamLastView", kept by
    // the editor).
    int tileView (juce::AudioProcessorValueTreeState& s)
    {
        const int chosen = (int) s.state.getProperty ("jamTileView", 0);
        if (chosen >= 1 && chosen <= 4)
            return chosen - 1;
        const int open = s.getRawParameterValue ("mainView") != nullptr ? (int) s.getRawParameterValue ("mainView")->load() : 0;
        return open >= 0 && open <= 3 ? open : clampView ((int) s.state.getProperty ("jamLastView", 0));
    }

    void captureCommon (juce::AudioProcessorValueTreeState& s, ViewState& v)
    {
        v.mainView = tileView (s);
        for (auto* id : paramIds)
            if (auto* p = s.getRawParameterValue (id))
                v.params.emplace_back (id, p->load());
        for (auto* id : textIds)
            if (s.state.hasProperty (id))
                v.texts.emplace_back (id, s.state.getProperty (id).toString().substring (0, maxTextBytes));
    }
}

//==============================================================================
ViewState ViewState::capture (ClodScopeProcessor& proc)
{
    ViewState v;
    captureCommon (proc.apvts, v);

    sculpt::PartialSet set;
    proc.sculpt.copyDisplayPartials (set);
    SculptWave w;
    w.count = juce::jlimit (0, SculptWave::maxPartials, set.count);
    w.harmonic = set.harmonic;
    for (int k = 0; k < w.count; ++k)
    {
        w.ratio[k] = set.ratio[(size_t) k];
        w.amp[k] = set.amp[(size_t) k];
        w.phase[k] = set.phase[(size_t) k];
    }
    v.extra = w.toBytes();
    return v;
}

ViewState ViewState::capture (juce::AudioProcessorValueTreeState& s)
{
    ViewState v;
    captureCommon (s, v);

    // the stored tables, morphed A -> B (the other macros need the sculptor's model)
    sculpt::BaseTable t[2] = { SculptModel::makeWave (0), SculptModel::makeWave (0) };
    const auto child = s.state.getChildWithName ("Sculpt");
    if (child.isValid())
    {
        SculptModel::tableFromString (child.getProperty ("tableA").toString(), t[0]);
        if (! SculptModel::tableFromString (child.getProperty ("tableB").toString(), t[1]))
            t[1] = t[0];
    }
    const float morph = juce::jlimit (0.0f, 1.0f, v.get ("scMorph", 0.0f));
    SculptWave w;
    w.count = SculptWave::maxPartials;
    for (int k = 0; k < w.count; ++k)
    {
        w.ratio[k] = (float) (k + 1);
        w.amp[k] = t[0].amp[(size_t) k] + (t[1].amp[(size_t) k] - t[0].amp[(size_t) k]) * morph;
        w.phase[k] = morph < 0.5f ? t[0].phase[(size_t) k] : t[1].phase[(size_t) k];
    }
    // scale so the wave peaks near 1, like the synth's set
    float peak = 1.0e-6f;
    for (int i = 0; i < 256; ++i)
        peak = juce::jmax (peak, std::abs (w.sampleCycle ((float) i / 256.0f)));
    for (int k = 0; k < w.count; ++k)
        w.amp[k] /= peak;
    v.extra = w.toBytes();
    return v;
}

//==============================================================================
juce::MemoryBlock ViewState::toBytes() const
{
    juce::MemoryOutputStream out (600);
    out.writeByte ('V');
    out.writeByte ('S');
    out.writeByte ((char) formatVersion);
    out.writeByte ((char) clampView (mainView));

    // only the ids the registry knows travel
    juce::MemoryOutputStream body (512);
    int n = 0;
    for (const auto& [id, value] : params)
        for (int i = 0; i < numParamIds; ++i)
            if (id == paramIds[i])
            {
                body.writeByte ((char) i);
                body.writeFloat (value);
                ++n;
                break;
            }
    out.writeByte ((char) n);
    out << body.getMemoryBlock();

    body.reset();
    n = 0;
    for (const auto& [id, text] : texts)
        for (int i = 0; i < numTextIds; ++i)
            if (id == textIds[i])
            {
                const auto utf8 = text.substring (0, maxTextBytes).toUTF8();
                const auto len = (int) juce::jmin ((size_t) 65535, std::strlen (utf8.getAddress()));
                body.writeByte ((char) i);
                body.writeShort ((short) len);
                body.write (utf8.getAddress(), (size_t) len);
                ++n;
                break;
            }
    out.writeByte ((char) n);
    out << body.getMemoryBlock();

    const auto extraSize = (int) juce::jmin ((size_t) 65535, extra.getSize());
    out.writeShort ((short) extraSize);
    out.write (extra.getData(), (size_t) extraSize);
    return out.getMemoryBlock();
}

bool ViewState::fromBytes (const void* data, size_t size, ViewState& out)
{
    if (data == nullptr || size < 7)
        return false;
    const auto* b = static_cast<const juce::uint8*> (data);
    if (b[0] != 'V' || b[1] != 'S' || b[2] < 1)
        return false;   // a newer format may only append, so its version is still readable here

    juce::MemoryInputStream in (data, size, false);
    in.skipNextBytes (3);
    ViewState v;
    v.mainView = clampView ((int) (juce::uint8) in.readByte());

    const int numParams = (juce::uint8) in.readByte();
    for (int k = 0; k < numParams; ++k)
    {
        if (in.getNumBytesRemaining() < 5)
            return false;
        const int index = (juce::uint8) in.readByte();
        const float value = in.readFloat();
        if (index < numParamIds && std::isfinite (value))
            v.params.emplace_back (paramIds[index], value);
    }

    if (in.getNumBytesRemaining() < 1)
        return false;
    const int numTexts = (juce::uint8) in.readByte();
    for (int k = 0; k < numTexts; ++k)
    {
        if (in.getNumBytesRemaining() < 3)
            return false;
        const int index = (juce::uint8) in.readByte();
        const int len = (juce::uint16) in.readShort();
        if (in.getNumBytesRemaining() < len)
            return false;
        juce::MemoryBlock text;
        in.readIntoMemoryBlock (text, len);
        if (index < numTextIds)
            v.texts.emplace_back (textIds[index], juce::String::fromUTF8 ((const char*) text.getData(), (int) text.getSize()));
    }

    if (in.getNumBytesRemaining() < 2)
        return false;
    const int extraSize = (juce::uint16) in.readShort();
    if (in.getNumBytesRemaining() < extraSize)
        return false;
    in.readIntoMemoryBlock (v.extra, extraSize);

    out = std::move (v);
    return true;
}

bool ViewState::sameAs (const ViewState& o) const
{
    return mainView == o.mainView && params == o.params && texts == o.texts && extra == o.extra;
}

float ViewState::get (juce::StringRef id, float fallback) const
{
    for (const auto& [key, value] : params)
        if (key == id)
            return value;
    return fallback;
}

juce::String ViewState::getText (juce::StringRef id, const juce::String& fallback) const
{
    for (const auto& [key, text] : texts)
        if (key == id)
            return text;
    return fallback;
}

//==============================================================================
// count, flags, then per partial: amp (u16, 0..2), phase (i16, -pi..pi), ratio (float) only when inharmonic
juce::MemoryBlock SculptWave::toBytes() const
{
    juce::MemoryOutputStream out (400);
    const int n = juce::jlimit (0, maxPartials, count);
    out.writeByte ((char) n);
    out.writeByte ((char) (harmonic ? 1 : 0));
    const float pi = juce::MathConstants<float>::pi;
    for (int k = 0; k < n; ++k)
    {
        out.writeShort ((short) (juce::uint16) juce::roundToInt (juce::jlimit (0.0f, 2.0f, amp[k]) * 32767.5f));
        float ph = std::remainder (phase[k], 2.0f * pi);
        out.writeShort ((short) juce::roundToInt (juce::jlimit (-1.0f, 1.0f, ph / pi) * 32767.0f));
        if (! harmonic)
            out.writeFloat (ratio[k]);
    }
    return out.getMemoryBlock();
}

bool SculptWave::fromBytes (const juce::MemoryBlock& block, SculptWave& out)
{
    if (block.getSize() < 2)
        return false;
    juce::MemoryInputStream in (block, false);
    SculptWave w;
    w.count = juce::jmin (maxPartials, (int) (juce::uint8) in.readByte());
    w.harmonic = in.readByte() != 0;
    const float pi = juce::MathConstants<float>::pi;
    for (int k = 0; k < w.count; ++k)
    {
        if (in.getNumBytesRemaining() < (w.harmonic ? 4 : 8))
            return false;
        w.amp[k] = (float) (juce::uint16) in.readShort() / 32767.5f;
        w.phase[k] = (float) in.readShort() / 32767.0f * pi;
        w.ratio[k] = w.harmonic ? (float) (k + 1) : in.readFloat();
    }
    out = w;
    return true;
}

float SculptWave::sampleCycle (float x) const
{
    const float twoPi = juce::MathConstants<float>::twoPi;
    float v = 0.0f;
    for (int k = 0; k < count; ++k)
        if (amp[k] > 0.0f)
            v += amp[k] * std::sin (twoPi * ratio[k] * x + phase[k]);
    return v;
}

//==============================================================================
float StateParams::get (juce::StringRef id, float fallback) const
{
    for (const auto& [key, value] : state.params)
        if (key == id)
            return value;
    if (defaults != nullptr)
        if (auto* p = defaults->getParameter (id))
            return p->convertFrom0to1 (p->getDefaultValue());
    return fallback;
}

juce::String StateParams::getText (juce::StringRef id, const juce::String& fallback) const
{
    return state.getText (id, fallback);
}
