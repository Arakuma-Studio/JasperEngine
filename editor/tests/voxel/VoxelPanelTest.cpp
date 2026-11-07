// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R59, R61, R62, R64 (AC45): the create dialog's numbers and its one step, the error strip per code
// with its log line once, a slot in use refusing to go, and the stats the panel lists.

#include "VoxelTestScene.h"

#include "Out.h"
#include "voxel/VoxelCreate.h"
#include "voxel/VoxelMaterials.h"
#include "voxel/VoxelProblems.h"
#include "voxel/VoxelVolumeOps.h"

#include <algorithm>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::voxeltest;

namespace {

    class VoxelCreate : public VoxelSceneTest {};
    class VoxelMaterials : public VoxelSceneTest {};

    class VoxelPanel : public VoxelSceneTest {
    protected:
        void TearDown() override {
            resetVoxelProblemLog();
            setVoxelLimitsOverride(voxelPlatformLimits());
            VoxelSceneTest::TearDown();
        }

        std::vector<VoxelProblemCode> codes(bool moduleOn = true) {
            std::vector<VoxelProblemCode> out;
            for (const VoxelProblem& problem : voxelProblems(*project, *sceneProject, owner, moduleOn)) {
                EXPECT_FALSE(problem.what.empty()) << voxelProblemName(problem.code);
                EXPECT_FALSE(problem.action.empty()) << voxelProblemName(problem.code) << " says what to do";
                out.push_back(problem.code);
            }
            return out;
        }

        bool has(VoxelProblemCode code, bool moduleOn = true) {
            const std::vector<VoxelProblemCode> list = codes(moduleOn);
            return std::find(list.begin(), list.end(), code) != list.end();
        }

        // a volume with its own file and generator
        void addVolumeWith(const std::string& path, const voxel::VolumeDesc& desc, const voxel::GeneratorDesc& generator) {
            asset = VoxelAssets::create(path, desc, generator, 1);
            AssetRef ref;
            std::string error;
            ASSERT_TRUE(saveNewVoxelAsset(*project, *asset, ref, &error)) << error;
            Scene* scene = sceneProject->scene;
            owner = scene->createEntity();
            scene->addComponent<Transform>(owner, {});
            scene->setEntityName(owner, "Odd");
            VoxelVolumeComponent component;
            component.volumeData = ref;
            component.collision = false;
            component.navigationUpdate = VoxelNavigationUpdate::Off;
            VoxelMaterialSlot slot;
            slot.name = "Default";
            component.materialSlots.push_back(slot);
            scene->addComponent<VoxelVolumeComponent>(owner, component);
            sceneProject->entities.push_back(owner);
            system->update(0.0);
        }
    };

}

