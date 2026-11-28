// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Gameplay Journal (diag/Journal.h, docs/specs/shipped-game-diagnostics.md AC1-AC4): the
// entry format, the ring keeping the newest whole entries, a journal that is off, and writes
// without heap allocation. No crash handler is installed in this binary, so configure() may set
// the ring again for each test.

#include "diag/Journal.h"
#include "AllocationCounter.h"
#include "LogCapture.h"

#include "Log.h"
#include "script/LuaBinding.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace JournalTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;
namespace journal = jasperengine::diag::journal;

namespace {

    // the entry without its "+<seconds> f<frame> " prefix
    std::string body(const std::string& line) {
        const size_t frame = line.find(" f");
        const size_t kind = line.find(' ', frame + 2);
        return line.substr(kind + 1);
    }

    struct JournalScope {
        explicit JournalScope(size_t bytes) { journal::configure(bytes); }
        ~JournalScope() { journal::configure(0); }
    };

}

TEST(Journal, FormatsKindAndSortedFields) {
    JournalScope scope(16 << 10);
    const journal::Field reversed[] = {
        {"zeta", std::string_view("needs quotes \"and\" \\ back\nslash")},
        {"seed", int64_t(42)},
        {"ratio", 0.1},
        {"name", std::string_view("forest-2")},
        {"hard", true},
        {"alpha", false},
    };
    journal::log("level.start", reversed);
    const std::vector<std::string> lines = journal::entries(1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_TRUE(std::regex_match(lines[0], std::regex(R"(\+\d+\.\d{3} f\d+ .*)"))) << lines[0];
    EXPECT_EQ(body(lines[0]),
              "level.start alpha=false hard=true name=forest-2 ratio=0.1 seed=42 "
              "zeta=\"needs quotes \\\"and\\\" \\\\ back\\nslash\"");

    // numbers as %.9g, whichever path writes them
    const std::pair<double, const char*> numbers[] = {{87.5, "87.5"}, {1.0 / 3, "0.333333333"}, {123456789.0, "123456789"}, {1e9, "1e+09"},
        {1e-5, "1e-05"}, {-2.5, "-2.5"}, {0.00012345678912, "0.000123456789"}, {999999999.5, "1e+09"}, {99999999.95, "100000000"},
        {12345.678901234, "12345.6789"}, {1e300, "1e+300"}, {-0.0, "-0"}, {0.5, "0.5"}, {100.0, "100"}};
    for (const auto& [value, text] : numbers) {
        const journal::Field field[] = {{"v", value}};
        journal::log("n", field);
        EXPECT_EQ(body(journal::entries(1)[0]), std::string("n v=") + text) << value;
    }

    // a long entry ends in '~' at 256 bytes
    const std::string longText(300, 'x');
    const journal::Field big[] = {{"text", std::string_view(longText)}};
    journal::log("long", big);
    const std::string cut = journal::entries(1)[0];
    EXPECT_EQ(cut.size(), journal::kMaxEntryBytes);
    EXPECT_EQ(cut.back(), '~');

    // a bad kind is kept as invalid, with one warning per process
    LogCapture log;
    journal::log("bad kind!");
    journal::log("bad kind!");
    EXPECT_EQ(body(journal::entries(1)[0]), "invalid kind=\"bad kind!\"");
    EXPECT_EQ(log.count(D_LOG_WARN, "Journal.log: kind \"bad kind!\" is not [A-Za-z0-9_.:-]{1,48}"), 1u);
    EXPECT_EQ(log.count(D_LOG_WARN, "Journal.log"), 1u);

    // at most 12 fields: the smallest keys
    std::vector<std::string> keys;
    for (int i = 0; i < 20; i++) {
        char key[8];
        std::snprintf(key, sizeof(key), "k%02d", 19 - i);
        keys.push_back(key);
    }
    std::vector<journal::Field> many;
    for (const std::string& key : keys) many.push_back({key, int64_t(1)});
    journal::log("many", many);
    EXPECT_EQ(body(journal::entries(1)[0]), "many k00=1 k01=1 k02=1 k03=1 k04=1 k05=1 k06=1 k07=1 k08=1 k09=1 k10=1 k11=1");
}

TEST(Journal, RingKeepsTheNewestWholeEntries) {
    JournalScope scope(4 << 10);
    for (int i = 0; i < 1000; i++) {
        const journal::Field fields[] = {{"i", int64_t(i)}};
        journal::log("entry", fields);
    }
    const std::string text = journal::text();
    ASSERT_EQ(text.rfind("JasperEngine journal v1\n", 0), 0u) << text.substr(0, 200);
    EXPECT_NE(text.find("\ncapacity: 4096\n"), std::string::npos);
    const size_t separator = text.find("\n---\n");
    ASSERT_NE(separator, std::string::npos);

    std::vector<std::string> lines;
    size_t at = separator + 5;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        ASSERT_NE(end, std::string::npos) << "the text ends with a whole line";
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    ASSERT_FALSE(lines.empty());
    int expected = -1;
    for (const std::string& line : lines) {
        ASSERT_TRUE(std::regex_match(line, std::regex(R"(\+\d+\.\d{3} f\d+ entry i=\d+)"))) << line;
        const int i = std::stoi(line.substr(line.find("i=") + 2));
        if (expected >= 0) {
            EXPECT_EQ(i, expected) << "consecutive, oldest first";
        }
        expected = i + 1;
    }
    EXPECT_EQ(expected, 1000) << "the last is entry 999";
    EXPECT_EQ(journal::dropped(), 1000u - lines.size());
    EXPECT_NE(text.find("\ndropped: " + std::to_string(1000 - lines.size()) + "\n"), std::string::npos);

    const std::vector<std::string> newest = journal::entries(5);
    ASSERT_EQ(newest.size(), 5u);
    for (int i = 0; i < 5; i++) { EXPECT_EQ(body(newest[i]), "entry i=" + std::to_string(995 + i)); }

    journal::clear();
    EXPECT_TRUE(journal::entries(5).empty());
    EXPECT_EQ(journal::dropped(), 0u);
    EXPECT_EQ(journal::text().find("\n---\n") + 5, journal::text().size()) << "only the header";
}

TEST(Journal, ZeroSizeAllocatesNothing) {
    JournalScope scope(0);
    EXPECT_FALSE(journal::enabled());
    const journal::Field fields[] = {{"name", std::string_view("forest-2")}, {"seed", int64_t(42)}};
    {
        testsupport::AllocationCountScope allocations;
        for (int i = 0; i < 1000; i++) journal::log("level.start", fields);
        if (testsupport::countsAllocations()) {
            EXPECT_EQ(allocations.count(), 0u);
        }
    }
    EXPECT_TRUE(journal::text().empty());
    EXPECT_TRUE(journal::entries(10).empty());
}

TEST(Journal, LogDoesNotAllocate) {
    JournalScope scope(32 << 10);
    const journal::Field fields[] = {
        {"level", std::string_view("forest-2")},
        {"weapon", std::string_view("bow of the north")},
        {"seed", int64_t(123456)},
        {"hp", 87.5},
    };
    journal::log("warm", fields);
    testsupport::ThreadAllocationCount allocations;
    for (int i = 0; i < 10000; i++) journal::log("player.hit", fields);
    if (testsupport::countsAllocations()) {
        EXPECT_EQ(allocations.count(), 0u);
    }
    EXPECT_EQ(body(journal::entries(1)[0]), "player.hit hp=87.5 level=forest-2 seed=123456 weapon=\"bow of the north\"");
}

TEST(JournalBench, LogWithFourFields) {
    JournalScope scope(32 << 10);
    const journal::Field fields[] = {
        {"level", std::string_view("forest-2")},
        {"weapon", std::string_view("bow")},
        {"seed", int64_t(123456)},
        {"hp", 87.5},
    };
    constexpr int kLogs = 200000;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kLogs; i++) journal::log("player.hit", fields);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kLogs;
    std::printf("[bench] Journal log, kind + 4 fields: %.0f ns\n", ns);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(ns, 250.0 * slack) << "R29";
    }
}

