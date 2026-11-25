// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's Profiler.gpuTiming (spec S98 REQ-26, AC33): it holds the GPU timer lease get_gpu_passes
// uses, frameStats() then carries gpuMs and the passes' GPU times, and false lets the timer go.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "render/SystemRender.h"
#include "script/LuaBinding.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "Readback.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::string runProfilerLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    class ProfilerLuaGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Primitive plane{&scene};

        void SetUp() override{
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 3.0f, 5.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            plane.createPlane(4.0f, 4.0f);
            plane.setColor(0.0f, 1.0f, 0.0f);
            plane.setReceiveLights(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            if (lua_State* L = LuaBinding::getLuaState())
                runProfilerLua(L, "Profiler.gpuTiming = false");
            Engine::removeScene(&scene);
        }
    };

}

TEST_F(ProfilerLuaGpu, GpuTimingArmsTimers){
    JASPER_REQUIRE_GPU();
    Scene* current = &scene;
    ASSERT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }, 5000));
    const GpuTimingState idle = SystemRender::getGpuTimingState();
    ASSERT_TRUE(idle.available) << "GPU timing unavailable: " << idle.reason;
    EXPECT_FALSE(idle.enabled);

    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    EXPECT_EQ(runProfilerLua(L, R"(
        assert(Profiler.gpuTiming == false, "off by default")
        assert(Profiler.frameStats().gpuPasses == nil, "no passes while off")
        Profiler.gpuTiming = true
        assert(Profiler.gpuTiming == true, "on once set")
    )"), "");
    EXPECT_TRUE(SystemRender::getGpuTimingState().enabled);

    // 10 frames, each finished on the GPU so its times resolve on the next commit
    for (int i = 0; i < 10; i++){
        GpuEnvironment::drawFrames(1);
#if defined(JASPER_TEST_HAS_GPU)
        ASSERT_TRUE(finishGpu());
#endif
    }
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(runProfilerLua(L, R"(
        local s = Profiler.frameStats()
        assert(type(s.gpuMs) == "number" and s.gpuMs >= 0, "gpuMs: " .. tostring(s.gpuMs))
        assert(type(s.gpuPasses) == "table" and #s.gpuPasses > 0, "gpuPasses: " .. tostring(s.gpuPasses))
        for _, pass in ipairs(s.gpuPasses) do
            assert(type(pass.name) == "string" and #pass.name > 0, "a pass name")
            assert(type(pass.ms) == "number" and pass.ms >= 0, "a pass time")
        end
    )"), "");

    // false drops the lease: the next commits time nothing, and the passes are gone
    EXPECT_EQ(runProfilerLua(L, R"(
        Profiler.gpuTiming = false
        assert(Profiler.gpuTiming == false, "off once cleared")
        assert(Profiler.frameStats().gpuPasses == nil, "no passes once off")
    )"), "");
    GpuEnvironment::drawFrames(2);
    EXPECT_FALSE(SystemRender::getGpuTimingState().enabled);
}
