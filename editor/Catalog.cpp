// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Catalog.h"
#include "component/AnimatorComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/RagdollComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/TextComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "component/TextEffectComponent.h"
#include "object/ParticleSystem.h"
#include "subsystem/ActionSystem.h"
#include "script/ScriptRuntime.h"

#include "Stream.h"
#include "reflect/Reflect.h"
#include "util/EntityClipboard.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectChecks.h"
#include "subsystem/RenderSystem.h"
#include "component/FluidSimulationComponent.h"
#include "component/SplineComponent.h"
#include "component/VideoPlayerComponent.h"
#include "external/IconsFontAwesome6.h"
#include "lattice/LatticeTypes.h"
#include "pose/PoseAnimatorRuntime.h"
#include "audio/AudioMixer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace {

    using namespace jasperengine;
    using namespace jasperengine::editor;

    bool endsWith(const std::string& text, const char* suffix) {
        const size_t length = std::strlen(suffix);
        return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
    }

    // a collider's authored data over another; the target's body is made again (its Box2D
    // shapes go with it), so no live handle or runtime copy travels
    void copyCollider2D(const Collider2D& source, Collider2D& target) {
        target = source;
        target.shapes.clear();
        target.chain = b2_nullChainId;
        target.dirty = true;
        target.materialInstance.reset();
        target.ownsMaterialInstance = false;
        target.decompositionKey = 0;
        target.pieces.clear();
    }

    void copyColliders2D(const std::vector<Collider2D>& source, std::vector<Collider2D>& target) {
        target.resize(source.size());
        for (size_t i = 0; i < source.size(); i++) {
            copyCollider2D(source[i], target[i]);
        }
    }

    // a 3D collider's authored data over another; the target's shape is made again, so no live
    // shape, surface or runtime material copy travels
    void copyCollider3D(const Collider3D& source, Collider3D& target) {
        target = source;
        target.shape = nullptr;
        target.surface = nullptr;
        target.dirty = true;
        target.materialInstance.reset();
        target.ownsMaterialInstance = false;
    }

    void copyColliders3D(const std::vector<Collider3D>& source, std::vector<Collider3D>& target) {
        target.resize(source.size());
        for (size_t i = 0; i < source.size(); i++) {
            copyCollider3D(source[i], target[i]);
        }
    }

    bool parseIndex(const std::string& text, size_t& pos, size_t& value) {
        if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) {
            return false;
        }

        // one spelling per index ("00" would alias "0" under another override name), and no
        // value that wraps around
        if (text[pos] == '0' && pos + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[pos + 1]))) {
            return false;
        }
        size_t parsedValue = 0;
        size_t digits = 0;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
            if (++digits > 9) {
                return false;
            }
            parsedValue = parsedValue * 10 + static_cast<size_t>(text[pos] - '0');
            pos++;
        }

        value = parsedValue;
        return true;
    }

    // Submesh properties the model loaders rewrite, and the bit that keeps an edit across a
    // reload. textureRect is absent on purpose: no loader touches it.
    struct SubmeshOverrideField {
        const char* name;
        uint64_t fields;
    };

    const SubmeshOverrideField submeshOverrideFields[] = {
        {"material", SubmeshOverride_Material},
        {"material.name", SubmeshOverride_MaterialName},
        {"material.baseColorFactor", SubmeshOverride_BaseColorFactor},
        {"material.metallicFactor", SubmeshOverride_MetallicFactor},
        {"material.roughnessFactor", SubmeshOverride_RoughnessFactor},
        {"material.alphaCutoff", SubmeshOverride_AlphaCutoff},
        {"material.foliageTransmission", SubmeshOverride_FoliageTransmission},
        {"material.alphaMode", SubmeshOverride_AlphaMode},
        {"material.emissiveFactor", SubmeshOverride_EmissiveFactor},
        {"material.baseColorTexture", SubmeshOverride_BaseColorTexture},
        {"material.emissiveTexture", SubmeshOverride_EmissiveTexture},
        {"material.metallicRoughnessTexture", SubmeshOverride_MetallicRoughnessTexture},
        {"material.occlusionTexture", SubmeshOverride_OcclusionTexture},
        {"material.normalTexture", SubmeshOverride_NormalTexture},
        {"material.shader", SubmeshOverride_Shader},
        {"material.shaderParams", SubmeshOverride_ShaderParams},
        {"material.shaderTextures", SubmeshOverride_ShaderTextures},
        {"material.shaderFeatures", SubmeshOverride_ShaderFeatures},
        {"material.shaderGlobalBindings", SubmeshOverride_ShaderGlobalBindings},
        {"material.blendMode", SubmeshOverride_BlendMode},
        {"material.cullMode", SubmeshOverride_CullMode},
        {"material.depthWrite", SubmeshOverride_DepthWrite},
        {"material.depthTest", SubmeshOverride_DepthTest},
        {"material.depthOffsetFactor", SubmeshOverride_DepthOffset},
        {"material.depthOffsetUnits", SubmeshOverride_DepthOffset},
        {"material.sortPriority", SubmeshOverride_SortPriority},
        {"material.lighting", SubmeshOverride_Lighting},
        {"material.stencil", SubmeshOverride_Stencil},
        {"material.colorWrite", SubmeshOverride_ColorWrite},
        {"material.extraPass", SubmeshOverride_ExtraPass},
        {"material.stochasticTiling", SubmeshOverride_StochasticTiling},
        {"material.stochasticCellSize", SubmeshOverride_StochasticTiling},
        {"material.stochasticBlendSharpness", SubmeshOverride_StochasticTiling},
        {"material.stochasticContrast", SubmeshOverride_StochasticTiling},
        {"material.stochasticRotation", SubmeshOverride_StochasticTiling},
        {"material.emissiveStrength", SubmeshOverride_EmissiveStrength},
        {"material.pbr", SubmeshOverride_PbrExtensions},
        {"faceCulling", SubmeshOverride_FaceCulling},
        {"textureShadow", SubmeshOverride_TextureShadow},
        {"primitiveType", SubmeshOverride_PrimitiveType}
    };

    // Splits "submeshes[N].<field>" into the submesh it addresses and the override bits it writes.
    bool parseSubmeshOverrideProperty(const std::string& propertyName, size_t& submeshIndex, uint64_t& fields) {
        if (propertyName.compare(0, 10, "submeshes[") != 0) {
            return false;
        }

        size_t pos = 10;
        if (!parseIndex(propertyName, pos, submeshIndex) || (pos + 1) >= propertyName.size() ||
                propertyName[pos] != ']' || propertyName[pos + 1] != '.') {
            return false;
        }

        const std::string field = propertyName.substr(pos + 2);
        for (const auto& entry : submeshOverrideFields) {
            if (field == entry.name) {
                fields = entry.fields;
                return true;
            }
        }
        // a field of the stencil or of the extra pass keeps the whole of it
        if (field.compare(0, 17, "material.stencil.") == 0) {
            fields = SubmeshOverride_Stencil;
            return true;
        }
        if (field.compare(0, 19, "material.extraPass.") == 0) {
            fields = SubmeshOverride_ExtraPass;
            return true;
        }
        // so does a field of the extended lobes (one bit for all of them)
        if (field.compare(0, 13, "material.pbr.") == 0) {
            fields = SubmeshOverride_PbrExtensions;
            return true;
        }

        return false;
    }

    // The properties of a component come from its reflection table (docs/specs/S08-reflection.md
    // R7, editor/reflect/Properties.cpp)
    PropertyData tableProperty(EntityRegistry* registry, Entity entity, ComponentType component, const std::string& propertyName) {
        const reflect::ComponentDesc* desc = reflect::find(component);
        if (!desc || !registry) {
            return PropertyData();
        }
        void* comp = desc->find(registry, entity);
        if (!comp) {
            return PropertyData();
        }
        return reflect::property(*desc, comp, propertyName);
    }

    bool tryEnumerateProperties(ComponentType component, void* compRef, std::map<std::string, PropertyData>& ps) {
        const reflect::ComponentDesc* desc = reflect::find(component);
        if (!desc) {
            return false;
        }
        reflect::listProperties(*desc, compRef, ps);
        return true;
    }

    bool tryEnumerateEntityProperties(EntityRegistry* registry, Entity entity, ComponentType component, std::map<std::string, PropertyData>& ps) {
        const reflect::ComponentDesc* desc = reflect::find(component);
        void* comp = desc ? desc->find(registry, entity) : nullptr;
        if (!comp) {
            return false;
        }
        reflect::listProperties(*desc, comp, ps);
        return true;
    }

}

using namespace jasperengine;

editor::Catalog::Catalog(){
}

std::string editor::Catalog::getComponentName(ComponentType component, bool removeSuffix){
    const reflect::ComponentDesc* desc = reflect::find(component);
    if (!desc) {
        return "";
    }
    return removeSuffix ? desc->key : desc->cppName;
}

std::string editor::Catalog::getComponentDisplayName(ComponentType component){
    const reflect::ComponentDesc* desc = reflect::find(component);
    return desc ? desc->displayName : "";
}

const char* editor::Catalog::getComponentIcon(ComponentType component){
    const reflect::ComponentDesc* desc = reflect::find(component);
    return desc ? desc->icon : ICON_FA_PUZZLE_PIECE;
}

ComponentType editor::Catalog::getConflictingComponent(ComponentType type){
    switch (type){
        case ComponentType::CharacterController2DComponent: return ComponentType::Rigidbody2DComponent;
        case ComponentType::Rigidbody2DComponent: return ComponentType::CharacterController2DComponent;
        case ComponentType::CharacterControllerComponent: return ComponentType::RigidbodyComponent;
        case ComponentType::RigidbodyComponent: return ComponentType::CharacterControllerComponent;
        default: return ComponentType::Transform;
    }
}

