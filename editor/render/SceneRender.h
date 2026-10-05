// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "JasperEngine.h"

#include "ToolsLayer.h"
#include "UILayer.h"
#include "RenderUtil.h"
#include "command/Command.h"

#include <memory>

namespace jasperengine::editor{

    constexpr float DEFAULT_EDITOR_CAMERA_NEAR = 0.3f;
    constexpr float DEFAULT_EDITOR_CAMERA_FAR = 4000.0f;
    constexpr float MIN_EDITOR_CAMERA_DISTANCE = 0.2f;
    constexpr float MAX_EDITOR_CAMERA_DISTANCE = 2000.0f;

    constexpr float DEFAULT_EDITOR_WALK_SPEED = 10.0f;
    constexpr float MIN_EDITOR_WALK_SPEED = 0.5f;
    constexpr float MAX_EDITOR_WALK_SPEED = 1010.0f;

    struct SceneDisplaySettings {
        bool showAllJoints       = false;
        bool showAllBones        = false;
        bool showAllBodies       = false;
        bool hideCameraView      = false;
        bool hideLightIcons      = false;
        bool hideSoundIcons      = false;
        bool hideContainerGuides = false;
        bool showOrigin          = true;
        bool showGrid3D          = true;
        bool hideSelectionOutline = false;
        bool disableFaceCulling  = false;
        bool disableFog          = false;
        bool showGrid2D          = false;
        float gridSpacing2D      = 50.0f;
        float gridSpacing3D      = 1.0f;
        bool snapToGrid          = false;
        bool snapTile            = false;
        bool snapRotation        = false;
        float rotationSnapDegrees = 15.0f;
    };

    struct CameraObjects{
        Sprite* icon = nullptr;
        LineRenderer* lines = nullptr;

        CameraType type = CameraType::CAMERA_UI;
        bool isMainCamera = false;
        float yfov = 0;
        float aspect = 0;
        float nearClip = 0;
        float farClip = 0;
        float leftClip = 0;
        float rightClip = 0;
        float bottomClip = 0;
        float topClip = 0;
    };

    struct SoundObjects{
        Sprite* icon = nullptr;
    };

    struct Light2DObjects{
        Sprite* icon = nullptr;
    };

    class Project;

    class SceneRender{
    private:
        Plane cursorPlane;
        Vector3 rotationAxis;
        Vector3 gizmoStartPosition;
        Vector3 cursorStartOffset;
        Quaternion rotationStartOffset;
        std::map<Entity, Matrix4> objectMatrixOffset;
        std::map<Entity, Vector2> objectSizeOffset;
        // RectTransforms whose anchor preset places them: their positionOffset and position
        // when the drag began. Their layout puts the position back, so a move goes to the
        // positionOffset, as Unity's tools move a RectTransform's anchoredPosition.
        std::map<Entity, std::pair<Vector2, Vector3>> anchoredMoveStart;
        Vector2 getAnchoredPositionOffset(Scene* target, Entity entity, const Vector3& localPosition, const Vector2& size) const;

        Ray mouseRay;
        bool mouseClicked;
        bool useGlobalTransform;

        float gizmoScale;
        float selectionOffset;

        Command* lastCommand;

        CursorSelected cursorSelected;

        // Tile sub-selection within a tilemap entity
        Entity selectedTileEntity = 0;  // NULL_ENTITY
        int selectedTileIndex = -1;
        Vector2 tileStartPosition;
        float tileStartWidth = 0;
        float tileStartHeight = 0;

        // Instance sub-selection within an instanced mesh entity
        Entity selectedInstanceEntity = 0;  // NULL_ENTITY
        int selectedInstanceIndex = -1;

        // ShadowCaster2D polygon point sub-selection
        Entity selectedOccluderPointEntity = 0;  // NULL_ENTITY
        int selectedOccluderPointIndex = -1;

        // Lines endpoint sub-selection (index = lineIndex * 2 + endpoint, 0 = A, 1 = B)
        Entity selectedLinePointEntity = 0;  // NULL_ENTITY
        int selectedLinePointIndex = -1;

        // Polygon / MeshPolygon vertex sub-selection (both store std::vector<PolygonPoint>;
        // isMesh picks which component, avoiding a ComponentType dependency in this header)
        Entity selectedPolygonPointEntity = 0;  // NULL_ENTITY
        int selectedPolygonPointIndex = -1;
        bool selectedPolygonPointIsMesh = false;

        // TranslateTracks keyframe point sub-selection (the tracks entity has no
        // Transform; its values[] live in the action target's parent space)
        Entity selectedTrackPointEntity = 0;  // NULL_ENTITY
        int selectedTrackPointIndex = -1;

