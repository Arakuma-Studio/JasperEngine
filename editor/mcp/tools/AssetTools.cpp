// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W4: assets, import, materials, shaders, text files, placeholder art
#include "McpTools.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/legacy/EditorActionExecutor.h"

#include "App.h"
#include "Catalog.h"
#include "Out.h"
#include "Project.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CopyFileCmd.h"
#include "command/type/CreateDirCmd.h"
#include "command/type/DeleteFileCmd.h"
#include "command/type/ForkShaderCmd.h"
#include "command/type/LinkMaterialCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/RenameFileCmd.h"
#include "command/type/ScenePropertyCmd.h"
#include "command/type/UnlinkMaterialCmd.h"
#include "component/MeshComponent.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "texture/Material.h"
#include "util/Base64.h"
#include "util/ProjectUtils.h"
#include "util/Util.h"
#include "window/CodeEditor.h"
#include "window/ProjectWindow.h"

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace jasperengine::editor::mcp {

namespace fs = std::filesystem;

// ======================================================================== shader checks (W4Shared.h)

namespace w4 {

namespace {

bool shaderTypeForComponent(ComponentType component, ShaderType& out) {
    switch (component) {
        case ComponentType::MeshComponent: out = ShaderType::MESH; return true;
        case ComponentType::UIComponent: out = ShaderType::UI; return true;
        case ComponentType::PointsComponent: out = ShaderType::POINTS; return true;
        case ComponentType::LineRendererComponent: out = ShaderType::LINES; return true;
        case ComponentType::SkyboxComponent: out = ShaderType::SKYBOX; return true;
        default: return false;
    }
}

// Every customShader value in use in the loaded scenes, with its shader type
std::vector<ShaderUse> allShaderForks(Project* project) {
    std::vector<ShaderUse> uses;
    std::set<std::pair<std::string, int>> seen;
    auto add = [&](const std::string& base, ShaderType type) {
        if (base.empty()) return;
        if (seen.insert({base, static_cast<int>(type)}).second) uses.push_back({base, static_cast<int>(type)});
    };
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (!sceneProject->scene) continue;
        Scene* scene = sceneProject->scene;
        for (Entity entity : sceneProject->entities) {
            for (ComponentType component : Catalog::findComponents(scene, entity)) {
                ShaderType type;
                if (!shaderTypeForComponent(component, type)) continue;
                if (auto* value = Catalog::getPropertyRef<std::string>(scene, entity, component, "customShader")) add(*value, type);
                if (component == ComponentType::MeshComponent) {
                    if (auto* depth = Catalog::getPropertyRef<std::string>(scene, entity, component, "customDepthShader")) add(*depth, ShaderType::DEPTH);
                }
            }
        }
        const std::pair<const char*, ShaderType> defaults[] = {
            {"default_mesh_shader", ShaderType::MESH}, {"default_ui_shader", ShaderType::UI},
            {"default_sky_shader", ShaderType::SKYBOX}, {"default_points_shader", ShaderType::POINTS},
            {"default_lines_shader", ShaderType::LINES}};
        for (const auto& [property, type] : defaults) {
            add(Catalog::getSceneProperty<std::string>(scene, property), type);
        }
        for (const PostProcessPass& pass : scene->getPostProcessPasses()) add(pass.shader, ShaderType::POSTPROCESS);
    }
    return uses;
}

} // namespace

std::vector<ShaderUse> shaderForksUsingFiles(Project* project, const std::vector<std::string>& files) {
    std::vector<ShaderUse> result;
    std::set<std::string> covered;
    const std::vector<ShaderUse> forks = allShaderForks(project);
    for (const std::string& file : files) {
        const std::string ext = extensionOf(file);
        if (ext == ".glsl") {
            // an include can be reached by any fork (private copies, project-root overrides)
            for (const ShaderUse& use : forks) {
                if (covered.insert(use.base + "#" + std::to_string(use.shaderType)).second) result.push_back(use);
            }
            continue;
        }
        bool found = false;
        for (const ShaderUse& use : forks) {
            Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(use.base);
            if (paths.vert == file || paths.frag == file) {
                found = true;
                if (covered.insert(use.base + "#" + std::to_string(use.shaderType)).second) result.push_back(use);
            }
        }
        if (!found && (ext == ".vert" || ext == ".frag")) {
            // not used by anything yet: check it if its pair exists
            const std::string base = (fs::path(file).parent_path() / fs::path(file).stem()).generic_string();
            std::error_code ec;
            if (fs::exists(project->getProjectPath() / (base + ".vert"), ec) && fs::exists(project->getProjectPath() / (base + ".frag"), ec) &&
                covered.insert(base + "#-1").second) {
                result.push_back({base, -1});
            }
        }
    }
    return result;
}

namespace {

std::string shaderTypeName(int type) {
    switch (static_cast<ShaderType>(type)) {
        case ShaderType::MESH: return "mesh";
        case ShaderType::UI: return "ui";
        case ShaderType::SKYBOX: return "sky";
        case ShaderType::POINTS: return "points";
        case ShaderType::LINES: return "lines";
        case ShaderType::DEPTH: return "depth";
        case ShaderType::POSTPROCESS: return "post_process";
        default: return "unknown";
    }
}

// Guesses the type of an unused fork from its fragment entry point name / includes
int guessShaderType(Project* project, const std::string& base) {
    std::string frag;
    readFile(project->getProjectPath() / Util::resolveCustomShaderPaths(base).frag, frag);
    if (frag.find("u_fs_postParams") != std::string::npos || frag.find("postprocess") != std::string::npos) return static_cast<int>(ShaderType::POSTPROCESS);
    if (frag.find("includes/ui") != std::string::npos) return static_cast<int>(ShaderType::UI);
    if (frag.find("includes/sky") != std::string::npos) return static_cast<int>(ShaderType::SKYBOX);
    if (frag.find("includes/points") != std::string::npos) return static_cast<int>(ShaderType::POINTS);
    if (frag.find("includes/lines") != std::string::npos) return static_cast<int>(ShaderType::LINES);
    return static_cast<int>(ShaderType::MESH);
}

Json parseShaderLog(const std::string& log) {
    // glslang: "ERROR: 0:12: 'x' : undeclared identifier" (also "<file>:12:")
    static const std::regex line(R"((ERROR|WARNING):\s*([^:\n]*):(\d+):\s*(.*))");
    Json errors = Json::array();
    std::istringstream in(log);
    std::string text;
    while (std::getline(in, text)) {
        std::smatch m;
        if (std::regex_search(text, m, line)) {
            errors.push_back({{"severity", m[1].str() == "ERROR" ? "error" : "warning"}, {"source", m[2].str()},
                              {"line", std::atoi(m[3].str().c_str())}, {"message", m[4].str()}});
        }
        if (errors.size() >= 30) break;
    }
    return errors;
}

} // namespace

void checkShadersAsync(const CallPtr& call, std::vector<ShaderUse> forks, std::function<void(Json results)> done) {
    Project* project = call->ctx().project();
    // Drops cached builds and source snapshots so both the check and the scene use the files on disk
    project->invalidateCustomShaders();
    struct Job {
        std::vector<ShaderUse> forks;
        std::vector<ShaderKey> keys;
        Json results = Json::array();
        std::atomic<bool> finished{false};
    };
    auto job = std::make_shared<Job>();
    for (ShaderUse& use : forks) {
        if (use.shaderType < 0) use.shaderType = guessShaderType(project, use.base);
        const uint16_t customId = ShaderPool::registerCustomShader(use.base);
        job->keys.push_back(ShaderPool::getShaderKey(static_cast<ShaderType>(use.shaderType), 0, customId));
        job->forks.push_back(use);
    }
    std::thread([job, project]() {
        ShaderBuilder builder;
        for (size_t i = 0; i < job->forks.size(); i++) {
            const uint64_t before = Out::lastLogId();
            Json item = {{"shader", job->forks[i].base}, {"type", shaderTypeName(job->forks[i].shaderType)}};
            try {
                builder.buildShaderForExport(job->keys[i], project, ShaderPool::getShaderBackend());
                item["ok"] = true;
            } catch (const std::exception& e) {
                item["ok"] = false;
                std::string log;
                for (const LogEntry& entry : Out::getLogsSince(before)) {
                    if (entry.type == LogType::Error && entry.message.find("Shader") != std::string::npos) log += entry.message + "\n";
                }
                if (log.empty()) log = e.what();
                if (log.size() > 6000) log = log.substr(0, 6000) + "\n...";
                item["errors"] = parseShaderLog(log);
                item["log"] = log;
            }
            job->results.push_back(item);
        }
        job->finished = true;
    }).detach();
    call->waitUntil([job]() { return job->finished.load(); }, 120000, [job, done](bool timedOut) {
        if (timedOut) {
            done(Json::array({{{"ok", false}, {"log", "Shader check timed out."}}}));
            return;
        }
        done(job->results);
    });
}

} // namespace w4

// ======================================================================== asset tools

namespace {

using namespace w4;

// ------------------------------------------------------------------ helpers

bool resolveExisting(const CallPtr& call, const std::string& key, fs::path& out, bool mustExist = true) {
    const Json& args = call->args();
    if (!args.contains(key) || !args[key].is_string() || args[key].get<std::string>().empty()) {
        call->fail(key + " is required (a project-relative path).");
        return false;
    }
    std::string error;
    if (!McpRefs::resolveProjectPath(call->ctx().project(), args[key].get<std::string>(), out, error, mustExist)) {
        call->fail(error);
        return false;
    }
    out = out.lexically_normal();
    return true;
}

bool addProjectCommand(McpContext& ctx, Command* cmd) {
    cmd->setNoMerge();
    const bool ok = ctx.project()->getProjectCommandHistory()->addCommand(cmd);
    if (ProjectWindow* window = ctx.projectWindow()) window->refreshCurrentDirectory();
    return ok;
}

bool addSceneCommand(uint32_t sceneId, Command* cmd) {
    cmd->setNoMerge();
    return CommandHandle::get(sceneId)->addCommand(cmd);
}

Json legacyToResult(const legacy::ActionResult& action) {
    Json data = action.data.is_object() ? action.data : Json::object();
    return data;
}

void replyLegacy(const CallPtr& call, const legacy::ActionResult& action, Json extra = Json::object()) {
    Json data = legacyToResult(action);
    for (auto it = extra.begin(); it != extra.end(); ++it) data[it.key()] = it.value();
    if (action.success) call->ok(action.message, data);
    else call->fail(action.message, data);
}

// scene/entity → the legacy scene_id/entity_id arguments
bool legacyTarget(const CallPtr& call, Json& legacyArgs, SceneProject*& scene, Entity& entity, bool entityRequired) {
    const Json& args = call->args();
    std::string error;
    scene = McpRefs::resolveScene(call->ctx().project(), args.contains("scene") ? args["scene"] : Json(), error);
    if (!scene) {
        call->fail(error);
        return false;
    }
    if (!scene->scene) {
        call->fail("Scene \"" + scene->name + "\" is not loaded; open it first.");
        return false;
    }
    legacyArgs["scene_id"] = scene->id;
    entity = NULL_ENTITY;
    if (args.contains("entity") && !args["entity"].is_null()) {
        entity = McpRefs::resolveEntity(scene, args["entity"], error);
        if (entity == NULL_ENTITY) {
            call->fail(error);
            return false;
        }
        legacyArgs["entity_id"] = entity;
    } else if (entityRequired) {
        call->fail("entity is required: a GameObject id or path.");
        return false;
    }
    if (args.contains("component") && args["component"].is_string()) {
        ComponentType type;
        if (!McpRefs::resolveComponent(args["component"], type, error)) {
            call->fail(error);
            return false;
        }
        legacyArgs["component"] = Catalog::getComponentName(type);
    }
    return true;
}

std::string lowerCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Glob: '*' any run except '/', '**' any run, '?' one char. Case-insensitive.
bool globMatch(const std::string& pattern, const std::string& text) {
    size_t p = 0, t = 0, starP = std::string::npos, starT = 0;
    bool starCrosses = false;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starCrosses = p + 1 < pattern.size() && pattern[p + 1] == '*';
            p += starCrosses ? 2 : 1;
            if (starCrosses && p < pattern.size() && pattern[p] == '/') p++;   // "**/" also matches no directory
            starP = p;
            starT = t;
            continue;
        }
        if (p < pattern.size() && (pattern[p] == '?' ? text[t] != '/' : std::tolower(static_cast<unsigned char>(pattern[p])) ==
                                                                          std::tolower(static_cast<unsigned char>(text[t])))) {
            p++;
            t++;
            continue;
        }
        if (starP != std::string::npos && (starCrosses || text[starT] != '/')) {
            starT++;
            t = starT;
            p = starP;
            continue;
        }
        return false;
    }
    while (p < pattern.size() && pattern[p] == '*') p++;
    return p == pattern.size();
}

