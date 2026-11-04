// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 2, 3.1, 3.4)
// - Purpose: the Vertex Animation asset (".vat" JSON + "<name>.vat.glb" mesh and payload) as data,
//   its one parser and canonical writer, the immutable runtime object the pool hands out, and the
//   playback math. The GLSL in shaderlib/includes/vat.glsl mirrors vatLocalTime / vatSample / vatWeightB /
//   vatPackInstance exactly: change both or neither (the parity test compares them).
// - Thread-safety: everything here is pure (no engine state) except VertexAnimation::getTexture,
//   which is used from the main thread only.
// - Units: times in seconds, `now` and play starts in the same clock (the VertexAnimator system's
//   scene time minus its epoch, spec 3.4), frames are sample indices (0-based within a clip,
//   "global" = clip.start + local).

#ifndef VERTEX_ANIMATION_H
#define VERTEX_ANIMATION_H

#include "Export.h"
#include "animation/AnimationClip.h"   // ClipEventData, ClipWarning
#include "animation/ClipTypes.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{

    class Texture;
    class VertexAnimationPool;

}

namespace jasperengine::vat{

    constexpr const char* VAT_FORMAT = "jasperengine.vat";
    constexpr int VAT_VERSION = 1;
    constexpr const char* VAT_EXTENSION = ".vat";
    // the generated mesh + payload next to the .vat: "<stem>.vat.glb"
    constexpr const char* VAT_MESH_SUFFIX = ".vat.glb";
    // glTF extension in the .vat.glb: {"version": 1, "payload": {"bufferView", "format", "width",
    // "height"}, "sockets": {"bufferView", "count", "frames"} (optional), "layoutHash"}
    constexpr const char* VAT_GLTF_EXTENSION = "JASPER_vertex_animation";
    // custom glTF vertex attribute: the vertex's index in the payload (float, exact below 2^24)
    constexpr const char* VAT_INDEX_ATTRIBUTE = "_VAT_INDEX";
    constexpr int VAT_MAX_CLIPS = 64;
    // VAT bones live in the payload texture, not the skinning palette (MeshComponent::bonesMatrix): the
    // limit is the glTF JOINTS_0 uint16 range (rigid destruction exports have hundreds of pieces)
    constexpr int VAT_MAX_BONES = 65535;
    constexpr int VAT_MAX_VERTICES = 1 << 24;
    // the CPU rebases its clock every VAT_EPOCH_SECONDS so float times keep millisecond precision
    constexpr double VAT_EPOCH_SECONDS = 2048.0;

    enum class VatMode : uint8_t {
        Vertices,   // per vertex per frame: position offset from rest (+ tangent frame) (+ colour)
        Bones       // per bone per frame: rotation quaternion, translation + uniform scale
    };

    enum class VatPrecision : uint8_t {
        Half,       // RGBA16F (Vertices default)
        Float       // RGBA32F (always for Bones)
    };

    struct JASPERENGINE_API VatBounds{
        Vector3 min = Vector3(0, 0, 0);
        Vector3 max = Vector3(0, 0, 0);
        bool valid = false;     // false = never extended

        void extend(const Vector3& p);
        void extend(const VatBounds& other);
    };

    struct JASPERENGINE_API VatClip{
        std::string name;
        int start = 0;          // first global frame (generated)
        int frames = 1;         // sample count, >= 1 (generated)
        float fps = 30.0f;      // samples per second, > 0, may be fractional (generated): a loop clip
                                // of length L sampled at F has frames = max(1, round(L*F)) and
                                // fps = frames / L; other wraps frames = max(1, round(L*F)) + 1 and
                                // fps = (frames - 1) / L, so sample i is at i / fps exactly
        ClipWrapMode wrap = ClipWrapMode::Loop;     // never Default (editable)
        float speed = 1.0f;     // editable, multiplies the player's speed
        bool interpolate = true;    // editable: false = nearest lower sample (fluids, stepped looks)
        bool restNone = false;  // Vertices: positions are absolute (rest = 0), "rest": "none" (generated)
        VatBounds bounds;       // object space, every frame of this clip (generated)
        std::vector<ClipEventData> events;  // editable, sorted by (time, function)

