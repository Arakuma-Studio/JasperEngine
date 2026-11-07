// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipRecordHooks.h"

#include "Catalog.h"
#include "animation/PropertyBindings.h"

#include <algorithm>
#include <cstdlib>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    // A listener is held by a shared_ptr: a call runs on a copy of the list, so a listener
    // removed (or one added) by another listener during the call neither moves nor frees the
    // function that is running; `removed` keeps a removed one from being called later in it.
    struct RecordEntry{
        ClipRecordHooks::Id id = 0;
        ClipRecordListener listener;
        bool removed = false;
    };

    struct ChannelEntry{
        ClipRecordHooks::Id id = 0;
        ChannelWriteListener listener;
        bool removed = false;
    };

    struct PropertyEntry{
        ClipRecordHooks::Id id = 0;
        PropertyWriteListener listener;
        bool removed = false;
    };

    struct ComponentEntry{
        ClipRecordHooks::Id id = 0;
        ComponentWriteListener listener;
        bool removed = false;
    };

    struct PreviewEntry{
        ClipRecordHooks::Id id = 0;
        ClipRecordHooks::PreviewQuery query;
        bool removed = false;
    };

    struct Registry{
        ClipRecordHooks::Id nextId = 1;
        std::vector<std::shared_ptr<PreviewEntry>> previews;
        std::vector<std::shared_ptr<RecordEntry>> record;
        std::vector<std::shared_ptr<ChannelEntry>> before;
        std::vector<std::shared_ptr<ChannelEntry>> after;
        std::vector<std::shared_ptr<PropertyEntry>> beforeProperty;
        std::vector<std::shared_ptr<PropertyEntry>> afterProperty;
        std::vector<std::shared_ptr<ComponentEntry>> beforeComponent;
        std::vector<std::shared_ptr<ComponentEntry>> afterComponent;
        uint64_t dragSerial = 0;
        std::string dragKey;
        // T3c: the open gesture of each RecordSource
        std::vector<std::pair<int, std::string>> gestures;
        std::string note;
    };

    // never destroyed: listeners of objects that live until static exit (the shared clip
    // controller) still find it when they remove themselves
    Registry& registry(){
        static Registry* instance = new Registry();
        return *instance;
    }

    template<typename Entry>
    bool removeFrom(std::vector<std::shared_ptr<Entry>>& list, ClipRecordHooks::Id id){
        for (auto it = list.begin(); it != list.end(); ++it){
            if ((*it)->id == id){
                (*it)->removed = true;
                list.erase(it);
                return true;
            }
        }
        return false;
    }

    void notify(const std::vector<std::shared_ptr<ChannelEntry>>& list, uint32_t sceneId, Entity entity, ClipChannel channel){
        if (list.empty()) return;
        const std::vector<std::shared_ptr<ChannelEntry>> copy = list;
        for (const std::shared_ptr<ChannelEntry>& entry : copy){
            if (!entry->removed && entry->listener) entry->listener(sceneId, entity, channel);
        }
    }
}

ClipRecordHooks::Id ClipRecordHooks::addRecordListener(ClipRecordListener listener){
    Registry& reg = registry();
    auto entry = std::make_shared<RecordEntry>();
    entry->id = reg.nextId++;
    entry->listener = std::move(listener);
    reg.record.push_back(entry);
    return entry->id;
}

void ClipRecordHooks::removeRecordListener(Id id){
    if (id == 0) return;
    removeFrom(registry().record, id);
}

bool ClipRecordHooks::hasRecordListeners(){
    return !registry().record.empty();
}

bool ClipRecordHooks::claims(uint32_t sceneId, Entity entity, ClipChannel channel){
    const std::vector<std::shared_ptr<RecordEntry>> copy = registry().record;
    for (const std::shared_ptr<RecordEntry>& entry : copy){
        if (!entry->removed && entry->listener.claims && entry->listener.claims(sceneId, entity, channel)) return true;
    }
    return false;
}

