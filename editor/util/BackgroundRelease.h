// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "yaml-cpp/yaml.h"

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    // Drops references on a background thread. A YAML tree of a thousand GameObjects takes
    // about as long to free as to build; handed here, the undo step or the paste that let
    // go of it does not wait for that.
    //
    // Only for data no other thread uses and whose destruction has no side effects beyond
    // freeing memory (YAML nodes, clipboard snapshots): what the main thread still holds
    // stays alive, since the worker only drops the reference it was given.
    class BackgroundRelease {
    public:
        // Takes the value over (moved in: a shared_ptr, a container of nodes)
        template<typename T>
        static void release(T&& value) {
            using Value = std::remove_cv_t<std::remove_reference_t<T>>;
            static_assert(!std::is_lvalue_reference<T>::value, "move the value in");
            // Type-erased owner; destroying it destroys the value
            push(std::make_shared<Holder<Value>>(std::move(value)));
        }

        // A YAML::Node copies instead of moving, and assigning to it writes through to the
        // node it refers to: this rebinds `node` to nothing, leaving the worker the only
        // reference it had
        static void release(YAML::Node& node) {
            std::vector<YAML::Node> nodes(1);
            nodes[0].reset(node);
            node.reset(YAML::Node());
            release(std::move(nodes));
        }

    private:
        struct HolderBase {
            virtual ~HolderBase() = default;
        };
        template<typename T>
        struct Holder : HolderBase {
            T value;
            explicit Holder(T v) : value(std::move(v)) {}
        };

        static void push(std::shared_ptr<HolderBase> holder);
    };

}
