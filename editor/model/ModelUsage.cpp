// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelUsage.h"

#include "Project.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "util/Util.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // the calls of the Lua and C++ script API that read a model's vertices at runtime
    const char* const SCRIPT_READERS[] = {"createMeshCollider", "createConvexHullCollider", "bakeAsync", "NavMeshSurface::bake"};

    int64_t stampOf(const fs::path& path, uint64_t& size){
        std::error_code ec;
        size = (uint64_t)fs::file_size(path, ec);
        if (ec)
            return -1;
        const auto time = fs::last_write_time(path, ec);
        return ec ? -1 : (int64_t)time.time_since_epoch().count();
    }

    std::string readText(const fs::path& path){
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    std::string scalar(const YAML::Node& node){
        try { return node && node.IsScalar() ? node.Scalar() : std::string(); } catch (...) { return {}; }
    }

    int64_t integer(const YAML::Node& node, int64_t fallback){
        try { return node ? node.as<int64_t>() : fallback; } catch (...) { return fallback; }
    }

    bool isBlendedValue(const std::string& alphaMode, const std::string& blendMode){
        std::string blend = blendMode;
        std::transform(blend.begin(), blend.end(), blend.begin(), [](unsigned char c){ return (char)std::toupper(c); });
        return alphaMode == "BLEND" || alphaMode == "blend" || blend == "ALPHA" || blend == "PREMULTIPLIED" || blend == "ADDITIVE" ||
               blend == "MULTIPLY";
    }

    // a model filename as the document stores it ("models/a.glb", "asset://models/a.glb") ->
    // asset-relative
    std::string modelAssetPath(Project& project, const std::string& filename){
        if (filename.empty())
            return {};
        const fs::path absolute = project.resolveAssetPath(filename).lexically_normal();
        if (!project.isInsideAssetsPath(absolute))
            return {};
        return project.normalizeToAssetsRelative(absolute).generic_string();
    }

}

int ModelUsage::objects() const{
    int total = 0;
    for (const ModelDocumentUse& use : documents)
        total += use.objects;
    return total;
}

std::set<std::pair<int, int>> ModelUsage::blended() const{
    std::set<std::pair<int, int>> out;
    for (const ModelDocumentUse& use : documents)
        for (int submesh : use.blendedSubmeshes)
            out.insert({-1, submesh});
    return out;
}

bool ModelUsage::anyLod() const{
    return std::any_of(documents.begin(), documents.end(), [](const ModelDocumentUse& use){ return use.lodEnabled; });
}

ModelUsageIndex& ModelUsageIndex::get(){
    static ModelUsageIndex* index = new ModelUsageIndex();
    return *index;
}

void ModelUsageIndex::clear(){
    docs.clear();
    scriptFiles.clear();
    scripts.clear();
    root.clear();
}

