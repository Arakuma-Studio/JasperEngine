// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Vertex Animator (vertex animation spec 3.3, 3.4, 3.6-3.9): registration and Catalog
// properties, Stream (authored fields only), the C++ the exporter emits, cloning, prefab
// overrides; and VertexAnimatorSystem on in-memory assets (VertexAnimation::create +
// VertexAnimationPool::put): every request kind for the entity and its instances, crossfade and
// blend, queues, finishes, events (forward, backward, wrap, ping-pong, low frame rates, the
// dominant track, instanceEvents), the epoch rebase, seeded random starts, instance add/remove,
// GPU writes only on change, mesh_mismatch, attachments, animated bounds, reload on a rebake,
// the edit-mode clock, and the Lua API through its bindings.

#include <gtest/gtest.h>
#include "component/ScriptComponent.h"

#include "engine/physics/PhysicsTestScene.h"

#include "Catalog.h"
#include "Factory.h"
#include "Scene.h"
#include "Stream.h"
#include "animation/AnimationEvent.h"
#include "animation/VertexAnimation.h"
#include "animation/VertexAnimatorRuntime.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "component/VertexAnimatorComponent.h"
#include "manager/SceneManager.h"
#include "object/Mesh.h"
#include "object/VertexAnimator.h"
#include "pool/VertexAnimationPool.h"
#include "project/ProjectPrefabInternal.h"
#include "registry/EntityCloner.h"
#include "script/LuaBinding.h"
#include "subsystem/VertexAnimatorSystem.h"
#include "util/EntityClipboard.h"
#include "util/ProjectUtils.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::vat;

namespace {

    const char* const HERO = "vat/hero.vat";
    const char* const HERO_MESH = "vat/hero.vat.glb";
    const char* const HERO_GUID = "0123456789abcdef0123456789abcdef";

    enum { WALK = 0, WAVE = 1, BOUNCE = 2, RUN = 3 };

    VatClip makeClip(const std::string& name, int start, int frames, float fps, ClipWrapMode wrap,
                     const Vector3& min, const Vector3& max, float eventTime, const std::string& function) {
        VatClip clip;
        clip.name = name;
        clip.start = start;
        clip.frames = frames;
        clip.fps = fps;
        clip.wrap = wrap;
        clip.bounds.extend(min);
        clip.bounds.extend(max);
        ClipEventData event;
        event.time = eventTime;
        event.function = function;
        event.args = {AnimationEvent::stringArg(name)};
        clip.events.push_back(event);
        return clip;
    }

    // four 1-second clips: Walk (loop), Wave (once), Bounce (ping-pong), Run (loop, 8 fps), one
    // socket whose position x is the global frame
    VatData heroData(bool reordered = false) {
        VatData data;
        data.guid = HERO_GUID;
        data.mode = VatMode::Vertices;
        data.mesh = "hero.vat.glb";
        data.layout.precision = VatPrecision::Float;
        data.layout.width = 8;
        data.layout.height = 3;
        data.layout.samples = 22;
        data.layout.vertices = 1;
        data.layout.normals = false;
        std::vector<VatClip> clips = {
            makeClip("Walk", 0, 4, 4.0f, ClipWrapMode::Loop, Vector3(-1, 0, -1), Vector3(1, 2, 1), 0.25f, "onStep"),
            makeClip("Wave", 4, 5, 4.0f, ClipWrapMode::Once, Vector3(-2, 0, -1), Vector3(2, 2, 1), 0.5f, "onWave"),
            makeClip("Bounce", 9, 5, 4.0f, ClipWrapMode::PingPong, Vector3(-1, 0, -1), Vector3(1, 3, 1), 0.25f, "onBounce"),
            makeClip("Run", 14, 8, 8.0f, ClipWrapMode::Loop, Vector3(-1, 0, -3), Vector3(1, 2, 3), 0.5f, "onRun")};
        if (reordered) std::reverse(clips.begin(), clips.end());
        data.clips = clips;
        for (const VatClip& clip : data.clips) data.bounds.extend(clip.bounds);
        VatSocket socket;
        socket.name = "RightHand";
        socket.path = "Armature/Hand";
        data.sockets.push_back(socket);
        data.layout.layoutHash = computeLayoutHash(data);
        return data;
    }

    std::shared_ptr<const VertexAnimation> heroAssetOf(const VatData& data) {
        VatPayload payload;
        payload.precision = VatPrecision::Float;
        payload.width = data.layout.width;
        payload.height = data.layout.height;
        payload.bytes.assign((size_t)payload.width * payload.height * 16, 0);
        VatSocketTracks sockets;
        sockets.sockets = 1;
        sockets.frames = data.layout.samples;
        for (int f = 0; f < sockets.frames; f++) {
            const float row[7] = {(float)f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
            sockets.data.insert(sockets.data.end(), row, row + 7);
        }
        std::string error;
        std::shared_ptr<const VertexAnimation> asset = VertexAnimation::create(data, payload, sockets, &error);
        EXPECT_TRUE(asset) << error;
        return asset;
    }

    std::shared_ptr<const VertexAnimation> heroAsset(bool reordered = false) {
        return heroAssetOf(heroData(reordered));
    }

    // every authored field away from its default
    VertexAnimatorComponent authoredAnimator() {
        VertexAnimatorComponent animator;
        animator.enabled = false;
        animator.animation = HERO;
        animator.guid = HERO_GUID;
        animator.defaultClip = "Walk";
        animator.playAutomatically = false;
        animator.speed = 1.5f;
        animator.randomStart = false;
        animator.defaultFadeTime = 0.4f;
        animator.instanceEvents = true;
        animator.attachments = {{"RightHand", 7}};
        return animator;
    }

    void expectSameAuthored(const VertexAnimatorComponent& a, const VertexAnimatorComponent& b) {
        EXPECT_EQ(a.enabled, b.enabled);
        EXPECT_EQ(a.animation, b.animation);
        EXPECT_EQ(a.guid, b.guid);
        EXPECT_EQ(a.defaultClip, b.defaultClip);
        EXPECT_EQ(a.playAutomatically, b.playAutomatically);
        EXPECT_FLOAT_EQ(a.speed, b.speed);
        EXPECT_EQ(a.randomStart, b.randomStart);
        EXPECT_FLOAT_EQ(a.defaultFadeTime, b.defaultFadeTime);
        EXPECT_EQ(a.instanceEvents, b.instanceEvents);
        EXPECT_EQ(a.attachments, b.attachments);
    }

    struct Fired {
        std::vector<std::string> events;     // "function@instance"
        std::vector<std::string> finishes;
    };

    class VertexAnimatorSystemTest : public ::testing::Test {
    protected:
        const bool installed = physicstest::installTestSystem();
        Scene scene;
        Entity hero = NULL_ENTITY;
        Fired fired;

        void SetUp() override {
            VertexAnimationPool::put(HERO, heroAsset());
        }

        std::shared_ptr<VertexAnimatorSystem> system() { return scene.getSystem<VertexAnimatorSystem>(); }

        Entity makeHero(int instances = -1, bool randomStart = false, const std::string& model = HERO_MESH) {
            Entity entity = scene.createUserEntity();
            scene.addComponent<Transform>(entity, {});
            scene.addComponent<MeshComponent>(entity, {});
            ModelComponent modelComponent;
            modelComponent.filename = model;
            modelComponent.skeleton = NULL_ENTITY;
            modelComponent.needUpdateModel = false;
            scene.addComponent<ModelComponent>(entity, modelComponent);
            if (instances >= 0) {
                InstancedMeshComponent instanced;
                instanced.instances.resize((size_t)instances);
                scene.addComponent<InstancedMeshComponent>(entity, instanced);
            }
            VertexAnimatorComponent animator;
            animator.animation = HERO;
            animator.randomStart = randomStart;
            scene.addComponent<VertexAnimatorComponent>(entity, animator);
            return entity;
        }

        VertexAnimatorComponent& animator(Entity entity) { return scene.getComponent<VertexAnimatorComponent>(entity); }

        void listen(Entity entity) {
            VertexAnimatorComponent& a = animator(entity);
            a.onEvent.add("test", std::function<void(const AnimationEvent&)>([this](const AnimationEvent& e) {
                fired.events.push_back(e.functionName + "@-1");
            }));
            a.onFinish.add("test", std::function<void(const std::string&)>([this](const std::string& clip) {
                fired.finishes.push_back(clip + "@-1");
            }));
            a.onInstanceEvent.add("test", std::function<void(int, const AnimationEvent&)>([this](int i, const AnimationEvent& e) {
                fired.events.push_back(e.functionName + "@" + std::to_string(i));
            }));
            a.onInstanceFinish.add("test", std::function<void(int, const std::string&)>([this](int i, const std::string& clip) {
                fired.finishes.push_back(clip + "@" + std::to_string(i));
            }));
        }

        void step(double dt) { system()->update(dt); }

        // a track's local time now (vatLocalTime)
        double tau(const VatTrack& track, Entity entity) {
            if (track.speed == 0.0f) return track.start;
            return ((double)animator(entity).lastNow - track.start) * track.speed;
        }

        static bool restPacked(const InstanceData& instance) {
            return instance.vat0 == InstanceData().vat0 && instance.vat1 == InstanceData().vat1;
        }

        static bool packedAs(const InstanceData& instance, const VatTrackPair& pair) {
            Vector4 v0, v1;
            vatPackInstance(pair, v0, v1);
            return instance.vat0 == v0 && instance.vat1 == v1;
        }
    };

}

// ---- registration, Catalog ------------------------------------------------------------------

TEST(VertexAnimatorComponent, RegistrationAndNames) {
    const ComponentType type = ComponentType::VertexAnimatorComponent;
    // appended after every older type (prefab override masks use the ordinal); the Trail
    // Renderer, the Canvas Group, then the Text Effect came after it
    EXPECT_EQ(static_cast<int>(type) + 1, static_cast<int>(ComponentType::TrailRendererComponent));
    EXPECT_EQ(Catalog::getComponentName(type, true), "vertexanimator");
    EXPECT_EQ(Catalog::getComponentName(type), "VertexAnimatorComponent");
    EXPECT_EQ(Catalog::getComponentDisplayName(type), "Vertex Animator");
    EXPECT_STRNE(Catalog::getComponentIcon(type), Catalog::getComponentIcon(ComponentType::ScriptComponent));
    EXPECT_EQ(Catalog::getComponentType("vertexanimator"), type);
    EXPECT_EQ(Catalog::getComponentType("VertexAnimatorComponent"), type);
    EXPECT_TRUE(Catalog::hasEnabledProperty(type));

    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<VertexAnimatorComponent>(entity, authoredAnimator());
    const std::vector<ComponentType> found = Catalog::findComponents(&scene, entity);
    EXPECT_NE(std::find(found.begin(), found.end(), type), found.end());
    EXPECT_TRUE(Catalog::componentTypeToSignature(&scene, type).test(scene.getComponentId<VertexAnimatorComponent>()));
}

TEST(VertexAnimatorComponent, CatalogProperties) {
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<VertexAnimatorComponent>(entity, authoredAnimator());
    const ComponentType type = ComponentType::VertexAnimatorComponent;
    VertexAnimatorComponent& animator = scene.getComponent<VertexAnimatorComponent>(entity);

    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "enabled").ref, &animator.enabled);
    const PropertyData animation = Catalog::findProperty(&scene, entity, type, "animation");
    EXPECT_EQ(animation.type, PropertyType::String);
    EXPECT_EQ(animation.ref, &animator.animation);
    EXPECT_TRUE(animation.updateFlags & UpdateFlags_VertexAnimator_Reload);
    for (const char* name : {"guid", "defaultClip", "playAutomatically", "speed", "randomStart", "defaultFadeTime", "instanceEvents"}) {
        EXPECT_TRUE(Catalog::findProperty(&scene, entity, type, name).ref) << name;
    }
    const PropertyData attachments = Catalog::findProperty(&scene, entity, type, "attachments");
    EXPECT_EQ(attachments.type, PropertyType::Custom);
    EXPECT_EQ(attachments.ref, &animator.attachments);
    const PropertyData socket = Catalog::findProperty(&scene, entity, type, "attachments[0].socket");
    EXPECT_EQ(socket.type, PropertyType::String);
    EXPECT_EQ(socket.ref, &animator.attachments[0].socket);
    const PropertyData target = Catalog::findProperty(&scene, entity, type, "attachments[0].entity");
    EXPECT_EQ(target.type, PropertyType::Entity);
    EXPECT_EQ(target.ref, &animator.attachments[0].entity);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "attachments[1].socket").ref);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "attachments[0].other").ref);
    // the runtime is not a property
    for (const char* name : {"preview", "state", "gpuActive", "loadError", "requests"}) {
        EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, name).ref) << name;
    }

    const std::map<std::string, PropertyData> all = Catalog::findEntityProperties(&scene, entity, type);
    for (const char* name : {"animation", "attachments", "attachments[0].socket", "attachments[0].entity", "enabled"}) {
        EXPECT_TRUE(all.count(name)) << name;
    }
    const std::map<std::string, PropertyData> templates = Catalog::getProperties(type, nullptr);
    EXPECT_TRUE(templates.count("attachments[].socket"));
    EXPECT_TRUE(templates.count("attachments[].entity"));

    // a new animation loads; a negative fade is none
    animator.needsLoad = false;
    animator.defaultFadeTime = -2.0f;
    Catalog::updateEntity(&scene, entity, UpdateFlags_VertexAnimator_Reload);
    EXPECT_TRUE(animator.needsLoad);
    EXPECT_FLOAT_EQ(animator.defaultFadeTime, 0.0f);

    // the attachment list copies as a value (prefab revert)
    const Entity other = scene.createUserEntity();
    scene.addComponent<Transform>(other, {});
    scene.addComponent<VertexAnimatorComponent>(other, {});
    Catalog::copyPropertyValue(&scene, entity, &scene, other, type, "attachments");
    EXPECT_EQ(scene.getComponent<VertexAnimatorComponent>(other).attachments, authoredAnimator().attachments);
}

