// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleFields.h"

#include <type_traits>
#include <unordered_map>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    const std::vector<std::string> SIMULATION_SPACE = {"Local", "World"};
    const std::vector<std::string> SCALING_MODE = {"Hierarchy", "Local", "Shape"};
    const std::vector<std::string> STOP_ACTION = {"None", "Disable", "Destroy", "Callback"};
    const std::vector<std::string> EMITTER_VELOCITY = {"Transform", "Rigidbody"};
    const std::vector<std::string> GRAVITY_SOURCE = {"Physics3D", "Physics2D"};
    const std::vector<std::string> SHAPE_TYPE = {"Sphere", "Hemisphere", "Cone", "ConeVolume", "Box", "BoxShell", "BoxEdge",
                                                 "Circle", "Edge", "Rectangle", "Donut"};
    const std::vector<std::string> MULTI_MODE = {"Random", "Loop", "PingPong", "BurstSpread"};
    const std::vector<std::string> INHERIT_MODE = {"Initial", "Current"};
    const std::vector<std::string> ANIMATION_MODE = {"Grid", "Sprites"};
    const std::vector<std::string> ANIMATION_TYPE = {"WholeSheet", "SingleRow"};
    const std::vector<std::string> ROW_MODE = {"Custom", "Random"};
    const std::vector<std::string> TIME_MODE = {"Lifetime", "Speed", "FPS"};
    const std::vector<std::string> RENDER_MODE = {"Billboard", "Stretch", "HorizontalBillboard", "VerticalBillboard", "Mesh"};
    const std::vector<std::string> SORT_MODE = {"None", "Distance", "OldestInFront", "YoungestInFront"};
    const std::vector<std::string> RENDER_SPACE = {"View", "World", "Local", "Facing", "Velocity"};

    // the C++ name of a module enum's type (the exported-build code casts to it)
    template<typename T> const char* enumTypeOf();
    template<> const char* enumTypeOf<ParticleSystemSimulationSpace>(){ return "ParticleSystemSimulationSpace"; }
    template<> const char* enumTypeOf<ParticleSystemScalingMode>(){ return "ParticleSystemScalingMode"; }
    template<> const char* enumTypeOf<ParticleSystemStopAction>(){ return "ParticleSystemStopAction"; }
    template<> const char* enumTypeOf<ParticleSystemEmitterVelocityMode>(){ return "ParticleSystemEmitterVelocityMode"; }
    template<> const char* enumTypeOf<ParticleSystemGravitySource>(){ return "ParticleSystemGravitySource"; }
    template<> const char* enumTypeOf<ParticleSystemShapeType>(){ return "ParticleSystemShapeType"; }
    template<> const char* enumTypeOf<ParticleSystemShapeMultiModeValue>(){ return "ParticleSystemShapeMultiModeValue"; }
    template<> const char* enumTypeOf<ParticleSystemInheritVelocityMode>(){ return "ParticleSystemInheritVelocityMode"; }
    template<> const char* enumTypeOf<ParticleSystemAnimationMode>(){ return "ParticleSystemAnimationMode"; }
    template<> const char* enumTypeOf<ParticleSystemAnimationType>(){ return "ParticleSystemAnimationType"; }
    template<> const char* enumTypeOf<ParticleSystemAnimationRowMode>(){ return "ParticleSystemAnimationRowMode"; }
    template<> const char* enumTypeOf<ParticleSystemAnimationTimeMode>(){ return "ParticleSystemAnimationTimeMode"; }
    template<> const char* enumTypeOf<ParticleSystemRenderMode>(){ return "ParticleSystemRenderMode"; }
    template<> const char* enumTypeOf<ParticleSystemSortMode>(){ return "ParticleSystemSortMode"; }
    template<> const char* enumTypeOf<ParticleSystemRenderSpace>(){ return "ParticleSystemRenderSpace"; }

    template<typename Module, typename Value>
    const char* enumTypeName(Module ParticleSystemComponent::*, Value Module::*){
        static_assert(sizeof(Value) == sizeof(int), "module enums are int sized (the Catalog reads them as int)");
        return enumTypeOf<Value>();
    }

