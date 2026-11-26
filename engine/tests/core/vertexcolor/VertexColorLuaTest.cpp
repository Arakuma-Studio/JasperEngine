// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R125, R80, R99: Lua's VertexColors through a real state on a box
// primitive (CPU only): targets, applyOps all or nothing with 1-based op numbers, getValues' cap,
// setValues, stats, sampleAt, paint; and the cost of one runtime paint on 10k vertices.

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/VertexColorsComponent.h"
#include "object/Mesh.h"
#include "object/Primitive.h"
#include "object/VertexColors.h"
#include "script/LuaBinding.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/VertexColorScene.h"
#include "support/AllocationCounter.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_lua_test {
    using namespace jasperengine::vertexcolor;

    class VertexColorLua : public ::testing::Test {
    protected:
        Scene scene;
        Primitive box{&scene};
        lua_State* L = nullptr;

        void SetUp() override {
            box.createBox(1.0f, 1.0f, 1.0f);
            VertexColorsComponent component;
            component.initialValue = InitialValue::Black;
            component.channelNames = {"Moss", "", "", "Wet"};
            scene.addComponent<VertexColorsComponent>(box.getEntity(), component);
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
            ASSERT_TRUE(luabridge::push(L, VertexColors(&scene, box.getEntity())));
            lua_setglobal(L, "vc");
        }

        void TearDown() override {
            lua_pushnil(L);
            lua_setglobal(L, "vc");
            lua_gc(L, LUA_GCCOLLECT, 0);
        }

        void run(const char* code) {
            ASSERT_EQ(luaL_dostring(L, code), 0) << lua_tostring(L, -1);
        }

        // the error a chunk raises ("" when it runs)
        std::string failure(const char* code) {
            if (luaL_dostring(L, code) == 0) return "";
            std::string message = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
            lua_pop(L, 1);
            return message;
        }

        double number(const char* name) {
            lua_getglobal(L, name);
            const double value = lua_tonumber(L, -1);
            lua_pop(L, 1);
            return value;
        }

        bool boolean(const char* name) {
            lua_getglobal(L, name);
            const bool value = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return value;
        }

        std::string text(const char* name) {
            lua_getglobal(L, name);
            const char* value = lua_tostring(L, -1);
            std::string out = value ? value : "";
            lua_pop(L, 1);
            return out;
        }

        const VertexColorsComponent& component() {
            return scene.getComponent<VertexColorsComponent>(box.getEntity());
        }
    };

TEST_F(VertexColorLua, PropertiesAndTargets) {
    run(R"(
        valid = vc.valid
        enabled = vc.enabled
        format = vc.format
        moss = vc.channelNames[1]
        wet = vc.channelNames[4]
        count = #vc.targets
        submesh = vc.targets[1].submesh
        vertices = vc.targets[1].vertexCount
        vc.enabled = false
    )");
    EXPECT_TRUE(boolean("valid"));
    EXPECT_TRUE(boolean("enabled"));
    EXPECT_EQ(text("format"), "rgba8");
    EXPECT_EQ(text("moss"), "Moss");
    EXPECT_EQ(text("wet"), "Wet");
    EXPECT_EQ(number("count"), 1.0);
    EXPECT_EQ(number("submesh"), 0.0);
    EXPECT_EQ(number("vertices"), 24.0);
    EXPECT_FALSE(component().enabled);
    EXPECT_TRUE(component().needUpdate);
}

