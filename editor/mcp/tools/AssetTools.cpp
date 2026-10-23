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
#include "command/type/PropertyCmd.h"
#include "command/type/RenameFileCmd.h"
#include "command/type/ScenePropertyCmd.h"
#include "command/type/UnlinkMaterialCmd.h"
#include "component/MeshComponent.h"
#include "htmlui/HtmlUiImporters.h"
#include "htmlui/VideoImporter.h"
#include "htmlui/compat/ImportManifest.h"
#include "material/MaterialChange.h"
#include "material/MaterialPresets.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "texture/Material.h"
#include "texture/TextureData.h"
#include "util/Base64.h"
#include "util/ProjectChecks.h"
#include "util/ProjectUtils.h"
#include "util/Util.h"
#include "subsystem/RenderSystem.h"
#include "window/CodeEditor.h"
#include "window/ProjectWindow.h"

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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

// Compiles the forks on a worker thread (ProjectChecks::runShaderCheck); `done` gets, on the main
// thread, the results [{shader, type, ok, errors, log}] and the findings of them all
void checkShadersWithFindings(const CallPtr& call, std::vector<ProjectChecks::ShaderUse> forks, std::function<void(Json results, Json findings)> done) {
    Project* project = call->ctx().project();
    // Drops cached builds and source snapshots so both the check and the scene use the files on disk
    ProjectChecks::invalidateShaderCaches(project);
    struct Job {
        std::vector<ProjectChecks::ShaderUse> forks;
        std::vector<ShaderKey> keys;
        Json results = Json::array();
        std::vector<ProjectChecks::Finding> findings;
        std::atomic<bool> finished{false};
    };
    auto job = std::make_shared<Job>();
    job->forks = std::move(forks);
    for (ProjectChecks::ShaderUse& fork : job->forks) {
        job->keys.push_back(ProjectChecks::prepareShaderCheck(project, fork));
    }
    std::thread([job, project]() {
        for (size_t i = 0; i < job->forks.size(); i++) {
            const ProjectChecks::ShaderCheck check = ProjectChecks::runShaderCheck(project, job->forks[i], job->keys[i]);
            job->results.push_back(ProjectChecks::toJson(check));
            // a UI effect's forms (phase 5) share their source: an error is reported once
            for (const ProjectChecks::Finding& finding : check.findings) {
                const bool seen = std::any_of(job->findings.begin(), job->findings.end(), [&](const ProjectChecks::Finding& f) {
                    return f.path == finding.path && f.line == finding.line && f.col == finding.col && f.message == finding.message;
                });
                if (!seen) job->findings.push_back(finding);
            }
        }
        job->finished = true;
    }).detach();
    call->waitUntil([job]() { return job->finished.load(); }, 120000, [job, done](bool timedOut) {
        if (timedOut) {
            done(Json::array({{{"ok", false}, {"log", "Shader check timed out."}}}), Json::array());
            return;
        }
        ProjectChecks::sortFindings(job->findings);
        done(job->results, ProjectChecks::toJson(job->findings));
    });
}

} // namespace

void checkShadersAsync(const CallPtr& call, std::vector<ProjectChecks::ShaderUse> forks, std::function<void(Json results)> done) {
    checkShadersWithFindings(call, std::move(forks), [done](Json results, Json) { done(std::move(results)); });
}

} // namespace w4

// ======================================================================== asset tools

