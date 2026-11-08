// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProceduralBake.h"

#include "NodeLibrary.h"
#include "ProceduralMaterialKind.h"

#include "DocumentFile.h"
#include "EditorVersion.h"
#include "Project.h"
#include "Stream.h"
#include "asset/AssetMeta.h"
#include "texture/Material.h"
#include "command/type/SetAssetMetaCmd.h"
#include "texsynth/SynthesisRecipe.h"
#include "texsynth/SynthImage.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"
#include "util/Sha256.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::procmat;

namespace fs = std::filesystem;

namespace {

    bool isBakeOutput(const std::string& name) {
        const std::vector<std::string>& names = bakeOutputNames();
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    void hashFloat(Sha256& hash, float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        hash.update(&bits, sizeof(bits));
    }

    void hashInt(Sha256& hash, int64_t value) {
        hash.update(&value, sizeof(value));
    }

    bool identifierChar(char c) {
        return std::isalnum((unsigned char)c) || c == '_';
    }

    // A pass's text with its generators renumbered in the order they appear: the expander numbers every node
    // of the graph (o1, o2, ...; inside names too: nm_o3), so a node no output reads would shift the names of the
    // ones after it
    std::string canonicalNames(const std::string& text, std::map<std::string, std::string>& names) {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size();) {
            if (text[i] == 'o' && (i == 0 || !identifierChar(text[i - 1]) || text[i - 1] == '_') && i + 1 < text.size() && std::isdigit((unsigned char)text[i + 1])) {
                size_t end = i + 1;
                while (end < text.size() && std::isdigit((unsigned char)text[end])) end++;
                const std::string name = text.substr(i, end - i);
                auto found = names.find(name);
                if (found == names.end()) found = names.emplace(name, "g" + std::to_string(names.size() + 1)).first;
                out += found->second;
                i = end;
                continue;
            }
            out += text[i++];
        }
        return out;
    }

    void hashText(Sha256& hash, const std::string& text) {
        hashInt(hash, (int64_t)text.size());
        hash.update(text);
    }

    // What a baked PNG's meta says, by output: colour or data, and how it cooks (docs/textures.md).
    // Written rather than inferred: an ORM in both slots would meet the occlusion inference rule.
    YAML::Node importSettingsOf(const std::string& output, YAML::Node node) {
        node["version"] = 1;
        node.remove("inferredFrom");
        if (output == "normal") {
            node["type"] = "normal";
            node["srgb"] = false;
        } else if (output == "height") {
            node["type"] = "single_channel";
            node["channel"] = "r";
            node["srgb"] = false;
        } else {
            node["type"] = "default";
            node["srgb"] = output == "base_color" || output == "emission";
        }
        return node;
    }

    // The 8-bit image a PNG is written from: RGBA, or one channel for the height (R14)
    texsynth::Image pngImage(const std::string& output, const OutputImage& image) {
        const bool grey = output == "height";
        texsynth::Image out = texsynth::Image::make(image.width, image.height, grey ? 1 : 4, 8);
        if (grey) {
            for (size_t i = 0; i < (size_t)image.width * image.height; i++) out.data[i] = image.rgba[i * 4];
        } else {
            out.data = image.rgba;
        }
        return out;
    }

