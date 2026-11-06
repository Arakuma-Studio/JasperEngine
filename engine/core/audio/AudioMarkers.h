// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOMARKERS_H
#define AUDIOMARKERS_H

#include "Export.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace jasperengine {

    class Scene;

    // The gameplay moments of a capture, on its clock beside the voices (docs/specs/S35-agent-audio.md
    // R14), so a check can tell which of them had a sound. Each site, behind
    // JASPERENGINE_AUDIO_CAPTURE, is `if (AudioMarkers::isEnabled())` just before its callbacks;
    // markers never subscribe to a signal, so a game's callbacks see what they saw before.
    class JASPERENGINE_API AudioMarkers {
    public:
#if JASPERENGINE_AUDIO_CAPTURE
        // true only between a capture's begin and end
        static bool isEnabled() {
            return enabled.load(std::memory_order_relaxed);
        }
        static void setEnabled(bool on);
        // the lines written since the capture began
        static uint64_t getCount();

        // speed < 0: none
        static void collision(const Scene* scene, const char* kind, uint32_t entity, uint32_t other, float speed);
        static void sensor(const Scene* scene, const char* kind, uint32_t entity, uint32_t other);
        // ui_press, ui_click, ui_hover: canvas coordinates of the pointer
        static void ui(const Scene* scene, const char* kind, uint32_t entity, float x, float y);
        // the element's id, else its tag path
        static void htmlClick(const Scene* scene, std::string_view element);
        static void animationEvent(const Scene* scene, uint32_t entity, std::string_view function, std::string_view state, double time);
        // prefabPath, guid: empty for a copy of a GameObject
        static void spawn(const Scene* scene, uint32_t root, std::string_view prefabPath, std::string_view guid);
        static void destroy(const Scene* scene, uint32_t root);
        // mode: start, load, add
        static void sceneLoad(std::string_view name, const char* mode);
        // device: key, mouse, touch, gamepad; action: down, up
        static void input(const char* device, const char* action, int code, float x, float y);

    private:
        static std::atomic<bool> enabled;
#else
        static bool isEnabled() { return false; }
#endif
    };

}

#endif //AUDIOMARKERS_H
