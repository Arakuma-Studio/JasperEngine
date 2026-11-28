// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteSkinEdit.h"

#include "SpriteSheetBuilder.h"

#include "Catalog.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/UndoTimeline.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/SetAssetMetaCmd.h"

#include "component/CameraComponent.h"
#include "component/SpriteComponent.h"
#include "component/SpriteSkinComponent.h"
#include "math/Quaternion.h"
#include "sprite/SpriteSheetIO.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"
#include "sprite/SpriteSkinning.h"
#include "texture/Texture.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace fs = std::filesystem;

namespace jasperengine::editor::sprite {

    const SpriteSkinGeometry* SpriteSkinSet::find(const std::string& frame) const {
        auto it = sprites.find(frame);
        return it == sprites.end() ? nullptr : it->second.get();
    }

    namespace {

        // a YAML scalar as a JSON number, null when it is not one
        nlohmann::json skinNumber(const YAML::Node& node) {
            double value = 0.0;
            if (!node || !node.IsScalar() || !YAML::convert<double>::decode(node, value)) return nullptr;
            return value;
        }

        // the skin's YAML (the record's JSON shape, flow lists) as that JSON
        nlohmann::json skinJson(const YAML::Node& node) {
            nlohmann::json j = nlohmann::json::object();
            auto point = [](const YAML::Node& p) {
                nlohmann::json out = nlohmann::json::array();
                if (p && p.IsSequence()) for (const YAML::Node& v : p) out.push_back(skinNumber(v));
                return out;
            };
            auto points = [&point](const YAML::Node& list) {
                nlohmann::json out = nlohmann::json::array();
                if (!list || !list.IsSequence()) return out;
                for (const YAML::Node& p : list) out.push_back(point(p));
                return out;
            };
            if (node["bones"] && node["bones"].IsSequence()) {
                j["bones"] = nlohmann::json::array();
                for (const YAML::Node& b : node["bones"]) {
                    nlohmann::json bone = nlohmann::json::object();
                    if (b["name"] && b["name"].IsScalar()) bone["name"] = b["name"].as<std::string>();
                    if (b["parent"]) bone["parent"] = skinNumber(b["parent"]);
                    if (b["head"]) bone["head"] = point(b["head"]);
                    if (b["tail"]) bone["tail"] = point(b["tail"]);
                    j["bones"].push_back(bone);
                }
            }
            if (node["vertices"]) j["vertices"] = points(node["vertices"]);
            if (node["indices"] && node["indices"].IsSequence()) {
                j["indices"] = nlohmann::json::array();
                for (const YAML::Node& i : node["indices"]) j["indices"].push_back(skinNumber(i));
            }
            if (node["weights"] && node["weights"].IsSequence()) {
                j["weights"] = nlohmann::json::array();
                for (const YAML::Node& w : node["weights"]) j["weights"].push_back(points(w));
            }
            return j;
        }

        // a vertex's influences as a map, and back (the four largest, normalized)
        std::map<int, float> influencesOf(const SpriteVertexWeights& w) {
            std::map<int, float> out;
            for (int k = 0; k < SPRITE_SKIN_MAX_INFLUENCES; k++) {
                if (w.weights[k] > 0.0f) out[w.bones[k]] += w.weights[k];
            }
            return out;
        }

        SpriteVertexWeights skinWeightsOf(const std::map<int, float>& influences) {
            std::vector<std::pair<int, float>> sorted(influences.begin(), influences.end());
            std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            SpriteVertexWeights w;
            float sum = 0.0f;
            for (size_t k = 0; k < sorted.size() && k < static_cast<size_t>(SPRITE_SKIN_MAX_INFLUENCES); k++) {
                if (sorted[k].second <= 0.0f) break;
                w.bones[k] = static_cast<uint8_t>(sorted[k].first);
                w.weights[k] = sorted[k].second;
                sum += sorted[k].second;
            }
            if (sum <= 0.0f) {
                w = SpriteVertexWeights();
                w.weights[0] = 1.0f;
                return w;
            }
            for (float& x : w.weights) x /= sum;
            return w;
        }

