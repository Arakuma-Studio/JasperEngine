// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPONENTMANAGER_H
#define COMPONENTMANAGER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>
#include "ComponentArray.h"
#include "Signature.h"
#include "Log.h"

namespace jasperengine {

	// A signature bit position: 0..kMaxComponentTypes-1
	using ComponentId = unsigned char;


	// Dense index per component type, so an array lookup is a vector subscript instead
	// of hashing typeid().name() on every call.
	class JASPERENGINE_API ComponentTypeRegistry {
	public:
		static size_t indexOf(const char* mangledName);
	};

	namespace ecs {
		// componentTypeIndex<T>()'s cache: constant-initialized (no guard on the lookup path);
		// set on first use, to the same value by any thread that races to it
		template<typename T>
		inline std::atomic<size_t> componentTypeIndexCache{SIZE_MAX};

		template<typename T>
		JE_ECS_COLD size_t componentTypeIndexFirstUse(){
			const size_t index = ComponentTypeRegistry::indexOf(typeid(T).name());
			componentTypeIndexCache<T>.store(index, std::memory_order_relaxed);
			return index;
		}
	}

	template<typename T>
	JE_ECS_INLINE size_t componentTypeIndex(){
		// Cached per module; the shared registry keeps the value equal across them.
		const size_t index = ecs::componentTypeIndexCache<T>.load(std::memory_order_relaxed);
		if (index == SIZE_MAX) [[unlikely]] {
			return ecs::componentTypeIndexFirstUse<T>();
		}
		return index;
	}

	namespace ecs {
		// "RenderSystem" for typeid(RenderSystem).name() on every compiler (namespaces dropped)
		JASPERENGINE_API std::string readableTypeName(const char* typeName);

		// Debug access check (SystemScheduler, D10): reports an access to a component type the
		// system running on this thread did not declare. Compiled in when
		// JASPERENGINE_ECS_ACCESS_CHECKS is on (the default in builds without NDEBUG).
		JASPERENGINE_API void checkComponentAccess(ComponentId id);
	}

#if !defined(JASPERENGINE_ECS_ACCESS_CHECKS) && !defined(NDEBUG)
	#define JASPERENGINE_ECS_ACCESS_CHECKS 1
#endif


	class JASPERENGINE_API ComponentManager {
	private:
		struct ComponentEntry {
			std::shared_ptr<ComponentArrayBase> array;
			ComponentId id = 0;
		};

		// Indexed by componentTypeIndex<T>(); unregistered types keep an empty slot.
		std::vector<ComponentEntry> entries{};
		// Indexed by ComponentId (the signature bit)
		std::array<ComponentArrayBase*, kMaxComponentTypes> arraysById{};
		std::array<std::string, kMaxComponentTypes> namesById{};
		size_t registeredCount = 0;

		// logs and throws std::out_of_range (the cold path of the getters)
		[[noreturn]] static void notRegistered();

		JE_ECS_INLINE const ComponentEntry* findEntry(size_t index) const {
			if (index >= entries.size() || !entries[index].array){
				return nullptr;
			}
			return &entries[index];
		}

	public:
		ComponentManager() = default;
		ComponentManager(const ComponentManager&) = delete;
		ComponentManager& operator=(const ComponentManager&) = delete;

		// whether registerComponent<T>() ran (the getters throw for a type that is not)
		template<typename T>
		bool isRegistered() const {
			return findEntry(componentTypeIndex<T>()) != nullptr;
		}

		// False (logged) for a type registered twice, or past kMaxComponentTypes: that type
		// stays unregistered
		template<typename T>
		bool registerComponent(ComponentOrder order = ComponentOrder::Insertion, const char* name = nullptr) {
			const size_t index = componentTypeIndex<T>();

			if (index < entries.size() && entries[index].array){
				Log::error("Registering component type more than once");
				return false;
			}

			if (registeredCount >= kMaxComponentTypes){
				Log::error("Cannot register component type %s: the limit of %zu component types is reached",
					name ? name : typeid(T).name(), kMaxComponentTypes);
				return false;
			}

			if (index >= entries.size()){
				entries.resize(index + 1);
			}

			// Ids stay sequential: they are signature bit positions.
			auto array = std::make_shared<ComponentArray<T>>();
			array->setOrder(order);
			const ComponentId id = (ComponentId)registeredCount;
			entries[index].array = array;
			entries[index].id = id;
			arraysById[id] = array.get();
			namesById[id] = name ? std::string(name) : std::string();

			++registeredCount;
			return true;
		}