        // seconds: loop clips frames / fps, others (frames - 1) / fps (0 for a 1-frame clip)
        float length() const;
    };

    struct JASPERENGINE_API VatSocket{
        std::string name;       // editable, unique
        std::string path;       // the source node (ClipBinding path from the baked root), informative
    };

    struct JASPERENGINE_API VatLayout{
        VatPrecision precision = VatPrecision::Half;
        int width = 2048;       // texels per row
        int height = 0;         // rows
        int samples = 0;        // global frames (sum of clip frames)
        int vertices = 0;       // Vertices mode: payload vertices (= the mesh's distinct _VAT_INDEX count)
        int bones = 0;          // Bones mode: baked bones
        bool normals = true;    // Vertices: a tangent-frame texel per sample
        bool colors = false;    // Vertices: a colour texel per sample (imports only)
        std::string layoutHash; // computeLayoutHash() of the generated fields, also in the glb

        // texels per (frame, vertex) in Vertices mode: 1 + normals + colors; 2 in Bones mode
        int texelsPerSample(VatMode mode) const;
    };

    struct JASPERENGINE_API VatRecipeSource{
        // "model" (a model file, `clips` = its animation names), "anim" (.anim files in `clips`,
        // played on `path`), "entity" (a scene GameObject path in `path` of scene `scene`),
        // "pose" (a .poseanim in `clips` on the model `path`), "houdini_vat3", "openvat"
        std::string kind;
        std::string path;       // project-relative
        std::string scene;      // "entity" only
        std::vector<std::string> clips;
        // content hashes "sha1:<hex>" of every file this source read (path -> hash), for staleness
        std::vector<std::pair<std::string, std::string>> hashes;
    };

    struct JASPERENGINE_API VatRecipe{
        std::vector<VatRecipeSource> sources;
        std::string mode = "auto";          // "auto" | "vertices" | "bones"
        float fps = 30.0f;                  // requested sample rate
        std::string preset = "mobile_web";  // "mobile_web" | "desktop"
        bool normals = true;
        std::string precision = "auto";     // "auto" | "half" | "float"
        float toleranceMm = 1.0f;
        std::vector<std::string> sockets;   // source node paths to bake as sockets
        // importer options as compact JSON ({"fps": 24, "axis": "unity"...}), written back verbatim
        std::string importOptions;
    };

    struct JASPERENGINE_API VatStats{
        uint64_t bytes = 0;     // payload bytes
        float maxErrorMm = 0.0f;
        float meanErrorMm = 0.0f;
    };

    struct JASPERENGINE_API VatData{
        std::string guid;
        VatMode mode = VatMode::Vertices;
        std::string mesh;       // the .vat.glb, relative to the .vat's directory (generated)
        VatLayout layout;
        VatBounds bounds;       // every clip (generated)
        std::vector<VatClip> clips;
        std::vector<VatSocket> sockets;
        VatRecipe recipe;
        VatStats stats;
        // unknown top-level keys (compact JSON text), file order: written back
        std::vector<std::pair<std::string, std::string>> unknownKeys;

        int findClip(const std::string& name) const;    // -1 when absent
        int findSocket(const std::string& name) const;
    };

    struct JASPERENGINE_API VatParseResult{
        std::optional<VatData> data;
        std::vector<ClipWarning> warnings;  // JSON paths, like .anim
        std::string error;                  // "line:col: message" or a message; empty when ok

        bool ok() const { return data.has_value(); }
    };

