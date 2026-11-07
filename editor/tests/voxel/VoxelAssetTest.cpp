// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R61, R77 (AC49): identity, writes, reloads and recovery of a .voxelvolume in a project

#include "VoxelTestScene.h"

#include "AppSettings.h"
#include "DocumentFile.h"
#include "SceneBackup.h"
#include "asset/AssetIndex.h"
#include "asset/AssetTypes.h"
#include "command/type/VoxelEditCmd.h"
#include "io/AtomicFile.h"
#include "reload/HotReloadService.h"
#include "reload/handlers/ReloadHandlers.h"
#include "voxel/VoxelFile.h"

#include "json.hpp"

#include <chrono>
#include <fstream>
#include <thread>

#if !defined(_WIN32)
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::voxeltest;

namespace voxel_asset_test {

    using jasperengine::test::TestTempDir;

    // a process that is gone, as a crashed session's backup names
    int64_t deadPid() {
#if defined(_WIN32)
        return 4294966;
#else
        const pid_t child = ::fork();
        if (child == 0) ::_exit(0);
        int status = 0;
        ::waitpid(child, &status, 0);
        return static_cast<int64_t>(child);
#endif
    }

    void setBackupPid(const fs::path& backup, int64_t pid) {
        const fs::path sidecar = SceneBackups::sidecarPath(backup);
        nlohmann::json json = nlohmann::json::parse(TestTempDir::read(sidecar));
        json["pid"] = pid;
        TestTempDir::write(sidecar, json.dump(2));
    }

    class VoxelAssetTest : public VoxelSceneTest {};

