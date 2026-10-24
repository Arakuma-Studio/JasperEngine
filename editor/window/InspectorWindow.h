// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "render/Render.h"
#include "shader/ShaderManifest.h"
#include "render/AtmospherePresets.h"
#include "render/preview/MeshPreviewRender.h"
#include "render/preview/DirectionRender.h"
#include "util/PrimitiveParameters.h"
#include "window/dialog/ScriptCreateDialog.h"
#include "window/dialog/ShaderForkDialog.h"
#include "window/dialog/ComponentAddDialog.h"
#include "window/dialog/TextureSlicerToolDialog.h"

#include "imgui.h"
#include "yaml-cpp/yaml.h"
#include "ecs/Signature.h"
#include "soloud.h"
#include "soloud_wav.h"

#include <functional>

namespace jasperengine::editor{

    class SceneWindow;
    class ForkShaderCmd;
    class MultiPropertyCmd;

    enum class RowPropertyType{
        Label,
        Bool,
        String,
        MultilineString,
        Float,
        Double,
        FloatPositive,
        DoublePositive,
        Float_0_1,
        Vector2,
        Vector3,
        Vector4,
        Quat,
        Color3L,
        Color4L,
        Int,
        UInt,
        Material,
        Texture,
        TextureCube,
        HalfCone,
        UIntSlider,
        IntSlider,
        Direction,
        Enum,
        Ease,
        Custom,
        LocalEntity,
        ExternalEntity,
        Font,
        LayerMask  // unsigned int culling mask: Unity's Nothing / Everything / per-layer dropdown
    };

    struct EnumEntry {
        int value;
        const char* name;
    };

    struct RowSettings{
        float stepSize = 0.1f;
        float secondColSize = -1;
        // Inclusive clamp for RowPropertyType::Int (drag and typed entry). Left at
        // 0/0 (min >= max) the field stays unbounded, preserving existing rows.
        int intMin = 0;
        int intMax = 0;
        bool child = false;
        std::string help = "";
        const char *format = "%.2f";
        bool showColors = true;
        std::vector<EnumEntry>* enumEntries = nullptr;
        std::vector<int>* sliderValues = nullptr;
        std::function<void()> onValueChanged = nullptr;
        // Some texture properties only accept file/data-backed images.
        bool allowCameraTexture = true;
        // Hide sampler controls when the consumer owns its filtering and wrapping.
        bool allowTextureSettings = true;
        // Optional filter for LocalEntity picker: only show entities matching all bits
        Signature entityFilter;
        // Entities hidden from the LocalEntity picker and rejected on drag-drop
        // (e.g. to prevent self-references)
        std::vector<Entity> excludeEntities;
    };

    class InspectorWindow{
    private:
        Project* project;
        SceneWindow* sceneWindow;
        Command* cmd;

        bool windowOpen;
        bool focusRequested;
        int nameFocusFrames = 0;   // visible frames left for focusNameInput() to take
        bool finishProperty;

        std::set<std::string> usedPreviewIds;

        std::map<std::string, MaterialRender> materialRenders;
        std::map<std::string, DirectionRender> directionRenders;

        MeshPreviewRender shapePreviewRender;

        // for drag and drop textures
        std::set<std::string> hasTextureDrag;
        std::map<std::string, std::map<Entity, Texture>> originalTex;

        // for drag and drop fonts, keyed by property id and slot
        std::set<std::string> hasFontDrag;
        std::map<std::string, std::map<Entity, FontArray>> originalFont;

        std::map<std::string, bool> materialButtonGroups;
        std::map<std::string, bool> spriteFramesButtonGroups;
        bool spriteFramesExpanded = false;

        std::map<std::string, bool> tilemapRectsButtonGroups;
        bool tilemapRectsExpanded = true;

