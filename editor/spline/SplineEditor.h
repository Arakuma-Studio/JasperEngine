// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The stroke finalisation (simplify, smooth, snap, extend, close) and the marquee rules follow
// OpenEngine's Apps/Editor/Source/SceneView/SplineTool.cpp, Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT License. See NOTICE.md. Re-implemented on our commands.

#pragma once

#include "command/type/SplineEditCmd.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "spline/SplineTypes.h"
#include "util/TerrainSplineBake.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;
    struct SceneProject;

    // Where a drawn stroke's points land (S18 R26)
    enum class SplineSnap{
        None,       // on the drawing plane
        Terrain,    // on the terrain under each point
        Surface     // on whatever is drawn under it (triangles; else terrain, else bounds top)
    };

    enum class SplineDrawMode{
        Click,      // a knot per click, drag pulls Bezier tangents
        Freehand    // a stroke, simplified and smoothed into a CatmullRom spline
    };

    // The editor's spline tools (S18 R24-R26): the Edit Spline context (the knots of one spline
    // selected and moved in the Scene view, the inspector's knot table in sync), the Draw Spline
    // tool and Live Terrain. Every edit is one SplineEditCmd: an undo step named per D13. One
    // context for the editor: editing one spline at a time, as Unity's spline tool context.
    class SplineEditor{
    public:
        struct DrawSettings{
            SplineDrawMode mode = SplineDrawMode::Freehand;
            SplineSnap snap = SplineSnap::Terrain;
            float tolerance3D = 0.5f;       // simplify, world units
            float tolerance2DPixels = 4.0f; // simplify, screen pixels (2D)
            unsigned int smoothIterations = 2;
            float pickRadiusPixels = 12.0f; // a stroke starting this near an endpoint extends it
        };

    private:
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        bool editing = false;
        std::vector<uint32_t> selection;

        // the Draw Spline tool
        bool drawing = false;
        uint32_t drawSceneId = 0;
        Entity drawEntity = NULL_ENTITY;
        std::vector<Vector3> stroke;            // world
        std::vector<spline::SplineKnot> clicked; // Click mode, world
        DrawSettings drawSettings;

        bool liveTerrain = false;
        TerrainSplineLiveSession liveSession;
        std::string status;

        SplineEditor() = default;

    public:
        static SplineEditor& get();

        // ---- the Edit Spline context
        bool isEditing() const { return editing; }
        bool isEditing(uint32_t sceneId, Entity entity) const { return editing && this->sceneId == sceneId && this->entity == entity; }
        uint32_t getSceneId() const { return sceneId; }
        Entity getEntity() const { return entity; }
        void beginEdit(Project* project, uint32_t sceneId, Entity entity);
        // also ends the Live Terrain session (its one grouped patch)
        void endEdit(Project* project);

        const std::vector<uint32_t>& getSelection() const { return selection; }
        void setSelection(std::vector<uint32_t> knots);
        void toggleSelected(uint32_t knot);
        bool isSelected(uint32_t knot) const;
        void clearSelection() { selection.clear(); }
        // drops knots past the count (after an undo shortened the spline)
        void validateSelection(size_t knotCount);

        // ---- edits (each one undo step)
        // the whole definition; kind names the step, continuous merges a drag into one
        static bool setState(Project* project, uint32_t sceneId, Entity entity, SplineEditState state, SplineEditKind kind,
                             bool continuous = false, const std::vector<uint32_t>& selection = {});
        // the selected knots moved, rotated or scaled by a world transform about pivot; one
        // selected knot scaled: its radius scales (S18 R25)
        bool transformSelected(Project* project, const Matrix4& worldTransform, const Vector3& pivot, bool scaleRadius,
                               bool continuous, std::string* error = nullptr);
        // a handle of a Bezier knot (world position of its tip), the other per the knot's mode
        bool setTangent(Project* project, uint32_t knot, bool out, const Vector3& worldTip, bool continuous);
        // a knot on the curve where it passes nearest the world point (Ctrl+click)
        bool insertOnCurve(Project* project, const Vector3& world, std::string* error = nullptr);
        // a knot past an end (Ctrl+click beyond the end: the nearer end)
        bool extendTo(Project* project, const Vector3& world, std::string* error = nullptr);
        // the selection removed; refused (with a status message) below two knots
        bool deleteSelected(Project* project, std::string* error = nullptr);
        // the knots' world bounds (F frames them; all knots without a selection)
        AABB selectionBounds(Project* project) const;
        // world positions of the knots and the selection's centroid
        std::vector<Vector3> worldKnots(Project* project) const;
        Vector3 selectionCentroid(Project* project) const;
        // the history mark that ends a continuous edit
        static void endContinuousEdit(uint32_t sceneId);

        // ---- the Draw Spline tool
        DrawSettings& getDrawSettings() { return drawSettings; }
        bool isDrawing() const { return drawing; }
        uint32_t getDrawSceneId() const { return drawSceneId; }
        Entity getDrawEntity() const { return drawEntity; }
        void beginDraw(uint32_t sceneId, Entity entity);
        void cancelDraw();
        // Freehand: the stroke grows; Click: a knot (tangentOut for a dragged Bezier handle)
        void addStrokePoint(const Vector3& world);
        void addClickKnot(const Vector3& world, const Vector3& tangentOut);
        void setLastClickTangent(const Vector3& tangentOut);
        bool removeLastClickKnot();
        const std::vector<Vector3>& getStroke() const { return stroke; }
        const std::vector<spline::SplineKnot>& getClickKnots() const { return clicked; }
        // the stroke or the clicks into the spline as "Draw Spline": a stroke starting at an end
        // of the spline extends it, one ending on its start closes it. worldPerPixel converts the
        // 2D pixel tolerances.
        bool finishDraw(Project* project, float worldPerPixel, std::string* error = nullptr);

        // A stroke (world points) into a spline, as finishDraw and edit_spline stroke do it
        static bool applyStroke(Project* project, uint32_t sceneId, Entity entity, const std::vector<Vector3>& worldStroke,
                                float tolerance, unsigned int smooth, SplineSnap snap, bool extend, float pickRadius,
                                std::string* error = nullptr);
        // what a snap mode finds under a world point (S26 triangles, terrain, bounds top)
        static spline::SplineSurfaceProbe makeProbe(SceneProject* sceneProject, SplineSnap snap, Entity ignore);

        // ---- Live Terrain (S18 R12)
        bool isLiveTerrain() const { return liveTerrain; }
        void setLiveTerrain(Project* project, bool live);
        const TerrainSplineLiveSession& getLiveSession() const { return liveSession; }

        // per frame: the live re-bake, an edited spline that went away
        void update(Project* project);

        // the last refusal ("A spline keeps two knots at least.")
        const std::string& getStatus() const { return status; }
    };

}
