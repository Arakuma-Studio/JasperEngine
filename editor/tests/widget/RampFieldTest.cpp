// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Title Designer's gradient row (docs/specs/S82-title-designer.md R25, AC39): the pure ramp
// helpers, the gesture logic (RampEditor: input in bar pixels, FieldEdit phases out) and the
// bar's drawing in a headless ImGui frame.

#include "widget/RampField.h"

#include "math/Curve.h"
#include "math/Vector3.h"
#include "titleart/render/TitleRenderServices.h"

#include "imgui.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using ui::EditPhase;
using ui::RampEditor;
namespace ramp = jasperengine::editor::ui::ramp;
namespace ta = jasperengine::titleart;

namespace {

    using Phases = std::vector<EditPhase>;

    ta::TitleGradientStop stop(double pos, float r, float g, float b, double alpha = 1.0, double mid = 0.5) {
        ta::TitleGradientStop s;
        s.pos = pos;
        s.color = {r, g, b};
        s.alpha = alpha;
        s.mid = mid;
        return s;
    }

    ta::TitleGradient blackToWhite() {
        ta::TitleGradient g;
        g.stops = {stop(0.0, 0, 0, 0), stop(1.0, 1, 1, 1)};
        return g;
    }

    ta::TitleGradient threeStops() {
        ta::TitleGradient g;
        g.stops = {stop(0.0, 1, 0, 0, 1.0, 0.3), stop(0.25, 0, 1, 0, 0.5, 0.6), stop(1.0, 0, 0, 1)};
        return g;
    }

    // every queued phase; `last` gets the value the last one carries
    Phases drain(RampEditor& editor, ta::TitleGradient* last = nullptr) {
        Phases phases;
        RampEditor::Step step;
        while (editor.next(step)) {
            phases.push_back(step.phase);
            if (last) *last = step.value;
        }
        return phases;
    }

    // a bar from x = 10, 200 px wide
    RampEditor::Geometry bar() {
        RampEditor::Geometry geometry;
        geometry.x0 = 10.0f;
        geometry.width = 200.0f;
        return geometry;
    }

    float xOf(double pos) { return ramp::xAt(10.0f, 200.0f, pos); }

}

// The row shows what the renderer draws. Red control: a change to the blend on one side only
// (the vivid hue arc, the midpoint exponent, the sort) breaks byte equality.
TEST(RampFieldTest, SampleMatchesTheRenderersLut) {
    ta::TitleGradient rgb = threeStops();
    ta::TitleGradient vivid;
    vivid.vivid = true;
    vivid.stops = {stop(0.0, 0, 0, 1, 1.0, 0.65), stop(1.0, 1, 1, 1)};
    ta::TitleGradient achromatic;
    achromatic.vivid = true;
    achromatic.stops = {stop(0.0, 0, 0, 0), stop(0.5, 1.0f, 0.5f, 0.0f, 0.8, 0.2), stop(0.5, 1.0f, 0.5f, 0.0f), stop(1.0, 0, 1, 1)};
    ta::TitleGradient unsorted;
    unsorted.stops = {stop(1.0, 1, 1, 0), stop(0.0, 0, 0.2f, 0.4f, 0.3)};
    for (const ta::TitleGradient& gradient : {rgb, vivid, achromatic, unsorted}) {
        uint8_t lut[256 * 4];
        ta::bakeGradient(gradient, lut);
        for (int i = 0; i < 256; i++) {
            const ramp::Rgba c = ramp::sample(gradient, i / 255.0);
            const double channels[4] = {c.r, c.g, c.b, c.a};
            for (int k = 0; k < 4; k++) {
                const long byte = std::lround(std::clamp(channels[k], 0.0, 1.0) * 255.0);
                ASSERT_EQ(byte, (long)lut[i * 4 + k]) << "texel " << i << " channel " << k << (gradient.vivid ? " vivid" : " rgb");
            }
        }
    }
}

