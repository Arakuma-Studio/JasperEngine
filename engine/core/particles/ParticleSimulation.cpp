// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleSimulation.h"
#include "ParticleNoise.h"
#include "ParticleRandom.h"
#include "ParticleShapes.h"
#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace jasperengine {

    // the over-lifetime curves of the inner loop baked (spec 8), with the module values they
    // were baked from: a change of any of them bakes again
    struct ParticleBaked {
        ParticleVelocityOverLifetimeModule velocity;
        ParticleLimitVelocityOverLifetimeModule limit;
        ParticleInheritVelocityModule inherit;
        ParticleForceOverLifetimeModule force;
        ParticleSizeOverLifetimeModule size;
        ParticleSizeBySpeedModule sizeBySpeed;
        ParticleRotationOverLifetimeModule rotation;
        ParticleRotationBySpeedModule rotationBySpeed;
        ParticleNoiseModule noise;
        ParticleTextureSheetAnimationModule sheet;

        BakedMinMaxCurve velX, velY, velZ, orbX, orbY, orbZ, offX, offY, offZ, radial, speedModifier;
        BakedMinMaxCurve limit1, limitX, limitY, limitZ, drag;
        BakedMinMaxCurve inheritCurve;
        BakedMinMaxCurve forceX, forceY, forceZ;
        BakedMinMaxCurve sizeX, sizeY, sizeZ;
        BakedMinMaxCurve speedSizeX, speedSizeY, speedSizeZ;
        BakedMinMaxCurve rotX, rotY, rotZ;
        BakedMinMaxCurve speedRotX, speedRotY, speedRotZ;
        BakedMinMaxCurve noiseX, noiseY, noiseZ, noisePosition, noiseRotation, noiseSize;
        BakedMinMaxCurve frameOverTime;
        bool hasOrbital = false, hasOffset = false, hasRadial = false;

        bool matches(const ParticleSystemComponent& ps) const{
            return velocity == ps.velocityOverLifetime && limit == ps.limitVelocityOverLifetime && inherit == ps.inheritVelocity &&
                   force == ps.forceOverLifetime && size == ps.sizeOverLifetime && sizeBySpeed == ps.sizeBySpeed &&
                   rotation == ps.rotationOverLifetime && rotationBySpeed == ps.rotationBySpeed && noise == ps.noise &&
                   sheet == ps.textureSheetAnimation;
        }

        void bake(const ParticleSystemComponent& ps){
            velocity = ps.velocityOverLifetime;
            limit = ps.limitVelocityOverLifetime;
            inherit = ps.inheritVelocity;
            force = ps.forceOverLifetime;
            size = ps.sizeOverLifetime;
            sizeBySpeed = ps.sizeBySpeed;
            rotation = ps.rotationOverLifetime;
            rotationBySpeed = ps.rotationBySpeed;
            noise = ps.noise;
            sheet = ps.textureSheetAnimation;

            velX.bake(velocity.x); velY.bake(velocity.y); velZ.bake(velocity.z);
            orbX.bake(velocity.orbitalX); orbY.bake(velocity.orbitalY); orbZ.bake(velocity.orbitalZ);
            offX.bake(velocity.orbitalOffsetX); offY.bake(velocity.orbitalOffsetY); offZ.bake(velocity.orbitalOffsetZ);
            radial.bake(velocity.radial); speedModifier.bake(velocity.speedModifier);
            auto nonZero = [](const MinMaxCurve& c){ return c.maxValue() != 0.0f || c.minValue() != 0.0f; };
            hasOrbital = nonZero(velocity.orbitalX) || nonZero(velocity.orbitalY) || nonZero(velocity.orbitalZ);
            hasOffset = nonZero(velocity.orbitalOffsetX) || nonZero(velocity.orbitalOffsetY) || nonZero(velocity.orbitalOffsetZ);
            hasRadial = nonZero(velocity.radial);
            limit1.bake(limit.limit); limitX.bake(limit.limitX); limitY.bake(limit.limitY); limitZ.bake(limit.limitZ);
            drag.bake(limit.drag);
            inheritCurve.bake(inherit.curve);
            forceX.bake(force.x); forceY.bake(force.y); forceZ.bake(force.z);
            sizeX.bake(size.size); sizeY.bake(size.y); sizeZ.bake(size.z);
            speedSizeX.bake(sizeBySpeed.size); speedSizeY.bake(sizeBySpeed.y); speedSizeZ.bake(sizeBySpeed.z);
            rotX.bake(rotation.x); rotY.bake(rotation.y); rotZ.bake(rotation.z);
            speedRotX.bake(rotationBySpeed.x); speedRotY.bake(rotationBySpeed.y); speedRotZ.bake(rotationBySpeed.z);
            noiseX.bake(noise.strength); noiseY.bake(noise.strengthY); noiseZ.bake(noise.strengthZ);
            noisePosition.bake(noise.positionAmount); noiseRotation.bake(noise.rotationAmount); noiseSize.bake(noise.sizeAmount);
            frameOverTime.bake(sheet.frameOverTime);
        }
    };

}

namespace {

    constexpr float PI = 3.14159265358979323846f;

    std::atomic<uint32_t> autoSeedCounter{0};

    float clamp01(float v){
        if (!(v >= 0.0f)) return 0.0f;
        return v > 1.0f ? 1.0f : v;
    }

    float safeInverse(float v){
        return (v != 0.0f && std::isfinite(v)) ? 1.0f / v : 0.0f;
    }

    Vector3 mul(const Vector3& a, const Vector3& b){
        return Vector3(a.x * b.x, a.y * b.y, a.z * b.z);
    }

    float duration(const ParticleSystemComponent& ps){
        return std::max(0.05f, ps.main.duration);
    }

    // the scale of the simulation frame: positions and speeds of a Local system are in it
    Vector3 simulationScale(const ParticleSystemComponent& ps, const ParticleEmitterFrame& e){
        switch (ps.main.scalingMode){
            case ParticleSystemScalingMode::Hierarchy: return e.worldScale;
            case ParticleSystemScalingMode::Local: return e.localScale;
            case ParticleSystemScalingMode::Shape: return Vector3(1, 1, 1);
        }
        return Vector3(1, 1, 1);
    }

