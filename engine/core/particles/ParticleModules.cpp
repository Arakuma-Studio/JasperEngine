// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleModules.h"

#include "pixelart/PaletteRampTexture.h"
#include "action/Ease.h"

#include <cstdlib>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::particles;

#define JE_ACC(T, member) [](void* o) -> void* { return &static_cast<T*>(o)->member; }
#define JE_IF(T, expr) [](const void* o) -> bool { const T& m = *static_cast<const T*>(o); (void)m; return (expr); }

namespace {

    template<class T>
    const T& defaultOf(){
        static const T value{};
        return value;
    }

    template<class T>
    StructDesc structOf(const char* name){
        StructDesc s;
        s.name = name;
        s.defaults = &defaultOf<T>();
        return s;
    }

    FieldDesc field(const char* key, const char* label, FieldType type, void* (*get)(void*), uint16_t flags = FieldFlag_None, const char* tooltip = nullptr){
        FieldDesc d;
        d.key = key;
        d.label = label;
        d.type = type;
        d.get = get;
        d.flags = flags;
        d.tooltip = tooltip;
        return d;
    }

    FieldDesc ranged(FieldDesc d, float lo, float hi){
        d.minValue = lo;
        d.maxValue = hi;
        return d;
    }

    FieldDesc shownIf(FieldDesc d, bool (*visible)(const void*)){
        d.visible = visible;
        return d;
    }

    template<size_t N>
    FieldDesc enumField(const char* key, const char* label, void* (*get)(void*), const EnumOption (&options)[N], uint16_t flags = FieldFlag_None, const char* tooltip = nullptr){
        FieldDesc d = field(key, label, FieldType::Enum, get, flags, tooltip);
        d.options = options;
        d.optionCount = (uint8_t)N;
        return d;
    }

    template<class V>
    ArrayOps arrayOps(){
        ArrayOps ops;
        ops.size = [](const void* v) -> size_t { return static_cast<const V*>(v)->size(); };
        ops.resize = [](void* v, size_t n){ static_cast<V*>(v)->resize(n); };
        ops.at = [](void* v, size_t i) -> void* { return &(*static_cast<V*>(v))[i]; };
        ops.erase = [](void* v, size_t i){ V& vector = *static_cast<V*>(v); vector.erase(vector.begin() + (std::ptrdiff_t)i); };
        return ops;
    }

    template<class V>
    FieldDesc arrayField(const char* key, const char* label, void* (*get)(void*), const StructDesc* element, uint32_t maxCount, uint16_t flags = FieldFlag_None, const char* tooltip = nullptr){
        FieldDesc d = field(key, label, FieldType::Array, get, flags, tooltip);
        d.element = element;
        d.array = arrayOps<V>();
        d.maxCount = maxCount;
        return d;
    }

    FieldDesc intArrayField(const char* key, const char* label, void* (*get)(void*), uint32_t maxCount, uint16_t flags = FieldFlag_None, const char* tooltip = nullptr){
        FieldDesc d = field(key, label, FieldType::IntArray, get, flags, tooltip);
        d.array = arrayOps<std::vector<int>>();
        d.maxCount = maxCount;
        return d;
    }

    FieldDesc stringArrayField(const char* key, const char* label, void* (*get)(void*), uint32_t maxCount, const char* tooltip = nullptr){
        FieldDesc d = field(key, label, FieldType::StringArray, get, FieldFlag_None, tooltip);
        d.array = arrayOps<std::vector<std::string>>();
        d.maxCount = maxCount;
        return d;
    }

    FieldDesc structField(const char* key, const char* label, void* (*get)(void*), const StructDesc* element, uint16_t flags = FieldFlag_None){
        FieldDesc d = field(key, label, FieldType::Struct, get, flags);
        d.element = element;
        return d;
    }

    // ---------------------------------------------------------------- enum tables

    const EnumOption kSimulationSpaces[] = {{"local", "Local", 0}, {"world", "World", 1}};
    const EnumOption kScalingModes[] = {{"hierarchy", "Hierarchy", 0}, {"local", "Local", 1}, {"shape", "Shape", 2}};
    const EnumOption kCullingModes[] = {{"automatic", "Automatic", 0}, {"pause", "Pause", 1}, {"pauseAndCatchUp", "Pause and Catch-up", 2}, {"alwaysSimulate", "Always Simulate", 3}};
    const EnumOption kBoundsModes[] = {{"computed", "Computed", 0}, {"authored", "Authored", 1}};
    const EnumOption kValueSpaces[] = {{"local", "Local", 0}, {"world", "World", 1}};
    const EnumOption kStopActions[] = {{"none", "None", 0}, {"disable", "Disable", 1}, {"destroy", "Destroy", 2}, {"callback", "Callback", 3}};
    const EnumOption kEmitterVelocityModes[] = {{"transform", "Transform", 0}, {"rigidbody", "Rigidbody", 1}};
    const EnumOption kGravitySources[] = {{"physics3D", "3D Physics", 0}, {"physics2D", "2D Physics", 1}};
    const EnumOption kInheritModes[] = {{"initial", "Initial", 0}, {"current", "Current", 1}};
    const EnumOption kShapeMultiModes[] = {{"random", "Random", 0}, {"loop", "Loop", 1}, {"pingPong", "Ping-Pong", 2}, {"burstSpread", "Burst Spread", 3}};
    const EnumOption kMeshAlignments[] = {{"simulation", "Simulation Space", 0}, {"view", "View", 1}, {"world", "World", 2},
                                          {"local", "Local", 3}, {"facing", "Facing", 4}, {"velocity", "Velocity", 5}};
    const EnumOption kShapeTypes[] = {{"sphere", "Sphere", 0}, {"hemisphere", "Hemisphere", 1}, {"cone", "Cone", 2}, {"box", "Box", 3},
                                      {"circle", "Circle", 4}, {"donut", "Donut", 5}, {"edge", "Edge", 6}, {"rectangle", "Rectangle", 7}, {"mesh", "Mesh", 8}};
    const EnumOption kEmitFrom[] = {{"volume", "Volume", 0}, {"shell", "Shell", 1}, {"edge", "Edge", 2}, {"base", "Base", 3}, {"vertex", "Vertex", 4}, {"triangle", "Triangle", 5}};
    const EnumOption kMeshSourceTypes[] = {{"primitive", "Primitive", 0}, {"model", "Model", 1}, {"gameObject", "This GameObject's Mesh", 2}};
    const EnumOption kPrimitives[] = {{"quad", "Quad", 0}, {"cube", "Cube", 1}, {"sphere", "Sphere", 2}, {"cylinder", "Cylinder", 3},
                                      {"capsule", "Capsule", 4}, {"torus", "Torus", 5}, {"plane", "Plane", 6}};
    const EnumOption kNoiseQualities[] = {{"low", "Low", 0}, {"medium", "Medium", 1}, {"high", "High", 2}};
    const EnumOption kSheetModes[] = {{"grid", "Grid", 0}, {"sprites", "Sprites", 1}};
    const EnumOption kSheetAnimations[] = {{"wholeSheet", "Whole Sheet", 0}, {"singleRow", "Single Row", 1}};
    const EnumOption kSheetRowModes[] = {{"custom", "Custom", 0}, {"random", "Random", 1}};
    const EnumOption kSheetTimeModes[] = {{"lifetime", "Lifetime", 0}, {"fps", "FPS", 1}, {"speed", "Speed", 2}};
    const EnumOption kCollisionTypes[] = {{"planes", "Planes", 0}, {"world", "World", 1}, {"terrain", "Terrain", 2}, {"world2D", "World 2D", 3}};
    const EnumOption kCollisionQualities[] = {{"low", "Low", 0}, {"medium", "Medium", 1}, {"high", "High", 2}};
    const EnumOption kTrailModes[] = {{"perParticle", "Particles", 0}, {"ribbon", "Ribbon", 1}};
    const EnumOption kTrailTextureModes[] = {{"stretch", "Stretch", 0}, {"tile", "Tile", 1}, {"distributePerSegment", "Distribute Per Segment", 2}};
    const EnumOption kRenderModes[] = {{"billboard", "Billboard", 0}, {"facingCameraPosition", "Facing Camera Position", 1},
                                       {"stretchedBillboard", "Stretched Billboard", 2}, {"horizontalBillboard", "Horizontal Billboard", 3},
                                       {"verticalBillboard", "Vertical Billboard", 4}, {"worldOriented", "World Oriented", 5},
                                       {"localOriented", "Local Oriented", 6}, {"mesh", "Mesh", 7}, {"none", "None", 8},
                                       {"velocityAligned", "Velocity Aligned", 9}, {"ring", "Ring", 10}, {"pixel", "Pixel", 11}};
    const EnumOption kVerticalLocks[] = {{"world", "World Y", 0}, {"emitter", "Emitter Y", 1}};
    const EnumOption kMeshSelections[] = {{"random", "Random", 0}, {"index", "By Spawn Index", 1}};
    const EnumOption kUntexturedShapes[] = {{"disc", "Soft Disc", 0}, {"square", "Square", 1}};
    const EnumOption kBlends[] = {{"alpha", "Alpha", 0}, {"premultiplied", "Premultiplied", 1}, {"additive", "Additive", 2}, {"multiply", "Multiply", 3}};
    const EnumOption kSortModes[] = {{"none", "None", 0}, {"byDistance", "By Distance", 1}, {"oldestInFront", "Oldest in Front", 2},
                                     {"youngestInFront", "Youngest in Front", 3}, {"byDepth", "By Depth", 4}};
    const EnumOption kLightingModes[] = {{"unlit", "Unlit", 0}, {"lit", "Lit", 1}, {"sixWay", "Six-Way", 2}};
    const EnumOption kSixWayLayouts[] = {{"rgba", "RGBA (A: right, top, back, opacity; B: left, bottom, front, emission)", 0},
                                         {"topLeftRightBottomBackFront", "Top Left Right / Bottom Back Front", 1},
                                         {"positiveNegative", "Positive / Negative axes", 2}};
    const EnumOption kSetProperties[] = {{"position", "Position", 0}, {"velocity", "Velocity", 1}, {"acceleration", "Acceleration", 2},
                                         {"color", "Color", 3}, {"alpha", "Alpha", 4}, {"size", "Size", 5}, {"rotation", "Rotation", 6},
                                         {"scale", "Scale", 7}, {"frame", "Frame", 8}};
    const EnumOption kSubEmitterTriggers[] = {{"birth", "Birth", 0}, {"death", "Death", 1}, {"collision", "Collision", 2}, {"trigger", "Trigger", 3}};
    const EnumOption kExposedTypes[] = {{"float", "Float", 0}, {"vector3", "Vector3", 1}, {"color", "Color", 2}};
    // pixel art (S49)
    const EnumOption kPixelShapes[] = {{"dot", "Dot", 0}, {"cross", "Cross", 1}, {"disc", "Disc", 2}, {"ring", "Ring", 3}, {"streak", "Streak", 4},
                                       {"puff", "Puff", 5}, {"shard", "Shard", 6}, {"chunk", "Chunk", 7}, {"glow", "Glow", 8}};
    const EnumOption kPixelFades[] = {{"dither", "Dither", 0}, {"cutout", "Cutout", 1}, {"alpha", "Alpha", 2}};
    const EnumOption kDitherSpaces[] = {{"screen", "Screen", 0}, {"particle", "Particle", 1}};
    const EnumOption kSecondaryModes[] = {{"rampOffset", "Ramp Offset", 0}, {"color", "Color", 1}};
    const EnumOption kSimulationModes[] = {{"simulated", "Simulated", 0}, {"analytic", "Analytic", 1}};
    const EnumOption kTriggerActions[] = {{"none", "Nothing", 0}, {"play", "Play", 1}, {"pause", "Pause", 2}, {"stop", "Stop", 3},
                                          {"stopAndClear", "Stop and Clear", 4}};

    // ---------------------------------------------------------------- nested structs

    StructDesc makeMeshSource(){
        StructDesc s = structOf<MeshSource>("MeshSource");
        s.fields.push_back(enumField("type", "Source", JE_ACC(MeshSource, type), kMeshSourceTypes));
        s.fields.push_back(shownIf(enumField("primitive", "Primitive", JE_ACC(MeshSource, primitive), kPrimitives), JE_IF(MeshSource, m.type == MeshSourceType::Primitive)));
        s.fields.push_back(shownIf(field("model", "Model", FieldType::String, JE_ACC(MeshSource, model), FieldFlag_AssetPath), JE_IF(MeshSource, m.type == MeshSourceType::Model)));
        s.fields.push_back(field("modelGuid", "Model GUID", FieldType::String, JE_ACC(MeshSource, modelGuid), FieldFlag_Guid | FieldFlag_Hidden));
        s.fields.push_back(shownIf(ranged(field("submesh", "Submesh", FieldType::Int, JE_ACC(MeshSource, submesh)), 0, 1024), JE_IF(MeshSource, m.type == MeshSourceType::Model)));
        return s;
    }
    const StructDesc& meshSourceDesc(){ static const StructDesc s = makeMeshSource(); return s; }