// ---- Stream, clipboard, ProjectUtils ---------------------------------------------------------

TEST(VertexAnimatorComponent, StreamRoundTripKeepsOnlyTheAuthoredFields) {
    VertexAnimatorComponent animator = authoredAnimator();
    // runtime values that must not be written
    animator.preview = true;
    animator.started = true;
    animator.lastNow = 12.0f;
    animator.state.a.clip = 2;
    animator.loadError = "x";
    const YAML::Node node = Stream::encodeVertexAnimatorComponent(animator);
    for (const char* key : {"enabled", "animation", "guid", "defaultClip", "playAutomatically", "speed", "randomStart",
                            "defaultFadeTime", "instanceEvents", "attachments"}) {
        EXPECT_TRUE(node[key]) << key;
    }
    for (const char* key : {"preview", "started", "lastNow", "state", "loadError", "gpuActive", "requests"}) {
        EXPECT_FALSE(node[key]) << key;
    }
    ASSERT_EQ(node["attachments"].size(), 1u);
    EXPECT_EQ(node["attachments"][0]["socket"].as<std::string>(), "RightHand");
    EXPECT_EQ(node["attachments"][0]["entity"].as<Entity>(), 7u);

    const VertexAnimatorComponent decoded = Stream::decodeVertexAnimatorComponent(node);
    expectSameAuthored(decoded, authoredAnimator());
    EXPECT_TRUE(decoded.needsLoad);
    EXPECT_FALSE(decoded.started);
    EXPECT_FALSE(decoded.preview);
    EXPECT_EQ(decoded.state.a.clip, -1);
    EXPECT_EQ(YAML::Dump(Stream::encodeVertexAnimatorComponent(decoded)), YAML::Dump(node)) << "a second trip writes the same text";

    // no GUID: not written; defaults and wrong shapes read as the defaults
    VertexAnimatorComponent plain;
    EXPECT_FALSE(Stream::encodeVertexAnimatorComponent(plain)["guid"]);
    const VertexAnimatorComponent shapes = Stream::decodeVertexAnimatorComponent(YAML::Load(
        "enabled: maybe\nspeed: [1]\ndefaultFadeTime: -3\nanimation: {a: b}\nattachments:\n  - [x]\n  - {socket: Head, entity: nope}\n"));
    const VertexAnimatorComponent defaults;
    EXPECT_EQ(shapes.enabled, defaults.enabled);
    EXPECT_FLOAT_EQ(shapes.speed, defaults.speed);
    EXPECT_FLOAT_EQ(shapes.defaultFadeTime, 0.0f);
    EXPECT_EQ(shapes.animation, "");
    ASSERT_EQ(shapes.attachments.size(), 1u);
    EXPECT_EQ(shapes.attachments[0].socket, "Head");
    EXPECT_EQ(shapes.attachments[0].entity, NULL_ENTITY);
}

// Create Crowd's per-instance starts: written only when set, read back, copied by value, emitted
TEST(VertexAnimatorComponent, InstanceClipsAndSpeedsRoundTrip) {
    VertexAnimatorComponent plain;
    const YAML::Node plainNode = Stream::encodeVertexAnimatorComponent(plain);
    EXPECT_FALSE(plainNode["instanceClips"]) << "other animators' YAML is unchanged";
    EXPECT_FALSE(plainNode["instanceSpeeds"]);

    VertexAnimatorComponent crowd = authoredAnimator();
    crowd.instanceClips = {"Walk", "", "Run"};
    crowd.instanceSpeeds = {1.25f, 0.5f};
    const YAML::Node node = Stream::encodeVertexAnimatorComponent(crowd);
    ASSERT_TRUE(node["instanceClips"]);
    ASSERT_TRUE(node["instanceSpeeds"]);
    const VertexAnimatorComponent decoded = Stream::decodeVertexAnimatorComponent(node);
    EXPECT_EQ(decoded.instanceClips, crowd.instanceClips);
    EXPECT_EQ(decoded.instanceSpeeds, crowd.instanceSpeeds);
    EXPECT_EQ(YAML::Dump(Stream::encodeVertexAnimatorComponent(decoded)), YAML::Dump(node));
    // decoding over an animator without them clears them (a prefab revert)
    EXPECT_TRUE(Stream::decodeVertexAnimatorComponent(plainNode, &crowd).instanceClips.empty());

    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<VertexAnimatorComponent>(entity, crowd);
    const ComponentType type = ComponentType::VertexAnimatorComponent;
    VertexAnimatorComponent& animator = scene.getComponent<VertexAnimatorComponent>(entity);
    const PropertyData clips = Catalog::findProperty(&scene, entity, type, "instanceClips");
    EXPECT_EQ(clips.type, PropertyType::Custom);
    EXPECT_EQ(clips.ref, &animator.instanceClips);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "instanceSpeeds").ref, &animator.instanceSpeeds);
    const Entity other = scene.createUserEntity();
    scene.addComponent<Transform>(other, {});
    scene.addComponent<VertexAnimatorComponent>(other, {});
    Catalog::copyPropertyValue(&scene, entity, &scene, other, type, "instanceClips");
    Catalog::copyPropertyValue(&scene, entity, &scene, other, type, "instanceSpeeds");
    EXPECT_EQ(scene.getComponent<VertexAnimatorComponent>(other).instanceClips, crowd.instanceClips);
    EXPECT_EQ(scene.getComponent<VertexAnimatorComponent>(other).instanceSpeeds, crowd.instanceSpeeds);

    const std::string code = Factory::createComponent(0, &scene, entity, type, "", "scene", "entity");
    EXPECT_NE(code.find("vertexanimator.instanceClips = {\"Walk\", \"\", \"Run\"};"), std::string::npos) << code;
    EXPECT_NE(code.find("vertexanimator.instanceSpeeds = {1.250000f, 0.500000f};"), std::string::npos) << code;
}

TEST(VertexAnimatorComponent, DecodingOverAnAnimatorResetsItsRunAndClearsCallbacks) {
    VertexAnimatorComponent old = authoredAnimator();
    old.preview = true;
    old.started = true;
    old.needsLoad = false;
    old.state.a.clip = 1;
    old.instanceStates.resize(3);
    old.runtime = std::make_shared<VertexAnimatorRuntime>();
    int calls = 0;
    old.onFinish.add("script", std::function<void(const std::string&)>([&calls](const std::string&) { calls++; }));
    old.onInstanceEvent.add("script", std::function<void(int, const AnimationEvent&)>([&calls](int, const AnimationEvent&) { calls++; }));

    const VertexAnimatorComponent decoded = Stream::decodeVertexAnimatorComponent(Stream::encodeVertexAnimatorComponent(old), &old);
    expectSameAuthored(decoded, old);
    EXPECT_TRUE(decoded.preview) << "the Inspector's Preview toggle stays";
    EXPECT_FALSE(decoded.started);
    EXPECT_TRUE(decoded.needsLoad);
    EXPECT_EQ(decoded.state.a.clip, -1);
    EXPECT_TRUE(decoded.instanceStates.empty());
    EXPECT_FALSE(decoded.runtime);
    EXPECT_FALSE(decoded.onFinish.hasSubscribers());
    EXPECT_FALSE(decoded.onInstanceEvent.hasSubscribers());
    EXPECT_EQ(calls, 0);
}

