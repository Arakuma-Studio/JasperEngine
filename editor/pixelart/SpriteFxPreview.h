// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The sprite material Inspector's Pixel FX helpers (docs/specs/S48-pixel-fx.md R37):
//  - SpriteFxThumbnail: a 40 x 40 offscreen render of a sprite's frame at an integer zoom,
//    Nearest, with one feature of its library:sprite material at the material's values, drawn
//    again only when what it shows changes.
//  - SpriteFxScrub: the Preview of an animated amount (flash, dither fade, dissolve, ramp): the
//    value written straight into the GameObject's material in the scene (Scene and Game views),
//    never through a command, so nothing is saved or dirtied, and put back on stop.

#include "Scene.h"
#include "math/Rect.h"
#include "texture/Material.h"

#include <memory>
#include <string>

namespace jasperengine {
    class Camera;
    class Sprite;
}

namespace jasperengine::editor {

    class SpriteFxThumbnail {
    public:
        static constexpr int SIZE = 40;

        SpriteFxThumbnail();
        ~SpriteFxThumbnail();
        SpriteFxThumbnail(const SpriteFxThumbnail&) = delete;
        SpriteFxThumbnail& operator=(const SpriteFxThumbnail&) = delete;

        // Shows `feature` (with its keyword choice) of `material` on the frame `frameRect` of
        // its base colour texture (frameTexelsX x frameTexelsY texels); renders when any of it
        // changed or the last render still waited for its variant or texture
        void update(const Material& material, const std::string& feature, const Rect& frameRect, int frameTexelsX, int frameTexelsY);
        // what update last asked for has not drawn yet
        bool isPending() const;
        Texture getTexture();

    private:
        Scene* scene = nullptr;
        Camera* camera = nullptr;
        Sprite* sprite = nullptr;
        std::string key;
        bool rendered = false;
    };

    // the Preview of one animated amount of one GameObject's material (one at a time)
    class SpriteFxScrub {
    public:
        // starts (or moves) the preview: `member` of submesh `submesh`'s material shows `value`
        void scrub(Scene* scene, uint32_t sceneId, Entity entity, unsigned int submesh, const std::string& member, float value);
        // plays it from `from` to `to` over `seconds` (the effect's direction)
        void play(Scene* scene, uint32_t sceneId, Entity entity, unsigned int submesh, const std::string& member,
                  float from, float to, float seconds);
        // advances a playing preview; it stops and restores at its end
        void update(float deltaSeconds);
        // puts the material's own value back (its entry removed when it had none)
        void stop();
        // drops the preview without touching the scene (it is closing)
        void forget(){ active = playing = false; scene = nullptr; entity = NULL_ENTITY; member.clear(); }

        bool isActive() const { return active; }
        bool isPlaying() const { return active && playing; }
        uint32_t getSceneId() const { return sceneId; }
        Entity getEntity() const { return entity; }
        const std::string& getMember() const { return member; }
        float getValue() const { return value; }

    private:
        bool active = false;
        bool playing = false;
        Scene* scene = nullptr;
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        unsigned int submesh = 0;
        std::string member;
        bool hadValue = false;
        Vector4 original;
        float value = 0.0f;
        float from = 0.0f, to = 0.0f, seconds = 1.0f, elapsed = 0.0f;

        void write(float shown);
    };

}
