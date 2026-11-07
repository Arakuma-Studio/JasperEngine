// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INSTANCED_MESH_COMPONENT_H
#define INSTANCED_MESH_COMPONENT_H

#include "Engine.h"
#include "math/Rect.h"
#include "buffer/ExternalBuffer.h"

#include <vector>

namespace jasperengine{

    // how a particle instance turns (particles spec 6.4; InstancedMeshComponent::particle).
    // None: the instance's own rotation in the mesh's space (every instanced mesh but a
    // version 2 Particle System's). The others build each instance in the world: position
    // in the mesh's space, scale the world size, rotation the particle's own rotation in the
    // frame the mode names (View: facing the camera plane, Vertical: upright facing the
    // camera, Horizontal: lying flat, Stretched: along the velocity facing the camera, World,
    // Local: the mesh GameObject's rotation, Facing: towards the camera position, Velocity:
    // forward along the velocity)
    enum class InstanceAlignment : uint8_t{
        None = 0,
        View,
        Vertical,
        Horizontal,
        Stretched,
        World,
        Local,
        Facing,
        Velocity
    };

    // the order instances draw in: Auto (blended batches back to front where the camera sorts),
    // ByDistance (back to front also when opaque), Keep (the instances' own order: the age sorts)
    enum class InstanceSortMode : uint8_t{
        Auto = 0,
        ByDistance,
        Keep
    };

    // what a Particle System's renderer module asks of its instanced mesh, written by the
    // system each frame it simulates (runtime, not serialized)
    struct InstanceParticleRender{
        InstanceAlignment alignment = InstanceAlignment::None;
        Vector3 pivot = Vector3(0.0, 0.0, 0.0);   // in particle sizes, in the aligned frame
        // Stretched: length = size x lengthScale + speed x velocityScale (+ the camera's speed
        // x cameraVelocityScale)
        float lengthScale = 2.0f;
        float velocityScale = 0.0f;
        float cameraVelocityScale = 0.0f;
        // the projected size kept within [min, max] x the viewport height (max 0: no limit)
        float minSize = 0.0f;
        float maxSize = 0.0f;
        InstanceSortMode sort = InstanceSortMode::Auto;
        // added to the camera distance in the transparent queue (lower draws on top, Unity)
        float sortingFudge = 0.0f;

        // the renderer's sortPriority, added to the mesh's (its shader's) in the transparent queue
        int sortPriority = 0;

        bool cameraDependent() const{
            return alignment == InstanceAlignment::View || alignment == InstanceAlignment::Vertical ||
                   alignment == InstanceAlignment::Stretched || alignment == InstanceAlignment::Facing ||
                   (alignment != InstanceAlignment::None && (minSize > 0.0f || maxSize > 0.0f));
        }
    };

    struct InstanceData{
        Vector3 position = Vector3(0.0, 0.0, 0.0);
        Quaternion rotation;
        Vector3 scale = Vector3(1.0, 1.0, 1.0);
        Vector4 color = Vector4(1.0, 1.0, 1.0, 1.0);  //linear color;
        Rect textureRect = Rect(0.0, 0.0, 1.0, 1.0);
        bool visible = true;
        // Vertex Animator state of this instance (vat::vatPackInstance), written by
        // VertexAnimatorSystem, not serialized; the default packs "no clip" (rest pose)
        Vector4 vat0 = Vector4(-1.0, 0.0, 1.0, -1.0);
        Vector4 vat1 = Vector4(0.0, 1.0, 0.0, 0.0);
        // a particle's (InstanceParticleRender alignment): its billboard roll in radians (the z
        // of its rotation) and its velocity in the world, not serialized. Kept readable for
        // other renderers of the particles (the HTML UI's <particle-system>)
        float roll = 0.0f;
        Vector3 velocity = Vector3(0.0, 0.0, 0.0);
    };

    struct InstanceRenderData{
        Matrix4 instanceMatrix;
        Vector4 color;
        Rect textureRect;
    };

    // an instance's Vertex Animator state on the GPU (i_vat0, i_vat1: InstanceData::vat0/vat1),
    // in a buffer of its own next to the InstanceRenderData one, only for a batch drawn with a
    // Vertex Animation variant
    struct InstanceVatRenderData{
        Vector4 vat0;
        Vector4 vat1;
    };

    struct InstanceBounds{
        Vector3 center; // model space, so an entity move needs no rebuild
        float radius = 0;
        float scale = 1; // largest instance axis
    };

