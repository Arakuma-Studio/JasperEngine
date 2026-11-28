// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Outline component (docs/specs/S94-rendering-anim-gaps.md F22) and its enums, with
// QuickOutline's names. Field order is the YAML key order.

#include "reflect/Components.h"

#include "component/OutlineComponent.h"

namespace jasperengine::editor::reflect {

namespace enums {

    namespace {
        constexpr int outlineValue(OutlineMode mode) { return static_cast<int>(mode); }
        constexpr int outlineValue(OutlineWidthUnits units) { return static_cast<int>(units); }

        constexpr EnumItem kOutlineModeItems[] = {
            {outlineValue(OutlineMode::OUTLINE_ALL), "outline_all", "Outline All", "OutlineMode::OUTLINE_ALL"},
            {outlineValue(OutlineMode::OUTLINE_VISIBLE), "outline_visible", "Outline Visible", "OutlineMode::OUTLINE_VISIBLE"},
            {outlineValue(OutlineMode::OUTLINE_HIDDEN), "outline_hidden", "Outline Hidden", "OutlineMode::OUTLINE_HIDDEN"},
            {outlineValue(OutlineMode::OUTLINE_AND_SILHOUETTE), "outline_and_silhouette", "Outline And Silhouette",
             "OutlineMode::OUTLINE_AND_SILHOUETTE"},
            {outlineValue(OutlineMode::SILHOUETTE_ONLY), "silhouette_only", "Silhouette Only", "OutlineMode::SILHOUETTE_ONLY"},
        };

        constexpr EnumItem kOutlineWidthUnitsItems[] = {
            {outlineValue(OutlineWidthUnits::PIXELS), "pixels", "Pixels", "OutlineWidthUnits::PIXELS"},
            {outlineValue(OutlineWidthUnits::WORLD), "world", "World", "OutlineWidthUnits::WORLD"},
        };
    }

    const EnumDesc kOutlineMode{"OutlineMode", kOutlineModeItems, outlineValue(OutlineMode::OUTLINE_ALL)};
    const EnumDesc kOutlineWidthUnits{"OutlineWidthUnits", kOutlineWidthUnitsItems, outlineValue(OutlineWidthUnits::PIXELS)};

}

namespace {

    constexpr FieldDesc kOutlineFields[] = {
        JE_FIELD(OutlineComponent, enabled),
        JE_FIELD(OutlineComponent, mode, .enumDesc = &enums::kOutlineMode, .flags = OmitIfDefault, .label = "Outline Mode"),
        JE_FIELD(OutlineComponent, color, .flags = OmitIfDefault | ColorLinear | Hdr, .label = "Outline Color"),
        JE_FIELD(OutlineComponent, width, .ops = &ops::NonNegativeFiniteFloat, .flags = OmitIfDefault, .min = 0.0f,
                 .label = "Outline Width"),
        JE_FIELD(OutlineComponent, widthUnits, .enumDesc = &enums::kOutlineWidthUnits, .flags = OmitIfDefault,
                 .label = "Width Units"),
        JE_FIELD(OutlineComponent, includeChildren, .flags = OmitIfDefault, .label = "Include Children"),
    };

}

const ComponentDesc kOutlineDesc{
    .type = ComponentType::OutlineComponent, .cppName = "OutlineComponent", .key = "outline",
    .displayName = "Outline", .icon = ICON_FA_VECTOR_SQUARE,
    .body = JE_STRUCT_DESC(OutlineComponent, "OutlineComponent", kOutlineFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(OutlineComponent),
};

}
