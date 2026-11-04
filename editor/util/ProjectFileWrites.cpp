// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProjectFileWrites.h"

#include "App.h"
#include "Backend.h"
#include "Catalog.h"
#include "Out.h"
#include "Project.h"
#include "DocumentFile.h"
#include "Stream.h"
#include "animation/AnimationClipDocument.h"
#include "command/CommandHistory.h"
#include "pool/TextureDataPool.h"
#include "pool/TexturePool.h"
#include "pose/PoseAnimationDocument.h"
#include "spritenormals/SpriteNormalsWatcher.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"
#include "texture/Texture.h"
#include "util/FileUtils.h"
#include "util/Util.h"
#include "vat/VatDocument.h"
#include "window/ProjectWindow.h"

#include <memory>
#include <set>
#include <system_error>
#include <utility>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    // A WriteFilesCmd whose every execute (redo included) and undo asks the guard first; a redo
    // or an undo the guard (or WriteFilesCmd's own beneath-root check) refuses is not offered
    // (canRedo / canUndo: the history keeps its place and says why, CommandHistory::lastRefusal)
    class GuardedWriteFilesCmd : public editor::WriteFilesCmd {
    public:
        GuardedWriteFilesCmd(std::vector<Item> items, std::function<void(const std::vector<fs::path>&)> afterChange,
                             editor::ProjectFileWrites::Guard guard, std::shared_ptr<std::string> failure)
            : WriteFilesCmd(std::move(items), std::move(afterChange)), guard(std::move(guard)), sink(std::move(failure)) {
            reportFailureTo(sink);
        }

        bool execute() override {
            std::string why;
            if (!guard(why)) {
                *sink = why;
                refused = why;
                return false;
            }
            refused.clear();
            return WriteFilesCmd::execute();
        }

        std::string failure() const override { return refused.empty() ? WriteFilesCmd::failure() : refused + " Remove the link, then redo again."; }

        std::string redoRefusal() const override {
            std::string why;
            if (!guard(why)) return why + " Remove the link, then redo again.";
            return WriteFilesCmd::redoRefusal();
        }

        std::string undoRefusal() const override {
            std::string why;
            if (!guard(why)) return why + " Remove the link, then undo again.";
            return WriteFilesCmd::undoRefusal();
        }

        void undo() override {
            // checked by canUndo first; a group's undo (CommandGroup) asks every step's canUndo too.
            // Refused here after all (a link made between the check and this): nothing removed,
            // and said
            std::string why;
            if (!guard(why)) {
                undoProblem += (undoProblem.empty() ? "" : "; ") + why + " Remove the link, then undo again.";
                return;
            }
            WriteFilesCmd::undo();
        }

    private:
        editor::ProjectFileWrites::Guard guard;
        std::shared_ptr<std::string> sink;
        std::string refused;
    };

    editor::ProjectFileWrites::SyncHook& syncHook() {
        static editor::ProjectFileWrites::SyncHook hook;
        return hook;
    }

    // Project-relative generic path, the absolute path when outside the project (as the MCP
    // tools name files: mcp::w4::projectRelative)
    std::string projectRelative(editor::Project* project, const fs::path& absolute) {
        const fs::path rel = absolute.lexically_normal().lexically_relative(project->getProjectPath().lexically_normal());
        if (rel.empty() || *rel.begin() == "..") return editor::FileUtils::pathToGenericUtf8(absolute);
        if (rel == ".") return "";
        return editor::FileUtils::pathToGenericUtf8(rel);
    }

    // A texture's stored path as a project-relative generic path (mcp::McpValues::assetPathToJson)
    std::string textureKey(editor::Project* project, const std::string& stored) {
        if (stored.empty()) return editor::FileUtils::pathToGenericUtf8(fs::path(stored));
        return editor::FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(project->resolveAssetPath(stored)));
    }

    // The textures of the open scenes and prefab stages reading one of `images` (project-relative)
    // released: they load the new pixels when next drawn. The count released.
    int reloadTextures(editor::Project* project, const std::set<std::string>& images) {
        if (images.empty()) return 0;
        int count = 0;
        auto matches = [&](const Texture& texture) {
            const std::string path = texture.getPath(0);
            if (path.empty()) return false;
            return images.count(textureKey(project, path)) > 0;
        };
        auto reload = [&](Texture& texture) {
            texture.invalidateRender();
            TextureDataPool::remove(texture.getId());
            texture.destroy();
            count++;
        };
        for (editor::SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            bool touched = false;
            for (Entity entity : sceneProject->entities) {
                for (editor::ComponentType component : editor::Catalog::findComponents(sceneProject->scene, entity)) {
                    for (auto& [name, property] : editor::Catalog::findEntityProperties(sceneProject->scene, entity, component)) {
                        if (!property.ref) continue;
                        if (property.type == editor::PropertyType::Texture) {
                            Texture* texture = static_cast<Texture*>(property.ref);
                            if (matches(*texture)) {
                                reload(*texture);
                                touched = true;
                            }
                        } else if (property.type == editor::PropertyType::Material) {
                            Material* material = static_cast<Material*>(property.ref);
                            for (Texture* texture : {&material->baseColorTexture, &material->emissiveTexture,
                                                     &material->metallicRoughnessTexture, &material->occlusionTexture,
                                                     &material->normalTexture}) {
                                if (matches(*texture)) {
                                    reload(*texture);
                                    touched = true;
                                }
                            }
                        } else if (property.type == editor::PropertyType::Custom && name.size() >= 14 &&
                                   name.compare(name.size() - 14, 14, "shaderTextures") == 0) {
                            // shader library textures, of a submesh material or a component; which
                            // textures it binds is resolved when it loads
                            bool reloaded = false;
                            for (auto& shaderTexture : *static_cast<std::vector<std::pair<std::string, Texture>>*>(property.ref)) {
                                if (matches(shaderTexture.second)) {
                                    reload(shaderTexture.second);
                                    reloaded = true;
                                }
                            }
                            if (reloaded) {
                                editor::Catalog::updateEntity(sceneProject->scene, entity, property.updateFlags);
                                touched = true;
                            }
                        }
                    }
                }
            }
            // the scene's post-processing images (phase 4): the user passes' textures and the LUT
            // strip, which the renderer caches by path
            bool postImages = false;
            std::vector<PostProcessPass> passes = sceneProject->scene->getPostProcessPasses();
            for (PostProcessPass& pass : passes) {
                for (auto& entry : pass.textures) {
                    if (matches(entry.second)) {
                        // the GPU texture is held by the chain's copies, not by the scene's: the
                        // pool entry is marked stale for them to rebuild it from the new pixels
                        TexturePool::invalidate(entry.second.getId());
                        reload(entry.second);
                        postImages = true;
                    }
                }
            }
            // the scene keeps the released textures (no pixels held), so the chain's reload below
            // reads the file again; same paths, so this is no edit (and no undo step)
            if (postImages) sceneProject->scene->setPostProcessPasses(passes);
            const std::string lut = sceneProject->scene->getPostProcessing().lutTexture;
            if (!lut.empty() && images.count(textureKey(project, lut)) > 0) {
                count++;
                postImages = true;
            }
            if (postImages) {
                if (std::shared_ptr<RenderSystem> render = sceneProject->scene->getSystem<RenderSystem>())
                    render->invalidatePostProcessTextures();
                touched = true;
            }

            if (touched) sceneProject->needUpdateRender = true;
        }
        return count;
    }

}

