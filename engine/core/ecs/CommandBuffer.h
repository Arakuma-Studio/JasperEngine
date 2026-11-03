// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ECS_COMMANDBUFFER_H
#define ECS_COMMANDBUFFER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "Entity.h"
#include "EntityRef.h"
#include "Export.h"

namespace jasperengine {

    class EntityRegistry;
    class Scene;

    // An entity the buffer creates at playback; usable as a target of later commands
    struct PendingEntity {
        static constexpr uint32_t kNone = 0xFFFFFFFFu;
        uint32_t index = kNone;
    };

    // What a command acts on: a live entity (EntityRef) or one the same buffer creates
    struct EntityTarget {
        EntityRef ref;
        uint32_t pending = PendingEntity::kNone;

        EntityTarget() = default;
        EntityTarget(EntityRef ref): ref(ref) {}
        EntityTarget(PendingEntity entity): pending(entity.index) {}

        bool isPending() const { return pending != PendingEntity::kNone; }
        // no entity at all (setParent to the top of the hierarchy)
        bool isNull() const { return !isPending() && ref.isNull(); }
    };

    namespace ecs {

        // A type-erased value moved into a command: up to 64 bytes inline, larger (or
        // throwing-move) types on the heap. Moved and destroyed through the type's own
        // operations, never copied bytewise, so strings, shared_ptr and Lua references are
        // released exactly once whether the command is played back or discarded.
        class CommandPayload {
        public:
            static constexpr size_t kInlineSize = 64;

        private:
            struct Ops {
                void (*move)(CommandPayload& to, CommandPayload& from);
                void (*destroy)(CommandPayload& payload);
                void* (*get)(CommandPayload& payload);
            };

            alignas(std::max_align_t) unsigned char storage[kInlineSize];
            void* heap = nullptr;
            const Ops* ops = nullptr;

            template<typename U>
            static constexpr bool fitsInline(){
                return sizeof(U) <= kInlineSize && alignof(U) <= alignof(std::max_align_t) && std::is_nothrow_move_constructible_v<U>;
            }

            template<typename U>
            static const Ops* opsFor(){
                if constexpr (fitsInline<U>()){
                    static const Ops ops = {
                        [](CommandPayload& to, CommandPayload& from){
                            U* source = std::launder(reinterpret_cast<U*>(from.storage));
                            ::new (static_cast<void*>(to.storage)) U(std::move(*source));
                            source->~U();
                        },
                        [](CommandPayload& payload){
                            std::launder(reinterpret_cast<U*>(payload.storage))->~U();
                        },
                        [](CommandPayload& payload) -> void* {
                            return std::launder(reinterpret_cast<U*>(payload.storage));
                        }
                    };
                    return &ops;
                }else{
                    static const Ops ops = {
                        [](CommandPayload& to, CommandPayload& from){
                            to.heap = from.heap;
                            from.heap = nullptr;
                        },
                        [](CommandPayload& payload){
                            delete static_cast<U*>(payload.heap);
                            payload.heap = nullptr;
                        },
                        [](CommandPayload& payload) -> void* {
                            return payload.heap;
                        }
                    };
                    return &ops;
                }
            }

            void moveFrom(CommandPayload& other){
                if (other.ops){
                    other.ops->move(*this, other);
                    ops = other.ops;
                    other.ops = nullptr;
                }
            }

        public:
            CommandPayload() = default;
            CommandPayload(const CommandPayload&) = delete;
            CommandPayload& operator=(const CommandPayload&) = delete;

            CommandPayload(CommandPayload&& other) noexcept {
                moveFrom(other);
            }

            CommandPayload& operator=(CommandPayload&& other) noexcept {
                if (this != &other){
                    reset();
                    moveFrom(other);
                }
                return *this;
            }

            ~CommandPayload(){
                reset();
            }

            template<typename U, typename... Args>
            U& emplace(Args&&... args){
                reset();
                if constexpr (fitsInline<U>()){
                    U* value = ::new (static_cast<void*>(storage)) U(std::forward<Args>(args)...);
                    ops = opsFor<U>();
                    return *value;
                }else{
                    U* value = new U(std::forward<Args>(args)...);
                    heap = value;
                    ops = opsFor<U>();
                    return *value;
                }
            }

            template<typename U>
            U& get(){
                return *static_cast<U*>(ops->get(*this));
            }

