// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Bakes a tree prototype's impostor (T5 F7, R16, R49, R56): views around Y into a grid atlas, with an
// orthographic fitted camera in an isolated preview scene, one view per frame on the main thread,
// colour dilated past the alpha edge, PNGs written on a job through writeFileAtomic. Edit time only:
// no player code bakes. The op is registered here, so Lua's applyOps does not know it.

#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct ImpostorBakeResult {
        int prototype = 0;
        std::string albedoPath;
        std::string normalPath;
        float boundsCenter[3] = {0.0f, 0.0f, 0.0f};
        float size[2] = {0.0f, 0.0f};
        int views = 0;
        std::string sourceHash;
        double milliseconds = 0.0;
        std::string error;
    };

    enum class ImpostorState { None, Baked, OutOfDate };

    class ImpostorBaker {
    public:
        struct Request {
            uint32_t sceneId = 0;
            Entity terrain = NULL_ENTITY;
            std::vector<int> prototypes;    // indices
            int views = 8;
            int resolution = 256;
        };
        using Finished = std::function<void(const std::vector<ImpostorBakeResult>&, bool cancelled)>;
        using Progress = std::function<void(double share)>;

        // false with error when it cannot start. The scene stays usable: one view a frame.
        static bool start(Project* project, const Request& request, Finished finished, Progress progress, std::string& error);
        static void cancel();
        static bool isRunning();
        // Each frame, from the editor's update
        static void update();

        // The grid (includes: library/impostor.glsl keeps the same): 4 views 2x2, 8 views 4x2, 16 views 4x4
        static void atlasGrid(int views, int& columns, int& rows);
        // A hash of the mesh file: what Out of Date compares (R49)
        static std::string sourceHash(Project* project, const std::string& meshPath);
        static ImpostorState stateOf(Project* project, const std::string& meshPath, int views, const std::string& storedHash);

        // The results on their prototypes, as one undo step (R56)
        static void applyResults(Project* project, uint32_t sceneId, Entity terrain, const std::vector<ImpostorBakeResult>& results);
        // Registers the bake_impostors op in the terrain op engine (editor only)
        static void registerOp();
        // The bytes of a dilated RGBA image as PNG, in memory (R68: no stbi_write_png to a path)
        static bool encodePng(const std::vector<uint8_t>& rgba, int width, int height, std::vector<uint8_t>& png);
        // Colour past the alpha edge, so filtering at distance does not bring in the background
        static void dilate(std::vector<uint8_t>& rgba, int width, int height, int passes);
    };

}
