// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StreamPreserved.h"
#include "component/Transform.h"

#include "Stream.h"
#include "util/NearestName.h"

#include <algorithm>
#include <map>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

// ---- PreservedEntity / PreservedStore ----

bool PreservedEntity::empty() const {
    if (!rootKeys.empty() || !components.empty())
        return false;
    for (const PreservedFieldGroup& group : fieldGroups) {
        if (!group.fields.empty())
            return false;
    }
    return true;
}

PreservedFieldGroup* PreservedEntity::group(const std::string& component) {
    for (PreservedFieldGroup& group : fieldGroups) {
        if (group.component == component)
            return &group;
    }
    return nullptr;
}

const PreservedFieldGroup* PreservedEntity::group(const std::string& component) const {
    for (const PreservedFieldGroup& group : fieldGroups) {
        if (group.component == component)
            return &group;
    }
    return nullptr;
}

void PreservedStore::entityDestroyed(Entity entity) {
    entities.erase(entity);
}

void PreservedStore::componentRemoved(Entity entity, ComponentId componentId) {
    auto it = entities.find(entity);
    if (it == entities.end())
        return;
    std::vector<PreservedFieldGroup>& groups = it->second.fieldGroups;
    groups.erase(std::remove_if(groups.begin(), groups.end(), [componentId](const PreservedFieldGroup& group) {
        return group.hasComponentId && group.componentId == componentId;
    }), groups.end());
    // a stand-in Transform goes with the live one
    auto& components = it->second.components;
    components.erase(std::remove_if(components.begin(), components.end(), [componentId](const auto& entry) {
        return entry.second.shadow && entry.second.componentId == componentId;
    }), components.end());
    prune(entity);
}

void PreservedStore::registryCleared() {
    entities.clear();
}

PreservedEntity* PreservedStore::find(Entity entity) {
    auto it = entities.find(entity);
    return it == entities.end() ? nullptr : &it->second;
}

const PreservedEntity* PreservedStore::find(Entity entity) const {
    auto it = entities.find(entity);
    return it == entities.end() ? nullptr : &it->second;
}

PreservedEntity& PreservedStore::entry(Entity entity) {
    return entities[entity];
}

void PreservedStore::reset(Entity entity) {
    entities.erase(entity);
}

void PreservedStore::resetComponent(Entity entity, const std::string& component) {
    auto it = entities.find(entity);
    if (it == entities.end())
        return;
    auto& groups = it->second.fieldGroups;
    groups.erase(std::remove_if(groups.begin(), groups.end(), [&component](const PreservedFieldGroup& group) {
        return group.component == component;
    }), groups.end());
    auto& components = it->second.components;
    components.erase(std::remove_if(components.begin(), components.end(), [&component](const auto& entry) {
        return entry.first == component;
    }), components.end());
    prune(entity);
}

void PreservedStore::prune(Entity entity) {
    auto it = entities.find(entity);
    if (it == entities.end())
        return;
    auto& groups = it->second.fieldGroups;
    groups.erase(std::remove_if(groups.begin(), groups.end(), [](const PreservedFieldGroup& group) {
        return group.fields.empty();
    }), groups.end());
    if (it->second.empty())
        entities.erase(it);
}

bool PreservedStore::holds(Entity entity, const std::string& component, const std::string& field) const {
    const PreservedEntity* preserved = find(entity);
    if (!preserved)
        return false;
    if (component.empty()) {
        return std::any_of(preserved->rootKeys.begin(), preserved->rootKeys.end(), [&field](const auto& entry) {
            return entry.first == field;
        });
    }
    if (field.empty()) {
        return std::any_of(preserved->components.begin(), preserved->components.end(), [&component](const auto& entry) {
            return entry.first == component;
        });
    }
    const PreservedFieldGroup* group = preserved->group(component);
    return group && std::any_of(group->fields.begin(), group->fields.end(), [&field](const auto& entry) {
        return entry.first == field;
    });
}