    bool isWorld(const ParticleSystemComponent& ps){
        return ps.main.simulationSpace == ParticleSystemSimulationSpace::World;
    }

    // a vector of the emitter's frame (rotation and simulation scale) in the world
    Vector3 localToWorldVector(const ParticleSystemComponent& ps, const ParticleEmitterFrame& e, const Vector3& v){
        return e.rotation * mul(simulationScale(ps, e), v);
    }

    Vector3 worldToLocalVector(const ParticleSystemComponent& ps, const ParticleEmitterFrame& e, const Vector3& v){
        const Vector3 s = simulationScale(ps, e);
        const Vector3 r = e.rotation.inverse() * v;
        return Vector3(r.x * safeInverse(s.x), r.y * safeInverse(s.y), r.z * safeInverse(s.z));
    }

    // a vector of `space` in the simulation space
    Vector3 toSimulationVector(const ParticleSystemComponent& ps, const ParticleEmitterFrame& e, ParticleSystemSimulationSpace space, const Vector3& v){
        const bool world = isWorld(ps);
        if ((space == ParticleSystemSimulationSpace::World) == world) return v;
        return world ? localToWorldVector(ps, e, v) : worldToLocalVector(ps, e, v);
    }

    Vector3 worldToSimulationVector(const ParticleSystemComponent& ps, const ParticleEmitterFrame& e, const Vector3& v){
        return isWorld(ps) ? v : worldToLocalVector(ps, e, v);
    }

    ParticleEmitterFrame lerpFrame(const ParticleEmitterFrame& a, const ParticleEmitterFrame& b, float f){
        if (f <= 0.0f) return a;
        if (f >= 1.0f) return b;
        ParticleEmitterFrame out = b;
        out.position = a.position + (b.position - a.position) * f;
        out.rotation = Quaternion::slerp(f, a.rotation, b.rotation, true);
        out.worldScale = a.worldScale + (b.worldScale - a.worldScale) * f;
        out.localScale = a.localScale + (b.localScale - a.localScale) * f;
        out.rigidbodyVelocity = a.rigidbodyVelocity + (b.rigidbodyVelocity - a.rigidbodyVelocity) * f;
        return out;
    }

    ParticleEmitterFrame previousFrame(const ParticleSimState& s, const ParticleEmitterFrame& current){
        if (!s.hasEmitter) return current;
        ParticleEmitterFrame prev = current;
        prev.position = s.emitterPosition;
        prev.rotation = s.emitterRotation;
        return prev;
    }

    void rememberEmitter(ParticleSimState& s, const ParticleEmitterFrame& e){
        s.hasEmitter = true;
        s.emitterPosition = e.position;
        s.emitterRotation = e.rotation;
        s.emitterScale = e.worldScale;
    }

    // the rotation taking +Y onto `dir` (unit)
    Quaternion rotationFromUp(const Vector3& dir){
        const Vector3 up(0, 1, 0);
        const float d = std::max(-1.0f, std::min(1.0f, up.dotProduct(dir)));
        if (d > 0.999999f) return Quaternion();
        if (d < -0.999999f) return Quaternion(0.0f, 1.0f, 0.0f, 0.0f);   // half a turn around X
        Vector3 axis = up.crossProduct(dir);
        axis.normalize();
        const float half = std::acos(d) * 0.5f;
        const float s = std::sin(half);
        return Quaternion(std::cos(half), axis.x * s, axis.y * s, axis.z * s);
    }

    Quaternion axisAngleRadians(const Vector3& axis, float angle){
        const float s = std::sin(angle * 0.5f);
        return Quaternion(std::cos(angle * 0.5f), axis.x * s, axis.y * s, axis.z * s);
    }

    ParticleBaked& baked(ParticleSystemComponent& ps){
        std::shared_ptr<ParticleBaked>& data = ps.state.baked.data;
        if (!data){
            data = std::make_shared<ParticleBaked>();
            data->bake(ps);
        }else if (!data->matches(ps)){
            data->bake(ps);
        }
        return *data;
    }

    Pcg32 systemRng(ParticleSimState& s){
        return Pcg32(s.rng, s.rngInc);
    }

    void resetClocks(ParticleSystemComponent& ps){
        ParticleSimState& s = ps.state;
        s.time = 0.0f;
        s.delay = 0.0f;
        s.rateAccumulator = 0.0f;
        s.distanceAccumulator = 0.0f;
        s.loops = 0;
        s.arcPhase = 0.0f;
        s.radiusPhase = 0.0f;
        s.noiseScroll = 0.0f;
        s.bursts.assign(ps.emission.bursts.size(), ParticleBurstState());
    }

    void removeParticle(ParticleSimState& s, size_t from, size_t to){
        s.position[to] = s.position[from];
        s.velocity[to] = s.velocity[from];
        s.totalVelocity[to] = s.totalVelocity[from];
        s.age[to] = s.age[from];
        s.lifetime[to] = s.lifetime[from];
        s.startSize[to] = s.startSize[from];
        s.size[to] = s.size[from];
        s.rotation[to] = s.rotation[from];
        s.rotationSign[to] = s.rotationSign[from];
        s.startColor[to] = s.startColor[from];
        s.color[to] = s.color[from];
        s.seed[to] = s.seed[from];
        s.frame[to] = s.frame[from];
        s.alignRotation[to] = s.alignRotation[from];
    }

    void resizeParticles(ParticleSimState& s, size_t n){
        s.position.resize(n);
        s.velocity.resize(n);
        s.totalVelocity.resize(n);
        s.age.resize(n);
        s.lifetime.resize(n);
        s.startSize.resize(n);
        s.size.resize(n);
        s.rotation.resize(n);
        s.rotationSign.resize(n);
        s.startColor.resize(n);
        s.color.resize(n);
        s.seed.resize(n);
        s.frame.resize(n);
        s.alignRotation.resize(n);
    }

