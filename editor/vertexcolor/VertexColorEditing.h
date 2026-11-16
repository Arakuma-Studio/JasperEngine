// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md: what the editor does to an entity's vertex colors. The one entry
// point the Vertex Paint window, the scene tools, the Inspector, MCP (edit_mesh_colors,
// sample_mesh_colors, bake_mesh_colors) and editor scripts share. Main thread unless said.
// Every change is one undo step (R107), adds the component and its .vcolors file when missing
// (R120 `created`), copies a shared file before its first edit (R132), and schedules the debounced
// atomic save (R8).

#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "math/Vector4.h"
#include "vertexcolor/VertexColorMesh.h"
#include "vertexcolor/VertexColorOps.h"
#include "vertexcolor/VertexColorSelect.h"
#include "vertexcolor/VertexColorTypes.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::vertexcolor {

    namespace vc = jasperengine::vertexcolor;

    // R1: Models and built-in primitives; generated meshes are refused, `why` names the generator
    bool canPaint(Project* project, uint32_t sceneId, Entity entity, std::string* why);

    struct TargetInfo {
        vc::TargetKey key;
        std::string name;               // "Body (node 3, primitive 0)", "Submesh 0"
        uint32_t vertexCount = 0;
        uint32_t triangleCount = 0;
        bool hasAssetColors = false;    // the mesh has its own COLOR_0
        bool painted = false;           // the .vcolors file has this target
        uint32_t splitVertices = 0;     // R12, R124
        bool stale = false;             // R10: fingerprint mismatch not yet transferred
        std::string material;           // the submesh's material name
    };
    // the paintable primitives of the entity (empty when !canPaint)
    std::vector<TargetInfo> listTargets(Project* project, uint32_t sceneId, Entity entity);

    // the target's geometry in local bind pose, built (cached per mesh load; null when not loaded)
    std::shared_ptr<const vc::MeshView> meshView(Project* project, uint32_t sceneId, Entity entity, const vc::TargetKey& key);
    // local -> world of the target (D12)
    Matrix4 targetModel(Project* project, uint32_t sceneId, Entity entity, const vc::TargetKey& key);
    // the live values (unsaved edits included); null when the target is not painted yet
    const vc::TargetData* targetData(Project* project, uint32_t sceneId, Entity entity, const vc::TargetKey& key);
    // the target's values, or a temporary initialized from its initial value when not painted yet
    vc::TargetData targetDataOrInitial(Project* project, uint32_t sceneId, Entity entity, const vc::TargetKey& key);
    // R60: the entity's colors as a transfer op's source, world space: every target's values (its
    // initial value where not painted) and, when one has hard edges, every corner's, so they carry
    // over. Null (with `error`) when no mesh of it is loaded
    std::shared_ptr<const vc::TransferSource> transferSource(Project* project, uint32_t sceneId, Entity entity, std::string* error);
    // ApplyOptions with the entity's model, format and initial value filled in
    vc::ApplyOptions applyOptions(Project* project, uint32_t sceneId, Entity entity, const vc::TargetKey& key, bool world);

    struct TargetRef {
        Entity entity = NULL_ENTITY;
        vc::TargetKey key;
        bool operator==(const TargetRef& o) const { return entity == o.entity && key == o.key; }
    };

    struct EditRequest {
        std::vector<TargetRef> targets;          // every op runs on each target
        std::vector<vc::Op> ops;
        // R107, R110: when not empty, perTargetOps[i] runs on targets[i] instead of `ops` (Apply and
        // Gradient Across on several targets' own selections, from_texture per material), one step
        std::vector<std::vector<vc::Op>> perTargetOps;
        bool world = true;
        bool dryRun = false;
        std::string undoName;                    // "Apply Color to 412 Faces"
        // R25 occluded filter and R42 sceneScope (null: none / the scene's static meshes for sceneScope)
        std::function<bool(const Vector3&)> visible;
        // R97: run on a job when large (ops over > 250k elements, any bake); done() is called on the
        // main thread with the result. Null: run now on the calling (main) thread.
        std::function<void(const struct EditResult&)> done;
        const std::atomic<bool>* cancel = nullptr;
        std::function<void(float)> progress;
    };
    struct TargetResult {
        TargetRef target;
        vc::OpsResult result;
        uint32_t splitVertices = 0;              // after the edit
    };
    struct EditResult {
        bool ok = false;
        bool async = false;                      // went to a job: the final result arrives in done()
        std::string error;                       // "ops[1].target.ids: ..." or a target problem
        size_t failedOp = 0;
        std::vector<TargetResult> targets;
        bool createdComponent = false;
        bool createdFile = false;
        std::vector<vc::Warning> warnings;       // Table 2, both per-op and per-entity (material_ignores_channels, low_density)
    };
    // all targets or none (R26); one undo step unless dryRun
    EditResult applyEdit(Project* project, uint32_t sceneId, const EditRequest& request);

    // R30, R31, R107: an interactive stroke, many applyOps calls, one undo step at end()
    class StrokeSession {
    public:
        StrokeSession();
        ~StrokeSession();
        bool begin(Project* project, uint32_t sceneId, std::vector<TargetRef> targets, const std::string& undoName, std::string* error);
        // runs the op (usually a stroke with the new points) on every target the stamps reach
        bool apply(const vc::Op& op, std::function<bool(const Vector3&)> visible = nullptr);
        // the pointer left the surface: the next op starts a new segment (no capsule swept across the
        // gap), the stroke's strength cap goes on
        void lift();
        void end();          // commits one undo step
        void cancel();       // R106: restores the values from before begin()
        bool active() const;
    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // ---- R50-R52: the element selection (editor state, part of selection undo)
    struct ElementSelection {
        Entity entity = NULL_ENTITY;
        vc::TargetKey key;
        vc::Selection selection;
    };
    const std::vector<ElementSelection>& elementSelection(uint32_t sceneId);
    // replaces the selection (one undo step named `undoName` when non-empty)
    void setElementSelection(Project* project, uint32_t sceneId, std::vector<ElementSelection> selection, const std::string& undoName);
    void clearElementSelection(Project* project, uint32_t sceneId);
    // the selection of one target as a Target (false when it has none)
    bool selectionTarget(uint32_t sceneId, Entity entity, const vc::TargetKey& key, vc::Target& out);

    // ---- R10: stale targets found on load, transferred
    struct RemapIssue {
        Entity entity = NULL_ENTITY;
        vc::TargetKey key;
        bool unmatched = false;           // vertex_colors_unmatched (else remapped)
        float meanDistance = 0.0f;
    };
    std::vector<RemapIssue> remapIssues(Project* project, uint32_t sceneId);

    // ---- component-level actions (each one undo step)
    // R100: adds the component when missing (true when added)
    bool ensureComponent(Project* project, uint32_t sceneId, Entity entity, std::string* error);
    // removes every target's paint (component kept)
    bool clearAll(Project* project, uint32_t sceneId, Entity entity, std::string* error);
    // R5: converts the stored values
    bool setFormat(Project* project, uint32_t sceneId, Entity entity, vc::Format format, std::string* error);
    // R132: a private copy of a shared file
    bool makeUnique(Project* project, uint32_t sceneId, Entity entity, std::string* error);
    int fileUserCount(Project* project, const std::string& assetPath);
    // R100: what the Inspector's Paint (and Paint Vertex Colors) calls once the component is there:
    // the Vertex Paint window sets it to open itself with the Paint tool armed (empty: nothing opens)
    std::function<void(Project* project, uint32_t sceneId, Entity entity)>& paintToolHook();

    // R61: a Model's file written again with the paint (with Merge Static Meshes too), or a plain
    // mesh's (or an OBJ Model's) geometry written as a new .glb. Replace in Scene swaps the GameObject
    // to a Model of it and removes the component, one undo step
    struct ExportModelOptions {
        std::string path;                 // project-relative .glb ("": beside the model, or the .vcolors file)
        bool replaceInScene = false;
        bool splitHardEdges = true;
    };
    bool exportAsModel(Project* project, uint32_t sceneId, Entity entity, const ExportModelOptions& options, std::string* error);

    // R62
    struct BakeTextureOptions {
        vc::TargetKey key;
        std::string path;                 // project-relative .png
        int size = 1024;
        int padding = 4;
        int uvSet = 0;
        bool srgb = true;
    };
    bool bakeToTexture(Project* project, uint32_t sceneId, Entity entity, const BakeTextureOptions& options, std::string* error);

    // R73: the painted channels the entity's materials do not read, with the fix ("Set the material's
    // Vertex Color Use to Masks"), empty when every channel is used
    std::vector<vc::Warning> materialWarnings(Project* project, uint32_t sceneId, Entity entity, vc::ChannelMask painted);
    // R73: the one-click fix of a materialWarnings() entry (the material's Vertex Color Use or channel
    // mapping set so it reads the painted channels), one undo step; false with `error`
    bool fixMaterialWarning(Project* project, uint32_t sceneId, Entity entity, const vc::Warning& warning, std::string* error);

    // R7, R72: what each channel feeds ("Moss (Lit: Roughness)", "Wind (foliage)") for the panel and MCP
    std::array<std::string, 4> channelUses(Project* project, uint32_t sceneId, Entity entity);

}