bool color01(const Json& value, std::array<float, 4>& out, std::string& error) {
    if (value.is_string()) {
        std::string hex = value.get<std::string>();
        if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
        if (hex.size() != 6 && hex.size() != 8) {
            error = "colors are [r,g,b(,a)] in 0..1 or \"#RRGGBB(AA)\".";
            return false;
        }
        for (size_t i = 0; i < hex.size() / 2; i++) {
            out[i] = static_cast<float>(std::stoi(hex.substr(i * 2, 2), nullptr, 16)) / 255.0f;
        }
        if (hex.size() == 6) out[3] = 1.0f;
        return true;
    }
    if (value.is_array() && (value.size() == 3 || value.size() == 4)) {
        for (size_t i = 0; i < value.size(); i++) {
            if (!value[i].is_number()) {
                error = "color components must be numbers 0..1.";
                return false;
            }
            out[i] = std::clamp(value[i].get<float>(), 0.0f, 1.0f);
        }
        if (value.size() == 3) out[3] = 1.0f;
        return true;
    }
    error = "colors are [r,g,b(,a)] in 0..1 or \"#RRGGBB(AA)\".";
    return false;
}

bool encodePng(const unsigned char* rgba, int width, int height, int maxSize, std::string& base64, int& outW, int& outH) {
    std::vector<unsigned char> scaled;
    const unsigned char* pixels = rgba;
    outW = width;
    outH = height;
    if (maxSize > 0 && (width > maxSize || height > maxSize)) {
        const double scale = static_cast<double>(maxSize) / std::max(width, height);
        outW = std::max(1, static_cast<int>(std::lround(width * scale)));
        outH = std::max(1, static_cast<int>(std::lround(height * scale)));
        scaled.resize(static_cast<size_t>(outW) * outH * 4);
        if (!stbir_resize_uint8_srgb(rgba, width, height, 0, scaled.data(), outW, outH, 0, STBIR_RGBA)) return false;
        pixels = scaled.data();
    }
    std::vector<unsigned char> png;
    auto append = [](void* context, void* data, int size) {
        auto* bytes = static_cast<std::vector<unsigned char>*>(context);
        const unsigned char* begin = static_cast<const unsigned char*>(data);
        bytes->insert(bytes->end(), begin, begin + size);
    };
    if (!stbi_write_png_to_func(append, &png, outW, outH, 4, pixels, outW * 4)) return false;
    base64 = Base64::encode(png.data(), png.size());
    return true;
}

// ------------------------------------------------------------------ model / audio info

Json gltfSummary(const Json& gltf) {
    Json info = Json::object();
    auto names = [](const Json& list, const char* fallback) {
        Json out = Json::array();
        if (!list.is_array()) return out;
        for (size_t i = 0; i < list.size() && i < 100; i++) {
            out.push_back(list[i].value("name", std::string(fallback) + " " + std::to_string(i)));
        }
        return out;
    };
    if (gltf.contains("asset") && gltf["asset"].is_object()) info["generator"] = gltf["asset"].value("generator", "");
    info["scenes"] = gltf.contains("scenes") ? gltf["scenes"].size() : 0;
    info["nodes"] = gltf.contains("nodes") ? gltf["nodes"].size() : 0;
    Json meshes = Json::array();
    if (gltf.contains("meshes") && gltf["meshes"].is_array()) {
        for (size_t i = 0; i < gltf["meshes"].size() && i < 100; i++) {
            const Json& mesh = gltf["meshes"][i];
            meshes.push_back({{"name", mesh.value("name", "mesh " + std::to_string(i))},
                              {"primitives", mesh.contains("primitives") ? mesh["primitives"].size() : 0}});
        }
    }
    info["meshes"] = meshes;
    info["materials"] = names(gltf.value("materials", Json::array()), "material");
    Json animations = Json::array();
    if (gltf.contains("animations") && gltf["animations"].is_array()) {
        const Json accessors = gltf.value("accessors", Json::array());
        for (size_t i = 0; i < gltf["animations"].size() && i < 100; i++) {
            const Json& animation = gltf["animations"][i];
            double duration = 0.0;
            if (animation.contains("samplers")) {
                for (const Json& sampler : animation["samplers"]) {
                    const int input = sampler.value("input", -1);
                    if (input >= 0 && input < static_cast<int>(accessors.size()) && accessors[input].contains("max") &&
                        accessors[input]["max"].is_array() && !accessors[input]["max"].empty()) {
                        duration = std::max(duration, accessors[input]["max"][0].get<double>());
                    }
                }
            }
            animations.push_back({{"name", animation.value("name", "animation " + std::to_string(i))},
                                  {"channels", animation.contains("channels") ? animation["channels"].size() : 0},
                                  {"duration_seconds", std::round(duration * 1000.0) / 1000.0}});
        }
    }
    info["animations"] = animations;
    info["skins"] = gltf.contains("skins") ? gltf["skins"].size() : 0;
    info["textures"] = gltf.contains("textures") ? gltf["textures"].size() : 0;
    Json images = Json::array();
    if (gltf.contains("images") && gltf["images"].is_array()) {
        for (const Json& image : gltf["images"]) {
            images.push_back(image.contains("uri") && image["uri"].is_string() && image["uri"].get<std::string>().rfind("data:", 0) != 0
                                 ? image["uri"].get<std::string>() : std::string("(embedded)"));
            if (images.size() >= 50) break;
        }
    }
    info["images"] = images;
    return info;
}

Json modelInfo(const fs::path& file, std::string& error) {
    const std::string ext = extensionOf(file);
    std::string content;
    if (!readFile(file, content)) {
        error = "Can't read the file.";
        return nullptr;
    }
    try {
        if (ext == ".gltf") return gltfSummary(Json::parse(content));
        if (ext == ".glb") {
            if (content.size() < 20 || content.compare(0, 4, "glTF") != 0) {
                error = "Not a binary glTF file.";
                return nullptr;
            }
            uint32_t chunkLength = 0, chunkType = 0;
            std::memcpy(&chunkLength, content.data() + 12, 4);
            std::memcpy(&chunkType, content.data() + 16, 4);
            if (chunkType != 0x4E4F534A || 20 + static_cast<size_t>(chunkLength) > content.size()) {
                error = "The GLB has no JSON chunk.";
                return nullptr;
            }
            return gltfSummary(Json::parse(content.substr(20, chunkLength)));
        }
        if (ext == ".obj") {
            size_t vertices = 0, faces = 0;
            Json objects = Json::array(), materials = Json::array(), libraries = Json::array();
            std::istringstream in(content);
            std::string line;
            while (std::getline(in, line)) {
                if (line.rfind("v ", 0) == 0) vertices++;
                else if (line.rfind("f ", 0) == 0) faces++;
                else if ((line.rfind("o ", 0) == 0 || line.rfind("g ", 0) == 0) && objects.size() < 100) objects.push_back(line.substr(2));
                else if (line.rfind("usemtl ", 0) == 0 && materials.size() < 100) materials.push_back(line.substr(7));
                else if (line.rfind("mtllib ", 0) == 0) libraries.push_back(line.substr(7));
            }
            return Json{{"vertices", vertices}, {"faces", faces}, {"objects", objects}, {"materials", materials}, {"mtllib", libraries}};
        }
    } catch (const std::exception& e) {
        error = std::string("Can't parse the model: ") + e.what();
        return nullptr;
    }
    error = "Unknown model format.";
    return nullptr;
}

Json wavInfo(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    char header[12];
    if (!in.read(header, 12) || std::strncmp(header, "RIFF", 4) != 0 || std::strncmp(header + 8, "WAVE", 4) != 0) return nullptr;
    uint16_t channels = 0;
    uint32_t sampleRate = 0, byteRate = 0, dataSize = 0;
    char chunk[8];
    while (in.read(chunk, 8)) {
        uint32_t size = 0;
        std::memcpy(&size, chunk + 4, 4);
        if (std::strncmp(chunk, "fmt ", 4) == 0) {
            std::vector<char> fmt(size);
            in.read(fmt.data(), size);
            if (size >= 12) {
                std::memcpy(&channels, fmt.data() + 2, 2);
                std::memcpy(&sampleRate, fmt.data() + 4, 4);
                std::memcpy(&byteRate, fmt.data() + 8, 4);
            }
        } else if (std::strncmp(chunk, "data", 4) == 0) {
            dataSize = size;
            break;
        } else {
            in.seekg(size + (size & 1), std::ios::cur);
        }
    }
    Json info = {{"channels", channels}, {"sample_rate", sampleRate}};
    if (byteRate > 0) info["duration_seconds"] = std::round(static_cast<double>(dataSize) / byteRate * 1000.0) / 1000.0;
    return info;
}

// ------------------------------------------------------------------ materials

const std::vector<std::pair<const char*, Texture Material::*>>& materialTextures() {
    static const std::vector<std::pair<const char*, Texture Material::*>> textures = {
        {"baseColorTexture", &Material::baseColorTexture},
        {"emissiveTexture", &Material::emissiveTexture},
        {"metallicRoughnessTexture", &Material::metallicRoughnessTexture},
        {"occlusionTexture", &Material::occlusionTexture},
        {"normalTexture", &Material::normalTexture}};
    return textures;
}

const std::vector<std::pair<const char*, int Material::*>>& materialTexCoords() {
    static const std::vector<std::pair<const char*, int Material::*>> fields = {
        {"baseColorTexCoord", &Material::baseColorTexCoord},
        {"metallicRoughnessTexCoord", &Material::metallicRoughnessTexCoord},
        {"occlusionTexCoord", &Material::occlusionTexCoord},
        {"emissiveTexCoord", &Material::emissiveTexCoord},
        {"normalTexCoord", &Material::normalTexCoord}};
    return fields;
}

std::string alphaModeName(MaterialAlphaMode mode) {
    switch (mode) {
        case MaterialAlphaMode::ALPHA_OPAQUE: return "OPAQUE";
        case MaterialAlphaMode::MASK: return "MASK";
        case MaterialAlphaMode::BLEND: return "BLEND";
        default: return "AUTO";
    }
}

Json materialToJson(Project* project, const Material& material) {
    Json data = {
        {"baseColorFactor", McpValues::toJson(material.baseColorFactor)},
        {"metallicFactor", material.metallicFactor},
        {"roughnessFactor", material.roughnessFactor},
        {"alphaCutoff", material.alphaCutoff},
        {"alphaMode", alphaModeName(material.alphaMode)},
        {"emissiveFactor", McpValues::toJson(material.emissiveFactor)},
        {"foliageTransmission", material.foliageTransmission}
    };
    for (const auto& [name, member] : materialTextures()) {
        const Texture& texture = material.*member;
        data[name] = texture.empty() ? Json(nullptr) : Json(McpValues::assetPathToJson(project, texture.getPath(0)));
    }
    for (const auto& [name, member] : materialTexCoords()) data[name] = material.*member;
    return data;
}

