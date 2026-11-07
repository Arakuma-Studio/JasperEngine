// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 2.4, 9.R3/R11/R12)
// - Purpose: glTF/FBX animations as read-only sub-asset clips: references "<model>#<name>" (or
//   "#<index>" when unnamed), their names, and the builder that turns one animation of a model
//   into ClipData with paths relative to the model root.
// - AnimationClipPool calls the builder on request (never loadGLTF); the editor's Extract
//   (ClipConverters) and get_asset_info use the same functions.
// - Thread-safety: the reference/name functions are pure. buildModelSubClip / listModelFileClips
//   may run on any thread (the shader-variant probe loads models off the UI thread): they read
//   the model through the ModelPool (holding the pooled-glTF mutex while reading the shared
//   tinygltf::Model, no reference kept afterwards) or their own parse of the file, kept behind a
//   mutex and never changed once kept (see dropModelFileParse).
// - Invariants:
//   * a reference splits at the first '#' that follows a ".gltf", ".glb" or ".fbx" extension
//     (case-insensitive): model file names may contain '#'.
//   * names: the glTF name, "Animation <i>" when empty (i 0-based, as loadGLTF names its
//     animation entities); the k-th later duplicate of a name is "<name> <k>" (k from 2), where
//     the loader's entities keep the duplicate name. Everything that lists a model's clips
//     (Inspector, get_asset_info, MCP) uses subClipNames.
//   * canonical reference form: formatSubClipReference writes the name (never the index); when
//     reading, a clip part that equals a name means that animation, and a number is an index only
//     when no animation has that name.
//   * the pool key of a sub-clip is "<normalized model path>#<animation index>", never the name.
//   * track paths mirror buildGLTFNodeHierarchy: node.name or "Node <i>", parent = the node that
//     lists it as a child, parentless nodes directly under the model root, "#k" = the count of
//     lower-index siblings with the same name. Frame rate 30, wrap Once, source {kind "model"};
//     buildModelSubClip then applies the clip's entry of "<model>.clips" (ModelClipSettings.h).
//   * accessor reads are bounds-checked and strided (byteStride); sparse accessors are rejected
//     with a warning.

#ifndef MODEL_SUB_CLIPS_H
#define MODEL_SUB_CLIPS_H

#include "Export.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "animation/AnimationClip.h"

#include <functional>
#include <string>
#include <vector>

namespace tinygltf{
    class Model;
}

namespace jasperengine::anim{

    struct SubClipReference{
        std::string model;      // the model path part (as given: asset-relative or project-relative)
        std::string clip;       // the part after '#': an animation name or "<index>"
    };

    // ".gltf", ".glb", ".fbx" (case-insensitive)
    JASPERENGINE_API bool isModelFilePath(const std::string& path);
    // true for "<model>#<clip>" with a non-empty clip part
    JASPERENGINE_API bool isSubClipReference(const std::string& reference);
    JASPERENGINE_API bool parseSubClipReference(const std::string& reference, SubClipReference& out);
    JASPERENGINE_API std::string formatSubClipReference(const std::string& model, const std::string& clip);

    struct ModelClipInfo{
        int index = -1;             // glTF animation index
        std::string name;           // the sub-clip name (see the naming invariant)
        float duration = 0.0f;      // seconds (last input time)
        size_t channels = 0;
    };

    // The sub-clip names of animations whose own (glTF/FBX) names are `animationNames`, in
    // order: the naming invariant above. A generated "<name> <k>" that another animation already
    // has as its own name moves on to the next k (names stay unique). get_asset_info uses it too.
    JASPERENGINE_API std::vector<std::string> subClipNames(const std::vector<std::string>& animationNames);

    // from a parsed model (no file access)
    JASPERENGINE_API std::vector<ModelClipInfo> listModelClips(const tinygltf::Model& model);
    // the animation index a reference's clip part names (a name, or a number), -1 when none
    JASPERENGINE_API int findModelClip(const tinygltf::Model& model, const std::string& clip);
    // One glTF node as loadGLTF makes it a GameObject: its name, its parent node (-1: under the
    // model root), its path relative to the model root and its local TRS
    struct ModelNodeInfo{
        std::string name;
        int parent = -1;
        std::string path;
        bool addressable = true;
        Vector3 position = Vector3(0, 0, 0);
        Quaternion rotation;
        Vector3 scale = Vector3(1, 1, 1);
    };

    // every node of a model file, index = glTF node index (the Animator's retargeting source)
    JASPERENGINE_API bool readModelFileNodes(const std::string& modelPath, std::vector<ModelNodeInfo>& out, std::string* error);

    // each node's path relative to the model root, index = glTF node index
    JASPERENGINE_API std::vector<std::string> modelNodePaths(const tinygltf::Model& model);
    // one animation as clip data; false (with `error`) when the index is out of range or nothing
    // could be read; dropped channels are warnings
    JASPERENGINE_API bool buildSubClip(const tinygltf::Model& model, int animationIndex, ClipData& out,
                                       std::vector<ClipWarning>& warnings, std::string* error);

    // the same from a model file (asset-relative path): the ModelPool's parse when the model is
    // loaded, else one parse of the file kept for the next calls (the last file parsed this way, its
    // animation data only), so the N sub-clips of a library file cost one parse
    JASPERENGINE_API bool listModelFileClips(const std::string& modelPath, std::vector<ModelClipInfo>& out, std::string* error);
    // the paths of a model file's nodes relative to the model root (modelNodePaths; the Avatar
    // Mask Inspector's tree), same parse rules
    JASPERENGINE_API bool listModelFileNodes(const std::string& modelPath, std::vector<std::string>& out, std::string* error);
    JASPERENGINE_API bool buildModelSubClip(const std::string& reference, ClipData& out,
                                            std::vector<ClipWarning>& warnings, std::string* error);
    // Runs `read` on the whole parsed model of `modelPath` (skins, meshes and buffers too, for a
    // humanoid source rig): the pooled parse under the pooled-glTF mutex when the model is loaded,
    // else a parse of the file of its own, not kept (callers cache what they derive from it:
    // HumanoidSourceCache)
    JASPERENGINE_API bool readModelFile(const std::string& modelPath, const std::function<bool(const tinygltf::Model&)>& read,
                                        std::string* error);
    // drops the kept parse of `modelPath` (every file when empty): AnimationClipPool::invalidateModel
    // (MeshSystem::invalidateModelFile / invalidateModelParse), clear and clearUnused call it
    JASPERENGINE_API void dropModelFileParse(const std::string& modelPath);

}

#endif //MODEL_SUB_CLIPS_H
