// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FogProfileEdits.h"

#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "asset/AssetTypes.h"
#include "command/CommandGroup.h"
#include "command/CommandNames.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/FogComponent.h"
#include "component/FogZoneComponent.h"
#include "util/FileUtils.h"

#include <filesystem>
#include <memory>
#include <set>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::fogprofiles;

namespace fs = std::filesystem;

// file-local, in a namespace of its own: a unity build merges the editor's sources
namespace jasperengine::editor::fogprofiles::edits {
namespace {

    // A profile file's text before and after, as ParticleEffectAssetCmd keeps a .particles file's:
    // executing writes it and hands the profile to the engine (every user shows it on the next
    // frame); an empty `before` creates it with `guid`, and undoing that deletes it and its meta.
    class FogProfileFileCmd : public Command {
    private:
        Project* project;
        std::string path;       // project-relative
        std::string before;
        std::string after;
        Guid guid;

        fs::path file() const { return project->getProjectPath() / FileUtils::pathFromUtf8(path); }

        std::string assetPath() const {
            return FileUtils::pathToGenericUtf8(file().lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal()));
        }

        bool apply(const std::string& text) {
            const fs::path target = file();
            if (text.empty()) {
                // undoing the creation: the file and its meta go, the asset leaves the database
                std::error_code ec;
                const fs::path relative = project->getAssetDatabase().toRelative(target);
                fs::remove(target, ec);
                fs::remove(project->getProjectPath() / AssetTypes::metaPathFor(relative.generic_string()), ec);
                project->getAssetDatabase().removeAssets(target);
                profileChanged(*project, assetPath());
                return true;
            }
            FogProfileData data;
            std::string error;
            if (!FogProfileIO::parse(text, data, &error)) {
                Out::error("Fog Profile %s: %s", path.c_str(), error.c_str());
                return false;
            }
            std::error_code ec;
            const bool created = !fs::exists(target, ec);
            if (!writeProfile(*project, path, data, &error)) {
                Out::error("%s", error.c_str());
                return false;
            }
            if (created && !guid.isNull()) project->getAssetDatabase().registerAsset(target, guid.str());
            return true;
        }

    public:
        FogProfileFileCmd(Project* project, std::string path, std::string before, std::string after, Guid guid)
            : project(project), path(std::move(path)), before(std::move(before)), after(std::move(after)), guid(guid) {}

        bool execute() override { return apply(after); }
        void undo() override { apply(before); }

        bool affectsStructure() const override { return before.empty(); }
        std::string describe() const override {
            return (before.empty() ? "Create Fog Profile " : "Edit Fog Profile ") + CommandNames::file(path);
        }
        const char* typeName() const override { return "FogProfileFileCmd"; }
        bool isNoOp() const override { return before == after; }
        size_t memoryBytes() const override { return sizeof(*this) + before.capacity() + after.capacity(); }
    };

    // a property edit setting `field` of the Fog of `entity` to `target`'s value
    void addFogFieldEdit(MultiPropertyCmd& multi, Project* project, uint32_t sceneId, Entity entity, const FogComponent& target, FogField field) {
        const FogFieldInfo& info = FogFields::info(field);
        float value[3] = {0.0f, 0.0f, 0.0f};
        FogFields::read(target, field, value);
        switch (info.kind) {
            case FogFieldKind::Float:
            case FogFieldKind::Extinction:
                multi.addPropertyCmd<float>(project, sceneId, entity, ComponentType::FogComponent, info.name, value[0]);
                break;
            case FogFieldKind::Color:
            case FogFieldKind::Vector3:
                multi.addPropertyCmd<Vector3>(project, sceneId, entity, ComponentType::FogComponent, info.name, Vector3(value[0], value[1], value[2]));
                break;
            case FogFieldKind::Bool:
                multi.addPropertyCmd<bool>(project, sceneId, entity, ComponentType::FogComponent, info.name, value[0] != 0.0f);
                break;
            case FogFieldKind::Enum:
                multi.addPropertyCmd<int>(project, sceneId, entity, ComponentType::FogComponent, info.name, static_cast<int>(value[0]));
                break;
        }
    }

    // one named undo step of the edits (all or nothing)
    Command* namedStep(std::unique_ptr<MultiPropertyCmd> multi, const std::string& label) {
        std::vector<std::unique_ptr<Command>> commands;
        commands.push_back(std::move(multi));
        return new CommandGroup(std::move(commands), label, false);
    }

    bool sameProfileRef(const AssetRef& ref, const std::string& assetPath) {
        return !ref.path.empty() && FogProfileLibrary::normalizeKey(ref.path) == FogProfileLibrary::normalizeKey(assetPath);
    }

}
}

using namespace jasperengine::editor::fogprofiles::edits;

Command* jasperengine::editor::fogprofiles::buildProfileEdit(Project& project, const std::string& projectRelativePath, const FogProfileData* before,
                                                            const FogProfileData& after) {
    const std::string afterText = FogProfileIO::write(after);
    if (before) {
        const std::string beforeText = FogProfileIO::write(*before);
        if (beforeText == afterText) return nullptr;
        return new FogProfileFileCmd(&project, projectRelativePath, beforeText, afterText, Guid());
    }
    return new FogProfileFileCmd(&project, projectRelativePath, std::string(), afterText, Guid::parse(AssetDatabase::generateGuid()));
}