    StructDesc makeBurst(){
        StructDesc s = structOf<Burst>("Burst");
        s.fields.push_back(ranged(field("time", "Time", FieldType::Float, JE_ACC(Burst, time)), 0.0f, 1.0e6f));
        s.fields.push_back(ranged(field("count", "Count", FieldType::MinMaxCurve, JE_ACC(Burst, count)), 0.0f, 1.0e6f));
        s.fields.push_back(ranged(field("cycles", "Cycles", FieldType::Int, JE_ACC(Burst, cycles), FieldFlag_None, "0 repeats for ever"), 0, 100000));
        s.fields.push_back(ranged(field("interval", "Interval", FieldType::Float, JE_ACC(Burst, interval)), 0.0001f, 1.0e6f));
        s.fields.push_back(ranged(field("probability", "Probability", FieldType::Float, JE_ACC(Burst, probability)), 0.0f, 1.0f));
        s.fields.push_back(field("soundEvent", "Sound Event", FieldType::String, JE_ACC(Burst, soundEvent), FieldFlag_AssetPath,
                                 "a .soundevent played at the emitter each time the burst fires"));
        s.fields.push_back(field("soundEventGuid", "Sound Event GUID", FieldType::String, JE_ACC(Burst, soundEventGuid), FieldFlag_Guid | FieldFlag_Hidden));
        return s;
    }
    const StructDesc& burstDesc(){ static const StructDesc s = makeBurst(); return s; }

    StructDesc makeSpriteRect(){
        StructDesc s = structOf<SpriteRect>("SpriteRect");
        s.fields.push_back(ranged(field("x", "X", FieldType::Float, JE_ACC(SpriteRect, x)), 0.0f, 1.0f));
        s.fields.push_back(ranged(field("y", "Y", FieldType::Float, JE_ACC(SpriteRect, y)), 0.0f, 1.0f));
        s.fields.push_back(ranged(field("width", "Width", FieldType::Float, JE_ACC(SpriteRect, width)), 0.0f, 1.0f));
        s.fields.push_back(ranged(field("height", "Height", FieldType::Float, JE_ACC(SpriteRect, height)), 0.0f, 1.0f));
        return s;
    }
    const StructDesc& spriteRectDesc(){ static const StructDesc s = makeSpriteRect(); return s; }

    StructDesc makePlane(){
        StructDesc s = structOf<CollisionPlane>("CollisionPlane");
        s.fields.push_back(field("point", "Point", FieldType::Vector3, JE_ACC(CollisionPlane, point)));
        s.fields.push_back(field("normal", "Normal", FieldType::Vector3, JE_ACC(CollisionPlane, normal)));
        return s;
    }
    const StructDesc& planeDesc(){ static const StructDesc s = makePlane(); return s; }

    StructDesc makeShaderProperty(){
        StructDesc s = structOf<ShaderProperty>("ShaderProperty");
        s.fields.push_back(field("name", "Name", FieldType::String, JE_ACC(ShaderProperty, name)));
        s.fields.push_back(field("value", "Value", FieldType::Vector4, JE_ACC(ShaderProperty, value)));
        s.fields.push_back(ranged(field("components", "Components", FieldType::Int, JE_ACC(ShaderProperty, components)), 1, 4));
        return s;
    }
    const StructDesc& shaderPropertyDesc(){ static const StructDesc s = makeShaderProperty(); return s; }

    StructDesc makeShaderTexture(){
        StructDesc s = structOf<ShaderTexture>("ShaderTexture");
        s.fields.push_back(field("name", "Name", FieldType::String, JE_ACC(ShaderTexture, name)));
        s.fields.push_back(field("path", "Texture", FieldType::String, JE_ACC(ShaderTexture, path), FieldFlag_AssetPath));
        s.fields.push_back(field("guid", "GUID", FieldType::String, JE_ACC(ShaderTexture, guid), FieldFlag_Guid | FieldFlag_Hidden));
        return s;
    }
    const StructDesc& shaderTextureDesc(){ static const StructDesc s = makeShaderTexture(); return s; }

    StructDesc makeSetEntry(){
        StructDesc s = structOf<SetOverAgeEntry>("SetOverAgeEntry");
        s.fields.push_back(enumField("property", "Property", JE_ACC(SetOverAgeEntry, property), kSetProperties));
        s.fields.push_back(ranged(field("fromTime", "From Age", FieldType::Float, JE_ACC(SetOverAgeEntry, fromTime)), 0.0f, 1.0e6f));
        s.fields.push_back(ranged(field("toTime", "To Age", FieldType::Float, JE_ACC(SetOverAgeEntry, toTime)), 0.0f, 1.0e6f));
        s.fields.push_back(field("curve", "Curve", FieldType::Curve, JE_ACC(SetOverAgeEntry, curve), FieldFlag_None, "the ease over the window, 0 to 1"));
        s.fields.push_back(field("from", "From", FieldType::Vector4, JE_ACC(SetOverAgeEntry, from)));
        s.fields.push_back(field("to", "To", FieldType::Vector4, JE_ACC(SetOverAgeEntry, to)));
        s.fields.push_back(shownIf(field("shortestPath", "Shortest Path", FieldType::Bool, JE_ACC(SetOverAgeEntry, shortestPath)), JE_IF(SetOverAgeEntry, m.property == SetProperty::Rotation)));
        s.fields.push_back(shownIf(field("srgb", "sRGB", FieldType::Bool, JE_ACC(SetOverAgeEntry, srgb)), JE_IF(SetOverAgeEntry, m.property == SetProperty::Color)));
        s.fields.push_back(shownIf(intArrayField("frames", "Frames", JE_ACC(SetOverAgeEntry, frames), (uint32_t)kMaxSpriteFrames), JE_IF(SetOverAgeEntry, m.property == SetProperty::Frame)));
        return s;
    }
    const StructDesc& setEntryDesc(){ static const StructDesc s = makeSetEntry(); return s; }

    // ---------------------------------------------------------------- modules

    template<class T>
    ModuleDesc module(ModuleId id, const char* key, const char* label, void* (*get)(ParticleEffectDocument&), const char* tooltip){
        ModuleDesc d;
        d.id = id;
        d.key = key;
        d.label = label;
        d.get = get;
        d.present = [](void* m) -> bool* { return &static_cast<T*>(m)->present; };
        d.enabled = [](void* m) -> bool* { return &static_cast<T*>(m)->enabled; };
        d.fields.name = label;
        d.fields.defaults = &defaultOf<T>();
        d.tooltip = tooltip;
        return d;
    }

    #define JE_DOC(member) [](ParticleEffectDocument& d) -> void* { return &d.member; }

    ModuleDesc makeMain(){
        ModuleDesc d;
        d.id = ModuleId::Main;
        d.key = "main";
        d.label = "Main";
        d.alwaysPresent = true;
        d.hasEnable = false;
        d.get = JE_DOC(main);
        d.fields.name = "Main";
        d.fields.defaults = &defaultOf<MainModule>();
        d.tooltip = "Start values of every particle";
        auto& f = d.fields.fields;
        f.push_back(ranged(field("startLifetime", "Start Lifetime", FieldType::MinMaxCurve, JE_ACC(MainModule, startLifetime)), 0.001f, 1.0e5f));
        f.push_back(field("startSpeed", "Start Speed", FieldType::MinMaxCurve, JE_ACC(MainModule, startSpeed)));
        f.push_back(field("startSize3D", "3D Start Size", FieldType::Bool, JE_ACC(MainModule, startSize3D)));
        f.push_back(ranged(field("startSize", "Start Size", FieldType::MinMaxCurve, JE_ACC(MainModule, startSize)), 0.0f, 1.0e6f));
        f.push_back(shownIf(ranged(field("startSizeY", "Start Size Y", FieldType::MinMaxCurve, JE_ACC(MainModule, startSizeY)), 0.0f, 1.0e6f), JE_IF(MainModule, m.startSize3D)));
        f.push_back(shownIf(ranged(field("startSizeZ", "Start Size Z", FieldType::MinMaxCurve, JE_ACC(MainModule, startSizeZ)), 0.0f, 1.0e6f), JE_IF(MainModule, m.startSize3D)));
        f.push_back(shownIf(field("startSizeLinked", "Same Random for XYZ", FieldType::Bool, JE_ACC(MainModule, startSizeLinked), FieldFlag_Advanced), JE_IF(MainModule, m.startSize3D)));
        f.push_back(field("startRotation3D", "3D Start Rotation", FieldType::Bool, JE_ACC(MainModule, startRotation3D)));
        f.push_back(field("startRotation", "Start Rotation", FieldType::MinMaxCurve, JE_ACC(MainModule, startRotation), FieldFlag_Degrees));
        f.push_back(shownIf(field("startRotationX", "Start Rotation X", FieldType::MinMaxCurve, JE_ACC(MainModule, startRotationX), FieldFlag_Degrees), JE_IF(MainModule, m.startRotation3D)));
        f.push_back(shownIf(field("startRotationY", "Start Rotation Y", FieldType::MinMaxCurve, JE_ACC(MainModule, startRotationY), FieldFlag_Degrees), JE_IF(MainModule, m.startRotation3D)));
        f.push_back(ranged(field("flipRotation", "Flip Rotation", FieldType::Float, JE_ACC(MainModule, flipRotation)), 0.0f, 1.0f));
        f.push_back(field("startColor", "Start Color", FieldType::MinMaxGradient, JE_ACC(MainModule, startColor)));
        f.push_back(field("gravityModifier", "Gravity Modifier", FieldType::MinMaxCurve, JE_ACC(MainModule, gravityModifier)));
        f.push_back(enumField("gravitySource", "Gravity Source", JE_ACC(MainModule, gravitySource), kGravitySources, FieldFlag_None,
                              "the gravity Gravity Modifier scales: the 3D world's, or the 2D world's (in the scene's units)"));
        f.push_back(field("inheritVelocity", "Inherit Velocity", FieldType::MinMaxCurve, JE_ACC(MainModule, inheritVelocity), FieldFlag_None, "share of the emitter's velocity a particle takes"));
        f.push_back(shownIf(enumField("inheritVelocityMode", "Inherit Mode", JE_ACC(MainModule, inheritVelocityMode), kInheritModes, FieldFlag_None,
                                      "Initial: at birth; Current: the emitter's change of velocity every tick (particles keep moving with it)"),
                            JE_IF(MainModule, !m.inheritVelocity.isZero())));
        f.push_back(field("startSizeFromTexture", "Start Size from Texture", FieldType::Bool, JE_ACC(MainModule, startSizeFromTexture), FieldFlag_Hidden));
        return d;
    }

    ModuleDesc makeEmission(){
        ModuleDesc d = module<EmissionModule>(ModuleId::Emission, "emission", "Emission", JE_DOC(emission), "How many particles are born, and when");
        auto& f = d.fields.fields;
        f.push_back(ranged(field("rateOverTime", "Rate over Time", FieldType::MinMaxCurve, JE_ACC(EmissionModule, rateOverTime)), 0.0f, 1.0e7f));
        f.push_back(ranged(field("rateOverDistance", "Rate over Distance", FieldType::MinMaxCurve, JE_ACC(EmissionModule, rateOverDistance)), 0.0f, 1.0e7f));
        f.push_back(arrayField<std::vector<Burst>>("bursts", "Bursts", JE_ACC(EmissionModule, bursts), &burstDesc(), (uint32_t)kMaxBursts));
        f.push_back(ranged(field("maxPerTick", "Max per Tick", FieldType::Int, JE_ACC(EmissionModule, maxPerTick), FieldFlag_Advanced, "0: no limit"), 0, 1.0e7f));
        return d;
    }

