// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The shadow cache's decisions (spec S10 AC10): every update path of R17 and every
// invalidation cause of R18 gives the expected path and cause; receiver-only edits skip. The
// caster registry's change reports feed it.

#include "render/shadow/ShadowCache.h"
#include "render/shadow/ShadowCasterRegistry.h"

#include "math/Matrix4.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;

namespace {

    // an orthographic shadow camera over [-10, 10]^2 looking down -Z from z = 50
    Matrix4 slotCamera(float x = 0.0f){
        return Matrix4::orthoMatrix(-10.0f + x, 10.0f + x, -10.0f, 10.0f, 0.0f, 100.0f) *
               Matrix4::lookAtMatrix(Vector3(0, 0, 50), Vector3(0, 0, 0), Vector3(0, 1, 0));
    }

    ShadowBounds box(float x, float y, float z, float half = 0.5f){
        ShadowBounds b;
        b.min = Vector3(x - half, y - half, z - half);
        b.max = Vector3(x + half, y + half, z + half);
        b.valid = true;
        return b;
    }

    ShadowSlotInput slotInput(){
        ShadowSlotInput input;
        input.key.viewProjection = slotCamera();
        input.key.rect = ShadowRect{0, 0, 1024, 1024};
        input.key.casterMask = 1u;
        input.key.lightRevision = 1;
        input.key.atlasGeneration = 1;
        input.hasStaticLayer = true;
        input.staticRect = ShadowRect{0, 0, 1024, 1024};
        input.staticGeneration = 1;
        return input;
    }

    ShadowCasterChange moved(ShadowBounds from, ShadowBounds to, bool isStatic, uint32_t layers = 1u){
        ShadowCasterChange change;
        change.oldBounds = from;
        change.newBounds = to;
        change.layerBits = layers;
        change.staticLayer = isStatic;
        change.dynamicLayer = !isStatic;
        return change;
    }

    const std::vector<ShadowCasterChange> none;

    // a slot drawn once, so its next frames test the cache
    ShadowSlotState warm(const ShadowSlotInput& input){
        ShadowSlotState state;
        const ShadowSlotDecision first = ShadowCache::evaluate(state, input, none, 1);
        EXPECT_EQ(first.path, ShadowSlotPath::RENDER_STATIC);
        EXPECT_EQ(first.cause, ShadowDirtyCause::FIRST);
        return state;
    }

}

TEST(ShadowCache, UnchangedKeySkips){
    const ShadowSlotInput input = slotInput();
    ShadowSlotState state = warm(input);
    const ShadowSlotDecision decision = ShadowCache::evaluate(state, input, none, 2);
    EXPECT_EQ(decision.path, ShadowSlotPath::SKIP);
    EXPECT_EQ(decision.cause, ShadowDirtyCause::NONE);
    EXPECT_FALSE(decision.drawStatic);
    EXPECT_FALSE(decision.drawDynamic);
    EXPECT_EQ(state.lastRender, 1u);
}

TEST(ShadowCache, DynamicCasterRestoresItsRectOnly){
    const ShadowSlotInput input = slotInput();
    ShadowSlotState state = warm(input);
    const std::vector<ShadowCasterChange> changes = {moved(box(-5, 0, 0), box(-4, 0, 0), false)};
    const ShadowSlotDecision decision = ShadowCache::evaluate(state, input, changes, 2);
    EXPECT_EQ(decision.path, ShadowSlotPath::RESTORE_DYNAMIC);
    EXPECT_EQ(decision.cause, ShadowDirtyCause::DYNAMIC_CASTER);
    EXPECT_TRUE(decision.copyStatic);
    EXPECT_TRUE(decision.drawDynamic);
    EXPECT_FALSE(decision.drawStatic);
    // old and new: x in [-5.5, -3.5] of [-10, 10] -> texels [230, 333] of 1024, a texel of margin
    EXPECT_LE(decision.dirtyRect.x, 230);
    EXPECT_GE(decision.dirtyRect.x + decision.dirtyRect.width, 333);
    EXPECT_LT(decision.dirtyRect.width, 140);
    EXPECT_LT(decision.dirtyRect.height, 80);
    EXPECT_TRUE(state.staticValid);
}

TEST(ShadowCache, StaticCasterRendersTheStaticLayer){
    const ShadowSlotInput input = slotInput();
    ShadowSlotState state = warm(input);
    const std::vector<ShadowCasterChange> changes = {moved(box(2, 2, 0), box(3, 2, 0), true)};
    const ShadowSlotDecision decision = ShadowCache::evaluate(state, input, changes, 2);
    EXPECT_EQ(decision.path, ShadowSlotPath::RENDER_STATIC);
    EXPECT_EQ(decision.cause, ShadowDirtyCause::STATIC_CASTER);
    EXPECT_TRUE(decision.drawStatic);
    EXPECT_TRUE(decision.drawDynamic);
    EXPECT_TRUE(decision.copyStatic);
}

