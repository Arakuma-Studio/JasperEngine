// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "export/AssetStripping.h"

#include "asset/AssetFileWrite.h"
#include "asset/AssetIndex.h"
#include "Scene.h"
#include "asset/AssetRefSchema.h"
#include "asset/AssetTypes.h"
#include "asset/TextReferences.h"
#include "media/VideoFormats.h"
#include "pixelart/BitmapFont.h"
#include "util/EditorScripts.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    bool strippingGlobSegment(const char* glob, const char* text) {
        for (; *glob; glob++, text++) {
            if (*glob == '*') {
                for (const char* rest = text;; rest++) {
                    if (strippingGlobSegment(glob + 1, rest)) return true;
                    if (!*rest) return false;
                }
            }
            if (!*text || (*glob != '?' && *glob != *text)) return false;
        }
        return !*text;
    }

    std::vector<std::string> strippingSplit(const std::string& path) {
        std::vector<std::string> parts;
        size_t begin = 0;
        while (begin <= path.size()) {
            const size_t slash = path.find('/', begin);
            const size_t end = slash == std::string::npos ? path.size() : slash;
            if (end > begin) parts.push_back(path.substr(begin, end - begin));
            if (slash == std::string::npos) break;
            begin = slash + 1;
        }
        return parts;
    }

    bool strippingGlobParts(const std::vector<std::string>& glob, size_t g, const std::vector<std::string>& path, size_t p) {
        if (g == glob.size()) return p == path.size();
        if (glob[g] == "**") {
            for (size_t k = p; k <= path.size(); k++)
                if (strippingGlobParts(glob, g + 1, path, k)) return true;
            return false;
        }
        return p < path.size() && strippingGlobSegment(glob[g].c_str(), path[p].c_str()) && strippingGlobParts(glob, g + 1, path, p + 1);
    }

    // "a/b" + "c/../d.png" -> "a/b/d.png"; "" root: the path itself
    std::string strippingJoin(const std::string& root, const std::string& path) {
        const fs::path joined = root.empty() ? FileUtils::pathFromUtf8(path) : FileUtils::pathFromUtf8(root) / FileUtils::pathFromUtf8(path);
        return FileUtils::pathToGenericUtf8(joined.lexically_normal());
    }

}

const char* editor::assetStrippingModeName(AssetStrippingMode mode) {
    return mode == AssetStrippingMode::ReferencedOnly ? "referenced_only" : "ship_all";
}

bool editor::assetStrippingModeFromName(const std::string& name, AssetStrippingMode& mode) {
    if (name == "ship_all") mode = AssetStrippingMode::ShipAll;
    else if (name == "referenced_only") mode = AssetStrippingMode::ReferencedOnly;
    else return false;
    return true;
}

bool editor::buildglob::matches(const std::string& glob, const std::string& projectRelative) {
    std::string pattern = glob;
    while (!pattern.empty() && pattern.back() == '/') pattern.pop_back();
    if (pattern.empty()) return false;
    if (pattern.find_first_of("*?") == std::string::npos) {
        return projectRelative == pattern ||
               (projectRelative.size() > pattern.size() && projectRelative.compare(0, pattern.size(), pattern) == 0 && projectRelative[pattern.size()] == '/');
    }
    return strippingGlobParts(strippingSplit(pattern), 0, strippingSplit(projectRelative), 0);
}

const std::string* editor::buildglob::firstMatch(const std::vector<std::string>& globs, const std::string& projectRelative) {
    for (const std::string& glob : globs)
        if (matches(glob, projectRelative)) return &glob;
    return nullptr;
}

std::string editor::buildglob::forSelection(const std::string& projectRelative, bool folder) {
    return folder ? projectRelative + "/**" : projectRelative;
}

bool editor::buildglob::isValid(const std::string& glob, std::string& error) {
    if (glob.empty()) error = "an empty glob: give a project-relative path such as \"assets/levels/**\"";
    else if (glob.find('\\') != std::string::npos) error = "\"" + glob + "\" has a backslash: use '/'";
    else if (glob[0] == '/' || (glob.size() > 1 && glob[1] == ':')) error = "\"" + glob + "\" is absolute: give it relative to the project folder";
    else if (glob == ".." || glob.rfind("../", 0) == 0 || glob.find("/../") != std::string::npos) error = "\"" + glob + "\" leaves the project folder";
    else return true;
    return false;
}

