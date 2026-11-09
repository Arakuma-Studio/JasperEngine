// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCMATTHUMBNAILS_H
#define PROCMATTHUMBNAILS_H

// The small images of the Procedural Material window: a thumbnail on each node and the Variations
// grid (S90 R24, R26, R27). The budget is R24's:
// - one evaluator draws them all, one image at a time (the shared quad scene: no MaterialRender, no
//   pipeline per node, since the sokol pools hold 2048 of each);
// - at most one image is finished per frame, the most wanted first;
// - an LRU of CAPACITY images of IMAGE_SIZE², the least recently wanted dropped first;
// - an image whose key changed (its node or what feeds it was edited) stays shown, out of date,
//   until its new one is drawn; a value edit to the image the evaluator holds is a uniform edit (R7).
// What is wanted, and how much, is the caller's to say (the window: per tier, R24). An image of the
// device stays on it; the CPU tier's comes as pixels. Main thread; drawn from App::engineRender.

#include "PassPlanner.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine {
    class TextureRender;
}

namespace jasperengine::editor::procmat {

    class ProcMatThumbnails {
    public:
        static constexpr int IMAGE_SIZE = 128;
        static constexpr size_t CAPACITY = 64;

        // what to draw: `roots` of the graph at `seed`; `key` changes whenever the image would
        struct Request {
            std::string id;
            std::vector<PlanRoot> roots;
            double seed = 0.0;
            uint64_t key = 0;
            int priority = 0;   // higher first
        };

        struct Image {
            std::shared_ptr<TextureRender> device;  // tiers 1 and 2: the image on the device
            std::vector<uint8_t> pixels;            // otherwise RGBA8, IMAGE_SIZE² (the CPU tier, a test)
            uint64_t key = 0;
            uint64_t serial = 0;                    // grows with every new image (a pixel upload follows it)
            double costMs = 0.0;                    // what drawing it took on this device
            std::string error;                      // why there is no image for this key
        };

        using MakeEvaluator = std::function<std::unique_ptr<ProceduralEvaluator>()>;
        // the device image of `output` of the evaluation just finished, taken from the evaluator; null
        // when it has none (its result's pixels are the image)
        using TakeImage = std::function<std::shared_ptr<TextureRender>(ProceduralEvaluator&, const std::string& output)>;

        ProcMatThumbnails(std::shared_ptr<const NodeLibrary> library, MakeEvaluator make, TakeImage take);
        ~ProcMatThumbnails();
        ProcMatThumbnails(const ProcMatThumbnails&) = delete;
        ProcMatThumbnails& operator=(const ProcMatThumbnails&) = delete;

        void setGraph(const graph::Graph& graph);
        // the frame's requests, replacing the last frame's (the CAPACITY most wanted); what is not asked
        // for again is never started, and one in flight that is no longer asked for is cancelled (the
        // CPU tier's job). A new graph drops them: their keys were the graph's before.
        void setRequests(std::vector<Request> requests);
        // Starts or advances the image in flight within budgetMs; at most one finishes per call.
        // True while a request still waits for its image.
        bool update(double budgetMs);
        // every image and the evaluator go (a project switch, the graph closes)
        void clear();

        // the latest image of `id` (whatever its key), null for none
        const Image* image(const std::string& id) const;
        // `id`'s image is not of its latest key, its plan failed (the picture is of the graph before), or
        // there is none yet
        bool isOutOfDate(const std::string& id) const;
        // asked for this frame and drawn at the key it was asked at, with no failure: what a click on it may trust
        bool isCurrent(const std::string& id) const;
        // the request being drawn, "" for none
        const std::string& inFlight() const { return drawing; }
        size_t size() const { return images.size(); }
        // how many images were finished, and how many evaluations used a prepare (a compile) rather
        // than a uniform edit of the held plan
        uint64_t finishedCount() const { return finished; }
        uint64_t prepareCount() const { return prepares; }
        ProceduralEvaluator* evaluator() const { return device.get(); }

    private:
        struct Entry {
            Image image;
            uint64_t used = 0;
        };

        bool start(const Request& request);
        void finish(EvaluationResult& result);
        void evict();

        std::shared_ptr<const NodeLibrary> library;
        MakeEvaluator make;
        TakeImage take;
        std::unique_ptr<ProceduralEvaluator> device;
        std::optional<graph::Graph> current;
        std::vector<Request> wanted;
        std::map<std::string, Entry> images;
        // the request drawn now and the plan the evaluator holds
        std::string drawing;
        uint64_t drawingKey = 0;
        std::string drawingOutput;
        double drawingMs = 0.0;
        std::optional<PassPlan> held;
        uint64_t clock = 0;
        uint64_t serials = 0;
        uint64_t finished = 0;
        uint64_t prepares = 0;
    };

}

#endif
