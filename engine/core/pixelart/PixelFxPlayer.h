// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PIXELFXPLAYER_H
#define PIXELART_PIXELFXPLAYER_H

// The PixelFX player (docs/specs/S48-pixel-fx.md R24-R26): plays library:sprite's effects over
// time on a GameObject's own material (flash, dithered fades and blinks, dissolve, the ramp
// recolour status looks, an outline pulse). It writes the material's values each frame the
// effect runs, on game time (hit-stop aware: a Hit Stop Exempt GameObject keeps playing) or
// unscaled time, and leaves every value it animates at its neutral 0 when the effect ends (the
// one-way fade_out and dissolve end at 1, the sprite gone). A feature the material lacks is
// turned on in that GameObject's material (never a .material file); the effect waits for the
// variant (S32's asynchronous path) and its progress stays 0 until it is drawn. A dissolve can
// emit S49's pixel particles where the front burns, from the CPU mirror of the shader's key
// (PixelFxKey.h). Nothing is allocated or run while no effect plays.
//
// Main thread only.

#include "Export.h"
#include "action/Ease.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "pixelart/PixelFxKey.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::pixelart{

    enum class PixelFxEffect : uint8_t{
        FLASH,
        FADE_OUT,
        FADE_IN,
        BLINK,
        DISSOLVE,
        UNDISSOLVE,
        PETRIFY,
        FREEZE,
        POISON,
        OUTLINE_PULSE
    };
    constexpr int kPixelFxEffectCount = 10;

    // "flash", "fade_out", ... (the Lua names)
    JASPERENGINE_API const char* pixelFxEffectName(PixelFxEffect effect);
    JASPERENGINE_API bool pixelFxEffectFromName(std::string_view name, PixelFxEffect& out);
    // the library:sprite feature an effect needs (FLASH, DITHER_FADE, DISSOLVE, RAMP_RECOLOR, OUTLINE)
    JASPERENGINE_API const char* pixelFxEffectFeature(PixelFxEffect effect);

    struct JASPERENGINE_API PixelFxOptions{
        EaseType ease = EaseType::LINEAR;
        // on unscaled time: neither Time.timeScale nor a hit-stop slows it
        bool unscaled = false;
        // flash and outline_pulse: the colour (linear), else the material's
        bool hasColor = false;
        Vector4 color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        // blink and outline_pulse: how many (0: 3)
        int count = 0;
        // dissolve and the status looks: the ramp texture (an N x 1 strip or
        // "<palette>.palette#ramp:<name>"); empty keeps the material's
        std::string ramp;
        // dissolve: the Pixel Particles effect embers come from (a preset name such as "ember",
        // a .particles path or an effect's JSON); empty: none
        std::string embers;
        // embers per second at most
        float emberRate = 40.0f;
        // petrify, freeze, poison: seconds held at full strength between in and out (< 0: half
        // the duration)
        float hold = -1.0f;
        // the contact sheet's: time moves only through PixelFxPlayer::seek (no embers)
        bool manual = false;
        // once, when the effect ends by itself (not on stop() or when another replaces it)
        std::function<void()> onFinished;
    };

    struct PixelFxRun;

    // A playing effect (Lua's PixelFXHandle): yieldable through isDone
    class JASPERENGINE_API PixelFxHandle{
    public:
        PixelFxHandle() = default;
        explicit PixelFxHandle(std::shared_ptr<PixelFxRun> run);

        // false for the handle of a call that played nothing
        bool isValid() const;
        // ended by itself, stopped, replaced, or its GameObject went
        bool isDone() const;
        // 0..1 of its duration; 0 while its variant is not ready
        float getProgress() const;
        // ends it now, its values back at their neutral 0 (no onFinished)
        void stop();
        // replaces the callback called once when it ends by itself
        void setOnFinished(std::function<void()> callback);
        // a slot the Lua binding keeps the script's callback in (so it can be read back)
        std::shared_ptr<void>& scriptData();

        const std::shared_ptr<PixelFxRun>& getRun() const { return run; }

    private:
        std::shared_ptr<PixelFxRun> run;
    };

    // one material value an effect writes at a time
    struct JASPERENGINE_API PixelFxValue{
        const char* member;
        Vector4 value;
    };

    // a sprite frame's opacity (alpha >= 0.5, as library:sprite counts it), rows from the top
    struct JASPERENGINE_API PixelFxFrameAlpha{
        int width = 0;
        int height = 0;
        std::vector<uint8_t> opaque;
    };

    // a frame texel: x right, row down from the frame's top (the image's rows)
    struct JASPERENGINE_API PixelFxTexel{
        int x = 0;
        int row = 0;
    };

    class JASPERENGINE_API PixelFxPlayer{
    public:
        // Plays `effect` for `seconds` on the GameObject's library:sprite submeshes (a Sprite
        // whose material has no shader takes library:sprite). A running effect of the same
        // feature on it is replaced. An invalid handle with `error` when the GameObject has no
        // such material.
        static PixelFxHandle play(Scene* scene, Entity entity, PixelFxEffect effect, float seconds,
                                  const PixelFxOptions& options = PixelFxOptions(), std::string* error = nullptr);
        // Sets a value directly: a channel ("flash", "fade", "dissolve", "ramp") or any
        // library:sprite property; it stops a running effect of that feature on the GameObject
        // and turns the feature on when needed
        static bool set(Scene* scene, Entity entity, const std::string& name, const Vector4& value, std::string* error = nullptr);

        // Engine::runFrame, after the scenes' update: the game delta (hit-stop aware per
        // GameObject through HitStop::deltaFor) and the unscaled one
        static void update(double deltaTime, double unscaledDeltaTime);
        // the effects on a GameObject at `seconds` from their start, their values written now
        // (capture_contact_sheet's frames)
        static void seek(Scene* scene, Entity entity, double seconds);
        // the effects not ended yet
        static size_t getActiveCount();
        // Play stop and engine shutdown: every effect and its callback dropped
        static void reset();

        // The values an effect writes at `seconds` (pure: the contact sheet, the tests). The
        // members are library:sprite's.
        static std::vector<PixelFxValue> sample(PixelFxEffect effect, float duration, const PixelFxOptions& options, double seconds);

        // Embers: where a dissolve's front burns from p0 to p1, over a frame's opaque texels,
        // in row-major order (the burn of library:sprite: a key below the front)
        static std::vector<PixelFxTexel> emberTexels(const PixelFxFrameAlpha& frame, const PfxDissolve& dissolve, float p0, float p1);
        // The emitter embers go to (tests replace it; default: particles::PixelParticles::emitAt)
        using EmberEmitter = std::function<void(Scene* scene, const std::string& effect, const std::vector<Vector3>& positions)>;
        static void setEmberEmitter(EmberEmitter emitter);
        // Turns an embers name (opts.embers) into what emitAt reads; the editor resolves the Pixel
        // Particles presets ("ember") to their effect for the project. Unset or empty result: the
        // name is used as it is (a .particles path or JSON).
        using EmberResolver = std::function<std::string(const std::string& name)>;
        static void setEmberResolver(EmberResolver resolver);
        // the frame alpha of a texture rect (normalized x, y, w, h from the top-left), read once
        // per texture and rect and cached; empty when the texture's pixels cannot be read (every
        // texel then counts as opaque)
        static std::shared_ptr<const PixelFxFrameAlpha> frameAlpha(const std::string& texturePath, float x, float y, float w, float h);
    };

}

#endif // PIXELART_PIXELFXPLAYER_H
