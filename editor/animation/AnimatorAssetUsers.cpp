// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorAssetUsers.h"

#include "Project.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimationClipIO.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AnimatorComponent.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"

#include <functional>
#include "spritenormals/SpriteNormalsField.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    bool isInside(const fs::path& root, const fs::path& absolute, fs::path* relative = nullptr) {
        const fs::path rel = absolute.lexically_normal().lexically_relative(root.lexically_normal());
        if (rel.empty() || *rel.begin() == "..") return false;
        if (relative) *relative = rel;
        return true;
    }

    // a path in any accepted form (absolute, project-relative, asset-relative) to an absolute one
    // inside the project; empty when outside
    fs::path absoluteOf(Project* project, const std::string& path) {
        if (!project || project->getProjectPath().empty() || path.empty()) return fs::path();
        const fs::path root = project->getProjectPath().lexically_normal();
        const fs::path given = FileUtils::pathFromUtf8(path);
        fs::path absolute;
        if (given.is_absolute()) {
            absolute = given.lexically_normal();
        } else {
            absolute = (root / given).lexically_normal();
            std::error_code ec;
            if (!fs::exists(absolute, ec)) {
                const fs::path assetForm = project->resolveAssetPath(given);
                if (!assetForm.empty() && fs::exists(assetForm, ec)) absolute = assetForm.lexically_normal();
            }
        }
        return isInside(root, absolute) ? absolute : fs::path();
    }

    fs::path storedFile(Project* project, const std::string& stored) {
        return stored.empty() ? fs::path() : project->resolveAssetPath(FileUtils::pathFromUtf8(stored)).lexically_normal();
    }

    // the GUID and the absolute file of an asset given by path or GUID
    void identify(Project* project, const std::string& asset, std::string& guid, fs::path& file) {
        if (anim::isClipGuid(asset)) {
            guid = asset;
            const fs::path relative = project->getAssetDatabase().getPath(guid);
            if (!relative.empty()) file = (project->getProjectPath() / relative).lexically_normal();
            return;
        }
        file = absoluteOf(project, asset);
        if (!file.empty()) guid = project->getAssetDatabase().getGuid(file);
    }

    template<class Component>
    void forEachComponent(Project* project, const std::function<void(AnimatorAssetUser, Component&)>& visit) {
        auto visitRegistry = [&](EntityRegistry* registry, AnimatorAssetUser site) {
            if (!registry) return;
            auto components = registry->getComponentArray<Component>();
            for (size_t i = 0; i < components->size(); i++) {
                site.entity = components->getEntity(i);
                visit(site, components->getComponentFromIndex(i));
            }
        };
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            AnimatorAssetUser site;
            site.sceneId = sceneProject->id;
            visitRegistry(sceneProject->scene, site);
        }
        project->forEachPrefabAsset([&](const fs::path& path, Prefab& prefab) {
            AnimatorAssetUser site;
            site.prefabAsset = true;
            site.prefabPath = path.generic_string();
            visitRegistry(prefab.registry.get(), site);
        });
    }

}

std::vector<AnimatorAssetUser> jasperengine::editor::findMaskUsers(Project* project, const std::string& asset) {
    std::vector<AnimatorAssetUser> users;
    if (!project || project->getProjectPath().empty() || asset.empty()) return users;
    std::string guid;
    fs::path file;
    identify(project, asset, guid, file);
    auto matches = [&](const std::string& path, const std::string& refGuid) {
        return (!guid.empty() && refGuid == guid) || (!path.empty() && !file.empty() && storedFile(project, path) == file);
    };
    forEachComponent<AnimationPlayerComponent>(project, [&](AnimatorAssetUser site, AnimationPlayerComponent& player) {
        for (size_t i = 0; i < player.layers.size(); i++) {
            if (!matches(player.layers[i].mask, player.layers[i].maskGuid)) continue;
            site.layer = (int)i;
            users.push_back(site);
        }
    });
    // the layers of the project's controllers
    const fs::path root = project->getProjectPath().lexically_normal();
    for (const fs::path& controllerFile : ProjectChecks::findProjectFiles(root, {".controller"})) {
        std::string text;
        if (!ProjectChecks::readFile(controllerFile, text)) continue;
        const anim::ControllerParseResult parsed = anim::parseController(text);
        if (!parsed.ok) continue;
        fs::path relative;
        isInside(root, controllerFile, &relative);
        for (size_t l = 0; l < parsed.data.layers.size(); l++) {
            const anim::AnimatorAssetRef& mask = parsed.data.layers[l].mask;
            if (!matches(mask.path, mask.guid)) continue;
            AnimatorAssetUser user;
            user.layer = (int)l;
            user.controllerFile = relative.generic_string();
            users.push_back(user);
        }
    }
    return users;
}

std::vector<AnimatorAssetUser> jasperengine::editor::findControllerUsers(Project* project, const std::string& asset) {
    std::vector<AnimatorAssetUser> users;
    if (!project || project->getProjectPath().empty() || asset.empty()) return users;
    std::string guid;
    fs::path file;
    identify(project, asset, guid, file);
    forEachComponent<AnimatorComponent>(project, [&](AnimatorAssetUser site, AnimatorComponent& animator) {
        const std::string refGuid = animator.controller.guid.isNull() ? std::string() : animator.controller.guid.str();
        const bool match = (!guid.empty() && refGuid == guid) ||
                           (!animator.controller.path.empty() && !file.empty() && storedFile(project, animator.controller.path) == file);
        if (match) users.push_back(site);
    });
    return users;
}