size_t PreservedStore::count() const {
    size_t total = 0;
    for (const auto& [entity, preserved] : entities) {
        total += preserved.rootKeys.size() + preserved.components.size();
        for (const PreservedFieldGroup& group : preserved.fieldGroups)
            total += group.fields.size();
    }
    return total;
}

std::vector<std::string> PreservedStore::preservedComponentNames(Entity entity) const {
    std::vector<std::string> names;
    if (const PreservedEntity* preserved = find(entity)) {
        for (const auto& [name, component] : preserved->components) {
            if (!component.shadow)
                names.push_back(name);
        }
    }
    return names;
}

size_t PreservedStore::preservedComponentCount() const {
    size_t count = 0;
    for (const auto& [entity, preserved] : entities) {
        for (const auto& [name, component] : preserved.components) {
            if (!component.shadow) count++;
        }
    }
    return count;
}

YAML::Node PreservedStore::preservedComponent(Entity entity, const std::string& component) const {
    if (const PreservedEntity* preserved = find(entity)) {
        for (const auto& [name, entry] : preserved->components) {
            if (name == component && !entry.shadow)
                return entry.authored;
        }
    }
    return YAML::Node();
}

bool PreservedStore::removePreservedComponent(Entity entity, const std::string& component) {
    PreservedEntity* preserved = find(entity);
    if (!preserved)
        return false;
    auto& components = preserved->components;
    auto it = std::find_if(components.begin(), components.end(), [&component](const auto& entry) {
        return entry.first == component && !entry.second.shadow;
    });
    if (it == components.end())
        return false;
    components.erase(it);
    prune(entity);
    return true;
}

void PreservedStore::restorePreservedComponent(Entity entity, const std::string& component, const YAML::Node& authored, size_t index) {
    auto& components = entry(entity).components;
    PreservedComponent preserved;
    preserved.authored = YAML::Clone(authored);
    components.emplace(components.begin() + (ptrdiff_t)std::min(index, components.size()), component, std::move(preserved));
}

size_t PreservedStore::preservedComponentIndex(Entity entity, const std::string& component) const {
    if (const PreservedEntity* preserved = find(entity)) {
        for (size_t i = 0; i < preserved->components.size(); i++) {
            if (preserved->components[i].first == component)
                return i;
        }
    }
    return 0;
}

PreservedStore* editor::findPreservedStore(const EntityRegistry* registry) {
    return registry ? dynamic_cast<PreservedStore*>(registry->getEditorData()) : nullptr;
}

PreservedStore& editor::preservedStore(EntityRegistry* registry) {
    PreservedStore* store = findPreservedStore(registry);
    if (!store) {
        auto created = std::make_shared<PreservedStore>();
        store = created.get();
        registry->setEditorData(std::move(created));
    }
    return *store;
}

// ---- decoding ----

struct editor::StreamLoad::Frame {
    EntityRegistry* registry = nullptr;
    Entity entity = NULL_ENTITY;
    std::string component;
    // the node being decoded (the authored one, or a copy without the keys that failed)
    YAML::Node node;
    // (d) candidates: the keys whose value did not decode, with their issue
    std::vector<std::pair<std::string, LoadIssue>> substitutions;
};

namespace {

    using Frame = StreamLoad::Frame;

    thread_local EntityRegistry* currentRegistry = nullptr;
    thread_local Entity currentEntity = NULL_ENTITY;
    thread_local Frame* currentFrame = nullptr;

    std::string scalarText(const YAML::Node& node) {
        return node && node.IsScalar() ? node.Scalar() : std::string();
    }

    void setPosition(LoadIssue& issue, const YAML::Node& node) {
        if (!node)
            return;
        const YAML::Mark mark = node.Mark();
        if (mark.line >= 0) {
            issue.line = mark.line + 1;
            issue.column = mark.column + 1;
        }
    }

    std::string nearest(const std::string& name, const std::vector<std::string>& candidates) {
        return NearestName(name, candidates);
    }

