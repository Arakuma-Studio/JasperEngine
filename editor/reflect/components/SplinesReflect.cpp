// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The spline components (docs/specs/S18-splines.md R29; docs/specs/S08-reflection.md): Spline,
// Spline Extrude, Spline Instantiate and Spline Animate on the generic codec. Values at their
// defaults are not written (a missing key reads as the default); a knot, a profile point, a pool
// entry and a span override are each one flow map, so a long spline diffs one line per knot.
// Knots are written as {p, in, out, r, roll, mode}. The curve's type, closed flag, up axis and
// knots live in spline::SplineData, reached through its editable accessors: a write (decode,
// property edit) is followed by UpdateFlags_Spline or postDecode, which bump its version.

#include "reflect/Components.h"

#include "Factory.h"
#include "component/SplineAnimateComponent.h"
#include "component/SplineComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/SplineInstantiateComponent.h"

#include <cmath>

namespace jasperengine::editor::reflect {

    JE_REFLECT_STRUCT_TRAITS(spline::SplineKnot, kSplineKnotStruct);
    JE_REFLECT_STRUCT_TRAITS(SplineTerrainSettings, kSplineTerrainStruct);
    JE_REFLECT_STRUCT_TRAITS(SplineProfilePoint, kSplineProfilePointStruct);
    JE_REFLECT_STRUCT_TRAITS(SplinePoolEntry, kSplinePoolEntryStruct);
    JE_REFLECT_STRUCT_TRAITS(SplineSpanOverride, kSplineSpanOverrideStruct);

namespace {

    template<typename E>
    constexpr int v(E value) { return static_cast<int>(value); }

    // ---------------------------------------------------------------- enums (YAML tokens of R29)