    // drops the dead ones, keeping the spawn order (the age sorts draw in it)
    void compact(ParticleSimState& s){
        size_t write = 0;
        const size_t n = s.count();
        for (size_t read = 0; read < n; read++){
            if (s.age[read] < s.lifetime[read]){
                if (write != read) removeParticle(s, read, write);
                write++;
            }
        }
        if (write != n) resizeParticles(s, write);
    }

    // ------------------------------------------------------------------ per particle

    void updateParticle(ParticleSystemComponent& ps, ParticleBaked& b, size_t i, float dt, const ParticleEmitterFrame& e,
                        const ParticleEnvironment& env, Pcg32& rng){
        ParticleSimState& s = ps.state;
        s.age[i] += dt;
        if (s.age[i] >= s.lifetime[i]){
            return;
        }
        const float t = s.age[i] / s.lifetime[i];
        const uint32_t seed = s.seed[i];
        const float tSys = s.time / duration(ps);
        Vector3& velocity = s.velocity[i];
        Vector3& position = s.position[i];

        // gravity (over the system's duration, as Unity), in the world's down
        const MinMaxCurve& gravityModifier = ps.main.gravityModifier;
        if (gravityModifier.mode != ParticleSystemCurveMode::Constant || gravityModifier.constant != 0.0f){
            const Vector3 g = ps.main.gravitySource == ParticleSystemGravitySource::Physics2D ? env.gravity2D : env.gravity3D;
            velocity += worldToSimulationVector(ps, e, g * gravityModifier.evaluate(tSys, hash01(seed, RANDOM_GRAVITY))) * dt;
        }

        if (ps.forceOverLifetime.enabled){
            const ParticleForceOverLifetimeModule& force = ps.forceOverLifetime;
            const float rx = force.randomized ? rng.next01() : hash01(seed, RANDOM_FORCE_X);
            const float ry = force.randomized ? rng.next01() : hash01(seed, RANDOM_FORCE_Y);
            const float rz = force.randomized ? rng.next01() : hash01(seed, RANDOM_FORCE_Z);
            const Vector3 f(b.forceX.evaluate(t, rx), b.forceY.evaluate(t, ry), b.forceZ.evaluate(t, rz));
            velocity += toSimulationVector(ps, e, force.space, f) * dt;
        }

        if (ps.inheritVelocity.enabled && ps.inheritVelocity.mode == ParticleSystemInheritVelocityMode::Current){
            velocity += worldToSimulationVector(ps, e, s.emitterVelocityDelta) * b.inheritCurve.evaluate(t, hash01(seed, RANDOM_INHERIT));
        }

        Vector3 animated(0, 0, 0);
        float speedModifier = 1.0f;
        if (ps.velocityOverLifetime.enabled){
            const ParticleVelocityOverLifetimeModule& vol = ps.velocityOverLifetime;
            const Vector3 v(b.velX.evaluate(t, hash01(seed, RANDOM_VELOCITY_X)), b.velY.evaluate(t, hash01(seed, RANDOM_VELOCITY_Y)),
                            b.velZ.evaluate(t, hash01(seed, RANDOM_VELOCITY_Z)));
            animated = toSimulationVector(ps, e, vol.space, v);

            if (b.hasOrbital || b.hasRadial){
                // the centre: the emitter (its origin in a Local system) plus the offset
                Vector3 offset(0, 0, 0);
                if (b.hasOffset){
                    offset = toSimulationVector(ps, e, ParticleSystemSimulationSpace::Local,
                        Vector3(b.offX.evaluate(t, hash01(seed, RANDOM_OFFSET_X)), b.offY.evaluate(t, hash01(seed, RANDOM_OFFSET_Y)),
                                b.offZ.evaluate(t, hash01(seed, RANDOM_OFFSET_Z))));
                }
                const Vector3 center = (isWorld(ps) ? e.position : Vector3(0, 0, 0)) + offset;
                if (b.hasOrbital){
                    const Vector3 w(b.orbX.evaluate(t, hash01(seed, RANDOM_ORBITAL_X)), b.orbY.evaluate(t, hash01(seed, RANDOM_ORBITAL_Y)),
                                    b.orbZ.evaluate(t, hash01(seed, RANDOM_ORBITAL_Z)));
                    Quaternion q = eulerRadians(w * dt);
                    if ((vol.space == ParticleSystemSimulationSpace::World) != isWorld(ps)){
                        q = isWorld(ps) ? e.rotation * q * e.rotation.inverse() : e.rotation.inverse() * q * e.rotation;
                    }
                    position = center + q * (position - center);
                }
                if (b.hasRadial){
                    Vector3 out = position - center;
                    const float length = out.length();
                    if (length > 1e-6f){
                        animated += out * (b.radial.evaluate(t, hash01(seed, RANDOM_RADIAL)) / length);
                    }
                }
            }
            speedModifier = b.speedModifier.evaluate(t, hash01(seed, RANDOM_SPEED_MODIFIER));
        }

        if (ps.limitVelocityOverLifetime.enabled){
            const ParticleLimitVelocityOverLifetimeModule& limit = ps.limitVelocityOverLifetime;
            const float keep = std::pow(std::max(0.0f, 1.0f - limit.dampen), dt * 30.0f);
            const bool otherSpace = (limit.space == ParticleSystemSimulationSpace::World) != isWorld(ps);
            Vector3 v = otherSpace ? (isWorld(ps) ? worldToLocalVector(ps, e, velocity) : localToWorldVector(ps, e, velocity)) : velocity;
            if (limit.separateAxes){
                const float lx = std::fabs(b.limitX.evaluate(t, hash01(seed, RANDOM_LIMIT_X)));
                const float ly = std::fabs(b.limitY.evaluate(t, hash01(seed, RANDOM_LIMIT_Y)));
                const float lz = std::fabs(b.limitZ.evaluate(t, hash01(seed, RANDOM_LIMIT_Z)));
                auto clampAxis = [keep](float value, float lim){
                    const float a = std::fabs(value);
                    if (a <= lim) return value;
                    return std::copysign(lim + (a - lim) * keep, value);
                };
                v = Vector3(clampAxis(v.x, lx), clampAxis(v.y, ly), clampAxis(v.z, lz));
            }else{
                const float lim = std::fabs(b.limit1.evaluate(t, hash01(seed, RANDOM_LIMIT)));
                const float speed = v.length();
                if (speed > lim && speed > 0.0f){
                    v = v * ((lim + (speed - lim) * keep) / speed);
                }
            }
            if (otherSpace){
                v = isWorld(ps) ? localToWorldVector(ps, e, v) : worldToLocalVector(ps, e, v);
            }
            velocity = v;

            const float dragValue = b.drag.evaluate(t, hash01(seed, RANDOM_DRAG));
            if (dragValue != 0.0f){
                float d = dragValue;
                if (limit.multiplyDragByParticleSize){
                    const Vector3& sz = s.size[i];
                    d *= std::max(std::fabs(sz.x), std::max(std::fabs(sz.y), std::fabs(sz.z)));
                }
                if (limit.multiplyDragByParticleVelocity){
                    d *= velocity.length();
                }
                velocity = velocity * std::max(0.0f, 1.0f - d * dt);
            }
        }

        Vector3 total = (velocity + animated) * speedModifier;

        Vector3 noiseValue(0, 0, 0);
        if (ps.noise.enabled){
            const ParticleNoiseModule& noise = ps.noise;
            const float frequency = std::max(1e-4f, noise.frequency);
            const Vector3 scroll(s.noiseScroll, s.noiseScroll, s.noiseScroll);
            noiseValue = fbm3(position * frequency + scroll, noise.octaves, noise.octaveMultiplier, noise.octaveScale);
            const float r = hash01(seed, RANDOM_NOISE);
            Vector3 strength;
            if (noise.separateAxes){
                strength = Vector3(b.noiseX.evaluate(t, r), b.noiseY.evaluate(t, hash01(seed, RANDOM_NOISE_Y)), b.noiseZ.evaluate(t, hash01(seed, RANDOM_NOISE_Z)));
            }else{
                const float v = b.noiseX.evaluate(t, r);
                strength = Vector3(v, v, v);
            }
            if (noise.damping) strength = strength / frequency;
            total += mul(noiseValue, strength) * b.noisePosition.evaluate(t, r);
        }

        position += total * dt;
        s.totalVelocity[i] = total;
        const float speed = total.length();

        // rotation (the flipRotation share turns the other way)
        const float sign = s.rotationSign[i];
        Vector3 angular(0, 0, 0);
        if (ps.rotationOverLifetime.enabled){
            const float rz = b.rotZ.evaluate(t, hash01(seed, RANDOM_ROTATION_LIFETIME));
            angular += ps.rotationOverLifetime.separateAxes
                ? Vector3(b.rotX.evaluate(t, hash01(seed, RANDOM_ROTATION_LIFETIME_X)), b.rotY.evaluate(t, hash01(seed, RANDOM_ROTATION_LIFETIME_Y)), rz)
                : Vector3(0, 0, rz);
        }
        if (ps.rotationBySpeed.enabled){
            const Vector2& range = ps.rotationBySpeed.range;
            const float ts = range.y != range.x ? clamp01((speed - range.x) / (range.y - range.x)) : 0.0f;
            const float rz = b.speedRotZ.evaluate(ts, hash01(seed, RANDOM_ROTATION_SPEED));
            angular += ps.rotationBySpeed.separateAxes
                ? Vector3(b.speedRotX.evaluate(ts, hash01(seed, RANDOM_ROTATION_SPEED_X)), b.speedRotY.evaluate(ts, hash01(seed, RANDOM_ROTATION_SPEED_Y)), rz)
                : Vector3(0, 0, rz);
        }
        if (ps.noise.enabled){
            angular.z += noiseValue.x * b.noiseRotation.evaluate(t, hash01(seed, RANDOM_NOISE));
        }
        s.rotation[i] += angular * (sign * dt);

        Vector3 sz = s.startSize[i];
        if (ps.sizeOverLifetime.enabled){
            const float rx = hash01(seed, RANDOM_SIZE_LIFETIME);
            const float x = b.sizeX.evaluate(t, rx);
            sz = mul(sz, ps.sizeOverLifetime.separateAxes
                ? Vector3(x, b.sizeY.evaluate(t, hash01(seed, RANDOM_SIZE_LIFETIME_Y)), b.sizeZ.evaluate(t, hash01(seed, RANDOM_SIZE_LIFETIME_Z)))
                : Vector3(x, x, x));
        }
        if (ps.sizeBySpeed.enabled){
            const Vector2& range = ps.sizeBySpeed.range;
            const float ts = range.y != range.x ? clamp01((speed - range.x) / (range.y - range.x)) : 0.0f;
            const float x = b.speedSizeX.evaluate(ts, hash01(seed, RANDOM_SIZE_SPEED));
            sz = mul(sz, ps.sizeBySpeed.separateAxes
                ? Vector3(x, b.speedSizeY.evaluate(ts, hash01(seed, RANDOM_SIZE_SPEED_Y)), b.speedSizeZ.evaluate(ts, hash01(seed, RANDOM_SIZE_SPEED_Z)))
                : Vector3(x, x, x));
        }
        if (ps.noise.enabled){
            const float amount = b.noiseSize.evaluate(t, hash01(seed, RANDOM_NOISE));
            if (amount != 0.0f) sz = sz * std::max(0.0f, 1.0f + noiseValue.x * amount);
        }
        s.size[i] = sz;

        Vector4 color = s.startColor[i];
        if (ps.colorOverLifetime.enabled){
            color = color * ps.colorOverLifetime.color.evaluate(t, hash01(seed, RANDOM_COLOR_LIFETIME));
        }
        if (ps.colorBySpeed.enabled){
            const Vector2& range = ps.colorBySpeed.range;
            const float ts = range.y != range.x ? clamp01((speed - range.x) / (range.y - range.x)) : 0.0f;
            color = color * ps.colorBySpeed.color.evaluate(ts, hash01(seed, RANDOM_COLOR_SPEED));
        }
        s.color[i] = color;

        const ParticleTextureSheetAnimationModule& sheet = ps.textureSheetAnimation;
        if (sheet.enabled){
            const int frames = textureSheetFrameCount(sheet);
            if (frames > 1){
                float value = 0.0f;
                switch (sheet.timeMode){
                    case ParticleSystemAnimationTimeMode::Lifetime: {
                        float cycleT = t * (float)std::max(1, sheet.cycles);
                        cycleT -= std::floor(cycleT);
                        value = std::floor(b.frameOverTime.evaluate(cycleT, hash01(seed, RANDOM_FRAME)) * (float)frames);
                        break;
                    }
                    case ParticleSystemAnimationTimeMode::Speed: {
                        const Vector2& range = sheet.speedRange;
                        const float ts = range.y != range.x ? clamp01((speed - range.x) / (range.y - range.x)) : 0.0f;
                        value = std::min(std::floor(ts * (float)frames), (float)(frames - 1));
                        break;
                    }
                    case ParticleSystemAnimationTimeMode::FPS:
                        value = std::floor(s.age[i] * std::max(0.0f, sheet.fps) + 1e-4f);
                        break;
                }
                value += std::floor(sheet.startFrame.evaluate(0.0f, hash01(seed, RANDOM_START_FRAME)));
                value = std::fmod(value, (float)frames);
                if (value < 0.0f) value += (float)frames;
                s.frame[i] = value;
            }else{
                s.frame[i] = 0.0f;
            }
        }
    }