TEST_F(VoxelCreate, DialogEstimateAndUndo) {
    EXPECT_EQ(voxelCreatePresetSize(VoxelCreatePreset::Small), (voxel::Int3{64, 64, 64}));
    EXPECT_EQ(voxelCreatePresetSize(VoxelCreatePreset::Medium), (voxel::Int3{128, 64, 128}));
    EXPECT_EQ(voxelCreatePresetSize(VoxelCreatePreset::Large), (voxel::Int3{256, 128, 256}));
    VoxelCreateOptions custom;
    custom.preset = VoxelCreatePreset::Custom;
    custom.dimensions = voxel::Int3{17, 32, 1};
    EXPECT_EQ(voxelCreateDimensions(custom), (voxel::Int3{32, 32, 16})) << "multiples of 16, rounded up";

    // an empty volume is its tables, to the byte
    const VoxelProjectSettings settings;
    VoxelCreateOptions options;
    options.base = VoxelCreateBase::Empty;
    {
        voxel::VolumeDesc desc;
        desc.dimensions = voxelCreateDimensions(options);
        desc.levels = voxel::levelsFor(desc.dimensions, voxel::MAX_LEVELS);
        voxel::Store store(desc);
        EXPECT_EQ(estimateVoxelCreate(options, settings).bytes, store.memory().total());
    }

    // it follows the dialog's fields, live
    const VoxelCreateEstimate small = estimateVoxelCreate(options, settings);
    EXPECT_TRUE(small.warning.empty());
    options.base = VoxelCreateBase::SolidGround;
    const VoxelCreateEstimate ground = estimateVoxelCreate(options, settings);
    EXPECT_GT(ground.bytes, small.bytes);
    options.base = VoxelCreateBase::Caves;
    const VoxelCreateEstimate caves = estimateVoxelCreate(options, settings);
    EXPECT_GT(caves.bytes, ground.bytes);
    options.preset = VoxelCreatePreset::Large;
    const VoxelCreateEstimate largePreset = estimateVoxelCreate(options, settings);
    EXPECT_GT(largePreset.bytes, caves.bytes);
    // 512 x 128 x 512 of caves: over the web's 32 MB, under the desktop's 256 MB
    options.preset = VoxelCreatePreset::Custom;
    options.dimensions = voxel::Int3{512, 128, 512};
    const VoxelCreateEstimate large = estimateVoxelCreate(options, settings);
    // over the budget of a target says which, and a project's lower budget is the one that counts
    ASSERT_FALSE(large.warning.empty());
    EXPECT_NE(std::find(large.overBudgetTargets.begin(), large.overBudgetTargets.end(), "web"), large.overBudgetTargets.end());
    EXPECT_NE(large.warning.find("web"), std::string::npos);
    EXPECT_EQ(std::find(large.overBudgetTargets.begin(), large.overBudgetTargets.end(), "linux"), large.overBudgetTargets.end());
    VoxelProjectSettings lower;
    lower.cpuBudgetMB = 1;
    const VoxelCreateEstimate lowered = estimateVoxelCreate(options, lower);
    EXPECT_NE(std::find(lowered.overBudgetTargets.begin(), lowered.overBudgetTargets.end(), "linux"), lowered.overBudgetTargets.end());
    custom.dimensions = voxel::Int3{512, 64, 64};
    const VoxelCreateEstimate oversize = estimateVoxelCreate(custom, settings);
    EXPECT_NE(std::find(oversize.oversizeTargets.begin(), oversize.oversizeTargets.end(), "web"), oversize.oversizeTargets.end());

    // what cannot be made says so
    VoxelCreateOptions noTerrain;
    noTerrain.base = VoxelCreateBase::FromTerrain;
    EXPECT_FALSE(voxelCreateProblem(noTerrain).empty());
    std::string error;
    EXPECT_EQ(createVoxelVolume(project.get(), sceneProject->id, noTerrain, &error), NULL_ENTITY);
    EXPECT_FALSE(error.empty());

    // one undo step: the GameObject with its file, next to the scene's assets, with a GUID
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    history->clear();
    VoxelCreateOptions make;
    make.name = "Cave";
    make.base = VoxelCreateBase::SolidGround;
    make.voxelSize = 2.0f;
    make.position = Vector3(1.0f, 2.0f, 3.0f);
    error.clear();
    const Entity entity = createVoxelVolume(project.get(), sceneProject->id, make, &error);
    ASSERT_NE(entity, NULL_ENTITY) << error;
    EXPECT_EQ(history->entries().size(), 1u);
    Scene* scene = sceneProject->scene;
    ASSERT_TRUE(scene->isEntityCreated(entity));
    const VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(entity);
    ASSERT_NE(component, nullptr);
    EXPECT_FALSE(component->volumeData.guid.isNull());
    EXPECT_EQ(component->materialSlots.size(), 1u);
    const Transform& transform = scene->getComponent<Transform>(entity);
    EXPECT_EQ(transform.position, Vector3(1.0f, 2.0f, 3.0f));
    const std::filesystem::path file = project->getAssetsPath() / component->volumeData.path;
    EXPECT_TRUE(std::filesystem::exists(file)) << file;
    const std::filesystem::path sceneFile = sceneProject->filepath.is_absolute() ? sceneProject->filepath : project->getProjectPath() / sceneProject->filepath;
    EXPECT_EQ(file.parent_path().lexically_normal(), sceneFile.parent_path().lexically_normal()) << "beside the scene";
    system->update(0.0);
    const std::shared_ptr<VoxelAsset> made = system->getAsset(entity);
    ASSERT_NE(made, nullptr);
    ASSERT_NE(made->store, nullptr);
    EXPECT_EQ(made->store->desc().dimensions, (voxel::Int3{64, 64, 64}));
    EXPECT_FLOAT_EQ(made->store->desc().voxelSize, 2.0f);
    EXPECT_EQ(made->generator.kind, "solid_ground");

    const std::string path = component->volumeData.path;
    ASSERT_TRUE(history->undo());
    EXPECT_FALSE(scene->isEntityCreated(entity));
    EXPECT_EQ(history->entries().size(), 1u);
    EXPECT_TRUE(std::filesystem::exists(file)) << "a redo finds the file";
    ASSERT_TRUE(history->redo());
    ASSERT_TRUE(scene->isEntityCreated(entity));
    EXPECT_EQ(scene->getComponent<VoxelVolumeComponent>(entity).volumeData.path, path);

    // a second one takes its own file
    VoxelCreateOptions another;
    another.name = "Cave";
    const Entity second = createVoxelVolume(project.get(), sceneProject->id, another, &error);
    ASSERT_NE(second, NULL_ENTITY);
    EXPECT_NE(scene->getComponent<VoxelVolumeComponent>(second).volumeData.path, path);
}

