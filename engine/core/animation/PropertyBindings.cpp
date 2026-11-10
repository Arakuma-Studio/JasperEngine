// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PropertyBindings.h"

#include "Scene.h"
#include "animation/CanvasGroupTracks.h"
#include "sprite/SpriteSheet.h"
#include "component/AudioSourceComponent.h"
#include "component/AnimatorComponent.h"
#include "component/BuoyancyComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/ForceFieldComponent.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "component/SortingGroupComponent.h"
#include "component/SpriteBatchComponent.h"
#include "component/SplineAnimateComponent.h"
#include "component/SplineComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/SplineInstantiateComponent.h"
#include "component/WaterExcluderComponent.h"
#include "component/WaterVolumeComponent.h"
#include "component/WaterWakeComponent.h"
#include "component/ButtonComponent.h"
#include "component/CameraComponent.h"
#include "component/CameraFollowComponent.h"
#include "component/CameraShakerComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/FluidSimulationComponent.h"
#include "component/FogComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeFollowerComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/Light2DComponent.h"
#include "component/LightComponent.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/ModelComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/RagdollComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/SafeAreaComponent.h"
#include "component/ScriptComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/CapsuleShadowCasterComponent.h"
#include "component/SkyboxComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/TextEffectComponent.h"
#include "component/TitleArtRendererComponent.h"
#include "subsystem/TitleArtSystem.h"
#include "component/TilemapComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/VoxelVolumeComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "component/VideoPlayerComponent.h"
#include "script/LuaBinding.h"
#include "script/ScriptRuntime.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "box2d/box2d.h"
#endif
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"
#include "subsystem/MeshSystem.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <type_traits>

using namespace jasperengine;
using anim::AnimatableProperty;
using anim::PropertyParams;

namespace {

    constexpr double PI = 3.14159265358979323846;

    // ---- stored value <-> floats ----

    void toFloats(float v, float* out){ out[0] = v; }
    void toFloats(double v, float* out){ out[0] = (float)v; }
    void toFloats(int v, float* out){ out[0] = (float)v; }
    void toFloats(unsigned int v, float* out){ out[0] = (float)v; }
    void toFloats(bool v, float* out){ out[0] = v ? 1.0f : 0.0f; }
    void toFloats(const Vector2& v, float* out){ out[0] = v.x; out[1] = v.y; }
    void toFloats(const Vector3& v, float* out){ out[0] = v.x; out[1] = v.y; out[2] = v.z; }
    void toFloats(const Vector4& v, float* out){ out[0] = v.x; out[1] = v.y; out[2] = v.z; out[3] = v.w; }

    void fromFloats(const float* in, float& v){ v = in[0]; }
    void fromFloats(const float* in, double& v){ v = in[0]; }
    void fromFloats(const float* in, int& v){ v = (int)std::lround(in[0]); }
    void fromFloats(const float* in, unsigned int& v){ v = in[0] <= 0.0f ? 0u : (unsigned int)std::lround(in[0]); }
    void fromFloats(const float* in, bool& v){ v = in[0] > 0.5f; }
    void fromFloats(const float* in, Vector2& v){ v = Vector2(in[0], in[1]); }
    void fromFloats(const float* in, Vector3& v){ v = Vector3(in[0], in[1], in[2]); }
    void fromFloats(const float* in, Vector4& v){ v = Vector4(in[0], in[1], in[2], in[3]); }

    bool finite(const float* in, size_t width){
        for (size_t i = 0; i < width; i++){
            if (!std::isfinite(in[i])) return false;
        }
        return true;
    }

    template<typename C>
    bool hasComponent(EntityRegistry* registry, Entity entity, const PropertyParams&){
        return registry && registry->findComponent<C>(entity) != nullptr;
    }

    bool always(EntityRegistry* registry, Entity entity, const PropertyParams&){
        return registry && registry->isEntityCreated(entity);
    }

    template<typename C>
    using DirtyFn = void (*)(Scene*, Entity, C&);

    // a plain field of component C; Dirty (optional) sets the flags the Lua setter sets
    template<typename C, typename T, T C::*Field>
    bool readField(Scene* scene, Entity entity, const PropertyParams&, float* out){
        C* component = scene ? scene->findComponent<C>(entity) : nullptr;
        if (!component) return false;
        toFloats(component->*Field, out);
        return true;
    }

    template<typename C, typename T, T C::*Field, DirtyFn<C> Dirty = nullptr>
    bool writeField(Scene* scene, Entity entity, const PropertyParams&, const float* in){
        C* component = scene ? scene->findComponent<C>(entity) : nullptr;
        if (!component) return false;
        T value;
        fromFloats(in, value);
        component->*Field = value;
        // (a function pointer template argument: a null one compares at compile time; GCC's
        // -Waddress warns on the non-null instantiations)
        if constexpr (!std::is_same_v<std::integral_constant<DirtyFn<C>, Dirty>, std::integral_constant<DirtyFn<C>, nullptr>>) Dirty(scene, entity, *component);
        return true;
    }

    // ---- dirty flags (the engine's own setters) ----

    void layoutSizesDirty(Scene*, Entity, RectTransformComponent& layout){
        layout.needUpdateSizes = true;
        if (layout.usingAnchors) layout.needUpdateAnchorOffsets = true;
    }
    // RectTransform:setAnchorPoint* / setAnchorOffset*: the preset stops applying
    void layoutAnchorsDirty(Scene*, Entity, RectTransformComponent& layout){
        layout.anchorPreset = AnchorPreset::NONE;
        layout.usingAnchors = true;
    }
    void layoutPivotDirty(Scene* scene, Entity entity, RectTransformComponent&){
        if (Transform* transform = scene->findComponent<Transform>(entity)) transform->needUpdate = true;
    }
    void shadowCameraDirty(Scene*, Entity, LightComponent& light){ light.needUpdateShadowCamera = true; }
    void cameraDirty(Scene*, Entity, CameraComponent& camera){ camera.needUpdate = true; }
    void audioDirty(Scene*, Entity, AudioSourceComponent& audio){ audio.needUpdate = true; }
    void buttonDirty(Scene*, Entity, ButtonComponent& button){ button.needUpdateButton = true; }
    void progressbarDirty(Scene*, Entity, ProgressbarComponent& bar){ bar.needUpdateProgressbar = true; }

    // the anchor preset and usingAnchors an anchor write changes (put back when a preview ends)
    bool snapshotLayoutAnchors(Scene* scene, Entity entity, const PropertyParams&, float* out){
        RectTransformComponent* layout = scene ? scene->findComponent<RectTransformComponent>(entity) : nullptr;
        if (!layout) return false;
        out[0] = (float)(int)layout->anchorPreset;
        out[1] = layout->usingAnchors ? 1.0f : 0.0f;
        out[2] = out[3] = 0.0f;
        return true;
    }
    bool restoreLayoutAnchors(Scene* scene, Entity entity, const PropertyParams&, const float* in){
        RectTransformComponent* layout = scene ? scene->findComponent<RectTransformComponent>(entity) : nullptr;
        if (!layout) return false;
        layout->anchorPreset = (AnchorPreset)(int)std::lround(in[0]);
        layout->usingAnchors = in[1] > 0.5f;
        return true;
    }

    // Light:setIntensity: a light turned on from 0 needs its shadow camera
    bool writeLightIntensity(Scene* scene, Entity entity, const PropertyParams&, const float* in){
        LightComponent* light = scene ? scene->findComponent<LightComponent>(entity) : nullptr;
        if (!light) return false;
        if (in[0] > 0.0f && light->intensity == 0.0f) light->needUpdateShadowCamera = true;
        light->intensity = in[0];
        return true;
    }

    // ---- GameObject active (deferred: Scene::setEntityActive in the flush) ----

    bool readActive(Scene* scene, Entity entity, const PropertyParams&, float* out){
        if (!scene || !scene->isEntityCreated(entity)) return false;
        out[0] = scene->isEntityActive(entity) ? 1.0f : 0.0f;
        return true;
    }
    bool writeActive(Scene* scene, Entity entity, const PropertyParams&, const float* in){
        if (!scene || !scene->isEntityCreated(entity)) return false;
        const bool active = in[0] > 0.5f;
        if (scene->isEntityActive(entity) != active) scene->setEntityActive(entity, active);
        return true;
    }

    // ---- the enabled checkbox ----

    template<typename C>
    bool readEnabled(Scene* scene, Entity entity, const PropertyParams&, float* out){
        C* component = scene ? scene->findComponent<C>(entity) : nullptr;
        if (!component) return false;
        out[0] = component->enabled ? 1.0f : 0.0f;
        return true;
    }
    // what each component's Lua setter does besides the flag (audited against every
    // setEnabled / Lua `enabled` setter: the others only set the flag)
    template<typename C>
    void setEnabledAsTheSetter(Scene*, Entity, C& component, bool enabled){
        component.enabled = enabled;
    }
    // TrailRenderer::setEnabled: disabled, the trail is cleared now (no streak when re-enabled)
    template<>
    void setEnabledAsTheSetter<TrailRendererComponent>(Scene*, Entity, TrailRendererComponent& trail, bool enabled){
        if (!enabled) trail.clearPoints();
        trail.enabled = enabled;
    }
    // Joint2D::setEnabled: a joint broken with DISABLE and enabled again is a fresh joint
    template<>
    void setEnabledAsTheSetter<Joint2DComponent>(Scene*, Entity, Joint2DComponent& joint, bool enabled){
        if (enabled && !joint.enabled && joint.broken){
            joint.broken = false;
            joint.breakArmed = true;
            joint.needUpdateJoint = true;
        }
        joint.enabled = enabled;
    }
    // Lattice::setEnabled: the deformation is rebuilt (revision)
    template<>
    void setEnabledAsTheSetter<LatticeComponent>(Scene*, Entity, LatticeComponent& lattice, bool enabled){
        if (lattice.enabled != enabled){
            lattice.enabled = enabled;
            lattice.revision++;
        }
    }
    // TextEffect::setEnabled: the effect is applied again (on, or the plain text back)
    template<>
    void setEnabledAsTheSetter<TextEffectComponent>(Scene*, Entity, TextEffectComponent& effect, bool enabled){
        effect.enabled = enabled;
        effect.needApply = true;
    }
    // Rigidbody2D::setEnabled: the loaded body leaves or joins the world now (an unloaded one
    // is loaded by the physics step, which applies the flag)
#ifdef JASPERENGINE_PHYSICS_2D
    template<>
    void setEnabledAsTheSetter<Rigidbody2DComponent>(Scene* scene, Entity entity, Rigidbody2DComponent& body, bool enabled){
        body.enabled = enabled;
        if (!b2Body_IsValid(body.body)) return;
        if (enabled && scene->isEntityActiveInHierarchy(entity)) b2Body_Enable(body.body);
        else b2Body_Disable(body.body);
    }
#endif

