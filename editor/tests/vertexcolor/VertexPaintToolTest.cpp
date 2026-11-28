// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R30-R35, R50-R55, R100-R110 (AC8, AC14's editor half): the Vertex Paint
// tools through the editor facade. A scripted stroke crossing a vertex ten times at 30 % moves it 30 %
// and more with Build-up; the Select tool's click, box and X-Ray on a box; Apply writes corners in
// Face mode and shared values in Vertex mode; Esc cancels a stroke and a second Esc leaves; arming
// switches the Scene view to Vertex Color and exiting gives the Draw Mode back unless it changed;
// Table 3's keys in their own context; the window and every dialog drawn in an ImGui frame.

#include "support/EditorTestSupport.h"
#include "support/PosixCompat.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Project.h"
#include "Scene.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "component/VertexColorsComponent.h"
#include "scenetool/SceneTool.h"
#include "shortcuts/ShortcutTestSupport.h"
#include "vertexcolor/VertexPaintBrush.h"
#include "vertexcolor/VertexPaintTool.h"
#include "vertexcolor/VertexPaintWindow.h"

#include "imgui.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace vertex_paint_tool_test {

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

// (a named namespace, tests included: a unity build merges the test sources)
    using namespace jasperengine::editor::vertexcolor;

    struct Headless {
        std::unique_ptr<Project> project;
        fs::path root;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            out.root = fs::temp_directory_path() / ("jasper-vertex-paint-test-" + std::to_string(::getpid()));
            fs::remove_all(out.root);
            fs::create_directories(out.root / "config");
            setenv("XDG_CONFIG_HOME", (out.root / "config").c_str(), 1);
            const fs::path dir = out.root / "project";
            fs::create_directories(dir / ".jasperengine");
            fs::create_directories(dir / "assets");
            std::ofstream(dir / "project.yaml") << "name: vertexpaint\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            editor::test::useProjectPaths(out.project.get());
            Engine::setAsyncLoading(false);
            if (!out.project->loadProject(dir, false)) throw std::runtime_error("the test project did not load");
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    struct TempCleanup : public ::testing::Environment {
        void TearDown() override {
            std::error_code ec;
            if (headless().project) fs::remove_all(headless().root, ec);
        }
    };

    class VertexPaint : public ::testing::Test {
    protected:
        Project* project = nullptr;
        uint32_t sceneId = 0;

        void SetUp() override {
            project = headless().project.get();
            ASSERT_NE(project, nullptr);
            if (SceneProject* current = project->getSelectedScene()) current->isModified = false;
            project->createNewScene("Paint", SceneType::SCENE_3D);
            sceneId = project->getSelectedSceneId();
            VertexPaintTools::get().setProject(project);
            VertexPaintWindow::get().setProject(project);
            VertexPaintTools::get().settings() = VertexPaintSettings();
        }

        void TearDown() override {
            VertexPaintTools::get().reset();
            if (SceneProject* scene = project ? project->getScene(sceneId) : nullptr) scene->isModified = false;
        }

        Scene* scene() { return project->getScene(sceneId)->scene; }
        SceneProject* sceneProject() { return project->getScene(sceneId); }
        CommandHistory* history() { return CommandHandle::get(sceneId); }

        Entity create(const std::string& name, EntityCreationType type) {
            auto* cmd = new CreateEntityCmd(project, sceneId, name, type);
            cmd->setQuiet(true);
            history()->addCommandNoMerge(cmd);
            const Entity entity = cmd->getEntity();
            project->replaceSelectedEntities(sceneId, {entity});
            return entity;
        }

        std::vector<TargetRef> targetsOf(Entity entity) {
            std::vector<TargetRef> out;
            for (const TargetInfo& info : listTargets(project, sceneId, entity)) out.push_back(TargetRef{entity, info.key});
            return out;
        }

        // the values the facade holds now (the initial values before the first edit)
        vc::TargetData data(const TargetRef& ref) { return targetDataOrInitial(project, sceneId, ref.entity, ref.key); }

        // a ray from the eye straight down -Z through (x, y): the Box faces it with its front (+Z)
        static Ray rayAt(float x, float y) { return Ray(Vector3(x, y, 5.0f), Vector3(0.0f, 0.0f, -10.0f)); }

        static SceneToolEvent eventAt(const Ray& ray, float x = 0.0f, float y = 0.0f) {
            SceneToolEvent event;
            event.ray = ray;
            event.x = x;
            event.y = y;
            return event;
        }
    };

[[maybe_unused]] static ::testing::Environment* const vertexPaintCleanup = ::testing::AddGlobalTestEnvironment(new TempCleanup);

// ==================================================================== the grammar (no scene)

TEST(VertexPaintBrush, ModifiersUndoNamesAndGestures) {
    EXPECT_EQ(effectiveStrokeMode(PaintTool::Paint, false), vc::StrokeMode::Paint);
    EXPECT_EQ(effectiveStrokeMode(PaintTool::Paint, true), vc::StrokeMode::Erase) << "R104: Ctrl inverts Paint";
    EXPECT_EQ(effectiveStrokeMode(PaintTool::Smooth, false), vc::StrokeMode::Smooth);
    EXPECT_EQ(effectiveStrokeMode(PaintTool::Smooth, true), vc::StrokeMode::Sharpen);
    EXPECT_EQ(strokeUndoName(vc::StrokeMode::Paint), "Paint Vertex Colors");
    EXPECT_EQ(applyUndoName(vc::ElementKind::Face, 412), "Apply Color to 412 Faces") << "R107's example";
    EXPECT_EQ(applyUndoName(vc::ElementKind::Vertex, 1), "Apply Color to 1 Vertex");
    EXPECT_EQ(selectionCountText(vc::ElementKind::Face, 412), "412 faces selected");

    VertexPaintSettings settings;
    settings.radius = 1.0f;
    settings.strength = 0.5f;
    // R104 Ctrl+RMB: up grows, right strengthens; Shift+RMB: up hardens, right turns the stencil
    applyRightDrag(settings, true, false, 40.0f, -50.0f);
    EXPECT_GT(settings.radius, 1.5f);
    EXPECT_NEAR(settings.strength, 0.6f, 1e-4f);
    applyRightDrag(settings, false, true, 20.0f, -40.0f);
    EXPECT_NEAR(settings.hardness, 0.4f, 1e-4f);
    EXPECT_NEAR(settings.stencilRotation, 10.0f, 1e-4f);
    // R35: a mouse presses fully
    EXPECT_EQ(pressureOf(-1.0f), 1.0f);
    EXPECT_EQ(pressureOf(0.25f), 0.25f);

    // R105: 1-4 toggle channels, X swaps
    settings.channels = vc::CHANNELS_RGB;
    toggleChannel(settings, 3);
    EXPECT_EQ(settings.channels, vc::CHANNELS_RGBA);
    toggleChannel(settings, 0);
    EXPECT_EQ(settings.channels, (vc::ChannelMask)(vc::CHANNEL_G | vc::CHANNEL_B | vc::CHANNEL_A));
    settings.primary = Vector4(1, 0, 0, 1);
    settings.secondary = Vector4(0, 0, 1, 1);
    swapColors(settings);
    EXPECT_EQ(settings.primary, Vector4(0, 0, 1, 1));

    // the stroke op carries what the window set
    settings.buildup = true;
    settings.element = vc::Element::Face;
    settings.masks.slope = true;
    const vc::StrokePoint point{Vector3(1, 2, 3), 0.5f};
    const vc::Op op = strokeOp(settings, vc::StrokeMode::Paint, true, std::span<const vc::StrokePoint>(&point, 1));
    EXPECT_EQ(op.type, vc::OpType::Stroke);
    EXPECT_TRUE(op.buildup);
    EXPECT_EQ(op.value, settings.secondary) << "Shift paints the secondary colour";
    EXPECT_EQ(op.target.element, vc::Element::Face);
    ASSERT_EQ(op.filters.size(), 1u);
    EXPECT_EQ(op.filters[0].kind, vc::FilterKind::Slope);
    // Apply: an exact set at full Replace, a fill otherwise
    settings.blend = vc::Blend::Replace;
    settings.strength = 1.0f;
    EXPECT_EQ(applyOp(settings, vc::Target(), false).type, vc::OpType::Set);
    settings.strength = 0.5f;
    EXPECT_EQ(applyOp(settings, vc::Target(), false).type, vc::OpType::Fill);
}

// R103: the outer ring, the hardness ring and the strength arc, the arc's length following strength
TEST(VertexPaintBrush, CursorRingsAndArc) {
    PaintCursorLines full, half;
    buildCursor(Vector3(0, 0, 0), Vector3(0, 1, 0), 1.0f, 0.5f, 1.0f, Vector4(1, 1, 1, 1), nullptr, full);
    buildCursor(Vector3(0, 0, 0), Vector3(0, 1, 0), 1.0f, 0.5f, 0.5f, Vector4(1, 1, 1, 1), nullptr, half);
    ASSERT_EQ(full.points.size(), full.colors.size() * 2);
    EXPECT_GT(full.colors.size(), half.colors.size()) << "a longer strength arc at 100 %";
    // every point lies on the plane of the normal, the rings at their radii
    size_t onOuter = 0, onInner = 0;
    for (const Vector3& p : full.points) {
        EXPECT_NEAR(p.y, 0.0f, 0.3f);
        const float r = std::sqrt(p.x * p.x + p.z * p.z);
        if (std::fabs(r - 1.0f) < 1e-3f) onOuter++;
        if (std::fabs(r - 0.5f) < 1e-3f) onInner++;
    }
    EXPECT_GE(onOuter, 96u);
    EXPECT_GE(onInner, 96u) << "the hardness ring";
    // single-channel work is grey (R103)
    VertexPaintSettings settings;
    settings.primary = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
    settings.channels = vc::CHANNEL_R;
    const Vector4 tint = cursorTint(settings, vc::StrokeMode::Paint, false, 1.0f);
    EXPECT_FLOAT_EQ(tint.x, tint.y);
    EXPECT_FLOAT_EQ(tint.y, tint.z);
}

// R51: box and lasso through a projection, X-Ray off hiding what the visibility test hides
TEST(VertexPaintBrush, RegionSelectionAndXRay) {
    // two quads, one behind the other along -Z, as seen down -Z
    vc::MeshView mesh;
    mesh.positions = {Vector3(-1, -1, 0), Vector3(1, -1, 0), Vector3(1, 1, 0), Vector3(-1, 1, 0),
                      Vector3(-1, -1, -1), Vector3(1, -1, -1), Vector3(1, 1, -1), Vector3(-1, 1, -1)};
    mesh.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    std::string error;
    ASSERT_TRUE(mesh.build(&error)) << error;
    const Matrix4 model;
    const ProjectFn project = [](const Vector3& w, Vector2& s) {
        s = Vector2(w.x * 100.0f + 500.0f, -w.y * 100.0f + 500.0f);
        return true;
    };
    const VisibleFn frontOnly = [](const Vector3& w, const Vector3&) { return w.z > -0.5f; };
    const vc::Selection xray = selectInRect(mesh, model, vc::ElementKind::Face, Vector2(350, 350), Vector2(650, 650), project, nullptr);
    EXPECT_EQ(xray.ids.size(), 4u) << "X-Ray: through to the hidden quad";
    const vc::Selection visible = selectInRect(mesh, model, vc::ElementKind::Face, Vector2(350, 350), Vector2(650, 650), project, frontOnly);
    EXPECT_EQ(visible.ids, (std::vector<uint32_t>{0, 1})) << "X-Ray off: only what is seen";
    const vc::Selection corner = selectInRect(mesh, model, vc::ElementKind::Vertex, Vector2(380, 380), Vector2(420, 420), project, frontOnly);
    ASSERT_EQ(corner.ids.size(), 1u) << "the vertex at (-1, 1) alone";
    // triangle 1's centroid (-1/3, 1/3) is at (467, 467), triangle 0's (1/3, -1/3) at (533, 533)
    const std::vector<Vector2> loop = {Vector2(350, 350), Vector2(500, 340), Vector2(510, 500), Vector2(345, 505)};
    const vc::Selection lasso = selectInLasso(mesh, model, vc::ElementKind::Face, loop, project, frontOnly);
    EXPECT_EQ(lasso.ids, (std::vector<uint32_t>{1})) << "only the triangle whose centroid the lasso holds";
    // Shift adds, Ctrl removes
    EXPECT_EQ(combineSelection(visible, lasso, false, true).ids, (std::vector<uint32_t>{0}));
    EXPECT_EQ(combineSelection(lasso, visible, true, false).ids, (std::vector<uint32_t>{0, 1}));

    // R108: a click's element under a barycentric hit
    EXPECT_EQ(elementAtHit(mesh, vc::ElementKind::Face, 1, Vector3(0.2f, 0.3f, 0.5f)), 1u);
    EXPECT_EQ(elementAtHit(mesh, vc::ElementKind::Corner, 1, Vector3(0.2f, 0.3f, 0.5f)), 5u);
    EXPECT_EQ(elementAtHit(mesh, vc::ElementKind::Vertex, 1, Vector3(0.2f, 0.3f, 0.5f)), mesh.logical[3]);
    const MeshHit hit = raycastMesh(mesh, model, Vector3(0.5f, -0.2f, 5.0f), Vector3(0, 0, -10), false);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.triangle, 0u) << "the front quad, not the one behind";
    EXPECT_NEAR(hit.point.z, 0.0f, 1e-5f);
}

