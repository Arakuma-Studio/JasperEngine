// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC20: scatter_by_rules, on T1's engine, without a GPU

#include "TerrainTestSupport.h"

#include "terrain/FoliageOps.h"
#include "terrain/FoliageRules.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

using namespace terraintest;

namespace {

    constexpr float SIZE = 256.0f;
    constexpr float MAX_HEIGHT = 100.0f;

    // Height rising along +X over the map, a slope that is known everywhere
    std::unique_ptr<RawMapStore> makeRampStore(int size = 128, float rise = 0.5f) {
        auto store = std::make_unique<RawMapStore>(SIZE, MAX_HEIGHT);
        store->addMap(HEIGHT, size, size, 1, 16);
        fillHeight(*store, size, size, [&](int x, int) { return rise * (static_cast<float>(x) + 0.5f) / static_cast<float>(size); });
        std::vector<std::pair<MapId, TexelRect>> dirty;
        store->takeDirty(dirty);
        return store;
    }

    FoliageTreeScatter basicScatter(uint32_t seed = 42, float density = 0.01f) {
        FoliageTreeScatter scatter;
        scatter.seed = seed;
        scatter.density = density;
        scatter.targets.push_back(FoliageTreeTarget{0, 3.0f});
        scatter.targets.push_back(FoliageTreeTarget{1, 1.0f});
        return scatter;
    }

    OpParam num(double value) {
        OpParam p;
        p.kind = OpParam::Kind::Number;
        p.number = value;
        return p;
    }

    OpParam str(const std::string& value) {
        OpParam p;
        p.kind = OpParam::Kind::String;
        p.text = value;
        return p;
    }

    OpParam list(std::initializer_list<OpParam> items) {
        OpParam p;
        p.kind = OpParam::Kind::List;
        p.items = items;
        return p;
    }

    OpParam obj(std::initializer_list<std::pair<std::string, OpParam>> fields) {
        OpParam p;
        p.kind = OpParam::Kind::Object;
        p.fields = fields;
        return p;
    }

    Op scatterOp(std::initializer_list<std::pair<std::string, OpParam>> fields, uint32_t seed = 42) {
        Op op;
        op.type = OpType::Registered;
        op.registeredName = "scatter_by_rules";
        op.params = obj(fields);
        op.seed = seed;
        return op;
    }

    const OpParam* field(const OpEntry& entry, std::string_view key) {
        for (const auto& [name, value] : entry.fields) {
            if (name == key) return &value;
        }
        return nullptr;
    }

    double count(const OpEntry& entry, std::string_view key) {
        const OpParam* value = field(entry, key);
        return value ? value->number : -1.0;
    }

    struct OpsFixture {
        std::unique_ptr<RawMapStore> store = makeRampStore();
        std::shared_ptr<TreeStore> trees = std::make_shared<TreeStore>();
        FoliageOpHost host{trees};

        OpsFixture() {
            registerFoliageOps();
            host.prototypes.push_back(FoliageOpHost::Prototype{0, "Pine", true});
            host.prototypes.push_back(FoliageOpHost::Prototype{1, "Birch", true});
        }
        ~OpsFixture() { unregisterFoliageOps(); }

        TerrainOpResult run(const Op& op, OpOptions options = {}) {
            FoliageOpScope scope(host);
            return applyOps(*store, std::span<const Op>(&op, 1), options);
        }
    };

}

