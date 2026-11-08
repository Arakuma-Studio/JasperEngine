// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R24: the node images of the Procedural Material window. One shared evaluator draws them, at most
// one is finished per frame and the most wanted first, an LRU keeps CAPACITY of them, an image whose
// key changed stays shown out of date until its new one is drawn, a value edit of the image held is a
// uniform edit, and one no longer wanted is cancelled.

#include "../../procmat/NodeLibrary.h"
#include "../../procmat/ProcMatThumbnails.h"
#include "../../procmat/ProceduralMaterialKind.h"
#include "ImageEvaluator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;
using jasperengine::editor::procmat::test::ImageEvaluator;

namespace {

    class ProcMatThumbnailsTest : public ::testing::Test {
    protected:
        void SetUp() override {
            std::string error;
            library = NodeLibrary::fromDirectory(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/procmat/nodes", error);
            ASSERT_TRUE(library) << error;
            file.kind = PROCEDURAL_MATERIAL_KIND;
            graph::Node noise;
            noise.id = "n1";
            noise.type = "perlin";
            graph::Node blend;
            blend.id = "n2";
            blend.type = "blend";
            file.nodes = {noise, blend};
            graph::Edge in;
            in.id = "e1";
            in.from = {"n1", "output"};
            in.to = {"n2", "s1"};
            file.edges = {in};
        }

        std::unique_ptr<ProcMatThumbnails> make(int slices = 1) {
            counts = std::make_shared<ImageEvaluator::Counts>();
            auto shared = counts;
            return std::make_unique<ProcMatThumbnails>(library, [shared, slices]() {
                shared->made++;
                return std::unique_ptr<ProceduralEvaluator>(new ImageEvaluator(shared, EvaluatorTier::Gpu, slices));
            }, nullptr);
        }

        ProcMatThumbnails::Request request(const std::string& id, uint64_t key, int priority = 0) {
            return {id, {{"image", id, "output"}}, 0.0, key, priority};
        }

        std::shared_ptr<const NodeLibrary> library;
        std::shared_ptr<ImageEvaluator::Counts> counts;
        graph::Graph file;
    };

}

TEST_F(ProcMatThumbnailsTest, OneImageAFrameTheMostWantedFirstFromOneEvaluator) {
    auto thumbs = make();
    thumbs->setGraph(file);
    thumbs->setRequests({request("n1", 1, 0), request("n2", 2, 30)});
    EXPECT_TRUE(thumbs->update(8.0));
    EXPECT_EQ(thumbs->finishedCount(), 1u) << "one image a frame";
    EXPECT_TRUE(thumbs->image("n2")) << "the most wanted first";
    EXPECT_FALSE(thumbs->image("n1"));
    EXPECT_FALSE(thumbs->update(8.0)) << "nothing waits once both are drawn";
    EXPECT_TRUE(thumbs->image("n1"));
    EXPECT_EQ(thumbs->finishedCount(), 2u);
    EXPECT_EQ(counts->made, 1) << "one evaluator for every image";
    EXPECT_EQ(thumbs->image("n1")->pixels.size(), (size_t)ProcMatThumbnails::IMAGE_SIZE * ProcMatThumbnails::IMAGE_SIZE * 4);
    EXPECT_FALSE(thumbs->update(8.0));
    EXPECT_EQ(thumbs->finishedCount(), 2u) << "an image of its key is not drawn again";
}

TEST_F(ProcMatThumbnailsTest, AnEditedNodeKeepsItsImageOutOfDateAndAValueEditCompilesNothing) {
    auto thumbs = make(2);
    thumbs->setGraph(file);
    thumbs->setRequests({request("n2", 1)});
    while (thumbs->update(8.0)) {}
    ASSERT_TRUE(thumbs->image("n2"));
    const uint64_t serial = thumbs->image("n2")->serial;
    const int prepares = counts->prepares;

    // a value of the node drawn last: its passes take it as a uniform
    file.nodes[1].params["amount"] = 0.25;
    thumbs->setGraph(file);
    thumbs->setRequests({request("n2", 2)});
    EXPECT_TRUE(thumbs->isOutOfDate("n2"));
    EXPECT_TRUE(thumbs->update(8.0)) << "drawing (two slices)";
    ASSERT_TRUE(thumbs->image("n2")) << "the previous image stays while the new one is drawn";
    EXPECT_EQ(thumbs->image("n2")->serial, serial);
    EXPECT_TRUE(thumbs->isOutOfDate("n2"));
    EXPECT_FALSE(thumbs->update(8.0));
    EXPECT_FALSE(thumbs->isOutOfDate("n2"));
    EXPECT_GT(thumbs->image("n2")->serial, serial);
    EXPECT_EQ(counts->prepares, prepares) << "no compile for a value edit";
    EXPECT_EQ(counts->uniformEdits, 1);

    // a structural edit (an enum) prepares
    file.nodes[1].params["blend_type"] = "multiply";
    thumbs->setGraph(file);
    thumbs->setRequests({request("n2", 3)});
    while (thumbs->update(8.0)) {}
    EXPECT_EQ(counts->prepares, prepares + 1);
}

TEST_F(ProcMatThumbnailsTest, AnImageNoLongerWantedIsCancelledAndTheLeastRecentlyWantedGoFirst) {
    auto thumbs = make(5);
    thumbs->setGraph(file);
    thumbs->setRequests({request("n1", 1)});
    EXPECT_TRUE(thumbs->update(1.0));
    EXPECT_EQ(thumbs->inFlight(), "n1");
    // the selection moved on (tier 3: the job of the node left stops)
    thumbs->setRequests({request("n2", 2)});
    EXPECT_EQ(counts->cancels, 1);
    EXPECT_EQ(thumbs->inFlight(), "");
    while (thumbs->update(8.0)) {}
    EXPECT_FALSE(thumbs->image("n1")) << "a cancelled image is no image";
    EXPECT_TRUE(thumbs->image("n2"));

    // more nodes than the cache holds: the ones wanted least recently are dropped
    auto lru = make();
    graph::Graph many;
    many.kind = PROCEDURAL_MATERIAL_KIND;
    std::vector<ProcMatThumbnails::Request> all;
    for (size_t i = 0; i < ProcMatThumbnails::CAPACITY + 4; i++) {
        graph::Node node;
        node.id = "p" + std::to_string(i);
        node.type = "perlin";
        many.nodes.push_back(node);
    }
    std::sort(many.nodes.begin(), many.nodes.end(), [](const graph::Node& a, const graph::Node& b) { return graph::idLess(a.id, b.id); });
    lru->setGraph(many);
    for (size_t i = 0; i < ProcMatThumbnails::CAPACITY + 4; i++) {
        lru->setRequests({request("p" + std::to_string(i), 1)});
        while (lru->update(8.0)) {}
    }
    EXPECT_EQ(lru->size(), ProcMatThumbnails::CAPACITY);
    EXPECT_FALSE(lru->image("p0")) << "the first wanted, never again: dropped";
    EXPECT_TRUE(lru->image("p" + std::to_string(ProcMatThumbnails::CAPACITY + 3)));
}

TEST_F(ProcMatThumbnailsTest, ANodeThatCannotBePlannedIsTriedOncePerKey) {
    auto thumbs = make();
    file.nodes[0].params["iterations"] = "many";
    thumbs->setGraph(file);
    thumbs->setRequests({request("n1", 1)});
    EXPECT_FALSE(thumbs->update(8.0)) << "its plan fails: nothing waits";
    EXPECT_FALSE(thumbs->image("n1"));
    EXPECT_FALSE(thumbs->update(8.0));
    EXPECT_EQ(counts->prepares, 0);
}

// review I1: more images wanted at once than the cache holds: the most wanted CAPACITY are kept and
// drawn once, and the cache settles (no image is dropped and drawn again forever)
TEST_F(ProcMatThumbnailsTest, MoreImagesWantedThanTheCacheHoldsSettle) {
    auto thumbs = make();
    graph::Graph many;
    many.kind = PROCEDURAL_MATERIAL_KIND;
    std::vector<ProcMatThumbnails::Request> all;
    for (size_t i = 0; i < ProcMatThumbnails::CAPACITY + 10; i++) {
        graph::Node node;
        node.id = "p" + std::to_string(i);
        node.type = "perlin";
        many.nodes.push_back(node);
        all.push_back(request(node.id, 1, (int)i));
    }
    std::sort(many.nodes.begin(), many.nodes.end(), [](const graph::Node& a, const graph::Node& b) { return graph::idLess(a.id, b.id); });
    thumbs->setGraph(many);
    int frames = 0;
    do {
        thumbs->setRequests(all);
        frames++;
    } while (thumbs->update(8.0) && frames < 1000);
    EXPECT_LT(frames, 1000) << "it settles";
    EXPECT_EQ(thumbs->finishedCount(), ProcMatThumbnails::CAPACITY) << "each kept image drawn once";
    EXPECT_EQ(thumbs->size(), ProcMatThumbnails::CAPACITY);
    EXPECT_TRUE(thumbs->image("p" + std::to_string(ProcMatThumbnails::CAPACITY + 9))) << "the most wanted kept";
    EXPECT_FALSE(thumbs->image("p0")) << "the least wanted left out";
}

// review I2: requests keyed to the graph before an edit are not drawn from the edited graph
TEST_F(ProcMatThumbnailsTest, AnEditedGraphDrawsNothingUnderTheOldKeys) {
    auto thumbs = make();
    thumbs->setGraph(file);
    thumbs->setRequests({request("n1", 1)});
    file.nodes[0].params["scale_x"] = 9.0;
    thumbs->setGraph(file);
    EXPECT_FALSE(thumbs->update(8.0)) << "the old requests are gone with the old graph";
    EXPECT_FALSE(thumbs->image("n1"));
    EXPECT_EQ(counts->prepares, 0);
    thumbs->setRequests({request("n1", 2)});
    while (thumbs->update(8.0)) {}
    ASSERT_TRUE(thumbs->image("n1"));
    EXPECT_EQ(thumbs->image("n1")->key, 2u);
}

// review M6: an image whose new plan fails is out of date (its picture is of the graph before), and
// nothing waits for it
TEST_F(ProcMatThumbnailsTest, AnImageWhoseNewPlanFailsIsOutOfDate) {
    auto thumbs = make();
    thumbs->setGraph(file);
    thumbs->setRequests({request("n1", 1)});
    while (thumbs->update(8.0)) {}
    ASSERT_FALSE(thumbs->isOutOfDate("n1"));
    file.nodes[0].params["iterations"] = "many";
    thumbs->setGraph(file);
    thumbs->setRequests({request("n1", 2)});
    EXPECT_FALSE(thumbs->update(8.0));
    ASSERT_TRUE(thumbs->image("n1"));
    EXPECT_FALSE(thumbs->image("n1")->error.empty());
    EXPECT_TRUE(thumbs->isOutOfDate("n1"));
    EXPECT_FALSE(thumbs->update(8.0));
}
