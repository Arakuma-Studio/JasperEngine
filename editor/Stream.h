// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "yaml-cpp/yaml.h"
#include "Scene.h"
#include "ecs/Entity.h"
#include "Project.h"
#include "action/Ease.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include "math/Matrix4.h"
#include "component/FluidSimulationComponent.h"
#include "component/TrailRendererComponent.h"
#include "math/AnimationCurve.h"
#include "math/Gradient.h"
#include "component/VideoPlayerComponent.h"

#include <filesystem>
#include <set>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace jasperengine::editor {
    class Workspace;

    class Stream {
    private:

        static std::string sceneTypeToString(SceneType type);
        static SceneType stringToSceneType(const std::string& str);

        static std::string primitiveTypeToString(PrimitiveType type);
        static PrimitiveType stringToPrimitiveType(const std::string& str);

        static YAML::Node encodeEase(const Ease& ease);
        static Ease decodeEase(const YAML::Node& node, const Ease* oldEase = nullptr);

        static std::string bufferTypeToString(BufferType type);
        static BufferType stringToBufferType(const std::string& str);

        static std::string bufferUsageToString(BufferUsage usage);
        static BufferUsage stringToBufferUsage(const std::string& str);

        static std::string attributeTypeToString(AttributeType type);
        static AttributeType stringToAttributeType(const std::string& str);

        static std::string attributeDataTypeToString(AttributeDataType type);
        static AttributeDataType stringToAttributeDataType(const std::string& str);

        static std::string cullingModeToString(CullingMode mode);
        static CullingMode stringToCullingMode(const std::string& str);

        static std::string windingOrderToString(WindingOrder order);
        static WindingOrder stringToWindingOrder(const std::string& str);

        static std::string lightTypeToString(LightType type);
        static LightType stringToLightType(const std::string& str);

        static std::string fogTypeToString(FogType type);
        static FogType stringToFogType(const std::string& str);

        static std::string lightStateToString(LightState state);
        static LightState stringToLightState(const std::string& str);

        static std::string audioSourceStateToString(AudioSourceState state);
        static AudioSourceState stringToAudioSourceState(const std::string& str);

        static std::string uiDocumentRenderModeToString(UIDocumentRenderMode mode);
        static UIDocumentRenderMode stringToUIDocumentRenderMode(const std::string& str);

        static std::string fluidOutputToString(FluidOutput output);
        static FluidOutput stringToFluidOutput(const std::string& str);
        // lattice enums: the lowercase enumerator names (spec §2.0)
        static std::string latticeInterpolationToString(LatticeInterpolation interpolation);
        static LatticeInterpolation stringToLatticeInterpolation(const std::string& str);
        static std::string latticeOutsideToString(LatticeOutside outside);
        static LatticeOutside stringToLatticeOutside(const std::string& str);
        static std::string latticeJiggleAnchorToString(LatticeJiggleAnchor anchor);
        static LatticeJiggleAnchor stringToLatticeJiggleAnchor(const std::string& str);
        static std::string latticeStageToString(LatticeStage stage);
        static LatticeStage stringToLatticeStage(const std::string& str);
        static std::string latticeMaskChannelToString(LatticeMaskChannel mask);
        static LatticeMaskChannel stringToLatticeMaskChannel(const std::string& str);
        // a list of Vector3 (lattice offsets, a pose, a lattice track key) and a list of them
        static YAML::Node encodeLatticeOffsets(const std::vector<Vector3>& offsets);
        static std::vector<Vector3> decodeLatticeOffsets(const YAML::Node& node);
        static YAML::Node encodeLatticeTrackKeys(const std::vector<std::vector<Vector3>>& keys);
        static std::vector<std::vector<Vector3>> decodeLatticeTrackKeys(const YAML::Node& node);

        static std::string audioSourceAttenuationToString(AudioSourceAttenuation attenuation);
        static AudioSourceAttenuation stringToAudioSourceAttenuation(const std::string& str);

        static std::string uiEventStateToString(UIEventState state);
        static UIEventState stringToUIEventState(const std::string& str);

        static std::string cameraTypeToString(CameraType type);
        static CameraType stringToCameraType(const std::string& str);

        static std::string pivotPresetToString(PivotPreset preset);
        static PivotPreset stringToPivotPreset(const std::string& str);

        static std::string anchorPresetToString(AnchorPreset preset);
        static AnchorPreset stringToAnchorPreset(const std::string& str);

        static std::string materialAlphaModeToString(MaterialAlphaMode mode);
        static MaterialAlphaMode stringToMaterialAlphaMode(const std::string& str);

        // shader library render state and lighting (Material, UI/Points/LineRenderer, Light)
        static std::string materialBlendModeToString(MaterialBlendMode mode);
        static MaterialBlendMode stringToMaterialBlendMode(const std::string& str);
        static std::string materialCullModeToString(MaterialCullMode mode);
        static MaterialCullMode stringToMaterialCullMode(const std::string& str);
        static std::string materialDepthWriteToString(MaterialDepthWrite mode);
        static MaterialDepthWrite stringToMaterialDepthWrite(const std::string& str);
        static std::string materialDepthTestToString(MaterialDepthTest mode);
        static MaterialDepthTest stringToMaterialDepthTest(const std::string& str);
        static std::string materialLightingToString(MaterialLighting mode);
        static MaterialLighting stringToMaterialLighting(const std::string& str);
        static std::string lightShadingToString(LightShading shading);
        static LightShading stringToLightShading(const std::string& str);

        // shader library textures (by @texture uniform) and features of a material or component
        static YAML::Node encodeShaderTextures(const std::vector<std::pair<std::string, Texture>>& textures, bool embedData);
        static std::vector<std::pair<std::string, Texture>> decodeShaderTextures(const YAML::Node& node);
        static YAML::Node encodeShaderFeatures(const std::vector<std::string>& features);
        static std::vector<std::string> decodeShaderFeatures(const YAML::Node& node);
        // the shaderTextures, shaderFeatures and (when given) blendMode/depthWrite keys of a
        // UI, Points, LineRenderer or Skybox component
        static void encodeComponentShaderLibrary(YAML::Node& node, const std::vector<std::pair<std::string, Texture>>& textures,
                const std::vector<std::string>& features, const MaterialBlendMode* blendMode, const MaterialDepthWrite* depthWrite, bool embedData);
        static void decodeComponentShaderLibrary(const YAML::Node& node, std::vector<std::pair<std::string, Texture>>& textures,
                std::vector<std::string>& features, MaterialBlendMode* blendMode, MaterialDepthWrite* depthWrite);

        // shader library phase 2: stencil, colour write and the extra pass
        // (docs/superpowers/specs/2026-10-10-shader-library-phase2-design.md)
        static std::string materialStencilCompareToString(MaterialStencilCompare compare);
        static MaterialStencilCompare stringToMaterialStencilCompare(const std::string& str);
        static std::string materialStencilOpToString(MaterialStencilOp op);
        static MaterialStencilOp stringToMaterialStencilOp(const std::string& str);
        static std::string materialPassOrderToString(MaterialPassOrder order);
        static MaterialPassOrder stringToMaterialPassOrder(const std::string& str);
        // a colour write mask as the manifest writes it ("rgba", "rgb", "none"...; -1 "auto")
        static std::string colorWriteToString(int colorWrite);
        static int stringToColorWrite(const std::string& str);
        // a stencil block: compare, then the fields away from their defaults
        static YAML::Node encodeStencil(const MaterialStencil& stencil);
        static MaterialStencil decodeStencil(const YAML::Node& node);
        // the stencil and colorWrite keys of a material, pass or UI/Points/LineRenderer
        // component, written only away from their defaults (AUTO)
        static void encodeStencilAndColorWrite(YAML::Node& node, const MaterialStencil& stencil, int colorWrite);
        static void decodeStencilAndColorWrite(const YAML::Node& node, MaterialStencil& stencil, int& colorWrite);
        // a material's extra pass; only the fields away from their defaults
        static YAML::Node encodeMaterialPass(const MaterialPass& pass, bool embedTextureData);
        static MaterialPass decodeMaterialPass(const YAML::Node& node);

        static std::string scriptPropertyTypeToString(ScriptPropertyType type);
        static ScriptPropertyType stringToScriptPropertyType(const std::string& str);

        static std::string particleEmitterShapeToString(ParticleEmitterShape shape);
        static ParticleEmitterShape stringToParticleEmitterShape(const std::string& str);

        static std::string scriptTypeToString(ScriptType type);
        static ScriptType stringToScriptType(const std::string& str);

        static std::string layoutGroupTypeToString(LayoutGroupType type);
        static LayoutGroupType stringToLayoutGroupType(const std::string& str);
        static std::string canvasGroupRenderModeToString(CanvasGroupRenderMode mode);
        static CanvasGroupRenderMode stringToCanvasGroupRenderMode(const std::string& str);
        static std::string alphaTweenTargetToString(AlphaTweenTarget target);
        static AlphaTweenTarget stringToAlphaTweenTarget(const std::string& str);

        static std::string scrollbarTypeToString(ScrollbarType type);
        static ScrollbarType stringToScrollbarType(const std::string& str);

        static std::string progressbarTypeToString(ProgressbarType type);
        static ProgressbarType stringToProgressbarType(const std::string& str);

        static std::string scalingModeToString(Scaling mode);
        static Scaling stringToScalingMode(const std::string& str);

        static std::string textureStrategyToString(TextureStrategy strategy);
        static TextureStrategy stringToTextureStrategy(const std::string& str);

        static std::string windowModeToString(WindowMode mode);
        static WindowMode stringToWindowMode(const std::string& str);

        static float finiteOr(float value, float fallback = 0.0f);
        static double finiteOr(double value, double fallback = 0.0);
        static float positiveFiniteOr(float value, float fallback = 1.0f);
        static float decodeFinite(const YAML::Node& node, float fallback);
        static double decodeFiniteDouble(const YAML::Node& node, double fallback);
        static float decodePositiveFinite(const YAML::Node& node, float fallback);
        // camera, light and reflection probe culling masks (one bit per layer)
        static uint32_t decodeLayerMask(const YAML::Node& node, uint32_t fallback);
        static void encodeFinite(YAML::Node& node, const char* key, float value);
        static void encodeFinite(YAML::Node& node, const char* key, double value);
        static void encodePositiveFinite(YAML::Node& node, const char* key, float value);
        static void encodeFiniteVector3(YAML::Node& node, const char* key, const Vector3& value);
        static Vector3 decodeFiniteVector3(const YAML::Node& node, const Vector3& fallback);

        // ==============================

        static YAML::Node encodeVector2(const Vector2& vec);
        static Vector2 decodeVector2(const YAML::Node& node);

        static YAML::Node encodeVector3(const Vector3& vec);
        static Vector3 decodeVector3(const YAML::Node& node);

        static YAML::Node encodeVector4(const Vector4& vec);
        static Vector4 decodeVector4(const YAML::Node& node);

        static YAML::Node encodeQuaternion(const Quaternion& quat);
        static Quaternion decodeQuaternion(const YAML::Node& node);

        static YAML::Node encodeShaderUniforms(const ShaderUniformValues& uniforms);
        static ShaderUniformValues decodeShaderUniforms(const YAML::Node& node);

        static YAML::Node encodeRect(const Rect& rect);
        static Rect decodeRect(const YAML::Node& node);

        static YAML::Node encodeMatrix4(const Matrix4& mat);
        static Matrix4 decodeMatrix4(const YAML::Node& node);

        static std::string makeEmbeddedTextureId();
        static const char* colorFormatToString(ColorFormat format);
        static ColorFormat stringToColorFormat(const std::string& value, int channels);
        static bool getEmbeddedTextureData(const Texture& texture, TextureData*& data);
        static bool setEmbeddedTextureData(Texture& texture, const std::string& preferredId, int width, int height, ColorFormat format, int channels, const std::vector<unsigned char>& pixels);

        static YAML::Node encodeTexture(const Texture& texture, bool embedData = true);
        static Texture decodeTexture(const YAML::Node& node);

        static YAML::Node encodeBuffer(const Buffer& buffer);
        static void decodeBuffer(Buffer& buffer, const YAML::Node& node);

        static YAML::Node encodeInterleavedBuffer(const InterleavedBuffer& buffer);
        static void decodeInterleavedBuffer(InterleavedBuffer& buffer, const YAML::Node& node);

        static YAML::Node encodeIndexBuffer(const IndexBuffer& buffer);
        static void decodeIndexBuffer(IndexBuffer& buffer, const YAML::Node& node);

        static YAML::Node encodeExternalBuffer(const ExternalBuffer& buffer);
        static void decodeExternalBuffer(ExternalBuffer& buffer, const YAML::Node& node);

        static YAML::Node encodeSubmesh(const Submesh& submesh, bool embedTextureData = true);
        static Submesh decodeSubmesh(const YAML::Node& node, const Submesh* oldSubmesh = nullptr);

        static YAML::Node encodeAABB(const AABB& aabb);
        static AABB decodeAABB(const YAML::Node& node);

        static YAML::Node encodeSpriteFrameData(const SpriteFrameData& frameData);
        static SpriteFrameData decodeSpriteFrameData(const YAML::Node& node);

        static YAML::Node encodeTileRectData(const TileRectData& tileRect);
        static TileRectData decodeTileRectData(const YAML::Node& node);

        static YAML::Node encodeTileData(const TileData& tile);
        static TileData decodeTileData(const YAML::Node& node);

        static YAML::Node encodeEntityAux(const Entity entity, const EntityRegistry* registry,
            const Project* project = nullptr, const SceneProject* sceneProject = nullptr,
            const std::filesystem::path& prefabPathOverride = {});

        // Prefab instance data next to its root (StreamPrefab.cpp): the `prefabInstance`
        // block, and the import that rebuilds the instance from it
        static void encodePrefabInstance(YAML::Node& entityNode, const Entity entity, const EntityRegistry* registry,
            const Project* project, const SceneProject* sceneProject, const Prefab* prefab);
        static void decodePrefabInstance(const YAML::Node& entityNode, Entity entity, EntityRegistry* registry,
            std::vector<Entity>* entities, Project* project, SceneProject* sceneProject,
            std::unordered_map<Entity, Entity>* entityRemap, std::vector<Entity>& allEntities);
        // A Missing Prefab root encoded as a plain GameObject gets back the instance keys it
        // was loaded with (Project::getMissingPrefab), nothing otherwise
        static void encodeMissingPrefab(YAML::Node& entityNode, Entity entity, const Project* project, const SceneProject* sceneProject);

        static YAML::Node encodeScriptProperty(const ScriptProperty& prop);
        static ScriptProperty decodeScriptProperty(const YAML::Node& node);

        // the Line and Trail Renderer settings both have (Stream.cpp)
        template<typename T>
        static void encodeRibbonFields(YAML::Node& node, const T& component, const T& defaults);
        template<typename T>
        static void decodeRibbonFields(const YAML::Node& node, T& component, const T& defaults);

    public:
        // Unity AnimationCurve and Gradient (lines and trails spec 6.1). A curve:
        // {keys: [[time, value], [time, value, inTangent, outTangent]...], pre: clamp|loop|
        // ping_pong, post: ...} (wraps written when not clamp, tangents when not flat, .inf
        // steps); a bare number reads as a constant curve. A gradient: {mode: blend|fixed,
        // colors: [[time, colour]...], alphas: [[time, alpha]...]}, a colour "#RRGGBB" (sRGB,
        // written when the linear value is exactly an 8-bit one) or [r, g, b] (linear); a single
        // colour ("#RRGGBB[AA]", [r, g, b(, a)]) reads as a constant gradient
        static YAML::Node encodeCurve(const AnimationCurve& curve);
        static AnimationCurve decodeCurve(const YAML::Node& node, const AnimationCurve& fallback = AnimationCurve::constant(1.0f));
        static YAML::Node encodeGradient(const Gradient& gradient);
        static Gradient decodeGradient(const YAML::Node& node, const Gradient& fallback = Gradient());
        static std::string curveWrapToString(CurveWrap wrap);
        static CurveWrap stringToCurveWrap(const std::string& str);
        // "#RRGGBB" / "#RRGGBBAA" (the # optional) as 0..1 sRGB values; false for other text
        static bool parseHexColor(const std::string& text, Vector4& srgb, bool& hasAlpha);
        // a colour value of the YAML/JSON forms: a hex string (sRGB, converted) or [r, g, b(, a)]
        // (linear) as linear RGBA
        static bool decodeColorValue(const YAML::Node& node, Vector4& linear, bool& hasAlpha);

        // A shader variant key (scene shaderKeys, project shaderAdditions/shaderExclusions):
        // a map of type, properties (a plain integer of up to 64 bits, left out when 0) and
        // customShader (a fork's project-relative path: the session-local id is not written;
        // a null node for an id without a name), and keyVersion 2 on a MESH key with bit 5 (Stochastic
        // Tiling): decodeShaderKey clears that bit of a key without it, a stale 'Pcf' (retired
        // July 2026) of a file written before. decodeShaderKey also reads the legacy packed integer
        // (32 property bits, customId dropped; bit 5 cleared). Neither normalizes.
        static YAML::Node encodeShaderKey(ShaderKey key);
        static bool decodeShaderKey(const YAML::Node& keyNode, ShaderKey& key);
        // A list of them, normalized both ways (ShaderPool::normalizeKey) so a since-retired
        // property bit stays out of the file
        static YAML::Node encodeShaderKeys(const std::set<ShaderKey>& shaderKeys);
        static std::vector<ShaderKey> decodeShaderKeys(const YAML::Node& node);

        // GameObject state next to the name: active, tag and layer
        static void encodeEntityState(YAML::Node& entityNode, const Entity entity, const EntityRegistry* registry);
        static void decodeEntityState(Entity entity, EntityRegistry* registry, const YAML::Node& entityNode);

        // An instance's data relative to its prefab from a node holding it: an instance root
        // node, a Missing Prefab record or a nested root of a prefab file. Reads the
        // `prefabInstance` block, or the keys written before it (`legacy` set then).
        static PrefabInstanceData decodePrefabInstanceData(const YAML::Node& node);
        // The `prefabInstance` block of parsed data (without `version`)
        static YAML::Node encodePrefabInstanceBlock(const PrefabInstanceData& data);
        // The keys a scene writes next to a prefab instance root (`prefabInstance`), relative
        // to it: what a Missing Prefab instance keeps of a live one
        static YAML::Node encodePrefabInstanceData(const Entity root, const EntityRegistry* registry,
            const Project* project, const SceneProject* sceneProject, const Prefab* prefab);
        // The GameObject keys of an entity node, the ones a Missing Prefab does not keep raw
        static bool isGameObjectKey(const std::string& key);

        // True when a mesh's geometry is regenerated from a model's source file on load
        // (the ModelComponent root or one of a multi-node model's generated child meshes),
        // so its buffers/embedded textures/bones must not be serialized, and replacing its
        // geometry in place would be undone on the next load.
        static bool isModelBackedMesh(const Entity entity, const EntityRegistry* registry, Signature signature);

        // True when a mesh's geometry is rebuilt on load from its sprite, tilemap, mesh
        // polygon or terrain component, so its buffers and bones must not be serialized.
        static bool isGeneratedMesh(const EntityRegistry* registry, Signature signature);

        // True when a UIComponent's geometry is rebuilt on load from its image, text or
        // polygon component, so its buffers must not be serialized.
        static bool isGeneratedUI(const EntityRegistry* registry, Signature signature);

        // Scaled single-shape offset above which physics may become unstable;
        // shared by the load-time warning and the Inspector warning
        static constexpr float MAX_SINGLE_SHAPE_PHYSICS_OFFSET = 50.0f;

        // Public because the enum names are also the wire format of AI editor actions
        // (EditorActionExecutor validates values by round-tripping through these).
        static std::string easeTypeToString(EaseType type);
        static EaseType stringToEaseType(const std::string& str);
        static std::string textureFilterToString(TextureFilter filter);
        static TextureFilter stringToTextureFilter(const std::string& str);

        static std::string textureWrapToString(TextureWrap wrap);
        static TextureWrap stringToTextureWrap(const std::string& str);
        static std::string textureAlphaBorderToString(TextureAlphaBorder alphaBorder);
        static TextureAlphaBorder stringToTextureAlphaBorder(const std::string& str);

        static YAML::Node encodeProject(Project* project);
        // The workspace supplies the per-user half of the project (tabs, cameras,
        // viewport toggles); project.yaml no longer carries it. See Workspace.h.
        static void decodeProject(Project* project, const YAML::Node& node, const Workspace& workspace);

        // Per-user state, written to .jasperengine/user/workspace.yaml. Kept here so the
        // reader that still accepts them from an old project.yaml and the writer
        // that produces the workspace file share one schema.
        static YAML::Node encodeTerrainEditorSettings(const TerrainEditorSettings& settings);
        static TerrainEditorSettings decodeTerrainEditorSettings(const YAML::Node& node);
        // Every field by name; a missing, unknown or unreadable value keeps its default
        static YAML::Node encodePhysicsPlacementSettings(const PhysicsPlacementSettings& settings);
        static PhysicsPlacementSettings decodePhysicsPlacementSettings(const YAML::Node& node);
        // The Sprite Normal Map window's: the settings as the recipe writes them (snake_case),
        // the view, light, toggles. A missing or unreadable value keeps its default; settings
        // out of their ranges throw (the workspace reader warns and keeps the defaults)
        static YAML::Node encodeSpriteNormalsSettings(const SpriteNormalsWindowSettings& settings);
        static SpriteNormalsWindowSettings decodeSpriteNormalsSettings(const YAML::Node& node);

        static YAML::Node encodeSceneDisplaySettings(const SceneDisplaySettings& settings);
        static void decodeSceneDisplaySettings(const YAML::Node& node, SceneDisplaySettings& settings);

        // True when a project.yaml scene entry still carries view keys, which is what
        // separates a pre-split file from one that only lists the scene.
        static bool hasSceneDisplaySettings(const YAML::Node& node);

        static YAML::Node encodeSceneProject(const Project* project, const SceneProject* sceneProject);
        static void decodeSceneProject(SceneProject* sceneProject, const YAML::Node& node, bool loadScene);
        static void decodeSceneCppScripts(SceneProject* sceneProject, const YAML::Node& node);
        // reservedBelow: user ids under it are reserved too and new ones start above it (a
        // prefab stage: ids its prefab handed out once, its nextEntity, are never given to
        // the members of the instances nested in it)
        static void decodeSceneProjectEntities(Project* project, SceneProject* sceneProject, const YAML::Node& node, Entity reservedBelow = NULL_ENTITY);

        static YAML::Node encodeEditorCamera(Camera* camera, float zoom = 0.0f, float walkSpeedOffset = 0.0f);
        static void decodeEditorCamera(Camera* camera, const YAML::Node& node, float& zoom, float& walkSpeedOffset);

        static YAML::Node encodeScene(Scene* scene);
        static Scene* decodeScene(Scene* scene, const YAML::Node& node);
        // PostProcessingSettings: the fields away from their defaults (an empty map when none)
        static YAML::Node encodePostProcessing(const PostProcessingSettings& settings);
        static PostProcessingSettings decodePostProcessing(const YAML::Node& node);

        static YAML::Node encodeEntitySelection(const std::vector<Entity>& entities, const EntityRegistry* registry, const Project* project = nullptr, const SceneProject* sceneProject = nullptr);
        static std::vector<Entity> decodeEntitySelection(const YAML::Node& entityNode, EntityRegistry* registry, std::vector<Entity>* entities = nullptr, Project* project = nullptr, SceneProject* sceneProject = nullptr, Entity parent = NULL_ENTITY, bool createNewIfExists = true, std::unordered_map<Entity, Entity>* entityRemap = nullptr);
        static YAML::Node encodeEntity(const Entity entity, const EntityRegistry* registry, const Project* project = nullptr, const SceneProject* sceneProject = nullptr);
        // Recovery can encode a member subtree independently of its containing prefab while
        // still keeping active imported prefab descendants opaque.
        static YAML::Node encodeEntity(const Entity entity, const EntityRegistry* registry,
            const Project* project, const SceneProject* sceneProject, bool ignoreContainingPrefab);
        // removeMissingComponents also removes the components the node does not hold, so a
        // live entity is restored exactly. Only for nodes with a full component encode.
        static std::vector<Entity> decodeEntity(const YAML::Node& entityNode, EntityRegistry* registry, std::vector<Entity>* entities = nullptr, Project* project = nullptr, SceneProject* sceneProject = nullptr, Entity parent = NULL_ENTITY, bool createNewIfExists = true, bool removeMissingComponents = false, std::unordered_map<Entity, Entity>* entityRemap = nullptr);

        static YAML::Node encodeMaterial(const Material& material, bool embedTextureData = true);
        static Material decodeMaterial(const YAML::Node& node, const Material* oldMaterial = nullptr);

        static YAML::Node encodeComponents(const Entity entity, const EntityRegistry* registry, Signature signature);
        static void decodeComponents(Entity entity, Entity parent, EntityRegistry* registry, const YAML::Node& compNode);

        static YAML::Node encodeTransform(const Transform& transform);
        static Transform decodeTransform(const YAML::Node& node, const Transform* oldTransform = nullptr);

        static YAML::Node encodeMeshComponent(const MeshComponent& mesh, bool encodeBuffers = true, bool embedTextureData = true, bool encodeBones = true);
        static MeshComponent decodeMeshComponent(const YAML::Node& node, const MeshComponent* oldMesh = nullptr);

        static YAML::Node encodeUIComponent(const UIComponent& ui, bool encodeBuffers = true, bool embedTextureData = true);
        static UIComponent decodeUIComponent(const YAML::Node& node, const UIComponent* oldUI = nullptr);

        static YAML::Node encodeButtonComponent(const ButtonComponent& button);
        static ButtonComponent decodeButtonComponent(const YAML::Node& node, const ButtonComponent* oldButton = nullptr);

        static YAML::Node encodeScrollbarComponent(const ScrollbarComponent& scrollbar);
        static ScrollbarComponent decodeScrollbarComponent(const YAML::Node& node, const ScrollbarComponent* oldScrollbar = nullptr);

        static YAML::Node encodeProgressbarComponent(const ProgressbarComponent& progressbar);
        static ProgressbarComponent decodeProgressbarComponent(const YAML::Node& node, const ProgressbarComponent* oldProgressbar = nullptr);

        static YAML::Node encodeInputFieldComponent(const InputFieldComponent& inputfield);
        static InputFieldComponent decodeInputFieldComponent(const YAML::Node& node, const InputFieldComponent* oldInputField = nullptr);

        static YAML::Node encodePanelComponent(const PanelComponent& panel);
        static PanelComponent decodePanelComponent(const YAML::Node& node, const PanelComponent* oldPanel = nullptr);

        static YAML::Node encodeRectTransformComponent(const RectTransformComponent& layout);
        static RectTransformComponent decodeRectTransformComponent(const YAML::Node& node, const RectTransformComponent* oldLayout = nullptr);

        static YAML::Node encodeLayoutGroupComponent(const LayoutGroupComponent& container);
        static LayoutGroupComponent decodeLayoutGroupComponent(const YAML::Node& node, const LayoutGroupComponent* oldContainer = nullptr);
        static YAML::Node encodeCanvasGroupComponent(const CanvasGroupComponent& group);
        static CanvasGroupComponent decodeCanvasGroupComponent(const YAML::Node& node, const CanvasGroupComponent* oldGroup = nullptr);

        static YAML::Node encodeTextComponent(const TextComponent& text);
        static TextComponent decodeTextComponent(const YAML::Node& node, const TextComponent* oldText = nullptr);

        static YAML::Node encodeImageComponent(const ImageComponent& image);
        static ImageComponent decodeImageComponent(const YAML::Node& node, const ImageComponent* oldImage = nullptr);

        static YAML::Node encodeSpriteComponent(const SpriteComponent& sprite);
        static SpriteComponent decodeSpriteComponent(const YAML::Node& node, const SpriteComponent* oldSprite = nullptr);

        static YAML::Node encodeTilemapComponent(const TilemapComponent& tilemap);
        static TilemapComponent decodeTilemapComponent(const YAML::Node& node, const TilemapComponent* oldTilemap = nullptr);

        static YAML::Node encodeTerrainSurfaceLayer(const TerrainSurfaceLayer& layer);
        static TerrainSurfaceLayer decodeTerrainSurfaceLayer(const YAML::Node& node);
        static YAML::Node encodeTerrainFoliageLayer(const TerrainFoliageLayer& layer);
        static TerrainFoliageLayer decodeTerrainFoliageLayer(const YAML::Node& node);
        static YAML::Node encodeTerrainComponent(const TerrainComponent& terrain);
        static TerrainComponent decodeTerrainComponent(const YAML::Node& node, const TerrainComponent* oldTerrain = nullptr);

        static YAML::Node encodeLightComponent(const LightComponent& light);
        static LightComponent decodeLightComponent(const YAML::Node& node, const LightComponent* oldLight = nullptr);

        static YAML::Node encodeLight2DComponent(const Light2DComponent& light);
        static Light2DComponent decodeLight2DComponent(const YAML::Node& node, const Light2DComponent* oldLight = nullptr);

        static YAML::Node encodeShadowCaster2DComponent(const ShadowCaster2DComponent& occluder);
        static ShadowCaster2DComponent decodeShadowCaster2DComponent(const YAML::Node& node, const ShadowCaster2DComponent* oldOccluder = nullptr);

        static YAML::Node encodeFogComponent(const FogComponent& fog);
        static FogComponent decodeFogComponent(const YAML::Node& node, const FogComponent* oldFog = nullptr);

        static YAML::Node encodeMirrorComponent(const MirrorComponent& mirror);
        static MirrorComponent decodeMirrorComponent(const YAML::Node& node, const MirrorComponent* oldMirror = nullptr);

        static YAML::Node encodeReflectionProbeComponent(const ReflectionProbeComponent& probe);
        static ReflectionProbeComponent decodeReflectionProbeComponent(const YAML::Node& node, const ReflectionProbeComponent* oldProbe = nullptr);

        static YAML::Node encodeUIDocumentComponent(const UIDocumentComponent& document);
        static UIDocumentComponent decodeUIDocumentComponent(const YAML::Node& node, const UIDocumentComponent* oldDocument = nullptr);

        // only what differs from the defaults is written; a missing key is the default. Over an
        // old component its runtime state (the simulator) stays; a new seed or start burst
        // restarts it (needReset)
        static YAML::Node encodeVideoPlayerComponent(const VideoPlayerComponent& player);
        static VideoPlayerComponent decodeVideoPlayerComponent(const YAML::Node& node, const VideoPlayerComponent* oldPlayer = nullptr);
        static YAML::Node encodeFluidSimulationComponent(const FluidSimulationComponent& fluid);
        static FluidSimulationComponent decodeFluidSimulationComponent(const YAML::Node& node, const FluidSimulationComponent* oldFluid = nullptr);

        static YAML::Node encodePoseAnimatorComponent(const PoseAnimatorComponent& animator);
        static PoseAnimatorComponent decodePoseAnimatorComponent(const YAML::Node& node, const PoseAnimatorComponent* oldAnimator = nullptr);

        // Animation Player (spec 3.2, 9.R8/R14/R25): the authored fields only. clips[] entries
        // are {clip, guid, name, wrap}: clip is the path hint (asset-relative .anim or
        // "<model>#<clip>"), guid the .anim's GUID (empty, and not written, for sub-clips),
        // name and wrap written when set. A plain string entry reads as {clip}. Decoding never
        // loads (play startup decodes off the main thread). Over an old component its runtime
        // stays (states, rest values) and a changed clip list sets needsLoad; the callbacks
        // are cleared.
        static YAML::Node encodeAnimationPlayerComponent(const AnimationPlayerComponent& player);
        static AnimationPlayerComponent decodeAnimationPlayerComponent(const YAML::Node& node, const AnimationPlayerComponent* oldPlayer = nullptr);

        // Vertex Animator (vertex animation spec 3.3, 3.9; scene key "vertexanimator"): the authored
        // fields only. `animation` is the .vat path hint (asset-relative), `guid` its GUID (not
        // written when empty), attachments[] are {socket, entity}. Decoding never loads; over an
        // old component everything it played is reset (the system starts it again) except the
        // editor's Preview toggle, and the callbacks are cleared.
        static YAML::Node encodeVertexAnimatorComponent(const VertexAnimatorComponent& animator);
        static VertexAnimatorComponent decodeVertexAnimatorComponent(const YAML::Node& node, const VertexAnimatorComponent* oldAnimator = nullptr);

        static YAML::Node encodeCameraComponent(const CameraComponent& camera);
        static CameraComponent decodeCameraComponent(const YAML::Node& node, const CameraComponent* oldCamera = nullptr);

        static YAML::Node encodeAudioSourceComponent(const AudioSourceComponent& audio);
        static AudioSourceComponent decodeAudioSourceComponent(const YAML::Node& node, const AudioSourceComponent* oldAudio = nullptr);

        static YAML::Node encodeScriptComponent(const ScriptComponent& script);
        static ScriptComponent decodeScriptComponent(const YAML::Node& node, const ScriptComponent* oldScript = nullptr);

        static YAML::Node encodeSkyboxComponent(const SkyboxComponent& sky);
        static SkyboxComponent decodeSkyboxComponent(const YAML::Node& node, const SkyboxComponent* oldSky = nullptr);

        static YAML::Node encodeModelComponent(const ModelComponent& model);
        static ModelComponent decodeModelComponent(const YAML::Node& node, const ModelComponent* oldModel = nullptr);

        static YAML::Node encodePrefabComponent(const PrefabComponent& prefab);
        static PrefabComponent decodePrefabComponent(const YAML::Node& node, const PrefabComponent* oldPrefab = nullptr);

        static std::string bodyTypeToString(BodyType type);
        static BodyType stringToBodyType(const std::string& str);

        static std::string rigidbodyMotionQualityToString(RigidbodyMotionQuality motionQuality);
        static RigidbodyMotionQuality stringToRigidbodyMotionQuality(const std::string& str);

        static std::string collider2DTypeToString(Collider2DType type);
        static Collider2DType stringToCollider2DType(const std::string& str);

        static std::string collider3DTypeToString(Collider3DType type);
        static Collider3DType stringToCollider3DType(const std::string& str);

        static std::string joint2DTypeToString(Joint2DType type);
        static Joint2DType stringToJoint2DType(const std::string& str);
        static std::string jointAnchorSpaceToString(JointAnchorSpace space);
        static JointAnchorSpace stringToJointAnchorSpace(const std::string& str);
        static std::string jointBreakActionToString(JointBreakAction action);
        static JointBreakAction stringToJointBreakAction(const std::string& str);

        static std::string joint3DTypeToString(Joint3DType type);
        static Joint3DType stringToJoint3DType(const std::string& str);

        static YAML::Node encodeRigidbody2DComponent(const Rigidbody2DComponent& body);
        static Rigidbody2DComponent decodeRigidbody2DComponent(const YAML::Node& node, const Rigidbody2DComponent* oldBody = nullptr);

        static YAML::Node encodeRigidbodyComponent(const RigidbodyComponent& body);
        static RigidbodyComponent decodeRigidbodyComponent(const YAML::Node& node, const RigidbodyComponent* oldBody = nullptr, const Vector3& bodyScale = Vector3::UNIT_SCALE);

        static YAML::Node encodeJoint2DComponent(const Joint2DComponent& joint);
        static Joint2DComponent decodeJoint2DComponent(const YAML::Node& node, const Joint2DComponent* oldJoint = nullptr);

        static YAML::Node encodeJoint3DComponent(const Joint3DComponent& joint);
        static Joint3DComponent decodeJoint3DComponent(const YAML::Node& node, const Joint3DComponent* oldJoint = nullptr);

        static std::string actionStateToString(ActionState state);
        static ActionState stringToActionState(const std::string& str);

        static YAML::Node encodeActionComponent(const ActionComponent& action);
        static ActionComponent decodeActionComponent(const YAML::Node& node, const ActionComponent* oldAction = nullptr);

        static YAML::Node encodeTweenComponent(const TweenComponent& timed);
        static TweenComponent decodeTweenComponent(const YAML::Node& node, const TweenComponent* oldTimed = nullptr);

        static YAML::Node encodePositionTweenComponent(const PositionTweenComponent& posAction);
        static PositionTweenComponent decodePositionTweenComponent(const YAML::Node& node, const PositionTweenComponent* oldPosAction = nullptr);

        static YAML::Node encodeRotationTweenComponent(const RotationTweenComponent& rotAction);
        static RotationTweenComponent decodeRotationTweenComponent(const YAML::Node& node, const RotationTweenComponent* oldRotAction = nullptr);

        static YAML::Node encodeScaleTweenComponent(const ScaleTweenComponent& scaleTween);
        static ScaleTweenComponent decodeScaleTweenComponent(const YAML::Node& node, const ScaleTweenComponent* oldScaleTween = nullptr);

        static YAML::Node encodeColorTweenComponent(const ColorTweenComponent& colorTween);
        static ColorTweenComponent decodeColorTweenComponent(const YAML::Node& node, const ColorTweenComponent* oldColorTween = nullptr);

        static YAML::Node encodeAlphaTweenComponent(const AlphaTweenComponent& alphaTween);
        static AlphaTweenComponent decodeAlphaTweenComponent(const YAML::Node& node, const AlphaTweenComponent* oldAlphaTween = nullptr);

        static YAML::Node encodeSpriteAnimationComponent(const SpriteAnimationComponent& spriteanim);
        static SpriteAnimationComponent decodeSpriteAnimationComponent(const YAML::Node& node, const SpriteAnimationComponent* oldSpriteanim = nullptr);

        static YAML::Node encodeAnimationComponent(const AnimationComponent& animation);
        static AnimationComponent decodeAnimationComponent(const YAML::Node& node, const AnimationComponent* oldAnimation = nullptr);

        static YAML::Node encodeBoneComponent(const BoneComponent& bone);
        static BoneComponent decodeBoneComponent(const YAML::Node& node, const BoneComponent* oldBone = nullptr);

        static YAML::Node encodeKeyframeTracksComponent(const KeyframeTracksComponent& tracks);
        static KeyframeTracksComponent decodeKeyframeTracksComponent(const YAML::Node& node, const KeyframeTracksComponent* oldTracks = nullptr);

        static YAML::Node encodeTranslateTracksComponent(const TranslateTracksComponent& tracks);
        static TranslateTracksComponent decodeTranslateTracksComponent(const YAML::Node& node, const TranslateTracksComponent* oldTracks = nullptr);

        static YAML::Node encodeRotateTracksComponent(const RotateTracksComponent& tracks);
        static RotateTracksComponent decodeRotateTracksComponent(const YAML::Node& node, const RotateTracksComponent* oldTracks = nullptr);

        static YAML::Node encodeScaleTracksComponent(const ScaleTracksComponent& tracks);
        static ScaleTracksComponent decodeScaleTracksComponent(const YAML::Node& node, const ScaleTracksComponent* oldTracks = nullptr);

        static YAML::Node encodeMorphTracksComponent(const MorphTracksComponent& tracks);
        static MorphTracksComponent decodeMorphTracksComponent(const YAML::Node& node, const MorphTracksComponent* oldTracks = nullptr);

        // Offsets are kept as many as the resolution has points: a list of another size is cut
        // or padded with zeros. Runtime state (jiggle, tweens, revision) is not written.
        static YAML::Node encodeLatticeComponent(const LatticeComponent& lattice);
        static LatticeComponent decodeLatticeComponent(const YAML::Node& node, const LatticeComponent* oldLattice = nullptr);

        static YAML::Node encodeLatticeModifierComponent(const LatticeModifierComponent& modifier);
        static LatticeModifierComponent decodeLatticeModifierComponent(const YAML::Node& node, const LatticeModifierComponent* oldModifier = nullptr);

        static YAML::Node encodeLatticeFollowerComponent(const LatticeFollowerComponent& follower);
        static LatticeFollowerComponent decodeLatticeFollowerComponent(const YAML::Node& node, const LatticeFollowerComponent* oldFollower = nullptr);

        static YAML::Node encodeLatticeTracksComponent(const LatticeTracksComponent& tracks);
        static LatticeTracksComponent decodeLatticeTracksComponent(const YAML::Node& node, const LatticeTracksComponent* oldTracks = nullptr);

        static YAML::Node encodeParticleSystemComponent(const ParticleSystemComponent& particles);
        static ParticleSystemComponent decodeParticleSystemComponent(const YAML::Node& node, const ParticleSystemComponent* oldParticles = nullptr);

        static YAML::Node encodeInstancedMeshComponent(const InstancedMeshComponent& instmesh);
        static InstancedMeshComponent decodeInstancedMeshComponent(const YAML::Node& node, const InstancedMeshComponent* oldInstmesh = nullptr);

        static YAML::Node encodePointsComponent(const PointsComponent& points);
        static PointsComponent decodePointsComponent(const YAML::Node& node, const PointsComponent* oldPoints = nullptr);

        static YAML::Node encodeLineRendererComponent(const LineRendererComponent& lines);
        static LineRendererComponent decodeLineRendererComponent(const YAML::Node& node, const LineRendererComponent* oldLines = nullptr);
        // a Line Renderer's polyline, width and ribbon keys (each written when not its default;
        // decoding an absent key gives the default)
        static void encodeLineRibbonSettings(YAML::Node& node, const LineRendererComponent& lines);
        static void decodeLineRibbonSettings(const YAML::Node& node, LineRendererComponent& lines);
        static YAML::Node encodeTrailRendererComponent(const TrailRendererComponent& trail);
        static TrailRendererComponent decodeTrailRendererComponent(const YAML::Node& node, const TrailRendererComponent* oldTrail = nullptr);
        static std::string lineRendererModeToString(LineRendererMode mode);
        static LineRendererMode stringToLineRendererMode(const std::string& str);
        static std::string lineWidthUnitToString(LineWidthUnit unit);
        static LineWidthUnit stringToLineWidthUnit(const std::string& str);
        static std::string lineAlignmentToString(LineAlignment alignment);
        static LineAlignment stringToLineAlignment(const std::string& str);
        static std::string lineTextureModeToString(LineTextureMode mode);
        static LineTextureMode stringToLineTextureMode(const std::string& str);

        static YAML::Node encodePolygonComponent(const PolygonComponent& polygon);
        static PolygonComponent decodePolygonComponent(const YAML::Node& node, const PolygonComponent* oldPolygon = nullptr);

        static YAML::Node encodeMeshPolygonComponent(const MeshPolygonComponent& polygon);
        static MeshPolygonComponent decodeMeshPolygonComponent(const YAML::Node& node, const MeshPolygonComponent* oldPolygon = nullptr);
    };
}