bool ClipRecordHooks::record(uint32_t sceneId, Entity entity, const ClipRecordValue& value, const std::string& mergeKey){
    const std::vector<std::shared_ptr<RecordEntry>> copy = registry().record;
    for (const std::shared_ptr<RecordEntry>& entry : copy){
        if (entry->removed) continue;
        if (value.channel == ClipChannel::Property){
            if (!entry->listener.claimsProperty || !entry->listener.claimsProperty(sceneId, entity, value.property)) continue;
        }else if (!entry->listener.claims || !entry->listener.claims(sceneId, entity, value.channel)){
            continue;
        }
        if (entry->listener.record) entry->listener.record(sceneId, entity, value, mergeKey);
        return true;
    }
    return false;
}

bool ClipRecordHooks::claimsProperty(uint32_t sceneId, Entity entity, const ClipPropertyKey& property){
    if (property.empty()) return false;
    const std::vector<std::shared_ptr<RecordEntry>> copy = registry().record;
    for (const std::shared_ptr<RecordEntry>& entry : copy){
        if (!entry->removed && entry->listener.claimsProperty && entry->listener.claimsProperty(sceneId, entity, property)) return true;
    }
    return false;
}

bool ClipRecordHooks::claimsAnyProperty(uint32_t sceneId, const std::vector<Entity>& entities, const ClipPropertyKey& property){
    if (registry().record.empty() || property.empty()) return false;
    for (Entity entity : entities){
        if (claimsProperty(sceneId, entity, property)) return true;
    }
    return false;
}

bool ClipRecordHooks::animatesProperty(uint32_t sceneId, Entity entity, const ClipPropertyKey& property){
    if (property.empty()) return false;
    const std::vector<std::shared_ptr<RecordEntry>> copy = registry().record;
    for (const std::shared_ptr<RecordEntry>& entry : copy){
        if (!entry->removed && entry->listener.animatesProperty && entry->listener.animatesProperty(sceneId, entity, property)) return true;
    }
    return false;
}

ClipRecordHooks::Id ClipRecordHooks::addBeforePropertyWrite(PropertyWriteListener listener){
    Registry& reg = registry();
    auto entry = std::make_shared<PropertyEntry>();
    entry->id = reg.nextId++;
    entry->listener = std::move(listener);
    reg.beforeProperty.push_back(entry);
    return entry->id;
}

ClipRecordHooks::Id ClipRecordHooks::addAfterPropertyWrite(PropertyWriteListener listener){
    Registry& reg = registry();
    auto entry = std::make_shared<PropertyEntry>();
    entry->id = reg.nextId++;
    entry->listener = std::move(listener);
    reg.afterProperty.push_back(entry);
    return entry->id;
}

namespace{
    void notifyProperty(const std::vector<std::shared_ptr<PropertyEntry>>& list, uint32_t sceneId, Entity entity, const ClipPropertyKey& property){
        if (list.empty() || property.empty()) return;
        const std::vector<std::shared_ptr<PropertyEntry>> copy = list;
        for (const std::shared_ptr<PropertyEntry>& entry : copy){
            if (!entry->removed && entry->listener) entry->listener(sceneId, entity, property);
        }
    }
}

void ClipRecordHooks::beforePropertyWrite(uint32_t sceneId, Entity entity, const ClipPropertyKey& property){
    notifyProperty(registry().beforeProperty, sceneId, entity, property);
}

void ClipRecordHooks::afterPropertyWrite(uint32_t sceneId, Entity entity, const ClipPropertyKey& property){
    notifyProperty(registry().afterProperty, sceneId, entity, property);
}

bool ClipRecordHooks::propertyOf(ComponentType type, const std::string& property, ClipPropertyKey& out){
    out = ClipPropertyKey();
    if (type == ComponentType::Transform || type == ComponentType::ScriptComponent || property.empty()) return false;
    const std::string component = Catalog::getComponentName(type, true);
    // the Animation Player's own checkbox is not animatable (spec non-goals)
    if (type == ComponentType::AnimationPlayerComponent) return false;
    anim::PropertyRef ref;
    if (!anim::parsePropertyRef(component, property, ref) || ref.swizzle >= 0) return false;
    out.component = component;
    out.property = property;
    return true;
}