    constexpr EnumItem kSplineTypeItems[] = {
        {v(spline::SplineType::Linear), "linear", "Linear", "spline::SplineType::Linear"},
        {v(spline::SplineType::CatmullRom), "catmull_rom", "Catmull Rom", "spline::SplineType::CatmullRom"},
        {v(spline::SplineType::CubicBezier), "bezier", "Cubic Bezier", "spline::SplineType::CubicBezier"},
    };
    constexpr EnumItem kSplineTangentModeItems[] = {
        {v(spline::TangentMode::Mirrored), "mirrored", "Mirrored", "spline::TangentMode::Mirrored"},
        {v(spline::TangentMode::Aligned), "aligned", "Aligned", "spline::TangentMode::Aligned"},
        {v(spline::TangentMode::Broken), "broken", "Broken", "spline::TangentMode::Broken"},
    };
    constexpr EnumItem kSplineTerrainRegionItems[] = {
        {v(SplineTerrainRegion::Path), "path", "Path", "SplineTerrainRegion::Path"},
        {v(SplineTerrainRegion::Area), "area", "Area", "SplineTerrainRegion::Area"},
    };
    constexpr EnumItem kSplineFlattenModeItems[] = {
        {v(SplineFlattenMode::Flatten), "flatten", "Flatten", "SplineFlattenMode::Flatten"},
        {v(SplineFlattenMode::CutOnly), "cut_only", "Cut Only", "SplineFlattenMode::CutOnly"},
        {v(SplineFlattenMode::FillOnly), "fill_only", "Fill Only", "SplineFlattenMode::FillOnly"},
    };
    constexpr EnumItem kSplineExtrudeShapeItems[] = {
        {v(SplineExtrudeShape::Flat), "flat", "Flat", "SplineExtrudeShape::Flat"},
        {v(SplineExtrudeShape::Bevel), "bevel", "Bevel", "SplineExtrudeShape::Bevel"},
        {v(SplineExtrudeShape::Crown), "crown", "Crown", "SplineExtrudeShape::Crown"},
        {v(SplineExtrudeShape::Rectangle), "rectangle", "Rectangle", "SplineExtrudeShape::Rectangle"},
        {v(SplineExtrudeShape::Tube), "tube", "Tube", "SplineExtrudeShape::Tube"},
        {v(SplineExtrudeShape::Custom), "custom", "Custom", "SplineExtrudeShape::Custom"},
        {v(SplineExtrudeShape::Fill), "fill", "Fill", "SplineExtrudeShape::Fill"},
        {v(SplineExtrudeShape::WaterBanks), "water_banks", "Water Banks", "SplineExtrudeShape::WaterBanks"},
    };
    constexpr EnumItem kSplineWidthSourceItems[] = {
        {v(SplineWidthSource::Radius), "radius", "Radius", "SplineWidthSource::Radius"},
        {v(SplineWidthSource::Constant), "constant", "Constant", "SplineWidthSource::Constant"},
    };
    constexpr EnumItem kSplineCornerStyleItems[] = {
        {v(SplineCornerStyle::None), "none", "None", "SplineCornerStyle::None"},
        {v(SplineCornerStyle::Mitre), "mitre", "Mitre", "SplineCornerStyle::Mitre"},
        {v(SplineCornerStyle::Round), "round", "Round", "SplineCornerStyle::Round"},
    };
    constexpr EnumItem kSplineGradeItems[] = {
        {v(SplineGrade::Racked), "racked", "Racked", "SplineGrade::Racked"},
        {v(SplineGrade::Stepped), "stepped", "Stepped", "SplineGrade::Stepped"},
    };
    constexpr EnumItem kSplineConformItems[] = {
        {v(SplineConform::None), "none", "None", "SplineConform::None"},
        {v(SplineConform::TerrainHeight), "terrain_height", "Terrain Height", "SplineConform::TerrainHeight"},
        {v(SplineConform::TerrainHeightAndSlope), "terrain_height_and_slope", "Terrain Height And Slope", "SplineConform::TerrainHeightAndSlope"},
    };
    constexpr EnumItem kSplineCollisionItems[] = {
        {v(SplineCollision::None), "none", "None", "SplineCollision::None"},
        {v(SplineCollision::Mesh), "mesh", "Mesh", "SplineCollision::Mesh"},
        {v(SplineCollision::Chain2D), "chain_2d", "Chain 2D", "SplineCollision::Chain2D"},
    };
    constexpr EnumItem kSplineInstantiateModeItems[] = {
        {v(SplineInstantiateMode::Spacing), "spacing", "Spacing", "SplineInstantiateMode::Spacing"},
        {v(SplineInstantiateMode::Tile), "tile", "Tile", "SplineInstantiateMode::Tile"},
        {v(SplineInstantiateMode::Fence), "fence", "Fence", "SplineInstantiateMode::Fence"},
    };
    constexpr EnumItem kSplineSpacingFitItems[] = {
        {v(SplineSpacingFit::FitToLength), "fit_to_length", "Fit To Length", "SplineSpacingFit::FitToLength"},
        {v(SplineSpacingFit::FixedPitch), "fixed_pitch", "Fixed Pitch", "SplineSpacingFit::FixedPitch"},
    };
    constexpr EnumItem kSplinePlantModeItems[] = {
        {v(SplinePlantMode::BoundsMin), "bounds_min", "Bounds Min", "SplinePlantMode::BoundsMin"},
        {v(SplinePlantMode::Pivot), "pivot", "Pivot", "SplinePlantMode::Pivot"},
    };
    constexpr EnumItem kSplineSpanOverrideKindItems[] = {
        {v(SplineSpanOverrideKind::Gate), "gate", "Gate", "SplineSpanOverrideKind::Gate"},
        {v(SplineSpanOverrideKind::Piece), "piece", "Piece", "SplineSpanOverrideKind::Piece"},
    };
    constexpr EnumItem kSplineAnimateMethodItems[] = {
        {v(SplineAnimateMethod::Duration), "duration", "Duration", "SplineAnimateMethod::Duration"},
        {v(SplineAnimateMethod::Speed), "speed", "Speed", "SplineAnimateMethod::Speed"},
    };
    constexpr EnumItem kSplineLoopItems[] = {
        {v(SplineLoop::Once), "once", "Once", "SplineLoop::Once"},
        {v(SplineLoop::Loop), "loop", "Loop", "SplineLoop::Loop"},
        {v(SplineLoop::PingPong), "ping_pong", "Ping Pong", "SplineLoop::PingPong"},
    };
    constexpr EnumItem kSplineEasingItems[] = {
        {v(SplineEasing::None), "none", "None", "SplineEasing::None"},
        {v(SplineEasing::EaseIn), "ease_in", "Ease In", "SplineEasing::EaseIn"},
        {v(SplineEasing::EaseOut), "ease_out", "Ease Out", "SplineEasing::EaseOut"},
        {v(SplineEasing::EaseInOut), "ease_in_out", "Ease In Out", "SplineEasing::EaseInOut"},
    };
    constexpr EnumItem kSplineAlignmentItems[] = {
        {v(SplineAlignment::None), "none", "None", "SplineAlignment::None"},
        {v(SplineAlignment::SplineTangent), "spline_tangent", "Spline Tangent", "SplineAlignment::SplineTangent"},
        {v(SplineAlignment::SplineTangentAndUp), "spline_tangent_and_up", "Spline Tangent And Up", "SplineAlignment::SplineTangentAndUp"},
        {v(SplineAlignment::World), "world", "World", "SplineAlignment::World"},
    };

}

namespace enums {
    const EnumDesc kSplineType{"SplineType", kSplineTypeItems, v(spline::SplineType::CatmullRom)};
    const EnumDesc kSplineTangentMode{"SplineTangentMode", kSplineTangentModeItems, v(spline::TangentMode::Mirrored)};
    const EnumDesc kSplineTerrainRegion{"SplineTerrainRegion", kSplineTerrainRegionItems, v(SplineTerrainRegion::Path)};
    const EnumDesc kSplineFlattenMode{"SplineFlattenMode", kSplineFlattenModeItems, v(SplineFlattenMode::Flatten)};
    const EnumDesc kSplineExtrudeShape{"SplineExtrudeShape", kSplineExtrudeShapeItems, v(SplineExtrudeShape::Bevel)};
    const EnumDesc kSplineWidthSource{"SplineWidthSource", kSplineWidthSourceItems, v(SplineWidthSource::Radius)};
    const EnumDesc kSplineCornerStyle{"SplineCornerStyle", kSplineCornerStyleItems, v(SplineCornerStyle::Mitre)};
    const EnumDesc kSplineGrade{"SplineGrade", kSplineGradeItems, v(SplineGrade::Racked)};
    const EnumDesc kSplineConform{"SplineConform", kSplineConformItems, v(SplineConform::None)};
    const EnumDesc kSplineCollision{"SplineCollision", kSplineCollisionItems, v(SplineCollision::None)};
    const EnumDesc kSplineInstantiateMode{"SplineInstantiateMode", kSplineInstantiateModeItems, v(SplineInstantiateMode::Spacing)};
    const EnumDesc kSplineSpacingFit{"SplineSpacingFit", kSplineSpacingFitItems, v(SplineSpacingFit::FitToLength)};
    const EnumDesc kSplinePlantMode{"SplinePlantMode", kSplinePlantModeItems, v(SplinePlantMode::BoundsMin)};
    const EnumDesc kSplineSpanOverrideKind{"SplineSpanOverrideKind", kSplineSpanOverrideKindItems, v(SplineSpanOverrideKind::Gate)};
    const EnumDesc kSplineAnimateMethod{"SplineAnimateMethod", kSplineAnimateMethodItems, v(SplineAnimateMethod::Duration)};
    const EnumDesc kSplineLoop{"SplineLoop", kSplineLoopItems, v(SplineLoop::Loop)};
    const EnumDesc kSplineEasing{"SplineEasing", kSplineEasingItems, v(SplineEasing::None)};
    const EnumDesc kSplineAlignment{"SplineAlignment", kSplineAlignmentItems, v(SplineAlignment::SplineTangentAndUp)};
}

namespace {

