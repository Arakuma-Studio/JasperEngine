// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spline edits as undo steps (docs/specs/S18-splines.md D13, AC13): each edit restores the
// previous knot array exactly when undone, a continuous drag of one selection is one step, the
// steps carry the S24 names, the spline tools refuse to leave fewer than two knots, a stroke
// becomes a CatmullRom spline that a later stroke from its end extends, and the menu presets
// create their GameObject in one step.

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/SplineEditCmd.h"
#include "component/SplineComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/SplineInstantiateComponent.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "object/spline/Spline.h"
#include "spline/SplineEditor.h"
#include "spline/SplinePresets.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <memory>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    bool sameKnots(const std::vector<spline::SplineKnot>& a, const std::vector<spline::SplineKnot>& b){
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++){
            if (a[i].position != b[i].position || a[i].tangentIn != b[i].tangentIn || a[i].tangentOut != b[i].tangentOut ||
                a[i].radius != b[i].radius || a[i].rollDeg != b[i].rollDeg || a[i].mode != b[i].mode){
                return false;
            }
        }
        return true;
    }

    class SplineEditCmdTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"spline-edit"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;
        Entity spline = NULL_ENTITY;

        void SetUp() override{
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()){
                if (!candidate.filepath.empty() && !candidate.scene){
                    project->loadScene(candidate.filepath, false, false, true);
                }
            }
            for (SceneProject& candidate : project->getScenes()){
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            project->setSelectedSceneId(scene->id);
            CreateEntityCmd* create = new CreateEntityCmd(project.get(), scene->id, "Road", EntityCreationType::SPLINE_ROAD);
            ASSERT_TRUE(history()->addCommandNoMerge(create));
            spline = create->getEntity();
            ASSERT_NE(spline, NULL_ENTITY);
        }

        void TearDown() override{
            if (SplineEditor::get().isEditing()) SplineEditor::get().endEdit(project.get());
            SplineEditor::get().cancelDraw();
            if (project){
                for (SceneProject& candidate : project->getScenes()){
                    CommandHandle::remove(candidate.id);
                }
                project->releaseScenes();
            }
            project.reset();
        }

        CommandHistory* history(){ return CommandHandle::get(scene->id); }
        SplineComponent& component(){ return scene->scene->getComponent<SplineComponent>(spline); }
        std::vector<spline::SplineKnot> knots(){ return component().spline.knots(); }
    };

}

TEST_F(SplineEditCmdTest, ThePresetIsOneStep){
    Scene* s = scene->scene;
    ASSERT_TRUE(s->findComponent<SplineComponent>(spline));
    ASSERT_TRUE(s->findComponent<SplineExtrudeComponent>(spline));
    ASSERT_TRUE(s->findComponent<MeshComponent>(spline));
    EXPECT_EQ(s->getComponent<SplineExtrudeComponent>(spline).shape, SplineExtrudeShape::Crown);
    EXPECT_GE(component().spline.knotCount(), 2u);
    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(s->isEntityCreated(spline) && s->findComponent<SplineComponent>(spline));
}

TEST_F(SplineEditCmdTest, EveryEditUndoesToTheKnotsBeforeIt){
    SplineEditor& editor = SplineEditor::get();
    editor.beginEdit(project.get(), scene->id, spline);

    const std::vector<spline::SplineKnot> start = knots();
    // insert on the curve
    const Vector3 onCurve = Spline(scene->scene, spline).evaluate(0.4f).position;
    ASSERT_TRUE(editor.insertOnCurve(project.get(), onCurve));
    ASSERT_NE(history()->top(), nullptr);
    EXPECT_NE(history()->top()->name().find("Insert Spline Knot"), std::string::npos) << history()->top()->name();
    const std::vector<spline::SplineKnot> inserted = knots();
    EXPECT_EQ(inserted.size(), start.size() + 1);

    // move the selection (the inserted knot)
    ASSERT_TRUE(editor.transformSelected(project.get(), Matrix4::translateMatrix(Vector3(0, 2, 0)), Vector3(0, 0, 0), false, false));
    EXPECT_NE(history()->top()->name().find("Move Spline Knots"), std::string::npos);
    const std::vector<spline::SplineKnot> moved = knots();

    // delete it
    ASSERT_TRUE(editor.deleteSelected(project.get()));
    EXPECT_NE(history()->top()->name().find("Delete Spline Knots"), std::string::npos);
    EXPECT_TRUE(sameKnots(knots(), start));

    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(sameKnots(knots(), moved));
    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(sameKnots(knots(), inserted));
    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(sameKnots(knots(), start));
}

