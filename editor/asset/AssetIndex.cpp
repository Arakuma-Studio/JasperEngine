// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetIndex.h"

#include "Out.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetRefSchema.h"
#include "asset/ModelSubAssets.h"
#include "asset/TextReferences.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/ModelSubClips.h"
#include "audio/SoundEventIO.h"
#include "util/Util.h"
#include "voxel/VoxelFile.h"
#include "voxel/VoxelGenerator.h"

#include "json.hpp"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleModules.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    // A cache's text. A file name in another encoding than UTF-8 can't be JSON: its entries are
    // left out (read from disk again next time) instead of the dump throwing, which failed the
    // project's load
    std::string cacheText(Json& root, const char* listKey) {
        try {
            return root.dump() + "\n";
        } catch (const Json::type_error&) {
            Json kept = Json::array();
            for (Json& item : root[listKey]) {
                try {
                    (void)item.dump();
                    kept.push_back(std::move(item));
                } catch (const Json::type_error&) {
                }
            }
            root[listKey] = std::move(kept);
            return root.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
        }
    }

    constexpr int indexCacheVersion = 1;

    std::string indexExtension(const std::string& path) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    std::string indexJoinRelative(const std::string& root, const std::string& path) {
        if (root.empty() || root == ".") {
            return fs::path(path).lexically_normal().generic_string();
        }
        return (fs::path(root) / path).lexically_normal().generic_string();
    }

    void statFile(const fs::path& full, uint64_t& size, int64_t& time) {
        std::error_code ec;
        size = static_cast<uint64_t>(fs::file_size(full, ec));
        if (ec) size = 0;
        const auto stamp = fs::last_write_time(full, ec);
        time = ec ? 0 : static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.time_since_epoch()).count());
    }

    Json indexEdgeJson(const DepEdge& edge) {
        Json item;
        if (!edge.target.isNull()) item["t"] = edge.target.str();
        item["p"] = edge.targetPath;
        item["k"] = depEdgeKindName(edge.kind);
        item["f"] = edge.field;
        if (edge.ordinal) item["o"] = edge.ordinal;
        if (edge.entity) item["e"] = edge.entity;
        if (!edge.entityPath.empty()) item["ep"] = edge.entityPath;
        if (!edge.component.empty()) item["c"] = edge.component;
        if (edge.line) item["l"] = edge.line;
        if (edge.expected != AssetType::Unknown) item["x"] = assetTypeName(edge.expected);
        return item;
    }

    DepEdge edgeFrom(const Json& item, const Guid& referrer) {
        DepEdge edge;
        edge.referrer = referrer;
        edge.target = Guid::parse(item.value("t", std::string()));
        edge.targetPath = item.value("p", std::string());
        edge.kind = depEdgeKindFromName(item.value("k", std::string()));
        edge.field = item.value("f", std::string());
        edge.ordinal = item.value("o", 0u);
        edge.entity = item.value("e", static_cast<uint64_t>(0));
        edge.entityPath = item.value("ep", std::string());
        edge.component = item.value("c", std::string());
        edge.line = item.value("l", 0u);
        edge.expected = assetTypeFromName(item.value("x", std::string()));
        return edge;
    }

    // The project's scene ids -> paths (child scenes are referenced by id)
    std::unordered_map<uint32_t, std::string> sceneIds(const fs::path& projectPath) {
        std::unordered_map<uint32_t, std::string> ids;
        try {
            const YAML::Node root = YAML::LoadFile((projectPath / "project.yaml").string());
            for (const YAML::Node& scene : root["scenes"]) {
                if (scene["id"] && scene["filepath"]) {
                    ids[scene["id"].as<uint32_t>(0)] = scene["filepath"].as<std::string>();
                }
            }
        } catch (const std::exception&) {
        }
        return ids;
    }

}

AssetIndex::~AssetIndex() {
    close();
}

Guid AssetIndex::projectReferrer() {
    static const Guid guid = Guid::derive(Guid(), "project.yaml");
    return guid;
}

std::string AssetIndex::toProjectRelative(const Roots& roots, AssetPathBase base, const std::string& hint) {
    if (hint.empty()) {
        return hint;
    }
    std::string container, key;
    const bool sub = splitSubAssetPath(hint, container, key);
    const std::string path = sub ? container : hint;
    std::string relative;
    switch (base) {
        case AssetPathBase::Assets: relative = indexJoinRelative(roots.assetsDir, path); break;
        case AssetPathBase::Lua: relative = indexJoinRelative(roots.luaDir, path); break;
        case AssetPathBase::Project: relative = fs::path(path).lexically_normal().generic_string(); break;
    }
    return sub ? relative + "#" + key : relative;
}

