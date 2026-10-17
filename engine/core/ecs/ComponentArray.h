// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPONENTARRAY_H
#define COMPONENTARRAY_H
    
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <numeric>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>
#include "Entity.h"
#include "Log.h"

namespace jasperengine {

    class JASPERENGINE_API ComponentArrayBase{
    public:
        virtual ~ComponentArrayBase() = default;
        virtual void entityDestroyed(Entity entity) = 0;
        // Several at once: the array is compacted once instead of shifted per entity
        virtual void entitiesDestroyed(const std::unordered_set<Entity>& entities) = 0;
    };


    template<typename T>
    class ComponentArray : public ComponentArrayBase{
        template<typename> friend class ComponentArray;

    private:
        std::vector<T> componentArray{};
        std::unordered_map<Entity, size_t> entityToIndexMap{};
        std::vector<Entity> indexToEntity{}; // parallel to componentArray

        // Grows by moving the components: a component type without a noexcept move
        // (most of them) would otherwise be copied whole on every reallocation
        void grow(){
            std::vector<T> bigger;
            bigger.reserve(std::max<size_t>(16, componentArray.capacity() * 2));
            for (T& component : componentArray){
                bigger.emplace_back(std::move(component));
            }
            componentArray.swap(bigger);
        }

        template<typename... Args>
        void emplace(Entity entity, Args&&... args) {
            if (entityToIndexMap.find(entity) != entityToIndexMap.end()) {
                Log::error("Component added to same entity more than once");
                return;
            }

            size_t newIndex = componentArray.size();
            if (componentArray.size() == componentArray.capacity()){
                // Built before growing: the arguments may name a component of this array
                T component(std::forward<Args>(args)...);
                grow();
                componentArray.emplace_back(std::move(component));
            }else{
                componentArray.emplace_back(std::forward<Args>(args)...);
            }
            entityToIndexMap[entity] = newIndex;
            indexToEntity.push_back(entity);
        }

        void reindex(size_t first, size_t last){
            for (size_t i = first; i < last; i++){
                entityToIndexMap[indexToEntity[i]] = i;
            }
        }

        void moveRange(size_t start, size_t length, size_t dst){
            size_t first, middle, last;
            if (start < dst){
                first  = start;
                middle = start + length;
                last   = dst + length;
            }else{
                first  = dst;
                middle = start;
                last   = start + length;
            }
            std::rotate(componentArray.begin() + first, componentArray.begin() + middle, componentArray.begin() + last);
            std::rotate(indexToEntity.begin() + first, indexToEntity.begin() + middle, indexToEntity.begin() + last);
            reindex(first, last);
        }

    public:
        void insert(Entity entity) {
            emplace(entity);
        }

        void insert(Entity entity, const T& component) {
            emplace(entity, component);
        }

        void insert(Entity entity, T&& component) {
            emplace(entity, std::move(component));
        }

        void remove(Entity entity) {
            auto it = entityToIndexMap.find(entity);
            if (it != entityToIndexMap.end()){

                // Remove component
                size_t indexOfRemovedEntity = it->second;
                entityToIndexMap.erase(it);
                componentArray.erase(componentArray.begin() + indexOfRemovedEntity);
                indexToEntity.erase(indexToEntity.begin() + indexOfRemovedEntity);

                // Update maps
                reindex(indexOfRemovedEntity, indexToEntity.size());

            } else {
                Log::error("Removing non-existent component");
            }
        }

        void moveEntityRangeToIndex(Entity start, Entity end, size_t newIndex){

            size_t startIndex = entityToIndexMap[start];
            size_t endIndex = entityToIndexMap[end];
            size_t length = endIndex - startIndex + 1;

            if ((newIndex + length) > componentArray.size()){
                Log::error("Cannot move entity range out of array");
                return;
            }

            moveRange(startIndex, length, newIndex);
        }

        void moveEntityToIndex(Entity entity, size_t newIndex){

            size_t oldIndex = entityToIndexMap[entity];

            moveRange(oldIndex, 1, newIndex);
        }

        template<typename C>
        void sortByComponent(ComponentArray<C>* otherComponent) {
            // Key: index in the other array; entities without it go last
            const size_t count = componentArray.size();
            const size_t missing = std::numeric_limits<size_t>::max();
            if (count < 2){
                return;
            }
            std::vector<size_t> keys(count);
            for (size_t i = 0; i < count; i++){
                auto it = otherComponent->entityToIndexMap.find(indexToEntity[i]);
                keys[i] = it != otherComponent->entityToIndexMap.end() ? it->second : missing;
            }

            sortByKeys(keys);
        }

