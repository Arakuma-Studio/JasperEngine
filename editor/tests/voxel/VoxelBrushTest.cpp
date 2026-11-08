// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R60 (AC45): the brush's grammar, its colours, its stamps and its keys, driven the way the Scene
// view drives it: rays and the modifiers read at the press.

#include "VoxelTestScene.h"

#include "shortcuts/ShortcutDispatcher.h"
#include "shortcuts/ShortcutTestSupport.h"
#include "voxel/VoxelBrushTool.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::voxeltest;

namespace {

    // straight down onto the ground of smallDesc at (x, z)
    Ray rayDown(float x, float z) {
        return Ray(Vector3(x, 60.0f, z), Vector3(0.0f, -60.0f, 0.0f));
    }

    class VoxelBrush : public VoxelSceneTest {
    protected:
        VoxelBrushTool tool;

        void SetUp() override {
            VoxelSceneTest::SetUp();
            addVolume();
            CommandHandle::get(sceneProject->id)->clear();
            tool.setTarget(project.get(), sceneProject->id, owner);
            tool.settings().radius = 3.0f;
            tool.settings().blend = 0.0f;
        }

        void TearDown() override {
            tool.reset();
            VoxelSceneTest::TearDown();
        }

        Scene* scene() const { return sceneProject->scene; }
        double valueAt(double x, double y, double z) { return voxel::valueAt(*asset->store, voxel::Vec3d{x, y, z}); }
    };

    void expectColour(const VoxelDecalColour& colour, float r, float g, float b) {
        EXPECT_FLOAT_EQ(colour.r, r);
        EXPECT_FLOAT_EQ(colour.g, g);
        EXPECT_FLOAT_EQ(colour.b, b);
    }

}

TEST_F(VoxelBrush, CtrlInvertsAndDecalColour) {
    // the table: Ctrl swaps Dig and Add, Shift smooths and wins, Pick stays Pick
    EXPECT_EQ(voxelEffectiveTool(VoxelTool::Dig, false, false), VoxelTool::Dig);
    EXPECT_EQ(voxelEffectiveTool(VoxelTool::Dig, true, false), VoxelTool::Add);
    EXPECT_EQ(voxelEffectiveTool(VoxelTool::Add, true, false), VoxelTool::Dig);
    EXPECT_EQ(voxelEffectiveTool(VoxelTool::Dig, false, true), VoxelTool::Smooth);
    EXPECT_EQ(voxelEffectiveTool(VoxelTool::Add, true, true), VoxelTool::Smooth);
    EXPECT_EQ(voxelEffectiveTool(VoxelTool::Paint, true, false), VoxelTool::Paint);
    EXPECT_EQ(voxelEffectiveTool(VoxelTool::Paint, false, true), VoxelTool::Smooth);
    EXPECT_EQ(voxelEffectiveTool(VoxelTool::Pick, true, true), VoxelTool::Pick);

    // T1's Table 4 for the same meanings
    expectColour(voxelDecalColour(VoxelTool::Add), 1.0f, 1.0f, 1.0f);
    expectColour(voxelDecalColour(VoxelTool::Dig), 0.2f, 0.2f, 0.2f);
    expectColour(voxelDecalColour(VoxelTool::Smooth), 0.5f, 0.0f, 0.2f);
    expectColour(voxelDecalColour(VoxelTool::Paint), 0.0f, 0.5f, 0.0f);
    expectColour(voxelDecalColour(VoxelTool::Pick), 1.0f, 1.0f, 1.0f);

    // the colour follows a modifier as soon as it is pressed, with no pointer move
    tool.arm(VoxelTool::Dig);
    tool.tick(VoxelBrushModifiers{}, false);
    tool.updateCursor(scene(), rayDown(32.0f, 32.0f));
    ASSERT_TRUE(tool.decalVisible());
    EXPECT_FALSE(tool.decal().ring.empty());
    EXPECT_FALSE(tool.decal().wire.empty());
    expectColour(tool.decal().colour, 0.2f, 0.2f, 0.2f);
    tool.tick(VoxelBrushModifiers{true, false}, false);
    expectColour(tool.decal().colour, 1.0f, 1.0f, 1.0f);
    tool.tick(VoxelBrushModifiers{false, true}, false);
    expectColour(tool.decal().colour, 0.5f, 0.0f, 0.2f);
    tool.tick(VoxelBrushModifiers{}, false);
    expectColour(tool.decal().colour, 0.2f, 0.2f, 0.2f);

    // the view reports the pointer every frame: the same ray builds nothing and does not count as moving
    const uint64_t built = tool.decalRevision();
    for (int frame = 0; frame < 3; frame++) tool.updateCursor(scene(), rayDown(32.0f, 32.0f));
    EXPECT_EQ(tool.decalRevision(), built);
    tool.updateCursor(scene(), rayDown(33.0f, 32.0f));
    EXPECT_NE(tool.decalRevision(), built);

    // fades after the pointer rests, and hides at once while the camera moves
    EXPECT_FLOAT_EQ(voxelDecalAlpha(0.0, false), 1.0f);
    EXPECT_FLOAT_EQ(voxelDecalAlpha(0.5, false), 1.0f);
    EXPECT_LT(voxelDecalAlpha(0.6, false), 1.0f);
    EXPECT_FLOAT_EQ(voxelDecalAlpha(5.0, false), 0.0f);
    EXPECT_FLOAT_EQ(voxelDecalAlpha(0.0, true), 0.0f);

    // A stroke with Ctrl adds where a Dig would have dug, and a modifier let go midway changes nothing:
    // they are read at the press
    const double airAbove = valueAt(32.0, 34.0, 32.0);
    ASSERT_GT(airAbove, 0.0);
    ASSERT_TRUE(tool.beginStroke(scene(), rayDown(32.0f, 32.0f), VoxelBrushModifiers{true, false}));
    EXPECT_EQ(tool.strokeTool(), VoxelTool::Add);
    tool.tick(VoxelBrushModifiers{}, false);
    EXPECT_EQ(tool.strokeTool(), VoxelTool::Add);
    expectColour(tool.decal().colour, 1.0f, 1.0f, 1.0f);
    tool.paintStroke(scene(), rayDown(36.0f, 32.0f));
    tool.endStroke();
    EXPECT_LT(valueAt(36.0, 33.0, 32.0), 0.0) << "Ctrl+Dig added";

    // and with nothing held it digs
    const double solidBelow = valueAt(16.0, 30.0, 16.0);
    ASSERT_LT(solidBelow, 0.0);
    ASSERT_TRUE(tool.beginStroke(scene(), rayDown(16.0f, 16.0f), VoxelBrushModifiers{}));
    EXPECT_EQ(tool.strokeTool(), VoxelTool::Dig);
    tool.endStroke();
    EXPECT_GT(valueAt(16.0, 30.0, 16.0), 0.0) << "a plain Dig dug";
}

