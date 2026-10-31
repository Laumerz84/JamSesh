#include "OffscreenGL.h"

#if defined (_WIN32)

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if defined (_MSC_VER)
 #pragma comment (lib, "opengl32.lib")
#endif

namespace
{
    const wchar_t* const windowClass = L"ClodScopeOffscreenGL";
    int classUsers = 0;   // live windows of that class (message thread)

    HINSTANCE thisModule()
    {
        HMODULE module = nullptr;
        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR> (&thisModule), &module);
        return module;
    }

    // WGL_ARB_create_context / WGL_ARB_create_context_profile
    constexpr int contextMajorVersion = 0x2091, contextMinorVersion = 0x2092,
                  contextProfileMask = 0x9126, contextCoreProfileBit = 0x0001;
    using CreateContextAttribs = HGLRC (WINAPI*) (HDC, HGLRC, const int*);
}

struct OffscreenGL::Impl
{
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC context = nullptr;
    HDC previousDC = nullptr;
    HGLRC previousContext = nullptr;
    bool attempted = false, usesClass = false;

    ~Impl()
    {
        if (context != nullptr)
        {
            if (wglGetCurrentContext() == context)
                wglMakeCurrent (nullptr, nullptr);
            wglDeleteContext (context);
        }
        if (dc != nullptr)
            ReleaseDC (window, dc);
        if (window != nullptr)
            DestroyWindow (window);
        // a DLL's window classes outlive it unless unregistered, and hosts unload and reload plugins
        if (usesClass && --classUsers == 0)
            UnregisterClassW (windowClass, thisModule());
    }