        // In the order the entities have in `order` (those not in it go last, in their order).
        // For components without a Transform whose order is a list's, e.g. the editor's
        // scene entity list for skyboxes (the first one is the scene's sky).
        void sortByEntityOrder(const std::vector<Entity>& order) {
            const size_t count = componentArray.size();
            if (count < 2){
                return;
            }
            std::vector<size_t> keys(count);
            for (size_t i = 0; i < count; i++){
                auto it = std::find(order.begin(), order.end(), indexToEntity[i]);
                keys[i] = (it != order.end()) ? (size_t)(it - order.begin()) : order.size() + i;
            }
            sortByKeys(keys);
        }

    private:
        // reorders the components by ascending key (one key per component, in array order)
        void sortByKeys(const std::vector<size_t>& keys) {
            const size_t count = componentArray.size();
            // Usually already ordered (a block appended at the end). Check the
            // keys directly before allocating and filling the permutation.
            if (std::is_sorted(keys.begin(), keys.end())){
                return;
            }

            // Create a vector of indices [0, 1, ..., N-1]
            std::vector<size_t> indices(count);
            std::iota(indices.begin(), indices.end(), 0);

            auto less = [&keys](size_t a, size_t b) {
                return keys[a] < keys[b];
            };

            // Sort indices based on the key in otherComponent
            std::sort(indices.begin(), indices.end(), less);

            // Only the span that changed is moved, component by component
            size_t first = 0;
            while (first < count && indices[first] == first) first++;
            size_t last = count;
            while (last > first && indices[last - 1] == last - 1) last--;

            std::vector<T> moved;
            moved.reserve(last - first);
            std::vector<Entity> movedEntities;
            movedEntities.reserve(last - first);
            for (size_t newIndex = first; newIndex < last; ++newIndex) {
                moved.emplace_back(std::move(componentArray[indices[newIndex]]));
                movedEntities.push_back(indexToEntity[indices[newIndex]]);
            }
            for (size_t newIndex = first; newIndex < last; ++newIndex) {
                componentArray[newIndex] = std::move(moved[newIndex - first]);
                indexToEntity[newIndex] = movedEntities[newIndex - first];
            }
            reindex(first, last);
        }

    public:
        bool hasEntity(Entity entity) {
            return entityToIndexMap.find(entity) != entityToIndexMap.end();
        }

        T* findComponent(Entity entity) {
            auto it = entityToIndexMap.find(entity);

            if (it == entityToIndexMap.end()) {
                 return NULL;
            }

            return &componentArray[it->second];
        }

        T& getComponent(Entity entity) {
            try{

                return componentArray[entityToIndexMap.at(entity)];

            }catch (const std::out_of_range& e){
                Log::error("Retrieving non-existent component: %s", e.what());
                throw;
            }
        }

        T* findComponentFromIndex(size_t index) {
            if (index >= componentArray.size()){
                return NULL;
            }

            return &componentArray[index];
        }

        T& getComponentFromIndex(size_t index) {
            try{

                return componentArray.at(index);

            }catch (const std::out_of_range& e){
                Log::error("Retrieving non-existent component: %s", e.what());
                throw;
            }
        }

        size_t getIndex(Entity entity){
            try{

                return entityToIndexMap.at(entity);

            }catch (const std::out_of_range& e){
                Log::error("Retrieving non-existent component: %s", e.what());
                throw;
            }
        }

        Entity getEntity(size_t index){
            if (index >= indexToEntity.size()){
                Log::error("Entity not found");
                return NULL_ENTITY;
            }

            return indexToEntity[index];
        }

        size_t size(){
            return componentArray.size();
        }

        void entityDestroyed(Entity entity) override {
            if (entityToIndexMap.find(entity) != entityToIndexMap.end()) {
                remove(entity);
            }
        }

        void entitiesDestroyed(const std::unordered_set<Entity>& entities) override {
            // from the first one this array holds; the order of the others is kept
            size_t first = componentArray.size();
            for (Entity entity : entities) {
                auto it = entityToIndexMap.find(entity);
                if (it != entityToIndexMap.end() && it->second < first) {
                    first = it->second;
                }
            }
            if (first == componentArray.size()) {
                return;
            }

            size_t write = first;
            for (size_t read = first; read < componentArray.size(); read++) {
                const Entity entity = indexToEntity[read];
                if (entities.count(entity)) {
                    entityToIndexMap.erase(entity);
                    continue;
                }
                if (write != read) {
                    componentArray[write] = std::move(componentArray[read]);
                    indexToEntity[write] = entity;
                }
                write++;
            }
            componentArray.erase(componentArray.begin() + write, componentArray.end());
            indexToEntity.resize(write);
            reindex(first, write);
        }
    };

}

#endif //COMPONENTARRAY_H
