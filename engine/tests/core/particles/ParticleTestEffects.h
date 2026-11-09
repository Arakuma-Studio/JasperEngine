// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_PARTICLETESTEFFECTS_H
#define TESTS_PARTICLETESTEFFECTS_H

// Effects the particle tests and je-bench-particles share: the spec's reference stack
// (docs/specs/S20-particles.md, Performance) and a document with every module set.

#include "particles/ParticleEffect.h"

namespace particletest {

    using namespace jasperengine;
    using namespace jasperengine::particles;

    // Rate + Cone shape, Start Lifetime/Speed/Size/Color ranges, gravity, Limit Velocity (drag),
    // Noise High (1 octave), Color + Size over Lifetime (curves), Texture Sheet 8x8 with frame
    // blending, Billboard, additive (no sort). `rate` per second, lifetimes 1.5 to 2.5 s.
    inline ParticleEffectDocument referenceStack(bool noise, float rate = 50000.0f, uint32_t maxParticles = 1u << 17){
        ParticleEffectDocument d;
        d.maxParticles = maxParticles;
        d.duration = 5.0f;
        d.looping = true;
        d.simulationSpace = SimulationSpace::World;
        d.seed = 12345;
        d.main.startLifetime = MinMaxCurve::twoConstants(1.5f, 2.5f);
        d.main.startSpeed = MinMaxCurve::twoConstants(2.0f, 4.0f);
        d.main.startSize = MinMaxCurve::twoConstants(0.05f, 0.15f);
        d.main.startColor.mode = GradientValueMode::TwoColors;
        d.main.startColor.color = Vector4(1.0f, 0.6f, 0.2f, 1.0f);
        d.main.startColor.colorMax = Vector4(1.0f, 0.9f, 0.5f, 1.0f);
        d.main.gravityModifier = MinMaxCurve(0.3f);
        d.emission.rateOverTime = MinMaxCurve(rate);
        d.shape.shape = ShapeType::Cone;
        d.shape.emitFrom = ShapeEmitFrom::Base;
        d.shape.radius = 0.5f;
        d.shape.angle = 20.0f;
        d.limitVelocity.present = true;
        d.limitVelocity.drag = MinMaxCurve(0.5f);
        d.limitVelocity.multiplyBySize = false;
        d.limitVelocity.limit = MinMaxCurve(100.0f);
        if (noise){
            d.noise.present = true;
            d.noise.strength = MinMaxCurve(1.0f);
            d.noise.frequency = 0.5f;
            d.noise.octaves = 1;
            d.noise.quality = NoiseQuality::High;
        }
        d.colorOverLifetime.present = true;
        d.colorOverLifetime.color.mode = GradientValueMode::Gradient;
        d.colorOverLifetime.color.gradient = Gradient::twoColors(Vector3(1.0f, 1.0f, 1.0f), Vector3(1.0f, 0.3f, 0.1f), 1.0f, 0.0f);
        d.sizeOverLifetime.present = true;
        d.sizeOverLifetime.size = MinMaxCurve::fromCurve(Curve::polyline({{0.0f, 0.2f}, {0.3f, 1.0f}, {1.0f, 0.5f}}));
        d.textureSheetAnimation.present = true;
        d.textureSheetAnimation.tilesX = 8;
        d.textureSheetAnimation.tilesY = 8;
        d.textureSheetAnimation.frameBlending = true;
        d.renderer.renderMode = RenderMode::Billboard;
        d.renderer.blend = ParticleBlend::Additive;
        return d;
    }