    // For a closed set of names the nearest one helps even past a typo's distance
    std::string nearestEnumName(const std::string& name, const std::vector<std::string>& candidates) {
        return name.empty() ? std::string() : NearestName(name, candidates, 64);
    }

    LoadIssue makeIssue(LoadIssueKind kind, LoadIssueFate fate, const EntityRegistry* registry, Entity entity,
            const std::string& component, const std::string& field, const std::string& message, const YAML::Node& at) {
        LoadIssue issue;
        issue.kind = kind;
        issue.fate = fate;
        issue.entity = entity;
        if (registry && entity != NULL_ENTITY)
            issue.entityPath = StreamLoad::entityPath(registry, entity);
        issue.component = component;
        issue.field = field;
        issue.message = message;
        issue.value = scalarText(at);
        setPosition(issue, at);
        return issue;
    }

    std::string dumpOrEmpty(const YAML::Node& node) {
        return node ? YAML::Dump(node) : std::string();
    }

    YAML::Node without(const YAML::Node& map, const std::string& key) {
        YAML::Node copy(YAML::NodeType::Map);
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (it->first.IsScalar() && it->first.Scalar() == key)
                continue;
            copy[it->first] = it->second;
        }
        return copy;
    }

    // The top-level key whose value holds the error position: the last value starting at or
    // before it
    std::string keyAtMark(const YAML::Node& map, const YAML::Mark& mark) {
        if (mark.pos < 0 || !map.IsMap())
            return {};
        std::string best;
        int bestPos = -1;
        for (auto it = map.begin(); it != map.end(); ++it) {
            const YAML::Mark start = it->second.Mark();
            if (start.pos >= 0 && start.pos <= mark.pos && start.pos > bestPos && it->first.IsScalar()) {
                bestPos = start.pos;
                best = it->first.Scalar();
            }
        }
        return best;
    }

    struct Failure {
        std::string message;
        YAML::Mark mark = YAML::Mark::null_mark();
    };

    bool attempt(Frame& frame, const YAML::Node& node, const std::function<void(const YAML::Node&)>& decode, Failure& failure) {
        // reset, not assignment: assigning to a bound handle would overwrite the node it
        // refers to (yaml-cpp), here the authored document
        frame.node.reset(node);
        frame.substitutions.clear();
        try {
            decode(node);
            return true;
        } catch (const YAML::Exception& e) {
            failure.message = e.msg;
            failure.mark = e.mark;
        } catch (const std::exception& e) {
            failure.message = e.what();
            failure.mark = YAML::Mark::null_mark();
        }
        return false;
    }

    std::string invalidMessage(const std::string& reason) {
        return reason.empty() ? std::string("invalid value") : "invalid value (" + reason + ")";
    }

}

StreamLoad::EntityScope::EntityScope(EntityRegistry* registry, Entity entity):
        previousRegistry(currentRegistry), previousEntity(currentEntity) {
    currentRegistry = registry;
    currentEntity = entity;
}

StreamLoad::EntityScope::~EntityScope() {
    currentRegistry = previousRegistry;
    currentEntity = previousEntity;
}

StreamLoad::BlockScope::BlockScope(EntityRegistry* registry, const std::string& block, const YAML::Node& node, RootKind kind):
        frame(new Frame()), previous(currentFrame) {
    frame->registry = registry;
    frame->entity = NULL_ENTITY;
    frame->component = block;
    frame->node.reset(node);
    currentFrame = frame;
    if (registry) {
        PreservedStore& store = preservedStore(registry);
        store.resetComponent(NULL_ENTITY, block);
        // the block's unknown keys stand next to its known ones
        if (node.IsMap()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                if (!it->first.IsScalar() || StreamSchema::isKnownRootKey(kind, it->first.Scalar()))
                    continue;
                const std::string key = it->first.Scalar();
                PreservedEntity& entry = store.entry(NULL_ENTITY);
                PreservedFieldGroup* group = entry.group(block);
                if (!group) {
                    entry.fieldGroups.push_back({block, false, 0, {}});
                    group = &entry.fieldGroups.back();
                }
                group->fields.emplace_back(key, PreservedField{YAML::Clone(it->second), false, {}});
                LoadIssue issue = makeIssue(LoadIssueKind::UnknownRootKey, LoadIssueFate::Preserved, registry, NULL_ENTITY,
                        block, key, "unknown key", it->first);
                issue.value.clear();
                issue.suggestion = nearest(key, StreamSchema::knownRootKeys(kind));
                recordLoadIssue(std::move(issue));
            }
        }
    }
}

