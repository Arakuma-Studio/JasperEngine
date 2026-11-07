// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R7, R49, R78 (AC6, AC39, AC50): what an export does with a volume

#include "cook/VoxelVolumeCook.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCollector.h"
#include "pool/ShaderPool.h"
#include "subsystem/VoxelSystem.h"
#include "support/TestTempDir.h"
#include "voxel/VoxelEdit.h"
#include "voxel/VoxelFile.h"
#include "voxel/VoxelGenerator.h"
#include "voxel/VoxelSample.h"

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::cook;

namespace voxel_cook_test {

    voxel::VolumeDesc desc(int x, int y, int z, int levels = 1) {
        voxel::VolumeDesc out;
        out.dimensions = {x, y, z};
        out.levels = levels;
        return out;
    }

    std::vector<uint8_t> editorFile(const voxel::VolumeDesc& volume, const std::string& generatorJson) {
        voxel::Store store(volume);
        store.setBase(voxel::makeEmptyBase());
        voxel::SaveOptions options;
        options.slotCount = 1;
        options.generatorJson = generatorJson;
        return voxel::saveVolumeFile(store, options);
    }

    std::string emptyGenerator() {
        return voxel::generatorToJson(voxel::GeneratorDesc());
    }

    // the defines of the MESH variants of a scene holding a volume of this quality
    std::set<std::string> voxelDefines(VoxelQuality quality, bool volumePresent = true) {
        Scene scene;
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        if (volumePresent) {
            VoxelVolumeComponent component;
            component.quality = quality;
            scene.addComponent<VoxelVolumeComponent>(entity, component);
        }
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = {entity};
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        std::set<std::string> out;
        for (ShaderKey key : keys) {
            if (ShaderPool::getShaderTypeFromKey(key) != ShaderType::MESH) continue;
            for (const std::string& define : ShaderReference::getFeatures(ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(key)))) {
                if (define.find("VOXEL") != std::string::npos) out.insert(define);
            }
        }
        return out;
    }

}

using namespace voxel_cook_test;

// AC6: over the target's limit, the export stops naming the volume and the target
TEST(VoxelExport, OversizeFailsNamingTarget) {
    const std::vector<uint8_t> file = editorFile(desc(512, 16, 16), emptyGenerator());
    VoxelCookOptions options;
    options.hasTarget = true;
    std::vector<uint8_t> cooked;
    std::string error;

    options.target = BuildPlatform::Web;
    EXPECT_FALSE(cookVoxelVolume(file, "caves/big.voxelvolume", options, cooked, error));
    EXPECT_NE(error.find("caves/big.voxelvolume"), std::string::npos) << error;
    EXPECT_NE(error.find("web"), std::string::npos) << error;
    EXPECT_NE(error.find("512"), std::string::npos) << error;
    EXPECT_NE(error.find("256"), std::string::npos) << error;

    // R7's table: it fits the others
    for (BuildPlatform fits : {BuildPlatform::Android, BuildPlatform::IOS, BuildPlatform::Windows, BuildPlatform::Linux, BuildPlatform::MacOS}) {
        options.target = fits;
        error.clear();
        EXPECT_TRUE(cookVoxelVolume(file, "caves/big.voxelvolume", options, cooked, error)) << buildPlatformName(fits) << ": " << error;
    }
    const std::vector<uint8_t> huge = editorFile(desc(1024, 16, 16), emptyGenerator());
    options.target = BuildPlatform::Android;
    error.clear();
    EXPECT_FALSE(cookVoxelVolume(huge, "caves/huge.voxelvolume", options, cooked, error));
    EXPECT_NE(error.find("android"), std::string::npos) << error;
    options.target = BuildPlatform::Linux;
    error.clear();
    EXPECT_TRUE(cookVoxelVolume(huge, "caves/huge.voxelvolume", options, cooked, error)) << error;

    // Project Settings > Voxels lowers it
    options.settings.maxDimension = 256;
    error.clear();
    EXPECT_FALSE(cookVoxelVolume(file, "caves/big.voxelvolume", options, cooked, error));
    EXPECT_NE(error.find("linux"), std::string::npos) << error;
    // and voxel.ignoreDimensionLimits lets it through
    options.settings.ignoreDimensionLimits = true;
    options.target = BuildPlatform::Web;
    error.clear();
    EXPECT_TRUE(cookVoxelVolume(file, "caves/big.voxelvolume", options, cooked, error)) << error;
    // a source export has no target
    VoxelCookOptions source;
    error.clear();
    EXPECT_TRUE(cookVoxelVolume(file, "caves/big.voxelvolume", source, cooked, error)) << error;
}

