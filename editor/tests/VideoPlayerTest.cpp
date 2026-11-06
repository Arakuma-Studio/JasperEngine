// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Video Player component (feature video-mp4, docs/superpowers/specs/2026-10-31-video-mp4-design.md
// §9): VideoPlayerSystem against a fake source (event order, game vs unscaled time, timeScale 0,
// stop and prepare again, Material Override restored, a player destroyed while preparing), the
// Catalog and scene-file round trip, the video|<entity> texture links (resolve, remap on
// duplicate), the cloner, and the export codegen.

#include "PhysicsTestScene.h"

#include "Catalog.h"
#include "Exporter.h"
#include "Factory.h"
#include "Stream.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/VideoPlayerComponent.h"
#include "htmlui/HtmlUIClock.h"
#include "media/IVideoSource.h"
#include "media/VideoSourceFactory.h"
#include "media/VideoTextureConverter.h"
#include "object/VideoPlayer.h"
#include "registry/EntityCloner.h"
#include "subsystem/VideoPlayerSystem.h"
#include "texture/Framebuffer.h"
#include "util/CameraTextureLink.h"
#include "util/VideoTextureLink.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::atomic<int> fakeSources{0};

    // a 1 s, 10 fps video: READY after `openPumps` getState calls, any frame of a time is there
    class FakeSource : public IVideoSource {
    public:
        explicit FakeSource(int openPumps): pumps(openPumps){
            fakeSources++;
            info.hasVideo = true;
            info.width = info.height = info.displayWidth = info.displayHeight = 4;
            info.frameRate = 10.0;
            info.frameCount = 10;
            info.duration = 1.0;
            info.durationNs = 1000000000;
            info.videoCodec = "avc1";
            info.container = "mp4";
        }
        ~FakeSource() override{ fakeSources--; }

        State getState() const override{
            if (pumps > 0){
                const_cast<FakeSource*>(this)->pumps--;
                return State::OPENING;
            }
            return failing ? State::FAILED : State::READY;
        }
        State waitOpen() override{ return getState(); }
        const VideoInfo& getInfo() const override{ return info; }
        MediaError getError() const override{
            MediaError error;
            if (failing){
                error.code = MediaErrorCode::SRC_NOT_SUPPORTED;
                error.message = "HEVC video does not play here";
            }
            return error;
        }
        void setLoop(bool value) override{ loop = value; }
        FrameResult acquireFrame(int iteration, double seconds, bool wait, int64_t shown, VideoFrame& frame) override{
            if (waitRequested) *waitRequested = *waitRequested || wait;
            const int64_t sequence = sequenceFor(iteration, seconds);
            if (sequence == shown)
                return FrameResult::SAME;
            frame = VideoFrame();
            frame.width = frame.height = 4;
            frame.y.assign(16, 128);
            frame.u.assign(4, 128);
            frame.v.assign(4, 128);
            frame.sequence = sequence;
            frame.index = frameIndexFor(seconds);
            acquired++;
            return FrameResult::NEW;
        }
        int64_t sequenceFor(int iteration, double seconds) const override{ return (int64_t)iteration * 10 + frameIndexFor(seconds); }
        int frameIndexFor(double seconds) const override{ return std::clamp((int)(seconds * 10.0 + 1e-6), 0, 9); }
        double keyframeTimeAtOrBefore(double) const override{ return 0.0; }
        void syncAudio(int64_t) override{}
        bool isAudioSyncPending() const override{ return false; }
        void waitAudioSync() override{}
        std::shared_ptr<VideoAudioRing> getAudioRing() const override{ return nullptr; }
        bool pump(int) override{ return false; }
        VideoStats getStats() const override{ return VideoStats(); }
        double getBufferedSeconds() const override{ return 1.0; }
        bool isFrameExact() const override{ return true; }
        const char* backendName() const override{ return "fake"; }

        int pumps;
        bool failing = false;
        bool loop = false;
        int acquired = 0;
        bool* waitRequested = nullptr;
        VideoInfo info;
    };

    class VideoPlayerSystemTest : public ::testing::Test {
    protected:
        const bool installed = physicstest::installTestSystem();
        Scene scene;
        std::vector<std::string> events;
        int openPumps = 2;
        bool failing = false;
        bool waitRequested = false;
        std::vector<std::string> opened;

        void SetUp() override{
            HtmlUIClock::setFixedTime(std::nullopt);
            VideoPlayerSystem::setAudioOutputForTesting(false);
            VideoSourceFactory::setSourceOverrideForTesting([this](const std::string& path, const VideoSourceFactory::Options&) -> std::unique_ptr<IVideoSource> {
                opened.push_back(path);
                auto source = std::make_unique<FakeSource>(openPumps);
                source->failing = failing;
                source->waitRequested = &waitRequested;
                return source;
            });
        }
        void TearDown() override{
            VideoSourceFactory::setSourceOverrideForTesting(nullptr);
            VideoPlayerSystem::setAudioOutputForTesting(true);
        }

        std::shared_ptr<VideoPlayerSystem> system(){ return scene.getSystem<VideoPlayerSystem>(); }

        Entity makePlayer(bool playOnAwake = true, VideoTimeUpdateMode mode = VideoTimeUpdateMode::GAME_TIME){
            const Entity entity = scene.createUserEntity();
            scene.addComponent<Transform>(entity, {});
            VideoPlayerComponent player;
            player.clip = "ads/ad1.mp4";
            player.playOnAwake = playOnAwake;
            player.timeUpdateMode = mode;
            player.audioOutputMode = VideoAudioOutputMode::NONE;
            scene.addComponent<VideoPlayerComponent>(entity, player);
            VideoPlayerComponent& added = scene.getComponent<VideoPlayerComponent>(entity);
            added.onPrepareCompleted = std::function<void(VideoPlayer)>([this](VideoPlayer){ events.push_back("prepared"); });
            added.onStarted = std::function<void(VideoPlayer)>([this](VideoPlayer){ events.push_back("started"); });
            added.onLoopPointReached = std::function<void(VideoPlayer)>([this](VideoPlayer){ events.push_back("loop"); });
            added.onSeekCompleted = std::function<void(VideoPlayer)>([this](VideoPlayer){ events.push_back("seeked"); });
            added.onErrorReceived = std::function<void(VideoPlayer, const std::string&)>([this](VideoPlayer, const std::string& m){ events.push_back("error:" + m); });
            return entity;
        }

        // frames of `dt` seconds (game time) with the UI clock moving by `unscaled`
        void run(int frames, double dt, double unscaled = -1.0){
            for (int i = 0; i < frames; i++){
                HtmlUIClock::advance(unscaled < 0.0 ? dt : unscaled);
                system()->update(dt);
            }
        }
    };

}

