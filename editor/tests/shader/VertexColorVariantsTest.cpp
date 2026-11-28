// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R16: an export's shader list holds the four-value colour stream
// variant ('Vc4') of the submeshes a Vertex Colors component paints, so a game does not build it at
// runtime; a mesh without the component ships none.

#include "shader/ShaderVariantCollector.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/VertexColorsComponent.h"
#include "object/Mesh.h"
#include "pool/ShaderPool.h"
#include "subsystem/MeshSystem.h"

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include <set>
#include <string>
#include <vector>

namespace VertexColorVariantsTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    class VertexColorVariants : public ::testing::Test {
    protected:
        static void SetUpTestSuite(){
            editor::test::ensureEngine();
        }
    };

    // a quad with positions and normals only: no colour of its own
    void buildQuad(Scene& scene, Mesh& quad){
        MeshComponent& mesh = quad.getComponent<MeshComponent>();
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
        mesh.numSubmeshes = 1;
        mesh.buffer.clear();
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
        const Vector3 corners[4] = {Vector3(-1, -1, 0), Vector3(1, -1, 0), Vector3(1, 1, 0), Vector3(-1, 1, 0)};
        for (const Vector3& corner : corners){
            mesh.buffer.addVector3(position, corner);
            mesh.buffer.addVector3(normal, Vector3(0, 0, 1));
        }
        const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), 6, (char*)indices, sizeof(uint16_t));
        scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);
    }

    // the MESH variant names of a scene's export that draw a colour stream of four values
    std::set<std::string> vc4Variants(Scene& scene, size_t& meshKeys){
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        std::set<std::string> names;
        meshKeys = 0;
        for (ShaderKey key : keys){
            if (ShaderPool::getShaderTypeFromKey(key) != ShaderType::MESH)
                continue;
            meshKeys++;
            const std::string name = ShaderPool::getShaderStr(ShaderType::MESH, ShaderPool::getPropertiesFromKey(key), 0);
            if (name.find("Vc4") != std::string::npos)
                names.insert(name);
        }
        return names;
    }

}

TEST_F(VertexColorVariants, PaintedSubmeshesShipTheColourStreamVariant){
    Scene scene;
    Mesh quad(&scene);
    buildQuad(scene, quad);

    size_t plainKeys = 0;
    EXPECT_TRUE(vc4Variants(scene, plainKeys).empty()) << "a mesh without colours and without the component draws none";
    ASSERT_GT(plainKeys, 0u);

    scene.addComponent<VertexColorsComponent>(quad.getEntity(), VertexColorsComponent());
    size_t paintedKeys = 0;
    const std::set<std::string> painted = vc4Variants(scene, paintedKeys);
    EXPECT_FALSE(painted.empty()) << "R16: the Vc4 variant of the painted submesh";
    EXPECT_EQ(paintedKeys, plainKeys + painted.size()) << "the plain variants stay (the component can be off at runtime)";
}

}  // namespace VertexColorVariantsTest_cpp