    std::string lowerExtension(const fs::path& path) {
        std::string extension = FileUtils::pathToUtf8(path.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return extension;
    }

    bool readText(const fs::path& path, std::string& text) {
        return ProjectChecks::readFile(path, text);
    }

}

const std::vector<std::string>& jasperengine::editor::procmat::bakeOutputNames() {
    static const std::vector<std::string> names = {"base_color", "normal", "orm", "emission", "height"};
    return names;
}

uint32_t jasperengine::editor::procmat::graphSeed(const graph::Graph& file) {
    const auto seed = file.metadata.find("seed");
    if (seed == file.metadata.end() || !seed->is_number()) return 0;
    const double value = seed->get<double>();
    return value >= 0.0 && value <= 2147483647.0 && value == std::floor(value) ? (uint32_t)value : 0;
}

std::vector<std::string> jasperengine::editor::procmat::graphBakeOutputs(const graph::Graph& file) {
    std::vector<std::string> names;
    for (const PlanRoot& root : materialBakeRoots(file)) names.push_back(root.name);
    return names;
}

bool jasperengine::editor::procmat::readGraphForBake(const fs::path& file, const std::string& rel, graph::Graph& out, std::string& error) {
    std::string text;
    if (!ProjectChecks::readFile(file, text)) {
        error = "could not read " + rel + ".";
        return false;
    }
    graph::LoadReport report;
    out = graph::load(text, report);
    if (!report.ok) {
        error = rel + " is not a graph file: " + report.error;
        return false;
    }
    if (out.kind != PROCEDURAL_MATERIAL_KIND) {
        error = rel + " is a '" + out.kind + "' graph, not a Procedural Material.";
        return false;
    }
    std::vector<std::string> errors;
    for (const graph::Issue& issue : graph::validate(out)) {
        if (issue.severity != graph::Severity::Error) continue;
        errors.push_back(issue.code + (issue.nodeId.empty() ? "" : " node '" + issue.nodeId + "'") + ": " + issue.message +
                         (issue.suggestion.empty() ? "" : " (" + issue.suggestion + ")"));
    }
    if (!errors.empty()) {
        error = rel + " has " + std::to_string(errors.size()) + " error(s); nothing was evaluated. Fix them with edit_graph:";
        for (size_t i = 0; i < errors.size() && i < 8; i++) error += "\n- " + errors[i];
        return false;
    }
    return true;
}

bool jasperengine::editor::procmat::planBake(const graph::Graph& file, const std::shared_ptr<const NodeLibrary>& library, const BakeOptions& options,
                                             PassPlan& plan, std::vector<std::string>& outputs, std::string& error) {
    if (options.size < BAKE_SIZE_MIN || options.size > BAKE_SIZE_MAX || (options.size & (options.size - 1)) != 0) {
        error = "size is a power of two from " + std::to_string(BAKE_SIZE_MIN) + " to " + std::to_string(BAKE_SIZE_MAX);
        return false;
    }
    std::vector<PlanRoot> roots = materialBakeRoots(file);
    if (roots.empty()) {
        error = "nothing is connected to the Material Output";
        return false;
    }
    if (!options.outputs.empty()) {
        std::vector<PlanRoot> chosen;
        for (const std::string& name : options.outputs) {
            auto found = std::find_if(roots.begin(), roots.end(), [&](const PlanRoot& root) { return root.name == name; });
            if (found == roots.end()) {
                std::string have;
                for (const PlanRoot& root : roots) have += (have.empty() ? "" : ", ") + root.name;
                error = isBakeOutput(name) ? "the graph has no " + name + " output (it has " + have + ")"
                                           : "'" + name + "' is not an output: base_color, normal, orm, emission or height";
                return false;
            }
            if (std::none_of(chosen.begin(), chosen.end(), [&](const PlanRoot& root) { return root.name == name; })) chosen.push_back(*found);
        }
        // in R14's order, whatever order they were asked in
        roots.erase(std::remove_if(roots.begin(), roots.end(), [&](const PlanRoot& root) {
            return std::none_of(chosen.begin(), chosen.end(), [&](const PlanRoot& c) { return c.name == root.name; });
        }), roots.end());
    }
    PlanOptions planOptions;
    planOptions.size = options.size;
    planOptions.readBack = true;
    planOptions.seed = (double)options.seed;
    if (!planGraph(file, library, roots, planOptions, plan, error)) return false;
    outputs.clear();
    for (const PlannedOutput& output : plan.outputs) outputs.push_back(output.name);
    return true;
}

std::string jasperengine::editor::procmat::bakeInputHash(const PassPlan& plan) {
    Sha256 hash;
    hashText(hash, "jasper.procmat bake");
    hashInt(hash, BAKE_VERSION);
    hashInt(hash, plan.size);
    for (const PlannedPass& pass : plan.passes) {
        std::map<std::string, std::string> names;
        hashText(hash, canonicalNames(pass.fragment, names));
        hashInt(hash, (int64_t)pass.uniforms.values.size());
        for (float value : pass.uniforms.values) hashFloat(hash, value);
        for (const PlannedTexture& texture : pass.textures) {
            hashText(hash, canonicalNames(texture.name, names));
            hashInt(hash, texture.buffer);
        }
        for (const PlannedSize& size : pass.sizes) {
            hashInt(hash, size.index);
            hashInt(hash, size.buffer);
        }
    }
    for (const PlannedBuffer& buffer : plan.buffers) {
        hashInt(hash, buffer.size);
        hashInt(hash, buffer.mips ? 1 : 0);
    }
    for (const PlannedStep& step : plan.steps) {
        hashInt(hash, step.pass);
        hashInt(hash, step.buffer);
        hashInt(hash, step.size);
    }
    for (const PlannedOutput& output : plan.outputs) {
        hashText(hash, output.name);
        hashInt(hash, output.buffer);
    }
    return hash.finishHex();
}

nlohmann::json jasperengine::editor::procmat::recipeToJson(const BakeRecipe& recipe) {
    return {{"format", BAKE_RECIPE_FORMAT}, {"generator", "jasperengine-editor procedural-material"}, {"engine", JASPERENGINE_EDITOR_VERSION},
            {"graph", {{"guid", recipe.graphGuid}, {"path", recipe.graphPath}}}, {"input_hash", recipe.inputHash}, {"seed", recipe.seed},
            {"size", recipe.size}, {"output", recipe.output}, {"outputs", recipe.outputs}, {"graph_outputs", recipe.graphOutputs},
            {"seed_from_graph", recipe.seedFromGraph}, {"tier", recipe.tier}, {"device", recipe.device}};
}

bool jasperengine::editor::procmat::recipeFromJson(const nlohmann::json& json, BakeRecipe& recipe, std::string& error) {
    const auto text = [&](const nlohmann::json& object, const char* key, std::string& out) {
        if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
            error = std::string(key) + " is not text";
            return false;
        }
        out = object[key].get<std::string>();
        return true;
    };
    if (!json.is_object() || json.value("format", 0) != BAKE_RECIPE_FORMAT) {
        error = "not a format " + std::to_string(BAKE_RECIPE_FORMAT) + " Procedural Material recipe";
        return false;
    }
    if (!json.contains("graph") || !text(json["graph"], "guid", recipe.graphGuid) || !text(json["graph"], "path", recipe.graphPath) ||
        !text(json, "input_hash", recipe.inputHash) || !text(json, "output", recipe.output) || !text(json, "tier", recipe.tier) ||
        !text(json, "device", recipe.device)) {
        if (error.empty()) error = "graph is not {guid, path}";
        return false;
    }
    if (!json.contains("seed") || !json["seed"].is_number_unsigned() || !json.contains("size") || !json["size"].is_number_integer()) {
        error = "seed or size is not a whole number";
        return false;
    }
    recipe.seed = json["seed"].get<uint32_t>();
    recipe.size = json["size"].get<int>();
    recipe.outputs.clear();
    if (!json.contains("outputs") || !json["outputs"].is_array()) {
        error = "outputs is not a list";
        return false;
    }
    for (const nlohmann::json& name : json["outputs"]) {
        if (!name.is_string() || !isBakeOutput(name.get<std::string>())) {
            error = "outputs holds something other than an output's name";
            return false;
        }
        recipe.outputs.push_back(name.get<std::string>());
    }
    // optional: what the graph had and where the seed came from
    recipe.graphOutputs = recipe.outputs;
    if (json.contains("graph_outputs")) {
        if (!json["graph_outputs"].is_array()) {
            error = "graph_outputs is not a list";
            return false;
        }
        recipe.graphOutputs.clear();
        for (const nlohmann::json& name : json["graph_outputs"]) {
            if (!name.is_string() || !isBakeOutput(name.get<std::string>())) {
                error = "graph_outputs holds something other than an output's name";
                return false;
            }
            recipe.graphOutputs.push_back(name.get<std::string>());
        }
    }
    recipe.seedFromGraph = json.value("seed_from_graph", false) == true;
    return true;
}