// R109
TEST(VertexPaintBrush, LowDensityText) {
    vc::MeshView mesh;
    mesh.positions = {Vector3(-5, 0, -5), Vector3(5, 0, -5), Vector3(5, 0, 5), Vector3(-5, 0, 5)};
    mesh.indices = {0, 2, 1, 0, 3, 2};
    ASSERT_TRUE(mesh.build());
    EXPECT_TRUE(isLowDensity(mesh, Matrix4(), 0.5f)) << "4 vertices on 100 m², a 0.5 m brush";
    EXPECT_FALSE(isLowDensity(mesh, Matrix4(), 10.0f));
    EXPECT_EQ(lowDensityText("Rock_01", 24), "Rock_01: 24 vertices \xE2\x80\x94 paint will look blocky");
}

// ==================================================================== through the facade

// AC8 (R31): ten crossings at 30 % move the vertex exactly 30 % of the way; Build-up passes it
TEST_F(VertexPaint, ScriptedStrokeCapsAtStrength) {
    const Entity plane = create("Plane", EntityCreationType::PLANE);
    const std::vector<TargetRef> targets = targetsOf(plane);
    ASSERT_FALSE(targets.empty());
    std::shared_ptr<const vc::MeshView> mesh = meshView(project, sceneId, plane, targets[0].key);
    ASSERT_TRUE(mesh && mesh->vertexCount() > 0);
    const Matrix4 model = targetModel(project, sceneId, plane, targets[0].key);
    // the first vertex, crossed back and forth along X
    const Vector3 vertex = model * mesh->positions[0];
    std::vector<vc::StrokePoint> points;
    for (int pass = 0; pass <= 10; pass++) points.push_back(vc::StrokePoint{vertex + Vector3(pass % 2 ? 0.3f : -0.3f, 0.0f, 0.0f), 1.0f});

    VertexPaintSettings settings;
    settings.primary = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
    settings.channels = vc::CHANNEL_R;
    settings.strength = 0.3f;
    settings.radius = 0.1f;
    settings.falloff = vc::FalloffPreset::Constant;
    settings.hardness = 1.0f;
    const float start = data(targets[0]).base[0].x;
    const float startG = data(targets[0]).base[0].y;
    std::string why;
    const size_t steps = history()->position();
    ASSERT_TRUE(VertexPaintTools::scriptedStroke(project, sceneId, targets, settings, vc::StrokeMode::Paint, false, points, why)) << why;
    EXPECT_EQ(history()->position(), steps + 1) << "one stroke, one undo step (R107)";
    ASSERT_NE(history()->top(), nullptr);
    EXPECT_EQ(history()->top()->name(), "Paint Vertex Colors");
    const float capped = data(targets[0]).base[0].x;
    EXPECT_NEAR(capped, start + (0.0f - start) * 0.3f, 1.0f / 255.0f) << "exactly 30 % of the way, however often crossed";
    EXPECT_EQ(data(targets[0]).base[0].y, startG) << "G was not a painted channel";

    ASSERT_TRUE(history()->undo());
    EXPECT_NEAR(data(targets[0]).base[0].x, start, 1.0f / 255.0f);
    settings.buildup = true;
    ASSERT_TRUE(VertexPaintTools::scriptedStroke(project, sceneId, targets, settings, vc::StrokeMode::Paint, false, points, why)) << why;
    EXPECT_LT(data(targets[0]).base[0].x, capped - 0.05f) << "Build-up adds every stamp: past 30 %";
}

