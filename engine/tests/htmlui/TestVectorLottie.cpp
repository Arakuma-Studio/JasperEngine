// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// <lottie> (p1-vector): the dotLottie reader (ZIP stored/deflate, v1/v2 manifests, images,
// ZIP64/encrypted rejection) and the element on ThorVG: load/error events, attributes and
// manifest settings, the UI clock, ONE dynamic texture per element updated in place only on
// frame changes (and a new one on resize), the C++ control API through UIElement.
//
// The element tests draw through a RecordingRenderInterface that also owns the element's
// HtmlUITexture handles (the phase-1 RmlRenderer contract: ReleaseTexture deletes them), so
// they need the headless GPU for the dynamic textures but not p1-render's renderer.

#include "harness/GpuEnvironment.h"
#include "harness/Readback.h"
#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "HtmlUIClock.h"
#include "HtmlUITexture.h"
#include "object/ui/UIElement.h"
#include "object/ui/UIElementInternal.h"
#include "render/SystemRender.h"
#include "vector/DotLottie.h"
#include "vector/UILottieElement.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::vector<uint8_t> readFixture(const std::string& name){
        std::ifstream file(std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/fixtures/p1/lottie/" + name, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // ---------------------------------------------------------------- recording + HtmlUITexture

    // what the phase-1 RmlRenderer does with an HtmlUITexture handle: ReleaseTexture deletes it
    class LottieRecording : public RecordingRenderInterface {
    public:
        void ReleaseTexture(Rml::TextureHandle handle) override{
            if (getTexture(handle)){
                RecordingRenderInterface::ReleaseTexture(handle);
                return;
            }
            HtmlUITexture* texture = reinterpret_cast<HtmlUITexture*>(handle);
            released.insert(texture);
            delete texture;
        }

        std::set<HtmlUITexture*> released;
    };

    class EventLog : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override{
            types.push_back(event.GetType());
            const Rml::String message = event.GetParameter<Rml::String>("message", "");
            if (!message.empty())
                messages.push_back(message);
        }

        size_t count(const std::string& type) const{
            size_t n = 0;
            for (const std::string& t : types)
                n += t == type ? 1 : 0;
            return n;
        }

        void clear(){
            types.clear();
            messages.clear();
        }

        std::vector<std::string> types;
        std::vector<std::string> messages;
    };

    const char* const eventTypes[] = {"load", "error", "play", "pause", "loop", "complete"};

    class LottieElementTest : public ::testing::Test {
    protected:
        void SetUp() override{
            TestSystem::get().clearLogs();
            HtmlUIClock::reset();
            HtmlUIClock::setFixedTime(0.0);
            ASSERT_TRUE(HtmlUI::acquire());
            static int counter = 0;
            name = "lottie-test-" + std::to_string(++counter);
            context = Rml::CreateContext(name, Rml::Vector2i(512, 384), &recording);
            ASSERT_NE(context, nullptr);
        }

        void TearDown() override{
            if (context){
                context->UnloadAllDocuments();
                context->Update();
                Rml::RemoveContext(name);
                Rml::ReleaseTextures(&recording);
                Rml::ReleaseRenderManagers();
            }
            HtmlUI::release();
            HtmlUIClock::reset();
        }

        // body + one <lottie id="anim" ...>, listeners for every Lottie event
        UILottieElement* load(const std::string& attributes, const std::string& style = "width: 100px; height: 100px;"){
            const std::string rml =
                "<rml><head><style>body { display: block; width: 512px; height: 384px; margin: 0; }"
                " #anim { display: block; " + style + " }</style></head>"
                "<body><lottie id=\"anim\" " + attributes + "/><div id=\"plain\"/></body></rml>";
            document = context->LoadDocumentFromMemory(rml, "p1/lottie/document.rml");
            if (!document)
                return nullptr;
            document->Show();
            Rml::Element* element = document->GetElementById("anim");
            for (const char* type : eventTypes)
                element->AddEventListener(type, &events);
            return rmlui_dynamic_cast<UILottieElement*>(element);
        }

        // one UI frame at UI time `time`: update, render, end of the sokol frame
        void frame(double time){
            HtmlUIClock::setFixedTime(time);
            context->Update();
            recording.beginFrame();
            context->Render();
            SystemRender::commit();
        }

        // the first frames after a load: load events come asynchronously, one frame later
        void settle(double time){
            frame(time);
            frame(time);
        }

        // the texture handle of the last draw of the element (0 if it drew nothing textured)
        Rml::TextureHandle drawnTexture() const{
            Rml::TextureHandle handle = 0;
            for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
                if (draw.texture)
                    handle = draw.texture;
            }
            return handle;
        }

        LottieRecording recording;
        Rml::Context* context = nullptr;
        Rml::ElementDocument* document = nullptr;
        EventLog events;
        std::string name;
    };

}