    // every module present with values that are not the defaults (and valid together)
    inline ParticleEffectDocument everyModule(){
        ParticleEffectDocument d;
        d.duration = 3.5f;
        d.looping = false;
        d.startDelay = 0.25f;
        d.simulationSpace = SimulationSpace::World;
        d.tickRate = 60;
        d.interpolate = false;
        d.maxParticles = 5000;
        d.seed = 99;
        d.scalingMode = ScalingMode::Shape;
        d.simulationSpeed = 1.5f;
        d.cullingMode = CullingMode::AlwaysSimulate;
        d.boundsMode = BoundsMode::Authored;
        d.boundsCenter = Vector3(1.0f, 2.0f, 3.0f);
        d.boundsSize = Vector3(4.0f, 5.0f, 6.0f);
        d.priority = 3;
        d.lod.enabled = true;
        d.lod.emissionScale = MinMaxCurve::fromCurve(Curve::linear(1.0f, 0.2f));
        d.lod.renderThinning = MinMaxCurve::fromCurve(Curve::linear(1.0f, 0.5f));
        d.lod.maxDistance = 80.0f;

        MinMaxCurve twoCurves;
        twoCurves.mode = CurveMode::TwoCurves;
        twoCurves.curve = Curve::linear(0.0f, 1.0f);
        twoCurves.curveMax = Curve::easeInOut(0.0f, 0.5f, 1.0f, 2.0f);
        twoCurves.multiplier = 2.0f;
        twoCurves.driver = CurveDriver::Age;
        twoCurves.driverMin = 0.0f;
        twoCurves.driverMax = 3.0f;
        twoCurves.wrap = CurveWrap::PingPong;

        MinMaxGradient twoGradients;
        twoGradients.mode = GradientValueMode::TwoGradients;
        twoGradients.gradient = Gradient::twoColors(Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), 1.0f, 0.5f);
        twoGradients.gradientMax = Gradient::twoColors(Vector3(0.0f, 0.0f, 1.0f), Vector3(1.0f, 1.0f, 0.0f));
        twoGradients.gradientMax.mode = GradientMode::Fixed;
        twoGradients.gradientMax.linear = true;

        d.main.startLifetime = MinMaxCurve::twoConstants(1.0f, 2.0f);
        d.main.startSpeed = MinMaxCurve::fromCurve(Curve::linear(1.0f, 3.0f), 1.5f);
        d.main.startSize3D = true;
        d.main.startSize = MinMaxCurve(0.5f);
        d.main.startSizeY = MinMaxCurve(0.25f);
        d.main.startSizeZ = MinMaxCurve(0.75f);
        d.main.startRotation3D = true;
        d.main.startRotation = MinMaxCurve::twoConstants(0.0f, 90.0f);
        d.main.startRotationX = MinMaxCurve(10.0f);
        d.main.startRotationY = MinMaxCurve(20.0f);
        d.main.flipRotation = 0.5f;
        d.main.startColor.mode = GradientValueMode::TwoColors;
        d.main.startColor.color = Vector4(1.0f, 0.5f, 0.25f, 1.0f);
        d.main.startColor.colorMax = Vector4(0.5f, 0.5f, 1.0f, 0.75f);
        d.main.gravityModifier = MinMaxCurve(0.5f);
        d.main.inheritVelocity = MinMaxCurve(0.25f);

        d.emission.enabled = false;
        d.emission.rateOverTime = MinMaxCurve::twoConstants(5.0f, 15.0f);
        d.emission.rateOverDistance = MinMaxCurve(2.0f);
        Burst burst;
        burst.time = 0.5f;
        burst.count = MinMaxCurve::twoConstants(10.0f, 20.0f);
        burst.cycles = 3;
        burst.interval = 0.2f;
        burst.probability = 0.75f;
        d.emission.bursts = {burst, Burst()};
        d.emission.maxPerTick = 50;

        d.shape.shape = ShapeType::Box;
        d.shape.emitFrom = ShapeEmitFrom::Shell;
        d.shape.radius = 2.0f;
        d.shape.radiusThickness = 0.5f;
        d.shape.angle = 30.0f;
        d.shape.length = 2.0f;
        d.shape.arc = 180.0f;
        d.shape.donutRadius = 0.3f;
        d.shape.boxThickness = Vector3(0.1f, 0.2f, 0.3f);
        d.shape.mesh.type = MeshSourceType::Primitive;
        d.shape.mesh.primitive = PrimitiveMesh::Sphere;
        d.shape.position = Vector3(0.0f, 1.0f, 0.0f);
        d.shape.rotation = Vector3(0.0f, 45.0f, 0.0f);
        d.shape.scale = Vector3(2.0f, 1.0f, 3.0f);
        d.shape.randomizeDirection = 0.1f;
        d.shape.spherizeDirection = 0.2f;
        d.shape.randomizePosition = 0.3f;

        d.startVelocity.present = true;
        d.startVelocity.min = Vector3(-1.0f, 0.0f, -1.0f);
        d.startVelocity.max = Vector3(1.0f, 2.0f, 1.0f);
        d.startVelocity.uniform = true;

