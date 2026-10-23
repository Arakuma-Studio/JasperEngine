// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The edit session of the Pose Animation window (spec: pose animation phase 4, section 2; the
// contract shared with the phase 5 viewport). It binds a scene Model GameObject with a valid
// humanoid and a .poseanim document, keeps the pose selection, and is the only code that
// writes the bound model's bone transforms in edit mode: posing shows the primary key pose
// (the working pose phase 5 handles edit), previewing shows the realised frames (the same
// frames as bake and runtime).
//
// The bones' local transforms are saved on the first apply and restored on unbind, window
// close, play mode, scene close or switch, project close and before the scene is saved (then
// applied again), so posing never dirties or corrupts the scene file.
//
// Main thread only.

#pragma once

#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "pose/PoseRig.h"
#include "pose/PoseTypes.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::editor{

    class Project;
    class PoseAnimationDocument;
    struct PoseRealisation;
    struct PoseRigInfo;

    class PoseEditSession{
    public:
        enum class Mode { Idle, Posing, Previewing };
        static PoseEditSession* active();
        // binding
        bool bind(Scene* scene, uint32_t sceneId, Entity model, std::string* error);   // a scene Model entity with a valid humanoid
        void unbind();                                   // restores bones
        Entity model() const;  Scene* scene() const;  uint32_t sceneId() const;
        const std::shared_ptr<pose::PoseRig>& rig() const;
        bool open(const std::string& poseanimPath, std::string* error);   // phase 2 document
        const std::shared_ptr<PoseAnimationDocument>& document() const;
        // selection
        int primary() const;  const std::vector<int>& selection() const;
        void select(int index, bool additive, bool range);
        // modes
        Mode mode() const;
        void startPosing();                              // applies the primary key pose to the model
        void startPreview();  void stopPreview();  void setPreviewTime(float t);  float previewTime() const;
        bool previewPlaying() const;  void setPreviewPlaying(bool);
        // working pose (posing): phase 5 handles edit it
        const pose::HumanPose& working() const;
        void setWorking(const pose::HumanPose&);         // applies to the bones immediately (no undo)
        bool commitWorking(const std::string& undoName, const std::string& mergeKey);  // toBodyPose, limits, grounding, doc commit
        // ghost / onion settings read by phase 5
        struct OnionSettings { bool previous = true; bool next = false; float opacity = 0.3f; };
        OnionSettings& onion();
        // global edit (phase 5 drives it; the window shows its bar)
        bool globalEditActive() const;  void beginGlobalEdit();  bool applyGlobalEdit(const std::vector<int>& targets /* empty = all others */);  void cancelGlobalEdit();
        const pose::HumanPose& globalEditBaseline() const;
        void addListener(std::function<void()>);         // any change: document, selection, mode, time

        // ---- additions (phase 4 implementation; the contract above is unchanged) ----

        // The viewport toggles of the window's Pose tab that phase 5 reads besides onion():
        // the centre-of-mass marker ("Show balance") and the key-pose ghost while previewing
        // ("Ghost during preview")
        struct ViewSettings { bool showBalance = true; bool ghostDuringPreview = false; };
        ViewSettings& view();

        explicit PoseEditSession(Project* project);
        ~PoseEditSession();
        PoseEditSession(const PoseEditSession&) = delete;
        PoseEditSession& operator=(const PoseEditSession&) = delete;

        // the window makes its session active while it is open (null: none)
        static void setActive(PoseEditSession* session);
        Project* project() const { return projectPtr; }

        bool bound() const { return rigPtr != nullptr; }
        // the bound model's rig info (map, missing slots, bone entities); null when unbound
        const std::shared_ptr<const PoseRigInfo>& rigInfo() const { return rigInfoPtr; }
        // project-relative model file of the bound GameObject ("" when unbound)
        const std::string& modelFile() const { return modelPath; }
        // the document realised on the bound rig (keys, frames, timeline, metrics); null
        // without a rig or a document
        std::shared_ptr<const PoseRealisation> realisation();
        // closes the document (bones restored, Idle)
        void closeDocument();

        // One undoable edit of the document's data from the window (one project undo step;
        // mergeKey merges a drag), then the selection to show (primary < 0 keeps it). Global
        // Edit is cancelled first unless keepGlobalEdit.
        bool commitData(const pose::PoseAnimationData& data, const std::string& undoName, const std::string& mergeKey,
                        const std::vector<int>* selection = nullptr, int primary = -1, std::string* error = nullptr,
                        bool keepGlobalEdit = false);
        void setSelection(const std::vector<int>& indices, int primary);

        // the bones back to their saved transforms and Idle (window close)
        void goIdle();
        // Before a scene save or play: the bones are restored now and applied again by update()
        // once the scene is stopped (play mode leaves the session Idle)
        void suspend();
        // the scene is gone: drop the binding without touching its entities
        void clearSceneState(uint32_t sceneId);
        // Per frame: a playing preview advances, deleted models unbind, play mode / scene switch
        // restore the bones, a suspended session applies its pose again
        void update(float dt);

        // preview helpers
        float previewLength();                // animation length (seconds), 0 without frames
        int previewFrame();                   // the frame index shown at previewTime()
        bool previewLoops();
        // the realised key pose of `index` (rest when unavailable)
        pose::HumanPose keyPose(int index);
        // the pose shown in the scene right now (working, the preview frame, or rest)
        const pose::HumanPose& displayed() const { return shown; }
        bool bonesApplied() const { return applied; }

        // the delta Global Edit would apply: the slots changed since beginGlobalEdit
        int globalEditChangedSlots();

    private:
        struct SavedBone{
            Entity entity = NULL_ENTITY;
            Vector3 position;
            Quaternion rotation;
            Vector3 scale;
        };

        Project* projectPtr = nullptr;
        Scene* scenePtr = nullptr;
        uint32_t sceneIdValue = 0;
        Entity modelEntity = NULL_ENTITY;
        std::string modelPath;
        std::shared_ptr<pose::PoseRig> rigPtr;
        std::shared_ptr<const PoseRigInfo> rigInfoPtr;
        std::shared_ptr<PoseAnimationDocument> doc;
        int documentListener = 0;

        std::vector<int> selected;
        int primaryIndex = -1;
        int anchorIndex = -1;
        Mode currentMode = Mode::Idle;

        float time = 0.0f;
        bool playing = false;
        float endPause = 0.0f;               // one-shot: the pause before it replays

        pose::HumanPose workingPose;
        pose::HumanPose shown;
        bool applied = false;                // the bones show a pose (saved transforms exist)
        bool suspended = false;              // restored for a save; apply again when stopped
        std::vector<SavedBone> savedBones;

        OnionSettings onionSettings;
        ViewSettings viewSettings;

        bool globalEdit = false;
        int globalEditPose = -1;
        pose::HumanPose globalBaseline;
        pose::BodyPose globalBaselineBody;

        std::vector<std::function<void()>> listeners;
        bool notifying = false;

        void notify();
        void applyPose(const pose::HumanPose& pose);
        void restoreBones();
        void applyCurrent();                 // what the mode shows: working pose / preview frame
        void samplePreview(pose::HumanPose& out);
        void documentChanged(int change);
        void clampSelectionToDocument();
        bool bonesValid() const;
        int poseCount() const;
        void dropBinding();
    };

}
