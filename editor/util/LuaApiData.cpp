// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaApiData.h"

#include "EditorVersion.h"
#include "lua_api_data.h"

using namespace jasperengine::editor;

namespace {

    std::string joined(const char* const* parts, unsigned int count){
        std::string text;
        for (unsigned int i = 0; i < count; i++) text += parts[i];
        return text;
    }

}

const mcp::Json& LuaApiData::json(){
    static const mcp::Json api = []() {
        mcp::Json value = mcp::Json::parse(joined(luaapi::jsonParts, luaapi::jsonPartCount), nullptr, false);
        if (value.is_discarded() || !value.is_object()) value = mcp::Json::object();
        value["engineVersion"] = JASPERENGINE_EDITOR_VERSION;
        return value;
    }();
    return api;
}

std::string LuaApiData::jsonText(){
    return json().dump(1) + "\n";
}

const std::string& LuaApiData::markdown(){
    static const std::string text = joined(luaapi::markdownParts, luaapi::markdownPartCount);
    return text;
}

mcp::Json LuaApiData::findClass(const std::string& name){
    const mcp::Json& api = json();
    if (!api.contains("classes")) return nullptr;
    for (const mcp::Json& cls : api["classes"]) {
        if (cls.value("name", std::string()) == name) return cls;
    }
    return nullptr;
}
