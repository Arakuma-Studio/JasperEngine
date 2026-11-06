// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "render/CapsuleShadows.h"

#include "SceneDisplaySettings.h"
#include "spline/SplineTypes.h"
#include "ToolsLayer.h"
#include "UILayer.h"
#include "RenderUtil.h"
#include "SceneDrawModes.h"
#include "navigation/NavigationOverlay.h"
#include "command/Command.h"
#include "command/CommandHistory.h"

#include <memory>
#include "io/watch/FileIdentity.h"

namespace jasperengine{
    class RenderSystem;
    class Sprite;
    struct InstancedMeshComponent;
    struct MeshComponent;
}

namespace jasperengine::editor{

    constexpr float DEFAULT_EDITOR_CAMERA_NEAR = 0.3f;
    constexpr float DEFAULT_EDITOR_CAMERA_FAR = 4000.0f;
    constexpr float MIN_EDITOR_CAMERA_DISTANCE = 0.2f;
    constexpr float MAX_EDITOR_CAMERA_DISTANCE = 2000.0f;

    constexpr float DEFAULT_EDITOR_WALK_SPEED = 10.0f;
    constexpr float MIN_EDITOR_WALK_SPEED = 0.5f;
    constexpr float MAX_EDITOR_WALK_SPEED = 1010.0f;

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
    class PoseViewport;
    class PhysicsPlacementWindow;

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

        // A gizmo or handle drag is one undo step, Esc (or Ctrl+Z) puts it back: the drag's
        // commands go through this edit, opened by its first one. Once it ends before the
        // mouse does (cancelled, or committed by a popup), the rest of the drag is ignored.
        InteractiveEdit dragEdit;
        bool dragEditStarted = false;
        bool pushDragCommand(size_t sceneId, Command* command);
        // this drag already said that Record does not key it (ClipRecordHooks::note)
        bool gizmoRecordNoted = false;

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
        // the line's mode when the point was picked (the index is a polyline position or a segment end)
        LineRendererMode selectedLinePointMode = LineRendererMode::Segments;
        bool linePointEditing = false;

        // Polygon / MeshPolygon vertex sub-selection (both store std::vector<PolygonPoint>;
        // isMesh picks which component, avoiding a ComponentType dependency in this header)
        Entity selectedPolygonPointEntity = 0;  // NULL_ENTITY
        int selectedPolygonPointIndex = -1;
        bool selectedPolygonPointIsMesh = false;

        // TranslateTracks keyframe point sub-selection (the tracks entity has no
        // Transform; its values[] live in the action target's parent space)
        Entity selectedTrackPointEntity = 0;  // NULL_ENTITY
        int selectedTrackPointIndex = -1;

        // Joint 2D anchor handle sub-selection: 0 anchor A, 1 anchor B (or a mouse joint's target)
        Entity selectedJointAnchorEntity = 0;  // NULL_ENTITY
        int selectedJointAnchorIndex = -1;

        // Capsule Shadow Caster handle sub-selection (index = capsule * CAPSULE_HANDLES + handle)
        Entity selectedCapsuleHandleEntity = 0;  // NULL_ENTITY
        int selectedCapsuleHandleIndex = -1;
        // the caster's capsules when the handle drag began: each drag frame edits from them, so
        // an end that fromWorld snaps to a bone axis does not drift the other end
        std::vector<CapsuleShadowShape> capsuleDragBase;

        // world position of vertex pointIndex of the selected entity's Polygon/MeshPolygon
        // points (nullptr list / bad index -> false)
        bool getPolygonPointWorld(Entity entity, bool isMesh, int pointIndex, Vector3& worldPoint);

        bool getTrackPointWorld(Entity entity, int pointIndex, Vector3& worldPoint);

        // world position of handle `index` of a Joint 2D (false when the joint has none there)
        bool getJointAnchorWorld(Entity entity, int index, Vector3& worldPoint);
        // the Joint 2D property a handle drag writes ("anchorA", "anchorB", "target")
        std::string getJointAnchorProperty(Entity entity, int index);

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