    // ------------------------------------------------------------------ spawn

    // one particle at `cycleTime` of the system's clock, from the emitter `e`, aged `age`
    // seconds into the step; false when the system is full or its lifetime is not positive
    bool spawnParticle(ParticleSystemComponent& ps, ParticleBaked& b, float cycleTime, const ParticleEmitterFrame& e, float age,
                       const ShapeContext& context, const ParticleEnvironment& env){
        ParticleSimState& s = ps.state;
        const size_t maxParticles = std::min<unsigned int>(ps.main.maxParticles, 100000u);
        if (s.count() >= maxParticles){
            return false;
        }
        Pcg32 rng = systemRng(s);
        const uint32_t seed = rng.next();
        const float tSys = clamp01(cycleTime / duration(ps));
        const ParticleMainModule& main = ps.main;

        const float lifetime = main.startLifetime.evaluate(tSys, hash01(seed, RANDOM_LIFETIME));
        if (!(lifetime > 0.0f)){
            return false;
        }
        const float speed = main.startSpeed.evaluate(tSys, hash01(seed, RANDOM_SPEED));

        Vector3 size;
        if (main.startSize3D){
            size = Vector3(main.startSize.evaluate(tSys, hash01(seed, RANDOM_SIZE)), main.startSizeY.evaluate(tSys, hash01(seed, RANDOM_SIZE_Y)),
                           main.startSizeZ.evaluate(tSys, hash01(seed, RANDOM_SIZE_Z)));
        }else{
            const float v = main.startSize.evaluate(tSys, hash01(seed, RANDOM_SIZE));
            size = Vector3(v, v, v);
        }

        Vector3 rotation(0, 0, main.startRotation.evaluate(tSys, hash01(seed, RANDOM_ROTATION)));
        if (main.startRotation3D){
            rotation.x = main.startRotationX.evaluate(tSys, hash01(seed, RANDOM_ROTATION_X));
            rotation.y = main.startRotationY.evaluate(tSys, hash01(seed, RANDOM_ROTATION_Y));
        }
        float sign = 1.0f;
        if (main.flipRotation > 0.0f && hash01(seed, RANDOM_FLIP_ROTATION) < main.flipRotation){
            sign = -1.0f;
            rotation = rotation * -1.0f;
        }

        const Vector4 color = main.startColor.evaluate(tSys, hash01(seed, RANDOM_COLOR));

        ShapeSample sample = sampleShape(ps.shape, context, rng);
        if (main.scalingMode == ParticleSystemScalingMode::Shape){
            sample.position = mul(sample.position, e.worldScale);
        }
        Vector3 velocity = sample.direction * speed;
        Vector3 position = sample.position;
        Vector3 direction = sample.direction;

        // the emitter's velocity at spawn
        Vector3 inherited(0, 0, 0);
        if (ps.inheritVelocity.enabled && ps.inheritVelocity.mode == ParticleSystemInheritVelocityMode::Initial){
            inherited = s.emitterVelocity * b.inheritCurve.evaluate(0.0f, hash01(seed, RANDOM_INHERIT));
        }

        if (isWorld(ps)){
            position = e.position + localToWorldVector(ps, e, position);
            velocity = localToWorldVector(ps, e, velocity) + inherited;
            direction = e.rotation * direction;
        }else{
            velocity += worldToLocalVector(ps, e, inherited);
        }

        const size_t i = s.count();
        resizeParticles(s, i + 1);
        s.position[i] = position;
        s.velocity[i] = velocity;
        s.totalVelocity[i] = velocity;
        s.age[i] = 0.0f;
        s.lifetime[i] = lifetime;
        s.startSize[i] = size;
        s.size[i] = size;
        s.rotation[i] = rotation;
        s.rotationSign[i] = sign;
        s.startColor[i] = color;
        s.color[i] = color;
        s.seed[i] = seed;
        s.frame[i] = 0.0f;
        s.alignRotation[i] = ps.shape.alignToDirection ? rotationFromUp(direction) : Quaternion();
        s.spawnCount++;

        // the per-frame values of its first step (size and colour over life at 0, the frame),
        // then the rest of the step it was born in
        updateParticle(ps, b, i, std::max(0.0f, age), e, env, rng);
        return true;
    }

