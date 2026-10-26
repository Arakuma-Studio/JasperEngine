// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What MCP reads from the reflection tables (docs/specs/S08-reflection.md R24, R25, R27), for every
// component type: its JSON Schema and per-field metadata, its properties' defaults as
// list_component_types gives them, and its properties' values and defaults as get_entity detail
// "full" gives them on a GameObject that has it; and the jasper://reflection document.

#include "Catalog.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "mcp/McpValues.h"
#include "reflect/Reflect.h"
#include "util/ProjectUtils.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

// (a named namespace, tests included: a unity build merges the test sources)
namespace reflect_mcp_read_test {

    TEST(ReflectMcp, EveryComponentDescribesAndReads) {
        jasperengine::test::TestTempDir dir{"reflect_mcp_read"};
        fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        auto project = std::make_unique<Project>();
        project->setSkipProjectLock(true);
        ASSERT_TRUE(project->loadProject(dir.path(), false));
        SceneProject* scene = nullptr;
        for (SceneProject& candidate : project->getScenes()) {
            if (!candidate.filepath.empty() && !candidate.scene) project->loadScene(candidate.filepath, false, false, true);
        }
        for (SceneProject& candidate : project->getScenes()) {
            if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
        }
        ASSERT_NE(scene, nullptr);

        for (int t = 0; t <= static_cast<int>(LAST_COMPONENT_TYPE); t++) {
            const ComponentType type = static_cast<ComponentType>(t);
            const reflect::ComponentDesc* desc = reflect::find(type);
            ASSERT_NE(desc, nullptr) << t;
            SCOPED_TRACE(desc->cppName);

            const Json schema = reflect::schema(*desc);
            EXPECT_EQ(schema.value("type", std::string()), "object");
            for (const reflect::FieldDesc& field : desc->body.fields) {
                EXPECT_TRUE(reflect::fieldMetadata(field).contains("label")) << field.name;
            }

            // list_component_types {component}
            mcp::McpValues::ReadContext defaults;
            defaults.project = project.get();
            defaults.component = type;
            defaults.hasComponent = false;
            for (const auto& [name, property] : Catalog::getProperties(type, nullptr)) {
                if (property.def && property.type != PropertyType::Custom) (void)mcp::McpValues::readProperty(defaults, name, property, true);
                (void)mcp::McpValues::enumOptions(type, name);
            }

            // get_entity detail "full" on a GameObject with it
            const Entity entity = scene->scene->createEntity();
            scene->entities.push_back(entity);
            if (type != ComponentType::Transform) ProjectUtils::addEntityComponent(scene->scene, entity, type, scene->entities);
            mcp::McpValues::ReadContext live;
            live.project = project.get();
            live.scene = scene;
            live.entity = entity;
            live.component = type;
            for (const auto& [name, property] : Catalog::findEntityProperties(scene->scene, entity, type)) {
                if (property.ref || property.type == PropertyType::Custom) (void)mcp::McpValues::readProperty(live, name, property);
                if (property.def && property.type != PropertyType::Custom) (void)mcp::McpValues::readProperty(live, name, property, true);
                if (const reflect::FieldDesc* field = reflect::findField(*desc, name)) (void)reflect::fieldMetadata(*field);
            }
        }

        const Json document = reflect::reflectionDocument();
        EXPECT_EQ(document["components"].size(), static_cast<size_t>(LAST_COMPONENT_TYPE) + 1);
        EXPECT_TRUE(document.contains("enums") && document.contains("post_processing"));

        for (SceneProject& candidate : project->getScenes()) CommandHandle::remove(candidate.id);
    }

}