    bool create()
    {
        if (attempted)
            return context != nullptr;
        attempted = true;

        const auto module = thisModule();
        if (classUsers == 0)
        {
            WNDCLASSEXW wc {};
            wc.cbSize = sizeof (wc);
            wc.style = CS_OWNDC;
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = module;
            wc.lpszClassName = windowClass;
            if (RegisterClassExW (&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;
        }
        ++classUsers;
        usesClass = true;

        // never shown (no WS_VISIBLE), never activated, never in the taskbar
        window = CreateWindowExW (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass, L"", WS_POPUP,
                                  0, 0, 16, 16, nullptr, nullptr, module, nullptr);
        if (window == nullptr)
            return false;
        dc = GetDC (window);

        PIXELFORMATDESCRIPTOR pfd {};
        pfd.nSize = sizeof (pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 24;
        pfd.cAlphaBits = 8;
        pfd.cDepthBits = 24;
        pfd.iLayerType = PFD_MAIN_PLANE;
        const int format = ChoosePixelFormat (dc, &pfd);
        if (format == 0 || ! SetPixelFormat (dc, format, &pfd))
            return false;

        // a legacy context first: wglCreateContextAttribsARB can only be looked up while one is current
        const auto oldDC = wglGetCurrentDC();
        const auto oldContext = wglGetCurrentContext();
        const auto legacy = wglCreateContext (dc);
        if (legacy == nullptr)
            return false;

        if (wglMakeCurrent (dc, legacy))
        {
            if (auto createAttribs = reinterpret_cast<CreateContextAttribs> (wglGetProcAddress ("wglCreateContextAttribsARB")))
            {
                const int attribs[] = { contextMajorVersion, 3, contextMinorVersion, 2,
                                        contextProfileMask, contextCoreProfileBit, 0 };
                context = createAttribs (dc, nullptr, attribs);
            }
        }
        wglMakeCurrent (oldDC, oldContext);
        wglDeleteContext (legacy);
        return context != nullptr;
    }

    bool makeCurrent()
    {
        if (context == nullptr)
            return false;
        previousDC = wglGetCurrentDC();
        previousContext = wglGetCurrentContext();
        if (wglMakeCurrent (dc, context))
            return true;
        release();
        return false;
    }

    void release()
    {
        wglMakeCurrent (previousDC, previousContext);
        previousDC = nullptr;
        previousContext = nullptr;
    }
};

OffscreenGL::OffscreenGL() : impl (std::make_unique<Impl>()) {}
OffscreenGL::~OffscreenGL() = default;

bool OffscreenGL::create()                 { return impl->create(); }
bool OffscreenGL::isValid() const noexcept { return impl->context != nullptr; }
bool OffscreenGL::makeCurrent()            { return impl->makeCurrent(); }
void OffscreenGL::release()                { impl->release(); }

#elif defined (__APPLE__)

// macOS: a CGL context needs no window at all. It is only ever current while rendering into framebuffer
// objects, so nothing reaches the screen except the painted image, as on Windows. OpenGL is deprecated on
// macOS but still there (Apple Silicon included); if no 3.2 core context can be made, the views say so.
#ifndef GL_SILENCE_DEPRECATION
 #define GL_SILENCE_DEPRECATION 1
#endif
#include <OpenGL/OpenGL.h>

struct OffscreenGL::Impl
{
    CGLContextObj context = nullptr;
    CGLContextObj previous = nullptr;
    bool attempted = false;

    ~Impl()
    {
        if (context != nullptr)
        {
            if (CGLGetCurrentContext() == context)
                CGLSetCurrentContext (nullptr);
            CGLReleaseContext (context);
        }
    }

    bool create()
    {
        if (attempted)
            return context != nullptr;
        attempted = true;

        // hardware first (any GPU: AllowOfflineRenderers keeps a dual-GPU Mac on the GPU it is using), then
        // whatever renderer the system offers
        for (const bool accelerated : { true, false })
        {
            CGLPixelFormatAttribute attribs[16] {};
            int n = 0;
            attribs[n++] = kCGLPFAOpenGLProfile;
            attribs[n++] = (CGLPixelFormatAttribute) kCGLOGLPVersion_3_2_Core;
            attribs[n++] = kCGLPFAColorSize;
            attribs[n++] = (CGLPixelFormatAttribute) 24;
            attribs[n++] = kCGLPFAAlphaSize;
            attribs[n++] = (CGLPixelFormatAttribute) 8;
            attribs[n++] = kCGLPFADepthSize;
            attribs[n++] = (CGLPixelFormatAttribute) 24;
            attribs[n++] = kCGLPFAAllowOfflineRenderers;
            if (accelerated)
                attribs[n++] = kCGLPFAAccelerated;
            attribs[n] = (CGLPixelFormatAttribute) 0;

            CGLPixelFormatObj format = nullptr;
            GLint formats = 0;
            if (CGLChoosePixelFormat (attribs, &format, &formats) != kCGLNoError || format == nullptr)
                continue;
            const auto err = CGLCreateContext (format, nullptr, &context);
            CGLReleasePixelFormat (format);
            if (err == kCGLNoError && context != nullptr)
                return true;
            context = nullptr;
        }
        return false;
    }

    bool makeCurrent()
    {
        if (context == nullptr)
            return false;
        previous = CGLGetCurrentContext();
        if (CGLSetCurrentContext (context) == kCGLNoError)
            return true;
        release();
        return false;
    }

    void release()
    {
        CGLSetCurrentContext (previous);
        previous = nullptr;
    }
};

OffscreenGL::OffscreenGL() : impl (std::make_unique<Impl>()) {}
OffscreenGL::~OffscreenGL() = default;

bool OffscreenGL::create()                 { return impl->create(); }
bool OffscreenGL::isValid() const noexcept { return impl->context != nullptr; }
bool OffscreenGL::makeCurrent()            { return impl->makeCurrent(); }
void OffscreenGL::release()                { impl->release(); }

#else

struct OffscreenGL::Impl {};

OffscreenGL::OffscreenGL() : impl (std::make_unique<Impl>()) {}
OffscreenGL::~OffscreenGL() = default;

bool OffscreenGL::create()                 { return false; }
bool OffscreenGL::isValid() const noexcept { return false; }
bool OffscreenGL::makeCurrent()            { return false; }
void OffscreenGL::release()                {}

#endif