namespace {

using namespace w4;
namespace importers = jasperengine::editor::htmlui;

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

// The optional submesh index of the material tools: `fallback` when absent or null; false (and
// a fix-it failure) for anything but a whole number from 0, rather than a raw JSON type error.
// The caller checks the upper bound against the mesh.
bool submeshArg(const CallPtr& call, int fallback, int& submesh) {
    const Json& args = call->args();
    submesh = fallback;
    if (!args.contains("submesh") || args["submesh"].is_null()) return true;
    const Json& value = args["submesh"];
    double number = -1.0;
    if (value.is_number()) number = value.get<double>();
    if (number < 0.0 || number != std::floor(number) || number > 1.0e6) {
        call->fail("submesh must be a submesh index, a whole number from 0 (got " + value.dump() + "); get_entity lists a "
                   "mesh's submeshes.");
        return false;
    }
    submesh = static_cast<int>(number);
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

// The scene (open), the GameObject (when given or required) and the component (when given)
// of a tool's scene / entity / component arguments
struct Target {
    SceneProject* scene = nullptr;
    Entity entity = NULL_ENTITY;
    bool hasComponent = false;
    ComponentType component = ComponentType::Transform;
};

bool resolveTarget(const CallPtr& call, Target& target, bool entityRequired) {
    const Json& args = call->args();
    std::string error;
    target.scene = McpRefs::resolveScene(call->ctx().project(), args.contains("scene") ? args["scene"] : Json(), error);
    if (!target.scene) {
        call->fail(error);
        return false;
    }
    if (!target.scene->scene) {
        call->fail(McpRefs::notOpenError(call->ctx().project(), *target.scene));
        return false;
    }
    if (args.contains("entity") && !args["entity"].is_null()) {
        target.entity = McpRefs::resolveEntity(target.scene, args["entity"], error);
        if (target.entity == NULL_ENTITY) {
            call->fail(error);
            return false;
        }
    } else if (entityRequired) {
        call->fail("entity is required: a GameObject id or path.");
        return false;
    }
    if (args.contains("component") && args["component"].is_string()) {
        if (!McpRefs::resolveComponent(args["component"], target.component, error)) {
            call->fail(error);
            return false;
        }
        target.hasComponent = true;
    }
    return true;
}

// The shader-capable component of target.entity: the given one, else the first renderable one
bool resolveShaderComponent(const CallPtr& call, Target& target, ShaderType& shaderType) {
    Scene* scene = target.scene->scene;
    if (target.hasComponent) {
        if (!ProjectChecks::shaderTypeForComponent(target.component, shaderType)) {
            call->fail("The component does not take custom shaders: use mesh, ui, points, linerenderer or skybox.");
            return false;
        }
        if (!McpRefs::hasComponent(target.scene, target.entity, target.component)) {
            call->fail("The GameObject has no " + McpRefs::componentName(target.component) + " component.");
            return false;
        }
        return true;
    }
    for (ComponentType candidate : {ComponentType::MeshComponent, ComponentType::UIComponent, ComponentType::PointsComponent,
                                    ComponentType::LineRendererComponent, ComponentType::SkyboxComponent}) {
        if (McpRefs::hasComponent(target.scene, target.entity, candidate)) {
            target.component = candidate;
            target.hasComponent = true;
            ProjectChecks::shaderTypeForComponent(candidate, shaderType);
            return true;
        }
    }
    (void)scene;
    call->fail("The GameObject has no shader-capable component (mesh, ui, points, linerenderer, skybox).");
    return false;
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
    if (!ProjectChecks::readFile(file, content)) {
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
    McpValues::materialStochasticToJson(material, data);
    for (const auto& [name, member] : materialTextures()) {
        const Texture& texture = material.*member;
        data[name] = texture.empty() ? Json(nullptr) : Json(McpValues::assetPathToJson(project, texture.getPath(0)));
    }
    for (const auto& [name, member] : materialTexCoords()) data[name] = material.*member;
    // shader library: the shader, its values and the render state
    McpValues::materialShaderLibraryToJson(project, nullptr, material, data, true);
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
        } else if (McpValues::isMaterialShaderLibraryKey(key)) {
            // applied together below, the shader first
        } else if (McpValues::isMaterialStochasticKey(key)) {
            // applied together below
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
                    "occlusionTexture, normalTexture, the *TexCoord indices, stochasticTiling and its stochasticCellSize, "
                    "stochasticBlendSharpness, stochasticContrast, stochasticRotation, and the shader library's shader, shaderParams, "
                    "shaderTextures, shaderFeatures, blendMode, cullMode, depthWrite, depthTest, depthOffsetFactor, "
                    "depthOffsetUnits, sortPriority, lighting, stencil, colorWrite, extraPass.";
            return false;
        }
    }
    if (!McpValues::materialStochasticFromJson(values, material, error)) return false;
    return McpValues::materialShaderLibraryFromJson(project, nullptr, values, material, error);
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
            if (it->is_directory(ec) && ProjectChecks::isSkippedDir(it->path())) {
                it.disable_recursion_pending();
                continue;
            }
            const std::string name = it->path().filename().string();
            if (!name.empty() && name[0] == '.') continue;
            consider(*it);
        }
    } else {
        for (const fs::directory_entry& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (entry.is_directory(ec) && ProjectChecks::isSkippedDir(entry.path())) continue;
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
        ProjectChecks::readFile(file, source);
        data["includes"] = ShaderVariantCompiler::getIncludeKeys(source);
        Json forks = Json::array();
        for (const ProjectChecks::ShaderUse& use : ProjectChecks::shaderForksUsingFiles(project, {rel})) forks.push_back(use.base);
        data["forks"] = forks;
    } else if (kind == "script") {
        std::string source;
        ProjectChecks::readFile(file, source);
        data["lines"] = std::count(source.begin(), source.end(), '\n') + 1;
        if (Util::isLuaFile(rel)) {
            ProjectChecks::LuaSyntax syntax = ProjectChecks::checkLuaSyntax(source, rel);
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
    if (!ProjectChecks::readFile(file, content)) {
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
        ProjectChecks::readFile(file, content);
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
        ProjectChecks::LuaSyntax syntax = ProjectChecks::checkLuaSyntax(content, rel);
        data["syntax_ok"] = syntax.ok;
        if (!syntax.ok) {
            data["errors"] = Json::array({{{"line", syntax.line}, {"message", syntax.message}}});
            text += " LUA SYNTAX ERROR line " + std::to_string(syntax.line) + ": " + syntax.message;
        }
    } else if (Util::isScriptFile(rel)) {
        text += " C++: run compile_scripts to check it.";
    }
    if (Util::isShaderFile(rel)) {
        std::vector<ProjectChecks::ShaderUse> forks = ProjectChecks::shaderForksUsingFiles(project, {rel});
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
                        text += "\n" + r.value("shader", std::string());
                        // a UI effect: its form and the diagnostic code (HTMLUI-FX-0015 effect-shader-compile, -0014 effect-manifest)
                        if (r.contains("effect")) text += " (" + r.value("form", std::string()) + " form)";
                        if (r.contains("code")) text += " [" + r.value("code", std::string()) + "]";
                        text += ": ";
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

// The remote half of import_files (phase-4 spec §3.6.5): the HTML UI importers (network) run on a
// worker thread; the main thread then rewrites the document and makes the editor notice the files.
struct RemoteImport {
    fs::path projectRoot;
    std::string destDir;                  // project-relative, inside the assets
    std::vector<std::string> urls, fonts, icons;
    std::string document;                 // from_document, project-relative ("" = none)
    // local videos the engine does not play (phase 6): converted into videoTarget by FFmpeg
    std::vector<fs::path> videos;
    fs::path videoTarget;
    bool overwrite = false;
    std::vector<importers::VideoImportResult> videoResults;
    // filled by the worker
    std::vector<std::pair<std::string, importers::ImportResult>> results;   // what was asked -> its result
    importers::ImportResult documentResult;
    std::atomic<bool> done{false};
};

bool isRemoteUrl(const std::string& text) {
    std::string lower = text.substr(0, 8);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

// An optional array of strings argument; false after failing the call
bool stringArray(const CallPtr& call, const char* key, std::vector<std::string>& out) {
    const Json& args = call->args();
    if (!args.contains(key) || args[key].is_null()) return true;
    if (!args[key].is_array()) {
        call->fail(std::string(key) + " must be an array of strings.");
        return false;
    }
    for (const Json& item : args[key]) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            call->fail(std::string(key) + " must be an array of non-empty strings.");
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

// Copies local files and folders into to_dir (one project undo step); false after failing the call
bool importLocalFiles(const CallPtr& call, const std::vector<std::string>& local, Json& data, std::string& text) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    if (!args.contains("to_dir") || args["to_dir"].is_null()) {
        call->fail("to_dir is required with local sources (a project folder, e.g. \"assets/sprites\").");
        return false;
    }
    fs::path target;
    if (!resolveExisting(call, "to_dir", target, false)) return false;
    std::error_code ec;
    std::vector<std::string> sources;
    Json imported = Json::array();
    for (const std::string& item : local) {
        const fs::path source = fs::u8path(item).lexically_normal();
        if (!source.is_absolute() || !fs::exists(source, ec)) {
            call->fail(item + " must be an existing absolute path (or an http(s) URL).");
            return false;
        }
        if (Util::isInsidePath(source, project->getProjectPath())) {
            call->fail(item + " is already in the project; use move_asset (copy: true) instead.");
            return false;
        }
        const fs::path dest = target / source.filename();
        if (fs::exists(dest, ec) && !args.value("overwrite", false)) {
            call->fail(projectRelative(project, dest) + " already exists; pass overwrite: true to replace it.");
            return false;
        }
        sources.push_back(source.string());
        imported.push_back(projectRelative(project, dest));
    }
    if (!fs::exists(target, ec)) {
        fs::create_directories(target, ec);
        if (ec) {
            call->fail("Can't create " + projectRelative(project, target) + ": " + ec.message());
            return false;
        }
    }
    if (!addProjectCommand(call->ctx(), new CopyFileCmd(project, sources, target.string(), true))) {
        call->fail("Copying the files failed.");
        return false;
    }
    std::vector<fs::path> changed;
    for (const Json& rel : imported) changed.push_back(project->getProjectPath() / fs::u8path(rel.get<std::string>()));
    syncFiles(call->ctx(), changed, false);
    data["imported"] = imported;
    text = "Imported " + std::to_string(imported.size()) + " item(s) into " + projectRelative(project, target) + ".";
    if (!project->isInsideAssetsPath(target)) {
        text += " Note: outside the assets directory (" + projectRelative(project, project->getAssetsPath()) + "), components can't reference them.";
    }
    return true;
}

// Every occurrence of `from` (also HTML-escaped, & as &amp;) in text replaced by `to`; the count
size_t replaceAll(std::string& text, const std::string& from, const std::string& to) {
    size_t count = 0;
    std::vector<std::string> forms = {from};
    std::string escaped;
    for (char c : from) escaped += c == '&' ? std::string("&amp;") : std::string(1, c);
    if (escaped != from) forms.push_back(escaped);
    for (const std::string& form : forms) {
        for (size_t at = text.find(form); at != std::string::npos; at = text.find(form, at + to.size())) {
            text.replace(at, form.size(), to);
            count++;
        }
    }
    return count;
}

void finishRemoteImport(const CallPtr& call, const std::shared_ptr<RemoteImport>& state, Json data, std::string text, bool timedOut) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    if (timedOut) {
        call->fail((text.empty() ? std::string() : text + " ") +
                   "The remote import is still running after 10 minutes (it goes on in the background; the files appear "
                   "in " + state->destDir + " when it ends).", data);
        return;
    }
    Json remote = Json::array(), failed = Json::array();
    std::vector<fs::path> changed;
    size_t written = 0;
    auto addFiles = [&](const importers::ImportResult& result) {
        for (const std::string& file : result.written) {
            changed.push_back(state->projectRoot / fs::u8path(file));
            written++;
        }
        for (const auto& [what, why] : result.failed) failed.push_back({{"source", what}, {"error", why}});
        if (!result.fontFaceCss.empty()) data["font_face_css"] = result.fontFaceCss;
    };
    for (const auto& [source, result] : state->results) {
        addFiles(result);
        if (!result.written.empty()) remote.push_back({{"source", source}, {"files", result.written}});
    }

    // the converted (or copied unconverted) videos
    Json videos = Json::array(), videoDiagnostics = Json::array();
    std::string videoText;
    for (const importers::VideoImportResult& result : state->videoResults) {
        Json item = {{"message", result.message}, {"converted", result.converted}};
        if (!result.output.empty()) {
            item["file"] = projectRelative(project, result.output);
            changed.push_back(result.output);
        }
        if (!result.command.empty()) item["command"] = result.command;
        if (!result.encoder.empty()) item["encoder"] = result.encoder;
        videoText += "\n" + result.message;
        for (const jasperengine::htmlui::compat::Diagnostic& d : result.diagnostics) {
            videoDiagnostics.push_back({{"code", d.code.id()}, {"severity", jasperengine::htmlui::compat::severityName(d.severity)},
                                        {"message", d.message}, {"suggestion", d.suggestion}});
            videoText += "\n" + std::string(jasperengine::htmlui::compat::severityName(d.severity)) + ": " + d.message + " (fix: " +
                         d.suggestion + ") [" + d.code.id() + "]";
        }
        if (!result.ok && result.diagnostics.empty())
            failed.push_back({{"source", result.output.filename().u8string()}, {"error", result.message}});
        videos.push_back(item);
    }
    if (!state->videoResults.empty()) {
        data["videos"] = videos;
        if (!videoDiagnostics.empty()) data["diagnostics"] = videoDiagnostics;
        syncFiles(ctx, changed, false);
        if (ProjectWindow* window = ctx.projectWindow()) window->refreshCurrentDirectory();
        text += videoText;
        if (state->urls.empty() && state->fonts.empty() && state->icons.empty() && state->document.empty()) {
            const bool anyOk = std::any_of(state->videoResults.begin(), state->videoResults.end(),
                                           [](const importers::VideoImportResult& r) { return r.ok || r.copied; });
            if (!anyOk && !data.contains("imported"))
                call->fail(text, data);
            else
                call->ok(text, data);
            return;
        }
    }

    // from_document: the URLs it imported, then (rewrite) each replaced in the file by its path relative to the file
    std::string rewriteNote;
    if (!state->document.empty()) {
        const importers::ImportResult& result = state->documentResult;
        addFiles(result);
        std::set<std::string> covered;
        for (const auto& [url, local] : result.replaced) {
            remote.push_back({{"source", url}, {"files", Json::array({local})}});
            covered.insert(local);
        }
        Json rest = Json::array();
        for (const std::string& file : result.written) if (!covered.count(file)) rest.push_back(file);
        if (!rest.empty()) remote.push_back({{"source", state->document}, {"files", rest}});

        const Json& args = call->args();
        if (args.value("rewrite", true) && !result.replaced.empty()) {
            const fs::path file = state->projectRoot / fs::u8path(state->document);
            std::string content, error;
            if (!ProjectChecks::readFile(file, content)) {
                rewriteNote = " Can't read " + state->document + " to rewrite it.";
            } else {
                const fs::path dir = file.parent_path().lexically_normal();
                Json replacements = Json::array();
                for (const auto& [url, local] : result.replaced) {
                    // a Google Fonts css2 link becomes its generated @font-face stylesheet
                    const std::string to = (state->projectRoot / fs::u8path(local)).lexically_normal().lexically_relative(dir).generic_u8string();
                    if (replaceAll(content, url, to) > 0) replacements.push_back({{"from", url}, {"to", to}});
                }
                if (!replacements.empty()) {
                    if (writeFilesUndoable(ctx, {{file, content}}, error)) {
                        data["rewritten"] = {{"file", state->document}, {"replacements", replacements}};
                    } else {
                        rewriteNote = " Rewriting " + state->document + " failed: " + error;
                    }
                }
            }
        }
    }
    if (written > 0) {
        // the runtime resolves remote URLs through the manifest (a document that keeps them: rewrite false)
        jasperengine::htmlui::compat::ImportManifest::reload();
        changed.push_back(project->getAssetsPath() / fs::u8path(jasperengine::htmlui::compat::ImportManifest::kPath));
        syncFiles(ctx, changed, false);
        if (ProjectWindow* window = ctx.projectWindow()) window->refreshCurrentDirectory();
    }
    data["remote"] = remote;
    data["failed"] = failed;

    std::string summary = "Remote import into " + state->destDir + ": " + std::to_string(written) + " file(s) written";
    if (data.contains("font_face_css")) summary += ", @font-face stylesheet " + data["font_face_css"].get<std::string>() + " (link it)";
    summary += ".";
    if (data.contains("rewritten")) {
        summary += " Rewrote " + std::to_string(data["rewritten"]["replacements"].size()) + " URL(s) in " + state->document +
                   " (undo scope project restores them).";
    }
    summary += rewriteNote;
    for (const Json& f : failed) summary += "\nFailed: " + f["source"].get<std::string>() + ": " + f["error"].get<std::string>();
    text += (text.empty() ? "" : "\n") + summary;
    const bool nothing = written == 0 && !data.contains("imported") && !data.contains("rewritten") &&
                         state->documentResult.replaced.empty();
    if (nothing && !failed.empty()) {
        call->fail(text, data);
        return;
    }
    call->ok(text, data);
}

void importFilesTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::vector<std::string> sources, fonts, icons;
    if (!stringArray(call, "sources", sources) || !stringArray(call, "fonts", fonts) || !stringArray(call, "icons", icons)) return;
    const bool fromDocument = args.contains("from_document") && !args["from_document"].is_null();
    if (sources.empty() && fonts.empty() && icons.empty() && !fromDocument) {
        call->fail("pass sources, fonts, icons or from_document.");
        return;
    }
    std::vector<std::string> local, urls;
    for (const std::string& source : sources) (isRemoteUrl(source) ? urls : local).push_back(source);
    if (!local.empty() && (!args.contains("to_dir") || args["to_dir"].is_null())) {
        call->fail("to_dir is required with local sources (a project folder, e.g. \"assets/sprites\").");
        return;
    }

    auto state = std::make_shared<RemoteImport>();
    state->projectRoot = project->getProjectPath();
    // videos the engine does not play are converted (FFmpeg, on the worker) instead of copied (phase 6)
    std::vector<std::string> plainFiles;
    for (const std::string& item : local) {
        const fs::path source = fs::u8path(item).lexically_normal();
        std::error_code exists;
        if (source.is_absolute() && fs::is_regular_file(source, exists) && importers::VideoImporter::isVideoFile(source) &&
            !importers::VideoImporter::isPlayable(source)) {
            state->videos.push_back(source);
        } else {
            plainFiles.push_back(item);
        }
    }
    if (!state->videos.empty()) {
        fs::path target;
        if (!resolveExisting(call, "to_dir", target, false)) return;
        state->videoTarget = target;
        state->overwrite = args.value("overwrite", false);
        for (const fs::path& video : state->videos) {
            fs::path output = target / video.filename();
            output.replace_extension(".webm");
            std::error_code ec;
            if (fs::exists(output, ec) && !state->overwrite) {
                call->fail(projectRelative(project, output) + " already exists; pass overwrite: true to replace it.");
                return;
            }
        }
    }
    local = plainFiles;
    const bool remote = !urls.empty() || !fonts.empty() || !icons.empty() || fromDocument;
    if (remote) {
        fs::path dest;
        std::string error;
        const std::string assetsRel = projectRelative(project, project->getAssetsPath());
        const std::string destArg = args.contains("to_dir") && args["to_dir"].is_string() ? args["to_dir"].get<std::string>()
                                                                                            : (assetsRel.empty() ? "" : assetsRel + "/") + "ui/imported";
        if (!McpRefs::resolveProjectPath(project, destArg, dest, error, false)) {
            call->fail(error);
            return;
        }
        if (!project->isInsideAssetsPath(dest.lexically_normal())) {
            call->fail("to_dir must be inside the assets directory (" + assetsRel + "/) for remote imports: documents load from there.");
            return;
        }
        state->destDir = projectRelative(project, dest.lexically_normal());
        if (fromDocument) {
            fs::path document;
            if (!args["from_document"].is_string() ||
                !McpRefs::resolveProjectPath(project, args["from_document"].get<std::string>(), document, error, true)) {
                call->fail(error.empty() ? "from_document must be a project-relative .html or .css path." : error);
                return;
            }
            document = document.lexically_normal();
            if (!ProjectChecks::isUiFile(document.generic_u8string())) {
                call->fail("from_document must be an .html, .htm or .css file.");
                return;
            }
            if (!project->isInsideAssetsPath(document)) {
                call->fail("from_document must be inside the assets directory (" + assetsRel + "/).");
                return;
            }
            state->document = projectRelative(project, document);
        }
        state->urls = urls;
        state->fonts = fonts;
        state->icons = icons;
    }

    Json data = Json::object();
    std::string text;
    if (!local.empty() && !importLocalFiles(call, local, data, text)) return;
    if (!remote && state->videos.empty()) {
        call->ok(text, data);
        return;
    }

    std::thread([state]() {
        for (const fs::path& video : state->videos)
            state->videoResults.push_back(importers::VideoImporter::importFile(video, state->videoTarget, state->overwrite));
        const std::string dest = state->destDir;
        for (const std::string& url : state->urls) state->results.emplace_back(url, importers::importUrl(state->projectRoot, url, dest));
        if (!state->fonts.empty()) {
            std::string label;
            for (const std::string& f : state->fonts) label += (label.empty() ? "" : ", ") + f;
            state->results.emplace_back(label, importers::importGoogleFonts(state->projectRoot, state->fonts, dest));
        }
        for (const std::string& icon : state->icons) {
            state->results.emplace_back(icon, importers::importLucideIcons(state->projectRoot, {icon}, dest));
        }
        if (!state->document.empty()) state->documentResult = importers::importDocumentReferences(state->projectRoot, state->document, dest);
        state->done.store(true, std::memory_order_release);
    }).detach();
    call->waitUntil([state]() { return state->done.load(std::memory_order_acquire); }, 600000,
                    [call, state, data, text](bool timedOut) { finishRemoteImport(call, state, data, text, timedOut); });
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
    int submeshIndex = 0;
    if (!submeshArg(call, 0, submeshIndex)) return;
    unsigned int submesh = static_cast<unsigned int>(submeshIndex);
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
    int submeshIndex = 0;
    if (!submeshArg(call, 0, submeshIndex)) return;
    const unsigned int submesh = static_cast<unsigned int>(submeshIndex);
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

// ------------------------------------------------------------------ material presets

// The fields a preset sets on a default material, as get_asset_info / update_material name them
Json presetFieldsJson(Project* project, const MaterialPreset& preset) {
    const Json before = materialToJson(project, Material());
    const Json after = materialToJson(project, applyMaterialPreset(preset, Material()));
    Json sets = Json::object();
    for (auto it = after.begin(); it != after.end(); ++it) {
        if (!before.contains(it.key()) || before[it.key()] != it.value()) sets[it.key()] = it.value();
    }
    return sets;
}

std::string presetIdsText() {
    std::string ids;
    for (const MaterialPreset& preset : getMaterialPresets()) ids += (ids.empty() ? "" : ", ") + preset.id;
    return ids.empty() ? "(none)" : ids;
}

void listMaterialPresetsTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    Json presets = Json::array();
    std::string text;
    for (const MaterialPreset& preset : getMaterialPresets()) {
        presets.push_back({{"id", preset.id}, {"name", preset.name}, {"description", preset.description},
                           {"sets", presetFieldsJson(project, preset)}});
        text += "\n" + preset.id + " (" + preset.name + "): " + preset.description;
    }
    call->ok(std::to_string(presets.size()) + " material preset(s); apply_material_preset applies one:" + text,
             Json{{"presets", presets}});
}

void applyMaterialPresetTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string id = args.value("preset", std::string());
    const MaterialPreset* preset = findMaterialPreset(id);
    if (!preset) {
        call->fail("No material preset \"" + id + "\". The presets: " + presetIdsText() + " (list_material_presets).");
        return;
    }
    std::string error;

    // a .material file: like update_material (linked submeshes follow; undo with scope project)
    if (args.contains("path") && !args["path"].is_null()) {
        if (args.contains("entity") || args.contains("entities")) {
            call->fail("Pass either path (a .material file) or entity/entities (submesh materials), not both.");
            return;
        }
        fs::path file;
        if (!resolveExisting(call, "path", file)) return;
        const std::string rel = projectRelative(project, file);
        if (extensionOf(file) != ".material") {
            call->fail(rel + " is not a .material file.");
            return;
        }
        Material material;
        if (!loadMaterialFile(file, material, error)) {
            call->fail(error);
            return;
        }
        material = applyMaterialPreset(*preset, material);
        material.name = rel;
        if (!writeFilesUndoable(call->ctx(), {{file, materialFileContent(material)}}, error)) {
            call->fail(error);
            return;
        }
        Json users = materialUsers(project, rel);
        call->ok("Applied preset " + preset->id + " to " + rel + "; " + std::to_string(users.size()) +
                 " linked submesh(es) follow it. Undo with scope project.",
                 Json{{"preset", preset->id}, {"path", rel}, {"material", materialToJson(project, material)}, {"linked_by", users}});
        return;
    }

    SceneProject* sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sceneProject) {
        call->fail(error);
        return;
    }
    std::vector<Entity> entities;
    if (!McpRefs::resolveEntityArgs(sceneProject, args, entities, error)) {
        call->fail(error);
        return;
    }
    int onlySubmesh = -1;
    if (!submeshArg(call, -1, onlySubmesh)) return;
    const bool oneSubmesh = onlySubmesh >= 0;

    auto* multi = new MultiPropertyCmd();
    Json applied = Json::array();
    std::vector<std::string> unlinked;
    for (Entity entity : entities) {
        MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
        if (!mesh || mesh->numSubmeshes == 0) {
            delete multi;
            call->fail(McpRefs::entityPath(sceneProject, entity) + " has no Mesh component with submeshes (presets set a "
                       "mesh material: meshes, models, sprites, tilemaps).");
            return;
        }
        if (oneSubmesh && (onlySubmesh < 0 || (unsigned int)onlySubmesh >= mesh->numSubmeshes)) {
            delete multi;
            call->fail("submesh must be 0.." + std::to_string(mesh->numSubmeshes - 1) + " for " + McpRefs::entityPath(sceneProject, entity) + ".");
            return;
        }
        for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
            if (oneSubmesh && (int)s != onlySubmesh) continue;
            Material from = mesh->submeshes[s].material;
            const std::string property = "submeshes[" + std::to_string(s) + "].material";
            const std::string linked = project->getMaterialFilePath(sceneProject->id, entity, s);
            if (!linked.empty()) {
                // the preset makes it the GameObject's own material, as an inline material edit does
                multi->addCommand(std::make_unique<UnlinkMaterialCmd>(project, sceneProject->id, ComponentType::MeshComponent, property, s,
                                                                      std::vector<Entity>{entity}));
                from.name.clear();
                unlinked.push_back(McpRefs::entityPath(sceneProject, entity) + " submesh " + std::to_string(s) + " (" + linked + ")");
            }
            // judged by the shader the submesh draws with (the mesh's or the scene's when it names none)
            const Material to = applyMaterialPreset(*preset, from, meshShaderFor(sceneProject->scene, entity));
            addMaterialChangeCommands(*multi, project, sceneProject->id, entity, s, from, to);
            applied.push_back({{"entity", McpRefs::entityJson(sceneProject, entity)}, {"submesh", s}, {"unlinked", !linked.empty()}});
        }
    }
    const uint32_t sceneId = sceneProject->id;
    if (!runSceneCommand(sceneId, multi, error, "Applying the preset changed nothing.")) {
        call->fail(error);
        return;
    }
    std::string text = "Applied preset " + preset->id + " to " + std::to_string(applied.size()) + " submesh(es) (one undo step).";
    if (!unlinked.empty()) {
        text += " Unlinked from their .material file (now their own material; apply_material_preset path= changes a shared file):";
        for (const std::string& entry : unlinked) text += " " + entry + ";";
    }
    call->ok(text, Json{{"preset", preset->id}, {"applied", applied}, {"sets", presetFieldsJson(project, *preset)}});
}