std::string editor::Catalog::getComponentConflict(EntityRegistry* registry, Entity entity, ComponentType type){
    const ComponentType other = getConflictingComponent(type);
    if (other == ComponentType::Transform || !registry || !registry->isEntityCreated(entity))
        return "";
    const std::vector<ComponentType> existing = findComponents(registry, entity);
    if (std::find(existing.begin(), existing.end(), other) == existing.end())
        return "";
    return "A " + getComponentDisplayName(type) + " cannot share its GameObject with a " + getComponentDisplayName(other) +
           " (remove the " + getComponentDisplayName(other) + " first).";
}

bool editor::Catalog::hasEnabledProperty(ComponentType component){
    const reflect::ComponentDesc* desc = reflect::find(component);
    return desc && desc->hasEnabled;
}

void editor::Catalog::markChanged(EntityRegistry* registry, Entity entity, ComponentType compType) {
    if (registry && registry->isEntityCreated(entity)) {
        registry->markChanged(getComponentId(registry, compType), entity);
    }
}

ComponentId editor::Catalog::getComponentId(const EntityRegistry* registry, ComponentType compType) {
    const reflect::ComponentDesc* desc = reflect::find(compType);
    return desc ? desc->componentId(registry) : 0;
}

editor::ComponentType editor::Catalog::getComponentType(const std::string& componentName) {
    // "light", "Light", "LightComponent", "lightcomponent"
    if (const reflect::ComponentDesc* desc = reflect::findByKey(componentName)) {
        return desc->type;
    }
    throw std::invalid_argument("Unknown component type: " + componentName);
}

Signature editor::Catalog::componentTypeToSignature(const EntityRegistry* registry, ComponentType compType) {
    Signature signature;
    ComponentId cid = getComponentId(registry, compType);
    signature.set(cid, true);
    return signature;
}

editor::PropertyType editor::Catalog::scriptPropertyTypeToPropertyType(ScriptPropertyType scriptType) {
    switch (scriptType) {
        case jasperengine::ScriptPropertyType::Bool: return editor::PropertyType::Bool;
        case jasperengine::ScriptPropertyType::Int: return editor::PropertyType::Int;
        case jasperengine::ScriptPropertyType::Float: return editor::PropertyType::Float;
        case jasperengine::ScriptPropertyType::String: return editor::PropertyType::String;
        case jasperengine::ScriptPropertyType::Vector2: return editor::PropertyType::Vector2;
        case jasperengine::ScriptPropertyType::Vector3: return editor::PropertyType::Vector3;
        case jasperengine::ScriptPropertyType::Vector4: return editor::PropertyType::Vector4;
        case jasperengine::ScriptPropertyType::Color3: return editor::PropertyType::Vector3;
        case jasperengine::ScriptPropertyType::Color4: return editor::PropertyType::Vector4;
        case jasperengine::ScriptPropertyType::EntityReference: return PropertyType::EntityReference;
        case jasperengine::ScriptPropertyType::Asset: return PropertyType::Asset;
        default: return editor::PropertyType::Custom;
    }
}

std::map<std::string, editor::PropertyData> editor::Catalog::getProperties(ComponentType component, void* compRef){
    std::map<std::string, editor::PropertyData> ps;
    tryEnumerateProperties(component, compRef, ps);
    return ps;
}

std::vector<editor::ComponentType> editor::Catalog::findComponents(EntityRegistry* registry, Entity entity){
    std::vector<editor::ComponentType> ret;
    for (const reflect::ComponentDesc* desc : reflect::listingOrder()) {
        if (desc->find(registry, entity)) {
            ret.push_back(desc->type);
        }
    }
    return ret;
}

bool editor::Catalog::mayHoldReferences(EntityRegistry* registry, Entity entity, ComponentType component, bool entities, bool cameraTextures){
    // The cases below rely on what those components enumerate. Checked once against their
    // property list, so a reference property added to one later turns the shortcut off
    // instead of being skipped.
    static const bool shortcutsHold = []() {
        // A line renderer lists its lines only with a component: one with a line
        LineRendererComponent lines;
        lines.lines.resize(1);
        for (ComponentType type : {ComponentType::Transform, ComponentType::SpriteComponent, ComponentType::MeshComponent,
                                   ComponentType::LineRendererComponent}) {
            for (const auto& [name, property] : getProperties(type, type == ComponentType::LineRendererComponent ? &lines : nullptr)) {
                if (property.type == PropertyType::Entity || property.type == PropertyType::EntityReference) {
                    return false;
                }
                if (property.type == PropertyType::Texture && (type != ComponentType::MeshComponent ||
                        (name != "submeshes[].material.baseColorTexture" && name != "submeshes[].material.emissiveTexture" &&
                         name != "submeshes[].material.metallicRoughnessTexture" && name != "submeshes[].material.occlusionTexture" &&
                         name != "submeshes[].material.normalTexture"))) {
                    return false;
                }
            }
        }
        return true;
    }();
    if (!shortcutsHold) {
        return entities || cameraTextures;
    }

    switch (component) {
        case ComponentType::Transform:
        case ComponentType::SpriteComponent:
        case ComponentType::LineRendererComponent:
            // Only values (the Transform's, the Sprite's; a line renderer's lines, its shader
            // textures being one Custom property, which no reference walk reads). The Scene view's
            // own line renderers (grid, gizmos) list thousands of lines: walking them was most of a
            // whole-scene CameraTextureLink::resolve
            return false;
        case ComponentType::MeshComponent: {
            // No entity properties; a texture of each submesh material (enumerateMeshProperties)
            if (!cameraTextures) {
                return false;
            }
            const MeshComponent* mesh = registry->findComponent<MeshComponent>(entity);
            if (!mesh) {
                return false;
            }
            for (size_t s = 0; s < mesh->submeshes.size(); s++) {
                const Material& material = mesh->submeshes[s].material;
                for (const Texture* texture : {&material.baseColorTexture, &material.emissiveTexture,
                        &material.metallicRoughnessTexture, &material.occlusionTexture, &material.normalTexture}) {
                    // Most are empty: no id to parse ("camera|<e>" or "fluid|<e>")
                    if (!texture->getId().empty() && CameraTextureLink::isLinkTexture(*texture)) {
                        return true;
                    }
                }
            }
            return false;
        }
        default:
            return entities || cameraTextures;
    }
}

std::map<std::string, editor::PropertyData> editor::Catalog::findEntityProperties(EntityRegistry* registry, Entity entity, ComponentType component){
    std::map<std::string, PropertyData> ps;
    tryEnumerateEntityProperties(registry, entity, component, ps);
    return ps;
}

uint64_t editor::Catalog::getChangedUpdateFlags(ComponentType compType, void* oldComp, void* newComp) {
    if (!oldComp || !newComp) return 0;
    const reflect::ComponentDesc* desc = reflect::find(compType);
    return desc ? reflect::changedUpdateFlags(*desc, oldComp, newComp) : 0;
}

uint64_t editor::Catalog::getComponentStructuralUpdateFlags(ComponentType compType) {
    const reflect::ComponentDesc* desc = reflect::find(compType);
    return desc ? desc->structuralUpdateFlags : UpdateFlags_None;
}

uint64_t* editor::Catalog::getSubmeshOverrideMask(EntityRegistry* registry, Entity entity, ComponentType component, const std::string& propertyName, uint64_t& propertyFields) {
    if (component != ComponentType::MeshComponent) {
        return nullptr;
    }

    size_t submeshIndex = 0;
    if (!parseSubmeshOverrideProperty(propertyName, submeshIndex, propertyFields)) {
        return nullptr;
    }

    MeshComponent* mesh = registry->findComponent<MeshComponent>(entity);
    if (!mesh || submeshIndex >= mesh->numSubmeshes) {
        return nullptr;
    }

    return &mesh->submeshes[submeshIndex].overrideFields;
}

std::vector<const CustomUniformBlock*> editor::Catalog::getShaderUniformBlocks(EntityRegistry* registry, Entity entity, ComponentType component) {
    std::vector<const CustomUniformBlock*> blocks;

    switch (component) {
        case ComponentType::MeshComponent:
            if (MeshComponent* mesh = registry->findComponent<MeshComponent>(entity)) {
                for (unsigned int i = 0; i < mesh->numSubmeshes; i++)
                    blocks.push_back(&mesh->submeshes[i].customVSParams);
                for (unsigned int i = 0; i < mesh->numSubmeshes; i++)
                    blocks.push_back(&mesh->submeshes[i].customFSParams);
                for (unsigned int i = 0; i < mesh->numSubmeshes; i++)
                    blocks.push_back(&mesh->submeshes[i].customVSDepthParams);
                for (unsigned int i = 0; i < mesh->numSubmeshes; i++)
                    blocks.push_back(&mesh->submeshes[i].customFSDepthParams);
            }
            break;
        case ComponentType::UIComponent:
            if (UIComponent* ui = registry->findComponent<UIComponent>(entity)) {
                blocks.push_back(&ui->customVSParams);
                blocks.push_back(&ui->customFSParams);
            }
            break;
        case ComponentType::PointsComponent:
            if (PointsComponent* points = registry->findComponent<PointsComponent>(entity)) {
                blocks.push_back(&points->customVSParams);
                blocks.push_back(&points->customFSParams);
            }
            break;
        case ComponentType::LineRendererComponent:
            if (LineRendererComponent* lines = registry->findComponent<LineRendererComponent>(entity)) {
                blocks.push_back(&lines->customVSParams);
                blocks.push_back(&lines->customFSParams);
            }
            break;
        case ComponentType::TrailRendererComponent:
            if (TrailRendererComponent* trail = registry->findComponent<TrailRendererComponent>(entity)) {
                blocks.push_back(&trail->customVSParams);
                blocks.push_back(&trail->customFSParams);
            }
            break;
        case ComponentType::SkyboxComponent:
            if (SkyboxComponent* sky = registry->findComponent<SkyboxComponent>(entity)) {
                blocks.push_back(&sky->customVSParams);
                blocks.push_back(&sky->customFSParams);
            }
            break;
        default:
            break;
    }

    return blocks;
}

