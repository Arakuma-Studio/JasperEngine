// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VertexAnimation.h"

#include "animation/AnimationClipIO.h"
#include "texture/Texture.h"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <variant>

using namespace jasperengine;
using namespace jasperengine::vat;

// ---- small types ----

void VatBounds::extend(const Vector3& p){
    if (!valid){
        min = p;
        max = p;
        valid = true;
        return;
    }
    min = Vector3(std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z));
    max = Vector3(std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z));
}

void VatBounds::extend(const VatBounds& other){
    if (!other.valid) return;
    extend(other.min);
    extend(other.max);
}

float VatClip::length() const{
    if (!(fps > 0.0f) || frames < 1) return 0.0f;
    if (wrap == ClipWrapMode::Loop) return (float)frames / fps;
    return (float)(frames - 1) / fps;
}

int VatLayout::texelsPerSample(VatMode mode) const{
    if (mode == VatMode::Bones) return 2;
    return 1 + (normals ? 1 : 0) + (colors ? 1 : 0);
}

int VatData::findClip(const std::string& name) const{
    for (size_t i = 0; i < clips.size(); i++){
        if (clips[i].name == name) return (int)i;
    }
    return -1;
}

int VatData::findSocket(const std::string& name) const{
    for (size_t i = 0; i < sockets.size(); i++){
        if (sockets[i].name == name) return (int)i;
    }
    return -1;
}

const char* vat::modeName(VatMode mode){
    return mode == VatMode::Bones ? "bones" : "vertices";
}

bool vat::modeFromName(const std::string& name, VatMode& out){
    if (name == "vertices"){
        out = VatMode::Vertices;
        return true;
    }
    if (name == "bones"){
        out = VatMode::Bones;
        return true;
    }
    return false;
}

const char* vat::precisionName(VatPrecision precision){
    return precision == VatPrecision::Float ? "float" : "half";
}

bool vat::precisionFromName(const std::string& name, VatPrecision& out){
    if (name == "half"){
        out = VatPrecision::Half;
        return true;
    }
    if (name == "float"){
        out = VatPrecision::Float;
        return true;
    }
    return false;
}

uint64_t vat::vatTexelIndex(VatMode mode, const VatLayout& layout, int globalFrame, int element, int k){
    uint64_t elements = (uint64_t)std::max(mode == VatMode::Bones ? layout.bones : layout.vertices, 0);
    uint64_t perSample = (uint64_t)layout.texelsPerSample(mode);
    return ((uint64_t)globalFrame * elements + (uint64_t)element) * perSample + (uint64_t)k;
}

namespace{

    // bytes per texel of a payload (4 channels)
    uint64_t texelBytes(VatPrecision precision){
        return precision == VatPrecision::Float ? 16 : 8;
    }

    // the socket track column of data.sockets[socket]: its path's index in recipe.sockets (the bake
    // order), or the socket's own index when the recipe lists none (imports); -1 when unresolved
    int socketColumn(const VatData& data, int socket){
        if (socket < 0 || (size_t)socket >= data.sockets.size()) return -1;
        if (data.recipe.sockets.empty()) return socket;
        const std::string& path = data.sockets[socket].path;
        for (size_t i = 0; i < data.recipe.sockets.size(); i++){
            if (data.recipe.sockets[i] == path) return (int)i;
        }
        return -1;
    }

    // payload texels the clips need: samples x elements x texels per sample
    uint64_t neededTexels(const VatData& data){
        uint64_t elements = (uint64_t)std::max(data.mode == VatMode::Bones ? data.layout.bones : data.layout.vertices, 0);
        return (uint64_t)std::max(data.layout.samples, 0) * elements * (uint64_t)data.layout.texelsPerSample(data.mode);
    }

}

// ---- playback math (vat.glsl mirrors this) ----

namespace{

    // the frame pair at sample position x (samples since the clip's first frame, not wrapped yet)
    VatFrameSample sampleAt(const VatClip& clip, float x){
        VatFrameSample out;
        const float frames = (float)std::max(clip.frames, 1);
        const float last = frames - 1.0f;
        float f0 = 0.0f;
        float f1 = 0.0f;
        if (clip.wrap == ClipWrapMode::Loop || clip.wrap == ClipWrapMode::Default){
            x = x - frames * std::floor(x / frames);
            // float rounding of a tiny negative x lands on `frames` (or just below 0): the same
            // phase as 0, kept continuous by f0 = last with alpha 1 (= the first frame)
            x = std::min(std::max(x, 0.0f), frames);
            f0 = std::min(std::floor(x), last);
            f1 = (f0 + 1.0f == frames) ? 0.0f : f0 + 1.0f;
            out.normalized = x / frames;
        }else{
            if (clip.wrap == ClipWrapMode::PingPong){
                const float p = 2.0f * last;
                const float y = p > 0.0f ? x - p * std::floor(x / p) : 0.0f;
                x = y <= last ? y : p - y;
            }else{
                // Once / ClampForever: past the end going forward, before 0 going backward
                out.finished = x > last || x < 0.0f;
            }
            x = std::min(std::max(x, 0.0f), last);
            f0 = std::floor(x);
            f1 = std::min(f0 + 1.0f, last);
            out.normalized = last > 0.0f ? x / last : 0.0f;
        }
        out.alpha = (clip.interpolate && clip.frames > 1) ? x - f0 : 0.0f;
        out.frame0 = clip.start + (int)f0;
        out.frame1 = clip.start + (int)f1;
        return out;
    }

}

VatFrameSample vat::vatSample(const VatClip& clip, float tau){
    // C++ guards (the shader gets valid data): a non-finite time or a clip without a rate
    // samples its first frame
    float x = (std::isfinite(tau) && clip.fps > 0.0f) ? tau * clip.fps : 0.0f;
    if (!std::isfinite(x)) x = 0.0f;
    return sampleAt(clip, x);
}

VatFrameSample vat::vatSampleNormalized(const VatClip& clip, float normalized){
    if (!std::isfinite(normalized)) normalized = 0.0f;
    const float frames = (float)std::max(clip.frames, 1);
    const bool loop = clip.wrap == ClipWrapMode::Loop || clip.wrap == ClipWrapMode::Default;
    return sampleAt(clip, normalized * (loop ? frames : frames - 1.0f));
}

float vat::vatWeightB(const VatTrackPair& pair, float now){
    if (pair.b.clip < 0) return 0.0f;
    if (pair.blend >= 0.0f) return std::min(std::max(pair.blend, 0.0f), 1.0f);
    if (pair.fadeEnd > pair.fadeStart){
        float w = (now - pair.fadeStart) / (pair.fadeEnd - pair.fadeStart);
        return std::min(std::max(w, 0.0f), 1.0f);
    }
    return 1.0f;
}

void vat::vatPackInstance(const VatTrackPair& pair, Vector4& v0, Vector4& v1){
    v0 = Vector4((float)pair.a.clip, pair.a.start, pair.a.speed, (float)pair.b.clip);
    // fade mode keeps w >= 0 (a fade that ended before the epoch has weight 1 at any now >= 0
    // either way), so w < 0 always means blend mode
    float w = pair.blend >= 0.0f ? -(1.0f + pair.blend) : std::max(pair.fadeEnd, 0.0f);
    v1 = Vector4(pair.b.start, pair.b.speed, pair.fadeStart, w);
}

VatTrackPair vat::vatUnpackInstance(const Vector4& v0, const Vector4& v1){
    VatTrackPair pair;
    pair.a.clip = (int)std::lround(v0.x);
    pair.a.start = v0.y;
    pair.a.speed = v0.z;
    pair.b.clip = (int)std::lround(v0.w);
    pair.b.start = v1.x;
    pair.b.speed = v1.y;
    pair.fadeStart = v1.z;
    if (v1.w < 0.0f){
        pair.blend = -v1.w - 1.0f;
        pair.fadeEnd = 0.0f;
    }else{
        pair.blend = -1.0f;
        pair.fadeEnd = v1.w;
    }
    return pair;
}

// ---- layout hash ----

namespace{

    // shortest decimal that reads back as the same float (%.6g when that is enough)
    std::string floatNumber(float value){
        if (!std::isfinite(value)) return "0";
        char buffer[48];
        for (int precision = 6; precision <= 9; precision++){
            std::snprintf(buffer, sizeof(buffer), "%.*g", precision, (double)value);
            if ((float)std::strtod(buffer, nullptr) == value) break;
        }
        std::string text = buffer;
        return text == "-0" ? "0" : text;
    }

}