    ModuleDesc makeShape(){
        ModuleDesc d = module<ShapeModule>(ModuleId::Shape, "shape", "Shape", JE_DOC(shape), "Where particles are born and which way they start");
        auto& f = d.fields.fields;
        f.push_back(enumField("shape", "Shape", JE_ACC(ShapeModule, shape), kShapeTypes));
        f.push_back(shownIf(enumField("emitFrom", "Emit from", JE_ACC(ShapeModule, emitFrom), kEmitFrom),
                            JE_IF(ShapeModule, m.shape == ShapeType::Cone || m.shape == ShapeType::Box || m.shape == ShapeType::Mesh)));
        f.push_back(shownIf(ranged(field("radius", "Radius", FieldType::Float, JE_ACC(ShapeModule, radius)), 0.0f, 1.0e6f),
                            JE_IF(ShapeModule, m.shape != ShapeType::Box && m.shape != ShapeType::Rectangle && m.shape != ShapeType::Mesh)));
        f.push_back(shownIf(ranged(field("radiusThickness", "Radius Thickness", FieldType::Float, JE_ACC(ShapeModule, radiusThickness)), 0.0f, 1.0f),
                            JE_IF(ShapeModule, m.shape != ShapeType::Box && m.shape != ShapeType::Rectangle && m.shape != ShapeType::Mesh && m.shape != ShapeType::Edge)));
        auto radial = JE_IF(ShapeModule, m.shape == ShapeType::Sphere || m.shape == ShapeType::Hemisphere || m.shape == ShapeType::Cone ||
                                         m.shape == ShapeType::Circle || m.shape == ShapeType::Donut || m.shape == ShapeType::Edge);
        f.push_back(shownIf(enumField("radiusMode", "Radius Mode", JE_ACC(ShapeModule, radiusMode), kShapeMultiModes, FieldFlag_None,
                                      "where along the radius (the Edge's length) particles are born: at random, moving at Speed, or a burst spread evenly"),
                            radial));
        f.push_back(shownIf(ranged(field("radiusSpread", "Radius Spread", FieldType::Float, JE_ACC(ShapeModule, radiusSpread), FieldFlag_None,
                                         "positions only at multiples of it (0: anywhere)"), 0.0f, 1.0f), radial));
        f.push_back(shownIf(field("radiusSpeed", "Radius Speed", FieldType::MinMaxCurve, JE_ACC(ShapeModule, radiusSpeed), FieldFlag_None,
                                  "radius lengths per second (Loop, Ping-Pong), over the emitter time"),
                            JE_IF(ShapeModule, (m.radiusMode == ShapeMultiMode::Loop || m.radiusMode == ShapeMultiMode::PingPong) &&
                                               (m.shape == ShapeType::Sphere || m.shape == ShapeType::Hemisphere || m.shape == ShapeType::Cone ||
                                                m.shape == ShapeType::Circle || m.shape == ShapeType::Donut || m.shape == ShapeType::Edge))));
        f.push_back(shownIf(ranged(field("angle", "Angle", FieldType::Float, JE_ACC(ShapeModule, angle), FieldFlag_Degrees), 0.0f, 90.0f), JE_IF(ShapeModule, m.shape == ShapeType::Cone)));
        f.push_back(shownIf(ranged(field("length", "Length", FieldType::Float, JE_ACC(ShapeModule, length)), 0.0f, 1.0e6f), JE_IF(ShapeModule, m.shape == ShapeType::Cone)));
        auto arced = JE_IF(ShapeModule, m.shape == ShapeType::Cone || m.shape == ShapeType::Circle || m.shape == ShapeType::Donut || m.shape == ShapeType::Sphere || m.shape == ShapeType::Hemisphere);
        f.push_back(shownIf(ranged(field("arc", "Arc", FieldType::Float, JE_ACC(ShapeModule, arc), FieldFlag_Degrees), 0.0f, 360.0f), arced));
        f.push_back(shownIf(enumField("arcMode", "Arc Mode", JE_ACC(ShapeModule, arcMode), kShapeMultiModes, FieldFlag_None,
                                      "where on the arc particles are born: at random, moving around it at Speed, or a burst spread evenly"),
                            arced));
        f.push_back(shownIf(ranged(field("arcSpread", "Arc Spread", FieldType::Float, JE_ACC(ShapeModule, arcSpread), FieldFlag_None,
                                         "angles only at multiples of this share of the arc (0: any)"), 0.0f, 1.0f), arced));
        f.push_back(shownIf(field("arcSpeed", "Arc Speed", FieldType::MinMaxCurve, JE_ACC(ShapeModule, arcSpeed), FieldFlag_None,
                                  "turns of the arc per second (Loop, Ping-Pong), over the emitter time"),
                            JE_IF(ShapeModule, (m.arcMode == ShapeMultiMode::Loop || m.arcMode == ShapeMultiMode::PingPong) &&
                                               (m.shape == ShapeType::Cone || m.shape == ShapeType::Circle || m.shape == ShapeType::Donut ||
                                                m.shape == ShapeType::Sphere || m.shape == ShapeType::Hemisphere))));
        f.push_back(shownIf(ranged(field("donutRadius", "Donut Radius", FieldType::Float, JE_ACC(ShapeModule, donutRadius)), 0.0f, 1.0e6f), JE_IF(ShapeModule, m.shape == ShapeType::Donut)));
        f.push_back(shownIf(field("boxThickness", "Box Thickness", FieldType::Vector3, JE_ACC(ShapeModule, boxThickness)), JE_IF(ShapeModule, m.shape == ShapeType::Box)));
        f.push_back(shownIf(structField("mesh", "Mesh", JE_ACC(ShapeModule, mesh), &meshSourceDesc()), JE_IF(ShapeModule, m.shape == ShapeType::Mesh)));
        f.push_back(field("position", "Position", FieldType::Vector3, JE_ACC(ShapeModule, position)));
        f.push_back(field("rotation", "Rotation", FieldType::Vector3, JE_ACC(ShapeModule, rotation), FieldFlag_Degrees));
        f.push_back(field("scale", "Scale", FieldType::Vector3, JE_ACC(ShapeModule, scale)));
        f.push_back(ranged(field("randomizeDirection", "Randomize Direction", FieldType::Float, JE_ACC(ShapeModule, randomizeDirection)), 0.0f, 1.0f));
        f.push_back(ranged(field("spherizeDirection", "Spherize Direction", FieldType::Float, JE_ACC(ShapeModule, spherizeDirection)), 0.0f, 1.0f));
        f.push_back(ranged(field("randomizePosition", "Randomize Position", FieldType::Float, JE_ACC(ShapeModule, randomizePosition)), 0.0f, 1.0e6f));
        f.push_back(field("alignToDirection", "Align to Direction", FieldType::Bool, JE_ACC(ShapeModule, alignToDirection), FieldFlag_None,
                          "particles start turned with their up axis along the direction they leave the shape in"));
        return d;
    }

    ModuleDesc makeStartVelocity(){
        ModuleDesc d = module<StartVelocityModule>(ModuleId::StartVelocity, "startVelocity", "Start Velocity", JE_DOC(startVelocity),
                                                   "A random start velocity between two vectors, in simulation space (added to Start Speed)");
        auto& f = d.fields.fields;
        f.push_back(field("min", "Min", FieldType::Vector3, JE_ACC(StartVelocityModule, min)));
        f.push_back(field("max", "Max", FieldType::Vector3, JE_ACC(StartVelocityModule, max)));
        f.push_back(field("uniform", "Uniform", FieldType::Bool, JE_ACC(StartVelocityModule, uniform), FieldFlag_None, "one random pick for the three axes"));
        return d;
    }

    ModuleDesc makeVelocityOverLifetime(){
        ModuleDesc d = module<VelocityOverLifetimeModule>(ModuleId::VelocityOverLifetime, "velocityOverLifetime", "Velocity over Lifetime",
                                                          JE_DOC(velocityOverLifetime), "Velocities added over the particle's life");
        auto& f = d.fields.fields;
        f.push_back(field("x", "Linear X", FieldType::MinMaxCurve, JE_ACC(VelocityOverLifetimeModule, x)));
        f.push_back(field("y", "Linear Y", FieldType::MinMaxCurve, JE_ACC(VelocityOverLifetimeModule, y)));
        f.push_back(field("z", "Linear Z", FieldType::MinMaxCurve, JE_ACC(VelocityOverLifetimeModule, z)));
        f.push_back(enumField("space", "Space", JE_ACC(VelocityOverLifetimeModule, space), kValueSpaces));
        f.push_back(field("orbitalX", "Orbital X", FieldType::MinMaxCurve, JE_ACC(VelocityOverLifetimeModule, orbitalX), FieldFlag_Degrees));
        f.push_back(field("orbitalY", "Orbital Y", FieldType::MinMaxCurve, JE_ACC(VelocityOverLifetimeModule, orbitalY), FieldFlag_Degrees));
        f.push_back(field("orbitalZ", "Orbital Z", FieldType::MinMaxCurve, JE_ACC(VelocityOverLifetimeModule, orbitalZ), FieldFlag_Degrees));
        f.push_back(field("offsetX", "Orbital Offset X", FieldType::MinMaxCurve, JE_ACC(VelocityOverLifetimeModule, offsetX), FieldFlag_None,
                          "the orbit's centre from the emitter, over the particle's life"));
        f.push_back(field("offsetY", "Orbital Offset Y", FieldType::MinMaxCurve, JE_ACC(VelocityOverLifetimeModule, offsetY)));
        f.push_back(field("offsetZ", "Orbital Offset Z", FieldType::MinMaxCurve, JE_ACC(VelocityOverLifetimeModule, offsetZ)));
        f.push_back(field("radial", "Radial", FieldType::MinMaxCurve, JE_ACC(VelocityOverLifetimeModule, radial)));
        f.push_back(field("speedModifier", "Speed Modifier", FieldType::MinMaxCurve, JE_ACC(VelocityOverLifetimeModule, speedModifier)));
        return d;
    }

    ModuleDesc makeLimitVelocity(){
        ModuleDesc d = module<LimitVelocityModule>(ModuleId::LimitVelocity, "limitVelocity", "Limit Velocity over Lifetime", JE_DOC(limitVelocity),
                                                   "Speed limit and drag, after every force");
        auto& f = d.fields.fields;
        f.push_back(field("separateAxes", "Separate Axes", FieldType::Bool, JE_ACC(LimitVelocityModule, separateAxes), FieldFlag_None,
                          "a limit per axis (in Space) instead of one on the speed"));
        f.push_back(ranged(field("limit", "Speed", FieldType::MinMaxCurve, JE_ACC(LimitVelocityModule, limit), FieldFlag_None, "the speed limit (X with Separate Axes)"), 0.0f, 1.0e6f));
        f.push_back(shownIf(ranged(field("limitY", "Speed Y", FieldType::MinMaxCurve, JE_ACC(LimitVelocityModule, limitY)), 0.0f, 1.0e6f), JE_IF(LimitVelocityModule, m.separateAxes)));
        f.push_back(shownIf(ranged(field("limitZ", "Speed Z", FieldType::MinMaxCurve, JE_ACC(LimitVelocityModule, limitZ)), 0.0f, 1.0e6f), JE_IF(LimitVelocityModule, m.separateAxes)));
        f.push_back(shownIf(enumField("space", "Space", JE_ACC(LimitVelocityModule, space), kValueSpaces), JE_IF(LimitVelocityModule, m.separateAxes)));
        f.push_back(ranged(field("dampen", "Dampen", FieldType::Float, JE_ACC(LimitVelocityModule, dampen), FieldFlag_None, "share of the speed above the limit removed per 1/30 s"), 0.0f, 1.0f));
        f.push_back(ranged(field("drag", "Drag", FieldType::MinMaxCurve, JE_ACC(LimitVelocityModule, drag)), 0.0f, 1.0e6f));
        f.push_back(ranged(field("quadraticDrag", "Quadratic Drag", FieldType::Float, JE_ACC(LimitVelocityModule, quadraticDrag)), 0.0f, 1.0e6f));
        f.push_back(field("multiplyBySize", "Multiply by Size", FieldType::Bool, JE_ACC(LimitVelocityModule, multiplyBySize)));
        f.push_back(field("multiplyByVelocity", "Multiply by Velocity", FieldType::Bool, JE_ACC(LimitVelocityModule, multiplyByVelocity)));
        return d;
    }

    ModuleDesc makeForceOverLifetime(){
        ModuleDesc d = module<ForceOverLifetimeModule>(ModuleId::ForceOverLifetime, "forceOverLifetime", "Force over Lifetime", JE_DOC(forceOverLifetime),
                                                       "An acceleration over the particle's life");
        auto& f = d.fields.fields;
        f.push_back(field("x", "X", FieldType::MinMaxCurve, JE_ACC(ForceOverLifetimeModule, x)));
        f.push_back(field("y", "Y", FieldType::MinMaxCurve, JE_ACC(ForceOverLifetimeModule, y)));
        f.push_back(field("z", "Z", FieldType::MinMaxCurve, JE_ACC(ForceOverLifetimeModule, z)));
        f.push_back(enumField("space", "Space", JE_ACC(ForceOverLifetimeModule, space), kValueSpaces));
        f.push_back(field("randomizePerTick", "Randomize", FieldType::Bool, JE_ACC(ForceOverLifetimeModule, randomizePerTick), FieldFlag_None,
                          "random modes pick a new value every tick instead of once per particle"));
        return d;
    }

    ModuleDesc makeNoise(){
        ModuleDesc d = module<NoiseModule>(ModuleId::Noise, "noise", "Noise", JE_DOC(noise), "Curl noise turbulence");
        auto& f = d.fields.fields;
        f.push_back(field("separateAxes", "Separate Axes", FieldType::Bool, JE_ACC(NoiseModule, separateAxes)));
        f.push_back(field("strength", "Strength", FieldType::MinMaxCurve, JE_ACC(NoiseModule, strength)));
        f.push_back(shownIf(field("strengthY", "Strength Y", FieldType::MinMaxCurve, JE_ACC(NoiseModule, strengthY)), JE_IF(NoiseModule, m.separateAxes)));
        f.push_back(shownIf(field("strengthZ", "Strength Z", FieldType::MinMaxCurve, JE_ACC(NoiseModule, strengthZ)), JE_IF(NoiseModule, m.separateAxes)));
        f.push_back(ranged(field("frequency", "Frequency", FieldType::Float, JE_ACC(NoiseModule, frequency)), 0.0001f, 1000.0f));
        f.push_back(field("scrollSpeed", "Scroll Speed", FieldType::MinMaxCurve, JE_ACC(NoiseModule, scrollSpeed), FieldFlag_None,
                          "how fast the field moves through the particles, over the emitter time"));
        f.push_back(field("damping", "Damping", FieldType::Bool, JE_ACC(NoiseModule, damping), FieldFlag_None, "the strength scales with 1 / frequency"));
        f.push_back(ranged(field("octaves", "Octaves", FieldType::Int, JE_ACC(NoiseModule, octaves)), 1, 4));
        f.push_back(shownIf(ranged(field("octaveMultiplier", "Octave Multiplier", FieldType::Float, JE_ACC(NoiseModule, octaveMultiplier)), 0.0f, 1.0f), JE_IF(NoiseModule, m.octaves > 1)));
        f.push_back(shownIf(ranged(field("octaveScale", "Octave Scale", FieldType::Float, JE_ACC(NoiseModule, octaveScale)), 1.0f, 4.0f), JE_IF(NoiseModule, m.octaves > 1)));
        f.push_back(enumField("quality", "Quality", JE_ACC(NoiseModule, quality), kNoiseQualities, FieldFlag_None,
                              "High: 3D curl noise; Medium: two potentials; Low: a cached field"));
        f.push_back(field("positionAmount", "Position Amount", FieldType::MinMaxCurve, JE_ACC(NoiseModule, positionAmount)));
        f.push_back(field("rotationAmount", "Rotation Amount", FieldType::MinMaxCurve, JE_ACC(NoiseModule, rotationAmount)));
        f.push_back(field("sizeAmount", "Size Amount", FieldType::MinMaxCurve, JE_ACC(NoiseModule, sizeAmount)));
        return d;
    }