    // slice of a RenderSystem instance view, nearest first and grouped by detail level
    struct InstanceViewRange{
        unsigned int offset = 0;
        // where the slice's InstanceVatRenderData start in the view's VAT list (a batch drawn
        // with a Vertex Animation variant; the others have none there)
        unsigned int vatOffset = 0;
        unsigned int lodCount[MAX_MESH_LODS] = {};
        // after the plain buckets, the copies of the instances cross-fading between two levels
        // (spec S54 R8: each instance in a level's band twice, once per level, its fade in the
        // first matrix column's w): per level, the copies keeping most of their pixels first
        // (fadeDominant of them: what a pass drawing one level per instance draws)
        unsigned int fadeCount[MAX_MESH_LODS] = {};
        unsigned int fadeDominant[MAX_MESH_LODS] = {};

        unsigned int count() const{
            unsigned int total = 0;
            for (unsigned int c : lodCount) total += c;
            for (unsigned int c : fadeCount) total += c;
            return total;
        }
        unsigned int plainCount() const{
            unsigned int total = 0;
            for (unsigned int c : lodCount) total += c;
            return total;
        }
        unsigned int fadeTotal() const{
            unsigned int total = 0;
            for (unsigned int c : fadeCount) total += c;
            return total;
        }
    };

    // settings the views depend on, compared each frame since setters do not flag a rebuild
    struct InstanceViewSettings{
        bool cull = true;
        bool castShadows = true;
        bool lodEnabled = true;
        float lodBias = 1;
        float cullDistance = 0;

        bool operator!=(const InstanceViewSettings& o) const{
            return cull != o.cull || castShadows != o.castShadows || lodEnabled != o.lodEnabled ||
                   lodBias != o.lodBias || cullDistance != o.cullDistance;
        }
    };

    struct JASPERENGINE_API InstancedMeshComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        ExternalBuffer buffer;

        std::vector<InstanceData> instances;
        std::vector<InstanceRenderData> renderInstances; //must be sorted
        std::vector<InstanceBounds> renderBounds; // parallel to renderInstances
        // parallel to renderInstances (same order) while the mesh draws with a Vertex Animation
        // variant, else empty; uploaded into vatBuffer (i_vat0, i_vat1), made by such a load
        std::vector<InstanceVatRenderData> renderVat;
        ExternalBuffer vatBuffer;

        unsigned int maxInstances = 100;
        unsigned int numVisible = 0;

        // culled slices: [0] main camera, [1 + s] shadow atlas slot s; other cameras draw them all
        InstanceViewRange views[1 + MAX_SHADOW_ATLAS_SLOTS];
        InstanceViewSettings viewSettings; // the views were built with these
        bool cullInstances = true; // off for shaders that move instances away from their bounds

        // world-space distance from the main camera past which instances are dropped from
        // the culled views (0 = unlimited); a hard cut, so hide it with fog
        float cullDistance = 0;

        // Instances shrink to nothing between fadeStart and fadeEnd, both model-space distances.
        // distanceFade picks the shader variant, so an empty range disables it without a rebuild.
        bool distanceFade = false;
        float fadeStart = 0;
        float fadeEnd = 0;
        // Instances grow from nothing at nearFadeStart to full size at nearFadeEnd, both model-space
        // distances (a far level of detail's inner edge); an empty range turns it off. Needs
        // distanceFade, whose variant draws both ranges
        float nearFadeStart = 0;
        float nearFadeEnd = 0;
        Vector3 fadeEyeLocal; //camera in model space, where the range is measured

        // fade ranges the render lists were cut at, and the eye movement they still cover (render
        // system cache)
        float lastFadeCullEnd = -1;
        float lastFadeCullNear = 0;
        float lastCullSlack = 0;
        Vector3 lastFadeCullEye;
        // a cylindrical billboard's last yaw, kept while the camera looks straight along the up axis
        float lastBillboardYaw = 0;

        bool instancedBillboard = false;
        bool instancedCylindricalBillboard = false;

        // a version 2 Particle System's renderer settings (runtime, written by its system)
        InstanceParticleRender particle;

        // a foliage prototype (T5 R6): its instances live in FoliageSystem's lists, by foliageSlot
        bool foliage = false;
        bool foliageWind = false;       // 'Wnd'
        bool foliageDither = false;     // 'Dth'
        bool foliageExpanded = false;   // R37: a program with no compact variant, from a 96-byte stream
        uint32_t foliageSlot = 0;

        bool needUpdateBuffer = false;
        bool needUpdateInstances = true;
    };

}

#endif //INSTANCED_MESH_COMPONENT_H