// ---------------------------------------------------------------- ZIP / dotLottie

TEST(DotLottieTest, StoredV1WithActiveAnimation){
    const std::vector<uint8_t> bytes = readFixture("stored_v1.lottie");
    ASSERT_FALSE(bytes.empty());
    EXPECT_TRUE(DotLottie::looksLikeZip(bytes.data(), bytes.size()));

    DotLottie archive;
    std::string error;
    ASSERT_TRUE(archive.open(bytes.data(), bytes.size(), error)) << error;
    EXPECT_EQ(archive.getVersion(), 1);
    ASSERT_EQ(archive.getAnimations().size(), 2u);
    EXPECT_EQ(archive.getAnimations()[0].id, "first");
    EXPECT_EQ(archive.getDefaultAnimationId(), "second");

    const DotLottie::Settings second = archive.getSettings("second");
    EXPECT_EQ(second.autoplay, std::optional<bool>(true));
    EXPECT_EQ(second.loop, std::optional<bool>(true));
    EXPECT_EQ(second.loopCount, std::optional<int>(3));
    EXPECT_EQ(second.speed, std::optional<float>(2.0f));
    EXPECT_EQ(second.direction, std::optional<int>(-1));
    EXPECT_EQ(second.playMode, std::optional<std::string>("bounce"));
    const DotLottie::Settings first = archive.getSettings("first");
    EXPECT_EQ(first.autoplay, std::optional<bool>(false));
    EXPECT_EQ(first.loop, std::optional<bool>(false));

    std::string json;
    ASSERT_TRUE(archive.readAnimation("second", json, error)) << error;
    EXPECT_NE(json.find("\"nm\":\"second\""), std::string::npos);
    EXPECT_FALSE(archive.readAnimation("missing", json, error));
    EXPECT_NE(error.find("missing"), std::string::npos);
}

TEST(DotLottieTest, DeflateV2WithInitialAnimation){
    const std::vector<uint8_t> bytes = readFixture("deflate_v2.lottie");
    DotLottie archive;
    std::string error;
    ASSERT_TRUE(archive.open(bytes.data(), bytes.size(), error)) << error;
    EXPECT_EQ(archive.getVersion(), 2);
    EXPECT_EQ(archive.getDefaultAnimationId(), "shapes");
    std::string json;
    ASSERT_TRUE(archive.readAnimation("shapes", json, error)) << error;
    // inflated byte for byte (the CRC is checked too)
    const std::vector<uint8_t> plain = readFixture("shapes.json");
    EXPECT_EQ(json, std::string(plain.begin(), plain.end()));
    const DotLottie::Settings settings = archive.getSettings("shapes");
    EXPECT_FALSE(settings.autoplay.has_value());
    EXPECT_FALSE(settings.speed.has_value());
}

