// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeviceRecords.h"

#include "io/AtomicFile.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "yaml-cpp/yaml.h"

#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::platform {

namespace fs = std::filesystem;

fs::path DeviceRecords::file(const fs::path& internalDir) {
    return internalDir / "user" / "devices.yaml";
}

DeviceRecords DeviceRecords::load(const fs::path& internalDir, std::string* warning) {
    DeviceRecords records;
    const fs::path path = file(internalDir);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return records;
    try {
        std::ifstream in(path, std::ios::binary);
        const YAML::Node root = YAML::Load(in);
        if (!root.IsMap()) throw YAML::Exception(YAML::Mark::null_mark(), "not a map");
        const int version = root["formatVersion"] ? root["formatVersion"].as<int>() : 0;
        if (version < 1 || version > FORMAT_VERSION) {
            if (warning) *warning = FileUtils::pathToGenericUtf8(path) + " is format " + std::to_string(version) + ", this editor reads " +
                                    std::to_string(FORMAT_VERSION) + ": ignored (it is rewritten at the next install)";
            return records;
        }
        if (root["selected"] && root["selected"].IsMap())
            for (const auto& kv : root["selected"]) records.selected[kv.first.as<std::string>()] = kv.second.as<std::string>();
        if (root["devices"] && root["devices"].IsMap()) {
            for (const auto& kv : root["devices"]) {
                const YAML::Node& node = kv.second;
                if (!node.IsMap()) continue;
                DeviceRecord record;
                record.name = node["name"] ? node["name"].as<std::string>() : "";
                record.platform = node["platform"] ? node["platform"].as<std::string>() : "";
                record.package = node["package"] ? node["package"].as<std::string>() : "";
                record.packageSha256 = node["packageSha256"] ? node["packageSha256"].as<std::string>() : "";
                record.playerId = node["playerId"] ? node["playerId"].as<std::string>() : "";
                record.playerSha256 = node["playerSha256"] ? node["playerSha256"].as<std::string>() : "";
                record.development = node["development"] && node["development"].as<bool>();
                record.debuggable = node["debuggable"] && node["debuggable"].as<bool>();
                record.patch = node["patch"] ? node["patch"].as<int>() : 0;
                if (node["content"] && node["content"].IsMap()) {
                    for (const auto& entry : node["content"]) {
                        player::ManifestFile file;
                        file.sha256 = entry.second["sha256"] ? entry.second["sha256"].as<std::string>() : "";
                        file.size = entry.second["size"] ? entry.second["size"].as<uint64_t>() : 0;
                        record.content[entry.first.as<std::string>()] = file;
                    }
                }
                records.devices[kv.first.as<std::string>()] = record;
            }
        }
    } catch (const std::exception& e) {
        if (warning) *warning = FileUtils::pathToGenericUtf8(path) + " cannot be read (" + e.what() + "): ignored (it is rewritten at the next install)";
        return DeviceRecords();
    }
    return records;
}

bool DeviceRecords::save(const fs::path& internalDir, std::string& error) const {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "formatVersion" << YAML::Value << FORMAT_VERSION;
    out << YAML::Key << "selected" << YAML::Value << YAML::BeginMap;
    for (const auto& [platform, id] : selected) out << YAML::Key << platform << YAML::Value << id;
    out << YAML::EndMap;
    out << YAML::Key << "devices" << YAML::Value << YAML::BeginMap;
    for (const auto& [id, record] : devices) {
        out << YAML::Key << id << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << record.name;
        out << YAML::Key << "platform" << YAML::Value << record.platform;
        out << YAML::Key << "package" << YAML::Value << record.package;
        out << YAML::Key << "packageSha256" << YAML::Value << record.packageSha256;
        out << YAML::Key << "playerId" << YAML::Value << record.playerId;
        out << YAML::Key << "playerSha256" << YAML::Value << record.playerSha256;
        out << YAML::Key << "development" << YAML::Value << record.development;
        out << YAML::Key << "debuggable" << YAML::Value << record.debuggable;
        out << YAML::Key << "patch" << YAML::Value << record.patch;
        out << YAML::Key << "content" << YAML::Value << YAML::BeginMap;
        for (const auto& [path, file] : record.content) {
            out << YAML::Key << path << YAML::Value << YAML::Flow << YAML::BeginMap;
            out << YAML::Key << "sha256" << YAML::Value << file.sha256;
            out << YAML::Key << "size" << YAML::Value << file.size;
            out << YAML::EndMap;
        }
        out << YAML::EndMap;
        out << YAML::EndMap;
    }
    out << YAML::EndMap;
    out << YAML::EndMap;
    const fs::path path = file(internalDir);
    // the editor's own per-user file (agents cannot write .jasperengine)
    safewrite::EditorInternal own(path.parent_path());
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const io::WriteResult written = io::writeFileAtomic(path, std::string(out.c_str()) + "\n");
    if (!written.ok()) {
        error = "cannot write " + FileUtils::pathToGenericUtf8(path) + ": " + written.message;
        return false;
    }
    return true;
}

const DeviceRecord* DeviceRecords::find(const std::string& deviceId) const {
    const auto it = devices.find(deviceId);
    return it == devices.end() ? nullptr : &it->second;
}

}  // namespace jasperengine::editor::platform
