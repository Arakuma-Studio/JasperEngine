// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/scene/SceneYamlModel.h"

#include "DocumentFile.h"
#include "reflect/Reflect.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <set>

namespace jasperengine::editor::vcs::scene {

namespace {

    // the entity node's keys that are no GameObject value of their own
    bool structuralEntityKey(std::string_view key) {
        return key == "type" || key == "entity" || key == "components" || key == "children" || key == kPrefabInstanceKey;
    }

    // the GameObject's keys Stream writes before its components (encodeEntityAux)
    bool leadingEntityKey(std::string_view key) {
        return key == "name" || key == "active" || key == "tag" || key == "layer";
    }

    bool parseNumber(const std::string& text, double& out) {
        if (text.empty()) return false;
        const char* begin = text.c_str();
        char* end = nullptr;
        out = std::strtod(begin, &end);
        if (end == begin || *end != '\0') return false;
        // "nan"/"inf" spellings compare as text
        return std::isfinite(out);
    }

    // The serialization rank of a component key (reflection's order); unknown ones after all
    size_t componentRank(const std::string& key) {
        static const std::map<std::string, size_t, std::less<>> ranks = []() {
            std::map<std::string, size_t, std::less<>> out;
            size_t rank = 0;
            for (const reflect::ComponentDesc* desc : reflect::serializationOrder()) out.emplace(desc->key, rank++);
            return out;
        }();
        auto it = ranks.find(key);
        return it != ranks.end() ? it->second : ranks.size();
    }

}

const YAML::Node* SceneObject::component(std::string_view key) const {
    for (const auto& [name, node] : components) {
        if (name == key) return &node;
    }
    return nullptr;
}

YAML::Node* SceneObject::component(std::string_view key) {
    for (auto& [name, node] : components) {
        if (name == key) return &node;
    }
    return nullptr;
}

bool parseId(const YAML::Node& node, uint64_t& out) {
    if (!node || !node.IsScalar()) return false;
    const std::string& text = node.Scalar();
    if (text.empty() || text[0] == '-' || text[0] == '+') return false;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    out = static_cast<uint64_t>(value);
    return true;
}

std::string flowString(const YAML::Node& node) {
    if (!node || !node.IsDefined()) return std::string();
    if (node.IsNull()) return "~";
    if (node.IsScalar()) return node.Scalar();
    YAML::Emitter out;
    out.SetMapFormat(YAML::Flow);
    out.SetSeqFormat(YAML::Flow);
    out << node;
    return out.c_str();
}

bool valuesEqual(const YAML::Node& a, const YAML::Node& b) {
    const bool hasA = a && a.IsDefined();
    const bool hasB = b && b.IsDefined();
    if (!hasA || !hasB) return hasA == hasB;
    if (a.Type() != b.Type()) {
        // an empty map and "{}" written as null read as one value
        const bool emptyA = a.IsNull() || ((a.IsMap() || a.IsSequence()) && a.size() == 0);
        const bool emptyB = b.IsNull() || ((b.IsMap() || b.IsSequence()) && b.size() == 0);
        return emptyA && emptyB && (a.IsNull() || b.IsNull());
    }
    switch (a.Type()) {
        case YAML::NodeType::Null:
        case YAML::NodeType::Undefined:
            return true;
        case YAML::NodeType::Scalar: {
            if (a.Scalar() == b.Scalar()) return true;
            double x = 0.0, y = 0.0;
            if (!parseNumber(a.Scalar(), x) || !parseNumber(b.Scalar(), y)) return false;
            // R14: a relative epsilon of 1e-6
            const double scale = std::max({1.0, std::fabs(x), std::fabs(y)});
            return std::fabs(x - y) <= 1e-6 * scale;
        }
        case YAML::NodeType::Sequence: {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); i++) {
                if (!valuesEqual(a[i], b[i])) return false;
            }
            return true;
        }
        case YAML::NodeType::Map: {
            if (a.size() != b.size()) return false;
            for (auto it = a.begin(); it != a.end(); ++it) {
                const YAML::Node other = b[it->first.Scalar()];
                if (!other || !valuesEqual(it->second, other)) return false;
            }
            return true;
        }
    }
    return false;
}

bool SceneYamlModel::parse(std::string_view text, SceneYamlModel& out, std::string* error) {
    YAML::Node document;
    try {
        document = YAML::Load(std::string(text));
    } catch (const YAML::Exception& e) {
        if (error) *error = std::string("not YAML: ") + e.what();
        return false;
    }
    return fromDocument(document, out, error);
}