TEST_F(VoxelBrush, AltClickPicksAltDragOrbits) {
    // a patch of slot 3 under the pointer
    VoxelVolumeComponent& component = scene()->getComponent<VoxelVolumeComponent>(owner);
    component.materialSlots.resize(4);
    voxel::EditOp paint;
    paint.kind = voxel::EditKind::Paint;
    paint.material = 3;
    paint.shape.center = voxel::Vec3d{32.0, 32.0, 32.0};
    paint.shape.radius = 6.0;
    voxel::EditOptions options;
    ASSERT_TRUE(system->edit(owner, std::span<const voxel::EditOp>(&paint, 1), options).ok);

    tool.arm(VoxelTool::Dig);
    tool.settings().material = 0;
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    const size_t steps = history->entries().size();
    const uint64_t before = hash();
    const float threshold = 6.0f;

    // past the threshold: an orbit, nothing picked
    tool.altPress(100.0f, 100.0f);
    EXPECT_FALSE(tool.altMove(102.0f, 100.0f, threshold));
    EXPECT_TRUE(tool.altHolding()) << "the camera waits inside the threshold";
    EXPECT_TRUE(tool.altMove(120.0f, 100.0f, threshold));
    EXPECT_FALSE(tool.altHolding()) << "past it the camera moves";
    // back near the start it is still the orbit it became
    EXPECT_TRUE(tool.altMove(100.0f, 100.0f, threshold));
    EXPECT_FALSE(tool.altRelease(scene(), rayDown(32.0f, 32.0f)));
    EXPECT_EQ(tool.settings().material, 0);

    // released inside the threshold: a pick, and no stroke
    tool.altPress(100.0f, 100.0f);
    EXPECT_FALSE(tool.altMove(103.0f, 101.0f, threshold));
    EXPECT_TRUE(tool.altRelease(scene(), rayDown(32.0f, 32.0f)));
    EXPECT_EQ(tool.settings().material, 3);
    EXPECT_FALSE(tool.isStrokeActive());
    EXPECT_EQ(hash(), before);
    EXPECT_EQ(history->entries().size(), steps);

    // Pick as a tool: the next click takes it and writes nothing, then the tool before it comes back
    tool.settings().material = 0;
    tool.arm(VoxelTool::Pick);
    EXPECT_TRUE(tool.beginStroke(scene(), rayDown(32.0f, 32.0f), VoxelBrushModifiers{true, true}));
    EXPECT_EQ(tool.settings().material, 3);
    EXPECT_FALSE(tool.isStrokeActive());
    EXPECT_EQ(tool.settings().tool, VoxelTool::Dig);
    EXPECT_EQ(hash(), before);
    EXPECT_EQ(history->entries().size(), steps);
}