bool applyMaterialJson(Project* project, Material& material, const Json& values, std::string& error) {
    for (auto it = values.begin(); it != values.end(); ++it) {
        const std::string key = it.key();
        const Json& value = it.value();
        bool handled = true;
        if (key == "baseColorFactor") {
            Vector4 v = material.baseColorFactor;
            if (!McpValues::parseVector4(value, v)) {
                Vector3 rgb;
                if (!McpValues::parseVector3(value, rgb)) {
                    error = "baseColorFactor must be [r,g,b,a] (linear, 0..1).";
                    return false;
                }
                v = Vector4(rgb.x, rgb.y, rgb.z, 1.0f);
            }
            material.baseColorFactor = v;
        } else if (key == "emissiveFactor") {
            if (!McpValues::parseVector3(value, material.emissiveFactor)) {
                error = "emissiveFactor must be [r,g,b] (linear).";
                return false;
            }
        } else if (key == "metallicFactor" || key == "roughnessFactor" || key == "alphaCutoff" || key == "foliageTransmission") {
            if (!value.is_number()) {
                error = key + " must be a number.";
                return false;
            }
            const float number = value.get<float>();
            if (key == "metallicFactor") material.metallicFactor = number;
            else if (key == "roughnessFactor") material.roughnessFactor = number;
            else if (key == "alphaCutoff") material.alphaCutoff = number;
            else material.foliageTransmission = std::clamp(number, 0.0f, 1.0f);
        } else if (key == "alphaMode") {
            const std::string mode = value.is_string() ? value.get<std::string>() : "";
            static const std::map<std::string, MaterialAlphaMode> modes = {
                {"AUTO", MaterialAlphaMode::AUTO}, {"OPAQUE", MaterialAlphaMode::ALPHA_OPAQUE},
                {"ALPHA_OPAQUE", MaterialAlphaMode::ALPHA_OPAQUE}, {"MASK", MaterialAlphaMode::MASK}, {"BLEND", MaterialAlphaMode::BLEND}};
            std::string upper = mode;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            auto found = modes.find(upper);
            if (found == modes.end()) {
                error = "alphaMode must be AUTO, OPAQUE, MASK or BLEND.";
                return false;
            }
            material.alphaMode = found->second;
        } else {
            handled = false;
        }
        if (handled) continue;
        for (const auto& [name, member] : materialTextures()) {
            if (key != name) continue;
            handled = true;
            if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) {
                material.*member = Texture();
            } else if (value.is_string()) {
                std::string stored;
                if (!McpValues::assetPathFromJson(project, value.get<std::string>(), stored, error)) return false;
                std::error_code ec;
                if (!fs::exists(project->getProjectPath() / value.get<std::string>(), ec)) {
                    error = key + ": " + value.get<std::string>() + " does not exist.";
                    return false;
                }
                Texture texture;
                texture.setPath(stored);
                material.*member = texture;
            } else {
                error = key + " must be a project-relative image path or null.";
                return false;
            }
        }
        for (const auto& [name, member] : materialTexCoords()) {
            if (key != name) continue;
            handled = true;
            if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 1) {
                error = key + " must be 0 or 1.";
                return false;
            }
            material.*member = value.get<int>();
        }
        if (!handled) {
            error = "Unknown material field \"" + key + "\". Fields: baseColorFactor, metallicFactor, roughnessFactor, alphaCutoff, "
                    "alphaMode, emissiveFactor, foliageTransmission, baseColorTexture, emissiveTexture, metallicRoughnessTexture, "
                    "occlusionTexture, normalTexture and the *TexCoord indices.";
            return false;
        }
    }
    return true;
}

bool loadMaterialFile(const fs::path& file, Material& material, std::string& error) {
    try {
        YAML::Node node = YAML::LoadFile(file.string());
        if (!node || !node.IsMap()) {
            error = "Not a material file (expected a YAML map).";
            return false;
        }
        material = Stream::decodeMaterial(node);
        return true;
    } catch (const std::exception& e) {
        error = std::string("Can't read the material: ") + e.what();
        return false;
    }
}

std::string materialFileContent(const Material& material) {
    return YAML::Dump(Stream::encodeMaterial(material)) + "\n";
}

// Linked submeshes of every loaded scene using the material file
Json materialUsers(Project* project, const std::string& rel) {
    Json users = Json::array();
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (!sceneProject->scene) continue;
        for (Entity entity : sceneProject->entities) {
            MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
            if (!mesh) continue;
            for (unsigned int i = 0; i < mesh->numSubmeshes; i++) {
                if (fs::path(mesh->submeshes[i].material.name).lexically_normal().generic_string() == rel) {
                    users.push_back({{"scene", sceneProject->id}, {"entity", McpRefs::entityJson(sceneProject, entity)}, {"submesh", i}});
                    if (users.size() >= 50) return users;
                }
            }
        }
    }
    return users;
}

// ------------------------------------------------------------------ list_assets

void listAssetsTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    fs::path root = project->getProjectPath();
    const std::string dirArg = args.value("directory", "");
    std::string error;
    if (!dirArg.empty() && dirArg != "." && !McpRefs::resolveProjectPath(project, dirArg, root, error, true)) {
        call->fail(error);
        return;
    }
    if (!fs::is_directory(root)) {
        call->fail(dirArg + " is not a directory.");
        return;
    }
    const std::string pattern = args.value("pattern", "");
    const std::string query = lowerCase(args.value("query", ""));
    const std::string type = args.value("type", "");
    const bool recursive = args.value("recursive", true);
    const bool details = args.value("details", false);
    const size_t limit = static_cast<size_t>(std::clamp(args.value("limit", 200), 1, 2000));

    Json entries = Json::array();
    size_t total = 0;
    std::error_code ec;
    auto consider = [&](const fs::directory_entry& entry) {
        const fs::path& path = entry.path();
        const bool isDir = entry.is_directory(ec);
        const std::string rel = projectRelative(project, path);
        const std::string kind = isDir ? "folder" : assetType(path);
        if (!type.empty() && type != "any" && kind != type) return;
        if (!query.empty() && lowerCase(path.filename().string()).find(query) == std::string::npos) return;
        if (!pattern.empty()) {
            const bool full = pattern.find('/') != std::string::npos;
            if (!globMatch(pattern, full ? rel : path.filename().string())) return;
        }
        total++;
        if (entries.size() >= limit) return;
        Json item = {{"path", rel}, {"type", kind}};
        if (!isDir && details) {
            item["size"] = entry.file_size(ec);
            if (kind == "texture" && extensionOf(path) != ".svg") {
                int w = 0, h = 0, c = 0;
                if (stbi_info(path.string().c_str(), &w, &h, &c)) {
                    item["width"] = w;
                    item["height"] = h;
                }
            }
        }
        entries.push_back(item);
    };
    if (recursive) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->is_directory(ec) && isSkippedDir(it->path())) {
                it.disable_recursion_pending();
                continue;
            }
            const std::string name = it->path().filename().string();
            if (!name.empty() && name[0] == '.') continue;
            consider(*it);
        }
    } else {
        for (const fs::directory_entry& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (entry.is_directory(ec) && isSkippedDir(entry.path())) continue;
            const std::string name = entry.path().filename().string();
            if (!name.empty() && name[0] == '.') continue;
            consider(entry);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Json& a, const Json& b) { return a["path"].get<std::string>() < b["path"].get<std::string>(); });
    Json data = {{"entries", entries}, {"total", total}, {"truncated", total > entries.size()},
                 {"assets_dir", projectRelative(project, project->getAssetsPath())},
                 {"lua_dir", projectRelative(project, project->getLuaPath())}};
    std::string text = std::to_string(total) + " match(es)" + (total > entries.size() ? " (showing " + std::to_string(entries.size()) + ", raise limit)" : "") + ":";
    for (const Json& item : entries) {
        text += "\n" + item["path"].get<std::string>() + (item["type"] == "folder" ? "/" : "");
        if (item.contains("width")) text += " (" + std::to_string(item["width"].get<int>()) + "x" + std::to_string(item["height"].get<int>()) + ")";
    }
    call->ok(text, data);
}

// ------------------------------------------------------------------ get_asset_info

void getAssetInfoTool(const CallPtr& call) {
    fs::path file;
    if (!resolveExisting(call, "path", file)) return;
    Project* project = call->ctx().project();
    const std::string rel = projectRelative(project, file);
    std::error_code ec;
    const bool isDir = fs::is_directory(file, ec);
    const std::string kind = isDir ? "folder" : assetType(file);
    Json data = {{"path", rel}, {"type", kind}};
    std::string text = rel + ": " + kind;
    if (isDir) {
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(file, ec)) {
            (void)entry;
            count++;
        }
        data["entries"] = count;
        call->ok(text + " with " + std::to_string(count) + " entries.", data);
        return;
    }
    data["size"] = fs::file_size(file, ec);
    if (kind != "script" && kind != "shader" && kind != "material" && kind != "scene" && kind != "prefab" && kind != "other") {
        data["inside_assets_dir"] = project->isInsideAssetsPath(file);
        if (!project->isInsideAssetsPath(file)) {
            data["note"] = "Outside the assets directory: components can't reference it. Move it under " +
                           projectRelative(project, project->getAssetsPath()) + ".";
        }
    }
    if (kind == "texture") {
        int w = 0, h = 0, c = 0;
        if (extensionOf(file) != ".svg" && stbi_info(file.string().c_str(), &w, &h, &c)) {
            data["width"] = w;
            data["height"] = h;
            data["channels"] = c;
            text += " " + std::to_string(w) + "x" + std::to_string(h) + ", " + std::to_string(c) + " channel(s)";
        }
        data["sampler_note"] = "Filtering/wrap are per texture property (set_texture_settings), not per file.";
    } else if (kind == "model") {
        std::string error;
        Json info = modelInfo(file, error);
        if (info.is_null()) data["error"] = error;
        else {
            data["model"] = info;
            if (info.contains("meshes")) {
                text += ", " + std::to_string(info["meshes"].size()) + " mesh(es), " + std::to_string(info["materials"].size()) +
                        " material(s), " + std::to_string(info["animations"].size()) + " animation(s)";
            }
        }
    } else if (kind == "audio") {
        if (extensionOf(file) == ".wav") {
            Json info = wavInfo(file);
            if (!info.is_null()) {
                data["audio"] = info;
                if (info.contains("duration_seconds")) text += ", " + std::to_string(info["duration_seconds"].get<double>()) + " s";
            }
        }
    } else if (kind == "material") {
        Material material;
        std::string error;
        if (loadMaterialFile(file, material, error)) {
            data["material"] = materialToJson(project, material);
            data["linked_by"] = materialUsers(project, rel);
        } else {
            data["error"] = error;
        }
    } else if (kind == "shader") {
        std::string source;
        readFile(file, source);
        data["includes"] = Util::getShaderIncludeKeys(source);
        Json forks = Json::array();
        for (const ShaderUse& use : shaderForksUsingFiles(project, {rel})) forks.push_back(use.base);
        data["forks"] = forks;
    } else if (kind == "script") {
        std::string source;
        readFile(file, source);
        data["lines"] = std::count(source.begin(), source.end(), '\n') + 1;
        if (Util::isLuaFile(rel)) {
            LuaSyntax syntax = checkLuaSyntax(source, rel);
            data["syntax_ok"] = syntax.ok;
            if (!syntax.ok) data["syntax_error"] = {{"line", syntax.line}, {"message", syntax.message}};
        }
    } else if (kind == "scene") {
        const uint32_t id = project->findSceneByPath(file);
        data["in_project"] = id != NULL_PROJECT_SCENE;
        if (const SceneProject* scene = project->getScene(id)) data["scene"] = McpRefs::sceneJson(project, *scene);
    }
    if (CodeEditor* editor = call->ctx().codeEditor()) {
        if (editor->isFileOpen(file.string())) data["open_in_code_editor"] = {{"unsaved", editor->isFileModified(file.string())}};
    }
    call->ok(text + ".", data);
}

