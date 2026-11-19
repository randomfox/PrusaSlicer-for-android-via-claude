#pragma once

#if defined(__ANDROID__) || defined(__linux__)
#include <malloc.h>
#endif

#include <atomic>

namespace Slic3r::Biz::Utils {

inline std::atomic<int>& pending_memory_releases()
{
    static std::atomic<int> frames{0};
    return frames;
}

/// Returns memory freed by large deallocations (a closed project, a replaced G-code preview)
/// to the system: the allocators otherwise keep around 100 MB of it in their caches.
inline void release_free_memory_now()
{
#if defined(__ANDROID__)
#ifdef M_PURGE
    ::mallopt(M_PURGE, 0); // Scudo (Android 11+); a no-op elsewhere
#endif
#elif defined(__linux__) && defined(__GLIBC__)
    ::malloc_trim(0);
#endif
}

/// Releases now and again after the next frames: views free their copies of the data (GPU
/// buffers, toolpaths) when they draw next.
inline void release_free_memory()
{
    release_free_memory_now();
    pending_memory_releases() = 3;
}

/// Called by the app after each drawn frame.
inline void release_free_memory_after_frame()
{
    auto& pending = pending_memory_releases();
    if (pending.load() > 0 && --pending == 0) {
        release_free_memory_now();
    }
}

} // namespace Slic3r::Biz::Utils
