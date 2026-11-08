// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Voxel Volume component (docs/specs/T6-voxel-volumes.md R1) on the generic codec. Values at
// their defaults are not written, so a volume in a scene is a few lines and a scene without one is
// byte-identical (R80). Dimensions, voxel size and the blocks belong to the .voxelvolume asset; the
// runtime fields (events, the debris count) are no fields here.

#include "reflect/Components.h"
#include "reflect/components/TerrainLayerReflect.h"

#include "Factory.h"
#include "component/VoxelVolumeComponent.h"
#include "serialization/codecs/VoxelVolumeCodec.h"

namespace jasperengine::editor::reflect {

    JE_REFLECT_STRUCT_TRAITS(VoxelMaterialSlot, kVoxelMaterialSlotStruct);

namespace {

    template<typename E>
    constexpr int v(E value) { return static_cast<int>(value); }

    constexpr EnumItem kVoxelQualityItems[] = {
        {v(VoxelQuality::Auto), "auto", "Auto", "VoxelQuality::Auto"},
        {v(VoxelQuality::High), "high", "High", "VoxelQuality::High"},
        {v(VoxelQuality::Medium), "medium", "Medium", "VoxelQuality::Medium"},
        {v(VoxelQuality::Low), "low", "Low", "VoxelQuality::Low"},
    };
    constexpr EnumItem kVoxelNavigationUpdateItems[] = {
        {v(VoxelNavigationUpdate::Off), "off", "Off", "VoxelNavigationUpdate::Off"},
        {v(VoxelNavigationUpdate::Automatic), "automatic", "Automatic", "VoxelNavigationUpdate::Automatic"},
    };
    constexpr EnumItem kVoxelTerrainCutItems[] = {
        {v(VoxelTerrainCut::Off), "off", "Off", "VoxelTerrainCut::Off"},
        {v(VoxelTerrainCut::Automatic), "automatic", "Automatic", "VoxelTerrainCut::Automatic"},
    };

}

namespace enums {
    const EnumDesc kVoxelQuality{"VoxelQuality", kVoxelQualityItems, v(VoxelQuality::Auto)};
    const EnumDesc kVoxelNavigationUpdate{"VoxelNavigationUpdate", kVoxelNavigationUpdateItems, v(VoxelNavigationUpdate::Automatic)};
    const EnumDesc kVoxelTerrainCut{"VoxelTerrainCut", kVoxelTerrainCutItems, v(VoxelTerrainCut::Off)};
}

namespace {

    constexpr uint32_t kOmit = OmitIfDefault | EmptyIsDefault | CodegenIfChanged;
    constexpr uint32_t kOmitFloat = OmitIfDefault | OmitIfNonFinite | EmptyIsDefault | CodegenIfChanged;

    constexpr FieldDesc kVoxelMaterialSlotFields[] = {
        JE_FIELD(VoxelMaterialSlot, name, .flags = kOmit),
        JE_FIELD(VoxelMaterialSlot, layer, .flags = kOmit),
        JE_FIELD(VoxelMaterialSlot, linkedTerrainLayer, .flags = kOmit, .min = -1.0f, .label = "Terrain Layer",
                 .tooltip = "The Terrain Layer this slot follows live (-1: its own)."),
    };

}

const StructDesc kVoxelMaterialSlotStruct = JE_STRUCT_DESC(VoxelMaterialSlot, "VoxelMaterialSlot", kVoxelMaterialSlotFields);

namespace {

