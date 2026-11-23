// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WglWorkerContext_h
#define WglWorkerContext_h

// The WGL worker context of the asynchronous shader creation (spec S32, GpuWorkerContext): a
// context in the render context's share group (wglCreateContextAttribsARB with it as the share
// context and the same attributes), current on the creation thread over the device context of a
// hidden 1x1 window with the render window's pixel format (a window's DC is not shared between
// threads). Header-only: the editor's backend and the Windows player create one each.

#include "render/GpuWorkerContext.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <memory>

namespace jasperengine {

    class WglWorkerContext : public GpuWorkerContext {
    public:
        using CreateContextAttribsProc = HGLRC (WINAPI*)(HDC, HGLRC, const int*);

        // Render thread, with `share` current on a DC of pixel format `pixelFormat`
        static std::unique_ptr<GpuWorkerContext> create(HGLRC share, int pixelFormat, const PIXELFORMATDESCRIPTOR& descriptor,
                                                        const int* attributes){
            PROC proc = wglGetProcAddress("wglCreateContextAttribsARB");
            if (!share || pixelFormat <= 0 || !proc)
                return nullptr;
            auto createContext = reinterpret_cast<CreateContextAttribsProc>(proc);
            HWND window = CreateWindowExW(0, L"STATIC", L"jasperengine-gpu-worker", WS_POPUP, 0, 0, 1, 1,
                                          nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!window)
                return nullptr;
            HDC deviceContext = GetDC(window);
            HGLRC context = nullptr;
            if (deviceContext && SetPixelFormat(deviceContext, pixelFormat, &descriptor))
                context = createContext(deviceContext, share, attributes);
            if (!context){
                if (deviceContext)
                    ReleaseDC(window, deviceContext);
                DestroyWindow(window);
                std::fprintf(stderr, "WGL: no worker context in the render context's share group\n");
                return nullptr;
            }
            return std::unique_ptr<GpuWorkerContext>(new WglWorkerContext(window, deviceContext, context));
        }

        ~WglWorkerContext() override {
            wglDeleteContext(context);
            ReleaseDC(window, deviceContext);
            DestroyWindow(window);
        }

        bool makeCurrent() override {
            return wglMakeCurrent(deviceContext, context) != FALSE;
        }

        void release() override {
            wglMakeCurrent(nullptr, nullptr);
        }

    private:
        WglWorkerContext(HWND window, HDC deviceContext, HGLRC context)
            : window(window), deviceContext(deviceContext), context(context) {}

        HWND window;
        HDC deviceContext;
        HGLRC context;
    };

}

#endif /* WglWorkerContext_h */
