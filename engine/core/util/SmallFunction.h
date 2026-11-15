// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace jasperengine {

    template<typename Signature, size_t InlineBytes = 48>
    class SmallFunction;

    // A move-only std::function with room for a callable of up to InlineBytes in itself: a
    // lambda capturing a few pointers or a shared_ptr is stored without allocating. Bigger
    // (or over-aligned, or throwing-move) callables go to the heap.
    template<typename R, typename... Args, size_t InlineBytes>
    class SmallFunction<R(Args...), InlineBytes> {
    public:
        SmallFunction() noexcept = default;
        SmallFunction(std::nullptr_t) noexcept {}

        template<typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, SmallFunction> &&
                                                         std::is_invocable_r_v<R, std::decay_t<F>&, Args...>>>
        SmallFunction(F&& function) {
            using Callable = std::decay_t<F>;
            if constexpr (fitsInline<Callable>()) {
                ::new (static_cast<void*>(&storage)) Callable(std::forward<F>(function));
                ops = &inlineOps<Callable>;
            } else {
                heap() = new Callable(std::forward<F>(function));
                ops = &heapOps<Callable>;
            }
        }

        SmallFunction(SmallFunction&& other) noexcept {
            moveFrom(other);
        }

        SmallFunction& operator=(SmallFunction&& other) noexcept {
            if (this != &other) {
                reset();
                moveFrom(other);
            }
            return *this;
        }

        SmallFunction& operator=(std::nullptr_t) noexcept {
            reset();
            return *this;
        }

        SmallFunction(const SmallFunction&) = delete;
        SmallFunction& operator=(const SmallFunction&) = delete;

        ~SmallFunction() {
            reset();
        }

        R operator()(Args... args) {
            return ops->invoke(&storage, std::forward<Args>(args)...);
        }

        explicit operator bool() const noexcept {
            return ops != nullptr;
        }

        void reset() noexcept {
            if (ops) {
                ops->destroy(&storage);
                ops = nullptr;
            }
        }

        // Whether F is stored without allocating
        template<typename F>
        static constexpr bool fitsInline() {
            return sizeof(F) <= InlineBytes && alignof(F) <= alignof(std::max_align_t) &&
                   std::is_nothrow_move_constructible_v<F>;
        }

    private:
        struct Ops {
            R (*invoke)(void* storage, Args&&... args);
            void (*move)(void* from, void* to) noexcept;
            void (*destroy)(void* storage) noexcept;
        };

        // (std::destroy_at, not ->~F(): MSVC looks ~F up in the class F names, C2300)
        template<typename F>
        static constexpr Ops inlineOps = {
            [](void* storage, Args&&... args) -> R {
                return (*static_cast<F*>(storage))(std::forward<Args>(args)...);
            },
            [](void* from, void* to) noexcept {
                ::new (to) F(std::move(*static_cast<F*>(from)));
                std::destroy_at(static_cast<F*>(from));
            },
            [](void* storage) noexcept {
                std::destroy_at(static_cast<F*>(storage));
            },
        };

        template<typename F>
        static constexpr Ops heapOps = {
            [](void* storage, Args&&... args) -> R {
                return (**static_cast<F**>(storage))(std::forward<Args>(args)...);
            },
            [](void* from, void* to) noexcept {
                *static_cast<F**>(to) = *static_cast<F**>(from);
            },
            [](void* storage) noexcept {
                delete *static_cast<F**>(storage);
            },
        };

        void*& heap() {
            return *reinterpret_cast<void**>(&storage);
        }

        void moveFrom(SmallFunction& other) noexcept {
            if (other.ops) {
                other.ops->move(&other.storage, &storage);
                ops = other.ops;
                other.ops = nullptr;
            }
        }

        alignas(std::max_align_t) unsigned char storage[InlineBytes < sizeof(void*) ? sizeof(void*) : InlineBytes];
        const Ops* ops = nullptr;
    };

}
