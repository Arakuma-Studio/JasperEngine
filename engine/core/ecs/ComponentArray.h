// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPONENTARRAY_H
#define COMPONENTARRAY_H

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "Entity.h"
#include "Export.h"
#include "Log.h"
#include "ChangeTracking.h"
#include "PagedStorage.h"

namespace jasperengine {

    // How a component array keeps its order (declared when the type is registered)
    enum class ComponentOrder : uint8_t {
        Insertion, // append order, survivors keep theirs on removal (the default)
        Hierarchy, // the order of the owning entities' Transforms, parents before children
        Manual,    // set by sortComponentsByEntityOrder (Skybox)
        Unordered  // swap-and-pop removal; only for new types that opt in
    };

    struct RemovedComponent {
        Entity entity;
        Tick tick;
    };

    // The Removed<T> log, oldest first: blocks of 1024 entries, so that a burst of removals
    // neither copies the log as it grows nor maps fresh memory for each doubling; trimming
    // drops whole blocks from the front (one spare kept)
    class RemovedLog {
    private:
        static constexpr size_t kBlock = 1024;
        static constexpr size_t kBlockBytes = kBlock * sizeof(RemovedComponent);

        // Blocks come from the page arena where there is one (reused across arrays and
        // scenes, so a burst of removals does not fault in fresh memory)
        struct BlockDelete {
            void operator()(RemovedComponent* block) const {
                if (!ecs::freeArenaPage(block, kBlockBytes)){
                    ::operator delete(block);
                }
            }
        };
        using Block = std::unique_ptr<RemovedComponent[], BlockDelete>;

        static_assert(std::is_trivially_copyable_v<RemovedComponent> && std::is_trivially_destructible_v<RemovedComponent>);

        static Block newBlock(){
            void* memory = ecs::allocateArenaPage(kBlockBytes);
            if (!memory){
                memory = ::operator new(kBlockBytes);
            }
            // RemovedComponent is trivial: the storage is its array
            return Block(static_cast<RemovedComponent*>(memory));
        }

        std::vector<Block> blocks;
        Block spare;
        size_t head = 0; // index of the oldest entry in blocks[0]
        size_t count = 0;

        void recycle(Block block){
            if (!spare){
                spare = std::move(block);
            }
        }

    public:
        size_t size() const { return count; }
        bool empty() const { return count == 0; }

        const RemovedComponent& operator[](size_t index) const {
            const size_t position = head + index;
            return blocks[position / kBlock][position % kBlock];
        }

        const RemovedComponent& front() const { return (*this)[0]; }
        const RemovedComponent& back() const { return (*this)[count - 1]; }

        void push_back(const RemovedComponent& entry){
            const size_t position = head + count;
            if (position / kBlock >= blocks.size()){
                blocks.push_back(spare ? std::move(spare) : newBlock());
            }
            blocks[position / kBlock][position % kBlock] = entry;
            count++;
        }

        // drops the oldest `n` entries
        void dropFront(size_t n){
            if (n >= count){
                clear();
                return;
            }
            head += n;
            count -= n;
            const size_t whole = head / kBlock;
            if (whole > 0){
                for (size_t b = 0; b < whole; b++){
                    recycle(std::move(blocks[b]));
                }
                blocks.erase(blocks.begin(), blocks.begin() + (ptrdiff_t)whole);
                head -= whole * kBlock;
            }
        }

        void clear(){
            if (!blocks.empty()){
                recycle(std::move(blocks.front()));
            }
            blocks.clear();
            head = 0;
            count = 0;
        }

        size_t bytes() const {
            return (blocks.size() + (spare ? 1 : 0)) * kBlock * sizeof(RemovedComponent) + blocks.capacity() * sizeof(void*);
        }
    };