    ModuleDesc makeOrbit(){
        ModuleDesc d = module<OrbitModule>(ModuleId::Orbit, "orbit", "Orbit", JE_DOC(orbit), "Turns particles around an axis");
        auto& f = d.fields.fields;
        f.push_back(field("axis", "Axis", FieldType::Vector3, JE_ACC(OrbitModule, axis)));
        f.push_back(field("center", "Center", FieldType::Vector3, JE_ACC(OrbitModule, center)));
        f.push_back(field("angularSpeed", "Angular Speed", FieldType::MinMaxCurve, JE_ACC(OrbitModule, angularSpeed), FieldFlag_Degrees));
        f.push_back(field("radialPull", "Radial Pull", FieldType::MinMaxCurve, JE_ACC(OrbitModule, radialPull)));
        f.push_back(enumField("space", "Space", JE_ACC(OrbitModule, space), kValueSpaces));
        return d;
    }

    ModuleDesc makeExternalForces(){
        ModuleDesc d = module<ExternalForcesModule>(ModuleId::ExternalForces, "externalForces", "External Forces", JE_DOC(externalForces),
                                                    "Force Field components in the scene push the particles");
        auto& f = d.fields.fields;
        f.push_back(field("multiplier", "Multiplier", FieldType::Float, JE_ACC(ExternalForcesModule, multiplier)));
        f.push_back(field("layerMask", "Influence Mask", FieldType::Mask, JE_ACC(ExternalForcesModule, layerMask)));
        return d;
    }

    ModuleDesc makeColorOverLifetime(){
        ModuleDesc d = module<ColorOverLifetimeModule>(ModuleId::ColorOverLifetime, "colorOverLifetime", "Color over Lifetime", JE_DOC(colorOverLifetime),
                                                       "Multiplies the start colour over the particle's life");
        auto& f = d.fields.fields;
        f.push_back(field("color", "Color", FieldType::MinMaxGradient, JE_ACC(ColorOverLifetimeModule, color)));
        f.push_back(field("setMode", "Replace Start Color", FieldType::Bool, JE_ACC(ColorOverLifetimeModule, setMode), FieldFlag_Advanced,
                          "the gradient's colour replaces the start colour instead of multiplying it (alpha still multiplies)"));
        return d;
    }

    ModuleDesc makeColorBySpeed(){
        ModuleDesc d = module<ColorBySpeedModule>(ModuleId::ColorBySpeed, "colorBySpeed", "Color by Speed", JE_DOC(colorBySpeed), "Colour from the particle's speed");
        auto& f = d.fields.fields;
        f.push_back(field("color", "Color", FieldType::MinMaxGradient, JE_ACC(ColorBySpeedModule, color)));
        f.push_back(field("speedRange", "Speed Range", FieldType::Vector2, JE_ACC(ColorBySpeedModule, speedRange)));
        return d;
    }

    ModuleDesc makeSizeOverLifetime(){
        ModuleDesc d = module<SizeOverLifetimeModule>(ModuleId::SizeOverLifetime, "sizeOverLifetime", "Size over Lifetime", JE_DOC(sizeOverLifetime),
                                                      "Multiplies the start size over the particle's life");
        auto& f = d.fields.fields;
        f.push_back(field("separateAxes", "Separate Axes", FieldType::Bool, JE_ACC(SizeOverLifetimeModule, separateAxes)));
        f.push_back(field("size", "Size", FieldType::MinMaxCurve, JE_ACC(SizeOverLifetimeModule, size)));
        f.push_back(shownIf(field("sizeY", "Size Y", FieldType::MinMaxCurve, JE_ACC(SizeOverLifetimeModule, sizeY)), JE_IF(SizeOverLifetimeModule, m.separateAxes)));
        f.push_back(shownIf(field("sizeZ", "Size Z", FieldType::MinMaxCurve, JE_ACC(SizeOverLifetimeModule, sizeZ)), JE_IF(SizeOverLifetimeModule, m.separateAxes)));
        return d;
    }

    ModuleDesc makeSizeBySpeed(){
        ModuleDesc d = module<SizeBySpeedModule>(ModuleId::SizeBySpeed, "sizeBySpeed", "Size by Speed", JE_DOC(sizeBySpeed), "Size from the particle's speed");
        auto& f = d.fields.fields;
        f.push_back(field("separateAxes", "Separate Axes", FieldType::Bool, JE_ACC(SizeBySpeedModule, separateAxes)));
        f.push_back(field("size", "Size", FieldType::MinMaxCurve, JE_ACC(SizeBySpeedModule, size)));
        f.push_back(shownIf(field("sizeY", "Size Y", FieldType::MinMaxCurve, JE_ACC(SizeBySpeedModule, sizeY)), JE_IF(SizeBySpeedModule, m.separateAxes)));
        f.push_back(shownIf(field("sizeZ", "Size Z", FieldType::MinMaxCurve, JE_ACC(SizeBySpeedModule, sizeZ)), JE_IF(SizeBySpeedModule, m.separateAxes)));
        f.push_back(field("speedRange", "Speed Range", FieldType::Vector2, JE_ACC(SizeBySpeedModule, speedRange)));
        return d;
    }

    ModuleDesc makeRotationOverLifetime(){
        ModuleDesc d = module<RotationOverLifetimeModule>(ModuleId::RotationOverLifetime, "rotationOverLifetime", "Rotation over Lifetime",
                                                          JE_DOC(rotationOverLifetime), "Angular velocity over the particle's life");
        auto& f = d.fields.fields;
        f.push_back(field("separateAxes", "Separate Axes", FieldType::Bool, JE_ACC(RotationOverLifetimeModule, separateAxes)));
        f.push_back(shownIf(field("angularVelocityX", "Angular Velocity X", FieldType::MinMaxCurve, JE_ACC(RotationOverLifetimeModule, angularVelocityX), FieldFlag_Degrees), JE_IF(RotationOverLifetimeModule, m.separateAxes)));
        f.push_back(shownIf(field("angularVelocityY", "Angular Velocity Y", FieldType::MinMaxCurve, JE_ACC(RotationOverLifetimeModule, angularVelocityY), FieldFlag_Degrees), JE_IF(RotationOverLifetimeModule, m.separateAxes)));
        f.push_back(field("angularVelocity", "Angular Velocity", FieldType::MinMaxCurve, JE_ACC(RotationOverLifetimeModule, angularVelocity), FieldFlag_Degrees));
        return d;
    }

    ModuleDesc makeRotationBySpeed(){
        ModuleDesc d = module<RotationBySpeedModule>(ModuleId::RotationBySpeed, "rotationBySpeed", "Rotation by Speed", JE_DOC(rotationBySpeed), "Angular velocity from the particle's speed");
        auto& f = d.fields.fields;
        f.push_back(field("separateAxes", "Separate Axes", FieldType::Bool, JE_ACC(RotationBySpeedModule, separateAxes), FieldFlag_None,
                          "a speed per axis (X and Y turn Mesh particles)"));
        f.push_back(shownIf(field("angularVelocityX", "Angular Velocity X", FieldType::MinMaxCurve, JE_ACC(RotationBySpeedModule, angularVelocityX), FieldFlag_Degrees), JE_IF(RotationBySpeedModule, m.separateAxes)));
        f.push_back(shownIf(field("angularVelocityY", "Angular Velocity Y", FieldType::MinMaxCurve, JE_ACC(RotationBySpeedModule, angularVelocityY), FieldFlag_Degrees), JE_IF(RotationBySpeedModule, m.separateAxes)));
        f.push_back(field("angularVelocity", "Angular Velocity", FieldType::MinMaxCurve, JE_ACC(RotationBySpeedModule, angularVelocity), FieldFlag_Degrees));
        f.push_back(field("speedRange", "Speed Range", FieldType::Vector2, JE_ACC(RotationBySpeedModule, speedRange)));
        return d;
    }

    ModuleDesc makeTextureSheet(){
        ModuleDesc d = module<TextureSheetAnimationModule>(ModuleId::TextureSheetAnimation, "textureSheetAnimation", "Texture Sheet Animation",
                                                           JE_DOC(textureSheetAnimation), "Flipbook frames of a sprite sheet");
        auto& f = d.fields.fields;
        f.push_back(enumField("mode", "Mode", JE_ACC(TextureSheetAnimationModule, mode), kSheetModes));
        f.push_back(shownIf(ranged(field("tilesX", "Tiles X", FieldType::Int, JE_ACC(TextureSheetAnimationModule, tilesX)), 1, 64), JE_IF(TextureSheetAnimationModule, m.mode == TextureSheetMode::Grid)));
        f.push_back(shownIf(ranged(field("tilesY", "Tiles Y", FieldType::Int, JE_ACC(TextureSheetAnimationModule, tilesY)), 1, 64), JE_IF(TextureSheetAnimationModule, m.mode == TextureSheetMode::Grid)));
        f.push_back(shownIf(enumField("animation", "Animation", JE_ACC(TextureSheetAnimationModule, animation), kSheetAnimations), JE_IF(TextureSheetAnimationModule, m.mode == TextureSheetMode::Grid)));
        f.push_back(shownIf(enumField("rowMode", "Row Mode", JE_ACC(TextureSheetAnimationModule, rowMode), kSheetRowModes),
                            JE_IF(TextureSheetAnimationModule, m.mode == TextureSheetMode::Grid && m.animation == TextureSheetAnimation::SingleRow)));
        f.push_back(shownIf(ranged(field("rowIndex", "Row", FieldType::Int, JE_ACC(TextureSheetAnimationModule, rowIndex)), 0, 63),
                            JE_IF(TextureSheetAnimationModule, m.mode == TextureSheetMode::Grid && m.animation == TextureSheetAnimation::SingleRow && m.rowMode == TextureSheetRowMode::Custom)));
        f.push_back(shownIf(arrayField<std::vector<SpriteRect>>("sprites", "Sprites", JE_ACC(TextureSheetAnimationModule, sprites), &spriteRectDesc(), (uint32_t)kMaxSpriteFrames),
                            JE_IF(TextureSheetAnimationModule, m.mode == TextureSheetMode::Sprites)));
        f.push_back(enumField("timeMode", "Time Mode", JE_ACC(TextureSheetAnimationModule, timeMode), kSheetTimeModes));
        f.push_back(shownIf(field("frameOverTime", "Frame over Time", FieldType::MinMaxCurve, JE_ACC(TextureSheetAnimationModule, frameOverTime)),
                            JE_IF(TextureSheetAnimationModule, m.timeMode != TextureSheetTimeMode::FPS)));
        f.push_back(shownIf(ranged(field("fps", "FPS", FieldType::Float, JE_ACC(TextureSheetAnimationModule, fps)), 0.0f, 1000.0f),
                            JE_IF(TextureSheetAnimationModule, m.timeMode == TextureSheetTimeMode::FPS)));
        f.push_back(shownIf(field("speedRange", "Speed Range", FieldType::Vector2, JE_ACC(TextureSheetAnimationModule, speedRange)),
                            JE_IF(TextureSheetAnimationModule, m.timeMode == TextureSheetTimeMode::Speed)));
        f.push_back(field("startFrame", "Start Frame", FieldType::MinMaxCurve, JE_ACC(TextureSheetAnimationModule, startFrame)));
        f.push_back(ranged(field("cycles", "Cycles", FieldType::Float, JE_ACC(TextureSheetAnimationModule, cycles)), 0.0f, 1000.0f));
        f.push_back(ranged(field("flipU", "Flip U", FieldType::Float, JE_ACC(TextureSheetAnimationModule, flipU), FieldFlag_None,
                                 "the share of particles whose frames are mirrored horizontally"), 0.0f, 1.0f));
        f.push_back(ranged(field("flipV", "Flip V", FieldType::Float, JE_ACC(TextureSheetAnimationModule, flipV), FieldFlag_None,
                                 "the share of particles whose frames are mirrored vertically"), 0.0f, 1.0f));
        f.push_back(field("frameBlending", "Frame Blending", FieldType::Bool, JE_ACC(TextureSheetAnimationModule, frameBlending), FieldFlag_None,
                          "cross-fades each frame into the next (FRAME_BLEND shader feature)"));
        f.push_back(intArrayField("startFrameList", "Start Frames", JE_ACC(TextureSheetAnimationModule, startFrameList), (uint32_t)kMaxSpriteFrames, FieldFlag_Hidden));
        return d;
    }

