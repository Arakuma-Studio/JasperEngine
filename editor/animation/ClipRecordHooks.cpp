// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipRecordHooks.h"

#include "Catalog.h"

using namespace jasperengine;
using namespace jasperengine::editor;

// TODO(T3a): the listener registries. The stubs keep no listener: nothing is claimed or recorded
// and the channel write notifications reach nobody.

ClipRecordHooks::Id ClipRecordHooks::addRecordListener(ClipRecordListener listener){
    (void)listener;
    return 0;
}

void ClipRecordHooks::removeRecordListener(Id id){
    (void)id;
}

bool ClipRecordHooks::claims(uint32_t sceneId, Entity entity, ClipChannel channel){
    (void)sceneId;
    (void)entity;
    (void)channel;
    return false;
}

bool ClipRecordHooks::record(uint32_t sceneId, Entity entity, const ClipRecordValue& value, const std::string& mergeKey){
    (void)sceneId;
    (void)entity;
    (void)value;
    (void)mergeKey;
    return false;
}

std::string ClipRecordHooks::beginDrag(){
    return "";
}

void ClipRecordHooks::endDrag(){
}

std::string ClipRecordHooks::currentMergeKey(){
    return "";
}

ClipRecordHooks::Id ClipRecordHooks::addBeforeChannelWrite(ChannelWriteListener listener){
    (void)listener;
    return 0;
}

ClipRecordHooks::Id ClipRecordHooks::addAfterChannelWrite(ChannelWriteListener listener){
    (void)listener;
    return 0;
}

void ClipRecordHooks::removeChannelWriteListener(Id id){
    (void)id;
}

void ClipRecordHooks::beforeChannelWrite(uint32_t sceneId, Entity entity, ClipChannel channel){
    (void)sceneId;
    (void)entity;
    (void)channel;
}

void ClipRecordHooks::afterChannelWrite(uint32_t sceneId, Entity entity, ClipChannel channel){
    (void)sceneId;
    (void)entity;
    (void)channel;
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
            if (property.empty()) return {ClipChannel::Morph};
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