    // The type-independent part of a component array: the entity -> slot index, the slot
    // owners, the change ticks and the lazy compaction and ordering state.
    //
    // Removal is O(1): it destroys the component and leaves a hole. Every index-based
    // observer (getComponentFromIndex, getEntity, getIndex, the move and sort operations,
    // views) first settles the array: one stable pass from the first hole closes the holes,
    // then a Hierarchy array that is out of order re-sorts. The result is exactly what an
    // array that erased each removed element at once would show. Lookups by entity never
    // settle.
    class JASPERENGINE_API ComponentArrayBase {
    protected:
        ecs::SparseIndex entityToSlot;
        ecs::PagedArray<Entity> slotEntities; // NULL_ENTITY marks a hole
        ecs::TickColumn changedTicks;
        ecs::TickColumn addedTicks;

        size_t slotCount = 0;  // slots in use, holes included
        size_t holeCount = 0;
        size_t firstHole = 0;  // valid while holeCount > 0

        ComponentOrder order = ComponentOrder::Insertion;
        bool orderDirty = false;
        // holes or orderDirty: the next index observation settles (one test on the hot path)
        bool unsettled = false;
        // removing a component runs no destructor (a trivially destructible type)
        bool trivialRemoval = false;
        const ComponentArrayBase* orderSource = nullptr; // the Transforms, for Hierarchy

        ChangeClock* clock = nullptr; // the owning registry's (none for a standalone array)
        Tick lastChanged = 0;
        Tick lastAdded = 0;
        Tick lastRemoved = 0;
        RemovedLog removedLog;

        size_t compactions = 0;
        size_t sorts = 0;

        static constexpr size_t kMissingKey = std::numeric_limits<size_t>::max();
        // the removal log of an array whose registry never ends a frame keeps this many
        static constexpr size_t kRemovedLogCap = 65536;

        virtual void settleSlow() = 0;

        // the cache line of the component in `slot` (JE_ECS_PREFETCH)
        virtual void prefetchComponent(size_t slot) const = 0;
        // the heap blocks the component in `slot` points to (JE_ECS_PREFETCH)
        virtual void prefetchOwned(size_t slot) const = 0;

        Tick stampTick(){
            return clock ? clock->stamp() : 0;
        }

        // a parallel wave may not compact or sort (SystemScheduler settles before it)
        bool mutationLocked() const {
            if (clock && clock->isWaveLocked()){
                Log::error("ECS: a component array was observed by index during a parallel wave before it was settled");
                assert(false && "ECS: array observed by index inside a parallel wave");
                return true;
            }
            return false;
        }

        void reserveColumns(size_t count){
            slotEntities.reserve(count);
            changedTicks.reserve(count);
            addedTicks.reserve(count);
        }

        void shrinkColumns(){
            slotEntities.shrinkTo(slotCount);
            changedTicks.shrinkTo(slotCount);
            addedTicks.shrinkTo(slotCount);
        }

        // the slots before `slot` are the ones in use when a tick column rebases
        void stampAppended(size_t slot, Tick tick){
            changedTicks.set(slot, tick, slot);
            addedTicks.set(slot, tick, slot);
            if (tick > lastChanged) lastChanged = tick;
            if (tick > lastAdded) lastAdded = tick;
        }

        // A Hierarchy array appended past an entity whose Transform comes later is out of order
        void noteAppended(Entity entity){
            if (order != ComponentOrder::Hierarchy || !orderSource || orderDirty || slotCount == 0){
                return;
            }
            const Entity previous = slotEntities[slotCount - 1];
            if (orderSource->orderKey(entity) < orderSource->orderKey(previous)){
                orderDirty = true;
                unsettled = true;
            }
        }

        void logRemoval(Entity entity, Tick tick){
            if (removedLog.size() >= kRemovedLogCap){
                removedLog.dropFront(kRemovedLogCap / 2);
            }
            removedLog.push_back({entity, tick});
            if (tick > lastRemoved) lastRemoved = tick;
        }

        // Hole bookkeeping after slot `slot` was emptied; trailing holes are dropped at once
        void noteHole(size_t slot){
            slotEntities[slot] = NULL_ENTITY;
            if (slot + 1 == slotCount){
                slotCount--;
                while (holeCount > 0 && slotCount > 0 && slotEntities[slotCount - 1] == NULL_ENTITY){
                    slotCount--;
                    holeCount--;
                }
                if (slotCount == 0){
                    shrinkStorage();
                }
            }else{
                if (holeCount == 0 || slot < firstHole){
                    firstHole = slot;
                }
                holeCount++;
                unsettled = true;
            }
        }