std::string vat::computeLayoutHash(const VatData& data){
    std::string text = std::string("mode=") + modeName(data.mode) +
                       ";precision=" + precisionName(data.layout.precision) +
                       ";width=" + std::to_string(data.layout.width) +
                       ";height=" + std::to_string(data.layout.height) +
                       ";samples=" + std::to_string(data.layout.samples) +
                       ";vertices=" + std::to_string(data.layout.vertices) +
                       ";bones=" + std::to_string(data.layout.bones) +
                       ";normals=" + (data.layout.normals ? "1" : "0") +
                       ";colors=" + (data.layout.colors ? "1" : "0") + ";clips=";
    for (const VatClip& clip : data.clips){
        text += std::to_string(clip.start) + "," + std::to_string(clip.frames) + "," + floatNumber(clip.fps) + "," +
                (clip.restNone ? "none" : "rest") + ";";
    }
    // 64-bit FNV-1a
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : text){
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)hash);
    return buffer;
}

// ---- parser ----

namespace{

    using OJson = nlohmann::ordered_json;

    const char* const REBAKE = "rebake (Inspector > Rebake, or bake_vertex_animation {rebake})";
    const char* const IMPORT_POINTER = "import the export with Import as Vertex Animation... (Project window, right-click) "
                                       "or bake_vertex_animation {source: \"<the export>\"}";

    void warn(std::vector<ClipWarning>& warnings, const std::string& path, const std::string& message){
        warnings.push_back({path, message});
    }

    std::string indexed(const std::string& path, const char* name, size_t index){
        return path + (path.empty() ? "" : ".") + name + "[" + std::to_string(index) + "]";
    }

    std::string member(const std::string& path, const std::string& name){
        return path.empty() ? name : path + "." + name;
    }

    std::string shortNumber(double value){
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
        return buffer;
    }

    // times live on the 1e-6 s grid the file stores (the .anim rule)
    float quantizeTime(float time){
        return (float)(std::round((double)time * 1e6) / 1e6);
    }

    bool toFloat(const OJson& value, float& out){
        if (!value.is_number()) return false;
        double number = value.get<double>();
        if (!std::isfinite(number) || std::fabs(number) > (double)FLT_MAX) return false;
        out = (float)number;
        return true;
    }

    // a whole JSON number within [min, max]
    bool toInteger(const OJson& value, int64_t min, int64_t max, int64_t& out){
        if (!value.is_number()) return false;
        double number = value.get<double>();
        if (!std::isfinite(number) || number != std::floor(number) || number < (double)min || number > (double)max) return false;
        if (value.is_number_unsigned()) out = (int64_t)value.get<uint64_t>();
        else if (value.is_number_integer()) out = value.get<int64_t>();
        else out = (int64_t)number;
        return out >= min && out <= max;
    }

    bool toInt(const OJson& value, int min, int max, int& out){
        int64_t number = 0;
        if (!toInteger(value, min, max, number)) return false;
        out = (int)number;
        return true;
    }

    bool isHex(const std::string& text){
        if (text.empty()) return false;
        for (char c : text){
            if (!std::isxdigit((unsigned char)c)) return false;
        }
        return true;
    }

    bool readBounds(const OJson& value, VatBounds& out){
        if (!value.is_object()) return false;
        auto min = value.find("min");
        auto max = value.find("max");
        if (min == value.end() || max == value.end() || !min->is_array() || !max->is_array() ||
            min->size() != 3 || max->size() != 3 || value.size() != 2) return false;
        float a[3];
        float b[3];
        for (size_t i = 0; i < 3; i++){
            if (!toFloat((*min)[i], a[i]) || !toFloat((*max)[i], b[i]) || a[i] > b[i]) return false;
        }
        out.min = Vector3(a[0], a[1], a[2]);
        out.max = Vector3(b[0], b[1], b[2]);
        out.valid = true;
        return true;
    }

    // the .anim event rules: {time, function, args}, a Lua identifier, at most
    // MAX_ANIMATION_EVENT_ARGS scalar args, time >= 0 on the 1e-6 grid
    bool readEvent(const OJson& json, const std::string& path, ClipEventData& event, std::vector<ClipWarning>& warnings){
        if (!json.is_object()){
            warn(warnings, path, "expected {\"time\", \"function\", \"args\"}, event dropped");
            return false;
        }
        for (auto it = json.begin(); it != json.end(); ++it){
            const std::string& name = it.key();
            if (name != "time" && name != "function" && name != "args"){
                warn(warnings, member(path, name), "unknown key ignored");
            }
        }
        auto time = json.find("time");
        if (time == json.end() || !toFloat(*time, event.time)){
            warn(warnings, member(path, "time"), "expected a time in seconds, event dropped");
            return false;
        }
        auto function = json.find("function");
        if (function == json.end() || !function->is_string()){
            warn(warnings, member(path, "function"), "expected a Lua function name, event dropped");
            return false;
        }
        event.function = function->get<std::string>();
        if (!anim::isLuaIdentifier(event.function)){
            warn(warnings, member(path, "function"), "\"" + event.function + "\" is not a Lua function name, event dropped");
            return false;
        }
        auto args = json.find("args");
        if (args != json.end()){
            if (!args->is_array()){
                warn(warnings, member(path, "args"), "expected an array of numbers, strings and booleans, ignored");
            }else{
                for (size_t i = 0; i < args->size(); i++){
                    const OJson& arg = (*args)[i];
                    if (arg.is_boolean()){
                        event.args.push_back(AnimationEvent::boolArg(arg.get<bool>()));
                    }else if (arg.is_number()){
                        event.args.push_back(AnimationEvent::numberArg(arg.get<double>()));
                    }else if (arg.is_string()){
                        event.args.push_back(AnimationEvent::stringArg(arg.get<std::string>()));
                    }else{
                        warn(warnings, indexed(path, "args", i), "expected a number, string or boolean, dropped");
                    }
                }
                if (event.args.size() > MAX_ANIMATION_EVENT_ARGS){
                    warn(warnings, member(path, "args"), "more than " + std::to_string(MAX_ANIMATION_EVENT_ARGS) +
                                                         " args: the rest are dropped");
                    event.args.resize(MAX_ANIMATION_EVENT_ARGS);
                }
            }
        }
        if (event.time < 0.0f){
            warn(warnings, member(path, "time"), "negative time " + shortNumber(event.time) + ", using 0");
            event.time = 0.0f;
        }
        event.time = quantizeTime(event.time);
        return true;
    }

    void sortEvents(std::vector<ClipEventData>& events){
        std::stable_sort(events.begin(), events.end(), [](const ClipEventData& a, const ClipEventData& b){
            if (a.time != b.time) return a.time < b.time;
            return a.function < b.function;
        });
    }

    // `base`, else "base 2", "base 3"... the first not in `used`
    std::string uniqueName(const std::string& base, const std::vector<std::string>& used){
        auto taken = [&](const std::string& name){
            return std::find(used.begin(), used.end(), name) != used.end();
        };
        if (!taken(base)) return base;
        for (int n = 2;; n++){
            std::string candidate = base + " " + std::to_string(n);
            if (!taken(candidate)) return candidate;
        }
    }

    // line and column (1-based, code points; a UTF-8 BOM is not a column) of the byte at `index`
    void textPosition(const std::string& text, size_t index, int& line, int& column){
        if (index > text.size()) index = text.size();
        line = 1;
        size_t lineStart = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? std::min<size_t>(3, index) : 0;
        for (size_t i = lineStart; i < index; i++){
            if (text[i] == '\n'){
                line++;
                lineStart = i + 1;
            }
        }
        column = 1;
        for (size_t i = lineStart; i < index; i++){
            if (((unsigned char)text[i] & 0xC0) != 0x80) column++;
        }
    }

    // nlohmann's DOM builder recording where and why the parse stopped (the .anim parser's)
    class VatSaxParser : public nlohmann::detail::json_sax_dom_parser<OJson>{
    public:
        explicit VatSaxParser(OJson& root): json_sax_dom_parser<OJson>(root, false){}

        template<class Exception>
        bool parse_error(std::size_t position, const std::string& lastToken, const Exception& ex){
            failed = true;
            errorPosition = position;
            errorToken = lastToken;
            message = ex.what();
            overflow = std::is_same<Exception, nlohmann::detail::out_of_range>::value;
            return json_sax_dom_parser<OJson>::parse_error(position, lastToken, ex);
        }

        bool failed = false;
        bool overflow = false;
        std::size_t errorPosition = 0;
        std::string errorToken;
        std::string message;
    };

