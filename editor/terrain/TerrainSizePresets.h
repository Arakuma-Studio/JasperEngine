// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T1 §6 Table 6: the sizes a new terrain starts from, one list for the Create Terrain dialog (R58)
// and edit_terrain's create (R68)

namespace jasperengine::editor {

    struct TerrainSizePreset {
        const char* id;
        const char* label;
        float size;             // metres
        float height;           // metres
        int heightmap;          // texels a side
        int control;            // blend map texels
        int detail;             // density map texels
        float meshResolution;
        int root;
        int levels;
    };

    inline constexpr TerrainSizePreset TERRAIN_SIZE_PRESETS[] = {
        {"test_patch", "Test Patch", 64.0f, 16.0f, 256, 256, 256, 16.0f, 1, 5},
        {"mobile", "Mobile", 512.0f, 100.0f, 512, 512, 256, 16.0f, 2, 5},
        {"small", "Small", 256.0f, 64.0f, 512, 512, 512, 32.0f, 2, 4},
        {"medium", "Medium", 1024.0f, 200.0f, 1024, 1024, 1024, 32.0f, 2, 5},
        {"large", "Large", 2048.0f, 400.0f, 2048, 1024, 1024, 32.0f, 2, 6},
        {"huge", "Huge", 4096.0f, 800.0f, 4096, 2048, 2048, 32.0f, 4, 6},
    };

}