// the settings never raise a limit
TEST(VoxelExport, SettingsOnlyLowerLimits) {
    VoxelProjectSettings settings;
    settings.maxDimension = 2048;
    settings.cpuBudgetMB = 512;
    settings.meshBudgetMB = 8;
    const VoxelLimits limits = lowerVoxelLimits(voxelTargetLimits(BuildPlatform::Web), settings);
    EXPECT_EQ(limits.maxDimension, 256);
    EXPECT_EQ(limits.cpuBudget, 32 * 1024 * 1024);
    EXPECT_EQ(limits.meshBudget, 8 * 1024 * 1024);
    EXPECT_EQ(generateVoxelSettingsCode(VoxelProjectSettings(), "    "), "");
    const std::string code = generateVoxelSettingsCode(settings, "    ");
    EXPECT_NE(code.find("setVoxelLimitsOverride"), std::string::npos);
    EXPECT_NE(code.find("#if JASPERENGINE_VOXEL"), std::string::npos);

    // project.yaml: nothing unless set, and it reads back
    EXPECT_EQ(encodeVoxelProjectSettings(VoxelProjectSettings()).size(), 0u);
    settings.ignoreDimensionLimits = true;
    EXPECT_EQ(decodeVoxelProjectSettings(encodeVoxelProjectSettings(settings)), settings);
    EXPECT_EQ(decodeVoxelProjectSettings(YAML::Node()), VoxelProjectSettings());
}

// AC39: chunks are in no saved scene, so the collector lists what they draw with
TEST(VoxelExport, CollectorListsTierVariants) {
    EXPECT_TRUE(voxelDefines(VoxelQuality::Auto, false).empty());
    EXPECT_EQ(voxelDefines(VoxelQuality::High), (std::set<std::string>{"HAS_VOXEL", "VOXEL_TIER_HIGH"}));
    EXPECT_EQ(voxelDefines(VoxelQuality::Medium), (std::set<std::string>{"HAS_VOXEL", "VOXEL_TIER_MEDIUM"}));
    EXPECT_EQ(voxelDefines(VoxelQuality::Low), (std::set<std::string>{"HAS_VOXEL", "VOXEL_TIER_LOW"}));
    // Auto: the target and an attached terrain choose (R51)
    EXPECT_EQ(voxelDefines(VoxelQuality::Auto), (std::set<std::string>{"HAS_VOXEL", "VOXEL_TIER_HIGH", "VOXEL_TIER_MEDIUM", "VOXEL_TIER_LOW"}));
}

namespace voxel_cook_test {

    // How many MESH keys of a scene with one terrain carry 'Thl', with or without an attached volume
    int holedTerrainKeys(VoxelTerrainCut cut, bool attached) {
        Scene scene;
        const Entity terrainEntity = scene.createEntity();
        scene.addComponent<Transform>(terrainEntity, {});
        TerrainComponent terrain;
        terrain.heightMap.setPath("height.png");
        scene.addComponent<TerrainComponent>(terrainEntity, terrain);
        MeshComponent mesh;
        mesh.numSubmeshes = 1;
        scene.addComponent<MeshComponent>(terrainEntity, mesh);
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = {terrainEntity};
        if (attached) {
            const Entity volume = scene.createEntity();
            scene.addComponent<Transform>(volume, {});
            VoxelVolumeComponent component;
            component.quality = VoxelQuality::Low;
            component.terrain = terrainEntity;
            component.terrainCut = cut;
            scene.addComponent<VoxelVolumeComponent>(volume, component);
            content.entities.push_back(volume);
        }
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        int holed = 0;
        for (ShaderKey key : keys) {
            if (ShaderPool::getShaderTypeFromKey(key) == ShaderType::MESH && (ShaderPool::getPropertiesFromKey(key) & ShaderPool::getTerrainHolesProperty(ShaderType::MESH))) holed++;
        }
        return holed;
    }

}