    ModuleDesc makeCollision(){
        ModuleDesc d = module<CollisionModule>(ModuleId::Collision, "collision", "Collision", JE_DOC(collision), "Particles bounce off planes, the physics world or terrain");
        auto& f = d.fields.fields;
        // "collisionType": a module entry's "type" is the module
        f.push_back(enumField("collisionType", "Type", JE_ACC(CollisionModule, type), kCollisionTypes));
        f.push_back(shownIf(arrayField<std::vector<CollisionPlane>>("planes", "Planes", JE_ACC(CollisionModule, planes), &planeDesc(), (uint32_t)kMaxCollisionPlanes),
                            JE_IF(CollisionModule, m.type == CollisionType::Planes)));
        f.push_back(ranged(field("dampen", "Dampen", FieldType::MinMaxCurve, JE_ACC(CollisionModule, dampen)), 0.0f, 1.0f));
        f.push_back(ranged(field("bounce", "Bounce", FieldType::MinMaxCurve, JE_ACC(CollisionModule, bounce)), 0.0f, 2.0f));
        f.push_back(ranged(field("lifetimeLoss", "Lifetime Loss", FieldType::MinMaxCurve, JE_ACC(CollisionModule, lifetimeLoss)), 0.0f, 1.0f));
        f.push_back(ranged(field("minKillSpeed", "Min Kill Speed", FieldType::Float, JE_ACC(CollisionModule, minKillSpeed)), 0.0f, 1.0e6f));
        f.push_back(ranged(field("maxKillSpeed", "Max Kill Speed", FieldType::Float, JE_ACC(CollisionModule, maxKillSpeed)), 0.0f, 1.0e6f));
        f.push_back(ranged(field("radiusScale", "Radius Scale", FieldType::Float, JE_ACC(CollisionModule, radiusScale)), 0.0f, 100.0f));
        f.push_back(field("killOnCollide", "Kill on Collide", FieldType::Bool, JE_ACC(CollisionModule, killOnCollide)));
        f.push_back(shownIf(field("collidesWith", "Collides With", FieldType::Mask, JE_ACC(CollisionModule, collidesWith)),
                            JE_IF(CollisionModule, m.type == CollisionType::World || m.type == CollisionType::World2D)));
        f.push_back(shownIf(enumField("quality", "Collision Quality", JE_ACC(CollisionModule, quality), kCollisionQualities, FieldFlag_None,
                                      "High queries every particle every tick; Medium at most Max Queries per Tick, the rest test the plane they last hit; Low only that plane"),
                            JE_IF(CollisionModule, m.type == CollisionType::World || m.type == CollisionType::World2D)));
        f.push_back(shownIf(ranged(field("maxQueriesPerTick", "Max Queries per Tick", FieldType::Int, JE_ACC(CollisionModule, maxQueriesPerTick)), 1, 65536),
                            JE_IF(CollisionModule, (m.type == CollisionType::World || m.type == CollisionType::World2D) && m.quality == CollisionQuality::Medium)));
        f.push_back(shownIf(ranged(field("planeCacheTicks", "Plane Cache Ticks", FieldType::Int, JE_ACC(CollisionModule, planeCacheTicks)), 1, 1024),
                            JE_IF(CollisionModule, (m.type == CollisionType::World || m.type == CollisionType::World2D) && m.quality == CollisionQuality::Medium)));
        f.push_back(field("sendEvents", "Send Collision Messages", FieldType::Bool, JE_ACC(CollisionModule, sendEvents)));
        f.push_back(shownIf(ranged(field("maxCollisionEvents", "Max Collision Events", FieldType::Int, JE_ACC(CollisionModule, maxCollisionEvents)), 1, 4096),
                            JE_IF(CollisionModule, m.sendEvents)));
        f.push_back(ranged(field("settleSpeed", "Settle Speed", FieldType::Float, JE_ACC(CollisionModule, settleSpeed), FieldFlag_None,
                                 "a hit slower than this (after the bounce) makes the particle rest until it dies; 0: off"), 0.0f, 1.0e6f));
        return d;
    }

    ModuleDesc makeTrails(){
        ModuleDesc d = module<TrailsModule>(ModuleId::Trails, "trails", "Trails", JE_DOC(trails), "Trails behind particles, or ribbons through them");
        auto& f = d.fields.fields;
        f.push_back(enumField("mode", "Mode", JE_ACC(TrailsModule, mode), kTrailModes));
        f.push_back(shownIf(ranged(field("ratio", "Ratio", FieldType::Float, JE_ACC(TrailsModule, ratio)), 0.0f, 1.0f), JE_IF(TrailsModule, m.mode == TrailMode::PerParticle)));
        f.push_back(shownIf(ranged(field("ribbonCount", "Ribbon Count", FieldType::Int, JE_ACC(TrailsModule, ribbonCount)), 1, 8), JE_IF(TrailsModule, m.mode == TrailMode::Ribbon)));
        f.push_back(shownIf(ranged(field("lifetime", "Lifetime", FieldType::MinMaxCurve, JE_ACC(TrailsModule, lifetime)), 0.0f, 1.0e4f), JE_IF(TrailsModule, m.mode == TrailMode::PerParticle)));
        f.push_back(shownIf(ranged(field("minVertexDistance", "Minimum Vertex Distance", FieldType::Float, JE_ACC(TrailsModule, minVertexDistance)), 0.0f, 1.0e6f), JE_IF(TrailsModule, m.mode == TrailMode::PerParticle)));
        f.push_back(shownIf(ranged(field("maxPoints", "Max Points", FieldType::Int, JE_ACC(TrailsModule, maxPoints)), 2, (float)kMaxTrailPoints), JE_IF(TrailsModule, m.mode == TrailMode::PerParticle)));
        f.push_back(field("worldSpace", "World Space", FieldType::Bool, JE_ACC(TrailsModule, worldSpace)));
        f.push_back(shownIf(field("dieWithParticles", "Die with Particles", FieldType::Bool, JE_ACC(TrailsModule, dieWithParticles)), JE_IF(TrailsModule, m.mode == TrailMode::PerParticle)));
        f.push_back(field("widthOverTrail", "Width over Trail", FieldType::MinMaxCurve, JE_ACC(TrailsModule, widthOverTrail)));
        f.push_back(field("colorOverLifetime", "Color over Lifetime", FieldType::MinMaxGradient, JE_ACC(TrailsModule, colorOverLifetime)));
        f.push_back(field("colorOverTrail", "Color over Trail", FieldType::MinMaxGradient, JE_ACC(TrailsModule, colorOverTrail)));
        f.push_back(field("inheritParticleColor", "Inherit Particle Color", FieldType::Bool, JE_ACC(TrailsModule, inheritParticleColor)));
        f.push_back(field("sizeAffectsWidth", "Size affects Width", FieldType::Bool, JE_ACC(TrailsModule, sizeAffectsWidth)));
        f.push_back(shownIf(field("sizeAffectsLifetime", "Size affects Lifetime", FieldType::Bool, JE_ACC(TrailsModule, sizeAffectsLifetime)), JE_IF(TrailsModule, m.mode == TrailMode::PerParticle)));
        f.push_back(enumField("textureMode", "Texture Mode", JE_ACC(TrailsModule, textureMode), kTrailTextureModes));
        // nothing reads it (lit trails are not built): old files keep loading, the Inspector does not offer it
        f.push_back(field("generateLightingData", "Generate Lighting Data", FieldType::Bool, JE_ACC(TrailsModule, generateLightingData), FieldFlag_Hidden));
        return d;
    }

    ModuleDesc makeSubEmittersModule(){
        return module<SubEmittersModule>(ModuleId::SubEmitters, "subEmitters", "Sub Emitters", JE_DOC(subEmittersModule),
                                         "Other effects born from this one's particles (the effect's Sub Emitters list)");
    }

    ModuleDesc makeCustomData(){
        ModuleDesc d = module<CustomDataModule>(ModuleId::CustomData, "customData", "Custom Data", JE_DOC(customData), "Four values over the particle's life, for custom shaders");
        auto& f = d.fields.fields;
        f.push_back(field("x", "X", FieldType::MinMaxCurve, JE_ACC(CustomDataModule, data[0])));
        f.push_back(field("y", "Y", FieldType::MinMaxCurve, JE_ACC(CustomDataModule, data[1])));
        f.push_back(field("z", "Z", FieldType::MinMaxCurve, JE_ACC(CustomDataModule, data[2])));
        f.push_back(field("w", "W", FieldType::MinMaxCurve, JE_ACC(CustomDataModule, data[3])));
        return d;
    }

