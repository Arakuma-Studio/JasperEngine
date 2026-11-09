// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLEPROGRAM_H
#define PARTICLES_PARTICLEPROGRAM_H

// A compiled particle effect (docs/specs/S20-particles.md D2, D3, D5, D10): which SoA channels
// the particles need, the flat lists of kernel ops a tick runs (spawn, update) and the render
// extraction reads, their parameter blocks and the baked curve/gradient lookup tables, all in
// one pool. A disabled module compiles to nothing. Programs are immutable and shared; two
// programs with the same structure (sameStructure) can swap under live particles.

#include "particles/ParticleEffect.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine { namespace particles {

    // The SoA channels (one float or uint32 array each). The core set is always there; the
    // others only when an enabled module needs them.
    enum Channel : uint8_t {
        // core: 14 x 4 = 56 bytes per particle
        ChPosX, ChPosY, ChPosZ,
        ChPrevX, ChPrevY, ChPrevZ,
        ChVelX, ChVelY, ChVelZ,
        ChAge,
        ChInvLife,
        ChSpawnIndex,     // uint32: the particle's spawn counter value (its random identity)
        ChStartSize,
        ChStartColor,     // uint32: RGBA8, sRGB-encoded (gamma 2.2) rgb
        // optional
        ChRotation, ChPrevRotation,   // radians, when ticked (by speed, noise, Set over Age)
        ChStartRotation,              // radians, when not a pure function of the spawn index
        ChSizeX, ChSizeY, ChSizeZ,    // the absolute size, when ticked (Set over Age)
        ChPrevSizeX,
        ChStartSizeY, ChStartSizeZ,   // 3D Start Size
        ChQuatX, ChQuatY, ChQuatZ, ChQuatW,   // 3D rotation (mesh particles, 3D start rotation)
        ChAngVelX, ChAngVelY, ChAngVelZ,      // 3D angular velocity, radians per second
        ChColorR, ChColorG, ChColorB, ChColorA,   // linear colour, when ticked (Set over Age / Replace Start Color)
        ChAccelX, ChAccelY, ChAccelZ,             // the particle's own acceleration (Set over Age)
        ChFrame,                      // a stored sprite frame (Set over Age Frame)
        ChNoiseSize,                  // the noise module's size factor of the last tick
        ChPlaneNX, ChPlaneNY, ChPlaneNZ, ChPlaneD,   // the cached contact plane (world collision)
        ChPlaneTick,                  // uint32: the tick the plane was found (0: none)
        ChTrailSlot,                  // uint32: the trail ring of the particle (~0: none)
        // the last tick's animated velocity (Velocity over Lifetime, Noise): stretched and
        // velocity-aligned particles follow the total velocity, as Unity's (origin/main's) did
        ChAnimVelX, ChAnimVelY, ChAnimVelZ,
        // a Follow Parent sub-emitter's child: its parent's spawn index (kNoParent once it died),
        // and the parent's rotation (Follow Rotation) and size (Follow Scale) at the last tick
        ChParentId,
        ChFollowQuatX, ChFollowQuatY, ChFollowQuatZ, ChFollowQuatW,
        ChFollowSize,
        ChSettled,                    // 1 once a collision made the particle rest (Settle Speed, S49 R14)
        ChCount
    };
    inline constexpr uint32_t kNoParent = 0xffffffffu;
    static_assert(ChCount <= 64, "the channel mask is 64 bits");

    using ChannelMask = uint64_t;
    constexpr ChannelMask channelBit(Channel c){ return ChannelMask(1) << c; }
    constexpr ChannelMask kCoreChannels =
        channelBit(ChPosX) | channelBit(ChPosY) | channelBit(ChPosZ) | channelBit(ChPrevX) | channelBit(ChPrevY) | channelBit(ChPrevZ) |
        channelBit(ChVelX) | channelBit(ChVelY) | channelBit(ChVelZ) | channelBit(ChAge) | channelBit(ChInvLife) |
        channelBit(ChSpawnIndex) | channelBit(ChStartSize) | channelBit(ChStartColor);

    JASPERENGINE_API const char* channelName(Channel channel);
    // Channels holding uint32 values (the rest are floats)
    constexpr bool isIntegerChannel(Channel c){ return c == ChSpawnIndex || c == ChStartColor || c == ChPlaneTick || c == ChTrailSlot || c == ChParentId; }

    // ---------------------------------------------------------------- compiled values

    // A MinMaxCurve after compilation: a constant, a random range, or one or two baked LUTs
    struct CurveParam {
        enum Kind : uint8_t { Const, Range, Lut, LutRange };
        Kind kind = Const;
        CurveDriver driver = CurveDriver::NormalizedAge;
        CurveWrap wrap = CurveWrap::Clamp;
        uint8_t pad = 0;
        float c0 = 0.0f, c1 = 0.0f;
        // the driver remap: input = driver * scale + offset, then wrapped into [0, 1]
        float scale = 1.0f, offset = 0.0f;
        uint32_t lut0 = 0, lut1 = 0;   // offsets of the LUTs in the program's float pool
        uint32_t lutSize = 0;          // entries (a power of two plus one)
        uint32_t stream = 0;           // the random stream of the per-particle pick
        bool isConst() const { return kind == Const; }
        bool isZero() const { return kind == Const && c0 == 0.0f; }
        bool usesRandom() const { return kind == Range || kind == LutRange; }
    };

    // A MinMaxGradient after compilation (linear RGBA). Ramp (S49 R12): the exact steps, lut0
    // their linear RGBA, lut1 their RGBA8 (uint32), lutSize the step count; never blended
    struct GradientParam {
        enum Kind : uint8_t { Const, Range, Lut, LutRange, Ramp };
        Kind kind = Const;
        CurveDriver driver = CurveDriver::NormalizedAge;
        CurveWrap wrap = CurveWrap::Clamp;
        uint8_t pad = 0;
        float c0[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float c1[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float scale = 1.0f, offset = 0.0f;
        uint32_t lut0 = 0, lut1 = 0;   // RGBA interleaved
        uint32_t lutSize = 0;
        uint32_t stream = 0;
        float rampBias = 1.0f;         // Ramp: t^bias
        uint32_t rampReverse = 0;
        bool isWhite() const { return kind == Const && c0[0] == 1.0f && c0[1] == 1.0f && c0[2] == 1.0f && c0[3] == 1.0f; }
        bool usesRandom() const { return kind == Range || kind == LutRange; }
    };

    // ---------------------------------------------------------------- ops

    enum class OpCode : uint8_t {
        // spawn (over the new particles)
        SpawnShape,
        SpawnVelocity,        // start speed along the shape's direction, Start Velocity, inherit velocity
        SpawnStartValues,     // lifetime, size, colour, rotation and the stored copies of them
        // update (over chunks, in this order)
        SetOverAge,
        Forces,               // gravity, Force over Lifetime, the particle's own acceleration
        ExternalForces,
        AnimatedVelocity,     // Velocity over Lifetime (displacement only)
        Noise,
        Integrate,            // velocity-Verlet with Limit Velocity, then age
        Orbit,                // turns positions (and velocities) about the axis: the radius is kept
        StoredAppearance,     // ticked rotation (by speed, angular velocity, noise)
        Collision,
        Kill,                 // marks the dead, reduces the bounds
        Count
    };

    struct Op {
        OpCode code;
        uint32_t params;      // byte offset of its block in the program's pool
    };

    enum class SpawnShapeKind : uint8_t { Point, Sphere, Hemisphere, Cone, Box, Circle, Donut, Edge, Rectangle, Mesh };

    struct ShapeParams {
        SpawnShapeKind kind = SpawnShapeKind::Point;
        ShapeEmitFrom emitFrom = ShapeEmitFrom::Volume;
        ShapeMultiMode arcMode = ShapeMultiMode::Random;
        ShapeMultiMode radiusMode = ShapeMultiMode::Random;
        float arcSpread = 0.0f;        // 0..1 of the arc (0: continuous)
        float radiusSpread = 0.0f;
        float arcDegrees = 360.0f;     // the arc, for the Loop / Ping-Pong phase (degrees)
        uint8_t alignToDirection = 0;  // the start rotation turned to the direction (ChQuat* / ChStartRotation)
        uint8_t pad[3] = {0, 0, 0};
        float radius = 1.0f;
        float innerRatio = 0.0f;       // inner radius / radius (1 - radius thickness)
        float tanAngle = 0.0f;
        float length = 0.0f;
        float arc = 6.2831853f;        // radians
        float donutRadius = 0.0f;
        float boxHalf[3] = {0.5f, 0.5f, 0.5f};
        float boxInner[3] = {0.0f, 0.0f, 0.0f};   // the inner half extents of a Box shell
        float randomizeDirection = 0.0f;
        float spherizeDirection = 0.0f;
        float randomizePosition = 0.0f;
        // the shape's transform (position, rotation, scale) in emitter space, column-major 3x4
        float transform[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // its rotation (directions)
        // Mesh: triangles (3 vertices of 3 floats + normal) and their cumulative area table
        uint32_t meshVertices = 0;     // pool offset of the vertex positions (x, y, z)
        uint32_t meshNormals = 0;
        uint32_t meshIndices = 0;      // pool offset of uint32 triangle indices
        uint32_t meshTriangles = 0;
        uint32_t meshVertexCount = 0;
        uint32_t meshAreaCdf = 0;      // pool offset of the cumulative triangle areas (normalized)
    };

    struct VelocityParams {
        CurveParam startSpeed;
        uint8_t hasStartVelocity = 0;
        uint8_t startVelocityUniform = 0;
        uint8_t hasInherit = 0;
        uint8_t pad = 0;
        float velMin[3] = {0, 0, 0};
        float velMax[3] = {0, 0, 0};
        CurveParam inherit;
    };

    struct StartParams {
        CurveParam lifetime;
        CurveParam size, sizeY, sizeZ;
        uint8_t size3D = 0;
        uint8_t rotation3D = 0;
        uint8_t storeRotation = 0;     // ChStartRotation
        uint8_t storeQuat = 0;         // ChQuat*
        CurveParam rotation, rotationX, rotationY;   // radians
        float flipRotation = 0.0f;
        GradientParam color;
        // set at spawn when the channels exist
        uint8_t initSize = 0;          // ChSize* = start size
        uint8_t initColor = 0;         // ChColor* = start colour (linear)
        uint8_t initAccel = 0;         // ChAccel* = the Force over Lifetime value
        uint8_t initRotation = 0;      // ChRotation = start rotation
        uint8_t initAngularVelocity = 0;
        uint8_t initFrame = 0;
        // Align to Direction: the start rotation turned by the shape's direction (SpawnShape left it
        // in the scratch's ax, ay, az): the quaternion, else the roll
        uint8_t alignToDirection = 0;
        uint8_t pad = 0;
        CurveParam angularVelocity, angularVelocityX, angularVelocityY;   // radians/s at birth (3D)
        CurveParam accelX, accelY, accelZ;
        uint8_t accelWorld = 0;
        uint8_t pad2[3] = {0, 0, 0};
        // initFrame: the start frame (Texture Sheet's start frame, or a pick of its list)
        CurveParam startFrame;
        uint32_t startFrameList = 0, startFrameListCount = 0;
    };

    // Set over Age, one entry
    struct alignas(16) SetParams {
        SetProperty property = SetProperty::Position;
        uint8_t shortestPath = 0;
        uint8_t srgb = 0;
        uint8_t pad = 0;
        float fromTime = 0.0f, toTime = 0.0f, invSpan = 0.0f;
        float from[4] = {0, 0, 0, 0};
        float to[4] = {0, 0, 0, 0};
        uint32_t curveLut = 0, curveLutSize = 0;   // the ease
        uint32_t frames = 0, frameCount = 0;       // pool offset of the frame list
        uint8_t worldSpace = 0;                    // positions and directions go through the emitter transform
        uint8_t pad2[3] = {0, 0, 0};
    };

    struct SetOverAgeParams {
        uint32_t count = 0;
        uint32_t entries = 0;   // pool offset of `count` SetParams
        // Color over Lifetime's Replace Start Color: rgb set from the gradient at the age
        uint8_t replaceColor = 0;
        uint8_t pad[3] = {0, 0, 0};
        GradientParam color;
    };

    struct ForceParams {
        uint8_t gravity = 0;
        uint8_t force = 0;
        uint8_t forceWorld = 0;
        uint8_t forcePerTick = 0;
        uint8_t ownAcceleration = 0;   // ChAccel* (replaces Force over Lifetime)
        uint8_t inheritCurrent = 0;    // Inherit Velocity's Current mode: the emitter's change of velocity
        uint8_t pad[2] = {0, 0};
        CurveParam gravityModifier;
        CurveParam fx, fy, fz;
        CurveParam inherit;            // the share of it, over the particle's life
    };

    struct AnimatedVelocityParams {
        uint8_t linear = 0;
        uint8_t linearWorld = 0;
        uint8_t orbital = 0;
        uint8_t radial = 0;
        uint8_t speedModifier = 0;
        uint8_t pad[3] = {0, 0, 0};
        CurveParam vx, vy, vz;
        CurveParam ox, oy, oz;        // radians per second
        uint8_t offsetConst = 1;      // the centre's offset is the same for every particle (offset[])
        uint8_t pad2[3] = {0, 0, 0};
        float offset[3] = {0, 0, 0};  // emitter space
        CurveParam offX, offY, offZ;  // the offset over the life (emitter space), when not constant
        CurveParam radialSpeed;
        CurveParam speed;
    };

    struct OrbitParams {
        uint8_t world = 0;            // axis and center in world space
        uint8_t pad[3] = {0, 0, 0};
        float axis[3] = {0, 1, 0};    // unit
        float center[3] = {0, 0, 0};
        CurveParam angular;           // radians per second
        CurveParam pull;              // units per second toward the axis
    };

    struct NoiseParams {
        NoiseQuality quality = NoiseQuality::High;
        uint8_t separateAxes = 0;
        uint8_t octaves = 1;
        uint8_t affectsRotation = 0;
        uint8_t affectsSize = 0;
        uint8_t pad[3] = {0, 0, 0};
        CurveParam strengthX, strengthY, strengthZ;
        float frequency = 0.5f;
        float strengthScale = 1.0f;   // Damping: 1 / frequency
        float scrollSpeed = 0.0f;     // a constant scroll speed (scrollAccumulated: the emitter's sum)
        uint8_t scrollAccumulated = 0;
        uint8_t pad2[3] = {0, 0, 0};
        float octaveMultiplier = 0.5f;
        float octaveScale = 2.0f;
        uint32_t seed = 0;
        uint32_t lowField = 0;        // pool offset of the Low quality field (kNoiseLowCells^3 x 3)
        CurveParam positionAmount, rotationAmount, sizeAmount;
    };
    inline constexpr uint32_t kNoiseLowCells = 16;   // samples per side of the Low field
    inline constexpr float kNoiseLowPeriod = 4.0f;   // its lattice period (cells of the field)

    struct IntegrateParams {
        uint8_t limit = 0;
        uint8_t drag = 0;
        uint8_t dragBySize = 0;
        uint8_t dragByVelocity = 0;
        uint8_t ticksRotation = 0;
        uint8_t limitAxes = 0;         // a limit per axis (limitSpeed is X)
        uint8_t limitWorld = 0;        // ... in world axes (else the emitter's)
        uint8_t pad = 0;
        CurveParam limitSpeed;
        CurveParam limitY, limitZ;
        float dampen = 0.0f;           // per 1/30 s
        float logKeep = 0.0f;          // ln(1 - dampen)
        CurveParam dragLinear;
        float dragQuadratic = 0.0f;
    };

    struct StoredAppearanceParams {
        uint8_t rotationBySpeed = 0;
        uint8_t angularVelocity = 0;   // Rotation over Lifetime with a ticked rotation channel
        uint8_t angularVelocity3D = 0; // ChAngVel* turn ChQuat*
        uint8_t rotationBySpeed3D = 0; // Rotation by Speed turns ChQuat* (Z, and X / Y with separate axes)
        CurveParam rotationSpeed;      // radians/s by speed (driver: speed)
        CurveParam rotationSpeedX, rotationSpeedY;
        CurveParam angular;            // radians/s over age
    };

    enum class CollisionKind : uint8_t { Planes, World, Terrain, World2D };

    struct CollisionParams {
        CollisionKind kind = CollisionKind::Planes;
        CollisionQuality quality = CollisionQuality::Medium;
        uint8_t kill = 0;
        uint8_t events = 0;
        uint32_t planeCount = 0;
        float planes[kMaxCollisionPlanes][4] = {};   // normal (unit), d: dot(n, p) + d = 0, simulation space
        CurveParam dampen, bounce, lifetimeLoss;
        float minKillSpeed = 0.0f, maxKillSpeed = 1.0e4f;
        float radiusScale = 1.0f;
        uint32_t layerMask = 0xffffffffu;
        uint32_t maxQueries = 256;
        uint32_t planeCacheTicks = 8;
        uint32_t maxEvents = 64;
        uint32_t sizeFromStart = 1;    // the radius is half the start size (or the stored size)
        float settleSpeed = 0.0f;      // a hit slower than this rests the particle (ChSettled)
    };

    struct ProgramExternalForces {
        float multiplier = 1.0f;
    };

    // Integrate reads Velocity over Lifetime's speed modifier
    struct IntegrateOp {
        IntegrateParams integrate;
        uint8_t hasAnimatedVelocity = 0;
        uint8_t pad[3] = {0, 0, 0};
        uint32_t animatedVelocity = 0;   // pool offset of its AnimatedVelocityParams
    };

    // ---------------------------------------------------------------- emission

    struct BurstParam {
        float time = 0.0f;
        float countMin = 0.0f, countMax = 0.0f;
        uint8_t countCurve = 0;        // count read from `count` at the burst's time
        uint8_t pad[3] = {0, 0, 0};
        CurveParam count;
        int cycles = 1;
        float interval = 0.01f;
        float probability = 1.0f;
    };

    struct EmissionParams {
        bool enabled = false;
        CurveParam rate;               // per second, over the emitter time
        CurveParam rateOverDistance;
        std::vector<BurstParam> bursts;
        uint32_t maxPerTick = 0;
    };

    // ---------------------------------------------------------------- render extraction

    enum class SheetKind : uint8_t { None, Grid, Sprites };

    // The Pixel Art module and the Pixel render mode after compilation (S49): what packing does
    // on the CPU (fade over lifetime, wobble, flicker, the pixel stream) and what the Pixel
    // variant's uniforms hold (the grid rule, the shape, the dither, the ramp)
    struct PixelParams {
        uint8_t active = 0;            // the Pixel variant draws it (the module on a camera-facing mode, or Pixel)
        uint8_t module = 0;            // the Pixel Art module is on
        uint8_t shapes = 0;            // Render Mode Pixel: the shapes, and the pixel stream
        uint8_t snap = 1, pixelSizes = 1, integerSize = 1;
        PixelFade fade = PixelFade::Alpha;
        uint8_t ditherSize = 4;
        DitherSpace ditherSpace = DitherSpace::Screen;
        SecondaryColorMode secondary = SecondaryColorMode::RampOffset;
        int8_t secondaryOffset = 1;
        PixelShape shape = PixelShape::Dot;
        uint8_t fadeCurve = 0;         // Fade over Lifetime is not the constant 1
        uint8_t wobble = 0, wobbleRandomPhase = 1;
        uint8_t discRim = 0, dotShadeBottom = 0;
        // the ramp the shapes and Ramp Offset read: 0 none, 1 Start Color's, 2 Color over Lifetime's
        uint8_t rampSource = 0;
        // the drawn rgb is the ramp step's own bytes (S49 R13): the other colour modules are white
        uint8_t exactColor = 0;
        uint8_t pad = 0;
        CurveParam fadeOverLifetime;
        GradientParam ramp;            // the source module's ramp (its step per particle)
        float holdFrameRate = 0.0f;
        float secondaryColor[4] = {1, 1, 1, 1};   // sRGB
        float wobbleAmplitude[2] = {0, 0};        // base px
        float wobbleFrequency[2] = {1, 1};        // Hz
        float flickerBelow = 0.0f, flickerRate = 40.0f;
        float streakLengthScale = 0.022f, streakMaxLength = 9.0f, streakHeadFraction = 0.45f;
        uint32_t streakWidth = 1, ringWidth = 1, glowBands = 3, rotationSteps = 16;
        bool needsStream() const { return shapes != 0; }
    };

    struct RenderParams {
        RenderMode mode = RenderMode::Billboard;
        ParticleBlend blend = ParticleBlend::Alpha;
        SortMode sort = SortMode::None;
        LightingMode lighting = LightingMode::Unlit;
        // appearance (pure functions of age, spawn index, start values and velocity)
        uint8_t sizeOverLifetime = 0, sizeOverLifetime3D = 0;
        uint8_t sizeBySpeed = 0, sizeBySpeed3D = 0;
        uint8_t colorOverLifetime = 0, colorBySpeed = 0;
        uint8_t rotationOverLifetime = 0;   // pure: start + integral of the curve over age
        uint8_t storedRotation = 0, storedSize = 0, storedColor = 0, storedFrame = 0, startRotationStored = 0;
        uint8_t size3D = 0, noiseSize = 0;
        uint8_t customData = 0;
        uint8_t stretched = 0;
        uint8_t mesh = 0, quat = 0;
        uint8_t frameBlending = 0;
        uint8_t pad[3] = {0, 0, 0};
        CurveParam sizeX, sizeY, sizeZ;
        CurveParam speedSizeX, speedSizeY, speedSizeZ;
        GradientParam color;
        GradientParam speedColor;
        CurveParam rotationRate;           // radians/s; its LUT holds the integral when a curve
        uint32_t rotationIntegralLut = 0;  // the cumulative integral of rotationRate over normalized age
        CurveParam startRotation;          // when pure (radians)
        float flipRotation = 0.0f;
        // texture sheet
        SheetKind sheet = SheetKind::None;
        TextureSheetTimeMode sheetTime = TextureSheetTimeMode::Lifetime;
        uint8_t sheetRandomRow = 0, sheetSingleRow = 0;
        uint32_t tilesX = 1, tilesY = 1, frameCount = 1;
        uint32_t rowIndex = 0;
        CurveParam frameOverTime;
        float fps = 10.0f;
        float speedMin = 0.0f, speedMax = 1.0f;
        CurveParam startFrame;
        float cycles = 1.0f;
        uint32_t startFrameList = 0, startFrameListCount = 0;   // pool offset of uint32 frames
        uint32_t sprites = 0, spriteCount = 0;                   // pool offset of float rects x, y, w, h
        CurveParam custom[4];
        // stretched billboards
        float cameraScale = 0.0f, speedScale = 0.0f, lengthScale = 1.0f;
        uint8_t freezeInView = 0;
        uint8_t verticalEmitter = 0;
        uint8_t meshSelection = 0;
        MeshAlignment meshAlignment = MeshAlignment::Simulation;
        uint32_t meshCount = 0;
        float pivot[3] = {0, 0, 0};
        float flip[3] = {0, 0, 0};
        float minScreenSize = 0.0f, maxScreenSize = 0.5f;
        float sortingFudge = 0.0f;
        float emissive = 0.0f;
        float flipU = 0.0f, flipV = 0.0f;  // the texture sheet's shares mirrored
        PixelParams pixel;                 // S49
    };

    // ---------------------------------------------------------------- trails

    struct TrailParams {
        bool enabled = false;
        TrailMode mode = TrailMode::PerParticle;
        float ratio = 1.0f;
        uint32_t ribbonCount = 1;
        CurveParam lifetime;
        float minVertexDistance = 0.2f;
        uint32_t maxPoints = 16;
        bool worldSpace = true;
        bool dieWithParticles = true;
        CurveParam width;
        GradientParam colorOverLifetime;
        GradientParam colorOverTrail;
        bool inheritParticleColor = true;
        bool sizeAffectsWidth = true;
        bool sizeAffectsLifetime = false;
        TrailTextureMode textureMode = TrailTextureMode::Stretch;
    };

    // ---------------------------------------------------------------- the program

    class JASPERENGINE_API ParticleProgram {
    public:
        // the effect settings the runtime reads directly
        float duration = 5.0f;
        bool looping = true;
        bool prewarm = false;
        bool prewarmSync = false;
        CurveParam startDelay;         // seconds, picked per play (a range: by the seed)
        SimulationSpace space = SimulationSpace::Local;
        int tickRate = 30;
        bool interpolate = true;
        uint32_t maxParticles = 1000;
        uint32_t seed = 0;
        ScalingMode scalingMode = ScalingMode::Local;
        float simulationSpeed = 1.0f;
        bool useUnscaledTime = false;
        StopAction stopAction = StopAction::None;
        EmitterVelocityMode emitterVelocityMode = EmitterVelocityMode::Transform;
        GravitySource gravitySource = GravitySource::Physics3D;
        // the shape's Loop / Ping-Pong speeds (over the emitter time, one pick per play), and the
        // noise scroll speed when it is not a constant
        bool shapePhases = false;
        CurveParam arcSpeed;           // degrees per second
        CurveParam radiusSpeed;        // radius lengths per second
        bool noiseScroll = false;
        CurveParam noiseScrollSpeed;
        CullingMode cullingMode = CullingMode::Automatic;
        BoundsMode boundsMode = BoundsMode::Computed;
        float boundsCenter[3] = {0, 0, 0};
        float boundsSize[3] = {10, 10, 10};
        int priority = 0;
        bool lod = false;
        CurveParam lodEmission;        // over distance / lodMaxDistance
        CurveParam lodThinning;
        float lodMaxDistance = 100.0f;
        bool startSizeFromTexture = false;
        float maxStartSize = 1.0f;     // for the computed bounds and collision radii
        float maxSizeFactor = 1.0f;    // the most the size modules multiply it by
        float maxSpeedHint = 0.0f;

        ChannelMask channels = kCoreChannels;
        std::vector<Op> spawnOps;
        std::vector<Op> updateOps;
        EmissionParams emission;
        RenderParams render;
        TrailParams trails;
        bool hasCollision = false;
        CollisionKind collisionKind = CollisionKind::Planes;
        bool collisionEvents = false;
        uint32_t maxCollisionEvents = 64;
        bool hasExternalForces = false;
        uint32_t externalForceMask = 0xffffffffu;
        float externalForceMultiplier = 1.0f;
        bool subEmitters = false;      // the Sub Emitters module is on (the list is in the document)
        // which channels a sub-emitter's parent writes at the child's spawn (inherit)
        bool inheritsRotation = false;
        // Analytic (S49 R15): no channels; each system compiles an AnalyticProgram for its seed
        bool analytic = false;
        float maxLifetime = 0.0f;      // the longest start lifetime (Analytic's end of life)
        // a Follow Parent child (ChParentId; ChFollowQuat* with rotation, ChFollowSize with scale)
        bool followsParent = false;
        bool followsRotation = false;
        bool followsScale = false;

        // the parameter blocks and LUTs (16-byte aligned blocks of 32-bit words)
        std::vector<uint32_t> pool;

        template<class T> const T& param(uint32_t offset) const { return *reinterpret_cast<const T*>(reinterpret_cast<const unsigned char*>(pool.data()) + offset); }
        const float* floats(uint32_t offset) const { return reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(pool.data()) + offset); }
        const uint32_t* uints(uint32_t offset) const { return reinterpret_cast<const uint32_t*>(reinterpret_cast<const unsigned char*>(pool.data()) + offset); }

        bool has(Channel c) const { return (channels & channelBit(c)) != 0; }
        // same channels, ops, trails and capacity: the particles of one can run on the other
        bool sameStructure(const ParticleProgram& other) const;
        // the number of particles the effect can reach (OpenEngine's ContinuousEmissionPeak):
        // rate x longest lifetime plus the bursts, at most maxParticles
        uint32_t capacityHint() const;
        // that peak itself, unrounded and without the storage's spare room
        double peakEstimate() const;
        // bytes of channel storage per particle
        size_t bytesPerParticle() const;
    };

    // One compiled use of an exposed parameter's field: a curve or colour value as one call of the
    // compiler turned it into a CurveParam / GradientParam, and where the copies of that param sit
    // in the program, so a new value is written there without compiling again (S91 R10,
    // ParticleCompiler::applyExposed)
    struct ExposedSlot {
        bool gradient = false;
        uint8_t tag = 0;              // the copies' pad byte: unique in the program
        uint32_t salt = 0;            // their random stream
        float scale = 1.0f;
        bool startValue = false;
        MinMaxCurve curve;            // the value that call compiled (with the driver it gave it)
        MinMaxGradient color;
        // byte offsets of the copies in the pool, in the program object and in emission.bursts
        std::vector<uint32_t> inPool, inProgram, inBursts;
    };

    // An exposed parameter whose field is a curve or colour value: the slots of its uses
    struct ExposedBinding {
        uint32_t parameter = 0;       // its index in the document's exposed list
        std::string member;           // the member it writes ("", "constant", "multiplier", "color", ...)
        // every compiled use of the field is one of `slots` (none read its numbers otherwise)
        bool rewritable = true;
        std::vector<uint32_t> slots;
    };

    // the program and the document it came from (for diagnostics, sub-emitters, exposed names)
    struct CompiledEffect {
        ParticleEffectDocument document;
        ParticleProgram program;
        std::vector<Diagnostic> diagnostics;
        bool valid = false;
        // empty for an effect without exposed parameters
        std::vector<ExposedSlot> exposedSlots;
        std::vector<ExposedBinding> exposedBindings;
    };

}}

#endif // PARTICLES_PARTICLEPROGRAM_H