StreamLoad::BlockScope::~BlockScope() {
    if (!finished)
        finish(YAML::Node());
    currentFrame = previous;
    delete frame;
}

void StreamLoad::BlockScope::finish(const YAML::Node& live) {
    if (finished)
        return;
    finished = true;
    for (auto& [key, issue] : frame->substitutions) {
        if (frame->registry && live) {
            PreservedEntity& entry = preservedStore(frame->registry).entry(NULL_ENTITY);
            PreservedFieldGroup* group = entry.group(frame->component);
            if (!group) {
                entry.fieldGroups.push_back({frame->component, false, 0, {}});
                group = &entry.fieldGroups.back();
            }
            group->fields.emplace_back(key, PreservedField{YAML::Clone(frame->node[key]), true, dumpOrEmpty(live[key])});
        } else {
            issue.fate = LoadIssueFate::Dropped;
        }
        recordLoadIssue(std::move(issue));
    }
    frame->substitutions.clear();
}

void StreamLoad::decodeComponent(ComponentType type, Entity entity, EntityRegistry* registry, const YAML::Node& compNode,
        const std::function<void(const YAML::Node&)>& decode) {
    const std::string name = Catalog::getComponentName(type, true);
    const YAML::Node authored = compNode[name];
    if (!authored)
        return;

    PreservedStore& store = preservedStore(registry);
    store.resetComponent(entity, name);

    Frame frame;
    frame.registry = registry;
    frame.entity = entity;
    frame.component = name;
    Frame* previousFrame = currentFrame;
    currentFrame = &frame;
    struct Restore {
        Frame* previous;
        ~Restore() { currentFrame = previous; }
    } restore{previousFrame};

    Failure failure;
    bool decoded = attempt(frame, authored, decode, failure);
    // keys whose value did not decode, and why
    std::vector<std::pair<std::string, std::string>> invalidKeys;
    if (!decoded && authored.IsMap()) {
        // the authored node never changes: each attempt decodes a fresh copy without the keys
        // that failed so far
        YAML::Node working;
        working.reset(authored);
        while (!decoded && invalidKeys.size() < 8) {
            std::string key = keyAtMark(working, failure.mark);
            if (key.empty()) {
                // no position (a value thrown on outside yaml-cpp): the key whose removal lets
                // the component decode
                for (auto it = working.begin(); it != working.end() && !decoded; ++it) {
                    if (!it->first.IsScalar())
                        continue;
                    const std::string candidate = it->first.Scalar();
                    const std::string reason = failure.message;
                    Failure next;
                    if (attempt(frame, without(working, candidate), decode, next)) {
                        invalidKeys.emplace_back(candidate, reason);
                        decoded = true;
                    }
                }
                break;
            }
            invalidKeys.emplace_back(key, failure.message);
            working.reset(without(working, key));
            decoded = attempt(frame, working, decode, failure);
        }
        if (!decoded)
            invalidKeys.clear();
    }

    const bool transform = type == ComponentType::Transform;
    if (!decoded) {
        // (e): kept whole; a Transform gets its default so the hierarchy stays
        PreservedComponent preserved;
        preserved.authored = YAML::Clone(authored);
        if (transform) {
            Failure ignored;
            if (attempt(frame, YAML::Node(YAML::NodeType::Map), decode, ignored)) {
                preserved.shadow = true;
                preserved.componentId = Catalog::getComponentId(registry, type);
                const YAML::Node live = Stream::encodeComponentsLive(entity, registry, Catalog::componentTypeToSignature(registry, type));
                preserved.liveAtLoad = dumpOrEmpty(live[name]);
            }
        }
        store.entry(entity).components.emplace_back(name, std::move(preserved));
        LoadIssue issue = makeIssue(LoadIssueKind::InvalidValue, LoadIssueFate::Preserved, registry, entity, name, "",
                transform ? "could not be read, a default Transform stands in (" + failure.message + ")" :
                            "could not be read, the component is kept but not loaded (" + failure.message + ")", authored);
        issue.value.clear();
        recordLoadIssue(std::move(issue));
        return;
    }

    std::vector<std::pair<std::string, PreservedField>> fields;
    // the component as the editor encodes it now (only when a stand-in needs it)
    YAML::Node live;
    bool liveEncoded = false;
    auto liveText = [&](const std::string& key) {
        if (!liveEncoded) {
            live.reset(Stream::encodeComponentsLive(entity, registry, Catalog::componentTypeToSignature(registry, type)));
            liveEncoded = true;
        }
        return dumpOrEmpty(live[name][key]);
    };

    // (d): the keys that failed, and the enum values the decoders did not know
    for (const auto& [key, reason] : invalidKeys) {
        fields.emplace_back(key, PreservedField{YAML::Clone(authored[key]), true, liveText(key)});
        LoadIssue issue = makeIssue(LoadIssueKind::InvalidValue, LoadIssueFate::Preserved, registry, entity, name, key,
                invalidMessage(reason), authored[key]);
        recordLoadIssue(std::move(issue));
    }
    for (auto& [key, issue] : frame.substitutions) {
        fields.emplace_back(key, PreservedField{YAML::Clone(authored[key]), true, liveText(key)});
        recordLoadIssue(std::move(issue));
    }

    // (b): keys this decoder does not read
    if (authored.IsMap()) {
        for (auto it = authored.begin(); it != authored.end(); ++it) {
            if (!it->first.IsScalar())
                continue;
            const std::string key = it->first.Scalar();
            if (StreamSchema::isKnownFieldKey(type, key))
                continue;
            fields.emplace_back(key, PreservedField{YAML::Clone(it->second), false, {}});
            LoadIssue issue = makeIssue(LoadIssueKind::UnknownField, LoadIssueFate::Preserved, registry, entity, name, key,
                    "unknown field", it->first);
            issue.value.clear();
            issue.suggestion = nearest(key, StreamSchema::knownFieldKeys(type));
            recordLoadIssue(std::move(issue));
        }
    }

    if (!fields.empty()) {
        PreservedFieldGroup group;
        group.component = name;
        group.hasComponentId = true;
        group.componentId = Catalog::getComponentId(registry, type);
        group.fields = std::move(fields);
        store.entry(entity).fieldGroups.push_back(std::move(group));
    }
}

