// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_LEGACYPARTICLEFIXTURES_H
#define TESTS_LEGACYPARTICLEFIXTURES_H

// The twelve old particle systems of the migration test (docs/specs/S20-particles.md AC13):
// every initializer, modifier, shape, burst, gradient and sprite case, with min == max
// initializers so the old system's rand() decides nothing. `configure` fills any struct with
// the old fields (the old ParticleSystemComponent, LegacyParticleSystem). Their trajectories,
// recorded from the old ActionSystem at 1/60 s steps before it was removed, are in
// fixtures/particles/legacy/<name>.csv. Include the old types first (particles/LegacyParticleSystem.h).

#include "math/Quaternion.h"
#include "math/Rect.h"
#include "math/Vector3.h"
#include "util/Angle.h"

#include <cstdint>
#include <string>
#include <vector>

namespace particletest {

    struct LegacyFixture {
        const char* name;
        bool mesh = false;                    // an InstancedMesh target (else Points)
        jasperengine::Vector3 position = jasperengine::Vector3(0, 0, 0);   // the target's transform
        jasperengine::Quaternion rotation;
        jasperengine::Vector3 scale = jasperengine::Vector3(1, 1, 1);
        std::vector<jasperengine::Rect> frames;   // the target's sprite rects
    };

    inline const std::vector<LegacyFixture>& legacyFixtures(){
        using namespace jasperengine;
        static const std::vector<LegacyFixture> fixtures = [](){
            std::vector<LegacyFixture> f;
            f.push_back({"rate_box"});
            f.push_back({"acceleration_max_per_update"});
            LegacyFixture bursts{"bursts_world_space"};
            bursts.position = Vector3(1, 2, 3);
            f.push_back(bursts);
            f.push_back({"position_modifier"});
            f.push_back({"velocity_modifier"});
            f.push_back({"acceleration_modifier"});
            f.push_back({"color_alpha"});
            f.push_back({"gradient_size"});
            LegacyFixture sprite{"sprite_frames"};
            sprite.frames = {Rect(0, 0, 0.5f, 0.5f), Rect(0.5f, 0, 0.5f, 0.5f), Rect(0, 0.5f, 0.5f, 0.5f), Rect(0.5f, 0.5f, 0.5f, 0.5f)};
            f.push_back(sprite);
            f.push_back({"rotation"});
            LegacyFixture shapes{"shapes_local_space"};
            shapes.position = Vector3(0, 1, 0);
            shapes.rotation = Quaternion(45.0f, Vector3(0, 0, 1));
            shapes.scale = Vector3(2, 2, 2);
            f.push_back(shapes);
            LegacyFixture mesh{"mesh_scale_rotation"};
            mesh.mesh = true;
            mesh.position = Vector3(0, 0, -2);
            f.push_back(mesh);
            return f;
        }();
        return fixtures;
    }