        d.velocityOverLifetime.present = true;
        d.velocityOverLifetime.x = MinMaxCurve(1.0f);
        d.velocityOverLifetime.y = twoCurves;
        d.velocityOverLifetime.z = MinMaxCurve::twoConstants(-1.0f, 1.0f);
        d.velocityOverLifetime.space = ValueSpace::World;
        d.velocityOverLifetime.orbitalX = MinMaxCurve(10.0f);
        d.velocityOverLifetime.orbitalY = MinMaxCurve(20.0f);
        d.velocityOverLifetime.orbitalZ = MinMaxCurve(30.0f);
        d.velocityOverLifetime.offsetX = MinMaxCurve(0.5f);
        d.velocityOverLifetime.radial = MinMaxCurve(0.5f);
        d.velocityOverLifetime.speedModifier = MinMaxCurve(0.9f);

        d.limitVelocity.present = true;
        d.limitVelocity.limit = MinMaxCurve(4.0f);
        d.limitVelocity.dampen = 0.5f;
        d.limitVelocity.drag = MinMaxCurve(0.2f);
        d.limitVelocity.quadraticDrag = 0.05f;
        d.limitVelocity.multiplyBySize = false;
        d.limitVelocity.multiplyByVelocity = true;

        d.forceOverLifetime.present = true;
        d.forceOverLifetime.x = MinMaxCurve(0.5f);
        d.forceOverLifetime.y = MinMaxCurve::twoConstants(-1.0f, 1.0f);
        d.forceOverLifetime.z = MinMaxCurve(-0.5f);
        d.forceOverLifetime.space = ValueSpace::World;
        d.forceOverLifetime.randomizePerTick = true;

        d.noise.present = true;
        d.noise.separateAxes = true;
        d.noise.strength = MinMaxCurve(2.0f);
        d.noise.strengthY = MinMaxCurve(1.5f);
        d.noise.strengthZ = MinMaxCurve(0.5f);
        d.noise.frequency = 1.25f;
        d.noise.scrollSpeed = 0.5f;
        d.noise.damping = false;
        d.noise.octaves = 2;
        d.noise.octaveMultiplier = 0.25f;
        d.noise.octaveScale = 3.0f;
        d.noise.quality = NoiseQuality::Medium;
        d.noise.positionAmount = MinMaxCurve(0.5f);
        d.noise.rotationAmount = MinMaxCurve(0.25f);
        d.noise.sizeAmount = MinMaxCurve(0.1f);

        d.orbit.present = true;
        d.orbit.axis = Vector3(1.0f, 0.0f, 0.0f);
        d.orbit.center = Vector3(0.0f, 1.0f, 0.0f);
        d.orbit.angularSpeed = MinMaxCurve(45.0f);
        d.orbit.radialPull = MinMaxCurve(0.1f);
        d.orbit.space = ValueSpace::World;

        d.externalForces.present = true;
        d.externalForces.multiplier = 2.0f;
        d.externalForces.layerMask = 0x5u;

        d.colorOverLifetime.present = true;
        d.colorOverLifetime.color = twoGradients;
        d.colorOverLifetime.setMode = true;

        d.colorBySpeed.present = true;
        d.colorBySpeed.color.mode = GradientValueMode::Gradient;
        d.colorBySpeed.color.gradient = Gradient::twoColors(Vector3(0.2f, 0.2f, 0.2f), Vector3(1.0f, 1.0f, 1.0f));
        d.colorBySpeed.speedRange = Vector2(1.0f, 5.0f);

        d.sizeOverLifetime.present = true;
        d.sizeOverLifetime.separateAxes = true;
        d.sizeOverLifetime.size = MinMaxCurve::fromCurve(Curve::linear(1.0f, 0.0f));
        d.sizeOverLifetime.sizeY = MinMaxCurve(0.5f);
        d.sizeOverLifetime.sizeZ = twoCurves;

        d.sizeBySpeed.present = true;
        d.sizeBySpeed.separateAxes = true;
        d.sizeBySpeed.size = MinMaxCurve(1.5f);
        d.sizeBySpeed.sizeY = MinMaxCurve(0.5f);
        d.sizeBySpeed.sizeZ = MinMaxCurve(2.0f);
        d.sizeBySpeed.speedRange = Vector2(0.5f, 2.0f);

        d.rotationOverLifetime.present = true;
        d.rotationOverLifetime.separateAxes = true;
        d.rotationOverLifetime.angularVelocity = MinMaxCurve(90.0f);
        d.rotationOverLifetime.angularVelocityX = MinMaxCurve(10.0f);
        d.rotationOverLifetime.angularVelocityY = MinMaxCurve(-10.0f);

        d.rotationBySpeed.present = true;
        d.rotationBySpeed.angularVelocity = MinMaxCurve(30.0f);
        d.rotationBySpeed.speedRange = Vector2(0.0f, 10.0f);