    // a file's bytes with the content of `index` digs
    std::string otherContent(int index) {
        voxel::Store store(smallDesc());
        store.setBase(voxel::makeSolidGroundBase(32.0, 1.0, 0));
        const std::vector<voxel::EditOp> ops = digAt(index);
        voxel::applyEdit(store, ops);
        voxel::SaveOptions options;
        options.slotCount = 1;
        options.generatorJson = voxel::generatorToJson(groundGenerator());
        const std::vector<uint8_t> bytes = voxel::saveVolumeFile(store, options);
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

}

using namespace voxel_asset_test;

TEST_F(VoxelAssetTest, GuidSidecarAndRefs) {
    addVolume();
    AssetDatabase& database = project->getAssetDatabase();
    EXPECT_EQ(database.typeOf(assetFile()), AssetType::VoxelVolume);
    const Guid guid = database.getAssetGuid(assetFile());
    ASSERT_FALSE(guid.isNull());
    EXPECT_TRUE(fs::exists(AssetTypes::metaPathFor(assetFile())));

    // the scene holds it by GUID, with the path as the hint
    VoxelVolumeComponent& component = sceneProject->scene->getComponent<VoxelVolumeComponent>(owner);
    EXPECT_EQ(component.volumeData.path, assetPath);
    EXPECT_EQ(component.volumeData.guid, guid);
    bool visited = false;
    AssetRefSchema::forEachAssetRef(*sceneProject->scene, owner, [&](AssetRef& ref, const AssetRefSite& site) {
        if (site.component == "VoxelVolumeComponent" && site.field == "volumeData") {
            visited = ref.path == assetPath;
        }
    });
    EXPECT_TRUE(visited);

    // what its generator reads is a dependency of the asset (R77)
    asset->generator.kind = "mesh";
    asset->generator.sourceGuid = "";
    asset->generator.sourcePath = "models/rock.glb";
    std::string error;
    asset->dirty = true;
    ASSERT_TRUE(saveVoxelAsset(*asset, &error)) << error;
    AssetIndex::Roots roots;
    roots.assetsDir = project->getAssetsPath().lexically_relative(project->getProjectPath()).generic_string();
    const std::vector<DepEdge> edges = AssetIndex::extractFile(database, project->getProjectPath(), roots,
        (project->getAssetsPath() / assetPath).lexically_relative(project->getProjectPath()).generic_string(), AssetType::VoxelVolume, guid);
    bool edge = false;
    for (const DepEdge& candidate : edges) {
        edge = edge || (candidate.field == "generator.source" && candidate.expected == AssetType::Model &&
                        candidate.targetPath.find("models/rock.glb") != std::string::npos);
    }
    EXPECT_TRUE(edge);
    EXPECT_TRUE(AssetIndex::isReferrerType(AssetType::VoxelVolume, assetPath));

    // S29's pattern list
    ASSERT_TRUE(project->writeVersionControlMetadata());
    EXPECT_NE(TestTempDir::read(project->getProjectPath() / ".gitattributes").find("*.voxelvolume"), std::string::npos);
}

TEST_F(VoxelAssetTest, AtomicWriteOnFailure) {
    addVolume();
    const std::string saved = TestTempDir::read(assetFile());
    const std::vector<voxel::SlotChange> delta = edit(1);
    ASSERT_FALSE(delta.empty());
    ASSERT_TRUE(asset->dirty);

    // every step of the write fails in turn: the file keeps its bytes, the volume stays dirty
    for (const char* failing : {"open", "write", "sync", "rename"}) {
        io::setAtomicFileFaultInjector([failing](std::string_view step, const fs::path&) {
            return step == failing ? std::make_error_code(std::errc::io_error) : std::error_code();
        });
        std::string error;
        EXPECT_FALSE(saveVoxelAsset(*asset, &error)) << failing;
        EXPECT_FALSE(error.empty()) << failing;
        EXPECT_EQ(TestTempDir::read(assetFile()), saved) << failing;
        EXPECT_TRUE(asset->dirty) << failing;
    }
    io::setAtomicFileFaultInjector(nullptr);
    for (const fs::directory_entry& entry : fs::directory_iterator(assetFile().parent_path())) {
        EXPECT_FALSE(io::isTemporarySiblingName(entry.path().filename().string())) << entry.path();
    }

    std::string error;
    ASSERT_TRUE(saveVoxelAsset(*asset, &error)) << error;
    EXPECT_NE(TestTempDir::read(assetFile()), saved);
    EXPECT_FALSE(asset->dirty);
    // the same content again: the file is not touched
    const auto stamp = fs::last_write_time(assetFile());
    asset->dirty = true;
    ASSERT_TRUE(saveVoxelAsset(*asset, &error)) << error;
    EXPECT_EQ(fs::last_write_time(assetFile()), stamp);
}

// R61: what did not load is not written over, with the scene's save neither
TEST_F(VoxelAssetTest, FailedLoadRefusesToOverwrite) {
    addVolume();
    const std::string garbage = "not a voxel volume";
    TestTempDir::write(assetFile(), garbage);
    ASSERT_TRUE(reloadVoxelVolumeFile(*project, assetPath));
    asset.reset();
    system->update(0.0);
    const std::shared_ptr<VoxelAsset> failed = system->getAsset(owner);
    ASSERT_NE(failed, nullptr);
    EXPECT_NE(failed->error, voxel::FileError::None);
    EXPECT_FALSE(system->getError(owner).empty());

    std::string error;
    failed->dirty = true;
    EXPECT_FALSE(saveVoxelAsset(*failed, &error));
    EXPECT_NE(error.find(assetPath), std::string::npos) << error;
    saveSceneVoxelAssets(*project, *sceneProject);
    EXPECT_EQ(TestTempDir::read(assetFile()), garbage);
}

TEST_F(VoxelAssetTest, ExternalChangeReloads) {
    addVolume();
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    history->clear();
    ASSERT_TRUE(history->addCommandNoMerge(new VoxelEditCmd(project.get(), sceneProject->id, owner, "Dig Sphere", edit(1))));
    ASSERT_EQ(history->entries().size(), 1u);
    std::weak_ptr<VoxelAsset> old = asset;
    std::string error;
    ASSERT_TRUE(saveVoxelAsset(*asset, &error)) << error;

    // the kind apply() files it under, and the service reading another program's write
    EXPECT_EQ(reload::kindOf(assetFile()), "voxel");
    HotReloadService& service = HotReloadService::get();
    service.attach(*project);
    const std::string external = otherContent(3);
    TestTempDir::write(assetFile(), external);

    // the editor's own report of a write is the write arriving: nothing reloads
    ChangeSet own = ChangeSet::fromPaths({assetFile()});
    own.changes[0].origin = ChangeOrigin::Editor;
    service.apply(own, ApplyOptions());
    EXPECT_EQ(system->getAsset(owner), asset);
    EXPECT_EQ(history->entries().size(), 1u);

    // another program's
    service.apply(ChangeSet::fromPaths({assetFile()}), ApplyOptions());
    service.detach();
    asset.reset();
    EXPECT_TRUE(history->entries().empty());    // the undo steps of the old content are gone
    EXPECT_TRUE(old.expired());                 // and with them what they kept
    system->update(0.0);
    const std::shared_ptr<VoxelAsset> reloaded = system->getAsset(owner);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->error, voxel::FileError::None);
    EXPECT_FALSE(reloaded->dirty);

