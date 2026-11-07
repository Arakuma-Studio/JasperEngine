// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T2 AC20, AC21, AC40, AC50: the generation path over a store that records every write, with no project.

#include "asset/AssetTypes.h"
#include "asset/AssetType.h"
#include "noise/NoiseGraphRuntime.h"
#include "noise/TerrainGeneration.h"
#include "noise/TerrainPresets.h"
#include "terrain/RawMapStore.h"
#include "terrain/TerrainOps.h"

#include <gtest/gtest.h>

#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using terrain::MapId;
using terrain::MapKind;
using terrain::TexelRect;

namespace {

    // A store that says what reached it
    class RecordingStore : public terrain::RawMapStore {
    public:
        RecordingStore(): terrain::RawMapStore(1000.0f, 400.0f) {}

        struct Write { MapId map; TexelRect rect; };
        std::vector<Write> writes;

        terrain::WriteResult write(MapId map, TexelRect rect, std::span<const float> in) override {
            writes.push_back({map, rect});
            return terrain::RawMapStore::write(map, rect, in);
        }
        terrain::WriteResult writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in) override {
            writes.push_back({map, rect});
            return terrain::RawMapStore::writeBytes(map, rect, in);
        }
    };

    void addHeight(terrain::RawMapStore& store, int size, float value) {
        const MapId height{MapKind::Height, 0};
        store.addMap(height, size, size, 1, 16);
        const std::vector<float> values(static_cast<size_t>(size) * size, value);
        store.write(height, TexelRect{0, 0, size, size}, values);
        std::vector<std::pair<MapId, TexelRect>> dirty;
        store.takeDirty(dirty);
    }

    graph::Graph presetGraph(const std::string& id) {
        noise::registerNoiseKind();
        graph::LoadReport report;
        return graph::load(TerrainPresets::json(id), report);
    }

    // height <- plains; layer 1 <- constant weight, layer 2 <- another
    graph::Graph layeredGraph(double weight1, double weight2, bool optional2 = false, int layer2 = 2) {
        graph::Graph graph = presetGraph("plains");
        const auto addConstant = [&graph](double value) {
            graph::Node node;
            node.id = graph.allocateId("n");
            node.type = "constant";
            node.params = {{"value", value}};
            graph.nodes.push_back(node);
            return node.id;
        };
        const auto addLayer = [&graph](const std::string& from, int layer, bool optional) {
            graph::Node node;
            node.id = graph.allocateId("n");
            node.type = "output_layer";
            node.params = {{"layer", layer}, {"optional", optional}};
            graph.nodes.push_back(node);
            graph::Edge edge;
            edge.id = graph.allocateId("e");
            edge.from.node = from;
            edge.from.port = "out";
            edge.to.node = node.id;
            edge.to.port = "weight";
            graph.edges.push_back(edge);
            return node.id;
        };
        addLayer(addConstant(weight1), 1, false);
        addLayer(addConstant(weight2), layer2, optional2);
        return graph;
    }

    GenerationRequest requestFor(graph::Graph graph) {
        GenerationRequest request;
        request.graph = std::move(graph);
        request.displayName = "Test";
        request.seed = 7;
        request.terrainSize = 1000.0f;
        return request;
    }

    bool runAll(terrain::TerrainMapStore& store, const GenerationRequest& request, GenerationResult& result) {
        auto prepared = TerrainGeneration::prepare(store, request, result);
        if (!prepared) return false;
        if (!TerrainGeneration::evaluate(*prepared, nullptr, false, result)) return false;
        return TerrainGeneration::apply(*prepared, store, result);
    }

}

TEST(AssetTypes, NoiseGraphIsSidecar) {
    const AssetTypeInfo* info = AssetTypes::find("assets/a/Hills.noisegraph", "assets");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->type, AssetType::NoiseGraph);
    EXPECT_EQ(info->identity, AssetIdentity::Sidecar);
    EXPECT_EQ(info->base, AssetPathBase::Assets);
}

TEST(AssetTypes, NoiseGraphValueIs32) {
    EXPECT_EQ(static_cast<int>(AssetType::NoiseGraph), 32) << "stored in assetmap.bin: it never moves";
    EXPECT_EQ(assetTypeFromName("noise_graph"), AssetType::NoiseGraph);
    EXPECT_STREQ(assetTypeName(AssetType::NoiseGraph), "noise_graph");
}