    // `count` particles at `cycleTime` of the clock, `offset` seconds into the substep from
    // its start (the emitter interpolated there) of length `dt`; a burst spreads its arc
    int spawnGroup(ParticleSystemComponent& ps, ParticleBaked& b, int count, float cycleTime, float offset, float dt,
                   const ParticleEmitterFrame& from, const ParticleEmitterFrame& to, const ParticleEnvironment& env, bool burst){
        int spawned = 0;
        ParticleSimState& s = ps.state;
        const float f = dt > 0.0f ? clamp01(offset / dt) : 1.0f;
        const ParticleEmitterFrame e = lerpFrame(from, to, f);
        for (int k = 0; k < count; k++){
            ShapeContext context;
            // the arc / radius position where this particle spawns in the substep
            context.arcPhase = s.arcPhase + s.arcRate * offset;
            context.radiusPhase = s.radiusPhase + s.radiusRate * offset;
            // BurstSpread spreads a burst; the rate's particles pick at random
            context.spreadIndex = k;
            context.spreadCount = burst ? count : 0;
            if (!spawnParticle(ps, b, cycleTime, e, dt - offset, context, env)) break;
            spawned++;
        }
        return spawned;
    }

    void emitOverTime(ParticleSystemComponent& ps, ParticleBaked& b, float dt, const ParticleEmitterFrame& from,
                      const ParticleEmitterFrame& to, const ParticleEnvironment& env){
        ParticleSimState& s = ps.state;
        const float total = duration(ps);
        Pcg32 rng = systemRng(s);

        // the start delay holds the clock and the emission
        float offset = 0.0f;
        if (s.delay > 0.0f){
            const float used = std::min(s.delay, dt);
            s.delay -= used;
            offset = used;
            if (s.delay > 0.0f) return;
        }

        // the cycles this substep covers (a looping system wraps, a finished one stops)
        while (offset < dt && s.emitting){
            const float start = s.time;
            const float remaining = dt - offset;
            float end = start + remaining;
            bool finishing = false;
            if (end >= total){
                end = total;
                finishing = true;
            }
            const float span = end - start;
            const float tSys = clamp01(start / total);

            if (ps.emission.enabled){
                // rate over time: spawns where the accumulated count crosses an integer
                const float rate = std::max(0.0f, ps.emission.rateOverTime.evaluate(tSys, rng.next01()));
                if (rate > 0.0f && span > 0.0f){
                    // the k-th spawn is where the accumulated count reaches k (a hair of tolerance:
                    // a whole number of particles a second over whole seconds comes out exact)
                    const float acc = s.rateAccumulator;
                    const float total = acc + rate * span;
                    const int count = (int)std::floor(total + 1e-4f);
                    for (int k = 1; k <= count; k++){
                        const float at = std::min(span, std::max(0.0f, ((float)k - acc) / rate));
                        spawnGroup(ps, b, 1, start + at, offset + at, dt, from, to, env, false);
                    }
                    s.rateAccumulator = std::max(0.0f, total - (float)count);
                }

                // bursts: each cycle at time + cycle x interval, its probability per cycle
                for (size_t bi = 0; bi < ps.emission.bursts.size(); bi++){
                    if (bi >= s.bursts.size()) s.bursts.resize(ps.emission.bursts.size());
                    const ParticleEmissionBurst& burst = ps.emission.bursts[bi];
                    ParticleBurstState& state = s.bursts[bi];
                    const float interval = std::max(1e-4f, burst.interval);
                    while (burst.cycles <= 0 || state.cyclesDone < burst.cycles){
                        const float fire = burst.time + (float)state.cyclesDone * interval;
                        // a burst at the very end of the cycle fires before it ends (or wraps)
                        const bool inside = fire >= start && (fire < end || (finishing && fire <= end));
                        if (fire < start){
                            state.cyclesDone++;   // a time jump went past it
                            continue;
                        }
                        if (!inside) break;
                        state.cyclesDone++;
                        if (burst.probability < 1.0f && rng.next01() >= burst.probability) continue;
                        const int count = (int)std::floor(std::max(0.0f, burst.count.evaluate(clamp01(fire / total), rng.next01())) + 0.5f);
                        spawnGroup(ps, b, count, fire, offset + (fire - start), dt, from, to, env, true);
                    }
                }
            }

            offset += span;
            s.time = end;
            if (finishing){
                if (ps.main.looping){
                    s.time = 0.0f;
                    s.loops++;
                    for (ParticleBurstState& state : s.bursts) state.cyclesDone = 0;
                    if (span <= 0.0f && remaining <= 0.0f) break;
                }else{
                    s.emitting = false;
                }
            }
            if (span <= 0.0f && !finishing) break;
        }
    }

