// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Adapted from OpenEngine (MIT), Copyright (c) 2026 Gamecrafters Guild Limited and contributors
// (Engine/Include/Components/Rendering/LightPhotometry.h: the Tanner-Helland black body fit and the
// lumen-to-candela solid angle; the exposure is our own, see exposureScale)

#ifndef LIGHTPHOTOMETRY_H
#define LIGHTPHOTOMETRY_H

#include "component/LightComponent.h"
#include "math/Vector3.h"

#include <algorithm>
#include <cmath>

namespace jasperengine{

    // Photometric light units and colour temperature, resolved on the CPU into the unitless
    // intensity and linear colour the shaders always took (LightComponent::resolvedIntensity,
    // resolvedColor): the GPU path is the same for every unit, and a light in ENGINE units
    // without temperature is drawn exactly as before.
    //
    // The renderer's target is 8-bit, with exposure baked into the colour of every shader, so
    // physical values are pre-exposed (Lagarde): a white Lambertian lit by E lux has luminance
    // E / pi, and the built-in Lit outputs I / pi for an intensity I, so I = E * exposureScale is
    // what a camera at that EV100 records. A 100 000 lx sun at EV100 15 (sunny 16 rule) is 2.54.
    namespace LightPhotometry{

        const float FOUR_PI = 12.566370614359172f;
        const float MIN_TEMPERATURE = 1000.0f;
        const float MAX_TEMPERATURE = 20000.0f;
        // the temperature drawn white (the display's D65 white point)
        const float WHITE_TEMPERATURE = 6500.0f;

        // Tanner-Helland's fit of a black body's sRGB colour (0..255 per channel), 1000..40000 K
        inline Vector3 blackBodySRGB(float kelvin){
            const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
            auto saturate = [](float v){ return std::clamp(v, 0.0f, 255.0f); };
            float r, g, b;
            if (t <= 66.0f){
                r = 255.0f;
                g = saturate(99.4708025861f * std::log(t) - 161.1195681661f);
            }else{
                r = saturate(329.698727446f * std::pow(t - 60.0f, -0.1332047592f));
                g = saturate(288.1221695283f * std::pow(t - 60.0f, -0.0755148492f));
            }
            if (t >= 66.0f)
                b = 255.0f;
            else if (t <= 19.0f)
                b = 0.0f;
            else
                b = saturate(138.5177312231f * std::log(t - 10.0f) - 305.0447927307f);
            return Vector3(r, g, b);
        }