std::optional<BakeRecipe> jasperengine::editor::procmat::readBakeRecipe(const fs::path& png, std::string* error) {
    std::string why;
    const std::optional<std::string> text = texsynth::readRecipeTextHead(png, &why, BAKE_RECIPE_KEYWORD);
    if (!text) {
        if (error) *error = why;
        return std::nullopt;
    }
    BakeRecipe recipe;
    try {
        if (recipeFromJson(nlohmann::json::parse(*text), recipe, why)) return recipe;
    } catch (const std::exception&) {
        why = "the recipe is not JSON";
    }
    if (error) *error = "garbled jasper.procmat recipe: " + why;
    return std::nullopt;
}

std::string jasperengine::editor::procmat::bakeStem(const std::string& graphPath) {
    return FileUtils::pathToUtf8(FileUtils::pathFromUtf8(graphPath).stem());
}

std::string jasperengine::editor::procmat::defaultBakeFolder(const std::string& graphPath) {
    const fs::path path = FileUtils::pathFromUtf8(graphPath);
    return FileUtils::pathToGenericUtf8(path.parent_path() / path.stem());
}

std::string jasperengine::editor::procmat::bakedImageName(const std::string& stem, const std::string& output) {
    return stem + "_" + output + ".png";
}

std::string jasperengine::editor::procmat::bakedMaterialName(const std::string& stem) {
    return stem + ".material";
}

void jasperengine::editor::procmat::applyBakeToMaterial(Material& material, const std::vector<BakedSlot>& baked, const std::vector<BakedSlot>& dropped) {
    // the slots an output fills: the ORM is both the occlusion and the metallic-roughness texture
    const auto slots = [&](const std::string& output) {
        std::vector<Texture*> out;
        if (output == "base_color") out.push_back(&material.baseColorTexture);
        else if (output == "normal") out.push_back(&material.normalTexture);
        else if (output == "orm") out = {&material.metallicRoughnessTexture, &material.occlusionTexture};
        else if (output == "emission") out.push_back(&material.emissiveTexture);
        return out;
    };
    const auto holds = [](const Texture& texture, const BakedSlot& slot) {
        return texture.getPath() == slot.assetPath || (!slot.guid.empty() && texture.getPathGuid() == Guid::parse(slot.guid));
    };
    for (const BakedSlot& slot : dropped) {
        for (Texture* texture : slots(slot.output)) {
            if (!holds(*texture, slot)) continue;
            *texture = Texture();
            if (slot.output == "emission") material.emissiveFactor = Vector3(0.0f, 0.0f, 0.0f);
        }
    }
    for (const BakedSlot& slot : baked) {
        const std::vector<Texture*> targets = slots(slot.output);
        if (targets.empty()) continue;
        const bool already = holds(*targets.front(), slot);
        for (Texture* texture : targets) {
            Texture baked;
            baked.setPath(slot.assetPath);
            baked.setPathGuid(0, Guid::parse(slot.guid));
            *texture = baked;
        }
        if (already) continue;
        if (slot.output == "base_color") material.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        if (slot.output == "orm") {
            material.metallicFactor = 1.0f;
            material.roughnessFactor = 1.0f;
        }
        if (slot.output == "emission") material.emissiveFactor = Vector3(1.0f, 1.0f, 1.0f);
    }
}