TEST(VertexAnimatorComponent, SceneComponentsClipboardAndProjectUtils) {
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<VertexAnimatorComponent>(entity, authoredAnimator());
    const YAML::Node components = Stream::encodeComponents(entity, &scene, scene.getSignature(entity));
    ASSERT_TRUE(components["vertexanimator"]) << "scene YAML key";

    Scene other;
    const Entity copy = other.createUserEntity();
    Stream::decodeComponents(copy, NULL_ENTITY, &other, components);
    ASSERT_TRUE(other.findComponent<VertexAnimatorComponent>(copy));
    expectSameAuthored(other.getComponent<VertexAnimatorComponent>(copy), authoredAnimator());

    // Paste Component Values; Add Component gives the defaults
    const Entity target = scene.createUserEntity();
    scene.addComponent<Transform>(target, {});
    scene.addComponent<VertexAnimatorComponent>(target, {});
    EntityClipboard::applyComponentValue(&scene, target, ComponentType::VertexAnimatorComponent,
                                         EntityClipboard::encodeComponent(&scene, entity, ComponentType::VertexAnimatorComponent));
    expectSameAuthored(scene.getComponent<VertexAnimatorComponent>(target), authoredAnimator());
    expectSameAuthored(Stream::decodeVertexAnimatorComponent(EntityClipboard::encodeDefaultComponent(ComponentType::VertexAnimatorComponent)),
                       VertexAnimatorComponent());

    std::vector<Entity> entities{target};
    const YAML::Node removed = ProjectUtils::removeEntityComponent(&scene, target, ComponentType::VertexAnimatorComponent, entities, true);
    EXPECT_FALSE(scene.findComponent<VertexAnimatorComponent>(target));
    ProjectUtils::addEntityComponent(&scene, target, ComponentType::VertexAnimatorComponent, entities, removed);
    ASSERT_TRUE(scene.findComponent<VertexAnimatorComponent>(target));
    expectSameAuthored(scene.getComponent<VertexAnimatorComponent>(target), authoredAnimator());
}

TEST(VertexAnimatorComponent, PrefabOverridesOfScalarsAndAttachments) {
    const ComponentType type = ComponentType::VertexAnimatorComponent;
    auto make = [](Scene& scene, const VertexAnimatorComponent& animator) {
        const Entity entity = scene.createUserEntity();
        scene.addComponent<Transform>(entity, {});
        scene.addComponent<VertexAnimatorComponent>(entity, animator);
        return entity;
    };
    Scene asset;
    VertexAnimatorComponent base;
    base.animation = HERO;
    base.attachments = {{"RightHand", NULL_ENTITY}};
    const Entity assetRoot = make(asset, base);

    Scene instance;
    VertexAnimatorComponent faster = base;
    faster.speed = 2.0f;
    EXPECT_EQ(prefabinternal::diffComponentProperties(&asset, assetRoot, &instance, make(instance, faster), type),
              std::vector<std::string>{"speed"});

    VertexAnimatorComponent other = base;
    other.attachments[0].socket = "Head";
    EXPECT_EQ(prefabinternal::diffComponentProperties(&asset, assetRoot, &instance, make(instance, other), type),
              (std::vector<std::string>{"attachments", "attachments[0].socket"}));

    // applied as the instance loads
    VertexAnimatorComponent both = other;
    both.speed = 3.0f;
    const Entity bothRoot = make(instance, both);
    prefabinternal::ScratchEntity values;
    values.takeComponents(&instance, bothRoot, Catalog::componentTypeToSignature(&instance, type));
    Scene loaded;
    const Entity loadedRoot = make(loaded, base);
    std::set<std::string> names{"attachments", "speed"};
    prefabinternal::copyProperties(values, &loaded, loadedRoot, type, names, "test");
    EXPECT_EQ(loaded.getComponent<VertexAnimatorComponent>(loadedRoot).attachments, other.attachments);
    EXPECT_FLOAT_EQ(loaded.getComponent<VertexAnimatorComponent>(loadedRoot).speed, 3.0f);
}

// ---- C++ export ------------------------------------------------------------------------------

namespace {

    // Factory's output for authoredAnimator(), pasted: it must keep compiling against the engine
    // headers and build the same component (one text with EMITTED_VERTEX_ANIMATOR)
    void emittedVertexAnimator(Scene* scene, Entity entity) {
        VertexAnimatorComponent vertexanimator;
        vertexanimator.enabled = false;
        vertexanimator.animation = "vat/hero.vat";
        vertexanimator.defaultClip = "Walk";
        vertexanimator.playAutomatically = false;
        vertexanimator.speed = 1.500000f;
        vertexanimator.randomStart = false;
        vertexanimator.defaultFadeTime = 0.400000f;
        vertexanimator.instanceEvents = true;
        vertexanimator.attachments.resize(1);
        vertexanimator.attachments[0].socket = "RightHand";
        vertexanimator.attachments[0].entity = 7;
        scene->addComponent<VertexAnimatorComponent>(entity, vertexanimator);
    }

    const char* const EMITTED_VERTEX_ANIMATOR =
        "VertexAnimatorComponent vertexanimator;\n"
        "vertexanimator.enabled = false;\n"
        "vertexanimator.animation = \"vat/hero.vat\";\n"
        "vertexanimator.defaultClip = \"Walk\";\n"
        "vertexanimator.playAutomatically = false;\n"
        "vertexanimator.speed = 1.500000f;\n"
        "vertexanimator.randomStart = false;\n"
        "vertexanimator.defaultFadeTime = 0.400000f;\n"
        "vertexanimator.instanceEvents = true;\n"
        "vertexanimator.attachments.resize(1);\n"
        "vertexanimator.attachments[0].socket = \"RightHand\";\n"
        "vertexanimator.attachments[0].entity = 7;\n"
        "scene->addComponent<VertexAnimatorComponent>(entity, vertexanimator);\n";

}

TEST(VertexAnimatorComponent, FactoryEmitsTheAuthoredFields) {
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<VertexAnimatorComponent>(entity, authoredAnimator());
    const std::string code = Factory::createComponent(0, &scene, entity, ComponentType::VertexAnimatorComponent, "", "scene", "entity");
    EXPECT_EQ(code, EMITTED_VERTEX_ANIMATOR);
    EXPECT_EQ(code.find(HERO_GUID), std::string::npos) << "the editor's GUID stays out of the build";

    Scene built;
    const Entity builtEntity = built.createUserEntity();
    emittedVertexAnimator(&built, builtEntity);
    VertexAnimatorComponent expected = authoredAnimator();
    expected.guid.clear();
    expectSameAuthored(built.getComponent<VertexAnimatorComponent>(builtEntity), expected);

    // the scene's preload list names the .vat (Factory writes resources.vats)
    SceneResources resources;
    resources.vats.push_back(HERO);
    EXPECT_EQ(resources.vats.size(), 1u);
}

// ---- cloning ---------------------------------------------------------------------------------

TEST_F(VertexAnimatorSystemTest, CloningCopiesAuthoredAndResetsTheRun) {
    hero = makeHero();
    const Entity sword = scene.createUserEntity();
    Transform swordTransform;
    swordTransform.parent = hero;
    scene.addComponent<Transform>(sword, swordTransform);
    animator(hero).attachments = {{"RightHand", sword}};
    animator(hero).preview = true;
    listen(hero);
    step(0.0);
    step(0.4);
    ASSERT_TRUE(animator(hero).asset);

    std::vector<Entity> copies = EntityCloner::clone(&scene, EntityCloner::collectHierarchy(&scene, hero), &scene);
    ASSERT_EQ(copies.size(), 2u);
    VertexAnimatorComponent& copy = animator(copies[0]);
    EXPECT_EQ(copy.animation, HERO);
    ASSERT_EQ(copy.attachments.size(), 1u);
    EXPECT_EQ(copy.attachments[0].entity, copies[1]) << "the copied attachment follows the copy";
    EXPECT_FALSE(copy.asset);
    EXPECT_FALSE(copy.runtime);
    EXPECT_FALSE(copy.started);
    EXPECT_FALSE(copy.preview);
    EXPECT_TRUE(copy.needsLoad);
    EXPECT_EQ(copy.state.a.clip, -1);
    EXPECT_FLOAT_EQ(copy.lastNow, 0.0f);
    EXPECT_FALSE(copy.gpuActive);
    EXPECT_FALSE(copy.onEvent.hasSubscribers());
    EXPECT_FALSE(copy.onInstanceFinish.hasSubscribers());
    // and it plays on its own from its start
    step(0.1);
    EXPECT_EQ(animator(copies[0]).state.a.clip, WALK);
    EXPECT_FLOAT_EQ(animator(copies[0]).lastNow, 0.1f);
}

// ---- playback --------------------------------------------------------------------------------

TEST_F(VertexAnimatorSystemTest, PlaysTheDefaultClipAndWritesTheGpuState) {
    hero = makeHero();
    step(0.0);
    VertexAnimatorComponent& a = animator(hero);
    EXPECT_TRUE(a.gpuActive);
    EXPECT_EQ(a.loadError, "");
    EXPECT_EQ(a.gpuMode, VatMode::Vertices);
    EXPECT_EQ(a.state.a.clip, WALK);
    EXPECT_FLOAT_EQ(a.state.a.start, 0.0f);
    Vector4 v0, v1;
    vatPackInstance(a.state, v0, v1);
    EXPECT_EQ(a.gpuState0, v0);
    EXPECT_EQ(a.gpuState1, v1);
    step(0.5);
    EXPECT_FLOAT_EQ(a.gpuNow, 0.5f);
    VertexAnimator object(&scene, hero);
    EXPECT_TRUE(object.isLoaded());
    EXPECT_TRUE(object.isPlaying());
    EXPECT_TRUE(object.isPlaying("Walk"));
    EXPECT_EQ(object.getCurrentClip(), "Walk");
    EXPECT_NEAR(object.getNormalizedTime(), 0.5f, 1e-5f);
    EXPECT_NEAR(object.getTime(), 0.5f, 1e-5f);
    EXPECT_EQ(object.getClipNames(), (std::vector<std::string>{"Walk", "Wave", "Bounce", "Run"}));
    EXPECT_FLOAT_EQ(object.getClipLength("Wave"), 1.0f);
    EXPECT_FLOAT_EQ(object.getClipLength("Nope"), 0.0f);

    // defaultClip, and no playAutomatically: rest
    Entity second = makeHero();
    animator(second).defaultClip = "Run";
    Entity third = makeHero();
    animator(third).playAutomatically = false;
    step(0.0);
    EXPECT_EQ(animator(second).state.a.clip, RUN);
    EXPECT_EQ(animator(third).state.a.clip, -1);
    EXPECT_FALSE(VertexAnimator(&scene, third).isPlaying());
}

