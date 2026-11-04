// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The table of a Terrain Layer (RenderReflect.cpp), which a Voxel Volume's slots hold too (T6 R43:
// the same struct and field names, so the panels look alike).

#include "reflect/Components.h"

#include "component/TerrainComponent.h"

namespace jasperengine::editor::reflect {

    JE_REFLECT_STRUCT_TRAITS(TerrainSurfaceLayer, kTerrainSurfaceLayerStruct);

}