TEST_F(VideoPlayerSystemTest, PlaysOnAwakeWithEventsInOrder){
    const Entity entity = makePlayer();
    VideoPlayer player(&scene, entity);
    run(1, 0.0);
    EXPECT_FALSE(player.isPrepared());
    EXPECT_EQ(opened, (std::vector<std::string>{"ads/ad1.mp4"}));
    run(3, 0.05);
    ASSERT_TRUE(player.isPrepared());
    EXPECT_TRUE(player.isPlaying());
    EXPECT_EQ(events, (std::vector<std::string>{"prepared", "started"}));
    EXPECT_STREQ(player.getBackend().c_str(), "fake");
    EXPECT_EQ(player.getVideoCodec(), "H.264");
    EXPECT_EQ(player.getFrameCount(), 10);
    EXPECT_DOUBLE_EQ(player.getLength(), 1.0);
    run(30, 0.05);
    EXPECT_EQ(events, (std::vector<std::string>{"prepared", "started", "loop"})) << "once at the end of a play that does not loop";
    EXPECT_FALSE(player.isPlaying());
    EXPECT_EQ(player.getFrame(), 9);
}

TEST_F(VideoPlayerSystemTest, LoopsFireAtEachWrap){
    const Entity entity = makePlayer();
    scene.getComponent<VideoPlayerComponent>(entity).isLooping = true;
    VideoPlayer player(&scene, entity);
    run(4, 0.0);
    ASSERT_TRUE(player.isPrepared());
    run(50, 0.05);   // 2.5 s
    EXPECT_EQ(std::count(events.begin(), events.end(), "loop"), 2);
    EXPECT_TRUE(player.isPlaying());
    EXPECT_NEAR(player.getTime(), 0.5, 0.06);
}

TEST_F(VideoPlayerSystemTest, PlayBeforePreparedPreparesThenPlays){
    const Entity entity = makePlayer(false);
    VideoPlayer player(&scene, entity);
    run(3, 0.05);
    EXPECT_TRUE(opened.empty()) << "no playOnAwake: nothing opened";
    player.play();
    run(1, 0.05);
    EXPECT_FALSE(player.isPrepared());
    run(3, 0.05);
    EXPECT_TRUE(player.isPrepared());
    EXPECT_EQ(events, (std::vector<std::string>{"prepared", "started"}));
}