            bool empty() const { return ops == nullptr; }
            bool isInline() const { return ops != nullptr && heap == nullptr; }

            void reset(){
                if (ops){
                    ops->destroy(*this);
                    ops = nullptr;
                }
            }
        };

    }

    // Structural changes recorded now and applied later, in order, on the main thread:
    // during iteration (StructuralDeferScope), by scheduled systems (one buffer per system,
    // played back after its wave) and by asynchronous code. Playback advances the registry's
    // change tick, resolves the PendingEntity of each create, skips (with a warning) commands
    // on an entity that is gone since it was recorded, applies consecutive destroys as one
    // destroyEntities and fires the registry's usual hooks.
    class JASPERENGINE_API EntityCommandBuffer {
    public:
        enum class Op : uint8_t {
            Create, Destroy, DestroyHierarchy, Add, Remove, SetParent,
            SetActive, SetName, SetTag, SetLayer, Defer
        };

    private:
        using ComponentFn = void (*)(EntityRegistry& registry, Entity entity, ecs::CommandPayload& payload);

        struct Command {
            Op op;
            EntityTarget target;
            EntityTarget other; // the parent of SetParent
            int32_t value = 0;  // SetActive, SetLayer, SetParent's keepWorld
            ComponentFn component = nullptr;
            ecs::CommandPayload payload;
        };

        std::vector<Command> commands;
        uint32_t pendingCount = 0;

        template<typename C, typename Registry>
        static void applyAdd(Registry& registry, Entity entity, ecs::CommandPayload& payload){
            registry.template addComponent<C>(entity, std::move(payload.get<C>()));
        }

        template<typename C, typename Registry>
        static void applyRemove(Registry& registry, Entity entity, ecs::CommandPayload&){
            // a removal recorded twice (remove, an add that found the component still there,
            // remove again, in one deferred scope) or of a component removed since: nothing left
            if (registry.template findComponent<C>(entity)){
                registry.template removeComponent<C>(entity);
            }
        }

        Command& push(Op op, EntityTarget target){
            commands.emplace_back();
            Command& command = commands.back();
            command.op = op;
            command.target = target;
            return command;
        }

        bool resolve(EntityRegistry& registry, const EntityTarget& target, const std::vector<Entity>& created, const char* what, Entity& entity) const;

    public:
        EntityCommandBuffer() = default;
        EntityCommandBuffer(const EntityCommandBuffer&) = delete;
        EntityCommandBuffer& operator=(const EntityCommandBuffer&) = delete;
        EntityCommandBuffer(EntityCommandBuffer&&) = default;
        EntityCommandBuffer& operator=(EntityCommandBuffer&&) = default;

        // An entity of the registry's default pool, made at playback
        PendingEntity create();

        // destroyEntity (registry semantics: this entity only)
        void destroy(EntityTarget entity);
        // GameObject.destroy semantics: the entity with its Transform children, scripts
        // stopped first (Scene only; a plain registry destroys the branch)
        void destroyHierarchy(EntityTarget entity);

        template<typename T>
        void add(EntityTarget entity, T&& component){
            using C = std::remove_cv_t<std::remove_reference_t<T>>;
            Command& command = push(Op::Add, entity);
            command.payload.emplace<C>(std::forward<T>(component));
            command.component = &applyAdd<C, EntityRegistry>;
        }

        template<typename T>
        void add(EntityTarget entity){
            add<T>(entity, T());
        }

        template<typename T>
        void remove(EntityTarget entity){
            Command& command = push(Op::Remove, entity);
            command.component = &applyRemove<T, EntityRegistry>;
        }

        // addEntityChild(parent, child, keepWorld); a null parent moves it to the top
        void setParent(EntityTarget child, EntityTarget parent, bool keepWorld);
        void setActive(EntityTarget entity, bool active);
        void setName(EntityTarget entity, std::string name);
        void setTag(EntityTarget entity, std::string tag);
        void setLayer(EntityTarget entity, int layer);

        // Runs at its place in the playback (needs a Scene)
        void defer(std::function<void(Scene&)> fn);

        // Applies every command in order and empties the buffer. Commands recorded while it
        // plays (by the hooks) wait for the next playback.
        void playback(EntityRegistry& registry);

        // Drops every command; their payloads are destroyed
        void clear();

        bool empty() const { return commands.empty(); }
        size_t size() const { return commands.size(); }
    };

}

#endif // ECS_COMMANDBUFFER_H