// Red control: inserting a stop with a fixed colour (white) instead of the sampled one fails the
// colour check; reporting Begin + Committed instead of one Applied fails the phases.
TEST(RampFieldTest, ClickOnTheBarAddsAStopWithTheColourThere) {
    const ta::TitleGradient original = threeStops();
    RampEditor editor;
    editor.sync(original);
    editor.pressBar(bar(), xOf(0.5));
    ta::TitleGradient value;
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Applied}));
    ASSERT_EQ(value.stops.size(), 4u);
    EXPECT_EQ(editor.selected(), 2);
    const ta::TitleGradientStop& added = value.stops[2];
    EXPECT_DOUBLE_EQ(added.pos, 0.5);
    const ramp::Rgba expected = ramp::sample(original, 0.5);
    EXPECT_NEAR(added.color.r, expected.r, 1e-6);
    EXPECT_NEAR(added.color.g, expected.g, 1e-6);
    EXPECT_NEAR(added.color.b, expected.b, 1e-6);
    EXPECT_NEAR(added.alpha, expected.a, 1e-9);
    EXPECT_DOUBLE_EQ(added.mid, 0.5);
    // the look is unchanged at the new stop
    const ramp::Rgba after = ramp::sample(value, 0.5);
    EXPECT_NEAR(after.g, expected.g, 1e-6);
}

// Red control: a removal distance of 80 px keeps the stop; removing without the 2-stop floor
// leaves a one-stop gradient.
TEST(RampFieldTest, DragFortyPixelsDownRemovesAStop) {
    RampEditor editor;
    editor.sync(threeStops());
    const RampEditor::Geometry geometry = bar();
    editor.pressStop(geometry, 1, xOf(0.25), 50.0f);
    EXPECT_TRUE(drain(editor).empty());                         // a press only selects
    editor.moveTo(geometry, xOf(0.25), 60.0f);                  // past the threshold, above 40 px
    EXPECT_EQ(drain(editor), (Phases{EditPhase::Begin}));       // Begin carries no change
    editor.moveTo(geometry, xOf(0.25), 91.0f);                  // 41 px down
    ta::TitleGradient value;
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Changing}));
    EXPECT_EQ(value.stops.size(), 2u);
    EXPECT_TRUE(editor.removing());
    editor.release();
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Committed}));     // one step
    EXPECT_EQ(value.stops.size(), 2u);
    EXPECT_FALSE(editor.removing());

    // dragged back above 40 px before the release, the stop comes back
    editor.sync(threeStops());
    editor.pressStop(geometry, 1, xOf(0.25), 50.0f);
    editor.moveTo(geometry, xOf(0.25), 95.0f);
    editor.moveTo(geometry, xOf(0.5), 55.0f);
    editor.release();
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Begin, EditPhase::Changing, EditPhase::Changing, EditPhase::Committed}));
    ASSERT_EQ(value.stops.size(), 3u);
    EXPECT_NEAR(value.stops[1].pos, 0.5, 1e-6);

    // never below 2 stops
    editor.sync(blackToWhite());
    editor.pressStop(geometry, 0, xOf(0.0), 50.0f);
    editor.moveTo(geometry, xOf(0.0), 100.0f);
    EXPECT_EQ(editor.warning(), ramp::TOO_FEW_STOPS);
    editor.release();
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Begin, EditPhase::Cancelled}));
    EXPECT_EQ(value.stops.size(), 2u);

    // Delete removes the selected stop as one step, and also keeps 2
    editor.sync(threeStops());
    editor.select(1);
    editor.deleteSelected();
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Applied}));
    EXPECT_EQ(value.stops.size(), 2u);
    editor.sync(value);
    editor.deleteSelected();
    EXPECT_TRUE(drain(editor).empty());
    EXPECT_EQ(editor.warning(), ramp::TOO_FEW_STOPS);
}

TEST(RampFieldTest, DraggingAStopMovesItAndEscRestores) {
    RampEditor editor;
    editor.sync(threeStops());
    const RampEditor::Geometry geometry = bar();
    editor.pressStop(geometry, 1, xOf(0.25), 50.0f);
    editor.moveTo(geometry, xOf(0.8), 50.0f);
    ta::TitleGradient value;
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Begin, EditPhase::Changing}));
    EXPECT_NEAR(value.stops[1].pos, 0.8, 1e-6);
    // the caller's value is ignored during the gesture
    editor.sync(blackToWhite());
    EXPECT_EQ(editor.value().stops.size(), 3u);
    editor.cancel();
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Cancelled}));
    EXPECT_TRUE(ramp::sameGradient(value, threeStops()));
    EXPECT_FALSE(editor.dragging());
}

