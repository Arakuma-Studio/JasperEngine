// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatBakeSource.h"

#include "Project.h"
#include "command/type/ModelLoadCmd.h"
#include "animation/ClipConverters.h"
#include "animation/ClipEditorUtil.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseBaker.h"
#include "util/EntityPath.h"
#include "util/FileUtils.h"
#include "util/SHA1.h"

#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/ModelSubClips.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/Transform.h"
#include "lattice/LatticeRender.h"
#include "object/Model.h"
#include "pool/AnimationClipPool.h"
#include "render/CpuSkin.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"

#include "json.hpp"
#include "tiny_gltf.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

namespace fs = std::filesystem;
using namespace jasperengine;

namespace jasperengine::editor::vat{

namespace{

    VatIssue makeIssue(const std::string& kind, bool error, const std::string& message, const std::string& fix){
        VatIssue issue;
        issue.kind = kind;
        issue.error = error;
        issue.message = message;
        issue.fix = fix;
        return issue;
    }

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    std::string extensionOf(const std::string& path){
        return lower(FileUtils::pathToUtf8(FileUtils::pathFromUtf8(path).extension()));
    }

    std::string stemOf(const std::string& path){
        return FileUtils::pathToUtf8(FileUtils::pathFromUtf8(path).stem());
    }

    std::string parentOf(const std::string& path){
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }

    std::string uniqueName(const std::string& base, const std::vector<VatSourceClip>& clips){
        auto taken = [&](const std::string& name){
            for (const VatSourceClip& clip : clips){
                if (clip.name == name) return true;
            }
            return false;
        };
        std::string name = base.empty() ? std::string("Clip") : base;
        const std::string stem = name;
        for (int n = 2; taken(name); n++) name = stem + " " + std::to_string(n);
        return name;
    }

