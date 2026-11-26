// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R30-R35, R50-R53, R101-R109: the grammar of the Vertex Paint tools.
// What the window and the Scene view share (the settings), which op a tool and its modifiers mean,
// the cursor's lines, the undo names, the low-density check and the screen-region selection maths.
// No ImGui and no scene here, so the tests drive what the window and the Scene view do.

#include "math/Matrix4.h"
#include "math/Ray.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "vertexcolor/VertexColorMesh.h"
#include "vertexcolor/VertexColorOps.h"
#include "vertexcolor/VertexColorSelect.h"
#include "vertexcolor/VertexColorTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class CommandCatalog;
}

namespace jasperengine::editor::vertexcolor {

    namespace vc = jasperengine::vertexcolor;

    // R101's tool strip
    enum class PaintTool : uint8_t { Select, Paint, Smooth, Fill, Gradient, Pick };
    constexpr int PAINT_TOOL_COUNT = 6;
    const char* paintToolName(PaintTool tool);          // "Select", "Paint", ...
    const char* paintToolCommand(PaintTool tool);       // "vertexPaint.tool.select", ...

    // R51: a drag of the Select tool draws a box or a lasso
    enum class SelectShape : uint8_t { Box, Lasso };

    // R101 Masks: each one an R25 filter of every brush stamp, Fill and Gradient
    struct PaintMasks {
        bool slope = false;
        float slopeMin = 0.0f, slopeMax = 35.0f;       // degrees from up
        bool height = false;
        float heightMin = 0.0f, heightMax = 1.0f;      // world Y
        bool curvature = false;
        float curvatureMin = 0.1f, curvatureMax = 1.0f;   // -1 concave .. 1 convex
        bool channel = false;
        int channelIndex = 0;
        float channelMin = 0.5f, channelMax = 1.0f;
        bool invert = false;                           // every mask inverted
        bool any() const { return slope || height || curvature || channel; }
    };

    struct VertexPaintSettings {
        PaintTool tool = PaintTool::Paint;
        // ---- Color (linear, D4: the window shows sRGB and hex)
        Vector4 primary = Vector4(0.27f, 0.43f, 0.06f, 1.0f);
        Vector4 secondary = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        vc::ChannelMask channels = vc::CHANNELS_RGB;
        vc::Blend blend = vc::Blend::Replace;
        float strength = 0.6f;
        // ---- Brush
        float radius = 0.5f;                           // world units, measured in world space (R32)
        float hardness = 0.3f;
        vc::FalloffPreset falloff = vc::FalloffPreset::Smooth;
        Curve falloffCurve;                            // FalloffPreset::Custom
        float spacing = 0.25f;                         // x radius (R30)
        bool buildup = false;                          // R31
        vc::Element element = vc::Element::Vertex;     // R24
        bool frontFacesOnly = true;
        bool skipOccluded = true;
        bool pressureStrength = true;                  // R35
        bool pressureSize = false;
        std::string stencilPath;                       // R33: a grayscale texture, "" none
        std::shared_ptr<const vc::Image> stencil;
        float stencilScale = 1.0f;
        float stencilRotation = 0.0f;                  // degrees
        PaintMasks masks;
        // ---- Selection (R50-R52)
        vc::ElementKind selectionMode = vc::ElementKind::Face;
        bool xray = false;
        SelectShape selectShape = SelectShape::Box;
        float flatRegionAngle = 10.0f;
        float byNormalAngle = 30.0f;
        vc::ColorMatch similar;
        int rangeChannel = 0;
        float rangeMin = 0.5f, rangeMax = 1.0f;
        bool pinSelection = false;                     // R50: kept when the tool exits
        // ---- Gradient tool and Gradient Across (R45, R53)
        vc::GradientType gradientType = vc::GradientType::Linear;
        vc::Interpolation interpolation = vc::Interpolation::Oklab;
        // ---- View (R102, R103)
        bool wireOnSelection = true;
        bool showAffected = true;
    };

    // ---- the brush
    constexpr float BRUSH_MIN_RADIUS = 0.001f;
    constexpr float BRUSH_MAX_RADIUS = 1000.0f;
    // R103
    constexpr size_t AFFECTED_DOTS_CAP = 2000;

    // R104: Ctrl inverts the mode (Paint -> Erase, Smooth -> Sharpen). Tools that do not stroke: Paint.
    vc::StrokeMode effectiveStrokeMode(PaintTool tool, bool ctrl);
    // R107: "Paint Vertex Colors", "Erase Vertex Colors", "Smooth Vertex Colors", "Sharpen Vertex Colors"
    std::string strokeUndoName(vc::StrokeMode mode);
    // R107: "Apply Color to 412 Faces", "Apply Color to 1 Vertex"
    std::string applyUndoName(vc::ElementKind kind, size_t count);
    // "412 faces selected", "1 vertex selected"
    std::string selectionCountText(vc::ElementKind kind, size_t count);
    const char* elementKindName(vc::ElementKind kind, bool plural);   // "Face" / "faces"

    // the colour a stroke writes: primary, or secondary while Shift is held (R104)
    Vector4 strokeValue(const VertexPaintSettings& settings, bool secondary);
    // R101 masks as R25 filters
    std::vector<vc::Filter> maskFilters(const VertexPaintSettings& settings);
    // R101 "Front faces only": the stamps facing the camera (world direction from the surface to the eye)
    vc::Filter facingFilter(const Vector3& towardEye);

