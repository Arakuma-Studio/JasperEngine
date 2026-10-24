// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ENTITYMANAGER_H
#define ENTITYMANAGER_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "Entity.h"
#include "Signature.h"
#include "PagedStorage.h"
#include "Log.h"

namespace jasperengine{

    // Unity's built-in tag, the one every GameObject starts with
    inline constexpr const char* DEFAULT_ENTITY_TAG = "Untagged";

    // The fields the getters read on hot paths (signature, layer, active) come first, in the
    // record's first 40 bytes; the name is read rarely
    struct EntityMetadata{
        Signature signature;
        // creation counter of the registry when the entity was made (EntityRef)
        uint32_t serial = 0;
        // GameObject state: tag (interned, 0 is "Untagged"), layer (0..31) and activeSelf
        uint16_t tag = 0;
        int8_t layer = 0;
        bool active = true;
        std::string name;
    };

    // Entity ids and their metadata. Ids are kept in a paged id -> slot table (ascending
    // iteration, O(1) lookup) over a dense slot array with a free list; system ids (1..1000)
    // come from an occupancy mask, user ids (1001..) count up and are never reused.
    class JASPERENGINE_API EntityManager {
    private:
        static constexpr Entity FIRST_SYSTEM_ENTITY = 1;
        static constexpr Entity LAST_SYSTEM_ENTITY  = 1000;
        static constexpr Entity FIRST_USER_ENTITY   = LAST_SYSTEM_ENTITY + 1;
        static constexpr size_t SYSTEM_MASK_WORDS   = (LAST_SYSTEM_ENTITY + 64) / 64; // 16

        Entity lastUserEntity = LAST_SYSTEM_ENTITY; // next user entity starts at 1001

        ecs::SparseIndex idToSlot;
        ecs::PagedArray<EntityMetadata> records;
        size_t recordCount = 0; // constructed records, free ones included
        std::vector<uint32_t> freeSlots;
        size_t liveCount = 0;
        size_t systemCount = 0;

        // bit e set: system id e is taken (bit 0 and the bits past 1000 always are)
        uint64_t systemUsed[SYSTEM_MASK_WORDS];

        uint32_t nextSerial = 0;

        // entities whose own active flag is off; zero lets hierarchy queries return early
        size_t inactiveCount = 0;

        // interned tags: id -> name, name -> id ("Untagged" is 0)
        std::vector<std::string> tagNames{DEFAULT_ENTITY_TAG};
        std::unordered_map<std::string, uint16_t> tagIds{{DEFAULT_ENTITY_TAG, 0}};

        void resetSystemMask(){
            for (size_t i = 0; i < SYSTEM_MASK_WORDS; i++){
                systemUsed[i] = 0;
            }
            systemUsed[0] |= 1u; // id 0 is NULL_ENTITY
            for (size_t e = LAST_SYSTEM_ENTITY + 1; e < SYSTEM_MASK_WORDS * 64; e++){
                systemUsed[e >> 6] |= uint64_t(1) << (e & 63);
            }
        }

        static bool isSystemId(Entity entity){
            return entity >= FIRST_SYSTEM_ENTITY && entity <= LAST_SYSTEM_ENTITY;
        }

        JE_ECS_INLINE const EntityMetadata* find(Entity entity) const {
            const uint32_t slot = idToSlot.find(entity);
            return slot == ecs::SparseIndex::kNone ? nullptr : records.slot(slot);
        }

        JE_ECS_INLINE EntityMetadata* find(Entity entity){
            const uint32_t slot = idToSlot.find(entity);
            return slot == ecs::SparseIndex::kNone ? nullptr : records.slot(slot);
        }

        EntityMetadata& add(Entity entity){
            uint32_t slot;
            if (!freeSlots.empty()){
                slot = freeSlots.back();
                freeSlots.pop_back();
            }else{
                records.reserve(recordCount + 1);
                ::new (static_cast<void*>(records.slot(recordCount))) EntityMetadata();
                slot = (uint32_t)recordCount++;
            }
            idToSlot.insert(entity, slot);
            if (isSystemId(entity)){
                systemUsed[entity >> 6] |= uint64_t(1) << (entity & 63);
                systemCount++;
            }
            liveCount++;

            EntityMetadata& data = records[slot];
            if (++nextSerial == 0){
                nextSerial = 1; // 0 is the serial of no entity
            }
            data.serial = nextSerial;
            return data;
        }

        void releaseRecords(){
            for (size_t i = 0; i < recordCount; i++){
                std::destroy_at(records.slot(i));
            }
            recordCount = 0;
            freeSlots.clear();
            freeSlots.shrink_to_fit();
            records.shrinkTo(0);
        }