        // Physics (G): a gizmo drag runs a placement gesture instead of transform commands.
        // The click decides (physics active here, no point/instance/tile sub-selection), the
        // first drag frame begins the gesture on the handle the click took.
        bool physicsClick = false;
        bool physicsDragBegun = false;
        bool physicsGestureStarted = false;
        // the movers' centre when the gesture began, and the gizmo's offset from it, so the
        // gizmo follows the objects (not the mouse) without a jump
        Vector3 physicsMoversStart;
        Vector3 physicsGizmoOffset;
        bool hasToolSubSelection(const std::vector<Entity>& selEntities);
        void physicsGizmoDrag(PhysicsPlacementWindow* physics, uint32_t sceneId, bool invertRotationSnap);
        void updatePhysicsOverlay(PhysicsPlacementWindow* physics, uint32_t sceneId);

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

        // --- the AI Navigation overlay (S19, editor/navigation/NavigationOverlay.cpp) ---
        std::unique_ptr<NavigationOverlay> navigationOverlay;
        void updateNavigationOverlay(const std::vector<Entity>& selEntities);

        // --- lattices: cages, point gizmo and drag (editor/lattice/LatticeSceneRender.cpp) ---
        struct LatticeCageLines{
            LineRenderer* overlay = nullptr; // tools scene: over the geometry (faded where hidden in 3D)
            LineRenderer* depth = nullptr;   // this scene: depth tested, at full colour (3D views)
            // what an unselected cage was last built from: its lines do not depend on the camera,
            // so they are rebuilt only when one of these changes (many cages, Show all lattices)
            bool cached = false;
            Matrix4 cachedModel;
            std::vector<Vector3> cachedOffsets;
            int cachedResolution[3] = {0, 0, 0};
            std::vector<float> cachedSettings;
        };
        // scratch of drawLatticeCage, kept between frames (no allocation per cage and frame)
        std::vector<Vector3> latticeRestScratch;
        std::vector<Vector3> latticeWorldScratch;
        std::map<Entity, LatticeCageLines> latticeCages;
        LineRenderer* latticeFalloffLines = nullptr;
        // the project scene this view shows, found on first use (selections are per scene)
        uint32_t latticeSceneId = 0;
        bool latticeSceneIdKnown = false;
        // a gizmo drag of lattice points, from mouseClickEvent to mouseReleaseEvent
        struct LatticeDrag{
            bool active = false;
            Entity lattice = 0; // NULL_ENTITY
            std::vector<int> points;
            std::vector<Vector3> baseOffsets;
            int resolution[3] = {0, 0, 0};
            Matrix4 latticeModel;
            Vector3 pivot;       // world, where the gizmo was
            Quaternion rotation; // gizmo rotation
        };
        LatticeDrag latticeDrag;

        uint32_t getLatticeSceneId();
        // the lattice whose points the tools edit: the only tool entity, a lattice with an
        // invertible Transform, in this scene (no edit layer); NULL_ENTITY otherwise
        Entity getPointEditLattice(const std::vector<Entity>& toolEntities);
        // world positions of every point of a lattice
        bool getLatticeWorldPoints(Entity entity, std::vector<Vector3>& points);
        // where the gizmo acts on the selected points: their centroid, lattice-local or world axes
        bool getLatticePointsPivot(Entity entity, const std::vector<int>& points, Vector3& pivot, Quaternion& rotation, OBB& obb);
        void updateLatticeCages(const std::vector<Entity>& selEntities, const std::vector<Entity>& entities, Entity editedLattice);
        void drawLatticeCage(LatticeCageLines& lines, Entity entity, bool selected, bool editable);
        void drawLatticeFalloff(Entity entity, bool visible);
        void hideLatticeCages();
        void destroyLatticeCages();
        // the world change of a gizmo drag about pivot (W/E/R or the 2D move), false when the
        // cursor is off the drag plane; shared by the lattice points and the spline knots
        bool gizmoDragDelta(const Vector3& pivot, const Quaternion& rotation, bool invertRotationSnap, Matrix4& delta);
        void beginLatticeDrag(Entity entity, const Vector3& pivot, const Quaternion& rotation);
        // the gizmo's change since the drag began (the entity drag's math, about the pivot)
        void dragLatticeGizmo(Project* project, uint32_t sceneId, bool invertRotationSnap);
        // a world-space change applied to the dragged points (and the falloff's)
        void dragLatticePoints(Project* project, uint32_t sceneId, const Matrix4& worldDelta);
        void endLatticeDrag();

