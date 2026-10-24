// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pose-aware rendering over PoseRender (spec: pose animation phase 3, section 2): a model and
// its pose rig (phase 2 rig provider), HumanPoses applied to the model's bones, the views of the
// spec in the rig's canonical character frame, one framing shared by a batch of poses (so the
// cells of a sheet row have the same scale) and the skeleton overlay drawn on the read-back
// image (left limbs coral, right limbs cyan, centre white) with the floor line.
//
// Decisions the spec leaves open:
// - Framing: the batch's skin sample points (PoseSkin, skinned on the CPU with the rig's FK)
//   and slot joints, plus the floor point under them (so the floor line stays in frame when
//   the batch jumps), fitted tightly along the view axes with an 8% margin.
// - Overlay segments: every present slot joined to its effective parent slot, plus short tips
//   for the head (0.2 humanScale) and the hands (0.08 humanScale) along their rest bone
//   direction, so the head and the wrists show their turn. Segments are drawn far to near
//   (painter's order along the view direction), joints after them; a segment takes the colour
//   of its child slot's side.
// - Custom view: yaw turns the Front direction about up (positive: toward the character's
//   left), pitch then tilts it (positive: the camera above, looking down).
// - Lights: the key light shines from the character's front-left-above, the fill from its
//   front-right (PoseRender's defaults, expressed in canonical axes).
//
// Main thread only.

#pragma once

#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "pose/PoseImage.h"
#include "pose/PoseRig.h"
#include "pose/PoseTypes.h"
#include "render/preview/PoseRender.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor{

    class Project;

    struct PoseView{
        enum Kind { Front, Side, ThreeQuarter, Back, Top, Custom };
        Kind kind = Side;
        float yaw = 0.0f;       // degrees, Custom only; yaw 0 = looking at the character's front
        float pitch = 0.0f;

        // "front", "side", "three_quarter", "back", "top", "custom"
        static const char* name(Kind kind);
        static bool fromName(const std::string& name, PoseView& out);
    };

    struct PoseRenderStyle{
        bool skeletonOverlay = true;
        bool floorLine = true;
        bool shaded = true;
        Vector4 background = Vector4(0.16f, 0.16f, 0.17f, 1.0f);
    };

    class PoseSheetRenderer{
    public:
        static constexpr PoseColor LEFT_COLOR = PoseColor::hex(0xFF7A45);
        static constexpr PoseColor RIGHT_COLOR = PoseColor::hex(0x36B3FF);
        static constexpr PoseColor CENTER_COLOR = PoseColor::hex(0xE8E8E8);
        static constexpr PoseColor FLOOR_COLOR = PoseColor::hex(0xFFA726);
        static constexpr float LINE_WIDTH = 2.0f;
        static constexpr float JOINT_DIAMETER = 3.0f;
        static constexpr float EFFECTOR_DIAMETER = 4.0f;
        static constexpr float FLOOR_LINE_WIDTH = 2.0f;
        static constexpr float MARGIN = 0.08f;

        using CellCallback = std::function<void(bool ok, PoseImage&& cell)>;
        // the image position (pixels, cell origin top-left) of every slot the rig has
        using PoseJoints = std::vector<std::pair<pose::HumanBone, Vector2>>;

        PoseSheetRenderer() = default;
        PoseSheetRenderer(const PoseSheetRenderer&) = delete;
        PoseSheetRenderer& operator=(const PoseSheetRenderer&) = delete;

        // Loads a model file (absolute) with the rig of the phase 2 rig provider (cached per
        // model file and .humanoid sidecar). False + error when the model does not load or is
        // not a usable humanoid (the error names the missing slots).
        bool setModel(Project* project, const std::filesystem::path& modelFile, std::string* error);
        // The same with a given rig (a scene model's): its skeleton must be the model file's. With
        // the model already loaded and a rig of the same skeleton and humanoid map (sameRig), the
        // rig is swapped in without reloading the model.
        bool setModel(const std::string& modelFile, const std::shared_ptr<pose::PoseRig>& rig, std::string* error);
        // true when `rig` has the loaded rig's skeleton (names, parents, rest TRS) and humanoid map
        bool sameRig(const std::shared_ptr<pose::PoseRig>& rig) const;
        const std::shared_ptr<pose::PoseRig>& rig() const { return poseRig; }
        const std::string& modelFile() const { return modelPath; }
        bool hasModel() const { return poseRig != nullptr; }

        // Frames every pose of a batch with one framing (the union of their bounds) for a view:
        // later cells of that view use it. A view without a batch frames each cell alone.
        void beginBatch(const std::vector<pose::HumanPose>& posesForFraming, const PoseView& view);
        void endBatches();

        // Renders now (false, `out` empty, when the renderer is not ready yet: requestCell waits)
        bool renderCell(const pose::HumanPose& pose, const PoseView& view, int width, int height,
                        const PoseRenderStyle& style, PoseImage& out, PoseJoints* joints = nullptr);
        // Queues a cell (pose, view and style captured now); done runs on the main thread from a
        // later editor frame, the overlay already drawn (without a model: at once, ok false).
        // `joints`, when given, gets the projected slot joints now.
        void requestCell(const pose::HumanPose& pose, const PoseView& view, int width, int height,
                         const PoseRenderStyle& style, CellCallback done, PoseJoints* joints = nullptr);
        size_t getPendingCount() const { return render.getPendingRenderCount(); }
        void cancel() { render.cancelRenders(); }

        // The view's look direction and screen up in model space (canonical axes through the rig)
        bool viewAxes(const PoseView& view, Vector3& direction, Vector3& up) const;

    private:
        struct Batch{
            PoseView view;
            std::vector<Vector3> points;
        };
        struct Segment{
            Vector2 a, b;
            float depth = 0.0f;
            PoseColor color;
        };
        struct Joint{
            Vector2 at;
            float depth = 0.0f;
            float diameter = 0.0f;
            PoseColor color;
        };
        struct Overlay{
            std::vector<Segment> segments;
            std::vector<Joint> joints;
            bool floor = false;
            float floorY = 0.0f;
        };

        PoseRender render;
        std::shared_ptr<pose::PoseRig> poseRig;
        std::string modelPath;
        // per rig skeleton bone: the PoseRender bone (-1 when missing) and the non-bone
        // ancestors folded into the rig's rest TRS (skeleton local = bridge * Transform local)
        std::vector<int> renderBone;
        std::vector<Matrix4> bridges;
        std::vector<bool> bridged;
        // rest bone directions in the bone's own frame for the head and hand tips
        Vector3 headTip, leftHandTip, rightHandTip;
        std::vector<Batch> batches;

        bool bindSkeleton(std::string* error);
        void applyPose(const pose::HumanPose& pose);
        // model-space points bounding a pose: skin samples (else joints) and the slot joints
        void posePoints(const pose::HumanPose& pose, std::vector<Vector3>& out) const;
        void setCamera(const pose::HumanPose& pose, const PoseView& view);
        void prepare(const pose::HumanPose& pose, const PoseView& view, int width, int height, const PoseRenderStyle& style);
        Overlay makeOverlay(const pose::HumanPose& pose, const PoseRenderStyle& style) const;
        void projectJoints(const pose::HumanPose& pose, PoseJoints& out) const;
        static void drawOverlay(const Overlay& overlay, PoseImage& image);
        Vector3 canonicalToModel(const Vector3& canonical) const;
    };

}