bool SceneYamlModel::fromDocument(const YAML::Node& source, SceneYamlModel& out, std::string* error) {
    out = SceneYamlModel();
    if (!source || !source.IsMap()) {
        if (error) *error = "the document is not a map";
        return false;
    }
    YAML::Node root = YAML::Clone(source);
    out.kind = root["type"] && root["type"].IsScalar() && root["type"].Scalar() == "Prefab" ? DocumentKind::Prefab : DocumentKind::Scene;
    try {
        // R13: the baseline in the current format before any comparison
        out.migration = StreamMigrations::migrate(out.kind, root);
    } catch (const std::exception& e) {
        if (error) *error = std::string("migration failed: ") + e.what();
        return false;
    }
    out.formatVersion = StreamMigrations::readVersion(out.kind, root);
    const char* listKey = out.kind == DocumentKind::Prefab ? "members" : "entities";
    if (out.kind == DocumentKind::Prefab) parseId(root["root"], out.prefabRoot);

    SceneObject settings;
    settings.id = kSceneSettingsId;
    settings.name = "Scene Settings";
    settings.settings = true;
    YAML::Node entities;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it->first.IsScalar()) {
            if (error) *error = "a document key is not a scalar";
            return false;
        }
        const std::string key = it->first.Scalar();
        out.documentKeys.push_back(key);
        if (key == listKey) {
            entities = it->second;
            continue;
        }
        settings.components.emplace_back(key, it->second);
    }
    if (entities && !entities.IsSequence() && !entities.IsNull()) {
        if (error) *error = std::string("`") + listKey + "` is not a list";
        return false;
    }
    out.objects.emplace(kSceneSettingsId, std::move(settings));

    // GameObjects without an id (hand-written files) get one above every id of the file, so
    // they still compare (as added or removed: they have no identity across versions)
    std::vector<YAML::Node> unnamed;
    uint64_t synthetic = 0;
    std::function<bool(const YAML::Node&, uint64_t, int)> visit = [&](const YAML::Node& node, uint64_t parent, int index) -> bool {
        if (!node.IsMap()) {
            if (error) *error = "a GameObject is not a map";
            return false;
        }
        SceneObject object;
        object.parentId = parent;
        object.siblingIndex = index;
        if (node["type"] && node["type"].IsScalar()) object.type = node["type"].Scalar();
        if (!parseId(node["entity"], object.id) || object.id == 0 || out.objects.count(object.id)) {
            object.id = (uint64_t(1) << 62) + (++synthetic);
        }
        if (node["name"] && node["name"].IsScalar()) object.name = node["name"].Scalar();
        YAML::Node own(YAML::NodeType::Map);
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string key = it->first.Scalar();
            if (structuralEntityKey(key)) continue;
            own[key] = it->second;
        }
        if (own.size() > 0) object.components.emplace_back(kGameObjectKey, own);
        if (const YAML::Node components = node["components"]; components && components.IsMap()) {
            for (auto it = components.begin(); it != components.end(); ++it) {
                object.components.emplace_back(it->first.Scalar(), it->second);
            }
        }
        if (node[kPrefabInstanceKey]) object.components.emplace_back(kPrefabInstanceKey, node[kPrefabInstanceKey]);
        const uint64_t id = object.id;
        out.objects.emplace(id, std::move(object));
        if (const YAML::Node children = node["children"]; children && children.IsSequence()) {
            int childIndex = 0;
            for (const YAML::Node& child : children) {
                if (!visit(child, id, childIndex++)) return false;
            }
        }
        return true;
    };
    if (entities && entities.IsSequence()) {
        int index = 0;
        for (const YAML::Node& entity : entities) {
            if (!visit(entity, 0, index++)) return false;
        }
    }
    return true;
}

const SceneObject* SceneYamlModel::find(uint64_t id) const {
    auto it = objects.find(id);
    return it != objects.end() ? &it->second : nullptr;
}

std::vector<uint64_t> SceneYamlModel::childrenOf(uint64_t parentId) const {
    std::vector<std::pair<int, uint64_t>> children;
    for (const auto& [id, object] : objects) {
        if (!object.settings && object.parentId == parentId) children.emplace_back(object.siblingIndex, id);
    }
    std::sort(children.begin(), children.end());
    std::vector<uint64_t> ids;
    ids.reserve(children.size());
    for (const auto& [index, id] : children) ids.push_back(id);
    return ids;
}

