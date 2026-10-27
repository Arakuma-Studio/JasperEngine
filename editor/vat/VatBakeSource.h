// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.1, 4.2 steps 1-3)
// - Purpose: every bake source normalised to clips on a hierarchy, with no file written and no
//   scene change that outlives a call:
//   * resolveVatSource: a VatSourceSpec -> VatBakeTarget (the scene and root entity to pose, the
//     clips as runtime AnimationClips, the files read with their sha1, the default output name).
//     The files hashed are exactly the ones the bake reads: the model/prefab file, the model file
//     of every Model at or under the root (GameObject and prefab sources), the .anim files of the
//     root's Animation Player, .anim / .poseanim sources, and (VatPoser::begin) the texture files
//     the materials reference; never the scene file (saving the scene changes nothing baked).
//     The default output folder is the source's folder (the model's for a GameObject with one,
//     else its scene's, else assets/); a folder outside the assets folder maps to the same
//     relative folder under it ("models/hero.gltf" -> "assets/models").
//     Model files, prefabs and .poseanim targets that are files are instantiated in a private
//     scratch Scene (EntityPool::System, never seen by the Engine, as PoseRigProvider and the
//     prefab previews do), with an Animation Player on the root so models load in the node
//     hierarchy the clips address. GameObject sources pose the open scene in place; one with a
//     Model whose meshes MeshSystem has not built yet (needUpdateModel: a GameObject brought back
//     by undo, a scene just opened, a new model file; or an editor ModelLoadCmd still loading)
//     is refused with source_loading (vatSourceLoading tells when it is ready): until the load
//     its meshes have submeshes but no vertex data, and nothing in the scene is changed to load
//     them early.
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
//     baked: a warning). Materials: each submesh's engine Material as VatMaterialData (named as
//     the source glTF material when it came from a model; PBR factors, alphaMode, doubleSided,
//     unlit, foliage transmission; textures that are files as absolute paths; images a model
//     holds inside (a glb buffer view, a data: URI) become VatEmbeddedImage, stored inside the
//     .vat.glb). Every setting a glTF material has no slot for (a library or project shader,
//     stochastic tiling, blend / cull front / depth / stencil / colour write state, sort priority,
//     an extra pass, vertex or mixed lighting, a second UV set) is reported: material_simplified
//     (one note per material listing them) / materials_not_vat_ready.
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
#include <set>
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

    // an image a source model holds inside (a glb buffer view, a data: URI): stored inside the
    // .vat.glb (VatMeshData::images; material fields VAT_EMBEDDED_TEXTURE + its index)
    using VatEmbeddedImage = VatImageData;

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
        std::string sceneFile;                  // GameObject sources: the scene's project-relative file ("" unsaved)
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

    // Whether a GameObject source's meshes are still being built: a Model at or under it whose
    // MeshSystem load has not run yet (source_loading). False for file sources and for sources
    // that don't resolve (resolveVatSource reports those).
    bool vatSourceLoading(Project* project, const VatSourceSpec& spec);

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
        // the bones a Bones bake would hold: joints used and rigid renderers, merged where the
        // bake merges them (`animated`: the entities the clips move, animatedEntities)
        int estimatedBones(const std::set<Entity>& animated) const;
        // the entities the transform tracks of `clips` move under the target's root
        std::set<Entity> animatedEntities(const std::vector<const VatSourceClip*>& clips) const;
        bool morphOrLattice() const;            // a renderer has morph targets or lattice bindings

    private:
        VatBakeTarget* target = nullptr;
        std::vector<Entity> rendererEntities;
        std::vector<std::vector<Entity>> rendererJoints;            // per renderer: skin joint -> entity
        std::vector<std::vector<jasperengine::Matrix4>> restSkins;  // per renderer: skin matrices at rest
        std::vector<Entity> socketEntities;
        jasperengine::Matrix4 rootInverse;      // inverse of the root's rest model matrix
        anim::ClipPreviewSession session;
        bool posed = false;
        const AnimationClip* lastClip = nullptr;   // what the scene is posed by
    };

}

#endif //VAT_BAKE_SOURCE_H
