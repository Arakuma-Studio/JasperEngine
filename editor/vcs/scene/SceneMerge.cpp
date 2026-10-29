// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/scene/SceneMerge.h"

#include "vcs/scene/SceneDiff.h"

#include "reflect/Reflect.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <set>

namespace jasperengine::editor::vcs::scene {

namespace {

    bool defined(const YAML::Node& node) {
        return node && node.IsDefined();
    }

    YAML::Node undefinedNode() {
        return YAML::Node(YAML::NodeType::Undefined);
    }

    YAML::Node cloned(const YAML::Node& node) {
        return defined(node) ? YAML::Clone(node) : undefinedNode();
    }

    std::string joined(const std::string& path, std::string_view key) {
        return path.empty() ? std::string(key) : path + "." + std::string(key);
    }

    // ---------------------------------------------------------------- id remapping

    void remapScalar(YAML::Node node, const std::map<uint64_t, uint64_t>& remap) {
        uint64_t id = 0;
        if (!parseId(node, id)) return;
        auto it = remap.find(id);
        if (it != remap.end()) node = it->second;
    }

    void remapStruct(const reflect::StructDesc& desc, YAML::Node node, const std::map<uint64_t, uint64_t>& remap, int depth);

    void remapField(const reflect::FieldDesc& field, YAML::Node value, const std::map<uint64_t, uint64_t>& remap, int depth) {
        if (!defined(value)) return;
        if (field.kind == reflect::Kind::Entity) {
            remapScalar(value, remap);
        } else if (field.kind == reflect::Kind::Struct && field.structDesc) {
            remapStruct(*field.structDesc, value, remap, depth + 1);
        } else if (field.kind == reflect::Kind::Array && value.IsSequence()) {
            for (YAML::Node item : value) {
                if (field.elementOps && field.elementOps->kind == reflect::Kind::Entity) remapScalar(item, remap);
                else if (field.structDesc) remapStruct(*field.structDesc, item, remap, depth + 1);
            }
        }
    }

    void remapStruct(const reflect::StructDesc& desc, YAML::Node node, const std::map<uint64_t, uint64_t>& remap, int depth) {
        if (depth > 8 || !defined(node) || !node.IsMap()) return;
        for (const reflect::FieldDesc& field : desc.fields) {
            const char* key = field.yamlKey ? field.yamlKey : field.name;
            if (node[key]) remapField(field, node[key], remap, depth);
        }
    }

    // The entity references a component holds: its table's Entity fields, and a script's
    // EntityReference properties
    void remapComponent(const std::string& key, YAML::Node node, const std::map<uint64_t, uint64_t>& remap) {
        if (!defined(node) || !node.IsMap()) return;
        if (const reflect::ComponentDesc* desc = reflect::findByKey(key)) remapStruct(desc->body, node, remap, 0);
        if (key == "script" && node["scripts"] && node["scripts"].IsSequence()) {
            for (YAML::Node script : node["scripts"]) {
                if (!script.IsMap() || !script["properties"] || !script["properties"].IsSequence()) continue;
                for (YAML::Node property : script["properties"]) {
                    if (property.IsMap() && property["type"] && property["type"].IsScalar() && property["type"].Scalar() == "EntityReference" &&
                        property["value"]) {
                        remapScalar(property["value"], remap);
                    }
                }
            }
        }
    }

    void remapInstanceBlock(YAML::Node block, const std::map<uint64_t, uint64_t>& remap) {
        if (!defined(block) || !block.IsMap()) return;
        if (YAML::Node ids = block["memberIds"]; ids && ids.IsSequence()) {
            for (YAML::Node pair : ids) {
                if (pair.IsSequence() && pair.size() == 2) remapScalar(pair[1], remap);
            }
        }
        if (YAML::Node added = block["addedGameObjects"]; added && added.IsSequence()) {
            for (YAML::Node entry : added) {
                if (entry.IsMap() && entry["entity"]) remapScalar(entry["entity"], remap);
            }
        }
        if (YAML::Node nested = block["nested"]; nested && nested.IsSequence()) {
            for (YAML::Node entry : nested) remapInstanceBlock(entry, remap);
        }
    }