    void emitOverDistance(ParticleSystemComponent& ps, ParticleBaked& b, float dt, const ParticleEmitterFrame& from,
                          const ParticleEmitterFrame& to, const ParticleEnvironment& env){
        ParticleSimState& s = ps.state;
        if (!ps.emission.enabled || !s.emitting || s.delay > 0.0f) return;
        const MinMaxCurve& rateCurve = ps.emission.rateOverDistance;
        if (rateCurve.mode == ParticleSystemCurveMode::Constant && rateCurve.constant <= 0.0f) return;
        const float distance = (to.position - from.position).length();
        if (distance <= 0.0f) return;
        if (distance > TELEPORT_DISTANCE){
            // a teleport: no burst of thousands (said once per system)
            if (!s.teleportWarned){
                Log::warn("Particle System: the emitter jumped %.1f units in one step; rate over distance spawned nothing for it",
                          distance);
                s.teleportWarned = true;
            }
            return;
        }
        Pcg32 rng = systemRng(s);
        const float tSys = clamp01(s.time / duration(ps));
        const float rate = std::max(0.0f, rateCurve.evaluate(tSys, rng.next01()));
        if (rate <= 0.0f) return;
        // spawns where the travelled distance makes one more, along the path
        const float acc = s.distanceAccumulator;
        const float total = acc + rate * distance;
        const int count = (int)std::floor(total + 1e-4f);
        for (int k = 1; k <= count; k++){
            const float f = std::min(1.0f, std::max(0.0f, ((float)k - acc) / rate / distance));
            spawnGroup(ps, b, 1, s.time, f * dt, dt, from, to, env, false);
        }
        s.distanceAccumulator = std::max(0.0f, total - (float)count);
    }