        // --- splines: the edited spline's knots, handles and radius band, the Draw Spline
        // tool's stroke, the knot gizmo's drag (editor/spline/SplineSceneRender.cpp) ---
        // the Wireframe and Shaded Wireframe Draw Modes' wires (depth tested in 3D views)
        LineRenderer* wireframeLines = nullptr;
        SceneWireframe wireframe;
        void updateWireframeOverlay();
    public:
        bool isWireframeTruncated() const { return wireframe.isTruncated(); }
    private:
        LineRenderer* splineOverlayLines = nullptr; // tools scene: over the geometry
        LineRenderer* splineDepthLines = nullptr;   // this scene: depth tested (3D views)
        struct SplineDrag{
            bool active = false;
            Entity spline = 0; // NULL_ENTITY
            std::vector<uint32_t> knots;
            std::vector<spline::SplineKnot> baseKnots;
            Matrix4 model;
            Vector3 pivot;
            Quaternion rotation;
        };
        SplineDrag splineDrag;
        // the spline whose knots the tools edit: the only tool entity, the one in Edit Spline
        Entity getKnotEditSpline(const std::vector<Entity>& toolEntities);
        bool getSplineKnotsPivot(Entity entity, const std::vector<uint32_t>& knots, Vector3& pivot, Quaternion& rotation, OBB& obb);
        void updateSplineOverlay(const std::vector<Entity>& selEntities, Entity editedSpline);
        void hideSplineOverlay();
        void destroySplineOverlay();
        void beginSplineDrag(Entity entity, const Vector3& pivot, const Quaternion& rotation);
        void dragSplineGizmo(Project* project, uint32_t sceneId, bool invertRotationSnap);
        void endSplineDrag();
        // a Lattice Follower driven on the channel the grabbed gizmo edits (W position, E
        // rotation, R scale, the 2D gizmo's move) is replaced by its rest: the follower is put
        // back every frame, so the tools move the spot it follows instead
        void redirectDrivenFollowers(Scene* target, std::vector<Entity>& entities);

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

        // the wire of a selected version 2 Particle System's Shape module (particles spec 6.8):
        // `entity` noted in `current`; the wires of systems not in it are dropped by
        // dropParticleShapeLines after the entity loop
        void updateParticleShapeLines(Entity entity, bool selected, std::set<Entity>& current);
        void dropParticleShapeLines(const std::set<Entity>& current);
        std::map<Entity, LineRenderer*> particleShapeLines;

        // Hides the icons and lines of one GameObject inactive in hierarchy (Unity draws no
        // gizmos for those; the icon picker refuses them too); called after its update
        virtual void hideEntityGizmos(Entity entity);
        // the GameObject is active in hierarchy (cheap when the scene has no inactive ones)
        bool gizmoActive(Entity entity) const;

        // Marks an editor gizmo or icon created in the scene it annotates: only the
        // Scene view draws it, never the game or a camera image
        void setEditorOverlay(Entity entity);
        template<typename T>
        T* overlay(T* object){
            setEditorOverlay(object->getEntity());
            return object;
        }

        // A gizmo whose entity its scene destroyed behind this view's back (Stop once swept the
        // gizmos the Scene view first drew while playing as made by the game): hiding or
        // updating it throws, and its id may already be another entity's. An overlay of this
        // scene that is no longer marked as one is gone too (the mark goes with its entity).
        // render: this scene's RenderSystem (the overlay marks)
        bool isGizmoGone(const EntityHandle* gizmo, const RenderSystem& render) const;
        // Deletes a gizmo; a gone one without touching the scene (it no longer owns its entity)
        void deleteGizmo(EntityHandle* gizmo, const RenderSystem& render) const;
        // Every per-entity gizmo whose own entity is gone is forgotten (update() makes it again
        // while what it annotates is there), so no pass touches it: run before the gizmo maps
        // are used (update, hideAllGizmos, the destructors). The views' single gizmos (selection,
        // terrain brush, physics, grid, tile and document lines, the editor camera) are not
        // swept: they are made with the view, before any Play, so the Play snapshot lists them as
        // alive before Play and Stop never destroys them, and nothing else destroys them.
        virtual void dropGoneGizmos();