TEST_F(VertexColorLua, ApplyOpsIsAllOrNothingWithOneBasedErrors) {
    const std::string error = failure(R"(
        vc:applyOps({
            { op = "set", target = { kind = "all" }, value = {1, 0, 0} },
            { op = "set", target = { kind = "vertices", ids = {99999} }, value = {0, 1, 0} },
        })
    )");
    EXPECT_NE(error.find("VertexColors:applyOps"), std::string::npos) << error;
    EXPECT_NE(error.find("ops[2]"), std::string::npos) << "Lua's op numbers are 1-based: " << error;
    EXPECT_TRUE(!component().loaded || component().loaded->targets.empty()) << "nothing written";

    run(R"(
        result = vc:applyOps({ { op = "set", target = { kind = "all" }, value = {1, 0.5, 0} } })
        results = #result
        target = result[1].target.submesh
        local v = vc:getValue(1, 0)
        r, g, b, a = v.x, v.y, v.z, v.w
        local same = vc:getValue({submesh = 0}, 0)
        sameR = same.x
    )");
    EXPECT_EQ(number("results"), 1.0);
    EXPECT_EQ(number("target"), 0.0);
    EXPECT_NEAR(number("r"), 1.0, 1e-6);
    EXPECT_NEAR(number("g"), 128.0 / 255.0, 1e-6) << "RGBA8 stores 1/255 steps";
    EXPECT_NEAR(number("b"), 0.0, 1e-6);
    EXPECT_NEAR(number("a"), 1.0, 1e-6) << "rgb values write rgb: the (opaque) black initial value's alpha stays";
    EXPECT_NEAR(number("sameR"), 1.0, 1e-6);
    ASSERT_TRUE(component().loaded);
    EXPECT_EQ(component().loaded->targets.size(), 1u);

    EXPECT_NE(failure("vc:applyOps({ { op = 'set', target = { kind = 'all' }, value = {1, 0, 0} } }, {submesh = 3})").find("no target submesh 3"),
              std::string::npos);
    EXPECT_NE(failure("vc:applyOps({}, 'x')").find("the target must be"), std::string::npos);
}

TEST_F(VertexColorLua, ValuesStatsAndSampling) {
    EXPECT_NE(failure("vc:getValues(1, 0, 65537)").find("over the cap of 65536"), std::string::npos);
    EXPECT_NE(failure("vc:getValues(1, 20, 5)").find("past the target's 24 vertices"), std::string::npos);
    EXPECT_NE(failure("vc:getValue(2, 0)").find("not one of the 1 targets"), std::string::npos);
    run(R"(
        local before = vc:getValues(1, 0, 4)
        initialCount = #before
        initialR = before[1].x
        vc:setValues(1, 2, { Vector4(0.25, 0.5, 0.75, 1), {1, 1, 1} })
        local after = vc:getValues(1, 2, 2)
        setR, setA = after[1].x, after[2].w
        local stats = vc:stats(1)
        statsCount = stats.count
        statsMax = stats.max[1]
    )");
    EXPECT_EQ(number("initialCount"), 4.0);
    EXPECT_EQ(number("initialR"), 0.0) << "the Black initial value";
    EXPECT_NEAR(number("setR"), 64.0 / 255.0, 1e-6);
    EXPECT_NEAR(number("setA"), 1.0, 1e-6) << "{r, g, b} is opaque";
    EXPECT_EQ(number("statsCount"), 36.0) << "every triangle corner";
    EXPECT_NEAR(number("statsMax"), 1.0, 1e-6);

    // the colour at the surface point nearest to a world point: vertex 3 holds white
    std::shared_ptr<const MeshView> view = sceneMeshView(&scene, box.getEntity(), component().loaded->targets[0].key);
    ASSERT_TRUE(view);
    const Vector3 corner = view->positions[3];
    lua_pushnumber(L, corner.x);
    lua_setglobal(L, "cx");
    lua_pushnumber(L, corner.y);
    lua_setglobal(L, "cy");
    lua_pushnumber(L, corner.z);
    lua_setglobal(L, "cz");
    run("local v = vc:sampleAt(Vector3(cx * 1.2, cy * 1.2, cz * 1.2)); sampled = v and v.x or -1");
    EXPECT_GE(number("sampled"), 0.0) << "found a surface point";
    EXPECT_NE(failure("vc:sampleAt(3)").find("a world point (Vector3) or a MeshRaycast hit"), std::string::npos);
}