ClipPropertyKey ClipRecordHooks::scriptPropertyOf(const std::string& className, const std::string& name){
    ClipPropertyKey key;
    if (className.empty() || name.empty()) return key;
    key.component = "script:" + className;
    key.property = name;
    return key;
}

ClipPropertyKey ClipRecordHooks::activeProperty(){
    ClipPropertyKey key;
    key.component = "gameobject";
    key.property = "active";
    return key;
}

std::string ClipRecordHooks::beginDrag(){
    Registry& reg = registry();
    reg.dragSerial++;
    reg.dragKey = "record#" + std::to_string(reg.dragSerial);
    return reg.dragKey;
}

void ClipRecordHooks::endDrag(){
    registry().dragKey.clear();
}

std::string ClipRecordHooks::currentMergeKey(){
    return registry().dragKey;
}

ClipRecordHooks::Id ClipRecordHooks::addBeforeChannelWrite(ChannelWriteListener listener){
    Registry& reg = registry();
    auto entry = std::make_shared<ChannelEntry>();
    entry->id = reg.nextId++;
    entry->listener = std::move(listener);
    reg.before.push_back(entry);
    return entry->id;
}

ClipRecordHooks::Id ClipRecordHooks::addAfterChannelWrite(ChannelWriteListener listener){
    Registry& reg = registry();
    auto entry = std::make_shared<ChannelEntry>();
    entry->id = reg.nextId++;
    entry->listener = std::move(listener);
    reg.after.push_back(entry);
    return entry->id;
}

void ClipRecordHooks::removeChannelWriteListener(Id id){
    if (id == 0) return;
    Registry& reg = registry();
    if (removeFrom(reg.before, id) || removeFrom(reg.after, id) || removeFrom(reg.beforeProperty, id) || removeFrom(reg.afterProperty, id)) return;
    if (removeFrom(reg.beforeComponent, id) || removeFrom(reg.afterComponent, id)) return;
    removeFrom(reg.previews, id);
}

ClipRecordHooks::Id ClipRecordHooks::addPreviewQuery(PreviewQuery query){
    Registry& reg = registry();
    auto entry = std::make_shared<PreviewEntry>();
    entry->id = reg.nextId++;
    entry->query = std::move(query);
    reg.previews.push_back(entry);
    return entry->id;
}

bool ClipRecordHooks::previews(uint32_t sceneId, Entity entity, ClipChannel channel){
    const std::vector<std::shared_ptr<PreviewEntry>> copy = registry().previews;
    for (const std::shared_ptr<PreviewEntry>& entry : copy){
        if (!entry->removed && entry->query && entry->query(sceneId, entity, channel)) return true;
    }
    return false;
}

void ClipRecordHooks::beforeChannelWrite(uint32_t sceneId, Entity entity, ClipChannel channel){
    notify(registry().before, sceneId, entity, channel);
}

void ClipRecordHooks::afterChannelWrite(uint32_t sceneId, Entity entity, ClipChannel channel){
    notify(registry().after, sceneId, entity, channel);
}

namespace{
    // the component has registry rows (its enabled checkbox included)
    bool hasPropertyRows(ComponentType type){
        if (type == ComponentType::Transform || type == ComponentType::AnimationPlayerComponent) return false;
        if (type == ComponentType::ScriptComponent) return true;
        const std::string component = Catalog::getComponentName(type, true);
        for (const anim::AnimatableProperty& row : anim::animatableProperties()){
            if (component == row.component) return true;
        }
        return false;
    }
}

