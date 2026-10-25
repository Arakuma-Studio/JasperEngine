// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipValidation.h"

#include "Project.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "component/LatticeComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"
#include "io/FileData.h"
#include "registry/EntityRegistry.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // Levenshtein distance (bytes)
    size_t editDistance(const std::string& a, const std::string& b){
        std::vector<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++) row[j] = j;
        for (size_t i = 1; i <= a.size(); i++){
            size_t diagonal = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); j++){
                const size_t above = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
                diagonal = above;
            }
        }
        return row[b.size()];
    }

    std::string lastSegment(const std::string& path){
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    std::string quotedPath(const std::string& path){
        return path.empty() ? std::string("the root") : "'" + path + "'";
    }

    std::string entityLabel(EntityRegistry* registry, Entity entity){
        return "'" + registry->getEntityName(entity) + "'";
    }

    // A model loaded without its node hierarchy (MeshSystem::needsNodeHierarchyReload without
    // the player condition: validation may run before a player is added)
    bool isLegacyLayoutModel(EntityRegistry* registry, Entity entity){
        const ModelComponent* model = registry->findComponent<ModelComponent>(entity);
        if (!model || model->filename.empty() || model->loadedFilename.empty() || !model->nodesIdMapping.empty()) return false;
        std::string extension = FileData::getFilePathExtension(model->filename);
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if (extension == "obj") return false;
        // merged static meshes animate only at the root (a single mesh node is not merged)
        if (model->mergeStaticMeshes){
            size_t meshNodes = 0;
            if (model->gltfModel){
                for (const tinygltf::Node& node : model->gltfModel->nodes){
                    if (node.mesh >= 0) meshNodes++;
                }
            }
            if (!model->gltfModel || meshNodes > 1) return false;
        }
        return true;
    }

    std::string formatSeconds(float seconds){
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%g", seconds);
        return buffer;
    }

    // `entity` itself or a Transform ancestor up to (and including) `root` that is a legacy
    // layout model; NULL_ENTITY when none
    Entity legacyModelAt(EntityRegistry* registry, Entity root, Entity entity){
        Entity current = entity;
        for (int depth = 0; current != NULL_ENTITY && depth < 65536; depth++){
            if (!registry->isEntityCreated(current)) break;
            if (isLegacyLayoutModel(registry, current)) return current;
            if (current == root) break;
            const Transform* transform = registry->findComponent<Transform>(current);
            if (!transform) break;
            current = transform->parent;
        }
        return NULL_ENTITY;
    }

    // The morph target count of the glTF mesh `entity` shows, -1 when it can't be told (not a
    // node of a loaded glTF model)
    int morphTargetCount(EntityRegistry* registry, Entity entity){
        Entity current = entity;
        for (int depth = 0; current != NULL_ENTITY && depth < 65536; depth++){
            if (!registry->isEntityCreated(current)) break;
            const ModelComponent* model = registry->findComponent<ModelComponent>(current);
            if (model && model->gltfModel){
                int node = -1;
                for (const auto& entry : model->nodesIdMapping){
                    if (entry.second == entity) node = entry.first;
                }
                for (const auto& entry : model->meshNodesMapping){
                    if (entry.second == entity) node = entry.first;
                }
                if (node >= 0){
                    const tinygltf::Model& gltf = *model->gltfModel;
                    if (node >= (int)gltf.nodes.size()) return -1;
                    const int mesh = gltf.nodes[node].mesh;
                    if (mesh < 0 || mesh >= (int)gltf.meshes.size()) return 0;
                    size_t targets = 0;
                    for (const tinygltf::Primitive& primitive : gltf.meshes[mesh].primitives){
                        targets = std::max(targets, primitive.targets.size());
                    }
                    return (int)std::min(targets, (size_t)MAX_MORPHTARGETS);
                }
                if (current != entity) return -1;
            }
            const Transform* transform = registry->findComponent<Transform>(current);
            if (!transform) break;
            current = transform->parent;
        }
        return -1;
    }

    // Entities under `root` whose name can't be addressed (they and their branch can't be bound)
    void collectUnaddressable(EntityRegistry* registry, Entity root, std::vector<ClipWarning>& warnings){
        std::vector<std::pair<std::string, Entity>> stack;
        for (Entity child : anim::getChildren(registry, root)) stack.push_back({std::string(), child});
        size_t visited = 0;
        size_t reported = 0;
        while (!stack.empty() && visited < 100000){
            const std::pair<std::string, Entity> current = stack.back();
            stack.pop_back();
            visited++;
            const std::string name = registry->getEntityName(current.second);
            const std::string label = current.first.empty() ? name : current.first + "/" + name;
            if (!anim::isAddressableName(name)){
                if (reported++ < 8){
                    ClipWarning warning;
                    warning.message = "'" + label + "' can't be animated by a clip: names with '/' or ending in #<digits> "
                                      "can't be addressed (rename it)";
                    warnings.push_back(warning);
                }
                continue;
            }
            for (Entity child : anim::getChildren(registry, current.second)) stack.push_back({label, child});
        }
        if (reported > 8){
            ClipWarning warning;
            warning.message = std::to_string(reported - 8) + " more GameObjects with names that can't be addressed";
            warnings.push_back(warning);
        }
    }

    // Lua method names a script defines: `function T:name(`, `function T.name(`, `T.name = function`
    void parseLuaFunctions(const std::string& text, std::set<std::string>& out){
        auto isIdentifier = [](char c){ return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        const std::string keyword = "function";
        size_t at = 0;
        while ((at = text.find(keyword, at)) != std::string::npos){
            const size_t start = at;
            at += keyword.size();
            if ((start > 0 && isIdentifier(text[start - 1])) || (at < text.size() && isIdentifier(text[at]))) continue;
            // `function a.b:c(` : the last identifier before '('
            size_t i = at;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) i++;
            size_t nameStart = i;
            std::string last;
            bool qualified = false;
            while (i < text.size()){
                nameStart = i;
                while (i < text.size() && isIdentifier(text[i])) i++;
                if (i == nameStart) break;
                last = text.substr(nameStart, i - nameStart);
                if (i < text.size() && (text[i] == '.' || text[i] == ':')){
                    qualified = true;
                    i++;
                    continue;
                }
                break;
            }
            size_t paren = i;
            while (paren < text.size() && (text[paren] == ' ' || text[paren] == '\t')) paren++;
            if (!last.empty() && paren < text.size() && text[paren] == '('){
                if (qualified) out.insert(last);
                continue;
            }
            // `T.name = function(`: the identifier before " = "
            size_t back = start;
            while (back > 0 && (text[back - 1] == ' ' || text[back - 1] == '\t')) back--;
            if (back > 0 && text[back - 1] == '='){
                back--;
                while (back > 0 && (text[back - 1] == ' ' || text[back - 1] == '\t')) back--;
                const size_t end = back;
                while (back > 0 && isIdentifier(text[back - 1])) back--;
                if (back < end && back > 0 && (text[back - 1] == '.' || text[back - 1] == ':')){
                    out.insert(text.substr(back, end - back));
                }
            }
        }
    }

}