std::vector<uint64_t> SceneYamlModel::treeOrder() const {
    std::map<uint64_t, std::vector<std::pair<int, uint64_t>>> byParent;
    for (const auto& [id, object] : objects) {
        if (!object.settings) byParent[object.parentId].emplace_back(object.siblingIndex, id);
    }
    for (auto& [parent, children] : byParent) std::sort(children.begin(), children.end());
    std::vector<uint64_t> order;
    order.reserve(objects.size());
    if (objects.count(kSceneSettingsId)) order.push_back(kSceneSettingsId);
    std::set<uint64_t> seen;
    std::function<void(uint64_t)> walk = [&](uint64_t parent) {
        auto it = byParent.find(parent);
        if (it == byParent.end()) return;
        for (const auto& [index, id] : it->second) {
            if (!seen.insert(id).second) continue;
            order.push_back(id);
            walk(id);
        }
    };
    walk(0);
    // a parent cycle or a parent that is gone: still listed
    for (const auto& [id, object] : objects) {
        if (!object.settings && !seen.count(id)) {
            seen.insert(id);
            order.push_back(id);
            walk(id);
        }
    }
    return order;
}

uint64_t SceneYamlModel::maxId() const {
    uint64_t max = 0;
    for (const auto& [id, object] : objects) {
        if (!object.settings && id < (uint64_t(1) << 62)) max = std::max(max, id);
    }
    return max;
}

uint64_t SceneYamlModel::nextEntity() const {
    const SceneObject* settings = find(kSceneSettingsId);
    if (!settings) return 0;
    const YAML::Node* next = settings->component("nextEntity");
    uint64_t value = 0;
    return next && parseId(*next, value) ? value : 0;
}

YAML::Node SceneYamlModel::entityNode(uint64_t id, const std::function<bool(uint64_t)>& includeChild) const {
    const SceneObject* found = find(id);
    if (!found) return YAML::Node(YAML::NodeType::Undefined);
    const SceneObject& object = *found;
    YAML::Node node(YAML::NodeType::Map);
    node["type"] = object.type;
    if (object.id < (uint64_t(1) << 62)) node["entity"] = object.id;
    const YAML::Node* own = object.component(kGameObjectKey);
    if (own && own->IsMap()) {
        for (const char* key : {"name", "active", "tag", "layer"}) {
            if ((*own)[key]) node[key] = (*own)[key];
        }
    }
    std::vector<std::pair<std::string, YAML::Node>> components;
    for (const auto& [key, value] : object.components) {
        if (key != kGameObjectKey && key != kPrefabInstanceKey) components.emplace_back(key, value);
    }
    std::stable_sort(components.begin(), components.end(),
                     [](const auto& a, const auto& b) { return componentRank(a.first) < componentRank(b.first); });
    if (!components.empty()) {
        YAML::Node map(YAML::NodeType::Map);
        for (const auto& [key, value] : components) map[key] = value;
        node["components"] = map;
    }
    if (const YAML::Node* instance = object.component(kPrefabInstanceKey)) node[kPrefabInstanceKey] = *instance;
    if (own && own->IsMap()) {
        for (auto it = own->begin(); it != own->end(); ++it) {
            const std::string key = it->first.Scalar();
            if (!leadingEntityKey(key)) node[key] = it->second;
        }
    }
    YAML::Node list(YAML::NodeType::Sequence);
    for (uint64_t child : childrenOf(object.id)) {
        if (!includeChild || includeChild(child)) list.push_back(entityNode(child, includeChild));
    }
    if (list.size() > 0) node["children"] = list;
    return node;
}

YAML::Node SceneYamlModel::encode() const {
    const SceneObject* settings = find(kSceneSettingsId);
    const char* listKey = kind == DocumentKind::Prefab ? "members" : "entities";

    YAML::Node root(YAML::NodeType::Map);
    auto emitList = [&]() {
        // no GameObjects: a null list, as Stream writes it
        YAML::Node list;
        for (uint64_t id : childrenOf(0)) list.push_back(entityNode(id));
        root[listKey] = list;
    };
    bool listed = false;
    std::set<std::string> written;
    for (const std::string& key : documentKeys) {
        if (key == listKey) {
            emitList();
            listed = true;
            continue;
        }
        if (!settings) continue;
        if (const YAML::Node* value = settings->component(key)) {
            root[key] = *value;
            written.insert(key);
        }
    }
    // document keys the merge brought from the other side
    if (settings) {
        for (const auto& [key, value] : settings->components) {
            if (!written.count(key)) root[key] = value;
        }
    }
    if (!listed) emitList();
    return root;
}

std::string SceneYamlModel::text() const {
    return DocumentFiles::text(encode());
}

}
