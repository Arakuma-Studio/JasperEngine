// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PsdLayerImport.h"

#include "Catalog.h"
#include "Out.h"
#include "Project.h"
#include "command/type/SetAssetMetaCmd.h"
#include "reflect/Reflect.h"
#include "sprite/SpritePacker.h"
#include "texture/TextureImportSettings.h"
#include "titleart/interchange/psd/PsdReader.h"
#include "util/Prefab.h"

#include "component/SpriteComponent.h"
#include "component/SpriteSkinComponent.h"
#include "component/Transform.h"
#include "io/AtomicFile.h"
#include "sprite/SpriteSheetIO.h"
#include "sprite/SpriteSheetPool.h"
#include "util/Angle.h"
#include "util/Sha256.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <set>

namespace fs = std::filesystem;

namespace jasperengine::editor {

    namespace {

        bool psdEndsWithNoCase(const std::string& s, const char* suffix) {
            const size_t n = std::char_traits<char>::length(suffix);
            if (s.size() < n) return false;
            for (size_t i = 0; i < n; i++) {
                if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i]) return false;
            }
            return true;
        }

        struct PsdLayerFrame {
            uint32_t layerId = 0;
            std::string name;
            psd::PsdRect bounds;
            std::shared_ptr<pixelart::PixelImage> image;
        };

        // the visible pixel layers in stack order (bottom first), and the tree as the prefab wants it
        void psdCollectLayers(const psd::PsdFile& file, const std::vector<psd::PsdNode>& nodes, bool hidden, std::vector<PsdLayerFrame>& frames,
                           std::vector<PsdLayerNode>& out, std::vector<std::string>& warnings) {
            // nodes come bottom first; the tree keeps the top first among siblings (the Hierarchy's order)
            for (const psd::PsdNode& node : nodes) {
                const psd::PsdLayerRecord& layer = file.layers[static_cast<size_t>(node.layer)];
                const bool invisible = hidden || !layer.visible;
                if (!node.children.empty() || layer.section == 1 || layer.section == 2) {
                    PsdLayerNode group;
                    group.group = true;
                    group.layerId = layer.id;
                    group.name = layer.name;
                    psdCollectLayers(file, node.children, invisible, frames, group.children, warnings);
                    if (!group.children.empty()) out.insert(out.begin(), std::move(group));
                    continue;
                }
                if (invisible) continue;
                if (!layer.adjustmentKey.empty() || !layer.fillKey.empty()) {
                    warnings.push_back("layer '" + layer.name + "' is an adjustment or fill layer: left out (rasterize it to import it)");
                    continue;
                }
                if (layer.bounds.empty() || !layer.planes[0].present()) {
                    if (layer.text || layer.smartObject) {
                        warnings.push_back("layer '" + layer.name + "' has no pixels in the file: left out (rasterize it to import it)");
                    }
                    continue;
                }
                if (layer.blendKey != "norm" && layer.blendKey != "pass") {
                    warnings.push_back("layer '" + layer.name + "' blends with '" + layer.blendKey + "': imported as Normal");
                }
                PsdLayerFrame frame;
                frame.layerId = layer.id;
                frame.name = layer.name;
                frame.bounds = layer.bounds;
                const std::vector<uint8_t> rgba = psd::psdLayerRgba8(file, layer);
                auto image = std::make_shared<pixelart::PixelImage>(layer.bounds.width(), layer.bounds.height());
                std::span<uint8_t> px = image->editPixels();
                const uint32_t opacity = static_cast<uint32_t>(layer.opacity) * layer.fillOpacity;
                for (size_t i = 0; i + 3 < rgba.size() && i + 3 < px.size(); i += 4) {
                    px[i] = rgba[i];
                    px[i + 1] = rgba[i + 1];
                    px[i + 2] = rgba[i + 2];
                    // the layer's opacity and fill go into its pixels (a sprite has one alpha)
                    px[i + 3] = static_cast<uint8_t>((rgba[i + 3] * opacity + 255u * 255u / 2u) / (255u * 255u));
                }
                frame.image = image;
                PsdLayerNode leaf;
                leaf.layerId = layer.id;
                leaf.name = layer.name;
                leaf.frame = static_cast<int>(frames.size());
                leaf.order = static_cast<int>(frames.size());
                frames.push_back(std::move(frame));
                out.insert(out.begin(), std::move(leaf));
            }
        }

        // a layer's place in its node, filled once the frames are known
        void psdPlaceNodes(std::vector<PsdLayerNode>& nodes, const std::vector<PsdLayerFrame>& frames, int width, int height, int ppu) {
            for (PsdLayerNode& node : nodes) {
                if (node.group) {
                    psdPlaceNodes(node.children, frames, width, height, ppu);
                    continue;
                }
                const psd::PsdRect& b = frames[static_cast<size_t>(node.frame)].bounds;
                const float cx = (b.left + b.right) / 2.0f, cy = (b.top + b.bottom) / 2.0f;
                node.x = (cx - width / 2.0f) / static_cast<float>(ppu);
                node.y = (height / 2.0f - cy) / static_cast<float>(ppu);
            }
        }

        std::string psdComponentKey(ComponentType type) { return Catalog::getComponentName(type, true); }

        Entity psdNodeEntity(const PsdLayerNode& node, size_t fallback) {
            // ids from the layer ids: an instance's overrides find their GameObject after a reimport
            return node.layerId != 0 ? static_cast<Entity>(100 + node.layerId) : static_cast<Entity>(100000 + fallback);
        }

        YAML::Node psdTransformNode(const Vector3& position, float rotationZ) {
            Transform transform;
            transform.position = position;
            transform.rotation = Quaternion(0.0f, 0.0f, rotationZ);
            return reflect::encodeValue(ComponentType::Transform, &transform);
        }

        // a bone of the rig in the document (pixels, y down)
        struct PsdRigBone {
            std::string name;
            std::string parent;
            Vector2 head, tail;
            Entity entity = NULL_ENTITY;
        };

    }

    bool isPsdPath(const std::string& path) {
        return psdEndsWithNoCase(path, ".psd") || psdEndsWithNoCase(path, ".psb");
    }

    PsdLayerImportResult importPsdLayers(const std::vector<uint8_t>& bytes, const std::string& assetPath, int pixelsPerUnit,
                                         const sprite::SpriteSkinSet& skins) {
        PsdLayerImportResult out;
        psd::PsdFile file;
        std::string error;
        psd::PsdReadOptions options;
        options.mergedImage = false;
        if (!psd::readPsd(bytes.data(), bytes.size(), file, error, options)) {
            out.error = assetPath + " does not read as a PSD/PSB: " + error;
            return out;
        }
        if (!file.rgbOrGray() || (file.depth != 8 && file.depth != 16)) {
            out.error = assetPath + " is " + psd::psdColorModeName(file.mode) + " " + std::to_string(file.depth) +
                        "-bit: Layers import reads RGB and Grayscale documents of 8 or 16 bits (convert it in Photoshop)";
            return out;
        }
        out.width = file.width;
        out.height = file.height;
        out.pixelsPerUnit = std::max(1, pixelsPerUnit);
        std::vector<PsdLayerFrame> frames;
        psdCollectLayers(file, psd::psdTree(file), false, frames, out.roots, out.warnings);
        if (frames.empty()) {
            out.error = assetPath + " has no visible pixel layer to import";
            return out;
        }
        psdPlaceNodes(out.roots, frames, out.width, out.height, out.pixelsPerUnit);

        sprite::PackMember member;
        member.path = assetPath;
        std::set<std::string> names;
        std::vector<std::string> frameNames;
        for (const PsdLayerFrame& f : frames) {
            std::string name = f.name.empty() ? "Layer" : f.name;
            if (names.count(name)) {
                for (int k = 2;; k++) {
                    const std::string unique = name + " " + std::to_string(k);
                    if (!names.count(unique)) {
                        out.warnings.push_back("two layers are named '" + name + "': the later one's sprite is '" + unique + "'");
                        name = unique;
                        break;
                    }
                }
            }
            names.insert(name);
            frameNames.push_back(name);
            member.frames.push_back({name, f.image, false});
        }
        sprite::PackSettings settings;
        settings.maxSize = 8192;
        sprite::PackResult packed = sprite::pack({member}, settings);
        if (!packed.ok()) {
            out.error = assetPath + ": " + packed.error.message;
            return out;
        }
        out.sheet.source = assetPath;
        out.sheet.pixelsPerUnit = static_cast<float>(out.pixelsPerUnit);
        for (const pixelart::IntSize& size : packed.layout.pages) out.sheet.pages.push_back({std::string(), size.w, size.h});
        for (size_t i = 0; i < frames.size(); i++) {
            const sprite::PackedFrame& p = packed.layout.frames[i];
            SpriteFrame frame;
            frame.name = frameNames[i];
            frame.key = "layer:" + std::to_string(frames[i].layerId);
            frame.page = static_cast<uint16_t>(p.page);
            frame.rect = Rect(static_cast<float>(p.rect.x), static_cast<float>(p.rect.y), static_cast<float>(p.rect.w), static_cast<float>(p.rect.h));
            frame.sourceSize = Vector2(static_cast<float>(p.sourceSize.w), static_cast<float>(p.sourceSize.h));
            frame.offset = Vector2(static_cast<float>(p.offset.x), static_cast<float>(p.offset.y));
            // the layer's centre: the prefab places each Sprite there
            frame.pivot = Vector2(std::floor(p.sourceSize.w / 2.0f), std::floor(p.sourceSize.h / 2.0f));
            out.sheet.frames.push_back(std::move(frame));
        }
        // the prefab's node names follow the frames' (unique) names
        std::function<void(std::vector<PsdLayerNode>&)> rename = [&](std::vector<PsdLayerNode>& nodes) {
            for (PsdLayerNode& node : nodes) {
                if (node.group) rename(node.children);
                else node.name = frameNames[static_cast<size_t>(node.frame)];
            }
        };
        rename(out.roots);
        sprite::applySpriteSkins(out.sheet, skins);
        out.pages = std::move(packed.pages);
        out.ok = true;
        return out;
    }

    YAML::Node psdLayerPrefab(const PsdLayerImportResult& import, const std::string& assetPath, const Guid& sourceGuid,
                              const Guid& prefabGuid, bool characterRig) {
        reflect::DocumentEncodeScope document;
        const float ppu = static_cast<float>(std::max(1, import.pixelsPerUnit));
        Entity next = 1;
        size_t fallback = 0;

        // Character Rig: the bones of every skinned layer, merged by name, in the document
        std::vector<PsdRigBone> rig;
        std::map<std::string, size_t> rigByName;
        if (characterRig) {
            std::function<void(const std::vector<PsdLayerNode>&)> gather = [&](const std::vector<PsdLayerNode>& nodes) {
                for (const PsdLayerNode& node : nodes) {
                    if (node.group) {
                        gather(node.children);
                        continue;
                    }
                    const SpriteFrame& frame = import.sheet.frames[static_cast<size_t>(node.frame)];
                    if (!frame.skin) continue;
                    // the frame's source origin in the document
                    const float left = node.x * ppu + import.width / 2.0f - frame.sourceSize.x / 2.0f;
                    const float top = import.height / 2.0f - node.y * ppu - frame.sourceSize.y / 2.0f;
                    for (const SpriteBone& bone : frame.skin->bones) {
                        if (rigByName.count(bone.name)) continue;
                        PsdRigBone r;
                        r.name = bone.name;
                        r.parent = bone.parent >= 0 ? frame.skin->bones[static_cast<size_t>(bone.parent)].name : std::string();
                        r.head = Vector2(left + bone.head.x, top + bone.head.y);
                        r.tail = Vector2(left + bone.tail.x, top + bone.tail.y);
                        r.entity = static_cast<Entity>(1000000 + rig.size());
                        rigByName[r.name] = rig.size();
                        rig.push_back(r);
                    }
                }
            };
            gather(import.roots);
        }
        // a bone's pose in the root's space: its head in units from the document's centre, y up,
        // its x axis along head -> tail (the Sprites' bind poses, as each Sprite sits at its layer)
        auto rootPose = [&](const PsdRigBone& bone, Vector2& at, float& angle) {
            at = Vector2((bone.head.x - import.width / 2.0f) / ppu, (import.height / 2.0f - bone.head.y) / ppu);
            const Vector2 d = Vector2(bone.tail.x - bone.head.x, bone.head.y - bone.tail.y);
            angle = (d.x == 0.0f && d.y == 0.0f) ? 0.0f : std::atan2(d.y, d.x);
        };

        std::function<YAML::Node(const PsdLayerNode&)> encodeNode = [&](const PsdLayerNode& node) {
            YAML::Node n(YAML::NodeType::Map);
            n["type"] = "Entity";
            const Entity entity = psdNodeEntity(node, fallback++);
            next = std::max(next, entity + 1);
            n["entity"] = entity;
            n["name"] = node.name;
            YAML::Node components(YAML::NodeType::Map);
            components[psdComponentKey(ComponentType::Transform)] = psdTransformNode(node.group ? Vector3(0, 0, 0) : Vector3(node.x, node.y, 0.0f), 0.0f);
            if (node.group) {
                n["components"] = components;
                YAML::Node children(YAML::NodeType::Sequence);
                for (const PsdLayerNode& child : node.children) children.push_back(encodeNode(child));
                n["children"] = children;
                return n;
            }
            const SpriteFrame& frame = import.sheet.frames[static_cast<size_t>(node.frame)];
            // the Mesh's defaults but its Order in Layer (a decode keeps the defaults of absent keys)
            YAML::Node mesh(YAML::NodeType::Map);
            mesh["sortingOrder"] = std::clamp(node.order, -32768, 32767);
            components[psdComponentKey(ComponentType::MeshComponent)] = mesh;
            SpriteComponent sprite;
            sprite.sheet = AssetRef(assetPath, sourceGuid);
            sprite.frame = node.frame;
            sprite.frameName = frame.name;
            sprite.pivotPreset = PivotPreset::FRAME;
            components[psdComponentKey(ComponentType::SpriteComponent)] = reflect::encodeValue(ComponentType::SpriteComponent, &sprite);
            if (characterRig && frame.skin) {
                SpriteSkinComponent skin;
                for (const SpriteBone& bone : frame.skin->bones) skin.bones.push_back(rig[rigByName[bone.name]].entity);
                if (!skin.bones.empty()) skin.rootBone = skin.bones[0];
                components[psdComponentKey(ComponentType::SpriteSkinComponent)] = reflect::encodeValue(ComponentType::SpriteSkinComponent, &skin);
            }
            n["components"] = components;
            return n;
        };

        YAML::Node root(YAML::NodeType::Map);
        root["type"] = "Entity";
        root["entity"] = 1;
        root["name"] = fs::path(assetPath).stem().string();
        YAML::Node rootComponents(YAML::NodeType::Map);
        rootComponents[psdComponentKey(ComponentType::Transform)] = psdTransformNode(Vector3(0, 0, 0), 0.0f);
        root["components"] = rootComponents;
        YAML::Node children(YAML::NodeType::Sequence);
        for (const PsdLayerNode& node : import.roots) children.push_back(encodeNode(node));
        // the skeleton after the layers: bones nest under their parents
        std::function<YAML::Node(size_t)> encodeBone = [&](size_t b) {
            const PsdRigBone& bone = rig[b];
            Vector2 at;
            float angle = 0.0f;
            rootPose(bone, at, angle);
            Vector3 local(at.x, at.y, 0.0f);
            float localAngle = angle;
            if (!bone.parent.empty() && rigByName.count(bone.parent)) {
                Vector2 parentAt;
                float parentAngle = 0.0f;
                rootPose(rig[rigByName[bone.parent]], parentAt, parentAngle);
                const Vector2 d = at - parentAt;
                const float c = std::cos(-parentAngle), s = std::sin(-parentAngle);
                local = Vector3(d.x * c - d.y * s, d.x * s + d.y * c, 0.0f);
                localAngle = angle - parentAngle;
            }
            YAML::Node n(YAML::NodeType::Map);
            n["type"] = "Entity";
            n["entity"] = bone.entity;
            next = std::max(next, bone.entity + 1);
            n["name"] = bone.name;
            YAML::Node components(YAML::NodeType::Map);
            components[psdComponentKey(ComponentType::Transform)] = psdTransformNode(local, Angle::radToDefault(localAngle));
            n["components"] = components;
            YAML::Node boneChildren(YAML::NodeType::Sequence);
            for (size_t c = 0; c < rig.size(); c++) {
                if (rig[c].parent == bone.name) boneChildren.push_back(encodeBone(c));
            }
            if (boneChildren.size() > 0) n["children"] = boneChildren;
            return n;
        };
        for (size_t b = 0; b < rig.size(); b++) {
            if (rig[b].parent.empty() || !rigByName.count(rig[b].parent)) children.push_back(encodeBone(b));
        }
        root["children"] = children;

        YAML::Node prefab(YAML::NodeType::Map);
        prefab["type"] = "Prefab";
        prefab["version"] = Prefab::FILE_VERSION;
        prefab["guid"] = prefabGuid.str();
        prefab["nextEntity"] = next;
        prefab["root"] = 1;
        YAML::Node members(YAML::NodeType::Sequence);
        members.push_back(root);
        prefab["members"] = members;
        return prefab;
    }

    // ---- the editor's importer

    PsdLayerImporter& PsdLayerImporter::get() {
        static PsdLayerImporter* importer = new PsdLayerImporter();
        return *importer;
    }

    void PsdLayerImporter::attach(Project* newProject) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            project = newProject;
            memos.clear();
            assetsRoot = project ? project->getAssetsPath() : fs::path();
            cacheRoot = project ? project->getProjectInternalPath() / "cache" / "imported" / "v1" / kCacheDir : fs::path();
            projectPixelsPerUnit = project ? project->getPixelsPerUnit() : 1;
        }
        if (!newProject) {
            SetAssetMetaCmd::removeWriteListener(this);
            return;
        }
        // Import Mode, Character Rig or the skins changed (an undo too): the prefab follows
        SetAssetMetaCmd::addWriteListener(this, [this](Project* writer, const fs::path& asset, const std::string& key) {
            if (writer != project || (key != TextureImportSettings::metaKey && key != sprite::kSpriteSkinKey)) return;
            const std::string assetPath = (project->getProjectPath() / asset).lexically_normal()
                                              .lexically_relative(project->getAssetsPath().lexically_normal()).generic_string();
            if (!isPsdPath(assetPath)) return;
            invalidate(assetPath);
            std::string error;
            if (isLayered(assetPath) && !writePrefab(assetPath, error)) Out::warning("%s: %s", assetPath.c_str(), error.c_str());
        });
    }

    bool PsdLayerImporter::isLayered(const std::string& assetPath) const {
        fs::path assets;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!project) return false;
            assets = assetsRoot;
        }
        if (!isPsdPath(assetPath)) return false;
        TextureImportSettings settings;
        return TextureImportSettings::load((assets / assetPath).string(), settings) && settings.resolvedImportMode() == TextureImportMode::Layers;
    }

    void PsdLayerImporter::invalidate(const std::string& assetPath) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            memos.erase(assetPath);
        }
        SpriteSheetPool::invalidate(assetPath);
    }

    std::string PsdLayerImporter::prefabPathOf(const std::string& assetPath) {
        return assetPath + ".prefab";
    }

    bool PsdLayerImporter::ensure(const std::string& assetPath, Memo& out) {
        fs::path assets, cache;
        int ppuDefault = 1;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!project) return false;
            assets = assetsRoot;
            cache = cacheRoot;
            ppuDefault = projectPixelsPerUnit;
        }
        if (!isLayered(assetPath)) return false;
        const fs::path absolute = (assets / assetPath).lexically_normal();
        const fs::path meta = SetAssetMetaCmd::metaPathOf(absolute);
        std::error_code ec, metaEc;
        Memo stamp;
        stamp.sourceTime = fs::last_write_time(absolute, ec);
        if (ec) return false;
        stamp.sourceSize = fs::file_size(absolute, ec);
        stamp.metaTime = fs::last_write_time(meta, metaEc);
        if (!metaEc) stamp.metaSize = fs::file_size(meta, metaEc);
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = memos.find(assetPath);
            if (it != memos.end() && it->second.sourceTime == stamp.sourceTime && it->second.sourceSize == stamp.sourceSize &&
                it->second.metaTime == stamp.metaTime && it->second.metaSize == stamp.metaSize) {
                out = it->second;
                return !out.dir.empty();
            }
        }
        TextureImportSettings settings;
        TextureImportSettings::load(absolute.string(), settings);
        const int ppu = settings.resolvedPixelsPerUnit(ppuDefault);
        const sprite::SpriteSkinSet skins = sprite::readSpriteSkins(absolute);
        std::ifstream in(absolute, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string key = sha256Hex(sha256Hex(std::string(bytes.begin(), bytes.end())) + "|path " + assetPath + "|ppu " + std::to_string(ppu) +
                                          "|skins " + sha256Hex(sprite::skinSetKey(skins)) + "|importer " + std::to_string(kPsdLayerImporterVersion) +
                                          "|packer " + std::to_string(sprite::kPackerVersion));
        const fs::path dir = cache / key.substr(0, 2) / key;
        const fs::path sheetFile = dir / "sheet.json";
        if (fs::exists(sheetFile, ec)) {
            // the record names its pages
            std::ifstream sheetIn(sheetFile, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(sheetIn)), std::istreambuf_iterator<char>());
            SpriteSheetParseResult parsed = parseSheet(text);
            stamp.dir = parsed.ok() ? dir : fs::path();
            stamp.pages = parsed.ok() ? parsed.sheet->pages.size() : 0;
        } else {
            PsdLayerImportResult result = importPsdLayers(bytes, assetPath, ppu, skins);
            if (result.ok) {
                io::AtomicWriteOptions options;
                options.durability = io::Durability::Cache;
                bool written = true;
                for (size_t p = 0; p < result.pages.size() && written; p++) {
                    written = io::writeFileAtomic(dir / ("page" + std::to_string(p) + ".png"), result.pages[p].encodePng(), options).ok();
                }
                // the record last: its presence says the import is whole
                written = written && io::writeFileAtomic(sheetFile, writeSheet(result.sheet), options).ok();
                stamp.dir = written ? dir : fs::path();
                stamp.pages = result.pages.size();
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        memos[assetPath] = stamp;
        out = stamp;
        return !stamp.dir.empty();
    }

    bool PsdLayerImporter::resolve(const std::string& assetPath, const std::string& key, std::string& fileOut) {
        Memo memo;
        if (!ensure(assetPath, memo)) return false;
        if (key == "sheet") {
            fileOut = (memo.dir / "sheet.json").string();
            return true;
        }
        if (key.rfind("image#", 0) == 0) {
            const std::string index = key.substr(6);
            if (index.empty() || index.find_first_not_of("0123456789") != std::string::npos) return false;
            const size_t page = static_cast<size_t>(std::stoul(index));
            if (page >= memo.pages) return false;
            fileOut = (memo.dir / ("page" + std::to_string(page) + ".png")).string();
            return true;
        }
        return false;
    }

    std::vector<std::pair<std::string, fs::path>> PsdLayerImporter::exportedFiles(const std::string& assetPath) {
        std::vector<std::pair<std::string, fs::path>> files;
        Memo memo;
        if (!ensure(assetPath, memo)) return files;
        files.push_back({"sheet", memo.dir / "sheet.json"});
        for (size_t p = 0; p < memo.pages; p++) files.push_back({"image#" + std::to_string(p), memo.dir / ("page" + std::to_string(p) + ".png")});
        return files;
    }

    bool PsdLayerImporter::import(const std::string& assetPath, PsdLayerImportResult& out) {
        fs::path assets;
        int ppuDefault = 1;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!project) return false;
            assets = assetsRoot;
            ppuDefault = projectPixelsPerUnit;
        }
        const fs::path absolute = (assets / assetPath).lexically_normal();
        TextureImportSettings settings;
        TextureImportSettings::load(absolute.string(), settings);
        std::ifstream in(absolute, std::ios::binary);
        if (!in) {
            out.error = "cannot read " + assetPath;
            return false;
        }
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        out = importPsdLayers(bytes, assetPath, settings.resolvedPixelsPerUnit(ppuDefault), sprite::readSpriteSkins(absolute));
        return out.ok;
    }

    bool PsdLayerImporter::writePrefab(const std::string& assetPath, std::string& error) {
        PsdLayerImportResult result;
        if (!import(assetPath, result)) {
            error = result.error;
            return false;
        }
        const fs::path absolute = (project->getAssetsPath() / assetPath).lexically_normal();
        TextureImportSettings settings;
        TextureImportSettings::load(absolute.string(), settings);
        const Guid sourceGuid = project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(absolute));
        const fs::path prefabFile = project->getAssetsPath() / prefabPathOf(assetPath);
        // an existing prefab keeps its GUID (scenes reference it), a new one's comes from its source
        Guid prefabGuid = sourceGuid.isNull() ? Guid() : Guid::derive(sourceGuid, "prefab");
        std::error_code ec;
        if (fs::exists(prefabFile, ec)) {
            try {
                const YAML::Node existing = YAML::LoadFile(prefabFile.string());
                if (existing["guid"] && Guid::isValidText(existing["guid"].as<std::string>())) prefabGuid = Guid::parse(existing["guid"].as<std::string>());
            } catch (const std::exception&) {
                // a broken prefab is written again whole
            }
        }
        if (prefabGuid.isNull()) prefabGuid = Guid::derive(Guid::derive(Guid(), assetPath), "prefab");
        const YAML::Node prefab = psdLayerPrefab(result, assetPath, sourceGuid, prefabGuid, settings.characterRig.value_or(false));
        YAML::Emitter emitter;
        emitter << prefab;
        io::AtomicWriteOptions options;
        options.skipIfUnchanged = true;
        const io::WriteResult written = io::writeFileAtomic(prefabFile, std::string(emitter.c_str()) + "\n", options);
        if (!written.ok()) {
            error = "cannot write " + prefabPathOf(assetPath) + ": " + written.message;
            return false;
        }
        return true;
    }

}
