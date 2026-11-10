// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteEditEvents.h"

#include <map>

namespace jasperengine::editor::pixelart::draw::SpriteEditEvents {

    namespace {

        struct Listener {
            std::function<void(const SpriteEdited&)> edited;
            std::function<void(const SpriteExternalSave&)> external;
        };

        std::map<const void*, Listener>& listeners() {
            static std::map<const void*, Listener> all;
            return all;
        }

    }

    void addListener(const void* owner, std::function<void(const SpriteEdited&)> edited, std::function<void(const SpriteExternalSave&)> external) {
        listeners()[owner] = {std::move(edited), std::move(external)};
    }

    void removeListener(const void* owner) { listeners().erase(owner); }

    void edited(const SpriteEdited& event) {
        // a copy: a listener may remove itself
        const std::map<const void*, Listener> current = listeners();
        for (const auto& [owner, listener] : current) {
            if (listener.edited) listener.edited(event);
        }
    }

    void externalSave(const SpriteExternalSave& event) {
        const std::map<const void*, Listener> current = listeners();
        for (const auto& [owner, listener] : current) {
            if (listener.external) listener.external(event);
        }
    }

}
