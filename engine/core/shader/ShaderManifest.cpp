// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderManifest.h"

#include "Log.h"
#include "io/Data.h"
#include "io/FileData.h"
#include "shader/ShaderData.h"
#include "shader/ShaderUniforms.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/Color.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>

// manifest lines of the library shaders (engine/shaderlib/library), always embedded
#include "shaderlib_manifests.h"

using namespace jasperengine;

namespace {
    // a feature= condition ("<DEFINE>", or "!<DEFINE>" with negate) against enabled defines
    bool conditionHolds(const std::string& define, bool negate, const std::vector<std::string>& enabled){
        const bool on = std::find(enabled.begin(), enabled.end(), define) != enabled.end();
        return on != negate;
    }

    struct Token{
        std::string text;
        bool quoted = false;
    };

    std::string trim(const std::string& value){
        const size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return "";
        const size_t last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::string lower(std::string value){
        for (char& c : value)
            c = (char)std::tolower((unsigned char)c);
        return value;
    }

    // Whitespace separated; a double-quoted string and a parenthesised group are one token
    bool tokenize(const std::string& text, std::vector<Token>& tokens, std::string& error){
        size_t i = 0;
        while (i < text.size()){
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
                i++;
            if (i >= text.size())
                break;

            Token token;
            if (text[i] == '"'){
                const size_t close = text.find('"', i + 1);
                if (close == std::string::npos){
                    error = "unterminated string";
                    return false;
                }
                token.text = text.substr(i + 1, close - i - 1);
                token.quoted = true;
                i = close + 1;
            }else{
                int depth = 0;
                const size_t start = i;
                while (i < text.size() && (depth > 0 || (text[i] != ' ' && text[i] != '\t'))){
                    if (text[i] == '(')
                        depth++;
                    else if (text[i] == ')')
                        depth--;
                    i++;
                }
                if (depth != 0){
                    error = "unbalanced parentheses";
                    return false;
                }
                token.text = text.substr(start, i - start);
            }
            tokens.push_back(token);
        }
        return true;
    }

    bool parseNumber(const std::string& text, float& value){
        const std::string value_ = trim(text);
        if (value_.empty())
            return false;
        char* end = nullptr;
        value = std::strtof(value_.c_str(), &end);
        return end && *end == '\0';
    }

    // "1.5", "(1, 2, 3)" or "1,2"
    bool parseNumbers(const std::string& text, std::vector<float>& values){
        std::string inner = trim(text);
        if (!inner.empty() && inner.front() == '('){
            if (inner.back() != ')')
                return false;
            inner = inner.substr(1, inner.size() - 2);
        }
        values.clear();
        size_t start = 0;
        while (start <= inner.size()){
            size_t comma = inner.find(',', start);
            if (comma == std::string::npos)
                comma = inner.size();
            float value;
            if (!parseNumber(inner.substr(start, comma - start), value))
                return false;
            values.push_back(value);
            start = comma + 1;
        }
        return !values.empty();
    }

    bool isIdentifier(const std::string& name){
        if (name.empty() || std::isdigit((unsigned char)name[0]))
            return false;
        for (char c : name){
            if (!std::isalnum((unsigned char)c) && c != '_')
                return false;
        }
        return true;
    }

    // a stencil reference or mask: 0..255, decimal or 0x hexadecimal
    bool parseStencilByte(const std::string& text, int& value){
        if (text.empty() || text.size() > 4)
            return false;
        int base = 10;
        size_t start = 0;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')){
            base = 16;
            start = 2;
        }
        int result = 0;
        for (size_t i = start; i < text.size(); i++){
            const char c = (char)std::tolower((unsigned char)text[i]);
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
            if (digit < 0)
                return false;
            result = result * base + digit;
        }
        if (result > 255)
            return false;
        value = result;
        return true;
    }

    // "key=value" tokens after the positional ones
    bool splitOption(const Token& token, std::string& key, std::string& value){
        if (token.quoted)
            return false;
        const size_t eq = token.text.find('=');
        if (eq == std::string::npos)
            return false;
        key = lower(token.text.substr(0, eq));
        value = token.text.substr(eq + 1);
        return true;
    }

    bool parseShaderTypeName(const std::string& token, ShaderType& type){
        const std::string name = lower(token);
        if (name == "mesh")   { type = ShaderType::MESH; return true; }
        if (name == "ui")     { type = ShaderType::UI; return true; }
        if (name == "points") { type = ShaderType::POINTS; return true; }
        if (name == "lines")  { type = ShaderType::LINES; return true; }
        if (name == "particles") { type = ShaderType::PARTICLES; return true; }
        if (name == "sky")    { type = ShaderType::SKYBOX; return true; }
        // a user post-process pass (PostProcessPass, phase 4): fullscreen.vert and the frag
        if (name == "postprocess") { type = ShaderType::POSTPROCESS; return true; }
        return false;
    }

    bool parseLightingMode(const std::string& token, ShaderLightingMode& mode){
        const std::string name = lower(token);
        if (name == "none")   { mode = ShaderLightingMode::NONE; return true; }
        if (name == "pixel")  { mode = ShaderLightingMode::PIXEL; return true; }
        if (name == "vertex") { mode = ShaderLightingMode::VERTEX; return true; }
        if (name == "mixed")  { mode = ShaderLightingMode::MIXED; return true; }
        return false;
    }

    bool parseQueue(const std::string& token, ShaderQueue& queue){
        const std::string name = lower(token);
        if (name == "geometry")    { queue = ShaderQueue::GEOMETRY; return true; }
        if (name == "alphatest")   { queue = ShaderQueue::ALPHATEST; return true; }
        if (name == "transparent") { queue = ShaderQueue::TRANSPARENT; return true; }
        return false;
    }

    bool parsePropertyKind(const std::string& token, ShaderPropertyKind& kind, float& min, float& max){
        const std::string name = lower(token);
        if (name == "float")    { kind = ShaderPropertyKind::FLOAT; return true; }
        if (name == "int")      { kind = ShaderPropertyKind::INT; return true; }
        if (name == "toggle")   { kind = ShaderPropertyKind::TOGGLE; return true; }
        if (name == "color")    { kind = ShaderPropertyKind::COLOR; return true; }
        if (name == "hdrcolor") { kind = ShaderPropertyKind::HDRCOLOR; return true; }
        if (name == "vector2")  { kind = ShaderPropertyKind::VECTOR2; return true; }
        if (name == "vector3")  { kind = ShaderPropertyKind::VECTOR3; return true; }
        if (name == "vector4")  { kind = ShaderPropertyKind::VECTOR4; return true; }
        if (name.compare(0, 5, "range") == 0){
            std::vector<float> bounds;
            if (!parseNumbers(name.substr(5), bounds) || bounds.size() != 2 || name.size() < 7 || name[5] != '(')
                return false;
            kind = ShaderPropertyKind::RANGE;
            min = bounds[0];
            max = bounds[1];
            return true;
        }
        return false;
    }

    // components a kind takes as its default
    size_t kindComponents(ShaderPropertyKind kind){
        switch (kind){
            case ShaderPropertyKind::VECTOR2:  return 2;
            case ShaderPropertyKind::VECTOR3:  return 3;
            case ShaderPropertyKind::VECTOR4:
            case ShaderPropertyKind::COLOR:
            case ShaderPropertyKind::HDRCOLOR: return 4;
            default:                           return 1;
        }
    }

    bool isColorKind(ShaderPropertyKind kind){
        return kind == ShaderPropertyKind::COLOR || kind == ShaderPropertyKind::HDRCOLOR;
    }

    // the line of the manifest region: blank, a line comment, #version or #extension
    bool isHeaderLine(const std::string& line){
        const std::string text = trim(line);
        return text.empty() || text.compare(0, 2, "//") == 0 ||
            text.compare(0, 8, "#version") == 0 || text.compare(0, 10, "#extension") == 0;
    }

    // the "@directive rest" of a manifest line, false for any other line
    bool directiveOf(const std::string& line, std::string& directive, std::string& rest){
        std::string text = trim(line);
        if (text.compare(0, 2, "//") != 0)
            return false;
        text = trim(text.substr(2));
        if (text.empty() || text[0] != '@')
            return false;
        size_t end = 1;
        while (end < text.size() && (std::isalnum((unsigned char)text[end]) || text[end] == '_'))
            end++;
        directive = text.substr(1, end - 1);
        rest = trim(text.substr(end));
        return !directive.empty();
    }

    const std::unordered_map<std::string, std::string>& librarySources(){
        static const std::unordered_map<std::string, std::string>* sources = [](){
            auto* map = new std::unordered_map<std::string, std::string>();
            for (size_t i = 0; i < shaderlib_manifests::fileCount; i++){
                map->emplace(shaderlib_manifests::files[i].name, shaderlib_manifests::files[i].source);
            }
            return map;
        }();
        return *sources;
    }

