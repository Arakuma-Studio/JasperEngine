// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainPresets.h"

#include "graph/Graph.h"
#include "noise_presets.gen.h"

#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const NoisePresetEmbeddedFile* findFile(const std::string& id) {
        for (const NoisePresetEmbeddedFile* file = noise_preset_embedded_files; file->id; file++) {
            if (id == file->id) return file;
        }
        return nullptr;
    }

    TerrainPreset presetOf(const NoisePresetEmbeddedFile& file) {
        TerrainPreset preset;
        preset.id = file.id;
        const graph::Json root = graph::Json::parse(file.data, file.data + file.size);
        const graph::Json& meta = root["metadata"]["preset"];
        preset.name = meta.value("name", preset.id);
        preset.description = meta.value("description", std::string());
        if (meta.contains("tags")) preset.tags = meta["tags"].get<std::vector<std::string>>();
        preset.terrainSize = meta.value("terrain_size", 1000.0f);
        preset.maxHeight = meta.value("max_height", 100.0f);
        preset.hidden = meta.value("hidden", false);
        return preset;
    }

}

std::vector<TerrainPreset> TerrainPresets::list(bool includeHidden) {
    std::vector<TerrainPreset> presets;
    for (const NoisePresetEmbeddedFile* file = noise_preset_embedded_files; file->id; file++) {
        TerrainPreset preset = presetOf(*file);
        if (includeHidden || !preset.hidden) presets.push_back(std::move(preset));
    }
    return presets;
}

bool TerrainPresets::find(const std::string& id, TerrainPreset& out) {
    const NoisePresetEmbeddedFile* file = findFile(id);
    if (!file) return false;
    out = presetOf(*file);
    return true;
}

std::string TerrainPresets::json(const std::string& id) {
    const NoisePresetEmbeddedFile* file = findFile(id);
    return file ? std::string(reinterpret_cast<const char*>(file->data), file->size) : std::string();
}

std::string TerrainPresets::blankJson() {
    graph::Graph graph;
    graph.kind = "noise";
    graph.kindVersion = 1;
    graph::Node constant;
    constant.id = graph.allocateId("n");
    constant.type = "constant";
    constant.params = {{"value", 0.5}};
    graph.nodes.push_back(constant);
    graph::Node output;
    output.id = graph.allocateId("n");
    output.type = "output_height";
    output.position.x = 240.0;
    output.params = {{"unit", "normalized"}};
    graph.nodes.push_back(output);
    graph::Edge edge;
    edge.id = graph.allocateId("e");
    edge.from.node = constant.id;
    edge.from.port = "out";
    edge.to.node = output.id;
    edge.to.port = "in";
    graph.edges.push_back(edge);
    graph.metadata = {{"noise", {{"space", "terrain"}}}};
    return graph::save(graph);
}
