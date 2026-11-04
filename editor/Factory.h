// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "SceneManager.h"
#include "Catalog.h"
#include "action/Ease.h"
#include "math/Curve.h"
#include "shader/ShaderGlobals.h"
#include <functional>
#include <memory>
#include <string>
#include <sstream>
#include <filesystem>
#include <array>
#include <set>
#include <map>
#include <unordered_map>
#include "util/Prefab.h"
#include "script/ScriptProperty.h"
#include "buffer/Buffer.h"

namespace fs = std::filesystem;

namespace jasperengine{
    enum class PivotPreset;
    enum class RigidbodyMotionQuality;
    enum class Collider2DType;
    enum class Collider3DType;
    enum class Collider3DSource;
    enum class ActionState;
    enum class AudioLoadType;
    enum class ScriptType;
    enum class ScriptPropertyType;
}

namespace jasperengine::editor{

    namespace reflect {
        struct EnumDesc;
    }


    class Project;

    // An instance's overrides of one of its GameObjects (not its root): the scene's values
    // of what it overrides go over what build_prefab_* made
    struct PrefabOverrideInfo {
        Entity sceneEntity;       // entity in the scene with override data
        // components with any property overridden: assigned whole from the scene
        std::vector<ComponentType> overriddenComponents;
        std::vector<ComponentType> addedComponents;
        std::vector<ComponentType> removedComponents;
        uint8_t state = 0;        // PrefabObjectOverrides::State bits
    };

    struct PrefabInstanceInfo {
        std::filesystem::path prefabPath;
        Entity rootEntity;
        std::set<Entity> memberEntities;
        std::vector<PrefabOverrideInfo> overrides;
        // Components of the prefab's root GameObject the instance root removed: the root is
        // a scene entity with its own components, which build_prefab_* only completes
        std::vector<ComponentType> rootRemovedComponents;
        // Members in the order build_prefab_* appends them, NULL_ENTITY when missing here
        std::vector<Entity> orderedMembers;
        // Entities the nested factory of each member appends, 0 for a plain member
        std::vector<size_t> nestedSizes;
        // A Prefab Variant's instance: the instance of its base at the same root, whose
        // GameObjects build_prefab_* appends right after the variant's own members
        // (editor/project/PREFABS.md 19), and how many entities that is
        std::shared_ptr<PrefabInstanceInfo> baseLevel;
        size_t baseSize = 0;
    };

    // A prefab as its instances are, for the export: one instance of it made by the editor's
    // import in a scratch scene (its nested prefabs at any depth, what it overrides of them,
    // the GameObjects it adds to them, references into them resolved), so the generated
    // build_prefab_* creates exactly what the editor shows (editor/project/PREFABS.md 17)
    struct BakedPrefab {
        std::shared_ptr<Scene> scene;
        Entity root = NULL_ENTITY;
        // The GameObjects in build_prefab_* member order: the prefab's own (registry order,
        // getPrefabMemberEntities), then, for each of them that is a nested prefab root, that
        // prefab's slots, recursively; NULL_ENTITY where a layer removed the GameObject (the
        // slot is kept so a scene's member indices do not move)
        std::vector<Entity> slots;
    };

    class Factory{
    private:
        static bool writeHeaderIfChanged(const std::filesystem::path& path, const std::string& varName, const unsigned char* data, size_t len);
        static bool ensureDefaultSkyFiles(const fs::path& baseDir);
        static void writeSkyIncludes(std::ostringstream& out, bool usesDefaultSky, const fs::path& generatedPath, const std::string& ind = "");