TEST(DotLottieTest, EmbeddedImage){
    const std::vector<uint8_t> bytes = readFixture("image_v2.lottie");
    DotLottie archive;
    std::string error;
    ASSERT_TRUE(archive.open(bytes.data(), bytes.size(), error)) << error;
    const std::vector<uint8_t> png = readFixture("dot.png");
    std::string image;
    // the path ThorVG builds: "<rpath>/<u><p>", u = "/i/"
    ASSERT_TRUE(archive.readImage("//i/dot.png", image));
    EXPECT_EQ(image, std::string(png.begin(), png.end()));
    ASSERT_TRUE(archive.readImage("i/dot.png", image));
    // by file name under images/ or i/
    ASSERT_TRUE(archive.readImage("./images/dot.png", image));
    EXPECT_FALSE(archive.readImage("i/other.png", image));
}

TEST(DotLottieTest, RejectedArchives){
    struct Case {
        const char* file;
        const char* message;
    };
    const Case cases[] = {
        {"zip64.lottie", "ZIP64"},
        {"encrypted.lottie", "encrypted"},
        {"bad_manifest.lottie", "no animations"},
        {"shapes.json", "not a ZIP archive"},
    };
    for (const Case& c : cases){
        const std::vector<uint8_t> bytes = readFixture(c.file);
        ASSERT_FALSE(bytes.empty()) << c.file;
        DotLottie archive;
        std::string error;
        EXPECT_FALSE(archive.open(bytes.data(), bytes.size(), error)) << c.file;
        EXPECT_NE(error.find(c.message), std::string::npos) << c.file << ": " << error;
    }

    DotLottie empty;
    std::string error;
    EXPECT_FALSE(empty.open(nullptr, 0, error));
}

TEST(DotLottieTest, CorruptEntryFailsTheCrc){
    std::vector<uint8_t> bytes = readFixture("stored_v1.lottie");
    // flip a byte inside the stored first animation's data
    const std::string marker = "\"nm\":\"first\"";
    auto it = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
    ASSERT_NE(it, bytes.end());
    *(it + 6) = 'F';
    DotLottie archive;
    std::string error;
    ASSERT_TRUE(archive.open(bytes.data(), bytes.size(), error)) << error;
    std::string json;
    EXPECT_FALSE(archive.readAnimation("first", json, error));
    EXPECT_NE(error.find("CRC"), std::string::npos) << error;
}

TEST(DotLottieTest, ZipEntryNames){
    const std::vector<uint8_t> bytes = readFixture("image_v2.lottie");
    ZipArchive zip;
    std::string error;
    ASSERT_TRUE(zip.open(bytes.data(), bytes.size(), error)) << error;
    EXPECT_EQ(zip.names(), (std::vector<std::string>{"a/image.json", "i/dot.png", "manifest.json"}));
    EXPECT_TRUE(zip.has("/i/dot.png"));
    std::string out;
    EXPECT_FALSE(zip.read("nothing", out, error));
}

// ---------------------------------------------------------------- the element

TEST_F(LottieElementTest, LoadsJsonWithIntrinsicSize){
    JASPER_REQUIRE_GPU();
    UILottieElement* lottie = load("src=\"shapes.json\"", "");
    ASSERT_NE(lottie, nullptr);
    frame(0.0);
    // loaded with the document, but the event comes in a later frame (listeners added after
    // LoadDocument still get it)
    EXPECT_TRUE(lottie->isLoaded());
    EXPECT_TRUE(events.types.empty());
    frame(0.0);
    EXPECT_EQ(events.types, std::vector<std::string>{"load"});
    EXPECT_EQ(lottie->getResolvedSource(), "p1/lottie/shapes.json");
    // the picture size (200 x 200) is the intrinsic size
    EXPECT_FLOAT_EQ(lottie->GetBox().GetSize().x, 200.0f);
    EXPECT_FLOAT_EQ(lottie->GetBox().GetSize().y, 200.0f);
    EXPECT_FLOAT_EQ(lottie->getTotalFrames(), 60.0f);
    EXPECT_FLOAT_EQ(lottie->getDuration(), 2.0f);
    EXPECT_FALSE(lottie->isPlaying());
    EXPECT_EQ(lottie->getCurrentFrame(), 0.0f);
    ASSERT_NE(lottie->getTexture(), nullptr);
    EXPECT_EQ(lottie->getTexture()->kind, HtmlUITexture::Kind::DYNAMIC);
    EXPECT_EQ(lottie->getTexture()->width, 200);
    EXPECT_EQ(drawnTexture(), reinterpret_cast<Rml::TextureHandle>(lottie->getTexture()));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u);
}