// ------------------------------------------------------------------ shaders

void forkShaderTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string target = args.value("target", args.contains("entity") ? "component" : "");
    Target where;
    if (!resolveTarget(call, where, target == "component" || target == "depth")) return;
    SceneProject* sceneProject = where.scene;

    auto finish = [call](const std::string& base, Json data, const std::string& text) {
        data["vert_path"] = Util::resolveCustomShaderPaths(base).vert;
        data["frag_path"] = Util::resolveCustomShaderPaths(base).frag;
        call->ok(text + " Read the files and change only the shading math; your edits are compiled before your next "
                        "call (errors in external_changes), or check them now with check_shader.", data);
    };
    const fs::path directory = fs::path(args.value("directory", ProjectUtils::defaultShaderForkDir().generic_string())).lexically_normal();
    if (directory != "." && !Util::isSafeRelativePath(directory)) {
        call->fail("directory must be project-relative.");
        return;
    }
    const bool forkIncludes = args.value("fork_includes", false);
    // ForkShaderCmd writes the files in execute(); refresh the Project window and confirm them
    auto runFork = [&](ForkShaderCmd* fork, std::string& base) {
        if (!fork->isValid()) {
            const std::string error = fork->getError();
            delete fork;
            call->fail(error.empty() ? "Can't plan the fork." : error);
            return false;
        }
        base = fork->getBase();
        if (!addSceneCommand(sceneProject->id, fork)) {
            call->fail("The fork failed (files not written).");
            return false;
        }
        if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
        std::error_code ec;
        if (!fs::exists(project->getProjectPath() / (base + ".vert"), ec) || !fs::exists(project->getProjectPath() / (base + ".frag"), ec)) {
            call->fail("The fork did not write its source files.");
            return false;
        }
        return true;
    };

    if (target == "scene_default") {
        static const std::map<std::string, ShaderType> types = {
            {"mesh", ShaderType::MESH}, {"ui", ShaderType::UI}, {"sky", ShaderType::SKYBOX},
            {"points", ShaderType::POINTS}, {"lines", ShaderType::LINES}};
        static const std::map<ShaderType, std::string> properties = {
            {ShaderType::MESH, "default_mesh_shader"}, {ShaderType::UI, "default_ui_shader"},
            {ShaderType::SKYBOX, "default_sky_shader"}, {ShaderType::POINTS, "default_points_shader"},
            {ShaderType::LINES, "default_lines_shader"}};
        const std::string typeName = lowerCase(args.value("shader_type", ""));
        auto type = types.find(typeName);
        if (type == types.end()) {
            call->fail("target scene_default needs shader_type: mesh, ui, sky, points or lines.");
            return;
        }
        const std::string property = properties.at(type->second);
        const std::string current = Catalog::getSceneProperty<std::string>(sceneProject->scene, property);
        if (!current.empty()) {
            call->fail("The scene already has a default " + typeName + " shader (" + current + "): edit that fork, or clear " +
                       property + " (set_scene_settings) before forking again.");
            return;
        }
        const std::string name = ProjectUtils::makeUniqueShaderName(project->getProjectPath() / directory, type->second,
                                                                    args.value("name", sceneProject->name + " " + typeName));
        std::string base;
        if (!runFork(new ForkShaderCmd(project, sceneProject, type->second, property, directory, name, forkIncludes), base)) return;
        finish(base, Json{{"custom_shader", base}, {"scene_property", property}},
               "Forked the scene default " + typeName + " shader to " + base + " (.vert/.frag) and set " + property + ".");
        return;
    }
    if (target == "component" || target == "depth") {
        ShaderType shaderType;
        if (!resolveShaderComponent(call, where, shaderType)) return;
        std::string property = "customShader";
        if (target == "depth") {
            if (where.component != ComponentType::MeshComponent) {
                call->fail("Only a mesh component has a depth shader.");
                return;
            }
            shaderType = ShaderType::DEPTH;
            property = "customDepthShader";
        }
        if (std::string* current = Catalog::getPropertyRef<std::string>(sceneProject->scene, where.entity, where.component, property)) {
            if (!current->empty()) {
                call->fail("The component already uses " + *current + " (" + property + "): edit that fork, or set " + property +
                           " to \"\" before forking again.");
                return;
            }
        }
        std::string desired = args.value("name", sceneProject->scene->getEntityName(where.entity) +
                                                     (shaderType == ShaderType::DEPTH ? "_depth" : ""));
        const std::string name = ProjectUtils::makeUniqueShaderName(project->getProjectPath() / directory, shaderType, desired);
        std::string base;
        if (!runFork(new ForkShaderCmd(project, sceneProject->id, where.entity, where.component, shaderType, directory, name,
                                       forkIncludes), base)) return;
        finish(base, Json{{"custom_shader", base}, {"property", property}, {"entity", McpRefs::entityJson(sceneProject, where.entity)},
                          {"component", McpRefs::componentName(where.component)}},
               "Forked the shader to " + base + " (.vert/.frag) and pointed " + property + " at it.");
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
        const std::string desired = args.value("name", sceneProject->name + " Post " + std::to_string(index + 1));
        const std::string name = ProjectUtils::makeUniqueShaderName(project->getProjectPath() / directory, ShaderType::POSTPROCESS, desired);
        const uint32_t sceneId = sceneProject->id;
        auto makePropertyCmd = [project, sceneId, index, passes](const std::string& base) mutable {
            passes[static_cast<size_t>(index)].shader = base;
            return std::make_unique<ScenePropertyCmd<std::vector<PostProcessPass>>>(project, sceneId, "post_process", passes);
        };
        std::string base;
        if (!runFork(new ForkShaderCmd(project, ShaderType::POSTPROCESS, makePropertyCmd, directory, name, forkIncludes), base)) return;
        finish(base, Json{{"custom_shader", base}, {"pass_index", index}, {"new_pass", append}},
               "Forked the post-process shader to " + base + " for pass " + std::to_string(index) + (append ? " (new pass)" : "") +
                   ". Its parameters are a u_fs_postParams block, set with set_shader_uniform (pass_index).");
        return;
    }
    call->fail("target must be component (a GameObject's renderable component), depth (a Mesh's depth/shadow pass), "
               "scene_default (shader_type for the whole scene) or post_process (a scene post-process pass).");
}