    // the old settings of fixture `index`
    template<class P>
    void configureLegacy(size_t index, P& p){
        using namespace jasperengine;
        p.maxParticles = 300;
        p.loop = true;
        p.rate = 10;
        p.maxPerUpdate = 100;
        p.lifeInitializer.minLife = p.lifeInitializer.maxLife = 1.0f;
        p.positionInitializer.shape = ParticleEmitterShape::Box;
        p.positionInitializer.minPosition = p.positionInitializer.maxPosition = Vector3(0, 1, 0);
        p.velocityInitializer.minVelocity = p.velocityInitializer.maxVelocity = Vector3(1, 2, 0);
        p.sizeInitializer.minSize = p.sizeInitializer.maxSize = 1.0f;
        auto window = [](auto& m, float from, float to, EaseType ease){
            m.fromTime = from;
            m.toTime = to;
            m.function.setType(ease);
        };
        switch (index){
            case 0:  // rate_box: the defaults above
                break;
            case 1:  // acceleration_max_per_update: 200/s clamped to 2 a frame (the excess is lost)
                p.rate = 200;
                p.maxPerUpdate = 2;
                p.lifeInitializer.minLife = p.lifeInitializer.maxLife = 0.5f;
                p.velocityInitializer.minVelocity = p.velocityInitializer.maxVelocity = Vector3(2, 5, 0);
                p.accelerationInitializer.minAcceleration = p.accelerationInitializer.maxAcceleration = Vector3(0, -9.8f, 0);
                break;
            case 2:  // bursts_world_space: two bursts and no rate, the target moved
                p.rate = 0;
                p.localSpace = false;
                p.bursts = {{0.0f, 3, 3}, {0.505f, 2, 2}};
                p.lifeInitializer.minLife = p.lifeInitializer.maxLife = 1.5f;
                p.velocityInitializer.minVelocity = p.velocityInitializer.maxVelocity = Vector3(0, 1, 0);
                break;
            case 3:  // position_modifier
                p.rate = 4;
                p.positionModifier.fromPosition = Vector3(0, 0, 0);
                p.positionModifier.toPosition = Vector3(2, 0, 0);
                window(p.positionModifier, 0.2f, 0.8f, EaseType::QUAD_IN);
                break;
            case 4:  // velocity_modifier
                p.rate = 4;
                p.velocityModifier.fromVelocity = Vector3(0, 0, 0);
                p.velocityModifier.toVelocity = Vector3(0, 4, 0);
                window(p.velocityModifier, 0.0f, 1.0f, EaseType::LINEAR);
                break;
            case 5:  // acceleration_modifier over an acceleration initializer
                p.rate = 4;
                p.accelerationInitializer.minAcceleration = p.accelerationInitializer.maxAcceleration = Vector3(0, -1, 0);
                p.accelerationModifier.fromAcceleration = Vector3(0, 0, 0);
                p.accelerationModifier.toAcceleration = Vector3(3, 0, 0);
                window(p.accelerationModifier, 0.1f, 0.6f, EaseType::QUAD_OUT);
                break;
            case 6:  // color_alpha
                p.rate = 4;
                p.colorInitializer.minColor = p.colorInitializer.maxColor = Vector3(0.5f, 0.2f, 0.1f);
                p.colorModifier.fromColor = Vector3(0.5f, 0.2f, 0.1f);
                p.colorModifier.toColor = Vector3(1, 1, 0);
                window(p.colorModifier, 0.0f, 1.0f, EaseType::CUBIC_IN_OUT);
                p.alphaInitializer.minAlpha = p.alphaInitializer.maxAlpha = 0.8f;
                p.alphaModifier.fromAlpha = 1.0f;
                p.alphaModifier.toAlpha = 0.0f;
                window(p.alphaModifier, 0.5f, 1.0f, EaseType::LINEAR);
                break;
            case 7:  // gradient_size
                p.rate = 4;
                p.colorGradient.stops = {{0.0f, Vector3(1, 0, 0)}, {0.4f, Vector3(1, 1, 0)}, {1.0f, Vector3(0, 0, 1)}};
                p.sizeInitializer.minSize = p.sizeInitializer.maxSize = 2.0f;
                p.sizeModifier.fromSize = 2.0f;
                p.sizeModifier.toSize = 4.0f;
                window(p.sizeModifier, 0.0f, 1.0f, EaseType::SINE_IN);
                break;
            case 8:  // sprite_frames
                p.rate = 4;
                p.spriteInitializer.frames = {2};
                p.spriteModifier.frames = {0, 1, 2, 3};
                window(p.spriteModifier, 0.5f, 1.0f, EaseType::LINEAR);
                break;
            case 9:  // rotation (points take the roll)
                p.rate = 4;
                p.rotationInitializer.minRotation = p.rotationInitializer.maxRotation = Quaternion(30.0f, Vector3(0, 0, 1));
                p.rotationModifier.fromRotation = Quaternion(0.0f, Vector3(0, 0, 1));
                p.rotationModifier.toRotation = Quaternion(170.0f, Vector3(0, 0, 1));
                window(p.rotationModifier, 0.0f, 1.0f, EaseType::LINEAR);
                break;
            case 10:  // shapes_local_space: a cone of height 0 in a rotated, scaled target
                p.rate = 6;
                p.localSpace = true;
                p.positionInitializer.shape = ParticleEmitterShape::Cone;
                p.positionInitializer.coneAngle = 20.0f;
                p.positionInitializer.coneHeight = 0.0f;
                p.velocityInitializer.minVelocity = p.velocityInitializer.maxVelocity = Vector3(0, 1.5f, 0);
                break;
            case 11:  // mesh_scale_rotation: an InstancedMesh target
                p.rate = 4;
                p.scaleInitializer.minScale = p.scaleInitializer.maxScale = Vector3(1, 2, 1);
                p.scaleModifier.fromScale = Vector3(1, 2, 1);
                p.scaleModifier.toScale = Vector3(2, 2, 2);
                window(p.scaleModifier, 0.0f, 1.0f, EaseType::LINEAR);
                p.rotationInitializer.minRotation = p.rotationInitializer.maxRotation = Quaternion(20.0f, Vector3(0, 1, 0));
                p.rotationModifier.fromRotation = Quaternion(0.0f, Vector3(1, 0, 0));
                p.rotationModifier.toRotation = Quaternion(90.0f, Vector3(0, 1, 0));
                window(p.rotationModifier, 0.2f, 0.9f, EaseType::QUAD_IN_OUT);
                p.colorInitializer.minColor = p.colorInitializer.maxColor = Vector3(0.2f, 0.8f, 0.4f);
                break;
        }
    }

    // the CSV columns: frame, slot (the spawn order), age, simulation position, colour, size (x, y, z),
    // rotation quaternion, rotation (points, radians), sprite rect
    inline const char* legacyCsvHeader(){
        return "frame,slot,age,px,py,pz,r,g,b,a,sx,sy,sz,qx,qy,qz,qw,rot,rx,ry,rw,rh";
    }

    inline constexpr int kLegacyFrames = 120;
    inline constexpr double kLegacyStep = 1.0 / 60.0;

}

#endif // TESTS_LEGACYPARTICLEFIXTURES_H
