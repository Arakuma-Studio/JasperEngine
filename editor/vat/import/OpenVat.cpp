// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// OpenVAT (Blender) import, spec 4.6. Decoded from the format's description and its engine
// decoders (OpenVAT-Engine_Tools, MIT); none of the GPL Blender add-on's code is used.
// Files: <name>_vat.(exr|png) (offsets, + normals in the lower half when packed),
// <name>_vnrm.(exr|png) (separate normals), <name>-remap_info.json ({"os-remap": {"Min": [3],
// "Max": [3], "Frames": N}, "animations": {name: {startFrame, endFrame, framerate, looping}}}),
// and the mesh whose VAT_UV layer addresses each vertex's frame-0 texel.

#include "vat/import/VatImportInternal.h"

#include "util/FileUtils.h"
#include "util/FloatImage.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::vat;
using jasperengine::editor::FileUtils;
using jasperengine::editor::FloatImage;
using Json = nlohmann::json;

namespace jasperengine::editor::vat::importer{

namespace{

    const char* const REMAP_SUFFIX = "-remap_info";

    bool endsWith(const std::string& text, const std::string& suffix){
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    struct Located{
        fs::path folder;
        std::string name;
        std::string givenMesh;
    };

    std::string remapNameIn(const fs::path& folder){
        std::error_code ec;
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(folder, ec)){
            if (!entry.is_regular_file(ec)) continue;
            const std::string file = FileUtils::pathToUtf8(entry.path().filename());
            if (lower(FileUtils::pathToUtf8(entry.path().extension())) != ".json") continue;
            const std::string stem = FileUtils::pathToUtf8(entry.path().stem());
            if (endsWith(stem, REMAP_SUFFIX) && stem.size() > std::string(REMAP_SUFFIX).size()){
                names.push_back(stem.substr(0, stem.size() - std::string(REMAP_SUFFIX).size()));
            }
            (void)file;
        }
        std::sort(names.begin(), names.end());
        return names.empty() ? std::string() : names.front();
    }

    bool locate(const std::string& path, Located& out){
        std::error_code ec;
        fs::path p = fs::absolute(FileUtils::pathFromUtf8(path), ec).lexically_normal();
        if (FileUtils::pathToUtf8(p.filename()).empty()) p = p.parent_path();
        if (fs::is_directory(p, ec)){
            out.folder = p;
            out.name = remapNameIn(p);
            return !out.name.empty();
        }
        if (!fs::is_regular_file(p, ec)) return false;
        out.folder = p.parent_path();
        const std::string stem = FileUtils::pathToUtf8(p.stem());
        const std::string ext = lower(FileUtils::pathToUtf8(p.extension()));
        if (ext == ".json" && endsWith(stem, REMAP_SUFFIX) && stem.size() > std::string(REMAP_SUFFIX).size()){
            out.name = stem.substr(0, stem.size() - std::string(REMAP_SUFFIX).size());
            return true;
        }
        const std::string folderName = remapNameIn(out.folder);
        if (folderName.empty()) return false;
        if ((ext == ".exr" || ext == ".png") && (endsWith(stem, "_vat") || endsWith(stem, "_vnrm"))){
            out.name = stem.substr(0, stem.rfind('_'));
            return true;
        }
        if (ext == ".fbx" || ext == ".glb" || ext == ".gltf"){
            out.name = folderName;
            out.givenMesh = FileUtils::pathToUtf8(p);
            return true;
        }
        return false;
    }

    std::string firstExisting(const std::vector<fs::path>& candidates){
        for (const fs::path& p : candidates){
            if (fileExists(FileUtils::pathToUtf8(p))) return FileUtils::pathToUtf8(fs::absolute(p).lexically_normal());
        }
        return {};
    }