const char* uniformTypeName(ShaderUniformType type) {
    switch (type) {
        case ShaderUniformType::FLOAT:  return "float";
        case ShaderUniformType::FLOAT2: return "vec2";
        case ShaderUniformType::FLOAT3: return "vec3";
        case ShaderUniformType::FLOAT4: return "vec4";
        case ShaderUniformType::INT:    return "int";
        case ShaderUniformType::INT2:   return "ivec2";
        case ShaderUniformType::INT3:   return "ivec3";
        case ShaderUniformType::INT4:   return "ivec4";
        default: return "other";
    }
}

Json vec4Array(const Vector4& v) {
    return Json::array({v.x, v.y, v.z, v.w});
}

Json uniformValuesJson(const ShaderUniformValues& values) {
    Json json = Json::object();
    for (const auto& uniform : values) json[uniform.first] = vec4Array(uniform.second);
    return json;
}

// One entry per declared member; engine-written ones (time, resolution) flagged
void appendUniformMembers(Json& members, const CustomUniformBlock& block, const ShaderUniformValues& values) {
    for (const ShaderUniform& uniform : block.members) {
        if (std::any_of(members.begin(), members.end(), [&](const Json& m) { return m.value("name", "") == uniform.name; })) continue;
        const bool reserved = ShaderUniforms::isReserved(uniform.name);
        const bool editable = ShaderUniforms::isEditable(uniform) && !reserved;
        Json member = {{"name", uniform.name}, {"type", uniformTypeName(uniform.type)}, {"engine_written", reserved}, {"editable", editable}};
        if (uniform.arrayCount > 1) member["array_count"] = uniform.arrayCount;
        if (editable) {
            member["value"] = vec4Array(ShaderUniforms::get(values, uniform.name));
            member["has_value"] = ShaderUniforms::has(values, uniform.name);
        }
        members.push_back(member);
    }
}

void componentUniformMembers(Json& members, Json& warnings, Scene* scene, Entity entity, ComponentType component,
                             const ShaderUniformValues& values) {
    std::vector<const CustomUniformBlock*> blocks = Catalog::getShaderUniformBlocks(scene, entity, component);
    for (const CustomUniformBlock* block : blocks) appendUniformMembers(members, *block, values);
    warnings = Catalog::getShaderUniformWarnings(blocks);
}

// Reflected from the shader pool: a pass still building (or failed) lists none
void passUniformMembers(Json& members, Json& warnings, const PostProcessPass& pass, bool& buildFailed) {
    const uint16_t customId = ShaderPool::registerCustomShader(pass.shader);
    buildFailed = customId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::POSTPROCESS, 0, customId);
    std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::POSTPROCESS, 0, customId);
    warnings = Json::array();
    if (!buildFailed && shader && shader->isCreated()) {
        CustomUniformBlock block;
        block.resolve(shader->shaderData, "u_fs_postParams");
        appendUniformMembers(members, block, pass.uniforms);
        warnings = Catalog::getShaderUniformWarnings({&block});
    }
}

// The value is stored either way; the note says when the shader cannot use it
std::string uniformSetNote(const Json& members, const std::string& name, bool& declared) {
    declared = false;
    for (const Json& member : members) {
        if (member.value("name", "") != name) continue;
        declared = true;
        if (!member.value("editable", false)) {
            return " Note: '" + name + "' is a matrix or array member, which takes no value and stays zero; declare one "
                   "scalar or vector member per value instead.";
        }
        return "";
    }
    return " Note: the loaded shader declares no member '" + name + "' (it may still be building, or the name differs); "
           "the value is kept and applies once declared.";
}

bool passIndexArg(const CallPtr& call, const std::vector<PostProcessPass>& passes, int& index) {
    const Json& value = call->args()["pass_index"];
    index = value.is_number_integer() ? value.get<int>() : -1;
    if (index < 0 || index >= static_cast<int>(passes.size())) {
        call->fail("pass_index must be 0.." + std::to_string(static_cast<int>(passes.size()) - 1) + " (the scene has " +
                   std::to_string(passes.size()) + " post-process pass(es)).");
        return false;
    }
    return true;
}

// The manifest parameters of a shader with what is stored for each (else its default)
Json manifestParameters(const ShaderManifest* manifest, const ShaderUniformValues& values) {
    Json list = Json::array();
    if (!manifest) return list;
    const Json described = McpValues::manifestToJson("", *manifest)["properties"];
    for (const ShaderManifestProperty& property : manifest->properties) {
        Json entry = described[list.size()];
        entry["value"] = vec4Array(ShaderUniforms::has(values, property.member) ? ShaderUniforms::get(values, property.member) : property.defaultValue);
        entry["has_value"] = ShaderUniforms::has(values, property.member);
        list.push_back(entry);
    }
    return list;
}

// A submesh material's shader parameters (submeshes[N].material.shaderParams) instead of the
// component's shaderUniforms: asked for with submesh, or the first submesh's material names
// a shader of its own and the mesh none
bool materialParamsTarget(const CallPtr& call, Scene* scene, Entity entity, ComponentType component, int& submesh) {
    submesh = -1;
    if (component != ComponentType::MeshComponent) {
        if (call->args().contains("submesh")) {
            call->fail("submesh is for a mesh's materials; this component keeps its parameters in shaderUniforms.");
            return false;
        }
        return true;
    }
    MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (!mesh) return true;
    if (call->args().contains("submesh")) {
        const Json& value = call->args()["submesh"];
        const int index = value.is_number_integer() ? value.get<int>() : -1;
        if (index < 0 || index >= (int)mesh->numSubmeshes) {
            call->fail("submesh must be 0.." + std::to_string((int)mesh->numSubmeshes - 1) + ".");
            return false;
        }
        submesh = index;
    } else if (mesh->numSubmeshes > 0 && !mesh->submeshes[0].material.shader.empty() && mesh->customShader.empty()) {
        submesh = 0;
    }
    return true;
}

