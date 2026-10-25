// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LUA_RELOAD_H
#define LUA_RELOAD_H

// Hot reload of Lua scripts in a running state (the editor's Play): new code goes into the
// live instances, which keep their identity and their field values. Works on a lua_State
// only, no editor dependency; compiled on every platform, called by the editor.

#include "Export.h"
#include "ecs/Entity.h"
#include "ScriptProperty.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

typedef struct lua_State lua_State;

namespace jasperengine {

    class Scene;
    struct ScriptEntry;

    // One live instance of a script: its entry holds the instance's registry ref
    struct ScriptInstanceRef {
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        ScriptEntry* entry = nullptr;
    };

    namespace lua_reload {

        // A RegisterEvent/RegisterEngineEvent call of an instance, as recorded: the event
        // object stays in the instance's record table (Lua side), these are its other fields
        struct Registration {
            std::string method;
            std::string tag;
            bool guarded = false;
        };

        struct Report {
            std::string path;
            int instances = 0;
            std::vector<std::string> added;       // properties (modules: keys) that are new
            std::vector<std::string> removed;     // properties now nil (modules: functions removed)
            std::vector<std::string> converted;   // kept with their kind changed, value converted
            std::vector<std::string> reset;       // kind changed and not convertible: the default
            std::vector<std::string> missingMethods; // recorded registrations whose method is gone
            std::vector<std::string> warnings;    // e.g. a module that returns no table now
            int droppedCallbacks = 0;             // anonymous callbacks and timers of the old code, dropped
            int replayedCallbacks = 0;            // named registrations moved to the new methods
            bool keptOld = false;                 // nothing applied: every instance runs the old code
            std::string error;                    // why (keptOld), "path:line: message"
            int errorLine = 0;
        };

        // The chunk name of a project script or module: "@<path>", so messages read
        // "scripts/player.lua:12: ..." (standard Lua and lua-language-server form)
        JASPERENGINE_API std::string chunkName(std::string_view path);

        // Compiles `code` (chunk "@<path>"), runs it once per live instance (the first run is the
        // probe: it must return a table), then, only when every run succeeded, swaps the new
        // module table in as each instance's __index, reconciles the script properties with
        // newProps (same name and kind keep the runtime value, kind changes are converted or
        // reset, new ones get their default, removed ones become nil), moves the instance's
        // named event registrations to the new methods, drops the anonymous callbacks the old
        // code made, and calls self:onReload(info). init is not run again. On any failure
        // nothing changes (keptOld). Later instances of the path use the new code. Each
        // entry's properties become newProps (with the instance's runtime values).
        JASPERENGINE_API Report reloadScript(lua_State* L, const std::string& path, std::string_view code,
                                             const std::vector<ScriptInstanceRef>& instances, const std::vector<ScriptProperty>& newProps);

        // A require()d module: the new chunk runs once and patches package.loaded[moduleName]
        // in place (functions replaced, added or removed; new data keys added, existing ones
        // keep their values), so every holder of the table sees the new functions. A module
        // that returns something else replaces the entry (with a warning). A module nobody
        // required is left alone (instances 0). `path` names the chunk.
        JASPERENGINE_API Report reloadModule(lua_State* L, const std::string& moduleName, const std::string& path, std::string_view code);
        // Whether package.loaded holds moduleName (a project module that was required)
        JASPERENGINE_API bool isModuleLoaded(lua_State* L, const std::string& moduleName);

        // The line of a Lua error message about `path` ("path:12: ...", also inside
        // [string "path"]:12:), 0 when it names none
        JASPERENGINE_API int errorLine(const std::string& message, const std::string& path);

        // ---- the bookkeeping the reload relies on (LuaBinding, LuaFunctionBase, Timer)

        // Records a registration of `self` (index) on the event at eventIndex, replacing one
        // with the same tag
        JASPERENGINE_API void recordRegistration(lua_State* L, int selfIndex, int eventIndex, const char* method,
                                                 const char* tag, bool guarded);
        JASPERENGINE_API std::vector<Registration> registrations(lua_State* L, int selfIndex);

        // Lua callbacks (event functions, timers) belong to the generation of the chunk that
        // defined them. A reload retires the generation of its chunk: its callbacks stop
        // running (they are the anonymous closures of the old code) and are counted.
        struct ChunkGeneration {
            std::atomic<bool> dropped{false};
            std::atomic<int> live{0};   // callbacks of this generation that still exist
        };
        // A live callback; copies of one callback share it (counted once)
        struct JASPERENGINE_API CallbackRecord {
            explicit CallbackRecord(std::shared_ptr<ChunkGeneration> generation);
            ~CallbackRecord();
            CallbackRecord(const CallbackRecord&) = delete;
            CallbackRecord& operator=(const CallbackRecord&) = delete;
            bool dropped() const { return generation->dropped.load(std::memory_order_relaxed); }
            std::shared_ptr<ChunkGeneration> generation;
        };
        // The record of the function at the top of the stack (nullptr when it is not Lua code
        // of a named chunk). Does not pop it. Lua thread only.
        JASPERENGINE_API std::shared_ptr<CallbackRecord> callbackRecordForFunction(lua_State* L);
        // Forgets every generation (Play starts again, the state closes)
        JASPERENGINE_API void clearGenerations();

    }

}

#endif // LUA_RELOAD_H