AssetStrippingSettings editor::withBuildGlob(const AssetStrippingSettings& settings, bool exclude, const std::string& glob, bool on) {
    AssetStrippingSettings changed = settings;
    std::vector<std::string>& list = exclude ? changed.exclude : changed.alwaysInclude;
    const auto found = std::find(list.begin(), list.end(), glob);
    if (on && found == list.end()) list.push_back(glob);
    if (!on && found != list.end()) list.erase(found);
    return changed;
}

bool editor::AssetStrippingPlan::isStrippable(const std::string& projectRelative, const std::string& assetsDir) {
    const AssetTypeInfo* info = AssetTypes::find(FileUtils::pathFromUtf8(projectRelative), FileUtils::pathFromUtf8(assetsDir));
    if (!info) return VideoFormats::isVideoPath(projectRelative);
    switch (info->type) {
        case AssetType::Prefab: case AssetType::Material: case AssetType::Texture: case AssetType::Model:
        case AssetType::Audio: case AssetType::Font: case AssetType::UIDocument: case AssetType::Stylesheet:
        case AssetType::ColorLut: case AssetType::ParticleEffect: case AssetType::AnimationClip: case AssetType::PoseAnimation:
        case AssetType::VertexAnimation: case AssetType::AnimatorController: case AssetType::AvatarMask:
        case AssetType::AnimationMontage: case AssetType::AsepriteSprite: case AssetType::Tileset: case AssetType::SoundEvent:
        case AssetType::TitleArt: case AssetType::VoxelVolume: case AssetType::Mesh:
            return true;
        default:
            return false;
    }
}

std::string editor::AssetStrippingPlan::leftOutReason(const std::string& projectRelative) const {
    if (const std::string* glob = buildglob::firstMatch(settings.exclude, projectRelative)) return "excluded: " + *glob;
    if (!referencedOnly() || kept.count(projectRelative) || !isStrippable(projectRelative, assetsDir)) return "";
    if (buildglob::firstMatch(settings.alwaysInclude, projectRelative)) return "";
    return "unreferenced";
}

void editor::AssetStrippingPlan::keep(const std::string& projectRelative) {
    kept.insert(projectRelative);
}

