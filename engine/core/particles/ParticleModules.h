// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLE_MODULES_H
#define PARTICLES_PARTICLE_MODULES_H

// The modules of a version 2 Particle System (particles spec 6.2): Unity's module model, names
// and defaults (a new Unity system's values unless noted). Rotations and angular speeds are in
// radians, angles of the shape in degrees, colours linear RGBA. The axes are the engine's (right
// handed, Y up): the shape emits along its local +Y (the importer turns Unity's +Z into it).

#include "Export.h"
#include "MinMax.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "math/Quaternion.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    // ------------------------------------------------------------------ enums (Unity names)
    // (int sized: the editor's Catalog reads enum properties as int)

    enum class ParticleSystemSimulationSpace { Local = 0, World = 1 };
    // Hierarchy: the whole world scale; Local: the system's own scale only; Shape: only the
    // shape's positions are scaled (not sizes nor speeds)
    enum class ParticleSystemScalingMode { Hierarchy = 0, Local = 1, Shape = 2 };
    enum class ParticleSystemStopAction { None = 0, Disable = 1, Destroy = 2, Callback = 3 };
    enum class ParticleSystemEmitterVelocityMode { Transform = 0, Rigidbody = 1 };
    // the gravity gravityModifier scales (Unity 2021.2 MainModule.gravitySource): the 3D world's
    // (m/s^2) or the 2D world's, in the scene's units (Box2D gravity x points per meter)
    enum class ParticleSystemGravitySource { Physics3D = 0, Physics2D = 1 };
    enum class ParticleSystemStopBehavior { StopEmittingAndClear = 0, StopEmitting = 1 };

    enum class ParticleSystemShapeType {
        Sphere = 0, Hemisphere = 1, Cone = 2, ConeVolume = 3, Box = 4, BoxShell = 5, BoxEdge = 6,
        Circle = 7, Edge = 8, Rectangle = 9, Donut = 10
    };
    // how a shape picks the angle on its arc (or the point along its radius)
    enum class ParticleSystemShapeMultiModeValue { Random = 0, Loop = 1, PingPong = 2, BurstSpread = 3 };

    enum class ParticleSystemInheritVelocityMode { Initial = 0, Current = 1 };

    enum class ParticleSystemAnimationMode { Grid = 0, Sprites = 1 };
    enum class ParticleSystemAnimationType { WholeSheet = 0, SingleRow = 1 };
    enum class ParticleSystemAnimationRowMode { Custom = 0, Random = 1 };
    enum class ParticleSystemAnimationTimeMode { Lifetime = 0, Speed = 1, FPS = 2 };

    enum class ParticleSystemRenderMode {
        Billboard = 0, Stretch = 1, HorizontalBillboard = 2, VerticalBillboard = 3, Mesh = 4
    };
    enum class ParticleSystemSortMode { None = 0, Distance = 1, OldestInFront = 2, YoungestInFront = 3 };
    // render alignment of Billboard and Mesh particles
    enum class ParticleSystemRenderSpace { View = 0, World = 1, Local = 2, Facing = 3, Velocity = 4 };

    // ------------------------------------------------------------------ modules

    struct JASPERENGINE_API ParticleMainModule {
        float duration = 5.0f;          // seconds, at least 0.05
        bool looping = true;
        bool prewarm = false;           // looping systems only
        MinMaxCurve startDelay = MinMaxCurve(0.0f);   // Constant or TwoConstants
        MinMaxCurve startLifetime = MinMaxCurve(5.0f);
        MinMaxCurve startSpeed = MinMaxCurve(5.0f);
        bool startSize3D = false;
        MinMaxCurve startSize = MinMaxCurve(1.0f);    // x when startSize3D (Unity startSizeX)
        MinMaxCurve startSizeY = MinMaxCurve(1.0f);
        MinMaxCurve startSizeZ = MinMaxCurve(1.0f);
        bool startRotation3D = false;
        MinMaxCurve startRotation = MinMaxCurve(0.0f);   // z, radians (Unity startRotationZ)
        MinMaxCurve startRotationX = MinMaxCurve(0.0f);
        MinMaxCurve startRotationY = MinMaxCurve(0.0f);
        float flipRotation = 0.0f;      // 0..1: the share spinning the other way (Unity randomizeRotationDirection)
        MinMaxGradient startColor;
        MinMaxCurve gravityModifier = MinMaxCurve(0.0f);
        ParticleSystemGravitySource gravitySource = ParticleSystemGravitySource::Physics3D;
        ParticleSystemSimulationSpace simulationSpace = ParticleSystemSimulationSpace::Local;
        float simulationSpeed = 1.0f;
        ParticleSystemScalingMode scalingMode = ParticleSystemScalingMode::Local;
        bool playOnAwake = true;
        unsigned int maxParticles = 1000;   // 0..100000
        ParticleSystemEmitterVelocityMode emitterVelocityMode = ParticleSystemEmitterVelocityMode::Transform;
        ParticleSystemStopAction stopAction = ParticleSystemStopAction::None;
        bool autoRandomSeed = true;
        uint32_t randomSeed = 0;

        bool operator==(const ParticleMainModule& o) const;
        bool operator!=(const ParticleMainModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleEmissionBurst {
        float time = 0.0f;
        MinMaxCurve count = MinMaxCurve(30.0f);
        int cycles = 1;          // 0 = repeats forever
        float interval = 0.01f;  // seconds between cycles
        float probability = 1.0f;

        bool operator==(const ParticleEmissionBurst& o) const;
        bool operator!=(const ParticleEmissionBurst& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleEmissionModule {
        bool enabled = true;
        MinMaxCurve rateOverTime = MinMaxCurve(10.0f);
        MinMaxCurve rateOverDistance = MinMaxCurve(0.0f);
        std::vector<ParticleEmissionBurst> bursts;

        bool operator==(const ParticleEmissionModule& o) const;
        bool operator!=(const ParticleEmissionModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleShapeModule {
        bool enabled = true;
        ParticleSystemShapeType shapeType = ParticleSystemShapeType::Cone;
        float radius = 1.0f;
        float radiusThickness = 1.0f;   // 0: on the surface .. 1: the whole volume
        ParticleSystemShapeMultiModeValue radiusMode = ParticleSystemShapeMultiModeValue::Random;
        float radiusSpread = 0.0f;
        MinMaxCurve radiusSpeed = MinMaxCurve(1.0f);
        float angle = 25.0f;            // degrees
        float length = 5.0f;
        float arc = 360.0f;             // degrees
        ParticleSystemShapeMultiModeValue arcMode = ParticleSystemShapeMultiModeValue::Random;
        float arcSpread = 0.0f;
        MinMaxCurve arcSpeed = MinMaxCurve(1.0f);
        Vector3 boxThickness = Vector3(0, 0, 0);
        float donutRadius = 0.2f;
        Vector3 position = Vector3(0, 0, 0);
        Vector3 rotation = Vector3(0, 0, 0);   // Euler degrees, X then Y then Z (Unity's shape transform)
        Vector3 scale = Vector3(1, 1, 1);
        bool alignToDirection = false;
        float randomDirectionAmount = 0.0f;
        float sphericalDirectionAmount = 0.0f;
        float randomPositionAmount = 0.0f;

        bool operator==(const ParticleShapeModule& o) const;
        bool operator!=(const ParticleShapeModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleVelocityOverLifetimeModule {
        bool enabled = false;
        MinMaxCurve x, y, z;
        MinMaxCurve orbitalX, orbitalY, orbitalZ;           // radians per second
        MinMaxCurve orbitalOffsetX, orbitalOffsetY, orbitalOffsetZ;
        MinMaxCurve radial;
        MinMaxCurve speedModifier = MinMaxCurve(1.0f);
        ParticleSystemSimulationSpace space = ParticleSystemSimulationSpace::Local;

        bool operator==(const ParticleVelocityOverLifetimeModule& o) const;
        bool operator!=(const ParticleVelocityOverLifetimeModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleLimitVelocityOverLifetimeModule {
        bool enabled = false;
        bool separateAxes = false;
        MinMaxCurve limit = MinMaxCurve(1.0f);
        MinMaxCurve limitX = MinMaxCurve(1.0f), limitY = MinMaxCurve(1.0f), limitZ = MinMaxCurve(1.0f);
        ParticleSystemSimulationSpace space = ParticleSystemSimulationSpace::Local;
        float dampen = 1.0f;
        MinMaxCurve drag = MinMaxCurve(0.0f);
        bool multiplyDragByParticleSize = true;
        bool multiplyDragByParticleVelocity = true;

        bool operator==(const ParticleLimitVelocityOverLifetimeModule& o) const;
        bool operator!=(const ParticleLimitVelocityOverLifetimeModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleInheritVelocityModule {
        bool enabled = false;
        ParticleSystemInheritVelocityMode mode = ParticleSystemInheritVelocityMode::Initial;
        MinMaxCurve curve = MinMaxCurve(0.0f);

        bool operator==(const ParticleInheritVelocityModule& o) const;
        bool operator!=(const ParticleInheritVelocityModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleForceOverLifetimeModule {
        bool enabled = false;
        MinMaxCurve x, y, z;
        ParticleSystemSimulationSpace space = ParticleSystemSimulationSpace::Local;
        bool randomized = false;   // a new random value each frame (two constants / two curves)

        bool operator==(const ParticleForceOverLifetimeModule& o) const;
        bool operator!=(const ParticleForceOverLifetimeModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleColorOverLifetimeModule {
        bool enabled = false;
        MinMaxGradient color = MinMaxGradient::fromGradient(jasperengine::Gradient());

        bool operator==(const ParticleColorOverLifetimeModule& o) const;
        bool operator!=(const ParticleColorOverLifetimeModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleColorBySpeedModule {
        bool enabled = false;
        MinMaxGradient color = MinMaxGradient::fromGradient(jasperengine::Gradient());
        Vector2 range = Vector2(0.0f, 1.0f);   // speeds mapped to 0 .. 1

        bool operator==(const ParticleColorBySpeedModule& o) const;
        bool operator!=(const ParticleColorBySpeedModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleSizeOverLifetimeModule {
        bool enabled = false;
        bool separateAxes = false;
        MinMaxCurve size = MinMaxCurve::fromCurve(AnimationCurve::linear(0.0f, 0.0f, 1.0f, 1.0f));   // x when separateAxes
        MinMaxCurve y = MinMaxCurve::fromCurve(AnimationCurve::linear(0.0f, 0.0f, 1.0f, 1.0f));
        MinMaxCurve z = MinMaxCurve::fromCurve(AnimationCurve::linear(0.0f, 0.0f, 1.0f, 1.0f));

        bool operator==(const ParticleSizeOverLifetimeModule& o) const;
        bool operator!=(const ParticleSizeOverLifetimeModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleSizeBySpeedModule {
        bool enabled = false;
        bool separateAxes = false;
        MinMaxCurve size = MinMaxCurve::fromCurve(AnimationCurve::linear(0.0f, 0.0f, 1.0f, 1.0f));
        MinMaxCurve y = MinMaxCurve::fromCurve(AnimationCurve::linear(0.0f, 0.0f, 1.0f, 1.0f));
        MinMaxCurve z = MinMaxCurve::fromCurve(AnimationCurve::linear(0.0f, 0.0f, 1.0f, 1.0f));
        Vector2 range = Vector2(0.0f, 1.0f);

        bool operator==(const ParticleSizeBySpeedModule& o) const;
        bool operator!=(const ParticleSizeBySpeedModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleRotationOverLifetimeModule {
        bool enabled = false;
        bool separateAxes = false;
        MinMaxCurve x, y;
        MinMaxCurve z = MinMaxCurve(0.785398163f);   // 45 degrees per second

        bool operator==(const ParticleRotationOverLifetimeModule& o) const;
        bool operator!=(const ParticleRotationOverLifetimeModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleRotationBySpeedModule {
        bool enabled = false;
        bool separateAxes = false;
        MinMaxCurve x, y;
        MinMaxCurve z = MinMaxCurve(0.785398163f);
        Vector2 range = Vector2(0.0f, 1.0f);

        bool operator==(const ParticleRotationBySpeedModule& o) const;
        bool operator!=(const ParticleRotationBySpeedModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleNoiseModule {
        bool enabled = false;
        bool separateAxes = false;
        MinMaxCurve strength = MinMaxCurve(1.0f);    // x when separateAxes
        MinMaxCurve strengthY = MinMaxCurve(1.0f);
        MinMaxCurve strengthZ = MinMaxCurve(1.0f);
        float frequency = 0.5f;
        MinMaxCurve scrollSpeed = MinMaxCurve(0.0f);
        bool damping = true;     // strength divided by frequency (Unity)
        int octaves = 1;         // 1..4
        float octaveMultiplier = 0.5f;
        float octaveScale = 2.0f;
        MinMaxCurve positionAmount = MinMaxCurve(1.0f);
        MinMaxCurve rotationAmount = MinMaxCurve(0.0f);
        MinMaxCurve sizeAmount = MinMaxCurve(0.0f);

        bool operator==(const ParticleNoiseModule& o) const;
        bool operator!=(const ParticleNoiseModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleTextureSheetAnimationModule {
        bool enabled = false;
        ParticleSystemAnimationMode mode = ParticleSystemAnimationMode::Grid;
        int numTilesX = 1;
        int numTilesY = 1;
        ParticleSystemAnimationType animation = ParticleSystemAnimationType::WholeSheet;
        ParticleSystemAnimationRowMode rowMode = ParticleSystemAnimationRowMode::Custom;
        int rowIndex = 0;
        ParticleSystemAnimationTimeMode timeMode = ParticleSystemAnimationTimeMode::Lifetime;
        // 0 .. 1 over the frames of the sheet (or row)
        MinMaxCurve frameOverTime = MinMaxCurve::fromCurve(AnimationCurve::linear(0.0f, 0.0f, 1.0f, 1.0f));
        float fps = 30.0f;
        Vector2 speedRange = Vector2(0.0f, 1.0f);
        MinMaxCurve startFrame = MinMaxCurve(0.0f);   // frames
        int cycles = 1;
        float flipU = 0.0f;   // 0..1: the share mirrored
        float flipV = 0.0f;
        std::vector<int> sprites;   // Sprites mode: frame ids of the target's sprite frames

        bool operator==(const ParticleTextureSheetAnimationModule& o) const;
        bool operator!=(const ParticleTextureSheetAnimationModule& o) const { return !(*this == o); }
    };

    struct JASPERENGINE_API ParticleRendererModule {
        ParticleSystemRenderMode renderMode = ParticleSystemRenderMode::Billboard;
        float cameraVelocityScale = 0.0f;
        float velocityScale = 0.0f;
        float lengthScale = 2.0f;
        ParticleSystemRenderSpace alignment = ParticleSystemRenderSpace::View;
        ParticleSystemSortMode sortMode = ParticleSystemSortMode::None;
        float sortingFudge = 0.0f;
        int sortPriority = 0;              // Unity's sortingOrder: higher draws later (on top)
        float minParticleSize = 0.0f;      // fractions of the viewport height
        float maxParticleSize = 0.5f;
        Vector3 pivot = Vector3(0, 0, 0);  // in particle sizes
        Vector3 flip = Vector3(0, 0, 0);   // 0..1: the share mirrored per axis
        // Mesh mode: the mesh the target draws ("quad", "cube", "sphere", "cylinder", "capsule",
        // "plane" or a model path); billboards always draw the target's quad
        std::string mesh = "quad";

        bool operator==(const ParticleRendererModule& o) const;
        bool operator!=(const ParticleRendererModule& o) const { return !(*this == o); }
    };

    // ------------------------------------------------------------------ runtime

    struct ParticleBurstState {
        int cyclesDone = 0;
    };

    // the curves baked for the simulation (ParticleSimulation.cpp) with the module values they
    // were baked from; a copy of the state starts without (each system bakes its own)
    struct ParticleBaked;
    struct JASPERENGINE_API ParticleBakedCache {
        std::shared_ptr<ParticleBaked> data;

        ParticleBakedCache() = default;
        ParticleBakedCache(const ParticleBakedCache&) {}
        ParticleBakedCache& operator=(const ParticleBakedCache&) { data.reset(); return *this; }
        ParticleBakedCache(ParticleBakedCache&&) noexcept = default;
        ParticleBakedCache& operator=(ParticleBakedCache&&) noexcept = default;
    };

    // the simulation's runtime data (not serialized, not compared): particles in arrays of
    // fields (spawn order kept: the age sorts and None draw in it), the system's clocks
    struct JASPERENGINE_API ParticleSimState {
        // per particle
        std::vector<Vector3> position;        // simulation space
        std::vector<Vector3> velocity;        // integrated (start speed, gravity, force, ...)
        std::vector<Vector3> totalVelocity;   // velocity + animated velocity x speed modifier, last step
        std::vector<float> age;
        std::vector<float> lifetime;
        std::vector<Vector3> startSize;
        std::vector<Vector3> size;            // last step, simulation units (scaled by the emitter as its mode says)
        std::vector<Vector3> rotation;        // radians (z: the billboard roll)
        std::vector<float> rotationSign;      // -1 for the flipRotation share, else 1
        std::vector<Vector4> startColor;
        std::vector<Vector4> color;           // last step
        std::vector<uint32_t> seed;           // per-module randoms derive from it
        std::vector<float> frame;             // texture sheet frame (index, fractional part ignored)
        std::vector<Quaternion> alignRotation;// alignToDirection: the start rotation as a quaternion

        // system
        bool playing = false;     // play() ran and the system has not stopped (Unity isPlaying)
        bool paused = false;
        bool emitting = false;
        bool prewarmed = false;
        bool awoken = false;      // playOnAwake handled for this activation
        float time = 0.0f;        // in the cycle (0 .. duration)
        float delay = 0.0f;       // start delay left
        float rateAccumulator = 0.0f;
        float distanceAccumulator = 0.0f;
        int loops = 0;
        std::vector<ParticleBurstState> bursts;
        uint64_t rng = 0x853c49e6748fea9bULL;   // PCG32 state
        uint64_t rngInc = 0xda3e39cb94b95bdbULL;
        uint32_t seedUsed = 0;
        uint32_t spawnCount = 0;  // for the Loop / PingPong / BurstSpread arc and radius modes
        float arcPhase = 0.0f;    // Loop / PingPong: degrees advanced along the arc
        float radiusPhase = 0.0f; // radius lengths advanced
        float noiseScroll = 0.0f; // the Noise module's scroll so far
        // the editor preview replays the same particles (spec 6.8): play() takes this seed
        bool fixedSeed = false;
        uint32_t fixedSeedValue = 1;

        // the emitter last step (world): spawn interpolation, rate over distance, inherit velocity
        bool hasEmitter = false;
        Vector3 emitterPosition;
        Quaternion emitterRotation;
        Vector3 emitterScale = Vector3(1, 1, 1);
        Vector3 emitterVelocity;
        Vector3 emitterVelocityDelta;
        bool teleportWarned = false;

        // baked curves (spec 8), rebuilt when the module values change
        ParticleBakedCache baked;

        size_t count() const { return age.size(); }
        void clearParticles();
        void reserve(size_t capacity);
    };

}

#endif //PARTICLES_PARTICLE_MODULES_H