    template<typename C>
    bool writeEnabled(Scene* scene, Entity entity, const PropertyParams&, const float* in){
        C* component = scene ? scene->findComponent<C>(entity) : nullptr;
        if (!component) return false;
        setEnabledAsTheSetter<C>(scene, entity, *component, in[0] > 0.5f);
        return true;
    }

    // ---- the canvas group rows (CanvasGroupTracks.h) ----

    template<bool (*Read)(Scene*, Entity, float*)>
    bool readSimple(Scene* scene, Entity entity, const PropertyParams&, float* out){ return Read(scene, entity, out); }
    template<bool (*Write)(Scene*, Entity, const float*)>
    bool writeSimple(Scene* scene, Entity entity, const PropertyParams&, const float* in){ return Write(scene, entity, in); }

    // ---- mesh submeshes and materials ----

    MeshComponent* meshOf(Scene* scene, Entity entity, const PropertyParams& params){
        MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
        if (!mesh || params.submesh < 0 || (unsigned int)params.submesh >= std::max(1u, mesh->numSubmeshes)){
            return nullptr;
        }
        return mesh;
    }

    bool hasSubmesh(EntityRegistry* registry, Entity entity, const PropertyParams& params){
        MeshComponent* mesh = registry ? registry->findComponent<MeshComponent>(entity) : nullptr;
        return mesh && params.submesh >= 0 && (unsigned int)params.submesh < std::max(1u, mesh->numSubmeshes);
    }

    template<typename T, T Material::*Field>
    bool readMaterial(Scene* scene, Entity entity, const PropertyParams& params, float* out){
        MeshComponent* mesh = meshOf(scene, entity, params);
        if (!mesh) return false;
        toFloats(mesh->submeshes[params.submesh].material.*Field, out);
        return true;
    }
    template<typename T, T Material::*Field>
    bool writeMaterial(Scene* scene, Entity entity, const PropertyParams& params, const float* in){
        MeshComponent* mesh = meshOf(scene, entity, params);
        if (!mesh) return false;
        T value;
        fromFloats(in, value);
        mesh->submeshes[params.submesh].material.*Field = value;
        return true;
    }

    // offset x y, scale x y (Unity's _MainTex_ST is scale, offset)
    bool readTextureRect(Scene* scene, Entity entity, const PropertyParams& params, float* out){
        MeshComponent* mesh = meshOf(scene, entity, params);
        if (!mesh) return false;
        const Rect& rect = mesh->submeshes[params.submesh].textureRect;
        out[0] = rect.getX(); out[1] = rect.getY(); out[2] = rect.getWidth(); out[3] = rect.getHeight();
        return true;
    }
    bool writeTextureRect(Scene* scene, Entity entity, const PropertyParams& params, const float* in){
        MeshComponent* mesh = meshOf(scene, entity, params);
        if (!mesh) return false;
        mesh->submeshes[params.submesh].textureRect = Rect(in[0], in[1], in[2], in[3]);
        return true;
    }

    // the shader a submesh's material parameters belong to: its material's, else the mesh's fork
    std::string submeshShader(MeshComponent& mesh, int submesh){
        const std::string& own = mesh.submeshes[submesh].material.shader;
        return own.empty() ? mesh.customShader.path : own;
    }

    const ShaderManifestProperty* manifestProperty(const std::string& shader, const std::string& member){
        if (shader.empty() || member.empty()) return nullptr;
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
        return manifest ? manifest->findProperty(member) : nullptr;
    }

    bool hasShaderParam(EntityRegistry* registry, Entity entity, const PropertyParams& params){
        if (!hasSubmesh(registry, entity, params) || params.member.empty()) return false;
        MeshComponent& mesh = *registry->findComponent<MeshComponent>(entity);
        const std::string shader = submeshShader(mesh, params.submesh);
        std::shared_ptr<const ShaderManifest> manifest = shader.empty() ? nullptr : ShaderManifests::get(shader);
        // a manifest lists its members; without one any member is accepted
        if (manifest && manifest->present) return manifest->findProperty(params.member) != nullptr;
        return true;
    }
    bool readShaderParam(Scene* scene, Entity entity, const PropertyParams& params, float* out){
        MeshComponent* mesh = meshOf(scene, entity, params);
        if (!mesh) return false;
        const Material& material = mesh->submeshes[params.submesh].material;
        toFloats(ShaderLibraryValues::getParam(material.shaderParams, submeshShader(*mesh, params.submesh), params.member), out);
        return true;
    }
    bool writeShaderParam(Scene* scene, Entity entity, const PropertyParams& params, const float* in){
        MeshComponent* mesh = meshOf(scene, entity, params);
        if (!mesh) return false;
        if (ShaderUniforms::set(mesh->submeshes[params.submesh].material.shaderParams, params.member, Vector4(in[0], in[1], in[2], in[3]))){
            mesh->needUpdateShaderUniforms = true;
        }
        return true;
    }

    // a component's custom uniform block member (UI, lines, points, sky)
    template<typename C>
    bool hasShaderUniform(EntityRegistry* registry, Entity entity, const PropertyParams& params){
        C* component = registry ? registry->findComponent<C>(entity) : nullptr;
        if (!component || params.member.empty() || ShaderUniforms::isReserved(params.member)) return false;
        std::shared_ptr<const ShaderManifest> manifest = component->customShader.empty() ? nullptr : ShaderManifests::get(component->customShader);
        if (manifest && manifest->present) return manifest->findProperty(params.member) != nullptr;
        return true;
    }
    template<typename C>
    bool readShaderUniform(Scene* scene, Entity entity, const PropertyParams& params, float* out){
        C* component = scene ? scene->findComponent<C>(entity) : nullptr;
        if (!component) return false;
        toFloats(ShaderLibraryValues::getParam(component->shaderUniforms, component->customShader, params.member), out);
        return true;
    }
    template<typename C>
    bool writeShaderUniform(Scene* scene, Entity entity, const PropertyParams& params, const float* in){
        C* component = scene ? scene->findComponent<C>(entity) : nullptr;
        if (!component) return false;
        if (ShaderUniforms::set(component->shaderUniforms, params.member, Vector4(in[0], in[1], in[2], in[3]))){
            component->needUpdateShaderUniforms = true;
        }
        return true;
    }

    // ---- sprite frame (Unity's m_Sprite swap): Sprite::setFrame(int) ----