        uint16_t internTag(const std::string& tag){
            auto it = tagIds.find(tag);
            if (it != tagIds.end()){
                return it->second;
            }
            if (tagNames.size() > std::numeric_limits<uint16_t>::max()){
                Log::error("Too many distinct tags, '%s' is stored as %s", tag.c_str(), DEFAULT_ENTITY_TAG);
                return 0;
            }
            const uint16_t id = (uint16_t)tagNames.size();
            tagNames.push_back(tag);
            tagIds.emplace(tag, id);
            return id;
        }

        bool findTag(const std::string& tag, uint16_t& id) const {
            auto it = tagIds.find(tag);
            if (it == tagIds.end()){
                return false;
            }
            id = it->second;
            return true;
        }

    public:

        static constexpr Entity firstSystemEntity() { return FIRST_SYSTEM_ENTITY; }
        static constexpr Entity lastSystemEntity()  { return LAST_SYSTEM_ENTITY; }
        static constexpr Entity firstUserEntity()   { return FIRST_USER_ENTITY; }

        EntityManager(){
            resetSystemMask();
        }

        EntityManager(const EntityManager&) = delete;
        EntityManager& operator=(const EntityManager&) = delete;

        ~EntityManager(){
            releaseRecords();
        }

        bool recreateEntity(Entity entity) { // for internal editor use only
            if (entity != NULL_ENTITY && !isCreated(entity)){
                add(entity);
                return true;
            }
            return false;
        }

        void setLastUserEntity(Entity entity){
            if (entity < LAST_SYSTEM_ENTITY) {
                this->lastUserEntity = LAST_SYSTEM_ENTITY;
            } else {
                this->lastUserEntity = entity;
            }
        }

        Entity getLastUserEntity() const{
            return this->lastUserEntity;
        }

        // System entity in range [1,1000], first free slot
        Entity createSystemEntity() {
            for (size_t word = 0; word < SYSTEM_MASK_WORDS; word++){
                const uint64_t free = ~systemUsed[word];
                if (free){
                    const Entity entity = (Entity)(word * 64 + ecs::countTrailingZeros(free));
                    add(entity);
                    return entity;
                }
            }
            Log::error("No free system entity slot available in range [1..1000]");
            return NULL_ENTITY;
        }

        // User entity in range [1001..max], monotonically increasing, skipping used ids
        Entity createUserEntity() {
            Entity candidate = lastUserEntity;
            while (true){
                if (candidate == std::numeric_limits<Entity>::max()){
                    Log::error("User entity counter overflow");
                    return NULL_ENTITY;
                }
                candidate++;

                if (!idToSlot.contains(candidate)){
                    break;
                }
            }

            lastUserEntity = candidate;
            add(candidate);
            return candidate;
        }

        bool isCreated(Entity entity) const {
            return idToSlot.contains(entity);
        }

        void destroy(Entity entity) {
            const uint32_t slot = idToSlot.find(entity);
            if (slot == ecs::SparseIndex::kNone){
                return;
            }
            EntityMetadata& data = records[slot];
            if (!data.active && inactiveCount > 0){
                inactiveCount--;
            }
            idToSlot.erase(entity);
            if (isSystemId(entity)){
                systemUsed[entity >> 6] &= ~(uint64_t(1) << (entity & 63));
                systemCount--;
            }
            liveCount--;

            if (liveCount == 0){
                releaseRecords();
                return;
            }
            std::string().swap(data.name);
            data.signature.reset();
            data.serial = 0;
            data.tag = 0;
            data.layer = 0;
            data.active = true;
            if (freeSlots.size() == freeSlots.capacity()){
                // a mass destroy grows it in two or three steps, not a dozen doublings
                freeSlots.reserve(std::max<size_t>(std::max<size_t>(freeSlots.capacity() * 2, 64), (liveCount + freeSlots.size()) / 4));
            }
            freeSlots.push_back(slot);
        }

        // live entities, and the system ones among them
        size_t count() const { return liveCount; }
        size_t countSystem() const { return systemCount; }

        std::vector<Entity> getEntityList() const{
            std::vector<Entity> list;
            list.reserve(liveCount);
            idToSlot.forEach([&list](Entity entity, uint32_t){
                list.push_back(entity);
                return true;
            });
            return list;
        }

        // fn(Entity) in ascending id order, without building a list
        template<typename F>
        void forEachEntity(F&& fn) const {
            idToSlot.forEach([&fn](Entity entity, uint32_t){
                fn(entity);
                return true;
            });
        }