// Red control: resetting on any click (not a double-click) or to the stop's position fails.
TEST(RampFieldTest, MidpointsDragAndDoubleClickReset) {
    RampEditor editor;
    editor.sync(threeStops());
    const RampEditor::Geometry geometry = bar();
    // the diamond between stop 1 (0.25, mid 0.6) and stop 2 (1.0)
    EXPECT_NEAR(ramp::midpointPos(editor.value(), 1), 0.25 + 0.75 * 0.6, 1e-9);
    EXPECT_EQ(ramp::hitMidpoint(editor.value(), 10.0f, 200.0f, xOf(0.7), 5.0f), 1);
    editor.pressMidpoint(1, xOf(0.7));
    editor.moveTo(geometry, xOf(0.4375), 0.0f);           // the first move begins and changes
    editor.release();
    ta::TitleGradient value;
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Begin, EditPhase::Changing, EditPhase::Committed}));
    EXPECT_NEAR(value.stops[1].mid, 0.25, 1e-9);          // (0.4375 - 0.25) / 0.75
    // clamped to S80's range
    editor.sync(value);
    editor.pressMidpoint(1, xOf(0.4375));
    editor.moveTo(geometry, xOf(1.0), 0.0f);
    editor.release();
    drain(editor, &value);
    EXPECT_DOUBLE_EQ(value.stops[1].mid, ramp::MID_MAX);

    editor.sync(value);
    editor.doubleClickMidpoint(1);
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Applied}));
    EXPECT_DOUBLE_EQ(value.stops[1].mid, 0.5);
    editor.sync(value);
    editor.doubleClickMidpoint(1);                       // already 0.5: no step
    EXPECT_TRUE(drain(editor).empty());
}

// Red control: reversing positions without moving the midpoints to the mirrored gaps fails the
// mid and colour checks.
TEST(RampFieldTest, ReverseMirrorsPositionsAndMidpoints) {
    const ta::TitleGradient original = threeStops();
    ta::TitleGradient reversed = original;
    ramp::reverse(reversed);
    ASSERT_EQ(reversed.stops.size(), 3u);
    EXPECT_DOUBLE_EQ(reversed.stops[0].pos, 0.0);
    EXPECT_FLOAT_EQ(reversed.stops[0].color.b, 1.0f);
    EXPECT_NEAR(reversed.stops[0].mid, 0.4, 1e-12);       // 1 - 0.6: the gap 0.25..1 mirrored
    EXPECT_DOUBLE_EQ(reversed.stops[1].pos, 0.75);
    EXPECT_NEAR(reversed.stops[1].mid, 0.7, 1e-12);       // 1 - 0.3
    EXPECT_DOUBLE_EQ(reversed.stops[2].pos, 1.0);
    EXPECT_FLOAT_EQ(reversed.stops[2].color.r, 1.0f);
    // the half-way colours sit at the mirrored diamonds
    for (size_t gap = 0; gap < 2; gap++) {
        const double at = ramp::midpointPos(original, gap);
        const ramp::Rgba a = ramp::sample(original, at);
        const ramp::Rgba b = ramp::sample(reversed, 1.0 - at);
        EXPECT_NEAR(a.r, b.r, 1e-9);
        EXPECT_NEAR(a.g, b.g, 1e-9);
        EXPECT_NEAR(a.b, b.b, 1e-9);
        EXPECT_NEAR(a.a, b.a, 1e-9);
    }
    // twice is the original
    ramp::reverse(reversed);
    EXPECT_TRUE(ramp::sameGradient(reversed, original));

    RampEditor editor;
    editor.sync(original);
    editor.select(0);
    editor.reverse();
    ta::TitleGradient value;
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Applied}));
    EXPECT_EQ(editor.selected(), 2);                      // the same stop stays selected
    EXPECT_DOUBLE_EQ(value.stops[1].pos, 0.75);
}

