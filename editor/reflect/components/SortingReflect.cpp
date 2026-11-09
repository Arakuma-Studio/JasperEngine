// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// 2D sorting and Sprite Batch (docs/specs/S52-sprite-instancing.md): the Sorting Group and Sprite
// Batch components and their enums. Field order is the YAML key order.

#include "reflect/Components.h"

#include "component/SortingGroupComponent.h"
#include "component/SpriteBatchComponent.h"
#include "component/SpriteComponent.h"

namespace jasperengine::editor::reflect {

namespace enums {

    namespace {
        constexpr int sortingValue(SpriteSortPoint point) { return static_cast<int>(point); }
        constexpr int sortingValue(SpriteBatchMode mode) { return static_cast<int>(mode); }
        constexpr int sortingValue(SpriteBatchSpace space) { return static_cast<int>(space); }

        constexpr EnumItem kSpriteSortPointItems[] = {
            {sortingValue(SpriteSortPoint::CENTER), "center", "Center", "SpriteSortPoint::CENTER"},
            {sortingValue(SpriteSortPoint::PIVOT), "pivot", "Pivot", "SpriteSortPoint::PIVOT"},
        };

        constexpr EnumItem kSpriteBatchModeItems[] = {
            {sortingValue(SpriteBatchMode::RETAINED), "retained", "Retained", "SpriteBatchMode::RETAINED"},
            {sortingValue(SpriteBatchMode::CLEAR_EACH_FRAME), "clear_each_frame", "Clear Each Frame", "SpriteBatchMode::CLEAR_EACH_FRAME"},
        };

        constexpr EnumItem kSpriteBatchSpaceItems[] = {
            {sortingValue(SpriteBatchSpace::LOCAL), "local", "Local", "SpriteBatchSpace::LOCAL"},
            {sortingValue(SpriteBatchSpace::WORLD), "world", "World", "SpriteBatchSpace::WORLD"},
        };
    }

    const EnumDesc kSpriteSortPoint{"SpriteSortPoint", kSpriteSortPointItems, sortingValue(SpriteSortPoint::CENTER)};
    const EnumDesc kSpriteBatchMode{"SpriteBatchMode", kSpriteBatchModeItems, sortingValue(SpriteBatchMode::RETAINED)};
    const EnumDesc kSpriteBatchSpace{"SpriteBatchSpace", kSpriteBatchSpaceItems, sortingValue(SpriteBatchSpace::LOCAL)};

}

namespace {

    constexpr FieldDesc kSortingGroupFields[] = {
        JE_FIELD(SortingGroupComponent, enabled, .flags = CodegenIfChanged),
        JE_FIELD(SortingGroupComponent, sortingLayer, .flags = OmitIfDefault | CodegenIfChanged, .min = 0.0f, .label = "Sorting Layer",
                 .drawer = "sortingLayer"),
        JE_FIELD(SortingGroupComponent, sortingOrder, .flags = OmitIfDefault | CodegenIfChanged, .min = -32768.0f, .max = 32767.0f,
                 .label = "Order in Layer"),
    };

    constexpr const char* kSortingGroupCodegenOrder[] = {"sortingLayer", "sortingOrder", "enabled"};

    // instances are runtime data: their count is shown, never saved or written
    constexpr uint32_t kRuntimeCount = Transient | ReadOnly | NoMcpWrite | NoDefault | NoCodegen;

    constexpr FieldDesc kSpriteBatchFields[] = {
        JE_FIELD(SpriteBatchComponent, enabled, .flags = CodegenIfChanged),
        JE_FIELD(SpriteBatchComponent, mode, .enumDesc = &enums::kSpriteBatchMode, .flags = OmitIfDefault | CodegenIfChanged),
        JE_FIELD(SpriteBatchComponent, space, .enumDesc = &enums::kSpriteBatchSpace, .flags = OmitIfDefault | CodegenIfChanged),
        JE_FIELD(SpriteBatchComponent, capacity, .flags = OmitIfDefault | CodegenIfChanged, .min = 1.0f, .max = 1048576.0f),
        JE_FIELD(SpriteBatchComponent, sortInstances, .flags = OmitIfDefault | CodegenIfChanged, .label = "Sort Instances"),
        JE_FIELD(SpriteBatchComponent, count, .flags = kRuntimeCount, .label = "Instances"),
    };

    constexpr const char* kSpriteBatchCodegenOrder[] = {"mode", "space", "capacity", "sortInstances", "enabled"};

}

const ComponentDesc kSortingGroupDesc{
    .type = ComponentType::SortingGroupComponent, .cppName = "SortingGroupComponent", .key = "sortinggroup",
    .displayName = "Sorting Group", .icon = ICON_FA_LAYER_GROUP,
    .body = JE_STRUCT_DESC(SortingGroupComponent, "SortingGroupComponent", kSortingGroupFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(SortingGroupComponent),
    .codegenVar = "sortingGroup", .codegenOrder = kSortingGroupCodegenOrder,
};

const ComponentDesc kSpriteBatchDesc{
    .type = ComponentType::SpriteBatchComponent, .cppName = "SpriteBatchComponent", .key = "spritebatch",
    .displayName = "Sprite Batch", .icon = ICON_FA_TABLE_CELLS_LARGE,
    .body = JE_STRUCT_DESC(SpriteBatchComponent, "SpriteBatchComponent", kSpriteBatchFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(SpriteBatchComponent),
    .codegenVar = "spriteBatch", .codegenOrder = kSpriteBatchCodegenOrder,
};

}
