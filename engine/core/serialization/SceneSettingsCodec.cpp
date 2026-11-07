// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneSettingsCodec.h"

#include "Scene.h"
#include "serialization/RuntimeFields.h"
#include "util/Fnv.h"

using namespace jasperengine;
using namespace jasperengine::cook;

namespace jasperengine::cook {

    template<>
    struct Codec<PostProcessPass> {
        static void write(Writer& w, const PostProcessPass& pass) {
            cook::write(w, pass.shader);
            w.boolean(pass.enabled);
            cook::write(w, pass.uniforms);
            cook::write(w, pass.textures);
            w.f32(pass.scale);
        }
        static void read(Reader& r, PostProcessPass& pass) {
            cook::read(r, pass.shader);
            pass.enabled = r.boolean();
            cook::read(r, pass.uniforms);
            cook::read(r, pass.textures);
            pass.scale = r.f32();
        }
    };

    // the engine's own post-processing table (PostProcessingSettings::getFields): one record
    // per field, {u32 FNV-1a 32 of its name, u8 type, u32 length, value}; a reader skips a field
    // it does not know
    template<>
    struct Codec<PostProcessingSettings> {
        using Type = PostProcessingSettings::FieldType;
        static void write(Writer& w, const PostProcessingSettings& settings) {
            for (const PostProcessingSettings::Field& field : PostProcessingSettings::getFields()) {
                const void* value = settings.getFieldRef(field);
                w.u32(fnv1a32(field.name));
                w.u8(static_cast<uint8_t>(field.type));
                const size_t at = w.beginLength();
                switch (field.type) {
                    case Type::Bool: w.boolean(*static_cast<const bool*>(value)); break;
                    case Type::Int: w.svarint(*static_cast<const int*>(value)); break;
                    case Type::Enum: w.svarint(*static_cast<const int*>(value)); break;
                    case Type::Float: w.f32(*static_cast<const float*>(value)); break;
                    case Type::Vector3: cook::write(w, *static_cast<const Vector3*>(value)); break;
                    case Type::String: w.string(*static_cast<const std::string*>(value)); break;
                    case Type::Asset: cook::write(w, *static_cast<const AssetRef*>(value)); break;
                }
                w.endLength(at);
            }
            w.u32(0);
        }
        static void read(Reader& r, PostProcessingSettings& settings) {
            const std::vector<PostProcessingSettings::Field>& fields = PostProcessingSettings::getFields();
            while (!r.failed()) {
                const uint32_t id = r.u32();
                if (id == 0) break;
                const Type type = static_cast<Type>(r.u8());
                Reader value = r.record();
                const PostProcessingSettings::Field* field = nullptr;
                for (const PostProcessingSettings::Field& candidate : fields)
                    if (fnv1a32(candidate.name) == id) field = &candidate;
                if (!field || field->type != type) continue;
                void* ref = field->ref(settings);
                switch (type) {
                    case Type::Bool: *static_cast<bool*>(ref) = value.boolean(); break;
                    case Type::Int: *static_cast<int*>(ref) = static_cast<int>(value.svarint()); break;
                    case Type::Enum: *static_cast<int*>(ref) = static_cast<int>(value.svarint()); break;
                    case Type::Float: *static_cast<float*>(ref) = value.f32(); break;
                    case Type::Vector3: cook::read(value, *static_cast<Vector3*>(ref)); break;
                    case Type::String: *static_cast<std::string*>(ref) = value.string(); break;
                    case Type::Asset: cook::read(value, *static_cast<AssetRef*>(ref)); break;
                }
            }
        }
    };

}

namespace {
    const ShaderType kDefaultShaderTypes[] = {ShaderType::MESH, ShaderType::UI, ShaderType::SKYBOX, ShaderType::POINTS, ShaderType::LINES};
}

void cook::writeSceneSettings(Writer& w, const Scene& scene) {
    write(w, scene.getBackgroundColor());
    write(w, scene.getShadowQuality());
    w.f32(scene.getShadowDistance());
    w.f32(scene.getShadowFadeFraction());
    w.boolean(scene.isShadowCascadeBlend());
    w.boolean(scene.isMeshLodEnabled());
    w.f32(scene.getMeshLodThreshold());
    w.boolean(scene.isDepthPrepassEnabled());
    write(w, scene.getSceneColorScale());
    w.f32(scene.getGlobalIlluminationIntensity());
    write(w, scene.getGlobalIlluminationColor());
    w.svarint(scene.getMaxLightsPerObject());
    write(w, scene.getLitQuality());
    w.f32(scene.getPhysicalLightEV100());
    w.f32(scene.getAmbientLight2DIntensity());
    write(w, scene.getAmbientLight2DColor());
    write(w, scene.getShadow2DQuality());
    write(w, scene.getGravity2D());
    write(w, scene.getGravity3D());
    w.boolean(scene.isPhysicsInterpolation());
    write(w, scene.getLightState());
    w.boolean(scene.isSSAOEnabled());
    w.f32(scene.getSSAORadius());
    w.f32(scene.getSSAOIntensity());
    w.f32(scene.getSSAOBias());
    w.boolean(scene.isSSREnabled());
    w.f32(scene.getSSRMaxDistance());
    w.f32(scene.getSSRThickness());
    w.svarint(scene.getSSRMaxSteps());
    w.f32(scene.getSSRIntensity());
    w.f32(scene.getSSRBlur());
    w.varint(scene.getFixedResolutionWidth());
    w.varint(scene.getFixedResolutionHeight());
    write(w, scene.getFixedResolutionFilter());
    w.boolean(scene.isFixedResolutionEnabled());
    write(w, scene.getEnableUIEvents());
    for (ShaderType type : kDefaultShaderTypes) write(w, scene.getDefaultShaderRef(type));
    write(w, scene.getPostProcessPasses());
    write(w, scene.getPostProcessing());
    // format 1.1 (S57): appended, so a 1.0 player skips them inside the record
    w.boolean(scene.isCapsuleShadowsEnabled());
    w.f32(scene.getCapsuleShadowIntensity());
    w.f32(scene.getCapsuleShadowSoftness());
    // format 1.2 (S54)
    w.boolean(scene.isMeshLodCrossFade());
    w.f32(scene.getMeshLodCrossFadeDuration());
    // format 1.3: the default shaders' property values
    for (ShaderType type : kDefaultShaderTypes) write(w, scene.getDefaultShaderUniforms(type));
}