bool ClipRecordHooks::componentWriteKey(ComponentType type, const std::string& property, ClipPropertyKey& out){
    out = ClipPropertyKey();
    if (type == ComponentType::ScriptComponent){
        // "scripts[N].<field>" (or "" / "scripts": every script), resolved by the listeners
        // against the GameObject's scripts
        if (!property.empty() && property.compare(0, 7, "scripts") != 0) return false;
        out.component = scriptRowComponent();
        out.property = property == "scripts" ? std::string() : property;
        return true;
    }
    if (property.empty()){
        if (!hasPropertyRows(type)) return false;
        out.component = Catalog::getComponentName(type, true);
        return true;
    }
    return propertyOf(type, property, out);
}

bool ClipRecordHooks::scriptRow(const std::string& property, size_t& index, std::string& field){
    if (property.compare(0, 8, "scripts[") != 0) return false;
    const size_t close = property.find("].", 8);
    if (close == std::string::npos || close == 8) return false;
    for (size_t i = 8; i < close; i++){
        if (property[i] < '0' || property[i] > '9') return false;
    }
    index = (size_t)std::strtoul(property.c_str() + 8, nullptr, 10);
    field = property.substr(close + 2);
    return !field.empty();
}

std::vector<ClipChannel> ClipRecordHooks::channelsOf(ComponentType type, const std::string& property){
    switch (type){
        case ComponentType::Transform:
            if (property.empty()) return {ClipChannel::Position, ClipChannel::Rotation, ClipChannel::Scale};
            if (property == "position") return {ClipChannel::Position};
            if (property == "rotation") return {ClipChannel::Rotation};
            if (property == "scale") return {ClipChannel::Scale};
            return {};
        case ComponentType::LatticeComponent:
            // the whole component also writes its enabled checkbox
            if (property.empty()) return {ClipChannel::Lattice, ClipChannel::Property};
            if (property == "offsets") return {ClipChannel::Lattice};
            break;
        case ComponentType::MeshComponent:
            // the whole component (its weights and every submesh row), or one weight
            // ("morphWeights[i]", T3c)
            if (property.empty()) return {ClipChannel::Morph, ClipChannel::Property};
            if (property.compare(0, 12, "morphWeights") == 0) return {ClipChannel::Morph};
            break;
        default:
            break;
    }
    ClipPropertyKey key;
    if (componentWriteKey(type, property, key)) return {ClipChannel::Property};
    return {};
}

namespace{
    void notifyComponent(const std::vector<std::shared_ptr<ComponentEntry>>& list, uint32_t sceneId, Entity entity,
                         ComponentType type, const std::string& property){
        if (list.empty()) return;
        const std::vector<std::shared_ptr<ComponentEntry>> copy = list;
        for (const std::shared_ptr<ComponentEntry>& entry : copy){
            if (!entry->removed && entry->listener) entry->listener(sceneId, entity, type, property);
        }
    }

    ClipRecordHooks::Id addComponentEntry(std::vector<std::shared_ptr<ComponentEntry>>& list, ComponentWriteListener listener){
        Registry& reg = registry();
        auto entry = std::make_shared<ComponentEntry>();
        entry->id = reg.nextId++;
        entry->listener = std::move(listener);
        list.push_back(entry);
        return entry->id;
    }
}

ClipRecordHooks::Id ClipRecordHooks::addBeforeComponentWrite(ComponentWriteListener listener){
    return addComponentEntry(registry().beforeComponent, std::move(listener));
}

ClipRecordHooks::Id ClipRecordHooks::addAfterComponentWrite(ComponentWriteListener listener){
    return addComponentEntry(registry().afterComponent, std::move(listener));
}

void ClipRecordHooks::beforeComponentWrite(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property){
    notifyComponent(registry().beforeComponent, sceneId, entity, type, property);
    for (ClipChannel channel : channelsOf(type, property)){
        if (channel != ClipChannel::Property){
            beforeChannelWrite(sceneId, entity, channel);
            continue;
        }
        ClipPropertyKey key;
        if (componentWriteKey(type, property, key)) beforePropertyWrite(sceneId, entity, key);
    }
}