    bool readAssetFile(const std::string& path, std::string& content){
        if (!ShaderVariantCompiler::isSafeRelativePath(path))
            return false;
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK)
            return false;
        content = data.readString();
        return true;
    }

    struct ManifestCache{
        std::mutex mutex;
        std::map<std::string, std::shared_ptr<const ShaderManifest>> manifests;
        std::set<std::string> reported; // missing sources already logged
        ShaderManifests::SourceReader forkReader = readAssetFile;
    };

    ManifestCache& cache(){
        static ManifestCache* c = new ManifestCache();
        return *c;
    }

    void logDiagnostics(const ShaderManifest& manifest){
        for (const ShaderManifestDiagnostic& d : manifest.diagnostics){
            if (d.error){
                Log::error("%s:%i: shader manifest: %s", d.file.c_str(), d.line, d.message.c_str());
            }else{
                Log::warn("%s:%i: shader manifest: %s", d.file.c_str(), d.line, d.message.c_str());
            }
        }
    }

}

// ---------------------------------------------------------------- ShaderManifest

void ShaderManifest::parse(const std::string& source, const std::string& file){
    auto report = [&](int line, bool error, const std::string& message){
        diagnostics.push_back({file, line, error, message});
    };

    int lineNumber = 0;
    size_t lineStart = 0;
    while (lineStart <= source.size()){
        size_t lineEnd = source.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = source.size();
        const std::string line = source.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        lineNumber++;

        if (!isHeaderLine(line))
            break;

        std::string directive;
        std::string rest;
        if (!directiveOf(line, directive, rest))
            continue;

        // compiler annotations (ShaderVariantCompiler), not manifest directives
        if (directive == "image_sample_type" || directive == "sampler_type")
            continue;

        present = true;

        std::vector<Token> args;
        std::string tokenError;
        if (!tokenize(rest, args, tokenError)){
            report(lineNumber, true, "@" + directive + ": " + tokenError);
            continue;
        }

        if (directive == "shader"){
            ShaderType parsed;
            if (args.size() != 1 || !parseShaderTypeName(args[0].text, parsed)){
                report(lineNumber, true, "@shader takes one of mesh, ui, points, lines, sky, postprocess");
                continue;
            }
            if (hasType && parsed != type){
                report(lineNumber, true, "@shader given twice with different types");
                continue;
            }
            hasType = true;
            type = parsed;

        }else if (directive == "name" || directive == "category" || directive == "description"){
            if (args.size() != 1){
                report(lineNumber, true, "@" + directive + " takes one double-quoted string");
                continue;
            }
            if (directive == "name") name = args[0].text;
            else if (directive == "category") category = args[0].text;
            else description = args[0].text;

        }else if (directive == "lighting"){
            if (args.empty()){
                report(lineNumber, true, "@lighting needs at least one of none, pixel, vertex, mixed");
                continue;
            }
            lighting.clear();
            for (const Token& arg : args){
                ShaderLightingMode mode;
                if (!parseLightingMode(arg.text, mode)){
                    report(lineNumber, true, "unknown lighting mode '" + arg.text + "' (none, pixel, vertex, mixed)");
                    continue;
                }
                if (std::find(lighting.begin(), lighting.end(), mode) == lighting.end())
                    lighting.push_back(mode);
            }

        }else if (directive == "require"){
            // "feature=<DEFINE>" (or "feature=!<DEFINE>") makes the line's inputs conditional
            ShaderManifestRequireRule rule;
            rule.line = lineNumber;
            bool conditional = false;
            bool valid = true;
            for (const Token& arg : args){
                std::string key, value;
                if (splitOption(arg, key, value)){
                    if (key != "feature" || conditional){
                        report(lineNumber, true, "@require takes <inputs...> [feature=<DEFINE>], got '" + arg.text + "'");
                        valid = false;
                        continue;
                    }
                    conditional = true;
                    rule.negate = !value.empty() && value[0] == '!';
                    rule.define = rule.negate ? value.substr(1) : value;
                    if (!ShaderReference::isValidDefine(rule.define)){
                        report(lineNumber, true, "@require feature= needs a feature or keyword define, got '" + value + "'");
                        valid = false;
                    }
                    continue;
                }
                const std::string input = lower(arg.text);
                if (input == "normals") rule.require |= ShaderRequire_Normals;
                else if (input == "tangents") rule.require |= ShaderRequire_Tangents;
                else if (input == "uv1") rule.require |= ShaderRequire_UV1;
                else if (input == "uv2") rule.require |= ShaderRequire_UV2;
                else if (input == "color") rule.require |= ShaderRequire_Color;
                else if (input == "position") rule.require |= ShaderRequire_Position;
                else if (input == "rawcolor") rule.require |= ShaderRequire_RawColor;
                else if (input == "smoothnormals") rule.require |= ShaderRequire_SmoothNormals;
                else if (input == "scenedepth") rule.require |= ShaderRequire_SceneDepth;
                else if (input == "scenecolor") rule.require |= ShaderRequire_SceneColor;
                else if (input == "environment") rule.require |= ShaderRequire_Environment;
                else if (input == "normalmap") rule.require |= ShaderRequire_NormalMap;
                else if (input == "planarreflection") rule.require |= ShaderRequire_PlanarReflection;
                // the world position the volumes and the map are tested at comes with them
                else if (input == "waterexclusion") rule.require |= ShaderRequire_WaterExclusion | ShaderRequire_Position;
                else if (input == "waterwake") rule.require |= ShaderRequire_WaterWake | ShaderRequire_Position;
                else report(lineNumber, true, "unknown input '" + arg.text + "' (normals, tangents, uv1, uv2, color, rawcolor, position, smoothnormals, scenedepth, scenecolor, environment, normalmap, planarreflection, waterexclusion, waterwake)");
            }
            if (!valid)
                continue;
            if (conditional){
                requireRules.push_back(rule);
            }else{
                require |= rule.require;
            }

        }else if (directive == "state" || directive == "queue"){
            if (directive == "queue"){
                if (args.size() != 1 || !parseQueue(args[0].text, state.queue))
                    report(lineNumber, true, "@queue takes geometry, alphatest or transparent");
                continue;
            }
            // "@state[<DEFINE>] ...": a rule over the base state while the define is on
            ShaderManifestState* target = &state;
            ShaderManifestStateRule rule;
            if (!args.empty() && !args[0].quoted && !args[0].text.empty() && args[0].text[0] == '['){
                const std::string& condition = args[0].text;
                const std::string define = (condition.size() > 2 && condition.back() == ']') ? condition.substr(1, condition.size() - 2) : "";
                rule.negate = !define.empty() && define[0] == '!';
                rule.define = rule.negate ? define.substr(1) : define;
                rule.line = lineNumber;
                if (!ShaderReference::isValidDefine(rule.define)){
                    report(lineNumber, true, "@state[<DEFINE>] needs a feature or keyword define, got '" + condition + "'");
                    continue;
                }
                args.erase(args.begin());
                target = &rule.state;
            }
            ShaderManifestState& state = *target;
            uint8_t lineStencil = 0; // the stencil keys of this line
            for (const Token& arg : args){
                std::string key, value;
                if (!splitOption(arg, key, value)){
                    report(lineNumber, true, "@state takes key=value pairs, got '" + arg.text + "'");
                    continue;
                }
                const std::string v = lower(value);
                bool valid = true;
                if (key == "blend"){
                    if (v == "opaque") state.blend = MaterialBlendMode::NONE;
                    else if (v == "alpha") state.blend = MaterialBlendMode::ALPHA;
                    else if (v == "premultiplied") state.blend = MaterialBlendMode::PREMULTIPLIED;
                    else if (v == "additive") state.blend = MaterialBlendMode::ADDITIVE;
                    else if (v == "multiply") state.blend = MaterialBlendMode::MULTIPLY;
                    else valid = false;
                }else if (key == "cull"){
                    if (v == "back") state.cull = MaterialCullMode::BACK;
                    else if (v == "front") state.cull = MaterialCullMode::FRONT;
                    else if (v == "off") state.cull = MaterialCullMode::OFF;
                    else valid = false;
                }else if (key == "zwrite"){
                    if (v == "on") state.depthWrite = MaterialDepthWrite::ENABLED;
                    else if (v == "off") state.depthWrite = MaterialDepthWrite::DISABLED;
                    else valid = false;
                }else if (key == "ztest"){
                    if (v == "less") state.depthTest = MaterialDepthTest::LESS;
                    else if (v == "lequal") state.depthTest = MaterialDepthTest::LESS_EQUAL;
                    else if (v == "equal") state.depthTest = MaterialDepthTest::EQUAL;
                    else if (v == "gequal") state.depthTest = MaterialDepthTest::GREATER_EQUAL;
                    else if (v == "greater") state.depthTest = MaterialDepthTest::GREATER;
                    else if (v == "notequal") state.depthTest = MaterialDepthTest::NOT_EQUAL;
                    else if (v == "always") state.depthTest = MaterialDepthTest::ALWAYS;
                    else valid = false;
                }else if (key == "offset"){
                    std::vector<float> values;
                    if (parseNumbers(value, values) && values.size() == 2){
                        state.hasDepthOffset = true;
                        state.depthOffsetFactor = values[0];
                        state.depthOffsetUnits = values[1];
                    }else{
                        valid = false;
                    }
                }else if (key == "queue"){
                    valid = parseQueue(value, state.queue);
                }else if (key == "shadows"){
                    if (v == "on") state.shadows = ShaderShadowCasting::ON;
                    else if (v == "off") state.shadows = ShaderShadowCasting::OFF;
                    else valid = false;
                }else if (key == "stencil"){
                    valid = parseStencilCompare(v, state.stencil.compare);
                    if (valid) lineStencil |= ShaderStencil_Compare;
                }else if (key == "ref" || key == "readmask" || key == "writemask"){
                    int number = 0;
                    valid = parseStencilByte(v, number);
                    if (!valid){
                        report(lineNumber, true, "@state " + key + "= takes an integer 0..255 (or 0x00..0xFF), got '" + value + "'");
                        continue;
                    }
                    if (key == "ref"){
                        state.stencil.ref = number;
                        lineStencil |= ShaderStencil_Ref;
                    }else if (key == "readmask"){
                        state.stencil.readMask = number;
                        lineStencil |= ShaderStencil_ReadMask;
                    }else{
                        state.stencil.writeMask = number;
                        lineStencil |= ShaderStencil_WriteMask;
                    }
                }else if (key == "pass" || key == "fail" || key == "zfail"){
                    MaterialStencilOp op;
                    valid = parseStencilOp(v, op);
                    if (!valid){
                        report(lineNumber, true, "@state " + key + "= takes keep, zero, replace, incr, decr, invert, incrwrap or decrwrap, got '" + value + "'");
                        continue;
                    }
                    if (key == "pass"){
                        state.stencil.pass = op;
                        lineStencil |= ShaderStencil_Pass;
                    }else if (key == "fail"){
                        state.stencil.fail = op;
                        lineStencil |= ShaderStencil_Fail;
                    }else{
                        state.stencil.depthFail = op;
                        lineStencil |= ShaderStencil_DepthFail;
                    }
                }else if (key == "colorwrite"){
                    const int mask = parseColorWrite(v);
                    valid = mask >= 0;
                    if (!valid){
                        report(lineNumber, true, "@state colorwrite= takes none or channels of rgba (rgba, rgb, a...), got '" + value + "'");
                        continue;
                    }
                    state.colorWrite = mask;
                }else if (key == "priority"){
                    // a signed integer, the draw order offset within the queue
                    char* end = nullptr;
                    const long number = std::strtol(value.c_str(), &end, 10);
                    valid = !value.empty() && end && *end == '\0' && number >= -1000000 && number <= 1000000;
                    if (!valid){
                        report(lineNumber, true, "@state priority= takes an integer (added to the sort priority: lower draws first), got '" + value + "'");
                        continue;
                    }
                    state.hasPriority = true;
                    state.priority = (int)number;
                }else{
                    report(lineNumber, true, "unknown @state key '" + key + "' (blend, cull, zwrite, ztest, offset, queue, shadows, "
                                             "stencil, ref, readmask, writemask, pass, fail, zfail, colorwrite, priority)");
                    continue;
                }
                if (!valid){
                    if (key == "stencil")
                        report(lineNumber, true, "@state stencil= takes off, never, less, equal, lequal, greater, notequal, gequal or always, got '" + value + "'");
                    else
                        report(lineNumber, true, "invalid @state value '" + arg.text + "'");
                }
            }
            // stencil values without a test: the line or the base @state before it names one
            state.stencilSet |= lineStencil;
            if ((lineStencil & ~ShaderStencil_Compare) && !(lineStencil & ShaderStencil_Compare) &&
                    !(this->state.stencilSet & ShaderStencil_Compare)){
                report(lineNumber, true, "@state ref/readmask/writemask/pass/fail/zfail need stencil=<compare> on the line or in the base @state before it");
            }
            if (target != &this->state)
                stateRules.push_back(rule);

        }else if (directive == "property"){
            if (args.size() < 3){
                report(lineNumber, true, "@property takes <member> <kind> <default> [\"label\"] [feature=F]");
                continue;
            }
            ShaderManifestProperty property;
            property.line = lineNumber;
            property.member = args[0].text;
            if (args[0].quoted || !isIdentifier(property.member)){
                report(lineNumber, true, "invalid @property member name '" + property.member + "'");
                continue;
            }
            if (property.member == "time" || property.member == "gameTime" || property.member == "resolution"){
                report(lineNumber, true, "'" + property.member + "' is written by the engine and cannot be a @property");
                continue;
            }
            if (findProperty(property.member)){
                report(lineNumber, true, "@property '" + property.member + "' declared twice");
                continue;
            }
            if (!parsePropertyKind(args[1].text, property.kind, property.min, property.max)){
                report(lineNumber, true, "unknown @property kind '" + args[1].text +
                    "' (float, range(min, max), int, toggle, color, hdrcolor, vector2, vector3, vector4)");
                continue;
            }
            std::vector<float> values;
            if (args[2].quoted || !parseNumbers(args[2].text, values)){
                report(lineNumber, true, "invalid @property default '" + args[2].text + "' (a number or a (x, y, ...) tuple)");
                continue;
            }
            const size_t wanted = kindComponents(property.kind);
            if (values.size() == 1 && wanted > 1)
                values.assign(wanted, values[0]);
            if (isColorKind(property.kind) && values.size() == 3)
                values.push_back(1.0f);
            if (values.size() != wanted){
                report(lineNumber, true, "@property '" + property.member + "' default needs " + std::to_string(wanted) + " components");
                continue;
            }
            float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (size_t c = 0; c < values.size() && c < 4; c++)
                v[c] = values[c];
            property.defaultValue = Vector4(v[0], v[1], v[2], v[3]);
            if (property.kind == ShaderPropertyKind::TOGGLE)
                property.defaultValue.x = property.defaultValue.x != 0.0f ? 1.0f : 0.0f;
            if (property.kind == ShaderPropertyKind::RANGE && property.min > property.max)
                report(lineNumber, true, "@property '" + property.member + "' range minimum is above its maximum");
            // colours are written as edited (sRGB), stored linear
            if (isColorKind(property.kind))
                property.defaultValue = Color::sRGBToLinear(property.defaultValue);

            bool valid = true;
            for (size_t a = 3; a < args.size(); a++){
                std::string key, value;
                if (splitOption(args[a], key, value) && key == "feature"){
                    property.feature = value;
                }else if (args[a].quoted && property.label.empty()){
                    property.label = args[a].text;
                }else{
                    report(lineNumber, true, "unexpected @property argument '" + args[a].text + "'");
                    valid = false;
                }
            }
            if (valid)
                properties.push_back(property);

        }else if (directive == "texture"){
            if (args.size() < 2){
                report(lineNumber, true, "@texture takes <uniform> <default> [\"label\"] [cube] [feature=F] [default[F]=<default>]");
                continue;
            }
            auto parseDefault = [](const std::string& name, ShaderTextureDefault& value){
                const std::string def = lower(name);
                if (def == "white") value = ShaderTextureDefault::WHITE;
                else if (def == "black") value = ShaderTextureDefault::BLACK;
                else if (def == "gray" || def == "grey") value = ShaderTextureDefault::GRAY;
                else if (def == "normal") value = ShaderTextureDefault::NORMAL;
                else if (def == "ripples") value = ShaderTextureDefault::RIPPLES;
                else if (def == "noise") value = ShaderTextureDefault::NOISE;
                else if (def == "particle") value = ShaderTextureDefault::PARTICLE;
                else return false;
                return true;
            };
            ShaderManifestTexture texture;
            texture.line = lineNumber;
            texture.uniform = args[0].text;
            if (args[0].quoted || !isIdentifier(texture.uniform)){
                report(lineNumber, true, "invalid @texture uniform name '" + texture.uniform + "'");
                continue;
            }
            if (findTexture(texture.uniform)){
                report(lineNumber, true, "@texture '" + texture.uniform + "' declared twice");
                continue;
            }
            if (!parseDefault(args[1].text, texture.defaultTexture)){
                report(lineNumber, true, "unknown @texture default '" + args[1].text + "' (white, black, gray, normal, ripples, noise, particle)");
                continue;
            }
            bool valid = true;
            for (size_t a = 2; a < args.size(); a++){
                std::string key, value;
                // default[<DEFINE>]=<default>, the define as written
                const std::string& text = args[a].text;
                const size_t close = text.find("]=");
                if (!args[a].quoted && lower(text.substr(0, 8)) == "default[" && close != std::string::npos && close > 8){
                    texture.featureDefaultDefine = text.substr(8, close - 8);
                    if (!parseDefault(text.substr(close + 2), texture.featureDefaultTexture)){
                        report(lineNumber, true, "unknown @texture default '" + text.substr(close + 2) + "' (white, black, gray, normal, ripples, noise, particle)");
                        valid = false;
                    }
                }else if (splitOption(args[a], key, value) && key == "feature"){
                    texture.feature = value;
                }else if (!args[a].quoted && lower(args[a].text) == "cube"){
                    texture.cube = true;
                }else if (args[a].quoted && texture.label.empty()){
                    texture.label = args[a].text;
                }else{
                    report(lineNumber, true, "unexpected @texture argument '" + args[a].text + "'");
                    valid = false;
                }
            }
            if (valid)
                textures.push_back(texture);

        }else if (directive == "palette"){
            if (args.empty() || args[0].quoted || !isIdentifier(args[0].text)){
                report(lineNumber, true, "@palette takes <uniform> [\"label\"] [feature=A,B]");
                continue;
            }
            ShaderManifestPalette palette;
            palette.line = lineNumber;
            palette.uniform = args[0].text;
            if (findPalette(palette.uniform) || findTexture(palette.uniform)){
                report(lineNumber, true, "@palette '" + palette.uniform + "' declared twice");
                continue;
            }
            bool valid = true;
            for (size_t a = 1; a < args.size(); a++){
                std::string key, value;
                if (splitOption(args[a], key, value) && key == "feature"){
                    size_t start = 0;
                    while (start <= value.size()){
                        const size_t comma = value.find(',', start);
                        const std::string define = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                        if (!define.empty())
                            palette.features.push_back(define);
                        if (comma == std::string::npos)
                            break;
                        start = comma + 1;
                    }
                }else if (args[a].quoted && palette.label.empty()){
                    palette.label = args[a].text;
                }else{
                    report(lineNumber, true, "unexpected @palette argument '" + args[a].text + "'");
                    valid = false;
                }
            }
            if (valid)
                palettes.push_back(palette);

        }else if (directive == "feature"){
            if (args.empty() || args[0].quoted || !ShaderReference::isValidDefine(args[0].text)){
                report(lineNumber, true, "@feature takes an upper-case <DEFINE> [\"label\"] [default=on]");
                continue;
            }
            ShaderManifestFeature feature;
            feature.line = lineNumber;
            feature.define = args[0].text;
            if (findFeature(feature.define) || findKeywordGroup(feature.define)){
                report(lineNumber, true, "feature '" + feature.define + "' declared twice");
                continue;
            }
            bool valid = true;
            for (size_t a = 1; a < args.size(); a++){
                std::string key, value;
                if (splitOption(args[a], key, value) && key == "default"){
                    const std::string v = lower(value);
                    if (v == "on") feature.defaultOn = true;
                    else if (v == "off") feature.defaultOn = false;
                    else { report(lineNumber, true, "@feature default must be on or off"); valid = false; }
                }else if (args[a].quoted && feature.label.empty()){
                    feature.label = args[a].text;
                }else{
                    report(lineNumber, true, "unexpected @feature argument '" + args[a].text + "'");
                    valid = false;
                }
            }
            if (valid)
                features.push_back(feature);

        }else if (directive == "keywords"){
            if (args.empty() || args[0].quoted || !ShaderReference::isValidDefine(args[0].text)){
                report(lineNumber, true, "@keywords takes an upper-case <GROUP> [\"label\"] <A> <B> ...");
                continue;
            }
            ShaderManifestKeywords group;
            group.line = lineNumber;
            group.group = args[0].text;
            bool valid = true;
            for (size_t a = 1; a < args.size(); a++){
                if (args[a].quoted){
                    if (group.label.empty() && group.values.empty()){
                        group.label = args[a].text;
                    }else{
                        report(lineNumber, true, "unexpected @keywords argument \"" + args[a].text + "\"");
                        valid = false;
                    }
                }else if (ShaderReference::isValidDefine(args[a].text)){
                    if (std::find(group.values.begin(), group.values.end(), args[a].text) != group.values.end()){
                        report(lineNumber, true, "@keywords value '" + args[a].text + "' given twice");
                        valid = false;
                    }
                    group.values.push_back(args[a].text);
                }else{
                    report(lineNumber, true, "invalid @keywords value '" + args[a].text + "' (upper case letters, digits, _)");
                    valid = false;
                }
            }
            if (group.values.size() < 2){
                report(lineNumber, true, "@keywords '" + group.group + "' needs at least two values");
                valid = false;
            }
            for (const ShaderManifestKeywords& other : keywords){
                if (other.group == group.group){
                    report(lineNumber, true, "@keywords group '" + group.group + "' declared twice");
                    valid = false;
                }
            }
            if (valid)
                keywords.push_back(group);

        }else if (directive == "depth"){
            if (!args.empty())
                report(lineNumber, true, "@depth takes no argument");
            depth = true;

        }else if (directive == "role"){
            const std::string value = args.size() == 1 && !args[0].quoted ? lower(args[0].text) : std::string();
            if (role != ShaderRole::NONE){
                report(lineNumber, true, "@role given twice");
            }else if (value == "water"){
                role = ShaderRole::WATER;
            }else if (value == "underwater"){
                role = ShaderRole::UNDERWATER;
            }else{
                report(lineNumber, true, "@role takes one of: water, underwater");
            }

        }else{
            report(lineNumber, false, "unknown directive @" + directive + " (ignored)");
        }
    }
}