        template<typename T>
        void dropGoneGizmos(std::map<Entity, T*>& gizmos, const RenderSystem& render){
            for (auto it = gizmos.begin(); it != gizmos.end();){
                if (isGizmoGone(it->second, render)){
                    deleteGizmo(it->second, render);
                    it = gizmos.erase(it);
                }else{
                    ++it;
                }
            }
        }

        // gizmos made of several objects (icon and lines): one gone drops them all
        template<typename V, typename... Parts>
        void dropGoneGizmos(std::map<Entity, V>& gizmos, const RenderSystem& render, Parts... parts){
            for (auto it = gizmos.begin(); it != gizmos.end();){
                V& gizmo = it->second;
                if ((isGizmoGone(gizmo.*parts, render) || ...)){
                    (deleteGizmo(gizmo.*parts, render), ...);
                    it = gizmos.erase(it);
                }else{
                    ++it;
                }
            }
        }

        // Editor-only scenes the Scene view draws over the scene and its layers
        // (gizmos, overlays), and the views they render into before it
        virtual void addOverlayLayers(std::vector<Engine::ViewLayer>& layers, int width, int height);
        virtual void drawOverlayTargets();
        // the editor overlays placed after the scene's update get their transforms for a draw
        void settleOverlayTransforms();

        Scene* scene;
        Camera* camera;
        Framebuffer framebuffer;
        Entity previewCameraEntity;
        // Set only while the Game view pass draws through a game camera
        Entity gameViewCameraEntity = NULL_ENTITY;

        LineRenderer* selLines;
        LineRenderer* terrainBrushLines;
        // Physics Placement: region, contacts, teleport ring, ghosts, proxies, distance rays
        LineRenderer* physicsLines;

        ToolsLayer toolslayer;
        UILayer uilayer;

        // posing a humanoid in the view (pose animation phase 5; 3D views only, null otherwise)
        std::unique_ptr<PoseViewport> poseViewport;

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
        // replacingBase: during Play, the scene a game load put in place of this one, drawn as the
        // base through replacingCamera (the editor camera's mirror in it) instead of this scene
        void drawSceneView(int width, int height, const std::vector<Scene*>& layerScenes, bool gameLayers,
            Scene* replacingBase = nullptr, Entity replacingCamera = NULL_ENTITY);
        virtual void updateSize(int width, int height);
        virtual void setOverlayScale(float scale);
        float getOverlayScale() const { return overlayScale; }
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

        // Record (animation clips 9.R5), the transform gizmo's drag: `localMatrix` is `entity`'s
        // new local matrix from `gizmo` (Move: Position, Rotate: Rotation, Scale: Scale). When
        // Record claims that channel for some of `selection`, a claimed entity's value is recorded
        // (the live value and a key at the playhead, one merge key per drag, no scene command) and
        // an unclaimed one stays as it is; returns false when Record does not take the drag (the
        // caller builds the scene command). Rotate / Scale about another pivot also key the moved
        // position. The drag's release ends the gesture (endGizmoRecord).
        // `selectionClaimed`: gizmoRecordClaims of the drag's selection (asked once per drag event)
        bool recordGizmoTransform(uint32_t sceneId, Entity entity, const Matrix4& localMatrix, GizmoSelected gizmo,
                                  bool selectionClaimed);
        bool gizmoRecordClaims(uint32_t sceneId, GizmoSelected gizmo, const std::vector<Entity>& selection) const;
        // the gizmo drag's step for one GameObject (Move / Rotate / Scale of its new local matrix):
        // recorded (null) or the ObjectTransformCmd to add (the caller adds it; the drag's release,
        // mouseReleaseEvent, ends its merging and the record gesture)
        Command* buildObjectTransformDrag(Project* project, uint32_t sceneId, Entity entity, const Matrix4& localMatrix,
                                          GizmoSelected gizmo, bool selectionClaimed);
        void endGizmoRecord();

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
        // Gizmo hover highlight, terrain brush or Paint Dye brush, which follow the mouse
        bool hasHoverFeedback() const;
        // the Paint Dye brush is armed on a fluid of this scene (FluidEditorTools)
        bool isFluidPainting() const;
        // The Paint Dye brush's circle under the pointer, in pixels of this view (y down), for
        // SceneWindow to draw over the image (a 2D scene draws sprites over its own lines)
        bool getFluidBrushCircle(std::vector<Vector2>& points);
        // a world point in pixels of this view (y down) through the editor camera; false behind it
        bool projectToView(const Vector3& world, Vector2& point);