        virtual void shrinkStorage() = 0;

    public:
        // Starts loading what removing the entity's component touches: its slot's owner and
        // component (one index lookup; nothing is read but the index)
        bool hasTrivialRemoval() const { return trivialRemoval; }

        // Starts loading the blocks the entity's component owns (what its destructor frees);
        // the component's own line should be in cache already (prefetchEntity)
        void prefetchOwnedBy(Entity entity) const {
            const uint32_t slot = entityToSlot.find(entity);
            if (slot != ecs::SparseIndex::kNone){
                prefetchOwned(slot);
            }
        }

        JE_ECS_INLINE void prefetchEntity(Entity entity) const {
            const uint32_t slot = entityToSlot.find(entity);
            if (slot != ecs::SparseIndex::kNone){
                JE_ECS_PREFETCH(slotEntities.slot(slot));
                prefetchComponent(slot);
            }
        }

        ComponentArrayBase() = default;
        ComponentArrayBase(const ComponentArrayBase&) = delete;
        ComponentArrayBase& operator=(const ComponentArrayBase&) = delete;
        virtual ~ComponentArrayBase() = default;

        virtual void entityDestroyed(Entity entity) = 0;
        // Several at once (kept for compatibility; removal is O(1) per entity anyway)
        virtual void entitiesDestroyed(const std::unordered_set<Entity>& entities) = 0;
        // ...in the list's order (the Removed<T> log follows it)
        virtual void entitiesDestroyedInOrder(const std::vector<Entity>& entities) = 0;

        // sizeof the component type, and the bytes of its pages
        virtual size_t componentSize() const = 0;
        virtual size_t componentBytes() const = 0;
        // what the components hold in arrays of their own outside the pages (a type with a
        // `size_t arrayHeapBytes() const` member: a mesh's submeshes, a sprite's frames), 0 for
        // the others. One pass over the components: diagnostics only (get_ecs_stats, Systems)
        virtual size_t arrayHeapBytes() = 0;

        // Compacts and re-sorts when needed (the first index observation does it)
        JE_ECS_INLINE void settle(){
            if (unsettled) [[unlikely]] {
                settleCold();
            }
        }

        JE_ECS_COLD void settleCold(){
            settleSlow();
        }

        bool isSettled() const {
            return holeCount == 0 && !orderDirty;
        }

        // Closes the holes now (and re-sorts a Hierarchy array out of order): what the next
        // index observation would do, at a point chosen for it (EntityRegistry::compactComponents:
        // the scene's update and draw, the editor between commands and before a thread reads)
        void compact(){
            settle();
        }

        bool hasHoles() const {
            return holeCount != 0;
        }

        size_t size() const {
            return slotCount - holeCount;
        }

        bool hasEntity(Entity entity) const {
            return entityToSlot.contains(entity);
        }

        JE_ECS_INLINE Entity getEntity(size_t index){
            settle();
            if (index >= slotCount) [[unlikely]] {
                Log::error("Entity not found");
                return NULL_ENTITY;
            }
            return slotEntities[index];
        }

        size_t getIndex(Entity entity){
            settle();
            const uint32_t slot = entityToSlot.find(entity);
            if (slot == ecs::SparseIndex::kNone) [[unlikely]] {
                Log::error("Retrieving non-existent component: entity %u", entity);
                throw std::out_of_range("Retrieving non-existent component");
            }
            return slot;
        }

        // The position of the entity's component among the array's slots, holes included,
        // without settling: slots keep their relative order until the next sort or move, so
        // it orders like the index (kMissingKey when the entity has none)
        size_t orderKey(Entity entity) const {
            const uint32_t slot = entityToSlot.find(entity);
            return slot == ecs::SparseIndex::kNone ? kMissingKey : (size_t)slot;
        }

        // Ordering policy

        ComponentOrder getOrder() const { return order; }
        void setOrder(ComponentOrder order){ this->order = order; }
        void setOrderSource(const ComponentArrayBase* source){ orderSource = source; }

