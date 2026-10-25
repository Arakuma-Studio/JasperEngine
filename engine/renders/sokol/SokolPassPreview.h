// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolPassPreview_h
#define SokolPassPreview_h

// The Frame Debugger's pass output preview (spec S15 R12, §6.7). A request names a pass by its
// view, label and occurrence in the frame ("the 2nd 'shadow' pass of the game frame"), one of
// its attachments and a mode. Our pass-ending call sites (SokolCamera::endRenderPass, the
// queued END_PASS of SokolCmdQueue, SokolFramebuffer's clear pass) call afterEndPass(); only
// when the pass that just ended matches does it add one blit pass that copies the attachment
// into a persistent RGBA8 target of at most 1024 px on its longest side (PASS_PREVIEW shader:
// colour, alpha, depth or one channel). MSAA colour attachments are read from their resolve
// image; MSAA depth without one and swapchain passes are unavailable, with the reason. A
// request is one-shot: the editor asks again (at most 10 Hz) while the Frame Debugger is live.
// Main thread (the sokol thread) only.

#include "render/RenderStats.h"

#include <cstdint>
#include <string>

namespace jasperengine{

    class TextureRender;

    class SokolPassPreview{
    public:
        static void request(const PassPreviewRequest& request);
        static void cancel();
        static bool isRequested();
        static PassPreviewResult result();
        // the preview target's colour (the editor shows it, MCP reads it back); null before the first
        static TextureRender* texture();

        // our pass-ending sites: right after sg_end_pass
        static void afterEndPass();
        // SokolSystem::shutdown
        static void destroy();
    };

}

#endif //SokolPassPreview_h