// ------------------------------------------------------------------ view_asset

void replyImage(const CallPtr& call, const fs::path& image, const std::string& label, int maxSize) {
    int w = 0, h = 0, c = 0;
    unsigned char* pixels = stbi_load(image.string().c_str(), &w, &h, &c, 4);
    if (!pixels) {
        call->fail("Can't decode " + label + ": " + std::string(stbi_failure_reason() ? stbi_failure_reason() : "unknown format") + ".");
        return;
    }
    std::string base64;
    int outW = 0, outH = 0;
    const bool encoded = encodePng(pixels, w, h, maxSize, base64, outW, outH);
    stbi_image_free(pixels);
    if (!encoded) {
        call->fail("Can't encode the preview of " + label + ".");
        return;
    }
    ToolResult result = ToolResult::success(label + " (" + std::to_string(w) + "x" + std::to_string(h) +
                                                (outW != w ? ", shown at " + std::to_string(outW) + "x" + std::to_string(outH) : std::string()) + ").",
                                            Json{{"width", w}, {"height", h}, {"preview_width", outW}, {"preview_height", outH}});
    result.images.push_back({base64, "image/png"});
    call->reply(std::move(result));
}

void viewAssetTool(const CallPtr& call) {
    fs::path file;
    if (!resolveExisting(call, "path", file)) return;
    Project* project = call->ctx().project();
    const std::string rel = projectRelative(project, file);
    const int maxSize = std::clamp(call->args().value("max_size", 512), 16, 2048);
    if (Util::isImageFile(rel) && extensionOf(file) != ".svg") {
        replyImage(call, file, rel, maxSize);
        return;
    }
    if (!(Util::isImageFile(rel) || Util::isModelFile(rel) || Util::isMaterialFile(rel) || Util::isPrefabFile(rel))) {
        call->fail("view_asset shows images, and thumbnails of models, materials and prefabs. For scenes use the screenshot tools.");
        return;
    }
    // The Project window's thumbnail, generated on demand
    std::error_code ec;
    const fs::path thumbnail = project->getThumbnailPath(file);
    if (fs::exists(thumbnail, ec)) {
        replyImage(call, thumbnail, rel + " thumbnail", maxSize);
        return;
    }
    ProjectWindow* window = call->ctx().projectWindow();
    if (!window) {
        call->fail("No thumbnail for " + rel + " and the Project window is unavailable.");
        return;
    }
    window->requestThumbnailGeneration(file);
    call->waitUntil([thumbnail]() {
        std::error_code e;
        return fs::exists(thumbnail, e) && fs::file_size(thumbnail, e) > 0;
    }, 15000, [call, thumbnail, rel, maxSize](bool timedOut) {
        if (timedOut) {
            call->fail("The thumbnail of " + rel + " was not ready in 15 s.");
            return;
        }
        // the writer may still be finishing the file
        call->afterFrames(2, [call, thumbnail, rel, maxSize]() { replyImage(call, thumbnail, rel + " thumbnail", maxSize); });
    });
}

// ------------------------------------------------------------------ read_file / write_file

void readFileTool(const CallPtr& call) {
    fs::path file;
    if (!resolveExisting(call, "path", file)) return;
    Project* project = call->ctx().project();
    const std::string rel = projectRelative(project, file);
    std::error_code ec;
    if (fs::is_directory(file, ec)) {
        call->fail(rel + " is a directory; list it with list_assets.");
        return;
    }
    if (!isTextFile(file)) {
        call->fail(rel + " is not a text file; use get_asset_info (or view_asset for images).");
        return;
    }
    std::string content;
    if (!readFile(file, content)) {
        call->fail("Can't read " + rel + ".");
        return;
    }
    const Json& args = call->args();
    const int start = std::max(1, args.value("start_line", 1));
    const int maxLines = std::clamp(args.value("max_lines", 2000), 1, 20000);
    std::istringstream in(content);
    std::string line, out;
    int number = 0, emitted = 0, total = 0;
    while (std::getline(in, line)) {
        number++;
        if (number < start || emitted >= maxLines) continue;
        out += line + "\n";
        emitted++;
    }
    total = number;
    if (out.size() > 400 * 1024) {
        out = out.substr(0, 400 * 1024) + "\n... (truncated: read a smaller line range)";
    }
    Json data = {{"path", rel}, {"total_lines", total}, {"start_line", start}, {"lines", emitted}, {"content", out}};
    if (CodeEditor* editor = call->ctx().codeEditor()) {
        if (editor->isFileModified(file.string())) {
            data["warning"] = "The code editor has unsaved edits of this file; this is the version on disk.";
        }
    }
    // The text is the content itself (agents read it), the rest is in structured
    std::string header = rel + " lines " + std::to_string(start) + "-" + std::to_string(start + emitted - 1) + " of " +
                         std::to_string(total) + ":\n";
    ToolResult result = ToolResult::success(header + out, Json{{"path", rel}, {"total_lines", total}, {"start_line", start}, {"lines", emitted}});
    if (data.contains("warning")) result.structured["warning"] = data["warning"];
    call->reply(std::move(result));
}

bool applyEdits(const Json& edits, std::string& content, std::string& error) {
    for (size_t i = 0; i < edits.size(); i++) {
        const Json& edit = edits[i];
        if (!edit.is_object() || !edit.contains("old_text") || !edit["old_text"].is_string() || !edit.contains("new_text") || !edit["new_text"].is_string()) {
            error = "edits[" + std::to_string(i) + "] must be {old_text, new_text}.";
            return false;
        }
        const std::string oldText = edit["old_text"].get<std::string>();
        const std::string newText = edit["new_text"].get<std::string>();
        if (oldText.empty()) {
            error = "edits[" + std::to_string(i) + "].old_text is empty.";
            return false;
        }
        const bool all = edit.value("replace_all", false);
        size_t count = 0;
        for (size_t pos = content.find(oldText); pos != std::string::npos; pos = content.find(oldText, pos + oldText.size())) count++;
        if (count == 0) {
            error = "edits[" + std::to_string(i) + "].old_text was not found (it must match the file exactly, whitespace included).";
            return false;
        }
        if (count > 1 && !all) {
            error = "edits[" + std::to_string(i) + "].old_text appears " + std::to_string(count) + " times: add context to make it unique, or pass replace_all.";
            return false;
        }
        size_t pos = 0;
        while ((pos = content.find(oldText, pos)) != std::string::npos) {
            content.replace(pos, oldText.size(), newText);
            pos += newText.size();
            if (!all) break;
        }
    }
    return true;
}

void writeFileTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    fs::path file;
    if (!resolveExisting(call, "path", file, false)) return;
    const std::string rel = projectRelative(project, file);
    std::string why;
    if (isEditorOwnedFile(project, file, why)) {
        call->fail(why);
        return;
    }
    if (!isTextFile(file)) {
        call->fail(rel + ": write_file writes text files (scripts, shaders, materials, configs). Import binary assets with import_files.");
        return;
    }
    const bool hasContent = args.contains("content") && args["content"].is_string();
    const bool hasEdits = args.contains("edits") && args["edits"].is_array();
    if (hasContent == hasEdits) {
        call->fail("Pass either content (the whole file) or edits ([{old_text, new_text}]).");
        return;
    }
    std::error_code ec;
    const bool exists = fs::is_regular_file(file, ec);
    std::string content;
    if (hasEdits) {
        if (!exists) {
            call->fail(rel + " does not exist; create it with content.");
            return;
        }
        readFile(file, content);
        std::string error;
        if (!applyEdits(args["edits"], content, error)) {
            call->fail(error);
            return;
        }
    } else {
        content = args["content"].get<std::string>();
    }
    if (content.size() > 4 * 1024 * 1024) {
        call->fail("Content is larger than 4 MB.");
        return;
    }
    CodeEditor* editor = call->ctx().codeEditor();
    if (editor && editor->isFileModified(file.string())) {
        if (!args.value("discard_unsaved_buffer", false)) {
            call->fail(rel + " is open in the code editor with unsaved edits (someone is editing it). Ask, or pass "
                       "discard_unsaved_buffer: true to overwrite them.");
            return;
        }
    }
    // Materials must stay loadable: linked meshes read them
    if (Util::isMaterialFile(rel)) {
        try {
            YAML::Node node = YAML::Load(content);
            if (!node || !node.IsMap()) throw std::runtime_error("expected a YAML map");
            Stream::decodeMaterial(node);
        } catch (const std::exception& e) {
            call->fail(std::string("Not a valid material: ") + e.what() + ". Prefer update_material.");
            return;
        }
    }
    std::string error;
    if (!writeFilesUndoable(call->ctx(), {{file, content}}, error)) {
        call->fail(error);
        return;
    }
    if (editor && editor->isFileModified(file.string())) editor->reloadFile(file.string());

    Json data = {{"path", rel}, {"bytes", content.size()}, {"created", !exists},
                 {"lines", std::count(content.begin(), content.end(), '\n') + (content.empty() || content.back() == '\n' ? 0 : 1)}};
    std::string text = (exists ? "Wrote " : "Created ") + rel + " (" + std::to_string(content.size()) + " bytes; undo with scope project).";
    if (Util::isLuaFile(rel)) {
        LuaSyntax syntax = checkLuaSyntax(content, rel);
        data["syntax_ok"] = syntax.ok;
        if (!syntax.ok) {
            data["errors"] = Json::array({{{"line", syntax.line}, {"message", syntax.message}}});
            text += " LUA SYNTAX ERROR line " + std::to_string(syntax.line) + ": " + syntax.message;
        }
    } else if (Util::isScriptFile(rel)) {
        text += " C++: run compile_scripts to check it.";
    }
    if (Util::isShaderFile(rel)) {
        std::vector<ShaderUse> forks = shaderForksUsingFiles(project, {rel});
        if (!forks.empty() && args.value("check", true)) {
            checkShadersAsync(call, forks, [call, data, text](Json results) mutable {
                bool ok = true;
                for (const Json& r : results) {
                    if (!r.value("ok", false)) ok = false;
                }
                data["shader_check"] = results;
                if (!ok) {
                    text += " SHADER COMPILE ERRORS:";
                    for (const Json& r : results) {
                        if (r.value("ok", false)) continue;
                        text += "\n" + r.value("shader", std::string()) + ": ";
                        if (r.contains("errors") && !r["errors"].empty()) {
                            for (const Json& e : r["errors"]) text += "\n  line " + std::to_string(e["line"].get<int>()) + ": " + e["message"].get<std::string>();
                        } else {
                            text += r.value("log", std::string());
                        }
                    }
                } else {
                    text += " Shader compiles.";
                }
                call->ok(text, data);
            });
            return;
        }
    }
    call->ok(text, data);
}

// ------------------------------------------------------------------ folders / move / copy / delete / import

void createFolderTool(const CallPtr& call) {
    fs::path dir;
    if (!resolveExisting(call, "path", dir, false)) return;
    Project* project = call->ctx().project();
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        call->fail(projectRelative(project, dir) + " already exists.");
        return;
    }
    if (!fs::is_directory(dir.parent_path(), ec)) {
        call->fail("The parent folder " + projectRelative(project, dir.parent_path()) + " does not exist; create it first.");
        return;
    }
    if (!addProjectCommand(call->ctx(), new CreateDirCmd(dir.filename().string(), dir.parent_path().string()))) {
        call->fail("Couldn't create the folder.");
        return;
    }
    call->ok("Created folder " + projectRelative(project, dir) + ".", Json{{"path", projectRelative(project, dir)}});
}