namespace voxel_cook_test {

    // The voxel tiers the MESH keys of a scene holding one volume of this Quality draw with
    std::set<std::string> collectedVoxelTiers(VoxelQuality quality) {
        Scene scene;
        const Entity volume = scene.createEntity();
        scene.addComponent<Transform>(volume, {});
        VoxelVolumeComponent component;
        component.quality = quality;
        scene.addComponent<VoxelVolumeComponent>(volume, component);
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = {volume};
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        std::set<std::string> tiers;
        for (ShaderKey key : keys) {
            if (ShaderPool::getShaderTypeFromKey(key) != ShaderType::MESH) continue;
            const std::string name = ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(key));
            for (const char* tier : {"VOXEL_TIER_HIGH", "VOXEL_TIER_MEDIUM", "VOXEL_TIER_LOW"}) {
                if (name.find(tier) != std::string::npos) tiers.insert(tier);
            }
        }
        return tiers;
    }

}

// AC39: the chunks' variants come out of the collector's per-device-tier pass, the one an export reads
TEST(VoxelExport, CollectorShipsChunkVariants) {
    EXPECT_EQ(collectedVoxelTiers(VoxelQuality::Low), (std::set<std::string>{"VOXEL_TIER_LOW"}));
    EXPECT_EQ(collectedVoxelTiers(VoxelQuality::Auto), (std::set<std::string>{"VOXEL_TIER_HIGH", "VOXEL_TIER_MEDIUM", "VOXEL_TIER_LOW"}));
}

// AC34, T3 R34: a first in-game dig finds the holed variant in the build
TEST(VoxelExport, AttachedShipsHoleVariant) {
    EXPECT_EQ(holedTerrainKeys(VoxelTerrainCut::Off, false), 0);
    EXPECT_EQ(holedTerrainKeys(VoxelTerrainCut::Off, true), 0);
    EXPECT_GT(holedTerrainKeys(VoxelTerrainCut::Automatic, true), 0);
}