TEST_F(VoxelPanel, ErrorStripPerCode) {
    voxel::VolumeDesc desc = smallDesc();
    voxel::GeneratorDesc ground = groundGenerator();

    // a healthy volume shows no strip, and logs nothing
    addVolumeWith("voxels/fine.voxelvolume", desc, ground);
    EXPECT_TRUE(codes().empty());

    // no_slots
    Scene* scene = sceneProject->scene;
    scene->getComponent<VoxelVolumeComponent>(owner).materialSlots.clear();
    EXPECT_TRUE(has(VoxelProblemCode::NoSlots));
    scene->getComponent<VoxelVolumeComponent>(owner).materialSlots.resize(1);

    // module_off, and nothing else then matters
    EXPECT_TRUE(has(VoxelProblemCode::ModuleOff, false));
    EXPECT_EQ(codes(false).size(), 1u);

    // non_unit_scale (R1)
    scene->getComponent<Transform>(owner).scale = Vector3(2.0f, 2.0f, 2.0f);
    EXPECT_TRUE(has(VoxelProblemCode::NonUnitScale));
    scene->getComponent<Transform>(owner).scale = Vector3(1.0f, 1.0f, 1.0f);

    // over_budget: a budget below what the volume holds, named per target. A small cavity or mound in
    // every level-0 block makes each one hold its samples: 64 blocks of 16 KB, and the coarser level
    VoxelProjectSettings lower;
    lower.cpuBudgetMB = 1;
    project->setVoxelSettings(lower);
    std::vector<voxel::EditOp> everyBlock;
    for (int z = 0; z < 4; z++) {
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                voxel::EditOp op;
                op.shape.kind = voxel::ShapeKind::Sphere;
                op.shape.center = voxel::Vec3d{x * 16.0 + 8.0, y * 16.0 + 8.0, z * 16.0 + 8.0};
                op.shape.radius = 3.0;
                // the ground is solid below y = 32: a cavity there, a mound above
                op.kind = op.shape.center.y < 32.0 ? voxel::EditKind::Dig : voxel::EditKind::Add;
                everyBlock.push_back(op);
            }
        }
    }
    ASSERT_TRUE(system->edit(owner, everyBlock, voxel::EditOptions{}).ok);
    ASSERT_GT(asset->store->memory().total(), int64_t(1) << 20);
    EXPECT_TRUE(has(VoxelProblemCode::OverBudget));
    project->setVoxelSettings(VoxelProjectSettings{});

    // oversize_for_target: larger than the web takes
    voxel::VolumeDesc big = desc;
    big.dimensions = voxel::Int3{512, 64, 64};
    addVolumeWith("voxels/big.voxelvolume", big, ground);
    EXPECT_TRUE(has(VoxelProblemCode::OversizeForTarget));
    VoxelProjectSettings ignore;
    ignore.ignoreDimensionLimits = true;
    project->setVoxelSettings(ignore);
    EXPECT_FALSE(has(VoxelProblemCode::OversizeForTarget));
    project->setVoxelSettings(VoxelProjectSettings{});

    // missing_noise_graph: a base that cannot be built names the missing graph
    voxel::GeneratorDesc graph;
    graph.kind = "noise_graph";
    graph.sourcePath = "gone/caves.noisegraph";
    addVolumeWith("voxels/graph.voxelvolume", desc, graph);
    EXPECT_TRUE(has(VoxelProblemCode::MissingNoiseGraph));
    EXPECT_FALSE(has(VoxelProblemCode::BaseStale)) << "the graph being gone is the reason";

    // base_stale: the source is there and the base does not reflect it
    const std::filesystem::path graphFile = project->getAssetsPath() / "voxels/there.noisegraph";
    {
        std::ofstream out(graphFile);
        out << "{}";
    }
    graph.sourcePath = "voxels/there.noisegraph";
    addVolumeWith("voxels/stale.voxelvolume", desc, graph);
    EXPECT_TRUE(has(VoxelProblemCode::BaseStale)) << "this session could not build it";
    EXPECT_FALSE(has(VoxelProblemCode::MissingNoiseGraph));
    // ... and built, against the hash the generator stored (V10)
    asset->baseResolved = true;
    EXPECT_FALSE(has(VoxelProblemCode::BaseStale)) << "no hash stored, nothing to compare";
    asset->generator.sourceHash = 1;
    EXPECT_TRUE(has(VoxelProblemCode::BaseStale)) << "the source changed since its hash";

    // missing_terrain
    voxel::GeneratorDesc terrain;
    terrain.kind = "terrain";
    addVolumeWith("voxels/terrain.voxelvolume", desc, terrain);
    EXPECT_TRUE(has(VoxelProblemCode::MissingTerrain));

    // missing_file: a path nobody has
    addVolumeWith("voxels/here.voxelvolume", desc, ground);
    const Entity missing = scene->createEntity();
    scene->addComponent<Transform>(missing, {});
    VoxelVolumeComponent nothing;
    nothing.collision = false;
    nothing.navigationUpdate = VoxelNavigationUpdate::Off;
    nothing.volumeData.path = "voxels/never-saved.voxelvolume";
    scene->addComponent<VoxelVolumeComponent>(missing, nothing);
    sceneProject->entities.push_back(missing);
    system->update(0.0);
    owner = missing;
    EXPECT_TRUE(has(VoxelProblemCode::MissingFile));

    // no_asset
    scene->getComponent<VoxelVolumeComponent>(missing).volumeData = AssetRef();
    EXPECT_TRUE(has(VoxelProblemCode::NoAsset));

    // corrupt_file: it loads as an error, the strip says so, and the file stays untouched (R61)
    const std::filesystem::path corrupt = project->getAssetsPath() / "voxels/bad.voxelvolume";
    {
        std::ofstream out(corrupt, std::ios::binary);
        out << "not a volume at all";
    }
    Entity broken = scene->createEntity();
    scene->addComponent<Transform>(broken, {});
    VoxelVolumeComponent bad;
    bad.collision = false;
    bad.navigationUpdate = VoxelNavigationUpdate::Off;
    bad.volumeData.path = "voxels/bad.voxelvolume";
    scene->addComponent<VoxelVolumeComponent>(broken, bad);
    sceneProject->entities.push_back(broken);
    system->update(0.0);
    owner = broken;
    EXPECT_TRUE(has(VoxelProblemCode::CorruptFile));
    const std::shared_ptr<VoxelAsset> badAsset = system->getAsset(broken);
    ASSERT_NE(badAsset, nullptr);
    std::string refusal;
    EXPECT_FALSE(saveVoxelVolume(project.get(), sceneProject->id, broken, &refusal));
    EXPECT_FALSE(refusal.empty());
    EXPECT_EQ(std::filesystem::file_size(corrupt), std::string("not a volume at all").size());

    // the log line, once per code until it goes away and comes back
    if (!Out::memorySinkId()) Out::startLogging();
    const std::vector<VoxelProblem> problems = voxelProblems(*project, *sceneProject, broken);
    ASSERT_FALSE(problems.empty());
    const uint64_t cursor = Out::lastLogId();
    EXPECT_EQ(logVoxelProblems(*sceneProject, broken, problems), static_cast<int>(problems.size()));
    EXPECT_EQ(logVoxelProblems(*sceneProject, broken, problems), 0);
    EXPECT_EQ(logVoxelProblems(*sceneProject, broken, problems), 0);
    const std::vector<editor::LogEntry> logs = Out::getLogsSince(cursor);
    EXPECT_EQ(logs.size(), problems.size());
    bool named = false;
    for (const editor::LogEntry& entry : logs) named = named || entry.message.find("corrupt_file") != std::string::npos;
    EXPECT_TRUE(named);
    EXPECT_EQ(logVoxelProblems(*sceneProject, broken, {}), 0);
    EXPECT_EQ(logVoxelProblems(*sceneProject, broken, problems), static_cast<int>(problems.size())) << "back again, it is news again";
}

