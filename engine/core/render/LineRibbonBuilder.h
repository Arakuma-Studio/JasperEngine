// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LINERIBBONBUILDER_H
#define LINERIBBONBUILDER_H

// The thick lines of Line Renderer and Trail Renderer (lines and trails spec 6.2): a
// camera-independent triangle list built on the CPU from the centre line, which the vertex
// shader (includes/lines_vs.glsl, LINE_RIBBON) turns to each camera that draws it. Every
// vertex carries its centre point, the incoming and outgoing directions of the line there,
// its side, half width and role; the shader decides the outer side of each turn (it depends
// on the view) and places the vertex: on the inner side every joint vertex collapses onto
// the miter point (zero-area triangles), on the outer side they spread into the miter,
// bevel or round joint. Pure, gtested (editor/tests/LineRibbonBuilderTest.cpp).

#include "Export.h"
#include "math/AABB.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine {

    struct AnimationCurve;
    struct Gradient;

    // Unity LineTextureMode: how u runs along the line
    enum class LineTextureMode : int {
        // once over the whole length (u = t x textureScale.x)
        Stretch = 0,
        // repeated every 1 / textureScale.x world units
        Tile = 1,
        // once over the whole line, each segment the same share
        DistributePerSegment = 2,
        // once per segment
        RepeatPerSegment = 3
    };

    // Unity LineAlignment: the ribbon faces the camera, or lies in the transform's XY plane
    enum class LineAlignment : int { View = 0, TransformZ = 1 };

    // the width is in world units (Unity) or screen pixels of the render target
    enum class LineWidthUnit : int { World = 0, Pixels = 1 };

    // A ribbon vertex, 19 floats; the order of the attributes of the ribbon variant
    // (RenderSystem::loadLines): a_position, a_color, a_texcoord1, a_tangentIn, a_tangentOut,
    // a_lineParams
    struct LineRibbonVertex {
        // the centre point (model space, or world space for useWorldSpace lines and trails)
        Vector3 position;
        // linear RGBA
        Vector4 color;
        // u along the line by the texture mode, v 0 on the -1 side and 1 on the +1 side
        Vector2 uv;
        // unit directions of the segments in and out of the point (= each other at an end)
        Vector3 tangentIn;
        Vector3 tangentOut;
        // x side (-1/+1; a cap: -1 at the start, +1 at the end), y half width (its unit),
        // z role (LineRibbonRole), w fan parameter in (0, 1) (an in or out vertex: 1 when
        // its joint is round, 0 when it may be a miter)
        Vector4 params;
    };
    static_assert(sizeof(LineRibbonVertex) == 19 * sizeof(float), "LineRibbonVertex is 19 packed floats");

    // params.z of a ribbon vertex (includes/lines_vs.glsl reads the same codes)
    enum LineRibbonRole : int {
        // body vertex ending the segment into the point (and both roles at an end)
        LineRibbonRole_In = 0,
        // corner fan vertex of a round joint
        LineRibbonRole_Corner = 1,
        // cap fan vertex of a round cap
        LineRibbonRole_Cap = 2,
        // body vertex starting the segment out of the point
        LineRibbonRole_Out = 3
    };

    // Unity's limit of numCornerVertices and numCapVertices
    constexpr unsigned int LINE_RIBBON_MAX_FAN_VERTICES = 90;

    struct LineRibbonSettings {
        bool loop = false;
        float widthMultiplier = 1.0f;
        // over t (null: 1)
        const AnimationCurve* widthCurve = nullptr;
        // over t (null: white); ignored when pointColors are given
        const Gradient* colorGradient = nullptr;
        unsigned int numCornerVertices = 0;
        unsigned int numCapVertices = 0;
        LineTextureMode textureMode = LineTextureMode::Stretch;
        Vector2 textureScale = Vector2(1.0f, 1.0f);
    };

    struct LineRibbonResult {
        // what was added to the lists
        size_t vertexCount = 0;
        size_t indexCount = 0;
        // length of the centre line (world or model units as the points), loop closed
        float length = 0.0f;
        // of the points used (not grown by the width)
        AABB bounds;
        float maxHalfWidth = 0.0f;
        // points skipped because they were not finite (NaN, inf)
        size_t invalidPoints = 0;
    };

    class JASPERENGINE_API LineRibbonBuilder {
    public:
        // The vertices and indices (triangle list) of one ribbon through `points`, appended to
        // `vertices` and `indices` (indices offset by the vertices already there). Consecutive
        // points closer than 1e-6 and non-finite points are skipped; fewer than 2 points that
        // remain add nothing. `pointT` (optional, one per point) is the curve and gradient
        // parameter of each point instead of the normalized length (trails: by age);
        // `pointColors` (optional, linear) replaces the gradient (Segments mode). Texture u
        // always runs along the length from the first point.
        static LineRibbonResult build(const Vector3* points, size_t count, const LineRibbonSettings& settings,
                                      std::vector<LineRibbonVertex>& vertices, std::vector<uint32_t>& indices,
                                      const float* pointT = nullptr, const Vector4* pointColors = nullptr);

        // the vertex and index counts build() adds for `points` distinct points
        static void counts(size_t points, bool loop, unsigned int numCornerVertices, unsigned int numCapVertices,
                           size_t& vertexCount, size_t& indexCount);

        // the half width (in the width unit) of t: multiplier x curve(t) / 2, never negative
        static float halfWidth(const LineRibbonSettings& settings, float t);

        // Where includes/lines_vs.glsl puts a vertex (world units): its offset from its centre
        // point in half widths for a ribbon facing `normal` (unit; View: towards the camera),
        // the directions already in the space of `normal`. The C++ mirror of the shader's
        // lineOffset, for tests and the editor's picking; keep them the same
        static Vector3 vertexOffset(const LineRibbonVertex& vertex, const Vector3& tangentIn, const Vector3& tangentOut, const Vector3& normal);
        static Vector3 vertexOffset(const LineRibbonVertex& vertex, const Vector3& normal);

        // Unity's startWidth / endWidth and startColor / endColor shortcuts of the line and
        // trail renderers. The width at t (multiplier x curve(t)); setting the two ends makes
        // the curve linear between them with the multiplier 1 (widths below 0 are 0)
        static float widthAt(float multiplier, const AnimationCurve& curve, float t);
        static void setEndWidths(float& multiplier, AnimationCurve& curve, float start, float end);
        // the gradient's colour at t as sRGB (alpha as is); setting one keys the colour and
        // the alpha at that end (time 0 or 1: the key there replaced, else one added)
        static Vector4 colorAt(const Gradient& gradient, float t);
        static void setEndColor(Gradient& gradient, float t, const Vector4& srgbColor);
    };

}

#endif //LINERIBBONBUILDER_H