		template<typename T>
		ComponentId getComponentId() const{
			if (const ComponentEntry* entry = findEntry(componentTypeIndex<T>())){
				return entry->id;
			}

			// Not a default: 0 is the first component's signature bit.
			Log::error("Component not registered before use");
			throw std::out_of_range("Component not registered before use");
		}

		// Raw pointer: copying the shared_ptr cost an atomic pair per access. Never
		// null, since every caller dereferences it right away.
		template<typename T>
		JE_ECS_INLINE ComponentArray<T>* getComponentArray() const {
			if (const ComponentEntry* entry = findEntry(componentTypeIndex<T>())) [[likely]] {
#if JASPERENGINE_ECS_ACCESS_CHECKS
				ecs::checkComponentAccess(entry->id);
#endif
				return static_cast<ComponentArray<T>*>(entry->array.get());
			}
			notRegistered();
		}

		// By signature bit; null for an unused id
		ComponentArrayBase* getArray(ComponentId id) const {
			return arraysById[id];
		}

		// The name given at registration ("" when none was)
		const std::string& getName(ComponentId id) const {
			return namesById[id];
		}

		// The id of the type registered under `name`; false when there is none
		bool findId(const std::string& name, ComponentId& id) const {
			for (size_t i = 0; i < registeredCount; i++){
				if (namesById[i] == name){
					id = (ComponentId)i;
					return true;
				}
			}
			return false;
		}

		size_t getRegisteredCount() const {
			return registeredCount;
		}

		template<typename T>
		void addComponent(Entity entity) {
			getComponentArray<T>()->insert(entity);
		}

		template<typename T>
		void addComponent(Entity entity, const T& component) {
			getComponentArray<T>()->insert(entity, component);
		}

		template<typename T>
		void addComponent(Entity entity, T&& component) {
			using Component = std::remove_cv_t<std::remove_reference_t<T>>;
			getComponentArray<Component>()->insert(entity, std::forward<T>(component));
		}

		template<typename T>
		void removeComponent(Entity entity) {
			getComponentArray<T>()->remove(entity);
		}

		template<typename T>
		JE_ECS_INLINE T* findComponent(Entity entity) const {
			return getComponentArray<T>()->findComponent(entity);
		}

		template<typename T>
		T& getComponent(Entity entity) const {
			return getComponentArray<T>()->getComponent(entity);
		}

		template<typename T>
	    T* findComponentFromIndex(size_t index) const{
		    return getComponentArray<T>()->findComponentFromIndex(index);
	    }

		template<typename T>
	    JE_ECS_INLINE T& getComponentFromIndex(size_t index) const {
		    return getComponentArray<T>()->getComponentFromIndex(index);
	    }

		// Every array (an entity whose signature is not known)
		void entityDestroyed(Entity entity) {
			for (size_t id = 0; id < registeredCount; id++) {
				arraysById[id]->entityDestroyed(entity);
			}
		}

		// starts loading the entity's slots in the arrays of the signature's types
		void prefetchEntity(Entity entity, const Signature& signature) const {
			signature.forEachSet([&](size_t id){
				if (const ComponentArrayBase* array = arraysById[id]){
					array->prefetchEntity(entity);
				}
			});
		}

		// Only the arrays of the signature's types
		void entityDestroyed(Entity entity, const Signature& signature) {
			signature.forEachSet([&](size_t id){
				if (ComponentArrayBase* array = arraysById[id]){
					array->entityDestroyed(entity);
				}
			});
		}

		void entitiesDestroyed(const std::unordered_set<Entity>& entities) {
			for (size_t id = 0; id < registeredCount; id++) {
				arraysById[id]->entitiesDestroyed(entities);
			}
		}

		// `types`: the union of the entities' signatures
		void entitiesDestroyed(const std::vector<Entity>& entities, const Signature& types) {
			types.forEachSet([&](size_t id){
				if (ComponentArrayBase* array = arraysById[id]){
					array->entitiesDestroyedInOrder(entities);
				}
			});
		}

		// fn(ComponentId, ComponentArrayBase&) for every registered type, by id
		template<typename F>
		void forEachArray(F&& fn) const {
			for (size_t id = 0; id < registeredCount; id++){
				fn((ComponentId)id, *arraysById[id]);
			}
		}
	};

}

#endif //COMPONENTMANAGER_H