    // What a file that is not ours looks like, for the refusal ("" = nothing recognised)
    std::string foreignKind(const std::string& text){
        size_t start = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
        while (start < text.size() && std::isspace((unsigned char)text[start])) start++;
        if (text.compare(start, 5, "%YAML") == 0 || text.find("--- !u!") != std::string::npos){
            return "a Unity asset (YAML)";
        }
        static const unsigned char exr[] = {0x76, 0x2f, 0x31, 0x01};
        static const unsigned char png[] = {0x89, 'P', 'N', 'G'};
        if (text.size() >= 4 && std::memcmp(text.data(), exr, 4) == 0) return "an EXR image (a VAT texture)";
        if (text.size() >= 4 && std::memcmp(text.data(), png, 4) == 0) return "a PNG image (a VAT texture)";
        if (text.compare(0, 4, "glTF") == 0) return "a binary glTF (the generated .vat.glb or a model)";
        if (text.compare(0, 18, "Kaydara FBX Binary") == 0) return "an FBX model (a VAT mesh)";
        if (text.find('\0') != std::string::npos) return "a binary file";
        return std::string();
    }

    // a JSON object of another tool's VAT export
    std::string foreignJsonKind(const OJson& root){
        auto has = [&](const char* key){ return root.find(key) != root.end(); };
        if (has("Bound Min X") || has("Houdini FPS") || has("Number of Frames") || has("Two Position Textures") ||
            has("Axis System")){
            return "a Houdini Labs VAT export's data JSON";
        }
        if (has("os-remap") || has("animations")){
            return "an OpenVAT remap info JSON";
        }
        auto format = root.find("format");
        if (format != root.end() && format->is_string() && format->get<std::string>() == anim::ANIM_FORMAT){
            return "an animation clip (.anim)";
        }
        return std::string();
    }

    std::string refusal(const std::string& kind){
        return "not a JasperEngine vertex animation: this is " + kind + ". JasperEngine .vat files are JSON {\"format\": \"" +
               std::string(VAT_FORMAT) + "\"} made by the baker or the importers: " + IMPORT_POINTER;
    }

    const char* const knownTopLevelKeys[] = {"format", "version", "guid", "mode", "mesh", "layout", "bounds", "clips",
                                             "sockets", "recipe", "stats"};

    bool isKnownTopLevelKey(const std::string& name){
        for (const char* known : knownTopLevelKeys){
            if (name == known) return true;
        }
        return false;
    }

    // The parse proper; `fail` records an invalid generated field
    class VatReader{
    public:
        VatParseResult& result;
        VatData data;

        explicit VatReader(VatParseResult& result): result(result){}

        bool fail(const std::string& path, const std::string& why){
            warn(result.warnings, path, why);
            result.error = "generated field " + path + " is invalid: " + REBAKE;
            return false;
        }

        bool read(const OJson& root);

    private:
        bool readLayout(const OJson& root);
        bool readClips(const OJson& root);
        bool readClip(const OJson& json, size_t index, VatClip& clip);
        void readEditableClipFields(const OJson& json, size_t index, VatClip& clip);
        void readSockets(const OJson& root);
        void readRecipe(const OJson& root);
        void readStats(const OJson& root);
    };

    bool VatReader::read(const OJson& root){
        std::vector<ClipWarning>& warnings = result.warnings;

        auto guid = root.find("guid");
        if (guid != root.end()){
            if (!guid->is_string() || !anim::isClipGuid(guid->get<std::string>())){
                warn(warnings, "guid", "expected 32 lowercase hex digits: dropped (a new one is given on save)");
            }else{
                data.guid = guid->get<std::string>();
            }
        }

        auto mode = root.find("mode");
        if (mode == root.end() || !mode->is_string() || !modeFromName(mode->get<std::string>(), data.mode)){
            return fail("mode", "expected \"vertices\" or \"bones\"");
        }

        auto mesh = root.find("mesh");
        if (mesh == root.end() || !mesh->is_string() || mesh->get<std::string>().empty()){
            return fail("mesh", "expected the generated mesh's file name (\"<name>.vat.glb\")");
        }
        data.mesh = mesh->get<std::string>();

        if (!readLayout(root)) return false;

        auto bounds = root.find("bounds");
        if (bounds != root.end() && !readBounds(*bounds, data.bounds)){
            warn(warnings, "bounds", "expected {\"min\": [x, y, z], \"max\": [x, y, z]} with min <= max, ignored");
        }

        // the recipe first: it names the baked sockets
        readRecipe(root);
        if (!readClips(root)) return false;
        readSockets(root);
        readStats(root);

        std::string unknownNames;
        for (auto it = root.begin(); it != root.end(); ++it){
            if (isKnownTopLevelKey(it.key())) continue;
            data.unknownKeys.push_back({it.key(), it->dump(-1, ' ', false, OJson::error_handler_t::replace)});
            unknownNames += (unknownNames.empty() ? "" : ", ") + it.key();
        }
        if (!unknownNames.empty()){
            warn(warnings, "", "unknown keys kept as they are (written back on save): " + unknownNames);
        }
        return true;
    }

    bool VatReader::readLayout(const OJson& root){
        auto found = root.find("layout");
        if (found == root.end() || !found->is_object()){
            return fail("layout", "expected the payload layout object");
        }
        const OJson& layout = *found;
        VatLayout& out = data.layout;
        static const char* const keys[] = {"precision", "width", "height", "samples", "vertices", "bones", "normals",
                                           "colors", "layout_hash"};
        for (auto it = layout.begin(); it != layout.end(); ++it){
            if (std::find_if(std::begin(keys), std::end(keys), [&](const char* key){ return it.key() == key; }) == std::end(keys)){
                warn(result.warnings, "layout." + it.key(), "unknown key ignored");
            }
        }
        auto field = [&](const char* name) -> const OJson*{
            auto it = layout.find(name);
            return it == layout.end() ? nullptr : &*it;
        };

        const OJson* precision = field("precision");
        if (!precision || !precision->is_string() || !precisionFromName(precision->get<std::string>(), out.precision)){
            return fail("layout.precision", "expected \"half\" or \"float\"");
        }
        if (data.mode == VatMode::Bones && out.precision != VatPrecision::Float){
            return fail("layout.precision", "bones payloads are always \"float\"");
        }
        const OJson* width = field("width");
        if (!width || !toInt(*width, 1, 65536, out.width)) return fail("layout.width", "expected texels per row (1..65536)");
        const OJson* height = field("height");
        if (!height || !toInt(*height, 1, 65536, out.height)) return fail("layout.height", "expected rows (1..65536)");
        const OJson* samples = field("samples");
        if (!samples || !toInt(*samples, 1, INT_MAX, out.samples)) return fail("layout.samples", "expected the sample count (>= 1)");
        const OJson* vertices = field("vertices");
        if (!vertices || !toInt(*vertices, data.mode == VatMode::Vertices ? 1 : 0, VAT_MAX_VERTICES, out.vertices)){
            return fail("layout.vertices", std::string("expected the payload vertex count (") +
                        (data.mode == VatMode::Vertices ? "1" : "0") + ".." + std::to_string(VAT_MAX_VERTICES) + ")");
        }
        const OJson* bones = field("bones");
        if (!bones || !toInt(*bones, data.mode == VatMode::Bones ? 1 : 0, VAT_MAX_BONES, out.bones)){
            return fail("layout.bones", std::string("expected the baked bone count (") +
                        (data.mode == VatMode::Bones ? "1" : "0") + ".." + std::to_string(VAT_MAX_BONES) + ")");
        }
        const OJson* normals = field("normals");
        if (!normals || !normals->is_boolean()) return fail("layout.normals", "expected true or false");
        out.normals = normals->get<bool>();
        const OJson* colors = field("colors");
        if (!colors || !colors->is_boolean()) return fail("layout.colors", "expected true or false");
        out.colors = colors->get<bool>();
        const OJson* hash = field("layout_hash");
        if (!hash || !hash->is_string() || !isHex(hash->get<std::string>())){
            return fail("layout.layout_hash", "expected the hex hash the .vat.glb also holds");
        }
        out.layoutHash = hash->get<std::string>();

        uint64_t capacity = (uint64_t)out.width * (uint64_t)out.height;
        uint64_t needed = neededTexels(data);
        if (needed > capacity){
            return fail("layout.height", "the payload holds " + std::to_string(out.width) + " x " + std::to_string(out.height) +
                        " texels, the samples need " + std::to_string(needed));
        }
        return true;
    }

    bool VatReader::readClips(const OJson& root){
        auto found = root.find("clips");
        if (found == root.end() || !found->is_array()){
            return fail("clips", "expected the array of clips");
        }
        std::vector<std::string> names;
        for (size_t i = 0; i < found->size(); i++){
            const OJson& json = (*found)[i];
            std::string path = indexed("", "clips", i);
            if (!json.is_object()){
                warn(result.warnings, path, "expected a clip object, dropped");
                continue;
            }
            if (data.clips.size() >= (size_t)VAT_MAX_CLIPS){
                warn(result.warnings, path, "more than " + std::to_string(VAT_MAX_CLIPS) + " clips: dropped");
                continue;
            }
            VatClip clip;
            if (!readClip(json, i, clip)) return false;
            std::string name = uniqueName(clip.name, names);
            if (name != clip.name){
                warn(result.warnings, member(path, "name"), "\"" + clip.name + "\" is used by another clip: renamed \"" + name + "\"");
                clip.name = name;
            }
            names.push_back(clip.name);
            data.clips.push_back(std::move(clip));
        }
        return true;
    }

