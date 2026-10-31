#include "Scene3D.h"
#include "OffscreenGL.h"
#include "../UI/Theme.h"
#include <juce_opengl/juce_opengl.h>
#include <atomic>
#include <cmath>
#include <mutex>

using namespace juce::gl;

namespace
{
    struct Vec3 { float x, y, z; };

    Vec3 sub (Vec3 a, Vec3 b)     { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    float dot (Vec3 a, Vec3 b)    { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 cross (Vec3 a, Vec3 b)   { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    Vec3 normalise (Vec3 a)
    {
        const float len = std::sqrt (dot (a, a));
        return len > 1.0e-9f ? Vec3 { a.x / len, a.y / len, a.z / len } : Vec3 { 0.0f, 0.0f, 0.0f };
    }

    // Column-major 4x4, as OpenGL expects
    struct Mat4 { float m[16] {}; };

    Mat4 mul (const Mat4& a, const Mat4& b)
    {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row)
            {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k)
                    s += a.m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = s;
            }
        return r;
    }

    Mat4 perspective (float fovYRad, float aspect, float n, float f)
    {
        Mat4 p;
        const float t = 1.0f / std::tan (fovYRad * 0.5f);
        p.m[0] = t / aspect;
        p.m[5] = t;
        p.m[10] = (f + n) / (n - f);
        p.m[11] = -1.0f;
        p.m[14] = 2.0f * f * n / (n - f);
        return p;
    }

    Mat4 lookAt (Vec3 eye, Vec3 target, Vec3 up)
    {
        const Vec3 f = normalise (sub (target, eye));
        const Vec3 s = normalise (cross (f, up));
        const Vec3 u = cross (s, f);
        Mat4 v;
        v.m[0] = s.x;  v.m[4] = s.y;  v.m[8]  = s.z;
        v.m[1] = u.x;  v.m[5] = u.y;  v.m[9]  = u.z;
        v.m[2] = -f.x; v.m[6] = -f.y; v.m[10] = -f.z;
        v.m[12] = -dot (s, eye);
        v.m[13] = -dot (u, eye);
        v.m[14] = dot (f, eye);
        v.m[15] = 1.0f;
        return v;
    }

    const char* basicVertexShader = R"(#version 150
in vec3 position;
in vec4 colour;
uniform mat4 mvp;
uniform float pointWorldSize;
uniform float pointPixelSize;
uniform float viewportH;
uniform float projScale;
out vec4 vColour;
void main()
{
    vec4 p = mvp * vec4 (position, 1.0);
    gl_Position = p;
    vColour = colour;
    float worldPx = pointWorldSize * projScale * viewportH * 0.5 / max (p.w, 0.0001);
    gl_PointSize = pointWorldSize > 0.0 ? clamp (worldPx, 1.0, 256.0) : max (pointPixelSize, 1.0);
}
)";

    const char* basicFragmentShader = R"(#version 150
in vec4 vColour;
uniform int isPoint;
out vec4 fragColour;
void main()
{
    vec4 c = vColour;
    if (isPoint == 1)
    {
        vec2 d = gl_PointCoord * 2.0 - 1.0;
        float r2 = dot (d, d);
        if (r2 > 1.0)
            discard;
        c.a *= exp (-2.2 * r2) * (1.0 - r2 * r2);
    }
    fragColour = c;
}
)";

    // Expands each line segment into a screen-aligned quad of `lineWidth` pixels, with soft edges
    const char* lineGeometryShader = R"(#version 150