bool ClipValidationReport::ok() const{
    for (const TrackReport& track : tracks){
        if (track.status != TrackBindingStatus::Ok && track.status != TrackBindingStatus::DuplicateSiblingName &&
                track.status != TrackBindingStatus::Unchecked && track.status != TrackBindingStatus::NotSampled){
            return false;
        }
    }
    for (const EventReport& event : events){
        if (event.status == EventHandlerStatus::NotFound) return false;
    }
    return true;
}

ClipValidationReport ClipValidation::validate(const ClipData& data, EntityRegistry* registry, Entity root,
                                              const ClipValidationOptions& options){
    ClipValidationReport report;
    report.root = root;
    const bool bound = registry && root != NULL_ENTITY && registry->isEntityCreated(root);
    if (!bound) report.root = NULL_ENTITY;

    std::vector<std::pair<std::string, Entity>> existing;   // descendantPaths, on the first missing path
    bool existingRead = false;
    Entity legacyModel = NULL_ENTITY;

    for (size_t i = 0; i < data.tracks.size(); i++){
        const ClipTrackData& clipTrack = data.tracks[i];
        TrackReport track;
        track.track = i;
        track.path = clipTrack.path;
        track.channel = clipTrack.channel;
        const std::string label = trackLabel(clipTrack);

        if (clipTrack.channel == ClipChannel::Property){
            track.status = TrackBindingStatus::NotSampled;
            track.message = label + ": property tracks are not supported yet (kept, not played)";
            report.tracks.push_back(track);
            continue;
        }
        report.total++;
        if (!bound){
            track.status = TrackBindingStatus::Unchecked;
            report.tracks.push_back(track);
            continue;
        }

        std::vector<anim::PathSegment> segments;
        if (!anim::parsePath(clipTrack.path, segments)){
            track.status = TrackBindingStatus::UnaddressableName;
            track.message = label + ": not a valid path (names joined by '/', \"Name#N\" for the N-th later sibling "
                                    "of that name)";
            report.tracks.push_back(track);
            continue;
        }

        anim::PathResolution resolution;
        const Entity entity = anim::resolvePath(registry, root, segments, &resolution);
        if (entity == NULL_ENTITY){
            // under a model in the legacy layout: one cause for all of them (R3)
            const Entity legacy = legacyModelAt(registry, root, resolution.lastMatched);
            if (legacy != NULL_ENTITY){
                track.status = TrackBindingStatus::LegacyModelLayout;
                track.message = label + ": " + entityLabel(registry, legacy) + " is loaded in the legacy layout (no node "
                                "hierarchy): an Animation Player reloads it with its glTF nodes";
                if (!report.legacyModelLayout) legacyModel = legacy;
                report.legacyModelLayout = true;
                report.tracks.push_back(track);
                continue;
            }
            if (!existingRead){
                existing = anim::descendantPaths(registry, root);
                existingRead = true;
            }
            track.status = TrackBindingStatus::MissingPath;
            track.nearest = nearestPaths(registry, root, clipTrack.path, options.maxNearest);
            std::string missing = segments.size() > resolution.resolvedSegments ? segments[resolution.resolvedSegments].name : "";
            track.message = label + ": no GameObject at " + quotedPath(clipTrack.path);
            if (!missing.empty() && resolution.resolvedSegments > 0){
                track.message += " ('" + missing + "' is not a child of '" + registry->getEntityName(resolution.lastMatched) + "')";
            }
            if (!track.nearest.empty()){
                track.message += ": did you mean " + quotedPath(track.nearest[0]) + "?";
            }
            report.tracks.push_back(track);
            continue;
        }
        track.entity = entity;

        // the component the channel writes
        bool failed = false;
        switch (clipTrack.channel){
            case ClipChannel::Position:
            case ClipChannel::Rotation:
            case ClipChannel::Scale:
                if (!registry->findComponent<Transform>(entity)){
                    track.status = TrackBindingStatus::MissingComponent;
                    track.message = label + ": " + entityLabel(registry, entity) + " has no Transform";
                    failed = true;
                }
                break;
            case ClipChannel::Morph: {
                const MeshComponent* mesh = registry->findComponent<MeshComponent>(entity);
                bool morphs = false;
                for (unsigned int s = 0; mesh && s < mesh->numSubmeshes && s < mesh->submeshes.size(); s++){
                    morphs = morphs || mesh->submeshes[s].hasMorphTarget;
                }
                if (!morphs){
                    track.status = TrackBindingStatus::MissingComponent;
                    track.message = label + ": " + entityLabel(registry, entity) + " has no mesh with blend shapes (morph targets)";
                    failed = true;
                    break;
                }
                size_t width = 0;
                for (const ClipKey& key : clipTrack.keys) width = std::max(width, key.value.size());
                const int targets = morphTargetCount(registry, entity);
                if (targets >= 0 && width > (size_t)targets){
                    track.status = TrackBindingStatus::MorphIndexOutOfRange;
                    track.message = label + ": " + std::to_string(width) + " weights, but " + entityLabel(registry, entity) +
                                    " has " + std::to_string(targets) + " blend shapes (the extra ones do nothing)";
                    failed = true;
                }
                break;
            }
            case ClipChannel::Lattice: {
                const LatticeComponent* lattice = registry->findComponent<LatticeComponent>(entity);
                if (!lattice){
                    track.status = TrackBindingStatus::MissingComponent;
                    track.message = label + ": " + entityLabel(registry, entity) + " has no Lattice";
                    failed = true;
                    break;
                }
                const long count = (long)lattice->resolutionX * lattice->resolutionY * lattice->resolutionZ;
                for (int point : clipTrack.points){
                    if (point < 0 || point >= count) track.badPoints.push_back(point);
                }
                if (!track.badPoints.empty()){
                    track.status = TrackBindingStatus::LatticeIndexOutOfRange;
                    std::string list;
                    for (size_t p = 0; p < track.badPoints.size() && p < 8; p++){
                        list += (p ? ", " : "") + std::to_string(track.badPoints[p]);
                    }
                    if (track.badPoints.size() > 8) list += ", ...";
                    track.message = label + ": points " + list + " are outside the " + std::to_string(lattice->resolutionX) + "x" +
                                    std::to_string(lattice->resolutionY) + "x" + std::to_string(lattice->resolutionZ) +
                                    " cage of " + entityLabel(registry, entity) + " (indices 0.." + std::to_string(count - 1) + ")";
                    failed = true;
                }
                break;
            }
            case ClipChannel::Property:
                break;
        }
        if (!failed){
            if (resolution.duplicateSibling){
                track.status = TrackBindingStatus::DuplicateSiblingName;
                track.message = label + ": several siblings share a name on the way; the first one is animated "
                                "(rename them, or use \"Name#N\")";
            }else{
                track.status = TrackBindingStatus::Ok;
            }
            report.resolved++;
        }
        report.tracks.push_back(track);
    }

    // every legacy-layout model on or under the root, its own outcome with a fix (R3): a player
    // added by a path that does not switch the layout (duplicate, create with a parent) leaves it so
    if (bound){
        std::vector<Entity> stack = {root};
        size_t visited = 0;
        while (!stack.empty() && visited++ < 100000){
            const Entity current = stack.back();
            stack.pop_back();
            if (isLegacyLayoutModel(registry, current)){
                report.legacyModels.push_back(current);
                continue;   // its parts are what the reload rebuilds
            }
            const std::vector<Entity> children = anim::getChildren(registry, current);
            stack.insert(stack.end(), children.rbegin(), children.rend());
        }
        if (legacyModel != NULL_ENTITY && std::find(report.legacyModels.begin(), report.legacyModels.end(), legacyModel) == report.legacyModels.end()){
            report.legacyModels.insert(report.legacyModels.begin(), legacyModel);
        }
    }
    for (Entity model : report.legacyModels){
        report.legacyModelLayout = true;
        ClipWarning warning;
        warning.message = entityLabel(registry, model) + " is loaded in the legacy layout: its glTF nodes are not "
                          "GameObjects, so clip paths under it don't bind. Reload it in the node hierarchy (the "
                          "Animation Player's fix action; adding a player does it).";
        report.warnings.push_back(warning);
    }
    if (bound){
        collectUnaddressable(registry, root, report.warnings);
    }

    const bool checkHandlers = bound && options.checkHandlers && !options.hasCppScript;
    const std::set<std::string> functions(options.scriptFunctions.begin(), options.scriptFunctions.end());
    for (size_t i = 0; i < data.events.size(); i++){
        EventReport event;
        event.event = i;
        event.function = data.events[i].function;
        if (checkHandlers){
            if (functions.count(event.function)){
                event.status = EventHandlerStatus::Found;
            }else{
                event.status = EventHandlerStatus::NotFound;
                event.message = "event " + event.function + " at " + formatSeconds(data.events[i].time) +
                                " s: no Lua script on " + entityLabel(registry, root) + " defines " + event.function +
                                "() (add the method, or subscribe to onEvent)";
            }
        }
        report.events.push_back(event);
    }
    return report;
}