TEST(ShadowCache, WithoutStaticLayerAChangeRedrawsInFull){
    ShadowSlotInput input = slotInput();
    input.hasStaticLayer = false;
    ShadowSlotState state;
    EXPECT_EQ(ShadowCache::evaluate(state, input, none, 1).path, ShadowSlotPath::FULL);
    const std::vector<ShadowCasterChange> changes = {moved(box(-5, 0, 0), box(-4, 0, 0), false)};
    const ShadowSlotDecision decision = ShadowCache::evaluate(state, input, changes, 2);
    EXPECT_EQ(decision.path, ShadowSlotPath::FULL);
    EXPECT_EQ(decision.cause, ShadowDirtyCause::DYNAMIC_CASTER);
    EXPECT_TRUE(decision.clearSlot);
}

TEST(ShadowCache, ChangesOutsideTheSlotSkip){
    const ShadowSlotInput input = slotInput();
    ShadowSlotState state = warm(input);
    // far outside the slot's box, and in a layer the slot does not draw
    const std::vector<ShadowCasterChange> changes = {
        moved(box(50, 0, 0), box(51, 0, 0), false),
        moved(box(0, 0, 0), box(1, 0, 0), true, 2u)};
    EXPECT_EQ(ShadowCache::evaluate(state, input, changes, 2).path, ShadowSlotPath::SKIP);
}

TEST(ShadowCache, MovingIntoTheSlotDirtiesIt){
    const ShadowSlotInput input = slotInput();
    ShadowSlotState state = warm(input);
    // from outside (old) to inside (new), and the other way: both dirty it
    std::vector<ShadowCasterChange> changes = {moved(box(50, 0, 0), box(5, 0, 0), false)};
    EXPECT_EQ(ShadowCache::evaluate(state, input, changes, 2).path, ShadowSlotPath::RESTORE_DYNAMIC);
    changes = {moved(box(5, 0, 0), box(50, 0, 0), false)};
    EXPECT_EQ(ShadowCache::evaluate(state, input, changes, 3).path, ShadowSlotPath::RESTORE_DYNAMIC);
    // removed (no new bounds) and added (no old bounds)
    changes = {moved(box(5, 0, 0), ShadowBounds(), true)};
    EXPECT_EQ(ShadowCache::evaluate(state, input, changes, 4).cause, ShadowDirtyCause::STATIC_CASTER);
    changes = {moved(ShadowBounds(), box(5, 0, 0), false)};
    EXPECT_EQ(ShadowCache::evaluate(state, input, changes, 5).cause, ShadowDirtyCause::DYNAMIC_CASTER);
}

TEST(ShadowCache, SetupChangesGiveTheirCause){
    const ShadowSlotInput base = slotInput();
    struct Case { const char* name; void (*edit)(ShadowSlotInput&); ShadowDirtyCause cause; };
    const Case cases[] = {
        {"light setting", [](ShadowSlotInput& i){ i.key.lightRevision++; }, ShadowDirtyCause::LIGHT},
        {"culling mask", [](ShadowSlotInput& i){ i.key.casterMask = 3u; }, ShadowDirtyCause::LIGHT},
        {"refit", [](ShadowSlotInput& i){ i.key.viewProjection = slotCamera(0.25f); }, ShadowDirtyCause::FIT},
        {"atlas recreated", [](ShadowSlotInput& i){ i.key.atlasGeneration++; }, ShadowDirtyCause::ATLAS},
        {"rect moved", [](ShadowSlotInput& i){ i.key.rect.x = 1024; }, ShadowDirtyCause::ATLAS},
        {"resolution", [](ShadowSlotInput& i){ i.key.rect.width = 512; i.key.rect.height = 512; }, ShadowDirtyCause::ATLAS},
        {"static cache lost", [](ShadowSlotInput& i){ i.staticGeneration++; }, ShadowDirtyCause::ATLAS},
        {"request", [](ShadowSlotInput& i){ i.requested = true; }, ShadowDirtyCause::FORCED},
        {"force", [](ShadowSlotInput& i){ i.forceRerender = true; }, ShadowDirtyCause::FORCED},
    };
    for (const Case& c : cases){
        ShadowSlotState state = warm(base);
        ShadowSlotInput input = base;
        c.edit(input);
        const ShadowSlotDecision decision = ShadowCache::evaluate(state, input, none, 2);
        EXPECT_EQ(decision.cause, c.cause) << c.name;
        EXPECT_EQ(decision.path, ShadowSlotPath::RENDER_STATIC) << c.name;
        // and the next frame skips again
        input.requested = false;
        input.forceRerender = false;
        EXPECT_EQ(ShadowCache::evaluate(state, input, none, 3).path, ShadowSlotPath::SKIP) << c.name;
    }
}