void ModelUsageIndex::readDocument(Project& project, const fs::path& absolute, const std::string& relative, Document& out){
    out.models.clear();
    out.readers.clear();
    out.lodEnabled = false;
    YAML::Node yaml;
    try{
        yaml = YAML::LoadFile(absolute.string());
    }catch (...){
        return;
    }
    if (!yaml.IsMap())
        return;
    out.lodEnabled = yaml["scene"] && yaml["scene"].IsMap() && scalar(yaml["scene"]["meshLodEnabled"]) == "true";
    const YAML::Node entities = yaml["entities"] ? yaml["entities"] : yaml["members"];
    if (!entities || !entities.IsSequence())
        return;
    std::map<int64_t, YAML::Node> byId;
    for (const YAML::Node& item : entities){
        if (!item.IsMap() || !item["components"] || !item["components"].IsMap())
            continue;
        byId[integer(item["entity"], -1)] = item;
    }
    // each model's GameObjects: the one with the Model and the ones of its mesh nodes
    std::map<std::string, std::set<int64_t>> owned;
    std::map<int64_t, std::string> names;
    for (const auto& [id, item] : byId){
        names[id] = scalar(item["name"]);
        const YAML::Node model = item["components"]["model"];
        if (!model || !model.IsMap())
            continue;
        const std::string path = modelAssetPath(project, scalar(model["filename"]));
        if (path.empty())
            continue;
        ModelDocumentUse& use = out.models[path];
        use.document = relative;
        use.scene = out.lodEnabled || Util::isSceneFile(relative);
        use.objects++;
        owned[path].insert(id);
        if (const YAML::Node mapping = model["meshNodesMapping"]; mapping && mapping.IsSequence())
            for (const YAML::Node& node : mapping)
                owned[path].insert(integer(node["entity"], -1));
    }
    if (out.models.empty())
        return;
    for (auto& [path, use] : out.models)
        use.lodEnabled = out.lodEnabled;
    const std::set<std::string> bake = [&](){
        std::set<std::string> runtime;
        for (const auto& [id, item] : byId){
            const YAML::Node surface = item["components"]["navmeshsurface"];
            if (surface && surface.IsMap() && scalar(surface["navMeshData"]).empty() && scalar(surface["useGeometry"]) != "physics")
                runtime.insert(names[id]);
        }
        return runtime;
    }();
    for (const auto& [path, entitiesOf] : owned){
        std::vector<std::string>& readers = out.readers[path];
        ModelDocumentUse& use = out.models[path];
        for (int64_t id : entitiesOf){
            auto it = byId.find(id);
            if (it == byId.end())
                continue;
            const YAML::Node mesh = it->second["components"]["mesh"];
            if (mesh && mesh.IsMap()){
                // a blend override on a submesh, or the whole mesh drawn transparent
                const bool transparent = scalar(mesh["transparent"]) == "true" && scalar(mesh["autoTransparency"]) == "false";
                if (const YAML::Node submeshes = mesh["submeshes"]; submeshes && submeshes.IsSequence()){
                    for (size_t s = 0; s < submeshes.size(); s++){
                        const YAML::Node submesh = submeshes[s];
                        const uint64_t fields = (uint64_t)integer(submesh["overrideFields"], 0);
                        const YAML::Node material = submesh["material"];
                        const bool overridden = (fields & (SubmeshOverride_AlphaMode | SubmeshOverride_BlendMode)) != 0;
                        if (transparent || (overridden && material && isBlendedValue(scalar(material["alphaMode"]), scalar(material["blendMode"]))))
                            use.blendedSubmeshes.insert((int)s);
                    }
                }
            }
        }
        // colliders built from one of its meshes, on any GameObject
        for (const auto& [id, item] : byId){
            const YAML::Node body = item["components"]["rigidbody"];
            if (!body || !body.IsMap() || !body["colliders"] || !body["colliders"].IsSequence())
                continue;
            for (const YAML::Node& collider : body["colliders"]){
                const std::string type = scalar(collider["type"]);
                std::string source = scalar(collider["source"]);
                if (source.empty() && (type == "mesh" || type == "convex_hull"))
                    source = "entity_mesh";
                if (source != "entity_mesh")
                    continue;
                const int64_t from = integer(collider["sourceEntity"], 0) > 0 ? integer(collider["sourceEntity"], 0) : id;
                if (entitiesOf.count(from)){
                    readers.push_back((type == "convex_hull" ? "Convex Hull Collider on " : "Mesh Collider on ") + names[id] + " in " + relative);
                    break;
                }
            }
        }
        for (const std::string& surface : bake)
            readers.push_back("Navigation bakes at runtime from " + surface + " in " + relative);
        // a particle system's mesh shape naming the file
        for (const auto& [id, item] : byId){
            const YAML::Node particles = item["components"]["particlesystem"];
            if (!particles || !particles.IsMap())
                continue;
            const std::string text = YAML::Dump(particles);
            const std::string file = fs::path(path).filename().generic_string();
            if (text.find(path) != std::string::npos || (text.find(file) != std::string::npos && file.size() > 4)){
                readers.push_back("Particle mesh shape on " + names[id] + " in " + relative);
                break;
            }
        }
    }
}