// Copies a file or folder to an exact destination; undo deletes the copy
class CopyAsCmd : public Command {
public:
    CopyAsCmd(Project* project, fs::path from, fs::path to) : project(project), from(std::move(from)), to(std::move(to)) {}
    bool execute() override {
        std::error_code ec;
        if (fs::is_directory(from, ec)) fs::copy(from, to, fs::copy_options::recursive, ec);
        else fs::copy_file(from, to, ec);
        if (ec) return false;
        if (fs::is_directory(to, ec) || Util::isPrefabFile(to.string())) project->importPrefabFiles(to, true, &copyGuids);
        return true;
    }
    void undo() override {
        std::error_code ec;
        fs::remove_all(to, ec);
        project->cleanupPrefabFilePath(to);
    }
    bool affectsStructure() const override { return false; }

private:
    Project* project;
    fs::path from;
    fs::path to;
    std::map<std::string, std::string> copyGuids;
};

void moveAssetTool(const CallPtr& call) {
    fs::path from, to;
    if (!resolveExisting(call, "path", from)) return;
    if (!resolveExisting(call, "to", to, false)) return;
    Project* project = call->ctx().project();
    const std::string fromRel = projectRelative(project, from), toRel = projectRelative(project, to);
    std::string why;
    if (fromRel.empty() || fromRel == "project.yaml" || fromRel[0] == '.') {
        call->fail("Can't move " + (fromRel.empty() ? std::string("the project root") : fromRel) + ".");
        return;
    }
    std::error_code ec;
    if (fs::exists(to, ec)) {
        call->fail(toRel + " already exists; delete it first or pick another name.");
        return;
    }
    if (!fs::is_directory(to.parent_path(), ec)) {
        call->fail("The destination folder " + projectRelative(project, to.parent_path()) + " does not exist; create it with create_folder.");
        return;
    }
    if (Util::isInsidePath(to, from)) {
        call->fail("Can't move a folder into itself.");
        return;
    }
    const bool copy = call->args().value("copy", false);
    Command* cmd = nullptr;
    if (copy) {
        cmd = new CopyAsCmd(project, from, to);
    } else if (from.parent_path() == to.parent_path()) {
        cmd = new RenameFileCmd(project, from.filename().string(), to.filename().string(), from.parent_path().string());
    } else {
        auto* multi = new MultiPropertyCmd();
        multi->addCommand(std::make_unique<CopyFileCmd>(project, std::vector<std::string>{from.string()}, to.parent_path().string(), false));
        if (from.filename() != to.filename()) {
            multi->addCommand(std::make_unique<RenameFileCmd>(project, from.filename().string(), to.filename().string(), to.parent_path().string()));
        }
        cmd = multi;
    }
    if (!addProjectCommand(call->ctx(), cmd)) {
        call->fail("Couldn't " + std::string(copy ? "copy " : "move ") + fromRel + " to " + toRel + ".");
        return;
    }
    if (CodeEditor* editor = call->ctx().codeEditor(); editor && !copy) editor->handleFileRename(from, to);
    syncFiles(call->ctx(), {from, to}, false);
    call->ok(std::string(copy ? "Copied " : "Moved ") + fromRel + " to " + toRel +
             (copy ? "." : "; references in scenes, prefabs and materials follow it. Undo with scope project."),
             Json{{"from", fromRel}, {"to", toRel}, {"copy", copy}});
}

void deleteAssetTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    if (!args.contains("paths") || !args["paths"].is_array() || args["paths"].empty()) {
        call->fail("paths must be a non-empty array of project-relative paths.");
        return;
    }
    std::vector<fs::path> files;
    Json deleted = Json::array();
    for (const Json& item : args["paths"]) {
        fs::path file;
        std::string error;
        if (!item.is_string() || !McpRefs::resolveProjectPath(project, item.get<std::string>(), file, error, true)) {
            call->fail(error.empty() ? "paths must be strings." : error);
            return;
        }
        const std::string rel = projectRelative(project, file.lexically_normal());
        if (rel.empty() || rel == "project.yaml" || rel[0] == '.') {
            call->fail("Refusing to delete " + (rel.empty() ? std::string("the project root") : rel) + ".");
            return;
        }
        for (SceneProject* scene : project->getScenesAndPrefabStages()) {
            if (!scene->filepath.empty() && projectRelative(project, scene->filepath) == rel && scene->scene) {
                call->fail(rel + " is open in the editor; close it first (or delete the scene with the scene tools).");
                return;
            }
        }
        files.push_back(file.lexically_normal());
        deleted.push_back(rel);
    }
    if (!addProjectCommand(call->ctx(), new DeleteFileCmd(project, files, project->getProjectPath()))) {
        call->fail("Couldn't delete the files.");
        return;
    }
    syncFiles(call->ctx(), files, false);
    call->ok("Moved " + std::to_string(files.size()) + " item(s) to the project trash (undo with scope project restores them).",
             Json{{"deleted", deleted}});
}

void importFilesTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    if (!args.contains("sources") || !args["sources"].is_array() || args["sources"].empty()) {
        call->fail("sources must be a non-empty array of absolute file or folder paths.");
        return;
    }
    fs::path target;
    if (!resolveExisting(call, "to_dir", target, false)) return;
    std::error_code ec;
    std::vector<std::string> sources;
    Json imported = Json::array();
    for (const Json& item : args["sources"]) {
        if (!item.is_string()) continue;
        const fs::path source = fs::path(item.get<std::string>()).lexically_normal();
        if (!source.is_absolute() || !fs::exists(source, ec)) {
            call->fail(item.get<std::string>() + " must be an existing absolute path.");
            return;
        }
        if (Util::isInsidePath(source, project->getProjectPath())) {
            call->fail(item.get<std::string>() + " is already in the project; use move_asset (copy: true) instead.");
            return;
        }
        const fs::path dest = target / source.filename();
        if (fs::exists(dest, ec) && !args.value("overwrite", false)) {
            call->fail(projectRelative(project, dest) + " already exists; pass overwrite: true to replace it.");
            return;
        }
        sources.push_back(source.string());
        imported.push_back(projectRelative(project, dest));
    }
    if (!fs::exists(target, ec)) {
        fs::create_directories(target, ec);
        if (ec) {
            call->fail("Can't create " + projectRelative(project, target) + ": " + ec.message());
            return;
        }
    }
    if (!addProjectCommand(call->ctx(), new CopyFileCmd(project, sources, target.string(), true))) {
        call->fail("Copying the files failed.");
        return;
    }
    std::vector<fs::path> changed;
    for (const Json& rel : imported) changed.push_back(project->getProjectPath() / rel.get<std::string>());
    syncFiles(call->ctx(), changed, false);
    Json data = {{"imported", imported}};
    std::string text = "Imported " + std::to_string(imported.size()) + " item(s) into " + projectRelative(project, target) + ".";
    if (!project->isInsideAssetsPath(target)) {
        text += " Note: outside the assets directory (" + projectRelative(project, project->getAssetsPath()) + "), components can't reference them.";
    }
    call->ok(text, data);
}

// ------------------------------------------------------------------ materials

void createMaterialTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    fs::path file;
    if (!resolveExisting(call, "path", file, false)) return;
    if (extensionOf(file) != ".material") file += ".material";
    const std::string rel = projectRelative(project, file);
    std::error_code ec;
    if (fs::exists(file, ec) && !args.value("overwrite", false)) {
        call->fail(rel + " already exists (edit it with update_material, or pass overwrite).");
        return;
    }
    Material material;
    SceneProject* sceneProject = nullptr;
    Entity entity = NULL_ENTITY;
    unsigned int submesh = static_cast<unsigned int>(std::max(0, args.value("submesh", 0)));
    std::string error;
    if (args.contains("entity") && !args["entity"].is_null()) {
        sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!sceneProject) {
            call->fail(error);
            return;
        }
        entity = McpRefs::resolveEntity(sceneProject, args["entity"], error);
        if (entity == NULL_ENTITY) {
            call->fail(error);
            return;
        }
        MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
        if (!mesh || submesh >= mesh->numSubmeshes) {
            call->fail(mesh ? "submesh must be 0.." + std::to_string(mesh->numSubmeshes - 1) + "."
                            : "The GameObject has no Mesh component.");
            return;
        }
        material = mesh->submeshes[submesh].material;
    }
    if (args.contains("values") && args["values"].is_object() && !applyMaterialJson(project, material, args["values"], error)) {
        call->fail(error);
        return;
    }
    material.name = rel;
    if (!writeFilesUndoable(call->ctx(), {{file, materialFileContent(material)}}, error)) {
        call->fail(error);
        return;
    }
    Json data = {{"path", rel}, {"material", materialToJson(project, material)}};
    std::string text = "Created " + rel + ".";
    const bool link = args.value("link", entity != NULL_ENTITY);
    if (link && entity != NULL_ENTITY) {
        const std::string property = "submeshes[" + std::to_string(submesh) + "].material";
        if (!addSceneCommand(sceneProject->id, new LinkMaterialCmd(project, sceneProject->id, entity, ComponentType::MeshComponent,
                                                                   property, submesh, material))) {
            call->fail(text + " Linking it failed.", data);
            return;
        }
        data["linked"] = {{"entity", McpRefs::entityJson(sceneProject, entity)}, {"submesh", submesh}};
        text += " Linked to submesh " + std::to_string(submesh) + " of " + McpRefs::entityPath(sceneProject, entity) + ".";
    }
    call->ok(text, data);
}

void updateMaterialTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    fs::path file;
    if (!resolveExisting(call, "path", file)) return;
    const std::string rel = projectRelative(project, file);
    if (extensionOf(file) != ".material") {
        call->fail(rel + " is not a .material file.");
        return;
    }
    if (!args.contains("values") || !args["values"].is_object() || args["values"].empty()) {
        call->fail("values must be an object of material fields, e.g. {\"baseColorFactor\":[1,0,0,1],\"roughnessFactor\":0.4}.");
        return;
    }
    Material material;
    std::string error;
    if (!loadMaterialFile(file, material, error) || !applyMaterialJson(project, material, args["values"], error)) {
        call->fail(error);
        return;
    }
    material.name = rel;
    if (!writeFilesUndoable(call->ctx(), {{file, materialFileContent(material)}}, error)) {
        call->fail(error);
        return;
    }
    Json users = materialUsers(project, rel);
    call->ok("Updated " + rel + "; " + std::to_string(users.size()) + " linked submesh(es) follow it. Undo with scope project.",
             Json{{"path", rel}, {"material", materialToJson(project, material)}, {"linked_by", users}});
}

void linkMaterialTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sceneProject) {
        call->fail(error);
        return;
    }
    Entity entity = McpRefs::resolveEntity(sceneProject, args.contains("entity") ? args["entity"] : Json(), error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
    if (!mesh || mesh->numSubmeshes == 0) {
        call->fail("The GameObject has no Mesh component with submeshes (sprites and UI use their own texture properties).");
        return;
    }
    const unsigned int submesh = static_cast<unsigned int>(std::max(0, args.value("submesh", 0)));
    if (submesh >= mesh->numSubmeshes) {
        call->fail("submesh must be 0.." + std::to_string(mesh->numSubmeshes - 1) + ".");
        return;
    }
    const std::string property = "submeshes[" + std::to_string(submesh) + "].material";
    const bool unlink = !args.contains("material") || args["material"].is_null() ||
                        (args["material"].is_string() && args["material"].get<std::string>().empty());
    if (unlink) {
        if (!project->isMaterialFileLinked(sceneProject->id, entity, submesh)) {
            call->fail("Submesh " + std::to_string(submesh) + " is not linked to a material file.");
            return;
        }
        if (!addSceneCommand(sceneProject->id, new UnlinkMaterialCmd(project, sceneProject->id, ComponentType::MeshComponent, property,
                                                                     submesh, std::vector<Entity>{entity}))) {
            call->fail("Unlinking failed.");
            return;
        }
        call->ok("Unlinked submesh " + std::to_string(submesh) + " of " + McpRefs::entityPath(sceneProject, entity) +
                 " (it keeps a copy of the material values).", Json{{"entity", McpRefs::entityJson(sceneProject, entity)}, {"submesh", submesh}});
        return;
    }
    fs::path file;
    if (!McpRefs::resolveProjectPath(project, args["material"].get<std::string>(), file, error, true)) {
        call->fail(error);
        return;
    }
    Material material;
    if (!loadMaterialFile(file, material, error)) {
        call->fail(error);
        return;
    }
    const std::string rel = projectRelative(project, file.lexically_normal());
    material.name = rel;
    if (!addSceneCommand(sceneProject->id, new LinkMaterialCmd(project, sceneProject->id, entity, ComponentType::MeshComponent,
                                                               property, submesh, material))) {
        call->fail("Linking failed.");
        return;
    }
    call->ok("Linked " + rel + " to submesh " + std::to_string(submesh) + " of " + McpRefs::entityPath(sceneProject, entity) + ".",
             Json{{"entity", McpRefs::entityJson(sceneProject, entity)}, {"submesh", submesh}, {"material", rel}});
}

// ------------------------------------------------------------------ shaders

void forkShaderTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string target = args.value("target", args.contains("entity") ? "component" : "");
    Json legacyArgs = Json::object();
    SceneProject* sceneProject = nullptr;
    Entity entity = NULL_ENTITY;
    if (!legacyTarget(call, legacyArgs, sceneProject, entity, target == "component" || target == "depth")) return;
    if (args.contains("directory")) legacyArgs["directory"] = args["directory"];
    if (args.contains("fork_includes")) legacyArgs["fork_includes"] = args["fork_includes"];

    auto finish = [call, project](const std::string& base, Json data, const std::string& text) {
        data["vert_path"] = Util::resolveCustomShaderPaths(base).vert;
        data["frag_path"] = Util::resolveCustomShaderPaths(base).frag;
        call->ok(text + " Read the files, change only the shading math, and edit them with write_file (it compiles the fork "
                        "and reports errors) or on disk + sync_files + check_shader.", data);
        (void)project;
    };

    if (target == "component" || target == "depth" || target == "scene_default") {
        if (target == "depth") legacyArgs["shader_type"] = "depth";
        if (target == "scene_default") {
            const std::string type = args.value("shader_type", "");
            if (type.empty()) {
                call->fail("target scene_default needs shader_type: mesh, ui, sky, points or lines.");
                return;
            }
            legacyArgs["shader_type"] = type;
            legacyArgs.erase("entity_id");
        }
        legacy::ActionResult action = call->ctx().legacyExecutor().execute("fork_shader", legacyArgs);
        if (!action.success) {
            call->fail(action.message, legacyToResult(action));
            return;
        }
        Json data = legacyToResult(action);
        const std::string base = data.value("custom_shader", std::string());
        finish(base, data, "Forked the shader to " + base + " (.vert/.frag) and pointed " +
                               (target == "scene_default" ? data.value("scene_property", std::string("the scene default")) : data.value("property", std::string("customShader"))) +
                               " at it (one undo step).");
        return;
    }
    if (target == "post_process") {
        std::vector<PostProcessPass> passes = Catalog::getSceneProperty<std::vector<PostProcessPass>>(sceneProject->scene, "post_process");
        long long index = args.contains("pass_index") && args["pass_index"].is_number_integer() ? args["pass_index"].get<long long>() : -1;
        if (index >= static_cast<long long>(passes.size())) {
            call->fail("pass_index must be 0.." + std::to_string(static_cast<long long>(passes.size()) - 1) + ", or omitted to add a new pass.");
            return;
        }
        if (index >= 0 && !passes[index].shader.empty()) {
            call->fail("Pass " + std::to_string(index) + " already uses " + passes[index].shader + "; edit that fork.");
            return;
        }
        const bool append = index < 0;
        if (append) {
            passes.push_back(PostProcessPass());
            index = static_cast<long long>(passes.size()) - 1;
        }
        const fs::path directory = fs::path(args.value("directory", ProjectUtils::defaultShaderForkDir().generic_string())).lexically_normal();
        if (!Util::isSafeRelativePath(directory) && directory != ".") {
            call->fail("directory must be project-relative.");
            return;
        }
        const std::string desired = args.value("name", sceneProject->name + " Post " + std::to_string(index + 1));
        const std::string name = ProjectUtils::makeUniqueShaderName(project->getProjectPath() / directory, ShaderType::POSTPROCESS, desired);
        const uint32_t sceneId = sceneProject->id;
        auto makePropertyCmd = [project, sceneId, index, passes](const std::string& base) mutable {
            passes[static_cast<size_t>(index)].shader = base;
            return std::make_unique<ScenePropertyCmd<std::vector<PostProcessPass>>>(project, sceneId, "post_process", passes);
        };
        auto* fork = new ForkShaderCmd(project, ShaderType::POSTPROCESS, makePropertyCmd, directory, name, args.value("fork_includes", false));
        if (!fork->isValid()) {
            const std::string error = fork->getError();
            delete fork;
            call->fail(error.empty() ? "Can't plan the fork." : error);
            return;
        }
        const std::string base = fork->getBase();
        if (!addSceneCommand(sceneId, fork)) {
            call->fail("The fork failed (files not written).");
            return;
        }
        if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
        finish(base, Json{{"custom_shader", base}, {"pass_index", index}, {"new_pass", append}},
               "Forked the post-process shader to " + base + " for pass " + std::to_string(index) + (append ? " (new pass)" : "") +
                   ". Its parameters are a u_fs_postParams block, set with set_shader_uniform (pass_index).");
        return;
    }
    call->fail("target must be component (a GameObject's renderable component), depth (a Mesh's depth/shadow pass), "
               "scene_default (shader_type for the whole scene) or post_process (a scene post-process pass).");
}

void inspectShaderUniformsTool(const CallPtr& call) {
    Json legacyArgs = Json::object();
    SceneProject* sceneProject = nullptr;
    Entity entity = NULL_ENTITY;
    const Json& args = call->args();
    const bool pass = args.contains("pass_index");
    if (!legacyTarget(call, legacyArgs, sceneProject, entity, !pass)) return;
    if (pass) {
        legacyArgs.erase("entity_id");
        legacyArgs["pass_index"] = args["pass_index"];
    }
    replyLegacy(call, call->ctx().legacyExecutor().execute("inspect_shader_uniforms", legacyArgs));
}

void setShaderUniformTool(const CallPtr& call) {
    Json legacyArgs = Json::object();
    SceneProject* sceneProject = nullptr;
    Entity entity = NULL_ENTITY;
    const Json& args = call->args();
    const bool pass = args.contains("pass_index");
    if (!legacyTarget(call, legacyArgs, sceneProject, entity, !pass)) return;
    if (pass) {
        legacyArgs.erase("entity_id");
        legacyArgs["pass_index"] = args["pass_index"];
    }
    legacyArgs["name"] = args.value("name", "");
    if (args.value("remove", false)) {
        legacyArgs["remove"] = true;
    } else if (args.contains("value")) {
        const Json& value = args["value"];
        if (value.is_number()) legacyArgs["number_value"] = value;
        else if (value.is_array() && value.size() >= 2 && value.size() <= 4) {
            const char* keys[] = {"x", "y", "z", "w"};
            Json v = Json::object();
            for (size_t i = 0; i < value.size(); i++) v[keys[i]] = value[i];
            legacyArgs[value.size() == 2 ? "vector2_value" : value.size() == 3 ? "vector3_value" : "vector4_value"] = v;
        } else {
            call->fail("value must be a number or [x,y], [x,y,z], [x,y,z,w].");
            return;
        }
    } else {
        call->fail("Pass value (number or vector) or remove: true.");
        return;
    }
    replyLegacy(call, call->ctx().legacyExecutor().execute("set_shader_uniform", legacyArgs));
}

void checkShaderTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::vector<ShaderUse> forks;
    if (args.contains("shader") && args["shader"].is_string()) {
        std::string base = args["shader"].get<std::string>();
        for (const char* ext : {".vert", ".frag"}) {
            if (base.size() > 5 && base.compare(base.size() - 5, 5, ext) == 0 && base.find('|') == std::string::npos) base = base.substr(0, base.size() - 5);
        }
        Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(base);
        std::error_code ec;
        if (!fs::exists(project->getProjectPath() / paths.vert, ec) || !fs::exists(project->getProjectPath() / paths.frag, ec)) {
            call->fail("Shader files not found: " + paths.vert + ", " + paths.frag + ".");
            return;
        }
        int type = -1;
        static const std::map<std::string, ShaderType> types = {
            {"mesh", ShaderType::MESH}, {"ui", ShaderType::UI}, {"sky", ShaderType::SKYBOX}, {"points", ShaderType::POINTS},
            {"lines", ShaderType::LINES}, {"depth", ShaderType::DEPTH}, {"post_process", ShaderType::POSTPROCESS}};
        if (args.contains("shader_type")) {
            auto it = types.find(args.value("shader_type", ""));
            if (it == types.end()) {
                call->fail("shader_type must be mesh, ui, sky, points, lines, depth or post_process.");
                return;
            }
            type = static_cast<int>(it->second);
        } else {
            for (const ShaderUse& use : shaderForksUsingFiles(project, {paths.vert})) {
                if (use.base == base) type = use.shaderType;
            }
        }
        forks.push_back({base, type});
    } else {
        // every fork in use
        forks = shaderForksUsingFiles(project, {"x.glsl"});
        if (forks.empty()) {
            call->ok("No custom shader is used by the loaded scenes.", Json{{"results", Json::array()}});
            return;
        }
    }
    checkShadersAsync(call, forks, [call](Json results) {
        size_t failed = 0;
        std::string text;
        for (const Json& r : results) {
            if (r.value("ok", false)) {
                text += "\n" + r.value("shader", std::string()) + " (" + r.value("type", std::string()) + "): OK";
                continue;
            }
            failed++;
            text += "\n" + r.value("shader", std::string()) + " (" + r.value("type", std::string()) + "): FAILED";
            if (r.contains("errors") && !r["errors"].empty()) {
                for (const Json& e : r["errors"]) text += "\n  line " + std::to_string(e["line"].get<int>()) + ": " + e["message"].get<std::string>();
            } else {
                text += "\n" + r.value("log", std::string());
            }
        }
        call->ok((failed ? std::to_string(failed) + " shader(s) failed to compile:" : std::string("All shaders compile:")) + text,
                 Json{{"results", results}, {"failed", failed}});
    });
}

void setTextureSettingsTool(const CallPtr& call) {
    Json legacyArgs = Json::object();
    SceneProject* sceneProject = nullptr;
    Entity entity = NULL_ENTITY;
    if (!legacyTarget(call, legacyArgs, sceneProject, entity, true)) return;
    const Json& args = call->args();
    if (!legacyArgs.contains("component")) {
        call->fail("component is required (e.g. sprite, mesh, image).");
        return;
    }
    for (const char* key : {"property", "min_filter", "mag_filter", "wrap_u", "wrap_v", "svg_scale"}) {
        if (args.contains(key)) legacyArgs[key] = args[key];
    }
    replyLegacy(call, call->ctx().legacyExecutor().execute("set_texture_settings", legacyArgs),
                Json{{"entity", McpRefs::entityJson(sceneProject, entity)}});
}

