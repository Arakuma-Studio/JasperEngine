// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Direct posing in the Scene view (spec: pose animation phase 5). While the Pose Animation
// window's session (PoseEditSession) poses a model of this view's scene, the view shows:
// - handles in the tools layer (always visible, screen-size constant): hand and foot spheres
//   (IK), the hips cube, joint dots, and the rotation rings of the active joint with the joint
//   limits as red ticks (a ring drag stops at a limit);
// - in the scene itself (editor overlays, never saved): the floor disc with its 0.25 m grid,
//   contact rings under the feet on the floor and the centre-of-mass marker;
// - ghosts in a layer of their own drawn over the scene: clones of the model's meshes with a
//   rim shaded, alpha-blended ghost material and a depth pre-pass (only the front surface
//   shows), whose bones are driven directly: the onion skin (previous / next pose), the key
//   pose a preview is leaving, the Global Edit baseline.
// Every drag is one undo step (PoseEditSession::setWorking while dragging, commitWorking on
// release). Clicking empty space or selecting another object leaves posing. Everything is
// hidden or destroyed on every exit path (the session idle, unbound or on another scene, play
// mode, the view or the editor closing).
//
// One per SceneRender of a 3D scene. Main thread only.

#pragma once

#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "math/Ray.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "pose/PoseTypes.h"
#include "pose/PoseViewportLogic.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{
    class Scene;
    class Camera;
    class Primitive;
    class LineRenderer;
}

namespace jasperengine::editor{

    class Project;
    class PoseEditSession;
    struct PoseRigInfo;

    class PoseViewport{
    public:
        PoseViewport(Scene* scene, Scene* toolsScene);
        ~PoseViewport();
        PoseViewport(const PoseViewport&) = delete;
        PoseViewport& operator=(const PoseViewport&) = delete;

        // the scene the ghosts live in: a layer of the Scene view between the scene and the tools
        Scene* getGhostScene() const { return ghostScene; }

        // Once per editor frame (App): a selection outside the posed model leaves posing, and a
        // view whose scene is not posed any more drops its ghosts even when it does not draw
        static void frame(Project* project);
        // the view of `scene`, null when it has none
        static PoseViewport* forScene(Scene* scene);

        // Each time the view updates (SceneRender3D::update): sizes in view (engine) pixels;
        // `disabled` when the view shows something else (play mode, a previewed camera)
        void update(Camera* camera, int viewWidth, int viewHeight, float overlayScale, bool disabled);

        // the session poses this view's scene: the handles show and take clicks
        bool isPosing() const;
        // the posed model or one of its descendants (their transform gizmo is hidden)
        bool covers(Entity entity) const;
        void removeCovered(std::vector<Entity>& entities) const;

        // Mouse (rays of the view's camera). mouseDown returns whether a handle took the click.
        bool mouseDown(const Ray& ray, bool shift, bool ctrl);
        void mouseDrag(const Ray& ray, bool shift, bool ctrl);
        void mouseUp();
        void hover(const Ray& ray);
        bool isDragging() const;

        // the joint whose rings show (HumanBone::Count: none); false when the rig lacks it
        bool setActiveJoint(pose::HumanBone bone);
        pose::HumanBone getActiveJoint() const { return activeJoint; }

        // What the view shows (MCP state, tests): handle positions in view pixels
        struct HandleView{
            std::string name;          // slot name ("left_hand", "hips", ...), ring_l/u/f/free for rings
            std::string kind;          // hand, foot, hips, joint, ring
            Vector2 position;
            bool visible = false;
        };
        struct State{
            bool posing = false;
            bool previewing = false;
            std::vector<HandleView> handles;
            std::string activeJoint;
            std::vector<std::string> ghosts;     // previous, next, preview, global_edit
            bool floor = false;
            std::vector<std::string> contacts;   // left, right
            bool balanceShown = false;
            bool balanced = false;
            int viewWidth = 0;
            int viewHeight = 0;
        };
        State getState() const;

        // hides everything, drops the ghosts and ends a drag without committing it
        void clear();

    private:
        struct Ghost{
            Entity root = NULL_ENTITY;
            std::vector<Entity> entities;        // every copy, destroyed with the ghost
            std::vector<Entity> bones;           // per skeleton bone (NULL_ENTITY: none)
            std::vector<Entity> meshes;
            std::string kind;
            Vector4 color;
            bool colorSet = false;
        };