std::vector<std::string> editor::Catalog::getShaderUniformWarnings(const std::vector<const CustomUniformBlock*>& blocks) {
    std::vector<std::string> warnings;

    for (size_t b = 0; b < blocks.size(); b++) {
        if (blocks[b]->mixedTypes) {
            warnings.push_back("A block mixes int and float members, which OpenGL uploads incorrectly. Keep one block to one scalar kind.");
        }
        for (const ShaderUniform& uniform : blocks[b]->members) {
            for (size_t other = 0; other < b; other++) {
                for (const ShaderUniform& existing : blocks[other]->members) {
                    if (existing.name == uniform.name && existing.type != uniform.type) {
                        warnings.push_back("'" + uniform.name + "' has different types in two blocks; one value feeds both.");
                    }
                }
            }
        }
    }

    return warnings;
}

bool editor::Catalog::isCustomShaderBuildFailed(Scene* scene, Entity entity, ComponentType component) {
    // a failed fork reloads on the built-in, which registers no custom id; a pending
    // reload still carries the previous id
    auto failed = [scene](const std::string& customShader, ShaderType shaderType, bool loaded, bool needReload, uint16_t customShaderId) {
        const std::string& effective = customShader.empty() ? scene->getDefaultCustomShader(shaderType) : customShader;
        return !effective.empty() && loaded && !needReload && customShaderId == 0;
    };

    switch (component) {
        case ComponentType::MeshComponent:
            if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
                // a submesh drawing with its material's, the mesh's or the scene's shader
                const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
                for (unsigned int s = 0; s < mesh->numSubmeshes; s++){
                    const std::string& shader = RenderSystem::getSubmeshShader(*mesh, mesh->submeshes[s], sceneMeshShader);
                    if (failed(shader, ShaderType::MESH, mesh->loaded, mesh->needReload, mesh->submeshes[s].customShaderId))
                        return true;
                }
            }
            break;
        case ComponentType::UIComponent:
            if (UIComponent* ui = scene->findComponent<UIComponent>(entity))
                return failed(ui->customShader, ShaderType::UI, ui->loaded, ui->needReload, ui->customShaderId);
            break;
        case ComponentType::PointsComponent:
            if (PointsComponent* points = scene->findComponent<PointsComponent>(entity))
                return failed(points->customShader, ShaderType::POINTS, points->loaded, points->needReload, points->customShaderId);
            break;
        case ComponentType::LineRendererComponent:
            if (LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(entity))
                return failed(lines->customShader, ShaderType::LINES, lines->loaded, lines->needReload, lines->customShaderId);
            break;
        case ComponentType::TrailRendererComponent:
            if (TrailRendererComponent* trail = scene->findComponent<TrailRendererComponent>(entity))
                return failed(trail->customShader, ShaderType::LINES, trail->loaded, trail->needReload, trail->customShaderId);
            break;
        case ComponentType::SkyboxComponent:
            if (SkyboxComponent* sky = scene->findComponent<SkyboxComponent>(entity))
                return failed(sky->customShader, ShaderType::SKYBOX, sky->loaded, sky->needReload, sky->customShaderId);
            break;
        default:
            break;
    }

    return false;
}

const char* editor::Catalog::getDefaultShaderUniformsProperty(ShaderType type){
    switch (type){
        case ShaderType::MESH:   return "default_mesh_shader_uniforms";
        case ShaderType::UI:     return "default_ui_shader_uniforms";
        case ShaderType::SKYBOX: return "default_sky_shader_uniforms";
        case ShaderType::POINTS: return "default_points_shader_uniforms";
        case ShaderType::LINES:  return "default_lines_shader_uniforms";
        default: return "";
    }
}

bool editor::Catalog::getDefaultShaderUniformsType(const std::string& propertyName, ShaderType& type){
    for (ShaderType candidate : {ShaderType::MESH, ShaderType::UI, ShaderType::SKYBOX, ShaderType::POINTS, ShaderType::LINES}){
        if (propertyName == getDefaultShaderUniformsProperty(candidate)){
            type = candidate;
            return true;
        }
    }
    return false;
}

editor::Catalog::UniformChainRefs editor::Catalog::getShaderUniformChain(Scene* scene, Entity entity, ComponentType component, int submesh){
    UniformChainRefs refs;
    ShaderType type;
    if (!scene || !ProjectChecks::shaderTypeForComponent(component, type))
        return refs;
    auto manifestOf = [](const std::string& shader) -> std::shared_ptr<const ShaderManifest> {
        std::shared_ptr<const ShaderManifest> manifest = shader.empty() ? nullptr : ShaderManifests::get(shader);
        return manifest && manifest->present ? manifest : nullptr;
    };
    if (component == ComponentType::MeshComponent && submesh >= 0){
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || submesh >= (int)mesh->numSubmeshes)
            return refs;
        Submesh& sub = mesh->submeshes[submesh];
        refs.chain.values = &sub.material.shaderParams;
        refs.chain.fallback = &mesh->shaderUniforms;
        refs.chain.sceneDefault = sub.material.shader.empty() ? scene->getInheritedShaderUniforms(type, mesh->customShader.path) : nullptr;
        refs.chain.bindings = sub.material.shaderGlobalBindings.empty() ? nullptr : &sub.material.shaderGlobalBindings;
        refs.manifest = manifestOf(RenderSystem::getSubmeshShader(*mesh, sub, scene->getDefaultCustomShader(type)));
    }else{
        const std::string* custom = getPropertyRef<std::string>(scene, entity, component, "customShader");
        refs.chain.values = getPropertyRef<ShaderUniformValues>(scene, entity, component, "shaderUniforms");
        if (!custom || !refs.chain.values)
            return refs;
        refs.chain.sceneDefault = scene->getInheritedShaderUniforms(type, *custom);
        // a mesh keeps its global bindings on its materials
        if (component != ComponentType::MeshComponent){
            const ShaderGlobalBindings* bindings = getPropertyRef<ShaderGlobalBindings>(scene, entity, component, "shaderGlobalBindings");
            refs.chain.bindings = bindings && !bindings->empty() ? bindings : nullptr;
        }
        refs.manifest = manifestOf(custom->empty() ? scene->getDefaultCustomShader(type) : *custom);
    }
    refs.chain.manifest = refs.manifest.get();
    return refs;
}

namespace {
    // the components of a type without a shader of their own, for getDefaultShaderUse
    template<typename T>
    void collectDefaultShaderUsers(Scene* scene, ComponentType component, editor::Catalog::DefaultShaderUse& use){
        auto components = scene->getComponentArray<T>();
        for (size_t i = 0; i < components->size(); i++){
            T& item = components->getComponentFromIndex(i);
            if (!item.customShader.empty())
                continue;
            const Entity entity = components->getEntity(i);
            use.users++;
            if (editor::Catalog::isCustomShaderBuildFailed(scene, entity, component))
                use.failed++;
            for (const CustomUniformBlock* block : editor::Catalog::getShaderUniformBlocks(scene, entity, component))
                use.blocks.push_back(block);
        }
    }
}

editor::Catalog::DefaultShaderUse editor::Catalog::getDefaultShaderUse(Scene* scene, ShaderType type){
    DefaultShaderUse use;
    if (!scene || scene->getDefaultCustomShader(type).empty())
        return use;
    switch (type){
        case ShaderType::MESH: {
            auto meshes = scene->getComponentArray<MeshComponent>();
            for (size_t i = 0; i < meshes->size(); i++){
                MeshComponent& mesh = meshes->getComponentFromIndex(i);
                if (!mesh.customShader.empty())
                    continue;
                // the submeshes whose material names no shader draw with the scene's
                bool uses = false;
                for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                    const Submesh& sub = mesh.submeshes[s];
                    if (!sub.material.shader.empty())
                        continue;
                    uses = true;
                    for (const CustomUniformBlock* block : {&sub.customVSParams, &sub.customFSParams, &sub.customVSDepthParams, &sub.customFSDepthParams})
                        use.blocks.push_back(block);
                }
                if (!uses)
                    continue;
                use.users++;
                if (isCustomShaderBuildFailed(scene, meshes->getEntity(i), ComponentType::MeshComponent))
                    use.failed++;
            }
            break;
        }
        case ShaderType::UI: collectDefaultShaderUsers<UIComponent>(scene, ComponentType::UIComponent, use); break;
        case ShaderType::SKYBOX: collectDefaultShaderUsers<SkyboxComponent>(scene, ComponentType::SkyboxComponent, use); break;
        case ShaderType::POINTS: collectDefaultShaderUsers<PointsComponent>(scene, ComponentType::PointsComponent, use); break;
        case ShaderType::LINES:
            collectDefaultShaderUsers<LineRendererComponent>(scene, ComponentType::LineRendererComponent, use);
            collectDefaultShaderUsers<TrailRendererComponent>(scene, ComponentType::TrailRendererComponent, use);
            break;
        default: break;
    }
    return use;
}

bool editor::Catalog::isCustomDepthShaderBuildFailed(Scene* scene, Entity entity) {
    // the depth fork has no scene default; loadMesh always builds it when set
    MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    return mesh && !mesh->customDepthShader.empty() && mesh->loaded && !mesh->needReload &&
           mesh->numSubmeshes > 0 && mesh->submeshes[0].customDepthShaderId == 0;
}

bool editor::Catalog::splitShaderEntryProperty(const std::string& propertyName, std::string& collection, std::string& key) {
    for (const char* name : {"shaderParams.", "shaderTextures.", "shaderUniforms.", "shaderGlobalBindings."}) {
        const size_t at = propertyName.find(name);
        if (at == std::string::npos) continue;
        const std::string prefix = propertyName.substr(0, at);
        // a submesh material's, or its extra pass's ("...material.extraPass.shaderParams.<member>")
        const bool material = prefix.compare(0, 10, "submeshes[") == 0 &&
                              (endsWith(prefix, "].material.") || endsWith(prefix, "].material.extraPass."));
        const bool component = prefix.empty();
        // a material has shaderParams, a component shaderUniforms; both have shaderTextures and
        // shaderGlobalBindings (an extra pass has none)
        const std::string field(name, std::strlen(name) - 1);
        if ((material && field == "shaderUniforms") || (component && field == "shaderParams") || (!material && !component) ||
                (field == "shaderGlobalBindings" && endsWith(prefix, "extraPass."))) {
            return false;
        }
        key = propertyName.substr(at + std::strlen(name));
        if (key.empty()) return false;
        collection = prefix + field;
        return true;
    }
    return false;
}