// R51: a click on a face; Shift adds, Ctrl removes; a box sees the front only unless X-Ray
TEST_F(VertexPaint, SelectClickBoxAndXRay) {
    const Entity box = create("Box", EntityCreationType::BOX);
    VertexPaintTools& tools = VertexPaintTools::get();
    tools.settings().selectionMode = vc::ElementKind::Face;
    tools.armIn(sceneId, PaintTool::Select);
    // the two triangles of the front (+Z) face, clicked at their centroids
    const std::vector<TargetRef> targets = targetsOf(box);
    ASSERT_EQ(targets.size(), 1u);
    std::shared_ptr<const vc::MeshView> mesh = meshView(project, sceneId, box, targets[0].key);
    ASSERT_TRUE(mesh);
    const Matrix4 model = targetModel(project, sceneId, box, targets[0].key);
    std::vector<Vector3> front;
    for (uint32_t t = 0; t < mesh->triangleCount(); t++) {
        if (mesh->faceNormals[t].z < 0.9f) continue;
        front.push_back(model * ((mesh->positions[mesh->indices[t * 3]] + mesh->positions[mesh->indices[t * 3 + 1]] + mesh->positions[mesh->indices[t * 3 + 2]]) / 3.0f));
    }
    ASSERT_EQ(front.size(), 2u) << "a box's front is two triangles";
    ASSERT_TRUE(tools.selectAt(sceneId, rayAt(front[0].x, front[0].y), false, false));
    ASSERT_EQ(elementSelection(sceneId).size(), 1u);
    EXPECT_EQ(elementSelection(sceneId)[0].entity, box);
    EXPECT_EQ(tools.selectedCount(sceneId), 1u);
    ASSERT_TRUE(tools.selectAt(sceneId, rayAt(front[1].x, front[1].y), true, false));
    EXPECT_EQ(tools.selectedCount(sceneId), 2u) << "Shift adds the front's other triangle";
    ASSERT_TRUE(tools.selectAt(sceneId, rayAt(front[0].x, front[0].y), false, true));
    EXPECT_EQ(tools.selectedCount(sceneId), 1u) << "Ctrl removes";

    // the box seen from +Z, orthographically, as pixels
    const ProjectFn project2d = [](const Vector3& w, Vector2& s) {
        s = Vector2(w.x * 100.0f + 500.0f, -w.y * 100.0f + 500.0f);
        return true;
    };
    const std::vector<Vector2> rect = {Vector2(300, 300), Vector2(700, 700)};
    tools.settings().xray = false;
    ASSERT_TRUE(tools.selectRegion(sceneId, rect, false, false, false, project2d, Vector3(0, 0, 5)));
    EXPECT_EQ(tools.selectedCount(sceneId), 2u) << "X-Ray off: the front face's two triangles";
    tools.settings().xray = true;
    ASSERT_TRUE(tools.selectRegion(sceneId, rect, false, false, false, project2d, Vector3(0, 0, 5)));
    EXPECT_EQ(tools.selectedCount(sceneId), 12u) << "X-Ray: every face";
    // R50: the selection goes with the tool unless pinned
    tools.exit();
    EXPECT_TRUE(elementSelection(sceneId).empty());
}

