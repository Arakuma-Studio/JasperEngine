// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "util/NumericExpression.h"

#include "Project.h"
#include "render/Render.h"
#include "shader/ShaderManifest.h"
#include "render/AtmospherePresets.h"
#include "render/preview/MeshPreviewRender.h"
#include "render/preview/DirectionRender.h"
#include "render/preview/ParticlePreviewRender.h"
#include "util/PrimitiveParameters.h"
#include "lattice/LatticePresets.h"
#include "window/dialog/ScriptCreateDialog.h"
#include "window/dialog/ShaderForkDialog.h"
#include "window/TextureImporterView.h"
#include "window/ModelImporterView.h"
#include "window/dialog/ComponentAddDialog.h"
#include "window/dialog/TextureSlicerToolDialog.h"
#include "vcs/scene/SceneDiff.h"
#include "animation/ClipRecordHooks.h"

#include "imgui.h"
#include "yaml-cpp/yaml.h"
#include "animation/ActionPreviewSnapshot.h"
#include "ecs/Signature.h"
#include "subsystem/PhysicsSystem.h"
#include "soloud.h"
#include "soloud_wav.h"

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include "render/preview/MaterialRender.h"
#include "component/AudioSourceComponent.h"
#include "component/TextComponent.h"
#include "io/watch/FileIdentity.h"
#include "render/ShaderRender.h"
#include "subsystem/MeshSystem.h"

namespace jasperengine{
    struct ImageComponent;
    struct MeshComponent;
}

namespace jasperengine::particles {
    struct ParticleEffectDocument;
}
#include <tuple>

namespace jasperengine::editor{

    namespace reflect {
        struct EnumDesc;
        struct FieldDesc;
    }


    class SceneWindow;
    class CommandHistory;
    class ForkShaderCmd;
    class MultiPropertyCmd;
    class AnimationClipDocument;
    class AnimatorAssetDocument;
    struct InspectorClipState;
    struct InspectorClipStateDeleter{
        void operator()(InspectorClipState* state) const;
    };
    // the Animation Player layers' Inspector state (InspectorWindowAnimLayers.cpp)
    struct InspectorLayerState;
    struct InspectorLayerStateDeleter{
        void operator()(InspectorLayerState* state) const;
    };

