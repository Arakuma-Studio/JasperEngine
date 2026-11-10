// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteNormalsApply.h"

#include "Project.h"
#include "Scene.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ActionComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "mcp/McpValues.h"
#include "texsynth/SynthImage.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include "util/FileUtils.h"

namespace fs = std::filesystem;

namespace jasperengine::editor::spritenormals {

    namespace {

        // One step of a scene history for everything applyToSprite adds (the WorldTools UndoStep
        // pattern): each command goes in with setNoMerge, commit() groups them, rollback()
        // undoes what was added when a later command fails
        class UndoStep {
        public:
            explicit UndoStep(uint32_t sceneId): history(CommandHandle::get(sceneId)), start(history->mark()) {}

            bool add(Command* command) {
                command->setNoMerge();
                const bool ok = history->addCommand(command);
                added = added || ok;
                return ok;
            }
            void commit() { history->groupSince(start); }
            // ... leaving nothing of it to redo; when nothing was added the history (and the
            // user's redo tail) is left as it was
            void rollback() {
                if (!added) return;
                while (history->position() > start && history->canUndo() && history->undo()) {}
                history->dropRedo();
            }

        private:
            CommandHistory* history;
            size_t start;
            bool added = false;
        };

        std::string normalized(const std::string& path) { return jasperengine::editor::FileUtils::pathToGenericUtf8(fs::u8path(path).lexically_normal()); }

        // A project-relative image's pixel size from its header
        bool imageSize(Project* project, const std::string& rel, int& width, int& height) {
            return !rel.empty() && texsynth::probeImage(project->getProjectPath() / fs::u8path(rel), width, height) && width > 0 && height > 0;
        }

        std::string sizeText(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

        // `stored` (an assets-relative texture path) with the sampling of `like`
        Texture textureLike(const std::string& stored, const Texture& like) {
            Texture texture(stored);
            texture.setMinFilter(like.getMinFilter());
            texture.setMagFilter(like.getMagFilter());
            texture.setWrapU(like.getWrapU());
            texture.setWrapV(like.getWrapV());
            return texture;
        }

    }

    namespace {

        void refuse(ApplyResult& result, const char* kind, const std::string& message, const std::string& fix) {
            result.ok = false;
            result.error = {kind, "", message, fix};
        }

        // One request's commands, added to `step` (the caller rolls the whole step back on false)
        bool applyOne(Project* project, SceneProject* sceneProject, const ApplyRequest& request, UndoStep& step, ApplyResult& result) {
            Scene* scene = sceneProject->scene;
            const Entity entity = request.entity;
            auto fail = [&](const char* kind, const std::string& message, const std::string& fix) {
                refuse(result, kind, message, fix);
                return false;
            };
            if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)) {
                return fail("not_a_sprite", "The GameObject " + std::to_string(entity) + " is not in the scene.", "Pick a Sprite GameObject of the scene.");
            }
            const std::string name = scene->getEntityName(entity);
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            // a Sprite's first submesh exists before its first update sets the count
            const unsigned submeshes = mesh ? std::max(mesh->numSubmeshes, scene->findComponent<SpriteComponent>(entity) ? 1u : 0u) : 0;
            if (submeshes == 0) {
                return fail("not_a_sprite", "\"" + name + "\" has no mesh to hold a normal map.", "Pick a Sprite (or a mesh) GameObject.");
            }
            // a normal map written already: its stored (assets-relative) path and its size
            auto readNormal = [&](const std::string& normalPath, std::string& stored, int& w, int& h) {
                std::string error;
                if (!normalPath.empty() && mcp::McpValues::assetPathFromJson(project, normalPath, stored, error) && imageSize(project, normalPath, w, h)) {
                    return true;
                }
                return fail("invalid_source", "The normal map " + (normalPath.empty() ? std::string("(none)") : normalPath) + " can't be read" +
                                                  (error.empty() ? std::string(".") : ": " + error),
                            "Write the normal map inside the project first.");
            };
            const bool animated = !request.atlasPath.empty();
            const ComponentType meshType = ComponentType::MeshComponent;
            std::unique_ptr<MultiPropertyCmd> multi(new MultiPropertyCmd());
            std::vector<int> given;