TEST(FoliageRules, SlopeHeightLayerRules) {
    std::unique_ptr<RawMapStore> store = makeRampStore();
    FoliageTreeScatter scatter = basicScatter();

    const FoliageTreeScatterResult all = scatterTreesByRules(*store, scatter, {});
    ASSERT_GT(all.trees.size(), 100u);

    // a uniform slope of atan(50 / 256) = 11 degrees: under the range, and in it
    FoliageTreeScatter steep = scatter;
    steep.rules.slope = FilterRange{30.0f, 60.0f};
    const FoliageTreeScatterResult none = scatterTreesByRules(*store, steep, {});
    EXPECT_TRUE(none.trees.empty());
    EXPECT_GT(none.rejected.slope, 0);

    // from 0: the forward difference clamps at the map's far edge, so the last texel reads flat, as
    // the surface query does
    FoliageTreeScatter fits = scatter;
    fits.rules.slope = FilterRange{0.0f, 20.0f};
    EXPECT_EQ(scatterTreesByRules(*store, fits, {}).trees.size(), all.trees.size());

    // height is terrain-local: the ramp reaches 50
    FoliageTreeScatter low = scatter;
    low.rules.height = FilterRange{0.0f, 25.0f};
    const FoliageTreeScatterResult lowResult = scatterTreesByRules(*store, low, {});
    EXPECT_GT(lowResult.rejected.height, 0);
    EXPECT_GT(lowResult.trees.size(), 0u);
    for (const TreeRecord& tree : lowResult.trees) {
        EXPECT_LE(tree.height * MAX_HEIGHT, 25.0f + 1e-3f);
    }

    // a surface layer's weight: a blend map full in its first channel on the -X half
    store->addMap(blend(0), 16, 16, 4, 8);
    std::vector<float> weights(16 * 16 * 4, 0.0f);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 8; x++) weights[(static_cast<size_t>(y) * 16 + x) * 4] = 1.0f;
    }
    store->write(blend(0), TexelRect{0, 0, 16, 16}, weights);
    FoliageTreeScatter painted = scatter;
    painted.rules.layers.push_back(FoliageRuleLayer{0, 0.5f, 1.0f});
    const FoliageTreeScatterResult onLayer = scatterTreesByRules(*store, painted, {});
    EXPECT_GT(onLayer.rejected.layer, 0);
    EXPECT_GT(onLayer.trees.size(), 0u);
    for (const TreeRecord& tree : onLayer.trees) {
        EXPECT_LT(foliageTreeLocalX(tree, SIZE), 1.0f);
    }
}

// T3 R15: a layer past the ninth weighs by a blend map's alpha, inverted (terrain::terrainLayerChannel),
// so a rule on layer 10 reads blend map 1's alpha, not a fourth map
TEST(FoliageRules, LayerPastNineReadsTheAlpha) {
    std::unique_ptr<RawMapStore> store = makeRampStore();
    FoliageTreeScatter scatter = basicScatter();
    const FoliageTreeScatterResult all = scatterTreesByRules(*store, scatter, {});
    ASSERT_GT(all.trees.size(), 100u);

    // blend map 1 opaque (layer 10 at 0) on the +X half, alpha 0 (layer 10 full) on the -X half
    store->addMap(blend(1), 16, 16, 4, 8);
    std::vector<float> weights(16 * 16 * 4, 0.0f);
    for (int y = 0; y < 16; y++) {
        for (int x = 8; x < 16; x++) weights[(static_cast<size_t>(y) * 16 + x) * 4 + 3] = 1.0f;
    }
    store->write(blend(1), TexelRect{0, 0, 16, 16}, weights);
    FoliageTreeScatter painted = scatter;
    painted.rules.layers.push_back(FoliageRuleLayer{10, 0.5f, 1.0f});
    const FoliageTreeScatterResult onLayer = scatterTreesByRules(*store, painted, {});
    EXPECT_GT(onLayer.rejected.layer, 0);
    ASSERT_GT(onLayer.trees.size(), 0u);
    for (const TreeRecord& tree : onLayer.trees) {
        EXPECT_LT(foliageTreeLocalX(tree, SIZE), 1.0f) << "layer 10 is the -X half, where blend map 1's alpha is 0";
    }
}

TEST(FoliageRules, NoiseMask) {
    std::unique_ptr<RawMapStore> store = makeRampStore();
    // a mask the caller evaluated (the editor's op evaluates T2's graph into one)
    std::vector<float> mask(64 * 64);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) mask[static_cast<size_t>(y) * 64 + x] = y < 32 ? 1.0f : 0.0f;
    }
    FoliageTreeScatter scatter = basicScatter();
    scatter.rules.noiseMask = FoliageRuleMask{mask, 64, 64, 0.45f};
    const FoliageTreeScatterResult result = scatterTreesByRules(*store, scatter, {});
    EXPECT_GT(result.rejected.noise, 0);
    EXPECT_GT(result.trees.size(), 0u);
    for (const TreeRecord& tree : result.trees) {
        EXPECT_LE(foliageTreeLocalZ(tree, SIZE), 1.0f);
    }
}