layout (lines) in;
layout (triangle_strip, max_vertices = 4) out;
in vec4 vColour[];
out vec4 gColour;
out float gEdge;
uniform vec2 viewport;
uniform float lineWidth;
void main()
{
    vec4 a = gl_in[0].gl_Position;
    vec4 b = gl_in[1].gl_Position;
    if (a.w <= 0.0001 || b.w <= 0.0001)
        return;
    vec2 sa = a.xy / a.w * viewport * 0.5;
    vec2 sb = b.xy / b.w * viewport * 0.5;
    vec2 dir = sb - sa;
    float len = length (dir);
    dir = len > 0.0001 ? dir / len : vec2 (1.0, 0.0);
    vec2 off = vec2 (-dir.y, dir.x) * (lineWidth * 0.5) / (viewport * 0.5);
    gColour = vColour[0]; gEdge =  1.0; gl_Position = vec4 (a.xy + off * a.w, a.zw); EmitVertex();
    gColour = vColour[0]; gEdge = -1.0; gl_Position = vec4 (a.xy - off * a.w, a.zw); EmitVertex();
    gColour = vColour[1]; gEdge =  1.0; gl_Position = vec4 (b.xy + off * b.w, b.zw); EmitVertex();
    gColour = vColour[1]; gEdge = -1.0; gl_Position = vec4 (b.xy - off * b.w, b.zw); EmitVertex();
    EndPrimitive();
}
)";

    const char* lineFragmentShader = R"(#version 150
in vec4 gColour;
in float gEdge;
out vec4 fragColour;
void main()
{
    vec4 c = gColour;
    c.a *= 1.0 - smoothstep (0.35, 1.0, abs (gEdge));
    fragColour = c;
}
)";

    constexpr float fovY = 0.785398f;   // 45 degrees
    constexpr float maxFramePixels = 4.0e6f;   // larger views render at this size and are scaled up

    // Context current. 0 if the stage does not compile.
    GLuint compileShader (GLenum type, const char* source)
    {
        const GLuint s = glCreateShader (type);
        glShaderSource (s, 1, &source, nullptr);
        glCompileShader (s);
        GLint ok = GL_FALSE;
        glGetShaderiv (s, GL_COMPILE_STATUS, &ok);
        if (ok == GL_FALSE)
        {
            char log[1024] = {};
            glGetShaderInfoLog (s, (GLsizei) sizeof (log), nullptr, log);
            DBG ("Scene3D shader error: " << log);
            glDeleteShader (s);
            return 0;
        }
        return s;
    }

    // Context current. Attribute 0 = position, 1 = colour; 0 if any stage fails.
    GLuint buildProgram (const char* vs, const char* gs, const char* fs)
    {
        const GLuint stages[] = { compileShader (GL_VERTEX_SHADER, vs),
                                  gs != nullptr ? compileShader (GL_GEOMETRY_SHADER, gs) : 0u,
                                  compileShader (GL_FRAGMENT_SHADER, fs) };
        GLuint prog = 0;
        if (stages[0] != 0 && stages[2] != 0 && (gs == nullptr || stages[1] != 0))
        {
            prog = glCreateProgram();
            for (auto s : stages)
                if (s != 0)
                    glAttachShader (prog, s);
            glBindAttribLocation (prog, 0, "position");
            glBindAttribLocation (prog, 1, "colour");
            glLinkProgram (prog);
            GLint ok = GL_FALSE;
            glGetProgramiv (prog, GL_LINK_STATUS, &ok);
            if (ok == GL_FALSE)
            {
                glDeleteProgram (prog);
                prog = 0;
            }
        }
        for (auto s : stages)
            if (s != 0)
                glDeleteShader (s);
        return prog;
    }
}