// ------------------------------------------------------------------ generate_placeholder_art

struct Rgba {
    float r = 0, g = 0, b = 0, a = 0;
};

void placeholderArtTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    fs::path file;
    if (!resolveExisting(call, "path", file, false)) return;
    if (extensionOf(file) != ".png") file += ".png";
    const std::string rel = projectRelative(project, file);
    std::error_code ec;
    if (fs::exists(file, ec) && !args.value("overwrite", false)) {
        call->fail(rel + " already exists; pass overwrite: true to replace it.");
        return;
    }
    const int width = std::clamp(args.value("width", 64), 1, 2048);
    const int height = std::clamp(args.value("height", 64), 1, 2048);
    const std::string shape = args.value("shape", "rect");
    std::array<float, 4> fill{1, 1, 1, 1}, second{0, 0, 0, 1}, outline{0, 0, 0, 1}, background{0, 0, 0, 0};
    std::string error;
    if ((args.contains("color") && !color01(args["color"], fill, error)) ||
        (args.contains("color2") && !color01(args["color2"], second, error)) ||
        (args.contains("outline_color") && !color01(args["outline_color"], outline, error)) ||
        (args.contains("background") && !color01(args["background"], background, error))) {
        call->fail(error);
        return;
    }
    static const std::set<std::string> shapes = {"rect", "rounded_rect", "circle", "ellipse", "triangle", "diamond", "gradient", "checker"};
    if (!shapes.count(shape)) {
        call->fail("shape must be rect, rounded_rect, circle, ellipse, triangle, diamond, gradient or checker.");
        return;
    }
    const float outlineWidth = std::max(0.0f, args.value("outline_width", 0.0f));
    const float radius = std::max(0.0f, args.value("corner_radius", std::min(width, height) * 0.2f));
    const bool vertical = args.value("direction", "vertical") == "vertical";
    const int cells = std::max(1, args.value("cells", 8));
    const float w = static_cast<float>(width), h = static_cast<float>(height);

    // inside test + distance to the edge (for the outline), image coordinates (y down)
    auto sample = [&](float x, float y, float& edge) -> bool {
        const float cx = w * 0.5f, cy = h * 0.5f;
        if (shape == "rect" || shape == "gradient" || shape == "checker") {
            edge = std::min(std::min(x, w - x), std::min(y, h - y));
            return true;
        }
        if (shape == "rounded_rect") {
            const float r = std::min(radius, std::min(cx, cy));
            const float qx = std::abs(x - cx) - (cx - r), qy = std::abs(y - cy) - (cy - r);
            const float outside = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f)) +
                                  std::min(std::max(qx, qy), 0.0f) - r;
            edge = -outside;
            return outside <= 0.0f;
        }
        if (shape == "circle" || shape == "ellipse") {
            const float rx = shape == "circle" ? std::min(cx, cy) : cx, ry = shape == "circle" ? std::min(cx, cy) : cy;
            const float dx = (x - cx) / rx, dy = (y - cy) / ry;
            const float d = std::sqrt(dx * dx + dy * dy);
            edge = (1.0f - d) * std::min(rx, ry);
            return d <= 1.0f;
        }
        if (shape == "diamond") {
            const float d = std::abs(x - cx) / cx + std::abs(y - cy) / cy;
            edge = (1.0f - d) * std::min(cx, cy) * 0.7071f;
            return d <= 1.0f;
        }
        // triangle pointing up
        const float ax = cx, ay = 0.0f, bx = 0.0f, by = h, qx = w, qy = h;
        auto side = [](float px, float py, float x1, float y1, float x2, float y2) {
            const float len = std::sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
            return ((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)) / len;
        };
        const float d1 = side(x, y, ax, ay, bx, by), d2 = side(x, y, bx, by, qx, qy), d3 = side(x, y, qx, qy, ax, ay);
        const bool inside = (d1 <= 0 && d2 <= 0 && d3 <= 0) || (d1 >= 0 && d2 >= 0 && d3 >= 0);
        edge = std::min(std::abs(d1), std::min(std::abs(d2), std::abs(d3)));
        return inside;
    };
    auto colorAt = [&](float x, float y) -> Rgba {
        float edge = 0.0f;
        if (!sample(x, y, edge)) return {background[0], background[1], background[2], background[3]};
        std::array<float, 4> c = fill;
        if (shape == "gradient") {
            const float t = std::clamp(vertical ? y / h : x / w, 0.0f, 1.0f);
            for (int i = 0; i < 4; i++) c[i] = fill[i] + (second[i] - fill[i]) * t;
        } else if (shape == "checker") {
            const int ix = static_cast<int>(x / (w / cells)), iy = static_cast<int>(y / (h / cells));
            if ((ix + iy) % 2) c = second;
        }
        if (outlineWidth > 0.0f && edge < outlineWidth) c = outline;
        return {c[0], c[1], c[2], c[3]};
    };

    const int ss = (width * height > 512 * 512) ? 2 : 4;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; sy++) {
                for (int sx = 0; sx < ss; sx++) {
                    Rgba c = colorAt(x + (sx + 0.5f) / ss, y + (sy + 0.5f) / ss);
                    r += c.r * c.a;
                    g += c.g * c.a;
                    b += c.b * c.a;
                    a += c.a;
                }
            }
            unsigned char* p = &pixels[(static_cast<size_t>(y) * width + x) * 4];
            const float n = static_cast<float>(ss * ss);
            p[0] = static_cast<unsigned char>(std::lround(std::clamp(a > 0 ? r / a : 0.0f, 0.0f, 1.0f) * 255.0f));
            p[1] = static_cast<unsigned char>(std::lround(std::clamp(a > 0 ? g / a : 0.0f, 0.0f, 1.0f) * 255.0f));
            p[2] = static_cast<unsigned char>(std::lround(std::clamp(a > 0 ? b / a : 0.0f, 0.0f, 1.0f) * 255.0f));
            p[3] = static_cast<unsigned char>(std::lround(std::clamp(a / n, 0.0f, 1.0f) * 255.0f));
        }
    }
    std::string png;
    auto append = [](void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size));
    };
    if (!stbi_write_png_to_func(append, &png, width, height, 4, pixels.data(), width * 4)) {
        call->fail("PNG encoding failed.");
        return;
    }
    if (!writeFilesUndoable(call->ctx(), {{file, png}}, error)) {
        call->fail(error);
        return;
    }
    std::string base64;
    int pw = 0, ph = 0;
    ToolResult result = ToolResult::success("Generated " + rel + " (" + std::to_string(width) + "x" + std::to_string(height) + " " + shape +
                                                ")." + (project->isInsideAssetsPath(file) ? "" : " Note: outside the assets directory, components can't reference it."),
                                            Json{{"path", rel}, {"width", width}, {"height", height}});
    if (encodePng(pixels.data(), width, height, 256, base64, pw, ph)) result.images.push_back({base64, "image/png"});
    call->reply(std::move(result));
}

} // namespace