std::string editor::Catalog::getShaderReferenceForProperty(Scene* scene, Entity entity, ComponentType component, const std::string& propertyName) {
    if (!scene) return std::string();
    switch (component) {
        case ComponentType::MeshComponent: {
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!mesh) return std::string();
            size_t submesh = 0;
            size_t pos = 10;
            if (propertyName.compare(0, 10, "submeshes[") == 0 && parseIndex(propertyName, pos, submesh) && submesh < mesh->numSubmeshes) {
                // the extra pass draws with its own shader only
                if (propertyName.compare(pos, 21, "].material.extraPass.") == 0)
                    return mesh->submeshes[submesh].material.extraPass->shader;
                return RenderSystem::getSubmeshShader(*mesh, mesh->submeshes[submesh], scene->getDefaultCustomShader(ShaderType::MESH));
            }
            return mesh->customShader.empty() ? scene->getDefaultCustomShader(ShaderType::MESH) : mesh->customShader.path;
        }
        case ComponentType::UIComponent:
            if (UIComponent* ui = scene->findComponent<UIComponent>(entity))
                return ui->customShader.empty() ? scene->getDefaultCustomShader(ShaderType::UI) : ui->customShader.path;
            break;
        case ComponentType::PointsComponent:
            if (PointsComponent* points = scene->findComponent<PointsComponent>(entity))
                return points->customShader.empty() ? scene->getDefaultCustomShader(ShaderType::POINTS) : points->customShader.path;
            break;
        case ComponentType::LineRendererComponent:
            if (LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(entity))
                return lines->customShader.empty() ? scene->getDefaultCustomShader(ShaderType::LINES) : lines->customShader.path;
            break;
        case ComponentType::TrailRendererComponent:
            if (TrailRendererComponent* trail = scene->findComponent<TrailRendererComponent>(entity))
                return trail->customShader.empty() ? scene->getDefaultCustomShader(ShaderType::LINES) : trail->customShader.path;
            break;
        case ComponentType::SkyboxComponent:
            if (SkyboxComponent* sky = scene->findComponent<SkyboxComponent>(entity))
                return sky->customShader.empty() ? scene->getDefaultCustomShader(ShaderType::SKYBOX) : sky->customShader.path;
            break;
        default:
            break;
    }
    return std::string();
}

namespace {
    // the manifest the runtime resolves lighting against: none for a shader without directives
    const ShaderManifest* lightingManifest(const ShaderManifest* manifest) {
        return (manifest && manifest->present) ? manifest : nullptr;
    }
}

std::vector<MaterialLighting> editor::Catalog::getShaderLightingModes(const ShaderManifest* manifest) {
    manifest = lightingManifest(manifest);
    // RenderSystem decides: its default (what AUTO draws) first, then every other mode it accepts
    const MaterialLighting defaultMode = RenderSystem::resolveLighting(MaterialLighting::AUTO, manifest);
    std::vector<MaterialLighting> modes = {defaultMode};
    for (MaterialLighting mode : {MaterialLighting::PIXEL, MaterialLighting::VERTEX, MaterialLighting::MIXED, MaterialLighting::NONE}) {
        if (mode != defaultMode && RenderSystem::supportsLighting(mode, manifest)) modes.push_back(mode);
    }
    return modes;
}

bool editor::Catalog::shaderReadsMaterialNormalMap(const ShaderManifest* manifest, const std::vector<std::string>& features) {
    return manifest && manifest->present && (manifest->resolveRequire(features) & ShaderRequire_NormalMap) != 0;
}

bool editor::Catalog::supportsLighting(MaterialLighting mode, const ShaderManifest* manifest) {
    return RenderSystem::supportsLighting(mode, lightingManifest(manifest));
}

std::string editor::Catalog::getSceneInputsLabel(uint32_t inputs) {
    inputs &= ShaderRequire_SceneInputs;
    if (inputs == ShaderRequire_SceneInputs) return "depth and colour";
    if (inputs & ShaderRequire_SceneDepth) return "depth";
    if (inputs & ShaderRequire_SceneColor) return "colour";
    return std::string();
}

std::string editor::Catalog::getSceneInputsCost(uint32_t inputs, bool brief) {
    const bool depth = (inputs & ShaderRequire_SceneDepth) != 0;
    const bool color = (inputs & ShaderRequire_SceneColor) != 0;
    if (!depth && !color) return std::string();
    if (brief) {
        return std::string(depth && color ? "a depth pre-pass and a colour copy" : depth ? "a depth pre-pass" : "a colour copy") +
               " per camera";
    }
    std::string text = "Paid only by the camera passes where such a draw is visible (scene camera, render-to-texture and mirror cameras, editor views). ";
    if (depth) {
        text += "Scene depth: one more geometry pass of the opaque meshes per camera (none when SSAO, SSR or a depth-reading "
                "post-process pass already draws one), and every mesh keeps a depth program. ";
    }
    if (color) {
        text += "Scene colour: a copy of the opaque scene and the sky per camera, which splits its render pass (a store and load "
                "of the targets on tile-based mobile GPUs); its resolution is the scene setting Scene Color > Scale "
                "(scene_color_scale; half, the default, copies a quarter of the pixels). ";
    }
    text += "Avoid on low-end mobile. The draw must be in the transparent queue (a blend other than Opaque).";
    return text;
}

std::string editor::Catalog::getShaderSceneInputsSummary(const ShaderManifest& manifest) {
    const uint32_t possible = manifest.getPossibleSceneInputs();
    if (!possible) return std::string();
    // per input: always (@require), else the feature= rules that add it
    auto when = [&manifest](uint32_t bit) {
        if (manifest.require & bit) return std::string();
        std::vector<std::string> defines;
        for (const ShaderManifestRequireRule& rule : manifest.requireRules) {
            const std::string define = (rule.negate ? "!" : "") + rule.define;
            if ((rule.require & bit) && std::find(defines.begin(), defines.end(), define) == defines.end()) defines.push_back(define);
        }
        std::string text;
        for (const std::string& define : defines) text += (text.empty() ? "" : " or ") + define;
        return " (with " + text + ")";
    };
    const std::string depth = (possible & ShaderRequire_SceneDepth) ? when(ShaderRequire_SceneDepth) : std::string();
    const std::string color = (possible & ShaderRequire_SceneColor) ? when(ShaderRequire_SceneColor) : std::string();
    if (possible == ShaderRequire_SceneInputs) {
        if (depth == color) return "Reads the scene depth and colour" + depth;
        return "Reads the scene depth" + depth + " and colour" + color;
    }
    return (possible & ShaderRequire_SceneDepth) ? "Reads the scene depth" + depth : "Reads the scene colour" + color;
}

uint32_t editor::Catalog::getFeatureSceneInputs(const ShaderManifest& manifest, const std::string& define, bool whenOff) {
    uint32_t inputs = 0;
    for (const ShaderManifestRequireRule& rule : manifest.requireRules) {
        if (rule.define == define && rule.negate == whenOff) inputs |= rule.require;
    }
    return inputs & ShaderRequire_SceneInputs & ~manifest.require;
}

uint32_t editor::Catalog::getShaderSceneInputs(const std::string& shader, const std::vector<std::string>& shaderFeatures) {
    std::shared_ptr<const ShaderManifest> manifest = shader.empty() ? nullptr : ShaderManifests::get(shader);
    if (!manifest || !manifest->present) return 0;
    return manifest->resolveRequire(ShaderReference::getEnabledFeatures(shader, shaderFeatures)) & ShaderRequire_SceneInputs;
}

uint32_t editor::Catalog::getMaterialSceneInputs(Scene* scene, Entity entity, unsigned int submesh, bool extraPass) {
    const MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    if (!mesh || submesh >= mesh->numSubmeshes) return 0;
    const Submesh& sub = mesh->submeshes[submesh];
    if (extraPass) {
        const MaterialPass& pass = sub.material.extraPass;
        return getShaderSceneInputs(pass.shader, pass.shaderFeatures);
    }
    return getShaderSceneInputs(RenderSystem::getSubmeshShader(*mesh, sub, scene->getDefaultCustomShader(ShaderType::MESH)),
                                sub.material.shaderFeatures);
}

uint32_t editor::Catalog::getComponentSceneInputs(Scene* scene, Entity entity, ComponentType component) {
    if (!scene) return 0;
    if (component == ComponentType::MeshComponent) {
        // the mesh's shader (its customShader, else the scene's) has no features of its own: the
        // submeshes drawing with it pick its variants through their materials
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh) return 0;
        const std::string& shader = mesh->customShader.empty() ? scene->getDefaultCustomShader(ShaderType::MESH) : mesh->customShader.path;
        uint32_t inputs = 0;
        for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
            if (mesh->submeshes[s].material.shader.empty()) inputs |= getShaderSceneInputs(shader, mesh->submeshes[s].material.shaderFeatures);
        }
        return inputs;
    }
    const std::vector<std::string>* features = getPropertyRef<std::vector<std::string>>(scene, entity, component, "shaderFeatures");
    if (!features) return 0;
    return getShaderSceneInputs(getShaderReferenceForProperty(scene, entity, component, "customShader"), *features);
}

uint32_t editor::Catalog::getSceneInputsInUse(Scene* scene) {
    auto render = scene ? scene->getSystem<RenderSystem>() : nullptr;
    if (!render) return 0;
    const uint32_t usage = render->getSceneInputUsage();
    return ((usage & 1u) ? ShaderRequire_SceneDepth : 0u) | ((usage & 2u) ? ShaderRequire_SceneColor : 0u);
}

