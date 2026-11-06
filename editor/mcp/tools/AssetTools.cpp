// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W4: assets, import, materials, shaders, text files, placeholder art
#include "asset/AssetOps.h"
#include "io/watch/LinkedFolders.h"
#include "asset/AssetTypes.h"
#include "camera/ShakePreset.h"
#include "camera/ShakeInstance.h"
#include "media/VideoFormats.h"
#include "media/VideoSourceFactory.h"
#include "McpTools.h"
#include "mcp/McpDataSafety.h"
#include "DocumentFile.h"
#include "util/ShaderGlobalsJson.h"
#include "shader/ShaderGlobals.h"
#include "SoundShared.h"
#include "W4Shared.h"
#include "audio/BuiltinSoundImport.h"
#include "AnimationClipTools.h"
#include "AnimatorTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "App.h"
#include "jobs/JobSystem.h"
#include "Catalog.h"
#include "Out.h"
#include "Project.h"
#include "Stream.h"
#include "animation/AnimationClipDocument.h"
#include "animation/ModelSubClips.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/CommandNames.h"
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
#include "vat/VatAssetFiles.h"
#include "vat/VatDocument.h"
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
#include "util/AssetDatabase.h"
#include "util/FbxImporter.h"
#include "model/ModelAudit.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "util/ProjectChecks.h"
#include "util/ProjectUtils.h"
#include "util/Util.h"
#include "subsystem/RenderSystem.h"
#include "window/CodeEditor.h"
#include "window/ProjectWindow.h"

#include "stb_image.h"
#include "unity/UnityParticleImport.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"
#include "yaml-cpp/yaml.h"
#include "util/FileUtils.h"
#include "command/type/SetAssetMetaCmd.h"
#include "texture/TextureCookReport.h"
#include "texture/TextureDecode.h"

#include <algorithm>
#include <cctype>
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

// Compiles the forks as a Background Heavy job (ProjectChecks::runShaderCheck); `done` gets, on the main
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
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.gate = jobs::GateClass::Heavy;
    options.name = "mcp-shader-check";
    jobs::submit([job, project]() {
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
    }, options);
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

// What a path argument is for: read (links followed, wherever they lead), or written / created
// (Write) or deleted / moved (Entry): then it must stay inside the project once links are resolved
// (McpRefs::resolveProjectWritePath)
enum class PathFor { Read, Write, Entry };