void getShaderUniformsTool(const CallPtr& call) {
    const Json& args = call->args();
    const bool pass = args.contains("pass_index");
    Target where;
    if (!resolveTarget(call, where, !pass)) return;
    Scene* scene = where.scene->scene;
    Json members = Json::array();
    Json warnings;
    if (pass) {
        const std::vector<PostProcessPass>& passes = scene->getPostProcessPasses();
        int index;
        if (!passIndexArg(call, passes, index)) return;
        const PostProcessPass& p = passes[index];
        bool buildFailed = false;
        passUniformMembers(members, warnings, p, buildFailed);
        call->ok("Post-process pass " + std::to_string(index) + " (" + p.shader + "): " + std::to_string(members.size()) + " member(s).",
                 Json{{"pass_index", index}, {"shader", p.shader}, {"enabled", p.enabled}, {"build_failed", buildFailed},
                      {"members", members}, {"warnings", warnings}, {"values", uniformValuesJson(p.uniforms)}});
        return;
    }
    ShaderType shaderType;
    if (!resolveShaderComponent(call, where, shaderType)) return;
    int submesh = -1;
    if (!materialParamsTarget(call, scene, where.entity, where.component, submesh)) return;
    if (submesh >= 0) {
        // a submesh material's shader (its own, else the mesh's or the scene's) and its values
        MeshComponent& mesh = scene->getComponent<MeshComponent>(where.entity);
        Submesh& sub = mesh.submeshes[submesh];
        const std::string property = "submeshes[" + std::to_string(submesh) + "].material.shaderParams";
        const std::string shader = RenderSystem::getSubmeshShader(mesh, sub, scene->getDefaultCustomShader(ShaderType::MESH));
        const std::shared_ptr<const ShaderManifest> manifest = McpValues::findManifest(shader);
        std::vector<const CustomUniformBlock*> blocks = {&sub.customVSParams, &sub.customFSParams, &sub.customVSDepthParams, &sub.customFSDepthParams};
        for (const CustomUniformBlock* block : blocks) appendUniformMembers(members, *block, sub.material.shaderParams);
        Json data = {{"entity", McpRefs::entityJson(where.scene, where.entity)},
                     {"component", McpRefs::componentName(where.component)},
                     {"submesh", submesh},
                     {"property", property},
                     {"material_shader", sub.material.shader},
                     {"effective_shader", shader},
                     {"build_failed", Catalog::isCustomShaderBuildFailed(scene, where.entity, where.component)},
                     {"members", members},
                     {"warnings", Catalog::getShaderUniformWarnings(blocks)},
                     {"values", uniformValuesJson(sub.material.shaderParams)}};
        if (manifest) data["parameters"] = manifestParameters(manifest.get(), sub.material.shaderParams);
        // what this variant reads of the scene (phase 3) and the cost
        if (const uint32_t reads = Catalog::getMaterialSceneInputs(scene, where.entity, submesh, false)) {
            data["scene_inputs"] = McpValues::sceneInputsToJson(reads);
            data["cost_note"] = Catalog::getSceneInputsCost(reads);
        }
        call->ok(std::to_string(members.size()) + " uniform member(s) of submesh " + std::to_string(submesh) + " on " +
                 McpRefs::entityPath(where.scene, where.entity) + (shader.empty() ? "." : " (" + shader + ")."), data);
        return;
    }
    ShaderUniformValues* values = Catalog::getPropertyRef<ShaderUniformValues>(scene, where.entity, where.component, "shaderUniforms");
    std::string* custom = Catalog::getPropertyRef<std::string>(scene, where.entity, where.component, "customShader");
    if (!values || !custom) {
        call->fail("The component has no shader uniforms.");
        return;
    }
    componentUniformMembers(members, warnings, scene, where.entity, where.component, *values);
    Json data = {{"entity", McpRefs::entityJson(where.scene, where.entity)},
                 {"component", McpRefs::componentName(where.component)},
                 {"custom_shader", *custom},
                 {"effective_shader", custom->empty() ? scene->getDefaultCustomShader(shaderType) : *custom},
                 {"build_failed", Catalog::isCustomShaderBuildFailed(scene, where.entity, where.component)},
                 {"members", members},
                 {"warnings", warnings},
                 {"values", uniformValuesJson(*values)}};
    // a mesh's depth fork declares blocks of its own, listed in the same members
    if (std::string* depth = Catalog::getPropertyRef<std::string>(scene, where.entity, where.component, "customDepthShader")) {
        data["custom_depth_shader"] = *depth;
        data["depth_build_failed"] = Catalog::isCustomDepthShaderBuildFailed(scene, where.entity);
    }
    // a shader with a manifest (a library shader) lists its parameters with labels and defaults
    if (const std::shared_ptr<const ShaderManifest> manifest = McpValues::findManifest(data["effective_shader"].get<std::string>())) {
        data["parameters"] = manifestParameters(manifest.get(), *values);
    }
    if (const uint32_t reads = Catalog::getComponentSceneInputs(scene, where.entity, where.component)) {
        data["scene_inputs"] = McpValues::sceneInputsToJson(reads);
        data["cost_note"] = Catalog::getSceneInputsCost(reads);
    }
    call->ok(std::to_string(members.size()) + " uniform member(s) on " + McpRefs::entityPath(where.scene, where.entity) + ".", data);
}

void setShaderUniformTool(const CallPtr& call) {
    const Json& args = call->args();
    const bool pass = args.contains("pass_index");
    Target where;
    if (!resolveTarget(call, where, !pass)) return;
    const std::string name = args.value("name", "");
    if (name.empty()) {
        call->fail("name is required: the uniform block member.");
        return;
    }
    if (ShaderUniforms::isReserved(name)) {
        call->fail("'" + name + "' is written by the engine every frame; rename the shader member to drive it yourself.");
        return;
    }
    const bool remove = args.value("remove", false);
    Vector4 value(0, 0, 0, 0);
    const Json rawValue = args.contains("value") ? args["value"] : Json();
    if (!remove) {
        const Json& v = rawValue;
        if (v.is_number()) {
            value.x = v.get<float>();
        } else if (v.is_array() && v.size() >= 2 && v.size() <= 4 &&
                   std::all_of(v.begin(), v.end(), [](const Json& c) { return c.is_number(); })) {
            float* out[] = {&value.x, &value.y, &value.z, &value.w};
            for (size_t i = 0; i < v.size(); i++) *out[i] = v[i].get<float>();
        } else if (!v.is_string()) {
            // a "#RRGGBB" colour is read below, once the shader's manifest is known
            call->fail("value must be a number or [x,y], [x,y,z], [x,y,z,w] (a colour parameter also takes \"#RRGGBB[AA]\", sRGB); "
                       "or pass remove: true.");
            return;
        }
    }
    Project* project = call->ctx().project();
    Scene* scene = where.scene->scene;
    const uint32_t sceneId = where.scene->id;
    Json declaredMembers = Json::array();
    Json warnings;
    bool declared = false;
    if (pass) {
        std::vector<PostProcessPass> passes = scene->getPostProcessPasses();
        int index;
        if (!passIndexArg(call, passes, index)) return;
        if (remove) {
            if (!passes[index].removeUniform(name)) {
                call->fail("Pass " + std::to_string(index) + " has no value '" + name + "'.");
                return;
            }
        } else {
            passes[index].setUniform(name, value);
        }
        if (!addSceneCommand(sceneId, new ScenePropertyCmd<std::vector<PostProcessPass>>(project, sceneId, "post_process", passes))) {
            call->fail("Could not change the pass.");
            return;
        }
        bool buildFailed = false;
        passUniformMembers(declaredMembers, warnings, passes[index], buildFailed);
        const std::string note = remove ? "" : uniformSetNote(declaredMembers, name, declared);
        call->ok((remove ? "Removed " : "Set ") + name + " on post-process pass " + std::to_string(index) + "." + note,
                 Json{{"pass_index", index}, {"name", name}, {"declared", declared}});
        return;
    }
    ShaderType shaderType;
    if (!resolveShaderComponent(call, where, shaderType)) return;
    int submesh = -1;
    if (!materialParamsTarget(call, scene, where.entity, where.component, submesh)) return;
    // a submesh material's parameters, or the component's shaderUniforms
    const std::string property = submesh >= 0 ? "submeshes[" + std::to_string(submesh) + "].material.shaderParams" : "shaderUniforms";
    ShaderUniformValues* current = Catalog::getPropertyRef<ShaderUniformValues>(scene, where.entity, where.component, property);
    if (!current) {
        call->fail("The component has no shader uniforms.");
        return;
    }
    const std::string shader = Catalog::getShaderReferenceForProperty(scene, where.entity, where.component, property);
    const std::shared_ptr<const ShaderManifest> manifest = McpValues::findManifest(shader);
    const ShaderManifestProperty* declaredProperty = manifest ? manifest->findProperty(name) : nullptr;
    if (!remove && ShaderReference::isLibrary(shader) && !manifest) {
        call->fail(shader + " is not a library shader the engine has; the library shaders: " + McpValues::libraryShaderNamesText() + ".");
        return;
    }
    if (!remove && ShaderReference::isLibrary(shader) && manifest && !declaredProperty) {
        std::string names;
        for (const ShaderManifestProperty& p : manifest->properties) names += (names.empty() ? "" : ", ") + p.member;
        call->fail(shader + " has no parameter '" + name + "'; its parameters: " + (names.empty() ? "(none)" : names) + ".");
        return;
    }
    if (!remove) {
        std::string why;
        if (!McpValues::shaderParamFromJson(declaredProperty, name, rawValue, value, why)) {
            call->fail(why);
            return;
        }
    }
    ShaderUniformValues values = *current;
    if (remove) {
        if (!ShaderUniforms::remove(values, name)) {
            call->fail("The component has no value '" + name + "'.");
            return;
        }
    } else {
        ShaderUniforms::set(values, name, value);
    }
    if (!addSceneCommand(sceneId, new PropertyCmd<ShaderUniformValues>(project, sceneId, where.entity, where.component,
                                                                       property, values))) {
        call->fail("Could not change the uniform.");
        return;
    }
    if (submesh >= 0) {
        const Submesh& sub = scene->getComponent<MeshComponent>(where.entity).submeshes[submesh];
        for (const CustomUniformBlock* block : {&sub.customVSParams, &sub.customFSParams, &sub.customVSDepthParams, &sub.customFSDepthParams}) {
            appendUniformMembers(declaredMembers, *block, values);
        }
    } else {
        componentUniformMembers(declaredMembers, warnings, scene, where.entity, where.component, values);
    }
    // a manifest parameter is declared even while its variant still builds
    const std::string note = remove || declaredProperty ? "" : uniformSetNote(declaredMembers, name, declared);
    declared = declared || declaredProperty != nullptr;
    Json result = {{"entity", McpRefs::entityJson(where.scene, where.entity)}, {"component", McpRefs::componentName(where.component)},
                   {"property", property}, {"name", name}, {"declared", declared}};
    if (!remove) result["value"] = vec4Array(value);
    call->ok((remove ? "Removed " : "Set ") + name + " on " + McpRefs::entityPath(where.scene, where.entity) +
             (submesh >= 0 ? " (submesh " + std::to_string(submesh) + " material)" : std::string()) + "." + note, result);
}

void listShadersTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string type = args.value("type", std::string());
    // the types a library shader may have (a filter outside them would silently list nothing)
    static const char* const shaderTypes[] = {"mesh", "ui", "points", "lines", "sky", "postprocess"};
    if (!type.empty() && std::none_of(std::begin(shaderTypes), std::end(shaderTypes), [&](const char* t) { return type == t; })) {
        call->fail("Unknown shader type \"" + type + "\". Valid types: mesh, ui, points, lines, sky, postprocess (sprites and "
                   "tilemaps are meshes: their shader is a mesh shader).");
        return;
    }
    std::string name = args.value("name", std::string());
    if (!name.empty() && !ShaderReference::isLibrary(name)) name = ShaderReference::LIBRARY_PREFIX + name;
    const bool detail = args.value("detail", !name.empty());
    Json shaders = Json::array();
    std::string text;
    for (const std::string& libraryName : ShaderManifests::getLibraryShaderNames()) {
        const std::string reference = ShaderReference::LIBRARY_PREFIX + libraryName;
        if (!name.empty() && ShaderReference::getBase(name) != reference) continue;
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(reference);
        if (!manifest) continue;
        if (!type.empty() && ShaderManifest::getShaderTypeToken(manifest->type) != type) continue;
        Json entry = McpValues::manifestToJson(reference, *manifest, detail);
        text += "\n" + reference + " (" + ShaderManifest::getShaderTypeToken(manifest->type) +
                (manifest->category.empty() ? "" : ", " + manifest->category) + "): " + manifest->description;
        // a shader reading the scene (phase 3) costs passes per camera: said on its line
        if (entry.contains("cost_note"))
            text += " [" + Catalog::getShaderSceneInputsSummary(*manifest) + ": " + Catalog::getSceneInputsCost(manifest->getPossibleSceneInputs(), true) + "]";
        shaders.push_back(std::move(entry));
    }
    if (!name.empty() && shaders.empty()) {
        call->fail("No library shader " + name + ". The library shaders: " + McpValues::libraryShaderNamesText() + ".");
        return;
    }
    call->ok(std::to_string(shaders.size()) + " library shader(s)" + (detail ? "" : " (detail: true or name for parameters)") + ":" + text,
             Json{{"shaders", shaders}});
}

void checkShaderTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::vector<ProjectChecks::ShaderUse> forks;
    // a project UI effect (phase 5): its .frag, checked in each of its forms
    if (args.contains("shader") && args["shader"].is_string() &&
        ProjectChecks::isUiEffectFile(project->getProjectPath() / fs::u8path(args["shader"].get<std::string>()))) {
        forks = ProjectChecks::uiEffectUses(project, args["shader"].get<std::string>());
    } else if (args.contains("shader") && args["shader"].is_string()) {
        std::string base = args["shader"].get<std::string>();
        const bool library = ShaderReference::isLibrary(base);
        for (const char* ext : {".vert", ".frag"}) {
            if (!library && base.size() > 5 && base.compare(base.size() - 5, 5, ext) == 0 && base.find('|') == std::string::npos) base = base.substr(0, base.size() - 5);
        }
        if (library) {
            // a library shader ("library:<name>", optionally "#FEATURES") is in the engine
            std::string vert, frag, error;
            if (!ShaderVariantCompiler::getLibraryShaderPaths(base, ShaderType::MESH, vert, frag, &error)) {
                call->fail(error + ". The library shaders: " + [] {
                    std::string names;
                    for (const std::string& name : ShaderManifests::getLibraryShaderNames()) names += (names.empty() ? "library:" : ", library:") + name;
                    return names;
                }() + ".");
                return;
            }
        } else {
            // project-relative, else under the assets directory (Project::findShaderFile)
            Util::CustomShaderPaths paths = project->resolveCustomShaderFiles(base);
            std::error_code ec;
            if (!fs::exists(project->getProjectPath() / paths.vert, ec) || !fs::exists(project->getProjectPath() / paths.frag, ec)) {
                call->fail("Shader files not found: " + paths.vert + ", " + paths.frag + ".");
                return;
            }
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
        } else if (!library) {
            for (const ProjectChecks::ShaderUse& use : ProjectChecks::shaderForksUsingFiles(project, {project->resolveCustomShaderFiles(base).vert})) {
                if (use.base == base) type = use.shaderType;
            }
        }
        if (library) {
            // the variant the engine would draw: the manifest's default features and keyword
            // values added to the ones named, and (a colour program) the blend define of its
            // default state, or of a BLEND_* the reference names
            std::vector<std::string> requested = ShaderReference::getFeatures(base);
            MaterialBlendMode blend = MaterialBlendMode::AUTO;
            static const std::pair<const char*, MaterialBlendMode> blends[] = {
                {"BLEND_NONE", MaterialBlendMode::NONE}, {"BLEND_ALPHA", MaterialBlendMode::ALPHA},
                {"BLEND_PREMULTIPLIED", MaterialBlendMode::PREMULTIPLIED}, {"BLEND_ADDITIVE", MaterialBlendMode::ADDITIVE},
                {"BLEND_MULTIPLY", MaterialBlendMode::MULTIPLY}};
            for (const auto& [define, mode] : blends) {
                if (std::find(requested.begin(), requested.end(), define) != requested.end()) blend = mode;
            }
            const std::string shaderBase = ShaderReference::getBase(base);
            base = (type == static_cast<int>(ShaderType::DEPTH))
                ? ShaderReference::getVariantReference(shaderBase, requested)
                : ShaderReference::getColorVariantReference(shaderBase, requested, blend);
        }
        forks.push_back({base, type});
    } else {
        // every fork in use
        forks = ProjectChecks::shaderForksUsingFiles(project, {"x.glsl"});
        if (forks.empty()) {
            call->ok("No custom shader is used by the loaded scenes.", Json{{"results", Json::array()}, {"failed", 0}, {"findings", Json::array()}});
            return;
        }
    }
    // with every shader in use: the materials reading the scene from the opaque queue (phase 3)
    const std::vector<ProjectChecks::Finding> stateFindings = args.contains("shader")
        ? std::vector<ProjectChecks::Finding>() : ProjectChecks::sceneInputFindings(project);
    checkShadersWithFindings(call, forks, [call, stateFindings](Json results, Json findings) {
        for (const ProjectChecks::Finding& finding : stateFindings) findings.push_back(ProjectChecks::toJson(finding));
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
        for (const ProjectChecks::Finding& finding : stateFindings) text += "\nerror: " + finding.message;
        call->ok((failed ? std::to_string(failed) + " shader(s) failed to compile:" : std::string("All shaders compile:")) + text,
                 Json{{"results", results}, {"failed", failed}, {"findings", findings}});
    });
}

void setTextureSettingsTool(const CallPtr& call) {
    const Json& args = call->args();
    Target where;
    if (!resolveTarget(call, where, true)) return;
    if (!where.hasComponent) {
        call->fail("component is required (e.g. sprite, mesh, image).");
        return;
    }
    Scene* scene = where.scene->scene;
    const std::string propertyName = args.value("property", "");
    auto properties = Catalog::findEntityProperties(scene, where.entity, where.component);
    auto it = properties.find(propertyName);
    if (it == properties.end() || !it->second.ref || it->second.type != PropertyType::Texture) {
        std::string names;
        for (const auto& entry : properties) {
            if (entry.second.type == PropertyType::Texture && entry.second.ref) names += (names.empty() ? "" : ", ") + entry.first;
        }
        call->fail("property must be a texture property of " + McpRefs::componentName(where.component) + ": " +
                   (names.empty() ? "it has none" : names) + ".");
        return;
    }
    Texture* current = static_cast<Texture*>(it->second.ref);
    Texture modified = *current;
    // A value counts only when it round-trips: the Stream converters fall back silently
    for (const char* field : {"min_filter", "mag_filter"}) {
        if (!args.contains(field)) continue;
        const std::string value = args[field].is_string() ? args[field].get<std::string>() : "";
        const TextureFilter filter = Stream::stringToTextureFilter(value);
        const bool mag = std::string(field) == "mag_filter";
        if (Stream::textureFilterToString(filter) != value || (mag && filter != TextureFilter::NEAREST && filter != TextureFilter::LINEAR)) {
            call->fail(std::string("Invalid ") + field + " \"" + value + "\"" + (mag ? " (nearest or linear)." : "."));
            return;
        }
        if (mag) modified.setMagFilter(filter);
        else modified.setMinFilter(filter);
    }
    for (const char* field : {"wrap_u", "wrap_v"}) {
        if (!args.contains(field)) continue;
        const std::string value = args[field].is_string() ? args[field].get<std::string>() : "";
        const TextureWrap wrap = Stream::stringToTextureWrap(value);
        if (Stream::textureWrapToString(wrap) != value) {
            call->fail(std::string("Invalid ") + field + " \"" + value + "\".");
            return;
        }
        if (std::string(field) == "wrap_v") modified.setWrapV(wrap);
        else modified.setWrapU(wrap);
    }
    if (args.contains("svg_scale")) {
        const float scale = args["svg_scale"].is_number() ? args["svg_scale"].get<float>() : 0.0f;
        if (!(scale > 0.0f)) {
            call->fail("svg_scale must be a number > 0.");
            return;
        }
        if (!TextureData::hasSvgExtension(modified.getPath().c_str())) {
            call->fail("svg_scale only applies to .svg textures.");
            return;
        }
        modified.setSvgScale(scale);
    }
    Json data = {{"entity", McpRefs::entityJson(where.scene, where.entity)}, {"component", McpRefs::componentName(where.component)},
                 {"property", propertyName}};
    if (modified == *current) {
        call->ok("The texture settings already match.", data);
        return;
    }
    // Sampler settings are baked into the GPU texture: drop the cached render on execute and undo
    const Entity entity = where.entity;
    const ComponentType component = where.component;
    auto onChanged = [scene, entity, component, propertyName]() {
        if (Texture* ref = Catalog::getPropertyRef<Texture>(scene, entity, component, propertyName)) ref->invalidateRender();
    };
    if (!addSceneCommand(where.scene->id, new PropertyCmd<Texture>(call->ctx().project(), where.scene->id, entity, component,
                                                                   propertyName, modified, onChanged))) {
        call->fail("Could not change the texture settings.");
        return;
    }
    call->ok("Set the texture settings of " + McpRefs::componentName(component) + "." + propertyName + ".", data);
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
    // project-relative like the paths of set_properties / create_entity, which only take assets
    if (!project->isInsideAssetsPath(file)) {
        const std::string assets = projectRelative(project, project->getAssetsPath());
        call->fail("path is project-relative, like the texture paths the other tools take: \"" + rel +
                   "\" is outside the assets directory (\"" + assets + "\"), so no component could use it. Use \"" +
                   projectRelative(project, project->getAssetsPath() / fs::path(rel)) + "\".");
        return;
    }
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
                                                "): reference it as \"" + rel + "\".",
                                            Json{{"path", rel}, {"width", width}, {"height", height}});
    if (encodePng(pixels.data(), width, height, 256, base64, pw, ph)) result.images.push_back({base64, "image/png"});
    call->reply(std::move(result));
}