    // R30-R35: one call's stroke op through these points (pressure 0..1, a mouse's 1)
    vc::Op strokeOp(const VertexPaintSettings& settings, vc::StrokeMode mode, bool secondary, std::span<const vc::StrokePoint> points);
    // R53 Apply: set (Replace at full strength) or fill with the blend and strength
    vc::Op applyOp(const VertexPaintSettings& settings, const vc::Target& target, bool secondary);
    // the Fill tool / bucket (Table 1: fill on a Linked or color target)
    vc::Op fillOp(const VertexPaintSettings& settings, const vc::Target& target, bool secondary);
    // R45 / R53 Gradient Across: primary at `from` to secondary at `to`, world space
    vc::Op gradientOp(const VertexPaintSettings& settings, const vc::Target& target, const Vector3& from, const Vector3& to);
    // R49, R54: every value matching `from` (8-bit rounded) becomes `to`
    vc::Op recolorOp(const Vector4& from, const Vector4& to);

    // R35: a negative reading is a mouse: full pressure
    float pressureOf(float reading);

    // R104 Ctrl+RMB drag: vertical is size, horizontal strength; Shift+RMB drag: vertical hardness,
    // horizontal stencil rotation. Pixels of mouse travel.
    void applyRightDrag(VertexPaintSettings& settings, bool ctrl, bool shift, float dx, float dy);
    void adjustSize(VertexPaintSettings& settings, float factor);
    void adjustStrength(VertexPaintSettings& settings, float factor);
    // R105 1/2/3/4
    void toggleChannel(VertexPaintSettings& settings, int channel);
    void swapColors(VertexPaintSettings& settings);

    // ---- R109 low density: fewer than one logical vertex per radius² of surface, on average
    bool isLowDensity(const vc::MeshView& mesh, const Matrix4& model, float radius);
    // "Rock_01: 24 vertices — paint will look blocky"
    std::string lowDensityText(const std::string& name, uint32_t logicalVertices);

    // ---- R103 the cursor: line segments (two points each) on the surface, in world space
    struct PaintCursorLines {
        std::vector<Vector3> points;
        std::vector<Vector4> colors;     // one per segment
    };
    // the decal's tint: the colour it paints, grey for single-channel work, alpha applied
    Vector4 cursorTint(const VertexPaintSettings& settings, vc::StrokeMode mode, bool secondary, float alpha);
    // outer ring (radius), hardness ring (radius * hardness) and the strength arc just outside the ring,
    // on the plane of `normal` at `center`; `drape` moves each point onto the surface (null: the plane)
    void buildCursor(const Vector3& center, const Vector3& normal, float radius, float hardness, float strength, const Vector4& tint,
                     const std::function<Vector3(const Vector3& point, const Vector3& normal)>& drape, PaintCursorLines& out);

    // ---- R108 / picking: a ray against a target's triangles in its local bind pose
    struct MeshHit {
        bool hit = false;
        float distance = 0.0f;           // world units along the unit ray direction
        uint32_t triangle = 0;
        Vector3 barycentric;             // weights of the triangle's corners 0, 1, 2
        Vector3 point;                   // world
        Vector3 normal;                  // world, unit, the face's (facing the ray when twoSided)
    };
    // brute force over the triangles; twoSided: back faces hit too (X-Ray)
    MeshHit raycastMesh(const vc::MeshView& mesh, const Matrix4& model, const Vector3& origin, const Vector3& direction, bool twoSided);
    // the world point of a triangle's barycentric point in the bind pose (D12)
    Vector3 surfacePoint(const vc::MeshView& mesh, const Matrix4& model, uint32_t triangle, const Vector3& barycentric);
    // R50 click: the element of `kind` under a hit (Vertex: the nearest corner's logical vertex)
    uint32_t elementAtHit(const vc::MeshView& mesh, vc::ElementKind kind, uint32_t triangle, const Vector3& barycentric);

    // ---- R51 box and lasso: the elements whose point projects inside the region. `project` maps a
    // world point to view pixels (false: behind the camera); `visible` is the X-Ray-off depth test
    // (null: X-Ray, everything). Faces use their centroid, corners a point a quarter of the way from
    // their vertex to the centroid.
    using ProjectFn = std::function<bool(const Vector3& world, Vector2& screen)>;
    using VisibleFn = std::function<bool(const Vector3& world, const Vector3& normal)>;
    vc::Selection selectInRect(const vc::MeshView& mesh, const Matrix4& model, vc::ElementKind kind, const Vector2& a, const Vector2& b,
                               const ProjectFn& project, const VisibleFn& visible);
    vc::Selection selectInLasso(const vc::MeshView& mesh, const Matrix4& model, vc::ElementKind kind, const std::vector<Vector2>& lasso,
                                const ProjectFn& project, const VisibleFn& visible);
    bool pointInPolygon(const std::vector<Vector2>& polygon, const Vector2& point);
    // R51: Shift adds, Ctrl removes, neither replaces
    vc::Selection combineSelection(const vc::Selection& current, const vc::Selection& picked, bool add, bool remove);

    // R105: the one-line legend under the tool strip, with the keys as they are bound now
    std::string paintLegend(const CommandCatalog& catalog, PaintTool tool);

}