    // ---------------------------------------------------------------- shared

    // only what differs from the defaults is written; a missing (or empty) key is the default
    constexpr uint32_t kOmit = OmitIfDefault | EmptyIsDefault | CodegenIfChanged;
    constexpr uint32_t kOmitFloat = OmitIfDefault | OmitIfNonFinite | EmptyIsDefault | CodegenIfChanged;
    constexpr uint64_t kCurve = UpdateFlags_Spline;

    // a list whose elements are flow maps, one per line; exported as push_backs of each
    // element's aggregate initializer (its members in table order, which is their order)
    YAML::Node flowListToYaml(const FieldDesc& field, const void* value, const EncodeContext& ctx) {
        YAML::Node node = detail::arrayToYaml(field, value, ctx);
        for (YAML::Node element : node) {
            element.SetStyle(YAML::EmitterStyle::Flow);
        }
        return node;
    }

    template<typename T>
    constexpr TypeOps flowList() {
        TypeOps ops = jasperengine::editor::reflect::ops::Array<T>;
        ops.toYaml = &flowListToYaml;
        ops.cppStatements = &detail::structPushBackStatements;
        return ops;
    }

    constexpr TypeOps kKnotListOps = flowList<spline::SplineKnot>();
    constexpr TypeOps kProfileListOps = flowList<SplineProfilePoint>();
    constexpr TypeOps kPoolListOps = flowList<SplinePoolEntry>();
    constexpr TypeOps kOverrideListOps = flowList<SplineSpanOverride>();