// ------------------------------------------------------------------ search_files

// Searches the project's text files (what read_file reads): literal or ECMAScript regex, per line
void searchFilesTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string pattern = args.value("pattern", "");
    if (pattern.empty()) {
        call->fail("pattern is required.");
        return;
    }
    const bool useRegex = args.value("regex", false);
    const bool caseSensitive = args.value("case_sensitive", true);
    const std::string glob = args.value("glob", "");
    const int context = std::clamp(args.value("context", 0), 0, 10);
    const size_t maxResults = static_cast<size_t>(std::clamp(args.value("max_results", 50), 1, 500));
    constexpr uintmax_t maxFileSize = 2 * 1024 * 1024;
    constexpr size_t maxText = 300;

    const fs::path root = project->getProjectPath();
    std::error_code ec;
    const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
    auto insideProject = [&](const fs::path& path) {
        std::error_code e;
        const fs::path canonical = fs::weakly_canonical(path, e);
        return !e && Util::isInsidePath(canonical, canonicalRoot);
    };

    // the filter: a glob over project-relative paths (file names when it has no '/'), or a file/folder
    fs::path scope = root;
    std::string fileGlob = glob;
    if (!glob.empty()) {
        const fs::path g(glob);
        if (g.is_absolute() || g.has_root_name() || g.has_root_directory()) {
            call->fail("glob must be relative to the project (no absolute paths).");
            return;
        }
        for (const fs::path& part : g.lexically_normal()) {
            if (part == "..") {
                call->fail("glob must stay inside the project (no '..').");
                return;
            }
        }
        if (glob.find_first_of("*?") == std::string::npos) {
            const fs::path target = (root / g).lexically_normal();
            if (fs::is_directory(target, ec)) {
                scope = target;
                fileGlob.clear();
            } else {
                fileGlob = g.lexically_normal().generic_string();
            }
        }
    }

    std::regex expression;
    if (useRegex) {
        try {
            auto flags = std::regex::ECMAScript;
            if (!caseSensitive) flags |= std::regex::icase;
            expression = std::regex(pattern, flags);
        } catch (const std::regex_error& e) {
            call->fail(std::string("Invalid regex (ECMAScript syntax): ") + e.what() + ". Or pass regex: false for literal text.");
            return;
        }
    }
    const std::string needle = caseSensitive ? pattern : lowerCase(pattern);
    auto matches = [&](const std::string& line) {
        if (useRegex) {
            // very long lines (minified data) would make std::regex recurse too deep
            return line.size() <= 4000 && std::regex_search(line, expression);
        }
        return (caseSensitive ? line : lowerCase(line)).find(needle) != std::string::npos;
    };
    auto clip = [&](const std::string& text) { return text.size() > maxText ? text.substr(0, maxText) + "..." : text; };

    std::vector<fs::path> files;
    const fs::path scopeRel = fs::path(projectRelative(project, scope));
    const bool scopeSkipped = scope != root && (ProjectChecks::isSkippedDir(scope) || ProjectChecks::isInSkippedDir(scopeRel));
    if (!scopeSkipped && insideProject(scope) && fs::is_directory(scope, ec)) {
        for (fs::recursive_directory_iterator it(scope, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec;
             it.increment(ec)) {
            const fs::path& path = it->path();
            const std::string name = path.filename().string();
            if (it->is_directory(ec)) {
                if (ProjectChecks::isSkippedDir(path)) it.disable_recursion_pending();
                continue;
            }
            if (name.empty() || name[0] == '.' || !it->is_regular_file(ec) || !isTextFile(path)) continue;
            if (it->is_symlink(ec) && !insideProject(path)) continue;
            if (!fileGlob.empty()) {
                const std::string rel = projectRelative(project, path);
                if (!globMatch(fileGlob, fileGlob.find('/') != std::string::npos ? rel : name)) continue;
            }
            files.push_back(path);
        }
    }
    std::sort(files.begin(), files.end());

    Json found = Json::array();
    size_t total = 0, searched = 0;
    bool timedOut = false;
    std::string text;
    const auto started = std::chrono::steady_clock::now();
    for (const fs::path& file : files) {
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(10)) {
            timedOut = true;
            break;
        }
        if (fs::file_size(file, ec) > maxFileSize || ec) continue;
        std::string content;
        if (!ProjectChecks::readFile(file, content)) continue;
        if (content.find('\0') != std::string::npos) continue;   // binary
        searched++;
        std::vector<std::string> lines;
        std::istringstream in(content);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
        const std::string rel = projectRelative(project, file);
        for (size_t i = 0; i < lines.size(); i++) {
            if (!matches(lines[i])) continue;
            total++;
            if (found.size() >= maxResults) continue;
            Json item = {{"path", rel}, {"line", i + 1}, {"text", clip(lines[i])}};
            if (context > 0) {
                Json before = Json::array(), after = Json::array();
                for (size_t j = i >= static_cast<size_t>(context) ? i - context : 0; j < i; j++) {
                    before.push_back(clip(lines[j]));
                    text += rel + "-" + std::to_string(j + 1) + "- " + clip(lines[j]) + "\n";
                }
                text += rel + ":" + std::to_string(i + 1) + ": " + clip(lines[i]) + "\n";
                for (size_t j = i + 1; j < lines.size() && j <= i + static_cast<size_t>(context); j++) {
                    after.push_back(clip(lines[j]));
                    text += rel + "-" + std::to_string(j + 1) + "- " + clip(lines[j]) + "\n";
                }
                item["context"] = {{"before", before}, {"after", after}};
                text += "--\n";
            } else {
                text += rel + ":" + std::to_string(i + 1) + ": " + clip(lines[i]) + "\n";
            }
            found.push_back(item);
        }
    }
    const bool truncated = total > found.size() || timedOut;
    std::string header = std::to_string(total) + " match(es) in " + std::to_string(searched) + " file(s) searched";
    if (total > found.size()) header += " (showing " + std::to_string(found.size()) + ": raise max_results or narrow glob)";
    if (timedOut) header += " (stopped after 10 s: narrow glob)";
    call->ok(header + (text.empty() ? "." : ":\n" + text),
             Json{{"matches", found}, {"total", total}, {"truncated", truncated}, {"files_searched", searched}});
}

} // namespace

