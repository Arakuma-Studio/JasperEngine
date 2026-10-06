// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ENTITYMANAGER_H
#define ENTITYMANAGER_H

#include <map>
#include <vector>
#include <string>
#include <limits>
#include "Entity.h"
#include "Signature.h"
#include "Log.h"

namespace jasperengine{

    // Unity's built-in tag, the one every GameObject starts with
    inline constexpr const char* DEFAULT_ENTITY_TAG = "Untagged";

    struct EntityMetadata{
        std::string name;
        Signature signature;
        // GameObject state: activeSelf, tag and layer (0..31)
        bool active = true;
        std::string tag = DEFAULT_ENTITY_TAG;
        int layer = 0;
    };

    class JASPERENGINE_API EntityManager {
    private:
        static constexpr Entity FIRST_SYSTEM_ENTITY = 1;
        static constexpr Entity LAST_SYSTEM_ENTITY  = 1000;
        static constexpr Entity FIRST_USER_ENTITY   = LAST_SYSTEM_ENTITY + 1;

        Entity lastUserEntity = LAST_SYSTEM_ENTITY; // next user entity starts at 1001
        std::map<Entity, EntityMetadata> metadata;
        // entities whose own active flag is off; zero lets hierarchy queries return early
        size_t inactiveCount = 0;

    public:

        static constexpr Entity firstSystemEntity() { return FIRST_SYSTEM_ENTITY; }
        static constexpr Entity lastSystemEntity()  { return LAST_SYSTEM_ENTITY; }
        static constexpr Entity firstUserEntity()   { return FIRST_USER_ENTITY; }

        bool recreateEntity(Entity entity) { // for internal editor use only
            if (!isCreated(entity)){
                metadata[entity];
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
            for (Entity e = FIRST_SYSTEM_ENTITY; e <= LAST_SYSTEM_ENTITY; ++e){
                if (metadata.count(e) == 0){
                    metadata[e];
                    return e;
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

                if (metadata.count(candidate) == 0){
                    break;
                }
            }

            lastUserEntity = candidate;
            metadata[candidate];
            return candidate;
        }

        bool isCreated(Entity entity) const {
            return metadata.count(entity) > 0;
        }

        void destroy(Entity entity) {
            auto it = metadata.find(entity);
            if (it == metadata.end()){
                return;
            }
            if (!it->second.active && inactiveCount > 0){
                inactiveCount--;
            }
            metadata.erase(it);
        }

        std::vector<Entity> getEntityList() const{
            std::vector<Entity> list;
            for (auto const& [key, val] : metadata){
                list.push_back(key);
            }
            return list;
        }

        bool getFirstEntity(Entity& entity) const {
            if (metadata.empty())
                return false;
            entity = metadata.begin()->first;
            return true;
        }

        void setSignature(Entity entity, Signature signature) {
            auto it = metadata.find(entity);
            if (it == metadata.end()){
                Log::error("Entity does not exist to set signature");
                it = metadata.try_emplace(entity).first;
            }
            it->second.signature = signature;
        }

        Signature getSignature(Entity entity) const{
            auto it = metadata.find(entity);
            if (it == metadata.end()){
                Log::error("Entity does not exist to get signature");
                return Signature();
            }
            return it->second.signature;
        }

        void setName(Entity entity, const std::string& name) {
            auto it = metadata.find(entity);
            if (it == metadata.end()){
                Log::error("Entity does not exist to set name");
                it = metadata.try_emplace(entity).first;
            }
            it->second.name = name;
        }

        std::string getName(Entity entity) const{
            auto it = metadata.find(entity);
            if (it == metadata.end()){
                Log::error("Entity does not exist to get name");
                return std::string();
            }
            return it->second.name;
        }

        // Scan metadata directly: building an id snapshot then looking up each
        // name/tag repeats tree searches and copies strings for every candidate.
        // Map iteration preserves the lowest-id first-match order.
        template<typename Predicate>
        Entity findByName(const std::string& name, Predicate matches) const {
            for (const auto& [entity, data] : metadata){
                if (data.name == name && matches(entity)){
                    return entity;
                }
            }
            return NULL_ENTITY;
        }

        Entity findByName(const std::string& name) const {
            return findByName(name, [](Entity){ return true; });
        }

        Entity findByTag(const std::string& tag) const {
            for (const auto& [entity, data] : metadata){
                if (data.tag == tag){
                    return entity;
                }
            }
            return NULL_ENTITY;
        }

        std::vector<Entity> findAllByTag(const std::string& tag) const {
            std::vector<Entity> found;
            for (const auto& [entity, data] : metadata){
                if (data.tag == tag){
                    found.push_back(entity);
                }
            }
            return found;
        }

        // Returns true when the flag changed
        bool setActive(Entity entity, bool active) {
            auto it = metadata.find(entity);
            if (it == metadata.end()){
                Log::error("Entity does not exist to set active");
                return false;
            }
            if (it->second.active == active){
                return false;
            }
            it->second.active = active;
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
            auto it = metadata.find(entity);
            return it == metadata.end() || it->second.active;
        }

        bool hasInactive() const {
            return inactiveCount > 0;
        }

        void setTag(Entity entity, const std::string& tag) {
            auto it = metadata.find(entity);
            if (it == metadata.end()){
                Log::error("Entity does not exist to set tag");
                return;
            }
            it->second.tag = tag.empty() ? std::string(DEFAULT_ENTITY_TAG) : tag;
        }

        std::string getTag(Entity entity) const {
            auto it = metadata.find(entity);
            if (it == metadata.end()){
                return std::string(DEFAULT_ENTITY_TAG);
            }
            return it->second.tag;
        }

        void setLayer(Entity entity, int layer) {
            auto it = metadata.find(entity);
            if (it == metadata.end()){
                Log::error("Entity does not exist to set layer");
                return;
            }
            if (layer < 0 || layer > 31){
                Log::warn("Layer %i is out of range (0 to 31), clamped", layer);
                layer = layer < 0 ? 0 : 31;
            }
            it->second.layer = layer;
        }

        int getLayer(Entity entity) const {
            auto it = metadata.find(entity);
            if (it == metadata.end()){
                return 0;
            }
            return it->second.layer;
        }
    };

}

#endif //ENTITYMANAGER_H