TEST(FoliageRules, HolesAreExcluded) {
    std::unique_ptr<RawMapStore> store = makeRampStore();
    store->addMap(HOLE, 32, 32, 1, 8);
    std::vector<float> holes(32 * 32, 1.0f);
    for (int y = 0; y < 32; y++) {
        for (int x = 16; x < 32; x++) holes[static_cast<size_t>(y) * 32 + x] = 0.0f;
    }
    store->write(HOLE, TexelRect{0, 0, 32, 32}, holes);

    FoliageTreeScatter scatter = basicScatter();
    const FoliageTreeScatterResult excluded = scatterTreesByRules(*store, scatter, {});
    EXPECT_GT(excluded.rejected.hole, 0);
    EXPECT_GT(excluded.trees.size(), 0u);
    for (const TreeRecord& tree : excluded.trees) {
        EXPECT_LE(foliageTreeLocalX(tree, SIZE), 0.0f);
    }

    scatter.rules.excludeHoles = false;
    const FoliageTreeScatterResult allowed = scatterTreesByRules(*store, scatter, {});
    EXPECT_EQ(allowed.rejected.hole, 0);
    EXPECT_GT(allowed.trees.size(), excluded.trees.size());
}

TEST(FoliageRules, SplineZoneIsExcluded) {
    std::unique_ptr<RawMapStore> store = makeRampStore();
    FoliageTreeScatter scatter = basicScatter(7, 0.02f);
    FoliageSplineZone road;
    road.points = {Vector2(-128.0f, 0.0f), Vector2(128.0f, 0.0f)};
    road.margin = 10.0f;
    scatter.rules.excludeSplines.push_back(road);
    const FoliageTreeScatterResult result = scatterTreesByRules(*store, scatter, {});
    EXPECT_GT(result.rejected.spline, 0);
    for (const TreeRecord& tree : result.trees) {
        EXPECT_GT(std::abs(foliageTreeLocalZ(tree, SIZE)), 10.0f);
    }
}

TEST(FoliageRules, MinSpacingHolds) {
    std::unique_ptr<RawMapStore> store = makeRampStore();
    FoliageTreeScatter scatter = basicScatter(5, 0.05f);
    scatter.rules.minSpacing = 6.0f;
    const std::vector<Vector2> existing = {Vector2(0.0f, 0.0f)};
    const FoliageTreeScatterResult result = scatterTreesByRules(*store, scatter, existing);
    EXPECT_GT(result.rejected.spacing, 0);
    ASSERT_GT(result.trees.size(), 50u);
    for (size_t i = 0; i < result.trees.size(); i++) {
        const float xi = foliageTreeLocalX(result.trees[i], SIZE);
        const float zi = foliageTreeLocalZ(result.trees[i], SIZE);
        EXPECT_GE(std::hypot(xi, zi), 6.0f - 1e-3f) << "existing trees count";
        for (size_t j = i + 1; j < result.trees.size(); j++) {
            const float dx = xi - foliageTreeLocalX(result.trees[j], SIZE);
            const float dz = zi - foliageTreeLocalZ(result.trees[j], SIZE);
            ASSERT_GE(std::sqrt(dx * dx + dz * dz), 6.0f - 2e-3f);
        }
    }
}

TEST(FoliageRules, GoldenHashOnEveryTarget) {
    std::unique_ptr<RawMapStore> store = makeRampStore();
    FoliageTreeScatter scatter = basicScatter();
    scatter.jitter.minScale = 0.8f;
    scatter.jitter.maxScale = 1.3f;
    scatter.jitter.minHeightRatio = 0.9f;
    scatter.jitter.maxHeightRatio = 1.2f;
    scatter.jitter.alignToNormal = 0.5f;
    scatter.rules.minSpacing = 3.0f;
    const FoliageTreeScatterResult result = scatterTreesByRules(*store, scatter, {});
    EXPECT_GT(result.trees.size(), 0u);
    EXPECT_EQ(result.hash, 0x5c6872872df93554ull) << "actual 0x" << std::hex << result.hash;
    EXPECT_EQ(scatterTreesByRules(*store, scatter, {}).hash, result.hash);
}