TEST_F(VertexAnimatorSystemTest, PlayStopSetTimeAndSpeedOnTheEntity) {
    hero = makeHero();
    step(0.0);
    step(0.3);
    VertexAnimator object(&scene, hero);

    object.play("Wave");    // applied now: the asset is loaded
    EXPECT_EQ(animator(hero).state.a.clip, WAVE);
    EXPECT_FLOAT_EQ(animator(hero).state.a.start, 0.3f);
    step(0.2);
    EXPECT_NEAR(object.getTime(), 0.2f, 1e-5f);

    object.setTime(0.75f);
    EXPECT_NEAR(object.getTime(), 0.75f, 1e-5f);

    // stop: holds the pose, the clock goes on
    object.stop();
    EXPECT_FALSE(object.isPlaying());
    step(0.5);
    EXPECT_NEAR(object.getTime(), 0.75f, 1e-4f);
    step(0.25);
    EXPECT_NEAR(object.getTime(), 0.75f, 1e-4f);
    object.play("Walk");
    EXPECT_TRUE(object.isPlaying("Walk"));

    // speed keeps the phase
    step(0.25);
    const float before = object.getTime();
    object.setSpeed(2.0f);
    step(0.0);
    EXPECT_NEAR(object.getTime(), before, 1e-4f);
    EXPECT_FLOAT_EQ(animator(hero).state.a.speed, 2.0f);
    step(0.1);
    EXPECT_NEAR(object.getTime(), before + 0.2f, 1e-4f);
    // speed 0 holds it: packed as speed 0 with the held time as its start, written once
    object.setSpeed(0.0f);
    step(0.3);
    const Vector4 heldState = animator(hero).gpuState0;
    step(0.3);
    EXPECT_NEAR(object.getTime(), before + 0.2f, 1e-4f);
    EXPECT_FLOAT_EQ(animator(hero).state.a.speed, 0.0f);
    EXPECT_NEAR(animator(hero).state.a.start, before + 0.2f, 1e-4f);
    EXPECT_EQ(animator(hero).gpuState0, heldState) << "not re-pinned at every update";

    // a clip with its own speed (VatClip::speed multiplies the track's)
    object.setSpeed(1.0f);
    VatData data = heroData();
    data.clips[RUN].speed = 3.0f;
    VertexAnimationPool::put(HERO, VertexAnimation::create(data, VertexAnimationPool::get(HERO)->getPayload(),
                                                           VertexAnimationPool::get(HERO)->getSocketTracks(), nullptr));
    step(0.0);
    object.play("Run");
    EXPECT_FLOAT_EQ(animator(hero).state.a.speed, 3.0f);

    // unknown clip: nothing changes
    object.play("Nope");
    EXPECT_EQ(animator(hero).state.a.clip, RUN);
}

TEST_F(VertexAnimatorSystemTest, CrossfadeTimelineAndCompletion) {
    hero = makeHero();
    step(0.0);
    step(0.5);
    VertexAnimator object(&scene, hero);
    object.crossFade("Run", 0.8f);
    const VatTrackPair& pair = animator(hero).state;
    EXPECT_EQ(pair.a.clip, WALK);
    EXPECT_EQ(pair.b.clip, RUN);
    EXPECT_FLOAT_EQ(pair.fadeStart, 0.5f);
    EXPECT_FLOAT_EQ(pair.fadeEnd, 1.3f);
    EXPECT_FLOAT_EQ(vatWeightB(pair, 0.9f), 0.5f);
    EXPECT_EQ(object.getCurrentClip(), "Run") << "the clip faded to";
    EXPECT_TRUE(object.isPlaying("Walk"));
    step(0.4);
    EXPECT_EQ(animator(hero).state.b.clip, RUN) << "still fading";
    step(0.5);
    // B became A at the end, its time running on
    EXPECT_EQ(animator(hero).state.a.clip, RUN);
    EXPECT_EQ(animator(hero).state.b.clip, -1);
    EXPECT_NEAR(tau(animator(hero).state.a, hero), 0.9, 1e-4);
    EXPECT_FALSE(object.isPlaying("Walk"));

    // crossFade with no fade plays at once; crossFade(name) uses defaultFadeTime
    object.crossFade("Walk", 0.0f);
    EXPECT_EQ(animator(hero).state.a.clip, WALK);
    EXPECT_EQ(animator(hero).state.b.clip, -1);
    animator(hero).defaultFadeTime = 0.25f;
    object.crossFade("Bounce");
    EXPECT_NEAR(animator(hero).state.fadeEnd - animator(hero).state.fadeStart, 0.25f, 1e-5f);
    // play with a fade = crossFade
    step(0.05);
    object.play("Run", 0.5f);
    EXPECT_EQ(animator(hero).state.b.clip, RUN);
    EXPECT_EQ(animator(hero).state.a.clip, WALK) << "a crossfade during a crossfade continues from the dominant clip";
}

TEST_F(VertexAnimatorSystemTest, BlendHoldsAWeightWithPhaseSync) {
    hero = makeHero();
    step(0.0);
    step(0.3);
    VertexAnimator object(&scene, hero);
    object.blend("Walk", "Run", 0.3f);
    const VatTrackPair& pair = animator(hero).state;
    EXPECT_EQ(pair.a.clip, WALK);
    EXPECT_EQ(pair.b.clip, RUN);
    EXPECT_FLOAT_EQ(pair.blend, 0.3f);
    EXPECT_FLOAT_EQ(pair.a.start, 0.0f) << "A keeps its phase";
    EXPECT_FLOAT_EQ(pair.b.start, pair.a.start);
    EXPECT_FLOAT_EQ(pair.b.speed, pair.a.speed);
    EXPECT_FLOAT_EQ(vatWeightB(pair, 10.0f), 0.3f);
    Vector4 v0, v1;
    vatPackInstance(pair, v0, v1);
    EXPECT_FLOAT_EQ(v1.w, -1.3f) << "blend mode packs -(1 + weight)";
    step(0.2);
    object.blend("Walk", "Run", 0.7f);
    EXPECT_FLOAT_EQ(animator(hero).state.blend, 0.7f);
    EXPECT_FLOAT_EQ(animator(hero).state.a.start, 0.0f) << "a new weight only";
    EXPECT_EQ(object.getCurrentClip(), "Walk");

    // the dominant clip's events: B at A's phase (Run's 0.5 = Walk's 0.5 here)
    listen(hero);
    step(0.4);      // Walk 0.5 -> 0.9
    EXPECT_TRUE(fired.events.empty()) << "already past 0.5? " << fired.events.size();
    step(0.7);      // Walk 0.9 -> 1.6: Run's 0.5 crossed at 1.5
    EXPECT_EQ(fired.events, std::vector<std::string>{"onRun@-1"});
}

TEST_F(VertexAnimatorSystemTest, QueueAndFinish) {
    hero = makeHero();
    listen(hero);
    step(0.0);
    VertexAnimator object(&scene, hero);
    object.play("Wave");
    object.queue("Walk");
    EXPECT_EQ(animator(hero).queued, std::vector<std::string>{"Walk"});
    step(0.6);
    EXPECT_EQ(animator(hero).state.a.clip, WAVE);
    step(0.6);      // Wave ends at 1.0: Walk from then
    EXPECT_EQ(animator(hero).state.a.clip, WALK);
    EXPECT_FLOAT_EQ(animator(hero).state.a.start, 1.0f);
    EXPECT_TRUE(animator(hero).queued.empty());
    EXPECT_EQ(fired.finishes, std::vector<std::string>{"Wave@-1"});

    // a loop waits for its cycle's end
    step(0.1);      // Walk at 0.3
    object.queue("Run");
    step(0.5);      // 0.8
    EXPECT_EQ(animator(hero).state.a.clip, WALK);
    step(0.4);      // the cycle ended at Walk 1.0 (clock 2.0)
    EXPECT_EQ(animator(hero).state.a.clip, RUN);
    EXPECT_FLOAT_EQ(animator(hero).state.a.start, 2.0f);

    // a once clip that already ended: queue plays now; play clears the queue
    object.play("Wave");
    step(1.5);
    EXPECT_EQ(fired.finishes.size(), 2u);
    EXPECT_FALSE(object.isPlaying());
    object.queue("Bounce");
    EXPECT_EQ(animator(hero).state.a.clip, BOUNCE);
    object.queue("Walk");
    object.play("Run");
    EXPECT_TRUE(animator(hero).queued.empty());
    // a finished once clip fires its finish once
    object.play("Wave");
    step(2.0);
    step(2.0);
    EXPECT_EQ(fired.finishes.size(), 3u);
}

// ---- events ----------------------------------------------------------------------------------

TEST(VertexAnimatorEvents, CrossingsFollowTheClipWrap) {
    const VatData data = heroData();
    auto crossed = [](const VatClip& clip, double from, double to, bool start = false) {
        std::vector<VatCrossedEvent> out;
        vatCrossedEvents(clip, from, to, start, out);
        std::vector<double> positions;
        for (const VatCrossedEvent& e : out) positions.push_back(e.position);
        return positions;
    };
    const VatClip& walk = data.clips[WALK];      // loop, event at 0.25
    EXPECT_EQ(crossed(walk, 0.0, 0.2), std::vector<double>{});
    EXPECT_EQ(crossed(walk, 0.2, 0.25), std::vector<double>{0.25}) << "forward: from < t <= to";
    EXPECT_EQ(crossed(walk, 0.25, 0.5), std::vector<double>{}) << "at most one firing per crossing";
    EXPECT_EQ(crossed(walk, 0.9, 1.3), std::vector<double>{1.25}) << "loop wrap";
    EXPECT_EQ(crossed(walk, 1.3, 4.3), std::vector<double>{2.25}) << "low frame rate: once per update";
    EXPECT_EQ(crossed(walk, -0.5, -1.0), std::vector<double>{-0.75}) << "backward";
    EXPECT_EQ(crossed(walk, -0.75, -1.0), std::vector<double>{}) << "backward: to <= t < from";
    EXPECT_EQ(crossed(walk, 0.25, 0.3, true), std::vector<double>{0.25}) << "a start exactly on the event";

    const VatClip& wave = data.clips[WAVE];      // once, event at 0.5
    EXPECT_EQ(crossed(wave, 0.4, 0.6), std::vector<double>{0.5});
    EXPECT_EQ(crossed(wave, 1.0, 5.0), std::vector<double>{}) << "clamped past the end";
    EXPECT_EQ(crossed(wave, 1.5, 0.2), std::vector<double>{0.5}) << "backward from past the end";

    const VatClip& bounce = data.clips[BOUNCE];  // ping-pong, event at 0.25 (and 1.75 on the way back)
    EXPECT_EQ(crossed(bounce, 0.0, 0.5), std::vector<double>{0.25});
    EXPECT_EQ(crossed(bounce, 0.5, 1.5), std::vector<double>{});
    EXPECT_EQ(crossed(bounce, 1.5, 2.0), std::vector<double>{1.75});
    EXPECT_EQ(crossed(bounce, 2.0, 2.5), std::vector<double>{2.25});

    // several events in crossing order, both ways
    VatClip two = walk;
    ClipEventData late = two.events[0];
    late.time = 0.75f;
    late.function = "late";
    two.events.push_back(late);
    std::vector<VatCrossedEvent> out;
    vatCrossedEvents(two, 0.5, 1.5, false, out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].index, 1u);
    EXPECT_EQ(out[1].index, 0u);
}

