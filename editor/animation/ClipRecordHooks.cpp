// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipRecordHooks.h"

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