        // Pose animation (phase 5): the Pose Animation window's session poses a model of this
        // scene. Its handles take a click first (poseMouseDown, x/y as screenToRay); a drag they
        // took goes through mouseDragEvent / mouseReleaseEvent like a gizmo's.
        bool isPoseEditing() const;
        bool poseMouseDown(float x, float y, bool shift, bool ctrl);
        bool isPoseDragging() const;
        PoseViewport* getPoseViewport() const { return poseViewport.get(); }

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
        // where a world point is in the Scene view (engine pixels, y down); false behind the camera
        bool worldToScreen(const Vector3& world, Vector2& screen) const;
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
        // the Scene view's frame draws without the scene's post-processing (update)
        bool viewDisablesPostProcessing = false;
        void endGameViewPass();

        // An inactive pane of the Split and Quad layouts (S26 R20, D8): the scene and its inline
        // child scenes through the pane's own camera into target, after this frame's draw, with
        // the icons, grid and selection outlines but no tools layer. gridPlane: 0 the XZ plane,
        // 1 XY (front and back views), 2 YZ (side views).
        struct PaneViewFlags {
            bool shadows = false;
            bool postProcessing = false;
            int gridPlane = 0;
        };
        void drawPaneView(uint32_t viewId, Entity paneCamera, Framebuffer* target, int width, int height, const PaneViewFlags& flags);
        // the plane the 3D grid lies in (the 2D views have none)
        virtual void setGridPlane(int plane) { (void)plane; }
        virtual int getGridPlane() const { return 0; }

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
        // A Line Renderer's point handles by index: a polyline's positions, or each segment's
        // A and B (lineIndex * 2 + end). Their world positions, the property that stores one and
        // a world position in that property's space
        static std::vector<Vector3> getLinePointWorldPositions(const LineRendererComponent& lines, const Transform& transform);
        static std::string getLinePointProperty(const LineRendererComponent& lines, int pointIndex);
        static Vector3 toLinePointSpace(const LineRendererComponent& lines, const Transform& transform, const Vector3& world);
        // the line's points (either mode) in the GameObject's local space, merged into aabb
        static void mergeLineLocalPoints(AABB& aabb, const LineRendererComponent& lines, const Transform& transform);
        // Inspector > Line Renderer > Edit Points: Ctrl+click on a selected polyline inserts a
        // point there (insertLinePointAt)
        bool isLinePointEditing() const { return linePointEditing; }
        void setLinePointEditing(bool editing) { linePointEditing = editing; }
        // a point on the selected polyline under (x, y), inserted (one undo step) and selected:
        // its index, -1 when the click is not on the line
        int insertLinePointAt(Project* project, uint32_t commandSceneId, Entity entity, float x, float y);
        // the selected point of a polyline removed (one undo step): false without one
        bool removeSelectedLinePoint(Project* project, uint32_t commandSceneId);
        int getSelectedLinePointIndex() const { return selectedLinePointIndex; }
        Entity getSelectedLinePointEntity() const { return selectedLinePointEntity; }
        void selectLinePoint(Entity entity, int pointIndex);
        void clearLinePointSelection();
        int hitTestLinePoint(Entity entity, float x, float y);
        OBB getLinePointOBB(Entity entity, int pointIndex);
        // world half-extent of a point handle so it stays a constant size on screen
        // (~8px): zoom-scaled in ortho views, distance-scaled in perspective views
        float getPointHandleHalfSize(const Vector3& worldPoint);

        // Lattice point sub-selection (the selection itself is LatticeEditorOps')
        // point of the lattice under the mouse (-1 when none)
        int hitTestLatticePoint(Entity entity, float x, float y);
        // the points drawn on the same spot as `index` (itself included): the stacked layers
        // of a flat cage seen face on, which one click takes together
        std::vector<int> getLatticeCoincidentPoints(Entity entity, int index);
        // points of the lattice inside a Scene-view rectangle (NDC corners, as selectObjectsByRect)
        std::vector<int> getLatticePointsInRect(Entity entity, Vector2 start, Vector2 end);
        // world bounds of points of a lattice (all of them when points is empty)
        AABB getLatticePointsAABB(Entity entity, const std::vector<int>& points);
        // a gizmo drag of lattice points is under way (the mouse wheel sizes the falloff)
        bool isLatticeDragActive() const { return latticeDrag.active; }