// R53, D5: Apply in Face mode writes corners (hard edges); in Vertex mode shared values, no corner
TEST_F(VertexPaint, ApplyHardInFaceModeSharedInVertexMode) {
    const Entity box = create("Box", EntityCreationType::BOX);
    const std::vector<TargetRef> targets = targetsOf(box);
    ASSERT_EQ(targets.size(), 1u);
    VertexPaintTools& tools = VertexPaintTools::get();
    tools.settings().primary = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
    tools.settings().strength = 1.0f;
    tools.settings().blend = vc::Blend::Replace;
    tools.armIn(sceneId, PaintTool::Select);

    vc::Selection faces;
    faces.kind = vc::ElementKind::Face;
    faces.ids = {0, 1};
    setElementSelection(project, sceneId, {ElementSelection{box, targets[0].key, faces}}, "");
    ASSERT_TRUE(tools.applyToSelection(sceneId, false)) << tools.lastError();
    EXPECT_EQ(history()->top()->name(), "Apply Color to 2 Faces");
    const vc::TargetData hard = data(targets[0]);
    EXPECT_TRUE(hard.hasOverrides()) << "Face mode: corner values, a hard edge around the face";
    ASSERT_TRUE(history()->undo());

    vc::Selection vertices;
    vertices.kind = vc::ElementKind::Vertex;
    vertices.ids = {0};
    setElementSelection(project, sceneId, {ElementSelection{box, targets[0].key, vertices}}, "");
    ASSERT_TRUE(tools.applyToSelection(sceneId, false)) << tools.lastError();
    EXPECT_EQ(history()->top()->name(), "Apply Color to 1 Vertex");
    const vc::TargetData shared = data(targets[0]);
    EXPECT_FALSE(shared.hasOverrides()) << "Vertex mode: one shared value";
    std::shared_ptr<const vc::MeshView> mesh = meshView(project, sceneId, box, targets[0].key);
    ASSERT_TRUE(mesh);
    for (uint32_t m = mesh->memberStart[0]; m < mesh->memberStart[1]; m++) {
        EXPECT_EQ(shared.base[mesh->members[m]], Vector4(1.0f, 0.0f, 0.0f, 1.0f)) << "every GPU vertex of the logical vertex (R4)";
    }
}