    ModuleDesc makeRenderer(){
        ModuleDesc d = module<RendererModule>(ModuleId::Renderer, "renderer", "Renderer", JE_DOC(renderer), "How particles are drawn");
        d.alwaysPresent = true;
        auto& f = d.fields.fields;
        f.push_back(enumField("renderMode", "Render Mode", JE_ACC(RendererModule, renderMode), kRenderModes));
        auto stretched = JE_IF(RendererModule, m.renderMode == RenderMode::StretchedBillboard);
        f.push_back(shownIf(field("cameraScale", "Camera Scale", FieldType::Float, JE_ACC(RendererModule, cameraScale)), stretched));
        f.push_back(shownIf(field("speedScale", "Speed Scale", FieldType::Float, JE_ACC(RendererModule, speedScale)), stretched));
        f.push_back(shownIf(field("lengthScale", "Length Scale", FieldType::Float, JE_ACC(RendererModule, lengthScale)), stretched));
        f.push_back(shownIf(field("freezeInView", "Freeze in View", FieldType::Bool, JE_ACC(RendererModule, freezeInView)), stretched));
        f.push_back(shownIf(enumField("verticalLock", "Vertical Lock", JE_ACC(RendererModule, verticalLock), kVerticalLocks), JE_IF(RendererModule, m.renderMode == RenderMode::VerticalBillboard)));
        auto mesh = JE_IF(RendererModule, m.renderMode == RenderMode::Mesh);
        auto ring = JE_IF(RendererModule, m.renderMode == RenderMode::Ring);
        f.push_back(shownIf(arrayField<std::vector<MeshSource>>("meshes", "Meshes", JE_ACC(RendererModule, meshes), &meshSourceDesc(), (uint32_t)kMaxRendererMeshes), mesh));
        f.push_back(shownIf(enumField("meshSelection", "Mesh Distribution", JE_ACC(RendererModule, meshSelection), kMeshSelections), mesh));
        f.push_back(shownIf(ranged(field("ringOuterRadius", "Outer Radius", FieldType::Float, JE_ACC(RendererModule, ringOuterRadius), FieldFlag_None,
                                         "at a particle size of 1; the size scales it"), 0.0f, 1.0e4f), ring));
        f.push_back(shownIf(ranged(field("ringInnerRadius", "Inner Radius", FieldType::Float, JE_ACC(RendererModule, ringInnerRadius), FieldFlag_None,
                                         "0 makes a disc"), 0.0f, 1.0e4f), ring));
        f.push_back(shownIf(ranged(field("ringArc", "Arc", FieldType::Float, JE_ACC(RendererModule, ringArc), FieldFlag_Degrees,
                                         "degrees of the circle, from +X towards +Y"), 0.0f, 360.0f), ring));
        f.push_back(shownIf(ranged(field("ringSegments", "Segments", FieldType::Int, JE_ACC(RendererModule, ringSegments), FieldFlag_None,
                                         "around the whole circle"), 3, (float)kMaxRingSegments), ring));
        f.push_back(shownIf(enumField("meshAlignment", "Render Alignment", JE_ACC(RendererModule, meshAlignment), kMeshAlignments, FieldFlag_None,
                                      "how meshes turn: in the simulation space, to the camera plane or position, in world or emitter axes, or +Z along the velocity"),
                            JE_IF(RendererModule, m.renderMode == RenderMode::Mesh || m.renderMode == RenderMode::Ring)));
        f.push_back(field("pivot", "Pivot", FieldType::Vector3, JE_ACC(RendererModule, pivot)));
        f.push_back(field("flip", "Flip", FieldType::Vector3, JE_ACC(RendererModule, flip)));
        f.push_back(field("shader", "Shader", FieldType::String, JE_ACC(RendererModule, shader), FieldFlag_None, "library:particles, or a fork of it"));
        f.push_back(stringArrayField("shaderFeatures", "Shader Features", JE_ACC(RendererModule, shaderFeatures), 64));
        f.push_back(arrayField<std::vector<ShaderProperty>>("shaderProperties", "Shader Properties", JE_ACC(RendererModule, shaderProperties), &shaderPropertyDesc(), 64));
        f.push_back(arrayField<std::vector<ShaderTexture>>("shaderTextures", "Shader Textures", JE_ACC(RendererModule, shaderTextures), &shaderTextureDesc(), 16));
        auto notPixel = JE_IF(RendererModule, m.renderMode != RenderMode::Pixel);
        f.push_back(shownIf(field("texture", "Texture", FieldType::String, JE_ACC(RendererModule, texture), FieldFlag_AssetPath), notPixel));
        f.push_back(field("textureGuid", "Texture GUID", FieldType::String, JE_ACC(RendererModule, textureGuid), FieldFlag_Guid | FieldFlag_Hidden));
        f.push_back(shownIf(field("textureTiling", "Tiling and Offset", FieldType::Vector4, JE_ACC(RendererModule, textureTiling), FieldFlag_None,
                                  "the texture's UVs scaled by X and Y, then offset by Z and W (a material's tiling and offset)"),
                            JE_IF(RendererModule, !m.texture.empty() && m.renderMode != RenderMode::Pixel)));
        f.push_back(shownIf(enumField("untexturedShape", "Without Texture", JE_ACC(RendererModule, untexturedShape), kUntexturedShapes, FieldFlag_Advanced,
                                      "what a billboard draws without a texture: a soft disc (Unity's default particle) or a square (the old points)"),
                            JE_IF(RendererModule, m.texture.empty() && m.renderMode != RenderMode::Pixel)));
        f.push_back(field("trailTexture", "Trail Texture", FieldType::String, JE_ACC(RendererModule, trailTexture), FieldFlag_AssetPath));
        f.push_back(field("trailTextureGuid", "Trail Texture GUID", FieldType::String, JE_ACC(RendererModule, trailTextureGuid), FieldFlag_Guid | FieldFlag_Hidden));
        f.push_back(enumField("blend", "Blend", JE_ACC(RendererModule, blend), kBlends));
        f.push_back(field("depthWrite", "Depth Write", FieldType::Bool, JE_ACC(RendererModule, depthWrite), FieldFlag_Advanced));
        f.push_back(enumField("sortMode", "Sort Mode", JE_ACC(RendererModule, sortMode), kSortModes, FieldFlag_None,
                              "sorts the particles of one emitter (Alpha and Premultiplied blends only)"));
        f.push_back(field("sortingFudge", "Sorting Fudge", FieldType::Float, JE_ACC(RendererModule, sortingFudge)));
        f.push_back(field("sortPriority", "Sort Priority", FieldType::Int, JE_ACC(RendererModule, sortPriority)));
        f.push_back(shownIf(ranged(field("minScreenSize", "Min Particle Size", FieldType::Float, JE_ACC(RendererModule, minScreenSize)), 0.0f, 1.0f), notPixel));
        f.push_back(shownIf(ranged(field("maxScreenSize", "Max Particle Size", FieldType::Float, JE_ACC(RendererModule, maxScreenSize)), 0.0f, 10.0f), notPixel));
        f.push_back(field("renderLayers", "Rendering Layer Mask", FieldType::Mask, JE_ACC(RendererModule, renderLayers)));
        f.push_back(field("receiveShadows", "Receive Shadows", FieldType::Bool, JE_ACC(RendererModule, receiveShadows)));
        f.push_back(enumField("lighting", "Lighting", JE_ACC(RendererModule, lighting), kLightingModes));
        auto sixWay = JE_IF(RendererModule, m.lighting == LightingMode::SixWay);
        f.push_back(shownIf(enumField("sixWayLayout", "Six-Way Layout", JE_ACC(RendererModule, sixWayLayout), kSixWayLayouts), sixWay));
        f.push_back(shownIf(field("sixWayMapA", "Lightmap A", FieldType::String, JE_ACC(RendererModule, sixWayMapA), FieldFlag_AssetPath), sixWay));
        f.push_back(field("sixWayMapAGuid", "Lightmap A GUID", FieldType::String, JE_ACC(RendererModule, sixWayMapAGuid), FieldFlag_Guid | FieldFlag_Hidden));
        f.push_back(shownIf(field("sixWayMapB", "Lightmap B", FieldType::String, JE_ACC(RendererModule, sixWayMapB), FieldFlag_AssetPath), sixWay));
        f.push_back(field("sixWayMapBGuid", "Lightmap B GUID", FieldType::String, JE_ACC(RendererModule, sixWayMapBGuid), FieldFlag_Guid | FieldFlag_Hidden));
        f.push_back(shownIf(ranged(field("sixWayContrast", "Contrast", FieldType::Float, JE_ACC(RendererModule, sixWayContrast)), 0.0f, 8.0f), sixWay));
        f.push_back(shownIf(field("emissionMap", "Emission Map", FieldType::String, JE_ACC(RendererModule, emissionMap), FieldFlag_AssetPath), sixWay));
        f.push_back(field("emissionMapGuid", "Emission Map GUID", FieldType::String, JE_ACC(RendererModule, emissionMapGuid), FieldFlag_Guid | FieldFlag_Hidden));
        f.push_back(shownIf(field("emissionColor", "Emission Color", FieldType::Color, JE_ACC(RendererModule, emissionColor)), sixWay));
        f.push_back(shownIf(ranged(field("emissionIntensity", "Emission Intensity", FieldType::Float, JE_ACC(RendererModule, emissionIntensity)), 0.0f, 1.0e4f), sixWay));
        // the Pixel render mode's shape (S49 R9, R31): each shape's own fields under it
        auto pixel = JE_IF(RendererModule, m.renderMode == RenderMode::Pixel);
        f.push_back(shownIf(enumField("pixelShape", "Pixel Shape", JE_ACC(RendererModule, pixelShape), kPixelShapes, FieldFlag_None,
                                      "drawn per whole base pixel, no texture"), pixel));
        auto streak = JE_IF(RendererModule, m.renderMode == RenderMode::Pixel && m.pixelShape == PixelShape::Streak);
        f.push_back(shownIf(ranged(field("streakLengthScale", "Length Scale", FieldType::Float, JE_ACC(RendererModule, streakLengthScale), FieldFlag_None,
                                         "pixels of length per pixel per second of speed"), 0.0f, 10.0f), streak));
        f.push_back(shownIf(ranged(field("streakMaxLength", "Max Length", FieldType::Float, JE_ACC(RendererModule, streakMaxLength)), 1.0f, 64.0f), streak));
        f.push_back(shownIf(ranged(field("streakHeadFraction", "Head Fraction", FieldType::Float, JE_ACC(RendererModule, streakHeadFraction), FieldFlag_None,
                                         "the share of the length from the head drawn in the primary colour; the rest is secondary"), 0.0f, 1.0f), streak));
        f.push_back(shownIf(ranged(field("streakWidth", "Width", FieldType::Int, JE_ACC(RendererModule, streakWidth)), 1, 3), streak));
        f.push_back(shownIf(field("discRim", "Rim", FieldType::Bool, JE_ACC(RendererModule, discRim), FieldFlag_None, "the outer pixel ring in the secondary colour"),
                            JE_IF(RendererModule, m.renderMode == RenderMode::Pixel && m.pixelShape == PixelShape::Disc)));
        f.push_back(shownIf(ranged(field("ringWidth", "Ring Width", FieldType::Int, JE_ACC(RendererModule, ringWidth)), 1, 8),
                            JE_IF(RendererModule, m.renderMode == RenderMode::Pixel && m.pixelShape == PixelShape::Ring)));
        f.push_back(shownIf(field("dotShadeBottom", "Shade Bottom", FieldType::Bool, JE_ACC(RendererModule, dotShadeBottom), FieldFlag_None,
                                  "the bottom row in the secondary colour (2 px and larger)"),
                            JE_IF(RendererModule, m.renderMode == RenderMode::Pixel && m.pixelShape == PixelShape::Dot)));
        f.push_back(shownIf(ranged(field("glowBands", "Bands", FieldType::Int, JE_ACC(RendererModule, glowBands)), 1, 4),
                            JE_IF(RendererModule, m.renderMode == RenderMode::Pixel && m.pixelShape == PixelShape::Glow)));
        f.push_back(shownIf(ranged(field("rotationSteps", "Rotation Steps", FieldType::Int, JE_ACC(RendererModule, rotationSteps), FieldFlag_None,
                                         "angles per turn the shape is drawn at"), 1, 360),
                            JE_IF(RendererModule, m.renderMode == RenderMode::Pixel && (m.pixelShape == PixelShape::Shard || m.pixelShape == PixelShape::Chunk))));
        return d;
    }

    ModuleDesc makeSetOverAge(){
        ModuleDesc d = module<SetOverAgeModule>(ModuleId::SetOverAge, "setOverAge", "Set over Age", JE_DOC(setOverAge),
                                                "Sets a property to a curve over the particle's age inside a window of ages (the old from-to modifiers)");
        d.fields.fields.push_back(arrayField<std::vector<SetOverAgeEntry>>("entries", "Entries", JE_ACC(SetOverAgeModule, entries), &setEntryDesc(), (uint32_t)kMaxSetOverAgeEntries));
        return d;
    }

    ModuleDesc makePixelArt(){
        ModuleDesc d = module<PixelArtModule>(ModuleId::PixelArt, "pixelArt", "Pixel Art", JE_DOC(pixelArt),
                                              "Particles on the pixel grid at whole-pixel sizes, faded with a dither, held to a drawn frame rate");
        auto& f = d.fields.fields;
        f.push_back(field("snap", "Snap to Pixel Grid", FieldType::Bool, JE_ACC(PixelArtModule, snap)));
        f.push_back(field("pixelSizes", "Sizes in Pixels", FieldType::Bool, JE_ACC(PixelArtModule, pixelSizes), FieldFlag_None,
                          "Start Size, Size over Lifetime and Size by Speed are base pixels"));
        f.push_back(field("integerSize", "Integer Size", FieldType::Bool, JE_ACC(PixelArtModule, integerSize), FieldFlag_None, "boxes of whole pixels"));
        f.push_back(enumField("fade", "Fade", JE_ACC(PixelArtModule, fade), kPixelFades, FieldFlag_None,
                              "Dither: a Bayer pattern of whole pixels; Cutout: drawn above half; Alpha: blended"));
        auto dither = JE_IF(PixelArtModule, m.fade == PixelFade::Dither);
        f.push_back(shownIf(ranged(field("ditherSize", "Dither Size", FieldType::Int, JE_ACC(PixelArtModule, ditherSize), FieldFlag_None, "2, 4 or 8"), 2, 8), dither));
        f.push_back(shownIf(enumField("ditherSpace", "Dither Space", JE_ACC(PixelArtModule, ditherSpace), kDitherSpaces, FieldFlag_None,
                                      "Screen: the pattern holds still as particles move; Particle: it travels with each one"), dither));
        f.push_back(ranged(field("fadeOverLifetime", "Fade over Lifetime", FieldType::MinMaxCurve, JE_ACC(PixelArtModule, fadeOverLifetime)), 0.0f, 1.0f));
        f.push_back(ranged(field("holdFrameRate", "Hold Frames", FieldType::Float, JE_ACC(PixelArtModule, holdFrameRate), FieldFlag_None,
                                 "Hz: the drawn state changes only at these steps; 0 = every frame"), 0.0f, 240.0f));
        f.push_back(enumField("secondary", "Secondary Color", JE_ACC(PixelArtModule, secondary), kSecondaryModes, FieldFlag_None,
                              "what a shape's second colour is (tails, rims, caps, tips)"));
        f.push_back(shownIf(ranged(field("secondaryOffset", "Ramp Offset", FieldType::Int, JE_ACC(PixelArtModule, secondaryOffset), FieldFlag_None,
                                         "ramp steps from the particle's own; positive toward the ramp's end"), -8, 8),
                            JE_IF(PixelArtModule, m.secondary == SecondaryColorMode::RampOffset)));
        f.push_back(field("secondaryColor", "Secondary", FieldType::Color, JE_ACC(PixelArtModule, secondaryColor), FieldFlag_None,
                          "the second colour (Ramp Offset falls back to it without a ramp)"));
        f.push_back(field("wobbleAmplitude", "Wobble", FieldType::Vector2, JE_ACC(PixelArtModule, wobbleAmplitude), FieldFlag_None,
                          "a drawn sway in base pixels (x, y); it never moves the simulation"));
        f.push_back(field("wobbleFrequency", "Wobble Frequency", FieldType::Vector2, JE_ACC(PixelArtModule, wobbleFrequency), FieldFlag_None,
                          "Hz on x and y; a loop's frequencies are whole multiples of 1 / duration"));
        f.push_back(field("wobbleRandomPhase", "Wobble Random Phase", FieldType::Bool, JE_ACC(PixelArtModule, wobbleRandomPhase)));
        f.push_back(ranged(field("flickerBelow", "Flicker Below", FieldType::Float, JE_ACC(PixelArtModule, flickerBelow), FieldFlag_None,
                                 "blinks when less than this share of its life is left; 0: off"), 0.0f, 1.0f));
        f.push_back(shownIf(ranged(field("flickerRate", "Flicker Rate", FieldType::Float, JE_ACC(PixelArtModule, flickerRate)), 0.1f, 240.0f),
                            JE_IF(PixelArtModule, m.flickerBelow > 0.0f)));
        return d;
    }

