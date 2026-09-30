// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "object/Camera.h"
#include <filesystem>

namespace fs = std::filesystem;

namespace jasperengine::editor {
    class GraphicUtils {
    public:
        static bool saveFramebufferImage(Framebuffer* framebuffer, fs::path path, bool flipY = false, std::function<void()> onComplete = nullptr);

        static Vector2 getRectTransformCenter(Scene* scene, Entity entity, const RectTransformComponent& layout);
    };
}