            if (!animated) {
                std::vector<ApplyRequest::Map> maps = request.maps;
                if (maps.empty()) maps.push_back({request.normalPath, request.colorSourcePath});
                for (const ApplyRequest::Map& map : maps) {
                    std::string storedNormal;
                    int normalW = 0, normalH = 0;
                    if (!readNormal(map.normalPath, storedNormal, normalW, normalH)) return false;
                    // every submesh drawing this map's colour source (a tilemap: its tileset's submeshes only)
                    const std::string wanted = normalized(map.colorSourcePath);
                    std::string first;
                    std::vector<int> matched;
                    for (unsigned i = 0; i < submeshes; i++) {
                        const Texture& base = mesh->submeshes[i].material.baseColorTexture;
                        const std::string rel = base.getPath().empty() ? std::string() : normalized(mcp::McpValues::assetPathToJson(project, base.getPath()));
                        if (first.empty()) first = rel;
                        if (!rel.empty() && rel == wanted) matched.push_back(static_cast<int>(i));
                    }
                    if (matched.empty()) {
                        return fail("not_a_sprite", "Its texture is " + (first.empty() ? std::string("none") : first) + ", not " + map.colorSourcePath + ".",
                                    "Apply the map to the GameObject drawing " + map.colorSourcePath + ", or generate from its own texture.");
                    }
                    int w = 0, h = 0;
                    if (imageSize(project, map.colorSourcePath, w, h) && (w != normalW || h != normalH)) {
                        return fail("size_mismatch",
                                    "The sprite's texture is " + sizeText(w, h) + " but the normal map is " + sizeText(normalW, normalH) +
                                        "; they must match pixel for pixel.",
                                    "Generate from the sprite's own texture (source: the GameObject).");
                    }
                    for (int i : matched) {
                        const Texture& base = mesh->submeshes[i].material.baseColorTexture;
                        multi->addPropertyCmd<Texture>(project, sceneProject->id, entity, meshType,
                                                       "submeshes[" + std::to_string(i) + "].material.normalTexture", textureLike(storedNormal, base));
                        if (std::find(given.begin(), given.end(), i) == given.end()) given.push_back(i);
                    }
                }
                if (result.undoName.empty()) result.undoName = "Assign Normal Map";
            } else {
                SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
                if (!sprite) {
                    return fail("not_a_sprite", "\"" + name + "\" is not a Sprite; an animation needs a Sprite GameObject.", "Pick a Sprite GameObject.");
                }
                const std::vector<Cell>& frames = request.frames;
                if (frames.size() > ARRAY_INDEX_LIMIT) {
                    return fail("too_many_frames",
                                "The animation has " + std::to_string(frames.size()) + " frames; a Sprite holds at most " + std::to_string(ARRAY_INDEX_LIMIT) + ".",
                                "Trim the animation, or use the atlas with your own frames.");
                }
                if (frames.empty()) return fail("frames_mismatch", "The animation has no frames.", "Generate from the GIF or APNG again.");
                std::string storedNormal, storedAtlas, error;
                int normalW = 0, normalH = 0, atlasW = 0, atlasH = 0;
                if (!readNormal(request.normalPath, storedNormal, normalW, normalH)) return false;
                if (!mcp::McpValues::assetPathFromJson(project, request.atlasPath, storedAtlas, error) || !imageSize(project, request.atlasPath, atlasW, atlasH)) {
                    return fail("invalid_source", "The colour atlas " + request.atlasPath + " can't be read" + (error.empty() ? std::string(".") : ": " + error),
                                "Write the atlas inside the project first.");
                }
                for (size_t i = 0; i < frames.size(); i++) {
                    const Cell& c = frames[i];
                    if (c.x < 0 || c.y < 0 || c.width < 1 || c.height < 1 || c.x + c.width > atlasW || c.y + c.height > atlasH) {
                        return fail("frames_mismatch",
                                    "Frame " + std::to_string(i) + " [" + std::to_string(c.x) + ", " + std::to_string(c.y) + ", " + std::to_string(c.width) +
                                        ", " + std::to_string(c.height) + "] is not inside the " + sizeText(atlasW, atlasH) + " atlas.",
                                    "Apply the frames the atlas was generated with.");
                    }
                }
                if (atlasW != normalW || atlasH != normalH) {
                    return fail("size_mismatch",
                                "The colour atlas is " + sizeText(atlasW, atlasH) + " but the normal map is " + sizeText(normalW, normalH) +
                                    "; they must match pixel for pixel.",
                                "Generate the atlas and its normal map together.");
                }
                const uint32_t sceneId = sceneProject->id;
                // the atlas as the base texture (sampling kept), the map with the same sampling
                const Texture base = mesh->submeshes[0].material.baseColorTexture;
                multi->addPropertyCmd<Texture>(project, sceneId, entity, meshType, "submeshes[0].material.baseColorTexture", textureLike(storedAtlas, base));
                multi->addPropertyCmd<Texture>(project, sceneId, entity, meshType, "submeshes[0].material.normalTexture", textureLike(storedNormal, base));
                given.push_back(0);
                // the frames (frame_0 ...), the old ones past them cleared
                const ComponentType spriteType = ComponentType::SpriteComponent;
                // a sheet sprite draws the colour atlas's frames from now on: its sheet is cleared
                if (!sprite->sheet.empty()) multi->addPropertyCmd<AssetRef>(project, sceneId, entity, spriteType, "sheet", AssetRef());
                for (unsigned i = static_cast<unsigned>(frames.size()); i < sprite->numFramesRect; i++) {
                    multi->addPropertyCmd<std::string>(project, sceneId, entity, spriteType, "framesRect[" + std::to_string(i) + "].name", std::string());
                    multi->addPropertyCmd<Vector4>(project, sceneId, entity, spriteType, "framesRect[" + std::to_string(i) + "].rect", Vector4(0, 0, 0, 0));
                }
                for (size_t i = 0; i < frames.size(); i++) {
                    const Cell& c = frames[i];
                    multi->addPropertyCmd<std::string>(project, sceneId, entity, spriteType, "framesRect[" + std::to_string(i) + "].name",
                                                       "frame_" + std::to_string(i));
                    multi->addPropertyCmd<Vector4>(project, sceneId, entity, spriteType, "framesRect[" + std::to_string(i) + "].rect",
                                                   Vector4(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.width), static_cast<float>(c.height)));
                }
                multi->addPropertyCmd<unsigned int>(project, sceneId, entity, spriteType, "numFramesRect", static_cast<unsigned int>(frames.size()));
                // one frame's size, frame 0 shown
                const Cell& f0 = frames[0];
                multi->addPropertyCmd<unsigned int>(project, sceneId, entity, spriteType, "width", static_cast<unsigned int>(f0.width));
                multi->addPropertyCmd<unsigned int>(project, sceneId, entity, spriteType, "height", static_cast<unsigned int>(f0.height));
                multi->addPropertyCmd<Vector4>(project, sceneId, entity, meshType, "submeshes[0].textureRect",
                                               Vector4(static_cast<float>(f0.x) / atlasW, static_cast<float>(f0.y) / atlasH,
                                                       static_cast<float>(f0.width) / atlasW, static_cast<float>(f0.height) / atlasH));
                result.undoName = "Assign Animated Sprite";
            }
            for (int i : given) {
                const std::string linked = project->getMaterialFilePath(sceneProject->id, entity, static_cast<unsigned int>(i));
                if (linked.empty()) continue;
                result.warnings.push_back({"material_linked", "", "The sprite's material comes from " + linked + "; the scene's copy changed, the file did not.",
                                           "Save the material from the Inspector to keep the map in " + linked + "."});
                break;
            }
            if (!step.add(multi.release())) return fail("failed", "The editor rejected the change to \"" + name + "\".", "Check the GameObject in the Inspector.");
            for (int i : given) result.submeshes.push_back(i);
            if (!animated) return true;