    // the frame whose rect the sprite shows (-1: none of them); a sheet sprite's frame is its index
    // into the sheet (S44 R50: no rect comparison)
    bool readSpriteFrame(Scene* scene, Entity entity, const PropertyParams&, float* out){
        SpriteComponent* sprite = scene ? scene->findComponent<SpriteComponent>(entity) : nullptr;
        MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
        if (!sprite || !mesh) return false;
        if (!sprite->sheet.empty()){
            out[0] = (float)sprite->frame;
            return true;
        }
        out[0] = -1.0f;
        const Rect shown = sprite->needUpdateFrameRect ? sprite->pendingFrameRect : MeshSystem::spriteSourceRect(*sprite, *mesh);
        const Texture& texture = mesh->submeshes[0].material.baseColorTexture;
        float textureW = (float)texture.getWidth(), textureH = (float)texture.getHeight();
        MeshSystem::spriteSourceSize(*sprite, *mesh, textureW, textureH);
        for (unsigned int id = 0; id < sprite->numFramesRect; id++){
            Rect rect = sprite->framesRect[id].rect;
            if (!shown.isNormalized() || rect.isNormalized()){
                // compare like with like
            }else if (textureW > 0 && textureH > 0){
                rect = Rect(rect.getX() / textureW, rect.getY() / textureH, rect.getWidth() / textureW, rect.getHeight() / textureH);
            }
            const float eps = 1e-4f;
            if (std::fabs(rect.getX() - shown.getX()) < eps && std::fabs(rect.getY() - shown.getY()) < eps &&
                std::fabs(std::fabs(rect.getWidth()) - std::fabs(shown.getWidth())) < eps &&
                std::fabs(std::fabs(rect.getHeight()) - std::fabs(shown.getHeight())) < eps){
                out[0] = (float)id;
                break;
            }
        }
        return true;
    }
    // an index outside the frames is skipped (the binding warns once)
    bool writeSpriteFrame(Scene* scene, Entity entity, const PropertyParams&, const float* in){
        SpriteComponent* sprite = scene ? scene->findComponent<SpriteComponent>(entity) : nullptr;
        MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
        if (!sprite || !mesh) return false;
        const long id = std::lround(in[0]);
        if (!sprite->sheet.empty()){
            // before the sheet loads any index is kept (the sprite clamps it once it knows the count)
            if (id < 0 || (sprite->sheetData && id >= (long)sprite->sheetData->frames.size())) return false;
            sprite->frame = (int)id;
            return true;
        }
        if (id < 0 || (unsigned long)id >= sprite->numFramesRect) return false;
        MeshSystem::setSpriteFrameRect(*mesh, *sprite, sprite->framesRect[(size_t)id].rect);
        return true;
    }
    bool hasSprite(EntityRegistry* registry, Entity entity, const PropertyParams&){
        return registry && registry->findComponent<SpriteComponent>(entity) && registry->findComponent<MeshComponent>(entity);
    }
    // the texture rect a frame write replaces (a sheet sprite: its frame index, the rest unused)
    bool snapshotSpriteRect(Scene* scene, Entity entity, const PropertyParams&, float* out){
        MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
        if (!mesh) return false;
        if (SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity); sprite && !sprite->sheet.empty()){
            out[0] = (float)sprite->frame;
            out[1] = out[2] = out[3] = 0.0f;
            return true;
        }
        // a legacy sprite drawn from an atlas page: its source's rect (S45 R8)
        SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
        const Rect rect = sprite ? MeshSystem::spriteSourceRect(*sprite, *mesh) : mesh->submeshes[0].textureRect;
        out[0] = rect.getX(); out[1] = rect.getY(); out[2] = rect.getWidth(); out[3] = rect.getHeight();
        return true;
    }
    bool restoreSpriteRect(Scene* scene, Entity entity, const PropertyParams&, const float* in){
        MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
        SpriteComponent* sprite = scene ? scene->findComponent<SpriteComponent>(entity) : nullptr;
        if (!mesh || !sprite) return false;
        if (!sprite->sheet.empty()){
            sprite->frame = std::max(0, (int)std::lround(in[0]));
            return true;
        }
        MeshSystem::setSpriteFrameRect(*mesh, *sprite, Rect(in[0], in[1], in[2], in[3]));
        return true;
    }

    // ---- the Title Art Renderer (S81 R40) ----

    // a written Live Effect value: the system reads it at its next update; a keyed sweep keeps
    // sweepAuto off for the frame (R34: the track writes after the system)
    void titleArtSweepKeyed(Scene*, Entity, TitleArtRendererComponent& title){
        title.sweepPositionKeyed = true;
    }

    // S85 R27: a written animationTime seeks the base animation (or, with driveAnimation, drives it)
    void titleArtAnimationTimeKeyed(Scene*, Entity, TitleArtRendererComponent& title){
        title.animationTimeKeyed = true;
    }

    // the document's Exposed parameter of an "overrides.<name>" row, null when it has none
    const titleart::TitleArtExposedParam* titleArtExposed(EntityRegistry* registry, Entity entity, const std::string& name){
        TitleArtRendererComponent* title = registry && registry->isComponentRegistered<TitleArtRendererComponent>()
                                           ? registry->findComponent<TitleArtRendererComponent>(entity) : nullptr;
        if (!title) return nullptr;
        TitleArtSystem::parameterNames(registry, entity);
        for (const titleart::TitleArtExposedParam& p : TitleArtSystem::instanceOf(*title).exposed)
            if (p.name == name) return &p;
        return nullptr;
    }

    // R40: number, color and bool parameters are keyable (text, font, image and color schemes
    // change through clip events)
    bool hasTitleArtOverride(EntityRegistry* registry, Entity entity, const PropertyParams& params){
        const titleart::TitleArtExposedParam* p = titleArtExposed(registry, entity, params.member);
        return p && (p->type == "number" || p->type == "color" || p->type == "bool");
    }
    bool readTitleArtOverride(Scene* scene, Entity entity, const PropertyParams& params, float* out){
        TitleArtValue value;
        std::string error;
        if (!scene || !TitleArtSystem::getParameter(scene, entity, params.member, value, error)) return false;
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        if (value.kind == TitleArtValue::Kind::Number) out[0] = (float)value.number;
        else if (value.kind == TitleArtValue::Kind::Bool) out[0] = value.boolean ? 1.0f : 0.0f;
        else if (value.kind == TitleArtValue::Kind::Color){ out[0] = value.color.x; out[1] = value.color.y; out[2] = value.color.z; }
        return true;
    }
    bool writeTitleArtOverride(Scene* scene, Entity entity, const PropertyParams& params, const float* in){
        const titleart::TitleArtExposedParam* p = titleArtExposed(scene, entity, params.member);
        if (!p) return false;
        TitleArtValue value;
        if (p->type == "number") value = TitleArtValue::fromNumber(in[0]);
        else if (p->type == "bool") value = TitleArtValue::fromBool(in[0] > 0.5f);
        else if (p->type == "color") value = TitleArtValue::fromColor(Vector4(in[0], in[1], in[2], 1.0f));
        else return false;
        // an unchanged value does nothing (setParameter compares; no render)
        std::string error;
        return TitleArtSystem::setParameter(scene, entity, params.member, value, error);
    }

    // ---- the registry ----

    using Row = AnimatableProperty;
    constexpr PropertyKind B = PropertyKind::Bool;
    constexpr PropertyKind I = PropertyKind::Int;
    constexpr PropertyKind F = PropertyKind::Float;
    constexpr PropertyKind V2 = PropertyKind::Vector2;
    constexpr PropertyKind V4 = PropertyKind::Vector4;
    constexpr PropertyKind C3 = PropertyKind::Color3;
    constexpr PropertyKind C4 = PropertyKind::Color4;
    constexpr PropertyDisplay ID = PropertyDisplay::Identity;
    constexpr PropertyDisplay SRGB = PropertyDisplay::SRGB;

    template<typename C>
    Row enabledRow(const char* component){
        return {component, "enabled", "Enabled", "m_Enabled", B, 1, true, false, ID, false,
                &hasComponent<C>, &readEnabled<C>, &writeEnabled<C>, nullptr, nullptr, true};
    }

    template<typename C, typename T, T C::*Field, DirtyFn<C> Dirty = nullptr>
    Row fieldRow(const char* component, const char* property, const char* label, const char* unity, PropertyKind kind,
                 PropertyDisplay display = ID, bool interpolateInt = false){
        return {component, property, label, unity, kind, anim::kindWidth(kind), anim::kindIsDiscrete(kind) && !interpolateInt,
                interpolateInt, display, false, &hasComponent<C>, &readField<C, T, Field>, &writeField<C, T, Field, Dirty>, nullptr, nullptr, true};
    }

    template<typename C, typename T, T C::*Field>
    Row anchorRow(const char* property, const char* label, const char* unity, PropertyKind kind, bool interpolateInt = false){
        Row row = fieldRow<C, T, Field, &layoutAnchorsDirty>("recttransform", property, label, unity, kind, ID, interpolateInt);
        row.snapshot = &snapshotLayoutAnchors;
        row.restore = &restoreLayoutAnchors;
        return row;
    }

    template<typename T, T Material::*Field>
    Row materialRow(const char* property, const char* label, const char* unity, PropertyKind kind, PropertyDisplay display = ID){
        return {"mesh", property, label, unity, kind, anim::kindWidth(kind), false, false, display, false,
                &hasSubmesh, &readMaterial<T, Field>, &writeMaterial<T, Field>, nullptr, nullptr, true};
    }

    template<typename C>
    Row shaderUniformRow(const char* component){
        return {component, "shaderUniforms.<member>", "Shader Uniform", "", PropertyKind::None, 0, false, false, ID, false,
                &hasShaderUniform<C>, &readShaderUniform<C>, &writeShaderUniform<C>, nullptr, nullptr};
    }

    std::vector<Row> buildRows(){
        std::vector<Row> rows;
        rows.push_back({"gameobject", "active", "Active", "m_IsActive", B, 1, true, false, ID, true, &always, &readActive, &writeActive, nullptr, nullptr});

        // UI graphics (Image, Text, Button graphic, any UIComponent): Unity m_Color, TMP m_fontColor
        rows.push_back(fieldRow<UIComponent, Vector4, &UIComponent::color>("ui", "color", "Color", "m_Color", C4, SRGB));
        rows.push_back(enabledRow<UIComponent>("ui"));
        rows.push_back(shaderUniformRow<UIComponent>("ui"));
        // a Button writes ui.color from its state colours: these are what to animate there
        rows.push_back(fieldRow<ButtonComponent, Vector4, &ButtonComponent::colorNormal, &buttonDirty>("button", "colorNormal", "Color Normal", "m_Colors.m_NormalColor", C4, SRGB));
        rows.push_back(fieldRow<ButtonComponent, Vector4, &ButtonComponent::colorHovered, &buttonDirty>("button", "colorHovered", "Color Hovered", "m_Colors.m_HighlightedColor", C4, SRGB));
        rows.push_back(fieldRow<ButtonComponent, Vector4, &ButtonComponent::colorPressed, &buttonDirty>("button", "colorPressed", "Color Pressed", "m_Colors.m_PressedColor", C4, SRGB));
        rows.push_back(fieldRow<ButtonComponent, Vector4, &ButtonComponent::colorDisabled, &buttonDirty>("button", "colorDisabled", "Color Disabled", "m_Colors.m_DisabledColor", C4, SRGB));
        rows.push_back(fieldRow<ButtonComponent, Vector4, &ButtonComponent::colorSelected, &buttonDirty>("button", "colorSelected", "Color Selected", "m_Colors.m_SelectedColor", C4, SRGB));
        rows.push_back(fieldRow<ButtonComponent, bool, &ButtonComponent::disabled, &buttonDirty>("button", "disabled", "Disabled", "m_Interactable", B));

        // Rect Transform (the RectTransform Lua setters)
        rows.push_back(fieldRow<RectTransformComponent, Vector2, &RectTransformComponent::positionOffset>("recttransform", "positionOffset", "Anchored Position", "m_AnchoredPosition (y negated: layout space is y-down)", V2));
        rows.push_back(fieldRow<RectTransformComponent, unsigned int, &RectTransformComponent::width, &layoutSizesDirty>("recttransform", "width", "Width", "m_SizeDelta.x", I, ID, true));
        rows.push_back(fieldRow<RectTransformComponent, unsigned int, &RectTransformComponent::height, &layoutSizesDirty>("recttransform", "height", "Height", "m_SizeDelta.y", I, ID, true));
        rows.push_back(fieldRow<RectTransformComponent, Vector2, &RectTransformComponent::pivot, &layoutPivotDirty>("recttransform", "pivot", "Pivot", "m_Pivot (y: 1 - y)", V2));
        rows.push_back(anchorRow<RectTransformComponent, float, &RectTransformComponent::anchorPointLeft>("anchorPointLeft", "Anchor Left", "m_AnchorMin.x", F));
        rows.push_back(anchorRow<RectTransformComponent, float, &RectTransformComponent::anchorPointTop>("anchorPointTop", "Anchor Top", "m_AnchorMax.y (1 - y)", F));
        rows.push_back(anchorRow<RectTransformComponent, float, &RectTransformComponent::anchorPointRight>("anchorPointRight", "Anchor Right", "m_AnchorMax.x", F));
        rows.push_back(anchorRow<RectTransformComponent, float, &RectTransformComponent::anchorPointBottom>("anchorPointBottom", "Anchor Bottom", "m_AnchorMin.y (1 - y)", F));
        rows.push_back(anchorRow<RectTransformComponent, int, &RectTransformComponent::anchorOffsetLeft>("anchorOffsetLeft", "Offset Left", "m_OffsetMin.x", I, true));
        rows.push_back(anchorRow<RectTransformComponent, int, &RectTransformComponent::anchorOffsetTop>("anchorOffsetTop", "Offset Top", "m_OffsetMax.y (negated)", I, true));
        rows.push_back(anchorRow<RectTransformComponent, int, &RectTransformComponent::anchorOffsetRight>("anchorOffsetRight", "Offset Right", "m_OffsetMax.x", I, true));
        rows.push_back(anchorRow<RectTransformComponent, int, &RectTransformComponent::anchorOffsetBottom>("anchorOffsetBottom", "Offset Bottom", "m_OffsetMin.y (negated)", I, true));

        // Canvas Group (CanvasGroupTracks.h)
        size_t groupCount = 0;
        const anim::CanvasGroupTrack* group = anim::canvasGroupTracks(groupCount);
        static const char* const groupLabels[] = {"Alpha", "Interactable", "Blocks Raycasts", "Ignore Parent Groups", "Enabled"};
        for (size_t i = 0; i < groupCount; i++){
            Row row = {"canvasgroup", group[i].property, i < 5 ? groupLabels[i] : group[i].property, group[i].unityName,
                       group[i].isBool ? B : F, 1, group[i].isBool, false, ID, false, &hasComponent<CanvasGroupComponent>, nullptr, nullptr, nullptr, nullptr, true};
            rows.push_back(row);
        }

        rows.push_back(fieldRow<ProgressbarComponent, float, &ProgressbarComponent::value, &progressbarDirty>("progressbar", "value", "Value", "m_Value", F));

        // meshes, sprites, models' mesh nodes, tilemaps: per submesh material values (Sprite.color
        // is submeshes[0].material.baseColorFactor)
        rows.push_back(materialRow<Vector4, &Material::baseColorFactor>("submeshes[N].material.baseColorFactor", "Base Color", "material._Color", C4, SRGB));
        rows.push_back(materialRow<Vector3, &Material::emissiveFactor>("submeshes[N].material.emissiveFactor", "Emissive Factor", "material._EmissionColor", C3, SRGB));
        rows.push_back(materialRow<float, &Material::metallicFactor>("submeshes[N].material.metallicFactor", "Metallic Factor", "material._Metallic", F));
        rows.push_back(materialRow<float, &Material::roughnessFactor>("submeshes[N].material.roughnessFactor", "Roughness Factor", "material._Glossiness", F));
        rows.push_back(materialRow<float, &Material::alphaCutoff>("submeshes[N].material.alphaCutoff", "Alpha Cutoff", "material._Cutoff", F));
        rows.push_back({"mesh", "submeshes[N].textureRect", "Texture Rect", "material._MainTex_ST", V4, 4, false, false, ID, false,
                        &hasSubmesh, &readTextureRect, &writeTextureRect, nullptr, nullptr, true});
        rows.push_back({"mesh", "submeshes[N].material.shaderParams.<member>", "Shader Param", "material.<_Property>", PropertyKind::None, 0, false, false, ID, false,
                        &hasShaderParam, &readShaderParam, &writeShaderParam, nullptr, nullptr});
        rows.push_back(enabledRow<MeshComponent>("mesh"));

        rows.push_back({"sprite", "frame", "Frame", "m_Sprite", PropertyKind::Frame, 1, true, false, ID, false,
                        &hasSprite, &readSpriteFrame, &writeSpriteFrame, &snapshotSpriteRect, &restoreSpriteRect});
        rows.push_back(enabledRow<SpriteComponent>("sprite"));

        // lights (the Light Lua setters: range and cones update the shadow camera)
        rows.push_back(fieldRow<LightComponent, Vector3, &LightComponent::color>("light", "color", "Color", "m_Color", C3, SRGB));
        rows.push_back({"light", "intensity", "Intensity", "m_Intensity", F, 1, false, false, ID, false,
                        &hasComponent<LightComponent>, &readField<LightComponent, float, &LightComponent::intensity>, &writeLightIntensity, nullptr, nullptr, true});
        rows.push_back(fieldRow<LightComponent, float, &LightComponent::range, &shadowCameraDirty>("light", "range", "Range", "m_Range", F));
        rows.push_back(fieldRow<LightComponent, float, &LightComponent::innerConeCos, &shadowCameraDirty>("light", "innerConeCos", "Inner Cone", "m_InnerSpotAngle", F, PropertyDisplay::ConeDegrees));
        rows.push_back(fieldRow<LightComponent, float, &LightComponent::outerConeCos, &shadowCameraDirty>("light", "outerConeCos", "Outer Cone", "m_SpotAngle", F, PropertyDisplay::ConeDegrees));
        rows.push_back(enabledRow<LightComponent>("light"));

        rows.push_back(fieldRow<Light2DComponent, Vector3, &Light2DComponent::color>("light2d", "color", "Color", "m_Color", C3, SRGB));
        rows.push_back(fieldRow<Light2DComponent, float, &Light2DComponent::intensity>("light2d", "intensity", "Intensity", "m_Intensity", F));
        rows.push_back(fieldRow<Light2DComponent, float, &Light2DComponent::range>("light2d", "range", "Range", "m_PointLightOuterRadius", F));
        rows.push_back(fieldRow<Light2DComponent, float, &Light2DComponent::falloff>("light2d", "falloff", "Falloff", "m_FalloffIntensity", F));
        rows.push_back(fieldRow<Light2DComponent, float, &Light2DComponent::height>("light2d", "height", "Height", "", F));
        rows.push_back(enabledRow<Light2DComponent>("light2d"));

        // cameras (needUpdate, as the Camera Lua setters)
        rows.push_back(fieldRow<CameraComponent, float, &CameraComponent::yfov, &cameraDirty>("camera", "yfov", "Y FOV", "field of view", F));
        rows.push_back(fieldRow<CameraComponent, float, &CameraComponent::nearClip, &cameraDirty>("camera", "near", "Near", "near clip plane", F));
        rows.push_back(fieldRow<CameraComponent, float, &CameraComponent::farClip, &cameraDirty>("camera", "far", "Far", "far clip plane", F));
        rows.push_back(fieldRow<CameraComponent, float, &CameraComponent::leftClip, &cameraDirty>("camera", "left", "Left", "orthographic size", F));
        rows.push_back(fieldRow<CameraComponent, float, &CameraComponent::rightClip, &cameraDirty>("camera", "right", "Right", "orthographic size", F));
        rows.push_back(fieldRow<CameraComponent, float, &CameraComponent::bottomClip, &cameraDirty>("camera", "bottom", "Bottom", "orthographic size", F));
        rows.push_back(fieldRow<CameraComponent, float, &CameraComponent::topClip, &cameraDirty>("camera", "top", "Top", "orthographic size", F));
        rows.push_back(enabledRow<CameraComponent>("camera"));

        // audio (applied to the playing voice by AudioSystem, needUpdate as the Lua setters)
        rows.push_back(fieldRow<AudioSourceComponent, double, &AudioSourceComponent::volume, &audioDirty>("audiosource", "volume", "Volume", "m_Volume", F));
        rows.push_back(fieldRow<AudioSourceComponent, float, &AudioSourceComponent::speed, &audioDirty>("audiosource", "speed", "Speed", "m_Pitch", F));
        rows.push_back(fieldRow<AudioSourceComponent, float, &AudioSourceComponent::pan, &audioDirty>("audiosource", "pan", "Pan", "m_StereoPan", F));
        rows.push_back(enabledRow<AudioSourceComponent>("audiosource"));

        rows.push_back(fieldRow<FogComponent, Vector3, &FogComponent::color>("fog", "color", "Color", "m_FogColor", C3, SRGB));
        rows.push_back(fieldRow<FogComponent, float, &FogComponent::density>("fog", "density", "Density", "m_FogDensity", F));
        rows.push_back(fieldRow<FogComponent, float, &FogComponent::linearStart>("fog", "linearStart", "Linear Start", "m_LinearFogStart", F));
        rows.push_back(fieldRow<FogComponent, float, &FogComponent::linearEnd>("fog", "linearEnd", "Linear End", "m_LinearFogEnd", F));

        rows.push_back(fieldRow<SkyboxComponent, Vector4, &SkyboxComponent::color>("skybox", "color", "Color", "_Tint", C4, SRGB));
        rows.push_back(shaderUniformRow<SkyboxComponent>("skybox"));
        rows.push_back(shaderUniformRow<LineRendererComponent>("linerenderer"));
        rows.push_back(shaderUniformRow<PointsComponent>("points"));

        // the other component checkboxes (Unity m_Enabled; physics bodies apply on the next step)
        rows.push_back(enabledRow<ModelComponent>("model"));
        rows.push_back(enabledRow<TilemapComponent>("tilemap"));
        rows.push_back(enabledRow<TerrainComponent>("terrain"));
        rows.push_back(enabledRow<VoxelVolumeComponent>("voxelvolume"));
        rows.push_back(enabledRow<TerrainWorldComponent>("terrainworld"));
        rows.push_back(enabledRow<PointsComponent>("points"));
        rows.push_back(enabledRow<LineRendererComponent>("linerenderer"));
        rows.push_back(enabledRow<TrailRendererComponent>("trailrenderer"));
        rows.push_back(enabledRow<ParticleSystemComponent>("particlesystem"));
        rows.push_back(enabledRow<InstancedMeshComponent>("instancedmesh"));
        // (a loaded body leaves or joins the Box2D world in the write: the main thread's)
        Row rigidbody2dEnabled = enabledRow<Rigidbody2DComponent>("rigidbody2d");
        rigidbody2dEnabled.workerSafe = false;
        rows.push_back(rigidbody2dEnabled);
        rows.push_back(enabledRow<RigidbodyComponent>("rigidbody"));
        rows.push_back(enabledRow<Joint2DComponent>("joint2d"));
        rows.push_back(enabledRow<Joint3DComponent>("joint3d"));
        rows.push_back(enabledRow<RagdollComponent>("ragdoll"));
        rows.push_back(enabledRow<UIDocumentComponent>("uidocument"));
        rows.push_back(enabledRow<FluidSimulationComponent>("fluidsimulation"));
        rows.push_back(enabledRow<LatticeComponent>("lattice"));
        rows.push_back(enabledRow<LatticeModifierComponent>("latticemodifier"));
        rows.push_back(enabledRow<LatticeFollowerComponent>("latticefollower"));
        rows.push_back(enabledRow<VertexAnimatorComponent>("vertexanimator"));
        rows.push_back(enabledRow<ShadowCaster2DComponent>("shadowcaster2d"));
        rows.push_back(enabledRow<OnScreenStickComponent>("onscreenstick"));
        rows.push_back(enabledRow<OnScreenButtonComponent>("onscreenbutton"));
        rows.push_back(enabledRow<TouchAreaComponent>("toucharea"));
        rows.push_back(enabledRow<CameraShakerComponent>("camerashaker"));
        rows.push_back(enabledRow<CameraFollowComponent>("camerafollow"));
        rows.push_back(enabledRow<TextEffectComponent>("texteffect"));
        rows.push_back(enabledRow<VideoPlayerComponent>("videoplayer"));
        rows.push_back(enabledRow<SafeAreaComponent>("safearea"));
        rows.push_back(enabledRow<CapsuleShadowCasterComponent>("capsuleshadowcaster"));
        rows.push_back(enabledRow<PixelPerfectCameraComponent>("pixelperfectcamera"));
        rows.push_back(enabledRow<SortingGroupComponent>("sortinggroup"));
        rows.push_back(enabledRow<SpriteBatchComponent>("spritebatch"));
        // oe-port's components with the Inspector's enabled checkbox (their Lua setters, where they
        // have one, only set the flag)
        rows.push_back(enabledRow<ForceFieldComponent>("forcefield"));
        rows.push_back(enabledRow<CharacterController2DComponent>("charactercontroller2d"));
        rows.push_back(enabledRow<CharacterControllerComponent>("charactercontroller"));
        rows.push_back(enabledRow<WaterVolumeComponent>("watervolume"));
        rows.push_back(enabledRow<WaterExcluderComponent>("waterexcluder"));
        rows.push_back(enabledRow<WaterWakeComponent>("waterwake"));
        rows.push_back(enabledRow<BuoyancyComponent>("buoyancy"));
        rows.push_back(enabledRow<NavMeshSurfaceComponent>("navmeshsurface"));
        rows.push_back(enabledRow<NavMeshAgentComponent>("navmeshagent"));
        rows.push_back(enabledRow<NavMeshObstacleComponent>("navmeshobstacle"));
        rows.push_back(enabledRow<NavMeshLinkComponent>("navmeshlink"));
        rows.push_back(enabledRow<NavMeshModifierComponent>("navmeshmodifier"));
        rows.push_back(enabledRow<NavGridComponent>("navgrid"));
        rows.push_back(enabledRow<SplineComponent>("spline"));
        rows.push_back(enabledRow<SplineExtrudeComponent>("splineextrude"));
        rows.push_back(enabledRow<SplineInstantiateComponent>("splineinstantiate"));
        rows.push_back(enabledRow<SplineAnimateComponent>("splineanimate"));
        rows.push_back(enabledRow<AnimatorComponent>("animator"));

        // the Title Art Renderer (S81 R40): its live layers' values and its numeric Exposed
        // parameters; S85's titleart.animationTime and animationSpeed
        using TA = TitleArtRendererComponent;
        rows.push_back(enabledRow<TA>("titleart"));
        rows.push_back(fieldRow<TA, float, &TA::liveTime>("titleart", "liveTime", "Live Time", "", F));
        rows.push_back(fieldRow<TA, float, &TA::animationTime, &titleArtAnimationTimeKeyed>("titleart", "animationTime", "Animation Time", "", F));
        rows.push_back(fieldRow<TA, float, &TA::animationSpeed>("titleart", "animationSpeed", "Animation Speed", "", F));
        rows.push_back(fieldRow<TA, float, &TA::sweepPosition, &titleArtSweepKeyed>("titleart", "sweepPosition", "Sweep Position", "", F));
        rows.push_back(fieldRow<TA, float, &TA::sweepWidth>("titleart", "sweepWidth", "Sweep Width", "", F));
        rows.push_back(fieldRow<TA, float, &TA::sweepSoftness>("titleart", "sweepSoftness", "Sweep Softness", "", F));
        rows.push_back(fieldRow<TA, float, &TA::glowPulse>("titleart", "glowPulse", "Glow Pulse", "", F));
        rows.push_back(fieldRow<TA, float, &TA::tintAmount>("titleart", "tintAmount", "Tint Amount", "", F));
        rows.push_back(fieldRow<TA, float, &TA::glintAmount>("titleart", "glintAmount", "Glint Amount", "", F));
        rows.push_back(fieldRow<TA, Vector4, &TA::sweepColor>("titleart", "sweepColor", "Sweep Color", "", C4, SRGB));
        rows.push_back(fieldRow<TA, Vector4, &TA::glowColor>("titleart", "glowColor", "Glow Color", "", C4, SRGB));
        rows.push_back(fieldRow<TA, Vector4, &TA::glintColor>("titleart", "glintColor", "Glint Color", "", C4, SRGB));
        rows.push_back(fieldRow<TA, Vector4, &TA::tintColor>("titleart", "tintColor", "Tint Color", "", C4, SRGB));
        rows.push_back({"titleart", "overrides.<member>", "Parameter", "", PropertyKind::None, 0, false, false, ID, false,
                        &hasTitleArtOverride, &readTitleArtOverride, &writeTitleArtOverride, nullptr, nullptr});

        // the canvas group rows' functions (filled here: the table holds plain pointers)
        for (Row& row : rows){
            if (std::strcmp(row.component, "canvasgroup") != 0) continue;
            for (size_t i = 0; i < groupCount; i++){
                if (std::strcmp(row.property, group[i].property) != 0) continue;
                // the five rows of canvasGroupTracks, by index
                static bool (*const reads[])(Scene*, Entity, const PropertyParams&, float*) = {
                    &readSimple<&anim::readCanvasGroupAlpha>, &readSimple<&anim::readCanvasGroupInteractable>,
                    &readSimple<&anim::readCanvasGroupBlocksRaycasts>, &readSimple<&anim::readCanvasGroupIgnoreParentGroups>,
                    &readSimple<&anim::readCanvasGroupEnabled>};
                static bool (*const writes[])(Scene*, Entity, const PropertyParams&, const float*) = {
                    &writeSimple<&anim::writeCanvasGroupAlpha>, &writeSimple<&anim::writeCanvasGroupInteractable>,
                    &writeSimple<&anim::writeCanvasGroupBlocksRaycasts>, &writeSimple<&anim::writeCanvasGroupIgnoreParentGroups>,
                    &writeSimple<&anim::writeCanvasGroupEnabled>};
                if (i < 5){
                    row.read = reads[i];
                    row.write = writes[i];
                }
            }
        }
        return rows;
    }

    // ---- names ----

    // "submeshes[3].material.baseColorFactor" -> 3 and the pattern "submeshes[N].material.baseColorFactor"
    bool submeshPattern(const std::string& property, int& index, std::string& pattern){
        static const std::string prefix = "submeshes[";
        if (property.compare(0, prefix.size(), prefix) != 0) return false;
        const size_t close = property.find(']', prefix.size());
        if (close == std::string::npos || close == prefix.size() || close - prefix.size() > 3) return false;
        int value = 0;
        for (size_t i = prefix.size(); i < close; i++){
            if (property[i] < '0' || property[i] > '9') return false;
            value = value * 10 + (property[i] - '0');
        }
        index = value;
        pattern = prefix + "N" + property.substr(close);
        return true;
    }

    // "...shaderParams.<member>" / "shaderUniforms.<member>": the member and the pattern
    bool memberPattern(const std::string& property, std::string& member, std::string& pattern){
        for (const char* collection : {"shaderParams.", "shaderUniforms.", "overrides."}){
            const size_t at = property.find(collection);
            if (at == std::string::npos) continue;
            if (at != 0 && property[at - 1] != '.') continue;
            const size_t start = at + std::strlen(collection);
            if (start >= property.size()) return false;
            member = property.substr(start);
            for (char c : member){
                if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
            }
            pattern = property.substr(0, start) + "<member>";
            return true;
        }
        return false;
    }

    size_t editDistance(const std::string& a, const std::string& b){
        std::vector<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++) row[j] = j;
        for (size_t i = 1; i <= a.size(); i++){
            size_t previous = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); j++){
                const size_t current = row[j];
                const bool same = std::tolower((unsigned char)a[i - 1]) == std::tolower((unsigned char)b[j - 1]);
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, previous + (same ? 0 : 1)});
                previous = current;
            }
        }
        return row[b.size()];
    }

    bool isAnimatableScriptType(ScriptPropertyType type){
        switch (type){
            case ScriptPropertyType::Bool:
            case ScriptPropertyType::Int:
            case ScriptPropertyType::Float:
            case ScriptPropertyType::Vector2:
            case ScriptPropertyType::Vector3:
            case ScriptPropertyType::Vector4:
            case ScriptPropertyType::Color3:
            case ScriptPropertyType::Color4:
                return true;
            default:
                return false;
        }
    }

    PropertyKind kindOfScriptType(ScriptPropertyType type){
        switch (type){
            case ScriptPropertyType::Bool: return PropertyKind::Bool;
            case ScriptPropertyType::Int: return PropertyKind::Int;
            case ScriptPropertyType::Float: return PropertyKind::Float;
            case ScriptPropertyType::Vector2: return PropertyKind::Vector2;
            case ScriptPropertyType::Vector3: return PropertyKind::Vector3;
            case ScriptPropertyType::Vector4: return PropertyKind::Vector4;
            case ScriptPropertyType::Color3: return PropertyKind::Color3;
            case ScriptPropertyType::Color4: return PropertyKind::Color4;
            default: return PropertyKind::None;
        }
    }

    ScriptPropertyType scriptTypeOfKind(PropertyKind kind){
        switch (kind){
            case PropertyKind::Bool: return ScriptPropertyType::Bool;
            case PropertyKind::Int:
            case PropertyKind::Frame: return ScriptPropertyType::Int;
            case PropertyKind::Vector2: return ScriptPropertyType::Vector2;
            case PropertyKind::Vector3: return ScriptPropertyType::Vector3;
            case PropertyKind::Vector4: return ScriptPropertyType::Vector4;
            case PropertyKind::Color3: return ScriptPropertyType::Color3;
            case PropertyKind::Color4: return ScriptPropertyType::Color4;
            default: return ScriptPropertyType::Float;
        }
    }

    // the kind of a value of `width` floats with no declaration (shader members, undeclared keys)
    PropertyKind kindOfWidth(size_t width){
        switch (width){
            case 2: return PropertyKind::Vector2;
            case 3: return PropertyKind::Vector3;
            case 4: return PropertyKind::Vector4;
            default: return PropertyKind::Float;
        }
    }

    const ScriptEntry* findScriptEntry(EntityRegistry* registry, Entity entity, const std::string& className, size_t* index = nullptr){
        ScriptComponent* scripts = registry ? registry->findComponent<ScriptComponent>(entity) : nullptr;
        if (!scripts) return nullptr;
        // the first enabled one, else the first (spec decision 7)
        size_t first = SIZE_MAX;
        for (size_t i = 0; i < scripts->scripts.size(); i++){
            const ScriptEntry& entry = scripts->scripts[i];
            if (entry.type != ScriptType::LUA || entry.className != className) continue;
            if (entry.enabled){
                if (index) *index = i;
                return &entry;
            }
            if (first == SIZE_MAX) first = i;
        }
        if (first == SIZE_MAX) return nullptr;
        if (index) *index = first;
        return &scripts->scripts[first];
    }

    // the script a binding picked (its index, while it holds that class); a binding made
    // without one (scriptIndex -1) picks as bindProperty does
    size_t boundScriptIndex(const anim::PropertyBinding& binding){
        return binding.scriptIndex >= 0 ? (size_t)binding.scriptIndex : SIZE_MAX;
    }
    // the script at `index` when it is a `className` Lua script
    const ScriptEntry* scriptAt(EntityRegistry* registry, Entity entity, const std::string& className, size_t index){
        ScriptComponent* scripts = registry ? registry->findComponent<ScriptComponent>(entity) : nullptr;
        if (!scripts || index >= scripts->scripts.size()) return nullptr;
        const ScriptEntry& entry = scripts->scripts[index];
        if (entry.type != ScriptType::LUA || entry.className != className) return nullptr;
        return &entry;
    }
    // `index` gets the script's index (a binding made without one picks as bindProperty does)
    const ScriptEntry* boundScriptEntry(EntityRegistry* registry, Entity entity, const std::string& className, const anim::PropertyBinding& binding,
                                        size_t* index = nullptr){
        if (binding.scriptIndex < 0) return findScriptEntry(registry, entity, className, index);
        if (index) *index = (size_t)binding.scriptIndex;
        return scriptAt(registry, entity, className, (size_t)binding.scriptIndex);
    }

    // ---- interned keys ----

    struct KeyTable{
        std::deque<anim::PropertyKeyInfo> infos;    // id - 1
        std::unordered_map<std::string, uint32_t> ids;
    };
    KeyTable& keyTable(){
        static KeyTable table;
        return table;
    }

    // Color::sRGBToLinear / linearTosRGB per component (the Inspector's widgets), odd-symmetric
    // so an ease overshooting below 0 does not give NaN
    float srgbToLinear(float v){
        const float m = Color::sRGBToLinear(Vector3(std::fabs(v), 0.0f, 0.0f)).x;
        return v < 0.0f ? -m : m;
    }
    float linearToSrgb(float v){
        const float m = Color::linearTosRGB(Vector3(std::fabs(v), 0.0f, 0.0f)).x;
        return v < 0.0f ? -m : m;
    }

}