TEST_F(LottieElementTest, OneTextureUpdatedOnlyOnFrameChanges){
    JASPER_REQUIRE_GPU();
    UILottieElement* lottie = load("src=\"shapes.json\" autoplay=\"\" loop=\"\"");
    ASSERT_NE(lottie, nullptr);
    frame(0.0);
    ASSERT_TRUE(lottie->isPlaying());
    HtmlUITexture* texture = lottie->getTexture();
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(lottie->getStats().texturesCreated, 1u);
    EXPECT_EQ(lottie->getStats().uploads, 1u);

    // 60 display frames at 60 Hz of a 30 fps animation: 30 frame changes (frames 0 .. 30)
    std::set<float> shown;
    for (int i = 1; i <= 60; i++){
        frame(i / 60.0);
        EXPECT_EQ(lottie->getTexture(), texture) << "frame " << i;
        EXPECT_EQ(drawnTexture(), reinterpret_cast<Rml::TextureHandle>(texture)) << "frame " << i;
        shown.insert(lottie->getCurrentFrame());
    }
    EXPECT_EQ(shown.size(), 31u);
    EXPECT_EQ(lottie->getStats().texturesCreated, 1u);
    EXPECT_EQ(lottie->getStats().uploads, 31u);
    EXPECT_EQ(lottie->getStats().rasterizations, 31u);
    EXPECT_TRUE(recording.released.empty());

    // a held clock: no new frame, no upload
    const uint32_t uploads = lottie->getStats().uploads;
    for (int i = 0; i < 10; i++)
        frame(1.0);
    EXPECT_EQ(lottie->getStats().uploads, uploads);
    EXPECT_EQ(lottie->getStats().rasterizations, uploads);
}

TEST_F(LottieElementTest, ResizeCreatesExactlyOneNewTexture){
    JASPER_REQUIRE_GPU();
    UILottieElement* lottie = load("src=\"shapes.json\"");
    ASSERT_NE(lottie, nullptr);
    frame(0.0);
    HtmlUITexture* first = lottie->getTexture();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->width, 100);

    lottie->SetProperty("width", "150px");
    frame(0.0);
    frame(0.0);
    EXPECT_EQ(lottie->getStats().texturesCreated, 2u);
    EXPECT_EQ(recording.released, std::set<HtmlUITexture*>{first});
    ASSERT_NE(lottie->getTexture(), nullptr);
    EXPECT_EQ(lottie->getTexture()->width, 150);
    EXPECT_EQ(lottie->getTexture()->height, 100);
    EXPECT_EQ(lottie->getRenderSize(), Rml::Vector2i(150, 100));
}

TEST_F(LottieElementTest, ContentIsFittedAndCentred){
    JASPER_REQUIRE_GPU();
    // 200 x 100 box: the 200 x 200 animation scales to 100 x 100, centred
    UILottieElement* lottie = load("src=\"image_ext.json\"", "width: 200px; height: 100px;");
    ASSERT_NE(lottie, nullptr);
    frame(0.0);
    frame(0.0);
    HtmlUITexture* texture = lottie->getTexture();
    ASSERT_NE(texture, nullptr);
    std::vector<uint8_t> pixels;
    ASSERT_TRUE(readTexturePixels(*texture->render, 200, 100, 4, pixels));
    auto alphaAt = [&](int x, int y){ return pixels[((size_t)y * 200 + x) * 4 + 3]; };
    EXPECT_EQ(alphaAt(10, 50), 0);
    EXPECT_EQ(alphaAt(100, 50), 255);
    EXPECT_EQ(alphaAt(190, 50), 0);
}

