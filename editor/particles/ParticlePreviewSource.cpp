// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticlePreviewSource.h"

#include "Project.h"
#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleModules.h"
#include "util/EntityPath.h"
#include "util/ParticleEffectEdit.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

bool ParticlePreviewSource::fromAsset(const std::string& assetPath, std::vector<ParticlePreviewSystem>& out, std::string& error){
    ParticlePreviewSystem system;
    std::vector<particles::Diagnostic> diagnostics;
    if (!ParticleEffectEdit::readAsset(assetPath, system.document, &diagnostics)){
        error = assetPath + " cannot be read: " + particles::ParticleEffectCodec::format(diagnostics);
        return false;
    }
    system.name = assetPath;
    system.asset = assetPath;
    out.push_back(std::move(system));
    return true;
}

std::vector<Entity> ParticlePreviewSource::subtreeSystems(Scene* scene, Entity root){
    std::vector<std::pair<std::string, Entity>> found;
    auto systems = scene->getComponentArray<ParticleSystemComponent>();
    for (size_t i = 0; i < systems->size(); i++){
        const Entity entity = systems->getEntity(i);
        // a GameObject that does not play is not part of the effect
        if (!scene->isEntityActiveInHierarchy(entity)) continue;
        std::string relative;
        if (!EntityPath::relative(scene, root, entity, relative)){
            // a name on the way that a path cannot hold: under the root all the same?
            Entity walk = entity;
            while (walk != NULL_ENTITY && walk != root){
                const Transform* transform = scene->findComponent<Transform>(walk);
                walk = transform ? transform->parent : NULL_ENTITY;
            }
            if (walk != root) continue;
            relative = "#" + std::to_string(entity);
        }
        found.push_back({relative, entity});
    }
    std::sort(found.begin(), found.end());
    std::vector<Entity> out;
    for (const auto& item : found) out.push_back(item.second);
    return out;
}

bool ParticlePreviewSource::fromSubtree(Scene* scene, Entity root, std::vector<ParticlePreviewSystem>& out, std::string& error){
    const std::vector<Entity> entities = subtreeSystems(scene, root);
    if (entities.empty()){
        error = "\"" + scene->getEntityName(root) + "\" has no Particle System in its subtree (create_particles adds one).";
        return false;
    }
    const Transform* rootTransform = scene->findComponent<Transform>(root);
    const Matrix4 toRoot = rootTransform ? rootTransform->modelMatrix.inverse() : Matrix4();
    const std::string rootName = scene->getEntityName(root);
    for (Entity entity : entities){
        const ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);
        ParticlePreviewSystem system;
        std::vector<particles::Diagnostic> diagnostics;
        if (!ParticleEffectEdit::read(particles, system.document, &diagnostics)){
            error = "the effect of \"" + scene->getEntityName(entity) + "\" cannot be read: " + particles::ParticleEffectCodec::format(diagnostics);
            return false;
        }
        for (const ParticleExposedValue& value : particles.exposedValues){
            particles::ParticleModules::setExposed(system.document, value.name, value.value);
        }
        std::string relative;
        system.name = EntityPath::relative(scene, root, entity, relative) && !relative.empty() ? rootName + "/" + relative : rootName;
        if (!particles.effect.empty()) system.asset = particles.effect.path;
        const Transform* transform = scene->findComponent<Transform>(entity);
        system.transform = transform ? toRoot * transform->modelMatrix : Matrix4();
        system.seed = particles.seed;
        out.push_back(std::move(system));
    }
    return true;
}

bool ParticlePreviewSource::applyArgs(const nlohmann::json& args, ParticlePreviewRequest& request, std::string& error){
    if (args.contains("seed")){
        if (!args["seed"].is_number_integer() || args["seed"].get<int64_t>() < 0 || args["seed"].get<int64_t>() > 0x7fffffff){
            error = "seed is an integer from 0 (each system's own) to 2147483647.";
            return false;
        }
        request.seed = (uint32_t)args["seed"].get<int64_t>();
    }
    if (args.contains("frames") && !args["frames"].is_number_integer()){
        error = "frames is an integer from 0 to " + std::to_string(ParticleEffectPreview::MAX_FRAMES) + ".";
        return false;
    }
    request.frames = args.value("frames", 6);
    if (args.contains("times")){
        if (!args["times"].is_array()){
            error = "times is a list of seconds, e.g. [0, 0.1, 0.5].";
            return false;
        }
        for (const nlohmann::json& t : args["times"]){
            if (!t.is_number()){
                error = "times holds numbers (seconds).";
                return false;
            }
            request.times.push_back(t.get<double>());
        }
    }
    if (args.contains("seconds") && !args["seconds"].is_number()){
        error = "seconds is a number: 0 for one play, up to " + std::to_string((int)ParticleEffectPreview::MAX_SECONDS) + ".";
        return false;
    }
    request.seconds = args.value("seconds", 0.0);
    const auto text = [&](const char* key, const std::string& fallback, std::initializer_list<const char*> options, std::string& out){
        out = args.contains(key) && args[key].is_string() ? args[key].get<std::string>() : fallback;
        for (const char* option : options){
            if (out == option) return true;
        }
        error = std::string(key) + " is";
        size_t k = 0;
        for (const char* option : options) error += std::string(k++ == 0 ? " " : k == options.size() ? " or " : ", ") + option;
        error += ".";
        return false;
    };
    if (!text("view", "three_quarter", {"three_quarter", "front", "side", "top"}, request.view)) return false;
    if (!text("background", "dark", {"dark", "light", "checker"}, request.background)) return false;
    if (!text("platform", "mobile", {"mobile", "web", "desktop"}, request.platform)) return false;
    request.overdrawMap = args.value("overdraw_map", false);
    return true;
}

bool ParticlePreviewSource::complete(Project* project, const nlohmann::json& args, const std::string& scope, ParticlePreviewRequest& request,
                                     std::string& error){
    if (!applyArgs(args, request, error)) return false;
    error = ParticleEffectPreview::validate(request);
    if (!error.empty()) return false;
    if (project) request.qa = project->getParticleQaSettings();
    request.rounds = rounds().previous(scope);
    return true;
}

std::string ParticlePreviewSource::subtreeScope(const SceneProject& sceneProject, Entity root){
    return "scene " + std::to_string(sceneProject.id) + ": " + EntityPath::of(sceneProject, root);
}

qa::FailRounds& ParticlePreviewSource::rounds(){
    static qa::FailRounds store;
    return store;
}