        std::map<std::string, bool> tilemapTilesButtonGroups;
        bool tilemapTilesExpanded = false;
        std::map<std::string, bool> spriteAnimationFramesButtonGroups;
        bool spriteAnimationFramesExpanded = false;
        std::map<std::string, bool> trackValuesExpanded;
        bool keyframeEasingsExpanded = false;
        bool instancesExpanded = false;
        std::map<std::string, bool> instancesButtonGroups;
        bool pointsExpanded = false;
        std::map<std::string, bool> pointsButtonGroups;
        bool linesExpanded = false;
        std::map<std::string, bool> linesButtonGroups;
        bool polygonExpanded = false;
        std::map<std::string, bool> polygonButtonGroups;
        bool meshPolygonExpanded = false;
        std::map<std::string, bool> meshPolygonButtonGroups;

        std::map<std::string, bool> textureCubeSingleMode;

        std::unordered_map<std::string, Texture> thumbnailTextures;

        // Dirty material deferred-write state
        enum class ScenePropertyInputType {
            Checkbox,
            DragFloat,
            DragInt,
            SliderFloat,
            ColorRGB,
            ColorRGBA,
            Combo
        };

        struct DirtyMaterialEntry {
            unsigned int sceneId;
            Entity entity;
            int submeshIndex;
            std::string relativePath;
            float timer;
        };

        static std::vector<DirtyMaterialEntry> dirtyMaterials;
        static constexpr float materialWriteDelaySec = 0.3f;

        // Sound preview state
        struct SoundPreviewRuntime {
            SoLoud::Wav sample;
            bool loaded = false;
            bool active = false;
            bool playing = false;
            Entity entity = NULL_ENTITY;
            uint32_t sceneId = 0;
            unsigned int handle = 0;
            std::string filename;
            std::string error;
            double length = 0.0;
            double time = 0.0;
        };

        SoundPreviewRuntime soundPreviewRuntime;

        // Action preview state
        bool actionPreviewPlaying = false;
        bool actionPreviewing = false;
        Entity actionPreviewEntity = 0;
        uint32_t actionPreviewSceneId = 0;
        struct ActionPreviewState {
            Entity entity = 0;
            Entity parent = 0;
            YAML::Node components;
        };
        std::vector<ActionPreviewState> actionPreviewStates;

        void startActionPreview(Entity entity, Scene* scene, SceneProject* sceneProject);
        void stopActionPreview(Scene* scene, SceneProject* sceneProject);
        void stopActionPreviewIfActive();
        void updateParticlePreviewSnapshot(YAML::Node& components, const ParticleSystemComponent& particles);

        // Dirty material helpers
        void markMaterialDirty(unsigned int sceneId, Entity entity, int submeshIndex, const std::string& relativePath);
        void flushDirtyMaterials(float deltaTime);
        void writeDirtyMaterial(const DirtyMaterialEntry& entry);
        template<typename T>
        void drawScenePropertyRow(SceneProject* sceneProject, const std::string& propertyName, const char* label, ScenePropertyInputType inputType, float inputWidth = -1.0f, float minValue = 0.0f, float maxValue = 1.0f, const std::string& help = "");

        // Sound preview helpers
        bool ensureSoundPreviewInitialized();
        void stopSoundPreview(bool unload = false);
        void applySoundPreviewSettings(const AudioSourceComponent& audio);
        bool loadSoundPreview(SceneProject* sceneProject, Entity entity, const AudioSourceComponent& audio);
        bool startSoundPreview(SceneProject* sceneProject, Entity entity, const AudioSourceComponent& audio);
        void pauseSoundPreview();
        bool seekSoundPreview(SceneProject* sceneProject, Entity entity, const AudioSourceComponent& audio, double time);
        void updateSoundPreview(const AudioSourceComponent& audio);

        // Property label formatting
        std::string formatPropertyLabelValue(const PropertyData& prop);

        // For entity picker popup
        char entityPickerSearchBuffer[128] = {};

        // Dialogs
        ScriptCreateDialog scriptCreateDialog;
        ShaderForkDialog shaderForkDialog;
        ComponentAddDialog componentAddDialog;
        TextureSlicerToolDialog textureSlicerToolDialog;

        static RowPropertyType scriptPropertyTypeToRowPropertyType(ScriptPropertyType scriptType);