TEST_F(LottieElementTest, ClockDrivesTheFrame){
    JASPER_REQUIRE_GPU();
    UILottieElement* lottie = load("src=\"shapes.json\" autoplay=\"autoplay\"");
    ASSERT_NE(lottie, nullptr);
    settle(0.0);
    EXPECT_EQ(events.types, (std::vector<std::string>{"load", "play"}));
    frame(0.5);
    EXPECT_EQ(lottie->getCurrentFrame(), 15.0f);
    // pinned: repeated frames at the same fixed time show the same frame
    frame(0.5);
    frame(0.5);
    EXPECT_EQ(lottie->getCurrentFrame(), 15.0f);
    frame(1.0);
    EXPECT_EQ(lottie->getCurrentFrame(), 30.0f);
    // not looping: complete at 2 s, on the last frame
    frame(2.5);
    EXPECT_EQ(lottie->getCurrentFrame(), 59.0f);
    EXPECT_EQ(events.count("complete"), 1u);
    EXPECT_FALSE(lottie->isPlaying());
    // only while playing does the element ask RmlUi for the next update, at the next frame boundary
    frame(2.6);
    EXPECT_GT(context->GetNextUpdateDelay(), 1e6);
    lottie->play();
    frame(2.6);
    EXPECT_NEAR(context->GetNextUpdateDelay(), 1.0 / 30.0, 1e-6);
    frame(2.61);
    EXPECT_NEAR(context->GetNextUpdateDelay(), 1.0 / 30.0 - 0.01, 1e-6);
}

TEST_F(LottieElementTest, Attributes){
    JASPER_REQUIRE_GPU();
    UILottieElement* lottie = load("src=\"shapes.json\" speed=\"2\" direction=\"-1\" loop=\"3\" segment=\"10 20\"");
    ASSERT_NE(lottie, nullptr);
    frame(0.0);
    const LottiePlayback& playback = lottie->getPlayback();
    EXPECT_FLOAT_EQ(playback.getSpeed(), 2.0f);
    EXPECT_EQ(playback.getDirection(), -1);
    EXPECT_EQ(playback.getLoopCount(), 3);
    EXPECT_FLOAT_EQ(playback.getSegmentStart(), 10.0f);
    EXPECT_FLOAT_EQ(playback.getSegmentEnd(), 20.0f);
    EXPECT_EQ(lottie->getCurrentFrame(), 19.0f);

    // changing attributes at run time
    lottie->SetAttribute("marker", "loop");
    lottie->SetAttribute("mode", "bounce");
    lottie->SetAttribute("loop", "");
    lottie->SetAttribute("speed", "0.5");
    frame(0.0);
    EXPECT_FLOAT_EQ(playback.getSegmentStart(), 30.0f);
    EXPECT_FLOAT_EQ(playback.getSegmentEnd(), 60.0f);
    EXPECT_EQ(playback.getMode(), LottiePlayback::Mode::BOUNCE);
    EXPECT_EQ(playback.getLoopCount(), 0);
    EXPECT_FLOAT_EQ(playback.getSpeed(), 0.5f);
    lottie->RemoveAttribute("loop");
    frame(0.0);
    EXPECT_EQ(playback.getLoopCount(), 1);

    // invalid values warn and fall back
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u);
    lottie->SetAttribute("speed", "fast");
    lottie->SetAttribute("direction", "2");
    lottie->SetAttribute("mode", "pingpong");
    lottie->SetAttribute("segment", "10");
    lottie->SetAttribute("marker", "missing");
    frame(0.0);
    EXPECT_FLOAT_EQ(playback.getSpeed(), 1.0f);
    EXPECT_EQ(playback.getDirection(), 1);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "speed=\"fast\""), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "direction=\"2\""), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "mode=\"pingpong\""), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "segment=\"10\""), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "no marker \"missing\" (markers: intro, loop)"), 1u);
}