// R106: Esc cancels the stroke and its values come back; a second Esc leaves the tool
TEST_F(VertexPaint, EscCancelsTheStrokeThenExits) {
    const Entity box = create("Box", EntityCreationType::BOX);
    const std::vector<TargetRef> targets = targetsOf(box);
    ASSERT_FALSE(targets.empty());
    VertexPaintTools& tools = VertexPaintTools::get();
    tools.settings().primary = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
    tools.settings().strength = 1.0f;
    tools.settings().radius = 2.0f;
    tools.settings().skipOccluded = false;
    tools.armIn(sceneId, PaintTool::Paint);
    SceneTool* tool = SceneTools::active(sceneId);
    ASSERT_NE(tool, nullptr);
    EXPECT_STREQ(tool->exitTooltip(), "Exit Vertex Paint") << "R106: the badge is the tool's";
    const uint64_t before = vc::hashTarget(data(targets[0]));
    const size_t steps = history()->position();
    ASSERT_EQ(tool->press(eventAt(rayAt(0.0f, 0.0f))), SceneToolPress::Captured);
    tool->drag(eventAt(rayAt(0.1f, 0.1f)));
    EXPECT_TRUE(tools.isStrokeActive());
    EXPECT_NE(vc::hashTarget(data(targets[0])), before) << "the stroke paints live";
    EXPECT_TRUE(tool->key(ImGuiKey_Escape, eventAt(rayAt(0.1f, 0.1f))));
    EXPECT_FALSE(tools.isStrokeActive());
    EXPECT_EQ(vc::hashTarget(data(targets[0])), before) << "Esc restored the values";
    EXPECT_EQ(history()->position(), steps) << "and left no step";
    EXPECT_TRUE(tool->available(*sceneProject()));
    EXPECT_TRUE(tool->key(ImGuiKey_Escape, eventAt(rayAt(0.1f, 0.1f))));
    EXPECT_FALSE(tool->available(*sceneProject())) << "the second Esc leaves (the view deactivates it)";
    SceneTools::deactivate(sceneId);
    EXPECT_FALSE(tools.isArmed());
}