    void substep(ParticleSystemComponent& ps, ParticleBaked& b, float dt, const ParticleEmitterFrame& from,
                 const ParticleEmitterFrame& to, const ParticleEnvironment& env){
        ParticleSimState& s = ps.state;

        // the emitter's velocity (the world's), for inherit velocity
        Vector3 emitterVelocity = (to.position - from.position) * (dt > 0.0f ? 1.0f / dt : 0.0f);
        if (ps.main.emitterVelocityMode == ParticleSystemEmitterVelocityMode::Rigidbody && to.hasRigidbodyVelocity){
            emitterVelocity = to.rigidbodyVelocity;
        }
        s.emitterVelocityDelta = emitterVelocity - s.emitterVelocity;
        s.emitterVelocity = emitterVelocity;

        // the speeds of the arc / radius modes and the noise scroll (a random range: one value
        // per system play, from its seed)
        const float tSys = clamp01(s.time / duration(ps));
        s.arcRate = ps.shape.arcSpeed.evaluate(tSys, hash01(s.seedUsed, RANDOM_SYSTEM_ARC_SPEED)) * 360.0f;
        s.radiusRate = ps.shape.radiusSpeed.evaluate(tSys, hash01(s.seedUsed, RANDOM_SYSTEM_RADIUS_SPEED));
        if (ps.noise.enabled){
            s.noiseScroll += ps.noise.scrollSpeed.evaluate(tSys, hash01(s.seedUsed, RANDOM_SYSTEM_SCROLL_SPEED)) * dt;
        }

        // the particles alive before this step
        Pcg32 rng = systemRng(s);
        const size_t alive = s.count();
        for (size_t i = 0; i < alive; i++){
            updateParticle(ps, b, i, dt, to, env, rng);
        }

        if (s.playing && s.emitting){
            emitOverTime(ps, b, dt, from, to, env);
            emitOverDistance(ps, b, dt, from, to, env);
        }
        s.arcPhase += s.arcRate * dt;
        s.radiusPhase += s.radiusRate * dt;

        compact(s);
    }

    uint32_t newAutoSeed(){
        return hash32(autoSeedCounter.fetch_add(1u, std::memory_order_relaxed) * 0x9E3779B9U + 0x1234567u);
    }

    void startClock(ParticleSystemComponent& ps, const ParticleEmitterFrame& emitter, const ParticleEnvironment& env, bool keepParticles){
        ParticleSimState& s = ps.state;
        if (!keepParticles) s.clearParticles();
        resetClocks(ps);
        const uint32_t seed = s.fixedSeed ? s.fixedSeedValue : (ps.main.autoRandomSeed ? newAutoSeed() : ps.main.randomSeed);
        s.seedUsed = seed;
        pcgSeed(s.rng, s.rngInc, seed, 54u);
        s.spawnCount = 0;
        s.playing = true;
        s.paused = false;
        s.emitting = true;
        s.prewarmed = false;
        s.reserve(std::min<unsigned int>(ps.main.maxParticles, 100000u));
        rememberEmitter(s, emitter);
        s.emitterVelocity = Vector3(0, 0, 0);
        s.emitterVelocityDelta = Vector3(0, 0, 0);

        // drawn in both cases: a prewarmed system's particles take the same randoms as the
        // same system played cold
        Pcg32 rng = systemRng(s);
        const float delay = std::max(0.0f, ps.main.startDelay.evaluate(0.0f, rng.next01()));
        if (ps.main.looping && ps.main.prewarm){
            // one whole cycle before the first frame, without the start delay
            const int steps = (int)std::ceil(duration(ps) / PREWARM_STEP - 1e-4f);
            ParticleBaked& b = baked(ps);
            for (int i = 0; i < steps; i++){
                substep(ps, b, PREWARM_STEP, emitter, emitter, env);
            }
            s.prewarmed = true;
        }else{
            s.delay = delay;
        }
    }

}

Quaternion particles::eulerRadians(const Vector3& a){
    const Quaternion qx = axisAngleRadians(Vector3(1, 0, 0), a.x);
    const Quaternion qy = axisAngleRadians(Vector3(0, 1, 0), a.y);
    const Quaternion qz = axisAngleRadians(Vector3(0, 0, 1), a.z);
    return qz * (qy * qx);
}

void particles::makeParticleTexture(uint32_t* pixels, int size){
    for (int y = 0; y < size; y++){
        for (int x = 0; x < size; x++){
            const float u = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
            const float v = ((float)y + 0.5f) / (float)size * 2.0f - 1.0f;
            const float r2 = std::min(1.0f, u * u + v * v);
            const float a = (1.0f - r2) * (1.0f - r2);
            const uint32_t alpha = (uint32_t)std::lround(std::min(1.0f, std::max(0.0f, a)) * 255.0f);
            // RGBA bytes in memory (little endian): white, then the alpha
            pixels[y * size + x] = 0x00FFFFFFu | (alpha << 24);
        }
    }
}

float particles::particleRandom(uint32_t seed, uint32_t id){
    return hash01(seed, id);
}

void particles::play(ParticleSystemComponent& ps, const ParticleEmitterFrame& emitter, const ParticleEnvironment& env){
    ParticleSimState& s = ps.state;
    if (s.playing && s.paused){
        s.paused = false;
        return;
    }
    if (s.playing && s.emitting){
        return;
    }
    // stopped, or stopping (emission over, particles living out): emission starts again at 0
    startClock(ps, emitter, env, s.playing);
}

bool particles::stop(ParticleSystemComponent& ps, ParticleSystemStopBehavior behavior){
    ParticleSimState& s = ps.state;
    s.emitting = false;
    s.paused = false;
    if (behavior == ParticleSystemStopBehavior::StopEmittingAndClear){
        s.clearParticles();
    }
    if (s.count() == 0){
        const bool wasPlaying = s.playing;
        s.playing = false;
        return wasPlaying;
    }
    return false;
}

void particles::pause(ParticleSystemComponent& ps){
    if (ps.state.playing){
        ps.state.paused = true;
    }
}

void particles::clear(ParticleSystemComponent& ps){
    ps.state.clearParticles();
}

