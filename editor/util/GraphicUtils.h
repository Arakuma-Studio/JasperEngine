// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "object/Camera.h"
#include "render/FramebufferRender.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace fs = std::filesystem;

namespace jasperengine::editor {
    class GraphicUtils {
    public:
        static bool saveFramebufferImage(Framebuffer* framebuffer, fs::path path, bool flipY = false, std::function<void()> onComplete = nullptr);

        // The framebuffer's color as a PNG in memory (RGB, alpha dropped), for screenshots
        // sent over MCP. Reads the GPU texture back synchronously: main thread, after the
        // frame that drew it. maxWidth > 0 scales a wider image down to it (aspect kept).
        // flipY: false for both the Scene view (SceneRender::getFramebuffer) and the Game
        // view (GameWindow::getFramebuffer) on every backend: the engine draws render
        // targets top row first, as ImGui shows them unflipped. Returns false when the
        // framebuffer is not created or the readback fails.
        static bool encodeFramebufferPng(Framebuffer* framebuffer, std::vector<uint8_t>& out, bool flipY = false,
                                         int maxWidth = 0, int* outWidth = nullptr, int* outHeight = nullptr);

        // A colour target's pixels, RGBA8 rows top first (the Frame Debugger's pass preview read
        // back for MCP). Synchronous, main thread. False when it is not created or the read fails.
        static bool readRenderPixels(FramebufferRender* render, int width, int height, std::vector<uint8_t>& rgba);

        // The framebuffer's colour as width * height RGBA8, rows top first (the engine draws
        // render targets top row first on every backend). Synchronous GPU readback: main
        // thread, after the draw. False when the framebuffer is not created or the readback fails.
        static bool readFramebufferRGBA(Framebuffer* framebuffer, std::vector<uint8_t>& out);

        static Vector2 getRectTransformCenter(Scene* scene, Entity entity, const RectTransformComponent& layout);
    };
}