std::string AssetIndex::fromProjectRelative(const Roots& roots, AssetPathBase base, const std::string& projectRelative) {
    const std::string& root = base == AssetPathBase::Assets ? roots.assetsDir : base == AssetPathBase::Lua ? roots.luaDir : std::string();
    if (root.empty() || root == "." || base == AssetPathBase::Project) {
        return projectRelative;
    }
    std::string container, key;
    const bool sub = splitSubAssetPath(projectRelative, container, key);
    const fs::path relative = fs::path(sub ? container : projectRelative).lexically_relative(root);
    std::string text = relative.empty() ? (sub ? container : projectRelative) : relative.generic_string();
    return sub ? text + "#" + key : text;
}

bool AssetIndex::isReferrerType(AssetType type, const std::string& relativePath) {
    switch (type) {
        case AssetType::Scene: case AssetType::Prefab: case AssetType::Material:
        case AssetType::UIDocument: case AssetType::Stylesheet: case AssetType::Shader: case AssetType::Model:
        case AssetType::ParticleEffect:
        case AssetType::AnimatorController: case AssetType::AnimationMontage:
        case AssetType::SoundEvent:
        // its audio cues' sounds (S40 R3)
        case AssetType::AnimationClip:
        case AssetType::VoxelVolume:
            return true;
        case AssetType::Script:
            return indexExtension(relativePath) == ".lua";
        default:
            return false;
    }
}

