// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SaveGame's budgets (docs/specs/savegame.md R26-R28; AC19), printed as [bench] lines and enforced
// under JASPER_TEST_PERF_SLACK (docs/testing.md).

#include "jobs/JobSystem.h"
#include "save/SaveGameStore.h"
#include "save/SaveValue.h"
#include "script/LuaBinding.h"

#include "support/AllocationCounter.h"
#include "support/TestTempDir.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace SaveGameBenchTest_cpp {

using namespace jasperengine;
using jasperengine::test::TestTempDir;

namespace {

    template<typename Fn>
    double saveBenchMedianMs(int runs, Fn&& fn){
        std::vector<double> times;
        for (int i = 0; i < runs; i++){
            const auto start = std::chrono::steady_clock::now();
            fn(i);
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    }

    void saveBenchBudget(const char* what, double ms, double budget){
        const double slack = testsupport::perfSlack();
        std::printf("[ bench    ] SaveGame %s: %.3f ms (budget %.1f ms)\n", what, ms, budget);
        if (slack > 0.0) {
            EXPECT_LE(ms, budget * slack) << what;
        }
    }

    // A table of 10,000 entries, about 256 KiB encoded, as the global __saveBench
    void makeBenchTable(lua_State* L){
        ASSERT_EQ(luaL_dostring(L, R"(
            local t = {}
            for i = 1, 5000 do t[i] = "item-" .. string.format("%015d", i) end
            for i = 1, 5000 do t["key" .. i] = {i, i * 0.5} end
            __saveBench = t
        )"), LUA_OK) << lua_tostring(L, -1);
    }

}

TEST(SaveGameBench, Encode10kEntries){
    lua_State* L = LuaBinding::getLuaState();
    makeBenchTable(L);
    lua_getglobal(L, "__saveBench");
    std::string payload;
    const double ms = saveBenchMedianMs(21, [&](int){
        payload.clear();
        savegame::SaveError error;
        ASSERT_TRUE(savegame::encodeLua(L, -1, payload, error)) << error.message;
    });
    lua_pop(L, 1);
    std::printf("[ bench    ] SaveGame payload of 10,000 entries: %zu bytes\n", payload.size());
    EXPECT_GT(payload.size(), 200u * 1024u);
    saveBenchBudget("encode 10,000 entries", ms, 3.0);
}

TEST(SaveGameBench, Decode10kEntries){
    lua_State* L = LuaBinding::getLuaState();
    makeBenchTable(L);
    lua_getglobal(L, "__saveBench");
    std::string payload;
    savegame::SaveError error;
    ASSERT_TRUE(savegame::encodeLua(L, -1, payload, error));
    lua_pop(L, 1);
    const double ms = saveBenchMedianMs(21, [&](int){
        ASSERT_TRUE(savegame::decodeLua(L, payload, error)) << error.message;
        lua_pop(L, 1);
    });
    saveBenchBudget("decode 10,000 entries", ms, 4.0);
}

TEST(SaveGameBench, SmallNamespaceInLargeSlot){
    TestTempDir dir("savegame-bench");
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    store.setFolder(dir.path() / "saves");
    // the Craftsman scenario: 2.7 MiB in 30 namespaces, then a 2 KiB one written again and again
    std::string big;
    savegame::SaveError error;
    ASSERT_TRUE(savegame::fromJson(nlohmann::json(std::string(92 * 1024, 'w')), big, error));
    for (int i = 0; i < 30; i++) {
        ASSERT_TRUE(store.write("big", "part" + std::to_string(i), big, savegame::WriteOptions(), nullptr).ok());
    }
    ASSERT_TRUE(store.flush("big").ok());
    std::string small;
    ASSERT_TRUE(savegame::fromJson(nlohmann::json(std::string(2 * 1024, 's')), small, error));
    // the main thread's part only: the file writes wait while the queue is held
    store.setHold(true);
    const double ms = saveBenchMedianMs(51, [&](int){
        ASSERT_TRUE(store.write("big", "player", small, savegame::WriteOptions(), nullptr).ok());
    });
    store.setHold(false);
    ASSERT_TRUE(store.flush("big").ok());
    jobs::drainMainQueue();
    saveBenchBudget("2 KiB namespace into a 2.7 MiB slot (main thread)", ms, 0.3);
    store.setFolder({});
}

TEST(SaveGameBench, List50Slots){
    TestTempDir dir("savegame-bench");
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    store.setFolder(dir.path() / "saves");
    std::string payload;
    savegame::SaveError error;
    ASSERT_TRUE(savegame::fromJson(nlohmann::json::parse(R"({"level":"forest"})"), payload, error));
    std::string blob;
    ASSERT_TRUE(savegame::fromJson(nlohmann::json(std::string(64 * 1024, 'b')), blob, error));
    savegame::WriteOptions options;
    options.hasMeta = true;
    ASSERT_TRUE(savegame::fromJson(nlohmann::json::parse(R"({"title":"Slot"})"), options.meta, error));
    for (int i = 0; i < 50; i++){
        ASSERT_TRUE(store.write("slot" + std::to_string(i), "main", payload, options, nullptr).ok());
        ASSERT_TRUE(store.write("slot" + std::to_string(i), "world", blob, savegame::WriteOptions(), nullptr).ok());
    }
    ASSERT_TRUE(store.flush().ok());
    jobs::drainMainQueue();
    size_t count = 0;
    const double ms = saveBenchMedianMs(11, [&](int){
        store.unload();
        count = store.list().size();
    });
    EXPECT_EQ(count, 50u);
    saveBenchBudget("list 50 slots (headers only)", ms, 5.0);
    store.setFolder({});
}

}  // namespace SaveGameBenchTest_cpp
