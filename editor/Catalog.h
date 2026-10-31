// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include "Scene.h"
#include "SceneSettings.h"
#include "util/Color.h"
#include "shader/ShaderManifest.h"

namespace jasperengine::editor{
    enum UpdateFlags : uint64_t {
        UpdateFlags_None                = 0,
        UpdateFlags_Transform           = 1 << 0,
        UpdateFlags_Camera              = 1 << 1,
        UpdateFlags_Scene_Mesh_Reload   = 1 << 2,
        UpdateFlags_Mesh_Reload         = 1 << 3,
        UpdateFlags_Mesh_Texture        = 1 << 4,
        UpdateFlags_UI_Reload           = 1 << 5,
        UpdateFlags_UI_Texture          = 1 << 6,
        UpdateFlags_Image_Patches       = 1 << 7,
        UpdateFlags_Layout_Sizes        = 1 << 8,
        UpdateFlags_Layout_Anchors      = 1 << 9,
        UpdateFlags_Sprite              = 1 << 10,
        UpdateFlags_LightShadowMap      = 1 << 11,
        UpdateFlags_LightShadowCamera   = 1 << 12,
        UpdateFlags_Sky_Texture         = 1 << 13,
        UpdateFlags_Sky                 = 1 << 14,
        UpdateFlags_Text                = 1 << 15,
        UpdateFlags_Text_Atlas          = 1 << 16,
        UpdateFlags_Rigidbody2D              = 1 << 17,
        UpdateFlags_Rigidbody              = 1 << 18,
        UpdateFlags_Joint2D             = 1 << 19,
        UpdateFlags_Joint3D             = 1 << 20,
        UpdateFlags_Model               = 1 << 21,
        UpdateFlags_Tilemap             = 1 << 22,
        UpdateFlags_Instanced_Mesh      = 1 << 23,
        UpdateFlags_Points              = 1 << 24,
        UpdateFlags_Terrain             = 1 << 25,
        UpdateFlags_Terrain_Texture     = 1 << 26,
        UpdateFlags_AudioSource               = 1 << 27,
        UpdateFlags_Lines               = 1 << 28,
        UpdateFlags_Lines_Reload        = 1 << 29,
        UpdateFlags_Points_Texture      = 1 << 30,
        // Generic "reload the renderable's shader" flag, used by the per-component
        // customShader property of Mesh/UI/Points/Lines/Sky.
        UpdateFlags_Shader_Reload       = (uint64_t)1 << 31,
        UpdateFlags_Reflection_Probe_Capture  = (uint64_t)1 << 32,
        UpdateFlags_Reflection_Probe_Sampling = (uint64_t)1 << 33,
        // for changes outside the probe itself, like a mesh leaving the capture pass
        UpdateFlags_Reflection_Probe_Recapture_All = (uint64_t)1 << 34,
        UpdateFlags_Terrain_Foliage     = (uint64_t)1 << 35,
        // shaderUniforms of Mesh/UI/Points/Lines/Sky (no shader reload)
        UpdateFlags_Shader_Uniforms     = (uint64_t)1 << 36,
        // every mesh needs a new shader variant for the scene's environment (lights, fog, probes),
        // its resources unchanged: the loaded variants draw until the new ones are built
        // (RenderSystem::needReloadMeshVariants)
        UpdateFlags_Scene_Mesh_Variant  = (uint64_t)1 << 37,
        // a UIDocument's file changed: load it again
        UpdateFlags_UIDocument_Reload   = (uint64_t)1 << 38,
        // what a FluidSimulation's start depends on changed (seed, the start burst): restart it
        // (sizes need nothing, FluidSimulator recreates its targets itself)
        UpdateFlags_Fluid_Reset         = (uint64_t)1 << 39,
        // a lattice's offsets, resolution or settings, or a modifier's bindings changed: bumps
        // LatticeComponent::revision and lets an over-full modifier warn again
        UpdateFlags_Lattice             = (uint64_t)1 << 40,
        // a Pose Animator's animation path changed: load it again
        UpdateFlags_PoseAnimator_Reload = (uint64_t)1 << 41,
        // an Animation Player's clip list changed: the next update reloads and rebinds it
        UpdateFlags_AnimationPlayer_Reload = (uint64_t)1 << 42,
        // a Vertex Animator's animation changed: load it again (the fade is kept >= 0)
        UpdateFlags_VertexAnimator_Reload = (uint64_t)1 << 43,
        // a 2D joint's motor, spring, limit or break field: applied to the live joint, no rebuild
        UpdateFlags_Joint2D_Params      = (uint64_t)1 << 44,
        // a Rigidbody 2D's gravity scale, damping, mass...: applied to the live body, no rebuild
        UpdateFlags_Rigidbody2D_Params  = (uint64_t)1 << 45,
        // a script entry's enabled flag (or the list) changed: while the scene's scripts run,
        // onEnable / onDisable as due (ScriptRuntime::refreshEntity)
        UpdateFlags_Script_Enabled      = (uint64_t)1 << 46,
        // a Trail Renderer's ribbon settings (width, colour, joints, caps, texture layout): its
        // ribbon is built again (values clamped: widths and time >= 0, vertex counts 0..90)
        UpdateFlags_Trail               = (uint64_t)1 << 47,
        // a Rigidbody's type, damping, mass fields...: applied to the live body (a type switch,
        // the mass again), no rebuild
        UpdateFlags_Rigidbody_Params    = (uint64_t)1 << 48,
        // a 3D joint's motor, spring, limit, friction or break field: applied to the live joint
        UpdateFlags_Joint3D_Params      = (uint64_t)1 << 49,
        // a Ragdoll's root or animator: its parts are collected again
        UpdateFlags_Ragdoll             = (uint64_t)1 << 50,
        // handlers that touch the whole scene, must be masked out when applying all flags
        UpdateFlags_SceneWide = UpdateFlags_Scene_Mesh_Reload | UpdateFlags_Scene_Mesh_Variant | UpdateFlags_Reflection_Probe_Recapture_All
    };