std::vector<DepEdge> AssetIndex::extractFile(const AssetDatabase& database, const fs::path& projectPath, const Roots& roots,
                                             const std::string& relativePath, AssetType type, const Guid& referrer) {
    std::vector<DepEdge> edges;
    const fs::path full = projectPath / relativePath;

    // a reference read from YAML: the GUID it holds, else what its hint names
    auto addRef = [&](const AssetRefFound& found) {
        DepEdge edge;
        edge.referrer = referrer;
        edge.kind = found.site.kind;
        edge.field = found.site.locator(found.scope);
        edge.ordinal = found.site.ordinal;
        edge.entity = found.fileEntity;
        edge.entityPath = found.entityPath;
        edge.component = found.site.component;
        edge.expected = found.site.expected;
        edge.target = found.ref.guid;
        edge.targetPath = toProjectRelative(roots, found.site.base, found.ref.path);
        if (edge.target.isNull()) {
            const AssetDatabase::Resolution resolution = database.resolve(Guid(), edge.targetPath, edge.expected, false);
            if (resolution.found()) edge.target = resolution.guid;
        }
        if (isValidDepEdge(edge)) edges.push_back(std::move(edge));
    };
    // a reference in source text: a path, with the asset's GUID when it is one
    auto addText = [&](const TextReference& reference, const std::string& target) {
        DepEdge edge;
        edge.referrer = referrer;
        edge.kind = reference.kind;
        edge.field = "text:" + std::to_string(reference.line);
        edge.line = reference.line;
        edge.targetPath = target;
        const AssetDatabase::Resolution resolution = database.lookupPath(target);
        if (resolution.found()) {
            edge.target = resolution.guid;
            edge.expected = resolution.type;
        }
        if (isValidDepEdge(edge)) edges.push_back(std::move(edge));
    };
    const std::string directory = fs::path(relativePath).parent_path().generic_string();
    auto besideFile = [&](const std::string& target) { return indexJoinRelative(directory, target); };

    try {
        switch (type) {
            case AssetType::Scene:
            case AssetType::Prefab: {
                const YAML::Node root = YAML::LoadFile(full.string());
                std::unordered_map<uint32_t, std::string> ids;
                bool idsRead = false;
                AssetRefSchema::extractDocumentRefs(root, type == AssetType::Prefab, [&](const AssetRefFound& found) {
                    if (found.sceneId == 0) {
                        addRef(found);
                        return;
                    }
                    if (!idsRead) {
                        ids = sceneIds(projectPath);
                        idsRead = true;
                    }
                    auto it = ids.find(found.sceneId);
                    if (it == ids.end()) return;
                    AssetRefFound child = found;
                    child.ref = AssetRef(it->second);
                    addRef(child);
                });
                break;
            }
            case AssetType::Material:
                AssetRefSchema::extractMaterialFileRefs(YAML::LoadFile(full.string()), addRef);
                break;
            case AssetType::UIDocument:
            case AssetType::Stylesheet: {
                std::string text;
                if (!assetio::readFile(full, text)) break;
                const std::vector<TextReference> references = type == AssetType::UIDocument && indexExtension(relativePath) != ".css"
                    ? TextReferences::scanHtml(text) : TextReferences::scanCss(text);
                for (const TextReference& reference : references) {
                    addText(reference, besideFile(reference.target));
                }
                break;
            }
            case AssetType::Script: {
                std::string text;
                if (!assetio::readFile(full, text)) break;
                for (const TextReference& reference : TextReferences::scanLua(text)) {
                    const std::string module = TextReferences::luaModulePath(reference.target);
                    std::string target = indexJoinRelative(roots.luaDir, module);
                    if (!database.lookupPath(target).found()) {
                        const std::string init = indexJoinRelative(roots.luaDir, module.substr(0, module.size() - 4) + "/init.lua");
                        if (database.lookupPath(init).found()) target = init;
                    }
                    addText(reference, target);
                }
                break;
            }
            case AssetType::Shader: {
                std::string text;
                if (!assetio::readFile(full, text)) break;
                for (const TextReference& reference : TextReferences::scanShader(text)) {
                    std::string target = besideFile(reference.target);
                    for (const std::string& candidate : {target, indexJoinRelative(std::string(), reference.target), indexJoinRelative(roots.assetsDir, reference.target)}) {
                        if (database.lookupPath(candidate).found()) {
                            target = candidate;
                            break;
                        }
                    }
                    addText(reference, target);
                }
                break;
            }
            case AssetType::ParticleEffect: {
                // the effect's textures, models and child effects (assets-relative, with GUIDs)
                std::string text;
                if (!assetio::readFile(full, text)) break;
                particles::ParticleEffectDocument document;
                std::vector<particles::Diagnostic> diagnostics;
                // (an effect with errors still names its assets)
                particles::ParticleEffectCodec::read(text, document, diagnostics);
                particles::ParticleModules::visitAssets(document, [&](std::string& path, std::string& guid, particles::ParticleModules::AssetKind kind, const std::string& field) {
                    if (path.empty()) return;
                    DepEdge edge;
                    edge.referrer = referrer;
                    edge.kind = DepEdgeKind::Other;
                    edge.field = field;
                    edge.expected = kind == particles::ParticleModules::AssetKind::Model ? AssetType::Model
                                  : (kind == particles::ParticleModules::AssetKind::Effect ? AssetType::ParticleEffect : AssetType::Texture);
                    edge.target = Guid::parse(guid);
                    edge.targetPath = toProjectRelative(roots, AssetPathBase::Assets, path);
                    if (edge.target.isNull()) {
                        const AssetDatabase::Resolution resolution = database.resolve(Guid(), edge.targetPath, edge.expected, false);
                        if (resolution.found()) edge.target = resolution.guid;
                    }
                    if (isValidDepEdge(edge)) edges.push_back(std::move(edge));
                });
                break;
            }
            case AssetType::VoxelVolume: {
                // T6 R77: what its generator reads, a Noise Graph, a mesh or what it blends
                std::string text;
                if (!assetio::readFile(full, text)) break;
                const voxel::VolumeFileResult info = voxel::readVolumeFileInfo(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
                voxel::GeneratorDesc generator;
                if (!info.ok || !voxel::generatorFromJson(info.info.generatorJson, generator)) break;
                auto addSource = [&](const std::string& kind, const std::string& guid, const std::string& path, const char* field) {
                    if (guid.empty() && path.empty()) return;
                    DepEdge edge;
                    edge.referrer = referrer;
                    edge.kind = DepEdgeKind::Other;
                    edge.field = field;
                    edge.expected = kind == "mesh" ? AssetType::Model : AssetType::Unknown;
                    edge.target = Guid::parse(guid);
                    edge.targetPath = toProjectRelative(roots, AssetPathBase::Assets, path);
                    if (edge.target.isNull()) {
                        const AssetDatabase::Resolution resolution = database.resolve(Guid(), edge.targetPath, edge.expected, false);
                        if (resolution.found()) edge.target = resolution.guid;
                    }
                    if (isValidDepEdge(edge)) edges.push_back(std::move(edge));
                };
                addSource(generator.kind, generator.sourceGuid, generator.sourcePath, "generator.source");
                addSource(generator.otherKind, generator.otherGuid, generator.otherPath, "generator.other");
                break;
            }
            case AssetType::AnimatorController:
            case AssetType::AnimationMontage: {
                // the clips its states, trees and montage play and the masks of its layers
                // (assets-relative {path, guid}; a "<model>#<clip>" names its model)
                std::string text;
                if (!assetio::readFile(full, text)) break;
                std::vector<std::pair<anim::AnimatorAssetRef, std::string>> refs;
                if (type == AssetType::AnimatorController){
                    anim::ControllerParseResult parsed = anim::parseController(text);
                    if (parsed.ok) anim::visitControllerRefs(parsed.data, [&](anim::AnimatorAssetRef& ref, const std::string& field){ refs.push_back({ref, field}); });
                }else{
                    anim::MontageParseResult parsed = anim::parseMontage(text);
                    if (parsed.ok) refs.push_back({parsed.data.clip, "clip"});
                }
                for (const auto& [ref, field] : refs){
                    if (ref.path.empty()) continue;
                    anim::SubClipReference subClip;
                    const bool isSubClip = anim::parseSubClipReference(ref.path, subClip);
                    DepEdge edge;
                    edge.referrer = referrer;
                    edge.kind = DepEdgeKind::Other;
                    edge.field = field;
                    edge.expected = isSubClip ? AssetType::Model :
                                    (Util::isAvatarMaskFile(ref.path) ? AssetType::AvatarMask : AssetType::AnimationClip);
                    edge.target = isSubClip ? Guid() : Guid::parse(ref.guid);
                    edge.targetPath = toProjectRelative(roots, AssetPathBase::Assets, isSubClip ? subClip.model : ref.path);
                    if (edge.target.isNull()) {
                        const AssetDatabase::Resolution resolution = database.resolve(Guid(), edge.targetPath, edge.expected, false);
                        if (resolution.found()) edge.target = resolution.guid;
                    }
                    if (isValidDepEdge(edge)) edges.push_back(std::move(edge));
                }
                break;
            }
            case AssetType::SoundEvent: {
                // one edge per clip (assets-relative {guid, path})
                std::string text;
                if (!assetio::readFile(full, text)) break;
                SoundEventDef event;
                if (!SoundEventIO::parse(text, event)) break;
                for (size_t i = 0; i < event.clips.size(); i++) {
                    const AssetRef& clip = event.clips[i].clip;
                    DepEdge edge;
                    edge.referrer = referrer;
                    edge.kind = DepEdgeKind::Other;
                    edge.field = "clips[" + std::to_string(i) + "].clip";
                    edge.expected = AssetType::Audio;
                    edge.target = clip.guid;
                    edge.targetPath = toProjectRelative(roots, AssetPathBase::Assets, clip.path);
                    if (edge.target.isNull()) {
                        const AssetDatabase::Resolution resolution = database.resolve(Guid(), edge.targetPath, edge.expected, false);
                        if (resolution.found()) edge.target = resolution.guid;
                    }
                    if (isValidDepEdge(edge)) edges.push_back(std::move(edge));
                }
                break;
            }
            case AssetType::AnimationClip: {
                // one edge per audio cue's sound (S40 R3): an audio clip or a Sound Event
                std::string text;
                if (!assetio::readFile(full, text) || text.find("\"audio\"") == std::string::npos) break;
                anim::ClipParseResult parsed = anim::parseClip(text);
                if (!parsed.ok()) break;
                const std::vector<ClipAudioCue>& cues = parsed.data->audio;
                for (size_t i = 0; i < cues.size(); i++) {
                    if (cues[i].kind != ClipCueKind::Sound || !cues[i].sound.isSet()) continue;
                    DepEdge edge;
                    edge.referrer = referrer;
                    edge.kind = DepEdgeKind::Other;
                    edge.field = "audio[" + std::to_string(i) + "].sound";
                    edge.expected = Util::isSoundEventFile(cues[i].sound.path) ? AssetType::SoundEvent : AssetType::Audio;
                    edge.target = cues[i].sound.guid;
                    edge.targetPath = toProjectRelative(roots, AssetPathBase::Assets, cues[i].sound.path);
                    if (edge.target.isNull()) {
                        const AssetDatabase::Resolution resolution = database.resolve(Guid(), edge.targetPath, edge.expected, false);
                        if (resolution.found()) edge.target = resolution.guid;
                    }
                    if (isValidDepEdge(edge)) edges.push_back(std::move(edge));
                }
                break;
            }
            case AssetType::Model: {
                ModelContents contents;
                if (!scanModelFile(full, contents)) break;
                for (const std::string& uri : contents.uris) {
                    if (!TextReferences::isLocalTarget(uri)) continue;
                    addText({uri, 0, DepEdgeKind::ModelUri}, besideFile(uri));
                }
                break;
            }
            default:
                break;
        }
    } catch (const std::exception& e) {
        Out::warning("Could not index the references of %s: %s", relativePath.c_str(), e.what());
    }
    return edges;
}

// ==================================================================== lifecycle

void AssetIndex::open(AssetDatabase* db, const fs::path& path, const Roots& projectRoots) {
    close();
    {
        std::lock_guard<std::mutex> lock(mutex);
        database = db;
        projectPath = path;
        roots = projectRoots;
        referrers.clear();
        ready = false;
        stopBuild = false;
    }
    loadCache();
    buildCancel = jobs::CancelToken();
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "asset-index-build";
    options.cancel = &buildCancel;
    buildTask = jobs::submit([this] { build(); }, options);
}

void AssetIndex::close() {
    stopBuild = true;
    buildCancel.cancel();
    // a build not started never runs; a running one sees stopBuild (no lock held: it takes ours)
    buildTask.cancel();
    buildTask.wait();
    buildTask = jobs::Task<void>();
    std::lock_guard<std::mutex> lock(mutex);
    database = nullptr;
    projectPath.clear();
    referrers.clear();
    byTarget.clear();
    byTargetPath.clear();
    ready = false;
    version++;
    readyChanged.notify_all();
}

bool AssetIndex::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex);
    return database != nullptr;
}