    bool VatReader::readClip(const OJson& json, size_t index, VatClip& clip){
        std::string path = indexed("", "clips", index);
        static const char* const keys[] = {"name", "start", "frames", "fps", "wrap", "speed", "interpolate", "rest",
                                           "bounds", "events"};
        for (auto it = json.begin(); it != json.end(); ++it){
            if (std::find_if(std::begin(keys), std::end(keys), [&](const char* key){ return it.key() == key; }) == std::end(keys)){
                warn(result.warnings, member(path, it.key()), "unknown key ignored");
            }
        }

        // generated
        auto start = json.find("start");
        if (start == json.end() || !toInt(*start, 0, INT_MAX, clip.start)){
            return fail(member(path, "start"), "expected the clip's first global frame (>= 0)");
        }
        auto frames = json.find("frames");
        if (frames == json.end() || !toInt(*frames, 1, INT_MAX, clip.frames)){
            return fail(member(path, "frames"), "expected the clip's sample count (>= 1)");
        }
        if ((int64_t)clip.start + (int64_t)clip.frames > (int64_t)data.layout.samples){
            return fail(member(path, "frames"), "frames " + std::to_string(clip.start) + ".." +
                        std::to_string((int64_t)clip.start + clip.frames - 1) + " are past the payload's " +
                        std::to_string(data.layout.samples) + " samples");
        }
        auto fps = json.find("fps");
        if (fps == json.end() || !toFloat(*fps, clip.fps) || !(clip.fps > 0.0f)){
            return fail(member(path, "fps"), "expected samples per second (> 0)");
        }
        auto rest = json.find("rest");
        if (rest != json.end()){
            if (!rest->is_string() || rest->get<std::string>() != "none"){
                return fail(member(path, "rest"), "expected \"none\" (absolute positions) or no \"rest\"");
            }
            clip.restNone = true;
        }
        auto bounds = json.find("bounds");
        if (bounds != json.end() && !readBounds(*bounds, clip.bounds)){
            warn(result.warnings, member(path, "bounds"), "expected {\"min\": [x, y, z], \"max\": [x, y, z]} with min <= max, ignored");
        }

        readEditableClipFields(json, index, clip);
        return true;
    }

    void VatReader::readEditableClipFields(const OJson& json, size_t index, VatClip& clip){
        std::vector<ClipWarning>& warnings = result.warnings;
        const std::string path = indexed("", "clips", index);

        auto name = json.find("name");
        if (name == json.end() || !name->is_string() || name->get<std::string>().empty()){
            std::string fallback = "Clip " + std::to_string(index + 1);
            warn(warnings, member(path, "name"), "expected a clip name: using \"" + fallback + "\"");
            clip.name = fallback;
        }else{
            clip.name = name->get<std::string>();
        }

        auto wrap = json.find("wrap");
        clip.wrap = ClipWrapMode::Loop;
        if (wrap != json.end()){
            ClipWrapMode parsed = ClipWrapMode::Loop;
            if (!wrap->is_string() || !anim::wrapFromName(wrap->get<std::string>(), parsed) || parsed == ClipWrapMode::Default){
                warn(warnings, member(path, "wrap"), "expected loop, once, ping_pong or clamp_forever: using loop");
                parsed = ClipWrapMode::Loop;
            }
            clip.wrap = parsed;
        }

        auto speed = json.find("speed");
        if (speed != json.end() && !toFloat(*speed, clip.speed)){
            warn(warnings, member(path, "speed"), "expected a number (1 = as baked, negative plays backwards): using 1");
            clip.speed = 1.0f;
        }

        auto interpolate = json.find("interpolate");
        if (interpolate != json.end()){
            if (!interpolate->is_boolean()){
                warn(warnings, member(path, "interpolate"), "expected true or false: using true");
                clip.interpolate = true;
            }else{
                clip.interpolate = interpolate->get<bool>();
            }
        }

        auto events = json.find("events");
        if (events != json.end()){
            if (!events->is_array()){
                warn(warnings, member(path, "events"), "expected an array of events, ignored");
            }else{
                const float length = clip.length();
                for (size_t i = 0; i < events->size(); i++){
                    std::string eventPath = indexed(path, "events", i);
                    ClipEventData event;
                    if (!readEvent((*events)[i], eventPath, event, warnings)) continue;
                    if (event.time > quantizeTime(length)){
                        warn(warnings, member(eventPath, "time"), "after the clip's end (" + shortNumber(length) + " s): using the end");
                        event.time = quantizeTime(length);
                    }
                    clip.events.push_back(std::move(event));
                }
                sortEvents(clip.events);
            }
        }
    }

    void VatReader::readSockets(const OJson& root){
        std::vector<ClipWarning>& warnings = result.warnings;
        auto found = root.find("sockets");
        if (found == root.end()) return;
        if (!found->is_array()){
            warn(warnings, "sockets", "expected an array of {\"name\", \"path\"}, ignored");
            return;
        }
        std::vector<std::string> names;
        for (size_t i = 0; i < found->size(); i++){
            const OJson& json = (*found)[i];
            std::string path = indexed("", "sockets", i);
            if (!json.is_object()){
                warn(warnings, path, "expected {\"name\", \"path\"}, socket dropped");
                continue;
            }
            for (auto it = json.begin(); it != json.end(); ++it){
                if (it.key() != "name" && it.key() != "path") warn(warnings, member(path, it.key()), "unknown key ignored");
            }
            VatSocket socket;
            auto nodePath = json.find("path");
            if (nodePath == json.end() || !nodePath->is_string()){
                warn(warnings, member(path, "path"), "expected the baked node's path, socket dropped");
                continue;
            }
            socket.path = nodePath->get<std::string>();
            if (!data.recipe.sockets.empty() &&
                std::find(data.recipe.sockets.begin(), data.recipe.sockets.end(), socket.path) == data.recipe.sockets.end()){
                warn(warnings, member(path, "path"), "\"" + socket.path + "\" is not a baked socket (recipe.sockets), socket "
                     "dropped: add it to the bake's sockets and rebake");
                continue;
            }
            auto name = json.find("name");
            if (name == json.end() || !name->is_string() || name->get<std::string>().empty()){
                size_t slash = socket.path.find_last_of('/');
                std::string fallback = slash == std::string::npos ? socket.path : socket.path.substr(slash + 1);
                if (fallback.empty()) fallback = "Socket " + std::to_string(i + 1);
                warn(warnings, member(path, "name"), "expected a socket name: using \"" + fallback + "\"");
                socket.name = fallback;
            }else{
                socket.name = name->get<std::string>();
            }
            std::string unique = uniqueName(socket.name, names);
            if (unique != socket.name){
                warn(warnings, member(path, "name"), "\"" + socket.name + "\" is used by another socket: renamed \"" + unique + "\"");
                socket.name = unique;
            }
            names.push_back(socket.name);
            data.sockets.push_back(std::move(socket));
        }
    }

    bool readStringArray(const OJson& value, std::vector<std::string>& out){
        if (!value.is_array()) return false;
        out.clear();
        for (const OJson& item : value){
            if (!item.is_string()) return false;
            out.push_back(item.get<std::string>());
        }
        return true;
    }

