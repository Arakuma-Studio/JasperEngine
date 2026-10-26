// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef Profile_h
#define Profile_h

// The profiling macros (spec S15 R13). With JASPERENGINE_PROFILER=0 (release games) every
// macro is ((void)0) and evaluates none of its arguments; compiled in, a scope costs one relaxed
// load and a branch while the profiler is off.
//
//     JE_PROFILE_SCOPE("Shadows");               // a string literal
//     JE_PROFILE_FUNCTION();                     // the enclosing function's name
//     JE_PROFILE_SCOPE_DYN(script.getName());    // a run-time name (interned)
//     JE_PROFILE_THREAD("je-video");             // names the calling thread
//     JE_PROFILE_FRAME();                        // the frame boundary (Engine only)
//     JE_PROFILE_COUNTER("Draw calls", draws);   // a monitor's value this frame
//     JE_GPU_ALLOC_SCOPE(PostFX, "Bloom");       // what the GPU resources made in scope are for
//
// With JASPERENGINE_TRACY the scopes are Tracy zones too, the frame mark is Tracy's FrameMark
// and counters are Tracy plots (desktop and Android, docs/profiling.md).

#ifndef JASPERENGINE_PROFILER
    #define JASPERENGINE_PROFILER 0
#endif

#if JASPERENGINE_PROFILER

#include "profiler/GpuMemory.h"
#include "profiler/Profiler.h"

#if JASPERENGINE_TRACY
    #include "tracy/Tracy.hpp"
    #define JE_PROFILE_TRACY_ZONE(name) ZoneScopedN(name)
    #define JE_PROFILE_TRACY_FUNCTION() ZoneScoped
    #define JE_PROFILE_TRACY_FRAME() FrameMark
    #define JE_PROFILE_TRACY_PLOT(name, value) TracyPlot(name, (double)(value))
#else
    #define JE_PROFILE_TRACY_ZONE(name)
    #define JE_PROFILE_TRACY_FUNCTION()
    #define JE_PROFILE_TRACY_FRAME()
    #define JE_PROFILE_TRACY_PLOT(name, value)
#endif

#define JE_PROFILE_CONCAT_INNER(a, b) a##b
#define JE_PROFILE_CONCAT(a, b) JE_PROFILE_CONCAT_INNER(a, b)

#define JE_PROFILE_SCOPE(name) \
    static ::jasperengine::profiler::ScopeSite JE_PROFILE_CONCAT(jeProfileSite_, __LINE__){name, __FILE__, (uint32_t)__LINE__}; \
    ::jasperengine::profiler::Scope JE_PROFILE_CONCAT(jeProfileScope_, __LINE__)(JE_PROFILE_CONCAT(jeProfileSite_, __LINE__)); \
    JE_PROFILE_TRACY_ZONE(name)
#define JE_PROFILE_FUNCTION() \
    static ::jasperengine::profiler::ScopeSite JE_PROFILE_CONCAT(jeProfileSite_, __LINE__){__func__, __FILE__, (uint32_t)__LINE__}; \
    ::jasperengine::profiler::Scope JE_PROFILE_CONCAT(jeProfileScope_, __LINE__)(JE_PROFILE_CONCAT(jeProfileSite_, __LINE__)); \
    JE_PROFILE_TRACY_FUNCTION()
// (with Tracy, DynScope opens a transient zone of the same name)
#define JE_PROFILE_SCOPE_DYN(name) \
    ::jasperengine::profiler::DynScope JE_PROFILE_CONCAT(jeProfileDyn_, __LINE__)(name, __FILE__, (uint32_t)__LINE__, __func__)
#define JE_PROFILE_THREAD(name) ::jasperengine::profiler::setThreadName(name)
#define JE_PROFILE_FRAME() do { ::jasperengine::profiler::frameMark(); JE_PROFILE_TRACY_FRAME(); } while (0)
#define JE_PROFILE_COUNTER(name, value) do { ::jasperengine::profiler::counter(name, (double)(value)); JE_PROFILE_TRACY_PLOT(name, value); } while (0)
#define JE_GPU_ALLOC_SCOPE(category, owner) \
    ::jasperengine::gpumem::Scope JE_PROFILE_CONCAT(jeGpuAlloc_, __LINE__)(::jasperengine::gpumem::Category::category, owner)

#else

#define JE_PROFILE_SCOPE(name) ((void)0)
#define JE_PROFILE_FUNCTION() ((void)0)
#define JE_PROFILE_SCOPE_DYN(name) ((void)0)
#define JE_PROFILE_THREAD(name) ((void)0)
#define JE_PROFILE_FRAME() ((void)0)
#define JE_PROFILE_COUNTER(name, value) ((void)0)
#define JE_GPU_ALLOC_SCOPE(category, owner) ((void)0)

#endif

#endif /* Profile_h */
