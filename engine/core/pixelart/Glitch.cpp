// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The burst structure follows pixelfx's glitch (src/fx/scifi/glitch.js, MIT, Copyright (c) 2026
// Patch): per corruption frame an RGB split of a few texels with a random side, a handful of
// torn rows of 1-5 rows shifted sideways by up to 16 texels times the strength, interlace on
// 30 % of burst frames, and a lone row slipping 2 texels now and then while calm. The hashes are
// pixel_fx.glsl's (PixelFxKey), and the bursts come from a seeded schedule instead of a table.

#include "pixelart/Glitch.h"

#include "SceneSettings.h"
#include "pixelart/PixelFxKey.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::pixelart {

    namespace {
        float burstRemaining = 0.0f;
        float burstIntensity = 0.0f;

        // salts of the frame's draws (the second hash coordinate)
        enum : uint32_t { R_SPLIT = 1, R_SIDE, R_TEARS, R_ROW, R_HEIGHT, R_SHIFT, R_CALM, R_CALM_ROW, R_CALM_SIDE, R_INTERLACE,
                          R_SLOT_START = 100, R_SLOT_STRENGTH };
        constexpr uint32_t kTearSalt = 16;
    }

    GlitchFrame glitchFrame(const PostProcessingSettings& settings, double time, int sourceRows, float scriptIntensity){
        GlitchFrame out;
        const double fps = std::min(std::max((double)settings.glitchFps, 1.0), 60.0);
        const double framed = std::floor(std::max(time, 0.0) * fps + 1e-6);
        out.frame = (uint32_t)std::fmod(framed, 4294967296.0);
        const uint32_t seed = (uint32_t)std::max(settings.glitchSeed, 0);
        const uint32_t f = out.frame;
        auto r = [&](uint32_t salt){ return pfxHash01(f, salt, seed); };

        // the bursts: one per slot of 1 / rate seconds, at a seeded place inside it
        const float intensity = std::min(std::max(settings.glitchIntensity, 0.0f), 1.0f);
        float strength = 0.0f;
        if (intensity > 0.0f){
            if (settings.glitchBurstRate <= 0.0f){
                strength = intensity;
            }else{
                const double t = framed / fps;
                const double slotLength = 1.0 / std::min((double)settings.glitchBurstRate, 8.0);
                const double length = std::min(std::max((double)settings.glitchBurstLength, 0.05), 1.0);
                const double slotD = std::floor(t / slotLength);
                const uint32_t slot = (uint32_t)std::fmod(slotD, 4294967296.0);
                const double start = slotD * slotLength + pfxHash01(slot, R_SLOT_START, seed) * std::max(slotLength - length, 0.0);
                if (t >= start && t < start + length)
                    strength = intensity * (0.5f + 0.5f * pfxHash01(slot, R_SLOT_STRENGTH, seed));
            }
        }
        strength = std::max(strength, std::min(std::max(scriptIntensity, 0.0f), 1.0f));
        out.strength = strength;

        const int rows = std::max(sourceRows, 1);
        const int maxShift = (int)std::lround(std::min(std::max(settings.glitchTearShift, 0.0f), 32.0f));
        const int tearLimit = std::min(std::max(settings.glitchTears, 0), GlitchFrame::kMaxTears);
        if (strength > 0.0f){
            const float splitMax = std::min(std::max(settings.glitchRgbSplit, 0.0f), 8.0f);
            if (splitMax >= 0.5f){
                const int split = std::max(1, (int)std::lround(splitMax * strength * (0.5f + 0.5f * r(R_SPLIT))));
                out.split = r(R_SIDE) < 0.5f ? -split : split;
            }
            if (tearLimit > 0 && maxShift > 0){
                const int count = std::min(tearLimit, 1 + (int)std::floor(r(R_TEARS) * tearLimit * strength));
                for (int i = 0; i < count; i++){
                    GlitchTear& tear = out.tears[out.tearCount];
                    const uint32_t salt = kTearSalt * (uint32_t)(i + 1);
                    tear.row = std::min((int)std::floor(r(salt + R_ROW) * rows), rows - 1);
                    tear.height = 1 + std::min((int)std::floor(r(salt + R_HEIGHT) * 5.0f), 4);
                    tear.shift = (int)std::lround((r(salt + R_SHIFT) - 0.5f) * 2.0f * maxShift * strength);
                    if (tear.shift != 0)
                        out.tearCount++;
                }
            }
            out.interlace = r(R_INTERLACE) < std::min(std::max(settings.glitchInterlace, 0.0f), 1.0f);
        }else if (intensity > 0.0f && tearLimit > 0 && maxShift > 0 && r(R_CALM) < 0.07f){
            // a lone row slips now and then even when it is calm
            GlitchTear& tear = out.tears[0];
            tear.row = std::min((int)std::floor(r(R_CALM_ROW) * rows), rows - 1);
            tear.height = 1;
            tear.shift = (r(R_CALM_SIDE) < 0.5f ? -1 : 1) * std::min(2, maxShift);
            out.tearCount = 1;
        }
        return out;
    }

    float packGlitchTear(const GlitchTear& tear){
        const int row = std::min(std::max(tear.row, 0), 8191);
        const int height = std::min(std::max(tear.height, 1), 8);
        const int shift = std::min(std::max(tear.shift, -64), 63);
        return (float)(row + 8192 * (height - 1) + 65536 * (shift + 64));
    }

    void Glitch::burst(float seconds, float intensity){
        if (!(seconds > 0.0f) || !(intensity > 0.0f))
            return;
        burstRemaining = std::max(burstRemaining, seconds);
        burstIntensity = std::max(burstIntensity, std::min(intensity, 1.0f));
    }

    void Glitch::stop(){
        burstRemaining = 0.0f;
        burstIntensity = 0.0f;
    }

    float Glitch::getBurstIntensity(){
        return burstRemaining > 0.0f ? burstIntensity : 0.0f;
    }

    float Glitch::getBurstRemaining(){
        return burstRemaining;
    }

    void Glitch::update(double unscaledDt){
        if (burstRemaining <= 0.0f)
            return;
        burstRemaining -= (float)unscaledDt;
        if (burstRemaining <= 0.0f)
            stop();
    }

}