            // the Sprite Animation targeting the sprite, else a new child
            const uint32_t sceneId = sceneProject->id;
            Entity animation = NULL_ENTITY;
            auto animations = scene->getComponentArray<SpriteAnimationComponent>();
            for (size_t i = 0; animations && i < animations->size(); i++) {
                const Entity candidate = animations->getEntity(i);
                ActionComponent* action = scene->findComponent<ActionComponent>(candidate);
                if (action && action->target == entity) {
                    animation = candidate;
                    break;
                }
            }
            bool created = false;
            if (animation == NULL_ENTITY) {
                auto* create = new CreateEntityCmd(project, sceneId, request.animationName.empty() ? std::string("Sprite Animation") : request.animationName,
                                                   EntityCreationType::SPRITE_ANIMATION, entity);
                if (!step.add(create)) return fail("failed", "Could not create the Sprite Animation GameObject.", "Try again.");
                animation = create->getEntity();
                created = true;
            }
            const SpriteAnimationComponent& anim = scene->getComponent<SpriteAnimationComponent>(animation);
            const ComponentType animType = ComponentType::SpriteAnimationComponent;
            auto* props = new MultiPropertyCmd();
            const size_t n = request.frames.size();
            const size_t old = std::max<size_t>(anim.framesSize, anim.framesTimeSize);
            for (size_t i = 0; i < std::max(n, old); i++) {
                const int ms = i < request.frameMs.size() ? std::max(1, request.frameMs[i]) : 100;
                props->addPropertyCmd<int>(project, sceneId, animation, animType, "frames[" + std::to_string(i) + "]", i < n ? static_cast<int>(i) : 0);
                props->addPropertyCmd<int>(project, sceneId, animation, animType, "framesTime[" + std::to_string(i) + "]", i < n ? ms : 0);
            }
            props->addPropertyCmd<unsigned int>(project, sceneId, animation, animType, "framesSize", static_cast<unsigned int>(n));
            props->addPropertyCmd<unsigned int>(project, sceneId, animation, animType, "framesTimeSize", static_cast<unsigned int>(n));
            // loop count 1: play once; 0 (forever) and more loop (a count is not kept)
            props->addPropertyCmd<bool>(project, sceneId, animation, animType, "loop", request.loopCount != 1);
            if (scene->getComponent<ActionComponent>(animation).target != entity) {
                props->addPropertyCmd<Entity>(project, sceneId, animation, ComponentType::ActionComponent, "target", entity);
            }
            // autoplay: a created animation runs (set_sprite_animation's autoplay, the Action's state)
            if (created) props->addPropertyCmd<ActionState>(project, sceneId, animation, ComponentType::ActionComponent, "state", ActionState::Running);
            if (!step.add(props)) return fail("failed", "The editor rejected the Sprite Animation edit.", "Check the GameObject in the Inspector.");
            result.animation = animation;
            result.createdAnimation = result.createdAnimation || created;
            return true;
        }

    }

    ApplyResult applyToSprites(Project* project, const std::vector<ApplyRequest>& requests) {
        ApplyResult result;
        auto fail = [&](const char* kind, const std::string& message, const std::string& fix) {
            ApplyResult failed;
            refuse(failed, kind, message, fix);
            return failed;
        };
        if (!project) return fail("not_a_sprite", "No project is open.", "Open the project first.");
        if (requests.empty()) return fail("not_a_sprite", "Nothing to apply.", "Give the Sprite and its normal map.");
        const uint32_t sceneId = requests[0].sceneId;
        for (const ApplyRequest& r : requests) {
            if (r.sceneId != sceneId) {
                return fail("not_a_sprite", "The maps are for scenes " + std::to_string(sceneId) + " and " + std::to_string(r.sceneId) + "; one step holds one scene.",
                            "Apply each scene's maps separately.");
            }
        }
        SceneProject* sceneProject = project->getScene(sceneId);
        if (!sceneProject || !sceneProject->scene) {
            return fail("not_a_sprite", "Scene " + std::to_string(sceneId) + " is not open.", "Open the scene of the Sprite.");
        }
        if (sceneProject->playState != ScenePlayState::STOPPED) {
            return fail("scene_playing", "Scene " + std::to_string(sceneId) + " is playing; stop play mode before assigning a normal map.",
                        "Stop the scene, then apply again.");
        }
        UndoStep step(sceneId);
        for (const ApplyRequest& request : requests) {
            ApplyResult one;
            if (!applyOne(project, sceneProject, request, step, one)) {
                // all or nothing: what the earlier requests added goes too
                step.rollback();
                ApplyResult failed;
                failed.error = one.error;
                return failed;
            }
            result.submeshes.insert(result.submeshes.end(), one.submeshes.begin(), one.submeshes.end());
            result.warnings.insert(result.warnings.end(), one.warnings.begin(), one.warnings.end());
            if (one.animation != NULL_ENTITY) {
                result.animation = one.animation;
                result.createdAnimation = result.createdAnimation || one.createdAnimation;
            }
            if (result.undoName.empty() || one.undoName == "Assign Animated Sprite") result.undoName = one.undoName;
        }
        if (requests.size() > 1 && result.undoName == "Assign Normal Map") result.undoName = "Assign Normal Maps (" + std::to_string(requests.size()) + ")";
        step.commit();
        sceneProject->isModified = true;
        result.ok = true;
        return result;
    }

    ApplyResult applyToSprite(Project* project, const ApplyRequest& request) {
        return applyToSprites(project, {request});
    }

}
