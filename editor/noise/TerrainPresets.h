// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The built-in Noise Graphs (T2 R34): embedded at configure time from editor/resources/noisegraphs,
// listed for the Generate section, Create Terrain and Project window > Create > Noise Graph.

#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TerrainPreset {
        std::string id;             // the file's name: "mountains"
        std::string name;           // "Mountains"
        std::string description;
        std::vector<std::string> tags;
        float terrainSize = 1000.0f;    // recommended, metres
        float maxHeight = 100.0f;       // recommended, metres
        bool hidden = false;            // a utility graph: not offered by Create Terrain
    };

    class TerrainPresets {
    public:
        // Every preset, in file order. includeHidden: the utility graphs too.
        static std::vector<TerrainPreset> list(bool includeHidden = false);
        static bool find(const std::string& id, TerrainPreset& out);
        // The file's text; empty for an unknown id
        static std::string json(const std::string& id);
        // A graph with a single output_height on a Constant (Project window's Blank)
        static std::string blankJson();
    };

}