    // Lenient like parseClip: fails only for non-JSON, a non-object root, a wrong/missing format,
    // a missing/newer version, or invalid generated fields (layout, clip start/frames/fps, mode,
    // mesh) with "generated field <path> is invalid: rebake (Inspector > Rebake, or
    // bake_vertex_animation {rebake})". Editable fields are repaired with warnings.
    JASPERENGINE_API VatParseResult parseVat(const std::string& text);
    // canonical text: 2-space indent, fixed key order (format, version, guid, mode, mesh, layout,
    // bounds, clips, sockets, recipe, stats, unknown keys), floats shortest round-trip (%.6g when
    // that reads back exactly, so parse(write(d)) keeps every float), deterministic bytes
    JASPERENGINE_API std::string writeVat(const VatData& data);
    // hash (hex) of mode, layout (minus layoutHash) and every clip's start/frames/fps/restNone
    JASPERENGINE_API std::string computeLayoutHash(const VatData& data);
    // "Vertices"/"Bones" file names: "vertices" | "bones"; "half" | "float"
    JASPERENGINE_API const char* modeName(VatMode mode);
    JASPERENGINE_API bool modeFromName(const std::string& name, VatMode& out);
    JASPERENGINE_API const char* precisionName(VatPrecision precision);
    JASPERENGINE_API bool precisionFromName(const std::string& name, VatPrecision& out);

    // ---- payload ----

    // Texel addressing (row-major, linear): texel t lives at (t % width, t / width)
    // Vertices: t = (globalFrame * vertices + vertex) * texelsPerSample + k
    //   k 0: position offset xyz (absolute when the clip's restNone), w 0
    //   k 1 (normals): unit quaternion (x, y, z, w) taking +X,+Y,+Z to tangent, bitangent, normal;
    //       consecutive frames of a vertex have dot >= 0 (hemisphere continuity)
    //   k 1 or 2 (colors): linear RGBA
    // Bones: t = (globalFrame * bones + bone) * 2 + k
    //   k 0: rotation quaternion (x, y, z, w), hemisphere-continuous per bone across frames
    //   k 1: translation xyz, uniform scale w
    //   skin matrix = T(translation) * R(rotation) * S(scale) (mesh space; identity = rest)
    JASPERENGINE_API uint64_t vatTexelIndex(VatMode mode, const VatLayout& layout, int globalFrame, int element, int k);

    struct JASPERENGINE_API VatPayload{
        VatPrecision precision = VatPrecision::Half;
        int width = 0;
        int height = 0;
        // width * height texels, 4 channels, little-endian half (uint16) or float32
        std::vector<uint8_t> bytes;
    };

    // CPU-side socket tracks: [globalFrame][socket] = position xyz + rotation xyzw (7 floats),
    // object space of the baked root
    struct JASPERENGINE_API VatSocketTracks{
        int sockets = 0;
        int frames = 0;
        std::vector<float> data;
    };

    // ---- playback (mirrored by vat.glsl) ----

    // one track: clip index (-1 = rest), when it started and its speed (negative plays backwards).
    // Speed 0 holds the track: `start` is then the local time it is held at (vatLocalTime), so a
    // paused or stopped track is written once and costs nothing per frame
    struct JASPERENGINE_API VatTrack{
        int clip = -1;
        float start = 0.0f;     // clock time it started at; the held local time when speed == 0
        float speed = 1.0f;
    };

    // A = current, B = fading in (fadeStart..fadeEnd, linear) or held by a fixed weight with phase
    // sync (blend >= 0: B's normalized time follows A's, weight = blend)
    struct JASPERENGINE_API VatTrackPair{
        VatTrack a;
        VatTrack b;             // b.clip -1: no B
        float fadeStart = 0.0f;
        float fadeEnd = 0.0f;
        float blend = -1.0f;    // < 0: fade mode
    };

    struct JASPERENGINE_API VatFrameSample{
        int frame0 = 0;         // global frames
        int frame1 = 0;
        float alpha = 0.0f;     // weight of frame1
        float normalized = 0.0f;    // 0..1 phase within the clip (loop: x / frames, others x / (frames - 1))
        bool finished = false;  // Once / ClampForever and past the end (forward) or before 0 (backward)
    };

    // a track's local time at `now` (seconds, unwrapped): speed == 0 ? start (held there)
    // : (now - start) * speed. Every caller of vatSample with a track uses it (vat.glsl's
    // vatLocalTime is the same expression)
    JASPERENGINE_API float vatLocalTime(const VatTrack& track, float now);