editor::Catalog::SceneInputQueueFix editor::Catalog::getSceneInputQueueFix(Scene* scene, Entity entity, unsigned int submesh) {
    const MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    if (!mesh || submesh >= mesh->numSubmeshes) return SceneInputQueueFix::NONE;
    const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
    bool opaqueQueue = false;
    if (!RenderSystem::getSubmeshSceneInputs(*mesh, mesh->submeshes[submesh], sceneMeshShader, opaqueQueue) || !opaqueQueue)
        return SceneInputQueueFix::NONE;
    // without auto transparency the mesh's own Transparent decides the queue: no blend moves it
    if (!mesh->autoTransparency) return SceneInputQueueFix::MESH_TRANSPARENT;
    // only when Alpha does move it (a fork's @state queue= decides before the blend)
    Submesh fixed = mesh->submeshes[submesh];
    fixed.material.blendMode = MaterialBlendMode::ALPHA;
    RenderSystem::getSubmeshSceneInputs(*mesh, fixed, sceneMeshShader, opaqueQueue);
    return opaqueQueue ? SceneInputQueueFix::NONE : SceneInputQueueFix::ALPHA_BLEND;
}

std::string editor::Catalog::getSceneInputQueueFixProperty(SceneInputQueueFix fix, unsigned int submesh) {
    switch (fix) {
        case SceneInputQueueFix::ALPHA_BLEND: return "submeshes[" + std::to_string(submesh) + "].material.blendMode";
        case SceneInputQueueFix::MESH_TRANSPARENT: return "transparent";
        default: return "";
    }
}

std::string editor::Catalog::getSceneInputQueueAdvice(SceneInputQueueFix fix, unsigned int submesh) {
    switch (fix) {
        case SceneInputQueueFix::ALPHA_BLEND:
            return "set a blend mode other than opaque (submeshes[" + std::to_string(submesh) + "].material.blendMode)";
        case SceneInputQueueFix::MESH_TRANSPARENT:
            return "the mesh's Auto Transparency is off and its Transparent off, which keeps it opaque whatever the blend: "
                   "turn the mesh's Transparent on (transparent)";
        default:
            return "its shader's state draws it in the opaque queue whatever the blend: use a shader whose @state is "
                   "transparent, or turn the mesh's Auto Transparency off and its Transparent on";
    }
}

uint64_t editor::Catalog::getMaterialUpdateFlags(const Material& from, const Material& to) {
    uint64_t flags = UpdateFlags_Mesh_Texture;
    // resolved when the mesh loads: the shader, its variant and its pipelines
    if (from.shader != to.shader || from.shaderFeatures != to.shaderFeatures || from.shaderTextures != to.shaderTextures ||
            from.blendMode != to.blendMode || from.cullMode != to.cullMode || from.depthWrite != to.depthWrite ||
            from.depthTest != to.depthTest || from.depthOffsetFactor != to.depthOffsetFactor ||
            from.depthOffsetUnits != to.depthOffsetUnits || from.sortPriority != to.sortPriority ||
            from.lighting != to.lighting || from.alphaMode != to.alphaMode || from.stochasticTiling != to.stochasticTiling) {
        flags |= UpdateFlags_Mesh_Reload;
    }
    // stencil and colour write are pipeline state too
    if (from.stencil != to.stencil || from.colorWrite != to.colorWrite) {
        flags |= UpdateFlags_Mesh_Reload;
    }
    // the extra pass: all of it but its parameters and texture files, as Mesh::setMaterial
    MaterialPass fromPass = from.extraPass;
    MaterialPass toPass = to.extraPass;
    fromPass.shaderParams.clear();
    toPass.shaderParams.clear();
    for (auto& texture : fromPass.shaderTextures) texture.second = Texture();
    for (auto& texture : toPass.shaderTextures) texture.second = Texture();
    if (fromPass != toPass) {
        flags |= UpdateFlags_Mesh_Reload;
    }
    if (from.shader != to.shader || from.shaderParams != to.shaderParams || from.extraPass->shaderParams != to.extraPass->shaderParams ||
            from.extraPass->shader != to.extraPass->shader) {
        flags |= UpdateFlags_Shader_Uniforms;
    }
    return flags;
}