    // a project-relative path inside the project, its absolute form
    bool projectFile(Project* project, const std::string& path, fs::path& absolute, std::string& relative, std::string& error){
        if (path.empty()){
            error = "no path given";
            return false;
        }
        fs::path given = FileUtils::pathFromUtf8(path);
        absolute = (given.is_absolute() ? given : project->getProjectPath() / given).lexically_normal();
        relative = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute));
        if (relative.compare(0, 2, "..") == 0 || FileUtils::pathFromUtf8(relative).is_absolute()){
            error = "'" + path + "' is outside the project";
            return false;
        }
        std::error_code ec;
        if (!fs::is_regular_file(absolute, ec)){
            error = "'" + relative + "' does not exist";
            return false;
        }
        return true;
    }

    void addHash(VatBakeTarget& target, const std::string& relative, const fs::path& absolute){
        for (const auto& entry : target.hashes){
            if (entry.first == relative) return;
        }
        target.hashes.emplace_back(relative, fileSha1(FileUtils::pathToUtf8(absolute)));
    }

    // a model file and the external buffers a .gltf reads (the geometry and animations)
    void addModelHashes(VatBakeTarget& target, Project* project, const std::string& relative, const fs::path& absolute){
        addHash(target, relative, absolute);
        if (extensionOf(relative) != ".gltf") return;
        std::string text;
        if (!clipui::readWholeFile(absolute, text)) return;
        nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
        if (!json.is_object() || !json.contains("buffers") || !json["buffers"].is_array()) return;
        for (const nlohmann::json& buffer : json["buffers"]){
            if (!buffer.is_object() || !buffer.contains("uri") || !buffer["uri"].is_string()) continue;
            const std::string uri = buffer["uri"].get<std::string>();
            if (uri.compare(0, 5, "data:") == 0) continue;
            const fs::path file = (absolute.parent_path() / FileUtils::pathFromUtf8(uri)).lexically_normal();
            std::error_code ec;
            if (!fs::is_regular_file(file, ec)) continue;
            addHash(target, FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file)), file);
        }
    }

    bool underRoot(Scene* scene, Entity entity, Entity root){
        Entity current = entity;
        for (int depth = 0; current != NULL_ENTITY && depth < 65536; depth++){
            if (current == root) return true;
            const Transform* transform = scene->findComponent<Transform>(current);
            if (!transform) return false;
            current = transform->parent;
        }
        return false;
    }

    // the model files of every Model at or under the root: the meshes a hierarchy bake reads
    // (the scene file itself is not a source: saving it changes nothing the bake read)
    void addHierarchyHashes(Project* project, VatBakeTarget& target){
        Scene* scene = target.scene;
        auto models = scene->getComponentArray<ModelComponent>();
        for (size_t i = 0; i < models->size(); i++){
            const ModelComponent& model = models->getComponentFromIndex(i);
            if (model.filename.empty() || !underRoot(scene, models->getEntity(i), target.root)) continue;
            const fs::path file = project->resolveAssetPath(FileUtils::pathFromUtf8(model.filename)).lexically_normal();
            std::error_code ec;
            if (!fs::is_regular_file(file, ec)) continue;
            addModelHashes(target, project, FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file)), file);
        }
    }

    std::string assetRelative(Project* project, const fs::path& absolute){
        return ClipConverters::assetRelative(project, absolute);
    }

    // the sampled tracks of a clip that resolve to nothing under the root
    void bindReport(Scene* scene, Entity root, VatSourceClip& clip){
        std::set<std::string> unbound;
        clip.tracks = 0;
        for (const AnimationClip::Track& track : clip.clip->getTracks()){
            if (!track.isSampled()) continue;
            clip.tracks++;
            if (anim::resolvePath(scene, root, track.path) == NULL_ENTITY) unbound.insert(track.path);
        }
        clip.unbound.assign(unbound.begin(), unbound.end());
    }

    bool addClip(VatBakeTarget& target, const std::string& name, const std::string& origin, std::shared_ptr<const AnimationClip> clip,
                 ClipWrapMode wrap){
        if (!clip) return false;
        VatSourceClip entry;
        entry.name = uniqueName(name, target.clips);
        entry.origin = origin;
        entry.wrap = wrap == ClipWrapMode::Default ? clip->getWrap() : wrap;
        if (entry.wrap == ClipWrapMode::Default) entry.wrap = ClipWrapMode::Loop;
        entry.length = clip->getLength();
        entry.events = clip->getEvents();
        entry.clip = std::move(clip);
        bindReport(target.scene, target.root, entry);
        target.clips.push_back(std::move(entry));
        return true;
    }

    // a .anim file -> runtime clip
    std::shared_ptr<const AnimationClip> loadAnimFile(const fs::path& absolute, std::string& error){
        std::string text;
        if (!clipui::readWholeFile(absolute, text)){
            error = "can't read " + FileUtils::pathToGenericUtf8(absolute);
            return nullptr;
        }
        anim::ClipParseResult parsed = anim::parseClip(text);
        std::shared_ptr<const AnimationClip> clip = AnimationClip::build(parsed);
        if (!clip) error = FileUtils::pathToGenericUtf8(absolute) + ": " + parsed.error.message;
        return clip;
    }

    // the model clips of a loaded model file (glTF/FBX carry no wrap: Loop)
    bool addModelClips(Project* project, VatBakeTarget& target, const std::string& modelAsset, const std::string& modelRelative,
                       const std::vector<std::string>& only, std::vector<VatIssue>& issues){
        std::vector<anim::ModelClipInfo> infos;
        std::string error;
        if (!anim::listModelFileClips(modelAsset, infos, &error)){
            issues.push_back(makeIssue("invalid_source", true, "can't list the animations of '" + modelRelative + "': " + error,
                                       "check that the model file loads"));
            return false;
        }
        (void)project;
        for (const anim::ModelClipInfo& info : infos){
            if (!only.empty() && std::find(only.begin(), only.end(), info.name) == only.end()) continue;
            ClipData data;
            std::vector<ClipWarning> warnings;
            const std::string reference = anim::formatSubClipReference(modelAsset, info.name);
            if (!ClipConverters::extractModelSubClip(reference, data, warnings, &error)){
                issues.push_back(makeIssue("not_converted", false, "the animation '" + info.name + "' of '" + modelRelative +
                                           "' can't be read: " + error, "fix or re-export the model"));
                continue;
            }
            data.wrap = ClipWrapMode::Loop;
            addClip(target, info.name, modelRelative + "#" + info.name, AnimationClip::build(data), ClipWrapMode::Loop);
        }
        return true;
    }

    // the open scene whose file is `scene` (project-relative)
    // The first Model at or under `root` whose meshes are not built yet, NULL_ENTITY when none.
    // A Model restored by undo (its GameObject's deletion undone), of a scene just opened or with
    // a new file has needUpdateModel: MeshSystem builds its meshes in a following frame (on a
    // worker in the editor), and until then they hold submeshes without vertex data. An editor
    // ModelLoadCmd still loading counts too. A load that fails clears needUpdateModel.
    Entity loadingModel(uint32_t sceneId, Scene* scene, Entity root){
        auto models = scene->getComponentArray<ModelComponent>();
        for (size_t i = 0; i < models->size(); i++){
            const Entity entity = models->getEntity(i);
            const ModelComponent& model = models->getComponentFromIndex(i);
            const bool loading = (model.needUpdateModel && !model.filename.empty()) || ModelLoadCmd::isLoadPending(sceneId, entity);
            if (!loading) continue;
            Entity current = entity;
            for (int depth = 0; current != NULL_ENTITY && depth < 65536; depth++){
                if (current == root) return entity;
                const Transform* transform = scene->findComponent<Transform>(current);
                if (!transform) break;
                current = transform->parent;
            }
        }
        return NULL_ENTITY;
    }

    SceneProject* openScene(Project* project, const std::string& scene){
        const fs::path wanted = (project->getProjectPath() / FileUtils::pathFromUtf8(scene)).lexically_normal();
        for (SceneProject& candidate : project->getScenes()){
            if (!candidate.scene || candidate.filepath.empty()) continue;
            fs::path file = candidate.filepath.is_absolute() ? candidate.filepath : project->getProjectPath() / candidate.filepath;
            if (file.lexically_normal() == wanted) return &candidate;
        }
        return nullptr;
    }

    bool resolveGameObject(Project* project, const VatSourceSpec& spec, VatBakeTarget& target, std::vector<VatIssue>& issues){
        SceneProject* sceneProject = nullptr;
        std::string sceneName = spec.scene;
        if (spec.sceneId != 0){
            sceneProject = project->getScene(spec.sceneId);
            if (sceneProject && !sceneProject->filepath.empty()){
                const fs::path file = sceneProject->filepath.is_absolute() ? sceneProject->filepath
                                                                           : project->getProjectPath() / sceneProject->filepath;
                sceneName = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file.lexically_normal()));
            }else if (sceneProject){
                sceneName.clear();
            }
        }else if (!spec.scene.empty()){
            sceneProject = openScene(project, spec.scene);
        }else{
            issues.push_back(makeIssue("invalid_source", true, "a GameObject source needs its scene file", "pass the scene (e.g. assets/scenes/main.scene)"));
            return false;
        }
        const std::string sceneLabel = sceneName.empty() ? (sceneProject ? sceneProject->name : std::string("the scene")) : sceneName;
        if (!sceneProject || !sceneProject->scene){
            issues.push_back(makeIssue("invalid_source", true, "the scene '" + sceneLabel + "' is not open", "open the scene to bake its GameObjects"));
            return false;
        }
        if (sceneProject->playState != ScenePlayState::STOPPED){
            issues.push_back(makeIssue("scene_playing", true, "the scene '" + sceneLabel + "' is playing", "stop play mode before baking"));
            return false;
        }
        const Entity entity = EntityPath::find(*sceneProject, spec.path);
        if (entity == NULL_ENTITY){
            issues.push_back(makeIssue("invalid_source", true, "no GameObject '" + spec.path + "' in '" + sceneLabel + "'",
                                       "give the GameObject's hierarchy path (e.g. Crowd/Hero)"));
            return false;
        }
        // its meshes, clips and files are read below and when it is posed: not before they exist
        const Entity loading = loadingModel(sceneProject->id, sceneProject->scene, entity);
        if (loading != NULL_ENTITY){
            issues.push_back(makeIssue("source_loading", true, "the model of '" + sceneProject->scene->getEntityName(loading) +
                                       "' is still loading (a GameObject brought back by undo, a scene just opened or a new model "
                                       "file builds its meshes in the next frames)", "bake again once it has loaded (a moment)"));
            return false;
        }
        target.sceneId = sceneProject->id;
        target.scene = sceneProject->scene;
        target.root = entity;
        target.sceneFile = sceneName;
        fs::path sceneFile;
        if (!sceneName.empty()) sceneFile = (project->getProjectPath() / FileUtils::pathFromUtf8(sceneName)).lexically_normal();
        addHierarchyHashes(project, target);
        target.stem = target.scene->getEntityName(entity);
        // next to its model when it has one, else next to the scene (inside assets), else assets/
        target.directory.clear();
        if (const ModelComponent* model = target.scene->findComponent<ModelComponent>(entity)){
            if (!model->filename.empty()){
                const fs::path file = project->resolveAssetPath(FileUtils::pathFromUtf8(model->filename)).lexically_normal();
                target.directory = parentOf(FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file)));
            }
        }
        if (target.directory.empty() && !sceneName.empty() && project->isInsideAssetsPath(sceneFile)) target.directory = parentOf(sceneName);
        if (target.directory.empty()) target.directory = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(project->getAssetsPath()));
        return true;
    }

    // a model or prefab file instantiated in the target's private scene
    bool instantiateFile(Project* project, const std::string& path, VatBakeTarget& target, Scene*& ownedScene, Model*& ownedModel,
                         std::vector<VatIssue>& issues){
        fs::path absolute;
        std::string relative, error;
        if (!projectFile(project, path, absolute, relative, error)){
            issues.push_back(makeIssue("invalid_source", true, error, "give a model (.gltf/.glb/.fbx) or prefab inside the project"));
            return false;
        }
        ownedScene = new Scene(EntityPool::System);
        target.scene = ownedScene;
        target.sceneId = 0;
        const std::string extension = extensionOf(relative);
        if (extension == ".prefab"){
            target.root = project->instantiatePrefabPreview(ownedScene, absolute);
            if (target.root == NULL_ENTITY){
                issues.push_back(makeIssue("invalid_source", true, "the prefab '" + relative + "' could not be instantiated", "check that it opens"));
                return false;
            }
            addHash(target, relative, absolute);
            addHierarchyHashes(project, target);
        }else if (anim::isModelFilePath(relative)){
            const std::string asset = assetRelative(project, absolute);
            if (!project->prepareModelFile(asset)){
                issues.push_back(makeIssue("invalid_source", true, "the model '" + relative + "' could not be imported", "check the file"));
                return false;
            }
            ownedModel = new Model(ownedScene);
            target.root = ownedModel->getEntity();
            // the node hierarchy the clips address (animation clips 9.R3)
            ownedScene->addComponent<AnimationPlayerComponent>(target.root, {});
            if (!ownedModel->loadModel(FileUtils::pathToUtf8(absolute))){
                issues.push_back(makeIssue("invalid_source", true, "the model '" + relative + "' could not be loaded", "check the file"));
                return false;
            }
            ownedScene->setEntityName(target.root, stemOf(relative));
            addModelHashes(target, project, relative, absolute);
        }else{
            issues.push_back(makeIssue("invalid_source", true, "'" + relative + "' is not a model or prefab",
                                       "play the clips on a .gltf, .glb, .fbx or .prefab"));
            return false;
        }
        cpuskin::updateTransforms(ownedScene);
        target.stem = stemOf(relative);
        target.directory = parentOf(relative);
        return true;
    }

    // a pose animation's clip on the target's root
    bool addPoseClip(Project* project, VatBakeTarget& target, const std::string& poseFile, std::vector<VatIssue>& issues, bool required){
        fs::path absolute;
        std::string relative, error;
        if (!projectFile(project, poseFile, absolute, relative, error)){
            issues.push_back(makeIssue(required ? "invalid_source" : "not_converted", required, error, "give an existing .poseanim"));
            return false;
        }
        std::shared_ptr<PoseAnimationDocument> document = PoseAnimationDocument::open(project, FileUtils::pathToUtf8(absolute), &error);
        if (!document){
            issues.push_back(makeIssue(required ? "invalid_source" : "not_converted", required, "'" + relative + "': " + error, "check the pose animation"));
            return false;
        }
        ClipData data;
        if (!PoseBaker::clipData(project, target.scene, target.root, *document, data, error)){
            issues.push_back(makeIssue(required ? "invalid_source" : "not_converted", required,
                                       "the pose animation '" + relative + "' can't play here: " + error, "play it on a humanoid model"));
            return false;
        }
        addHash(target, relative, absolute);
        addClip(target, stemOf(relative), relative, AnimationClip::build(data), ClipWrapMode::Default);
        return true;
    }

    // a GameObject's animations: player states, entity Animations, its Pose Animator
    void addEntityClips(Project* project, VatBakeTarget& target, std::vector<VatIssue>& issues){
        Scene* scene = target.scene;
        const Entity root = target.root;
        if (const AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(root)){
            const std::vector<std::string> names = anim::playerStateNames(player->clips);
            for (size_t i = 0; i < player->clips.size() && i < names.size(); i++){
                const AnimationPlayerClip& entry = player->clips[i];
                std::string error;
                std::shared_ptr<const AnimationClip> clip = AnimationClipPool::getOrLoad(entry.clip, &error);
                if (!clip){
                    issues.push_back(makeIssue("not_converted", false, "the Animation Player state '" + names[i] + "' (" + entry.clip +
                                               ") can't load: " + error, "fix the clip entry"));
                    continue;
                }
                addClip(target, names[i], entry.clip, clip, entry.wrap);
                // the .anim files it plays are sources too
                if (!anim::isSubClipReference(entry.clip)){
                    const fs::path file = project->resolveAssetPath(FileUtils::pathFromUtf8(entry.clip)).lexically_normal();
                    std::error_code ec;
                    if (fs::is_regular_file(file, ec)) addHash(target, FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file)), file);
                }
            }
        }

        // entity Animations: the model's, and the ones whose tracks animate this root
        std::vector<Entity> animations;
        if (const ModelComponent* model = scene->findComponent<ModelComponent>(root)){
            for (Entity animation : model->animations){
                if (animation != NULL_ENTITY && scene->isEntityCreated(animation) && scene->findComponent<AnimationComponent>(animation)){
                    animations.push_back(animation);
                }
            }
        }
        auto array = scene->getComponentArray<AnimationComponent>();
        for (size_t i = 0; i < array->size(); i++){
            const Entity animation = array->getEntity(i);
            if (std::find(animations.begin(), animations.end(), animation) != animations.end()) continue;
            if (ClipConverters::defaultConvertRoot(scene, animation) == root) animations.push_back(animation);
        }
        for (Entity animation : animations){
            ClipData data;
            ClipConvertReport report;
            std::string error;
            const std::string name = scene->getEntityName(animation);
            if (!ClipConverters::convertEntityAnimation(scene, animation, root, data, report, &error)){
                issues.push_back(makeIssue("not_converted", false, "the Animation '" + name + "' can't be converted: " + error,
                                           "convert it to a clip by hand (Animation window > Convert to Clip)"));
                continue;
            }
            if (!report.notConverted.empty()){
                std::string parts;
                for (const std::string& part : report.notConverted) parts += (parts.empty() ? "" : ", ") + part;
                issues.push_back(makeIssue("not_converted", false, "the Animation '" + name + "' has parts a clip can't hold: " + parts,
                                           "they don't appear in the bake"));
            }
            if (report.convertedTracks == 0) continue;
            addClip(target, name, "Animation '" + name + "'", AnimationClip::build(data), ClipWrapMode::Default);
        }

        if (const PoseAnimatorComponent* animator = scene->findComponent<PoseAnimatorComponent>(root)){
            if (!animator->animation.empty()){
                const fs::path file = project->resolveAssetPath(FileUtils::pathFromUtf8(animator->animation)).lexically_normal();
                addPoseClip(project, target, FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file)), issues, false);
            }
        }
    }

}

