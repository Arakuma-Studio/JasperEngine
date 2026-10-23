// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pose thumbnails of the Pose Animation window and the Project window (spec: pose animation
// phase 4, sections 3.1 and 3.5), drawn by the phase 3 offscreen PoseRender: a model in any
// pose seen three-quarter from the front-right, shaded, one framing for every pose of a model
// (so cards share one scale).
//
// - PoseThumbnails: GPU textures of poses of one model, keyed by the caller's pose hash,
//   rendered lazily (a few requests in flight) and kept in a small LRU cache.
// - PoseAssetThumbnailer: the Project window's .poseanim thumbnails (the middle pose on the
//   asset's preview model) written as PNG files to the thumbnail cache, one at a time.
//
// Main thread only.

#pragma once

#include "pose/PoseRig.h"
#include "pose/PoseTypes.h"
#include "texture/Texture.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{
    class TextureRender;
}

namespace jasperengine::editor{

    class Project;
    class PoseRender;
    class PoseImage;

    // The camera of a pose thumbnail for a rig: three-quarter view (Front turned -40 degrees
    // about up) in model space, and a box framing the neutral pose with room for raised arms
    // and wide stances
    struct PoseThumbnailView{
        Vector3 direction;      // where the camera looks (model space)
        Vector3 up;
    };
    PoseThumbnailView poseThumbnailView(const pose::PoseRig& rig);

    // Sets every bone of `render` from `pose` on `rig` (bones matched by name; non-slot bones at
    // rest). False when the render has no model.
    // `bridges` (optional, PoseRigInfo::bridges): per skeleton bone, the non-bone ancestors the
    // skeleton folds into its rest TRS (taken off again for the bone transforms)
    bool applyPoseToRender(PoseRender& render, const pose::PoseRig& rig, const pose::HumanPose& pose,
                           const std::vector<Matrix4>* bridges = nullptr);

    // The Project window's .poseanim file icon: a figure mid-stride on a key-pose card, RGBA
    void makePoseAnimationIcon(int size, std::vector<uint8_t>& rgba);

    class PoseThumbnails{
    public:
        PoseThumbnails();
        ~PoseThumbnails();
        PoseThumbnails(const PoseThumbnails&) = delete;
        PoseThumbnails& operator=(const PoseThumbnails&) = delete;

        // The model file (project-relative or absolute) and its rig; a change drops the cache
        void setModel(const std::string& modelFile, const std::shared_ptr<pose::PoseRig>& rig,
                      const std::vector<Matrix4>& bridges = {});
        void clear();
        const std::string& getModel() const { return modelFile; }

        // The thumbnail of `pose` (key: the caller's hash of it), or null until it is rendered.
        // `stale` (optional) gives the last texture of `slot` while a new one renders, so a
        // live card does not flicker (slot: the caller's card id, -1 for none).
        TextureRender* get(uint64_t key, const pose::HumanPose& pose, int width, int height, int slot = -1);

        // Once per frame before drawing: frees textures retired in the previous frame
        void beginFrame();

        static uint64_t hashPose(const pose::HumanPose& pose);
        static uint64_t hashKey(const pose::PoseKey& key);

        // a request waits until fewer than this many are rendering
        static constexpr int MAX_IN_FLIGHT = 2;
        static constexpr size_t MAX_ENTRIES = 160;

    private:
        struct Entry{
            Texture texture;
            std::vector<uint8_t> pixels;     // the texture's data (not owned by TextureData)
            int width = 0;
            int height = 0;
            bool ready = false;
            bool failed = false;
            int lastUsedFrame = 0;
        };
        std::unique_ptr<PoseRender> render;
        std::string modelFile;
        std::shared_ptr<pose::PoseRig> rig;
        bool framed = false;
        bool framingFailed = false;
        float frameMin[3] = {0, 0, 0};
        float frameMax[3] = {0, 0, 0};
        std::map<uint64_t, Entry> entries;
        std::map<int, uint64_t> slotKeys;    // the last ready key per slot
        struct Retired{
            Texture texture;
            std::vector<uint8_t> pixels;
        };
        std::vector<Retired> retired;
        std::vector<Matrix4> bridges;
        int inFlight = 0;
        int generation = 0;                  // bumps on setModel/clear: stale callbacks are dropped
        int frameCounter = 0;
        std::shared_ptr<bool> alive;

        bool ensureFraming();
        void request(uint64_t key, const pose::HumanPose& pose, int width, int height);
        void store(uint64_t key, PoseImage&& image);
        void evict();
    };

    class PoseAssetThumbnailer{
    public:
        PoseAssetThumbnailer();
        ~PoseAssetThumbnailer();

        // Queues the thumbnail of a .poseanim (absolute path) to `thumbnailPath`; `done(ok)` runs
        // on the main thread from process()
        void request(Project* project, const std::filesystem::path& asset, const std::filesystem::path& thumbnailPath,
                     std::function<void(bool ok)> done);
        // once per frame
        void process();
        void cancel();
        bool busy() const { return active || !queue.empty(); }

        static constexpr int SIZE = 128;

    private:
        struct Job{
            Project* project = nullptr;
            std::filesystem::path asset;
            std::filesystem::path thumbnail;
            std::function<void(bool)> done;
        };
        std::deque<Job> queue;
        bool active = false;
        Job current;
        std::unique_ptr<PoseRender> render;
        std::shared_ptr<bool> alive;

        void start(Job job);
        void finish(bool ok);
    };

}