TEST(ShadowCache, CasterLayerChangesAreTheCastersCause){
    // a caster moving to a layer no other caster is on changes the slot's caster mask with the
    // light unchanged: the redraw is the caster's (static here), not the light's
    const ShadowSlotInput base = slotInput();
    ShadowSlotState state = warm(base);
    ShadowSlotInput input = base;
    input.key.casterMask = 1u | 8u;
    const std::vector<ShadowCasterChange> changes = {moved(box(0, 0, 0), box(0, 0, 0), true, 1u | 8u)};
    const ShadowSlotDecision decision = ShadowCache::evaluate(state, input, changes, 2);
    EXPECT_EQ(decision.cause, ShadowDirtyCause::STATIC_CASTER);
    EXPECT_EQ(decision.path, ShadowSlotPath::RENDER_STATIC);
}

TEST(ShadowCache, ReceiverOnlyEditsSkip){
    // biases, strength, filter, fade and the tier's filter are no part of the key: the same
    // key, whatever they are, skips
    const ShadowSlotInput input = slotInput();
    ShadowSlotState state = warm(input);
    ShadowSlotInput same = input;
    EXPECT_EQ(ShadowCache::evaluate(state, same, none, 2).path, ShadowSlotPath::SKIP);
}

TEST(ShadowCache, CameraCastersAndEveryFrameNeverCache){
    ShadowSlotInput input = slotInput();
    input.key.cameraCasters = true;
    ShadowSlotState state;
    ShadowSlotDecision decision = ShadowCache::evaluate(state, input, none, 1);
    EXPECT_EQ(decision.path, ShadowSlotPath::UNCACHED);
    decision = ShadowCache::evaluate(state, input, none, 2);
    EXPECT_EQ(decision.path, ShadowSlotPath::UNCACHED);
    EXPECT_EQ(decision.cause, ShadowDirtyCause::CAMERA_CASTER);

    input = slotInput();
    input.mode = ShadowUpdateMode::EVERY_FRAME;
    state = ShadowSlotState();
    ShadowCache::evaluate(state, input, none, 1);
    decision = ShadowCache::evaluate(state, input, none, 2);
    EXPECT_EQ(decision.path, ShadowSlotPath::UNCACHED);
    EXPECT_EQ(decision.cause, ShadowDirtyCause::FORCED);
}

TEST(ShadowCache, OnDemandIgnoresCastersUntilRequested){
    ShadowSlotInput input = slotInput();
    input.mode = ShadowUpdateMode::ON_DEMAND;
    ShadowSlotState state;
    EXPECT_EQ(ShadowCache::evaluate(state, input, none, 1).cause, ShadowDirtyCause::FIRST);
    const std::vector<ShadowCasterChange> changes = {moved(box(0, 0, 0), box(1, 0, 0), true)};
    EXPECT_EQ(ShadowCache::evaluate(state, input, changes, 2).path, ShadowSlotPath::SKIP);
    // a light change still redraws (On Demand: on enable, a light or resolution change)
    input.key.lightRevision++;
    EXPECT_EQ(ShadowCache::evaluate(state, input, changes, 3).cause, ShadowDirtyCause::LIGHT);
    input.requested = true;
    const ShadowSlotDecision decision = ShadowCache::evaluate(state, input, none, 4);
    EXPECT_EQ(decision.cause, ShadowDirtyCause::FORCED);
    // no static layer outside Auto
    EXPECT_EQ(decision.path, ShadowSlotPath::FULL);
}

TEST(ShadowCache, OtherCamerasSkipOrRedrawOnly){
    ShadowSlotInput input = slotInput();
    input.caching = false;
    ShadowSlotState state;
    EXPECT_EQ(ShadowCache::evaluate(state, input, none, 1).path, ShadowSlotPath::FULL);
    EXPECT_EQ(ShadowCache::evaluate(state, input, none, 2).path, ShadowSlotPath::SKIP);
    const std::vector<ShadowCasterChange> changes = {moved(box(0, 0, 0), box(1, 0, 0), false)};
    const ShadowSlotDecision decision = ShadowCache::evaluate(state, input, changes, 3);
    EXPECT_EQ(decision.path, ShadowSlotPath::FULL);
    EXPECT_EQ(decision.cause, ShadowDirtyCause::DYNAMIC_CASTER);
}