void cook::readSceneSettings(Reader& r, Scene& scene) {
    Vector4 backgroundColor;
    read(r, backgroundColor);
    scene.setBackgroundColor(backgroundColor);
    ShadowQuality shadowQuality{};
    read(r, shadowQuality);
    scene.setShadowQuality(shadowQuality);
    scene.setShadowDistance(r.f32());
    scene.setShadowFadeFraction(r.f32());
    scene.setShadowCascadeBlend(r.boolean());
    scene.setMeshLodEnabled(r.boolean());
    scene.setMeshLodThreshold(r.f32());
    scene.setDepthPrepassEnabled(r.boolean());
    SceneColorScale colorScale{};
    read(r, colorScale);
    scene.setSceneColorScale(colorScale);
    scene.setGlobalIllumination(r.f32());
    Vector3 globalIllumination;
    read(r, globalIllumination);
    scene.setGlobalIllumination(globalIllumination);
    scene.setMaxLightsPerObject(static_cast<int>(r.svarint()));
    LitQuality litQuality{};
    read(r, litQuality);
    scene.setLitQuality(litQuality);
    scene.setPhysicalLightEV100(r.f32());
    scene.setAmbientLight2D(r.f32());
    Vector3 ambient2D;
    read(r, ambient2D);
    scene.setAmbientLight2D(ambient2D);
    ShadowQuality shadow2D{};
    read(r, shadow2D);
    scene.setShadow2DQuality(shadow2D);
    Vector2 gravity2D;
    read(r, gravity2D);
    scene.setGravity2D(gravity2D);
    Vector3 gravity3D;
    read(r, gravity3D);
    scene.setGravity3D(gravity3D);
    scene.setPhysicsInterpolation(r.boolean());
    LightState lightState{};
    read(r, lightState);
    scene.setLightState(lightState);
    scene.setSSAOEnabled(r.boolean());
    scene.setSSAORadius(r.f32());
    scene.setSSAOIntensity(r.f32());
    scene.setSSAOBias(r.f32());
    scene.setSSREnabled(r.boolean());
    scene.setSSRMaxDistance(r.f32());
    scene.setSSRThickness(r.f32());
    scene.setSSRMaxSteps(static_cast<int>(r.svarint()));
    scene.setSSRIntensity(r.f32());
    scene.setSSRBlur(r.f32());
    const unsigned int fixedWidth = static_cast<unsigned int>(r.varint());
    const unsigned int fixedHeight = static_cast<unsigned int>(r.varint());
    scene.setFixedResolutionSize(fixedWidth, fixedHeight);
    TextureFilter fixedFilter{};
    read(r, fixedFilter);
    scene.setFixedResolutionFilter(fixedFilter);
    scene.setFixedResolutionEnabled(r.boolean());
    UIEventState uiEvents{};
    read(r, uiEvents);
    scene.setEnableUIEvents(uiEvents);
    for (ShaderType type : kDefaultShaderTypes) {
        ShaderRef ref;
        read(r, ref);
        if (!ref.empty() || ref.isSet()) scene.setDefaultShaderRef(type, ref);
    }
    std::vector<PostProcessPass> passes;
    read(r, passes);
    if (!passes.empty()) scene.setPostProcessPasses(passes);
    PostProcessingSettings postProcessing;
    read(r, postProcessing);
    if (postProcessing != PostProcessingSettings()) scene.setPostProcessing(postProcessing);
    // a 1.0 record ends here: the capsule shadows keep their defaults
    if (r.atEnd()) return;
    scene.setCapsuleShadowsEnabled(r.boolean());
    scene.setCapsuleShadowIntensity(r.f32());
    scene.setCapsuleShadowSoftness(r.f32());
    // a 1.1 record ends here: the LOD cross-fade keeps its defaults (format 1.2)
    if (r.atEnd()) return;
    scene.setMeshLodCrossFade(r.boolean());
    scene.setMeshLodCrossFadeDuration(r.f32());
    // a 1.2 record ends here: no default shader property values (format 1.3)
    if (r.atEnd()) return;
    for (ShaderType type : kDefaultShaderTypes) {
        ShaderUniformValues values;
        read(r, values);
        if (!values.empty()) scene.setDefaultShaderUniforms(type, values);
    }
}