        // world position of vertex pointIndex of the selected entity's Polygon/MeshPolygon
        // points (nullptr list / bad index -> false)
        bool getPolygonPointWorld(Entity entity, bool isMesh, int pointIndex, Vector3& worldPoint);

        bool getTrackPointWorld(Entity entity, int pointIndex, Vector3& worldPoint);

        // Bounds of an entity of target: this scene, or the edit layer's scene
        AABB getAABB(Scene* target, Entity entity, bool local);
        AABB getFamilyAABB(Scene* target, Entity entity, float offset);

        // visual=true returns a shear-preserving box (parallelepiped) that hugs the
        // rendered geometry, for the selection outline / gizmo handles only.
        // visual=false (default) returns an orthonormal OBB, required by ray picking
        // (Ray::intersects) and OBB::enclose, which assume orthonormal axes.
        OBB getOBB(Scene* target, Entity entity, bool local, bool visual = false);
        OBB getFamilyVisualOBB(Scene* target, Entity entity, float offset);

        // A scene drawn over this one in the Scene view (an inline child scene, a running
        // game's layer scene) whose GameObjects the transform tools edit, see setEditLayer().
        // Its gizmos and outline live in a tools layer of their own, seen through the
        // layer's camera; this scene's tools layer keeps the icons of this scene.
        struct EditLayer{
            bool active = false;
            uint32_t sceneId = 0;
            Scene* scene = nullptr;
            LayerView view;
            std::vector<Entity> selection;
            std::vector<Entity> entities;
            bool running = false;
        };
        EditLayer editLayer;
        // [0] with 2D gizmos (orthographic layers), [1] with 3D gizmos; made on first use
        std::unique_ptr<ToolsLayer> layerTools[2];
        LineRenderer* layerSelLines[2] = {nullptr, nullptr};
        // activate() put the editor layers in the Engine, and which layer tools it added
        bool engineLayersActive = false;
        int engineLayerTools = -1;

        // the mouse through this scene's camera, for the terrain brush (mouseRay follows
        // the edit layer while one is set)
        Ray ownerMouseRay;

        Scene* gizmoScene() const;
        ToolsLayer& gizmoTools();
        const ToolsLayer& gizmoTools() const;
        int layerToolsIndex() const;
        ToolsLayer& getLayerTools(int index);
        void syncEngineLayerTools();
        Ray gizmoRay(float x, float y);
        Vector3 gizmoViewDirection() const;
        bool isOrthographicView(Scene* target) const;
        float targetBillboardScale(Scene* target, const Vector3& worldPos, float logicalScale) const;
        static OBB transformAABBPreservingShear(const Matrix4& modelMatrix, const AABB& localAABB);
        void updateTerrainBrushCursor();
        float snapRotationAngle(float angle, bool invertRotationSnap) const;
        static Vector3 getMatrixScale(const Matrix4& matrix);
        static bool isCorner2DGizmoSide(Gizmo2DSideSelected side);
        static bool gizmo2DSideUsesNegativeX(Gizmo2DSideSelected side);
        static bool gizmo2DSideUsesNegativeY(Gizmo2DSideSelected side);
        static Vector2 lockObject2DAspectRatio(const Vector2& startSize, const Vector2& candidateSize);
        bool isPreviewCameraUsable(Entity entity);
        Entity getActiveCameraEntity();
        void syncSceneCamera();

        // size the Scene view draws at, in engine pixels
        int viewWidth = 0;
        int viewHeight = 0;

    protected:
        // selection outline of boxes: their near face (2D views), or all twelve edges
        static void fillSelectionLines(LineRenderer* lines, const std::vector<OBB>& obbs, bool boxes);

        void updateCameraFrustum(CameraObjects& co, const CameraComponent& cameraComponent, bool isMainCamera, bool fixedSizeFrustum = true);
        void drawCameraFrustumLines(LineRenderer* lines, const CameraComponent& cameraComponent, bool isMainCamera, bool fixedSizeFrustum = true);
        void setupCameraIcon(CameraObjects& co);
        void setupSoundIcon(SoundObjects& so);
        void setupLight2DIcon(Light2DObjects& lo);

        float overlayPx(float logicalPixels) const { return logicalPixels * overlayScale; }
        void ensureOverlayLines(std::map<Entity, LineRenderer*>& lines, Entity entity);

        // Marks an editor gizmo or icon created in the scene it annotates: only the
        // Scene view draws it, never the game or a camera image
        template<typename T>
        T* overlay(T* object){
            scene->getSystem<RenderSystem>()->setEditorOverlay(object->getEntity(), true);
            return object;
        }

        // Editor-only scenes the Scene view draws over the scene and its layers
        // (gizmos, overlays), and the views they render into before it
        virtual void addOverlayLayers(std::vector<Engine::ViewLayer>& layers, int width, int height);
        virtual void drawOverlayTargets();