TEST_F(VoxelPanel, StatsMatchSample) {
    addVolume();
    const std::vector<voxel::EditOp> ops = digAt(3);
    ASSERT_TRUE(system->edit(owner, ops, voxel::EditOptions{}).ok);
    // the meshes finish on jobs and show on the system's next updates
    for (int i = 0; i < 4; i++) {
        system->finishPendingWork();
        system->update(0.0);
    }

    const VoxelStats stats = system->getStats(owner);
    const voxel::Store::BlockCounts counted = asset->store->countBlocks();
    EXPECT_EQ(stats.blocks.total, counted.total);
    EXPECT_GT(stats.blocks.modified, 0);

    const std::vector<VoxelStatsRow> rows = voxelStatsRows(stats, project->getVoxelSettings());
    const auto rowValue = [&rows](const std::string& label) {
        for (const VoxelStatsRow& row : rows) {
            if (row.label == label) return row.value;
        }
        ADD_FAILURE() << "no row " << label;
        return std::string();
    };
    EXPECT_EQ(rowValue("Blocks Total"), std::to_string(counted.total));
    EXPECT_EQ(rowValue("Uniform"), std::to_string(counted.uniform));
    EXPECT_EQ(rowValue("Non-uniform"), std::to_string(counted.nonUniform));
    EXPECT_EQ(rowValue("Hot"), std::to_string(counted.total - counted.uniform - counted.base - counted.cold));
    EXPECT_EQ(rowValue("Cold"), std::to_string(counted.cold));
    EXPECT_EQ(rowValue("Modified"), std::to_string(counted.modified));
    EXPECT_EQ(rowValue("Triangles"), std::to_string(stats.triangles));
    EXPECT_EQ(rowValue("Chunks"), std::to_string(stats.chunks));
    EXPECT_EQ(rowValue("Draws"), std::to_string(stats.draws));
    EXPECT_EQ(rowValue("Jobs Pending"), std::to_string(stats.jobsPending));
    EXPECT_EQ(rowValue("Collision Bodies"), std::to_string(stats.collisionBodies));
    EXPECT_GT(stats.chunks, 0);
    EXPECT_GT(stats.triangles, 0);
    // memory against each target's budget
    const std::string web = rowValue("Budget web");
    EXPECT_NE(web.find("32.00 MB"), std::string::npos) << web;
    VoxelProjectSettings lower;
    lower.cpuBudgetMB = 16;
    const std::vector<VoxelStatsRow> lowered = voxelStatsRows(stats, lower);
    bool sawLowered = false;
    for (const VoxelStatsRow& row : lowered) sawLowered = sawLowered || (row.label == "Budget web" && row.value.find("16.00 MB") != std::string::npos);
    EXPECT_TRUE(sawLowered);
}