        float skinSegmentDistance(Vector2 p, Vector2 a, Vector2 b) {
            const Vector2 ab = b - a;
            const float length2 = ab.x * ab.x + ab.y * ab.y;
            float t = length2 > 0.0f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / length2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            const Vector2 q = Vector2(a.x + ab.x * t, a.y + ab.y * t);
            return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
        }

        void ensureWeights(SpriteSkinGeometry& skin) {
            if (skin.weights.size() == skin.vertices.size()) return;
            SpriteVertexWeights rigid;
            rigid.weights[0] = 1.0f;
            skin.weights.assign(skin.vertices.size(), rigid);
        }

    }

    bool skinSetFromYaml(const YAML::Node& node, SpriteSkinSet& out, std::string& error, std::vector<std::string>* problems) {
        out = SpriteSkinSet();
        if (!node || node.IsNull()) return true;
        if (!node.IsMap()) {
            error = "import.spriteSkin is not a map";
            return false;
        }
        const int version = node["version"] ? node["version"].as<int>(1) : 1;
        if (version > SpriteSkinSet::kVersion) {
            error = "import.spriteSkin is version " + std::to_string(version) + ", written by a newer editor";
            return false;
        }
        const YAML::Node sprites = node["sprites"];
        if (!sprites || !sprites.IsMap()) return true;
        for (auto it = sprites.begin(); it != sprites.end(); ++it) {
            const std::string name = it->first.as<std::string>();
            std::string why;
            std::shared_ptr<SpriteSkinGeometry> skin = parseSpriteSkin(skinJson(it->second), why);
            if (!skin) {
                if (problems) problems->push_back("sprites." + name + ": " + why);
                continue;
            }
            out.sprites[name] = std::move(skin);
        }
        return true;
    }

    YAML::Node skinSetToYaml(const SpriteSkinSet& set) {
        YAML::Node node(YAML::NodeType::Map);
        node["version"] = SpriteSkinSet::kVersion;
        YAML::Node sprites(YAML::NodeType::Map);
        for (const auto& [name, skin] : set.sprites) {
            // the record's text is JSON, which YAML reads as flow collections
            sprites[name] = YAML::Load("{" + writeSpriteSkin(*skin) + "}");
        }
        node["sprites"] = sprites;
        return node;
    }

    SpriteSkinSet readSpriteSkins(const fs::path& absoluteSource, std::vector<std::string>* problems) {
        SpriteSkinSet set;
        std::string error;
        const YAML::Node node = SetAssetMetaCmd::read(absoluteSource, kSpriteSkinKey);
        if (!skinSetFromYaml(node, set, error, problems) && problems) problems->push_back(error);
        return set;
    }

    void applySpriteSkins(SpriteSheetData& sheet, const SpriteSkinSet& set) {
        if (set.empty()) return;
        for (SpriteFrame& frame : sheet.frames) {
            auto it = set.sprites.find(frame.name);
            if (it != set.sprites.end()) frame.skin = it->second;
        }
    }

    std::string skinSetKey(const SpriteSkinSet& set) {
        std::string key;
        for (const auto& [name, skin] : set.sprites) key += name + "\n" + writeSpriteSkin(*skin) + "\n";
        return key;
    }

