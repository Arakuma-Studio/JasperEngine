// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PICKTYPES_H
#define PICKTYPES_H

// The vocabulary of triangle picking (docs/specs/S26-scene-view.md): what a query looks for
// (PickFilter) and what it finds (PickHit). The Scene view's picker, the drop placement, the
// measure tool and the Lua MeshRaycast all speak it.

#include "Entity.h"
#include "math/Vector3.h"
#include "registry/TagManager.h"

#include <cstdint>
#include <functional>
#include <span>

namespace jasperengine {

    // How a hit was tested
    enum class PickKind : uint8_t {
        MESH,       // a MeshComponent's triangles (sprites, tilemaps and mesh polygons are meshes)
        INSTANCE,   // one instance of an InstancedMeshComponent, through the shared mesh's triangles
        TERRAIN,    // a terrain's height field surface
        UI,         // a RectTransform's rect (or a UI element's bounds)
        BOUNDS,     // bounds only: points, lines, particles, 2D occluders, meshes without CPU data or still preparing
        ICON,       // an editor icon (lights, cameras, audio, probes), tested in screen space by the editor
        TREE,       // a Terrain's tree, through its LOD0's triangles (T5 R35)
        DETAIL      // a Terrain's detail instance: opt-in, on resident cells only
    };

    // PickFilter::kinds bits, one per PickKind
    inline constexpr uint32_t PICK_MESH     = 1u << (uint32_t)PickKind::MESH;
    inline constexpr uint32_t PICK_INSTANCE = 1u << (uint32_t)PickKind::INSTANCE;
    inline constexpr uint32_t PICK_TERRAIN  = 1u << (uint32_t)PickKind::TERRAIN;
    inline constexpr uint32_t PICK_UI       = 1u << (uint32_t)PickKind::UI;
    inline constexpr uint32_t PICK_BOUNDS   = 1u << (uint32_t)PickKind::BOUNDS;
    inline constexpr uint32_t PICK_ICON     = 1u << (uint32_t)PickKind::ICON;
    inline constexpr uint32_t PICK_TREE     = 1u << (uint32_t)PickKind::TREE;
    inline constexpr uint32_t PICK_DETAIL   = 1u << (uint32_t)PickKind::DETAIL;
    // not grass: it doesn't block default rays
    inline constexpr uint32_t PICK_ALL      = PICK_MESH | PICK_INSTANCE | PICK_TERRAIN | PICK_UI | PICK_BOUNDS | PICK_ICON | PICK_TREE;

    // EntityMetadata::editorFlags (editor only: never saved, cloned into Play or seen by Lua)
    inline constexpr uint8_t EDITOR_HIDDEN      = 1u << 0;  // Scene view visibility off (the Hierarchy's eye)
    inline constexpr uint8_t EDITOR_PICK_LOCKED = 1u << 1;  // Scene view picking off (the Hierarchy's hand)

    JASPERENGINE_API const char* pickKindName(PickKind kind);

    struct PickFilter {
        // layers (0..31) the query sees
        uint32_t layerMask = LAYER_MASK_EVERYTHING;
        // PICK_* bits of the kinds it tests
        uint32_t kinds = PICK_ALL;
        // also hidden, inactive and disabled ones; by default only what is drawn
        bool includeInactive = false;
        // triangles are hit from both sides, whatever their submesh culls (Unity's
        // Physics.queriesHitBackfaces)
        bool hitBackfaces = false;
        // a mesh whose BVH is still building: built (or helped) before the query returns;
        // false returns its bounds as a hit with pending set
        bool waitForBvh = true;
        // entities the query passes through
        std::span<const Entity> ignore;
        // only this entity (NULL_ENTITY: any)
        Entity restrictTo = NULL_ENTITY;
        // entities with any of these EntityMetadata::editorFlags bits are skipped
        uint8_t excludeEditorFlags = 0;
        // when set, only the entities it accepts (the editor: a view's own GameObjects)
        std::function<bool(Entity)> accept;
    };

    struct PickHit {
        Entity entity = NULL_ENTITY;
        PickKind kind = PickKind::MESH;
        // mesh hits: the submesh and the triangle (counted over the whole mesh, submeshes in
        // order, strips as their triangles); 0 for the other kinds
        uint32_t submesh = 0;
        uint32_t triangle = 0;
        // the instance of an INSTANCE hit, -1 otherwise
        int32_t instance = -1;
        // (w0, w1, w2) of the triangle's corners
        Vector3 barycentric;
        // world space
        Vector3 point;
        // interpolated when the mesh has normals, else the face normal; both face the ray
        Vector3 normal;
        Vector3 faceNormal;
        // world units along the normalized ray direction
        float distance = 0.0f;
        // a BOUNDS hit standing in for a mesh whose BVH is still building
        bool pending = false;
        // TREE: the tree (entity is its Terrain); -1 / 0 otherwise. DETAIL: the layer in prototype
        uint64_t foliageId = 0;
        int32_t prototype = -1;
    };

}

#endif // PICKTYPES_H