TEST_F(VideoPlayerSystemTest, GameTimeFollowsTimeScaleAndUnscaledDoesNot){
    const Entity game = makePlayer(true, VideoTimeUpdateMode::GAME_TIME);
    const Entity real = makePlayer(true, VideoTimeUpdateMode::UNSCALED_GAME_TIME);
    VideoPlayer gamePlayer(&scene, game), realPlayer(&scene, real);
    run(4, 0.0, 0.0);
    ASSERT_TRUE(gamePlayer.isPrepared());
    ASSERT_TRUE(realPlayer.isPrepared());
    // timeScale 0: the game delta is 0, real time moves on
    run(10, 0.0, 0.05);
    EXPECT_NEAR(gamePlayer.getTime(), 0.0, 1e-9);
    EXPECT_NEAR(realPlayer.getTime(), 0.5, 0.06);
    // half speed
    run(10, 0.025, 0.05);
    EXPECT_NEAR(gamePlayer.getTime(), 0.25, 0.03);
    EXPECT_NEAR(realPlayer.getTime(), 1.0, 0.06);
}

TEST_F(VideoPlayerSystemTest, SeekAndStepForward){
    const Entity entity = makePlayer();
    VideoPlayer player(&scene, entity);
    run(4, 0.0);
    player.pause();
    player.setTime(0.55);
    run(1, 0.05);
    EXPECT_EQ(player.getFrame(), 5);
    EXPECT_EQ(std::count(events.begin(), events.end(), "seeked"), 1);
    player.stepForward();
    run(1, 0.05);
    EXPECT_EQ(player.getFrame(), 6);
    EXPECT_TRUE(player.isPaused());
    player.setFrame(2);
    run(1, 0.05);
    EXPECT_EQ(player.getFrame(), 2);
}

TEST_F(VideoPlayerSystemTest, StopReleasesAndPrepareOpensAgain){
    const Entity entity = makePlayer();
    VideoPlayer player(&scene, entity);
    run(4, 0.05);
    ASSERT_TRUE(player.isPrepared());
    EXPECT_EQ(fakeSources.load(), 1);
    player.stop();
    EXPECT_FALSE(player.isPrepared());
    EXPECT_EQ(fakeSources.load(), 0) << "the decoder is released";
    EXPECT_EQ(system()->getOpenSources(), 0);
    player.prepare();
    run(4, 0.05);
    EXPECT_TRUE(player.isPrepared());
    EXPECT_FALSE(player.isPlaying()) << "prepare does not play";
    EXPECT_EQ(std::count(events.begin(), events.end(), "prepared"), 2);
}

TEST_F(VideoPlayerSystemTest, PausedHoldsPlayOnAwakeAndResetArmsItAgain){
    // the editor's edit mode: the system is paused, a player made then set later plays at Play
    const Entity entity = makePlayer();
    scene.getComponent<VideoPlayerComponent>(entity).clip = "";
    system()->setPaused(true);
    run(3, 0.05);
    scene.getComponent<VideoPlayerComponent>(entity).clip = "ads/ad1.mp4";
    run(3, 0.05);
    EXPECT_TRUE(opened.empty()) << "nothing plays while paused";
    system()->setPaused(false);
    run(4, 0.05);
    VideoPlayer player(&scene, entity);
    EXPECT_TRUE(player.isPlaying()) << "playOnAwake at the first unpaused update";
    // Play stop: everything closes; the next Play runs playOnAwake again
    system()->resetPlayers();
    EXPECT_FALSE(player.isPrepared());
    EXPECT_EQ(system()->getOpenSources(), 0);
    run(4, 0.05);
    EXPECT_TRUE(player.isPlaying());
    EXPECT_EQ(opened.size(), 2u);
}

TEST_F(VideoPlayerSystemTest, AnotherClipStopsThePlayer){
    const Entity entity = makePlayer();
    VideoPlayer player(&scene, entity);
    run(4, 0.05);
    ASSERT_TRUE(player.isPrepared());
    player.setClip("ads/ad2.mp4");
    run(2, 0.05);
    EXPECT_FALSE(player.isPrepared());
    EXPECT_EQ(opened.size(), 1u) << "playOnAwake does not start it again";
    // a new url and prepare() at once (the ad flow): the new one opens
    player.setClip("ads/ad3.mp4");
    player.prepare();
    run(4, 0.05);
    EXPECT_TRUE(player.isPrepared());
    EXPECT_EQ(opened.back(), "ads/ad3.mp4");
}

