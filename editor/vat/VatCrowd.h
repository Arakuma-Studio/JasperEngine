// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 5.4)
// - Purpose: Create Crowd: one GameObject with Model (the .vat.glb) + Instanced Mesh (the
//   instances) + Vertex Animator (the .vat), its instances scattered over an area, out of step
//   (random start), each with its own clip (weighted pick) and speed (instanceClips /
//   instanceSpeeds, applied when the animator starts). One scene undo step. The Inspector's
//   dialog, the MCP tools and the baker's "Place: Crowd" call createCrowd; layoutCrowd is the pure
//   part (tests, previews).
// - Owner: W5. Thread-safety: layoutCrowd is pure; createCrowd runs on the main thread.
// - Placement: instances are in the GameObject's space, the GameObject at `center`. Each point is
//   dropped onto the ground: with a `surface` GameObject, a ray straight down onto its mesh (its
//   triangles: UIWorldRaycast; a Terrain: its height field), else onto its world bounds' top; with
//   none, or where the ray misses, the ground plane y = center.y (no physics query: placement does
//   not need the physics world running). Minimum spacing is Poisson-disc style rejection: up to
//   `triesPerInstance` candidates per instance, an instance that finds no free spot is not placed
//   (`placed` < `count`, said in the message).
// - Determinism: the same settings (seed included) give the same instances, bit for bit, on every
//   machine (a fixed 64-bit generator, no std:: distributions).

#pragma once

#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{
    class Project;
}

namespace jasperengine::editor::vat{

    enum class CrowdArea { Box, Disc };

    struct CrowdClipWeight{
        std::string clip;
        float weight = 1.0f;        // relative (70 / 30 and 0.7 / 0.3 are the same)
    };

    struct CrowdSettings{
        std::string vat;            // the .vat (asset-relative, project-relative or absolute)
        int count = 100;            // 1 .. MAX_CROWD
        CrowdArea area = CrowdArea::Box;
        float width = 10.0f;        // box: along X; disc: the diameter
        float depth = 10.0f;        // box: along Z (unused for a disc)
        Vector3 center = Vector3(0, 0, 0);   // world position of the area's center (the GameObject's)
        Entity surface = NULL_ENTITY;   // the GameObject to stand on; NULL_ENTITY: the ground plane
        float minSpacing = 0.5f;    // metres between instance origins (0: no check)
        int triesPerInstance = 30;
        bool randomYaw = true;
        float scaleMin = 1.0f;
        float scaleMax = 1.0f;
        std::vector<CrowdClipWeight> clipWeights;   // empty: every instance plays the default clip
        float speedMin = 1.0f;
        float speedMax = 1.0f;
        bool randomStart = true;
        uint32_t seed = 1;
        bool instanceEvents = false;
        std::string name;           // the GameObject; empty: "<stem> Crowd"
        Entity parent = NULL_ENTITY;
    };

    constexpr int MAX_CROWD = 100000;

    struct CrowdInstanceSpec{
        Vector3 position;           // GameObject space
        Quaternion rotation;
        Vector3 scale = Vector3(1, 1, 1);
        std::string clip;           // "" = the default clip
        float speed = 1.0f;
    };

    struct CrowdLayout{
        std::vector<CrowdInstanceSpec> instances;
        int requested = 0;
        int placed = 0;
    };

    // `groundHeight(x, z, y)`: the world height under (x, z), false for none (then center.y).
    // The settings are taken as given (createCrowd validates them first)
    CrowdLayout layoutCrowd(const CrowdSettings& settings, const std::function<bool(float x, float z, float& y)>& groundHeight);

    // The problems that stop a crowd ("count must be 1..100000", an unknown clip in the
    // weights, a negative weight or no positive one, min > max...), "" when none. `clipNames`:
    // the .vat's clips
    std::string validateCrowd(const CrowdSettings& settings, const std::vector<std::string>& clipNames);

    struct CrowdResult{
        bool ok = false;
        Entity entity = NULL_ENTITY;
        int requested = 0;
        int placed = 0;
        std::string message;        // "Placed 480 of 500 (minimum spacing 0.5 m left no room for 20)"; the error when !ok
    };

    // Validates, lays the crowd out and creates the GameObject as one scene undo step (selected)
    CrowdResult createCrowd(Project* project, uint32_t sceneId, const CrowdSettings& settings);

}