//==============================================================================
// Renders on the message thread, inside the canvas's paint: the scene is drawn off-screen, read back
// and painted as an image (see OffscreenGL.h for why nothing here is a GPU surface on screen).
struct Scene3D::Impl : public juce::MouseListener,
                       public juce::ComponentListener,
                       private juce::Timer
{
    struct Canvas : public juce::Component
    {
        explicit Canvas (Impl& i) : impl (i) { setOpaque (true); setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override { impl.paintFrame (g, getLocalBounds()); }
        Impl& impl;
    };

    struct Overlay : public juce::Component
    {
        explicit Overlay (Impl& i) : impl (i) { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override { impl.paintOverlay (g, getLocalBounds()); }
        Impl& impl;
    };

    explicit Impl (Scene3D& o) : owner (o), canvas (*this), overlay (*this)
    {
        lastTick = juce::Time::getMillisecondCounterHiRes();

        owner.addAndMakeVisible (canvas);
        owner.addAndMakeVisible (overlay);
        owner.addMouseListener (this, false);
        owner.addComponentListener (this);

        startTimerHz (60);
    }

    ~Impl() override
    {
        stopTimer();
        releaseGpu();
        owner.removeMouseListener (this);
        owner.removeComponentListener (this);
    }

    //==========================================================================
    // GPU, context current unless noted
    bool startGpu()   // any context state; makes the context and shaders on first use
    {
        if (gpuOk.load() || gpuFailed.load())
            return gpuOk.load();

        if (gl.create() && gl.makeCurrent())
        {
            juce::gl::loadFunctions();
            basicProgram = buildProgram (basicVertexShader, nullptr, basicFragmentShader);
            if (basicProgram != 0)
            {
                lineProgram = buildProgram (basicVertexShader, lineGeometryShader, lineFragmentShader);   // optional
                glGenVertexArrays (1, &vao);
                glGenBuffers (1, &vbo);
                GLint maxSamples = 0;
                glGetIntegerv (GL_MAX_SAMPLES, &maxSamples);
                samples = juce::jlimit (0, 4, (int) maxSamples);
                gpuOk = true;
            }
            gl.release();
        }

        gpuFailed = ! gpuOk.load();
        return gpuOk.load();
    }

    void releaseGpu()   // any context state
    {
        if (gl.isValid() && gl.makeCurrent())
        {
            releaseFramebuffers();
            if (basicProgram != 0) glDeleteProgram (basicProgram);
            if (lineProgram != 0)  glDeleteProgram (lineProgram);
            if (vbo != 0) glDeleteBuffers (1, &vbo);
            if (vao != 0) glDeleteVertexArrays (1, &vao);
            gl.release();
        }
        basicProgram = lineProgram = vbo = vao = 0;
        gpuOk = false;
    }

    void releaseFramebuffers()
    {
        if (drawFbo != 0)    glDeleteFramebuffers (1, &drawFbo);
        if (resolveFbo != 0) glDeleteFramebuffers (1, &resolveFbo);
        const GLuint buffers[] = { drawColour, drawDepth, resolveColour };
        glDeleteRenderbuffers (3, buffers);   // zero names are ignored
        drawFbo = resolveFbo = drawColour = drawDepth = resolveColour = 0;
        fboW = fboH = 0;
    }

    // Multisampled colour + depth to draw into, and a plain colour buffer to resolve into and read back
    bool ensureFramebuffers (int w, int h)
    {
        if (drawFbo != 0 && w == fboW && h == fboH)
            return true;
        releaseFramebuffers();

        for (const int s : { samples, 0 })   // without multisampling if the driver refuses it
        {
            glGenRenderbuffers (1, &drawColour);
            glBindRenderbuffer (GL_RENDERBUFFER, drawColour);
            glRenderbufferStorageMultisample (GL_RENDERBUFFER, s, GL_RGBA8, w, h);
            glGenRenderbuffers (1, &drawDepth);
            glBindRenderbuffer (GL_RENDERBUFFER, drawDepth);
            glRenderbufferStorageMultisample (GL_RENDERBUFFER, s, GL_DEPTH_COMPONENT24, w, h);
            glGenFramebuffers (1, &drawFbo);
            glBindFramebuffer (GL_FRAMEBUFFER, drawFbo);
            glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, drawColour);
            glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, drawDepth);
            const bool drawOk = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

            glGenRenderbuffers (1, &resolveColour);
            glBindRenderbuffer (GL_RENDERBUFFER, resolveColour);
            glRenderbufferStorage (GL_RENDERBUFFER, GL_RGBA8, w, h);
            glGenFramebuffers (1, &resolveFbo);
            glBindFramebuffer (GL_FRAMEBUFFER, resolveFbo);
            glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, resolveColour);
            const bool resolveOk = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

            glBindRenderbuffer (GL_RENDERBUFFER, 0);
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
            if (drawOk && resolveOk)
            {
                fboW = w;
                fboH = h;
                return true;
            }
            releaseFramebuffers();
            if (s == 0)
                break;
        }
        return false;
    }

    Mat4 viewProjection (float width, float height, float& projScale) const
    {
        const float R = juce::jmax (1.0e-4f, radius.load());
        const float D = R * 2.8f / juce::jlimit (0.05f, 50.0f, zoom.load());
        const float yawR = juce::degreesToRadians (yaw.load());
        const float pitchR = juce::degreesToRadians (juce::jlimit (-89.0f, 89.0f, pitch.load()));
        const Vec3 eye { D * std::cos (pitchR) * std::sin (yawR), D * std::sin (pitchR), D * std::cos (pitchR) * std::cos (yawR) };

        const Mat4 view = lookAt (eye, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
        const Mat4 proj = perspective (fovY, juce::jmax (0.01f, width / juce::jmax (1.0f, height)),
                                       juce::jmax (D * 0.01f, 1.0e-4f), D + R * 20.0f);
        projScale = proj.m[5];
        return mul (proj, view);
    }

    void buildGuides (std::vector<DrawBatch>& out, bool axes)
    {
        const float R = radius.load();
        const juce::Colour gc = Theme::style().light ? Palette::screen.interpolatedWith (Palette::screenText, 0.25f)
                                                     : Palette::grid.brighter (0.25f);

        DrawBatch grid;
        grid.type = DrawBatch::lines;
        grid.lineWidth = 1.0f;
        grid.additive = false;
        const float floorY = -R;
        const int halfLines = 6;
        const float spacing = R / 3.0f;
        const float ext = (float) halfLines * spacing;
        for (int i = -halfLines; i <= halfLines; ++i)
        {
            const float p = (float) i * spacing;
            const float a = i == 0 ? 0.55f : 0.3f;
            grid.vertices.push_back ({ p, floorY, -ext, gc.getFloatRed(), gc.getFloatGreen(), gc.getFloatBlue(), a });
            grid.vertices.push_back ({ p, floorY,  ext, gc.getFloatRed(), gc.getFloatGreen(), gc.getFloatBlue(), a });
            grid.vertices.push_back ({ -ext, floorY, p, gc.getFloatRed(), gc.getFloatGreen(), gc.getFloatBlue(), a });
            grid.vertices.push_back ({  ext, floorY, p, gc.getFloatRed(), gc.getFloatGreen(), gc.getFloatBlue(), a });
        }
        out.push_back (std::move (grid));

        if (axes)
        {
            DrawBatch ax;
            ax.type = DrawBatch::lines;
            ax.lineWidth = 1.3f;
            ax.additive = true;
            const juce::Colour cols[3] = { Scene3D::ink (Palette::pink), Scene3D::ink (Palette::green), Scene3D::ink (Palette::blue) };
            for (int k = 0; k < 3; ++k)
            {
                const auto c = cols[k];
                float a[3] = { 0.0f, 0.0f, 0.0f }, b[3] = { 0.0f, 0.0f, 0.0f };
                a[k] = -R;
                b[k] = R;
                ax.vertices.push_back ({ a[0], a[1], a[2], c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue(), 0.15f });
                ax.vertices.push_back ({ b[0], b[1], b[2], c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue(), 0.65f });
            }
            out.push_back (std::move (ax));
        }
    }

    void drawBatch (const DrawBatch& b, const Mat4& mvp, float projScale, int w, int h, float renderScale)
    {
        const auto& v = b.vertices;
        if (v.empty() || basicProgram == 0)
            return;

        if (b.additive) glBlendFunc (GL_SRC_ALPHA, GL_ONE);
        else            glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        if (b.depthTest)
        {
            glEnable (GL_DEPTH_TEST);
            glDepthFunc (GL_LEQUAL);
            glDepthMask (GL_TRUE);
        }
        else
        {
            glDisable (GL_DEPTH_TEST);
            glDepthMask (GL_FALSE);
        }

        glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (v.size() * sizeof (Vertex3)), v.data(), GL_STREAM_DRAW);
        glEnableVertexAttribArray (0);
        glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, (GLsizei) sizeof (Vertex3), nullptr);
        glEnableVertexAttribArray (1);
        glVertexAttribPointer (1, 4, GL_FLOAT, GL_FALSE, (GLsizei) sizeof (Vertex3), (const void*) (3 * sizeof (float)));

        const bool isLine = b.type == DrawBatch::lines || b.type == DrawBatch::lineStrip;
        const float widthPx = b.lineWidth * renderScale;
        const GLuint prog = (isLine && lineProgram != 0 && widthPx > 1.01f) ? lineProgram : basicProgram;
        glUseProgram (prog);
        glUniformMatrix4fv (glGetUniformLocation (prog, "mvp"), 1, GL_FALSE, mvp.m);

        if (prog == lineProgram)
        {
            glUniform2f (glGetUniformLocation (prog, "viewport"), (GLfloat) w, (GLfloat) h);
            glUniform1f (glGetUniformLocation (prog, "lineWidth"), widthPx);
        }
        else
        {
            glUniform1i (glGetUniformLocation (prog, "isPoint"), b.type == DrawBatch::points ? 1 : 0);
            glUniform1f (glGetUniformLocation (prog, "pointWorldSize"), b.pointWorldSize);
            glUniform1f (glGetUniformLocation (prog, "pointPixelSize"), b.pointSize * renderScale);
            glUniform1f (glGetUniformLocation (prog, "viewportH"), (GLfloat) h);
            glUniform1f (glGetUniformLocation (prog, "projScale"), projScale);
        }

        GLenum mode = GL_LINE_STRIP;
        switch (b.type)
        {
            case DrawBatch::lines:     mode = GL_LINES; break;
            case DrawBatch::lineStrip: mode = GL_LINE_STRIP; break;
            case DrawBatch::points:    mode = GL_POINTS; break;
            case DrawBatch::triangles: mode = GL_TRIANGLES; break;
        }
        glDrawArrays (mode, 0, (GLsizei) v.size());
    }

    // Draws the scene into the framebuffers (w x h physical pixels) and reads it back into `frame`
    void renderFrame (int w, int h, float renderScale)
    {
        {
            std::lock_guard<std::mutex> lock (frameLock);
            if (hasPending)
            {
                std::swap (current, pending);
                hasPending = false;
            }
        }

        const auto bg = Palette::screen;   // dark even in light themes: the views add light
        glBindFramebuffer (GL_FRAMEBUFFER, drawFbo);
        glViewport (0, 0, w, h);
        glClearColor (bg.getFloatRed(), bg.getFloatGreen(), bg.getFloatBlue(), 1.0f);
        glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable (GL_BLEND);
        glEnable (GL_PROGRAM_POINT_SIZE);
        glEnable (GL_MULTISAMPLE);
        glDisable (GL_CULL_FACE);

        // drawn upside down, so the rows read back top row first, as images store them
        float projScale = 1.0f;
        Mat4 flipY;
        flipY.m[0] = 1.0f; flipY.m[5] = -1.0f; flipY.m[10] = 1.0f; flipY.m[15] = 1.0f;
        const Mat4 mvp = mul (flipY, viewProjection ((float) w, (float) h, projScale));

        guides.clear();
        buildGuides (guides, axesShown.load());

        glBindVertexArray (vao);
        glBindBuffer (GL_ARRAY_BUFFER, vbo);
        for (auto& b : guides)
            drawBatch (b, mvp, projScale, w, h, renderScale);
        for (auto& b : current)
            drawBatch (b, mvp, projScale, w, h, renderScale);
        glBindBuffer (GL_ARRAY_BUFFER, 0);
        glBindVertexArray (0);
        glUseProgram (0);
        glDisable (GL_DEPTH_TEST);
        glDepthMask (GL_TRUE);
        glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glBindFramebuffer (GL_READ_FRAMEBUFFER, drawFbo);
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, resolveFbo);
        glBlitFramebuffer (0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, resolveFbo);

        // an opaque RGB image, so painting it is a plain copy
        if (frame.getWidth() != w || frame.getHeight() != h)
            frame = juce::Image (juce::Image::RGB, w, h, false, juce::SoftwareImageType());
        {
            juce::Image::BitmapData px (frame, juce::Image::BitmapData::writeOnly);
            jassert (px.pixelStride == 3 && px.lineStride == ((3 * w + 3) & ~3));   // rows padded to 4 bytes, as GL packs them
            glPixelStorei (GL_PACK_ALIGNMENT, 4);
           #if JUCE_MAC
            glReadPixels (0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data);   // JUCE's RGB pixels are R, G, B bytes on macOS
           #else
            glReadPixels (0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, px.data);   // BGR bytes = JUCE's RGB pixels in memory
           #endif
        }
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
    }

    //==========================================================================
    // Message thread (Canvas::paint): re-renders if anything changed, then draws the latest frame
    void paintFrame (juce::Graphics& g, juce::Rectangle<int> area)
    {
        float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const float pixels = (float) area.getWidth() * (float) area.getHeight() * scale * scale;
        if (pixels > maxFramePixels)
            scale *= std::sqrt (maxFramePixels / pixels);
        const int w = juce::jmax (1, juce::roundToInt ((float) area.getWidth() * scale));
        const int h = juce::jmax (1, juce::roundToInt ((float) area.getHeight() * scale));

        const bool changed = dirty.exchange (false);
        if (startGpu() && (changed || frame.getWidth() != w || frame.getHeight() != h) && gl.makeCurrent())
        {
            if (ensureFramebuffers (w, h))
            {
                renderFrame (w, h, scale);
            }
            else
            {
                gpuOk = false;
                gpuFailed = true;
            }
            gl.release();
        }

        if (gpuOk.load() && frame.isValid())
        {
            g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
            g.drawImageTransformed (frame, juce::AffineTransform::scale ((float) area.getWidth() / (float) frame.getWidth(),
                                                                         (float) area.getHeight() / (float) frame.getHeight()));
        }
        else
        {
            g.fillAll (Palette::screen);
        }
    }

    //==========================================================================
    // Overlay (painted over the frame like any other component)
    bool project (Vec3 p, juce::Rectangle<int> area, juce::Point<float>& out) const
    {
        float ps = 1.0f;
        const Mat4 m = viewProjection ((float) area.getWidth(), (float) area.getHeight(), ps);
        const float cx = m.m[0] * p.x + m.m[4] * p.y + m.m[8]  * p.z + m.m[12];
        const float cy = m.m[1] * p.x + m.m[5] * p.y + m.m[9]  * p.z + m.m[13];
        const float cw = m.m[3] * p.x + m.m[7] * p.y + m.m[11] * p.z + m.m[15];
        if (cw <= 1.0e-4f)
            return false;
        out = { (cx / cw * 0.5f + 0.5f) * (float) area.getWidth(), (0.5f - cy / cw * 0.5f) * (float) area.getHeight() };
        return true;
    }

    void paintOverlay (juce::Graphics& g, juce::Rectangle<int> area)
    {
        using namespace Palette;
        if (! gpuOk.load())
        {
            if (gpuFailed.load())
            {
                g.fillAll (screen);
                g.setColour (Scene3D::ink (amber));
                g.setFont (Theme::font (14.0f, true));
                g.drawText ("3D view unavailable", area.reduced (20).withTrimmedBottom (area.getHeight() / 2), juce::Justification::centredBottom);
                g.setColour (Scene3D::inkDim());
                g.setFont (Theme::font (12.0f));
                g.drawFittedText ("This view needs OpenGL 3.2 on the graphics card, which could not be started here.\n"
                                  "Updating the GPU driver usually fixes this. The 2D scope still works.",
                                  area.reduced (30).withTrimmedTop (area.getHeight() / 2), juce::Justification::centredTop, 3);
            }
            return;
        }

        juce::String labels[3];
        juce::StringArray top, bottom;
        bool axes = false;
        {
            const juce::ScopedLock sl (textLock);
            for (int k = 0; k < 3; ++k)
                labels[k] = axisLabel[k];
            top = topLines;
            bottom = bottomLines;
            axes = axesShown.load();
        }

        if (axes)
        {
            const float R = radius.load() * 1.1f;
            const Vec3 ends[3] = { { R, 0.0f, 0.0f }, { 0.0f, R, 0.0f }, { 0.0f, 0.0f, R } };
            const juce::Colour cols[3] = { Scene3D::ink (pink), Scene3D::ink (green), Scene3D::ink (blue) };
            g.setFont (Theme::font (12.0f, true));
            for (int k = 0; k < 3; ++k)
            {
                juce::Point<float> p;
                if (labels[k].isNotEmpty() && project (ends[k], area, p))
                {
                    const int tw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), labels[k]) + 12;
                    // kept inside the view (small Jam Sesh tiles put the y label at the top edge)
                    auto r = juce::Rectangle<int> ((int) p.x + 4, (int) p.y - 9, tw, 18).constrainedWithin (area);
                    g.setColour (screen.withAlpha (0.65f));
                    g.fillRoundedRectangle (r.toFloat(), 4.0f);
                    g.setColour (cols[k]);
                    g.drawText (labels[k], r, juce::Justification::centred);
                }
            }
        }

        auto textArea = area.reduced (10, 8);
        for (int i = 0; i < top.size(); ++i)
        {
            g.setFont (Theme::font (i == 0 ? 13.0f : 11.5f, i == 0));
            g.setColour (i == 0 ? screenText : Scene3D::inkDim());
            g.drawText (top[i], textArea.removeFromTop (i == 0 ? 18 : 15), juce::Justification::topLeft);
        }

        auto bottomArea = area.reduced (10, 6);
        g.setFont (Theme::font (11.0f));
        g.setColour (Scene3D::inkDim());
        if (area.getWidth() >= 560)   // small views (Jam Sesh tiles) have no room for the hint
            g.drawText ("drag: rotate   wheel: zoom   double-click: reset", bottomArea, juce::Justification::bottomRight);
        for (int i = bottom.size(); --i >= 0;)
        {
            g.setColour (Scene3D::inkDim().brighter (0.2f));
            g.drawText (bottom[i], bottomArea.removeFromBottom (15), juce::Justification::bottomLeft);
        }
    }

    //==========================================================================
    // Message thread
    void timerCallback() override
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = (float) juce::jlimit (0.0, 0.1, (now - lastTick) * 0.001);
        lastTick = now;
        const float s = spin.load();
        if (! dragging.load() && s != 0.0f)
        {
            yaw = std::fmod (yaw.load() + s * dt, 360.0f);
            dirty = true;
        }

        if (dirty.load() && ! gpuFailed.load() && owner.isShowing())
            owner.repaint();
    }

    void componentMovedOrResized (juce::Component& c, bool, bool) override
    {
        if (&c != &owner)
            return;
        canvas.setBounds (owner.getLocalBounds());
        overlay.setBounds (owner.getLocalBounds());
        dirty = true;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isLeftButtonDown())
            return;
        dragging = true;
        dragStart = e.position;
        dragYaw = yaw.load();
        dragPitch = pitch.load();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging.load())
            return;
        const auto d = e.position - dragStart;
        yaw = dragYaw - d.x * 0.4f;
        pitch = juce::jlimit (-89.0f, 89.0f, dragPitch + d.y * 0.4f);
        dirty = true;
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        zoom = juce::jlimit (0.15f, 12.0f, zoom.load() * std::exp (w.deltaY * 0.9f));
        dirty = true;
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { resetCamera(); }

    void resetCamera()
    {
        const Camera c;
        yaw = c.yawDeg;
        pitch = c.pitchDeg;
        zoom = c.zoom;
        dirty = true;
    }

    //==========================================================================
    Scene3D& owner;
    OffscreenGL gl;
    Canvas canvas;
    Overlay overlay;

    std::mutex frameLock;
    std::vector<DrawBatch> pending, current, guides;
    bool hasPending = false;

    std::atomic<float> yaw { 35.0f }, pitch { 20.0f }, zoom { 1.0f }, radius { 1.0f }, spin { 0.0f };
    std::atomic<bool> dragging { false }, gpuOk { false }, gpuFailed { false }, axesShown { false }, dirty { true };

    juce::CriticalSection textLock;
    juce::String axisLabel[3];
    juce::StringArray topLines, bottomLines;

    GLuint basicProgram = 0, lineProgram = 0, vao = 0, vbo = 0;
    GLuint drawFbo = 0, drawColour = 0, drawDepth = 0, resolveFbo = 0, resolveColour = 0;
    int fboW = 0, fboH = 0, samples = 0;
    juce::Image frame;

    double lastTick = 0.0;
    juce::Point<float> dragStart;
    float dragYaw = 0.0f, dragPitch = 0.0f;
};