// ---- registry ----

const std::vector<AnimatableProperty>& anim::animatableProperties(){
    static const std::vector<AnimatableProperty> rows = buildRows();
    return rows;
}

const AnimatableProperty* anim::findAnimatableProperty(const std::string& component, const std::string& property){
    std::string pattern = property;
    int index = 0;
    std::string submeshed;
    if (submeshPattern(property, index, submeshed)) pattern = submeshed;
    std::string member;
    std::string membered;
    if (memberPattern(pattern, member, membered)) pattern = membered;
    for (const AnimatableProperty& row : animatableProperties()){
        if (component == row.component && pattern == row.property) return &row;
    }
    return nullptr;
}

uint8_t anim::kindWidth(PropertyKind kind){
    switch (kind){
        case PropertyKind::None: return 0;
        case PropertyKind::Bool:
        case PropertyKind::Int:
        case PropertyKind::Frame:
        case PropertyKind::Float: return 1;
        case PropertyKind::Vector2: return 2;
        case PropertyKind::Vector3:
        case PropertyKind::Color3: return 3;
        case PropertyKind::Vector4:
        case PropertyKind::Color4: return 4;
    }
    return 0;
}

bool anim::kindIsDiscrete(PropertyKind kind){
    return kind == PropertyKind::Bool || kind == PropertyKind::Int || kind == PropertyKind::Frame;
}