// R102: arming shows Vertex Color (RGB); exit gives the Draw Mode back, unless it changed meanwhile
TEST_F(VertexPaint, DrawModeSwitchesAndComesBack) {
    create("Box", EntityCreationType::BOX);
    VertexPaintTools& tools = VertexPaintTools::get();
    SceneDisplaySettings& display = sceneProject()->displaySettings;
    display.drawMode = ViewDrawMode::SHADED_WIREFRAME;
    tools.armIn(sceneId, PaintTool::Paint);
    EXPECT_EQ(display.drawMode, ViewDrawMode::VERTEX_COLOR);
    EXPECT_EQ(display.vertexColorChannel, ViewVertexColorChannel::RGB);
    tools.armIn(sceneId, PaintTool::Smooth);
    EXPECT_EQ(display.drawMode, ViewDrawMode::VERTEX_COLOR) << "a tool switch keeps the view";
    // the View row's channels are the tool's own changes
    tools.setViewChannel(ViewVertexColorChannel::A);
    EXPECT_EQ(display.vertexColorChannel, ViewVertexColorChannel::A);
    tools.exit();
    EXPECT_EQ(display.drawMode, ViewDrawMode::SHADED_WIREFRAME) << "given back";
    EXPECT_EQ(display.vertexColorChannel, ViewVertexColorChannel::RGB);

    tools.armIn(sceneId, PaintTool::Paint);
    display.drawMode = ViewDrawMode::WIREFRAME;   // the user, from the toolbar
    tools.exit();
    EXPECT_EQ(display.drawMode, ViewDrawMode::WIREFRAME) << "a change of the user's stays";
}

