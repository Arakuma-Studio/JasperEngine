// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VERTEXCOLORRENDERER_H
#define VERTEXCOLORRENDERER_H

// docs/specs/V1-vertex-colors.md R11-R16, R80-R83, R95: the Vertex Colors components of a scene on
// their meshes, owned by its RenderSystem. Once a frame, before the meshes load (RenderSystem::update
// calls prepare), it reads each enabled component's .vcolors data, checks each target against its
// mesh and makes what the mesh binds for it (VertexColorBinding.h): a colour stream over the shared
// geometry, or split vertices built on a job where corners differ. It keeps the targets' MeshViews
// (VertexColorScene). A scene without the component pays one array size check a frame.

#include "Export.h"
#include "ecs/Entity.h"
#include "subsystem/VertexColorBinding.h"
#include "vertexcolor/VertexColorMesh.h"
#include "vertexcolor/VertexColorSplit.h"
#include "vertexcolor/VertexColorTypes.h"

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine {

    class Scene;
    struct MeshComponent;
    struct VertexColorsComponent;

    class JASPERENGINE_API VertexColorRenderer {
    public:
        // what the render path did (tests, the profiler)
        struct Stats {
            uint64_t colorUploads = 0;  // colour streams written in place (R14, R82)
            uint64_t bindings = 0;      // bindings made or replaced (each reloads its mesh)
            uint64_t splitJobs = 0;     // split builds started (R12)
            uint64_t fileReads = 0;     // .vcolors files decoded
            uint64_t viewBuilds = 0;    // MeshViews built from CPU geometry
        };

        explicit VertexColorRenderer(Scene* scene);
        ~VertexColorRenderer();
        VertexColorRenderer(const VertexColorRenderer&) = delete;
        VertexColorRenderer& operator=(const VertexColorRenderer&) = delete;

        // everything let go (RenderSystem::destroy): the meshes reload without it
        void clear();

        // RenderSystem::update, before the meshes load: bindings follow the components (R11-R15)
        void prepare();

        // RenderSystem::loadMesh: what a submesh of the mesh entity binds (null: nothing)
        std::shared_ptr<SubmeshVertexColors> getBinding(Entity meshEntity, unsigned int submesh) const;
        bool hasBindings() const { return !meshBindings.empty(); }

        // VertexColorScene: the cached view of a mesh's submesh (owner: the component's entity, for
        // R83's message)
        std::shared_ptr<const vertexcolor::MeshView> getMeshView(Entity owner, Entity meshEntity, unsigned int submesh, MeshComponent& mesh);
        uint32_t getBoundSplitVertices(Entity owner, const vertexcolor::TargetKey& key) const;

        const Stats& getStats() const { return stats; }
        // split builds not swapped in yet
        size_t getPendingSplits() const;
        // the target has a binding now (tests, MCP facts)
        bool isBound(Entity owner, const vertexcolor::TargetKey& key) const;

    private:
        struct CachedView {
            uint64_t stamp = 0;
            std::shared_ptr<const vertexcolor::MeshView> view;
        };
        struct OwnerState;
        struct TargetState;

        Scene* scene;
        uint64_t frame = 0;

        bool loadData(Entity entity, VertexColorsComponent& colors);
        uint64_t ownerLayout(Entity entity);
        void syncOwner(Entity entity, VertexColorsComponent& colors, OwnerState& owner, double now);
        void syncTarget(Entity entity, VertexColorsComponent& colors, TargetState& target);
        void unbindTarget(TargetState& target);
        void unbindOwner(OwnerState& owner);
        void setBinding(Entity meshEntity, unsigned int submesh, const std::shared_ptr<SubmeshVertexColors>& binding);
        void rebind(MeshComponent& mesh, TargetState& target, vertexcolor::Format format, std::vector<uint8_t>&& bytes, bool dynamic);
        std::shared_ptr<SubmeshVertexColors> makeColorBinding(unsigned int submesh, vertexcolor::Format format, std::vector<uint8_t>&& bytes,
                                                              unsigned int count, bool dynamic);
        std::shared_ptr<SubmeshVertexColors> makeSplitBinding(MeshComponent& mesh, unsigned int submesh, vertexcolor::Format format,
                                                              const vertexcolor::SplitResult& split, std::vector<uint8_t>&& colorBytes, bool dynamic);

        std::map<std::pair<Entity, unsigned int>, CachedView> views;
        std::unordered_map<Entity, std::vector<std::shared_ptr<SubmeshVertexColors>>> meshBindings;
        std::unordered_map<Entity, std::unique_ptr<OwnerState>> owners;
        Stats stats;
    };

}

#endif // VERTEXCOLORRENDERER_H
