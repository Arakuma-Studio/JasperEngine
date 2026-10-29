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
#include <mutex>
#include <unordered_set>
#include <utility>
#include "Entity.h"
#include "Log.h"

namespace jasperengine {

    class JASPERENGINE_API ComponentArrayBase{
    public:
        virtual ~ComponentArrayBase() = default;
        virtual void entityDestroyed(Entity entity) = 0;
        virtual void entitiesDestroyed(const std::unordered_set<Entity>& entities) = 0;
        // closes the holes removals left (see ComponentArray)
        virtual void compact() = 0;
    };


    // The components of one type, packed in an order other code relies on (the Transforms
    // in hierarchy order: a parent's branch follows it; the meshes, UI, ... in their
    // Transforms' order), with the entity of each and the index of each entity's.
    //
    // A removal from the middle leaves a hole (the slot's component replaced by a default
    // one, its entity NULL_ENTITY) instead of shifting everything after it, so a run of
    // removals (destroying entities one by one, a scene's teardown) costs O(1) each instead
    // of O(size) each. The holes are invisible: size(), getIndex(), getComponentFromIndex(),
    // getEntity() and the rest answer as if every removal had shifted the array (through a
    // rank tree over the slots while there are holes, O(log size) an access), and none of
    // them moves a component: a reference or pointer taken by entity stays valid through
    // any read. The holes are closed, in order, by compact() (the scene does it at fixed
    // points: the start of its update and draw, after the frame's deferred destroys, between
    // editor commands and MCP calls, before a thread reads it), by the moves and sorts that
    // really reorder (they moved components before too), and by an insert once half the
    // slots are holes (an insert could always move them all, growing the array). Removing
    // the last slot pops it, with the holes before it.
    //
    // Reads from several threads at once are safe as before (the rank tree is built under a
    // lock); a thread reading while another removes or compacts is not (it never was).


    template<typename T>
    class ComponentArray : public ComponentArrayBase{
        template<typename> friend class ComponentArray;

    private:
        std::vector<T> componentArray{};
        std::unordered_map<Entity, size_t> entityToIndexMap{}; // slot of each entity's
        std::vector<Entity> indexToEntity{}; // parallel to componentArray, NULL_ENTITY: a hole
        size_t holes = 0;
        size_t firstHole = 0; // with holes: none before it
        // For a hole: a slot after it, no further than the first live one after it (made
        // shorter by each search, so a search over a run of holes costs O(1) amortized)
        std::vector<size_t> holeNext{};
        // While there are holes and something read by index: a Fenwick tree counting the live
        // slots (1-based), kept by removals and inserts once built
        std::vector<size_t> rankTree{};
        bool rankTreeBuilt = false;
        std::recursive_mutex rankMutex; // the rank tree and the skip links, for readers on several threads

        bool isHole(size_t slot) const{
            return indexToEntity[slot] == NULL_ENTITY;
        }

        // the first live slot at or after `slot` (componentArray.size() when none)
        size_t liveSlotFrom(size_t slot){
            const size_t count = indexToEntity.size();
            size_t live = slot;
            while (live < count && isHole(live)){
                live = holeNext[live];
            }
            live = std::min(live, count);
            while (slot < live && isHole(slot)){
                const size_t next = holeNext[slot];
                holeNext[slot] = live;
                slot = next;
            }
            return live;
        }

        // ---- rank tree

        static size_t lowBit(size_t i){
            return i & (~i + 1);
        }

        void buildRankTree(){
            const size_t count = indexToEntity.size();
            rankTree.assign(count + 1, 0);
            for (size_t i = 1; i <= count; i++){
                rankTree[i] += isHole(i - 1) ? 0 : 1;
                const size_t parent = i + lowBit(i);
                if (parent <= count){
                    rankTree[parent] += rankTree[i];
                }
            }
            rankTreeBuilt = true;
        }

        void dropRankTree(){
            rankTree.clear();
            rankTreeBuilt = false;
        }

        // live slots among the first `count`
        size_t liveBefore(size_t count) const{
            size_t sum = 0;
            for (size_t i = count; i > 0; i -= lowBit(i)){
                sum += rankTree[i];
            }
            return sum;
        }

        void rankTreeHole(size_t slot){
            for (size_t i = slot + 1; i < rankTree.size(); i += lowBit(i)){
                rankTree[i]--;
            }
        }

        // the slot just appended (live), the tree having the ones before it
        void rankTreeAppend(){
            const size_t i = rankTree.size();
            rankTree.push_back(1 + liveBefore(i - 1) - liveBefore(i - lowBit(i)));
        }