    void resolve(const Located& where, VatImportFiles& files){
        const fs::path& dir = where.folder;
        const std::string& n = where.name;
        files.remapInfo = firstExisting({dir / FileUtils::pathFromUtf8(n + REMAP_SUFFIX + ".json")});
        files.vat = firstExisting({dir / FileUtils::pathFromUtf8(n + "_vat.exr"), dir / FileUtils::pathFromUtf8(n + "_vat.png")});
        files.normals = firstExisting({dir / FileUtils::pathFromUtf8(n + "_vnrm.exr"), dir / FileUtils::pathFromUtf8(n + "_vnrm.png")});
        if (!where.givenMesh.empty()){
            files.mesh = where.givenMesh;
            return;
        }
        std::vector<fs::path> candidates;
        for (const char* ext : {".glb", ".gltf", ".fbx"}) candidates.push_back(dir / FileUtils::pathFromUtf8(n + ext));
        files.mesh = firstExisting(candidates);
        if (!files.mesh.empty()) return;
        // any single mesh in the folder
        std::error_code ec;
        std::vector<std::string> meshes;
        for (const auto& entry : fs::directory_iterator(dir, ec)){
            if (!entry.is_regular_file(ec)) continue;
            const std::string ext = lower(FileUtils::pathToUtf8(entry.path().extension()));
            if (ext == ".glb" || ext == ".gltf" || ext == ".fbx") meshes.push_back(FileUtils::pathToUtf8(fs::absolute(entry.path()).lexically_normal()));
        }
        std::sort(meshes.begin(), meshes.end());
        if (meshes.size() == 1) files.mesh = meshes.front();
    }

    struct Animation{
        std::string name;
        double start = 0, end = 0;
        double fps = 0;
        bool looping = true;
    };

    struct Remap{
        double min[3] = {0, 0, 0};
        double max[3] = {1, 1, 1};
        bool haveRemap = false;
        int frames = 0;
        std::vector<Animation> animations;
        bool listForm = false;
        std::string error;
    };

    bool numberOf(const Json& value, double& out){
        if (value.is_number()){ out = value.get<double>(); return true; }
        if (value.is_string()){
            char* end = nullptr;
            const std::string text = value.get<std::string>();
            out = std::strtod(text.c_str(), &end);
            return end && *end == '\0' && !text.empty();
        }
        return false;
    }

    const Json* member(const Json& object, std::initializer_list<const char*> names){
        if (!object.is_object()) return nullptr;
        for (const char* name : names){
            auto found = object.find(name);
            if (found != object.end()) return &*found;
        }
        return nullptr;
    }

    Animation animationFrom(const std::string& name, const Json& info){
        Animation a;
        a.name = name;
        double v = 0;
        if (const Json* s = member(info, {"startFrame", "start_frame", "frameStart", "start"})) if (numberOf(*s, v)) a.start = v;
        if (const Json* e = member(info, {"endFrame", "end_frame", "frameEnd", "end"})) if (numberOf(*e, v)) a.end = v;
        if (const Json* f = member(info, {"framerate", "frameRate", "fps"})) if (numberOf(*f, v) && v > 0) a.fps = v;
        if (const Json* l = member(info, {"looping", "loop"})){
            if (l->is_boolean()) a.looping = l->get<bool>();
            else if (numberOf(*l, v)) a.looping = v != 0;
        }
        return a;
    }

    bool parseRemap(const std::string& text, Remap& out){
        Json root = Json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object()){
            out.error = "not a JSON object";
            return false;
        }
        const Json* remap = member(root, {"os-remap", "os_remap"});
        if (remap && remap->is_object()){
            const Json* mn = member(*remap, {"Min", "min"});
            const Json* mx = member(*remap, {"Max", "max"});
            if (mn && mx && mn->is_array() && mx->is_array() && mn->size() == 3 && mx->size() == 3){
                bool ok = true;
                for (int i = 0; i < 3; i++) ok = ok && numberOf((*mn)[(size_t)i], out.min[i]) && numberOf((*mx)[(size_t)i], out.max[i]);
                out.haveRemap = ok;
            }
        }
        double frames = 0;
        const Json* framesValue = remap ? member(*remap, {"Frames", "frames"}) : nullptr;
        if (!framesValue) framesValue = member(root, {"Frames", "frames"});
        if (framesValue && numberOf(*framesValue, frames) && frames >= 1) out.frames = (int)std::lround(frames);
        const Json* animations = member(root, {"animations"});
        if (animations && animations->is_object()){
            for (auto it = animations->begin(); it != animations->end(); ++it){
                if (it.value().is_object()) out.animations.push_back(animationFrom(it.key(), it.value()));
            }
        }else if (animations && animations->is_array()){
            out.listForm = !animations->empty();
            int index = 0;
            for (const Json& item : *animations){
                index++;
                if (!item.is_object()) continue;
                std::string name = "Clip " + std::to_string(index);
                if (const Json* n = member(item, {"name", "Name"})) if (n->is_string() && !n->get<std::string>().empty()) name = n->get<std::string>();
                out.animations.push_back(animationFrom(name, item));
            }
        }
        if (!out.haveRemap){
            out.error = "no os-remap Min/Max";
            return false;
        }
        return true;
    }