        inline float sRGBToLinear(float c){
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        inline Vector3 blackBodyLinear(float kelvin){
            const Vector3 srgb = blackBodySRGB(kelvin) / 255.0f;
            return Vector3(sRGBToLinear(srgb.x), sRGBToLinear(srgb.y), sRGBToLinear(srgb.z));
        }

        // A colour temperature (Kelvin, clamped to 1000..20000) as a linear colour whose largest
        // channel is 1: a tint, not a brightness. White balanced to WHITE_TEMPERATURE, so 6500 K is
        // exactly white (a light's default temperature does not tint it), lower is warmer, higher
        // bluer.
        inline Vector3 kelvinToLinearRGB(float kelvin){
            const float clamped = std::clamp(std::isfinite(kelvin) ? kelvin : WHITE_TEMPERATURE, MIN_TEMPERATURE, MAX_TEMPERATURE);
            const Vector3 color = blackBodyLinear(clamped);
            const Vector3 white = blackBodyLinear(WHITE_TEMPERATURE);
            Vector3 balanced(color.x / white.x, color.y / white.y, color.z / white.z);
            const float maxChannel = std::max(balanced.x, std::max(balanced.y, balanced.z));
            return maxChannel > 1e-6f ? balanced / maxChannel : Vector3(1.0f, 1.0f, 1.0f);
        }

        // what one physical unit is in engine units at a camera exposure (EV100)
        inline float exposureScale(float ev100){
            return 1.0f / (1.2f * std::exp2(ev100));
        }

        // whether a light of the type can be authored in the unit
        inline bool isUnitValid(LightUnit unit, LightType type){
            if (unit == LightUnit::ENGINE)
                return true;
            if (type == LightType::DIRECTIONAL)
                return unit == LightUnit::LUX;
            return unit == LightUnit::CANDELA || unit == LightUnit::LUMEN;
        }

        // the unit a light of the type takes for one it cannot (lux and candela swap: lux is
        // candela at 1 m, the same number)
        inline LightUnit validUnit(LightUnit unit, LightType type){
            if (isUnitValid(unit, type))
                return unit;
            return type == LightType::DIRECTIONAL ? LightUnit::LUX : LightUnit::CANDELA;
        }

        // A value in a unit as engine units. A lumen is spread over the whole sphere (4 pi), also
        // for a spot, so the cone's angles do not change its brightness (Unity's "reflector" off).
        inline float toEngineIntensity(float value, LightUnit unit, float ev100){
            switch (unit){
                case LightUnit::LUX:
                case LightUnit::CANDELA: return value * exposureScale(ev100);
                case LightUnit::LUMEN:   return value / FOUR_PI * exposureScale(ev100);
                default:                 return value;
            }
        }

        // toEngineIntensity's inverse: the value in a unit that draws as `engine`
        inline float fromEngineIntensity(float engine, LightUnit unit, float ev100){
            switch (unit){
                case LightUnit::LUX:
                case LightUnit::CANDELA: return engine / exposureScale(ev100);
                case LightUnit::LUMEN:   return engine * FOUR_PI / exposureScale(ev100);
                default:                 return engine;
            }
        }

        // The intensity in another unit that keeps the light as bright (an invalid target unit for
        // the light's type takes its valid neighbour, validUnit)
        inline float convertIntensity(float value, LightUnit from, LightUnit to, float ev100){
            return fromEngineIntensity(toEngineIntensity(value, from, ev100), to, ev100);
        }

        // Fills resolvedColor and resolvedIntensity from the authored values, only when they or
        // the EV100 changed since the last call (true then). A light in ENGINE units without
        // temperature is copied as it is.
        inline bool resolveLight(LightComponent& light, float ev100){
            LightComponent::ResolvedKey& key = light.resolvedKey;
            if (key.intensity == light.intensity && key.color == light.color && key.intensityUnit == light.intensityUnit &&
                    key.useColorTemperature == light.useColorTemperature && key.type == light.type &&
                    (!light.useColorTemperature || key.colorTemperature == light.colorTemperature) &&
                    (light.intensityUnit == LightUnit::ENGINE || key.ev100 == ev100)){
                return false;
            }
            key.intensity = light.intensity;
            key.color = light.color;
            key.intensityUnit = light.intensityUnit;
            key.useColorTemperature = light.useColorTemperature;
            key.colorTemperature = light.colorTemperature;
            key.type = light.type;
            key.ev100 = ev100;

            if (light.intensityUnit == LightUnit::ENGINE && !light.useColorTemperature){
                light.resolvedColor = light.color;
                light.resolvedIntensity = light.intensity;
                return true;
            }
            light.resolvedColor = light.useColorTemperature ? light.color * kelvinToLinearRGB(light.colorTemperature) : light.color;
            light.resolvedIntensity = toEngineIntensity(light.intensity, validUnit(light.intensityUnit, light.type), ev100);
            return true;
        }

        // Changes a light's unit; with preserveBrightness its intensity is converted so it draws
        // the same (else the number stays and means the new unit). An invalid unit for the type
        // takes its valid neighbour.
        inline void setIntensityUnit(LightComponent& light, LightUnit unit, bool preserveBrightness, float ev100){
            const LightUnit target = validUnit(unit, light.type);
            if (preserveBrightness)
                light.intensity = convertIntensity(light.intensity, validUnit(light.intensityUnit, light.type), target, ev100);
            light.intensityUnit = target;
        }

        inline const char* getUnitName(LightUnit unit){
            switch (unit){
                case LightUnit::LUX:     return "Lux";
                case LightUnit::CANDELA: return "Candela";
                case LightUnit::LUMEN:   return "Lumen";
                default:                 return "Engine";
            }
        }
    }

}

#endif //LIGHTPHOTOMETRY_H