void ShaderManifest::checkComplete(const std::string& file, bool library){
    auto report = [&](int line, const std::string& message){
        diagnostics.push_back({file, line, true, message});
    };

    if (library){
        if (!hasType)
            report(1, "a library shader needs @shader");
        if (name.empty())
            report(1, "a library shader needs @name");
    }

    // feature= on rows must name a declared feature (or a keyword define)
    auto knownFeature = [this](const std::string& condition){
        const std::string define = (!condition.empty() && condition[0] == '!') ? condition.substr(1) : condition;
        return findFeature(define) || findKeywordGroup(define);
    };
    for (const ShaderManifestProperty& property : properties){
        if (!property.feature.empty() && !knownFeature(property.feature))
            report(property.line, "@property '" + property.member + "' names undeclared feature '" + property.feature + "'");
    }
    for (const ShaderManifestTexture& texture : textures){
        if (!texture.feature.empty() && !knownFeature(texture.feature))
            report(texture.line, "@texture '" + texture.uniform + "' names undeclared feature '" + texture.feature + "'");
        if (!texture.featureDefaultDefine.empty() && !knownFeature(texture.featureDefaultDefine))
            report(texture.line, "@texture '" + texture.uniform + "' default[" + texture.featureDefaultDefine + "] names undeclared feature");
    }
    for (const ShaderManifestPalette& palette : palettes){
        for (const std::string& feature : palette.features){
            if (!knownFeature(feature))
                report(palette.line, "@palette '" + palette.uniform + "' names undeclared feature '" + feature + "'");
        }
    }
    for (const ShaderManifestStateRule& rule : stateRules){
        if (!knownFeature(rule.define))
            report(rule.line, "@state[" + rule.define + "] names undeclared feature '" + rule.define + "'");
    }
    for (const ShaderManifestRequireRule& rule : requireRules){
        if (!knownFeature(rule.define))
            report(rule.line, "@require feature=" + rule.define + " names undeclared feature '" + rule.define + "'");
    }
    // BLEND_* defines are the engine's (the variant's resolved blend mode)
    for (const ShaderManifestFeature& feature : features){
        if (ShaderReference::isBlendDefine(feature.define))
            report(feature.line, "feature '" + feature.define + "' is reserved: the engine defines it from the blend mode");
    }
    for (const ShaderManifestKeywords& group : keywords){
        for (const std::string& value : group.values){
            if (ShaderReference::isBlendDefine(group.group + "_" + value))
                report(group.line, "keyword '" + group.group + "_" + value + "' is reserved: the engine defines it from the blend mode");
        }
    }
    if (!lighting.empty() && hasType && type != ShaderType::MESH){
        for (ShaderLightingMode mode : lighting){
            if (mode != ShaderLightingMode::NONE){
                report(1, "@lighting other than none needs @shader mesh");
                break;
            }
        }
    }
    if (depth && hasType && type != ShaderType::MESH)
        report(1, "@depth needs @shader mesh");

    // scene inputs (phase 3): the engine binds them to mesh, points and particle draws only, by
    // these names, and produces them for what draws after the opaque queue
    const uint32_t sceneInputs = getPossibleSceneInputs();
    if (sceneInputs){
        int line = 1;
        for (const ShaderManifestRequireRule& rule : requireRules){
            if (rule.require & ShaderRequire_SceneInputs){
                line = rule.line;
                break;
            }
        }
        if (hasType && type != ShaderType::MESH && type != ShaderType::POINTS && type != ShaderType::PARTICLES)
            report(line, "@require scenedepth / scenecolor needs @shader mesh, points or particles");
    }
    for (const ShaderManifestTexture& texture : textures){
        if (texture.uniform == "u_sceneDepthTexture" || texture.uniform == "u_sceneColorTexture")
            report(texture.line, "@texture '" + texture.uniform + "' is reserved: the engine binds it for @require scenedepth / scenecolor");
        if (texture.uniform == "u_environmentTexture")
            report(texture.line, "@texture 'u_environmentTexture' is reserved: the engine binds it for @require environment");
    }
    // the environment is bound to mesh draws (RenderSystem::applyEnvironment)
    uint32_t possibleRequire = require;
    int environmentLine = 1;
    for (const ShaderManifestRequireRule& rule : requireRules){
        if ((rule.require & ShaderRequire_Environment) && !(possibleRequire & ShaderRequire_Environment))
            environmentLine = rule.line;
        possibleRequire |= rule.require;
    }
    if ((possibleRequire & ShaderRequire_Environment) && hasType && type != ShaderType::MESH)
        report(environmentLine, "@require environment needs @shader mesh");
    // the planar reflection is bound to mesh draws (RenderSystem::applyPlanarReflection)
    if ((possibleRequire & ShaderRequire_PlanarReflection) && hasType && type != ShaderType::MESH){
        int line = 1;
        for (const ShaderManifestRequireRule& rule : requireRules){
            if (rule.require & ShaderRequire_PlanarReflection){
                line = rule.line;
                break;
            }
        }
        report(line, "@require planarreflection needs @shader mesh");
    }
    // the excluders and the wake map are bound to mesh draws (RenderSystem::applyWaterInteraction)
    if ((possibleRequire & (ShaderRequire_WaterExclusion | ShaderRequire_WaterWake)) && hasType && type != ShaderType::MESH){
        int line = 1;
        for (const ShaderManifestRequireRule& rule : requireRules){
            if (rule.require & (ShaderRequire_WaterExclusion | ShaderRequire_WaterWake)){
                line = rule.line;
                break;
            }
        }
        report(line, "@require waterexclusion / waterwake needs @shader mesh");
    }
    if (role != ShaderRole::NONE && hasType && type != (role == ShaderRole::WATER ? ShaderType::MESH : ShaderType::POSTPROCESS))
        report(1, role == ShaderRole::WATER ? "@role water needs @shader mesh" : "@role underwater needs @shader postprocess");
    // the material's normal map is bound to mesh draws, by the name the 2D light path reads
    if (possibleRequire & ShaderRequire_NormalMap){
        // the first feature= line asking for it (1 when every variant does)
        int normalMapLine = 1;
        for (const ShaderManifestRequireRule& rule : requireRules){
            if (!(require & ShaderRequire_NormalMap) && (rule.require & ShaderRequire_NormalMap)){
                normalMapLine = rule.line;
                break;
            }
        }
        if (hasType && type != ShaderType::MESH)
            report(normalMapLine, "@require normalmap needs @shader mesh");
        for (const ShaderManifestTexture& texture : textures){
            if (texture.uniform == "u_normalTexture")
                report(texture.line, "@texture 'u_normalTexture' is reserved with @require normalmap: it is the material's Normal Texture");
        }
    }
    // a mesh variant reading the scene must draw after the opaque queue: its state has to put
    // it in the transparent queue (a material can still override it; the inspector and the
    // checks report that). Checked for the default features and for each feature= rule's
    // condition that adds a scene input.
    if (sceneInputs && (!hasType || type == ShaderType::MESH)){
        auto transparent = [](const ShaderManifestState& resolved){
            if (resolved.queue == ShaderQueue::TRANSPARENT)
                return true;
            if (resolved.queue != ShaderQueue::AUTO)
                return false;
            return resolved.blend != MaterialBlendMode::AUTO && resolved.blend != MaterialBlendMode::NONE;
        };
        const char* const fix = "needs the transparent queue: add @state queue=transparent or a blend (alpha, premultiplied, additive, multiply)";
        if ((require & ShaderRequire_SceneInputs) && !transparent(resolveState(resolveFeatures({}))))
            report(1, std::string("a shader requiring scenedepth / scenecolor ") + fix);
        for (const ShaderManifestRequireRule& rule : requireRules){
            if (!(rule.require & ShaderRequire_SceneInputs))
                continue;
            const std::vector<std::string> enabled = resolveFeatures({rule.negate ? "!" + rule.define : rule.define});
            if (conditionHolds(rule.define, rule.negate, enabled) && !transparent(resolveState(enabled)))
                report(rule.line, "@require scenedepth / scenecolor feature=" + std::string(rule.negate ? "!" : "") + rule.define + ": the variant " + fix);
        }
    }
}