void particles::reset(ParticleSystemComponent& ps){
    ParticleSimState& s = ps.state;
    s.clearParticles();
    resetClocks(ps);
    s.playing = false;
    s.paused = false;
    s.emitting = false;
    s.prewarmed = false;
    s.hasEmitter = false;
    s.emitterVelocity = Vector3(0, 0, 0);
    s.emitterVelocityDelta = Vector3(0, 0, 0);
}

int particles::emit(ParticleSystemComponent& ps, int count, const ParticleEmitterFrame& emitter, const ParticleEnvironment& env){
    ParticleSimState& s = ps.state;
    if (count <= 0) return 0;
    if (!s.hasEmitter){
        rememberEmitter(s, emitter);
        if (!s.playing){
            // a stopped system that was never played draws from its seed
            const uint32_t seed = s.fixedSeed ? s.fixedSeedValue : (ps.main.autoRandomSeed ? newAutoSeed() : ps.main.randomSeed);
            s.seedUsed = seed;
            pcgSeed(s.rng, s.rngInc, seed, 54u);
        }
    }
    ParticleBaked& b = baked(ps);
    const int spawned = spawnGroup(ps, b, count, s.time, 0.0f, 0.0f, emitter, emitter, env, true);
    compact(s);
    return spawned;
}

bool particles::step(ParticleSystemComponent& ps, float dt, const ParticleEmitterFrame& emitter, const ParticleEnvironment& env){
    ParticleSimState& s = ps.state;
    if (s.paused){
        return false;
    }
    if (!s.playing && s.count() == 0){
        rememberEmitter(s, emitter);
        return false;
    }
    if (!(dt > 0.0f) || !std::isfinite(dt)){
        return false;
    }
    ParticleBaked& b = baked(ps);
    const ParticleEmitterFrame previous = previousFrame(s, emitter);
    const int steps = std::max(1, (int)std::ceil(dt / MAX_SUBSTEP - 1e-5f));
    const float sub = dt / (float)steps;
    for (int i = 0; i < steps; i++){
        const ParticleEmitterFrame from = lerpFrame(previous, emitter, (float)i / (float)steps);
        const ParticleEmitterFrame to = lerpFrame(previous, emitter, (float)(i + 1) / (float)steps);
        substep(ps, b, sub, from, to, env);
    }
    rememberEmitter(s, emitter);

    if (s.playing && !s.emitting && s.count() == 0){
        s.playing = false;
        return true;
    }
    return false;
}

void particles::simulate(ParticleSystemComponent& ps, float t, bool restart, const ParticleEmitterFrame& emitter, const ParticleEnvironment& env){
    if (restart){
        reset(ps);
        play(ps, emitter, env);
    }else if (!ps.state.playing){
        play(ps, emitter, env);
    }
    ps.state.paused = false;
    float left = std::max(0.0f, t);
    while (left > 0.0f){
        const float dt = std::min(left, PREWARM_STEP);
        if (step(ps, dt, emitter, env)) break;
        left -= dt;
    }
    if (ps.state.playing){
        ps.state.paused = true;
    }
}

bool particles::isAlive(const ParticleSystemComponent& ps){
    return ps.state.count() > 0 || (ps.state.playing && ps.state.emitting);
}

size_t particles::particleCount(const ParticleSystemComponent& ps){
    return ps.state.count();
}

Matrix4 particles::simulationToWorld(const ParticleSystemComponent& ps, const ParticleEmitterFrame& e){
    if (isWorld(ps)) return Matrix4();
    return Matrix4::translateMatrix(e.position) * e.rotation.getRotationMatrix() * Matrix4::scaleMatrix(simulationScale(ps, e));
}

Vector3 particles::sizeScale(const ParticleSystemComponent& ps, const ParticleEmitterFrame& e){
    switch (ps.main.scalingMode){
        case ParticleSystemScalingMode::Hierarchy: return e.worldScale;
        case ParticleSystemScalingMode::Local: return e.localScale;
        case ParticleSystemScalingMode::Shape: return Vector3(1, 1, 1);
    }
    return Vector3(1, 1, 1);
}

Vector3 particles::simulationVectorToWorld(const ParticleSystemComponent& ps, const ParticleEmitterFrame& e, const Vector3& v){
    return isWorld(ps) ? v : localToWorldVector(ps, e, v);
}

int particles::textureSheetFrameCount(const ParticleTextureSheetAnimationModule& sheet){
    if (!sheet.enabled) return 1;
    if (sheet.mode == ParticleSystemAnimationMode::Sprites){
        return std::max(1, (int)sheet.sprites.size());
    }
    const int tx = std::max(1, sheet.numTilesX), ty = std::max(1, sheet.numTilesY);
    return sheet.animation == ParticleSystemAnimationType::SingleRow ? tx : tx * ty;
}

Rect particles::textureSheetRect(const ParticleTextureSheetAnimationModule& sheet, int frame, int row){
    const int tx = std::max(1, sheet.numTilesX), ty = std::max(1, sheet.numTilesY);
    int col, r;
    if (sheet.animation == ParticleSystemAnimationType::SingleRow){
        col = ((frame % tx) + tx) % tx;
        r = std::max(0, std::min(row, ty - 1));
    }else{
        const int total = tx * ty;
        const int f = ((frame % total) + total) % total;
        col = f % tx;
        r = f / tx;
    }
    return Rect((float)col / (float)tx, (float)r / (float)ty, 1.0f / (float)tx, 1.0f / (float)ty);
}

int particles::textureSheetRow(const ParticleSystemComponent& ps, size_t index){
    const ParticleTextureSheetAnimationModule& sheet = ps.textureSheetAnimation;
    if (sheet.animation != ParticleSystemAnimationType::SingleRow) return 0;
    if (sheet.rowMode == ParticleSystemAnimationRowMode::Random && index < ps.state.seed.size()){
        const int ty = std::max(1, sheet.numTilesY);
        return std::min(ty - 1, (int)(hash01(ps.state.seed[index], RANDOM_ROW) * (float)ty));
    }
    return sheet.rowIndex;
}