#define PS_FIELD(MODULE, MEMBER, TYPE) \
    FieldInfo{#MODULE, #MEMBER, FieldType::TYPE, [](ParticleSystemComponent& c) -> void* { return &c.MODULE.MEMBER; }}
#define PS_ENUM(MODULE, MEMBER, NAMES) \
    FieldInfo{#MODULE, #MEMBER, FieldType::Enum, [](ParticleSystemComponent& c) -> void* { return &c.MODULE.MEMBER; }, &NAMES, false, false, \
              enumTypeName(&ParticleSystemComponent::MODULE, &std::remove_reference_t<decltype(std::declval<ParticleSystemComponent&>().MODULE)>::MEMBER)}

    FieldInfo rebuild(FieldInfo field){
        field.rebuild = true;
        return field;
    }

    FieldInfo radians(FieldInfo field){
        field.radians = true;
        return field;
    }

    std::vector<FieldInfo> makeFields(){
        return {
            // main
            PS_FIELD(main, duration, Float),
            PS_FIELD(main, looping, Bool),
            PS_FIELD(main, prewarm, Bool),
            PS_FIELD(main, startDelay, Curve),
            PS_FIELD(main, startLifetime, Curve),
            PS_FIELD(main, startSpeed, Curve),
            PS_FIELD(main, startSize3D, Bool),
            PS_FIELD(main, startSize, Curve),
            PS_FIELD(main, startSizeY, Curve),
            PS_FIELD(main, startSizeZ, Curve),
            PS_FIELD(main, startRotation3D, Bool),
            radians(PS_FIELD(main, startRotation, Curve)),
            radians(PS_FIELD(main, startRotationX, Curve)),
            radians(PS_FIELD(main, startRotationY, Curve)),
            PS_FIELD(main, flipRotation, Float),
            PS_FIELD(main, startColor, Gradient),
            PS_FIELD(main, gravityModifier, Curve),
            PS_ENUM(main, gravitySource, GRAVITY_SOURCE),
            PS_ENUM(main, simulationSpace, SIMULATION_SPACE),
            PS_FIELD(main, simulationSpeed, Float),
            PS_ENUM(main, scalingMode, SCALING_MODE),
            PS_FIELD(main, playOnAwake, Bool),
            rebuild(PS_FIELD(main, maxParticles, UInt)),
            PS_ENUM(main, emitterVelocityMode, EMITTER_VELOCITY),
            PS_ENUM(main, stopAction, STOP_ACTION),
            PS_FIELD(main, autoRandomSeed, Bool),
            PS_FIELD(main, randomSeed, UInt),

            // emission
            PS_FIELD(emission, enabled, Bool),
            PS_FIELD(emission, rateOverTime, Curve),
            PS_FIELD(emission, rateOverDistance, Curve),
            PS_FIELD(emission, bursts, Bursts),

            // shape
            PS_FIELD(shape, enabled, Bool),
            PS_ENUM(shape, shapeType, SHAPE_TYPE),
            PS_FIELD(shape, radius, Float),
            PS_FIELD(shape, radiusThickness, Float),
            PS_ENUM(shape, radiusMode, MULTI_MODE),
            PS_FIELD(shape, radiusSpread, Float),
            PS_FIELD(shape, radiusSpeed, Curve),
            PS_FIELD(shape, angle, Float),
            PS_FIELD(shape, length, Float),
            PS_FIELD(shape, arc, Float),
            PS_ENUM(shape, arcMode, MULTI_MODE),
            PS_FIELD(shape, arcSpread, Float),
            PS_FIELD(shape, arcSpeed, Curve),
            PS_FIELD(shape, boxThickness, Vector3),
            PS_FIELD(shape, donutRadius, Float),
            PS_FIELD(shape, position, Vector3),
            PS_FIELD(shape, rotation, Vector3),
            PS_FIELD(shape, scale, Vector3),
            PS_FIELD(shape, alignToDirection, Bool),
            PS_FIELD(shape, randomDirectionAmount, Float),
            PS_FIELD(shape, sphericalDirectionAmount, Float),
            PS_FIELD(shape, randomPositionAmount, Float),

            // velocity over lifetime
            PS_FIELD(velocityOverLifetime, enabled, Bool),
            PS_FIELD(velocityOverLifetime, x, Curve),
            PS_FIELD(velocityOverLifetime, y, Curve),
            PS_FIELD(velocityOverLifetime, z, Curve),
            radians(PS_FIELD(velocityOverLifetime, orbitalX, Curve)),
            radians(PS_FIELD(velocityOverLifetime, orbitalY, Curve)),
            radians(PS_FIELD(velocityOverLifetime, orbitalZ, Curve)),
            PS_FIELD(velocityOverLifetime, orbitalOffsetX, Curve),
            PS_FIELD(velocityOverLifetime, orbitalOffsetY, Curve),
            PS_FIELD(velocityOverLifetime, orbitalOffsetZ, Curve),
            PS_FIELD(velocityOverLifetime, radial, Curve),
            PS_FIELD(velocityOverLifetime, speedModifier, Curve),
            PS_ENUM(velocityOverLifetime, space, SIMULATION_SPACE),

            // limit velocity over lifetime
            PS_FIELD(limitVelocityOverLifetime, enabled, Bool),
            PS_FIELD(limitVelocityOverLifetime, separateAxes, Bool),
            PS_FIELD(limitVelocityOverLifetime, limit, Curve),
            PS_FIELD(limitVelocityOverLifetime, limitX, Curve),
            PS_FIELD(limitVelocityOverLifetime, limitY, Curve),
            PS_FIELD(limitVelocityOverLifetime, limitZ, Curve),
            PS_ENUM(limitVelocityOverLifetime, space, SIMULATION_SPACE),
            PS_FIELD(limitVelocityOverLifetime, dampen, Float),
            PS_FIELD(limitVelocityOverLifetime, drag, Curve),
            PS_FIELD(limitVelocityOverLifetime, multiplyDragByParticleSize, Bool),
            PS_FIELD(limitVelocityOverLifetime, multiplyDragByParticleVelocity, Bool),

            // inherit velocity
            PS_FIELD(inheritVelocity, enabled, Bool),
            PS_ENUM(inheritVelocity, mode, INHERIT_MODE),
            PS_FIELD(inheritVelocity, curve, Curve),

            // force over lifetime
            PS_FIELD(forceOverLifetime, enabled, Bool),
            PS_FIELD(forceOverLifetime, x, Curve),
            PS_FIELD(forceOverLifetime, y, Curve),
            PS_FIELD(forceOverLifetime, z, Curve),
            PS_ENUM(forceOverLifetime, space, SIMULATION_SPACE),
            PS_FIELD(forceOverLifetime, randomized, Bool),

            // colour over lifetime / by speed
            PS_FIELD(colorOverLifetime, enabled, Bool),
            PS_FIELD(colorOverLifetime, color, Gradient),
            PS_FIELD(colorBySpeed, enabled, Bool),
            PS_FIELD(colorBySpeed, color, Gradient),
            PS_FIELD(colorBySpeed, range, Vector2),

            // size over lifetime / by speed
            PS_FIELD(sizeOverLifetime, enabled, Bool),
            PS_FIELD(sizeOverLifetime, separateAxes, Bool),
            PS_FIELD(sizeOverLifetime, size, Curve),
            PS_FIELD(sizeOverLifetime, y, Curve),
            PS_FIELD(sizeOverLifetime, z, Curve),
            PS_FIELD(sizeBySpeed, enabled, Bool),
            PS_FIELD(sizeBySpeed, separateAxes, Bool),
            PS_FIELD(sizeBySpeed, size, Curve),
            PS_FIELD(sizeBySpeed, y, Curve),
            PS_FIELD(sizeBySpeed, z, Curve),
            PS_FIELD(sizeBySpeed, range, Vector2),

            // rotation over lifetime / by speed (radians per second)
            PS_FIELD(rotationOverLifetime, enabled, Bool),
            PS_FIELD(rotationOverLifetime, separateAxes, Bool),
            radians(PS_FIELD(rotationOverLifetime, x, Curve)),
            radians(PS_FIELD(rotationOverLifetime, y, Curve)),
            radians(PS_FIELD(rotationOverLifetime, z, Curve)),
            PS_FIELD(rotationBySpeed, enabled, Bool),
            PS_FIELD(rotationBySpeed, separateAxes, Bool),
            radians(PS_FIELD(rotationBySpeed, x, Curve)),
            radians(PS_FIELD(rotationBySpeed, y, Curve)),
            radians(PS_FIELD(rotationBySpeed, z, Curve)),
            PS_FIELD(rotationBySpeed, range, Vector2),

            // noise
            PS_FIELD(noise, enabled, Bool),
            PS_FIELD(noise, separateAxes, Bool),
            PS_FIELD(noise, strength, Curve),
            PS_FIELD(noise, strengthY, Curve),
            PS_FIELD(noise, strengthZ, Curve),
            PS_FIELD(noise, frequency, Float),
            PS_FIELD(noise, scrollSpeed, Curve),
            PS_FIELD(noise, damping, Bool),
            PS_FIELD(noise, octaves, Int),
            PS_FIELD(noise, octaveMultiplier, Float),
            PS_FIELD(noise, octaveScale, Float),
            PS_FIELD(noise, positionAmount, Curve),
            radians(PS_FIELD(noise, rotationAmount, Curve)),
            PS_FIELD(noise, sizeAmount, Curve),

            // texture sheet animation
            PS_FIELD(textureSheetAnimation, enabled, Bool),
            PS_ENUM(textureSheetAnimation, mode, ANIMATION_MODE),
            PS_FIELD(textureSheetAnimation, numTilesX, Int),
            PS_FIELD(textureSheetAnimation, numTilesY, Int),
            PS_ENUM(textureSheetAnimation, animation, ANIMATION_TYPE),
            PS_ENUM(textureSheetAnimation, rowMode, ROW_MODE),
            PS_FIELD(textureSheetAnimation, rowIndex, Int),
            PS_ENUM(textureSheetAnimation, timeMode, TIME_MODE),
            PS_FIELD(textureSheetAnimation, frameOverTime, Curve),
            PS_FIELD(textureSheetAnimation, fps, Float),
            PS_FIELD(textureSheetAnimation, speedRange, Vector2),
            PS_FIELD(textureSheetAnimation, startFrame, Curve),
            PS_FIELD(textureSheetAnimation, cycles, Int),
            PS_FIELD(textureSheetAnimation, flipU, Float),
            PS_FIELD(textureSheetAnimation, flipV, Float),
            PS_FIELD(textureSheetAnimation, sprites, IntList),

            // renderer
            rebuild(PS_ENUM(renderer, renderMode, RENDER_MODE)),
            PS_FIELD(renderer, cameraVelocityScale, Float),
            PS_FIELD(renderer, velocityScale, Float),
            PS_FIELD(renderer, lengthScale, Float),
            PS_ENUM(renderer, alignment, RENDER_SPACE),
            PS_ENUM(renderer, sortMode, SORT_MODE),
            PS_FIELD(renderer, sortingFudge, Float),
            PS_FIELD(renderer, sortPriority, Int),
            PS_FIELD(renderer, minParticleSize, Float),
            PS_FIELD(renderer, maxParticleSize, Float),
            PS_FIELD(renderer, pivot, Vector3),
            PS_FIELD(renderer, flip, Vector3),
            rebuild(PS_FIELD(renderer, mesh, String)),
        };
    }

#undef PS_FIELD
#undef PS_ENUM

    const std::unordered_map<std::string, const FieldInfo*>& fieldIndex(){
        static const std::unordered_map<std::string, const FieldInfo*> index = []{
            std::unordered_map<std::string, const FieldInfo*> map;
            for (const FieldInfo& field : fields()) map[field.path()] = &field;
            return map;
        }();
        return index;
    }

}

