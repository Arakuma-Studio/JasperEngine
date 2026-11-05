// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S53 R12, R13 (AC11's export half): the export cooks every model for its families into
// assets/je-cooked/models/<hash>.glb with index.json (one file per identical cook, Read/Write's
// decision per family), leaves the cooked sources out unless Ship Source File is on, and a cook
// error fails the export naming the file.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "support/ModelFixtures.h"
#include "support/TestTempDir.h"

#include "command/type/SetAssetMetaCmd.h"
#include "model/CookedModels.h"
#include "model/GltfDocument.h"
#include "model/ModelExport.h"

#include "json.hpp"

#include <fstream>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    std::unique_ptr<Project> loadFixture(const fs::path& dir){
        fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/textures", dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        const std::vector<uint8_t> model = makeTestModel(8, false);
        std::ofstream(dir / "assets" / "models" / "robot.glb", std::ios::binary).write(reinterpret_cast<const char*>(model.data()), (std::streamsize)model.size());
        auto project = std::make_unique<Project>();
        project->setSkipProjectLock(true);
        if (!project->loadProject(dir, false))
            return nullptr;
        project->getAssetIndex().waitUntilReady();
        return project;
    }

}

TEST(ModelExport, CooksEveryModelIntoTheIndexAndLeavesTheSourcesOut){
    jasperengine::test::TestTempDir dir{"model-export"};
    std::unique_ptr<Project> project = loadFixture(dir.path());
    ASSERT_TRUE(project);
    // one model ships its source too
    SetAssetMetaCmd ship(project.get(), "assets/models/panel.gltf", "import.modelImporter", YAML::Load("{version: 1, shipSource: true}"));
    ASSERT_TRUE(ship.execute());
    ModelExportInputs inputs;
    std::string error;
    ASSERT_TRUE(ModelExport::gather(*project, {ModelFamily::Desktop, ModelFamily::Mobile}, inputs, error)) << error;
    EXPECT_EQ(inputs.items.size(), 4u) << "two models x two families";
    const fs::path out = dir.path() / "export" / "assets";
    const ModelExportResult result = ModelExport::cookForExport(*project, inputs, out, 2, nullptr, nullptr);
    ASSERT_TRUE(result.ok) << result.error;
    std::ifstream in(out / CookedModels::kIndexFile, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const nlohmann::json index = nlohmann::json::parse(text.str(), nullptr, false);
    ASSERT_TRUE(index.is_object()) << text.str();
    EXPECT_EQ(index["format"], CookedModels::kFormatVersion);
    for (const char* model : {"models/robot.glb", "models/panel.gltf"}){
        ASSERT_TRUE(index["models"].contains(model)) << model;
        for (const char* family : {"desktop", "mobile"}){
            const nlohmann::json& entry = index["models"][model][family];
            const fs::path file = out / CookedModels::kCookedDir / entry["file"].get<std::string>();
            EXPECT_TRUE(fs::is_regular_file(file)) << file;
            EXPECT_EQ(entry["bytes"].get<uint64_t>(), (uint64_t)fs::file_size(file));
            // nothing in the fixture reads the panel on the CPU; the robot is skinned
            EXPECT_EQ(entry["keepCpuData"].get<bool>(), std::string(model) == "models/robot.glb") << model << " " << family;
        }
    }
    // identical settings for both families: one file
    EXPECT_EQ(index["models"]["models/robot.glb"]["desktop"]["file"], index["models"]["models/robot.glb"]["mobile"]["file"]);
    EXPECT_TRUE(result.cookedSources.count("models/robot.glb"));
    EXPECT_FALSE(result.cookedSources.count("models/panel.gltf")) << "Ship Source File keeps it";
    closeProject(*project);
}

TEST(ModelExport, ALoadErrorFailsTheExportNamingTheFile){
    jasperengine::test::TestTempDir dir{"model-export-error"};
    std::unique_ptr<Project> project = loadFixture(dir.path());
    ASSERT_TRUE(project);
    // a model that requires an extension the engine does not read (S53 R5): the game would load
    // it without that extension, so it does not cook and the export stops
    const nlohmann::ordered_json gltf = {{"asset", {{"version", "2.0"}}}, {"extensionsUsed", {"VENDOR_required_test"}},
            {"extensionsRequired", {"VENDOR_required_test"}}, {"meshes", nlohmann::ordered_json::array()}};
    std::ofstream(dir.path() / "assets" / "models" / "unread.gltf") << gltf.dump();
    project->refreshAssetDatabase(true);
    project->getAssetIndex().waitUntilReady();
    ModelExportInputs inputs;
    std::string error;
    ASSERT_TRUE(ModelExport::gather(*project, {ModelFamily::Desktop}, inputs, error)) << error;
    const ModelExportResult result = ModelExport::cookForExport(*project, inputs, dir.path() / "export" / "assets", 1, nullptr, nullptr);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("models/unread.gltf"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("VENDOR_required_test"), std::string::npos) << result.error;
    closeProject(*project);
}

TEST(ModelExport, ADracoModelShipsItsCookWithoutDraco){
    jasperengine::test::TestTempDir dir{"model-export-draco"};
    std::unique_ptr<Project> project = loadFixture(dir.path());
    ASSERT_TRUE(project);
    fs::copy_file(fs::path(JASPER_TEST_REPO_DIR) / "engine/tests/core/fixtures/gltf/s41/draco.glb", dir.path() / "assets" / "models" / "draco.glb");
    project->refreshAssetDatabase(true);
    project->getAssetIndex().waitUntilReady();
    ModelExportInputs inputs;
    std::string error;
    ASSERT_TRUE(ModelExport::gather(*project, {ModelFamily::Web}, inputs, error)) << error;
    const fs::path out = dir.path() / "export" / "assets";
    const ModelExportResult result = ModelExport::cookForExport(*project, inputs, out, 2, nullptr, nullptr);
    ASSERT_TRUE(result.ok) << result.error;
    std::ifstream in(out / CookedModels::kIndexFile, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const nlohmann::json index = nlohmann::json::parse(text.str(), nullptr, false);
    ASSERT_TRUE(index.is_object() && index["models"].contains("models/draco.glb")) << text.str();
    const fs::path cooked = out / CookedModels::kCookedDir / index["models"]["models/draco.glb"]["web"]["file"].get<std::string>();
    GltfDocument doc;
    ASSERT_TRUE(doc.load(cooked, error)) << error;
    for (const std::string& extension : doc.usedExtensions())
        EXPECT_NE(extension, "KHR_draco_mesh_compression");
    EXPECT_TRUE(result.cookedSources.count("models/draco.glb")) << "the Draco source stays out of the build";
    closeProject(*project);
}
