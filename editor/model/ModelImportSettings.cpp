// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelImportSettings.h"

#include "command/type/SetAssetMetaCmd.h"

#include <algorithm>
#include <cmath>
#include <locale>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    template<class E>
    struct EnumName {
        E value;
        const char* name;
        const char* label;
    };

    const EnumName<ModelFamily> familyNames[] = {
        {ModelFamily::Desktop, "desktop", "Desktop"}, {ModelFamily::Mobile, "mobile", "Mobile"}, {ModelFamily::Web, "web", "Web"}};
    const EnumName<OptimizeMesh> optimizeNames[] = {
        {OptimizeMesh::Everything, "everything", "Everything"}, {OptimizeMesh::PolygonOrder, "polygon_order", "Polygon Order"},
        {OptimizeMesh::VertexOrder, "vertex_order", "Vertex Order"}, {OptimizeMesh::Nothing, "nothing", "Nothing"}};
    const EnumName<MeshCompression> compressionNames[] = {
        {MeshCompression::Off, "off", "Off"}, {MeshCompression::Lossless, "lossless", "Lossless"},
        {MeshCompression::Low, "low", "Low"}, {MeshCompression::Medium, "medium", "Medium"}, {MeshCompression::High, "high", "High"}};
    const EnumName<IndexFormat> indexNames[] = {
        {IndexFormat::Auto, "auto", "Auto"}, {IndexFormat::Bits16, "u16", "16 bit"}, {IndexFormat::Bits32, "u32", "32 bit"}};
    const EnumName<ModelReadWrite> readWriteNames[] = {
        {ModelReadWrite::Automatic, "automatic", "Automatic"}, {ModelReadWrite::On, "on", "On"}};
    const EnumName<ModelNormals> normalsNames[] = {
        {ModelNormals::Import, "import", "Import"}, {ModelNormals::Calculate, "calculate", "Calculate"}};
    const EnumName<GenerateLods> lodsNames[] = {
        {GenerateLods::Automatic, "automatic", "Automatic"}, {GenerateLods::On, "on", "On"}, {GenerateLods::Off, "off", "Off"}};
    const EnumName<LodReduction> reductionNames[] = {
        {LodReduction::PreserveDetail, "preserve_detail", "Preserve Detail"}, {LodReduction::Balanced, "balanced", "Balanced"},
        {LodReduction::Smallest, "smallest", "Smallest"}};
    const EnumName<ScriptMeshData> scriptNames[] = {
        {ScriptMeshData::Automatic, "automatic", "Automatic"}, {ScriptMeshData::Keep, "keep", "Keep"},
        {ScriptMeshData::Release, "release", "Release"}};

    template<class E, size_t N>
    const EnumName<E>& entryOf(const EnumName<E> (&table)[N], E value){
        for (const auto& entry : table)
            if (entry.value == value)
                return entry;
        return table[0];
    }

    template<class E, size_t N>
    bool valueOf(const EnumName<E> (&table)[N], const std::string& name, E& value){
        for (const auto& entry : table){
            if (name == entry.name){
                value = entry.value;
                return true;
            }
        }
        return false;
    }

    template<class E, size_t N>
    std::vector<std::string> namesOf(const EnumName<E> (&table)[N]){
        std::vector<std::string> out;
        for (const auto& entry : table)
            out.push_back(entry.name);
        return out;
    }

    std::string joined(const std::vector<std::string>& values){
        std::string out;
        for (const std::string& value : values){
            if (!out.empty()) out += ", ";
            out += value;
        }
        return out;
    }

    const char* const settingKeys[] = {
        "optimizeMesh", "meshCompression", "weldVertices", "indexFormat", "readWrite", "normals", "smoothingAngle",
        "generateLods", "lodCount", "lodReduction", "shipSource"};

    bool isSettingKey(const std::string& key){
        return std::find_if(std::begin(settingKeys), std::end(settingKeys), [&](const char* k){ return key == k; }) != std::end(settingKeys);
    }

    // a field read: absent stays absent, a wrong value is an error (the first one is kept)
    struct Reader {
        const YAML::Node& node;
        std::string& error;
        std::string prefix;

        bool fail(const std::string& field, const std::string& why){
            if (error.empty()) error = prefix + field + ": " + why;
            return false;
        }
        bool boolean(const char* field, std::optional<bool>& out){
            if (!node[field]) return true;
            try { out = node[field].as<bool>(); return true; } catch (...) { return fail(field, "expected true or false"); }
        }
        bool integer(const char* field, std::optional<int>& out, int low, int high){
            if (!node[field]) return true;
            try{
                const int value = node[field].as<int>();
                if (value < low || value > high) return fail(field, "must be " + std::to_string(low) + "..." + std::to_string(high));
                out = value;
                return true;
            }catch (...){ return fail(field, "expected a whole number"); }
        }
        bool number(const char* field, std::optional<float>& out, float low, float high){
            if (!node[field]) return true;
            try{
                const float value = node[field].as<float>();
                if (!(value >= low && value <= high))
                    return fail(field, "must be " + std::to_string((int)low) + "..." + std::to_string((int)high));
                out = value;
                return true;
            }catch (...){ return fail(field, "expected a number"); }
        }
        template<class E, size_t N>
        bool enumeration(const char* field, const EnumName<E> (&table)[N], std::optional<E>& out){
            if (!node[field]) return true;
            // the scalar's text: YAML 1.1 readers take a plain on/off for a boolean
            std::string name;
            try { name = node[field].Scalar(); } catch (...) { name.clear(); }
            E value;
            if (!valueOf(table, name, value))
                return fail(field, "'" + name + "' is not one of " + joined(namesOf(table)));
            out = value;
            return true;
        }
    };

    void readValues(Reader& r, ModelSettingValues& v){
        r.enumeration("optimizeMesh", optimizeNames, v.optimizeMesh);
        r.enumeration("meshCompression", compressionNames, v.meshCompression);
        r.boolean("weldVertices", v.weldVertices);
        r.enumeration("indexFormat", indexNames, v.indexFormat);
        r.enumeration("readWrite", readWriteNames, v.readWrite);
        r.enumeration("normals", normalsNames, v.normals);
        r.number("smoothingAngle", v.smoothingAngle, 0.0f, 180.0f);
        r.enumeration("generateLods", lodsNames, v.generateLods);
        r.integer("lodCount", v.lodCount, 1, 3);
        r.enumeration("lodReduction", reductionNames, v.lodReduction);
        r.boolean("shipSource", v.shipSource);
    }

    void writeValues(YAML::Node& node, const ModelSettingValues& v){
        if (v.optimizeMesh) node["optimizeMesh"] = entryOf(optimizeNames, *v.optimizeMesh).name;
        if (v.meshCompression) node["meshCompression"] = entryOf(compressionNames, *v.meshCompression).name;
        if (v.weldVertices) node["weldVertices"] = *v.weldVertices;
        if (v.indexFormat) node["indexFormat"] = entryOf(indexNames, *v.indexFormat).name;
        if (v.readWrite) node["readWrite"] = entryOf(readWriteNames, *v.readWrite).name;
        if (v.normals) node["normals"] = entryOf(normalsNames, *v.normals).name;
        if (v.smoothingAngle) node["smoothingAngle"] = *v.smoothingAngle;
        if (v.generateLods) node["generateLods"] = entryOf(lodsNames, *v.generateLods).name;
        if (v.lodCount) node["lodCount"] = *v.lodCount;
        if (v.lodReduction) node["lodReduction"] = entryOf(reductionNames, *v.lodReduction).name;
        if (v.shipSource) node["shipSource"] = *v.shipSource;
    }

    // the first value that is set: the platform override, the model's, the project default
    template<class T>
    T pick(const std::optional<T>& platform, bool overrides, const std::optional<T>& own, const std::optional<T>& project, T fallback){
        if (overrides && platform) return *platform;
        if (own) return *own;
        if (project) return *project;
        return fallback;
    }

    // integers only: no locale and no printf rounding reach a key
    std::string canonicalNumber(float value){
        const long long scaled = std::llround((double)value * 1000.0);
        return std::to_string(scaled);
    }

}

