// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Upstream's animation components on the reflection tables (docs/specs/S08-reflection.md,
// Deviations "Upstream animation"): the Pose Animator and the Animation Player round-trip through
// their descriptors' codecs, a clip entry's fields resolve through the struct table, the schema
// offers the wrap tokens, and the Mesh's morph weights and the Model's animationEntities are
// table fields. Upstream's Vertex Animator (round 3) round-trips the same way: its attachments'
// entities are entity references, and a decode over an animator keeps its Preview toggle only.

#include "reflect/Reflect.h"
#include "reflect/TypeOps.h"

#include "Stream.h"
#include "component/AnimationPlayerComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/VertexAnimatorComponent.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

// (a named namespace, tests included: a unity build merges the test sources)
namespace clip_players_reflect_test {

    template<typename T>
    T roundTrip(ComponentType type, const T& value) {
        const reflect::ComponentDesc* desc = reflect::find(type);
        EXPECT_NE(desc, nullptr);
        const YAML::Node node = reflect::encode(*desc, &value, reflect::EncodeContext{});
        T decoded;
        reflect::DecodeContext ctx;
        ctx.component = desc->key;
        reflect::DecodeReport report;
        reflect::decode(*desc, YAML::Load(YAML::Dump(node)), &decoded, ctx, report);
        EXPECT_TRUE(report.skipped.empty());
        return decoded;
    }

TEST(ClipPlayersReflect, PoseAnimatorRoundTrip) {
    PoseAnimatorComponent animator;
    animator.animation = AssetRef("poses/run.poseanim", Guid::parse("0123456789abcdef0123456789abcdef"));
    animator.autoplay = false;
    animator.speed = 1.5f;
    animator.weight = 0.25f;
    const PoseAnimatorComponent back = roundTrip(ComponentType::PoseAnimatorComponent, animator);
    EXPECT_EQ(back.animation, animator.animation);
    EXPECT_EQ(back.autoplay, false);
    EXPECT_EQ(back.speed, 1.5f);
    EXPECT_EQ(back.weight, 0.25f);

    const reflect::ComponentDesc& desc = *reflect::find(ComponentType::PoseAnimatorComponent);
    EXPECT_STREQ(desc.key, "poseanimator");
    EXPECT_FALSE(desc.structural);
    const reflect::FieldDesc* weight = reflect::findField(desc, "weight");
    ASSERT_NE(weight, nullptr);
    EXPECT_TRUE(weight->updateFlags & UpdateFlags_PoseAnimator_Reload);
    EXPECT_EQ(reflect::findField(desc, "animation")->asset, reflect::AssetKind::PoseAnimation);
}

TEST(ClipPlayersReflect, AnimationPlayerRoundTrip) {
    AnimationPlayerComponent player;
    player.enabled = false;
    player.clips = {{"anim/walk.anim", "0123456789abcdef0123456789abcdef", "Walk", ClipWrapMode::PingPong},
                    {"models/hero.gltf#Run", "", "", ClipWrapMode::Default}};
    player.defaultClip = "Walk";
    player.playAutomatically = false;
    player.speed = 2.0f;
    player.defaultFadeTime = 0.5f;
    const AnimationPlayerComponent back = roundTrip(ComponentType::AnimationPlayerComponent, player);
    EXPECT_EQ(back.enabled, false);
    EXPECT_EQ(back.clips, player.clips);
    EXPECT_EQ(back.defaultClip, "Walk");
    EXPECT_EQ(back.playAutomatically, false);
    EXPECT_EQ(back.speed, 2.0f);
    EXPECT_EQ(back.defaultFadeTime, 0.5f);

    // a clip entry's fields through the struct table
    const reflect::ComponentDesc& desc = *reflect::find(ComponentType::AnimationPlayerComponent);
    const reflect::FieldRef wrap = reflect::resolve(desc, &player, "clips[0].wrap");
    ASSERT_TRUE(wrap);
    EXPECT_EQ(wrap.ref, &player.clips[0].wrap);
    EXPECT_EQ(wrap.ops->kind, reflect::Kind::Enum);
    EXPECT_EQ(wrap.ops->propertyType, PropertyType::Custom) << "a one-byte enum is never an int-sized Enum property";
    EXPECT_EQ(wrap.ops->size, sizeof(ClipWrapMode));
    EXPECT_EQ(reflect::readEnum(*wrap.field, wrap.ref), static_cast<int>(ClipWrapMode::PingPong));
    EXPECT_TRUE(reflect::resolve(desc, &player, "clips[1].clip"));
    EXPECT_FALSE(reflect::resolve(desc, &player, "clips[2].clip"));
    EXPECT_FALSE(reflect::resolve(desc, &player, "clips[01].clip")) << "one spelling per index";

    // the schema offers the wrap tokens anim::wrapName writes
    const std::string schema = reflect::schema(desc).dump();
    for (const char* token : {"default", "once", "loop", "ping_pong", "clamp_forever"}) {
        EXPECT_NE(schema.find(std::string("\"") + token + "\""), std::string::npos) << token;
    }
}

TEST(ClipPlayersReflect, MeshAndModelFields) {
    const reflect::ComponentDesc& mesh = *reflect::find(ComponentType::MeshComponent);
    MeshComponent component;
    component.morphWeights[3] = 0.75f;
    const reflect::FieldRef weight = reflect::resolve(mesh, &component, "morphWeights[3]");
    ASSERT_TRUE(weight);
    EXPECT_EQ(weight.ref, &component.morphWeights[3]);
    EXPECT_FALSE(reflect::resolve(mesh, &component, "morphWeights[" + std::to_string(MAX_MORPHTARGETS) + "]"));

    const reflect::ComponentDesc& model = *reflect::find(ComponentType::ModelComponent);
    ModelComponent modelComponent;
    modelComponent.animationEntities = false;
    ASSERT_NE(reflect::findField(model, "animationEntities"), nullptr);
    const ModelComponent back = roundTrip(ComponentType::ModelComponent, modelComponent);
    EXPECT_FALSE(back.animationEntities);
}

TEST(ClipPlayersReflect, VertexAnimatorRoundTrip) {
    VertexAnimatorComponent animator;
    animator.enabled = false;
    animator.animation = "crowd/walk.vat";
    animator.guid = "fedcba9876543210fedcba9876543210";
    animator.defaultClip = "Run";
    animator.playAutomatically = false;
    animator.speed = 1.25f;
    animator.randomStart = false;
    animator.defaultFadeTime = 0.4f;
    animator.instanceEvents = true;
    animator.attachments = {{"Hand", 1042}, {"Head", NULL_ENTITY}};
    animator.instanceClips = {"Walk", "", "Run"};
    animator.instanceSpeeds = {1.0f, 0.5f};
    const VertexAnimatorComponent back = roundTrip(ComponentType::VertexAnimatorComponent, animator);
    EXPECT_EQ(back.enabled, false);
    EXPECT_EQ(back.animation, "crowd/walk.vat");
    EXPECT_EQ(back.guid, animator.guid);
    EXPECT_EQ(back.defaultClip, "Run");
    EXPECT_EQ(back.playAutomatically, false);
    EXPECT_EQ(back.speed, 1.25f);
    EXPECT_EQ(back.randomStart, false);
    EXPECT_EQ(back.defaultFadeTime, 0.4f);
    EXPECT_EQ(back.instanceEvents, true);
    EXPECT_EQ(back.attachments, animator.attachments);
    EXPECT_EQ(back.instanceClips, animator.instanceClips);
    EXPECT_EQ(back.instanceSpeeds, animator.instanceSpeeds);

    const reflect::ComponentDesc& desc = *reflect::find(ComponentType::VertexAnimatorComponent);
    EXPECT_STREQ(desc.key, "vertexanimator");
    EXPECT_STREQ(desc.displayName, "Vertex Animator");
    EXPECT_TRUE(desc.hasEnabled);
    EXPECT_FALSE(desc.structural);
    const reflect::FieldDesc* animation = reflect::findField(desc, "animation");
    ASSERT_NE(animation, nullptr);
    EXPECT_EQ(animation->asset, reflect::AssetKind::VertexAnimation);
    EXPECT_TRUE(animation->updateFlags & UpdateFlags_VertexAnimator_Reload);
    EXPECT_TRUE(reflect::findField(desc, "instanceClips")->updateFlags & UpdateFlags_VertexAnimator_Reload);
    EXPECT_EQ(reflect::findField(desc, "preview"), nullptr) << "the Inspector's Preview is runtime, not a property";

    // an attachment's fields through the struct table; its entity is an entity reference
    const reflect::FieldRef entity = reflect::resolve(desc, &animator, "attachments[0].entity");
    ASSERT_TRUE(entity);
    EXPECT_EQ(entity.ref, &animator.attachments[0].entity);
    EXPECT_EQ(entity.ops, &reflect::ops::Entity);
    EXPECT_TRUE(reflect::resolve(desc, &animator, "attachments[1].socket"));
    EXPECT_FALSE(reflect::resolve(desc, &animator, "attachments[2].socket"));
}

TEST(ClipPlayersReflect, VertexAnimatorDecodeOverKeepsPreviewOnly) {
    const reflect::ComponentDesc& desc = *reflect::find(ComponentType::VertexAnimatorComponent);
    VertexAnimatorComponent source;
    source.animation = "crowd/run.vat";
    const YAML::Node node = YAML::Load(YAML::Dump(reflect::encode(desc, &source, reflect::EncodeContext{})));

    VertexAnimatorComponent target;
    target.animation = "crowd/walk.vat";
    target.preview = true;
    target.started = true;
    target.queued = {"Wave"};
    target.speed = 3.0f;
    const VertexAnimatorComponent old = target;
    reflect::DecodeContext ctx;
    ctx.component = desc.key;
    ctx.old = &old;
    reflect::DecodeReport report;
    reflect::decode(desc, node, &target, ctx, report);
    EXPECT_TRUE(report.skipped.empty());
    EXPECT_EQ(target.animation, "crowd/run.vat");
    EXPECT_TRUE(target.preview) << "the Inspector's Preview toggle stays";
    EXPECT_FALSE(target.started) << "what it played starts over";
    EXPECT_TRUE(target.queued.empty());
    EXPECT_EQ(target.speed, 1.0f) << "a key at its default is the default, not the old value";
}

}
