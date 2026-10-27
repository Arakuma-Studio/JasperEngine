// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SideFX Labs VAT 3 (Houdini "Vertex Animation Textures" ROP 3.0) import, spec 4.6.1. The decoding
// mirrors SideFX's Unity Shader Graphs (VAT_SoftBodyDeformation / VAT_RigidBodyDynamics /
// VAT_DynamicRemeshing and their sub-graphs, SideFX Labs, BSD) node for node, run on the CPU in
// double precision, then re-packed into the engine's layout (spec 2.2):
//   flags in the bounds' second decimal (HDR, active ratios, position-alpha scale, LDR rotation
//   range / lookup depth), UV addressing of texel centres with rows from the top, LDR remaps,
//   Two Position Textures, smallest-three rigid quaternions, Houdini's precomputed-angle slerp,
//   smooth trajectories, compressed spheremap normals, the fluid lookup table.

#include "vat/import/VatImportInternal.h"

#include "util/FileUtils.h"
#include "util/FloatImage.h"

#include "json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::vat;
using jasperengine::editor::FileUtils;
using jasperengine::editor::FloatImage;
using Json = nlohmann::json;

namespace jasperengine::editor::vat::importer{

namespace{

    constexpr double PI = 3.14159265358979323846;
    // frame counts past this are refused (they would overflow the int frame math)
    constexpr double MAX_FRAME_COUNT = 1e7;

    // ---- the target engine's axes ----

    // target = F * W * houdini * scale: W a signed permutation (det +1 or -1), F a diagonal of
    // signs (the exporter's "Position Invert"). Pivots, compressed normals and accelerations are
    // written in W space (the Unity graphs mirror them themselves), positions and rotations in
    // the full target space.
    struct Axis{
        std::string name;
        double W[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        double F[3] = {1, 1, 1};
        double scale = 1.0;

        // W^T v
        D3 fromW(const D3& v) const{
            return D3(W[0][0] * v.x + W[1][0] * v.y + W[2][0] * v.z,
                      W[0][1] * v.x + W[1][1] * v.y + W[2][1] * v.z,
                      W[0][2] * v.x + W[1][2] * v.y + W[2][2] * v.z);
        }
        D3 toW(const D3& v) const{
            return D3(W[0][0] * v.x + W[0][1] * v.y + W[0][2] * v.z,
                      W[1][0] * v.x + W[1][1] * v.y + W[1][2] * v.z,
                      W[2][0] * v.x + W[2][1] * v.y + W[2][2] * v.z);
        }
        // a target-space position or offset in Houdini's space
        D3 point(const D3& t) const { return fromW(D3(F[0] * t.x, F[1] * t.y, F[2] * t.z)) * (1.0 / scale); }
        D3 toTarget(const D3& h) const{
            const D3 w = toW(h * scale);
            return D3(F[0] * w.x, F[1] * w.y, F[2] * w.z);
        }
        double det() const{
            const double dw = W[0][0] * (W[1][1] * W[2][2] - W[1][2] * W[2][1]) - W[0][1] * (W[1][0] * W[2][2] - W[1][2] * W[2][0]) +
                              W[0][2] * (W[1][0] * W[2][1] - W[1][1] * W[2][0]);
            return dw * F[0] * F[1] * F[2];
        }
        // a target-space rotation in Houdini's space: the axis is a pseudovector
        DQ quat(const DQ& q) const{
            const D3 axis = point(D3(q.x, q.y, q.z)) * (scale * det());
            return DQ(axis.x, axis.y, axis.z, q.w);
        }
    };

    bool axisFromName(const std::string& rawName, double scale, Axis& out){
        const std::string name = lower(rawName);
        Axis axis;
        if (name == "unity"){
            axis.F[0] = -1;
        }else if (name == "unreal"){
            // "X Z Y": target = (x, z, y)
            axis.W[1][1] = 0; axis.W[1][2] = 1;
            axis.W[2][2] = 0; axis.W[2][1] = 1;
            axis.scale = 100.0;
        }else if (name == "houdini"){
            // right-handed Y-up: as Houdini
        }else if (name == "zup"){
            // right-handed Z-up: target = (x, -z, y)
            axis.W[1][1] = 0; axis.W[1][2] = -1;
            axis.W[2][2] = 0; axis.W[2][1] = 1;
        }else{
            return false;
        }
        axis.name = name;
        if (scale > 0.0) axis.scale = scale;
        out = axis;
        return true;
    }

    // the JSON's "Axis System": a coordsys menu index or its label
    std::string axisNameFromJson(const Json& value){
        if (value.is_number()){
            switch (value.get<int>()){
                case 0: return "houdini";
                case 1: return "zup";
                case 2: return "unity";
                case 4: return "unreal";
                default: return {};
            }
        }
        if (!value.is_string()) return {};
        const std::string text = lower(value.get<std::string>());
        if (text.find("unity") != std::string::npos) return "unity";
        if (text.find("unreal") != std::string::npos) return "unreal";
        const bool left = text.find("left") != std::string::npos;
        const bool right = text.find("right") != std::string::npos;
        const bool zUp = text.find("z-up") != std::string::npos || text.find("z up") != std::string::npos;
        const bool yUp = text.find("y-up") != std::string::npos || text.find("y up") != std::string::npos;
        if (left && yUp) return "unity";
        if (left && zUp) return "unreal";
        if (right && yUp) return "houdini";
        if (right && zUp) return "zup";
        if (text == "0" || text == "1" || text == "2" || text == "4") return axisNameFromJson(Json(std::atoi(text.c_str())));
        return {};
    }

    // ---- metadata ----

    struct Meta{
        std::string source;             // "material" | "data json" | "mesh bounds"
        HoudiniMode mode = HoudiniMode::Unknown;
        std::string modeSource;
        bool haveBounds = false;
        double bMin[3] = {0, 0, 0};
        double bMax[3] = {0, 0, 0};
        float fps = 0.0f;
        int frames = 0;
        // material flags (-1 unknown, 0 off, 1 on)
        int compressedNormals = -1;
        int twoPositions = -1;
        int loadColor = -1;
        int smoothTrajectories = -1;
        int stretchByVelocity = -1;
        int pscaleInAlpha = -1;
        double pscaleMultiplier = 1.0;
        int twoSided = -1;
        int uvFromRG = -1;
        std::string target;             // axis name
        double targetScale = 0.0;
        std::string error;              // a metadata file that doesn't parse
    };

    // shader GUIDs of SideFX Labs' VAT 3.0 Unity shader graphs (as referenced by the ROP's .mat templates)
    HoudiniMode modeFromShaderGuid(const std::string& guid){
        if (guid == "987a09a96b5f17d46a88a7c429d130d2") return HoudiniMode::Soft;
        if (guid == "085ef3ec3a5a70d4790c0957d30c9f63") return HoudiniMode::Rigid;
        if (guid == "19b419010e1004644affea18d0fc8dff") return HoudiniMode::Fluid;
        if (guid == "8bf13a6837d559049924774e12033384") return HoudiniMode::Sprite;
        return HoudiniMode::Unknown;
    }

    std::string trim(const std::string& text){
        size_t a = 0, b = text.size();
        while (a < b && std::isspace((unsigned char)text[a])) a++;
        while (b > a && std::isspace((unsigned char)text[b - 1])) b--;
        return text.substr(a, b - a);
    }

    bool parseNumber(const std::string& text, double& out){
        const std::string t = trim(text);
        if (t.empty()) return false;
        char* end = nullptr;
        out = std::strtod(t.c_str(), &end);
        return end && *end == '\0' && std::isfinite(out);
    }

    // Unity material YAML: the shader GUID, m_ShaderKeywords and m_Floats ("- _name: value")
    bool parseMaterial(const std::string& text, Meta& meta){
        std::map<std::string, double> floats;
        std::set<std::string> keywords;
        std::set<std::string> textures;
        std::string shaderGuid;
        size_t shader = text.find("m_Shader:");
        if (shader != std::string::npos){
            const size_t guid = text.find("guid:", shader);
            const size_t close = text.find('}', shader);
            if (guid != std::string::npos && (close == std::string::npos || guid < close)){
                size_t i = guid + 5;
                while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
                while (i < text.size() && std::isxdigit((unsigned char)text[i])) shaderGuid += (char)std::tolower((unsigned char)text[i++]);
            }
        }
        size_t pos = 0;
        bool inTexEnvs = false;
        std::string pendingTexture;
        while (pos <= text.size()){
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const std::string line = trim(text.substr(pos, end - pos));
            pos = end + 1;
            if (line.rfind("m_ShaderKeywords:", 0) == 0){
                std::string list = trim(line.substr(17));
                size_t k = 0;
                while (k < list.size()){
                    size_t space = list.find(' ', k);
                    if (space == std::string::npos) space = list.size();
                    const std::string keyword = list.substr(k, space - k);
                    if (!keyword.empty()) keywords.insert(keyword);
                    k = space + 1;
                }
                continue;
            }
            if (line.rfind("m_TexEnvs:", 0) == 0){ inTexEnvs = true; continue; }
            if (line.rfind("m_Ints:", 0) == 0 || line.rfind("m_Floats:", 0) == 0 || line.rfind("m_Colors:", 0) == 0) inTexEnvs = false;
            if (line.rfind("- ", 0) == 0){
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                const std::string name = trim(line.substr(2, colon - 2));
                const std::string value = trim(line.substr(colon + 1));
                double number = 0;
                if (!value.empty() && parseNumber(value, number)) floats[name] = number;
                if (inTexEnvs && value.empty()) pendingTexture = name;
                continue;
            }
            if (inTexEnvs && !pendingTexture.empty() && line.rfind("m_Texture:", 0) == 0){
                if (line.find("fileID: 0") == std::string::npos) textures.insert(pendingTexture);
                pendingTexture.clear();
            }
        }
        const bool any = !shaderGuid.empty() || !floats.empty();
        if (!any) return false;

        auto flag = [&](const std::string& name) -> int {
            if (keywords.count(name)) return 1;
            auto found = floats.find(name);
            if (found == floats.end()) return -1;
            return found->second != 0.0 ? 1 : 0;
        };
        auto number = [&](const std::string& name, double& out) -> bool {
            auto found = floats.find(name);
            if (found == floats.end()) return false;
            out = found->second;
            return true;
        };
        meta.source = "material";
        meta.mode = modeFromShaderGuid(shaderGuid);
        if (meta.mode != HoudiniMode::Unknown){
            meta.modeSource = "material shader";
        }else{
            if (floats.count("_B_uvFromRG") || textures.count("_lookupTable")){
                meta.mode = HoudiniMode::Fluid;
            }else if (floats.count("_B_SMOOTH_TRAJECTORIES") || floats.count("_B_stretchByVel") || floats.count("_B_animateFirstFrame")){
                meta.mode = HoudiniMode::Rigid;
            }else if (floats.count("_B_CAN_SPIN") || floats.count("_B_hideOverlappingOrigin") || floats.count("_B_spinFromHeading")){
                meta.mode = HoudiniMode::Sprite;
            }else if (floats.count("_B_UNLOAD_ROT_TEX")){
                meta.mode = HoudiniMode::Soft;
            }
            if (meta.mode != HoudiniMode::Unknown) meta.modeSource = "material keywords";
        }
        const char* bounds[6] = {"_boundMinX", "_boundMinY", "_boundMinZ", "_boundMaxX", "_boundMaxY", "_boundMaxZ"};
        int found = 0;
        for (int i = 0; i < 6; i++){
            double v = 0;
            if (number(bounds[i], v)){
                (i < 3 ? meta.bMin[i] : meta.bMax[i - 3]) = v;
                found++;
            }
        }
        // the ROP's template writes zeros until a render fills them
        bool allZero = true;
        for (int i = 0; i < 3; i++) allZero = allZero && meta.bMin[i] == 0.0 && meta.bMax[i] == 0.0;
        meta.haveBounds = found == 6 && !allZero;
        double v = 0;
        if (number("_houdiniFPS", v) && v > 0) meta.fps = (float)v;
        if (number("_frameCount", v) && v >= 1 && v <= MAX_FRAME_COUNT) meta.frames = (int)std::lround(v);
        meta.compressedNormals = flag("_B_UNLOAD_ROT_TEX");
        meta.twoPositions = flag("_B_LOAD_POS_TWO_TEX");
        meta.loadColor = flag("_B_LOAD_COL_TEX");
        meta.smoothTrajectories = flag("_B_SMOOTH_TRAJECTORIES");
        meta.stretchByVelocity = flag("_B_stretchByVel");
        meta.pscaleInAlpha = flag("_B_pscaleAreInPosA");
        if (number("_globalPscaleMul", v)) meta.pscaleMultiplier = v;
        meta.twoSided = flag("_B_twoSidedNorms");
        meta.uvFromRG = flag("_B_uvFromRG");
        // the .mat is the Unity target's
        meta.target = "unity";
        return true;
    }

