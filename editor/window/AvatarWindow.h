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
//   project undo step (ProjectFileWrites: the model's clips and rigs reload); closing with changes
//   asks Apply, Discard or Cancel. The action functions are what the widgets run (editor tests
//   drive them).
// - The preview writes the bone Transforms directly and puts them back (stopPreview) when the tab
//   or the window closes, before a save or Play (stopTransientPreviews), and on project changes.
// - Thread-safety: main thread.

#pragma once

#include "animation/HumanoidClip.h"
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
        pose::HumanoidOverrides saved;
        pose::HumanoidOverrides edited;
        Resolved current;
        std::string status;
        bool statusError = false;
        char boneFilter[64] = "";
        int tab = 0;

        // preview
        bool previewing = false;
        struct Saved{ Entity entity; Vector3 position; Quaternion rotation; };
        std::vector<Saved> savedTransforms;
        std::string testClip;
        std::shared_ptr<const AnimationClip> testLoaded;
        std::string testError;
        float testTime = 0.0f;
        bool testPlaying = false;
        bool mirror = false;
        bool footIK = false;
        char testClipText[256] = "";
    };

}