void editor::Catalog::updateEntity(EntityRegistry* registry, Entity entity, uint64_t updateFlags){
    if (updateFlags & UpdateFlags_CanvasGroup){
        // alpha in [0, 1] at once (a NaN keeps the value UISystem last saw)
        if (CanvasGroupComponent* group = registry->findComponent<CanvasGroupComponent>(entity)){
            if (std::isnan(group->alpha)){
                group->alpha = group->seen.valid ? group->seen.alpha : 1.0f;
            }else{
                group->alpha = std::clamp(group->alpha, 0.0f, 1.0f);
            }
        }
    }
    if (updateFlags & UpdateFlags_Script_Enabled){
        // in Play the Inspector checkbox, MCP and undo enable scripts as Lua's self.enabled does
        if (Scene* scene = dynamic_cast<Scene*>(registry)){
            ScriptRuntime::refreshEntity(scene, entity);
        }
    }
    if (updateFlags & UpdateFlags_Transform){
        if (Transform* transform = registry->findComponent<Transform>(entity)){
            transform->needUpdate = true;
        }
    }
    if (updateFlags & UpdateFlags_Camera){
        if (CameraComponent* camera = registry->findComponent<CameraComponent>(entity)){
            camera->needUpdate = true;
        }
    }
    if (updateFlags & UpdateFlags_Scene_Mesh_Reload){
        auto meshes = registry->getComponentArray<MeshComponent>();
        for (int i = 0; i < meshes->size(); i++) {
            MeshComponent& mesh = meshes->getComponentFromIndex(i);
            mesh.needReload = true;
        }
    }else if (updateFlags & UpdateFlags_Scene_Mesh_Variant){
        // as RenderSystem::needReloadMeshVariants (the registry may be a prefab's, without one)
        auto meshes = registry->getComponentArray<MeshComponent>();
        for (int i = 0; i < meshes->size(); i++) {
            MeshComponent& mesh = meshes->getComponentFromIndex(i);
            if (mesh.loaded && !mesh.needReload)
                mesh.needVariantReload = true;
            else
                mesh.needReload = true;
        }
    }
    if (updateFlags & (UpdateFlags_Mesh_Reload | UpdateFlags_Mesh_Texture)){
        if (MeshComponent* mesh = registry->findComponent<MeshComponent>(entity)){
            if (updateFlags & UpdateFlags_Mesh_Reload)
                mesh->needReload = true;
            if (updateFlags & UpdateFlags_Mesh_Texture){
                for (unsigned int i = 0; i < mesh->numSubmeshes; i++)
                    mesh->submeshes[i].needUpdateTexture = true;
            }
        }
        if (updateFlags & UpdateFlags_Mesh_Reload){
            if (MeshPolygonComponent* meshPolygon = registry->findComponent<MeshPolygonComponent>(entity)){
                meshPolygon->needUpdatePolygon = true;
            }
        }
    }
    if (updateFlags & (UpdateFlags_LightShadowMap | UpdateFlags_LightShadowCamera)){
        if (LightComponent* light = registry->findComponent<LightComponent>(entity)){
            if (updateFlags & UpdateFlags_LightShadowMap)
                light->needUpdateShadowMap = true;
            if (updateFlags & UpdateFlags_LightShadowCamera)
                light->needUpdateShadowCamera = true;
        }
    }
    if (updateFlags & (UpdateFlags_UI_Reload | UpdateFlags_UI_Texture)){
        if (UIComponent* ui = registry->findComponent<UIComponent>(entity)){
            if (updateFlags & UpdateFlags_UI_Reload)
                ui->needReload = true;
            if (updateFlags & UpdateFlags_UI_Texture)
                ui->needUpdateTexture = true;
        }
        if (PolygonComponent* polygon = registry->findComponent<PolygonComponent>(entity)){
            if (updateFlags & UpdateFlags_UI_Reload)
                polygon->needUpdatePolygon = true;
        }
    }
    if (updateFlags & UpdateFlags_Image_Patches){
        if (ImageComponent* image = registry->findComponent<ImageComponent>(entity)){
            image->needUpdatePatches = true;
        }
    }
    if (updateFlags & (UpdateFlags_Layout_Sizes | UpdateFlags_Layout_Anchors)){
        // May be requested even if not have RectTransformComponent
        if (RectTransformComponent* layout = registry->findComponent<RectTransformComponent>(entity)){
            if (updateFlags & UpdateFlags_Layout_Sizes)
                layout->needUpdateSizes = true;
            if ((updateFlags & UpdateFlags_Layout_Anchors) && layout->usingAnchors)
                layout->needUpdateAnchorOffsets = true;
        }
    }
    if (updateFlags & (UpdateFlags_Sky | UpdateFlags_Sky_Texture)){
        if (SkyboxComponent* sky = registry->findComponent<SkyboxComponent>(entity)){
            if (updateFlags & UpdateFlags_Sky_Texture)
                sky->needUpdateTexture = true;
            if (updateFlags & UpdateFlags_Sky)
                sky->needUpdateSky = true;
        }
    }
    if (updateFlags & UpdateFlags_Reflection_Probe_Capture){
        if (ReflectionProbeComponent* probe = registry->findComponent<ReflectionProbeComponent>(entity)){
            probe->needUpdate = true;
            probe->captureRevision++;
        }
    }
    if (updateFlags & UpdateFlags_Reflection_Probe_Recapture_All){
        auto probes = registry->getComponentArray<ReflectionProbeComponent>();
        for (int i = 0; i < probes->size(); i++) {
            ReflectionProbeComponent& probe = probes->getComponentFromIndex(i);
            probe.needUpdate = true;
            probe.captureRevision++;
        }
    }
    if (updateFlags & UpdateFlags_Shader_Reload){
        // Forked-shader assignment/reset: reload whichever renderable the entity has
        // so it re-fetches its (possibly custom) shader from the pool.
        if (MeshComponent* m = registry->findComponent<MeshComponent>(entity)) m->needReload = true;
        if (UIComponent* u = registry->findComponent<UIComponent>(entity)) u->needReload = true;
        if (PointsComponent* p = registry->findComponent<PointsComponent>(entity)) p->needReload = true;
        if (LineRendererComponent* l = registry->findComponent<LineRendererComponent>(entity)) l->needReload = true;
        if (TrailRendererComponent* t = registry->findComponent<TrailRendererComponent>(entity)) t->needReload = true;
        if (SkyboxComponent* s = registry->findComponent<SkyboxComponent>(entity)) s->needReload = true;
    }
    if (updateFlags & UpdateFlags_Shader_Uniforms){
        if (MeshComponent* m = registry->findComponent<MeshComponent>(entity)) m->needUpdateShaderUniforms = true;
        if (UIComponent* u = registry->findComponent<UIComponent>(entity)) u->needUpdateShaderUniforms = true;
        if (PointsComponent* p = registry->findComponent<PointsComponent>(entity)) p->needUpdateShaderUniforms = true;
        if (LineRendererComponent* l = registry->findComponent<LineRendererComponent>(entity)) l->needUpdateShaderUniforms = true;
        if (TrailRendererComponent* t = registry->findComponent<TrailRendererComponent>(entity)) t->needUpdateShaderUniforms = true;
        if (SkyboxComponent* s = registry->findComponent<SkyboxComponent>(entity)) s->needUpdateShaderUniforms = true;
    }
    if (updateFlags & UpdateFlags_Sprite){
        if (SpriteComponent* sprite = registry->findComponent<SpriteComponent>(entity)){
            sprite->needUpdateSprite = true;
        }
        if (ActionComponent* action = registry->findComponent<ActionComponent>(entity)){
            if (action->target != NULL_ENTITY){
                if (SpriteComponent* targetSprite = registry->findComponent<SpriteComponent>(action->target)){
                    targetSprite->needUpdateSprite = true;
                }
            }
        }
    }
    if (updateFlags & UpdateFlags_TextEffect){
        if (TextEffectComponent* effect = registry->findComponent<TextEffectComponent>(entity)){
            effect->needApply = true;
            // a type changed: an amplitude left at the old type's default takes the new one's
            // (Grow and Unfold use TMP's Amplitude, 1 = the whole change)
            for (size_t i = 0; i < effect->layers.size(); i++){
                TextEffectLayer& layer = effect->layers[i];
                if (layer.type != effect->editedTypes[i]){
                    const TextEffectType type = layer.type;
                    layer.type = effect->editedTypes[i];
                    TextEffects::setType(layer, type);
                    effect->editedTypes[i] = type;
                }
            }
        }
    }
    if (updateFlags & (UpdateFlags_Text | UpdateFlags_Text_Atlas)){
        if (TextComponent* text = registry->findComponent<TextComponent>(entity)){
            if (updateFlags & UpdateFlags_Text)
                text->needUpdateText = true;
            if (updateFlags & UpdateFlags_Text_Atlas)
                text->needReloadAtlas = true;
        }
    }
    if (updateFlags & UpdateFlags_Rigidbody2D){
        if (Rigidbody2DComponent* body = registry->findComponent<Rigidbody2DComponent>(entity)){
            body->needReloadBody = true;
            body->needUpdateColliders = true;
        }
    }
    if (updateFlags & UpdateFlags_Rigidbody){
        if (RigidbodyComponent* body = registry->findComponent<RigidbodyComponent>(entity)){
            // a live body swaps its shape (it keeps its id, velocities and joints, D13); one not
            // made yet is made
            if (body->body.IsInvalid()){
                body->needReloadBody = true;
            }
            for (Collider3D& collider : body->colliders){
                collider.dirty = true;
            }
            body->needUpdateColliders = true;
            body->appliedMaterialRevision = 0;
        }
    }
    if (updateFlags & UpdateFlags_Rigidbody_Params){
        if (RigidbodyComponent* body = registry->findComponent<RigidbodyComponent>(entity)){
            body->needApplyParameters = true;
            body->massDirty = true;
            body->appliedMaterialRevision = 0;
        }
    }
    if (updateFlags & UpdateFlags_Rigidbody2D_Params){
        if (Rigidbody2DComponent* body = registry->findComponent<Rigidbody2DComponent>(entity)){
            body->needApplyParameters = true;
        }
    }
    if (updateFlags & UpdateFlags_Joint2D){
        if (Joint2DComponent* joint = registry->findComponent<Joint2DComponent>(entity)){
            joint->needUpdateJoint = true;
        }
    }
    if (updateFlags & UpdateFlags_Joint2D_Params){
        if (Joint2DComponent* joint = registry->findComponent<Joint2DComponent>(entity)){
            joint->needApplyParameters = true;
            // a joint broken by its threshold can break again
            joint->breakArmed = true;
        }
    }
    if (updateFlags & UpdateFlags_Joint3D){
        if (Joint3DComponent* joint = registry->findComponent<Joint3DComponent>(entity)){
            joint->needUpdateJoint = true;
        }
    }
    if (updateFlags & UpdateFlags_Joint3D_Params){
        if (Joint3DComponent* joint = registry->findComponent<Joint3DComponent>(entity)){
            joint->needApplyParameters = true;
            // a joint broken by its threshold can break again
            joint->breakArmed = true;
        }
    }
    if (updateFlags & UpdateFlags_Ragdoll){
        if (RagdollComponent* ragdoll = registry->findComponent<RagdollComponent>(entity)){
            ragdoll->needCollectParts = true;
        }
    }
    if (updateFlags & UpdateFlags_Model){
        if (ModelComponent* model = registry->findComponent<ModelComponent>(entity)){
            model->needUpdateModel = true;
        }
    }
    if (updateFlags & UpdateFlags_Tilemap){
        if (TilemapComponent* tilemap = registry->findComponent<TilemapComponent>(entity)){
            tilemap->needUpdateTilemap = true;
        }
    }
    if (updateFlags & (UpdateFlags_Terrain | UpdateFlags_Terrain_Texture | UpdateFlags_Terrain_Foliage)){
        if (TerrainComponent* terrain = registry->findComponent<TerrainComponent>(entity)){
            if (updateFlags & UpdateFlags_Terrain)
                terrain->needUpdateTerrain = true;
            if (updateFlags & UpdateFlags_Terrain_Texture)
                terrain->needUpdateTexture = true;
            if (updateFlags & UpdateFlags_Terrain_Foliage)
                terrain->needUpdateFoliage = true;
        }
    }
    if (updateFlags & UpdateFlags_Instanced_Mesh){
        if (InstancedMeshComponent* instmesh = registry->findComponent<InstancedMeshComponent>(entity)){
            instmesh->needUpdateInstances = true;
        }
    }
    if (updateFlags & (UpdateFlags_Points | UpdateFlags_Points_Texture)){
        if (PointsComponent* pts = registry->findComponent<PointsComponent>(entity)){
            if (updateFlags & UpdateFlags_Points)
                pts->needUpdate = true;
            if (updateFlags & UpdateFlags_Points_Texture)
                pts->needUpdateTexture = true;
        }
    }
    if (updateFlags & (UpdateFlags_Lines | UpdateFlags_Lines_Reload)){
        if (LineRendererComponent* lines = registry->findComponent<LineRendererComponent>(entity)){
            // clamped as the Lua setters do; the geometry (ribbon, polyline segments) is built
            // again by the render system, which reloads when the variant or capacity changed
            lines->widthMultiplier = (std::isfinite(lines->widthMultiplier) && lines->widthMultiplier > 0.0f) ? lines->widthMultiplier : 0.0f;
            lines->numCornerVertices = std::min(lines->numCornerVertices, LINE_RIBBON_MAX_FAN_VERTICES);
            lines->numCapVertices = std::min(lines->numCapVertices, LINE_RIBBON_MAX_FAN_VERTICES);
            lines->widthCurve.normalize();
            lines->colorGradient.normalize();
            lines->needUpdateRibbon = true;
            bool needsLargerBuffer = lines->lines.size() > lines->maxLines;
            if (needsLargerBuffer){
                lines->maxLines = static_cast<unsigned int>(lines->lines.size());
            }

            if ((updateFlags & UpdateFlags_Lines_Reload) || needsLargerBuffer){
                lines->needReload = true;
            }else{
                lines->needUpdateBuffer = true;
            }
        }
    }
    if (updateFlags & UpdateFlags_Trail){
        if (TrailRendererComponent* trail = registry->findComponent<TrailRendererComponent>(entity)){
            auto nonNegative = [](float value){ return (std::isfinite(value) && value > 0.0f) ? value : 0.0f; };
            trail->time = nonNegative(trail->time);
            trail->minVertexDistance = nonNegative(trail->minVertexDistance);
            trail->widthMultiplier = nonNegative(trail->widthMultiplier);
            trail->numCornerVertices = std::min(trail->numCornerVertices, LINE_RIBBON_MAX_FAN_VERTICES);
            trail->numCapVertices = std::min(trail->numCapVertices, LINE_RIBBON_MAX_FAN_VERTICES);
            trail->widthCurve.normalize();
            trail->colorGradient.normalize();
            trail->needUpdateRibbon = true;
        }
    }
    if (updateFlags & UpdateFlags_AudioSource){
        if (AudioSourceComponent* audio = registry->findComponent<AudioSourceComponent>(entity)){
            audio->needUpdate = true;
            audio->priority = std::clamp(audio->priority, 0, 256);
            audio->maxVoices = std::clamp(audio->maxVoices, 1, 64);
            // the group is set by name: its id follows (a name not in the mixer keeps the last id,
            // the runtime then finds the group by name, else plays through Master)
            if (audio->outputGroupName.empty()) {
                audio->outputGroup.clear();
            } else {
                const std::string id = AudioMixer::findGroupId(audio->outputGroupName);
                if (!id.empty()) audio->outputGroup = id;
            }
            audio->groupWarned = false;
        }
    }
    if (updateFlags & UpdateFlags_Fluid_Reset){
        if (FluidSimulationComponent* fluid = registry->findComponent<FluidSimulationComponent>(entity)){
            fluid->needReset = true;
        }
    }
    if (updateFlags & UpdateFlags_UIDocument_Reload){
        if (UIDocumentComponent* document = registry->findComponent<UIDocumentComponent>(entity)){
            // filename, render mode, texture size, pixel ratio (sortOrder is re-applied each update)
            document->textureWidth = std::clamp(document->textureWidth, 1, 8192);
            document->textureHeight = std::clamp(document->textureHeight, 1, 8192);
            if (!(document->pixelRatio >= 0.0f)) document->pixelRatio = 0.0f;
            document->needReload = true;
        }
    }
    if (updateFlags & UpdateFlags_Particles){
        if (ParticleSystemComponent* particles = registry->findComponent<ParticleSystemComponent>(entity)){
            particles->needReload = true;
        }
    }
    if (updateFlags & UpdateFlags_Spline){
        // a knot, the type, closed or the up axis was written in place: a new version (radii
        // kept non-negative, the up axis a unit one, as the setters keep them)
        if (SplineComponent* spline = registry->findComponent<SplineComponent>(entity)){
            for (spline::SplineKnot& knot : spline->spline.editableKnots()){
                knot.radius = std::isfinite(knot.radius) ? std::max(0.0f, knot.radius) : 0.0f;
            }
            Vector3& up = spline->spline.editableUpAxis();
            const float length = up.length();
            up = (std::isfinite(length) && length > 0.0f) ? up / length : Vector3(0.0f, 1.0f, 0.0f);
            spline->spline.markDirty();
        }
    }
    if (updateFlags & UpdateFlags_Lattice){
        // offsets are left as they are: LatticeEditorOps::setResolution writes them resampled
        // with the resolution, and a prefab override applies them before it
        if (LatticeComponent* lattice = registry->findComponent<LatticeComponent>(entity)){
            lattice->resolutionX = std::clamp(lattice->resolutionX, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION);
            lattice->resolutionY = std::clamp(lattice->resolutionY, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION);
            lattice->resolutionZ = std::clamp(lattice->resolutionZ, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION);
            lattice->revision++;
        }
        if (LatticeModifierComponent* modifier = registry->findComponent<LatticeModifierComponent>(entity)){
            modifier->subdivisions = std::clamp(modifier->subdivisions, 1, 64);
            for (LatticeBinding& binding : modifier->bindings){
                binding.weight = std::isfinite(binding.weight) ? std::clamp(binding.weight, 0.0f, 2.0f) : 1.0f;
            }
            modifier->warnedOverflow = false;
        }
    }
    if (updateFlags & UpdateFlags_PoseAnimator_Reload){
        if (PoseAnimatorComponent* animator = registry->findComponent<PoseAnimatorComponent>(entity)){
            animator->weight = std::clamp(animator->weight, 0.0f, 1.0f);
            // the same path keeps what is loaded (a weight change); a new one loads, and so does
            // the same one again after it failed (the file may be fixed now)
            if (!animator->runtime || animator->runtime->path != animator->animation || animator->runtime->failed){
                animator->needsLoad = true;
            }
        }
    }
    if (updateFlags & UpdateFlags_Animator_Reload){
        // a new controller, override, IK constraint or written-bones setting: loaded and bound
        // again by the next update, its states kept
        if (AnimatorComponent* animator = registry->findComponent<AnimatorComponent>(entity)){
            animator->needsLoad = true;
        }
    }
    if (updateFlags & UpdateFlags_AnimationPlayer_Reload){
        // the clip list changed (an entry added, removed, retargeted, renamed or rewrapped): the
        // player loads it again and rebinds, keeping its states and rest values
        if (AnimationPlayerComponent* player = registry->findComponent<AnimationPlayerComponent>(entity)){
            player->needsLoad = true;
        }
    }
    if (updateFlags & UpdateFlags_VertexAnimator_Reload){
        // a new .vat (or the same one again: a fixed file) loads at the next update; a new one
        // starts over (VertexAnimatorSystem compares the path)
        if (VertexAnimatorComponent* animator = registry->findComponent<VertexAnimatorComponent>(entity)){
            animator->defaultFadeTime = std::isfinite(animator->defaultFadeTime) ? std::max(0.0f, animator->defaultFadeTime) : 0.0f;
            animator->needsLoad = true;
        }
    }
}