    // the order of components here affects properties window
    enum class ComponentType : int {
        Transform,
        ScriptComponent,
        MeshComponent,
        LineRendererComponent,
        LightComponent,
        TerrainComponent,
        PointsComponent,
        SkyboxComponent,
        ModelComponent,
        UIComponent,
        RectTransformComponent,
        ActionComponent,
        TweenComponent,
        AlphaTweenComponent,
        AnimationComponent,
        AudioSourceComponent,
        Rigidbody2DComponent,
        RigidbodyComponent,
        BoneComponent,
        PrefabComponent,
        ButtonComponent,
        CameraComponent,
        ColorTweenComponent,
        FogComponent,
        ImageComponent,
        InstancedMeshComponent,
        Joint2DComponent,
        Joint3DComponent,
        RagdollComponent,
        KeyframeTracksComponent,
        MeshPolygonComponent,
        MorphTracksComponent,
        PanelComponent,
        ParticleSystemComponent,
        PolygonComponent,
        PositionTweenComponent,
        ProgressbarComponent,
        RotateTracksComponent,
        RotationTweenComponent,
        ScaleTweenComponent,
        ScaleTracksComponent,
        ScrollbarComponent,
        SpriteAnimationComponent,
        SpriteComponent,
        TextComponent,
        InputFieldComponent,
        TilemapComponent,
        TranslateTracksComponent,
        LayoutGroupComponent,
        MirrorComponent,
        Light2DComponent,
        ShadowCaster2DComponent,
        // Keep new component types at the end: their ordinal is used as the bit
        // position in Prefab's in-memory component override mask.
        ReflectionProbeComponent,
        UIDocumentComponent,
        FluidSimulationComponent,
        LatticeComponent,
        LatticeModifierComponent,
        LatticeFollowerComponent,
        LatticeTracksComponent,
        PoseAnimatorComponent,
        // Unity's legacy Animation component (animation clips spec 3.2)
        AnimationPlayerComponent,
        // baked vertex / bone animation played on the GPU (vertex animation spec 3.3)
        VertexAnimatorComponent,
        // Unity's Trail Renderer (lines and trails spec 6.4)
        TrailRendererComponent
    };

    // The last ComponentType: loops over every type (MCP lists, name lookups) run to it.
    // Move it with each new type appended above.
    constexpr ComponentType LastComponentType = ComponentType::TrailRendererComponent;
    constexpr int ComponentTypeCount = static_cast<int>(LastComponentType) + 1;

    enum class PropertyType{
        Bool,
        String,
        Float,
        Double,
        Vector2,
        Vector3,
        Vector4,
        Quat,
        Int,
        UInt,
        Material,
        Texture,
        Font,
        Enum,
        Ease,
        Custom,
        Entity,
        EntityReference,
        // Unity AnimationCurve / Gradient (math/AnimationCurve.h, math/Gradient.h)
        Curve,
        Gradient
    };

    struct PropertyData{
        PropertyType type;
        uint64_t updateFlags;
        void* def;
        void* ref;
    };

    class Catalog{
    private:

        static std::string removeComponentSuffix(std::string str);

    public:
        Catalog();

        static std::string getComponentName(ComponentType component, bool removeSuffix = false);
        // Unity-style name for the UI ("Sprite Renderer", "Rigidbody 2D", "Audio Source").
        // Display only: getComponentName stays the key of scenes, generated code and tools.
        static std::string getComponentDisplayName(ComponentType component);
        // Font Awesome icon of the component, for Inspector headers and the Add Component popup
        static const char* getComponentIcon(ComponentType component);
        // The component has the Unity enable checkbox (an "enabled" property)
        static bool hasEnabledProperty(ComponentType component);
        static ComponentId getComponentId(const EntityRegistry* registry, ComponentType compType);
        static ComponentType getComponentType(const std::string& componentName);
        static Signature componentTypeToSignature(const EntityRegistry* registry, ComponentType compType);