    voxel::Store expected(smallDesc());
    expected.setBase(voxel::makeSolidGroundBase(32.0, 1.0, 0));
    const std::vector<voxel::EditOp> ops = digAt(3);
    voxel::applyEdit(expected, ops);
    EXPECT_EQ(voxel::contentHash(*reloaded->store), voxel::contentHash(expected));
}

// A step that came before the reload, folded into a bigger step, acts on nothing
TEST_F(VoxelAssetTest, StepOfTheOldContentDoesNothing) {
    addVolume();
    auto stale = std::make_unique<VoxelEditCmd>(project.get(), sceneProject->id, owner, "Dig Sphere", edit(1));
    ASSERT_TRUE(stale->execute());
    TestTempDir::write(assetFile(), otherContent(3));
    ASSERT_TRUE(reloadVoxelVolumeFile(*project, assetPath));
    system->update(0.0);
    const std::shared_ptr<VoxelAsset> reloaded = system->getAsset(owner);
    ASSERT_NE(reloaded, nullptr);
    ASSERT_NE(reloaded, asset);
    const uint64_t before = voxel::contentHash(*reloaded->store);
    stale->undo();
    EXPECT_EQ(voxel::contentHash(*reloaded->store), before);
    EXPECT_FALSE(reloaded->dirty);
}