bool ShaderManifest::hasErrors() const{
    for (const ShaderManifestDiagnostic& d : diagnostics){
        if (d.error)
            return true;
    }
    return false;
}

ShaderLightingMode ShaderManifest::getDefaultLighting() const{
    return lighting.empty() ? ShaderLightingMode::NONE : lighting.front();
}

bool ShaderManifest::supportsLighting(ShaderLightingMode mode) const{
    if (lighting.empty())
        return mode == ShaderLightingMode::NONE;
    return std::find(lighting.begin(), lighting.end(), mode) != lighting.end();
}

const ShaderManifestProperty* ShaderManifest::findProperty(const std::string& member) const{
    for (const ShaderManifestProperty& property : properties){
        if (property.member == member)
            return &property;
    }
    return nullptr;
}

ShaderTextureDefault ShaderManifestTexture::getDefault(const std::vector<std::string>& enabledFeatures) const{
    if (featureDefaultDefine.empty())
        return defaultTexture;
    // "!<DEFINE>": while it is off
    const bool negated = featureDefaultDefine[0] == '!';
    const std::string define = negated ? featureDefaultDefine.substr(1) : featureDefaultDefine;
    const bool on = std::find(enabledFeatures.begin(), enabledFeatures.end(), define) != enabledFeatures.end();
    return on != negated ? featureDefaultTexture : defaultTexture;
}