    bool autoGeometry(const pixelart::PixelImage& page, const SpriteFrame& frame, const AutoGeometrySettings& settings,
                      SpriteSkinGeometry& skin, std::string& error) {
        const int cell = std::clamp(settings.cellSize, 1, 256);
        const int tolerance = std::clamp(settings.alphaTolerance, 0, 254);
        // the packed pixels in source coordinates
        const int ox = static_cast<int>(frame.offset.x), oy = static_cast<int>(frame.offset.y);
        const int rx = static_cast<int>(frame.rect.getX()), ry = static_cast<int>(frame.rect.getY());
        const int rw = static_cast<int>(frame.rect.getWidth()), rh = static_cast<int>(frame.rect.getHeight());
        if (rw <= 0 || rh <= 0) {
            error = "frame '" + frame.name + "' has no pixels";
            return false;
        }
        auto opaque = [&](int sx, int sy) {
            const int px = rx + sx - ox, py = ry + sy - oy;
            if (sx < ox || sy < oy || sx >= ox + rw || sy >= oy + rh || px < 0 || py < 0 || px >= page.width() || py >= page.height()) return false;
            return page.row(py)[px * 4 + 3] > tolerance;
        };
        const int columns = (rw + cell - 1) / cell, rows = (rh + cell - 1) / cell;
        std::vector<char> filled(static_cast<size_t>(columns * rows), 0);
        bool any = false;
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < columns; c++) {
                const int x0 = ox + c * cell, y0 = oy + r * cell;
                bool hit = false;
                for (int y = y0; y < std::min(y0 + cell, oy + rh) && !hit; y++) {
                    for (int x = x0; x < std::min(x0 + cell, ox + rw) && !hit; x++) hit = opaque(x, y);
                }
                filled[static_cast<size_t>(r * columns + c)] = hit ? 1 : 0;
                any = any || hit;
            }
        }
        if (!any) {
            error = "frame '" + frame.name + "' has no pixel with alpha above " + std::to_string(tolerance);
            return false;
        }
        // the corners of the kept cells, shared, clamped to the packed pixels
        std::map<std::pair<int, int>, uint16_t> corner;
        std::vector<Vector2> vertices;
        std::vector<uint16_t> indices;
        auto vertexAt = [&](int c, int r) -> int {
            const auto key = std::make_pair(c, r);
            auto it = corner.find(key);
            if (it != corner.end()) return it->second;
            if (vertices.size() >= SPRITE_SKIN_MAX_VERTICES) return -1;
            const float x = static_cast<float>(std::min(ox + c * cell, ox + rw));
            const float y = static_cast<float>(std::min(oy + r * cell, oy + rh));
            vertices.emplace_back(x, y);
            const uint16_t index = static_cast<uint16_t>(vertices.size() - 1);
            corner[key] = index;
            return index;
        };
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < columns; c++) {
                if (!filled[static_cast<size_t>(r * columns + c)]) continue;
                const int a = vertexAt(c, r), b = vertexAt(c + 1, r), d = vertexAt(c + 1, r + 1), e = vertexAt(c, r + 1);
                if (a < 0 || b < 0 || d < 0 || e < 0) {
                    error = "the mesh would pass " + std::to_string(SPRITE_SKIN_MAX_VERTICES) + " vertices: use a larger cell size";
                    return false;
                }
                indices.insert(indices.end(), {static_cast<uint16_t>(a), static_cast<uint16_t>(b), static_cast<uint16_t>(d),
                                               static_cast<uint16_t>(a), static_cast<uint16_t>(d), static_cast<uint16_t>(e)});
            }
        }
        skin.vertices = std::move(vertices);
        skin.indices = std::move(indices);
        skin.weights.clear();
        return true;
    }

    bool autoWeights(SpriteSkinGeometry& skin, std::string& error) {
        if (skin.bones.empty()) {
            error = "the skin has no bones: create bones first";
            return false;
        }
        if (skin.vertices.empty()) {
            error = "the skin has no geometry: run Auto Geometry first";
            return false;
        }
        skin.weights.resize(skin.vertices.size());
        for (size_t v = 0; v < skin.vertices.size(); v++) {
            std::map<int, float> influences;
            for (size_t b = 0; b < skin.bones.size(); b++) {
                const float d = skinSegmentDistance(skin.vertices[v], skin.bones[b].head, skin.bones[b].tail);
                // a vertex on a bone belongs to it; farther ones fall off with the square
                influences[static_cast<int>(b)] = 1.0f / (d * d + 1.0f);
            }
            skin.weights[v] = skinWeightsOf(influences);
        }
        return true;
    }

    int findBone(const SpriteSkinGeometry& skin, const std::string& name) {
        for (size_t b = 0; b < skin.bones.size(); b++) {
            if (skin.bones[b].name == name) return static_cast<int>(b);
        }
        return -1;
    }

    bool addBone(SpriteSkinGeometry& skin, const SpriteBone& bone, std::string& error) {
        if (bone.name.empty()) {
            error = "a bone needs a name";
            return false;
        }
        if (findBone(skin, bone.name) >= 0) {
            error = "the skin already has a bone '" + bone.name + "'";
            return false;
        }
        if (skin.bones.size() >= static_cast<size_t>(SPRITE_SKIN_MAX_BONES)) {
            error = "a skin has at most " + std::to_string(SPRITE_SKIN_MAX_BONES) + " bones";
            return false;
        }
        if (bone.parent < -1 || bone.parent >= static_cast<int>(skin.bones.size())) {
            error = "the parent of '" + bone.name + "' is not a bone of the skin";
            return false;
        }
        skin.bones.push_back(bone);
        return true;
    }

    bool removeBone(SpriteSkinGeometry& skin, const std::string& name, std::string& error) {
        const int removed = findBone(skin, name);
        if (removed < 0) {
            error = "the skin has no bone '" + name + "'";
            return false;
        }
        const int heir = skin.bones[static_cast<size_t>(removed)].parent;
        // weights: the removed bone's go to its heir; indices above it move down
        auto remap = [&](int b) { return b == removed ? std::max(heir, 0) : b > removed ? b - 1 : b; };
        for (SpriteVertexWeights& w : skin.weights) {
            std::map<int, float> moved;
            for (const auto& [b, weight] : influencesOf(w)) {
                if (skin.bones.size() > 1) moved[remap(b)] += weight;
            }
            w = skinWeightsOf(moved);
        }
        for (SpriteBone& bone : skin.bones) {
            if (bone.parent == removed) bone.parent = heir;
            else if (bone.parent > removed) bone.parent--;
        }
        skin.bones.erase(skin.bones.begin() + removed);
        if (skin.bones.empty()) skin.weights.clear();
        return true;
    }

    bool setWeights(SpriteSkinGeometry& skin, const std::vector<int>& vertices, int bone, float weight, std::string& error) {
        if (bone < 0 || bone >= static_cast<int>(skin.bones.size())) {
            error = "no such bone";
            return false;
        }
        if (skin.vertices.empty()) {
            error = "the skin has no geometry: run Auto Geometry first";
            return false;
        }
        for (int v : vertices) {
            if (v < 0 || v >= static_cast<int>(skin.vertices.size())) {
                error = "vertex " + std::to_string(v) + " is not one of the " + std::to_string(skin.vertices.size());
                return false;
            }
        }
        weight = std::clamp(weight, 0.0f, 1.0f);
        ensureWeights(skin);
        auto apply = [&](size_t v) {
            std::map<int, float> influences = influencesOf(skin.weights[v]);
            influences.erase(bone);
            float rest = 0.0f;
            for (const auto& [b, w] : influences) rest += w;
            for (auto& [b, w] : influences) w = rest > 0.0f ? w / rest * (1.0f - weight) : 0.0f;
            if (weight > 0.0f) influences[bone] = weight;
            // nothing left to fill the rest with: the bone takes it all
            if (rest <= 0.0f) influences[bone] = 1.0f;
            skin.weights[v] = skinWeightsOf(influences);
        };
        if (vertices.empty()) {
            for (size_t v = 0; v < skin.vertices.size(); v++) apply(v);
        } else {
            for (int v : vertices) apply(static_cast<size_t>(v));
        }
        return true;
    }

    std::vector<int> brushWeights(SpriteSkinGeometry& skin, Vector2 center, float radius, int bone, float strength, WeightBrushMode mode) {
        std::vector<int> changed;
        if (skin.vertices.empty() || radius <= 0.0f) return changed;
        if (mode != WeightBrushMode::Smooth && (bone < 0 || bone >= static_cast<int>(skin.bones.size()))) return changed;
        ensureWeights(skin);
        strength = std::clamp(strength, 0.0f, 1.0f);
        // Smooth: each vertex's neighbours along the triangles' edges
        std::vector<std::set<int>> neighbours;
        if (mode == WeightBrushMode::Smooth) {
            neighbours.resize(skin.vertices.size());
            for (size_t t = 0; t + 2 < skin.indices.size(); t += 3) {
                for (int k = 0; k < 3; k++) {
                    const int a = skin.indices[t + k], b = skin.indices[t + (k + 1) % 3];
                    neighbours[static_cast<size_t>(a)].insert(b);
                    neighbours[static_cast<size_t>(b)].insert(a);
                }
            }
        }
        const std::vector<SpriteVertexWeights> before = skin.weights;
        for (size_t v = 0; v < skin.vertices.size(); v++) {
            const Vector2 d = skin.vertices[v] - center;
            const float distance = std::sqrt(d.x * d.x + d.y * d.y);
            if (distance > radius) continue;
            // a smooth falloff to the brush's edge
            const float t = distance / radius;
            const float amount = strength * (1.0f - t * t) * (1.0f - t * t);
            std::map<int, float> influences = influencesOf(before[v]);
            if (mode == WeightBrushMode::Smooth) {
                if (neighbours[v].empty()) continue;
                std::map<int, float> average;
                for (int n : neighbours[v]) {
                    for (const auto& [b, w] : influencesOf(before[static_cast<size_t>(n)])) average[b] += w / static_cast<float>(neighbours[v].size());
                }
                std::set<int> bones;
                for (const auto& [b, w] : influences) bones.insert(b);
                for (const auto& [b, w] : average) bones.insert(b);
                std::map<int, float> mixed;
                for (int b : bones) mixed[b] = influences[b] * (1.0f - amount) + average[b] * amount;
                influences = mixed;
            } else if (mode == WeightBrushMode::Add) {
                influences[bone] += amount;
            } else {
                influences[bone] = std::max(0.0f, influences[bone] - amount);
                float sum = 0.0f;
                for (const auto& [b, w] : influences) sum += w;
                // nothing left: the vertex stays on what it had
                if (sum <= 0.0f) continue;
            }
            skin.weights[v] = skinWeightsOf(influences);
            changed.push_back(static_cast<int>(v));
        }
        return changed;
    }

    std::vector<std::map<int, float>> weightTable(const SpriteSkinGeometry& skin) {
        std::vector<std::map<int, float>> table;
        for (const SpriteVertexWeights& w : skin.weights) table.push_back(influencesOf(w));
        return table;
    }

    // ---- a Sprite in a scene

    bool resolveSkinTarget(Project* project, uint32_t sceneId, Entity entity, SkinTarget& out, std::string& error) {
        out = SkinTarget();
        out.project = project;
        out.sceneId = sceneId;
        out.entity = entity;
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        Scene* scene = sceneProject ? sceneProject->scene : nullptr;
        SpriteComponent* sprite = scene && scene->isEntityCreated(entity) ? scene->findComponent<SpriteComponent>(entity) : nullptr;
        if (!sprite) {
            error = "the GameObject has no Sprite: a Sprite Skin bends a Sprite (add one, or pick a Sprite GameObject)";
            return false;
        }
        std::string source = sprite->sheet.path;
        if (source.empty()) {
            // a legacy Sprite of one whole texture becomes a sheet Sprite of it
            // (through the Catalog: the Mesh's header is one of the editor's widest includes)
            const PropertyData property = Catalog::findProperty(scene, entity, ComponentType::MeshComponent, "submeshes[0].material.baseColorTexture");
            const Texture* texture = property.ref && property.type == PropertyType::Texture ? static_cast<const Texture*>(property.ref) : nullptr;
            if (sprite->numFramesRect > 0 || !texture || texture->getPath().empty()) {
                error = "the Sprite has no sheet: give it one (set_sprite_frames sheet, or the Sprite's Sheet field) before skinning it";
                return false;
            }
            source = project->normalizeToAssetsRelative(project->resolveAssetPath(texture->getPath())).generic_string();
            out.convert = true;
        }
        if (SpriteSheetPool::isAsepritePath(source) || SpriteSheetSources::isAtlasPath(source) || !SpriteSheetSources::isTexturePath(source)) {
            error = "'" + source + "' cannot hold a skin: skins live on textures and PSD/PSB files (a Sprite of an atlas skins its source texture)";
            return false;
        }
        out.source = source;
        out.absoluteSource = (project->getAssetsPath() / source).lexically_normal();
        out.sheet = SpriteSheetPool::get(source);
        if (!out.sheet || out.sheet->frames.empty()) {
            error = "'" + source + "' has no frames yet (still importing, or it does not read)";
            return false;
        }
        out.frame = out.convert ? 0 : std::clamp(sprite->frame, 0, out.sheet->frameCount() - 1);
        return true;
    }

    SpriteSkinGeometry storedSkin(const SkinTarget& target) {
        const SpriteSkinSet set = readSpriteSkins(target.absoluteSource);
        const SpriteSkinGeometry* skin = set.find(target.frameData().name);
        return skin ? *skin : SpriteSkinGeometry();
    }

    Command* skinMetaCommand(const SkinTarget& target, const SpriteSkinGeometry& skin) {
        SpriteSkinSet set = readSpriteSkins(target.absoluteSource);
        const std::string& name = target.frameData().name;
        if (skin.bones.empty() && skin.vertices.empty()) set.sprites.erase(name);
        else set.sprites[name] = std::make_shared<SpriteSkinGeometry>(skin);
        auto* command = new SetAssetMetaCmd(target.project, target.project->normalizeToProjectRelative(target.absoluteSource), kSpriteSkinKey,
                                            set.empty() ? YAML::Node() : skinSetToYaml(set));
        command->setAction("Edit Sprite Skin");
        return command;
    }

    SpriteFrameGeometryInput skinGeometryInput(Scene* scene, Entity entity, const SpriteSheetData& sheet, const SpriteFrame& frame) {
        SpriteFrameGeometryInput input;
        const SpriteComponent& sprite = scene->getComponent<SpriteComponent>(entity);
        input.width = sprite.width;
        input.height = sprite.height;
        input.pivot = sprite.pivotPreset;
        input.pixelsPerUnit = sprite.pixelsPerUnit > 0.0f ? sprite.pixelsPerUnit : sheet.pixelsPerUnit;
        input.pixelRatio = sheet.pixelRatio;
        const CameraComponent* camera = scene->findComponent<CameraComponent>(scene->getCamera());
        input.uiCamera = camera && camera->type == CameraType::CAMERA_UI;
        input.flipY = sprite.flipY;
        if (frame.page < sheet.pages.size()) {
            input.pageWidth = sheet.pages[frame.page].width;
            input.pageHeight = sheet.pages[frame.page].height;
        }
        return input;
    }

    std::optional<pixelart::PixelImage> loadFramePage(Project* project, const SpriteSheetData& sheet, const SpriteFrame& frame, std::string* error) {
        if (frame.page >= sheet.pages.size() || sheet.pages[frame.page].texture.empty()) {
            if (error) *error = "the frame's page is not there";
            return std::nullopt;
        }
        const fs::path page = project->resolveAssetPath(sheet.pages[frame.page].texture);
        return loadSpriteImage(page, error);
    }

    bool bindSkinBones(const SkinTarget& target, const SpriteSkinGeometry& skin, const std::vector<std::string>& removedBones,
                       const SkinRunCommand& run, std::string& error, std::vector<Entity>* bonesOut) {
        SceneProject* sceneProject = target.project->getScene(target.sceneId);
        Scene* scene = sceneProject ? sceneProject->scene : nullptr;
        if (!scene || !scene->isEntityCreated(target.entity)) {
            error = "the Sprite's GameObject is gone";
            return false;
        }
        if (target.convert && scene->getComponent<SpriteComponent>(target.entity).sheet.path.empty()) {
            const Guid guid = target.project->getAssetDatabase().getAssetGuid(target.project->normalizeToProjectRelative(target.absoluteSource));
            if (!run(new PropertyCmd<AssetRef>(target.project, target.sceneId, target.entity, ComponentType::SpriteComponent, "sheet",
                                               AssetRef(target.source, guid))) ||
                !run(new PropertyCmd<int>(target.project, target.sceneId, target.entity, ComponentType::SpriteComponent, "frame", 0))) {
                error = "couldn't make the Sprite a sheet Sprite of " + target.source;
                return false;
            }
        }
        // the bone GameObjects bound now, by name
        std::map<std::string, Entity> existing;
        if (const SpriteSkinComponent* component = scene->findComponent<SpriteSkinComponent>(target.entity)) {
            for (Entity bone : component->bones) {
                if (bone != NULL_ENTITY && scene->isEntityCreated(bone)) existing[scene->getEntityName(bone)] = bone;
            }
        }
        for (const std::string& name : removedBones) {
            auto it = existing.find(name);
            if (it == existing.end()) continue;
            if (!run(new DeleteEntityCmd(target.project, target.sceneId, it->second))) {
                error = "couldn't delete the GameObject of bone '" + name + "'";
                return false;
            }
            existing.erase(it);
        }
        const SpriteFrame& frame = target.frameData();
        const SpriteFrameGeometryInput input = skinGeometryInput(scene, target.entity, *target.sheet, frame);
        std::vector<Entity> bound(skin.bones.size(), NULL_ENTITY);
        for (size_t b = 0; b < skin.bones.size(); b++) {
            const SpriteBone& bone = skin.bones[b];
            auto it = existing.find(bone.name);
            // a bone GameObject deleted with its parent's is made again
            if (it != existing.end() && scene->isEntityCreated(it->second)) {
                bound[b] = it->second;
                continue;
            }
            const Entity parent = bone.parent >= 0 ? bound[static_cast<size_t>(bone.parent)] : target.entity;
            const SpriteBoneRest rest = spriteBoneRestTransform(frame, skin, input, static_cast<int>(b));
            auto* create = new CreateEntityCmd(target.project, target.sceneId, bone.name, EntityCreationType::GAMEOBJECT, parent);
            create->setQuiet(true);
            create->addProperty<Vector3>(ComponentType::Transform, "position", rest.position);
            create->addProperty<Quaternion>(ComponentType::Transform, "rotation", Quaternion(0.0f, 0.0f, rest.rotationZ));
            if (!run(create)) {
                error = "couldn't create the GameObject of bone '" + bone.name + "'";
                return false;
            }
            bound[b] = create->getEntity();
        }
        if (!scene->findComponent<SpriteSkinComponent>(target.entity) &&
            !run(new AddComponentCmd(target.project, target.sceneId, target.entity, ComponentType::SpriteSkinComponent))) {
            error = "couldn't add the Sprite Skin";
            return false;
        }
        Entity root = NULL_ENTITY;
        for (size_t b = 0; b < skin.bones.size() && root == NULL_ENTITY; b++) {
            if (skin.bones[b].parent < 0) root = bound[b];
        }
        const SpriteSkinComponent& component = scene->getComponent<SpriteSkinComponent>(target.entity);
        if (component.bones != bound &&
            !run(new PropertyCmd<std::vector<Entity>>(target.project, target.sceneId, target.entity, ComponentType::SpriteSkinComponent, "bones", bound))) {
            error = "couldn't set the Sprite Skin's bones";
            return false;
        }
        if (component.rootBone != root &&
            !run(new PropertyCmd<Entity>(target.project, target.sceneId, target.entity, ComponentType::SpriteSkinComponent, "rootBone", root))) {
            error = "couldn't set the Sprite Skin's Root Bone";
            return false;
        }
        if (bonesOut) *bonesOut = bound;
        return true;
    }

    bool applySkinEdit(const SkinTarget& target, const SpriteSkinGeometry& skin, const std::vector<std::string>& removedBones, bool bind,
                       std::string& error, std::vector<Entity>* bones, bool* changed) {
        if (changed) *changed = false;
        CommandHistory* projectHistory = target.project->getProjectCommandHistory();
        bool metaAdded = false;
        Command* meta = skinMetaCommand(target, skin);
        if (meta->isNoOp()) {
            delete meta;
        } else {
            meta->setNoMerge();
            if (!projectHistory->addCommand(meta)) {
                error = "couldn't write the skin into " + target.source + "'s .meta (is it writable?)";
                return false;
            }
            metaAdded = true;
        }
        CommandHistory* history = CommandHandle::get(target.sceneId);
        const size_t start = history->mark();
        if (bind && (!skin.bones.empty() || !removedBones.empty())) {
            const SkinRunCommand run = [history](Command* command) {
                command->setNoMerge();
                return history->addCommand(command);
            };
            if (!bindSkinBones(target, skin, removedBones, run, error, bones)) {
                while (history->position() > start && history->canUndo() && history->undo()) {}
                if (metaAdded && projectHistory->canUndo()) projectHistory->undo();
                return false;
            }
        } else if (bones) {
            SceneProject* sceneProject = target.project->getScene(target.sceneId);
            const SpriteSkinComponent* component = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<SpriteSkinComponent>(target.entity) : nullptr;
            *bones = component ? component->bones : std::vector<Entity>();
        }
        history->groupSince(start);
        const bool sceneChanged = history->position() > start;
        if (metaAdded && sceneChanged) UndoTimeline::link({projectHistory, history});
        if (changed) *changed = metaAdded || sceneChanged;
        return true;
    }

}
