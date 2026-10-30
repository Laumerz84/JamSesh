#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <memory>

// GPU (OpenGL) 3D viewport shared by the 3D wave views and the orbital view.
// Producers build geometry on the message thread and submit it once per frame. The GPU renders
// off-screen and the frame is painted as an image, so screen capture sees an ordinary window
// (Render3D/OffscreenGL.h).
//
// Owner: Agent B (implements Scene3D.cpp; may add private members here). Public API is a contract.

struct Vertex3
{
    float x, y, z;       // world space; +y up
    float r, g, b, a;    // 0..1
};

struct DrawBatch
{
    enum Type { lines, lineStrip, points, triangles };

    Type type = lineStrip;
    std::vector<Vertex3> vertices;
    float lineWidth = 1.5f;        // pixels (drivers may clamp; implement thick lines yourself if needed)
    float pointSize = 2.0f;        // pixels, used when pointWorldSize <= 0
    float pointWorldSize = 0.0f;   // > 0: soft round point sprites of this world-space diameter, perspective-scaled
    bool additive = true;          // additive blending (order-independent glow); false = normal alpha blend
    bool depthTest = false;
};

class Scene3D : public juce::Component
{
public:
    Scene3D();
    ~Scene3D() override;

    // Message thread. Replaces the geometry drawn from the next frame on (internally double-buffered).
    void submitFrame (std::vector<DrawBatch>&& batches);

    // Camera frames a sphere of this radius around the origin (producers work in their own units).
    void setWorldRadius (float radius);

    // Draws X/Y/Z axes through the origin with labels at the ends (projected 2D text).
    void setAxes (bool show, const juce::String& xLabel, const juce::String& yLabel, const juce::String& zLabel);

    // 2D text drawn over the 3D view.
    void setOverlay (const juce::StringArray& topLeftLines, const juce::StringArray& bottomLeftLines);

    void setAutoSpin (float degreesPerSecond);   // 0 = off. Paused while the user is dragging.
    void resetCamera();

    struct Camera { float yawDeg = 35.0f, pitchDeg = 20.0f, zoom = 1.0f; };
    Camera getCamera() const;
    void setCamera (Camera);

    bool isGpuAvailable() const;   // false if OpenGL failed to start; the component then paints a message

    // Colours for drawing on the 3D screen (Palette::screen). The screen stays dark in the light themes,
    // whose palette is made for light backgrounds, so there a colour is lifted to read on it.
    // Dark themes: unchanged (ink (c) == c, inkDim() == Palette::dim).
    static juce::Colour ink (juce::Colour);
    static juce::Colour inkDim();   // secondary text on the screen

    // Mouse: left-drag rotates, wheel zooms, double-click resets the camera.

    void lookAndFeelChanged() override;   // themes: the frame is rendered again on the new background

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