// AC50 (without its run_build half): a baked file loads with no source and evaluates nothing
TEST(VoxelCook, BakedLoadsWithoutSources) {
    // a base only the resolver can make, as a noise graph or a mesh's is, counted
    static std::atomic<int> resolved{0};
    resolved = 0;
    VoxelAssets::setBaseResolver([](const voxel::GeneratorDesc&, const voxel::VolumeDesc& volume) -> std::shared_ptr<const voxel::BaseSource> {
        resolved++;
        return voxel::makeSolidGroundBase(32.0, volume.voxelSize, 0);
    });

    const voxel::VolumeDesc volume = desc(64, 64, 64, 2);
    voxel::GeneratorDesc generator;
    generator.kind = "mesh";
    generator.sourcePath = "models/rock.glb";
    voxel::Store store(volume);
    store.setBase(voxel::makeSolidGroundBase(32.0, volume.voxelSize, 0));
    voxel::EditOp dig;
    dig.kind = voxel::EditKind::Dig;
    dig.shape.kind = voxel::ShapeKind::Sphere;
    dig.shape.center = voxel::Vec3d{20.0, 32.0, 20.0};
    dig.shape.radius = 6.0;
    const voxel::EditOp ops[] = {dig};
    ASSERT_TRUE(voxel::applyEdit(store, ops).ok);
    const uint64_t editHash = voxel::contentHash(store);
    voxel::SaveOptions save;
    save.slotCount = 1;
    save.generatorJson = voxel::generatorToJson(generator);
    const std::vector<uint8_t> editor = voxel::saveVolumeFile(store, save);

    std::vector<uint8_t> cooked;
    std::string error;
    ASSERT_TRUE(cookVoxelVolume(editor, "rock.voxelvolume", VoxelCookOptions(), cooked, error)) << error;
    EXPECT_EQ(resolved.load(), 1);      // the cook evaluates it

    // flagged, the generator gone, zstd in it
    const voxel::VolumeFileResult info = voxel::readVolumeFileInfo(cooked);
    ASSERT_TRUE(info.ok) << info.message;
    EXPECT_TRUE(info.info.flags & voxel::VOXEL_FILE_COOKED);
    EXPECT_TRUE(info.info.generatorJson.empty());
    EXPECT_GT(info.info.entryCount, 0u);
    // fixed level, canonical: the same input cooks to the same bytes
    std::vector<uint8_t> second;
    ASSERT_TRUE(cookVoxelVolume(editor, "rock.voxelvolume", VoxelCookOptions(), second, error)) << error;
    EXPECT_EQ(second, cooked);

    // a player's load: no resolver reached, and the content is what the editor had
    resolved = 0;
    auto bytes = std::make_shared<std::vector<uint8_t>>(cooked);
    std::shared_ptr<VoxelAsset> loaded = VoxelAssets::fromBytes("mem://baked.voxelvolume", bytes);
    ASSERT_EQ(loaded->error, voxel::FileError::None) << loaded->errorMessage;
    EXPECT_EQ(resolved.load(), 0);
    EXPECT_EQ(voxel::contentHash(*loaded->store), editHash);
    VoxelAssets::forget("mem://baked.voxelvolume");

    // runtimeGeneration keeps the generator and the player's base is that, so only the edits ship
    VoxelCookOptions keep;
    keep.keepGenerator = true;
    std::vector<uint8_t> kept;
    ASSERT_TRUE(cookVoxelVolume(editor, "rock.voxelvolume", keep, kept, error)) << error;
    const voxel::VolumeFileResult keptInfo = voxel::readVolumeFileInfo(kept);
    ASSERT_TRUE(keptInfo.ok) << keptInfo.message;
    EXPECT_NE(keptInfo.info.generatorJson.find("mesh"), std::string::npos);
    EXPECT_LT(kept.size(), cooked.size());

    // no resolver: a generator nothing can evaluate stops the cook, naming the volume
    VoxelAssets::setBaseResolver(nullptr);
    EXPECT_FALSE(cookVoxelVolume(editor, "rock.voxelvolume", VoxelCookOptions(), cooked, error));
    EXPECT_NE(error.find("rock.voxelvolume"), std::string::npos) << error;
}

TEST(VoxelCook, CorruptFileNamesTheVolume) {
    std::vector<uint8_t> file = editorFile(desc(64, 64, 64), emptyGenerator());
    file[file.size() / 2] ^= 0xFF;
    std::vector<uint8_t> cooked;
    std::string error;
    EXPECT_FALSE(cookVoxelVolume(file, "broken.voxelvolume", VoxelCookOptions(), cooked, error));
    EXPECT_NE(error.find("broken.voxelvolume"), std::string::npos) << error;
}

TEST(VoxelCook, FileIsWrittenAtomically) {
    jasperengine::test::TestTempDir dir("voxel-cook");
    const std::vector<uint8_t> file = editorFile(desc(64, 64, 64), emptyGenerator());
    jasperengine::test::TestTempDir::write(dir.path() / "a.voxelvolume", std::string(reinterpret_cast<const char*>(file.data()), file.size()));
    std::string error;
    ASSERT_TRUE(cookVoxelVolumeFile(dir.path() / "a.voxelvolume", dir.path() / "out" / "a.voxelvolume", "a.voxelvolume", VoxelCookOptions(), error)) << error;
    const std::string cooked = jasperengine::test::TestTempDir::read(dir.path() / "out" / "a.voxelvolume");
    const voxel::VolumeFileResult info = voxel::readVolumeFileInfo(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(cooked.data()), cooked.size()));
    ASSERT_TRUE(info.ok) << info.message;
    EXPECT_TRUE(info.info.flags & voxel::VOXEL_FILE_COOKED);
    EXPECT_TRUE(isVoxelVolumeFile("x/Y.VoxelVolume"));
    EXPECT_FALSE(isVoxelVolumeFile("x/y.navmesh"));
}