bool AssetIndex::isReady() const {
    return ready.load();
}

void AssetIndex::waitUntilReady() {
    // the build job (a wait runs it here when no worker has started it: no lock is held)
    jobs::Task<void> task = buildTask;
    task.wait();
}

void AssetIndex::setOverlayProvider(OverlayProvider provider) {
    std::lock_guard<std::mutex> lock(mutex);
    overlay = std::move(provider);
}

uint64_t AssetIndex::getVersion() const {
    return version.load();
}

fs::path AssetIndex::cacheFile() const {
    return projectPath / ".jasperengine" / "assetdb" / "deps.json";
}

void AssetIndex::loadCache() {
    std::string text;
    if (!assetio::readFile(cacheFile(), text)) {
        return;
    }
    Json root = Json::parse(text, nullptr, false);
    if (!root.is_object() || root.value("version", 0) != indexCacheVersion || !root.contains("referrers")) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    for (const Json& item : root["referrers"]) {
        const Guid guid = Guid::parse(item.value("g", std::string()));
        if (guid.isNull()) continue;
        Referrer referrer;
        referrer.path = item.value("p", std::string());
        referrer.size = item.value("sz", static_cast<uint64_t>(0));
        referrer.writeTime = item.value("mt", static_cast<int64_t>(0));
        if (item.contains("e")) {
            for (const Json& edge : item["e"]) referrer.edges.push_back(edgeFrom(edge, guid));
        }
        referrers[guid] = std::move(referrer);
    }
}