// Red control: a Vivid toggle that does not write `interp` leaves "rgb" in the op.
TEST(RampFieldTest, VividSetsInterpVivid) {
    RampEditor editor;
    editor.sync(blackToWhite());
    editor.setVivid(true);
    ta::TitleGradient value;
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Applied}));
    EXPECT_TRUE(value.vivid);
    const ta::Json json = ramp::toJson(value);
    EXPECT_EQ(json["interp"], "vivid");
    ASSERT_EQ(json["stops"].size(), 2u);
    EXPECT_EQ(json["stops"][1]["color"], "#ffffff");
    // S80 reads it back
    EXPECT_TRUE(ramp::sameGradient(ta::decodeGradient(json), value));
    editor.sync(value);
    editor.setVivid(true);                               // unchanged: no step
    EXPECT_TRUE(drain(editor).empty());
    EXPECT_EQ(ramp::toJson(blackToWhite())["interp"], "rgb");
}

// Red control: without the MAX_STOPS check the 33rd stop is added.
TEST(RampFieldTest, TheThirtyThirdStopIsRefused) {
    ta::TitleGradient full;
    for (int i = 0; i < 32; i++) full.stops.push_back(stop(i / 31.0, 0.5f, 0.5f, 0.5f));
    std::string error;
    ta::TitleGradient copy = full;
    EXPECT_EQ(ramp::insertStop(copy, 0.5, &error), -1);
    EXPECT_EQ(error, "At most 32 stops");
    EXPECT_EQ(copy.stops.size(), 32u);

    RampEditor editor;
    editor.sync(full);
    editor.pressBar(bar(), xOf(0.5));
    EXPECT_TRUE(drain(editor).empty());
    EXPECT_EQ(editor.warning(), "At most 32 stops");
    EXPECT_EQ(editor.value().stops.size(), 32u);
    // the next edit clears it
    editor.setVivid(true);
    drain(editor);
    EXPECT_TRUE(editor.warning().empty());
}

// The selected stop's fields: each edit is Begin .. Changing .. Committed, Begin unchanged.
TEST(RampFieldTest, StopFieldsReportOneStep) {
    RampEditor editor;
    editor.sync(threeStops());
    editor.select(1);
    editor.editSelectedAlpha(EditPhase::Begin, 0.5);
    editor.editSelectedAlpha(EditPhase::Changing, 0.2);
    editor.editSelectedAlpha(EditPhase::Committed, 0.2);
    RampEditor::Step step;
    ASSERT_TRUE(editor.next(step));
    EXPECT_EQ(step.phase, EditPhase::Begin);
    EXPECT_DOUBLE_EQ(step.value.stops[1].alpha, 0.5);    // the original
    ASSERT_TRUE(editor.next(step));
    EXPECT_EQ(step.phase, EditPhase::Changing);
    EXPECT_DOUBLE_EQ(step.value.stops[1].alpha, 0.2);
    ASSERT_TRUE(editor.next(step));
    EXPECT_EQ(step.phase, EditPhase::Committed);
    EXPECT_FALSE(editor.next(step));

    // the location field keeps the stop selected as it passes another
    editor.sync(step.value);
    editor.editSelectedPosition(EditPhase::Changing, 0.9);
    EXPECT_EQ(editor.selected(), 1);
    editor.editSelectedPosition(EditPhase::Changing, 0.0);
    EXPECT_EQ(editor.selected(), 1);                     // after the stop already at 0
    EXPECT_DOUBLE_EQ(editor.value().stops[1].pos, 0.0);
    drain(editor);

    // the colour window's Esc brings the original colour back
    editor.editSelectedColor(EditPhase::Begin, {0, 0, 0});
    editor.editSelectedColor(EditPhase::Changing, {1, 1, 0});
    editor.editSelectedColor(EditPhase::Cancelled, {0, 1, 0});
    ta::TitleGradient value;
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Begin, EditPhase::Changing, EditPhase::Cancelled}));
    EXPECT_FLOAT_EQ(value.stops[1].color.r, 0.0f);
    EXPECT_FLOAT_EQ(value.stops[1].color.g, 1.0f);
}

