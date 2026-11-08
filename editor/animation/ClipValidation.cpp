// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipValidation.h"
#include "component/SpriteComponent.h"
#include "util/FileUtils.h"

#include "Project.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/ClipEditorUtil.h"
#include "animation/PropertyBindings.h"
#include "component/ButtonComponent.h"
#include "animation/HumanoidClip.h"
#include "component/LatticeComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"
#include "io/FileData.h"
#include "registry/EntityRegistry.h"
#include "subsystem/MeshSystem.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <filesystem>
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

    using clipui::lastSegment;

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
        return model && MeshSystem::isLegacyLayout(*model);
    }

    // a time in messages, as short as it is exact ("0.5"): the editor's one formatter of it
    using clipui::formatSecondsPlain;

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

    // `text` without its Lua comments (`-- ...` lines and `--[[ ]]` / `--[==[ ]==]` blocks);
    // strings are kept as they are, so a "--" inside one is not a comment
    std::string stripLuaComments(const std::string& text){
        std::string out;
        out.reserve(text.size());
        // the "]==]" that closes a long bracket opened at `at` ("[==["), empty when none is
        auto longBracketClose = [&](size_t at) -> std::string {
            if (at >= text.size() || text[at] != '[') return std::string();
            size_t i = at + 1;
            while (i < text.size() && text[i] == '=') i++;
            if (i >= text.size() || text[i] != '[') return std::string();
            return "]" + std::string(i - at - 1, '=') + "]";
        };
        size_t i = 0;
        while (i < text.size()){
            const char c = text[i];
            if (c == '"' || c == '\''){
                size_t j = i + 1;
                while (j < text.size() && text[j] != c && text[j] != '\n'){
                    if (text[j] == '\\') j++;
                    j++;
                }
                j = std::min(j + 1, text.size());
                out.append(text, i, j - i);
                i = j;
                continue;
            }
            if (c == '['){
                const std::string close = longBracketClose(i);
                if (!close.empty()){
                    const size_t end = text.find(close, i);
                    const size_t j = end == std::string::npos ? text.size() : end + close.size();
                    out.append(text, i, j - i);
                    i = j;
                    continue;
                }
            }
            if (c == '-' && i + 1 < text.size() && text[i + 1] == '-'){
                const std::string close = longBracketClose(i + 2);
                if (!close.empty()){
                    const size_t end = text.find(close, i + 2);
                    i = end == std::string::npos ? text.size() : end + close.size();
                    out += ' ';
                }else{
                    const size_t end = text.find('\n', i);
                    i = end == std::string::npos ? text.size() : end;
                }
                continue;
            }
            out += c;
            i++;
        }
        return out;
    }

    // Lua method names a script defines on `owner` (its class or module table; empty: any
    // table): `function owner:name(`, `function owner.name(`, `owner.name = function`
    void parseLuaFunctions(const std::string& source, const std::string& owner, std::set<std::string>& out){
        const std::string text = stripLuaComments(source);
        auto isIdentifier = [](char c){ return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        auto ownerMatches = [&](const std::string& table){ return owner.empty() || table == owner; };
        const std::string keyword = "function";
        size_t at = 0;
        while ((at = text.find(keyword, at)) != std::string::npos){
            const size_t start = at;
            at += keyword.size();
            if ((start > 0 && isIdentifier(text[start - 1])) || (at < text.size() && isIdentifier(text[at]))) continue;
            // `function a.b:c(`: the last identifier before '(' and the table it is on
            size_t i = at;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) i++;
            std::string last;
            std::string table;
            while (i < text.size()){
                const size_t nameStart = i;
                while (i < text.size() && isIdentifier(text[i])) i++;
                if (i == nameStart) break;
                const std::string name = text.substr(nameStart, i - nameStart);
                if (i < text.size() && (text[i] == '.' || text[i] == ':')){
                    table = name;
                    i++;
                    continue;
                }
                last = name;
                break;
            }
            size_t paren = i;
            while (paren < text.size() && (text[paren] == ' ' || text[paren] == '\t')) paren++;
            if (!last.empty() && paren < text.size() && text[paren] == '('){
                if (!table.empty() && ownerMatches(table)) out.insert(last);
                continue;
            }
            // `T.name = function(`: the identifier before " = " and its table
            size_t back = start;
            while (back > 0 && (text[back - 1] == ' ' || text[back - 1] == '\t')) back--;
            if (back > 0 && text[back - 1] == '=' && (back < 2 || (text[back - 2] != '=' && text[back - 2] != '~' &&
                                                                    text[back - 2] != '<' && text[back - 2] != '>'))){
                back--;
                while (back > 0 && (text[back - 1] == ' ' || text[back - 1] == '\t')) back--;
                const size_t end = back;
                while (back > 0 && isIdentifier(text[back - 1])) back--;
                if (back < end && back > 0 && (text[back - 1] == '.' || text[back - 1] == ':')){
                    const size_t tableEnd = back - 1;
                    size_t tableStart = tableEnd;
                    while (tableStart > 0 && isIdentifier(text[tableStart - 1])) tableStart--;
                    if (ownerMatches(text.substr(tableStart, tableEnd - tableStart))){
                        out.insert(text.substr(back, end - back));
                    }
                }
            }
        }
    }

    bool isLuaIdentifierChar(char c){
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    size_t skipLuaSpace(const std::string& text, size_t i){
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
        return i;
    }

    // the identifier starting at `i` (empty when none)
    std::string luaIdentifierAt(const std::string& text, size_t i){
        size_t end = i;
        while (end < text.size() && isLuaIdentifierChar(text[end])) end++;
        if (end == i || std::isdigit(static_cast<unsigned char>(text[i]))) return std::string();
        return text.substr(i, end - i);
    }

    // `name` in `name = <at>` / `local name = <at>` (single '='), empty when `at` is not assigned
    std::string luaAssignedTo(const std::string& text, size_t at){
        size_t back = at;
        while (back > 0 && std::isspace(static_cast<unsigned char>(text[back - 1]))) back--;
        if (back == 0 || text[back - 1] != '=') return std::string();
        back--;
        if (back > 0 && (text[back - 1] == '=' || text[back - 1] == '~' || text[back - 1] == '<' || text[back - 1] == '>')){
            return std::string();
        }
        while (back > 0 && std::isspace(static_cast<unsigned char>(text[back - 1]))) back--;
        const size_t end = back;
        while (back > 0 && isLuaIdentifierChar(text[back - 1])) back--;
        if (back > 0 && (text[back - 1] == '.' || text[back - 1] == ':')) return std::string();
        return text.substr(back, end - back);
    }

    // the index just past the bracket matching the one at `open` ('{' or '(')
    size_t luaMatchingClose(const std::string& text, size_t open){
        const char opening = text[open];
        const char closing = opening == '{' ? '}' : ')';
        int depth = 0;
        for (size_t i = open; i < text.size(); i++){
            if (text[i] == '"' || text[i] == '\''){
                const char quote = text[i];
                for (i++; i < text.size() && text[i] != quote; i++){
                    if (text[i] == '\\') i++;
                }
                continue;
            }
            if (text[i] == opening) depth++;
            if (text[i] == closing && --depth == 0) return i + 1;
        }
        return text.size();
    }

    // The tables `owner` (empty: any table) inherits methods from through a metatable:
    // `setmetatable(owner, {__index = Base})`, `setmetatable(owner, Base)` and
    // `owner = setmetatable({...}, {__index = Base} | Base)`. `text` has no comments.
    std::vector<std::string> luaBaseTables(const std::string& text, const std::string& owner){
        std::vector<std::string> bases;
        const std::string keyword = "setmetatable";
        size_t at = 0;
        while ((at = text.find(keyword, at)) != std::string::npos){
            const size_t start = at;
            at += keyword.size();
            if ((start > 0 && isLuaIdentifierChar(text[start - 1])) || (at < text.size() && isLuaIdentifierChar(text[at]))) continue;
            size_t i = skipLuaSpace(text, at);
            if (i >= text.size() || text[i] != '(') continue;
            i = skipLuaSpace(text, i + 1);
            // the first argument: the table getting the metatable
            std::string table;
            if (i < text.size() && text[i] == '{'){
                i = luaMatchingClose(text, i);
                table = luaAssignedTo(text, start);
            }else{
                table = luaIdentifierAt(text, i);
                i += table.size();
            }
            if (table.empty() || (!owner.empty() && table != owner)) continue;
            i = skipLuaSpace(text, i);
            if (i >= text.size() || text[i] != ',') continue;
            i = skipLuaSpace(text, i + 1);
            std::string base;
            if (i < text.size() && text[i] == '{'){
                // {__index = Base, ...}
                const size_t end = luaMatchingClose(text, i);
                const std::string fields = text.substr(i, end - i);
                size_t index = fields.find("__index");
                while (index != std::string::npos && index > 0 && isLuaIdentifierChar(fields[index - 1])){
                    index = fields.find("__index", index + 1);
                }
                if (index != std::string::npos){
                    size_t j = skipLuaSpace(fields, index + 7);
                    if (j < fields.size() && fields[j] == '='){
                        base = luaIdentifierAt(fields, skipLuaSpace(fields, j + 1));
                    }
                }
            }else{
                base = luaIdentifierAt(text, i);
            }
            if (!base.empty() && base != table && std::find(bases.begin(), bases.end(), base) == bases.end()){
                bases.push_back(base);
            }
        }
        return bases;
    }

    // the module `name = require("a.b")` (or `require "a.b"`) loads, empty when `name` is not
    // assigned a require in `text`
    std::string luaRequiredModule(const std::string& text, const std::string& name){
        const std::string keyword = "require";
        size_t at = 0;
        while ((at = text.find(keyword, at)) != std::string::npos){
            const size_t start = at;
            at += keyword.size();
            if ((start > 0 && isLuaIdentifierChar(text[start - 1])) || (at < text.size() && isLuaIdentifierChar(text[at]))) continue;
            if (luaAssignedTo(text, start) != name) continue;
            size_t i = skipLuaSpace(text, at);
            if (i < text.size() && text[i] == '(') i = skipLuaSpace(text, i + 1);
            if (i >= text.size() || (text[i] != '"' && text[i] != '\'')) continue;
            const size_t end = text.find(text[i], i + 1);
            if (end == std::string::npos) continue;
            return text.substr(i + 1, end - i - 1);
        }
        return std::string();
    }

    // the table a module script returns (`return Name` as its last statement), empty when none
    std::string luaReturnedTable(const std::string& text){
        size_t at = text.rfind("return");
        while (at != std::string::npos){
            const bool wordStart = at == 0 || !isLuaIdentifierChar(text[at - 1]);
            const bool wordEnd = at + 6 >= text.size() || !isLuaIdentifierChar(text[at + 6]);
            if (wordStart && wordEnd){
                const size_t i = skipLuaSpace(text, at + 6);
                const std::string name = luaIdentifierAt(text, i);
                return skipLuaSpace(text, i + name.size()) >= text.size() ? name : std::string();
            }
            if (at == 0) break;
            at = text.rfind("return", at - 1);
        }
        return std::string();
    }

    bool readText(const std::filesystem::path& file, std::string& out){
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

    // The methods a script defines on `owner` and on the tables it inherits from through a
    // metatable: a base table defined in the same script, or one a `require` of the project's
    // Lua folder returns (as the engine's module loader finds it: "a.b" is a/b.lua or lua/a/b.lua)
    void collectLuaMethods(Project* project, const std::filesystem::path& file, const std::string& source, const std::string& owner,
                           std::set<std::string>& out, std::set<std::string>& visited, int depth){
        if (depth > 8 || !visited.insert(file.generic_string() + "\n" + owner).second) return;
        parseLuaFunctions(source, owner, out);
        const std::string text = stripLuaComments(source);
        for (const std::string& base : luaBaseTables(text, owner)){
            const std::string module = luaRequiredModule(text, base);
            if (module.empty()){
                collectLuaMethods(project, file, source, base, out, visited, depth + 1);
                continue;
            }
            if (!project) continue;
            std::string relative = module;
            std::replace(relative.begin(), relative.end(), '.', '/');
            relative += ".lua";
            for (const std::string& candidate : {relative, "lua/" + relative}){
                const std::filesystem::path path = project->resolveLuaPath(editor::FileUtils::pathFromUtf8(candidate));
                std::string moduleSource;
                if (!readText(path, moduleSource)) continue;
                collectLuaMethods(project, path, moduleSource, luaReturnedTable(stripLuaComments(moduleSource)), out, visited, depth + 1);
                break;
            }
        }
    }

    // the paths of `paths` nearest to `path` (descendantPaths of the root, read once per report)
    std::vector<std::string> nearestIn(const std::vector<std::pair<std::string, Entity>>& paths, const std::string& path, size_t max){
        std::vector<std::string> out;
        if (max == 0) return out;
        struct Candidate{
            size_t distance;
            size_t nameDistance;
            size_t order;
            std::string path;
        };
        std::vector<Candidate> candidates;
        const std::string name = lastSegment(path);
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

    std::vector<std::pair<std::string, Entity>> existing;   // descendantPaths, read on the first missing path
    bool existingRead = false;
    Entity legacyModel = NULL_ENTITY;

    for (size_t i = 0; i < data.tracks.size(); i++){
        const ClipTrackData& clipTrack = data.tracks[i];
        TrackReport track;
        track.track = i;
        track.path = clipTrack.path;
        track.channel = clipTrack.channel;
        if (clipTrack.channel == ClipChannel::Property) track.property = clipTrack.component + "." + clipTrack.property;
        const std::string label = trackLabel(clipTrack);

        // a property this engine cannot animate (anim-properties spec 6): kept verbatim, not played
        anim::PropertyRef propertyRef;
        if (clipTrack.channel == ClipChannel::Property && !anim::parsePropertyRef(clipTrack.component, clipTrack.property, propertyRef)){
            track.status = TrackBindingStatus::UnknownProperty;
            track.reason = propertyRef.error;
            track.message = label + ": " + propertyRef.error + " (kept, not played)";
            report.tracks.push_back(track);
            continue;
        }
        report.total++;
        track.humanBone = clipTrack.humanBone;
        if (!bound){
            track.status = TrackBindingStatus::Unchecked;
            report.tracks.push_back(track);
            continue;
        }

        // humanoid tracks (anim-retarget): realised on the root's humanoid rig, not bound by path
        if (isHumanoidChannel(clipTrack.channel)){
            const anim::HumanoidSlots* humanoid = options.humanoid.get();
            if (!humanoid || !humanoid->valid || !humanoid->target){
                track.status = TrackBindingStatus::HumanoidNoRig;
                track.message = label + ": " + entityLabel(registry, root) + " has no humanoid rig" +
                                (humanoid && !humanoid->error.empty() ? " (" + humanoid->error + ")" : std::string()) +
                                ": map its model's bones (Configure Avatar...); the clip's other tracks still play";
                report.tracks.push_back(track);
                continue;
            }
            const anim::HumanoidTarget& target = *humanoid->target;
            if (clipTrack.channel == ClipChannel::Humanoid && clipTrack.humanBone < pose::HUMAN_BONE_COUNT){
                track.entity = target.slotEntities[(size_t)clipTrack.humanBone];
                if (track.entity == NULL_ENTITY){
                    // folded into its next present descendant slot (anim-retarget 7.3)
                    track.message = label + ": the rig has no " + anim::humanoidTrackBoneName(clipTrack.channel, clipTrack.humanBone) +
                                    ": its rotation is folded into the next slot below it";
                }
            }else if (clipTrack.channel == ClipChannel::Humanoid){
                track.entity = target.extraEntities[(size_t)(clipTrack.humanBone - pose::HUMAN_BONE_COUNT)];
                if (track.entity == NULL_ENTITY){
                    track.message = label + ": the rig has no " + anim::humanoidTrackBoneName(clipTrack.channel, clipTrack.humanBone) +
                                    " (not played; map it in Configure Avatar...)";
                }
            }else if (clipTrack.humanBone == HUMANOID_POSITION_HIPS){
                track.entity = target.slotEntities[0];
            }
            track.status = TrackBindingStatus::Ok;
            report.resolved++;
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
            track.nearest = nearestIn(existing, clipTrack.path, options.maxNearest);
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
                for (unsigned int s = 0; mesh && s < mesh->numSubmeshes; s++){
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
            case ClipChannel::Property: {
                // the component, submesh, shader member or script field (anim-properties spec 8)
                const anim::PropertyBinding binding = anim::bindProperty(registry, entity, propertyRef, clipTrack.width);
                if (binding.status != anim::PropertyBindStatus::Bound){
                    track.status = binding.status == anim::PropertyBindStatus::UnknownProperty ? TrackBindingStatus::UnknownProperty
                                                                                               : TrackBindingStatus::MissingComponent;
                    track.reason = binding.message;
                    track.message = label + ": " + entityLabel(registry, entity) + ": " + binding.message;
                    failed = true;
                    break;
                }
                // P5: a Button writes its graphic's colour from its state colours
                if (clipTrack.component == "ui" && propertyRef.base == "color" && registry->findComponent<ButtonComponent>(entity)){
                    ClipWarning warning;
                    warning.path = "tracks[" + std::to_string(i) + "]";
                    warning.message = label + ": " + entityLabel(registry, entity) + " is a Button: it writes ui.color from its state "
                                      "colours every frame (animate button.colorNormal instead)";
                    report.warnings.push_back(warning);
                }
                // R50: a clip an .aseprite import made, on a sprite that shows another sheet
                if (clipTrack.component == "sprite" && clipTrack.property == "frame" && data.source.kind == "aseprite" && !data.source.path.empty()){
                    if (const SpriteComponent* sprite = registry->findComponent<SpriteComponent>(entity)){
                        const std::string made = std::filesystem::path(data.source.path).filename().string();
                        const std::string shows = sprite->sheet.empty() ? std::string("its inline frames")
                                                                        : std::filesystem::path(sprite->sheet.path).filename().string();
                        if (sprite->sheet.empty() || std::filesystem::path(sprite->sheet.path).lexically_normal() !=
                                                         std::filesystem::path(data.source.path).lexically_normal()){
                            ClipWarning warning;
                            warning.path = "tracks[" + std::to_string(i) + "]";
                            warning.message = label + ": clip made for " + made + ", sprite shows " + shows;
                            report.warnings.push_back(warning);
                        }
                    }
                }
                if (clipTrack.component == "gameobject" && entity == root){
                    ClipWarning warning;
                    warning.path = "tracks[" + std::to_string(i) + "]";
                    warning.message = label + ": animates its own active state: the player stops when it turns it off";
                    report.warnings.push_back(warning);
                }
                break;
            }
            case ClipChannel::Humanoid:
            case ClipChannel::HumanoidPosition:
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

    // a swizzle track and the whole-property track of the same property: the swizzle wins for
    // its component (anim-properties spec 6, Overlapping tracks)
    for (size_t i = 0; i < data.tracks.size(); i++){
        const ClipTrackData& part = data.tracks[i];
        if (part.channel != ClipChannel::Property || !part.known || part.swizzle < 0) continue;
        const size_t dot = part.property.rfind('.');
        if (dot == std::string::npos) continue;
        const std::string base = part.property.substr(0, dot);
        for (const ClipTrackData& whole : data.tracks){
            if (whole.channel != ClipChannel::Property || whole.path != part.path || whole.component != part.component || whole.property != base) continue;
            static const char* const colorNames[] = {"red", "green", "blue", "alpha"};
            const bool colour = whole.kind == PropertyKind::Color3 || whole.kind == PropertyKind::Color4;
            ClipWarning warning;
            warning.path = "tracks[" + std::to_string(i) + "]";
            warning.message = part.property + " overrides the " + (colour ? colorNames[part.swizzle] : std::string(1, "xyzw"[part.swizzle])) +
                              " of the " + base + " track" + (part.path.empty() ? "" : " on " + quotedPath(part.path));
            report.warnings.push_back(warning);
        }
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
                event.message = "event " + event.function + " at " + formatSecondsPlain(data.events[i].time) +
                                " s: no Lua script on " + entityLabel(registry, root) + " defines " + event.function +
                                "() (add the method, or subscribe to onEvent)";
            }
        }
        report.events.push_back(event);
    }

    // An event calls the method of its name on the root's Lua scripts (spec 3.5), Unity's
    // SendMessage: one named like a method the engine calls itself (init, onDestroy) or an engine
    // event a script registers (onUpdate...) calls that method again, with the event's args.
    // Allowed, but said once per name (final review M2; lua-api 4.8.2)
    std::set<std::string> lifecycleWarned;
    for (const ClipEventData& clipEvent : data.events){
        if (!isScriptLifecycleMethod(clipEvent.function) || !lifecycleWarned.insert(clipEvent.function).second) continue;
        ClipWarning warning;
        warning.message = "event " + clipEvent.function + " at " + formatSecondsPlain(clipEvent.time) + " s: " + clipEvent.function +
                          " is a script method the engine calls itself (lifecycle or engine event): the event calls it again, "
                          "with the event's args; give the event its own function name";
        report.warnings.push_back(warning);
    }
    return report;
}

char ClipValidation::boneSide(const std::string& path){
    const size_t slash = path.rfind('/');
    const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    // the words: separators split, and camelCase ("LeftUpLeg", "LArm": an upper case letter after a
    // lower case one, or before a lower case one after another upper case one)
    std::vector<std::string> words;
    std::string word;
    for (size_t i = 0; i < leaf.size(); i++){
        const unsigned char c = (unsigned char)leaf[i];
        if (!std::isalnum(c)){
            if (!word.empty()) words.push_back(word);
            word.clear();
            continue;
        }
        if (!word.empty() && std::isupper(c)){
            const unsigned char previous = (unsigned char)leaf[i - 1];
            const bool nextLower = i + 1 < leaf.size() && std::islower((unsigned char)leaf[i + 1]);
            if (std::islower(previous) || std::isdigit(previous) || (std::isupper(previous) && nextLower)){
                words.push_back(word);
                word.clear();
            }
        }
        word += (char)c;
    }
    if (!word.empty()) words.push_back(word);
    char side = 0;
    for (std::string& w : words){
        for (char& c : w) c = (char)std::tolower((unsigned char)c);
        const char found = (w == "left" || w == "l") ? 'L' : (w == "right" || w == "r") ? 'R' : 0;
        if (!found) continue;
        if (side && side != found) return 0;
        side = found;
    }
    return side;
}

bool ClipValidation::isScriptLifecycleMethod(const std::string& function){
    // LuaBinding calls init / onDestroy by name; RegisterEngineEvent's names (lua-api 1.3)
    static const char* const names[] = {
        "init", "onDestroy", "onUpdate", "onFixedUpdate", "onPostUpdate", "onDraw", "onViewLoaded", "onCanvasChanged",
        "onViewDestroyed", "onPause", "onResume", "onShutdown", "onSceneLoaded", "onKeyDown", "onKeyUp", "onCharInput",
        "onMouseDown", "onMouseUp", "onMouseMove", "onMouseScroll", "onMouseEnter", "onMouseLeave", "onTouchStart",
        "onTouchMove", "onTouchEnd", "onTouchCancel", "onGamepadConnect", "onGamepadDisconnect", "onGamepadButtonDown",
        "onGamepadButtonUp", "onGamepadAxisMove"};
    for (const char* name : names){
        if (function == name) return true;
    }
    return false;
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
        if (data.isHumanoid()){
            options.humanoid = std::make_shared<anim::HumanoidSlots>(anim::resolveHumanoidSlots(scene->scene, root));
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
        // the engine calls methods on enabled Lua scripts only
        if (entry.type != ScriptType::LUA || entry.path.empty() || !entry.enabled) continue;
        std::filesystem::path file = project ? project->resolveLuaPath(FileUtils::pathFromUtf8(entry.path.path))
                                             : FileUtils::pathFromUtf8(entry.path.path);
        std::string text;
        if (!readText(file, text)) continue;
        // methods of the script's own table (named like its class) and of the tables it inherits
        // from through a metatable; a script whose table is named otherwise: methods of any table
        std::set<std::string> own;
        std::set<std::string> visited;
        collectLuaMethods(project, file, text, entry.className, own, visited, 0);
        if (own.empty()){
            visited.clear();
            collectLuaMethods(project, file, text, std::string(), own, visited, 0);
        }
        found.insert(own.begin(), own.end());
    }
    return std::vector<std::string>(found.begin(), found.end());
}

std::vector<std::string> ClipValidation::nearestPaths(EntityRegistry* registry, Entity root, const std::string& path, size_t max){
    if (!registry || root == NULL_ENTITY) return {};
    return nearestIn(anim::descendantPaths(registry, root), path, max);
}

std::string ClipValidation::trackLabel(const ClipTrackData& track){
    if (track.channel == ClipChannel::Property) return track.path + " : " + track.component + "." + track.property;
    if (isHumanoidChannel(track.channel)) return clipui::humanoidTrackLabel(track.channel, track.humanBone);
    return track.path + " : " + anim::channelName(track.channel);
}
