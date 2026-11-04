// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Only a person's interactive editor records recent projects and the last project: the command
// line, agents' editors and the unit tests run on the default EditorHost, which leaves the user's
// settings alone (docs/superpowers/specs/2026-11-04-build-toolchain-design.md 6.1)

#include "AppSettings.h"
#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

namespace {

    // the person's editor: records what it opens and saves
    class InteractiveHost : public EditorHost {
    public:
        bool recordsRecentProjects() const override { return true; }
    };

    struct HostGuard {
        explicit HostGuard(EditorHost* host) { setEditorHost(host); }
        ~HostGuard() { setEditorHost(nullptr); }
    };

    void release(Project& project) {
        for (SceneProject& scene : project.getScenes()) CommandHandle::remove(scene.id);
        project.releaseScenes();
    }

    std::vector<std::string> recentPaths(const YAML::Node& root) {
        std::vector<std::string> paths;
        for (const YAML::Node& entry : root["recent_projects"]) {
            paths.push_back(entry.IsMap() ? entry["path"].as<std::string>() : entry.as<std::string>());
        }
        return paths;
    }

    bool same(const fs::path& a, const fs::path& b) {
        std::error_code ec;
        return !a.empty() && fs::equivalent(a, b, ec);
    }

    bool lists(const std::vector<std::string>& paths, const fs::path& project) {
        return std::any_of(paths.begin(), paths.end(), [&](const std::string& p) { return same(p, project); });
    }

    // a copy of a fixture project and the user's settings naming another project as the last one
    struct Setup {
        TestTempDir dir{"recent-projects"};
        fs::path settings = dir / "settings.yaml";
        fs::path other = dir / "Other";
        fs::path project = dir / "Game";
        Setup() {
            fs::create_directories(other);
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", project, fs::copy_options::recursive);
            TestTempDir::write(settings, "last_project_path: " + other.generic_string() + "\nrecent_projects:\n  - " +
                                         other.generic_string() + "\n");
            EXPECT_TRUE(AppSettings::initializeWithFile(settings));
        }
    };

}

TEST(RecentProjects, ToolProcessesLeaveSettingsAlone) {
    HostGuard host(nullptr);
    Setup setup;
    ASSERT_FALSE(getEditorHost().recordsRecentProjects());
    {
        Project project;
        project.setSkipProjectLock(true);
        // as a CLI export does: no record on load, then the export saves the project
        ASSERT_TRUE(project.loadProject(setup.project, true));
        ASSERT_TRUE(project.saveProject());
        release(project);
    }
    fs::path temp;
    {
        // the fallback of a failed `mcp --project` launch
        Project project;
        project.setSkipProjectLock(true);
        ASSERT_TRUE(project.createTempProject("RecentProjectsTemp", true));
        temp = project.getProjectPath();
        release(project);
    }
    std::error_code ec;
    fs::remove_all(temp, ec);

    const YAML::Node root = YAML::LoadFile(setup.settings.string());
    EXPECT_TRUE(same(root["last_project_path"].as<std::string>(""), setup.other)) << YAML::Dump(root);
    EXPECT_FALSE(lists(recentPaths(root), setup.project)) << YAML::Dump(root);
    EXPECT_TRUE(lists(recentPaths(root), setup.other)) << YAML::Dump(root);
    EXPECT_TRUE(same(AppSettings::getLastProjectPath(), setup.other));
}

TEST(RecentProjects, InteractiveEditorRecords) {
    InteractiveHost interactive;
    HostGuard host(&interactive);
    Setup setup;
    {
        Project project;
        project.setSkipProjectLock(true);
        ASSERT_TRUE(project.loadProject(setup.project, false));
        ASSERT_TRUE(project.saveProject());
        release(project);
    }
    const YAML::Node root = YAML::LoadFile(setup.settings.string());
    EXPECT_TRUE(same(root["last_project_path"].as<std::string>(""), setup.project)) << YAML::Dump(root);
    EXPECT_TRUE(lists(recentPaths(root), setup.project)) << YAML::Dump(root);
}
