// CPU time of the calling thread, for the snapshot tool's cost estimates (kept in its own file so that
// <windows.h> stays out of the JUCE code).
#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>

double threadCpuMs()
{
    FILETIME created, exited, kernel, user;
    if (! GetThreadTimes (GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0.0;
    auto ms = [] (const FILETIME& f) { return (double) (((ULONGLONG) f.dwHighDateTime << 32) | f.dwLowDateTime) * 1.0e-4; };
    return ms (kernel) + ms (user);
}
#else
 #include <ctime>

double threadCpuMs() { return 1000.0 * (double) std::clock() / CLOCKS_PER_SEC; }
#endif
