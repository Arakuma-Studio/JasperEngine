// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R30-R35, R50-R55, R100-R110: the Vertex Paint tools in the Scene view.
// VertexPaintTools is the state the window, the Scene view and the shortcuts share (the settings, the
// armed tool, the stroke, the cursor); VertexPaintSceneTool is the SceneTool (S50 D9) that takes the
// view's left button while a tool is armed. Every write goes through the editor facade
// (VertexColorEditing.h): a stroke is a StrokeSession, everything else one applyEdit.

#include "ecs/Entity.h"
#include "render/ViewDrawMode.h"
#include "scenetool/SceneTool.h"
#include "vertexcolor/VertexColorEditing.h"
#include "vertexcolor/VertexPaintBrush.h"
#include "voxel/VoxelBrush.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::vertexcolor {

    class VertexPaintSceneTool;

    // a point of a painted surface under a ray
    struct TargetHit {
        bool hit = false;
        TargetRef target;
        std::string name;               // the entity's name and the target's
        uint32_t triangle = 0;
        Vector3 barycentric;
        Vector3 point;                  // world, bind pose (D12)
        Vector3 normal;                 // world, unit, facing the ray
        float distance = 0.0f;
    };

    class VertexPaintTools {
    public:
        static VertexPaintTools& get();

        void setProject(Project* project);
        Project* project() const { return projectPtr; }
        // drops the tool, the stroke and the remembered view (project switch); settings stay
        void reset();

        VertexPaintSettings& settings() { return brush; }
        const VertexPaintSettings& settings() const { return brush; }

        // ---- arming (R102): the selected scene's view gets the tool; its Draw Mode becomes Vertex
        // Color (RGB), given back on exit unless changed meanwhile
        void arm(PaintTool tool);
        void armIn(uint32_t sceneId, PaintTool tool);
        void exit();
        bool isArmed() const { return activeTool != nullptr; }
        bool isArmedIn(uint32_t sceneId) const;
        uint32_t armedScene() const { return toolScene; }
        PaintTool currentTool() const { return brush.tool; }

        // ---- the View row (R101, R102): a channel, or Off (the Draw Mode the tool found)
        void setViewChannel(ViewVertexColorChannel channel);
        void setViewOff();
        // Shift+V
        void cycleViewChannel();
        bool viewIsOff() const;

        // ---- what the tools act on (R110): every selected entity that can be painted, each target
        std::vector<TargetRef> targets(uint32_t sceneId) const;
        // the window's Target combo: -1 all, else an index in targets()
        int targetFilter = -1;
        std::vector<TargetRef> actingTargets(uint32_t sceneId) const;
        // the nearest hit of a ray on the targets (posed meshes through MeshRaycast, mapped to bind pose)
        TargetHit hitTargets(uint32_t sceneId, const Ray& ray) const;

        // ---- strokes (R30, R31, R106, R107)
        bool beginStroke(uint32_t sceneId, const Ray& ray, bool ctrl, bool shift, float pressure);
        void continueStroke(const Ray& ray, float pressure);
        void endStroke();
        void cancelStroke();
        bool isStrokeActive() const { return stroke.active; }
        vc::StrokeMode strokeMode() const { return stroke.mode; }

        // no mouse: one stroke through world points, the same ops a drag through them sends, one undo
        // step (AC8). False with `why`.
        static bool scriptedStroke(Project* project, uint32_t sceneId, const std::vector<TargetRef>& targets, const VertexPaintSettings& settings,
                                   vc::StrokeMode mode, bool secondary, const std::vector<vc::StrokePoint>& points, std::string& why);

        // ---- clicks of the other tools
        // R108: the stored value under the ray into the primary (secondary) colour
        bool pickAt(uint32_t sceneId, const Ray& ray, bool secondary);
        // Fill: the selection of the hit target, else its connected part; ctrl: the faces of the clicked colour
        bool fillAt(uint32_t sceneId, const Ray& ray, bool ctrl, bool secondary);
        // Gradient: primary at `from`, secondary at `to`, over the selection (else all) of every target
        bool gradient(uint32_t sceneId, const Vector3& from, const Vector3& to);

        // ---- element selection (R50-R53)
        // R51 click: the element under the ray; add / remove per the modifiers
        bool selectAt(uint32_t sceneId, const Ray& ray, bool add, bool remove);
        // R51 box / lasso in view pixels
        bool selectRegion(uint32_t sceneId, const std::vector<Vector2>& region, bool lasso, bool add, bool remove, const ProjectFn& project, const Vector3& eye);
        enum class SelectCommand { All, None, Invert, Grow, Shrink, Linked, FlatRegion, Similar, ByNormal, ByChannelRange };
        bool selectCommand(uint32_t sceneId, SelectCommand command);
        // R50: the mode of every target's selection, converted
        void setSelectionMode(uint32_t sceneId, vc::ElementKind kind);
        size_t selectedCount(uint32_t sceneId) const;
        // R53 Apply (Enter): Face / Corner mode writes corners (hard edges), Vertex mode shared values
        bool applyToSelection(uint32_t sceneId, bool secondary);
        // R53: across the selection's extent along its longest axis (or between two picked points)
        bool gradientAcross(uint32_t sceneId);
        // R53: the selection's average into the primary colour; its hex for the clipboard
        bool copyColorFromSelection(uint32_t sceneId, std::string* hex);
        // R54 recolor: every value matching `from` becomes `to` on the acting targets
        bool recolor(uint32_t sceneId, const Vector4& from, const Vector4& to);
        // R54 select: the faces holding `value`
        bool selectByColor(uint32_t sceneId, const Vector4& value);
        // R55 Shift-click on a swatch: the colour onto the selection
        bool applyColorToSelection(uint32_t sceneId, const Vector4& value);
        // the seed of Flat Region and the axis of By Normal: the last clicked face
        struct ClickedFace {
            bool valid = false;
            TargetRef target;
            uint32_t triangle = 0;
            Vector3 normal;             // local
        };
        ClickedFace lastClicked;

        // ---- an edit's result for the window (errors, warnings)
        const std::string& lastError() const { return error; }
        const std::string& lastNotice() const { return notice; }
        void clearMessages() { error.clear(); notice.clear(); }
        // runs an edit on the acting targets, reporting its error; one undo step
        bool runEdit(uint32_t sceneId, std::vector<vc::Op> ops, const std::string& undoName, std::function<void(const EditResult&)> done = nullptr,
                     const std::atomic<bool>* cancel = nullptr, std::function<void(float)> progress = nullptr);

        // ---- the cursor (R103) and status (R109)
        void hover(uint32_t sceneId, const Ray& ray, bool ctrl, bool shift);
        void hideCursor();
        // each frame: the fade (voxel decal rule)
        void tick(bool cameraMoving);
        const PaintCursorLines& cursorLines() const { return cursor; }
        uint64_t cursorRevision() const { return cursorRev; }
        bool cursorVisible() const { return cursorShown && cursorAlpha > 0.0f; }
        float cursorOpacity() const { return cursorAlpha; }
        const std::vector<Vector3>& affectedVertices() const { return affected; }
        // R51: the element selection (and the wire of the targets) as lines on the surfaces
        const PaintCursorLines& selectionLines() const { return selectionDrawn; }
        uint64_t selectionRevision() const { return selectionRev; }
        const std::string& statusText() const { return status; }

        // R102: what the tool found, for the restore and the tests
        struct ViewMemory {
            bool held = false;
            uint32_t sceneId = 0;
            ViewDrawMode previousMode = ViewDrawMode::SHADED;
            ViewVertexColorChannel previousChannel = ViewVertexColorChannel::RGB;
            ViewDrawMode setMode = ViewDrawMode::VERTEX_COLOR;
            ViewVertexColorChannel setChannel = ViewVertexColorChannel::RGB;
        };
        const ViewMemory& viewMemory() const { return view; }

    private:
        friend class VertexPaintSceneTool;

        struct Stroke {
            bool active = false;
            uint32_t sceneId = 0;
            vc::StrokeMode mode = vc::StrokeMode::Paint;
            bool secondary = false;
            Vector3 eye;
            std::unique_ptr<StrokeSession> session;
        };

        SceneProject* sceneProject(uint32_t sceneId) const;
        std::function<bool(const Vector3&)> visibilityFor(uint32_t sceneId, const Vector3& eye, float tolerance) const;
        vc::Op strokeOpAt(const Vector3& point, float pressure, const Vector3& eye) const;
        void enterView(uint32_t sceneId);
        void leaveView();
        void detach(VertexPaintSceneTool* tool);
        bool setSelection(uint32_t sceneId, std::vector<ElementSelection> selection, const std::string& undoName);
        void rebuildCursor(uint32_t sceneId, const TargetHit& hit, vc::StrokeMode mode, bool secondary);
        void rebuildSelectionLines(uint32_t sceneId);

        Project* projectPtr = nullptr;
        VertexPaintSettings brush;
        PaintTool toolBeforePick = PaintTool::Paint;
        VertexPaintSceneTool* activeTool = nullptr;
        uint32_t toolScene = 0;
        Stroke stroke;
        ViewMemory view;

        // the cursor
        PaintCursorLines cursor;
        std::vector<Vector3> affected;
        uint64_t cursorRev = 1;
        bool cursorShown = false;
        float cursorAlpha = 0.0f;
        bool hasRay = false;
        Vector3 rayOrigin, rayDirection;
        std::chrono::steady_clock::time_point pointerMoved;
        struct CursorKey {
            TargetRef target;
            uint32_t triangle = 0;
            Vector3 point;
            float radius = 0.0f, hardness = 0.0f, strength = 0.0f;
            Vector4 tint;
            bool valid = false;
        };
        CursorKey cursorKey;
        struct DensityKey {
            const vc::MeshView* mesh = nullptr;
            float radius = 0.0f;
            bool low = false;
        };
        mutable DensityKey density;
        PaintCursorLines selectionDrawn;
        uint64_t selectionSignature = 0;
        uint64_t selectionRev = 1;
        std::string status;
        std::string error;
        std::string notice;
    };

    // the SceneTool of the armed Vertex Paint tool (one for every tool of the strip: switching
    // tools changes VertexPaintSettings::tool, the SceneTool stays)
    class VertexPaintSceneTool final : public SceneTool {
    public:
        explicit VertexPaintSceneTool(uint32_t sceneId);
        ~VertexPaintSceneTool() override;

        const char* id() const override { return "vertexPaint"; }
        const char* label() const override;
        const char* exitTooltip() const override { return "Exit Vertex Paint"; }
        bool available(const SceneProject& sceneProject) const override;
        SceneToolPress press(const SceneToolEvent& event) override;
        void drag(const SceneToolEvent& event) override;
        void release(const SceneToolEvent& event) override;
        void hover(const SceneToolEvent& event) override;
        bool key(ImGuiKey key, const SceneToolEvent& event) override;
        bool altClick(const SceneToolEvent& event) override;
        bool rightDrag(const SceneToolEvent& event, float dx, float dy) override;
        void cancel() override;
        void drawOverlay(SceneRender& render, ImDrawList& list) override;
        const SceneToolDecal* decal() const override;
        ImGuiMouseCursor cursor() const override { return ImGuiMouseCursor_Arrow; }
        std::string status() const override;

        uint32_t scene() const { return sceneId; }

    private:
        enum class Gesture { None, Stroke, SelectRegion, Gradient, Click };

        uint32_t sceneId = 0;
        mutable std::string labelText;
        bool exitRequested = false;
        Gesture gesture = Gesture::None;
        Vector2 pressAt;
        std::vector<Vector2> region;
        bool regionMoved = false;
        bool modAdd = false, modRemove = false;
        Ray pressRay;
        bool gradientHasStart = false;
        Vector3 gradientFrom, gradientTo;
        mutable SceneToolDecal decalLines;
        mutable uint64_t decalBuilt = 0;
    };

}