TEST_F(VertexColorLua, PaintIsALayerOverTheAuthoredValues) {
    run(R"(
        touched = vc:paint(Vector3(0.5, 0.5, 0.5), { radius = 0.3, value = {0, 1, 0}, channels = "g" })
        missed = vc:paint(Vector3(9, 9, 9), { radius = 0.3, value = {0, 1, 0} })
        local v = vc:sampleAt(Vector3(0.5, 0.5, 0.5))
        green = v.y
    )");
    EXPECT_EQ(number("touched"), 3.0) << "the corner's three face vertices";
    EXPECT_EQ(number("missed"), 0.0);
    EXPECT_NEAR(number("green"), 1.0, 1e-5);
    EXPECT_FALSE(component().loaded && !component().loaded->targets.empty()) << "R80: the authored values are not written";
    ASSERT_EQ(component().runtimeLayers.size(), 1u);
    run("vc:clearRuntime()");
    EXPECT_TRUE(component().runtimeLayers.empty());
    EXPECT_NE(failure("vc:paint(Vector3(0, 0, 0), { radius = -1 })").find("radius must be a number above 0"), std::string::npos);
    EXPECT_NE(failure("vc:paint(Vector3(0, 0, 0), { channels = 'xyz' })").find("opts.channels"), std::string::npos);
}

// R99: one runtime paint on a 10k-vertex mesh, ≤ 0.3 ms on the mobile reference device; here the
// desktop's time, enforced with perfSlack()
TEST(VertexColorRuntimeBench, PaintOnTenThousandVertices) {
    Scene scene;
    Mesh grid(&scene);
    constexpr int CELLS = 100;   // 101 x 101 = 10,201 vertices
    MeshComponent& mesh = grid.getComponent<MeshComponent>();
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;
    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    for (int j = 0; j <= CELLS; j++) {
        for (int i = 0; i <= CELLS; i++) {
            mesh.buffer.addVector3(position, Vector3((float)i / CELLS * 10.0f, 0.0f, (float)j / CELLS * 10.0f));
            mesh.buffer.addVector3(normal, Vector3(0.0f, 1.0f, 0.0f));
        }
    }
    std::vector<uint16_t> indices;
    for (int j = 0; j < CELLS; j++) {
        for (int i = 0; i < CELLS; i++) {
            const uint16_t a = (uint16_t)(j * (CELLS + 1) + i), b = (uint16_t)(a + 1), c = (uint16_t)(a + CELLS + 1), d = (uint16_t)(c + 1);
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    mesh.indices.clear();
    mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), (char*)indices.data(), sizeof(uint16_t));
    scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);
    scene.addComponent<VertexColorsComponent>(grid.getEntity(), VertexColorsComponent());
    VertexColors colors(&scene, grid.getEntity());
    const std::vector<SceneTarget> targets = colors.getTargets();
    ASSERT_EQ(targets.size(), 1u);
    ASSERT_EQ(targets[0].vertexCount, 10201u);

    RuntimePaint paint;
    paint.radius = 0.6f;
    paint.hold = 1.0f;
    paint.fade = 1.0f;
    std::string error;
    ASSERT_GT(colors.paint(Vector3(5.0f, 0.0f, 5.0f), paint, error), 0u) << error;   // warm: the view is built once
    std::vector<double> times;
    for (int i = 0; i < 200; i++) {
        const Vector3 at((float)(i % 20) * 0.5f, 0.0f, (float)(i / 20) * 1.0f);
        const auto start = std::chrono::steady_clock::now();
        colors.paint(at, paint, error);
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    const double median = times[times.size() / 2];
    std::printf("[bench] VertexColors:paint on 10,201 vertices: median %.4f ms, p95 %.4f ms (R99: 0.3 ms on the mobile device)\n",
                median, times[times.size() * 95 / 100]);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(median, 0.3 * testsupport::perfSlack());
    }
}

} // namespace vertex_color_lua_test