const char* anim::kindName(PropertyKind kind){
    switch (kind){
        case PropertyKind::None: return "none";
        case PropertyKind::Bool: return "bool";
        case PropertyKind::Int: return "int";
        case PropertyKind::Frame: return "frame";
        case PropertyKind::Float: return "float";
        case PropertyKind::Vector2: return "vector2";
        case PropertyKind::Vector3: return "vector3";
        case PropertyKind::Vector4: return "vector4";
        case PropertyKind::Color3: return "color3";
        case PropertyKind::Color4: return "color4";
    }
    return "none";
}

int anim::swizzleIndex(PropertyKind kind, char letter){
    const size_t width = kindWidth(kind);
    int index = -1;
    if (kind == PropertyKind::Color3 || kind == PropertyKind::Color4){
        const char* letters = "rgba";
        const char* at = std::strchr(letters, letter);
        if (letter != 0 && at) index = (int)(at - letters);
    }else if (kind == PropertyKind::Vector2 || kind == PropertyKind::Vector3 || kind == PropertyKind::Vector4){
        const char* letters = "xyzw";
        const char* at = std::strchr(letters, letter);
        if (letter != 0 && at) index = (int)(at - letters);
    }
    return index >= 0 && (size_t)index < width ? index : -1;
}

bool anim::parsePropertyRef(const std::string& component, const std::string& property, PropertyRef& out){
    out = PropertyRef();
    if (component.compare(0, 7, "script:") == 0){
        out.script = true;
        out.params.scriptClass = component.substr(7);
        std::string field = property;
        const size_t dot = field.find('.');
        if (dot != std::string::npos){
            // a swizzle of a vector / colour field: checked against its declared type at bind
            const std::string suffix = field.substr(dot + 1);
            field = field.substr(0, dot);
            static const char* const vectorLetters = "xyzw";
            static const char* const colorLetters = "rgba";
            const char* at = suffix.size() == 1 ? std::strchr(vectorLetters, suffix[0]) : nullptr;
            const char* letters = vectorLetters;
            if (!at && suffix.size() == 1){
                at = std::strchr(colorLetters, suffix[0]);
                letters = colorLetters;
            }
            if (at && suffix[0] != 0){
                out.swizzle = (int)(at - letters);
            }else{
                out.error = "unknown animatable property '" + component + "." + property + "' (a script field takes one swizzle: .x .y .z .w or .r .g .b .a)";
                return false;
            }
        }
        bool identifier = !field.empty() && (std::isalpha((unsigned char)field[0]) || field[0] == '_');
        for (char c : field){
            if (!(std::isalnum((unsigned char)c) || c == '_')) identifier = false;
        }
        if (out.params.scriptClass.empty() || !identifier){
            out.error = "unknown animatable property '" + component + "." + property + "' (expected script:<Class> and a field name)";
            return false;
        }
        out.params.member = field;
        out.base = field;
        out.known = true;
        if (field == "enabled"){
            // the script's checkbox: ScriptRuntime::setScriptEnabled (onEnable / onDisable), last
            out.kind = PropertyKind::Bool;
            out.width = 1;
            out.discrete = true;
            out.deferred = true;
            if (out.swizzle >= 0){
                out.known = false;
                out.error = "unknown animatable property '" + component + "." + property + "' (enabled is a bool)";
                return false;
            }
        }else if (field == "isActiveAndEnabled" || field == "gameObject"){
            out.known = false;
            out.error = "unknown animatable property '" + component + "." + property + "' (" + field + " is read-only)";
            return false;
        }
        return true;
    }

    // the whole property and its swizzle
    const AnimatableProperty* row = findAnimatableProperty(component, property);
    std::string base = property;
    int swizzle = -1;
    if (!row){
        const size_t dot = property.rfind('.');
        if (dot != std::string::npos && dot + 2 == property.size()){
            const AnimatableProperty* whole = findAnimatableProperty(component, property.substr(0, dot));
            if (whole){
                swizzle = swizzleIndex(whole->kind, property[dot + 1]);
                if (swizzle >= 0){
                    row = whole;
                    base = property.substr(0, dot);
                }else if (whole->kind == PropertyKind::None){
                    // a shader member: its width is the bound one (checked at bind)
                    static const char* const vectorLetters = "xyzw";
                    static const char* const colorLetters = "rgba";
                    const char letter = property[dot + 1];
                    const char* at = letter ? std::strchr(vectorLetters, letter) : nullptr;
                    const char* letters = vectorLetters;
                    if (!at && letter){
                        at = std::strchr(colorLetters, letter);
                        letters = colorLetters;
                    }
                    if (at){
                        row = whole;
                        base = property.substr(0, dot);
                        swizzle = (int)(at - letters);
                    }
                }
            }
        }
    }
    if (!row){
        // did you mean: the component's rows nearest by name
        std::string best;
        size_t bestDistance = SIZE_MAX;
        bool componentKnown = false;
        for (const AnimatableProperty& candidate : animatableProperties()){
            if (component != candidate.component) continue;
            componentKnown = true;
            const size_t distance = editDistance(property, candidate.property);
            if (distance < bestDistance){
                bestDistance = distance;
                best = candidate.property;
            }
        }
        out.error = "unknown animatable property '" + component + "." + property + "'";
        if (componentKnown && !best.empty()){
            out.error += " (did you mean '" + best + "'?)";
        }else if (!componentKnown){
            // the Catalog key: lower case, no "Component" suffix ("Light" -> "light")
            std::string lowered;
            for (char c : component) lowered += (char)std::tolower((unsigned char)c);
            if (lowered.size() > 9 && lowered.compare(lowered.size() - 9, 9, "component") == 0) lowered.resize(lowered.size() - 9);
            bool lowerKnown = false;
            for (const AnimatableProperty& candidate : animatableProperties()){
                if (lowered == candidate.component) lowerKnown = true;
            }
            if (lowerKnown && lowered != component) out.error += " (components are Catalog keys: did you mean '" + lowered + "'?)";
            else out.error += " (no animatable properties on component '" + component + "')";
        }
        return false;
    }
    out.known = true;
    out.entry = row;
    out.base = base;
    out.swizzle = swizzle;
    out.kind = row->kind;
    out.width = row->width;
    out.discrete = row->discrete;
    out.interpolateInt = row->interpolateInt;
    out.display = row->display;
    out.deferred = row->deferred;
    std::string pattern;
    submeshPattern(base, out.params.submesh, pattern);
    std::string member;
    if (memberPattern(base, member, pattern)) out.params.member = member;
    return true;
}