        // A Hierarchy array re-sorts by its order source at its next index observation
        void markOrderDirty(){
            if (order == ComponentOrder::Hierarchy && size() > 1){
                orderDirty = true;
                unsettled = true;
            }
        }

        bool isOrderDirty() const { return orderDirty; }
        size_t getHoleCount() const { return holeCount; }

        // counters for tests and statistics
        size_t getCompactionCount() const { return compactions; }
        size_t getSortCount() const { return sorts; }

        // Change tracking

        void setClock(ChangeClock* clock){ this->clock = clock; }

        Tick getChangedTick(size_t index){
            settle();
            return changedTicks.get(index);
        }

        Tick getAddedTick(size_t index){
            settle();
            return addedTicks.get(index);
        }

        // by entity (no settle); 0 when the entity has no component here
        Tick getChangedTickOf(Entity entity) const {
            const uint32_t slot = entityToSlot.find(entity);
            return slot == ecs::SparseIndex::kNone ? 0 : changedTicks.get(slot);
        }

        Tick getAddedTickOf(Entity entity) const {
            const uint32_t slot = entityToSlot.find(entity);
            return slot == ecs::SparseIndex::kNone ? 0 : addedTicks.get(slot);
        }

        void markChangedAt(size_t index, Tick tick){
            settle();
            if (index < slotCount){
                changedTicks.set(index, tick, slotCount);
                if (tick > lastChanged) lastChanged = tick;
            }
        }

        void markChangedAt(size_t index){
            markChangedAt(index, stampTick());
        }

        // by entity (no settle); false when the entity has no component here
        bool markChanged(Entity entity, Tick tick){
            const uint32_t slot = entityToSlot.find(entity);
            if (slot == ecs::SparseIndex::kNone){
                return false;
            }
            changedTicks.set(slot, tick, slotCount);
            if (tick > lastChanged) lastChanged = tick;
            return true;
        }

        bool markChanged(Entity entity){
            return markChanged(entity, stampTick());
        }

        // The newest tick of any change, addition or removal: a consumer whose gate is newer
        // skips the type in O(1)
        Tick getLastChangedTick() const { return lastChanged; }
        Tick getLastAddedTick() const { return lastAdded; }
        Tick getLastRemovedTick() const { return lastRemoved; }

        // Removed<T>: (entity, tick) of each removal, oldest first
        const RemovedLog& getRemoved() const { return removedLog; }

        // drops the removals stamped at or before `tick`
        void trimRemoved(Tick tick){
            // oldest first, so the ticks ascend
            size_t keep = 0;
            while (keep < removedLog.size() && removedLog[keep].tick <= tick){
                keep++;
            }
            removedLog.dropFront(keep);
        }

        // Memory of everything but the components themselves: index pages, slot owners, ticks
        // and the removal log
        size_t overheadBytes() const {
            return entityToSlot.bytes() + slotEntities.bytes() + changedTicks.bytes() + addedTicks.bytes()
                + removedLog.bytes();
        }

        size_t indexPageCount() const { return entityToSlot.pageCount(); }
    };


    template<typename T>
    class ComponentArray : public ComponentArrayBase{
        template<typename> friend class ComponentArray;

    private:
        ecs::PagedArray<T> components;

        JE_ECS_INLINE T* slotPtr(size_t slot) const {
            return components.slot(slot);
        }

        template<typename... Args>
        T* emplace(Entity entity, Args&&... args) {
            if (entity == NULL_ENTITY){
                Log::error("Component added to the null entity");
                return nullptr;
            }
            if (entityToSlot.contains(entity)) {
                Log::error("Component added to same entity more than once");
                return nullptr;
            }

            components.reserve(slotCount + 1);
            reserveColumns(slotCount + 1);

            // Pages never move: the arguments may name a component of this array
            T* component = ::new (static_cast<void*>(slotPtr(slotCount))) T(std::forward<Args>(args)...);

            noteAppended(entity);
            slotEntities[slotCount] = entity;
            entityToSlot.insert(entity, (uint32_t)slotCount);
            stampAppended(slotCount, stampTick());
            slotCount++;
            return component;
        }

