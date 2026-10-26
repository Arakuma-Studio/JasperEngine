// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Catalog.h"

#include "Stream.h"
#include "util/EntityClipboard.h"
#include "util/CameraTextureLink.h"
#include "subsystem/RenderSystem.h"
#include "component/FluidSimulationComponent.h"
#include "external/IconsFontAwesome6.h"
#include "lattice/LatticeTypes.h"
#include "pose/PoseAnimatorRuntime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace {

    using namespace jasperengine;
    using namespace jasperengine::editor;

    using FastDefGetter = void* (*)();
    using FastRefGetter = void* (*)(void*);

    struct FastPropertyDescriptor {
        const char* name;
        PropertyType type;
        uint64_t updateFlags;
        FastDefGetter getDef;
        FastRefGetter getRef;
    };

    using FastResolverFn = PropertyData (*)(void*, const std::string&);
    using FastEnumerateFn = void (*)(void*, std::map<std::string, PropertyData>&);
    using FastFindComponentFn = void* (*)(EntityRegistry*, Entity);

    struct FastComponentResolver {
        ComponentType component;
        FastFindComponentFn findComponent;
        FastResolverFn resolve;
        FastEnumerateFn enumerate;
    };

    template<typename Component>
    Component& getDefaultComponent() {
        static Component component{};
        return component;
    }

    template<typename Component, typename Member, Member Component::*MemberPtr>
    void* getMemberDef() {
        return &(getDefaultComponent<Component>().*MemberPtr);
    }

    template<typename Component, typename Member, Member Component::*MemberPtr>
    void* getMemberRef(void* comp) {
        return &(static_cast<Component*>(comp)->*MemberPtr);
    }

    template<typename Component, typename Member, Member Component::*MemberPtr>
    constexpr FastPropertyDescriptor makeFastProperty(const char* name, PropertyType type, uint64_t updateFlags) {
        return {name, type, updateFlags, &getMemberDef<Component, Member, MemberPtr>, &getMemberRef<Component, Member, MemberPtr>};
    }

    template<typename Component, typename Member, Member Component::*MemberPtr>
    constexpr FastPropertyDescriptor makeFastPropertyNoDefault(const char* name, PropertyType type, uint64_t updateFlags) {
        return {name, type, updateFlags, nullptr, &getMemberRef<Component, Member, MemberPtr>};
    }

    template<typename Component, size_t N>
    PropertyData resolveDirectProperties(Component* comp, const std::string& propertyName, const FastPropertyDescriptor (&descriptors)[N]) {
        if (!comp) {
            return PropertyData();
        }

        for (const FastPropertyDescriptor& descriptor : descriptors) {
            if (propertyName == descriptor.name) {
                return {
                    descriptor.type,
                    descriptor.updateFlags,
                    descriptor.getDef ? descriptor.getDef() : nullptr,
                    descriptor.getRef ? descriptor.getRef(comp) : nullptr
                };
            }
        }

        return PropertyData();
    }

    template<size_t N>
    void enumerateFromDescriptors(void* comp, std::map<std::string, PropertyData>& ps,
                                  const FastPropertyDescriptor (&descriptors)[N]) {
        for (const auto& desc : descriptors) {
            ps[desc.name] = {
                desc.type,
                desc.updateFlags,
                desc.getDef ? desc.getDef() : nullptr,
                (comp && desc.getRef) ? desc.getRef(comp) : nullptr
            };
        }
    }

    template<typename Component>
    void* findComponentPtr(EntityRegistry* registry, Entity entity) {
        return registry->findComponent<Component>(entity);
    }

    bool endsWith(const std::string& text, const char* suffix) {
        const size_t length = std::strlen(suffix);
        return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
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

    // A field of a material's extended lobes and hex tiling ("pbr.<lobe>.<field>",
    // MaterialPbr's table; the value is made on access, as the extra pass). An enabled flag, a
    // map or its UV set picks the built-in Lit's variant, built while the loaded one draws
    // (RenderSystem, from the texture update); the factors are uniforms.
    PropertyData getMaterialPbrProperty(Material& material, Material* def, const std::string& path, bool hasRef) {
        const PbrField* field = MaterialPbr::findField(path);
        if (!field) return PropertyData();
        PropertyType type = PropertyType::Float;
        uint64_t flags = UpdateFlags_None;
        switch (field->type) {
            case PbrFieldType::Bool: type = PropertyType::Bool; flags = UpdateFlags_Mesh_Texture; break;
            case PbrFieldType::Float: type = PropertyType::Float; break;
            case PbrFieldType::Vector2: type = PropertyType::Vector2; break;
            case PbrFieldType::Color: type = PropertyType::Vector3; break;
            case PbrFieldType::Texture: type = PropertyType::Texture; flags = UpdateFlags_Mesh_Texture; break;
            case PbrFieldType::TexCoord: type = PropertyType::Int; flags = UpdateFlags_Mesh_Texture; break;
        }
        return {type, flags, def ? field->ref(def->pbr.edit()) : nullptr, hasRef ? field->ref(material.pbr.edit()) : nullptr};
    }

    // The shader library fields of a submesh material ("submeshes[N].material.<field>"). The
    // shader, its features, textures and render state are resolved when the mesh loads, so a
    // change reloads it; parameters only rewrite the uniform blocks.
    const char* const kMaterialLibraryFields[] = {
        "shader", "shaderParams", "shaderTextures", "shaderFeatures", "blendMode", "cullMode", "depthWrite",
        "depthTest", "depthOffsetFactor", "depthOffsetUnits", "sortPriority", "lighting",
        // phase 2
        "stencil", "stencil.compare", "stencil.ref", "stencil.readMask", "stencil.writeMask", "stencil.pass",
        "stencil.fail", "stencil.depthFail", "colorWrite", "extraPass", "extraPass.shader", "extraPass.shaderParams",
        "extraPass.shaderTextures", "extraPass.shaderFeatures", "extraPass.blendMode", "extraPass.cullMode",
        "extraPass.depthWrite", "extraPass.depthTest", "extraPass.depthOffsetFactor", "extraPass.depthOffsetUnits",
        "extraPass.stencil", "extraPass.stencil.compare", "extraPass.stencil.ref", "extraPass.stencil.readMask",
        "extraPass.stencil.writeMask", "extraPass.stencil.pass", "extraPass.stencil.fail", "extraPass.stencil.depthFail",
        "extraPass.colorWrite", "extraPass.order"
    };

    // "stencil", "stencil.<field>" and "colorWrite" of a material, an extra pass or a
    // UI/Points/LineRenderer component: pipeline state, so a change reloads with `flags`
    PropertyData getStencilColorWriteProperty(MaterialStencil* stencil, MaterialStencil* defStencil, int* colorWrite, int* defColorWrite,
                                              const std::string& field, uint64_t flags) {
        auto data = [&](PropertyType type, auto member) -> PropertyData {
            return {type, flags, defStencil ? (void*)&(defStencil->*member) : nullptr, stencil ? (void*)&(stencil->*member) : nullptr};
        };
        if (field == "colorWrite") return {PropertyType::Int, flags, (void*)defColorWrite, (void*)colorWrite};
        if (field == "stencil") return {PropertyType::Custom, flags, (void*)defStencil, (void*)stencil};
        if (field.compare(0, 8, "stencil.") != 0) return PropertyData();
        const std::string name = field.substr(8);
        if (name == "compare") return data(PropertyType::Enum, &MaterialStencil::compare);
        if (name == "ref") return data(PropertyType::Int, &MaterialStencil::ref);
        if (name == "readMask") return data(PropertyType::Int, &MaterialStencil::readMask);
        if (name == "writeMask") return data(PropertyType::Int, &MaterialStencil::writeMask);
        if (name == "pass") return data(PropertyType::Enum, &MaterialStencil::pass);
        if (name == "fail") return data(PropertyType::Enum, &MaterialStencil::fail);
        if (name == "depthFail") return data(PropertyType::Enum, &MaterialStencil::depthFail);
        return PropertyData();
    }

    const char* const kComponentStencilFields[] = {
        "stencil", "stencil.compare", "stencil.ref", "stencil.readMask", "stencil.writeMask", "stencil.pass",
        "stencil.fail", "stencil.depthFail", "colorWrite"
    };

    // The fields of a material's extra pass ("extraPass.<field>"): all of it is resolved when
    // the mesh loads but its parameters (rewritten into its blocks) and texture files (rebound)
    PropertyData getMaterialPassProperty(MaterialPass* pass, MaterialPass* def, const std::string& field) {
        auto data = [&](PropertyType type, uint64_t flags, auto member) -> PropertyData {
            return {type, flags, def ? (void*)&(def->*member) : nullptr, pass ? (void*)&(pass->*member) : nullptr};
        };
        if (field == "shader") return data(PropertyType::Shader, UpdateFlags_Mesh_Reload | UpdateFlags_Shader_Uniforms, &MaterialPass::shader);
        if (field == "shaderParams") return data(PropertyType::Custom, UpdateFlags_Shader_Uniforms, &MaterialPass::shaderParams);
        if (field == "shaderTextures") return data(PropertyType::Custom, UpdateFlags_Mesh_Reload | UpdateFlags_Mesh_Texture, &MaterialPass::shaderTextures);
        if (field == "shaderFeatures") return data(PropertyType::Custom, UpdateFlags_Mesh_Reload, &MaterialPass::shaderFeatures);
        if (field == "blendMode") return data(PropertyType::Enum, UpdateFlags_Mesh_Reload, &MaterialPass::blendMode);
        if (field == "cullMode") return data(PropertyType::Enum, UpdateFlags_Mesh_Reload, &MaterialPass::cullMode);
        if (field == "depthWrite") return data(PropertyType::Enum, UpdateFlags_Mesh_Reload, &MaterialPass::depthWrite);
        if (field == "depthTest") return data(PropertyType::Enum, UpdateFlags_Mesh_Reload, &MaterialPass::depthTest);
        if (field == "depthOffsetFactor") return data(PropertyType::Float, UpdateFlags_Mesh_Reload, &MaterialPass::depthOffsetFactor);
        if (field == "depthOffsetUnits") return data(PropertyType::Float, UpdateFlags_Mesh_Reload, &MaterialPass::depthOffsetUnits);
        if (field == "order") return data(PropertyType::Enum, UpdateFlags_Mesh_Reload, &MaterialPass::order);
        return getStencilColorWriteProperty(pass ? &pass->stencil : nullptr, def ? &def->stencil : nullptr,
                                            pass ? &pass->colorWrite : nullptr, def ? &def->colorWrite : nullptr, field, UpdateFlags_Mesh_Reload);
    }

    PropertyData getMaterialLibraryProperty(Material& material, Material* def, const std::string& field, bool hasRef) {
        auto data = [&](PropertyType type, uint64_t flags, auto member) -> PropertyData {
            return {type, flags, def ? (void*)&(def->*member) : nullptr, hasRef ? (void*)&(material.*member) : nullptr};
        };
        if (field == "shader") return data(PropertyType::Shader, UpdateFlags_Shader_Reload | UpdateFlags_Shader_Uniforms, &Material::shader);
        if (field == "shaderParams") return data(PropertyType::Custom, UpdateFlags_Shader_Uniforms, &Material::shaderParams);
        if (field == "shaderTextures") return data(PropertyType::Custom, UpdateFlags_Mesh_Reload | UpdateFlags_Mesh_Texture, &Material::shaderTextures);
        if (field == "shaderFeatures") return data(PropertyType::Custom, UpdateFlags_Mesh_Reload, &Material::shaderFeatures);
        if (field == "blendMode") return data(PropertyType::Enum, UpdateFlags_Mesh_Reload, &Material::blendMode);
        if (field == "cullMode") return data(PropertyType::Enum, UpdateFlags_Mesh_Reload, &Material::cullMode);
        if (field == "depthWrite") return data(PropertyType::Enum, UpdateFlags_Mesh_Reload, &Material::depthWrite);
        if (field == "depthTest") return data(PropertyType::Enum, UpdateFlags_Mesh_Reload, &Material::depthTest);
        if (field == "depthOffsetFactor") return data(PropertyType::Float, UpdateFlags_Mesh_Reload, &Material::depthOffsetFactor);
        if (field == "depthOffsetUnits") return data(PropertyType::Float, UpdateFlags_Mesh_Reload, &Material::depthOffsetUnits);
        if (field == "sortPriority") return data(PropertyType::Int, UpdateFlags_Mesh_Reload, &Material::sortPriority);
        if (field == "lighting") return data(PropertyType::Enum, UpdateFlags_Mesh_Reload, &Material::lighting);
        // phase 2: stencil and colour write are pipeline state; the extra pass is its own draw
        // (a MaterialPass: the material's own, made on access, see MaterialExtraPass)
        if (field == "extraPass")
            return {PropertyType::Custom, UpdateFlags_Mesh_Reload | UpdateFlags_Mesh_Texture | UpdateFlags_Shader_Uniforms,
                    def ? (void*)&def->extraPass.edit() : nullptr, hasRef ? (void*)&material.extraPass.edit() : nullptr};
        if (field.compare(0, 10, "extraPass.") == 0)
            return getMaterialPassProperty(hasRef ? &material.extraPass.edit() : nullptr, def ? &def->extraPass.edit() : nullptr, field.substr(10));
        if (field.compare(0, 4, "pbr.") == 0)
            return getMaterialPbrProperty(material, def, field.substr(4), hasRef);
        if (field == "emissiveStrength")
            return data(PropertyType::Float, UpdateFlags_None, &Material::emissiveStrength);
        return getStencilColorWriteProperty(hasRef ? &material.stencil : nullptr, def ? &def->stencil : nullptr,
                                            hasRef ? &material.colorWrite : nullptr, def ? &def->colorWrite : nullptr, field, UpdateFlags_Mesh_Reload);
    }

    void* getLightShadowNearRef(void* comp) {
        return &static_cast<LightComponent*>(comp)->shadowCameraNearFar.x;
    }

    void* getLightShadowFarRef(void* comp) {
        return &static_cast<LightComponent*>(comp)->shadowCameraNearFar.y;
    }

    static const FastPropertyDescriptor kTransformProperties[] = {
        makeFastProperty<Transform, Vector3, &Transform::position>("position", PropertyType::Vector3, UpdateFlags_Transform | UpdateFlags_Layout_Anchors),
        makeFastProperty<Transform, Quaternion, &Transform::rotation>("rotation", PropertyType::Quat, UpdateFlags_Transform),
        makeFastProperty<Transform, Vector3, &Transform::scale>("scale", PropertyType::Vector3, UpdateFlags_Transform | UpdateFlags_Rigidbody2D | UpdateFlags_Rigidbody),
        makeFastProperty<Transform, bool, &Transform::visible>("visible", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<Transform, bool, &Transform::billboard>("billboard", PropertyType::Bool, UpdateFlags_Transform),
        makeFastProperty<Transform, bool, &Transform::fakeBillboard>("fakeBillboard", PropertyType::Bool, UpdateFlags_Transform),
        makeFastProperty<Transform, bool, &Transform::cylindricalBillboard>("cylindricalBillboard", PropertyType::Bool, UpdateFlags_Transform),
        makeFastProperty<Transform, Quaternion, &Transform::billboardRotation>("billboardRotation", PropertyType::Quat, UpdateFlags_Transform),
    };

    static const FastPropertyDescriptor kUIProperties[] = {
        makeFastProperty<UIComponent, Vector4, &UIComponent::color>("color", PropertyType::Vector4, UpdateFlags_None),
        makeFastProperty<UIComponent, Texture, &UIComponent::texture>("texture", PropertyType::Texture, UpdateFlags_UI_Texture),
        makeFastProperty<UIComponent, ShaderRef, &UIComponent::customShader>("customShader", PropertyType::Shader, UpdateFlags_Shader_Reload),
        makeFastProperty<UIComponent, ShaderUniformValues, &UIComponent::shaderUniforms>("shaderUniforms", PropertyType::Custom, UpdateFlags_Shader_Uniforms),
        // shader library: resolved when the UI loads its shader
        makeFastProperty<UIComponent, std::vector<std::pair<std::string, Texture>>, &UIComponent::shaderTextures>("shaderTextures", PropertyType::Custom, UpdateFlags_Shader_Reload),
        makeFastProperty<UIComponent, std::vector<std::string>, &UIComponent::shaderFeatures>("shaderFeatures", PropertyType::Custom, UpdateFlags_Shader_Reload),
        makeFastProperty<UIComponent, MaterialBlendMode, &UIComponent::blendMode>("blendMode", PropertyType::Enum, UpdateFlags_Shader_Reload),
        makeFastProperty<UIComponent, MaterialDepthWrite, &UIComponent::depthWrite>("depthWrite", PropertyType::Enum, UpdateFlags_Shader_Reload),
    };

    static const FastPropertyDescriptor kRectTransformProperties[] = {
        makeFastPropertyNoDefault<RectTransformComponent, unsigned int, &RectTransformComponent::width>("width", PropertyType::UInt, UpdateFlags_Layout_Sizes | UpdateFlags_Layout_Anchors),
        makeFastPropertyNoDefault<RectTransformComponent, unsigned int, &RectTransformComponent::height>("height", PropertyType::UInt, UpdateFlags_Layout_Sizes | UpdateFlags_Layout_Anchors),
        makeFastProperty<RectTransformComponent, float, &RectTransformComponent::anchorPointLeft>("anchorPointLeft", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, float, &RectTransformComponent::anchorPointTop>("anchorPointTop", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, float, &RectTransformComponent::anchorPointRight>("anchorPointRight", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, float, &RectTransformComponent::anchorPointBottom>("anchorPointBottom", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, int, &RectTransformComponent::anchorOffsetLeft>("anchorOffsetLeft", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, int, &RectTransformComponent::anchorOffsetTop>("anchorOffsetTop", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, int, &RectTransformComponent::anchorOffsetRight>("anchorOffsetRight", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, int, &RectTransformComponent::anchorOffsetBottom>("anchorOffsetBottom", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, Vector2, &RectTransformComponent::positionOffset>("positionOffset", PropertyType::Vector2, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, Vector2, &RectTransformComponent::pivot>("pivot", PropertyType::Vector2, UpdateFlags_Transform),
        makeFastProperty<RectTransformComponent, AnchorPreset, &RectTransformComponent::anchorPreset>("anchorPreset", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, bool, &RectTransformComponent::usingAnchors>("usingAnchors", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, bool, &RectTransformComponent::ignoreScissor>("ignoreScissor", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<RectTransformComponent, bool, &RectTransformComponent::ignoreEvents>("ignoreEvents", PropertyType::Bool, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kImageProperties[] = {
        makeFastProperty<ImageComponent, unsigned int, &ImageComponent::patchMarginLeft>("patchMarginLeft", PropertyType::UInt, UpdateFlags_Image_Patches),
        makeFastProperty<ImageComponent, unsigned int, &ImageComponent::patchMarginRight>("patchMarginRight", PropertyType::UInt, UpdateFlags_Image_Patches),
        makeFastProperty<ImageComponent, unsigned int, &ImageComponent::patchMarginTop>("patchMarginTop", PropertyType::UInt, UpdateFlags_Image_Patches),
        makeFastProperty<ImageComponent, unsigned int, &ImageComponent::patchMarginBottom>("patchMarginBottom", PropertyType::UInt, UpdateFlags_Image_Patches),
        makeFastProperty<ImageComponent, float, &ImageComponent::textureScaleFactor>("textureScaleFactor", PropertyType::Float, UpdateFlags_Image_Patches),
    };

    static const FastPropertyDescriptor kButtonProperties[] = {
        makeFastProperty<ButtonComponent, Entity, &ButtonComponent::label>("label", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<ButtonComponent, Texture, &ButtonComponent::textureNormal>("textureNormal", PropertyType::Texture, UpdateFlags_None),
        makeFastProperty<ButtonComponent, Texture, &ButtonComponent::textureHovered>("textureHovered", PropertyType::Texture, UpdateFlags_None),
        makeFastProperty<ButtonComponent, Texture, &ButtonComponent::texturePressed>("texturePressed", PropertyType::Texture, UpdateFlags_None),
        makeFastProperty<ButtonComponent, Texture, &ButtonComponent::textureDisabled>("textureDisabled", PropertyType::Texture, UpdateFlags_None),
        makeFastProperty<ButtonComponent, Vector4, &ButtonComponent::colorNormal>("colorNormal", PropertyType::Vector4, UpdateFlags_None),
        makeFastProperty<ButtonComponent, Vector4, &ButtonComponent::colorHovered>("colorHovered", PropertyType::Vector4, UpdateFlags_None),
        makeFastProperty<ButtonComponent, Vector4, &ButtonComponent::colorPressed>("colorPressed", PropertyType::Vector4, UpdateFlags_None),
        makeFastProperty<ButtonComponent, Vector4, &ButtonComponent::colorDisabled>("colorDisabled", PropertyType::Vector4, UpdateFlags_None),
        makeFastProperty<ButtonComponent, bool, &ButtonComponent::disabled>("disabled", PropertyType::Bool, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kScrollbarProperties[] = {
        makeFastProperty<ScrollbarComponent, Entity, &ScrollbarComponent::bar>("bar", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<ScrollbarComponent, ScrollbarType, &ScrollbarComponent::type>("type", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<ScrollbarComponent, float, &ScrollbarComponent::barSize>("barSize", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<ScrollbarComponent, float, &ScrollbarComponent::step>("step", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<ScrollbarComponent, int, &ScrollbarComponent::barMarginLeft>("barMarginLeft", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<ScrollbarComponent, int, &ScrollbarComponent::barMarginRight>("barMarginRight", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<ScrollbarComponent, int, &ScrollbarComponent::barMarginTop>("barMarginTop", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<ScrollbarComponent, int, &ScrollbarComponent::barMarginBottom>("barMarginBottom", PropertyType::Int, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kProgressbarProperties[] = {
        makeFastProperty<ProgressbarComponent, Entity, &ProgressbarComponent::fill>("fill", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<ProgressbarComponent, ProgressbarType, &ProgressbarComponent::type>("type", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<ProgressbarComponent, float, &ProgressbarComponent::value>("value", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<ProgressbarComponent, int, &ProgressbarComponent::fillMarginLeft>("fillMarginLeft", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<ProgressbarComponent, int, &ProgressbarComponent::fillMarginRight>("fillMarginRight", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<ProgressbarComponent, int, &ProgressbarComponent::fillMarginTop>("fillMarginTop", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<ProgressbarComponent, int, &ProgressbarComponent::fillMarginBottom>("fillMarginBottom", PropertyType::Int, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kPanelProperties[] = {
        makeFastProperty<PanelComponent, Entity, &PanelComponent::headerimage>("headerimage", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<PanelComponent, Entity, &PanelComponent::headercontainer>("headercontainer", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<PanelComponent, Entity, &PanelComponent::headertext>("headertext", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<PanelComponent, AnchorPreset, &PanelComponent::titleAnchorPreset>("titleAnchorPreset", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<PanelComponent, unsigned int, &PanelComponent::minWidth>("minWidth", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<PanelComponent, unsigned int, &PanelComponent::minHeight>("minHeight", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<PanelComponent, int, &PanelComponent::headerMarginLeft>("headerMarginLeft", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<PanelComponent, int, &PanelComponent::headerMarginTop>("headerMarginTop", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<PanelComponent, int, &PanelComponent::headerMarginRight>("headerMarginRight", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<PanelComponent, int, &PanelComponent::headerMarginBottom>("headerMarginBottom", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<PanelComponent, bool, &PanelComponent::defaultHeaderMargin>("defaultHeaderMargin", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<PanelComponent, int, &PanelComponent::resizeMargin>("resizeMargin", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<PanelComponent, bool, &PanelComponent::canMove>("canMove", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<PanelComponent, bool, &PanelComponent::canResize>("canResize", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<PanelComponent, bool, &PanelComponent::canBringToFront>("canBringToFront", PropertyType::Bool, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kInputFieldProperties[] = {
        makeFastProperty<InputFieldComponent, Entity, &InputFieldComponent::text>("text", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, Entity, &InputFieldComponent::selection>("selection", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, Entity, &InputFieldComponent::cursor>("cursor", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, float, &InputFieldComponent::cursorBlink>("cursorBlink", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, float, &InputFieldComponent::cursorWidth>("cursorWidth", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, Vector4, &InputFieldComponent::cursorColor>("cursorColor", PropertyType::Vector4, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, Vector4, &InputFieldComponent::selectionColor>("selectionColor", PropertyType::Vector4, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, Vector4, &InputFieldComponent::placeholderColor>("placeholderColor", PropertyType::Vector4, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, std::string, &InputFieldComponent::placeholder>("placeholder", PropertyType::String, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, int, &InputFieldComponent::cursorIndex>("cursorIndex", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, int, &InputFieldComponent::selectionAnchor>("selectionAnchor", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, bool, &InputFieldComponent::disabled>("disabled", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<InputFieldComponent, bool, &InputFieldComponent::password>("password", PropertyType::Bool, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kSpriteTopProperties[] = {
        makeFastPropertyNoDefault<SpriteComponent, unsigned int, &SpriteComponent::width>("width", PropertyType::UInt, UpdateFlags_Sprite),
        makeFastPropertyNoDefault<SpriteComponent, unsigned int, &SpriteComponent::height>("height", PropertyType::UInt, UpdateFlags_Sprite),
        makeFastProperty<SpriteComponent, PivotPreset, &SpriteComponent::pivotPreset>("pivotPreset", PropertyType::Enum, UpdateFlags_Sprite),
        makeFastProperty<SpriteComponent, float, &SpriteComponent::textureScaleFactor>("textureScaleFactor", PropertyType::Float, UpdateFlags_Sprite),
        makeFastProperty<SpriteComponent, bool, &SpriteComponent::automaticFlipY>("automaticFlipY", PropertyType::Bool, UpdateFlags_Sprite),
        makeFastProperty<SpriteComponent, bool, &SpriteComponent::flipY>("flipY", PropertyType::Bool, UpdateFlags_Sprite),
    };

    static const FastPropertyDescriptor kTilemapTopProperties[] = {
        makeFastPropertyNoDefault<TilemapComponent, unsigned int, &TilemapComponent::width>("width", PropertyType::UInt, UpdateFlags_Tilemap),
        makeFastPropertyNoDefault<TilemapComponent, unsigned int, &TilemapComponent::height>("height", PropertyType::UInt, UpdateFlags_Tilemap),
        makeFastProperty<TilemapComponent, bool, &TilemapComponent::automaticFlipY>("automaticFlipY", PropertyType::Bool, UpdateFlags_Tilemap),
        makeFastProperty<TilemapComponent, bool, &TilemapComponent::flipY>("flipY", PropertyType::Bool, UpdateFlags_Tilemap),
        makeFastProperty<TilemapComponent, float, &TilemapComponent::textureScaleFactor>("textureScaleFactor", PropertyType::Float, UpdateFlags_Tilemap),
        makeFastProperty<TilemapComponent, unsigned int, &TilemapComponent::reserveTiles>("reserveTiles", PropertyType::UInt, UpdateFlags_Tilemap),
    };

    static const FastPropertyDescriptor kMeshPolygonTopProperties[] = {
        makeFastProperty<MeshPolygonComponent, bool, &MeshPolygonComponent::automaticFlipY>("automaticFlipY", PropertyType::Bool, UpdateFlags_Mesh_Reload),
        makeFastProperty<MeshPolygonComponent, bool, &MeshPolygonComponent::flipY>("flipY", PropertyType::Bool, UpdateFlags_Mesh_Reload),
    };

    static const FastPropertyDescriptor kTerrainProperties[] = {
        makeFastProperty<TerrainComponent, Texture, &TerrainComponent::heightMap>("heightMap", PropertyType::Texture, UpdateFlags_Terrain | UpdateFlags_Terrain_Texture | UpdateFlags_Terrain_Foliage),
        makeFastProperty<TerrainComponent, bool, &TerrainComponent::autoSetRanges>("autoSetRanges", PropertyType::Bool, UpdateFlags_Terrain),
        makeFastProperty<TerrainComponent, Vector2, &TerrainComponent::offset>("offset", PropertyType::Vector2, UpdateFlags_None),
        makeFastProperty<TerrainComponent, float, &TerrainComponent::terrainSize>("terrainSize", PropertyType::Float, UpdateFlags_Terrain | UpdateFlags_Terrain_Foliage),
        makeFastProperty<TerrainComponent, float, &TerrainComponent::maxHeight>("maxHeight", PropertyType::Float, UpdateFlags_Terrain | UpdateFlags_Terrain_Foliage),
        makeFastProperty<TerrainComponent, float, &TerrainComponent::resolution>("resolution", PropertyType::Float, UpdateFlags_Terrain | UpdateFlags_Mesh_Reload),
        makeFastProperty<TerrainComponent, float, &TerrainComponent::textureBaseTiles>("textureBaseTiles", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<TerrainComponent, float, &TerrainComponent::textureDetailTiles>("textureDetailTiles", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<TerrainComponent, int, &TerrainComponent::rootGridSize>("rootGridSize", PropertyType::Int, UpdateFlags_Terrain | UpdateFlags_Mesh_Reload),
        makeFastProperty<TerrainComponent, int, &TerrainComponent::levels>("levels", PropertyType::Int, UpdateFlags_Terrain | UpdateFlags_Mesh_Reload),
    };

    static const FastPropertyDescriptor kActionProperties[] = {
        makeFastPropertyNoDefault<ActionComponent, ActionState, &ActionComponent::state>("state", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<ActionComponent, float, &ActionComponent::speed>("speed", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<ActionComponent, Entity, &ActionComponent::target>("target", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<ActionComponent, bool, &ActionComponent::ownedTarget>("ownedTarget", PropertyType::Bool, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kTweenProperties[] = {
        makeFastProperty<TweenComponent, float, &TweenComponent::duration>("duration", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<TweenComponent, bool, &TweenComponent::loop>("loop", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<TweenComponent, bool, &TweenComponent::pingPong>("pingPong", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<TweenComponent, Ease, &TweenComponent::function>("function", PropertyType::Ease, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kPositionTweenProperties[] = {
        makeFastPropertyNoDefault<PositionTweenComponent, Vector3, &PositionTweenComponent::endPosition>("endPosition", PropertyType::Vector3, UpdateFlags_None),
        makeFastPropertyNoDefault<PositionTweenComponent, Vector3, &PositionTweenComponent::startPosition>("startPosition", PropertyType::Vector3, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kRotationTweenProperties[] = {
        makeFastPropertyNoDefault<RotationTweenComponent, Quaternion, &RotationTweenComponent::endRotation>("endRotation", PropertyType::Quat, UpdateFlags_None),
        makeFastPropertyNoDefault<RotationTweenComponent, Quaternion, &RotationTweenComponent::startRotation>("startRotation", PropertyType::Quat, UpdateFlags_None),
        makeFastProperty<RotationTweenComponent, bool, &RotationTweenComponent::shortestPath>("shortestPath", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<RotationTweenComponent, bool, &RotationTweenComponent::spin>("spin", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<RotationTweenComponent, Vector3, &RotationTweenComponent::axis>("axis", PropertyType::Vector3, UpdateFlags_None),
        makeFastProperty<RotationTweenComponent, float, &RotationTweenComponent::angle>("angle", PropertyType::Float, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kScaleTweenProperties[] = {
        makeFastPropertyNoDefault<ScaleTweenComponent, Vector3, &ScaleTweenComponent::endScale>("endScale", PropertyType::Vector3, UpdateFlags_None),
        makeFastPropertyNoDefault<ScaleTweenComponent, Vector3, &ScaleTweenComponent::startScale>("startScale", PropertyType::Vector3, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kColorTweenProperties[] = {
        makeFastPropertyNoDefault<ColorTweenComponent, Vector3, &ColorTweenComponent::endColor>("endColor", PropertyType::Vector3, UpdateFlags_None),
        makeFastPropertyNoDefault<ColorTweenComponent, Vector3, &ColorTweenComponent::startColor>("startColor", PropertyType::Vector3, UpdateFlags_None),
        makeFastProperty<ColorTweenComponent, bool, &ColorTweenComponent::useSRGB>("useSRGB", PropertyType::Bool, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kAlphaTweenProperties[] = {
        makeFastPropertyNoDefault<AlphaTweenComponent, float, &AlphaTweenComponent::endAlpha>("endAlpha", PropertyType::Float, UpdateFlags_None),
        makeFastPropertyNoDefault<AlphaTweenComponent, float, &AlphaTweenComponent::startAlpha>("startAlpha", PropertyType::Float, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kPrefabProperties[] = {
        makeFastProperty<PrefabComponent, std::string, &PrefabComponent::name>("name", PropertyType::String, UpdateFlags_None),
        makeFastProperty<PrefabComponent, std::string, &PrefabComponent::path>("path", PropertyType::String, UpdateFlags_None),
        makeFastProperty<PrefabComponent, std::string, &PrefabComponent::guid>("guid", PropertyType::String, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kBoneProperties[] = {
        makeFastPropertyNoDefault<BoneComponent, Entity, &BoneComponent::model>("model", PropertyType::Entity, UpdateFlags_None),
        makeFastPropertyNoDefault<BoneComponent, int, &BoneComponent::index>("index", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<BoneComponent, Vector3, &BoneComponent::bindPosition>("bindPosition", PropertyType::Vector3, UpdateFlags_None),
        makeFastProperty<BoneComponent, Quaternion, &BoneComponent::bindRotation>("bindRotation", PropertyType::Quat, UpdateFlags_None),
        makeFastProperty<BoneComponent, Vector3, &BoneComponent::bindScale>("bindScale", PropertyType::Vector3, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kKeyframeTracksProperties[] = {
        makeFastPropertyNoDefault<KeyframeTracksComponent, int, &KeyframeTracksComponent::index>("index", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<KeyframeTracksComponent, float, &KeyframeTracksComponent::interpolation>("interpolation", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<KeyframeTracksComponent, bool, &KeyframeTracksComponent::loop>("loop", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<KeyframeTracksComponent, bool, &KeyframeTracksComponent::relative>("relative", PropertyType::Bool, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kSpriteAnimationProperties[] = {
        makeFastProperty<SpriteAnimationComponent, bool, &SpriteAnimationComponent::loop>("loop", PropertyType::Bool, UpdateFlags_None),
        makeFastPropertyNoDefault<SpriteAnimationComponent, unsigned int, &SpriteAnimationComponent::framesSize>("framesSize", PropertyType::UInt, UpdateFlags_None),
        makeFastPropertyNoDefault<SpriteAnimationComponent, unsigned int, &SpriteAnimationComponent::framesTimeSize>("framesTimeSize", PropertyType::UInt, UpdateFlags_None),
        makeFastPropertyNoDefault<SpriteAnimationComponent, int, &SpriteAnimationComponent::frameIndex>("frameIndex", PropertyType::Int, UpdateFlags_Sprite),
        makeFastPropertyNoDefault<SpriteAnimationComponent, int, &SpriteAnimationComponent::frameTimeIndex>("frameTimeIndex", PropertyType::Int, UpdateFlags_None),
        makeFastPropertyNoDefault<SpriteAnimationComponent, unsigned int, &SpriteAnimationComponent::spriteFrameCount>("spriteFrameCount", PropertyType::UInt, UpdateFlags_Sprite),
    };

    static const FastPropertyDescriptor kAnimationProperties[] = {
        makeFastProperty<AnimationComponent, bool, &AnimationComponent::loop>("loop", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<AnimationComponent, float, &AnimationComponent::duration>("duration", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<AnimationComponent, bool, &AnimationComponent::ownedActions>("ownedActions", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<AnimationComponent, float, &AnimationComponent::defaultFadeTime>("defaultFadeTime", PropertyType::Float, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kLightProperties[] = {
        makeFastPropertyNoDefault<LightComponent, LightType, &LightComponent::type>("type", PropertyType::Enum, UpdateFlags_LightShadowMap | UpdateFlags_LightShadowCamera | UpdateFlags_Scene_Mesh_Variant),
        makeFastProperty<LightComponent, Vector3, &LightComponent::direction>("direction", PropertyType::Vector3, UpdateFlags_Transform),
        makeFastProperty<LightComponent, bool, &LightComponent::shadows>("shadows", PropertyType::Bool, UpdateFlags_LightShadowCamera | UpdateFlags_Scene_Mesh_Variant),
        makeFastProperty<LightComponent, float, &LightComponent::intensity>("intensity", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<LightComponent, float, &LightComponent::range>("range", PropertyType::Float, UpdateFlags_LightShadowCamera),
        makeFastProperty<LightComponent, Vector3, &LightComponent::color>("color", PropertyType::Vector3, UpdateFlags_None),
        makeFastProperty<LightComponent, float, &LightComponent::innerConeCos>("innerConeCos", PropertyType::Float, UpdateFlags_LightShadowCamera),
        makeFastProperty<LightComponent, float, &LightComponent::outerConeCos>("outerConeCos", PropertyType::Float, UpdateFlags_LightShadowCamera),
        makeFastProperty<LightComponent, Texture, &LightComponent::spotMask>("spotMask", PropertyType::Texture, UpdateFlags_LightShadowCamera),
        // read by RenderSystem every frame (lighting and shadow casters)
        makeFastProperty<LightComponent, uint32_t, &LightComponent::cullingMask>("cullingMask", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<LightComponent, float, &LightComponent::shadowBias>("shadowBias", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<LightComponent, unsigned int, &LightComponent::mapResolution>("mapResolution", PropertyType::UInt, UpdateFlags_LightShadowMap | UpdateFlags_Scene_Mesh_Variant),
        makeFastProperty<LightComponent, bool, &LightComponent::automaticShadowCamera>("automaticShadowCamera", PropertyType::Bool, UpdateFlags_LightShadowCamera),
        {"shadowCameraNear", PropertyType::Float, UpdateFlags_LightShadowCamera, nullptr, &getLightShadowNearRef},
        {"shadowCameraFar", PropertyType::Float, UpdateFlags_LightShadowCamera, nullptr, &getLightShadowFarRef},
        makeFastProperty<LightComponent, unsigned int, &LightComponent::numShadowCascades>("numShadowCascades", PropertyType::UInt, UpdateFlags_LightShadowCamera | UpdateFlags_Scene_Mesh_Variant),
        // read by RenderSystem when it picks each mesh's lights
        makeFastProperty<LightComponent, LightShading, &LightComponent::shading>("shading", PropertyType::Enum, UpdateFlags_None),
        // photometric units and colour temperature, resolved on the CPU (util/LightPhotometry.h)
        makeFastProperty<LightComponent, LightUnit, &LightComponent::intensityUnit>("intensityUnit", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<LightComponent, bool, &LightComponent::useColorTemperature>("useColorTemperature", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<LightComponent, float, &LightComponent::colorTemperature>("colorTemperature", PropertyType::Float, UpdateFlags_None),
        // shadows (docs/rendering/shadows.md): what the maps depend on reaches RenderSystem
        // through the light's settings hash every frame, the rest is read by the receivers
        makeFastProperty<LightComponent, bool, &LightComponent::softShadows>("softShadows", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<LightComponent, float, &LightComponent::shadowStrength>("shadowStrength", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<LightComponent, float, &LightComponent::shadowNormalBias>("shadowNormalBias", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<LightComponent, CascadeSplitMode, &LightComponent::cascadeSplitMode>("cascadeSplitMode", PropertyType::Enum, UpdateFlags_LightShadowCamera),
        makeFastProperty<LightComponent, Vector3, &LightComponent::cascadeSplits>("cascadeSplits", PropertyType::Vector3, UpdateFlags_LightShadowCamera),
        makeFastProperty<LightComponent, float, &LightComponent::shadowDistance>("shadowDistance", PropertyType::Float, UpdateFlags_LightShadowCamera),
        makeFastProperty<LightComponent, float, &LightComponent::shadowAngularDiameter>("shadowAngularDiameter", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<LightComponent, float, &LightComponent::shadowSourceRadius>("shadowSourceRadius", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<LightComponent, ShadowUpdateMode, &LightComponent::shadowUpdateMode>("shadowUpdateMode", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<LightComponent, float, &LightComponent::volumetricIntensity>("volumetricIntensity", PropertyType::Float, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kLight2DProperties[] = {
        makeFastProperty<Light2DComponent, Vector3, &Light2DComponent::color>("color", PropertyType::Vector3, UpdateFlags_None),
        makeFastProperty<Light2DComponent, float, &Light2DComponent::intensity>("intensity", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<Light2DComponent, float, &Light2DComponent::range>("range", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<Light2DComponent, float, &Light2DComponent::falloff>("falloff", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<Light2DComponent, float, &Light2DComponent::height>("height", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<Light2DComponent, bool, &Light2DComponent::shadows>("shadows", PropertyType::Bool, UpdateFlags_Scene_Mesh_Variant),
        makeFastProperty<Light2DComponent, float, &Light2DComponent::shadowBias>("shadowBias", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<Light2DComponent, float, &Light2DComponent::shadowSoftness>("shadowSoftness", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<Light2DComponent, unsigned int, &Light2DComponent::mapResolution>("mapResolution", PropertyType::UInt, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kShadowCaster2DProperties[] = {
        makeFastProperty<ShadowCaster2DComponent, ShadowCaster2DShape, &ShadowCaster2DComponent::shape>("shape", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<ShadowCaster2DComponent, bool, &ShadowCaster2DComponent::closed>("closed", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<ShadowCaster2DComponent, bool, &ShadowCaster2DComponent::enabled>("enabled", PropertyType::Bool, UpdateFlags_None),
        makeFastPropertyNoDefault<ShadowCaster2DComponent, std::vector<Vector2>, &ShadowCaster2DComponent::points>("points", PropertyType::Custom, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kFogProperties[] = {
        makeFastProperty<FogComponent, FogType, &FogComponent::type>("type", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<FogComponent, Vector3, &FogComponent::color>("color", PropertyType::Vector3, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::density>("density", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::linearStart>("linearStart", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::linearEnd>("linearEnd", PropertyType::Float, UpdateFlags_None),
        // the height fog (type HEIGHT; RenderSystem switches the variants and depth programs itself)
        makeFastProperty<FogComponent, FogColorMode, &FogComponent::colorMode>("colorMode", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::attenuationDistance>("attenuationDistance", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::baseHeight>("baseHeight", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::maximumHeight>("maximumHeight", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::startDistance>("startDistance", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::maxFogDistance>("maxFogDistance", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::maxOpacity>("maxOpacity", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::directionalInscattering>("directionalInscattering", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::anisotropy>("anisotropy", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, bool, &FogComponent::skyFill>("skyFill", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::skyFillAngle>("skyFillAngle", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, bool, &FogComponent::volumetric>("volumetric", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::volumetricDistance>("volumetricDistance", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, bool, &FogComponent::lightShafts>("lightShafts", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::noiseIntensity>("noiseIntensity", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, float, &FogComponent::noiseScale>("noiseScale", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FogComponent, Vector3, &FogComponent::wind>("wind", PropertyType::Vector3, UpdateFlags_None),
        makeFastProperty<FogComponent, AtmosphereQualityOverride, &FogComponent::qualityOverride>("qualityOverride", PropertyType::Enum, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kLocalVolumetricFogProperties[] = {
        makeFastProperty<LocalVolumetricFogComponent, FogVolumeShape, &LocalVolumetricFogComponent::shape>("shape", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<LocalVolumetricFogComponent, Vector3, &LocalVolumetricFogComponent::size>("size", PropertyType::Vector3, UpdateFlags_None),
        makeFastProperty<LocalVolumetricFogComponent, float, &LocalVolumetricFogComponent::attenuationDistance>("attenuationDistance", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<LocalVolumetricFogComponent, Vector3, &LocalVolumetricFogComponent::albedo>("albedo", PropertyType::Vector3, UpdateFlags_None),
        makeFastProperty<LocalVolumetricFogComponent, float, &LocalVolumetricFogComponent::blendDistance>("blendDistance", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<LocalVolumetricFogComponent, FogVolumeFalloff, &LocalVolumetricFogComponent::falloff>("falloff", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<LocalVolumetricFogComponent, float, &LocalVolumetricFogComponent::noiseIntensity>("noiseIntensity", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<LocalVolumetricFogComponent, float, &LocalVolumetricFogComponent::noiseScale>("noiseScale", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<LocalVolumetricFogComponent, Vector3, &LocalVolumetricFogComponent::scrollSpeed>("scrollSpeed", PropertyType::Vector3, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kVolumetricCloudsProperties[] = {
        makeFastProperty<VolumetricCloudsComponent, CloudsPreset, &VolumetricCloudsComponent::preset>("preset", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, CloudsRendering, &VolumetricCloudsComponent::rendering>("rendering", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::coverage>("coverage", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::density>("density", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::shapeScale>("shapeScale", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::shapeFactor>("shapeFactor", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::erosionFactor>("erosionFactor", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::erosionScale>("erosionScale", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::bottomAltitude>("bottomAltitude", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::altitudeRange>("altitudeRange", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::earthCurvature>("earthCurvature", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::windDirection>("windDirection", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::windSpeed>("windSpeed", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, Texture, &VolumetricCloudsComponent::weatherMap>("weatherMap", PropertyType::Texture, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::weatherMapScale>("weatherMapScale", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::sunLightDimmer>("sunLightDimmer", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::ambientLightDimmer>("ambientLightDimmer", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, float, &VolumetricCloudsComponent::maxDistance>("maxDistance", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<VolumetricCloudsComponent, AtmosphereQualityOverride, &VolumetricCloudsComponent::qualityOverride>("qualityOverride", PropertyType::Enum, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kMirrorProperties[] = {
        makeFastProperty<MirrorComponent, Vector3, &MirrorComponent::normal>("normal", PropertyType::Vector3, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kReflectionProbeProperties[] = {
        makeFastProperty<ReflectionProbeComponent, ReflectionProbeMode, &ReflectionProbeComponent::mode>("mode", PropertyType::Enum, UpdateFlags_Reflection_Probe_Capture),
        makeFastProperty<ReflectionProbeComponent, ReflectionProbeUpdateMode, &ReflectionProbeComponent::updateMode>("updateMode", PropertyType::Enum, UpdateFlags_Reflection_Probe_Sampling),
        makeFastProperty<ReflectionProbeComponent, Vector3, &ReflectionProbeComponent::boxOffset>("boxOffset", PropertyType::Vector3, UpdateFlags_Reflection_Probe_Sampling),
        makeFastProperty<ReflectionProbeComponent, Vector3, &ReflectionProbeComponent::boxSize>("boxSize", PropertyType::Vector3, UpdateFlags_Reflection_Probe_Sampling),
        makeFastProperty<ReflectionProbeComponent, float, &ReflectionProbeComponent::blendDistance>("blendDistance", PropertyType::Float, UpdateFlags_Reflection_Probe_Sampling),
        makeFastProperty<ReflectionProbeComponent, float, &ReflectionProbeComponent::intensity>("intensity", PropertyType::Float, UpdateFlags_Reflection_Probe_Sampling),
        makeFastProperty<ReflectionProbeComponent, int, &ReflectionProbeComponent::priority>("priority", PropertyType::Int, UpdateFlags_Reflection_Probe_Sampling),
        makeFastProperty<ReflectionProbeComponent, unsigned int, &ReflectionProbeComponent::resolution>("resolution", PropertyType::UInt, UpdateFlags_Reflection_Probe_Capture),
        makeFastProperty<ReflectionProbeComponent, float, &ReflectionProbeComponent::nearClip>("nearClip", PropertyType::Float, UpdateFlags_Reflection_Probe_Capture),
        makeFastProperty<ReflectionProbeComponent, float, &ReflectionProbeComponent::farClip>("farClip", PropertyType::Float, UpdateFlags_Reflection_Probe_Capture),
        makeFastProperty<ReflectionProbeComponent, float, &ReflectionProbeComponent::updateInterval>("updateInterval", PropertyType::Float, UpdateFlags_Reflection_Probe_Sampling),
        makeFastProperty<ReflectionProbeComponent, bool, &ReflectionProbeComponent::includeSky>("includeSky", PropertyType::Bool, UpdateFlags_Reflection_Probe_Capture),
        makeFastProperty<ReflectionProbeComponent, uint32_t, &ReflectionProbeComponent::cullingMask>("cullingMask", PropertyType::UInt, UpdateFlags_Reflection_Probe_Capture),
        makeFastProperty<ReflectionProbeComponent, Texture, &ReflectionProbeComponent::texture>("texture", PropertyType::Texture, UpdateFlags_Reflection_Probe_Capture),
    };

    // enabled is the shared component checkbox (findEnabledGetter)
    static const FastPropertyDescriptor kUIDocumentProperties[] = {
        makeFastProperty<UIDocumentComponent, AssetRef, &UIDocumentComponent::filename>("filename", PropertyType::Asset, UpdateFlags_UIDocument_Reload),
        makeFastProperty<UIDocumentComponent, int, &UIDocumentComponent::sortOrder>("sortOrder", PropertyType::Int, UpdateFlags_None),
        makeFastProperty<UIDocumentComponent, bool, &UIDocumentComponent::interactive>("interactive", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<UIDocumentComponent, UIDocumentRenderMode, &UIDocumentComponent::renderMode>("renderMode", PropertyType::Enum, UpdateFlags_UIDocument_Reload),
        // clamped to 1..8192 by updateEntity
        makeFastProperty<UIDocumentComponent, int, &UIDocumentComponent::textureWidth>("textureWidth", PropertyType::Int, UpdateFlags_UIDocument_Reload),
        makeFastProperty<UIDocumentComponent, int, &UIDocumentComponent::textureHeight>("textureHeight", PropertyType::Int, UpdateFlags_UIDocument_Reload),
        makeFastProperty<UIDocumentComponent, Entity, &UIDocumentComponent::inputSurface>("inputSurface", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<UIDocumentComponent, float, &UIDocumentComponent::pixelRatio>("pixelRatio", PropertyType::Float, UpdateFlags_UIDocument_Reload),
    };

    // enabled is the shared component checkbox (findEnabledGetter). The runtime fields (the
    // simulator, the queued splats, bursts and steps, needReset, revision) are not properties.
    // Sizes and the look need no flag: FluidSimulator recreates its targets and reads every
    // value each frame; what the start depends on (the seed, the start burst) restarts the
    // fluid (UpdateFlags_Fluid_Reset).
    static const FastPropertyDescriptor kFluidSimulationProperties[] = {
        makeFastProperty<FluidSimulationComponent, FluidOutput, &FluidSimulationComponent::output>("output", PropertyType::Enum, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, bool, &FluidSimulationComponent::interactive>("interactive", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::aspectRatio>("aspectRatio", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, unsigned int, &FluidSimulationComponent::simResolution>("simResolution", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, unsigned int, &FluidSimulationComponent::dyeResolution>("dyeResolution", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::densityDissipation>("densityDissipation", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::velocityDissipation>("velocityDissipation", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::pressure>("pressure", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, unsigned int, &FluidSimulationComponent::pressureIterations>("pressureIterations", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::curl>("curl", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::gradientScale>("gradientScale", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::simulationRate>("simulationRate", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, unsigned int, &FluidSimulationComponent::maxSubsteps>("maxSubsteps", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::timeScale>("timeScale", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, bool, &FluidSimulationComponent::paused>("paused", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::splatRadius>("splatRadius", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::splatForce>("splatForce", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, bool, &FluidSimulationComponent::colorful>("colorful", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::colorUpdateSpeed>("colorUpdateSpeed", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, bool, &FluidSimulationComponent::splatsOnStart>("splatsOnStart", PropertyType::Bool, UpdateFlags_Fluid_Reset),
        makeFastProperty<FluidSimulationComponent, unsigned int, &FluidSimulationComponent::randomSplatCount>("randomSplatCount", PropertyType::UInt, UpdateFlags_Fluid_Reset),
        makeFastProperty<FluidSimulationComponent, uint32_t, &FluidSimulationComponent::seed>("seed", PropertyType::UInt, UpdateFlags_Fluid_Reset),
        makeFastProperty<FluidSimulationComponent, bool, &FluidSimulationComponent::shading>("shading", PropertyType::Bool, UpdateFlags_None),
        // display-space values like the dye (no linear conversion: a Vector3, not a Color3L row)
        makeFastProperty<FluidSimulationComponent, Vector3, &FluidSimulationComponent::backColor>("backColor", PropertyType::Vector3, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, bool, &FluidSimulationComponent::transparent>("transparent", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, bool, &FluidSimulationComponent::bloom>("bloom", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, unsigned int, &FluidSimulationComponent::bloomIterations>("bloomIterations", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, unsigned int, &FluidSimulationComponent::bloomResolution>("bloomResolution", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::bloomIntensity>("bloomIntensity", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::bloomThreshold>("bloomThreshold", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::bloomSoftKnee>("bloomSoftKnee", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, bool, &FluidSimulationComponent::sunrays>("sunrays", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, unsigned int, &FluidSimulationComponent::sunraysResolution>("sunraysResolution", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<FluidSimulationComponent, float, &FluidSimulationComponent::sunraysWeight>("sunraysWeight", PropertyType::Float, UpdateFlags_None),
    };

    // enabled is the shared component checkbox (findEnabledGetter). Resolution is written by
    // LatticeEditorOps::setResolution together with the resampled offsets; updateEntity clamps it.
    static const FastPropertyDescriptor kLatticeProperties[] = {
        makeFastProperty<LatticeComponent, int, &LatticeComponent::resolutionX>("resolutionX", PropertyType::Int, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, int, &LatticeComponent::resolutionY>("resolutionY", PropertyType::Int, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, int, &LatticeComponent::resolutionZ>("resolutionZ", PropertyType::Int, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, LatticeInterpolation, &LatticeComponent::interpolation>("interpolation", PropertyType::Enum, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, LatticeOutside, &LatticeComponent::outside>("outside", PropertyType::Enum, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, float, &LatticeComponent::fadeDistance>("fadeDistance", PropertyType::Float, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, bool, &LatticeComponent::jiggle>("jiggle", PropertyType::Bool, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, float, &LatticeComponent::jiggleStiffness>("jiggleStiffness", PropertyType::Float, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, float, &LatticeComponent::jiggleDamping>("jiggleDamping", PropertyType::Float, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, float, &LatticeComponent::jiggleInertia>("jiggleInertia", PropertyType::Float, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, LatticeJiggleAnchor, &LatticeComponent::jiggleAnchor>("jiggleAnchor", PropertyType::Enum, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, float, &LatticeComponent::jiggleMaxOffset>("jiggleMaxOffset", PropertyType::Float, UpdateFlags_Lattice),
        makeFastProperty<LatticeComponent, std::vector<Vector3>, &LatticeComponent::offsets>("offsets", PropertyType::Custom, UpdateFlags_Lattice),
        // poses only matter when applied, which writes offsets
        makeFastProperty<LatticeComponent, std::vector<LatticePose>, &LatticeComponent::poses>("poses", PropertyType::Custom, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kLatticeModifierProperties[] = {
        makeFastProperty<LatticeModifierComponent, std::vector<LatticeBinding>, &LatticeModifierComponent::bindings>("bindings", PropertyType::Custom, UpdateFlags_Lattice),
        makeFastProperty<LatticeModifierComponent, bool, &LatticeModifierComponent::includeChildren>("includeChildren", PropertyType::Bool, UpdateFlags_Lattice),
        // clamped to 1..64 by updateEntity
        makeFastProperty<LatticeModifierComponent, int, &LatticeModifierComponent::subdivisions>("subdivisions", PropertyType::Int, UpdateFlags_Lattice),
    };

    // read by LatticeSystem::lateUpdate every frame (no update flag needed)
    static const FastPropertyDescriptor kLatticeFollowerProperties[] = {
        makeFastProperty<LatticeFollowerComponent, Entity, &LatticeFollowerComponent::lattice>("lattice", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<LatticeFollowerComponent, Entity, &LatticeFollowerComponent::rest>("rest", PropertyType::Entity, UpdateFlags_None),
        makeFastProperty<LatticeFollowerComponent, bool, &LatticeFollowerComponent::applyPosition>("applyPosition", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<LatticeFollowerComponent, bool, &LatticeFollowerComponent::applyRotation>("applyRotation", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<LatticeFollowerComponent, bool, &LatticeFollowerComponent::applyScale>("applyScale", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<LatticeFollowerComponent, float, &LatticeFollowerComponent::weight>("weight", PropertyType::Float, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kLatticeTracksProperties[] = {
        makeFastProperty<LatticeTracksComponent, std::vector<int>, &LatticeTracksComponent::points>("points", PropertyType::Custom, UpdateFlags_None),
        makeFastProperty<LatticeTracksComponent, std::vector<std::vector<Vector3>>, &LatticeTracksComponent::values>("values", PropertyType::Custom, UpdateFlags_None),
        // Hermite tangents, kept mirrored with values by key-edit commands
        makeFastProperty<LatticeTracksComponent, std::vector<std::vector<Vector3>>, &LatticeTracksComponent::inTangents>("inTangents", PropertyType::Custom, UpdateFlags_None),
        makeFastProperty<LatticeTracksComponent, std::vector<std::vector<Vector3>>, &LatticeTracksComponent::outTangents>("outTangents", PropertyType::Custom, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kPoseAnimatorProperties[] = {
        makeFastProperty<PoseAnimatorComponent, std::string, &PoseAnimatorComponent::animation>("animation", PropertyType::String, UpdateFlags_PoseAnimator_Reload),
        makeFastProperty<PoseAnimatorComponent, bool, &PoseAnimatorComponent::autoplay>("autoplay", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<PoseAnimatorComponent, float, &PoseAnimatorComponent::speed>("speed", PropertyType::Float, UpdateFlags_None),
        // clamped to 0..1 by updateEntity
        makeFastProperty<PoseAnimatorComponent, float, &PoseAnimatorComponent::weight>("weight", PropertyType::Float, UpdateFlags_PoseAnimator_Reload),
    };

    // enabled is the shared component checkbox (findEnabledGetter). The clip list is `clips`
    // (Custom: the whole list, what a prefab override of the list holds) plus
    // clips[i].clip/guid/name/wrap (resolveAnimationPlayerPropertyFast); the runtime fields are
    // not properties. The settings are read by the player when it plays (no update flag).
    static const FastPropertyDescriptor kAnimationPlayerProperties[] = {
        makeFastProperty<AnimationPlayerComponent, std::vector<AnimationPlayerClip>, &AnimationPlayerComponent::clips>("clips", PropertyType::Custom, UpdateFlags_AnimationPlayer_Reload),
        makeFastProperty<AnimationPlayerComponent, std::string, &AnimationPlayerComponent::defaultClip>("defaultClip", PropertyType::String, UpdateFlags_None),
        makeFastProperty<AnimationPlayerComponent, bool, &AnimationPlayerComponent::playAutomatically>("playAutomatically", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<AnimationPlayerComponent, float, &AnimationPlayerComponent::speed>("speed", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<AnimationPlayerComponent, float, &AnimationPlayerComponent::defaultFadeTime>("defaultFadeTime", PropertyType::Float, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kCameraProperties[] = {
        makeFastProperty<CameraComponent, CameraType, &CameraComponent::type>("type", PropertyType::Enum, UpdateFlags_Camera),
        // read by RenderSystem when the camera draws
        makeFastProperty<CameraComponent, uint32_t, &CameraComponent::cullingMask>("cullingMask", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<CameraComponent, Vector3, &CameraComponent::target>("target", PropertyType::Vector3, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, Vector3, &CameraComponent::up>("up", PropertyType::Vector3, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, float, &CameraComponent::leftClip>("left", PropertyType::Float, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, float, &CameraComponent::rightClip>("right", PropertyType::Float, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, float, &CameraComponent::bottomClip>("bottom", PropertyType::Float, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, float, &CameraComponent::topClip>("top", PropertyType::Float, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, float, &CameraComponent::yfov>("yfov", PropertyType::Float, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, float, &CameraComponent::aspect>("aspect", PropertyType::Float, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, float, &CameraComponent::nearClip>("near", PropertyType::Float, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, float, &CameraComponent::farClip>("far", PropertyType::Float, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, bool, &CameraComponent::renderToTexture>("renderToTexture", PropertyType::Bool, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, bool, &CameraComponent::transparentSort>("transparentSort", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<CameraComponent, bool, &CameraComponent::depthTest>("depthTest", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<CameraComponent, bool, &CameraComponent::useTarget>("useTarget", PropertyType::Bool, UpdateFlags_Camera),
        makeFastProperty<CameraComponent, bool, &CameraComponent::autoResize>("autoResize", PropertyType::Bool, UpdateFlags_Camera),
        // framebuffer settings are applied by RenderSystem::update (no update flag needed)
        makeFastProperty<CameraComponent, unsigned int, &CameraComponent::framebufferWidth>("framebufferWidth", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<CameraComponent, unsigned int, &CameraComponent::framebufferHeight>("framebufferHeight", PropertyType::UInt, UpdateFlags_None),
        makeFastProperty<CameraComponent, TextureFilter, &CameraComponent::framebufferFilter>("framebufferFilter", PropertyType::Enum, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kAudioSourceProperties[] = {
        makeFastProperty<AudioSourceComponent, AudioSourceState, &AudioSourceComponent::state>("state", PropertyType::Enum, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, AssetRef, &AudioSourceComponent::filename>("filename", PropertyType::Asset, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, bool, &AudioSourceComponent::enableClocked>("enableClocked", PropertyType::Bool, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, double, &AudioSourceComponent::volume>("volume", PropertyType::Double, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, float, &AudioSourceComponent::speed>("speed", PropertyType::Float, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, float, &AudioSourceComponent::pan>("pan", PropertyType::Float, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, bool, &AudioSourceComponent::looping>("looping", PropertyType::Bool, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, double, &AudioSourceComponent::loopingPoint>("loopingPoint", PropertyType::Double, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, bool, &AudioSourceComponent::protectVoice>("protectVoice", PropertyType::Bool, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, bool, &AudioSourceComponent::inaudibleBehaviorMustTick>("inaudibleBehaviorMustTick", PropertyType::Bool, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, bool, &AudioSourceComponent::inaudibleBehaviorKill>("inaudibleBehaviorKill", PropertyType::Bool, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, float, &AudioSourceComponent::minDistance>("minDistance", PropertyType::Float, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, float, &AudioSourceComponent::maxDistance>("maxDistance", PropertyType::Float, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, AudioSourceAttenuation, &AudioSourceComponent::attenuationModel>("attenuationModel", PropertyType::Enum, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, float, &AudioSourceComponent::attenuationRolloffFactor>("attenuationRolloffFactor", PropertyType::Float, UpdateFlags_AudioSource),
        makeFastProperty<AudioSourceComponent, float, &AudioSourceComponent::dopplerFactor>("dopplerFactor", PropertyType::Float, UpdateFlags_AudioSource),
        makeFastPropertyNoDefault<AudioSourceComponent, double, &AudioSourceComponent::length>("length", PropertyType::Double, UpdateFlags_None),
        makeFastPropertyNoDefault<AudioSourceComponent, double, &AudioSourceComponent::playingTime>("playingTime", PropertyType::Double, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kSkyProperties[] = {
        makeFastProperty<SkyboxComponent, Texture, &SkyboxComponent::texture>("texture", PropertyType::Texture, UpdateFlags_Sky_Texture),
        makeFastProperty<SkyboxComponent, Vector4, &SkyboxComponent::color>("color", PropertyType::Vector4, UpdateFlags_None),
        makeFastProperty<SkyboxComponent, float, &SkyboxComponent::rotation>("rotation", PropertyType::Float, UpdateFlags_Sky),
        makeFastProperty<SkyboxComponent, bool, &SkyboxComponent::visible>("visible", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<SkyboxComponent, ShaderRef, &SkyboxComponent::customShader>("customShader", PropertyType::Shader, UpdateFlags_Shader_Reload),
        makeFastProperty<SkyboxComponent, ShaderUniformValues, &SkyboxComponent::shaderUniforms>("shaderUniforms", PropertyType::Custom, UpdateFlags_Shader_Uniforms),
        makeFastProperty<SkyboxComponent, std::vector<std::pair<std::string, Texture>>, &SkyboxComponent::shaderTextures>("shaderTextures", PropertyType::Custom, UpdateFlags_Shader_Reload),
        makeFastProperty<SkyboxComponent, std::vector<std::string>, &SkyboxComponent::shaderFeatures>("shaderFeatures", PropertyType::Custom, UpdateFlags_Shader_Reload),
    };

    static const FastPropertyDescriptor kTextProperties[] = {
        makeFastProperty<TextComponent, std::string, &TextComponent::text>("text", PropertyType::String, UpdateFlags_Text),
        makeFastProperty<TextComponent, FontArray, &TextComponent::font>("font", PropertyType::Font, UpdateFlags_Text_Atlas),
        makeFastProperty<TextComponent, unsigned int, &TextComponent::fontSize>("fontSize", PropertyType::UInt, UpdateFlags_Text_Atlas),
        makeFastProperty<TextComponent, bool, &TextComponent::multiline>("multiline", PropertyType::Bool, UpdateFlags_Text),
        makeFastProperty<TextComponent, unsigned int, &TextComponent::maxTextSize>("maxTextSize", PropertyType::UInt, UpdateFlags_Text),
        makeFastProperty<TextComponent, bool, &TextComponent::fixedWidth>("fixedWidth", PropertyType::Bool, UpdateFlags_Text),
        makeFastProperty<TextComponent, bool, &TextComponent::fixedHeight>("fixedHeight", PropertyType::Bool, UpdateFlags_Text),
        makeFastProperty<TextComponent, bool, &TextComponent::pivotBaseline>("pivotBaseline", PropertyType::Bool, UpdateFlags_Text),
        makeFastProperty<TextComponent, bool, &TextComponent::pivotCentered>("pivotCentered", PropertyType::Bool, UpdateFlags_Text),
    };

    static const FastPropertyDescriptor kJoint2DProperties[] = {
        makeFastProperty<Joint2DComponent, Joint2DType, &Joint2DComponent::type>("type", PropertyType::Enum, UpdateFlags_Joint2D),
        makeFastProperty<Joint2DComponent, Entity, &Joint2DComponent::bodyA>("bodyA", PropertyType::Entity, UpdateFlags_Joint2D),
        makeFastProperty<Joint2DComponent, Entity, &Joint2DComponent::bodyB>("bodyB", PropertyType::Entity, UpdateFlags_Joint2D),
        makeFastProperty<Joint2DComponent, Vector2, &Joint2DComponent::anchorA>("anchorA", PropertyType::Vector2, UpdateFlags_Joint2D),
        makeFastProperty<Joint2DComponent, Vector2, &Joint2DComponent::anchorB>("anchorB", PropertyType::Vector2, UpdateFlags_Joint2D),
        makeFastProperty<Joint2DComponent, Vector2, &Joint2DComponent::axis>("axis", PropertyType::Vector2, UpdateFlags_Joint2D),
        makeFastProperty<Joint2DComponent, Vector2, &Joint2DComponent::target>("target", PropertyType::Vector2, UpdateFlags_Joint2D),
        makeFastProperty<Joint2DComponent, bool, &Joint2DComponent::autoAnchors>("autoAnchors", PropertyType::Bool, UpdateFlags_Joint2D),
        makeFastProperty<Joint2DComponent, bool, &Joint2DComponent::rope>("rope", PropertyType::Bool, UpdateFlags_Joint2D),
    };

    static const FastPropertyDescriptor kJoint3DProperties[] = {
        makeFastProperty<Joint3DComponent, Joint3DType, &Joint3DComponent::type>("type", PropertyType::Enum, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Entity, &Joint3DComponent::bodyA>("bodyA", PropertyType::Entity, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Entity, &Joint3DComponent::bodyB>("bodyB", PropertyType::Entity, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::anchorA>("anchorA", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::anchorB>("anchorB", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::anchor>("anchor", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::axis>("axis", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::normal>("normal", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::twistAxis>("twistAxis", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::planeAxis>("planeAxis", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::axisX>("axisX", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::axisY>("axisY", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, float, &Joint3DComponent::limitsMin>("limitsMin", PropertyType::Float, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, float, &Joint3DComponent::limitsMax>("limitsMax", PropertyType::Float, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, float, &Joint3DComponent::normalHalfConeAngle>("normalHalfConeAngle", PropertyType::Float, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, float, &Joint3DComponent::planeHalfConeAngle>("planeHalfConeAngle", PropertyType::Float, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, float, &Joint3DComponent::twistMinAngle>("twistMinAngle", PropertyType::Float, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, float, &Joint3DComponent::twistMaxAngle>("twistMaxAngle", PropertyType::Float, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::fixedPointA>("fixedPointA", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::fixedPointB>("fixedPointB", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Entity, &Joint3DComponent::hingeA>("hingeA", PropertyType::Entity, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Entity, &Joint3DComponent::hingeB>("hingeB", PropertyType::Entity, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Entity, &Joint3DComponent::hinge>("hinge", PropertyType::Entity, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Entity, &Joint3DComponent::slider>("slider", PropertyType::Entity, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, int, &Joint3DComponent::numTeethGearA>("numTeethGearA", PropertyType::Int, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, int, &Joint3DComponent::numTeethGearB>("numTeethGearB", PropertyType::Int, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, int, &Joint3DComponent::numTeethRack>("numTeethRack", PropertyType::Int, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, int, &Joint3DComponent::numTeethGear>("numTeethGear", PropertyType::Int, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, int, &Joint3DComponent::rackLength>("rackLength", PropertyType::Int, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, Vector3, &Joint3DComponent::pathPosition>("pathPosition", PropertyType::Vector3, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, bool, &Joint3DComponent::isLooping>("isLooping", PropertyType::Bool, UpdateFlags_Joint3D),
        makeFastProperty<Joint3DComponent, bool, &Joint3DComponent::autoAnchors>("autoAnchors", PropertyType::Bool, UpdateFlags_Joint3D),
    };

    static const FastPropertyDescriptor kMeshTopProperties[] = {
        makeFastProperty<MeshComponent, bool, &MeshComponent::receiveLights>("receiveLights", PropertyType::Bool, UpdateFlags_Mesh_Reload),
        makeFastProperty<MeshComponent, bool, &MeshComponent::receiveIBL>("receiveIBL", PropertyType::Bool, UpdateFlags_Mesh_Reload),
        makeFastProperty<MeshComponent, bool, &MeshComponent::castShadows>("castShadows", PropertyType::Bool, UpdateFlags_Mesh_Reload),
        makeFastProperty<MeshComponent, bool, &MeshComponent::receiveShadows>("receiveShadows", PropertyType::Bool, UpdateFlags_Mesh_Reload),
        // which shadow layer it is cached in (RenderSystem's caster registry reads it every frame)
        makeFastProperty<MeshComponent, bool, &MeshComponent::staticShadowCaster>("staticShadowCaster", PropertyType::Bool, UpdateFlags_None),
        makeFastProperty<MeshComponent, bool, &MeshComponent::lodEnabled>("lodEnabled", PropertyType::Bool, UpdateFlags_Mesh_Reload),
        makeFastProperty<MeshComponent, float, &MeshComponent::lodBias>("lodBias", PropertyType::Float, UpdateFlags_None),
        makeFastProperty<MeshComponent, bool, &MeshComponent::renderInReflectionProbes>("renderInReflectionProbes", PropertyType::Bool, UpdateFlags_Reflection_Probe_Recapture_All),
        makeFastProperty<MeshComponent, bool, &MeshComponent::transparent>("transparent", PropertyType::Bool, UpdateFlags_Mesh_Reload),
        makeFastProperty<MeshComponent, bool, &MeshComponent::autoTransparency>("autoTransparency", PropertyType::Bool, UpdateFlags_Mesh_Reload),
        makeFastProperty<MeshComponent, CullingMode, &MeshComponent::cullingMode>("cullingMode", PropertyType::Enum, UpdateFlags_Mesh_Reload),
        makeFastProperty<MeshComponent, ShaderRef, &MeshComponent::customShader>("customShader", PropertyType::Shader, UpdateFlags_Shader_Reload),
        makeFastProperty<MeshComponent, ShaderRef, &MeshComponent::customDepthShader>("customDepthShader", PropertyType::Shader, UpdateFlags_Shader_Reload),
        makeFastProperty<MeshComponent, ShaderUniformValues, &MeshComponent::shaderUniforms>("shaderUniforms", PropertyType::Custom, UpdateFlags_Shader_Uniforms),
        makeFastProperty<MeshComponent, unsigned int, &MeshComponent::numSubmeshes>("numSubmeshes", PropertyType::UInt, UpdateFlags_None),
    };

    static const FastPropertyDescriptor kLayoutGroupTopProperties[] = {
        makeFastProperty<LayoutGroupComponent, LayoutGroupType, &LayoutGroupComponent::type>("type", PropertyType::Enum, UpdateFlags_Layout_Sizes),
        makeFastProperty<LayoutGroupComponent, bool, &LayoutGroupComponent::useAllWrapSpace>("useAllWrapSpace", PropertyType::Bool, UpdateFlags_Layout_Sizes),
        makeFastProperty<LayoutGroupComponent, unsigned int, &LayoutGroupComponent::wrapCellWidth>("wrapCellWidth", PropertyType::UInt, UpdateFlags_Layout_Sizes),
        makeFastProperty<LayoutGroupComponent, unsigned int, &LayoutGroupComponent::wrapCellHeight>("wrapCellHeight", PropertyType::UInt, UpdateFlags_Layout_Sizes),
        makeFastProperty<LayoutGroupComponent, unsigned int, &LayoutGroupComponent::numBoxes>("numBoxes", PropertyType::UInt, UpdateFlags_None),
    };

    PropertyData getMeshPropertyFast(MeshComponent* comp, const std::string& propertyName) {
        MeshComponent& def = getDefaultComponent<MeshComponent>();

        if (!comp) {
            return PropertyData();
        }

        // Flat properties
        PropertyData result = resolveDirectProperties(comp, propertyName, kMeshTopProperties);
        if (result.ref) return result;

        // morphWeights[N]: one blend shape weight, so a prefab instance overrides weights one by
        // one (the Inspector's Blend Shapes rows, animation clips T3c)
        if (propertyName.compare(0, 13, "morphWeights[") == 0) {
            size_t pos = 13;
            size_t weight = 0;
            if (!parseIndex(propertyName, pos, weight) || pos + 1 != propertyName.size() || propertyName[pos] != ']' ||
                weight >= (size_t)MAX_MORPHTARGETS) {
                return PropertyData();
            }
            static float defWeight = 0.0f;
            return {PropertyType::Float, UpdateFlags_None, (void*)&defWeight, (void*)&comp->morphWeights[weight]};
        }

        // Submeshes: submeshes[N].field
        if (propertyName.compare(0, 10, "submeshes[") != 0) {
            return PropertyData();
        }

        size_t pos = 10;
        size_t submeshIndex = 0;
        if (!parseIndex(propertyName, pos, submeshIndex) || pos >= propertyName.size() || propertyName[pos] != ']') {
            return PropertyData();
        }

        Submesh& sub = comp->submeshes[submeshIndex];
        Submesh& defSub = def.submeshes[0];

        pos++;
        if (pos == propertyName.size()) {
            return {PropertyType::Custom, UpdateFlags_Mesh_Texture | UpdateFlags_Mesh_Reload | UpdateFlags_Shader_Uniforms, (void*)&defSub, (void*)&sub};
        }
        if (propertyName[pos] != '.') {
            return PropertyData();
        }

        const size_t fieldPos = pos + 1;

        // submeshes[N].material
        if (propertyName.compare(fieldPos, 8, "material") == 0) {
            if (fieldPos + 8 == propertyName.size()) {
                // a whole material may bring another shader, features or render state
                return {PropertyType::Material, UpdateFlags_Mesh_Texture | UpdateFlags_Mesh_Reload | UpdateFlags_Shader_Uniforms, (void*)&defSub.material, (void*)&sub.material};
            }
            if (propertyName[fieldPos + 8] != '.') {
                return PropertyData();
            }
            const size_t matFieldPos = fieldPos + 9;
            if (propertyName.compare(matFieldPos, 4, "name") == 0 && matFieldPos + 4 == propertyName.size()) {
                return {PropertyType::Asset, UpdateFlags_None, (void*)&defSub.material.name, (void*)&sub.material.name};
            }
            if (propertyName.compare(matFieldPos, 15, "baseColorFactor") == 0 && matFieldPos + 15 == propertyName.size()) {
                return {PropertyType::Vector4, UpdateFlags_None, (void*)&defSub.material.baseColorFactor, (void*)&sub.material.baseColorFactor};
            }
            if (propertyName.compare(matFieldPos, 14, "metallicFactor") == 0 && matFieldPos + 14 == propertyName.size()) {
                return {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.metallicFactor, (void*)&sub.material.metallicFactor};
            }
            if (propertyName.compare(matFieldPos, 15, "roughnessFactor") == 0 && matFieldPos + 15 == propertyName.size()) {
                return {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.roughnessFactor, (void*)&sub.material.roughnessFactor};
            }
            if (propertyName.compare(matFieldPos, 11, "alphaCutoff") == 0 && matFieldPos + 11 == propertyName.size()) {
                return {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.alphaCutoff, (void*)&sub.material.alphaCutoff};
            }
            if (propertyName.compare(matFieldPos, 19, "foliageTransmission") == 0 && matFieldPos + 19 == propertyName.size()) {
                return {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.foliageTransmission, (void*)&sub.material.foliageTransmission};
            }
            if (propertyName.compare(matFieldPos, 9, "alphaMode") == 0 && matFieldPos + 9 == propertyName.size()) {
                return {PropertyType::Enum, UpdateFlags_Mesh_Reload, (void*)&defSub.material.alphaMode, (void*)&sub.material.alphaMode};
            }
            // Stochastic Tiling: the switch picks the variants, the values ride in the uniforms
            if (propertyName.compare(matFieldPos, 10, "stochastic") == 0) {
                const std::string field = propertyName.substr(matFieldPos);
                if (field == "stochasticTiling") return {PropertyType::Bool, UpdateFlags_Mesh_Reload, (void*)&defSub.material.stochasticTiling, (void*)&sub.material.stochasticTiling};
                if (field == "stochasticCellSize") return {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.stochasticCellSize, (void*)&sub.material.stochasticCellSize};
                if (field == "stochasticBlendSharpness") return {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.stochasticBlendSharpness, (void*)&sub.material.stochasticBlendSharpness};
                if (field == "stochasticContrast") return {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.stochasticContrast, (void*)&sub.material.stochasticContrast};
                if (field == "stochasticRotation") return {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.stochasticRotation, (void*)&sub.material.stochasticRotation};
                return PropertyData();
            }
            if (propertyName.compare(matFieldPos, 14, "emissiveFactor") == 0 && matFieldPos + 14 == propertyName.size()) {
                return {PropertyType::Vector3, UpdateFlags_None, (void*)&defSub.material.emissiveFactor, (void*)&sub.material.emissiveFactor};
            }
            if (propertyName.compare(matFieldPos, 16, "baseColorTexture") == 0 && matFieldPos + 16 == propertyName.size()) {
                return {PropertyType::Texture, UpdateFlags_Mesh_Texture, (void*)&defSub.material.baseColorTexture, (void*)&sub.material.baseColorTexture};
            }
            if (propertyName.compare(matFieldPos, 15, "emissiveTexture") == 0 && matFieldPos + 15 == propertyName.size()) {
                return {PropertyType::Texture, UpdateFlags_Mesh_Texture, (void*)&defSub.material.emissiveTexture, (void*)&sub.material.emissiveTexture};
            }
            if (propertyName.compare(matFieldPos, 24, "metallicRoughnessTexture") == 0 && matFieldPos + 24 == propertyName.size()) {
                return {PropertyType::Texture, UpdateFlags_Mesh_Texture, (void*)&defSub.material.metallicRoughnessTexture, (void*)&sub.material.metallicRoughnessTexture};
            }
            if (propertyName.compare(matFieldPos, 16, "occlusionTexture") == 0 && matFieldPos + 16 == propertyName.size()) {
                return {PropertyType::Texture, UpdateFlags_Mesh_Texture, (void*)&defSub.material.occlusionTexture, (void*)&sub.material.occlusionTexture};
            }
            if (propertyName.compare(matFieldPos, 13, "normalTexture") == 0 && matFieldPos + 13 == propertyName.size()) {
                return {PropertyType::Texture, UpdateFlags_Mesh_Texture, (void*)&defSub.material.normalTexture, (void*)&sub.material.normalTexture};
            }
            return getMaterialLibraryProperty(sub.material, &defSub.material, propertyName.substr(matFieldPos), true);
        }

        // submeshes[N].primitiveType, faceCulling, textureShadow, textureRect
        if (propertyName.compare(fieldPos, 13, "primitiveType") == 0 && fieldPos + 13 == propertyName.size()) {
            return {PropertyType::Enum, UpdateFlags_Mesh_Reload, (void*)&defSub.primitiveType, (void*)&sub.primitiveType};
        }
        if (propertyName.compare(fieldPos, 11, "faceCulling") == 0 && fieldPos + 11 == propertyName.size()) {
            return {PropertyType::Bool, UpdateFlags_Mesh_Reload, (void*)&defSub.faceCulling, (void*)&sub.faceCulling};
        }
        if (propertyName.compare(fieldPos, 13, "textureShadow") == 0 && fieldPos + 13 == propertyName.size()) {
            return {PropertyType::Bool, UpdateFlags_Mesh_Reload, (void*)&defSub.textureShadow, (void*)&sub.textureShadow};
        }
        if (propertyName.compare(fieldPos, 11, "textureRect") == 0 && fieldPos + 11 == propertyName.size()) {
            return {PropertyType::Vector4, UpdateFlags_None, (void*)&defSub.textureRect, (void*)&sub.textureRect};
        }

        return PropertyData();
    }

    PropertyData getLayoutGroupPropertyFast(LayoutGroupComponent* comp, const std::string& propertyName) {
        LayoutGroupComponent& def = getDefaultComponent<LayoutGroupComponent>();

        if (!comp) {
            return PropertyData();
        }

        // Flat properties
        PropertyData result = resolveDirectProperties(comp, propertyName, kLayoutGroupTopProperties);
        if (result.ref) return result;

        // boxes[N].field
        if (propertyName.compare(0, 6, "boxes[") != 0) {
            return PropertyData();
        }

        size_t pos = 6;
        size_t boxIndex = 0;
        if (!parseIndex(propertyName, pos, boxIndex) || pos >= propertyName.size() || propertyName[pos] != ']') {
            return PropertyData();
        }
        if (boxIndex >= MAX_CONTAINER_BOXES) {
            return PropertyData();
        }

        ContainerBox& box = comp->boxes[boxIndex];
        ContainerBox& defBox = def.boxes[0];

        pos++;
        if (pos == propertyName.size()) {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&defBox, (void*)&box};
        }
        if (propertyName[pos] != '.') {
            return PropertyData();
        }

        const size_t fieldPos = pos + 1;

        if (propertyName.compare(fieldPos, 6, "layout") == 0 && fieldPos + 6 == propertyName.size()) {
            return {PropertyType::UInt, UpdateFlags_None, (void*)&defBox.layout, (void*)&box.layout};
        }
        if (propertyName.compare(fieldPos, 6, "expand") == 0 && fieldPos + 6 == propertyName.size()) {
            return {PropertyType::Bool, UpdateFlags_Layout_Sizes, (void*)&defBox.expand, (void*)&box.expand};
        }
        if (propertyName.compare(fieldPos, 6, "rect.x") == 0 && fieldPos + 6 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_None, (void*)&defBox.rect.ptr()[0], (void*)&box.rect.ptr()[0]};
        }
        if (propertyName.compare(fieldPos, 6, "rect.y") == 0 && fieldPos + 6 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_None, (void*)&defBox.rect.ptr()[1], (void*)&box.rect.ptr()[1]};
        }
        if (propertyName.compare(fieldPos, 10, "rect.width") == 0 && fieldPos + 10 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_None, (void*)&defBox.rect.ptr()[2], (void*)&box.rect.ptr()[2]};
        }
        if (propertyName.compare(fieldPos, 11, "rect.height") == 0 && fieldPos + 11 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_None, (void*)&defBox.rect.ptr()[3], (void*)&box.rect.ptr()[3]};
        }

        return PropertyData();
    }

    PropertyData getScriptPropertyFast(ScriptComponent* comp, const std::string& propertyName) {
        if (!comp) {
            return PropertyData();
        }

        if (propertyName == "scripts") {
            return {PropertyType::Custom, UpdateFlags_None, nullptr, (void*)&comp->scripts};
        }

        // Parse "scripts[N].fieldName"
        if (propertyName.compare(0, 8, "scripts[") != 0) {
            return PropertyData();
        }

        size_t pos = 8;
        size_t index = 0;
        if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
            return PropertyData();
        }
        pos++;
        if (pos >= propertyName.size() || propertyName[pos] != '.') {
            return PropertyData();
        }
        pos++;

        if (index >= comp->scripts.size()) {
            return PropertyData();
        }
        auto& scriptEntry = comp->scripts[index];

        std::string fieldName = propertyName.substr(pos);

        if (fieldName == "enabled") {
            return {PropertyType::Bool, UpdateFlags_None, nullptr, (void*)&scriptEntry.enabled};
        }

        for (auto& prop : scriptEntry.properties) {
            if (prop.name == fieldName) {
                PropertyData result;
                result.type = Catalog::scriptPropertyTypeToPropertyType(prop.type);
                result.updateFlags = UpdateFlags_None;

                switch (prop.type) {
                    case ScriptPropertyType::Bool:
                        result.ref = &prop.valueRef<bool>();
                        result.def = &prop.defaultValueRef<bool>();
                        break;
                    case ScriptPropertyType::Int:
                        result.ref = &prop.valueRef<int>();
                        result.def = &prop.defaultValueRef<int>();
                        break;
                    case ScriptPropertyType::Float:
                        result.ref = &prop.valueRef<float>();
                        result.def = &prop.defaultValueRef<float>();
                        break;
                    case ScriptPropertyType::String:
                        result.ref = &prop.valueRef<std::string>();
                        result.def = &prop.defaultValueRef<std::string>();
                        break;
                    case ScriptPropertyType::Vector2:
                        result.ref = &prop.valueRef<Vector2>();
                        result.def = &prop.defaultValueRef<Vector2>();
                        break;
                    case ScriptPropertyType::Vector3:
                    case ScriptPropertyType::Color3:
                        result.ref = &prop.valueRef<Vector3>();
                        result.def = &prop.defaultValueRef<Vector3>();
                        break;
                    case ScriptPropertyType::Vector4:
                    case ScriptPropertyType::Color4:
                        result.ref = &prop.valueRef<Vector4>();
                        result.def = &prop.defaultValueRef<Vector4>();
                        break;
                    case ScriptPropertyType::EntityReference:
                        result.ref = &prop.valueRef<EntityReference>().entity;
                        result.def = &prop.defaultValueRef<EntityReference>().entity;
                        break;
                    case ScriptPropertyType::Prefab:
                        result.ref = &prop.valueRef<PrefabAsset>();
                        result.def = &prop.defaultValueRef<PrefabAsset>();
                        break;
                    case ScriptPropertyType::Asset:
                        result.ref = &prop.valueRef<AssetRef>();
                        result.def = &prop.defaultValueRef<AssetRef>();
                        break;
                    default:
                        result.ref = nullptr;
                        result.def = nullptr;
                        break;
                }
                return result;
            }

            // Handle "propName.sceneId" for EntityReference properties
            if (prop.type == ScriptPropertyType::EntityReference) {
                std::string sceneIdField = prop.name + ".sceneId";
                if (fieldName == sceneIdField) {
                    static uint32_t defSceneId = 0;
                    return {PropertyType::UInt, UpdateFlags_None, (void*)&defSceneId, (void*)&prop.valueRef<EntityReference>().sceneId};
                }
            }
        }

        return PropertyData();
    }

    PropertyData getRigidbody2DPropertyFast(Rigidbody2DComponent* comp, const std::string& propertyName) {
        Rigidbody2DComponent& defaultRigidbody2D = getDefaultComponent<Rigidbody2DComponent>();

        if (!comp) {
            return PropertyData();
        }

        if (propertyName == "type") {
            return {PropertyType::Enum, UpdateFlags_Rigidbody2D, (void*)&defaultRigidbody2D.type, (void*)&comp->type};
        }
        if (propertyName == "numColliders") {
            return {PropertyType::UInt, UpdateFlags_Rigidbody2D, (void*)&defaultRigidbody2D.numColliders, (void*)&comp->numColliders};
        }

        constexpr const char* shapePrefix = "colliders[";
        if (propertyName.compare(0, 10, shapePrefix) != 0) {
            return PropertyData();
        }

        size_t pos = 10;
        size_t colliderIndex = 0;
        if (!parseIndex(propertyName, pos, colliderIndex) || pos >= propertyName.size() || propertyName[pos] != ']') {
            return PropertyData();
        }

        if (colliderIndex >= MAX_COLLIDERS_2D) {
            return PropertyData();
        }
        Collider2D& shape = comp->colliders[colliderIndex];
        Collider2D& defaultShape = defaultRigidbody2D.colliders[0];

        pos++;
        if (pos == propertyName.size()) {
            return {PropertyType::Custom, UpdateFlags_Rigidbody2D, (void*)&defaultShape, (void*)&shape};
        }

        if (propertyName[pos] != '.') {
            return PropertyData();
        }

        const size_t fieldPos = pos + 1;
        if (propertyName.compare(fieldPos, 4, "type") == 0 && fieldPos + 4 == propertyName.size()) {
            return {PropertyType::Enum, UpdateFlags_Rigidbody2D, (void*)&defaultShape.type, (void*)&shape.type};
        }
        if (propertyName.compare(fieldPos, 6, "pointA") == 0 && fieldPos + 6 == propertyName.size()) {
            return {PropertyType::Vector2, UpdateFlags_Rigidbody2D, (void*)&defaultShape.pointA, (void*)&shape.pointA};
        }
        if (propertyName.compare(fieldPos, 6, "pointB") == 0 && fieldPos + 6 == propertyName.size()) {
            return {PropertyType::Vector2, UpdateFlags_Rigidbody2D, (void*)&defaultShape.pointB, (void*)&shape.pointB};
        }
        if (propertyName.compare(fieldPos, 6, "radius") == 0 && fieldPos + 6 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody2D, (void*)&defaultShape.radius, (void*)&shape.radius};
        }
        if (propertyName.compare(fieldPos, 4, "loop") == 0 && fieldPos + 4 == propertyName.size()) {
            return {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defaultShape.loop, (void*)&shape.loop};
        }
        if (propertyName.compare(fieldPos, 7, "density") == 0 && fieldPos + 7 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody2D, (void*)&defaultShape.density, (void*)&shape.density};
        }
        if (propertyName.compare(fieldPos, 8, "friction") == 0 && fieldPos + 8 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody2D, (void*)&defaultShape.friction, (void*)&shape.friction};
        }
        if (propertyName.compare(fieldPos, 11, "restitution") == 0 && fieldPos + 11 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody2D, (void*)&defaultShape.restitution, (void*)&shape.restitution};
        }
        if (propertyName.compare(fieldPos, 6, "sensor") == 0 && fieldPos + 6 == propertyName.size()) {
            return {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defaultShape.sensor, (void*)&shape.sensor};
        }
        if (propertyName.compare(fieldPos, 15, "enableHitEvents") == 0 && fieldPos + 15 == propertyName.size()) {
            return {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defaultShape.enableHitEvents, (void*)&shape.enableHitEvents};
        }
        if (propertyName.compare(fieldPos, 13, "contactEvents") == 0 && fieldPos + 13 == propertyName.size()) {
            return {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defaultShape.contactEvents, (void*)&shape.contactEvents};
        }
        if (propertyName.compare(fieldPos, 14, "preSolveEvents") == 0 && fieldPos + 14 == propertyName.size()) {
            return {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defaultShape.preSolveEvents, (void*)&shape.preSolveEvents};
        }
        if (propertyName.compare(fieldPos, 12, "sensorEvents") == 0 && fieldPos + 12 == propertyName.size()) {
            return {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defaultShape.sensorEvents, (void*)&shape.sensorEvents};
        }

        constexpr const char* verticesPrefix = "vertices[";
        if (propertyName.compare(fieldPos, 9, verticesPrefix) == 0) {
            size_t vertexPos = fieldPos + 9;
            size_t vertexIndex = 0;
            if (!parseIndex(propertyName, vertexPos, vertexIndex) || vertexPos >= propertyName.size() || propertyName[vertexPos] != ']') {
                return PropertyData();
            }

            if (vertexPos + 1 != propertyName.size()) {
                return PropertyData();
            }

            return {PropertyType::Vector2, UpdateFlags_Rigidbody2D, (void*)&defaultShape.vertices[0], (void*)&shape.vertices[vertexIndex]};
        }

        return PropertyData();
    }

    jasperengine::editor::PropertyData getRigidbodyPropertyFast(jasperengine::RigidbodyComponent* comp, const std::string& propertyName) {
        RigidbodyComponent& defaultRigidbody = getDefaultComponent<RigidbodyComponent>();

        if (!comp) {
            return PropertyData();
        }

        if (propertyName == "type") {
            return {PropertyType::Enum, UpdateFlags_Rigidbody, (void*)&defaultRigidbody.type, (void*)&comp->type};
        }
        if (propertyName == "motionQuality") {
            return {PropertyType::Enum, UpdateFlags_Rigidbody, (void*)&defaultRigidbody.motionQuality, (void*)&comp->motionQuality};
        }
        if (propertyName == "sensor") {
            return {PropertyType::Bool, UpdateFlags_Rigidbody, (void*)&defaultRigidbody.sensor, (void*)&comp->sensor};
        }
        if (propertyName == "gravityFactor") {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultRigidbody.gravityFactor, (void*)&comp->gravityFactor};
        }
        if (propertyName == "friction") {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultRigidbody.friction, (void*)&comp->friction};
        }
        if (propertyName == "restitution") {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultRigidbody.restitution, (void*)&comp->restitution};
        }
        // read by each fixed step (Water::applyBuoyancy): the body stays as it is
        if (propertyName == "buoyancy") {
            return {PropertyType::Float, UpdateFlags_None, (void*)&defaultRigidbody.buoyancy, (void*)&comp->buoyancy};
        }
        if (propertyName == "waterDrag") {
            return {PropertyType::Float, UpdateFlags_None, (void*)&defaultRigidbody.waterDrag, (void*)&comp->waterDrag};
        }
        if (propertyName == "waterAngularDrag") {
            return {PropertyType::Float, UpdateFlags_None, (void*)&defaultRigidbody.waterAngularDrag, (void*)&comp->waterAngularDrag};
        }
        if (propertyName == "numColliders") {
            return {PropertyType::UInt, UpdateFlags_Rigidbody, (void*)&defaultRigidbody.numColliders, (void*)&comp->numColliders};
        }
        if (propertyName == "overrideMassProperties") {
            return {PropertyType::Bool, UpdateFlags_Rigidbody, (void*)&defaultRigidbody.overrideMassProperties, (void*)&comp->overrideMassProperties};
        }
        if (propertyName == "solidBoxSize") {
            return {PropertyType::Vector3, UpdateFlags_Rigidbody, (void*)&defaultRigidbody.solidBoxSize, (void*)&comp->solidBoxSize};
        }
        if (propertyName == "solidBoxDensity") {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultRigidbody.solidBoxDensity, (void*)&comp->solidBoxDensity};
        }

        constexpr const char* shapePrefix = "colliders[";
        if (propertyName.compare(0, 10, shapePrefix) != 0) {
            return PropertyData();
        }

        size_t pos = 10;
        size_t colliderIndex = 0;
        if (!parseIndex(propertyName, pos, colliderIndex) || pos >= propertyName.size() || propertyName[pos] != ']') {
            return PropertyData();
        }

        if (colliderIndex >= MAX_COLLIDERS_3D) {
            return PropertyData();
        }
        Collider3D& shape = comp->colliders[colliderIndex];
        Collider3D& defaultShape = defaultRigidbody.colliders[0];

        pos++;
        if (pos == propertyName.size()) {
            return {PropertyType::Custom, UpdateFlags_Rigidbody, (void*)&defaultShape, (void*)&shape};
        }

        if (propertyName[pos] != '.') {
            return PropertyData();
        }

        const size_t fieldPos = pos + 1;

        if (propertyName.compare(fieldPos, 4, "type") == 0 && fieldPos + 4 == propertyName.size()) {
            return {PropertyType::Enum, UpdateFlags_Rigidbody, (void*)&defaultShape.type, (void*)&shape.type};
        }
        if (propertyName.compare(fieldPos, 8, "position") == 0 && fieldPos + 8 == propertyName.size()) {
            return {PropertyType::Vector3, UpdateFlags_Rigidbody, (void*)&defaultShape.position, (void*)&shape.position};
        }
        if (propertyName.compare(fieldPos, 8, "rotation") == 0 && fieldPos + 8 == propertyName.size()) {
            return {PropertyType::Quat, UpdateFlags_Rigidbody, (void*)&defaultShape.rotation, (void*)&shape.rotation};
        }
        if (propertyName.compare(fieldPos, 5, "width") == 0 && fieldPos + 5 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultShape.width, (void*)&shape.width};
        }
        if (propertyName.compare(fieldPos, 6, "height") == 0 && fieldPos + 6 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultShape.height, (void*)&shape.height};
        }
        if (propertyName.compare(fieldPos, 5, "depth") == 0 && fieldPos + 5 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultShape.depth, (void*)&shape.depth};
        }
        if (propertyName.compare(fieldPos, 6, "radius") == 0 && fieldPos + 6 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultShape.radius, (void*)&shape.radius};
        }
        if (propertyName.compare(fieldPos, 10, "halfHeight") == 0 && fieldPos + 10 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultShape.halfHeight, (void*)&shape.halfHeight};
        }
        if (propertyName.compare(fieldPos, 9, "topRadius") == 0 && fieldPos + 9 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultShape.topRadius, (void*)&shape.topRadius};
        }
        if (propertyName.compare(fieldPos, 12, "bottomRadius") == 0 && fieldPos + 12 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultShape.bottomRadius, (void*)&shape.bottomRadius};
        }
        if (propertyName.compare(fieldPos, 7, "density") == 0 && fieldPos + 7 == propertyName.size()) {
            return {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defaultShape.density, (void*)&shape.density};
        }
        if (propertyName.compare(fieldPos, 6, "source") == 0 && fieldPos + 6 == propertyName.size()) {
            return {PropertyType::Enum, UpdateFlags_Rigidbody, (void*)&defaultShape.source, (void*)&shape.source};
        }
        if (propertyName.compare(fieldPos, 12, "sourceEntity") == 0 && fieldPos + 12 == propertyName.size()) {
            return {PropertyType::Entity, UpdateFlags_Rigidbody, (void*)&defaultShape.sourceEntity, (void*)&shape.sourceEntity};
        }
        if (propertyName.compare(fieldPos, 11, "samplesSize") == 0 && fieldPos + 11 == propertyName.size()) {
            return {PropertyType::UInt, UpdateFlags_Rigidbody, (void*)&defaultShape.samplesSize, (void*)&shape.samplesSize};
        }
        if (propertyName.compare(fieldPos, 11, "numVertices") == 0 && fieldPos + 11 == propertyName.size()) {
            return {PropertyType::UInt, UpdateFlags_Rigidbody, (void*)&defaultShape.numVertices, (void*)&shape.numVertices};
        }
        if (propertyName.compare(fieldPos, 10, "numIndices") == 0 && fieldPos + 10 == propertyName.size()) {
            return {PropertyType::UInt, UpdateFlags_Rigidbody, (void*)&defaultShape.numIndices, (void*)&shape.numIndices};
        }

        constexpr const char* verticesPrefix = "vertices[";
        if (propertyName.compare(fieldPos, 9, verticesPrefix) == 0) {
            size_t vertexPos = fieldPos + 9;
            size_t vertexIndex = 0;
            if (!parseIndex(propertyName, vertexPos, vertexIndex) || vertexPos >= propertyName.size() || propertyName[vertexPos] != ']') {
                return PropertyData();
            }

            if (vertexPos + 1 != propertyName.size()) {
                return PropertyData();
            }

            return {PropertyType::Vector3, UpdateFlags_Rigidbody, (void*)&defaultShape.vertices[0], (void*)&shape.vertices[vertexIndex]};
        }

        return PropertyData();
    }

    // ── Resolve wrappers (single-property lookup) ──

    PropertyData resolveTransformPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<Transform*>(comp), propertyName, kTransformProperties);
    }

    PropertyData resolveUIPropertyFast(void* comp, const std::string& propertyName) {
        PropertyData property = resolveDirectProperties(static_cast<UIComponent*>(comp), propertyName, kUIProperties);
        if (property.ref || !comp) return property;
        // phase 2: the stencil and colour write of its pipeline
        UIComponent& def = getDefaultComponent<UIComponent>();
        UIComponent* ui = static_cast<UIComponent*>(comp);
        return getStencilColorWriteProperty(&ui->stencil, &def.stencil, &ui->colorWrite, &def.colorWrite, propertyName, UpdateFlags_Shader_Reload);
    }

    PropertyData resolveRectTransformPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<RectTransformComponent*>(comp), propertyName, kRectTransformProperties);
    }

    PropertyData resolveImagePropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<ImageComponent*>(comp), propertyName, kImageProperties);
    }

    PropertyData resolveButtonPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<ButtonComponent*>(comp), propertyName, kButtonProperties);
    }

    PropertyData resolveScrollbarPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<ScrollbarComponent*>(comp), propertyName, kScrollbarProperties);
    }

    PropertyData resolveProgressbarPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<ProgressbarComponent*>(comp), propertyName, kProgressbarProperties);
    }

    PropertyData resolveInputFieldPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<InputFieldComponent*>(comp), propertyName, kInputFieldProperties);
    }

    PropertyData resolvePanelPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<PanelComponent*>(comp), propertyName, kPanelProperties);
    }

    PropertyData getSpritePropertyFast(SpriteComponent* comp, const std::string& propertyName) {
        SpriteComponent& def = getDefaultComponent<SpriteComponent>();

        if (!comp) {
            return PropertyData();
        }

        // Flat properties
        PropertyData result = resolveDirectProperties(comp, propertyName, kSpriteTopProperties);
        if (result.ref) return result;

        // numFramesRect
        if (propertyName == "numFramesRect") {
            // Frame definitions are animation metadata. They do not change the
            // sprite quad, so rebuilding it here only causes the texture to be
            // loaded again after applying a large slice operation.
            return {PropertyType::UInt, UpdateFlags_None, (void*)&def.numFramesRect, (void*)&comp->numFramesRect};
        }

        // framesRect[N].field
        if (propertyName.compare(0, 11, "framesRect[") != 0) {
            return PropertyData();
        }

        size_t pos = 11;
        size_t frameIndex = 0;
        if (!parseIndex(propertyName, pos, frameIndex) || pos >= propertyName.size() || propertyName[pos] != ']') {
            return PropertyData();
        }

        SpriteFrameData& frame = comp->framesRect[frameIndex];
        SpriteFrameData& defFrame = def.framesRect[0];

        pos++;
        if (pos == propertyName.size()) {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&defFrame, (void*)&frame};
        }
        if (propertyName[pos] != '.') {
            return PropertyData();
        }

        const size_t fieldPos = pos + 1;

        if (propertyName.compare(fieldPos, 4, "name") == 0 && fieldPos + 4 == propertyName.size()) {
            return {PropertyType::String, UpdateFlags_None, (void*)&defFrame.name, (void*)&frame.name};
        }
        if (propertyName.compare(fieldPos, 4, "rect") == 0 && fieldPos + 4 == propertyName.size()) {
            return {PropertyType::Vector4, UpdateFlags_None, (void*)&defFrame.rect, (void*)&frame.rect};
        }

        return PropertyData();
    }

    PropertyData resolveSpritePropertyFast(void* comp, const std::string& propertyName) {
        return getSpritePropertyFast(static_cast<SpriteComponent*>(comp), propertyName);
    }

    PropertyData getTilemapPropertyFast(TilemapComponent* comp, const std::string& propertyName) {
        TilemapComponent& def = getDefaultComponent<TilemapComponent>();

        if (!comp) {
            return PropertyData();
        }

        // Flat properties
        PropertyData result = resolveDirectProperties(comp, propertyName, kTilemapTopProperties);
        if (result.ref) return result;

        // numTilesRect
        if (propertyName == "numTilesRect") {
            return {PropertyType::UInt, UpdateFlags_Tilemap, (void*)&def.numTilesRect, (void*)&comp->numTilesRect};
        }

        // numTiles
        if (propertyName == "numTiles") {
            return {PropertyType::UInt, UpdateFlags_Tilemap, (void*)&def.numTiles, (void*)&comp->numTiles};
        }

        // tilesRect[N].field
        if (propertyName.compare(0, 10, "tilesRect[") == 0) {
            size_t pos = 10;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }

            TileRectData& tileRect = comp->tilesRect[index];
            TileRectData& defTileRect = def.tilesRect[0];

            pos++;
            if (pos == propertyName.size()) {
                return {PropertyType::Custom, UpdateFlags_Tilemap, (void*)&defTileRect, (void*)&tileRect};
            }
            if (propertyName[pos] != '.') {
                return PropertyData();
            }

            const size_t fieldPos = pos + 1;

            if (propertyName.compare(fieldPos, 4, "name") == 0 && fieldPos + 4 == propertyName.size()) {
                return {PropertyType::String, UpdateFlags_Tilemap, (void*)&defTileRect.name, (void*)&tileRect.name};
            }
            if (propertyName.compare(fieldPos, 9, "submeshId") == 0 && fieldPos + 9 == propertyName.size()) {
                return {PropertyType::Int, UpdateFlags_Tilemap, (void*)&defTileRect.submeshId, (void*)&tileRect.submeshId};
            }
            if (propertyName.compare(fieldPos, 4, "rect") == 0 && fieldPos + 4 == propertyName.size()) {
                return {PropertyType::Vector4, UpdateFlags_Tilemap, (void*)&defTileRect.rect, (void*)&tileRect.rect};
            }

            return PropertyData();
        }

        // tiles[N].field
        if (propertyName.compare(0, 6, "tiles[") == 0) {
            size_t pos = 6;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }

            TileData& tile = comp->tiles[index];
            TileData& defTile = def.tiles[0];

            pos++;
            if (pos == propertyName.size()) {
                return {PropertyType::Custom, UpdateFlags_Tilemap, (void*)&defTile, (void*)&tile};
            }
            if (propertyName[pos] != '.') {
                return PropertyData();
            }

            const size_t fieldPos = pos + 1;

            if (propertyName.compare(fieldPos, 4, "name") == 0 && fieldPos + 4 == propertyName.size()) {
                return {PropertyType::String, UpdateFlags_Tilemap, (void*)&defTile.name, (void*)&tile.name};
            }
            if (propertyName.compare(fieldPos, 6, "rectId") == 0 && fieldPos + 6 == propertyName.size()) {
                return {PropertyType::Int, UpdateFlags_Tilemap, (void*)&defTile.rectId, (void*)&tile.rectId};
            }
            if (propertyName.compare(fieldPos, 8, "position") == 0 && fieldPos + 8 == propertyName.size()) {
                return {PropertyType::Vector2, UpdateFlags_Tilemap, (void*)&defTile.position, (void*)&tile.position};
            }
            if (propertyName.compare(fieldPos, 5, "width") == 0 && fieldPos + 5 == propertyName.size()) {
                return {PropertyType::Float, UpdateFlags_Tilemap, (void*)&defTile.width, (void*)&tile.width};
            }
            if (propertyName.compare(fieldPos, 6, "height") == 0 && fieldPos + 6 == propertyName.size()) {
                return {PropertyType::Float, UpdateFlags_Tilemap, (void*)&defTile.height, (void*)&tile.height};
            }

            return PropertyData();
        }

        return PropertyData();
    }

    PropertyData resolveTilemapPropertyFast(void* comp, const std::string& propertyName) {
        return getTilemapPropertyFast(static_cast<TilemapComponent*>(comp), propertyName);
    }

    PropertyData getTerrainFoliageLayerPropertyFast(TerrainFoliageLayer& layer, const std::string& fieldName) {
        static TerrainFoliageLayer def;
        if (fieldName == ".meshPath") return {PropertyType::Asset, UpdateFlags_Terrain_Foliage, &def.meshPath, &layer.meshPath};
        if (fieldName == ".densityMap") return {PropertyType::Texture, UpdateFlags_Terrain_Foliage, &def.densityMap, &layer.densityMap};
        if (fieldName == ".density") return {PropertyType::Float, UpdateFlags_Terrain_Foliage, &def.density, &layer.density};
        if (fieldName == ".minScale") return {PropertyType::Float, UpdateFlags_Terrain_Foliage, &def.minScale, &layer.minScale};
        if (fieldName == ".maxScale") return {PropertyType::Float, UpdateFlags_Terrain_Foliage, &def.maxScale, &layer.maxScale};
        if (fieldName == ".rotationJitter") return {PropertyType::Float, UpdateFlags_Terrain_Foliage, &def.rotationJitter, &layer.rotationJitter};
        if (fieldName == ".alignToNormal") return {PropertyType::Float, UpdateFlags_Terrain_Foliage, &def.alignToNormal, &layer.alignToNormal};
        if (fieldName == ".minSlope") return {PropertyType::Float, UpdateFlags_Terrain_Foliage, &def.minSlope, &layer.minSlope};
        if (fieldName == ".maxSlope") return {PropertyType::Float, UpdateFlags_Terrain_Foliage, &def.maxSlope, &layer.maxSlope};
        if (fieldName == ".minHeight") return {PropertyType::Float, UpdateFlags_Terrain_Foliage, &def.minHeight, &layer.minHeight};
        if (fieldName == ".maxHeight") return {PropertyType::Float, UpdateFlags_Terrain_Foliage, &def.maxHeight, &layer.maxHeight};
        if (fieldName == ".drawDistance") return {PropertyType::Float, UpdateFlags_Terrain_Foliage, &def.drawDistance, &layer.drawDistance};
        if (fieldName == ".seed") return {PropertyType::UInt, UpdateFlags_Terrain_Foliage, &def.seed, &layer.seed};
        if (fieldName == ".castShadows") return {PropertyType::Bool, UpdateFlags_Terrain_Foliage, &def.castShadows, &layer.castShadows};

        return PropertyData();
    }

    PropertyData getTerrainSurfaceLayerPropertyFast(TerrainSurfaceLayer& layer, const std::string& fieldName) {
        static TerrainSurfaceLayer def;
        if (fieldName == ".pbr") return {PropertyType::Bool, UpdateFlags_Terrain_Texture, &def.pbr, &layer.pbr};
        if (fieldName == ".colorTexture") return {PropertyType::Texture, UpdateFlags_Terrain_Texture, &def.colorTexture, &layer.colorTexture};
        if (fieldName == ".normalTexture") return {PropertyType::Texture, UpdateFlags_Terrain_Texture, &def.normalTexture, &layer.normalTexture};
        if (fieldName == ".roughnessTexture") return {PropertyType::Texture, UpdateFlags_Terrain_Texture, &def.roughnessTexture, &layer.roughnessTexture};
        if (fieldName == ".metallicTexture") return {PropertyType::Texture, UpdateFlags_Terrain_Texture, &def.metallicTexture, &layer.metallicTexture};
        if (fieldName == ".occlusionTexture") return {PropertyType::Texture, UpdateFlags_Terrain_Texture, &def.occlusionTexture, &layer.occlusionTexture};
        if (fieldName == ".heightTexture") return {PropertyType::Texture, UpdateFlags_Terrain_Texture, &def.heightTexture, &layer.heightTexture};
        if (fieldName == ".colorFactor") return {PropertyType::Vector4, UpdateFlags_Terrain_Texture, &def.colorFactor, &layer.colorFactor};
        if (fieldName == ".normalStrength") return {PropertyType::Float, UpdateFlags_Terrain_Texture, &def.normalStrength, &layer.normalStrength};
        if (fieldName == ".roughnessFactor") return {PropertyType::Float, UpdateFlags_Terrain_Texture, &def.roughnessFactor, &layer.roughnessFactor};
        if (fieldName == ".metallicFactor") return {PropertyType::Float, UpdateFlags_Terrain_Texture, &def.metallicFactor, &layer.metallicFactor};
        if (fieldName == ".occlusionStrength") return {PropertyType::Float, UpdateFlags_Terrain_Texture, &def.occlusionStrength, &layer.occlusionStrength};
        if (fieldName == ".uvScale") return {PropertyType::Vector2, UpdateFlags_Terrain_Texture, &def.uvScale, &layer.uvScale};
        if (fieldName == ".uvOffset") return {PropertyType::Vector2, UpdateFlags_Terrain_Texture, &def.uvOffset, &layer.uvOffset};
        if (fieldName == ".stochasticTiling") return {PropertyType::Bool, UpdateFlags_Terrain_Texture, &def.stochasticTiling, &layer.stochasticTiling};
        if (fieldName == ".stochasticCellSize") return {PropertyType::Float, UpdateFlags_Terrain_Texture, &def.stochasticCellSize, &layer.stochasticCellSize};
        if (fieldName == ".stochasticBlendSharpness") return {PropertyType::Float, UpdateFlags_Terrain_Texture, &def.stochasticBlendSharpness, &layer.stochasticBlendSharpness};
        if (fieldName == ".stochasticContrast") return {PropertyType::Float, UpdateFlags_Terrain_Texture, &def.stochasticContrast, &layer.stochasticContrast};
        if (fieldName == ".stochasticRotation") return {PropertyType::Float, UpdateFlags_Terrain_Texture, &def.stochasticRotation, &layer.stochasticRotation};
        // hex tiling: a variant define and a uniform, which RenderSystem follows on its own
        if (fieldName == ".hexTiling") return {PropertyType::Bool, UpdateFlags_None, &def.hexTiling, &layer.hexTiling};
        if (fieldName == ".hexRotation") return {PropertyType::Float, UpdateFlags_None, &def.hexRotation, &layer.hexRotation};
        return PropertyData();
    }

    PropertyData getTerrainTexturePropertyFast(std::vector<Texture>& textures, const std::string& propertyName, size_t pos) {
        size_t index = 0;
        if (!parseIndex(propertyName, pos, index) || pos + 1 != propertyName.size() || propertyName[pos] != ']') {
            return PropertyData();
        }
        if (index >= textures.size()) {
            return PropertyData();
        }
        static Texture defTexture;
        return {PropertyType::Texture, UpdateFlags_Terrain_Texture, (void*)&defTexture, (void*)&textures[index]};
    }

    PropertyData resolveTerrainPropertyFast(void* compRef, const std::string& propertyName) {
        TerrainComponent* comp = static_cast<TerrainComponent*>(compRef);
        if (!comp) return PropertyData();

        PropertyData result = resolveDirectProperties(comp, propertyName, kTerrainProperties);
        if (result.ref) return result;

        TerrainComponent& def = getDefaultComponent<TerrainComponent>();

        if (propertyName == "ranges") {
            return {PropertyType::Custom, UpdateFlags_Terrain, (void*)&def.ranges, (void*)&comp->ranges};
        }

        if (propertyName.compare(0, 7, "ranges[") == 0) {
            size_t pos = 7;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            if (pos + 1 != propertyName.size() || index >= comp->ranges.size()) {
                return PropertyData();
            }
            static float defValue = 0.0f;
            return {PropertyType::Float, UpdateFlags_Terrain, (void*)&defValue, (void*)&comp->ranges[index]};
        }

        if (propertyName == "blendMaps") {
            return {PropertyType::Custom, UpdateFlags_Terrain_Texture, (void*)&def.blendMaps, (void*)&comp->blendMaps};
        }

        if (propertyName.compare(0, 10, "blendMaps[") == 0) {
            return getTerrainTexturePropertyFast(comp->blendMaps, propertyName, 10);
        }

        if (propertyName == "surfaceLayers") {
            return {PropertyType::Custom, UpdateFlags_Terrain_Texture, (void*)&def.surfaceLayers, (void*)&comp->surfaceLayers};
        }

        if (propertyName.compare(0, 14, "surfaceLayers[") == 0) {
            size_t pos = 14;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            if (index >= comp->surfaceLayers.size()) {
                return PropertyData();
            }
            return getTerrainSurfaceLayerPropertyFast(comp->surfaceLayers[index], propertyName.substr(pos + 1));
        }

        if (propertyName == "foliageLayers") {
            return {PropertyType::Custom, UpdateFlags_Terrain_Foliage, (void*)&def.foliageLayers, (void*)&comp->foliageLayers};
        }

        if (propertyName.compare(0, 14, "foliageLayers[") == 0) {
            size_t pos = 14;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            if (index >= comp->foliageLayers.size()) {
                return PropertyData();
            }
            return getTerrainFoliageLayerPropertyFast(comp->foliageLayers[index], propertyName.substr(pos + 1));
        }

        return PropertyData();
    }

    PropertyData resolveLightPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<LightComponent*>(comp), propertyName, kLightProperties);
    }

    PropertyData resolveFogPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<FogComponent*>(comp), propertyName, kFogProperties);
    }

    PropertyData resolveLocalVolumetricFogPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<LocalVolumetricFogComponent*>(comp), propertyName, kLocalVolumetricFogProperties);
    }

    PropertyData resolveVolumetricCloudsPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<VolumetricCloudsComponent*>(comp), propertyName, kVolumetricCloudsProperties);
    }

    PropertyData resolveMirrorPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<MirrorComponent*>(comp), propertyName, kMirrorProperties);
    }

    PropertyData resolveReflectionProbePropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<ReflectionProbeComponent*>(comp), propertyName, kReflectionProbeProperties);
    }

    PropertyData resolveUIDocumentPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<UIDocumentComponent*>(comp), propertyName, kUIDocumentProperties);
    }

    PropertyData resolveFluidSimulationPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<FluidSimulationComponent*>(comp), propertyName, kFluidSimulationProperties);
    }

    // "<prefix>[N]" followed by nothing (fieldPos = npos) or by ".<field>" (fieldPos at the field);
    // false for any other text or an index out of [0, size)
    bool parseIndexedProperty(const std::string& propertyName, const char* prefix, size_t size, size_t& index, size_t& fieldPos) {
        const size_t length = std::strlen(prefix);
        if (propertyName.compare(0, length, prefix) != 0) {
            return false;
        }
        size_t pos = length;
        if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']' || index >= size) {
            return false;
        }
        pos++;
        if (pos == propertyName.size()) {
            fieldPos = std::string::npos;
            return true;
        }
        if (propertyName[pos] != '.' || pos + 1 == propertyName.size()) {
            return false;
        }
        fieldPos = pos + 1;
        return true;
    }

    PropertyData resolveLatticePropertyFast(void* compRef, const std::string& propertyName) {
        LatticeComponent* comp = static_cast<LatticeComponent*>(compRef);
        if (!comp) return PropertyData();

        PropertyData result = resolveDirectProperties(comp, propertyName, kLatticeProperties);
        if (result.ref) return result;

        // offsets[N]: one point, so a prefab instance overrides points one by one
        size_t index = 0;
        size_t fieldPos = 0;
        if (parseIndexedProperty(propertyName, "offsets[", comp->offsets.size(), index, fieldPos) && fieldPos == std::string::npos) {
            static Vector3 defOffset = Vector3::ZERO;
            return {PropertyType::Vector3, UpdateFlags_Lattice, (void*)&defOffset, (void*)&comp->offsets[index]};
        }

        return PropertyData();
    }

    PropertyData getLatticeBindingPropertyFast(LatticeBinding& binding, const std::string& field) {
        static LatticeBinding def;
        if (field == "lattice") return {PropertyType::Entity, UpdateFlags_Lattice, (void*)&def.lattice, (void*)&binding.lattice};
        if (field == "weight") return {PropertyType::Float, UpdateFlags_Lattice, (void*)&def.weight, (void*)&binding.weight};
        if (field == "stage") return {PropertyType::Enum, UpdateFlags_Lattice, (void*)&def.stage, (void*)&binding.stage};
        if (field == "submesh") return {PropertyType::Int, UpdateFlags_Lattice, (void*)&def.submesh, (void*)&binding.submesh};
        if (field == "mask") return {PropertyType::Enum, UpdateFlags_Lattice, (void*)&def.mask, (void*)&binding.mask};
        return PropertyData();
    }

    PropertyData resolveLatticeModifierPropertyFast(void* compRef, const std::string& propertyName) {
        LatticeModifierComponent* comp = static_cast<LatticeModifierComponent*>(compRef);
        if (!comp) return PropertyData();

        PropertyData result = resolveDirectProperties(comp, propertyName, kLatticeModifierProperties);
        if (result.ref) return result;

        // bindings[N].<field>
        size_t index = 0;
        size_t fieldPos = 0;
        if (parseIndexedProperty(propertyName, "bindings[", comp->bindings.size(), index, fieldPos) && fieldPos != std::string::npos) {
            return getLatticeBindingPropertyFast(comp->bindings[index], propertyName.substr(fieldPos));
        }

        return PropertyData();
    }

    PropertyData resolveLatticeFollowerPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<LatticeFollowerComponent*>(comp), propertyName, kLatticeFollowerProperties);
    }

    PropertyData resolveLatticeTracksPropertyFast(void* compRef, const std::string& propertyName) {
        LatticeTracksComponent* comp = static_cast<LatticeTracksComponent*>(compRef);
        if (!comp) return PropertyData();

        PropertyData result = resolveDirectProperties(comp, propertyName, kLatticeTracksProperties);
        if (result.ref) return result;

        size_t index = 0;
        size_t fieldPos = 0;
        // points[N]: the flat point index of column N of every key
        if (parseIndexedProperty(propertyName, "points[", comp->points.size(), index, fieldPos) && fieldPos == std::string::npos) {
            static int defPoint = 0;
            return {PropertyType::Int, UpdateFlags_None, (void*)&defPoint, (void*)&comp->points[index]};
        }
        // values[N]: the offsets of key N, one per entry of points
        if (parseIndexedProperty(propertyName, "values[", comp->values.size(), index, fieldPos) && fieldPos == std::string::npos) {
            static std::vector<Vector3> defValue;
            return {PropertyType::Custom, UpdateFlags_None, (void*)&defValue, (void*)&comp->values[index]};
        }

        return PropertyData();
    }

    PropertyData resolvePoseAnimatorPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<PoseAnimatorComponent*>(comp), propertyName, kPoseAnimatorProperties);
    }

    // clips[i].clip / .guid / .name are strings; clips[i].wrap is a ClipWrapMode, one byte, so it
    // is Custom (PropertyCmd<ClipWrapMode>, copyPropertyValue) and never an int-sized Enum
    PropertyData resolveAnimationPlayerPropertyFast(void* compRef, const std::string& propertyName) {
        AnimationPlayerComponent* comp = static_cast<AnimationPlayerComponent*>(compRef);
        if (!comp) return PropertyData();

        PropertyData result = resolveDirectProperties(comp, propertyName, kAnimationPlayerProperties);
        if (result.ref) return result;

        if (propertyName.compare(0, 6, "clips[") == 0) {
            size_t pos = 6;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index)) return PropertyData();
            if (pos + 1 >= propertyName.length() || propertyName[pos] != ']') return PropertyData();
            if (index < comp->clips.size()) {
                static AnimationPlayerClip defClip;
                AnimationPlayerClip& entry = comp->clips[index];
                const std::string fieldName = propertyName.substr(pos + 1);
                if (fieldName == ".clip") {
                    return {PropertyType::String, UpdateFlags_AnimationPlayer_Reload, (void*)&defClip.clip, (void*)&entry.clip};
                }
                if (fieldName == ".guid") {
                    return {PropertyType::String, UpdateFlags_AnimationPlayer_Reload, (void*)&defClip.guid, (void*)&entry.guid};
                }
                if (fieldName == ".name") {
                    return {PropertyType::String, UpdateFlags_AnimationPlayer_Reload, (void*)&defClip.name, (void*)&entry.name};
                }
                if (fieldName == ".wrap") {
                    return {PropertyType::Custom, UpdateFlags_AnimationPlayer_Reload, (void*)&defClip.wrap, (void*)&entry.wrap};
                }
            }
        }

        return PropertyData();
    }

    PropertyData resolveLight2DPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<Light2DComponent*>(comp), propertyName, kLight2DProperties);
    }

    PropertyData resolveShadowCaster2DPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<ShadowCaster2DComponent*>(comp), propertyName, kShadowCaster2DProperties);
    }

    PropertyData resolveCameraPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<CameraComponent*>(comp), propertyName, kCameraProperties);
    }

    PropertyData resolveAudioPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<AudioSourceComponent*>(comp), propertyName, kAudioSourceProperties);
    }

    PropertyData resolveSkyPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<SkyboxComponent*>(comp), propertyName, kSkyProperties);
    }

    PropertyData resolveTextPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<TextComponent*>(comp), propertyName, kTextProperties);
    }

    PropertyData resolveJoint2DPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<Joint2DComponent*>(comp), propertyName, kJoint2DProperties);
    }

    PropertyData resolveJoint3DPropertyFast(void* comp, const std::string& propertyName) {
        Joint3DComponent* joint = static_cast<Joint3DComponent*>(comp);
        if (!joint) return PropertyData();

        // Flat properties from descriptor array
        PropertyData result = resolveDirectProperties(joint, propertyName, kJoint3DProperties);
        if (result.ref) return result;

        Joint3DComponent& def = getDefaultComponent<Joint3DComponent>();

        if (propertyName == "pathPoints") {
            return {PropertyType::Custom, UpdateFlags_Joint3D, (void*)&def.pathPoints, (void*)&joint->pathPoints};
        }

        // pathPoints[N]
        if (propertyName.compare(0, 11, "pathPoints[") == 0) {
            size_t pos = 11;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            if (pos + 1 != propertyName.size() || index >= joint->pathPoints.size()) {
                return PropertyData();
            }
            return {PropertyType::Vector3, UpdateFlags_Joint3D, (void*)&def.pathPosition, (void*)&joint->pathPoints[index]};
        }

        return PropertyData();
    }

    PropertyData resolveRigidbody2DPropertyFast(void* comp, const std::string& propertyName) {
        return getRigidbody2DPropertyFast(static_cast<Rigidbody2DComponent*>(comp), propertyName);
    }

    PropertyData resolveRigidbodyPropertyFast(void* comp, const std::string& propertyName) {
        return getRigidbodyPropertyFast(static_cast<RigidbodyComponent*>(comp), propertyName);
    }

    PropertyData getParticlesPropertyFast(ParticleSystemComponent* comp, const std::string& propertyName) {
        ParticleSystemComponent& def = getDefaultComponent<ParticleSystemComponent>();
        if (!comp) return PropertyData();

        // Top-level properties
        if (propertyName == "maxParticles") return {PropertyType::UInt, UpdateFlags_None, &def.maxParticles, &comp->maxParticles};
        if (propertyName == "emitter") return {PropertyType::Bool, UpdateFlags_None, &def.emitter, &comp->emitter};
        if (propertyName == "loop") return {PropertyType::Bool, UpdateFlags_None, &def.loop, &comp->loop};
        if (propertyName == "localSpace") return {PropertyType::Bool, UpdateFlags_None, &def.localSpace, &comp->localSpace};
        if (propertyName == "rate") return {PropertyType::Int, UpdateFlags_None, &def.rate, &comp->rate};
        if (propertyName == "maxPerUpdate") return {PropertyType::Int, UpdateFlags_None, &def.maxPerUpdate, &comp->maxPerUpdate};
        if (propertyName == "bursts") return {PropertyType::Custom, UpdateFlags_None, &def.bursts, &comp->bursts};

        // Life Initializer
        if (propertyName == "lifeInitializer.minLife") return {PropertyType::Float, UpdateFlags_None, &def.lifeInitializer.minLife, &comp->lifeInitializer.minLife};
        if (propertyName == "lifeInitializer.maxLife") return {PropertyType::Float, UpdateFlags_None, &def.lifeInitializer.maxLife, &comp->lifeInitializer.maxLife};

        // Position Initializer
        if (propertyName == "positionInitializer.shape") return {PropertyType::Enum, UpdateFlags_None, &def.positionInitializer.shape, &comp->positionInitializer.shape};
        if (propertyName == "positionInitializer.minPosition") return {PropertyType::Vector3, UpdateFlags_None, &def.positionInitializer.minPosition, &comp->positionInitializer.minPosition};
        if (propertyName == "positionInitializer.maxPosition") return {PropertyType::Vector3, UpdateFlags_None, &def.positionInitializer.maxPosition, &comp->positionInitializer.maxPosition};
        if (propertyName == "positionInitializer.radius") return {PropertyType::Float, UpdateFlags_None, &def.positionInitializer.radius, &comp->positionInitializer.radius};
        if (propertyName == "positionInitializer.innerRadius") return {PropertyType::Float, UpdateFlags_None, &def.positionInitializer.innerRadius, &comp->positionInitializer.innerRadius};
        if (propertyName == "positionInitializer.coneAngle") return {PropertyType::Float, UpdateFlags_None, &def.positionInitializer.coneAngle, &comp->positionInitializer.coneAngle};
        if (propertyName == "positionInitializer.coneHeight") return {PropertyType::Float, UpdateFlags_None, &def.positionInitializer.coneHeight, &comp->positionInitializer.coneHeight};

        // Position Modifier
        if (propertyName == "positionModifier.fromTime") return {PropertyType::Float, UpdateFlags_None, &def.positionModifier.fromTime, &comp->positionModifier.fromTime};
        if (propertyName == "positionModifier.toTime") return {PropertyType::Float, UpdateFlags_None, &def.positionModifier.toTime, &comp->positionModifier.toTime};
        if (propertyName == "positionModifier.function") return {PropertyType::Ease, UpdateFlags_None, &def.positionModifier.function, &comp->positionModifier.function};
        if (propertyName == "positionModifier.fromPosition") return {PropertyType::Vector3, UpdateFlags_None, &def.positionModifier.fromPosition, &comp->positionModifier.fromPosition};
        if (propertyName == "positionModifier.toPosition") return {PropertyType::Vector3, UpdateFlags_None, &def.positionModifier.toPosition, &comp->positionModifier.toPosition};

        // Velocity Initializer
        if (propertyName == "velocityInitializer.minVelocity") return {PropertyType::Vector3, UpdateFlags_None, &def.velocityInitializer.minVelocity, &comp->velocityInitializer.minVelocity};
        if (propertyName == "velocityInitializer.maxVelocity") return {PropertyType::Vector3, UpdateFlags_None, &def.velocityInitializer.maxVelocity, &comp->velocityInitializer.maxVelocity};

        // Velocity Modifier
        if (propertyName == "velocityModifier.fromTime") return {PropertyType::Float, UpdateFlags_None, &def.velocityModifier.fromTime, &comp->velocityModifier.fromTime};
        if (propertyName == "velocityModifier.toTime") return {PropertyType::Float, UpdateFlags_None, &def.velocityModifier.toTime, &comp->velocityModifier.toTime};
        if (propertyName == "velocityModifier.function") return {PropertyType::Ease, UpdateFlags_None, &def.velocityModifier.function, &comp->velocityModifier.function};
        if (propertyName == "velocityModifier.fromVelocity") return {PropertyType::Vector3, UpdateFlags_None, &def.velocityModifier.fromVelocity, &comp->velocityModifier.fromVelocity};
        if (propertyName == "velocityModifier.toVelocity") return {PropertyType::Vector3, UpdateFlags_None, &def.velocityModifier.toVelocity, &comp->velocityModifier.toVelocity};

        // Acceleration Initializer
        if (propertyName == "accelerationInitializer.minAcceleration") return {PropertyType::Vector3, UpdateFlags_None, &def.accelerationInitializer.minAcceleration, &comp->accelerationInitializer.minAcceleration};
        if (propertyName == "accelerationInitializer.maxAcceleration") return {PropertyType::Vector3, UpdateFlags_None, &def.accelerationInitializer.maxAcceleration, &comp->accelerationInitializer.maxAcceleration};

        // Acceleration Modifier
        if (propertyName == "accelerationModifier.fromTime") return {PropertyType::Float, UpdateFlags_None, &def.accelerationModifier.fromTime, &comp->accelerationModifier.fromTime};
        if (propertyName == "accelerationModifier.toTime") return {PropertyType::Float, UpdateFlags_None, &def.accelerationModifier.toTime, &comp->accelerationModifier.toTime};
        if (propertyName == "accelerationModifier.function") return {PropertyType::Ease, UpdateFlags_None, &def.accelerationModifier.function, &comp->accelerationModifier.function};
        if (propertyName == "accelerationModifier.fromAcceleration") return {PropertyType::Vector3, UpdateFlags_None, &def.accelerationModifier.fromAcceleration, &comp->accelerationModifier.fromAcceleration};
        if (propertyName == "accelerationModifier.toAcceleration") return {PropertyType::Vector3, UpdateFlags_None, &def.accelerationModifier.toAcceleration, &comp->accelerationModifier.toAcceleration};

        // Color Initializer
        if (propertyName == "colorInitializer.minColor") return {PropertyType::Vector3, UpdateFlags_None, &def.colorInitializer.minColor, &comp->colorInitializer.minColor};
        if (propertyName == "colorInitializer.maxColor") return {PropertyType::Vector3, UpdateFlags_None, &def.colorInitializer.maxColor, &comp->colorInitializer.maxColor};
        if (propertyName == "colorInitializer.useSRGB") return {PropertyType::Bool, UpdateFlags_None, &def.colorInitializer.useSRGB, &comp->colorInitializer.useSRGB};

        // Color Modifier
        if (propertyName == "colorModifier.fromTime") return {PropertyType::Float, UpdateFlags_None, &def.colorModifier.fromTime, &comp->colorModifier.fromTime};
        if (propertyName == "colorModifier.toTime") return {PropertyType::Float, UpdateFlags_None, &def.colorModifier.toTime, &comp->colorModifier.toTime};
        if (propertyName == "colorModifier.function") return {PropertyType::Ease, UpdateFlags_None, &def.colorModifier.function, &comp->colorModifier.function};
        if (propertyName == "colorModifier.fromColor") return {PropertyType::Vector3, UpdateFlags_None, &def.colorModifier.fromColor, &comp->colorModifier.fromColor};
        if (propertyName == "colorModifier.toColor") return {PropertyType::Vector3, UpdateFlags_None, &def.colorModifier.toColor, &comp->colorModifier.toColor};
        if (propertyName == "colorModifier.useSRGB") return {PropertyType::Bool, UpdateFlags_None, &def.colorModifier.useSRGB, &comp->colorModifier.useSRGB};

        // Color Gradient (overrides colorModifier when non-empty)
        if (propertyName == "colorGradient") return {PropertyType::Custom, UpdateFlags_None, &def.colorGradient, &comp->colorGradient};

        // Alpha Initializer
        if (propertyName == "alphaInitializer.minAlpha") return {PropertyType::Float, UpdateFlags_None, &def.alphaInitializer.minAlpha, &comp->alphaInitializer.minAlpha};
        if (propertyName == "alphaInitializer.maxAlpha") return {PropertyType::Float, UpdateFlags_None, &def.alphaInitializer.maxAlpha, &comp->alphaInitializer.maxAlpha};

        // Alpha Modifier
        if (propertyName == "alphaModifier.fromTime") return {PropertyType::Float, UpdateFlags_None, &def.alphaModifier.fromTime, &comp->alphaModifier.fromTime};
        if (propertyName == "alphaModifier.toTime") return {PropertyType::Float, UpdateFlags_None, &def.alphaModifier.toTime, &comp->alphaModifier.toTime};
        if (propertyName == "alphaModifier.function") return {PropertyType::Ease, UpdateFlags_None, &def.alphaModifier.function, &comp->alphaModifier.function};
        if (propertyName == "alphaModifier.fromAlpha") return {PropertyType::Float, UpdateFlags_None, &def.alphaModifier.fromAlpha, &comp->alphaModifier.fromAlpha};
        if (propertyName == "alphaModifier.toAlpha") return {PropertyType::Float, UpdateFlags_None, &def.alphaModifier.toAlpha, &comp->alphaModifier.toAlpha};

        // Size Initializer
        if (propertyName == "sizeInitializer.minSize") return {PropertyType::Float, UpdateFlags_None, &def.sizeInitializer.minSize, &comp->sizeInitializer.minSize};
        if (propertyName == "sizeInitializer.maxSize") return {PropertyType::Float, UpdateFlags_None, &def.sizeInitializer.maxSize, &comp->sizeInitializer.maxSize};

        // Size Modifier
        if (propertyName == "sizeModifier.fromTime") return {PropertyType::Float, UpdateFlags_None, &def.sizeModifier.fromTime, &comp->sizeModifier.fromTime};
        if (propertyName == "sizeModifier.toTime") return {PropertyType::Float, UpdateFlags_None, &def.sizeModifier.toTime, &comp->sizeModifier.toTime};
        if (propertyName == "sizeModifier.function") return {PropertyType::Ease, UpdateFlags_None, &def.sizeModifier.function, &comp->sizeModifier.function};
        if (propertyName == "sizeModifier.fromSize") return {PropertyType::Float, UpdateFlags_None, &def.sizeModifier.fromSize, &comp->sizeModifier.fromSize};
        if (propertyName == "sizeModifier.toSize") return {PropertyType::Float, UpdateFlags_None, &def.sizeModifier.toSize, &comp->sizeModifier.toSize};

        // Sprite Initializer
        if (propertyName == "spriteInitializer.frames") return {PropertyType::Custom, UpdateFlags_None, &def.spriteInitializer.frames, &comp->spriteInitializer.frames};

        // Sprite Modifier
        if (propertyName == "spriteModifier.fromTime") return {PropertyType::Float, UpdateFlags_None, &def.spriteModifier.fromTime, &comp->spriteModifier.fromTime};
        if (propertyName == "spriteModifier.toTime") return {PropertyType::Float, UpdateFlags_None, &def.spriteModifier.toTime, &comp->spriteModifier.toTime};
        if (propertyName == "spriteModifier.function") return {PropertyType::Ease, UpdateFlags_None, &def.spriteModifier.function, &comp->spriteModifier.function};
        if (propertyName == "spriteModifier.frames") return {PropertyType::Custom, UpdateFlags_None, &def.spriteModifier.frames, &comp->spriteModifier.frames};

        // Rotation Initializer
        if (propertyName == "rotationInitializer.minRotation") return {PropertyType::Quat, UpdateFlags_None, &def.rotationInitializer.minRotation, &comp->rotationInitializer.minRotation};
        if (propertyName == "rotationInitializer.maxRotation") return {PropertyType::Quat, UpdateFlags_None, &def.rotationInitializer.maxRotation, &comp->rotationInitializer.maxRotation};
        if (propertyName == "rotationInitializer.shortestPath") return {PropertyType::Bool, UpdateFlags_None, &def.rotationInitializer.shortestPath, &comp->rotationInitializer.shortestPath};

        // Rotation Modifier
        if (propertyName == "rotationModifier.fromTime") return {PropertyType::Float, UpdateFlags_None, &def.rotationModifier.fromTime, &comp->rotationModifier.fromTime};
        if (propertyName == "rotationModifier.toTime") return {PropertyType::Float, UpdateFlags_None, &def.rotationModifier.toTime, &comp->rotationModifier.toTime};
        if (propertyName == "rotationModifier.function") return {PropertyType::Ease, UpdateFlags_None, &def.rotationModifier.function, &comp->rotationModifier.function};
        if (propertyName == "rotationModifier.fromRotation") return {PropertyType::Quat, UpdateFlags_None, &def.rotationModifier.fromRotation, &comp->rotationModifier.fromRotation};
        if (propertyName == "rotationModifier.toRotation") return {PropertyType::Quat, UpdateFlags_None, &def.rotationModifier.toRotation, &comp->rotationModifier.toRotation};
        if (propertyName == "rotationModifier.shortestPath") return {PropertyType::Bool, UpdateFlags_None, &def.rotationModifier.shortestPath, &comp->rotationModifier.shortestPath};

        // Scale Initializer
        if (propertyName == "scaleInitializer.minScale") return {PropertyType::Vector3, UpdateFlags_None, &def.scaleInitializer.minScale, &comp->scaleInitializer.minScale};
        if (propertyName == "scaleInitializer.maxScale") return {PropertyType::Vector3, UpdateFlags_None, &def.scaleInitializer.maxScale, &comp->scaleInitializer.maxScale};
        if (propertyName == "scaleInitializer.linearSort") return {PropertyType::Bool, UpdateFlags_None, &def.scaleInitializer.linearSort, &comp->scaleInitializer.linearSort};

        // Scale Modifier
        if (propertyName == "scaleModifier.fromTime") return {PropertyType::Float, UpdateFlags_None, &def.scaleModifier.fromTime, &comp->scaleModifier.fromTime};
        if (propertyName == "scaleModifier.toTime") return {PropertyType::Float, UpdateFlags_None, &def.scaleModifier.toTime, &comp->scaleModifier.toTime};
        if (propertyName == "scaleModifier.function") return {PropertyType::Ease, UpdateFlags_None, &def.scaleModifier.function, &comp->scaleModifier.function};
        if (propertyName == "scaleModifier.fromScale") return {PropertyType::Vector3, UpdateFlags_None, &def.scaleModifier.fromScale, &comp->scaleModifier.fromScale};
        if (propertyName == "scaleModifier.toScale") return {PropertyType::Vector3, UpdateFlags_None, &def.scaleModifier.toScale, &comp->scaleModifier.toScale};

        return PropertyData();
    }

    PropertyData resolveParticlesPropertyFast(void* comp, const std::string& propertyName) {
        return getParticlesPropertyFast(static_cast<ParticleSystemComponent*>(comp), propertyName);
    }

    PropertyData getInstancedMeshPropertyFast(InstancedMeshComponent* comp, const std::string& propertyName) {
        InstancedMeshComponent& def = getDefaultComponent<InstancedMeshComponent>();
        if (propertyName == "maxInstances") return {PropertyType::UInt, UpdateFlags_Mesh_Reload, &def.maxInstances, &comp->maxInstances};
        // distanceFade picks the shader variant, so changing it needs the reload that
        // fadeStart and fadeEnd, which are plain uniforms, do not.
        if (propertyName == "distanceFade") return {PropertyType::Bool, UpdateFlags_Mesh_Reload, &def.distanceFade, &comp->distanceFade};
        if (propertyName == "cullInstances") return {PropertyType::Bool, UpdateFlags_Instanced_Mesh, &def.cullInstances, &comp->cullInstances};
        if (propertyName == "fadeStart") return {PropertyType::Float, UpdateFlags_None, &def.fadeStart, &comp->fadeStart};
        if (propertyName == "cullDistance") return {PropertyType::Float, UpdateFlags_None, &def.cullDistance, &comp->cullDistance};
        if (propertyName == "fadeEnd") return {PropertyType::Float, UpdateFlags_None, &def.fadeEnd, &comp->fadeEnd};
        if (propertyName == "instancedBillboard") return {PropertyType::Bool, UpdateFlags_Mesh_Reload, &def.instancedBillboard, &comp->instancedBillboard};
        if (propertyName == "instancedCylindricalBillboard") return {PropertyType::Bool, UpdateFlags_Mesh_Reload, &def.instancedCylindricalBillboard, &comp->instancedCylindricalBillboard};

        if (propertyName == "instances") {
            static std::vector<InstanceData> defInstances;
            return {PropertyType::Custom, UpdateFlags_Instanced_Mesh, (void*)&defInstances, (void*)&comp->instances};
        }

        if (propertyName.compare(0, 10, "instances[") == 0) {
            size_t pos = 10;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') return PropertyData();
            if (pos + 1 >= propertyName.size()) return PropertyData();
            std::string fieldName = propertyName.substr(pos + 1);
            if (index >= comp->instances.size()) return PropertyData();
            if (fieldName == ".position") {
                static Vector3 defPos = Vector3(0.0f, 0.0f, 0.0f);
                return {PropertyType::Vector3, UpdateFlags_Instanced_Mesh, (void*)&defPos, (void*)&comp->instances[index].position};
            }
            if (fieldName == ".rotation") {
                static Quaternion defRot;
                return {PropertyType::Quat, UpdateFlags_Instanced_Mesh, (void*)&defRot, (void*)&comp->instances[index].rotation};
            }
            if (fieldName == ".scale") {
                static Vector3 defScale = Vector3(1.0f, 1.0f, 1.0f);
                return {PropertyType::Vector3, UpdateFlags_Instanced_Mesh, (void*)&defScale, (void*)&comp->instances[index].scale};
            }
            if (fieldName == ".color") {
                static Vector4 defColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                return {PropertyType::Vector4, UpdateFlags_Instanced_Mesh, (void*)&defColor, (void*)&comp->instances[index].color};
            }
            if (fieldName == ".visible") {
                static bool defVisible = true;
                return {PropertyType::Bool, UpdateFlags_Instanced_Mesh, (void*)&defVisible, (void*)&comp->instances[index].visible};
            }
        }

        return PropertyData();
    }

    PropertyData resolveInstancedMeshPropertyFast(void* comp, const std::string& propertyName) {
        return getInstancedMeshPropertyFast(static_cast<InstancedMeshComponent*>(comp), propertyName);
    }

    void enumerateInstancedMeshProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        InstancedMeshComponent* comp = static_cast<InstancedMeshComponent*>(compRef);
        InstancedMeshComponent& def = getDefaultComponent<InstancedMeshComponent>();
        ps["maxInstances"] = {PropertyType::UInt, UpdateFlags_Mesh_Reload, &def.maxInstances, comp ? &comp->maxInstances : nullptr};
        ps["distanceFade"] = {PropertyType::Bool, UpdateFlags_Mesh_Reload, &def.distanceFade, comp ? &comp->distanceFade : nullptr};
        ps["cullInstances"] = {PropertyType::Bool, UpdateFlags_Instanced_Mesh, &def.cullInstances, comp ? &comp->cullInstances : nullptr};
        ps["fadeStart"] = {PropertyType::Float, UpdateFlags_None, &def.fadeStart, comp ? &comp->fadeStart : nullptr};
        ps["cullDistance"] = {PropertyType::Float, UpdateFlags_None, &def.cullDistance, comp ? &comp->cullDistance : nullptr};
        ps["fadeEnd"] = {PropertyType::Float, UpdateFlags_None, &def.fadeEnd, comp ? &comp->fadeEnd : nullptr};
        ps["instancedBillboard"] = {PropertyType::Bool, UpdateFlags_Mesh_Reload, &def.instancedBillboard, comp ? &comp->instancedBillboard : nullptr};
        ps["instancedCylindricalBillboard"] = {PropertyType::Bool, UpdateFlags_Mesh_Reload, &def.instancedCylindricalBillboard, comp ? &comp->instancedCylindricalBillboard : nullptr};
        static std::vector<InstanceData> defInstances;
        ps["instances"] = {PropertyType::Custom, UpdateFlags_Instanced_Mesh, (void*)&defInstances, comp ? (void*)&comp->instances : nullptr};
        if (comp) {
            static Vector3 defPos = Vector3(0.0f, 0.0f, 0.0f);
            static Quaternion defRot;
            static Vector3 defScale = Vector3(1.0f, 1.0f, 1.0f);
            static Vector4 defColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
            static bool defVisible = true;
            for (size_t i = 0; i < comp->instances.size(); i++) {
                std::string prefix = "instances[" + std::to_string(i) + "]";
                ps[prefix + ".position"] = {PropertyType::Vector3, UpdateFlags_Instanced_Mesh, (void*)&defPos, (void*)&comp->instances[i].position};
                ps[prefix + ".rotation"] = {PropertyType::Quat, UpdateFlags_Instanced_Mesh, (void*)&defRot, (void*)&comp->instances[i].rotation};
                ps[prefix + ".scale"] = {PropertyType::Vector3, UpdateFlags_Instanced_Mesh, (void*)&defScale, (void*)&comp->instances[i].scale};
                ps[prefix + ".color"] = {PropertyType::Vector4, UpdateFlags_Instanced_Mesh, (void*)&defColor, (void*)&comp->instances[i].color};
                ps[prefix + ".visible"] = {PropertyType::Bool, UpdateFlags_Instanced_Mesh, (void*)&defVisible, (void*)&comp->instances[i].visible};
            }
        }
    }

    PropertyData resolveActionPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<ActionComponent*>(comp), propertyName, kActionProperties);
    }

    PropertyData resolveTweenPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<TweenComponent*>(comp), propertyName, kTweenProperties);
    }

    PropertyData resolvePositionTweenPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<PositionTweenComponent*>(comp), propertyName, kPositionTweenProperties);
    }

    PropertyData resolveRotationTweenPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<RotationTweenComponent*>(comp), propertyName, kRotationTweenProperties);
    }

    PropertyData resolveScaleTweenPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<ScaleTweenComponent*>(comp), propertyName, kScaleTweenProperties);
    }

    PropertyData resolveColorTweenPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<ColorTweenComponent*>(comp), propertyName, kColorTweenProperties);
    }

    PropertyData resolveAlphaTweenPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<AlphaTweenComponent*>(comp), propertyName, kAlphaTweenProperties);
    }

    PropertyData resolveSpriteAnimationPropertyFast(void* compRef, const std::string& propertyName) {
        SpriteAnimationComponent* comp = static_cast<SpriteAnimationComponent*>(compRef);
        if (!comp) return PropertyData();

        PropertyData result = resolveDirectProperties(comp, propertyName, kSpriteAnimationProperties);
        if (result.ref) return result;

        SpriteAnimationComponent& def = getDefaultComponent<SpriteAnimationComponent>();

        if (propertyName.compare(0, 7, "frames[") == 0) {
            size_t pos = 7;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') return PropertyData();
            if (index < MAX_SPRITE_FRAMES) {
                return {PropertyType::Int, UpdateFlags_None, (void*)&def.frames[index], (void*)&comp->frames[index]};
            }
        }
        if (propertyName.compare(0, 11, "framesTime[") == 0) {
            size_t pos = 11;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') return PropertyData();
            if (index < MAX_SPRITE_FRAMES) {
                return {PropertyType::Int, UpdateFlags_None, (void*)&def.framesTime[index], (void*)&comp->framesTime[index]};
            }
        }
        return PropertyData();
    }

    PropertyData resolveAnimationPropertyFast(void* compRef, const std::string& propertyName) {
        AnimationComponent* comp = static_cast<AnimationComponent*>(compRef);
        if (!comp) return PropertyData();

        PropertyData result = resolveDirectProperties(comp, propertyName, kAnimationProperties);
        if (result.ref) return result;

        AnimationComponent& def = getDefaultComponent<AnimationComponent>();

        if (propertyName == "actions") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.actions, (void*)&comp->actions};
        }

        if (propertyName.compare(0, 8, "actions[") == 0) {
            size_t pos = 8;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index)) return PropertyData();
            if (pos + 1 >= propertyName.length() || propertyName[pos] != ']') return PropertyData();
            if (index < comp->actions.size()) {
                std::string fieldName = propertyName.substr(pos + 1);
                if (fieldName == ".startTime") {
                    static float defStartTime = 0;
                    return {PropertyType::Float, UpdateFlags_None, (void*)&defStartTime, (void*)&comp->actions[index].startTime};
                }
                if (fieldName == ".duration") {
                    static float defDuration = 0;
                    return {PropertyType::Float, UpdateFlags_None, (void*)&defDuration, (void*)&comp->actions[index].duration};
                }
                if (fieldName == ".action") {
                    static Entity defAction = NULL_ENTITY;
                    return {PropertyType::Entity, UpdateFlags_None, (void*)&defAction, (void*)&comp->actions[index].action};
                }
                if (fieldName == ".track") {
                    static uint32_t defTrack = 0;
                    return {PropertyType::UInt, UpdateFlags_None, (void*)&defTrack, (void*)&comp->actions[index].track};
                }
            }
        }

        return PropertyData();
    }

    PropertyData resolveModelPropertyFast(void* compRef, const std::string& propertyName) {
        ModelComponent* comp = static_cast<ModelComponent*>(compRef);
        if (!comp) return PropertyData();

        ModelComponent& def = getDefaultComponent<ModelComponent>();

        if (propertyName == "filename") {
            return {PropertyType::Asset, UpdateFlags_None, (void*)&def.filename, (void*)&comp->filename};
        }

        if (propertyName == "mergeStaticMeshes") {
            return {PropertyType::Bool, UpdateFlags_Model, (void*)&def.mergeStaticMeshes, (void*)&comp->mergeStaticMeshes};
        }

        // read by the loader on the next load or reload; the scene step that turns it off
        // ("Use Animation Player", 9.R15) removes the animation entities itself
        if (propertyName == "animationEntities") {
            return {PropertyType::Bool, UpdateFlags_None, (void*)&def.animationEntities, (void*)&comp->animationEntities};
        }

        if (propertyName == "skeleton") {
            return {PropertyType::Entity, UpdateFlags_Mesh_Reload, (void*)&def.skeleton, (void*)&comp->skeleton};
        }

        if (propertyName == "animations") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.animations, (void*)&comp->animations};
        }

        if (propertyName.compare(0, 11, "animations[") == 0) {
            size_t pos = 11;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            if (pos + 1 != propertyName.size() || index >= comp->animations.size()) {
                return PropertyData();
            }
            static Entity defEntity = NULL_ENTITY;
            return {PropertyType::Entity, UpdateFlags_None, (void*)&defEntity, (void*)&comp->animations[index]};
        }

        return PropertyData();
    }

    PropertyData resolveMeshPropertyFast(void* comp, const std::string& propertyName) {
        return getMeshPropertyFast(static_cast<MeshComponent*>(comp), propertyName);
    }

    PropertyData resolveLayoutGroupPropertyFast(void* comp, const std::string& propertyName) {
        return getLayoutGroupPropertyFast(static_cast<LayoutGroupComponent*>(comp), propertyName);
    }

    PropertyData resolveScriptPropertyFast(void* comp, const std::string& propertyName) {
        return getScriptPropertyFast(static_cast<ScriptComponent*>(comp), propertyName);
    }

    PropertyData resolvePrefabPropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<PrefabComponent*>(comp), propertyName, kPrefabProperties);
    }

    PropertyData resolveBonePropertyFast(void* comp, const std::string& propertyName) {
        return resolveDirectProperties(static_cast<BoneComponent*>(comp), propertyName, kBoneProperties);
    }

    PropertyData resolveKeyframeTracksPropertyFast(void* comp, const std::string& propertyName) {
        KeyframeTracksComponent* trackComp = static_cast<KeyframeTracksComponent*>(comp);
        if (!trackComp) return PropertyData();

        PropertyData result = resolveDirectProperties(trackComp, propertyName, kKeyframeTracksProperties);
        if (result.ref) return result;

        KeyframeTracksComponent& def = getDefaultComponent<KeyframeTracksComponent>();

        if (propertyName == "times") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.times, (void*)&trackComp->times};
        }

        if (propertyName.compare(0, 6, "times[") == 0) {
            size_t pos = 6;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            if (pos + 1 != propertyName.size() || index >= trackComp->times.size()) {
                return PropertyData();
            }
            static float defValue = 0.0f;
            return {PropertyType::Float, UpdateFlags_None, (void*)&defValue, (void*)&trackComp->times[index]};
        }

        if (propertyName == "easings") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.easings, (void*)&trackComp->easings};
        }

        if (propertyName.compare(0, 8, "easings[") == 0) {
            size_t pos = 8;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            if (pos + 1 != propertyName.size() || index >= trackComp->easings.size()) {
                return PropertyData();
            }
            static EaseType defValue = EaseType::LINEAR;
            return {PropertyType::Enum, UpdateFlags_None, (void*)&defValue, (void*)&trackComp->easings[index]};
        }

        return PropertyData();
    }

    PropertyData resolveTranslateTracksPropertyFast(void* comp, const std::string& propertyName) {
        TranslateTracksComponent* trackComp = static_cast<TranslateTracksComponent*>(comp);
        if (!trackComp) return PropertyData();

        TranslateTracksComponent& def = getDefaultComponent<TranslateTracksComponent>();

        if (propertyName == "values") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.values, (void*)&trackComp->values};
        }

        // Hermite tangents (GLTF CUBICSPLINE), kept mirrored with values by key-edit commands
        if (propertyName == "inTangents") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.inTangents, (void*)&trackComp->inTangents};
        }
        if (propertyName == "outTangents") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.outTangents, (void*)&trackComp->outTangents};
        }

        if (propertyName.compare(0, 7, "values[") == 0) {
            size_t pos = 7;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            if (pos + 1 != propertyName.size() || index >= trackComp->values.size()) {
                return PropertyData();
            }
            static Vector3 defValue = Vector3::ZERO;
            return {PropertyType::Vector3, UpdateFlags_None, (void*)&defValue, (void*)&trackComp->values[index]};
        }

        return PropertyData();
    }

    PropertyData resolveRotateTracksPropertyFast(void* comp, const std::string& propertyName) {
        RotateTracksComponent* trackComp = static_cast<RotateTracksComponent*>(comp);
        if (!trackComp) return PropertyData();

        RotateTracksComponent& def = getDefaultComponent<RotateTracksComponent>();

        if (propertyName == "values") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.values, (void*)&trackComp->values};
        }

        // Hermite tangents (GLTF CUBICSPLINE), kept mirrored with values by key-edit commands
        if (propertyName == "inTangents") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.inTangents, (void*)&trackComp->inTangents};
        }
        if (propertyName == "outTangents") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.outTangents, (void*)&trackComp->outTangents};
        }

        if (propertyName.compare(0, 7, "values[") == 0) {
            size_t pos = 7;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            if (pos + 1 != propertyName.size() || index >= trackComp->values.size()) {
                return PropertyData();
            }
            static Quaternion defValue = Quaternion::IDENTITY;
            return {PropertyType::Quat, UpdateFlags_None, (void*)&defValue, (void*)&trackComp->values[index]};
        }

        return PropertyData();
    }

    PropertyData resolveScaleTracksPropertyFast(void* comp, const std::string& propertyName) {
        ScaleTracksComponent* trackComp = static_cast<ScaleTracksComponent*>(comp);
        if (!trackComp) return PropertyData();

        ScaleTracksComponent& def = getDefaultComponent<ScaleTracksComponent>();

        if (propertyName == "values") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.values, (void*)&trackComp->values};
        }

        // Hermite tangents (GLTF CUBICSPLINE), kept mirrored with values by key-edit commands
        if (propertyName == "inTangents") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.inTangents, (void*)&trackComp->inTangents};
        }
        if (propertyName == "outTangents") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.outTangents, (void*)&trackComp->outTangents};
        }

        if (propertyName.compare(0, 7, "values[") == 0) {
            size_t pos = 7;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            if (pos + 1 != propertyName.size() || index >= trackComp->values.size()) {
                return PropertyData();
            }
            static Vector3 defValue = Vector3::ZERO;
            return {PropertyType::Vector3, UpdateFlags_None, (void*)&defValue, (void*)&trackComp->values[index]};
        }

        return PropertyData();
    }

    PropertyData resolveMorphTracksPropertyFast(void* comp, const std::string& propertyName) {
        MorphTracksComponent* trackComp = static_cast<MorphTracksComponent*>(comp);
        if (!trackComp) return PropertyData();

        MorphTracksComponent& def = getDefaultComponent<MorphTracksComponent>();

        if (propertyName == "values") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.values, (void*)&trackComp->values};
        }

        // Hermite tangents (GLTF CUBICSPLINE), kept mirrored with values by key-edit commands
        if (propertyName == "inTangents") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.inTangents, (void*)&trackComp->inTangents};
        }
        if (propertyName == "outTangents") {
            return {PropertyType::Custom, UpdateFlags_None, (void*)&def.outTangents, (void*)&trackComp->outTangents};
        }

        if (propertyName.compare(0, 7, "values[") == 0) {
            size_t pos = 7;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            if (pos + 1 != propertyName.size() || index >= trackComp->values.size()) {
                return PropertyData();
            }
            static std::vector<float> defValue;
            return {PropertyType::Custom, UpdateFlags_None, (void*)&defValue, (void*)&trackComp->values[index]};
        }

        return PropertyData();
    }

    // ── Enumerate functions (build full property map) ──

    void enumerateActionProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kActionProperties);
    }

    void enumerateTweenProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kTweenProperties);
    }

    void enumeratePositionTweenProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kPositionTweenProperties);
    }

    void enumerateRotationTweenProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kRotationTweenProperties);
    }

    void enumerateScaleTweenProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kScaleTweenProperties);
    }

    void enumerateColorTweenProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kColorTweenProperties);
    }

    void enumerateAlphaTweenProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kAlphaTweenProperties);
    }

    void enumerateModelProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        ModelComponent* comp = static_cast<ModelComponent*>(compRef);
        ModelComponent& def = getDefaultComponent<ModelComponent>();

        ps["filename"] = {PropertyType::Asset, UpdateFlags_Model, (void*)&def.filename, compRef ? (void*)&comp->filename : nullptr};
        ps["mergeStaticMeshes"] = {PropertyType::Bool, UpdateFlags_Model, (void*)&def.mergeStaticMeshes, compRef ? (void*)&comp->mergeStaticMeshes : nullptr};
        ps["animationEntities"] = {PropertyType::Bool, UpdateFlags_None, (void*)&def.animationEntities, compRef ? (void*)&comp->animationEntities : nullptr};
        ps["skeleton"] = {PropertyType::Entity, UpdateFlags_Mesh_Reload, (void*)&def.skeleton, compRef ? (void*)&comp->skeleton : nullptr};
        ps["animations"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.animations, compRef ? (void*)&comp->animations : nullptr};

        for (size_t i = 0; i < (compRef ? comp->animations.size() : 0); i++) {
            std::string idx = std::to_string(i);
            static Entity defEntity = NULL_ENTITY;
            ps["animations[" + idx + "]"] = {PropertyType::Entity, UpdateFlags_None, (void*)&defEntity, (void*)&comp->animations[i]};
        }

        if (compRef) {
            static Entity defEntity = NULL_ENTITY;
            for (auto& [boneId, boneEntity] : comp->bonesIdMapping) {
                ps["bonesIdMapping[" + std::to_string(boneId) + "]"] = {PropertyType::Entity, UpdateFlags_None, (void*)&defEntity, (void*)&boneEntity};
            }
            for (auto& [boneName, boneEntity] : comp->bonesNameMapping) {
                ps["bonesNameMapping[" + boneName + "]"] = {PropertyType::Entity, UpdateFlags_None, (void*)&defEntity, (void*)&boneEntity};
            }
            // The per-node child mesh entities are entity references too: they must be remapped
            // when the model is duplicated so the copy points at its own generated children
            // rather than sharing (and reloading into) the source model's meshes.
            for (auto& [nodeIdx, nodeEntity] : comp->meshNodesMapping) {
                ps["meshNodesMapping[" + std::to_string(nodeIdx) + "]"] = {PropertyType::Entity, UpdateFlags_None, (void*)&defEntity, (void*)&nodeEntity};
            }
            for (auto& [nodeIdx, nodeEntity] : comp->nodesIdMapping) {
                ps["nodesIdMapping[" + std::to_string(nodeIdx) + "]"] = {PropertyType::Entity, UpdateFlags_None, (void*)&defEntity, (void*)&nodeEntity};
            }
        }
    }

    void enumeratePrefabProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kPrefabProperties);
    }

    void enumerateBoneProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kBoneProperties);
    }

    void enumerateKeyframeTracksProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        KeyframeTracksComponent* comp = static_cast<KeyframeTracksComponent*>(compRef);
        KeyframeTracksComponent& def = getDefaultComponent<KeyframeTracksComponent>();

        enumerateFromDescriptors(compRef, ps, kKeyframeTracksProperties);

        ps["times"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.times, compRef ? (void*)&comp->times : nullptr};

        static float defValue = 0.0f;
        for (size_t i = 0; i < (compRef ? comp->times.size() : 1); i++) {
            std::string idx = compRef ? std::to_string(i) : "";
            ps["times[" + idx + "]"] = {PropertyType::Float, UpdateFlags_None, (void*)&defValue, compRef ? (void*)&comp->times[i] : nullptr};
        }

        ps["easings"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.easings, compRef ? (void*)&comp->easings : nullptr};

        static EaseType defEase = EaseType::LINEAR;
        for (size_t i = 0; i < (compRef ? comp->easings.size() : 1); i++) {
            std::string idx = compRef ? std::to_string(i) : "";
            ps["easings[" + idx + "]"] = {PropertyType::Enum, UpdateFlags_None, (void*)&defEase, compRef ? (void*)&comp->easings[i] : nullptr};
        }
    }

    void enumerateTranslateTracksProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        TranslateTracksComponent* comp = static_cast<TranslateTracksComponent*>(compRef);
        TranslateTracksComponent& def = getDefaultComponent<TranslateTracksComponent>();

        ps["values"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.values, compRef ? (void*)&comp->values : nullptr};
        ps["inTangents"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.inTangents, compRef ? (void*)&comp->inTangents : nullptr};
        ps["outTangents"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.outTangents, compRef ? (void*)&comp->outTangents : nullptr};

        static Vector3 defValue = Vector3::ZERO;
        for (size_t i = 0; i < (compRef ? comp->values.size() : 1); i++) {
            std::string idx = compRef ? std::to_string(i) : "";
            ps["values[" + idx + "]"] = {PropertyType::Vector3, UpdateFlags_None, (void*)&defValue, compRef ? (void*)&comp->values[i] : nullptr};
        }
    }

    void enumerateRotateTracksProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        RotateTracksComponent* comp = static_cast<RotateTracksComponent*>(compRef);
        RotateTracksComponent& def = getDefaultComponent<RotateTracksComponent>();

        ps["values"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.values, compRef ? (void*)&comp->values : nullptr};
        ps["inTangents"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.inTangents, compRef ? (void*)&comp->inTangents : nullptr};
        ps["outTangents"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.outTangents, compRef ? (void*)&comp->outTangents : nullptr};

        static Quaternion defValue = Quaternion::IDENTITY;
        for (size_t i = 0; i < (compRef ? comp->values.size() : 1); i++) {
            std::string idx = compRef ? std::to_string(i) : "";
            ps["values[" + idx + "]"] = {PropertyType::Quat, UpdateFlags_None, (void*)&defValue, compRef ? (void*)&comp->values[i] : nullptr};
        }
    }

    void enumerateScaleTracksProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        ScaleTracksComponent* comp = static_cast<ScaleTracksComponent*>(compRef);
        ScaleTracksComponent& def = getDefaultComponent<ScaleTracksComponent>();

        ps["values"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.values, compRef ? (void*)&comp->values : nullptr};
        ps["inTangents"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.inTangents, compRef ? (void*)&comp->inTangents : nullptr};
        ps["outTangents"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.outTangents, compRef ? (void*)&comp->outTangents : nullptr};

        static Vector3 defValue = Vector3::ZERO;
        for (size_t i = 0; i < (compRef ? comp->values.size() : 1); i++) {
            std::string idx = compRef ? std::to_string(i) : "";
            ps["values[" + idx + "]"] = {PropertyType::Vector3, UpdateFlags_None, (void*)&defValue, compRef ? (void*)&comp->values[i] : nullptr};
        }
    }

    void enumerateMorphTracksProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        MorphTracksComponent* comp = static_cast<MorphTracksComponent*>(compRef);
        MorphTracksComponent& def = getDefaultComponent<MorphTracksComponent>();

        ps["values"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.values, compRef ? (void*)&comp->values : nullptr};
        ps["inTangents"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.inTangents, compRef ? (void*)&comp->inTangents : nullptr};
        ps["outTangents"] = {PropertyType::Custom, UpdateFlags_None, (void*)&def.outTangents, compRef ? (void*)&comp->outTangents : nullptr};

        static std::vector<float> defValue;
        for (size_t i = 0; i < (compRef ? comp->values.size() : 1); i++) {
            std::string idx = compRef ? std::to_string(i) : "";
            ps["values[" + idx + "]"] = {PropertyType::Custom, UpdateFlags_None, (void*)&defValue, compRef ? (void*)&comp->values[i] : nullptr};
        }
    }

    void enumerateSpriteAnimationProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kSpriteAnimationProperties);
    }

    void enumerateAnimationProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        AnimationComponent* comp = static_cast<AnimationComponent*>(compRef);

        enumerateFromDescriptors(compRef, ps, kAnimationProperties);

        size_t actionCount = compRef ? comp->actions.size() : 0;
        static size_t defActionCount = 0;
        ps["actionFrameCount"] = {PropertyType::UInt, UpdateFlags_None, (void*)&defActionCount, compRef ? (void*)&actionCount : nullptr};

        if (compRef) {
            static Entity defEntity = NULL_ENTITY;
            for (size_t i = 0; i < comp->actions.size(); i++) {
                ps["actions[" + std::to_string(i) + "].action"] = {PropertyType::Entity, UpdateFlags_None, (void*)&defEntity, (void*)&comp->actions[i].action};
            }
        }
    }

    void enumerateTransformProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kTransformProperties);
    }

    void enumerateUIProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kUIProperties);
        UIComponent& def = getDefaultComponent<UIComponent>();
        UIComponent* ui = static_cast<UIComponent*>(comp);
        for (const char* field : kComponentStencilFields) {
            ps[field] = getStencilColorWriteProperty(ui ? &ui->stencil : nullptr, &def.stencil, ui ? &ui->colorWrite : nullptr,
                                                     &def.colorWrite, field, UpdateFlags_Shader_Reload);
        }
    }

    void enumerateRectTransformProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kRectTransformProperties);
    }

    void enumerateImageProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kImageProperties);
    }

    void enumerateButtonProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kButtonProperties);
    }

    void enumerateScrollbarProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kScrollbarProperties);
    }

    void enumerateProgressbarProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kProgressbarProperties);
    }

    void enumerateInputFieldProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kInputFieldProperties);
    }

    void enumeratePanelProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kPanelProperties);
    }

    void enumerateSpriteProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        SpriteComponent* comp = static_cast<SpriteComponent*>(compRef);
        SpriteComponent& def = getDefaultComponent<SpriteComponent>();

        enumerateFromDescriptors(compRef, ps, kSpriteTopProperties);

        ps["numFramesRect"] = {PropertyType::UInt, UpdateFlags_None, (void*)&def.numFramesRect, compRef ? (void*)&comp->numFramesRect : nullptr};

        for (unsigned int i = 0; i < (compRef ? comp->numFramesRect : 0); i++) {
            std::string idx = std::to_string(i);

            SpriteFrameData& frame = comp->framesRect[i];
            SpriteFrameData& defFrame = def.framesRect[0];

            ps["framesRect[" + idx + "].name"] = {PropertyType::String, UpdateFlags_None, (void*)&defFrame.name, (void*)&frame.name};
            ps["framesRect[" + idx + "].rect"] = {PropertyType::Vector4, UpdateFlags_None, (void*)&defFrame.rect, (void*)&frame.rect};
        }
    }

    void enumerateTilemapProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        TilemapComponent* comp = static_cast<TilemapComponent*>(compRef);
        TilemapComponent& def = getDefaultComponent<TilemapComponent>();

        enumerateFromDescriptors(compRef, ps, kTilemapTopProperties);

        ps["numTilesRect"] = {PropertyType::UInt, UpdateFlags_Tilemap, (void*)&def.numTilesRect, compRef ? (void*)&comp->numTilesRect : nullptr};
        ps["numTiles"] = {PropertyType::UInt, UpdateFlags_Tilemap, (void*)&def.numTiles, compRef ? (void*)&comp->numTiles : nullptr};

        for (unsigned int i = 0; i < (compRef ? comp->numTilesRect : 0); i++) {
            std::string idx = std::to_string(i);

            TileRectData& tileRect = comp->tilesRect[i];
            TileRectData& defTileRect = def.tilesRect[0];

            ps["tilesRect[" + idx + "].name"] = {PropertyType::String, UpdateFlags_Tilemap, (void*)&defTileRect.name, (void*)&tileRect.name};
            ps["tilesRect[" + idx + "].submeshId"] = {PropertyType::Int, UpdateFlags_Tilemap, (void*)&defTileRect.submeshId, (void*)&tileRect.submeshId};
            ps["tilesRect[" + idx + "].rect"] = {PropertyType::Vector4, UpdateFlags_Tilemap, (void*)&defTileRect.rect, (void*)&tileRect.rect};
        }

        for (unsigned int i = 0; i < (compRef ? comp->numTiles : 0); i++) {
            std::string idx = std::to_string(i);

            TileData& tile = comp->tiles[i];
            TileData& defTile = def.tiles[0];

            ps["tiles[" + idx + "].name"] = {PropertyType::String, UpdateFlags_Tilemap, (void*)&defTile.name, (void*)&tile.name};
            ps["tiles[" + idx + "].rectId"] = {PropertyType::Int, UpdateFlags_Tilemap, (void*)&defTile.rectId, (void*)&tile.rectId};
            ps["tiles[" + idx + "].position"] = {PropertyType::Vector2, UpdateFlags_Tilemap, (void*)&defTile.position, (void*)&tile.position};
            ps["tiles[" + idx + "].width"] = {PropertyType::Float, UpdateFlags_Tilemap, (void*)&defTile.width, (void*)&tile.width};
            ps["tiles[" + idx + "].height"] = {PropertyType::Float, UpdateFlags_Tilemap, (void*)&defTile.height, (void*)&tile.height};
        }
    }

    void enumerateTerrainProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        TerrainComponent* comp = static_cast<TerrainComponent*>(compRef);
        TerrainComponent& def = getDefaultComponent<TerrainComponent>();

        enumerateFromDescriptors(compRef, ps, kTerrainProperties);

        ps["ranges"] = {PropertyType::Custom, UpdateFlags_Terrain, (void*)&def.ranges, compRef ? (void*)&comp->ranges : nullptr};

        static float defValue = 0.0f;
        for (size_t i = 0; i < (compRef ? comp->ranges.size() : 1); i++) {
            std::string idx = compRef ? std::to_string(i) : "";
            ps["ranges[" + idx + "]"] = {PropertyType::Float, UpdateFlags_Terrain, (void*)&defValue, compRef ? (void*)&comp->ranges[i] : nullptr};
        }

        ps["blendMaps"] = {PropertyType::Custom, UpdateFlags_Terrain_Texture, (void*)&def.blendMaps, compRef ? (void*)&comp->blendMaps : nullptr};
        ps["surfaceLayers"] = {PropertyType::Custom, UpdateFlags_Terrain_Texture, (void*)&def.surfaceLayers, compRef ? (void*)&comp->surfaceLayers : nullptr};

        static Texture defTexture;
        for (size_t i = 0; i < (compRef ? comp->blendMaps.size() : 1); i++) {
            std::string idx = compRef ? std::to_string(i) : "";
            ps["blendMaps[" + idx + "]"] = {PropertyType::Texture, UpdateFlags_Terrain_Texture, (void*)&defTexture, compRef ? (void*)&comp->blendMaps[i] : nullptr};
        }

        static TerrainSurfaceLayer defSurfaceLayer;
        for (size_t i = 0; i < (compRef ? comp->surfaceLayers.size() : 1); i++) {
            std::string prefix = "surfaceLayers[" + (compRef ? std::to_string(i) : "") + "]";
            TerrainSurfaceLayer* layer = compRef ? &comp->surfaceLayers[i] : nullptr;
            auto field = [&](const char* name, PropertyType type, void* defRef, void* ref) {
                ps[prefix + name] = {type, UpdateFlags_Terrain_Texture, defRef, layer ? ref : nullptr};
            };
            field(".pbr", PropertyType::Bool, (void*)&defSurfaceLayer.pbr, layer ? (void*)&layer->pbr : nullptr);
            field(".colorTexture", PropertyType::Texture, (void*)&defSurfaceLayer.colorTexture, layer ? (void*)&layer->colorTexture : nullptr);
            field(".normalTexture", PropertyType::Texture, (void*)&defSurfaceLayer.normalTexture, layer ? (void*)&layer->normalTexture : nullptr);
            field(".roughnessTexture", PropertyType::Texture, (void*)&defSurfaceLayer.roughnessTexture, layer ? (void*)&layer->roughnessTexture : nullptr);
            field(".metallicTexture", PropertyType::Texture, (void*)&defSurfaceLayer.metallicTexture, layer ? (void*)&layer->metallicTexture : nullptr);
            field(".occlusionTexture", PropertyType::Texture, (void*)&defSurfaceLayer.occlusionTexture, layer ? (void*)&layer->occlusionTexture : nullptr);
            field(".heightTexture", PropertyType::Texture, (void*)&defSurfaceLayer.heightTexture, layer ? (void*)&layer->heightTexture : nullptr);
            field(".colorFactor", PropertyType::Vector4, (void*)&defSurfaceLayer.colorFactor, layer ? (void*)&layer->colorFactor : nullptr);
            field(".normalStrength", PropertyType::Float, (void*)&defSurfaceLayer.normalStrength, layer ? (void*)&layer->normalStrength : nullptr);
            field(".roughnessFactor", PropertyType::Float, (void*)&defSurfaceLayer.roughnessFactor, layer ? (void*)&layer->roughnessFactor : nullptr);
            field(".metallicFactor", PropertyType::Float, (void*)&defSurfaceLayer.metallicFactor, layer ? (void*)&layer->metallicFactor : nullptr);
            field(".occlusionStrength", PropertyType::Float, (void*)&defSurfaceLayer.occlusionStrength, layer ? (void*)&layer->occlusionStrength : nullptr);
            field(".uvScale", PropertyType::Vector2, (void*)&defSurfaceLayer.uvScale, layer ? (void*)&layer->uvScale : nullptr);
            field(".uvOffset", PropertyType::Vector2, (void*)&defSurfaceLayer.uvOffset, layer ? (void*)&layer->uvOffset : nullptr);
            field(".stochasticTiling", PropertyType::Bool, (void*)&defSurfaceLayer.stochasticTiling, layer ? (void*)&layer->stochasticTiling : nullptr);
            field(".stochasticCellSize", PropertyType::Float, (void*)&defSurfaceLayer.stochasticCellSize, layer ? (void*)&layer->stochasticCellSize : nullptr);
            field(".stochasticBlendSharpness", PropertyType::Float, (void*)&defSurfaceLayer.stochasticBlendSharpness, layer ? (void*)&layer->stochasticBlendSharpness : nullptr);
            field(".stochasticContrast", PropertyType::Float, (void*)&defSurfaceLayer.stochasticContrast, layer ? (void*)&layer->stochasticContrast : nullptr);
            field(".stochasticRotation", PropertyType::Float, (void*)&defSurfaceLayer.stochasticRotation, layer ? (void*)&layer->stochasticRotation : nullptr);
            field(".hexTiling", PropertyType::Bool, (void*)&defSurfaceLayer.hexTiling, layer ? (void*)&layer->hexTiling : nullptr);
            field(".hexRotation", PropertyType::Float, (void*)&defSurfaceLayer.hexRotation, layer ? (void*)&layer->hexRotation : nullptr);
        }

        ps["foliageLayers"] = {PropertyType::Custom, UpdateFlags_Terrain_Foliage, (void*)&def.foliageLayers, compRef ? (void*)&comp->foliageLayers : nullptr};

        static TerrainFoliageLayer defLayer;
        for (size_t i = 0; i < (compRef ? comp->foliageLayers.size() : 1); i++) {
            std::string prefix = "foliageLayers[" + (compRef ? std::to_string(i) : "") + "]";
            TerrainFoliageLayer* layer = compRef ? &comp->foliageLayers[i] : nullptr;
            auto field = [&](const char* name, PropertyType type, void* defRef, void* ref) {
                ps[prefix + name] = {type, UpdateFlags_Terrain_Foliage, defRef, layer ? ref : nullptr};
            };
            field(".meshPath", PropertyType::Asset, (void*)&defLayer.meshPath, layer ? (void*)&layer->meshPath : nullptr);
            field(".densityMap", PropertyType::Texture, (void*)&defLayer.densityMap, layer ? (void*)&layer->densityMap : nullptr);
            field(".density", PropertyType::Float, (void*)&defLayer.density, layer ? (void*)&layer->density : nullptr);
            field(".minScale", PropertyType::Float, (void*)&defLayer.minScale, layer ? (void*)&layer->minScale : nullptr);
            field(".maxScale", PropertyType::Float, (void*)&defLayer.maxScale, layer ? (void*)&layer->maxScale : nullptr);
            field(".rotationJitter", PropertyType::Float, (void*)&defLayer.rotationJitter, layer ? (void*)&layer->rotationJitter : nullptr);
            field(".alignToNormal", PropertyType::Float, (void*)&defLayer.alignToNormal, layer ? (void*)&layer->alignToNormal : nullptr);
            field(".minSlope", PropertyType::Float, (void*)&defLayer.minSlope, layer ? (void*)&layer->minSlope : nullptr);
            field(".maxSlope", PropertyType::Float, (void*)&defLayer.maxSlope, layer ? (void*)&layer->maxSlope : nullptr);
            field(".minHeight", PropertyType::Float, (void*)&defLayer.minHeight, layer ? (void*)&layer->minHeight : nullptr);
            field(".maxHeight", PropertyType::Float, (void*)&defLayer.maxHeight, layer ? (void*)&layer->maxHeight : nullptr);
            field(".drawDistance", PropertyType::Float, (void*)&defLayer.drawDistance, layer ? (void*)&layer->drawDistance : nullptr);
            field(".seed", PropertyType::UInt, (void*)&defLayer.seed, layer ? (void*)&layer->seed : nullptr);
            field(".castShadows", PropertyType::Bool, (void*)&defLayer.castShadows, layer ? (void*)&layer->castShadows : nullptr);
        }
    }

    void enumerateLightProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kLightProperties);
    }

    void enumerateFogProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kFogProperties);
    }

    void enumerateLocalVolumetricFogProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kLocalVolumetricFogProperties);
    }

    void enumerateVolumetricCloudsProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kVolumetricCloudsProperties);
    }

    void enumerateMirrorProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kMirrorProperties);
    }

    void enumerateReflectionProbeProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kReflectionProbeProperties);
    }

    void enumerateUIDocumentProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kUIDocumentProperties);
    }

    void enumerateFluidSimulationProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kFluidSimulationProperties);
    }

    void enumerateLatticeProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        LatticeComponent* comp = static_cast<LatticeComponent*>(compRef);

        enumerateFromDescriptors(compRef, ps, kLatticeProperties);

        static Vector3 defOffset = Vector3::ZERO;
        for (size_t i = 0; i < (compRef ? comp->offsets.size() : 1); i++) {
            std::string idx = compRef ? std::to_string(i) : "";
            ps["offsets[" + idx + "]"] = {PropertyType::Vector3, UpdateFlags_Lattice, (void*)&defOffset, compRef ? (void*)&comp->offsets[i] : nullptr};
        }
    }

    void enumerateLatticeModifierProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        LatticeModifierComponent* comp = static_cast<LatticeModifierComponent*>(compRef);

        enumerateFromDescriptors(compRef, ps, kLatticeModifierProperties);

        static LatticeBinding defBinding;
        for (size_t i = 0; i < (compRef ? comp->bindings.size() : 1); i++) {
            std::string prefix = "bindings[" + (compRef ? std::to_string(i) : "") + "].";
            LatticeBinding& binding = compRef ? comp->bindings[i] : defBinding;
            for (const char* field : {"lattice", "weight", "stage", "submesh", "mask"}) {
                PropertyData property = getLatticeBindingPropertyFast(binding, field);
                if (!compRef) property.ref = nullptr;
                ps[prefix + field] = property;
            }
        }
    }

    void enumerateLatticeFollowerProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kLatticeFollowerProperties);
    }

    void enumerateLatticeTracksProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        LatticeTracksComponent* comp = static_cast<LatticeTracksComponent*>(compRef);

        enumerateFromDescriptors(compRef, ps, kLatticeTracksProperties);

        static int defPoint = 0;
        for (size_t i = 0; i < (compRef ? comp->points.size() : 1); i++) {
            std::string idx = compRef ? std::to_string(i) : "";
            ps["points[" + idx + "]"] = {PropertyType::Int, UpdateFlags_None, (void*)&defPoint, compRef ? (void*)&comp->points[i] : nullptr};
        }
        static std::vector<Vector3> defValue;
        for (size_t i = 0; i < (compRef ? comp->values.size() : 1); i++) {
            std::string idx = compRef ? std::to_string(i) : "";
            ps["values[" + idx + "]"] = {PropertyType::Custom, UpdateFlags_None, (void*)&defValue, compRef ? (void*)&comp->values[i] : nullptr};
        }
    }

    void enumeratePoseAnimatorProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kPoseAnimatorProperties);
    }

    void enumerateAnimationPlayerProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        AnimationPlayerComponent* comp = static_cast<AnimationPlayerComponent*>(compRef);

        enumerateFromDescriptors(compRef, ps, kAnimationPlayerProperties);

        // without a component: the "clips[]" templates
        static AnimationPlayerClip defClip;
        for (size_t i = 0; i < (compRef ? comp->clips.size() : 1); i++) {
            const std::string prefix = "clips[" + (compRef ? std::to_string(i) : std::string()) + "]";
            AnimationPlayerClip* entry = compRef ? &comp->clips[i] : nullptr;
            ps[prefix + ".clip"] = {PropertyType::String, UpdateFlags_AnimationPlayer_Reload, (void*)&defClip.clip, entry ? (void*)&entry->clip : nullptr};
            ps[prefix + ".guid"] = {PropertyType::String, UpdateFlags_AnimationPlayer_Reload, (void*)&defClip.guid, entry ? (void*)&entry->guid : nullptr};
            ps[prefix + ".name"] = {PropertyType::String, UpdateFlags_AnimationPlayer_Reload, (void*)&defClip.name, entry ? (void*)&entry->name : nullptr};
            ps[prefix + ".wrap"] = {PropertyType::Custom, UpdateFlags_AnimationPlayer_Reload, (void*)&defClip.wrap, entry ? (void*)&entry->wrap : nullptr};
        }
    }

    void enumerateLight2DProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kLight2DProperties);
    }

    void enumerateShadowCaster2DProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kShadowCaster2DProperties);
    }

    void enumerateCameraProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kCameraProperties);
    }

    void enumerateAudioProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kAudioSourceProperties);
    }

    void enumerateSkyProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kSkyProperties);
    }

    void enumerateTextProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kTextProperties);
    }

    void enumerateJoint2DProperties(void* comp, std::map<std::string, PropertyData>& ps) {
        enumerateFromDescriptors(comp, ps, kJoint2DProperties);
    }

    void enumerateJoint3DProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        Joint3DComponent* comp = static_cast<Joint3DComponent*>(compRef);
        Joint3DComponent& def = getDefaultComponent<Joint3DComponent>();

        enumerateFromDescriptors(compRef, ps, kJoint3DProperties);

        ps["pathPoints"] = {PropertyType::Custom, UpdateFlags_Joint3D, (void*)&def.pathPoints, compRef ? (void*)&comp->pathPoints : nullptr};

        for (size_t p = 0; p < (compRef ? comp->pathPoints.size() : 1); p++) {
            std::string idx = compRef ? std::to_string(p) : "";
            ps["pathPoints[" + idx + "]"] = {PropertyType::Vector3, UpdateFlags_Joint3D, (void*)&def.pathPosition, compRef ? (void*)&comp->pathPoints[p] : nullptr};
        }
    }

    void enumerateRigidbody2DProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        Rigidbody2DComponent* comp = static_cast<Rigidbody2DComponent*>(compRef);
        Rigidbody2DComponent& def = getDefaultComponent<Rigidbody2DComponent>();

        ps["type"] = {PropertyType::Enum, UpdateFlags_Rigidbody2D, (void*)&def.type, compRef ? (void*)&comp->type : nullptr};
        ps["numColliders"] = {PropertyType::UInt, UpdateFlags_Rigidbody2D, (void*)&def.numColliders, compRef ? (void*)&comp->numColliders : nullptr};

        for (int s = 0; s < (compRef ? (int)comp->numColliders : 1); s++) {
            std::string idx = compRef ? std::to_string(s) : "";

            Collider2D& shape = compRef ? comp->colliders[s] : def.colliders[0];
            Collider2D& defShape = def.colliders[0];

            ps["colliders[" + idx + "]"] = {PropertyType::Custom, UpdateFlags_Rigidbody2D, (void*)&defShape, compRef ? (void*)&shape : nullptr};
            ps["colliders[" + idx + "].type"] = {PropertyType::Enum, UpdateFlags_Rigidbody2D, (void*)&defShape.type, compRef ? (void*)&shape.type : nullptr};
            ps["colliders[" + idx + "].pointA"] = {PropertyType::Vector2, UpdateFlags_Rigidbody2D, (void*)&defShape.pointA, compRef ? (void*)&shape.pointA : nullptr};
            ps["colliders[" + idx + "].pointB"] = {PropertyType::Vector2, UpdateFlags_Rigidbody2D, (void*)&defShape.pointB, compRef ? (void*)&shape.pointB : nullptr};
            ps["colliders[" + idx + "].radius"] = {PropertyType::Float, UpdateFlags_Rigidbody2D, (void*)&defShape.radius, compRef ? (void*)&shape.radius : nullptr};
            ps["colliders[" + idx + "].loop"] = {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defShape.loop, compRef ? (void*)&shape.loop : nullptr};
            ps["colliders[" + idx + "].density"] = {PropertyType::Float, UpdateFlags_Rigidbody2D, (void*)&defShape.density, compRef ? (void*)&shape.density : nullptr};
            ps["colliders[" + idx + "].friction"] = {PropertyType::Float, UpdateFlags_Rigidbody2D, (void*)&defShape.friction, compRef ? (void*)&shape.friction : nullptr};
            ps["colliders[" + idx + "].restitution"] = {PropertyType::Float, UpdateFlags_Rigidbody2D, (void*)&defShape.restitution, compRef ? (void*)&shape.restitution : nullptr};
            ps["colliders[" + idx + "].sensor"] = {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defShape.sensor, compRef ? (void*)&shape.sensor : nullptr};
            ps["colliders[" + idx + "].enableHitEvents"] = {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defShape.enableHitEvents, compRef ? (void*)&shape.enableHitEvents : nullptr};
            ps["colliders[" + idx + "].contactEvents"] = {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defShape.contactEvents, compRef ? (void*)&shape.contactEvents : nullptr};
            ps["colliders[" + idx + "].preSolveEvents"] = {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defShape.preSolveEvents, compRef ? (void*)&shape.preSolveEvents : nullptr};
            ps["colliders[" + idx + "].sensorEvents"] = {PropertyType::Bool, UpdateFlags_Rigidbody2D, (void*)&defShape.sensorEvents, compRef ? (void*)&shape.sensorEvents : nullptr};

            for (int v = 0; v < (compRef ? (int)shape.numVertices : 1); v++) {
                std::string vidx = compRef ? std::to_string(v) : "";
                ps["colliders[" + idx + "].vertices[" + vidx + "]"] = {PropertyType::Vector2, UpdateFlags_Rigidbody2D, (void*)&defShape.vertices[0], compRef ? (void*)&shape.vertices[v] : nullptr};
            }
        }
    }

    void enumerateRigidbodyProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        RigidbodyComponent* comp = static_cast<RigidbodyComponent*>(compRef);
        RigidbodyComponent& def = getDefaultComponent<RigidbodyComponent>();

        ps["type"] = {PropertyType::Enum, UpdateFlags_Rigidbody, (void*)&def.type, compRef ? (void*)&comp->type : nullptr};
        ps["motionQuality"] = {PropertyType::Enum, UpdateFlags_Rigidbody, (void*)&def.motionQuality, compRef ? (void*)&comp->motionQuality : nullptr};
        ps["sensor"] = {PropertyType::Bool, UpdateFlags_Rigidbody, (void*)&def.sensor, compRef ? (void*)&comp->sensor : nullptr};
        ps["gravityFactor"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&def.gravityFactor, compRef ? (void*)&comp->gravityFactor : nullptr};
        ps["friction"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&def.friction, compRef ? (void*)&comp->friction : nullptr};
        ps["restitution"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&def.restitution, compRef ? (void*)&comp->restitution : nullptr};
        ps["buoyancy"] = {PropertyType::Float, UpdateFlags_None, (void*)&def.buoyancy, compRef ? (void*)&comp->buoyancy : nullptr};
        ps["waterDrag"] = {PropertyType::Float, UpdateFlags_None, (void*)&def.waterDrag, compRef ? (void*)&comp->waterDrag : nullptr};
        ps["waterAngularDrag"] = {PropertyType::Float, UpdateFlags_None, (void*)&def.waterAngularDrag, compRef ? (void*)&comp->waterAngularDrag : nullptr};
        ps["numColliders"] = {PropertyType::UInt, UpdateFlags_Rigidbody, (void*)&def.numColliders, compRef ? (void*)&comp->numColliders : nullptr};
        ps["overrideMassProperties"] = {PropertyType::Bool, UpdateFlags_Rigidbody, (void*)&def.overrideMassProperties, compRef ? (void*)&comp->overrideMassProperties : nullptr};
        ps["solidBoxSize"] = {PropertyType::Vector3, UpdateFlags_Rigidbody, (void*)&def.solidBoxSize, compRef ? (void*)&comp->solidBoxSize : nullptr};
        ps["solidBoxDensity"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&def.solidBoxDensity, compRef ? (void*)&comp->solidBoxDensity : nullptr};

        for (int s = 0; s < (compRef ? (int)comp->numColliders : 1); s++) {
            std::string idx = compRef ? std::to_string(s) : "";

            Collider3D& shape = compRef ? comp->colliders[s] : def.colliders[0];
            Collider3D& defShape = def.colliders[0];

            ps["colliders[" + idx + "]"] = {PropertyType::Custom, UpdateFlags_Rigidbody, (void*)&defShape, compRef ? (void*)&shape : nullptr};
            ps["colliders[" + idx + "].type"] = {PropertyType::Enum, UpdateFlags_Rigidbody, (void*)&defShape.type, compRef ? (void*)&shape.type : nullptr};
            ps["colliders[" + idx + "].position"] = {PropertyType::Vector3, UpdateFlags_Rigidbody, (void*)&defShape.position, compRef ? (void*)&shape.position : nullptr};
            ps["colliders[" + idx + "].rotation"] = {PropertyType::Quat, UpdateFlags_Rigidbody, (void*)&defShape.rotation, compRef ? (void*)&shape.rotation : nullptr};
            ps["colliders[" + idx + "].width"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defShape.width, compRef ? (void*)&shape.width : nullptr};
            ps["colliders[" + idx + "].height"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defShape.height, compRef ? (void*)&shape.height : nullptr};
            ps["colliders[" + idx + "].depth"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defShape.depth, compRef ? (void*)&shape.depth : nullptr};
            ps["colliders[" + idx + "].radius"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defShape.radius, compRef ? (void*)&shape.radius : nullptr};
            ps["colliders[" + idx + "].halfHeight"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defShape.halfHeight, compRef ? (void*)&shape.halfHeight : nullptr};
            ps["colliders[" + idx + "].topRadius"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defShape.topRadius, compRef ? (void*)&shape.topRadius : nullptr};
            ps["colliders[" + idx + "].bottomRadius"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defShape.bottomRadius, compRef ? (void*)&shape.bottomRadius : nullptr};
            ps["colliders[" + idx + "].density"] = {PropertyType::Float, UpdateFlags_Rigidbody, (void*)&defShape.density, compRef ? (void*)&shape.density : nullptr};
            ps["colliders[" + idx + "].source"] = {PropertyType::Enum, UpdateFlags_Rigidbody, (void*)&defShape.source, compRef ? (void*)&shape.source : nullptr};
            ps["colliders[" + idx + "].sourceEntity"] = {PropertyType::Entity, UpdateFlags_Rigidbody, (void*)&defShape.sourceEntity, compRef ? (void*)&shape.sourceEntity : nullptr};
            ps["colliders[" + idx + "].samplesSize"] = {PropertyType::UInt, UpdateFlags_Rigidbody, (void*)&defShape.samplesSize, compRef ? (void*)&shape.samplesSize : nullptr};
            ps["colliders[" + idx + "].numVertices"] = {PropertyType::UInt, UpdateFlags_Rigidbody, (void*)&defShape.numVertices, compRef ? (void*)&shape.numVertices : nullptr};
            ps["colliders[" + idx + "].numIndices"] = {PropertyType::UInt, UpdateFlags_Rigidbody, (void*)&defShape.numIndices, compRef ? (void*)&shape.numIndices : nullptr};

            for (int v = 0; v < (compRef ? (int)shape.numVertices : 1); v++) {
                std::string vidx = compRef ? std::to_string(v) : "";
                ps["colliders[" + idx + "].vertices[" + vidx + "]"] = {PropertyType::Vector3, UpdateFlags_Rigidbody, (void*)&defShape.vertices[0], compRef ? (void*)&shape.vertices[v] : nullptr};
            }
        }
    }

    void enumerateMeshProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        MeshComponent* comp = static_cast<MeshComponent*>(compRef);
        MeshComponent& def = getDefaultComponent<MeshComponent>();

        enumerateFromDescriptors(compRef, ps, kMeshTopProperties);

        static float defWeight = 0.0f;
        for (int w = 0; w < (compRef ? MAX_MORPHTARGETS : 1); w++) {
            std::string idx = compRef ? std::to_string(w) : "";
            ps["morphWeights[" + idx + "]"] = {PropertyType::Float, UpdateFlags_None, (void*)&defWeight, compRef ? (void*)&comp->morphWeights[w] : nullptr};
        }

        for (int s = 0; s < (compRef ? (int)comp->submeshes.size() : 1); s++) {
            std::string idx = compRef ? std::to_string(s) : "";

            Submesh& sub = compRef ? comp->submeshes[s] : def.submeshes[0];
            Submesh& defSub = def.submeshes[0];

            ps["submeshes[" + idx + "].material"] = {PropertyType::Material, UpdateFlags_Mesh_Texture | UpdateFlags_Mesh_Reload | UpdateFlags_Shader_Uniforms, (void*)&defSub.material, compRef ? (void*)&sub.material : nullptr};
            for (const char* field : kMaterialLibraryFields) {
                ps["submeshes[" + idx + "].material." + field] = getMaterialLibraryProperty(sub.material, &defSub.material, field, compRef != nullptr);
            }
            ps["submeshes[" + idx + "].material.emissiveStrength"] = getMaterialLibraryProperty(sub.material, &defSub.material, "emissiveStrength", compRef != nullptr);
            for (const PbrField& field : MaterialPbr::getFields()) {
                ps["submeshes[" + idx + "].material.pbr." + MaterialPbr::getFieldPath(field)] =
                    getMaterialPbrProperty(sub.material, &defSub.material, MaterialPbr::getFieldPath(field), compRef != nullptr);
            }
            ps["submeshes[" + idx + "].material.name"] = {PropertyType::Asset, UpdateFlags_None, (void*)&defSub.material.name, compRef ? (void*)&sub.material.name : nullptr};
            ps["submeshes[" + idx + "].material.baseColorFactor"] = {PropertyType::Vector4, UpdateFlags_None, (void*)&defSub.material.baseColorFactor, compRef ? (void*)&sub.material.baseColorFactor : nullptr};
            ps["submeshes[" + idx + "].material.metallicFactor"] = {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.metallicFactor, compRef ? (void*)&sub.material.metallicFactor : nullptr};
            ps["submeshes[" + idx + "].material.roughnessFactor"] = {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.roughnessFactor, compRef ? (void*)&sub.material.roughnessFactor : nullptr};
            ps["submeshes[" + idx + "].material.alphaCutoff"] = {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.alphaCutoff, compRef ? (void*)&sub.material.alphaCutoff : nullptr};
            ps["submeshes[" + idx + "].material.foliageTransmission"] = {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.foliageTransmission, compRef ? (void*)&sub.material.foliageTransmission : nullptr};
            ps["submeshes[" + idx + "].material.alphaMode"] = {PropertyType::Enum, UpdateFlags_Mesh_Reload, (void*)&defSub.material.alphaMode, compRef ? (void*)&sub.material.alphaMode : nullptr};
            ps["submeshes[" + idx + "].material.stochasticTiling"] = {PropertyType::Bool, UpdateFlags_Mesh_Reload, (void*)&defSub.material.stochasticTiling, compRef ? (void*)&sub.material.stochasticTiling : nullptr};
            ps["submeshes[" + idx + "].material.stochasticCellSize"] = {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.stochasticCellSize, compRef ? (void*)&sub.material.stochasticCellSize : nullptr};
            ps["submeshes[" + idx + "].material.stochasticBlendSharpness"] = {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.stochasticBlendSharpness, compRef ? (void*)&sub.material.stochasticBlendSharpness : nullptr};
            ps["submeshes[" + idx + "].material.stochasticContrast"] = {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.stochasticContrast, compRef ? (void*)&sub.material.stochasticContrast : nullptr};
            ps["submeshes[" + idx + "].material.stochasticRotation"] = {PropertyType::Float, UpdateFlags_None, (void*)&defSub.material.stochasticRotation, compRef ? (void*)&sub.material.stochasticRotation : nullptr};
            ps["submeshes[" + idx + "].material.emissiveFactor"] = {PropertyType::Vector3, UpdateFlags_None, (void*)&defSub.material.emissiveFactor, compRef ? (void*)&sub.material.emissiveFactor : nullptr};
            ps["submeshes[" + idx + "].material.baseColorTexture"] = {PropertyType::Texture, UpdateFlags_Mesh_Texture, (void*)&defSub.material.baseColorTexture, compRef ? (void*)&sub.material.baseColorTexture : nullptr};
            ps["submeshes[" + idx + "].material.emissiveTexture"] = {PropertyType::Texture, UpdateFlags_Mesh_Texture, (void*)&defSub.material.emissiveTexture, compRef ? (void*)&sub.material.emissiveTexture : nullptr};
            ps["submeshes[" + idx + "].material.metallicRoughnessTexture"] = {PropertyType::Texture, UpdateFlags_Mesh_Texture, (void*)&defSub.material.metallicRoughnessTexture, compRef ? (void*)&sub.material.metallicRoughnessTexture : nullptr};
            ps["submeshes[" + idx + "].material.occlusionTexture"] = {PropertyType::Texture, UpdateFlags_Mesh_Texture, (void*)&defSub.material.occlusionTexture, compRef ? (void*)&sub.material.occlusionTexture : nullptr};
            ps["submeshes[" + idx + "].material.normalTexture"] = {PropertyType::Texture, UpdateFlags_Mesh_Texture, (void*)&defSub.material.normalTexture, compRef ? (void*)&sub.material.normalTexture : nullptr};

            ps["submeshes[" + idx + "].primitiveType"] = {PropertyType::Enum, UpdateFlags_Mesh_Reload, (void*)&defSub.primitiveType, compRef ? (void*)&sub.primitiveType : nullptr};
            ps["submeshes[" + idx + "].faceCulling"] = {PropertyType::Bool, UpdateFlags_Mesh_Reload, (void*)&defSub.faceCulling, compRef ? (void*)&sub.faceCulling : nullptr};
            ps["submeshes[" + idx + "].textureShadow"] = {PropertyType::Bool, UpdateFlags_Mesh_Reload, (void*)&defSub.textureShadow, compRef ? (void*)&sub.textureShadow : nullptr};
            ps["submeshes[" + idx + "].textureRect"] = {PropertyType::Vector4, UpdateFlags_None, (void*)&defSub.textureRect, compRef ? (void*)&sub.textureRect : nullptr};
        }
    }

    void enumerateLayoutGroupProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        LayoutGroupComponent* comp = static_cast<LayoutGroupComponent*>(compRef);
        LayoutGroupComponent& def = getDefaultComponent<LayoutGroupComponent>();

        enumerateFromDescriptors(compRef, ps, kLayoutGroupTopProperties);

        for (int b = 0; b < (compRef ? MAX_CONTAINER_BOXES : 1); b++) {
            std::string idx = compRef ? std::to_string(b) : "";

            ContainerBox& box = compRef ? comp->boxes[b] : def.boxes[0];
            ContainerBox& defBox = def.boxes[0];
            float* defRect = defBox.rect.ptr();
            float* boxRect = compRef ? box.rect.ptr() : nullptr;

            ps["boxes[" + idx + "].layout"] = {PropertyType::UInt, UpdateFlags_None, (void*)&defBox.layout, compRef ? (void*)&box.layout : nullptr};
            ps["boxes[" + idx + "].expand"] = {PropertyType::Bool, UpdateFlags_Layout_Sizes, (void*)&defBox.expand, compRef ? (void*)&box.expand : nullptr};
            ps["boxes[" + idx + "].rect.x"] = {PropertyType::Float, UpdateFlags_None, (void*)&defRect[0], compRef ? (void*)&boxRect[0] : nullptr};
            ps["boxes[" + idx + "].rect.y"] = {PropertyType::Float, UpdateFlags_None, (void*)&defRect[1], compRef ? (void*)&boxRect[1] : nullptr};
            ps["boxes[" + idx + "].rect.width"] = {PropertyType::Float, UpdateFlags_None, (void*)&defRect[2], compRef ? (void*)&boxRect[2] : nullptr};
            ps["boxes[" + idx + "].rect.height"] = {PropertyType::Float, UpdateFlags_None, (void*)&defRect[3], compRef ? (void*)&boxRect[3] : nullptr};
        }
    }

    void enumerateScriptProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        ScriptComponent* comp = static_cast<ScriptComponent*>(compRef);

        ps["scripts"] = {PropertyType::Custom, UpdateFlags_None, nullptr, compRef ? (void*)&comp->scripts : nullptr};

        if (compRef && comp) {
            for (size_t i = 0; i < comp->scripts.size(); i++) {
                auto& script = comp->scripts[i];

                std::string enabledKey = "scripts[" + std::to_string(i) + "].enabled";
                ps[enabledKey] = {PropertyType::Bool, UpdateFlags_None, nullptr, (void*)&script.enabled};

                for (auto& prop : script.properties) {
                    std::string key = "scripts[" + std::to_string(i) + "]." + prop.name;
                    PropertyData propData;
                    propData.type = Catalog::scriptPropertyTypeToPropertyType(prop.type);
                    propData.updateFlags = UpdateFlags_None;

                    switch (prop.type) {
                        case ScriptPropertyType::Bool:
                            propData.ref = &prop.valueRef<bool>();
                            propData.def = &prop.defaultValueRef<bool>();
                            break;
                        case ScriptPropertyType::Int:
                            propData.ref = &prop.valueRef<int>();
                            propData.def = &prop.defaultValueRef<int>();
                            break;
                        case ScriptPropertyType::Float:
                            propData.ref = &prop.valueRef<float>();
                            propData.def = &prop.defaultValueRef<float>();
                            break;
                        case ScriptPropertyType::String:
                            propData.ref = &prop.valueRef<std::string>();
                            propData.def = &prop.defaultValueRef<std::string>();
                            break;
                        case ScriptPropertyType::Vector2:
                            propData.ref = &prop.valueRef<Vector2>();
                            propData.def = &prop.defaultValueRef<Vector2>();
                            break;
                        case ScriptPropertyType::Vector3:
                        case ScriptPropertyType::Color3:
                            propData.ref = &prop.valueRef<Vector3>();
                            propData.def = &prop.defaultValueRef<Vector3>();
                            break;
                        case ScriptPropertyType::Vector4:
                        case ScriptPropertyType::Color4:
                            propData.ref = &prop.valueRef<Vector4>();
                            propData.def = &prop.defaultValueRef<Vector4>();
                            break;
                        case ScriptPropertyType::EntityReference:
                            propData.ref = &prop.valueRef<EntityReference>().entity;
                            propData.def = &prop.defaultValueRef<EntityReference>().entity;
                            break;
                        case ScriptPropertyType::Prefab:
                            propData.ref = &prop.valueRef<PrefabAsset>();
                            propData.def = &prop.defaultValueRef<PrefabAsset>();
                            break;
                        case ScriptPropertyType::Asset:
                            propData.ref = &prop.valueRef<AssetRef>();
                            propData.def = &prop.defaultValueRef<AssetRef>();
                            break;
                        default:
                            propData.ref = nullptr;
                            propData.def = nullptr;
                            break;
                    }

                    ps[key] = propData;

                    // Also expose sceneId for EntityReference properties
                    if (prop.type == ScriptPropertyType::EntityReference) {
                        static uint32_t defSceneId = 0;
                        std::string sceneIdKey = key + ".sceneId";
                        ps[sceneIdKey] = {PropertyType::UInt, UpdateFlags_None, (void*)&defSceneId, (void*)&prop.valueRef<EntityReference>().sceneId};
                    }
                }
            }
        }
    }

    void enumerateParticlesProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        ParticleSystemComponent* comp = static_cast<ParticleSystemComponent*>(compRef);
        ParticleSystemComponent& def = getDefaultComponent<ParticleSystemComponent>();

        ps["maxParticles"] = {PropertyType::UInt, UpdateFlags_None, &def.maxParticles, comp ? &comp->maxParticles : nullptr};
        ps["emitter"] = {PropertyType::Bool, UpdateFlags_None, &def.emitter, comp ? &comp->emitter : nullptr};
        ps["loop"] = {PropertyType::Bool, UpdateFlags_None, &def.loop, comp ? &comp->loop : nullptr};
        ps["localSpace"] = {PropertyType::Bool, UpdateFlags_None, &def.localSpace, comp ? &comp->localSpace : nullptr};
        ps["rate"] = {PropertyType::Int, UpdateFlags_None, &def.rate, comp ? &comp->rate : nullptr};
        ps["maxPerUpdate"] = {PropertyType::Int, UpdateFlags_None, &def.maxPerUpdate, comp ? &comp->maxPerUpdate : nullptr};
        ps["bursts"] = {PropertyType::Custom, UpdateFlags_None, &def.bursts, comp ? &comp->bursts : nullptr};

        ps["lifeInitializer.minLife"] = {PropertyType::Float, UpdateFlags_None, &def.lifeInitializer.minLife, comp ? &comp->lifeInitializer.minLife : nullptr};
        ps["lifeInitializer.maxLife"] = {PropertyType::Float, UpdateFlags_None, &def.lifeInitializer.maxLife, comp ? &comp->lifeInitializer.maxLife : nullptr};

        ps["positionInitializer.shape"] = {PropertyType::Enum, UpdateFlags_None, &def.positionInitializer.shape, comp ? &comp->positionInitializer.shape : nullptr};
        ps["positionInitializer.minPosition"] = {PropertyType::Vector3, UpdateFlags_None, &def.positionInitializer.minPosition, comp ? &comp->positionInitializer.minPosition : nullptr};
        ps["positionInitializer.maxPosition"] = {PropertyType::Vector3, UpdateFlags_None, &def.positionInitializer.maxPosition, comp ? &comp->positionInitializer.maxPosition : nullptr};
        ps["positionInitializer.radius"] = {PropertyType::Float, UpdateFlags_None, &def.positionInitializer.radius, comp ? &comp->positionInitializer.radius : nullptr};
        ps["positionInitializer.innerRadius"] = {PropertyType::Float, UpdateFlags_None, &def.positionInitializer.innerRadius, comp ? &comp->positionInitializer.innerRadius : nullptr};
        ps["positionInitializer.coneAngle"] = {PropertyType::Float, UpdateFlags_None, &def.positionInitializer.coneAngle, comp ? &comp->positionInitializer.coneAngle : nullptr};
        ps["positionInitializer.coneHeight"] = {PropertyType::Float, UpdateFlags_None, &def.positionInitializer.coneHeight, comp ? &comp->positionInitializer.coneHeight : nullptr};
        ps["positionModifier.fromTime"] = {PropertyType::Float, UpdateFlags_None, &def.positionModifier.fromTime, comp ? &comp->positionModifier.fromTime : nullptr};
        ps["positionModifier.toTime"] = {PropertyType::Float, UpdateFlags_None, &def.positionModifier.toTime, comp ? &comp->positionModifier.toTime : nullptr};
        ps["positionModifier.function"] = {PropertyType::Ease, UpdateFlags_None, &def.positionModifier.function, comp ? &comp->positionModifier.function : nullptr};
        ps["positionModifier.fromPosition"] = {PropertyType::Vector3, UpdateFlags_None, &def.positionModifier.fromPosition, comp ? &comp->positionModifier.fromPosition : nullptr};
        ps["positionModifier.toPosition"] = {PropertyType::Vector3, UpdateFlags_None, &def.positionModifier.toPosition, comp ? &comp->positionModifier.toPosition : nullptr};

        ps["velocityInitializer.minVelocity"] = {PropertyType::Vector3, UpdateFlags_None, &def.velocityInitializer.minVelocity, comp ? &comp->velocityInitializer.minVelocity : nullptr};
        ps["velocityInitializer.maxVelocity"] = {PropertyType::Vector3, UpdateFlags_None, &def.velocityInitializer.maxVelocity, comp ? &comp->velocityInitializer.maxVelocity : nullptr};
        ps["velocityModifier.fromTime"] = {PropertyType::Float, UpdateFlags_None, &def.velocityModifier.fromTime, comp ? &comp->velocityModifier.fromTime : nullptr};
        ps["velocityModifier.toTime"] = {PropertyType::Float, UpdateFlags_None, &def.velocityModifier.toTime, comp ? &comp->velocityModifier.toTime : nullptr};
        ps["velocityModifier.function"] = {PropertyType::Ease, UpdateFlags_None, &def.velocityModifier.function, comp ? &comp->velocityModifier.function : nullptr};
        ps["velocityModifier.fromVelocity"] = {PropertyType::Vector3, UpdateFlags_None, &def.velocityModifier.fromVelocity, comp ? &comp->velocityModifier.fromVelocity : nullptr};
        ps["velocityModifier.toVelocity"] = {PropertyType::Vector3, UpdateFlags_None, &def.velocityModifier.toVelocity, comp ? &comp->velocityModifier.toVelocity : nullptr};

        ps["accelerationInitializer.minAcceleration"] = {PropertyType::Vector3, UpdateFlags_None, &def.accelerationInitializer.minAcceleration, comp ? &comp->accelerationInitializer.minAcceleration : nullptr};
        ps["accelerationInitializer.maxAcceleration"] = {PropertyType::Vector3, UpdateFlags_None, &def.accelerationInitializer.maxAcceleration, comp ? &comp->accelerationInitializer.maxAcceleration : nullptr};
        ps["accelerationModifier.fromTime"] = {PropertyType::Float, UpdateFlags_None, &def.accelerationModifier.fromTime, comp ? &comp->accelerationModifier.fromTime : nullptr};
        ps["accelerationModifier.toTime"] = {PropertyType::Float, UpdateFlags_None, &def.accelerationModifier.toTime, comp ? &comp->accelerationModifier.toTime : nullptr};
        ps["accelerationModifier.function"] = {PropertyType::Ease, UpdateFlags_None, &def.accelerationModifier.function, comp ? &comp->accelerationModifier.function : nullptr};
        ps["accelerationModifier.fromAcceleration"] = {PropertyType::Vector3, UpdateFlags_None, &def.accelerationModifier.fromAcceleration, comp ? &comp->accelerationModifier.fromAcceleration : nullptr};
        ps["accelerationModifier.toAcceleration"] = {PropertyType::Vector3, UpdateFlags_None, &def.accelerationModifier.toAcceleration, comp ? &comp->accelerationModifier.toAcceleration : nullptr};

        ps["colorInitializer.minColor"] = {PropertyType::Vector3, UpdateFlags_None, &def.colorInitializer.minColor, comp ? &comp->colorInitializer.minColor : nullptr};
        ps["colorInitializer.maxColor"] = {PropertyType::Vector3, UpdateFlags_None, &def.colorInitializer.maxColor, comp ? &comp->colorInitializer.maxColor : nullptr};
        ps["colorInitializer.useSRGB"] = {PropertyType::Bool, UpdateFlags_None, &def.colorInitializer.useSRGB, comp ? &comp->colorInitializer.useSRGB : nullptr};
        ps["colorModifier.fromTime"] = {PropertyType::Float, UpdateFlags_None, &def.colorModifier.fromTime, comp ? &comp->colorModifier.fromTime : nullptr};
        ps["colorModifier.toTime"] = {PropertyType::Float, UpdateFlags_None, &def.colorModifier.toTime, comp ? &comp->colorModifier.toTime : nullptr};
        ps["colorModifier.function"] = {PropertyType::Ease, UpdateFlags_None, &def.colorModifier.function, comp ? &comp->colorModifier.function : nullptr};
        ps["colorModifier.fromColor"] = {PropertyType::Vector3, UpdateFlags_None, &def.colorModifier.fromColor, comp ? &comp->colorModifier.fromColor : nullptr};
        ps["colorModifier.toColor"] = {PropertyType::Vector3, UpdateFlags_None, &def.colorModifier.toColor, comp ? &comp->colorModifier.toColor : nullptr};
        ps["colorModifier.useSRGB"] = {PropertyType::Bool, UpdateFlags_None, &def.colorModifier.useSRGB, comp ? &comp->colorModifier.useSRGB : nullptr};

        ps["colorGradient"] = {PropertyType::Custom, UpdateFlags_None, &def.colorGradient, comp ? &comp->colorGradient : nullptr};

        ps["alphaInitializer.minAlpha"] = {PropertyType::Float, UpdateFlags_None, &def.alphaInitializer.minAlpha, comp ? &comp->alphaInitializer.minAlpha : nullptr};
        ps["alphaInitializer.maxAlpha"] = {PropertyType::Float, UpdateFlags_None, &def.alphaInitializer.maxAlpha, comp ? &comp->alphaInitializer.maxAlpha : nullptr};
        ps["alphaModifier.fromTime"] = {PropertyType::Float, UpdateFlags_None, &def.alphaModifier.fromTime, comp ? &comp->alphaModifier.fromTime : nullptr};
        ps["alphaModifier.toTime"] = {PropertyType::Float, UpdateFlags_None, &def.alphaModifier.toTime, comp ? &comp->alphaModifier.toTime : nullptr};
        ps["alphaModifier.function"] = {PropertyType::Ease, UpdateFlags_None, &def.alphaModifier.function, comp ? &comp->alphaModifier.function : nullptr};
        ps["alphaModifier.fromAlpha"] = {PropertyType::Float, UpdateFlags_None, &def.alphaModifier.fromAlpha, comp ? &comp->alphaModifier.fromAlpha : nullptr};
        ps["alphaModifier.toAlpha"] = {PropertyType::Float, UpdateFlags_None, &def.alphaModifier.toAlpha, comp ? &comp->alphaModifier.toAlpha : nullptr};

        ps["sizeInitializer.minSize"] = {PropertyType::Float, UpdateFlags_None, &def.sizeInitializer.minSize, comp ? &comp->sizeInitializer.minSize : nullptr};
        ps["sizeInitializer.maxSize"] = {PropertyType::Float, UpdateFlags_None, &def.sizeInitializer.maxSize, comp ? &comp->sizeInitializer.maxSize : nullptr};
        ps["sizeModifier.fromTime"] = {PropertyType::Float, UpdateFlags_None, &def.sizeModifier.fromTime, comp ? &comp->sizeModifier.fromTime : nullptr};
        ps["sizeModifier.toTime"] = {PropertyType::Float, UpdateFlags_None, &def.sizeModifier.toTime, comp ? &comp->sizeModifier.toTime : nullptr};
        ps["sizeModifier.function"] = {PropertyType::Ease, UpdateFlags_None, &def.sizeModifier.function, comp ? &comp->sizeModifier.function : nullptr};
        ps["sizeModifier.fromSize"] = {PropertyType::Float, UpdateFlags_None, &def.sizeModifier.fromSize, comp ? &comp->sizeModifier.fromSize : nullptr};
        ps["sizeModifier.toSize"] = {PropertyType::Float, UpdateFlags_None, &def.sizeModifier.toSize, comp ? &comp->sizeModifier.toSize : nullptr};

        ps["spriteInitializer.frames"] = {PropertyType::Custom, UpdateFlags_None, &def.spriteInitializer.frames, comp ? &comp->spriteInitializer.frames : nullptr};
        ps["spriteModifier.fromTime"] = {PropertyType::Float, UpdateFlags_None, &def.spriteModifier.fromTime, comp ? &comp->spriteModifier.fromTime : nullptr};
        ps["spriteModifier.toTime"] = {PropertyType::Float, UpdateFlags_None, &def.spriteModifier.toTime, comp ? &comp->spriteModifier.toTime : nullptr};
        ps["spriteModifier.function"] = {PropertyType::Ease, UpdateFlags_None, &def.spriteModifier.function, comp ? &comp->spriteModifier.function : nullptr};
        ps["spriteModifier.frames"] = {PropertyType::Custom, UpdateFlags_None, &def.spriteModifier.frames, comp ? &comp->spriteModifier.frames : nullptr};

        ps["rotationInitializer.minRotation"] = {PropertyType::Quat, UpdateFlags_None, &def.rotationInitializer.minRotation, comp ? &comp->rotationInitializer.minRotation : nullptr};
        ps["rotationInitializer.maxRotation"] = {PropertyType::Quat, UpdateFlags_None, &def.rotationInitializer.maxRotation, comp ? &comp->rotationInitializer.maxRotation : nullptr};
        ps["rotationInitializer.shortestPath"] = {PropertyType::Bool, UpdateFlags_None, &def.rotationInitializer.shortestPath, comp ? &comp->rotationInitializer.shortestPath : nullptr};
        ps["rotationModifier.fromTime"] = {PropertyType::Float, UpdateFlags_None, &def.rotationModifier.fromTime, comp ? &comp->rotationModifier.fromTime : nullptr};
        ps["rotationModifier.toTime"] = {PropertyType::Float, UpdateFlags_None, &def.rotationModifier.toTime, comp ? &comp->rotationModifier.toTime : nullptr};
        ps["rotationModifier.function"] = {PropertyType::Ease, UpdateFlags_None, &def.rotationModifier.function, comp ? &comp->rotationModifier.function : nullptr};
        ps["rotationModifier.fromRotation"] = {PropertyType::Quat, UpdateFlags_None, &def.rotationModifier.fromRotation, comp ? &comp->rotationModifier.fromRotation : nullptr};
        ps["rotationModifier.toRotation"] = {PropertyType::Quat, UpdateFlags_None, &def.rotationModifier.toRotation, comp ? &comp->rotationModifier.toRotation : nullptr};
        ps["rotationModifier.shortestPath"] = {PropertyType::Bool, UpdateFlags_None, &def.rotationModifier.shortestPath, comp ? &comp->rotationModifier.shortestPath : nullptr};

        ps["scaleInitializer.minScale"] = {PropertyType::Vector3, UpdateFlags_None, &def.scaleInitializer.minScale, comp ? &comp->scaleInitializer.minScale : nullptr};
        ps["scaleInitializer.maxScale"] = {PropertyType::Vector3, UpdateFlags_None, &def.scaleInitializer.maxScale, comp ? &comp->scaleInitializer.maxScale : nullptr};
        ps["scaleInitializer.linearSort"] = {PropertyType::Bool, UpdateFlags_None, &def.scaleInitializer.linearSort, comp ? &comp->scaleInitializer.linearSort : nullptr};
        ps["scaleModifier.fromTime"] = {PropertyType::Float, UpdateFlags_None, &def.scaleModifier.fromTime, comp ? &comp->scaleModifier.fromTime : nullptr};
        ps["scaleModifier.toTime"] = {PropertyType::Float, UpdateFlags_None, &def.scaleModifier.toTime, comp ? &comp->scaleModifier.toTime : nullptr};
        ps["scaleModifier.function"] = {PropertyType::Ease, UpdateFlags_None, &def.scaleModifier.function, comp ? &comp->scaleModifier.function : nullptr};
        ps["scaleModifier.fromScale"] = {PropertyType::Vector3, UpdateFlags_None, &def.scaleModifier.fromScale, comp ? &comp->scaleModifier.fromScale : nullptr};
        ps["scaleModifier.toScale"] = {PropertyType::Vector3, UpdateFlags_None, &def.scaleModifier.toScale, comp ? &comp->scaleModifier.toScale : nullptr};
    }

    PropertyData getPointsPropertyFast(PointsComponent* comp, const std::string& propertyName) {
        PointsComponent& def = getDefaultComponent<PointsComponent>();
        if (!comp) return PropertyData();

        if (propertyName == "maxPoints") return {PropertyType::UInt, UpdateFlags_None, &def.maxPoints, &comp->maxPoints};
        if (propertyName == "transparent") return {PropertyType::Bool, UpdateFlags_None, &def.transparent, &comp->transparent};
        if (propertyName == "autoTransparency") return {PropertyType::Bool, UpdateFlags_None, &def.autoTransparency, &comp->autoTransparency};
        if (propertyName == "texture") return {PropertyType::Texture, UpdateFlags_Points_Texture, &def.texture, &comp->texture};
        if (propertyName == "customShader") return {PropertyType::Shader, UpdateFlags_Shader_Reload, &def.customShader, &comp->customShader};
        if (propertyName == "shaderUniforms") return {PropertyType::Custom, UpdateFlags_Shader_Uniforms, &def.shaderUniforms, &comp->shaderUniforms};
        if (propertyName == "shaderTextures") return {PropertyType::Custom, UpdateFlags_Shader_Reload, &def.shaderTextures, &comp->shaderTextures};
        if (propertyName == "shaderFeatures") return {PropertyType::Custom, UpdateFlags_Shader_Reload, &def.shaderFeatures, &comp->shaderFeatures};
        if (propertyName == "blendMode") return {PropertyType::Enum, UpdateFlags_Shader_Reload, &def.blendMode, &comp->blendMode};
        if (propertyName == "depthWrite") return {PropertyType::Enum, UpdateFlags_Shader_Reload, &def.depthWrite, &comp->depthWrite};
        if (propertyName == "colorWrite" || propertyName.compare(0, 7, "stencil") == 0) {
            PropertyData stencil = getStencilColorWriteProperty(&comp->stencil, &def.stencil, &comp->colorWrite, &def.colorWrite,
                                                                propertyName, UpdateFlags_Shader_Reload);
            if (stencil.ref) return stencil;
        }

        if (propertyName == "numFramesRect") {
            return {PropertyType::UInt, UpdateFlags_None, (void*)&def.numFramesRect, (void*)&comp->numFramesRect};
        }

        if (propertyName.compare(0, 11, "framesRect[") == 0) {
            size_t pos = 11;
            size_t frameIndex = 0;
            if (!parseIndex(propertyName, pos, frameIndex) || pos >= propertyName.size() || propertyName[pos] != ']') {
                return PropertyData();
            }
            SpriteFrameData& frame = comp->framesRect[frameIndex];
            SpriteFrameData& defFrame = def.framesRect[0];
            pos++;
            if (pos == propertyName.size()) {
                return {PropertyType::Custom, UpdateFlags_None, (void*)&defFrame, (void*)&frame};
            }
            if (propertyName[pos] != '.') return PropertyData();
            const size_t fieldPos = pos + 1;
            if (propertyName.compare(fieldPos, 4, "name") == 0 && fieldPos + 4 == propertyName.size()) {
                return {PropertyType::String, UpdateFlags_None, (void*)&defFrame.name, (void*)&frame.name};
            }
            if (propertyName.compare(fieldPos, 4, "rect") == 0 && fieldPos + 4 == propertyName.size()) {
                return {PropertyType::Vector4, UpdateFlags_None, (void*)&defFrame.rect, (void*)&frame.rect};
            }
            return PropertyData();
        }

        if (propertyName == "points") {
            static std::vector<PointData> defPoints;
            return {PropertyType::Custom, UpdateFlags_Points, (void*)&defPoints, (void*)&comp->points};
        }

        if (propertyName.compare(0, 7, "points[") == 0) {
            size_t pos = 7;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') return PropertyData();
            if (index >= comp->points.size()) return PropertyData();
            if (pos + 1 >= propertyName.size()) return PropertyData();
            std::string fieldName = propertyName.substr(pos + 1);
            if (fieldName == ".position") {
                static Vector3 defPos = Vector3(0.0f, 0.0f, 0.0f);
                return {PropertyType::Vector3, UpdateFlags_Points, (void*)&defPos, (void*)&comp->points[index].position};
            }
            if (fieldName == ".color") {
                static Vector4 defColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                return {PropertyType::Vector4, UpdateFlags_Points, (void*)&defColor, (void*)&comp->points[index].color};
            }
            if (fieldName == ".size") {
                static float defSize = 1.0f;
                return {PropertyType::Float, UpdateFlags_Points, (void*)&defSize, (void*)&comp->points[index].size};
            }
            if (fieldName == ".rotation") {
                static float defRotation = 0.0f;
                return {PropertyType::Float, UpdateFlags_Points, (void*)&defRotation, (void*)&comp->points[index].rotation};
            }
            if (fieldName == ".textureRect") {
                static Rect defRect = Rect(0.0f, 0.0f, 1.0f, 1.0f);
                return {PropertyType::Vector4, UpdateFlags_Points, (void*)&defRect, (void*)&comp->points[index].textureRect};
            }
            if (fieldName == ".visible") {
                static bool defVisible = true;
                return {PropertyType::Bool, UpdateFlags_Points, (void*)&defVisible, (void*)&comp->points[index].visible};
            }
        }

        return PropertyData();
    }

    PropertyData resolvePointsPropertyFast(void* comp, const std::string& propertyName) {
        return getPointsPropertyFast(static_cast<PointsComponent*>(comp), propertyName);
    }

    void enumeratePointsProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        PointsComponent* comp = static_cast<PointsComponent*>(compRef);
        PointsComponent& def = getDefaultComponent<PointsComponent>();

        ps["maxPoints"] = {PropertyType::UInt, UpdateFlags_None, &def.maxPoints, comp ? &comp->maxPoints : nullptr};
        ps["transparent"] = {PropertyType::Bool, UpdateFlags_None, &def.transparent, comp ? &comp->transparent : nullptr};
        ps["autoTransparency"] = {PropertyType::Bool, UpdateFlags_None, &def.autoTransparency, comp ? &comp->autoTransparency : nullptr};
        ps["texture"] = {PropertyType::Texture, UpdateFlags_Points_Texture, &def.texture, comp ? &comp->texture : nullptr};
        ps["customShader"] = {PropertyType::Shader, UpdateFlags_Shader_Reload, &def.customShader, comp ? &comp->customShader : nullptr};
        ps["shaderUniforms"] = {PropertyType::Custom, UpdateFlags_Shader_Uniforms, &def.shaderUniforms, comp ? &comp->shaderUniforms : nullptr};
        ps["shaderTextures"] = {PropertyType::Custom, UpdateFlags_Shader_Reload, &def.shaderTextures, comp ? &comp->shaderTextures : nullptr};
        ps["shaderFeatures"] = {PropertyType::Custom, UpdateFlags_Shader_Reload, &def.shaderFeatures, comp ? &comp->shaderFeatures : nullptr};
        ps["blendMode"] = {PropertyType::Enum, UpdateFlags_Shader_Reload, &def.blendMode, comp ? &comp->blendMode : nullptr};
        ps["depthWrite"] = {PropertyType::Enum, UpdateFlags_Shader_Reload, &def.depthWrite, comp ? &comp->depthWrite : nullptr};
        for (const char* field : kComponentStencilFields) {
            ps[field] = getStencilColorWriteProperty(comp ? &comp->stencil : nullptr, &def.stencil, comp ? &comp->colorWrite : nullptr,
                                                     &def.colorWrite, field, UpdateFlags_Shader_Reload);
        }

        static std::vector<PointData> defPoints;
        ps["points"] = {PropertyType::Custom, UpdateFlags_Points, (void*)&defPoints, comp ? (void*)&comp->points : nullptr};

        if (comp) {
            ps["numFramesRect"] = {PropertyType::UInt, UpdateFlags_None, (void*)&def.numFramesRect, (void*)&comp->numFramesRect};
            for (unsigned int i = 0; i < comp->numFramesRect; i++) {
                std::string idx = std::to_string(i);
                ps["framesRect[" + idx + "].name"] = {PropertyType::String, UpdateFlags_None, (void*)&def.framesRect[0].name, (void*)&comp->framesRect[i].name};
                ps["framesRect[" + idx + "].rect"] = {PropertyType::Vector4, UpdateFlags_None, (void*)&def.framesRect[0].rect, (void*)&comp->framesRect[i].rect};
            }
            static Vector3 defPos = Vector3(0.0f, 0.0f, 0.0f);
            static Vector4 defColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
            static float defSize = 1.0f;
            static float defRotation = 0.0f;
            static Rect defRect = Rect(0.0f, 0.0f, 1.0f, 1.0f);
            static bool defVisible = true;
            for (size_t i = 0; i < comp->points.size(); i++) {
                std::string idx = std::to_string(i);
                ps["points[" + idx + "].position"] = {PropertyType::Vector3, UpdateFlags_Points, (void*)&defPos, (void*)&comp->points[i].position};
                ps["points[" + idx + "].color"] = {PropertyType::Vector4, UpdateFlags_Points, (void*)&defColor, (void*)&comp->points[i].color};
                ps["points[" + idx + "].size"] = {PropertyType::Float, UpdateFlags_Points, (void*)&defSize, (void*)&comp->points[i].size};
                ps["points[" + idx + "].rotation"] = {PropertyType::Float, UpdateFlags_Points, (void*)&defRotation, (void*)&comp->points[i].rotation};
                ps["points[" + idx + "].textureRect"] = {PropertyType::Vector4, UpdateFlags_Points, (void*)&defRect, (void*)&comp->points[i].textureRect};
                ps["points[" + idx + "].visible"] = {PropertyType::Bool, UpdateFlags_Points, (void*)&defVisible, (void*)&comp->points[i].visible};
            }
        }
    }

    PropertyData getLinesPropertyFast(LineRendererComponent* comp, const std::string& propertyName) {
        LineRendererComponent& def = getDefaultComponent<LineRendererComponent>();
        if (!comp) return PropertyData();

        if (propertyName == "maxLines") return {PropertyType::UInt, UpdateFlags_Lines_Reload, &def.maxLines, &comp->maxLines};
        if (propertyName == "customShader") return {PropertyType::Shader, UpdateFlags_Shader_Reload, &def.customShader, &comp->customShader};
        if (propertyName == "shaderUniforms") return {PropertyType::Custom, UpdateFlags_Shader_Uniforms, &def.shaderUniforms, &comp->shaderUniforms};
        if (propertyName == "shaderTextures") return {PropertyType::Custom, UpdateFlags_Shader_Reload, &def.shaderTextures, &comp->shaderTextures};
        if (propertyName == "shaderFeatures") return {PropertyType::Custom, UpdateFlags_Shader_Reload, &def.shaderFeatures, &comp->shaderFeatures};
        if (propertyName == "blendMode") return {PropertyType::Enum, UpdateFlags_Shader_Reload, &def.blendMode, &comp->blendMode};
        if (propertyName == "depthWrite") return {PropertyType::Enum, UpdateFlags_Shader_Reload, &def.depthWrite, &comp->depthWrite};
        if (propertyName == "colorWrite" || propertyName.compare(0, 7, "stencil") == 0) {
            PropertyData stencil = getStencilColorWriteProperty(&comp->stencil, &def.stencil, &comp->colorWrite, &def.colorWrite,
                                                                propertyName, UpdateFlags_Shader_Reload);
            if (stencil.ref) return stencil;
        }

        if (propertyName == "lines") {
            static std::vector<LineData> defLines;
            return {PropertyType::Custom, UpdateFlags_Lines, (void*)&defLines, (void*)&comp->lines};
        }

        if (propertyName.compare(0, 6, "lines[") == 0) {
            size_t pos = 6;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') return PropertyData();
            if (index >= comp->lines.size()) return PropertyData();
            if (pos + 1 >= propertyName.size()) return PropertyData();

            std::string fieldName = propertyName.substr(pos + 1);
            if (fieldName == ".pointA") {
                static Vector3 defPoint = Vector3(0.0f, 0.0f, 0.0f);
                return {PropertyType::Vector3, UpdateFlags_Lines, (void*)&defPoint, (void*)&comp->lines[index].pointA};
            }
            if (fieldName == ".colorA") {
                static Vector4 defColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                return {PropertyType::Vector4, UpdateFlags_Lines, (void*)&defColor, (void*)&comp->lines[index].colorA};
            }
            if (fieldName == ".pointB") {
                static Vector3 defPoint = Vector3(0.0f, 0.0f, 0.0f);
                return {PropertyType::Vector3, UpdateFlags_Lines, (void*)&defPoint, (void*)&comp->lines[index].pointB};
            }
            if (fieldName == ".colorB") {
                static Vector4 defColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                return {PropertyType::Vector4, UpdateFlags_Lines, (void*)&defColor, (void*)&comp->lines[index].colorB};
            }
        }

        return PropertyData();
    }

    PropertyData resolveLinesPropertyFast(void* comp, const std::string& propertyName) {
        return getLinesPropertyFast(static_cast<LineRendererComponent*>(comp), propertyName);
    }

    void enumerateLinesProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        LineRendererComponent* comp = static_cast<LineRendererComponent*>(compRef);
        LineRendererComponent& def = getDefaultComponent<LineRendererComponent>();

        ps["maxLines"] = {PropertyType::UInt, UpdateFlags_Lines_Reload, &def.maxLines, comp ? &comp->maxLines : nullptr};
        ps["customShader"] = {PropertyType::Shader, UpdateFlags_Shader_Reload, &def.customShader, comp ? &comp->customShader : nullptr};
        ps["shaderUniforms"] = {PropertyType::Custom, UpdateFlags_Shader_Uniforms, &def.shaderUniforms, comp ? &comp->shaderUniforms : nullptr};
        ps["shaderTextures"] = {PropertyType::Custom, UpdateFlags_Shader_Reload, &def.shaderTextures, comp ? &comp->shaderTextures : nullptr};
        ps["shaderFeatures"] = {PropertyType::Custom, UpdateFlags_Shader_Reload, &def.shaderFeatures, comp ? &comp->shaderFeatures : nullptr};
        ps["blendMode"] = {PropertyType::Enum, UpdateFlags_Shader_Reload, &def.blendMode, comp ? &comp->blendMode : nullptr};
        ps["depthWrite"] = {PropertyType::Enum, UpdateFlags_Shader_Reload, &def.depthWrite, comp ? &comp->depthWrite : nullptr};
        for (const char* field : kComponentStencilFields) {
            ps[field] = getStencilColorWriteProperty(comp ? &comp->stencil : nullptr, &def.stencil, comp ? &comp->colorWrite : nullptr,
                                                     &def.colorWrite, field, UpdateFlags_Shader_Reload);
        }

        static std::vector<LineData> defLines;
        ps["lines"] = {PropertyType::Custom, UpdateFlags_Lines, (void*)&defLines, comp ? (void*)&comp->lines : nullptr};

        if (comp) {
            static Vector3 defPoint = Vector3(0.0f, 0.0f, 0.0f);
            static Vector4 defColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
            for (size_t i = 0; i < comp->lines.size(); i++) {
                std::string idx = std::to_string(i);
                ps["lines[" + idx + "].pointA"] = {PropertyType::Vector3, UpdateFlags_Lines, (void*)&defPoint, (void*)&comp->lines[i].pointA};
                ps["lines[" + idx + "].colorA"] = {PropertyType::Vector4, UpdateFlags_Lines, (void*)&defColor, (void*)&comp->lines[i].colorA};
                ps["lines[" + idx + "].pointB"] = {PropertyType::Vector3, UpdateFlags_Lines, (void*)&defPoint, (void*)&comp->lines[i].pointB};
                ps["lines[" + idx + "].colorB"] = {PropertyType::Vector4, UpdateFlags_Lines, (void*)&defColor, (void*)&comp->lines[i].colorB};
            }
        }
    }

    PropertyData getPolygonPropertyFast(PolygonComponent* comp, const std::string& propertyName) {
        if (!comp) return PropertyData();

        if (propertyName == "points") {
            static std::vector<PolygonPoint> defPoints;
            return {PropertyType::Custom, UpdateFlags_UI_Reload, (void*)&defPoints, (void*)&comp->points};
        }

        if (propertyName.compare(0, 7, "points[") == 0) {
            size_t pos = 7;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') return PropertyData();
            if (index >= comp->points.size()) return PropertyData();
            if (pos + 1 >= propertyName.size()) return PropertyData();

            std::string fieldName = propertyName.substr(pos + 1);
            if (fieldName == ".position") {
                static Vector3 defPosition = Vector3(0.0f, 0.0f, 0.0f);
                return {PropertyType::Vector3, UpdateFlags_UI_Reload, (void*)&defPosition, (void*)&comp->points[index].position};
            }
            if (fieldName == ".color") {
                static Vector4 defColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                return {PropertyType::Vector4, UpdateFlags_UI_Reload, (void*)&defColor, (void*)&comp->points[index].color};
            }
        }

        return PropertyData();
    }

    PropertyData resolvePolygonPropertyFast(void* comp, const std::string& propertyName) {
        return getPolygonPropertyFast(static_cast<PolygonComponent*>(comp), propertyName);
    }

    void enumeratePolygonProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        PolygonComponent* comp = static_cast<PolygonComponent*>(compRef);

        static std::vector<PolygonPoint> defPoints;
        ps["points"] = {PropertyType::Custom, UpdateFlags_UI_Reload, (void*)&defPoints, comp ? (void*)&comp->points : nullptr};

        if (comp) {
            static Vector3 defPosition = Vector3(0.0f, 0.0f, 0.0f);
            static Vector4 defColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
            for (size_t i = 0; i < comp->points.size(); i++) {
                std::string idx = std::to_string(i);
                ps["points[" + idx + "].position"] = {PropertyType::Vector3, UpdateFlags_UI_Reload, (void*)&defPosition, (void*)&comp->points[i].position};
                ps["points[" + idx + "].color"] = {PropertyType::Vector4, UpdateFlags_UI_Reload, (void*)&defColor, (void*)&comp->points[i].color};
            }
        }
    }

    PropertyData getMeshPolygonPropertyFast(MeshPolygonComponent* comp, const std::string& propertyName) {
        if (!comp) return PropertyData();

        PropertyData result = resolveDirectProperties(comp, propertyName, kMeshPolygonTopProperties);
        if (result.ref) return result;

        if (propertyName == "points") {
            static std::vector<PolygonPoint> defPoints;
            return {PropertyType::Custom, UpdateFlags_Mesh_Reload, (void*)&defPoints, (void*)&comp->points};
        }

        if (propertyName.compare(0, 7, "points[") == 0) {
            size_t pos = 7;
            size_t index = 0;
            if (!parseIndex(propertyName, pos, index) || pos >= propertyName.size() || propertyName[pos] != ']') return PropertyData();
            if (index >= comp->points.size()) return PropertyData();
            if (pos + 1 >= propertyName.size()) return PropertyData();

            std::string fieldName = propertyName.substr(pos + 1);
            if (fieldName == ".position") {
                static Vector3 defPosition = Vector3(0.0f, 0.0f, 0.0f);
                return {PropertyType::Vector3, UpdateFlags_Mesh_Reload, (void*)&defPosition, (void*)&comp->points[index].position};
            }
            if (fieldName == ".color") {
                static Vector4 defColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                return {PropertyType::Vector4, UpdateFlags_Mesh_Reload, (void*)&defColor, (void*)&comp->points[index].color};
            }
        }

        return PropertyData();
    }

    PropertyData resolveMeshPolygonPropertyFast(void* comp, const std::string& propertyName) {
        return getMeshPolygonPropertyFast(static_cast<MeshPolygonComponent*>(comp), propertyName);
    }

    void enumerateMeshPolygonProperties(void* compRef, std::map<std::string, PropertyData>& ps) {
        MeshPolygonComponent* comp = static_cast<MeshPolygonComponent*>(compRef);

        enumerateFromDescriptors(compRef, ps, kMeshPolygonTopProperties);

        static std::vector<PolygonPoint> defPoints;
        ps["points"] = {PropertyType::Custom, UpdateFlags_Mesh_Reload, (void*)&defPoints, comp ? (void*)&comp->points : nullptr};

        if (comp) {
            static Vector3 defPosition = Vector3(0.0f, 0.0f, 0.0f);
            static Vector4 defColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
            for (size_t i = 0; i < comp->points.size(); i++) {
                std::string idx = std::to_string(i);
                ps["points[" + idx + "].position"] = {PropertyType::Vector3, UpdateFlags_Mesh_Reload, (void*)&defPosition, (void*)&comp->points[i].position};
                ps["points[" + idx + "].color"] = {PropertyType::Vector4, UpdateFlags_Mesh_Reload, (void*)&defColor, (void*)&comp->points[i].color};
            }
        }
    }

    // ── Resolver dispatch table ──

    static const FastComponentResolver kFastComponentResolvers[] = {
        {ComponentType::Transform, &findComponentPtr<Transform>, &resolveTransformPropertyFast, &enumerateTransformProperties},
        {ComponentType::MeshComponent, &findComponentPtr<MeshComponent>, &resolveMeshPropertyFast, &enumerateMeshProperties},
        {ComponentType::UIComponent, &findComponentPtr<UIComponent>, &resolveUIPropertyFast, &enumerateUIProperties},
        {ComponentType::RectTransformComponent, &findComponentPtr<RectTransformComponent>, &resolveRectTransformPropertyFast, &enumerateRectTransformProperties},
        {ComponentType::LayoutGroupComponent, &findComponentPtr<LayoutGroupComponent>, &resolveLayoutGroupPropertyFast, &enumerateLayoutGroupProperties},
        {ComponentType::ImageComponent, &findComponentPtr<ImageComponent>, &resolveImagePropertyFast, &enumerateImageProperties},
        {ComponentType::ButtonComponent, &findComponentPtr<ButtonComponent>, &resolveButtonPropertyFast, &enumerateButtonProperties},
        {ComponentType::ScrollbarComponent, &findComponentPtr<ScrollbarComponent>, &resolveScrollbarPropertyFast, &enumerateScrollbarProperties},
        {ComponentType::ProgressbarComponent, &findComponentPtr<ProgressbarComponent>, &resolveProgressbarPropertyFast, &enumerateProgressbarProperties},
        {ComponentType::InputFieldComponent, &findComponentPtr<InputFieldComponent>, &resolveInputFieldPropertyFast, &enumerateInputFieldProperties},
        {ComponentType::PanelComponent, &findComponentPtr<PanelComponent>, &resolvePanelPropertyFast, &enumeratePanelProperties},
        {ComponentType::PolygonComponent, &findComponentPtr<PolygonComponent>, &resolvePolygonPropertyFast, &enumeratePolygonProperties},
        {ComponentType::MeshPolygonComponent, &findComponentPtr<MeshPolygonComponent>, &resolveMeshPolygonPropertyFast, &enumerateMeshPolygonProperties},
        {ComponentType::SpriteComponent, &findComponentPtr<SpriteComponent>, &resolveSpritePropertyFast, &enumerateSpriteProperties},
        {ComponentType::TilemapComponent, &findComponentPtr<TilemapComponent>, &resolveTilemapPropertyFast, &enumerateTilemapProperties},
        {ComponentType::TerrainComponent, &findComponentPtr<TerrainComponent>, &resolveTerrainPropertyFast, &enumerateTerrainProperties},
        {ComponentType::LightComponent, &findComponentPtr<LightComponent>, &resolveLightPropertyFast, &enumerateLightProperties},
        {ComponentType::FogComponent, &findComponentPtr<FogComponent>, &resolveFogPropertyFast, &enumerateFogProperties},
        {ComponentType::LocalVolumetricFogComponent, &findComponentPtr<LocalVolumetricFogComponent>, &resolveLocalVolumetricFogPropertyFast, &enumerateLocalVolumetricFogProperties},
        {ComponentType::VolumetricCloudsComponent, &findComponentPtr<VolumetricCloudsComponent>, &resolveVolumetricCloudsPropertyFast, &enumerateVolumetricCloudsProperties},
        {ComponentType::MirrorComponent, &findComponentPtr<MirrorComponent>, &resolveMirrorPropertyFast, &enumerateMirrorProperties},
        {ComponentType::ReflectionProbeComponent, &findComponentPtr<ReflectionProbeComponent>, &resolveReflectionProbePropertyFast, &enumerateReflectionProbeProperties},
        {ComponentType::UIDocumentComponent, &findComponentPtr<UIDocumentComponent>, &resolveUIDocumentPropertyFast, &enumerateUIDocumentProperties},
        {ComponentType::FluidSimulationComponent, &findComponentPtr<FluidSimulationComponent>, &resolveFluidSimulationPropertyFast, &enumerateFluidSimulationProperties},
        {ComponentType::PoseAnimatorComponent, &findComponentPtr<PoseAnimatorComponent>, &resolvePoseAnimatorPropertyFast, &enumeratePoseAnimatorProperties},
        {ComponentType::AnimationPlayerComponent, &findComponentPtr<AnimationPlayerComponent>, &resolveAnimationPlayerPropertyFast, &enumerateAnimationPlayerProperties},
        {ComponentType::Light2DComponent, &findComponentPtr<Light2DComponent>, &resolveLight2DPropertyFast, &enumerateLight2DProperties},
        {ComponentType::ShadowCaster2DComponent, &findComponentPtr<ShadowCaster2DComponent>, &resolveShadowCaster2DPropertyFast, &enumerateShadowCaster2DProperties},
        {ComponentType::CameraComponent, &findComponentPtr<CameraComponent>, &resolveCameraPropertyFast, &enumerateCameraProperties},
        {ComponentType::AudioSourceComponent, &findComponentPtr<AudioSourceComponent>, &resolveAudioPropertyFast, &enumerateAudioProperties},
        {ComponentType::SkyboxComponent, &findComponentPtr<SkyboxComponent>, &resolveSkyPropertyFast, &enumerateSkyProperties},
        {ComponentType::TextComponent, &findComponentPtr<TextComponent>, &resolveTextPropertyFast, &enumerateTextProperties},
        {ComponentType::ScriptComponent, &findComponentPtr<ScriptComponent>, &resolveScriptPropertyFast, &enumerateScriptProperties},
        {ComponentType::Joint2DComponent, &findComponentPtr<Joint2DComponent>, &resolveJoint2DPropertyFast, &enumerateJoint2DProperties},
        {ComponentType::Joint3DComponent, &findComponentPtr<Joint3DComponent>, &resolveJoint3DPropertyFast, &enumerateJoint3DProperties},
        {ComponentType::Rigidbody2DComponent, &findComponentPtr<Rigidbody2DComponent>, &resolveRigidbody2DPropertyFast, &enumerateRigidbody2DProperties},
        {ComponentType::RigidbodyComponent, &findComponentPtr<RigidbodyComponent>, &resolveRigidbodyPropertyFast, &enumerateRigidbodyProperties},
        {ComponentType::InstancedMeshComponent, &findComponentPtr<InstancedMeshComponent>, &resolveInstancedMeshPropertyFast, &enumerateInstancedMeshProperties},
        {ComponentType::ParticleSystemComponent, &findComponentPtr<ParticleSystemComponent>, &resolveParticlesPropertyFast, &enumerateParticlesProperties},
        {ComponentType::PointsComponent, &findComponentPtr<PointsComponent>, &resolvePointsPropertyFast, &enumeratePointsProperties},
        {ComponentType::LineRendererComponent, &findComponentPtr<LineRendererComponent>, &resolveLinesPropertyFast, &enumerateLinesProperties},
        {ComponentType::ActionComponent, &findComponentPtr<ActionComponent>, &resolveActionPropertyFast, &enumerateActionProperties},
        {ComponentType::TweenComponent, &findComponentPtr<TweenComponent>, &resolveTweenPropertyFast, &enumerateTweenProperties},
        {ComponentType::PositionTweenComponent, &findComponentPtr<PositionTweenComponent>, &resolvePositionTweenPropertyFast, &enumeratePositionTweenProperties},
        {ComponentType::RotationTweenComponent, &findComponentPtr<RotationTweenComponent>, &resolveRotationTweenPropertyFast, &enumerateRotationTweenProperties},
        {ComponentType::ScaleTweenComponent, &findComponentPtr<ScaleTweenComponent>, &resolveScaleTweenPropertyFast, &enumerateScaleTweenProperties},
        {ComponentType::ColorTweenComponent, &findComponentPtr<ColorTweenComponent>, &resolveColorTweenPropertyFast, &enumerateColorTweenProperties},
        {ComponentType::AlphaTweenComponent, &findComponentPtr<AlphaTweenComponent>, &resolveAlphaTweenPropertyFast, &enumerateAlphaTweenProperties},
        {ComponentType::SpriteAnimationComponent, &findComponentPtr<SpriteAnimationComponent>, &resolveSpriteAnimationPropertyFast, &enumerateSpriteAnimationProperties},
        {ComponentType::AnimationComponent, &findComponentPtr<AnimationComponent>, &resolveAnimationPropertyFast, &enumerateAnimationProperties},
        {ComponentType::ModelComponent, &findComponentPtr<ModelComponent>, &resolveModelPropertyFast, &enumerateModelProperties},
        {ComponentType::PrefabComponent, &findComponentPtr<PrefabComponent>, &resolvePrefabPropertyFast, &enumeratePrefabProperties},
        {ComponentType::BoneComponent, &findComponentPtr<BoneComponent>, &resolveBonePropertyFast, &enumerateBoneProperties},
        {ComponentType::KeyframeTracksComponent, &findComponentPtr<KeyframeTracksComponent>, &resolveKeyframeTracksPropertyFast, &enumerateKeyframeTracksProperties},
        {ComponentType::TranslateTracksComponent, &findComponentPtr<TranslateTracksComponent>, &resolveTranslateTracksPropertyFast, &enumerateTranslateTracksProperties},
        {ComponentType::RotateTracksComponent, &findComponentPtr<RotateTracksComponent>, &resolveRotateTracksPropertyFast, &enumerateRotateTracksProperties},
        {ComponentType::ScaleTracksComponent, &findComponentPtr<ScaleTracksComponent>, &resolveScaleTracksPropertyFast, &enumerateScaleTracksProperties},
        {ComponentType::MorphTracksComponent, &findComponentPtr<MorphTracksComponent>, &resolveMorphTracksPropertyFast, &enumerateMorphTracksProperties},
        {ComponentType::LatticeComponent, &findComponentPtr<LatticeComponent>, &resolveLatticePropertyFast, &enumerateLatticeProperties},
        {ComponentType::LatticeModifierComponent, &findComponentPtr<LatticeModifierComponent>, &resolveLatticeModifierPropertyFast, &enumerateLatticeModifierProperties},
        {ComponentType::LatticeFollowerComponent, &findComponentPtr<LatticeFollowerComponent>, &resolveLatticeFollowerPropertyFast, &enumerateLatticeFollowerProperties},
        {ComponentType::LatticeTracksComponent, &findComponentPtr<LatticeTracksComponent>, &resolveLatticeTracksPropertyFast, &enumerateLatticeTracksProperties},
    };

    // The Unity component checkbox: "enabled" of every component that has one, shared
    // here instead of repeated in each table (ShadowCaster2D lists its own)
    using EnabledRefGetter = bool* (*)(void*);

    template<typename Component>
    bool* getEnabledRef(void* comp) {
        return &static_cast<Component*>(comp)->enabled;
    }

    EnabledRefGetter findEnabledGetter(ComponentType component) {
        switch (component) {
            case ComponentType::MeshComponent: return &getEnabledRef<MeshComponent>;
            case ComponentType::ModelComponent: return &getEnabledRef<ModelComponent>;
            case ComponentType::SpriteComponent: return &getEnabledRef<SpriteComponent>;
            case ComponentType::TilemapComponent: return &getEnabledRef<TilemapComponent>;
            case ComponentType::TerrainComponent: return &getEnabledRef<TerrainComponent>;
            case ComponentType::PointsComponent: return &getEnabledRef<PointsComponent>;
            case ComponentType::LineRendererComponent: return &getEnabledRef<LineRendererComponent>;
            case ComponentType::ParticleSystemComponent: return &getEnabledRef<ParticleSystemComponent>;
            case ComponentType::InstancedMeshComponent: return &getEnabledRef<InstancedMeshComponent>;
            case ComponentType::LightComponent: return &getEnabledRef<LightComponent>;
            case ComponentType::Light2DComponent: return &getEnabledRef<Light2DComponent>;
            case ComponentType::CameraComponent: return &getEnabledRef<CameraComponent>;
            case ComponentType::AudioSourceComponent: return &getEnabledRef<AudioSourceComponent>;
            case ComponentType::Rigidbody2DComponent: return &getEnabledRef<Rigidbody2DComponent>;
            case ComponentType::RigidbodyComponent: return &getEnabledRef<RigidbodyComponent>;
            case ComponentType::Joint2DComponent: return &getEnabledRef<Joint2DComponent>;
            case ComponentType::Joint3DComponent: return &getEnabledRef<Joint3DComponent>;
            case ComponentType::UIComponent: return &getEnabledRef<UIComponent>;
            case ComponentType::UIDocumentComponent: return &getEnabledRef<UIDocumentComponent>;
            case ComponentType::FluidSimulationComponent: return &getEnabledRef<FluidSimulationComponent>;
            case ComponentType::LatticeComponent: return &getEnabledRef<LatticeComponent>;
            case ComponentType::LatticeModifierComponent: return &getEnabledRef<LatticeModifierComponent>;
            case ComponentType::LatticeFollowerComponent: return &getEnabledRef<LatticeFollowerComponent>;
            case ComponentType::AnimationPlayerComponent: return &getEnabledRef<AnimationPlayerComponent>;
            default: return nullptr;
        }
    }

    PropertyData resolveEnabledProperty(ComponentType component, void* comp) {
        static bool defaultEnabled = true;
        EnabledRefGetter getter = findEnabledGetter(component);
        if (!getter) {
            return PropertyData();
        }
        return {PropertyType::Bool, UpdateFlags_None, &defaultEnabled, comp ? getter(comp) : nullptr};
    }

    PropertyData tryGetFastProperty(EntityRegistry* registry, Entity entity, ComponentType component, const std::string& propertyName) {
        for (const FastComponentResolver& resolver : kFastComponentResolvers) {
            if (resolver.component != component) {
                continue;
            }

            void* comp = resolver.findComponent(registry, entity);
            if (!comp) {
                return PropertyData();
            }

            if (propertyName == "enabled" && findEnabledGetter(component)) {
                return resolveEnabledProperty(component, comp);
            }

            return resolver.resolve(comp, propertyName);
        }

        return PropertyData();
    }

    bool tryEnumerateProperties(ComponentType component, void* compRef, std::map<std::string, PropertyData>& ps) {
        for (const FastComponentResolver& resolver : kFastComponentResolvers) {
            if (resolver.component == component) {
                resolver.enumerate(compRef, ps);
                if (findEnabledGetter(component)) {
                    ps["enabled"] = resolveEnabledProperty(component, compRef);
                }
                return true;
            }
        }
        return false;
    }

    bool tryEnumerateEntityProperties(EntityRegistry* registry, Entity entity, ComponentType component, std::map<std::string, PropertyData>& ps) {
        for (const FastComponentResolver& resolver : kFastComponentResolvers) {
            if (resolver.component == component) {
                void* comp = resolver.findComponent(registry, entity);
                if (!comp) return false;
                resolver.enumerate(comp, ps);
                if (findEnabledGetter(component)) {
                    ps["enabled"] = resolveEnabledProperty(component, comp);
                }
                return true;
            }
        }
        return false;
    }

}

using namespace jasperengine;

editor::Catalog::Catalog(){
}

std::string editor::Catalog::removeComponentSuffix(std::string str) {
    // Convert to lowercase first
    std::transform(str.begin(), str.end(), str.begin(), 
                   [](unsigned char c) { return std::tolower(c); });

    // Check if string ends with "component"
    const std::string suffix = "component";
    if (str.length() >= suffix.length() && 
        str.substr(str.length() - suffix.length()) == suffix) {
        str.erase(str.length() - suffix.length());
    }

    return str;
}

namespace {
    std::string computeComponentName(editor::ComponentType component);
}

std::string editor::Catalog::getComponentName(ComponentType component, bool removeSuffix){
    // Asked for every component type of every decoded entity: computed once per thread
    thread_local std::unordered_map<int, std::string> names;
    const int key = static_cast<int>(component) * 2 + (removeSuffix ? 1 : 0);
    auto it = names.find(key);
    if (it == names.end()) {
        const std::string name = computeComponentName(component);
        it = names.emplace(key, removeSuffix && !name.empty() ? removeComponentSuffix(name) : name).first;
    }
    return it->second;
}

namespace {
std::string computeComponentName(editor::ComponentType component){
    using namespace jasperengine::editor;
    std::string name;

    if(component == ComponentType::ActionComponent){
        name = "ActionComponent";
    }else if(component == ComponentType::AlphaTweenComponent){
        name = "AlphaTweenComponent";
    }else if(component == ComponentType::AnimationComponent){
        name = "AnimationComponent";
    }else if(component == ComponentType::AudioSourceComponent){
        name = "AudioSourceComponent";
    }else if(component == ComponentType::Rigidbody2DComponent){
        name = "Rigidbody2DComponent";
    }else if(component == ComponentType::RigidbodyComponent){
        name = "RigidbodyComponent";
    }else if(component == ComponentType::BoneComponent){
        name = "BoneComponent";
    }else if(component == ComponentType::PrefabComponent){
        name = "PrefabComponent";
    }else if(component == ComponentType::ButtonComponent){
        name = "ButtonComponent";
    }else if(component == ComponentType::CameraComponent){
        name = "CameraComponent";
    }else if(component == ComponentType::ColorTweenComponent){
        name = "ColorTweenComponent";
    }else if(component == ComponentType::FogComponent){
        name = "FogComponent";
    }else if(component == ComponentType::LocalVolumetricFogComponent){
        name = "LocalVolumetricFogComponent";
    }else if(component == ComponentType::VolumetricCloudsComponent){
        name = "VolumetricCloudsComponent";
    }else if(component == ComponentType::MirrorComponent){
        name = "MirrorComponent";
    }else if(component == ComponentType::ReflectionProbeComponent){
        name = "ReflectionProbeComponent";
    }else if(component == ComponentType::UIDocumentComponent){
        name = "UIDocumentComponent";
    }else if(component == ComponentType::FluidSimulationComponent){
        name = "FluidSimulationComponent";
    }else if(component == ComponentType::LatticeComponent){
        name = "LatticeComponent";
    }else if(component == ComponentType::LatticeModifierComponent){
        name = "LatticeModifierComponent";
    }else if(component == ComponentType::LatticeFollowerComponent){
        name = "LatticeFollowerComponent";
    }else if(component == ComponentType::LatticeTracksComponent){
        name = "LatticeTracksComponent";
    }else if(component == ComponentType::PoseAnimatorComponent){
        name = "PoseAnimatorComponent";
    }else if(component == ComponentType::AnimationPlayerComponent){
        name = "AnimationPlayerComponent";
    }else if(component == ComponentType::Light2DComponent){
        name = "Light2DComponent";
    }else if(component == ComponentType::ShadowCaster2DComponent){
        name = "ShadowCaster2DComponent";
    }else if(component == ComponentType::ImageComponent){
        name = "ImageComponent";
    }else if(component == ComponentType::InstancedMeshComponent){
        name = "InstancedMeshComponent";
    }else if(component == ComponentType::Joint2DComponent){
        name = "Joint2DComponent";
    }else if(component == ComponentType::Joint3DComponent){
        name = "Joint3DComponent";
    }else if(component == ComponentType::KeyframeTracksComponent){
        name = "KeyframeTracksComponent";
    }else if(component == ComponentType::LightComponent){
        name = "LightComponent";
    }else if(component == ComponentType::LineRendererComponent){
        name = "LineRendererComponent";
    }else if(component == ComponentType::MeshComponent){
        name = "MeshComponent";
    }else if(component == ComponentType::MeshPolygonComponent){
        name = "MeshPolygonComponent";
    }else if(component == ComponentType::ModelComponent){
        name = "ModelComponent";
    }else if(component == ComponentType::MorphTracksComponent){
        name = "MorphTracksComponent";
    }else if(component == ComponentType::PanelComponent){
        name = "PanelComponent";
    }else if(component == ComponentType::ParticleSystemComponent){
        name = "ParticleSystemComponent";
    }else if(component == ComponentType::PointsComponent){
        name = "PointsComponent";
    }else if(component == ComponentType::PolygonComponent){
        name = "PolygonComponent";
    }else if(component == ComponentType::PositionTweenComponent){
        name = "PositionTweenComponent";
    }else if(component == ComponentType::ProgressbarComponent){
        name = "ProgressbarComponent";
    }else if(component == ComponentType::RotateTracksComponent){
        name = "RotateTracksComponent";
    }else if(component == ComponentType::RotationTweenComponent){
        name = "RotationTweenComponent";
    }else if(component == ComponentType::ScaleTweenComponent){
        name = "ScaleTweenComponent";
    }else if(component == ComponentType::ScaleTracksComponent){
        name = "ScaleTracksComponent";
    }else if(component == ComponentType::ScriptComponent){
        name = "ScriptComponent";
    }else if(component == ComponentType::ScrollbarComponent){
        name = "ScrollbarComponent";
    }else if(component == ComponentType::SkyboxComponent){
        name = "SkyboxComponent";
    }else if(component == ComponentType::SpriteAnimationComponent){
        name = "SpriteAnimationComponent";
    }else if(component == ComponentType::SpriteComponent){
        name = "SpriteComponent";
    }else if(component == ComponentType::TerrainComponent){
        name = "TerrainComponent";
    }else if(component == ComponentType::TextComponent){
        name = "TextComponent";
    }else if(component == ComponentType::InputFieldComponent){
        name = "InputFieldComponent";
    }else if(component == ComponentType::TilemapComponent){
        name = "TilemapComponent";

    }else if(component == ComponentType::TweenComponent){
        name = "TweenComponent";
    }else if(component == ComponentType::Transform){
        name = "Transform";
    }else if(component == ComponentType::TranslateTracksComponent){
        name = "TranslateTracksComponent";
    }else if(component == ComponentType::UIComponent){
        name = "UIComponent";
    }else if(component == ComponentType::LayoutGroupComponent){
        name = "LayoutGroupComponent";
    }else if(component == ComponentType::RectTransformComponent){
        name = "RectTransformComponent";
    }else{
        return "";
    }

    return name;
}
}

std::string editor::Catalog::getComponentDisplayName(ComponentType component){
    switch (component){
        case ComponentType::Transform: return "Transform";
        case ComponentType::ScriptComponent: return "Script";
        case ComponentType::MeshComponent: return "Mesh Renderer";
        case ComponentType::LineRendererComponent: return "Line Renderer";
        case ComponentType::LightComponent: return "Light";
        case ComponentType::TerrainComponent: return "Terrain";
        case ComponentType::PointsComponent: return "Point Renderer";
        case ComponentType::SkyboxComponent: return "Skybox";
        case ComponentType::ModelComponent: return "Model";
        case ComponentType::UIComponent: return "Canvas Renderer";
        case ComponentType::RectTransformComponent: return "Rect Transform";
        case ComponentType::ActionComponent: return "Action";
        case ComponentType::TweenComponent: return "Tween";
        case ComponentType::AlphaTweenComponent: return "Alpha Tween";
        case ComponentType::AnimationComponent: return "Animation";
        case ComponentType::AudioSourceComponent: return "Audio Source";
        case ComponentType::Rigidbody2DComponent: return "Rigidbody 2D";
        case ComponentType::RigidbodyComponent: return "Rigidbody";
        case ComponentType::BoneComponent: return "Bone";
        case ComponentType::PrefabComponent: return "Prefab";
        case ComponentType::ButtonComponent: return "Button";
        case ComponentType::CameraComponent: return "Camera";
        case ComponentType::ColorTweenComponent: return "Color Tween";
        case ComponentType::FogComponent: return "Fog";
        case ComponentType::ImageComponent: return "Image";
        case ComponentType::InstancedMeshComponent: return "Mesh Instancing";
        case ComponentType::Joint2DComponent: return "Joint 2D";
        case ComponentType::Joint3DComponent: return "Joint";
        case ComponentType::KeyframeTracksComponent: return "Keyframe Tracks";
        case ComponentType::MeshPolygonComponent: return "Polygon Mesh";
        case ComponentType::MorphTracksComponent: return "Morph Tracks";
        case ComponentType::PanelComponent: return "Panel";
        case ComponentType::ParticleSystemComponent: return "Particle System";
        case ComponentType::PolygonComponent: return "Polygon";
        case ComponentType::PositionTweenComponent: return "Position Tween";
        case ComponentType::ProgressbarComponent: return "Progress Bar";
        case ComponentType::RotateTracksComponent: return "Rotation Tracks";
        case ComponentType::RotationTweenComponent: return "Rotation Tween";
        case ComponentType::ScaleTweenComponent: return "Scale Tween";
        case ComponentType::ScaleTracksComponent: return "Scale Tracks";
        case ComponentType::ScrollbarComponent: return "Scrollbar";
        case ComponentType::SpriteAnimationComponent: return "Sprite Animation";
        case ComponentType::SpriteComponent: return "Sprite Renderer";
        case ComponentType::TextComponent: return "Text";
        case ComponentType::InputFieldComponent: return "Input Field";
        case ComponentType::TilemapComponent: return "Tilemap";
        case ComponentType::TranslateTracksComponent: return "Position Tracks";
        case ComponentType::LayoutGroupComponent: return "Layout Group";
        case ComponentType::MirrorComponent: return "Mirror";
        case ComponentType::Light2DComponent: return "Light 2D";
        case ComponentType::ShadowCaster2DComponent: return "Shadow Caster 2D";
        case ComponentType::ReflectionProbeComponent: return "Reflection Probe";
        case ComponentType::UIDocumentComponent: return "UI Document";
        case ComponentType::FluidSimulationComponent: return "Fluid Simulation";
        case ComponentType::LatticeComponent: return "Lattice";
        case ComponentType::LatticeModifierComponent: return "Lattice Modifier";
        case ComponentType::LatticeFollowerComponent: return "Lattice Follower";
        case ComponentType::LatticeTracksComponent: return "Lattice Tracks";
        case ComponentType::LocalVolumetricFogComponent: return "Local Volumetric Fog";
        case ComponentType::VolumetricCloudsComponent: return "Volumetric Clouds";
        case ComponentType::PoseAnimatorComponent: return "Pose Animator";
        case ComponentType::AnimationPlayerComponent: return "Animation Player";
    }
    return getComponentName(component);
}

const char* editor::Catalog::getComponentIcon(ComponentType component){
    switch (component){
        case ComponentType::Transform: return ICON_FA_UP_DOWN_LEFT_RIGHT;
        case ComponentType::ScriptComponent: return ICON_FA_CODE;
        case ComponentType::MeshComponent: return ICON_FA_SHAPES;
        case ComponentType::LineRendererComponent: return ICON_FA_GRIP_LINES;
        case ComponentType::LightComponent: return ICON_FA_LIGHTBULB;
        case ComponentType::TerrainComponent: return ICON_FA_MOUNTAIN;
        case ComponentType::PointsComponent: return ICON_FA_BRAILLE;
        case ComponentType::SkyboxComponent: return ICON_FA_CLOUD_SUN;
        case ComponentType::ModelComponent: return ICON_FA_CUBES;
        case ComponentType::UIComponent: return ICON_FA_PAINTBRUSH;
        case ComponentType::RectTransformComponent: return ICON_FA_VECTOR_SQUARE;
        case ComponentType::ActionComponent: return ICON_FA_PLAY;
        case ComponentType::TweenComponent: return ICON_FA_CLOCK;
        case ComponentType::AlphaTweenComponent: return ICON_FA_EYE;
        case ComponentType::AnimationComponent: return ICON_FA_FILM;
        case ComponentType::AudioSourceComponent: return ICON_FA_VOLUME_HIGH;
        case ComponentType::Rigidbody2DComponent: return ICON_FA_CIRCLE;
        case ComponentType::RigidbodyComponent: return ICON_FA_CUBE;
        case ComponentType::BoneComponent: return ICON_FA_BONE;
        case ComponentType::PrefabComponent: return ICON_FA_CUBE;
        case ComponentType::ButtonComponent: return ICON_FA_SQUARE_CHECK;
        case ComponentType::CameraComponent: return ICON_FA_VIDEO;
        case ComponentType::ColorTweenComponent: return ICON_FA_PALETTE;
        case ComponentType::FogComponent: return ICON_FA_SMOG;
        case ComponentType::ImageComponent: return ICON_FA_IMAGE;
        case ComponentType::InstancedMeshComponent: return ICON_FA_CLONE;
        case ComponentType::Joint2DComponent: return ICON_FA_LINK;
        case ComponentType::Joint3DComponent: return ICON_FA_LINK;
        case ComponentType::KeyframeTracksComponent: return ICON_FA_KEY;
        case ComponentType::MeshPolygonComponent: return ICON_FA_DRAW_POLYGON;
        case ComponentType::MorphTracksComponent: return ICON_FA_FACE_SMILE;
        case ComponentType::PanelComponent: return ICON_FA_WINDOW_MAXIMIZE;
        case ComponentType::ParticleSystemComponent: return ICON_FA_FIRE;
        case ComponentType::PolygonComponent: return ICON_FA_DRAW_POLYGON;
        case ComponentType::PositionTweenComponent: return ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT;
        case ComponentType::ProgressbarComponent: return ICON_FA_BARS_PROGRESS;
        case ComponentType::RotateTracksComponent: return ICON_FA_CIRCLE_NOTCH;
        case ComponentType::RotationTweenComponent: return ICON_FA_ROTATE;
        case ComponentType::ScaleTweenComponent: return ICON_FA_EXPAND;
        case ComponentType::ScaleTracksComponent: return ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER;
        case ComponentType::ScrollbarComponent: return ICON_FA_ARROWS_UP_DOWN;
        case ComponentType::SpriteAnimationComponent: return ICON_FA_FILM;
        case ComponentType::SpriteComponent: return ICON_FA_IMAGE;
        case ComponentType::TextComponent: return ICON_FA_FONT;
        case ComponentType::InputFieldComponent: return ICON_FA_KEYBOARD;
        case ComponentType::TilemapComponent: return ICON_FA_BORDER_ALL;
        case ComponentType::TranslateTracksComponent: return ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT;
        case ComponentType::LayoutGroupComponent: return ICON_FA_TABLE_CELLS;
        case ComponentType::MirrorComponent: return ICON_FA_CLONE;
        case ComponentType::Light2DComponent: return ICON_FA_LIGHTBULB;
        case ComponentType::ShadowCaster2DComponent: return ICON_FA_CIRCLE_HALF_STROKE;
        case ComponentType::ReflectionProbeComponent: return ICON_FA_CIRCLE_DOT;
        case ComponentType::UIDocumentComponent: return ICON_FA_FILE_CODE;
        case ComponentType::FluidSimulationComponent: return ICON_FA_DROPLET;
        case ComponentType::LatticeComponent: return ICON_FA_BORDER_NONE;
        case ComponentType::LatticeModifierComponent: return ICON_FA_WAND_MAGIC_SPARKLES;
        case ComponentType::LatticeFollowerComponent: return ICON_FA_LOCATION_ARROW;
        case ComponentType::LatticeTracksComponent: return ICON_FA_CUBES_STACKED;
        case ComponentType::LocalVolumetricFogComponent: return ICON_FA_SMOG;
        case ComponentType::VolumetricCloudsComponent: return ICON_FA_CLOUD;
        case ComponentType::PoseAnimatorComponent: return ICON_FA_PERSON_RUNNING;
        case ComponentType::AnimationPlayerComponent: return ICON_FA_FILM;
    }
    return ICON_FA_PUZZLE_PIECE;
}

bool editor::Catalog::hasEnabledProperty(ComponentType component){
    return component == ComponentType::ShadowCaster2DComponent || findEnabledGetter(component) != nullptr;
}

void editor::Catalog::markChanged(EntityRegistry* registry, Entity entity, ComponentType compType) {
    if (registry && registry->isEntityCreated(entity)) {
        registry->markChanged(getComponentId(registry, compType), entity);
    }
}

ComponentId editor::Catalog::getComponentId(const EntityRegistry* registry, ComponentType compType) {
    using namespace jasperengine;
    switch (compType) {
        case ComponentType::Transform:
            return registry->getComponentId<Transform>();
        case ComponentType::MeshComponent:
            return registry->getComponentId<MeshComponent>();
        case ComponentType::UIComponent:
            return registry->getComponentId<UIComponent>();
        case ComponentType::RectTransformComponent:
            return registry->getComponentId<RectTransformComponent>();
        case ComponentType::ActionComponent:
            return registry->getComponentId<ActionComponent>();
        case ComponentType::AlphaTweenComponent:
            return registry->getComponentId<AlphaTweenComponent>();
        case ComponentType::AnimationComponent:
            return registry->getComponentId<AnimationComponent>();
        case ComponentType::AudioSourceComponent:
            return registry->getComponentId<AudioSourceComponent>();
        case ComponentType::Rigidbody2DComponent:
            return registry->getComponentId<Rigidbody2DComponent>();
        case ComponentType::RigidbodyComponent:
            return registry->getComponentId<RigidbodyComponent>();
        case ComponentType::BoneComponent:
            return registry->getComponentId<BoneComponent>();
        case ComponentType::PrefabComponent:
            return registry->getComponentId<PrefabComponent>();
        case ComponentType::ButtonComponent:
            return registry->getComponentId<ButtonComponent>();
        case ComponentType::CameraComponent:
            return registry->getComponentId<CameraComponent>();
        case ComponentType::ColorTweenComponent:
            return registry->getComponentId<ColorTweenComponent>();
        case ComponentType::FogComponent:
            return registry->getComponentId<FogComponent>();
        case ComponentType::LocalVolumetricFogComponent:
            return registry->getComponentId<LocalVolumetricFogComponent>();
        case ComponentType::VolumetricCloudsComponent:
            return registry->getComponentId<VolumetricCloudsComponent>();
        case ComponentType::MirrorComponent:
            return registry->getComponentId<MirrorComponent>();
        case ComponentType::ReflectionProbeComponent:
            return registry->getComponentId<ReflectionProbeComponent>();
        case ComponentType::UIDocumentComponent:
            return registry->getComponentId<UIDocumentComponent>();
        case ComponentType::FluidSimulationComponent:
            return registry->getComponentId<FluidSimulationComponent>();
        case ComponentType::LatticeComponent:
            return registry->getComponentId<LatticeComponent>();
        case ComponentType::LatticeModifierComponent:
            return registry->getComponentId<LatticeModifierComponent>();
        case ComponentType::LatticeFollowerComponent:
            return registry->getComponentId<LatticeFollowerComponent>();
        case ComponentType::LatticeTracksComponent:
            return registry->getComponentId<LatticeTracksComponent>();
        case ComponentType::PoseAnimatorComponent:
            return registry->getComponentId<PoseAnimatorComponent>();
        case ComponentType::AnimationPlayerComponent:
            return registry->getComponentId<AnimationPlayerComponent>();
        case ComponentType::Light2DComponent:
            return registry->getComponentId<Light2DComponent>();
        case ComponentType::ShadowCaster2DComponent:
            return registry->getComponentId<ShadowCaster2DComponent>();
        case ComponentType::ImageComponent:
            return registry->getComponentId<ImageComponent>();
        case ComponentType::InstancedMeshComponent:
            return registry->getComponentId<InstancedMeshComponent>();
        case ComponentType::Joint2DComponent:
            return registry->getComponentId<Joint2DComponent>();
        case ComponentType::Joint3DComponent:
            return registry->getComponentId<Joint3DComponent>();
        case ComponentType::KeyframeTracksComponent:
            return registry->getComponentId<KeyframeTracksComponent>();
        case ComponentType::LightComponent:
            return registry->getComponentId<LightComponent>();
        case ComponentType::LineRendererComponent:
            return registry->getComponentId<LineRendererComponent>();
        case ComponentType::MeshPolygonComponent:
            return registry->getComponentId<MeshPolygonComponent>();
        case ComponentType::ModelComponent:
            return registry->getComponentId<ModelComponent>();
        case ComponentType::MorphTracksComponent:
            return registry->getComponentId<MorphTracksComponent>();
        case ComponentType::PanelComponent:
            return registry->getComponentId<PanelComponent>();
        case ComponentType::ParticleSystemComponent:
            return registry->getComponentId<ParticleSystemComponent>();
        case ComponentType::PointsComponent:
            return registry->getComponentId<PointsComponent>();
        case ComponentType::PolygonComponent:
            return registry->getComponentId<PolygonComponent>();
        case ComponentType::PositionTweenComponent:
            return registry->getComponentId<PositionTweenComponent>();
        case ComponentType::ProgressbarComponent:
            return registry->getComponentId<ProgressbarComponent>();
        case ComponentType::RotateTracksComponent:
            return registry->getComponentId<RotateTracksComponent>();
        case ComponentType::RotationTweenComponent:
            return registry->getComponentId<RotationTweenComponent>();
        case ComponentType::ScaleTweenComponent:
            return registry->getComponentId<ScaleTweenComponent>();
        case ComponentType::ScaleTracksComponent:
            return registry->getComponentId<ScaleTracksComponent>();
        case ComponentType::ScriptComponent:
            return registry->getComponentId<ScriptComponent>();
        case ComponentType::ScrollbarComponent:
            return registry->getComponentId<ScrollbarComponent>();
        case ComponentType::SkyboxComponent:
            return registry->getComponentId<SkyboxComponent>();
        case ComponentType::SpriteAnimationComponent:
            return registry->getComponentId<SpriteAnimationComponent>();
        case ComponentType::SpriteComponent:
            return registry->getComponentId<SpriteComponent>();
        case ComponentType::TerrainComponent:
            return registry->getComponentId<TerrainComponent>();
        case ComponentType::TextComponent:
            return registry->getComponentId<TextComponent>();
        case ComponentType::InputFieldComponent:
            return registry->getComponentId<InputFieldComponent>();
        case ComponentType::TilemapComponent:
            return registry->getComponentId<TilemapComponent>();
        case ComponentType::TweenComponent:
            return registry->getComponentId<TweenComponent>();
        case ComponentType::TranslateTracksComponent:
            return registry->getComponentId<TranslateTracksComponent>();
        case ComponentType::LayoutGroupComponent:
            return registry->getComponentId<LayoutGroupComponent>();
        default:
            return 0;
    }
}

editor::ComponentType editor::Catalog::getComponentType(const std::string& componentName) {
    std::string normalizedName = removeComponentSuffix(componentName);

    if(normalizedName == "action"){
        return ComponentType::ActionComponent;
    }else if(normalizedName == "alphatween"){
        return ComponentType::AlphaTweenComponent;
    }else if(normalizedName == "animation"){
        return ComponentType::AnimationComponent;
    }else if(normalizedName == "audiosource"){
        return ComponentType::AudioSourceComponent;
    }else if(normalizedName == "rigidbody2d"){
        return ComponentType::Rigidbody2DComponent;
    }else if(normalizedName == "rigidbody"){
        return ComponentType::RigidbodyComponent;
    }else if(normalizedName == "bone"){
        return ComponentType::BoneComponent;
    }else if(normalizedName == "prefab"){
        return ComponentType::PrefabComponent;
    }else if(normalizedName == "button"){
        return ComponentType::ButtonComponent;
    }else if(normalizedName == "camera"){
        return ComponentType::CameraComponent;
    }else if(normalizedName == "colortween"){
        return ComponentType::ColorTweenComponent;
    }else if(normalizedName == "fog"){
        return ComponentType::FogComponent;
    }else if(normalizedName == "localvolumetricfog"){
        return ComponentType::LocalVolumetricFogComponent;
    }else if(normalizedName == "volumetricclouds"){
        return ComponentType::VolumetricCloudsComponent;
    }else if(normalizedName == "mirror"){
        return ComponentType::MirrorComponent;
    }else if(normalizedName == "reflectionprobe"){
        return ComponentType::ReflectionProbeComponent;
    }else if(normalizedName == "uidocument"){
        return ComponentType::UIDocumentComponent;
    }else if(normalizedName == "fluidsimulation"){
        return ComponentType::FluidSimulationComponent;
    }else if(normalizedName == "lattice"){
        return ComponentType::LatticeComponent;
    }else if(normalizedName == "latticemodifier"){
        return ComponentType::LatticeModifierComponent;
    }else if(normalizedName == "latticefollower"){
        return ComponentType::LatticeFollowerComponent;
    }else if(normalizedName == "latticetracks"){
        return ComponentType::LatticeTracksComponent;
    }else if(normalizedName == "poseanimator"){
        return ComponentType::PoseAnimatorComponent;
    }else if(normalizedName == "animationplayer"){
        return ComponentType::AnimationPlayerComponent;
    }else if(normalizedName == "light2d"){
        return ComponentType::Light2DComponent;
    }else if(normalizedName == "shadowcaster2d"){
        return ComponentType::ShadowCaster2DComponent;
    }else if(normalizedName == "image"){
        return ComponentType::ImageComponent;
    }else if(normalizedName == "instancedmesh"){
        return ComponentType::InstancedMeshComponent;
    }else if(normalizedName == "joint2d"){
        return ComponentType::Joint2DComponent;
    }else if(normalizedName == "joint3d"){
        return ComponentType::Joint3DComponent;
    }else if(normalizedName == "keyframetracks"){
        return ComponentType::KeyframeTracksComponent;
    }else if(normalizedName == "light"){
        return ComponentType::LightComponent;
    }else if(normalizedName == "linerenderer"){
        return ComponentType::LineRendererComponent;
    }else if(normalizedName == "mesh"){
        return ComponentType::MeshComponent;
    }else if(normalizedName == "meshpolygon"){
        return ComponentType::MeshPolygonComponent;
    }else if(normalizedName == "model"){
        return ComponentType::ModelComponent;
    }else if(normalizedName == "morphtracks"){
        return ComponentType::MorphTracksComponent;
    }else if(normalizedName == "panel"){
        return ComponentType::PanelComponent;
    }else if(normalizedName == "particlesystem"){
        return ComponentType::ParticleSystemComponent;
    }else if(normalizedName == "points"){
        return ComponentType::PointsComponent;
    }else if(normalizedName == "polygon"){
        return ComponentType::PolygonComponent;
    }else if(normalizedName == "positiontween"){
        return ComponentType::PositionTweenComponent;
    }else if(normalizedName == "progressbar"){
        return ComponentType::ProgressbarComponent;
    }else if(normalizedName == "rotatetracks"){
        return ComponentType::RotateTracksComponent;
    }else if(normalizedName == "rotationtween"){
        return ComponentType::RotationTweenComponent;
    }else if(normalizedName == "scaletween"){
        return ComponentType::ScaleTweenComponent;
    }else if(normalizedName == "scaletracks"){
        return ComponentType::ScaleTracksComponent;
    }else if(normalizedName == "script"){
        return ComponentType::ScriptComponent;
    }else if(normalizedName == "scrollbar"){
        return ComponentType::ScrollbarComponent;
    }else if(normalizedName == "skybox"){
        return ComponentType::SkyboxComponent;
    }else if(normalizedName == "spriteanimation"){
        return ComponentType::SpriteAnimationComponent;
    }else if(normalizedName == "sprite"){
        return ComponentType::SpriteComponent;
    }else if(normalizedName == "terrain"){
        return ComponentType::TerrainComponent;
    }else if(normalizedName == "text"){
        return ComponentType::TextComponent;
    }else if(normalizedName == "inputfield"){
        return ComponentType::InputFieldComponent;
    }else if(normalizedName == "tilemap"){
        return ComponentType::TilemapComponent;
    }else if(normalizedName == "tween"){
        return ComponentType::TweenComponent;
    }else if(normalizedName == "transform"){
        return ComponentType::Transform;
    }else if(normalizedName == "translatetracks"){
        return ComponentType::TranslateTracksComponent;
    }else if(normalizedName == "ui"){
        return ComponentType::UIComponent;
    }else if(normalizedName == "layoutgroup"){
        return ComponentType::LayoutGroupComponent;
    }else if(normalizedName == "recttransform"){
        return ComponentType::RectTransformComponent;
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

    if (registry->findComponent<ActionComponent>(entity)){
        ret.push_back(ComponentType::ActionComponent);
    }
    if (registry->findComponent<AlphaTweenComponent>(entity)){
        ret.push_back(ComponentType::AlphaTweenComponent);
    }
    if (registry->findComponent<AnimationComponent>(entity)){
        ret.push_back(ComponentType::AnimationComponent);
    }
    if (registry->findComponent<AudioSourceComponent>(entity)){
        ret.push_back(ComponentType::AudioSourceComponent);
    }
    if (registry->findComponent<Rigidbody2DComponent>(entity)){
        ret.push_back(ComponentType::Rigidbody2DComponent);
    }
    if (registry->findComponent<RigidbodyComponent>(entity)){
        ret.push_back(ComponentType::RigidbodyComponent);
    }
    if (registry->findComponent<BoneComponent>(entity)){
        ret.push_back(ComponentType::BoneComponent);
    }
    if (registry->findComponent<PrefabComponent>(entity)){
        ret.push_back(ComponentType::PrefabComponent);
    }
    if (registry->findComponent<ButtonComponent>(entity)){
        ret.push_back(ComponentType::ButtonComponent);
    }
    if (registry->findComponent<CameraComponent>(entity)){
        ret.push_back(ComponentType::CameraComponent);
    }
    if (registry->findComponent<ColorTweenComponent>(entity)){
        ret.push_back(ComponentType::ColorTweenComponent);
    }
    if (registry->findComponent<FogComponent>(entity)){
        ret.push_back(ComponentType::FogComponent);
    }
    if (registry->findComponent<LocalVolumetricFogComponent>(entity)){
        ret.push_back(ComponentType::LocalVolumetricFogComponent);
    }
    if (registry->findComponent<VolumetricCloudsComponent>(entity)){
        ret.push_back(ComponentType::VolumetricCloudsComponent);
    }
    if (registry->findComponent<MirrorComponent>(entity)){
        ret.push_back(ComponentType::MirrorComponent);
    }
    if (registry->findComponent<ReflectionProbeComponent>(entity)){
        ret.push_back(ComponentType::ReflectionProbeComponent);
    }
    if (registry->findComponent<UIDocumentComponent>(entity)){
        ret.push_back(ComponentType::UIDocumentComponent);
    }
    if (registry->findComponent<FluidSimulationComponent>(entity)){
        ret.push_back(ComponentType::FluidSimulationComponent);
    }
    if (registry->findComponent<LatticeComponent>(entity)){
        ret.push_back(ComponentType::LatticeComponent);
    }
    if (registry->findComponent<LatticeModifierComponent>(entity)){
        ret.push_back(ComponentType::LatticeModifierComponent);
    }
    if (registry->findComponent<LatticeFollowerComponent>(entity)){
        ret.push_back(ComponentType::LatticeFollowerComponent);
    }
    if (registry->findComponent<LatticeTracksComponent>(entity)){
        ret.push_back(ComponentType::LatticeTracksComponent);
    }
    if (registry->findComponent<Light2DComponent>(entity)){
        ret.push_back(ComponentType::Light2DComponent);
    }
    if (registry->findComponent<ShadowCaster2DComponent>(entity)){
        ret.push_back(ComponentType::ShadowCaster2DComponent);
    }
    if (registry->findComponent<ImageComponent>(entity)){
        ret.push_back(ComponentType::ImageComponent);
    }
    if (registry->findComponent<InstancedMeshComponent>(entity)){
        ret.push_back(ComponentType::InstancedMeshComponent);
    }
    if (registry->findComponent<Joint2DComponent>(entity)){
        ret.push_back(ComponentType::Joint2DComponent);
    }
    if (registry->findComponent<Joint3DComponent>(entity)){
        ret.push_back(ComponentType::Joint3DComponent);
    }
    if (registry->findComponent<KeyframeTracksComponent>(entity)){
        ret.push_back(ComponentType::KeyframeTracksComponent);
    }
    if (registry->findComponent<LightComponent>(entity)){
        ret.push_back(ComponentType::LightComponent);
    }
    if (registry->findComponent<LineRendererComponent>(entity)){
        ret.push_back(ComponentType::LineRendererComponent);
    }
    if (registry->findComponent<MeshComponent>(entity)){
        ret.push_back(ComponentType::MeshComponent);
    }
    if (registry->findComponent<MeshPolygonComponent>(entity)){
        ret.push_back(ComponentType::MeshPolygonComponent);
    }
    if (registry->findComponent<ModelComponent>(entity)){
        ret.push_back(ComponentType::ModelComponent);
    }
    if (registry->findComponent<PoseAnimatorComponent>(entity)){
        ret.push_back(ComponentType::PoseAnimatorComponent);
    }
    if (registry->findComponent<AnimationPlayerComponent>(entity)){
        ret.push_back(ComponentType::AnimationPlayerComponent);
    }
    if (registry->findComponent<MorphTracksComponent>(entity)){
        ret.push_back(ComponentType::MorphTracksComponent);
    }
    if (registry->findComponent<PanelComponent>(entity)){
        ret.push_back(ComponentType::PanelComponent);
    }
    if (registry->findComponent<ParticleSystemComponent>(entity)){
        ret.push_back(ComponentType::ParticleSystemComponent);
    }
    if (registry->findComponent<PointsComponent>(entity)){
        ret.push_back(ComponentType::PointsComponent);
    }
    if (registry->findComponent<PolygonComponent>(entity)){
        ret.push_back(ComponentType::PolygonComponent);
    }
    if (registry->findComponent<PositionTweenComponent>(entity)){
        ret.push_back(ComponentType::PositionTweenComponent);
    }
    if (registry->findComponent<ProgressbarComponent>(entity)){
        ret.push_back(ComponentType::ProgressbarComponent);
    }
    if (registry->findComponent<RotateTracksComponent>(entity)){
        ret.push_back(ComponentType::RotateTracksComponent);
    }
    if (registry->findComponent<RotationTweenComponent>(entity)){
        ret.push_back(ComponentType::RotationTweenComponent);
    }
    if (registry->findComponent<ScaleTweenComponent>(entity)){
        ret.push_back(ComponentType::ScaleTweenComponent);
    }
    if (registry->findComponent<ScaleTracksComponent>(entity)){
        ret.push_back(ComponentType::ScaleTracksComponent);
    }
    if (registry->findComponent<ScriptComponent>(entity)){
        ret.push_back(ComponentType::ScriptComponent);
    }
    if (registry->findComponent<ScrollbarComponent>(entity)){
        ret.push_back(ComponentType::ScrollbarComponent);
    }
    if (registry->findComponent<SkyboxComponent>(entity)){
        ret.push_back(ComponentType::SkyboxComponent);
    }
    if (registry->findComponent<SpriteAnimationComponent>(entity)){
        ret.push_back(ComponentType::SpriteAnimationComponent);
    }
    if (registry->findComponent<SpriteComponent>(entity)){
        ret.push_back(ComponentType::SpriteComponent);
    }
    if (registry->findComponent<TerrainComponent>(entity)){
        ret.push_back(ComponentType::TerrainComponent);
    }
    if (registry->findComponent<TextComponent>(entity)){
        ret.push_back(ComponentType::TextComponent);
    }
    if (registry->findComponent<InputFieldComponent>(entity)){
        ret.push_back(ComponentType::InputFieldComponent);
    }
    if (registry->findComponent<TilemapComponent>(entity)){
        ret.push_back(ComponentType::TilemapComponent);
    }
    if (registry->findComponent<TweenComponent>(entity)){
        ret.push_back(ComponentType::TweenComponent);
    }
    if (registry->findComponent<Transform>(entity)){
        ret.push_back(ComponentType::Transform);
    }
    if (registry->findComponent<TranslateTracksComponent>(entity)){
        ret.push_back(ComponentType::TranslateTracksComponent);
    }
    if (registry->findComponent<UIComponent>(entity)){
        ret.push_back(ComponentType::UIComponent);
    }
    if (registry->findComponent<LayoutGroupComponent>(entity)){
        ret.push_back(ComponentType::LayoutGroupComponent);
    }
    if (registry->findComponent<RectTransformComponent>(entity)){
        ret.push_back(ComponentType::RectTransformComponent);
    }

    return ret;
}

bool editor::Catalog::mayHoldReferences(EntityRegistry* registry, Entity entity, ComponentType component, bool entities, bool cameraTextures){
    // The cases below rely on what those components enumerate. Checked once against their
    // property list, so a reference property added to one later turns the shortcut off
    // instead of being skipped.
    static const bool shortcutsHold = []() {
        for (ComponentType type : {ComponentType::Transform, ComponentType::SpriteComponent, ComponentType::MeshComponent}) {
            for (const auto& [name, property] : getProperties(type, nullptr)) {
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
            // Only values (kTransformProperties, enumerateSpriteProperties)
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

    uint64_t flags = 0;

    auto oldProps = Catalog::getProperties(compType, oldComp);
    auto newProps = Catalog::getProperties(compType, newComp);

    for (auto& [name, newProp] : newProps) {
        auto it = oldProps.find(name);
        if (it == oldProps.end()) continue;

        PropertyData& oldProp = it->second;
        if (!oldProp.ref || !newProp.ref) continue;

        // A script property can change type between the two, the casts below would read
        // the old value as the new type
        if (oldProp.type != newProp.type) {
            flags |= newProp.updateFlags;
            continue;
        }

        bool changed = false;
        switch (newProp.type) {
            case PropertyType::Bool:
                changed = *static_cast<bool*>(oldProp.ref) != *static_cast<bool*>(newProp.ref);
                break;
            case PropertyType::Float:
                changed = *static_cast<float*>(oldProp.ref) != *static_cast<float*>(newProp.ref);
                break;
            case PropertyType::Double:
                changed = *static_cast<double*>(oldProp.ref) != *static_cast<double*>(newProp.ref);
                break;
            case PropertyType::Int:
                changed = *static_cast<int*>(oldProp.ref) != *static_cast<int*>(newProp.ref);
                break;
            case PropertyType::UInt:
                changed = *static_cast<unsigned int*>(oldProp.ref) != *static_cast<unsigned int*>(newProp.ref);
                break;
            case PropertyType::String:
                changed = *static_cast<std::string*>(oldProp.ref) != *static_cast<std::string*>(newProp.ref);
                break;
            case PropertyType::Asset:
                changed = *static_cast<AssetRef*>(oldProp.ref) != *static_cast<AssetRef*>(newProp.ref);
                break;
            case PropertyType::Shader:
                changed = *static_cast<ShaderRef*>(oldProp.ref) != *static_cast<ShaderRef*>(newProp.ref);
                break;
            case PropertyType::Font:
                changed = *static_cast<FontArray*>(oldProp.ref) != *static_cast<FontArray*>(newProp.ref);
                break;
            case PropertyType::Vector2:
                changed = *static_cast<Vector2*>(oldProp.ref) != *static_cast<Vector2*>(newProp.ref);
                break;
            case PropertyType::Vector3:
                changed = *static_cast<Vector3*>(oldProp.ref) != *static_cast<Vector3*>(newProp.ref);
                break;
            case PropertyType::Vector4:
                changed = *static_cast<Vector4*>(oldProp.ref) != *static_cast<Vector4*>(newProp.ref);
                break;
            case PropertyType::Quat:
                changed = *static_cast<Quaternion*>(oldProp.ref) != *static_cast<Quaternion*>(newProp.ref);
                break;
            case PropertyType::Enum:
                changed = *static_cast<int*>(oldProp.ref) != *static_cast<int*>(newProp.ref);
                break;
            case PropertyType::Ease:
                changed = *static_cast<Ease*>(oldProp.ref) != *static_cast<Ease*>(newProp.ref);
                break;
            case PropertyType::Texture:
                changed = *static_cast<Texture*>(oldProp.ref) != *static_cast<Texture*>(newProp.ref);
                break;
            case PropertyType::Material:
                changed = *static_cast<Material*>(oldProp.ref) != *static_cast<Material*>(newProp.ref);
                break;
            case PropertyType::Entity:
                changed = *static_cast<Entity*>(oldProp.ref) != *static_cast<Entity*>(newProp.ref);
                break;
            case PropertyType::EntityReference:
                changed = static_cast<EntityReference*>(oldProp.ref)->entity != static_cast<EntityReference*>(newProp.ref)->entity ||
                          static_cast<EntityReference*>(oldProp.ref)->sceneId != static_cast<EntityReference*>(newProp.ref)->sceneId;
                break;
            case PropertyType::Custom:
                // Conservative: assume changed for complex types
                changed = true;
                break;
        }

        if (changed) {
            flags |= newProp.updateFlags;
        }
    }

    return flags;
}

uint64_t editor::Catalog::getComponentStructuralUpdateFlags(ComponentType compType) {
    switch (compType) {
        case ComponentType::LightComponent:
        // adding/removing a 2D light toggles the USE_LIGHT2D shader variant
        case ComponentType::Light2DComponent:
        case ComponentType::FogComponent:
        case ComponentType::ReflectionProbeComponent:
            // variants only: the loaded ones draw until the new ones are built
            return UpdateFlags_Scene_Mesh_Variant;
        // adding/removing a mirror toggles the inverted-culling pipeline baked
        // into meshes, so they must reload
        case ComponentType::MirrorComponent:
            return UpdateFlags_Scene_Mesh_Reload;
        default:
            return UpdateFlags_None;
    }
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
        case ComponentType::SkyboxComponent:
            if (SkyboxComponent* sky = scene->findComponent<SkyboxComponent>(entity))
                return failed(sky->customShader, ShaderType::SKYBOX, sky->loaded, sky->needReload, sky->customShaderId);
            break;
        default:
            break;
    }

    return false;
}

bool editor::Catalog::isCustomDepthShaderBuildFailed(Scene* scene, Entity entity) {
    // the depth fork has no scene default; loadMesh always builds it when set
    MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    return mesh && !mesh->customDepthShader.empty() && mesh->loaded && !mesh->needReload &&
           mesh->numSubmeshes > 0 && mesh->submeshes[0].customDepthShaderId == 0;
}

bool editor::Catalog::splitShaderEntryProperty(const std::string& propertyName, std::string& collection, std::string& key) {
    for (const char* name : {"shaderParams.", "shaderTextures.", "shaderUniforms."}) {
        const size_t at = propertyName.find(name);
        if (at == std::string::npos) continue;
        const std::string prefix = propertyName.substr(0, at);
        // a submesh material's, or its extra pass's ("...material.extraPass.shaderParams.<member>")
        const bool material = prefix.compare(0, 10, "submeshes[") == 0 &&
                              (endsWith(prefix, "].material.") || endsWith(prefix, "].material.extraPass."));
        const bool component = prefix.empty();
        // a material has shaderParams, a component shaderUniforms; both have shaderTextures
        const std::string field(name, std::strlen(name) - 1);
        if ((material && field == "shaderUniforms") || (component && field == "shaderParams") || (!material && !component)) {
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
        if (SkyboxComponent* s = registry->findComponent<SkyboxComponent>(entity)) s->needReload = true;
    }
    if (updateFlags & UpdateFlags_Shader_Uniforms){
        if (MeshComponent* m = registry->findComponent<MeshComponent>(entity)) m->needUpdateShaderUniforms = true;
        if (UIComponent* u = registry->findComponent<UIComponent>(entity)) u->needUpdateShaderUniforms = true;
        if (PointsComponent* p = registry->findComponent<PointsComponent>(entity)) p->needUpdateShaderUniforms = true;
        if (LineRendererComponent* l = registry->findComponent<LineRendererComponent>(entity)) l->needUpdateShaderUniforms = true;
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
            body->needReloadBody = true;
            body->needUpdateColliders = true;
        }
    }
    if (updateFlags & UpdateFlags_Joint2D){
        if (Joint2DComponent* joint = registry->findComponent<Joint2DComponent>(entity)){
            joint->needUpdateJoint = true;
        }
    }
    if (updateFlags & UpdateFlags_Joint3D){
        if (Joint3DComponent* joint = registry->findComponent<Joint3DComponent>(entity)){
            joint->needUpdateJoint = true;
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
    if (updateFlags & UpdateFlags_AudioSource){
        if (AudioSourceComponent* audio = registry->findComponent<AudioSourceComponent>(entity)){
            audio->needUpdate = true;
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
    if (updateFlags & UpdateFlags_AnimationPlayer_Reload){
        // the clip list changed (an entry added, removed, retargeted, renamed or rewrapped): the
        // player loads it again and rebinds, keeping its states and rest values
        if (AnimationPlayerComponent* player = registry->findComponent<AnimationPlayerComponent>(entity)){
            player->needsLoad = true;
        }
    }
}

void editor::Catalog::copyComponent(EntityRegistry* sourceRegistry, Entity sourceEntity,
                                   EntityRegistry* targetRegistry, Entity targetEntity,
                                   ComponentType compType) {

    switch (compType) {
        case ComponentType::Transform: {
            Entity parent = targetRegistry->getComponent<Transform>(targetEntity).parent;
            YAML::Node encoded = Stream::encodeTransform(sourceRegistry->getComponent<Transform>(sourceEntity));
            targetRegistry->getComponent<Transform>(targetEntity) = Stream::decodeTransform(encoded);
            targetRegistry->getComponent<Transform>(targetEntity).parent = parent; // not need to re-order because it is same parent
            break;
        }

        case ComponentType::MeshComponent: {
            YAML::Node encoded = Stream::encodeMeshComponent(sourceRegistry->getComponent<MeshComponent>(sourceEntity));
            targetRegistry->getComponent<MeshComponent>(targetEntity) = Stream::decodeMeshComponent(encoded);
            break;
        }

        case ComponentType::UIComponent: {
            YAML::Node encoded = Stream::encodeUIComponent(sourceRegistry->getComponent<UIComponent>(sourceEntity));
            targetRegistry->getComponent<UIComponent>(targetEntity) = Stream::decodeUIComponent(encoded);
            break;
        }

        case ComponentType::ButtonComponent: {
            YAML::Node encoded = Stream::encodeButtonComponent(sourceRegistry->getComponent<ButtonComponent>(sourceEntity));
            targetRegistry->getComponent<ButtonComponent>(targetEntity) = Stream::decodeButtonComponent(encoded);
            break;
        }

        case ComponentType::ScrollbarComponent: {
            YAML::Node encoded = Stream::encodeScrollbarComponent(sourceRegistry->getComponent<ScrollbarComponent>(sourceEntity));
            targetRegistry->getComponent<ScrollbarComponent>(targetEntity) = Stream::decodeScrollbarComponent(encoded);
            break;
        }

        case ComponentType::ProgressbarComponent: {
            YAML::Node encoded = Stream::encodeProgressbarComponent(sourceRegistry->getComponent<ProgressbarComponent>(sourceEntity));
            targetRegistry->getComponent<ProgressbarComponent>(targetEntity) = Stream::decodeProgressbarComponent(encoded);
            break;
        }

        case ComponentType::InputFieldComponent: {
            YAML::Node encoded = Stream::encodeInputFieldComponent(sourceRegistry->getComponent<InputFieldComponent>(sourceEntity));
            targetRegistry->getComponent<InputFieldComponent>(targetEntity) = Stream::decodeInputFieldComponent(encoded);
            break;
        }

        case ComponentType::PanelComponent: {
            YAML::Node encoded = Stream::encodePanelComponent(sourceRegistry->getComponent<PanelComponent>(sourceEntity));
            targetRegistry->getComponent<PanelComponent>(targetEntity) = Stream::decodePanelComponent(encoded);
            break;
        }

        case ComponentType::PrefabComponent: {
            YAML::Node encoded = Stream::encodePrefabComponent(sourceRegistry->getComponent<PrefabComponent>(sourceEntity));
            targetRegistry->getComponent<PrefabComponent>(targetEntity) = Stream::decodePrefabComponent(encoded);
            break;
        }

        case ComponentType::RectTransformComponent: {
            YAML::Node encoded = Stream::encodeRectTransformComponent(sourceRegistry->getComponent<RectTransformComponent>(sourceEntity));
            targetRegistry->getComponent<RectTransformComponent>(targetEntity) = Stream::decodeRectTransformComponent(encoded);
            break;
        }

        case ComponentType::LayoutGroupComponent: {
            YAML::Node encoded = Stream::encodeLayoutGroupComponent(sourceRegistry->getComponent<LayoutGroupComponent>(sourceEntity));
            targetRegistry->getComponent<LayoutGroupComponent>(targetEntity) = Stream::decodeLayoutGroupComponent(encoded);
            break;
        }

        case ComponentType::ImageComponent: {
            YAML::Node encoded = Stream::encodeImageComponent(sourceRegistry->getComponent<ImageComponent>(sourceEntity));
            targetRegistry->getComponent<ImageComponent>(targetEntity) = Stream::decodeImageComponent(encoded);
            break;
        }

        case ComponentType::TextComponent: {
            YAML::Node encoded = Stream::encodeTextComponent(sourceRegistry->getComponent<TextComponent>(sourceEntity));
            targetRegistry->getComponent<TextComponent>(targetEntity) = Stream::decodeTextComponent(encoded);
            break;
        }

        case ComponentType::SpriteComponent: {
            YAML::Node encoded = Stream::encodeSpriteComponent(sourceRegistry->getComponent<SpriteComponent>(sourceEntity));
            targetRegistry->getComponent<SpriteComponent>(targetEntity) = Stream::decodeSpriteComponent(encoded);
            break;
        }

        case ComponentType::TilemapComponent: {
            YAML::Node encoded = Stream::encodeTilemapComponent(sourceRegistry->getComponent<TilemapComponent>(sourceEntity));
            targetRegistry->getComponent<TilemapComponent>(targetEntity) = Stream::decodeTilemapComponent(encoded);
            break;
        }

        case ComponentType::TerrainComponent: {
            YAML::Node encoded = Stream::encodeTerrainComponent(sourceRegistry->getComponent<TerrainComponent>(sourceEntity));
            targetRegistry->getComponent<TerrainComponent>(targetEntity) = Stream::decodeTerrainComponent(encoded);
            break;
        }

        case ComponentType::LightComponent: {
            YAML::Node encoded = Stream::encodeLightComponent(sourceRegistry->getComponent<LightComponent>(sourceEntity));
            targetRegistry->getComponent<LightComponent>(targetEntity) = Stream::decodeLightComponent(encoded);
            break;
        }

        case ComponentType::LineRendererComponent: {
            YAML::Node encoded = Stream::encodeLineRendererComponent(sourceRegistry->getComponent<LineRendererComponent>(sourceEntity));
            targetRegistry->getComponent<LineRendererComponent>(targetEntity) = Stream::decodeLineRendererComponent(encoded);
            break;
        }

        case ComponentType::PolygonComponent: {
            YAML::Node encoded = Stream::encodePolygonComponent(sourceRegistry->getComponent<PolygonComponent>(sourceEntity));
            targetRegistry->getComponent<PolygonComponent>(targetEntity) = Stream::decodePolygonComponent(encoded);
            break;
        }

        case ComponentType::MeshPolygonComponent: {
            YAML::Node encoded = Stream::encodeMeshPolygonComponent(sourceRegistry->getComponent<MeshPolygonComponent>(sourceEntity));
            targetRegistry->getComponent<MeshPolygonComponent>(targetEntity) = Stream::decodeMeshPolygonComponent(encoded);
            break;
        }

        case ComponentType::FogComponent: {
            YAML::Node encoded = Stream::encodeFogComponent(sourceRegistry->getComponent<FogComponent>(sourceEntity));
            targetRegistry->getComponent<FogComponent>(targetEntity) = Stream::decodeFogComponent(encoded);
            break;
        }

        case ComponentType::LocalVolumetricFogComponent: {
            YAML::Node encoded = Stream::encodeLocalVolumetricFogComponent(sourceRegistry->getComponent<LocalVolumetricFogComponent>(sourceEntity));
            targetRegistry->getComponent<LocalVolumetricFogComponent>(targetEntity) = Stream::decodeLocalVolumetricFogComponent(encoded);
            break;
        }

        case ComponentType::VolumetricCloudsComponent: {
            YAML::Node encoded = Stream::encodeVolumetricCloudsComponent(sourceRegistry->getComponent<VolumetricCloudsComponent>(sourceEntity));
            targetRegistry->getComponent<VolumetricCloudsComponent>(targetEntity) = Stream::decodeVolumetricCloudsComponent(encoded);
            break;
        }

        case ComponentType::MirrorComponent: {
            YAML::Node encoded = Stream::encodeMirrorComponent(sourceRegistry->getComponent<MirrorComponent>(sourceEntity));
            targetRegistry->getComponent<MirrorComponent>(targetEntity) = Stream::decodeMirrorComponent(encoded);
            break;
        }

        case ComponentType::ReflectionProbeComponent: {
            YAML::Node encoded = Stream::encodeReflectionProbeComponent(sourceRegistry->getComponent<ReflectionProbeComponent>(sourceEntity));
            targetRegistry->getComponent<ReflectionProbeComponent>(targetEntity) = Stream::decodeReflectionProbeComponent(encoded);
            break;
        }

        case ComponentType::UIDocumentComponent: {
            // decoded over the target's own: its loaded document stays unless the file changes
            YAML::Node encoded = Stream::encodeUIDocumentComponent(sourceRegistry->getComponent<UIDocumentComponent>(sourceEntity));
            UIDocumentComponent& target = targetRegistry->getComponent<UIDocumentComponent>(targetEntity);
            target = Stream::decodeUIDocumentComponent(encoded, &target);
            break;
        }

        case ComponentType::FluidSimulationComponent: {
            // decoded over the target's own: its simulator (and every texture showing its
            // output) stays; a new seed or start burst restarts it
            YAML::Node encoded = Stream::encodeFluidSimulationComponent(sourceRegistry->getComponent<FluidSimulationComponent>(sourceEntity));
            FluidSimulationComponent& target = targetRegistry->getComponent<FluidSimulationComponent>(targetEntity);
            target = Stream::decodeFluidSimulationComponent(encoded, &target);
            break;
        }

        case ComponentType::PoseAnimatorComponent: {
            // decoded over the target's own: what it plays stays unless the animation changes
            YAML::Node encoded = Stream::encodePoseAnimatorComponent(sourceRegistry->getComponent<PoseAnimatorComponent>(sourceEntity));
            PoseAnimatorComponent& target = targetRegistry->getComponent<PoseAnimatorComponent>(targetEntity);
            target = Stream::decodePoseAnimatorComponent(encoded, &target);
            break;
        }

        case ComponentType::AnimationPlayerComponent: {
            // decoded over the target's own: what it plays stays, a changed clip list reloads
            YAML::Node encoded = Stream::encodeAnimationPlayerComponent(sourceRegistry->getComponent<AnimationPlayerComponent>(sourceEntity));
            AnimationPlayerComponent& target = targetRegistry->getComponent<AnimationPlayerComponent>(targetEntity);
            target = Stream::decodeAnimationPlayerComponent(encoded, &target);
            break;
        }

        case ComponentType::Light2DComponent: {
            YAML::Node encoded = Stream::encodeLight2DComponent(sourceRegistry->getComponent<Light2DComponent>(sourceEntity));
            targetRegistry->getComponent<Light2DComponent>(targetEntity) = Stream::decodeLight2DComponent(encoded);
            break;
        }

        case ComponentType::ShadowCaster2DComponent: {
            YAML::Node encoded = Stream::encodeShadowCaster2DComponent(sourceRegistry->getComponent<ShadowCaster2DComponent>(sourceEntity));
            targetRegistry->getComponent<ShadowCaster2DComponent>(targetEntity) = Stream::decodeShadowCaster2DComponent(encoded);
            break;
        }

        case ComponentType::CameraComponent: {
            YAML::Node encoded = Stream::encodeCameraComponent(sourceRegistry->getComponent<CameraComponent>(sourceEntity));
            targetRegistry->getComponent<CameraComponent>(targetEntity) = Stream::decodeCameraComponent(encoded);
            break;
        }

        case ComponentType::AudioSourceComponent: {
            YAML::Node encoded = Stream::encodeAudioSourceComponent(sourceRegistry->getComponent<AudioSourceComponent>(sourceEntity));
            targetRegistry->getComponent<AudioSourceComponent>(targetEntity) = Stream::decodeAudioSourceComponent(encoded);
            break;
        }

        case ComponentType::SkyboxComponent: {
            YAML::Node encoded = Stream::encodeSkyboxComponent(sourceRegistry->getComponent<SkyboxComponent>(sourceEntity));
            targetRegistry->getComponent<SkyboxComponent>(targetEntity) = Stream::decodeSkyboxComponent(encoded);
            break;
        }

        case ComponentType::ScriptComponent: {
            YAML::Node encoded = Stream::encodeScriptComponent(sourceRegistry->getComponent<ScriptComponent>(sourceEntity));
            targetRegistry->getComponent<ScriptComponent>(targetEntity) = Stream::decodeScriptComponent(encoded);
            break;
        }

        case ComponentType::Joint2DComponent: {
            YAML::Node encoded = Stream::encodeJoint2DComponent(sourceRegistry->getComponent<Joint2DComponent>(sourceEntity));
            targetRegistry->getComponent<Joint2DComponent>(targetEntity) = Stream::decodeJoint2DComponent(encoded);
            break;
        }

        case ComponentType::Joint3DComponent: {
            YAML::Node encoded = Stream::encodeJoint3DComponent(sourceRegistry->getComponent<Joint3DComponent>(sourceEntity));
            targetRegistry->getComponent<Joint3DComponent>(targetEntity) = Stream::decodeJoint3DComponent(encoded);
            break;
        }

        case ComponentType::BoneComponent: {
            YAML::Node encoded = Stream::encodeBoneComponent(sourceRegistry->getComponent<BoneComponent>(sourceEntity));
            targetRegistry->getComponent<BoneComponent>(targetEntity) = Stream::decodeBoneComponent(encoded);
            break;
        }

        case ComponentType::KeyframeTracksComponent: {
            YAML::Node encoded = Stream::encodeKeyframeTracksComponent(sourceRegistry->getComponent<KeyframeTracksComponent>(sourceEntity));
            targetRegistry->getComponent<KeyframeTracksComponent>(targetEntity) = Stream::decodeKeyframeTracksComponent(encoded);
            break;
        }

        case ComponentType::TranslateTracksComponent: {
            YAML::Node encoded = Stream::encodeTranslateTracksComponent(sourceRegistry->getComponent<TranslateTracksComponent>(sourceEntity));
            targetRegistry->getComponent<TranslateTracksComponent>(targetEntity) = Stream::decodeTranslateTracksComponent(encoded);
            break;
        }

        case ComponentType::RotateTracksComponent: {
            YAML::Node encoded = Stream::encodeRotateTracksComponent(sourceRegistry->getComponent<RotateTracksComponent>(sourceEntity));
            targetRegistry->getComponent<RotateTracksComponent>(targetEntity) = Stream::decodeRotateTracksComponent(encoded);
            break;
        }

        case ComponentType::ScaleTracksComponent: {
            YAML::Node encoded = Stream::encodeScaleTracksComponent(sourceRegistry->getComponent<ScaleTracksComponent>(sourceEntity));
            targetRegistry->getComponent<ScaleTracksComponent>(targetEntity) = Stream::decodeScaleTracksComponent(encoded);
            break;
        }

        case ComponentType::MorphTracksComponent: {
            YAML::Node encoded = Stream::encodeMorphTracksComponent(sourceRegistry->getComponent<MorphTracksComponent>(sourceEntity));
            targetRegistry->getComponent<MorphTracksComponent>(targetEntity) = Stream::decodeMorphTracksComponent(encoded);
            break;
        }

        case ComponentType::TweenComponent: {
            YAML::Node encoded = Stream::encodeTweenComponent(sourceRegistry->getComponent<TweenComponent>(sourceEntity));
            targetRegistry->getComponent<TweenComponent>(targetEntity) = Stream::decodeTweenComponent(encoded);
            break;
        }

        case ComponentType::PositionTweenComponent: {
            YAML::Node encoded = Stream::encodePositionTweenComponent(sourceRegistry->getComponent<PositionTweenComponent>(sourceEntity));
            targetRegistry->getComponent<PositionTweenComponent>(targetEntity) = Stream::decodePositionTweenComponent(encoded);
            break;
        }

        case ComponentType::RotationTweenComponent: {
            YAML::Node encoded = Stream::encodeRotationTweenComponent(sourceRegistry->getComponent<RotationTweenComponent>(sourceEntity));
            targetRegistry->getComponent<RotationTweenComponent>(targetEntity) = Stream::decodeRotationTweenComponent(encoded);
            break;
        }

        case ComponentType::ScaleTweenComponent: {
            YAML::Node encoded = Stream::encodeScaleTweenComponent(sourceRegistry->getComponent<ScaleTweenComponent>(sourceEntity));
            targetRegistry->getComponent<ScaleTweenComponent>(targetEntity) = Stream::decodeScaleTweenComponent(encoded);
            break;
        }

        case ComponentType::ColorTweenComponent: {
            YAML::Node encoded = Stream::encodeColorTweenComponent(sourceRegistry->getComponent<ColorTweenComponent>(sourceEntity));
            targetRegistry->getComponent<ColorTweenComponent>(targetEntity) = Stream::decodeColorTweenComponent(encoded);
            break;
        }

        case ComponentType::AlphaTweenComponent: {
            YAML::Node encoded = Stream::encodeAlphaTweenComponent(sourceRegistry->getComponent<AlphaTweenComponent>(sourceEntity));
            targetRegistry->getComponent<AlphaTweenComponent>(targetEntity) = Stream::decodeAlphaTweenComponent(encoded);
            break;
        }

        case ComponentType::ParticleSystemComponent: {
            YAML::Node encoded = Stream::encodeParticleSystemComponent(sourceRegistry->getComponent<ParticleSystemComponent>(sourceEntity));
            targetRegistry->getComponent<ParticleSystemComponent>(targetEntity) = Stream::decodeParticleSystemComponent(encoded);
            break;
        }

        // decoded over the target's own: a live lattice keeps its jiggle and tween state
        case ComponentType::LatticeComponent: {
            YAML::Node encoded = Stream::encodeLatticeComponent(sourceRegistry->getComponent<LatticeComponent>(sourceEntity));
            LatticeComponent& target = targetRegistry->getComponent<LatticeComponent>(targetEntity);
            target = Stream::decodeLatticeComponent(encoded, &target);
            target.revision++;
            break;
        }

        case ComponentType::LatticeModifierComponent: {
            YAML::Node encoded = Stream::encodeLatticeModifierComponent(sourceRegistry->getComponent<LatticeModifierComponent>(sourceEntity));
            targetRegistry->getComponent<LatticeModifierComponent>(targetEntity) = Stream::decodeLatticeModifierComponent(encoded);
            break;
        }

        case ComponentType::LatticeFollowerComponent: {
            YAML::Node encoded = Stream::encodeLatticeFollowerComponent(sourceRegistry->getComponent<LatticeFollowerComponent>(sourceEntity));
            targetRegistry->getComponent<LatticeFollowerComponent>(targetEntity) = Stream::decodeLatticeFollowerComponent(encoded);
            break;
        }

        case ComponentType::LatticeTracksComponent: {
            YAML::Node encoded = Stream::encodeLatticeTracksComponent(sourceRegistry->getComponent<LatticeTracksComponent>(sourceEntity));
            targetRegistry->getComponent<LatticeTracksComponent>(targetEntity) = Stream::decodeLatticeTracksComponent(encoded);
            break;
        }

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
            break;

        default:
            printf("WARNING: Unsupported component type for copying: %s\n", getComponentName(compType).c_str());
            break;
    }
}

void editor::Catalog::copyPropertyValue(EntityRegistry* sourceRegistry, Entity sourceEntity, 
                                       EntityRegistry* targetRegistry, Entity targetEntity, 
                                       ComponentType compType, const std::string& property) {

    // Get the property data to determine the type
    auto sourceProperties = Catalog::findEntityProperties(sourceRegistry, sourceEntity, compType);
    auto propIt = sourceProperties.find(property);
    if (propIt == sourceProperties.end()) {
        return; // Property not found
    }

    PropertyType propType = propIt->second.type;

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
        case PropertyType::Custom:
            if (compType == ComponentType::ScriptComponent) {
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
            } else if (compType == ComponentType::AnimationPlayerComponent && property == "clips") {
                auto* source = Catalog::getPropertyRef<std::vector<AnimationPlayerClip>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<AnimationPlayerClip>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
            } else if (compType == ComponentType::AnimationPlayerComponent && property.compare(0, 6, "clips[") == 0 && endsWith(property, "].wrap")) {
                auto* source = Catalog::getPropertyRef<ClipWrapMode>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<ClipWrapMode>(targetRegistry, targetEntity, compType, property);
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
            } else if (property == "shaderFeatures" || endsWith(property, "material.shaderFeatures") || endsWith(property, "extraPass.shaderFeatures")) {
                auto* source = Catalog::getPropertyRef<std::vector<std::string>>(sourceRegistry, sourceEntity, compType, property);
                auto* target = Catalog::getPropertyRef<std::vector<std::string>>(targetRegistry, targetEntity, compType, property);
                if (source && target) *target = *source;
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

    // Apply any update flags that are associated with this property
    updateEntity(targetRegistry, targetEntity, propIt->second.updateFlags);
}

editor::PropertyData editor::Catalog::findProperty(EntityRegistry* registry, Entity entity, ComponentType component, std::string propertyName){
    return tryGetFastProperty(registry, entity, component, propertyName);
}

editor::PropertyData editor::Catalog::getProperty(EntityRegistry* registry, Entity entity, ComponentType component, std::string propertyName){
    PropertyData fastProperty = findProperty(registry, entity, component, propertyName);
    if (fastProperty.ref || fastProperty.def){
        return fastProperty;
    }

    printf("ERROR: Cannot find property %s\n", propertyName.c_str());
    return PropertyData();
}