void anim::keyToStored(PropertyKind kind, PropertyDisplay display, bool roundInt, float* values, size_t width){
    if (display == PropertyDisplay::SRGB){
        for (size_t i = 0; i < width && i < 3; i++) values[i] = srgbToLinear(values[i]);
    }else if (display == PropertyDisplay::ConeDegrees){
        for (size_t i = 0; i < width; i++) values[i] = (float)std::cos(values[i] * 0.5 * PI / 180.0);
    }
    if (kind == PropertyKind::Bool){
        for (size_t i = 0; i < width; i++) values[i] = values[i] > 0.5f ? 1.0f : 0.0f;
    }else if (roundInt && (kind == PropertyKind::Int || kind == PropertyKind::Frame)){
        for (size_t i = 0; i < width; i++) values[i] = (float)std::round(values[i]);
    }
}

void anim::storedToKey(PropertyKind kind, PropertyDisplay display, float* values, size_t width){
    (void)kind;
    if (display == PropertyDisplay::SRGB){
        for (size_t i = 0; i < width && i < 3; i++) values[i] = linearToSrgb(values[i]);
    }else if (display == PropertyDisplay::ConeDegrees){
        for (size_t i = 0; i < width; i++){
            const double c = std::min(1.0, std::max(-1.0, (double)values[i]));
            values[i] = (float)(std::acos(c) * 2.0 * 180.0 / PI);
        }
    }
}