TEST_F(VertexAnimatorSystemTest, EventsForwardBackwardPingPongAndLowFrameRates) {
    hero = makeHero();
    listen(hero);
    step(0.0);
    step(0.1);
    EXPECT_TRUE(fired.events.empty());
    step(0.2);
    EXPECT_EQ(fired.events, std::vector<std::string>{"onStep@-1"});
    step(1.0);
    EXPECT_EQ(fired.events.size(), 2u) << "the loop wrapped";
    step(3.0);
    EXPECT_EQ(fired.events.size(), 3u) << "three cycles in one update fire once";

    // backward
    fired.events.clear();
    VertexAnimator object(&scene, hero);
    object.play("Walk");
    object.setSpeed(-1.0f);
    step(0.5);      // 0 -> -0.5: the event is at -0.75
    EXPECT_TRUE(fired.events.empty());
    step(0.5);
    EXPECT_EQ(fired.events, std::vector<std::string>{"onStep@-1"});

    // ping-pong: there and back
    object.setSpeed(1.0f);
    step(0.0);
    fired.events.clear();
    object.play("Bounce");
    step(0.5);
    step(1.0);
    EXPECT_EQ(fired.events.size(), 1u);
    step(0.5);
    EXPECT_EQ(fired.events.size(), 2u);
    EXPECT_EQ(fired.events.back(), "onBounce@-1");

    // events carry the clip and the args
    AnimationEvent got;
    animator(hero).onEvent.add("args", std::function<void(const AnimationEvent&)>([&got](const AnimationEvent& e) { got = e; }));
    object.play("Wave");
    step(0.6);
    EXPECT_EQ(got.functionName, "onWave");
    EXPECT_EQ(got.state, "Wave");
    EXPECT_FLOAT_EQ(got.time, 0.5f);
    ASSERT_EQ(got.args.size(), 1u);
    EXPECT_EQ(got.getStringArg(0), "Wave");
}

TEST_F(VertexAnimatorSystemTest, OnlyTheDominantClipFiresInACrossfade) {
    hero = makeHero();
    listen(hero);
    step(0.0);
    step(0.5);      // Walk's 0.25 fired
    fired.events.clear();
    VertexAnimator object(&scene, hero);
    object.crossFade("Run", 0.8f);  // handover at 0.9; Walk's next event at 1.25 (after it), Run's at 1.0
    step(1.0);
    EXPECT_EQ(fired.events, std::vector<std::string>{"onRun@-1"});
}

TEST_F(VertexAnimatorSystemTest, InstanceEventsOnlyWhenAskedAndNoneInEditMode) {
    hero = makeHero(2);
    listen(hero);
    step(0.0);
    step(0.5);
    EXPECT_TRUE(fired.events.empty()) << "a crowd's own state fires nothing, its instances only with instanceEvents";
    animator(hero).instanceEvents = true;
    step(1.0);
    EXPECT_EQ(fired.events, (std::vector<std::string>{"onStep@0", "onStep@1"}));
    fired.events.clear();
    VertexAnimator(&scene, hero).playInstance(1, "Wave");
    step(1.5);
    EXPECT_EQ(fired.finishes, std::vector<std::string>{"Wave@1"});

    // edit mode: no callbacks at all
    fired = Fired();
    Entity single = makeHero();
    listen(single);
    system()->setPaused(true);
    animator(single).preview = true;
    step(0.0);
    step(2.0);
    EXPECT_TRUE(fired.events.empty());
    EXPECT_FLOAT_EQ(animator(single).lastNow, 2.0f) << "previewed";
}

TEST_F(VertexAnimatorSystemTest, EventsReachLuaSubscribersAndScriptMethods) {
    hero = makeHero();
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    ASSERT_EQ(luaL_dostring(L,
        "vatLog = {}\n"
        "function vatOnEvent(event) vatLog[#vatLog + 1] = event.functionName .. ':' .. tostring(event.args[1]) end\n"
        "function vatOnFinish(clip) vatLog[#vatLog + 1] = 'finish:' .. clip end\n"), LUA_OK);
    lua_getglobal(L, "vatOnEvent");
    animator(hero).onEvent.add("lua", L);
    lua_getglobal(L, "vatOnFinish");
    animator(hero).onFinish.add("lua", L);

    ASSERT_EQ(luaL_dostring(L,
        "VatScript = {}\n"
        "function VatScript:onWave(arg) vatLog[#vatLog + 1] = 'method:' .. arg end\n"
        "vatScriptInstance = setmetatable({}, {__index = VatScript})\n"), LUA_OK);
    lua_getglobal(L, "vatScriptInstance");
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.enabled = true;
    entry.className = "VatScript";
    entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
    scripts.scripts.push_back(entry);
    scene.addComponent<ScriptComponent>(hero, scripts);

    step(0.0);
    VertexAnimator(&scene, hero).play("Wave");
    step(1.5);
    ASSERT_EQ(luaL_dostring(L, "vatResult = table.concat(vatLog, '|')"), LUA_OK);
    lua_getglobal(L, "vatResult");
    const std::string result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    EXPECT_EQ(result, "onWave:Wave|method:Wave|finish:Wave") << "subscribers first, then the script method, then the finish";

    scene.getComponent<ScriptComponent>(hero).scripts.clear();
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    animator(hero).onEvent.clear();
    animator(hero).onFinish.clear();
}

// ---- instances -------------------------------------------------------------------------------

TEST_F(VertexAnimatorSystemTest, InstanceRequestsAndGpuWritesOnlyOnChange) {
    hero = makeHero(3);
    step(0.0);
    VertexAnimatorComponent& a = animator(hero);
    InstancedMeshComponent& instanced = scene.getComponent<InstancedMeshComponent>(hero);
    ASSERT_EQ(a.instanceStates.size(), 3u);
    for (size_t i = 0; i < 3; i++) {
        EXPECT_EQ(a.instanceStates[i].a.clip, WALK);
        Vector4 v0, v1;
        vatPackInstance(a.instanceStates[i], v0, v1);
        EXPECT_EQ(instanced.instances[i].vat0, v0);
        EXPECT_EQ(instanced.instances[i].vat1, v1);
    }
    EXPECT_TRUE(instanced.needUpdateInstances);

    // looping: nothing written
    instanced.needUpdateInstances = false;
    const Vector4 before = instanced.instances[0].vat0;
    step(0.5);
    step(0.7);
    EXPECT_FALSE(instanced.needUpdateInstances);
    EXPECT_EQ(instanced.instances[0].vat0, before);

    VertexAnimator object(&scene, hero);
    object.playInstance(1, "Run");
    EXPECT_TRUE(instanced.needUpdateInstances);
    EXPECT_FLOAT_EQ(instanced.instances[1].vat0.x, (float)RUN);
    EXPECT_FLOAT_EQ(instanced.instances[0].vat0.x, (float)WALK);
    EXPECT_EQ(object.getInstanceClip(1), "Run");
    EXPECT_EQ(object.getInstanceClip(0), "Walk");
    EXPECT_EQ(object.getInstanceClip(9), "");

    object.crossFadeInstance(0, "Bounce", 0.5f);
    EXPECT_EQ(a.instanceStates[0].b.clip, BOUNCE);
    EXPECT_EQ(object.getInstanceClip(0), "Bounce");

    object.setInstanceSpeed(2, 0.5f);
    EXPECT_FLOAT_EQ(a.instanceStates[2].a.speed, 0.5f);
    object.setInstanceTime(2, 0.25f);
    EXPECT_NEAR(object.getInstanceNormalizedTime(2), 0.25f, 1e-4f);

    // an index out of range does nothing
    instanced.needUpdateInstances = false;
    object.playInstance(7, "Run");
    object.playInstance(-1, "Run");
    EXPECT_FALSE(instanced.needUpdateInstances);

    // everyone (and the GameObject)
    object.playAll("Wave");
    for (const VatTrackPair& pair : a.instanceStates) EXPECT_EQ(pair.a.clip, WAVE);
    EXPECT_EQ(a.state.a.clip, WAVE);

    // seeded times: the same seed gives the same starts
    object.playAll("Walk");
    object.randomizeTimes(7);
    std::vector<float> first;
    for (const VatTrackPair& pair : a.instanceStates) first.push_back(pair.a.start);
    object.randomizeTimes(7);
    std::vector<float> second;
    for (const VatTrackPair& pair : a.instanceStates) second.push_back(pair.a.start);
    EXPECT_EQ(first, second);
    EXPECT_NE(first[0], first[1]);
    object.randomizeTimes(8);
    EXPECT_NE(a.instanceStates[0].a.start, first[0]);

    // queue and stop per instance through requests
    VertexAnimatorRequest stop;
    stop.kind = VertexAnimatorRequest::Kind::Stop;
    stop.instance = 2;
    a.requests.push_back(stop);
    step(0.1);
    const float held = object.getInstanceNormalizedTime(2);
    step(0.3);
    EXPECT_NEAR(object.getInstanceNormalizedTime(2), held, 1e-4f);
}

// instanceClips / instanceSpeeds (Create Crowd): each instance starts with its own clip and speed;
// a missing or empty entry is the default clip at speed 1, an unknown clip is the default (warned)
TEST_F(VertexAnimatorSystemTest, InstancesStartWithTheirOwnClipAndSpeed) {
    hero = makeHero(4);
    VertexAnimatorComponent& a = animator(hero);
    a.instanceClips = {"Run", "", "Nope"};
    a.instanceSpeeds = {2.0f, 0.5f};
    step(0.0);
    ASSERT_EQ(a.instanceStates.size(), 4u);
    EXPECT_EQ(a.instanceStates[0].a.clip, RUN);
    EXPECT_EQ(a.instanceStates[1].a.clip, WALK);
    EXPECT_EQ(a.instanceStates[2].a.clip, WALK) << "unknown: the default clip";
    EXPECT_EQ(a.instanceStates[3].a.clip, WALK) << "past the list: the default clip";
    EXPECT_EQ(a.state.a.clip, WALK) << "the GameObject's own state keeps the default";
    EXPECT_FLOAT_EQ(a.instanceStates[0].a.speed, 2.0f);
    EXPECT_FLOAT_EQ(a.instanceStates[1].a.speed, 0.5f);
    EXPECT_FLOAT_EQ(a.instanceStates[2].a.speed, 1.0f);
    EXPECT_FLOAT_EQ(a.instanceStates[3].a.speed, 1.0f);
    // the speed stays the instance's own after a play (setInstanceSpeed semantics)
    VertexAnimator object(&scene, hero);
    object.playInstance(1, "Run");
    EXPECT_FLOAT_EQ(a.instanceStates[1].a.speed, 0.5f);
    // with playAutomatically off nothing plays, whatever the list says
    Entity still = makeHero(2);
    animator(still).playAutomatically = false;
    animator(still).instanceClips = {"Run", "Run"};
    step(0.0);
    EXPECT_EQ(animator(still).instanceStates[0].a.clip, -1);
}

TEST_F(VertexAnimatorSystemTest, RandomStartIsSeededByEntityAndInstance) {
    hero = makeHero(3, true);
    step(0.0);
    const VertexAnimatorComponent& a = animator(hero);
    EXPECT_FLOAT_EQ(a.state.a.start, (float)(0.0 - vatRandomUnit((uint64_t)hero, 0) * 1.0));
    for (size_t i = 0; i < 3; i++) {
        EXPECT_FLOAT_EQ(a.instanceStates[i].a.start, (float)(0.0 - vatRandomUnit((uint64_t)hero, (int64_t)i + 1) * 1.0));
    }
    EXPECT_NE(a.instanceStates[0].a.start, a.instanceStates[1].a.start);

    // another scene with the same ids starts the same
    Scene other;
    const Entity same = other.createUserEntity();
    ASSERT_EQ(same, hero);
    other.addComponent<Transform>(same, {});
    other.addComponent<MeshComponent>(same, {});
    ModelComponent model;
    model.filename = HERO_MESH;
    model.skeleton = NULL_ENTITY;
    model.needUpdateModel = false;
    other.addComponent<ModelComponent>(same, model);
    InstancedMeshComponent instanced;
    instanced.instances.resize(3);
    other.addComponent<InstancedMeshComponent>(same, instanced);
    VertexAnimatorComponent animatorComponent;
    animatorComponent.animation = HERO;
    other.addComponent<VertexAnimatorComponent>(same, animatorComponent);
    other.getSystem<VertexAnimatorSystem>()->update(0.0);
    const VertexAnimatorComponent& b = other.getComponent<VertexAnimatorComponent>(same);
    for (size_t i = 0; i < 3; i++) EXPECT_EQ(a.instanceStates[i].a.start, b.instanceStates[i].a.start);
}

