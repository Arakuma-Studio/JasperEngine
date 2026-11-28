// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Script data (spec 2026-10-31-script-data 6.4): user_settings, the project's UserSettings /
// PlayerPrefs store. The editor and a game in Play share it (UserSettingsStore, the file
// .jasperengine/user/rundata/UserSettings.xml), so what this tool changes a running game sees
// at once, and what the game wrote is listed here. With store "saves" it works on the SaveGame
// slots (docs/saved-data.md) of the store the game uses in Play (savegame::SaveGameStore), which the
// Saved Data window shows too; its destructive actions are the window's undo steps (SaveSlotCmd).
#include "McpTools.h"
#include "test/PlayData.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/SaveSlotCmd.h"
#include "save/SaveFormat.h"
#include "save/SaveGameStore.h"
#include "io/UserSettings.h"
#include "io/UserSettingsStore.h"

#include <climits>
#include <filesystem>
#include <fstream>
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

// ---------------------------------------------------------------- store "saves"

Json payloadJson(const std::string& payload) {
    if (payload.empty()) return nullptr;
    Json out;
    savegame::SaveError error;
    if (!savegame::toJson(payload, out, error)) return nullptr;
    return out;
}

Json slotInfoJson(const savegame::SlotInfo& info) {
    return Json{{"slot", info.slot}, {"version", info.version}, {"saved_at", info.savedAt}, {"size", info.size},
                {"generation", info.generation}, {"generations", info.generations}, {"namespaces", info.namespaces},
                {"meta", payloadJson(info.meta)}, {"requires", payloadJson(info.requirements)}, {"status", info.status}};
}

std::string slotNames(savegame::SaveGameStore& store) {
    std::string names;
    for (const savegame::SlotInfo& info : store.list()) names += (names.empty() ? "" : ", ") + info.slot;
    return names.empty() ? "none" : names;
}

// The undoable steps (the Saved Data window's)
bool runSlotCommand(const CallPtr& call, SaveSlotCmd::Action action, const std::string& slot, uint64_t generation,
                    savegame::CorruptMode mode) {
    Project* project = call->ctx().project();
    auto* command = new SaveSlotCmd(action, slot, generation, mode);
    command->setNoMerge();
    CommandHistory* history = project->getProjectCommandHistory();
    if (!history->addCommand(command)) {
        call->fail(history->lastRefusal().empty() ? "The saved data could not be changed (see read_console)." : history->lastRefusal());
        return false;
    }
    // stopped partway: what it changed is one undo step
    if (!command->partialFailure().empty()) {
        call->fail(command->partialFailure() + " What changed is one undo step: undo {scope: \"project\"} puts it back.");
        return false;
    }
    return true;
}

