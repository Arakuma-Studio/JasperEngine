// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ECS_VIEW_H
#define ECS_VIEW_H

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>
#include "ComponentManager.h"
#include "ChangeTracking.h"

namespace jasperengine {

    // Access declarations of a view: the entity has a T it reads or writes, may have a T
    // (the callback gets a pointer, null when it has none), or has no T
    template<typename T> struct Read { using type = T; };
    template<typename T> struct Write { using type = T; };
    template<typename T> struct Optional { using type = T; };
    template<typename T> struct Without { using type = T; };

    namespace ecs::view_detail {

        template<typename Q> struct Kind;
        template<typename T> struct Kind<Read<T>>     { static constexpr bool required = true;  static constexpr bool optional = false; static constexpr bool excluded = false; static constexpr bool write = false; using param = const T&; };
        template<typename T> struct Kind<Write<T>>    { static constexpr bool required = true;  static constexpr bool optional = false; static constexpr bool excluded = false; static constexpr bool write = true;  using param = T&; };
        template<typename T> struct Kind<Optional<T>> { static constexpr bool required = false; static constexpr bool optional = true;  static constexpr bool excluded = false; static constexpr bool write = false; using param = T*; };
        template<typename T> struct Kind<Without<T>>  { static constexpr bool required = false; static constexpr bool optional = false; static constexpr bool excluded = true;  static constexpr bool write = false; using param = void; };

        // index of the first Read/Write declaration: the driving array
        template<size_t I, typename... Q> struct FirstRequired;
        template<size_t I> struct FirstRequired<I> { static constexpr size_t value = I; };
        template<size_t I, typename Q, typename... Rest> struct FirstRequired<I, Q, Rest...> {
            static constexpr size_t value = Kind<Q>::required ? I : FirstRequired<I + 1, Rest...>::value;
        };

    }

    // A typed query over a registry's component arrays.
    //
    // The first Read or Write type drives the iteration: rows come in its array's order, so a
    // view driven by Transform (or a Hierarchy type) visits parents before children. Every
    // other type is looked up by entity. The driving array is settled (compacted and sorted)
    // when the iteration starts. Write<T> declares the access (for the scheduler and the debug
    // access check); it does not stamp a change: call markChanged for that.
    //
    //   registry.view<Read<Transform>, Write<MeshComponent>, Without<UIComponent>>()
    //       .changedSince(gate)
    //       .each([](Entity e, const Transform& t, MeshComponent& m){ ... });
    template<typename... Q>
    class View {
    private:
        static_assert(sizeof...(Q) > 0, "View needs at least one component type");
        static constexpr size_t kDriving = ecs::view_detail::FirstRequired<0, Q...>::value;
        static_assert(kDriving < sizeof...(Q), "View needs a Read or Write component type to drive the iteration");

        using Arrays = std::tuple<ComponentArray<typename Q::type>*...>;
        using DrivingType = typename std::tuple_element_t<kDriving, std::tuple<Q...>>::type;

        enum class Filter { None, Changed, Added };

        Arrays arrays;
        Filter filter = Filter::None;
        Tick since = 0;

        ComponentArray<DrivingType>* driving() const {
            return std::get<kDriving>(arrays);
        }

        // false when no row can pass the change filter: every required array is older
        bool anyNewer() const {
            if (filter == Filter::None){
                return true;
            }
            return anyNewerImpl(std::index_sequence_for<Q...>{});
        }

        template<size_t... I>
        bool anyNewerImpl(std::index_sequence<I...>) const {
            bool newer = false;
            ((newer = newer || (ecs::view_detail::Kind<std::tuple_element_t<I, std::tuple<Q...>>>::required &&
                (filter == Filter::Changed ? std::get<I>(arrays)->getLastChangedTick() : std::get<I>(arrays)->getLastAddedTick()) > since)), ...);
            return newer;
        }