        Vector3 roundZero(const Vector3& val, const float threshold) const;
        float snapDisplayedAngle(float angle, float threshold) const;
        Vector3 getDisplayedEulerAngles(const Quaternion& value, const RotationOrder& order, float zeroThreshold) const;

        bool compareVectorFloat(const float* a, const float* b, size_t elements, const float threshold);

        float getLabelSize(std::string label, bool addRotateIconSpace = true);
        static bool drawSummaryAddButton(const std::string& label, float trailingReservedWidth = 0.0f, bool enabled = true, const char* disabledTooltip = nullptr);

        void helpMarker(std::string desc);

        Texture* findThumbnail(const std::string& path);
        void drawImageWithBorderAndRounding(Texture* texture, const ImVec2& size, float rounding = 4.0f, ImU32 border_col = IM_COL32(0, 0, 0, 255), float border_thickness = 1.0f, bool flipY = false);
        void dragDropResourcesFont(ComponentType cpType, std::string id, size_t slot, SceneProject* sceneProject, std::vector<Entity> entities);
        void setFontSlot(ComponentType cpType, const std::string& id, size_t slot, const std::string& path, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged);
        // one slot of a font property: file name frame, file picker, clear button and drag-drop.
        // width covers the buttons too, -1 fills the table cell; extraButton adds one more
        void drawFontSlot(ComponentType cpType, const std::string& id, size_t slot, bool dif, const FontArray& value, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged, float width = -1.0f, const std::function<void(const ImVec2&)>& extraButton = nullptr);
        // fallback slots of a font property, opened by the Font row
        void drawFontFallbacksPopup(const char* popupId, ComponentType cpType, const std::string& id, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged);
        void dragDropResourcesTexture(ComponentType cpType, std::string id, SceneProject* sceneProject, std::vector<Entity> entities);
        void applyCameraTexture(Entity cameraEntity, ComponentType cpType, const std::string& id, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged);
        void drawTextureSettingsPopup(const char* popupId, ComponentType cpType, const std::string& id, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged);
        void dragDropResourcesTextureCube(ComponentType cpType, const std::string& id, const ImVec2& rectMin, const ImVec2& rectMax, SceneProject* sceneProject, const std::vector<Entity>& entities, int faceIndex = -1);

        // popupId: opened by the header's right click or its kebab button
        void handleComponentMenu(const char* popupId, SceneProject* sceneProject, std::vector<Entity> entities, ComponentType cpType, bool isPrefab, bool& headerOpen, bool readOnly);

        // Unity Inspector header: active checkbox, icon and name, then Tag and Layer
        void drawGameObjectHeader(SceneProject* sceneProject, const std::vector<Entity>& entities, const std::string& names, bool readOnly);
        // Unity's Prefab bar of the GameObject header (InspectorPrefabBar.cpp), when every
        // selected GameObject is in a prefab instance; gap is the header's column gap
        void drawPrefabBar(SceneProject* sceneProject, const std::vector<Entity>& entities, bool readOnly, float gap);
        // Unity's Overrides dropdown of the prefab bar, for the instance rooted at root
        void drawPrefabOverridesPopup(SceneProject* sceneProject, Entity root, bool readOnly);

        // ---- Prefab overrides in the Inspector (InspectorPrefabBar.cpp) ----
        // Unity: an overridden value has a bold label and a blue bar in the margin; a right
        // click on it (or on a component header, or the GameObject's name, active state, tag
        // or layer) offers Apply to the prefab(s) and Revert.
        struct PrefabRowContext{
            SceneProject* sceneProject = nullptr;
            std::vector<Entity> entities;       // the selection
            ComponentType type = ComponentType::Transform;
            bool active = false;                // some of them are GameObjects of prefab instances
            bool readOnly = false;
            // their overrides, read once per component drawn
            std::map<Entity, Prefab::ObjectOverrides> overrides;
        };
        PrefabRowContext prefabRow;
        // The property the next propertyHeader draws (propertyRow sets it), cleared by it
        std::string prefabRowProperty;
        // The Overrides dropdown's selected item and the root it belongs to
        std::string prefabOverridesSelection;
        Entity prefabOverridesRoot = NULL_ENTITY;

