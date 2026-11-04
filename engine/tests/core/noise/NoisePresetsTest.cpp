// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T2 AC26, R35, N14: the built-in presets load, validate and look like their names. Thresholds in the
// shapes of N14, fixed on first landing from the measured values in docs/perf/noise.md.

#include "NoiseTestSupport.h"
#include "noise_presets.gen.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::noise;
using namespace noisetest;

namespace {

    struct LoadedPreset {
        std::string id;
        graph::Graph graph;
    };

    std::vector<LoadedPreset> loadPresets() {
        registerNoiseKind();
        std::vector<LoadedPreset> out;
        for (const NoisePresetEmbeddedFile* file = noise_preset_embedded_files; file->id; file++) {
            graph::LoadReport report;
            LoadedPreset preset;
            preset.id = file->id;
            preset.graph = graph::load(std::string(reinterpret_cast<const char*>(file->data), file->size), report);
            EXPECT_TRUE(report.ok) << file->id << ": " << report.error;
            out.push_back(std::move(preset));
        }
        return out;
    }

    constexpr int kSize = 257;

    struct Map {
        std::vector<float> height;
        std::vector<float> flow;
        float maxHeight = 100.0f;
        float metresPerTexel = 1000.0f / static_cast<float>(kSize);
    };

    Map generate(const graph::Graph& graph, uint32_t seed) {
        Map map;
        map.maxHeight = static_cast<float>(graph.metadata["preset"].value("max_height", 100.0));
        CompileOptions options;
        options.maxHeight = map.maxHeight;
        const CompileResult result = compile(graph, options);
        EXPECT_NE(result.graph, nullptr);
        if (!result.graph) return map;
        GridDomain domain;
        domain.width = domain.depth = kSize;
        domain.unitsPerTexel = 1.0 / static_cast<double>(kSize);
        EvalContext context;
        context.seed = seed;
        context.maxHeight = map.maxHeight;
        context.metresPerTexel = map.metresPerTexel;
        map.height.resize(static_cast<size_t>(kSize) * kSize);
        EXPECT_TRUE(result.graph->evaluateGrid(domain, context, result.graph->outputIndex("height"), map.height.data()));
        return map;
    }

    float percentile(std::vector<float> values, float p) {
        std::sort(values.begin(), values.end());
        return values[static_cast<size_t>(p * static_cast<float>(values.size() - 1))];
    }

    // The slope of every texel in degrees
    std::vector<float> slopes(const Map& map) {
        std::vector<float> out;
        out.reserve(map.height.size());
        for (int z = 1; z + 1 < kSize; z++) {
            for (int x = 1; x + 1 < kSize; x++) {
                const size_t i = static_cast<size_t>(z) * kSize + x;
                const float gx = (map.height[i + 1] - map.height[i - 1]) * map.maxHeight * 0.5f / map.metresPerTexel;
                const float gz = (map.height[i + kSize] - map.height[i - kSize]) * map.maxHeight * 0.5f / map.metresPerTexel;
                out.push_back(std::atan(std::sqrt(gx * gx + gz * gz)) * 57.29578f);
            }
        }
        return out;
    }

    std::vector<int> histogram(const std::vector<float>& values, int bins) {
        std::vector<int> counts(static_cast<size_t>(bins), 0);
        for (float value : values) {
            const int bin = std::clamp(static_cast<int>(value * static_cast<float>(bins)), 0, bins - 1);
            counts[static_cast<size_t>(bin)]++;
        }
        return counts;
    }

    // Local maxima of a histogram standing clear of their valleys
    int histogramPeaks(const std::vector<int>& counts) {
        const int floor = static_cast<int>(*std::max_element(counts.begin(), counts.end())) / 20;
        int peaks = 0;
        for (size_t i = 0; i < counts.size(); i++) {
            const int left = i > 0 ? counts[i - 1] : 0;
            const int right = i + 1 < counts.size() ? counts[i + 1] : 0;
            if (counts[i] > floor && counts[i] >= left && counts[i] > right) peaks++;
        }
        return peaks;
    }

}

TEST(NoisePresets, AllLoadAndValidate) {
    const std::vector<LoadedPreset> presets = loadPresets();
    EXPECT_EQ(presets.size(), 10u);
    for (const LoadedPreset& preset : presets) {
        const CompileResult result = compile(preset.graph);
        EXPECT_NE(result.graph, nullptr) << preset.id;
        for (const graph::Issue& issue : result.issues) {
            EXPECT_NE(issue.severity, graph::Severity::Error) << preset.id << ": " << issue.message;
        }
        const graph::Json& meta = preset.graph.metadata["preset"];
        EXPECT_EQ(meta.value("id", std::string()), preset.id);
        EXPECT_FALSE(meta.value("name", std::string()).empty()) << preset.id;
        EXPECT_FALSE(meta.value("description", std::string()).empty()) << preset.id;
        EXPECT_TRUE(meta.contains("tags")) << preset.id;
        EXPECT_GT(meta.value("terrain_size", 0.0), 0.0) << preset.id;
        EXPECT_GT(meta.value("max_height", 0.0), 0.0) << preset.id;
        EXPECT_EQ(meta.value("hidden", false), preset.id == "erode_existing" || preset.id == "paint_by_slope") << preset.id;
    }
}

