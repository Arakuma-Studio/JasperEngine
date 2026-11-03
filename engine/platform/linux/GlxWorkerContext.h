// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GlxWorkerContext_h
#define GlxWorkerContext_h

// The GLX worker context of the asynchronous shader creation (spec S32, GpuWorkerContext): a
// context in the render context's share group, made with the same attributes (GLX refuses a share
// list across different robustness strategies), current on the creation thread over a 1x1 pbuffer
// (or no drawable, which GLX_ARB_create_context allows for 3.0+ contexts, when the configuration
// has no pbuffers). Header-only: the editor's backend, the Linux player and the test context
// create one each. The display must be thread-safe (XInitThreads before it was opened).

#include "render/GpuWorkerContext.h"

#include <GL/glx.h>

#include <cstdio>
#include <memory>

namespace jasperengine {

    class GlxWorkerContext : public GpuWorkerContext {
    public:
        using CreateContextAttribsProc = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

        // Render thread, with `share` current; null when GLX refuses it
        static std::unique_ptr<GpuWorkerContext> create(Display* display, GLXFBConfig config, GLXContext share,
                                                        const int* attributes){
            if (!display || !config || !share)
                return nullptr;
            auto createContext = reinterpret_cast<CreateContextAttribsProc>(
                glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));
            if (!createContext)
                return nullptr;
            XSync(display, False);
            failed() = false;
            XErrorHandler previous = XSetErrorHandler(recordError);
            GLXContext context = createContext(display, config, share, True, attributes);
            XSync(display, False);
            GLXPbuffer pbuffer = 0;
            int drawableType = 0;
            if (context && !failed() && glXGetFBConfigAttrib(display, config, GLX_DRAWABLE_TYPE, &drawableType) == Success &&
                    (drawableType & GLX_PBUFFER_BIT)){
                const int pbufferAttributes[] = {GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, None};
                pbuffer = glXCreatePbuffer(display, config, pbufferAttributes);
                XSync(display, False);
                if (failed()){
                    // no pbuffer: the context is made current without a drawable
                    pbuffer = 0;
                    failed() = false;
                }
            }
            XSetErrorHandler(previous);
            if (!context || failed()){
                if (context)
                    glXDestroyContext(display, context);
                std::fprintf(stderr, "GLX: no worker context in the render context's share group\n");
                return nullptr;
            }
            return std::unique_ptr<GpuWorkerContext>(new GlxWorkerContext(display, context, pbuffer));
        }

        ~GlxWorkerContext() override {
            if (pbuffer)
                glXDestroyPbuffer(display, pbuffer);
            glXDestroyContext(display, context);
        }

        bool makeCurrent() override {
            return glXMakeContextCurrent(display, pbuffer, pbuffer, context) == True;
        }

        void release() override {
            glXMakeContextCurrent(display, None, None, nullptr);
        }

    private:
        GlxWorkerContext(Display* display, GLXContext context, GLXPbuffer pbuffer)
            : display(display), context(context), pbuffer(pbuffer) {}

        static bool& failed(){
            static bool value = false;
            return value;
        }
        static int recordError(Display*, XErrorEvent*){
            failed() = true;
            return 0;
        }

        Display* display;
        GLXContext context;
        GLXPbuffer pbuffer;
    };

}

#endif /* GlxWorkerContext_h */