ClipValidationReport ClipValidation::validateInScene(Project* project, SceneProject* scene, const ClipData& data, Entity root){
    if (!scene || !scene->scene){
        return validate(data, nullptr, root);
    }
    ClipValidationOptions options;
    options.scriptFunctions = scriptFunctions(project, scene, root);
    if (root != NULL_ENTITY && scene->scene->isEntityCreated(root)){
        if (const ScriptComponent* scripts = scene->scene->findComponent<ScriptComponent>(root)){
            for (const ScriptEntry& entry : scripts->scripts){
                // C++ scripts subscribe through REGISTER_COMPONENT_EVENT: not checked (R23)
                if (entry.type == ScriptType::CPP) options.hasCppScript = true;
            }
        }
    }
    return validate(data, scene->scene, root, options);
}

std::vector<std::string> ClipValidation::scriptFunctions(Project* project, SceneProject* scene, Entity entity){
    std::set<std::string> found;
    if (!scene || !scene->scene || entity == NULL_ENTITY || !scene->scene->isEntityCreated(entity)) return {};
    const ScriptComponent* scripts = scene->scene->findComponent<ScriptComponent>(entity);
    if (!scripts) return {};
    for (const ScriptEntry& entry : scripts->scripts){
        if (entry.type != ScriptType::LUA || entry.path.empty()) continue;
        std::filesystem::path file = project ? project->resolveLuaPath(std::filesystem::u8path(entry.path))
                                             : std::filesystem::u8path(entry.path);
        std::ifstream in(file, std::ios::binary);
        if (!in) continue;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        parseLuaFunctions(text, found);
    }
    return std::vector<std::string>(found.begin(), found.end());
}