    public:
        // the C++ text of values (the reflection tables' exported code uses them too, editor/reflect)
        static std::string indentation(int spaces);
        static std::string formatVector2(const Vector2& v);
        static std::string formatVector3(const Vector3& v);
        static std::string formatVector4(const Vector4& v);
        static std::string formatShaderUniforms(const ShaderUniformValues& uniforms);
        static std::string formatRect(const Rect& r);
        static std::string formatPrimitiveType(PrimitiveType type);
        // the C++ name of a component enum's value (its reflection table's, editor/reflect/Enums.cpp)
        static std::string formatEnum(const reflect::EnumDesc& desc, int value);
        static std::string formatEaseType(EaseType type);
        static std::string formatEase(const Ease& ease);
        static std::string formatPivotPreset(PivotPreset preset);
        static std::string formatBodyType(BodyType type);
        static std::string formatRigidbodyMotionQuality(RigidbodyMotionQuality motionQuality);
        static std::string formatCollider2DType(Collider2DType type);
        static std::string formatCollider3DType(Collider3DType type);
        static std::string formatCollider3DSource(Collider3DSource source);
        static std::string formatActionState(ActionState state);
        static std::string formatLightState(LightState state);
        static std::string formatShadowQuality(ShadowQuality quality);
        static std::string formatSceneColorScale(SceneColorScale scale);
        static std::string formatPostProcessing(int indentSpaces, const PostProcessingSettings& settings, const std::string& variableName);
        static std::string formatAudioLoadType(AudioLoadType loadType);
        static std::string formatUIEventState(UIEventState state);
        static std::string formatScriptType(ScriptType type);
        static std::string formatQuaternion(const Quaternion& q);
        static std::string formatBool(bool value);
        // setEntityActive/Tag/Layer lines for what differs from a new GameObject
        static std::string formatEntityState(const std::string& ind, const std::string& entityExpr, const EntityRegistry* registry, Entity entity);
        static std::string formatPositiveFloat(float value);
        static std::string formatDouble(double value);
        static std::string formatInt(int value);
        static std::string formatUInt(unsigned int value);
        static std::string formatEntity(Entity entity, const std::unordered_map<Entity, std::string>* entityVarNames);
        // a culling mask in hex: 0xFFFFFFFFu is Everything
        static std::string formatLayerMask(uint32_t mask);
        static std::string formatChar(char value);
        static std::string formatAttributeType(AttributeType type);
        static std::string formatAttributeDataType(AttributeDataType type);
        static std::string formatTextureFilter(TextureFilter filter);
        static std::string formatTextureWrap(TextureWrap wrap);
        static std::string formatTextureAlphaBorder(TextureAlphaBorder alphaBorder);
        static std::string formatScriptPropertyType(ScriptPropertyType type);
        static std::string formatScriptPropertyValue(const ScriptPropertyValue& value, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);

        static std::string formatTexture(int indentSpaces, const Texture& texture, const std::string& variableName, const fs::path& projectPath);
        // a Text's font atlas: its sampler settings as Stream::encodeAtlasSampler writes them
        static std::string formatAtlasSampler(int indentSpaces, const Texture& texture, const std::string& variableName);
        // shader library: the statements filling `target` (a shaderTextures vector) and the
        // initializer of a shaderFeatures list
        static std::string formatShaderTextures(int indentSpaces, const std::vector<std::pair<std::string, Texture>>& textures, const std::string& target, const fs::path& projectPath);
        static std::string formatShaderFeatures(const std::vector<std::string>& features);
        // the shader-library fields of a Material named `target` that differ from the defaults
        static std::string formatMaterialShaderLibrary(int indentSpaces, const Material& material, const std::string& target, const fs::path& projectPath);
        // the emission intensity and the extended lobes of a material (target: its expression),
        // their values away from the defaults only
        static std::string formatMaterialPbr(int indentSpaces, const Material& material, const std::string& target, const fs::path& projectPath);
        // phase 2: the stencil fields and colour write away from their defaults, of a Material,
        // a MaterialPass or a UI/Points/LineRenderer component named `target`
        static std::string formatStencilAndColorWrite(int indentSpaces, const MaterialStencil& stencil, int colorWrite, const std::string& target);
        // the fields of an extra pass (a MaterialPass named `target`) away from their defaults
        static std::string formatMaterialPass(int indentSpaces, const MaterialPass& pass, const std::string& target, const fs::path& projectPath);
        // the same for a UI/Points/LineRenderer/Skybox component (blendMode, depthWrite when given)
        static std::string formatComponentShaderLibrary(int indentSpaces, const std::vector<std::pair<std::string, Texture>>& textures,
            const std::vector<std::string>& features, const MaterialBlendMode* blendMode, const MaterialDepthWrite* depthWrite,
            const std::string& target, const fs::path& projectPath);
        // attributes, settings and bytes of a saved buffer; nothing for an empty one
        static std::string formatBufferContents(const std::string& ind, const std::string& target, const Buffer& buffer, const Buffer& defaults);


        static std::string beginHeapComponentCode(std::ostringstream& code, const std::string& ind, const std::string& componentType, const std::string& varName);
        static void addComponentCode(std::ostringstream& code, const std::string& ind, const std::string& sceneName, const std::string& entityName, Entity entity, const std::string& componentType, const std::string& varName, bool assignExisting = false);
        static std::string guardComponentCode(ComponentType type, const std::string& code);
        static std::string guardPhysicsCode(const std::string& code, const char* backendMacro);

    public:
        // a CanvasScalerSettings expression with its non-default fields (generated code)
        static std::string formatCanvasScaler(const CanvasScalerSettings& settings);
        // Registry members in build_prefab_* order (the prefab's root GameObject left out: it is
        // the root build_prefab_* is given). A nested root's children are GameObjects the prefab
        // adds to that nested instance: members like any other.
        static std::vector<Entity> getPrefabMemberEntities(EntityRegistry* registry, const std::vector<Entity>& registryEntities, Entity root = NULL_ENTITY);