    StructDesc makeDocument(){
        StructDesc s = structOf<ParticleEffectDocument>("ParticleEffect");
        auto& f = s.fields;
        f.push_back(ranged(field("duration", "Duration", FieldType::Float, JE_ACC(ParticleEffectDocument, duration)), 0.05f, 1.0e5f));
        f.push_back(field("looping", "Looping", FieldType::Bool, JE_ACC(ParticleEffectDocument, looping)));
        f.push_back(shownIf(field("prewarm", "Prewarm", FieldType::Bool, JE_ACC(ParticleEffectDocument, prewarm)), JE_IF(ParticleEffectDocument, m.looping)));
        f.push_back(shownIf(field("prewarmSync", "Prewarm at Play", FieldType::Bool, JE_ACC(ParticleEffectDocument, prewarmSync), FieldFlag_Advanced,
                                  "simulates the prewarm in one go when the system plays instead of over the first frames"),
                            JE_IF(ParticleEffectDocument, m.looping && m.prewarm)));
        f.push_back(ranged(field("startDelay", "Start Delay", FieldType::MinMaxCurve, JE_ACC(ParticleEffectDocument, startDelay), FieldFlag_None,
                                 "seconds before it emits; a random range picks one each play"), 0.0f, 1.0e5f));
        f.push_back(enumField("simulationSpace", "Simulation Space", JE_ACC(ParticleEffectDocument, simulationSpace), kSimulationSpaces));
        f.push_back(enumField("simulationMode", "Simulation Mode", JE_ACC(ParticleEffectDocument, simulationMode), kSimulationModes, FieldFlag_None,
                              "Simulated: every module, ticked; Analytic: each particle computed on the GPU from its slot and the time "
                              "(exact loops and scrubbing, no CPU cost per particle, a subset of the modules)"));
        f.push_back(ranged(field("tickRate", "Tick Rate", FieldType::Int, JE_ACC(ParticleEffectDocument, tickRate), FieldFlag_None,
                                 "simulation ticks per second; 0 follows the frames in bounded steps"), 0, 240));
        f.push_back(field("interpolate", "Interpolate", FieldType::Bool, JE_ACC(ParticleEffectDocument, interpolate)));
        f.push_back(ranged(field("maxParticles", "Max Particles", FieldType::UInt, JE_ACC(ParticleEffectDocument, maxParticles)), 1, (float)kMaxParticlesPerEmitter));
        f.push_back(field("seed", "Random Seed", FieldType::UInt, JE_ACC(ParticleEffectDocument, seed), FieldFlag_None, "0: every Particle System picks its own"));
        f.push_back(enumField("scalingMode", "Scaling Mode", JE_ACC(ParticleEffectDocument, scalingMode), kScalingModes));
        f.push_back(ranged(field("simulationSpeed", "Simulation Speed", FieldType::Float, JE_ACC(ParticleEffectDocument, simulationSpeed)), 0.0f, 100.0f));
        f.push_back(field("useUnscaledTime", "Unscaled Time", FieldType::Bool, JE_ACC(ParticleEffectDocument, useUnscaledTime), FieldFlag_None,
                          "ticks with the unscaled frame time: Time.timeScale does not slow it (UI, pause menus)"));
        f.push_back(enumField("stopAction", "Stop Action", JE_ACC(ParticleEffectDocument, stopAction), kStopActions, FieldFlag_None,
                              "when it has finished (not looping, its last particle gone): nothing, deactivate or destroy its GameObject, or "
                              "call its scripts' onParticleSystemStopped"));
        f.push_back(enumField("emitterVelocityMode", "Emitter Velocity", JE_ACC(ParticleEffectDocument, emitterVelocityMode), kEmitterVelocityModes,
                              FieldFlag_None, "the velocity Inherit Velocity and Rate over Distance read: the Transform's motion, or its Rigidbody's"));
        f.push_back(enumField("cullingMode", "Culling Mode", JE_ACC(ParticleEffectDocument, cullingMode), kCullingModes));
        f.push_back(enumField("boundsMode", "Bounds", JE_ACC(ParticleEffectDocument, boundsMode), kBoundsModes, FieldFlag_Advanced));
        f.push_back(shownIf(field("boundsCenter", "Bounds Center", FieldType::Vector3, JE_ACC(ParticleEffectDocument, boundsCenter), FieldFlag_Advanced),
                            JE_IF(ParticleEffectDocument, m.boundsMode == BoundsMode::Authored)));
        f.push_back(shownIf(field("boundsSize", "Bounds Size", FieldType::Vector3, JE_ACC(ParticleEffectDocument, boundsSize), FieldFlag_Advanced),
                            JE_IF(ParticleEffectDocument, m.boundsMode == BoundsMode::Authored)));
        f.push_back(field("priority", "Budget Priority", FieldType::Int, JE_ACC(ParticleEffectDocument, priority), FieldFlag_Advanced,
                          "over the world's particle budget, higher priorities keep more of their emission"));
        f.push_back(field("lodEnabled", "Distance LOD", FieldType::Bool, JE_ACC(ParticleEffectDocument, lod.enabled), FieldFlag_Advanced));
        auto lod = JE_IF(ParticleEffectDocument, m.lod.enabled);
        f.push_back(shownIf(ranged(field("lodEmissionScale", "LOD Emission", FieldType::MinMaxCurve, JE_ACC(ParticleEffectDocument, lod.emissionScale), FieldFlag_Advanced), 0.0f, 1.0f), lod));
        f.push_back(shownIf(ranged(field("lodRenderThinning", "LOD Drawn Share", FieldType::MinMaxCurve, JE_ACC(ParticleEffectDocument, lod.renderThinning), FieldFlag_Advanced), 0.0f, 1.0f), lod));
        f.push_back(shownIf(ranged(field("lodMaxDistance", "LOD Max Distance", FieldType::Float, JE_ACC(ParticleEffectDocument, lod.maxDistance), FieldFlag_Advanced), 0.0f, 1.0e7f), lod));
        f.push_back(ranged(field("designPixelsPerUnit", "Design Pixels Per Unit", FieldType::Float, JE_ACC(ParticleEffectDocument, designPixelsPerUnit),
                                 FieldFlag_Hidden, "the project Pixels Per Unit a pixel preset was made for"), 0.0f, 100000.0f));
        return s;
    }

    StructDesc makeSubEmitter(){
        StructDesc s = structOf<SubEmitterEntry>("SubEmitter");
        auto& f = s.fields;
        f.push_back(enumField("trigger", "Trigger", JE_ACC(SubEmitterEntry, trigger), kSubEmitterTriggers));
        f.push_back(field("effect", "Effect", FieldType::String, JE_ACC(SubEmitterEntry, effect), FieldFlag_AssetPath));
        f.push_back(field("effectGuid", "Effect GUID", FieldType::String, JE_ACC(SubEmitterEntry, effectGuid), FieldFlag_Guid | FieldFlag_Hidden));
        f.push_back(shownIf(field("triggerName", "Trigger Name", FieldType::String, JE_ACC(SubEmitterEntry, triggerName)), JE_IF(SubEmitterEntry, m.trigger == SubEmitterTrigger::Trigger)));
        f.push_back(field("inheritColor", "Inherit Color", FieldType::Bool, JE_ACC(SubEmitterEntry, inheritColor)));
        f.push_back(field("inheritSize", "Inherit Size", FieldType::Bool, JE_ACC(SubEmitterEntry, inheritSize)));
        f.push_back(field("inheritRotation", "Inherit Rotation", FieldType::Bool, JE_ACC(SubEmitterEntry, inheritRotation)));
        f.push_back(field("inheritLifetime", "Inherit Lifetime", FieldType::Bool, JE_ACC(SubEmitterEntry, inheritLifetime)));
        f.push_back(field("inheritVelocity", "Inherit Velocity", FieldType::Bool, JE_ACC(SubEmitterEntry, inheritVelocity)));
        f.push_back(ranged(field("inheritFactor", "Inherit Factor", FieldType::Float, JE_ACC(SubEmitterEntry, inheritFactor)), 0.0f, 100.0f));
        f.push_back(ranged(field("emitCount", "Emit Count", FieldType::Int, JE_ACC(SubEmitterEntry, emitCount)), 0, 10000));
        f.push_back(ranged(field("probability", "Emit Probability", FieldType::Float, JE_ACC(SubEmitterEntry, probability)), 0.0f, 1.0f));
        f.push_back(field("followParent", "Follow Parent", FieldType::Bool, JE_ACC(SubEmitterEntry, followParent), FieldFlag_None,
                          "the children move with the particle that spawned them while it lives (trails on a moving parent)"));
        auto follow = JE_IF(SubEmitterEntry, m.followParent);
        f.push_back(shownIf(field("followRotation", "Follow Rotation", FieldType::Bool, JE_ACC(SubEmitterEntry, followRotation), FieldFlag_None,
                                  "and turn with it: its 3D rotation (mesh particles), else its roll about the Z axis"), follow));
        f.push_back(shownIf(field("followScale", "Follow Scale", FieldType::Bool, JE_ACC(SubEmitterEntry, followScale), FieldFlag_None,
                                  "and spread or gather as its size changes"), follow));
        f.push_back(field("soundEvent", "Sound Event", FieldType::String, JE_ACC(SubEmitterEntry, soundEvent), FieldFlag_AssetPath,
                          "a .soundevent played where the entry fires"));
        f.push_back(field("soundEventGuid", "Sound Event GUID", FieldType::String, JE_ACC(SubEmitterEntry, soundEventGuid), FieldFlag_Guid | FieldFlag_Hidden));
        return s;
    }

    StructDesc makeTrigger(){
        StructDesc s = structOf<EffectTrigger>("Trigger");
        auto& f = s.fields;
        f.push_back(field("name", "Name", FieldType::String, JE_ACC(EffectTrigger, name), FieldFlag_None, "what sendTrigger names"));
        f.push_back(enumField("action", "Action", JE_ACC(EffectTrigger, action), kTriggerActions, FieldFlag_None,
                              "what it does to this emitter; Trigger sub-emitters of the same name fire too"));
        f.push_back(field("soundEvent", "Sound Event", FieldType::String, JE_ACC(EffectTrigger, soundEvent), FieldFlag_AssetPath,
                          "a .soundevent played at the emitter"));
        f.push_back(field("soundEventGuid", "Sound Event GUID", FieldType::String, JE_ACC(EffectTrigger, soundEventGuid), FieldFlag_Guid | FieldFlag_Hidden));
        return s;
    }

    StructDesc makeExposed(){
        StructDesc s = structOf<ExposedParameter>("ExposedParameter");
        auto& f = s.fields;
        f.push_back(field("name", "Name", FieldType::String, JE_ACC(ExposedParameter, name)));
        f.push_back(field("path", "Field", FieldType::String, JE_ACC(ExposedParameter, path)));
        f.push_back(enumField("type", "Type", JE_ACC(ExposedParameter, type), kExposedTypes));
        return s;
    }

    // splits "bursts[3]" into ("bursts", 3); index -1 without brackets
    bool splitIndex(std::string_view part, std::string_view& key, long& index){
        index = -1;
        const size_t open = part.find('[');
        if (open == std::string_view::npos){
            key = part;
            return true;
        }
        if (part.back() != ']' || open + 2 > part.size() - 1)
            return false;
        key = part.substr(0, open);
        const std::string digits(part.substr(open + 1, part.size() - open - 2));
        char* end = nullptr;
        index = std::strtol(digits.c_str(), &end, 10);
        return end && *end == '\0' && index >= 0;
    }

}

const EnumOption* FieldDesc::findOption(std::string_view json) const{
    for (uint8_t i = 0; i < optionCount; i++){
        if (json == options[i].json)
            return &options[i];
    }
    return nullptr;
}

const EnumOption* FieldDesc::findOption(uint8_t value) const{
    for (uint8_t i = 0; i < optionCount; i++){
        if (options[i].value == value)
            return &options[i];
    }
    return nullptr;
}

const FieldDesc* StructDesc::find(std::string_view key) const{
    for (const FieldDesc& f : fields){
        if (key == f.key)
            return &f;
    }
    return nullptr;
}

bool ModuleDesc::isPresent(const ParticleEffectDocument& document) const{
    if (alwaysPresent || !present)
        return true;
    return *present(const_cast<void*>(module(document)));
}

bool ModuleDesc::isEnabled(const ParticleEffectDocument& document) const{
    if (!isPresent(document))
        return false;
    if (!hasEnable || !enabled)
        return true;
    return *enabled(const_cast<void*>(module(document)));
}

void ModuleDesc::setPresent(ParticleEffectDocument& document, bool value) const{
    if (!alwaysPresent && present)
        *present(module(document)) = value;
}

void ModuleDesc::setEnabled(ParticleEffectDocument& document, bool value) const{
    if (hasEnable && enabled)
        *enabled(module(document)) = value;
}

const std::vector<ModuleDesc>& ParticleModules::all(){
    static const std::vector<ModuleDesc> modules = {
        makeMain(), makeEmission(), makeShape(), makeStartVelocity(), makeVelocityOverLifetime(), makeLimitVelocity(),
        makeForceOverLifetime(), makeColorOverLifetime(), makeColorBySpeed(), makeSizeOverLifetime(), makeSizeBySpeed(),
        makeRotationOverLifetime(), makeRotationBySpeed(), makeExternalForces(), makeNoise(), makeOrbit(), makeCollision(),
        makeSubEmittersModule(), makeTextureSheet(), makeTrails(), makeCustomData(), makeSetOverAge(), makePixelArt(), makeRenderer()
    };
    return modules;
}

const ModuleDesc* ParticleModules::find(std::string_view key){
    for (const ModuleDesc& m : all()){
        if (key == m.key)
            return &m;
    }
    return nullptr;
}

const ModuleDesc& ParticleModules::get(ModuleId id){
    for (const ModuleDesc& m : all()){
        if (m.id == id)
            return m;
    }
    return all().front();
}

const StructDesc& ParticleModules::documentFields(){
    static const StructDesc s = makeDocument();
    return s;
}

const StructDesc& ParticleModules::subEmitterFields(){
    static const StructDesc s = makeSubEmitter();
    return s;
}

const StructDesc& ParticleModules::exposedFields(){
    static const StructDesc s = makeExposed();
    return s;
}

const StructDesc& ParticleModules::triggerFields(){
    static const StructDesc s = makeTrigger();
    return s;
}

