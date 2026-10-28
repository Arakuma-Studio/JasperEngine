// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S26 R13, R14: what a click does with the hits under it (ScenePicker::decideClick): the root
// first, then the GameObject under the pointer once the selection is inside the instance,
// same-spot clicks walking back through what overlaps, Shift and Ctrl on the exact hit.
// Pure: the hits, the selection and the click before are given.

#include "picking/ScenePicker.h"

#include <gtest/gtest.h>

#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr uint32_t SCENE = 1;

    ScenePickEntry entry(Entity entity, PickKind kind = PickKind::MESH){
        ScenePickEntry e;
        e.sceneId = SCENE;
        e.entity = entity;
        e.hit.entity = entity;
        e.hit.kind = kind;
        return e;
    }

    // a prefab instance 10 holding 11 (hit nearest) over a wall 20 behind it:
    // hits [11, 20], candidates [10 (root), 11, 20]
    ScenePickResult prefabOverWall(){
        ScenePickResult result;
        result.hits = {entry(11), entry(20)};
        result.candidates = {entry(10), entry(11), entry(20)};
        return result;
    }

    struct Selection {
        std::set<Entity> selected;
        Entity active = NULL_ENTITY;
    };

    ScenePickContext context(const Selection& selection, float x = 100.0f, float y = 100.0f){
        ScenePickContext c;
        c.x = x;
        c.y = y;
        if (selection.active != NULL_ENTITY){
            c.primary = entry(selection.active);
        }
        c.isSelected = [selection](const ScenePickEntry& e){ return selection.selected.count(e.entity) > 0; };
        // 11 is inside 10
        c.isInside = [](uint32_t, Entity entity, Entity ancestor){ return entity == ancestor || (entity == 11 && ancestor == 10); };
        return c;
    }

}

TEST(ScenePicker, FirstClickSelectsTheRoot){
    const ScenePickClick click = ScenePicker::decideClick(prefabOverWall(), context({}));
    EXPECT_EQ(click.action, ScenePickClick::Action::SELECT);
    EXPECT_EQ(click.target.entity, 10u);
    EXPECT_EQ(click.cycleIndex, 0);
    EXPECT_EQ(click.cycleCount, 3);
}

TEST(ScenePicker, SecondClickDrillsToTheHit){
    // the root is selected, the click elsewhere on the instance: the GameObject under the pointer
    Selection selection{{10}, 10};
    ScenePickContext c = context(selection, 300.0f, 300.0f);
    c.lastX = 100.0f;
    c.lastY = 100.0f;
    const ScenePickClick click = ScenePicker::decideClick(prefabOverWall(), c);
    EXPECT_EQ(click.action, ScenePickClick::Action::SELECT);
    EXPECT_EQ(click.target.entity, 11u);
}

TEST(ScenePicker, SameSpotCyclesAndWraps){
    // within 4 px of the click before, with the selection among the candidates: the next one
    Selection selection{{10}, 10};
    ScenePickContext c = context(selection, 101.0f, 99.0f);
    c.lastX = 100.0f;
    c.lastY = 100.0f;
    ScenePickClick click = ScenePicker::decideClick(prefabOverWall(), c);
    EXPECT_EQ(click.target.entity, 11u);
    EXPECT_EQ(click.cycleIndex, 1);

    selection = {{11}, 11};
    c = context(selection, 100.0f, 100.0f);
    c.lastX = 100.0f;
    c.lastY = 100.0f;
    click = ScenePicker::decideClick(prefabOverWall(), c);
    EXPECT_EQ(click.target.entity, 20u) << "the next one behind";

    selection = {{20}, 20};
    c = context(selection, 100.0f, 100.0f);
    c.lastX = 100.0f;
    c.lastY = 100.0f;
    click = ScenePicker::decideClick(prefabOverWall(), c);
    EXPECT_EQ(click.target.entity, 10u) << "wraps to the first";
    EXPECT_EQ(click.cycleIndex, 0);

    // 5 px away is a new click
    selection = {{20}, 20};
    c = context(selection, 105.0f, 100.0f);
    c.lastX = 100.0f;
    c.lastY = 100.0f;
    click = ScenePicker::decideClick(prefabOverWall(), c);
    EXPECT_EQ(click.target.entity, 10u);
    EXPECT_EQ(click.cycleIndex, 0);
}

TEST(ScenePicker, WheelStepIsConfirmed){
    // Ctrl+wheel selected the wall: the click at that spot keeps it
    Selection selection{{20}, 20};
    ScenePickContext c = context(selection);
    c.lastX = 100.0f;
    c.lastY = 100.0f;
    c.depthAnchor = true;
    const ScenePickClick click = ScenePicker::decideClick(prefabOverWall(), c);
    EXPECT_EQ(click.target.entity, 20u);
    EXPECT_EQ(click.cycleIndex, 2);
}

TEST(ScenePicker, ExactPickSelectsTheHit){
    ScenePickResult result;
    result.hits = {entry(11), entry(20)};
    result.candidates = {entry(11), entry(20)};   // no roots
    ScenePickContext c = context({});
    c.exactPick = true;
    const ScenePickClick click = ScenePicker::decideClick(result, c);
    EXPECT_EQ(click.target.entity, 11u);
}

TEST(ScenePicker, ShiftAndCtrlTargetTheExactHitWhenSelected){
    // nothing of the instance selected: Shift adds the root
    ScenePickContext c = context({{30}, 30});
    c.shift = true;
    ScenePickClick click = ScenePicker::decideClick(prefabOverWall(), c);
    EXPECT_EQ(click.action, ScenePickClick::Action::ADD);
    EXPECT_EQ(click.target.entity, 10u);

    // the exact hit is selected: Ctrl toggles that one, not the root
    c = context({{11, 30}, 11});
    c.ctrl = true;
    click = ScenePicker::decideClick(prefabOverWall(), c);
    EXPECT_EQ(click.action, ScenePickClick::Action::TOGGLE);
    EXPECT_EQ(click.target.entity, 11u);
}

TEST(ScenePicker, MissClearsUnlessModified){
    const ScenePickResult nothing;
    EXPECT_EQ(ScenePicker::decideClick(nothing, context({{10}, 10})).action, ScenePickClick::Action::CLEAR);
    ScenePickContext c = context({{10}, 10});
    c.shift = true;
    EXPECT_EQ(ScenePicker::decideClick(nothing, c).action, ScenePickClick::Action::NONE);
}

TEST(ScenePicker, IconsComeFirstAndDoNotDrill){
    ScenePickResult result = prefabOverWall();
    result.hits.insert(result.hits.begin(), entry(40, PickKind::ICON));
    result.candidates.insert(result.candidates.begin(), entry(40, PickKind::ICON));
    result.iconCount = 1;
    const ScenePickClick click = ScenePicker::decideClick(result, context({{10}, 10}, 300.0f, 300.0f));
    EXPECT_EQ(click.target.entity, 40u) << "the light icon drawn over the mesh";
}