VatBakeTarget::VatBakeTarget() = default;

VatBakeTarget::~VatBakeTarget(){
    // the model handle first, then its private scene (PoseRigProvider's order)
    delete ownedModel;
    delete ownedScene;
}

int VatBakeTarget::findClip(const std::string& name) const{
    for (size_t i = 0; i < clips.size(); i++){
        if (clips[i].name == name) return (int)i;
    }
    return -1;
}

std::string fileSha1(const std::string& absolutePath){
    std::string content;
    if (!clipui::readWholeFile(FileUtils::pathFromUtf8(absolutePath), content)) return "";
    return "sha1:" + SHA1::hash(content);
}

bool resolveVatSource(Project* project, const VatSourceSpec& spec, VatBakeTarget& target, std::vector<VatIssue>& issues){
    if (!project){
        issues.push_back(makeIssue("invalid_source", true, "no project is open", "open a project"));
        return false;
    }
    target.project = project;
    const size_t before = issues.size();
    auto failed = [&](){
        for (size_t i = before; i < issues.size(); i++){
            if (issues[i].error) return true;
        }
        return false;
    };
    const bool gameObject = isGameObjectSource(spec);

    switch (spec.kind){
        case VatSourceKind::Model:{
            if (gameObject){
                issues.push_back(makeIssue("invalid_source", true, "a model source is a file, not a GameObject",
                                           "use kind entity for a GameObject"));
                return false;
            }
            // a prefab: its root's animations (Animation Player states, entity Animations, Pose
            // Animator), as a GameObject source would list them
            if (extensionOf(spec.path) == ".prefab"){
                if (!instantiateFile(project, spec.path, target, target.ownedScene, target.ownedModel, issues)) return false;
                addEntityClips(project, target, issues);
                break;
            }
            if (!anim::isModelFilePath(spec.path)){
                issues.push_back(makeIssue("invalid_source", true, "'" + spec.path + "' is not a model or prefab file",
                                           "give a .gltf, .glb, .fbx or .prefab"));
                return false;
            }
            if (!instantiateFile(project, spec.path, target, target.ownedScene, target.ownedModel, issues)) return false;
            fs::path absolute;
            std::string relative, error;
            projectFile(project, spec.path, absolute, relative, error);
            addModelClips(project, target, assetRelative(project, absolute), relative, {}, issues);
            break;
        }
        case VatSourceKind::Anim:{
            if (spec.anims.empty()){
                issues.push_back(makeIssue("no_animation", true, "no .anim file given", "list the clips to bake"));
                return false;
            }
            if (gameObject){
                if (!resolveGameObject(project, spec, target, issues)) return false;
            }else if (!instantiateFile(project, spec.path, target, target.ownedScene, target.ownedModel, issues)){
                return false;
            }
            for (const std::string& anim : spec.anims){
                fs::path absolute;
                std::string relative, error;
                if (!projectFile(project, anim, absolute, relative, error) || extensionOf(relative) != ".anim"){
                    issues.push_back(makeIssue("invalid_source", true, error.empty() ? "'" + anim + "' is not an .anim file" : error,
                                               "give .anim files inside the project"));
                    continue;
                }
                std::shared_ptr<const AnimationClip> clip = loadAnimFile(absolute, error);
                if (!clip){
                    issues.push_back(makeIssue("invalid_source", true, error, "fix the clip file"));
                    continue;
                }
                addHash(target, relative, absolute);
                addClip(target, stemOf(relative), relative, clip, ClipWrapMode::Default);
            }
            // one clip: named after it, next to it
            if (spec.anims.size() == 1 && !target.clips.empty()){
                target.stem = stemOf(spec.anims[0]);
                target.directory = parentOf(FileUtils::pathToGenericUtf8(FileUtils::pathFromUtf8(spec.anims[0])));
            }
            break;
        }
        case VatSourceKind::Entity:{
            if (!resolveGameObject(project, spec, target, issues)) return false;
            addEntityClips(project, target, issues);
            break;
        }
        case VatSourceKind::Pose:{
            if (spec.anims.size() != 1){
                issues.push_back(makeIssue("invalid_source", true, "a pose source takes one .poseanim (" + std::to_string(spec.anims.size()) + " given)",
                                           "give the pose animation to bake"));
                return false;
            }
            if (gameObject){
                if (!resolveGameObject(project, spec, target, issues)) return false;
            }else if (!instantiateFile(project, spec.path, target, target.ownedScene, target.ownedModel, issues)){
                return false;
            }
            if (!addPoseClip(project, target, spec.anims[0], issues, true)) return false;
            target.stem = stemOf(spec.anims[0]);
            target.directory = parentOf(spec.anims[0]);
            break;
        }
    }
    if (failed()) return false;
    if (target.clips.empty()){
        issues.push_back(makeIssue("no_animation", true, "the source has no animation to bake",
                                   spec.kind == VatSourceKind::Entity ? "give the GameObject an Animation Player, an Animation or a Pose Animator"
                                                                      : "choose a source with animations"));
        return false;
    }
    // the default output stays inside the assets folder: a source outside it (models/hero.gltf)
    // bakes to the same folder under assets/ (assets/models/hero.vat)
    const std::string assets = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(project->getAssetsPath()));
    if (!project->isInsideAssetsPath((project->getProjectPath() / FileUtils::pathFromUtf8(target.directory)).lexically_normal())){
        target.directory = target.directory.empty() ? assets : assets + "/" + target.directory;
    }
    return true;
}