    // the curve's own data, through SplineData's editable accessors
    void* splineType(void* owner) { return &static_cast<SplineComponent*>(owner)->spline.editableType(); }
    void* splineClosed(void* owner) { return &static_cast<SplineComponent*>(owner)->spline.editableClosed(); }
    void* splineUpAxis(void* owner) { return &static_cast<SplineComponent*>(owner)->spline.editableUpAxis(); }
    void* splineKnots(void* owner) { return &static_cast<SplineComponent*>(owner)->spline.editableKnots(); }

    template<typename M>
    constexpr FieldDesc curveField(const char* name, const char* cppPath, void* (*ref)(void*), FieldMeta meta) {
        meta.cppPath = cppPath;
        return detail::makeField<M>(name, ref, meta);
    }

    // ---------------------------------------------------------------- Spline

    // a knot: {p: [x, y, z], in: [..], out: [..], r: radius, roll: degrees, mode: tangent mode}
    constexpr FieldDesc kSplineKnotFields[] = {
        JE_FIELD(spline::SplineKnot, position, .yamlKey = "p", .ops = &ops::FiniteVector3Always, .updateFlags = kCurve, .unit = Unit::Meters),
        JE_FIELD(spline::SplineKnot, tangentIn, .yamlKey = "in", .flags = kOmit, .updateFlags = kCurve, .unit = Unit::Meters,
                 .tooltip = "Bezier: the handle the curve arrives along (negated), from the knot."),
        JE_FIELD(spline::SplineKnot, tangentOut, .yamlKey = "out", .flags = kOmit, .updateFlags = kCurve, .unit = Unit::Meters,
                 .tooltip = "Bezier: the handle the curve leaves along, from the knot."),
        JE_FIELD(spline::SplineKnot, radius, .yamlKey = "r", .ops = &ops::NonNegativeFiniteFloat, .flags = kOmitFloat | Clamped,
                 .updateFlags = kCurve, .unit = Unit::Meters, .min = 0.0f, .tooltip = "Half the width here (Extrude's Radius width)."),
        JE_FIELD_NAMED(spline::SplineKnot, rollDeg, "roll", .yamlKey = "roll", .ops = &ops::FiniteFloat, .flags = kOmitFloat, .updateFlags = kCurve,
                       .unit = Unit::Degrees, .tooltip = "Bank about the direction of travel; positive raises the right side."),
        JE_FIELD(spline::SplineKnot, mode, .enumDesc = &enums::kSplineTangentMode, .flags = kOmit, .updateFlags = kCurve),
    };

    constexpr FieldDesc kSplineTerrainFields[] = {
        JE_FIELD(SplineTerrainSettings, region, .enumDesc = &enums::kSplineTerrainRegion, .flags = kOmit,
                 .tooltip = "Path: the band along the curve. Area: the inside of a closed spline."),
        JE_FIELD(SplineTerrainSettings, extraWidth, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters,
                 .tooltip = "Added to the knots' radius on each side."),
        JE_FIELD(SplineTerrainSettings, falloff, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "Blend to the untouched terrain."),
        JE_FIELD(SplineTerrainSettings, flatten, .flags = kOmit),
        JE_FIELD(SplineTerrainSettings, flattenMode, .enumDesc = &enums::kSplineFlattenMode, .flags = kOmit),
        JE_FIELD(SplineTerrainSettings, heightOffset, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters,
                 .tooltip = "The bed sits this far from the curve (a road: minus its crown)."),
        JE_FIELD(SplineTerrainSettings, paint, .flags = kOmit),
        JE_FIELD(SplineTerrainSettings, paintLayer, .flags = kOmit, .tooltip = "Surface layer painted (blend map layer / 3, channel layer % 3)."),
        JE_FIELD(SplineTerrainSettings, paintStrength, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .min = 0.0f, .max = 1.0f),
        JE_FIELD(SplineTerrainSettings, paintFalloff, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(SplineTerrainSettings, paintNoise, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .min = 0.0f, .max = 1.0f,
                 .label = "Edge Noise", .tooltip = "Break-up of the painted band's edge."),
        JE_FIELD(SplineTerrainSettings, clearFoliage, .flags = kOmit),
        JE_FIELD(SplineTerrainSettings, foliageMask, .flags = kOmit, .label = "Foliage Layer Mask", .tooltip = "Bit n clears foliage layer n."),
        JE_FIELD(SplineTerrainSettings, terrain, .ops = &ops::Entity, .flags = kOmit, .tooltip = "None: the first terrain under the spline."),
    };

}

const StructDesc kSplineKnotStruct = JE_STRUCT_DESC(spline::SplineKnot, "SplineKnot", kSplineKnotFields);
const StructDesc kSplineTerrainStruct = JE_STRUCT_DESC(SplineTerrainSettings, "SplineTerrainSettings", kSplineTerrainFields);

namespace {