        struct Drag{
            poseview::HandleKind kind = poseview::HandleKind::None;
            pose::HumanBone bone = pose::HumanBone::Count;
            pose::HumanPose start;
            bool changed = false;
            // point drags (hands, feet, hips), world space
            Vector3 startWorld;
            Vector3 offset;
            // ring drags
            int ring = -1;                       // 0..2 = L, U, F, 3 = free
            Vector3 axisModel;
            Vector3 axisWorld;
            Vector3 startDirection;
            float angle = 0.0f;
            float applied = 0.0f;
            poseview::AngleRange range;
            bool limited = false;
            Vector3 reference;                   // world, ring angle 0 (limit ticks)
            std::string undoName;
            std::string mergeKey;
        };

        Scene* scene;
        Scene* toolsScene;
        Scene* ghostScene = nullptr;
        Camera* ghostCamera = nullptr;

        // tools layer
        std::array<Primitive*, 4> ends{};        // left hand, right hand, left foot, right foot
        Primitive* hipsCube = nullptr;
        std::vector<Primitive*> dots;            // poseview::jointDotBones() order
        LineRenderer* ringLines = nullptr;
        // the scene (overlays)
        Primitive* floorFill = nullptr;
        LineRenderer* floorLines = nullptr;

        std::vector<Ghost> ghosts;
        const void* ghostSource = nullptr;       // the rig info the ghosts were cloned for
        Entity ghostModel = NULL_ENTITY;

        // the camera as of the last update
        Matrix4 viewProjection;
        Vector3 cameraPosition;
        Vector3 viewDirection{0, 0, -1};
        bool orthographic = false;
        float nearClip = 0.1f;
        float farClip = 1000.0f;
        int width = 0;
        int height = 0;
        float pixelScale = 1.0f;
        bool updated = false;

        bool posing = false;
        bool previewing = false;
        pose::HumanBone activeJoint = pose::HumanBone::Count;
        const void* activeRig = nullptr;
        poseview::HandleKind hoverKind = poseview::HandleKind::None;
        pose::HumanBone hoverBone = pose::HumanBone::Count;
        int hoverRing = -1;
        Ray hoverRay;
        bool hasHoverRay = false;
        Drag drag;
        int dragSerial = 0;

        // what was drawn (state, picking)
        std::vector<poseview::Handle> handles;
        std::vector<poseview::Ring> rings;
        std::vector<std::string> ghostKinds;
        std::vector<std::string> contactSides;
        bool floorShown = false;
        bool balanceShown = false;
        bool balanced = false;

        // metrics and ring limits of the last pose
        pose::HumanPose metricsPose;
        bool metricsValid = false;
        float leftHeight = 1.0f, rightHeight = 1.0f;
        Vector3 leftFoot, rightFoot, centerOfMass;
        bool metricsBalanced = false;

        // the selection watched to leave posing
        std::vector<Entity> lastSelection;
        bool watching = false;

        PoseEditSession* posedSession() const;
        Matrix4 modelMatrix() const;
        Quaternion modelRotation() const;
        float modelScale() const;
        Vector3 toWorld(const Vector3& model) const;
        float pixelsToWorld(const Vector3& world, float pixels) const;
        bool jointLimits() const;
        bool grounded() const;

        void createObjects();
        void hideHandles();
        void hideFloor();
        void buildHandles(const pose::PoseRig& rig, const pose::HumanPose& pose);
        void buildRings(const pose::PoseRig& rig, const pose::HumanPose& pose);
        void drawHandles();
        void drawRings(const pose::PoseRig& rig, const pose::HumanPose& pose);
        void drawFloor(const pose::PoseRig& rig, const pose::HumanPose& pose, bool showBalance);
        void updateMetrics(const pose::PoseRig& rig, const pose::HumanPose& pose);
        void syncGhostCamera(Camera* camera);

        // ghosts
        struct GhostWish{ std::string kind; pose::HumanPose pose; Vector4 color; };
        void updateGhosts(PoseEditSession& session, const std::vector<GhostWish>& wishes);
        bool createGhost(PoseEditSession& session, Ghost& ghost);
        void destroyGhost(Ghost& ghost);
        void dropGhosts();
        void poseGhost(PoseEditSession& session, Ghost& ghost, const pose::HumanPose& pose);
        void colorGhost(Ghost& ghost, const Vector4& color);

        // a selection outside the model leaves posing (`act`: posing, else only followed)
        void watchSelection(Project* project, bool act);
        // tells the session whether a handle is hovered or dragged (its Scene view key routing)
        void publishEngaged();
        bool engagedPublished = false;
        // the scene is stopped (not saving or playing): handles may edit
        bool sceneEditable() const;
        void endDrag(bool commit);
    };

}