void AssetIndex::saveCache() const {
    Json list = Json::array();
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (projectPath.empty()) return;
        for (const auto& [guid, referrer] : referrers) {
            Json item;
            item["g"] = guid.str();
            item["p"] = referrer.path;
            item["sz"] = referrer.size;
            item["mt"] = referrer.writeTime;
            Json edges = Json::array();
            for (const DepEdge& edge : referrer.edges) edges.push_back(indexEdgeJson(edge));
            item["e"] = edges;
            list.push_back(item);
        }
    }
    Json root;
    root["version"] = indexCacheVersion;
    root["referrers"] = list;
    assetio::writeFile(cacheFile(), cacheText(root, "referrers"), assetio::Durability::Cache);
}

bool AssetIndex::readReferrer(const AssetDatabase::AssetInfo& info, Referrer& referrer) const {
    referrer.path = info.path;
    statFile(projectPath / info.path, referrer.size, referrer.writeTime);
    referrer.edges = extractFile(*database, projectPath, roots, info.path, info.type, info.guid);
    return true;
}

void AssetIndex::build() {
    AssetDatabase* db;
    {
        std::lock_guard<std::mutex> lock(mutex);
        db = database;
    }
    if (!db) return;

    std::vector<AssetDatabase::AssetInfo> candidates;
    db->forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        if (isReferrerType(info.type, info.path)) candidates.push_back(info);
    });

    // the referrers parsed in parallel (R22): each unit reads one file; the cache answers for
    // the unchanged ones
    std::vector<Referrer> parsed(candidates.size());
    const std::function<bool()> stop = [this] { return stopBuild.load() || jobs::cancelRequested(); };
    jobs::ParallelForOptions parallel;
    parallel.lane = jobs::Lane::Background;
    parallel.stop = &stop;
    const bool complete = jobs::parallelFor(candidates.size(), [&](size_t i) {
        const AssetDatabase::AssetInfo& info = candidates[i];
        uint64_t size = 0;
        int64_t time = 0;
        statFile(projectPath / info.path, size, time);
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto cached = referrers.find(info.guid);
            if (cached != referrers.end() && cached->second.path == info.path && cached->second.size == size && cached->second.writeTime == time) {
                parsed[i] = cached->second;
                return true;
            }
        }
        readReferrer(info, parsed[i]);
        return true;
    }, parallel);
    if (!complete || stopBuild) return;
    std::unordered_map<Guid, Referrer> fresh;
    fresh.reserve(candidates.size() + 1);
    for (size_t i = 0; i < candidates.size(); i++) {
        fresh[candidates[i].guid] = std::move(parsed[i]);
    }
    // project.yaml
    {
        AssetDatabase::AssetInfo project;
        project.guid = projectReferrer();
        project.path = "project.yaml";
        Referrer referrer;
        referrer.path = project.path;
        statFile(projectPath / project.path, referrer.size, referrer.writeTime);
        try {
            const YAML::Node root = YAML::LoadFile((projectPath / "project.yaml").string());
            AssetRefSchema::extractProjectRefs(root, [&](const AssetRefFound& found) {
                DepEdge edge;
                edge.referrer = project.guid;
                edge.kind = found.site.kind;
                edge.field = found.site.locator(found.scope);
                edge.expected = found.site.expected;
                edge.target = found.ref.guid;
                edge.targetPath = toProjectRelative(roots, found.site.base, found.ref.path);
                if (edge.target.isNull()) {
                    const AssetDatabase::Resolution resolution = db->resolve(Guid(), edge.targetPath, edge.expected, false);
                    if (resolution.found()) edge.target = resolution.guid;
                }
                if (isValidDepEdge(edge)) referrer.edges.push_back(edge);
            });
        } catch (const std::exception&) {
        }
        fresh[project.guid] = std::move(referrer);
    }
    if (stopBuild) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        referrers = std::move(fresh);
        rebuildReverse();
        ready = true;
        version++;
    }
    readyChanged.notify_all();
    saveCache();
}