TEST_F(VideoPlayerSystemTest, ErrorsReachTheHandler){
    failing = true;
    openPumps = 0;
    const Entity entity = makePlayer();
    VideoPlayer player(&scene, entity);
    run(2, 0.05);
    EXPECT_EQ(events, (std::vector<std::string>{"error:HEVC video does not play here"}));
    EXPECT_FALSE(player.isPrepared());
    EXPECT_EQ(player.getError(), "HEVC video does not play here");
    run(3, 0.05);
    EXPECT_EQ(events.size(), 1u) << "once";
}

TEST_F(VideoPlayerSystemTest, DestroyedWhilePreparingClosesTheSource){
    openPumps = 100;
    const Entity entity = makePlayer();
    run(2, 0.05);
    EXPECT_EQ(fakeSources.load(), 1);
    scene.destroyEntity(entity);
    EXPECT_EQ(fakeSources.load(), 0);
    run(2, 0.05);
    EXPECT_TRUE(events.empty());
}

TEST_F(VideoPlayerSystemTest, MaterialOverrideIsRestored){
    const Entity screen = scene.createUserEntity();
    scene.addComponent<Transform>(screen, {});
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    mesh.submeshes[0].material.baseColorTexture.setPath("tv.png");
    scene.addComponent<MeshComponent>(screen, mesh);
    const Entity entity = makePlayer();
    VideoPlayerComponent& component = scene.getComponent<VideoPlayerComponent>(entity);
    component.renderMode = VideoRenderMode::MATERIAL_OVERRIDE;
    component.targetMaterialRenderer = screen;
    run(4, 0.05);
    // as if the first frame had been converted (no GPU here)
    component.output->setPictureForTesting(4, 4);
    run(1, 0.05);
    Texture& slot = scene.getComponent<MeshComponent>(screen).submeshes[0].material.baseColorTexture;
    EXPECT_EQ(slot.getFramebuffer(), component.output->getFramebuffer()) << "the player's texture in the slot";
    VideoPlayer(&scene, entity).stop();
    EXPECT_EQ(slot.getPath(), "tv.png") << "its own texture again";
    EXPECT_EQ(slot.getFramebuffer(), nullptr);

    // a renderer without the slot: nothing overridden
    VideoPlayerComponent& again = scene.getComponent<VideoPlayerComponent>(entity);
    again.targetMaterialProperty = "noSuchTexture";
    VideoPlayer(&scene, entity).play();
    run(4, 0.05);
    again.output->setPictureForTesting(4, 4);
    run(1, 0.05);
    EXPECT_EQ(slot.getPath(), "tv.png");
}

// ---------------------------------------------------------------- editor side

TEST(VideoPlayerComponent, CatalogAndNames){
    const ComponentType type = ComponentType::VideoPlayerComponent;
    // appended after the Text Effect (prefab override masks use the ordinal)
    EXPECT_EQ(static_cast<int>(type), static_cast<int>(ComponentType::TextEffectComponent) + 1);
    EXPECT_GE(static_cast<int>(LastComponentType), static_cast<int>(type));
    EXPECT_EQ(Catalog::getComponentName(type, true), "videoplayer");
    EXPECT_EQ(Catalog::getComponentDisplayName(type), "Video Player");
    EXPECT_EQ(Catalog::getComponentType("videoplayer"), type);
    EXPECT_TRUE(Catalog::hasEnabledProperty(type));
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<VideoPlayerComponent>(entity, {});
    VideoPlayerComponent& player = scene.getComponent<VideoPlayerComponent>(entity);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "url").ref, &player.url);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "renderMode").type, PropertyType::Enum);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "targetCamera").type, PropertyType::Entity);
    EXPECT_TRUE(player.output) << "the render texture exists before anything plays (links bind it)";
}