        void beginPrefabComponent(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType type, bool readOnly);
        void endPrefabComponent();
        // The overridden names matching a property row (exact, "*", or nested in it), per entity
        std::vector<std::string> prefabOverridesOf(SceneProject* sceneProject, Entity entity, ComponentType type, const std::string& property) const;
        bool isPrefabPropertyOverridden(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType type, const std::string& property) const;
        // Unity's blue bar at the Inspector's left margin, from y0 to y1
        void drawPrefabOverrideBar(float y0, float y1);
        // The Apply / Revert entries for a property, a component or GameObject state, into the
        // menu being drawn; stateBits for state (type ignored), property empty for a whole component
        void drawPrefabApplyRevertItems(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType type,
            const std::string& property, uint8_t stateBits, bool readOnly);
        // A property row's label: bold with the bar when overridden, right click for the menu
        void drawPrefabPropertyLabel(const std::string& label);
        // Context menu of a GameObject header field (name, active, tag, layer) under the last item
        void prefabStateContext(SceneProject* sceneProject, const std::vector<Entity>& entities, uint8_t stateBit, bool readOnly,
            float barTop, float barBottom);
        // Unity's "(Removed)" component entries after the components
        void drawRemovedPrefabComponents(SceneProject* sceneProject, const std::vector<Entity>& entities, bool readOnly);
        // The Overrides dropdown's comparison of one item (Prefab Source | Override)
        void drawPrefabComparison(SceneProject* sceneProject, const PrefabOverrideEntry& entry, bool locked);
        std::string formatPrefabValue(EntityRegistry* registry, const PropertyData& prop);
        void drawTagCombo(SceneProject* sceneProject, const std::vector<Entity>& entities, bool readOnly, float width);
        void drawLayerCombo(SceneProject* sceneProject, const std::vector<Entity>& entities, bool readOnly, float width);
        // Unity component header: foldout, icon, enable checkbox, name and a kebab menu; true when open
        bool drawComponentHeader(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType cpType, bool isPrefab, bool readOnly);
        // centered button after the last component, opening the Add Component popup
        void drawAddComponentButton(SceneProject* sceneProject, const std::vector<Entity>& entities, bool readOnly, bool forceOpen);
        void openNewScriptDialog(SceneProject* sceneProject, const std::vector<Entity>& entities);
        // Add Tag... / Add Layer... asked for Project Settings > Tags and Layers
        bool tagsAndLayersRequested = false;
        // layer picked for GameObjects with children, waiting for the Change Layer answer
        uint32_t pendingLayerSceneId = NULL_PROJECT_SCENE;
        std::vector<Entity> pendingLayerEntities;
        int pendingLayer = 0;

        bool canAddComponent(SceneProject* sceneProject, Entity entity, ComponentType cpType);

        // meshShader: what the material draws with when it names no shader (the mesh's or the scene's)
        Texture getMaterialPreview(const Material& material, const std::string id, bool receiveIBL = true,
                                   const std::string& meshShader = std::string());
        Texture getDirectionPreview(const Vector3& direction, const std::string id);
        bool drawSpriteFramePreview(Texture* texture, const Rect& rect, const ImVec2& size, const char* itemId);

        void updateShapePreview(const PrimitiveParameters& shapeParams);
        void updateMeshShape(MeshComponent& meshComp, MeshSystem* meshSys, const PrimitiveParameters& shapeParams);

        void drawNinePatchesPreview(const ImageComponent& img, Texture* texture, Texture* thumbTexture, const ImVec2& size = ImVec2(0, 0));

        struct EntityPickerResult {
            bool chosen = false;
            Entity entity = NULL_ENTITY;
            uint32_t sceneId = 0;
        };

        EntityPickerResult drawEntityPickerPopup(const std::string& popupId, const Signature& filter, SceneProject* owningScene, bool includeChildScenes, Entity currentValue, uint32_t currentValueSceneId = 0, const std::vector<Entity>& excludeEntities = {});
        void syncSubSelection(SceneProject* sceneProject, Entity entity, int tileIndex, int instanceIndex);