void ClipRecordHooks::afterComponentWrite(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property){
    for (ClipChannel channel : channelsOf(type, property)){
        if (channel != ClipChannel::Property){
            afterChannelWrite(sceneId, entity, channel);
            continue;
        }
        ClipPropertyKey key;
        if (componentWriteKey(type, property, key)) afterPropertyWrite(sceneId, entity, key);
    }
    notifyComponent(registry().afterComponent, sceneId, entity, type, property);
}

// ---- record source helpers (T3c) ----

bool ClipRecordHooks::claimsAny(uint32_t sceneId, const std::vector<Entity>& entities, ClipChannel channel){
    if (registry().record.empty()) return false;
    for (Entity entity : entities){
        if (claims(sceneId, entity, channel)) return true;
    }
    return false;
}

ClipRecordValue ClipRecordHooks::transformValue(ClipChannel channel, const Vector3& position, const Quaternion& rotation, const Vector3& scale){
    ClipRecordValue out;
    out.channel = channel;
    switch (channel){
        case ClipChannel::Position:
            out.value = {position.x, position.y, position.z};
            break;
        case ClipChannel::Rotation:
            out.value.resize(4);
            quaternionToXYZW(rotation, out.value.data());
            break;
        case ClipChannel::Scale:
            out.value = {scale.x, scale.y, scale.z};
            break;
        default:
            break;
    }
    return out;
}

std::string ClipRecordHooks::gestureMergeKey(RecordSource source, bool* opened){
    if (opened) *opened = false;
    Registry& reg = registry();
    for (const auto& gesture : reg.gestures){
        if (gesture.first == (int)source) return gesture.second;
    }
    if (opened) *opened = true;
    reg.dragSerial++;
    const std::string key = "record#" + std::to_string(reg.dragSerial);
    reg.gestures.push_back({(int)source, key});
    return key;
}

void ClipRecordHooks::endGesture(RecordSource source){
    std::vector<std::pair<int, std::string>>& gestures = registry().gestures;
    gestures.erase(std::remove_if(gestures.begin(), gestures.end(), [source](const auto& g){ return g.first == (int)source; }), gestures.end());
}

bool ClipRecordHooks::hasGesture(RecordSource source){
    for (const auto& gesture : registry().gestures){
        if (gesture.first == (int)source) return true;
    }
    return false;
}

void ClipRecordHooks::endAllGestures(){
    registry().gestures.clear();
    registry().dragKey.clear();
}

bool ClipRecordHooks::animates(uint32_t sceneId, Entity entity, ClipChannel channel){
    const std::vector<std::shared_ptr<RecordEntry>> copy = registry().record;
    for (const std::shared_ptr<RecordEntry>& entry : copy){
        if (!entry->removed && entry->listener.animates && entry->listener.animates(sceneId, entity, channel)) return true;
    }
    return false;
}

void ClipRecordHooks::note(const std::string& message){
    registry().note = message;
}

std::string ClipRecordHooks::takeNote(){
    std::string out;
    std::swap(out, registry().note);
    return out;
}

// ---- explicit writes (FX) ----

namespace{
    ClipRecordHooks::ExplicitWrite& currentExplicitWrite(){
        static ClipRecordHooks::ExplicitWrite* write = new ClipRecordHooks::ExplicitWrite();
        return *write;
    }
}

bool ClipRecordHooks::ExplicitWrite::covers(size_t component) const{
    return all || std::find(components.begin(), components.end(), component) != components.end();
}

const ClipRecordHooks::ExplicitWrite& ClipRecordHooks::explicitWrite(){
    return currentExplicitWrite();
}

ClipRecordHooks::ExplicitWriteScope::ExplicitWriteScope(ExplicitWrite write): previous(currentExplicitWrite()){
    currentExplicitWrite() = std::move(write);
}

ClipRecordHooks::ExplicitWriteScope::~ExplicitWriteScope(){
    currentExplicitWrite() = std::move(previous);
}
