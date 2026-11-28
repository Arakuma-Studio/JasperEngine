// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Vertex Colors component (docs/specs/V1-vertex-colors.md R1, R5-R7, R15) on the generic codec.
// Values at their defaults are not written, so a scene without one is byte-identical and one with a
// fresh component is a single line (R130). The colors are the .vcolors asset's; the decoded data,
// revisions and gameplay layers are runtime members and no fields here.

#include "reflect/Components.h"

#include "component/VertexColorsComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    template<typename E>
    constexpr int vertexColorValue(E value) { return static_cast<int>(value); }

    constexpr EnumItem kVertexColorFormatItems[] = {
        {vertexColorValue(vertexcolor::Format::RGBA8), "rgba8", "RGBA8", "vertexcolor::Format::RGBA8"},
        {vertexColorValue(vertexcolor::Format::RGBA16F), "rgba16f", "High Precision (RGBA16F)", "vertexcolor::Format::RGBA16F"},
    };
    constexpr EnumItem kVertexColorInitialValueItems[] = {
        {vertexColorValue(vertexcolor::InitialValue::Asset), "asset", "Asset Colors", "vertexcolor::InitialValue::Asset"},
        {vertexColorValue(vertexcolor::InitialValue::White), "white", "White", "vertexcolor::InitialValue::White"},
        {vertexColorValue(vertexcolor::InitialValue::Black), "black", "Black", "vertexcolor::InitialValue::Black"},
        {vertexColorValue(vertexcolor::InitialValue::Custom), "custom", "Custom", "vertexcolor::InitialValue::Custom"},
    };

}

namespace enums {
    const EnumDesc kVertexColorFormat{"VertexColorFormat", kVertexColorFormatItems, vertexColorValue(vertexcolor::Format::RGBA8)};
    const EnumDesc kVertexColorInitialValue{"VertexColorInitialValue", kVertexColorInitialValueItems, vertexColorValue(vertexcolor::InitialValue::Asset)};
}

namespace {

    constexpr uint32_t kVertexColorsOmit = OmitIfDefault | EmptyIsDefault;
    constexpr uint64_t kVertexColorsUpdate = UpdateFlags_VertexColors;

    bool vertexColorsCustomInitialVisible(const void* owner) {
        return static_cast<const VertexColorsComponent*>(owner)->initialValue == vertexcolor::InitialValue::Custom;
    }

    constexpr FieldDesc kVertexColorsFields[] = {
        JE_FIELD(VertexColorsComponent, enabled, .flags = kVertexColorsOmit, .updateFlags = kVertexColorsUpdate,
                 .tooltip = "Off: the mesh draws as without vertex colors, at no cost."),
        JE_FIELD(VertexColorsComponent, data, .flags = kVertexColorsOmit, .updateFlags = kVertexColorsUpdate, .asset = AssetKind::VertexColors, .label = "Data",
                 .tooltip = "The .vcolors asset holding this object's colors. Empty until the first edit creates it."),
        JE_FIELD(VertexColorsComponent, format, .enumDesc = &enums::kVertexColorFormat, .flags = kVertexColorsOmit, .updateFlags = kVertexColorsUpdate,
                 .tooltip = "How the colors are stored. High Precision keeps values above 1 and fine data channels."),
        JE_FIELD(VertexColorsComponent, initialValue, .enumDesc = &enums::kVertexColorInitialValue, .flags = kVertexColorsOmit, .updateFlags = kVertexColorsUpdate, .label = "Initial Value",
                 .tooltip = "What a mesh holds before its first edit, and what Erase goes back to. Asset Colors: the mesh's own "
                            "colors, or white."),
        JE_FIELD(VertexColorsComponent, customInitial, .flags = kVertexColorsOmit | ColorLinear | Hdr, .updateFlags = kVertexColorsUpdate, .label = "Custom Initial",
                 .visibleIf = &vertexColorsCustomInitialVisible),
        JE_FIELD(VertexColorsComponent, channelNames, .flags = kVertexColorsOmit | ListElements, .label = "Channel Names",
                 .tooltip = "Names for R, G, B and A (\"Moss\", \"Wetness\"); empty: unnamed."),
    };

}

const ComponentDesc kVertexColorsDesc{
    .type = ComponentType::VertexColorsComponent, .cppName = "VertexColorsComponent", .key = "vertexcolors",
    .displayName = "Vertex Colors", .icon = ICON_FA_PALETTE,
    .body = JE_STRUCT_DESC(VertexColorsComponent, "VertexColorsComponent", kVertexColorsFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(VertexColorsComponent),
};

}