std::string jasperengine::editor::procmat::metaGuid(const std::string& existing) {
    AssetMeta meta;
    if (existing.empty() || !AssetMeta::parse(existing, meta)) return std::string();
    return meta.guid.str();
}

std::string jasperengine::editor::procmat::bakedMetaText(const std::string& existing, const std::string& guid, const std::string& output) {
    AssetMeta meta;
    if (existing.empty() || !AssetMeta::parse(existing, meta)) {
        meta = AssetMeta();
        meta.guid = Guid::parse(guid);
    }
    YAML::Node import(YAML::NodeType::Map);
    if (meta.hasSection("import")) {
        try {
            const YAML::Node parsed = YAML::Load(meta.getSection("import"));
            if (parsed.IsMap()) import = YAML::Clone(parsed);
        } catch (const std::exception&) {
            // unreadable: written again whole, as a new meta would be
        }
    }
    const YAML::Node stored = import["textureImporter"];
    import["textureImporter"] = importSettingsOf(output, stored && stored.IsMap() ? YAML::Clone(stored) : YAML::Node(YAML::NodeType::Map));
    // the section's value as written after "import:", as SetAssetMetaCmd writes it
    YAML::Node wrapped(YAML::NodeType::Map);
    wrapped["import"] = import;
    YAML::Emitter out;
    out << wrapped;
    meta.setSection("import", std::string(out.c_str()).substr(std::strlen("import") + 1));
    return meta.serialize();
}

bool jasperengine::editor::procmat::encodeBake(Project* project, const BakeWrite& bake, BakeFiles& out, std::string& error) {
    out = BakeFiles();
    if (!project || project->getProjectPath().empty()) {
        error = "no project is open";
        return false;
    }
    const fs::path root = project->getProjectPath();
    const fs::path folder = root / FileUtils::pathFromUtf8(bake.folder);
    if (!project->isInsideAssetsPath(folder / "x")) {
        error = "output must be a folder inside the assets directory";
        return false;
    }
    const std::string stem = bakeStem(bake.graphPath);
    std::vector<std::string> outputs;
    for (const OutputImage& image : bake.images) outputs.push_back(image.name);

    std::vector<BakedSlot> baked, dropped;
    for (const OutputImage& image : bake.images) {
        if (!isBakeOutput(image.name) || image.rgba.size() != (size_t)image.width * image.height * 4) {
            error = "the evaluation gave no image for " + image.name;
            return false;
        }
        const fs::path absolute = folder / bakedImageName(stem, image.name);
        std::error_code ec;
        std::string existingMeta;
        if (fs::exists(absolute, ec)) {
            const std::optional<BakeRecipe> earlier = readBakeRecipe(absolute);
            if (!bake.overwrite && (!earlier || earlier->graphGuid != bake.recipe.graphGuid)) {
                error = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute)) +
                        " exists and is not a bake of this graph: overwrite: true replaces it, or bake into another output folder";
                return false;
            }
        }
        readText(SetAssetMetaCmd::metaPathOf(absolute), existingMeta);
        std::string guid = metaGuid(existingMeta);
        if (guid.empty()) guid = AssetDatabase::generateGuid();

        BakeRecipe recipe = bake.recipe;
        recipe.output = image.name;
        recipe.outputs = outputs;
        std::string text, recipeError;
        if (!texsynth::jsonText(recipeToJson(recipe), text, recipeError)) {
            error = recipeError;
            return false;
        }
        std::vector<uint8_t> png, withRecipe;
        if (!texsynth::encodePng(pngImage(image.name, image), png) || !texsynth::insertRecipeChunk(png, text, withRecipe, recipeError, BAKE_RECIPE_KEYWORD)) {
            error = "the " + image.name + " PNG could not be encoded" + (recipeError.empty() ? std::string() : ": " + recipeError);
            return false;
        }
        texsynth::OutputFile file;
        file.role = image.name;
        file.absolute = absolute;
        file.path = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute));
        file.bytes.assign(withRecipe.begin(), withRecipe.end());
        out.files.push_back(std::move(file));
        out.images.push_back(out.files.back().path);

        texsynth::OutputFile meta;
        meta.role = image.name + ".meta";
        meta.absolute = SetAssetMetaCmd::metaPathOf(absolute);
        meta.path = out.files.back().path + ".meta";
        meta.bytes = bakedMetaText(existingMeta, guid, image.name);
        out.files.push_back(std::move(meta));
        baked.push_back({image.name, FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(absolute)), guid});
    }
    // an output an earlier bake had and this one has not: its slot no longer shows this graph
    for (const std::string& name : bakeOutputNames()) {
        if (std::find(bake.graphOutputs.begin(), bake.graphOutputs.end(), name) != bake.graphOutputs.end()) continue;
        const fs::path absolute = folder / bakedImageName(stem, name);
        std::string existingMeta;
        readText(SetAssetMetaCmd::metaPathOf(absolute), existingMeta);
        dropped.push_back({name, FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(absolute)), metaGuid(existingMeta)});
    }

    const fs::path materialFile = folder / bakedMaterialName(stem);
    Material material;
    std::error_code ec;
    if (fs::exists(materialFile, ec)) {
        try {
            const YAML::Node node = DocumentFiles::read(materialFile, DocumentKind::Material).root;
            if (!node || !node.IsMap()) throw std::runtime_error("not a YAML map");
            material = Stream::decodeMaterial(node);
        } catch (const std::exception& e) {
            error = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(materialFile)) + " cannot be read (" + e.what() +
                    "): fix or remove it, nothing was written";
            return false;
        }
    }
    applyBakeToMaterial(material, baked, dropped);
    out.material = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(materialFile));
    material.name = out.material;
    texsynth::OutputFile file;
    file.role = "material";
    file.absolute = materialFile;
    file.path = out.material;
    file.bytes = project->materialFileText(materialFile, material);
    out.files.push_back(std::move(file));
    return true;
}