ParticleModules::Resolved ParticleModules::resolve(ParticleEffectDocument& document, std::string_view path){
    Resolved result;
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= path.size()){
        const size_t dot = path.find('.', start);
        const size_t end = dot == std::string_view::npos ? path.size() : dot;
        parts.push_back(path.substr(start, end - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    if (parts.empty() || parts[0].empty())
        return result;

    const StructDesc* desc = nullptr;
    void* owner = nullptr;
    size_t next = 0;
    if (const ModuleDesc* module = find(parts[0])){
        result.module = module;
        desc = &module->fields;
        owner = module->module(document);
        next = 1;
    }else{
        desc = &documentFields();
        owner = &document;
    }

    for (size_t i = next; i < parts.size(); i++){
        std::string_view key;
        long index = -1;
        if (!splitIndex(parts[i], key, index))
            return Resolved{};
        const FieldDesc* field = desc ? desc->find(key) : nullptr;
        if (!field)
            return Resolved{};
        void* storage = field->field(owner);
        if (index >= 0){
            if (field->type != FieldType::Array || (size_t)index >= field->array.size(storage))
                return Resolved{};
            owner = field->array.at(storage, (size_t)index);
            desc = field->element;
            if (i + 1 == parts.size())
                return Resolved{};   // an element is not a field
            continue;
        }
        if (field->type == FieldType::Struct && i + 1 < parts.size()){
            owner = storage;
            desc = field->element;
            continue;
        }
        result.field = field;
        result.storage = storage;
        if (i + 1 < parts.size()){
            // a member of a value: "rateOverTime.constant", "startColor.color"
            if (i + 2 != parts.size() || (field->type != FieldType::MinMaxCurve && field->type != FieldType::MinMaxGradient &&
                                          field->type != FieldType::Vector3 && field->type != FieldType::Vector4 && field->type != FieldType::Color &&
                                          field->type != FieldType::Vector2))
                return Resolved{};
            result.member = std::string(parts[i + 1]);
        }
        return result;
    }
    return Resolved{};
}

const std::vector<EnumOption>& ParticleModules::curveModes(){
    static const std::vector<EnumOption> v = {{"constant", "Constant", 0}, {"twoConstants", "Random Between Two Constants", 1},
                                              {"curve", "Curve", 2}, {"twoCurves", "Random Between Two Curves", 3}, {"ease", "Ease", 4}};
    return v;
}

const std::vector<EnumOption>& ParticleModules::easeTypes(){
    // every preset with a function of its own (CUSTOM and CURVE have none)
    static const std::vector<EnumOption> v = {
        {"linear", "Linear", (uint8_t)EaseType::LINEAR},
        {"quadIn", "Quad In", (uint8_t)EaseType::QUAD_IN}, {"quadOut", "Quad Out", (uint8_t)EaseType::QUAD_OUT},
        {"quadInOut", "Quad In Out", (uint8_t)EaseType::QUAD_IN_OUT},
        {"cubicIn", "Cubic In", (uint8_t)EaseType::CUBIC_IN}, {"cubicOut", "Cubic Out", (uint8_t)EaseType::CUBIC_OUT},
        {"cubicInOut", "Cubic In Out", (uint8_t)EaseType::CUBIC_IN_OUT},
        {"quartIn", "Quart In", (uint8_t)EaseType::QUART_IN}, {"quartOut", "Quart Out", (uint8_t)EaseType::QUART_OUT},
        {"quartInOut", "Quart In Out", (uint8_t)EaseType::QUART_IN_OUT},
        {"quintIn", "Quint In", (uint8_t)EaseType::QUINT_IN}, {"quintOut", "Quint Out", (uint8_t)EaseType::QUINT_OUT},
        {"quintInOut", "Quint In Out", (uint8_t)EaseType::QUINT_IN_OUT},
        {"sineIn", "Sine In", (uint8_t)EaseType::SINE_IN}, {"sineOut", "Sine Out", (uint8_t)EaseType::SINE_OUT},
        {"sineInOut", "Sine In Out", (uint8_t)EaseType::SINE_IN_OUT},
        {"expoIn", "Expo In", (uint8_t)EaseType::EXPO_IN}, {"expoOut", "Expo Out", (uint8_t)EaseType::EXPO_OUT},
        {"expoInOut", "Expo In Out", (uint8_t)EaseType::EXPO_IN_OUT},
        {"circIn", "Circ In", (uint8_t)EaseType::CIRC_IN}, {"circOut", "Circ Out", (uint8_t)EaseType::CIRC_OUT},
        {"circInOut", "Circ In Out", (uint8_t)EaseType::CIRC_IN_OUT},
        {"elasticIn", "Elastic In", (uint8_t)EaseType::ELASTIC_IN}, {"elasticOut", "Elastic Out", (uint8_t)EaseType::ELASTIC_OUT},
        {"elasticInOut", "Elastic In Out", (uint8_t)EaseType::ELASTIC_IN_OUT},
        {"backIn", "Back In", (uint8_t)EaseType::BACK_IN}, {"backOut", "Back Out", (uint8_t)EaseType::BACK_OUT},
        {"backInOut", "Back In Out", (uint8_t)EaseType::BACK_IN_OUT},
        {"bounceIn", "Bounce In", (uint8_t)EaseType::BOUNCE_IN}, {"bounceOut", "Bounce Out", (uint8_t)EaseType::BOUNCE_OUT},
        {"bounceInOut", "Bounce In Out", (uint8_t)EaseType::BOUNCE_IN_OUT},
        {"step", "Step", (uint8_t)EaseType::STEP}};
    return v;
}

const std::vector<EnumOption>& ParticleModules::gradientModes(){
    static const std::vector<EnumOption> v = {{"color", "Color", 0}, {"gradient", "Gradient", 1},
                                              {"twoColors", "Random Between Two Colors", 2}, {"twoGradients", "Random Between Two Gradients", 3},
                                              {"randomColor", "Random Color", 4}, {"ramp", "Palette Ramp", 5}};
    return v;
}

const std::vector<EnumOption>& ParticleModules::curveDrivers(){
    static const std::vector<EnumOption> v = {{"normalizedAge", "Normalized Age", 0}, {"age", "Age (s)", 1}, {"speed", "Speed", 2},
                                              {"emitterTime", "Emitter Time", 3}, {"random", "Random", 4}};
    return v;
}

const std::vector<EnumOption>& ParticleModules::curveWraps(){
    static const std::vector<EnumOption> v = {{"clamp", "Clamp", 0}, {"repeat", "Repeat", 1}, {"pingPong", "Ping-Pong", 2}};
    return v;
}

const std::vector<EnumOption>& ParticleModules::gradientBlendModes(){
    static const std::vector<EnumOption> v = {{"blend", "Blend", 0}, {"fixed", "Fixed", 1}};
    return v;
}

namespace {

    const ExposedParameter* findExposed(const ParticleEffectDocument& document, std::string_view name){
        for (const ExposedParameter& e : document.exposed){
            if (e.name == name)
                return &e;
        }
        return nullptr;
    }

    float* componentOf(Vector4& v, const std::string& member){
        if (member == "x") return &v.x;
        if (member == "y") return &v.y;
        if (member == "z") return &v.z;
        if (member == "w") return &v.w;
        return nullptr;
    }

    float* componentOf(Vector3& v, const std::string& member){
        if (member == "x") return &v.x;
        if (member == "y") return &v.y;
        if (member == "z") return &v.z;
        return nullptr;
    }

    // the exposed value's storage: reads (write false) or writes it
    bool accessExposed(ParticleEffectDocument& document, std::string_view name, Vector4& value, bool write){
        const ExposedParameter* e = findExposed(document, name);
        if (!e)
            return false;
        const ParticleModules::Resolved r = ParticleModules::resolve(document, e->path);
        if (!r.field || !r.storage)
            return false;
        const std::string& m = r.member;
        switch (r.field->type){
            case FieldType::Float: {
                if (!m.empty()) return false;
                float& f = *static_cast<float*>(r.storage);
                if (write) f = value.x; else value = Vector4(f, 0, 0, 0);
                return true;
            }
            case FieldType::Vector3: {
                Vector3& v = *static_cast<Vector3*>(r.storage);
                if (m.empty()){
                    if (write) v = Vector3(value.x, value.y, value.z); else value = Vector4(v.x, v.y, v.z, 0);
                    return true;
                }
                float* c = componentOf(v, m);
                if (!c) return false;
                if (write) *c = value.x; else value = Vector4(*c, 0, 0, 0);
                return true;
            }
            case FieldType::Vector4:
            case FieldType::Color: {
                Vector4& v = *static_cast<Vector4*>(r.storage);
                if (m.empty()){
                    if (write) v = value; else value = v;
                    return true;
                }
                float* c = componentOf(v, m);
                if (!c) return false;
                if (write) *c = value.x; else value = Vector4(*c, 0, 0, 0);
                return true;
            }
            case FieldType::MinMaxCurve: {
                MinMaxCurve& c = *static_cast<MinMaxCurve*>(r.storage);
                if (write)
                    return ParticleModules::writeExposedMember(c, m, value.x);
                float* target = nullptr;
                if (m == "constant") target = &c.constant;
                else if (m == "constantMax" || (m.empty() && c.mode == CurveMode::Ease)) target = &c.constantMax;
                else if (m == "multiplier" || (m.empty() && c.usesCurve())) target = &c.multiplier;
                else if (m.empty()) target = &c.constant;
                if (!target) return false;
                value = Vector4(*target, 0, 0, 0);
                return true;
            }
            case FieldType::MinMaxGradient: {
                MinMaxGradient& g = *static_cast<MinMaxGradient*>(r.storage);
                if (write)
                    return ParticleModules::writeExposedMember(g, m, value);
                const Vector4* target = m == "colorMax" ? &g.colorMax : ((m.empty() || m == "color") ? &g.color : nullptr);
                if (!target) return false;
                value = *target;
                return true;
            }
            default:
                return false;
        }
    }

}

bool ParticleModules::writeExposedMember(MinMaxCurve& c, const std::string& member, float value){
    float* target = nullptr;
    if (member == "constant") target = &c.constant;
    // an Ease value without a member: the value it eases to (its multiplier stays as shown)
    else if (member == "constantMax" || (member.empty() && c.mode == CurveMode::Ease)) target = &c.constantMax;
    else if (member == "multiplier" || (member.empty() && c.usesCurve())) target = &c.multiplier;
    else if (member.empty()) target = &c.constant;
    if (!target) return false;
    *target = value;
    if (member.empty() && c.mode == CurveMode::Constant) c.constantMax = value;
    // an ease's start or end moves its curve
    if (c.mode == CurveMode::Ease && target != &c.multiplier) c.rebuildEase();
    return true;
}

bool ParticleModules::writeExposedMember(MinMaxGradient& g, const std::string& member, const Vector4& value){
    Vector4* target = member == "colorMax" ? &g.colorMax : ((member.empty() || member == "color") ? &g.color : nullptr);
    if (!target) return false;
    *target = value;
    if (member.empty() && g.mode == GradientValueMode::Color) g.colorMax = value;
    return true;
}

bool ParticleModules::setExposed(ParticleEffectDocument& document, std::string_view name, const Vector4& value){
    Vector4 v = value;
    return accessExposed(document, name, v, true);
}

bool ParticleModules::getExposed(const ParticleEffectDocument& document, std::string_view name, Vector4& value){
    return accessExposed(const_cast<ParticleEffectDocument&>(document), name, value, false);
}

namespace {

    void visitStruct(const StructDesc& desc, void* owner, const std::string& prefix, const ParticleModules::AssetVisitor& visit){
        for (size_t f = 0; f < desc.fields.size(); f++){
            const FieldDesc& field = desc.fields[f];
            const std::string path = prefix.empty() ? std::string(field.key) : prefix + "." + field.key;
            if (field.flags & FieldFlag_AssetPath){
                std::string& value = *static_cast<std::string*>(field.field(owner));
                // its GUID is the field after it
                std::string* guid = nullptr;
                if (f + 1 < desc.fields.size() && (desc.fields[f + 1].flags & FieldFlag_Guid))
                    guid = static_cast<std::string*>(desc.fields[f + 1].field(owner));
                std::string none;
                const std::string key = field.key;
                const ParticleModules::AssetKind kind = key == "model" ? ParticleModules::AssetKind::Model
                                                      : key == "effect" ? ParticleModules::AssetKind::Effect
                                                      : key == "soundEvent" ? ParticleModules::AssetKind::SoundEvent : ParticleModules::AssetKind::Texture;
                visit(value, guid ? *guid : none, kind, path);
            }else if (field.type == FieldType::MinMaxGradient){
                // a Ramp's palette (S49 R43): the path before "#ramp:", put back with its ramp name
                MinMaxGradient& gradient = *static_cast<MinMaxGradient*>(field.field(owner));
                std::string palette, ramp;
                if (gradient.mode == GradientValueMode::Ramp && pixelart::parsePaletteRampPath(gradient.ramp, &palette, &ramp)){
                    std::string guid;
                    const std::string before = palette;
                    visit(palette, guid, ParticleModules::AssetKind::Palette, path + ".ramp");
                    if (palette != before)
                        gradient.ramp = palette.empty() ? std::string() : palette + "#ramp:" + ramp;
                }
            }else if (field.type == FieldType::Struct && field.element){
                visitStruct(*field.element, field.field(owner), path, visit);
            }else if (field.type == FieldType::Array && field.element){
                void* vector = field.field(owner);
                const size_t count = field.array.size(vector);
                for (size_t i = 0; i < count; i++)
                    visitStruct(*field.element, field.array.at(vector, i), path + "[" + std::to_string(i) + "]", visit);
            }
        }
    }

}

void ParticleModules::visitAssets(ParticleEffectDocument& document, const AssetVisitor& visit){
    visitStruct(documentFields(), &document, std::string(), visit);
    for (const ModuleDesc& module : all())
        visitStruct(module.fields, module.module(document), module.key, visit);
    for (size_t i = 0; i < document.subEmitters.size(); i++)
        visitStruct(subEmitterFields(), &document.subEmitters[i], "subEmitters[" + std::to_string(i) + "]", visit);
    for (size_t i = 0; i < document.triggers.size(); i++)
        visitStruct(triggerFields(), &document.triggers[i], "triggers[" + std::to_string(i) + "]", visit);
}
