// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S53 R5, R18: a model that requires an extension the engine cannot read reads as failed
// (its cook status, no measured numbers), and shows the loader's finding once, though the cook's
// record names the file by its asset path and the last load by its absolute path.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include "model/ModelAudit.h"
#include "model/ModelCookQueue.h"

#include "json.hpp"

#include <algorithm>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

TEST(ModelAudit, AnUnreadRequiredExtensionFailsAndShowsOneFinding){
    jasperengine::test::TestTempDir dir{"model-audit-unread"};
    fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/textures", dir.path(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    const nlohmann::ordered_json gltf = {{"asset", {{"version", "2.0"}}}, {"extensionsUsed", {"VENDOR_required_test"}},
            {"extensionsRequired", {"VENDOR_required_test"}}, {"meshes", nlohmann::ordered_json::array()}};
    const fs::path file = dir.path() / "assets" / "models" / "unread.gltf";
    std::ofstream(file) << gltf.dump();
    auto project = std::make_unique<Project>();
    project->setSkipProjectLock(true);
    ASSERT_TRUE(project->loadProject(dir.path(), false));
    project->getAssetIndex().waitUntilReady();
    ModelCookQueue& queue = ModelCookQueue::get();
    queue.attach(project.get());
    const std::string assetPath = "models/unread.gltf";
    ASSERT_TRUE(queue.waitFor({assetPath}, {ModelFamily::Desktop}, 60.0));

    // what a load of the source reports, by its absolute path
    GltfDiagnostic loaded;
    loaded.severity = GltfDiagnosticSeverity::Error;
    loaded.extension = "VENDOR_required_test";
    loaded.message = "glTF " + file.generic_string() + " requires VENDOR_required_test, which the engine does not read; it is loaded without it";
    loaded.fix = "export the model again without VENDOR_required_test";
    ModelLoadDiagnostics::set(file.generic_string(), {loaded});

    const ModelAuditRow row = ModelAudit::row(*project, assetPath, ModelFamily::Desktop);
    EXPECT_EQ(row.status(), "failed");
    ASSERT_TRUE(row.hasState);
    EXPECT_EQ(row.state.status, ModelCookStatus::Failed);
    EXPECT_EQ(row.diagnostics.size(), 1u) << "the cook's record and the last load name one finding";
    const long long errors = std::count_if(row.issues.begin(), row.issues.end(), [](const ModelIssue& issue){ return issue.severity == "error"; });
    EXPECT_EQ(errors, 1);
    ASSERT_FALSE(row.issues.empty());
    EXPECT_EQ(row.issues[0].code, "gltf-required-extension");
    const nlohmann::json json = nlohmann::json::parse(ModelAudit::toJson(row, true).dump());
    EXPECT_EQ(json["status"], "failed");
    EXPECT_EQ(json["cook"]["status"], "failed");
    EXPECT_FALSE(json["cook"].contains("record"));

    ModelLoadDiagnostics::clear(file.generic_string());
    queue.attach(nullptr);
    closeProject(*project);
}
