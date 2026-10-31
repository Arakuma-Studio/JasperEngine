// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Script data (spec 2026-10-31-script-data 6.4): user_settings, the project's UserSettings /
// PlayerPrefs store. The editor and a game in Play share it (UserSettingsStore, the file
// .jasperengine/user/rundata/UserSettings.xml), so what this tool changes a running game sees
// at once, and what the game wrote is listed here.
#include "McpTools.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "io/UserSettings.h"
#include "io/UserSettingsStore.h"

#include <climits>
#include <string>

namespace jasperengine::editor::mcp {

namespace {

// Lines of text output; the structured data always has every key
constexpr size_t MAX_TEXT_KEYS = 100;
constexpr size_t MAX_TEXT_VALUE = 200;

// A stored value as JSON text: a game may store bytes that are no UTF-8 (U+FFFD then)
std::string quoted(const std::string& value) {
    return Json(value).dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json textJson(const std::string& value) {
    return Json::parse(quoted(value));
}

// For the text output: quoted, long ones cut at a character boundary
std::string shown(const std::string& value) {
    if (value.size() <= MAX_TEXT_VALUE) return quoted(value);
    size_t cut = MAX_TEXT_VALUE;
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) cut--;
    return quoted(value.substr(0, cut)) + "... (" + std::to_string(value.size()) + " bytes)";
}

// A stored key as valid UTF-8 (the structured result is JSON)
std::string keyText(const std::string& key) {
    return textJson(key).get<std::string>();
}

std::string storeFile(McpContext& ctx) {
    Project* project = ctx.project();
    if (!project) return "UserSettings.xml";
    return w4::projectRelative(project, project->getUserDataPath() / "UserSettings.xml");
}

void userSettingsTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string action = args.value("action", "");
    const std::string key = args.contains("key") && args["key"].is_string() ? args["key"].get<std::string>() : "";
    const std::string file = storeFile(call->ctx());
    UserSettingsStore& store = UserSettingsStore::instance();

    if (action == "list") {
        const std::map<std::string, std::string> entries = store.entries();
        Json values = Json::object();
        std::string text = std::to_string(entries.size()) + (entries.size() == 1 ? " key" : " keys") + " in " + file +
                           (entries.empty() ? "." : ":");
        size_t lines = 0;
        for (const auto& [name, value] : entries) {
            // stored text, as UserSettings.getStringForKey reads it (numbers as "120", "0.5")
            values[keyText(name)] = textJson(value);
            if (lines++ < MAX_TEXT_KEYS) text += "\n" + keyText(name) + " = " + shown(value);
        }
        if (entries.size() > MAX_TEXT_KEYS)
            text += "\n... " + std::to_string(entries.size() - MAX_TEXT_KEYS) + " more in data.values";
        call->ok(text, Json{{"values", values}, {"count", entries.size()}, {"file", file}});
        return;
    }

    if (action == "clear") {
        const size_t count = store.entries().size();
        UserSettings::deleteAll();
        UserSettings::save();
        call->ok("Deleted all " + std::to_string(count) + (count == 1 ? " key" : " keys") + " of " + file + ".",
                 Json{{"deleted", count}, {"file", file}});
        return;
    }

    if (key.empty()) {
        call->fail(action + ": `key` (a non-empty string) is required.");
        return;
    }

    if (action == "get") {
        std::string value;
        if (!store.get(key, value)) {
            call->ok("No key " + shown(key) + " in " + file + ".", Json{{"key", key}, {"exists", false}});
            return;
        }
        call->ok(key + " = " + shown(value), Json{{"key", key}, {"exists", true}, {"value", textJson(value)}});
        return;
    }

    if (action == "delete") {
        const bool existed = store.has(key);
        UserSettings::deleteKey(key.c_str());
        UserSettings::save();
        call->ok(existed ? "Deleted " + shown(key) + "." : "No key " + shown(key) + " (nothing to delete).",
                 Json{{"key", key}, {"deleted", existed}});
        return;
    }

    if (action == "set") {
        if (!args.contains("value") || args["value"].is_null() || args["value"].is_object() || args["value"].is_array()) {
            call->fail("set: `value` (a string, number or boolean) is required.");
            return;
        }
        const Json& value = args["value"];
        std::string type = args.value("type", "");
        if (type.empty()) {
            type = value.is_boolean() ? "bool" : value.is_number_integer() ? "int" : value.is_number() ? "float" : "string";
        }
        if (type == "int") {
            if (!value.is_number_integer() || value.get<long long>() < INT_MIN || value.get<long long>() > INT_MAX) {
                call->fail("set: type int needs an integer value within 32 bits (PlayerPrefs.getInt), got " + value.dump() + ".");
                return;
            }
            UserSettings::setIntegerForKey(key.c_str(), value.get<int>());
        } else if (type == "float") {
            if (!value.is_number()) {
                call->fail("set: type float needs a number value, got " + value.dump() + ".");
                return;
            }
            UserSettings::setFloatForKey(key.c_str(), value.get<float>());
        } else if (type == "bool") {
            if (!value.is_boolean()) {
                call->fail("set: type bool needs true or false, got " + value.dump() + ".");
                return;
            }
            UserSettings::setBoolForKey(key.c_str(), value.get<bool>());
        } else if (type == "string") {
            if (!value.is_string()) {
                call->fail("set: type string needs a string value, got " + value.dump() + " (pass type int, float or bool for numbers).");
                return;
            }
            UserSettings::setStringForKey(key.c_str(), value.get<std::string>());
        } else {
            call->fail("set: unknown type \"" + type + "\". Valid: string, int, float, bool.");
            return;
        }
        UserSettings::save();
        std::string stored;
        store.get(key, stored);
        call->ok("Set " + key + " = " + shown(stored) + " (" + type + ") in " + file + ".",
                 Json{{"key", key}, {"type", type}, {"value", textJson(stored)}, {"file", file}});
        return;
    }

    call->fail("Unknown action \"" + action + "\". Valid: list, get, set, delete, clear.");
}

} // namespace

void registerDataTools(McpRegistry& registry) {
    using namespace schema;

    ToolDef tool;
    tool.name = "user_settings";
    tool.title = "User Settings (PlayerPrefs)";
    tool.description =
        "The project's UserSettings/PlayerPrefs store, shared with the game in Play (changes apply at once): list "
        "{key: text}, get, set (type from value or `type`), delete, clear. Writes save to disk.";
    tool.inputSchema = object({
        {"action", stringEnum("", {"list", "get", "set", "delete", "clear"})},
        {"key", string("")},
        {"value", any("set: string, number or boolean.")},
        {"type", stringEnum("set: how it is stored.", {"string", "int", "float", "bool"})}
    }, {"action"});
    tool.annotations.destructive = true;
    // .jasperengine/user/rundata is no project content: an export running does not mind
    tool.activity = ToolActivity::Control;
    tool.handler = userSettingsTool;
    registry.addTool(std::move(tool));
}

}