TEST(FoliageRules, DryRunMatchesARealRun) {
    OpsFixture fixture;
    Op op = scatterOp({
        {"target", obj({{"trees", list({obj({{"prototype", str("Pine")}, {"weight", num(3)}}), obj({{"prototype", str("Birch")}, {"weight", num(1)}})})}})},
        {"density", num(0.01)},
        {"rules", obj({{"min_spacing", num(2)}})}
    });

    OpOptions dry;
    dry.dryRun = true;
    const TerrainOpResult preview = fixture.run(op, dry);
    ASSERT_TRUE(preview.ok) << preview.error;
    EXPECT_TRUE(fixture.trees->empty()) << "a dry run changes nothing";

    const TerrainOpResult real = fixture.run(op);
    ASSERT_TRUE(real.ok) << real.error;
    ASSERT_EQ(real.ops.size(), 1u);
    EXPECT_GT(count(real.ops[0], "instances_added"), 0.0);
    EXPECT_EQ(count(preview.ops[0], "instances_added"), count(real.ops[0], "instances_added"));
    EXPECT_EQ(field(preview.ops[0], "hash")->text, field(real.ops[0], "hash")->text);
    const OpParam* previewRejected = field(preview.ops[0], "rejected");
    const OpParam* realRejected = field(real.ops[0], "rejected");
    for (const char* rule : {"slope", "height", "layer", "noise", "hole", "spline", "spacing"}) {
        EXPECT_EQ(previewRejected->find(rule)->number, realRejected->find(rule)->number) << rule;
    }

    // the host holds the result until the caller commits; a commit lands exactly the added count
    EXPECT_EQ(fixture.host.current().size(), static_cast<size_t>(count(real.ops[0], "instances_added")));
}

TEST(FoliageRules, InvalidInputChangesNothing) {
    OpsFixture fixture;
    const uint64_t heightBefore = mapHash(*fixture.store, HEIGHT);

    const Op unknownKey = scatterOp({{"target", obj({{"trees", list({obj({{"prototype", str("Pine")}})})}})}, {"densty", num(0.1)}});
    TerrainOpResult result = fixture.run(unknownKey);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("densty"), std::string::npos) << result.error;

    const Op unknownPrototype = scatterOp({{"target", obj({{"trees", list({obj({{"prototype", str("Oak")}})})}})}, {"density", num(0.1)}});
    result = fixture.run(unknownPrototype);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("Pine"), std::string::npos) << "an error names the fix: " << result.error;

    const Op noLayer = scatterOp({{"target", obj({{"detail_layer", num(3)}})}, {"density", num(0.1)}});
    result = fixture.run(noLayer);
    EXPECT_FALSE(result.ok);

    const Op noiseWithoutGraphs = scatterOp({
        {"target", obj({{"trees", list({obj({{"prototype", str("Pine")}})})}})},
        {"density", num(0.1)},
        {"rules", obj({{"noise_mask", obj({{"graph", str("noise/forest.noisegraph")}})}})}
    });
    result = fixture.run(noiseWithoutGraphs);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("noise_mask"), std::string::npos) << result.error;

    EXPECT_EQ(mapHash(*fixture.store, HEIGHT), heightBefore);
    EXPECT_FALSE(fixture.host.touched());
    EXPECT_TRUE(fixture.trees->empty());
}