bool jasperengine::editor::procmat::writeBakeUndoable(Project* project, const BakeFiles& files, const std::string& stem, std::string& error,
                                                      const fs::path& beneath) {
    std::vector<WriteFilesCmd::Item> items;
    for (const texsynth::OutputFile& file : files.files) items.push_back({file.absolute, file.bytes});
    return ProjectFileWrites::writeUndoable(project, std::move(items), error, nullptr, nullptr, beneath, beneath.empty(),
                                            "Bake Procedural Material " + stem);
}

const char* jasperengine::editor::procmat::bakeStateName(BakeState state) {
    switch (state) {
        case BakeState::Fresh: return "fresh";
        case BakeState::Stale: return "stale";
        case BakeState::GraphMissing: return "graph_missing";
        case BakeState::GraphInvalid: return "graph_invalid";
    }
    return "stale";
}

fs::path jasperengine::editor::procmat::findBakeGraph(const fs::path& projectDir, const BakeRecipe& recipe) {
    const Guid wanted = Guid::parse(recipe.graphGuid);
    const fs::path recorded = projectDir / FileUtils::pathFromUtf8(recipe.graphPath);
    std::error_code ec;
    // the recorded path first: it is where the graph almost always still is
    Guid guid;
    std::vector<Guid> former;
    if (fs::is_regular_file(recorded, ec) && AssetDatabase::readInFileIdentity(recorded, guid, former) && guid == wanted) return recorded;
    for (const fs::path& file : ProjectChecks::findProjectFiles(projectDir, {PROCEDURAL_MATERIAL_EXTENSION})) {
        if (!AssetDatabase::readInFileIdentity(file, guid, former)) continue;
        if (guid == wanted || std::find(former.begin(), former.end(), wanted) != former.end()) return file;
    }
    return fs::is_regular_file(recorded, ec) ? recorded : fs::path();
}

