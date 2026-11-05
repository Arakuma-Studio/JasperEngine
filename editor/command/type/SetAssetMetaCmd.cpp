// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SetAssetMetaCmd.h"

#include <algorithm>
#include <vector>

#include "Out.h"
#include "Project.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetMeta.h"
#include "command/CommandNames.h"
#include "io/AtomicFile.h"
#include "io/watch/FileWatchService.h"
#include "util/YamlFootprint.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    std::vector<std::string> splitKey(const std::string& key){
        std::vector<std::string> parts;
        std::string part;
        std::istringstream stream(key);
        while (std::getline(stream, part, '.')){
            if (!part.empty()) parts.push_back(part);
        }
        return parts;
    }

    YAML::Node loadMeta(const std::filesystem::path& path, bool& existed){
        std::error_code ec;
        existed = std::filesystem::is_regular_file(path, ec);
        if (!existed){
            return YAML::Node(YAML::NodeType::Map);
        }
        try{
            YAML::Node root = YAML::LoadFile(path.string());
            return root.IsMap() ? root : YAML::Node(YAML::NodeType::Map);
        }catch (const std::exception& e){
            editor::Out::warning("Could not read %s: %s", path.generic_string().c_str(), e.what());
            return YAML::Node(YAML::NodeType::Map);
        }
    }

    YAML::Node findKey(YAML::Node root, const std::vector<std::string>& parts){
        YAML::Node node = root;
        for (const std::string& part : parts){
            if (!node.IsMap() || !node[part]){
                return YAML::Node();
            }
            node.reset(node[part]);
        }
        return YAML::Clone(node);
    }

    // Sets (or with a null value removes) the key, making the maps on the way
    void setKey(YAML::Node root, const std::vector<std::string>& parts, const YAML::Node& value){
        if (parts.empty()){
            return;
        }
        YAML::Node node = root;
        for (size_t i = 0; i + 1 < parts.size(); i++){
            if (!node[parts[i]] || !node[parts[i]].IsMap()){
                if (value.IsNull() || !value.IsDefined()) return;
                node[parts[i]] = YAML::Node(YAML::NodeType::Map);
            }
            YAML::Node next = node[parts[i]];
            node.reset(next);
        }
        if (value.IsNull() || !value.IsDefined()){
            node.remove(parts.back());
            // and the maps on the way that it leaves empty, innermost first
            for (size_t depth = parts.size() - 1; depth > 0; depth--){
                YAML::Node parent = root;
                for (size_t i = 0; i + 1 < depth; i++){
                    YAML::Node next = parent[parts[i]];
                    parent.reset(next);
                }
                const YAML::Node child = parent[parts[depth - 1]];
                if (!child.IsMap() || child.size() != 0) break;
                parent.remove(parts[depth - 1]);
            }
        }else{
            node[parts.back()] = YAML::Clone(value);
        }
    }

}

editor::SetAssetMetaCmd::SetAssetMetaCmd(Project* project, const std::filesystem::path& asset, std::string key, YAML::Node value):
    project(project), asset(asset), key(std::move(key)), newValue(YAML::Clone(value)){
}

std::filesystem::path editor::SetAssetMetaCmd::metaPathOf(const std::filesystem::path& absoluteAsset){
    std::filesystem::path meta = absoluteAsset;
    meta += ".meta";
    return meta;
}

std::filesystem::path editor::SetAssetMetaCmd::metaPath() const{
    return metaPathOf(project->getProjectPath() / asset);
}

YAML::Node editor::SetAssetMetaCmd::read(const std::filesystem::path& absoluteAsset, const std::string& key){
    bool existed = false;
    YAML::Node root = loadMeta(metaPathOf(absoluteAsset), existed);
    return findKey(root, splitKey(key));
}

namespace {
    // in the order they were added, so the queues hear a write in a fixed order
    std::vector<std::pair<const void*, editor::SetAssetMetaCmd::WriteListener>>& metaWriteListeners(){
        static std::vector<std::pair<const void*, editor::SetAssetMetaCmd::WriteListener>> listeners;
        return listeners;
    }
}

