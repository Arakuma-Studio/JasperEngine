// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Wiring a generated normal map onto a GameObject as ONE step of its scene's command history
// (the Save & Assign button, MCP apply): every submesh whose base colour texture is the map's
// colour source gets normalTexture = the map, with the colour texture's min/mag filter and wrap
// copied onto it. For an animated source (GIF/APNG atlas) the Sprite also gets the colour atlas
// as its base texture (filter and wrap kept), the frames (frame_0..), its size set to one frame,
// frame 0 shown, and a Sprite Animation child (new, or the existing one targeting it) cycling
// the frames with the file's delays (playbackMs) and loop (loopCount 1 -> no loop), autoplay
// true when created. Refused while the scene plays. Main thread.

#include "SpriteNormals.h"
#include "SpriteMedia.h"

#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::spritenormals {

    struct ApplyRequest {
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        std::string normalPath;              // project-relative, written already
        std::string colorSourcePath;         // the texture the map was made from (matches submeshes by it)
        struct Map {
            std::string normalPath;
            std::string colorSourcePath;
        };
        std::vector<Map> maps;               // stills: every (normal, colour) pair, all applied in one step
                                             // (normalPath/colorSourcePath are maps[0] when maps is empty)
        // Animated sources: the colour atlas becomes the base texture and these the Sprite's frames
        std::string atlasPath;               // "" for stills
        std::vector<Cell> frames;
        std::vector<int> frameMs;            // one per frame (Delay::playbackMs)
        int loopCount = 0;
        std::string animationName = "Sprite Animation";
    };

    struct ApplyResult {
        bool ok = false;
        Problem error;                       // scene_playing, not_a_sprite, size_mismatch, too_many_frames
        std::vector<Problem> warnings;       // material_linked
        std::vector<int> submeshes;          // the submeshes given the map
        Entity animation = NULL_ENTITY;      // the Sprite Animation made or updated
        bool createdAnimation = false;
        std::string undoName;                // "Assign Normal Map" / "Assign Animated Sprite"
    };

    ApplyResult applyToSprite(Project* project, const ApplyRequest& request);
    // One Scene undo step for all (one scene); all or nothing: the first refusal undoes what was
    // added and is the result's error
    ApplyResult applyToSprites(Project* project, const std::vector<ApplyRequest>& requests);
}