TEST_F(VoxelAssetTest, AutoSaveRecovery) {
    addVolume();
    const fs::path scenePath = sceneProject->filepath;
    const fs::path volumeSource = (project->getAssetsPath() / assetPath).lexically_relative(project->getProjectPath());
    // the scene is on disk as it is: a volume edit is all a backup has to protect
    ASSERT_TRUE(project->saveSceneToPath(sceneProject->id, sceneProject->filepath));
    const std::string savedVolume = TestTempDir::read(assetFile());
    const std::vector<voxel::SlotChange> delta = edit(4);
    ASSERT_FALSE(delta.empty());
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    ASSERT_TRUE(history->addCommandNoMerge(new VoxelEditCmd(project.get(), sceneProject->id, owner, "Dig Sphere", delta)));
    const uint64_t edited = hash();

    // Auto Save's own path: the first tick starts the interval, the next one writes
    const bool autosave = AppSettings::isAutosaveEnabled();
    AppSettings::setAutosaveEnabled(true);
    const float interval = project->getAutosaveInterval(sceneProject->id);
    project->tickAutosave(interval + 2.0f, false);
    project->tickAutosave(interval + 2.0f, false);
    AppSettings::setAutosaveEnabled(autosave);
    const fs::path sceneBackup = SceneBackups::backupPath(project->getProjectPath(), BackupKind::Scene, scenePath);
    const fs::path volumeBackup = SceneBackups::backupPath(project->getProjectPath(), BackupKind::Voxel, volumeSource);
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!fs::exists(SceneBackups::sidecarPath(sceneBackup)) && std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(fs::exists(SceneBackups::sidecarPath(sceneBackup)));
    ASSERT_TRUE(fs::exists(SceneBackups::sidecarPath(volumeBackup)));
    // nothing of it reached the user's files, and no listing offers the volume on its own
    EXPECT_EQ(TestTempDir::read(assetFile()), savedVolume);
    for (const SceneBackupInfo& listed : project->listSceneBackups()) EXPECT_NE(listed.source, volumeSource);

    // a session that crashed: its pid is gone, and the file is as it was
    const int64_t pid = deadPid();
    setBackupPid(sceneBackup, pid);
    setBackupPid(volumeBackup, pid);
    system.reset();
    asset.reset();
    history->clear();
    const uint32_t sceneId = sceneProject->id;
    // a crash closes nothing: closing a modified scene would discard its backup (S03 R16)
    sceneProject->isModified = false;
    project->closeScene(sceneId, true);
    sceneProject = nullptr;

    // open_scene recovery: Restore brings both
    project->setNextRecoveryChoice(RecoveryChoice::Restore);
    ASSERT_TRUE(project->loadScene(project->getProjectPath() / scenePath, true, false, true));
    EXPECT_EQ(project->getLastRecovery().applied, RecoveryChoice::Restore);
    sceneProject = project->getScene(project->findSceneByPath(project->getProjectPath() / scenePath));
    ASSERT_NE(sceneProject, nullptr);
    ASSERT_EQ(sceneProject->restoredVoxelAssets.size(), 1u);
    asset = sceneProject->restoredVoxelAssets.front();
    EXPECT_TRUE(asset->dirty);
    EXPECT_EQ(hash(), edited);
    EXPECT_EQ(TestTempDir::read(assetFile()), savedVolume);
    // the scene's volume finds it
    EXPECT_EQ(VoxelAssets::acquire(assetPath), asset);

    // the save keeps it, and nothing is left to offer
    system = sceneProject->scene->getSystem<VoxelSystem>();
    system->update(0.0);
    ASSERT_TRUE(project->saveSceneToPath(sceneProject->id, sceneProject->filepath));
    EXPECT_FALSE(asset->dirty);
    EXPECT_NE(TestTempDir::read(assetFile()), savedVolume);
    EXPECT_FALSE(fs::exists(volumeBackup));
}

TEST_F(VoxelAssetTest, DiscardingRecoveryDiscardsTheVolumesCopy) {
    addVolume();
    const fs::path scenePath = sceneProject->filepath;
    const fs::path volumeSource = (project->getAssetsPath() / assetPath).lexically_relative(project->getProjectPath());
    ASSERT_TRUE(project->saveSceneToPath(sceneProject->id, sceneProject->filepath));
    const std::string sceneText = TestTempDir::read(project->getProjectPath() / scenePath);
    const std::vector<voxel::SlotChange> delta = edit(4);
    std::map<std::string, uint64_t> versions;
    const std::vector<VoxelBackup> backups = collectVoxelBackups(*project, *sceneProject, versions);
    ASSERT_EQ(backups.size(), 1u);
    writeVoxelBackups(project->getProjectPath(), backups);
    // a second look at the same version encodes nothing, and still names it
    const std::vector<VoxelBackup> again = collectVoxelBackups(*project, *sceneProject, versions);
    ASSERT_EQ(again.size(), 1u);
    EXPECT_TRUE(again[0].bytes.empty());
    const std::string marked = sceneText + voxelBackupMarkers(backups);
    ASSERT_TRUE(SceneBackups::write(project->getProjectPath(), BackupKind::Scene, scenePath, 0, marked, DocumentFiles::sha1(sceneText), 1));
    const int64_t pid = deadPid();
    const fs::path sceneBackup = SceneBackups::backupPath(project->getProjectPath(), BackupKind::Scene, scenePath);
    const fs::path volumeBackup = SceneBackups::backupPath(project->getProjectPath(), BackupKind::Voxel, volumeSource);
    setBackupPid(sceneBackup, pid);
    setBackupPid(volumeBackup, pid);

    system.reset();
    asset.reset();
    project->closeScene(sceneProject->id, true);
    project->setNextRecoveryChoice(RecoveryChoice::Discard);
    ASSERT_TRUE(project->loadScene(project->getProjectPath() / scenePath, true, false, true));
    EXPECT_FALSE(fs::exists(sceneBackup));
    EXPECT_FALSE(fs::exists(volumeBackup));
}