void StreamLoad::preserveUnknownComponents(Entity entity, EntityRegistry* registry, const YAML::Node& compNode) {
    if (!compNode.IsMap())
        return;
    for (auto it = compNode.begin(); it != compNode.end(); ++it) {
        if (!it->first.IsScalar())
            continue;
        const std::string name = it->first.Scalar();
        ComponentType type;
        if (StreamSchema::findComponentType(name, type))
            continue;
        PreservedStore& store = preservedStore(registry);
        store.resetComponent(entity, name);
        PreservedComponent preserved;
        preserved.authored = YAML::Clone(it->second);
        store.entry(entity).components.emplace_back(name, std::move(preserved));
        LoadIssue issue = makeIssue(LoadIssueKind::UnknownComponent, LoadIssueFate::Preserved, registry, entity, name, "",
                "unknown component", it->first);
        issue.value.clear();
        issue.suggestion = nearest(name, StreamSchema::componentNames());
        recordLoadIssue(std::move(issue));
    }
}

void StreamLoad::preserveRootKeys(EntityRegistry* registry, Entity entity, const YAML::Node& node, RootKind kind) {
    if (!node.IsMap())
        return;
    PreservedStore* store = registry ? &preservedStore(registry) : nullptr;
    if (store) {
        if (PreservedEntity* preserved = store->find(entity))
            preserved->rootKeys.clear();
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar())
            continue;
        const std::string key = it->first.Scalar();
        if (StreamSchema::isKnownRootKey(kind, key))
            continue;
        if (store)
            store->entry(entity).rootKeys.emplace_back(key, YAML::Clone(it->second));
        LoadIssue issue = makeIssue(LoadIssueKind::UnknownRootKey, store ? LoadIssueFate::Preserved : LoadIssueFate::Dropped,
                registry, entity, "", key, "unknown key", it->first);
        issue.value.clear();
        issue.suggestion = nearest(key, StreamSchema::knownRootKeys(kind));
        recordLoadIssue(std::move(issue));
    }
    if (store)
        store->prune(entity);
}