TEST_F(VoxelBrush, StrokeSpacing) {
    EXPECT_DOUBLE_EQ(voxelStrokeSpacing(4.0, 1.0), 1.0);
    EXPECT_DOUBLE_EQ(voxelStrokeSpacing(0.5, 1.0), 0.5);

    // the stamps are multiples of the spacing along the path, however the path is cut into frames
    const auto stampsOf = [](const std::vector<double>& xs, double spacing) {
        VoxelStrokeStamper stamper(spacing);
        std::vector<voxel::Vec3d> stamps;
        stamper.start(voxel::Vec3d{xs.front(), 0.0, 0.0}, stamps);
        for (size_t i = 1; i < xs.size(); i++) stamper.moveTo(voxel::Vec3d{xs[i], 0.0, 0.0}, stamps);
        return stamps;
    };
    const std::vector<voxel::Vec3d> whole = stampsOf({0.0, 10.0}, 1.0);
    const std::vector<voxel::Vec3d> cut = stampsOf({0.0, 0.3, 0.31, 2.7, 2.7, 6.9, 10.0}, 1.0);
    ASSERT_EQ(whole.size(), 11u);
    ASSERT_EQ(cut.size(), whole.size());
    for (size_t i = 0; i < whole.size(); i++) {
        EXPECT_NEAR(whole[i].x, static_cast<double>(i), 1e-9);
        EXPECT_NEAR(cut[i].x, static_cast<double>(i), 1e-9);
    }
    // holding still stamps nothing more
    EXPECT_EQ(stampsOf({5.0, 5.0, 5.0}, 1.0).size(), 1u);

    // a ray leaving the surface does not streak across the gap
    VoxelStrokeStamper stamper(1.0);
    std::vector<voxel::Vec3d> stamps;
    stamper.start(voxel::Vec3d{0.0, 0.0, 0.0}, stamps);
    stamper.lift();
    stamper.moveTo(voxel::Vec3d{50.0, 0.0, 0.0}, stamps);
    EXPECT_EQ(stamps.size(), 2u);

    // through the tool: holding still changes nothing, and a stroke, however long, is one undo step
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    const size_t steps = history->entries().size();
    const uint64_t start = hash();
    tool.arm(VoxelTool::Dig);
    ASSERT_TRUE(tool.beginStroke(scene(), rayDown(20.0f, 32.0f), VoxelBrushModifiers{}));
    const uint64_t afterFirst = hash();
    EXPECT_NE(afterFirst, start);
    // the ray hits the surface as it was at the press: frame after frame on one spot digs no deeper
    for (int frame = 0; frame < 5; frame++) tool.paintStroke(scene(), rayDown(20.0f, 32.0f));
    const uint64_t afterSecond = hash();
    EXPECT_EQ(afterSecond, afterFirst) << "holding still adds nothing";
    tool.paintStroke(scene(), rayDown(24.0f, 32.0f));
    tool.paintStroke(scene(), rayDown(28.0f, 32.0f));
    tool.paintStroke(scene(), rayDown(40.0f, 32.0f));
    tool.endStroke();
    EXPECT_NE(hash(), afterSecond);
    EXPECT_EQ(history->entries().size(), steps + 1);
    EXPECT_EQ(history->top()->name(), "Dig Sphere (Cave)");
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(hash(), start);
    ASSERT_TRUE(history->redo());
    EXPECT_NE(hash(), start);

    // the next stroke is a step of its own
    ASSERT_TRUE(tool.beginStroke(scene(), rayDown(20.0f, 10.0f), VoxelBrushModifiers{}));
    tool.endStroke();
    EXPECT_EQ(history->entries().size(), steps + 2);

    // Esc: the stroke goes as if it never was
    const uint64_t beforeCancelled = hash();
    ASSERT_TRUE(tool.beginStroke(scene(), rayDown(20.0f, 50.0f), VoxelBrushModifiers{}));
    EXPECT_NE(hash(), beforeCancelled);
    tool.cancelStroke();
    EXPECT_EQ(hash(), beforeCancelled);
    EXPECT_EQ(history->entries().size(), steps + 2);
}