        d.textureSheetAnimation.present = true;
        d.textureSheetAnimation.mode = TextureSheetMode::Sprites;
        d.textureSheetAnimation.tilesX = 4;
        d.textureSheetAnimation.tilesY = 2;
        d.textureSheetAnimation.animation = TextureSheetAnimation::SingleRow;
        d.textureSheetAnimation.rowMode = TextureSheetRowMode::Random;
        d.textureSheetAnimation.rowIndex = 1;
        SpriteRect a, b;
        a.width = 0.5f;
        b.x = 0.5f;
        b.width = 0.5f;
        d.textureSheetAnimation.sprites = {a, b};
        d.textureSheetAnimation.timeMode = TextureSheetTimeMode::FPS;
        d.textureSheetAnimation.frameOverTime = MinMaxCurve::fromCurve(Curve::linear(0.0f, 0.5f));
        d.textureSheetAnimation.fps = 24.0f;
        d.textureSheetAnimation.speedRange = Vector2(0.0f, 4.0f);
        d.textureSheetAnimation.startFrame = MinMaxCurve::twoConstants(0.0f, 1.0f);
        d.textureSheetAnimation.cycles = 2.0f;
        d.textureSheetAnimation.frameBlending = true;
        d.textureSheetAnimation.startFrameList = {0, 1};

        d.collision.present = true;
        d.collision.type = CollisionType::Planes;
        CollisionPlane plane;
        plane.point = Vector3(0.0f, -1.0f, 0.0f);
        plane.normal = Vector3(0.0f, 1.0f, 0.0f);
        d.collision.planes = {plane};
        d.collision.dampen = MinMaxCurve(0.2f);
        d.collision.bounce = MinMaxCurve(0.6f);
        d.collision.settleSpeed = 0.5f;
        d.collision.lifetimeLoss = MinMaxCurve(0.1f);
        d.collision.minKillSpeed = 0.01f;
        d.collision.maxKillSpeed = 500.0f;
        d.collision.radiusScale = 0.5f;
        d.collision.killOnCollide = true;
        d.collision.collidesWith = 0x3u;
        d.collision.quality = CollisionQuality::High;
        d.collision.maxQueriesPerTick = 128;
        d.collision.planeCacheTicks = 4;
        d.collision.sendEvents = true;
        d.collision.maxCollisionEvents = 32;

        d.trails.present = true;
        d.trails.mode = TrailMode::PerParticle;
        d.trails.ratio = 0.5f;
        d.trails.ribbonCount = 2;
        d.trails.lifetime = MinMaxCurve(0.5f);
        d.trails.minVertexDistance = 0.1f;
        d.trails.maxPoints = 24;
        d.trails.worldSpace = false;
        d.trails.dieWithParticles = false;
        d.trails.widthOverTrail = MinMaxCurve::fromCurve(Curve::linear(1.0f, 0.0f));
        d.trails.colorOverLifetime = MinMaxGradient(Vector4(1.0f, 0.0f, 0.0f, 1.0f));
        d.trails.colorOverTrail.mode = GradientValueMode::Gradient;
        d.trails.colorOverTrail.gradient = Gradient::twoColors(Vector3(1.0f, 1.0f, 1.0f), Vector3(1.0f, 1.0f, 1.0f), 1.0f, 0.0f);
        d.trails.inheritParticleColor = false;
        d.trails.sizeAffectsWidth = false;
        d.trails.sizeAffectsLifetime = true;
        d.trails.textureMode = TrailTextureMode::Tile;
        d.trails.generateLightingData = true;

