// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICE_RENDER_H
#define LATTICE_RENDER_H

// Per-frame packing of lattice bindings into MeshComponent::lattice (spec §5).

#include "Engine.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "lattice/LatticeTypes.h"
#include <cstdint>
#include <vector>

namespace jasperengine{

    class Scene;
    class EntityRegistry;
    struct LatticeComponent;
    struct LatticeModifierComponent;

    // What LatticeRender::prepare keeps from one frame to the next, one per scene (its
    // RenderSystem owns it). Engine-internal: the members are prepare's bookkeeping and
    // scratch, kept so a frame where nothing changes allocates nothing.
    struct JASPERENGINE_API LatticeRenderState{
        // the meshes prepare filled last frame, ascending: one that is not filled this frame
        // lost its bindings and is reset
        std::vector<Entity> meshes;

        // --- scratch of one frame ---

        // a lattice entity some binding names, validated and padded once a frame
        struct Lattice{
            Entity entity = NULL_ENTITY;
            const LatticeComponent* component = nullptr;
            Matrix4 model;
            int resolution[3] = {LATTICE_MIN_RESOLUTION, LATTICE_MIN_RESOLUTION, LATTICE_MIN_RESOLUTION}; // clamped
            std::vector<Vector3> padded;
            float maxOffset = 0.0f;  // largest |offset| of the padded grid, local units
            float worldScale = 0.0f; // largest stretch of a local unit into the world
            bool valid = false;
        };
        // an ancestor (or the mesh itself) on the path of the hierarchy walk
        struct PathEntry{
            Entity entity = NULL_ENTITY;
            LatticeModifierComponent* inherited = nullptr; // enabled, includeChildren
            size_t modifierIndex = 0;
        };
        // a binding that deforms the mesh being filled
        struct Accepted{
            size_t lattice = 0;          // into lattices
            Matrix4 toNormalized;
            Matrix4 toObject;
            float weight = 1.0f;
            int stage = 1;               // 0 before skinning, 1 after
            int mask = -1;               // -1 none, 0-3 rgba, 4-7 inverted rgba
            int submesh = -1;
            const LatticeModifierComponent* modifier = nullptr;
        };

        std::vector<Entity> filled;          // the meshes filled this frame
        std::vector<size_t> modifierStarts;  // Transform indices of the modifiers' entities, ascending
        std::vector<PathEntry> path;
        std::vector<Lattice> lattices;       // the first latticeCount are this frame's
        size_t latticeCount = 0;
        std::vector<Entity> overflow;        // per modifier (array index): a mesh that dropped one of its bindings
        std::vector<Vector4> texels;
    };

    class JASPERENGINE_API LatticeRender{
    public:
        // called by RenderSystem::update after transforms and bones are current and before
        // the mesh AABB loop; rebuilds LatticeMeshState of every affected mesh
        static void prepare(Scene* scene);
        // the same with the scene's state given (RenderSystem passes its own)
        static void prepare(Scene* scene, LatticeRenderState& state);

        // u_vs_lattice of one submesh's draw (includes/lattice.glsl): bindings, the bitmask of
        // those that deform the submesh, whether the pass reads the stretch, record starts
        static void getUniformBlock(const LatticeMeshState& lattice, unsigned int submesh, bool stretch, Vector4 block[2]);

        // Whether the mesh of an entity is bound to a lattice, by its own Lattice Modifier or an
        // ancestor's with includeChildren, counting disabled modifiers, lattices and bindings (a
        // script can turn them on): the shader variant collection's question (a registry of a
        // scene or a prefab, nothing loaded)
        static bool isLatticeTarget(EntityRegistry* registry, Entity entity);
    };

}

#endif //LATTICE_RENDER_H