    void remapModel(SceneYamlModel& model, const std::map<uint64_t, uint64_t>& remap) {
        if (remap.empty()) return;
        std::map<uint64_t, SceneObject> objects;
        for (auto& [id, object] : model.objects) {
            auto it = remap.find(object.id);
            if (it != remap.end()) object.id = it->second;
            auto parent = remap.find(object.parentId);
            if (parent != remap.end()) object.parentId = parent->second;
            for (auto& [key, node] : object.components) {
                if (object.settings) {
                    if (key == "mainCamera" || key == "root") remapScalar(node, remap);
                } else if (key == kPrefabInstanceKey) {
                    remapInstanceBlock(node, remap);
                } else if (key != kGameObjectKey) {
                    remapComponent(key, node, remap);
                }
            }
            objects.emplace(object.id, std::move(object));
        }
        model.objects = std::move(objects);
        auto root = remap.find(model.prefabRoot);
        if (root != remap.end()) model.prefabRoot = root->second;
    }

    // a deep copy of a model (its nodes cloned: the merge edits the copy)
    SceneYamlModel clonedModel(const SceneYamlModel& model) {
        SceneYamlModel copy = model;
        for (auto& [id, object] : copy.objects) {
            // reset(): assigning a node would write through to the model it was copied from
            for (auto& [key, node] : object.components) node.reset(cloned(node));
        }
        return copy;
    }

    bool sameContent(const SceneObject& a, const SceneObject& b) {
        if (a.parentId != b.parentId || a.components.size() != b.components.size()) return false;
        for (const auto& [key, node] : a.components) {
            const YAML::Node* other = b.component(key);
            if (!other || !valuesEqual(node, *other)) return false;
        }
        return true;
    }

    bool modifiedFrom(const SceneObject& base, const SceneObject& side) {
        return !sameContent(base, side) || base.siblingIndex != side.siblingIndex;
    }

    // ---------------------------------------------------------------- the merge

    struct Merger {
        const std::map<std::string, MergeSide>& resolutions;
        std::vector<MergeConflict>& conflicts;
        uint64_t object = 0;
        std::string objectName;

        // a conflict, or the side its resolution names; ours unless theirs is chosen
        YAML::Node conflict(const std::string& path, const std::string& reason, const YAML::Node& b, const YAML::Node& o, const YAML::Node& t) {
            MergeConflict entry;
            entry.object = object;
            entry.name = objectName;
            entry.path = path;
            entry.reason = reason;
            entry.base = flowString(b);
            entry.ours = flowString(o);
            entry.theirs = flowString(t);
            auto it = resolutions.find(entry.key());
            if (it != resolutions.end()) return it->second == MergeSide::Theirs ? cloned(t) : cloned(o);
            conflicts.push_back(std::move(entry));
            return cloned(o);
        }

