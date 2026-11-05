// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorHost.h"

#include "Out.h"
#include "util/Scheduler.h"

using namespace jasperengine::editor;

void EditorHost::enqueueMainThreadTask(std::function<void()> task) {
    if (task) {
        task();
    }
}

void EditorHost::processMainThreadTasks() {
}

bool EditorHost::isMainThread() const {
    return true;
}

void EditorHost::addNewSceneToDock(uint32_t) {
}

void EditorHost::clearSceneWindowState(uint32_t) {
}

void EditorHost::sceneDeleting(uint32_t) {
}

void EditorHost::prepareForProjectSwitch() {
}

void EditorHost::requestProjectChange(std::function<void()> change) {
    if (change) {
        change();
    }
}

void EditorHost::resetLastActivatedScene() {
}

bool EditorHost::shouldSyncEngineApi() const {
    return false;
}

void EditorHost::updateResourcesPath() {
}

void EditorHost::updateWindowTitle(const std::string&) {
}

void EditorHost::stopTransientPreviews() {
}

void EditorHost::flushSceneMaterialWrites(uint32_t) {
}

void EditorHost::saveAllCodeEditors() {
}

void EditorHost::requestScenePlayFocus(uint32_t) {
}

void EditorHost::playStarting(uint32_t) {
}

std::string EditorHost::backgroundWorkConflict(const std::string&) const {
    return {};
}

void EditorHost::reportLoadingProgress(const std::string&) {
}

void EditorHost::registerAlert(std::string title, std::string message) {
    if (!title.empty()) {
        Out::warning("%s: %s", title.c_str(), message.c_str());
    } else {
        Out::warning(message);
    }
}

void EditorHost::registerConfirmAlert(std::string, std::string, std::function<void()> onYes, std::function<void()>) {
    if (onYes) {
        onYes();
    }
}

void EditorHost::registerThreeButtonAlert(std::string, std::string, std::function<void()> onYes, std::function<void()>, std::function<void()>) {
    if (onYes) {
        onYes();
    }
}

void EditorHost::registerSaveDiscardAlert(std::string, std::string, std::function<void()> onSave, std::function<void()>, std::function<void()>) {
    if (onSave) {
        onSave();
    }
}

void EditorHost::openInputActions(const std::filesystem::path& path) {
    Out::warning("The Input Actions window needs the editor window: %s not opened", path.string().c_str());
}

void EditorHost::openPrefabAsset(const std::filesystem::path& path, uint32_t, uint32_t, bool) {
    Out::warning("Prefab Mode needs the editor window: %s not opened", path.string().c_str());
}

void EditorHost::openBuildReport(const std::filesystem::path&) {
}

void EditorHost::openMeshAudit() {
}

void EditorHost::openConsole() {
}

void EditorHost::openAudioMixerWindow(const std::filesystem::path&) {
}

void EditorHost::openTextureSynthesis(const std::filesystem::path& image, bool) {
    Out::warning("Texture Synthesis needs the editor window: %s not opened", image.string().c_str());
}

void EditorHost::openVertexAnimationBaker(const std::filesystem::path& path, bool) {
    Out::warning("The Vertex Animation Baker needs the editor window: %s not opened", path.string().c_str());
}

void EditorHost::openSpriteNormals(const std::vector<std::filesystem::path>&, bool) {
    Out::warning("Sprite Normal Map needs the editor window: use the generate_sprite_normals MCP tool or the generate-sprite-normals command");
}

void EditorHost::openSpriteNormalsForEntity(uint32_t, jasperengine::Entity, unsigned, bool) {
    Out::warning("Sprite Normal Map needs the editor window: use the generate_sprite_normals MCP tool or the generate-sprite-normals command");
}

void EditorHost::registerSaveSceneDialog(uint32_t, std::function<void(bool)> callback) {
    // nobody to ask where: nothing is saved
    if (callback) {
        callback(false);
    }
}

DegradedSavePolicy EditorHost::confirmDegradedSave(const DegradedSaveRequest&) {
    return DegradedSavePolicy::Refuse;
}

RecoveryChoice EditorHost::resolveSceneRecovery(const SceneBackupInfo&) {
    return RecoveryChoice::Keep;
}

bool EditorHost::confirmOpenLockedProject(const ProjectLockInfo&) {
    return false;
}

void EditorHost::registerProjectSaveDialog(std::function<void(bool)> callback) {
    if (callback) {
        callback(false);
    }
}

InspectorWindow* EditorHost::getInspectorWindow() const {
    return nullptr;
}

CodeEditor* EditorHost::getCodeEditor() const {
    return nullptr;
}

ProjectWindow* EditorHost::getProjectWindow() const {
    return nullptr;
}

ImageViewerWindow* EditorHost::getImageViewerWindow() const {
    return nullptr;
}

static EditorHost s_defaultHost;
static EditorHost* s_activeHost = &s_defaultHost;

void jasperengine::editor::setEditorHost(EditorHost* host) {
    s_activeHost = host ? host : &s_defaultHost;
}

EditorHost& jasperengine::editor::getEditorHost() {
    return *s_activeHost;
}

jasperengine::Scheduler& jasperengine::editor::scheduler() {
    // Never destroyed: tasks posted by threads still running at exit
    static Scheduler* instance = new Scheduler("editor");
    return *instance;
}
