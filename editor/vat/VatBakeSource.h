// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.1, 4.2 steps 1-3)
// - Purpose: every bake source normalised to clips on a hierarchy, with no file written and no
//   scene change that outlives a call:
//   * resolveVatSource: a VatSourceSpec -> VatBakeTarget (the scene and root entity to pose, the
//     clips as runtime AnimationClips, the files read with their sha1, the default output name).
//     Model files, prefabs and .poseanim targets that are files are instantiated in a private
//     scratch Scene (EntityPool::System, never seen by the Engine, as PoseRigProvider and the
//     prefab previews do), with an Animation Player on the root so models load in the node
//     hierarchy the clips address. GameObject sources pose the open scene in place.
//     Clips: model animations through ClipConverters::extractModelSubClip (glTF/FBX carry no
//     wrap: Loop); .anim files parsed as they are; an Animation Player's states with their
//     entry wraps; entity Animations through ClipConverters::convertEntityAnimation; a Pose
//     Animator's animation (or a Pose source) through PoseBaker::clipData. Lattice tracks are
//     clip tracks like any other; their deformation reaches the bake through CpuSkin.
//   * VatPoser: per sample, ActionSystem::previewPlayer of one clip at one time (wrap
//     ClampForever: the time is already inside the clip), cpuskin::updateTransforms,
//     LatticeRender::prepare, then each renderer's skin matrices, morph weights, lattice records
//     and mesh-to-root matrix (root = the root's rest model matrix, so motion of the root itself
//     is baked) and the sockets' root-space matrices. restore() puts the scene back
//     (restorePlayerPreview, then the matrices again). Renderers: every enabled MeshComponent
//     with submeshes on an active entity at or under the root (Instanced Mesh copies are not
//     baked: a warning). Materials: each submesh's engine Material as VatMaterialData (PBR
//     factors, textures as absolute files; images embedded in a glTF become VatEmbeddedImage
//     files written next to the .vat; a library or project shader, stochastic tiling or other
//     non-PBR settings are lost: material_simplified / materials_not_vat_ready).
// - Thread-safety: main thread (reads and poses scenes). The VatBakeRenderer/VatSampleState values
//   it produces are plain data for computeVatBake on any thread.

#ifndef VAT_BAKE_SOURCE_H
#define VAT_BAKE_SOURCE_H

#include "vat/VatBakeCompute.h"
#include "vat/VatBakeTypes.h"

#include "animation/AnimationClip.h"
#include "animation/AnimationPlayerRuntime.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{
    class Scene;
    class Model;
}

namespace jasperengine::editor{
    class Project;
}

namespace jasperengine::editor::vat{

    // a clip the source offers
    struct VatSourceClip{
        std::string name;                       // unique in the source ("Walk", "Walk 2")
        std::string origin;                     // what it came from ("assets/models/hero.gltf#Walk", ...)
        std::shared_ptr<const AnimationClip> clip;
        ClipWrapMode wrap = ClipWrapMode::Loop; // never Default
        float length = 0.0f;
        std::vector<ClipEventData> events;
        int tracks = 0;                         // sampled tracks
        std::vector<std::string> unbound;       // their paths that resolve to nothing under the root
    };

    // an image a glTF holds inside (buffer view or data URI): written next to the .vat
    struct VatEmbeddedImage{
        std::string name;                       // file name suffix ("Body_baseColor")
        std::string extension;                  // ".png" | ".jpg" | ...
        std::string bytes;
    };
    // VatMaterialData texture fields holding this prefix + an index name an embedded image
    constexpr const char* VAT_EMBEDDED_TEXTURE = "embedded:";

    class VatBakeTarget{
    public:
        VatBakeTarget();
        ~VatBakeTarget();
        VatBakeTarget(const VatBakeTarget&) = delete;
        VatBakeTarget& operator=(const VatBakeTarget&) = delete;

        Project* project = nullptr;
        uint32_t sceneId = 0;                   // the open scene posed; 0 = the private scratch scene
        Scene* scene = nullptr;
        Entity root = NULL_ENTITY;
        std::vector<VatSourceClip> clips;       // every clip the source offers, in source order
        // the files the source read: project-relative path -> "sha1:<hex>"
        std::vector<std::pair<std::string, std::string>> hashes;
        std::string stem;                       // the default output's file stem
        std::string directory;                  // its project-relative directory ("assets/models")

        bool scratch() const{ return sceneId == 0; }
        int findClip(const std::string& name) const;

    private:
        friend bool resolveVatSource(Project* project, const VatSourceSpec& spec, VatBakeTarget& target, std::vector<VatIssue>& issues);
        Scene* ownedScene = nullptr;
        Model* ownedModel = nullptr;
    };

    // False with the problems (invalid_source, no_animation...) in `issues`; warnings are added too
    bool resolveVatSource(Project* project, const VatSourceSpec& spec, VatBakeTarget& target, std::vector<VatIssue>& issues);

    // "sha1:<hex>" of a file's bytes, "" when it can't be read
    std::string fileSha1(const std::string& absolutePath);

    // The renderers, materials and socket nodes of a target, captured at rest, and the posing of
    // samples (main thread). One poser per bake run.
    class VatPoser{
    public:
        VatPoser();
        ~VatPoser();
        VatPoser(const VatPoser&) = delete;
        VatPoser& operator=(const VatPoser&) = delete;

        // the renderers at rest (their CpuSkin sources), materials and sockets (paths below the
        // root). False with `issues` (no_mesh, socket_not_found) or `error`.
        bool begin(VatBakeTarget& target, const std::vector<std::string>& sockets, std::vector<VatIssue>& issues, std::string& error);
        // the hierarchy posed by `clip` at `time` (seconds inside the clip), recorded
        bool capture(const VatSourceClip& clip, float time, VatSampleState& out, std::string& error);
        // the scene back to its rest (after each slice of captures, and at the end)
        void restore();

        // what begin() captured (moved into the compute input by the session)
        std::vector<VatBakeRenderer> renderers;
        std::vector<VatMaterialData> materials;
        std::vector<VatEmbeddedImage> embeddedImages;
        std::vector<VatIssue> warnings;         // material_simplified, materials_not_vat_ready, not_converted
        int estimatedVertices() const;          // distinct vertices before merging renderers
        int estimatedBones() const;             // joints used + one per rigid renderer
        bool morphOrLattice() const;            // a renderer has morph targets or lattice bindings

    private:
        VatBakeTarget* target = nullptr;
        std::vector<Entity> rendererEntities;
        std::vector<Entity> socketEntities;
        jasperengine::Matrix4 rootInverse;      // inverse of the root's rest model matrix
        anim::ClipPreviewSession session;
        bool posed = false;
    };

}

#endif //VAT_BAKE_SOURCE_H
