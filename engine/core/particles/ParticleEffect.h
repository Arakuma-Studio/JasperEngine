// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLEEFFECT_H
#define PARTICLES_PARTICLEEFFECT_H

// The particle effect document (docs/specs/S20-particles.md R1, R2): what a `.particles` file
// holds (JSON, ParticleEffectCodec), and what a Particle System holds inline. A module is a
// plain struct; `present` modules show in the inspector (the others are hidden behind "+"),
// `enabled` ones run. Main and Renderer are always present. Every module's fields are listed
// once, in ParticleModules.cpp: the JSON codec, the inspector, MCP patches and the Lua
// exposed parameters all read that table.

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "particles/ParticleValues.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine { namespace particles {

    inline constexpr int kEffectSchemaVersion = 1;
    inline constexpr uint32_t kMaxParticlesPerEmitter = 1u << 20;
    inline constexpr size_t kMaxBursts = 16;
    inline constexpr size_t kMaxCollisionPlanes = 8;
    inline constexpr size_t kMaxSubEmitters = 8;
    inline constexpr int kMaxSubEmitterDepth = 3;
    inline constexpr size_t kMaxRendererMeshes = 4;
    inline constexpr size_t kMaxSpriteFrames = 64;
    inline constexpr uint32_t kMaxTrailPoints = 64;
    inline constexpr size_t kMaxSetOverAgeEntries = 16;
    inline constexpr size_t kMaxExposed = 32;

    enum class SimulationSpace : uint8_t { Local, World };
    enum class ScalingMode : uint8_t { Hierarchy, Local, Shape };
    enum class CullingMode : uint8_t { Automatic, Pause, PauseAndCatchUp, AlwaysSimulate };
    enum class BoundsMode : uint8_t { Computed, Authored };
    enum class ValueSpace : uint8_t { Local, World };
    // what the GameObject does when the effect has finished (Unity's Stop Action): not looping,
    // its last particle gone (or stopped and empty)
    enum class StopAction : uint8_t { None, Disable, Destroy, Callback };
    // the emitter velocity Inherit Velocity and Rate over Distance read (Unity's Emitter Velocity Mode)
    enum class EmitterVelocityMode : uint8_t { Transform, Rigidbody };
    // the world the Gravity Modifier scales (Unity's Gravity Source)
    enum class GravitySource : uint8_t { Physics3D, Physics2D };
    // Inherit Velocity at birth only, or the emitter's change of velocity every tick (Unity's mode)
    enum class InheritVelocityMode : uint8_t { Initial, Current };

    // ---------------------------------------------------------------- modules

    struct MainModule {
        MinMaxCurve startLifetime = MinMaxCurve(5.0f);
        MinMaxCurve startSpeed = MinMaxCurve(5.0f);
        bool startSize3D = false;
        MinMaxCurve startSize = MinMaxCurve(1.0f);      // X with startSize3D
        MinMaxCurve startSizeY = MinMaxCurve(1.0f);
        MinMaxCurve startSizeZ = MinMaxCurve(1.0f);
        // one random pick for the three axes of a 3D start size (the old scale initializer)
        bool startSizeLinked = false;
        bool startRotation3D = false;
        MinMaxCurve startRotation = MinMaxCurve(0.0f);  // degrees; Z with startRotation3D
        MinMaxCurve startRotationX = MinMaxCurve(0.0f);
        MinMaxCurve startRotationY = MinMaxCurve(0.0f);
        float flipRotation = 0.0f;                      // share of particles turning the other way
        MinMaxGradient startColor;
        MinMaxCurve gravityModifier = MinMaxCurve(0.0f);
        GravitySource gravitySource = GravitySource::Physics3D;
        MinMaxCurve inheritVelocity = MinMaxCurve(0.0f); // share of the emitter's velocity
        InheritVelocityMode inheritVelocityMode = InheritVelocityMode::Initial;
        // the old Points default (size 0): the start size is the texture's largest side
        bool startSizeFromTexture = false;
    };

    struct Burst {
        float time = 0.0f;
        // constants, a random range, or curves over the emitter time (read at the burst's time)
        MinMaxCurve count = MinMaxCurve(30.0f);
        int cycles = 1;           // 0 repeats for ever
        float interval = 0.01f;   // seconds between cycles
        float probability = 1.0f;
    };

    struct EmissionModule {
        bool present = true;
        bool enabled = true;
        MinMaxCurve rateOverTime = MinMaxCurve(10.0f);      // per second; curves over the emitter time
        MinMaxCurve rateOverDistance = MinMaxCurve(0.0f);   // per unit the emitter moves
        std::vector<Burst> bursts;
        int maxPerTick = 0;                                 // 0: no limit
    };

    enum class ShapeType : uint8_t { Sphere, Hemisphere, Cone, Box, Circle, Donut, Edge, Rectangle, Mesh };
    enum class ShapeEmitFrom : uint8_t { Volume, Shell, Edge, Base, Vertex, Triangle };
    enum class MeshSourceType : uint8_t { Primitive, Model, GameObject };
    enum class PrimitiveMesh : uint8_t { Quad, Cube, Sphere, Cylinder, Capsule, Torus, Plane };

    // A mesh a particle effect uses (Shape > Mesh, Renderer > Meshes)
    struct MeshSource {
        MeshSourceType type = MeshSourceType::Primitive;
        PrimitiveMesh primitive = PrimitiveMesh::Cube;
        std::string model;      // a model asset (.gltf, .glb, .obj), project-relative
        std::string modelGuid;  // its S04 GUID (32 hex), empty when unknown
        int submesh = 0;        // the mesh / primitive of the model
        bool operator==(const MeshSource& other) const {
            return type == other.type && primitive == other.primitive && model == other.model &&
                   modelGuid == other.modelGuid && submesh == other.submesh;
        }
        bool operator!=(const MeshSource& other) const { return !(*this == other); }
    };

    // How a shape picks the angle on its arc or the point along its radius (Unity's
    // ParticleSystemShapeMultiModeValue): at random, around it at Speed (Loop, Ping-Pong), or a
    // burst's particles spread evenly over it
    enum class ShapeMultiMode : uint8_t { Random, Loop, PingPong, BurstSpread };

    struct ShapeModule {
        bool present = true;
        bool enabled = true;
        ShapeType shape = ShapeType::Cone;
        ShapeEmitFrom emitFrom = ShapeEmitFrom::Base;
        float radius = 1.0f;
        float radiusThickness = 1.0f;   // 1 = the volume, 0 = the surface
        ShapeMultiMode radiusMode = ShapeMultiMode::Random;
        float radiusSpread = 0.0f;      // 0..1: positions only at multiples of it (0: anywhere)
        MinMaxCurve radiusSpeed = MinMaxCurve(1.0f);   // radius lengths per second (Loop, Ping-Pong)
        float angle = 25.0f;            // cone half-angle, degrees
        float length = 5.0f;            // cone length (Volume)
        float arc = 360.0f;             // degrees
        ShapeMultiMode arcMode = ShapeMultiMode::Random;
        float arcSpread = 0.0f;         // 0..1 of the arc
        MinMaxCurve arcSpeed = MinMaxCurve(1.0f);      // turns of the arc per second (Loop, Ping-Pong)
        float donutRadius = 0.2f;
        Vector3 boxThickness = Vector3(0.0f, 0.0f, 0.0f);
        MeshSource mesh;
        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 rotation = Vector3(0.0f, 0.0f, 0.0f);   // degrees
        Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);      // the box's size, the others' scale
        float randomizeDirection = 0.0f;
        float spherizeDirection = 0.0f;
        float randomizePosition = 0.0f;
        // particles start turned towards their direction (their up axis along it)
        bool alignToDirection = false;
    };

    struct StartVelocityModule {
        bool present = false;
        bool enabled = true;
        Vector3 min = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 max = Vector3(0.0f, 0.0f, 0.0f);
        bool uniform = false;   // one random pick for the three axes
    };

    struct VelocityOverLifetimeModule {
        bool present = false;
        bool enabled = true;
        MinMaxCurve x, y, z;
        ValueSpace space = ValueSpace::Local;
        MinMaxCurve orbitalX, orbitalY, orbitalZ;   // degrees per second around the offset
        // the orbit's centre from the emitter, over the particle's life (an older file's
        // "offset" vector reads as constants)
        MinMaxCurve offsetX, offsetY, offsetZ;
        MinMaxCurve radial;
        MinMaxCurve speedModifier = MinMaxCurve(1.0f);
    };

    struct LimitVelocityModule {
        bool present = false;
        bool enabled = true;
        bool separateAxes = false;
        MinMaxCurve limit = MinMaxCurve(1.0f);          // the speed; X with separateAxes
        MinMaxCurve limitY = MinMaxCurve(1.0f);
        MinMaxCurve limitZ = MinMaxCurve(1.0f);
        ValueSpace space = ValueSpace::Local;           // the axes of separateAxes
        float dampen = 1.0f;               // share of the excess speed removed per 1/30 s (1: clamped)
        MinMaxCurve drag = MinMaxCurve(0.0f);   // linear drag, 1/s
        float quadraticDrag = 0.0f;        // 1/unit
        bool multiplyBySize = true;
        bool multiplyByVelocity = false;   // the linear drag grows with the speed
    };

    struct ForceOverLifetimeModule {
        bool present = false;
        bool enabled = true;
        MinMaxCurve x, y, z;               // acceleration, units/s^2
        ValueSpace space = ValueSpace::Local;
        bool randomizePerTick = false;     // random modes pick again every tick
    };

    enum class NoiseQuality : uint8_t { Low, Medium, High };

    struct NoiseModule {
        bool present = false;
        bool enabled = true;
        bool separateAxes = false;
        MinMaxCurve strength = MinMaxCurve(1.0f);   // X with separateAxes
        MinMaxCurve strengthY = MinMaxCurve(1.0f);
        MinMaxCurve strengthZ = MinMaxCurve(1.0f);
        float frequency = 0.5f;
        MinMaxCurve scrollSpeed = MinMaxCurve(0.0f);   // over the emitter time
        bool damping = true;               // strength scales with 1/frequency
        int octaves = 1;
        float octaveMultiplier = 0.5f;
        float octaveScale = 2.0f;
        NoiseQuality quality = NoiseQuality::High;
        MinMaxCurve positionAmount = MinMaxCurve(1.0f);
        MinMaxCurve rotationAmount = MinMaxCurve(0.0f);
        MinMaxCurve sizeAmount = MinMaxCurve(0.0f);
    };

    struct OrbitModule {
        bool present = false;
        bool enabled = true;
        Vector3 axis = Vector3(0.0f, 1.0f, 0.0f);
        Vector3 center = Vector3(0.0f, 0.0f, 0.0f);
        MinMaxCurve angularSpeed = MinMaxCurve(90.0f);   // degrees per second
        MinMaxCurve radialPull = MinMaxCurve(0.0f);      // units per second toward the axis
        ValueSpace space = ValueSpace::Local;
    };

    struct ExternalForcesModule {
        bool present = false;
        bool enabled = true;
        float multiplier = 1.0f;
        uint32_t layerMask = 0xffffffffu;
    };

    struct ColorOverLifetimeModule {
        bool present = false;
        bool enabled = true;
        MinMaxGradient color;
        // the gradient replaces the start colour's RGB instead of multiplying it (old
        // colour gradients); it is evaluated in the tick and stored
        bool setMode = false;
    };

    struct ColorBySpeedModule {
        bool present = false;
        bool enabled = true;
        MinMaxGradient color;
        Vector2 speedRange = Vector2(0.0f, 1.0f);
    };

    struct SizeOverLifetimeModule {
        bool present = false;
        bool enabled = true;
        bool separateAxes = false;
        MinMaxCurve size = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f));
        MinMaxCurve sizeY = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f));
        MinMaxCurve sizeZ = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f));
    };

    struct SizeBySpeedModule {
        bool present = false;
        bool enabled = true;
        bool separateAxes = false;
        MinMaxCurve size = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f));
        MinMaxCurve sizeY = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f));
        MinMaxCurve sizeZ = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f));
        Vector2 speedRange = Vector2(0.0f, 1.0f);
    };

    struct RotationOverLifetimeModule {
        bool present = false;
        bool enabled = true;
        bool separateAxes = false;
        MinMaxCurve angularVelocity = MinMaxCurve(45.0f);   // degrees per second (Z)
        MinMaxCurve angularVelocityX = MinMaxCurve(0.0f);
        MinMaxCurve angularVelocityY = MinMaxCurve(0.0f);
    };

    struct RotationBySpeedModule {
        bool present = false;
        bool enabled = true;
        bool separateAxes = false;
        MinMaxCurve angularVelocity = MinMaxCurve(45.0f);   // degrees per second (Z)
        MinMaxCurve angularVelocityX = MinMaxCurve(0.0f);
        MinMaxCurve angularVelocityY = MinMaxCurve(0.0f);
        Vector2 speedRange = Vector2(0.0f, 1.0f);
    };

    enum class TextureSheetMode : uint8_t { Grid, Sprites };
    enum class TextureSheetAnimation : uint8_t { WholeSheet, SingleRow };
    enum class TextureSheetRowMode : uint8_t { Custom, Random };
    enum class TextureSheetTimeMode : uint8_t { Lifetime, FPS, Speed };

    struct SpriteRect {
        float x = 0.0f, y = 0.0f, width = 1.0f, height = 1.0f;   // in texture UV (0 to 1)
        bool operator==(const SpriteRect& o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
        bool operator!=(const SpriteRect& o) const { return !(*this == o); }
    };

    struct TextureSheetAnimationModule {
        bool present = false;
        bool enabled = true;
        TextureSheetMode mode = TextureSheetMode::Grid;
        int tilesX = 1;
        int tilesY = 1;
        TextureSheetAnimation animation = TextureSheetAnimation::WholeSheet;
        TextureSheetRowMode rowMode = TextureSheetRowMode::Custom;
        int rowIndex = 0;
        std::vector<SpriteRect> sprites;
        TextureSheetTimeMode timeMode = TextureSheetTimeMode::Lifetime;
        MinMaxCurve frameOverTime = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f));   // 0 to 1 of the frames
        float fps = 10.0f;
        Vector2 speedRange = Vector2(0.0f, 1.0f);
        MinMaxCurve startFrame = MinMaxCurve(0.0f);   // frames
        float cycles = 1.0f;
        float flipU = 0.0f;        // 0..1: the share of particles mirrored horizontally
        float flipV = 0.0f;
        bool frameBlending = false;
        // old sprite initializers: the start frame is picked from this list
        std::vector<int> startFrameList;
    };

    enum class CollisionType : uint8_t { Planes, World, Terrain, World2D };
    enum class CollisionQuality : uint8_t { Low, Medium, High };

    struct CollisionPlane {
        Vector3 point = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
        bool operator==(const CollisionPlane& o) const { return point == o.point && normal == o.normal; }
        bool operator!=(const CollisionPlane& o) const { return !(*this == o); }
    };

    struct CollisionModule {
        bool present = false;
        bool enabled = true;
        CollisionType type = CollisionType::Planes;
        std::vector<CollisionPlane> planes;   // literal planes, in simulation space
        MinMaxCurve dampen = MinMaxCurve(0.0f);
        MinMaxCurve bounce = MinMaxCurve(1.0f);
        MinMaxCurve lifetimeLoss = MinMaxCurve(0.0f);
        float minKillSpeed = 0.0f;
        float maxKillSpeed = 10000.0f;
        float radiusScale = 1.0f;
        bool killOnCollide = false;
        uint32_t collidesWith = 0xffffffffu;
        CollisionQuality quality = CollisionQuality::Medium;
        int maxQueriesPerTick = 256;
        int planeCacheTicks = 8;
        bool sendEvents = false;
        int maxCollisionEvents = 64;
        // a hit whose speed after the response is below it makes the particle rest until its
        // life ends (S49 R14; 0: off)
        float settleSpeed = 0.0f;
    };

    enum class TrailMode : uint8_t { PerParticle, Ribbon };
    enum class TrailTextureMode : uint8_t { Stretch, Tile, DistributePerSegment };

    struct TrailsModule {
        bool present = false;
        bool enabled = true;
        TrailMode mode = TrailMode::PerParticle;
        float ratio = 1.0f;
        int ribbonCount = 1;
        MinMaxCurve lifetime = MinMaxCurve(1.0f);   // seconds a point lasts
        float minVertexDistance = 0.2f;
        int maxPoints = 16;
        bool worldSpace = true;
        bool dieWithParticles = true;
        MinMaxCurve widthOverTrail = MinMaxCurve(1.0f);   // over the trail, 0 head to 1 tail
        MinMaxGradient colorOverLifetime;
        MinMaxGradient colorOverTrail;
        bool inheritParticleColor = true;
        bool sizeAffectsWidth = true;
        bool sizeAffectsLifetime = false;
        TrailTextureMode textureMode = TrailTextureMode::Stretch;
        bool generateLightingData = false;
    };

    struct SubEmittersModule {
        bool present = false;
        bool enabled = true;
    };

    struct CustomDataModule {
        bool present = false;
        bool enabled = true;
        MinMaxCurve data[4];
    };

    enum class RenderMode : uint8_t {
        Billboard,               // faces the camera plane
        FacingCameraPosition,    // faces the camera position
        StretchedBillboard,
        HorizontalBillboard,
        VerticalBillboard,
        WorldOriented,
        LocalOriented,
        Mesh,
        None,                    // trails only
        VelocityAligned,         // faces along the velocity (Unity's Velocity alignment)
        Pixel                    // a pixel shape drawn per base pixel on the grid (S49 R9)
    };
    // How Mesh particles turn (Unity's Render Alignment): in the simulation space (the
    // effect's own default), facing the camera plane or position, in world or emitter axes, or
    // their +Z along the velocity
    enum class MeshAlignment : uint8_t { Simulation, View, World, Local, Facing, Velocity };
    enum class VerticalLock : uint8_t { World, Emitter };
    enum class MeshSelection : uint8_t { Random, Index };
    enum class ParticleBlend : uint8_t { Alpha, Premultiplied, Additive, Multiply };
    enum class SortMode : uint8_t { None, ByDistance, OldestInFront, YoungestInFront, ByDepth };
    enum class LightingMode : uint8_t { Unlit, Lit, SixWay };
    // the three map layouts of OpenEngine's particle_six_way.glsl
    enum class SixWayLayout : uint8_t { RGBA, TopLeftRightBottomBackFront, PositiveNegative };
    // a billboard without a texture: Unity's default particle (a soft disc), or the old points' square
    enum class UntexturedShape : uint8_t { Disc, Square };

    // ---------------------------------------------------------------- pixel art (S49)

    // the shapes of the Pixel render mode, drawn per whole base pixel (S49 R9)
    enum class PixelShape : uint8_t { Dot, Cross, Disc, Ring, Streak, Puff, Shard, Chunk, Glow };
    // how a particle fades: a Bayer dither, a cutout at half, or S20's alpha blending
    enum class PixelFade : uint8_t { Dither, Cutout, Alpha };
    // the dither cell: the target's base pixels (the pattern holds still), or the particle's
    enum class DitherSpace : uint8_t { Screen, Particle };
    enum class SecondaryColorMode : uint8_t { RampOffset, Color };
    // Analytic: every particle a closed-form function of its slot and the time (S49 R15)
    enum class SimulationMode : uint8_t { Simulated, Analytic };

    struct PixelArtModule {
        bool present = false;
        bool enabled = true;
        bool snap = true;               // the lower-left corner of the pixel box on the grid
        bool pixelSizes = true;         // sizes in base pixels
        bool integerSize = true;        // whole-pixel boxes
        PixelFade fade = PixelFade::Dither;
        int ditherSize = 4;             // 2, 4 or 8
        DitherSpace ditherSpace = DitherSpace::Screen;
        MinMaxCurve fadeOverLifetime = MinMaxCurve(1.0f);   // times the colour's alpha
        float holdFrameRate = 0.0f;     // Hz; 0: every frame
        SecondaryColorMode secondary = SecondaryColorMode::RampOffset;
        int secondaryOffset = 1;        // ramp steps from the primary
        Vector4 secondaryColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);   // sRGB
        Vector2 wobbleAmplitude = Vector2(0.0f, 0.0f);          // base px
        Vector2 wobbleFrequency = Vector2(1.0f, 1.0f);          // Hz, per axis
        bool wobbleRandomPhase = true;
        float flickerBelow = 0.0f;      // the remaining life under which it blinks (0: off)
        float flickerRate = 40.0f;      // Hz
    };

    struct ShaderProperty {
        std::string name;
        Vector4 value = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        int components = 1;   // 1 float, 4 vec4/colour
        bool operator==(const ShaderProperty& o) const { return name == o.name && value == o.value && components == o.components; }
        bool operator!=(const ShaderProperty& o) const { return !(*this == o); }
    };

    struct ShaderTexture {
        std::string name;    // the @texture uniform
        std::string path;
        std::string guid;
        bool operator==(const ShaderTexture& o) const { return name == o.name && path == o.path && guid == o.guid; }
        bool operator!=(const ShaderTexture& o) const { return !(*this == o); }
    };

    struct RendererModule {
        bool present = true;
        bool enabled = true;
        RenderMode renderMode = RenderMode::Billboard;
        float cameraScale = 0.0f;
        float speedScale = 0.0f;
        float lengthScale = 2.0f;
        bool freezeInView = false;
        VerticalLock verticalLock = VerticalLock::World;
        std::vector<MeshSource> meshes;
        MeshSelection meshSelection = MeshSelection::Random;
        // (an older file's alignToVelocity reads as Velocity)
        MeshAlignment meshAlignment = MeshAlignment::Simulation;
        Vector3 pivot = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 flip = Vector3(0.0f, 0.0f, 0.0f);
        std::string shader = "library:particles";
        std::vector<std::string> shaderFeatures;
        std::vector<ShaderProperty> shaderProperties;
        std::vector<ShaderTexture> shaderTextures;
        std::string texture;
        std::string textureGuid;
        // the texture's UVs (and the sheet's frames) scaled by xy, then offset by zw (a
        // material's tiling and offset)
        Vector4 textureTiling = Vector4(1.0f, 1.0f, 0.0f, 0.0f);
        UntexturedShape untexturedShape = UntexturedShape::Disc;
        std::string trailTexture;
        std::string trailTextureGuid;
        ParticleBlend blend = ParticleBlend::Alpha;
        bool depthWrite = false;
        SortMode sortMode = SortMode::None;
        float sortingFudge = 0.0f;
        int sortPriority = 0;
        float minScreenSize = 0.0f;
        float maxScreenSize = 0.5f;
        uint32_t renderLayers = 0xffffffffu;
        bool receiveShadows = false;
        LightingMode lighting = LightingMode::Unlit;
        SixWayLayout sixWayLayout = SixWayLayout::RGBA;
        std::string sixWayMapA;
        std::string sixWayMapAGuid;
        std::string sixWayMapB;
        std::string sixWayMapBGuid;
        float sixWayContrast = 1.0f;
        std::string emissionMap;
        std::string emissionMapGuid;
        Vector4 emissionColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);   // sRGB
        float emissionIntensity = 0.0f;
        // the Pixel render mode's shape and its settings (S49 R9)
        PixelShape pixelShape = PixelShape::Dot;
        float streakLengthScale = 0.022f;   // px of length per px/s
        float streakMaxLength = 9.0f;
        float streakHeadFraction = 0.45f;
        int streakWidth = 1;
        bool discRim = false;
        int ringWidth = 1;
        bool dotShadeBottom = false;
        int glowBands = 3;
        int rotationSteps = 16;             // Shard and Chunk: angles per turn
    };

    // Old modifiers (ActionSystem's "from-to" per property, before S20): inside the window of
    // ages [fromTime, toTime] the property is set to lerp(from, to, curve((age - from) / span));
    // outside it, it keeps its last value
    enum class SetProperty : uint8_t { Position, Velocity, Acceleration, Color, Alpha, Size, Rotation, Scale, Frame };

    struct SetOverAgeEntry {
        SetProperty property = SetProperty::Position;
        float fromTime = 0.0f;
        float toTime = 0.0f;
        Curve curve = Curve::linear(0.0f, 1.0f);   // the ease, over the window
        Vector4 from = Vector4(0.0f, 0.0f, 0.0f, 0.0f);   // rotation: a quaternion (x, y, z, w)
        Vector4 to = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        bool shortestPath = false;    // Rotation
        bool srgb = true;             // Color: sRGB values
        std::vector<int> frames;      // Frame: sprite frames picked along the window
    };

    struct SetOverAgeModule {
        bool present = false;
        bool enabled = true;
        std::vector<SetOverAgeEntry> entries;
    };

    // ---------------------------------------------------------------- effect

    enum class SubEmitterTrigger : uint8_t { Birth, Death, Collision, Trigger };

    struct SubEmitterEntry {
        SubEmitterTrigger trigger = SubEmitterTrigger::Death;
        std::string effect;         // a .particles asset
        std::string effectGuid;
        std::string triggerName;    // Trigger: the name sendTrigger() raises
        bool inheritColor = false;
        bool inheritSize = false;
        bool inheritRotation = false;
        bool inheritLifetime = false;
        bool inheritVelocity = false;
        float inheritFactor = 1.0f; // scales the inherited size, lifetime and velocity
        int emitCount = 1;
        float probability = 1.0f;
    };

    // A named parameter scripts and the component set (setFloat("Intensity", 2)): it writes
    // the module field at `path` ("emission.rateOverTime", "main.startColor.color")
    enum class ExposedType : uint8_t { Float, Vector3, Color };

    struct ExposedParameter {
        std::string name;
        std::string path;
        ExposedType type = ExposedType::Float;
    };

    struct DistanceLod {
        bool enabled = false;
        // emission and the share of particles drawn over the camera distance (0 to maxDistance)
        MinMaxCurve emissionScale = MinMaxCurve(1.0f);
        MinMaxCurve renderThinning = MinMaxCurve(1.0f);
        float maxDistance = 100.0f;   // beyond it the emitter is culled
    };

    struct ParticleEffectDocument {
        int version = kEffectSchemaVersion;
        float duration = 5.0f;
        bool looping = true;
        bool prewarm = false;
        bool prewarmSync = false;
        MinMaxCurve startDelay = MinMaxCurve(0.0f);   // seconds; a random range picks per play
        SimulationSpace simulationSpace = SimulationSpace::Local;
        int tickRate = 30;           // ticks per second, 0: a bounded variable step
        bool interpolate = true;
        uint32_t maxParticles = 1000;
        uint32_t seed = 0;           // 0: each Particle System picks its own
        ScalingMode scalingMode = ScalingMode::Local;
        float simulationSpeed = 1.0f;
        // ticks with the unscaled frame time (Time.timeScale does not slow it: UI, pause menus)
        bool useUnscaledTime = false;
        StopAction stopAction = StopAction::None;
        EmitterVelocityMode emitterVelocityMode = EmitterVelocityMode::Transform;
        CullingMode cullingMode = CullingMode::Automatic;
        BoundsMode boundsMode = BoundsMode::Computed;
        Vector3 boundsCenter = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 boundsSize = Vector3(10.0f, 10.0f, 10.0f);
        int priority = 0;            // the world budget serves higher priorities first
        DistanceLod lod;
        SimulationMode simulationMode = SimulationMode::Simulated;
        // the project Pixels Per Unit a pixel preset was made for (0: none; S49 R20)
        float designPixelsPerUnit = 0.0f;

        MainModule main;
        EmissionModule emission;
        ShapeModule shape;
        StartVelocityModule startVelocity;
        VelocityOverLifetimeModule velocityOverLifetime;
        LimitVelocityModule limitVelocity;
        ForceOverLifetimeModule forceOverLifetime;
        NoiseModule noise;
        OrbitModule orbit;
        ExternalForcesModule externalForces;
        ColorOverLifetimeModule colorOverLifetime;
        ColorBySpeedModule colorBySpeed;
        SizeOverLifetimeModule sizeOverLifetime;
        SizeBySpeedModule sizeBySpeed;
        RotationOverLifetimeModule rotationOverLifetime;
        RotationBySpeedModule rotationBySpeed;
        TextureSheetAnimationModule textureSheetAnimation;
        CollisionModule collision;
        TrailsModule trails;
        SubEmittersModule subEmittersModule;
        CustomDataModule customData;
        RendererModule renderer;
        SetOverAgeModule setOverAge;
        PixelArtModule pixelArt;

        std::vector<SubEmitterEntry> subEmitters;
        std::vector<ExposedParameter> exposed;
    };

    // A finding of the validation: the JSON path of the field and what is wrong with it
    struct Diagnostic {
        enum class Severity : uint8_t { Error, Warning };
        Severity severity = Severity::Error;
        std::string path;      // "modules[2].bursts[0].probability", "$" for the document
        std::string message;
        // a typed finding ("analytic_unsupported") and how to fix it; empty for the others
        std::string code;
        std::string fix;
    };

    // A .particles JSON text into a document, for code that cannot include the JSON library
    // (JasperEngine.h, exported scenes): ParticleEffectCodec::read without its diagnostics
    JASPERENGINE_API bool readEffectText(const std::string& text, ParticleEffectDocument& document);

}}

#endif // PARTICLES_PARTICLEEFFECT_H