TEST(ShadowCache, FreezeHoldsEverything){
    ShadowSlotInput input = slotInput();
    ShadowSlotState state = warm(input);
    input.freezeCache = true;
    input.key.lightRevision++;
    EXPECT_EQ(ShadowCache::evaluate(state, input, none, 2).path, ShadowSlotPath::SKIP);
}

TEST(ShadowCache, IntersectsAndProjectsPerspectiveCameras){
    // a spot-like camera at the origin looking down -Z
    const Matrix4 spot = Matrix4::perspectiveMatrix(1.0f, 1.0f, 0.1f, 20.0f) *
                         Matrix4::lookAtMatrix(Vector3(0, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0));
    EXPECT_TRUE(ShadowCache::intersects(spot, box(0, 0, -5)));
    EXPECT_FALSE(ShadowCache::intersects(spot, box(0, 0, 5)));
    EXPECT_FALSE(ShadowCache::intersects(spot, box(0, 0, -30)));
    EXPECT_FALSE(ShadowCache::intersects(spot, box(20, 0, -5)));
    const ShadowRect slot{256, 0, 256, 256};
    // a box across the camera plane can cover any of the slot
    EXPECT_EQ(ShadowCache::projectedRect(spot, slot, box(0, 0, 0)), slot);
    const ShadowRect centred = ShadowCache::projectedRect(spot, slot, box(0, 0, -5));
    EXPECT_FALSE(centred.empty());
    EXPECT_GE(centred.x, slot.x);
    EXPECT_LE(centred.x + centred.width, slot.x + slot.width);
    EXPECT_LT(centred.width, slot.width);
}

TEST(ShadowCasterRegistry, ReportsAddsMovesTogglesAndRemovals){
    ShadowCasterRegistry registry;
    ShadowCasterState a;
    a.bounds = box(0, 0, 0);
    a.layerBit = 1u;
    ShadowCasterState b = a;
    b.bounds = box(5, 0, 0);
    b.isStatic = true;

    registry.beginUpdate();
    registry.update(10, a);
    registry.update(11, b);
    registry.endUpdate();
    ASSERT_EQ(registry.getChanges().size(), 2u);
    EXPECT_FALSE(registry.getChanges()[0].oldBounds.valid);
    EXPECT_EQ(registry.size(), 2u);
    EXPECT_EQ(registry.getStaticLayers(), 1u);
    EXPECT_TRUE(registry.getBounds().valid);

    // nothing changed: nothing reported
    registry.beginUpdate();
    EXPECT_FALSE(registry.update(10, a));
    EXPECT_FALSE(registry.update(11, b));
    registry.endUpdate();
    EXPECT_TRUE(registry.getChanges().empty());

    // a move reports old and new bounds; a revision bump (a mesh reload) too
    registry.beginUpdate();
    ShadowCasterState movedA = a;
    movedA.bounds = box(1, 0, 0);
    EXPECT_TRUE(registry.update(10, movedA));
    ShadowCasterState reloadedB = b;
    reloadedB.revision++;
    EXPECT_TRUE(registry.update(11, reloadedB));
    registry.endUpdate();
    ASSERT_EQ(registry.getChanges().size(), 2u);
    EXPECT_EQ(registry.getChanges()[0].oldBounds, a.bounds);
    EXPECT_EQ(registry.getChanges()[0].newBounds, movedA.bounds);
    EXPECT_TRUE(registry.getChanges()[0].dynamicLayer);
    EXPECT_TRUE(registry.getChanges()[1].staticLayer);

    // the static flag toggled touches both layers; an animated caster changes every update
    registry.beginUpdate();
    ShadowCasterState toggled = movedA;
    toggled.isStatic = true;
    registry.update(10, toggled);
    ShadowCasterState animated = reloadedB;
    animated.animated = true;
    EXPECT_TRUE(registry.update(11, animated));
    registry.endUpdate();
    ASSERT_EQ(registry.getChanges().size(), 2u);
    EXPECT_TRUE(registry.getChanges()[0].staticLayer && registry.getChanges()[0].dynamicLayer);

    // not reported: removed
    registry.beginUpdate();
    registry.update(10, toggled);
    registry.endUpdate();
    ASSERT_EQ(registry.getChanges().size(), 1u);
    EXPECT_EQ(registry.getChanges()[0].oldBounds, animated.bounds);
    EXPECT_FALSE(registry.getChanges()[0].newBounds.valid);
    EXPECT_EQ(registry.size(), 1u);
}