bool anim::parseHexColor(const std::string& text, std::vector<float>& out){
    if (text.size() != 7 && text.size() != 9) return false;
    if (text[0] != '#') return false;
    out.clear();
    for (size_t i = 1; i + 1 < text.size(); i += 2){
        int value = 0;
        for (size_t j = i; j < i + 2; j++){
            const char c = (char)std::tolower((unsigned char)text[j]);
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = 10 + c - 'a';
            if (digit < 0) return false;
            value = value * 16 + digit;
        }
        out.push_back((float)value / 255.0f);
    }
    return true;
}

// ---- binding ----

anim::PropertyBinding anim::bindProperty(EntityRegistry* scene, Entity entity, const PropertyRef& ref, size_t keyWidth, int preferredScriptIndex){
    PropertyBinding binding;
    if (!ref.known){
        binding.status = PropertyBindStatus::UnknownProperty;
        binding.message = ref.error.empty() ? "unknown animatable property" : ref.error;
        return binding;
    }
    if (!scene || !scene->isEntityCreated(entity)){
        binding.status = PropertyBindStatus::MissingComponent;
        binding.message = "no GameObject";
        return binding;
    }
    binding.entry = ref.entry;
    binding.swizzle = (int8_t)ref.swizzle;
    binding.kind = ref.kind;
    binding.width = ref.width;
    binding.discrete = ref.discrete;
    binding.interpolateInt = ref.interpolateInt;
    binding.display = ref.display;
    binding.deferred = ref.deferred;

    if (ref.script){
        // the script the binding had keeps it while it is still one of the class (a rebind never
        // moves a track to another instance of it); else the first enabled one, else the first
        size_t scriptIndex = 0;
        const ScriptEntry* entry = preferredScriptIndex >= 0 ? scriptAt(scene, entity, ref.params.scriptClass, (size_t)preferredScriptIndex) : nullptr;
        if (entry) scriptIndex = (size_t)preferredScriptIndex;
        else entry = findScriptEntry(scene, entity, ref.params.scriptClass, &scriptIndex);
        if (!entry){
            binding.status = PropertyBindStatus::MissingComponent;
            binding.message = "no Lua script '" + ref.params.scriptClass + "' on the GameObject";
            return binding;
        }
        // one instance for the binding's life (two scripts of a class: always the same one)
        binding.scriptIndex = (int16_t)std::min<size_t>(scriptIndex, INT16_MAX);
        binding.scriptInstance = entry->instance;
        binding.scriptGeneration = entry->instanceGeneration;
        if (ref.params.member != "enabled"){
            const ScriptProperty* declared = nullptr;
            for (const ScriptProperty& property : entry->properties){
                if (property.name == ref.params.member) declared = &property;
            }
            if (!declared){
                binding.status = PropertyBindStatus::MissingComponent;
                binding.message = "script '" + ref.params.scriptClass + "' declares no property '" + ref.params.member + "'";
                return binding;
            }
            if (!isAnimatableScriptType(declared->type)){
                binding.status = PropertyBindStatus::UnknownProperty;
                binding.message = "script property '" + ref.params.member + "' is not animatable (bool, int, float, vector and color fields are)";
                return binding;
            }
            // the declared type wins (an Int rounds and steps, a Color keys sRGB)
            binding.kind = kindOfScriptType(declared->type);
            binding.width = kindWidth(binding.kind);
            binding.discrete = kindIsDiscrete(binding.kind);
            binding.display = (binding.kind == PropertyKind::Color3 || binding.kind == PropertyKind::Color4) ? PropertyDisplay::SRGB : PropertyDisplay::Identity;
            binding.deferred = false;
            if (binding.swizzle >= 0 && (size_t)binding.swizzle >= binding.width){
                binding.status = PropertyBindStatus::UnknownProperty;
                binding.message = "script property '" + ref.params.member + "' has no component " + std::to_string(binding.swizzle);
                return binding;
            }
            if (binding.swizzle < 0 && keyWidth != 0 && keyWidth != binding.width && !(keyWidth == 3 && binding.kind == PropertyKind::Color4)){
                binding.status = PropertyBindStatus::UnknownProperty;
                binding.message = "keys of " + std::to_string(keyWidth) + " values for the " + kindName(binding.kind) + " property '" + ref.params.member + "'";
                return binding;
            }
        }
    }else{
        const AnimatableProperty& row = *ref.entry;
        if (!row.has || !row.has(scene, entity, ref.params)){
            binding.status = PropertyBindStatus::MissingComponent;
            binding.message = ref.params.submesh >= 0 ? "no submesh " + std::to_string(ref.params.submesh) + " (or no mesh)" :
                              !ref.params.member.empty() ? "no shader member '" + ref.params.member + "'" :
                              "no '" + std::string(row.component) + "' component";
            return binding;
        }
        if (row.kind == PropertyKind::None && std::strcmp(row.component, "titleart") == 0){
            // an Exposed parameter: the document's type (R40: number Float, color Color3 sRGB, bool Bool)
            const titleart::TitleArtExposedParam* p = titleArtExposed(scene, entity, ref.params.member);
            binding.kind = p && p->type == "color" ? PropertyKind::Color3 : p && p->type == "bool" ? PropertyKind::Bool : PropertyKind::Float;
            binding.display = binding.kind == PropertyKind::Color3 ? PropertyDisplay::SRGB : PropertyDisplay::Identity;
            binding.width = kindWidth(binding.kind);
            binding.discrete = binding.kind == PropertyKind::Bool;
        }else if (row.kind == PropertyKind::None){
            // a shader member: the manifest's type, else the keys' width
            std::string shader;
            if (MeshComponent* mesh = (ref.params.submesh >= 0) ? scene->findComponent<MeshComponent>(entity) : nullptr){
                shader = submeshShader(*mesh, ref.params.submesh);
            }else if (std::strcmp(row.component, "ui") == 0){
                if (UIComponent* ui = scene->findComponent<UIComponent>(entity)) shader = ui->customShader;
            }else if (std::strcmp(row.component, "skybox") == 0){
                if (SkyboxComponent* sky = scene->findComponent<SkyboxComponent>(entity)) shader = sky->customShader;
            }else if (std::strcmp(row.component, "linerenderer") == 0){
                if (LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(entity)) shader = lines->customShader;
            }else if (std::strcmp(row.component, "points") == 0){
                if (PointsComponent* points = scene->findComponent<PointsComponent>(entity)) shader = points->customShader;
            }
            const ShaderManifestProperty* declared = manifestProperty(shader, ref.params.member);
            binding.width = 0;
            if (declared){
                switch (declared->kind){
                    case ShaderPropertyKind::FLOAT:
                    case ShaderPropertyKind::RANGE:
                    case ShaderPropertyKind::TOGGLE: binding.kind = PropertyKind::Float; break;
                    case ShaderPropertyKind::INT: binding.kind = PropertyKind::Float; break;
                    case ShaderPropertyKind::COLOR:
                    case ShaderPropertyKind::HDRCOLOR:
                        binding.kind = PropertyKind::Color4;
                        binding.display = PropertyDisplay::SRGB;
                        break;
                    case ShaderPropertyKind::VECTOR2: binding.kind = PropertyKind::Vector2; break;
                    case ShaderPropertyKind::VECTOR3: binding.kind = PropertyKind::Vector3; break;
                    case ShaderPropertyKind::VECTOR4: binding.kind = PropertyKind::Vector4; break;
                }
                binding.width = kindWidth(binding.kind);
                if (binding.swizzle < 0 && keyWidth > 0 && keyWidth < binding.width){
                    // fewer key values than the member has: the rest keep their value
                    binding.width = (uint8_t)std::max<size_t>(keyWidth, binding.width);
                }
            }else{
                binding.kind = kindOfWidth(binding.swizzle >= 0 ? 4 : keyWidth);
                binding.width = binding.swizzle >= 0 ? 4 : (uint8_t)std::max<size_t>(1, std::min<size_t>(4, keyWidth));
            }
            if (binding.swizzle >= 0 && (size_t)binding.swizzle >= binding.width){
                binding.status = PropertyBindStatus::UnknownProperty;
                binding.message = "shader member '" + ref.params.member + "' has no component " + std::to_string(binding.swizzle);
                return binding;
            }
            binding.discrete = false;
        }
    }
    binding.key = internPropertyKey(ref.script ? "script:" + ref.params.scriptClass : std::string(ref.entry->component), ref);
    binding.status = PropertyBindStatus::Bound;
    return binding;
}

uint32_t anim::internPropertyKey(const std::string& component, const PropertyRef& ref){
    KeyTable& table = keyTable();
    const std::string name = component + "|" + ref.base;
    auto found = table.ids.find(name);
    if (found != table.ids.end()) return found->second;
    PropertyKeyInfo info;
    info.component = component;
    info.property = ref.base;
    info.ref = ref;
    info.ref.swizzle = -1;
    table.infos.push_back(std::move(info));
    const uint32_t id = (uint32_t)table.infos.size();
    table.ids.emplace(name, id);
    return id;
}

