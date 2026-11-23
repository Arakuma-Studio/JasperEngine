// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Frame checks (docs/specs/S99-agent-feedback-area-lights.md R6-R9): what is wrong with what a
// Scene or Game view shows, each with the call that fixes it. The screenshot tool returns them as
// findings; the Scene view's Frame Checks overlay draws them. Editor-only CPU work on the main
// thread: the image's luminance, the picking BVH's frustum query and bounds, the S04 missing-asset
// tracker and the meshes' triangle BVHs.

#include "Entity.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Plane.h"
#include "math/Vector3.h"
#include "registry/TagManager.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::framechecks {

    // R7's thresholds, on the image as shown (sRGB-encoded luminance, 0..1)
    inline constexpr float CLIPPED_LUMINANCE = 0.98f;
    inline constexpr float CLIPPED_SHARE = 0.20f;
    inline constexpr float DARK_MEAN = 0.04f;
    inline constexpr float DARK_LUMINANCE = 0.02f;
    inline constexpr float DARK_SHARE = 0.80f;
    inline constexpr float OUT_OF_FRAME_SHARE = 0.50f;
    inline constexpr float TOO_SMALL_SHARE = 0.01f;
    // metres: faces this close to one plane and overlapping by more than this z-fight
    inline constexpr float COPLANAR_TOLERANCE = 0.001f;
    // same-facing: the faces' normals within about 0.8 degrees
    inline constexpr float COPLANAR_COS = 0.9999f;
    inline constexpr size_t MAX_FINDINGS = 5;
    // R8: the cost aimed at on the fixture project's main scene, and the hard budget past which
    // the remaining checks are skipped (one info finding names them)
    inline constexpr double TARGET_MS = 10.0;
    inline constexpr double HARD_BUDGET_MS = 30.0;
    // the Scene view overlay recomputes at most this often (R9)
    inline constexpr double OVERLAY_REFRESH_SECONDS = 0.5;

    struct Luminance {
        bool valid = false;
        size_t samples = 0;
        float mean = 0.0f;
        float clippedShare = 0.0f;  // at or above CLIPPED_LUMINANCE
        float darkShare = 0.0f;     // under DARK_LUMINANCE
    };
    // `pixels`: rows top first, `channels` bytes each (RGB or RGBA, sRGB); the rectangle
    // (rx, ry, rw, rh) of it (rw or rh 0: the whole image). Sampled on a grid of at most 2^18 pixels.
    Luminance measureLuminance(const uint8_t* pixels, int width, int height, int channels,
                               int rx = 0, int ry = 0, int rw = 0, int rh = 0);

    enum class Severity { WARNING, INFO };
    const char* severityName(Severity severity);

    struct Finding {
        std::string kind;
        Severity severity = Severity::WARNING;
        std::string message;
        std::string fix;
        std::vector<Entity> entities;
    };

    struct Request {
        Project* project = nullptr;
        SceneProject* sceneProject = nullptr;
        // the camera the view draws through, and the scene holding it
        Scene* cameraScene = nullptr;
        Entity camera = NULL_ENTITY;
        // the Scene view (frame_entity fixes, the GameObjects it hides left out) or the Game view
        // (the game camera's fixes)
        bool sceneView = true;
        uint32_t layerMask = LAYER_MASK_EVERYTHING;
        Luminance luminance;
        // recompute the missing-asset tracker with the unsaved edits (a screenshot call); the overlay
        // reads it as the status bar does
        bool refreshMissingAssets = false;
        double budgetMs = HARD_BUDGET_MS;
    };

    struct Result {
        // most severe first, at most MAX_FINDINGS
        std::vector<Finding> findings;
        double ms = 0.0;
    };

    Result run(const Request& request);

    // ---- the parts, for the tests

    struct Triangle {
        Vector3 a;
        Vector3 b;
        Vector3 c;
    };

    // a and b lie on one plane (every corner of one within `tolerance` of the other's plane),
    // face the same way and overlap by more than `tolerance` inside it
    bool coplanarOverlap(const Triangle& a, const Triangle& b, float tolerance);

    // One mesh's faces near a region, bucketed by plane: the dominant axis and side of the normal
    // and a grid cell across that axis, each entry with the plane's height along the axis at the
    // cell's centre. A face of another mesh is tested only against the faces of its cells whose
    // height is close to its own there; faces spanning too many cells are kept apart and tested
    // against every face of their side.
    class PlaneBuckets {
    public:
        // origin, radius: the region the faces come from (a pair's overlapping bounds)
        PlaneBuckets(float tolerance, const Vector3& origin, float radius);
        void build(std::span<const Triangle> triangles);
        // the built face `triangle` z-fights with (coplanarOverlap), -1 for none
        int findOverlap(const Triangle& triangle) const;
        Vector3 normalOf(int face) const { return faces[(size_t)face].normal; }
        size_t size() const { return faces.size(); }

    private:
        struct Face {
            Triangle triangle;
            Vector3 normal;
            float distance = 0.0f;  // of its plane from the origin
        };
        struct Cell {
            int group = 0;
            int32_t x = 0;
            int32_t y = 0;
            bool operator==(const Cell& other) const { return group == other.group && x == other.x && y == other.y; }
        };
        struct CellHash {
            size_t operator()(const Cell& cell) const;
        };
        struct Entry {
            uint32_t face = 0;
            float height = 0.0f;
        };
        float tolerance;
        Vector3 origin;
        float radius;
        float cellSize = 1.0f;     // the grid across the axis
        float heightSlack = 1.0f;  // the most two faces that meet in a cell differ in height there
        std::vector<Face> faces;
        // a cell's entries: [first, first + count) of `entries`
        std::unordered_map<Cell, std::pair<uint32_t, uint32_t>, CellHash> cells;
        std::vector<Entry> entries;
        std::vector<uint32_t> wide[6];
        // a face's last query, so a face met in several cells is tested once
        mutable std::vector<uint32_t> testedBy;
        mutable uint32_t queries = 0;
        // the cells a face covers across `axis`, false when they are too many
        bool cellsOf(const Triangle& triangle, int axis, int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1) const;
        float heightAt(const Vector3& normal, float distance, int axis, int32_t x, int32_t y) const;
        bool test(uint32_t face, const Triangle& triangle, const Vector3& normal) const;
    };

    // Where a world box lands in a view (NDC [-1, 1]): the share of its projected rectangle outside
    // the image and the share of the image it covers. Corners behind the eye are clipped at it, so a
    // box around the eye reads as mostly outside. planes: the view's frustum (a box outside one is
    // wholly out), or null.
    struct Projection {
        bool inFront = false;       // every corner in front of the eye
        float outsideShare = 1.0f;
        float coverage = 0.0f;
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    };
    Projection projectBounds(const Matrix4& viewProjection, const AABB& box, const Plane* planes = nullptr);

}