// R29: Lua's Journal.log(kind, {4 fields}) as a game writes it, the table built at each call; the
// binding itself allocates nothing (a table built once)
TEST(JournalBench, LuaLogWithFourFields) {
    JournalScope scope(32 << 10);
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_EQ(luaL_dostring(L, R"(
        function __journalBenchLog(n)
            for i = 1, n do Journal.log("player.hit", {level = "forest-2", weapon = "bow", seed = 123456, hp = 87.5}) end
        end
        local fields = {level = "forest-2", weapon = "bow", seed = 123456, hp = 87.5}
        function __journalBenchSame(n)
            for i = 1, n do Journal.log("player.hit", fields) end
        end
    )"), LUA_OK) << lua_tostring(L, -1);
    auto run = [&](const char* function, int n) {
        lua_getglobal(L, function);
        lua_pushinteger(L, n);
        const auto start = std::chrono::steady_clock::now();
        const int status = lua_pcall(L, 1, 0, 0);
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / n;
        EXPECT_EQ(status, LUA_OK) << (status == LUA_OK ? "" : lua_tostring(L, -1));
        if (status != LUA_OK) lua_pop(L, 1);
        return ns;
    };
    run("__journalBenchLog", 1000);
    constexpr int kLogs = 100000;
    double best = 1e9;
    for (int round = 0; round < 3; round++) best = std::min(best, run("__journalBenchLog", kLogs));
    run("__journalBenchSame", 1000);
    uint64_t allocations = 0;
    {
        testsupport::ThreadAllocationCount count;
        run("__journalBenchSame", 10000);
        allocations = count.count();
    }
    EXPECT_EQ(body(journal::entries(1)[0]), "player.hit hp=87.5 level=forest-2 seed=123456 weapon=bow");
    std::printf("[bench] Lua Journal.log, kind + a new 4-field table: %.0f ns (budget 1500); %llu allocations in 10,000 logs of one table\n",
                best, static_cast<unsigned long long>(allocations));
    if (testsupport::countsAllocations()) {
        EXPECT_EQ(allocations, 0u) << "R29: the binding reads the table's strings in place";
    }
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(best, 1500.0 * slack) << "R29";
    }
    lua_pushnil(L);
    lua_setglobal(L, "__journalBenchLog");
    lua_pushnil(L);
    lua_setglobal(L, "__journalBenchSame");
}

}  // namespace JournalTest_cpp