        void beginTable(ComponentType cpType, float firstColSize, std::string nameAddon = "");
        void endTable();
        bool propertyHeader(std::string label, float secondColSize = -1, bool defChanged = false, bool child = false);
        bool propertyRow(RowPropertyType type, ComponentType cpType, std::string id, std::string label, SceneProject* sceneProject, std::vector<Entity> entities, RowSettings settings = RowSettings());
        bool propertyRowWithAutoButton(RowPropertyType propType, ComponentType cpType, std::string id, std::string label, std::string autoId, std::string autoLabel, SceneProject* sceneProject, std::vector<Entity> entities, RowSettings settings = RowSettings());
        void setParticleFrames(ComponentType cpType, const std::string& propertyId, SceneProject* sceneProject, Entity entity, const std::vector<int>& frames);
        void setParticleBursts(ComponentType cpType, SceneProject* sceneProject, Entity entity, const std::vector<ParticleBurst>& bursts);
        void setParticleColorGradient(ComponentType cpType, SceneProject* sceneProject, Entity entity, const ParticleColorGradient& gradient);
        void drawParticleFrameList(ComponentType cpType, const std::string& propertyId, const std::string& tableId, SceneProject* sceneProject, const std::vector<Entity>& entities);
        // Shared "Shader" row for components that support forking a built-in shader
        // (Mesh/UI/Points/Lines/Sky): fork, edit files, open .vert/.frag, reset to built-in,
        // plus .vert/.frag drag-drop. The .vert/.frag entry points may share a base name or
        // live in separate files (see Util::resolveCustomShaderPaths).
        void drawCustomShaderRow(ComponentType cpType, ShaderType shaderType, SceneProject* sceneProject, std::vector<Entity> entities);
        // one shader property of that row (customShader, or a mesh's customDepthShader)
        void drawComponentShaderRow(ComponentType cpType, ShaderType shaderType, SceneProject* sceneProject, Entity entity, const char* propertyName, const std::string& idSuffix);
        // Custom block members of the component's loaded shader (own or scene default
        // fork), one per name with the vertex declaration first.
        struct ShaderUniformRows {
            std::vector<ShaderUniform> members;
            std::vector<std::string> warnings;
            bool buildFailed = false; // the fork failed and the built-in is rendering
        };
        ShaderUniformRows resolveShaderUniformRows(ComponentType cpType, SceneProject* sceneProject, Entity entity);
        // rows under the Shader row, stored by name in shaderUniforms
        void drawShaderUniformRows(ComponentType cpType, SceneProject* sceneProject, Entity entity, const ShaderUniformRows& rows);
        // one member row: a label for engine-written and non-editable members, else a
        // drag field; true with newValue when it changed
        bool drawShaderUniformRow(const ShaderUniform& uniform, const ShaderUniformValues& values, const std::string& idPrefix, Vector4& newValue);
        void drawShaderUniformWarnings(const std::vector<std::string>& warnings);
        // ---- shader library (InspectorShaderLibrary.cpp) ----
        // The Shader picker: the built-in shader, the library shaders of `type` grouped by
        // @category (with @description tooltips) and "Custom file..." (the Edit Shader Files
        // popup named popupName, which the caller draws)
        // builtInLabel names the empty entry instead ("None" for an extra pass)
        void drawLibraryShaderCombo(const std::string& currentShader, ShaderType type,
                                    const std::function<void(const std::string&)>& setShader, const std::string& popupName,
                                    const char* builtInLabel = nullptr);
        // The manifest rows of `shader` for one GameObject, into the current table: feature
        // checkboxes, keyword combos, parameters (colour pickers editing sRGB, sliders for
        // ranges, checkboxes for toggles, drag fields) and texture slots, hiding the ones
        // behind a feature that is off; each resets to the manifest default. They edit the
        // collection properties given. False when the shader has no manifest.
        bool drawShaderManifestRows(ComponentType cpType, SceneProject* sceneProject, Entity entity, const std::string& shader,
                                    const std::string& paramsProperty, const std::string& texturesProperty,
                                    const std::string& featuresProperty, const std::function<void()>& onValueChanged);
        // The value widget of one manifest parameter (@property) at `value`, full width: colour
        // pickers editing sRGB (stored linear), sliders for ranges, checkboxes for toggles, drag
        // fields. True with `edited` when it changed; `continuous` false for a one-click change
        // (its own undo step) and true while dragging (merged until released)
        bool drawShaderPropertyValue(const ShaderManifestProperty& property, const std::string& id, const Vector4& value,
                                     Vector4& edited, bool& continuous);
        // A submesh material's Shader row and manifest rows; true when the built-in PBR rows
        // apply (built-in Lit or a fork)
        bool drawMaterialShaderRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                    unsigned int submesh, const RowSettings& settingsMaterial);
        // A submesh material's Surface (blend, cull, depth write and test, depth offset, sort
        // priority) and Lighting rows; Auto shows what it resolves to
        void drawMaterialSurfaceRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                     unsigned int submesh, const RowSettings& settingsMaterial);
        // UI/Points/LineRenderer blend mode, depth write, stencil and colour write rows, into the current table
        void drawComponentStateRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities);
        // ---- shader library phase 2 (docs/superpowers/specs/2026-10-10-shader-library-phase2-design.md)
        // The Stencil rows (the test, with Auto naming the rule it resolves to; ref, masks and
        // ops while it is explicit) and the Color Write row (Auto, or RGBA checkboxes) of a
        // material (prefix "submeshes[N].material."), its extra pass ("...extraPass.") or a
        // component (""), into the current table. resolveAuto: an entity's state with both on
        // Auto. An edit changes only what it names on each selected GameObject.
        void drawStencilColorWriteRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                       const std::string& prefix, const std::function<RenderState(Entity)>& resolveAuto,
                                       const RowSettings& settings);
        // A submesh material's Extra pass section: its shader picker ("None" = no pass), its
        // manifest rows, order, render state, stencil and colour write
        void drawMaterialExtraPassRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                       unsigned int submesh, const RowSettings& settingsMaterial);
        // The Preset row of a submesh material: a menu of getMaterialPresets(), one applied to
        // every selected submesh as one undo step
        void drawMaterialPresetRow(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                   unsigned int submesh, const RowSettings& settingsMaterial);

        // Scene-level variant: edits a scene default shader property (e.g. "default_mesh_shader"),
        // used by every component of that type whose customShader is empty.
        void drawSceneShaderRow(SceneProject* sceneProject, ShaderType shaderType, const char* scenePropertyName, const char* label);

        // Ordered user post-process chain: per-pass enable, shader fork/pick, reorder
        // and remove. Uniform rows come from the fork's own u_fs_postParams block.
        void drawScenePostProcess(SceneProject* sceneProject);
        // A pass's resolved shader. The pool lookup takes a lock, so it is done once per
        // pass per frame and shared by the label column and the uniform rows.
        struct PostProcessShader {
            std::shared_ptr<ShaderRender> shader;
            CustomUniformBlock block; // u_fs_postParams, slot -1 when not declared
            bool buildFailed = false;
            // the shader's manifest (library shaders, forks keeping theirs): the members it
            // declares as @property rows show its labels, ranges and defaults
            std::shared_ptr<const ShaderManifest> manifest;
        };
        PostProcessShader resolvePostProcessShader(const PostProcessPass& pass);
        float getPostProcessLabelSize(const PostProcessShader& resolved, const PostProcessPass& pass);
        void drawPostProcessUniforms(uint32_t sceneId, const std::vector<PostProcessPass>& passes, size_t index,
                                     const PostProcessShader& resolved);
        // the user pass's own fields beyond shader, enabled and uniforms (InspectorPostProcessing.cpp)
        void drawPostProcessPassFields(SceneProject* sceneProject, const std::vector<PostProcessPass>& passes, size_t index);

        // The engine's post-processing (PostProcessingSettings, InspectorPostProcessing.cpp): effects
        // by group with enable checkboxes and cost tiers, presets, the estimated cost readout
        void drawScenePostProcessing(SceneProject* sceneProject);
        void drawPostProcessingFieldRow(SceneProject* sceneProject, const PostProcessingSettings& settings,
                                        const PostProcessingSettings::Field& field, const char* label,
                                        const std::string& help, bool indent);
        void drawPostProcessingCost(SceneProject* sceneProject, const PostProcessingSettings& settings);

        // Commits a planned fork as one undoable step and opens the new .vert/.frag in the
        // code editor. Alerts instead when the plan is invalid or the files cannot be written.
        void commitShaderFork(uint32_t sceneId, std::unique_ptr<ForkShaderCmd> forkCmd);

        // Interaction core shared by the entity and scene shader rows; draws into the current
        // table value cell. setShader issues the undoable command; onFork opens the modal
        // that selects the destination and exact shader name before files are written.
        // idSuffix uniquifies ImGui IDs and the edit-files popup within the window.
        void drawShaderRowContents(const std::string& currentShader,
                                   const std::function<void(const std::string&)>& setShader,
                                   const std::function<void()>& onFork,
                                   const std::string& idSuffix);
        void drawShaderFilesPopup(const std::string& popupName, const std::function<void(const std::string&)>& setShader, const std::string& currentVert, const std::string& currentFrag);
        void drawTransform(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawMeshComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawModelComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawUIComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawButtonComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawScrollbarComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawProgressbarComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawInputFieldComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawPanelComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawTextComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawRectTransformComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLayoutGroupComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawImageComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawSpriteComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawTilemapComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawTerrainComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLightComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLight2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawShadowCaster2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawFogComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawMirrorComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLocalVolumetricFogComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawVolumetricCloudsComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // the Presets menu of a Fog or Volumetric Clouds: a pick applies it as one undo step
        void drawAtmospherePresetsMenu(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                       const std::vector<AtmospherePreset>& presets, const char* current);
        // a foldout of a component's rows (an ImGui tree node, open by default)
        bool beginFoldout(const char* label);
        void endFoldout();
        void drawReflectionProbeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawUIDocumentComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawCameraComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawAudioComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawSkyboxComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawParticleSystemComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLineRendererComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawPolygonComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawMeshPolygonComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawPointsComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawInstancedMeshComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawScriptComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawRigidbody2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawRigidbodyComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawJoint2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawJoint3DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawActionComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawPositionTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawRotationTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawScaleTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawColorTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawAlphaTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawSpriteAnimationComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawAnimationComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawPrefabComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawBoneComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawKeyframeTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // onRemoveValue/onAddValue let the caller add companion property commands
        // (same undo step) when an entry is deleted or appended, e.g. keyframe
        // easings or cubic-spline tangents
        template<typename Component, typename ValueType>
        void drawTrackValues(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities, RowPropertyType rowType, const ValueType& defaultNewValue, const char* idPrefix, std::vector<ValueType> Component::*memberPtr, const char* propertyName,
                             const std::function<void(MultiPropertyCmd*, Entity, Component*, size_t)>& onRemoveValue = nullptr,
                             const std::function<void(MultiPropertyCmd*, Entity, Component*)>& onAddValue = nullptr);
        void drawTranslateTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawRotateTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawScaleTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawMorphTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);

    public:
        static constexpr const char* WINDOW_NAME = "Inspector";

        InspectorWindow(Project* project);

        void setSceneWindow(SceneWindow* sceneWindow);

        void show();
        void stopTransientPreviews();
        void clearSceneState(uint32_t sceneId);
        void flushDirtyMaterialsForScene(uint32_t sceneId);
        void setOpen(bool open);
        bool isOpen() const;
        void focusNameInput();
        // Component > Add... (main menu): opens the Add Component dialog for the selection
        void requestAddComponent();
        // true once after Add Tag... or Add Layer...: the App opens Project Settings on Tags and Layers
        bool consumeTagsAndLayersRequest();
    private:
        bool addComponentRequested = false;
    };

}
