// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Vertex colors in the model cook and the export (docs/specs/V1-vertex-colors.md R9, R92, D11): the
// painted models whose primitives keep their order in every family, the check that every shipped
// target still fits its mesh, and the shipped file without its editor-only section.

#include "VertexColorEditorInternal.h"
#include "VertexColorProject.h"

#include "Project.h"
#include "component/ModelComponent.h"
#include "component/VertexColorsComponent.h"
#include "model/ModelCookQueue.h"
#include "subsystem/VertexColorScene.h"
#include "util/FileUtils.h"
#include "vertexcolor/VertexColorFile.h"

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vertexcolor;
namespace vcd = jasperengine::editor::vertexcolor::detail;

std::set<std::string> editor::livePaintedModels(Project& project) {
    std::set<std::string> models;
    for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
        Scene* scene = sceneProject->scene;
        if (!scene) continue;
        auto array = scene->getComponentArray<VertexColorsComponent>();
        for (size_t i = 0; i < array->size(); i++) {
            if (const ModelComponent* model = scene->findComponent<ModelComponent>(array->getEntity(i)); model && !model->filename.path.empty()) {
                models.insert(model->filename.path);
            }
        }
    }
    return models;
}

void editor::paintedModelChanged(Project& project, const std::string& modelAssetPath) {
    // its cook key now has the painted set: the desktop cook the editor draws is made again in the
    // source's order, and every export family follows (D11)
    if (ModelCookQueue::get().getProject() == &project) ModelCookQueue::get().settingsChanged(modelAssetPath);
}

std::vector<std::string> editor::vertexColorExportProblems(Project& project) {
    std::vector<std::string> problems;
    for (SceneProject& sceneProject : project.getScenes()) {
        Scene* scene = sceneProject.scene;
        if (!scene) continue;
        auto array = scene->getComponentArray<VertexColorsComponent>();
        for (size_t i = 0; i < array->size(); i++) {
            const VertexColorsComponent& component = array->getComponentFromIndex(i);
            if (component.data.path.empty()) continue;
            const Entity entity = array->getEntity(i);
            const std::string where = "Vertex Colors of '" + scene->getEntityName(entity) + "' in scene '" + sceneProject.name + "'";
            // what ships is the file on disk (the scenes were saved before this check)
            vc::Data shipped;
            std::string error;
            if (!vc::readFile(vcd::absoluteAssetPath(project, component.data.path), shipped, error)) {
                problems.push_back(where + ": " + error + ". Fix or replace the file, or clear the component's Data.");
                continue;
            }
            for (const vc::TargetData& target : shipped.targets) {
                const std::shared_ptr<const vc::MeshView> mesh = vc::sceneMeshView(scene, entity, target.key);
                // a mesh not loaded is checked by the game, which draws such a target without its colors
                if (!mesh) continue;
                if (mesh->fingerprint().sameTopology(target.fingerprint)) continue;
                problems.push_back(where + ": " + target.key.describe() + " was painted on another version of its mesh (vertex_colors_unmatched). "
                                   "Open the scene and save it, which transfers the colors to the mesh as it is, or clear and repaint them.");
            }
        }
    }
    return problems;
}

bool editor::isVertexColorsFile(const fs::path& path) {
    return path.extension() == vc::VCOLORS_EXTENSION;
}

bool editor::cookVertexColorsFile(const fs::path& source, const fs::path& destination, std::string& error) {
    vc::Data data;
    if (!vc::readFile(source, data, error)) return false;
    // R92: the snapshot is the editor's (a reimport's transfer), no game reads it
    return vc::writeFile(destination, data, false, error);
}