        // Moves the component, owner and ticks of slot `from` into the empty slot `to`
        void relocate(size_t from, size_t to){
            T* source = slotPtr(from);
            ::new (static_cast<void*>(slotPtr(to))) T(std::move(*source));
            std::destroy_at(source);
            const Entity entity = slotEntities[from];
            slotEntities[to] = entity;
            slotEntities[from] = NULL_ENTITY;
            changedTicks.copy(to, from);
            addedTicks.copy(to, from);
            entityToSlot.assign(entity, (uint32_t)to);
        }

        void removeSlot(size_t slot, Tick tick){
            const Entity entity = slotEntities[slot];
            entityToSlot.erase(entity);
            // the component's resources go now (strings, shared_ptr, Lua references)
            std::destroy_at(slotPtr(slot));
            logRemoval(entity, tick);

            if (order == ComponentOrder::Unordered){
                const size_t last = slotCount - 1;
                if (slot != last){
                    relocate(last, slot);
                }
                slotEntities[last] = NULL_ENTITY;
                slotCount--;
                if (slotCount == 0){
                    shrinkStorage();
                }
                return;
            }

            noteHole(slot);
        }

        // One stable pass from the first hole: survivors keep their relative order
        void compactHoles(){
            size_t write = firstHole;
            for (size_t read = firstHole + 1; read < slotCount; read++){
                if (slotEntities[read] != NULL_ENTITY){
                    relocate(read, write);
                    write++;
                }
            }
            slotCount = write;
            holeCount = 0;
            compactions++;
            shrinkStorage();
        }

        void prefetchComponent(size_t slot) const override {
            // a trivially destructible component is not read when it is removed (a Transform:
            // its line would only crowd the cache)
            if constexpr (!std::is_trivially_destructible_v<T>){
                JE_ECS_PREFETCH(slotPtr(slot));
            }
        }

        // The destructor of a component that owns heap blocks (a string, a vector, a
        // shared_ptr's control block) reads each block's allocator header: without a hint
        // each is a cache miss in turn. The component's first 64 bytes are scanned for words
        // that look like user-space pointers, and those are prefetched; a hint never faults,
        // and a word that is no pointer costs at most a wasted line.
        void prefetchOwned(size_t slot) const override {
            if constexpr (!std::is_trivially_destructible_v<T> && sizeof(T) >= sizeof(uintptr_t)){
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(slotPtr(slot));
                constexpr size_t words = (sizeof(T) < 64 ? sizeof(T) : 64) / sizeof(uintptr_t);
                constexpr uintptr_t kMaxUserAddress = sizeof(uintptr_t) == 8 ? static_cast<uintptr_t>(0x7fffffffffffULL) : UINTPTR_MAX;
                for (size_t i = 0; i < words; i++){
                    uintptr_t word;
                    std::memcpy(&word, bytes + i * sizeof(uintptr_t), sizeof(word));
                    if (word >= 0x10000 && word <= kMaxUserAddress && (word & 7) == 0){
                        JE_ECS_PREFETCH(reinterpret_cast<const unsigned char*>(word) - 16);
                    }
                }
            }
        }

        void settleSlow() override {
            if (mutationLocked()){
                return;
            }
            if (holeCount != 0){
                compactHoles();
            }
            if (orderDirty){
                orderDirty = false;
                if (order == ComponentOrder::Hierarchy && orderSource){
                    sortByOrderSource();
                }
            }
            unsettled = false;
        }

        void shrinkStorage() override {
            components.shrinkTo(slotCount);
            shrinkColumns();
        }

        void sortByOrderSource(){
            const size_t count = slotCount;
            if (count < 2){
                return;
            }
            std::vector<size_t> keys(count);
            for (size_t i = 0; i < count; i++){
                keys[i] = orderSource->orderKey(slotEntities[i]);
            }
            sortByKeys(keys);
        }