editor::ProjectFileWrites::Refreshed editor::ProjectFileWrites::refresh(Project* project, const std::vector<fs::path>& changedIn, bool force) {
    Refreshed refreshed;
    if (!project) return refreshed;
    std::set<fs::path> changed;
    for (const fs::path& path : changedIn) changed.insert(path.lexically_normal());
    const std::vector<fs::path> changedPaths(changed.begin(), changed.end());

    bool anyShader = false, anyMaterial = false;
    std::set<std::string> images;
    for (const fs::path& path : changed) {
        const std::string rel = projectRelative(project, path);
        if (Util::isShaderFile(rel)) anyShader = true;
        if (Util::isMaterialFile(rel)) anyMaterial = true;
        if (Util::isImageFile(rel)) images.insert(rel);
    }

    // Pose animation documents (editor/pose) of changed .poseanim files reload; so do clip
    // documents (a changed model: its sub-clips), and AnimationClipPool drops the clips so the
    // Animation Players using them reload (edit preview and play alike); .vat documents too,
    // VertexAnimationPool drops the changed ones and their Vertex Animators reload
    PoseAnimationDocument::filesChanged(changedPaths);
    AnimationClipDocument::filesChanged(changedPaths, project);
    VatDocument::filesChanged(changedPaths, project);
    // the project's audio mixer is heard as the file now says (Edit mode and Play alike)
    project->audioFilesChanged(changedPaths);

    if (anyMaterial || force) {
        // the linked .material files written (every one when forced) read again into their
        // submeshes (oe-port's Project::applyLinkedMaterialFile; upstream's refreshLinkedMaterials)
        for (const std::string& linked : project->getLinkedMaterialFiles()) {
            const fs::path path = (project->getProjectPath() / linked).lexically_normal();
            if (!force && changed.count(path) == 0) continue;
            try {
                project->applyLinkedMaterialFile(linked, Stream::decodeMaterial(DocumentFiles::read(path, DocumentKind::Material).root));
            } catch (const std::exception& e) {
                Out::error("%s", e.what());
            }
        }
    }
    if (anyShader || force) project->invalidateCustomShaders();
    refreshed.shadersInvalidated = anyShader;
    refreshed.texturesReloaded = reloadTextures(project, images);

    project->refreshAssetDatabase(true);
    App& app = Backend::getApp();
    if (app.getProject() == project) {
        if (ProjectWindow* window = app.getProjectWindow()) {
            for (const fs::path& path : changed) window->notifyResourceFileChanged(path);
            window->refreshCurrentDirectory();
        }
    }
    // the watcher reads a changed image again soon (a new generated map wakes a dormant one)
    if (SpriteNormalsWatcher* watcher = SpriteNormalsWatcher::active()) {
        for (const fs::path& path : changed) {
            if (Util::isImageFile(path.string())) watcher->invalidate(path);
        }
    }
    return refreshed;
}

