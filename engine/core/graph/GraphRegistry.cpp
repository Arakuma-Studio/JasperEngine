// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphRegistry.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

using namespace jasperengine::graph;

GraphRegistry& GraphRegistry::get(){
    static GraphRegistry* registry = new GraphRegistry();
    return *registry;
}

void GraphRegistry::registerKind(GraphKindDesc kind){
    std::sort(kind.migrations.begin(), kind.migrations.end(), [](const Migration& a, const Migration& b) { return a.toVersion < b.toVersion; });
    auto shared = std::make_shared<const GraphKindDesc>(std::move(kind));
    std::scoped_lock lock(mutex);
    kinds[shared->id] = std::move(shared);
}

void GraphRegistry::unregisterKind(const std::string& id){
    std::scoped_lock lock(mutex);
    kinds.erase(id);
}

std::shared_ptr<const GraphKindDesc> GraphRegistry::kind(const std::string& id) const {
    std::scoped_lock lock(mutex);
    auto it = kinds.find(id);
    return it != kinds.end() ? it->second : nullptr;
}

std::shared_ptr<const GraphKindDesc> GraphRegistry::kindForExtension(const std::string& extension) const {
    std::scoped_lock lock(mutex);
    for (const auto& [id, kind] : kinds) {
        if (!kind->fileExtension.empty() && kind->fileExtension == extension) return kind;
    }
    return nullptr;
}

std::shared_ptr<const GraphKindDesc> GraphRegistry::kindForFile(std::string_view path) const {
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    std::scoped_lock lock(mutex);
    for (const auto& [id, kind] : kinds) {
        const std::string& extension = kind->fileExtension;
        if (extension.empty() || path.size() <= extension.size()) continue;
        const std::string_view tail = path.substr(path.size() - extension.size());
        if (std::equal(tail.begin(), tail.end(), extension.begin(), [&](char a, char b) { return lower(a) == lower(b); })) return kind;
    }
    return nullptr;
}

std::vector<std::string> GraphRegistry::kindIds() const {
    std::scoped_lock lock(mutex);
    std::vector<std::string> ids;
    for (const auto& [id, kind] : kinds) ids.push_back(id);
    return ids;
}

const NodeTypeDesc* GraphRegistry::nodeType(const GraphKindDesc& kind, const std::string& typeId){
    for (const NodeTypeDesc& type : kind.nodeTypes) {
        if (type.id == typeId) return &type;
    }
    return nullptr;
}

const PortTypeDesc* GraphRegistry::portType(const GraphKindDesc& kind, const std::string& typeId){
    for (const PortTypeDesc& type : kind.portTypes) {
        if (type.id == typeId) return &type;
    }
    return nullptr;
}

bool GraphRegistry::canConnect(const GraphKindDesc* kind, const std::string& out, const std::string& in){
    if (out == in || out == "any" || in == "any") return true;
    if (!kind) return false;
    const PortTypeDesc* type = portType(*kind, out);
    return type && std::find(type->implicitTo.begin(), type->implicitTo.end(), in) != type->implicitTo.end();
}

namespace {

    Json portJson(const Port& port){
        Json out = {{"id", port.id}, {"direction", port.direction == PortDirection::Out ? "out" : "in"}, {"type", port.type}};
        if (!port.name.empty()) out["name"] = port.name;
        if (port.multi) out["multi"] = true;
        if (port.optional) out["optional"] = true;
        return out;
    }

}

Json jasperengine::graph::toJson(const NodeTypeDesc& type){
    Json out = {{"id", type.id}, {"category", type.category}, {"display_name", type.displayName}};
    Json ports = Json::array();
    for (const Port& port : type.ports) ports.push_back(portJson(port));
    out["ports"] = std::move(ports);
    Json params = Json::array();
    for (const ParamDesc& param : type.params) {
        Json item = {{"name", param.name}, {"kind", param.kind}, {"default", param.defaultValue}};
        if (param.min) item["min"] = *param.min;
        if (param.max) item["max"] = *param.max;
        if (!param.enumValues.empty()) item["values"] = param.enumValues;
        params.push_back(std::move(item));
    }
    out["params"] = std::move(params);
    out["dynamic_ports"] = static_cast<bool>(type.dynamicPorts);
    return out;
}

Json jasperengine::graph::toJson(const GraphKindDesc& kind){
    Json out = {{"id", kind.id}, {"display_name", kind.displayName}, {"file_extension", kind.fileExtension},
                {"kind_version", kind.kindVersion}, {"acyclic", kind.acyclic}, {"node_types", kind.nodeTypes.size()}};
    if (kind.strictParams) out["strict_params"] = true;
    Json portTypes = Json::array();
    for (const PortTypeDesc& type : kind.portTypes) {
        char color[10];
        std::snprintf(color, sizeof(color), "#%08x", type.color);
        portTypes.push_back({{"id", type.id}, {"color", color}, {"implicit_to", type.implicitTo}});
    }
    out["port_types"] = std::move(portTypes);
    return out;
}
