// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SetAssetMetaCmd.h"

#include "Out.h"
#include "Project.h"
#include "command/CommandNames.h"
#include "io/AtomicFile.h"
#include "util/YamlFootprint.h"

#include <fstream>
#include <sstream>
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

bool editor::SetAssetMetaCmd::write(const YAML::Node& value, bool removeFileIfEmpty){
    const std::filesystem::path path = metaPath();
    bool existed = false;
    YAML::Node root = loadMeta(path, existed);
    setKey(root, splitKey(key), value);
    if (removeFileIfEmpty && root.size() == 0){
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return !ec;
    }
    YAML::Emitter out;
    out << root;
    const io::WriteResult result = io::writeFileAtomic(path, std::string(out.c_str()) + "\n");
    if (!result.ok()){
        Out::error("Could not write %s: %s", path.generic_string().c_str(), result.message.c_str());
        return false;
    }
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