void AssetIndex::rebuildReverse() {
    byTarget.clear();
    byTargetPath.clear();
    for (const auto& [guid, referrer] : referrers) {
        for (const DepEdge& edge : referrer.edges) {
            if (!edge.target.isNull()) {
                std::vector<Guid>& list = byTarget[edge.target];
                if (list.empty() || list.back() != guid) list.push_back(guid);
            }
            if (!edge.targetPath.empty()) {
                std::vector<Guid>& list = byTargetPath[edge.targetPath];
                if (list.empty() || list.back() != guid) list.push_back(guid);
            }
        }
    }
}

void AssetIndex::linkReverse(const Guid& guid, const Referrer& referrer) {
    for (const DepEdge& edge : referrer.edges) {
        if (!edge.target.isNull()) {
            std::vector<Guid>& list = byTarget[edge.target];
            if (std::find(list.begin(), list.end(), guid) == list.end()) list.push_back(guid);
        }
        if (!edge.targetPath.empty()) {
            std::vector<Guid>& list = byTargetPath[edge.targetPath];
            if (std::find(list.begin(), list.end(), guid) == list.end()) list.push_back(guid);
        }
    }
}

void AssetIndex::unlinkReverse(const Guid& guid, const Referrer& referrer) {
    auto drop = [&guid](auto& map, const auto& key) {
        auto it = map.find(key);
        if (it == map.end()) return;
        it->second.erase(std::remove(it->second.begin(), it->second.end(), guid), it->second.end());
        if (it->second.empty()) map.erase(it);
    };
    for (const DepEdge& edge : referrer.edges) {
        if (!edge.target.isNull()) drop(byTarget, edge.target);
        if (!edge.targetPath.empty()) drop(byTargetPath, edge.targetPath);
    }
}

void AssetIndex::replaceReferrer(const Guid& guid, Referrer&& referrer) {
    auto old = referrers.find(guid);
    if (old != referrers.end()) {
        unlinkReverse(guid, old->second);
    }
    linkReverse(guid, referrer);
    referrers[guid] = std::move(referrer);
}