std::vector<std::string> ClipValidation::nearestPaths(EntityRegistry* registry, Entity root, const std::string& path, size_t max){
    std::vector<std::string> out;
    if (!registry || root == NULL_ENTITY || max == 0) return out;
    struct Candidate{
        size_t distance;
        size_t nameDistance;
        size_t order;
        std::string path;
    };
    std::vector<Candidate> candidates;
    const std::string name = lastSegment(path);
    const std::vector<std::pair<std::string, Entity>> paths = anim::descendantPaths(registry, root);
    for (size_t i = 0; i < paths.size(); i++){
        const std::string& candidate = paths[i].first;
        if (candidate.empty() || candidate == path) continue;
        const size_t distance = editDistance(path, candidate);
        const size_t nameDistance = editDistance(name, lastSegment(candidate));
        // nothing in common: no suggestion
        if (distance >= std::max(path.size(), candidate.size()) && nameDistance >= std::max(name.size(), lastSegment(candidate).size())){
            continue;
        }
        // the same name somewhere else (another parent) is the best guess
        candidates.push_back({std::min(distance, nameDistance == 0 ? (size_t)0 : distance), nameDistance, i, candidate});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b){
        if (a.distance != b.distance) return a.distance < b.distance;
        if (a.nameDistance != b.nameDistance) return a.nameDistance < b.nameDistance;
        return a.order < b.order;
    });
    for (size_t i = 0; i < candidates.size() && out.size() < max; i++){
        out.push_back(candidates[i].path);
    }
    return out;
}

std::string ClipValidation::trackLabel(const ClipTrackData& track){
    return track.path + " : " + anim::channelName(track.channel);
}