TEST_F(VertexAnimatorSystemTest, InstanceAddAndRemoveKeepStatesAligned) {
    hero = makeHero(3);
    step(0.0);
    VertexAnimator object(&scene, hero);
    object.playInstance(0, "Walk");
    object.playInstance(1, "Wave");
    object.playInstance(2, "Run");
    object.setInstanceSpeed(2, 0.5f);

    // added: started like the animator did
    InstancedMeshComponent& instanced = scene.getComponent<InstancedMeshComponent>(hero);
    instanced.instances.push_back(InstanceData());
    step(0.1);
    ASSERT_EQ(animator(hero).instanceStates.size(), 4u);
    ASSERT_EQ(animator(hero).runtime->instances.size(), 4u);
    EXPECT_EQ(animator(hero).instanceStates[3].a.clip, WALK);

    // removed from the middle (Mesh::removeInstance): the later ones keep theirs
    Mesh mesh(&scene, hero);
    mesh.removeInstance(1);
    ASSERT_EQ(animator(hero).instanceStates.size(), 3u);
    EXPECT_EQ(object.getInstanceClip(1), "Run");
    EXPECT_FLOAT_EQ(animator(hero).instanceStates[1].a.speed, 0.5f);
    EXPECT_EQ(animator(hero).runtime->instances[1].speed, 0.5f);
    step(0.1);
    EXPECT_FLOAT_EQ(instanced.instances[1].vat0.x, (float)RUN);

    // truncated elsewhere: resized at the next update
    instanced.instances.resize(1);
    step(0.1);
    EXPECT_EQ(animator(hero).instanceStates.size(), 1u);
    mesh.clearInstances();
    EXPECT_TRUE(animator(hero).instanceStates.empty());

    // an InstanceData written whole (vat0/vat1 defaults) gets its state again
    instanced.instances.push_back(InstanceData());
    step(0.1);
    instanced.needUpdateInstances = false;
    instanced.instances[0] = InstanceData();
    instanced.needUpdateInstances = true;
    step(0.1);
    EXPECT_FLOAT_EQ(instanced.instances[0].vat0.x, (float)WALK);
}

// ---- time ------------------------------------------------------------------------------------

TEST_F(VertexAnimatorSystemTest, EpochRebaseKeepsPhasesContinuous) {
    hero = makeHero(2);
    step(0.0);
    VertexAnimator object(&scene, hero);
    object.playInstance(1, "Wave");
    VertexAnimatorComponent& a = animator(hero);
    // near the epoch, mid-crossfade on the entity
    a.runtime->clock = VAT_EPOCH_SECONDS - 0.4;
    step(0.0);
    object.crossFade("Run", 0.6f);
    const double normalizedBefore = vatSample(heroData().clips[WALK], (float)tau(a.instanceStates[0].a, hero)).normalized;
    EXPECT_NEAR(normalizedBefore, 0.6, 1e-3);

    step(0.5);      // crosses 2048 s
    EXPECT_EQ(a.runtime->epoch, 1);
    EXPECT_NEAR(a.lastNow, 0.1f, 1e-4f);
    // the loop's phase went on by 0.5 s, its start brought near the clock
    const double normalizedAfter = vatSample(heroData().clips[WALK], (float)tau(a.instanceStates[0].a, hero)).normalized;
    EXPECT_NEAR(normalizedAfter, 0.1, 1e-3);
    EXPECT_LE(std::abs(a.instanceStates[0].a.start), 1.0f);
    // the ended once clip stays at its end
    EXPECT_TRUE(vatSample(heroData().clips[WAVE], (float)tau(a.instanceStates[1].a, hero)).finished);
    // the crossfade is at the same weight: started 0.5 s ago of 0.6
    EXPECT_NEAR(vatWeightB(a.state, a.lastNow), 0.5f / 0.6f, 1e-3f);
    EXPECT_NEAR(tau(a.state.b, hero), 0.5, 1e-3);
    // and every instance written once again
    Vector4 v0, v1;
    vatPackInstance(a.instanceStates[0], v0, v1);
    EXPECT_EQ(scene.getComponent<InstancedMeshComponent>(hero).instances[0].vat0, v0);
}

// The Inspector's preview scrubber: a SetTime request applied at once in edit mode (applyRequests)
TEST_F(VertexAnimatorSystemTest, PreviewScrubSetsTheTimeAtOnce) {
    hero = makeHero(2);
    system()->setPaused(true);
    VertexAnimatorComponent& a = animator(hero);
    a.preview = true;
    step(0.1);
    step(0.1);
    a.speed = 0.001f;
    step(0.1);
    VertexAnimatorRequest request;
    request.kind = VertexAnimatorRequest::Kind::SetTime;
    request.value = 0.9f;
    a.requests.push_back(request);
    request.instance = 1;
    request.value = 0.4f;
    a.requests.push_back(request);
    system()->applyRequests(hero);
    VertexAnimator object(&scene, hero);
    EXPECT_NEAR(object.getTime(), 0.9f, 1e-3f);
    EXPECT_NEAR(object.getInstanceNormalizedTime(1), 0.4f, 1e-3f);
    step(0.1);
    EXPECT_NEAR(object.getTime(), 0.9f, 1e-3f) << "nearly still at speed 0.001";
}

TEST_F(VertexAnimatorSystemTest, EditModeClockMovesOnlyWithPreview) {
    hero = makeHero();
    system()->setPaused(true);
    step(0.0);
    step(1.0);
    VertexAnimatorComponent& a = animator(hero);
    EXPECT_FLOAT_EQ(a.lastNow, 0.0f);
    EXPECT_EQ(a.state.a.clip, WALK) << "drawn at its start";
    EXPECT_TRUE(a.gpuActive);

    a.preview = true;
    step(0.5);
    EXPECT_FLOAT_EQ(a.lastNow, 0.5f);
    a.preview = false;
    step(0.5);
    EXPECT_FLOAT_EQ(a.lastNow, 0.0f) << "Preview off: back at its start";
    EXPECT_FLOAT_EQ(a.state.a.start, 0.0f);

    a.preview = true;
    step(0.3);
    EXPECT_TRUE(system()->stopPreviews());
    EXPECT_FALSE(a.preview);
    EXPECT_FLOAT_EQ(a.lastNow, 0.0f);
    EXPECT_FALSE(system()->stopPreviews());

    // play mode: the clock runs
    system()->setPaused(false);
    step(0.25);
    EXPECT_FLOAT_EQ(a.lastNow, 0.25f);
    system()->resetAnimators();
    EXPECT_FALSE(a.started);
    EXPECT_FLOAT_EQ(a.lastNow, 0.0f);
}

// ---- asset and mesh --------------------------------------------------------------------------

TEST_F(VertexAnimatorSystemTest, MeshMismatchDrawsAtRestUntilFixed) {
    hero = makeHero(-1, false, "models/hero.gltf");
    step(0.0);
    VertexAnimatorComponent& a = animator(hero);
    EXPECT_FALSE(a.gpuActive);
    EXPECT_EQ(a.loadError.compare(0, 14, "mesh_mismatch:"), 0) << a.loadError;
    EXPECT_NE(a.loadError.find("vat/hero.vat.glb"), std::string::npos) << a.loadError;
    EXPECT_NE(a.loadError.find("Use hero.vat.glb"), std::string::npos) << a.loadError;
    EXPECT_FALSE(VertexAnimator(&scene, hero).isLoaded());
    EXPECT_TRUE(scene.getComponent<MeshComponent>(hero).animatedBounds.isNull());

    // loading: waits without an error
    ModelComponent& model = scene.getComponent<ModelComponent>(hero);
    model.filename = HERO_MESH;
    model.needUpdateModel = true;
    step(0.1);
    EXPECT_FALSE(a.gpuActive);
    model.needUpdateModel = false;
    step(0.1);
    EXPECT_TRUE(a.gpuActive);
    EXPECT_EQ(a.loadError, "");

    // no Model at all
    scene.removeComponent<ModelComponent>(hero);
    step(0.1);
    EXPECT_FALSE(a.gpuActive);
    EXPECT_NE(a.loadError.find("has no Model"), std::string::npos) << a.loadError;
}

TEST_F(VertexAnimatorSystemTest, LoadErrorsAndReloadOnARebake) {
    Entity broken = makeHero();
    animator(broken).animation = "vat/missing.vat";
    hero = makeHero();
    step(0.0);
    EXPECT_FALSE(animator(broken).gpuActive);
    EXPECT_FALSE(animator(broken).loadError.empty());
    EXPECT_FALSE(animator(broken).asset);

    step(0.0);
    step(0.5);
    VertexAnimator object(&scene, hero);
    object.playInstance(0, "Walk");     // (no instances: warned, nothing)
    object.play("Run");
    const uint64_t generation = animator(hero).loadedGeneration;
    // rebaked with another clip order: it plays the clip of the same name
    VertexAnimationPool::put(HERO, heroAsset(true));
    step(0.1);
    EXPECT_GT(animator(hero).loadedGeneration, generation);
    EXPECT_EQ(animator(hero).asset->getData().clips[0].name, "Run");
    EXPECT_EQ(animator(hero).state.a.clip, 0);
    EXPECT_EQ(object.getCurrentClip(), "Run");
    EXPECT_NEAR(tau(animator(hero).state.a, hero), 0.1, 1e-4);
}

TEST_F(VertexAnimatorSystemTest, AttachmentsFollowTheirSocket) {
    hero = makeHero();
    const Entity sword = scene.createUserEntity();
    Transform swordTransform;
    swordTransform.parent = hero;
    swordTransform.scale = Vector3(2, 2, 2);
    scene.addComponent<Transform>(sword, swordTransform);
    animator(hero).attachments = {{"RightHand", sword}, {"Nope", sword}};
    step(0.0);
    step(0.5);
    Vector3 position;
    Quaternion rotation;
    ASSERT_TRUE(animator(hero).asset->sampleSocket(0, animator(hero).state, animator(hero).lastNow, position, rotation));
    const Transform& t = scene.getComponent<Transform>(sword);
    EXPECT_EQ(t.position, position);
    EXPECT_EQ(t.rotation, rotation);
    EXPECT_EQ(t.scale, Vector3(2, 2, 2)) << "its own scale";
    EXPECT_NEAR(position.x, 2.0f, 1e-4f) << "Walk at 0.5 s: global frame 2";

    // the socket pose read from scripts
    VertexAnimatorSocketPose pose = VertexAnimator(&scene, hero).getSocketPose("RightHand");
    EXPECT_TRUE(pose.valid);
    EXPECT_EQ(pose.position, position);
    EXPECT_FALSE(VertexAnimator(&scene, hero).getSocketPose("Nope").valid);
    EXPECT_FALSE(VertexAnimator(&scene, hero).getSocketPose("RightHand", 3).valid) << "no such instance";

    step(0.25);
    EXPECT_NEAR(scene.getComponent<Transform>(sword).position.x, 3.0f, 1e-4f);
}

