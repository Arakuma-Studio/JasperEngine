// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R20-R28, R30-R34, R41-R49, R60, Table 1: the one operation engine.
// Every write of vertex colors (brush, selection Apply, generators, bakes, MCP edit_mesh_colors, Lua
// applyOps) is a list of Op run by applyOps over one target. Pure: geometry + current values + ops ->
// new values, deterministic on every platform (R27). What only the caller can provide (texture pixels,
// another mesh's values, scene occlusion, visibility from the camera) comes in already resolved on the
// op or through ApplyOptions hooks.

#include "Export.h"
#include "math/Curve.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "vertexcolor/VertexColorMesh.h"
#include "vertexcolor/VertexColorTypes.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::vertexcolor {

    enum class OpType : uint8_t {
        Set, Fill, Stroke, Gradient, Smooth, Sharpen, Noise, Adjust, Channel, ReplaceColor,
        FromTexture, BakeAO, BakeThickness, BakeCurvature, ByDirection, Transfer, Clear
    };
    // "set", "fill", ... as MCP and Lua spell them
    JASPERENGINE_API const char* opName(OpType type);
    JASPERENGINE_API bool opFromName(const std::string& name, OpType& out);

    // ---- R23: where an op acts
    enum class TargetKind : uint8_t { All, Vertices, Faces, Corners, Sphere, Box, Capsule, Color };

    // R23 color target / R52 Similar Color / R121 palette matching
    struct ColorMatch {
        Vector4 value = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        float tolerance = 0.02f;       // HSV distance (hue on a circle) when ignoreH/S/V, else max channel difference
        bool ignoreH = false;
        bool ignoreS = false;
        bool ignoreV = false;
        bool ignoreAlpha = true;
        float coverage = 1.0f;          // fraction of a face's corners (Face element) that must match
    };

    struct Target {
        TargetKind kind = TargetKind::All;
        std::vector<uint32_t> ids;      // Vertices: logical ids; Faces: triangle ids; Corners: triangle*3+k
        Vector3 center;                 // Sphere, Box
        float radius = 0.5f;            // Sphere, Capsule
        Vector3 halfExtents = Vector3(0.5f, 0.5f, 0.5f);   // Box
        Quaternion rotation;            // Box
        Vector3 from;                   // Capsule
        Vector3 to;
        ColorMatch color;               // Color
        Element element = Element::Vertex;   // shape and Color targets (R24); id targets imply theirs
    };

    // ---- R25
    enum class FilterKind : uint8_t { Facing, Slope, Height, Curvature, Channel, Occluded };
    struct Filter {
        FilterKind kind = FilterKind::Facing;
        bool invert = false;
        Vector3 direction = Vector3(0.0f, 1.0f, 0.0f);   // Facing (world or local per ApplyOptions::world)
        float angle = 90.0f;            // Facing: degrees from direction
        float min = 0.0f;               // Slope: degrees from up; Height: world/local Y; Curvature: -1..1; Channel: value
        float max = 0.0f;
        int channel = 0;                // Channel: 0..3
    };

    // ---- R30-R35
    enum class StrokeMode : uint8_t { Paint, Erase, Smooth, Sharpen };
    enum class FalloffPreset : uint8_t { Smooth, Linear, Constant, Sphere, Custom };
    struct StrokePoint {
        Vector3 position;
        float pressure = 1.0f;
    };
    // a grayscale texture in linear 0..1 (R33 stencil, or a texture for from_texture as RGBA)
    struct Image {
        int width = 0;
        int height = 0;
        std::vector<Vector4> pixels;    // row-major, linear (the caller decodes sRGB color data)
        bool wrap = true;               // repeat (true) or clamp UV
        Vector4 sample(float u, float v) const;   // bilinear
    };

    // ---- R45
    enum class GradientType : uint8_t { Linear, Radial, Height, Axis };
    enum class Interpolation : uint8_t { Linear, Oklab, Hsv };
    enum class HueDirection : uint8_t { Near, Far, Cw, Ccw };
    enum class Extend : uint8_t { Off, Forward, Backward, Both };
    struct GradientStop {
        float t = 0.0f;
        Vector4 value;
        float strength = 1.0f;
    };

    // ---- R46, R47, R48
    enum class NoiseKind : uint8_t { Value, Simplex, Cellular };
    enum class AdjustKind : uint8_t { Levels, BrightnessContrast, Hsv, Invert, Clamp };
    enum class ChannelOpKind : uint8_t { Copy, Swap, Invert, Pack };

    // ---- R60: another mesh's values, in world space, gathered by the caller
    struct TransferSource {
        std::vector<Vector3> positions;
        std::vector<Vector3> normals;   // may be empty: no normal filter
        std::vector<Vector4> values;
        // optional: the source's triangles (into positions) and each corner's value, so its hard
        // edges carry over as corner overrides. Empty: values per vertex only
        std::vector<uint32_t> indices;
        std::vector<Vector4> cornerValues;
    };
    enum class Unmatched : uint8_t { Keep, Nearest };
    enum class ClearTo : uint8_t { Initial, Asset, White, Black };

    struct Op {
        OpType type = OpType::Set;
        Target target;
        std::vector<Filter> filters;
        ChannelMask channels = CHANNELS_RGBA;
        Blend blend = Blend::Replace;
        float strength = 1.0f;
        Vector4 value = Vector4(1.0f, 1.0f, 1.0f, 1.0f);     // set/fill/stroke value; noise low; replace_color to
        Vector4 value2 = Vector4(1.0f, 1.0f, 1.0f, 1.0f);    // noise high

        // stroke
        std::vector<StrokePoint> points;
        float radius = 0.5f;
        FalloffPreset falloff = FalloffPreset::Smooth;
        Curve falloffCurve;             // FalloffPreset::Custom: x = distance/radius 0..1 -> weight
        float hardness = 0.0f;
        float spacing = 0.25f;
        bool buildup = false;
        StrokeMode mode = StrokeMode::Paint;
        bool pressureStrength = true;
        bool pressureSize = false;
        std::shared_ptr<const Image> stencil;
        float stencilScale = 1.0f;
        float stencilRotation = 0.0f;   // degrees

        // gradient
        GradientType gradientType = GradientType::Linear;
        Vector3 gradientFrom;
        Vector3 gradientTo = Vector3(0.0f, 1.0f, 0.0f);
        Vector3 axis = Vector3(0.0f, 1.0f, 0.0f);
        std::vector<GradientStop> stops;
        Interpolation interpolation = Interpolation::Oklab;
        HueDirection hueDirection = HueDirection::Near;
        Extend extend = Extend::Both;

        // smooth / sharpen / curvature
        int iterations = 1;

        // noise
        NoiseKind noiseKind = NoiseKind::Simplex;
        float noiseScale = 1.0f;
        int octaves = 3;
        uint32_t seed = 0;
        bool noiseWorld = true;

        // adjust
        AdjustKind adjustKind = AdjustKind::Levels;
        float inBlack = 0.0f, inWhite = 1.0f, gamma = 1.0f, outBlack = 0.0f, outWhite = 1.0f;
        float brightness = 0.0f, contrast = 0.0f;
        float hueShift = 0.0f, saturationShift = 0.0f, valueShift = 0.0f;   // degrees, -1..1, -1..1

        // channel
        ChannelOpKind channelKind = ChannelOpKind::Copy;
        int sourceChannel = 0;
        int destChannel = 1;
        std::array<int8_t, 4> packSource = {0, 1, 2, 3};   // -1: packConstant
        std::array<float, 4> packConstant = {0.0f, 0.0f, 0.0f, 0.0f};

        // replace_color: target.color matches, value is the replacement

        // from_texture (R41)
        std::shared_ptr<const Image> image;
        int uvSet = 0;
        int samples = 16;
        bool keepSeams = true;
        float seamDeltaE = 2.0f;        // OKLab ΔE ×100 scale as CIEDE-style numbers

        // bake_ao / bake_thickness (R42)
        int rays = 64;
        float distance = 1.0f;
        bool sceneScope = false;

        // by_direction (R44): smoothstep of dot(normal, direction) from rangeMin to rangeMax
        Vector3 direction = Vector3(0.0f, 1.0f, 0.0f);
        float rangeMin = 0.0f;
        float rangeMax = 1.0f;

        // transfer (R60)
        std::shared_ptr<const TransferSource> source;
        float maxDistance = 0.1f;
        float normalAngle = 60.0f;
        Unmatched unmatched = Unmatched::Keep;

        // clear
        ClearTo clearTo = ClearTo::Initial;
    };

    struct ApplyOptions {
        // local -> world of the target (bind pose in the object's current transform, D12)
        Matrix4 model;
        // ops' points, shapes, directions and heights are world space (true) or local (false)
        bool world = true;
        // R6: Erase and clear{initial} go back to this; fresh targets start from it
        InitialValue initial = InitialValue::Asset;
        Vector4 customInitial = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        // the format values are quantized to after the batch (stats and hashes see stored values)
        Format format = Format::RGBA8;
        // R25 occluded filter: is this world point visible from the brush's viewpoint (null: all are)
        std::function<bool(const Vector3& worldPoint)> visible;
        // R42 sceneScope: does a world ray hit other scene geometry before maxDistance (null: no)
        std::function<bool(const Vector3& origin, const Vector3& direction, float maxDistance)> sceneOccluded;
        // R31: the stroke session's per-vertex start values, kept by the caller across calls of one
        // stroke so the cap holds over many applyOps calls (null: the cap holds within this call)
        struct StrokeState* stroke = nullptr;
        // R97
        const std::atomic<bool>* cancel = nullptr;
        std::function<void(float progress)> progress;
        // the first op's number in errors ("ops[i]"): 0 for JSON callers, 1 for Lua (OpJsonHooks::firstIndex)
        size_t firstIndex = 0;
        // R28 per-channel stats before and after; they read every corner, so a brush turns them off
        bool stats = true;
    };

    // R31: per-stroke memory (start value and the strongest weight so far per touched element)
    struct JASPERENGINE_API StrokeState {
        std::vector<uint32_t> touched;          // GPU vertices (base values), or corners | 0x80000000, touched
        std::vector<Vector4> start;             // value at the stroke's start, parallel to touched
        std::vector<float> weight;              // strongest weight so far, parallel to touched
        std::vector<int32_t> slot;              // GPU vertex -> index in touched, -1
        std::vector<int32_t> cornerSlot;        // corner -> index in touched, -1
        bool hasLastPoint = false;              // the last input point of the previous call (spacing carries over)
        Vector3 lastPoint;
        float lastPressure = 1.0f;
        float carry = 0.0f;                     // arc length since the last stamp
        // the last stamp: the next one sweeps a capsule from it (R30), across calls too
        bool hasLastStamp = false;
        Vector3 lastStamp;
        float lastStampRadius = 0.0f;
        float lastStampPressure = 1.0f;
        void reset();
    };

    struct Warning {
        std::string code;      // Table 2
        std::string message;
    };

    struct ChannelStats {
        std::array<float, 4> min = {0, 0, 0, 0};
        std::array<float, 4> mean = {0, 0, 0, 0};
        std::array<float, 4> max = {0, 0, 0, 0};
        std::array<float, 4> p05 = {0, 0, 0, 0};
        std::array<float, 4> p50 = {0, 0, 0, 0};
        std::array<float, 4> p95 = {0, 0, 0, 0};
        std::array<std::array<uint32_t, 16>, 4> histogram = {};   // 16 bins over 0..1 (outside clamped)
        uint32_t count = 0;                     // corners counted (every triangle corner)
    };

    struct OpResult {
        uint32_t changedVertices = 0;           // logical vertices whose value changed
        uint32_t changedCorners = 0;            // corner overrides added, changed or removed
        uint32_t splitAdded = 0;                // corner overrides newly created
        uint32_t filteredOut = 0;               // elements in the target the filters removed
        uint32_t inTarget = 0;                  // elements the target selected before filters
        // from_texture (R41)
        float meanDeltaE = 0.0f;
        float p95DeltaE = 0.0f;
        uint32_t underSampledFaces = 0;
        // transfer (R60)
        uint32_t unmatched = 0;
        float meanDistance = 0.0f;
    };

    struct OpsResult {
        bool ok = false;
        bool cancelled = false;
        std::string error;                      // "ops[2].target.ids: ..." (index from ApplyOptions caller)
        size_t failedOp = 0;
        std::vector<OpResult> ops;
        ChannelStats before;
        ChannelStats after;
        uint64_t hash = 0;                      // hashTarget of the result
        std::vector<Warning> warnings;
    };

    // R26: all ops or none. `data` is unchanged when the result is not ok (or dryRun). A target
    // whose base is empty is first filled from the initial value (R6). Values are quantized to
    // options.format at the end.
    JASPERENGINE_API OpsResult applyOps(TargetData& data, const MeshView& mesh, std::span<const Op> ops,
                                        const ApplyOptions& options, bool dryRun = false);

    // fills an empty target (base sized to the mesh, overrides cleared, key/fingerprint kept)
    JASPERENGINE_API void initializeTarget(TargetData& data, const MeshView& mesh, InitialValue initial, const Vector4& custom);

    // R28 / R121
    JASPERENGINE_API ChannelStats computeStats(const TargetData& data, const MeshView& mesh);
    JASPERENGINE_API uint64_t hashTarget(const TargetData& data);   // base + overrides, stored precision

    struct PaletteEntry {
        Vector4 value;          // rounded to 8 bits per channel
        uint32_t corners = 0;   // how many triangle corners hold it
    };
    // R54, R121: distinct values sorted by hue then value, at most maxEntries (most used kept)
    JASPERENGINE_API std::vector<PaletteEntry> palette(const TargetData& data, const MeshView& mesh, size_t maxEntries = 64);

    // R23-R25: the elements a target and its filters select (ids of `kind`): what sample_mesh_colors
    // {elements} lists and what the selection commands build on
    JASPERENGINE_API std::vector<uint32_t> resolveElements(const TargetData& data, const MeshView& mesh, const Target& target,
                                                           std::span<const Filter> filters, const ApplyOptions& options,
                                                           ElementKind& kind);

    // ---- color maths shared by the editor UI, MCP and Lua (D4)
    JASPERENGINE_API float srgbToLinear(float c);
    JASPERENGINE_API float linearToSrgb(float c);
    // "#RRGGBB" / "#RRGGBBAA" (sRGB) -> linear; false on bad input
    JASPERENGINE_API bool parseHex(const std::string& hex, Vector4& linear);
    JASPERENGINE_API std::string toHex(const Vector4& linear, bool alpha);
    JASPERENGINE_API Vector4 blendValue(const Vector4& dst, const Vector4& src, Blend blend, float strength, ChannelMask channels);
    // OKLab ΔE ×100 between two linear colors (alpha ignored)
    JASPERENGINE_API float deltaE(const Vector4& a, const Vector4& b);

}