    // ---- decoding ----

    // Blender Z-up to Y-up (x, z, -y)
    D3 blenderToYUp(const D3& v){ return D3(v.x, v.z, -v.y); }

    bool normalLooking(const FloatImage& image, int col, int row){
        const float* t = image.rgba.data() + ((size_t)row * (size_t)image.width + (size_t)col) * 4;
        const double x = t[0] * 2.0 - 1.0, y = t[1] * 2.0 - 1.0, z = t[2] * 2.0 - 1.0;
        return std::fabs(std::sqrt(x * x + y * y + z * z) - 1.0) < 0.08;
    }

}

void detectOpenVat(const std::string& path, VatImportDetection& out){
    Located where;
    if (!locate(path, where)) return;
    VatImportFiles files;
    resolve(where, files);
    if (files.remapInfo.empty()) return;
    out.kind = VatImportKind::OpenVat;
    out.assetName = where.name;
    out.folder = FileUtils::pathToUtf8(where.folder);
    out.files = files;
    out.metadataSource = "remap_info";
    std::string text;
    Remap remap;
    auto issue = [&](const std::string& kind, const std::string& message, const std::string& fix, bool error){
        VatImportIssue i;
        i.kind = kind;
        i.message = message;
        i.fix = fix;
        i.error = error;
        out.issues.push_back(i);
    };
    if (!readText(files.remapInfo, text) || !parseRemap(text, remap)){
        issue("read_failed", fileName(files.remapInfo) + " can't be read: " + (remap.error.empty() ? "unreadable" : remap.error),
              "re-export from OpenVAT", true);
    }
    out.frames = remap.frames;
    for (const Animation& a : remap.animations) out.clips.push_back(a.name);
    if (!remap.animations.empty()) out.fps = (float)remap.animations.front().fps;
    if (files.vat.empty()){
        issue("missing_file", "the VAT texture (" + where.name + "_vat.exr or .png) was not found", "export it with OpenVAT, or pass vat", true);
    }
    if (files.mesh.empty()){
        issue("missing_file", "the mesh (" + where.name + ".glb, .gltf or .fbx) was not found",
              "export the model with OpenVAT (Export Model), or pass mesh", true);
    }
    if (remap.animations.empty()){
        issue("fps_unknown", "the export has no animations: its frame rate is unknown", "pass fps (the Blender scene's frames per second)", true);
    }
    if (remap.listForm){
        issue("animations_list_form", "animations is a list (older OpenVAT exports): read as named clips in list order", "nothing to do", false);
    }
    out.summary = "OpenVAT export \"" + out.assetName + "\"" + (out.frames > 0 ? ", " + std::to_string(out.frames) + " frames" : std::string()) +
                  (out.clips.empty() ? std::string() : ", " + std::to_string(out.clips.size()) + " animation" + (out.clips.size() == 1 ? "" : "s")) +
                  (files.normals.empty() ? std::string() : ", separate normals");
}

