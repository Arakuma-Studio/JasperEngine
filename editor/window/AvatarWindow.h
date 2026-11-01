// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim-retarget spec 9)
// - Purpose: the Avatar Configuration window: a model's humanoid Avatar, which is its .humanoid
//   sidecar (Unity's Rig tab "Configure Avatar"). Opened for a scene Model from the Inspector's
//   Rig foldout, the Pose window's Map Bones... and an Animation Player's Bindings report.
//   * Mapping tab: Body (the 22 slots) and Hands & Head (fingers, eyes, jaw): per row a combo of
//     the skeleton's bones with search, Auto (back to auto-detect) and None (forced unmapped);
//     required missing slots red, auto-detected rows grey, overrides white; Auto Map All (every
//     bone back to auto-detect) and Clear Overrides (that and the twists).
//   * Pose tab: the model in the Scene view in its neutral pose (what every humanoid clip is
//     relative to), a twist correction slider per slot (-180..180, live), a Test clip played
//     retargeted with a scrubber, Mirror and Foot IK toggles. Twists also change the model's pose
//     animations (said there).
//   * Notes: the auto-detect notes.
// - Edits are a working copy: the preview follows them live; Apply writes the sidecar as one
//   project undo step (ProjectFileWrites: the model's clips and rigs reload) and refuses hands and
//   head overrides that do not validate; closing with changes, or opening another model's Avatar,
//   asks Apply, Discard or Cancel. The action functions are what the widgets run (editor tests
//   drive them).
// - syncWithProject (every frame): a sidecar written outside the window (undo of Apply, Map
//   Bones, set_humanoid, the Inspector's Animation Type) becomes the saved copy, and the working
//   copy too when it has no edits; a reimported model or a changed hierarchy rebinds the bones
//   (the edits stay); another model file on the GameObject reopens.
// - The preview owns the model root (PreviewOwner, animation clips 9.R7: a delete, copy, save or
//   another preview restores it first), writes the bone Transforms directly and puts them back
//   (stopPreview) when the tab or the window closes, before a save or Play
//   (stopTransientPreviews), and on project changes. A test clip converted from this model's own
//   rig plays with the unsaved edits on both sides.
// - Thread-safety: main thread.

#pragma once

#include "animation/HumanoidClip.h"
#include "animation/PreviewOwner.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "pose/HumanExtraBones.h"
#include "pose/HumanoidMap.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseRig.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine{
    class AnimationClip;
    class Scene;
}

namespace jasperengine::editor{

    class Project;

    class AvatarWindow{
    public:
        static constexpr const char* WINDOW_NAME = "Avatar Configuration";

        explicit AvatarWindow(Project* project);
        ~AvatarWindow();

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        void requestFocus() { focusRequested = true; }

        // ---- actions (the widgets and the tests) ----

        // the Avatar of the Model on `entity` (its sidecar read as the working copy); false + error
        // when it has no Model with bones. Unsaved edits of another model are asked about first in
        // the UI; here they are dropped.
        bool openModel(uint32_t sceneId, Entity entity, std::string* error);
        // what Configure Avatar... runs: the bound model again keeps its edits; another model with
        // unsaved edits asks first (Apply / Discard / Cancel), then opens
        bool requestOpen(uint32_t sceneId, Entity entity, std::string* error);
        bool hasPendingOpen() const { return pendingOpen; }
        void syncWithProject();
        uint32_t boundScene() const { return sceneId; }
        Entity boundEntity() const { return entity; }
        const std::string& modelFile() const { return model; }        // asset-relative

        // nullopt: auto-detect, "": forced unmapped, else the bone name
        void setBone(pose::HumanBone slot, const std::optional<std::string>& bone);
        void setExtraBone(pose::HumanExtraBone bone, const std::optional<std::string>& name);
        void setTwist(pose::HumanBone slot, float degrees);
        void setAnimationType(pose::AnimationType type);
        void autoMapAll();
        void clearOverrides();
        // writes the sidecar (one project undo step); false + error when the write fails
        // (also when the sidecar on disk cannot be read: fix or delete it first)
        bool apply(std::string* error);
        void discard();
        bool isDirty() const { return edited != saved; }
        const pose::HumanoidOverrides& overrides() const { return edited; }

        // ---- what the working copy resolves to ----
        struct Resolved{
            pose::HumanoidMap humanoid;
            pose::HumanExtraMap extra;
            std::shared_ptr<pose::PoseRig> rig;     // null when the map is no valid humanoid
            std::string error;
            std::string extraError;                 // hands and head overrides that do not validate (Apply refuses)
            std::vector<std::string> notes;
        };
        const Resolved& resolved() const { return current; }
        const std::vector<std::string>& boneNames() const { return bones; }

        // ---- the Pose tab's preview ----
        void setPreview(bool on);                   // the neutral pose (and the test clip) on the model
        bool isPreviewing() const { return previewing; }
        void setTestClip(const std::string& clip);  // asset-relative .anim or "<model>#<clip>", "" none
        void setTestTime(float seconds);
        void setMirror(bool on) { mirror = on; }
        void setFootIK(bool on) { footIK = on; }
        // writes the preview pose now (show() does it every frame while previewing)
        void updatePreview();
        void stopPreview();

        void stopTransientPreviews();
        void clearSceneState(uint32_t sceneId);
        void projectClosed();

    private:
        void resolve();
        Scene* scene() const;
        bool readSaved(std::string* problem);
        void rebind();
        void restoreTransforms();
        std::shared_ptr<const AnimationClip> previewClip();
        void drawHeader();
        void drawMapping();
        void drawPose();
        void drawNotes();
        void drawClosePrompt();
        std::shared_ptr<anim::HumanoidTarget> previewTarget() const;

        Project* project = nullptr;
        bool windowOpen = false;
        bool focusRequested = false;
        bool closePrompt = false;

        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        std::string model;
        std::filesystem::path sidecar;
        pose::Skeleton skeleton;
        std::vector<Entity> boneEntities;
        std::vector<Matrix4> bridges;
        std::vector<std::string> bones;
        uint64_t boundRevision = 0;
        // the sidecar as last read or written (stat first, bytes when it moved)
        std::string sidecarBytes;
        bool sidecarExists = false;
        std::string sidecarProblem;         // the sidecar exists but does not read: Apply refuses
        std::filesystem::file_time_type sidecarTime{};
        uintmax_t sidecarSize = 0;
        bool pendingOpen = false;
        uint32_t pendingScene = 0;
        Entity pendingEntity = NULL_ENTITY;
        pose::HumanoidOverrides saved;
        pose::HumanoidOverrides edited;
        Resolved current;
        std::string status;
        bool statusError = false;
        char boneFilter[64] = "";
        int tab = 0;

        // preview
        bool previewing = false;
        PreviewOwner::Token previewToken = 0;
        struct Saved{ Entity entity; Vector3 position; Quaternion rotation; };
        std::vector<Saved> savedTransforms;
        std::string testClip;
        std::shared_ptr<const AnimationClip> testLoaded;
        // a generic test clip of this model: converted with the working copy while it has edits
        std::shared_ptr<const AnimationClip> testBase;
        std::string testSourceModel;
        std::shared_ptr<const AnimationClip> testLocal;     // null after a failed conversion (kept for testLocalWith)
        std::optional<pose::HumanoidOverrides> testLocalWith;
        uint64_t testPoolRevision = 0;      // AnimationClipPool::revision() testLoaded was loaded at
        std::string testError;
        float testTime = 0.0f;
        bool testPlaying = false;
        bool mirror = false;
        bool footIK = false;
        char testClipText[256] = "";
    };

}