void StreamLoad::reportRootKeys(const YAML::Node& node, RootKind kind) {
    if (!node.IsMap() || !ScopedLoadReport::current())
        return;
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar() || StreamSchema::isKnownRootKey(kind, it->first.Scalar()))
            continue;
        const std::string key = it->first.Scalar();
        LoadIssue issue = makeIssue(LoadIssueKind::UnknownRootKey, LoadIssueFate::Preserved, nullptr, NULL_ENTITY, "", key,
                "unknown key", it->first);
        issue.value.clear();
        issue.suggestion = nearest(key, StreamSchema::knownRootKeys(kind));
        recordLoadIssue(std::move(issue));
    }
}

void StreamLoad::checkNestedKeys(const YAML::Node& node, NestedKind kind) {
    Frame* frame = currentFrame;
    // only inside a component: a .material file's root keys are its document's (R28)
    if (!frame || frame->entity == NULL_ENTITY || !node || !node.IsMap() || !ScopedLoadReport::current())
        return;
    std::string path;
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar() || StreamSchema::isKnownNestedKey(kind, it->first.Scalar()))
            continue;
        if (path.empty())
            path = locate(frame->node, node);
        const std::string key = it->first.Scalar();
        LoadIssue issue = makeIssue(LoadIssueKind::UnknownField, LoadIssueFate::Dropped, frame->registry, frame->entity,
                frame->component, path.empty() ? key : path + "." + key,
                std::string("unknown field of ") + StreamSchema::nestedKindName(kind), it->first);
        issue.value.clear();
        issue.suggestion = nearest(key, StreamSchema::knownNestedKeys(kind));
        recordLoadIssue(std::move(issue));
    }
}

void StreamLoad::unknownEntityType(const YAML::Node& entityNode, EntityRegistry* registry, Entity parent) {
    if (!ScopedLoadReport::current())
        return;
    const YAML::Node type = entityNode.IsMap() ? entityNode["type"] : YAML::Node();
    LoadIssue issue = makeIssue(LoadIssueKind::UnknownEntityType, LoadIssueFate::Dropped, registry, NULL_ENTITY, "", "type",
            "unknown entity type", type);
    std::string name = entityNode.IsMap() && entityNode["name"] && entityNode["name"].IsScalar() ? entityNode["name"].Scalar() : std::string();
    if (registry && parent != NULL_ENTITY)
        name = entityPath(registry, parent) + (name.empty() ? std::string() : "/" + name);
    issue.entityPath = name;
    issue.suggestion = nearestEnumName(issue.value, {"Entity"});
    recordLoadIssue(std::move(issue));
}

void StreamLoad::unknownOverrideComponent(const YAML::Node& node, const std::string& name) {
    if (!ScopedLoadReport::current())
        return;
    LoadIssue issue = makeIssue(LoadIssueKind::UnknownComponent, LoadIssueFate::Dropped, currentRegistry, currentEntity, name, "",
            "unknown component in a prefab override", node);
    issue.value.clear();
    issue.suggestion = nearest(name, StreamSchema::componentNames());
    recordLoadIssue(std::move(issue));
}