//==============================================================================
Scene3D::Scene3D()
{
    impl = std::make_unique<Impl> (*this);
}

Scene3D::~Scene3D()
{
    impl.reset();
}

juce::Colour Scene3D::ink (juce::Colour c)
{
    if (! Theme::style().light)
        return c;
    return juce::Colour::fromHSV (c.getHue(), juce::jmin (c.getSaturation(), 0.7f), juce::jmax (c.getBrightness(), 0.95f), c.getFloatAlpha());
}

juce::Colour Scene3D::inkDim()
{
    return Theme::style().light ? Palette::screenText.interpolatedWith (Palette::screen, 0.42f) : Palette::dim;
}

void Scene3D::lookAndFeelChanged()
{
    // the frame is cleared to Palette::screen: render it again in the new theme
    if (impl != nullptr)
        impl->dirty = true;
    repaint();
}

void Scene3D::submitFrame (std::vector<DrawBatch>&& batches)
{
    {
        std::lock_guard<std::mutex> lock (impl->frameLock);
        impl->pending = std::move (batches);
        impl->hasPending = true;
    }
    impl->dirty = true;
}

void Scene3D::setWorldRadius (float r)
{
    impl->radius = juce::jmax (1.0e-4f, r);
    impl->dirty = true;
}

void Scene3D::setAxes (bool show, const juce::String& xLabel, const juce::String& yLabel, const juce::String& zLabel)
{
    const juce::ScopedLock sl (impl->textLock);
    impl->axesShown = show;
    impl->axisLabel[0] = xLabel;
    impl->axisLabel[1] = yLabel;
    impl->axisLabel[2] = zLabel;
    impl->dirty = true;
}

void Scene3D::setOverlay (const juce::StringArray& topLeftLines, const juce::StringArray& bottomLeftLines)
{
    const juce::ScopedLock sl (impl->textLock);
    impl->topLines = topLeftLines;
    impl->bottomLines = bottomLeftLines;
    impl->dirty = true;
}

void Scene3D::setAutoSpin (float degreesPerSecond) { impl->spin = degreesPerSecond; }
void Scene3D::resetCamera()                        { impl->resetCamera(); }

Scene3D::Camera Scene3D::getCamera() const
{
    return { impl->yaw.load(), impl->pitch.load(), impl->zoom.load() };
}

void Scene3D::setCamera (Camera c)
{
    impl->yaw = c.yawDeg;
    impl->pitch = juce::jlimit (-89.0f, 89.0f, c.pitchDeg);
    impl->zoom = juce::jlimit (0.15f, 12.0f, c.zoom);
    impl->dirty = true;
}

bool Scene3D::isGpuAvailable() const { return impl->gpuOk.load(); }