TEST(VideoPlayerComponent, StreamRoundTripKeepsTheAuthoredFields){
    VideoPlayerComponent player;
    player.source = VideoPlayerSource::URL;
    player.url = "data://ads/ad1.mp4";
    player.playOnAwake = false;
    player.isLooping = true;
    player.playbackSpeed = 2.0f;
    player.timeUpdateMode = VideoTimeUpdateMode::UNSCALED_GAME_TIME;
    player.renderMode = VideoRenderMode::CAMERA_FAR_PLANE;
    player.targetCamera = 7;
    player.aspectRatio = VideoAspectRatio::FIT_OUTSIDE;
    player.renderTextureWidth = 1920;
    player.renderTextureHeight = 1080;
    player.audioOutputMode = VideoAudioOutputMode::AUDIO_SOURCE;
    player.directVolume = 0.25f;
    const YAML::Node node = Stream::encodeVideoPlayerComponent(player);
    EXPECT_EQ(node["renderMode"].as<std::string>(), "camera_far_plane");
    EXPECT_EQ(node["timeUpdateMode"].as<std::string>(), "unscaled_game_time");
    EXPECT_FALSE(node["clip"]) << "defaults are not written";
    const VideoPlayerComponent decoded = Stream::decodeVideoPlayerComponent(node);
    EXPECT_EQ(decoded.source, VideoPlayerSource::URL);
    EXPECT_EQ(decoded.url, "data://ads/ad1.mp4");
    EXPECT_FALSE(decoded.playOnAwake);
    EXPECT_TRUE(decoded.isLooping);
    EXPECT_EQ(decoded.playbackSpeed, 2.0f);
    EXPECT_EQ(decoded.renderMode, VideoRenderMode::CAMERA_FAR_PLANE);
    EXPECT_EQ(decoded.targetCamera, 7u);
    EXPECT_EQ(decoded.aspectRatio, VideoAspectRatio::FIT_OUTSIDE);
    EXPECT_EQ(decoded.renderTextureWidth, 1920u);
    EXPECT_EQ(decoded.audioOutputMode, VideoAudioOutputMode::AUDIO_SOURCE);
    EXPECT_EQ(decoded.directVolume, 0.25f);
    EXPECT_EQ(YAML::Dump(Stream::encodeVideoPlayerComponent(decoded)), YAML::Dump(node));
    // an empty node (an old file, a missing key) is the defaults
    const VideoPlayerComponent defaults = Stream::decodeVideoPlayerComponent(YAML::Node());
    EXPECT_EQ(defaults.renderMode, VideoRenderMode::RENDER_TEXTURE);
    EXPECT_TRUE(defaults.playOnAwake);
    EXPECT_EQ(defaults.targetMaterialProperty, "baseColorTexture");
    // out-of-range values are clamped
    YAML::Node odd;
    odd["playbackSpeed"] = 100.0f;
    odd["directVolume"] = -3.0f;
    odd["renderMode"] = "nonsense";
    const VideoPlayerComponent clamped = Stream::decodeVideoPlayerComponent(odd);
    EXPECT_EQ(clamped.playbackSpeed, 16.0f);
    EXPECT_EQ(clamped.directVolume, 0.0f);
    EXPECT_EQ(clamped.renderMode, VideoRenderMode::RENDER_TEXTURE);
}

TEST(VideoPlayerComponent, TextureLinksResolveAndFollowCopies){
    Scene scene;
    const Entity player = scene.createUserEntity();
    scene.addComponent<Transform>(player, {});
    scene.addComponent<VideoPlayerComponent>(player, {});
    const Entity image = scene.createUserEntity();
    scene.addComponent<Transform>(image, {});
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    mesh.submeshes[0].material.baseColorTexture.setId(VideoTextureLink::makeId(player));
    scene.addComponent<MeshComponent>(image, mesh);

    EXPECT_EQ(CameraTextureLink::parseLink("video|" + std::to_string(player)), player);
    EXPECT_TRUE(CameraTextureLink::isLinkSource(&scene, player));
    CameraTextureLink::resolve(&scene);
    Texture& texture = scene.getComponent<MeshComponent>(image).submeshes[0].material.baseColorTexture;
    Framebuffer* output = scene.getComponent<VideoPlayerComponent>(player).output->getFramebuffer();
    EXPECT_EQ(texture.getFramebuffer(), output);
    EXPECT_EQ(texture.getId(), VideoTextureLink::makeId(player)) << "the link id stays";
    EXPECT_EQ(VideoTextureLink::videoName(&scene, texture), scene.getEntityName(player));
    EXPECT_TRUE(VideoTextureLink::isVideoUsed(&scene, player));
    // the scene file keeps the link
    const YAML::Node encoded = Stream::encodeMeshComponent(scene.getComponent<MeshComponent>(image), false, false, false);
    const std::string text = YAML::Dump(encoded);
    EXPECT_NE(text.find("source: video"), std::string::npos) << text;
    EXPECT_EQ(Stream::decodeMeshComponent(encoded).submeshes[0].material.baseColorTexture.getId(), VideoTextureLink::makeId(player));

    // a copy of both: the copy's texture shows the copy's player
    const std::vector<Entity> copies = EntityCloner::clone(&scene, {player, image}, &scene);
    ASSERT_EQ(copies.size(), 2u);
    VideoPlayerComponent& copiedPlayer = scene.getComponent<VideoPlayerComponent>(copies[0]);
    ASSERT_TRUE(copiedPlayer.output);
    EXPECT_NE(copiedPlayer.output.get(), scene.getComponent<VideoPlayerComponent>(player).output.get()) << "its own render texture";
    const Texture& copiedTexture = scene.getComponent<MeshComponent>(copies[1]).submeshes[0].material.baseColorTexture;
    EXPECT_EQ(copiedTexture.getFramebuffer(), copiedPlayer.output->getFramebuffer());
    EXPECT_EQ(copiedTexture.getId(), VideoTextureLink::makeId(copies[0]));
}