namespace {

    // A value the decoder replaced by its default: kept as a stand-in when it is a key of a
    // component's (or block's) top level, reported dropped otherwise
    void valueNotRead(const YAML::Node& value, LoadIssueKind kind, const std::string& message, const std::string& suggestion) {
        Frame* frame = currentFrame;
        if (!frame) {
            // outside any component or block (workspace state, an editor camera): nothing keeps it
            if (!ScopedLoadReport::current())
                return;
            LoadIssue issue = makeIssue(kind, LoadIssueFate::Dropped, currentRegistry, currentEntity, "", "", message, value);
            issue.suggestion = suggestion;
            recordLoadIssue(std::move(issue));
            return;
        }
        const std::string path = StreamLoad::locate(frame->node, value);
        const bool topLevel = !path.empty() && path.find('.') == std::string::npos && path.find('[') == std::string::npos;
        LoadIssue issue = makeIssue(kind, topLevel ? LoadIssueFate::Preserved : LoadIssueFate::Dropped, frame->registry, frame->entity,
                frame->component, path, message, value);
        issue.suggestion = suggestion;
        if (topLevel) {
            // recorded when the component (or block) has decoded, with its live value
            for (const auto& pending : frame->substitutions) {
                if (pending.first == path)
                    return;
            }
            frame->substitutions.emplace_back(path, std::move(issue));
            return;
        }
        recordLoadIssue(std::move(issue));
    }

}

void StreamLoad::invalidValue(const YAML::Node& value, const std::string& expected) {
    // null is "no value", which every decoder reads as its default
    if (!value || value.IsNull())
        return;
    valueNotRead(value, LoadIssueKind::InvalidValue, "invalid value, expected " + expected, std::string());
}

void StreamLoad::unreadableValue(const YAML::Node& value, const std::string& reason) {
    if (!value)
        return;
    valueNotRead(value, LoadIssueKind::InvalidValue, invalidMessage(reason), std::string());
}

void StreamLoad::fallbackValue(const YAML::Node& value, const std::string& message) {
    if (!value)
        return;
    Frame* frame = currentFrame;
    if (!frame) {
        if (!ScopedLoadReport::current())
            return;
        recordLoadIssue(makeIssue(LoadIssueKind::Fallback, LoadIssueFate::Info, currentRegistry, currentEntity, "", "", message, value));
        return;
    }
    recordLoadIssue(makeIssue(LoadIssueKind::Fallback, LoadIssueFate::Info, frame->registry, frame->entity, frame->component,
                              StreamLoad::locate(frame->node, value), message, value));
}

void StreamLoad::unknownEnumValue(const YAML::Node& value, const char* typeName, const std::vector<std::string>& names) {
    const bool scalar = value && value.IsScalar();
    const std::string text = scalar ? value.Scalar() : std::string();
    valueNotRead(value, scalar ? LoadIssueKind::UnknownEnumValue : LoadIssueKind::InvalidValue,
            scalar ? "unknown value" : std::string("not a ") + typeName + " name", nearestEnumName(text, names));
}

namespace {

    bool locateIn(const YAML::Node& node, const YAML::Node& target, std::string& path, int depth) {
        if (node.is(target))
            return true;
        if (depth > 64)
            return false;
        if (node.IsMap()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                std::string sub;
                if (locateIn(it->second, target, sub, depth + 1)) {
                    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string("?");
                    path = sub.empty() ? key : (sub[0] == '[' ? key + sub : key + "." + sub);
                    return true;
                }
            }
        } else if (node.IsSequence()) {
            for (size_t i = 0; i < node.size(); i++) {
                std::string sub;
                if (locateIn(node[i], target, sub, depth + 1)) {
                    const std::string index = "[" + std::to_string(i) + "]";
                    path = sub.empty() ? index : (sub[0] == '[' ? index + sub : index + "." + sub);
                    return true;
                }
            }
        }
        return false;
    }

}

