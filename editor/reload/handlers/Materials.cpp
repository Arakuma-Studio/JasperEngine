// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// .material files (S05 R11): read, parsed and migrated on a worker, then applied on the main
// thread to every submesh linked to the file (Project::applyLinkedMaterialFile). The textures
// the material names load through the texture rule.

#include "ReloadHandlers.h"

#include "DocumentFile.h"
#include "Project.h"
#include "Stream.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadMaterials(Context& ctx, const std::vector<fs::path>& filesIn) {
    std::vector<fs::path> files = filesIn;
    if (ctx.options.full) {
        // a full sync: every linked file
        for (const std::string& linked : ctx.project.getLinkedMaterialFiles()) {
            const fs::path path = (ctx.project.getProjectPath() / linked).lexically_normal();
            if (std::find(files.begin(), files.end(), path) == files.end()) files.push_back(path);
        }
    }
    const std::vector<std::string> linked = ctx.project.getLinkedMaterialFiles();
    Project* project = &ctx.project;
    for (const fs::path& path : files) {
        const std::string relative = rel(ctx.project, path);
        if (std::find(linked.begin(), linked.end(), relative) == linked.end()) continue;   // linked nowhere
        ctx.service.startJob("material", path, [path, relative, project]() -> HotReloadService::AdoptFn {
            // a parse error throws "<file>:<line>:<column>: <message>"
            YAML::Node root = DocumentFiles::read(path, DocumentKind::Material).root;
            return [root, relative, project](std::string& error) {
                try {
                    project->applyLinkedMaterialFile(relative, Stream::decodeMaterial(root));
                } catch (const std::exception& e) {
                    error = e.what();
                    return false;
                }
                return true;
            };
        });
    }
}