const ShaderManifestTexture* ShaderManifest::findTexture(const std::string& uniform) const{
    for (const ShaderManifestTexture& texture : textures){
        if (texture.uniform == uniform)
            return &texture;
    }
    return nullptr;
}

const ShaderManifestPalette* ShaderManifest::findPalette(const std::string& uniform) const{
    for (const ShaderManifestPalette& palette : palettes){
        if (palette.uniform == uniform)
            return &palette;
    }
    return nullptr;
}

bool ShaderManifestPalette::isEnabled(const std::vector<std::string>& enabledFeatures) const{
    if (features.empty())
        return true;
    for (const std::string& feature : features){
        if (std::find(enabledFeatures.begin(), enabledFeatures.end(), feature) != enabledFeatures.end())
            return true;
    }
    return false;
}

const ShaderManifestFeature* ShaderManifest::findFeature(const std::string& define) const{
    for (const ShaderManifestFeature& feature : features){
        if (feature.define == define)
            return &feature;
    }
    return nullptr;
}

const ShaderManifestKeywords* ShaderManifest::findKeywordGroup(const std::string& define) const{
    for (const ShaderManifestKeywords& group : keywords){
        if (define.size() > group.group.size() + 1 && define.compare(0, group.group.size(), group.group) == 0 &&
                define[group.group.size()] == '_'){
            const std::string value = define.substr(group.group.size() + 1);
            if (std::find(group.values.begin(), group.values.end(), value) != group.values.end())
                return &group;
        }
    }
    return nullptr;
}

bool ShaderManifest::isRowShown(const std::string& feature, const std::vector<std::string>& enabled){
    if (feature.empty())
        return true;
    const bool negate = feature[0] == '!';
    const std::string define = negate ? feature.substr(1) : feature;
    const bool on = std::find(enabled.begin(), enabled.end(), define) != enabled.end();
    return negate ? !on : on;
}

std::vector<std::string> ShaderManifest::resolveFeatures(const std::vector<std::string>& list) const{
    std::vector<std::string> defines;

    for (const ShaderManifestFeature& feature : features){
        bool on = feature.defaultOn;
        for (const std::string& entry : list){
            if (entry == feature.define){
                on = true;
                break;
            }
            if (entry.size() == feature.define.size() + 1 && entry[0] == '!' && entry.compare(1, std::string::npos, feature.define) == 0){
                on = false;
                break;
            }
        }
        if (on)
            defines.push_back(feature.define);
    }

    for (const ShaderManifestKeywords& group : keywords){
        std::string chosen = group.group + "_" + group.values.front();
        for (const std::string& entry : list){
            if (findKeywordGroup(entry) == &group){
                chosen = entry;
                break;
            }
        }
        defines.push_back(chosen);
    }

    std::sort(defines.begin(), defines.end());
    defines.erase(std::unique(defines.begin(), defines.end()), defines.end());
    return defines;
}

bool ShaderManifest::getDefault(const std::string& member, Vector4& value) const{
    if (const ShaderManifestProperty* property = findProperty(member)){
        value = property->defaultValue;
        return true;
    }
    return false;
}