const std::vector<std::string>& particles::moduleNames(){
    static const std::vector<std::string> names = {
        "main", "emission", "shape", "velocityOverLifetime", "limitVelocityOverLifetime", "inheritVelocity",
        "forceOverLifetime", "colorOverLifetime", "colorBySpeed", "sizeOverLifetime", "sizeBySpeed",
        "rotationOverLifetime", "rotationBySpeed", "noise", "textureSheetAnimation", "renderer"};
    return names;
}

const std::vector<FieldInfo>& particles::fields(){
    static const std::vector<FieldInfo> list = makeFields();
    return list;
}

const FieldInfo* particles::findField(const std::string& path){
    const auto& index = fieldIndex();
    auto it = index.find(path);
    return it == index.end() ? nullptr : it->second;
}

std::vector<const FieldInfo*> particles::moduleFields(const std::string& module){
    std::vector<const FieldInfo*> out;
    for (const FieldInfo& field : fields()){
        if (module == field.module) out.push_back(&field);
    }
    return out;
}

bool particles::moduleHasEnabled(const std::string& module){
    return module != "main" && module != "renderer";
}

std::string particles::enumName(const FieldInfo& field, int value){
    if (!field.enumNames || value < 0 || (size_t)value >= field.enumNames->size()) return "";
    return (*field.enumNames)[(size_t)value];
}