TEST(VideoPlayerComponent, ExportCodegen){
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    VideoPlayerComponent player;
    player.url = "https://cdn.example.com/a.mp4";
    player.source = VideoPlayerSource::URL;
    player.renderMode = VideoRenderMode::CAMERA_NEAR_PLANE;
    scene.addComponent<VideoPlayerComponent>(entity, player);
    const std::string code = Factory::createVideoPlayerComponent(4, &scene, entity, "scene", "entity");
    EXPECT_NE(code.find("VideoPlayerComponent player;"), std::string::npos) << code;
    EXPECT_NE(code.find("player.url = \"https://cdn.example.com/a.mp4\";"), std::string::npos) << code;
    EXPECT_NE(code.find("player.renderMode = VideoRenderMode(1);"), std::string::npos) << code;
    EXPECT_EQ(code.find("player.clip"), std::string::npos) << "defaults are not written";
}

TEST(VideoPlayerComponent, AndroidInternetForUrlPlayers){
    // the scene file's text as Stream writes it: an http(s) url needs INTERNET, a clip or a
    // data:// url does not
    VideoPlayerComponent player;
    player.source = VideoPlayerSource::URL;
    player.url = "https://cdn.example.com/ad.mp4";
    YAML::Node node;
    node["components"]["videoplayer"] = Stream::encodeVideoPlayerComponent(player);
    EXPECT_TRUE(Exporter::hasUrlVideoPlayer(YAML::Dump(node))) << YAML::Dump(node);
    player.url = "data://ads/ad1.mp4";
    node["components"]["videoplayer"] = Stream::encodeVideoPlayerComponent(player);
    EXPECT_FALSE(Exporter::hasUrlVideoPlayer(YAML::Dump(node)));
    EXPECT_FALSE(Exporter::hasUrlVideoPlayer("components:\n  sprite:\n    url: https://x.example/a.png\n"));
}

TEST(VideoPlayerComponent, AndroidInternetCheckParsesTheYaml){
    // a scene line of 100 KB (an embedded buffer) does not overflow the check (no regex over it)
    YAML::Node node;
    node["entities"][0]["components"]["mesh"]["data"] = std::string(100 * 1024, 'A');
    VideoPlayerComponent player;
    player.url = "HTTPS://cdn.example.com/ad.mp4";
    node["entities"][1]["components"]["videoplayer"] = Stream::encodeVideoPlayerComponent(player);
    EXPECT_TRUE(Exporter::hasUrlVideoPlayer(YAML::Dump(node))) << "the scheme in any case";
    // a player without a url and another GameObject's url: nothing
    YAML::Node other;
    other["entities"][0]["components"]["videoplayer"]["clip"] = "assets/video/a.mp4";
    other["entities"][1]["components"]["sprite"]["url"] = "https://cdn.example.com/b.png";
    EXPECT_FALSE(Exporter::hasUrlVideoPlayer(YAML::Dump(other)));
    // not YAML at all
    EXPECT_FALSE(Exporter::hasUrlVideoPlayer("videoplayer: [unclosed"));
}

