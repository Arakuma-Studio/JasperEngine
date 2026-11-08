// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelArtAssign.h"

#include "Project.h"
#include "texture/Texture.h"
#include "texture/TextureImportSettings.h"
#include "pixelart/PixelArtInput.h"
#include "pixelart/PixelArtRules.h"
#include "util/EntityPath.h"

#include "component/MeshComponent.h"
#include "component/UIComponent.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;

AssignmentDefaults jasperengine::editor::pixelart::assignmentDefaults(const Project& project, const std::string& texturePath){
    AssignmentDefaults out;
    if (texturePath.empty())
        return out;
    const std::filesystem::path absolute = project.getAssetsPath() / std::filesystem::path(texturePath);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(absolute, ec))
        return out;
    TextureImportSettings settings;
    const bool stored = TextureImportSettings::load(absolute.string(), settings);
    // a texture not imported yet in Pixel Art Mode imports as pixel art (R1)
    if (!stored && project.getPixelArtSettings().enabled)
        settings.pixelArt = true;
    TextureFilter minFilter = TextureFilter::LINEAR, magFilter = TextureFilter::LINEAR;
    textureFilterModeFilters(settings.resolvedFilterMode(project.getDefaultFilterMode()), minFilter, magFilter);
    out.known = true;
    out.minFilter = (int)minFilter;
    out.magFilter = (int)magFilter;
    if (settings.pixelsPerUnit && *settings.pixelsPerUnit != project.getPixelsPerUnit())
        out.pixelsPerUnit = *settings.pixelsPerUnit;
    return out;
}

bool jasperengine::editor::pixelart::applyAssignmentFilters(const Project& project, Texture& texture){
    if (texture.empty() || texture.isFramebuffer() || texture.isCubeMap())
        return false;
    const AssignmentDefaults defaults = assignmentDefaults(project, texture.getPath());
    if (!defaults.known)
        return false;
    texture.setMinFilter((TextureFilter)defaults.minFilter);
    texture.setMagFilter((TextureFilter)defaults.magFilter);
    return true;
}

std::vector<FixChange> jasperengine::editor::pixelart::referenceFilterChanges(Project& project, const std::string& texturePath){
    std::vector<FixChange> out;
    const AssignmentDefaults defaults = assignmentDefaults(project, texturePath);
    if (!defaults.known)
        return out;
    const nlohmann::json to = {{"min_filter", rules::filterName((TextureFilter)defaults.minFilter)},
                               {"mag_filter", rules::filterName((TextureFilter)defaults.magFilter)}};
    for (SceneProject& scene : project.getScenes()){
        if (!scene.scene)
            continue;
        EntityPath::Cache paths;
        const std::string scenePath = scenePathOf(project, scene);
        for (Entity entity : scene.entities){
            auto add = [&](const Texture& texture, const char* field){
                if (texture.getPath() != texturePath)
                    return;
                if ((int)texture.getMinFilter() == defaults.minFilter && (int)texture.getMagFilter() == defaults.magFilter)
                    return;
                std::string path = EntityPath::of(scene, entity, &paths);
                if (path.empty() || path[0] != '/')
                    path = "/" + path;
                out.push_back({scenePath + "#" + path, field,
                               {{"min_filter", rules::filterName(texture.getMinFilter())}, {"mag_filter", rules::filterName(texture.getMagFilter())}}, to});
            };
            if (const MeshComponent* mesh = scene.scene->findComponent<MeshComponent>(entity); mesh && mesh->numSubmeshes > 0)
                add(mesh->submeshes[0].material.baseColorTexture, "mesh.submeshes[0].material.baseColorTexture");
            if (const UIComponent* ui = scene.scene->findComponent<UIComponent>(entity))
                add(ui->texture, "ui.texture");
        }
    }
    return out;
}