void ShaderManifest::validateReflection(const std::vector<const ShaderData*>& variants, const std::string& file){
    // what any of the variants declares
    std::map<std::string, ShaderUniformType> members;
    std::map<std::string, TextureType> declaredTextures;
    for (const ShaderData* data : variants){
        if (!data)
            continue;
        for (const ShaderStage& stage : data->stages){
            for (const ShaderUniformBlock& block : stage.uniformblocks){
                // (a post-process pass's parameters are u_fs_postParams)
                if (block.name != "u_vs_customParams" && block.name != "u_fs_customParams" && block.name != "u_fs_postParams")
                    continue;
                for (const ShaderUniform& uniform : block.uniforms)
                    members[ShaderData::getUniformShortName(uniform.name)] = uniform.type;
            }
            for (const ShaderTexture& texture : stage.textures)
                declaredTextures[texture.name] = texture.type;
        }
    }

    for (const ShaderManifestProperty& property : properties){
        auto it = members.find(property.member);
        if (it == members.end()){
            diagnostics.push_back({file, property.line, true, "@property '" + property.member +
                "' is not a member of u_vs_customParams, u_fs_customParams or u_fs_postParams"});
            continue;
        }
        size_t components = 0;
        switch (it->second){
            case ShaderUniformType::FLOAT: case ShaderUniformType::INT:   components = 1; break;
            case ShaderUniformType::FLOAT2: case ShaderUniformType::INT2: components = 2; break;
            case ShaderUniformType::FLOAT3: case ShaderUniformType::INT3: components = 3; break;
            case ShaderUniformType::FLOAT4: case ShaderUniformType::INT4: components = 4; break;
            default: break;
        }
        const size_t wanted = kindComponents(property.kind);
        const bool colorAsVec3 = isColorKind(property.kind) && components == 3;
        if (components == 0 || (components != wanted && !colorAsVec3)){
            diagnostics.push_back({file, property.line, true, "@property '" + property.member +
                "' kind does not match its member type (" + std::to_string(wanted) + " components expected)"});
        }
    }

    for (const ShaderManifestTexture& texture : textures){
        auto it = declaredTextures.find(texture.uniform);
        if (it == declaredTextures.end()){
            diagnostics.push_back({file, texture.line, true, "@texture '" + texture.uniform + "' is not declared by the shader"});
            continue;
        }
        const bool isCube = it->second == TextureType::TEXTURE_CUBE;
        if (isCube != texture.cube){
            diagnostics.push_back({file, texture.line, true, "@texture '" + texture.uniform + "' is " +
                (isCube ? "a textureCube: add 'cube'" : "a texture2D: remove 'cube'")});
        }
    }

    for (const ShaderManifestPalette& palette : palettes){
        for (const std::string& texture : {palette.cellsTexture(), palette.rowsTexture()}){
            auto it = declaredTextures.find(texture);
            if (it == declaredTextures.end()){
                diagnostics.push_back({file, palette.line, true, "@palette '" + palette.uniform + "' needs the texture2D '" + texture +
                    "' (includes/palette.glsl declares it)"});
            }else if (it->second == TextureType::TEXTURE_CUBE){
                diagnostics.push_back({file, palette.line, true, "@palette '" + palette.uniform + "': '" + texture + "' must be a texture2D"});
            }
        }
    }

    for (const auto& member : members){
        // the engine writes the reserved members (time, resolution, sceneTime, the water's)
        if (!ShaderUniforms::isReserved(member.first) && !findProperty(member.first)){
            diagnostics.push_back({file, 1, false, "member '" + member.first + "' of the custom block has no @property (it stays zero)"});
        }
    }
}

void ShaderManifestState::apply(const ShaderManifestState& other){
    if (other.blend != MaterialBlendMode::AUTO) blend = other.blend;
    if (other.cull != MaterialCullMode::AUTO) cull = other.cull;
    if (other.depthWrite != MaterialDepthWrite::AUTO) depthWrite = other.depthWrite;
    if (other.depthTest != MaterialDepthTest::AUTO) depthTest = other.depthTest;
    if (other.hasDepthOffset){
        hasDepthOffset = true;
        depthOffsetFactor = other.depthOffsetFactor;
        depthOffsetUnits = other.depthOffsetUnits;
    }
    if (other.queue != ShaderQueue::AUTO) queue = other.queue;
    if (other.shadows != ShaderShadowCasting::AUTO) shadows = other.shadows;
    if (other.stencilSet & ShaderStencil_Compare) stencil.compare = other.stencil.compare;
    if (other.stencilSet & ShaderStencil_Ref) stencil.ref = other.stencil.ref;
    if (other.stencilSet & ShaderStencil_ReadMask) stencil.readMask = other.stencil.readMask;
    if (other.stencilSet & ShaderStencil_WriteMask) stencil.writeMask = other.stencil.writeMask;
    if (other.stencilSet & ShaderStencil_Pass) stencil.pass = other.stencil.pass;
    if (other.stencilSet & ShaderStencil_Fail) stencil.fail = other.stencil.fail;
    if (other.stencilSet & ShaderStencil_DepthFail) stencil.depthFail = other.stencil.depthFail;
    stencilSet |= other.stencilSet;
    if (other.colorWrite >= 0) colorWrite = other.colorWrite;
    if (other.hasPriority){
        hasPriority = true;
        priority = other.priority;
    }
}

bool ShaderManifestState::hasStencil() const{
    return stencil.compare != MaterialStencilCompare::AUTO && stencil.compare != MaterialStencilCompare::DISABLED;
}


ShaderManifestState ShaderManifest::resolveState(const std::vector<std::string>& enabled) const{
    ShaderManifestState resolved = state;
    for (const ShaderManifestStateRule& rule : stateRules){
        if (conditionHolds(rule.define, rule.negate, enabled))
            resolved.apply(rule.state);
    }
    return resolved;
}

uint32_t ShaderManifest::resolveRequire(const std::vector<std::string>& enabled) const{
    uint32_t resolved = require;
    for (const ShaderManifestRequireRule& rule : requireRules){
        if (conditionHolds(rule.define, rule.negate, enabled))
            resolved |= rule.require;
    }
    return resolved;
}

std::vector<std::string> ShaderManifest::getRequireDefines(const std::vector<std::string>& enabled) const{
    return getRequireDefines(resolveRequire(enabled));
}

std::vector<std::string> ShaderManifest::getRequireDefines(uint32_t require){
    std::vector<std::string> defines;
    if (require & ShaderRequire_Position) defines.push_back("SURFACE_POSITION");
    if (require & ShaderRequire_Normals)  defines.push_back("SURFACE_NORMALS");
    if (require & ShaderRequire_Tangents) defines.push_back("SURFACE_TANGENTS");
    if (require & ShaderRequire_UV1)      defines.push_back("SURFACE_UV1");
    if (require & ShaderRequire_UV2)      defines.push_back("SURFACE_UV2");
    if (require & ShaderRequire_Color)    defines.push_back("SURFACE_COLOR");
    if (require & ShaderRequire_RawColor) defines.push_back("SURFACE_RAW_COLOR");
    if (require & ShaderRequire_SmoothNormals) defines.push_back("SURFACE_SMOOTH_NORMALS");
    if (require & ShaderRequire_SceneDepth) defines.push_back("SURFACE_SCENE_DEPTH");
    if (require & ShaderRequire_SceneColor) defines.push_back("SURFACE_SCENE_COLOR");
    if (require & ShaderRequire_Environment) defines.push_back("SURFACE_ENVIRONMENT");
    if (require & ShaderRequire_NormalMap) defines.push_back("SURFACE_NORMAL_MAP");
    if (require & ShaderRequire_PlanarReflection) defines.push_back("SURFACE_PLANAR_REFLECTION");
    return defines;
}

uint32_t ShaderManifest::getPossibleSceneInputs() const{
    return getPossibleRequire() & ShaderRequire_SceneInputs;
}

uint32_t ShaderManifest::getPossibleRequire() const{
    uint32_t inputs = require;
    for (const ShaderManifestRequireRule& rule : requireRules)
        inputs |= rule.require;
    return inputs;
}

std::vector<std::string> ShaderManifest::getRequireNames(uint32_t require){
    static const std::pair<uint32_t, const char*> names[] = {
        {ShaderRequire_Position, "position"}, {ShaderRequire_Normals, "normals"}, {ShaderRequire_Tangents, "tangents"},
        {ShaderRequire_UV1, "uv1"}, {ShaderRequire_UV2, "uv2"}, {ShaderRequire_Color, "color"},
        {ShaderRequire_RawColor, "rawcolor"}, {ShaderRequire_SmoothNormals, "smoothnormals"},
        {ShaderRequire_SceneDepth, "scenedepth"}, {ShaderRequire_SceneColor, "scenecolor"},
        {ShaderRequire_Environment, "environment"}, {ShaderRequire_NormalMap, "normalmap"},
        {ShaderRequire_PlanarReflection, "planarreflection"}, {ShaderRequire_WaterExclusion, "waterexclusion"},
        {ShaderRequire_WaterWake, "waterwake"}};
    std::vector<std::string> result;
    for (const auto& name : names){
        if (require & name.first)
            result.push_back(name.second);
    }
    return result;
}

bool ShaderManifest::hasManifestLines(const std::string& source){
    ShaderManifest manifest;
    manifest.parse(source, "");
    return manifest.present;
}

