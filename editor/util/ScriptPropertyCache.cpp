// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptPropertyCache.h"

#include "Out.h"
#include "util/ProjectChecks.h"
#include "util/ProjectUtils.h"
#include "util/ScriptParser.h"

#include <atomic>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    std::atomic<int> luaRuns{0};
}

void ScriptPropertyCache::loadLua(ScriptEntry& entry, const std::filesystem::path& luaFile) {
    const std::filesystem::path key = luaFile.lexically_normal();
    auto found = luaFiles.find(key);
    if (found == luaFiles.end()) {
        LuaFile file;
        std::string code;
        if (!ProjectChecks::readFile(key, code)) {
            Out::error("Failed to load Lua script \"%s\": cannot open it", key.string().c_str());
        } else {
            ScriptEntry probe;
            luaRuns++;
            const ProjectUtils::LuaPropertiesResult result = ProjectUtils::readLuaScriptProperties(probe, code, key.string());
            if (result.status == ProjectUtils::LuaPropertiesResult::LoadError || result.status == ProjectUtils::LuaPropertiesResult::RunError) {
                Out::error("Failed to load Lua script \"%s\": %s", key.string().c_str(), result.message.c_str());
            } else if (result.status == ProjectUtils::LuaPropertiesResult::PropertyError) {
                Out::error("Failed to read the properties of Lua script \"%s\": %s", key.string().c_str(), result.message.c_str());
            } else {
                // a module that returns no table runs as no script: its fields stay until it does
                file.ok = result.returnedTable;
                file.properties = probe.properties;
            }
        }
        found = luaFiles.emplace(key, std::move(file)).first;
    }
    if (found->second.ok) {
        entry.properties = found->second.properties;
    }
}

const std::vector<ScriptProperty>& ScriptPropertyCache::cppProperties(const std::filesystem::path& header) {
    const std::filesystem::path key = header.lexically_normal();
    auto found = headers.find(key);
    if (found == headers.end()) {
        found = headers.emplace(key, ScriptParser::parseScriptProperties(key)).first;
    }
    return found->second;
}

int ScriptPropertyCache::runsForTest() {
    return luaRuns.load();
}