    // type, closed, up axis and the knots: the curve (its version bumps); the terrain settings
    // are read by the editor's bake only
    constexpr FieldDesc kSplineFields[] = {
        JE_FIELD(SplineComponent, enabled),
        curveField<spline::SplineType>("type", "spline.editableType()", &splineType,
                                       {.enumDesc = &enums::kSplineType, .flags = kOmit, .updateFlags = kCurve}),
        curveField<bool>("closed", "spline.editableClosed()", &splineClosed, {.flags = kOmit, .updateFlags = kCurve}),
        curveField<Vector3>("upAxis", "spline.editableUpAxis()", &splineUpAxis,
                            {.ops = &ops::FiniteVector3Always, .flags = kOmit, .updateFlags = kCurve,
                             .tooltip = "+Y in 3D; +Z for a 2D spline in the XY plane."}),
        // the whole list (a prefab override when the count changes) and each knot's members
        curveField<std::vector<spline::SplineKnot>>("knots", "spline.editableKnots()", &splineKnots,
                                                    {.ops = &kKnotListOps, .flags = ListWhole | ListEachElement | ListTemplate,
                                                     .updateFlags = kCurve}),
        JE_FIELD(SplineComponent, terrain, .flags = kOmit),
    };

    // what a decode wrote went past the setters: keep their invariants, then a new version
    void splinePostDecode(void* comp, const DecodeContext&) {
        spline::SplineData& data = static_cast<SplineComponent*>(comp)->spline;
        for (spline::SplineKnot& knot : data.editableKnots()) {
            knot.radius = std::isfinite(knot.radius) ? std::max(0.0f, knot.radius) : 0.0f;
        }
        Vector3& up = data.editableUpAxis();
        const float length = up.length();
        up = (std::isfinite(length) && length > 0.0f) ? up / length : Vector3(0.0f, 1.0f, 0.0f);
        data.markDirty();
    }

    // the terrain settings away from their defaults (a struct the field list does not export)
    void splineCodegen(const void* comp, const CodegenContext& ctx, std::string& out) {
        const SplineComponent& spline = *static_cast<const SplineComponent*>(comp);
        const SplineTerrainSettings defaults;
        void* settings = const_cast<SplineTerrainSettings*>(&spline.terrain);
        const std::string ind = Factory::indentation(ctx.indentSpaces);
        for (const FieldDesc& member : kSplineTerrainFields) {
            const void* value = member.ref(settings);
            if (member.ops->equals(value, member.ref(const_cast<SplineTerrainSettings*>(&defaults)), 0.0f) || !member.ops->cppLiteral) continue;
            out += ind + ctx.variable + ".terrain." + member.name + " = " + member.ops->cppLiteral(member, value, ctx) + ";\n";
        }
    }

    // ---------------------------------------------------------------- Spline Extrude

    bool isShape(const void* owner, SplineExtrudeShape shape) { return static_cast<const SplineExtrudeComponent*>(owner)->shape == shape; }

    constexpr FieldDesc kSplineProfilePointFields[] = {
        JE_FIELD(SplineProfilePoint, position, .yamlKey = "p", .unit = Unit::Meters, .tooltip = "Across (right of travel) and up."),
        JE_FIELD(SplineProfilePoint, hard, .flags = kOmit, .tooltip = "A crease: its two faces keep their own normals."),
        JE_FIELD(SplineProfilePoint, grounded, .flags = kOmit, .tooltip = "Follows the ground (a wall's base)."),
    };

}

const StructDesc kSplineProfilePointStruct = JE_STRUCT_DESC(SplineProfilePoint, "SplineProfilePoint", kSplineProfilePointFields);

namespace {