void AssetIndex::update(const std::vector<AssetDatabase::Change>& changes) {
    bool projectChanged = false;
    for (const AssetDatabase::Change& change : changes) {
        const Guid guid = Guid::parse(change.guid);
        if (change.type == AssetDatabase::Change::Type::Removed) {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = referrers.find(guid);
            if (it != referrers.end()) {
                unlinkReverse(guid, it->second);
                referrers.erase(it);
                version++;
            }
            continue;
        }
        updateReferrer(change.path.generic_string());
        projectChanged = true;
    }
    if (projectChanged) {
        updateReferrer("project.yaml");
    }
}

void AssetIndex::updateReferrer(const std::string& relativePath) {
    AssetDatabase* db;
    {
        std::lock_guard<std::mutex> lock(mutex);
        db = database;
    }
    if (!db) return;
    if (relativePath == "project.yaml") {
        // the project's lists: re-read as in a build
        Referrer referrer;
        referrer.path = relativePath;
        statFile(projectPath / relativePath, referrer.size, referrer.writeTime);
        try {
            const YAML::Node root = YAML::LoadFile((projectPath / relativePath).string());
            AssetRefSchema::extractProjectRefs(root, [&](const AssetRefFound& found) {
                DepEdge edge;
                edge.referrer = projectReferrer();
                edge.kind = found.site.kind;
                edge.field = found.site.locator(found.scope);
                edge.expected = found.site.expected;
                edge.target = found.ref.guid;
                edge.targetPath = toProjectRelative(roots, found.site.base, found.ref.path);
                if (edge.target.isNull()) {
                    const AssetDatabase::Resolution resolution = db->resolve(Guid(), edge.targetPath, edge.expected, false);
                    if (resolution.found()) edge.target = resolution.guid;
                }
                if (isValidDepEdge(edge)) referrer.edges.push_back(edge);
            });
        } catch (const std::exception&) {
        }
        std::lock_guard<std::mutex> lock(mutex);
        replaceReferrer(projectReferrer(), std::move(referrer));
        version++;
        return;
    }
    AssetDatabase::AssetInfo info;
    if (!db->getInfo(fs::path(relativePath), info) || !isReferrerType(info.type, info.path)) {
        return;
    }
    Referrer referrer;
    readReferrer(info, referrer);
    std::lock_guard<std::mutex> lock(mutex);
    // a moved referrer keeps its GUID: its entry is replaced
    replaceReferrer(info.guid, std::move(referrer));
    version++;
}

// ==================================================================== queries

std::unordered_map<Guid, std::vector<DepEdge>> AssetIndex::overlayEdges() const {
    std::unordered_map<Guid, std::vector<DepEdge>> memory;
    OverlayProvider provider;
    {
        std::lock_guard<std::mutex> lock(mutex);
        provider = overlay;
    }
    if (provider) {
        provider(memory);
    }
    return memory;
}

void AssetIndex::collectReferences(const Guid& target, const std::string& targetPath,
                                   const std::unordered_map<Guid, std::vector<DepEdge>>& memory,
                                   std::vector<DepEdge>& out, size_t limit) const {
    // the target and its sub-assets
    std::unordered_set<Guid> targets;
    if (!target.isNull()) {
        targets.insert(target);
        AssetDatabase::AssetInfo info;
        if (database && database->getInfo(target, info)) {
            for (const AssetDatabase::SubAsset& sub : info.subAssets) targets.insert(sub.guid);
            for (const Guid& former : info.formerGuids) targets.insert(former);
        }
    }
    const std::string directoryPrefix = targetPath.empty() ? std::string() : targetPath + "/";
    auto matches = [&](const DepEdge& edge) {
        if (!edge.target.isNull() && targets.count(edge.target)) return true;
        if (!targetPath.empty() && edge.targetPath == targetPath && (edge.target.isNull() || isTextEdge(edge.kind))) return true;
        if (!directoryPrefix.empty() && isTextEdge(edge.kind) && edge.targetPath.compare(0, directoryPrefix.size(), directoryPrefix) == 0) return true;
        return false;
    };
    std::unordered_set<Guid> candidates;
    for (const Guid& guid : targets) {
        auto it = byTarget.find(guid);
        if (it != byTarget.end()) candidates.insert(it->second.begin(), it->second.end());
    }
    if (!targetPath.empty()) {
        auto it = byTargetPath.find(targetPath);
        if (it != byTargetPath.end()) candidates.insert(it->second.begin(), it->second.end());
        // a directory: the text edges into it
        for (const auto& [path, list] : byTargetPath) {
            if (path.compare(0, directoryPrefix.size(), directoryPrefix) == 0) candidates.insert(list.begin(), list.end());
        }
    }
    for (const auto& [guid, edges] : memory) {
        candidates.insert(guid);
    }
    std::vector<Guid> ordered(candidates.begin(), candidates.end());
    const std::string noPath;
    std::sort(ordered.begin(), ordered.end(), [&](const Guid& a, const Guid& b) {
        auto pa = referrers.find(a), pb = referrers.find(b);
        const std::string& na = pa != referrers.end() ? pa->second.path : noPath;
        const std::string& nb = pb != referrers.end() ? pb->second.path : noPath;
        return na != nb ? na < nb : a < b;
    });
    for (const Guid& guid : ordered) {
        auto memoryEdges = memory.find(guid);
        const std::vector<DepEdge>* edges = nullptr;
        if (memoryEdges != memory.end()) {
            edges = &memoryEdges->second;
        } else {
            auto it = referrers.find(guid);
            if (it == referrers.end()) continue;
            edges = &it->second.edges;
        }
        for (const DepEdge& edge : *edges) {
            if (out.size() >= limit) return;
            if (matches(edge)) out.push_back(edge);
        }
    }
}

