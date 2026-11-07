// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAVIGATION_FINDINGS_H
#define NAVIGATION_FINDINGS_H

// The project's navigation findings (docs/specs/S75-navigation-diagnostics.md R13, R36):
// NavigationSystem::getFindings over every loaded scene that has a file, with the scene added.
// The Navigation window's Problems list, get_navigation_info, the CLI check and the export
// preflight read them; nothing else makes navigation findings.

#include "ecs/Entity.h"
#include "navigation/NavDiagnostics.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct ProjectNavFinding {
        nav::NavFinding finding;
        std::string scenePath, sceneName, entityName;
        // the scene's project id (selecting the GameObject); 0 for a project-wide finding
        uint32_t sceneId = 0;
    };

    struct ProjectFindingOptions {
        // false: no stale_bake (no collection)
        bool stale = true;
        // a surface's dirty tiles from the caller's cache (the editor's); unset: collected now
        std::function<int(uint32_t sceneId, Entity surface)> dirtyTilesOf;
    };

    // every scene's findings, in scene order (each scene's sorted as getFindings sorts them);
    // agent_type_unused only once, for the types no scene uses, at the end. surfaces: the
    // enabled NavMesh Surfaces looked at
    std::vector<ProjectNavFinding> collectProjectFindings(Project* project, size_t* surfaces = nullptr,
                                                          const ProjectFindingOptions& options = {});

    // the export preflight's lines: warnings and errors (never info), each naming its scene and
    // "Window > AI > Navigation"; it never blocks the export
    void logNavigationPreflight(Project* project);

}

#endif // NAVIGATION_FINDINGS_H