    void VatReader::readRecipe(const OJson& root){
        std::vector<ClipWarning>& warnings = result.warnings;
        VatRecipe& recipe = data.recipe;
        auto found = root.find("recipe");
        if (found == root.end()){
            warn(warnings, "recipe", "missing: Rebake and the staleness check are not available");
            return;
        }
        if (!found->is_object()){
            warn(warnings, "recipe", "expected the bake recipe object, ignored: Rebake and the staleness check are not available");
            return;
        }
        for (auto it = found->begin(); it != found->end(); ++it){
            const std::string& key = it.key();
            const OJson& value = *it;
            std::string path = "recipe." + key;
            if (key == "sources"){
                if (!value.is_array()){
                    warn(warnings, path, "expected an array of sources, ignored");
                    continue;
                }
                for (size_t i = 0; i < value.size(); i++){
                    const OJson& json = value[i];
                    std::string sourcePath = indexed("recipe", "sources", i);
                    if (!json.is_object()){
                        warn(warnings, sourcePath, "expected {\"kind\", \"path\", \"clips\", \"hashes\"}, source dropped");
                        continue;
                    }
                    VatRecipeSource source;
                    bool ok = true;
                    for (auto field = json.begin(); field != json.end() && ok; ++field){
                        const std::string& name = field.key();
                        std::string fieldPath = member(sourcePath, name);
                        if (name == "kind" || name == "path" || name == "scene"){
                            if (!field->is_string()){
                                warn(warnings, fieldPath, "expected a string, source dropped");
                                ok = false;
                                break;
                            }
                            (name == "kind" ? source.kind : name == "path" ? source.path : source.scene) = field->get<std::string>();
                        }else if (name == "clips"){
                            if (!readStringArray(*field, source.clips)){
                                warn(warnings, fieldPath, "expected an array of clip names, ignored");
                                source.clips.clear();
                            }
                        }else if (name == "hashes"){
                            if (!field->is_object()){
                                warn(warnings, fieldPath, "expected {\"<file>\": \"sha1:<hex>\"}, ignored");
                                continue;
                            }
                            for (auto hash = field->begin(); hash != field->end(); ++hash){
                                if (!hash->is_string()){
                                    warn(warnings, member(fieldPath, hash.key()), "expected \"sha1:<hex>\", ignored");
                                    continue;
                                }
                                source.hashes.push_back({hash.key(), hash->get<std::string>()});
                            }
                        }else{
                            warn(warnings, fieldPath, "unknown key ignored");
                        }
                    }
                    if (!ok) continue;
                    if (source.kind.empty()){
                        warn(warnings, member(sourcePath, "kind"), "missing, source dropped");
                        continue;
                    }
                    recipe.sources.push_back(std::move(source));
                }
            }else if (key == "mode"){
                if (!value.is_string() || (value.get<std::string>() != "auto" && value.get<std::string>() != "vertices" &&
                                           value.get<std::string>() != "bones")){
                    warn(warnings, path, "expected auto, vertices or bones: using auto");
                    recipe.mode = "auto";
                }else{
                    recipe.mode = value.get<std::string>();
                }
            }else if (key == "fps"){
                if (!toFloat(value, recipe.fps) || !(recipe.fps > 0.0f)){
                    warn(warnings, path, "expected samples per second (> 0): using 30");
                    recipe.fps = 30.0f;
                }
            }else if (key == "preset"){
                if (!value.is_string() || (value.get<std::string>() != "mobile_web" && value.get<std::string>() != "desktop")){
                    warn(warnings, path, "expected mobile_web or desktop: using mobile_web");
                    recipe.preset = "mobile_web";
                }else{
                    recipe.preset = value.get<std::string>();
                }
            }else if (key == "normals"){
                if (!value.is_boolean()){
                    warn(warnings, path, "expected true or false: using true");
                    recipe.normals = true;
                }else{
                    recipe.normals = value.get<bool>();
                }
            }else if (key == "precision"){
                if (!value.is_string() || (value.get<std::string>() != "auto" && value.get<std::string>() != "half" &&
                                           value.get<std::string>() != "float")){
                    warn(warnings, path, "expected auto, half or float: using auto");
                    recipe.precision = "auto";
                }else{
                    recipe.precision = value.get<std::string>();
                }
            }else if (key == "tolerance_mm"){
                if (!toFloat(value, recipe.toleranceMm) || recipe.toleranceMm < 0.0f){
                    warn(warnings, path, "expected millimetres (>= 0): using 1");
                    recipe.toleranceMm = 1.0f;
                }
            }else if (key == "sockets"){
                if (!readStringArray(value, recipe.sockets)){
                    warn(warnings, path, "expected an array of node paths, ignored");
                    recipe.sockets.clear();
                }
            }else if (key == "import"){
                if (!value.is_object()){
                    warn(warnings, path, "expected the importer's options object, ignored");
                }else{
                    recipe.importOptions = value.dump(-1, ' ', false, OJson::error_handler_t::replace);
                }
            }else{
                warn(warnings, path, "unknown key ignored");
            }
        }
    }

    void VatReader::readStats(const OJson& root){
        std::vector<ClipWarning>& warnings = result.warnings;
        auto found = root.find("stats");
        if (found == root.end()) return;
        if (!found->is_object()){
            warn(warnings, "stats", "expected {\"bytes\", \"max_error_mm\", \"mean_error_mm\"}, ignored");
            return;
        }
        for (auto it = found->begin(); it != found->end(); ++it){
            const std::string& key = it.key();
            std::string path = "stats." + key;
            if (key == "bytes"){
                int64_t bytes = 0;
                if (!toInteger(*it, 0, INT64_MAX, bytes)){
                    warn(warnings, path, "expected the payload size in bytes, using 0");
                    bytes = 0;
                }
                data.stats.bytes = (uint64_t)bytes;
            }else if (key == "max_error_mm" || key == "mean_error_mm"){
                float& field = key == "max_error_mm" ? data.stats.maxErrorMm : data.stats.meanErrorMm;
                if (!toFloat(*it, field) || field < 0.0f){
                    warn(warnings, path, "expected millimetres (>= 0), using 0");
                    field = 0.0f;
                }
            }else{
                warn(warnings, path, "unknown key ignored");
            }
        }
    }

}

static VatParseResult parseVatText(const std::string& text){
    VatParseResult result;
    std::string kind = foreignKind(text);
    if (!kind.empty()){
        result.error = refusal(kind);
        return result;
    }

    OJson root;
    VatSaxParser sax(root);
    OJson::sax_parse(text, &sax);
    if (sax.failed){
        int line = 0;
        int column = 0;
        std::string message;
        if (sax.overflow){
            size_t end = std::min(sax.errorPosition, text.size());
            size_t start = sax.errorToken.empty() ? std::string::npos : text.rfind(sax.errorToken, end);
            textPosition(text, start != std::string::npos ? start : (end > 0 ? end - 1 : 0), line, column);
            message = "not valid JSON: the number " + sax.errorToken + " is out of range";
        }else{
            textPosition(text, sax.errorPosition > 0 ? sax.errorPosition - 1 : 0, line, column);
            size_t at = sax.message.find("syntax error");
            message = "not valid JSON: " + (at != std::string::npos ? sax.message.substr(at) : sax.message);
        }
        result.error = std::to_string(line) + ":" + std::to_string(column) + ": " + message;
        return result;
    }
    if (!root.is_object()){
        result.error = "not a vertex animation: the top level must be a JSON object";
        return result;
    }

    auto format = root.find("format");
    if (format == root.end() || !format->is_string() || format->get<std::string>() != VAT_FORMAT){
        std::string foreign = foreignJsonKind(root);
        result.error = foreign.empty()
            ? std::string("not a vertex animation: \"format\" must be \"") + VAT_FORMAT + "\""
            : refusal(foreign);
        return result;
    }
    auto version = root.find("version");
    if (version == root.end()){
        result.error = "missing \"version\" (1)";
        return result;
    }
    if (!version->is_number() || version->get<double>() != std::floor(version->get<double>()) || version->get<double>() < 1.0){
        result.error = "invalid \"version\": expected a whole number from 1";
        return result;
    }
    if (version->get<double>() > VAT_VERSION){
        result.error = "unsupported vertex animation version " + shortNumber(version->get<double>()) +
                       " (this engine reads versions up to " + std::to_string(VAT_VERSION) + ")";
        return result;
    }

    VatReader reader(result);
    if (reader.read(root)) result.data = std::move(reader.data);
    return result;
}

VatParseResult vat::parseVat(const std::string& text){
    // problems are results; anything thrown below is a bug, never a reason to take a loader down
    try{
        return parseVatText(text);
    }catch (const std::exception& e){
        VatParseResult result;
        result.error = std::string("cannot read the vertex animation: ") + e.what();
        return result;
    }catch (...){
        VatParseResult result;
        result.error = "cannot read the vertex animation";
        return result;
    }
}

// ---- canonical writer ----

namespace{

    std::string quote(const std::string& text){
        return OJson(text).dump(-1, ' ', false, OJson::error_handler_t::replace);
    }

    // seconds with at most 6 decimals, no trailing zeros (the .anim time rule)
    std::string timeNumber(float value){
        if (!std::isfinite(value)) return "0";
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6f", value);
        std::string text = buffer;
        size_t end = text.size();
        while (end > 0 && text[end - 1] == '0') end--;
        if (end > 0 && text[end - 1] == '.') end--;
        text.resize(end);
        return (text == "-0" || text.empty()) ? "0" : text;
    }

    std::string argText(const AnimationEventArg& arg){
        if (std::holds_alternative<bool>(arg)) return std::get<bool>(arg) ? "true" : "false";
        if (std::holds_alternative<std::string>(arg)) return quote(std::get<std::string>(arg));
        double value = std::get<double>(arg);
        if (!std::isfinite(value)) return "0";
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.15g", value);
        std::string text = buffer;
        return text == "-0" ? "0" : text;
    }

    std::string boolText(bool value){
        return value ? "true" : "false";
    }

