// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundDrop.h"

#include "Project.h"
#include "audio/BuiltinSoundImport.h"
#include "audio/SoundEssentials.h"
#include "audio/SoundLibrary.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/AudioSourceComponent.h"
#include "component/ButtonComponent.h"
#include "component/Transform.h"
#include "util/ProjectFileWrites.h"
#include "util/Util.h"
#include "window/SoundBrowserWindow.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    bool isSoundFile(const std::string& path) {
        return Util::isAudioFile(path) || Util::isSoundEventFile(path);
    }

    // the AudioSource's field for the sound
    void setSoundOn(MultiPropertyCmd* multi, Project* project, uint32_t sceneId, Entity entity, const std::string& path) {
        multi->addPropertyCmd<AssetRef>(project, sceneId, entity, ComponentType::AudioSourceComponent,
                                        Util::isSoundEventFile(path) ? "event" : "filename", AssetRef(path));
    }

    bool importBuiltins(Project* project, const std::vector<std::string>& references, std::vector<std::string>& paths, std::string& error) {
        std::vector<BuiltinSoundImport::Clip> clips;
        if (!BuiltinSoundImport::resolve(references, clips, error)) return false;
        for (const BuiltinSoundImport::Clip& clip : clips) {
            const char* folder = clip.library ? SoundLibrary::IMPORT_FOLDER : SoundEssentials::IMPORT_FOLDER;
            BuiltinSoundImport::Plan plan;
            if (!BuiltinSoundImport::plan({clip}, project->getAssetsPath() / fs::path(folder), BuiltinSoundImport::Existing::SkipIdentical, plan, error)) return false;
            if (!plan.writes.empty()) {
                std::vector<WriteFilesCmd::Item> items;
                for (BuiltinSoundImport::File& file : plan.writes) items.push_back({file.path, std::move(file.content)});
                if (!ProjectFileWrites::writeUndoable(project, std::move(items), error)) return false;
            }
            for (const fs::path& file : plan.imported) paths.push_back(project->normalizeToAssetsRelative(file).generic_string());
            for (const fs::path& file : plan.skipped) paths.push_back(project->normalizeToAssetsRelative(file).generic_string());
            if (!plan.conflicts.empty()) {
                error = project->normalizeToAssetsRelative(plan.conflicts.front()).generic_string() + " exists and differs from the built-in clip: drop that file instead";
                return false;
            }
        }
        return true;
    }

}

bool SoundDrop::accepts(const ImGuiPayload* payload) {
    if (!payload) return false;
    if (payload->IsDataType(SoundBrowserWindow::BUILTIN_PAYLOAD)) return true;
    if (!payload->IsDataType("resource_files")) return false;
    for (const std::string& path : Util::getStringsFromPayload(payload)) {
        if (isSoundFile(path)) return true;
    }
    return false;
}

bool SoundDrop::resolve(Project* project, const ImGuiPayload* payload, std::vector<std::string>& paths, std::string& error) {
    const std::vector<std::string> strings = Util::getStringsFromPayload(payload);
    if (payload->IsDataType(SoundBrowserWindow::BUILTIN_PAYLOAD)) return importBuiltins(project, strings, paths, error);
    for (const std::string& path : strings) {
        if (!isSoundFile(path) || !project->isInsideAssetsPath(path)) continue;
        paths.push_back(project->normalizeToAssetsRelative(path).generic_string());
    }
    return true;
}

bool SoundDrop::createSource(Project* project, uint32_t sceneId, const std::string& path, bool has3D, const Vector3& position, std::string& error) {
    CommandHistory* handle = CommandHandle::get(sceneId);
    UndoGroupScope group(handle);
    const std::string name = fs::path(path).stem().string();
    auto* create = new CreateEntityCmd(project, sceneId, name, has3D ? EntityCreationType::AUDIO_SOURCE_3D : EntityCreationType::AUDIO_SOURCE);
    handle->addCommandNoMerge(create);
    const Entity entity = create->getEntity();
    if (entity == NULL_ENTITY) {
        error = "the GameObject could not be created";
        return false;
    }
    auto* multi = new MultiPropertyCmd();
    setSoundOn(multi, project, sceneId, entity, path);
    if (has3D) multi->addPropertyCmd<Vector3>(project, sceneId, entity, ComponentType::Transform, "position", position);
    handle->addCommandNoMerge(multi);
    return true;
}

bool SoundDrop::assignToButton(Project* project, uint32_t sceneId, Entity entity, const std::string& path, std::string& error) {
    SceneProject* scene = project->getScene(sceneId);
    if (!scene || !scene->scene || !scene->scene->findComponent<ButtonComponent>(entity)) return false;
    CommandHistory* handle = CommandHandle::get(sceneId);
    UndoGroupScope group(handle);
    if (!scene->scene->findComponent<AudioSourceComponent>(entity)) {
        handle->addCommandNoMerge(new AddComponentCmd(project, sceneId, entity, ComponentType::AudioSourceComponent));
    }
    if (!scene->scene->findComponent<AudioSourceComponent>(entity)) {
        error = "the AudioSource could not be added";
        return false;
    }
    auto* multi = new MultiPropertyCmd();
    setSoundOn(multi, project, sceneId, entity, path);
    multi->addPropertyCmd<unsigned int>(project, sceneId, entity, ComponentType::AudioSourceComponent, "playOn",
                                        scene->scene->getComponent<AudioSourceComponent>(entity).playOn | AudioPlayOn_UIClick);
    handle->addCommandNoMerge(multi);
    return true;
}