void editor::Catalog::copyComponent(EntityRegistry* sourceRegistry, Entity sourceEntity,
                                   EntityRegistry* targetRegistry, Entity targetEntity,
                                   ComponentType compType) {
    const reflect::ComponentDesc* desc = reflect::find(compType);
    void* source = desc ? desc->find(sourceRegistry, sourceEntity) : nullptr;
    void* target = desc ? desc->find(targetRegistry, targetEntity) : nullptr;
    if (!source || !target) {
        printf("WARNING: Unsupported component type for copying: %s\n", getComponentName(compType).c_str());
        return;
    }

    switch (compType) {
        // Written over the live component, so a live instance keeps its runtime state (an
        // instanced mesh keeps its vertex layout, a body its physics handle)
        case ComponentType::PointsComponent:
        case ComponentType::ActionComponent:
        case ComponentType::AnimationComponent:
        case ComponentType::SpriteAnimationComponent:
        case ComponentType::Rigidbody2DComponent:
        case ComponentType::RigidbodyComponent:
        case ComponentType::InstancedMeshComponent:
            EntityClipboard::applyComponentValue(targetRegistry, targetEntity, compType,
                EntityClipboard::encodeComponent(sourceRegistry, sourceEntity, compType));
            return;
        default:
            break;
    }

    // The source's YAML decoded into a new component, as the scene file would load it; over the
    // target's own for a UI document (its loaded document stays unless the file changes), a fluid
    // (its simulator, and every texture showing its output, stays) and a lattice (its jiggle and
    // tween state stay), a Pose Animator (what it plays stays unless the animation changes) and an
    // Animation Player (what it plays stays, a changed clip list reloads)
    const YAML::Node encoded = reflect::encode(*desc, source, reflect::EncodeContext{});
    const bool overTarget = compType == ComponentType::UIDocumentComponent || compType == ComponentType::FluidSimulationComponent ||
                            compType == ComponentType::LatticeComponent || compType == ComponentType::PoseAnimatorComponent ||
                            compType == ComponentType::AnimationPlayerComponent || compType == ComponentType::AnimatorComponent;
    reflect::Value copy(desc->body.ops, overTarget ? target : nullptr);
    reflect::DecodeContext ctx;
    ctx.old = overTarget ? target : nullptr;
    ctx.component = desc->cppName;
    reflect::DecodeReport report;
    reflect::decode(*desc, encoded, copy.get(), ctx, report);
    if (compType == ComponentType::Transform) {
        // the target keeps its place in the hierarchy (same parent: no re-ordering)
        static_cast<Transform*>(copy.get())->parent = static_cast<Transform*>(target)->parent;
    }
    desc->body.ops->move(target, copy.get());
    if (compType == ComponentType::LatticeComponent) {
        static_cast<LatticeComponent*>(target)->revision++;
    }
}