TEST_F(LottieElementTest, ManifestSettingsAndAttributeOverrides){
    JASPER_REQUIRE_GPU();
    UILottieElement* lottie = load("src=\"stored_v1.lottie\"");
    ASSERT_NE(lottie, nullptr);
    settle(0.0);
    ASSERT_TRUE(lottie->isLoaded());
    // the active animation "second": autoplay, speed 2, direction -1, bounce, loop 3
    EXPECT_TRUE(lottie->isPlaying());
    EXPECT_EQ(events.types, (std::vector<std::string>{"load", "play"}));
    const LottiePlayback& playback = lottie->getPlayback();
    EXPECT_FLOAT_EQ(playback.getSpeed(), 2.0f);
    EXPECT_EQ(playback.getDirection(), -1);
    EXPECT_EQ(playback.getMode(), LottiePlayback::Mode::BOUNCE);
    EXPECT_EQ(playback.getLoopCount(), 3);

    // the attributes win; another animation by id
    events.clear();
    lottie->SetAttribute("animation", "first");
    lottie->SetAttribute("speed", "1");
    settle(0.0);
    EXPECT_EQ(events.types, std::vector<std::string>{"load"});
    EXPECT_FALSE(lottie->isPlaying());
    EXPECT_FLOAT_EQ(playback.getSpeed(), 1.0f);
    EXPECT_EQ(playback.getDirection(), 1);
    EXPECT_EQ(playback.getLoopCount(), 1);
    EXPECT_EQ(lottie->getStats().loads, 2u);
}

TEST_F(LottieElementTest, DeflatedV2WithMarker){
    JASPER_REQUIRE_GPU();
    UILottieElement* lottie = load("src=\"deflate_v2.lottie\" marker=\"loop\"");
    ASSERT_NE(lottie, nullptr);
    frame(0.0);
    ASSERT_TRUE(lottie->isLoaded());
    EXPECT_EQ(lottie->getCurrentFrame(), 30.0f);
    EXPECT_FALSE(lottie->isPlaying());
}

TEST_F(LottieElementTest, ErrorsDispatchAnErrorEvent){
    JASPER_REQUIRE_GPU();
    struct Case {
        std::string attributes;
        std::string message;
    };
    const Case cases[] = {
        {"src=\"missing.json\"", "could not read 'p1/lottie/missing.json'"},
        {"src=\"zip64.lottie\"", "ZIP64 archives are not supported"},
        {"src=\"encrypted.lottie\"", "encrypted ZIP archives are not supported"},
        {"src=\"bad_manifest.lottie\"", "lists no animations"},
        {"src=\"stored_v1.lottie\" animation=\"third\"", "has no animation 'third'"},
        {"src=\"../texturedata/units.svg\"", "is not a Lottie animation"},
    };
    for (const Case& c : cases){
        events.clear();
        TestSystem::get().clearLogs();
        if (document)
            document->Close();
        UILottieElement* lottie = load(c.attributes);
        ASSERT_NE(lottie, nullptr);
        settle(0.0);
        EXPECT_FALSE(lottie->isLoaded()) << c.attributes;
        EXPECT_EQ(events.types, std::vector<std::string>{"error"}) << c.attributes;
        ASSERT_EQ(events.messages.size(), 1u) << c.attributes;
        EXPECT_NE(events.messages[0].find(c.message), std::string::npos) << events.messages[0];
        EXPECT_EQ(lottie->getError(), events.messages[0]);
        EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, c.message), 1u) << c.attributes;
        // an element without an animation draws nothing and its controls do nothing
        EXPECT_EQ(lottie->getTexture(), nullptr);
        lottie->play();
        EXPECT_FALSE(lottie->isPlaying());
    }
    // no source: nothing to load, no event
    events.clear();
    document->Close();
    UILottieElement* empty = load("");
    ASSERT_NE(empty, nullptr);
    settle(0.0);
    EXPECT_TRUE(events.types.empty());
    EXPECT_FALSE(empty->isLoaded());
}