bool vatSourceLoading(Project* project, const VatSourceSpec& spec){
    if (!project || !isGameObjectSource(spec)) return false;
    SceneProject* sceneProject = spec.sceneId != 0 ? project->getScene(spec.sceneId)
                                                   : (spec.scene.empty() ? nullptr : openScene(project, spec.scene));
    if (!sceneProject || !sceneProject->scene) return false;
    const Entity entity = EntityPath::find(*sceneProject, spec.path);
    return entity != NULL_ENTITY && loadingModel(sceneProject->id, sceneProject->scene, entity) != NULL_ENTITY;
}

// ---- materials ----

namespace{

    std::string decodeUri(const std::string& uri){
        std::string out;
        for (size_t i = 0; i < uri.size(); i++){
            if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit((unsigned char)uri[i + 1]) && std::isxdigit((unsigned char)uri[i + 2])){
                out += (char)std::stoi(uri.substr(i + 1, 2), nullptr, 16);
                i += 2;
            }else{
                out += uri[i];
            }
        }
        return out;
    }

    bool decodeBase64(const std::string& text, std::string& out){
        static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        out.clear();
        int value = 0, bits = -8;
        for (char c : text){
            if (c == '=') break;
            const size_t index = alphabet.find(c);
            if (index == std::string::npos){
                if (std::isspace((unsigned char)c)) continue;
                return false;
            }
            value = (value << 6) + (int)index;
            bits += 6;
            if (bits >= 0){
                out += (char)((value >> bits) & 0xff);
                bits -= 8;
            }
        }
        return true;
    }

    // the MIME type of an encoded image: the one given, else from its first bytes
    std::string imageMime(const std::string& given, const std::string& bytes){
        if (!given.empty()) return given;
        auto starts = [&](const char* magic, size_t size, size_t at = 0){
            return bytes.size() >= at + size && bytes.compare(at, size, magic, size) == 0;
        };
        if (starts("\x89PNG", 4)) return "image/png";
        if (starts("\xff\xd8", 2)) return "image/jpeg";
        if (starts("RIFF", 4) && starts("WEBP", 4, 8)) return "image/webp";
        if (starts("\xabKTX 20", 7)) return "image/ktx2";
        return "image/png";
    }

    // the glTF model files parsed for their images (no decoding), per material conversion run
    struct GltfImages{
        std::map<std::string, std::shared_ptr<tinygltf::Model>> models;

        tinygltf::Model* get(const std::string& file){
            auto it = models.find(file);
            if (it != models.end()) return it->second.get();
            auto model = std::make_shared<tinygltf::Model>();
            std::string error;
            if (!MeshSystem::parseGLTFFile(file, *model, &error)) model.reset();
            models[file] = model;
            return model.get();
        }
    };

    class MaterialConverter{
    public:
        Project* project;
        GltfImages images;
        std::vector<VatEmbeddedImage>& embedded;
        std::map<std::string, std::string> embeddedByKey;      // "<file>|<image>" -> placeholder
        std::vector<std::string> notes;                         // material_simplified messages
        std::vector<std::string> forks;                         // materials_not_vat_ready messages
        std::vector<std::string> files;                         // the texture files referenced (absolute)

        MaterialConverter(Project* project, std::vector<VatEmbeddedImage>& embedded): project(project), embedded(embedded){}

        void addFile(const std::string& file){
            if (std::find(files.begin(), files.end(), file) == files.end()) files.push_back(file);
        }

        // "" (none), an absolute file, or an embedded placeholder (VAT_EMBEDDED_TEXTURE + index)
        std::string texture(const Texture& texture, const std::string& material, const char* slot){
            if (texture.empty()) return "";
            const std::string& id = texture.getId();
            const std::string marker = "|gltf-image|";
            const size_t at = id.find(marker);
            if (at != std::string::npos){
                const std::string file = id.substr(0, at);
                const int image = std::atoi(id.c_str() + at + marker.size());
                tinygltf::Model* model = images.get(file);
                if (!model || image < 0 || (size_t)image >= model->images.size()){
                    notes.push_back(std::string("the ") + slot + " texture of '" + material + "' can't be found in " + file);
                    return "";
                }
                const tinygltf::Image& source = model->images[(size_t)image];
                // a file of its own (MeshSystem::parseGLTFFile reads no image file, its bytes stay
                // empty): referenced by its path
                if (source.image.empty() && source.bufferView < 0 && !source.uri.empty() && source.uri.compare(0, 5, "data:") != 0){
                    const std::string path = FileUtils::pathToUtf8((fs::path(FileUtils::pathFromUtf8(file)).parent_path() /
                                                                    FileUtils::pathFromUtf8(decodeUri(source.uri))).lexically_normal());
                    addFile(path);
                    return path;
                }
                // held inside the model (a buffer view, a data: URI): stored inside the .vat.glb
                const std::string key = file + "|" + std::to_string(image);
                auto known = embeddedByKey.find(key);
                if (known != embeddedByKey.end()) return known->second;
                std::string bytes;
                std::string mime = source.mimeType;
                if (!source.image.empty()){
                    // SetImagesAsIs: a buffer view's or a data: URI's bytes, still encoded
                    bytes.assign(source.image.begin(), source.image.end());
                }else if (source.bufferView >= 0 && (size_t)source.bufferView < model->bufferViews.size()){
                    const tinygltf::BufferView& view = model->bufferViews[(size_t)source.bufferView];
                    if (view.buffer >= 0 && (size_t)view.buffer < model->buffers.size() &&
                        view.byteOffset + view.byteLength <= model->buffers[(size_t)view.buffer].data.size()){
                        const unsigned char* data = model->buffers[(size_t)view.buffer].data.data() + view.byteOffset;
                        bytes.assign(reinterpret_cast<const char*>(data), view.byteLength);
                    }
                }else if (source.uri.compare(0, 5, "data:") == 0){
                    const size_t comma = source.uri.find(',');
                    const std::string header = source.uri.substr(5, comma == std::string::npos ? 0 : comma - 5);
                    if (comma != std::string::npos && header.find(";base64") != std::string::npos){
                        decodeBase64(source.uri.substr(comma + 1), bytes);
                        if (mime.empty()) mime = header.substr(0, header.find(';'));
                    }
                }
                if (bytes.empty()){
                    notes.push_back(std::string("the embedded ") + slot + " texture of '" + material + "' can't be read");
                    return "";
                }
                VatEmbeddedImage out;
                out.name = source.name.empty() ? material + " " + slot : source.name;
                out.mimeType = imageMime(mime, bytes);
                out.bytes = std::move(bytes);
                embedded.push_back(std::move(out));
                const std::string placeholder = std::string(VAT_EMBEDDED_TEXTURE) + std::to_string(embedded.size() - 1);
                embeddedByKey[key] = placeholder;
                return placeholder;
            }
            const std::string path = texture.getPath(0);
            if (path.empty()){
                notes.push_back(std::string("the ") + slot + " texture of '" + material + "' is generated in memory and is not kept");
                return "";
            }
            fs::path file = FileUtils::pathFromUtf8(path);
            if (!file.is_absolute()) file = project->resolveAssetPath(file);
            const std::string absolute = FileUtils::pathToUtf8(file.lexically_normal());
            addFile(absolute);
            return absolute;
        }

        // `sourceName`: the material's name in the model file it came from ("" when unknown)
        VatMaterialData convert(const Submesh& submesh, int index, const std::string& sourceName){
            const Material& material = submesh.material;
            VatMaterialData out;
            out.name = !material.name.empty() ? material.name : !sourceName.empty() ? sourceName : "Material " + std::to_string(index);
            out.baseColor[0] = material.baseColorFactor.x;
            out.baseColor[1] = material.baseColorFactor.y;
            out.baseColor[2] = material.baseColorFactor.z;
            out.baseColor[3] = material.baseColorFactor.w;
            out.metallic = material.metallicFactor;
            out.roughness = material.roughnessFactor;
            out.emissive[0] = material.emissiveFactor.x;
            out.emissive[1] = material.emissiveFactor.y;
            out.emissive[2] = material.emissiveFactor.z;
            out.alphaCutoff = material.alphaCutoff;
            switch (material.alphaMode){
                case MaterialAlphaMode::MASK: out.alphaMode = "MASK"; break;
                case MaterialAlphaMode::BLEND: out.alphaMode = "BLEND"; break;
                case MaterialAlphaMode::ALPHA_OPAQUE: out.alphaMode = "OPAQUE"; break;
                default: out.alphaMode = material.baseColorFactor.w < 1.0f ? "BLEND" : "OPAQUE"; break;
            }
            out.doubleSided = material.cullMode == MaterialCullMode::OFF || !submesh.faceCulling;
            out.unlit = material.lighting == MaterialLighting::NONE;
            out.foliageTransmission = material.foliageTransmission;
            out.baseColorTexture = texture(material.baseColorTexture, out.name, "base colour");
            out.normalTexture = texture(material.normalTexture, out.name, "normal");
            out.metallicRoughnessTexture = texture(material.metallicRoughnessTexture, out.name, "metallic-roughness");
            out.occlusionTexture = texture(material.occlusionTexture, out.name, "occlusion");
            out.emissiveTexture = texture(material.emissiveTexture, out.name, "emissive");
            if (!material.shader.empty()){
                if (material.shader.compare(0, 8, "library:") == 0){
                    notes.push_back("'" + out.name + "' uses the library shader " + material.shader.substr(8) +
                                    ": the .vat.glb keeps its PBR colours and textures only");
                }else{
                    forks.push_back("'" + out.name + "' uses the project shader '" + material.shader +
                                    "', which the .vat.glb can't carry: the VAT mesh draws with the built-in Lit material");
                }
            }
            if (out.unlit){
                notes.push_back("'" + out.name + "' draws unlit (Lighting None): the .vat.glb marks it KHR_materials_unlit, which the "
                                "engine loads lit: set Lighting to None on the placed object's material");
            }
            // what a glTF material has no slot for
            std::vector<std::string> lost;
            if (material.lighting == MaterialLighting::VERTEX) lost.push_back("Lighting Vertex");
            if (material.lighting == MaterialLighting::MIXED) lost.push_back("Lighting Mixed");
            if (material.stochasticTiling) lost.push_back("Stochastic Tiling");
            if (material.blendMode != MaterialBlendMode::AUTO) lost.push_back("its blend mode");
            if (material.cullMode == MaterialCullMode::FRONT) lost.push_back("front-face culling");
            if (material.depthWrite != MaterialDepthWrite::AUTO) lost.push_back("its depth write");
            if (material.depthTest != MaterialDepthTest::AUTO) lost.push_back("its depth test");
            if (material.depthOffsetFactor != 0.0f || material.depthOffsetUnits != 0.0f) lost.push_back("its depth offset");
            if (material.stencil.compare != MaterialStencilCompare::AUTO) lost.push_back("its stencil");
            if (material.colorWrite != -1) lost.push_back("its colour write mask");
            if (material.sortPriority != 0) lost.push_back("its sort priority (" + std::to_string(material.sortPriority) + ")");
            if (!material.extraPass->shader.empty()) lost.push_back("its extra pass (" + material.extraPass->shader + ")");
            if (!lost.empty()){
                std::string list;
                for (size_t i = 0; i < lost.size(); i++) list += (i ? ", " : "") + lost[i];
                notes.push_back("'" + out.name + "' loses " + list + ": a glTF material has no slot for it");
            }
            if (material.baseColorTexCoord != 0 || material.normalTexCoord != 0 || material.metallicRoughnessTexCoord != 0 ||
                material.occlusionTexCoord != 0 || material.emissiveTexCoord != 0){
                notes.push_back("'" + out.name + "' samples a texture with its second UV set: the .vat.glb uses the first");
            }
            return out;
        }

        // the name of the glTF material submesh `submesh` of `entity` came from: its model's file
        // (the nearest Model at or above it), its glTF node (meshNodesMapping, or the one mesh node
        // of a model its root draws) and that node's primitive; "" when unknown (merged models,
        // FBX imports, meshes made in the editor)
        std::string sourceName(Scene* scene, Entity entity, unsigned int submesh){
            Entity owner = entity;
            const ModelComponent* model = nullptr;
            for (int depth = 0; owner != NULL_ENTITY && depth < 65536; depth++){
                model = scene->findComponent<ModelComponent>(owner);
                if (model && !model->filename.empty()) break;
                model = nullptr;
                const Transform* transform = scene->findComponent<Transform>(owner);
                owner = transform ? transform->parent : NULL_ENTITY;
            }
            if (!model || model->mergeStaticMeshes) return "";
            const std::string extension = extensionOf(model->filename);
            if (extension != ".gltf" && extension != ".glb") return "";
            const std::string file = FileUtils::pathToUtf8(project->resolveAssetPath(FileUtils::pathFromUtf8(model->filename)).lexically_normal());
            tinygltf::Model* gltf = images.get(file);
            if (!gltf) return "";
            int node = -1;
            for (const auto& entry : model->meshNodesMapping){
                if (entry.second == entity) node = entry.first;
            }
            if (node < 0 && entity == owner){
                for (size_t n = 0; n < gltf->nodes.size(); n++){
                    if (gltf->nodes[n].mesh < 0) continue;
                    if (node >= 0) return "";       // several mesh nodes drawn by the root
                    node = (int)n;
                }
            }
            if (node < 0 || (size_t)node >= gltf->nodes.size()) return "";
            const int mesh = gltf->nodes[(size_t)node].mesh;
            if (mesh < 0 || (size_t)mesh >= gltf->meshes.size() || submesh >= gltf->meshes[(size_t)mesh].primitives.size()) return "";
            const int material = gltf->meshes[(size_t)mesh].primitives[submesh].material;
            if (material < 0 || (size_t)material >= gltf->materials.size()) return "";
            return gltf->materials[(size_t)material].name;
        }
    };

    bool isUnder(Scene* scene, Entity entity, Entity root){
        Entity current = entity;
        for (int depth = 0; current != NULL_ENTITY && depth < 65536; depth++){
            if (current == root) return true;
            const Transform* transform = scene->findComponent<Transform>(current);
            if (!transform) return false;
            current = transform->parent;
        }
        return false;
    }

}