void ModelUsageIndex::refresh(Project& project){
    const fs::path projectRoot = project.getProjectPath().lexically_normal();
    if (projectRoot != root){
        clear();
        root = projectRoot;
    }
    if (root.empty())
        return;
    std::set<std::string> seenDocs, seenScripts;
    bool scriptsChanged = false;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)){
        const fs::path& path = it->path();
        const fs::path relativePath = path.lexically_relative(root);
        const std::string first = relativePath.begin() != relativePath.end() ? relativePath.begin()->string() : std::string();
        if (it->is_directory(ec)){
            if (!first.empty() && (first[0] == '.' || Project::isNonAssetTopLevelDir(first) || first == "build"))
                it.disable_recursion_pending();
            continue;
        }
        const std::string relative = relativePath.generic_string();
        const std::string text = path.generic_string();
        uint64_t size = 0;
        if (Util::isSceneFile(text) || Util::isPrefabFile(text)){
            const int64_t stamp = stampOf(path, size);
            seenDocs.insert(relative);
            Document& doc = docs[relative];
            if (doc.stamp != stamp || doc.size != size){
                doc.stamp = stamp;
                doc.size = size;
                doc.scene = Util::isSceneFile(text);
                readDocument(project, path, relative, doc);
            }
        }else if (Util::isScriptFile(text) || path.extension() == ".cpp" || path.extension() == ".h"){
            const int64_t stamp = stampOf(path, size);
            seenScripts.insert(relative);
            Script& script = scriptFiles[relative];
            if (script.stamp != stamp || script.size != size){
                script.stamp = stamp;
                script.size = size;
                script.calls.clear();
                const std::string source = readText(path);
                for (const char* call : SCRIPT_READERS)
                    if (source.find(call) != std::string::npos)
                        script.calls.push_back(call);
                scriptsChanged = true;
            }
        }
    }
    for (auto it = docs.begin(); it != docs.end();)
        it = seenDocs.count(it->first) ? std::next(it) : docs.erase(it);
    for (auto it = scriptFiles.begin(); it != scriptFiles.end();){
        if (seenScripts.count(it->first)){
            ++it;
        }else{
            it = scriptFiles.erase(it);
            scriptsChanged = true;
        }
    }
    if (scriptsChanged || scripts.empty()){
        scripts.clear();
        for (const auto& [path, script] : scriptFiles)
            for (const std::string& call : script.calls)
                scripts.push_back(path + " calls " + call);
    }
    // the open scenes as they are now (an override not saved yet)
    for (SceneProject* sceneProject : project.getScenesAndPrefabStages()){
        Scene* scene = sceneProject->scene;
        if (!scene || !scene->isComponentRegistered<ModelComponent>())
            continue;
        const std::string relative = project.normalizeToProjectRelative(sceneProject->filepath).generic_string();
        Document& doc = docs[relative.empty() ? "<unsaved " + std::to_string(sceneProject->id) + ">" : relative];
        auto models = scene->getComponentArray<ModelComponent>();
        for (size_t i = 0; i < models->size(); i++){
            const ModelComponent& model = models->getComponentFromIndex(i);
            const std::string path = modelAssetPath(project, model.filename);
            if (path.empty())
                continue;
            ModelDocumentUse& use = doc.models[path];
            if (use.document.empty()){
                use.document = relative;
                use.objects = 1;
            }
            use.lodEnabled = scene->isMeshLodEnabled();
            std::vector<Entity> entities = {models->getEntity(i)};
            for (const auto& node : model.meshNodesMapping)
                entities.push_back(node.second);
            for (Entity entity : entities){
                const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
                if (!mesh)
                    continue;
                for (unsigned int s = 0; s < mesh->numSubmeshes; s++){
                    const Submesh& submesh = mesh->submeshes[s];
                    const bool overridden = (submesh.overrideFields & (SubmeshOverride_AlphaMode | SubmeshOverride_BlendMode)) != 0;
                    const bool blended = submesh.material.alphaMode == MaterialAlphaMode::BLEND ||
                                         (submesh.material.blendMode != MaterialBlendMode::AUTO && submesh.material.blendMode != MaterialBlendMode::NONE);
                    if ((overridden && blended) || (!mesh->autoTransparency && mesh->transparent))
                        use.blendedSubmeshes.insert((int)s);
                }
            }
        }
    }
}

ModelUsage ModelUsageIndex::usageOf(const std::string& assetPath) const{
    ModelUsage usage;
    for (const auto& [relative, doc] : docs){
        auto it = doc.models.find(assetPath);
        if (it == doc.models.end())
            continue;
        usage.documents.push_back(it->second);
        auto readers = doc.readers.find(assetPath);
        if (readers != doc.readers.end())
            for (const std::string& reader : readers->second)
                if (std::find(usage.readers.begin(), usage.readers.end(), reader) == usage.readers.end())
                    usage.readers.push_back(reader);
    }
    return usage;
}

std::vector<std::string> ModelUsageIndex::documents() const{
    std::vector<std::string> out;
    for (const auto& [relative, doc] : docs)
        if (relative.rfind("<unsaved", 0) != 0)
            out.push_back(relative);
    return out;
}

std::vector<std::string> ModelUsageIndex::modelsOf(const std::string& document) const{
    std::vector<std::string> out;
    auto it = docs.find(document);
    if (it != docs.end())
        for (const auto& [path, use] : it->second.models)
            out.push_back(path);
    return out;
}

ModelReadWriteDecision ModelUsageIndex::decideReadWrite(const std::string& assetPath, ModelReadWrite setting, ScriptMeshData scriptData,
                                                        bool skinned, bool bakedLevels) const{
    return decide(usageOf(assetPath), scripts, setting, scriptData, skinned, bakedLevels);
}

ModelReadWriteDecision ModelUsageIndex::decide(const ModelUsage& usage, const std::vector<std::string>& scripts, ModelReadWrite setting,
                                               ScriptMeshData scriptData, bool skinned, bool bakedLevels){
    ModelReadWriteDecision decision;
    if (setting == ModelReadWrite::On){
        decision.reasons.push_back("Read/Write is On");
        return decision;
    }
    decision.reasons = usage.readers;
    if (skinned)
        decision.reasons.push_back("Skinned: poses and retargeting read its vertices on the CPU");
    if (!bakedLevels){
        for (const ModelDocumentUse& use : usage.documents)
            if (use.lodEnabled){
                decision.reasons.push_back("Mesh LOD is on in " + use.document + " and the cook baked no levels: they are built at runtime");
                break;
            }
    }
    if (scriptData == ScriptMeshData::Keep)
        decision.reasons.push_back("Project Settings > Mesh keeps CPU data for scripts");
    else if (scriptData == ScriptMeshData::Automatic && !scripts.empty())
        decision.reasons.push_back(scripts.front() + (scripts.size() > 1 ? " (and " + std::to_string(scripts.size() - 1) + " more)" : ""));
    decision.keep = !decision.reasons.empty();
    return decision;
}