        static std::string toIdentifier(const std::string& name);
        // C++ string literal of the value, quotes included
        static std::string formatString(const std::string& value);
        static std::string formatFloat(float value);
        // Curve / Gradient literals (the Line and Trail Renderers' width curve and colour
        // gradient): stream/CurveCodec's C++ forms
        static std::string formatCurve(const Curve& curve);
        static std::string formatGradient(const Gradient& gradient);
        // {{"member", "global"}, ...} and a ShaderGlobalDecl{...} literal (shader globals spec 6.5)
        static std::string formatShaderGlobalBindings(const ShaderGlobalBindings& bindings);
        static std::string formatShaderGlobalDecl(const ShaderGlobalDecl& decl);

        static std::string prefabToFunctionName(const std::filesystem::path& prefabPath);
        static std::string prefabToBuildFunctionName(const std::filesystem::path& prefabPath);
        static std::string prefabToFileName(const std::filesystem::path& prefabPath);
        // File and symbol name of a scene's generated source: never the name of main.cpp,
        // scene_scripts.cpp, a prefab or another scene, even ignoring case
        static std::string sceneToFileName(uint32_t sceneId, const std::string& sceneName);

        // defaultCamera: the scene's Default Camera (not one of its GameObjects), whose canvas size
        // the game sets itself
        static std::string createComponent(int indentSpaces, EntityRegistry* scene, Entity entity, ComponentType componentType, const fs::path& projectPath, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr, bool defaultCamera = false);
        static std::string createAllComponents(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName = "", std::string entityName = "", const std::unordered_map<Entity, std::string>* entityVarNames = nullptr, bool defaultCamera = false);
        // First line of a generated scene source. Its number follows the source's shape, so an
        // older source counts as missing and is written again.
        static const char* const sceneSourceHeader;

        static std::string createScene(int indentSpaces, Scene* scene, uint32_t sceneId, std::string name, std::vector<Entity> entities, Entity camera, const fs::path& projectPath, const fs::path& generatedPath, const std::vector<PrefabInstanceInfo>& prefabInstances = {}, const SceneResources& resources = {});

        // The source of a prefab's build_prefab_* / create_prefab_* functions from its baked
        // instance. prefabGuid: the asset GUID of a prefab path (written on the bare root)
        static std::string createPrefab(const std::filesystem::path& prefabPath, const BakedPrefab& baked, const fs::path& projectPath, const fs::path& generatedPath,
            const std::function<std::string(const std::filesystem::path&)>& prefabGuid = nullptr);
        static std::string createPrefabHeader(const std::filesystem::path& prefabPath);
        // Binds each camera-linked texture of the entities to its camera's framebuffer, and each
        // fluid-linked one ("fluid|<e>") to its FluidSimulation's output; with namedCamerasOnly,
        // only links to cameras (fluids) that have a variable in entityVarNames
        static std::string createCameraTextureLinks(int indentSpaces, EntityRegistry* registry, const std::vector<Entity>& entities, const std::unordered_map<Entity, std::string>* entityVarNames, bool namedCamerasOnly);


        static std::string createTransform(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool skipParent = false, bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createMeshComponent(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createUIComponent(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createPolygonComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createMeshPolygonComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createSafeAreaComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createTextEffectComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createSpriteComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createTilemapComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createTerrainComponent(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createCameraShakerComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createCameraFollowComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createVideoPlayerComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createFluidSimulationComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createScriptComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createSkyboxComponent(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createRigidbody2DComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createRigidbodyComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        // navigation (S19, FactoryNavigation.cpp)
        static std::string createNavMeshSurfaceComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createNavMeshAgentComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createNavMeshObstacleComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createNavMeshLinkComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createNavMeshModifierComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createNavGridComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createSpriteAnimationComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createModelComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createKeyframeTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createTranslateTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createRotateTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createScaleTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createMorphTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createLatticeComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createLatticeModifierComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createLatticeFollowerComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createLatticeTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createPoseAnimatorComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        // the authored fields only (clip paths, names, wraps; not the editor's GUIDs)
        static std::string createAnimationPlayerComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        // the authored fields (no GUID: the build loads by path); attachments by entity
        static std::string createVertexAnimatorComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createParticleSystemComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createForceFieldComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false);
        static std::string createWaterVolumeComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false);
        static std::string createBuoyancyComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false);
        static std::string createCharacterControllerComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false);
        static std::string createCharacterController2DComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false);
        static std::string createLineRendererComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createTrailRendererComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createPointsComponent(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
        static std::string createInstancedMeshComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName = "", std::string entityName = "", bool assignExisting = false, const std::unordered_map<Entity, std::string>* entityVarNames = nullptr);
    };

}