bool ModelSettingValues::empty() const{
    return !optimizeMesh && !meshCompression && !weldVertices && !indexFormat && !readWrite && !normals && !smoothingAngle &&
           !generateLods && !lodCount && !lodReduction && !shipSource;
}

const char* editor::modelFamilyName(ModelFamily family){ return entryOf(familyNames, family).name; }
bool editor::modelFamilyFromName(const std::string& name, ModelFamily& family){ return valueOf(familyNames, name, family); }
const char* editor::modelFamilyLabel(ModelFamily family){ return entryOf(familyNames, family).label; }
const char* editor::optimizeMeshName(OptimizeMesh value){ return entryOf(optimizeNames, value).name; }
const char* editor::meshCompressionName(MeshCompression value){ return entryOf(compressionNames, value).name; }
const char* editor::indexFormatName(IndexFormat value){ return entryOf(indexNames, value).name; }
const char* editor::modelReadWriteName(ModelReadWrite value){ return entryOf(readWriteNames, value).name; }
const char* editor::modelNormalsName(ModelNormals value){ return entryOf(normalsNames, value).name; }
const char* editor::generateLodsName(GenerateLods value){ return entryOf(lodsNames, value).name; }
const char* editor::lodReductionName(LodReduction value){ return entryOf(reductionNames, value).name; }
const char* editor::scriptMeshDataName(ScriptMeshData value){ return entryOf(scriptNames, value).name; }
const char* editor::optimizeMeshLabel(OptimizeMesh value){ return entryOf(optimizeNames, value).label; }
const char* editor::meshCompressionLabel(MeshCompression value){ return entryOf(compressionNames, value).label; }
const char* editor::indexFormatLabel(IndexFormat value){ return entryOf(indexNames, value).label; }
const char* editor::lodReductionLabel(LodReduction value){ return entryOf(reductionNames, value).label; }