TEST_F(VoxelMaterials, RemoveRefusedInUse) {
    addVolume();
    Scene* scene = sceneProject->scene;
    VoxelVolumeComponent& component = scene->getComponent<VoxelVolumeComponent>(owner);
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    std::string error;

    // Add takes the next index, each one undo step
    component.materialSlots.resize(1);
    component.materialSlots[0].name = "Rock";
    history->clear();
    EXPECT_EQ(addVoxelMaterialSlot(project.get(), sceneProject->id, owner, "Sand", &error), 1);
    EXPECT_EQ(addVoxelMaterialSlot(project.get(), sceneProject->id, owner, "Moss", &error), 2);
    EXPECT_EQ(addVoxelMaterialSlot(project.get(), sceneProject->id, owner, "Ice", &error), 3);
    EXPECT_EQ(history->entries().size(), 3u);
    ASSERT_EQ(scene->getComponent<VoxelVolumeComponent>(owner).materialSlots.size(), 4u);

    // voxels on slot 2 (paint a patch around the ground's surface)
    voxel::EditOp paint;
    paint.kind = voxel::EditKind::Paint;
    paint.material = 2;
    paint.shape.center = voxel::Vec3d{32.0, 32.0, 32.0};
    paint.shape.radius = 5.0;
    ASSERT_TRUE(system->edit(owner, std::span<const voxel::EditOp>(&paint, 1), voxel::EditOptions{}).ok);
    const uint16_t used = voxelSlotsInUse(*asset->store);
    EXPECT_TRUE(used & (1u << 2));
    EXPECT_FALSE(used & (1u << 1));

    // in use: refused, saying what to do, nothing changes
    const size_t steps = history->entries().size();
    error.clear();
    EXPECT_FALSE(removeVoxelMaterialSlot(project.get(), sceneProject->id, owner, 2, &error));
    EXPECT_NE(error.find("Replace With"), std::string::npos) << error;
    EXPECT_EQ(history->entries().size(), steps);
    EXPECT_EQ(scene->getComponent<VoxelVolumeComponent>(owner).materialSlots[2].name, "Moss");

    // not in use: it goes, and the slots after it stay where they are
    error.clear();
    EXPECT_TRUE(removeVoxelMaterialSlot(project.get(), sceneProject->id, owner, 1, &error)) << error;
    {
        const std::vector<VoxelMaterialSlot>& slots = scene->getComponent<VoxelVolumeComponent>(owner).materialSlots;
        ASSERT_EQ(slots.size(), 4u);
        EXPECT_TRUE(isVoxelSlotEmpty(slots[1]));
        EXPECT_EQ(slots[2].name, "Moss");
        EXPECT_EQ(slots[3].name, "Ice");
    }
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(scene->getComponent<VoxelVolumeComponent>(owner).materialSlots[1].name, "Sand");

    // Replace With: the voxels move and the slot goes in one undo step
    const uint64_t before = hash();
    // (the undone removal is the redo tail, which the new step replaces)
    const size_t stepsBefore = history->position();
    error.clear();
    ASSERT_TRUE(replaceVoxelMaterialSlot(project.get(), sceneProject->id, owner, 2, 3, &error)) << error;
    EXPECT_EQ(history->position(), stepsBefore + 1);
    EXPECT_FALSE(history->canRedo());
    const uint16_t after = voxelSlotsInUse(*asset->store);
    EXPECT_FALSE(after & (1u << 2));
    EXPECT_TRUE(after & (1u << 3));
    EXPECT_NE(hash(), before);
    EXPECT_EQ(voxel::materialAtPoint(*asset->store, voxel::Vec3d{32.0, 31.0, 32.0}).slot, 3);
    {
        const std::vector<VoxelMaterialSlot>& slots = scene->getComponent<VoxelVolumeComponent>(owner).materialSlots;
        ASSERT_EQ(slots.size(), 4u);
        EXPECT_TRUE(isVoxelSlotEmpty(slots[2]));
        EXPECT_EQ(slots[3].name, "Ice") << "indices stay";
    }
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(hash(), before);
    EXPECT_EQ(scene->getComponent<VoxelVolumeComponent>(owner).materialSlots[2].name, "Moss");
    EXPECT_TRUE(voxelSlotsInUse(*asset->store) & (1u << 2));
    ASSERT_TRUE(history->redo());
    EXPECT_FALSE(voxelSlotsInUse(*asset->store) & (1u << 2));

    // the free index is filled first; a slot removed at the end shortens the list
    error.clear();
    EXPECT_EQ(addVoxelMaterialSlot(project.get(), sceneProject->id, owner, "Tar", &error), 2);
    EXPECT_TRUE(removeVoxelMaterialSlot(project.get(), sceneProject->id, owner, 2, &error)) << error;
    ASSERT_TRUE(replaceVoxelMaterialSlot(project.get(), sceneProject->id, owner, 3, 0, &error)) << error;
    EXPECT_LE(scene->getComponent<VoxelVolumeComponent>(owner).materialSlots.size(), 2u);

    // a link follows a Terrain Layer, one undo step
    ASSERT_TRUE(linkVoxelMaterialSlot(project.get(), sceneProject->id, owner, 0, 4, &error)) << error;
    EXPECT_EQ(scene->getComponent<VoxelVolumeComponent>(owner).materialSlots[0].linkedTerrainLayer, 4);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(scene->getComponent<VoxelVolumeComponent>(owner).materialSlots[0].linkedTerrainLayer, -1);
}