        template<size_t I>
        auto fetch(Entity entity, size_t index, bool& pass, bool& newer) const {
            using Qi = std::tuple_element_t<I, std::tuple<Q...>>;
            using K = ecs::view_detail::Kind<Qi>;
            using T = typename Qi::type;
            auto* array = std::get<I>(arrays);
            T* component;
            Tick tick = 0;
            if constexpr (I == kDriving){
                component = &array->componentAtSettled(index);
                if (filter != Filter::None){
                    tick = filter == Filter::Changed ? array->changedTickAtSettled(index) : array->addedTickAtSettled(index);
                }
            }else{
                component = array->findComponent(entity);
                if (component && K::required && filter != Filter::None){
                    tick = filter == Filter::Changed ? array->getChangedTickOf(entity) : array->getAddedTickOf(entity);
                }
            }
            if constexpr (K::required){
                if (!component) pass = false;
                if (tick > since) newer = true;
            }
            if constexpr (K::excluded){
                if (component) pass = false;
            }
            return component;
        }

        // the callback argument of declaration I: none for Without, a pointer for Optional,
        // a reference otherwise
        template<size_t I, typename T>
        static auto argument(T* component){
            using K = ecs::view_detail::Kind<std::tuple_element_t<I, std::tuple<Q...>>>;
            if constexpr (K::excluded){
                return std::tuple<>{};
            }else if constexpr (K::optional){
                return std::tuple<T*>(component);
            }else if constexpr (K::write){
                return std::tuple<T&>(*component);
            }else{
                return std::tuple<const T&>(*component);
            }
        }

        template<typename F, size_t... I>
        void row(F& fn, size_t index, std::index_sequence<I...>) const {
            const Entity entity = driving()->entityAtSettled(index);
            bool pass = true;
            bool newer = filter == Filter::None;
            const auto components = std::make_tuple(fetch<I>(entity, index, pass, newer)...);
            if (!pass || !newer){
                return;
            }
            std::apply(fn, std::tuple_cat(std::tuple<Entity>(entity), argument<I>(std::get<I>(components))...));
        }

    public:
        // A run of the driving array's rows that share one storage page
        class Page {
        private:
            const View* view;
            size_t first;
            size_t last;

        public:
            Page(const View* view, size_t first, size_t last): view(view), first(first), last(last) {}

            size_t begin() const { return first; }
            size_t end() const { return last; }
            // the driving rows of the page, before filtering
            size_t size() const { return last - first; }

            template<typename F>
            void each(F&& fn) const {
                for (size_t i = first; i < last; i++){
                    view->row(fn, i, std::index_sequence_for<Q...>{});
                }
            }
        };

        explicit View(const ComponentManager& manager)
            : arrays(manager.getComponentArray<typename Q::type>()...) {}

        // only rows where a Read/Write component changed after the gate
        View& changedSince(const ChangeGate& gate){
            filter = Filter::Changed;
            since = gate.last;
            return *this;
        }

        View& changedSince(Tick tick){
            return changedSince(ChangeGate{tick});
        }

        // only rows where a Read/Write component was added after the gate
        View& addedSince(const ChangeGate& gate){
            filter = Filter::Added;
            since = gate.last;
            return *this;
        }

        View& addedSince(Tick tick){
            return addedSince(ChangeGate{tick});
        }

        // fn(Entity, const A&, B&, C*, ...) for each matching entity, in driving order
        template<typename F>
        void each(F&& fn){
            // a change filter older than every array skips the type in O(1)
            if (!anyNewer()){
                return;
            }
            ComponentArray<DrivingType>* array = driving();
            array->settle();
            const size_t count = array->size();
            for (size_t i = 0; i < count; i++){
                row(fn, i, std::index_sequence_for<Q...>{});
            }
        }

        // fn(const View::Page&) for each storage page of the driving array, in order; the
        // pages together visit the rows of each(). Pages can go to different jobs.
        template<typename F>
        void forEachPage(F&& fn){
            // a change filter older than every array skips the type in O(1)
            if (!anyNewer()){
                return;
            }
            ComponentArray<DrivingType>* array = driving();
            array->settle();
            const size_t count = array->size();
            const size_t capacity = ComponentArray<DrivingType>::pageCapacity();
            for (size_t first = 0; first < count; first += capacity){
                const size_t last = first + capacity < count ? first + capacity : count;
                fn(Page(this, first, last));
            }
        }

        // the entities each() would visit
        size_t count(){
            size_t total = 0;
            each([&total](Entity, auto&&...){ total++; });
            return total;
        }
    };

}

#endif // ECS_VIEW_H