// ---- poser ----

VatPoser::VatPoser() = default;

VatPoser::~VatPoser(){
    restore();
}

bool VatPoser::begin(VatBakeTarget& bakeTarget, const std::vector<std::string>& sockets, std::vector<VatIssue>& issues, std::string& error){
    target = &bakeTarget;
    Scene* scene = target->scene;
    cpuskin::updateTransforms(scene);
    LatticeRender::prepare(scene);
    const Transform* rootTransform = scene->findComponent<Transform>(target->root);
    if (!rootTransform){
        error = "the source root has no Transform";
        return false;
    }
    rootInverse = rootTransform->modelMatrix.inverse();

    // the renderers, in Transform order (parents first)
    MaterialConverter converter(target->project, embeddedImages);
    auto transforms = scene->getComponentArray<Transform>();
    bool instanced = false;
    for (size_t i = 0; i < transforms->size(); i++){
        const Entity entity = transforms->getEntity(i);
        if (!isUnder(scene, entity, target->root) || !scene->isEntityActiveInHierarchy(entity)) continue;
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || !mesh->enabled || mesh->numSubmeshes == 0) continue;
        if (scene->findComponent<InstancedMeshComponent>(entity)) instanced = true;
        VatBakeRenderer renderer;
        std::string path;
        renderer.name = anim::relativePath(scene, target->root, entity, path) ? path : scene->getEntityName(entity);
        if (renderer.name.empty()) renderer.name = scene->getEntityName(entity);
        std::string failure;
        if (!cpuskin::buildSource(scene, entity, renderer.source, &failure)){
            error = "'" + renderer.name + "': " + failure;
            return false;
        }
        const Transform& transform = scene->getComponent<Transform>(entity);
        renderer.restToRoot = rootInverse * transform.modelMatrix;
        for (int t = 0; t < MAX_MORPHTARGETS; t++) renderer.restMorph[(size_t)t] = mesh->morphWeights[t];
        if (!cpuskin::latticeInputs(*mesh, renderer.restLattice, &failure)){
            error = "'" + renderer.name + "': " + failure;
            return false;
        }
        for (const cpuskin::CpuSkinSubmesh& submesh : renderer.source.submeshes){
            int material = -1;
            if (submesh.submesh < mesh->numSubmeshes){
                materials.push_back(converter.convert(mesh->submeshes[submesh.submesh], (int)materials.size(),
                                                      converter.sourceName(scene, entity, submesh.submesh)));
                material = (int)materials.size() - 1;
            }
            renderer.submeshMaterial.push_back(material);
        }
        // its joints' entities and rest skin matrices (the bone estimate merges shared joints)
        std::vector<Entity> joints;
        auto models = scene->getComponentArray<ModelComponent>();
        for (size_t m = 0; m < models->size(); m++){
            for (const ModelSkinBinding& binding : models->getComponentFromIndex(m).skinBindings){
                if (binding.mesh == entity) joints = binding.joints;
            }
        }
        std::vector<Matrix4> restSkin;
        cpuskin::computeSkinMatrices(scene, entity, restSkin, nullptr);
        rendererJoints.push_back(joints);
        restSkins.push_back(restSkin);
        renderers.push_back(std::move(renderer));
        rendererEntities.push_back(entity);
    }
    if (renderers.empty()){
        VatIssue issue;
        issue.kind = "no_mesh";
        issue.error = true;
        issue.message = "nothing at or under '" + scene->getEntityName(target->root) + "' draws a mesh";
        issue.fix = "bake a GameObject or model with a mesh";
        issues.push_back(issue);
        return false;
    }
    if (instanced){
        warnings.push_back({"not_converted", "an Instanced Mesh under the source bakes its mesh once, not its copies",
                            "bake the single object, then place copies with crowd", false, {}});
    }
    // the texture files the materials read are sources too
    for (const std::string& file : converter.files){
        const fs::path absolute = FileUtils::pathFromUtf8(file);
        std::error_code ec;
        if (!fs::is_regular_file(absolute, ec)) continue;
        const std::string relative = FileUtils::pathToGenericUtf8(target->project->normalizeToProjectRelative(absolute));
        if (relative.compare(0, 2, "..") == 0 || FileUtils::pathFromUtf8(relative).is_absolute()) continue;
        addHash(*target, relative, absolute);
    }
    for (const std::string& note : converter.notes){
        warnings.push_back({"material_simplified", note, "set up the material on the placed VAT object if needed", false, {}});
    }
    for (const std::string& fork : converter.forks){
        warnings.push_back({"materials_not_vat_ready", fork, "re-fork the shader from the library (VAT-ready) and assign it to the VAT object", false, {}});
    }

    for (const std::string& socket : sockets){
        const Entity node = anim::resolvePath(scene, target->root, socket);
        if (node == NULL_ENTITY || !scene->findComponent<Transform>(node)){
            VatIssue issue;
            issue.kind = "socket_not_found";
            issue.error = true;
            issue.message = "no node '" + socket + "' under the source for a socket";
            issue.fix = "give a node path below the root (e.g. Armature/Hips/Spine/RightHand)";
            issues.push_back(issue);
            continue;
        }
        socketEntities.push_back(node);
    }
    return !hasErrors(issues);
}