TEST_F(SplineEditCmdTest, AContinuousDragIsOneStep){
    SplineEditor& editor = SplineEditor::get();
    editor.beginEdit(project.get(), scene->id, spline);
    editor.setSelection({0, 1});
    const std::vector<spline::SplineKnot> start = knots();
    const size_t position = history()->position();
    SplineEditor::endContinuousEdit(scene->id);
    for (int i = 0; i < 10; i++){
        ASSERT_TRUE(editor.transformSelected(project.get(), Matrix4::translateMatrix(Vector3(0.1f, 0, 0)), Vector3(0, 0, 0), false, true));
    }
    SplineEditor::endContinuousEdit(scene->id);
    EXPECT_EQ(history()->position(), position + 1);
    EXPECT_NEAR(knots()[0].position.x, start[0].position.x + 1.0f, 1.0e-4f);
    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(sameKnots(knots(), start));
}

TEST_F(SplineEditCmdTest, TwoKnotsStay){
    SplineEditor& editor = SplineEditor::get();
    editor.beginEdit(project.get(), scene->id, spline);
    std::vector<uint32_t> all;
    for (uint32_t i = 0; i < component().spline.knotCount(); i++) all.push_back(i);
    editor.setSelection(all);
    const std::vector<spline::SplineKnot> start = knots();
    std::string error;
    EXPECT_FALSE(editor.deleteSelected(project.get(), &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(editor.getStatus().empty());
    EXPECT_TRUE(sameKnots(knots(), start));
}

TEST_F(SplineEditCmdTest, AStrokeDrawsAndAStrokeFromTheEndExtends){
    // a stroke along X: a CatmullRom spline of a few knots, one "Draw Spline" step
    std::vector<Vector3> stroke;
    for (int i = 0; i <= 100; i++) stroke.push_back(Vector3(0.3f * static_cast<float>(i), 0.0f, std::sin(static_cast<float>(i) * 0.05f)));
    std::string error;
    ASSERT_TRUE(SplineEditor::applyStroke(project.get(), scene->id, spline, stroke, 0.5f, 2, SplineSnap::None, false, 1.0f, &error)) << error;
    EXPECT_NE(history()->top()->name().find("Draw Spline"), std::string::npos);
    EXPECT_EQ(component().spline.type(), spline::SplineType::CatmullRom);
    const std::vector<spline::SplineKnot> drawn = knots();
    ASSERT_GE(drawn.size(), 2u);
    EXPECT_LT(drawn.size(), 30u);

    // from its end: appended, the drawn knots bit-identical
    const Vector3 end = scene->scene->getComponent<Transform>(spline).modelMatrix * drawn.back().position;
    std::vector<Vector3> more;
    for (int i = 0; i <= 40; i++) more.push_back(end + Vector3(0.25f * static_cast<float>(i), 0.0f, 0.1f * static_cast<float>(i)));
    ASSERT_TRUE(SplineEditor::applyStroke(project.get(), scene->id, spline, more, 0.5f, 2, SplineSnap::None, true, 1.0f, &error)) << error;
    const std::vector<spline::SplineKnot> extended = knots();
    ASSERT_GT(extended.size(), drawn.size());
    for (size_t i = 0; i < drawn.size(); i++){
        EXPECT_EQ(extended[i].position, drawn[i].position);
    }
    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(sameKnots(knots(), drawn));
}

TEST_F(SplineEditCmdTest, EveryPresetBuildsItsComponents){
    for (const std::string& name : SplinePresets::names()){
        SplinePreset preset;
        ASSERT_TRUE(SplinePresets::fromName(name, preset));
        const SplinePresetSetup setup = SplinePresets::make(project.get(), preset, false);
        if (preset != SplinePreset::Empty){
            EXPECT_GE(setup.spline.spline.knotCount(), 2u) << name;
        }
        EXPECT_EQ(SplinePresets::is2D(preset), setup.spline.spline.upAxis().z != 0.0f) << name;
        if (preset == SplinePreset::Fence || preset == SplinePreset::Scatter){
            EXPECT_TRUE(setup.hasInstantiate) << name;
            const std::string piece = preset == SplinePreset::Fence ? setup.instantiate.posts[0].asset.path : setup.instantiate.items[0].asset.path;
            EXPECT_TRUE(fs::exists(project->getAssetsPath() / piece)) << piece;
        }
    }
}
