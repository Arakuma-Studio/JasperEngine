// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "ShaderGlobalsUsage.h"

#include "Project.h"
#include "Scene.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/PointsComponent.h"
#include "component/SkyboxComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/UIComponent.h"
#include "subsystem/RenderSystem.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    using Owner = ShaderGlobalsUsage::Owner;

    const char* ownerName(Owner owner) {
        switch (owner) {
            case Owner::Material: return "material";
            case Owner::UI: return "UI";
            case Owner::Points: return "Points";
            case Owner::Lines: return "Line Renderer";
            case Owner::Sky: return "Skybox";
            case Owner::Trail: return "Trail Renderer";
            case Owner::PostProcess: return "post-process pass";
        }
        return "";
    }

    std::string whereOf(Owner owner, int index) {
        if (owner == Owner::Material) return "submesh " + std::to_string(index) + " material";
        if (owner == Owner::PostProcess) return "post-process pass " + std::to_string(index);
        return ownerName(owner);
    }

    // the shader a component draws with (its own, else the scene's default of its type)
    template<typename T>
    std::string componentShader(Scene* scene, const T& component, ShaderType type) {
        return component.customShader.empty() ? scene->getDefaultCustomShader(type) : component.customShader;
    }

    // what a loaded program reads of the globals (its layouts' members, its texture slots)
    void addShaderUses(std::vector<ShaderGlobalsUsage::Use>& uses, const ShaderGlobalsUsage::Use& base, const ShaderLibraryBindings& bindings,
                       const std::string& global) {
        auto add = [&](const std::string& member, const char* stage) {
            if (!global.empty() && member != global) return;
            for (const ShaderGlobalsUsage::Use& use : uses) {
                if (!use.binding && use.member == member && use.shader == base.shader && use.stage == stage && use.sceneId == base.sceneId &&
                        use.entity == base.entity && use.where == base.where)
                    return;
            }
            ShaderGlobalsUsage::Use use = base;
            use.global = member;
            use.member = member;
            use.stage = stage;
            uses.push_back(use);
        };
        if (bindings.vsGlobals) {
            for (const GlobalsBlockLayout::Member& member : bindings.vsGlobals->members) add(member.name, "vertex");
        }
        if (bindings.fsGlobals) {
            for (const GlobalsBlockLayout::Member& member : bindings.fsGlobals->members) add(member.name, "fragment");
        }
        for (const GlobalTextureSlot& texture : bindings.globalTextures) add(ShaderGlobals::nameOf(texture.globalId), "texture");
    }

}

void ShaderGlobalsUsage::forEachBindings(Project* project, const std::function<void(const BindingsRef&, const ShaderGlobalBindings&)>& visit) {
    for (SceneProject& sceneProject : project->getScenes()) {
        Scene* scene = sceneProject.scene;
        if (!scene) continue;
        auto meshes = scene->getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++) {
            const MeshComponent& mesh = meshes->getComponentFromIndex(i);
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
                if (!mesh.submeshes[s].material.shaderGlobalBindings.empty())
                    visit({sceneProject.id, meshes->getEntity(i), Owner::Material, (int)s}, mesh.submeshes[s].material.shaderGlobalBindings);
            }
        }
        auto each = [&](auto* array, Owner owner) {
            for (size_t i = 0; i < array->size(); i++) {
                const auto& component = array->getComponentFromIndex(i);
                if (!component.shaderGlobalBindings.empty())
                    visit({sceneProject.id, array->getEntity(i), owner, 0}, component.shaderGlobalBindings);
            }
        };
        each(scene->getComponentArray<UIComponent>(), Owner::UI);
        each(scene->getComponentArray<PointsComponent>(), Owner::Points);
        each(scene->getComponentArray<LineRendererComponent>(), Owner::Lines);
        each(scene->getComponentArray<SkyboxComponent>(), Owner::Sky);
        each(scene->getComponentArray<TrailRendererComponent>(), Owner::Trail);
        const std::vector<PostProcessPass>& passes = scene->getPostProcessPasses();
        for (size_t p = 0; p < passes.size(); p++) {
            if (!passes[p].shaderGlobalBindings.empty())
                visit({sceneProject.id, NULL_ENTITY, Owner::PostProcess, (int)p}, passes[p].shaderGlobalBindings);
        }
    }
}

ShaderGlobalBindings* ShaderGlobalsUsage::bindingsOf(Project* project, const BindingsRef& ref) {
    SceneProject* sceneProject = project->getScene(ref.sceneId);
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    if (!scene) return nullptr;
    switch (ref.owner) {
        case Owner::Material: {
            MeshComponent* mesh = scene->findComponent<MeshComponent>(ref.entity);
            if (!mesh || ref.index < 0 || ref.index >= (int)mesh->numSubmeshes) return nullptr;
            return &mesh->submeshes[ref.index].material.shaderGlobalBindings;
        }
        case Owner::UI: {
            UIComponent* ui = scene->findComponent<UIComponent>(ref.entity);
            return ui ? &ui->shaderGlobalBindings : nullptr;
        }
        case Owner::Points: {
            PointsComponent* points = scene->findComponent<PointsComponent>(ref.entity);
            return points ? &points->shaderGlobalBindings : nullptr;
        }
        case Owner::Lines: {
            LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(ref.entity);
            return lines ? &lines->shaderGlobalBindings : nullptr;
        }
        case Owner::Sky: {
            SkyboxComponent* sky = scene->findComponent<SkyboxComponent>(ref.entity);
            return sky ? &sky->shaderGlobalBindings : nullptr;
        }
        case Owner::Trail: {
            TrailRendererComponent* trail = scene->findComponent<TrailRendererComponent>(ref.entity);
            return trail ? &trail->shaderGlobalBindings : nullptr;
        }
        case Owner::PostProcess:
            return nullptr; // a scene setting: setBindings goes through Scene::setPostProcessPasses
    }
    return nullptr;
}