    std::string vectorText(const Vector3& v){
        return "[" + floatNumber(v.x) + ", " + floatNumber(v.y) + ", " + floatNumber(v.z) + "]";
    }

    std::string boundsText(const VatBounds& bounds){
        return "{\"min\": " + vectorText(bounds.min) + ", \"max\": " + vectorText(bounds.max) + "}";
    }

    std::string stringsText(const std::vector<std::string>& values){
        std::string text = "[";
        for (size_t i = 0; i < values.size(); i++){
            if (i > 0) text += ", ";
            text += quote(values[i]);
        }
        return text + "]";
    }

    std::string eventText(const ClipEventData& event){
        std::string text = "{\"time\": " + timeNumber(event.time) + ", \"function\": " + quote(event.function);
        if (!event.args.empty()){
            text += ", \"args\": [";
            for (size_t i = 0; i < event.args.size(); i++){
                if (i > 0) text += ", ";
                text += argText(event.args[i]);
            }
            text += "]";
        }
        return text + "}";
    }

    std::string clipText(const VatClip& clip){
        ClipWrapMode wrap = clip.wrap == ClipWrapMode::Default ? ClipWrapMode::Loop : clip.wrap;
        std::string text = "    {\n";
        text += "      \"name\": " + quote(clip.name) + ",\n";
        text += "      \"start\": " + std::to_string(clip.start) + ",\n";
        text += "      \"frames\": " + std::to_string(clip.frames) + ",\n";
        text += "      \"fps\": " + floatNumber(clip.fps) + ",\n";
        text += std::string("      \"wrap\": ") + quote(anim::wrapName(wrap)) + ",\n";
        text += "      \"speed\": " + floatNumber(clip.speed) + ",\n";
        text += "      \"interpolate\": " + boolText(clip.interpolate) + ",\n";
        if (clip.restNone) text += "      \"rest\": \"none\",\n";
        if (clip.bounds.valid) text += "      \"bounds\": " + boundsText(clip.bounds) + ",\n";
        if (!clip.events.empty()){
            std::vector<ClipEventData> events = clip.events;
            sortEvents(events);
            text += "      \"events\": [\n";
            for (size_t i = 0; i < events.size(); i++){
                text += "        " + eventText(events[i]) + (i + 1 < events.size() ? ",\n" : "\n");
            }
            text += "      ],\n";
        }
        text.erase(text.size() - 2, 1);     // the last member's comma
        return text + "    }";
    }

    std::string sourceText(const VatRecipeSource& source){
        std::string text = "{\"kind\": " + quote(source.kind) + ", \"path\": " + quote(source.path);
        if (!source.scene.empty()) text += ", \"scene\": " + quote(source.scene);
        text += ", \"clips\": " + stringsText(source.clips) + ", \"hashes\": {";
        for (size_t i = 0; i < source.hashes.size(); i++){
            if (i > 0) text += ", ";
            text += quote(source.hashes[i].first) + ": " + quote(source.hashes[i].second);
        }
        return text + "}}";
    }

}

std::string vat::writeVat(const VatData& data){
    std::string text = "{\n";
    text += std::string("  \"format\": ") + quote(VAT_FORMAT) + ",\n";
    text += "  \"version\": " + std::to_string(VAT_VERSION) + ",\n";
    if (!data.guid.empty()) text += "  \"guid\": " + quote(data.guid) + ",\n";
    text += std::string("  \"mode\": ") + quote(modeName(data.mode)) + ",\n";
    text += "  \"mesh\": " + quote(data.mesh) + ",\n";

    const VatLayout& layout = data.layout;
    text += "  \"layout\": {\n";
    text += std::string("    \"precision\": ") + quote(precisionName(layout.precision)) + ",\n";
    text += "    \"width\": " + std::to_string(layout.width) + ",\n";
    text += "    \"height\": " + std::to_string(layout.height) + ",\n";
    text += "    \"samples\": " + std::to_string(layout.samples) + ",\n";
    text += "    \"vertices\": " + std::to_string(layout.vertices) + ",\n";
    text += "    \"bones\": " + std::to_string(layout.bones) + ",\n";
    text += "    \"normals\": " + boolText(layout.normals) + ",\n";
    text += "    \"colors\": " + boolText(layout.colors) + ",\n";
    text += "    \"layout_hash\": " + quote(layout.layoutHash) + "\n";
    text += "  },\n";

    if (data.bounds.valid) text += "  \"bounds\": " + boundsText(data.bounds) + ",\n";

    if (data.clips.empty()){
        text += "  \"clips\": [],\n";
    }else{
        text += "  \"clips\": [\n";
        for (size_t i = 0; i < data.clips.size(); i++){
            text += clipText(data.clips[i]) + (i + 1 < data.clips.size() ? ",\n" : "\n");
        }
        text += "  ],\n";
    }

    if (data.sockets.empty()){
        text += "  \"sockets\": [],\n";
    }else{
        text += "  \"sockets\": [\n";
        for (size_t i = 0; i < data.sockets.size(); i++){
            text += "    {\"name\": " + quote(data.sockets[i].name) + ", \"path\": " + quote(data.sockets[i].path) + "}";
            text += (i + 1 < data.sockets.size()) ? ",\n" : "\n";
        }
        text += "  ],\n";
    }

    const VatRecipe& recipe = data.recipe;
    text += "  \"recipe\": {\n";
    if (recipe.sources.empty()){
        text += "    \"sources\": [],\n";
    }else{
        text += "    \"sources\": [\n";
        for (size_t i = 0; i < recipe.sources.size(); i++){
            text += "      " + sourceText(recipe.sources[i]) + (i + 1 < recipe.sources.size() ? ",\n" : "\n");
        }
        text += "    ],\n";
    }
    text += "    \"mode\": " + quote(recipe.mode) + ",\n";
    text += "    \"fps\": " + floatNumber(recipe.fps) + ",\n";
    text += "    \"preset\": " + quote(recipe.preset) + ",\n";
    text += "    \"normals\": " + boolText(recipe.normals) + ",\n";
    text += "    \"precision\": " + quote(recipe.precision) + ",\n";
    text += "    \"tolerance_mm\": " + floatNumber(recipe.toleranceMm) + ",\n";
    text += "    \"sockets\": " + stringsText(recipe.sockets) + ",\n";
    if (!recipe.importOptions.empty()){
        OJson options = OJson::parse(recipe.importOptions, nullptr, false);
        if (options.is_object()){
            text += "    \"import\": " + options.dump(-1, ' ', false, OJson::error_handler_t::replace) + ",\n";
        }
    }
    text.erase(text.size() - 2, 1);
    text += "  },\n";

    text += "  \"stats\": {\"bytes\": " + std::to_string(data.stats.bytes) + ", \"max_error_mm\": " +
            floatNumber(data.stats.maxErrorMm) + ", \"mean_error_mm\": " + floatNumber(data.stats.meanErrorMm) + "},\n";

    // unknown keys in file order (the last of a repeated name), verbatim
    for (size_t i = 0; i < data.unknownKeys.size(); i++){
        const auto& entry = data.unknownKeys[i];
        if (isKnownTopLevelKey(entry.first)) continue;
        bool repeated = false;
        for (size_t j = i + 1; j < data.unknownKeys.size(); j++){
            if (data.unknownKeys[j].first == entry.first) repeated = true;
        }
        if (repeated) continue;
        text += "  " + quote(entry.first) + ": " + (entry.second.empty() ? std::string("null") : entry.second) + ",\n";
    }
    text.erase(text.size() - 2, 1);
    text += "}\n";
    return text;
}

// ---- runtime asset ----