TEST(TerrainGeneration, WritesOnlyThroughOps) {
    RecordingStore store;
    store.setFrame(1000.0f, 400.0f);
    addHeight(store, 33, 0.25f);
    store.writes.clear();
    GenerationRequest request = requestFor(presetGraph("rolling_hills"));
    GenerationResult result;
    ASSERT_TRUE(runAll(store, request, result)) << result.error;
    ASSERT_FALSE(store.writes.empty());
    for (const RecordingStore::Write& write : store.writes) {
        EXPECT_EQ(write.map.kind, MapKind::Height);
        EXPECT_TRUE((TexelRect{0, 0, 33, 33}).contains(write.rect));
    }
    ASSERT_EQ(result.changed.size(), 1u);
    EXPECT_EQ(result.changed[0].map.kind, MapKind::Height);
    EXPECT_EQ(result.undoName, "Generate Terrain (Test, seed 7)");
    // A dry run writes nothing, with the same hash and a truthful report
    store.writes.clear();
    GenerationRequest dry = request;
    dry.dryRun = true;
    GenerationResult dryResult;
    ASSERT_TRUE(runAll(store, dry, dryResult)) << dryResult.error;
    EXPECT_TRUE(store.writes.empty());
    EXPECT_EQ(dryResult.outputs[0].hash, result.outputs[0].hash);
}

// R30, through T1's single encoder: a sum above 1 is normalized, layer 0 takes the rest
TEST(TerrainGeneration, WeightsNormalize) {
    RecordingStore store;
    store.setFrame(1000.0f, 400.0f);
    addHeight(store, 17, 0.25f);
    GenerationRequest over = requestFor(layeredGraph(0.75, 0.75));
    over.layerCount = 3;
    GenerationResult overResult;
    ASSERT_TRUE(runAll(store, over, overResult)) << overResult.error;
    std::vector<float> stored(static_cast<size_t>(17) * 17 * 4);
    ASSERT_TRUE(store.read(MapId{MapKind::Blend, 0}, TexelRect{0, 0, 17, 17}, stored));
    EXPECT_NEAR(stored[0], 0.0f, 1.0f / 255.0f) << "layer 0 has nothing left";
    EXPECT_NEAR(stored[1], 0.5f, 2.0f / 255.0f);
    EXPECT_NEAR(stored[2], 0.5f, 2.0f / 255.0f);

    GenerationRequest under = requestFor(layeredGraph(0.25, 0.25));
    under.layerCount = 3;
    GenerationResult underResult;
    ASSERT_TRUE(runAll(store, under, underResult)) << underResult.error;
    ASSERT_TRUE(store.read(MapId{MapKind::Blend, 0}, TexelRect{0, 0, 17, 17}, stored));
    EXPECT_NEAR(stored[0], 0.5f, 2.0f / 255.0f) << "layer 0 takes the rest";
    EXPECT_NEAR(stored[1], 0.25f, 2.0f / 255.0f);
}

TEST(TerrainGeneration, OptionalLayerWarnsAndSkips) {
    RecordingStore store;
    store.setFrame(1000.0f, 400.0f);
    addHeight(store, 17, 0.25f);
    GenerationRequest request = requestFor(layeredGraph(0.5, 0.5, true, 5));
    request.layerCount = 2;
    GenerationResult result;
    ASSERT_TRUE(runAll(store, request, result)) << result.error;
    bool warned = false;
    for (const GenerationIssue& warning : result.warnings) warned = warned || warning.code == "layer-missing";
    EXPECT_TRUE(warned);
    EXPECT_TRUE(result.issues.empty());
    // Heights still apply
    bool heightChanged = false;
    for (const terrain::MapChangeReport& report : result.changed) heightChanged = heightChanged || report.map.kind == MapKind::Height;
    EXPECT_TRUE(heightChanged);
}

TEST(TerrainGeneration, RequiredLayerFailsBeforeEvaluating) {
    RecordingStore store;
    store.setFrame(1000.0f, 400.0f);
    addHeight(store, 17, 0.25f);
    store.writes.clear();
    GenerationRequest request = requestFor(layeredGraph(0.5, 0.5, false, 5));
    request.layerCount = 2;
    GenerationResult result;
    EXPECT_EQ(TerrainGeneration::prepare(store, request, result), nullptr);
    EXPECT_EQ(result.errorCode, "layer-missing");
    EXPECT_FALSE(result.issues.at(0).nodeId.empty()) << "the issue names its node";
    EXPECT_EQ(result.evaluateMs, 0.0) << "no evaluation ran";
    EXPECT_TRUE(store.writes.empty());
}

// R52: live grid buffers * texels * 4 + 64 MiB
TEST(TerrainGeneration, PeakBytesWithinBudget) {
    RecordingStore store;
    store.setFrame(2000.0f, 800.0f);
    addHeight(store, 1024, 0.0f);
    GenerationRequest request = requestFor(presetGraph("mountains"));
    request.terrainSize = 2000.0f;
    request.layerCount = 2;
    GenerationResult result;
    auto prepared = TerrainGeneration::prepare(store, request, result);
    ASSERT_NE(prepared, nullptr) << result.error;
    ASSERT_TRUE(TerrainGeneration::evaluate(*prepared, nullptr, true, result));
    const size_t texels = 1024u * 1024u;
    const size_t grids = 4;     // two outputs and the two the slope mask reads and writes
    EXPECT_GT(result.peakBytes, 0u);
    EXPECT_LE(result.peakBytes, grids * texels * 4u + 64u * 1024u * 1024u);
}
