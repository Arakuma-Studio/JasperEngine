// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleCooked3D.h"

#include "Log.h"
#include "io/Data.h"
#include "texture/CookedTextures.h"
#include "titleart/TitleArtHash.h"
#include "titleart/three_d/TitleMeshFile.h"

#include <cstdio>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    const char* const PLATFORM_NAMES[6] = {"macos", "ios", "web", "android", "linux", "windows"};   // Engine::Platform order

    bool readFile(const std::string& path, std::vector<uint8_t>& out) {
        Data data;
        if (data.open(path.c_str()) != FILEDATA_OK) return false;
        out.assign(data.getMemPtr(), data.getMemPtr() + data.length());
        return true;
    }

    struct CookedIndex {
        bool read = false;
        bool present = false;
        Json root;
    };

    // where je-titleart/ is: the assets root (relative paths), a test's folder
    std::string& cookedRoot() {
        static std::string root;
        return root;
    }

    std::string cookedPath(const std::string& file) {
        const std::string& root = cookedRoot();
        return (root.empty() ? std::string() : root + "/") + TITLE_COOKED_DIR + "/" + file;
    }

    CookedIndex& cookedIndex() {
        static CookedIndex index;
        if (index.read) return index;
        index.read = true;
        std::vector<uint8_t> bytes;
        if (!readFile(cookedPath(TITLE_COOKED_INDEX_FILE), bytes)) return index;
        Json root = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (!root.is_object() || root.value("version", 0) != TITLE_COOKED_INDEX_VERSION) {
            Log::error("Title Art: %s/%s is not a version %d index of cooked 3D data: 3D titles mesh and light at runtime", TITLE_COOKED_DIR,
                       TITLE_COOKED_INDEX_FILE, TITLE_COOKED_INDEX_VERSION);
            return index;
        }
        index.present = true;
        index.root = std::move(root);
        return index;
    }

    // environment cubes, one copy for every title that lights with them
    std::shared_ptr<const std::vector<uint8_t>> sharedFile(const std::string& path) {
        static std::map<std::string, std::weak_ptr<const std::vector<uint8_t>>> files;
        if (auto held = files[path].lock()) return held;
        auto bytes = std::make_shared<std::vector<uint8_t>>();
        if (!readFile(path, *bytes)) {
            Log::error("Title Art: the cooked environment %s is missing from the build", path.c_str());
            return nullptr;
        }
        std::shared_ptr<const std::vector<uint8_t>> out = bytes;
        files[path] = out;
        return out;
    }

    class CookedSource : public TitleCooked3DSource {
    public:
        CookedSource(std::string title, std::map<std::string, std::map<int, std::string>> meshFiles,
                     std::map<std::string, std::string> environmentFiles)
            : title(std::move(title)), meshFiles(std::move(meshFiles)), environmentFiles(std::move(environmentFiles)) {}

        void fill(int tier, TitleJobSpec& spec) override {
            spec.cookedMeshes3D = meshesAt(tier);
            if (environments.empty()) {
                for (const auto& [preset, file] : environmentFiles) {
                    if (auto bytes = sharedFile(cookedPath(file))) environments[preset] = bytes;
                }
            }
            spec.cookedEnvironments3D = environments;
        }

    private:
        std::string title;
        std::map<std::string, std::map<int, std::string>> meshFiles;    // layer id -> tier -> file
        std::map<std::string, std::string> environmentFiles;            // preset -> file
        std::map<int, std::map<std::string, std::shared_ptr<const TitleMesh>>> meshes;
        std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> environments;

        const std::map<std::string, std::shared_ptr<const TitleMesh>>& meshesAt(int tier) {
            auto loaded = meshes.find(tier);
            if (loaded != meshes.end()) return loaded->second;
            std::map<std::string, std::shared_ptr<const TitleMesh>>& out = meshes[tier];
            for (const auto& [layerId, files] : meshFiles) {
                auto it = files.find(tier);
                if (it == files.end()) continue;
                const std::string path = cookedPath(it->second);
                std::vector<uint8_t> bytes;
                std::string error;
                std::shared_ptr<TitleMesh> mesh = readFile(path, bytes) ? readTitleMesh(bytes, &error) : nullptr;
                if (!mesh) {
                    Log::error("Title Art '%s': the cooked mesh %s of layer %s does not load%s%s", title.c_str(), path.c_str(), layerId.c_str(),
                               error.empty() ? " (missing from the build)" : ": ", error.c_str());
                    continue;
                }
                out[layerId] = std::move(mesh);
            }
            return out;
        }
    };

    // "3d.x" is "style_3d.x"; "band:<id>.x" and "band#<n>.x" name a band's member x
    bool safeBandMember(const std::string& member) {
        return member.rfind("material", 0) == 0 || member.rfind("side", 0) == 0 || member == "name";
    }

    // a rigged letter's transforms are matrices (S86 R4): its CharAdjust's own rig, or tilts or dz
    bool charRigged(const Json& layer, const std::string& index) {
        auto chars = layer.find("chars");
        if (chars == layer.end() || !chars->is_object()) return false;
        auto c = chars->find(index);
        if (c == chars->end() || !c->is_object()) return false;
        if (c->value("rig", false)) return true;
        for (const char* key : {"dz", "tilt_x", "tilt_y"}) {
            auto v = c->find(key);
            if (v != c->end() && v->is_number() && v->get<double>() != 0.0) return true;
        }
        return false;
    }

    bool safeField(const Json& layer, std::string field) {
        if (field.rfind("3d.", 0) == 0) field = "style_3d." + field.substr(3);
        auto startsWith = [&field](const char* prefix) {
            const std::string p = prefix;
            return field == p || field.rfind(p + ".", 0) == 0 || field.rfind(p + "[", 0) == 0;
        };
        for (const char* prefix : {"transform", "opacity", "blend", "visible", "effects", "style_3d.face", "style_3d.side",
                                   "style_3d.bevel_material", "style_3d.lighting", "style_3d.glint", "style_3d.ao", "style_3d.env_lock",
                                   "style_3d.camera", "style_3d.skew", "style_3d.face_image", "style_3d.edge_image"}) {
            if (startsWith(prefix)) return true;
        }
        if (field.rfind("fx:", 0) == 0) return true;
        if (field.rfind("band:", 0) == 0 || field.rfind("band#", 0) == 0) {
            const size_t dot = field.find('.');
            return dot != std::string::npos && safeBandMember(field.substr(dot + 1));
        }
        if (field.rfind("style_3d.bands[", 0) == 0) {
            const size_t close = field.find("].");
            return close != std::string::npos && safeBandMember(field.substr(close + 2));
        }
        // the 2D style is not drawn while 3D, unless the 3D style is derived from it (S86 R2, R64)
        if (startsWith("style")) {
            auto style3d = layer.find("style_3d");
            return style3d != layer.end() && style3d->is_object();
        }
        if (field.rfind("chars.", 0) == 0) {
            const size_t dot = field.find('.', 6);
            if (dot == std::string::npos) return false;
            const std::string index = field.substr(6, dot - 6);
            const std::string member = field.substr(dot + 1);
            if (member.rfind("face_3d", 0) == 0 || member.rfind("side_3d", 0) == 0 || member.rfind("bands_3d", 0) == 0) return true;
            for (const char* rig : {"dz", "tilt_x", "tilt_y", "offset", "rotation", "scale", "h_scale", "v_scale"}) {
                if (member == rig || member.rfind(std::string(rig) + ".", 0) == 0) return charRigged(layer, index);
            }
            return false;
        }
        return false;
    }

}