        // the slot of the index-th live one (index < live count)
        size_t slotOfLive(size_t index) const{
            size_t slot = 0;
            size_t remaining = index + 1;
            size_t step = 1;
            while (step * 2 < rankTree.size()) step *= 2;
            for (; step > 0; step /= 2){
                if (slot + step < rankTree.size() && rankTree[slot + step] < remaining){
                    slot += step;
                    remaining -= rankTree[slot];
                }
            }
            return slot;
        }

        // index <-> slot while there are holes
        size_t slotOfIndex(size_t index){
            std::lock_guard<std::recursive_mutex> lock(rankMutex);
            if (!rankTreeBuilt) buildRankTree();
            return slotOfLive(index);
        }

        size_t indexOfSlot(size_t slot){
            std::lock_guard<std::recursive_mutex> lock(rankMutex);
            if (!rankTreeBuilt) buildRankTree();
            return liveBefore(slot);
        }

        size_t indexOfEntity(Entity entity){
            const size_t slot = entityToIndexMap.at(entity);
            return holes == 0 ? slot : indexOfSlot(slot);
        }

        // ----

        void removeSlot(size_t slot){
            if (slot + 1 == componentArray.size()){
                componentArray.pop_back();
                indexToEntity.pop_back();
                // the holes before it are at the end now
                while (!indexToEntity.empty() && isHole(indexToEntity.size() - 1)){
                    componentArray.pop_back();
                    indexToEntity.pop_back();
                    holes--;
                }
                if (holes == 0){
                    holeNext.clear();
                    dropRankTree();
                }else if (rankTreeBuilt){
                    rankTree.resize(componentArray.size() + 1);
                }
                return;
            }
            // what shifting the ones after it over it did: the removed component replaced
            componentArray[slot] = T();
            indexToEntity[slot] = NULL_ENTITY;
            if (holeNext.size() < componentArray.size()){
                holeNext.resize(componentArray.size());
            }
            holeNext[slot] = slot + 1;
            firstHole = (holes == 0) ? slot : std::min(firstHole, slot);
            holes++;
            if (rankTreeBuilt){
                rankTreeHole(slot);
            }
        }

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

            if (holes >= 64 && holes * 2 >= componentArray.size()){
                // Built before closing: the arguments may name a component of this array
                T component(std::forward<Args>(args)...);
                closeHoles();
                append(entity, std::move(component));
                return;
            }
            append(entity, std::forward<Args>(args)...);
        }

        template<typename... Args>
        void append(Entity entity, Args&&... args) {
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
            if (rankTreeBuilt){
                rankTreeAppend();
            }
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

        void closeHolesNow(){
            size_t write = firstHole;
            for (size_t read = firstHole; read < componentArray.size(); read++){
                const Entity entity = indexToEntity[read];
                if (entity == NULL_ENTITY){
                    continue;
                }
                componentArray[write] = std::move(componentArray[read]);
                indexToEntity[write] = entity;
                entityToIndexMap.find(entity)->second = write;
                write++;
            }
            componentArray.erase(componentArray.begin() + write, componentArray.end());
            indexToEntity.resize(write);
            holes = 0;
            holeNext.clear();
            dropRankTree();
        }

        static void outOfRange(){
            Log::error("Retrieving non-existent component: %s", "index out of range");
            throw std::out_of_range("ComponentArray: index out of range");
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
                const size_t slot = it->second;
                entityToIndexMap.erase(it);
                removeSlot(slot);
            } else {
                Log::error("Removing non-existent component");
            }
        }

        // Closes the holes left by removals, keeping the order: O(size) once for any number
        // of them, nothing without. Moves the components after the first hole.
        void compact() override {
            closeHoles();
        }

        // what compact() does, without a virtual call
        void closeHoles(){
            if (holes != 0){
                closeHolesNow();
            }
        }

        bool hasHoles() const{
            return holes != 0;
        }

        // Calls fn(entity, component) for the components after `entity`'s, in order, while it
        // returns true (O(1) amortized a step over holes). Nothing when the entity has none.
        // fn must not add or remove components of this array.
        template<typename Fn>
        void forEachAfter(Entity entity, Fn&& fn) {
            auto it = entityToIndexMap.find(entity);
            if (it == entityToIndexMap.end()){
                return;
            }
            std::unique_lock<std::recursive_mutex> lock(rankMutex, std::defer_lock);
            if (holes != 0) lock.lock(); // the walk shortens the skip links
            for (size_t slot = liveSlotFrom(it->second + 1); slot < componentArray.size(); slot = liveSlotFrom(slot + 1)){
                if (!fn(indexToEntity[slot], componentArray[slot])){
                    return;
                }
            }
        }

