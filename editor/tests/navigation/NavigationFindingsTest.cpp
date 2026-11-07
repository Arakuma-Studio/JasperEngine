// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The project's navigation findings (docs/specs/S75-navigation-diagnostics.md R13, R36; AC10):
// every loaded scene with a file, in scene order, the scene named; agent types unused by every
// scene once; and the export preflight's lines.

#include "JasperEngine.h"
#include "Out.h"
#include "Project.h"
#include "navigation/NavigationFindings.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    Entity navSurface(Scene& scene, const char* name) {
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity);
        scene.setEntityName(entity, name);
        scene.addComponent<NavMeshSurfaceComponent>(entity, NavMeshSurfaceComponent());
        return entity;
    }

    void addNavScene(Project& project, Scene& scene, uint32_t id, const std::string& name, const std::string& file) {
        SceneProject sceneProject;
        sceneProject.id = id;
        sceneProject.name = name;
        sceneProject.scene = &scene;
        sceneProject.filepath = file;
        project.getScenes().push_back(std::move(sceneProject));
    }

}

TEST(NavigationFindings, ProjectWrapper){
    if (!Out::memorySinkId())
        Out::startLogging();

    // scene A: an unbaked Humanoid surface; scene B: an unbaked Giant surface and an agent of an
    // unknown type (an error); scene C has no file (not checked)
    Scene a, b, c;
    navSurface(a, "Ground A");
    const Entity giantSurface = navSurface(b, "Ground B");
    b.getComponent<NavMeshSurfaceComponent>(giantSurface).agentType = 1;
    const Entity stray = b.createEntity();
    b.addComponent<Transform>(stray);
    b.setEntityName(stray, "Stray");
    NavMeshAgentComponent agent;
    agent.agentType = 9;
    b.addComponent<NavMeshAgentComponent>(stray, agent);
    navSurface(c, "Ground C");

    // a Project applies its own navigation block: the test's types go in after it
    Project project;
    nav::NavigationSettings settings = nav::NavigationSettings::defaults();
    nav::AgentTypeSettings giant;
    giant.id = 1;
    giant.name = "Giant";
    nav::AgentTypeSettings spare;
    spare.id = 2;
    spare.name = "Spare";
    settings.agentTypes.push_back(giant);
    settings.agentTypes.push_back(spare);
    nav::setSettings(settings);
    addNavScene(project, a, 1, "A", "Scenes/A.scene");
    addNavScene(project, b, 2, "B", "Scenes/B.scene");
    addNavScene(project, c, 3, "C", "");

    size_t surfaces = 0;
    const std::vector<ProjectNavFinding> findings = collectProjectFindings(&project, &surfaces);
    EXPECT_EQ(surfaces, 2u);
    // A's, then B's (errors first), then the unused type once
    std::vector<std::string> order;
    for (const ProjectNavFinding& item : findings)
        order.push_back(item.sceneName + ":" + item.finding.code + ":" + item.entityName);
    const std::vector<std::string> expected = {"A:not_baked:Ground A", "B:agent_type_missing:Stray", "B:not_baked:Ground B",
                                               "B:agent_no_surface:Stray", ":agent_type_unused:"};
    EXPECT_EQ(order, expected);
    EXPECT_EQ(findings[0].scenePath, "Scenes/A.scene");
    EXPECT_EQ(findings[0].sceneId, 1u);
    EXPECT_EQ(findings[1].sceneId, 2u);
    EXPECT_EQ(findings.back().sceneId, 0u);
    EXPECT_NE(findings.back().finding.message.find("'Spare'"), std::string::npos) << findings.back().finding.message;

    // the export preflight: warnings and errors with their scene, never the info
    const uint64_t cursor = Out::lastLogId();
    logNavigationPreflight(&project);
    std::vector<editor::LogEntry> logs = Out::getLogsSince(cursor);
    logs.erase(std::remove_if(logs.begin(), logs.end(), [](const editor::LogEntry& e) { return e.category != "editor"; }), logs.end());
    ASSERT_EQ(logs.size(), 4u);
    EXPECT_NE(logs[0].message.find("(scene A). Window > AI > Navigation"), std::string::npos) << logs[0].message;
    EXPECT_EQ(logs[1].type, LogType::Error);
    EXPECT_NE(logs[1].message.find("'Stray'"), std::string::npos) << logs[1].message;
    EXPECT_EQ(logs[2].type, LogType::Warning);
    for (const editor::LogEntry& entry : logs)
        EXPECT_EQ(entry.message.find("Spare"), std::string::npos);

    project.getScenes().clear();
    nav::setSettings(nav::NavigationSettings::defaults());
}