const std::vector<std::string>& editor::modelEnumValues(const std::string& field){
    static const std::map<std::string, std::vector<std::string>> values = {
        {"optimizeMesh", namesOf(optimizeNames)}, {"meshCompression", namesOf(compressionNames)},
        {"indexFormat", namesOf(indexNames)}, {"readWrite", namesOf(readWriteNames)}, {"normals", namesOf(normalsNames)},
        {"generateLods", namesOf(lodsNames)}, {"lodReduction", namesOf(reductionNames)}, {"platform", namesOf(familyNames)},
        {"scriptMeshData", namesOf(scriptNames)}};
    static const std::vector<std::string> none;
    auto it = values.find(field);
    return it == values.end() ? none : it->second;
}

const std::vector<std::string>& editor::modelSettingKeys(){
    static const std::vector<std::string> keys(std::begin(settingKeys), std::end(settingKeys));
    return keys;
}

bool ModelImportSettings::parse(const YAML::Node& node, ModelImportSettings& out, std::string* errorOut){
    out = ModelImportSettings();
    std::string error;
    if (!node || node.IsNull())
        return true;
    if (!node.IsMap()){
        if (errorOut) *errorOut = "modelImporter: expected a map";
        return false;
    }
    Reader r{node, error, ""};
    std::optional<int> version;
    r.integer("version", version, 1, 1000000);
    // version 1 is the first: an older section does not exist, a newer one is read for what
    // this editor knows (its new keys are kept as unknown)
    out.version = currentVersion;
    readValues(r, out.values);
    if (const YAML::Node platforms = node["platforms"]){
        if (!platforms.IsMap()){
            r.fail("platforms", "expected a map of desktop, mobile, web");
        }else{
            for (const auto& entry : platforms){
                const std::string name = entry.first.as<std::string>();
                ModelFamily family;
                if (!modelFamilyFromName(name, family)){
                    r.fail("platforms." + name, "not one of " + joined(namesOf(familyNames)));
                    continue;
                }
                const YAML::Node block = entry.second;
                if (!block.IsMap()){
                    r.fail("platforms." + name, "expected a map");
                    continue;
                }
                ModelPlatformSettings& p = out.platforms[(size_t)family];
                Reader pr{block, error, "platforms." + name + "."};
                std::optional<bool> overrides;
                pr.boolean("override", overrides);
                p.override = overrides.value_or(false);
                readValues(pr, p.values);
            }
        }
    }
    out.unknown = YAML::Node(YAML::NodeType::Map);
    for (const auto& entry : node){
        const std::string key = entry.first.as<std::string>();
        if (key != "version" && key != "platforms" && !isSettingKey(key))
            out.unknown[key] = YAML::Clone(entry.second);
    }
    if (!error.empty()){
        if (errorOut) *errorOut = error;
        return false;
    }
    return true;
}

YAML::Node ModelImportSettings::serialize() const{
    YAML::Node node(YAML::NodeType::Map);
    node["version"] = version;
    writeValues(node, values);
    YAML::Node platformsNode(YAML::NodeType::Map);
    for (size_t f = 0; f < kModelFamilyCount; f++){
        const ModelPlatformSettings& p = platforms[f];
        if (p.empty())
            continue;
        YAML::Node block(YAML::NodeType::Map);
        block.SetStyle(YAML::EmitterStyle::Flow);
        block["override"] = p.override;
        writeValues(block, p.values);
        platformsNode[modelFamilyName((ModelFamily)f)] = block;
    }
    if (platformsNode.size() > 0)
        node["platforms"] = platformsNode;
    if (unknown && unknown.IsMap()){
        for (const auto& entry : unknown)
            node[entry.first.as<std::string>()] = YAML::Clone(entry.second);
    }
    return node;
}