        // The same for every component, in order
        template<typename Fn>
        void forEach(Fn&& fn) {
            std::unique_lock<std::recursive_mutex> lock(rankMutex, std::defer_lock);
            if (holes != 0) lock.lock();
            for (size_t slot = liveSlotFrom(0); slot < componentArray.size(); slot = liveSlotFrom(slot + 1)){
                if (!fn(indexToEntity[slot], componentArray[slot])){
                    return;
                }
            }
        }

        void moveEntityRangeToIndex(Entity start, Entity end, size_t newIndex){

            size_t startIndex = indexOfEntity(start);
            size_t endIndex = indexOfEntity(end);
            size_t length = endIndex - startIndex + 1;

            if ((newIndex + length) > size()){
                Log::error("Cannot move entity range out of array");
                return;
            }
            if (newIndex == startIndex){
                return; // where it is (the rotation moved nothing)
            }

            closeHoles();
            moveRange(startIndex, length, newIndex);
        }

        void moveEntityToIndex(Entity entity, size_t newIndex){

            size_t oldIndex = indexOfEntity(entity);
            if (newIndex == oldIndex){
                return; // where it is (the rotation moved nothing)
            }

            closeHoles();
            moveRange(oldIndex, 1, newIndex);
        }

        template<typename C>
        void sortByComponent(ComponentArray<C>* otherComponent) {
            // Key: index in the other array; entities without it go last
            const size_t count = size();
            const size_t missing = std::numeric_limits<size_t>::max();
            if (count < 2){
                return;
            }
            std::vector<size_t> keys;
            keys.reserve(count);
            forEach([&keys, otherComponent, missing](Entity entity, T&){
                keys.push_back(otherComponent->hasEntity(entity) ? otherComponent->indexOfEntity(entity) : missing);
                return true;
            });

            sortByKeys(keys);
        }

        // In the order the entities have in `order` (those not in it go last, in their order).
        // For components without a Transform whose order is a list's, e.g. the editor's
        // scene entity list for skyboxes (the first one is the scene's sky).
        void sortByEntityOrder(const std::vector<Entity>& order) {
            const size_t count = size();
            if (count < 2){
                return;
            }
            std::vector<size_t> keys;
            keys.reserve(count);
            forEach([&keys, &order](Entity entity, T&){
                auto it = std::find(order.begin(), order.end(), entity);
                keys.push_back((it != order.end()) ? (size_t)(it - order.begin()) : order.size() + keys.size());
                return true;
            });
            sortByKeys(keys);
        }

    private:
        // reorders the components by ascending key (one key per component, in array order)
        void sortByKeys(const std::vector<size_t>& keys) {
            // Usually already ordered (a block appended at the end). Check the
            // keys directly before allocating and filling the permutation.
            if (std::is_sorted(keys.begin(), keys.end())){
                return;
            }
            closeHoles();
            const size_t count = componentArray.size();

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
            if (holes != 0){
                return index < size() ? &componentArray[slotOfIndex(index)] : NULL;
            }
            if (index >= componentArray.size()){
                return NULL;
            }

            return &componentArray[index];
        }

        T& getComponentFromIndex(size_t index) {
            if (holes != 0){
                if (index >= size()){
                    outOfRange();
                }
                return componentArray[slotOfIndex(index)];
            }
            try{

                return componentArray.at(index);

            }catch (const std::out_of_range& e){
                Log::error("Retrieving non-existent component: %s", e.what());
                throw;
            }
        }

        size_t getIndex(Entity entity){
            try{

                return indexOfEntity(entity);

            }catch (const std::out_of_range& e){
                Log::error("Retrieving non-existent component: %s", e.what());
                throw;
            }
        }

        Entity getEntity(size_t index){
            if (index >= size()){
                Log::error("Entity not found");
                return NULL_ENTITY;
            }

            return indexToEntity[holes != 0 ? slotOfIndex(index) : index];
        }

        size_t size(){
            return componentArray.size() - holes;
        }

        void entityDestroyed(Entity entity) override {
            if (entityToIndexMap.find(entity) != entityToIndexMap.end()) {
                remove(entity);
            }
        }

        void entitiesDestroyed(const std::unordered_set<Entity>& entities) override {
            for (Entity entity : entities) {
                auto it = entityToIndexMap.find(entity);
                if (it != entityToIndexMap.end()) {
                    const size_t slot = it->second;
                    entityToIndexMap.erase(it);
                    removeSlot(slot);
                }
            }
        }
    };

}

#endif //COMPONENTARRAY_H