    // Frame math for one clip at local time tau = vatLocalTime(track, now) (seconds):
    //   x = tau * fps (in samples)
    //   Loop:        x = x - frames * floor(x / frames); f0 = floor(x); f1 = f0 + 1 == frames ? 0 : f0 + 1
    //   Once/Clamp:  x = clamp(x, 0, frames - 1); f0 = floor(x); f1 = min(f0 + 1, frames - 1)
    //   PingPong:    p = 2 * (frames - 1); y = p > 0 ? x - p * floor(x / p) : 0;
    //                x = y <= frames - 1 ? y : p - y; then as Once
    //   alpha = interpolate ? x - f0 : 0; frames == 1 gives f0 = f1 = 0, alpha 0
    JASPERENGINE_API VatFrameSample vatSample(const VatClip& clip, float tau);
    // the same with the phase given (0..1) instead of a time (phase-synced B)
    JASPERENGINE_API VatFrameSample vatSampleNormalized(const VatClip& clip, float normalized);
    // weight of B at `now`: 0 without B; blend mode: clamp(blend, 0, 1); fade mode:
    // fadeEnd > fadeStart ? clamp((now - fadeStart) / (fadeEnd - fadeStart), 0, 1) : 1
    JASPERENGINE_API float vatWeightB(const VatTrackPair& pair, float now);

    // GPU packing (per-instance attributes i_vat0/i_vat1, or the uniform block for a single entity):
    //   v0 = (a.clip, a.start, a.speed, b.clip)
    //   v1 = (b.start, b.speed, fadeStart, blend >= 0 ? -(1 + blend) : fadeEnd)
    // so v1.w < 0 means blend mode with weight -v1.w - 1. Clip indices are exact small floats.
    // A speed of 0 is a held track whose start is its local time (vatLocalTime).
    JASPERENGINE_API void vatPackInstance(const VatTrackPair& pair, Vector4& v0, Vector4& v1);
    JASPERENGINE_API VatTrackPair vatUnpackInstance(const Vector4& v0, const Vector4& v1);

    // ---- the runtime asset ----

    class JASPERENGINE_API VertexAnimation{
    public:
        // validates sizes (payload = layout width x height texels of the precision, sockets =
        // sockets x samples x 7 floats) and returns null with `error` set when they disagree
        static std::shared_ptr<const VertexAnimation> create(VatData data, VatPayload payload,
                                                             VatSocketTracks sockets, std::string* error);

        const VatData& getData() const { return data; }
        const VatPayload& getPayload() const { return payload; }
        const VatSocketTracks& getSocketTracks() const { return sockets; }
        // the asset path it was loaded from (pool key), empty for in-memory assets
        const std::string& getPath() const { return path; }

        int findClip(const std::string& name) const { return data.findClip(name); }
        float clipLength(int clip) const;

        // socket pose at `now` for a track pair (crossfades blend position linearly and rotation
        // with nlerp, exactly as the GPU blends bones); false for an unknown socket or no clips
        bool sampleSocket(int socket, const VatTrackPair& pair, float now, Vector3& position, Quaternion& rotation) const;

        // the payload as a nearest/clamp float texture, created on first use (main thread)
        Texture* getTexture() const;

    private:
        friend class jasperengine::VertexAnimationPool;
        VertexAnimation() = default;

        VatData data;
        VatPayload payload;
        VatSocketTracks sockets;
        std::string path;
        mutable std::shared_ptr<Texture> texture;
    };

    // The .vat.glb: the JSON chunk's JASPER_vertex_animation extension and the payload / socket
    // buffer views (a minimal GLB reader: no tinygltf, any thread)
    struct JASPERENGINE_API VatGlbContents{
        std::string layoutHash;
        VatPayload payload;
        VatSocketTracks sockets;
    };
    JASPERENGINE_API bool readVatGlb(const std::vector<uint8_t>& glb, VatGlbContents& out, std::string* error);

}

#endif //VERTEX_ANIMATION_H