void importOpenVat(const VatImportSpec& spec, const VatImportDetection& detection, VatImportResult& result){
    (void)detection;
    result.report.kind = VatImportKind::OpenVat;
    Located where;
    if (!locate(spec.path, where)){
        addError(result, "not_found", "no OpenVAT export at " + spec.path, "pass the export folder or its -remap_info.json");
        return;
    }
    VatImportFiles files;
    resolve(where, files);
    auto override = [](std::string& file, const std::string& given){
        if (!given.empty()) file = FileUtils::pathToUtf8(fs::absolute(FileUtils::pathFromUtf8(given)).lexically_normal());
    };
    override(files.mesh, spec.mesh);
    override(files.vat, spec.vat);
    override(files.normals, spec.normalsTexture);

    std::string text;
    Remap remap;
    if (files.remapInfo.empty() || !readText(files.remapInfo, text)){
        addError(result, "missing_file", "the remap info (" + where.name + "-remap_info.json) was not found", "export it with OpenVAT");
        return;
    }
    if (!parseRemap(text, remap)){
        addError(result, "read_failed", fileName(files.remapInfo) + " can't be read: " + remap.error, "re-export from OpenVAT");
        return;
    }
    result.filesRead.push_back(files.remapInfo);
    if (remap.listForm){
        addWarning(result, "animations_list_form", "animations is a list (older OpenVAT exports): read as named clips in list order",
                   "nothing to do");
    }
    if (files.vat.empty() || !fileExists(files.vat)){
        addError(result, "missing_file", "the VAT texture (" + where.name + "_vat.exr or .png) was not found", "export it with OpenVAT, or pass vat");
        return;
    }
    if (files.mesh.empty() || !fileExists(files.mesh)){
        addError(result, "missing_file", "the mesh (" + where.name + ".glb, .gltf or .fbx) was not found",
                 "export the model with OpenVAT (Export Model), or pass mesh");
        return;
    }
    SourceMesh src;
    std::string error;
    if (!readSourceMesh(files.mesh, src, error)){
        addError(result, error.find("no triangles") != std::string::npos ? "bad_mesh" : "read_failed", error, "re-export the model");
        return;
    }
    result.filesRead.push_back(files.mesh);
    result.report.sourcePoints = (int)(src.positions.size() / 3);
    FloatImage vat, vnrm;
    if (!readFloatImage(files.vat, vat, &error)){
        addError(result, "read_failed", "the VAT texture can't be read: " + error, "re-export it (EXR or PNG)");
        return;
    }
    result.filesRead.push_back(files.vat);
    const bool separateNormals = !files.normals.empty() && spec.normals;
    if (separateNormals){
        if (!readFloatImage(files.normals, vnrm, &error)){
            addError(result, "read_failed", "the normals texture can't be read: " + error, "re-export it (EXR or PNG)");
            return;
        }
        if (vnrm.width != vat.width || vnrm.height != vat.height){
            addError(result, "metadata_mismatch", fileName(files.normals) + " is " + std::to_string(vnrm.width) + " x " + std::to_string(vnrm.height) +
                     ", the VAT texture " + std::to_string(vat.width) + " x " + std::to_string(vat.height), "re-export them together");
            return;
        }
        result.filesRead.push_back(files.normals);
    }

    // the VAT_UV layer: the FBX set named so, else (glTF) the last TEXCOORD
    int vatUv = src.findUv("VAT_UV");
    if (vatUv < 0) vatUv = (int)src.uvs.size() - 1;
    if (vatUv < 0 || (src.gltf && src.uvs.size() < 1)){
        addError(result, "bad_mesh", fileName(files.mesh) + " has no VAT_UV layer", "export the model with OpenVAT (it adds VAT_UV)");
        return;
    }
    const int W = vat.width, H = vat.height;
    const int F = remap.frames;
    if (F < 1){
        addError(result, "metadata_mismatch", fileName(files.remapInfo) + " has no Frames (os-remap.Frames)", "re-export from OpenVAT");
        return;
    }
    std::vector<int> element(src.corners(), -1);
    std::map<std::pair<int, int>, int> texelElement;
    std::vector<std::pair<int, int>> elementTexel;
    std::vector<size_t> elementCorner;
    int maxRow = 0;
    for (size_t c = 0; c < src.corners(); c++){
        const double u = src.uvs[(size_t)vatUv][c * 2], v = src.uvs[(size_t)vatUv][c * 2 + 1];
        const double x = u * W;
        const double y = (src.gltf ? v : 1.0 - v) * H;
        const int col = (int)std::floor(x), row = (int)std::floor(y);
        if (std::fabs(x - col - 0.5) > 0.25 || std::fabs(y - row - 0.5) > 0.25 || col < 0 || row < 0 || col >= W || row + F > H){
            addError(result, "metadata_mismatch", "VAT_UV (" + num(u) + ", " + num(v) + ") addresses (" + num(x) + ", " + num(y) + ") of the " +
                     std::to_string(W) + " x " + std::to_string(H) + " texture: not a texel centre with " + std::to_string(F) + " frames below it",
                     "re-export the model and the texture together");
            return;
        }
        auto found = texelElement.find({col, row});
        if (found == texelElement.end()){
            found = texelElement.emplace(std::make_pair(col, row), (int)elementTexel.size()).first;
            elementTexel.push_back({col, row});
            elementCorner.push_back(c);
        }
        element[c] = found->second;
        maxRow = std::max(maxRow, row);
    }
    const int elements = (int)elementTexel.size();
    // packed normals: the lower half holds them (rows + H / 2)
    bool packed = false;
    if (spec.normals && !separateNormals && H % 2 == 0 && maxRow + F <= H / 2){
        int looksNormal = 0, checked = 0;
        for (int e = 0; e < elements && checked < 256; e += std::max(1, elements / 256)){
            checked++;
            if (normalLooking(vat, elementTexel[(size_t)e].first, elementTexel[(size_t)e].second + H / 2)) looksNormal++;
        }
        packed = checked > 0 && looksNormal * 10 >= checked * 9;
    }
    const bool normals = spec.normals && (separateNormals || packed);

    // clips: 1-based scene frames to rows
    std::vector<VatClip> clips;
    if (remap.animations.empty()){
        if (spec.fps <= 0.0f){
            addError(result, "fps_unknown", "the export has no animations: its frame rate is unknown", "pass fps (the Blender scene's frames per second)");
            return;
        }
        VatClip clip;
        clip.name = where.name;
        clip.start = 0;
        clip.frames = F;
        clip.fps = spec.fps;
        clips.push_back(clip);
    }else{
        double base = 1.0;
        for (const Animation& a : remap.animations) base = std::min(base, a.start);
        std::set<std::string> names;
        for (const Animation& a : remap.animations){
            if (clips.size() >= (size_t)VAT_MAX_CLIPS){
                addWarning(result, "clip_out_of_range", "more than " + std::to_string(VAT_MAX_CLIPS) + " animations: the rest are left out",
                           "split the export");
                break;
            }
            int start = (int)std::lround(a.start - base);
            int end = (int)std::lround(a.end - base);
            if (end < start) std::swap(start, end);
            if (start >= F){
                addWarning(result, "clip_out_of_range", "animation \"" + a.name + "\" starts at frame " + num(a.start) + ", past the texture's " +
                           std::to_string(F) + " frames: left out", "re-export with the animation inside the frame range");
                continue;
            }
            if (end >= F){
                addWarning(result, "clip_out_of_range", "animation \"" + a.name + "\" ends at frame " + num(a.end) + ", past the texture's " +
                           std::to_string(F) + " frames: clamped", "re-export with the animation inside the frame range");
                end = F - 1;
            }
            const double fps = spec.fps > 0.0f ? spec.fps : a.fps;
            if (!(fps > 0.0)){
                addError(result, "fps_unknown", "animation \"" + a.name + "\" has no framerate", "pass fps");
                return;
            }
            VatClip clip;
            std::string name = a.name.empty() ? "Clip" : a.name;
            std::string unique = name;
            for (int n = 2; names.count(unique); n++) unique = name + " " + std::to_string(n);
            names.insert(unique);
            clip.name = unique;
            clip.start = start;
            clip.frames = end - start + 1;
            clip.wrap = a.looping ? ClipWrapMode::Loop : ClipWrapMode::Once;
            // loop clips play frames / fps seconds; others (frames - 1) / fps (the last frame is the end)
            clip.fps = (float)fps;
            clips.push_back(clip);
        }
        if (clips.empty()){
            addError(result, "metadata_mismatch", "no animation of " + fileName(files.remapInfo) + " is inside the texture's " + std::to_string(F) + " frames",
                     "re-export from OpenVAT");
            return;
        }
    }

    PresetLimits limits;
    presetLimits(spec.preset, limits);
    Assembled in;
    in.payload.mode = VatMode::Vertices;
    in.payload.layout.width = limits.width;
    in.payload.layout.samples = F;
    in.payload.layout.vertices = elements;
    in.payload.layout.normals = normals;
    std::vector<std::string> fixes = {"use the desktop preset (preset: desktop)"};
    if (normals) fixes.push_back("leave out the normals (normals: false)");
    fixes.push_back("export fewer frames");
    if (!checkPayloadSize(VatMode::Vertices, F, elements, in.payload.layout.texelsPerSample(VatMode::Vertices), limits,
                          "the model's " + std::to_string(elements) + " vertices", fixes, result)){
        return;
    }
    if (!in.payload.allocate()){
        addError(result, "too_large", "not enough memory for the payload", "export fewer frames");
        return;
    }
    const double* mn = remap.min;
    const double* mx = remap.max;
    for (int e = 0; e < elements; e++){
        const int col = elementTexel[(size_t)e].first;
        const int row0 = elementTexel[(size_t)e].second;
        const size_t corner = elementCorner[(size_t)e];
        const D3 restTangent = cornerTangent(src, corner);
        for (int f = 0; f < F; f++){
            const float* t = vat.rgba.data() + ((size_t)(row0 + f) * (size_t)W + (size_t)col) * 4;
            D3 offset(mn[0] + t[0] * (mx[0] - mn[0]), mn[1] + t[1] * (mx[1] - mn[1]), mn[2] + t[2] * (mx[2] - mn[2]));
            if (src.gltf) offset = blenderToYUp(offset);
            in.payload.set(f, e, 0, offset.x, offset.y, offset.z, 0.0);
            if (normals){
                const FloatImage& image = separateNormals ? vnrm : vat;
                const int row = row0 + f + (separateNormals ? 0 : H / 2);
                const float* n = image.rgba.data() + ((size_t)row * (size_t)W + (size_t)col) * 4;
                D3 normal(n[0] * 2.0 - 1.0, n[1] * 2.0 - 1.0, n[2] * 2.0 - 1.0);
                if (src.gltf) normal = blenderToYUp(normal);
                const DQ frame = tangentFrame(normal, restTangent);
                in.payload.set(f, e, 1, frame.x, frame.y, frame.z, frame.w);
            }
        }
        in.elementRest.push_back(src.position(corner));
    }

    // material UVs: the first TEXCOORD that isn't VAT_UV
    int materialUv = -1;
    for (int u = 0; u < (int)src.uvs.size(); u++){
        if (u != vatUv){
            materialUv = u;
            break;
        }
    }
    BuiltMesh built;
    buildMesh(src, element, materialUv, true, true, built, false);
    in.mesh = std::move(built.mesh);
    in.xform = src.xform;
    in.clips = clips;
    in.modeReason = "OpenVAT export: per-vertex offsets" + std::string(normals ? (packed ? " and packed normals" : " and separate normals") : "") +
                    ", " + std::to_string(clips.size()) + " clip" + (clips.size() == 1 ? "" : "s");
    in.sourceFps = clips.front().fps;
    in.sourceFrames = F;
    if (src.embeddedTextures){
        addWarning(result, "embedded_textures_dropped", "textures embedded in " + fileName(files.mesh) + " are not copied to the .vat.glb",
                   "export the model with separate textures (glTF Separate), or assign them in the material");
    }
    finishImport(in, spec, result);
}

}