TEST_F(LottieElementTest, EmbeddedAndExternalImages){
    JASPER_REQUIRE_GPU();
    for (const char* source : {"image_v2.lottie", "image_ext.json"}){
        if (document)
            document->Close();
        UILottieElement* lottie = load(std::string("src=\"") + source + "\"");
        ASSERT_NE(lottie, nullptr);
        frame(0.0);
        frame(0.0);
        ASSERT_TRUE(lottie->isLoaded()) << source;
        HtmlUITexture* texture = lottie->getTexture();
        ASSERT_NE(texture, nullptr) << source;
        std::vector<uint8_t> pixels;
        ASSERT_TRUE(readTexturePixels(*texture->render, 100, 100, 4, pixels));
        const uint8_t* centre = &pixels[(50 * 100 + 50) * 4];
        EXPECT_EQ(centre[0], 255) << source;
        EXPECT_EQ(centre[1], 0) << source;
        EXPECT_EQ(centre[2], 0) << source;
        EXPECT_EQ(centre[3], 255) << source;
    }
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "not found"), 0u);
}

TEST_F(LottieElementTest, PremultipliedOutput){
    JASPER_REQUIRE_GPU();
    UILottieElement* lottie = load("src=\"shapes.json\"");
    ASSERT_NE(lottie, nullptr);
    frame(0.0);
    frame(0.0);
    std::vector<uint8_t> pixels;
    ASSERT_TRUE(readTexturePixels(*lottie->getTexture()->render, 100, 100, 4, pixels));
    size_t partial = 0;
    for (size_t i = 0; i < pixels.size(); i += 4){
        EXPECT_LE(pixels[i + 0], pixels[i + 3]);
        EXPECT_LE(pixels[i + 1], pixels[i + 3]);
        EXPECT_LE(pixels[i + 2], pixels[i + 3]);
        partial += (pixels[i + 3] > 0 && pixels[i + 3] < 255) ? 1 : 0;
    }
    EXPECT_GT(partial, 0u);   // antialiased edges exist, so the check above means something
}

TEST_F(LottieElementTest, ViewLossRecreatesTheTexture){
    JASPER_REQUIRE_GPU();
    UILottieElement* lottie = load("src=\"shapes.json\"");
    ASSERT_NE(lottie, nullptr);
    frame(0.0);
    HtmlUITexture* first = lottie->getTexture();
    ASSERT_NE(first, nullptr);
    // what HtmlUIContext::releaseGpuResources does: RmlUi gives every texture back
    Rml::ReleaseTextures(&recording);
    EXPECT_EQ(recording.released, std::set<HtmlUITexture*>{first});
    frame(0.0);
    EXPECT_EQ(lottie->getStats().texturesCreated, 2u);
    ASSERT_NE(lottie->getTexture(), nullptr);
    EXPECT_EQ(drawnTexture(), reinterpret_cast<Rml::TextureHandle>(lottie->getTexture()));
    // the current frame went up with it, without another rasterisation
    EXPECT_EQ(lottie->getStats().rasterizations, 1u);
    EXPECT_EQ(lottie->getStats().uploads, 2u);
}

TEST_F(LottieElementTest, UnloadReleasesTheTexture){
    JASPER_REQUIRE_GPU();
    UILottieElement* lottie = load("src=\"shapes.json\"");
    ASSERT_NE(lottie, nullptr);
    frame(0.0);
    HtmlUITexture* texture = lottie->getTexture();
    ASSERT_NE(texture, nullptr);
    document->Close();
    context->Update();
    document = nullptr;
    EXPECT_EQ(recording.released, std::set<HtmlUITexture*>{texture});
    EXPECT_EQ(recording.getLiveGeometryCount(), 0u);
}