TEST(NoisePresets, EmbeddedMatchesTheSourceFolder) {
    const std::filesystem::path folder = std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor" / "resources" / "noisegraphs";
    size_t onDisk = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() == ".noisegraph") onDisk++;
    }
    size_t embedded = 0;
    for (const NoisePresetEmbeddedFile* file = noise_preset_embedded_files; file->id; file++) {
        embedded++;
        std::ifstream in(folder / (std::string(file->id) + ".noisegraph"), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(text, std::string(reinterpret_cast<const char*>(file->data), file->size)) << file->id << ": reconfigure";
    }
    EXPECT_EQ(onDisk, embedded);
}

TEST(NoisePresets, Character) {
    std::map<std::string, graph::Graph> presets;
    for (LoadedPreset& preset : loadPresets()) presets[preset.id] = preset.graph;
    for (uint32_t seed : {1u, 2u, 3u}) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        {
            const Map map = generate(presets["plains"], seed);
            EXPECT_LT(percentile(map.height, 0.95f) - percentile(map.height, 0.05f), 0.1f);
            EXPECT_LT(percentile(slopes(map), 0.95f), 8.0f);
        }
        {
            const Map map = generate(presets["rolling_hills"], seed);
            const float spread = percentile(map.height, 0.95f) - percentile(map.height, 0.05f);
            EXPECT_GE(spread, 0.15f);
            EXPECT_LE(spread, 0.5f);
            EXPECT_LT(percentile(slopes(map), 0.95f), 25.0f);
        }
        {
            const Map map = generate(presets["mountains"], seed);
            EXPECT_GT(percentile(map.height, 0.95f) - percentile(map.height, 0.05f), 0.4f);
            EXPECT_GT(percentile(slopes(map), 0.95f), 30.0f);
        }
        {
            const graph::Graph& graph = presets["alpine"];
            const Map map = generate(graph, seed);
            EXPECT_GT(percentile(map.height, 0.95f) - percentile(map.height, 0.05f), 0.4f);
            EXPECT_GT(percentile(slopes(map), 0.95f), 30.0f);
            // flow: the hydraulic node's own output, through a copy of the graph that reads it
            graph::Graph withFlow = graph;
            std::string hydraulic;
            for (const graph::Node& node : withFlow.nodes) {
                if (node.type == "hydraulic_erosion") hydraulic = node.id;
            }
            ASSERT_FALSE(hydraulic.empty());
            graph::Node output;
            output.id = withFlow.allocateId("n");
            output.type = "output_value";
            output.params = {{"name", "flow"}};
            withFlow.nodes.push_back(output);
            graph::Edge edge;
            edge.id = withFlow.allocateId("e");
            edge.from.node = hydraulic;
            edge.from.port = "flow";
            edge.to.node = output.id;
            edge.to.port = "in";
            withFlow.edges.push_back(edge);
            const CompileResult result = compile(withFlow);
            ASSERT_NE(result.graph, nullptr);
            GridDomain domain;
            domain.width = domain.depth = kSize;
            domain.unitsPerTexel = 1.0 / static_cast<double>(kSize);
            EvalContext context;
            context.seed = seed;
            context.maxHeight = map.maxHeight;
            context.metresPerTexel = map.metresPerTexel;
            std::vector<float> flow(map.height.size());
            ASSERT_TRUE(result.graph->evaluateGrid(domain, context, result.graph->outputIndex("flow"), flow.data()));
            EXPECT_GT(percentile(flow, 0.99f), percentile(flow, 0.5f) * 10.0f);
        }
        {
            const Map map = generate(presets["canyons"], seed);
            EXPECT_GE(histogramPeaks(histogram(map.height, 24)), 2) << "bimodal";
            EXPECT_GT(percentile(slopes(map), 0.99f), 45.0f);
        }
        {
            const Map map = generate(presets["islands"], seed);
            for (int i = 0; i < kSize; i++) {
                ASSERT_LT(map.height[static_cast<size_t>(i)], 0.02f);
                ASSERT_LT(map.height[static_cast<size_t>(kSize - 1) * kSize + i], 0.02f);
                ASSERT_LT(map.height[static_cast<size_t>(i) * kSize], 0.02f);
                ASSERT_LT(map.height[static_cast<size_t>(i) * kSize + kSize - 1], 0.02f);
            }
            double centre = 0.0;
            int count = 0;
            for (int z = kSize / 3; z < 2 * kSize / 3; z++) {
                for (int x = kSize / 3; x < 2 * kSize / 3; x++) {
                    centre += map.height[static_cast<size_t>(z) * kSize + x];
                    count++;
                }
            }
            EXPECT_GT(centre / count, 0.2);
        }
        {
            const Map map = generate(presets["dunes"], seed);
            // The ridges run across the wind, so heights change far faster along z than along x
            double alongX = 0.0, alongZ = 0.0;
            for (int z = 0; z + 1 < kSize; z++) {
                for (int x = 0; x + 1 < kSize; x++) {
                    const size_t i = static_cast<size_t>(z) * kSize + x;
                    const double dx = map.height[i + 1] - map.height[i];
                    const double dz = map.height[i + kSize] - map.height[i];
                    alongX += dx * dx;
                    alongZ += dz * dz;
                }
            }
            EXPECT_GT(alongZ / alongX, 3.0);
        }
        {
            const Map map = generate(presets["plateaus"], seed);
            EXPECT_GE(histogramPeaks(histogram(map.height, 64)), 4);
        }
    }
}
