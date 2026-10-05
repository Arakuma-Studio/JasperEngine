// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "object/Camera.h"
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

        static Vector2 getRectTransformCenter(Scene* scene, Entity entity, const RectTransformComponent& layout);
    };
}