    std::string normalizedKey(const std::string& key){
        std::string out;
        for (char c : key) if (std::isalnum((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
        return out;
    }

    // every (normalized key -> value) of a JSON tree, first occurrence wins
    void collectKeys(const Json& node, std::map<std::string, Json>& out){
        if (node.is_object()){
            for (auto it = node.begin(); it != node.end(); ++it){
                const std::string key = normalizedKey(it.key());
                if (!out.count(key)) out[key] = it.value();
                collectKeys(it.value(), out);
            }
        }else if (node.is_array()){
            for (const Json& item : node) collectKeys(item, out);
        }
    }

    bool jsonNumber(const Json& value, double& out){
        if (value.is_number()){ out = value.get<double>(); return true; }
        if (value.is_boolean()){ out = value.get<bool>() ? 1.0 : 0.0; return true; }
        if (value.is_string()){
            const std::string t = lower(trim(value.get<std::string>()));
            if (t == "true" || t == "on" || t == "yes") { out = 1; return true; }
            if (t == "false" || t == "off" || t == "no") { out = 0; return true; }
            return parseNumber(t, out);
        }
        return false;
    }

    // the legacy real-time data JSON: "Bound Min X" ..., "Houdini FPS", "Two Position Textures",
    // "Axis System" (keys matched case- and punctuation-insensitively, anywhere in the tree)
    bool parseDataJson(const std::string& text, Meta& meta){
        Json root = Json::parse(text, nullptr, false);
        if (root.is_discarded()){
            meta.error = "not valid JSON";
            return false;
        }
        std::map<std::string, Json> keys;
        collectKeys(root, keys);
        auto get = [&](std::initializer_list<const char*> names, double& out) -> bool {
            for (const char* name : names){
                auto found = keys.find(name);
                if (found != keys.end() && jsonNumber(found->second, out)) return true;
            }
            return false;
        };
        const char* bounds[6] = {"boundminx", "boundminy", "boundminz", "boundmaxx", "boundmaxy", "boundmaxz"};
        int found = 0;
        for (int i = 0; i < 6; i++){
            double v = 0;
            if (get({bounds[i]}, v)){
                (i < 3 ? meta.bMin[i] : meta.bMax[i - 3]) = v;
                found++;
            }
        }
        meta.haveBounds = found == 6;
        if (!meta.haveBounds) return false;
        meta.source = "data json";
        double v = 0;
        if (get({"houdinifps", "fps"}, v) && v > 0) meta.fps = (float)v;
        if (get({"framecount", "numberofframes", "frames"}, v) && v >= 1 && v <= MAX_FRAME_COUNT) meta.frames = (int)std::lround(v);
        if (get({"twopositiontextures", "positionsrequiretwotextures"}, v)) meta.twoPositions = v != 0 ? 1 : 0;
        if (get({"usecompressednormals", "compressednormals", "packednormals"}, v)) meta.compressedNormals = v != 0 ? 1 : 0;
        if (get({"smoothlyinterpolatedtrajectories", "smoothtrajectories"}, v)) meta.smoothTrajectories = v != 0 ? 1 : 0;
        auto axis = keys.find("axissystem");
        if (axis != keys.end()) meta.target = axisNameFromJson(axis->second);
        if (get({"scale", "1metreinengineunits", "enginescale"}, v) && v > 0) meta.targetScale = v;
        auto mode = keys.find("mode");
        if (mode != keys.end()){
            std::string m;
            if (mode->second.is_string()) m = lower(mode->second.get<std::string>());
            double n = -1;
            if (mode->second.is_number()) n = mode->second.get<double>();
            if (m.find("soft") != std::string::npos || n == 0) meta.mode = HoudiniMode::Soft;
            else if (m.find("rigid") != std::string::npos || n == 1) meta.mode = HoudiniMode::Rigid;
            else if (m.find("fluid") != std::string::npos || m.find("remesh") != std::string::npos || n == 2) meta.mode = HoudiniMode::Fluid;
            else if (m.find("sprite") != std::string::npos || m.find("particle") != std::string::npos || n == 3) meta.mode = HoudiniMode::Sprite;
            if (meta.mode != HoudiniMode::Unknown) meta.modeSource = "data json";
        }
        return true;
    }

    // ---- the flags in the bounds (spec 4.6.1) ----

    // x * 10 in double, snapped to the integer when within 1e-3. The float32 trap: Unity and FBX
    // keep the bounds as float32, so 8.1 arrives as 8.10000038 and -bMax.x * 10 = -81.0000038,
    // whose fractional part reads 0.9999962 instead of 0 (an active height ratio of ~0, not 1);
    // float32 arithmetic (as in the shader) can't tell the two apart, double with snapping can.
    double tenths(double value){
        const double x = value * 10.0;
        const double r = std::round(x);
        return std::fabs(x - r) < 1e-3 ? r : x;
    }

    struct Flags{
        bool hdr = true;
        double activeU = 1.0;           // Ur
        double activeV = 1.0;           // Vr
        double alphaScale = 1.0;        // position alpha / (1 - fract(bMax.y*10))
        double minXDigit = 0.0;         // fract(-bMin.x*10): rigid LDR rotation range, fluid lookup depth
    };

    Flags decodeFlags(const double bMin[3], const double bMax[3]){
        Flags f;
        f.hdr = fract(tenths(bMax[2])) >= 0.5;
        const double minZ = tenths(bMin[2]);
        f.activeU = 1.0 - (std::ceil(minZ) - minZ);
        f.activeV = 1.0 - fract(-tenths(bMax[0]));
        const double alpha = 1.0 - fract(tenths(bMax[1]));
        f.alphaScale = alpha > 1e-9 ? 1.0 / alpha : 1.0;
        f.minXDigit = fract(-tenths(bMin[0]));
        return f;
    }

    // ---- files ----

    const char* const TEXTURE_EXTENSIONS[] = {".exr", ".png", ".tga", ".tif", ".tiff"};
    const char* const MESH_EXTENSIONS[] = {".fbx", ".glb", ".gltf"};

    bool hasSuffix(const std::string& text, const std::string& suffix){
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string firstExisting(const std::vector<fs::path>& candidates){
        for (const fs::path& p : candidates){
            const std::string s = FileUtils::pathToUtf8(p);
            if (fileExists(s)) return FileUtils::pathToUtf8(fs::absolute(p).lexically_normal());
        }
        return {};
    }

    std::string findTexture(const fs::path& root, const std::string& asset, const std::string& kind){
        std::vector<fs::path> candidates;
        for (const char* ext : TEXTURE_EXTENSIONS){
            candidates.push_back(root / "tex" / FileUtils::pathFromUtf8(asset + "_" + kind + ext));
            candidates.push_back(root / FileUtils::pathFromUtf8(asset + "_" + kind + ext));
        }
        return firstExisting(candidates);
    }

    // the folder names the ROP groups files into
    bool isExportSubfolder(const std::string& name){
        const std::string n = lower(name);
        return n == "geo" || n == "tex" || n == "unity" || n == "data";
    }

    // "<asset>_<suffix>" from a file stem, "" when the stem doesn't end so
    std::string stripSuffix(const std::string& stem, const std::string& suffix){
        if (stem.size() > suffix.size() && hasSuffix(stem, suffix)) return stem.substr(0, stem.size() - suffix.size());
        return {};
    }

    // the first asset name found in an export folder
    std::string assetInFolder(const fs::path& root){
        std::error_code ec;
        const std::pair<const char*, std::string> places[] = {{"geo", "_mesh"}, {"", "_mesh"}, {"tex", "_pos"}, {"", "_pos"},
                                                              {"unity", "_mat"}, {"data", "_data"}};
        for (const auto& place : places){
            const fs::path dir = place.first[0] ? root / place.first : root;
            if (!fs::is_directory(dir, ec)) continue;
            std::vector<std::string> names;
            for (const auto& entry : fs::directory_iterator(dir, ec)){
                if (!entry.is_regular_file(ec)) continue;
                const std::string stem = FileUtils::pathToUtf8(entry.path().stem());
                const std::string asset = stripSuffix(stem, place.second);
                if (!asset.empty()) names.push_back(asset);
            }
            std::sort(names.begin(), names.end());
            if (!names.empty()) return names.front();
        }
        return {};
    }

    struct Located{
        std::string asset;
        fs::path root;
        std::string givenMesh;      // the path itself when it is a mesh
    };

    bool locate(const std::string& path, Located& out){
        std::error_code ec;
        fs::path p = fs::absolute(FileUtils::pathFromUtf8(path), ec).lexically_normal();
        if (FileUtils::pathToUtf8(p.filename()).empty()) p = p.parent_path();
        if (fs::is_directory(p, ec)){
            fs::path root = p;
            if (isExportSubfolder(FileUtils::pathToUtf8(p.filename()))) root = p.parent_path();
            out.root = root;
            out.asset = assetInFolder(root);
            return !out.asset.empty();
        }
        if (!fs::is_regular_file(p, ec)) return false;
        const std::string ext = lower(FileUtils::pathToUtf8(p.extension()));
        const std::string stem = FileUtils::pathToUtf8(p.stem());
        fs::path dir = p.parent_path();
        if (isExportSubfolder(FileUtils::pathToUtf8(dir.filename()))) dir = dir.parent_path();
        out.root = dir;
        if (ext == ".mat"){
            out.asset = stripSuffix(stem, "_mat");
            return !out.asset.empty();
        }
        if (ext == ".json"){
            out.asset = stripSuffix(stem, "_data");
            return !out.asset.empty();
        }
        for (const char* m : MESH_EXTENSIONS){
            if (ext == m){
                const size_t at = stem.rfind("_mesh");
                out.asset = at != std::string::npos && at > 0 ? stem.substr(0, at) : stem;
                out.givenMesh = FileUtils::pathToUtf8(p);
                return true;
            }
        }
        for (const char* t : TEXTURE_EXTENSIONS){
            if (ext == t){
                for (const char* kind : {"_pos2", "_pos", "_rot", "_col2", "_col", "_lookup"}){
                    const std::string asset = stripSuffix(stem, kind);
                    if (!asset.empty()){
                        out.asset = asset;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // ---- textures ----

    // (x, y) is a finite point inside a width x height texture (so its floor is a valid texel)
    bool texelInside(double x, double y, int width, int height){
        return std::isfinite(x) && std::isfinite(y) && x >= 0.0 && y >= 0.0 && x < (double)width && y < (double)height;
    }

    struct Texture{
        std::string path;
        FloatImage image;
        bool loaded = false;

        const float* texel(int col, int row) const{
            return image.rgba.data() + ((size_t)row * (size_t)image.width + (size_t)col) * 4;
        }
    };

    bool loadTexture(const std::string& path, const char* what, Texture& out, VatImportResult& result){
        out.path = path;
        std::string error;
        if (!readFloatImage(path, out.image, &error)){
            addError(result, "read_failed", std::string("the ") + what + " texture can't be read: " + error,
                     "re-export it (EXR, PNG or TGA)");
            return false;
        }
        out.loaded = true;
        result.filesRead.push_back(path);
        return true;
    }

    // ---- shared decoding state ----

    struct Decoder{
        Meta meta;
        Flags flags;
        Axis axis;
        Texture pos, pos2, rot, col, lookup;
        bool twoPositions = false;
        bool useColor = false;
        int frames = 0;                 // N
        int width = 0, height = 0;      // the data textures' size
        int activeRows = 0;             // Vr * H
        int activeCols = 0;             // Ur * W
        int rowsPerFrame = 0;

        // a texel's target-space position (two textures, LDR remap)
        D3 position(int c, int r) const{
            const float* t = pos.texel(c, r);
            D3 p(t[0], t[1], t[2]);
            if (twoPositions){
                const float* t2 = pos2.texel(c, r);
                p = p + D3(t2[0], t2[1], t2[2]) * 0.01;
            }
            if (!flags.hdr){
                p = D3(p.x * (meta.bMax[0] - meta.bMin[0]) + meta.bMin[0], p.y * (meta.bMax[1] - meta.bMin[1]) + meta.bMin[1],
                       p.z * (meta.bMax[2] - meta.bMin[2]) + meta.bMin[2]);
            }
            return p;
        }
        // a rotation texel (HDR raw, LDR (t - 0.5) * 2)
        void rotation(int c, int r, double out[4]) const{
            const float* t = rot.texel(c, r);
            for (int i = 0; i < 4; i++) out[i] = flags.hdr ? t[i] : (t[i] - 0.5) * 2.0;
        }
        // the soft-body / fluid normal frame from the rotation texture: N = rotate(q, (0,1,0)),
        // T = rotate(q, (-1,0,0)) in Unity's mirrored axes = rotate(q_h, (1,0,0)) in Houdini's
        DQ rotationFrame(int c, int r) const{
            double q[4];
            rotation(c, r, q);
            // as the graph: rotate with the stored (LDR: not quite unit) quaternion, then normalise
            const DQ qh = axis.quat(DQ(q[0], q[1], q[2], q[3]));
            return tangentFrame(normalize(rotate(qh, D3(0, 1, 0))), normalize(rotate(qh, D3(1, 0, 0)), D3(1, 0, 0)));
        }
        // the compressed (10-bit spheremap) normal in position alpha, in Houdini's axes
        D3 compressedNormal(int c, int r) const{
            const double a = pos.texel(c, r)[3];
            const double x = a * 1024.0;
            const double hi = std::floor(x / 32.0);
            const double ex = hi / 31.5 * 4.0 - 2.0;
            const double ey = (x - hi * 32.0) / 31.5 * 4.0 - 2.0;
            const double e2 = ex * ex + ey * ey;
            const double g = std::sqrt(std::max(0.0, 1.0 - e2 / 4.0));
            D3 n(ex * g, 1.0 - e2 / 2.0, ey * g);
            n = D3(std::clamp(n.x, -1.0, 1.0), std::clamp(n.y, -1.0, 1.0), std::clamp(n.z, -1.0, 1.0));
            return normalize(axis.fromW(n));
        }
    };

    // the VAT texel a UV addresses at frame 0: column and row (from the top) of texel centres
    bool addressTexel(double u, double v, int texWidth, int texHeight, double activeU, double activeV,
                      int& col, int& row, std::string& problem){
        const double x = u * activeU * texWidth;
        const double y = (1.0 - v) * activeV * texHeight;
        const bool inside = texelInside(x, y, texWidth, texHeight);
        col = inside ? (int)std::floor(x) : -1;
        row = inside ? (int)std::floor(y) : -1;
        if (!inside || std::fabs(x - col - 0.5) > 0.25 || std::fabs(y - row - 0.5) > 0.25){
            problem = "UV (" + num(u) + ", " + num(v) + ") addresses (" + num(x) + ", " + num(y) + ") of the " + std::to_string(texWidth) +
                      " x " + std::to_string(texHeight) + " texture, not a texel centre (active ratios " + num(activeU) + ", " +
                      num(activeV) + " from the bounds)";
            return false;
        }
        return true;
    }

    VatClip makeClip(const std::string& name, int frames, double fps, bool interpolate, bool restNone){
        VatClip clip;
        clip.name = name;
        clip.start = 0;
        clip.frames = frames;
        clip.fps = (float)fps;
        clip.wrap = ClipWrapMode::Loop;
        clip.interpolate = interpolate;
        clip.restNone = restNone;
        return clip;
    }

    // ---- per mode ----

    // Soft body: per point offsets from the FBX rest position, the rotation-texture frame (or the
    // compressed normal with the mesh tangent), colours
    void importSoft(const VatImportSpec& spec, Decoder& d, const SourceMesh& src, int vatUv, const std::string& clipName,
                    const PresetLimits& limits, VatImportResult& result){
        // elements: the distinct frame-0 texels of the kept corners
        std::vector<int> element(src.corners(), -1);
        std::map<std::pair<int, int>, int> texelElement;
        std::vector<std::pair<int, int>> elementTexel;
        std::vector<size_t> elementCorner;
        for (size_t t = 0; t < src.triangles(); t++){
            bool guard = false;
            for (int c = 0; c < 3; c++) guard = guard || src.uvs[(size_t)vatUv][(t * 3 + (size_t)c) * 2 + 1] <= 0.1;
            if (guard) continue;
            for (int c = 0; c < 3; c++){
                const size_t corner = t * 3 + (size_t)c;
                int col = 0, row = 0;
                std::string problem;
                if (!addressTexel(src.uvs[(size_t)vatUv][corner * 2], src.uvs[(size_t)vatUv][corner * 2 + 1], d.width, d.activeRows,
                                  d.flags.activeU, 1.0, col, row, problem)){
                    addError(result, "metadata_mismatch", problem, "re-export the textures and the mesh together");
                    return;
                }
                if (row >= d.rowsPerFrame || col >= d.activeCols){
                    addError(result, "metadata_mismatch",
                             "a vertex addresses row " + std::to_string(row) + ", column " + std::to_string(col) + " of frame 1, which has " +
                             std::to_string(d.rowsPerFrame) + " rows of " + std::to_string(d.activeCols) + " texels",
                             "re-export the textures and the mesh together");
                    return;
                }
                auto found = texelElement.find({col, row});
                if (found == texelElement.end()){
                    found = texelElement.emplace(std::make_pair(col, row), (int)elementTexel.size()).first;
                    elementTexel.push_back({col, row});
                    elementCorner.push_back(corner);
                }
                element[corner] = found->second;
            }
        }
        const int elements = (int)elementTexel.size();
        if (elements == 0){
            addError(result, "bad_mesh", "every triangle of the mesh is a guard triangle (VAT UV V <= 0.1)", "re-export the mesh");
            return;
        }
        const bool compressed = d.meta.compressedNormals == 1 || (d.meta.compressedNormals < 0 && !d.rot.loaded);
        const bool normals = spec.normals && (d.rot.loaded || compressed);
        const bool colors = spec.colors && d.useColor;
        Assembled in;
        in.payload.mode = VatMode::Vertices;
        in.payload.layout.width = limits.width;
        in.payload.layout.samples = d.frames;
        in.payload.layout.vertices = elements;
        in.payload.layout.normals = normals;
        in.payload.layout.colors = colors;
        std::vector<std::string> fixes = {"use the desktop preset (preset: desktop)"};
        if (colors) fixes.push_back("leave out the colours (colors: false)");
        if (normals) fixes.push_back("leave out the normals (normals: false)");
        fixes.push_back("export fewer frames");
        if (!checkPayloadSize(VatMode::Vertices, d.frames, elements, in.payload.layout.texelsPerSample(VatMode::Vertices), limits,
                              "the soft body's " + std::to_string(elements) + " points", fixes, result)){
            return;
        }
        if (!in.payload.allocate()){
            addError(result, "too_large", "not enough memory for the payload", "export fewer frames");
            return;
        }
        for (int e = 0; e < elements; e++){
            const int col = elementTexel[(size_t)e].first;
            const int row0 = elementTexel[(size_t)e].second;
            const size_t corner = elementCorner[(size_t)e];
            const D3 restTangent = cornerTangent(src, corner);
            for (int f = 0; f < d.frames; f++){
                const int row = row0 + f * d.rowsPerFrame;
                const D3 offset = d.axis.point(d.position(col, row));
                in.payload.set(f, e, 0, offset.x, offset.y, offset.z, 0.0);
                int k = 1;
                if (normals){
                    DQ frame;
                    if (compressed){
                        frame = tangentFrame(d.compressedNormal(col, row), restTangent);
                    }else{
                        frame = d.rotationFrame(col, row);
                    }
                    in.payload.set(f, e, k++, frame.x, frame.y, frame.z, frame.w);
                }
                if (colors){
                    const float* c = d.col.texel(col, row);
                    in.payload.set(f, e, k, c[0], c[1], c[2], c[3]);
                }
            }
            in.elementRest.push_back(src.position(corner));
        }
        BuiltMesh built;
        buildMesh(src, element, 0, true, true, built, d.meta.twoSided == 1);
        in.mesh = std::move(built.mesh);
        in.xform = src.xform;
        in.clips.push_back(makeClip(clipName, d.frames, d.meta.fps, true, false));
        in.modeReason = "Houdini soft-body export: per-point offsets" + std::string(normals ? (compressed ? " and compressed normals" : " and rotation-texture normals") : "") +
                        (colors ? " and colours" : "");
        in.sourceFps = d.meta.fps;
        in.sourceFrames = d.frames;
        finishImport(in, spec, result);
    }

    // one rigid piece's motion as Houdini's shader evaluates it at frame f + t
    struct PieceTrack{
        D3 pivot;                       // Houdini space
        double radius = 0.0;            // furthest vertex from the pivot
        std::vector<D3> position;       // per source frame (Houdini space)
        std::vector<DQ> rotation;       // per source frame (Houdini space, decoded)
        std::vector<double> angle;      // rotation alpha (turns, signed; LDR range applied)
        std::vector<double> scale;
        std::vector<D3> acceleration;   // smooth trajectories (Houdini space, units/s^2)
        std::vector<std::array<float, 4>> color;
    };

    void evaluatePiece(const PieceTrack& p, int f, double t, int frames, double fps, bool smooth, D3& position, DQ& rotation, double& scale){
        const int next = (f + 1) % frames;
        // rotation: Houdini's slerp with the precomputed angle (n77..n126 of the rigid sub-graph)
        const DQ q0 = p.rotation[(size_t)f];
        const double a = p.angle[(size_t)f];
        if (a == 0.0 || t == 0.0){
            rotation = q0;
        }else{
            const DQ q1 = a < 0.0 ? qneg(p.rotation[(size_t)next]) : p.rotation[(size_t)next];
            const double theta = fract(std::fabs(a)) * PI;
            const double phi = fract(std::fabs(a) * t) * PI;
            const double s = std::sin(theta);
            DQ q;
            if (std::fabs(s) < 1e-9){
                q = DQ(q0.x + (q1.x - q0.x) * t, q0.y + (q1.y - q0.y) * t, q0.z + (q1.z - q0.z) * t, q0.w + (q1.w - q0.w) * t);
            }else{
                const double w0 = std::sin(theta - phi) / s, w1 = std::sin(phi) / s;
                q = DQ(q0.x * w0 + q1.x * w1, q0.y * w0 + q1.y * w1, q0.z * w0 + q1.z * w1, q0.w * w0 + q1.w * w1);
            }
            rotation = qnormalize(q);
        }
        // position: lerp, or P + V T + A T^2 / 2 with V the central difference (not on frame 1)
        const D3 p0 = p.position[(size_t)f];
        const D3 p1 = p.position[(size_t)next];
        if (smooth && f != 0){
            const int prev = (f - 1 + frames) % frames;
            const D3 v = (p1 - p.position[(size_t)prev]) * (0.5 * fps);
            const double T = t / fps;
            position = p0 + v * T + p.acceleration[(size_t)f] * (0.5 * T * T);
        }else{
            position = lerp(p0, p1, t);
        }
        scale = p.scale[(size_t)f] + (p.scale[(size_t)next] - p.scale[(size_t)f]) * t;
    }

    void importRigid(const VatImportSpec& spec, Decoder& d, const SourceMesh& src, const std::string& clipName,
                     const PresetLimits& limits, VatImportResult& result){
        const int vatUv = 1;
        if (src.uvs.size() < 4){
            addError(result, "bad_mesh", "the rigid-body mesh has " + std::to_string(src.uvs.size()) +
                     " UV sets, it needs 4 (UV, the VAT address and the two pivot sets)",
                     "re-export the mesh from the VAT ROP (Rigid-Body Dynamics mode)");
            return;
        }
        // pieces: the distinct frame-0 texels
        std::vector<int> pieceOfCorner(src.corners(), -1);
        std::map<std::pair<int, int>, int> texelPiece;
        std::vector<std::pair<int, int>> pieceTexel;
        std::vector<size_t> pieceCorner;
        for (size_t t = 0; t < src.triangles(); t++){
            bool guard = false;
            for (int c = 0; c < 3; c++) guard = guard || src.uvs[(size_t)vatUv][(t * 3 + (size_t)c) * 2 + 1] <= 0.1;
            if (guard) continue;
            for (int c = 0; c < 3; c++){
                const size_t corner = t * 3 + (size_t)c;
                int col = 0, row = 0;
                std::string problem;
                if (!addressTexel(src.uvs[(size_t)vatUv][corner * 2], src.uvs[(size_t)vatUv][corner * 2 + 1], d.width, d.activeRows,
                                  d.flags.activeU, 1.0, col, row, problem)){
                    addError(result, "metadata_mismatch", problem, "re-export the textures and the mesh together");
                    return;
                }
                if (row >= d.rowsPerFrame || col >= d.activeCols){
                    addError(result, "metadata_mismatch",
                             "a piece addresses row " + std::to_string(row) + ", column " + std::to_string(col) + " of frame 1, which has " +
                             std::to_string(d.rowsPerFrame) + " rows of " + std::to_string(d.activeCols) + " texels",
                             "re-export the textures and the mesh together");
                    return;
                }
                auto found = texelPiece.find({col, row});
                if (found == texelPiece.end()){
                    found = texelPiece.emplace(std::make_pair(col, row), (int)pieceTexel.size()).first;
                    pieceTexel.push_back({col, row});
                    pieceCorner.push_back(corner);
                }
                pieceOfCorner[corner] = found->second;
            }
        }
        const int pieces = (int)pieceTexel.size();
        result.report.pieces = pieces;
        if (pieces == 0){
            addError(result, "bad_mesh", "every triangle of the mesh is a guard triangle (VAT UV V <= 0.1)", "re-export the mesh");
            return;
        }

        // mode
        bool bones = pieces <= VAT_MAX_BONES;
        std::string modeReason;
        if (spec.mode == "vertices"){
            bones = false;
            modeReason = "Houdini rigid-body export, Vertices requested: per-vertex positions of " + std::to_string(pieces) + " pieces";
        }else if (spec.mode == "bones"){
            if (pieces > VAT_MAX_BONES){
                addError(result, "bone_limit", std::to_string(pieces) + " rigid pieces, Bones mode holds at most " + std::to_string(VAT_MAX_BONES),
                         "import as Vertices (mode: vertices or auto)");
                return;
            }
            modeReason = "Houdini rigid-body export: one bone per piece (" + std::to_string(pieces) + " pieces)";
        }else if (bones){
            modeReason = "Houdini rigid-body export: one bone per piece (" + std::to_string(pieces) + " pieces)";
        }else{
            modeReason = "Houdini rigid-body export with " + std::to_string(pieces) + " pieces, over the " + std::to_string(VAT_MAX_BONES) +
                         " bones of Bones mode: per-vertex positions";
            addWarning(result, "bone_limit", std::to_string(pieces) + " rigid pieces exceed Bones mode's " + std::to_string(VAT_MAX_BONES) +
                       " bones: imported as Vertices (a texel or two per vertex per frame instead of two per piece)",
                       "nothing to do; fewer pieces (merge small debris in Houdini) would allow Bones");
        }

        const bool smooth = d.meta.smoothTrajectories == 1;
        if (smooth && !d.col.loaded){
            addError(result, "missing_file", "smooth trajectories need the colour texture (the accelerations): " + d.meta.source +
                     " says Smoothly Interpolated Trajectories is on", "keep <asset>_col next to the position texture, or pass color");
            return;
        }
        if (d.meta.stretchByVelocity == 1){
            addWarning(result, "stretch_by_velocity_ignored", "Stretch by Velocity scales pieces non-uniformly, which bones and the "
                       "engine's layout can't hold: the pieces keep their shape", "nothing to do (turn it off in the material to match)");
        }
        const double pscaleMul = d.meta.pscaleMultiplier;
        const bool pscaleInAlpha = d.meta.pscaleInAlpha != 0;
        const double fps = d.meta.fps;

        // decode every piece's frames
        std::vector<PieceTrack> tracks((size_t)pieces);
        for (int b = 0; b < pieces; b++){
            PieceTrack& track = tracks[(size_t)b];
            const size_t corner = pieceCorner[(size_t)b];
            const double u2 = src.uvs[2][corner * 2], u3 = src.uvs[3][corner * 2], v3 = src.uvs[3][corner * 2 + 1];
            track.pivot = d.axis.fromW(D3(u2, u3, 1.0 - v3)) * (1.0 / d.axis.scale);
            const int col = pieceTexel[(size_t)b].first;
            const int row0 = pieceTexel[(size_t)b].second;
            for (int f = 0; f < d.frames; f++){
                const int row = row0 + f * d.rowsPerFrame;
                track.position.push_back(d.axis.point(d.position(col, row)));
                double r[4];
                d.rotation(col, row, r);
                const double alpha = d.pos.texel(col, row)[3];
                const int index = (int)std::floor(alpha * 4.0);
                const double w = std::sqrt(std::max(0.0, 1.0 - r[0] * r[0] - r[1] * r[1] - r[2] * r[2]));
                DQ q;
                switch (index){
                    case 1: q = DQ(w, r[1], r[2], r[0]); break;
                    case 2: q = DQ(r[0], -w, r[2], -r[1]); break;
                    case 3: q = DQ(r[0], r[1], -w, -r[2]); break;
                    default: q = DQ(r[0], r[1], r[2], w); break;
                }
                track.rotation.push_back(qnormalize(d.axis.quat(q)));
                double angle = r[3];
                if (!d.flags.hdr) angle = angle / std::max(1e-9, 1.0 - d.flags.minXDigit);
                track.angle.push_back(angle);
                const double pscale = pscaleInAlpha ? (1.0 - fract(alpha * 4.0)) * d.flags.alphaScale : 1.0;
                track.scale.push_back(pscale * pscaleMul);
                if (smooth){
                    const float* c = d.col.texel(col, row);
                    track.acceleration.push_back(d.axis.fromW(D3(c[0], c[1], c[2])) * (1.0 / d.axis.scale));
                }else if (d.useColor){
                    const float* c = d.col.texel(col, row);
                    track.color.push_back({c[0], c[1], c[2], c[3]});
                }
            }
        }
        for (size_t corner = 0; corner < src.corners(); corner++){
            const int b = pieceOfCorner[corner];
            if (b < 0) continue;
            tracks[(size_t)b].radius = std::max(tracks[(size_t)b].radius, length(src.position(corner) - tracks[(size_t)b].pivot));
        }

        // our mesh
        BuiltMesh built;
        buildMesh(src, pieceOfCorner, 0, true, !d.useColor || smooth, built, d.meta.twoSided == 1);
        VatMeshData& mesh = built.mesh;
        const size_t vertices = mesh.vertexCount();
        std::vector<int> vertexPiece(vertices);
        for (size_t v = 0; v < vertices; v++) vertexPiece[v] = (int)mesh.vatIndex[v];

        // tolerance (mm) from the motion's extent
        VatBounds extent;
        for (const PieceTrack& track : tracks){
            for (const D3& p : track.position){
                extent.extend(Vector3((float)(p.x - track.radius), (float)(p.y - track.radius), (float)(p.z - track.radius)));
                extent.extend(Vector3((float)(p.x + track.radius), (float)(p.y + track.radius), (float)(p.z + track.radius)));
            }
        }
        const Vector3 diag = extent.max - extent.min;
        const double toleranceM = std::max((double)spec.toleranceMm, std::sqrt((double)diag.x * diag.x + (double)diag.y * diag.y + (double)diag.z * diag.z) * 1000.0 * 0.0005) / 1000.0;

        // the upsampling factor: every sub-step turns at most 60 degrees, and the GPU's lerp (and
        // nlerp) between samples stays within the tolerance of Houdini's motion
        auto stepsFor = [&](const PieceTrack& track, int f, double lever) -> double {
            const double a = std::fabs(track.angle[(size_t)f]);
            double need = 1.0;
            if (a > 0.0){
                const double turnDegrees = a * 360.0;
                need = std::max(need, std::ceil(turnDegrees / 60.0 - 1e-9));
                const double phi = a * 2.0 * PI;
                if (lever > toleranceM){
                    const double maxHalf = std::acos(1.0 - toleranceM / lever);
                    need = std::max(need, std::ceil(phi / (2.0 * maxHalf) - 1e-9));
                }
            }
            if (smooth && f != 0){
                const D3& acc = track.acceleration[(size_t)f];
                const double magnitude = length(acc);
                if (magnitude > 0.0) need = std::max(need, std::ceil(std::sqrt(magnitude / (8.0 * toleranceM)) / fps - 1e-9));
            }
            return need;
        };
        auto leverOf = [&](const PieceTrack& track, int f) -> double {
            double maxScale = 0.0;
            for (double s : track.scale) maxScale = std::max(maxScale, std::fabs(s));
            (void)f;
            return bones ? maxScale * (length(track.pivot) + track.radius) : maxScale * track.radius;
        };
        double needed = 1.0;
        for (const PieceTrack& track : tracks){
            for (int f = 0; f < d.frames; f++) needed = std::max(needed, stepsFor(track, f, leverOf(track, f)));
        }
        const bool vertexColors = !bones && spec.colors && d.useColor && !smooth;
        const int perSample = bones ? 2 : 1 + (spec.normals ? 1 : 0) + (vertexColors ? 1 : 0);
        const int elements = bones ? pieces : (int)vertices;
        const uint64_t perFrame = (uint64_t)elements * (uint64_t)perSample;
        const uint64_t budget = perFrame > 0 ? limits.maxTexels / (perFrame * (uint64_t)d.frames) : 0;
        int upsample = 1;
        if (spec.upsample >= 1){
            upsample = spec.upsample;
        }else{
            const double capped = std::min(needed, (double)std::max<uint64_t>(budget, 1));
            upsample = (int)std::min(capped, 1024.0);
            if (needed > upsample){
                addWarning(result, "large_interpolation_error",
                           "fast-spinning pieces need " + num(needed) + " samples per frame to follow Houdini's motion within " +
                           num(toleranceM * 1000.0) + " mm, the preset allows " + std::to_string(upsample),
                           "use the desktop preset (preset: desktop), or accept the error");
            }
        }
        // the remaining error at that factor
        double interpolationError = 0.0;
        for (const PieceTrack& track : tracks){
            for (int f = 0; f < d.frames; f++){
                const double a = std::fabs(track.angle[(size_t)f]);
                if (a > 0.0){
                    const double phi = a * 2.0 * PI / upsample;
                    interpolationError = std::max(interpolationError, leverOf(track, f) * (1.0 - std::cos(phi / 2.0)));
                }
            }
        }
        const int samples = d.frames * upsample;
        std::vector<std::string> fixes = {"use the desktop preset (preset: desktop)"};
        if (spec.upsample > 1) fixes.push_back("lower the upsampling (upsample)");
        if (!bones) fixes.push_back("leave out the normals (normals: false)");
        fixes.push_back("export fewer frames");
        if (!checkPayloadSize(bones ? VatMode::Bones : VatMode::Vertices, samples, elements, perSample, limits,
                              "the rigid body's " + std::to_string(pieces) + " pieces", fixes, result)){
            return;
        }

        Assembled in;
        in.payload.mode = bones ? VatMode::Bones : VatMode::Vertices;
        in.payload.layout.width = limits.width;
        in.payload.layout.samples = samples;
        if (bones){
            in.payload.layout.bones = pieces;
            in.payload.layout.normals = false;
        }else{
            in.payload.layout.vertices = elements;
            in.payload.layout.normals = spec.normals;
            in.payload.layout.colors = vertexColors;
        }
        if (!in.payload.allocate()){
            addError(result, "too_large", "not enough memory for the payload", "export fewer frames");
            return;
        }
        // rest frames of our vertices (Vertices mode)
        std::vector<DQ> restFrame;
        if (!bones && spec.normals){
            restFrame.resize(vertices);
            for (size_t v = 0; v < vertices; v++){
                const size_t corner = built.sourceCorner[v];
                restFrame[v] = tangentFrame(cornerNormal(src, corner), cornerTangent(src, corner));
            }
        }
        std::vector<D3> piecePosition((size_t)pieces);
        std::vector<DQ> pieceRotation((size_t)pieces);
        std::vector<double> pieceScale((size_t)pieces);
        for (int f = 0; f < d.frames; f++){
            for (int j = 0; j < upsample; j++){
                const double t = (double)j / upsample;
                const int g = f * upsample + j;
                for (int b = 0; b < pieces; b++){
                    const PieceTrack& track = tracks[(size_t)b];
                    evaluatePiece(track, f, t, d.frames, fps, smooth, piecePosition[(size_t)b], pieceRotation[(size_t)b], pieceScale[(size_t)b]);
                    if (bones){
                        const DQ& rotation = pieceRotation[(size_t)b];
                        const double scale = pieceScale[(size_t)b];
                        const D3 translation = piecePosition[(size_t)b] - rotate(rotation, track.pivot * scale);
                        in.payload.set(g, b, 0, rotation.x, rotation.y, rotation.z, rotation.w);
                        in.payload.set(g, b, 1, translation.x, translation.y, translation.z, scale);
                    }
                }
                if (bones) continue;
                const int next = (f + 1) % d.frames;
                for (size_t v = 0; v < vertices; v++){
                    const int b = vertexPiece[v];
                    const PieceTrack& track = tracks[(size_t)b];
                    const DQ& rotation = pieceRotation[(size_t)b];
                    const D3 rest(mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2]);
                    const D3 now = piecePosition[(size_t)b] + rotate(rotation, (rest - track.pivot) * pieceScale[(size_t)b]);
                    const D3 offset = now - rest;
                    in.payload.set(g, (int)v, 0, offset.x, offset.y, offset.z, 0.0);
                    int k = 1;
                    if (spec.normals){
                        const DQ frame = qnormalize(qmul(rotation, restFrame[v]));
                        in.payload.set(g, (int)v, k++, frame.x, frame.y, frame.z, frame.w);
                    }
                    if (vertexColors){
                        const std::array<float, 4>& c0 = track.color[(size_t)f];
                        const std::array<float, 4>& c1 = track.color[(size_t)next];
                        in.payload.set(g, (int)v, k, c0[0] + (c1[0] - c0[0]) * t, c0[1] + (c1[1] - c0[1]) * t,
                                       c0[2] + (c1[2] - c0[2]) * t, c0[3] + (c1[3] - c0[3]) * t);
                    }
                }
            }
        }
        if (bones){
            mesh.joints.assign(vertices * 4, 0);
            mesh.weights.assign(vertices * 4, 0.0f);
            for (size_t v = 0; v < vertices; v++){
                mesh.joints[v * 4] = (uint16_t)vertexPiece[v];
                mesh.weights[v * 4] = 1.0f;
                mesh.vatIndex[v] = (float)v;
            }
            // per-piece colours become static vertex colours
            if (d.useColor && !smooth && spec.colors){
                bool changes = false;
                for (const PieceTrack& track : tracks){
                    for (const auto& c : track.color){
                        for (int k = 0; k < 4; k++) changes = changes || std::fabs(c[(size_t)k] - track.color[0][(size_t)k]) > 1e-6f;
                    }
                }
                mesh.colors.assign(vertices * 4, 1.0f);
                for (size_t v = 0; v < vertices; v++){
                    for (int k = 0; k < 4; k++) mesh.colors[v * 4 + (size_t)k] = tracks[(size_t)vertexPiece[v]].color[0][(size_t)k];
                }
                if (changes){
                    addWarning(result, "color_animation_ignored", "the pieces' colours change over time; Bones mode keeps each piece's "
                               "first-frame colour as a vertex colour", "import as Vertices (mode: vertices) to keep the animated colours");
                }
            }
        }else{
            if (vertexColors) mesh.colors.clear();
            mesh.joints.clear();
            mesh.weights.clear();
            for (size_t v = 0; v < vertices; v++){
                in.elementRest.push_back(D3(mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2]));
                mesh.vatIndex[v] = (float)v;
            }
        }
        in.mesh = std::move(mesh);
        in.xform = src.xform;
        in.clips.push_back(makeClip(clipName, samples, fps * upsample, true, false));
        in.modeReason = modeReason + (upsample > 1 ? ", upsampled x" + std::to_string(upsample) + " (" + std::to_string(samples) +
                                                     " samples) to follow fast spins" : std::string());
        in.sourceFps = (float)fps;
        in.sourceFrames = d.frames;
        in.upsample = upsample;
        in.interpolationErrorMm = (float)(interpolationError * 1000.0);
        finishImport(in, spec, result);
    }

    // Dynamic remeshing (fluid): every corner's position per frame through the lookup table,
    // absolute, no interpolation
    void importFluid(const VatImportSpec& spec, Decoder& d, const SourceMesh& src, const std::string& clipName,
                     const PresetLimits& limits, VatImportResult& result){
        const int lookupUv = 0;
        const FloatImage& L = d.lookup.image;
        const bool raw8 = !L.raw8.empty();
        const double depth = d.flags.minXDigit >= 0.5 ? 2048.0 : 255.0;
        const int lookupActiveRows = (int)std::lround(d.flags.activeV * L.height);
        if (std::fabs(d.flags.activeV * L.height - lookupActiveRows) > 0.25 || lookupActiveRows <= 0 || lookupActiveRows % d.frames != 0){
            addError(result, "metadata_mismatch", "the lookup table's active height (" + num(d.flags.activeV * L.height) + " of " +
                     std::to_string(L.height) + " rows) isn't a whole number of rows per frame for " + std::to_string(d.frames) + " frames",
                     "re-export the lookup table with the textures");
            return;
        }
        const int lookupRowsPerFrame = lookupActiveRows / d.frames;

        std::vector<int> element(src.corners(), -1);
        std::map<std::pair<int, int>, int> texelElement;
        std::vector<std::pair<int, int>> elementTexel;
        std::vector<size_t> elementCorner;
        for (size_t t = 0; t < src.triangles(); t++){
            bool guard = false;
            for (int c = 0; c < 3; c++) guard = guard || src.uvs[(size_t)lookupUv][(t * 3 + (size_t)c) * 2 + 1] <= 0.1;
            if (guard) continue;
            for (int c = 0; c < 3; c++){
                const size_t corner = t * 3 + (size_t)c;
                int col = 0, row = 0;
                std::string problem;
                if (!addressTexel(src.uvs[(size_t)lookupUv][corner * 2], src.uvs[(size_t)lookupUv][corner * 2 + 1], L.width,
                                  lookupActiveRows, d.flags.activeU, 1.0, col, row, problem)){
                    addError(result, "metadata_mismatch", "lookup: " + problem, "re-export the lookup table and the mesh together");
                    return;
                }
                if (row >= lookupRowsPerFrame){
                    addError(result, "metadata_mismatch", "a corner addresses lookup row " + std::to_string(row) + " of frame 1, which has " +
                             std::to_string(lookupRowsPerFrame) + " rows", "re-export the lookup table and the mesh together");
                    return;
                }
                auto found = texelElement.find({col, row});
                if (found == texelElement.end()){
                    found = texelElement.emplace(std::make_pair(col, row), (int)elementTexel.size()).first;
                    elementTexel.push_back({col, row});
                    elementCorner.push_back(corner);
                }
                element[corner] = found->second;
            }
        }
        const int elements = (int)elementTexel.size();
        if (elements == 0){
            addError(result, "bad_mesh", "every triangle of the mesh is a guard triangle (lookup UV V <= 0.1)", "re-export the mesh");
            return;
        }
        const bool compressed = d.meta.compressedNormals == 1 || (d.meta.compressedNormals < 0 && !d.rot.loaded);
        const bool normals = spec.normals && (d.rot.loaded || compressed);
        const bool colors = spec.colors && d.useColor;
        Assembled in;
        in.payload.mode = VatMode::Vertices;
        in.payload.layout.width = limits.width;
        in.payload.layout.samples = d.frames;
        in.payload.layout.vertices = elements;
        in.payload.layout.normals = normals;
        in.payload.layout.colors = colors;
        std::vector<std::string> fixes = {"use the desktop preset (preset: desktop)"};
        if (colors) fixes.push_back("leave out the colours (colors: false)");
        if (normals) fixes.push_back("leave out the normals (normals: false)");
        fixes.push_back("export fewer frames or a coarser surface");
        if (!checkPayloadSize(VatMode::Vertices, d.frames, elements, in.payload.layout.texelsPerSample(VatMode::Vertices), limits,
                              "the fluid's " + std::to_string(elements) + " corners", fixes, result)){
            return;
        }
        if (!in.payload.allocate()){
            addError(result, "too_large", "not enough memory for the payload", "export fewer frames");
            return;
        }
        auto lookupValue = [&](int c, int r, int channel) -> double {
            const size_t i = ((size_t)r * (size_t)L.width + (size_t)c) * 4 + (size_t)channel;
            return raw8 ? L.raw8[i] / 255.0 : (double)L.rgba[i];
        };
        std::vector<D3> firstPosition((size_t)elements);
        std::vector<D3> firstNormal((size_t)elements, D3(0, 1, 0));
        for (int e = 0; e < elements; e++){
            const int lc = elementTexel[(size_t)e].first;
            const int lr0 = elementTexel[(size_t)e].second;
            for (int f = 0; f < d.frames; f++){
                const int lr = lr0 + f * lookupRowsPerFrame;
                const double u = lookupValue(lc, lr, 0) + lookupValue(lc, lr, 1) / depth;
                const double vTop = lookupValue(lc, lr, 2) + lookupValue(lc, lr, 3) / depth;     // 1 - v
                const double x = u * d.width, y = vTop * d.height;
                const bool inside = texelInside(x, y, d.width, d.height);
                int col = inside ? (int)std::floor(x) : -1, row = inside ? (int)std::floor(y) : -1;
                // a corner that doesn't exist in this frame has an all-zero entry: the shader then
                // samples the texture's corner, texel (0, 0)
                const bool unused = lookupValue(lc, lr, 0) == 0.0 && lookupValue(lc, lr, 1) == 0.0 && lookupValue(lc, lr, 2) == 0.0 &&
                                    lookupValue(lc, lr, 3) == 0.0;
                if (unused){
                    col = 0;
                    row = 0;
                }else if (!inside || std::fabs(x - col - 0.5) > 0.25 || std::fabs(y - row - 0.5) > 0.25){
                    addError(result, "metadata_mismatch",
                             "lookup texel (" + std::to_string(lc) + ", " + std::to_string(lr) + ") decodes to (" + num(x) + ", " + num(y) +
                             ") of the " + std::to_string(d.width) + " x " + std::to_string(d.height) + " position texture, not a pixel centre "
                             "(lookup depth " + num(depth) + " from the bounds)", "re-export the lookup table with the textures");
                    return;
                }
                const D3 p = d.axis.point(d.position(col, row));
                in.payload.set(f, e, 0, p.x, p.y, p.z, 0.0);
                int k = 1;
                if (normals){
                    const DQ frame = compressed ? tangentFrame(d.compressedNormal(col, row), D3(1, 0, 0)) : d.rotationFrame(col, row);
                    in.payload.set(f, e, k++, frame.x, frame.y, frame.z, frame.w);
                    if (f == 0) firstNormal[(size_t)e] = rotate(frame, D3(0, 0, 1));
                }
                if (colors){
                    const float* c = d.col.texel(col, row);
                    in.payload.set(f, e, k, c[0], c[1], c[2], c[3]);
                }
                if (f == 0) firstPosition[(size_t)e] = p;
            }
        }
        // one vertex per corner element, at its first-frame position (the shader ignores the rest)
        VatMeshData& mesh = in.mesh;
        for (int e = 0; e < elements; e++){
            const D3& p = firstPosition[(size_t)e];
            mesh.positions.push_back((float)p.x); mesh.positions.push_back((float)p.y); mesh.positions.push_back((float)p.z);
            const D3& n = firstNormal[(size_t)e];
            mesh.normals.push_back((float)n.x); mesh.normals.push_back((float)n.y); mesh.normals.push_back((float)n.z);
            mesh.vatIndex.push_back((float)e);
            in.elementRest.push_back(p);
        }
        std::map<int, std::vector<uint32_t>> byMaterial;
        for (size_t t = 0; t < src.triangles(); t++){
            if (element[t * 3] < 0 || element[t * 3 + 1] < 0 || element[t * 3 + 2] < 0) continue;
            for (int c = 0; c < 3; c++) byMaterial[src.triangleMaterial[t]].push_back((uint32_t)element[t * 3 + (size_t)c]);
        }
        mesh.materials = src.materials;
        for (auto& entry : byMaterial){
            VatSubmesh submesh;
            submesh.indices = std::move(entry.second);
            submesh.material = entry.first;
            mesh.submeshes.push_back(std::move(submesh));
        }
        if (d.meta.twoSided == 1) makeDoubleSided(mesh);
        in.xform = src.xform;
        in.clips.push_back(makeClip(clipName, d.frames, d.meta.fps, false, true));
        in.modeReason = "Houdini dynamic-remeshing (fluid) export: absolute positions of " + std::to_string(elements) +
                        " corners per frame through the lookup table, no interpolation";
        in.sourceFps = d.meta.fps;
        in.sourceFrames = d.frames;
        addWarning(result, "topology_changes", "the fluid's surface changes topology every frame: stored as absolute positions per "
                   "corner with interpolation off (larger than Houdini's packed form)", "nothing to do");
        if (d.meta.uvFromRG == 1){
            addWarning(result, "fluid_uv_from_color_ignored", "the fluid's surface UVs come from the colour texture's red and green, "
                       "which the engine's layout doesn't carry: the surface has no UVs", "texture the fluid with a triplanar or "
                       "world-space material instead");
        }
        finishImport(in, spec, result);
    }

    // ---- shared resolution ----

    struct Resolved{
        Located where;
        VatImportFiles files;
        Meta meta;
    };

    void resolve(const Located& where, Resolved& out){
        out.where = where;
        const fs::path& root = where.root;
        const std::string& a = where.asset;
        if (!where.givenMesh.empty()){
            out.files.mesh = where.givenMesh;
        }else{
            std::vector<fs::path> candidates;
            for (const char* ext : MESH_EXTENSIONS){
                candidates.push_back(root / "geo" / FileUtils::pathFromUtf8(a + "_mesh" + ext));
                candidates.push_back(root / FileUtils::pathFromUtf8(a + "_mesh" + ext));
            }
            out.files.mesh = firstExisting(candidates);
        }
        out.files.position = findTexture(root, a, "pos");
        out.files.position2 = findTexture(root, a, "pos2");
        out.files.rotation = findTexture(root, a, "rot");
        out.files.color = findTexture(root, a, "col");
        out.files.lookup = findTexture(root, a, "lookup");
        out.files.material = firstExisting({root / "unity" / FileUtils::pathFromUtf8(a + "_mat.mat"), root / FileUtils::pathFromUtf8(a + "_mat.mat")});
        out.files.dataJson = firstExisting({root / "data" / FileUtils::pathFromUtf8(a + "_data.json"), root / FileUtils::pathFromUtf8(a + "_data.json")});
    }

    // metadata from the .mat, else the data JSON (the mesh bounds are read at import)
    void readMeta(Resolved& r){
        std::string text;
        if (!r.files.material.empty() && readText(r.files.material, text) && parseMaterial(text, r.meta)) return;
        Meta jsonMeta;
        if (!r.files.dataJson.empty() && readText(r.files.dataJson, text)){
            if (parseDataJson(text, jsonMeta)){
                r.meta = jsonMeta;
                return;
            }
            if (!jsonMeta.error.empty()) r.meta.error = fileName(r.files.dataJson) + ": " + jsonMeta.error;
        }
        r.meta.source = "mesh bounds";
    }

    // sprite exports: every triangle's corners share one VAT texel (the particle) and no rotation texture
    bool looksLikeSprites(const SourceMesh& src){
        if (src.uvs.size() < 2) return false;
        size_t checked = 0, shared = 0;
        for (size_t t = 0; t < src.triangles() && checked < 2000; t++){
            const double* uv = &src.uvs[1][t * 6];
            if (uv[1] <= 0.1 || uv[3] <= 0.1 || uv[5] <= 0.1) continue;
            checked++;
            if (uv[0] == uv[2] && uv[2] == uv[4] && uv[1] == uv[3] && uv[3] == uv[5]){
                // corners at different places sharing one texel
                if (length(src.position(t * 3) - src.position(t * 3 + 1)) > 0.0) shared++;
            }
        }
        return checked > 0 && shared == checked;
    }

    HoudiniMode modeFromFiles(const Resolved& r, const SourceMesh* mesh, std::string& source){
        if (!r.files.lookup.empty()){
            source = "textures and UV sets";
            return HoudiniMode::Fluid;
        }
        if (!mesh) return HoudiniMode::Unknown;
        source = "textures and UV sets";
        if (!r.files.rotation.empty() && mesh->uvs.size() >= 4) return HoudiniMode::Rigid;
        if (r.files.rotation.empty() && looksLikeSprites(*mesh)) return HoudiniMode::Sprite;
        return HoudiniMode::Soft;
    }

    void addIssue(VatImportDetection& out, const std::string& kind, const std::string& message, const std::string& fix, bool error){
        VatImportIssue issue;
        issue.kind = kind;
        issue.message = message;
        issue.fix = fix;
        issue.error = error;
        out.issues.push_back(issue);
    }

    // the missing pieces for a mode
    void checkFiles(const Resolved& r, HoudiniMode mode, VatImportDetection& out){
        const std::string& a = r.where.asset;
        if (r.files.mesh.empty()){
            addIssue(out, "missing_file", "the mesh (geo/" + a + "_mesh.fbx) was not found", "export with the Geometry enabled, or pass mesh", true);
        }
        if (r.files.position.empty()){
            addIssue(out, "missing_file", "the position texture (tex/" + a + "_pos.exr) was not found",
                     "export with the Position texture enabled, or pass position", true);
        }
        if (r.meta.twoPositions == 1 && r.files.position2.empty()){
            addIssue(out, "missing_file", "the second position texture (tex/" + a + "_pos2) was not found: " + r.meta.source +
                     " says positions use two textures", "keep " + a + "_pos2 next to " + a + "_pos, or pass position2", true);
        }
        if (mode == HoudiniMode::Fluid && r.files.lookup.empty()){
            addIssue(out, "missing_file", "the lookup table (tex/" + a + "_lookup.png) was not found: dynamic remeshing needs it",
                     "export the First Pass (lookup table) too, or pass lookup", true);
        }
        if (mode == HoudiniMode::Rigid && r.files.rotation.empty()){
            addIssue(out, "missing_file", "the rotation texture (tex/" + a + "_rot) was not found: rigid pieces need it",
                     "export with the Rotation texture enabled, or pass rotation", true);
        }
        if ((mode == HoudiniMode::Soft || mode == HoudiniMode::Fluid) && r.files.rotation.empty() && r.meta.compressedNormals == 0){
            addIssue(out, "missing_file", "rotation texture not found next to the mesh (tex/" + a + "_rot)",
                     "export with Normals enabled, or pass rotation (or import without normals: normals false)", true);
        }
        if (mode == HoudiniMode::Sprite){
            addIssue(out, "unsupported", "particle sprites are not supported yet (Houdini VAT 3 Particle Sprites export)",
                     "export the particles as rigid pieces (Rigid-Body Dynamics) or soft-body geometry instead", true);
        }
        if (r.meta.fps <= 0.0f){
            addIssue(out, "fps_unknown", "the export's frame rate is unknown (" + r.meta.source + " has no Houdini FPS)",
                     "pass fps (the Houdini scene's frames per second)", true);
        }
        if (r.meta.target.empty()){
            addIssue(out, "unknown_target", "the export's target engine (axes and units) can't be determined from the " + r.meta.source,
                     "re-export with Target Engine = Unity, or pass axis (unity, unreal, houdini or zup)", true);
        }
        if (!r.meta.error.empty()){
            addIssue(out, "read_failed", r.meta.error, "re-export the data file", false);
        }
    }

}

void detectHoudini(const std::string& path, VatImportDetection& out){
    Located where;
    if (!locate(path, where)) return;
    Resolved r;
    resolve(where, r);
    if (r.files.mesh.empty() && r.files.position.empty()) return;
    readMeta(r);
    out.kind = VatImportKind::HoudiniVat3;
    out.assetName = where.asset;
    out.folder = FileUtils::pathToUtf8(where.root);
    out.files = r.files;
    out.metadataSource = r.meta.source;
    out.frames = r.meta.frames;
    out.fps = r.meta.fps;
    out.target = r.meta.target;
    out.mode = r.meta.mode;
    out.modeSource = r.meta.modeSource;
    if (out.mode == HoudiniMode::Unknown){
        SourceMesh mesh;
        std::string error;
        const bool haveMesh = !r.files.mesh.empty() && readSourceMesh(r.files.mesh, mesh, error);
        out.mode = modeFromFiles(r, haveMesh ? &mesh : nullptr, out.modeSource);
        if (!haveMesh && !error.empty()) addIssue(out, "read_failed", error, "re-export the mesh", true);
    }
    checkFiles(r, out.mode, out);
    out.summary = "Houdini VAT 3 " + std::string(out.mode == HoudiniMode::Unknown ? "export" : std::string(houdiniModeName(out.mode)) + " export") +
                  " \"" + out.assetName + "\"" + (out.frames > 0 ? ", " + std::to_string(out.frames) + " frames" : std::string()) +
                  (out.fps > 0 ? " at " + num(out.fps) + " fps" : std::string()) + " (metadata: " + out.metadataSource + ")";
}

void importHoudini(const VatImportSpec& spec, const VatImportDetection& detection, VatImportResult& result){
    Located where;
    if (!locate(spec.path, where)){
        addError(result, "not_found", "no Houdini VAT 3 export at " + spec.path, "pass the ROP's export folder, its mesh or its material");
        return;
    }
    Resolved r;
    resolve(where, r);
    readMeta(r);
    auto override = [](std::string& file, const std::string& given){
        if (!given.empty()) file = FileUtils::pathToUtf8(fs::absolute(FileUtils::pathFromUtf8(given)).lexically_normal());
    };
    override(r.files.mesh, spec.mesh);
    override(r.files.position, spec.position);
    override(r.files.position2, spec.position2);
    override(r.files.rotation, spec.rotation);
    override(r.files.color, spec.color);
    override(r.files.lookup, spec.lookup);
    result.report.kind = VatImportKind::HoudiniVat3;

    Decoder d;
    d.meta = r.meta;
    if (spec.fps > 0.0f) d.meta.fps = spec.fps;
    if (!spec.axis.empty()) d.meta.target = spec.axis;

    // the mesh (needed for the mode fallback and the bounds fallback)
    SourceMesh src;
    std::string error;
    if (r.files.mesh.empty() || !fileExists(r.files.mesh)){
        addError(result, "missing_file", "the mesh (geo/" + where.asset + "_mesh.fbx) was not found", "export with the Geometry enabled, or pass mesh");
        return;
    }
    if (!readSourceMesh(r.files.mesh, src, error)){
        addError(result, src.point.empty() && error.find("no triangles") != std::string::npos ? "bad_mesh" : "read_failed", error,
                 "re-export the mesh");
        return;
    }
    result.filesRead.push_back(r.files.mesh);
    result.report.sourcePoints = (int)(src.positions.size() / 3);
    if (src.gltf){
        // the ROP's UVs are Houdini's / Unity's (V up); a glTF re-export stores 1 - v
        for (std::vector<double>& set : src.uvs){
            for (size_t i = 1; i < set.size(); i += 2) set[i] = 1.0 - set[i];
        }
        src.gltf = false;
    }
    if (src.embeddedTextures){
        addWarning(result, "embedded_textures_dropped", "textures embedded in " + fileName(r.files.mesh) + " are not copied to the .vat.glb",
                   "keep the textures as files next to the mesh, or assign them in the material");
    }
    HoudiniMode mode = d.meta.mode;
    if (mode == HoudiniMode::Unknown){
        std::string source;
        mode = modeFromFiles(r, &src, source);
    }
    result.report.houdiniMode = mode;

    // the remaining checks, with the options applied
    VatImportDetection issues;
    Resolved checked = r;
    checked.meta = d.meta;
    checkFiles(checked, mode, issues);
    for (const VatImportIssue& issue : issues.issues){
        if (issue.kind == "missing_file" && issue.message.rfind("rotation texture not found", 0) == 0 && !spec.normals) continue;
        if (issue.error) addError(result, issue.kind, issue.message, issue.fix);
        else addWarning(result, issue.kind, issue.message, issue.fix);
    }
    if (!result.errors.empty()) return;
    if (!axisFromName(d.meta.target, spec.axisScale > 0 ? spec.axisScale : d.meta.targetScale, d.axis)){
        addError(result, spec.axis.empty() ? "unknown_target" : "invalid_settings", "unknown axis \"" + d.meta.target + "\"",
                 "pass axis unity, unreal, houdini or zup");
        return;
    }
    if (!r.files.material.empty() && d.meta.source == "material") result.filesRead.push_back(r.files.material);
    if (!r.files.dataJson.empty() && d.meta.source == "data json") result.filesRead.push_back(r.files.dataJson);

    // bounds: the metadata's, else the mesh's raw box (the guard vertices) in the target's axes
    if (!d.meta.haveBounds){
        const D3 lo = src.boundsMin, hi = src.boundsMax;
        double mn[3] = {1e300, 1e300, 1e300}, mx[3] = {-1e300, -1e300, -1e300};
        for (int i = 0; i < 8; i++){
            const D3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
            const D3 t = d.axis.toTarget(corner);
            for (int k = 0; k < 3; k++){
                mn[k] = std::min(mn[k], t[k]);
                mx[k] = std::max(mx[k], t[k]);
            }
        }
        for (int k = 0; k < 3; k++){
            d.meta.bMin[k] = mn[k];
            d.meta.bMax[k] = mx[k];
        }
        d.meta.haveBounds = true;
    }
    for (int k = 0; k < 3; k++){
        if (!std::isfinite(d.meta.bMin[k]) || !std::isfinite(d.meta.bMax[k]) || std::fabs(d.meta.bMin[k]) > 1e15 || std::fabs(d.meta.bMax[k]) > 1e15){
            addError(result, "invalid_data", "the export's bounds (" + num(d.meta.bMin[k]) + " .. " + num(d.meta.bMax[k]) + " on axis " +
                     std::to_string(k) + ") are not finite numbers", "re-export from the VAT ROP");
            return;
        }
    }
    d.flags = decodeFlags(d.meta.bMin, d.meta.bMax);
    result.report.target = d.axis.name;

    // textures
    if (!loadTexture(r.files.position, "position", d.pos, result)) return;
    d.width = d.pos.image.width;
    d.height = d.pos.image.height;
    d.twoPositions = d.meta.twoPositions == 1 || (d.meta.twoPositions < 0 && !r.files.position2.empty());
    if (d.twoPositions && !loadTexture(r.files.position2, "second position", d.pos2, result)) return;
    const bool compressed = d.meta.compressedNormals == 1 || (d.meta.compressedNormals < 0 && r.files.rotation.empty());
    const bool needRotation = mode == HoudiniMode::Rigid || ((mode == HoudiniMode::Soft || mode == HoudiniMode::Fluid) && spec.normals && !compressed);
    if (needRotation){
        if (r.files.rotation.empty()){
            addError(result, "missing_file", "rotation texture not found next to the mesh (tex/" + where.asset + "_rot)",
                     "export with Normals enabled, or pass rotation (or import without normals: normals false)");
            return;
        }
        if (!loadTexture(r.files.rotation, "rotation", d.rot, result)) return;
    }
    if (compressed && (mode == HoudiniMode::Soft || mode == HoudiniMode::Fluid) && spec.normals && d.pos.image.sourceChannels < 4){
        addError(result, "missing_file", "no rotation texture and the position texture has no alpha for compressed normals",
                 "export with Normals enabled (rotation texture or compressed normals), pass rotation, or import without normals (normals: false)");
        return;
    }
    const bool loadColor = d.meta.loadColor != 0 && !r.files.color.empty();
    const bool smooth = mode == HoudiniMode::Rigid && d.meta.smoothTrajectories == 1;
    if ((loadColor && (spec.colors || smooth)) || (smooth && !r.files.color.empty())){
        if (!loadTexture(r.files.color, "colour", d.col, result)) return;
        d.useColor = !smooth;
    }
    if (mode == HoudiniMode::Fluid && !loadTexture(r.files.lookup, "lookup", d.lookup, result)) return;
    for (const Texture* t : {&d.pos2, &d.rot, &d.col}){
        if (t->loaded && (t->image.width != d.width || t->image.height != d.height)){
            addError(result, "metadata_mismatch", fileName(t->path) + " is " + std::to_string(t->image.width) + " x " +
                     std::to_string(t->image.height) + ", the position texture " + std::to_string(d.width) + " x " + std::to_string(d.height),
                     "re-export the textures together");
            return;
        }
    }
    if (d.meta.fps <= 0.0f){
        addError(result, "fps_unknown", "the export's frame rate is unknown", "pass fps (the Houdini scene's frames per second)");
        return;
    }

    // active area and frames (cross-checked with the textures and the UV rows)
    const double cols = d.flags.activeU * d.width;
    d.activeCols = (int)std::lround(cols);
    if (std::fabs(cols - d.activeCols) > 0.25 || d.activeCols < 1){
        addError(result, "metadata_mismatch", "the bounds' active width ratio " + num(d.flags.activeU) + " gives " + num(cols) + " of the " +
                 std::to_string(d.width) + " columns, not a whole number", "re-export (the bounds and textures disagree)");
        return;
    }
    if (mode != HoudiniMode::Fluid){
        const double rows = d.flags.activeV * d.height;
        d.activeRows = (int)std::lround(rows);
        if (std::fabs(rows - d.activeRows) > 0.25 || d.activeRows < 1){
            addError(result, "metadata_mismatch", "the bounds' active height ratio " + num(d.flags.activeV) + " gives " + num(rows) + " of the " +
                     std::to_string(d.height) + " rows, not a whole number", "re-export (the bounds and textures disagree)");
            return;
        }
        // rows of frame 1 used by the UVs
        const int vatUv = 1;
        if ((int)src.uvs.size() <= vatUv){
            addError(result, "bad_mesh", "the mesh has no second UV set (the VAT address, Houdini's uv2)", "re-export the mesh from the VAT ROP");
            return;
        }
        std::set<int> rowsUsed;
        for (size_t c = 0; c < src.corners(); c++){
            const double v = src.uvs[(size_t)vatUv][c * 2 + 1];
            if (v > 0.1 && v <= 1.0) rowsUsed.insert((int)std::floor((1.0 - v) * d.activeRows));   // (a UV outside the texture is refused later)
        }
        const int uvRows = (int)rowsUsed.size();
        if (d.meta.frames > 0){
            d.frames = d.meta.frames;
        }else{
            if (uvRows == 0 || d.activeRows % uvRows != 0){
                addError(result, "metadata_mismatch", "the frame count is unknown and " + std::to_string(d.activeRows) + " active rows aren't a "
                         "multiple of the " + std::to_string(uvRows) + " rows the UVs use", "pass the frame count through the material, or re-export");
                return;
            }
            d.frames = d.activeRows / uvRows;
        }
        if (d.activeRows % d.frames != 0){
            addError(result, "metadata_mismatch", std::to_string(d.frames) + " frames don't divide the " + std::to_string(d.activeRows) +
                     " active rows of the " + std::to_string(d.width) + " x " + std::to_string(d.height) + " textures",
                     "re-export the textures and the material together");
            return;
        }
        d.rowsPerFrame = d.activeRows / d.frames;
        if (uvRows > d.rowsPerFrame){
            addError(result, "metadata_mismatch", "the UVs use " + std::to_string(uvRows) + " rows per frame, the textures have " +
                     std::to_string(d.rowsPerFrame) + " (" + std::to_string(d.activeRows) + " rows / " + std::to_string(d.frames) + " frames)",
                     "re-export the textures and the mesh together");
            return;
        }
    }else{
        if (src.uvs.empty()){
            addError(result, "bad_mesh", "the fluid mesh has no UV set (the lookup address, Houdini's uv)", "re-export the mesh from the VAT ROP");
            return;
        }
        if (d.meta.frames <= 0){
            const int uv = 0;
            std::set<long long> distinct;
            for (size_t c = 0; c < src.corners(); c++){
                const double v = src.uvs[(size_t)uv][c * 2 + 1];
                if (v > 0.1 && v <= 1.0) distinct.insert(std::llround(std::floor((1.0 - v) * d.flags.activeV * d.lookup.image.height)));
            }
            const int lookupRows = (int)std::lround(d.flags.activeV * d.lookup.image.height);
            if (distinct.empty() || lookupRows % (int)distinct.size() != 0){
                addError(result, "metadata_mismatch", "the frame count is unknown and the lookup's " + std::to_string(lookupRows) +
                         " rows aren't a multiple of the " + std::to_string(distinct.size()) + " rows the UVs use", "pass the material with the export");
                return;
            }
            d.frames = lookupRows / (int)distinct.size();
        }else{
            d.frames = d.meta.frames;
        }
    }
    result.report.sourceFrames = d.frames;
    result.report.fps = d.meta.fps;

    PresetLimits limits;
    presetLimits(spec.preset, limits);
    const std::string clipName = spec.clipName.empty() ? where.asset : spec.clipName;
    switch (mode){
        case HoudiniMode::Soft: importSoft(spec, d, src, 1, clipName, limits, result); break;
        case HoudiniMode::Rigid: importRigid(spec, d, src, clipName, limits, result); break;
        case HoudiniMode::Fluid: importFluid(spec, d, src, clipName, limits, result); break;
        default:
            addError(result, "unsupported", "the export's mode can't be told (no material, JSON mode or telling textures)",
                     "keep the Unity material (unity/" + where.asset + "_mat.mat) next to the export");
            return;
    }
    (void)detection;
}

}