bool resolveExisting(const CallPtr& call, const std::string& key, fs::path& out, bool mustExist = true, PathFor use = PathFor::Read) {
    const Json& args = call->args();
    if (!args.contains(key) || !args[key].is_string() || args[key].get<std::string>().empty()) {
        call->fail(key + " is required (a project-relative path).");
        return false;
    }
    std::string error;
    const bool ok = use == PathFor::Read
        ? McpRefs::resolveProjectPath(call->ctx().project(), args[key].get<std::string>(), out, error, mustExist)
        : McpRefs::resolveProjectWritePath(call->ctx().project(), args[key].get<std::string>(), out, error, mustExist,
                                           use == PathFor::Entry ? McpRefs::WriteUse::Entry : McpRefs::WriteUse::Write);
    if (!ok) {
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
                                    ComponentType::LineRendererComponent, ComponentType::TrailRendererComponent, ComponentType::SkyboxComponent}) {
        if (McpRefs::hasComponent(target.scene, target.entity, candidate)) {
            target.component = candidate;
            target.hasComponent = true;
            ProjectChecks::shaderTypeForComponent(candidate, shaderType);
            return true;
        }
    }
    (void)scene;
    call->fail("The GameObject has no shader-capable component (mesh, ui, points, linerenderer, trailrenderer, skybox).");
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
        // the sub-clip names ("<model>#<name>" references, the loader's animation entity names):
        // "Animation <i>" when unnamed, "<name> <k>" for a later duplicate (spec 9.R11)
        std::vector<std::string> ownNames;
        for (const Json& animation : gltf["animations"]) {
            ownNames.push_back(animation.is_object() && animation.contains("name") && animation["name"].is_string()
                                   ? animation["name"].get<std::string>() : std::string());
        }
        const std::vector<std::string> clipNames = jasperengine::anim::subClipNames(ownNames);
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
            animations.push_back({{"name", clipNames[i]},
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
        {"metallicFactor", McpValues::number(material.metallicFactor)},
        {"roughnessFactor", McpValues::number(material.roughnessFactor)},
        {"alphaCutoff", McpValues::number(material.alphaCutoff)},
        {"alphaMode", alphaModeName(material.alphaMode)},
        {"emissiveFactor", McpValues::toJson(material.emissiveFactor)},
        {"foliageTransmission", McpValues::number(material.foliageTransmission)}
    };
    McpValues::materialStochasticToJson(material, data);
    for (const auto& [name, member] : materialTextures()) {
        const Texture& texture = material.*member;
        data[name] = texture.empty() ? Json(nullptr) : Json(McpValues::assetPathToJson(project, texture.getPath(0)));
    }
    for (const auto& [name, member] : materialTexCoords()) data[name] = material.*member;
    // shader library: the shader, its values and the render state
    McpValues::materialShaderLibraryToJson(project, nullptr, material, data, true);
    // the built-in Lit's emission intensity and extended lobes (those set)
    data["emissiveStrength"] = McpValues::number(material.emissiveStrength);
    if (material.pbr != MaterialPbrExtensions()) data["pbr"] = McpValues::materialPbrToJson(project, nullptr, material.pbr);
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
        } else if (key == "emissiveStrength") {
            if (!value.is_number() || !(value.get<float>() >= 0.0f) || !std::isfinite(value.get<float>())) {
                error = "emissiveStrength must be a number >= 0.";
                return false;
            }
            material.emissiveStrength = value.get<float>();
        } else if (key == "pbr") {
            // a partial object merges into the lobes the material has
            MaterialPbrExtensions pbr = material.pbr;
            if (!McpValues::materialPbrFromJson(project, nullptr, value, pbr, error)) return false;
            material.pbr = pbr;
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
                    "alphaMode, emissiveFactor, emissiveStrength, pbr, foliageTransmission, baseColorTexture, emissiveTexture, metallicRoughnessTexture, "
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
        YAML::Node node = DocumentFiles::read(file, DocumentKind::Material).root;
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

// The file's text with its identity (S04: the GUID it has, or a new one for a new file),
// formatVersion first and the existing file's unknown root keys kept (docs/data-safety.md)
std::string materialFileContent(Project* project, const fs::path& file, const Material& material) {
    Material bound = material;
    project->bindMaterialAssetRefs(bound);
    YAML::Node root = Stream::encodeMaterial(bound);
    project->prepareDocumentIdentity(root, file);
    return DocumentFiles::materialText(root, file) + "\n";
}

// A material file that did not load completely is written only with allow_data_loss: false
// after failing the call with degraded_document
bool materialWriteAllowed(const CallPtr& call, const fs::path& file, const std::string& rel) {
    if (call->args().value("allow_data_loss", false)) return true;
    const LoadReport report = DocumentFiles::materialFileReport(file);
    if (report.dropped() == 0) return true;
    failDegraded(*call, rel, report);
    return false;
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
                if (FileUtils::pathToGenericUtf8(fs::path(mesh->submeshes[i].material.name).lexically_normal()) == rel) {
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
    // search: the Project window's grammar over the asset index (S25 R20), by score then name
    if (args.contains("search") && args["search"].is_string()) {
        const size_t limit = static_cast<size_t>(std::clamp(args.value("limit", 200), 1, 2000));
        Json data = searchAssetsJson(project, args["search"].get<std::string>(), dirArg, limit, error);
        if (!error.empty()) {
            call->fail(error);
            return;
        }
        std::string text = std::to_string(data["total"].get<size_t>()) + " match(es) of " + data["query"].get<std::string>() + ":";
        for (const Json& item : data["entries"]) text += "\n" + item["path"].get<std::string>();
        call->ok(text, data);
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
        // the asset .meta sidecars go with their assets (S04): not listed
        if (!isDir && AssetTypes::isMetaFile(path)) return;
        const std::string kind = isDir ? "folder" : assetType(path);
        if (!type.empty() && type != "any" && kind != type) return;
        if (!query.empty() && lowerCase(FileUtils::pathToUtf8(path.filename())).find(query) == std::string::npos) return;
        if (!pattern.empty()) {
            const bool full = pattern.find('/') != std::string::npos;
            if (!globMatch(pattern, full ? rel : FileUtils::pathToUtf8(path.filename()))) return;
        }
        total++;
        if (entries.size() >= limit) return;
        Json item = {{"path", rel}, {"type", kind}};
        if (!isDir) {
            const Guid guid = project->getAssetDatabase().getAssetGuid(rel);
            if (!guid.isNull()) item["guid"] = guid.str();
        }
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
        for (watch::LinkedDirectoryIterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
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
    // its identity (S04): GUID, former GUIDs, a model's sub-assets
    AssetDatabase::AssetInfo identity;
    if (project->getAssetDatabase().getInfo(fs::path(rel), identity)) {
        data["guid"] = identity.guid.str();
        text += " (GUID " + identity.guid.str() + ")";
        if (!identity.formerGuids.empty()) {
            Json former = Json::array();
            for (const Guid& guid : identity.formerGuids) former.push_back(guid.str());
            data["former_guids"] = former;
        }
        if (!identity.subAssets.empty()) {
            Json subAssets = Json::array();
            for (const AssetDatabase::SubAsset& sub : identity.subAssets) {
                subAssets.push_back({{"key", sub.key}, {"guid", sub.guid.str()}, {"name", sub.name}, {"kind", subAssetKindName(sub.kind)}});
            }
            data["sub_assets"] = subAssets;
        }
    }
    if (Util::isSoundEventFile(rel)) {
        data["sound_event"] = soundEventInfo(project, file);
        const Json& problems = data["sound_event"]["problems"];
        call->ok(text + ": " + data["sound_event"]["mode"].get<std::string>() + ", " + std::to_string(data["sound_event"]["clips"].size()) +
                 " clip(s), " + std::to_string(problems.size()) + " problem(s).", data);
        return;
    }
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
        if (extensionOf(file) == ".png") {
            // a texture written by the synthesis: its recipe, whether its sources changed, and
            // the synthesize_texture arguments that regenerate it
            Json synthesis = synthesisAssetInfo(project, file, true);
            if (!synthesis.is_null()) {
                data["synthesis"] = synthesis;
                text += "." + synthesisAssetText(synthesis);
                text.pop_back();
            }
        }
        if (extensionOf(file) != ".svg") {
            // a sprite normal map: its recipe, whether its source changed, the regenerate arguments;
            // a plain image: its normal map by the default names
            const Json normals = spriteNormalsAssetInfo(project, file, true);
            if (normals.is_object()) {
                for (auto it = normals.begin(); it != normals.end(); ++it) data[it.key()] = it.value();
                text += "." + spriteNormalsAssetText(normals);
                text.pop_back();
            }
        }
    } else if (kind == "model") {
        std::string error;
        Json info;
        if (FbxImporter::isFbxFile(file)) {
            // An FBX loads as the glTF it is converted into, so that is what is summarized. Read
            // only: the conversion is not made here (set_model, import_files, Play and export do).
            if (!project->isInsideAssetsPath(file)) {
                error = "Outside the assets directory, so never converted.";
            } else {
                const fs::path glbPath = project->getImportedModelPath(file);
                const FbxImporter::State state = FbxImporter::getState(file, glbPath, project->getAssetsPath());
                data["fbx_import"] = {{"path", projectRelative(project, glbPath)},
                    {"state", state == FbxImporter::State::UpToDate ? "current" : state == FbxImporter::State::Stale ? "stale" : "missing"}};
                if (state == FbxImporter::State::Missing) {
                    error = "Not converted yet: loading it (set_model) or import_files converts it.";
                } else {
                    info = modelInfo(glbPath, error);
                }
            }
        } else {
            info = modelInfo(file, error);
        }
        // the model cook (S53 R23): required extensions, cooked sizes and status per platform
        if (project->isInsideAssetsPath(file) && isModelSource(project->normalizeToAssetsRelative(file).generic_string()))
            data["cook"] = ModelAudit::cookSummaryJson(*project, project->normalizeToAssetsRelative(file).generic_string());
        if (info.is_null()) data["error"] = error;
        else {
            data["model"] = info;
            if (info.contains("meshes")) {
                text += ", " + std::to_string(info["meshes"].size()) + " mesh(es), " + std::to_string(info["materials"].size()) +
                        " material(s), " + std::to_string(info["animations"].size()) + " animation(s)";
            }
            // the Avatar of a skinned model (anim-retarget): its humanoid map and Animation Type
            if (info.contains("skins") && info["skins"].get<size_t>() > 0) {
                const Json avatar = clip_tools::avatarJson(project, file);
                if (!avatar.is_null() && !avatar.contains("humanoid")) {
                    data["avatar"] = avatar;
                    text += ", avatar: " + avatar["error"].get<std::string>();
                } else if (!avatar.is_null()) {
                    data["avatar"] = avatar;
                    text += std::string(", ") + (avatar["humanoid"].get<bool>() ? "humanoid" : "not a humanoid") + " (Animation Type " +
                            avatar["animation_type"].get<std::string>() + ")";
                }
            }
        }
    } else if (kind == "audio") {
        // any audio format: its format and an analysis summary, measured off the main thread
        assetInfoAudio(call, file, rel, [call, file, text, data](const Json& audio, const std::string& error) mutable {
            if (audio.is_null()) {
                data["audio_error"] = error;
                text += " (" + error + ")";
            } else {
                data["audio"] = audio;
                char line[160];
                std::snprintf(line, sizeof(line), ", %s %s, %d ch, %d Hz, %.3f s", audio["codec"].get<std::string>().c_str(),
                              audio["encoding"].get<std::string>().c_str(), audio["channels"].get<int>(), audio["sample_rate"].get<int>(),
                              audio["duration_seconds"].get<double>());
                text += line;
                const Json& momentary = audio["analysis"]["momentary_max"];
                if (momentary.is_number()) {
                    std::snprintf(line, sizeof(line), ", %.1f LUFS-M", momentary.get<double>());
                    text += line;
                }
            }
            if (CodeEditor* editor = call->ctx().codeEditor()) {
                if (editor->isFileOpen(file.string())) data["open_in_code_editor"] = {{"unsaved", editor->isFileModified(file.string())}};
            }
            call->ok(text + ".", data);
        });
        return;
    } else if (kind == "video") {
        // the probe (header boxes only) and where it plays (video-mp4 spec 6.10)
        const VideoProbe probe = VideoFormats::probeFile(file.string());
        Json video = {{"container", probe.container}, {"readable", probe.readable}, {"width", probe.width}, {"height", probe.height},
                      {"duration", std::round(probe.duration * 1000.0) / 1000.0}};
        if (!probe.videoCodec.empty()) video["video_codec"] = {{"id", probe.videoCodecString.empty() ? probe.videoCodec : probe.videoCodecString},
                                                               {"name", VideoFormats::codecName(probe.videoCodec)}};
        if (!probe.audioCodec.empty()) video["audio_codec"] = {{"id", probe.audioCodecString.empty() ? probe.audioCodec : probe.audioCodecString},
                                                               {"name", VideoFormats::codecName(probe.audioCodec)}, {"channels", probe.audioChannels}};
        if (probe.frameRate > 0.0) video["frame_rate"] = std::round(probe.frameRate * 1000.0) / 1000.0;
        if (probe.rotation != 0) video["rotation"] = probe.rotation;
        const bool mp4 = probe.container == "mp4" || probe.container == "mov" || probe.container == "3gp";
        if (mp4) video["fast_start"] = probe.fastStart;
        if (probe.encrypted) video["encrypted"] = true;
        Json playable = Json::object();
        for (int p = 0; p < VideoSourceFactory::PLATFORM_COUNT; p++) {
            const auto platform = (VideoSourceFactory::Platform)p;
            playable[VideoSourceFactory::platformId(platform)] = VideoSourceFactory::playableOn(platform, probe);
        }
        video["playable_on"] = playable;
        // here, in this editor (Play)
        video["plays_here"] = probe.playable;
        video["backend_here"] = probe.backend;
        if (!probe.playable && probe.error) video["error"] = probe.error.message;
        data["video"] = video;
        if (mp4) data["fast_start"] = probe.fastStart;
        data["playable_on"] = playable;
        text += ", " + probe.container + " " + VideoFormats::codecName(probe.videoCodec) + " " + std::to_string(probe.width) + "x" +
                std::to_string(probe.height) + ", playable_on " + playable.dump();
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
    } else if (kind == "animation" && Util::isAnimationClipFile(rel)) {
        // an .anim clip: settings, tracks, events, load warnings and the Animation Players using it
        clip_tools::clipAssetInfo(project, file, data, text);
    } else if (kind == "vertex_animation") {
        // a .vat: mode, clips, layout, stats, staleness against its recipe, the Vertex Animators using it
        vatAssetInfo(project, file, data, text);
    } else if (kind == "shake") {
        // a .shake: its values and the peaks of its graph (the Inspector's: 60 Hz, seed 0)
        std::string source;
        ProjectChecks::readFile(file, source);
        const camera::ShakeParseResult parsed = camera::parseShakePreset(source);
        if (!parsed.ok()) {
            data["error"] = parsed.errorText();
            text += "; not a valid shake preset: " + parsed.errorText();
        } else {
            const camera::ShakePreset& preset = parsed.file->preset;
            const float duration = camera::shakePreviewDuration(preset);
            const camera::ShakeOffset peaks = camera::shakeOffsetPeaks(camera::sampleShakePreset(preset, duration, 60.0f, 0));
            data["shake"] = {{"type", camera::shakeTypeName(preset.type)}, {"strength", McpValues::number(preset.strength)},
                             {"roughness", McpValues::number(preset.roughness)}, {"fade_in", McpValues::number(preset.fadeIn)},
                             {"fade_out", McpValues::number(preset.fadeOut)},
                             {"position_influence", McpValues::toJson(preset.positionInfluence)},
                             {"rotation_influence", McpValues::toJson(preset.rotationInfluence)}};
            data["peak"] = {{"position", McpValues::toJson(peaks.position)}, {"rotation", McpValues::toJson(peaks.rotation)},
                            {"duration", McpValues::number(duration)}};
            if (!parsed.warnings.empty()) data["warnings"] = parsed.warnings;
            char summary[160];
            std::snprintf(summary, sizeof(summary), "; %s shake, strength %g, roughness %g; peak rotation %.2f deg",
                          camera::shakeTypeName(preset.type), preset.strength, preset.roughness,
                          std::max(peaks.rotation.x, std::max(peaks.rotation.y, peaks.rotation.z)));
            text += summary;
        }
    }
    if (kind == "model" && rel.size() > 8 && Util::isVertexAnimationFile(rel.substr(0, rel.size() - 4))) {
        // "<stem>.vat.glb": the baked mesh and payload of the .vat next to it
        const std::string owner = rel.substr(0, rel.size() - 4);
        data["generated_by"] = owner;
        text += "; generated with " + owner + " (do not edit: rebake with bake_vertex_animation {rebake: \"" + owner + "\"})";
    }
    if (CodeEditor* editor = call->ctx().codeEditor()) {
        if (editor->isFileOpen(file.string())) data["open_in_code_editor"] = {{"unsaved", editor->isFileModified(file.string())}};
    }
    call->ok(text + ".", data);
}

// ------------------------------------------------------------------ view_asset

void replyImage(const CallPtr& call, const fs::path& image, const std::string& label, int maxSize, const Json& synthesis = Json(),
                const Json& normals = Json()) {
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
    Json data = {{"width", w}, {"height", h}, {"preview_width", outW}, {"preview_height", outH}};
    if (!synthesis.is_null()) data["synthesis"] = synthesis;
    if (normals.is_object()) {
        for (auto it = normals.begin(); it != normals.end(); ++it) data[it.key()] = it.value();
    }
    ToolResult result = ToolResult::success(label + " (" + std::to_string(w) + "x" + std::to_string(h) +
                                                (outW != w ? ", shown at " + std::to_string(outW) + "x" + std::to_string(outH) : std::string()) + ")." +
                                                synthesisAssetText(synthesis) + spriteNormalsAssetText(normals),
                                            data);
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
        replyImage(call, file, rel, maxSize, extensionOf(file) == ".png" ? synthesisAssetInfo(project, file, false) : Json(),
                   spriteNormalsAssetInfo(project, file, false));
        return;
    }
    if (Util::isVertexAnimationFile(rel)) {
        call->fail(rel + " is a Vertex Animation (no image): bake_vertex_animation with its source and no output returns a contact sheet "
                   "(source vs VAT, nothing written); in a scene, play it and take a screenshot. get_asset_info lists its clips.");
        return;
    }
    if (Util::isAnimationClipFile(rel)) {
        call->fail(rel + " is an animation clip (no image): see it with preview_animation {animation: <a GameObject whose Animation Player "
                   "plays it>, action: seek, time} and a screenshot, or read its values with get_animation {clip, times}.");
        return;
    }
    // .poseanim: the middle pose on its preview model (the Project window's thumbnail)
    if (!(Util::isImageFile(rel) || Util::isModelFile(rel) || Util::isMaterialFile(rel) || Util::isPrefabFile(rel) ||
          extensionOf(file) == ".poseanim")) {
        call->fail("view_asset shows images, and thumbnails of models, materials, prefabs and pose animations. For scenes use the screenshot tools.");
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
    if (!resolveExisting(call, "path", file, false, PathFor::Write)) return;
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
    // Clips must stay loadable (Animation Players read them): content problems are warnings
    std::vector<std::string> clipWarnings;
    if (Util::isAnimationClipFile(rel)) {
        std::string why;
        if (!clip_tools::checkClipText(content, clipWarnings, why)) {
            call->fail("Not a valid animation clip: " + why + ". Prefer edit_animation {clip} (the format: jasper://docs/animation).");
            return;
        }
    }
    // Avatar Masks and Animator Controllers must stay loadable (layers and players read them): content problems
    // (and a controller's validation) are warnings
    std::vector<std::string> animWarnings;
    if (Util::isAvatarMaskFile(rel) || Util::isAnimatorControllerFile(rel)) {
        std::string why;
        const bool mask = Util::isAvatarMaskFile(rel);
        if (mask ? !animator_tools::checkMaskText(content, animWarnings, why) : !animator_tools::checkControllerText(project, content, animWarnings, why)) {
            call->fail(std::string(mask ? "Not a valid Avatar Mask: " : "Not a valid Animator Controller: ") + why +
                       (mask ? ". The format: jasper://docs/animator (Avatar Masks); or edit_avatar_mask." : ". Prefer edit_animator_controller (jasper://docs/animator)."));
            return;
        }
    }
    // Shake Presets must stay readable (the shakers load them): content problems are warnings
    std::vector<std::string> shakeWarnings;
    if (Util::isShakePresetFile(rel)) {
        const camera::ShakeParseResult parsed = camera::parseShakePreset(content);
        if (!parsed.ok()) {
            call->fail("Not a valid shake preset: " + parsed.errorText() + ". The format: {\"format\": \"jasperengine.shake\", "
                       "\"version\": 1, \"type\", \"strength\", \"roughness\", \"fade_in\", \"fade_out\", \"position_influence\", "
                       "\"rotation_influence\"} (jasper://docs/camera).");
            return;
        }
        shakeWarnings = parsed.warnings;
    }
    // Vertex Animations must stay loadable: generated fields are the baker's (rebake), the rest are warnings
    std::vector<std::string> vatWarnings;
    if (Util::isVertexAnimationFile(rel)) {
        std::string why;
        if (!checkVatText(project, file, content, vatWarnings, why)) {
            call->fail("Not a valid Vertex Animation: " + why + " (jasper://docs/vertex-animation).");
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
    } else if (Util::isVertexAnimationFile(rel)) {
        data["warnings"] = vatWarnings;
        text += vatWarnings.empty() ? std::string(" Valid Vertex Animation (its animators reload).")
                                    : " Vertex Animation loads with " + std::to_string(vatWarnings.size()) + " warning(s) (the bad parts are dropped):";
        for (size_t i = 0; i < vatWarnings.size() && i < 8; i++) text += "\n" + vatWarnings[i];
    } else if (Util::isAvatarMaskFile(rel) || Util::isAnimatorControllerFile(rel)) {
        data["warnings"] = animWarnings;
        const char* kind = Util::isAvatarMaskFile(rel) ? "Avatar Mask" : "Animator Controller";
        text += animWarnings.empty() ? std::string(" Valid ") + kind + " (its players reload)."
                                     : std::string(" ") + kind + " loads with " + std::to_string(animWarnings.size()) + " warning(s):";
        for (size_t i = 0; i < animWarnings.size() && i < 8; i++) text += "\n" + animWarnings[i];
    } else if (Util::isShakePresetFile(rel)) {
        data["warnings"] = shakeWarnings;
        text += shakeWarnings.empty() ? std::string(" Valid shake preset.") : " Shake preset loads with " + std::to_string(shakeWarnings.size()) +
                                                                            " warning(s) (bad values repaired):";
        for (size_t i = 0; i < shakeWarnings.size() && i < 8; i++) text += "\n" + shakeWarnings[i];
    } else if (Util::isAnimationClipFile(rel)) {
        data["warnings"] = clipWarnings;
        text += clipWarnings.empty() ? std::string(" Valid clip.") : " Clip loads with " + std::to_string(clipWarnings.size()) + " warning(s) (the bad parts are dropped):";
        for (size_t i = 0; i < clipWarnings.size() && i < 8; i++) text += "\n" + clipWarnings[i];
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
    if (!resolveExisting(call, "path", dir, false, PathFor::Write)) return;
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
    // made beneath the project: no link planted on the way since the check is followed
    auto* create = new CreateDirCmd(dir.filename().string(), dir.parent_path().string());
    create->setWriteRoots(McpRefs::trustedRoots(project));
    if (!addProjectCommand(call->ctx(), create)) {
        call->fail("Couldn't create the folder.");
        return;
    }
    call->ok("Created folder " + projectRelative(project, dir) + ".", Json{{"path", projectRelative(project, dir)}});
}

// The other file of a Vertex Animation pair goes with a copy or a move (vat/VatAssetFiles.h): the
// pair's two operations are one AssetOps result (its files written and broken text references)
void mergeResult(AssetOps::Result& into, const AssetOps::Result& other) {
    into.filesWritten.insert(into.filesWritten.end(), other.filesWritten.begin(), other.filesWritten.end());
    into.textReferencesBroken.insert(into.textReferencesBroken.end(), other.textReferencesBroken.begin(), other.textReferencesBroken.end());
    into.changes.insert(into.changes.end(), other.changes.begin(), other.changes.end());
}

// The .vat of a pair now at `vatFile` names its mesh beside it; `before` gets the text it had
// (empty: unchanged). The database learns the new size and time, its documents reload
void nameVatMesh(Project* project, const fs::path& vatFile, std::string* before, AssetOps::Result& report) {
    std::string text;
    if (!ProjectChecks::readFile(vatFile, text)) return;
    std::string updated = text;
    if (!vat::setMeshField(updated, FileUtils::pathToUtf8(vat::pairPathOf(vatFile).filename())) ||
        !FileUtils::writeFileReplacing(vatFile, updated)) {
        return;
    }
    if (before) *before = text;
    project->importPrefabFiles(vatFile);
    VatDocument::filesChanged({vatFile}, project);
    const std::string relative = projectRelative(project, vatFile);
    if (std::find(report.filesWritten.begin(), report.filesWritten.end(), relative) == report.filesWritten.end()) {
        report.filesWritten.push_back(relative);
    }
}

// Copies a file or folder to an exact destination; undo deletes the copy. The copy and its
// meta get new GUIDs (the same ones again on a redo). A Vertex Animation is copied with its
// generated mesh, the copied .vat naming the copied mesh
class CopyAsCmd : public Command {
public:
    CopyAsCmd(Project* project, fs::path from, fs::path to, std::shared_ptr<AssetOps::Result> report)
        : project(project), from(std::move(from)), to(std::move(to)), report(std::move(report)) {}

    std::vector<Touched> touchedPaths(bool) const override {
        std::vector<Touched> out = {{to, true}};
        if (!pairTo.empty()) out.push_back({pairTo, true});
        return out;
    }

    bool execute() override {
        *report = AssetOps::copy(*project, from, to, &copyGuids);
        if (!report->ok) return false;
        pairTo.clear();
        fs::path vatCopy;
        if (vat::renamedPair(from, to, pairFrom, pairTo)) {
            const AssetOps::Result pair = AssetOps::copy(*project, pairFrom, pairTo, &copyGuids);
            if (!pair.ok) {
                AssetOps::removeCopy(*project, to);
                report->ok = false;
                report->error = pair.error;
                pairTo.clear();
                return false;
            }
            mergeResult(*report, pair);
            vatCopy = Util::isVertexAnimationFile(FileUtils::pathToUtf8(to)) ? to : pairTo;
            nameVatMesh(project, vatCopy, nullptr, *report);
        }
        // a copied .vat.glb in another directory names the same texture files (the copies of a
        // copied folder's own textures)
        std::vector<std::pair<fs::path, fs::path>> went = {{from, to}};
        if (!pairTo.empty()) went.emplace_back(pairFrom, pairTo);
        vat::rebaseMovedVatMeshes(went);
        return true;
    }
    void undo() override {
        // references to the copies stay (Missing until a redo)
        if (!pairTo.empty()) AssetOps::removeCopy(*project, pairTo);
        AssetOps::removeCopy(*project, to);
    }
    bool affectsStructure() const override { return false; }

    // "Copy File a.png → b.png"
    std::string describe() const override {
        return "Copy File " + CommandNames::file(from) + CommandNames::ARROW + CommandNames::file(to);
    }
    const char* typeName() const override { return "CopyAsCmd"; }
    size_t memoryBytes() const override {
        size_t bytes = sizeof(*this) + from.native().size() + to.native().size() + pairFrom.native().size() + pairTo.native().size();
        for (const auto& [path, guid] : copyGuids) bytes += path.size() + guid.size();
        return bytes;
    }

private:
    Project* project;
    fs::path from;
    fs::path to;
    fs::path pairFrom, pairTo;      // the other file of a Vertex Animation and its copy
    std::map<std::string, std::string> copyGuids;
    std::shared_ptr<AssetOps::Result> report;
};

// Moves (or renames) a file or folder through AssetOps: GUIDs kept, references refreshed. A
// Vertex Animation's other file goes too, the moved .vat naming its mesh; the mesh's texture
// URIs follow a change of directory (all put back by the undo)
class MoveAsCmd : public Command {
public:
    MoveAsCmd(Project* project, fs::path from, fs::path to, std::shared_ptr<AssetOps::Result> report)
        : project(project), from(std::move(from)), to(std::move(to)), report(std::move(report)) {}
    bool execute() override {
        *report = AssetOps::move(*project, from, to);
        if (!report->ok) return false;
        pairTo.clear();
        vatFile.clear();
        vatTextBefore.clear();
        if (vat::renamedPair(from, to, pairFrom, pairTo)) {
            const AssetOps::Result pair = AssetOps::move(*project, pairFrom, pairTo);
            if (!pair.ok) {
                AssetOps::move(*project, to, from);
                report->ok = false;
                report->error = pair.error;
                pairTo.clear();
                return false;
            }
            mergeResult(*report, pair);
            vatFile = Util::isVertexAnimationFile(FileUtils::pathToUtf8(to)) ? to : pairTo;
            nameVatMesh(project, vatFile, &vatTextBefore, *report);
        }
        moved = {{from, to}};
        if (!pairTo.empty()) moved.emplace_back(pairFrom, pairTo);
        uriEdits = vat::rebaseMovedVatMeshes(moved);
        return true;
    }
    void undo() override {
        // the .vat's text back while it holds what this command wrote, then the files
        if (!vatFile.empty() && !vatTextBefore.empty()) {
            std::string now;
            std::string written = vatTextBefore;
            vat::setMeshField(written, FileUtils::pathToUtf8(vat::pairPathOf(vatFile).filename()));
            if (ProjectChecks::readFile(vatFile, now) && now == written) {
                FileUtils::writeFileReplacing(vatFile, vatTextBefore);
            }
        }
        if (!pairTo.empty()) {
            const AssetOps::Result back = AssetOps::move(*project, pairTo, pairFrom);
            if (!back.ok) Out::error("Couldn't undo the move of %s: %s", pairFrom.filename().string().c_str(), back.error.c_str());
        }
        const AssetOps::Result back = AssetOps::move(*project, to, from);
        if (!back.ok) Out::error("Couldn't undo the move of %s: %s", from.filename().string().c_str(), back.error.c_str());
        vat::restoreVatMeshUris(uriEdits, moved);
        uriEdits.clear();
        if (!vatFile.empty()) {
            const fs::path vatBack = (vatFile == to) ? from : pairFrom;
            project->importPrefabFiles(vatBack);
            VatDocument::filesChanged({vatBack}, project);
        }
    }
    std::string describe() const override {
        if (from.parent_path() == to.parent_path()) {
            return CommandNames::clip("Rename File " + CommandNames::file(from) + CommandNames::ARROW + CommandNames::file(to));
        }
        return CommandNames::clip("Move " + CommandNames::file(from));
    }
    const char* typeName() const override { return "MoveAsCmd"; }
    // both entries renamed (and the other file of a Vertex Animation pair), each way
    std::vector<Touched> touchedPaths(bool) const override {
        std::vector<Touched> out = {{from, true}, {to, true}};
        if (!pairTo.empty()) {
            out.push_back({pairFrom, true});
            out.push_back({pairTo, true});
        }
        return out;
    }

private:
    Project* project;
    fs::path from;
    fs::path to;
    fs::path pairFrom, pairTo;      // the other file of a Vertex Animation and where it went
    fs::path vatFile;               // the moved .vat whose mesh field was rewritten
    std::string vatTextBefore;
    std::vector<std::pair<fs::path, fs::path>> moved;
    std::vector<vat::VatUriEdit> uriEdits;
    std::shared_ptr<AssetOps::Result> report;
};

Json textReferencesJson(const std::vector<AssetOps::TextReference>& references) {
    Json list = Json::array();
    for (const AssetOps::TextReference& reference : references) {
        list.push_back({{"referrer", reference.referrer}, {"line", reference.line}, {"target", reference.target},
                        {"kind", depEdgeKindName(reference.kind)}});
    }
    return list;
}

// The project root, its project.yaml or something under a top-level hidden folder (.jasperengine,
// .trash, .git ...): as named and where the entry resolves (a link inside the project to its root
// or its .jasperengine is no way in); names compared as the file system may (letter case, Windows
// aliases). What a tool never moves away or deletes.
bool isProjectEntry(Project* project, const fs::path& absolute) {
    // any hidden ('.'-named) component, anywhere in the path (assets/lib/.git too)
    auto spelled = [](const fs::path& rel) {
        if (rel.empty() || rel == ".") return true;
        if (*rel.begin() == "..") return false;
        for (const fs::path& part : rel) {
            const std::string name = safewrite::comparableName(part);
            if (!name.empty() && name[0] == '.' && name != "." && name != "..") return true;
        }
        return std::distance(rel.begin(), rel.end()) == 1 && safewrite::isProjectFileName(rel);
    };
    const fs::path normal = absolute.lexically_normal();
    if (spelled(normal.lexically_relative(project->getProjectPath().lexically_normal()))) return true;
    const fs::path where = safewrite::entryLocation(normal);
    // as the entry resolves, against every trusted root (an assets or Lua root outside the project too)
    for (const fs::path& root : McpRefs::trustedRoots(project)) {
        if (!where.empty() && safewrite::isAtOrBelow(where, root) && spelled(where.lexically_relative(root))) {
            // the root itself is not "an entry of the project"; trustedRootTouched handles roots
            if (where.lexically_relative(root) == ".") continue;
            return true;
        }
    }
    return false;
}

void moveAssetTool(const CallPtr& call) {
    fs::path from, to;
    // a copy only reads its source (a copy out of a linked-in library is fine); a move moves the entry
    const bool copying = call->args().value("copy", false);
    if (!resolveExisting(call, "path", from, true, copying ? PathFor::Read : PathFor::Entry)) return;
    if (!resolveExisting(call, "to", to, false, PathFor::Write)) return;
    Project* project = call->ctx().project();
    const std::string fromRel = projectRelative(project, from), toRel = projectRelative(project, to);
    std::string why;
    // not the project root, its project.yaml or a hidden folder's contents, nor a file named
    // project.yaml anywhere (as named or where it resolves)
    if (isProjectEntry(project, from) || !McpRefs::protectedPlace(project, from, copying ? McpRefs::WriteUse::Write : McpRefs::WriteUse::Entry).empty()) {
        call->fail("Can't " + std::string(copying ? "copy " : "move ") + (fromRel.empty() ? std::string("the project root") : fromRel) +
                   ": it is the project root, a project.yaml, or under a hidden folder (or leads there through a link).");
        return;
    }
    // never into a hidden or editor-owned place (.jasperengine, .trash, .git ...; project.yaml,
    // scenes' generated files): a link moved there would let a build or an export write through it.
    // Checked as named and where it resolves (a user link to .jasperengine is no way in)
    {
        std::string owned;
        if (isEditorOwnedFile(project, to, owned)) {
            call->fail("Can't move to " + toRel + ": " + owned + " Pick a folder of the project's assets.");
            return;
        }
    }
    // a trusted root (the assets or Lua folder, or a folder holding one) is not moved away, nor
    // anything moved into its place
    if (const std::string touched = McpRefs::trustedRootTouched(project, from, false); !touched.empty() && !copying) {
        call->fail(touched);
        return;
    }
    if (const std::string touched = McpRefs::trustedRootTouched(project, to, true); !touched.empty()) {
        call->fail(touched);
        return;
    }
    // moving a link that leads out of the project's trusted roots (or a folder holding one, at
    // any depth) would relocate an escape hatch
    {
        std::error_code linkError;
        const fs::file_status status = fs::symlink_status(from, linkError);
        if (!copying && fs::is_symlink(status)) {
            if (const std::string escape = McpRefs::writeEscape(project, from); !escape.empty()) {
                call->fail("Can't move " + fromRel + ": it is a link leading out of the project (" + escape +
                           ") — moving the link would take that elsewhere. Copy what it holds instead (move_asset copy: true), or move it yourself.");
                return;
            }
        } else if (!copying && fs::is_directory(status)) {
            if (const std::string escape = McpRefs::linkOutInside(project, from); !escape.empty()) {
                call->fail("Can't move " + fromRel + ": it holds a link leading out of the project (" + escape +
                           ") — moving the folder would take that link elsewhere. Remove the link first, or move the folder yourself.");
                return;
            }
        }
    }
    // a copy makes no second project.yaml (the copy of a folder holding one: refused up front; the
    // writers refuse it too)
    if (copying) {
        std::error_code scanError;
        size_t entries = 0;
        if (fs::is_directory(from, scanError)) {
            for (fs::recursive_directory_iterator it(from, fs::directory_options::none, scanError), end; !scanError && it != end && entries < 200000;
                 it.increment(scanError), entries++) {
                if (!safewrite::isProjectFileName(it->path())) continue;
                call->fail("Can't copy " + fromRel + ": it holds " + projectRelative(project, it->path()) +
                           ", and tools make no file named project.yaml (another one would be a project to open). "
                           "Copy the folder without it, or in the editor.");
                return;
            }
        }
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
    // a Vertex Animation's other file goes along (CopyAsCmd / MoveAsCmd take the pair:
    // vat/VatAssetFiles.h); a destination that would break the pair is refused here
    fs::path pairFrom, pairTo;
    const bool pair = vat::renamedPair(from, to, pairFrom, pairTo);
    if (const std::string refusal = vat::pairRenameRefusal(from, to, fromRel); !refusal.empty()) {
        call->fail(refusal);
        return;
    }
    if (pair && fs::exists(pairTo, ec)) {
        call->fail(projectRelative(project, pairTo) + " already exists (the other file of the Vertex Animation goes there): delete it first or pick another name.");
        return;
    }
    auto report = std::make_shared<AssetOps::Result>();
    Command* cmd = copy ? static_cast<Command*>(new CopyAsCmd(project, from, to, report))
                        : static_cast<Command*>(new MoveAsCmd(project, from, to, report));
    // made beneath the project's trusted roots: its undo and redo stay there too (util/SafeWrite.h)
    cmd->setWriteRoots(McpRefs::trustedRoots(project));
    if (!addProjectCommand(call->ctx(), cmd)) {
        call->fail("Couldn't " + std::string(copy ? "copy " : "move ") + fromRel + " to " + toRel +
                   (report->error.empty() ? "." : ": " + report->error));
        return;
    }
    if (CodeEditor* editor = call->ctx().codeEditor(); editor && !copy) editor->handleFileRename(from, to);
    std::vector<fs::path> synced = {from, to};
    if (pair) synced.insert(synced.end(), {pairFrom, pairTo});
    syncFiles(call->ctx(), synced, false);
    Json data{{"from", fromRel}, {"to", toRel}, {"copy", copy}, {"files_written", report->filesWritten},
              {"text_references_broken", textReferencesJson(report->textReferencesBroken)}};
    const Guid guid = project->getAssetDatabase().getAssetGuid(toRel);
    if (!guid.isNull()) data["guid"] = guid.str();
    std::string text = std::string(copy ? "Copied " : "Moved ") + fromRel + " to " + toRel +
        (copy ? (Util::isAnimationClipFile(toRel) ? " (a new clip with its own GUID; Animation Players keep the original)."
                 : pair ? " (a new Vertex Animation with its own GUID)." : " (new GUIDs).")
              : "; references follow it by GUID and no referrer file was written. Undo with scope project.");
    if (pair) {
        data["pair"] = {{"from", projectRelative(project, pairFrom)}, {"to", projectRelative(project, pairTo)}};
        text += " With it, " + projectRelative(project, pairFrom) + " -> " + projectRelative(project, pairTo) +
                " (a Vertex Animation and its mesh stay together; the .vat's mesh follows).";
    }
    for (const AssetOps::TextReference& broken : report->textReferencesBroken) {
        text += "\nStill names the old path: " + broken.referrer + ":" + std::to_string(broken.line) + " (" + broken.target + ")";
    }
    call->ok(text, data);
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
        // the entry itself goes to the trash: a link there is moved, never followed
        if (!item.is_string() || !McpRefs::resolveProjectWritePath(project, item.get<std::string>(), file, error, true, McpRefs::WriteUse::Entry)) {
            call->fail(error.empty() ? "paths must be strings." : error);
            return;
        }
        const std::string rel = projectRelative(project, file.lexically_normal());
        if (isProjectEntry(project, file)) {
            call->fail("Refusing to delete " + (rel.empty() ? std::string("the project root") : rel) + ".");
            return;
        }
        // a trusted root (the assets or Lua folder, or a folder holding one): never deleted by a tool
        if (const std::string touched = McpRefs::trustedRootTouched(project, file, false); !touched.empty()) {
            call->fail(touched);
            return;
        }
        for (SceneProject* scene : project->getScenesAndPrefabStages()) {
            if (!scene->filepath.empty() && projectRelative(project, scene->filepath) == rel && scene->scene) {
                call->fail(rel + " is open in the editor; close it first (or delete the scene with the scene tools).");
                return;
            }
        }
        files.push_back(file.lexically_normal());
        Json entry = {{"path", rel}};
        const Guid guid = project->getAssetDatabase().getAssetGuid(rel);
        if (!guid.isNull()) entry["guid"] = guid.str();
        deleted.push_back(entry);
    }
    // a Vertex Animation goes with its generated mesh, and the reverse (DeleteFileCmd takes the
    // pair: vat/VatAssetFiles.h): reported, and synced with the rest
    Json pairs = Json::array();
    const std::vector<fs::path> listed = files;
    for (const fs::path& file : vat::withPairs(listed)) {
        if (std::find(listed.begin(), listed.end(), file) != listed.end()) continue;
        const std::string rel = projectRelative(project, file);
        pairs.push_back(rel);
        Json entry = {{"path", rel}};
        const Guid guid = project->getAssetDatabase().getAssetGuid(rel);
        if (!guid.isNull()) entry["guid"] = guid.str();
        deleted.push_back(entry);
    }
    // what will reference the deleted files: GUID references become Missing (list_missing_assets),
    // text ones (Lua require, HTML/CSS, shader #include) break
    std::vector<AssetOps::TextReference> broken;
    for (const fs::path& file : vat::withPairs(listed)) {
        for (const AssetOps::TextReference& reference : AssetOps::textReferencesInto(*project, file)) broken.push_back(reference);
    }
    // the Animation Players using a deleted clip keep the reference (Missing; undo relinks it)
    size_t clipUsers = 0;
    for (const Json& entry : deleted) {
        const std::string rel = entry["path"].get<std::string>();
        if (Util::isAnimationClipFile(rel)) clipUsers += findClipUsers(project, rel).size();
    }
    auto* deleteCmd = new DeleteFileCmd(project, files, project->getProjectPath());
    // moved to the trash beneath the project: no link planted on the way since the check is followed
    deleteCmd->setWriteRoots(McpRefs::trustedRoots(project));
    if (!addProjectCommand(call->ctx(), deleteCmd)) {
        call->fail("Couldn't delete the files.");
        return;
    }
    syncFiles(call->ctx(), vat::withPairs(listed), false);
    Json data = {{"deleted", deleted}, {"text_references_broken", textReferencesJson(broken)}};
    std::string text = "Moved " + std::to_string(deleted.size()) + " item(s) to the project trash; references to them stay and show as missing "
                       "(undo with scope project restores the files and resolves them again).";
    if (!pairs.empty()) {
        data["with_pair"] = pairs;
        text += " With them, the other half of each Vertex Animation (.vat / .vat.glb):";
        for (const Json& p : pairs) text += " " + p.get<std::string>();
        text += ".";
    }
    if (clipUsers > 0) {
        data["clip_users"] = clipUsers;
        text += " " + std::to_string(clipUsers) + " Animation Player clip entr" + (clipUsers == 1 ? "y keeps" : "ies keep") +
                " the reference (Missing until the clip is back; undo relinks).";
    }
    call->ok(text, data);
}

// The remote half of import_files (phase-4 spec §3.6.5): the HTML UI importers (network) run on a
// worker thread; the main thread then rewrites the document and makes the editor notice the files.
struct RemoteImport {
    fs::path projectRoot;
    std::vector<fs::path> writeRoots;     // the project's trusted roots, for the worker's write scope
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
    if (!resolveExisting(call, "to_dir", target, false, PathFor::Write)) return false;
    std::error_code ec;
    std::vector<std::string> sources;
    Json imported = Json::array();
    for (const std::string& item : local) {
        const fs::path source = FileUtils::pathFromUtf8(item).lexically_normal();
        if (!source.is_absolute() || !fs::exists(source, ec)) {
            call->fail(item + " must be an existing absolute path (or an http(s) URL).");
            return false;
        }
        // "already in the project" checked where it resolves, not just as spelled: /proc/self/root,
        // a \\?\ prefix, a symbolic link or a case variant all land inside the project
        const fs::path sourceReal = safewrite::resolvedPath(source);
        const fs::path projectReal = safewrite::resolvedPath(project->getProjectPath());
        if (Util::isInsidePath(source, project->getProjectPath()) ||
            (!sourceReal.empty() && !projectReal.empty() && safewrite::isAtOrBelow(sourceReal, projectReal))) {
            call->fail(item + " is already in the project; use move_asset (copy: true) instead.");
            return false;
        }
        const fs::path dest = target / source.filename();
        // never into an editor-owned or hidden place (.jasperengine, .git, .trash, project.yaml,
        // CMakeLists.txt), as named or where it resolves
        if (const std::string owned = McpRefs::protectedPlace(project, dest); !owned.empty()) {
            call->fail("Can't import to " + projectRelative(project, dest) + ": " + owned);
            return false;
        }
        // a link at the destination name leading out of the project: the copy would follow it
        if (const std::string escape = McpRefs::writeEscape(project, dest); !escape.empty()) {
            call->fail(escape);
            return false;
        }
        if (fs::exists(dest, ec) && !args.value("overwrite", false)) {
            call->fail(projectRelative(project, dest) + " already exists; pass overwrite: true to replace it.");
            return false;
        }
        sources.push_back(source.string());
        imported.push_back(projectRelative(project, dest));
    }
    if (!fs::exists(target, ec)) {
        FileUtils::createDirectories(target, ec);   // beneath the project (the call's write scope)
        if (ec) {
            call->fail("Can't create " + projectRelative(project, target) + ": " + ec.message());
            return false;
        }
    }
    auto* copyCmd = new CopyFileCmd(project, sources, target.string(), true);
    copyCmd->setOverwrite(args.value("overwrite", false));
    if (!addProjectCommand(call->ctx(), copyCmd)) {
        call->fail("Copying the files failed.");
        return false;
    }
    std::vector<fs::path> changed;
    for (const Json& rel : imported) changed.push_back(project->getProjectPath() / FileUtils::pathFromUtf8(rel.get<std::string>()));
    syncFiles(call->ctx(), changed, false);
    data["imported"] = imported;
    // An FBX loads from the glTF it is converted into, made now so the next tool can use it
    Json fbxImports = Json::array();
    Json fbxFailed = Json::array();
    auto importFbx = [&](const fs::path& fbxPath) {
        if (!FbxImporter::isFbxFile(fbxPath) || !project->isInsideAssetsPath(fbxPath)) return;
        (project->importFbxModel(fbxPath) ? fbxImports : fbxFailed).push_back(projectRelative(project, fbxPath));
    };
    for (const fs::path& path : changed) {
        if (fs::is_directory(path, ec)) {
            for (watch::LinkedDirectoryIterator it(path, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec)) importFbx(it->path());
            }
            ec.clear();
        } else {
            importFbx(path);
        }
    }
    if (!fbxImports.empty()) data["fbx_imports"] = fbxImports;
    if (!fbxFailed.empty()) data["fbx_import_failed"] = fbxFailed;
    Json importedGuids = Json::object();
    for (const Json& rel : imported) {
        const Guid guid = project->getAssetDatabase().getAssetGuid(rel.get<std::string>());
        if (!guid.isNull()) importedGuids[rel.get<std::string>()] = guid.str();
    }
    data["imported_guids"] = importedGuids;
    text = "Imported " + std::to_string(imported.size()) + " item(s) into " + projectRelative(project, target) + ".";
    if (!fbxImports.empty()) {
        text += " Converted " + std::to_string(fbxImports.size()) + " FBX model(s) to glTF.";
    }
    if (!fbxFailed.empty()) {
        text += " " + std::to_string(fbxFailed.size()) + " FBX model(s) could not be converted (fbx_import_failed; the console has details).";
    }
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
            changed.push_back(state->projectRoot / FileUtils::pathFromUtf8(file));
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
            failed.push_back({{"source", FileUtils::pathToUtf8(result.output.filename())}, {"error", result.message}});
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
    bool rewritten = false;
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
            const fs::path file = state->projectRoot / FileUtils::pathFromUtf8(state->document);
            std::string content, error;
            if (!ProjectChecks::readFile(file, content)) {
                rewriteNote = " Can't read " + state->document + " to rewrite it.";
            } else {
                const fs::path dir = file.parent_path().lexically_normal();
                Json replacements = Json::array();
                for (const auto& [url, local] : result.replaced) {
                    // a Google Fonts css2 link becomes its generated @font-face stylesheet
                    const std::string to = FileUtils::pathToGenericUtf8((state->projectRoot / FileUtils::pathFromUtf8(local)).lexically_normal().lexically_relative(dir));
                    if (replaceAll(content, url, to) > 0) replacements.push_back({{"from", url}, {"to", to}});
                }
                if (!replacements.empty()) {
                    // the downloads and the manifest reload with the rewrite: the document once
                    jasperengine::htmlui::compat::ImportManifest::reload();
                    std::vector<fs::path> downloads = changed;
                    downloads.push_back(project->getAssetsPath() / FileUtils::pathFromUtf8(jasperengine::htmlui::compat::ImportManifest::kPath));
                    if (writeFilesUndoable(ctx, {{file, content}}, error, downloads)) {
                        data["rewritten"] = {{"file", state->document}, {"replacements", replacements}};
                        rewritten = true;
                    } else {
                        rewriteNote = " Rewriting " + state->document + " failed: " + error;
                    }
                }
            }
        }
    }
    if (written > 0) {
        // the runtime resolves remote URLs through the manifest (a document that keeps them: rewrite false)
        if (!rewritten) {
            jasperengine::htmlui::compat::ImportManifest::reload();
            changed.push_back(project->getAssetsPath() / FileUtils::pathFromUtf8(jasperengine::htmlui::compat::ImportManifest::kPath));
            syncFiles(ctx, changed, false);
        }
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
    // built-in sounds (essentials:<id>, library:<id>): their own plan, one undo step
    std::vector<std::string> builtin;
    for (const std::string& source : sources) {
        if (BuiltinSoundImport::isReference(source)) builtin.push_back(source);
    }
    if (!builtin.empty()) {
        if (builtin.size() != sources.size() || !fonts.empty() || !icons.empty() || fromDocument) {
            call->fail("import built-in sounds (essentials:, library:) in a call of their own.");
            return;
        }
        fs::path target;
        if (!args.contains("to_dir") || args["to_dir"].is_null()) {
            call->fail("to_dir is required with built-in sounds (a project folder, e.g. \"assets/sounds\").");
            return;
        }
        if (!resolveExisting(call, "to_dir", target, false, PathFor::Write)) return;
        if (!project->isInsideAssetsPath(target.lexically_normal())) {
            call->fail("to_dir must be inside the assets folder: only an asset has a .meta.");
            return;
        }
        importBuiltinSounds(call, builtin, target.lexically_normal(), args.value("overwrite", false));
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
    state->writeRoots = McpWriteScope::projectRoots();
    // videos the engine does not play are converted (FFmpeg, on the worker) instead of copied (phase 6)
    std::vector<std::string> plainFiles;
    for (const std::string& item : local) {
        const fs::path source = FileUtils::pathFromUtf8(item).lexically_normal();
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
        if (!resolveExisting(call, "to_dir", target, false, PathFor::Write)) return;
        state->videoTarget = target;
        state->overwrite = args.value("overwrite", false);
        for (const fs::path& video : state->videos) {
            fs::path output = target / video.filename();
            output.replace_extension(".webm");
            std::error_code ec;
            // the converted file, its fallback name and the unconverted copy are written by the
            // importer and FFmpeg: every name inside the project once links are resolved
            // (named as the importer names them: its pathString is UTF-8)
            for (const fs::path& name : {output, target / (FileUtils::pathToUtf8(video.stem()) + ".av1.webm"), target / video.filename()}) {
                if (const std::string escape = McpRefs::writeEscape(project, name); !escape.empty()) {
                    call->fail(escape);
                    return;
                }
            }
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
        if (!McpRefs::resolveProjectWritePath(project, destArg, dest, error, false)) {
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
            if (!ProjectChecks::isUiFile(FileUtils::pathToGenericUtf8(document))) {
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

    // FFmpeg and the network: the process channel
    jobs::SubmitOptions options;
    options.name = "mcp-import";
    jobs::system().processChannel().submit([state]() {
        // the importers' files beneath the project, as on the main thread (the scope is per thread)
        McpWriteScope writeScope(state->writeRoots);
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
    }, options);
    call->waitUntil([state]() { return state->done.load(std::memory_order_acquire); }, 600000,
                    [call, state, data, text](bool timedOut) { finishRemoteImport(call, state, data, text, timedOut); });
}

// ------------------------------------------------------------------ materials

void createMaterialTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    fs::path file;
    if (!resolveExisting(call, "path", file, false, PathFor::Write)) return;
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
    if (!materialWriteAllowed(call, file, rel)) return;
    if (!writeFilesUndoable(call->ctx(), {{file, materialFileContent(call->ctx().project(), file, material)}}, error)) {
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
    if (!resolveExisting(call, "path", file, true, PathFor::Write)) return;
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
    if (!materialWriteAllowed(call, file, rel)) return;
    if (!writeFilesUndoable(call->ctx(), {{file, materialFileContent(call->ctx().project(), file, material)}}, error)) {
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
             Json{{"entity", McpRefs::entityJson(sceneProject, entity)}, {"submesh", submesh}, {"material", rel},
                  {"material_guid", project->getAssetDatabase().getAssetGuid(rel).str()}});
}

// ------------------------------------------------------------------ material presets

// The fields a preset sets on a default material, as get_asset_info / update_material name them
Json presetFieldsJson(Project* project, const MaterialPreset& preset) {
    // in the form a submesh's material reads back in (get_entity, set_properties): enum names
    // and float text as McpValues gives them
    const Json before = McpValues::materialToJson(project, nullptr, Material());
    const Json after = McpValues::materialToJson(project, nullptr, applyMaterialPreset(preset, Material()));
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
        if (!resolveExisting(call, "path", file, true, PathFor::Write)) return;
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
        if (!materialWriteAllowed(call, file, rel)) return;
        if (!writeFilesUndoable(call->ctx(), {{file, materialFileContent(call->ctx().project(), file, material)}}, error)) {
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
    const fs::path directory = FileUtils::pathFromUtf8(args.value("directory", FileUtils::pathToGenericUtf8(ProjectUtils::defaultShaderForkDir()))).lexically_normal();
    if (directory != "." && !Util::isSafeRelativePath(directory)) {
        call->fail("directory must be project-relative.");
        return;
    }
    // the fork's files are written there: inside the project once links are resolved
    if (const std::string escape = McpRefs::writeEscape(project, directory == "." ? project->getProjectPath() : project->getProjectPath() / directory);
        !escape.empty()) {
        call->fail("directory: " + escape);
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

// What each editable member renders with and where that value comes from (its own value, the
// mesh's under a material, the scene's default-shader value, the shader's default, ...)
void annotateEffectiveValues(Json& members, const UniformChain& chain) {
    for (Json& member : members) {
        if (!member.value("editable", false)) continue;
        UniformSource source;
        member["effective"] = vec4Array(ShaderUniforms::resolve(chain, member["name"].get<std::string>(), &source));
        member["source"] = uniformSourceName(source);
    }
}

const char* sceneDefaultTypeName(ShaderType type) {
    switch (type) {
        case ShaderType::MESH: return "mesh";
        case ShaderType::UI: return "ui";
        case ShaderType::SKYBOX: return "sky";
        case ShaderType::POINTS: return "points";
        case ShaderType::LINES: return "lines";
        default: return "";
    }
}

// the values of a type without a default shader are kept, unused until one is set
std::string noSceneDefaultShaderNext(ShaderType type) {
    return std::string("The scene has no default ") + sceneDefaultTypeName(type) +
           " shader: its values apply once one is set (fork_shader target scene_default).";
}

// shader_type of get/set_shader_uniform: the scene's default shader of that type
bool sceneDefaultShaderType(const CallPtr& call, ShaderType& type) {
    const std::string name = call->args().value("shader_type", std::string());
    for (ShaderType candidate : {ShaderType::MESH, ShaderType::UI, ShaderType::SKYBOX, ShaderType::POINTS, ShaderType::LINES}) {
        if (name == sceneDefaultTypeName(candidate)) {
            type = candidate;
            return true;
        }
    }
    call->fail("shader_type is mesh, ui, sky, points or lines.");
    return false;
}

Json manifestParameters(const ShaderManifest* manifest, const ShaderUniformValues& values, const UniformChain* chain = nullptr);

// The scene default's members (the union over the objects drawing with it), values and status
Json sceneDefaultUniformsJson(Scene* scene, ShaderType type, Json& members) {
    const std::string shader = scene->getDefaultCustomShader(type);
    const ShaderUniformValues& values = scene->getDefaultShaderUniforms(type);
    const Catalog::DefaultShaderUse use = Catalog::getDefaultShaderUse(scene, type);
    for (const CustomUniformBlock* block : use.blocks) appendUniformMembers(members, *block, values);
    const std::shared_ptr<const ShaderManifest> manifest = McpValues::findManifest(shader);
    UniformChain chain;
    chain.values = &values;
    chain.manifest = manifest.get();
    annotateEffectiveValues(members, chain);
    Json unused = Json::array();
    if (!use.blocks.empty()) {
        for (const auto& value : values) {
            if (std::none_of(members.begin(), members.end(), [&](const Json& m) { return m.value("name", "") == value.first; }))
                unused.push_back(value.first);
        }
    }
    Json data = {{"shader_type", sceneDefaultTypeName(type)}, {"default_shader", shader}, {"used_by", use.users},
                 {"build_failed", use.users > 0 && use.failed == use.users}, {"members", members},
                 {"warnings", Catalog::getShaderUniformWarnings(use.blocks)}, {"values", uniformValuesJson(values)},
                 {"unused_values", unused}, {"property", Catalog::getDefaultShaderUniformsProperty(type)}};
    if (manifest) data["parameters"] = manifestParameters(manifest.get(), values);
    return data;
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

// ---- global shader values (ShaderGlobals): what a shader's globals blocks and texture
// uniforms read, and the members bound to globals

const char* globalsMemberGlsl(ShaderUniformType type) {
    return type == ShaderUniformType::MAT4 ? "mat4" : type == ShaderUniformType::MAT3 ? "mat3" : uniformTypeName(type);
}

void addGlobalMember(Json& list, const std::string& member, const char* stage, const std::string& glsl) {
    for (const Json& entry : list) {
        if (entry.value("member", "") == member && entry.value("stage", "") == stage) return;
    }
    ShaderGlobalValue value;
    const bool exists = ShaderGlobals::get(ShaderGlobals::findID(member), value);
    Json entry = {{"member", member}, {"stage", stage}, {"global", member}, {"glsl", glsl}, {"declared", exists && value.declared}};
    if (!exists) {
        entry["type"] = nullptr;
        entry["value"] = nullptr;
    } else {
        entry["type"] = ShaderGlobals::getTypeName(value.type);
        if (ShaderGlobals::isTexture(value.type))
            entry["value"] = value.texture.isFramebuffer() ? std::string("<render target>") : value.texture.getPath(0);
        else
            entry["value"] = ShaderGlobalsJson::valueToJson(value.type, value.arraySize, value.values);
    }
    list.push_back(entry);
}

void appendGlobalMembers(Json& list, const ShaderLibraryBindings& bindings) {
    auto block = [&list](const std::shared_ptr<GlobalsBlockLayout>& layout, const char* stage) {
        if (!layout) return;
        for (const GlobalsBlockLayout::Member& member : layout->members) {
            std::string glsl = globalsMemberGlsl(member.type);
            if (member.arrayCount > 1) glsl += "[" + std::to_string(member.arrayCount) + "]";
            addGlobalMember(list, member.name, stage, glsl);
        }
    };
    block(bindings.vsGlobals, "vertex");
    block(bindings.fsGlobals, "fragment");
    for (const GlobalTextureSlot& texture : bindings.globalTextures)
        addGlobalMember(list, ShaderGlobals::nameOf(texture.globalId), "texture", texture.cube ? "textureCube" : "texture2D");
}

// the globals the loaded shaders of the entity's component read (a mesh: every submesh, or one)
Json componentGlobalMembers(Scene* scene, Entity entity, ComponentType component, int submesh) {
    Json list = Json::array();
    if (component == ComponentType::MeshComponent) {
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
            for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
                if (submesh >= 0 && (int)s != submesh) continue;
                appendGlobalMembers(list, mesh->submeshes[s].libraryBindings);
                appendGlobalMembers(list, mesh->submeshes[s].depthLibraryBindings);
            }
        }
    } else if (UIComponent* ui = component == ComponentType::UIComponent ? scene->findComponent<UIComponent>(entity) : nullptr) {
        appendGlobalMembers(list, ui->libraryBindings);
    } else if (PointsComponent* points = component == ComponentType::PointsComponent ? scene->findComponent<PointsComponent>(entity) : nullptr) {
        appendGlobalMembers(list, points->libraryBindings);
    } else if (LineRendererComponent* lines = component == ComponentType::LineRendererComponent ? scene->findComponent<LineRendererComponent>(entity) : nullptr) {
        appendGlobalMembers(list, lines->libraryBindings);
    } else if (SkyboxComponent* sky = component == ComponentType::SkyboxComponent ? scene->findComponent<SkyboxComponent>(entity) : nullptr) {
        appendGlobalMembers(list, sky->libraryBindings);
    } else if (TrailRendererComponent* trail = component == ComponentType::TrailRendererComponent ? scene->findComponent<TrailRendererComponent>(entity) : nullptr) {
        appendGlobalMembers(list, trail->libraryBindings);
    }
    return list;
}

Json globalBindingsJson(const ShaderGlobalBindings& bindings) {
    Json json = Json::object();
    for (const auto& binding : bindings) json[binding.first] = binding.second;
    return json;
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
Json manifestParameters(const ShaderManifest* manifest, const ShaderUniformValues& values, const UniformChain* chain) {
    Json list = Json::array();
    if (!manifest) return list;
    const Json described = McpValues::manifestToJson("", *manifest)["properties"];
    for (const ShaderManifestProperty& property : manifest->properties) {
        Json entry = described[list.size()];
        entry["value"] = vec4Array(ShaderUniforms::has(values, property.member) ? ShaderUniforms::get(values, property.member) : property.defaultValue);
        entry["has_value"] = ShaderUniforms::has(values, property.member);
        // what it renders with, and where that comes from (the scene's default-shader value, ...)
        if (chain) {
            UniformSource source;
            entry["effective"] = vec4Array(ShaderUniforms::resolve(*chain, property.member, &source));
            entry["source"] = uniformSourceName(source);
        }
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
    // shader_type without an entity: the scene's default shader of that type
    const bool sceneDefault = !pass && args.contains("shader_type") && (!args.contains("entity") || args["entity"].is_null());
    Target where;
    if (!resolveTarget(call, where, !pass && !sceneDefault)) return;
    Scene* scene = where.scene->scene;
    Json members = Json::array();
    Json warnings;
    if (sceneDefault) {
        ShaderType type;
        if (!sceneDefaultShaderType(call, type)) return;
        Json data = sceneDefaultUniformsJson(scene, type, members);
        const std::string shader = data["default_shader"].get<std::string>();
        std::string text = shader.empty() ? std::string("The scene has no default ") + sceneDefaultTypeName(type) + " shader"
                                          : "Scene default " + std::string(sceneDefaultTypeName(type)) + " shader " + shader + ": used by " +
                                                std::to_string(data["used_by"].get<int>()) + " object(s), " + std::to_string(members.size()) +
                                                " member(s)";
        text += ", " + std::to_string(data["values"].size()) + " value(s) set.";
        if (shader.empty()) data["next"] = noSceneDefaultShaderNext(type);
        call->ok(text, data);
        return;
    }
    if (pass) {
        const std::vector<PostProcessPass>& passes = scene->getPostProcessPasses();
        int index;
        if (!passIndexArg(call, passes, index)) return;
        const PostProcessPass& p = passes[index];
        bool buildFailed = false;
        passUniformMembers(members, warnings, p, buildFailed);
        // the globals the pass's shader reads, from its reflection
        Json globals = Json::array();
        std::shared_ptr<ShaderRender> passShader = ShaderPool::get(ShaderType::POSTPROCESS, 0, ShaderPool::registerCustomShader(p.shader));
        if (!buildFailed && passShader && passShader->isCreated()) {
            ShaderLibraryBindings bindings;
            bindings.vsGlobals = ShaderGlobals::acquireLayout(passShader->shaderData, ShaderGlobals::VERTEX_BLOCK);
            bindings.fsGlobals = ShaderGlobals::acquireLayout(passShader->shaderData, ShaderGlobals::FRAGMENT_BLOCK);
            appendGlobalMembers(globals, bindings);
        }
        call->ok("Post-process pass " + std::to_string(index) + " (" + p.shader + "): " + std::to_string(members.size()) + " member(s).",
                 Json{{"pass_index", index}, {"shader", p.shader}, {"enabled", p.enabled}, {"build_failed", buildFailed},
                      {"members", members}, {"warnings", warnings}, {"values", uniformValuesJson(p.uniforms)},
                      {"globals", globals}, {"global_bindings", globalBindingsJson(p.shaderGlobalBindings)}});
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
        const Catalog::UniformChainRefs submeshChain = Catalog::getShaderUniformChain(scene, where.entity, where.component, submesh);
        annotateEffectiveValues(members, submeshChain.chain);
        Json data = {{"entity", McpRefs::entityJson(where.scene, where.entity)},
                     {"component", McpRefs::componentName(where.component)},
                     {"submesh", submesh},
                     {"property", property},
                     {"material_shader", sub.material.shader},
                     {"effective_shader", shader},
                     {"build_failed", Catalog::isCustomShaderBuildFailed(scene, where.entity, where.component)},
                     {"members", members},
                     {"warnings", Catalog::getShaderUniformWarnings(blocks)},
                     {"values", uniformValuesJson(sub.material.shaderParams)},
                     {"globals", componentGlobalMembers(scene, where.entity, where.component, submesh)},
                     {"global_bindings", globalBindingsJson(sub.material.shaderGlobalBindings)}};
        if (manifest) data["parameters"] = manifestParameters(manifest.get(), sub.material.shaderParams, &submeshChain.chain);
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
    const Catalog::UniformChainRefs chain = Catalog::getShaderUniformChain(scene, where.entity, where.component);
    annotateEffectiveValues(members, chain.chain);
    Json data = {{"entity", McpRefs::entityJson(where.scene, where.entity)},
                 {"component", McpRefs::componentName(where.component)},
                 {"custom_shader", *custom},
                 {"effective_shader", custom->empty() ? scene->getDefaultCustomShader(shaderType) : *custom},
                 {"build_failed", Catalog::isCustomShaderBuildFailed(scene, where.entity, where.component)},
                 {"members", members},
                 {"warnings", warnings},
                 {"values", uniformValuesJson(*values)},
                 {"globals", componentGlobalMembers(scene, where.entity, where.component, -1)}};
    if (const ShaderGlobalBindings* bindings = Catalog::getPropertyRef<ShaderGlobalBindings>(scene, where.entity, where.component, "shaderGlobalBindings"))
        data["global_bindings"] = globalBindingsJson(*bindings);
    // a mesh's depth fork declares blocks of its own, listed in the same members
    if (std::string* depth = Catalog::getPropertyRef<std::string>(scene, where.entity, where.component, "customDepthShader")) {
        data["custom_depth_shader"] = *depth;
        data["depth_build_failed"] = Catalog::isCustomDepthShaderBuildFailed(scene, where.entity);
    }
    // a shader with a manifest (a library shader) lists its parameters with labels and defaults
    if (const std::shared_ptr<const ShaderManifest> manifest = McpValues::findManifest(data["effective_shader"].get<std::string>())) {
        data["parameters"] = manifestParameters(manifest.get(), *values, &chain.chain);
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
    // shader_type without an entity: the scene's default shader of that type
    const bool sceneDefault = !pass && args.contains("shader_type") && (!args.contains("entity") || args["entity"].is_null());
    Target where;
    if (!resolveTarget(call, where, !pass && !sceneDefault)) return;
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
    if (sceneDefault) {
        ShaderType type;
        if (!sceneDefaultShaderType(call, type)) return;
        const std::string shader = scene->getDefaultCustomShader(type);
        const std::shared_ptr<const ShaderManifest> manifest = McpValues::findManifest(shader);
        const ShaderManifestProperty* declaredProperty = manifest ? manifest->findProperty(name) : nullptr;
        if (!remove && manifest && !declaredProperty) {
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
        ShaderUniformValues values = scene->getDefaultShaderUniforms(type);
        if (remove) {
            if (!ShaderUniforms::remove(values, name)) {
                call->fail(std::string("The scene's default ") + sceneDefaultTypeName(type) + " shader has no value '" + name + "'.");
                return;
            }
        } else {
            ShaderUniforms::set(values, name, value);
        }
        const char* property = Catalog::getDefaultShaderUniformsProperty(type);
        if (!addSceneCommand(sceneId, new ScenePropertyCmd<ShaderUniformValues>(project, sceneId, property, values))) {
            call->fail("Could not change the scene's default shader value.");
            return;
        }
        Json members = Json::array();
        Json data = sceneDefaultUniformsJson(scene, type, members);
        std::string note;
        if (!shader.empty() && !remove && !declaredProperty) note = uniformSetNote(members, name, declared);
        declared = declared || declaredProperty != nullptr;
        Json result = {{"shader_type", sceneDefaultTypeName(type)}, {"property", property}, {"name", name}, {"declared", declared},
                       {"used_by", data["used_by"]}};
        if (!remove) result["value"] = vec4Array(value);
        if (shader.empty()) {
            result["next"] = noSceneDefaultShaderNext(type);
            note = " " + result["next"].get<std::string>();
        }
        call->ok((remove ? "Removed " : "Set ") + name + " on the scene's default " + sceneDefaultTypeName(type) + " shader (inherited by " +
                 std::to_string(data["used_by"].get<int>()) + " object(s))." + note, result);
        return;
    }
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
        ProjectChecks::isUiEffectFile(project->getProjectPath() / FileUtils::pathFromUtf8(args["shader"].get<std::string>()))) {
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
            // no shader to compile: the scenes' own findings still apply (Stochastic Tiling,
            // Line and Trail Renderers)
            std::vector<ProjectChecks::Finding> sceneFindings = ProjectChecks::stochasticTilingFindings(project);
            const std::vector<ProjectChecks::Finding> lines = ProjectChecks::lineTrailFindings(project);
            sceneFindings.insert(sceneFindings.end(), lines.begin(), lines.end());
            const std::vector<ProjectChecks::Finding> particleChecks = ProjectChecks::particleFindings(project);
            sceneFindings.insert(sceneFindings.end(), particleChecks.begin(), particleChecks.end());
            std::string text = "No custom shader is used by the loaded scenes.";
            Json findings = Json::array();
            for (const ProjectChecks::Finding& finding : sceneFindings) {
                findings.push_back(ProjectChecks::toJson(finding));
                text += "\n" + finding.severity + ": " + finding.message;
            }
            call->ok(text, Json{{"results", Json::array()}, {"failed", 0}, {"findings", findings}});
            return;
        }
    }
    // with every shader in use: the materials reading the scene from the opaque queue (phase 3)
    std::vector<ProjectChecks::Finding> stateFindings = args.contains("shader")
        ? std::vector<ProjectChecks::Finding>() : ProjectChecks::sceneInputFindings(project);
    // and Stochastic Tiling's warnings (tilted normal maps under Rotation, no effect)
    if (!args.contains("shader")) {
        const std::vector<ProjectChecks::Finding> stochastic = ProjectChecks::stochasticTilingFindings(project);
        stateFindings.insert(stateFindings.end(), stochastic.begin(), stochastic.end());
        // Line and Trail Renderers: a hairline sampling a texture, a trail with Time 0
        const std::vector<ProjectChecks::Finding> lines = ProjectChecks::lineTrailFindings(project);
        stateFindings.insert(stateFindings.end(), lines.begin(), lines.end());
        // Particle Systems: legacy ones, points with a render mode, the mobile budget
        const std::vector<ProjectChecks::Finding> particleChecks = ProjectChecks::particleFindings(project);
        stateFindings.insert(stateFindings.end(), particleChecks.begin(), particleChecks.end());
    }
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
        for (const Json& finding : findings) {
            if (finding.value("severity", std::string()) == "warning")
                text += "\nwarning: " + finding.value("path", std::string()) + ": " + finding.value("message", std::string());
        }
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
    // the sampler is part of the TexturePool key, so the new settings are their own GPU texture
    const Entity entity = where.entity;
    const ComponentType component = where.component;
    if (!addSceneCommand(where.scene->id, new PropertyCmd<Texture>(call->ctx().project(), where.scene->id, entity, component,
                                                                   propertyName, modified))) {
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
    if (!resolveExisting(call, "path", file, false, PathFor::Write)) return;
    if (extensionOf(file) != ".png") file += ".png";
    const std::string rel = projectRelative(project, file);
    // project-relative like the paths of set_properties / create_entity, which only take assets
    if (!project->isInsideAssetsPath(file)) {
        const std::string assets = projectRelative(project, project->getAssetsPath());
        call->fail("path is project-relative, like the texture paths the other tools take: \"" + rel +
                   "\" is outside the assets directory (\"" + assets + "\"), so no component could use it. Use \"" +
                   projectRelative(project, project->getAssetsPath() / FileUtils::pathFromUtf8(rel)) + "\".");
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
// import_unity_particles: a Unity .prefab / .unity (absolute path) as engine prefabs (particles spec 6.10)
void importUnityParticlesTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    if (!args.contains("path") || !args["path"].is_string() || args["path"].get<std::string>().empty()) {
        call->fail("path is required: the absolute path of a .prefab or .unity file inside a Unity project.");
        return;
    }
    unity::ImportOptions options;
    options.file = fs::u8path(args["path"].get<std::string>()).lexically_normal();
    if (!options.file.is_absolute()) {
        call->fail("path must be absolute (the Unity project is outside the engine project).");
        return;
    }
    if (args.contains("object") && args["object"].is_string()) options.object = args["object"].get<std::string>();
    if (args.contains("dest") && args["dest"].is_string()) options.dest = args["dest"].get<std::string>();
    if (args.contains("unity_project") && args["unity_project"].is_string()) {
        options.unityProject = fs::u8path(args["unity_project"].get<std::string>()).lexically_normal();
    }
    options.dryRun = args.value("dry_run", false);
    options.overwrite = args.value("overwrite", false);
    // the output folder: inside the project once links are resolved
    fs::path dest;
    std::string error;
    if (!McpRefs::resolveProjectWritePath(project, options.dest, dest, error, false)) {
        call->fail(error);
        return;
    }
    const unity::ImportResult result = unity::importParticlesIntoProject(project, options);
    if (!result.ok) {
        call->fail(result.error);
        return;
    }
    int notes = 0;
    for (const Json& system : result.report["systems"]) notes += (int)system["notes"].size();
    notes += (int)result.report["warnings"].size();
    std::string message = std::string(options.dryRun ? "Would import " : "Imported ") + std::to_string(result.report["systems"].size()) +
                          " particle system(s) into " + std::to_string(result.prefabs.size()) + " prefab(s)";
    message += notes ? " with " + std::to_string(notes) + " note(s) (see report)." : ".";
    if (!options.dryRun) message += " Instantiate a prefab with instantiate_prefab; undo removes the files.";
    Json data = {{"prefabs", result.prefabs}, {"report", result.report}};
    if (!result.reportFile.empty()) data["report_file"] = result.reportFile;
    call->ok(message, data);
}

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
        const fs::path g = FileUtils::pathFromUtf8(glob);
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
                fileGlob = FileUtils::pathToGenericUtf8(g.lexically_normal());
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
    const fs::path scopeRel = FileUtils::pathFromUtf8(projectRelative(project, scope));
    const bool scopeSkipped = scope != root && (ProjectChecks::isSkippedDir(scope) || ProjectChecks::isInSkippedDir(scopeRel));
    if (!scopeSkipped && insideProject(scope) && fs::is_directory(scope, ec)) {
        for (fs::recursive_directory_iterator it(scope, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec;
             it.increment(ec)) {
            const fs::path& path = it->path();
            const std::string name = FileUtils::pathToUtf8(path.filename());
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

    const std::vector<std::string> types = {"any", "texture", "model", "audio", "font", "html", "css", "script", "scene", "prefab", "material", "shader", "animation", "vertex_animation", "shake", "folder", "other"};

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
            {"search", string("Project search: t:texture l:Hero ext: path: re: ref: missing: (asset labels too).")},
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
            "One project file's details by type: image size/channels (synthesized PNG: its recipe, changed sources, "
            "regenerate arguments); model meshes, materials, animations, nodes (an FBX: of its glTF conversion, fbx_import.state); "
            "audio format/duration; .material fields, "
            ".anim clip settings, tracks, events, users; .vat clips, staleness, users; "
            "linked submeshes; shader includes, forks using it; Lua syntax; scene status; unsaved code editor edits.";
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
            "Read a project text file (as on disk); the result text is the content.";
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
            "Create or change a project text file (undo scope project; for clients without file access) and "
            "validate it: Lua syntax, forks using it recompiled (errors with lines), materials, .anim and .vat parsed. content = "
            "whole file, or edits [{old_text, new_text}] (old_text must match once unless replace_all). Refuses "
            "project.yaml, .scene, .prefab and files with unsaved code editor edits.";
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
            "shader forks, Animation Players (as the Project window does); or copy it (a copied prefab or clip gets a new GUID). The "
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
            "Copy files/folders from absolute local paths into a project folder (made if missing), as a drop on "
            "the Project window (assets under the assets dir). Remote: URLs in sources, fonts [\"Inter:400,700\"] "
            "(Google Fonts -> .ttf + @font-face css), icons [\"lucide:play\"] (.svg), from_document (import every "
            "remote URL a .html/.css uses and rewrite it). Videos become AV1+Opus .webm (FFmpeg), .fbx is converted "
            "to glTF (fbx_imports).";
        tool.inputSchema = object({
            {"sources", array(string(""), "Absolute paths or http(s) URLs.")},
            {"to_dir", assetPath("e.g. \"assets/sprites\" (remote default assets/ui/imported).")},
            {"overwrite", withDefault(boolean(""), false)},
            {"fonts", array(string(""), "")},
            {"icons", array(string(""), "")},
            {"from_document", assetPath("")},
            {"rewrite", withDefault(boolean(""), true)}
        });
        tool.annotations.openWorld = true;
        tool.handler = importFilesTool;
        registry.addTool(std::move(tool));
    }

    const Json materialValues = anyObject(
        "baseColorFactor [r,g,b,a], metallicFactor, roughnessFactor, alphaMode, alphaCutoff, emissiveFactor, "
        "foliageTransmission, *Texture (path|null), *TexCoord, stochasticTiling (Lit: hides tiling repetition, up to 3x the "
        "reads) with stochasticCellSize (0.25-4) / BlendSharpness / Contrast / Rotation (0-1); emissiveStrength; pbr "
        "{clearcoat|sheen|anisotropy|iridescence|transmission|volume|specular|hex: {enabled, fields, map: path|null}} merged "
        "(jasper://docs/materials); shader (\"library:<name>\"), shaderParams {member: value}, shaderTextures {uniform: path}, "
        "shaderFeatures [defines]|{define: bool}, render state (blendMode, cullMode, depth*, sortPriority, lighting, stencil "
        "{...}, colorWrite), extraPass; null removes. Fields: jasper://docs/shaders.");

    {
        ToolDef tool;
        tool.name = "create_material";
        tool.title = "Create Material";
        tool.description =
            "Create a shared .material file (linked submeshes follow its edits) from defaults or a mesh submesh's "
            "material (entity + submesh; linked unless link=false), plus values.";
        tool.inputSchema = object({
            {"path", assetPath("")},
            {"values", materialValues},
            {"scene", sceneRef()},
            {"entity", entityRef("Mesh to copy from (and link).")},
            {"submesh", withDefault(integer("", 0, 1024), 0)},
            {"link", boolean("Default: true with entity.")},
            {"overwrite", withDefault(boolean(""), false)},
            {"allow_data_loss", boolean("")}
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
            {"values", anyObject("As create_material's values.")},
            {"allow_data_loss", boolean("")}
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
            "One-click material setups (outlines, see-through silhouette, stencil masks, draw-once transparency, the built-in "
            "Lit's car paint, velvet, brushed metal, soap bubble, clear glass, tiled floor...): "
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
            {"submesh", integer("", 0, 1024)},
            {"path", assetPath("")},
            {"allow_data_loss", boolean("")}
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
            "Copy a built-in shader to <directory>/<name>.vert/.frag and point a target at it: "
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
            "A custom or library shader's parameters with each one's effective value and source, of entity (+ component, "
            "+ submesh), pass_index or shader_type (the scene's default shader). Known once compiled.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"component", componentName("Default: auto-detect.")},
            {"submesh", integer("Default 0 if its material has a shader.")},
            {"pass_index", integer("A post-process pass instead.")},
            {"shader_type", stringEnum("", {"mesh", "ui", "sky", "points", "lines"})}
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
            "Set or remove one parameter of a custom or library shader of entity (+ component, + submesh), "
            "pass_index or shader_type (the scene's default shader). Undeclared names are stored but unused.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"component", componentName("Default: auto-detect.")},
            {"submesh", integer("Default 0 if its material has a shader.")},
            {"pass_index", integer("A post-process pass instead.")},
            {"shader_type", stringEnum("", {"mesh", "ui", "sky", "points", "lines"})},
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
            "shader; ui, points, lines, sky: a component's customShader; postprocess: a post_process pass), "
            "lighting modes; detail (or name) adds parameters, textures, features/keywords, required inputs, "
            "default state, rules; scene_inputs/cost_note: reads the scene (an extra pass per camera). Usage: "
            "jasper://docs/shaders.";
        tool.inputSchema = object({
            {"type", stringEnum("", {"mesh", "ui", "points", "lines", "sky", "postprocess"})},
            {"name", string("One shader (\"toon\" or \"library:toon\"); implies detail.")},
            {"detail", withDefault(boolean(""), false)}
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
            "Compile shader forks or library shaders now: errors (line, message, log). shader: a customShader "
            "base (\"shaders/Water\"), its .vert/.frag, or \"library:<name>[#FEATURES]\"; shader_type if unused "
            "yet. No shader: every custom and library shader in use.";
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
            {"path", assetPath("A .png in the assets dir (\"assets/sprites/coin.png\"), then a texture path.")},
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
        tool.name = "import_unity_particles";
        tool.title = "Import Unity Particles";
        tool.group = "particles";   // out of core to fit the budget (S31)
        tool.description =
            "Import the ParticleSystems of a Unity .prefab/.unity (absolute path; the Unity project is only read) as "
            "engine prefabs (S20 systems: each renderer's material in its Renderer module) and textures. Returns the prefabs and a report (per module imported/"
            "approximated/unsupported, notes). jasper://docs/particles";
        tool.inputSchema = object({
            {"path", string("Absolute .prefab or .unity path.")},
            {"object", string("GameObject path, e.g. \"Player/Camera/EndParticles\".")},
            {"dest", withDefault(assetPath(""), "assets/fx")},
            {"unity_project", string("Default: found from path.")},
            {"dry_run", withDefault(boolean(""), false)},
            {"overwrite", withDefault(boolean(""), false)}
        }, {"path"});
        tool.annotations.openWorld = true;
        tool.group = "particles";  // S31 tool groups: listed once enabled, callable always
        tool.handler = importUnityParticlesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "search_files";
        tool.title = "Search Files";
        tool.description =
            "Grep project text files (scripts, shaders, docs, configs). ECMAScript regex; matches [{path, "
            "line, text, context}].";
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

// ------------------------------------------------------------------ textures (S14 R27)

namespace {

// once the dependency index has its first build (the references decide the inferred type)
void textureWhenIndexed(const CallPtr& call, std::function<void()> then) {
    Project* project = call->ctx().project();
    project->refreshAssetDatabase(true);
    if (project->getAssetIndex().isReady()) {
        then();
        return;
    }
    call->waitUntil([project] { return project->getAssetIndex().isReady(); }, 60000, [call, then](bool timedOut) {
        if (timedOut) {
            call->fail("The asset index is still being built; try again in a moment.");
            return;
        }
        then();
    });
}

// MCP's snake_case keys of the import settings and their sidecar (YAML) names
const std::vector<std::pair<std::string, std::string>>& textureSettingFields() {
    static const std::vector<std::pair<std::string, std::string>> fields = {
        {"type", "type"}, {"shape", "shape"}, {"srgb", "srgb"}, {"alpha_source", "alphaSource"},
        {"alpha_is_transparency", "alphaIsTransparency"}, {"readable", "readable"}, {"max_size", "maxSize"},
        {"resize_filter", "resizeFilter"}, {"mipmaps", "mipmaps"}, {"mip_filter", "mipFilter"}, {"mip_limit", "mipLimit"},
        {"preserve_coverage", "preserveCoverage"}, {"alpha_cutoff", "alphaCutoff"}, {"flip_green", "flipGreen"},
        {"channel", "channel"}, {"quality", "quality"}, {"effort", "effort"}, {"streamable", "streamable"},
        {"ship_source", "shipSource"}};
    return fields;
}

const std::vector<std::pair<std::string, std::string>>& texturePlatformFields() {
    static const std::vector<std::pair<std::string, std::string>> fields = {
        {"override", "override"}, {"max_size", "maxSize"}, {"format", "format"}, {"quality", "quality"},
        {"effort", "effort"}, {"rdo", "rdo"}};
    return fields;
}

std::string textureSnakeOf(const std::string& yamlKey) {
    for (const auto& [snake, yaml] : textureSettingFields())
        if (yaml == yamlKey) return snake;
    for (const auto& [snake, yaml] : texturePlatformFields())
        if (yaml == yamlKey) return snake;
    return yamlKey;
}

Json textureYamlToJson(const YAML::Node& node) {
    if (!node || node.IsNull()) return nullptr;
    if (node.IsMap()) {
        Json out = Json::object();
        for (const auto& entry : node) out[textureSnakeOf(entry.first.as<std::string>())] = textureYamlToJson(entry.second);
        return out;
    }
    if (node.IsSequence()) {
        Json out = Json::array();
        for (const auto& item : node) out.push_back(textureYamlToJson(item));
        return out;
    }
    const std::string text = node.as<std::string>();
    if (text == "true" || text == "false") return text == "true";
    char* end = nullptr;
    const long integer = std::strtol(text.c_str(), &end, 10);
    if (end && *end == '\0' && !text.empty()) return integer;
    const double number = std::strtod(text.c_str(), &end);
    if (end && *end == '\0' && !text.empty()) return number;
    return text;
}

YAML::Node textureJsonToYaml(const Json& value) {
    if (value.is_boolean()) return YAML::Node(value.get<bool>());
    if (value.is_number_integer()) return YAML::Node(value.get<long long>());
    if (value.is_number()) return YAML::Node(value.get<double>());
    if (value.is_string()) return YAML::Node(value.get<std::string>());
    return YAML::Node();
}

Json resolvedTextureJson(const ResolvedTextureSettings& r) {
    return Json{{"type", textureImportTypeName(r.type)}, {"shape", textureShapeName(r.shape)}, {"srgb", r.srgb},
                {"alpha_source", alphaSourceName(r.alphaSource)}, {"alpha_is_transparency", r.alphaIsTransparency},
                {"readable", r.readable}, {"max_size", r.maxSize}, {"resize_filter", mipFilterName(r.resizeFilter)},
                {"mipmaps", r.mipmaps}, {"mip_filter", mipFilterName(r.mipFilter)}, {"mip_limit", r.mipLimit},
                {"preserve_coverage", r.preserveCoverage}, {"alpha_cutoff", r.alphaCutoff}, {"flip_green", r.flipGreen},
                {"channel", textureChannelName(r.channel)}, {"quality", textureQualityName(r.quality)},
                {"effort", textureEffortName(r.effort)}, {"streamable", r.streamable},
                {"format", r.format.empty() ? Json(nullptr) : Json(r.format)}, {"rdo", r.rdo < 0.0f ? Json(nullptr) : Json(r.rdo)},
                {"cpu_readable", r.cpuReadable}};
}

// a texture source named by its project-relative path; its asset-relative form
bool textureArg(const CallPtr& call, const std::string& given, std::string& assetPath, std::string& projectPath) {
    Project* project = call->ctx().project();
    fs::path file;
    std::string error;
    if (!McpRefs::resolveProjectPath(project, given, file, error, true)) {
        call->fail(error);
        return false;
    }
    file = file.lexically_normal();
    projectPath = w4::projectRelative(project, file);
    assetPath = project->normalizeToAssetsRelative(file).generic_string();
    if (!isTextureSource(assetPath) || !project->isInsideAssetsPath(file)) {
        call->fail(projectPath + " is not a texture source in the assets directory (png, jpg, tga, bmp, psd, gif, hdr, exr, svg, ktx2).");
        return false;
    }
    return true;
}

bool textureFamiliesArg(const CallPtr& call, const std::string& key, std::vector<TextureFamily>& out, bool allByDefault) {
    out.clear();
    const Json& args = call->args();
    if (!args.contains(key) || args[key].is_null()) {
        out = {TextureFamily::Desktop};
        if (allByDefault) {
            out = {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::Web};
            if (call->ctx().project()->getTextureSettings().androidEtc2Fallback) out.push_back(TextureFamily::AndroidEtc2);
        }
        return true;
    }
    const Json list = args[key].is_array() ? args[key] : Json::array({args[key]});
    for (const Json& item : list) {
        TextureFamily family;
        if (!item.is_string() || !textureFamilyFromName(item.get<std::string>(), family)) {
            call->fail(key + ": " + item.dump() + " is not desktop, mobile, android_etc2 or web.");
            return false;
        }
        if (std::find(out.begin(), out.end(), family) == out.end()) out.push_back(family);
    }
    return true;
}

bool texturePathsArg(const CallPtr& call, std::vector<std::string>& assetPaths, std::vector<std::string>& projectPaths) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    if (!args.contains("paths") || args["paths"].is_null() || (args["paths"].is_string() && args["paths"].get<std::string>() == "all")) {
        for (const std::string& path : projectTextureSources(*project, true)) {
            assetPaths.push_back(path);
            projectPaths.push_back(w4::projectRelative(project, project->getAssetsPath() / path));
        }
        return true;
    }
    if (!args["paths"].is_array()) {
        call->fail("paths is a list of project-relative texture paths (or \"all\").");
        return false;
    }
    for (const Json& item : args["paths"]) {
        if (!item.is_string()) {
            call->fail("paths holds project-relative texture paths.");
            return false;
        }
        std::string assetPath, projectPath;
        if (!textureArg(call, item.get<std::string>(), assetPath, projectPath)) return false;
        assetPaths.push_back(assetPath);
        projectPaths.push_back(projectPath);
    }
    return true;
}

void getTextureImportSettingsTool(const CallPtr& call) {
    textureWhenIndexed(call, [call] {
        Project* project = call->ctx().project();
        std::string assetPath, projectPath;
        if (!textureArg(call, call->args().value("path", ""), assetPath, projectPath)) return;
        std::vector<TextureFamily> families;
        if (!textureFamiliesArg(call, "platform", families, true)) return;
        TextureCookInputs inputs;
        if (!gatherTextureInputs(*project, assetPath, inputs)) {
            std::string reason;
            TextureImportSettings ignored;
            TextureImportSettings::load(inputs.absolute.string(), ignored, &reason);
            call->fail(projectPath + ": its import settings cannot be read: " + reason);
            return;
        }
        const TextureProjectSettings settings = textureProjectSettingsOf(*project);
        Json resolved = Json::object();
        Json warnings = Json::array();
        for (const std::string& conflict : TextureTypeInference::conflicts(inputs.settings.resolvedType(), inputs.inference))
            warnings.push_back(conflict);
        for (TextureFamily family : families) {
            ResolvedTextureSettings r = inputs.settings.resolve(family, settings);
            r.cpuReadable = r.cpuReadable || inputs.inference.cpuReadable || r.readable;
            Json entry = resolvedTextureJson(r);
            TextureCookState state;
            if (TextureCookQueue::get().getProject() == project && TextureCookQueue::get().getState(assetPath, family, state)) {
                if (!state.plan.key.empty()) entry["cooked_format"] = state.plan.choice.label();
                for (const std::string& warning : state.warnings)
                    if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) warnings.push_back(warning);
            }
            resolved[textureFamilyName(family)] = entry;
        }
        const YAML::Node stored = SetAssetMetaCmd::read(inputs.absolute, TextureImportSettings::metaKey);
        Json inferred = {{"type", textureImportTypeName(inputs.inference.type)}, {"reason", inputs.inference.reason},
                         {"linear", inputs.inference.linear}, {"shape", textureShapeName(inputs.inference.shape)},
                         {"cpu_readable", inputs.inference.cpuReadable}, {"source_only", inputs.inference.sourceOnly},
                         {"reads_source", inputs.inference.readsSource}};
        if (!inputs.inference.cpuReason.empty()) inferred["cpu_reason"] = inputs.inference.cpuReason;
        Json data = {{"path", projectPath}, {"stored", textureYamlToJson(stored)}, {"inferred", inferred},
                     {"resolved", resolved}, {"warnings", warnings}};
        call->ok(projectPath + ": " + textureImportTypeName(inputs.settings.resolvedType()) +
                 (stored && !stored.IsNull() ? "" : " (inferred: " + inputs.inference.reason + ")"), data);
    });
}

void setTextureImportSettingsTool(const CallPtr& call) {
    textureWhenIndexed(call, [call] {
        Project* project = call->ctx().project();
        const Json& args = call->args();
        std::vector<std::string> assetPaths, projectPaths;
        if (!args.contains("paths") || !args["paths"].is_array() || args["paths"].empty()) {
            call->fail("paths is a non-empty list of project-relative texture paths.");
            return;
        }
        if (!texturePathsArg(call, assetPaths, projectPaths)) return;
        if (!args.contains("settings") || !args["settings"].is_object() || args["settings"].empty()) {
            call->fail("settings is an object of import settings (type, srgb, max_size, quality...); null removes one.");
            return;
        }
        const Json& settings = args["settings"];
        TextureFamily family = TextureFamily::Desktop;
        const bool perPlatform = args.contains("platform") && !args["platform"].is_null();
        if (perPlatform && (!args["platform"].is_string() || !textureFamilyFromName(args["platform"].get<std::string>(), family))) {
            call->fail("platform must be desktop, mobile, android_etc2 or web.");
            return;
        }
        const auto& fields = perPlatform ? texturePlatformFields() : textureSettingFields();
        std::vector<std::pair<std::string, YAML::Node>> edits;   // yaml key, value (null: removed)
        for (const auto& [key, value] : settings.items()) {
            auto field = std::find_if(fields.begin(), fields.end(), [&](const auto& f) { return f.first == key; });
            if (field == fields.end()) {
                std::string valid;
                for (const auto& f : fields) valid += (valid.empty() ? "" : ", ") + f.first;
                call->fail("settings." + key + " is not " + (perPlatform ? "a platform override" : "an import setting") + " (" + valid + ").");
                return;
            }
            const YAML::Node yaml = value.is_null() ? YAML::Node() : textureJsonToYaml(value);
            if (!value.is_null()) {
                if (!yaml) {
                    call->fail("settings." + key + " must be a boolean, number or string.");
                    return;
                }
                std::string error;
                // a platform field is checked as the top-level one of its name (maxSize: a power of two)
                const std::string checked = perPlatform ? (field->second == "maxSize" || field->second == "quality" || field->second == "effort" ? field->second : std::string()) : field->second;
                if (!checked.empty() && !validateTextureSettingsField(checked, yaml, error)) {
                    call->fail("settings." + key + ": " + error.substr(error.find(':') == std::string::npos ? 0 : error.find(':') + 2));
                    return;
                }
                if (perPlatform && field->second == "format" && !TextureFormatSelector::isFormatName(value.get<std::string>())) {
                    std::string names;
                    for (const std::string& name : TextureFormatSelector::formatNames()) names += (names.empty() ? "" : ", ") + name;
                    call->fail("settings.format: '" + value.get<std::string>() + "' is not one of " + names + ".");
                    return;
                }
            }
            edits.push_back({field->second, yaml});
        }
        // every texture's new section, checked whole before anything is written
        std::vector<std::pair<size_t, YAML::Node>> writes;
        for (size_t i = 0; i < assetPaths.size(); i++) {
            const fs::path absolute = project->getAssetsPath() / assetPaths[i];
            const YAML::Node stored = SetAssetMetaCmd::read(absolute, TextureImportSettings::metaKey);
            YAML::Node node = stored && stored.IsMap() ? YAML::Clone(stored) : YAML::Node(YAML::NodeType::Map);
            if (!node["version"]) node["version"] = TextureImportSettings::currentVersion;
            // an explicit choice ends the inference's say on it
            if (!perPlatform) node.remove("inferredFrom");
            auto apply = [&edits](YAML::Node target) {
                for (const auto& [key, value] : edits) {
                    if (value) target[key] = value;
                    else target.remove(key);
                }
            };
            if (perPlatform) {
                if (!node["platforms"] || !node["platforms"].IsMap()) node["platforms"] = YAML::Node(YAML::NodeType::Map);
                YAML::Node platforms = node["platforms"];
                const std::string name = textureFamilyName(family);
                if (!platforms[name] || !platforms[name].IsMap()) platforms[name] = YAML::Node(YAML::NodeType::Map);
                // (a node bound once: yaml-cpp's assignment to a bound node overwrites what it holds)
                YAML::Node platform = platforms[name];
                if (!settings.contains("override")) platform["override"] = true;
                apply(platform);
            } else {
                apply(node);
            }
            TextureImportSettings parsed;
            std::string error;
            if (!TextureImportSettings::parse(node, parsed, &error)) {
                call->fail(projectPaths[i] + ": " + error);
                return;
            }
            // the canonical form (flow style platform blocks, known order)
            writes.push_back({i, parsed.serialize()});
        }
        CommandHistory* history = project->getProjectCommandHistory();
        InteractiveEdit edit = history->beginInteractive("Set Texture Import Settings");
        Json updated = Json::array();
        for (const auto& [i, node] : writes) {
            Command* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(project->getAssetsPath() / assetPaths[i]),
                                                   TextureImportSettings::metaKey, node);
            if (command->isNoOp()) {
                delete command;
                continue;
            }
            if (!history->addCommand(command)) {
                edit.cancel();
                call->fail("Couldn't write the import settings of " + projectPaths[i] + ".");
                return;
            }
            updated.push_back(projectPaths[i]);
        }
        edit.commit();
        if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
        call->ok("Updated the import settings of " + std::to_string(updated.size()) + " texture(s); their cooks are queued. "
                 "Undo with scope project.", Json{{"updated", updated}, {"unchanged", writes.size() - updated.size()}});
    });
}

Json textureReportRows(Project* project, const std::vector<std::string>& assetPaths, const std::vector<std::string>& projectPaths,
                       const std::vector<TextureFamily>& families, std::vector<TextureReportRow>& rows) {
    TextureCookQueue& queue = TextureCookQueue::get();
    for (size_t i = 0; i < assetPaths.size(); i++) {
        TextureReportRow row;
        row.path = projectPaths[i];
        TextureImportSettings settings;
        TextureImportSettings::load((project->getAssetsPath() / assetPaths[i]).string(), settings);
        row.type = settings.resolvedType();
        row.inferredFrom = settings.inferredFrom;
        for (TextureFamily family : families) {
            TextureCookState state;
            if (!queue.getState(assetPaths[i], family, state)) state.status = TextureCookStatus::None;
            row.families[family] = state;
        }
        rows.push_back(std::move(row));
    }
    return TextureCookReport::toJson(rows);
}

void reimportTexturesTool(const CallPtr& call) {
    textureWhenIndexed(call, [call] {
        Project* project = call->ctx().project();
        TextureCookQueue& queue = TextureCookQueue::get();
        if (queue.getProject() != project) {
            call->fail("The texture cook queue is not running for this project yet; try again in a moment.");
            return;
        }
        std::vector<std::string> assetPaths, projectPaths;
        if (!texturePathsArg(call, assetPaths, projectPaths)) return;
        std::vector<TextureFamily> families;
        if (!textureFamiliesArg(call, "platforms", families, false)) return;
        for (const std::string& path : assetPaths) {
            if (!isCookableTexture(path)) continue;
            queue.invalidate(path);
            for (TextureFamily family : families) queue.request(path, family, TextureCookQueue::Selected);
        }
        const bool wait = call->args().value("wait", true);
        const int timeoutMs = (int)std::lround(std::clamp(call->args().value("timeout_s", 120.0), 1.0, 3600.0) * 1000.0);
        auto finish = [call, project, assetPaths, projectPaths, families](bool timedOut) {
            std::vector<TextureReportRow> rows;
            Json report = textureReportRows(project, assetPaths, projectPaths, families, rows);
            report["timed_out"] = timedOut;
            const std::vector<std::string> problems = TextureCookReport::problems(rows);
            int failed = 0;
            for (const TextureReportRow& row : rows)
                for (const auto& [family, state] : row.families) failed += state.status == TextureCookStatus::Failed ? 1 : 0;
            const std::string text = timedOut ? "Some cooks are still running (timed_out); get_texture_cook_report shows them later."
                                              : "Reimported " + std::to_string(rows.size()) + " texture(s)" +
                                                    (failed ? ", " + std::to_string(failed) + " cook(s) failed." : ".");
            if (failed && !timedOut) call->fail(text, report);
            else call->ok(text, report);
        };
        if (!wait) {
            finish(false);
            return;
        }
        call->waitUntil([&queue, assetPaths, families]() {
            for (const std::string& path : assetPaths) {
                for (TextureFamily family : families) {
                    if (queue.isPending(path, family))
                        return false;
                }
            }
            return true;
        }, timeoutMs, finish);
    });
}

void getTextureCookReportTool(const CallPtr& call) {
    textureWhenIndexed(call, [call] {
        Project* project = call->ctx().project();
        std::vector<std::string> assetPaths, projectPaths;
        if (!texturePathsArg(call, assetPaths, projectPaths)) return;
        std::vector<TextureFamily> families;
        if (!textureFamiliesArg(call, "platforms", families, false)) return;
        std::vector<TextureReportRow> rows;
        const Json report = textureReportRows(project, assetPaths, projectPaths, families, rows);
        ToolResult result = ToolResult::success(TextureCookReport::toText(rows), report);
        if (call->args().value("include_preview", false)) {
            // the cooked level 0 as the family's devices see it, for the first textures
            TextureCookCache& cache = TextureCookQueue::get().getCache();
            int shown = 0;
            for (const TextureReportRow& row : rows) {
                if (shown >= 4) break;
                for (const auto& [family, state] : row.families) {
                    if (state.status != TextureCookStatus::Cooked) continue;
                    std::vector<uint8_t> file;
                    if (!cache.readArtifact(state.record.key, file)) continue;
                    GpuTextureFormat format;
                    int width = 0, height = 0, faces = 1;
                    std::vector<uint8_t> pixels;
                    std::string error;
                    if (!decodeKtx2ForPreview(file, 0, format, width, height, faces, pixels, error)) continue;
                    std::vector<uint8_t> rgba;
                    if (format == GpuTextureFormat::RGBA8) {
                        rgba = std::move(pixels);
                    } else if (format == GpuTextureFormat::RGBA16F) {
                        TextureDecode::convert(format, GpuTextureFormat::RGBA8, width, height * faces, pixels.data(), pixels.size(), rgba);
                    } else {
                        const size_t channels = format == GpuTextureFormat::RG8 ? 2 : 1;
                        const size_t texels = pixels.size() / channels;
                        rgba.resize(texels * 4);
                        for (size_t t = 0; t < texels; t++) {
                            rgba[t * 4] = pixels[t * channels];
                            rgba[t * 4 + 1] = channels == 2 ? pixels[t * channels + 1] : pixels[t * channels];
                            rgba[t * 4 + 2] = channels == 2 ? 255 : pixels[t * channels];
                            rgba[t * 4 + 3] = 255;
                        }
                    }
                    std::string base64;
                    int outW = 0, outH = 0;
                    if (rgba.size() >= (size_t)width * height * 4 && encodePng(rgba.data(), width, height, 256, base64, outW, outH)) {
                        result.images.push_back({base64, "image/png"});
                        result.text += "\nPreview: " + row.path + " [" + textureFamilyName(family) + "]";
                        shown++;
                    }
                    break;
                }
            }
        }
        call->reply(std::move(result));
    });
}

} // namespace

void registerTextureTools(McpRegistry& registry) {
    using namespace schema;
    {
        ToolDef tool;
        tool.name = "get_texture_import_settings";
        tool.title = "Get Texture Import Settings";
        tool.description = "A texture's import settings: stored (null until set), inferred type and reason, resolved per "
                           "platform (desktop, mobile, android_etc2, web), warnings (a reference that does not fit the type).";
        tool.inputSchema = object({
            {"path", string("Project-relative texture path.")},
            {"platform", stringEnum("", {"desktop", "mobile", "android_etc2", "web"})}
        }, {"path"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getTextureImportSettingsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_texture_import_settings";
        tool.title = "Set Texture Import Settings";
        tool.description = "Partial update of textures' import settings (type, srgb, max_size, mipmaps, quality, effort, "
                           "flip_green...; null removes one); with platform, that platform's override (max_size, format, "
                           "quality, effort, rdo). One undo step; recooks.";
        tool.inputSchema = object({
            {"paths", array(string(""), "Project-relative texture paths.")},
            {"settings", anyObject("")},
            {"platform", stringEnum("", {"desktop", "mobile", "android_etc2", "web"})}
        }, {"paths", "settings"});
        tool.allowedWhilePlaying = false;
        tool.handler = setTextureImportSettingsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "reimport_textures";
        tool.title = "Reimport Textures";
        tool.description = "Cooks textures again (paths, or all) for platforms (default desktop); waits for the results "
                           "unless wait is false. Returns get_texture_cook_report's fields.";
        tool.inputSchema = object({
            {"paths", array(string(""), "Project-relative texture paths; omit for all.")},
            {"platforms", array(stringEnum("", {"desktop", "mobile", "android_etc2", "web"}), "")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_s", withDefault(number("", 1, 3600), 120)}
        });
        tool.allowedWhilePlaying = false;
        tool.handler = reimportTexturesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_texture_cook_report";
        tool.title = "Get Texture Cook Report";
        tool.description = "Per texture and platform: status, format, size, mips, VRAM, disk and source bytes, PSNR, cache "
                           "hit, warnings; totals per platform. include_preview adds the cooked images (first 4).";
        tool.inputSchema = object({
            {"paths", array(string(""), "Project-relative texture paths; omit for all.")},
            {"platforms", array(stringEnum("", {"desktop", "mobile", "android_etc2", "web"}), "")},
            {"include_preview", withDefault(boolean(""), false)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getTextureCookReportTool;
        registry.addTool(std::move(tool));
    }
}

}