bool ModelImportSettings::operator==(const ModelImportSettings& other) const{
    return YAML::Dump(serialize()) == YAML::Dump(other.serialize());
}

bool ModelImportSettings::isDefault() const{
    if (!values.empty() || (unknown && unknown.IsMap() && unknown.size() > 0))
        return false;
    for (const ModelPlatformSettings& p : platforms)
        if (!p.empty())
            return false;
    return true;
}

bool ModelImportSettings::load(const std::string& absoluteAsset, ModelImportSettings& out, std::string* error){
    const YAML::Node node = SetAssetMetaCmd::read(absoluteAsset, metaKey);
    if (!node || node.IsNull()){
        out = ModelImportSettings();
        return false;
    }
    return parse(node, out, error);
}

ResolvedModelSettings ModelImportSettings::resolve(ModelFamily family, const ModelProjectSettings& project) const{
    ResolvedModelSettings r;
    const ModelPlatformSettings& p = platforms[(size_t)family];
    const ModelSettingValues& d = project.defaults[(size_t)family];
    const bool o = p.override;
    const ModelSettingValues& v = values;
    r.optimizeMesh = pick(p.values.optimizeMesh, o, v.optimizeMesh, d.optimizeMesh, r.optimizeMesh);
    r.meshCompression = pick(p.values.meshCompression, o, v.meshCompression, d.meshCompression, r.meshCompression);
    r.weldVertices = pick(p.values.weldVertices, o, v.weldVertices, d.weldVertices, r.weldVertices);
    r.indexFormat = pick(p.values.indexFormat, o, v.indexFormat, d.indexFormat, r.indexFormat);
    r.readWrite = pick(p.values.readWrite, o, v.readWrite, d.readWrite, r.readWrite);
    r.normals = pick(p.values.normals, o, v.normals, d.normals, r.normals);
    r.smoothingAngle = pick(p.values.smoothingAngle, o, v.smoothingAngle, d.smoothingAngle, r.smoothingAngle);
    r.generateLods = pick(p.values.generateLods, o, v.generateLods, d.generateLods, r.generateLods);
    r.lodCount = pick(p.values.lodCount, o, v.lodCount, d.lodCount, r.lodCount);
    r.lodReduction = pick(p.values.lodReduction, o, v.lodReduction, d.lodReduction, r.lodReduction);
    r.shipSource = pick(p.values.shipSource, o, v.shipSource, d.shipSource, r.shipSource);
    return r;
}

std::string ResolvedModelSettings::canonical() const{
    // every field the cook's output depends on, in a fixed order: the key must not move when
    // the code that prints it does. Read/Write and Ship Source File change what an export
    // ships, not the cooked file.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"optimizeMesh\":\"" << optimizeMeshName(optimizeMesh) << "\""
        << ",\"meshCompression\":\"" << meshCompressionName(meshCompression) << "\""
        << ",\"weldVertices\":" << (weldVertices ? 1 : 0)
        << ",\"indexFormat\":\"" << indexFormatName(indexFormat) << "\""
        << ",\"normals\":\"" << modelNormalsName(normals) << "\"";
    if (normals == ModelNormals::Calculate)
        out << ",\"smoothingAngle\":" << canonicalNumber(smoothingAngle);
    out << ",\"generateLods\":\"" << generateLodsName(generateLods) << "\"";
    // Automatic bakes the runtime's recipe whatever LOD Count and Reduction say
    if (generateLods == GenerateLods::On)
        out << ",\"lodCount\":" << lodCount << ",\"lodReduction\":\"" << lodReductionName(lodReduction) << "\"";
    out << "}";
    return out.str();
}

bool editor::validateModelSettingsField(const std::string& field, const YAML::Node& value, std::string& error){
    if (!isSettingKey(field)){
        error = field + ": not a model import setting (" + joined(modelSettingKeys()) + ")";
        return false;
    }
    YAML::Node wrapper(YAML::NodeType::Map);
    wrapper[field] = value;
    ModelImportSettings scratch;
    return ModelImportSettings::parse(wrapper, scratch, &error);
}

