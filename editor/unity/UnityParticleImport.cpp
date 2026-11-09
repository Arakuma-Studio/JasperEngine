// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Unity ParticleSystem importer (UnityParticleImport.h; docs/specs/S20-particles.md,
// Upstream absorption). Steps: the file is read into one object graph (prefab instances
// resolved recursively, their modifications applied by property path, their objects given
// Unity's own ids: source id XOR instance id, so the file's stripped placeholders and
// references meet them), the roots are picked, each root's GameObject tree is built in a
// scratch Scene (transforms converted by S = diag(-1, 1, 1); each ParticleSystem an S20
// Particle System with an inline effect, its renderer's material in the Renderer module),
// then written as a prefab with its textures, and the report.

#include "UnityParticleImport.h"
#include "UnityProject.h"
#include "UnityYaml.h"

#include "Catalog.h"
#include "Project.h"
#include "util/ProjectFileWrites.h"
#include "Stream.h"
#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "registry/SortingLayers.h"
#include "component/Transform.h"
#include "math/Curve.h"
#include "particles/ParticleEffect.h"
#include "shader/ShaderUniforms.h"
#include "stream/UpstreamParticles.h"
#include "texture/Material.h"
#include "util/AssetDatabase.h"
#include "util/Prefab.h"
#include "util/Color.h"
#include "util/SafeWrite.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::unity;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using UnityProject = jasperengine::editor::unity::Project;
using particles::MinMaxCurve;
using particles::MinMaxGradient;
using particles::ParticleEffectDocument;

namespace {

    // Unity's view is left handed: a positive billboard roll turns clockwise on screen there,
    // counter-clockwise here; Z rotations and Z angular speeds change sign. If a capture from
    // Unity shows otherwise, only this changes.
    constexpr float ROTATION_SIGN = -1.0f;
    constexpr float PI = 3.14159265358979323846f;
    // Unity stores rotations and angular speeds in radians; S20's are degrees
    constexpr float RAD_TO_DEG = 180.0f / PI;

    // ------------------------------------------------------------------ the object graph

    // the id Unity gives an object of a prefab instance in the file holding the instance: its
    // source object's id XOR the instance's, without the sign bit (observed in the game's files;
    // a scene's `stripped` placeholders may have their own ids instead: aliased, resolveFile)
    int64_t instanceObjectId(int64_t source, int64_t instance){
        return (int64_t)(((uint64_t)source ^ (uint64_t)instance) & 0x7FFFFFFFFFFFFFFFull);
    }

    // every local reference (no guid) to an alias key rewritten to its target
    void rewriteAliases(YAML::Node node, const std::map<int64_t, int64_t>& aliases){
        if (node.IsMap()){
            if (node["fileID"] && node["fileID"].IsScalar() && !node["guid"]){
                const int64_t id = std::strtoll(node["fileID"].Scalar().c_str(), nullptr, 10);
                auto it = aliases.find(id);
                if (it != aliases.end()) node["fileID"] = std::to_string(it->second);
            }
            for (auto it = node.begin(); it != node.end(); ++it) rewriteAliases(it->second, aliases);
        }else if (node.IsSequence()){
            for (size_t i = 0; i < node.size(); i++) rewriteAliases(node[i], aliases);
        }
    }

    struct Object {
        int classId = 0;
        int64_t id = 0;
        YAML::Node body;
        // prefab overrides of this object that could not be applied (reported when it is imported)
        std::vector<std::string> unknownOverrides;
        // its place in the file (an instance's objects at the instance's): roots and unlisted
        // children keep the file's order
        int64_t order = 0;
    };

    // a ParticleSystem's own top-level and module fields (an override of one is worth a warning
    // even when its object is not found)
    bool particleOverridePath(const std::string& path){
        const std::string first = path.substr(0, path.find('.'));
        static const std::set<std::string> top = {"lengthInSec", "simulationSpeed", "stopAction", "cullingMode", "ringBufferMode",
            "emitterVelocityMode", "looping", "prewarm", "playOnAwake", "useUnscaledTime", "autoRandomSeed", "startDelay",
            "moveWithTransform", "scalingMode", "randomSeed"};
        return top.count(first) || (first.size() > 6 && first.compare(first.size() - 6, 6, "Module") == 0);
    }

    using Graph = std::map<int64_t, Object>;

    struct Context {
        UnityProject project;
        ImportOptions options;
        fs::path engineRoot;
        fs::path dest;            // absolute
        std::string destRel;      // project-relative, generic
        std::vector<std::string> warnings;
        Json materials = Json::array();
        Json textures = Json::array();
        // Unity material path -> the engine material (the Renderer modules take its values) and the
        // notes its import made (given again to every system that uses it)
        struct CachedMaterial { Material material; std::vector<ImportNote> notes; };
        std::map<std::string, CachedMaterial> materialCache;
        // Unity texture path -> engine path ("" when not copied) and its notes
        struct CachedTexture { std::string path; std::vector<ImportNote> notes; };
        std::map<std::string, CachedTexture> textureCache;
        // output path -> the Unity asset it was given to (two "smoke.png" get two names)
        std::map<std::string, std::string> claimedOutputs;
        std::vector<std::pair<fs::path, std::string>> files;  // to write: absolute path, bytes
        std::set<std::string> usedNames;
        // each prefab file resolved once (a scene instances the same prefab many times)
        std::map<std::string, Graph> prefabCache;
        int64_t nextOrder = 0;
        // the notes of the system being converted
        std::vector<ImportNote>* notes = nullptr;
    };