void registerAssetTools(McpRegistry& registry) {
    using namespace schema;

    // Superseded legacy tools
    for (const char* name : {"search_resources", "read_resource_file", "rename_resource", "copy_resource", "delete_resource",
                             "import_resource_file", "create_material_file", "update_material_file", "unlink_material",
                             "write_shader_file"}) {
        registry.removeTool(name);
    }

    const std::vector<std::string> types = {"any", "texture", "model", "audio", "font", "script", "scene", "prefab", "material", "shader", "folder", "other"};

    {
        ToolDef tool;
        tool.name = "list_assets";
        tool.title = "List Assets";
        tool.description =
            "List / search the project's files (what the Project window shows; hidden dirs, .jasperengine and build are "
            "skipped). Filter by type, by a glob pattern (\"*.png\" on file names; with a '/' on the project-relative path, "
            "\"assets/**/enemy*.png\"; ** crosses folders) and/or a name substring query. details adds sizes and image "
            "dimensions. Returns paths (project-relative, usable as asset references), the assets and Lua directories, "
            "and total/truncated.";
        tool.inputSchema = object({
            {"directory", assetPath("Folder to list (default: the project root).")},
            {"type", stringEnum("Asset type filter.", types)},
            {"pattern", string("Glob on the file name, or on the project-relative path when it contains '/'.")},
            {"query", string("Case-insensitive substring of the file name.")},
            {"recursive", withDefault(boolean("Include subfolders."), true)},
            {"details", withDefault(boolean("Add size and image width/height."), false)},
            {"limit", withDefault(integer("Max entries returned.", 1, 2000), 200)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listAssetsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_asset_info";
        tool.title = "Get Asset Info";
        tool.description =
            "Details of one project file: type and size, plus image width/height/channels; model contents (glTF/GLB: meshes, "
            "materials, animations with durations, nodes, skins, images; OBJ: objects, materials); WAV channels, rate and "
            "duration; a .material's fields (same names update_material takes) and the submeshes linked to it; a shader's "
            "includes and the forks using it; a Lua script's syntax status; a scene's project status. Also whether the code "
            "editor has unsaved edits of it.";
        tool.inputSchema = object({{"path", assetPath("Project-relative file.")}}, {"path"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getAssetInfoTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "view_asset";
        tool.title = "View Asset";
        tool.description =
            "Look at an image asset: returns it as MCP image content (PNG, downscaled to max_size). For models, materials "
            "and prefabs it returns the Project window thumbnail (rendered on demand, may take a few seconds). SVGs are not "
            "rasterized here.";
        tool.inputSchema = object({
            {"path", assetPath("Project-relative image, model, material or prefab.")},
            {"max_size", withDefault(integer("Longest side of the returned image in pixels.", 16, 2048), 512)}
        }, {"path"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = viewAssetTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "read_file";
        tool.title = "Read File";
        tool.description =
            "Read a text file of the project (scripts, shaders, materials, configs, docs) as it is on disk; the text "
            "result is the content. Use start_line / max_lines for big files. If you have your own file tools you may use "
            "them instead.";
        tool.inputSchema = object({
            {"path", assetPath("Project-relative text file.")},
            {"start_line", withDefault(integer("First line (1-based).", 1, 100000000), 1)},
            {"max_lines", withDefault(integer("Max lines returned.", 1, 20000), 2000)}
        }, {"path"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = readFileTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "write_file";
        tool.title = "Write File";
        tool.description =
            "Create or change a text file through the editor (one undo step, scope project) and validate it: Lua syntax "
            "is checked, shader forks using a .vert/.frag/.glsl are recompiled and their errors returned (line + message), "
            "materials must parse. Open code editor buffers reload, attached scripts re-parse their properties, the Project "
            "window refreshes. Pass content (whole file) or edits: [{old_text, new_text, replace_all?}] applied in order "
            "(old_text must match exactly once). Refuses project.yaml, .scene/.prefab (use the editor tools) and files "
            "the human has unsaved edits of in the code editor (unless discard_unsaved_buffer). If you edit files with "
            "your own tools instead, call sync_files afterwards.";
        tool.inputSchema = object({
            {"path", assetPath("Project-relative file; parent folders are created.")},
            {"content", string("The whole new file content.")},
            {"edits", array(object({
                {"old_text", string("Exact text to replace.")},
                {"new_text", string("Replacement.")},
                {"replace_all", withDefault(boolean("Replace every occurrence."), false)}
            }, {"old_text", "new_text"}), "Targeted replacements on the current file.")},
            {"check", withDefault(boolean("Compile affected shader forks."), true)},
            {"discard_unsaved_buffer", withDefault(boolean("Overwrite even if the code editor has unsaved edits of it."), false)}
        }, {"path"});
        tool.handler = writeFileTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "create_folder";
        tool.title = "Create Folder";
        tool.description = "Create a folder in the project (one undo step, scope project). The parent must exist.";
        tool.inputSchema = object({{"path", assetPath("Project-relative path of the new folder, e.g. \"assets/sprites/enemies\".")}}, {"path"});
        tool.handler = createFolderTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "move_asset";
        tool.title = "Move / Rename / Copy Asset";
        tool.description =
            "Rename or move a file or folder (to = its new project-relative path), updating the references to it in "
            "scenes, prefabs, materials, scripts entries and shader forks like the Project window does; or copy it "
            "(copy: true; a copied prefab gets a new GUID). One undo step (scope project). The destination must not exist "
            "and its folder must.";
        tool.inputSchema = object({
            {"path", assetPath("Current project-relative path.")},
            {"to", assetPath("New project-relative path (folder + name).")},
            {"copy", withDefault(boolean("Copy instead of moving."), false)}
        }, {"path", "to"});
        tool.allowedWhilePlaying = false;
        tool.handler = moveAssetTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "delete_asset";
        tool.title = "Delete Assets";
        tool.description =
            "Delete files or folders by moving them to the project trash, like the Project window (one undo step, scope "
            "project, restores them). References to them become missing (prefab instances turn into Missing Prefabs). "
            "Open scenes can't be deleted.";
        tool.inputSchema = object({{"paths", array(string("Project-relative path."), "What to delete.")}}, {"paths"});
        tool.annotations.destructive = true;
        tool.allowedWhilePlaying = false;
        tool.handler = deleteAssetTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "import_files";
        tool.title = "Import Files";
        tool.description =
            "Copy files or folders from absolute paths on this machine into a project folder (to_dir, created if missing), "
            "like dragging them into the Project window. One undo step (scope project). Put assets components reference "
            "under the assets directory. Downloads are not supported: fetch files yourself first.";
        tool.inputSchema = object({
            {"sources", array(string("Absolute path outside the project."), "Files or folders to import.")},
            {"to_dir", assetPath("Destination project folder, e.g. \"assets/sprites\".")},
            {"overwrite", withDefault(boolean("Replace existing files with the same name."), false)}
        }, {"sources", "to_dir"});
        tool.annotations.openWorld = true;
        tool.handler = importFilesTool;
        registry.addTool(std::move(tool));
    }

    const Json materialValues = anyObject(
        "Material fields: baseColorFactor [r,g,b,a] (linear 0..1), metallicFactor, roughnessFactor, alphaCutoff, alphaMode "
        "(AUTO, OPAQUE, MASK, BLEND), emissiveFactor [r,g,b], foliageTransmission, baseColorTexture / emissiveTexture / "
        "metallicRoughnessTexture / occlusionTexture / normalTexture (project-relative image path inside the assets "
        "directory, or null), *TexCoord (0 or 1).");

    {
        ToolDef tool;
        tool.name = "create_material";
        tool.title = "Create Material";
        tool.description =
            "Create a .material file (a shared material: every submesh linked to it follows its edits). Starts from the "
            "defaults, or from a mesh submesh's current material (entity + submesh), then applies values. With entity, "
            "link (default true) links that submesh to the new file. File creation is a project undo step, the link a "
            "scene undo step. Example: {\"path\":\"assets/materials/Red.material\",\"values\":{\"baseColorFactor\":[1,0,0,1]}}.";
        tool.inputSchema = object({
            {"path", assetPath("New project-relative .material path.")},
            {"values", materialValues},
            {"scene", sceneRef()},
            {"entity", entityRef("Mesh GameObject to copy the material from (and link).")},
            {"submesh", withDefault(integer("Submesh index.", 0, 1024), 0)},
            {"link", boolean("Link the submesh to the new file (default: true when entity is given).")},
            {"overwrite", withDefault(boolean("Replace an existing file."), false)}
        }, {"path"});
        tool.handler = createMaterialTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "update_material";
        tool.title = "Update Material";
        tool.description =
            "Change fields of a .material file (only the given ones); every linked submesh updates. One undo step (scope "
            "project). Read the current fields with get_asset_info. Returns the new fields and the linked submeshes.";
        tool.inputSchema = object({
            {"path", assetPath("Project-relative .material file.")},
            {"values", materialValues}
        }, {"path", "values"});
        tool.handler = updateMaterialTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "link_material";
        tool.title = "Link Material";
        tool.description =
            "Link a mesh submesh to a .material file (the submesh then follows the file), or unlink it (material null: it "
            "keeps a copy of the values). One undo step in the scene. To change a single submesh's own material values "
            "without a file, set its submeshes[i].material properties with the component property tools.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("Mesh GameObject.")},
            {"submesh", withDefault(integer("Submesh index.", 0, 1024), 0)},
            {"material", any("Project-relative .material path, or null to unlink.")}
        }, {"entity"});
        tool.allowedWhilePlaying = false;
        tool.handler = linkMaterialTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "fork_shader";
        tool.title = "Fork Shader";
        tool.description =
            "Copy a built-in shader into the project (<directory>/<name>.vert + .frag) and point something at it, as one "
            "undo step: target component (a GameObject's Mesh/UI/Points/Line Renderer/Skybox customShader; component "
            "optional), depth (a Mesh's depth pass: shadows/SSAO; needed when a mesh fork displaces vertices or discards), "
            "scene_default (shader_type mesh/ui/sky/points/lines for every component of the scene without its own), "
            "post_process (a scene post-process pass: pass_index of a pass with the built-in shader, or omit to add a new "
            "pass). fork_includes also copies the engine .glsl includes it uses into the fork's own folder. Then edit the "
            "files (write_file checks the compile) and set parameters with set_shader_uniform.";
        tool.inputSchema = object({
            {"target", stringEnum("What gets the fork (default component when entity is given).", {"component", "depth", "scene_default", "post_process"})},
            {"scene", sceneRef()},
            {"entity", entityRef("For component / depth.")},
            {"component", componentName("Renderable component (default: auto-detect).")},
            {"shader_type", stringEnum("For scene_default.", {"mesh", "ui", "sky", "points", "lines"})},
            {"pass_index", integer("For post_process: the pass to fork (omit to add a pass).")},
            {"directory", withDefault(assetPath("Destination folder."), "shaders")},
            {"name", string("post_process: fork name (others are named after the GameObject / scene).")},
            {"fork_includes", withDefault(boolean("Copy the engine includes too."), false)}
        });
        tool.allowedWhilePlaying = false;
        tool.handler = forkShaderTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "inspect_shader_uniforms";
        tool.title = "Inspect Shader Uniforms";
        tool.description =
            "The custom parameters of a forked shader: the members of its u_vs_customParams / u_fs_customParams block "
            "(u_fs_postParams for a post-process pass) with GLSL type, whether the engine writes it (time, resolution) "
            "and the stored value. Give entity (+ component) or pass_index. Members appear once the fork has compiled.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"component", componentName("Renderable component (default: auto-detect).")},
            {"pass_index", integer("Post-process pass index instead of a GameObject.")}
        });
        tool.annotations.readOnly = true;
        tool.handler = inspectShaderUniformsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_shader_uniform";
        tool.title = "Set Shader Uniform";
        tool.description =
            "Set (or remove) one custom uniform of a forked shader by member name, one undo step: value is a number "
            "(float/int members) or [x,y], [x,y,z], [x,y,z,w]. Give entity (+ component) or pass_index for a post-process "
            "pass. time and resolution are engine-written. Names the shader doesn't declare are stored but unused: check "
            "with inspect_shader_uniforms.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"component", componentName("Renderable component (default: auto-detect).")},
            {"pass_index", integer("Post-process pass index instead of a GameObject.")},
            {"name", string("Member name, e.g. \"amplitude\".")},
            {"value", any("Number or [x,y(,z(,w))].")},
            {"remove", withDefault(boolean("Remove the stored value."), false)}
        }, {"name"});
        tool.handler = setShaderUniformTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "check_shader";
        tool.title = "Check Shader";
        tool.description =
            "Compile shader forks now and return their compile errors (line + message, and the compiler log). shader: a "
            "customShader value (\"shaders/Water\" or one of its .vert/.frag paths); shader_type when it isn't used by a "
            "component yet. Without shader: every fork the loaded scenes use. Use after editing shaders on disk (and "
            "sync_files). A failing fork renders with the built-in shader.";
        tool.inputSchema = object({
            {"shader", string("Fork base path or .vert/.frag path.")},
            {"shader_type", stringEnum("Shader type to compile as.", {"mesh", "ui", "sky", "points", "lines", "depth", "post_process"})}
        });
        tool.annotations.readOnly = true;
        tool.handler = checkShaderTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_texture_settings";
        tool.title = "Set Texture Settings";
        tool.description =
            "Sampler settings of a component's texture property (one undo step): min_filter (nearest, linear, "
            "nearest_mipmap_nearest, nearest_mipmap_linear, linear_mipmap_nearest, linear_mipmap_linear), mag_filter "
            "(nearest or linear; nearest for crisp pixel art), wrap_u / wrap_v (repeat, mirrored_repeat, clamp_to_edge, "
            "clamp_to_border), svg_scale for .svg sources. Only the given fields change. Settings live on the property "
            "(e.g. sprite texture, submeshes[0].material.baseColorTexture), not on the image file.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"component", componentName()},
            {"property", string("Texture property path, e.g. \"texture\" or \"submeshes[0].material.baseColorTexture\".")},
            {"min_filter", string("Minification filter.")},
            {"mag_filter", stringEnum("Magnification filter.", {"nearest", "linear"})},
            {"wrap_u", stringEnum("Wrap along U.", {"repeat", "mirrored_repeat", "clamp_to_edge", "clamp_to_border"})},
            {"wrap_v", stringEnum("Wrap along V.", {"repeat", "mirrored_repeat", "clamp_to_edge", "clamp_to_border"})},
            {"svg_scale", number("Rasterization scale of .svg sources.")}
        }, {"entity", "component", "property"});
        tool.handler = setTextureSettingsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "generate_placeholder_art";
        tool.title = "Generate Placeholder Art";
        tool.description =
            "Draw a placeholder PNG (antialiased) when you have no art: shape rect, rounded_rect (corner_radius), circle, "
            "ellipse, triangle (pointing up), diamond, gradient (color -> color2, direction), checker (cells, color / "
            "color2); optional outline (outline_width, outline_color) and background (default transparent). Colors: "
            "[r,g,b(,a)] 0..1 or \"#RRGGBB(AA)\". Writes it (one undo step) and returns a preview. Put sprites under the "
            "assets directory. Example: {\"path\":\"assets/sprites/player.png\",\"width\":64,\"height\":64,"
            "\"shape\":\"circle\",\"color\":\"#3080ff\",\"outline_width\":3}.";
        tool.inputSchema = object({
            {"path", assetPath("Project-relative .png path.")},
            {"width", withDefault(integer("Width in pixels.", 1, 2048), 64)},
            {"height", withDefault(integer("Height in pixels.", 1, 2048), 64)},
            {"shape", withDefault(stringEnum("Shape.", {"rect", "rounded_rect", "circle", "ellipse", "triangle", "diamond", "gradient", "checker"}), "rect")},
            {"color", any("Fill color.")},
            {"color2", any("Second color (gradient end, checker).")},
            {"outline_width", withDefault(number("Outline width in pixels.", 0, 512), 0)},
            {"outline_color", any("Outline color (default black).")},
            {"background", any("Color outside the shape (default transparent).")},
            {"corner_radius", number("rounded_rect corner radius in pixels.")},
            {"direction", withDefault(stringEnum("gradient direction.", {"vertical", "horizontal"}), "vertical")},
            {"cells", withDefault(integer("checker cells per side.", 1, 256), 8)},
            {"overwrite", withDefault(boolean("Replace an existing file."), false)}
        }, {"path"});
        tool.handler = placeholderArtTool;
        registry.addTool(std::move(tool));
    }
}

}