        void reindex(size_t first, size_t last){
            for (size_t i = first; i < last; i++){
                entityToSlot.assign(slotEntities[i], (uint32_t)i);
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
            ecs::rotateElements(components, first, middle, last);
            ecs::rotateElements(slotEntities, first, middle, last);
            ecs::rotateElements(changedTicks.storage(), first, middle, last);
            ecs::rotateElements(addedTicks.storage(), first, middle, last);
            reindex(first, last);
        }

        // A stable ascending order of the keys: the ascending runs are merged pairwise (a
        // moved branch leaves two or three of them), many runs fall back to stable_sort
        static std::vector<uint32_t> stableOrder(const std::vector<size_t>& keys){
            const size_t count = keys.size();
            std::vector<uint32_t> indices(count);
            std::iota(indices.begin(), indices.end(), 0u);
            auto less = [&keys](uint32_t a, uint32_t b){ return keys[a] < keys[b]; };

            std::vector<size_t> runs{0};
            for (size_t i = 1; i < count; i++){
                if (keys[i] < keys[i - 1]){
                    runs.push_back(i);
                }
            }
            if (runs.size() > 64){
                std::stable_sort(indices.begin(), indices.end(), less);
                return indices;
            }
            runs.push_back(count);

            std::vector<uint32_t> buffer(count);
            while (runs.size() > 2){
                std::vector<size_t> merged{0};
                size_t r = 0;
                for (; r + 2 < runs.size(); r += 2){
                    std::merge(indices.begin() + (ptrdiff_t)runs[r], indices.begin() + (ptrdiff_t)runs[r + 1],
                               indices.begin() + (ptrdiff_t)runs[r + 1], indices.begin() + (ptrdiff_t)runs[r + 2],
                               buffer.begin() + (ptrdiff_t)runs[r], less);
                    merged.push_back(runs[r + 2]);
                }
                if (r + 1 < runs.size()){
                    std::copy(indices.begin() + (ptrdiff_t)runs[r], indices.begin() + (ptrdiff_t)runs[r + 1], buffer.begin() + (ptrdiff_t)runs[r]);
                    if (merged.back() != runs[r + 1]){
                        merged.push_back(runs[r + 1]);
                    }
                }
                indices.swap(buffer);
                runs.swap(merged);
            }
            return indices;
        }

        // reorders the components by ascending key (one key per component, in array order);
        // equal keys keep their relative order
        void sortByKeys(const std::vector<size_t>& keys) {
            const size_t count = slotCount;
            // Usually already ordered (a block appended at the end). Check the
            // keys directly before allocating and filling the permutation.
            if (std::is_sorted(keys.begin(), keys.end())){
                return;
            }

            const std::vector<uint32_t> indices = stableOrder(keys);

            // Only the span that changed is moved, component by component
            size_t first = 0;
            while (first < count && indices[first] == first) first++;
            size_t last = count;
            while (last > first && indices[last - 1] == last - 1) last--;

            std::vector<T> moved;
            moved.reserve(last - first);
            std::vector<Entity> movedEntities(last - first);
            std::vector<uint32_t> movedChanged(last - first);
            std::vector<uint32_t> movedAdded(last - first);
            for (size_t newIndex = first; newIndex < last; ++newIndex) {
                const size_t from = indices[newIndex];
                moved.emplace_back(std::move(*slotPtr(from)));
                movedEntities[newIndex - first] = slotEntities[from];
                movedChanged[newIndex - first] = changedTicks.raw(from);
                movedAdded[newIndex - first] = addedTicks.raw(from);
            }
            for (size_t newIndex = first; newIndex < last; ++newIndex) {
                *slotPtr(newIndex) = std::move(moved[newIndex - first]);
                slotEntities[newIndex] = movedEntities[newIndex - first];
                changedTicks.setRaw(newIndex, movedChanged[newIndex - first]);
                addedTicks.setRaw(newIndex, movedAdded[newIndex - first]);
            }
            reindex(first, last);
            sorts++;
        }

        template<typename Fn>
        void walkFrom(size_t slot, Fn& fn) {
            for (; slot < slotCount; slot++){
                const Entity entity = slotEntities[slot];
                if (entity != NULL_ENTITY && !fn(entity, *slotPtr(slot))){
                    return;
                }
            }
        }

    public:
        ComponentArray(){
            trivialRemoval = std::is_trivially_destructible_v<T>;
        }

        ~ComponentArray() override {
            for (size_t slot = 0; slot < slotCount; slot++){
                if (slotEntities[slot] != NULL_ENTITY){
                    std::destroy_at(slotPtr(slot));
                }
            }
        }

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
            const uint32_t slot = entityToSlot.find(entity);
            if (slot != ecs::SparseIndex::kNone){
                removeSlot(slot, stampTick());
            } else {
                Log::error("Removing non-existent component");
            }
        }