std::string titleart::titleCookedMeshFile(const std::string& titlePath, const std::string& layerId, TitleTier tier) {
    char name[64];
    std::snprintf(name, sizeof(name), "meshes/%016llx-%s%s", (unsigned long long)fnv1a64(titlePath + "\x1f" + layerId), titleTierName(tier),
                  TITLE_MESH_EXTENSION);
    return name;
}

std::string titleart::titleCookedEnvironmentFile(const std::string& family, const std::string& preset) {
    return "environments/" + family + "/" + preset + ".ktx2";
}

bool titleart::titleArt3DFixedGeometry(const Json& root, const Json& layer, std::string* why) {
    const std::string id = layer.value("id", std::string());
    auto exposed = root.find("exposed");
    if (exposed == root.end() || !exposed->is_array()) return true;
    for (const Json& param : *exposed) {
        if (!param.is_object()) continue;
        auto targets = param.find("targets");
        if (targets == param.end() || !targets->is_array()) continue;
        for (const Json& target : *targets) {
            if (!target.is_object() || target.value("layer", std::string()) != id) continue;
            const std::string field = target.value("field", std::string());
            if (safeField(layer, field)) continue;
            if (why) *why = "the Exposed parameter '" + param.value("name", std::string()) + "' sets its " + field;
            return false;
        }
    }
    return true;
}

std::shared_ptr<TitleCooked3DSource> titleart::titleCooked3DFor(const std::string& titlePath) {
    const CookedIndex& index = cookedIndex();
    if (!index.present) return nullptr;
    const Json& titles = index.root.value("titles", Json::object());
    auto entry = titles.find(titlePath);
    if (entry == titles.end() || !entry->is_object()) return nullptr;
    std::map<std::string, std::map<int, std::string>> meshFiles;
    const Json meshes = entry->value("meshes", Json::object());
    for (const auto& [layerId, tiers] : meshes.items()) {
        if (!tiers.is_object()) continue;
        for (const auto& [name, file] : tiers.items()) {
            TitleTier tier;
            if (file.is_string() && titleTierFromName(name, tier)) meshFiles[layerId][(int)tier] = file.get<std::string>();
        }
    }
    // the build's texture family first, then any other the export cooked (a device without the
    // family's format decodes a cube to RGBA16F)
    std::map<std::string, std::string> environmentFiles;
    const Json& families = index.root.value("environments", Json::object());
    std::vector<std::string> order = {CookedTextures::activeFamily()};
    for (const auto& [family, presets] : families.items()) {
        if (family != order.front()) order.push_back(family);
    }
    for (const Json& preset : entry->value("environments", Json::array())) {
        if (!preset.is_string()) continue;
        for (const std::string& family : order) {
            auto f = families.find(family);
            if (f == families.end() || !f->is_object()) continue;
            auto file = f->find(preset.get<std::string>());
            if (file == f->end() || !file->is_string()) continue;
            environmentFiles[preset.get<std::string>()] = file->get<std::string>();
            break;
        }
    }
    if (meshFiles.empty() && environmentFiles.empty()) return nullptr;
    return std::make_shared<CookedSource>(titlePath, std::move(meshFiles), std::move(environmentFiles));
}

bool titleart::titleCookedProjectTier(int platform, TitleTier& out) {
    const CookedIndex& index = cookedIndex();
    if (!index.present || platform < 0 || platform >= 6) return false;
    const Json& tiers = index.root.value("realtime_3d_quality", Json::object());
    auto it = tiers.find(PLATFORM_NAMES[platform]);
    TitleTier tier;
    if (it == tiers.end() || !it->is_string() || !titleTierFromName(it->get<std::string>(), tier) || tier == TitleTier::Bake) return false;
    out = tier;
    return true;
}

void titleart::resetTitleCookedIndexForTesting(const std::string& root) {
    cookedRoot() = root;
    cookedIndex() = CookedIndex();
}
