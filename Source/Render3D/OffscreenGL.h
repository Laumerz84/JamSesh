#pragma once

#include <memory>

// An OpenGL 3.2 core context on a hidden window that is never shown (on macOS a CGL context, which needs no
// window). It only renders into framebuffer objects and never presents (no SwapBuffers), so nothing on screen is a GPU surface: the 3D views reach
// the window as an ordinary painted image, and window capture of the host (Discord, OBS) sees them as
// part of the host's window (see UI/StreamSafe.h). Message thread only.
class OffscreenGL
{
public:
    OffscreenGL();
    ~OffscreenGL();

    bool create();                    // false if no OpenGL 3.2 core context could be made here
    bool isValid() const noexcept;

    // Makes the context current on this thread; release() puts back whatever context was current before
    bool makeCurrent();
    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