        static PropertyType scriptPropertyTypeToPropertyType(ScriptPropertyType scriptType);

        static std::map<std::string, PropertyData> getProperties(ComponentType component, void* compRef);

        static std::vector<ComponentType> findComponents(EntityRegistry* registry, Entity entity);
        static std::map<std::string, PropertyData> findEntityProperties(EntityRegistry* registry, Entity entity, ComponentType component);
        // False when the component certainly holds none of the properties a reference pass
        // looks at: Entity and EntityReference ones (entities) or Textures naming a
        // render-to-texture camera or a fluid (cameraTextures: "camera|<e>" and "fluid|<e>"
        // links, CameraTextureLink::parseLink). Such a pass can then skip building its
        // property map, the costly part for a thousand sprites. Knows Transform, Sprite
        // and Mesh; any other component answers true.
        static bool mayHoldReferences(EntityRegistry* registry, Entity entity, ComponentType component, bool entities, bool cameraTextures);

        static uint64_t getChangedUpdateFlags(ComponentType compType, void* oldComp, void* newComp);
        static uint64_t getComponentStructuralUpdateFlags(ComponentType compType);

        static void updateEntity(EntityRegistry* registry, Entity entity, uint64_t updateFlags);

        static void copyComponent(EntityRegistry* sourceRegistry, Entity sourceEntity,
                                EntityRegistry* targetRegistry, Entity targetEntity,
                                ComponentType compType);
        static void copyPropertyValue(EntityRegistry* sourceRegistry, Entity sourceEntity, 
                                    EntityRegistry* targetRegistry, Entity targetEntity, 
                                    ComponentType compType, const std::string& property);

        static PropertyData getProperty(EntityRegistry* registry, Entity entity, ComponentType component, std::string propertyName);

        // The same lookup without the not-found log, for callers a miss is expected on
        static PropertyData findProperty(EntityRegistry* registry, Entity entity, ComponentType component, std::string propertyName);

        // Submesh::overrideFields of a "submeshes[N].<field>" property, null for anything else,
        // with propertyFields set to the bits that property occupies. A command ORs them in after
        // writing the value, and puts the whole mask back on undo.
        static uint32_t* getSubmeshOverrideMask(EntityRegistry* registry, Entity entity, ComponentType component, const std::string& propertyName, uint32_t& propertyFields);

        // custom uniform blocks of a Mesh/UI/Points/Lines/Sky component's loaded shader, vertex first
        static std::vector<const CustomUniformBlock*> getShaderUniformBlocks(EntityRegistry* registry, Entity entity, ComponentType component);
        // declarations the engine cannot upload right: mixed int/float blocks, one name with two types
        static std::vector<std::string> getShaderUniformWarnings(const std::vector<const CustomUniformBlock*>& blocks);
        // the component's custom shader (own or scene default) failed and the built-in is rendering
        static bool isCustomShaderBuildFailed(Scene* scene, Entity entity, ComponentType component);
        // same for a mesh's depth fork
        static bool isCustomDepthShaderBuildFailed(Scene* scene, Entity entity);

        // ---- shader library values (docs/superpowers/specs/2026-10-10-shader-library-phase1-design.md)
        // One entry of a shader-library collection property, addressed by its key:
        // "submeshes[N].material.shaderParams.<member>", "submeshes[N].material.shaderTextures.<uniform>",
        // the same under "submeshes[N].material.extraPass.",
        // and a component's "shaderUniforms.<member>" / "shaderTextures.<uniform>". Gives the
        // collection property and the key; edits go through the collection property, so undo,
        // prefab overrides and model reloads see one value.
        static bool splitShaderEntryProperty(const std::string& propertyName, std::string& collection, std::string& key);
        // The shader whose manifest describes a property's shader-library values: for a submesh
        // material property ("submeshes[N].material...") the shader that submesh draws with (its
        // material's, the mesh's, the scene's), for its extra pass ("...material.extraPass...")
        // the pass's own shader, else the component's own or the scene's default
        static std::string getShaderReferenceForProperty(Scene* scene, Entity entity, ComponentType component, const std::string& propertyName);
        // The lighting modes a material may ask of a shader, its default first: a manifest's
        // @lighting list (none without the directive), else the built-in Lit's (and a fork's
        // without a manifest) pixel, vertex, mixed, none. AUTO is always allowed and draws the
        // default. Asks RenderSystem::resolveLighting / supportsLighting (the runtime's rules).
        static std::vector<MaterialLighting> getShaderLightingModes(const ShaderManifest* manifest);
        static bool supportsLighting(MaterialLighting mode, const ShaderManifest* manifest);
        // Whether a shader with this manifest reads the material's own Normal Texture (on the 2D
        // light path: @require normalmap, ShaderRequire_NormalMap) with the enabled features
        // (ShaderReference::getEnabledFeatures). False for the built-in shader and forks without
        // a manifest (null; they read it on every path) and for an absent manifest
        static bool shaderReadsMaterialNormalMap(const ShaderManifest* manifest, const std::vector<std::string>& features);
        // The update flags a change from one material value to another needs (a whole-material
        // assignment outside a property command: linked material files)
        static uint64_t getMaterialUpdateFlags(const Material& from, const Material& to);