int particles::enumValue(const FieldInfo& field, const std::string& name){
    if (!field.enumNames) return -1;
    for (size_t i = 0; i < field.enumNames->size(); i++){
        if ((*field.enumNames)[i] == name) return (int)i;
    }
    return -1;
}

bool particles::isDefault(const FieldInfo& field, const ParticleSystemComponent& component){
    static ParticleSystemComponent defaults;
    const void* a = field.get(component);
    const void* b = field.get(defaults);
    switch (field.type){
        case FieldType::Bool: return *static_cast<const bool*>(a) == *static_cast<const bool*>(b);
        case FieldType::Int:
        case FieldType::Enum: return *static_cast<const int*>(a) == *static_cast<const int*>(b);
        case FieldType::UInt: return *static_cast<const unsigned int*>(a) == *static_cast<const unsigned int*>(b);
        case FieldType::Float: return *static_cast<const float*>(a) == *static_cast<const float*>(b);
        case FieldType::Vector2: return *static_cast<const Vector2*>(a) == *static_cast<const Vector2*>(b);
        case FieldType::Vector3: return *static_cast<const Vector3*>(a) == *static_cast<const Vector3*>(b);
        case FieldType::Curve: return *static_cast<const MinMaxCurve*>(a) == *static_cast<const MinMaxCurve*>(b);
        case FieldType::Gradient: return *static_cast<const MinMaxGradient*>(a) == *static_cast<const MinMaxGradient*>(b);
        case FieldType::Bursts: return *static_cast<const std::vector<ParticleEmissionBurst>*>(a) == *static_cast<const std::vector<ParticleEmissionBurst>*>(b);
        case FieldType::IntList: return *static_cast<const std::vector<int>*>(a) == *static_cast<const std::vector<int>*>(b);
        case FieldType::String: return *static_cast<const std::string*>(a) == *static_cast<const std::string*>(b);
    }
    return false;
}