YAML::Node editor::encodeModelProjectSettings(const ModelProjectSettings& settings){
    const ModelProjectSettings defaults;
    YAML::Node node(YAML::NodeType::Map);
    YAML::Node families(YAML::NodeType::Map);
    for (size_t f = 0; f < kModelFamilyCount; f++){
        YAML::Node family(YAML::NodeType::Map);
        writeValues(family, settings.defaults[f]);
        if (family.size())
            families[modelFamilyName((ModelFamily)f)] = family;
    }
    if (families.size())
        node["defaults"] = families;
    if (settings.cacheSizeMB != defaults.cacheSizeMB)
        node["cacheSizeMB"] = settings.cacheSizeMB;
    if (settings.scriptMeshData != defaults.scriptMeshData)
        node["keepCpuDataForScripts"] = scriptMeshDataName(settings.scriptMeshData);
    YAML::Node budgets(YAML::NodeType::Map);
    for (size_t f = 0; f < kModelFamilyCount; f++){
        const RenderBudget& b = settings.renderBudgets[f];
        if (b.empty())
            continue;
        YAML::Node block(YAML::NodeType::Map);
        block.SetStyle(YAML::EmitterStyle::Flow);
        if (b.triangles > 0) block["triangles"] = b.triangles;
        if (b.drawCalls > 0) block["drawCalls"] = b.drawCalls;
        if (b.meshMemoryMB > 0.0) block["meshMemoryMB"] = b.meshMemoryMB;
        if (b.downloadMB > 0.0) block["downloadMB"] = b.downloadMB;
        budgets[modelFamilyName((ModelFamily)f)] = block;
    }
    if (budgets.size())
        node["renderBudgets"] = budgets;
    return node;
}

void editor::decodeModelProjectSettings(const YAML::Node& node, ModelProjectSettings& settings){
    settings = ModelProjectSettings();
    if (!node || !node.IsMap())
        return;
    if (const YAML::Node families = node["defaults"]; families && families.IsMap()){
        for (size_t f = 0; f < kModelFamilyCount; f++){
            const YAML::Node family = families[modelFamilyName((ModelFamily)f)];
            if (!family || !family.IsMap())
                continue;
            // each value on its own: a bad one is skipped, not the platform's whole block
            for (const std::string& key : modelSettingKeys()){
                if (!family[key])
                    continue;
                YAML::Node single(YAML::NodeType::Map);
                single[key] = family[key];
                std::string error;
                Reader r{single, error, ""};
                ModelSettingValues value;
                readValues(r, value);
                if (!error.empty())
                    continue;
                ModelSettingValues& target = settings.defaults[f];
                if (value.optimizeMesh) target.optimizeMesh = value.optimizeMesh;
                if (value.meshCompression) target.meshCompression = value.meshCompression;
                if (value.weldVertices) target.weldVertices = value.weldVertices;
                if (value.indexFormat) target.indexFormat = value.indexFormat;
                if (value.readWrite) target.readWrite = value.readWrite;
                if (value.normals) target.normals = value.normals;
                if (value.smoothingAngle) target.smoothingAngle = value.smoothingAngle;
                if (value.generateLods) target.generateLods = value.generateLods;
                if (value.lodCount) target.lodCount = value.lodCount;
                if (value.lodReduction) target.lodReduction = value.lodReduction;
                if (value.shipSource) target.shipSource = value.shipSource;
            }
        }
    }
    try{
        if (node["cacheSizeMB"])
            settings.cacheSizeMB = std::clamp(node["cacheSizeMB"].as<int>(), 64, 1 << 20);
    }catch (...){}
    if (node["keepCpuDataForScripts"]){
        ScriptMeshData value;
        std::string name;
        try { name = node["keepCpuDataForScripts"].Scalar(); } catch (...) { name.clear(); }
        if (valueOf(scriptNames, name, value))
            settings.scriptMeshData = value;
    }
    if (const YAML::Node budgets = node["renderBudgets"]; budgets && budgets.IsMap()){
        for (size_t f = 0; f < kModelFamilyCount; f++){
            const YAML::Node block = budgets[modelFamilyName((ModelFamily)f)];
            if (!block || !block.IsMap())
                continue;
            RenderBudget& b = settings.renderBudgets[f];
            try { if (block["triangles"]) b.triangles = std::max(0LL, block["triangles"].as<long long>()); } catch (...) {}
            try { if (block["drawCalls"]) b.drawCalls = std::max(0LL, block["drawCalls"].as<long long>()); } catch (...) {}
            try { if (block["meshMemoryMB"]) b.meshMemoryMB = std::max(0.0, block["meshMemoryMB"].as<double>()); } catch (...) {}
            try { if (block["downloadMB"]) b.downloadMB = std::max(0.0, block["downloadMB"].as<double>()); } catch (...) {}
        }
    }
}