// R100: Paint Vertex Colors adds the component (undoable), and the hook opens the window with Paint armed
TEST_F(VertexPaint, PaintVertexColorsOpensTheWindowArmed) {
    const Entity box = create("Box", EntityCreationType::BOX);
    project->replaceSelectedEntities(sceneId, {});
    std::string error;
    // what the Paint buttons run: the component, then the hook the window set at startup
    VertexPaintWindow::installPaintToolHook();
    ASSERT_TRUE(ensureComponent(project, sceneId, box, &error)) << error;
    ASSERT_TRUE(paintToolHook());
    paintToolHook()(project, sceneId, box);
    EXPECT_NE(scene()->findComponent<VertexColorsComponent>(box), nullptr);
    EXPECT_TRUE(VertexPaintWindow::get().isOpen());
    EXPECT_TRUE(VertexPaintTools::get().isArmedIn(sceneId));
    EXPECT_EQ(VertexPaintTools::get().currentTool(), PaintTool::Paint);
    EXPECT_EQ(project->getSelectedEntities(sceneId), std::vector<Entity>{box});
    VertexPaintWindow::get().setOpen(false);
}

// R108: Pick reads the stored corner values at the hit
TEST_F(VertexPaint, PickReadsTheStoredValue) {
    const Entity box = create("Box", EntityCreationType::BOX);
    const std::vector<TargetRef> targets = targetsOf(box);
    ASSERT_FALSE(targets.empty());
    VertexPaintTools& tools = VertexPaintTools::get();
    tools.armIn(sceneId, PaintTool::Select);
    tools.settings().primary = Vector4(0.0f, 1.0f, 0.0f, 1.0f);
    tools.settings().strength = 1.0f;
    ASSERT_TRUE(tools.selectCommand(sceneId, VertexPaintTools::SelectCommand::All));
    ASSERT_TRUE(tools.applyToSelection(sceneId, false)) << tools.lastError();
    tools.settings().primary = Vector4(1.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(tools.pickAt(sceneId, rayAt(0.1f, 0.2f), false));
    EXPECT_NEAR(tools.settings().primary.y, 1.0f, 1e-3f);
    EXPECT_NEAR(tools.settings().primary.x, 0.0f, 1e-3f);
}

// R105, Table 3: the keys of the VertexPaint context win only while a tool is armed
TEST(VertexPaintShortcuts, ContextOutranksPanelsWhileArmed) {
    using namespace jasperengine::editor::test;
    EXPECT_STREQ(shortcutContextName(ShortcutContext::VertexPaint), "Vertex Paint");
    EXPECT_STREQ(shortcutContextKey(ShortcutContext::VertexPaint), "vertex_paint");
    EXPECT_EQ(parseShortcutContext("vertex_paint"), ShortcutContext::VertexPaint);
    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    ShortcutDispatcher dispatcher(catalog);
    struct Row {
        const char* id;
        const char* chord;
    };
    const Row rows[] = {{"vertexPaint.tool.select", "Q"}, {"vertexPaint.tool.paint", "B"}, {"vertexPaint.tool.smooth", "S"},
                        {"vertexPaint.tool.fill", "G"}, {"vertexPaint.tool.gradient", "D"}, {"vertexPaint.tool.pick", "I"},
                        {"vertexPaint.channelR", "1"}, {"vertexPaint.channelG", "2"}, {"vertexPaint.channelB", "3"}, {"vertexPaint.channelA", "4"},
                        {"vertexPaint.swapColors", "X"}, {"vertexPaint.mode.vertex", "F1"}, {"vertexPaint.mode.face", "F2"},
                        {"vertexPaint.mode.corner", "F3"}, {"vertexPaint.apply", "Enter"}, {"vertexPaint.select.all", "Ctrl+A"},
                        {"vertexPaint.select.none", "Alt+A"}, {"vertexPaint.select.invert", "Ctrl+I"}, {"vertexPaint.select.grow", "Ctrl+="},
                        {"vertexPaint.select.shrink", "Ctrl+-"}, {"vertexPaint.select.linked", "L"}, {"vertexPaint.view.rgb", "V"},
                        {"vertexPaint.view.cycle", "Shift+V"}, {"vertexPaint.xray", "Alt+Z"}};
    ShortcutState armed;
    armed.panels = {ShortcutContext::SceneView, ShortcutContext::VertexPaint};
    ShortcutState unarmed;
    unarmed.panels = {ShortcutContext::SceneView};
    for (const Row& row : rows) {
        const CommandDef* def = catalog.find(row.id);
        ASSERT_NE(def, nullptr) << row.id;
        EXPECT_EQ(def->context, ShortcutContext::VertexPaint) << row.id;
        const std::span<const Chord> chords = catalog.bindings(row.id);
        ASSERT_FALSE(chords.empty()) << row.id;
        EXPECT_EQ(chords[0], chordOf(row.chord)) << row.id;
        const CommandDef* armedTarget = dispatcher.resolve(chordOf(row.chord), armed);
        ASSERT_NE(armedTarget, nullptr) << row.chord;
        EXPECT_EQ(armedTarget->id, row.id) << "the armed tool outranks the Scene view and the editor";
        const CommandDef* unarmedTarget = dispatcher.resolve(chordOf(row.chord), unarmed);
        if (unarmedTarget) {
            EXPECT_NE(unarmedTarget->id, row.id) << "an unarmed tool leaves the key to the rest";
        }
    }
    // [ and ] (Shift: strength) stay the Scene view's brush keys, which reach the armed tool
    EXPECT_EQ(dispatcher.resolve(chordOf("["), armed)->id, "terrain.brushSmaller");
    EXPECT_EQ(dispatcher.resolve(chordOf("Shift+]"), armed)->id, "terrain.brushStronger");
    // L and Ctrl+A keep their meanings unarmed
    EXPECT_EQ(dispatcher.resolve(chordOf("L"), unarmed)->id, "sceneView.togglePickLock");
    EXPECT_TRUE(catalog.resolveConflicts().empty());
    EXPECT_NE(catalog.markdownReference().find("Vertex Paint"), std::string::npos);
    std::string reason;
    ASSERT_TRUE(catalog.setBindings("vertexPaint.swapColors", {chordOf("K")}, &reason)) << reason;
    EXPECT_EQ(dispatcher.resolve(chordOf("K"), armed)->id, "vertexPaint.swapColors") << "rebindable";
}

// R101: the window, each section and each Generate & Bake dialog, drawn in a headless ImGui frame
TEST_F(VertexPaint, WindowAndDialogsDrawInAFrame) {
    const Entity box = create("Box", EntityCreationType::BOX);
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1200, 1400);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    VertexPaintWindow& window = VertexPaintWindow::get();
    auto frame = [&]() {
        ImGui::NewFrame();
        window.show();
        ImGui::EndFrame();
    };
    std::string error;
    VertexPaintWindow::installPaintToolHook();
    ensureComponent(project, sceneId, box, &error);
    paintToolHook()(project, sceneId, box);
    frame();
    VertexPaintTools::get().armIn(sceneId, PaintTool::Select);
    ASSERT_TRUE(VertexPaintTools::get().selectCommand(sceneId, VertexPaintTools::SelectCommand::All));
    frame();
    using D = VertexPaintWindow::Dialog;
    for (D d : {D::FromTexture, D::AmbientOcclusion, D::Curvature, D::Thickness, D::Gradient, D::Noise, D::Direction, D::TransferFrom, D::Adjust,
                D::Channels, D::ExportModel, D::BakeTexture}) {
        window.openDialog(d);
        frame();
        EXPECT_EQ(window.currentDialog(), d) << VertexPaintWindow::dialogTitle(d);
    }
    // a generator's ops: one per target, narrowed to the selection
    std::vector<std::vector<vc::Op>> perTarget;
    std::vector<TargetRef> targets;
    std::string undo;
    ASSERT_TRUE(window.dialogOps(D::AmbientOcclusion, perTarget, targets, undo, error)) << error;
    ASSERT_EQ(perTarget.size(), targets.size());
    EXPECT_EQ(undo, "Bake AO") << "R107";
    EXPECT_EQ(perTarget[0][0].type, vc::OpType::BakeAO);
    EXPECT_EQ(perTarget[0][0].target.kind, vc::TargetKind::Faces) << "the selection narrows it";
    window.setOpen(false);
    frame();
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}

} // namespace vertex_paint_tool_test