void particles::copyField(const FieldInfo& field, const ParticleSystemComponent& from, ParticleSystemComponent& to){
    const void* a = field.get(from);
    void* b = field.get(to);
    switch (field.type){
        case FieldType::Bool: *static_cast<bool*>(b) = *static_cast<const bool*>(a); break;
        case FieldType::Int:
        case FieldType::Enum: *static_cast<int*>(b) = *static_cast<const int*>(a); break;
        case FieldType::UInt: *static_cast<unsigned int*>(b) = *static_cast<const unsigned int*>(a); break;
        case FieldType::Float: *static_cast<float*>(b) = *static_cast<const float*>(a); break;
        case FieldType::Vector2: *static_cast<Vector2*>(b) = *static_cast<const Vector2*>(a); break;
        case FieldType::Vector3: *static_cast<Vector3*>(b) = *static_cast<const Vector3*>(a); break;
        case FieldType::Curve: *static_cast<MinMaxCurve*>(b) = *static_cast<const MinMaxCurve*>(a); break;
        case FieldType::Gradient: *static_cast<MinMaxGradient*>(b) = *static_cast<const MinMaxGradient*>(a); break;
        case FieldType::Bursts: *static_cast<std::vector<ParticleEmissionBurst>*>(b) = *static_cast<const std::vector<ParticleEmissionBurst>*>(a); break;
        case FieldType::IntList: *static_cast<std::vector<int>*>(b) = *static_cast<const std::vector<int>*>(a); break;
        case FieldType::String: *static_cast<std::string*>(b) = *static_cast<const std::string*>(a); break;
    }
}