    // file names compare without case (case-insensitive file systems)
    std::string lowercaseName(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    void note(Context& ctx, const std::string& code, const std::string& message){
        if (ctx.notes) ctx.notes->push_back({code, message});
        else ctx.warnings.push_back(code + ": " + message);
    }

    // the notes made since `mark` (the current system's), to keep with a cached asset
    std::vector<ImportNote> notesSince(const Context& ctx, size_t mark){
        if (!ctx.notes || ctx.notes->size() < mark) return {};
        return std::vector<ImportNote>(ctx.notes->begin() + (long)mark, ctx.notes->end());
    }

    size_t noteMark(const Context& ctx){
        return ctx.notes ? ctx.notes->size() : 0;
    }

    void replayNotes(Context& ctx, const std::vector<ImportNote>& notes){
        for (const ImportNote& n : notes) note(ctx, n.code, n.message);
    }

    // a project-relative output path in `dir` for the Unity asset `source`: `stem` + `ext`, or
    // "<stem> 1" ... when another asset of this import has that name already
    std::string claimOutput(Context& ctx, const std::string& dir, const std::string& stem, const std::string& ext, const std::string& source){
        for (int i = 0; ; i++){
            const std::string rel = dir + "/" + (i == 0 ? stem : stem + " " + std::to_string(i)) + ext;
            auto it = ctx.claimedOutputs.find(lowercaseName(rel));
            if (it == ctx.claimedOutputs.end()){
                ctx.claimedOutputs[lowercaseName(rel)] = source;
                return rel;
            }
            if (it->second == source) return rel;
        }
    }

    // a local reference's id inside the graph moved by an instance (XOR with the instance id)
    void remapLocalRefs(YAML::Node node, int64_t key, const std::set<int64_t>& ids){
        if (node.IsMap()){
            if (node["fileID"] && node["fileID"].IsScalar() && !node["guid"]){
                const int64_t id = std::strtoll(node["fileID"].Scalar().c_str(), nullptr, 10);
                if (id != 0 && ids.count(id)) node["fileID"] = std::to_string(instanceObjectId(id, key));
            }
            for (auto it = node.begin(); it != node.end(); ++it) remapLocalRefs(it->second, key, ids);
        }else if (node.IsSequence()){
            for (size_t i = 0; i < node.size(); i++) remapLocalRefs(node[i], key, ids);
        }
    }

    // a modification's value written into the node it targets: the object reference when it
    // names one ({fileID, guid, type}), else the scalar
    void assignModification(YAML::Node target, const YAML::Node& value, const Ref& objectReference){
        if (objectReference.fileID != 0 || !objectReference.guid.empty()){
            YAML::Node ref(YAML::NodeType::Map);
            ref["fileID"] = std::to_string(objectReference.fileID);
            if (!objectReference.guid.empty()){
                ref["guid"] = objectReference.guid;
                ref["type"] = std::to_string(objectReference.type);
            }
            target = ref;
        }else{
            target = value.IsScalar() ? value.Scalar() : std::string();
        }
    }

    // Unity's propertyPath ("InitialModule.startSize.scalar", "...m_Curve.Array.data[0].value",
    // "...Array.size") set to `value` (or the object reference); false when the path is not there
    bool applyModification(YAML::Node body, const std::string& path, const YAML::Node& value, const Ref& objectReference){
        std::vector<std::string> tokens;
        std::stringstream in(path);
        std::string token;
        while (std::getline(in, token, '.')) tokens.push_back(token);
        YAML::Node current;
        current.reset(body);
        for (size_t i = 0; i < tokens.size(); i++){
            const std::string& t = tokens[i];
            const bool last = i + 1 == tokens.size();
            if (t == "Array" && i + 1 < tokens.size()){
                if (!current.IsSequence() && !current.IsNull()) return false;
                const std::string& next = tokens[++i];
                if (next == "size"){
                    const long size = std::strtol(value.Scalar().c_str(), nullptr, 10);
                    if (size < 0) return false;
                    YAML::Node list(YAML::NodeType::Sequence);
                    for (long k = 0; k < size; k++){
                        if ((size_t)k < current.size()) list.push_back(current[(size_t)k]);
                        else if (current.size() > 0) list.push_back(YAML::Clone(current[current.size() - 1]));
                        else list.push_back(YAML::Node(YAML::NodeType::Map));
                    }
                    current = list;
                    return true;
                }
                if (next.compare(0, 5, "data[") != 0) return false;
                const size_t index = (size_t)std::strtoul(next.c_str() + 5, nullptr, 10);
                while (current.size() <= index){
                    current.push_back(current.size() > 0 ? YAML::Clone(current[current.size() - 1]) : YAML::Node(YAML::NodeType::Map));
                }
                YAML::Node element = current[index];
                if (i + 1 == tokens.size()){
                    // an element itself: a value or an object reference (m_Materials.Array.data[0])
                    assignModification(element, value, objectReference);
                    return true;
                }
                current.reset(element);
                continue;
            }
            if (!current.IsMap() || !current[t]){
                return false;
            }
            if (last){
                YAML::Node target = current[t];
                assignModification(target, value, objectReference);
                return true;
            }
            YAML::Node next = current[t];
            current.reset(next);
        }
        return false;
    }

    bool resolveFile(const fs::path& file, Graph& graph, Context& ctx, int depth);

    // a prefab instance: its source prefab's objects, modified, moved to this file's ids
    void instantiate(const Document& doc, Graph& graph, Context& ctx, int depth){
        const Ref source = readRef(doc.body, "m_SourcePrefab");
        const fs::path sourcePath = ctx.project.pathOf(source.guid);
        if (sourcePath.empty()){
            ctx.warnings.push_back("prefab-missing: the prefab of instance &" + std::to_string(doc.fileId) + " (guid " + source.guid + ") is not in the project");
            return;
        }
        auto cached = ctx.prefabCache.find(sourcePath.generic_string());
        if (cached == ctx.prefabCache.end()){
            Graph resolved;
            if (!resolveFile(sourcePath, resolved, ctx, depth + 1)) return;
            cached = ctx.prefabCache.emplace(sourcePath.generic_string(), std::move(resolved)).first;
        }
        // a copy: the modifications are this instance's
        Graph objects;
        for (const auto& item : cached->second){
            objects[item.first] = {item.second.classId, item.second.id, YAML::Clone(item.second.body), item.second.unknownOverrides,
                                   item.second.order};
        }

        const YAML::Node modification = doc.body["m_Modification"];
        if (modification && modification["m_Modifications"] && modification["m_Modifications"].IsSequence()){
            for (const YAML::Node& m : modification["m_Modifications"]){
                const Ref target = readRef(m, "target");
                const std::string path = readString(m, "propertyPath");
                auto it = target.guid == source.guid ? objects.find(target.fileID) : objects.end();
                if (it == objects.end() || !applyModification(it->second.body, path, m["value"], readRef(m, "objectReference"))){
                    // transform hints that need nothing
                    if (path.compare(0, 22, "m_LocalEulerAnglesHint") == 0) continue;
                    const std::string message = ctx.project.relative(sourcePath) + ": " + path + " (on &" + std::to_string(target.fileID) + ")";
                    if (it != objects.end()){
                        // reported if that object is imported
                        it->second.unknownOverrides.push_back(message);
                    }else if (particleOverridePath(path)){
                        ctx.warnings.push_back("override-unknown: " + message);
                    }
                }
            }
        }
        // removed components
        if (modification && modification["m_RemovedComponents"] && modification["m_RemovedComponents"].IsSequence()){
            for (const YAML::Node& r : modification["m_RemovedComponents"]){
                const Ref removed = toRef(r);
                objects.erase(removed.fileID);
            }
        }
        // removed GameObjects: with their components and their children's
        if (modification && modification["m_RemovedGameObjects"] && modification["m_RemovedGameObjects"].IsSequence()){
            std::set<int64_t> gone;
            for (const YAML::Node& r : modification["m_RemovedGameObjects"]){
                const int64_t id = toRef(r).fileID;
                if (id != 0) gone.insert(id);
            }
            bool grew = !gone.empty();
            while (grew){
                grew = false;
                // a transform under a removed GameObject's transform: its GameObject goes too
                std::set<int64_t> goneTransforms;
                for (const auto& item : objects){
                    if ((item.second.classId == CLASS_TRANSFORM || item.second.classId == CLASS_RECTTRANSFORM) &&
                        gone.count(readRef(item.second.body, "m_GameObject").fileID)) goneTransforms.insert(item.first);
                }
                for (const auto& item : objects){
                    if ((item.second.classId == CLASS_TRANSFORM || item.second.classId == CLASS_RECTTRANSFORM) &&
                        goneTransforms.count(readRef(item.second.body, "m_Father").fileID)){
                        const int64_t child = readRef(item.second.body, "m_GameObject").fileID;
                        if (child != 0 && gone.insert(child).second) grew = true;
                    }
                }
            }
            for (auto it = objects.begin(); it != objects.end();){
                const bool remove = gone.count(it->first) || (it->second.classId != CLASS_GAMEOBJECT &&
                                                             gone.count(readRef(it->second.body, "m_GameObject").fileID));
                it = remove ? objects.erase(it) : std::next(it);
            }
        }
        // components added to the instance are documents of this file naming its GameObject's
        // placeholder (componentsOf finds them)

        std::set<int64_t> ids;
        for (const auto& item : objects) ids.insert(item.first);
        const Ref parent = readRef(modification, "m_TransformParent");
        const int64_t base = ctx.nextOrder;
        for (auto& item : objects){
            ctx.nextOrder = std::max(ctx.nextOrder, base + item.second.order + 1);
            Object object = item.second;
            remapLocalRefs(object.body, doc.fileId, ids);
            // the source's root hangs from the instance's parent
            if ((object.classId == CLASS_TRANSFORM || object.classId == CLASS_RECTTRANSFORM) && readRef(object.body, "m_Father").fileID == 0){
                YAML::Node father(YAML::NodeType::Map);
                father["fileID"] = std::to_string(parent.fileID);
                object.body["m_Father"] = father;
            }
            object.id = instanceObjectId(item.first, doc.fileId);
            object.order = base + item.second.order;
            graph.erase(object.id);
            graph.emplace(object.id, object);
        }
    }

    bool resolveFile(const fs::path& file, Graph& graph, Context& ctx, int depth){
        if (depth > 16){
            ctx.warnings.push_back("prefab-depth: nested prefabs deeper than 16 at " + file.string());
            return false;
        }
        YamlFile yaml;
        std::string error;
        if (!yaml.load(file, error)){
            ctx.warnings.push_back("yaml: " + error);
            return false;
        }
        std::map<int64_t, int64_t> aliases;
        for (const Document& doc : yaml.documents){
            if (doc.classId == CLASS_PREFABINSTANCE){
                instantiate(doc, graph, ctx, depth);
            }else if (doc.stripped){
                // a placeholder of an instance's object: references to it mean that object
                const int64_t source = readRef(doc.body, "m_CorrespondingSourceObject").fileID;
                const int64_t instance = readRef(doc.body, "m_PrefabInstance").fileID;
                const int64_t target = instanceObjectId(source, instance);
                if (source != 0 && instance != 0 && target != doc.fileId) aliases[doc.fileId] = target;
            }else if (!graph.count(doc.fileId)){
                graph[doc.fileId] = {doc.classId, doc.fileId, YAML::Clone(doc.body), {}, ctx.nextOrder++};
            }
        }
        if (!aliases.empty()){
            for (auto& item : graph) rewriteAliases(item.second.body, aliases);
        }
        return true;
    }

    // ------------------------------------------------------------------ the hierarchy

    struct Hierarchy {
        std::map<int64_t, int64_t> transformOf;    // GameObject -> its Transform
        std::map<int64_t, int64_t> parentOf;       // GameObject -> parent GameObject (0: a root)
        std::map<int64_t, std::vector<int64_t>> children;
        std::vector<int64_t> roots;
    };

    // the components of each GameObject: its m_Component list, then the ones naming it in their
    // m_GameObject only (added to a prefab instance's GameObject in this file); built once per
    // graph (a scene has thousands of objects)
    struct ComponentIndex {
        const void* graph = nullptr;
        std::map<int64_t, std::vector<int64_t>> components;
    };
    thread_local ComponentIndex componentIndex;

    const std::vector<int64_t>& componentsOf(const Graph& graph, int64_t gameObject){
        const void*& indexed = componentIndex.graph;
        std::map<int64_t, std::vector<int64_t>>& index = componentIndex.components;
        static const std::vector<int64_t> none;
        if (indexed != &graph){
            index.clear();
            for (const auto& item : graph){
                if (item.second.classId != CLASS_GAMEOBJECT) continue;
                std::vector<int64_t>& list = index[item.first];
                const YAML::Node components = item.second.body["m_Component"];
                if (!components || !components.IsSequence()) continue;
                for (const YAML::Node& entry : components){
                    const int64_t id = readRef(entry, "component").fileID;
                    if (graph.count(id)) list.push_back(id);
                }
            }
            for (const auto& item : graph){
                if (item.second.classId == CLASS_GAMEOBJECT) continue;
                const int64_t owner = readRef(item.second.body, "m_GameObject").fileID;
                auto it = index.find(owner);
                if (it != index.end() && std::find(it->second.begin(), it->second.end(), item.first) == it->second.end()){
                    it->second.push_back(item.first);
                }
            }
            indexed = &graph;
        }
        auto it = index.find(gameObject);
        return it == index.end() ? none : it->second;
    }

    const Object* componentOfClass(const Graph& graph, int64_t gameObject, int classId){
        for (int64_t id : componentsOf(graph, gameObject)){
            const Object& o = graph.at(id);
            if (o.classId == classId) return &o;
        }
        return nullptr;
    }

    Hierarchy buildHierarchy(const Graph& graph){
        Hierarchy h;
        std::map<int64_t, int64_t> goOfTransform;
        for (const auto& item : graph){
            const Object& o = item.second;
            if (o.classId == CLASS_TRANSFORM || o.classId == CLASS_RECTTRANSFORM){
                const int64_t go = readRef(o.body, "m_GameObject").fileID;
                if (graph.count(go)){
                    h.transformOf[go] = o.id;
                    goOfTransform[o.id] = go;
                }
            }
        }
        for (const auto& item : h.transformOf){
            const Object& t = graph.at(item.second);
            const int64_t fatherTransform = readRef(t.body, "m_Father").fileID;
            auto parent = goOfTransform.find(fatherTransform);
            h.parentOf[item.first] = parent == goOfTransform.end() ? 0 : parent->second;
        }
        // children in their parent's m_Children order, then the ones it does not list
        for (const auto& item : h.transformOf){
            const int64_t go = item.first;
            const Object& t = graph.at(item.second);
            std::vector<int64_t>& list = h.children[go];
            if (t.body["m_Children"] && t.body["m_Children"].IsSequence()){
                for (const YAML::Node& c : t.body["m_Children"]){
                    auto child = goOfTransform.find(toRef(c).fileID);
                    if (child != goOfTransform.end() && h.parentOf[child->second] == go) list.push_back(child->second);
                }
            }
        }
        // the rest in the file's order
        std::vector<int64_t> byOrder;
        for (const auto& item : h.parentOf) byOrder.push_back(item.first);
        std::stable_sort(byOrder.begin(), byOrder.end(), [&](int64_t a, int64_t b){ return graph.at(a).order < graph.at(b).order; });
        for (int64_t go : byOrder){
            const int64_t parent = h.parentOf.at(go);
            if (parent == 0){
                h.roots.push_back(go);
                continue;
            }
            std::vector<int64_t>& list = h.children[parent];
            if (std::find(list.begin(), list.end(), go) == list.end()) list.push_back(go);
        }
        return h;
    }

    std::string nameOf(const Graph& graph, int64_t gameObject){
        auto it = graph.find(gameObject);
        return it == graph.end() ? std::string() : readString(it->second.body, "m_Name");
    }

    std::string pathOf(const Graph& graph, const Hierarchy& h, int64_t gameObject){
        std::string path = nameOf(graph, gameObject);
        auto it = h.parentOf.find(gameObject);
        while (it != h.parentOf.end() && it->second != 0){
            path = nameOf(graph, it->second) + "/" + path;
            it = h.parentOf.find(it->second);
        }
        return path;
    }

    bool hasSystem(const Graph& graph, int64_t gameObject){
        return componentOfClass(graph, gameObject, CLASS_PARTICLESYSTEM) != nullptr;
    }

    bool subtreeHasSystem(const Graph& graph, const Hierarchy& h, int64_t gameObject){
        if (hasSystem(graph, gameObject)) return true;
        auto it = h.children.find(gameObject);
        if (it == h.children.end()) return false;
        for (int64_t child : it->second){
            if (subtreeHasSystem(graph, h, child)) return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ values

    // Unity's AnimationCurve ({m_Curve: [{time, value, inSlope, outSlope, weightedMode}],
    // m_PreInfinity, m_PostInfinity}); weighted keys become the plain Hermite (noted)
    Curve readCurve(const YAML::Node& node, Context& ctx, const std::string& where){
        Curve curve;
        if (!node || !node.IsMap()) return Curve::constant(1.0f);
        const YAML::Node keys = node["m_Curve"];
        bool weighted = false;
        if (keys && keys.IsSequence()){
            for (const YAML::Node& k : keys){
                CurveKey key;
                key.time = readFloat(k, "time");
                key.value = readFloat(k, "value");
                key.inTangent = readFloat(k, "inSlope");
                key.outTangent = readFloat(k, "outSlope");
                if (readInt(k, "weightedMode") != 0) weighted = true;
                curve.keys.push_back(key);
            }
        }
        // Unity's internal wrap: 0 ping pong, 1 repeat, 2 clamp
        auto wrap = [](int mode){
            return mode == 0 ? CurveWrapMode::PingPong : mode == 1 ? CurveWrapMode::Loop : CurveWrapMode::Clamp;
        };
        curve.preWrapMode = wrap(readInt(node, "m_PreInfinity", 2));
        curve.postWrapMode = wrap(readInt(node, "m_PostInfinity", 2));
        curve.normalize();
        if (curve.keys.empty()) curve = Curve::constant(0.0f);
        if (weighted) note(ctx, "weighted-tangents", where + ": weighted tangents became plain (Hermite) tangents");
        return curve;
    }

    // Unity's MinMaxCurve ({minMaxState, scalar, minScalar, maxCurve, minCurve}) times `scale`
    MinMaxCurve readMinMaxCurve(const YAML::Node& node, Context& ctx, const std::string& where, float scale = 1.0f, float fallback = 0.0f){
        if (!node || !node.IsMap()) return MinMaxCurve(fallback * scale);
        const int state = readInt(node, "minMaxState");
        const float scalar = readFloat(node, "scalar", fallback);
        const float minScalar = readFloat(node, "minScalar", scalar);
        switch (state){
            case 1: return MinMaxCurve::fromCurve(readCurve(node["maxCurve"], ctx, where), scalar * scale);
            case 2: {
                // S20's Two Curves: curve the first (Unity's min), curveMax the second
                MinMaxCurve value = MinMaxCurve::fromCurve(readCurve(node["minCurve"], ctx, where), scalar * scale);
                value.mode = particles::CurveMode::TwoCurves;
                value.curveMax = readCurve(node["maxCurve"], ctx, where);
                return value;
            }
            case 3: return MinMaxCurve::twoConstants(minScalar * scale, scalar * scale);
            default: return MinMaxCurve(scalar * scale);
        }
    }

    // Unity's Gradient ({key0..7, ctime0..7, atime0..7 in 0..65535, m_Mode, m_NumColorKeys,
    // m_NumAlphaKeys}): its colours are sRGB (gamma) as S27's keys are, blended there as Unity does
    Gradient readGradient(const YAML::Node& node){
        if (!node || !node.IsMap()) return Gradient();
        Gradient g;
        const int colors = std::min(8, readInt(node, "m_NumColorKeys", 2));
        const int alphas = std::min(8, readInt(node, "m_NumAlphaKeys", 2));
        for (int i = 0; i < colors; i++){
            const std::string k = "key" + std::to_string(i), t = "ctime" + std::to_string(i);
            const Vector4 c = readColor(node, k.c_str());
            g.colorKeys.push_back(GradientColorKey{readFloat(node, t.c_str()) / 65535.0f, Vector3(std::max(0.0f, c.x), std::max(0.0f, c.y), std::max(0.0f, c.z))});
        }
        for (int i = 0; i < alphas; i++){
            const std::string k = "key" + std::to_string(i), t = "atime" + std::to_string(i);
            g.alphaKeys.push_back(GradientAlphaKey{readFloat(node, t.c_str()) / 65535.0f, readColor(node, k.c_str()).w});
        }
        g.mode = readInt(node, "m_Mode") == 1 ? GradientMode::Fixed : GradientMode::Blend;
        g.normalize();
        return g;
    }

    // a Unity colour (sRGB, alpha) as S20's (sRGB, alpha)
    Vector4 srgbColor(const Vector4& c){
        return Vector4(std::max(0.0f, c.x), std::max(0.0f, c.y), std::max(0.0f, c.z), c.w);
    }

    MinMaxGradient readMinMaxGradient(const YAML::Node& node){
        if (!node || !node.IsMap()) return MinMaxGradient();
        const Vector4 minColor = srgbColor(readColor(node, "minColor"));
        const Vector4 maxColor = srgbColor(readColor(node, "maxColor"));
        MinMaxGradient value;
        switch (readInt(node, "minMaxState")){
            case 1:
                value.mode = particles::GradientValueMode::Gradient;
                value.gradient = readGradient(node["maxGradient"]);
                return value;
            case 2:
                value.mode = particles::GradientValueMode::TwoColors;
                value.color = minColor;
                value.colorMax = maxColor;
                return value;
            case 3:
                value.mode = particles::GradientValueMode::TwoGradients;
                value.gradient = readGradient(node["minGradient"]);
                value.gradientMax = readGradient(node["maxGradient"]);
                return value;
            case 4:
                value.mode = particles::GradientValueMode::RandomColor;
                value.gradient = readGradient(node["maxGradient"]);
                return value;
            default:
                return MinMaxGradient(maxColor);
        }
    }

    MinMaxCurve negated(MinMaxCurve c){
        if (c.usesCurve()){
            c.multiplier = -c.multiplier;
        }else{
            c.constant = -c.constant;
            c.constantMax = -c.constantMax;
        }
        return c;
    }

    // Euler degrees applied X, then Y, then Z of a quaternion (q = qz * qy * qx), from its
    // rotation matrix (R = Rz Ry Rx), gimbal lock (Y at +-90) included
    Vector3 eulerXYZDegrees(const Quaternion& q){
        const float r00 = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
        const float r01 = 2.0f * (q.x * q.y - q.w * q.z);
        const float r02 = 2.0f * (q.x * q.z + q.w * q.y);
        const float r10 = 2.0f * (q.x * q.y + q.w * q.z);
        const float r20 = 2.0f * (q.x * q.z - q.w * q.y);
        const float r21 = 2.0f * (q.y * q.z + q.w * q.x);
        const float r22 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
        Vector3 e;
        if (std::fabs(r20) < 0.99999f){
            e.x = std::atan2(r21, r22);
            e.y = std::asin(-r20);
            e.z = std::atan2(r10, r00);
        }else if (r20 < 0.0f){
            e.x = std::atan2(r01, r02);
            e.y = PI * 0.5f;
            e.z = 0.0f;
        }else{
            e.x = std::atan2(-r01, -r02);
            e.y = -PI * 0.5f;
            e.z = 0.0f;
        }
        return e * (180.0f / PI);
    }

    Quaternion axisAngle(const Vector3& axis, float degrees){
        const float half = degrees * PI / 360.0f;
        return Quaternion(std::cos(half), axis.x * std::sin(half), axis.y * std::sin(half), axis.z * std::sin(half));
    }

    // a module of a ParticleSystem document; an empty map when the file has none (a missing
    // key of a const node is invalid: looking into it would throw)
    YAML::Node moduleOf(const YAML::Node& ps, const char* name){
        const YAML::Node module = ps[name];
        if (module && module.IsMap()) return module;
        return YAML::Node(YAML::NodeType::Map);
    }

    // S = diag(-1, 1, 1) on a rotation: (w, x, y, z) -> (w, x, -y, -z)
    Quaternion mirrored(const Quaternion& q){
        return Quaternion(q.w, q.x, -q.y, -q.z);
    }

    std::string sanitize(const std::string& name){
        std::string out;
        for (char c : name){
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') out += '_';
            else out += c;
        }
        return out.empty() ? std::string("Unnamed") : out;
    }

}

void jasperengine::editor::unity::convertShapeRotation(const float unityEuler[3], bool edge, float engineEuler[3]){
    // Unity's Euler: Z, then X, then Y (q = qy * qx * qz)
    const Quaternion qx = axisAngle(Vector3(1, 0, 0), unityEuler[0]);
    const Quaternion qy = axisAngle(Vector3(0, 1, 0), unityEuler[1]);
    const Quaternion qz = axisAngle(Vector3(0, 0, 1), unityEuler[2]);
    const Quaternion unity = qy * (qx * qz);
    // C = Rx(90) * Ry(180): the engine shape's (x, y, z) are Unity's (x, z, y); Unity's Edge
    // emits along its +Y (the engine's +Z after C), the engine's along +Y: one more Rx(90)
    Quaternion c = axisAngle(Vector3(1, 0, 0), 90.0f) * axisAngle(Vector3(0, 1, 0), 180.0f);
    if (edge) c = c * axisAngle(Vector3(1, 0, 0), 90.0f);
    const Quaternion engine = mirrored(unity) * c;
    // S20's shape order (Z, then X, then Y) of the engine's turn
    const Vector3 e = upstreamparticles::shapeRotation(eulerXYZDegrees(engine));
    engineEuler[0] = e.x;
    engineEuler[1] = e.y;
    engineEuler[2] = e.z;
}

namespace {

    // ------------------------------------------------------------------ materials and textures

    enum class Blend { Alpha, Additive, Multiply, Premultiplied, Opaque };

    bool blendOfShaderName(const std::string& name, Blend& blend){
        static const std::map<std::string, Blend> table = {
            {"Mobile/Particles/Additive", Blend::Additive}, {"Legacy Shaders/Particles/Additive", Blend::Additive},
            {"Particles/Additive", Blend::Additive}, {"Particles/Additive (Soft)", Blend::Additive},
            {"Legacy Shaders/Particles/Additive (Soft)", Blend::Additive}, {"UI/Additive", Blend::Additive},
            {"Mobile/Particles/Alpha Blended", Blend::Alpha}, {"Legacy Shaders/Particles/Alpha Blended", Blend::Alpha},
            {"Particles/Alpha Blended", Blend::Alpha}, {"Particles/Standard Unlit", Blend::Alpha},
            {"Particles/Standard Surface", Blend::Alpha}, {"Particles/Multiply", Blend::Multiply},
            {"Legacy Shaders/Particles/Multiply", Blend::Multiply}, {"Mobile/Particles/Multiply", Blend::Multiply},
            {"Particles/Alpha Blended Premultiply", Blend::Premultiplied},
            {"Legacy Shaders/Particles/Alpha Blended Premultiply", Blend::Premultiplied},
            {"Simple/Unlit Vertex Color_Alpha", Blend::Alpha},
        };
        auto it = table.find(name);
        if (it == table.end()) return false;
        blend = it->second;
        return true;
    }

    // a custom shader's blend from its source: Blend One One, DstColor SrcAlpha / Zero, SrcAlpha
    // OneMinusSrcAlpha, or none with ZWrite On (opaque)
    std::string lowercase(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    // Unity's UnityEngine.Rendering.BlendMode values (what _SrcBlend / _DstBlend hold) by name
    int blendFactorOfName(const std::string& name){
        static const std::map<std::string, int> factors = {
            {"zero", 0}, {"one", 1}, {"dstcolor", 2}, {"srccolor", 3}, {"oneminusdstcolor", 4}, {"srcalpha", 5},
            {"oneminussrccolor", 6}, {"dstalpha", 7}, {"oneminusdstalpha", 8}, {"srcalphasaturate", 9}, {"oneminussrcalpha", 10}};
        auto it = factors.find(lowercase(name));
        return it == factors.end() ? -1 : it->second;
    }

    // the blend of a (source, destination) factor pair; false when no engine blend matches it
    bool blendOfFactors(int src, int dst, Blend& blend){
        if (src == 1 && dst == 0) { blend = Blend::Opaque; return true; }
        if (dst == 1 && (src == 1 || src == 5)) { blend = Blend::Additive; return true; }
        if (src == 5 && dst == 10) { blend = Blend::Alpha; return true; }
        if (src == 1 && dst == 10) { blend = Blend::Premultiplied; return true; }
        if ((src == 2 && (dst == 0 || dst == 5 || dst == 3)) || (src == 0 && dst == 3)) { blend = Blend::Multiply; return true; }
        return false;
    }

    // a material's float by name (m_SavedProperties.m_Floats), `fallback` when missing
    float materialFloat(const YAML::Node& saved, const char* name, float fallback);

    // the blend of a material's _SrcBlend / _DstBlend (or Unity's Standard particle _Mode)
    bool blendOfMaterialFloats(const YAML::Node& saved, Blend& blend){
        const float src = materialFloat(saved, "_SrcBlend", -1.0f), dst = materialFloat(saved, "_DstBlend", -1.0f);
        if (src >= 0.0f && dst >= 0.0f && blendOfFactors((int)src, (int)dst, blend)) return true;
        // Particles/Standard Unlit / Surface: 0 opaque, 1 cutout, 2 fade, 3 transparent,
        // 4 additive, 5 subtractive, 6 modulate
        switch ((int)materialFloat(saved, "_Mode", -1.0f)){
            case 0: case 1: blend = Blend::Opaque; return true;
            case 2: blend = Blend::Alpha; return true;
            case 3: blend = Blend::Premultiplied; return true;
            case 4: case 5: blend = Blend::Additive; return true;
            case 6: blend = Blend::Multiply; return true;
            default: return false;
        }
    }

    // A custom shader's blend from its source: its first `Blend <src> <dst>` line, factors by
    // name (any case) or by material property ([_SrcBlend] [_DstBlend]: `byProperties`); none
    // with ZWrite On: opaque
    bool blendOfShaderSource(const fs::path& file, std::string& shaderName, Blend& blend, bool& byProperties){
        byProperties = false;
        std::ifstream in(file);
        if (!in) return false;
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        const size_t quote = text.find("Shader \"");
        if (quote != std::string::npos){
            const size_t end = text.find('"', quote + 8);
            if (end != std::string::npos) shaderName = text.substr(quote + 8, end - quote - 8);
        }
        std::istringstream lines(text);
        std::string line;
        bool zwriteOn = false;
        while (std::getline(lines, line)){
            // a line comment says nothing
            const size_t comment = line.find("//");
            if (comment != std::string::npos) line.erase(comment);
            std::istringstream words(line);
            std::vector<std::string> tokens;
            std::string word;
            while (words >> word){
                word.erase(std::remove(word.begin(), word.end(), ','), word.end());
                if (!word.empty()) tokens.push_back(word);
            }
            for (size_t i = 0; i + 1 < tokens.size(); i++){
                const std::string key = lowercase(tokens[i]);
                if (key == "zwrite" && lowercase(tokens[i + 1]) == "on") zwriteOn = true;
                if (key != "blend" || i + 2 >= tokens.size()) continue;
                const std::string& a = tokens[i + 1];
                const std::string& b = tokens[i + 2];
                if (lowercase(a) == "off") { blend = Blend::Opaque; return true; }
                if (a.front() == '[' || b.front() == '['){
                    byProperties = true;
                    return true;
                }
                if (blendOfFactors(blendFactorOfName(a), blendFactorOfName(b), blend)) return true;
            }
        }
        if (zwriteOn){
            blend = Blend::Opaque;
            return true;
        }
        return false;
    }

    // the `_Name` entry of a material's saved property list ([{_Name: value}, ...])
    YAML::Node savedProperty(const YAML::Node& list, const char* name){
        if (!list || !list.IsSequence()) return YAML::Node();
        for (const YAML::Node& entry : list){
            if (entry.IsMap() && entry[name]) return entry[name];
        }
        return YAML::Node();
    }

    float materialFloat(const YAML::Node& saved, const char* name, float fallback){
        float value = fallback;
        const YAML::Node node = savedProperty(saved["m_Floats"], name);
        if (node && scalarFloat(node, value)) return value;
        return fallback;
    }

    // a texture copied into dest/textures (A8 ones expanded to white + alpha); its engine path
    std::string importTexture(Context& ctx, const fs::path& source){
        const std::string key = source.generic_string();
        auto cached = ctx.textureCache.find(key);
        if (cached != ctx.textureCache.end()){
            replayNotes(ctx, cached->second.notes);
            return cached->second.path;
        }
        const size_t mark = noteMark(ctx);
        auto finish = [&](const std::string& path){
            ctx.textureCache[key] = {path, notesSince(ctx, mark)};
            return path;
        };
        std::string ext = source.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        Json entry = {{"unity", ctx.project.relative(source)}};
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".tga"){
            note(ctx, "texture-format", ctx.project.relative(source) + ": a " + ext + " texture is not copied (convert it to PNG)");
            entry["status"] = "texture-format";
            ctx.textures.push_back(entry);
            return finish("");
        }
        // Unity's Alpha8 import: the image's alpha (or grey) is all it keeps
        bool alpha8 = false;
        const YAML::Node meta = ctx.project.metaOf(source);
        const YAML::Node importer = meta ? meta["TextureImporter"] : YAML::Node();
        if (importer && importer["platformSettings"] && importer["platformSettings"].IsSequence()){
            for (const YAML::Node& platform : importer["platformSettings"]){
                if (readString(platform, "buildTarget") == "DefaultTexturePlatform" && readInt(platform, "textureFormat", -1) == 1) alpha8 = true;
            }
        }
        if (importer && readInt(importer, "textureFormat", -1) == 1) alpha8 = true;
        const std::string rel = claimOutput(ctx, ctx.destRel + "/textures", source.stem().string(), alpha8 ? std::string(".png") : source.extension().string(), key);
        std::error_code exists;
        if (!ctx.options.overwrite && fs::exists(ctx.engineRoot / rel, exists)){
            // an earlier import's (or the user's) file: kept
            entry["file"] = rel;
            entry["reused"] = true;
            ctx.textures.push_back(entry);
            return finish(rel);
        }
        std::string bytes;
        if (alpha8){
            int w = 0, h = 0, n = 0;
            unsigned char* pixels = stbi_load(source.string().c_str(), &w, &h, &n, 4);
            if (pixels){
                for (int i = 0; i < w * h; i++){
                    unsigned char* p = pixels + i * 4;
                    const unsigned char a = n == 4 || n == 2 ? p[3] : (unsigned char)((p[0] * 30 + p[1] * 59 + p[2] * 11) / 100);
                    p[0] = p[1] = p[2] = 255;
                    p[3] = a;
                }
                stbi_write_png_to_func([](void* context, void* data, int size){
                    static_cast<std::string*>(context)->append(static_cast<const char*>(data), (size_t)size);
                }, &bytes, w, h, 4, pixels, w * 4);
                stbi_image_free(pixels);
            }
            entry["alpha8"] = true;
        }else{
            std::ifstream in(source, std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            bytes = buffer.str();
        }
        if (bytes.empty()){
            note(ctx, "texture-format", ctx.project.relative(source) + ": the image could not be read");
            return finish("");
        }
        ctx.files.push_back({ctx.engineRoot / rel, bytes});
        entry["file"] = rel;
        ctx.textures.push_back(entry);
        return finish(rel);
    }

    // the engine material of a renderer's first Unity material
    Material importMaterial(Context& ctx, const Ref& ref){
        Material material;
        material.shader = "library:unlit";
        material.blendMode = MaterialBlendMode::ALPHA;
        material.depthWrite = MaterialDepthWrite::DISABLED;
        material.cullMode = MaterialCullMode::OFF;

        const BuiltinAsset* builtin = UnityProject::builtin(ref);
        if (ref.isNull() || (builtin && builtin->kind == BuiltinAsset::Material)){
            // the default particle materials: the built-in soft particle texture, alpha
            material.name = "";
            material.shaderFeatures = {"PARTICLE_TEXTURE"};
            return material;
        }
        if (UnityProject::isBuiltinGuid(ref.guid)){
            note(ctx, "material-unknown", "built-in material id " + std::to_string(ref.fileID) + " is unknown: the default particle material is used");
            material.shaderFeatures = {"PARTICLE_TEXTURE"};
            return material;
        }
        const fs::path file = ctx.project.pathOf(ref.guid);
        const std::string key = file.generic_string();
        auto cached = ctx.materialCache.find(key);
        if (cached != ctx.materialCache.end()){
            replayNotes(ctx, cached->second.notes);
            return cached->second.material;
        }
        const size_t mark = noteMark(ctx);
        if (file.empty()){
            note(ctx, "material-missing", "material guid " + ref.guid + " is not in the project: the default particle material is used");
            material.shaderFeatures = {"PARTICLE_TEXTURE"};
            return material;
        }

        YamlFile yaml;
        std::string error;
        const Document* doc = nullptr;
        if (yaml.load(file, error)){
            for (const Document& d : yaml.documents){
                if (d.classId == CLASS_MATERIAL){ doc = &d; break; }
            }
        }
        Json entry = {{"unity", ctx.project.relative(file)}};
        if (!doc){
            note(ctx, "material-missing", ctx.project.relative(file) + ": not a readable material");
            material.shaderFeatures = {"PARTICLE_TEXTURE"};
            return material;
        }
        const std::string unityName = readString(doc->body, "m_Name", file.stem().string());

        // the shader: a built-in, a known name, or a custom one approximated by its Blend
        const Ref shaderRef = readRef(doc->body, "m_Shader");
        std::string shaderName;
        Blend blend = Blend::Alpha;
        bool known = false;
        bool legacyTint = false;
        const YAML::Node saved = moduleOf(doc->body, "m_SavedProperties");
        if (const BuiltinAsset* shader = UnityProject::builtin(shaderRef); shader && shader->kind == BuiltinAsset::Shader){
            shaderName = shader->name;
            // Unity's Standard particle shaders blend by the material's own settings
            known = (shaderName.find("Standard") != std::string::npos && blendOfMaterialFloats(saved, blend)) ||
                    blendOfShaderName(shaderName, blend);
        }else if (!shaderRef.guid.empty() && !UnityProject::isBuiltinGuid(shaderRef.guid)){
            const fs::path shaderFile = ctx.project.pathOf(shaderRef.guid);
            bool byProperties = false;
            if (!shaderFile.empty() && blendOfShaderSource(shaderFile, shaderName, blend, byProperties) &&
                (!byProperties || blendOfMaterialFloats(saved, blend))){
                if (!blendOfShaderName(shaderName, blend)){
                    note(ctx, "shader-approximated", "material " + unityName + ": the shader '" + shaderName +
                         "' is drawn with library:unlit by its Blend state");
                }
                known = true;
            }else{
                note(ctx, "shader-approximated", "material " + unityName + ": the shader " +
                     (shaderName.empty() ? ctx.project.relative(shaderFile) : "'" + shaderName + "'") + " has no readable Blend: alpha blended");
            }
        }else if (blendOfMaterialFloats(saved, blend)){
            known = true;
            note(ctx, "shader-approximated", "material " + unityName + ": built-in shader id " + std::to_string(shaderRef.fileID) +
                 " is unknown: blended by the material's _SrcBlend / _DstBlend");
        }else{
            note(ctx, "shader-approximated", "material " + unityName + ": built-in shader id " + std::to_string(shaderRef.fileID) + " is unknown: alpha blended");
        }
        if (!known && shaderName.empty()) shaderName = "(unknown)";
        legacyTint = shaderName.find("Particles/") != std::string::npos && shaderName.find("Mobile/") == std::string::npos &&
                     shaderName.find("Standard") == std::string::npos;

        switch (blend){
            case Blend::Additive: material.blendMode = MaterialBlendMode::ADDITIVE; break;
            case Blend::Multiply: material.blendMode = MaterialBlendMode::MULTIPLY; break;
            case Blend::Premultiplied: material.blendMode = MaterialBlendMode::PREMULTIPLIED; break;
            case Blend::Opaque:
                material.blendMode = MaterialBlendMode::NONE;
                material.depthWrite = MaterialDepthWrite::ENABLED;
                break;
            case Blend::Alpha: break;
        }

        const YAML::Node mainTex = savedProperty(saved["m_TexEnvs"], "_MainTex");
        const Ref textureRef = readRef(mainTex, "m_Texture");
        if (const BuiltinAsset* texture = UnityProject::builtin(textureRef); texture && texture->kind == BuiltinAsset::Texture){
            material.shaderFeatures = {"PARTICLE_TEXTURE"};
        }else if (!textureRef.guid.empty()){
            const fs::path texture = ctx.project.pathOf(textureRef.guid);
            if (texture.empty()){
                note(ctx, "texture-missing", "material " + unityName + ": its _MainTex (guid " + textureRef.guid +
                     ") is not in the project: the built-in particle texture is used");
            }
            const std::string rel = texture.empty() ? std::string() : importTexture(ctx, texture);
            if (!rel.empty()){
                material.shaderTextures = {{"u_baseTexture", Texture(rel)}};
                entry["texture"] = rel;
            }else{
                // not copied: the soft particle rather than a white quad
                material.shaderFeatures = {"PARTICLE_TEXTURE"};
            }
        }else{
            material.shaderFeatures = {"PARTICLE_TEXTURE"};
        }
        const Vector3 scale = readVector3(mainTex, "m_Scale", Vector3(1, 1, 0));
        const Vector3 offset = readVector3(mainTex, "m_Offset", Vector3(0, 0, 0));
        if (scale.x != 1.0f || scale.y != 1.0f || offset.x != 0.0f || offset.y != 0.0f){
            ShaderUniforms::set(material.shaderParams, "tiling", Vector4(scale.x, scale.y, offset.x, offset.y));
        }
        // _TintColor (x 2 in Unity's legacy particle shaders) or _Color: the base colour
        YAML::Node tint = savedProperty(saved["m_Colors"], "_TintColor");
        const bool hasTint = tint && tint.IsMap();
        if (!hasTint) tint.reset(savedProperty(saved["m_Colors"], "_Color"));
        if (tint && tint.IsMap()){
            Vector4 c(readFloat(tint, "r", 1), readFloat(tint, "g", 1), readFloat(tint, "b", 1), readFloat(tint, "a", 1));
            if (hasTint && legacyTint) c = c * 2.0f;
            // gamma to linear past 1 too (an HDR tint)
            auto lin = [](float v){ return v <= 1.0f ? Color::sRGBToLinear(Vector3(v, 0, 0)).x : std::pow(v, 2.2f); };
            const Vector4 base(lin(std::max(0.0f, c.x)), lin(std::max(0.0f, c.y)), lin(std::max(0.0f, c.z)), std::min(1.0f, c.w));
            if (base != Vector4(1, 1, 1, 1)) ShaderUniforms::set(material.shaderParams, "baseColor", base);
        }
        // soft particles: _InvFade -> the SOFT feature (softness its inverse)
        const YAML::Node invFade = savedProperty(saved["m_Floats"], "_InvFade");
        float invFadeValue = 0.0f;
        if (invFade && scalarFloat(invFade, invFadeValue) && invFadeValue > 0.0f && shaderName.find("Soft") != std::string::npos){
            material.shaderFeatures.push_back("SOFT");
            ShaderUniforms::set(material.shaderParams, "softness", Vector4(1.0f / invFadeValue, 0, 0, 0));
        }

        // no file of its own: the systems' Renderer modules take its values (S20)
        material.name = unityName;
        entry["shader"] = shaderName;
        entry["blend"] = blend == Blend::Additive ? "additive" : blend == Blend::Multiply ? "multiply" :
                         blend == Blend::Premultiplied ? "premultiplied" : blend == Blend::Opaque ? "opaque" : "alpha";
        ctx.materials.push_back(entry);
        ctx.materialCache[key] = {material, notesSince(ctx, mark)};
        return material;
    }

    // ------------------------------------------------------------------ the system

    const char* const UNSUPPORTED_MODULES[] = {"SubModule", "CollisionModule", "TriggerModule", "LightsModule", "TrailModule",
                                               "CustomDataModule", "ExternalForcesModule", "LifetimeByEmitterSpeedModule"};

    // the S20 shape of Unity's shape type (spec of ParticleSystemShapeType); false: not supported
    bool shapeOf(int type, particles::ShapeModule& s, float& thicknessOverride, bool& edge){
        using particles::ShapeType;
        using particles::ShapeEmitFrom;
        thicknessOverride = -1.0f;
        edge = false;
        s.emitFrom = ShapeEmitFrom::Volume;
        switch (type){
            case 0: s.shape = ShapeType::Sphere; break;
            case 1: s.shape = ShapeType::Sphere; thicknessOverride = 0.0f; break;
            case 2: s.shape = ShapeType::Hemisphere; break;
            case 3: s.shape = ShapeType::Hemisphere; thicknessOverride = 0.0f; break;
            case 4: s.shape = ShapeType::Cone; s.emitFrom = ShapeEmitFrom::Base; break;
            case 5: s.shape = ShapeType::Box; break;
            case 7: s.shape = ShapeType::Cone; s.emitFrom = ShapeEmitFrom::Base; thicknessOverride = 0.0f; break;
            case 8: s.shape = ShapeType::Cone; break;
            case 9: s.shape = ShapeType::Cone; thicknessOverride = 0.0f; break;
            case 10: s.shape = ShapeType::Circle; break;
            case 11: s.shape = ShapeType::Circle; thicknessOverride = 0.0f; break;
            case 12: s.shape = ShapeType::Edge; edge = true; break;
            case 15: s.shape = ShapeType::Box; s.emitFrom = ShapeEmitFrom::Shell; break;
            case 16: s.shape = ShapeType::Box; s.emitFrom = ShapeEmitFrom::Edge; break;
            case 17: s.shape = ShapeType::Donut; break;
            case 18: s.shape = ShapeType::Rectangle; break;
            default:
                s.shape = ShapeType::Cone;
                s.emitFrom = ShapeEmitFrom::Base;
                return false;
        }
        return true;
    }

    particles::ShapeMultiMode multiModeOf(int mode){
        switch (std::max(0, std::min(3, mode))){
            case 1: return particles::ShapeMultiMode::Loop;
            case 2: return particles::ShapeMultiMode::PingPong;
            case 3: return particles::ShapeMultiMode::BurstSpread;
            default: return particles::ShapeMultiMode::Random;
        }
    }

    particles::ValueSpace spaceOf(const YAML::Node& module){
        return readBool(module, "inWorldSpace") ? particles::ValueSpace::World : particles::ValueSpace::Local;
    }

    // a Unity Sorting Layer id as its 32 bits: TagManager writes uniqueID unsigned, a renderer's
    // m_SortingLayerID signed (3184598343 and -1110369953 are one layer)
    uint32_t unitySortingLayerBits(const YAML::Node& value){
        return value ? (uint32_t)std::strtoll(value.Scalar().c_str(), nullptr, 10) : 0u;
    }

    // a Unity Sorting Layer's name by its unique id (ProjectSettings/TagManager.asset m_SortingLayers),
    // "" when the project does not list it
    std::string unitySortingLayerName(Context& ctx, uint32_t uniqueId){
        YamlFile tags;
        std::string error;
        if (!tags.load(ctx.project.root() / "ProjectSettings" / "TagManager.asset", error))
            return std::string();
        for (const Document& document : tags.documents){
            const YAML::Node layers = document.body["m_SortingLayers"];
            if (!layers || !layers.IsSequence())
                continue;
            for (const YAML::Node& layer : layers){
                if (layer["uniqueID"] && unitySortingLayerBits(layer["uniqueID"]) == uniqueId && layer["name"])
                    return layer["name"].Scalar();
            }
        }
        return std::string();
    }

    // a Unity ParticleSystem as an S20 effect (and the component's own Play On Awake and seed);
    // `modules` the report
    void convertSystem(const YAML::Node& ps, ParticleEffectDocument& d, ParticleSystemComponent& c, Json& modules, Context& ctx){
        using namespace particles;
        d = ParticleEffectDocument();
        auto status = [&](const char* unityModule, const char* status){ modules[unityModule] = status; };

        // main
        MainModule& m = d.main;
        d.duration = std::max(0.05f, readFloat(ps, "lengthInSec", 5.0f));
        d.looping = readBool(ps, "looping", true);
        d.prewarm = readBool(ps, "prewarm");
        c.playOnAwake = readBool(ps, "playOnAwake", true);
        d.simulationSpeed = readFloat(ps, "simulationSpeed", 1.0f);
        d.startDelay = readMinMaxCurve(ps["startDelay"], ctx, "startDelay");
        if (d.startDelay.usesCurve()){
            d.startDelay = MinMaxCurve(d.startDelay.curve.evaluate(0.0f) * d.startDelay.multiplier);
            note(ctx, "start-delay-curve", "a curve start delay starts at its value at 0");
        }
        // 0: a new seed at every play (Unity's Auto Random Seed)
        if (!readBool(ps, "autoRandomSeed", true)){
            const int seed = readInt(ps, "randomSeed");
            c.seed = seed == 0 ? 1u : (uint32_t)seed;
        }
        const int stopAction = readInt(ps, "stopAction");
        d.stopAction = (stopAction >= 0 && stopAction <= 3) ? (StopAction)stopAction : StopAction::None;
        const int space = readInt(ps, "moveWithTransform");
        d.simulationSpace = space == 0 ? SimulationSpace::Local : SimulationSpace::World;
        if (space == 2) note(ctx, "custom-simulation-space", "a custom simulation space is simulated in World space");
        const int scaling = readInt(ps, "scalingMode", 1);
        d.scalingMode = (scaling >= 0 && scaling <= 2) ? (ScalingMode)scaling : ScalingMode::Local;
        d.useUnscaledTime = readBool(ps, "useUnscaledTime");
        d.emitterVelocityMode = readInt(ps, "emitterVelocityMode") == 1 ? EmitterVelocityMode::Rigidbody : EmitterVelocityMode::Transform;
        const int culling = readInt(ps, "cullingMode");
        // Unity's Automatic, Pause and Catch-up, Pause, Always Simulate
        switch (culling){
            case 1: d.cullingMode = particles::CullingMode::PauseAndCatchUp; break;
            case 2: d.cullingMode = particles::CullingMode::Pause; break;
            case 3: d.cullingMode = particles::CullingMode::AlwaysSimulate; break;
            default: d.cullingMode = particles::CullingMode::Automatic; break;
        }
        if (readInt(ps, "ringBufferMode") != 0) note(ctx, "ring-buffer", "ring buffer mode is not supported: the oldest particles are not recycled");

        const YAML::Node init = moduleOf(ps, "InitialModule");
        m.startLifetime = readMinMaxCurve(init["startLifetime"], ctx, "startLifetime", 1.0f, 5.0f);
        m.startSpeed = readMinMaxCurve(init["startSpeed"], ctx, "startSpeed", 1.0f, 5.0f);
        m.startColor = readMinMaxGradient(init["startColor"]);
        m.startSize3D = readBool(init, "size3D");
        m.startSize = readMinMaxCurve(init["startSize"], ctx, "startSize", 1.0f, 1.0f);
        m.startSizeY = readMinMaxCurve(init["startSizeY"], ctx, "startSizeY", 1.0f, 1.0f);
        m.startSizeZ = readMinMaxCurve(init["startSizeZ"], ctx, "startSizeZ", 1.0f, 1.0f);
        m.startRotation3D = readBool(init, "rotation3D");
        // radians to degrees; (x, y, z) -> (x, -y, -z) (S), the roll's sign by ROTATION_SIGN
        m.startRotation = readMinMaxCurve(init["startRotation"], ctx, "startRotation", ROTATION_SIGN * RAD_TO_DEG);
        m.startRotationX = readMinMaxCurve(init["startRotationX"], ctx, "startRotationX", RAD_TO_DEG);
        m.startRotationY = negated(readMinMaxCurve(init["startRotationY"], ctx, "startRotationY", RAD_TO_DEG));
        m.flipRotation = readFloat(init, "randomizeRotationDirection");
        m.gravityModifier = readMinMaxCurve(init["gravityModifier"], ctx, "gravityModifier");
        // over the system's time in Unity
        if (m.gravityModifier.usesCurve()) m.gravityModifier.driver = CurveDriver::EmitterTime;
        m.gravitySource = readInt(init, "gravitySource") == 1 ? GravitySource::Physics2D : GravitySource::Physics3D;
        d.maxParticles = (uint32_t)std::max(0, std::min((int)kMaxParticlesPerEmitter, readInt(init, "maxNumParticles", 1000)));
        status("InitialModule", "imported");

        // emission
        const YAML::Node emission = moduleOf(ps, "EmissionModule");
        d.emission.enabled = readBool(emission, "enabled", true);
        d.emission.rateOverTime = readMinMaxCurve(emission["rateOverTime"], ctx, "rateOverTime", 1.0f, 10.0f);
        d.emission.rateOverDistance = readMinMaxCurve(emission["rateOverDistance"], ctx, "rateOverDistance");
        if (emission["m_Bursts"] && emission["m_Bursts"].IsSequence()){
            for (const YAML::Node& b : emission["m_Bursts"]){
                if (d.emission.bursts.size() >= kMaxBursts){
                    note(ctx, "bursts-capped", "more than " + std::to_string(kMaxBursts) + " bursts: the first " + std::to_string(kMaxBursts) + " are kept");
                    break;
                }
                Burst burst;
                burst.time = readFloat(b, "time");
                if (b["countCurve"]){
                    burst.count = readMinMaxCurve(b["countCurve"], ctx, "burst count");
                }else{
                    const float lo = (float)readInt(b, "minCount", 30), hi = (float)readInt(b, "maxCount", 30);
                    burst.count = lo == hi ? MinMaxCurve(lo) : MinMaxCurve::twoConstants(lo, hi);
                }
                burst.cycles = std::max(0, readInt(b, "cycleCount", 1));
                burst.interval = std::max(0.0001f, readFloat(b, "repeatInterval", 0.01f));
                burst.probability = std::min(1.0f, std::max(0.0f, readFloat(b, "probability", 1.0f)));
                d.emission.bursts.push_back(burst);
            }
        }
        status("EmissionModule", d.emission.enabled ? "imported" : "disabled");

        // shape (the shape frame converted; scale and box thickness (x, z, y))
        const YAML::Node shape = moduleOf(ps, "ShapeModule");
        ShapeModule& s = d.shape;
        s.enabled = readBool(shape, "enabled", true);
        const int type = readInt(shape, "type", 4);
        bool edge = false;
        float thicknessOverride = -1.0f;
        if (!shapeOf(type, s, thicknessOverride, edge) && s.enabled){
            note(ctx, "shape-unsupported", "shape type " + std::to_string(type) + " (a mesh, sprite or texture shape) is not supported: a cone is used");
        }
        const YAML::Node radius = shape["radius"];
        s.radius = radius && radius.IsMap() ? readFloat(radius, "value", 1.0f) : readFloat(shape, "radius", 1.0f);
        if (radius && radius.IsMap()){
            s.radiusMode = multiModeOf(readInt(radius, "mode"));
            s.radiusSpread = readFloat(radius, "spread");
            s.radiusSpeed = readMinMaxCurve(radius["speed"], ctx, "radius speed", 1.0f, 1.0f);
        }
        s.radiusThickness = thicknessOverride >= 0.0f ? thicknessOverride : readFloat(shape, "radiusThickness", 1.0f);
        const YAML::Node arc = shape["arc"];
        s.arc = arc && arc.IsMap() ? readFloat(arc, "value", 360.0f) : readFloat(shape, "arc", 360.0f);
        if (arc && arc.IsMap()){
            s.arcMode = multiModeOf(readInt(arc, "mode"));
            s.arcSpread = readFloat(arc, "spread");
            s.arcSpeed = readMinMaxCurve(arc["speed"], ctx, "arc speed", 1.0f, 1.0f);
        }
        s.angle = std::max(0.0f, std::min(90.0f, readFloat(shape, "angle", 25.0f)));
        s.length = readFloat(shape, "length", 5.0f);
        const Vector3 thickness = readVector3(shape, "boxThickness");
        s.boxThickness = Vector3(thickness.x, thickness.z, thickness.y);
        s.donutRadius = readFloat(shape, "donutRadius", 0.2f);
        const Vector3 position = readVector3(shape, "m_Position");
        s.position = Vector3(-position.x, position.y, position.z);
        const Vector3 rotation = readVector3(shape, "m_Rotation");
        const float unityEuler[3] = {rotation.x, rotation.y, rotation.z};
        float engineEuler[3];
        convertShapeRotation(unityEuler, edge, engineEuler);
        s.rotation = Vector3(engineEuler[0], engineEuler[1], engineEuler[2]);
        const Vector3 scale = readVector3(shape, "m_Scale", Vector3(1, 1, 1));
        s.scale = Vector3(scale.x, scale.z, scale.y);
        s.alignToDirection = readBool(shape, "alignToDirection");
        s.randomizeDirection = readFloat(shape, "randomDirectionAmount");
        s.spherizeDirection = readFloat(shape, "sphericalDirectionAmount");
        s.randomizePosition = readFloat(shape, "randomPositionAmount");
        if (!s.enabled){
            // Unity emits along the system's +Z without a shape: a point emitting straight there
            s = ShapeModule();
            s.shape = ShapeType::Cone;
            s.emitFrom = ShapeEmitFrom::Base;
            s.radius = 0.0f;
            s.angle = 0.0f;
            s.rotation = Vector3(90.0f, 0.0f, 0.0f);
            status("ShapeModule", "disabled");
        }else{
            status("ShapeModule", "imported");
        }

        // velocity over lifetime (vectors by S: x negated; angular speeds (x, -y, -z), in degrees)
        const YAML::Node vel = moduleOf(ps, "VelocityModule");
        VelocityOverLifetimeModule& v = d.velocityOverLifetime;
        v.enabled = readBool(vel, "enabled");
        v.present = v.enabled;
        v.x = negated(readMinMaxCurve(vel["x"], ctx, "velocity x"));
        v.y = readMinMaxCurve(vel["y"], ctx, "velocity y");
        v.z = readMinMaxCurve(vel["z"], ctx, "velocity z");
        v.orbitalX = readMinMaxCurve(vel["orbitalX"], ctx, "orbital x", RAD_TO_DEG);
        v.orbitalY = negated(readMinMaxCurve(vel["orbitalY"], ctx, "orbital y", RAD_TO_DEG));
        v.orbitalZ = negated(readMinMaxCurve(vel["orbitalZ"], ctx, "orbital z", RAD_TO_DEG));
        v.offsetX = negated(readMinMaxCurve(vel["orbitalOffsetX"], ctx, "orbital offset x"));
        v.offsetY = readMinMaxCurve(vel["orbitalOffsetY"], ctx, "orbital offset y");
        v.offsetZ = readMinMaxCurve(vel["orbitalOffsetZ"], ctx, "orbital offset z");
        v.radial = readMinMaxCurve(vel["radial"], ctx, "radial");
        v.speedModifier = readMinMaxCurve(vel["speedModifier"], ctx, "speed modifier", 1.0f, 1.0f);
        v.space = spaceOf(vel);
        status("VelocityModule", v.enabled ? "imported" : "disabled");

        const YAML::Node clamp = moduleOf(ps, "ClampVelocityModule");
        LimitVelocityModule& lim = d.limitVelocity;
        lim.enabled = readBool(clamp, "enabled");
        lim.present = lim.enabled;
        lim.separateAxes = readBool(clamp, "separateAxis");
        lim.limit = lim.separateAxes ? readMinMaxCurve(clamp["x"], ctx, "limit x", 1.0f, 1.0f) : readMinMaxCurve(clamp["magnitude"], ctx, "limit", 1.0f, 1.0f);
        lim.limitY = readMinMaxCurve(clamp["y"], ctx, "limit y", 1.0f, 1.0f);
        lim.limitZ = readMinMaxCurve(clamp["z"], ctx, "limit z", 1.0f, 1.0f);
        lim.space = spaceOf(clamp);
        lim.dampen = readFloat(clamp, "dampen", 1.0f);
        lim.drag = readMinMaxCurve(clamp["drag"], ctx, "drag");
        lim.multiplyBySize = readBool(clamp, "multiplyDragByParticleSize", true);
        lim.multiplyByVelocity = readBool(clamp, "multiplyDragByParticleVelocity", true);
        status("ClampVelocityModule", lim.enabled ? "imported" : "disabled");

        // inherit velocity: Main's here
        const YAML::Node inherit = moduleOf(ps, "InheritVelocityModule");
        if (readBool(inherit, "enabled")){
            m.inheritVelocityMode = readInt(inherit, "m_Mode") == 1 ? InheritVelocityMode::Current : InheritVelocityMode::Initial;
            m.inheritVelocity = readMinMaxCurve(inherit["m_Curve"], ctx, "inherit velocity");
            status("InheritVelocityModule", "imported");
        }else{
            status("InheritVelocityModule", "disabled");
        }

        const YAML::Node force = moduleOf(ps, "ForceModule");
        ForceOverLifetimeModule& f = d.forceOverLifetime;
        f.enabled = readBool(force, "enabled");
        f.present = f.enabled;
        f.x = negated(readMinMaxCurve(force["x"], ctx, "force x"));
        f.y = readMinMaxCurve(force["y"], ctx, "force y");
        f.z = readMinMaxCurve(force["z"], ctx, "force z");
        f.space = spaceOf(force);
        f.randomizePerTick = readBool(force, "randomizePerFrame");
        status("ForceModule", f.enabled ? "imported" : "disabled");

        const YAML::Node color = moduleOf(ps, "ColorModule");
        d.colorOverLifetime.enabled = readBool(color, "enabled");
        d.colorOverLifetime.present = d.colorOverLifetime.enabled;
        d.colorOverLifetime.color = readMinMaxGradient(color["gradient"]);
        status("ColorModule", d.colorOverLifetime.enabled ? "imported" : "disabled");
        const YAML::Node colorSpeed = moduleOf(ps, "ColorBySpeedModule");
        d.colorBySpeed.enabled = readBool(colorSpeed, "enabled");
        d.colorBySpeed.present = d.colorBySpeed.enabled;
        d.colorBySpeed.color = readMinMaxGradient(colorSpeed["gradient"]);
        { const Vector3 r = readVector3(colorSpeed, "range", Vector3(0, 1, 0)); d.colorBySpeed.speedRange = Vector2(r.x, r.y); }
        status("ColorBySpeedModule", d.colorBySpeed.enabled ? "imported" : "disabled");

        const YAML::Node size = moduleOf(ps, "SizeModule");
        SizeOverLifetimeModule& so = d.sizeOverLifetime;
        so.enabled = readBool(size, "enabled");
        so.present = so.enabled;
        so.separateAxes = readBool(size, "separateAxes");
        so.size = readMinMaxCurve(size["curve"], ctx, "size", 1.0f, 1.0f);
        so.sizeY = readMinMaxCurve(size["y"], ctx, "size y", 1.0f, 1.0f);
        so.sizeZ = readMinMaxCurve(size["z"], ctx, "size z", 1.0f, 1.0f);
        status("SizeModule", so.enabled ? "imported" : "disabled");
        const YAML::Node sizeSpeed = moduleOf(ps, "SizeBySpeedModule");
        SizeBySpeedModule& ss = d.sizeBySpeed;
        ss.enabled = readBool(sizeSpeed, "enabled");
        ss.present = ss.enabled;
        ss.separateAxes = readBool(sizeSpeed, "separateAxes");
        ss.size = readMinMaxCurve(sizeSpeed["curve"], ctx, "size by speed", 1.0f, 1.0f);
        ss.sizeY = readMinMaxCurve(sizeSpeed["y"], ctx, "size by speed y", 1.0f, 1.0f);
        ss.sizeZ = readMinMaxCurve(sizeSpeed["z"], ctx, "size by speed z", 1.0f, 1.0f);
        { const Vector3 r = readVector3(sizeSpeed, "range", Vector3(0, 1, 0)); ss.speedRange = Vector2(r.x, r.y); }
        status("SizeBySpeedModule", ss.enabled ? "imported" : "disabled");

        const YAML::Node rot = moduleOf(ps, "RotationModule");
        RotationOverLifetimeModule& ro = d.rotationOverLifetime;
        ro.enabled = readBool(rot, "enabled");
        ro.present = ro.enabled;
        ro.separateAxes = readBool(rot, "separateAxes");
        ro.angularVelocityX = readMinMaxCurve(rot["x"], ctx, "rotation x", RAD_TO_DEG);
        ro.angularVelocityY = negated(readMinMaxCurve(rot["y"], ctx, "rotation y", RAD_TO_DEG));
        ro.angularVelocity = readMinMaxCurve(rot["curve"], ctx, "rotation", ROTATION_SIGN * RAD_TO_DEG, 0.785398163f);
        status("RotationModule", ro.enabled ? "imported" : "disabled");
        const YAML::Node rotSpeed = moduleOf(ps, "RotationBySpeedModule");
        RotationBySpeedModule& rs = d.rotationBySpeed;
        rs.enabled = readBool(rotSpeed, "enabled");
        rs.present = rs.enabled;
        rs.separateAxes = readBool(rotSpeed, "separateAxes");
        rs.angularVelocityX = readMinMaxCurve(rotSpeed["x"], ctx, "rotation by speed x", RAD_TO_DEG);
        rs.angularVelocityY = negated(readMinMaxCurve(rotSpeed["y"], ctx, "rotation by speed y", RAD_TO_DEG));
        rs.angularVelocity = readMinMaxCurve(rotSpeed["curve"], ctx, "rotation by speed", ROTATION_SIGN * RAD_TO_DEG, 0.785398163f);
        { const Vector3 r = readVector3(rotSpeed, "range", Vector3(0, 1, 0)); rs.speedRange = Vector2(r.x, r.y); }
        status("RotationBySpeedModule", rs.enabled ? "imported" : "disabled");

        const YAML::Node noise = moduleOf(ps, "NoiseModule");
        NoiseModule& n = d.noise;
        n.enabled = readBool(noise, "enabled");
        n.present = n.enabled;
        n.separateAxes = readBool(noise, "separateAxes");
        n.strength = readMinMaxCurve(noise["strength"], ctx, "noise strength", 1.0f, 1.0f);
        n.strengthY = readMinMaxCurve(noise["strengthY"], ctx, "noise strength y", 1.0f, 1.0f);
        n.strengthZ = readMinMaxCurve(noise["strengthZ"], ctx, "noise strength z", 1.0f, 1.0f);
        n.frequency = std::max(0.0001f, readFloat(noise, "frequency", 0.5f));
        n.damping = readBool(noise, "damping", true);
        n.octaves = std::max(1, std::min(4, readInt(noise, "octaves", 1)));
        n.octaveMultiplier = readFloat(noise, "octaveMultiplier", 0.5f);
        n.octaveScale = readFloat(noise, "octaveScale", 2.0f);
        // Unity's quality: Low (2D), Medium, High
        const int quality = readInt(noise, "quality", 2);
        n.quality = quality == 0 ? NoiseQuality::Low : quality == 1 ? NoiseQuality::Medium : NoiseQuality::High;
        n.scrollSpeed = readMinMaxCurve(noise["scrollSpeed"], ctx, "noise scroll");
        n.positionAmount = readMinMaxCurve(noise["positionAmount"], ctx, "noise position", 1.0f, 1.0f);
        // degrees in Unity, radians per second at amount 1 here (as the roll the noise turns)
        n.rotationAmount = readMinMaxCurve(noise["rotationAmount"], ctx, "noise rotation", ROTATION_SIGN * PI / 180.0f);
        n.sizeAmount = readMinMaxCurve(noise["sizeAmount"], ctx, "noise size");
        if (n.enabled){
            note(ctx, "noise-pattern", "the noise uses the engine's own 3D noise: the same parameters, another pattern" +
                 std::string(readBool(noise, "remapEnabled") ? " (its remap curve is not imported)" : ""));
            status("NoiseModule", "approximated");
        }else{
            status("NoiseModule", "disabled");
        }

        const YAML::Node uv = moduleOf(ps, "UVModule");
        TextureSheetAnimationModule& sheet = d.textureSheetAnimation;
        sheet.enabled = readBool(uv, "enabled");
        sheet.present = sheet.enabled;
        sheet.tilesX = std::max(1, readInt(uv, "tilesX", 1));
        sheet.tilesY = std::max(1, readInt(uv, "tilesY", 1));
        sheet.animation = readInt(uv, "animationType") == 1 ? TextureSheetAnimation::SingleRow : TextureSheetAnimation::WholeSheet;
        sheet.rowMode = readInt(uv, "rowMode", readBool(uv, "randomRow") ? 1 : 0) == 0 ? TextureSheetRowMode::Custom : TextureSheetRowMode::Random;
        sheet.rowIndex = readInt(uv, "rowIndex");
        const int timeMode = readInt(uv, "timeMode");
        sheet.timeMode = timeMode == 1 ? TextureSheetTimeMode::Speed : timeMode == 2 ? TextureSheetTimeMode::FPS : TextureSheetTimeMode::Lifetime;
        sheet.fps = readFloat(uv, "fps", 30.0f);
        sheet.frameOverTime = readMinMaxCurve(uv["frameOverTime"], ctx, "frame over time", 1.0f, 1.0f);
        { const Vector3 r = readVector3(uv, "speedRange", Vector3(0, 1, 0)); sheet.speedRange = Vector2(r.x, r.y); }
        // Unity stores the start frame as a share of the frames
        const int frames = sheet.animation == TextureSheetAnimation::SingleRow ? sheet.tilesX : sheet.tilesX * sheet.tilesY;
        sheet.startFrame = readMinMaxCurve(uv["startFrame"], ctx, "start frame", (float)frames);
        sheet.cycles = (float)std::max(1, (int)std::lround(readFloat(uv, "cycles", 1.0f)));
        sheet.flipU = readFloat(uv, "flipU");
        sheet.flipV = readFloat(uv, "flipV");
        if (sheet.enabled && readInt(uv, "mode") == 1){
            note(ctx, "module-unsupported", "Texture Sheet Animation in Sprites mode (Unity sprite assets) is not imported: Grid is used");
        }
        status("UVModule", sheet.enabled ? "imported" : "disabled");

        for (const char* unsupported : UNSUPPORTED_MODULES){
            const YAML::Node module = ps[unsupported];
            if (!module) continue;
            bool on = readBool(module, "enabled");
            if (std::string(unsupported) == "SubModule" && on){
                on = module["subEmitters"] && module["subEmitters"].IsSequence() && module["subEmitters"].size() > 0;
            }
            if (on){
                note(ctx, "module-unsupported", std::string(unsupported) + " is not imported (S20 has the module: set it up in the Inspector)");
                status(unsupported, "unsupported");
            }else{
                status(unsupported, "disabled");
            }
        }
    }

    // the renderer's values on the effect's Renderer module (its first material's in it)
    void convertRenderer(const YAML::Node& r, ParticleEffectDocument& d, ParticleSystemComponent& c, Context& ctx){
        using namespace particles;
        RendererModule& rm = d.renderer;
        const int mode = readInt(r, "m_RenderMode");
        // Unity's None (5): simulated, not drawn (its renderer disabled below)
        const bool notRendered = mode == 5;
        const int align = readInt(r, "m_RenderAlignment");
        if (!upstreamparticles::applyRenderMode(mode >= 0 && mode <= 4 ? mode : 0, align >= 0 && align <= 4 ? align : 0,
                                                 d.simulationSpace == SimulationSpace::Local, rm)){
            note(ctx, "render-alignment", "a stretched, horizontal or vertical billboard has no render alignment here: left out");
        }
        const int sort = readInt(r, "m_SortMode");
        rm.sortMode = sort == 1 ? SortMode::ByDistance : sort == 2 ? SortMode::OldestInFront : sort == 3 ? SortMode::YoungestInFront : SortMode::None;
        rm.sortingFudge = readFloat(r, "m_SortingFudge");
        rm.minScreenSize = readFloat(r, "m_MinParticleSize");
        rm.maxScreenSize = readFloat(r, "m_MaxParticleSize", 0.5f);
        rm.cameraScale = readFloat(r, "m_CameraVelocityScale");
        rm.speedScale = readFloat(r, "m_VelocityScale");
        rm.lengthScale = readFloat(r, "m_LengthScale", 2.0f);
        rm.pivot = readVector3(r, "m_Pivot");
        rm.flip = readVector3(r, "m_Flip");
        // S52 R41: Unity's Order in Layer and Sorting Layer are the system's own (effects
        // imported earlier keep their sortPriority, which sorts after them); the layer by name
        c.sortingOrder = std::clamp(readInt(r, "m_SortingOrder"), -32768, 32767);
        const uint32_t unityLayer = unitySortingLayerBits(r["m_SortingLayerID"]);
        if (unityLayer != 0){
            const std::string name = unitySortingLayerName(ctx, unityLayer);
            const int32_t id = name.empty() ? -1 : SortingLayers::nameToId(name);
            if (id >= 0){
                c.sortingLayer = id;
            }else{
                note(ctx, "sorting-layer-missing", "the Sorting Layer " + (name.empty() ? "id " + std::to_string(unityLayer) : "\"" + name + "\"") +
                     " is not in Project Settings > Tags and Layers: it draws on Default (add the layer, then set Sorting Layer)");
            }
        }

        if (rm.renderMode == RenderMode::Mesh){
            const Ref meshRef = readRef(r, "m_Mesh");
            bool primitive = false;
            if (const BuiltinAsset* builtin = UnityProject::builtin(meshRef); builtin && builtin->kind == BuiltinAsset::Mesh){
                rm.meshes = {upstreamparticles::meshSourceOf(builtin->engine, primitive)};
            }else{
                const fs::path file = ctx.project.pathOf(meshRef.guid);
                note(ctx, "mesh-missing", "the particle mesh " + (file.empty() ? "guid " + meshRef.guid : ctx.project.relative(file)) +
                     " is not a built-in one: particles draw as spheres; import the model and set its mesh");
                rm.meshes = {upstreamparticles::meshSourceOf("sphere", primitive)};
            }
        }
        // the first material (particles draw with one)
        Ref materialRef;
        if (r["m_Materials"] && r["m_Materials"].IsSequence() && r["m_Materials"].size() > 0) materialRef = toRef(r["m_Materials"][0]);
        const Material material = importMaterial(ctx, materialRef);
        std::vector<std::string> notes;
        upstreamparticles::applyMaterial(material, rm, notes);
        for (const std::string& line : notes) note(ctx, "material-approximated", "material " + material.name + ": " + line);
        if (readInt(r, "m_CastShadows") != 0) note(ctx, "cast-shadows", "the renderer casts shadows: particles do not");
        rm.receiveShadows = readBool(r, "m_ReceiveShadows");
        if (!readBool(r, "m_Enabled", true) || notRendered) rm.enabled = false;
    }

    // ------------------------------------------------------------------ the prefab

    struct Built {
        Entity entity = NULL_ENTITY;
        std::vector<Built> children;
    };

    Built buildTree(const Graph& graph, const Hierarchy& h, int64_t go, Scene& scene, Context& ctx, Json& systems, const std::string& prefabName,
                    const std::string& enginePath){
        Built built;
        const Entity entity = scene.createUserEntity();
        built.entity = entity;
        const std::string name = nameOf(graph, go);
        scene.setEntityName(entity, name);
        auto goIt = graph.find(go);
        if (goIt != graph.end() && !readBool(goIt->second.body, "m_IsActive", true)) scene.setEntityActive(entity, false);

        Transform transform;
        auto t = h.transformOf.find(go);
        if (t != h.transformOf.end()){
            const YAML::Node body = graph.at(t->second).body;
            const Vector3 p = readVector3(body, "m_LocalPosition");
            transform.position = Vector3(-p.x, p.y, p.z);
            transform.rotation = mirrored(readQuaternion(body, "m_LocalRotation"));
            transform.scale = readVector3(body, "m_LocalScale", Vector3(1, 1, 1));
        }
        scene.addComponent<Transform>(entity, transform);

        const std::string thisPath = enginePath.empty() ? name : enginePath + "/" + name;
        // the overrides of this GameObject's objects that did not apply
        {
            std::vector<int64_t> used = {go};
            if (t != h.transformOf.end()) used.push_back(t->second);
            for (int64_t id : componentsOf(graph, go)) used.push_back(id);
            for (int64_t id : used){
                auto object = graph.find(id);
                if (object == graph.end()) continue;
                for (const std::string& message : object->second.unknownOverrides){
                    ctx.warnings.push_back("override-unknown: " + message);
                }
            }
        }
        if (const Object* ps = componentOfClass(graph, go, CLASS_PARTICLESYSTEM)){
            Json system = {{"path", pathOf(graph, h, go)}, {"engine_entity", prefabName + ":" + thisPath}};
            std::vector<ImportNote> notes;
            ctx.notes = &notes;
            Json modules = Json::object();
            // an S20 system on the GameObject itself, its effect inline (it draws itself)
            ParticleSystemComponent component;
            convertSystem(ps->body, component.inlineEffect, component, modules, ctx);
            particles::ParticleEffectDocument& effect = component.inlineEffect;
            if (const Object* renderer = componentOfClass(graph, go, CLASS_PARTICLESYSTEMRENDERER)){
                convertRenderer(renderer->body, effect, component, ctx);
                modules["ParticleSystemRenderer"] = "imported";
            }else{
                note(ctx, "renderer-missing", "no ParticleSystemRenderer: drawn with the default particle material");
            }
            // known scripts on the GameObject
            for (int64_t id : componentsOf(graph, go)){
                const Object& component2 = graph.at(id);
                if (component2.classId != CLASS_MONOBEHAVIOUR) continue;
                const fs::path script = ctx.project.pathOf(readRef(component2.body, "m_Script").guid);
                const std::string stem = script.stem().string();
                if (stem == "CFX_AutoDestructShuriken"){
                    effect.stopAction = particles::StopAction::Destroy;
                    note(ctx, "auto-destruct", "CFX_AutoDestructShuriken: Stop Action Destroy");
                }else if (stem == "UIParticle" || stem == "UIParticleAttractor"){
                    note(ctx, "ui-particle", "drawn inside a UI canvas by Coffee UIParticle in Unity: put the prefab under a UI element (feature ui-scaling-text)");
                }
            }
            // the parent's UIParticle draws its children too
            int64_t up = h.parentOf.count(go) ? h.parentOf.at(go) : 0;
            while (up != 0){
                for (int64_t id : componentsOf(graph, up)){
                    const Object& component3 = graph.at(id);
                    if (component3.classId == CLASS_MONOBEHAVIOUR &&
                        ctx.project.pathOf(readRef(component3.body, "m_Script").guid).stem().string() == "UIParticle"){
                        bool already = false;
                        for (const ImportNote& n : notes) already = already || n.code == "ui-particle";
                        if (!already) note(ctx, "ui-particle", "drawn inside a UI canvas by Coffee UIParticle in Unity: put the prefab under a UI element (feature ui-scaling-text)");
                    }
                }
                up = h.parentOf.count(up) ? h.parentOf.at(up) : 0;
            }
            scene.addComponent<ParticleSystemComponent>(entity, component);
            ctx.notes = nullptr;
            Json notesJson = Json::array();
            for (const ImportNote& n : notes) notesJson.push_back({{"code", n.code}, {"message", n.message}});
            system["modules"] = modules;
            system["notes"] = notesJson;
            systems.push_back(system);
        }

        auto children = h.children.find(go);
        if (children != h.children.end()){
            for (int64_t child : children->second){
                built.children.push_back(buildTree(graph, h, child, scene, ctx, systems, prefabName, thisPath));
            }
        }
        return built;
    }

    YAML::Node encodeBuilt(const Built& built, Scene& scene){
        YAML::Node node;
        node["type"] = "Entity";
        node["entity"] = built.entity;
        node["name"] = scene.getEntityName(built.entity);
        Stream::encodeEntityState(node, built.entity, &scene);
        node["components"] = Stream::encodeComponents(built.entity, &scene, scene.getSignature(built.entity));
        if (!built.children.empty()){
            YAML::Node children(YAML::NodeType::Sequence);
            for (const Built& child : built.children) children.push_back(encodeBuilt(child, scene));
            node["children"] = children;
        }
        return node;
    }

    Entity maxEntity(const Built& built){
        Entity m = built.entity;
        for (const Built& child : built.children) m = std::max(m, maxEntity(child));
        return m;
    }

    std::string uniqueFile(Context& ctx, const std::string& stem, const std::string& ext){
        std::error_code ec;
        for (int i = 0; ; i++){
            const std::string name = i == 0 ? stem + ext : stem + " " + std::to_string(i) + ext;
            const std::string rel = ctx.destRel + "/" + name;
            if (ctx.usedNames.count(rel)) continue;
            if (!ctx.options.overwrite && fs::exists(ctx.engineRoot / rel, ec)) continue;
            ctx.usedNames.insert(rel);
            return rel;
        }
    }

}

namespace {
    ImportResult importParticlesUnchecked(const ImportOptions& options);
}

ImportResult jasperengine::editor::unity::importParticles(const ImportOptions& options){
    // a malformed file (yaml-cpp's errors past the document split) fails the import, never the caller
    try {
        return importParticlesUnchecked(options);
    } catch (const std::exception& e){
        componentIndex = ComponentIndex();
        ImportResult result;
        result.error = "cannot import " + options.file.string() + ": " + e.what();
        return result;
    }
}

namespace {

ImportResult importParticlesUnchecked(const ImportOptions& options){
    ImportResult result;
    Context ctx;
    ctx.options = options;
    std::error_code ec;

    const fs::path file = fs::absolute(options.file, ec).lexically_normal();
    if (!fs::is_regular_file(file, ec)){
        result.error = "no such file: " + options.file.string();
        return result;
    }
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    if (ext != ".prefab" && ext != ".unity"){
        result.error = "not a Unity prefab or scene (.prefab, .unity): " + file.string();
        return result;
    }
    const fs::path unityRoot = options.unityProject.empty() ? UnityProject::findRoot(file) : options.unityProject;
    if (unityRoot.empty()){
        result.error = "no Unity project around " + file.string() + " (a folder with Assets/ and ProjectSettings/): pass unity_project";
        return result;
    }
    std::string error;
    if (!ctx.project.open(unityRoot, error)){
        result.error = error;
        return result;
    }
    ctx.engineRoot = fs::absolute(options.engineProject, ec).lexically_normal();
    std::string dest = options.dest.empty() ? std::string("assets/fx") : options.dest;
    while (!dest.empty() && (dest.back() == '/' || dest.back() == '\\')) dest.pop_back();
    const fs::path destPath = fs::path(dest).lexically_normal();
    if (destPath.is_absolute() || destPath.empty() || *destPath.begin() == ".."){
        result.error = "dest must be a folder inside the engine project (project-relative): " + options.dest;
        return result;
    }
    ctx.destRel = destPath.generic_string();
    ctx.dest = ctx.engineRoot / destPath;
    // never into the Unity project (an engine project inside it, or the same folder)
    {
        const std::string unityDir = lowercaseName(fs::weakly_canonical(ctx.project.root(), ec).generic_string());
        const std::string destDir = lowercaseName(fs::weakly_canonical(ctx.dest, ec).generic_string());
        if (!unityDir.empty() && (destDir == unityDir || destDir.compare(0, unityDir.size() + 1, unityDir + "/") == 0)){
            result.error = "dest " + ctx.dest.string() + " is inside the Unity project " + ctx.project.root().string() +
                           ": the importer never writes there (use an engine project outside it)";
            return result;
        }
    }

    Graph graph;
    componentIndex = ComponentIndex();   // built for this graph on first use
    if (!resolveFile(file, graph, ctx, 0)){
        result.error = "cannot read " + file.string() + (ctx.warnings.empty() ? std::string() : ": " + ctx.warnings.back());
        return result;
    }
    const Hierarchy h = buildHierarchy(graph);

    // the roots: a named object, the prefab's root, or in a scene each topmost GameObject with a system
    std::vector<int64_t> roots;
    if (!options.object.empty()){
        // an exact path first; else a name, which must be unique
        std::vector<int64_t> byPath, byName;
        std::vector<int64_t> ordered;
        for (const auto& item : h.transformOf) ordered.push_back(item.first);
        std::stable_sort(ordered.begin(), ordered.end(), [&](int64_t a, int64_t b){ return graph.at(a).order < graph.at(b).order; });
        for (int64_t go : ordered){
            if (pathOf(graph, h, go) == options.object) byPath.push_back(go);
            else if (nameOf(graph, go) == options.object) byName.push_back(go);
        }
        const std::vector<int64_t>& found = byPath.empty() ? byName : byPath;
        if (found.empty()){
            result.error = "no GameObject '" + options.object + "' in " + file.filename().string();
            return result;
        }
        if (found.size() > 1){
            std::string paths;
            for (int64_t go : found) paths += "\n  " + pathOf(graph, h, go);
            result.error = "'" + options.object + "' names " + std::to_string(found.size()) + " GameObjects in " + file.filename().string() +
                           ": give the path of one" + paths;
            return result;
        }
        roots.push_back(found.front());
    }else if (ext == ".prefab"){
        for (int64_t root : h.roots){
            if (subtreeHasSystem(graph, h, root)) roots.push_back(root);
        }
    }else{
        std::function<void(int64_t)> walk = [&](int64_t go){
            if (hasSystem(graph, go)){
                roots.push_back(go);
                return;
            }
            auto it = h.children.find(go);
            if (it != h.children.end()) for (int64_t child : it->second) walk(child);
        };
        for (int64_t root : h.roots) walk(root);
    }
    if (roots.empty()){
        result.error = "no ParticleSystem in " + file.filename().string() + (options.object.empty() ? "" : " under '" + options.object + "'");
        return result;
    }

    Json systems = Json::array();
    for (int64_t root : roots){
        Scene scene;
        const std::string name = sanitize(nameOf(graph, root));
        const std::string prefabRel = uniqueFile(ctx, name, ".prefab");
        Built built = buildTree(graph, h, root, scene, ctx, systems, prefabRel, "");
        // the prefab's root sits at the origin (its place was the Unity scene's)
        Transform& rootTransform = scene.getComponent<Transform>(built.entity);
        rootTransform.position = Vector3(0, 0, 0);
        if (ext == ".unity") rootTransform.rotation = Quaternion();

        YAML::Node prefab;
        prefab["type"] = "Prefab";
        prefab["version"] = Prefab::FILE_VERSION;
        prefab["guid"] = AssetDatabase::generateGuid();
        prefab["nextEntity"] = maxEntity(built) + 1;
        prefab["root"] = built.entity;
        YAML::Node members(YAML::NodeType::Sequence);
        members.push_back(encodeBuilt(built, scene));
        prefab["members"] = members;
        ctx.files.push_back({ctx.engineRoot / prefabRel, YAML::Dump(prefab) + "\n"});
        result.prefabs.push_back(prefabRel);
    }

    // the report
    Json report;
    report["source"] = ctx.project.relative(file);
    report["unity_project"] = ctx.project.root().string();
    report["prefabs"] = result.prefabs;
    report["systems"] = systems;
    report["materials"] = ctx.materials;
    report["textures"] = ctx.textures;
    report["warnings"] = ctx.warnings;
    report["dry_run"] = options.dryRun;
    bool notes = !ctx.warnings.empty();
    for (const Json& system : systems) notes = notes || !system["notes"].empty();
    result.hasWarnings = notes;
    result.report = report;

    if (!options.dryRun){
        // next to the first prefab, named after it ("Fire 1.prefab": "Fire 1.import-report.json")
        const std::string reportRel = ctx.destRel + "/" + fs::path(result.prefabs.front()).stem().string() + ".import-report.json";
        ctx.files.push_back({ctx.engineRoot / reportRel, report.dump(2) + "\n"});
        result.files = ctx.files;
        result.reportFile = reportRel;
    }
    if (!options.dryRun && options.writeFiles){
        for (const auto& [path, bytes] : ctx.files){
            std::string why;
            if (!safewrite::writeFileBeneath(ctx.engineRoot, path, bytes, why)){
                result.error = "cannot write " + path.string() + ": " + why;
                return result;
            }
            result.written.push_back(path.lexically_relative(ctx.engineRoot).generic_string());
        }
    }
    componentIndex = ComponentIndex();
    result.ok = true;
    return result;
}

}

ImportResult jasperengine::editor::unity::importParticlesIntoProject(jasperengine::editor::Project* project, ImportOptions options){
    if (!project || project->getProjectPath().empty()){
        ImportResult result;
        result.error = "no project is open";
        return result;
    }
    options.engineProject = project->getProjectPath();
    options.writeFiles = false;
    ImportResult result = importParticles(options);
    if (!result.ok || options.dryRun) return result;
    std::vector<WriteFilesCmd::Item> items;
    for (const auto& [path, bytes] : result.files) items.push_back({path, bytes});
    std::string error;
    if (!ProjectFileWrites::writeUndoable(project, std::move(items), error, nullptr, nullptr, project->getProjectPath())){
        result.ok = false;
        result.error = error.empty() ? std::string("the files could not be written") : error;
        return result;
    }
    for (const auto& file : result.files){
        result.written.push_back(file.first.lexically_relative(project->getProjectPath()).generic_string());
    }
    return result;
}
