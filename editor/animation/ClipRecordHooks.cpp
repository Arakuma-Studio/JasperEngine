// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipRecordHooks.h"

#include "Catalog.h"

#include <algorithm>
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

    struct Registry{
        ClipRecordHooks::Id nextId = 1;
        std::vector<std::shared_ptr<RecordEntry>> record;
        std::vector<std::shared_ptr<ChannelEntry>> before;
        std::vector<std::shared_ptr<ChannelEntry>> after;
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
        if (entry->removed || !entry->listener.claims || !entry->listener.claims(sceneId, entity, value.channel)) continue;
        if (entry->listener.record) entry->listener.record(sceneId, entity, value, mergeKey);
        return true;
    }
    return false;
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
    if (!removeFrom(reg.before, id)) removeFrom(reg.after, id);
}

void ClipRecordHooks::beforeChannelWrite(uint32_t sceneId, Entity entity, ClipChannel channel){
    notify(registry().before, sceneId, entity, channel);
}

void ClipRecordHooks::afterChannelWrite(uint32_t sceneId, Entity entity, ClipChannel channel){
    notify(registry().after, sceneId, entity, channel);
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
            if (property.empty() || property == "offsets") return {ClipChannel::Lattice};
            return {};
        case ComponentType::MeshComponent:
            // the whole component, or one weight ("morphWeights[i]", T3c)
            if (property.empty() || property.compare(0, 12, "morphWeights") == 0) return {ClipChannel::Morph};
            return {};
        default:
            return {};
    }
}

void ClipRecordHooks::beforeComponentWrite(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property){
    for (ClipChannel channel : channelsOf(type, property)) beforeChannelWrite(sceneId, entity, channel);
}

void ClipRecordHooks::afterComponentWrite(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property){
    for (ClipChannel channel : channelsOf(type, property)) afterChannelWrite(sceneId, entity, channel);
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