        void moveEntityRangeToIndex(Entity start, Entity end, size_t newIndex){
            settle();
            const uint32_t startSlot = entityToSlot.find(start);
            const uint32_t endSlot = entityToSlot.find(end);
            if (startSlot == ecs::SparseIndex::kNone || endSlot == ecs::SparseIndex::kNone || endSlot < startSlot){
                Log::error("Cannot move entity range: entities not in the array");
                return;
            }

            size_t startIndex = startSlot;
            size_t endIndex = endSlot;
            size_t length = endIndex - startIndex + 1;

            if ((newIndex + length) > slotCount){
                Log::error("Cannot move entity range out of array");
                return;
            }

            moveRange(startIndex, length, newIndex);
        }

        void moveEntityToIndex(Entity entity, size_t newIndex){
            settle();
            const uint32_t oldSlot = entityToSlot.find(entity);
            if (oldSlot == ecs::SparseIndex::kNone || newIndex >= slotCount){
                Log::error("Cannot move entity to index %zu", newIndex);
                return;
            }

            moveRange(oldSlot, 1, newIndex);
        }

        template<typename C>
        void sortByComponent(ComponentArray<C>* otherComponent) {
            // Key: index in the other array; entities without it go last
            settle();
            otherComponent->settle();
            const size_t count = slotCount;
            if (count < 2){
                return;
            }
            std::vector<size_t> keys(count);
            for (size_t i = 0; i < count; i++){
                keys[i] = otherComponent->orderKey(slotEntities[i]);
            }

            sortByKeys(keys);
        }

        // In the order the entities have in `order` (those not in it go last, in their order).
        // For components without a Transform whose order is a list's, e.g. the editor's
        // scene entity list for skyboxes (the first one is the scene's sky).
        void sortByEntityOrder(const std::vector<Entity>& order) {
            settle();
            const size_t count = slotCount;
            if (count < 2){
                return;
            }
            // first position of each entity, as a search of the list would find it
            std::unordered_map<Entity, size_t> positions;
            positions.reserve(order.size());
            for (size_t i = 0; i < order.size(); i++){
                positions.emplace(order[i], i);
            }
            std::vector<size_t> keys(count);
            for (size_t i = 0; i < count; i++){
                auto it = positions.find(slotEntities[i]);
                keys[i] = (it != positions.end()) ? it->second : order.size() + i;
            }
            sortByKeys(keys);
        }

        // The component in the next live slot after the entity's, without settling: holes
        // keep the slots' relative order, so it is the next index once the holes close
        // (nullptr: none, or the entity has no component)
        const T* findNextAfter(Entity entity) const {
            const uint32_t slot = entityToSlot.find(entity);
            if (slot == ecs::SparseIndex::kNone) return nullptr;
            for (size_t next = (size_t)slot + 1; next < slotCount; next++){
                if (slotEntities[next] != NULL_ENTITY) return slotPtr(next);
            }
            return nullptr;
        }

        // Calls fn(entity, component) for every component in index order while it returns
        // true. Holes are stepped over, not closed: a component found by entity before stays
        // where it is (only a Hierarchy array out of order is sorted first, as an index read
        // would). fn must not add or remove components of this array.
        template<typename Fn>
        void forEach(Fn&& fn) {
            if (orderDirty){
                settle();
            }
            walkFrom(0, fn);
        }

        // The same for the components after the entity's (nothing when it has none here)
        template<typename Fn>
        void forEachAfter(Entity entity, Fn&& fn) {
            if (orderDirty){
                settle();
            }
            const uint32_t slot = entityToSlot.find(entity);
            if (slot != ecs::SparseIndex::kNone){
                walkFrom((size_t)slot + 1, fn);
            }
        }

