// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NodeLibrary.h"

#include <fstream>
#include <mutex>
#include <sstream>

using namespace jasperengine::editor::procmat;
namespace fs = std::filesystem;

std::shared_ptr<const NodeLibrary> NodeLibrary::fromFiles(const std::vector<std::pair<std::string, std::string>>& files, std::string& error) {
    auto library = std::make_shared<NodeLibrary>();
    for (const auto& [name, text] : files) {
        if (name == "_manifest.json") continue; // the importer's record, nothing here reads it
        Json data = Json::parse(text, nullptr, false);
        if (!data.is_object()) {
            error = name + " is not a JSON object";
            return nullptr;
        }
        if (name == "_functions.json") {
            for (auto it = data["functions"].begin(); it != data["functions"].end(); ++it) library->functions[it.key()] = *it;
        } else if (name == "_types.json") {
            for (const Json& type : data["types"]) library->types[type.value("name", std::string())] = type;
        } else if (name == "_library.json") {
            for (const Json& entry : data["entries"]) library->entries[entry.value("type", std::string())] = entry;
        } else {
            const std::string id = data.value("id", std::string());
            if (id.empty()) {
                error = name + " has no id";
                return nullptr;
            }
            library->nodes[id] = std::move(data);
        }
    }
    if (library->functions.empty() || library->types.empty() || library->nodes.empty()) {
        error = "the node library is incomplete: it needs _functions.json, _types.json and descriptors";
        return nullptr;
    }
    return library;
}

std::shared_ptr<const NodeLibrary> NodeLibrary::fromDirectory(const fs::path& directory, std::string& error) {
    std::vector<std::pair<std::string, std::string>> files;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, ec)) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream file(entry.path(), std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        files.emplace_back(entry.path().filename().string(), text.str());
    }
    if (ec) {
        error = "cannot read " + directory.string() + ": " + ec.message();
        return nullptr;
    }
    return fromFiles(files, error);
}

const Json* NodeLibrary::descriptor(const std::string& id) const {
    auto it = nodes.find(id);
    return it == nodes.end() ? nullptr : &it->second;
}

const Json* NodeLibrary::function(const std::string& id) const {
    auto it = functions.find(id);
    return it == functions.end() ? nullptr : &it->second;
}

const Json* NodeLibrary::portType(const std::string& name) const {
    auto it = types.find(name);
    return it == types.end() ? nullptr : &it->second;
}

const Json* NodeLibrary::catalogEntry(const std::string& id) const {
    auto it = entries.find(id);
    return it == entries.end() ? nullptr : &it->second;
}

const std::vector<LeftOutNode>& NodeLibrary::leftOutNodes() {
    // decided by ProcMatNodeImageGpuSlow on llvmpipe against SpirvInterpreter (S90 R20, AC8; the spec's Deviations)
    static const std::vector<LeftOutNode> nodes = {};
    return nodes;
}

const char* NodeLibrary::leftOutReason(const std::string& id) {
    for (const LeftOutNode& node : leftOutNodes()) {
        if (id == node.id) return node.reason;
    }
    return nullptr;
}