    // SplineSystem rebuilds when these values change (no update flag)
    constexpr FieldDesc kSplineExtrudeFields[] = {
        JE_FIELD(SplineExtrudeComponent, enabled),
        JE_FIELD(SplineExtrudeComponent, shape, .enumDesc = &enums::kSplineExtrudeShape, .flags = kOmit),
        JE_FIELD(SplineExtrudeComponent, widthSource, .enumDesc = &enums::kSplineWidthSource, .flags = kOmit, .label = "Width From"),
        JE_FIELD(SplineExtrudeComponent, width, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "The full width with Width From Constant."),
        JE_FIELD(SplineExtrudeComponent, edgeDrop, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters,
                 .visibleIf = [](const void* o) { return isShape(o, SplineExtrudeShape::Bevel); }),
        JE_FIELD(SplineExtrudeComponent, edgeInset, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f,
                 .visibleIf = [](const void* o) { return isShape(o, SplineExtrudeShape::Bevel); }),
        JE_FIELD(SplineExtrudeComponent, crownRise, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters,
                 .visibleIf = [](const void* o) { return isShape(o, SplineExtrudeShape::Crown); }),
        JE_FIELD(SplineExtrudeComponent, shoulderWidth, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f,
                 .visibleIf = [](const void* o) { return isShape(o, SplineExtrudeShape::Crown); }),
        JE_FIELD(SplineExtrudeComponent, shoulderDrop, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters,
                 .visibleIf = [](const void* o) { return isShape(o, SplineExtrudeShape::Crown); }),
        JE_FIELD(SplineExtrudeComponent, thickness, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f,
                 .visibleIf = [](const void* o) { return isShape(o, SplineExtrudeShape::Rectangle); }),
        JE_FIELD(SplineExtrudeComponent, height, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f,
                 .visibleIf = [](const void* o) { return isShape(o, SplineExtrudeShape::Rectangle); }),
        JE_FIELD(SplineExtrudeComponent, grade, .enumDesc = &enums::kSplineGrade, .flags = kOmit,
                 .visibleIf = [](const void* o) { return isShape(o, SplineExtrudeShape::Rectangle); }),
        JE_FIELD(SplineExtrudeComponent, stepHeight, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f,
                 .visibleIf = [](const void* o) { return isShape(o, SplineExtrudeShape::Rectangle); }),
        JE_FIELD(SplineExtrudeComponent, tubeSides, .flags = kOmit, .min = 3.0f, .label = "Sides",
                 .visibleIf = [](const void* o) { return isShape(o, SplineExtrudeShape::Tube); }),
        JE_FIELD(SplineExtrudeComponent, customProfile, .ops = &kProfileListOps, .flags = kOmit | ListWhole | ListEachElement | ListTemplate,
                 .tooltip = "At most 32 points, left to right."),
        JE_FIELD(SplineExtrudeComponent, customClosed, .flags = kOmit, .label = "Closed Profile"),
        JE_FIELD(SplineExtrudeComponent, maxHalfWidth, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "Water Banks: the flood stops this far from the centreline."),
        JE_FIELD(SplineExtrudeComponent, waterEdgeDrop, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f,
                 .label = "Edge Drop", .tooltip = "Water Banks: how far the rim is buried under the banks."),
        JE_FIELD(SplineExtrudeComponent, seaLevelFloor, .flags = kOmit),
        JE_FIELD(SplineExtrudeComponent, seaLevel, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters),
        JE_FIELD(SplineExtrudeComponent, flow, .flags = kOmit, .tooltip = "Writes the flow library:water's FLOW reads (vertex colour)."),
        JE_FIELD(SplineExtrudeComponent, flowSpeed, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .min = 0.0f, .max = 1.0f),
        JE_FIELD(SplineExtrudeComponent, tilesPerMetreU, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .min = 0.0f),
        JE_FIELD(SplineExtrudeComponent, tilesPerMetreV, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .min = 0.0f),
        JE_FIELD(SplineExtrudeComponent, levelCourseV, .flags = kOmit, .label = "Level Courses", .tooltip = "Walls: V from world height."),
        JE_FIELD(SplineExtrudeComponent, corners, .enumDesc = &enums::kSplineCornerStyle, .flags = kOmit, .tooltip = "At the knots of a Linear spline."),
        JE_FIELD(SplineExtrudeComponent, endCaps, .flags = kOmit),
        JE_FIELD(SplineExtrudeComponent, endTaper, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(SplineExtrudeComponent, lateralOffset, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters),
        JE_FIELD(SplineExtrudeComponent, verticalOffset, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters),
        JE_FIELD(SplineExtrudeComponent, conform, .enumDesc = &enums::kSplineConform, .flags = kOmit),
        JE_FIELD(SplineExtrudeComponent, slopeBlend, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .min = 0.0f, .max = 1.0f),
        JE_FIELD(SplineExtrudeComponent, stationSpacing, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.05f),
        JE_FIELD(SplineExtrudeComponent, collision, .enumDesc = &enums::kSplineCollision, .flags = kOmit),
    };

    // ---------------------------------------------------------------- Spline Instantiate

    constexpr FieldDesc kSplinePoolEntryFields[] = {
        JE_FIELD(SplinePoolEntry, asset, .asset = AssetKind::Any, .tooltip = "A model (instanced) or a prefab (GameObjects)."),
        JE_FIELD(SplinePoolEntry, weight, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .min = 0.0f),
    };

    constexpr FieldDesc kSplineSpanOverrideFields[] = {
        JE_FIELD(SplineSpanOverride, knot, .tooltip = "The knot the run starts at."),
        JE_FIELD(SplineSpanOverride, span, .tooltip = "The span of that run."),
        JE_FIELD(SplineSpanOverride, kind, .enumDesc = &enums::kSplineSpanOverrideKind),
        JE_FIELD(SplineSpanOverride, piece, .tooltip = "The pool entry (gates for Gate, spans for Piece)."),
    };

}

const StructDesc kSplinePoolEntryStruct = JE_STRUCT_DESC(SplinePoolEntry, "SplinePoolEntry", kSplinePoolEntryFields);
const StructDesc kSplineSpanOverrideStruct = JE_STRUCT_DESC(SplineSpanOverride, "SplineSpanOverride", kSplineSpanOverrideFields);

namespace {

