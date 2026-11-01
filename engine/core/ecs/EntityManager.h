// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ENTITYMANAGER_H
#define ENTITYMANAGER_H

#include <bitset>
#include <map>
#include <memory>
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

        // Which ids are created and their signatures, by id in pages of 256: isCreated and
        // getSignature run per entity in the systems' loops (the Canvas Group pass, rendering,
        // physics), where a search of `metadata` cost a dozen dependent cache misses an entity;
        // here they are one array read, and entities created together share a page. A mirror
        // of `metadata` (every change of it goes through track/untrack/setSignature); ids from
        // SIGNATURE_PAGED_LIMIT up (never made by createUserEntity in practice) use `metadata`
        // only, so a stray huge id cannot grow the page table. A page goes with its last entity.
        static constexpr unsigned SIGNATURE_PAGE_BITS = 8;
        static constexpr Entity SIGNATURE_PAGE_SIZE = Entity(1) << SIGNATURE_PAGE_BITS;
        static constexpr Entity SIGNATURE_PAGED_LIMIT = Entity(1) << 28;
        struct SignaturePage{
            Signature signatures[SIGNATURE_PAGE_SIZE];
            std::bitset<SIGNATURE_PAGE_SIZE> created;
            size_t count = 0;
        };
        std::vector<std::unique_ptr<SignaturePage>> signaturePages;

        static bool isPaged(Entity entity){
            return entity < SIGNATURE_PAGED_LIMIT;
        }

        const SignaturePage* findPage(Entity entity) const{
            const size_t page = entity >> SIGNATURE_PAGE_BITS;
            return page < signaturePages.size() ? signaturePages[page].get() : nullptr;
        }

        // `entity` was added to `metadata` (with an empty signature)
        void track(Entity entity){
            if (!isPaged(entity)){
                return;
            }
            const size_t index = entity >> SIGNATURE_PAGE_BITS;
            if (index >= signaturePages.size()){
                signaturePages.resize(index + 1);
            }
            if (!signaturePages[index]){
                signaturePages[index] = std::make_unique<SignaturePage>();
            }
            SignaturePage& page = *signaturePages[index];
            const Entity slot = entity & (SIGNATURE_PAGE_SIZE - 1);
            if (!page.created.test(slot)){
                page.created.set(slot);
                page.signatures[slot] = Signature();
                page.count++;
            }
        }

        // `entity` was erased from `metadata`
        void untrack(Entity entity){
            if (!isPaged(entity)){
                return;
            }
            const size_t index = entity >> SIGNATURE_PAGE_BITS;
            if (index >= signaturePages.size() || !signaturePages[index]){
                return;
            }
            SignaturePage& page = *signaturePages[index];
            const Entity slot = entity & (SIGNATURE_PAGE_SIZE - 1);
            if (!page.created.test(slot)){
                return;
            }
            page.created.reset(slot);
            page.signatures[slot] = Signature();
            if (--page.count == 0){
                signaturePages[index].reset();
                while (!signaturePages.empty() && !signaturePages.back()){
                    signaturePages.pop_back();
                }
            }
        }
        // entities whose own active flag is off; zero lets hierarchy queries return early
        size_t inactiveCount = 0;

    public:

        static constexpr Entity firstSystemEntity() { return FIRST_SYSTEM_ENTITY; }
        static constexpr Entity lastSystemEntity()  { return LAST_SYSTEM_ENTITY; }
        static constexpr Entity firstUserEntity()   { return FIRST_USER_ENTITY; }

        bool recreateEntity(Entity entity) { // for internal editor use only
            if (!isCreated(entity)){
                metadata[entity];
                track(entity);
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
                if (!isCreated(e)){
                    metadata[e];
                    track(e);
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

                if (!isCreated(candidate)){
                    break;
                }
            }

            lastUserEntity = candidate;
            metadata[candidate];
            track(candidate);
            return candidate;
        }

        bool isCreated(Entity entity) const {
            if (isPaged(entity)){
                const SignaturePage* page = findPage(entity);
                return page && page->created.test(entity & (SIGNATURE_PAGE_SIZE - 1));
            }
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
            untrack(entity);
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
                track(entity);
            }
            it->second.signature = signature;
            if (isPaged(entity)){
                signaturePages[entity >> SIGNATURE_PAGE_BITS]->signatures[entity & (SIGNATURE_PAGE_SIZE - 1)] = signature;
            }
        }

        Signature getSignature(Entity entity) const{
            if (isPaged(entity)){
                const SignaturePage* page = findPage(entity);
                const Entity slot = entity & (SIGNATURE_PAGE_SIZE - 1);
                if (page && page->created.test(slot)){
                    return page->signatures[slot];
                }
                Log::error("Entity does not exist to get signature");
                return Signature();
            }
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
                track(entity);
            }
            it->second.name = name;
        }

        // the name without a copy (an empty one for an unknown entity)
        const std::string& getNameRef(Entity entity) const{
            static const std::string none;
            auto it = metadata.find(entity);
            return it == metadata.end() ? none : it->second.name;
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