bool ShaderGlobalsUsage::setBindings(Project* project, const BindingsRef& ref, const ShaderGlobalBindings& bindings) {
    SceneProject* sceneProject = project->getScene(ref.sceneId);
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    if (!scene) return false;
    if (ref.owner == Owner::PostProcess) {
        std::vector<PostProcessPass> passes = scene->getPostProcessPasses();
        if (ref.index < 0 || ref.index >= (int)passes.size()) return false;
        passes[ref.index].shaderGlobalBindings = bindings;
        scene->setPostProcessPasses(passes);
    } else {
        ShaderGlobalBindings* list = bindingsOf(project, ref);
        if (!list) return false;
        *list = bindings;
        switch (ref.owner) {
            case Owner::Material: scene->getComponent<MeshComponent>(ref.entity).needUpdateShaderUniforms = true; break;
            case Owner::UI: scene->getComponent<UIComponent>(ref.entity).needUpdateShaderUniforms = true; break;
            case Owner::Points: scene->getComponent<PointsComponent>(ref.entity).needUpdateShaderUniforms = true; break;
            case Owner::Lines: scene->getComponent<LineRendererComponent>(ref.entity).needUpdateShaderUniforms = true; break;
            case Owner::Sky: scene->getComponent<SkyboxComponent>(ref.entity).needUpdateShaderUniforms = true; break;
            case Owner::Trail: scene->getComponent<TrailRendererComponent>(ref.entity).needUpdateShaderUniforms = true; break;
            default: break;
        }
    }
    sceneProject->isModified = true;
    sceneProject->needUpdateRender = true;
    return true;
}

ShaderGlobalBindings ShaderGlobalsUsage::renamed(const ShaderGlobalBindings& bindings, const std::map<std::string, std::string>& renames) {
    ShaderGlobalBindings result = bindings;
    for (auto& binding : result) {
        auto it = renames.find(binding.second);
        if (it != renames.end()) binding.second = it->second;
    }
    return result;
}

std::vector<ShaderGlobalsUsage::Use> ShaderGlobalsUsage::find(Project* project, const std::string& global) {
    std::vector<Use> uses;
    forEachBindings(project, [&](const BindingsRef& ref, const ShaderGlobalBindings& bindings) {
        const SceneProject* sceneProject = project->getScene(ref.sceneId);
        for (const auto& binding : bindings) {
            if (!global.empty() && binding.second != global) continue;
            Use use;
            use.global = binding.second;
            use.binding = true;
            use.sceneId = ref.sceneId;
            use.scene = sceneProject ? sceneProject->name : std::string();
            use.entity = ref.entity;
            use.object = (ref.entity != NULL_ENTITY && sceneProject && sceneProject->scene) ? sceneProject->scene->getEntityName(ref.entity) : std::string();
            use.where = whereOf(ref.owner, ref.index);
            use.member = binding.first;
            uses.push_back(use);
        }
    });

    // the loaded shaders of the open scenes' objects that declare the globals
    for (SceneProject& sceneProject : project->getScenes()) {
        Scene* scene = sceneProject.scene;
        if (!scene) continue;
        Use base;
        base.sceneId = sceneProject.id;
        base.scene = sceneProject.name;
        auto meshes = scene->getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++) {
            const MeshComponent& mesh = meshes->getComponentFromIndex(i);
            base.entity = meshes->getEntity(i);
            base.object = scene->getEntityName(base.entity);
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
                base.where = whereOf(Owner::Material, (int)s);
                base.shader = RenderSystem::getSubmeshShader(mesh, mesh.submeshes[s], scene->getDefaultCustomShader(ShaderType::MESH));
                addShaderUses(uses, base, mesh.submeshes[s].libraryBindings, global);
                base.shader = mesh.customDepthShader.empty() ? base.shader : mesh.customDepthShader;
                addShaderUses(uses, base, mesh.submeshes[s].depthLibraryBindings, global);
            }
        }
        auto each = [&](auto* array, Owner owner, ShaderType type) {
            for (size_t i = 0; i < array->size(); i++) {
                const auto& component = array->getComponentFromIndex(i);
                base.entity = array->getEntity(i);
                base.object = scene->getEntityName(base.entity);
                base.where = whereOf(owner, 0);
                base.shader = componentShader(scene, component, type);
                addShaderUses(uses, base, component.libraryBindings, global);
            }
        };
        each(scene->getComponentArray<UIComponent>(), Owner::UI, ShaderType::UI);
        each(scene->getComponentArray<PointsComponent>(), Owner::Points, ShaderType::POINTS);
        each(scene->getComponentArray<LineRendererComponent>(), Owner::Lines, ShaderType::LINES);
        each(scene->getComponentArray<SkyboxComponent>(), Owner::Sky, ShaderType::SKYBOX);
        each(scene->getComponentArray<TrailRendererComponent>(), Owner::Trail, ShaderType::LINES);
    }
    return uses;
}

std::string ShaderGlobalsUsage::describe(const Use& use) {
    std::string owner = use.object.empty() ? use.where : use.object + " (" + use.where + ")";
    if (use.binding)
        return owner + ", member " + use.member + ", in " + use.scene;
    return (use.shader.empty() ? std::string("built-in shader") : use.shader) + " (" + use.stage + ") of " + owner + " in " + use.scene;
}