        Scene* scene;
        Camera* camera;
        Framebuffer framebuffer;
        Entity previewCameraEntity;
        // Set only while the Game view pass draws through a game camera
        Entity gameViewCameraEntity = NULL_ENTITY;

        LineRenderer* selLines;
        LineRenderer* terrainBrushLines;

        ToolsLayer toolslayer;
        UILayer uilayer;

        std::vector<Scene*> childSceneLayers;

        // Prefab Mode In Context: the scene drawn under this one through underlayCamera,
        // and a layer between them (the Gray context's veil)
        Scene* underlayScene = nullptr;
        Entity underlayCamera = NULL_ENTITY;
        Scene* underlayVeil = nullptr;

        bool multipleEntitiesSelected;
        bool isPlaying;

        SceneDisplaySettings displaySettings;

        float zoom;       // current zoom level (units per pixel) for 2D
        float overlayScale = 1.0f;

    public:

        SceneRender(Scene* scene, bool use2DGizmos, bool enable3DOverlays, float gizmoScale, float selectionOffset);
        virtual ~SceneRender();

        // bounds as drawn: a skinned mesh is posed by its bones, so skinnedAABB and not aabb
        static AABB getMeshLocalAABB(const MeshComponent& mesh);

        virtual void hideAllGizmos();

        void setPlayMode(bool isPlaying);

        // The Scene view draws through the Engine: this scene as its main scene, with the
        // editor layers on top, into the Scene view's framebuffer
        virtual void activate();
        // The game draws through the Engine into target (the Game view's): this scene
        // as its main scene with none of the editor layers
        void activateGame(Framebuffer* target);
        // The Scene view as a draw-only view, while a game plays: this scene through the
        // editor camera (or the camera it previews), the scenes layered over it and the
        // editor gizmos and overlays. For the running scene, after the game's frame,
        // layers are the game's layer scenes framed like the game frames its canvas;
        // for a stopped scene beside it, they are its inline child scenes.
        void drawSceneView(int width, int height, const std::vector<Scene*>& layerScenes, bool gameLayers);
        virtual void updateSize(int width, int height);
        virtual void setOverlayScale(float scale);
        float billboardScreenScale(const Vector3& worldPos, float logicalScale) const;
        virtual void updateSelLines(std::vector<OBB> obbs) = 0;

        void updateRenderSystem();

        virtual void update(std::vector<Entity> selEntities, std::vector<Entity> entities, Entity mainCamera, const SceneDisplaySettings& settings = SceneDisplaySettings{});
        virtual void mouseHoverEvent(float x, float y);
        virtual void mouseClickEvent(float x, float y, std::vector<Entity> selEntities);
        virtual void mouseReleaseEvent(float x, float y);
        // sceneId is this scene: its history takes the commands. While an edit layer is set
        // the tools work on the layer's selection, and its commands target the layer.
        virtual void mouseDragEvent(float x, float y, float origX, float origY, Project* project, size_t sceneId, std::vector<Entity> selEntities, bool disableSelection, bool invertRotationSnap, bool preserveAspectRatio);

        virtual bool isAnyGizmoSideSelected() const;

        // The transform tools edit the GameObjects selected in a scene layered over this
        // one: layerScene (sceneId, a scene or a play runtime id) as view shows it. entities
        // are its authored GameObjects; while running, only those take the tools, like the
        // running scene's own. Called every frame the view updates; clearEditLayer() hands
        // the tools back to this scene.
        void setEditLayer(uint32_t sceneId, Scene* layerScene, const LayerView& view, const std::vector<Entity>& selection,
                          const std::vector<Entity>& entities, bool running);
        void clearEditLayer();
        bool hasEditLayer() const;
        uint32_t getEditLayerSceneId() const;
        // the edit layer is seen through a UI camera, whose y runs down the view
        bool isEditLayerYDown() const;
        // The tools layer the gizmo the mouse works is in: the edit layer's while one is set
        ToolsLayer* getActiveToolsLayer();

        // Where the Scene view draws a running game's layer scenes: the game's canvas,
        // fitted and centered in a view of width x height
        Rect getGameLayerRect(int width, int height) const;
        bool isTerrainEditing() const;

        void setChildSceneLayers(const std::vector<Scene*>& layers);
        // Prefab Mode In Context: activate() makes `scene` the Engine's main scene, seen
        // through `camera` without its editor overlays, then `veil` (may be null), then this
        // scene as a layer over them. Null to stop; a change needs a new activate().
        // Returns whether it changed.
        bool setUnderlay(Scene* scene, Entity camera, Scene* veil);
        bool hasUnderlay() const { return underlayScene != nullptr; }