        bool getFirstEntity(Entity& entity) const {
            return idToSlot.first(entity);
        }

        // creation serial (EntityRef); 0 when the entity does not exist
        uint32_t getSerial(Entity entity) const {
            const EntityMetadata* data = find(entity);
            return data ? data->serial : 0;
        }

        void setSignature(Entity entity, Signature signature) {
            EntityMetadata* data = find(entity);
            if (!data){
                Log::error("Entity does not exist to set signature");
                data = &add(entity);
            }
            data->signature = signature;
        }

        JE_ECS_INLINE Signature getSignature(Entity entity) const{
            const EntityMetadata* data = find(entity);
            if (!data) [[unlikely]] {
                Log::error("Entity does not exist to get signature");
                return Signature();
            }
            return data->signature;
        }

        // Sets or clears one bit; false when the entity does not exist
        bool setSignatureBit(Entity entity, size_t bit, bool value){
            EntityMetadata* data = find(entity);
            if (!data){
                return false;
            }
            data->signature.set(bit, value);
            return true;
        }

        void setName(Entity entity, const std::string& name) {
            EntityMetadata* data = find(entity);
            if (!data){
                Log::error("Entity does not exist to set name");
                data = &add(entity);
            }
            data->name = name;
        }

        std::string getName(Entity entity) const{
            const EntityMetadata* data = find(entity);
            if (!data){
                Log::error("Entity does not exist to get name");
                return std::string();
            }
            return data->name;
        }

        // Ascending id order: the lowest-id match is the first
        template<typename Predicate>
        Entity findByName(const std::string& name, Predicate matches) const {
            Entity found = NULL_ENTITY;
            idToSlot.forEach([&](Entity entity, uint32_t slot){
                if (records[slot].name == name && matches(entity)){
                    found = entity;
                    return false;
                }
                return true;
            });
            return found;
        }

        Entity findByName(const std::string& name) const {
            return findByName(name, [](Entity){ return true; });
        }

        Entity findByTag(const std::string& tag) const {
            uint16_t id;
            if (!findTag(tag, id)){
                return NULL_ENTITY;
            }
            Entity found = NULL_ENTITY;
            idToSlot.forEach([&](Entity entity, uint32_t slot){
                if (records[slot].tag == id){
                    found = entity;
                    return false;
                }
                return true;
            });
            return found;
        }

        std::vector<Entity> findAllByTag(const std::string& tag) const {
            std::vector<Entity> found;
            uint16_t id;
            if (!findTag(tag, id)){
                return found;
            }
            idToSlot.forEach([&](Entity entity, uint32_t slot){
                if (records[slot].tag == id){
                    found.push_back(entity);
                }
                return true;
            });
            return found;
        }

        // Returns true when the flag changed
        bool setActive(Entity entity, bool active) {
            EntityMetadata* data = find(entity);
            if (!data){
                Log::error("Entity does not exist to set active");
                return false;
            }
            if (data->active == active){
                return false;
            }
            data->active = active;
            if (active){
                if (inactiveCount > 0) inactiveCount--;
            }else{
                inactiveCount++;
            }
            return true;
        }

        bool isActive(Entity entity) const {
            if (inactiveCount == 0){
                return true;
            }
            const EntityMetadata* data = find(entity);
            return !data || data->active;
        }

        bool hasInactive() const {
            return inactiveCount > 0;
        }

        void setTag(Entity entity, const std::string& tag) {
            EntityMetadata* data = find(entity);
            if (!data){
                Log::error("Entity does not exist to set tag");
                return;
            }
            data->tag = tag.empty() ? 0 : internTag(tag);
        }

        std::string getTag(Entity entity) const {
            const EntityMetadata* data = find(entity);
            if (!data){
                return std::string(DEFAULT_ENTITY_TAG);
            }
            return tagNames[data->tag];
        }

        void setLayer(Entity entity, int layer) {
            EntityMetadata* data = find(entity);
            if (!data){
                Log::error("Entity does not exist to set layer");
                return;
            }
            if (layer < 0 || layer > 31){
                Log::warn("Layer %i is out of range (0 to 31), clamped", layer);
                layer = layer < 0 ? 0 : 31;
            }
            data->layer = (int8_t)layer;
        }

        JE_ECS_INLINE int getLayer(Entity entity) const {
            const EntityMetadata* data = find(entity);
            if (!data){
                return 0;
            }
            return data->layer;
        }

        // Memory of the metadata, the strings' heap buffers left out
        size_t bytes() const {
            return idToSlot.bytes() + records.bytes() + freeSlots.capacity() * sizeof(uint32_t);
        }
    };

}

#endif //ENTITYMANAGER_H