TEST_F(LottieElementTest, ControlApiThroughUIElement){
    JASPER_REQUIRE_GPU();
    UILottieElement* lottie = load("src=\"shapes.json\"");
    ASSERT_NE(lottie, nullptr);
    settle(0.0);
    events.clear();

    UIElement element = UIElementAccess::wrap(lottie);
    ASSERT_TRUE(element.isValid());
    EXPECT_FLOAT_EQ(element.getTotalFrames(), 60.0f);
    EXPECT_FLOAT_EQ(element.getDuration(), 2.0f);

    element.play();
    EXPECT_TRUE(element.isPlaying());
    EXPECT_EQ(events.types, std::vector<std::string>{"play"});
    HtmlUIClock::setFixedTime(0.5);
    EXPECT_EQ(element.getCurrentFrame(), 15.0f);
    element.pause();
    EXPECT_FALSE(element.isPlaying());
    EXPECT_EQ(events.types, (std::vector<std::string>{"play", "pause"}));

    element.seek(1.0f);
    EXPECT_EQ(element.getCurrentFrame(), 30.0f);
    element.setFrame(45.0f);
    EXPECT_EQ(element.getCurrentFrame(), 45.0f);
    frame(0.5);
    EXPECT_EQ(lottie->getStats().rasterizations, 2u);   // frame 0, then 45

    element.setSpeed(2.0f);
    element.setDirection(-1);
    element.setLoop(true);
    EXPECT_EQ(lottie->getPlayback().getLoopCount(), 0);
    element.setLoopCount(2);
    EXPECT_EQ(lottie->getPlayback().getLoopCount(), 2);
    element.setSegment(10.0f, 20.0f);
    EXPECT_EQ(element.getCurrentFrame(), 19.0f);
    element.setMarker("intro");
    EXPECT_FLOAT_EQ(lottie->getPlayback().getSegmentEnd(), 30.0f);
    EXPECT_FLOAT_EQ(lottie->getPlayback().getSpeed(), 2.0f);
    element.stop();
    EXPECT_FALSE(element.isPlaying());
    EXPECT_EQ(element.getCurrentFrame(), 29.0f);   // direction -1 starts at the end

    // invalid arguments warn, the state stays
    element.setSpeed(-1.0f);
    element.setDirection(0);
    EXPECT_FLOAT_EQ(lottie->getPlayback().getSpeed(), 2.0f);
    EXPECT_EQ(lottie->getPlayback().getDirection(), -1);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "setSpeed(-1)"), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "setDirection(0)"), 1u);

    // complete through the clock: speed 2, frames 29 -> 0 twice (loop 2) = 30 frames / 60 fps x 2
    element.play();
    frame(0.6);
    frame(1.6);
    EXPECT_EQ(events.count("loop"), 1u);
    EXPECT_EQ(events.count("complete"), 1u);
    EXPECT_EQ(element.getCurrentFrame(), 0.0f);
}

TEST_F(LottieElementTest, ControlApiOnOtherElementsLogsOnce){
    JASPER_REQUIRE_GPU();
    ASSERT_NE(load("src=\"shapes.json\""), nullptr);
    frame(0.0);
    UIElement plain = UIElementAccess::wrap(document->GetElementById("plain"));
    ASSERT_TRUE(plain.isValid());
    plain.play();
    plain.play();
    EXPECT_FALSE(plain.isPlaying());
    EXPECT_EQ(plain.getTotalFrames(), 0.0f);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "UIElement::play: <div> is not a <lottie> element"), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "UIElement::isPlaying: <div> is not a <lottie> element"), 1u);

    // a dead handle: the lifetime message
    UIElement anim = UIElementAccess::wrap(document->GetElementById("anim"));
    document->Close();
    context->Update();
    document = nullptr;
    EXPECT_FALSE(anim.isValid());
    TestSystem::get().clearLogs();
    anim.pause();
    // as every UIElement method on a dead handle: the [HTML UI] warning, once per method
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] UIElement: element no longer exists"), 1u);
}
