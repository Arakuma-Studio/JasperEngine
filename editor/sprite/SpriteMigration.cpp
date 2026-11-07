// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteMigration.h"
#include "SpriteAtlasBuilder.h"
#include "SpriteSheetBuilder.h"

#include "Catalog.h"
#include "Out.h"
#include "util/Prefab.h"
#include "PrefabStage.h"
#include "Project.h"
#include "Stream.h"
#include "asset/AssetFileWrite.h"
#include "command/type/SetAssetMetaCmd.h"
#include "util/EntityClipboard.h"
#include "util/Util.h"

#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "subsystem/MeshSystem.h"
#include "sprite/SpriteSheetPool.h"

#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace jasperengine::editor::sprite {

    namespace {

        using pixelart::PixelProblem;

        std::string normalAssetPath(const std::string& path) {
            std::string p = fs::path(path).lexically_normal().generic_string();
            if (p.rfind("asset://", 0) == 0) p = p.substr(8);
            return p;
        }

        bool imageSizeOf(const fs::path& file, int& w, int& h) {
            int comp = 0;
            return stbi_info(file.string().c_str(), &w, &h, &comp) != 0;
        }

        // the frame a texture rect shows: like the clip binding's comparison
        int shownFrame(const Rect& shown, const std::vector<SpriteFrameData>& pixelFrames, int w, int h) {
            for (size_t i = 0; i < pixelFrames.size(); i++) {
                Rect r = pixelFrames[i].rect;
                Rect s = shown;
                if (s.isNormalized() && w > 0 && h > 0) s = Rect(s.getX() * w, s.getY() * h, s.getWidth() * w, s.getHeight() * h);
                const float eps = 0.01f;
                if (std::fabs(r.getX() - s.getX()) < eps && std::fabs(r.getY() - s.getY()) < eps &&
                    std::fabs(std::fabs(r.getWidth()) - std::fabs(s.getWidth())) < eps && std::fabs(std::fabs(r.getHeight()) - std::fabs(s.getHeight())) < eps) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        void scanEntities(const YAML::Node& list, const std::string& parentPath, const std::string& owner, const fs::path& assetsRoot,
                          std::vector<LegacySprite>& out) {
            if (!list || !list.IsSequence()) return;
            for (const YAML::Node& entity : list) {
                if (!entity.IsMap()) continue;
                const std::string name = entity["name"] ? entity["name"].as<std::string>("") : std::string();
                const std::string path = parentPath.empty() ? name : parentPath + "/" + name;
                const YAML::Node components = entity["components"];
                if (components && components.IsMap() && components["sprite"] && components["mesh"]) {
                    const YAML::Node spriteNode = components["sprite"];
                    if (!spriteNode["sheet"] && spriteNode["framesRect"] && spriteNode["framesRect"].IsSequence() && spriteNode["framesRect"].size() > 0) {
                        const SpriteComponent sprite = Stream::decodeSpriteComponent(spriteNode);
                        const YAML::Node submesh = components["mesh"]["submeshes"] && components["mesh"]["submeshes"].IsSequence() &&
                                                   components["mesh"]["submeshes"].size() > 0 ? components["mesh"]["submeshes"][0] : YAML::Node();
                        const YAML::Node texture = submesh && submesh["material"] ? submesh["material"]["baseColorTexture"] : YAML::Node();
                        LegacySprite legacy;
                        legacy.owner = owner;
                        legacy.entity = entity["entity"] ? entity["entity"].as<uint64_t>(0) : 0;
                        legacy.entityPath = path;
                        legacy.texture = texture && texture["path"] ? normalAssetPath(texture["path"].as<std::string>("")) : std::string();
                        for (unsigned i = 0; i < sprite.numFramesRect; i++) legacy.frames.push_back(sprite.framesRect[i]);
                        legacy.shown = -1;
                        if (submesh && submesh["textureRect"]) {
                            int w = 0, h = 0;
                            if (!assetsRoot.empty() && !legacy.texture.empty()) imageSizeOf(assetsRoot / legacy.texture, w, h);
                            const Rect shown = Stream::decodeRect(submesh["textureRect"]);
                            legacy.shown = shownFrame(shown, legacyPixelFrames(legacy.frames, w, h), w, h);
                        }
                        out.push_back(std::move(legacy));
                    }
                }
                scanEntities(entity["children"], path, owner, assetsRoot, out);
            }
        }

        // the closed file's sprite node of an entity
        bool findEntityYaml(const YAML::Node& list, uint64_t id, YAML::Node& out) {
            if (!list || !list.IsSequence()) return false;
            for (const YAML::Node& entity : list) {
                if (!entity.IsMap()) continue;
                if (entity["entity"] && entity["entity"].as<uint64_t>(0) == id) {
                    out.reset(entity);
                    return true;
                }
                if (findEntityYaml(entity["children"], id, out)) return true;
            }
            return false;
        }

        struct Loaded {
            SceneProject* scene = nullptr;
            Prefab* prefab = nullptr;
            EntityRegistry* registry() const { return scene ? scene->scene : prefab ? prefab->registry.get() : nullptr; }
        };

        Loaded loadedOwner(Project& project, const std::string& owner) {
            Loaded loaded;
            for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
                if (!sceneProject->scene) continue;
                const PrefabStage* stage = project.getPrefabStage(sceneProject->id);
                const fs::path path = stage ? stage->assetPath : sceneProject->filepath;
                if (!path.empty() && project.normalizeToProjectRelative(path).generic_string() == owner) {
                    loaded.scene = sceneProject;
                    return loaded;
                }
            }
            if (Util::isPrefabFile(owner)) {
                Prefab* prefab = project.getPrefab(fs::path(owner));
                if (prefab && prefab->registry) loaded.prefab = prefab;
            }
            return loaded;
        }

        std::string ownerOf(Project& project, SceneProject* sceneProject) {
            const PrefabStage* stage = project.getPrefabStage(sceneProject->id);
            const fs::path path = stage ? stage->assetPath : sceneProject->filepath;
            return path.empty() ? std::string() : project.normalizeToProjectRelative(path).generic_string();
        }

        void legacyOfLoaded(Project& project, EntityRegistry* registry, Entity entity, const std::string& owner, LegacySprite& out) {
            const SpriteComponent& sprite = registry->getComponent<SpriteComponent>(entity);
            const MeshComponent& mesh = registry->getComponent<MeshComponent>(entity);
            out = LegacySprite();
            out.owner = owner;
            out.entity = entity;
            out.entityPath = registry->getEntityName(entity);
            out.texture = normalAssetPath(MeshSystem::spriteSourceTexture(sprite, mesh));
            for (unsigned i = 0; i < sprite.numFramesRect; i++) out.frames.push_back(sprite.framesRect[i]);
            int w = 0, h = 0;
            imageSizeOf(project.getAssetsPath() / out.texture, w, h);
            out.shown = shownFrame(MeshSystem::spriteSourceRect(sprite, mesh), legacyPixelFrames(out.frames, w, h), w, h);
            out.open = true;
        }

        // the sheet sprite a legacy one becomes (the component's other fields kept)
        void applyToComponent(SpriteComponent& sprite, const std::string& texture, const Guid& guid, int frame, const std::string& frameName) {
            sprite.sheet = AssetRef(texture, guid);
            sprite.frame = std::max(0, frame);
            sprite.frameName = frameName;
            sprite.framesRect.clear();
            sprite.numFramesRect = 0;
            sprite.sheetData.reset();
            sprite.builtFrame = -1;
            sprite.resolveFrameName = false;
            sprite.needUpdateSprite = true;
            sprite.inlineAtlas.reset();
        }

    }

    std::vector<SpriteFrameData> legacyPixelFrames(const std::vector<SpriteFrameData>& frames, int w, int h) {
        std::vector<SpriteFrameData> out = frames;
        for (SpriteFrameData& f : out) {
            if (f.rect.isNormalized() && w > 0 && h > 0) {
                f.rect = Rect(std::round(f.rect.getX() * w), std::round(f.rect.getY() * h), std::round(f.rect.getWidth() * w), std::round(f.rect.getHeight() * h));
            }
        }
        return out;
    }

    bool sameRects(const std::vector<SpriteFrameData>& a, const std::vector<SpriteFrameData>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            Rect ra = a[i].rect;
            if (ra != b[i].rect) return false;
        }
        return true;
    }

    TextureSheetSettings sheetFromFrames(const std::string& texture, const std::vector<SpriteFrameData>& pixelFrames) {
        TextureSheetSettings sheet;
        sheet.mode = TextureSpriteMode::MULTIPLE;
        sheet.pivotPreset = PivotPreset::CENTER;   // inline frames had no pivot: their centre
        const std::string stem = fs::path(texture).stem().string();
        for (size_t i = 0; i < pixelFrames.size(); i++) {
            TextureSheetFrame f;
            f.name = pixelFrames[i].name.empty() ? stem + "_" + std::to_string(i) : pixelFrames[i].name;
            f.rect = pixelFrames[i].rect;
            sheet.frames.push_back(f);
        }
        return sheet;
    }

    std::vector<LegacySprite> scanSceneFile(const fs::path& file, const std::string& owner, const fs::path& assetsRoot) {
        std::vector<LegacySprite> out;
        try {
            const YAML::Node root = YAML::LoadFile(file.string());
            if (!root.IsMap()) return out;
            scanEntities(root["entities"], std::string(), owner, assetsRoot, out);
            scanEntities(root["members"], std::string(), owner, assetsRoot, out);
        } catch (const std::exception&) {
            return out;
        }
        return out;
    }

    SpriteCheckReport scanProject(const fs::path& projectRoot, const fs::path& assetsRoot) {
        SpriteCheckReport report;
        std::error_code ec;
        std::vector<fs::path> atlases;
        std::vector<fs::path> documents;
        for (auto it = fs::recursive_directory_iterator(projectRoot, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const std::string name = it->path().filename().string();
            if (it->is_directory(ec) && (name.empty() || name[0] == '.' || name == "build")) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            const std::string p = it->path().string();
            if (Util::isSceneFile(p) || Util::isPrefabFile(p)) documents.push_back(it->path());
            else if (SpriteSheetSources::isAtlasPath(p)) atlases.push_back(it->path());
        }
        std::sort(documents.begin(), documents.end());
        std::sort(atlases.begin(), atlases.end());
        for (const fs::path& d : documents) {
            const std::vector<LegacySprite> found = scanSceneFile(d, d.lexically_relative(projectRoot).generic_string(), assetsRoot);
            report.legacy.insert(report.legacy.end(), found.begin(), found.end());
            report.files++;
        }
        std::map<std::string, std::vector<std::string>> packers;
        for (const fs::path& a : atlases) {
            report.atlases++;
            const std::string atlas = a.lexically_relative(assetsRoot).generic_string();
            PixelProblem problem;
            std::optional<SpriteAtlasDocument> doc = SpriteAtlasBuilder::readDocument(a, &problem);
            if (!doc) {
                report.atlasErrors.emplace_back(atlas, problem);
                continue;
            }
            std::set<std::string> members;
            for (const SpriteAtlasMember& m : doc->members) {
                if (m.isFolder()) {
                    if (!fs::is_directory(assetsRoot / m.folder, ec)) {
                        report.atlasErrors.emplace_back(atlas, PixelProblem{"missing_member", m.folder, "the folder member " + m.folder + " is missing", "add the folder at its new path"});
                        continue;
                    }
                    for (auto it = fs::recursive_directory_iterator(assetsRoot / m.folder, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                        if (ec) break;
                        const std::string rel = it->path().lexically_relative(assetsRoot).generic_string();
                        const bool texture = SpriteSheetSources::isTexturePath(rel), aseprite = SpriteSheetPool::isAsepritePath(rel);
                        if (!it->is_regular_file(ec) || (!texture && !aseprite)) continue;
                        if (!m.types.empty() && std::find(m.types.begin(), m.types.end(), texture ? "texture" : "aseprite_sprite") == m.types.end()) continue;
                        members.insert(rel);
                    }
                } else if (!fs::is_regular_file(assetsRoot / m.path, ec)) {
                    report.atlasErrors.emplace_back(atlas, PixelProblem{"missing_member", m.path, "the member " + m.path + " is missing", "restore it or remove it from the atlas"});
                } else {
                    members.insert(m.path);
                }
            }
            for (const std::string& e : doc->exclude) {
                for (auto it = members.begin(); it != members.end();) {
                    if (*it == e || it->rfind(e + "/", 0) == 0) it = members.erase(it);
                    else ++it;
                }
            }
            // a frame larger than the max size (textures: their sheet's frames)
            const int maxSize = doc->settings.maxSize;
            const int margin = 2 * doc->settings.extrude + doc->settings.padding * 2;
            for (const std::string& m : members) {
                if (doc->includeInBuild) packers[m].push_back(atlas);
                if (!SpriteSheetSources::isTexturePath(m)) continue;
                int w = 0, h = 0;
                if (!imageSizeOf(assetsRoot / m, w, h)) continue;
                const TextureSheetInfo info = textureSheetSettings(assetsRoot / m);
                int fw = w, fh = h;
                if (info.settings.mode == TextureSpriteMode::GRID) {
                    fw = info.settings.grid.cellWidth;
                    fh = info.settings.grid.cellHeight;
                } else if (info.settings.mode == TextureSpriteMode::MULTIPLE) {
                    fw = fh = 0;
                    for (const TextureSheetFrame& f : info.settings.frames) {
                        Rect r = f.rect;
                        fw = std::max(fw, static_cast<int>(r.getWidth()));
                        fh = std::max(fh, static_cast<int>(r.getHeight()));
                    }
                }
                if (fw + margin > maxSize || fh + margin > maxSize) {
                    report.atlasErrors.emplace_back(atlas, PixelProblem{"frame_too_large", m, m + " has a " + std::to_string(fw) + "x" + std::to_string(fh) +
                                                                        " frame, more than the max size " + std::to_string(maxSize) + " holds", "raise max_size or split the frame"});
                }
            }
        }
        for (const auto& [member, list] : packers) {
            if (list.size() < 2) continue;
            for (const std::string& atlas : list) {
                report.atlasWarnings.emplace_back(atlas, PixelProblem{"double_packed", member, member + " is packed by " + std::to_string(list.size()) +
                                                                      " atlases; " + list[0] + " draws it", "remove it from all but one atlas"});
            }
        }
        return report;
    }

    std::vector<LegacySprite> findLegacySprites(Project& project) {
        std::vector<LegacySprite> out;
        std::set<std::string> openOwners;
        for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            const std::string owner = ownerOf(project, sceneProject);
            openOwners.insert(owner);
            EntityRegistry* registry = sceneProject->scene;
            for (Entity entity : registry->getEntityList()) {
                const SpriteComponent* sprite = registry->findComponent<SpriteComponent>(entity);
                if (!sprite || !sprite->sheet.empty() || sprite->numFramesRect == 0 || !registry->findComponent<MeshComponent>(entity)) continue;
                LegacySprite legacy;
                legacyOfLoaded(project, registry, entity, owner, legacy);
                out.push_back(std::move(legacy));
            }
        }
        project.getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
            if ((info.type != AssetType::Scene && info.type != AssetType::Prefab) || openOwners.count(info.path)) return;
            std::vector<LegacySprite> found = scanSceneFile(project.getProjectPath() / info.path, info.path, project.getAssetsPath());
            out.insert(out.end(), found.begin(), found.end());
        });
        return out;
    }

    bool legacySpriteOf(Project& project, uint32_t sceneId, Entity entity, LegacySprite& out, PixelProblem& problem) {
        SceneProject* sceneProject = project.getScene(sceneId);
        if (!sceneProject || !sceneProject->scene) {
            problem = {"not_a_sheet", "scene", "no open scene " + std::to_string(sceneId), ""};
            return false;
        }
        EntityRegistry* registry = sceneProject->scene;
        const SpriteComponent* sprite = registry->findComponent<SpriteComponent>(entity);
        if (!sprite || !registry->findComponent<MeshComponent>(entity)) {
            problem = {"not_a_sheet", "sprite", "the GameObject is not a Sprite", ""};
            return false;
        }
        if (!sprite->sheet.empty() || sprite->numFramesRect == 0) {
            problem = {"not_a_sheet", "convert", "the Sprite has no inline frames to convert", "it already draws a sheet (sheet, frame)"};
            return false;
        }
        legacyOfLoaded(project, registry, entity, ownerOf(project, sceneProject), out);
        return true;
    }

    bool planConversion(Project& project, const LegacySprite& sprite, bool allUsers, ConvertPlan& plan, PixelProblem& problem) {
        plan = ConvertPlan();
        if (sprite.texture.empty()) {
            problem = {"not_a_sheet", "texture", "the Sprite has no texture to hold its frames", "set its texture first"};
            return false;
        }
        const fs::path absolute = project.getAssetsPath() / sprite.texture;
        int w = 0, h = 0;
        if (!imageSizeOf(absolute, w, h)) {
            problem = {"not_a_sheet", "texture", "the texture " + sprite.texture + " does not read", "fix or replace the texture"};
            return false;
        }
        const std::vector<SpriteFrameData> rects = legacyPixelFrames(sprite.frames, w, h);
        plan.texture = sprite.texture;
        plan.sheet = sheetFromFrames(sprite.texture, rects);
        // the texture's own sheet: the same frames, or a conflict
        const TextureSheetInfo info = textureSheetSettings(absolute);
        if (info.readOnly) {
            problem = {"sheet_conflict", "texture", sprite.texture + "'s Sprite Mode is a newer version", "keep the sprite inline (set_sprite_frames inline: true)"};
            return false;
        }
        if (info.isSheet()) {
            TextureSheetBuild built = buildTextureSheet(absolute, sprite.texture, info.settings);
            std::vector<SpriteFrameData> existing;
            for (const SpriteFrame& f : built.sheet.frames) existing.push_back({f.name, f.rect});
            if (!sameRects(existing, rects)) {
                problem = {"sheet_conflict", "texture", sprite.texture + " already has a " + std::string(spriteModeName(info.settings.mode)) + " sheet of " +
                                                        std::to_string(existing.size()) + " frames that differ from the sprite's " + std::to_string(rects.size()),
                           "keep the sprite inline (set_sprite_frames inline: true), or slice the texture to the sprite's frames first"};
                return false;
            }
            plan.sheet = info.settings;
        }
        plan.targets.push_back(sprite);
        if (!allUsers) return true;
        std::set<std::pair<std::string, uint64_t>> seen = {{sprite.owner, sprite.entity}};
        for (const LegacySprite& other : findLegacySprites(project)) {
            if (other.texture != sprite.texture || seen.count({other.owner, other.entity})) continue;
            if (!sameRects(legacyPixelFrames(other.frames, w, h), rects)) continue;
            seen.insert({other.owner, other.entity});
            plan.targets.push_back(other);
        }
        return true;
    }

    ConvertInlineFramesCmd::ConvertInlineFramesCmd(Project* project, ConvertPlan plan, std::shared_ptr<ConvertReport> report)
        : project(project), plan(std::move(plan)), report(report ? report : std::make_shared<ConvertReport>()) {}

    ConvertInlineFramesCmd::~ConvertInlineFramesCmd() = default;

    std::string ConvertInlineFramesCmd::describe() const {
        return "Convert " + std::to_string(plan.targets.size()) + (plan.targets.size() == 1 ? " Sprite" : " Sprites") + " to Sprite Sheet " +
               fs::path(plan.texture).filename().string();
    }

    bool ConvertInlineFramesCmd::execute() {
        undoData.clear();
        report->filesWritten.clear();
        report->converted = 0;
        const fs::path textureAbsolute = project->getAssetsPath() / plan.texture;
        const Guid guid = project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(textureAbsolute));
        meta = std::make_unique<SetAssetMetaCmd>(project, project->normalizeToProjectRelative(textureAbsolute), kSpriteSheetKey, sheetSettingsToYaml(plan.sheet));
        if (meta->isNoOp()) {
            meta.reset();
        } else if (!meta->execute()) {
            report->error = "couldn't write the Sprite Mode of " + plan.texture;
            meta.reset();
            return false;
        }
        std::vector<std::string> names;
        for (const TextureSheetFrame& f : plan.sheet.frames) names.push_back(f.name);
        std::map<std::string, std::vector<const LegacySprite*>> byOwner;
        for (const LegacySprite& t : plan.targets) byOwner[t.owner].push_back(&t);
        for (const auto& [owner, sprites] : byOwner) {
            OwnerUndo entry;
            entry.owner = owner;
            const Loaded loaded = loadedOwner(*project, owner);
            if (EntityRegistry* registry = loaded.registry()) {
                entry.wasModified = loaded.scene ? loaded.scene->isModified : loaded.prefab->isModified;
                for (const LegacySprite* s : sprites) {
                    const Entity entity = static_cast<Entity>(s->entity);
                    SpriteComponent* sprite = registry->findComponent<SpriteComponent>(entity);
                    if (!sprite) continue;
                    entry.sprites.push_back({entity, EntityClipboard::encodeComponent(registry, entity, ComponentType::SpriteComponent)});
                    const int frame = std::max(0, s->shown);
                    applyToComponent(*sprite, plan.texture, guid, frame, frame < static_cast<int>(names.size()) ? names[static_cast<size_t>(frame)] : std::string());
                    report->converted++;
                }
                if (loaded.scene) {
                    loaded.scene->isModified = true;
                    loaded.scene->needUpdateRender = true;
                }
                if (loaded.prefab) loaded.prefab->isModified = true;
                undoData.push_back(std::move(entry));
                continue;
            }
            const fs::path full = project->getProjectPath() / owner;
            entry.closedFile = true;
            if (!assetio::readFile(full, entry.bytes)) {
                report->error = "cannot read " + owner;
                revert();
                return false;
            }
            try {
                YAML::Node root = YAML::Load(entry.bytes);
                for (const LegacySprite* s : sprites) {
                    YAML::Node entity;
                    if (!findEntityYaml(root["entities"], s->entity, entity) && !findEntityYaml(root["members"], s->entity, entity)) continue;
                    YAML::Node node = entity["components"]["sprite"];
                    SpriteComponent sprite = Stream::decodeSpriteComponent(node);
                    const int frame = std::max(0, s->shown);
                    applyToComponent(sprite, plan.texture, guid, frame, frame < static_cast<int>(names.size()) ? names[static_cast<size_t>(frame)] : std::string());
                    const YAML::Node encoded = Stream::encodeSpriteComponent(sprite);
                    node.remove("framesRect");
                    for (auto it = encoded.begin(); it != encoded.end(); ++it) node[it->first.as<std::string>()] = it->second;
                    report->converted++;
                }
                std::string writeError;
                if (!assetio::writeFile(full, YAML::Dump(root) + "\n", assetio::Durability::Document, &writeError)) {
                    report->error = "couldn't save " + owner + ": " + writeError;
                    revert();
                    return false;
                }
            } catch (const std::exception& e) {
                report->error = owner + ": " + e.what();
                revert();
                return false;
            }
            report->filesWritten.push_back(owner);
            project->getAssetDatabase().importAssets(full);
            project->getAssetIndex().updateReferrer(owner);
            undoData.push_back(std::move(entry));
        }
        SpriteSheetBuilder::get().invalidate(plan.texture);
        return true;
    }

    void ConvertInlineFramesCmd::revert() {
        for (auto it = undoData.rbegin(); it != undoData.rend(); ++it) {
            const OwnerUndo& entry = *it;
            if (entry.closedFile) {
                const fs::path full = project->getProjectPath() / entry.owner;
                std::string error;
                if (!assetio::writeFile(full, entry.bytes, assetio::Durability::Document, &error)) {
                    Out::error("Couldn't restore \"%s\": %s", entry.owner.c_str(), error.c_str());
                    continue;
                }
                project->getAssetDatabase().importAssets(full);
                project->getAssetIndex().updateReferrer(entry.owner);
                continue;
            }
            const Loaded loaded = loadedOwner(*project, entry.owner);
            if (EntityRegistry* registry = loaded.registry()) {
                for (auto s = entry.sprites.rbegin(); s != entry.sprites.rend(); ++s) {
                    EntityClipboard::applyComponentValue(registry, s->first, ComponentType::SpriteComponent, s->second);
                    if (SpriteComponent* sprite = registry->findComponent<SpriteComponent>(s->first)) {
                        sprite->sheetData.reset();
                        sprite->needUpdateSprite = true;
                        sprite->builtFrame = -1;
                    }
                }
                if (loaded.scene) {
                    loaded.scene->isModified = entry.wasModified;
                    loaded.scene->needUpdateRender = true;
                }
                if (loaded.prefab) loaded.prefab->isModified = entry.wasModified;
            }
        }
        undoData.clear();
        if (meta) {
            meta->undo();
            meta.reset();
        }
        SpriteSheetBuilder::get().invalidate(plan.texture);
    }

    void ConvertInlineFramesCmd::undo() {
        revert();
    }

}