void registerAssetTools(McpRegistry& registry) {
    using namespace schema;

    const std::vector<std::string> types = {"any", "texture", "model", "audio", "font", "html", "css", "script", "scene", "prefab", "material", "shader", "folder", "other"};

    {
        ToolDef tool;
        tool.name = "list_assets";
        tool.title = "List Assets";
        tool.description =
            "List/search project files as the Project window shows them. pattern globs the file name, or the path "
            "with a '/' (\"assets/**/enemy*.png\"). Returns project-relative paths (asset refs), the assets and Lua "
            "dirs, total/truncated.";
        tool.inputSchema = object({
            {"directory", assetPath("Default: project root.")},
            {"type", stringEnum("", types)},
            {"pattern", string("")},
            {"query", string("Name substring.")},
            {"recursive", withDefault(boolean(""), true)},
            {"details", withDefault(boolean("Add sizes and image dimensions."), false)},
            {"limit", withDefault(integer("", 1, 2000), 200)}
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
            "One project file's details by type: image size/channels; model meshes, materials, animations, nodes; "
            "WAV format/duration; .material fields, linked submeshes; shader includes, forks using it; Lua syntax; "
            "scene status; unsaved code editor edits.";
        tool.inputSchema = object({{"path", assetPath("")}}, {"path"});
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
            "See an image asset as PNG image content; models, materials and prefabs give their Project window "
            "thumbnail (may take seconds). SVGs are not rasterized.";
        tool.inputSchema = object({
            {"path", assetPath("")},
            {"max_size", withDefault(integer("Longest side, px.", 16, 2048), 512)}
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
            "Read a project text file (as on disk); the result text is the content. For clients without file access "
            "(remote HTTP).";
        tool.inputSchema = object({
            {"path", assetPath("")},
            {"start_line", withDefault(integer("1-based.", 1, 100000000), 1)},
            {"max_lines", withDefault(integer("", 1, 20000), 2000)}
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
            "Create or change a project text file (undo scope project; for clients without file access) and validate "
            "it: Lua syntax, forks using it recompiled (errors with lines), materials parsed. content = whole file, "
            "or edits [{old_text, new_text}] (old_text matches once unless replace_all). Refuses project.yaml, "
            ".scene, .prefab and files with unsaved code editor edits.";
        tool.inputSchema = object({
            {"path", assetPath("Parent folders are created.")},
            {"content", string("")},
            {"edits", array(object({
                {"old_text", string("")},
                {"new_text", string("")},
                {"replace_all", withDefault(boolean(""), false)}
            }, {"old_text", "new_text"}), "")},
            {"check", withDefault(boolean("Compile affected shader forks."), true)},
            {"discard_unsaved_buffer", withDefault(boolean(""), false)}
        }, {"path"});
        tool.handler = writeFileTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "create_folder";
        tool.title = "Create Folder";
        tool.description = "Create a project folder (its parent must exist).";
        tool.inputSchema = object({{"path", assetPath("")}}, {"path"});
        tool.annotations.idempotent = true;
        tool.handler = createFolderTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "move_asset";
        tool.title = "Move / Rename / Copy Asset";
        tool.description =
            "Rename/move a file or folder, updating references in scenes, prefabs, materials, script entries and "
            "shader forks (as the Project window does); or copy it (a copied prefab gets a new GUID). The "
            "destination must not exist; its folder must.";
        tool.inputSchema = object({
            {"path", assetPath("")},
            {"to", assetPath("New path (folder + name).")},
            {"copy", withDefault(boolean(""), false)}
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
            "Move files/folders to the project trash (undo restores them). References become missing (instances turn "
            "into Missing Prefabs). Open scenes cannot be deleted.";
        tool.inputSchema = object({{"paths", array(string(""), "")}}, {"paths"});
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
            "Copy files/folders from absolute local paths into a project folder (created if missing), like dropping "
            "them on the Project window; assets go under the assets dir. Remote: URLs in sources, fonts "
            "[\"Inter:400,700\"] (Google Fonts -> .ttf + @font-face css), icons [\"lucide:play\"] (.svg), "
            "from_document (import every remote URL a .html/.css uses and rewrite it). Videos become AV1+Opus .webm (FFmpeg).";
        tool.inputSchema = object({
            {"sources", array(string(""), "Absolute paths or http(s) URLs.")},
            {"to_dir", assetPath("e.g. \"assets/sprites\" (remote default assets/ui/imported).")},
            {"overwrite", withDefault(boolean(""), false)},
            {"fonts", array(string(""), "")},
            {"icons", array(string(""), "")},
            {"from_document", assetPath(".html/.css")},
            {"rewrite", withDefault(boolean(""), true)}
        });
        tool.annotations.openWorld = true;
        tool.handler = importFilesTool;
        registry.addTool(std::move(tool));
    }

    const Json materialValues = anyObject(
        "baseColorFactor [r,g,b,a], metallicFactor, roughnessFactor, alphaMode, alphaCutoff, emissiveFactor, "
        "foliageTransmission, *Texture (path|null), *TexCoord, stochasticTiling (Lit: hides tiling repetition, up to 3x the "
        "reads) with stochasticCellSize (0.25-4) / BlendSharpness / Contrast / Rotation (0-1); shader (\"library:<name>\"), shaderParams {member: value}, "
        "shaderTextures {uniform: path}, shaderFeatures [defines]|{define: bool}, blendMode, cullMode, depthWrite, depthTest, "
        "depthOffsetFactor/Units, sortPriority, lighting, stencil {compare, ref, readMask, writeMask, pass, fail, depthFail}, "
        "colorWrite, extraPass {shader, its params, state, stencil, order}; null removes (jasper://docs/shaders).");

    {
        ToolDef tool;
        tool.name = "create_material";
        tool.title = "Create Material";
        tool.description =
            "Create a shared .material file (linked submeshes follow its edits) from defaults or a mesh submesh's "
            "material (entity + submesh; linked unless link=false), plus values.";
        tool.inputSchema = object({
            {"path", assetPath("New .material path.")},
            {"values", materialValues},
            {"scene", sceneRef()},
            {"entity", entityRef("Mesh to copy from (and link).")},
            {"submesh", withDefault(integer("", 0, 1024), 0)},
            {"link", boolean("Default: true with entity.")},
            {"overwrite", withDefault(boolean(""), false)}
        }, {"path"});
        tool.handler = createMaterialTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "update_material";
        tool.title = "Update Material";
        tool.description =
            "Change the given fields of a .material file; linked submeshes update. Current fields: get_asset_info.";
        tool.inputSchema = object({
            {"path", assetPath("")},
            {"values", anyObject("The fields to change, as create_material's values (null removes).")}
        }, {"path", "values"});
        tool.handler = updateMaterialTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "link_material";
        tool.title = "Link Material";
        tool.description =
            "Link a mesh submesh to a .material file, or unlink it (material null: it keeps a copy of the values). "
            "Its own values: set_properties submeshes[i].material.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"submesh", withDefault(integer("", 0, 1024), 0)},
            {"material", {{"type", {"string", "null"}}, {"description", ".material path, or null to unlink."}}}
        }, {"entity"});
        tool.allowedWhilePlaying = false;
        tool.handler = linkMaterialTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "list_material_presets";
        tool.title = "List Material Presets";
        tool.description =
            "One-click material setups (outlines, see-through silhouette, stencil masks, draw-once transparency...): "
            "id, name, description, the fields each sets. Apply with apply_material_preset.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listMaterialPresetsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "apply_material_preset";
        tool.title = "Apply Material Preset";
        tool.description =
            "Apply a material preset (list_material_presets) in one undo step to submesh materials (entity/entities, "
            "all submeshes unless submesh) or a .material file (path: linked submeshes follow; undo scope project). "
            "Only its fields change (a linked submesh gets its own material); fine-tune with set_properties "
            "submeshes[i].material.*.";
        tool.inputSchema = object({
            {"preset", string("Preset id, e.g. \"outline_hull\".")},
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"submesh", integer("One submesh (default: every submesh).", 0, 1024)},
            {"path", assetPath("A .material file instead of entities.")}
        }, {"preset"});
        tool.allowedWhilePlaying = false;
        tool.handler = applyMaterialPresetTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "fork_shader";
        tool.title = "Fork Shader";
        tool.description =
            "Copy a built-in shader into the project (<directory>/<name>.vert/.frag) and point a target at it: "
            "component (a renderable's customShader), depth (a mesh's shadow/depth pass; needed when a mesh fork "
            "displaces or discards), scene_default (shader_type, for components without their own), post_process "
            "(pass_index, or omit to add a pass). Parameters: set_shader_uniform.";
        tool.inputSchema = object({
            {"target", stringEnum("Default: component when entity is given.", {"component", "depth", "scene_default", "post_process"})},
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"component", componentName("Default: auto-detect.")},
            {"shader_type", stringEnum("", {"mesh", "ui", "sky", "points", "lines"})},
            {"pass_index", integer("")},
            {"directory", withDefault(assetPath(""), "shaders")},
            {"name", string("Default: from the GameObject / scene.")},
            {"fork_includes", withDefault(boolean("Also copy the engine .glsl includes."), false)}
        });
        tool.allowedWhilePlaying = false;
        tool.handler = forkShaderTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_shader_uniforms";
        tool.title = "Get Shader Uniforms";
        tool.description =
            "A custom or library shader's parameters (u_vs/u_fs_customParams, a pass's u_fs_postParams): GLSL type, "
            "engine-written (time, resolution), value; library shaders add label, kind, default. entity (+ "
            "component, + submesh) or pass_index; known once compiled.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"component", componentName("Default: auto-detect.")},
            {"submesh", integer("A submesh's material (default 0 when its material has a shader).")},
            {"pass_index", integer("A post-process pass instead.")}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getShaderUniformsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_shader_uniform";
        tool.title = "Set Shader Uniform";
        tool.description =
            "Set or remove one parameter of a custom or library shader by member (entity (+ component, + submesh for "
            "a material) or pass_index). Undeclared names are stored but unused (the result says so).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"component", componentName("Default: auto-detect.")},
            {"submesh", integer("A submesh's material (default 0 when its material has a shader).")},
            {"pass_index", integer("A post-process pass instead.")},
            {"name", string("")},
            {"value", {{"type", {"number", "array", "string"}}, {"items", {{"type", "number"}}},
                       {"description", "Number or [x,y(,z(,w))] (colours linear), or \"#RRGGBB[AA]\" (sRGB) for a colour."}}},
            {"remove", withDefault(boolean(""), false)}
        }, {"name"});
        tool.handler = setShaderUniformTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "list_shaders";
        tool.title = "List Shaders";
        tool.description =
            "The engine's library shaders (\"library:<name>\"), to pick before forking: type (mesh: a material's "
            "shader; ui, points, lines, sky: a component's customShader; postprocess: a post_process pass) and "
            "lighting modes; detail (or name) adds parameters, textures, features/keywords, required inputs, default "
            "state and their rules; scene_inputs/cost_note: reads the scene (an extra pass per camera). Using them: "
            "jasper://docs/shaders.";
        tool.inputSchema = object({
            {"type", stringEnum("", {"mesh", "ui", "points", "lines", "sky", "postprocess"})},
            {"name", string("One shader (\"toon\" or \"library:toon\"); implies detail.")},
            {"detail", withDefault(boolean("Parameters, textures, features, keywords, inputs, state and their rules."), false)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listShadersTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "check_shader";
        tool.title = "Check Shader";
        tool.description =
            "Compile shader forks or library shaders now: errors (line, message, log). shader: a customShader base "
            "(\"shaders/Water\"), its .vert/.frag, or \"library:<name>[#FEATURES]\"; shader_type if unused yet. No "
            "shader: every custom and library shader in use. A failing shader draws as the built-in one.";
        tool.inputSchema = object({
            {"shader", string("")},
            {"shader_type", stringEnum("", {"mesh", "ui", "sky", "points", "lines", "depth", "post_process"})}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = checkShaderTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_texture_settings";
        tool.title = "Set Texture Settings";
        tool.description =
            "Sampler settings of a component's texture property (on the property, e.g. sprite texture or "
            "submeshes[0].material.baseColorTexture, not the file). Only given fields change; mag_filter nearest for "
            "pixel art.";
        const std::vector<std::string> wraps = {"repeat", "mirrored_repeat", "clamp_to_edge", "clamp_to_border"};
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"component", componentName()},
            {"property", string("Texture property path.")},
            {"min_filter", stringEnum("", {"nearest", "linear", "nearest_mipmap_nearest", "nearest_mipmap_linear",
                                           "linear_mipmap_nearest", "linear_mipmap_linear"})},
            {"mag_filter", stringEnum("", {"nearest", "linear"})},
            {"wrap_u", stringEnum("", wraps)},
            {"wrap_v", stringEnum("", wraps)},
            {"svg_scale", number(".svg rasterization scale.")}
        }, {"entity", "component", "property"});
        tool.annotations.idempotent = true;
        tool.handler = setTextureSettingsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "generate_placeholder_art";
        tool.title = "Generate Placeholder Art";
        tool.description =
            "An antialiased placeholder PNG (no art yet) and its preview: a shape with fill color, optional outline "
            "and background (default transparent); gradient: color -> color2, checker alternates them. Colors "
            "[r,g,b(,a)] 0-1 or \"#RRGGBB(AA)\".";
        tool.inputSchema = object({
            {"path", assetPath("A .png inside the assets dir, e.g. \"assets/sprites/coin.png\" (then a texture path).")},
            {"width", withDefault(integer("", 1, 2048), 64)},
            {"height", withDefault(integer("", 1, 2048), 64)},
            {"shape", withDefault(stringEnum("", {"rect", "rounded_rect", "circle", "ellipse", "triangle", "diamond", "gradient", "checker"}), "rect")},
            {"color", any("")},
            {"color2", any("")},
            {"outline_width", withDefault(number("px", 0, 512), 0)},
            {"outline_color", any("Default black.")},
            {"background", any("")},
            {"corner_radius", number("rounded_rect, px.")},
            {"direction", withDefault(stringEnum("", {"vertical", "horizontal"}), "vertical")},
            {"cells", withDefault(integer("checker cells per side.", 1, 256), 8)},
            {"overwrite", withDefault(boolean(""), false)}
        }, {"path"});
        tool.handler = placeholderArtTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "search_files";
        tool.title = "Search Files";
        tool.description =
            "Grep project text files (scripts, shaders, docs, configs), for HTTP clients without file access. "
            "ECMAScript regex; matches [{path, line, text, context}].";
        tool.inputSchema = object({
            {"pattern", string("")},
            {"regex", boolean("")},
            {"case_sensitive", withDefault(boolean(""), true)},
            {"glob", string("\"*.lua\", \"scripts/**\" or a path.")},
            {"context", integer("", 0, 10)},
            {"max_results", withDefault(integer("", 1, 500), 50)}
        }, {"pattern"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = searchFilesTool;
        registry.addTool(std::move(tool));
    }
}

}