std::string StreamLoad::locate(const YAML::Node& root, const YAML::Node& target) {
    std::string path;
    if (!root || !target || root.is(target))
        return path;
    locateIn(root, target, path, 0);
    return path;
}

std::string StreamLoad::entityPath(const EntityRegistry* registry, Entity entity) {
    std::vector<std::string> names;
    Entity current = entity;
    for (int depth = 0; current != NULL_ENTITY && depth < 256 && registry->isEntityCreated(current); depth++) {
        names.push_back(registry->getEntityName(current));
        const Signature signature = registry->getSignature(current);
        if (!signature.test(registry->getComponentId<Transform>()))
            break;
        current = registry->getComponent<Transform>(current).parent;
    }
    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        if (!path.empty())
            path += "/";
        path += *it;
    }
    return path;
}

// ---- encoding ----

void StreamSave::applyToComponents(const EntityRegistry* registry, Entity entity, YAML::Node& compNode, bool wholeEntity) {
    PreservedStore* store = findPreservedStore(registry);
    PreservedEntity* preserved = store ? store->find(entity) : nullptr;
    if (!preserved)
        return;

    for (PreservedFieldGroup& group : preserved->fieldGroups) {
        if (!compNode.IsMap() || !compNode[group.component])
            continue;
        YAML::Node component = compNode[group.component];
        if (!component.IsMap())
            continue;
        auto& fields = group.fields;
        for (auto it = fields.begin(); it != fields.end();) {
            PreservedField& field = it->second;
            if (field.substitution) {
                // the authored value stands while the live one is what the load left
                if (dumpOrEmpty(component[it->first]) != field.liveAtLoad) {
                    it = fields.erase(it);
                    continue;
                }
                component[it->first] = YAML::Clone(field.authored);
            } else if (!component[it->first]) {
                component[it->first] = YAML::Clone(field.authored);
            }
            ++it;
        }
    }

    auto& components = preserved->components;
    for (auto it = components.begin(); it != components.end();) {
        PreservedComponent& component = it->second;
        if (component.shadow) {
            if (compNode.IsMap() && compNode[it->first]) {
                if (dumpOrEmpty(compNode[it->first]) != component.liveAtLoad) {
                    it = components.erase(it);
                    continue;
                }
                compNode[it->first] = YAML::Clone(component.authored);
            }
        } else if (wholeEntity && (!compNode.IsDefined() || compNode.IsNull() || compNode.IsMap()) && !compNode[it->first]) {
            compNode[it->first] = YAML::Clone(component.authored);
        }
        ++it;
    }
    store->prune(entity);
}

void StreamSave::applyRootKeys(const EntityRegistry* registry, Entity entity, YAML::Node& node) {
    const PreservedStore* store = findPreservedStore(registry);
    const PreservedEntity* preserved = store ? store->find(entity) : nullptr;
    if (!preserved || !node.IsMap())
        return;
    for (const auto& [key, value] : preserved->rootKeys) {
        if (!node[key])
            node[key] = YAML::Clone(value);
    }
}

void StreamSave::applyBlock(const EntityRegistry* registry, const std::string& block, YAML::Node& node) {
    PreservedStore* store = findPreservedStore(registry);
    PreservedEntity* preserved = store ? store->find(NULL_ENTITY) : nullptr;
    PreservedFieldGroup* group = preserved ? preserved->group(block) : nullptr;
    if (!group || !node.IsMap())
        return;
    auto& fields = group->fields;
    for (auto it = fields.begin(); it != fields.end();) {
        PreservedField& field = it->second;
        if (field.substitution) {
            if (dumpOrEmpty(node[it->first]) != field.liveAtLoad) {
                it = fields.erase(it);
                continue;
            }
            node[it->first] = YAML::Clone(field.authored);
        } else if (!node[it->first]) {
            node[it->first] = YAML::Clone(field.authored);
        }
        ++it;
    }
    store->prune(NULL_ENTITY);
}
