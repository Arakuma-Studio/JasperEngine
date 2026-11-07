// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AsepriteSpriteSetup.h"

#include "Catalog.h"
#include "Out.h"
#include "Project.h"
#include "mcp/McpRefs.h"
#include "mcp/McpValues.h"
#include "command/type/ClipSceneActions.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/PropertyCmd.h"

#include "component/AnimationPlayerComponent.h"
#include "component/MeshComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/SpriteComponent.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"

#include <algorithm>
#include <filesystem>
#include <cctype>

namespace jasperengine::editor::pixelart::aseprite {

    std::shared_ptr<const ImportRecord> importedRecord(const std::string& assetPath, PixelProblem& problem) {
        AsepriteImporter& importer = AsepriteImporter::get();
        if (!importer.ensureImport(assetPath)) {
            problem = {"import_failed", "sheet", assetPath + " is still importing", "call again once get_asset_info shows its outputs"};
            return nullptr;
        }
        std::shared_ptr<const ImportRecord> record = importer.record(assetPath);
        if (!record) {
            problem = {"import_failed", "sheet", assetPath + " is not an Aseprite file of this project's assets",
                       "pass an asset-relative .aseprite path (list_assets finds them)"};
            return nullptr;
        }
        if (!record->ok) {
            problem = record->error;
            problem.field = "sheet";
            problem.message = assetPath + " did not import: " + record->error.message;
            return nullptr;
        }
        return record;
    }