std::shared_ptr<const VertexAnimation> VertexAnimation::create(VatData data, VatPayload payload, VatSocketTracks sockets,
                                                               std::string* error){
    auto failWith = [&](const std::string& message) -> std::shared_ptr<const VertexAnimation>{
        if (error) *error = message;
        return nullptr;
    };
    const VatLayout& layout = data.layout;
    if (layout.width < 1 || layout.height < 1 || layout.samples < 1){
        return failWith("invalid layout " + std::to_string(layout.width) + " x " + std::to_string(layout.height) + ", " +
                        std::to_string(layout.samples) + " samples: " + REBAKE);
    }
    if (data.mode == VatMode::Bones && (layout.bones < 1 || layout.precision != VatPrecision::Float)){
        return failWith(std::string("a bones payload needs bones >= 1 and float precision: ") + REBAKE);
    }
    if (data.mode == VatMode::Vertices && layout.vertices < 1){
        return failWith(std::string("a vertices payload needs vertices >= 1: ") + REBAKE);
    }
    if (payload.precision != layout.precision || payload.width != layout.width || payload.height != layout.height){
        return failWith("the payload is " + std::to_string(payload.width) + " x " + std::to_string(payload.height) + " " +
                        precisionName(payload.precision) + ", the layout says " + std::to_string(layout.width) + " x " +
                        std::to_string(layout.height) + " " + precisionName(layout.precision) + ": " + REBAKE);
    }
    uint64_t texels = (uint64_t)layout.width * (uint64_t)layout.height;
    uint64_t expectedBytes = texels * texelBytes(layout.precision);
    if ((uint64_t)payload.bytes.size() != expectedBytes){
        return failWith("the payload has " + std::to_string(payload.bytes.size()) + " bytes, " + std::to_string(layout.width) +
                        " x " + std::to_string(layout.height) + " " + precisionName(layout.precision) + " texels are " +
                        std::to_string(expectedBytes) + ": " + REBAKE);
    }
    uint64_t needed = neededTexels(data);
    if (needed > texels){
        return failWith("the samples need " + std::to_string(needed) + " texels, the payload holds " + std::to_string(texels) +
                        ": " + REBAKE);
    }
    for (size_t i = 0; i < data.clips.size(); i++){
        const VatClip& clip = data.clips[i];
        if (clip.start < 0 || clip.frames < 1 || (int64_t)clip.start + clip.frames > layout.samples || !(clip.fps > 0.0f)){
            return failWith("clip \"" + clip.name + "\" (frames " + std::to_string(clip.start) + " + " +
                            std::to_string(clip.frames) + ") doesn't fit the payload's " + std::to_string(layout.samples) +
                            " samples: " + REBAKE);
        }
    }
    if (sockets.sockets < 0 || sockets.frames < 0 ||
        (uint64_t)sockets.data.size() != (uint64_t)sockets.sockets * (uint64_t)sockets.frames * 7){
        return failWith("the socket tracks have " + std::to_string(sockets.data.size()) + " floats, " +
                        std::to_string(sockets.sockets) + " sockets x " + std::to_string(sockets.frames) +
                        " frames x 7 are " + std::to_string((uint64_t)std::max(sockets.sockets, 0) *
                                                           (uint64_t)std::max(sockets.frames, 0) * 7) + ": " + REBAKE);
    }
    if (sockets.sockets > 0 && sockets.frames != layout.samples){
        return failWith("the socket tracks have " + std::to_string(sockets.frames) + " frames, the payload " +
                        std::to_string(layout.samples) + " samples: " + REBAKE);
    }
    for (size_t i = 0; i < data.sockets.size(); i++){
        int column = socketColumn(data, (int)i);
        if (column < 0 || column >= sockets.sockets){
            return failWith("socket \"" + data.sockets[i].name + "\" has no baked track (" + std::to_string(sockets.sockets) +
                            " in the payload): " + REBAKE);
        }
    }

    std::shared_ptr<VertexAnimation> asset(new VertexAnimation());
    asset->data = std::move(data);
    asset->payload = std::move(payload);
    asset->sockets = std::move(sockets);
    return asset;
}

float VertexAnimation::clipLength(int clip) const{
    if (clip < 0 || (size_t)clip >= data.clips.size()) return 0.0f;
    return data.clips[clip].length();
}

namespace{

    struct SocketPose{
        float p[3] = {0, 0, 0};
        float q[4] = {0, 0, 0, 1};      // x y z w
    };

    // nlerp with the hemisphere fix (b flipped to a's side)
    void nlerp(const float* a, const float* b, float t, float* out){
        float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        float sign = dot < 0.0f ? -1.0f : 1.0f;
        float length = 0.0f;
        for (int i = 0; i < 4; i++){
            out[i] = a[i] + (sign * b[i] - a[i]) * t;
            length += out[i] * out[i];
        }
        length = std::sqrt(length);
        if (length > 0.0f){
            for (int i = 0; i < 4; i++) out[i] /= length;
        }else{
            out[0] = out[1] = out[2] = 0.0f;
            out[3] = 1.0f;
        }
    }

    SocketPose blendPoses(const SocketPose& a, const SocketPose& b, float t){
        SocketPose out;
        for (int i = 0; i < 3; i++) out.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
        nlerp(a.q, b.q, t, out.q);
        return out;
    }

    SocketPose socketPoseAt(const VatSocketTracks& tracks, int column, int frame){
        SocketPose pose;
        const float* row = tracks.data.data() + ((size_t)frame * (size_t)tracks.sockets + (size_t)column) * 7;
        for (int i = 0; i < 3; i++) pose.p[i] = row[i];
        for (int i = 0; i < 4; i++) pose.q[i] = row[3 + i];
        return pose;
    }

    SocketPose socketPoseAt(const VatSocketTracks& tracks, int column, const VatFrameSample& sample){
        return blendPoses(socketPoseAt(tracks, column, sample.frame0), socketPoseAt(tracks, column, sample.frame1), sample.alpha);
    }

}

bool VertexAnimation::sampleSocket(int socket, const VatTrackPair& pair, float now, Vector3& position, Quaternion& rotation) const{
    int column = socketColumn(data, socket);
    if (column < 0 || column >= sockets.sockets) return false;
    const int clipCount = (int)data.clips.size();
    const bool hasA = pair.a.clip >= 0 && pair.a.clip < clipCount;
    const bool hasB = pair.b.clip >= 0 && pair.b.clip < clipCount;
    if (!hasA && !hasB) return false;

    SocketPose pose;
    VatFrameSample sampleA;
    if (hasA){
        sampleA = vatSample(data.clips[pair.a.clip], (now - pair.a.start) * pair.a.speed);
        pose = socketPoseAt(sockets, column, sampleA);
    }
    if (hasB){
        const VatClip& clipB = data.clips[pair.b.clip];
        // blend mode: B's phase follows A's; a fade plays B on its own clock
        VatFrameSample sampleB = (pair.blend >= 0.0f && hasA)
            ? vatSampleNormalized(clipB, sampleA.normalized)
            : vatSample(clipB, (now - pair.b.start) * pair.b.speed);
        SocketPose poseB = socketPoseAt(sockets, column, sampleB);
        // the sockets have no rest pose: B alone when A is the rest
        pose = hasA ? blendPoses(pose, poseB, vatWeightB(pair, now)) : poseB;
    }
    position = Vector3(pose.p[0], pose.p[1], pose.p[2]);
    rotation = Quaternion(pose.q[3], pose.q[0], pose.q[1], pose.q[2]);
    return true;
}

Texture* VertexAnimation::getTexture() const{
    if (texture) return texture.get();
    static std::atomic<uint64_t> counter{0};
    // unique per asset object: a rebake under the same path must not get the old pooled pixels
    std::string id = "vat:" + (path.empty() ? std::string("memory") : path) + "#" + std::to_string(++counter);
    ColorFormat format = payload.precision == VatPrecision::Float ? ColorFormat::RGBA32F : ColorFormat::RGBA16F;
    // the pixels stay owned by the payload, which lives as long as this asset
    TextureData textureData(payload.width, payload.height, (unsigned int)payload.bytes.size(), format, 4,
                            const_cast<uint8_t*>(payload.bytes.data()));
    // the texture data pool entry points into the payload: destroyed with the asset
    texture = std::shared_ptr<Texture>(new Texture(), [](Texture* t){
        t->destroy();
        delete t;
    });
    texture->setData(id, textureData);
    texture->setMinFilter(TextureFilter::NEAREST);
    texture->setMagFilter(TextureFilter::NEAREST);
    texture->setWrapU(TextureWrap::CLAMP_TO_EDGE);
    texture->setWrapV(TextureWrap::CLAMP_TO_EDGE);
    return texture.get();
}

// ---- the .vat.glb reader ----

namespace{

    uint32_t readU32(const std::vector<uint8_t>& bytes, size_t at){
        return (uint32_t)bytes[at] | ((uint32_t)bytes[at + 1] << 8) | ((uint32_t)bytes[at + 2] << 16) | ((uint32_t)bytes[at + 3] << 24);
    }

    bool glbFail(std::string* error, const std::string& message){
        if (error) *error = "invalid vertex animation mesh: " + message;
        return false;
    }

    // a whole JSON number >= 0
    bool jsonCount(const nlohmann::json& object, const char* key, uint64_t& out, bool required, uint64_t fallback = 0){
        auto it = object.find(key);
        if (it == object.end()){
            out = fallback;
            return !required;
        }
        if (it->is_number_unsigned()){
            out = it->get<uint64_t>();
            return true;
        }
        if (it->is_number_integer()){
            int64_t value = it->get<int64_t>();
            if (value < 0) return false;
            out = (uint64_t)value;
            return true;
        }
        if (it->is_number_float()){
            double value = it->get<double>();
            if (!(value >= 0.0) || value != std::floor(value) || value > 9.0e15) return false;
            out = (uint64_t)value;
            return true;
        }
        return false;
    }