    constexpr FieldDesc kVoxelVolumeFields[] = {
        JE_FIELD(VoxelVolumeComponent, enabled, .flags = kOmit),
        JE_FIELD(VoxelVolumeComponent, volumeData, .flags = OmitIfDefault | EmptyIsDefault | CodegenIfChanged, .label = "Volume Data",
                 .tooltip = "The .voxelvolume asset: dimensions, voxel size and the blocks."),
        // exported by voxelCodegen: a slot's layer holds textures, which take statements
        JE_FIELD(VoxelVolumeComponent, materialSlots, .flags = OmitIfDefault | EmptyIsDefault | NoCodegen | ListWhole | ListTemplate,
                 .label = "Material Slots"),
        JE_FIELD(VoxelVolumeComponent, quality, .enumDesc = &enums::kVoxelQuality, .flags = kOmit,
                 .tooltip = "Auto: the platform's, or the attached terrain's."),
        JE_FIELD(VoxelVolumeComponent, lodLevels, .flags = kOmit | Clamped, .min = 0.0f, .max = 4.0f, .label = "LOD Levels",
                 .tooltip = "0: from Quality."),
        JE_FIELD(VoxelVolumeComponent, lodRadius, .flags = kOmit | Clamped, .min = 4.0f, .max = 8.0f, .label = "LOD Radius",
                 .tooltip = "Blocks per LOD level around the focus: 4, 6 or 8."),
        JE_FIELD(VoxelVolumeComponent, maxDrawDistance, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "0: none."),
        JE_FIELD(VoxelVolumeComponent, castShadows, .flags = kOmit),
        JE_FIELD(VoxelVolumeComponent, collision, .flags = kOmit),
        JE_FIELD(VoxelVolumeComponent, collisionRadius, .flags = kOmit | Clamped, .min = 0.0f, .tooltip = "Blocks around what moves; 0: from Quality."),
        JE_FIELD(VoxelVolumeComponent, rescueBodies, .flags = kOmit, .tooltip = "Bodies a dig leaves inside solid come back out."),
        JE_FIELD(VoxelVolumeComponent, navigationUpdate, .enumDesc = &enums::kVoxelNavigationUpdate, .flags = kOmit,
                 .tooltip = "Automatic: a NavMesh Surface rebakes what an edit changed."),
        JE_FIELD(VoxelVolumeComponent, terrain, .ops = &ops::Entity, .flags = kOmit, .tooltip = "The Terrain it is attached to; none: free-standing."),
        JE_FIELD(VoxelVolumeComponent, terrainCut, .enumDesc = &enums::kVoxelTerrainCut, .flags = kOmit,
                 .tooltip = "Automatic: cuts Terrain holes where the volume breaks its surface."),
        JE_FIELD(VoxelVolumeComponent, overlapDepth, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters,
                 .tooltip = "How far the volume's surface sits under the terrain's. Negative: 0.02 x Voxel Size."),
        JE_FIELD(VoxelVolumeComponent, cutTolerance, .ops = &ops::FiniteFloat, .flags = kOmitFloat, .unit = Unit::Meters,
                 .tooltip = "A surface this close to the terrain's cuts no hole. Negative: 0.25 x Voxel Size."),
        JE_FIELD(VoxelVolumeComponent, focus, .ops = &ops::Entity, .flags = kOmit, .tooltip = "What LOD follows; none: the active camera."),
        JE_FIELD(VoxelVolumeComponent, runtimeGeneration, .flags = kOmit,
                 .tooltip = "Keep the generator in the build and evaluate it in the player, instead of shipping baked blocks."),
    };

    void voxelPostDecode(void* comp, const DecodeContext&) {
        sanitizeVoxelVolume(*static_cast<VoxelVolumeComponent*>(comp));
    }

    // each slot as its own block: a layer's textures are statements (Factory::formatTexture), so
    // the slot cannot be one aggregate literal
    void voxelCodegen(const void* comp, const CodegenContext& ctx, std::string& out) {
        const VoxelVolumeComponent& volume = *static_cast<const VoxelVolumeComponent*>(comp);
        if (volume.materialSlots.empty()) return;
        const std::string ind = Factory::indentation(ctx.indentSpaces);
        out += ind + ctx.variable + ".materialSlots.resize(" + std::to_string(volume.materialSlots.size()) + ");\n";
        const TerrainSurfaceLayer defaults;
        for (size_t i = 0; i < volume.materialSlots.size(); i++) {
            const VoxelMaterialSlot& slot = volume.materialSlots[i];
            const std::string target = ctx.variable + ".materialSlots[" + std::to_string(i) + "]";
            if (!slot.name.empty()) out += ind + target + ".name = " + Factory::formatString(slot.name) + ";\n";
            if (slot.linkedTerrainLayer != -1) out += ind + target + ".linkedTerrainLayer = " + std::to_string(slot.linkedTerrainLayer) + ";\n";
            for (const FieldDesc& member : kTerrainSurfaceLayerStruct.fields) {
                void* value = member.ref(const_cast<TerrainSurfaceLayer*>(&slot.layer));
                if (member.ops->equals(value, member.ref(const_cast<TerrainSurfaceLayer*>(&defaults)), 0.0f)) continue;
                const std::string name = target + ".layer." + (member.cppPath ? member.cppPath : member.name);
                if (member.ops->cppStatements) member.ops->cppStatements(member, value, name, ctx, out);
                else if (member.ops->cppLiteral) out += ind + name + " = " + member.ops->cppLiteral(member, value, ctx) + ";\n";
            }
        }
    }

}

const ComponentDesc kVoxelVolumeDesc{
    .type = ComponentType::VoxelVolumeComponent, .cppName = "VoxelVolumeComponent", .key = "voxelvolume",
    .displayName = "Voxel Volume", .icon = ICON_FA_CUBES,
    .body = JE_STRUCT_DESC(VoxelVolumeComponent, "VoxelVolumeComponent", kVoxelVolumeFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(VoxelVolumeComponent),
    .postDecode = &voxelPostDecode,
    .codegenVar = "voxelVolume", .codegenExtra = &voxelCodegen,
};

}