bool ShaderManifest::definesLitStochasticParams(const std::string& source){
    static const std::string name = "LIT_STOCHASTIC_PARAMS";
    bool inBlockComment = false;
    size_t lineStart = 0;
    while (lineStart <= source.size()){
        size_t lineEnd = source.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = source.size();
        // the line without its comments
        std::string code;
        for (size_t i = lineStart; i < lineEnd; i++){
            if (inBlockComment){
                if (source.compare(i, 2, "*/") == 0){
                    inBlockComment = false;
                    i++;
                }
                continue;
            }
            if (source.compare(i, 2, "//") == 0)
                break;
            if (source.compare(i, 2, "/*") == 0){
                inBlockComment = true;
                i++;
                code += ' ';
                continue;
            }
            code += source[i];
        }
        // "#", "define", the name and then nothing of a longer name
        size_t at = code.find_first_not_of(" \t\r");
        if (at != std::string::npos && code[at] == '#'){
            at = code.find_first_not_of(" \t", at + 1);
            if (at != std::string::npos && code.compare(at, 6, "define") == 0){
                const size_t afterDefine = at + 6;
                at = code.find_first_not_of(" \t", afterDefine);
                if (at != std::string::npos && at > afterDefine && code.compare(at, name.size(), name) == 0){
                    const size_t after = at + name.size();
                    if (after == code.size() || !(std::isalnum((unsigned char)code[after]) || code[after] == '_'))
                        return true;
                }
            }
        }
        lineStart = lineEnd + 1;
    }
    return false;
}

std::string ShaderManifest::getLightingModeName(ShaderLightingMode mode){
    switch (mode){
        case ShaderLightingMode::PIXEL:  return "pixel";
        case ShaderLightingMode::VERTEX: return "vertex";
        case ShaderLightingMode::MIXED:  return "mixed";
        default:                         return "none";
    }
}

const char* ShaderManifest::getStencilCompareName(MaterialStencilCompare compare){
    switch (compare){
        case MaterialStencilCompare::DISABLED:      return "off";
        case MaterialStencilCompare::NEVER:         return "never";
        case MaterialStencilCompare::LESS:          return "less";
        case MaterialStencilCompare::EQUAL:         return "equal";
        case MaterialStencilCompare::LESS_EQUAL:    return "lequal";
        case MaterialStencilCompare::GREATER:       return "greater";
        case MaterialStencilCompare::NOT_EQUAL:     return "notequal";
        case MaterialStencilCompare::GREATER_EQUAL: return "gequal";
        case MaterialStencilCompare::ALWAYS:        return "always";
        default:                                    return "auto";
    }
}

bool ShaderManifest::parseStencilCompare(const std::string& name, MaterialStencilCompare& compare){
    static const MaterialStencilCompare values[] = {
        MaterialStencilCompare::DISABLED, MaterialStencilCompare::NEVER, MaterialStencilCompare::LESS,
        MaterialStencilCompare::EQUAL, MaterialStencilCompare::LESS_EQUAL, MaterialStencilCompare::GREATER,
        MaterialStencilCompare::NOT_EQUAL, MaterialStencilCompare::GREATER_EQUAL, MaterialStencilCompare::ALWAYS
    };
    for (MaterialStencilCompare value : values){
        if (name == getStencilCompareName(value)){
            compare = value;
            return true;
        }
    }
    return false;
}

const char* ShaderManifest::getStencilOpName(MaterialStencilOp op){
    switch (op){
        case MaterialStencilOp::ZERO:       return "zero";
        case MaterialStencilOp::REPLACE:    return "replace";
        case MaterialStencilOp::INCR_CLAMP: return "incr";
        case MaterialStencilOp::DECR_CLAMP: return "decr";
        case MaterialStencilOp::INVERT:     return "invert";
        case MaterialStencilOp::INCR_WRAP:  return "incrwrap";
        case MaterialStencilOp::DECR_WRAP:  return "decrwrap";
        default:                            return "keep";
    }
}

bool ShaderManifest::parseStencilOp(const std::string& name, MaterialStencilOp& op){
    static const MaterialStencilOp values[] = {
        MaterialStencilOp::KEEP, MaterialStencilOp::ZERO, MaterialStencilOp::REPLACE, MaterialStencilOp::INCR_CLAMP,
        MaterialStencilOp::DECR_CLAMP, MaterialStencilOp::INVERT, MaterialStencilOp::INCR_WRAP, MaterialStencilOp::DECR_WRAP
    };
    for (MaterialStencilOp value : values){
        if (name == getStencilOpName(value)){
            op = value;
            return true;
        }
    }
    return false;
}

std::string ShaderManifest::getColorWriteName(int colorWrite){
    if (colorWrite < 0)
        return "auto";
    if ((colorWrite & MATERIAL_COLOR_WRITE_ALL) == 0)
        return "none";
    std::string name;
    if (colorWrite & MATERIAL_COLOR_WRITE_R) name += 'r';
    if (colorWrite & MATERIAL_COLOR_WRITE_G) name += 'g';
    if (colorWrite & MATERIAL_COLOR_WRITE_B) name += 'b';
    if (colorWrite & MATERIAL_COLOR_WRITE_A) name += 'a';
    return name;
}

int ShaderManifest::parseColorWrite(const std::string& name){
    if (name == "none")
        return 0;
    if (name.empty())
        return -1;
    int mask = 0;
    for (char c : name){
        int bit = 0;
        switch (c){
            case 'r': bit = MATERIAL_COLOR_WRITE_R; break;
            case 'g': bit = MATERIAL_COLOR_WRITE_G; break;
            case 'b': bit = MATERIAL_COLOR_WRITE_B; break;
            case 'a': bit = MATERIAL_COLOR_WRITE_A; break;
            default: return -1;
        }
        if (mask & bit)
            return -1; // a channel twice
        mask |= bit;
    }
    return mask;
}

std::string ShaderManifest::getShaderTypeToken(ShaderType type){
    switch (type){
        case ShaderType::MESH:   return "mesh";
        case ShaderType::UI:     return "ui";
        case ShaderType::POINTS: return "points";
        case ShaderType::LINES:  return "lines";
        case ShaderType::PARTICLES: return "particles";
        case ShaderType::SKYBOX: return "sky";
        case ShaderType::POSTPROCESS: return "postprocess";
        default:                 return "";
    }
}

// ---------------------------------------------------------------- ShaderReference

const char* const ShaderReference::LIBRARY_PREFIX = "library:";
const char* const ShaderReference::BUILTIN_PREFIX = "builtin:";
const char* const ShaderReference::BUILTIN_LIT = "builtin:lit";

bool ShaderReference::isLibrary(const std::string& reference){
    return reference.compare(0, 8, LIBRARY_PREFIX) == 0;
}

bool ShaderReference::isBuiltin(const std::string& reference){
    return reference.compare(0, 8, BUILTIN_PREFIX) == 0;
}

std::string ShaderReference::getLibraryName(const std::string& reference){
    if (!isLibrary(reference))
        return "";
    return getBase(reference).substr(8);
}

std::string ShaderReference::getBase(const std::string& reference){
    const size_t hash = reference.find('#');
    return hash == std::string::npos ? reference : reference.substr(0, hash);
}

std::vector<std::string> ShaderReference::getFeatures(const std::string& reference){
    std::vector<std::string> features;
    const size_t hash = reference.find('#');
    if (hash == std::string::npos)
        return features;
    size_t start = hash + 1;
    while (start <= reference.size()){
        size_t comma = reference.find(',', start);
        if (comma == std::string::npos)
            comma = reference.size();
        const std::string feature = trim(reference.substr(start, comma - start));
        if (!feature.empty())
            features.push_back(feature);
        start = comma + 1;
    }
    return features;
}