BakeStatus jasperengine::editor::procmat::bakeStatus(const fs::path& projectDir, const BakeRecipe& recipe) {
    BakeStatus status;
    status.recipe = recipe;
    const fs::path graphFile = findBakeGraph(projectDir, recipe);
    if (graphFile.empty()) {
        status.state = BakeState::GraphMissing;
        status.reason = "its graph " + recipe.graphPath + " is gone";
        return status;
    }
    status.graphPath = FileUtils::pathToGenericUtf8(graphFile.lexically_relative(projectDir));
    std::string text;
    graph::LoadReport report;
    graph::Graph file;
    if (readText(graphFile, text)) file = graph::load(text, report);
    if (text.empty() || !report.ok) {
        status.state = BakeState::GraphInvalid;
        status.reason = status.graphPath + " does not load" + (report.error.empty() ? std::string() : ": " + report.error);
        return status;
    }
    // an output connected or disconnected since: the bake's .material no longer shows the graph
    const std::vector<std::string> now = graphBakeOutputs(file);
    std::vector<std::string> added, removed;
    for (const std::string& name : now) {
        if (std::find(recipe.graphOutputs.begin(), recipe.graphOutputs.end(), name) == recipe.graphOutputs.end()) added.push_back(name);
    }
    for (const std::string& name : recipe.graphOutputs) {
        if (std::find(now.begin(), now.end(), name) == now.end()) removed.push_back(name);
    }
    if (!added.empty() || !removed.empty()) {
        std::string what;
        for (const std::string& name : added) what += (what.empty() ? "" : ", ") + name + " is connected";
        for (const std::string& name : removed) what += (what.empty() ? "" : ", ") + name + " is no longer connected";
        status.state = BakeState::Stale;
        status.reason = status.graphPath + "'s outputs changed since this bake (" + what + ")";
        return status;
    }
    BakeOptions options;
    options.size = recipe.size;
    // the graph's own seed, when the bake took it: a reroll is an edit of the graph
    options.seed = recipe.seedFromGraph ? graphSeed(file) : recipe.seed;
    options.outputs = recipe.outputs;
    PassPlan plan;
    std::vector<std::string> outputs;
    std::string error;
    if (!planBake(file, NodeLibrary::shipped(), options, plan, outputs, error)) {
        status.state = BakeState::GraphInvalid;
        status.reason = status.graphPath + " does not bake: " + error;
        return status;
    }
    if (bakeInputHash(plan) != recipe.inputHash) {
        status.state = BakeState::Stale;
        status.reason = options.seed != recipe.seed ? status.graphPath + "'s seed changed since this bake (" + std::to_string(recipe.seed) + " to " +
                                                          std::to_string(options.seed) + ")"
                                                    : status.graphPath + " changed since this bake";
    }
    return status;
}

std::optional<BakeStatus> jasperengine::editor::procmat::cachedBakeStatus(const fs::path& projectDir, const fs::path& png) {
    struct Stamp {
        fs::file_time_type time{};
        uintmax_t size = 0;
        bool operator==(const Stamp& other) const { return time == other.time && size == other.size; }
    };
    const auto stampOf = [](const fs::path& path) {
        Stamp stamp;
        std::error_code ec;
        stamp.time = fs::last_write_time(path, ec);
        stamp.size = ec ? 0 : fs::file_size(path, ec);
        return stamp;
    };
    struct Entry {
        Stamp image;
        fs::path graph;
        Stamp graphStamp;
        std::optional<BakeStatus> status;
        std::chrono::steady_clock::time_point checkedAt;
    };
    static std::map<fs::path, Entry> cache;
    if (lowerExtension(png) != ".png") return std::nullopt;
    const Stamp image = stampOf(png);
    auto found = cache.find(png);
    // a graph that is gone may come back (an undone delete): asked again every couple of seconds, not each frame
    const auto now = std::chrono::steady_clock::now();
    if (found != cache.end() && found->second.image == image &&
        (found->second.graph.empty() ? (!found->second.status || now - found->second.checkedAt < std::chrono::seconds(2))
                                     : stampOf(found->second.graph) == found->second.graphStamp)) {
        return found->second.status;
    }
    Entry entry;
    entry.image = image;
    entry.checkedAt = now;
    if (const std::optional<BakeRecipe> recipe = readBakeRecipe(png)) {
        entry.status = bakeStatus(projectDir, *recipe);
        if (!entry.status->graphPath.empty()) {
            entry.graph = projectDir / FileUtils::pathFromUtf8(entry.status->graphPath);
            entry.graphStamp = stampOf(entry.graph);
        }
    }
    cache[png] = entry;
    return entry.status;
}