void editor::SetAssetMetaCmd::addWriteListener(const void* owner, WriteListener listener){
    auto& listeners = metaWriteListeners();
    for (auto& entry : listeners){
        if (entry.first == owner){
            entry.second = std::move(listener);
            return;
        }
    }
    listeners.emplace_back(owner, std::move(listener));
}

void editor::SetAssetMetaCmd::removeWriteListener(const void* owner){
    auto& listeners = metaWriteListeners();
    listeners.erase(std::remove_if(listeners.begin(), listeners.end(), [owner](const auto& entry){ return entry.first == owner; }),
                    listeners.end());
}

bool editor::SetAssetMetaCmd::write(const YAML::Node& value, bool removeFileIfEmpty){
    const bool written = writeFile(value, removeFileIfEmpty);
    if (written){
        // a copy: a listener may add or remove listeners (a queue attaching to a project)
        const auto listeners = metaWriteListeners();
        for (const auto& [owner, listener] : listeners){
            if (listener) listener(project, asset, key);
        }
    }
    return written;
}

bool editor::SetAssetMetaCmd::writeFile(const YAML::Node& value, bool removeFileIfEmpty){
    const std::filesystem::path path = metaPath();
    bool existed = false;
    YAML::Node root = loadMeta(path, existed);
    setKey(root, splitKey(key), value);
    // the editor's own write (S05 R10): the watcher and sync_files do not take it for an
    // external change (an inferred texture import setting is written down after the project opened)
    ScopedExpectedWrite expected(path);
    if (removeFileIfEmpty && root.size() == 0){
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (!ec) expected.reportedRemoved(path);
        return !ec;
    }
    // A sidecar S04 reads keeps every other section byte for byte (its GUID lines, the labels of
    // S25): only the key's top-level section is written again
    std::string text;
    const std::string section = splitKey(key).front();
    std::string bytes;
    AssetMeta meta;
    const bool sectionOfItsOwn = !AssetMeta::isKnownSection(section);
    if (sectionOfItsOwn && existed && assetio::readFile(path, text) && AssetMeta::parse(text, meta) && root[section]){
        YAML::Node wrapped(YAML::NodeType::Map);
        wrapped[section] = root[section];
        YAML::Emitter out;
        out << wrapped;
        const std::string emitted = out.c_str();
        meta.setSection(section, emitted.substr(section.size() + 1));
        bytes = meta.serialize();
    }else if (sectionOfItsOwn && existed && assetio::readFile(path, text) && AssetMeta::parse(text, meta) && !root[section]){
        meta.removeSection(section);
        bytes = meta.serialize();
    }else{
        YAML::Emitter out;
        out << root;
        bytes = std::string(out.c_str()) + "\n";
    }
    const io::WriteResult result = io::writeFileAtomic(path, bytes);
    if (!result.ok()){
        Out::error("Could not write %s: %s", path.generic_string().c_str(), result.message.c_str());
        return false;
    }
    expected.reported();
    return true;
}

bool editor::SetAssetMetaCmd::execute(){
    if (key.empty() || splitKey(key).empty()){
        return false;
    }
    if (!captured){
        const std::filesystem::path path = metaPath();
        YAML::Node root = loadMeta(path, fileExisted);
        oldValue = findKey(root, splitKey(key));
        captured = true;
    }
    return write(newValue, false);
}

void editor::SetAssetMetaCmd::undo(){
    write(oldValue, !fileExisted);
}

std::string editor::SetAssetMetaCmd::describe() const{
    std::string label;
    for (const std::string& part : splitKey(key)){
        if (!label.empty()) label += " ";
        label += CommandNames::property(part);
    }
    return "Set " + label + " (" + CommandNames::file(asset) + ")";
}

bool editor::SetAssetMetaCmd::isNoOp() const{
    return YAML::Dump(oldValue) == YAML::Dump(newValue) && (oldValue.IsDefined() == newValue.IsDefined());
}

size_t editor::SetAssetMetaCmd::memoryBytes() const{
    return sizeof(*this) + key.size() + asset.native().size() + yamlFootprint(newValue) + yamlFootprint(oldValue);
}