const anim::PropertyKeyInfo* anim::propertyKeyInfo(uint32_t key){
    KeyTable& table = keyTable();
    if (key == 0 || key > table.infos.size()) return nullptr;
    return &table.infos[key - 1];
}

bool anim::scriptBindingStale(EntityRegistry* registry, Entity entity, const PropertyBinding& binding){
    if (binding.status != PropertyBindStatus::Bound || binding.scriptIndex < 0) return false;
    const PropertyKeyInfo* info = propertyKeyInfo(binding.key);
    if (!info || !info->ref.script) return false;
    const ScriptEntry* entry = boundScriptEntry(registry, entity, info->ref.params.scriptClass, binding);
    // the ref alone is not enough: a released one is reused by the next instance
    return !entry || entry->instance != binding.scriptInstance || entry->instanceGeneration != binding.scriptGeneration;
}

bool anim::readPropertyStored(Scene* scene, Entity entity, const PropertyBinding& binding, float* out){
    if (binding.status != PropertyBindStatus::Bound || !scene || !scene->isEntityCreated(entity)) return false;
    const PropertyKeyInfo* info = propertyKeyInfo(binding.key);
    if (!info) return false;
    float values[4] = {0, 0, 0, 0};
    bool ok = false;
    if (info->ref.script){
        if (info->ref.params.member == "enabled"){
            const ScriptEntry* entry = boundScriptEntry(scene, entity, info->ref.params.scriptClass, binding);
            if (!entry) return false;
            values[0] = entry->enabled ? 1.0f : 0.0f;
            ok = true;
        }else{
            ok = LuaBinding::getLuaScriptProperty(scene, entity, info->ref.params.scriptClass, info->ref.params.member,
                                                  scriptTypeOfKind(binding.kind), values, boundScriptIndex(binding));
        }
    }else if (binding.entry && binding.entry->read){
        ok = binding.entry->read(scene, entity, info->ref.params, values);
    }
    if (!ok) return false;
    for (size_t i = 0; i < binding.width && i < 4; i++) out[i] = values[i];
    return true;
}

bool anim::writePropertyStored(Scene* scene, Entity entity, const PropertyBinding& binding, const float* in){
    if (binding.status != PropertyBindStatus::Bound || !scene || !scene->isEntityCreated(entity)) return false;
    const PropertyKeyInfo* info = propertyKeyInfo(binding.key);
    if (!info || !finite(in, binding.width)) return false;
    float values[4] = {0, 0, 0, 0};
    if (binding.width < 4){
        // the components the value does not have keep theirs (a shader member written as a vec4)
        float current[4] = {0, 0, 0, 0};
        if (!info->ref.script && binding.entry && binding.entry->read && binding.entry->kind == PropertyKind::None){
            binding.entry->read(scene, entity, info->ref.params, current);
        }
        for (size_t i = 0; i < 4; i++) values[i] = current[i];
    }
    for (size_t i = 0; i < binding.width && i < 4; i++) values[i] = in[i];
    if (info->ref.script){
        if (info->ref.params.member == "enabled"){
            // (a binding without its index resolves it here: setScriptEnabled needs a real one)
            size_t index = 0;
            const ScriptEntry* entry = boundScriptEntry(scene, entity, info->ref.params.scriptClass, binding, &index);
            if (!entry) return false;
            const bool enabled = values[0] > 0.5f;
            if (entry->enabled != enabled) ScriptRuntime::setScriptEnabled(scene, entity, index, enabled);
            return true;
        }
        return LuaBinding::setLuaScriptProperty(scene, entity, info->ref.params.scriptClass, info->ref.params.member,
                                                scriptTypeOfKind(binding.kind), values, boundScriptIndex(binding));
    }
    return binding.entry && binding.entry->write && binding.entry->write(scene, entity, info->ref.params, values);
}

bool anim::readPropertyKeySpace(Scene* scene, Entity entity, const PropertyBinding& binding, float* out){
    if (!readPropertyStored(scene, entity, binding, out)) return false;
    storedToKey(binding.kind, binding.display, out, binding.width);
    return true;
}

bool anim::writePropertyKeySpace(Scene* scene, Entity entity, const PropertyBinding& binding, const float* in){
    float values[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < binding.width && i < 4; i++) values[i] = in[i];
    keyToStored(binding.kind, binding.display, binding.discrete || binding.interpolateInt, values, binding.width);
    return writePropertyStored(scene, entity, binding, values);
}

bool anim::snapshotProperty(Scene* scene, Entity entity, const PropertyBinding& binding, float* out){
    const PropertyKeyInfo* info = propertyKeyInfo(binding.key);
    if (!info || !binding.entry || !binding.entry->snapshot || !scene || !scene->isEntityCreated(entity)) return false;
    return binding.entry->snapshot(scene, entity, info->ref.params, out);
}

bool anim::restoreProperty(Scene* scene, Entity entity, const PropertyBinding& binding, const float* in){
    const PropertyKeyInfo* info = propertyKeyInfo(binding.key);
    if (!info || !binding.entry || !binding.entry->restore || !scene || !scene->isEntityCreated(entity)) return false;
    return binding.entry->restore(scene, entity, info->ref.params, in);
}

// ---- discovery ----

std::vector<anim::AnimatablePropertyInfo> anim::listAnimatableProperties(Scene* scene, Entity entity){
    std::vector<AnimatablePropertyInfo> out;
    if (!scene || !scene->isEntityCreated(entity)) return out;
    auto add = [&](const std::string& component, const std::string& property, const std::string& label, PropertyKind kind, bool discrete){
        AnimatablePropertyInfo info;
        info.component = component;
        info.property = property;
        info.label = label;
        info.kind = kind;
        info.discrete = discrete;
        out.push_back(std::move(info));
    };
    // the shader whose manifest lists a row's members
    auto members = [&](const std::string& shader, const std::string& prefix, const std::string& component, const std::string& labelPrefix){
        if (shader.empty()) return;
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
        if (!manifest || !manifest->present) return;
        for (const ShaderManifestProperty& declared : manifest->properties){
            PropertyKind kind = PropertyKind::Float;
            switch (declared.kind){
                case ShaderPropertyKind::COLOR:
                case ShaderPropertyKind::HDRCOLOR: kind = PropertyKind::Color4; break;
                case ShaderPropertyKind::VECTOR2: kind = PropertyKind::Vector2; break;
                case ShaderPropertyKind::VECTOR3: kind = PropertyKind::Vector3; break;
                case ShaderPropertyKind::VECTOR4: kind = PropertyKind::Vector4; break;
                default: break;
            }
            add(component, prefix + declared.member, labelPrefix + (declared.label.empty() ? declared.member : declared.label), kind, false);
        }
    };
    for (const AnimatableProperty& row : animatableProperties()){
        const std::string component = row.component;
        const std::string pattern = row.property;
        if (pattern.find("submeshes[N]") != std::string::npos){
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!mesh) continue;
            const unsigned int count = std::max(1u, mesh->numSubmeshes);
            for (unsigned int s = 0; s < count; s++){
                const std::string prefix = "submeshes[" + std::to_string(s) + "]";
                const std::string property = prefix + pattern.substr(std::strlen("submeshes[N]"));
                const std::string labelPrefix = "Material [" + std::to_string(s) + "] : ";
                if (row.kind == PropertyKind::None){
                    members(submeshShader(*mesh, (int)s), prefix + ".material.shaderParams.", component, labelPrefix);
                    continue;
                }
                PropertyParams params;
                params.submesh = (int)s;
                if (!row.has(scene, entity, params)) continue;
                add(component, property, (pattern.find("textureRect") != std::string::npos ? "Submesh [" + std::to_string(s) + "] : " : labelPrefix) + row.label,
                    row.kind, row.discrete);
            }
            continue;
        }
        if (row.kind == PropertyKind::None && component == "titleart"){
            // the title's numeric Exposed parameters (S81 R40)
            TitleArtRendererComponent* title = scene->findComponent<TitleArtRendererComponent>(entity);
            if (!title) continue;
            TitleArtSystem::parameterNames(scene, entity);
            for (const titleart::TitleArtExposedParam& p : TitleArtSystem::instanceOf(*title).exposed){
                if (p.type != "number" && p.type != "color" && p.type != "bool") continue;
                const PropertyKind kind = p.type == "color" ? PropertyKind::Color3 : p.type == "bool" ? PropertyKind::Bool : PropertyKind::Float;
                add(component, "overrides." + p.name, "Parameter : " + (p.label.empty() ? p.name : p.label), kind, kind == PropertyKind::Bool);
            }
            continue;
        }
        if (row.kind == PropertyKind::None){
            // a component's shader uniforms: its fork's manifest members
            std::string shader;
            if (component == "ui"){ if (UIComponent* c = scene->findComponent<UIComponent>(entity)) shader = c->customShader; else continue; }
            else if (component == "skybox"){ if (SkyboxComponent* c = scene->findComponent<SkyboxComponent>(entity)) shader = c->customShader; else continue; }
            else if (component == "linerenderer"){ if (LineRendererComponent* c = scene->findComponent<LineRendererComponent>(entity)) shader = c->customShader; else continue; }
            else if (component == "points"){ if (PointsComponent* c = scene->findComponent<PointsComponent>(entity)) shader = c->customShader; else continue; }
            members(shader, "shaderUniforms.", component, "Shader : ");
            continue;
        }
        if (!row.has || !row.has(scene, entity, PropertyParams())) continue;
        add(component, pattern, row.label, row.kind, row.discrete);
    }
    // Lua scripts: enabled and the declared animatable fields
    if (ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity)){
        std::vector<std::string> seen;
        for (const ScriptEntry& entry : scripts->scripts){
            if (entry.type != ScriptType::LUA || entry.className.empty()) continue;
            if (std::find(seen.begin(), seen.end(), entry.className) != seen.end()) continue;
            seen.push_back(entry.className);
            const std::string component = "script:" + entry.className;
            add(component, "enabled", "Enabled", PropertyKind::Bool, true);
            for (const ScriptProperty& property : entry.properties){
                if (!isAnimatableScriptType(property.type)) continue;
                const PropertyKind kind = kindOfScriptType(property.type);
                add(component, property.name, property.displayName.empty() ? property.name : property.displayName, kind, kindIsDiscrete(kind));
            }
        }
    }
    return out;
}