bool VatPoser::capture(const VatSourceClip& clip, float time, VatSampleState& out, std::string& error){
    Scene* scene = target->scene;
    std::shared_ptr<ActionSystem> actions = scene->getSystem<ActionSystem>();
    ActionSystem::PlayerPreview preview;
    ActionSystem::PlayerPreviewState state;
    state.clip = clip.clip;
    state.time = time;
    state.wrap = ClipWrapMode::ClampForever;
    preview.states.push_back(state);
    // another clip starts from the rest: what the previous one wrote and this one doesn't
    // animate would stay posed
    if (posed && lastClip != clip.clip.get()) restore();
    if (session.root != target->root){
        session.reset();
        session.root = target->root;
    }
    actions->previewPlayer(target->root, preview, session);
    posed = true;
    lastClip = clip.clip.get();
    cpuskin::updateTransforms(scene);
    LatticeRender::prepare(scene);

    out.renderers.resize(rendererEntities.size());
    for (size_t r = 0; r < rendererEntities.size(); r++){
        const Entity entity = rendererEntities[r];
        VatRendererState& rendererState = out.renderers[r];
        std::string failure;
        if (!cpuskin::computeSkinMatrices(scene, entity, rendererState.skin, &failure)){
            error = "'" + renderers[r].name + "': " + failure;
            return false;
        }
        const MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        for (int t = 0; t < MAX_MORPHTARGETS; t++) rendererState.morph[(size_t)t] = mesh.morphWeights[t];
        if (!cpuskin::latticeInputs(mesh, rendererState.lattice, &failure)){
            error = "'" + renderers[r].name + "': " + failure;
            return false;
        }
        rendererState.toRoot = rootInverse * scene->getComponent<Transform>(entity).modelMatrix;
    }
    out.sockets.resize(socketEntities.size());
    for (size_t s = 0; s < socketEntities.size(); s++){
        out.sockets[s] = rootInverse * scene->getComponent<Transform>(socketEntities[s]).modelMatrix;
    }
    return true;
}