TEST(FoliageRules, DetailLayerTargetWritesAbsoluteDensity) {
    OpsFixture fixture;
    fixture.store->addMap(density(0), 64, 64, 1, 8);
    fixture.host.layers.push_back(FoliageOpHost::DetailLayer{"grass", 2.0f, true});

    // half the layer's density, as an absolute value, on the half under height 25
    Op op = scatterOp({
        {"target", obj({{"detail_layer", num(0)}})},
        {"density", num(1.0)},
        {"rules", obj({{"height", list({num(0), num(25)})}})}
    });
    const TerrainOpResult result = fixture.run(op);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_FALSE(result.changed.empty());

    std::vector<float> values(64 * 64);
    ASSERT_TRUE(fixture.store->read(density(0), TexelRect{0, 0, 64, 64}, values));
    EXPECT_NEAR(values[0], 0.5f, 1.0f / 255.0f);
    EXPECT_EQ(values[63], 0.0f);

    // replace is idempotent; remove takes it away
    const uint64_t once = mapHash(*fixture.store, density(0));
    ASSERT_TRUE(fixture.run(op).ok);
    EXPECT_EQ(mapHash(*fixture.store, density(0)), once);

    Op remove = op;
    remove.params.fields.emplace_back("mode", str("remove"));
    ASSERT_TRUE(fixture.run(remove).ok);
    ASSERT_TRUE(fixture.store->read(density(0), TexelRect{0, 0, 64, 64}, values));
    EXPECT_EQ(values[0], 0.0f);
}

TEST(FoliageRules, ModesOnTrees) {
    OpsFixture fixture;
    Op add = scatterOp({
        {"target", obj({{"trees", list({obj({{"prototype", str("Pine")}})})}})},
        {"density", num(0.005)},
        {"mode", str("add")}
    });
    ASSERT_TRUE(fixture.run(add).ok);
    const size_t first = fixture.host.current().size();
    ASSERT_GT(first, 0u);

    // add keeps what is there
    Op again = add;
    again.seed = 43;
    ASSERT_TRUE(fixture.run(again).ok);
    const size_t second = fixture.host.current().size();
    EXPECT_GT(second, first);

    // replace in a rectangle leaves the rest
    Op replace = add;
    replace.params.fields.clear();
    replace.params = obj({
        {"target", obj({{"trees", list({obj({{"prototype", str("Pine")}})})}})},
        {"density", num(0.0)},
        {"mode", str("replace")}
    });
    replace.shape.type = ShapeType::Rect;
    replace.shape.min = Vector2(-128.0f, -128.0f);
    replace.shape.max = Vector2(0.0f, 128.0f);
    const TerrainOpResult replaced = fixture.run(replace);
    ASSERT_TRUE(replaced.ok) << replaced.error;
    EXPECT_GT(count(replaced.ops[0], "instances_removed"), 0.0);
    for (const TreeRecord& tree : fixture.host.current().records()) {
        EXPECT_GE(foliageTreeLocalX(tree, SIZE), 0.0f);
    }

    // remove by rule
    Op remove = scatterOp({
        {"target", obj({{"trees", list({obj({{"prototype", str("Pine")}})})}})},
        {"mode", str("remove")},
        {"rules", obj({{"height", list({num(0), num(40)})}})}
    });
    ASSERT_TRUE(fixture.run(remove).ok);
    for (const TreeRecord& tree : fixture.host.current().records()) {
        EXPECT_GT(tree.height * MAX_HEIGHT, 40.0f - 1e-3f);
    }
}