std::vector<DepEdge> AssetIndex::referencesTo(const Guid& target, const std::string& targetPath, bool transitive, size_t limit) const {
    const std::unordered_map<Guid, std::vector<DepEdge>> memory = overlayEdges();
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<DepEdge> out;
    collectReferences(target, targetPath, memory, out, limit);
    if (!transitive) {
        return out;
    }
    // breadth first over the referrers: who references the referrers
    std::unordered_set<Guid> visited{target};
    std::deque<Guid> queue;
    for (const DepEdge& edge : out) {
        if (visited.insert(edge.referrer).second) queue.push_back(edge.referrer);
    }
    while (!queue.empty() && out.size() < limit) {
        const Guid next = queue.front();
        queue.pop_front();
        std::vector<DepEdge> hop;
        collectReferences(next, std::string(), memory, hop, limit - out.size());
        for (const DepEdge& edge : hop) {
            out.push_back(edge);
            if (visited.insert(edge.referrer).second) queue.push_back(edge.referrer);
        }
    }
    return out;
}

std::vector<DepEdge> AssetIndex::dependenciesOf(const Guid& referrer, int depth) const {
    const std::unordered_map<Guid, std::vector<DepEdge>> memory = overlayEdges();
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<DepEdge> out;
    std::unordered_set<Guid> visited{referrer};
    std::vector<Guid> level{referrer};
    for (int d = 0; d < std::max(depth, 1) && !level.empty(); d++) {
        std::vector<Guid> next;
        for (const Guid& guid : level) {
            const std::vector<DepEdge>* edges = nullptr;
            auto memoryEdges = memory.find(guid);
            if (memoryEdges != memory.end()) {
                edges = &memoryEdges->second;
            } else {
                auto it = referrers.find(guid);
                if (it == referrers.end()) continue;
                edges = &it->second.edges;
            }
            for (const DepEdge& edge : *edges) {
                out.push_back(edge);
                if (!edge.target.isNull() && visited.insert(edge.target).second) next.push_back(edge.target);
            }
        }
        level = std::move(next);
    }
    return out;
}

void AssetIndex::forEachEdge(const std::function<void(const DepEdge&)>& visitor) const {
    const std::unordered_map<Guid, std::vector<DepEdge>> memory = overlayEdges();
    std::vector<DepEdge> edges;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [guid, referrer] : referrers) {
            if (memory.count(guid)) continue;
            edges.insert(edges.end(), referrer.edges.begin(), referrer.edges.end());
        }
    }
    for (const auto& [guid, list] : memory) {
        edges.insert(edges.end(), list.begin(), list.end());
    }
    for (const DepEdge& edge : edges) {
        visitor(edge);
    }
}

std::vector<DepEdge> AssetIndex::fileEdgesOf(const Guid& referrer) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = referrers.find(referrer);
    return it != referrers.end() ? it->second.edges : std::vector<DepEdge>();
}

std::string AssetIndex::referrerPath(const Guid& referrer) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = referrers.find(referrer);
    if (it != referrers.end()) {
        return it->second.path;
    }
    if (referrer == projectReferrer()) {
        return "project.yaml";
    }
    return database ? database->getPath(referrer).generic_string() : std::string();
}