        // ---- scene inputs (docs/superpowers/specs/2026-10-10-shader-library-phase3-design.md):
        // what a draw reads of the opaque scene, ShaderRequire_SceneDepth / _SceneColor bits.
        // The inspector (picker marks, info and error rows) and MCP (list_shaders cost_note,
        // get_entity / get_shader_uniforms scene_inputs) say the same through these.
        // "depth", "colour" or "depth and colour" ("" for none)
        static std::string getSceneInputsLabel(uint32_t inputs);
        // What reading them costs: brief, the extra work per camera pass ("a depth pre-pass and a
        // colour copy per camera"); else the whole explanation (when it is paid, what it costs,
        // the scene setting, mobile advice, the transparent-queue rule). "" for none
        static std::string getSceneInputsCost(uint32_t inputs, bool brief = false);
        // What some variant of a shader reads and with which features ("Reads the scene depth
        // (with SOFT) and colour"), "" for a shader that reads nothing of the scene
        static std::string getShaderSceneInputsSummary(const ShaderManifest& manifest);
        // The scene inputs a @feature or @keywords define (<GROUP>_<VALUE>) turns on through its
        // feature= rules, without what the shader reads anyway; whenOff: the ones its
        // "feature=!<DEFINE>" rules read while it is off
        static uint32_t getFeatureSceneInputs(const ShaderManifest& manifest, const std::string& define, bool whenOff = false);
        // What a variant of a shader reference reads with a shaderFeatures list (0 without a manifest)
        static uint32_t getShaderSceneInputs(const std::string& shader, const std::vector<std::string>& shaderFeatures);
        // What submesh `submesh`'s material reads in its current variant: its shader (its own,
        // else the mesh's or the scene's), or with extraPass its extra pass's shader
        static uint32_t getMaterialSceneInputs(Scene* scene, Entity entity, unsigned int submesh, bool extraPass);
        // What a UI/Points/Lines/Sky component's shader (own or the scene's) reads in its current variant;
        // for a mesh, what its shader reads in the variants of the submeshes drawing with it
        static uint32_t getComponentSceneInputs(Scene* scene, Entity entity, ComponentType component);
        // What this scene's last frame produced for its draws reading the scene, as ShaderRequire_
        // bits (RenderSystem::getSceneInputUsage): the scene settings' "In Use" row and
        // get_scene_settings' scene_inputs_in_use
        static uint32_t getSceneInputsInUse(Scene* scene);
        // A submesh whose shader or extra pass reads the scene while it draws in the opaque queue
        // (an error: RenderSystem::getSubmeshSceneInputs): what moves it to the transparent queue,
        // the inspector's fix button and MCP's fix. With the mesh's Auto Transparency on, Alpha
        // blending (submeshes[N].material.blendMode), when it does move it (not a shader forcing
        // another queue: NONE); with it off the mesh's Transparent decides the queue whatever the
        // blend, so turning it on (transparent). NONE too when it is not such a submesh.
        enum class SceneInputQueueFix { NONE, ALPHA_BLEND, MESH_TRANSPARENT };
        static SceneInputQueueFix getSceneInputQueueFix(Scene* scene, Entity entity, unsigned int submesh);
        // the property the fix sets ("submeshes[N].material.blendMode", "transparent"; "" for NONE)
        static std::string getSceneInputQueueFixProperty(SceneInputQueueFix fix, unsigned int submesh);
        // why the submesh draws in the opaque queue and what to do, one sentence for the inspector,
        // the checks and MCP (the cause the fix would remove, or the shader's state)
        static std::string getSceneInputQueueAdvice(SceneInputQueueFix fix, unsigned int submesh);

        template<typename T>
        static T* getPropertyRef(EntityRegistry* registry, Entity entity, ComponentType component, std::string propertyName){
            PropertyData property = Catalog::getProperty(registry, entity, component, propertyName);
            if (property.ref){
                return static_cast<T*>(property.ref);
            }

            return nullptr;
        }

        // Scene property "post_processing.<snake name>": a PostProcessingSettings field (bool,
        // int (Int and Enum fields: the enum's value), float, Vector3, std::string), null for
        // any other name
        static const PostProcessingSettings::Field* getPostProcessingField(const std::string& propertyName) {
            static const std::string prefix = "post_processing.";
            if (propertyName.compare(0, prefix.size(), prefix) != 0) return nullptr;
            return PostProcessingSettings::findField(propertyName.substr(prefix.size()));
        }

