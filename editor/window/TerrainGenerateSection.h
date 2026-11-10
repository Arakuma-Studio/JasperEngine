// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Terrain window's collapsible Generate section (T2 R60): Noise Graph (a preset list and an asset picker),
// Seed with a dice, the graph's exposed variables, Apply To, Region with Feather, Preview (no write: a
// hillshaded image, min, max, slope histogram), Apply (one undo step), a progress bar and Cancel. It is one
// call from TerrainEditWindow so that window's merges stay clean (D2); everything else lives in the .cpp.
// Collapsed, it has no job and no texture (R50). The last graph and seed persist in TerrainEditorSettings.
// Main thread.

#pragma once

#include "ecs/Entity.h"

#include <cstdint>

namespace jasperengine::editor {

    class Project;

    void drawTerrainGenerateSection(Project* project, uint32_t sceneId, Entity entity);

}
