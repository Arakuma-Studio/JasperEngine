// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Simulation Culling Inspector (docs/specs/culling-group-and-simulation-distance.md R25, R26,
// AC19): the reflected rows the default Inspector draws, with Unity-like labels and the Cull
// foldout; Reduced Update Interval hidden without a reduced band; the runtime state out of the
// rows (the drawer shows it in Play); one PropertyCmd edit is one undo step, two GameObjects at
// once one step too; and the Device Tier cell edits and resets.

#include "Project.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/SimulationCullingComponent.h"
#include "device/DeviceSettings.h"
#include "reflect/Reflect.h"
#include "window/dialog/ComponentAddDialog.h"
#include "command/UndoFakeCommand.h"
#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace inspector_simulation_culling_test {

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;


    namespace fs = std::filesystem;

    TEST(InspectorSimulationCulling, RowsAndLabels) {
        const reflect::ComponentDesc& desc = *reflect::find(ComponentType::SimulationCullingComponent);
        EXPECT_STREQ(desc.displayName, "Simulation Culling");
        EXPECT_TRUE(ComponentAddDialog::isListed(ComponentType::SimulationCullingComponent));

        // the rows in order, with their labels and foldout
        const std::vector<std::pair<std::string, std::string>> rows = {
            {"simulationDistance", "Simulation Distance"}, {"reducedRateDistance", "Reduced Rate Distance"},
            {"reducedUpdateInterval", "Reduced Update Interval"}, {"hysteresis", "Hysteresis"}, {"boundsRadius", "Bounds Radius"},
            {"simulateWhenVisible", "Simulate When Visible"}, {"freezeWhenGroundNotLoaded", "Freeze When Ground Not Loaded"},
            {"cullScripts", "Scripts"}, {"cullAnimator", "Animator"}, {"cullPhysics", "Physics"}};
        for (const auto& [name, label] : rows) {
            const reflect::FieldDesc* field = reflect::findField(desc, name);
            ASSERT_NE(field, nullptr) << name;
            EXPECT_EQ(reflect::label(*field), label) << name;
            EXPECT_FALSE(field->flags & reflect::Hidden) << name;
            const bool cull = name.rfind("cull", 0) == 0;
            EXPECT_EQ(field->group != nullptr && std::string(field->group) == "Cull", cull) << name;
        }
        EXPECT_NE(std::string(reflect::findField(desc, "boundsRadius")->tooltip).find("0 = automatic"), std::string::npos);

        // Reduced Update Interval shows only with a reduced band
        const reflect::FieldDesc* interval = reflect::findField(desc, "reducedUpdateInterval");
        ASSERT_NE(interval->visibleIf, nullptr);
        SimulationCullingComponent component;
        EXPECT_FALSE(interval->visibleIf(&component));
        component.reducedRateDistance = 20.0f;
        EXPECT_TRUE(interval->visibleIf(&component));

        // the runtime state: not saved, not a row, not written by MCP
        for (const char* name : {"state", "reason", "focusDistance"}) {
            const reflect::FieldDesc* field = reflect::findField(desc, name);
            ASSERT_NE(field, nullptr) << name;
            EXPECT_TRUE(field->flags & reflect::NoSave) << name;
            EXPECT_TRUE(field->flags & reflect::Hidden) << name;
            EXPECT_TRUE(field->flags & reflect::ReadOnly) << name;
        }
        // nothing runtime in a saved component
        component.state = SimulationState::Culled;
        const YAML::Node encoded = reflect::encode(desc, &component, reflect::EncodeContext{});
        EXPECT_FALSE(encoded["state"].IsDefined()) << YAML::Dump(encoded);
        EXPECT_FALSE(encoded["focusDistance"].IsDefined());
    }

    class InspectorSimulationCullingProject : public ::testing::Test {
    protected:
        UndoStatics statics;
        jasperengine::test::TestTempDir dir{"simculling"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;
        uint32_t sceneId = 0;
        Entity a = NULL_ENTITY, b = NULL_ENTITY;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) project->loadScene(candidate.filepath, false, false, true);
            }
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            sceneId = scene->id;
            project->setSelectedSceneId(sceneId);
            a = 1001;
            b = 1002;
            scene->scene->addComponent<SimulationCullingComponent>(a, SimulationCullingComponent{});
            scene->scene->addComponent<SimulationCullingComponent>(b, SimulationCullingComponent{});
        }

        void TearDown() override {
            if (project) closeProject(*project);
            project.reset();
        }

        CommandHistory* history() { return CommandHandle::get(sceneId); }
        SimulationCullingComponent& culling(Entity entity) { return scene->scene->getComponent<SimulationCullingComponent>(entity); }
    };

    TEST_F(InspectorSimulationCullingProject, RowsAndUndo) {
        // one edit, one undo step
        ASSERT_TRUE(history()->addCommand(new PropertyCmd<float>(project.get(), sceneId, a, ComponentType::SimulationCullingComponent,
                                                                "simulationDistance", 80.0f)));
        EXPECT_FLOAT_EQ(culling(a).simulationDistance, 80.0f);
        ASSERT_TRUE(history()->undo());
        EXPECT_FLOAT_EQ(culling(a).simulationDistance, 64.0f);
        EXPECT_FALSE(history()->canUndo());

        // two GameObjects edited at once: one step
        auto* multi = new MultiPropertyCmd();
        multi->addPropertyCmd<bool>(project.get(), sceneId, a, ComponentType::SimulationCullingComponent, "cullPhysics", false);
        multi->addPropertyCmd<bool>(project.get(), sceneId, b, ComponentType::SimulationCullingComponent, "cullPhysics", false);
        ASSERT_TRUE(history()->addCommand(multi));
        EXPECT_FALSE(culling(a).cullPhysics);
        EXPECT_FALSE(culling(b).cullPhysics);
        ASSERT_TRUE(history()->undo());
        EXPECT_TRUE(culling(a).cullPhysics);
        EXPECT_TRUE(culling(b).cullPhysics);
        EXPECT_FALSE(history()->canUndo());
    }

    // R26: the Device Tier table's Simulation Distance row: a cell edited, then reset to the defaults
    TEST(InspectorSimulationCulling, DeviceTierCellEditsAndResets) {
        DeviceSettings settings;
        const DeviceSettings defaults;
        DeviceTierOverrides& low = settings.tier(DevicePlatformGroup::Mobile, DeviceTier::Low);
        ASSERT_TRUE(low.get(DeviceOverride::SimulationDistance).has_value());
        std::string error;
        float value = 0.0f;
        ASSERT_TRUE(DeviceOverrides::parse(DeviceOverride::SimulationDistance, "0.5", value, error)) << error;
        low.set(DeviceOverride::SimulationDistance, value);
        EXPECT_NE(settings, defaults);
        EXPECT_NE(settings.toJson().find("simulation_distance"), std::string::npos);
        // the row's reset puts every tier's default back
        for (int t = 0; t < DEVICE_TIER_COUNT; t++) {
            settings.tiers[(size_t)DevicePlatformGroup::Mobile][t].set(DeviceOverride::SimulationDistance,
                defaults.tiers[(size_t)DevicePlatformGroup::Mobile][t].get(DeviceOverride::SimulationDistance));
        }
        EXPECT_EQ(settings, defaults);
        EXPECT_EQ(DeviceOverrides::format(DeviceOverride::SimulationDistance, 0.5f), "0.5");
    }

}