TEST_F(VoxelBrush, ScriptedStrokeSame) {
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    const uint64_t start = hash();

    // the path a mouse stroke's rays land on: the surface as it is at the press
    const Ray rays[] = {rayDown(16.0f, 16.0f), rayDown(19.0f, 18.0f), rayDown(23.0f, 18.5f), rayDown(31.0f, 24.0f), rayDown(32.0f, 40.0f)};
    std::vector<voxel::Vec3d> path;
    for (const Ray& ray : rays) {
        voxel::RaycastHit hit;
        ASSERT_TRUE(VoxelBrushTool::hitVolume(*system, owner, ray, false, hit));
        path.push_back(hit.point);
    }

    tool.arm(VoxelTool::Dig);
    tool.settings().radius = 2.0f;
    ASSERT_TRUE(tool.beginStroke(scene(), rays[0], VoxelBrushModifiers{}));
    for (size_t i = 1; i < std::size(rays); i++) ASSERT_TRUE(tool.paintStroke(scene(), rays[i]));
    tool.endStroke();
    const uint64_t byMouse = hash();
    EXPECT_NE(byMouse, start);
    ASSERT_TRUE(history->undo());
    ASSERT_EQ(hash(), start);

    // the same path with no mouse: the same bytes, one step
    const size_t steps = history->entries().size();
    std::string why;
    ASSERT_TRUE(VoxelBrushTool::scriptedStroke(project.get(), sceneProject->id, owner, tool.settings(), VoxelTool::Dig, path, why)) << why;
    EXPECT_EQ(hash(), byMouse);
    EXPECT_GE(history->entries().size(), steps);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(hash(), start);
}

TEST_F(VoxelBrush, ShortcutContextRebindable) {
    using namespace jasperengine::editor::test;
    EXPECT_STREQ(shortcutContextName(ShortcutContext::VoxelBrush), "Voxel Brush");
    EXPECT_STREQ(shortcutContextKey(ShortcutContext::VoxelBrush), "voxel_brush");
    EXPECT_EQ(parseShortcutContext("voxel_brush"), ShortcutContext::VoxelBrush);
    EXPECT_EQ(parseShortcutContext("Voxel Brush"), ShortcutContext::VoxelBrush);

    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    ShortcutDispatcher dispatcher(catalog);
    struct Row {
        const char* id;
        const char* chord;
    };
    const Row rows[] = {{"voxel.tool.dig", "D"}, {"voxel.tool.add", "A"}, {"voxel.tool.smooth", "S"}, {"voxel.tool.paint", "P"},
                        {"voxel.tool.pick", "K"}, {"voxel.brushSmaller", "["}, {"voxel.brushLarger", "]"}};
    ShortcutState armed;
    armed.panels = {ShortcutContext::SceneView, ShortcutContext::VoxelBrush};
    ShortcutState unarmed;
    unarmed.panels = {ShortcutContext::SceneView};
    for (const Row& row : rows) {
        const CommandDef* def = catalog.find(row.id);
        ASSERT_NE(def, nullptr) << row.id;
        EXPECT_EQ(def->context, ShortcutContext::VoxelBrush);
        const std::span<const Chord> chords = catalog.bindings(row.id);
        ASSERT_EQ(chords.size(), 1u);
        EXPECT_EQ(chords[0], chordOf(row.chord));
        const CommandDef* armedTarget = dispatcher.resolve(chordOf(row.chord), armed);
        ASSERT_NE(armedTarget, nullptr) << row.chord;
        EXPECT_EQ(armedTarget->id, row.id) << "the armed brush outranks the Scene view";
        const CommandDef* unarmedTarget = dispatcher.resolve(chordOf(row.chord), unarmed);
        if (unarmedTarget) {
            EXPECT_NE(unarmedTarget->id, row.id) << "a brush not armed leaves the key to the view";
        }
    }
    // it stays listed with the others, and the defaults still hold no conflict
    EXPECT_TRUE(catalog.resolveConflicts().empty());
    EXPECT_NE(catalog.markdownReference().find("Voxel Brush"), std::string::npos);

    // the legend says what is bound
    EXPECT_NE(voxelBrushLegend(catalog).find("D Dig"), std::string::npos);
    std::string reason;
    ASSERT_TRUE(catalog.setBindings("voxel.tool.dig", {chordOf("G")}, &reason)) << reason;
    EXPECT_EQ(dispatcher.resolve(chordOf("G"), armed)->id, "voxel.tool.dig");
    EXPECT_EQ(dispatcher.resolve(chordOf("D"), armed), nullptr);
    EXPECT_NE(voxelBrushLegend(catalog).find("G Dig"), std::string::npos);
    catalog.resetBindings("voxel.tool.dig");
    EXPECT_EQ(dispatcher.resolve(chordOf("D"), armed)->id, "voxel.tool.dig");

    // a press reaches the command once
    recording.ran.clear();
    const std::vector<std::string> fired = dispatcher.process(FakeKeys::press(chordOf("]")), armed);
    EXPECT_EQ(fired, std::vector<std::string>{"voxel.brushLarger"});
}
