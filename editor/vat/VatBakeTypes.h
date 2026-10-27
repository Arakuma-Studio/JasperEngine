// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 2.4, 4.1, 4.2, 4.7)
// - Purpose: the value types of a Vertex Animation bake shared by every front end (the baker
//   window, Inspector actions, the MCP tool bake_vertex_animation, the bake-vat CLI): what to
//   bake (VatBakeSpec), the problems found (VatIssue, every kind of spec 4.7 with a message and
//   a fix), the size estimate made before any sampling (VatEstimate), the report of a finished
//   bake (VatBakeReport) and their JSON forms (the MCP / CLI report shape of spec 6).
// - Pure: no scene, no files. The preset limits and the size arithmetic live here so the
//   estimate, the validation and the bake agree on every number.
// - Paths in a spec are project-relative with '/' ("assets/models/hero.gltf"); GameObjects are
//   EntityPath paths ("Crowd/Hero") of an open scene named by its project-relative file.
// - Thread-safety: plain values and pure functions.

#ifndef VAT_BAKE_TYPES_H
#define VAT_BAKE_TYPES_H

#include "animation/VertexAnimation.h"

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::vat{

    // ---- the source (spec 4.1) ----

    enum class VatSourceKind{
        Model,      // a model file (.gltf/.glb/.fbx) and its animations (`clips`: names, empty = all)
        Anim,       // .anim files (`anims`) played on a model or prefab file (`path`), or on a
                    // GameObject (`path` an EntityPath of the open scene `scene`)
        Entity,     // a GameObject (`path` in `scene`): its Animation Player states, its entity
                    // Animations (converted with ClipConverters::convertEntityAnimation) and its
                    // Pose Animator's animation, with any lattice tracks they hold
        Pose        // a pose animation (`anims`: one .poseanim) on a model file or GameObject
    };

    struct VatSourceSpec{
        VatSourceKind kind = VatSourceKind::Model;
        std::string path;
        std::string scene;                  // GameObject sources: the scene file (must be open)
        uint32_t sceneId = 0;               // or the open scene by id (the window, Inspector: unsaved scenes too)
        std::vector<std::string> anims;     // Anim: .anim files; Pose: the .poseanim
    };
    // `path` names a GameObject of an open scene (scene or sceneId given), not a file
    inline bool isGameObjectSource(const VatSourceSpec& source){ return !source.scene.empty() || source.sceneId != 0; }

    // ---- settings ----

    enum class VatModeChoice{ Auto, Vertices, Bones };
    enum class VatPrecisionChoice{ Auto, Half, Float };
    enum class VatPreset{ MobileWeb, Desktop };
    enum class VatPlace{ None, Beside, Replace, Crowd };

    // spec 2.4
    struct VatPresetLimits{
        int width = 2048;                   // texels per row
        uint64_t maxTexels = 2048ull * 2048ull;
        int maxHeight = 2048;
        uint64_t memoryWarningBytes = 32ull * 1024ull * 1024ull;
    };
    VatPresetLimits presetLimits(VatPreset preset);
    const char* presetName(VatPreset preset);               // "mobile_web" | "desktop"
    bool presetFromName(const std::string& name, VatPreset& out);
    const char* modeChoiceName(VatModeChoice mode);         // "auto" | "vertices" | "bones"
    bool modeChoiceFromName(const std::string& name, VatModeChoice& out);
    const char* precisionChoiceName(VatPrecisionChoice precision);  // "auto" | "half" | "float"
    bool precisionChoiceFromName(const std::string& name, VatPrecisionChoice& out);
    const char* placeName(VatPlace place);                  // "none" | "beside" | "replace" | "crowd"
    bool placeFromName(const std::string& name, VatPlace& out);
    const char* sourceKindName(VatSourceKind kind);         // the recipe kinds: "model" | "anim" | "entity" | "pose"
    bool sourceKindFromName(const std::string& name, VatSourceKind& out);

    // Create Crowd (spec 5.4, the place mode's part): `count` instances in a square of side
    // `area` (metres) centred on the placed GameObject, random yaw, seeded
    struct VatCrowd{
        int count = 100;
        float area = 10.0f;
        uint32_t seed = 1;
    };

    // a per-clip setting the window's clip rows edit (and Rebake carries over from the .vat)
    struct VatClipOverride{
        std::string name;                   // the clip's name in the source listing
        ClipWrapMode wrap = ClipWrapMode::Default;  // Default: the source's (model clips: Loop)
        float fps = 0.0f;                   // 0: the spec's fps
    };

    struct VatBakeSpec{
        VatSourceSpec source;
        std::vector<std::string> clips;     // the clip names to bake, in this order (empty = all, source order)
        std::vector<VatClipOverride> clipOverrides;
        VatModeChoice mode = VatModeChoice::Auto;
        float fps = 30.0f;                  // samples per second, (0, 240]
        bool normals = true;                // Vertices: a tangent-frame texel per sample
        VatPrecisionChoice precision = VatPrecisionChoice::Auto;
        float toleranceMm = 1.0f;           // > 0; scaled up to 0.05% of the bounds diagonal
        VatPreset preset = VatPreset::MobileWeb;
        std::vector<std::string> sockets;   // node paths below the root (ClipBinding grammar)
        std::string output;                 // project-relative .vat; empty: <source dir>/<source stem>.vat
        bool overwrite = false;             // an explicit `output` that exists is refused without it
        VatPlace place = VatPlace::None;
        VatCrowd crowd;
    };

    // ---- problems (spec 4.7) ----

    // One action that resolves (part of) an issue: a button in the window, a hint in MCP.
    // `setting` names the spec field ("fps", "normals", "mode", "clips", "preset", "precision",
    // "interpolate", "output", "overwrite", "source"), `value` the value to set (JSON text).
    struct VatFix{
        std::string label;                  // "Lower to 20 fps"
        std::string setting;
        std::string value;
    };

    // kinds:
    //   errors (the bake is refused): too_large, no_animation, vertex_limit, bone_limit,
    //     no_mesh, invalid_source, invalid_settings, output_exists, socket_not_found,
    //     scene_playing, clip_limit
    //   warnings: unbound_tracks, large_interpolation_error, precision_switched, memory_high,
    //     mixed_rest, materials_not_vat_ready, material_simplified, events_dropped,
    //     not_converted
    struct VatIssue{
        std::string kind;
        std::string message;                // one sentence, names the clip/file/node concerned
        std::string fix;                    // one sentence (the fixes' labels for too_large)
        bool error = false;
        std::vector<VatFix> fixes;          // ordered by effect (too_large), may be empty
    };
    bool hasErrors(const std::vector<VatIssue>& issues);
    // "kind: message (fix)" lines
    std::string issuesText(const std::vector<VatIssue>& issues, bool errorsOnly = false);

    // ---- sizes ----

    // frames and fps of a clip of `length` seconds sampled at `fps` (the VatClip rules: loop
    // clips frames = max(1, round(L*F)), fps = frames / L; others frames + 1, fps = (frames - 1) / L;
    // a clip of length ~0 has one frame at `fps`)
    void clipFrames(float length, ClipWrapMode wrap, float fps, int& frames, float& sampleFps);
    // the time of sample `index` of such a clip
    float clipSampleTime(float length, ClipWrapMode wrap, int frames, float sampleFps, int index);

    // texels of a payload: samples x elements x texels per sample
    uint64_t payloadTexels(jasperengine::vat::VatMode mode, int samples, int elements, bool normals);
    // rows of `width` texels holding `texels`
    int payloadHeight(uint64_t texels, int width);
    // bytes of a payload of `width` x `height` texels
    uint64_t payloadBytes(int width, int height, jasperengine::vat::VatPrecision precision);
    // texel fetches per vertex per frame of playback, without crossfades (spec 3.5): Vertices
    // 2 (positions) / 4 (with the tangent frame); Bones 16, 4 when every vertex has one weight-1 bone
    int fetchesPerVertex(jasperengine::vat::VatMode mode, bool normals, bool rigidOnly);

    // ---- estimate (before any sampling) ----

    struct VatClipEstimate{
        std::string name;
        float length = 0.0f;
        ClipWrapMode wrap = ClipWrapMode::Loop;
        float requestedFps = 0.0f;          // the spec's fps or the clip's override
        int frames = 0;
        float fps = 0.0f;
        uint64_t bytes = 0;                 // its rows of the payload
    };

    // What the size of a bake depends on (estimate, too_large's fixes, the bake's own check)
    struct VatSizeInputs{
        jasperengine::vat::VatMode mode = jasperengine::vat::VatMode::Vertices;
        int vertices = 0;
        int bones = 0;
        bool normals = true;
        bool bonesPossible = true;          // a Bones bake could reproduce the source (no morph/lattice motion)
        VatPreset preset = VatPreset::MobileWeb;
        float fps = 30.0f;                  // the spec's (clips with their own fps keep theirs)
        std::vector<VatClipEstimate> clips; // name, length, wrap, requestedFps
    };
    // texels of the payload for these inputs (clip frames from their lengths and fps)
    uint64_t sizeTexels(const VatSizeInputs& inputs);
    // too_large for inputs over the preset's texels (or rows): the fixes that apply (lower fps to
    // what fits, drop normals, Bones mode, leave out the largest clip, desktop preset), ordered
    // by how far under the limit each one gets
    VatIssue tooLargeIssue(const VatSizeInputs& inputs);

    struct VatEstimate{
        jasperengine::vat::VatMode mode = jasperengine::vat::VatMode::Vertices;  // Auto: the likely choice
        std::string modeReason;
        jasperengine::vat::VatPrecision precision = jasperengine::vat::VatPrecision::Half;
        int vertices = 0;                   // distinct vertices of the merged mesh
        int bones = 0;                      // bones a Bones bake would hold (before merging equal ones)
        int renderers = 0;
        int samples = 0;
        std::vector<VatClipEstimate> clips;
        int width = 0;
        int height = 0;
        uint64_t texels = 0;
        uint64_t bytes = 0;
        uint64_t maxTexels = 0;             // the preset's
        int fetchesPerVertex = 0;
        std::vector<VatIssue> issues;
    };

    // ---- the result of a bake (spec 6 report) ----

    struct VatClipReport{
        std::string name;
        int frames = 0;
        float fps = 0.0f;
        uint64_t bytes = 0;
        float maxErrorMm = 0.0f;            // at its frames and mid-frames
        float interpolationErrorMm = 0.0f;  // at its mid-frames
    };

    struct VatBakeReport{
        jasperengine::vat::VatMode mode = jasperengine::vat::VatMode::Vertices;
        std::string modeReason;
        int vertices = 0;
        int bones = 0;
        std::vector<VatClipReport> clips;
        int width = 0;
        int height = 0;
        jasperengine::vat::VatPrecision precision = jasperengine::vat::VatPrecision::Half;
        uint64_t bytes = 0;
        float maxErrorMm = 0.0f;
        float meanErrorMm = 0.0f;
        float quantizationErrorMm = 0.0f;   // Vertices: half vs exact at the frames (0 for float)
        float toleranceMm = 0.0f;           // the tolerance used (scaled by the bounds)
        int fetchesPerVertex = 0;
        std::vector<VatIssue> warnings;
    };

    // the JSON forms MCP and the CLI print
    nlohmann::json issueJson(const VatIssue& issue);
    nlohmann::json issuesJson(const std::vector<VatIssue>& issues);
    nlohmann::json estimateJson(const VatEstimate& estimate);
    // {mode, mode_reason, vertices, bones, clips[{name, frames, fps, bytes}], texture{width,
    //  height, format}, bytes, max_error_mm, mean_error_mm, fetches_per_vertex, warnings[]}
    nlohmann::json reportJson(const VatBakeReport& report);

    // ---- recipe <-> spec ----

    // the recipe a bake of `spec` records (sources without hashes: the bake adds them)
    jasperengine::vat::VatRecipe recipeFromSpec(const VatBakeSpec& spec);
    // a spec that re-runs `recipe` (Rebake): false with `error` for a recipe this baker can't
    // run (an importer's: houdini_vat3 / openvat, or an unknown kind)
    bool specFromRecipe(const jasperengine::vat::VatRecipe& recipe, VatBakeSpec& spec, std::string& error);
    // the two describe the same source (kind, path, scene, the same files): the existing-file rule
    bool sameRecipeSource(const jasperengine::vat::VatRecipeSource& a, const jasperengine::vat::VatRecipeSource& b);

}

#endif //VAT_BAKE_TYPES_H