    struct InspectorVatState;
    struct InspectorVatStateDeleter{
        void operator()(InspectorVatState* state) const;
    };

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
        Asset,      // an asset file reference (AssetRef): path, drop target, missing state
        LayerMask,  // unsigned int culling mask: Unity's Nothing / Everything / per-layer dropdown
        Curve,      // an animation curve: the Curve field (S27, widget/CurveField.h)
        Gradient    // a colour gradient: the Gradient field (S27, widget/GradientField.h)
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
        // A clamped slider for the float rows (Float, FloatPositive, Float_0_1) instead of the
        // drag field: used when floatMin < floatMax (drag and typed entry stay in range);
        // logarithmic for a range across magnitudes (e.g. Stochastic Tiling's Cell Size 0.25-4)
        float floatMin = 0.0f;
        float floatMax = 0.0f;
        bool logarithmic = false;
        bool child = false;
        // the label indented one step: a setting that belongs to the row above it
        bool indent = false;
        std::string help = "";
        const char *format = "%.2f";
        // MinMaxCurve rows: what the numbers show is the stored value times this (57.29578:
        // degrees of a value stored in radians)
        float displayScale = 1.0f;
        bool showColors = true;
        // Color3L / Color4L (S27 R1): HDR (an Intensity in the Color window), and a colour stored
        // as shown (S08 ColorSRGB) instead of linear
        bool colorHdr = false;
        bool colorSrgb = false;
        // Curve: the field's view of values (time 0 to 1)
        float curveValueMin = 0.0f;
        float curveValueMax = 1.0f;
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
        // Texture rows: one more square button after the row's own (the Normal Texture's
        // Sprite Normal Map wand), drawn with the rows' button size
        std::function<void(const ImVec2&)> extraButton = nullptr;
        // String rows: a dropdown of these values beside the field (free text still allowed),
        // e.g. the project's Input actions
        const std::vector<std::string>* stringSuggestions = nullptr;
        const char* suggestionsEmpty = nullptr;   // shown in the dropdown when there are none
        // What a number typed into the row's fields is in (25cm on a Meters field), S23 R25
        NumericUnit unit = NumericUnit::None;
    };

    // One row the Inspector drew (get_inspector_layout, docs/specs/S08-reflection.md R28): the
    // property it edits, its label and widget, the group heading it sits under, whether the
    // selection's values differ, and whether it is read-only (drawn disabled) or hidden
    struct InspectorLayoutRow{
        ComponentType component = ComponentType::Transform;
        std::string path;
        std::string label;
        std::string kind;
        std::string group;
        bool mixed = false;
        bool readOnly = false;
        bool visible = true;
    };

    // What one frame of the Inspector drew for the selection
    struct InspectorLayout{
        int frame = -1;     // ImGui frame count
        uint32_t sceneId = 0;
        std::vector<Entity> entities;
        std::vector<InspectorLayoutRow> rows;
    };

    class InspectorWindow{
    private:
        // the Project window's texture files: their import settings (S14)
        std::unique_ptr<TextureImporterView> textureImporter;
        // and its model files (S53)
        std::unique_ptr<ModelImporterView> modelImporter;
        Project* project;
        SceneWindow* sceneWindow;
        Command* cmd;
        // the split bar drag's undo step, merged while the handle is held
        Command* splitBarCmd = nullptr;
        // why the last Fit To Skeleton failed, and on which GameObject (cleared by the next fit)
        std::string capsuleFitError;
        Entity capsuleFitErrorEntity = NULL_ENTITY;
        // Commands that can run scripts at once while playing (a GameObject's Active and a
        // script's enabled checkbox: onEnable / onDisable, which may instantiate or destroy and
        // move the component arrays): run at the start of the next show(), when the Inspector
        // holds no reference into the scene
        std::vector<std::function<void()>> deferredCommands;
        void runDeferredCommands();

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

        // the scene whose history a Blend Shapes slider drag merges into (finishMorphEdit marks
        // it: no command pointer is kept across frames), 0 when none
        uint32_t morphEditScene = 0;

        // Lattice inspector state (edits apply through LatticeEditorOps when a field is let go)
        int latticeResolutionPending[3] = {3, 3, 3};
        int latticeResolutionAxis = -1;      // axis field being edited, -1 none
        Vector3 latticeOffsetPending;
        bool latticeOffsetEditing = false;
        lattice::PresetParams latticePreset;
        char latticePoseName[128] = "";
        std::string latticePoseRenaming;     // pose whose Rename popup is open
        bool latticePosesExpanded = true;
        bool latticeBindingsExpanded = true;
        // folded cells of the shown lattice, kept while its points and settings stay the same
        struct LatticeFoldCache {
            uint32_t sceneId = 0;
            Entity entity = NULL_ENTITY;
            uint64_t key = 0;
            std::vector<std::array<int, 3>> cells;
        };
        LatticeFoldCache latticeFoldCache;

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
        // the authored state of what the action preview writes; values commands write meanwhile
        // are kept (runtime-core spec 6.6)
        ActionPreviewSnapshot actionPreviewSnapshot;

        // PreviewOwner tokens of the action preview's roots (animation clips 9.R7): acquiring them
        // stops the Animation window or Pose window preview of the same model first
        std::vector<int> actionPreviewOwners;
        void releaseActionPreviewOwners();

        void startActionPreview(Entity entity, Scene* scene, SceneProject* sceneProject);
        void stopActionPreview(Scene* scene, SceneProject* sceneProject);
        void stopActionPreviewIfActive();

        // ---- animation clips (InspectorWindowClips.cpp; spec 4.2, 9.R30) ----
        // caches, popup state and error lines of the clip sections (defined in
        // InspectorWindowClips.cpp, so this header needs none of the clip headers)
        std::unique_ptr<InspectorClipState, InspectorClipStateDeleter> clipStatePtr;
        InspectorClipState& clipState();
        // Unity's activeObject: a programmatic selection change (MCP, undo) after the Project
        // window set an asset shows the selection again; user clicks clear it at their sites
        void trackInspectedObject();
        // ProjectWindow::revealAsset (InspectorWindow.cpp, which has the App)
        void revealAssetInProject(const std::filesystem::path& absolutePath);

        void setPlayerClipEntry(ComponentType cpType, SceneProject* sceneProject, Entity entity, size_t index, const std::string& stored);
        void removePlayerClipEntry(ComponentType cpType, SceneProject* sceneProject, Entity entity, size_t index);
        bool usePlayerClipFiles(ComponentType cpType, SceneProject* sceneProject, Entity entity, size_t index, const std::vector<std::string>& files);
        void drawAnimationPlayerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawAnimatorComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawAnimationPlayerClips(ComponentType cpType, SceneProject* sceneProject, Entity entity);
        void drawPlayerClipNameRow(ComponentType cpType, SceneProject* sceneProject, Entity entity, size_t index);
        void drawPlayerDefaultClipRow(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities);
        void drawAnimationPlayerBindings(SceneProject* sceneProject, Entity entity);
        void drawAnimationPlayerStates(ComponentType cpType, SceneProject* sceneProject, Entity entity);
        // the inspected .anim (or model with clips): true when it drew the window's contents
        bool drawInspectedAsset();
        void drawAssetHeader(const char* icon, const std::filesystem::path& asset, const char* kind);
        void drawClipAsset(const std::filesystem::path& asset, const std::string& reference);
        // the Animator assets (InspectorAnimator.cpp): .controller, .mask, .montage
        bool drawAnimatorAsset(const std::filesystem::path& asset);
        void drawControllerAsset(const std::shared_ptr<AnimatorAssetDocument>& document);
        void drawMaskAsset(const std::shared_ptr<AnimatorAssetDocument>& document);
        void drawMontageAsset(const std::shared_ptr<AnimatorAssetDocument>& document);
        void drawModelClipsAsset(const std::filesystem::path& asset, const std::string& model);
        void drawClipDocument(AnimationClipDocument& document, const std::string& reference);
        // the Model section: the clip buttons of an entity animation row that stands for a
        // sub-clip, and the rows of the sub-clips no entity row shows
        float modelRowClipButtonsWidth() const;
        void drawModelRowClipButtons(SceneProject* sceneProject, Entity model, const std::string& subClip, const std::string& entityName);
        void drawModelClipRows(ComponentType cpType, SceneProject* sceneProject, Entity entity, const std::vector<std::string>& shownInRows);
        std::vector<std::string> modelSubClipNames(const std::string& model);
        void drawAnimationClipButtons(SceneProject* sceneProject, Entity entity);

        // ---- Animation Player layers (InspectorWindowAnimLayers.cpp; upstream anim layers spec 9,
        // docs/specs/S21-animation.md "Upstream absorption") ----
        std::unique_ptr<InspectorLayerState, InspectorLayerStateDeleter> layerStatePtr;
        InspectorLayerState& layerState();
        // the player's Animator Controller row: Create Animator Controller from Clips (an
        // Animator plays it, the player hands over), or what a handing-over player plays
        void drawAnimationPlayerController(ComponentType cpType, SceneProject* sceneProject, Entity entity);
        // the Layers table (one scene step per edit, PlayerLayerActions)
        void drawAnimationPlayerLayers(ComponentType cpType, SceneProject* sceneProject, Entity entity);
        void setPlayerLayerMask(SceneProject* sceneProject, Entity entity, int layer, const std::string& mask);
        // a clip entry's Layer combo
        void drawPlayerClipLayerRow(ComponentType cpType, SceneProject* sceneProject, Entity entity, size_t index);
        // Bindings: "mask: N/M bound entities covered" per masked layer, the masks' warnings
        void drawPlayerMaskCoverage(SceneProject* sceneProject, Entity entity);
        // play mode: a layer's header in the States panel (live weight slider)
        void drawPlayerLayerStatesHeader(SceneProject* sceneProject, Entity entity, int layer);
        // the mask asset view's Users (players' and controllers' layers) and a controller's
        // Users (Animators), cached per path
        void drawAnimatorAssetUsers(const std::string& projectPath, bool mask);

        // ---- vertex animation (InspectorWindowVat.cpp; vertex animation spec 3.6, 5.2, 5.4) ----
        std::unique_ptr<InspectorVatState, InspectorVatStateDeleter> vatStatePtr;
        InspectorVatState& vatState();
        void drawVertexAnimatorComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // the Animation field: a .vat (or its .vat.glb) as the component's path + GUID, one step
        // the Vertex Animator's Animation of `entities` (one undo step): the path and its GUID
        void setVertexAnimatorFile(SceneProject* sceneProject, const std::vector<Entity>& entities, const std::string& file);
        void drawVertexAnimatorFileField(SceneProject* sceneProject, const std::vector<Entity>& entities, const std::string& animation,
                                         bool mixed, bool stopped);
        // Preview: the GameObject plays in edit mode (PreviewOwner); one at a time
        void startVertexAnimatorPreview(SceneProject* sceneProject, Entity entity);
        void stopVertexAnimatorPreview();
        void syncVertexAnimatorPreview(SceneProject* shown, const std::vector<Entity>& selection);
        // a closed scene: its preview is forgotten (the scene is gone, nothing to put back)
        void clearVertexAnimatorPreviewScene(uint32_t sceneId);
        // the inspected .vat (Project window click)
        void drawVatAsset(const std::filesystem::path& asset);
        // InspectorWindowCamera.cpp (camera shake spec 9)
        void drawCameraShakerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawCameraFollowComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawShakePresetAsset(const std::filesystem::path& asset);
        // the Test strip's edit-mode preview stops once its GameObject (or .shake) is not shown
        void syncCameraShakePreview(SceneProject* shown, const std::vector<Entity>& selection);
        // a selected .mixer: groups, exposed parameters, snapshots; Use as Project Mixer, Open in Audio Mixer
        void drawAudioMixerAsset(const std::filesystem::path& asset);
        // a selected .soundevent: its editor, problems, Match Levels and Audition (InspectorSoundEvent.cpp)
        void drawSoundEventAsset(const std::filesystem::path& asset);
        // AudioSource: the Event, Play On, impact and Variation rows and their warnings
        void drawAudioEventRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities);
        // an audio file: its measurements, import settings, provenance and audition (InspectorAudioClip.cpp)
        void drawAudioClipAsset(const std::filesystem::path& asset);
        // a .physicsmaterial: friction, bounciness and their combine modes (InspectorPhysics2D.cpp)
        void drawPhysicsMaterialAsset(const std::filesystem::path& asset);
        // a .physicsmaterial path field (drop from the Project window, pick, clear) with undo
        bool physicsMaterialRow(ComponentType cpType, const std::string& property, const std::string& label,
                                SceneProject* sceneProject, const std::vector<Entity>& entities, const std::string& help = "");
        void drawCreateCrowdDialog();
        // Sprite normals (InspectorWindowSpriteNormals.cpp): a generated normal map (or its source
        // image) clicked in the Project window; false for any other image (the scene selection
        // stays). The Normal Texture rows' settings with the Sprite Normal Map wand added when the
        // submesh's Base Texture is an image file.
        bool drawSpriteNormalsAsset(const std::filesystem::path& asset);
        RowSettings normalTextureRowSettings(const RowSettings& base, SceneProject* sceneProject, const std::vector<Entity>& entities, unsigned int submesh);
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
        // the last component header row was clicked (not its checkbox or buttons)
        static bool lastHeaderRowClicked();
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
        // a texture slot showing a FluidSimulation's output ("fluid|<entity>", FluidTextureLink)
        void applyFluidTexture(Entity fluidEntity, ComponentType cpType, const std::string& id, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged);
        void applyVideoTexture(Entity videoEntity, ComponentType cpType, const std::string& id, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged);
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
    public:
        // The overridden property names a row's Apply/Revert covers: the row's property, the
        // ones inside it ("clips[0]" covers "clips[0].clip" and its "clips[0].guid") and the one it
        // is part of
        static std::vector<std::string> matchingPrefabOverrides(const std::set<std::string>& names, const std::string& property);
    private:
        bool isPrefabPropertyOverridden(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType type, const std::string& property) const;
        // Unity's blue bar at the Inspector's left margin, from y0 to y1
        void drawPrefabOverrideBar(float y0, float y1);
        // The Apply / Revert entries for a property, a component or GameObject state, into the
        // menu being drawn; stateBits for state (type ignored), property empty for a whole component
        void drawPrefabApplyRevertItems(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType type,
            const std::string& property, uint8_t stateBits, bool readOnly);
        // A property row's label: bold with the bar when overridden, right click for the menu
        void drawPrefabPropertyLabel(const std::string& label);
        // S29 (InspectorVcs.cpp): the scene diff's markers on property rows and component menus,
        // and the lock banner (true: read only, someone else holds the lock)
        std::string vcsDiffPathOfRow(const vcs::scene::ComponentDiff& component, const std::string& rowId) const;
        const vcs::scene::PropertyDiff* findVcsPropertyDiff(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType type,
                                                            const std::string& rowId, const vcs::scene::ObjectDiff** object,
                                                            const vcs::scene::ComponentDiff** component) const;
        void drawVcsBaselineItems(SceneProject* sceneProject, const vcs::scene::ObjectDiff& object, const vcs::scene::ComponentDiff* component,
                                  const vcs::scene::PropertyDiff* property, bool readOnly);
        bool drawVcsPropertyMarker(const std::string& rowId, const ImVec2& cellMin, const ImVec2& cellMax, bool openMenu);
        void drawVcsComponentMenuItems(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType type, bool readOnly);
        bool drawVcsLockBanner(const std::vector<std::filesystem::path>& files);
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
        // a parameter row's link popup asked for Project Settings > Graphics (Shader Globals)
        bool shaderGlobalsRequested = false;
        // layer picked for GameObjects with children, waiting for the Change Layer answer
        uint32_t pendingLayerSceneId = NULL_PROJECT_SCENE;
        std::vector<Entity> pendingLayerEntities;
        int pendingLayer = 0;

        bool canAddComponent(SceneProject* sceneProject, Entity entity, ComponentType cpType);
        void addComponents(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType cpType);

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
        bool propertyHeader(std::string label, float secondColSize = -1, bool defChanged = false, bool child = false, bool indent = false);
        bool propertyRow(RowPropertyType type, ComponentType cpType, std::string id, std::string label, SceneProject* sceneProject, std::vector<Entity> entities, RowSettings settings = RowSettings());
        // Play mode, under a Rigidbody (2D): the pairs it touches and the contact callbacks its
        // scripts define (physics-contacts 6.3)
        void drawPhysicsContacts(SceneProject* sceneProject, Entity entity, bool is2D);
        struct PhysicsContactsView{
            double readAt = -1.0;
            std::vector<ContactSummary> contacts;
            std::vector<std::string> callbacks;
        };
        std::map<std::tuple<uint32_t, Entity, bool>, PhysicsContactsView> physicsContactsViews;
        bool propertyRowWithAutoButton(RowPropertyType propType, ComponentType cpType, std::string id, std::string label, std::string autoId, std::string autoLabel, SceneProject* sceneProject, std::vector<Entity> entities, RowSettings settings = RowSettings());
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
        // reserveRight: room left after the field (a link button drawn on the same line)
        bool drawShaderUniformRow(const ShaderUniform& uniform, const ShaderUniformValues& values, const std::string& idPrefix, Vector4& newValue,
                                  float reserveRight = 0.0f);
        // Under a Stochastic Tiling Rotation row (a material's, a terrain layer's): when the rotation is
        // on and the normal map's average normal leans (util/NormalMapTilt), a warning and a button that
        // sets rotationProperty to 0 on every entity in one undo step
        void drawStochasticRotationWarning(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                           const std::string& rotationProperty, float rotation, const Texture& normalTexture,
                                           const std::function<void()>& onValueChanged);
        void drawShaderUniformWarnings(const std::vector<std::string>& warnings);
        // ---- global shader values (shader globals spec 6.6, InspectorShaderLibrary.cpp) ----
        // The link button closing a shader parameter row's value cell (on the same line, after
        // it): its popup binds the member to a global of its kind (components: the member's, 1-4; a
        // colour and a vector4 take each other) or unbinds it, through `set` (one undo step)
        void drawGlobalLinkButton(const std::string& member, int components, const ShaderGlobalBindings& bindings,
                                  const std::function<void(const ShaderGlobalBindings&)>& set);
        // The value cell of a bound row: @Global and the global's current value, read-only, in
        // `width` (the cell less the link button)
        void drawBoundGlobalValue(const std::string& global, int components, bool color, float width);
        float getGlobalLinkButtonWidth() const;
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
                                     Vector4& edited, bool& continuous, float itemWidth = -1.0f);
        // A submesh material's Shader row and manifest rows; true when the built-in PBR rows
        // apply (built-in Lit or a fork)
        bool drawMaterialShaderRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                    unsigned int submesh, const RowSettings& settingsMaterial);
        // A Light's Intensity row with its unit (Engine, Lux, Candela, Lumen by type; a change
        // converts the value so the light stays as bright), and its colour temperature rows (Use
        // Color Temperature, Temperature with the black body gradient and Unity's presets)
        void drawLightIntensityRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                    const RowSettings& settingsFloat);
        void drawLightTemperatureRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities);
        // The scene's Physical Light Exposure (EV100) row and its presets (Lighting settings)
        void drawPhysicalLightExposureRow(SceneProject* sceneProject);
        // A built-in Lit material's Advanced Surface rows (InspectorMaterialPbr.cpp): a header per
        // extended lobe with its checkbox, its values and maps, and the variant footer (its
        // codes, Compiling/Ready, extension maps n / 3, the maps left out)
        void drawMaterialAdvancedSurfaceRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                             unsigned int submesh, const RowSettings& settingsMaterial);
        // the value and map rows of one extended lobe under prefix ("submeshes[N].material.pbr."), into the current table
        void drawMaterialPbrLobeRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                     const std::string& prefix, PbrLobe lobe, const RowSettings& settingsMaterial);
        // the built-in Lit's Shading Model row (Standard / Hair: the hair lobe's enabled flag, one
        // undo step) and the hair rows while it is Hair, into the current table
        void drawMaterialShadingModelRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                          unsigned int submesh, const RowSettings& settingsMaterial);
        // A submesh material's Surface (blend, cull, depth write and test, depth offset, sort
        // priority) and Lighting rows; Auto shows what it resolves to
        // (and, with litRows, the built-in Lit's Shading Model)
        void drawMaterialSurfaceRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                     unsigned int submesh, const RowSettings& settingsMaterial, bool litRows);
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

        // ---- Fluid Simulation (InspectorFluid.cpp): Post-processing-style groups, presets, the
        // preview bar (FluidSystem::setPreview), the Scene view's Paint Dye toggle and the stats
        // readOnly: the property rows disabled, the preview bar and the stats still work
        void drawFluidSimulationComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities, bool readOnly);
        // InspectorVideo.cpp: the Video Player (Unity's order), its codec support per platform and
        // its playback in Play; the asset view of a video file (probe, platform matrix, fast start,
        // a preview)
        void drawVideoPlayerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities, bool readOnly);
        void drawVideoAsset(const std::filesystem::path& asset);
        // the asset preview's decoder goes once the asset is not shown any more
        void closeIdleVideoPreview();
        // why the fluid draws nothing (device, simulator error, no surface for Material)
        void drawFluidStatus(SceneProject* sceneProject, const std::vector<Entity>& entities);
        void drawFluidPreviewBar(SceneProject* sceneProject, const std::vector<Entity>& entities);
        void drawFluidStats(SceneProject* sceneProject, const std::vector<Entity>& entities);
        // a fluid row's label cell (prefab override marker, help tooltip, reset arrow; true when
        // the arrow is clicked), leaving the value cell current at full width
        bool fluidRowHeader(const std::string& property, const std::string& label, const std::string& help, bool defChanged, bool indent);
        // a value for every selected fluid: merged while the widget is held, else one step
        template<typename T>
        void commitFluidProperty(SceneProject* sceneProject, const std::vector<Entity>& entities, const std::string& property,
                                 const T& value, bool merge);
        // a FluidPreset's fields on every selected fluid, one undo step
        void applyFluidPreset(SceneProject* sceneProject, const std::vector<Entity>& entities, const std::string& presetId);

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
        // ---- the default Inspector (InspectorWindow/InspectorDefault.cpp, docs/specs/S08-reflection.md R10)
        // A component's own drawer, null when its reflection table draws it
        using ComponentDrawer = void (InspectorWindow::*)(ComponentType, SceneProject*, std::vector<Entity>);
        static ComponentDrawer componentDrawer(ComponentType cpType);
        // the rows of a component's table in its Inspector order, groups under their headings
        void drawDefaultComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // a struct field as a foldout of its members; an array field as a foldout of its elements
        // with move, remove and add buttons (ReflectedValueCmd)
        void drawDefaultStruct(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                               const reflect::FieldDesc& field, const std::string& path, const RowSettings& base);
        void drawDefaultArray(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                              const reflect::FieldDesc& field, const std::string& path, const RowSettings& base);
        // what a component drawn by default shows after its rows (a warning)
        void drawDefaultComponentNotes(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities);

        void drawTransform(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawMeshComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // the morph target weights of a mesh with blend shapes (names from its model)
        void drawBlendShapes(SceneProject* sceneProject, const std::vector<Entity>& entities);
        void drawModelComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // the Model's Rig foldout (anim-retarget 9): Animation Type, the Avatar's status, Configure
        // Avatar..., human scale (InspectorModelRig.cpp)
        void drawModelRig(ComponentType cpType, SceneProject* sceneProject, Entity entity);
        void drawUIComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawInputFieldComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawOnScreenStickComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawOnScreenButtonComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawTouchAreaComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawTextEffectComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLayoutGroupComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawCanvasGroupComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // "Inherited alpha 0.25 · not interactable · click-through (Canvas Group: Panel)" and a
        // Select button, when the entity is under a Canvas Group
        void drawCanvasGroupInheritedLine(SceneProject* sceneProject, Entity entity);
        void drawSafeAreaComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawAudioSnapshotComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawImageComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawSpriteComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawTilemapComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawTerrainComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLightComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // the light's Shadows section (InspectorShadows.cpp), and its pieces
        void drawLightShadows(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawShadowSplitBar(SceneProject* sceneProject, std::vector<Entity> entities, const LightComponent& light);
        // the scene's Shadows settings (Quality, distance, fade, blend, the effective tier)
        void drawSceneShadows(SceneProject* sceneProject);
        void drawShadowCaster2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // Cast Shadows, the capsule list (up to 8) and Fit To Skeleton, each an undo step
        void drawCapsuleShadowCasterComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawFogComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawVolumetricCloudsComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // the Presets menu of a Fog or Volumetric Clouds: a pick applies it as one undo step
        void drawAtmospherePresetsMenu(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                       const std::vector<AtmospherePreset>& presets, const char* current);
        // a foldout of a component's rows (an ImGui tree node, open by default)
        bool beginFoldout(const char* label);
        void endFoldout();
        // ImGui::SeparatorText that also names the group of the rows below it (the layout)
        void separatorText(const char* label);

        // ---- the layout of the frame being drawn (get_inspector_layout)
        InspectorLayout layoutDrawing;
        InspectorLayout layoutLast;
        // set while a component's body draws: its rows go into the layout
        bool layoutRecording = false;
        ComponentType layoutComponent = ComponentType::Transform;
        // the group headings open around the row being drawn: the component's separator, then
        // each foldout (a separator inside a foldout renames that level)
        std::vector<std::string> layoutGroups;
        void beginLayoutComponent(ComponentType cpType);
        void endLayoutComponent();
        void recordLayoutRow(RowPropertyType type, ComponentType cpType, const std::string& id, const std::string& label,
                             SceneProject* sceneProject, const std::vector<Entity>& entities, bool visible = true);
        void drawReflectionProbeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawUIDocumentComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawPoseAnimatorComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawCameraComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawAudioComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawSkyboxComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawParticleSystemComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawParticleExposedValues(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                       const particles::ParticleEffectDocument& document);
        void drawParticleCollisionPlanes(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities);
        void drawForceFieldComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // a particle edit's undo step, merging until the edit ends (InspectorParticles.cpp)
        void pushParticleCommand(CommandHistory* history, Command* command, bool commit);
        Command* particleEditCmd = nullptr;
        // a .particles asset picked in the Project window, shown until the GameObject selection
        // changes (Unity: the newest selection is what the Inspector shows)
        std::string inspectedParticleAsset;
        std::string lastProjectParticleFile;
        uint32_t inspectedParticleScene = 0;
        std::vector<Entity> inspectedParticleSelection;
        ParticlePreviewRender particleAssetPreview;
        bool updateInspectedParticleAsset(SceneProject* sceneProject, const std::vector<Entity>& entities);
        void drawParticleAssetInspector();
        // the curve and gradient fields whose keys are open, by field path
        std::set<std::string> particleExpanded;
        void drawLineRendererComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // Line and Trail Renderers (InspectorLinesTrails.cpp)
        void drawLineSegmentsList(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLinePositionsList(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLineRibbonRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities, bool hairline, bool gradient);
        void drawLineTextureRow(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities);
        void drawTrailRendererComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawPolygonComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawMeshPolygonComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawPointsComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawInstancedMeshComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawScriptComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawRigidbody2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawRigidbodyComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawJoint2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawCharacterController2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawCharacterControllerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawWaterVolumeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawBuoyancyComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // InspectorSplines.cpp (S18)
        void drawSplineComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawSplineExtrudeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawSplineInstantiateComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawSplineAnimateComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // the last spline terrain bake's or Bake to GameObjects' message
        std::string splineBakeMessage;
        void drawJoint3DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        // navigation (InspectorNavigation.cpp)
        void drawNavMeshSurfaceComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawNavMeshAgentComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawNavMeshObstacleComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawNavMeshLinkComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawNavMeshModifierComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawNavGridComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void navAreaMaskRow(ComponentType cpType, const std::string& id, const std::string& label, SceneProject* sceneProject,
                            std::vector<Entity> entities, const char* help);
        void drawRagdollComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawActionComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawPositionTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawRotationTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawScaleTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawColorTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawAlphaTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawSpriteAnimationComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawAnimationComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
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
        void drawLatticeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLatticeModifierComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLatticeFollowerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);
        void drawLatticeTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities);

    public:
        // a dropdown's entries from an enum table (its labels, in its Inspector order)
        static std::vector<EnumEntry>* enumEntries(const reflect::EnumDesc& desc, uint64_t mask = 0);
        // the power-of-two sizes of a size slider (shadow maps, probes)
        static std::vector<int> powerOfTwoSizes;

        // the commands soundEvent.audition and soundEvent.matchLevels, on the inspected .soundevent
        void requestSoundEventAudition();
        void requestSoundEventMatchLevels();
        // soundEvent.playAsMusic: the inspected event through the music system (S39 R18), or stops it
        void requestSoundEventPlayAsMusic();
        // the command audioClip.detectTempo, on the inspected audio file (InspectorAudioClip.cpp)
        void requestAudioClipDetectTempo();

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
        // Add Component's own path (the Search palette's components): one undo step per
        // GameObject of the selection; false with *why when it cannot be added to all of them
        bool canAddComponentToSelection(ComponentType type);
        bool addComponentToSelection(ComponentType type, std::string* why = nullptr);
        // true once after Add Tag... or Add Layer...: the App opens Project Settings on Tags and Layers
        bool consumeTagsAndLayersRequest();
        // true once after a link popup's "Shader Globals...": the App opens Project Settings on Graphics
        bool consumeShaderGlobalsRequest();

        // ---- Record sources (animation clips 9.R5; the rows and the window test hook call them) ----

        // A Transform position / rotation / scale row edit of `entities` (`valueOf`: an entity's
        // new value, x y z or rotation x y z w): when Record claims the channel for some of them,
        // the claimed ones are recorded (live value + key at the playhead, one merge key until the
        // row is let go: finishRecordRow) and the others stay as they are; false: Record does not
        // take it (the row builds its PropertyCmd)
        bool recordRowEdit(SceneProject* sceneProject, ComponentType cpType, const std::string& id,
                           const std::vector<Entity>& entities, const std::function<std::vector<float>(Entity)>& valueOf);
        // the row was let go (deactivated): its record gesture ends
        void finishRecordRow();
        // anim-properties: the animatable property a row edits on `entity` (a component's Catalog
        // property, "enabled", a Lua script's "scripts[N].<field>"), empty when none
        ClipPropertyKey rowPropertyKey(SceneProject* sceneProject, Entity entity, ComponentType cpType, const std::string& id) const;
        // a row's command of an animatable property: recorded (its values keyed at the playhead
        // and previewed, no scene step) when Record claims it for some entities, which deletes it
        // (cmd is cleared); else added to the scene's history
        void submitRowCommand(SceneProject* sceneProject, ComponentType cpType, const std::string& id);
        bool recordPropertyCommand(SceneProject* sceneProject, ComponentType cpType, const std::string& id, Command* command);
        // a checkbox of a property (GameObject active, a component's enabled) set to `value`:
        // recorded for the claimed entities when Record claims it, false when it does not
        bool recordPropertyToggle(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType cpType,
                                  const ClipPropertyKey& key, bool value);
        // The Blend Shapes rows (MeshComponent morph weights): weight `index` of `entities` set to
        // `value`; recorded when Record claims the channel, else one scene step per drag (the
        // command notifies the channel write listeners); finishMorphEdit when the slider is let go
        bool setMorphWeight(SceneProject* sceneProject, const std::vector<Entity>& entities, int index, float value);
        void finishMorphEdit();
        // The rows of the last frame the Inspector was drawn (get_inspector_layout)
        const InspectorLayout& getLastLayout() const;
        // "Float", "Color3L"...: the widget a row draws
        static const char* rowPropertyTypeName(RowPropertyType type);
        // The selection's values of a property differ (lanes of vectors and colours compared
        // one by one, floats within 1e-4, as the rows show them)
        static bool propertyValuesDiffer(SceneProject* sceneProject, ComponentType cpType, const std::string& id, const std::vector<Entity>& entities);
    private:
        bool addComponentRequested = false;
    };

}