void VatPoser::restore(){
    if (!posed || !target || !target->scene) return;
    target->scene->getSystem<ActionSystem>()->restorePlayerPreview(session);
    session.reset();
    cpuskin::updateTransforms(target->scene);
    LatticeRender::prepare(target->scene);
    posed = false;
}

int VatPoser::estimatedVertices() const{
    int total = 0;
    for (const VatBakeRenderer& renderer : renderers){
        const cpuskin::CpuSkinSource& source = renderer.source;
        for (size_t v = 0; v < source.vertexCount(); v++){
            if (source.canonical[v] == v) total++;
        }
    }
    return total;
}

int VatPoser::estimatedBones(const std::set<Entity>& animated) const{
    // what the bake merges: candidates whose matrices are equal at every frame. A candidate no
    // animated transform moves (its joint, or a rigid renderer, and their ancestors up to the
    // root) keeps its rest matrix: those merge by that matrix; a joint shared by two renderers
    // with the same bind merges too
    Scene* scene = target ? target->scene : nullptr;
    // the nearest animated entity at or above `entity` (up to the root), NULL_ENTITY when none:
    // below it nothing moves on its own, so a joint's matrix follows that entity's (equal to it
    // when the joint's bind pose is its rest pose)
    auto mover = [&](Entity entity) -> Entity{
        for (int depth = 0; scene && entity != NULL_ENTITY && depth < 65536; depth++){
            if (animated.count(entity)) return entity;
            if (entity == target->root) return NULL_ENTITY;
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform) return NULL_ENTITY;
            entity = transform->parent;
        }
        return NULL_ENTITY;
    };
    struct Known{
        Entity joint;           // what moves it (NULL_ENTITY: nothing)
        Matrix4 rest;
    };
    std::vector<Known> known;
    auto sameMatrix = [](const Matrix4& a, const Matrix4& b){
        for (int c = 0; c < 4; c++){
            for (int r = 0; r < 3; r++){
                if (std::fabs(a.get(c, r) - b.get(c, r)) > 1e-4f) return false;
            }
        }
        return true;
    };
    int total = 0;
    auto add = [&](Entity joint, const Matrix4& rest){
        for (const Known& other : known){
            if (other.joint == joint && sameMatrix(other.rest, rest)) return;
        }
        known.push_back({joint, rest});
        total++;
    };
    for (size_t index = 0; index < renderers.size(); index++){
        const VatBakeRenderer& renderer = renderers[index];
        const cpuskin::CpuSkinSource& source = renderer.source;
        std::set<int> joints;
        bool rigid = false;
        for (size_t v = 0; v < source.vertexCount(); v++){
            const cpuskin::CpuSkinSubmesh& submesh = source.submeshOf((uint32_t)v);
            if (!submesh.hasSkinning){
                rigid = true;
                continue;
            }
            bool any = false;
            for (int k = 0; k < 4; k++){
                if (source.boneWeights[v][k] != 0.0f){
                    joints.insert(source.boneIds[v][k]);
                    any = true;
                }
            }
            if (!any) rigid = true;
        }
        const Matrix4 toRest = renderer.restToRoot.inverse();
        for (int joint : joints){
            const std::vector<Entity>& entities = rendererJoints[index];
            const std::vector<Matrix4>& skins = restSkins[index];
            if (joint < 0 || (size_t)joint >= entities.size() || (size_t)joint >= skins.size() || entities[(size_t)joint] == NULL_ENTITY){
                total++;
                continue;
            }
            const Matrix4 rest = renderer.restToRoot * skins[(size_t)joint] * toRest;
            add(mover(entities[(size_t)joint]), rest);
        }
        if (rigid) add(mover(rendererEntities[index]), Matrix4());
    }
    return total;
}

std::set<Entity> VatPoser::animatedEntities(const std::vector<const VatSourceClip*>& clips) const{
    std::set<Entity> out;
    if (!target || !target->scene) return out;
    for (const VatSourceClip* clip : clips){
        for (const AnimationClip::Track& track : clip->clip->getTracks()){
            if (!track.isSampled()) continue;
            if (track.channel != ClipChannel::Position && track.channel != ClipChannel::Rotation && track.channel != ClipChannel::Scale) continue;
            const Entity entity = anim::resolvePath(target->scene, target->root, track.path);
            if (entity != NULL_ENTITY) out.insert(entity);
        }
    }
    return out;
}

bool VatPoser::morphOrLattice() const{
    for (const VatBakeRenderer& renderer : renderers){
        if (!renderer.restLattice.bindings.empty()) return true;
        for (const cpuskin::CpuSkinSubmesh& submesh : renderer.source.submeshes){
            if (submesh.morphTargets > 0) return true;
        }
    }
    return false;
}

}