void savesTool(const CallPtr& call, const std::string& action, const std::string& key) {
    const Json& args = call->args();
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    Project* project = call->ctx().project();
    const std::string folder = project ? w4::projectRelative(project, store.folder()) : store.folder().string();
    const std::string nameSpace = args.contains("namespace") && args["namespace"].is_string() ? args["namespace"].get<std::string>() : "main";

    if (action == "list") {
        Json slots = Json::array();
        std::string text;
        for (const savegame::SlotInfo& info : store.list()) {
            slots.push_back(slotInfoJson(info));
            text += "\n" + info.slot + ": version " + std::to_string(info.version) + ", " + std::to_string(info.size) + " bytes, " +
                    std::to_string(info.generations) + " generation(s), " + info.status;
        }
        call->ok(std::to_string(slots.size()) + (slots.size() == 1 ? " slot" : " slots") + " in " + folder + (slots.empty() ? "." : ":") + text,
                 Json{{"store", "saves"}, {"slots", slots}, {"count", slots.size()}, {"folder", folder}});
        return;
    }
    if (action == "clear") {
        const size_t count = store.list().size();
        if (count && !runSlotCommand(call, SaveSlotCmd::Action::ClearAll, "", 0, savegame::CorruptMode::Flip)) return;
        call->ok("Deleted all " + std::to_string(count) + (count == 1 ? " slot" : " slots") + " (undo brings them back).",
                 Json{{"store", "saves"}, {"deleted", count}, {"folder", folder}});
        return;
    }

    if (action == "get" && args.contains("file") && args["file"].is_string()) {
        std::filesystem::path path = args["file"].get<std::string>();
        if (path.is_relative() && project) path = project->getProjectPath() / path;
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) {
            call->fail("get: cannot read the file " + path.string() + " (" + ec.message() + "); `file` is a project-relative or absolute path of a .jsave.");
            return;
        }
        if (size > savegame::MAX_FILE_BYTES) {
            call->fail("get: " + path.filename().string() + " is " + std::to_string(size) + " bytes, above the 64 MiB a save file may be.");
            return;
        }
        std::ifstream in(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        savegame::SaveFile file;
        savegame::SaveError error;
        if (!savegame::parseSaveFile(bytes, file, error)) {
            call->fail("get: " + path.filename().string() + " is not a readable save file (" + std::string(savegame::errorCodeName(error.code)) + ": " + error.message + ").");
            return;
        }
        savegame::SaveMetadata metadata;
        savegame::splitMetadata(file.metadata, metadata);
        Json namespaces = Json::object();
        std::vector<std::string> names;
        for (const savegame::SaveSection& section : file.sections) {
            namespaces[section.name] = payloadJson(*section.payload);
            names.push_back(section.name);
        }
        call->ok(path.filename().string() + ": generation " + std::to_string(file.generation) + ", version " + std::to_string(file.dataVersion) +
                 ", namespaces " + Json(names).dump() + ".",
                 Json{{"store", "saves"}, {"file", path.string()}, {"generation", file.generation}, {"version", file.dataVersion}, {"namespaces", namespaces},
                      {"meta", payloadJson(metadata.meta)}, {"requires", payloadJson(metadata.requirements)}});
        return;
    }

    if (key.empty()) {
        call->fail(action + ": `key` (the slot name) is required. Slots: " + slotNames(store) + ".");
        return;
    }
    if (!savegame::isValidName(key, savegame::MAX_SLOT_NAME)) {
        call->fail("Slot names use a-z, 0-9, '_' and '-' (got \"" + key + "\"); try \"" + savegame::suggestName(key, savegame::MAX_SLOT_NAME) + "\".");
        return;
    }
    const bool known = store.exists(key);
    if (!known && action != "set") {
        call->fail("No slot \"" + key + "\". Slots: " + slotNames(store) + ".");
        return;
    }

    if (action == "get") {
        const uint64_t generation = args.contains("generation") && args["generation"].is_number_integer() ? args["generation"].get<uint64_t>() : 0;
        savegame::SlotData data;
        const savegame::SaveError error = store.readSlot(key, data, generation);
        if (!error.ok()) {
            call->fail("get: " + std::string(savegame::errorCodeName(error.code)) + ": " + error.message);
            return;
        }
        const savegame::SaveSection* found = nullptr;
        for (const savegame::SaveSection& section : data.sections) if (section.name == nameSpace) found = &section;
        if (!found) {
            call->fail("get: slot \"" + key + "\" has no namespace \"" + nameSpace + "\". Namespaces: " + Json(data.info.namespaces).dump() + ".");
            return;
        }
        Json value = payloadJson(*found->payload);
        Json result{{"store", "saves"}, {"slot", key}, {"namespace", nameSpace}, {"value", value}, {"version", data.info.version},
                    {"generation", data.info.generation}, {"namespaces", data.info.namespaces}, {"meta", payloadJson(data.info.meta)},
                    {"requires", payloadJson(data.info.requirements)}};
        if (data.info.recoveredFrom) result["recovered_from"] = data.info.recoveredFrom;
        if (!data.info.problems.empty()) result["problems"] = data.info.problems;
        std::string text = value.dump();
        if (text.size() > 2000) text = text.substr(0, 2000) + "... (" + std::to_string(text.size()) + " characters in data.value)";
        call->ok(key + "/" + nameSpace + " (version " + std::to_string(data.info.version) + ", generation " +
                 std::to_string(data.info.generation) + (data.info.recoveredFrom ? ", recovered" : "") + ") = " + text, result);
        return;
    }

    if (action == "set") {
        if (!args.contains("value") || !(args["value"].is_object() || args["value"].is_array())) {
            call->fail("\"value\" must be an object or array in SaveGame JSON; vectors are {\"$vec3\": [x, y, z]}");
            return;
        }
        if (!savegame::isValidName(nameSpace, savegame::MAX_SECTION_NAME)) {
            call->fail("set: namespace names use 1-32 characters of a-z, 0-9, '_' and '-' (got \"" + nameSpace + "\").");
            return;
        }
        std::string payload;
        savegame::SaveError error;
        if (!savegame::fromJson(args["value"], payload, error)) {
            call->fail("set: " + error.message);
            return;
        }
        savegame::WriteOptions options;
        const bool playing = project && project->isAnyScenePlaying();
        if (args.contains("version")) {
            // the listed schema's bounds are not enforced before the handler: 0, a negative or a
            // huge number would wrap into another version
            const Json& version = args["version"];
            long long value = 0;
            bool valid = false;
            if (version.is_number_unsigned()) {
                const uint64_t number = version.get<uint64_t>();
                valid = number >= 1 && number <= 2147483647u;
                value = static_cast<long long>(valid ? number : 0);
            } else if (version.is_number_integer()) {
                value = version.get<long long>();
                valid = value >= 1 && value <= 2147483647;
            }
            if (!valid) {
                call->fail("set: version must be an integer from 1 to 2147483647 (got " + version.dump() + ").");
                return;
            }
            options.version = static_cast<uint32_t>(value);
        } else if (playing) {
            options.version = savegame::gameDataVersion();
        } else {
            savegame::SlotInfo info;
            options.version = known && store.info(key, info).ok() && info.version > 0 ? info.version : 1;
        }
        error = store.write(key, nameSpace, std::move(payload), options, nullptr);
        if (error.code == savegame::SaveErrorCode::NewerVersion) {
            savegame::SlotInfo info;
            store.info(key, info);
            call->fail("slot \"" + key + "\" is version " + std::to_string(info.version) + " and this game writes " +
                       std::to_string(options.version) + ": pass version " + std::to_string(info.version) +
                       " to overwrite it, or read it with get.");
            return;
        }
        if (error.ok()) error = store.flush(key);
        if (!error.ok()) {
            call->fail("set: " + std::string(savegame::errorCodeName(error.code)) + ": " + error.message);
            return;
        }
        call->ok("Wrote " + key + "/" + nameSpace + " at version " + std::to_string(options.version) + " (generation " +
                 std::to_string(store.newestGeneration(key)) + ").",
                 Json{{"store", "saves"}, {"slot", key}, {"namespace", nameSpace}, {"version", options.version}, {"generation", store.newestGeneration(key)}});
        return;
    }

    if (action == "delete") {
        if (!runSlotCommand(call, SaveSlotCmd::Action::Delete, key, 0, savegame::CorruptMode::Flip)) return;
        call->ok("Deleted slot \"" + key + "\" (undo brings it back).", Json{{"store", "saves"}, {"slot", key}, {"deleted", true}});
        return;
    }

    if (action == "corrupt") {
        const std::string modeName = args.value("mode", "flip");
        const savegame::CorruptMode mode = modeName == "truncate" ? savegame::CorruptMode::Truncate
                                     : modeName == "header" ? savegame::CorruptMode::Header : savegame::CorruptMode::Flip;
        const uint64_t generation = store.newestGeneration(key);
        if (!runSlotCommand(call, SaveSlotCmd::Action::Corrupt, key, 0, mode)) return;
        call->ok("Corrupted " + savegame::slotFileName(key, generation) + " (" + modeName + ").",
                 Json{{"store", "saves"}, {"slot", key}, {"generation", generation}, {"mode", modeName},
                      {"next", "Read it in the game (eval_lua \"return SaveGame.read('" + key + "')\") to see the recovery, or "
                               "user_settings {store: \"saves\", action: \"get\", key: \"" + key + "\"}."}});
        return;
    }

    if (action == "restore") {
        if (!args.contains("generation") || !args["generation"].is_number_integer()) {
            std::vector<savegame::BackupInfo> backups;
            store.backups(key, backups);
            std::string list;
            for (const savegame::BackupInfo& backup : backups)
                list += (list.empty() ? "" : ", ") + std::to_string(backup.generation) + (backup.valid ? "" : " (corrupt)");
            call->fail("restore: `generation` is required. Generations of \"" + key + "\": " + list + ".");
            return;
        }
        const uint64_t generation = args["generation"].get<uint64_t>();
        std::vector<savegame::BackupInfo> backups;
        store.backups(key, backups);
        const savegame::BackupInfo* chosen = nullptr;
        std::string list;
        for (const savegame::BackupInfo& backup : backups) {
            if (backup.generation == generation) chosen = &backup;
            list += (list.empty() ? "" : ", ") + std::to_string(backup.generation);
        }
        if (!chosen) {
            call->fail("restore: slot \"" + key + "\" has no generation " + std::to_string(generation) + ". Generations: " + list + ".");
            return;
        }
        if (!chosen->valid) {
            call->fail("Couldn't restore \"" + key + "\" generation " + std::to_string(generation) + ": the file is corrupt (" +
                       chosen->problem + ").");
            return;
        }
        if (!runSlotCommand(call, SaveSlotCmd::Action::Restore, key, generation, savegame::CorruptMode::Flip)) return;
        call->ok("Restored generation " + std::to_string(generation) + " of \"" + key + "\" as generation " +
                 std::to_string(store.newestGeneration(key)) + ".",
                 Json{{"store", "saves"}, {"slot", key}, {"restored", generation}, {"generation", store.newestGeneration(key)}});
        return;
    }

    call->fail("Unknown action \"" + action + "\". Valid with store \"saves\": list, get, set, delete, clear, corrupt, restore.");
}

void userSettingsTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string action = args.value("action", "");
    const std::string key = args.contains("key") && args["key"].is_string() ? args["key"].get<std::string>() : "";
    const std::string storeName = args.value("store", "prefs");
    if (storeName == "saves") {
        savesTool(call, action, key);
        return;
    }
    if (storeName != "prefs") {
        call->fail("Unknown store \"" + storeName + "\". Valid: prefs (UserSettings/PlayerPrefs), saves (SaveGame slots).");
        return;
    }
    if (action == "corrupt" || action == "restore") {
        call->fail(action + " works on SaveGame slots: pass store \"saves\".");
        return;
    }
    const std::string file = storeFile(call->ctx());
    // the Play Data profile whose store this is (project-test-runner R43)
    const std::string profile = gametests::PlayData::get().profile();
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
        call->ok(text, Json{{"store", "prefs"}, {"profile", profile}, {"values", values}, {"count", entries.size()}, {"file", file}});
        return;
    }

    if (action == "clear") {
        const size_t count = store.entries().size();
        UserSettings::deleteAll();
        UserSettings::save();
        call->ok("Deleted all " + std::to_string(count) + (count == 1 ? " key" : " keys") + " of " + file + ".",
                 Json{{"store", "prefs"}, {"profile", profile}, {"deleted", count}, {"file", file}});
        return;
    }

    if (key.empty()) {
        call->fail(action + ": `key` (a non-empty string) is required.");
        return;
    }

    if (action == "get") {
        std::string value;
        if (!store.get(key, value)) {
            call->ok("No key " + shown(key) + " in " + file + ".", Json{{"store", "prefs"}, {"profile", profile}, {"key", key}, {"exists", false}});
            return;
        }
        call->ok(key + " = " + shown(value), Json{{"store", "prefs"}, {"profile", profile}, {"key", key}, {"exists", true}, {"value", textJson(value)}});
        return;
    }

    if (action == "delete") {
        const bool existed = store.has(key);
        UserSettings::deleteKey(key.c_str());
        UserSettings::save();
        call->ok(existed ? "Deleted " + shown(key) + "." : "No key " + shown(key) + " (nothing to delete).",
                 Json{{"store", "prefs"}, {"profile", profile}, {"key", key}, {"deleted", existed}});
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
                 Json{{"store", "prefs"}, {"profile", profile}, {"key", key}, {"type", type}, {"value", textJson(stored)}, {"file", file}});
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
        "{key: text}, get, set (type from value or `type`), delete, clear. Writes save to disk. "
        "store \"saves\": SaveGame slots (key = slot): list, get (namespace, generation, or a .jsave file), set (JSON; "
        "vectors {\"$vec3\":[..]}), delete, clear, corrupt, restore (generation).";
    tool.inputSchema = object({
        {"action", stringEnum("", {"list", "get", "set", "delete", "clear", "corrupt", "restore"})},
        {"store", stringEnum("Default prefs.", {"prefs", "saves"})},
        {"key", string("")},
        {"value", any("set: string, number or boolean; saves: an object or array.")},
        {"type", stringEnum("set: how it is stored.", {"string", "int", "float", "bool"})},
        {"namespace", string("saves: default main.")},
        {"generation", integer("saves get/restore.", 1, LLONG_MAX)},
        {"version", integer("saves set: the data version.", 1, INT_MAX)},
        {"mode", stringEnum("saves corrupt.", {"truncate", "flip", "header"})},
        {"file", string("saves get: a .jsave path.")}
    }, {"action"});
    tool.annotations.destructive = true;
    // .jasperengine/user/rundata is no project content: an export running does not mind
    tool.activity = ToolActivity::Control;
    tool.handler = userSettingsTool;
    registry.addTool(std::move(tool));
}

}
