// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A project's fork shaders give their manifests through the reader the project installs in
// ShaderManifests, which captures the project: a closed project takes it back, so a later read
// (an export's variant keys after a test's project went) never calls into a freed Project.

#include <gtest/gtest.h>

#include "Project.h"
#include "shader/ShaderManifest.h"

#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

TEST(ProjectForkManifest, AClosedProjectNoLongerReadsItsForks) {
    editor::test::ensureEngine();
    jasperengine::test::TestTempDir dir{"fork-manifest"};
    const std::string fork = "shaders/projectOnlyFork";
    {
        Project project;
        std::string error;
        ASSERT_TRUE(project.createProjectAt(dir.path() / "Forks", "Forks", &error)) << error;
        fs::create_directories(dir.path() / "Forks" / "shaders");
        std::ofstream(dir.path() / "Forks" / "shaders" / "projectOnlyFork.frag") << "void main() {}\n";
        ShaderManifests::clearForks();
        ASSERT_NE(ShaderManifests::get(fork), nullptr) << "the open project reads its fork";
    }
    // the file is still there: only the closed project's reader could find it
    ShaderManifests::clearForks();
    EXPECT_EQ(ShaderManifests::get(fork), nullptr) << "a closed project's forks are still read through it";
}