MaterialsCheck jasperengine::editor::procmat::checkMaterials(const fs::path& projectDir, const std::vector<fs::path>& files) {
    MaterialsCheck check;
    // the kind's own checks (strict parameters, its types) are what validate() reports
    registerProceduralMaterialKind();
    std::vector<fs::path> graphs, images;
    for (const fs::path& file : files) {
        const std::string extension = lowerExtension(file);
        if (extension == PROCEDURAL_MATERIAL_EXTENSION) graphs.push_back(file);
        else if (extension == ".png") images.push_back(file);
    }
    const bool whole = files.empty();
    if (whole) graphs = ProjectChecks::findProjectFiles(projectDir, {PROCEDURAL_MATERIAL_EXTENSION});
    const std::vector<fs::path> pngs = (whole || !graphs.empty()) ? ProjectChecks::findProjectFiles(projectDir, {".png"}) : std::vector<fs::path>();
    std::set<std::string> graphGuids;
    for (const fs::path& file : graphs) {
        check.graphs++;
        const std::string rel = ProjectChecks::projectRelative(projectDir, file);
        Guid guid;
        std::vector<Guid> former;
        if (AssetDatabase::readInFileIdentity(file, guid, former)) graphGuids.insert(guid.str());
        std::string text;
        graph::LoadReport report;
        graph::Graph loaded;
        if (readText(file, text)) loaded = graph::load(text, report);
        if (text.empty() || !report.ok) {
            check.findings.push_back({"materials", rel, 0, 0, "error", "not a graph file: " + (report.error.empty() ? std::string("it cannot be read") : report.error)});
            continue;
        }
        bool errors = false;
        for (const graph::Issue& issue : graph::validate(loaded)) {
            ProjectChecks::Finding finding;
            finding.tool = "materials";
            finding.path = rel;
            finding.severity = issue.severity == graph::Severity::Error ? "error" : "warning";
            finding.code = issue.code;
            finding.message = (issue.nodeId.empty() ? std::string() : "node '" + issue.nodeId + "': ") + issue.message;
            finding.suggestion = issue.suggestion;
            errors = errors || issue.severity == graph::Severity::Error;
            check.findings.push_back(finding);
        }
        if (errors) continue;
        PassPlan plan;
        std::vector<std::string> outputs;
        std::string error;
        if (!planBake(loaded, NodeLibrary::shipped(), BakeOptions(), plan, outputs, error)) {
            check.findings.push_back({"materials", rel, 0, 0, "error", "does not bake: " + error});
        }
    }
    // the bakes: the PNGs given, or those of the graphs checked (every one for the whole project)
    if (!whole) {
        for (const fs::path& png : pngs) {
            std::optional<BakeRecipe> recipe = readBakeRecipe(png);
            if (recipe && graphGuids.count(recipe->graphGuid)) images.push_back(png);
        }
    } else {
        images = pngs;
    }
    for (const fs::path& png : images) {
        const std::string rel = ProjectChecks::projectRelative(projectDir, png);
        std::string why;
        const std::optional<BakeRecipe> recipe = readBakeRecipe(png, &why);
        if (!recipe) {
            if (!why.empty() && why.find("jasper.procmat") != std::string::npos) check.findings.push_back({"materials", rel, 0, 0, "warning", why});
            continue;
        }
        check.bakes++;
        const BakeStatus status = bakeStatus(projectDir, *recipe);
        if (status.state == BakeState::Fresh) continue;
        ProjectChecks::Finding finding;
        finding.tool = "materials";
        finding.path = rel;
        finding.severity = "warning";
        finding.code = std::string("procmat-") + bakeStateName(status.state);
        finding.message = status.state == BakeState::GraphInvalid ? "its graph does not bake now: " + status.reason : "a stale bake: " + status.reason;
        finding.suggestion = status.state == BakeState::GraphMissing ? "bake it again from its graph, or delete the baked files"
                                                                     : "bake it again (bake_procedural_material, or procedural-material bake " + status.graphPath + ")";
        check.findings.push_back(finding);
    }
    return check;
}

namespace {

    // the material_output ports an output of the bake reads
    std::vector<std::string> portsOf(const graph::Graph& file, const graph::Node& output, const std::string& name) {
        if (name == "orm") return {"occlusion", "roughness", "metallic"};
        if (name == "normal") {
            const bool connected = std::any_of(file.edges.begin(), file.edges.end(), [&](const graph::Edge& edge) {
                return edge.to.node == output.id && edge.to.port == "normal";
            });
            return {connected ? "normal" : "height"};
        }
        return {name};
    }

    // every node an output of the bake is drawn from
    std::vector<const graph::Node*> upstreamOf(const graph::Graph& file, const graph::Node& output, const std::string& name) {
        std::vector<std::string> pending;
        for (const std::string& port : portsOf(file, output, name)) {
            for (const graph::Edge& edge : file.edges) {
                if (edge.to.node == output.id && edge.to.port == port) pending.push_back(edge.from.node);
            }
        }
        std::set<std::string> seen;
        std::vector<const graph::Node*> nodes;
        while (!pending.empty()) {
            const std::string id = pending.back();
            pending.pop_back();
            if (!seen.insert(id).second) continue;
            if (const graph::Node* node = file.findNode(id)) nodes.push_back(node);
            for (const graph::Edge& edge : file.edges) {
                if (edge.to.node == id) pending.push_back(edge.from.node);
            }
        }
        return nodes;
    }

    std::string numberText(double value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", value);
        return text;
    }

}