        template<typename T>
        static bool postProcessingFieldMatches(const PostProcessingSettings::Field& field) {
            using FT = PostProcessingSettings::FieldType;
            if constexpr (std::is_same_v<T, bool>) return field.type == FT::Bool;
            else if constexpr (std::is_same_v<T, int>) return field.type == FT::Int || field.type == FT::Enum;
            else if constexpr (std::is_same_v<T, float>) return field.type == FT::Float;
            else if constexpr (std::is_same_v<T, Vector3>) return field.type == FT::Vector3;
            else if constexpr (std::is_same_v<T, std::string>) return field.type == FT::String;
            else return false;
        }

        template<typename T>
        static T getSceneProperty(Scene* scene, const std::string& propertyName) {
            if (!scene) {
                // Return default values if scene is null
                if constexpr (std::is_same_v<T, Vector4>) return Vector4(0.0, 0.0, 0.0, 1.0);
                if constexpr (std::is_same_v<T, Vector3>) return Vector3(1.0, 1.0, 1.0);
                if constexpr (std::is_same_v<T, Vector2>) return Vector2(0.0, 0.0);
                if constexpr (std::is_same_v<T, bool>) return false;
                if constexpr (std::is_same_v<T, float>) return 0.0f;
                if constexpr (std::is_same_v<T, int>) return 0;
                if constexpr (std::is_same_v<T, LightState>) return LightState::AUTO;
                if constexpr (std::is_same_v<T, ShadowQuality>) return ShadowQuality::LOW;
                if constexpr (std::is_same_v<T, SceneColorScale>) return SceneColorScale::HALF;
                if constexpr (std::is_same_v<T, TextureFilter>) return TextureFilter::NEAREST;
                if constexpr (std::is_same_v<T, std::string>) return std::string();
                if constexpr (std::is_same_v<T, PostProcessingSettings>) return PostProcessingSettings();
                // Add other types as needed
            }

            if (const PostProcessingSettings::Field* field = getPostProcessingField(propertyName)) {
                if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int> || std::is_same_v<T, float> ||
                              std::is_same_v<T, Vector3> || std::is_same_v<T, std::string>) {
                    if (postProcessingFieldMatches<T>(*field))
                        return *(const T*)scene->getPostProcessing().getFieldRef(*field);
                }
            }
            else if (propertyName == "post_processing") {
                if constexpr (std::is_same_v<T, PostProcessingSettings>) {
                    return scene->getPostProcessing();
                }
            }
            else if (propertyName == "background_color") {
                if constexpr (std::is_same_v<T, Vector4>) {
                    return scene->getBackgroundColor();
                }
            }
            else if (propertyName == "shadows_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) {
                    return scene->getShadowQuality();
                }
            }
            else if (propertyName == "global_illumination_color") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    return scene->getGlobalIlluminationColor();
                }
            }
            else if (propertyName == "global_illumination_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getGlobalIlluminationIntensity();
                }
            }
            else if (propertyName == "max_lights_per_object") {
                if constexpr (std::is_same_v<T, int>) {
                    return scene->getMaxLightsPerObject();
                }
            }
            else if (propertyName == "ambient_light_2d_color") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    return scene->getAmbientLight2DColor();
                }
            }
            else if (propertyName == "ambient_light_2d_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getAmbientLight2DIntensity();
                }
            }
            else if (propertyName == "shadows_2d_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) {
                    return scene->getShadow2DQuality();
                }
            }
            else if (propertyName == "physics_gravity_2d") {
                if constexpr (std::is_same_v<T, Vector2>) {
                    return scene->getGravity2D();
                }
            }
            else if (propertyName == "physics_gravity_3d") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    return scene->getGravity3D();
                }
            }
            else if (propertyName == "physics_interpolation") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isPhysicsInterpolation();
                }
            }
            else if (propertyName == "physics_2d_sub_steps") {
                if constexpr (std::is_same_v<T, int>) {
                    return scene->getPhysics2DSubSteps();
                }
            }
            else if (propertyName == "light_state") {
                if constexpr (std::is_same_v<T, LightState>) {
                    return scene->getLightState();
                }
            }
            else if (propertyName == "mesh_lod_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isMeshLodEnabled();
                }
            }
            else if (propertyName == "mesh_lod_threshold") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getMeshLodThreshold();
                }
            }
            else if (propertyName == "depth_prepass_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isDepthPrepassEnabled();
                }
            }
            else if (propertyName == "scene_color_scale") {
                if constexpr (std::is_same_v<T, SceneColorScale>) {
                    return scene->getSceneColorScale();
                }
            }
            else if (propertyName == "ssao_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isSSAOEnabled();
                }
            }
            else if (propertyName == "ssao_radius") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSAORadius();
                }
            }
            else if (propertyName == "ssao_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSAOIntensity();
                }
            }
            else if (propertyName == "ssao_bias") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSAOBias();
                }
            }
            else if (propertyName == "ssao_debug") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isSSAODebug();
                }
            }
            else if (propertyName == "ssr_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isSSREnabled();
                }
            }
            else if (propertyName == "ssr_max_distance") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSRMaxDistance();
                }
            }
            else if (propertyName == "ssr_thickness") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSRThickness();
                }
            }
            else if (propertyName == "ssr_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSRIntensity();
                }
            }
            else if (propertyName == "ssr_blur") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSRBlur();
                }
            }
            else if (propertyName == "ssr_max_steps") {
                if constexpr (std::is_same_v<T, int>) {
                    return scene->getSSRMaxSteps();
                }
            }
            else if (propertyName == "ssr_debug_mode") {
                if constexpr (std::is_same_v<T, int>) {
                    return scene->getSSRDebugMode();
                }
            }
            else if (propertyName == "fixed_resolution_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isFixedResolutionEnabled();
                }
            }
            else if (propertyName == "fixed_resolution_width") {
                if constexpr (std::is_same_v<T, int>) {
                    return (int)scene->getFixedResolutionWidth();
                }
            }
            else if (propertyName == "fixed_resolution_height") {
                if constexpr (std::is_same_v<T, int>) {
                    return (int)scene->getFixedResolutionHeight();
                }
            }
            else if (propertyName == "fixed_resolution_filter") {
                if constexpr (std::is_same_v<T, TextureFilter>) {
                    return scene->getFixedResolutionFilter();
                }
            }
            else if (propertyName == "default_mesh_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    return scene->getDefaultMeshShader();
                }
            }
            else if (propertyName == "default_ui_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    return scene->getDefaultUIShader();
                }
            }
            else if (propertyName == "default_sky_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    return scene->getDefaultSkyShader();
                }
            }
            else if (propertyName == "default_points_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    return scene->getDefaultPointsShader();
                }
            }
            else if (propertyName == "default_lines_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    return scene->getDefaultLinesShader();
                }
            }
            else if (propertyName == "post_process") {
                if constexpr (std::is_same_v<T, std::vector<PostProcessPass>>) {
                    return scene->getPostProcessPasses();
                }
            }

            // Return default value if property not found
            if constexpr (std::is_same_v<T, Vector4>) return Vector4(0.0, 0.0, 0.0, 1.0);
            if constexpr (std::is_same_v<T, Vector3>) return Vector3(1.0, 1.0, 1.0);
            if constexpr (std::is_same_v<T, Vector2>) return Vector2(0.0, 0.0);
            if constexpr (std::is_same_v<T, bool>) return false;
            if constexpr (std::is_same_v<T, float>) return 0.0f;
            if constexpr (std::is_same_v<T, int>) return 0;
            if constexpr (std::is_same_v<T, LightState>) return LightState::AUTO;
                if constexpr (std::is_same_v<T, ShadowQuality>) return ShadowQuality::LOW;
            if constexpr (std::is_same_v<T, SceneColorScale>) return SceneColorScale::HALF;
            if constexpr (std::is_same_v<T, TextureFilter>) return TextureFilter::NEAREST;
            if constexpr (std::is_same_v<T, std::string>) return std::string();
            if constexpr (std::is_same_v<T, std::vector<PostProcessPass>>) return std::vector<PostProcessPass>();
            if constexpr (std::is_same_v<T, PostProcessingSettings>) return PostProcessingSettings();
            // Add other types as needed
        }

        // Factory default for a scene property, used by the Inspector panel's "reset to
        // default" arrow. Values come straight from a default-constructed SceneSettings — the
        // engine's single source of truth for scene defaults (Scene embeds one) — so they can
        // never drift from what a new scene gets. SceneSettings is plain data (no physics
        // runtime), so this is cheap; no caching needed. Values are returned in the same space
        // getSceneProperty exposes (GI/ambient colors are converted from stored linear to sRGB).
        template<typename T>
        static T getScenePropertyDefault(const std::string& propertyName) {
            static const SceneSettings d;

            if (const PostProcessingSettings::Field* field = getPostProcessingField(propertyName)) {
                if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int> || std::is_same_v<T, float> ||
                              std::is_same_v<T, Vector3> || std::is_same_v<T, std::string>) {
                    if (postProcessingFieldMatches<T>(*field))
                        return *(const T*)d.postProcessing.getFieldRef(*field);
                }
            }
            else if (propertyName == "post_processing") {
                if constexpr (std::is_same_v<T, PostProcessingSettings>) return d.postProcessing;
            }
            else if (propertyName == "background_color") {
                if constexpr (std::is_same_v<T, Vector4>) return d.backgroundColor;
            }
            else if (propertyName == "shadows_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) return d.shadowQuality;
            }
            else if (propertyName == "global_illumination_color") {
                if constexpr (std::is_same_v<T, Vector3>) return Color::linearTosRGB(d.globalIllumColor);
            }
            else if (propertyName == "global_illumination_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.globalIllumIntensity;
            }
            else if (propertyName == "max_lights_per_object") {
                if constexpr (std::is_same_v<T, int>) return d.maxLightsPerObject;
            }
            else if (propertyName == "ambient_light_2d_color") {
                if constexpr (std::is_same_v<T, Vector3>) return Color::linearTosRGB(d.ambientLight2DColor);
            }
            else if (propertyName == "ambient_light_2d_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.ambientLight2DIntensity;
            }
            else if (propertyName == "shadows_2d_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) return d.shadow2DQuality;
            }
            else if (propertyName == "physics_gravity_2d") {
                if constexpr (std::is_same_v<T, Vector2>) return d.gravity2D;
            }
            else if (propertyName == "physics_gravity_3d") {
                if constexpr (std::is_same_v<T, Vector3>) return d.gravity3D;
            }
            else if (propertyName == "physics_interpolation") {
                if constexpr (std::is_same_v<T, bool>) return d.physicsInterpolation;
            }
            else if (propertyName == "physics_2d_sub_steps") {
                if constexpr (std::is_same_v<T, int>) return d.physics2DSubSteps;
            }
            else if (propertyName == "light_state") {
                if constexpr (std::is_same_v<T, LightState>) return d.lightState;
            }
            else if (propertyName == "mesh_lod_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.meshLodEnabled;
            }
            else if (propertyName == "mesh_lod_threshold") {
                if constexpr (std::is_same_v<T, float>) return d.meshLodThreshold;
            }
            else if (propertyName == "depth_prepass_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.depthPrepassEnabled;
            }
            else if (propertyName == "scene_color_scale") {
                if constexpr (std::is_same_v<T, SceneColorScale>) return d.sceneColorScale;
            }
            else if (propertyName == "ssao_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.ssaoEnabled;
            }
            else if (propertyName == "ssao_radius") {
                if constexpr (std::is_same_v<T, float>) return d.ssaoRadius;
            }
            else if (propertyName == "ssao_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.ssaoIntensity;
            }
            else if (propertyName == "ssao_bias") {
                if constexpr (std::is_same_v<T, float>) return d.ssaoBias;
            }
            else if (propertyName == "ssao_debug") {
                if constexpr (std::is_same_v<T, bool>) return d.ssaoDebug;
            }
            else if (propertyName == "ssr_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.ssrEnabled;
            }
            else if (propertyName == "ssr_max_distance") {
                if constexpr (std::is_same_v<T, float>) return d.ssrMaxDistance;
            }
            else if (propertyName == "ssr_thickness") {
                if constexpr (std::is_same_v<T, float>) return d.ssrThickness;
            }
            else if (propertyName == "ssr_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.ssrIntensity;
            }
            else if (propertyName == "ssr_blur") {
                if constexpr (std::is_same_v<T, float>) return d.ssrBlur;
            }
            else if (propertyName == "ssr_max_steps") {
                if constexpr (std::is_same_v<T, int>) return d.ssrMaxSteps;
            }
            else if (propertyName == "ssr_debug_mode") {
                if constexpr (std::is_same_v<T, int>) return d.ssrDebugMode;
            }
            else if (propertyName == "fixed_resolution_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.fixedResolutionEnabled;
            }
            else if (propertyName == "fixed_resolution_width") {
                if constexpr (std::is_same_v<T, int>) return (int)d.fixedResolutionWidth;
            }
            else if (propertyName == "fixed_resolution_height") {
                if constexpr (std::is_same_v<T, int>) return (int)d.fixedResolutionHeight;
            }
            else if (propertyName == "fixed_resolution_filter") {
                if constexpr (std::is_same_v<T, TextureFilter>) return d.fixedResolutionFilter;
            }
            else if (propertyName == "default_mesh_shader") {
                if constexpr (std::is_same_v<T, std::string>) return d.defaultMeshShader;
            }
            else if (propertyName == "default_ui_shader") {
                if constexpr (std::is_same_v<T, std::string>) return d.defaultUIShader;
            }
            else if (propertyName == "default_sky_shader") {
                if constexpr (std::is_same_v<T, std::string>) return d.defaultSkyShader;
            }
            else if (propertyName == "default_points_shader") {
                if constexpr (std::is_same_v<T, std::string>) return d.defaultPointsShader;
            }
            else if (propertyName == "default_lines_shader") {
                if constexpr (std::is_same_v<T, std::string>) return d.defaultLinesShader;
            }
            else if (propertyName == "post_process") {
                if constexpr (std::is_same_v<T, std::vector<PostProcessPass>>) return d.postProcess;
            }

            // Unknown property (or type mismatch): fall back to the type-generic default.
            return getSceneProperty<T>(nullptr, propertyName);
        }

        template<typename T>
        static void setSceneProperty(Scene* scene, const std::string& propertyName, const T& value) {
            if (!scene) return;

            if (const PostProcessingSettings::Field* field = getPostProcessingField(propertyName)) {
                if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int> || std::is_same_v<T, float> ||
                              std::is_same_v<T, Vector3> || std::is_same_v<T, std::string>) {
                    if (postProcessingFieldMatches<T>(*field)) {
                        PostProcessingSettings settings = scene->getPostProcessing();
                        *(T*)field->ref(settings) = value;
                        scene->setPostProcessing(settings);
                    }
                }
            }
            else if (propertyName == "post_processing") {
                if constexpr (std::is_same_v<T, PostProcessingSettings>) {
                    scene->setPostProcessing(value);
                }
            }
            else if (propertyName == "background_color") {
                if constexpr (std::is_same_v<T, Vector4>) {
                    scene->setBackgroundColor(value);
                }
            }
            else if (propertyName == "shadows_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) {
                    scene->setShadowQuality(value);
                }
            }
            else if (propertyName == "global_illumination_color") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    scene->setGlobalIllumination(value);
                }
            }
            else if (propertyName == "global_illumination_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setGlobalIllumination(value);
                }
            }
            else if (propertyName == "max_lights_per_object") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setMaxLightsPerObject(value);
                }
            }
            else if (propertyName == "ambient_light_2d_color") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    scene->setAmbientLight2D(value);
                }
            }
            else if (propertyName == "ambient_light_2d_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setAmbientLight2D(value);
                }
            }
            else if (propertyName == "shadows_2d_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) {
                    scene->setShadow2DQuality(value);
                }
            }
            else if (propertyName == "physics_gravity_2d") {
                if constexpr (std::is_same_v<T, Vector2>) {
                    scene->setGravity2D(value);
                }
            }
            else if (propertyName == "physics_gravity_3d") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    scene->setGravity3D(value);
                }
            }
            else if (propertyName == "physics_interpolation") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setPhysicsInterpolation(value);
                }
            }
            else if (propertyName == "physics_2d_sub_steps") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setPhysics2DSubSteps(value);
                }
            }
            else if (propertyName == "light_state") {
                if constexpr (std::is_same_v<T, LightState>) {
                    scene->setLightState(value);
                }
            }
            else if (propertyName == "mesh_lod_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setMeshLodEnabled(value);
                }
            }
            else if (propertyName == "mesh_lod_threshold") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setMeshLodThreshold(value);
                }
            }
            else if (propertyName == "depth_prepass_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setDepthPrepassEnabled(value);
                }
            }
            else if (propertyName == "scene_color_scale") {
                if constexpr (std::is_same_v<T, SceneColorScale>) {
                    scene->setSceneColorScale(value);
                }
            }
            else if (propertyName == "ssao_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setSSAOEnabled(value);
                }
            }
            else if (propertyName == "ssao_radius") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSAORadius(value);
                }
            }
            else if (propertyName == "ssao_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSAOIntensity(value);
                }
            }
            else if (propertyName == "ssao_bias") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSAOBias(value);
                }
            }
            else if (propertyName == "ssao_debug") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setSSAODebug(value);
                }
            }
            else if (propertyName == "ssr_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setSSREnabled(value);
                }
            }
            else if (propertyName == "ssr_max_distance") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSRMaxDistance(value);
                }
            }
            else if (propertyName == "ssr_thickness") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSRThickness(value);
                }
            }
            else if (propertyName == "ssr_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSRIntensity(value);
                }
            }
            else if (propertyName == "ssr_blur") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSRBlur(value);
                }
            }
            else if (propertyName == "ssr_max_steps") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setSSRMaxSteps(value);
                }
            }
            else if (propertyName == "ssr_debug_mode") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setSSRDebugMode(value);
                }
            }
            else if (propertyName == "fixed_resolution_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setFixedResolutionEnabled(value);
                }
            }
            else if (propertyName == "fixed_resolution_width") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setFixedResolutionWidth((unsigned int)std::max(1, (int)value));
                }
            }
            else if (propertyName == "fixed_resolution_height") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setFixedResolutionHeight((unsigned int)std::max(1, (int)value));
                }
            }
            else if (propertyName == "fixed_resolution_filter") {
                if constexpr (std::is_same_v<T, TextureFilter>) {
                    scene->setFixedResolutionFilter(value);
                }
            }
            else if (propertyName == "default_mesh_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    scene->setDefaultMeshShader(value);
                }
            }
            else if (propertyName == "default_ui_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    scene->setDefaultUIShader(value);
                }
            }
            else if (propertyName == "default_sky_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    scene->setDefaultSkyShader(value);
                }
            }
            else if (propertyName == "default_points_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    scene->setDefaultPointsShader(value);
                }
            }
            else if (propertyName == "default_lines_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    scene->setDefaultLinesShader(value);
                }
            }
            else if (propertyName == "post_process") {
                if constexpr (std::is_same_v<T, std::vector<PostProcessPass>>) {
                    scene->setPostProcessPasses(value);
                }
            }
        }
    };

}