        JE_ECS_INLINE T* findComponent(Entity entity) {
            const uint32_t slot = entityToSlot.find(entity);
            return slot == ecs::SparseIndex::kNone ? nullptr : slotPtr(slot);
        }

        T& getComponent(Entity entity) {
            const uint32_t slot = entityToSlot.find(entity);
            if (slot == ecs::SparseIndex::kNone) [[unlikely]] {
                Log::error("Retrieving non-existent component: entity %u", entity);
                throw std::out_of_range("Retrieving non-existent component");
            }
            return *slotPtr(slot);
        }

        JE_ECS_INLINE T* findComponentFromIndex(size_t index) {
            settle();
            if (index >= slotCount){
                return nullptr;
            }

            return slotPtr(index);
        }

        JE_ECS_INLINE T& getComponentFromIndex(size_t index) {
            settle();
            if (index >= slotCount) [[unlikely]] {
                Log::error("Retrieving non-existent component: index %zu", index);
                throw std::out_of_range("Retrieving non-existent component");
            }
            return *slotPtr(index);
        }

        void entityDestroyed(Entity entity) override {
            const uint32_t slot = entityToSlot.find(entity);
            if (slot != ecs::SparseIndex::kNone) {
                removeSlot(slot, stampTick());
            }
        }

        void entitiesDestroyed(const std::unordered_set<Entity>& entities) override {
            const Tick tick = stampTick();
            for (Entity entity : entities) {
                const uint32_t slot = entityToSlot.find(entity);
                if (slot != ecs::SparseIndex::kNone) {
                    removeSlot(slot, tick);
                }
            }
        }

        void entitiesDestroyedInOrder(const std::vector<Entity>& entities) override {
            const Tick tick = stampTick();
            for (Entity entity : entities) {
                const uint32_t slot = entityToSlot.find(entity);
                if (slot != ecs::SparseIndex::kNone) {
                    removeSlot(slot, tick);
                }
            }
        }

        size_t componentSize() const override {
            return sizeof(T);
        }

        size_t componentBytes() const override {
            return components.bytes();
        }

        size_t arrayHeapBytes() override {
            if constexpr (requires(const T& component) { component.arrayHeapBytes(); }) {
                size_t bytes = 0;
                const size_t count = size();
                for (size_t i = 0; i < count; i++) {
                    if (const T* component = findComponentFromIndex(i)) {
                        bytes += component->arrayHeapBytes();
                    }
                }
                return bytes;
            } else {
                return 0;
            }
        }

        // Pages of a settled array: page p holds the components [p * pageCapacity(), ...),
        // contiguous in memory. Settle first (views do).
        static constexpr size_t pageCapacity() { return ecs::PagedArray<T>::kPageCapacity; }

        size_t pageCount() const {
            return (slotCount + pageCapacity() - 1) / pageCapacity();
        }

        T* pageData(size_t page) const {
            return components.page(page);
        }

        // without settling: for views over an array settled before
        T& componentAtSettled(size_t index) const { return *slotPtr(index); }
        Entity entityAtSettled(size_t index) const { return slotEntities[index]; }
        Tick changedTickAtSettled(size_t index) const { return changedTicks.get(index); }
        Tick addedTickAtSettled(size_t index) const { return addedTicks.get(index); }
        void markChangedAtSettled(size_t index, Tick tick){
            changedTicks.set(index, tick, slotCount);
            if (tick > lastChanged) lastChanged = tick;
        }

        // The entity whose component is at `component` (a pointer into this array), or
        // NULL_ENTITY: the page search the Lua setters use to stamp a change
        Entity findOwner(const T* component) const {
            const uintptr_t address = reinterpret_cast<uintptr_t>(component);
            for (size_t page = 0; page < components.pageCount(); page++){
                const uintptr_t begin = reinterpret_cast<uintptr_t>(components.page(page));
                const uintptr_t end = begin + pageCapacity() * sizeof(T);
                if (address >= begin && address < end){
                    const size_t slot = page * pageCapacity() + (address - begin) / sizeof(T);
                    return slot < slotCount ? slotEntities[slot] : NULL_ENTITY;
                }
            }
            return NULL_ENTITY;
        }
    };

}

#endif //COMPONENTARRAY_H