        TextureRender& getTexture();
        // What the Scene view shows (the scene, its layers and the editor overlays), as last
        // drawn; for screenshots read it with GraphicUtils::encodeFramebufferPng
        Framebuffer* getFramebuffer() { return &framebuffer; }
        Camera* getCamera();
        // ray through (x, y) of the Scene view (engine pixels) from the editor camera
        Ray screenToRay(float x, float y);
        int getViewWidth() const { return viewWidth; }
        int getViewHeight() const { return viewHeight; }
        bool setPreviewCamera(Entity entity);
        void clearPreviewCamera();
        Entity getPreviewCameraEntity() const;
        bool isPreviewCameraActive();

        // Game view pass (edit mode): draws this scene through cameraEntity into target,
        // without the editor layers and gizmos. The caller sizes the view and draws;
        // endGameViewPass() hands the scene back to the editor camera, and the next
        // editor draw needs activate() again.
        void beginGameViewPass(Entity cameraEntity, Framebuffer* target);
        void endGameViewPass();
        ToolsLayer* getToolsLayer();
        UILayer* getUILayer();

        bool isUseGlobalTransform() const;
        void setUseGlobalTransform(bool useGlobalTransform);

        void enableCursorPointer();
        void enableCursorHand();
        CursorSelected getCursorSelected() const;

        bool isMultipleEntitesSelected() const;

        AABB getEntitiesAABB(const std::vector<Entity>& entities);

        // Tile sub-selection
        int getSelectedTileIndex() const { return selectedTileIndex; }
        Entity getSelectedTileEntity() const { return selectedTileEntity; }
        void selectTile(Entity entity, int tileIndex);
        void clearTileSelection();
        int hitTestTile(Entity entity, float x, float y);
        OBB getTileOBB(Entity entity, int tileIndex, bool visual = false);

        // Instance sub-selection
        int getSelectedInstanceIndex() const { return selectedInstanceIndex; }
        Entity getSelectedInstanceEntity() const { return selectedInstanceEntity; }
        void selectInstance(Entity entity, int instanceIndex);
        void clearInstanceSelection();
        int hitTestInstance(Entity entity, float x, float y);
        OBB getInstanceOBB(Entity entity, int instanceIndex, bool visual = false);
        Quaternion getInstanceWorldRotation(const Transform& transform, const InstancedMeshComponent& instmesh, const InstanceData& inst) const;

        // ShadowCaster2D polygon point sub-selection
        int getSelectedOccluderPointIndex() const { return selectedOccluderPointIndex; }
        Entity getSelectedOccluderPointEntity() const { return selectedOccluderPointEntity; }
        void selectOccluderPoint(Entity entity, int pointIndex);
        void clearOccluderPointSelection();
        int hitTestOccluderPoint(Entity entity, float x, float y);
        OBB getOccluderPointOBB(Entity entity, int pointIndex);

        // Lines endpoint sub-selection (index = lineIndex * 2 + endpoint, 0 = A, 1 = B)
        int getSelectedLinePointIndex() const { return selectedLinePointIndex; }
        Entity getSelectedLinePointEntity() const { return selectedLinePointEntity; }
        void selectLinePoint(Entity entity, int pointIndex);
        void clearLinePointSelection();
        int hitTestLinePoint(Entity entity, float x, float y);
        OBB getLinePointOBB(Entity entity, int pointIndex);
        // world half-extent of a point handle so it stays a constant size on screen
        // (~8px): zoom-scaled in ortho views, distance-scaled in perspective views
        float getPointHandleHalfSize(const Vector3& worldPoint);

        // Polygon / MeshPolygon vertex sub-selection
        int getSelectedPolygonPointIndex() const { return selectedPolygonPointIndex; }
        Entity getSelectedPolygonPointEntity() const { return selectedPolygonPointEntity; }
        bool isSelectedPolygonPointMesh() const { return selectedPolygonPointIsMesh; }
        void selectPolygonPoint(Entity entity, bool isMesh, int pointIndex);
        void clearPolygonPointSelection();
        int hitTestPolygonPoint(Entity entity, bool isMesh, float x, float y);
        OBB getPolygonPointOBB(Entity entity, bool isMesh, int pointIndex);

        // TranslateTracks keyframe point sub-selection
        int getSelectedTrackPointIndex() const { return selectedTrackPointIndex; }
        Entity getSelectedTrackPointEntity() const { return selectedTrackPointEntity; }
        void selectTrackPoint(Entity entity, int pointIndex);
        void clearTrackPointSelection();
        int hitTestTrackPoint(Entity entity, float x, float y);
        OBB getTrackPointOBB(Entity entity, int pointIndex);
        // matrix mapping TranslateTracks values[] to world space: the action
        // target's parent modelMatrix (identity when there is no target/parent)
        Matrix4 getTrackPointsWorldMatrix(Entity entity);
    };

}