    // the bytes of bufferView `index` inside the BIN chunk
    bool bufferViewSpan(const nlohmann::json& root, uint64_t index, size_t binStart, size_t binLength, size_t& offset,
                        size_t& length, std::string* error, const std::string& what){
        auto views = root.find("bufferViews");
        if (views == root.end() || !views->is_array() || index >= views->size()){
            return glbFail(error, what + ": bufferView " + std::to_string(index) + " doesn't exist");
        }
        const nlohmann::json& view = (*views)[index];
        if (!view.is_object()) return glbFail(error, what + ": bufferView " + std::to_string(index) + " is not an object");
        uint64_t buffer = 0;
        uint64_t byteOffset = 0;
        uint64_t byteLength = 0;
        if (!jsonCount(view, "buffer", buffer, true) || !jsonCount(view, "byteOffset", byteOffset, false) ||
            !jsonCount(view, "byteLength", byteLength, true)){
            return glbFail(error, what + ": bufferView " + std::to_string(index) + " has no valid buffer/byteOffset/byteLength");
        }
        if (view.find("byteStride") != view.end()){
            return glbFail(error, what + ": bufferView " + std::to_string(index) + " must be tightly packed (no byteStride)");
        }
        auto buffers = root.find("buffers");
        if (buffer != 0 || buffers == root.end() || !buffers->is_array() || buffers->empty() || !(*buffers)[0].is_object() ||
            (*buffers)[0].find("uri") != (*buffers)[0].end()){
            return glbFail(error, what + ": bufferView " + std::to_string(index) + " must be in the GLB's own buffer (buffer 0, no uri)");
        }
        if (binLength == 0 && byteLength > 0) return glbFail(error, what + ": the file has no BIN chunk");
        if (byteOffset > binLength || byteLength > binLength - byteOffset){
            return glbFail(error, what + ": bufferView " + std::to_string(index) + " (" + std::to_string(byteOffset) + " + " +
                           std::to_string(byteLength) + " bytes) is past the BIN chunk (" + std::to_string(binLength) + " bytes)");
        }
        offset = binStart + (size_t)byteOffset;
        length = (size_t)byteLength;
        return true;
    }

}

bool vat::readVatGlb(const std::vector<uint8_t>& glb, VatGlbContents& out, std::string* error){
    out = VatGlbContents();
    const uint32_t MAGIC = 0x46546C67;         // "glTF"
    const uint32_t CHUNK_JSON = 0x4E4F534A;    // "JSON"
    const uint32_t CHUNK_BIN = 0x004E4942;     // "BIN\0"
    if (glb.size() < 12 || readU32(glb, 0) != MAGIC) return glbFail(error, "not a binary glTF (.glb)");
    if (readU32(glb, 4) != 2) return glbFail(error, "glTF container version " + std::to_string(readU32(glb, 4)) + ", expected 2");
    uint64_t total = readU32(glb, 8);
    if (total != glb.size()){
        return glbFail(error, "the header says " + std::to_string(total) + " bytes, the file has " + std::to_string(glb.size()));
    }

    size_t jsonStart = 0;
    size_t jsonLength = 0;
    size_t binStart = 0;
    size_t binLength = 0;
    bool hasJson = false;
    size_t at = 12;
    while (at < glb.size()){
        if (glb.size() - at < 8) return glbFail(error, "truncated chunk header at byte " + std::to_string(at));
        uint64_t length = readU32(glb, at);
        uint32_t type = readU32(glb, at + 4);
        if (length > glb.size() - at - 8){
            return glbFail(error, "chunk at byte " + std::to_string(at) + " (" + std::to_string(length) + " bytes) is past the end of the file");
        }
        if (!hasJson){
            if (type != CHUNK_JSON) return glbFail(error, "the first chunk is not JSON");
            jsonStart = at + 8;
            jsonLength = (size_t)length;
            hasJson = true;
        }else if (type == CHUNK_BIN && binStart == 0){
            binStart = at + 8;
            binLength = (size_t)length;
        }
        at += 8 + (size_t)length;
    }
    if (!hasJson) return glbFail(error, "no JSON chunk");

    nlohmann::json root = nlohmann::json::parse(glb.begin() + (std::ptrdiff_t)jsonStart,
                                                glb.begin() + (std::ptrdiff_t)(jsonStart + jsonLength), nullptr, false);
    if (root.is_discarded() || !root.is_object()) return glbFail(error, "the JSON chunk is not a JSON object");

    auto extensions = root.find("extensions");
    if (extensions == root.end() || !extensions->is_object() || extensions->find(VAT_GLTF_EXTENSION) == extensions->end()){
        return glbFail(error, std::string("no ") + VAT_GLTF_EXTENSION + " extension (not a baked vertex animation mesh): " + REBAKE);
    }
    const nlohmann::json& extension = (*extensions)[VAT_GLTF_EXTENSION];
    if (!extension.is_object()) return glbFail(error, std::string(VAT_GLTF_EXTENSION) + " is not an object");
    uint64_t version = 0;
    if (!jsonCount(extension, "version", version, true) || version < 1){
        return glbFail(error, std::string(VAT_GLTF_EXTENSION) + ".version: expected a whole number from 1");
    }
    if (version > (uint64_t)VAT_VERSION){
        return glbFail(error, std::string(VAT_GLTF_EXTENSION) + " version " + std::to_string(version) +
                       " (this engine reads versions up to " + std::to_string(VAT_VERSION) + ")");
    }
    auto hash = extension.find("layoutHash");
    if (hash == extension.end() || !hash->is_string() || hash->get<std::string>().empty()){
        return glbFail(error, std::string(VAT_GLTF_EXTENSION) + ".layoutHash: expected the layout hash");
    }
    out.layoutHash = hash->get<std::string>();

    auto payload = extension.find("payload");
    if (payload == extension.end() || !payload->is_object()) return glbFail(error, std::string(VAT_GLTF_EXTENSION) + ".payload: missing");
    uint64_t view = 0;
    uint64_t width = 0;
    uint64_t height = 0;
    auto format = payload->find("format");
    if (!jsonCount(*payload, "bufferView", view, true) || !jsonCount(*payload, "width", width, true) ||
        !jsonCount(*payload, "height", height, true) || width < 1 || height < 1 || width > 65536 || height > 65536 ||
        format == payload->end() || !format->is_string() || !precisionFromName(format->get<std::string>(), out.payload.precision)){
        return glbFail(error, std::string(VAT_GLTF_EXTENSION) + ".payload: expected {bufferView, format: half|float, width >= 1, height >= 1}");
    }
    size_t offset = 0;
    size_t length = 0;
    if (!bufferViewSpan(root, view, binStart, binLength, offset, length, error, "payload")) return false;
    uint64_t expected = width * height * texelBytes(out.payload.precision);
    if ((uint64_t)length != expected){
        return glbFail(error, "payload: " + std::to_string(length) + " bytes, " + std::to_string(width) + " x " +
                       std::to_string(height) + " " + precisionName(out.payload.precision) + " texels are " + std::to_string(expected));
    }
    out.payload.width = (int)width;
    out.payload.height = (int)height;
    out.payload.bytes.assign(glb.begin() + (std::ptrdiff_t)offset, glb.begin() + (std::ptrdiff_t)(offset + length));

    auto sockets = extension.find("sockets");
    if (sockets != extension.end()){
        uint64_t socketView = 0;
        uint64_t count = 0;
        uint64_t frames = 0;
        if (!sockets->is_object() || !jsonCount(*sockets, "bufferView", socketView, true) ||
            !jsonCount(*sockets, "count", count, true) || !jsonCount(*sockets, "frames", frames, true) ||
            count > (uint64_t)INT_MAX || frames > (uint64_t)INT_MAX){
            return glbFail(error, std::string(VAT_GLTF_EXTENSION) + ".sockets: expected {bufferView, count, frames}");
        }
        if (!bufferViewSpan(root, socketView, binStart, binLength, offset, length, error, "sockets")) return false;
        if (count != 0 && frames > UINT64_MAX / 28 / count){
            return glbFail(error, "sockets: count x frames is too large");
        }
        uint64_t socketBytes = count * frames * 7 * sizeof(float);
        if ((uint64_t)length != socketBytes){
            return glbFail(error, "sockets: " + std::to_string(length) + " bytes, " + std::to_string(count) + " sockets x " +
                           std::to_string(frames) + " frames x 7 floats are " + std::to_string(socketBytes));
        }
        out.sockets.sockets = (int)count;
        out.sockets.frames = (int)frames;
        out.sockets.data.resize((size_t)(count * frames * 7));
        if (length > 0) std::memcpy(out.sockets.data.data(), glb.data() + offset, length);
    }
    return true;
}