TEST_F(VertexAnimatorSystemTest, AnimatedBoundsAreTheClipsInUse) {
    hero = makeHero();
    step(0.0);
    const VatData data = heroData();
    MeshComponent& mesh = scene.getComponent<MeshComponent>(hero);
    EXPECT_EQ(mesh.animatedBounds, AABB(data.clips[WALK].bounds.min, data.clips[WALK].bounds.max));
    VertexAnimator object(&scene, hero);
    object.play("Run");
    EXPECT_EQ(mesh.animatedBounds, AABB(data.clips[RUN].bounds.min, data.clips[RUN].bounds.max));
    object.crossFade("Wave", 0.5f);
    VatBounds both = data.clips[RUN].bounds;
    both.extend(data.clips[WAVE].bounds);
    EXPECT_EQ(mesh.animatedBounds, AABB(both.min, both.max));

    // a crowd: its instances' clips
    Entity crowd = makeHero(2);
    step(0.0);
    VertexAnimator(&scene, crowd).playInstance(1, "Bounce");
    VatBounds crowdBounds = data.clips[WALK].bounds;
    crowdBounds.extend(data.clips[BOUNCE].bounds);
    EXPECT_EQ(scene.getComponent<MeshComponent>(crowd).animatedBounds, AABB(crowdBounds.min, crowdBounds.max));

    // rest pose: the mesh's own (the clips' bounds don't hold it)
    mesh.verticesAABB = AABB(Vector3(-0.5f, 0.0f, -0.5f), Vector3(0.5f, 1.5f, 0.5f));
    animator(hero).playAutomatically = false;
    system()->resetAnimators();
    step(0.0);
    EXPECT_EQ(mesh.animatedBounds, mesh.verticesAABB);
    // disabled or removed: none
    animator(hero).enabled = false;
    step(0.0);
    EXPECT_TRUE(mesh.animatedBounds.isNull());
    animator(hero).enabled = true;
    step(0.0);
    EXPECT_FALSE(mesh.animatedBounds.isNull());
    scene.removeComponent<VertexAnimatorComponent>(hero);
    EXPECT_TRUE(mesh.animatedBounds.isNull());
}

// ---- review fixes ----------------------------------------------------------------------------

// The crossing window (previous now, now] uses the previous clock exactly: a float copy of it
// rounds by up to 30 us at 1000 s, so an event just crossed fired again at the next update
TEST_F(VertexAnimatorSystemTest, AnEventJustCrossedAtALargeClockFiresOnce) {
    VatData data = heroData();
    data.clips[WALK].events[0].time = 0.3f;
    VertexAnimationPool::put(HERO, heroAssetOf(data));
    hero = makeHero();
    listen(hero);
    step(0.0);
    step(1000.0);
    fired.events.clear();
    step(0.30001);
    EXPECT_EQ(fired.events, std::vector<std::string>{"onStep@-1"});
    step(0.01);
    step(0.01);
    EXPECT_EQ(fired.events.size(), 1u) << "the event at 1000.3 fired again";
}

// A steady frame rate fires every event once: 50 events a second at 60 fps, from clocks where a
// float copy of the clock rounds (512, 1024) and across the epoch rebase (2047 -> 2048)
TEST_F(VertexAnimatorSystemTest, EventsAtASteadyFrameRateFireExactlyOnce) {
    VatData data = heroData();
    std::vector<ClipEventData> events;
    for (int i = 0; i < 50; i++) {
        ClipEventData event;
        event.time = (float)((i + 0.5) / 50.0);
        event.function = "onTick";
        events.push_back(event);
    }
    data.clips[WALK].events = events;
    VertexAnimationPool::put(HERO, heroAssetOf(data));

    struct Run { double from; int frames; };
    for (const Run run : {Run{512.0, 600}, Run{1024.0, 36000}, Run{2047.0, 600}}) {
        Scene runScene;
        const Entity entity = runScene.createUserEntity();
        runScene.addComponent<Transform>(entity, {});
        runScene.addComponent<MeshComponent>(entity, {});
        ModelComponent model;
        model.filename = HERO_MESH;
        model.skeleton = NULL_ENTITY;
        model.needUpdateModel = false;
        runScene.addComponent<ModelComponent>(entity, model);
        VertexAnimatorComponent added;
        added.animation = HERO;
        added.randomStart = false;
        runScene.addComponent<VertexAnimatorComponent>(entity, added);
        size_t count = 0;
        runScene.getComponent<VertexAnimatorComponent>(entity).onEvent.add("count",
            std::function<void(const AnimationEvent&)>([&count](const AnimationEvent&) { count++; }));
        std::shared_ptr<VertexAnimatorSystem> runSystem = runScene.getSystem<VertexAnimatorSystem>();
        runSystem->update(0.0);
        runSystem->update(run.from);
        count = 0;
        for (int f = 0; f < run.frames; f++) runSystem->update(1.0 / 60.0);
        const double end = runScene.getComponent<VertexAnimatorComponent>(entity).runtime->clock;
        // the events (i + 0.5) / 50 in (from, end]
        const size_t expected = (size_t)(std::floor(end * 50.0 - 0.5) - std::floor(run.from * 50.0 - 0.5));
        EXPECT_EQ(count, expected) << "from clock " << run.from << " for " << run.frames << " frames";
        EXPECT_EQ(expected, (size_t)(run.frames * 50 / 60));
    }
}

