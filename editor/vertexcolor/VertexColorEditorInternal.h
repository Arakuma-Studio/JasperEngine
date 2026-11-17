// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What the editor's vertex color sources share (docs/specs/V1-vertex-colors.md R8, R10, R26, R100,
// R107, R132, D7) and nothing else includes: the live data of each .vcolors file, where a new file
// goes, the background writer, the preparation of an entity for its first write, and the undo step.
//
// The live values of a file are one vertexcolor::Data per file, shared by every component showing it
// (VertexColorsComponent::loaded, loadedPath = the AssetRef's path). The editor keeps them in a table
// by file, so an undo that puts a path back finds the unsaved values of that file rather than the
// disk's, and the scene save writes every dirty one.

#include "VertexColorEditing.h"

#include "asset/AssetRef.h"
#include "command/Command.h"
#include "math/Vector4.h"
#include "vertexcolor/VertexColorTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
    struct VertexColorsComponent;
}

namespace jasperengine::editor {
    struct SceneProject;
}

namespace jasperengine::editor::vertexcolor::detail {

    SceneProject* sceneProjectOf(Project* project, uint32_t sceneId);
    Scene* sceneOf(Project* project, uint32_t sceneId);
    VertexColorsComponent* componentOf(Project* project, uint32_t sceneId, Entity entity);
    std::filesystem::path absoluteAssetPath(Project& project, const std::string& assetPath);

    // ---- live data (VertexColorFiles.cpp)

    // The component's decoded file: adopted when the component holds it already, else the table's
    // unsaved values of that file, else read from disk. Null when the component has no file, or the
    // file does not read (`error`; the file is then never written, R131).
    std::shared_ptr<vc::Data> resolve(Project* project, uint32_t sceneId, Entity entity, std::string* error = nullptr);
    // After a write to live values: every component showing them re-uploads (R14), the file is dirty
    // and, with queueWrite, its write is queued (R8) unless it waits for the scene's save (R10). A
    // stroke queues once, at its end: a copy of the values per stamp would cost more than the stamp.
    void touched(Project* project, uint32_t sceneId, Entity entity, bool queueWrite = true);
    // The table learns a file's values (a new file, a copy)
    void registerData(Project& project, const std::string& assetPath, std::shared_ptr<vc::Data> data, bool dirty);
    // R10: the file keeps its bytes until the scene is saved
    void holdUntilSave(Project& project, const std::string& assetPath);
    bool isDirty(Project& project, const std::string& assetPath);
    // Writes `data` (editor section included) at once, through AtomicFile; the file gets its GUID
    // (S04) and `ref` names it
    bool writeNewFile(Project& project, const std::string& assetPath, const vc::Data& data, AssetRef& ref, std::string* error);
    // Beside the scene, in "<Scene>_VertexColors/<GameObject>.vcolors" (a scene not saved yet, or one
    // outside the assets: "VertexColors/"), numbered when taken
    std::string newFilePath(Project& project, const SceneProject& sceneProject, const std::string& entityName);
    // Blocks until every queued write was tried; false when one failed (kept for the next flush)
    bool flushWrites();
    // R9, R10: the editor-only positions, normals and triangles of the target from its mesh (nothing
    // when the target does not match it: a stale one keeps the mesh it was painted on)
    void fillSnapshot(vc::TargetData& target, const vc::MeshView& mesh);
    // fillSnapshot of each target of the file from the entity's meshes
    void refreshSnapshots(Project* project, uint32_t sceneId, Entity entity, vc::Data& data);
    // A project closing (and tests): the table, the remap checks and issues, the element selections
    // forget everything
    void resetFiles();
    void resetRemap();
    void resetSelections();

    // ---- what an entity holds or will hold (VertexColorEditing.cpp)

    // the component's initial value, or the one Add Component would give it: on a library:vertex_blend
    // material, whose unpainted white mixes every layer, Black (the base layer alone), or with five
    // layers, where A weighs the fifth, a Custom transparent black
    vc::InitialValue initialFor(Project* project, uint32_t sceneId, Entity entity, Vector4* custom);
    // the most layers a library:vertex_blend material of the entity blends (0: none uses it)
    int vertexBlendLayers(Scene* scene, Entity entity);
    // other users of the file, counting prefab assets: a write needs a copy first (R132)
    bool fileShared(Project* project, uint32_t sceneId, Entity entity);

    // Makes the entity ready for its first write, pushing undoable steps onto the scene's history (the
    // caller groups them): the component (R100, R120 `created`), its file, a private copy of a shared
    // one (R132). The live values are resolved on return.
    struct Prepared {
        bool createdComponent = false;
        bool createdFile = false;
        bool copied = false;
    };
    bool prepareForWrite(Project* project, uint32_t sceneId, Entity entity, Prepared& prepared, std::string* error);

    // ---- the undo step (VertexColorEditCmd.cpp, D7)

    // Per target, the XOR of its base values before and after in chunks of 4,096 vertices, packed with
    // packTerrainBytes (all-zero chunks left out), and its override tables before and after, packed. A
    // target absent on one side is all zeros there, so creation and removal are deltas too. Undo and
    // redo are the same XOR. Refused when the target is not what the step left (its mesh was remapped).
    class VertexColorEditCmd : public Command {
    public:
        static constexpr uint32_t CHUNK_VERTICES = 4096;

        // alreadyApplied: the live values hold the after state already (the first execute only
        // records); else the first execute applies it
        VertexColorEditCmd(Project* project, uint32_t sceneId, std::string name, bool alreadyApplied);

        // before / after: null for absent
        void addTarget(Entity entity, const vc::TargetData* before, const vc::TargetData* after);
        // the file's format changed with the values (setFormat)
        void setFormatChange(Entity entity, vc::Format before, vc::Format after);
        bool empty() const { return targets.empty() && formats.empty(); }

        bool execute() override;
        void undo() override;
        std::string undoRefusal() const override;
        std::string redoRefusal() const override;
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override { return empty(); }
        std::string describe() const override { return name; }
        const char* typeName() const override { return "VertexColorEditCmd"; }
        size_t memoryBytes() const override;
        bool releaseOffThread() const override { return true; }

        // tests
        size_t chunkCount() const;

    private:
        struct Chunk {
            uint32_t index = 0;
            std::vector<uint8_t> bytes;    // packTerrainBytes of the chunk's XOR
        };
        struct Corners {
            uint32_t count = 0;
            // packTerrainBytes of count x (u32 corner, 4 x f32); before and after share it when equal
            std::shared_ptr<const std::vector<uint8_t>> bytes;
        };
        struct TargetDelta {
            Entity entity = NULL_ENTITY;
            vc::TargetKey key;
            bool existsBefore = false;
            bool existsAfter = false;
            vc::Fingerprint fingerprintBefore;
            vc::Fingerprint fingerprintAfter;
            uint32_t countBefore = 0;
            uint32_t countAfter = 0;
            std::vector<Chunk> chunks;
            Corners cornersBefore;
            Corners cornersAfter;
        };
        struct FormatChange {
            Entity entity = NULL_ENTITY;
            vc::Format before = vc::Format::RGBA8;
            vc::Format after = vc::Format::RGBA8;
        };

        void apply(bool toAfter);
        std::string refusalFor(bool toAfter) const;

        Project* project;
        uint32_t sceneId;
        std::string name;
        bool applied;
        bool wasModified = false;
        std::vector<TargetDelta> targets;
        std::vector<FormatChange> formats;
    };

}