        // Spline knots and Bezier handles under the cursor (the Edit Spline context)
        int hitTestSplineKnot(Entity entity, float x, float y);
        bool hitTestSplineHandle(Entity entity, float x, float y, uint32_t& knot, bool& out);
        std::vector<uint32_t> getSplineKnotsInRect(Entity entity, Vector2 start, Vector2 end);
        // the point of the curve under the cursor, within a few pixels (Ctrl+click inserts there)
        bool hitTestSplineCurve(Entity entity, float x, float y, Vector3& worldPoint);
        // the world point under the cursor for a new knot or stroke point: the snap's surface,
        // else the plane through the spline (y 0, or z 0 in 2D)
        bool splinePointUnderCursor(Entity entity, float x, float y, bool snapToSurface, Vector3& worldPoint);
        // a Bezier handle tip dragged to the cursor, in the plane facing the camera through it
        void dragSplineHandle(Project* project, uint32_t sceneId, Entity entity, uint32_t knot, bool out, float x, float y);
        bool isSplineDragActive() const { return splineDrag.active; }
        // world units of one pixel at a point (the Draw tool's pixel tolerances)
        float worldPerPixelAt(const Vector3& worldPoint) const;

        // Polygon / MeshPolygon vertex sub-selection
        int getSelectedPolygonPointIndex() const { return selectedPolygonPointIndex; }
        Entity getSelectedPolygonPointEntity() const { return selectedPolygonPointEntity; }
        bool isSelectedPolygonPointMesh() const { return selectedPolygonPointIsMesh; }
        void selectPolygonPoint(Entity entity, bool isMesh, int pointIndex);
        void clearPolygonPointSelection();
        int hitTestPolygonPoint(Entity entity, bool isMesh, float x, float y);
        OBB getPolygonPointOBB(Entity entity, bool isMesh, int pointIndex);

        // Joint 2D anchor handles (the Move tool drags them)
        int getSelectedJointAnchorIndex() const { return selectedJointAnchorIndex; }
        Entity getSelectedJointAnchorEntity() const { return selectedJointAnchorEntity; }
        void selectJointAnchor(Entity entity, int index);
        void clearJointAnchorSelection();
        int hitTestJointAnchor(Entity entity, float x, float y);
        OBB getJointAnchorOBB(Entity entity, int index);

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

        // Capsule Shadow Caster handles (the Move tool drags them): each capsule's two ends
        // (0 end A, 1 end B) and a point on its surface (2 radius), by capsule * CAPSULE_HANDLES + handle
        static constexpr int CAPSULE_HANDLES = 3;
        int getSelectedCapsuleHandleIndex() const { return selectedCapsuleHandleIndex; }
        Entity getSelectedCapsuleHandleEntity() const { return selectedCapsuleHandleEntity; }
        void selectCapsuleHandle(Entity entity, int index);
        void clearCapsuleHandleSelection();
        int hitTestCapsuleHandle(Entity entity, float x, float y);
        OBB getCapsuleHandleOBB(Entity entity, int index);
        // the bone a capsule lies on: the GameObject's descendant of that name (the first in
        // hierarchy order, as RenderSystem resolves it), the GameObject itself for ""
        Entity findCapsuleBone(Entity entity, const std::string& bone);
        // capsule `capsule` of a caster in world space, false when its bone is not found
        bool getCapsuleWorld(Entity entity, size_t capsule, CapsuleShadows::World& world);
        bool getCapsuleHandleWorld(Entity entity, int index, Vector3& worldPoint);
        // base (a caster's capsules) with handle `index` dragged to `world`: an end moves (the
        // other stays), the radius handle sets the radius to its distance from the segment
        bool capsulesWithHandleAt(Entity entity, int index, const Vector3& world, const std::vector<CapsuleShadowShape>& base,
                                  std::vector<CapsuleShadowShape>& capsules);
    };

}