editor::AssetStrippingPlan editor::AssetStrippingPlan::make(Project& project) {
    AssetStrippingPlan plan;
    plan.settings = project.getAssetStripping();
    const AssetIndex::Roots roots = project.getAssetRoots();
    plan.assetsDir = roots.assetsDir;
    if (!plan.referencedOnly()) return plan;

    AssetIndex& index = project.getAssetIndex();
    index.waitUntilReady();
    const AssetDatabase& database = project.getAssetDatabase();
    const fs::path projectPath = project.getProjectPath();

    std::vector<std::string> queue;
    auto add = [&](std::string path) {
        const size_t hash = path.find('#');
        if (hash != std::string::npos) path.erase(hash);
        path = strippingJoin(std::string(), path);
        if (path.empty() || path == "." || path == ".." || path.rfind("../", 0) == 0) return;
        if (plan.kept.insert(path).second) queue.push_back(path);
    };
    auto exists = [&](const std::string& path) {
        std::error_code ec;
        return fs::exists(projectPath / FileUtils::pathFromUtf8(path), ec);
    };
    // D3: a literal that names a file keeps it: relative to the assets folder (what the engine's
    // loaders take), to the project, or to the Lua folder; "asset://" and "lua://" name their root
    auto addLiteral = [&](std::string literal) {
        if (literal.empty() || literal.size() > 1024 || literal.find_first_of("\n\r\\") != std::string::npos) return;
        literal = literal.substr(0, literal.find('?'));
        std::vector<std::string> bases = {roots.assetsDir, std::string(), roots.luaDir};
        if (literal.rfind("asset://", 0) == 0) {
            literal = literal.substr(8);
            bases = {roots.assetsDir};
        } else if (literal.rfind("lua://", 0) == 0) {
            literal = literal.substr(6);
            bases = {roots.luaDir};
        }
        if (literal.empty() || literal[0] == '/' || literal.find(':') != std::string::npos || FileUtils::pathFromUtf8(literal).extension().empty()) return;
        for (const std::string& base : bases) {
            const std::string candidate = strippingJoin(base, literal);
            if (candidate.rfind("..", 0) != 0 && exists(candidate)) {
                add(candidate);
                return;
            }
        }
    };
    auto follow = [&](const DepEdge& edge) {
        if (!edge.target.isNull()) {
            const AssetDatabase::Resolution found = database.resolve(edge.target, edge.targetPath, edge.expected, true);
            if (found.found()) {
                add(found.path);
                return;
            }
        }
        if (!edge.targetPath.empty() && exists(edge.targetPath)) add(edge.targetPath);
    };

    // the build's scenes and what project.yaml names (its scene list and standalone prefabs as
    // edges; the window icon, the audio mixer and every other path as literals)
    for (const SceneProject& scene : project.getScenes()) {
        // a scene opened without being listed is in no build (S98 REQ-28)
        if (!scene.filepath.empty() && scene.inBuild) add(FileUtils::pathToGenericUtf8(project.normalizeToProjectRelative(projectPath / scene.filepath)));
    }
    // a scene never saved ships as the editor holds it (S98 REQ-4), and the index has no file of it
    for (SceneProject& scene : project.getScenes()) {
        if (!scene.filepath.empty() || !scene.scene || !scene.inBuild) continue;
        AssetRefSchema::forEachAssetRef(*scene.scene, [&](AssetRef& ref, const AssetRefSite& site) {
            DepEdge edge;
            edge.target = ref.guid;
            edge.targetPath = AssetIndex::toProjectRelative(roots, site.base, ref.path);
            edge.expected = site.expected;
            follow(edge);
        });
    }
    for (const DepEdge& edge : index.dependenciesOf(AssetIndex::projectReferrer(), 1)) follow(edge);
    try {
        std::function<void(const YAML::Node&)> walk = [&](const YAML::Node& node) {
            if (node.IsScalar()) addLiteral(node.Scalar());
            else if (node.IsSequence()) for (const YAML::Node& item : node) walk(item);
            else if (node.IsMap()) for (const auto& item : node) walk(item.second);
        };
        walk(YAML::LoadFile(FileUtils::pathToUtf8(projectPath / "project.yaml")));
    } catch (const std::exception&) {
        // an unreadable project.yaml names nothing more
    }
    // Lua and C++ sources ship whole, so what they name counts (and data files, which ship)
    std::vector<std::string> seeds;
    database.forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        const bool source = info.type == AssetType::Script ||
                            (info.type == AssetType::Data && FileUtils::pathFromUtf8(info.path).extension() == ".json");
        if (source || buildglob::firstMatch(plan.settings.alwaysInclude, info.path)) seeds.push_back(info.path);
    });
    for (const std::string& path : seeds) {
        const fs::path absolute = projectPath / FileUtils::pathFromUtf8(path);
        if (FileUtils::pathFromUtf8(path).extension() == ".lua" && EditorScripts::isEditorOnlyScript(absolute, projectPath)) continue;
        add(path);
    }

    for (size_t i = 0; i < queue.size(); i++) {
        const std::string path = queue[i];
        AssetDatabase::AssetInfo info;
        const bool known = database.getInfo(FileUtils::pathFromUtf8(path), info);
        if (known && AssetIndex::isReferrerType(info.type, path)) {
            for (const DepEdge& edge : index.dependenciesOf(info.guid, 1)) follow(edge);
        }
        std::string extension = FileUtils::pathToUtf8(FileUtils::pathFromUtf8(path).extension());
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if ((known && info.type == AssetType::Script) || extension == ".json") {
            std::string text;
            if (assetio::readFile(projectPath / FileUtils::pathFromUtf8(path), text)) {
                for (const std::string& literal : TextReferences::scanStringLiterals(text, extension == ".lua")) addLiteral(literal);
            }
        }
        // a bitmap font's sheets and TrueType file are not in the index
        if (Util::isBitmapFontFile(path)) {
            const std::string assetsRelative = roots.assetsDir.empty() ? path : FileUtils::pathToGenericUtf8(FileUtils::pathFromUtf8(path).lexically_relative(FileUtils::pathFromUtf8(roots.assetsDir)));
            if (std::shared_ptr<::jasperengine::pixelart::BitmapFont> font = ::jasperengine::pixelart::BitmapFont::load(assetsRelative)) {
                for (const std::string& dependency : font->getDependencies()) add(strippingJoin(roots.assetsDir, dependency));
            }
        }
    }
    return plan;
}
