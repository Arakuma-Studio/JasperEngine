// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_EDITOR_MENU_H
#define JASPERENGINE_EDITOR_MENU_H

// Editor menu items written in C++ (Unity's [MenuItem]), for .cpp files under a folder named
// Editor:
//
//     #include "script/EditorMenu.h"
//
//     JE_MENU_ITEM("Tools/Bake Lights", 100)
//     static void bakeLights(const jasperengine::EditorMenuContext& ctx) {
//         ctx.log("baking");
//         ctx.call("set_transform", R"({"entity": 3, "position": [0, 1, 0]})");
//     }
//
// The editor finds the items by reading the files (they show before anything is built). The
// first click builds the project's editor-scripts library (never part of a game), checks its
// toolchain fingerprint and loads it; every edit goes through the editor's MCP tools, so a
// run is one undo step named after the item. The context and the results are JSON text.

#include <cstdint>
#include <string>

// The C ABI between the editor and its editor-scripts library
extern "C" {
    struct JE_EditorHostApi {
        uint32_t abiVersion;                 // JE_EDITOR_HOST_ABI_VERSION
        void* host;
        // Runs an MCP tool; *outJson (freed with freeString) gets {ok, text, data}. 0 when it ran.
        int (*call)(void* host, const char* tool, const char* jsonArgs, char** outJson);
        void (*freeString)(char* text);
        // level: 0 info, 1 warning, 2 error
        void (*log)(void* host, int level, const char* text);
    };
}

#define JE_EDITOR_HOST_ABI_VERSION 1u

namespace jasperengine {

    struct EditorMenuContext {
        const JE_EditorHostApi* host = nullptr;
        // {menuPath, selection: [ids], scene, assets: [paths], component}
        std::string contextJson;

        // The tool's {ok, text, data} as JSON text
        std::string call(const std::string& tool, const std::string& jsonArgs = "{}") const {
            if (!host || !host->call) return "{\"ok\":false,\"text\":\"no editor\"}";
            char* out = nullptr;
            host->call(host->host, tool.c_str(), jsonArgs.c_str(), &out);
            std::string result = out ? out : "";
            if (out && host->freeString) host->freeString(out);
            return result;
        }
        void log(const std::string& text) const {
            if (host && host->log) host->log(host->host, 0, text.c_str());
        }
        void warning(const std::string& text) const {
            if (host && host->log) host->log(host->host, 1, text.c_str());
        }
    };

    using EditorMenuFunction = void (*)(const EditorMenuContext&);

}

// Marks the function that follows as an editor menu item (path, priority). The editor reads it
// from the file; the build compiles the file with a registration of the function after it.
#define JE_MENU_ITEM(PATH, PRIORITY)

#endif // JASPERENGINE_EDITOR_MENU_H