// An InstanceData written whole inside a callback (after the system wrote the states, before the
// RenderSystem uploads and clears needUpdateInstances) gets its state back the same update; and
// Mesh::updateInstance keeps vat0/vat1 (runtime state, not user data)
TEST_F(VertexAnimatorSystemTest, AnInstanceRewrittenInACallbackKeepsItsState) {
    hero = makeHero(3);
    animator(hero).instanceEvents = true;
    step(0.0);
    InstancedMeshComponent& instanced = scene.getComponent<InstancedMeshComponent>(hero);
    animator(hero).onInstanceEvent.add("rewrite", std::function<void(int, const AnimationEvent&)>([&](int i, const AnimationEvent&) {
        if (i == 0) {
            InstanceData fresh;
            fresh.position = Vector3(1.0f, 2.0f, 3.0f);
            instanced.instances[0] = fresh;
            instanced.needUpdateInstances = true;
        }
    }));
    instanced.needUpdateInstances = false;     // as the RenderSystem leaves it
    step(0.3);      // Walk's 0.25 crossed: the callback runs
    EXPECT_EQ(instanced.instances[0].position, Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_TRUE(packedAs(instanced.instances[0], animator(hero).instanceStates[0])) << "drawn at rest until its state changes";
    EXPECT_TRUE(instanced.needUpdateInstances);
    instanced.needUpdateInstances = false;
    step(0.1);
    EXPECT_TRUE(packedAs(instanced.instances[0], animator(hero).instanceStates[0]));
    animator(hero).onInstanceEvent.remove("rewrite");

    // Mesh::updateInstance(index, InstanceData) and its overloads keep the instance's state
    Mesh mesh(&scene, hero);
    InstanceData moved;
    moved.position = Vector3(4.0f, 0.0f, 0.0f);
    mesh.updateInstance(1, moved);
    EXPECT_EQ(instanced.instances[1].position, Vector3(4.0f, 0.0f, 0.0f));
    EXPECT_TRUE(packedAs(instanced.instances[1], animator(hero).instanceStates[1]));
    mesh.updateInstance(2, Vector3(5.0f, 0.0f, 0.0f));
    EXPECT_TRUE(packedAs(instanced.instances[2], animator(hero).instanceStates[2]));
}

// randomizeTimes re-seeds A's time and keeps a blend (B follows A's phase)
TEST_F(VertexAnimatorSystemTest, RandomizeTimesKeepsABlend) {
    hero = makeHero();
    step(0.0);
    step(0.3);
    VertexAnimator object(&scene, hero);
    object.blend("Walk", "Run", 0.7f);
    object.randomizeTimes(7);
    const VatTrackPair& pair = animator(hero).state;
    EXPECT_EQ(pair.a.clip, WALK);
    EXPECT_EQ(pair.b.clip, RUN);
    EXPECT_FLOAT_EQ(pair.blend, 0.7f);
    EXPECT_NEAR(tau(pair.a, hero), vatRandomUnit(7, 0) * 1.0, 1e-4);
    EXPECT_FLOAT_EQ(pair.b.start, pair.a.start);
    EXPECT_FLOAT_EQ(pair.b.speed, pair.a.speed);
}

// A callback that removes an instance: the callbacks still to dispatch in that update follow the
// instances (the later ones move down) and the removed one's are dropped
TEST_F(VertexAnimatorSystemTest, CallbacksAfterAnInstanceRemovalUseTheNewIndices) {
    hero = makeHero(3);
    animator(hero).instanceEvents = true;
    listen(hero);
    step(0.0);
    animator(hero).onInstanceEvent.add("remove", std::function<void(int, const AnimationEvent&)>([&](int i, const AnimationEvent&) {
        if (i == 0 && scene.getComponent<InstancedMeshComponent>(hero).instances.size() == 3) {
            Mesh(&scene, hero).removeInstance(1);
        }
    }));
    step(0.3);      // the three instances cross Walk's 0.25 together
    EXPECT_EQ(fired.events, (std::vector<std::string>{"onStep@0", "onStep@1"})) << "instance 2 is now 1; 1 is gone";

    // every instance cleared: none of theirs is called
    fired = Fired();
    animator(hero).onInstanceEvent.remove("remove");
    animator(hero).onInstanceEvent.add("clear", std::function<void(int, const AnimationEvent&)>([&](int, const AnimationEvent&) {
        Mesh(&scene, hero).clearInstances();
    }));
    step(1.0);
    EXPECT_EQ(fired.events.size(), 1u);
    animator(hero).onInstanceEvent.remove("clear");
}

// A mesh drawn at rest (no clip, or an instance with none) is culled with its rest bounds too
TEST_F(VertexAnimatorSystemTest, AnimatedBoundsHoldTheRestPoseWhenATrackIsAtRest) {
    const VatData data = heroData();
    hero = makeHero(2);
    animator(hero).playAutomatically = false;
    MeshComponent& mesh = scene.getComponent<MeshComponent>(hero);
    const AABB rest(Vector3(-0.5f, 0.0f, -0.5f), Vector3(0.5f, 1.5f, 4.0f));
    mesh.verticesAABB = rest;
    step(0.0);
    EXPECT_EQ(mesh.animatedBounds, rest) << "every instance at rest";

    VertexAnimator object(&scene, hero);
    object.playInstance(0, "Walk");
    AABB both(data.clips[WALK].bounds.min, data.clips[WALK].bounds.max);
    both.merge(rest);
    EXPECT_EQ(mesh.animatedBounds, both) << "instance 1 is still at rest";
    object.playInstance(1, "Walk");
    EXPECT_EQ(mesh.animatedBounds, AABB(data.clips[WALK].bounds.min, data.clips[WALK].bounds.max));

    // the mesh's own bounds known later (its model loaded after the animator started)
    Entity single = makeHero();
    animator(single).playAutomatically = false;
    step(0.0);
    MeshComponent& singleMesh = scene.getComponent<MeshComponent>(single);
    singleMesh.verticesAABB = rest;
    step(0.0);
    EXPECT_EQ(singleMesh.animatedBounds, rest);
}

// Held tracks (speed 0, stop) are packed once (speed 0, start = the held time): a paused crowd
// costs no instance upload per frame, and the epoch rebase leaves them as they are
TEST_F(VertexAnimatorSystemTest, HeldInstancesAreWrittenOnce) {
    hero = makeHero(3);
    step(0.0);
    step(0.2);
    VertexAnimator object(&scene, hero);
    InstancedMeshComponent& instanced = scene.getComponent<InstancedMeshComponent>(hero);
    object.setInstanceSpeed(0, 0.0f);
    object.setInstanceSpeed(1, 0.0f);
    step(0.1);
    VertexAnimatorRequest stop;
    stop.kind = VertexAnimatorRequest::Kind::Stop;
    stop.instance = 2;
    animator(hero).requests.push_back(stop);
    system()->applyRequests(hero);
    for (size_t i = 0; i < 3; i++) {
        EXPECT_FLOAT_EQ(instanced.instances[i].vat0.z, 0.0f) << i;
        EXPECT_NEAR(instanced.instances[i].vat0.y, i < 2 ? 0.2f : 0.3f, 1e-5f) << "the held local time, " << i;
    }
    instanced.needUpdateInstances = false;
    for (int f = 0; f < 10; f++) step(1.0 / 60.0);
    EXPECT_FALSE(instanced.needUpdateInstances) << "no instance upload while held";
    EXPECT_NEAR(object.getInstanceNormalizedTime(0), 0.2f, 1e-5f);
    EXPECT_NEAR(object.getInstanceNormalizedTime(2), 0.3f, 1e-5f);

    // the epoch rebase: the held times stay
    animator(hero).runtime->clock = VAT_EPOCH_SECONDS - 0.1;
    step(0.0);
    step(0.2);
    EXPECT_EQ(animator(hero).runtime->epoch, 1);
    EXPECT_NEAR(object.getInstanceNormalizedTime(0), 0.2f, 1e-5f);
    EXPECT_NEAR(object.getInstanceNormalizedTime(2), 0.3f, 1e-5f);
    // played again: from the held time on
    object.setInstanceSpeed(0, 1.0f);
    step(0.25);
    EXPECT_NEAR(object.getInstanceNormalizedTime(0), 0.45f, 1e-4f);
}

// Disabled: the mesh draws at rest at once (single and instanced), once; enabled again: the
// states are written back
TEST_F(VertexAnimatorSystemTest, DisablingWritesTheRestStateOnce) {
    hero = makeHero(2);
    Entity single = makeHero();
    step(0.0);
    step(0.3);
    InstancedMeshComponent& instanced = scene.getComponent<InstancedMeshComponent>(hero);
    ASSERT_TRUE(packedAs(instanced.instances[0], animator(hero).instanceStates[0]));
    instanced.needUpdateInstances = false;
    animator(hero).enabled = false;
    animator(single).enabled = false;
    step(0.1);
    EXPECT_FALSE(animator(hero).gpuActive);
    EXPECT_FALSE(animator(single).gpuActive);
    EXPECT_TRUE(restPacked(instanced.instances[0]));
    EXPECT_TRUE(restPacked(instanced.instances[1]));
    EXPECT_TRUE(instanced.needUpdateInstances);
    EXPECT_EQ(animator(single).gpuState0, VertexAnimatorComponent().gpuState0);
    EXPECT_EQ(animator(single).gpuState1, VertexAnimatorComponent().gpuState1);
    instanced.needUpdateInstances = false;
    step(0.1);
    EXPECT_FALSE(instanced.needUpdateInstances) << "written once";

    animator(hero).enabled = true;
    animator(single).enabled = true;
    step(0.1);
    EXPECT_TRUE(animator(hero).gpuActive);
    EXPECT_TRUE(packedAs(instanced.instances[0], animator(hero).instanceStates[0]));
    EXPECT_TRUE(packedAs(instanced.instances[1], animator(hero).instanceStates[1]));
    EXPECT_TRUE(instanced.needUpdateInstances);
    Vector4 v0, v1;
    vatPackInstance(animator(single).state, v0, v1);
    EXPECT_EQ(animator(single).gpuState0, v0);
}

// A request made from a callback applies right away, at the update's clock
TEST_F(VertexAnimatorSystemTest, RequestsFromCallbacksApplyAtOnce) {
    hero = makeHero();
    step(0.0);
    VertexAnimator object(&scene, hero);
    object.play("Wave");
    animator(hero).onFinish.add("next", std::function<void(const std::string&)>([&](const std::string&) {
        VertexAnimator(&scene, hero).play("Run");
    }));
    step(1.2);
    EXPECT_EQ(animator(hero).state.a.clip, RUN);
    EXPECT_FLOAT_EQ(animator(hero).state.a.start, 1.2f) << "at the update's clock";
    Vector4 v0, v1;
    vatPackInstance(animator(hero).state, v0, v1);
    EXPECT_EQ(animator(hero).gpuState0, v0) << "and written for this frame's draw";
    animator(hero).onFinish.remove("next");
}

// ---- Lua -------------------------------------------------------------------------------------

TEST_F(VertexAnimatorSystemTest, LuaApiThroughTheBindings) {
    hero = makeHero(2);
    const Entity plain = scene.createUserEntity();
    scene.addComponent<Transform>(plain, {});
    step(0.0);
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    luabridge::setGlobal(L, &scene, "vatScene");
    luabridge::setGlobal(L, hero, "vatHero");
    luabridge::setGlobal(L, plain, "vatPlain");
    const int status = luaL_dostring(L,
        "local go = GameObject(vatScene, vatHero)\n"
        "local va = go:getVertexAnimator()\n"
        "vatNone = GameObject(vatScene, vatPlain):getVertexAnimator() == nil\n"
        "vatHas = go:hasVertexAnimator()\n"
        "va:play('Wave')\n"
        "vatCurrent = va.currentClip\n"
        "vatPlaying = va:isPlaying('Wave')\n"
        "vatNames = table.concat(va:getClipNames(), ',')\n"
        "vatLength = va:getClipLength('Run')\n"
        "va:crossFade('Run', 0.5)\n"
        "va:blend('Walk', 'Run', 0.25)\n"
        "va:queue('Wave')\n"
        "va:stop()\n"
        "vatStopped = not va:isPlaying()\n"
        "va:play('Walk')\n"
        "va.time = 0.5\n"
        "vatTime = va.time\n"
        "vatNormalized = va.normalizedTime\n"
        "va.speed = 2\n"
        "local pose = va:getSocketPose('RightHand')\n"
        "vatPose = pose.valid and pose.position.y\n"
        "vatInstancePose = va:getSocketPose('RightHand', 1).valid\n"
        "va:playInstance(0, 'Run')\n"
        "va:playInstance(1, 'Bounce', 0.25)\n"
        "va:crossFadeInstance(0, 'Walk', 0.5)\n"
        "va:setInstanceSpeed(1, 0.5)\n"
        "va:setInstanceTime(1, 0.25)\n"
        "vatInstanceClip = va:getInstanceClip(1)\n"
        "vatInstanceTime = va:getInstanceNormalizedTime(1)\n"
        "vatCount = va:getInstanceCount()\n"
        "va:playAll('Wave', 0.2)\n"
        "va:playAll('Walk')\n"
        "va:randomizeTimes(3)\n"
        "va.onEvent = function(e) end\n"
        "va.onFinish = function(c) end\n"
        "va.onInstanceEvent = function(i, e) end\n"
        "va.onInstanceFinish = function(i, c) end\n"
        "va.preview = false\n"
        "va.instanceEvents = true\n"
        "vatAnimation = va.animation\n"
        "vatLoaded = va:isLoaded()\n"
        "vatError = va:getLoadError()\n"
        "local comp = va:getVertexAnimatorComponent()\n"
        "vatCompSpeed = comp.speed\n"
        "vatGpu = comp.gpuActive\n");
    ASSERT_EQ(status, LUA_OK) << lua_tostring(L, -1);
    auto global = [L](const char* name) { return luabridge::getGlobal(L, name); };
    EXPECT_TRUE(global("vatNone").cast<bool>().value());
    EXPECT_TRUE(global("vatHas").cast<bool>().value());
    EXPECT_EQ(global("vatCurrent").cast<std::string>().value(), "Wave");
    EXPECT_TRUE(global("vatPlaying").cast<bool>().value());
    EXPECT_EQ(global("vatNames").cast<std::string>().value(), "Walk,Wave,Bounce,Run");
    EXPECT_FLOAT_EQ(global("vatLength").cast<float>().value(), 1.0f);
    EXPECT_TRUE(global("vatStopped").cast<bool>().value());
    EXPECT_NEAR(global("vatTime").cast<float>().value(), 0.5f, 1e-4f);
    EXPECT_NEAR(global("vatNormalized").cast<float>().value(), 0.5f, 1e-4f);
    EXPECT_FLOAT_EQ(global("vatPose").cast<float>().value(), 1.0f);
    EXPECT_TRUE(global("vatInstancePose").cast<bool>().value());
    EXPECT_EQ(global("vatInstanceClip").cast<std::string>().value(), "Bounce");
    EXPECT_NEAR(global("vatInstanceTime").cast<float>().value(), 0.25f, 1e-4f) << "setInstanceTime during a crossfade: the clip faded to";
    EXPECT_EQ(global("vatCount").cast<int>().value(), 2);
    EXPECT_EQ(global("vatAnimation").cast<std::string>().value(), HERO);
    EXPECT_TRUE(global("vatLoaded").cast<bool>().value());
    EXPECT_EQ(global("vatError").cast<std::string>().value(), "");
    EXPECT_FLOAT_EQ(global("vatCompSpeed").cast<float>().value(), 2.0f);
    EXPECT_TRUE(global("vatGpu").cast<bool>().value());
    VertexAnimatorComponent& a = animator(hero);
    EXPECT_FLOAT_EQ(a.speed, 2.0f);
    EXPECT_TRUE(a.instanceEvents);
    EXPECT_TRUE(a.onEvent.hasSubscribers());
    EXPECT_TRUE(a.onInstanceFinish.hasSubscribers());
    for (const VatTrackPair& pair : a.instanceStates) EXPECT_EQ(pair.a.clip, WALK);

    // the script reset clears them, as Animation Player callbacks
    scene.removeSubscriptionsByTag("");
    a.onEvent.clear();
    a.onFinish.clear();
    a.onInstanceEvent.clear();
    a.onInstanceFinish.clear();
}