Command* jasperengine::editor::fogprofiles::buildLink(Project& project, uint32_t sceneId, Entity entity, const std::string& name, FogLinkPlan& plan,
                                                     std::string& error) {
    error.clear();
    SceneProject* sceneProject = project.getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        error = "Scene not found";
        return nullptr;
    }
    Scene* scene = sceneProject->scene;
    const FogComponent* fog = scene->findComponent<FogComponent>(entity);
    const FogZoneComponent* zone = fog || !scene->isComponentRegistered<FogZoneComponent>() ? nullptr : scene->findComponent<FogZoneComponent>(entity);
    if (!fog && !zone) {
        error = "\"" + scene->getEntityName(entity) + "\" has neither a Fog nor a Fog Zone";
        return nullptr;
    }
    plan.zone = zone != nullptr;
    plan.profile = resolveProfileName(project, name);
    if (!plan.profile.ok) {
        error = plan.profile.error;
        return nullptr;
    }

    auto multi = std::make_unique<MultiPropertyCmd>();
    Guid guid = project.getAssetDatabase().getAssetGuid(project.getProjectPath() / FileUtils::pathFromUtf8(plan.profile.path));
    if (plan.profile.needsCopy) {
        // the built-in's project copy, written in this step (undo deletes it)
        guid = Guid::parse(AssetDatabase::generateGuid());
        multi->addCommand(std::make_unique<FogProfileFileCmd>(&project, plan.profile.path, std::string(), plan.profile.copyText, guid));
    }
    const AssetRef& current = fog ? fog->profile : zone->profile;
    const bool linked = !plan.profile.needsCopy && sameProfileRef(current, plan.profile.assetPath);
    if (!linked) {
        multi->addPropertyCmd<AssetRef>(&project, sceneId, entity, fog ? ComponentType::FogComponent : ComponentType::FogZoneComponent, "profile",
                                        AssetRef(plan.profile.assetPath, guid));
    }
    if (fog && fog->type != FogType::HEIGHT) {
        // as the presets did: a look of height fog fields makes the Fog a height fog (R23)
        for (size_t i = 0; i < FOG_FIELD_COUNT && !plan.typeToHeight; i++) {
            const FogField field = static_cast<FogField>(i);
            plan.typeToHeight = plan.profile.data.isSet(field) && !FogFields::readByType(fog->type, field);
        }
        if (plan.typeToHeight)
            multi->addPropertyCmd<int>(&project, sceneId, entity, ComponentType::FogComponent, "type", static_cast<int>(FogType::HEIGHT));
    }
    if (multi->empty()) return nullptr;
    const std::string file = fs::path(plan.profile.path).filename().generic_string();
    return namedStep(std::move(multi), "Link Fog Profile " + file + " (" + CommandNames::entities(&project, sceneId, {entity}) + ")");
}

Command* jasperengine::editor::fogprofiles::buildUnlink(Project& project, uint32_t sceneId, Entity entity, std::string& error) {
    error.clear();
    SceneProject* sceneProject = project.getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        error = "Scene not found";
        return nullptr;
    }
    Scene* scene = sceneProject->scene;
    const FogComponent* fog = scene->findComponent<FogComponent>(entity);
    if (!fog) {
        error = "\"" + scene->getEntityName(entity) + "\" has no Fog";
        return nullptr;
    }
    if (!fog->profile.isSet()) return nullptr;

    // the look it shows now (its own fields under its profile) becomes its own
    FogComponent look = *fog;
    if (const std::shared_ptr<const FogProfileData> profile = FogProfileLibrary::getRef(fog->profile)) profile->applyTo(look);
    auto multi = std::make_unique<MultiPropertyCmd>();
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++) {
        const FogField field = static_cast<FogField>(i);
        if (!FogFields::equal(*fog, look, field)) addFogFieldEdit(*multi, &project, sceneId, entity, look, field);
    }
    multi->addPropertyCmd<AssetRef>(&project, sceneId, entity, ComponentType::FogComponent, "profile", AssetRef());
    return namedStep(std::move(multi), "Unlink Fog Profile (" + CommandNames::entities(&project, sceneId, {entity}) + ")");
}

FogProfileUsers jasperengine::editor::fogprofiles::profileUsers(Project& project, const std::string& assetPath) {
    FogProfileUsers users;
    users.open = usersInOpenScenes(project, assetPath);
    // an open scene's file holds what it held when saved: its live GameObjects count instead
    std::set<std::string> openFiles;
    for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
        if (!sceneProject->scene || sceneProject->filepath.empty()) continue;
        const fs::path file = sceneProject->filepath.is_absolute() ? sceneProject->filepath : project.getProjectPath() / sceneProject->filepath;
        openFiles.insert(FileUtils::pathToGenericUtf8(file.lexically_normal().lexically_relative(project.getProjectPath().lexically_normal())));
    }
    for (const std::string& file : referencingFiles(project, assetPath)) {
        if (!openFiles.count(file)) users.closedFiles.push_back(file);
    }
    return users;
}

std::string jasperengine::editor::fogprofiles::projectPathOfKey(const Project& project, const std::string& key) {
    if (key.empty() || FogProfiles::isBuiltinKey(key) || FogProfileLibrary::isRuntimeKey(key) || !FogProfileIO::isProfilePath(key)) return key;
    const fs::path file = project.getAssetsPath() / FileUtils::pathFromUtf8(FogProfileLibrary::normalizeKey(key));
    return FileUtils::pathToGenericUtf8(file.lexically_normal().lexically_relative(project.getProjectPath().lexically_normal()));
}