TEST(VideoPlayerComponent, AndroidInternetForScriptsSettingAUrl){
    const fs::path root = fs::temp_directory_path() / "jasper-video-internet";
    fs::remove_all(root);
    fs::create_directories(root / "lua");
    fs::create_directories(root / "assets/ui");
    auto write = [&](const std::string& rel, const std::string& text){ std::ofstream(root / rel) << text; };
    write("lua/Plain.lua", "local x = 1\n");
    write("lua/Ad.lua", "local vp = self.gameObject:getVideoPlayer()\nvp.url = base .. '/ad.mp4'\n");
    write("assets/ui/menu.html", "<html><body><video src=\"ui/intro.webm\"></video></body></html>");
    write("assets/ui/promo.html", "<html><body><video autoplay src=\"https://cdn.example.com/p.mp4\"></video></body></html>");
    EXPECT_EQ(Exporter::scriptVideoUrlFile(root, {"lua/Plain.lua", "assets/ui/menu.html"}), "");
    EXPECT_EQ(Exporter::scriptVideoUrlFile(root, {"lua/Plain.lua", "lua/Ad.lua"}), "lua/Ad.lua");
    EXPECT_EQ(Exporter::scriptVideoUrlFile(root, {"assets/ui/promo.html"}), "assets/ui/promo.html");
    fs::remove_all(root);
}

TEST(VideoPlayerComponent, AnImageOutlivesItsPlayer){
    // the ad flow: image:setTexture(player:getFramebuffer()), then the player's GameObject goes
    const int liveConverters = VideoTextureConverter::getLiveCount();
    Scene scene;
    const Entity player = scene.createUserEntity();
    scene.addComponent<Transform>(player, {});
    scene.addComponent<VideoPlayerComponent>(player, {});
    VideoPlayerComponent& component = scene.getComponent<VideoPlayerComponent>(player);
    if (!component.output)
        component.output = std::make_shared<VideoTextureConverter>();
    Texture shown(component.output->getFramebuffer());
    std::weak_ptr<Framebuffer> owner = component.output->getFramebuffer()->weak_from_this();
    ASSERT_FALSE(owner.expired()) << "the render texture is shared (Texture keeps its owner)";
    // (the component and its output go now: `component` is not to be touched after this)
    scene.destroyEntity(player);
    EXPECT_EQ(VideoTextureConverter::getLiveCount(), liveConverters) << "the player's converter went with it";
    EXPECT_FALSE(owner.expired()) << "the Image's texture keeps the object alive";
    ASSERT_NE(shown.getFramebuffer(), nullptr);
    EXPECT_FALSE(shown.getFramebuffer()->isCreated()) << "its GPU side is gone: the texture draws its fallback";
}

TEST(VideoPlayerComponent, LinksFollowRemapsAndDeletes){
    // a prefab boundary or paste retargets video|<id> to the copy's player (CameraTextureLink::retarget)
    Scene scene;
    const Entity a = scene.createUserEntity();
    const Entity b = scene.createUserEntity();
    for (Entity e : {a, b}){
        scene.addComponent<Transform>(e, {});
        scene.addComponent<VideoPlayerComponent>(e, {});
    }
    Texture texture = VideoTextureLink::make(&scene, a);
    ASSERT_EQ(texture.getId(), VideoTextureLink::makeId(a));
    CameraTextureLink::retarget(&scene, texture, b);
    EXPECT_EQ(texture.getId(), VideoTextureLink::makeId(b));
    EXPECT_EQ(texture.getFramebuffer(), scene.getComponent<VideoPlayerComponent>(b).output->getFramebuffer());
    // the player deleted: links to it resolve to nothing, and keep naming it (undo brings it back)
    const Entity image = scene.createUserEntity();
    scene.addComponent<Transform>(image, {});
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    mesh.submeshes[0].material.baseColorTexture = texture;
    scene.addComponent<MeshComponent>(image, mesh);
    scene.destroyEntity(b);
    EXPECT_TRUE(CameraTextureLink::resolve(&scene));
    const Texture& after = scene.getComponent<MeshComponent>(image).submeshes[0].material.baseColorTexture;
    EXPECT_EQ(after.getFramebuffer(), nullptr);
    EXPECT_EQ(after.getId(), VideoTextureLink::makeId(b));
}