        YAML::Node merge(const std::string& component, std::string_view field, const YAML::Node& b, const YAML::Node& o, const YAML::Node& t,
                         const std::string& path, bool componentLevel) {
            if (valuesEqual(o, t)) return cloned(o);
            if (valuesEqual(b, o)) return cloned(t);
            if (valuesEqual(b, t)) return cloned(o);
            const bool baseMapOrNone = !defined(b) || b.IsMap();
            if (defined(o) && defined(t) && o.IsMap() && t.IsMap() && baseMapOrNone) {
                YAML::Node out(YAML::NodeType::Map);
                std::vector<std::string> keys;
                std::set<std::string> seen;
                for (const YAML::Node* map : {&o, &t}) {
                    for (auto it = map->begin(); it != map->end(); ++it) {
                        const std::string key = it->first.Scalar();
                        if (seen.insert(key).second) keys.push_back(key);
                    }
                }
                if (defined(b)) {
                    for (auto it = b.begin(); it != b.end(); ++it) {
                        const std::string key = it->first.Scalar();
                        if (seen.insert(key).second) keys.push_back(key);
                    }
                }
                for (const std::string& key : keys) {
                    const YAML::Node vb = defined(b) ? b[key] : undefinedNode();
                    const YAML::Node vo = o[key] ? o[key] : undefinedNode();
                    const YAML::Node vt = t[key] ? t[key] : undefinedNode();
                    YAML::Node value;
                    if (componentLevel && isDerivedKey(component, key)) {
                        // a derived value is recomputed: ours, never a conflict
                        value = defined(vo) ? cloned(vo) : cloned(vt);
                    } else {
                        value = merge(component, key, vb, vo, vt, joined(path, key), false);
                    }
                    if (defined(value)) out[key] = value;
                }
                return out;
            }
            const bool sequences = defined(o) && defined(t) && o.IsSequence() && t.IsSequence() && (!defined(b) || b.IsSequence());
            if (sequences && keyedSequence(field, o) && keyedSequence(field, t) && (!defined(b) || keyedSequence(field, b))) {
                auto items = [field](const YAML::Node& sequence) {
                    std::vector<std::pair<std::string, YAML::Node>> out;
                    if (!defined(sequence)) return out;
                    for (const YAML::Node& item : sequence) {
                        std::string key;
                        sequenceItemKey(field, item, key);
                        out.emplace_back(key, item);
                    }
                    return out;
                };
                const auto itemsB = items(b), itemsO = items(o), itemsT = items(t);
                std::map<std::string, YAML::Node> byB(itemsB.begin(), itemsB.end()), byT(itemsT.begin(), itemsT.end());
                YAML::Node out(YAML::NodeType::Sequence);
                std::set<std::string> done;
                auto take = [&](const std::string& key, const YAML::Node& vo) {
                    done.insert(key);
                    auto ib = byB.find(key);
                    auto it = byT.find(key);
                    YAML::Node value = merge(component, std::string_view(), ib != byB.end() ? ib->second : undefinedNode(), vo,
                                             it != byT.end() ? it->second : undefinedNode(), path + "[" + key + "]", false);
                    if (defined(value)) out.push_back(value);
                };
                // ours' order, then what only theirs has, in theirs' order
                for (const auto& [key, item] : itemsO) take(key, item);
                for (const auto& [key, item] : itemsT) {
                    if (!done.count(key)) take(key, undefinedNode());
                }
                return out;
            }
            if (isDerivedDocumentKey(field)) return cloned(o);
            return conflict(path, "changed on both sides", b, o, t);
        }
    };

}

std::string MergeConflict::key() const {
    return std::to_string(object) + "/" + path;
}

MergeResult mergeScenes(const SceneYamlModel& base, const SceneYamlModel& ours, const SceneYamlModel& theirsIn,
                        const std::map<std::string, MergeSide>& resolutions) {
    MergeResult result;
    if (ours.kind != theirsIn.kind) {
        result.error = "one side is a scene and the other a prefab";
        return result;
    }
    SceneYamlModel theirs = clonedModel(theirsIn);

    // ---- both sides added the same id with different content: theirs is renumbered (R17)
    uint64_t top = std::max({base.maxId(), ours.maxId(), theirs.maxId()});
    for (const SceneYamlModel* model : std::initializer_list<const SceneYamlModel*>{&base, &ours, &theirs}) {
        const uint64_t next = model->nextEntity();
        if (next > 0) top = std::max(top, next - 1);
    }
    std::map<uint64_t, uint64_t> remap;
    for (const auto& [id, object] : theirs.objects) {
        if (object.settings || base.find(id)) continue;
        const SceneObject* mine = ours.find(id);
        if (mine && !sameContent(*mine, object)) remap[id] = ++top;
    }
    remapModel(theirs, remap);
    result.renumbered = remap;

    SceneYamlModel merged;
    merged.kind = ours.kind;
    merged.formatVersion = ours.formatVersion;
    merged.documentKeys = ours.documentKeys;
    for (const std::string& key : theirs.documentKeys) {
        if (std::find(merged.documentKeys.begin(), merged.documentKeys.end(), key) == merged.documentKeys.end()) merged.documentKeys.push_back(key);
    }

    Merger merger{resolutions, result.conflicts};
    std::set<uint64_t> ids;
    for (const SceneYamlModel* model : std::initializer_list<const SceneYamlModel*>{&base, &ours, &theirs}) {
        for (const auto& [id, object] : model->objects) ids.insert(id);
    }
    for (uint64_t id : ids) {
        const SceneObject* b = base.find(id);
        const SceneObject* o = ours.find(id);
        const SceneObject* t = theirs.find(id);
        merger.object = id;
        merger.objectName = o ? o->name : t ? t->name : b->name;
        const SceneObject* kept = nullptr;
        if (!b) {
            // added: by one side, or by both with the same content
            kept = o ? o : t;
            merged.objects.emplace(id, *kept);
            continue;
        }
        if (!o && !t) continue;
        if (!o || !t) {
            const SceneObject* survivor = o ? o : t;
            if (!modifiedFrom(*b, *survivor)) continue;   // deleted on one side, untouched on the other
            // the conflict's sides say what each did to it; the chosen one (ours, unless a
            // resolution says theirs) decides whether it stays
            const YAML::Node choice = merger.conflict("", o ? "deleted in theirs, modified in ours" : "deleted in ours, modified in theirs",
                                                      YAML::Node("present"), YAML::Node(o ? "modified" : "deleted"),
                                                      YAML::Node(t ? "modified" : "deleted"));
            const bool keep = choice.IsScalar() && choice.Scalar() == "modified";
            if (keep) merged.objects.emplace(id, *survivor);
            continue;
        }
        SceneObject out;
        out.id = id;
        out.settings = o->settings;
        out.type = o->type;
        // the parent and the sibling index, each from the side that changed it
        if (o->parentId == t->parentId || b->parentId == t->parentId) out.parentId = o->parentId;
        else if (b->parentId == o->parentId) out.parentId = t->parentId;
        else out.parentId = merger.conflict("@parent", "moved on both sides", YAML::Node(b->parentId), YAML::Node(o->parentId),
                                            YAML::Node(t->parentId)).as<uint64_t>();
        if (o->siblingIndex == t->siblingIndex || b->siblingIndex == t->siblingIndex) out.siblingIndex = o->siblingIndex;
        else if (b->siblingIndex == o->siblingIndex) out.siblingIndex = t->siblingIndex;
        else out.siblingIndex = merger.conflict("@siblingIndex", "reordered on both sides", YAML::Node(b->siblingIndex),
                                                YAML::Node(o->siblingIndex), YAML::Node(t->siblingIndex)).as<int>();
        // components: ours' order, then theirs' new ones
        std::vector<std::string> keys;
        std::set<std::string> seen;
        for (const SceneObject* side : {o, t, b}) {
            for (const auto& [key, node] : side->components) {
                if (seen.insert(key).second) keys.push_back(key);
            }
        }
        for (const std::string& key : keys) {
            const YAML::Node* vb = b->component(key);
            const YAML::Node* vo = o->component(key);
            const YAML::Node* vt = t->component(key);
            const YAML::Node nb = vb ? *vb : undefinedNode();
            const YAML::Node no = vo ? *vo : undefinedNode();
            const YAML::Node nt = vt ? *vt : undefinedNode();
            YAML::Node value;
            // (a prefab's nextEntity is set to the max below: never a conflict)
            if (out.settings && (isDerivedDocumentKey(key) || key == "nextEntity")) {
                value = defined(no) ? cloned(no) : cloned(nt);
            } else {
                value = merger.merge(key, key, nb, no, nt, key, !out.settings);
            }
            if (defined(value)) out.components.emplace_back(key, value);
        }
        if (const YAML::Node* own = out.component(kGameObjectKey); own && (*own)["name"] && (*own)["name"].IsScalar()) out.name = (*own)["name"].Scalar();
        else out.name = o->name;
        if (out.settings) out.name = o->name;
        merged.objects.emplace(id, std::move(out));
    }

    // a GameObject whose parent the merge removed goes to the root (a conflict)
    for (auto& [id, object] : merged.objects) {
        if (object.settings || object.parentId == 0 || merged.find(object.parentId)) continue;
        merger.object = id;
        merger.objectName = object.name;
        merger.conflict("@parent", "its parent was deleted", YAML::Node(object.parentId), YAML::Node(object.parentId), YAML::Node(0));
        object.parentId = 0;
    }
    // siblings in one order without gaps: the merged indices, ties by id
    std::map<uint64_t, std::vector<std::pair<int, uint64_t>>> byParent;
    for (const auto& [id, object] : merged.objects) {
        if (!object.settings) byParent[object.parentId].emplace_back(object.siblingIndex, id);
    }
    for (auto& [parent, children] : byParent) {
        std::sort(children.begin(), children.end());
        int index = 0;
        for (const auto& [old, id] : children) merged.objects[id].siblingIndex = index++;
    }
    // R17: a prefab's nextEntity takes the max, above every id
    if (merged.kind == DocumentKind::Prefab) {
        merged.prefabRoot = ours.prefabRoot;
        if (SceneObject* settings = merged.objects.count(kSceneSettingsId) ? &merged.objects[kSceneSettingsId] : nullptr) {
            const uint64_t next = std::max({ours.nextEntity(), theirs.nextEntity(), merged.maxId() + 1});
            if (YAML::Node* value = settings->component("nextEntity")) value->reset(YAML::Node(next));
            else settings->components.emplace_back("nextEntity", YAML::Node(next));
        }
    }

    // round trip before anything is written (Risks: a merge never writes what does not load)
    const std::string text = merged.text();
    SceneYamlModel check;
    std::string parseError;
    if (!SceneYamlModel::parse(text, check, &parseError)) {
        result.error = "the merged document does not read back: " + parseError;
        return result;
    }
    result.merged = std::move(merged);
    result.text = text;
    result.ok = true;
    return result;
}

MergeResult mergeSceneTexts(const std::string& baseText, const std::string& oursText, const std::string& theirsText,
                            const std::map<std::string, MergeSide>& resolutions) {
    MergeResult result;
    // no base (both added the file): an empty document of the same kind
    const bool emptyBase = baseText.find_first_not_of(" \t\r\n") == std::string::npos;
    std::vector<std::string_view> texts = {oursText, theirsText};
    if (!emptyBase) texts.push_back(baseText);
    std::vector<SceneYamlModel> models;
    std::vector<std::string> errors;
    if (!SceneYamlModel::parseAll(texts, models, errors)) {
        static const char* const sides[] = {"ours", "theirs", "base"};
        for (size_t i = 0; i < errors.size(); i++) {
            if (!errors[i].empty()) {
                result.error = std::string(sides[i]) + ": " + errors[i];
                break;
            }
        }
        return result;
    }
    SceneYamlModel base;
    if (emptyBase) {
        base.kind = models[0].kind;
        base.objects.emplace(kSceneSettingsId, SceneObject{kSceneSettingsId, "Scene Settings", 0, 0, "Entity", true, {}});
    } else {
        base = std::move(models[2]);
    }
    return mergeScenes(base, models[0], models[1], resolutions);
}

std::string conflictsText(const std::string& file, const std::vector<MergeConflict>& conflicts) {
    YAML::Node root;
    root["file"] = file;
    YAML::Node list(YAML::NodeType::Sequence);
    for (const MergeConflict& conflict : conflicts) {
        YAML::Node entry;
        entry["object"] = conflict.object == kSceneSettingsId ? std::string("Scene Settings") : std::to_string(conflict.object);
        entry["name"] = conflict.name;
        entry["path"] = conflict.path;
        entry["reason"] = conflict.reason;
        entry["base"] = conflict.base;
        entry["ours"] = conflict.ours;
        entry["theirs"] = conflict.theirs;
        list.push_back(entry);
    }
    root["conflicts"] = list;
    return YAML::Dump(root) + "\n";
}

}