TEST(FoliageRules, PlaceRemoveAndClearOps) {
    OpsFixture fixture;

    Op place;
    place.type = OpType::Registered;
    place.registeredName = "place_trees";
    place.params = obj({
        {"prototype", str("Birch")},
        {"points", list({list({num(10), num(20)}), list({num(-5), num(-6), num(90), num(2)})})}
    });
    const TerrainOpResult placed = fixture.run(place);
    ASSERT_TRUE(placed.ok) << placed.error;
    const OpParam* ids = field(placed.ops[0], "ids");
    ASSERT_NE(ids, nullptr);
    ASSERT_EQ(ids->items.size(), 2u);

    const TreeRecord* first = fixture.host.current().find(static_cast<uint32_t>(ids->items[0].number));
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->prototypeId, 1);
    EXPECT_NEAR(foliageTreeLocalX(*first, SIZE), 10.0f, 1e-3f);

    // outside the terrain: named, and nothing lands
    Op outside = place;
    outside.params = obj({{"prototype", str("Birch")}, {"points", list({list({num(500), num(0)})})}});
    const size_t countBefore = fixture.host.current().size();
    EXPECT_FALSE(fixture.run(outside).ok);
    EXPECT_EQ(fixture.host.current().size(), countBefore);

    Op removeOne;
    removeOne.type = OpType::Registered;
    removeOne.registeredName = "remove_trees";
    removeOne.params = obj({{"ids", list({num(ids->items[0].number)})}});
    const TerrainOpResult removed = fixture.run(removeOne);
    ASSERT_TRUE(removed.ok) << removed.error;
    EXPECT_EQ(count(removed.ops[0], "instances_removed"), 1.0);
    EXPECT_EQ(fixture.host.current().size(), 1u);

    Op unknownId = removeOne;
    unknownId.params = obj({{"ids", list({num(9999)})}});
    EXPECT_FALSE(fixture.run(unknownId).ok);

    Op clear;
    clear.type = OpType::Registered;
    clear.registeredName = "clear_foliage";
    clear.params = obj({{"target", str("all")}});
    const TerrainOpResult cleared = fixture.run(clear);
    ASSERT_TRUE(cleared.ok) << cleared.error;
    EXPECT_EQ(count(cleared.ops[0], "instances_removed"), 1.0);
    EXPECT_TRUE(fixture.host.current().empty());

    // ids are not given again, even after everything went
    const TerrainOpResult again = fixture.run(place);
    ASSERT_TRUE(again.ok);
    EXPECT_GT(field(again.ops[0], "ids")->items[0].number, ids->items[1].number);
}

TEST(FoliageRules, ConvertTakesTheHostsInstances) {
    OpsFixture fixture;
    FoliageOpHost::PlacedHost placed;
    placed.name = "Pine host";
    placed.prototypeId = 0;
    FoliageOpHost::PlacedInstance instance;
    instance.position[0] = 8.0f;
    instance.position[1] = 40.0f;
    instance.position[2] = 8.0f;
    instance.scale[0] = instance.scale[2] = 2.0f;
    instance.scale[1] = 3.0f;
    placed.instances.push_back(instance);
    fixture.host.placedHosts["12"] = placed;

    Op op;
    op.type = OpType::Registered;
    op.registeredName = "convert_placed_to_trees";
    op.params = obj({{"host", num(12)}});
    const TerrainOpResult result = fixture.run(op);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(count(result.ops[0], "instances_added"), 1.0);
    ASSERT_EQ(fixture.host.convertedHosts.size(), 1u);
    EXPECT_EQ(fixture.host.convertedHosts[0], "12");
    const TreeRecord& tree = fixture.host.current().records()[0];
    EXPECT_EQ(tree.scale, 2.0f);
    EXPECT_EQ(tree.heightRatio, 192);
    // it stays where it was, whatever the surface under it
    FoliageRuleSurface surface(*fixture.store);
    EXPECT_NEAR(tree.height * MAX_HEIGHT + tree.yOffset, 40.0f, 1e-3f);
    EXPECT_NEAR(surface.height(8.0f, 8.0f), tree.height * MAX_HEIGHT, 1e-3f);

    op.params = obj({{"host", num(13)}});
    EXPECT_FALSE(fixture.run(op).ok);
}

TEST(FoliageRules, FailedListLeavesTheTreesAlone) {
    OpsFixture fixture;
    fixture.trees->add(TreeRecord{});
    const uint64_t before = fixture.trees->contentHash();

    Op place;
    place.type = OpType::Registered;
    place.registeredName = "place_trees";
    place.params = obj({{"prototype", str("Pine")}, {"points", list({list({num(1), num(2)})})}});
    Op bad = scatterOp({{"target", obj({{"trees", list({obj({{"prototype", str("Nothing")}})})}})}});
    const Op listOps[] = {place, bad};
    FoliageOpScope scope(fixture.host);
    const TerrainOpResult result = applyOps(*fixture.store, listOps);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(fixture.trees->contentHash(), before) << "the base is only written on commit";
}