    constexpr uint32_t kPool = kOmit | ListWhole | ListEachElement | ListTemplate;

    constexpr FieldDesc kSplineInstantiateFields[] = {
        JE_FIELD(SplineInstantiateComponent, enabled),
        JE_FIELD(SplineInstantiateComponent, mode, .enumDesc = &enums::kSplineInstantiateMode, .flags = kOmit),
        JE_FIELD(SplineInstantiateComponent, items, .ops = &kPoolListOps, .flags = kPool, .tooltip = "Spacing and Tile pieces."),
        JE_FIELD(SplineInstantiateComponent, posts, .ops = &kPoolListOps, .flags = kPool),
        JE_FIELD(SplineInstantiateComponent, spans, .ops = &kPoolListOps, .flags = kPool),
        JE_FIELD(SplineInstantiateComponent, gates, .ops = &kPoolListOps, .flags = kPool),
        JE_FIELD(SplineInstantiateComponent, crests, .ops = &kPoolListOps, .flags = kPool, .tooltip = "Along the tops of the spans."),
        JE_FIELD(SplineInstantiateComponent, caps, .ops = &kPoolListOps, .flags = kPool, .tooltip = "Close the remainders beside posts and gates."),
        JE_FIELD(SplineInstantiateComponent, spacing, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(SplineInstantiateComponent, fit, .enumDesc = &enums::kSplineSpacingFit, .flags = kOmit),
        JE_FIELD(SplineInstantiateComponent, postPitch, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(SplineInstantiateComponent, spanMaxStretch, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .min = 1.0f),
        JE_FIELD(SplineInstantiateComponent, fenceGrade, .enumDesc = &enums::kSplineGrade, .flags = kOmit, .label = "Grade"),
        JE_FIELD(SplineInstantiateComponent, crestPitch, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "Between crests (0: none)."),
        JE_FIELD(SplineInstantiateComponent, spanOverrides, .ops = &kOverrideListOps, .flags = kPool),
        JE_FIELD(SplineInstantiateComponent, lateralOffset, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters),
        JE_FIELD(SplineInstantiateComponent, verticalOffset, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters),
        JE_FIELD(SplineInstantiateComponent, spacingJitter, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(SplineInstantiateComponent, yawJitter, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Degrees, .min = 0.0f),
        JE_FIELD(SplineInstantiateComponent, lateralJitter, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(SplineInstantiateComponent, scaleRange, .flags = kOmit),
        JE_FIELD(SplineInstantiateComponent, dropout, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .min = 0.0f, .max = 1.0f),
        JE_FIELD(SplineInstantiateComponent, seed, .flags = kOmit),
        JE_FIELD(SplineInstantiateComponent, endTrim, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(SplineInstantiateComponent, plantMode, .enumDesc = &enums::kSplinePlantMode, .flags = kOmit, .label = "Plant"),
        JE_FIELD(SplineInstantiateComponent, maxTilt, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Degrees, .min = 0.0f),
        JE_FIELD(SplineInstantiateComponent, conform, .enumDesc = &enums::kSplineConform, .flags = kOmit),
        JE_FIELD(SplineInstantiateComponent, slopeBlend, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .min = 0.0f, .max = 1.0f),
        JE_FIELD(SplineInstantiateComponent, stationSpacing, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.05f),
    };

    // ---------------------------------------------------------------- Spline Animate

    // the runtime state (playing, elapsed, distance...) and onComplete are no fields
    constexpr FieldDesc kSplineAnimateFields[] = {
        JE_FIELD(SplineAnimateComponent, enabled),
        JE_FIELD(SplineAnimateComponent, spline, .ops = &ops::Entity, .flags = kOmit, .tooltip = "The GameObject with the Spline."),
        JE_FIELD(SplineAnimateComponent, method, .enumDesc = &enums::kSplineAnimateMethod, .flags = kOmit),
        JE_FIELD(SplineAnimateComponent, duration, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Seconds, .min = 0.0f),
        JE_FIELD(SplineAnimateComponent, speed, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::MetersPerSecond, .min = 0.0f),
        JE_FIELD(SplineAnimateComponent, loop, .enumDesc = &enums::kSplineLoop, .flags = kOmit),
        JE_FIELD(SplineAnimateComponent, easing, .enumDesc = &enums::kSplineEasing, .flags = kOmit),
        JE_FIELD(SplineAnimateComponent, alignment, .enumDesc = &enums::kSplineAlignment, .flags = kOmit),
        JE_FIELD(SplineAnimateComponent, startOffset, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .min = 0.0f, .max = 1.0f,
                 .tooltip = "Where it starts, as a share of the length."),
        JE_FIELD(SplineAnimateComponent, lateralOffset, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters),
        JE_FIELD(SplineAnimateComponent, verticalOffset, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters),
        JE_FIELD(SplineAnimateComponent, playOnAwake, .flags = kOmit),
    };

}

const ComponentDesc kSplineDesc{
    .type = ComponentType::SplineComponent, .cppName = "SplineComponent", .key = "spline", .displayName = "Spline",
    .icon = ICON_FA_BEZIER_CURVE,
    .body = JE_STRUCT_DESC(SplineComponent, "SplineComponent", kSplineFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(SplineComponent),
    .postDecode = &splinePostDecode,
    .codegenVar = "spline", .codegenExtra = &splineCodegen,
};

const ComponentDesc kSplineExtrudeDesc{
    .type = ComponentType::SplineExtrudeComponent, .cppName = "SplineExtrudeComponent", .key = "splineextrude",
    .displayName = "Spline Extrude", .icon = ICON_FA_ROAD,
    .body = JE_STRUCT_DESC(SplineExtrudeComponent, "SplineExtrudeComponent", kSplineExtrudeFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(SplineExtrudeComponent),
    .codegenVar = "splineExtrude",
};

const ComponentDesc kSplineInstantiateDesc{
    .type = ComponentType::SplineInstantiateComponent, .cppName = "SplineInstantiateComponent", .key = "splineinstantiate",
    .displayName = "Spline Instantiate", .icon = ICON_FA_TREE,
    .body = JE_STRUCT_DESC(SplineInstantiateComponent, "SplineInstantiateComponent", kSplineInstantiateFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(SplineInstantiateComponent),
    .codegenVar = "splineInstantiate",
};

const ComponentDesc kSplineAnimateDesc{
    .type = ComponentType::SplineAnimateComponent, .cppName = "SplineAnimateComponent", .key = "splineanimate",
    .displayName = "Spline Animate", .icon = ICON_FA_ROUTE,
    .body = JE_STRUCT_DESC(SplineAnimateComponent, "SplineAnimateComponent", kSplineAnimateFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(SplineAnimateComponent),
    .codegenVar = "splineAnimate",
};

}