void editor::Catalog::copyPropertyValue(EntityRegistry* sourceRegistry, Entity sourceEntity, 
                                       EntityRegistry* targetRegistry, Entity targetEntity, 
                                       ComponentType compType, const std::string& property) {

    // Get the property data to determine the type: the enumerated names, else the lookup (names
    // it resolves but does not list, such as a 2D collider's "colliders[i].points[j]")
    auto sourceProperties = Catalog::findEntityProperties(sourceRegistry, sourceEntity, compType);
    auto propIt = sourceProperties.find(property);
    PropertyType propType;
    uint64_t updateFlags;
    if (propIt != sourceProperties.end()) {
        propType = propIt->second.type;
        updateFlags = propIt->second.updateFlags;
    } else {
        const PropertyData found = Catalog::findProperty(sourceRegistry, sourceEntity, compType, property);
        if (!found.ref || !Catalog::findProperty(targetRegistry, targetEntity, compType, property).ref) {
            return; // Property not found
        }
        propType = found.type;
        updateFlags = found.updateFlags;
    }

    // Copy based on property type
    switch (propType) {
        case PropertyType::Bool: {
            bool* source = Catalog::getPropertyRef<bool>(sourceRegistry, sourceEntity, compType, property);
            bool* target = Catalog::getPropertyRef<bool>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Float: {
            float* source = Catalog::getPropertyRef<float>(sourceRegistry, sourceEntity, compType, property);
            float* target = Catalog::getPropertyRef<float>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Double: {
            double* source = Catalog::getPropertyRef<double>(sourceRegistry, sourceEntity, compType, property);
            double* target = Catalog::getPropertyRef<double>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Int: {
            int* source = Catalog::getPropertyRef<int>(sourceRegistry, sourceEntity, compType, property);
            int* target = Catalog::getPropertyRef<int>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::UInt: {
            unsigned int* source = Catalog::getPropertyRef<unsigned int>(sourceRegistry, sourceEntity, compType, property);
            unsigned int* target = Catalog::getPropertyRef<unsigned int>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Vector2: {
            Vector2* source = Catalog::getPropertyRef<Vector2>(sourceRegistry, sourceEntity, compType, property);
            Vector2* target = Catalog::getPropertyRef<Vector2>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Vector3: {
            Vector3* source = Catalog::getPropertyRef<Vector3>(sourceRegistry, sourceEntity, compType, property);
            Vector3* target = Catalog::getPropertyRef<Vector3>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Vector4: {
            Vector4* source = Catalog::getPropertyRef<Vector4>(sourceRegistry, sourceEntity, compType, property);
            Vector4* target = Catalog::getPropertyRef<Vector4>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Quat: {
            Quaternion* source = Catalog::getPropertyRef<Quaternion>(sourceRegistry, sourceEntity, compType, property);
            Quaternion* target = Catalog::getPropertyRef<Quaternion>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::String: {
            std::string* source = Catalog::getPropertyRef<std::string>(sourceRegistry, sourceEntity, compType, property);
            std::string* target = Catalog::getPropertyRef<std::string>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Asset: {
            AssetRef* source = Catalog::getPropertyRef<AssetRef>(sourceRegistry, sourceEntity, compType, property);
            AssetRef* target = Catalog::getPropertyRef<AssetRef>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Shader: {
            ShaderRef* source = Catalog::getPropertyRef<ShaderRef>(sourceRegistry, sourceEntity, compType, property);
            ShaderRef* target = Catalog::getPropertyRef<ShaderRef>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Curve: {
            Curve* source = Catalog::getPropertyRef<Curve>(sourceRegistry, sourceEntity, compType, property);
            Curve* target = Catalog::getPropertyRef<Curve>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Font: {
            FontArray* source = Catalog::getPropertyRef<FontArray>(sourceRegistry, sourceEntity, compType, property);
            FontArray* target = Catalog::getPropertyRef<FontArray>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Material: {
            Material* source = Catalog::getPropertyRef<Material>(sourceRegistry, sourceEntity, compType, property);
            Material* target = Catalog::getPropertyRef<Material>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Texture: {
            Texture* source = Catalog::getPropertyRef<Texture>(sourceRegistry, sourceEntity, compType, property);
            Texture* target = Catalog::getPropertyRef<Texture>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Enum: {
            int* source = Catalog::getPropertyRef<int>(sourceRegistry, sourceEntity, compType, property);
            int* target = Catalog::getPropertyRef<int>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Ease: {
            Ease* source = Catalog::getPropertyRef<Ease>(sourceRegistry, sourceEntity, compType, property);
            Ease* target = Catalog::getPropertyRef<Ease>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Entity: {
            Entity* source = Catalog::getPropertyRef<Entity>(sourceRegistry, sourceEntity, compType, property);
            Entity* target = Catalog::getPropertyRef<Entity>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::EntityReference: {
            EntityReference* source = Catalog::getPropertyRef<EntityReference>(sourceRegistry, sourceEntity, compType, property);
            EntityReference* target = Catalog::getPropertyRef<EntityReference>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Gradient: {
            Gradient* source = Catalog::getPropertyRef<Gradient>(sourceRegistry, sourceEntity, compType, property);
            Gradient* target = Catalog::getPropertyRef<Gradient>(targetRegistry, targetEntity, compType, property);
            if (source && target) *target = *source;
            break;
        }
        case PropertyType::Custom:
            // upstream's collider lists: their authored data, no live shape (copyCollider2D / 3D)
            if (compType == ComponentType::RigidbodyComponent && property == "colliders") {
                auto* source = Catalog::getPropertyRef<std::vector<Collider3D>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<Collider3D>>(targetRegistry, targetEntity, compType, property);
                if (source && target) copyColliders3D(*source, *target);
            } else if (compType == ComponentType::RigidbodyComponent && property.compare(0, 10, "colliders[") == 0 && endsWith(property, "]")) {
                auto* source = Catalog::getPropertyRef<Collider3D>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<Collider3D>(targetRegistry, targetEntity, compType, property);
                if (source && target) copyCollider3D(*source, *target);
            } else if (compType == ComponentType::RigidbodyComponent && property.compare(0, 10, "colliders[") == 0 && endsWith(property, "].vertices")) {
                auto* source = Catalog::getPropertyRef<std::vector<Vector3>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<Vector3>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::RigidbodyComponent && property.compare(0, 10, "colliders[") == 0 && endsWith(property, "].indices")) {
                auto* source = Catalog::getPropertyRef<std::vector<uint32_t>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<uint32_t>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::Rigidbody2DComponent && property == "colliders") {
                auto* source = Catalog::getPropertyRef<std::vector<Collider2D>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<Collider2D>>(targetRegistry, targetEntity, compType, property);
                if (source && target) copyColliders2D(*source, *target);
            } else if (compType == ComponentType::Rigidbody2DComponent && property.compare(0, 10, "colliders[") == 0 && endsWith(property, "]")) {
                auto* source = Catalog::getPropertyRef<Collider2D>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<Collider2D>(targetRegistry, targetEntity, compType, property);
                if (source && target) copyCollider2D(*source, *target);
            } else if (compType == ComponentType::Rigidbody2DComponent && property.compare(0, 10, "colliders[") == 0 && endsWith(property, "].points")) {
                auto* source = Catalog::getPropertyRef<std::vector<Vector2>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<Vector2>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::ScriptComponent) {
                copyComponent(sourceRegistry, sourceEntity, targetRegistry, targetEntity, compType);
            } else if (compType == ComponentType::TerrainComponent && property == "foliageLayers") {
                auto* source = Catalog::getPropertyRef<std::vector<TerrainFoliageLayer>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<TerrainFoliageLayer>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::TerrainComponent && property == "surfaceLayers") {
                auto* source = Catalog::getPropertyRef<std::vector<TerrainSurfaceLayer>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<TerrainSurfaceLayer>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::TerrainComponent && property == "blendMaps") {
                auto* source = Catalog::getPropertyRef<std::vector<Texture>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<Texture>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::LatticeComponent && property == "offsets") {
                auto* source = Catalog::getPropertyRef<std::vector<Vector3>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<Vector3>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::LatticeComponent && property == "poses") {
                auto* source = Catalog::getPropertyRef<std::vector<LatticePose>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<LatticePose>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::LatticeModifierComponent && property == "bindings") {
                auto* source = Catalog::getPropertyRef<std::vector<LatticeBinding>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<LatticeBinding>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::LatticeTracksComponent && property == "points") {
                auto* source = Catalog::getPropertyRef<std::vector<int>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<int>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::LatticeTracksComponent &&
                       (property == "values" || property == "inTangents" || property == "outTangents")) {
                auto* source = Catalog::getPropertyRef<std::vector<std::vector<Vector3>>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<std::vector<Vector3>>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::LineRendererComponent && property == "positions") {
                auto* source = Catalog::getPropertyRef<std::vector<Vector3>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<Vector3>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::LineRendererComponent && property == "lines") {
                auto* source = Catalog::getPropertyRef<std::vector<LineData>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<LineData>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::LatticeTracksComponent && property.compare(0, 7, "values[") == 0) {
                auto* source = Catalog::getPropertyRef<std::vector<Vector3>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<Vector3>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (endsWith(property, "material.extraPass")) {
                auto* source = Catalog::getPropertyRef<MaterialPass>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<MaterialPass>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (property == "stencil" || endsWith(property, ".stencil")) {
                auto* source = Catalog::getPropertyRef<MaterialStencil>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<MaterialStencil>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (property == "shaderUniforms" || endsWith(property, "material.shaderParams") || endsWith(property, "extraPass.shaderParams")) {
                auto* source = Catalog::getPropertyRef<ShaderUniformValues>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<ShaderUniformValues>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (property == "shaderTextures" || endsWith(property, "material.shaderTextures") || endsWith(property, "extraPass.shaderTextures")) {
                auto* source = Catalog::getPropertyRef<std::vector<std::pair<std::string, Texture>>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<std::pair<std::string, Texture>>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (property == "shaderGlobalBindings" || endsWith(property, "material.shaderGlobalBindings")) {
                auto* source = Catalog::getPropertyRef<ShaderGlobalBindings>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<ShaderGlobalBindings>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (property == "shaderFeatures" || endsWith(property, "material.shaderFeatures") || endsWith(property, "extraPass.shaderFeatures")) {
                auto* source = Catalog::getPropertyRef<std::vector<std::string>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<std::string>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::ParticleSystemComponent && property == "inlineEffect") {
                auto* source = Catalog::getPropertyRef<particles::ParticleEffectDocument>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<particles::ParticleEffectDocument>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::ParticleSystemComponent && property == "exposedValues") {
                auto* source = Catalog::getPropertyRef<std::vector<ParticleExposedValue>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<ParticleExposedValue>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::ParticleSystemComponent && property == "collisionPlanes") {
                auto* source = Catalog::getPropertyRef<std::vector<Entity>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<Entity>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::ForceFieldComponent && property == "falloff") {
                auto* source = Catalog::getPropertyRef<Curve>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<Curve>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (const reflect::ComponentDesc* desc = reflect::find(compType)) {
                // any other value its table describes (Buoyancy customPoints...): copied by its
                // type's ops, so a new field needs no branch here for prefab Apply/Revert
                void* sourceComp = desc->find(sourceRegistry, sourceEntity);
                void* targetComp = desc->find(targetRegistry, targetEntity);
                const reflect::FieldRef source = sourceComp ? reflect::resolve(*desc, sourceComp, property) : reflect::FieldRef{};
                const reflect::FieldRef target = targetComp ? reflect::resolve(*desc, targetComp, property) : reflect::FieldRef{};
                if (source && target && source.ops && source.ops == target.ops && source.ops->copy) source.ops->copy(target.ref, source.ref);
            }
            break;
        default:
            // For any unknown/unsupported type, do nothing
            break;
    }

    // The override bit travels with the value, or the target keeps it only until its model loads.
    uint64_t propertyFields = 0;
    if (uint64_t* source = getSubmeshOverrideMask(sourceRegistry, sourceEntity, compType, property, propertyFields)) {
        if (uint64_t* target = getSubmeshOverrideMask(targetRegistry, targetEntity, compType, property, propertyFields)) {
            *target = (*target & ~propertyFields) | (*source & propertyFields);
        }
    }

    // Apply any update flags that are associated with this property (a looked-up name's too:
    // propIt is end() for those)
    updateEntity(targetRegistry, targetEntity, updateFlags);
}

editor::PropertyData editor::Catalog::findProperty(EntityRegistry* registry, Entity entity, ComponentType component, std::string propertyName){
    return tableProperty(registry, entity, component, propertyName);
}

editor::PropertyData editor::Catalog::getProperty(EntityRegistry* registry, Entity entity, ComponentType component, std::string propertyName){
    PropertyData fastProperty = findProperty(registry, entity, component, propertyName);
    if (fastProperty.ref || fastProperty.def){
        return fastProperty;
    }

    printf("ERROR: Cannot find property %s\n", propertyName.c_str());
    return PropertyData();
}