        d.subEmittersModule.present = true;
        d.customData.present = true;
        d.customData.data[0] = MinMaxCurve(1.0f);
        d.customData.data[1] = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f));
        d.customData.data[2] = MinMaxCurve::twoConstants(2.0f, 3.0f);
        d.customData.data[3] = twoCurves;

        // S49's Pixel Art module (a warning beside the Mesh renderer, still valid)
        d.pixelArt.present = true;
        d.pixelArt.snap = false;
        d.pixelArt.integerSize = false;
        d.pixelArt.fade = PixelFade::Cutout;
        d.pixelArt.ditherSize = 8;
        d.pixelArt.ditherSpace = DitherSpace::Particle;
        d.pixelArt.fadeOverLifetime = MinMaxCurve::fromCurve(Curve::linear(1.0f, 0.0f));
        d.pixelArt.holdFrameRate = 12.0f;
        d.pixelArt.secondary = SecondaryColorMode::Color;
        d.pixelArt.secondaryOffset = -2;
        d.pixelArt.secondaryColor = Vector4(0.5f, 0.25f, 1.0f, 1.0f);
        d.pixelArt.wobbleAmplitude = Vector2(2.0f, 1.0f);
        d.pixelArt.wobbleFrequency = Vector2(0.5f, 0.75f);
        d.pixelArt.flickerBelow = 0.25f;

        d.renderer.renderMode = RenderMode::Mesh;
        d.renderer.cameraScale = 0.5f;
        d.renderer.speedScale = 0.25f;
        d.renderer.lengthScale = 3.0f;
        d.renderer.freezeInView = true;
        d.renderer.verticalLock = VerticalLock::Emitter;
        MeshSource cube, model;
        model.type = MeshSourceType::Model;
        model.model = "models/rock.glb";
        model.modelGuid = "0123456789abcdef0123456789abcdef";
        model.submesh = 1;
        d.renderer.meshes = {cube, model};
        d.renderer.meshSelection = MeshSelection::Index;
        d.renderer.meshAlignment = MeshAlignment::Velocity;
        d.renderer.pivot = Vector3(0.0f, 0.5f, 0.0f);
        d.renderer.flip = Vector3(0.5f, 0.0f, 0.0f);
        d.renderer.shader = "shaders/myparticles";
        d.renderer.shaderFeatures = {"GLOW", "SOFT"};
        ShaderProperty glow;
        glow.name = "glowScale";
        glow.value = Vector4(2.0f, 0.0f, 0.0f, 0.0f);
        d.renderer.shaderProperties = {glow};
        ShaderTexture noiseTex;
        noiseTex.name = "u_erosionTexture";
        noiseTex.path = "textures/noise.png";
        d.renderer.shaderTextures = {noiseTex};
        d.renderer.texture = "textures/smoke.png";
        d.renderer.textureGuid = "fedcba9876543210fedcba9876543210";
        d.renderer.trailTexture = "textures/trail.png";
        d.renderer.blend = ParticleBlend::Premultiplied;
        d.renderer.depthWrite = true;
        d.renderer.sortMode = SortMode::ByDistance;
        d.renderer.sortingFudge = 1.5f;
        d.renderer.sortPriority = -2;
        d.renderer.minScreenSize = 0.01f;
        d.renderer.maxScreenSize = 0.8f;
        d.renderer.renderLayers = 0xffu;
        d.renderer.receiveShadows = true;
        d.renderer.lighting = LightingMode::SixWay;
        d.renderer.sixWayLayout = SixWayLayout::TopLeftRightBottomBackFront;
        d.renderer.sixWayMapA = "textures/smoke_a.png";
        d.renderer.sixWayMapB = "textures/smoke_b.png";
        d.renderer.sixWayContrast = 1.5f;
        d.renderer.emissionMap = "textures/smoke_e.png";
        d.renderer.emissionColor = Vector4(1.0f, 0.5f, 0.0f, 1.0f);
        d.renderer.emissionIntensity = 2.0f;

        d.setOverAge.present = true;
        SetOverAgeEntry position;
        position.property = SetProperty::Position;
        position.fromTime = 0.5f;
        position.toTime = 1.0f;
        position.curve = Curve::easeInOut(0.0f, 0.0f, 1.0f, 1.0f);
        position.from = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        position.to = Vector4(1.0f, 2.0f, 3.0f, 0.0f);
        SetOverAgeEntry frame;
        frame.property = SetProperty::Frame;
        frame.fromTime = 0.0f;
        frame.toTime = 1.0f;
        frame.frames = {1, 0};
        frame.shortestPath = true;
        frame.srgb = false;
        d.setOverAge.entries = {position, frame};

        SubEmitterEntry sub;
        sub.trigger = SubEmitterTrigger::Trigger;
        sub.effect = "effects/sparks.particles";
        sub.effectGuid = "00112233445566778899aabbccddeeff";
        sub.triggerName = "Boom";
        sub.inheritColor = sub.inheritSize = sub.inheritRotation = sub.inheritLifetime = sub.inheritVelocity = true;
        sub.inheritFactor = 0.5f;
        sub.emitCount = 5;
        sub.probability = 0.5f;
        d.subEmitters = {sub};

        ExposedParameter rate;
        rate.name = "Rate";
        rate.path = "emission.rateOverTime";
        ExposedParameter tint;
        tint.name = "Tint";
        tint.path = "main.startColor";
        tint.type = ExposedType::Color;
        d.exposed = {rate, tint};
        return d;
    }

}

#endif // TESTS_PARTICLETESTEFFECTS_H