    std::string defaultTagClip(const std::vector<std::string>& names) {
        for (const std::string& name : names) {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == "idle") return name;
        }
        return names.empty() ? std::string() : names.front();
    }

    bool sheetSpriteCommands(Project* project, uint32_t sceneId, Entity entity, const SheetSpriteRequest& request,
                             std::vector<Command*>& commands, SheetSpriteResult& result, PixelProblem& problem) {
        Scene* scene = project->getScene(sceneId)->scene;
        const bool aseprite = SpriteSheetPool::isAsepritePath(request.sheet);
        if (aseprite) {
            result.record = importedRecord(request.sheet, problem);
            if (!result.record) return false;
        }
        result.sheet = SpriteSheetPool::get(request.sheet);
        if (!result.sheet || result.sheet->frames.empty()) {
            problem = {"not_a_sheet", "sheet", request.sheet + " is not a sprite sheet with frames",
                       "pass an .aseprite file or a texture of the project's assets"};
            return false;
        }
        const int count = static_cast<int>(result.sheet->frames.size());
        if (!request.frameName.empty()) {
            result.frame = result.sheet->findFrame(request.frameName);
            if (result.frame < 0) {
                std::string names;
                for (int i = 0; i < count && i < 20; i++) names += (i ? ", " : "") + result.sheet->frames[static_cast<size_t>(i)].name;
                problem = {"unknown_frame", "frame", "the sheet has no frame \"" + request.frameName + "\"",
                           "frames are: " + names + (count > 20 ? ", ..." : "") + " (or an index below " + std::to_string(count) + ")"};
                return false;
            }
        } else {
            result.frame = request.frame;
            if (result.frame < 0 || result.frame >= count) {
                problem = {"unknown_frame", "frame", "frame " + std::to_string(result.frame) + " is out of range",
                           "pass an index below " + std::to_string(count) + " or a frame name"};
                return false;
            }
        }
        result.frameName = result.sheet->frames[static_cast<size_t>(result.frame)].name;

        const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
        if (!sprite) {
            problem = {"not_a_sheet", "entity", "\"" + scene->getEntityName(entity) + "\" is not a Sprite", "pick a Sprite or create one"};
            return false;
        }
        const ComponentType cp = ComponentType::SpriteComponent;
        if (sprite->sheet.path != request.sheet) {
            commands.push_back(new PropertyCmd<AssetRef>(project, sceneId, entity, cp, "sheet", project->makeAssetRef(request.sheet, AssetPathBase::Assets)));
        }
        if (sprite->sheet.empty()) {
            // a sprite turning into a sheet sprite samples its pages as pixel art (R15): Nearest,
            // the page itself coming from the sheet
            const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            Texture sampler;
            if (mesh && mesh->numSubmeshes > 0) {
                sampler.setWrapU(mesh->submeshes[0].material.baseColorTexture.getWrapU());
                sampler.setWrapV(mesh->submeshes[0].material.baseColorTexture.getWrapV());
            }
            sampler.setMinFilter(TextureFilter::NEAREST);
            sampler.setMagFilter(TextureFilter::NEAREST);
            commands.push_back(new PropertyCmd<Texture>(project, sceneId, entity, ComponentType::MeshComponent,
                                                        "submeshes[0].material.baseColorTexture", sampler));
        }
        commands.push_back(new PropertyCmd<int>(project, sceneId, entity, cp, "frame", result.frame));
        commands.push_back(new PropertyCmd<std::string>(project, sceneId, entity, cp, "frameName", result.frameName));
        if (request.resize && (sprite->width != 0 || sprite->height != 0)) {
            commands.push_back(new PropertyCmd<unsigned int>(project, sceneId, entity, cp, "width", 0u));
            commands.push_back(new PropertyCmd<unsigned int>(project, sceneId, entity, cp, "height", 0u));
        }

        if (!aseprite || !request.clips || result.record->clips.empty()) return true;
        std::vector<AnimationPlayerClip> entries;
        std::vector<std::string> names;
        for (const ImportRecord::Clip& clip : result.record->clips) {
            AnimationPlayerClip entry;
            entry.clip = result.record->assetPath + "#" + clip.name;
            entries.push_back(entry);
            names.push_back(clip.name);
            result.clips.push_back(entry.clip);
        }
        ClipActionResult player;
        ClipSceneActions::appendPlayerCommands(project, sceneId, entity, entries, commands, player);
        result.addedPlayer = player.addedPlayer;
        if (player.addedPlayer) {
            // a new player plays the idle (or first) tag on start, as a dropped character should
            result.defaultClip = defaultTagClip(names);
            const ComponentType pc = ComponentType::AnimationPlayerComponent;
            commands.push_back(new PropertyCmd<std::string>(project, sceneId, entity, pc, "defaultClip", result.defaultClip));
            commands.push_back(new PropertyCmd<bool>(project, sceneId, entity, pc, "playAutomatically", true));
        }
        return true;
    }

    bool sliceColliderCommands(Project* project, uint32_t sceneId, Entity entity, std::vector<Command*>& commands,
                               std::vector<std::string>& names, PixelProblem& problem) {
        Scene* scene = project->getScene(sceneId)->scene;
        const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
        if (!sprite || sprite->sheet.empty()) {
            problem = {"not_a_sheet", "entity", "\"" + scene->getEntityName(entity) + "\" is not a sheet sprite", "give it a sheet first"};
            return false;
        }
        std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get(sprite->sheet.path);
        if (!sheet || sheet->slices.empty()) {
            problem = {"unknown_slice", "slice", sprite->sheet.path + " has no slices", "add slices in Aseprite (the Slice tool) and save"};
            return false;
        }
        std::vector<Collider2D> colliders;
        const Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);
        if (body) {
            colliders = body->colliders;
        } else {
            commands.push_back(new AddComponentCmd(project, sceneId, entity, ComponentType::Rigidbody2DComponent));
        }
        for (const SpriteSlice& slice : sheet->slices) {
            const bool followed = std::any_of(colliders.begin(), colliders.end(), [&](const Collider2D& c) { return c.slice == slice.name; });
            if (followed) continue;
            Collider2D collider;
            collider.type = Collider2DType::POLYGON;
            collider.slice = slice.name;
            const nlohmann::json& props = slice.userData.is_object() ? slice.userData.value("properties", nlohmann::json()) : nlohmann::json();
            collider.sensor = props.is_object() && props.contains("sensor") && props["sensor"].is_boolean() && props["sensor"].get<bool>();
            colliders.push_back(collider);
            names.push_back(slice.name);
        }
        if (names.empty()) {
            problem = {"unknown_slice", "slice", "every slice of " + sprite->sheet.path + " already has a collider", ""};
            for (Command* c : commands) delete c;
            commands.clear();
            return false;
        }
        commands.push_back(new PropertyCmd<std::vector<Collider2D>>(project, sceneId, entity, ComponentType::Rigidbody2DComponent, "colliders", colliders));
        return true;
    }

    bool applySpriteProperties(Project* project, uint32_t sceneId, Entity entity, const SpriteSheetData& sheet,
                               const std::function<bool(Command*)>& run, std::vector<std::string>& applied,
                               std::vector<PixelProblem>& problems) {
        const nlohmann::json& ud = sheet.userData;
        if (!ud.is_object() || !ud.contains("properties") || !ud["properties"].is_object()) return true;
        Scene* scene = project->getScene(sceneId)->scene;
        for (const auto& [key, value] : ud["properties"].items()) {
            const size_t dot = key.find('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 >= key.size()) continue;   // data only
            ComponentType type;
            std::string error;
            if (!mcp::McpRefs::resolveComponent(key.substr(0, dot), type, error)) continue;   // not a component: data only
            const std::string property = key.substr(dot + 1);
            const bool has = (scene->getSignature(entity) & Catalog::componentTypeToSignature(scene, type)).any();
            if (!has &&
                !run(new AddComponentCmd(project, sceneId, entity, type))) {
                problems.push_back({"invalid_settings", key, "cannot add " + Catalog::getComponentName(type, true), ""});
                return false;
            }
            const PropertyData data = Catalog::findProperty(scene, entity, type, property);
            if (!data.ref) {
                problems.push_back({"invalid_settings", key, Catalog::getComponentName(type, true) + " has no property " + property,
                                    "rename the user property in Aseprite (Sprite Properties)"});
                continue;
            }
            Command* command = mcp::McpValues::buildPropertyCommand(project, sceneId, entity, type, property, data, value, error);
            if (!command) {
                problems.push_back({"invalid_settings", key, error, ""});
                continue;
            }
            if (!run(command)) return false;
            applied.push_back(key);
        }
        return true;
    }

    bool runAsOneStep(uint32_t sceneId, std::vector<Command*> commands) {
        CommandHistory* history = CommandHandle::get(sceneId);
        const size_t mark = history->mark();
        for (size_t i = 0; i < commands.size(); i++) {
            commands[i]->setNoMerge();
            if (!history->addCommand(commands[i])) {
                // the history deleted the refused one
                for (size_t j = i + 1; j < commands.size(); j++) delete commands[j];
                while (history->position() > mark && history->canUndo() && history->undo()) {}
                return false;
            }
        }
        history->groupSince(mark);
        return true;
    }

    Entity createSpriteFromAseprite(Project* project, uint32_t sceneId, const std::string& assetPath, Entity parent,
                                    const Vector3& position, PixelProblem& problem) {
        std::shared_ptr<const ImportRecord> record = importedRecord(assetPath, problem);
        if (!record) return NULL_ENTITY;
        std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get(assetPath);
        if (!sheet || sheet->frames.empty()) {
            problem = {"not_a_sheet", "asset", assetPath + " imported no frames", "check its layers in the Aseprite Importer"};
            return NULL_ENTITY;
        }
        const bool layered = record->settings.value("import_mode", std::string()) == "layers_as_sprites";
        const std::string name = std::filesystem::path(assetPath).stem().string();
        CommandHistory* history = CommandHandle::get(sceneId);
        const size_t mark = history->mark();
        auto fail = [&](const PixelProblem& why) {
            while (history->position() > mark && history->canUndo() && history->undo()) {}
            problem = why;
            return NULL_ENTITY;
        };
        auto run = [&](Command* command) {
            command->setNoMerge();
            return history->addCommand(command);
        };

        auto* create = new CreateEntityCmd(project, sceneId, name.empty() ? std::string("Sprite") : name,
                                           layered ? EntityCreationType::EMPTY : EntityCreationType::SPRITE, parent);
        create->addProperty<Vector3>(ComponentType::Transform, "position", position);
        if (!run(create)) return fail({"import_failed", "asset", "the GameObject could not be created", ""});
        const Entity root = create->getEntity();

        // the sprites: the root, or one child per layer (frames "<layer>/<i>", in layer order)
        std::vector<std::pair<std::string, int>> layers;   // child name, its first frame
        if (layered) {
            for (int i = 0; i < sheet->frameCount(); i++) {
                const std::string& frameName = sheet->frames[static_cast<size_t>(i)].name;
                const size_t slash = frameName.rfind('/');
                std::string layer = slash == std::string::npos ? frameName : frameName.substr(0, slash);
                std::replace(layer.begin(), layer.end(), '/', '_');
                if (layers.empty() || layers.back().first != layer) layers.emplace_back(layer, i);
            }
        }
        auto setUp = [&](Entity sprite, int frame, bool clips) -> bool {
            SheetSpriteRequest request;
            request.sheet = assetPath;
            request.frame = frame;
            request.clips = clips;
            std::vector<Command*> commands;
            SheetSpriteResult result;
            if (!sheetSpriteCommands(project, sceneId, sprite, request, commands, result, problem)) {
                for (Command* c : commands) delete c;
                return false;
            }
            commands.push_back(new PropertyCmd<PivotPreset>(project, sceneId, sprite, ComponentType::SpriteComponent, "pivotPreset", PivotPreset::FRAME));
            for (size_t i = 0; i < commands.size(); i++) {
                if (!run(commands[i])) {
                    for (size_t j = i + 1; j < commands.size(); j++) delete commands[j];
                    return false;
                }
            }
            return true;
        };
        if (!layered) {
            if (!setUp(root, 0, true)) return fail(problem);
        } else {
            for (const auto& [layer, first] : layers) {
                auto* child = new CreateEntityCmd(project, sceneId, layer, EntityCreationType::SPRITE, root);
                if (!run(child)) return fail({"import_failed", "asset", "the layer child " + layer + " could not be created", ""});
                if (!setUp(child->getEntity(), first, false)) return fail(problem);
            }
            // the clips' tracks address the layer children by name: the root plays them
            std::vector<AnimationPlayerClip> entries;
            std::vector<std::string> names;
            for (const ImportRecord::Clip& clip : record->clips) {
                AnimationPlayerClip entry;
                entry.clip = record->assetPath + "#" + clip.name;
                entries.push_back(entry);
                names.push_back(clip.name);
            }
            if (!entries.empty()) {
                std::vector<Command*> commands;
                ClipActionResult player;
                ClipSceneActions::appendPlayerCommands(project, sceneId, root, entries, commands, player);
                commands.push_back(new PropertyCmd<std::string>(project, sceneId, root, ComponentType::AnimationPlayerComponent, "defaultClip", defaultTagClip(names)));
                commands.push_back(new PropertyCmd<bool>(project, sceneId, root, ComponentType::AnimationPlayerComponent, "playAutomatically", true));
                for (size_t i = 0; i < commands.size(); i++) {
                    if (!run(commands[i])) {
                        for (size_t j = i + 1; j < commands.size(); j++) delete commands[j];
                        return fail({"import_failed", "asset", "the Animation Player could not be set up", ""});
                    }
                }
            }
        }
        std::vector<std::string> applied;
        std::vector<PixelProblem> problems;
        if (!applySpriteProperties(project, sceneId, root, *sheet, run, applied, problems)) {
            return fail(problems.empty() ? PixelProblem{"invalid_settings", "properties", "a sprite property could not be applied", ""} : problems.back());
        }
        history->groupSince(mark);
        return root;
    }

    Entity dropAseprite(Project* project, uint32_t sceneId, const std::string& assetPath, Entity target, Entity parent,
                        const Vector3& position) {
        if (!AsepriteImporter::get().ensureImport(assetPath)) {
            // the drop completes once the file is imported (the editor never waits on it)
            Out::info("%s is importing: it drops in when its import is done", assetPath.c_str());
            AsepriteImporter::get().whenImported(assetPath, [project, sceneId, assetPath, target, parent, position]() {
                SceneProject* sceneProject = project->getScene(sceneId);
                if (!sceneProject || !sceneProject->scene || sceneProject->playState != ScenePlayState::STOPPED) return;
                const Entity stillTarget = target != NULL_ENTITY && sceneProject->scene->isEntityCreated(target) ? target : NULL_ENTITY;
                const Entity stillParent = parent != NULL_ENTITY && sceneProject->scene->isEntityCreated(parent) ? parent : NULL_ENTITY;
                if (AsepriteImporter::get().ensureImport(assetPath)) dropAseprite(project, sceneId, assetPath, stillTarget, stillParent, position);
            });
            return NULL_ENTITY;
        }
        Scene* scene = project->getScene(sceneId)->scene;
        PixelProblem problem;
        Entity result = NULL_ENTITY;
        if (target != NULL_ENTITY && scene->findComponent<SpriteComponent>(target)) {
            SheetSpriteRequest request;
            request.sheet = assetPath;
            std::vector<Command*> commands;
            SheetSpriteResult setup;
            if (sheetSpriteCommands(project, sceneId, target, request, commands, setup, problem)) {
                result = runAsOneStep(sceneId, std::move(commands)) ? target : NULL_ENTITY;
                if (!result) problem = {"import_failed", "asset", "the sprite edit was refused", ""};
            } else {
                for (Command* c : commands) delete c;
            }
        } else if (target != NULL_ENTITY && scene->findComponent<AnimationPlayerComponent>(target)) {
            std::shared_ptr<const ImportRecord> record = importedRecord(assetPath, problem);
            if (record) {
                std::vector<AnimationPlayerClip> entries;
                for (const ImportRecord::Clip& clip : record->clips) {
                    AnimationPlayerClip entry;
                    entry.clip = record->assetPath + "#" + clip.name;
                    entries.push_back(entry);
                }
                std::vector<Command*> commands;
                ClipActionResult player;
                ClipSceneActions::appendPlayerCommands(project, sceneId, target, entries, commands, player);
                result = runAsOneStep(sceneId, std::move(commands)) ? target : NULL_ENTITY;
                if (!result) problem = {"import_failed", "asset", "the Animation Player edit was refused", ""};
            }
        } else {
            result = createSpriteFromAseprite(project, sceneId, assetPath, parent, position, problem);
        }
        if (result == NULL_ENTITY) {
            Out::warning("%s: %s%s%s", assetPath.c_str(), problem.message.c_str(), problem.fix.empty() ? "" : ". ", problem.fix.c_str());
        }
        return result;
    }

}
