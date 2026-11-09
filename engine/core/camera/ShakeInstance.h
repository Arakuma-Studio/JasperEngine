// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 7.2)
// - Purpose: one playing shake (MilkShake's ShakeInstance) and the trauma signal, as pure maths:
//   no engine state, no time source. The shaker (CameraControlSystem) steps them with its delta.
// - Thread-safety: plain values: one thread at a time.
// - MilkShake's formulas, exactly:
//   * noise N(x, y) = (perlinNoise(x, y) - 0.5) * 2;
//   * seeds: b (the given seed, or a random integer in [-10000, 10000]), s1 = b/2, s2 = b/3,
//     s3 = b/4; the noise clock t starts at b;
//   * position (N(t + s1, b), N(b, t), N(s3 + t, b + t)) * strength (x) position influence;
//     rotation degrees (N(t - b, s3), N(b, t + s2), N(b + t, s1 + t)) * strength (x) rotation
//     influence, with strength = preset strength * fade * strengthScale;
//   * update(dt) returns the offset of the state before the step, then steps (MilkShake's
//     UpdateShake): the pause fade (paused 0 -> 1 over the pause time), the noise clock
//     t += (1 - paused) * dt * roughness (roughness = preset roughness * fade * roughnessScale,
//     with the fade before this step), then the fade toward 1 (fading in) or 0 (fading out) at
//     dt / time (a time of 0 is instant), clamped to [0, 1]. From FadingIn, fade 1 makes a
//     sustained shake Sustained and a one-shot FadingOut (over the preset fade_out); fade 0 while
//     FadingOut is Stopped (finished: the shaker removes it).
//   * A step with dt <= 0 changes nothing (time scale 0 freezes a scaled shake where it is).
//   * Two deviations from MilkShake, both guards: a zero step never stops a shake that is still
//     fading in (MilkShake stopped a shake started at time scale 0), and only a shake fading in
//     turns Sustained (MilkShake undid a stop() of a sustained shake taken during a zero step).
// - Trauma (Eiserloh): trauma in [0, 1] decays linearly; the amplitude is trauma^power; its own
//   noise clock tau advances by dt * frequency; position max offset (x) (N(tau + 11, sT),
//   N(sT, tau + 23), N(tau + 37, sT + 5)) * k, rotation max angle (x) (N(tau + 41, sT + 3),
//   N(sT + 7, tau + 53), N(tau + 61, sT + 9)) * k.
// - S48 R22: a Kick preset is pixelfx's hit shake (ShakePreset.h), stepped on its own clock
//   (paused like the noise clock); a preset with a tick rate holds its offset between ticks of
//   its clock; a preset in pixel units reports its position in ShakeOffset::pixels, which the
//   shaker converts through the art grid.

#ifndef SHAKE_INSTANCE_H
#define SHAKE_INSTANCE_H

#include "Export.h"
#include "camera/ShakePreset.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstdint>
#include <vector>

namespace jasperengine::camera{

    // MilkShake's ShakeState
    enum class ShakeState : uint8_t{
        FadingIn,
        Sustained,
        FadingOut,
        Stopped
    };

    // a shake's offset: position in units, rotation in Euler degrees (applied Z, then X, then Y),
    // and the position of the presets in pixel units, in art pixels (S48)
    struct JASPERENGINE_API ShakeOffset{
        Vector3 position = Vector3(0, 0, 0);
        Vector3 rotation = Vector3(0, 0, 0);
        Vector3 pixels = Vector3(0, 0, 0);

        ShakeOffset& operator+=(const ShakeOffset& other){
            position += other.position;
            rotation += other.rotation;
            pixels += other.pixels;
            return *this;
        }
    };

    // pixelfx's kick offset (act.js) at `remaining` of `length` seconds: k = strength *
    // remaining / length, f = floor(remaining * tick), (sin(2.4 f) k, cos(3.7 f) k 0.7), each
    // rounded when `round` (pixel units)
    JASPERENGINE_API Vector3 kickOffset(float strength, float remaining, float length, float tick, bool round);

