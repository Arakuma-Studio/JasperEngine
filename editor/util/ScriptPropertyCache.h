// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPTPROPERTYCACHE_H
#define SCRIPTPROPERTYCACHE_H

#include "component/ScriptComponent.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // The declared properties of script files for one refresh pass (Play, a scene opened, files
    // changed): each Lua file runs once and each C++ header is parsed once, however many
    // GameObjects use it. Saved files only, never code being edited.
    class ScriptPropertyCache {
    public:
        // entry's properties as its saved Lua file declares them; entry keeps its own on a load
        // or run error (reported once per file)
        void loadLua(ScriptEntry& entry, const std::filesystem::path& luaFile);
        // the DPROPERTY fields of a C++ script header; none when it can't be read (missing)
        std::optional<std::vector<ScriptProperty>> cppProperties(const std::filesystem::path& header);

        // how many Lua chunks the caches of the process have run (tests)
        static int runsForTest();

    private:
        struct LuaFile {
            bool ok = false;
            std::vector<ScriptProperty> properties;
        };
        std::map<std::filesystem::path, LuaFile> luaFiles;
        std::map<std::filesystem::path, std::optional<std::vector<ScriptProperty>>> headers;
    };

}

#endif // SCRIPTPROPERTYCACHE_H