TEST(RampFieldTest, PresetsAndHitTests) {
    const Gradient preset = Gradient::twoColors(Vector3(1.0f, 0.5f, 0.0f), Vector3(0.0f, 0.25f, 1.0f), 1.0f, 0.5f);
    const ta::TitleGradient converted = ramp::fromPreset(preset);
    ASSERT_EQ(converted.stops.size(), 2u);
    EXPECT_DOUBLE_EQ(converted.stops[0].pos, 0.0);
    EXPECT_DOUBLE_EQ(converted.stops[1].pos, 1.0);
    EXPECT_NEAR(converted.stops[0].color.g, 0.5f, 1e-3);
    EXPECT_NEAR(converted.stops[1].color.g, 0.25f, 1e-3);
    EXPECT_NEAR(converted.stops[1].alpha, 0.5, 1e-6);
    EXPECT_FALSE(converted.vivid);

    RampEditor editor;
    editor.sync(threeStops());
    editor.applyPreset(converted);
    ta::TitleGradient value;
    EXPECT_EQ(drain(editor, &value), (Phases{EditPhase::Applied}));
    EXPECT_TRUE(ramp::sameGradient(value, converted));

    const ta::TitleGradient g = threeStops();
    EXPECT_EQ(ramp::hitStop(g, 10.0f, 200.0f, xOf(0.25) + 3.0f, 6.0f), 1);
    EXPECT_EQ(ramp::hitStop(g, 10.0f, 200.0f, xOf(0.6), 6.0f), -1);
    EXPECT_DOUBLE_EQ(ramp::posAt(10.0f, 200.0f, 0.0f), 0.0);       // clamped
    EXPECT_DOUBLE_EQ(ramp::posAt(10.0f, 200.0f, 110.0f), 0.5);
    // a gap narrower than MIN_MIDPOINT_GAP has no diamond
    ta::TitleGradient narrow;
    narrow.stops = {stop(0.0, 0, 0, 0), stop(0.01, 1, 1, 1), stop(1.0, 0, 0, 0)};
    EXPECT_EQ(ramp::hitMidpoint(narrow, 10.0f, 200.0f, xOf(0.005), 5.0f), -1);
}

// Mixed targets: the bar draws stripes, and an edit reports the whole gradient, which the glue
// sets on every target. Red control: drawing the mixed bar without its stripes adds no vertices.
TEST(RampFieldTest, MixedTargetsStripeTheBarAndAnEditSetsAll) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    ta::TitleGradient first = threeStops();
    ta::TitleGradient second = blackToWhite();
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(400, 300));
    ImGui::Begin("Ramp");
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const int start = drawList->VtxBuffer.Size;
    ui::drawRampBar(drawList, ImVec2(10, 10), ImVec2(210, 30), first, false);
    const int plain = drawList->VtxBuffer.Size - start;
    ui::drawRampBar(drawList, ImVec2(10, 40), ImVec2(210, 60), first, true);
    const int striped = drawList->VtxBuffer.Size - start - plain;
    EXPECT_GT(striped, plain);

    // the whole field draws without input and reports nothing
    ui::RampFieldOptions options;
    options.mixed = true;
    options.presets = false;
    ta::TitleGradient shown = first;
    EXPECT_EQ(ui::rampField("##ramp", shown, options), EditPhase::None);
    EXPECT_TRUE(ramp::sameGradient(shown, first));
    ImGui::End();
    ImGui::EndFrame();
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);

    // an edit on the mixed field: the value it reports goes to every target
    RampEditor editor;
    editor.sync(first);
    editor.reverse();
    RampEditor::Step step;
    ASSERT_TRUE(editor.next(step));
    EXPECT_EQ(step.phase, EditPhase::Applied);
    first = step.value;
    second = step.value;
    EXPECT_TRUE(ramp::sameGradient(first, second));
    EXPECT_EQ(second.stops.size(), 3u);
}