    // MilkShake's getNoise: (perlin(x, y) - 0.5) * 2, in [-1, 1]
    JASPERENGINE_API float shakeNoise(float x, float y);

    // The offset of a shake with base seed `b`, noise clock `t` and current strength `strength`
    // (preset strength * fade * strengthScale)
    JASPERENGINE_API ShakeOffset evaluateShake(const ShakePreset& preset, int baseSeed, float noiseTime, float strength);

    // A uniform random seed in [-10000, 10000]
    JASPERENGINE_API int randomShakeSeed();

    // Unity's localEulerAngles order: Z first, then X, then Y (q = qy * qx * qz), always degrees
    JASPERENGINE_API Quaternion shakeEulerToQuaternion(const Vector3& degrees);

    class JASPERENGINE_API ShakeInstance{
    private:
        ShakePreset preset;
        int baseSeed = 0;
        float noiseTime = 0.0f;

        ShakeState state = ShakeState::FadingIn;
        float fade = 0.0f;
        float fadeInTime = 0.0f;
        float fadeOutTime = 0.0f;

        bool paused = false;
        float pauseAmount = 0.0f;       // 0 running .. 1 paused
        float pauseFadeTime = 0.0f;

        // S48: a kick's clock, and a ticked preset's held offset
        float kickTime = 0.0f;
        float tickClock = 0.0f;
        bool hasHeld = false;
        ShakeOffset held;

        // the offset of the state, in the preset's units (position)
        ShakeOffset rawOffset() const;

    public:
        float strengthScale = 1.0f;
        float roughnessScale = 1.0f;

        ShakeInstance() = default;
        ShakeInstance(const ShakePreset& preset, int seed);

        // MilkShake's UpdateShake: the offset before the step, then the step
        ShakeOffset update(float dt);
        // the offset of the current state
        ShakeOffset getOffset() const;
        void step(float dt);

        // fades out over `fadeTime` (< 0: the preset's fade_out; 0: now). No-op once Stopped
        void stop(float fadeTime = -1.0f);
        // the noise clock slows to a stop over `fadeTime` (0: at once); resume speeds it up again
        void pause(float fadeTime = 0.0f);
        void resume(float fadeTime = 0.0f);

        bool isPaused() const { return paused; }
        bool isFinished() const { return state == ShakeState::Stopped; }
        ShakeState getState() const { return state; }
        float getFade() const { return fade; }
        float getNoiseTime() const { return noiseTime; }
        int getBaseSeed() const { return baseSeed; }
        const ShakePreset& getPreset() const { return preset; }
        float getCurrentStrength() const;
        float getCurrentRoughness() const;
        // overrides the fade times the preset gave (options fadeIn / fadeOut)
        void setFadeTimes(float fadeIn, float fadeOut);
    };

    // A preset played offline (the editor graph, the MCP peaks): the offset at each of
    // `hz` samples per second over `duration` seconds, seed `seed`, a sustained shake stopped at
    // fade_in + 1 s (over its fade_out)
    JASPERENGINE_API std::vector<ShakeOffset> sampleShakePreset(const ShakePreset& preset, float duration, float hz, int seed);
    // the largest |value| of each axis over the samples
    JASPERENGINE_API ShakeOffset shakeOffsetPeaks(const std::vector<ShakeOffset>& samples);

    // The trauma signal of one shaker
    struct JASPERENGINE_API TraumaState{
        float trauma = 0.0f;
        float clock = 0.0f;
        float seed = 0.0f;

        // trauma = clamp(trauma + amount, 0, 1)
        void add(float amount);
        // linear decay, then the clock advances (dt <= 0 changes nothing)
        void step(float dt, float decay, float frequency);
        // the offset at the current trauma and clock
        ShakeOffset getOffset(float power, const Vector3& maxOffset, const Vector3& maxAngle) const;
    };

}

#endif //SHAKE_INSTANCE_H
