// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef StateHash_h
#define StateHash_h

// The state a recording's checkpoints sample (docs/specs/input-recorder-and-input-scripts.md R17):
// a hash of the running scenes' transforms and the game's checkpoint values, and the positions of
// a few probe GameObjects. Detection, not correction: a replay compares them to say where it
// diverged.

#include "Export.h"
#include "ecs/Entity.h"
#include "input/InputScript.h"
#include "math/Vector3.h"

#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::input {

    // a coordinate as the hash sees it: 1/1024 steps, so a last-bit difference hashes equal
    JASPERENGINE_API int32_t quantizeState(float value);

    // XXH3-64 over each scene (in order), each entity with a Transform (id order): id, active in
    // hierarchy, world position, rotation and scale (quantized), then the values (name order;
    // numbers quantized, strings as bytes, booleans as 0/1, arrays element by element)
    JASPERENGINE_API uint64_t hashState(const std::vector<Scene*>& scenes, const ScriptJson& values);

    struct StateProbe {
        std::string path;           // "Enemies/Bat (3)"
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
    };

    // the GameObject at each path (the first scene that has it); a path that matches nothing keeps
    // no entity (its position reads 0, 0, 0)
    JASPERENGINE_API std::vector<StateProbe> findProbes(const std::vector<Scene*>& scenes, const std::vector<std::string>& paths);
    // the first `count` by id with a Rigidbody, Character Controller, NavMesh Agent or script
    JASPERENGINE_API std::vector<StateProbe> chooseProbes(const std::vector<Scene*>& scenes, size_t count = 16);
    JASPERENGINE_API Vector3 probePosition(const StateProbe& probe);
    // "Parent/Child" of a GameObject
    JASPERENGINE_API std::string entityPath(Scene* scene, Entity entity);

}

#endif
