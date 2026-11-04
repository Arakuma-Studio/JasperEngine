// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavigationFindings.h"

#include "Out.h"
#include "Project.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "navigation/NavigationEditor.h"
#include "subsystem/NavigationSystem.h"

#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

std::vector<ProjectNavFinding> editor::collectProjectFindings(Project* project, size_t* surfaces, const ProjectFindingOptions& options) {
    std::vector<ProjectNavFinding> out;
    size_t count = 0;
    // agent_type_unused: the types every scene with navigation reports unused, once
    std::map<double, ProjectNavFinding> unused;
    std::set<double> used;
    if (project) {
        for (SceneProject& sceneProject : project->getScenes()) {
            if (!sceneProject.scene || sceneProject.filepath.empty())
                continue;
            NavigationSystem* system = NavigationEditor::systemOf(sceneProject);
            if (!system)
                continue;
            auto array = sceneProject.scene->getComponentArray<NavMeshSurfaceComponent>();
            for (size_t i = 0; i < array->size(); i++) {
                if (array->getComponentFromIndex(i).enabled)
                    count++;
            }
            nav::NavFindingOptions sceneOptions;
            sceneOptions.stale = options.stale;
            if (options.dirtyTilesOf) {
                const uint32_t sceneId = sceneProject.id;
                sceneOptions.dirtyTilesOf = [&options, sceneId](Entity surface) { return options.dirtyTilesOf(sceneId, surface); };
            }
            const bool navigates = array->size() > 0 || sceneProject.scene->getComponentArray<NavMeshAgentComponent>()->size() > 0;
            std::set<double> unusedHere;
            for (nav::NavFinding& finding : system->getFindings(sceneOptions)) {
                ProjectNavFinding item;
                item.scenePath = sceneProject.filepath.generic_string();
                item.sceneName = sceneProject.name;
                if (finding.code == "agent_type_unused") {
                    for (const auto& detail : finding.details) {
                        if (detail.first == "agent_type") {
                            unusedHere.insert(detail.second);
                            item.finding = std::move(finding);
                            unused.emplace(detail.second, std::move(item));
                            break;
                        }
                    }
                    continue;
                }
                item.sceneId = sceneProject.id;
                item.entityName = finding.entity != NULL_ENTITY ? sceneProject.scene->getEntityName(finding.entity) : std::string();
                item.finding = std::move(finding);
                out.push_back(std::move(item));
            }
            if (navigates) {
                for (const nav::AgentTypeSettings& type : nav::getSettings().agentTypes) {
                    if (!unusedHere.count((double)type.id))
                        used.insert((double)type.id);
                }
            }
        }
    }
    for (auto& entry : unused) {
        if (used.count(entry.first))
            continue;
        entry.second.scenePath.clear();
        entry.second.sceneName.clear();
        out.push_back(std::move(entry.second));
    }
    if (surfaces)
        *surfaces = count;
    return out;
}

void editor::logNavigationPreflight(Project* project) {
    for (const ProjectNavFinding& item : collectProjectFindings(project)) {
        if (item.finding.severity == nav::FindingSeverity::Info)
            continue;
        const std::string line = item.finding.message + " (scene " + item.sceneName + "). Window > AI > Navigation shows the problems.";
        if (item.finding.severity == nav::FindingSeverity::Error)
            Out::error("%s", line.c_str());
        else
            Out::warning("%s", line.c_str());
    }
}