void editor::ProjectFileWrites::filesWritten(Project* project, const std::vector<fs::path>& changed) {
    if (!project) return;
    if (const SyncHook& hook = syncHook(); hook && hook(project, changed)) return;
    refresh(project, changed, false);
}

void editor::ProjectFileWrites::setSyncHook(SyncHook hook) {
    syncHook() = std::move(hook);
}

bool editor::ProjectFileWrites::writeUndoable(Project* project, std::vector<WriteFilesCmd::Item> items, std::string& error,
                                              std::function<void(const std::vector<fs::path>&)> afterChange, Guard guard,
                                              fs::path beneath, bool ownFolders, const std::string& stepName) {
    if (!project) {
        error = "No project is open to write into.";
        return false;
    }
    for (const WriteFilesCmd::Item& item : items) {
        std::error_code ec;
        if (fs::is_directory(item.path, ec)) {
            error = FileUtils::pathToGenericUtf8(item.path) + " is a directory.";
            return false;
        }
    }
    if (!afterChange) {
        afterChange = [project](const std::vector<fs::path>& changed) { filesWritten(project, changed); };
    }
    auto failure = std::make_shared<std::string>();
    WriteFilesCmd* cmd = guard ? new GuardedWriteFilesCmd(std::move(items), std::move(afterChange), std::move(guard), failure)
                               : new WriteFilesCmd(std::move(items), std::move(afterChange));
    cmd->setNoMerge();
    cmd->reportFailureTo(failure);
    if (!stepName.empty()) cmd->setName(stepName);
    // every write, removal and restore beneath `beneath` when it happens; else beneath the write
    // scope it is made in (an MCP call: the project, Command::writeRoot), else (`ownFolders`) each
    // file beneath its own folder
    if (!beneath.empty()) cmd->writeBeneath(beneath);
    else if (ownFolders && cmd->writeRoot().empty()) cmd->writeBeneathOwnFolders();
    // addCommand runs execute() and deletes the command when it fails (execute then changed nothing)
    if (!project->getProjectCommandHistory()->addCommand(cmd)) {
        error = "Writing the file(s) failed: " +
                (failure->empty() ? std::string("permissions, a read-only disk, or a parent path that is a file") : *failure) + ".";
        return false;
    }
    return true;
}