void jasperengine::editor::procmat::attachGraphFixes(qa::Json& report, const graph::Graph& file, const std::shared_ptr<const NodeLibrary>& library) {
    if (!report.contains("fixes") || !report["fixes"].is_array()) report["fixes"] = qa::Json::array();
    const graph::Node* output = nullptr;
    for (const graph::Node& node : file.nodes) {
        if (node.type == MATERIAL_OUTPUT_NODE) output = &node;
    }
    if (!output || !report.contains("checks")) return;
    for (qa::Json& row : report["checks"]) {
        const std::string id = row.value("id", "");
        const std::string status = row.value("status", "");
        if (id == "tileability" && status == "FAIL" && row["measured"].is_object() && row["threshold"].is_number()) {
            const double threshold = row["threshold"].get<double>();
            std::vector<std::string> named;
            std::set<std::string> seen;
            for (auto it = row["measured"].begin(); it != row["measured"].end(); ++it) {
                if (!it.value().is_number() || it.value().get<double>() <= threshold) continue;
                for (const graph::Node* node : upstreamOf(file, *output, it.key())) {
                    const Json* descriptor = library ? library->descriptor(node->type) : nullptr;
                    if (!descriptor || !descriptor->contains("parameters")) continue;
                    for (const Json& parameter : (*descriptor)["parameters"]) {
                        const std::string name = parameter.value("name", std::string());
                        if (parameter.value("type", std::string()) != "float" || name.find("scale") == std::string::npos) continue;
                        const auto stored = node->params.find(name);
                        const Json& value = stored != node->params.end() ? *stored : parameter.value("default", Json());
                        if (!value.is_number() || std::floor(value.get<double>()) == value.get<double>()) continue;
                        double whole = std::max(1.0, std::round(value.get<double>()));
                        if (parameter.contains("max") && parameter["max"].is_number()) whole = std::min(whole, std::floor(parameter["max"].get<double>()));
                        if (parameter.contains("min") && parameter["min"].is_number() && whole < parameter["min"].get<double>()) continue;
                        if (!seen.insert(node->id + "|" + name).second) continue;
                        named.push_back("node " + node->id + " (" + node->type + ") has " + name + " " + numberText(value.get<double>()));
                        report["fixes"].push_back({{"check", "tileability"}, {"node", node->id},
                                                   {"op", {{"op", "setParam"}, {"node", node->id}, {"name", name}, {"value", whole}}}});
                    }
                }
            }
            if (!named.empty()) {
                std::string list;
                for (const std::string& item : named) list += (list.empty() ? "" : "; ") + item;
                row["limited_by"] = row.value("limited_by", std::string()) + " " + list + ": not a whole number of repeats.";
                const qa::Json& op = report["fixes"].back()["op"];
                row["fix_hint"] = named.size() == 1 ? "Set " + op.value("name", std::string()) + " of node " + op.value("node", std::string()) + " to " +
                                                          numberText(op["value"].get<double>()) + ": a whole number of repeats tiles."
                                                    : "Give each of them a whole number of repeats: a scale with a fraction does not tile.";
            }
        }
        if (id == "normal" && (status == "FAIL" || status == "WARN")) {
            const auto into = [&](const char* port) {
                return std::find_if(file.edges.begin(), file.edges.end(), [&](const graph::Edge& edge) { return edge.to.node == output->id && edge.to.port == port; });
            };
            const auto normal = into("normal");
            if (normal == file.edges.end() || into("height") == file.edges.end()) continue;
            row["limited_by"] = row.value("limited_by", std::string()) + " Node " + normal->from.node + " feeds Normal.";
            report["fixes"].push_back({{"check", "normal"}, {"node", output->id}, {"op", {{"op", "disconnect"}, {"id", normal->id}}}});
        }
    }
}

qa::FailRounds& jasperengine::editor::procmat::checkRounds() {
    static qa::FailRounds rounds;
    return rounds;
}

std::string jasperengine::editor::procmat::bakedHeightReader(Project* project, const std::string& graphPath) {
    const fs::path materialFile = project->getProjectPath() / FileUtils::pathFromUtf8(defaultBakeFolder(graphPath)) / bakedMaterialName(bakeStem(graphPath));
    std::error_code ec;
    if (!fs::exists(materialFile, ec)) return std::string();
    Material material;
    try {
        const YAML::Node node = DocumentFiles::read(materialFile, DocumentKind::Material).root;
        if (!node || !node.IsMap()) return std::string();
        material = Stream::decodeMaterial(node);
    } catch (const std::exception&) {
        return std::string();
    }
    for (const auto& [uniform, texture] : material.shaderTextures) {
        if (texture.getPath().empty()) continue;
        const std::optional<BakeRecipe> recipe = readBakeRecipe(project->getAssetsPath() / FileUtils::pathFromUtf8(texture.getPath()));
        if (recipe && recipe->output == "height") return "the shader texture " + uniform;
    }
    return std::string();
}

qa::Json jasperengine::editor::procmat::graphCheckReport(Project* project, const std::string& path, const graph::Graph& graph,
                                                          const EvaluationResult& images, const std::vector<std::string>& only) {
    MaterialImages material;
    for (const OutputImage& image : images.outputs) material.outputs[image.name] = image;
    // the graph's own height output is what a shader of its bake would sample
    if (!materialOutputRoots(graph).empty()) material.heightReader = bakedHeightReader(project, path);
    CheckRequest request;
    request.qa = project->getProcMatQaSettings();
    request.only = only;
    request.rounds = checkRounds().previous(path);
    qa::Json report = ProceduralChecks::report(material, request);
    attachGraphFixes(report, graph, NodeLibrary::shipped());
    return report;
}
