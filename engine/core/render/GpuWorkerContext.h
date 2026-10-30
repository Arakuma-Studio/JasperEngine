// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GpuWorkerContext_h
#define GpuWorkerContext_h

#include "Export.h"

#include <functional>
#include <memory>

namespace jasperengine {

    // A GL context in the render context's share group, for the thread that creates GPU programs
    // off the render thread (spec S32, SokolAsyncCreate). Its programs, textures, buffers and
    // samplers are the render context's; its VAOs and framebuffers are its own.
    class GpuWorkerContext {
    public:
        virtual ~GpuWorkerContext() = default;
        // On the worker thread, once before its first GL call; false: unusable (the creation
        // falls back to the render thread's context)
        virtual bool makeCurrent() = 0;
        // On the worker thread, after its last GL call
        virtual void release() = 0;
    };

    // Called on the render thread with its context current, when the render system is set up.
    // A platform without shared contexts (the web) sets none, or returns null.
    using GpuWorkerContextFactory = std::function<std::unique_ptr<GpuWorkerContext>()>;

    // The platform's factory, set before Engine::systemViewLoaded (null: none)
    JASPERENGINE_API void setGpuWorkerContextFactory(GpuWorkerContextFactory factory);
    JASPERENGINE_API const GpuWorkerContextFactory& getGpuWorkerContextFactory();

}

#endif /* GpuWorkerContext_h */