bool ShaderReference::isValidDefine(const std::string& define){
    if (define.empty() || std::isdigit((unsigned char)define[0]))
        return false;
    for (char c : define){
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

bool ShaderReference::isValidLibraryName(const std::string& name){
    if (name.empty() || !(name[0] >= 'a' && name[0] <= 'z'))
        return false;
    for (char c : name){
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

std::string ShaderReference::withFeatures(const std::string& base, const std::vector<std::string>& defines){
    if (defines.empty())
        return base;
    std::string reference = base + "#";
    for (size_t i = 0; i < defines.size(); i++){
        if (i > 0)
            reference += ",";
        reference += defines[i];
    }
    return reference;
}

std::vector<std::string> ShaderReference::getEnabledFeatures(const std::string& reference, const std::vector<std::string>& shaderFeatures){
    const std::string base = getBase(reference);
    if (base.empty())
        return {};
    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(base);
    if (!manifest || (manifest->features.empty() && manifest->keywords.empty()))
        return {};

    // a suffix written in the reference itself counts as listed first
    std::vector<std::string> list = getFeatures(reference);
    list.insert(list.end(), shaderFeatures.begin(), shaderFeatures.end());
    return manifest->resolveFeatures(list);
}

std::string ShaderReference::getVariantReference(const std::string& reference, const std::vector<std::string>& shaderFeatures){
    return withFeatures(getBase(reference), getEnabledFeatures(reference, shaderFeatures));
}

std::string ShaderReference::getColorVariantReference(const std::string& reference, const std::vector<std::string>& shaderFeatures, MaterialBlendMode blendMode){
    const std::string base = getBase(reference);
    if (base.empty())
        return base;
    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(base);
    std::vector<std::string> defines = getEnabledFeatures(reference, shaderFeatures);
    if (!manifest || !manifest->present)
        return withFeatures(base, defines);

    if (blendMode == MaterialBlendMode::AUTO)
        blendMode = manifest->resolveState(defines).blend;
    defines.push_back(getBlendDefine(blendMode));
    std::sort(defines.begin(), defines.end());
    return withFeatures(base, defines);
}

const char* ShaderReference::getBlendDefine(MaterialBlendMode blendMode){
    switch (blendMode){
        case MaterialBlendMode::NONE:          return "BLEND_NONE";
        case MaterialBlendMode::PREMULTIPLIED: return "BLEND_PREMULTIPLIED";
        case MaterialBlendMode::ADDITIVE:      return "BLEND_ADDITIVE";
        case MaterialBlendMode::MULTIPLY:      return "BLEND_MULTIPLY";
        default:                               return "BLEND_ALPHA"; // AUTO: the legacy alpha blend
    }
}

bool ShaderReference::isBlendDefine(const std::string& define){
    return define == "BLEND_NONE" || define == "BLEND_ALPHA" || define == "BLEND_PREMULTIPLIED" ||
        define == "BLEND_ADDITIVE" || define == "BLEND_MULTIPLY";
}

// ---------------------------------------------------------------- ShaderLibraryValues

Vector4 ShaderLibraryValues::getParam(const ShaderUniformValues& values, const std::string& shader, const std::string& name){
    if (ShaderUniforms::has(values, name))
        return ShaderUniforms::get(values, name);
    Vector4 value = Vector4::ZERO;
    if (std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader))
        manifest->getDefault(name, value);
    return value;
}

bool ShaderLibraryValues::setTexture(std::vector<std::pair<std::string, Texture>>& textures, const std::string& uniform, const std::string& path){
    for (size_t t = 0; t < textures.size(); t++){
        if (textures[t].first != uniform)
            continue;
        if (path.empty()){
            textures.erase(textures.begin() + t);
            return true;
        }
        if (textures[t].second.getPath() == path && !textures[t].second.isCubeMap())
            return false;
        textures[t].second.setPath(path);
        return true;
    }
    if (path.empty())
        return false;
    textures.push_back({uniform, Texture(path)});
    return true;
}

bool ShaderLibraryValues::setCubeTexture(std::vector<std::pair<std::string, Texture>>& textures, const std::string& uniform, const std::string& path){
    if (path.empty())
        return setTexture(textures, uniform, path);
    for (auto& texture : textures){
        if (texture.first != uniform)
            continue;
        if (texture.second.isCubeMap() && texture.second.getPath(0) == path && texture.second.getPath(1).empty())
            return false;
        texture.second.setCubeMap(path);
        return true;
    }
    Texture texture;
    texture.setCubeMap(path);
    textures.push_back({uniform, texture});
    return true;
}

bool ShaderLibraryValues::setCubeTextureFaces(std::vector<std::pair<std::string, Texture>>& textures, const std::string& uniform,
                                              const std::string& front, const std::string& back, const std::string& left,
                                              const std::string& right, const std::string& up, const std::string& down){
    // in face index order (+X, -X, +Y, -Y, +Z, -Z), as setCubePaths stores them
    const std::string faces[6] = {right, left, up, down, front, back};
    for (auto& texture : textures){
        if (texture.first != uniform)
            continue;
        bool same = texture.second.isCubeMap();
        for (size_t f = 0; f < 6 && same; f++)
            same = texture.second.getPath(f) == faces[f];
        if (same)
            return false;
        texture.second.setCubePaths(front, back, left, right, up, down);
        return true;
    }
    Texture texture;
    texture.setCubePaths(front, back, left, right, up, down);
    textures.push_back({uniform, texture});
    return true;
}

std::string ShaderLibraryValues::getTexture(const std::vector<std::pair<std::string, Texture>>& textures, const std::string& uniform){
    for (const auto& texture : textures){
        if (texture.first != uniform)
            continue;
        // six face images: the front one (+Z, face 4); a single cube map image is face 0
        if (texture.second.isCubeMap() && !texture.second.getPath(1).empty())
            return texture.second.getPath(4);
        return texture.second.getPath();
    }
    return "";
}

bool ShaderLibraryValues::setFeature(std::vector<std::string>& features, const std::string& shader, const std::string& define, bool enabled){
    const std::vector<std::string> before = features;
    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
    const ShaderManifestKeywords* group = manifest ? manifest->findKeywordGroup(define) : nullptr;

    features.erase(std::remove_if(features.begin(), features.end(), [&](const std::string& entry){
        if (entry == define || entry == "!" + define)
            return true;
        // a keyword value replaces the other values of its group
        return enabled && group && manifest->findKeywordGroup(entry) == group;
    }), features.end());

    if (enabled){
        features.push_back(define);
    }else if (!group){
        // explicit off, so a default-on feature stays off
        features.push_back("!" + define);
    }
    return features != before;
}

bool ShaderLibraryValues::isFeatureEnabled(const std::vector<std::string>& features, const std::string& shader, const std::string& define){
    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
    if (!manifest)
        return std::find(features.begin(), features.end(), define) != features.end();
    const std::vector<std::string> enabled = manifest->resolveFeatures(features);
    return std::find(enabled.begin(), enabled.end(), define) != enabled.end();
}

// ---------------------------------------------------------------- ShaderManifests

std::shared_ptr<const ShaderManifest> ShaderManifests::get(const std::string& reference){
    const std::string base = ShaderReference::getBase(reference);
    if (base.empty())
        return nullptr;

    ManifestCache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (auto it = c.manifests.find(base); it != c.manifests.end())
        return it->second;

    auto manifest = std::make_shared<ShaderManifest>();
    bool found = false;
    if (ShaderReference::isLibrary(base)){
        const std::string name = ShaderReference::getLibraryName(base);
        const auto& sources = librarySources();
        const std::string fragPath = "library/" + name + ".frag";
        const std::string vertPath = "library/" + name + ".vert";
        auto frag = sources.find(fragPath);
        if (frag != sources.end()){
            found = true;
            manifest->parse(frag->second, "engine/shaderlib/" + fragPath);
            if (auto vert = sources.find(vertPath); vert != sources.end())
                manifest->parse(vert->second, "engine/shaderlib/" + vertPath);
            manifest->checkComplete("engine/shaderlib/" + fragPath, true);
        }
    }else{
        std::string vertFile, fragFile;
        ShaderVariantCompiler::getCustomShaderPaths(base, vertFile, fragFile);
        std::string content;
        if (c.forkReader && c.forkReader(fragFile, content)){
            found = true;
            manifest->parse(content, fragFile);
            manifest->litStochasticParams = ShaderManifest::definesLitStochasticParams(content);
            if (c.forkReader(vertFile, content))
                manifest->parse(content, vertFile);
            if (manifest->present)
                manifest->checkComplete(fragFile, false);
        }
    }

    if (!found){
        // no retry until the sources change (clearForks); reported once
        if (c.reported.insert(base).second){
            Log::error("Shader '%s' has no manifest to read: its parameters render zero-initialised", base.c_str());
        }
        c.manifests[base] = nullptr;
        return nullptr;
    }

    logDiagnostics(*manifest);
    c.manifests[base] = manifest;
    return manifest;
}

void ShaderManifests::setForkReader(SourceReader reader){
    ManifestCache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.forkReader = reader ? reader : SourceReader(readAssetFile);
    for (auto it = c.manifests.begin(); it != c.manifests.end();){
        if (!ShaderReference::isLibrary(it->first)){
            c.reported.erase(it->first);
            it = c.manifests.erase(it);
        }else{
            ++it;
        }
    }
}

bool ShaderManifests::readForkFile(const std::string& path, std::string& content){
    SourceReader reader;
    {
        ManifestCache& c = cache();
        std::lock_guard<std::mutex> lock(c.mutex);
        reader = c.forkReader;
    }
    return reader && reader(path, content);
}

void ShaderManifests::clearForks(){
    clearForks([](const std::string&){ return true; });
}

void ShaderManifests::clearForks(const std::function<bool(const std::string& fork)>& which){
    ManifestCache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    for (auto it = c.manifests.begin(); it != c.manifests.end();){
        if (!ShaderReference::isLibrary(it->first) && which(it->first)){
            c.reported.erase(it->first);
            it = c.manifests.erase(it);
        }else{
            ++it;
        }
    }
}

std::vector<std::string> ShaderManifests::getLibraryShaderNames(){
    std::vector<std::string> names;
    for (const auto& file : librarySources()){
        const std::string& path = file.first;
        const std::string prefix = "library/";
        const std::string suffix = ".frag";
        if (path.size() <= prefix.size() + suffix.size() || path.compare(0, prefix.size(), prefix) != 0 ||
                path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        const std::string name = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
        // depth programs and the shared surface files are not shaders of their own
        if (name.find('/') != std::string::npos || name == "surface" || name == "surface_depth")
            continue;
        if (name.size() > 6 && name.compare(name.size() - 6, 6, "_depth") == 0)
            continue;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

const std::unordered_map<std::string, std::string>& ShaderManifests::getLibraryManifestSources(){
    return librarySources();
}

bool ShaderManifests::hasLibraryFile(const std::string& path){
    return librarySources().count(path) > 0;
}