TEST(VideoPlayerComponent, ExportedLinkCode){
    Scene scene;
    const Entity player = scene.createUserEntity();
    scene.addComponent<Transform>(player, {});
    scene.addComponent<VideoPlayerComponent>(player, {});
    const Entity image = scene.createUserEntity();
    scene.addComponent<Transform>(image, {});
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    mesh.submeshes[0].material.baseColorTexture.setId(VideoTextureLink::makeId(player));
    scene.addComponent<MeshComponent>(image, mesh);
    const std::string code = Factory::createCameraTextureLinks(4, &scene, {player, image}, nullptr, false);
    EXPECT_NE(code.find(".output = std::make_shared<VideoTextureConverter>();"), std::string::npos) << code;
    EXPECT_NE(code.find(".setFramebuffer(scene->getComponent<VideoPlayerComponent>("), std::string::npos) << code;
    EXPECT_NE(code.find(".output->getFramebuffer());"), std::string::npos) << code;
}

TEST_F(VideoPlayerSystemTest, AMovedPlayerKeepsPlaying){
    // to another scene (DontDestroyOnLoad): its texture, its listeners and its playback go along
    const Entity entity = makePlayer();
    run(4, 0.05);
    VideoPlayer player(&scene, entity);
    ASSERT_TRUE(player.isPlaying());
    Framebuffer* texture = scene.getComponent<VideoPlayerComponent>(entity).output->getFramebuffer();
    const Entity image = scene.createUserEntity();
    scene.addComponent<Transform>(image, {});
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    mesh.submeshes[0].material.baseColorTexture = VideoTextureLink::make(&scene, entity);
    scene.addComponent<MeshComponent>(image, mesh);

    Scene persistent;
    std::unordered_map<Entity, Entity> map;
    const std::vector<Entity> moved = EntityCloner::move(&scene, {entity, image}, &persistent, map);
    ASSERT_EQ(moved.size(), 2u);
    for (Entity e : {entity, image})
        scene.destroyEntity(e);
    const Entity there = map[entity];
    VideoPlayerComponent& movedPlayer = persistent.getComponent<VideoPlayerComponent>(there);
    ASSERT_TRUE(movedPlayer.output);
    EXPECT_EQ(movedPlayer.output->getFramebuffer(), texture) << "the same render texture";
    const Texture& shown = persistent.getComponent<MeshComponent>(map[image]).submeshes[0].material.baseColorTexture;
    EXPECT_EQ(shown.getFramebuffer(), texture);
    EXPECT_EQ(shown.getId(), VideoTextureLink::makeId(there));
    VideoPlayer movedHandle(&persistent, there);
    EXPECT_TRUE(movedHandle.isPrepared()) << "not opened again";
    EXPECT_TRUE(movedHandle.isPlaying());
    EXPECT_EQ(opened.size(), 1u);
    const double before = movedHandle.getTime();
    events.clear();
    for (int i = 0; i < 4; i++){
        HtmlUIClock::advance(0.05);
        persistent.getSystem<VideoPlayerSystem>()->update(0.05);
    }
    EXPECT_GT(movedHandle.getTime(), before) << "it plays on there";
    for (int i = 0; i < 30; i++){
        HtmlUIClock::advance(0.05);
        persistent.getSystem<VideoPlayerSystem>()->update(0.05);
    }
    EXPECT_EQ(std::count(events.begin(), events.end(), "loop"), 1) << "its listeners came along";
}

TEST_F(VideoPlayerSystemTest, PrepareAfterAnErrorTriesAgain){
    failing = true;
    const Entity entity = makePlayer();
    run(4, 0.05);
    VideoPlayer player(&scene, entity);
    ASSERT_FALSE(player.getError().empty());
    failing = false;
    player.prepare();
    run(4, 0.05);
    EXPECT_TRUE(player.isPrepared()) << "a failed source is dropped by prepare()";
    EXPECT_EQ(opened.size(), 2u);
}

TEST_F(VideoPlayerSystemTest, SkipOnDropOffNeverBlocks){
    const double previousFixed = Engine::getFixedDeltatime();
    Engine::setFixedDeltatime(0.0);
    const Entity entity = makePlayer();
    scene.getComponent<VideoPlayerComponent>(entity).skipOnDrop = false;
    VideoPlayer player(&scene, entity);
    run(4, 0.05);
    run(10, 0.05);
    Engine::setFixedDeltatime(previousFixed);
    // IVideoSource's wait flag asks the decoder to block; real-time playback never sets it.
    EXPECT_FALSE(waitRequested);
    EXPECT_TRUE(player.isPlaying());
    EXPECT_GT(player.getFrame(), 0) << "the player actually consumed frames";
}
