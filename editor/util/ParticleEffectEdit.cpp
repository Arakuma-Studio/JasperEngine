// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleEffectEdit.h"

#include "Project.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/ParticleEffectAssetCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ParticleSystemComponent.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "util/AssetDatabase.h"

#include <filesystem>
#include <map>
#include <memory>
#include "pose/PoseUiLogic.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

bool ParticleEffectEdit::readAsset(const std::string& assetPath, particles::ParticleEffectDocument& document,
                                   std::vector<particles::Diagnostic>* diagnostics) {
    const particles::EffectAsset& asset = particles::ParticleEffectLibrary::get(assetPath);
    if (diagnostics) *diagnostics = asset.diagnostics;
    if (!asset.document) return false;
    document = *asset.document;
    return true;
}

bool ParticleEffectEdit::read(const ParticleSystemComponent& particles, particles::ParticleEffectDocument& document,
                              std::vector<particles::Diagnostic>* diagnostics) {
    if (!particles.effect.empty()) {
        return readAsset(particles.effect.path, document, diagnostics);
    }
    document = particles.inlineEffect;
    if (diagnostics) {
        diagnostics->clear();
        particles::ParticleEffectCodec::validate(document, *diagnostics);
    }
    return true;
}

Json ParticleEffectEdit::diff(const Json& before, const Json& after) {
    if (!before.is_object() || !after.is_object()) return after;
    Json patch = Json::object();
    for (auto it = before.begin(); it != before.end(); ++it) {
        if (!after.contains(it.key())) patch[it.key()] = nullptr;
    }
    for (auto it = after.begin(); it != after.end(); ++it) {
        if (!before.contains(it.key())) {
            patch[it.key()] = *it;
        } else if (before[it.key()] != *it) {
            // objects merge key by key, anything else is replaced whole
            patch[it.key()] = (it->is_object() && before[it.key()].is_object()) ? diff(before[it.key()], *it) : *it;
        }
    }
    return patch;
}

Command* ParticleEffectEdit::build(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, const Edit& edit) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return nullptr;
    Scene* scene = sceneProject->scene;

    auto multi = std::make_unique<MultiPropertyCmd>();
    std::map<std::string, bool> assetsDone;
    for (Entity entity : entities) {
        ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity);
        if (!particles) continue;
        if (!particles->effect.empty()) {
            const std::string& path = particles->effect.path;
            if (assetsDone[path]) continue;
            assetsDone[path] = true;
            particles::ParticleEffectDocument document;
            if (!readAsset(path, document)) continue;
            const std::string before = particles::ParticleEffectCodec::write(document);
            if (!edit(document)) continue;
            const std::string after = particles::ParticleEffectCodec::write(document);
            if (after == before) continue;
            multi->addCommand(std::make_unique<ParticleEffectAssetCmd>(project, path, before, after));
        } else {
            particles::ParticleEffectDocument document = particles->inlineEffect;
            const std::string before = particles::ParticleEffectCodec::write(document);
            if (!edit(document)) continue;
            if (particles::ParticleEffectCodec::write(document) == before) continue;
            multi->addPropertyCmd<particles::ParticleEffectDocument>(project, sceneId, entity, ComponentType::ParticleSystemComponent, "inlineEffect", document);
        }
    }
    if (multi->empty()) return nullptr;
    return multi.release();
}

Command* ParticleEffectEdit::buildAsset(Project* project, const std::string& assetPath, const Edit& edit) {
    particles::ParticleEffectDocument document;
    if (!readAsset(assetPath, document)) return nullptr;
    const std::string before = particles::ParticleEffectCodec::write(document);
    if (!edit(document)) return nullptr;
    const std::string after = particles::ParticleEffectCodec::write(document);
    if (after == before) return nullptr;
    return new ParticleEffectAssetCmd(project, assetPath, before, after);
}

Command* ParticleEffectEdit::buildSaveAsAsset(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, const std::string& assetPath) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || entities.empty()) return nullptr;
    ParticleSystemComponent* first = sceneProject->scene->findComponent<ParticleSystemComponent>(entities.front());
    particles::ParticleEffectDocument document;
    if (!first || !read(*first, document)) return nullptr;

    const Guid guid = Guid::parse(AssetDatabase::generateGuid());
    auto multi = std::make_unique<MultiPropertyCmd>();
    multi->addCommand(std::make_unique<ParticleEffectAssetCmd>(project, assetPath, std::string(), particles::ParticleEffectCodec::write(document), guid));
    AssetRef ref;
    ref.set(assetPath, guid);
    for (Entity entity : entities) {
        if (!sceneProject->scene->findComponent<ParticleSystemComponent>(entity)) continue;
        multi->addPropertyCmd<AssetRef>(project, sceneId, entity, ComponentType::ParticleSystemComponent, "effect", ref);
    }
    return multi.release();
}

Command* ParticleEffectEdit::buildMakeInline(Project* project, uint32_t sceneId, const std::vector<Entity>& entities) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return nullptr;
    auto multi = std::make_unique<MultiPropertyCmd>();
    for (Entity entity : entities) {
        ParticleSystemComponent* particles = sceneProject->scene->findComponent<ParticleSystemComponent>(entity);
        if (!particles || particles->effect.empty()) continue;
        particles::ParticleEffectDocument document;
        if (!readAsset(particles->effect.path, document)) continue;
        multi->addPropertyCmd<particles::ParticleEffectDocument>(project, sceneId, entity, ComponentType::ParticleSystemComponent, "inlineEffect", document);
        multi->addPropertyCmd<AssetRef>(project, sceneId, entity, ComponentType::ParticleSystemComponent, "effect", AssetRef());
    }
    if (multi->empty()) return nullptr;
    return multi.release();
}

std::string ParticleEffectEdit::freeAssetPath(Project* project, const std::string& directory, const std::string& name) {
    std::string base;
    for (char c : name) {
        // a file name every platform takes
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ';
        base += keep ? c : '_';
    }
    if (base.empty()) base = "Particle Effect";
    const std::string prefix = directory.empty() ? std::string() : directory + "/";
    std::string candidate = prefix + base + ".particles";
    for (int i = 1; fs::exists(project->resolveAssetPath(candidate)); i++) {
        candidate = prefix + base + " " + std::to_string(i) + ".particles";
    }
    return candidate;
}
