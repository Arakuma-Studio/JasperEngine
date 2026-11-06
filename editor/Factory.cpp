// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Factory.h"
#include "component/AudioSourceComponent.h"
#include "component/BuoyancyComponent.h"
#include "component/CameraComponent.h"
#include "component/CameraFollowComponent.h"
#include "component/CameraShakerComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/ForceFieldComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeFollowerComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/ModelComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/SafeAreaComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/SkyboxComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/TextComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "component/TranslateTracksComponent.h"
#include "component/UIComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "component/WaterVolumeComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/PrefabComponent.h"
#include "component/ScriptComponent.h"
#include "component/TextEffectComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/VideoPlayerComponent.h"
#include "reflect/Reflect.h"
#include "reflect/Enums.h"

#include "Scene.h"
#include "Catalog.h"
#include "Configs.h"
#include "Stream.h"
#include "particles/ParticleEffectCodec.h"
#include "util/CameraTextureLink.h"
#include "util/FluidTextureLink.h"
#include "util/VideoTextureLink.h"
#include "component/FluidSimulationComponent.h"
#include "editor/Out.h"
#include <cmath>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <array>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <filesystem>

#include "util/FileUtils.h"
#include "stream/CurveCodec.h"

#include "resources/sky/Daylight_Box_Back_png.h"
#include "resources/sky/Daylight_Box_Bottom_png.h"
#include "resources/sky/Daylight_Box_Front_png.h"
#include "resources/sky/Daylight_Box_Left_png.h"
#include "resources/sky/Daylight_Box_Right_png.h"
#include "resources/sky/Daylight_Box_Top_png.h"

using namespace jasperengine;

// A path is exported whatever became of its load (empty() turns true once a load failed, on the
// first draw that asked for it): the generated code never depends on what was drawn before it
// (Stream::encodeTexture, the same way)
static bool isTextureExported(const Texture& texture) {
    if (!texture.empty()) return true;
    for (size_t face = 0; face < 6; face++) {
        if (!texture.getPath(face).empty()) return true;
    }
    return false;
}

template <typename T>
static T sanitizeFiniteNumber(T value) {
    return std::isfinite(value) ? value : static_cast<T>(0);
}

// Matrix4's 16-float constructor takes the entries row by row, matrix[column][row]
static std::string formatMatrix4(const Matrix4& matrix) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << "Matrix4(";
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            if (row > 0 || col > 0) oss << ", ";
            oss << sanitizeFiniteNumber(matrix[col][row]) << "f";
        }
    }
    oss << ")";
    return oss.str();
}

static std::string formatCullingMode(CullingMode mode) {
    return mode == CullingMode::FRONT ? "CullingMode::FRONT" : "CullingMode::BACK";
}

static std::string formatWindingOrder(WindingOrder order) {
    return order == WindingOrder::CW ? "WindingOrder::CW" : "WindingOrder::CCW";
}

static std::string formatBufferType(BufferType type) {
    switch (type) {
        case BufferType::INDEX_BUFFER: return "BufferType::INDEX_BUFFER";
        case BufferType::STORAGE_BUFFER: return "BufferType::STORAGE_BUFFER";
        default: return "BufferType::VERTEX_BUFFER";
    }
}

static std::string formatBufferUsage(BufferUsage usage) {
    switch (usage) {
        case BufferUsage::DYNAMIC: return "BufferUsage::DYNAMIC";
        case BufferUsage::STREAM: return "BufferUsage::STREAM";
        default: return "BufferUsage::IMMUTABLE";
    }
}

// Raw buffer bytes as a static array: the data can be megabytes, which must not land on
// the generated factory's stack (create_<scene>() would overflow at function entry).
static void writeStaticBytes(std::ostringstream& code, const std::string& ind, const char* qualifiers, const unsigned char* bytes, size_t size) {
    code << ind << "    static " << qualifiers << "unsigned char data[] = {";
    for (size_t i = 0; i < size; i++) {
        if (i % 50 == 0) code << "\n" << ind << "        ";
        code << "0x" << std::hex << (int)bytes[i] << std::dec << ", ";
    }
    code << "};\n";
}

// The buffer settings saved with a scene that differ from a newly constructed buffer of the
// same kind, so the generated buffer is set up the way the editor had it.
static void writeBufferSettings(std::ostringstream& code, const std::string& ind, const std::string& target, const Buffer& buffer, const Buffer& defaults) {
    if (buffer.getType() != defaults.getType())
        code << ind << target << ".setType(" << formatBufferType(buffer.getType()) << ");\n";
    if (buffer.getUsage() != defaults.getUsage())
        code << ind << target << ".setUsage(" << formatBufferUsage(buffer.getUsage()) << ");\n";
    if (buffer.isRenderAttributes() != defaults.isRenderAttributes())
        code << ind << target << ".setRenderAttributes(" << (buffer.isRenderAttributes() ? "true" : "false") << ");\n";
    if (buffer.isInstanceBuffer() != defaults.isInstanceBuffer())
        code << ind << target << ".setInstanceBuffer(" << (buffer.isInstanceBuffer() ? "true" : "false") << ");\n";
}

bool editor::Factory::writeHeaderIfChanged(const fs::path& path, const std::string& varName, const unsigned char* data, size_t len) {
    std::ostringstream out;
    out << "// This file is auto-generated by JasperEngine Editor. Do not edit manually.\n\n";
    out << "#ifndef " << varName << "_H\n";
    out << "#define " << varName << "_H\n\n";

    out << "inline unsigned char " << varName << "_data[] = {";
    for (size_t i = 0; i < len; i++) {
        if (i % 20 == 0) {
            out << "\n    ";
        }
        out << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << ", ";
    }
    out << "\n};\n";
    out << "inline const unsigned int " << varName << "_len = " << std::dec << len << ";\n\n";

    out << "#endif // " << varName << "_H\n";

    return FileUtils::writeIfChanged(path, out.str());
}

bool editor::Factory::ensureDefaultSkyFiles(const fs::path& baseDir) {
    if (baseDir.empty()) {
        return false;
    }

    const fs::path frontPath = baseDir / "Daylight_Box_Front.h";
    const fs::path backPath = baseDir / "Daylight_Box_Back.h";
    const fs::path leftPath = baseDir / "Daylight_Box_Left.h";
    const fs::path rightPath = baseDir / "Daylight_Box_Right.h";
    const fs::path topPath = baseDir / "Daylight_Box_Top.h";
    const fs::path bottomPath = baseDir / "Daylight_Box_Bottom.h";

    if (!writeHeaderIfChanged(frontPath, "Daylight_Box_Front", Daylight_Box_Front_png, Daylight_Box_Front_png_len)) return false;
    if (!writeHeaderIfChanged(backPath, "Daylight_Box_Back", Daylight_Box_Back_png, Daylight_Box_Back_png_len)) return false;
    if (!writeHeaderIfChanged(leftPath, "Daylight_Box_Left", Daylight_Box_Left_png, Daylight_Box_Left_png_len)) return false;
    if (!writeHeaderIfChanged(rightPath, "Daylight_Box_Right", Daylight_Box_Right_png, Daylight_Box_Right_png_len)) return false;
    if (!writeHeaderIfChanged(topPath, "Daylight_Box_Top", Daylight_Box_Top_png, Daylight_Box_Top_png_len)) return false;
    if (!writeHeaderIfChanged(bottomPath, "Daylight_Box_Bottom", Daylight_Box_Bottom_png, Daylight_Box_Bottom_png_len)) return false;

    return true;
}

void editor::Factory::writeSkyIncludes(std::ostringstream& out, bool usesDefaultSky, const fs::path& generatedPath, const std::string& ind) {
    fs::path skyResourcesPath = generatedPath / "resources" / "sky";
    if (usesDefaultSky) {
        ensureDefaultSkyFiles(skyResourcesPath);
        out << ind << "#include \"resources/sky/Daylight_Box_Front.h\"\n";
        out << ind << "#include \"resources/sky/Daylight_Box_Back.h\"\n";
        out << ind << "#include \"resources/sky/Daylight_Box_Left.h\"\n";
        out << ind << "#include \"resources/sky/Daylight_Box_Right.h\"\n";
        out << ind << "#include \"resources/sky/Daylight_Box_Top.h\"\n";
        out << ind << "#include \"resources/sky/Daylight_Box_Bottom.h\"\n";
    }
    out << ind << "\n";
}

std::string editor::Factory::indentation(int spaces) {
    if (spaces <= 0) {
        return "";
    }
    return std::string(static_cast<size_t>(spaces), ' ');
}

std::string editor::Factory::toIdentifier(const std::string& name) {
    std::string out;
    out.reserve(name.size());

    auto isIdentStart = [](unsigned char c) {
        return std::isalpha(c) || c == '_';
    };
    auto isIdentChar = [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    };

    bool lastWasUnderscore = false;
    for (unsigned char c : name) {
        if (isIdentChar(c)) {
            out.push_back(static_cast<char>(c));
            lastWasUnderscore = (c == '_');
        } else {
            if (!out.empty() && !lastWasUnderscore) {
                out.push_back('_');
                lastWasUnderscore = true;
            }
        }
    }

    while (!out.empty() && out.front() == '_') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }

    if (out.empty()) {
        out = "var";
    }

    if (!isIdentStart(static_cast<unsigned char>(out[0]))) {
        out.insert(out.begin(), '_');
    }

    static const std::unordered_set<std::string> kCppKeywords = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
        "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
        "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
        "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
        "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
        "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline",
        "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
        "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
        "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
    };

    if (kCppKeywords.find(out) != kCppKeywords.end()) {
        out.push_back('_');
    }

    return out;
}

std::string editor::Factory::formatCanvasScaler(const CanvasScalerSettings& settings) {
    static const char* const modes[] = {"UIScaleMode::FollowGameCanvas", "UIScaleMode::ConstantPixelSize",
                                        "UIScaleMode::ScaleWithScreenSize", "UIScaleMode::ConstantPhysicalSize"};
    static const char* const matchModes[] = {"ScreenMatchMode::MatchWidthOrHeight", "ScreenMatchMode::Expand", "ScreenMatchMode::Shrink"};
    static const char* const units[] = {"PhysicalUnit::Centimeters", "PhysicalUnit::Millimeters", "PhysicalUnit::Inches",
                                        "PhysicalUnit::Points", "PhysicalUnit::Picas"};
    const CanvasScalerSettings d;
    std::string out = "[](){ CanvasScalerSettings s;";
    if (settings.mode != d.mode) out += std::string(" s.mode = ") + modes[(int)settings.mode] + ";";
    if (settings.scaleFactor != d.scaleFactor) out += " s.scaleFactor = " + formatFloat(settings.scaleFactor) + ";";
    if (settings.referenceResolution != d.referenceResolution) out += " s.referenceResolution = " + formatVector2(settings.referenceResolution) + ";";
    if (settings.screenMatchMode != d.screenMatchMode) out += std::string(" s.screenMatchMode = ") + matchModes[(int)settings.screenMatchMode] + ";";
    if (settings.matchWidthOrHeight != d.matchWidthOrHeight) out += " s.matchWidthOrHeight = " + formatFloat(settings.matchWidthOrHeight) + ";";
    if (settings.physicalUnit != d.physicalUnit) out += std::string(" s.physicalUnit = ") + units[(int)settings.physicalUnit] + ";";
    if (settings.fallbackScreenDPI != d.fallbackScreenDPI) out += " s.fallbackScreenDPI = " + formatFloat(settings.fallbackScreenDPI) + ";";
    return out + " return s; }()";
}

std::string editor::Factory::formatVector2(const Vector2& v) {
    return "Vector2(" + formatFloat(v.x) + ", " + formatFloat(v.y) + ")";
}

std::string editor::Factory::formatVector3(const Vector3& v) {
    return "Vector3(" + formatFloat(v.x) + ", " + formatFloat(v.y) + ", " + formatFloat(v.z) + ")";
}

std::string editor::Factory::formatVector4(const Vector4& v) {
    return "Vector4(" + formatFloat(v.x) + ", " + formatFloat(v.y) + ", " + formatFloat(v.z) + ", " + formatFloat(v.w) + ")";
}

std::string editor::Factory::formatShaderUniforms(const ShaderUniformValues& uniforms) {
    std::string out = "{";
    for (size_t i = 0; i < uniforms.size(); i++) {
        if (i > 0)
            out += ", ";
        out += "{" + formatString(uniforms[i].first) + ", " + formatVector4(uniforms[i].second) + "}";
    }
    return out + "}";
}

std::string editor::Factory::formatShaderGlobalBindings(const ShaderGlobalBindings& bindings) {
    std::string out = "{";
    for (size_t i = 0; i < bindings.size(); i++) {
        if (i > 0)
            out += ", ";
        out += "{" + formatString(bindings[i].first) + ", " + formatString(bindings[i].second) + "}";
    }
    return out + "}";
}

std::string editor::Factory::formatShaderGlobalDecl(const ShaderGlobalDecl& decl) {
    static const char* const typeNames[] = {"Float", "Vector2", "Vector3", "Vector4", "Color", "Matrix4", "FloatArray",
                                            "Vector4Array", "Texture2D", "TextureCube"};
    std::string values = "{";
    for (size_t i = 0; i < decl.defaultValue.size(); i++) {
        if (i > 0)
            values += ", ";
        values += formatVector4(decl.defaultValue[i]);
    }
    values += "}";
    return "ShaderGlobalDecl{" + formatString(decl.name) + ", ShaderGlobalType::" + typeNames[static_cast<int>(decl.type)] + ", " +
           std::to_string(decl.arraySize) + "u, " + values + ", " + formatString(decl.defaultTexture) + "}";
}

std::string editor::Factory::formatShaderTextures(int indentSpaces, const std::vector<std::pair<std::string, Texture>>& textures, const std::string& target, const fs::path& projectPath) {
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    for (const auto& [uniform, texture] : textures) {
        if (!isTextureExported(texture)) continue; // binds the manifest default, like no entry
        code << ind << "{\n";
        code << ind << "    Texture texture;\n";
        code << formatTexture(indentSpaces + 4, texture, "texture", projectPath);
        code << ind << "    " << target << ".push_back({" << formatString(uniform) << ", texture});\n";
        code << ind << "}\n";
    }
    return code.str();
}

std::string editor::Factory::formatShaderFeatures(const std::vector<std::string>& features) {
    std::string out = "{";
    for (size_t i = 0; i < features.size(); i++) {
        if (i > 0)
            out += ", ";
        out += formatString(features[i]);
    }
    return out + "}";
}

static std::string formatBlendMode(MaterialBlendMode mode) {
    switch (mode) {
        case MaterialBlendMode::NONE: return "MaterialBlendMode::NONE";
        case MaterialBlendMode::ALPHA: return "MaterialBlendMode::ALPHA";
        case MaterialBlendMode::PREMULTIPLIED: return "MaterialBlendMode::PREMULTIPLIED";
        case MaterialBlendMode::ADDITIVE: return "MaterialBlendMode::ADDITIVE";
        case MaterialBlendMode::MULTIPLY: return "MaterialBlendMode::MULTIPLY";
        default: return "MaterialBlendMode::AUTO";
    }
}

static std::string formatCullMode(MaterialCullMode mode) {
    switch (mode) {
        case MaterialCullMode::BACK: return "MaterialCullMode::BACK";
        case MaterialCullMode::FRONT: return "MaterialCullMode::FRONT";
        case MaterialCullMode::OFF: return "MaterialCullMode::OFF";
        default: return "MaterialCullMode::AUTO";
    }
}

static std::string formatDepthWrite(MaterialDepthWrite mode) {
    switch (mode) {
        case MaterialDepthWrite::ENABLED: return "MaterialDepthWrite::ENABLED";
        case MaterialDepthWrite::DISABLED: return "MaterialDepthWrite::DISABLED";
        default: return "MaterialDepthWrite::AUTO";
    }
}

static std::string formatDepthTest(MaterialDepthTest mode) {
    switch (mode) {
        case MaterialDepthTest::LESS: return "MaterialDepthTest::LESS";
        case MaterialDepthTest::LESS_EQUAL: return "MaterialDepthTest::LESS_EQUAL";
        case MaterialDepthTest::EQUAL: return "MaterialDepthTest::EQUAL";
        case MaterialDepthTest::GREATER_EQUAL: return "MaterialDepthTest::GREATER_EQUAL";
        case MaterialDepthTest::GREATER: return "MaterialDepthTest::GREATER";
        case MaterialDepthTest::NOT_EQUAL: return "MaterialDepthTest::NOT_EQUAL";
        case MaterialDepthTest::ALWAYS: return "MaterialDepthTest::ALWAYS";
        default: return "MaterialDepthTest::AUTO";
    }
}

static std::string formatMaterialLighting(MaterialLighting mode) {
    switch (mode) {
        case MaterialLighting::PIXEL: return "MaterialLighting::PIXEL";
        case MaterialLighting::VERTEX: return "MaterialLighting::VERTEX";
        case MaterialLighting::MIXED: return "MaterialLighting::MIXED";
        case MaterialLighting::NONE: return "MaterialLighting::NONE";
        default: return "MaterialLighting::AUTO";
    }
}

static std::string formatStencilCompare(MaterialStencilCompare compare) {
    switch (compare) {
        case MaterialStencilCompare::DISABLED: return "MaterialStencilCompare::DISABLED";
        case MaterialStencilCompare::NEVER: return "MaterialStencilCompare::NEVER";
        case MaterialStencilCompare::LESS: return "MaterialStencilCompare::LESS";
        case MaterialStencilCompare::EQUAL: return "MaterialStencilCompare::EQUAL";
        case MaterialStencilCompare::LESS_EQUAL: return "MaterialStencilCompare::LESS_EQUAL";
        case MaterialStencilCompare::GREATER: return "MaterialStencilCompare::GREATER";
        case MaterialStencilCompare::NOT_EQUAL: return "MaterialStencilCompare::NOT_EQUAL";
        case MaterialStencilCompare::GREATER_EQUAL: return "MaterialStencilCompare::GREATER_EQUAL";
        case MaterialStencilCompare::ALWAYS: return "MaterialStencilCompare::ALWAYS";
        default: return "MaterialStencilCompare::AUTO";
    }
}

static std::string formatStencilOp(MaterialStencilOp op) {
    switch (op) {
        case MaterialStencilOp::ZERO: return "MaterialStencilOp::ZERO";
        case MaterialStencilOp::REPLACE: return "MaterialStencilOp::REPLACE";
        case MaterialStencilOp::INCR_CLAMP: return "MaterialStencilOp::INCR_CLAMP";
        case MaterialStencilOp::DECR_CLAMP: return "MaterialStencilOp::DECR_CLAMP";
        case MaterialStencilOp::INVERT: return "MaterialStencilOp::INVERT";
        case MaterialStencilOp::INCR_WRAP: return "MaterialStencilOp::INCR_WRAP";
        case MaterialStencilOp::DECR_WRAP: return "MaterialStencilOp::DECR_WRAP";
        default: return "MaterialStencilOp::KEEP";
    }
}

std::string editor::Factory::formatStencilAndColorWrite(int indentSpaces, const MaterialStencil& stencil, int colorWrite, const std::string& target) {
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    const MaterialStencil defaults;
    if (stencil.compare != defaults.compare)
        code << ind << target << ".stencil.compare = " << formatStencilCompare(stencil.compare) << ";\n";
    if (stencil.ref != defaults.ref)
        code << ind << target << ".stencil.ref = " << formatInt(stencil.ref) << ";\n";
    if (stencil.readMask != defaults.readMask)
        code << ind << target << ".stencil.readMask = " << formatInt(stencil.readMask) << ";\n";
    if (stencil.writeMask != defaults.writeMask)
        code << ind << target << ".stencil.writeMask = " << formatInt(stencil.writeMask) << ";\n";
    if (stencil.pass != defaults.pass)
        code << ind << target << ".stencil.pass = " << formatStencilOp(stencil.pass) << ";\n";
    if (stencil.fail != defaults.fail)
        code << ind << target << ".stencil.fail = " << formatStencilOp(stencil.fail) << ";\n";
    if (stencil.depthFail != defaults.depthFail)
        code << ind << target << ".stencil.depthFail = " << formatStencilOp(stencil.depthFail) << ";\n";
    if (colorWrite != -1)
        code << ind << target << ".colorWrite = " << formatInt(colorWrite) << ";\n";
    return code.str();
}

std::string editor::Factory::formatMaterialPass(int indentSpaces, const MaterialPass& pass, const std::string& target, const fs::path& projectPath) {
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    if (!pass.shader.empty())
        code << ind << target << ".shader = " << formatString(pass.shader) << ";\n";
    if (!pass.shaderParams.empty())
        code << ind << target << ".shaderParams = " << formatShaderUniforms(pass.shaderParams) << ";\n";
    code << formatShaderTextures(indentSpaces, pass.shaderTextures, target + ".shaderTextures", projectPath);
    if (!pass.shaderFeatures.empty())
        code << ind << target << ".shaderFeatures = " << formatShaderFeatures(pass.shaderFeatures) << ";\n";
    if (pass.blendMode != MaterialBlendMode::AUTO)
        code << ind << target << ".blendMode = " << formatBlendMode(pass.blendMode) << ";\n";
    if (pass.cullMode != MaterialCullMode::AUTO)
        code << ind << target << ".cullMode = " << formatCullMode(pass.cullMode) << ";\n";
    if (pass.depthWrite != MaterialDepthWrite::AUTO)
        code << ind << target << ".depthWrite = " << formatDepthWrite(pass.depthWrite) << ";\n";
    if (pass.depthTest != MaterialDepthTest::AUTO)
        code << ind << target << ".depthTest = " << formatDepthTest(pass.depthTest) << ";\n";
    if (pass.depthOffsetFactor != 0.0f)
        code << ind << target << ".depthOffsetFactor = " << formatFloat(pass.depthOffsetFactor) << ";\n";
    if (pass.depthOffsetUnits != 0.0f)
        code << ind << target << ".depthOffsetUnits = " << formatFloat(pass.depthOffsetUnits) << ";\n";
    code << formatStencilAndColorWrite(indentSpaces, pass.stencil, pass.colorWrite, target);
    if (pass.order != MaterialPassOrder::AFTER)
        code << ind << target << ".order = MaterialPassOrder::BEFORE;\n";
    return code.str();
}

std::string editor::Factory::formatMaterialPbr(int indentSpaces, const Material& material, const std::string& target, const fs::path& projectPath) {
    const std::string ind = indentation(indentSpaces);
    std::ostringstream code;
    if (material.emissiveStrength != 1.0f)
        code << ind << target << ".emissiveStrength = " << formatFloat(material.emissiveStrength) << ";\n";
    if (material.pbr == MaterialPbrExtensions())
        return code.str();
    static const MaterialPbrExtensions defaults;
    MaterialPbrExtensions& values = const_cast<MaterialPbrExtensions&>(material.pbr.get());
    MaterialPbrExtensions& defaultValues = const_cast<MaterialPbrExtensions&>(defaults);
    // the struct's members carry the table's names (MaterialPbr::getFields)
    for (const PbrField& field : MaterialPbr::getFields()) {
        const std::string member = target + ".pbr.edit()." + MaterialPbr::getLobe(field.lobe).name + "." + field.name;
        const void* value = field.ref(values);
        const void* def = field.ref(defaultValues);
        switch (field.type) {
            case PbrFieldType::Bool:
                if (*(const bool*)value != *(const bool*)def) code << ind << member << " = " << formatBool(*(const bool*)value) << ";\n";
                break;
            case PbrFieldType::Float:
                if (*(const float*)value != *(const float*)def && std::isfinite(*(const float*)value))
                    code << ind << member << " = " << formatFloat(*(const float*)value) << ";\n";
                break;
            case PbrFieldType::Vector2:
                if (*(const Vector2*)value != *(const Vector2*)def) code << ind << member << " = " << formatVector2(*(const Vector2*)value) << ";\n";
                break;
            case PbrFieldType::Color:
                if (*(const Vector3*)value != *(const Vector3*)def) code << ind << member << " = " << formatVector3(*(const Vector3*)value) << ";\n";
                break;
            case PbrFieldType::Texture:
                code << formatTexture(indentSpaces, *(const Texture*)value, member, projectPath);
                break;
            case PbrFieldType::TexCoord:
                if (*(const int*)value != *(const int*)def) code << ind << member << " = " << formatInt(*(const int*)value) << ";\n";
                break;
        }
    }
    return code.str();
}

std::string editor::Factory::formatMaterialShaderLibrary(int indentSpaces, const Material& material, const std::string& target, const fs::path& projectPath) {
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    if (!material.shader.empty())
        code << ind << target << ".shader = " << formatString(material.shader) << ";\n";
    if (!material.shaderParams.empty())
        code << ind << target << ".shaderParams = " << formatShaderUniforms(material.shaderParams) << ";\n";
    code << formatShaderTextures(indentSpaces, material.shaderTextures, target + ".shaderTextures", projectPath);
    if (!material.shaderFeatures.empty())
        code << ind << target << ".shaderFeatures = " << formatShaderFeatures(material.shaderFeatures) << ";\n";
    if (!material.shaderGlobalBindings.empty())
        code << ind << target << ".shaderGlobalBindings = " << formatShaderGlobalBindings(material.shaderGlobalBindings) << ";\n";
    if (material.blendMode != MaterialBlendMode::AUTO)
        code << ind << target << ".blendMode = " << formatBlendMode(material.blendMode) << ";\n";
    if (material.cullMode != MaterialCullMode::AUTO)
        code << ind << target << ".cullMode = " << formatCullMode(material.cullMode) << ";\n";
    if (material.depthWrite != MaterialDepthWrite::AUTO)
        code << ind << target << ".depthWrite = " << formatDepthWrite(material.depthWrite) << ";\n";
    if (material.depthTest != MaterialDepthTest::AUTO)
        code << ind << target << ".depthTest = " << formatDepthTest(material.depthTest) << ";\n";
    if (material.depthOffsetFactor != 0.0f)
        code << ind << target << ".depthOffsetFactor = " << formatFloat(material.depthOffsetFactor) << ";\n";
    if (material.depthOffsetUnits != 0.0f)
        code << ind << target << ".depthOffsetUnits = " << formatFloat(material.depthOffsetUnits) << ";\n";
    if (material.sortPriority != 0)
        code << ind << target << ".sortPriority = " << formatInt(material.sortPriority) << ";\n";
    if (material.lighting != MaterialLighting::AUTO)
        code << ind << target << ".lighting = " << formatMaterialLighting(material.lighting) << ";\n";
    // phase 2: stencil, colour write and the extra pass
    code << formatStencilAndColorWrite(indentSpaces, material.stencil, material.colorWrite, target);
    code << formatMaterialPass(indentSpaces, material.extraPass, target + ".extraPass.edit()", projectPath);
    return code.str();
}

std::string editor::Factory::formatComponentShaderLibrary(int indentSpaces, const std::vector<std::pair<std::string, Texture>>& textures,
        const std::vector<std::string>& features, const MaterialBlendMode* blendMode, const MaterialDepthWrite* depthWrite,
        const std::string& target, const fs::path& projectPath) {
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << formatShaderTextures(indentSpaces, textures, target + ".shaderTextures", projectPath);
    if (!features.empty())
        code << ind << target << ".shaderFeatures = " << formatShaderFeatures(features) << ";\n";
    if (blendMode && *blendMode != MaterialBlendMode::AUTO)
        code << ind << target << ".blendMode = " << formatBlendMode(*blendMode) << ";\n";
    if (depthWrite && *depthWrite != MaterialDepthWrite::AUTO)
        code << ind << target << ".depthWrite = " << formatDepthWrite(*depthWrite) << ";\n";
    return code.str();
}

std::string editor::Factory::formatRect(const Rect& r) {
    return "Rect(" + formatFloat(r.getX()) + ", " + formatFloat(r.getY()) + ", " + formatFloat(r.getWidth()) + ", " + formatFloat(r.getHeight()) + ")";
}

std::string editor::Factory::formatPrimitiveType(PrimitiveType type) {
    return formatEnum(reflect::enums::kPrimitiveType, static_cast<int>(type));
}

std::string editor::Factory::formatEnum(const reflect::EnumDesc& desc, int value) {
    // the C++ name of the value, of the table's fallback when the value is not one of its own
    if (const reflect::EnumItem* item = reflect::enumItem(desc, value)) return item->cppName;
    const reflect::EnumItem* fallback = reflect::enumItem(desc, desc.fallback);
    return fallback ? fallback->cppName : std::string();
}

std::string editor::Factory::formatEaseType(EaseType type) {
    return formatEnum(reflect::enums::kEaseType, static_cast<int>(type));
}

std::string editor::Factory::formatEase(const Ease& ease) {
    return "Ease(" + formatEaseType(ease.getType()) + ")";
}

std::string editor::Factory::formatPivotPreset(PivotPreset preset) {
    return formatEnum(reflect::enums::kPivotPreset, static_cast<int>(preset));
}

std::string editor::Factory::formatLightState(LightState state) {
    switch (state) {
        case LightState::OFF: return "LightState::OFF";
        case LightState::ON: return "LightState::ON";
        case LightState::AUTO: return "LightState::AUTO";
        default: return "LightState::AUTO";
    }
}

std::string editor::Factory::formatShadowQuality(ShadowQuality quality) {
    switch (quality) {
        case ShadowQuality::NONE: return "ShadowQuality::NONE";
        case ShadowQuality::LOW: return "ShadowQuality::LOW";
        case ShadowQuality::MEDIUM: return "ShadowQuality::MEDIUM";
        case ShadowQuality::HIGH: return "ShadowQuality::HIGH";
        case ShadowQuality::AUTO: return "ShadowQuality::AUTO";
        case ShadowQuality::ULTRA: return "ShadowQuality::ULTRA";
        default: return "ShadowQuality::AUTO";
    }
}

std::string editor::Factory::formatSceneColorScale(SceneColorScale scale) {
    switch (scale) {
        case SceneColorScale::FULL: return "SceneColorScale::FULL";
        case SceneColorScale::HALF: return "SceneColorScale::HALF";
        default: return "SceneColorScale::HALF";
    }
}

std::string editor::Factory::formatAudioLoadType(AudioLoadType loadType) {
    switch (loadType) {
        case AudioLoadType::Auto: return "AudioLoadType::Auto";
        case AudioLoadType::DecompressOnLoad: return "AudioLoadType::DecompressOnLoad";
        case AudioLoadType::CompressedInMemory: return "AudioLoadType::CompressedInMemory";
        case AudioLoadType::Streaming: return "AudioLoadType::Streaming";
    }
    return "AudioLoadType::Auto";
}

std::string editor::Factory::formatUIEventState(UIEventState state) {
    return formatEnum(reflect::enums::kUIEventState, static_cast<int>(state));
}

std::string editor::Factory::formatScriptType(ScriptType type) {
    return formatEnum(reflect::enums::kScriptType, static_cast<int>(type));
}

std::string editor::Factory::formatQuaternion(const Quaternion& q) {
    return "Quaternion(" + formatFloat(q.w) + ", " + formatFloat(q.x) + ", " + formatFloat(q.y) + ", " + formatFloat(q.z) + ")";
}

std::string editor::Factory::formatEntityState(const std::string& ind, const std::string& entityExpr, const EntityRegistry* registry, Entity entity) {
    std::string code;
    if (!registry->isEntityActive(entity)) {
        code += ind + "scene->setEntityActive(" + entityExpr + ", false);\n";
    }
    std::string tag = registry->getEntityTag(entity);
    if (tag != DEFAULT_ENTITY_TAG) {
        code += ind + "scene->setEntityTag(" + entityExpr + ", " + formatString(tag) + ");\n";
    }
    int layer = registry->getEntityLayer(entity);
    if (layer != 0) {
        code += ind + "scene->setEntityLayer(" + entityExpr + ", " + formatInt(layer) + ");\n";
    }
    return code;
}

std::string editor::Factory::formatBool(bool value) {
    return value ? "true" : "false";
}

std::string editor::Factory::formatFloat(float value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);
    oss << sanitizeFiniteNumber(value) << "f";
    return oss.str();
}

namespace {

    const char* formatLineWidthUnit(LineWidthUnit unit) {
        return unit == LineWidthUnit::Pixels ? "LineWidthUnit::Pixels" : "LineWidthUnit::World";
    }

    const char* formatLineAlignment(LineAlignment alignment) {
        return alignment == LineAlignment::TransformZ ? "LineAlignment::TransformZ" : "LineAlignment::View";
    }

    const char* formatLineTextureMode(LineTextureMode mode) {
        switch (mode) {
            case LineTextureMode::Tile: return "LineTextureMode::Tile";
            case LineTextureMode::DistributePerSegment: return "LineTextureMode::DistributePerSegment";
            case LineTextureMode::RepeatPerSegment: return "LineTextureMode::RepeatPerSegment";
            default: return "LineTextureMode::Stretch";
        }
    }

    // the ribbon settings Line and Trail Renderers share, each away from its default
    template<typename T>
    void formatRibbonFields(std::ostringstream& code, const std::string& ind, const std::string& var, const T& component, const T& defaults) {
        using editor::Factory;
        if (component.widthMultiplier != defaults.widthMultiplier)
            code << ind << var << ".widthMultiplier = " << Factory::formatFloat(component.widthMultiplier) << ";\n";
        if (component.widthCurve != defaults.widthCurve)
            code << ind << var << ".widthCurve = " << Factory::formatCurve(component.widthCurve) << ";\n";
        if (component.widthUnit != defaults.widthUnit)
            code << ind << var << ".widthUnit = " << formatLineWidthUnit(component.widthUnit) << ";\n";
        if (component.colorGradient != defaults.colorGradient)
            code << ind << var << ".colorGradient = " << Factory::formatGradient(component.colorGradient) << ";\n";
        if (component.numCornerVertices != defaults.numCornerVertices)
            code << ind << var << ".numCornerVertices = " << std::to_string(component.numCornerVertices) << ";\n";
        if (component.numCapVertices != defaults.numCapVertices)
            code << ind << var << ".numCapVertices = " << std::to_string(component.numCapVertices) << ";\n";
        if (component.alignment != defaults.alignment)
            code << ind << var << ".alignment = " << formatLineAlignment(component.alignment) << ";\n";
        if (component.textureMode != defaults.textureMode)
            code << ind << var << ".textureMode = " << formatLineTextureMode(component.textureMode) << ";\n";
        if (component.textureScale != defaults.textureScale)
            code << ind << var << ".textureScale = Vector2(" << Factory::formatFloat(component.textureScale.x) << ", " << Factory::formatFloat(component.textureScale.y) << ");\n";
        if (component.sortPriority != defaults.sortPriority)
            code << ind << var << ".sortPriority = " << std::to_string(component.sortPriority) << ";\n";
    }

}

std::string editor::Factory::formatCurve(const Curve& curve) {
    return curvecodec::curveCppLiteral(curve);
}

std::string editor::Factory::formatGradient(const Gradient& gradient) {
    return curvecodec::gradientCppLiteral(gradient);
}

std::string editor::Factory::formatPositiveFloat(float value) {
    return formatFloat(std::isfinite(value) && value > 0.0f ? value : 1.0f);
}

std::string editor::Factory::formatDouble(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);
    oss << sanitizeFiniteNumber(value);
    return oss.str();
}

std::string editor::Factory::formatInt(int value) {
    return std::to_string(value);
}

std::string editor::Factory::formatEntity(Entity entity, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (entityVarNames && entityVarNames->count(entity)) {
        return entityVarNames->at(entity);
    }
    if (entity == NULL_ENTITY) return "NULL_ENTITY";
    return formatUInt(entity);
}

std::string editor::Factory::formatUInt(unsigned int value) {
    return std::to_string(value);
}

// culling masks read best as hex: 0xFFFFFFFFu is Everything
std::string editor::Factory::formatLayerMask(uint32_t mask) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "0x%08Xu", (unsigned int)mask);
    return buffer;
}

// Escape one byte into a C++ string or char literal. Non-ASCII stays raw in
// strings for readability, but a lone high byte is not a valid char literal.
static void appendCppEscape(std::string& out, unsigned char c, bool charLiteral) {
    const unsigned char quote = charLiteral ? '\'' : '"';
    if (c == quote || c == '\\') {
        out.push_back('\\');
        out.push_back(static_cast<char>(c));
        return;
    }

    switch (c) {
        case '\n': out += "\\n"; return;
        case '\r': out += "\\r"; return;
        case '\t': out += "\\t"; return;
    }

    if (c < 0x20 || c == 0x7F || (charLiteral && c > 0x7F)) {
        // Always three digits, so a following 0-7 is not absorbed.
        out.push_back('\\');
        out.push_back(static_cast<char>('0' + ((c >> 6) & 0x07)));
        out.push_back(static_cast<char>('0' + ((c >> 3) & 0x07)));
        out.push_back(static_cast<char>('0' + (c & 0x07)));
        return;
    }

    out.push_back(static_cast<char>(c));
}

std::string editor::Factory::formatString(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (unsigned char c : value) {
        appendCppEscape(result, c, false);
    }
    result.push_back('"');
    return result;
}

std::string editor::Factory::formatChar(char value) {
    std::string result;
    result.push_back('\'');
    appendCppEscape(result, static_cast<unsigned char>(value), true);
    result.push_back('\'');
    return result;
}

std::string editor::Factory::formatAttributeType(AttributeType type) {
    switch (type) {
        case AttributeType::INDEX: return "AttributeType::INDEX";
        case AttributeType::POSITION: return "AttributeType::POSITION";
        case AttributeType::TEXCOORD1: return "AttributeType::TEXCOORD1";
        case AttributeType::TEXCOORD2: return "AttributeType::TEXCOORD2";
        case AttributeType::NORMAL: return "AttributeType::NORMAL";
        case AttributeType::TANGENT: return "AttributeType::TANGENT";
        case AttributeType::COLOR: return "AttributeType::COLOR";
        case AttributeType::POINTSIZE: return "AttributeType::POINTSIZE";
        case AttributeType::POINTROTATION: return "AttributeType::POINTROTATION";
        case AttributeType::TEXTURERECT: return "AttributeType::TEXTURERECT";
        case AttributeType::BONEWEIGHTS: return "AttributeType::BONEWEIGHTS";
        case AttributeType::BONEIDS: return "AttributeType::BONEIDS";
        case AttributeType::MORPHTARGET0: return "AttributeType::MORPHTARGET0";
        case AttributeType::MORPHTARGET1: return "AttributeType::MORPHTARGET1";
        case AttributeType::MORPHTARGET2: return "AttributeType::MORPHTARGET2";
        case AttributeType::MORPHTARGET3: return "AttributeType::MORPHTARGET3";
        case AttributeType::MORPHTARGET4: return "AttributeType::MORPHTARGET4";
        case AttributeType::MORPHTARGET5: return "AttributeType::MORPHTARGET5";
        case AttributeType::MORPHTARGET6: return "AttributeType::MORPHTARGET6";
        case AttributeType::MORPHTARGET7: return "AttributeType::MORPHTARGET7";
        case AttributeType::MORPHNORMAL0: return "AttributeType::MORPHNORMAL0";
        case AttributeType::MORPHNORMAL1: return "AttributeType::MORPHNORMAL1";
        case AttributeType::MORPHNORMAL2: return "AttributeType::MORPHNORMAL2";
        case AttributeType::MORPHNORMAL3: return "AttributeType::MORPHNORMAL3";
        case AttributeType::MORPHTANGENT0: return "AttributeType::MORPHTANGENT0";
        case AttributeType::MORPHTANGENT1: return "AttributeType::MORPHTANGENT1";
        case AttributeType::INSTANCEMATRIXCOL1: return "AttributeType::INSTANCEMATRIXCOL1";
        case AttributeType::INSTANCEMATRIXCOL2: return "AttributeType::INSTANCEMATRIXCOL2";
        case AttributeType::INSTANCEMATRIXCOL3: return "AttributeType::INSTANCEMATRIXCOL3";
        case AttributeType::INSTANCEMATRIXCOL4: return "AttributeType::INSTANCEMATRIXCOL4";
        case AttributeType::INSTANCECOLOR: return "AttributeType::INSTANCECOLOR";
        case AttributeType::INSTANCETEXTURERECT: return "AttributeType::INSTANCETEXTURERECT";
        case AttributeType::TERRAINNODEPOSITION: return "AttributeType::TERRAINNODEPOSITION";
        case AttributeType::TERRAINNODESIZE: return "AttributeType::TERRAINNODESIZE";
        case AttributeType::TERRAINNODERANGE: return "AttributeType::TERRAINNODERANGE";
        case AttributeType::TERRAINNODERESOLUTION: return "AttributeType::TERRAINNODERESOLUTION";
        case AttributeType::SMOOTHNORMAL: return "AttributeType::SMOOTHNORMAL";
        case AttributeType::PARTICLECORNER: return "AttributeType::PARTICLECORNER";
        case AttributeType::PARTICLEPOSITION: return "AttributeType::PARTICLEPOSITION";
        case AttributeType::PARTICLECOLOR: return "AttributeType::PARTICLECOLOR";
        case AttributeType::PARTICLESIZE: return "AttributeType::PARTICLESIZE";
        case AttributeType::PARTICLEROTATIONFRAME: return "AttributeType::PARTICLEROTATIONFRAME";
        case AttributeType::PARTICLEVELOCITY: return "AttributeType::PARTICLEVELOCITY";
        case AttributeType::PARTICLEROTATION: return "AttributeType::PARTICLEROTATION";
        case AttributeType::PARTICLECUSTOM: return "AttributeType::PARTICLECUSTOM";
        case AttributeType::VAT_INDEX: return "AttributeType::VAT_INDEX";
        case AttributeType::INSTANCEVAT0: return "AttributeType::INSTANCEVAT0";
        case AttributeType::INSTANCEVAT1: return "AttributeType::INSTANCEVAT1";
        case AttributeType::LINETANGENTIN: return "AttributeType::LINETANGENTIN";
        case AttributeType::LINETANGENTOUT: return "AttributeType::LINETANGENTOUT";
        case AttributeType::LINEPARAMS: return "AttributeType::LINEPARAMS";
        default: return "AttributeType::POSITION";
    }
}

std::string editor::Factory::formatAttributeDataType(AttributeDataType type) {
    switch (type) {
        case AttributeDataType::BYTE: return "AttributeDataType::BYTE";
        case AttributeDataType::UNSIGNED_BYTE: return "AttributeDataType::UNSIGNED_BYTE";
        case AttributeDataType::SHORT: return "AttributeDataType::SHORT";
        case AttributeDataType::UNSIGNED_SHORT: return "AttributeDataType::UNSIGNED_SHORT";
        case AttributeDataType::INT: return "AttributeDataType::INT";
        case AttributeDataType::UNSIGNED_INT: return "AttributeDataType::UNSIGNED_INT";
        case AttributeDataType::FLOAT: return "AttributeDataType::FLOAT";
        case AttributeDataType::HALF: return "AttributeDataType::HALF";
        default: return "AttributeDataType::FLOAT";
    }
}

// the PostProcessingSettings fields away from their defaults, one assignment each
std::string editor::Factory::formatPostProcessing(int indentSpaces, const PostProcessingSettings& settings, const std::string& variableName) {
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    static const PostProcessingSettings defaults;
    using FieldType = PostProcessingSettings::FieldType;
    for (const PostProcessingSettings::Field& field : PostProcessingSettings::getFields()) {
        const void* value = settings.getFieldRef(field);
        const void* def = defaults.getFieldRef(field);
        std::string text;
        switch (field.type) {
            case FieldType::Bool:
                if (*(const bool*)value != *(const bool*)def) text = formatBool(*(const bool*)value);
                break;
            case FieldType::Int:
                if (*(const int*)value != *(const int*)def) text = std::to_string(*(const int*)value);
                break;
            case FieldType::Float:
                if (*(const float*)value != *(const float*)def) text = formatFloat(*(const float*)value);
                break;
            case FieldType::Vector3:
                if (*(const Vector3*)value != *(const Vector3*)def) text = formatVector3(*(const Vector3*)value);
                break;
            case FieldType::String:
                if (*(const std::string*)value != *(const std::string*)def) text = formatString(*(const std::string*)value);
                break;
            case FieldType::Asset:
                // the game loads by path (an AssetRef assigned a path)
                if (((const AssetRef*)value)->path != ((const AssetRef*)def)->path) text = formatString(((const AssetRef*)value)->path);
                break;
            case FieldType::Enum:
                if (*(const int*)value != *(const int*)def)
                    text = std::string(field.enumType) + "::" + field.enumValues[*(const int*)value];
                break;
        }
        if (!text.empty())
            code << ind << variableName << "." << field.name << " = " << text << ";\n";
    }
    return code.str();
}

std::string editor::Factory::formatTextureFilter(TextureFilter filter) {
    return formatEnum(reflect::enums::kTextureFilter, static_cast<int>(filter));
}

std::string editor::Factory::formatTextureWrap(TextureWrap wrap) {
    return formatEnum(reflect::enums::kTextureWrap, static_cast<int>(wrap));
}

std::string editor::Factory::formatTextureAlphaBorder(TextureAlphaBorder alphaBorder) {
    return formatEnum(reflect::enums::kTextureAlphaBorder, static_cast<int>(alphaBorder));
}

std::string editor::Factory::formatScriptPropertyType(ScriptPropertyType type) {
    switch (type) {
        case ScriptPropertyType::Bool: return "ScriptPropertyType::Bool";
        case ScriptPropertyType::Int: return "ScriptPropertyType::Int";
        case ScriptPropertyType::Float: return "ScriptPropertyType::Float";
        case ScriptPropertyType::String: return "ScriptPropertyType::String";
        case ScriptPropertyType::Vector2: return "ScriptPropertyType::Vector2";
        case ScriptPropertyType::Vector3: return "ScriptPropertyType::Vector3";
        case ScriptPropertyType::Vector4: return "ScriptPropertyType::Vector4";
        case ScriptPropertyType::Color3: return "ScriptPropertyType::Color3";
        case ScriptPropertyType::Color4: return "ScriptPropertyType::Color4";
        case ScriptPropertyType::EntityReference: return "ScriptPropertyType::EntityReference";
        case ScriptPropertyType::Prefab: return "ScriptPropertyType::Prefab";
        case ScriptPropertyType::Asset: return "ScriptPropertyType::Asset";
        default: return "ScriptPropertyType::Bool";
    }
}

std::string editor::Factory::formatScriptPropertyValue(const ScriptPropertyValue& value, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (std::holds_alternative<bool>(value)) {
        return formatBool(std::get<bool>(value));
    } else if (std::holds_alternative<int>(value)) {
        return formatInt(std::get<int>(value));
    } else if (std::holds_alternative<float>(value)) {
        return formatFloat(std::get<float>(value));
    } else if (std::holds_alternative<std::string>(value)) {
        // not a bare literal: AssetRef, also an alternative, converts from a C string too
        return "std::string(" + formatString(std::get<std::string>(value)) + ")";
    } else if (std::holds_alternative<Vector2>(value)) {
        return formatVector2(std::get<Vector2>(value));
    } else if (std::holds_alternative<Vector3>(value)) {
        return formatVector3(std::get<Vector3>(value));
    } else if (std::holds_alternative<Vector4>(value)) {
        return formatVector4(std::get<Vector4>(value));
    } else if (std::holds_alternative<EntityReference>(value)) {
        const auto& ref = std::get<EntityReference>(value);
        // only a same-scene reference can point at a prefab member
        return "EntityReference{Entity(" + formatEntity(ref.entity, ref.sceneId == 0 ? entityVarNames : nullptr) + "), " + std::to_string(ref.sceneId) + "}";
    } else if (std::holds_alternative<PrefabAsset>(value)) {
        // found by GUID in PrefabManager, as in Play
        const auto& prefab = std::get<PrefabAsset>(value);
        return "PrefabAsset(" + formatString(prefab.getGuid()) + ", " + formatString(prefab.getPath()) + ")";
    } else if (std::holds_alternative<AssetRef>(value)) {
        // the path resolved at export; the GUID for Asset.guid / AssetRef.guid in scripts
        const auto& ref = std::get<AssetRef>(value);
        return "AssetRef(" + formatString(ref.path) + ", Guid::parse(" + formatString(ref.guid.str()) + "))";
    }
    return "ScriptPropertyValue{}";
}

std::string editor::Factory::formatAtlasSampler(int indentSpaces, const Texture& texture, const std::string& variableName) {
    const YAML::Node sampler = Stream::encodeAtlasSampler(texture);
    if (!sampler.IsMap()) return "";
    std::ostringstream code;
    std::string ind = indentation(indentSpaces);
    if (texture.getAlphaBorder() != TextureAlphaBorder::AUTO) {
        code << ind << variableName << ".setAlphaBorder(" << formatTextureAlphaBorder(texture.getAlphaBorder()) << ");\n";
    }
    code << ind << variableName << ".setMinFilter(" << formatTextureFilter(texture.getMinFilter()) << ");\n";
    code << ind << variableName << ".setMagFilter(" << formatTextureFilter(texture.getMagFilter()) << ");\n";
    code << ind << variableName << ".setWrapU(" << formatTextureWrap(texture.getWrapU()) << ");\n";
    code << ind << variableName << ".setWrapV(" << formatTextureWrap(texture.getWrapV()) << ");\n";
    return code.str();
}

std::string editor::Factory::formatTexture(int indentSpaces, const Texture& texture, const std::string& variableName, const fs::path& projectPath) {
    if (!isTextureExported(texture)) return "";
    std::ostringstream code;
    std::string ind = indentation(indentSpaces);

    const bool isCube = texture.isCubeMap() || (texture.getNumFaces() == 6);
    if (isCube) {
        bool hasAnyNonZeroFace = false;
        for (int f = 1; f < 6; f++) {
            if (!texture.getPath((size_t)f).empty()) {
                hasAnyNonZeroFace = true;
                break;
            }
        }

        const std::string p0 = texture.getPath(0);
        const bool singleFileCube = !p0.empty() && !hasAnyNonZeroFace;
        if (singleFileCube) {
            code << ind << variableName << ".setCubeMap(" << formatString(p0) << ");\n";
        } else {
            // Face indices (engine order): 0=Right(+X), 1=Left(-X), 2=Top(+Y), 3=Bottom(-Y), 4=Front(+Z), 5=Back(-Z)
            const std::string p1 = texture.getPath(1);
            const std::string p2 = texture.getPath(2);
            const std::string p3 = texture.getPath(3);
            const std::string p4 = texture.getPath(4);
            const std::string p5 = texture.getPath(5);

            // setCubePaths takes OpenGL-style naming: (front, back, left, right, up, down)
            code << ind << variableName << ".setCubePaths(";
            code << formatString(p4) << ", " << formatString(p5) << ", ";
            code << formatString(p1) << ", " << formatString(p0) << ", ";
            code << formatString(p2) << ", " << formatString(p3);
            code << ");\n";
        }
    } else {
        const std::string p0 = texture.getPath(0);
        if (!p0.empty()) {
            code << ind << variableName << ".setPath(" << formatString(p0) << ");\n";
        } else {
            const std::string textureId = texture.getId();
            if (!textureId.empty()) {
                // Keep only the texture id reference. Model-backed GLTF textures
                // are rebuilt from the source model at runtime, and other
                // in-memory textures are no longer exported to disk.
                code << ind << variableName << ".setId(" << formatString(textureId) << ");\n";
            }
        }
    }

    // After setPath, which resets the scale to 1.0
    if (texture.getSvgScale() != 1.0f) {
        code << ind << variableName << ".setSvgScale(" << formatFloat(texture.getSvgScale()) << ");\n";
    }
    if (texture.getAlphaBorder() != TextureAlphaBorder::AUTO) {
        code << ind << variableName << ".setAlphaBorder(" << formatTextureAlphaBorder(texture.getAlphaBorder()) << ");\n";
    }

    code << ind << variableName << ".setMinFilter(" << formatTextureFilter(texture.getMinFilter()) << ");\n";
    code << ind << variableName << ".setMagFilter(" << formatTextureFilter(texture.getMagFilter()) << ");\n";
    code << ind << variableName << ".setWrapU(" << formatTextureWrap(texture.getWrapU()) << ");\n";
    code << ind << variableName << ".setWrapV(" << formatTextureWrap(texture.getWrapV()) << ");\n";
    // after the path setters, which turn it back on; kept for textures whose pixels are read after loading
    if (!texture.isReleaseDataAfterLoad() && !texture.isFramebuffer()) {
        code << ind << variableName << ".setReleaseDataAfterLoad(false);\n";
    }

    return code.str();
}

std::string editor::Factory::formatBufferContents(const std::string& ind, const std::string& target, const Buffer& buffer, const Buffer& defaults) {
    if (buffer.getSize() == 0 || !buffer.getData()) return "";
    std::ostringstream code;
    code << ind << target << ".clearAll();\n";
    writeBufferSettings(code, ind, target, buffer, defaults);
    for (auto const& [type, attr] : buffer.getAttributes()) {
        code << ind << target << ".Buffer::addAttribute(" << formatAttributeType(type) << ", Attribute("
             << formatAttributeDataType(attr.getDataType()) << ", " << formatString(attr.getBufferName()) << ", "
             << attr.getElements() << ", " << attr.getOffset() << ", " << attr.getCount() << ", "
             << formatBool(attr.getNormalized()) << ", " << formatBool(attr.getPerInstance()) << "));\n";
    }
    if (const InterleavedBuffer* interleaved = dynamic_cast<const InterleavedBuffer*>(&buffer)) {
        code << ind << target << ".setVertexSize(" << formatUInt(interleaved->getVertexSize()) << ");\n";
    }
    code << ind << "{\n";
    writeStaticBytes(code, ind, "const ", buffer.getData(), buffer.getSize());
    code << ind << "    " << target << ".importData(data, sizeof(data));\n";
    code << ind << "}\n";
    code << ind << target << ".setStride(" << formatUInt(buffer.getStride()) << ");\n";
    code << ind << target << ".setCount(" << formatUInt(buffer.getCount()) << ");\n";
    return code.str();
}

std::string editor::Factory::beginHeapComponentCode(std::ostringstream& code, const std::string& ind, const std::string& componentType, const std::string& varName) {
    // A component holds render objects and materials (kilobytes for a mesh), and a scene
    // factory makes many: they are built on the heap, not as automatic variables.
    const std::string storageName = varName + "Storage";
    code << ind << "auto " << storageName << " = std::make_unique<" << componentType << ">();\n";
    code << ind << componentType << "& " << varName << " = *" << storageName << ";\n";
    return "std::move(*" + storageName + ")";
}

void editor::Factory::addComponentCode(std::ostringstream& code, const std::string& ind, const std::string& sceneName, const std::string& entityName, Entity entity, const std::string& componentType, const std::string& varName, bool assignExisting) {
    if (!sceneName.empty()) {
        std::string entityRef = entityName.empty() ? std::to_string(entity) : entityName;
        if (sceneName == "scene") {
            if (assignExisting)
                code << ind << sceneName << "->getComponent<" << componentType << ">(" << entityRef << ") = " << varName << ";\n";
            else
                code << ind << sceneName << "->addComponent<" << componentType << ">(" << entityRef << ", " << varName << ");\n";
        } else {
            if (assignExisting)
                code << ind << sceneName << ".getComponent<" << componentType << ">(" << entityRef << ") = " << varName << ";\n";
            else
                code << ind << sceneName << ".addComponent<" << componentType << ">(" << entityRef << ", " << varName << ");\n";
        }
    }
}

// Exports can drop a physics backend, whose components are then never registered
std::string editor::Factory::guardPhysicsCode(const std::string& code, const char* backendMacro) {
    if (code.empty()) return code;
    return "#ifdef " + std::string(backendMacro) + "\n" + code + "#endif // " + backendMacro + "\n";
}

// Code naming a component type, guarded as its creation is
std::string editor::Factory::guardComponentCode(ComponentType type, const std::string& code) {
    switch (type) {
        case ComponentType::Rigidbody2DComponent:
        case ComponentType::Joint2DComponent:
        case ComponentType::CharacterController2DComponent:
            return guardPhysicsCode(code, "JASPERENGINE_PHYSICS_2D");
        case ComponentType::RigidbodyComponent:
        case ComponentType::Joint3DComponent:
        case ComponentType::CharacterControllerComponent:
            return guardPhysicsCode(code, "JASPERENGINE_PHYSICS_3D");
        default:
            return code;
    }
}

std::string editor::Factory::createTransform(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool skipParent, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<Transform>(entity)) return "";
    Transform& transform = scene->getComponent<Transform>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "Transform transform;\n";
    code << ind << "transform.position = " << formatVector3(transform.position) << ";\n";
    code << ind << "transform.rotation = " << formatQuaternion(transform.rotation) << ";\n";
    code << ind << "transform.scale = " << formatVector3(transform.scale) << ";\n";
    code << ind << "transform.visible = " << formatBool(transform.visible) << ";\n";
    code << ind << "transform.billboard = " << formatBool(transform.billboard) << ";\n";
    code << ind << "transform.fakeBillboard = " << formatBool(transform.fakeBillboard) << ";\n";
    code << ind << "transform.cylindricalBillboard = " << formatBool(transform.cylindricalBillboard) << ";\n";
    code << ind << "transform.billboardRotation = " << formatQuaternion(transform.billboardRotation) << ";\n";
    if (!skipParent) {
        code << ind << "transform.parent = " << formatEntity(transform.parent, entityVarNames) << ";\n";
    }
    addComponentCode(code, ind, sceneName, entityName, entity, "Transform", "transform", assignExisting);
    return code.str();
}

std::string editor::Factory::createMeshComponent(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<MeshComponent>(entity)) return "";
    MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    // MeshComponent contains large fixed-capacity arrays in standalone builds.
    // Keep it off the scene factory's stack and move it directly into the ECS.
    const std::string meshValue = beginHeapComponentCode(code, ind, "MeshComponent", "mesh");

    // A model's load and a Sprite, Tilemap, Mesh Polygon, Terrain or Spline Extrude make their
    // geometry when they run, so it is not emitted: as in the scene file (Stream::encodeComponents),
    // it exists here only if a frame made it before this save. An empty model filename is no
    // geometry source and keeps its inline data.
    const bool hasModel = Stream::isModelBackedMesh(entity, scene, scene->getSignature(entity));
    const bool hasGeneratedGeometry = Stream::isGeneratedMesh(scene, scene->getSignature(entity));
    const bool hasRuntimeGeometrySource = hasModel || hasGeneratedGeometry;
    const unsigned int numSubmeshes = Stream::savedSubmeshCount(mesh, scene, scene->getSignature(entity));

    // receiveLights picks the lit or the unlit shader variant: the export only ships the
    // variants the editor's scene used, so a mesh generated lit that was unlit there asks
    // for one that was never compiled
    code << ind << "mesh.receiveLights = " << formatBool(mesh.receiveLights) << ";\n";
    code << ind << "mesh.receiveIBL = " << formatBool(mesh.receiveIBL) << ";\n";
    code << ind << "mesh.castShadows = " << formatBool(mesh.castShadows) << ";\n";
    code << ind << "mesh.receiveShadows = " << formatBool(mesh.receiveShadows) << ";\n";
    code << ind << "mesh.shadowsBillboard = " << formatBool(mesh.shadowsBillboard) << ";\n";
    if (mesh.staticShadowCaster)
        code << ind << "mesh.staticShadowCaster = true;\n";
    code << ind << "mesh.lodEnabled = " << formatBool(mesh.lodEnabled) << ";\n";
    code << ind << "mesh.lodBias = " << formatFloat(mesh.lodBias) << ";\n";
    if (!mesh.allowGpuInstancing)
        code << ind << "mesh.allowGpuInstancing = false;\n";
    // the fade settings only away from their defaults (spec S54 R9)
    if (mesh.lodFadeMode != LodFadeMode::CROSS_FADE)
        code << ind << "mesh.lodFadeMode = LodFadeMode::NONE;\n";
    if (!mesh.lodAnimateCrossFading)
        code << ind << "mesh.lodAnimateCrossFading = false;\n";
    if (mesh.lodFadeTransitionWidth != 0.1f)
        code << ind << "mesh.lodFadeTransitionWidth = " << formatFloat(mesh.lodFadeTransitionWidth) << ";\n";
    if (mesh.lodCullBelow != 0.0f)
        code << ind << "mesh.lodCullBelow = " << formatFloat(mesh.lodCullBelow) << ";\n";
    code << ind << "mesh.renderInReflectionProbes = " << formatBool(mesh.renderInReflectionProbes) << ";\n";
    code << ind << "mesh.transparent = " << formatBool(mesh.transparent) << ";\n";
    code << ind << "mesh.autoTransparency = " << formatBool(mesh.autoTransparency) << ";\n";
    code << ind << "mesh.cullingMode = " << formatCullingMode(mesh.cullingMode) << ";\n";
    code << ind << "mesh.windingOrder = " << formatWindingOrder(mesh.windingOrder) << ";\n";
    if (mesh.mirroredWinding)
        code << ind << "mesh.mirroredWinding = true;\n";
    // a model load sets these again from its file
    if (mesh.normAdjustJoint != 1.0f)
        code << ind << "mesh.normAdjustJoint = " << formatFloat(mesh.normAdjustJoint) << ";\n";
    if (mesh.normAdjustWeight != 1.0f)
        code << ind << "mesh.normAdjustWeight = " << formatFloat(mesh.normAdjustWeight) << ";\n";
    for (int w = 0; w < MAX_MORPHTARGETS; w++) {
        if (mesh.morphWeights[w] != 0.0f)
            code << ind << "mesh.morphWeights[" << w << "] = " << formatFloat(mesh.morphWeights[w]) << ";\n";
    }
    // the model's load keeps these weights (zeros too) instead of its file's defaults, as the
    // editor saves them (Stream): weights a load or an edit set, or non-zero ones set before any
    // load, up to the targets they cover (more in the model file: theirs are its defaults)
    const int presentCount = morphWeightsPresentCount(mesh.morphWeights);
    if (mesh.morphWeightsSet || presentCount > 0) {
        code << ind << "mesh.morphWeightsSet = true;\n";
        code << ind << "mesh.morphTargetCount = " << (mesh.morphWeightsSet ? mesh.morphTargetCount : presentCount) << ";\n";
    }
    if (!mesh.customShader.empty())
        code << ind << "mesh.customShader = " << formatString(mesh.customShader) << ";\n";
    if (!mesh.customDepthShader.empty())
        code << ind << "mesh.customDepthShader = " << formatString(mesh.customDepthShader) << ";\n";
    if (!mesh.shaderUniforms.empty())
        code << ind << "mesh.shaderUniforms = " << formatShaderUniforms(mesh.shaderUniforms) << ";\n";
    // Kept for a model too: its load reads the count to find the edited submeshes.
    code << ind << "mesh.numSubmeshes = " << formatUInt(numSubmeshes) << ";\n";
    for (unsigned int s = 0; s < numSubmeshes; s++){
        // A model load rewrites every submesh, so only the edited ones are worth emitting.
        if (hasModel && mesh.submeshes[s].overrideFields == 0){
            continue;
        }

        std::string idx = std::to_string(s);
        code << ind << "mesh.submeshes[" << idx << "].material.name = " << formatString(mesh.submeshes[s].material.name) << ";\n";
        code << ind << "mesh.submeshes[" << idx << "].material.baseColorFactor = " << formatVector4(mesh.submeshes[s].material.baseColorFactor) << ";\n";
        code << ind << "mesh.submeshes[" << idx << "].material.metallicFactor = " << formatFloat(mesh.submeshes[s].material.metallicFactor) << ";\n";
        code << ind << "mesh.submeshes[" << idx << "].material.roughnessFactor = " << formatFloat(mesh.submeshes[s].material.roughnessFactor) << ";\n";
        code << ind << "mesh.submeshes[" << idx << "].material.alphaCutoff = " << formatFloat(mesh.submeshes[s].material.alphaCutoff) << ";\n";
        code << ind << "mesh.submeshes[" << idx << "].material.foliageTransmission = " << formatFloat(mesh.submeshes[s].material.foliageTransmission) << ";\n";
        code << ind << "mesh.submeshes[" << idx << "].material.alphaMode = static_cast<MaterialAlphaMode>(" << static_cast<int>(mesh.submeshes[s].material.alphaMode) << ");\n";
        code << ind << "mesh.submeshes[" << idx << "].material.emissiveFactor = " << formatVector3(mesh.submeshes[s].material.emissiveFactor) << ";\n";

        code << formatTexture(indentSpaces, mesh.submeshes[s].material.baseColorTexture, "mesh.submeshes[" + idx + "].material.baseColorTexture", projectPath);
        code << formatTexture(indentSpaces, mesh.submeshes[s].material.emissiveTexture, "mesh.submeshes[" + idx + "].material.emissiveTexture", projectPath);
        code << formatTexture(indentSpaces, mesh.submeshes[s].material.metallicRoughnessTexture, "mesh.submeshes[" + idx + "].material.metallicRoughnessTexture", projectPath);
        code << formatTexture(indentSpaces, mesh.submeshes[s].material.occlusionTexture, "mesh.submeshes[" + idx + "].material.occlusionTexture", projectPath);
        code << formatTexture(indentSpaces, mesh.submeshes[s].material.normalTexture, "mesh.submeshes[" + idx + "].material.normalTexture", projectPath);

        // The UV set each texture samples: only a model or a linked material sets it, and an
        // overridden texture is put back together with it.
        const Material& submeshMaterial = mesh.submeshes[s].material;
        if (submeshMaterial.baseColorTexCoord != 0)
            code << ind << "mesh.submeshes[" << idx << "].material.baseColorTexCoord = " << formatInt(submeshMaterial.baseColorTexCoord) << ";\n";
        if (submeshMaterial.emissiveTexCoord != 0)
            code << ind << "mesh.submeshes[" << idx << "].material.emissiveTexCoord = " << formatInt(submeshMaterial.emissiveTexCoord) << ";\n";
        if (submeshMaterial.metallicRoughnessTexCoord != 0)
            code << ind << "mesh.submeshes[" << idx << "].material.metallicRoughnessTexCoord = " << formatInt(submeshMaterial.metallicRoughnessTexCoord) << ";\n";
        if (submeshMaterial.occlusionTexCoord != 0)
            code << ind << "mesh.submeshes[" << idx << "].material.occlusionTexCoord = " << formatInt(submeshMaterial.occlusionTexCoord) << ";\n";
        if (submeshMaterial.normalTexCoord != 0)
            code << ind << "mesh.submeshes[" << idx << "].material.normalTexCoord = " << formatInt(submeshMaterial.normalTexCoord) << ";\n";
        // Stochastic Tiling, away from its defaults
        static const Material defaultMaterial = Material();
        if (submeshMaterial.stochasticTiling)
            code << ind << "mesh.submeshes[" << idx << "].material.stochasticTiling = true;\n";
        if (submeshMaterial.stochasticCellSize != defaultMaterial.stochasticCellSize)
            code << ind << "mesh.submeshes[" << idx << "].material.stochasticCellSize = " << formatFloat(submeshMaterial.stochasticCellSize) << ";\n";
        if (submeshMaterial.stochasticBlendSharpness != defaultMaterial.stochasticBlendSharpness)
            code << ind << "mesh.submeshes[" << idx << "].material.stochasticBlendSharpness = " << formatFloat(submeshMaterial.stochasticBlendSharpness) << ";\n";
        if (submeshMaterial.stochasticContrast != defaultMaterial.stochasticContrast)
            code << ind << "mesh.submeshes[" << idx << "].material.stochasticContrast = " << formatFloat(submeshMaterial.stochasticContrast) << ";\n";
        if (submeshMaterial.stochasticRotation != defaultMaterial.stochasticRotation)
            code << ind << "mesh.submeshes[" << idx << "].material.stochasticRotation = " << formatFloat(submeshMaterial.stochasticRotation) << ";\n";
        // shader library: the shader, its values and the render state, when set
        code << formatMaterialShaderLibrary(indentSpaces, submeshMaterial, "mesh.submeshes[" + idx + "].material", projectPath);
        code << formatMaterialPbr(indentSpaces, submeshMaterial, "mesh.submeshes[" + idx + "].material", projectPath);

        code << ind << "mesh.submeshes[" << idx << "].primitiveType = " << formatPrimitiveType(mesh.submeshes[s].primitiveType) << ";\n";
        if (!hasGeneratedGeometry)
            code << ind << "mesh.submeshes[" << idx << "].vertexCount = " << formatUInt(mesh.submeshes[s].vertexCount) << ";\n";
        code << ind << "mesh.submeshes[" << idx << "].faceCulling = " << formatBool(mesh.submeshes[s].faceCulling) << ";\n";
        code << ind << "mesh.submeshes[" << idx << "].textureShadow = " << formatBool(mesh.submeshes[s].textureShadow) << ";\n";
        code << ind << "mesh.submeshes[" << idx << "].textureRect = " << formatRect(mesh.submeshes[s].textureRect) << ";\n";

        // The model load puts these fields back over the ones the file gives the new submesh.
        if (mesh.submeshes[s].overrideFields != 0){
            // the bits past 31 (the extended lobes) need a 64-bit literal
            const uint64_t fields = mesh.submeshes[s].overrideFields;
            code << ind << "mesh.submeshes[" << idx << "].overrideFields = " << (fields > 0xFFFFFFFFull ? std::to_string(fields) + "ull" : formatUInt((unsigned int)fields)) << ";\n";
        }

        if (hasRuntimeGeometrySource){ // what makes the geometry makes the attributes
            continue;
        }

        if (mesh.submeshes[s].hasSkinningNormalization){
            code << ind << "mesh.submeshes[" << idx << "].normAdjustJoint = " << formatFloat(mesh.submeshes[s].normAdjustJoint) << ";\n";
            code << ind << "mesh.submeshes[" << idx << "].normAdjustWeight = " << formatFloat(mesh.submeshes[s].normAdjustWeight) << ";\n";
            code << ind << "mesh.submeshes[" << idx << "].hasSkinningNormalization = true;\n";
        }

        for (auto const& [type, attr] : mesh.submeshes[s].attributes) {
            code << ind << "{\n";
            code << ind << "    Attribute attr;\n";
            code << ind << "    attr.setBufferName(" << formatString(attr.getBufferName()) << ");\n";
            code << ind << "    attr.setDataType(" << formatAttributeDataType(attr.getDataType()) << ");\n";
            code << ind << "    attr.setElements(" << attr.getElements() << ");\n";
            code << ind << "    attr.setOffset(" << attr.getOffset() << ");\n";
            code << ind << "    attr.setCount(" << attr.getCount() << ");\n";
            code << ind << "    attr.setNormalized(" << formatBool(attr.getNormalized()) << ");\n";
            code << ind << "    attr.setPerInstance(" << formatBool(attr.getPerInstance()) << ");\n";
            code << ind << "    mesh.submeshes[" << idx << "].attributes[" << formatAttributeType(type) << "] = attr;\n";
            code << ind << "}\n";
        }
    }

    if (!hasRuntimeGeometrySource && mesh.buffer.getSize() > 0){
        code << ind << "mesh.buffer.clearAll();\n";
        std::map<AttributeType, Attribute> attributes = mesh.buffer.getAttributes();

        std::vector<std::pair<AttributeType, Attribute>> sortedAttributes(attributes.begin(), attributes.end());
        std::sort(sortedAttributes.begin(), sortedAttributes.end(), 
            [](const std::pair<AttributeType, Attribute>& a, const std::pair<AttributeType, Attribute>& b) {
                return a.second.getOffset() < b.second.getOffset();
            });

        for (auto const& [type, val] : sortedAttributes) {
            std::string perInstanceCode = "";
            if (val.getPerInstance())
                perInstanceCode = ", true";
            code << ind << "mesh.buffer.addAttribute(" << formatAttributeType(type) << ", " << val.getElements() << perInstanceCode << ");\n";
            code << ind << "mesh.buffer.getAttribute(" << formatAttributeType(type) << ")->setDataType(" << formatAttributeDataType(val.getDataType()) << ");\n";
            code << ind << "mesh.buffer.getAttribute(" << formatAttributeType(type) << ")->setOffset(" << val.getOffset() << ");\n";
            code << ind << "mesh.buffer.getAttribute(" << formatAttributeType(type) << ")->setCount(" << val.getCount() << ");\n";
            code << ind << "mesh.buffer.getAttribute(" << formatAttributeType(type) << ")->setNormalized(" << formatBool(val.getNormalized()) << ");\n";
            if (!val.getBufferName().empty())
                code << ind << "mesh.buffer.getAttribute(" << formatAttributeType(type) << ")->setBufferName(" << formatString(val.getBufferName()) << ");\n";
        }
        code << ind << "mesh.buffer.setVertexSize(" << formatUInt(mesh.buffer.getVertexSize()) << ");\n";
        writeBufferSettings(code, ind, "mesh.buffer", mesh.buffer, InterleavedBuffer());

        code << ind << "{\n";
        // Raw mesh data can be several megabytes. Keeping the generated array
        // automatic makes create_<scene>() reserve the entire blob on the thread
        // stack and can crash at function entry before any entity is created.
        code << ind << "    static const unsigned char data[] = {";
        unsigned char* bufData = mesh.buffer.getData();
        for(size_t i=0; i<mesh.buffer.getSize(); i++){
                if (i % 50 == 0) code << "\n" << ind << "        ";
                code << "0x" << std::hex << (int)bufData[i] << std::dec << ", ";
        }
        code << "};\n";
        code << ind << "    mesh.buffer.importData(data, sizeof(data));\n";
        code << ind << "}\n";
        code << ind << "mesh.buffer.setStride(" << formatUInt(mesh.buffer.getStride()) << ");\n";
        code << ind << "mesh.buffer.setCount(" << formatUInt(mesh.buffer.getCount()) << ");\n";
    }

    if (!hasRuntimeGeometrySource && mesh.indices.getSize() > 0){
        code << ind << "mesh.indices.clearAll();\n";
        code << ind << "{\n";
        // See the vertex-buffer case above: index data belongs in static storage,
        // not in the generated scene factory's stack frame.
        code << ind << "    static const unsigned char data[] = {";
        unsigned char* idxData = mesh.indices.getData();
        for(size_t i=0; i<mesh.indices.getSize(); i++){
                if (i % 50 == 0) code << "\n" << ind << "        ";
                code << "0x" << std::hex << (int)idxData[i] << std::dec << ", ";
        }
        code << "};\n";
        code << ind << "    mesh.indices.importData(data, sizeof(data));\n";
        code << ind << "}\n";
        if (mesh.indices.getAttribute(AttributeType::INDEX)){
            Attribute* indexAttr = mesh.indices.getAttribute(AttributeType::INDEX);
            code << ind << "mesh.indices.getAttribute(AttributeType::INDEX)->setDataType(" << formatAttributeDataType(indexAttr->getDataType()) << ");\n";
            code << ind << "mesh.indices.getAttribute(AttributeType::INDEX)->setOffset(" << indexAttr->getOffset() << ");\n";
            code << ind << "mesh.indices.getAttribute(AttributeType::INDEX)->setCount(" << indexAttr->getCount() << ");\n";
            code << ind << "mesh.indices.getAttribute(AttributeType::INDEX)->setNormalized(" << formatBool(indexAttr->getNormalized()) << ");\n";
        }
        code << ind << "mesh.indices.setStride(" << formatUInt(mesh.indices.getStride()) << ");\n";
        code << ind << "mesh.indices.setCount(" << formatUInt(mesh.indices.getCount()) << ");\n";
        writeBufferSettings(code, ind, "mesh.indices", mesh.indices, IndexBuffer());
    }

    // External buffers (a model's separate vertex streams, e.g. skinning or morph targets)
    // are saved with a mesh that no model load rebuilds, and its submesh attributes read
    // them by name. ExternalBuffer does not own its bytes, so they stay in static storage.
    if (!hasRuntimeGeometrySource && mesh.numExternalBuffers > 0){
        unsigned int numExternalBuffers = 0;
        for (unsigned int b = 0; b < mesh.numExternalBuffers; b++){
            const ExternalBuffer& eBuffer = mesh.eBuffers[b];
            const std::string target = "mesh.eBuffers[" + std::to_string(b) + "]";
            code << ind << target << ".setName(" << formatString(eBuffer.getName()) << ");\n";
            writeBufferSettings(code, ind, target, eBuffer, ExternalBuffer());
            for (auto const& [type, attr] : eBuffer.getAttributes()) {
                code << ind << target << ".Buffer::addAttribute(" << formatAttributeType(type) << ", Attribute("
                     << formatAttributeDataType(attr.getDataType()) << ", " << formatString(attr.getBufferName()) << ", "
                     << attr.getElements() << ", " << attr.getOffset() << ", " << attr.getCount() << ", "
                     << formatBool(attr.getNormalized()) << ", " << formatBool(attr.getPerInstance()) << "));\n";
            }
            if (eBuffer.getData() && eBuffer.getSize() > 0){
                code << ind << "{\n";
                writeStaticBytes(code, ind, "", eBuffer.getData(), eBuffer.getSize());
                code << ind << "    " << target << ".setData(data, sizeof(data));\n";
                code << ind << "}\n";
            }
            code << ind << target << ".setStride(" << formatUInt(eBuffer.getStride()) << ");\n";
            code << ind << target << ".setCount(" << formatUInt(eBuffer.getCount()) << ");\n";
            numExternalBuffers++;
        }
        code << ind << "mesh.numExternalBuffers = " << numExternalBuffers << ";\n";
    }

    // Saved for a mesh whose geometry is neither loaded from a model nor generated by its
    // component (Stream::encodeComponents); without a skeleton driving them they pose the mesh.
    if (!hasRuntimeGeometrySource){
        bool hasBones = false;
        for (size_t b = 0; b < mesh.bonesMatrix.stored(); b++){
            const Matrix4& bone = mesh.bonesMatrix[b];
            // (the last one always: the palette's length is its index plus one)
            if (bone == Matrix4() && b + 1 < mesh.bonesMatrix.stored())
                continue;
            code << ind << "mesh.bonesMatrix[" << b << "] = " << formatMatrix4(bone) << ";\n";
            hasBones = true;
        }
        if (hasBones)
            code << ind << "mesh.needUpdateBones = true;\n";
    }

    if (!mesh.enabled) code << ind << "mesh.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "MeshComponent", meshValue, assignExisting);
    return code.str();
}

std::string editor::Factory::createUIComponent(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<UIComponent>(entity)) return "";
    UIComponent& ui = scene->getComponent<UIComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "UIComponent ui;\n";
    code << ind << "ui.color = " << formatVector4(ui.color) << ";\n";
    if (!ui.customShader.empty())
        code << ind << "ui.customShader = " << formatString(ui.customShader) << ";\n";
    if (!ui.shaderUniforms.empty())
        code << ind << "ui.shaderUniforms = " << formatShaderUniforms(ui.shaderUniforms) << ";\n";
    if (!ui.shaderGlobalBindings.empty())
        code << ind << "ui.shaderGlobalBindings = " << formatShaderGlobalBindings(ui.shaderGlobalBindings) << ";\n";
    code << formatComponentShaderLibrary(indentSpaces, ui.shaderTextures, ui.shaderFeatures, &ui.blendMode, &ui.depthWrite, "ui", projectPath);
    code << formatStencilAndColorWrite(indentSpaces, ui.stencil, ui.colorWrite, "ui");
    // a Text's texture is its font atlas, which the game builds (Stream::encodeUIComponent)
    if (scene->findComponent<TextComponent>(entity))
        code << formatAtlasSampler(indentSpaces, ui.texture, "ui.texture");
    else
        code << formatTexture(indentSpaces, ui.texture, "ui.texture", projectPath);
    // flipY is worked out from the camera unless automaticFlipY is off
    code << ind << "ui.automaticFlipY = " << formatBool(ui.automaticFlipY) << ";\n";
    if (!ui.automaticFlipY)
        code << ind << "ui.flipY = " << formatBool(ui.flipY) << ";\n";

    // Image, Text and Polygon build the geometry on load; any other UI keeps what it was saved with
    if (!Stream::isGeneratedUI(scene, scene->getSignature(entity))){
        code << ind << "ui.primitiveType = " << formatPrimitiveType(ui.primitiveType) << ";\n";
        if (ui.minBufferCount > 0)
            code << ind << "ui.minBufferCount = " << formatUInt(ui.minBufferCount) << ";\n";
        if (ui.minIndicesCount > 0)
            code << ind << "ui.minIndicesCount = " << formatUInt(ui.minIndicesCount) << ";\n";
        code << formatBufferContents(ind, "ui.buffer", ui.buffer, InterleavedBuffer());
        code << formatBufferContents(ind, "ui.indices", ui.indices, IndexBuffer());
    }
    if (!ui.enabled) code << ind << "ui.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "UIComponent", "ui", assignExisting);
    return code.str();
}

std::string editor::Factory::createPolygonComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<PolygonComponent>(entity)) return "";
    PolygonComponent& polygon = scene->getComponent<PolygonComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "PolygonComponent polygoncomp;\n";

    for (size_t i = 0; i < polygon.points.size(); i++) {
        const PolygonPoint& point = polygon.points[i];
        code << ind << "{\n";
        code << ind << "    PolygonPoint pt;\n";
        code << ind << "    pt.position = " << formatVector3(point.position) << ";\n";
        code << ind << "    pt.color = " << formatVector4(point.color) << ";\n";
        code << ind << "    polygoncomp.points.push_back(pt);\n";
        code << ind << "}\n";
    }

    addComponentCode(code, ind, sceneName, entityName, entity, "PolygonComponent", "polygoncomp", assignExisting);
    return code.str();
}

std::string editor::Factory::createMeshPolygonComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<MeshPolygonComponent>(entity)) return "";
    MeshPolygonComponent& polygon = scene->getComponent<MeshPolygonComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "MeshPolygonComponent meshpolygon;\n";
    code << ind << "meshpolygon.automaticFlipY = " << formatBool(polygon.automaticFlipY) << ";\n";
    code << ind << "meshpolygon.flipY = " << formatBool(polygon.flipY) << ";\n";

    for (size_t i = 0; i < polygon.points.size(); i++) {
        const PolygonPoint& point = polygon.points[i];
        code << ind << "{\n";
        code << ind << "    PolygonPoint pt;\n";
        code << ind << "    pt.position = " << formatVector3(point.position) << ";\n";
        code << ind << "    pt.color = " << formatVector4(point.color) << ";\n";
        code << ind << "    meshpolygon.points.push_back(pt);\n";
        code << ind << "}\n";
    }

    addComponentCode(code, ind, sceneName, entityName, entity, "MeshPolygonComponent", "meshpolygon", assignExisting);
    return code.str();
}

std::string editor::Factory::createSafeAreaComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<SafeAreaComponent>(entity)) return "";
    const SafeAreaComponent& safeArea = scene->getComponent<SafeAreaComponent>(entity);
    const SafeAreaComponent defaults;
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "SafeAreaComponent safearea;\n";
    // what differs from the defaults (every edge, extras included)
    if (safeArea.enabled != defaults.enabled) code << ind << "safearea.enabled = " << formatBool(safeArea.enabled) << ";\n";
    if (safeArea.left != defaults.left) code << ind << "safearea.left = " << formatBool(safeArea.left) << ";\n";
    if (safeArea.top != defaults.top) code << ind << "safearea.top = " << formatBool(safeArea.top) << ";\n";
    if (safeArea.right != defaults.right) code << ind << "safearea.right = " << formatBool(safeArea.right) << ";\n";
    if (safeArea.bottom != defaults.bottom) code << ind << "safearea.bottom = " << formatBool(safeArea.bottom) << ";\n";
    if (safeArea.includeExtraInsets != defaults.includeExtraInsets) code << ind << "safearea.includeExtraInsets = " << formatBool(safeArea.includeExtraInsets) << ";\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "SafeAreaComponent", "safearea", assignExisting);
    return code.str();
}

std::string editor::Factory::createTextEffectComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<TextEffectComponent>(entity)) return "";
    TextEffectComponent& effect = scene->getComponent<TextEffectComponent>(entity);
    static const char* const types[] = {"None", "Wave", "Bounce", "Shake", "Pulse", "Typewriter", "FadeIn", "Grow", "Unfold", "Rainbow", "Gradient"};
    static const char* const mixes[] = {"Multiply", "Add", "Replace"};
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    const TextEffectComponent d;
    const TextEffectLayer dl;
    code << ind << "TextEffectComponent textEffect;\n";
    if (effect.enabled != d.enabled) code << ind << "textEffect.enabled = " << formatBool(effect.enabled) << ";\n";
    for (size_t i = 0; i < effect.layers.size(); i++){
        const TextEffectLayer& l = effect.layers[i];
        if (l == dl) continue;
        const std::string layer = "textEffect.layers[" + std::to_string(i) + "]";
        code << ind << layer << ".type = TextEffectType::" << types[std::min((size_t)l.type, (size_t)10)] << ";\n";
        if (l.enabled != dl.enabled) code << ind << layer << ".enabled = " << formatBool(l.enabled) << ";\n";
        if (l.amplitude != dl.amplitude) code << ind << layer << ".amplitude = " << formatFloat(l.amplitude) << ";\n";
        if (l.duration != dl.duration) code << ind << layer << ".duration = " << formatFloat(l.duration) << ";\n";
        if (l.spread != dl.spread) code << ind << layer << ".spread = " << formatFloat(l.spread) << ";\n";
        if (l.phaseStep != dl.phaseStep) code << ind << layer << ".phaseStep = " << formatFloat(l.phaseStep) << ";\n";
        if (l.frequency != dl.frequency) code << ind << layer << ".frequency = " << formatFloat(l.frequency) << ";\n";
        if (l.charactersPerSecond != dl.charactersPerSecond) code << ind << layer << ".charactersPerSecond = " << formatFloat(l.charactersPerSecond) << ";\n";
        if (l.fadeTime != dl.fadeTime) code << ind << layer << ".fadeTime = " << formatFloat(l.fadeTime) << ";\n";
        for (const TextEffectGradientStop& stop : l.gradient){
            code << ind << layer << ".gradient.push_back(TextEffectGradientStop{" << formatVector4(stop.color) << ", " << formatFloat(stop.position) << "});\n";
        }
        if (l.mix != dl.mix) code << ind << layer << ".mix = TextEffectMix::" << mixes[std::min((size_t)l.mix, (size_t)2)] << ";\n";
        if (l.saturation != dl.saturation) code << ind << layer << ".saturation = " << formatFloat(l.saturation) << ";\n";
        if (l.lightness != dl.lightness) code << ind << layer << ".lightness = " << formatFloat(l.lightness) << ";\n";
    }
    if (effect.autoPlay != d.autoPlay) code << ind << "textEffect.autoPlay = " << formatBool(effect.autoPlay) << ";\n";
    if (effect.loop != d.loop) code << ind << "textEffect.loop = " << formatBool(effect.loop) << ";\n";
    if (effect.useUnscaledTime != d.useUnscaledTime) code << ind << "textEffect.useUnscaledTime = " << formatBool(effect.useUnscaledTime) << ";\n";
    if (effect.startIndex != d.startIndex) code << ind << "textEffect.startIndex = " << effect.startIndex << ";\n";
    if (effect.length != d.length) code << ind << "textEffect.length = " << effect.length << ";\n";
    if (!effect.forWords.empty()) code << ind << "textEffect.forWords = " << formatString(effect.forWords) << ";\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "TextEffectComponent", "textEffect", assignExisting);
    return code.str();
}

std::string editor::Factory::createSpriteComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<SpriteComponent>(entity)) return "";
    SpriteComponent& sprite = scene->getComponent<SpriteComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "SpriteComponent sprite;\n";
    code << ind << "sprite.width = " << formatUInt(sprite.width) << ";\n";
    code << ind << "sprite.height = " << formatUInt(sprite.height) << ";\n";
    code << ind << "sprite.automaticFlipY = " << formatBool(sprite.automaticFlipY) << ";\n";
    code << ind << "sprite.flipY = " << formatBool(sprite.flipY) << ";\n";
    code << ind << "sprite.pivotPreset = " << formatPivotPreset(sprite.pivotPreset) << ";\n";
    code << ind << "sprite.textureScaleFactor = " << formatFloat(sprite.textureScaleFactor) << ";\n";
    // as authored, in pixels or normalised: the game normalises a frame when it shows it
    // (MeshSystem::setSpriteFrameRect), once its texture's size is known
    for (unsigned int i = 0; i < sprite.numFramesRect; i++) {
        if (!sprite.framesRect[i].name.empty()) {
            code << ind << "sprite.framesRect[" << i << "].name = " << formatString(sprite.framesRect[i].name) << ";\n";
        }
        code << ind << "sprite.framesRect[" << i << "].rect = " << formatRect(sprite.framesRect[i].rect) << ";\n";
    }
    if (sprite.numFramesRect > 0) {
        code << ind << "sprite.numFramesRect = " << sprite.numFramesRect << ";\n";
    }
    if (!sprite.enabled) code << ind << "sprite.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "SpriteComponent", "sprite", assignExisting);
    return code.str();
}

std::string editor::Factory::createTilemapComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<TilemapComponent>(entity)) return "";
    TilemapComponent& tilemap = scene->getComponent<TilemapComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    const std::string tilemapValue = beginHeapComponentCode(code, ind, "TilemapComponent", "tilemap");
    code << ind << "tilemap.width = " << formatUInt(tilemap.width) << ";\n";
    code << ind << "tilemap.height = " << formatUInt(tilemap.height) << ";\n";
    code << ind << "tilemap.automaticFlipY = " << formatBool(tilemap.automaticFlipY) << ";\n";
    code << ind << "tilemap.flipY = " << formatBool(tilemap.flipY) << ";\n";
    code << ind << "tilemap.textureScaleFactor = " << formatFloat(tilemap.textureScaleFactor) << ";\n";
    code << ind << "tilemap.reserveTiles = " << formatUInt(tilemap.reserveTiles) << ";\n";
    for (unsigned int i = 0; i < tilemap.numTilesRect; i++) {
        if (!tilemap.tilesRect[i].name.empty()) {
            code << ind << "tilemap.tilesRect[" << i << "].name = " << formatString(tilemap.tilesRect[i].name) << ";\n";
        }
        code << ind << "tilemap.tilesRect[" << i << "].submeshId = " << tilemap.tilesRect[i].submeshId << ";\n";
        code << ind << "tilemap.tilesRect[" << i << "].rect = " << formatRect(tilemap.tilesRect[i].rect) << ";\n";
    }
    if (tilemap.numTilesRect > 0) {
        code << ind << "tilemap.numTilesRect = " << tilemap.numTilesRect << ";\n";
    }
    for (unsigned int i = 0; i < tilemap.numTiles; i++) {
        if (!tilemap.tiles[i].name.empty()) {
            code << ind << "tilemap.tiles[" << i << "].name = " << formatString(tilemap.tiles[i].name) << ";\n";
        }
        code << ind << "tilemap.tiles[" << i << "].rectId = " << tilemap.tiles[i].rectId << ";\n";
        code << ind << "tilemap.tiles[" << i << "].position = " << formatVector2(tilemap.tiles[i].position) << ";\n";
        code << ind << "tilemap.tiles[" << i << "].width = " << formatFloat(tilemap.tiles[i].width) << ";\n";
        code << ind << "tilemap.tiles[" << i << "].height = " << formatFloat(tilemap.tiles[i].height) << ";\n";
    }
    if (tilemap.numTiles > 0) {
        code << ind << "tilemap.numTiles = " << tilemap.numTiles << ";\n";
    }
    if (!tilemap.enabled) code << ind << "tilemap.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "TilemapComponent", tilemapValue, assignExisting);
    return code.str();
}

std::string editor::Factory::createTerrainComponent(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<TerrainComponent>(entity)) return "";
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "TerrainComponent terrain;\n";
    code << formatTexture(indentSpaces, terrain.heightMap, "terrain.heightMap", projectPath);
    code << ind << "terrain.blendMaps.resize(" << terrain.blendMaps.size() << ");\n";
    for (size_t i = 0; i < terrain.blendMaps.size(); i++){
        code << formatTexture(indentSpaces, terrain.blendMaps[i], "terrain.blendMaps[" + std::to_string(i) + "]", projectPath);
    }
    code << ind << "terrain.surfaceLayers.resize(" << terrain.surfaceLayers.size() << ");\n";
    for (size_t i = 0; i < terrain.surfaceLayers.size(); i++){
        const TerrainSurfaceLayer& layer = terrain.surfaceLayers[i];
        const std::string var = "terrain.surfaceLayers[" + std::to_string(i) + "]";
        if (layer.pbr){
            code << ind << var << ".pbr = true;\n";
        }
        code << formatTexture(indentSpaces, layer.colorTexture, var + ".colorTexture", projectPath);
        code << formatTexture(indentSpaces, layer.normalTexture, var + ".normalTexture", projectPath);
        code << formatTexture(indentSpaces, layer.roughnessTexture, var + ".roughnessTexture", projectPath);
        code << formatTexture(indentSpaces, layer.metallicTexture, var + ".metallicTexture", projectPath);
        code << formatTexture(indentSpaces, layer.occlusionTexture, var + ".occlusionTexture", projectPath);
        code << formatTexture(indentSpaces, layer.heightTexture, var + ".heightTexture", projectPath);
        code << ind << var << ".colorFactor = " << formatVector4(layer.colorFactor) << ";\n";
        code << ind << var << ".normalStrength = " << formatFloat(layer.normalStrength) << ";\n";
        code << ind << var << ".roughnessFactor = " << formatFloat(layer.roughnessFactor) << ";\n";
        code << ind << var << ".metallicFactor = " << formatFloat(layer.metallicFactor) << ";\n";
        code << ind << var << ".occlusionStrength = " << formatFloat(layer.occlusionStrength) << ";\n";
        code << ind << var << ".uvScale = " << formatVector2(layer.uvScale) << ";\n";
        code << ind << var << ".uvOffset = " << formatVector2(layer.uvOffset) << ";\n";
        if (layer.stochasticTiling){
            code << ind << var << ".stochasticTiling = true;\n";
        }
        static const TerrainSurfaceLayer defaults;
        if (layer.stochasticCellSize != defaults.stochasticCellSize){
            code << ind << var << ".stochasticCellSize = " << formatFloat(layer.stochasticCellSize) << ";\n";
        }
        if (layer.stochasticBlendSharpness != defaults.stochasticBlendSharpness){
            code << ind << var << ".stochasticBlendSharpness = " << formatFloat(layer.stochasticBlendSharpness) << ";\n";
        }
        if (layer.stochasticContrast != defaults.stochasticContrast){
            code << ind << var << ".stochasticContrast = " << formatFloat(layer.stochasticContrast) << ";\n";
        }
        if (layer.stochasticRotation != defaults.stochasticRotation){
            code << ind << var << ".stochasticRotation = " << formatFloat(layer.stochasticRotation) << ";\n";
        }
        if (layer.hexTiling){
            code << ind << var << ".hexTiling = true;\n";
        }
        if (layer.hexRotation != TerrainSurfaceLayer().hexRotation){
            code << ind << var << ".hexRotation = " << formatFloat(layer.hexRotation) << ";\n";
        }
    }
    code << ind << "terrain.autoSetRanges = " << formatBool(terrain.autoSetRanges) << ";\n";
    code << ind << "terrain.offset = " << formatVector2(terrain.offset) << ";\n";
    code << ind << "terrain.terrainSize = " << formatFloat(terrain.terrainSize) << ";\n";
    code << ind << "terrain.maxHeight = " << formatFloat(terrain.maxHeight) << ";\n";
    code << ind << "terrain.resolution = " << formatFloat(terrain.resolution) << ";\n";
    code << ind << "terrain.textureBaseTiles = " << formatFloat(terrain.textureBaseTiles) << ";\n";
    code << ind << "terrain.textureDetailTiles = " << formatFloat(terrain.textureDetailTiles) << ";\n";
    code << ind << "terrain.rootGridSize = " << formatInt(terrain.rootGridSize) << ";\n";
    code << ind << "terrain.levels = " << formatInt(terrain.levels) << ";\n";
    // (Auto Ranges: the terrain sets them from the camera's far clip when it runs)
    if (!terrain.autoSetRanges && !terrain.ranges.empty()) {
        code << ind << "terrain.ranges = {";
        for (size_t i = 0; i < terrain.ranges.size(); i++) {
            if (i > 0) code << ", ";
            code << formatFloat(terrain.ranges[i]);
        }
        code << "};\n";
    }
    for (size_t i = 0; i < terrain.foliageLayers.size(); i++) {
        const TerrainFoliageLayer& layer = terrain.foliageLayers[i];
        const std::string var = "foliageLayer" + std::to_string(i);
        code << ind << "TerrainFoliageLayer " << var << ";\n";
        code << ind << var << ".meshPath = " << formatString(layer.meshPath) << ";\n";
        code << formatTexture(indentSpaces, layer.densityMap, var + ".densityMap", projectPath);
        code << ind << var << ".density = " << formatFloat(layer.density) << ";\n";
        code << ind << var << ".minScale = " << formatFloat(layer.minScale) << ";\n";
        code << ind << var << ".maxScale = " << formatFloat(layer.maxScale) << ";\n";
        code << ind << var << ".rotationJitter = " << formatFloat(layer.rotationJitter) << ";\n";
        code << ind << var << ".alignToNormal = " << formatFloat(layer.alignToNormal) << ";\n";
        code << ind << var << ".minSlope = " << formatFloat(layer.minSlope) << ";\n";
        code << ind << var << ".maxSlope = " << formatFloat(layer.maxSlope) << ";\n";
        code << ind << var << ".minHeight = " << formatFloat(layer.minHeight) << ";\n";
        code << ind << var << ".maxHeight = " << formatFloat(layer.maxHeight) << ";\n";
        code << ind << var << ".drawDistance = " << formatFloat(layer.drawDistance) << ";\n";
        code << ind << var << ".seed = " << formatUInt(layer.seed) << ";\n";
        code << ind << var << ".castShadows = " << formatBool(layer.castShadows) << ";\n";
        code << ind << "terrain.foliageLayers.push_back(" << var << ");\n";
    }
    code << ind << "terrain.needUpdateTerrain = true;\n";
    code << ind << "terrain.needUpdateTexture = true;\n";
    code << ind << "terrain.needUpdateFoliage = true;\n";
    code << ind << "terrain.heightMapLoaded = false;\n";
    if (!terrain.enabled) code << ind << "terrain.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "TerrainComponent", "terrain", assignExisting);
    return code.str();
}

std::string editor::Factory::createVideoPlayerComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->isComponentRegistered<VideoPlayerComponent>() || !scene->findComponent<VideoPlayerComponent>(entity)) return "";
    VideoPlayerComponent& player = scene->getComponent<VideoPlayerComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "VideoPlayerComponent player;\n";
    if (!player.enabled) code << ind << "player.enabled = false;\n";
    // only what differs from the defaults; the runtime (its texture, its listeners) starts fresh
    const VideoPlayerComponent defaults;
    if (player.source != defaults.source) code << ind << "player.source = " << "VideoPlayerSource(" << (int)player.source << ")" << ";\n";
    if (player.clip != defaults.clip) code << ind << "player.clip = " << formatString(player.clip) << ";\n";
    if (player.url != defaults.url) code << ind << "player.url = " << formatString(player.url) << ";\n";
    if (player.playOnAwake != defaults.playOnAwake) code << ind << "player.playOnAwake = " << formatBool(player.playOnAwake) << ";\n";
    if (player.waitForFirstFrame != defaults.waitForFirstFrame) code << ind << "player.waitForFirstFrame = " << formatBool(player.waitForFirstFrame) << ";\n";
    if (player.isLooping != defaults.isLooping) code << ind << "player.isLooping = " << formatBool(player.isLooping) << ";\n";
    if (player.skipOnDrop != defaults.skipOnDrop) code << ind << "player.skipOnDrop = " << formatBool(player.skipOnDrop) << ";\n";
    if (player.playbackSpeed != defaults.playbackSpeed) code << ind << "player.playbackSpeed = " << formatFloat(player.playbackSpeed) << ";\n";
    if (player.timeUpdateMode != defaults.timeUpdateMode) code << ind << "player.timeUpdateMode = " << "VideoTimeUpdateMode(" << (int)player.timeUpdateMode << ")" << ";\n";
    if (player.renderMode != defaults.renderMode) code << ind << "player.renderMode = " << "VideoRenderMode(" << (int)player.renderMode << ")" << ";\n";
    if (player.targetCamera != defaults.targetCamera) code << ind << "player.targetCamera = " << formatEntity(player.targetCamera, entityVarNames) << ";\n";
    if (player.targetCameraAlpha != defaults.targetCameraAlpha) code << ind << "player.targetCameraAlpha = " << formatFloat(player.targetCameraAlpha) << ";\n";
    if (player.aspectRatio != defaults.aspectRatio) code << ind << "player.aspectRatio = " << "VideoAspectRatio(" << (int)player.aspectRatio << ")" << ";\n";
    if (player.targetMaterialRenderer != defaults.targetMaterialRenderer) code << ind << "player.targetMaterialRenderer = " << formatEntity(player.targetMaterialRenderer, entityVarNames) << ";\n";
    if (player.targetMaterialProperty != defaults.targetMaterialProperty) code << ind << "player.targetMaterialProperty = " << formatString(player.targetMaterialProperty) << ";\n";
    if (player.targetSubmesh != defaults.targetSubmesh) code << ind << "player.targetSubmesh = " << std::to_string(player.targetSubmesh) << ";\n";
    if (player.renderTextureWidth != defaults.renderTextureWidth) code << ind << "player.renderTextureWidth = " << formatUInt(player.renderTextureWidth) << "u" << ";\n";
    if (player.renderTextureHeight != defaults.renderTextureHeight) code << ind << "player.renderTextureHeight = " << formatUInt(player.renderTextureHeight) << "u" << ";\n";
    if (player.audioOutputMode != defaults.audioOutputMode) code << ind << "player.audioOutputMode = " << "VideoAudioOutputMode(" << (int)player.audioOutputMode << ")" << ";\n";
    if (player.directVolume != defaults.directVolume) code << ind << "player.directVolume = " << formatFloat(player.directVolume) << ";\n";
    if (player.directMute != defaults.directMute) code << ind << "player.directMute = " << formatBool(player.directMute) << ";\n";
    if (player.targetAudioSource != defaults.targetAudioSource) code << ind << "player.targetAudioSource = " << formatEntity(player.targetAudioSource, entityVarNames) << ";\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "VideoPlayerComponent", "player", assignExisting);
    return code.str();
}

std::string editor::Factory::createScriptComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<ScriptComponent>(entity)) return "";
    ScriptComponent& script = scene->getComponent<ScriptComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "ScriptComponent script;\n";

    for (size_t i = 0; i < script.scripts.size(); i++) {
        std::string idx = std::to_string(i);
        code << ind << "script.scripts.push_back(ScriptEntry());\n";
        code << ind << "script.scripts[" << idx << "].type = " << formatScriptType(script.scripts[i].type) << ";\n";
        code << ind << "script.scripts[" << idx << "].path = " << formatString(script.scripts[i].path) << ";\n";
        code << ind << "script.scripts[" << idx << "].headerPath = " << formatString(script.scripts[i].headerPath) << ";\n";
        code << ind << "script.scripts[" << idx << "].className = " << formatString(script.scripts[i].className) << ";\n";
        code << ind << "script.scripts[" << idx << "].enabled = " << formatBool(script.scripts[i].enabled) << ";\n";

        for (size_t p = 0; p < script.scripts[i].properties.size(); p++) {
            const ScriptProperty& prop = script.scripts[i].properties[p];
            std::string pidx = std::to_string(p);
            code << ind << "script.scripts[" << idx << "].properties.push_back(ScriptProperty());\n";
            code << ind << "script.scripts[" << idx << "].properties[" << pidx << "].name = " << formatString(prop.name) << ";\n";
            code << ind << "script.scripts[" << idx << "].properties[" << pidx << "].type = " << formatScriptPropertyType(prop.type) << ";\n";
            code << ind << "script.scripts[" << idx << "].properties[" << pidx << "].value = " << formatScriptPropertyValue(prop.value, entityVarNames) << ";\n";
            code << ind << "script.scripts[" << idx << "].properties[" << pidx << "].defaultValue = " << formatScriptPropertyValue(prop.defaultValue, entityVarNames) << ";\n";
            if (!prop.ptrTypeName.empty()) {
                code << ind << "script.scripts[" << idx << "].properties[" << pidx << "].ptrTypeName = " << formatString(prop.ptrTypeName) << ";\n";
            }
        }
    }

    addComponentCode(code, ind, sceneName, entityName, entity, "ScriptComponent", "script", assignExisting);
    return code.str();
}

std::string editor::Factory::createSkyboxComponent(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<SkyboxComponent>(entity)) return "";
    SkyboxComponent& sky = scene->getComponent<SkyboxComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "SkyboxComponent sky;\n";

    if (sky.texture.getId() == DEFAULT_SKY_ID) {
        code << ind << "TextureData skyFront;\n";
        code << ind << "TextureData skyBack;\n";
        code << ind << "TextureData skyLeft;\n";
        code << ind << "TextureData skyRight;\n";
        code << ind << "TextureData skyTop;\n";
        code << ind << "TextureData skyBottom;\n";

        code << ind << "skyFront.loadTextureFromMemory(Daylight_Box_Front_data, Daylight_Box_Front_len);\n";
        code << ind << "skyBack.loadTextureFromMemory(Daylight_Box_Back_data, Daylight_Box_Back_len);\n";
        code << ind << "skyLeft.loadTextureFromMemory(Daylight_Box_Left_data, Daylight_Box_Left_len);\n";
        code << ind << "skyRight.loadTextureFromMemory(Daylight_Box_Right_data, Daylight_Box_Right_len);\n";
        code << ind << "skyTop.loadTextureFromMemory(Daylight_Box_Top_data, Daylight_Box_Top_len);\n";
        code << ind << "skyBottom.loadTextureFromMemory(Daylight_Box_Bottom_data, Daylight_Box_Bottom_len);\n";

        code << ind << "sky.texture.setId(" << formatString(sky.texture.getId()) << ");\n";
        code << ind << "sky.texture.setCubeDatas(" << formatString(sky.texture.getId()) << ", skyFront, skyBack, skyLeft, skyRight, skyTop, skyBottom);\n";
        code << ind << "sky.texture.setMinFilter(" << formatTextureFilter(sky.texture.getMinFilter()) << ");\n";
        code << ind << "sky.texture.setMagFilter(" << formatTextureFilter(sky.texture.getMagFilter()) << ");\n";
        code << ind << "sky.texture.setWrapU(" << formatTextureWrap(sky.texture.getWrapU()) << ");\n";
        code << ind << "sky.texture.setWrapV(" << formatTextureWrap(sky.texture.getWrapV()) << ");\n";
    } else {
        code << formatTexture(indentSpaces, sky.texture, "sky.texture", projectPath);
    }
    code << ind << "sky.color = " << formatVector4(sky.color) << ";\n";
    code << ind << "sky.rotation = " << formatFloat(sky.rotation) << ";\n";
    code << ind << "sky.visible = " << formatBool(sky.visible) << ";\n";
    if (!sky.customShader.empty())
        code << ind << "sky.customShader = " << formatString(sky.customShader) << ";\n";
    if (!sky.shaderUniforms.empty())
        code << ind << "sky.shaderUniforms = " << formatShaderUniforms(sky.shaderUniforms) << ";\n";
    if (!sky.shaderGlobalBindings.empty())
        code << ind << "sky.shaderGlobalBindings = " << formatShaderGlobalBindings(sky.shaderGlobalBindings) << ";\n";
    code << formatComponentShaderLibrary(indentSpaces, sky.shaderTextures, sky.shaderFeatures, nullptr, nullptr, "sky", projectPath);

    addComponentCode(code, ind, sceneName, entityName, entity, "SkyboxComponent", "sky", assignExisting);
    return code.str();
}

std::string editor::Factory::formatBodyType(BodyType type) {
    return formatEnum(reflect::enums::kBodyType, static_cast<int>(type));
}

std::string editor::Factory::formatCollider2DType(Collider2DType type) {
    return formatEnum(reflect::enums::kCollider2DType, static_cast<int>(type));
}

std::string editor::Factory::formatRigidbodyMotionQuality(RigidbodyMotionQuality motionQuality) {
    return formatEnum(reflect::enums::kRigidbodyMotionQuality, static_cast<int>(motionQuality));
}

std::string editor::Factory::formatCollider3DType(Collider3DType type) {
    return formatEnum(reflect::enums::kCollider3DType, static_cast<int>(type));
}

std::string editor::Factory::formatCollider3DSource(Collider3DSource source) {
    return formatEnum(reflect::enums::kCollider3DSource, static_cast<int>(source));
}

std::string editor::Factory::formatActionState(ActionState state) {
    return formatEnum(reflect::enums::kActionState, static_cast<int>(state));
}

std::string editor::Factory::createRigidbody2DComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<Rigidbody2DComponent>(entity)) return "";
    Rigidbody2DComponent& body = scene->getComponent<Rigidbody2DComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    const Rigidbody2DComponent def;
    const Collider2D defCollider;
    code << ind << "Rigidbody2DComponent rigidbody2d;\n";
    code << ind << "rigidbody2d.type = " << formatBodyType(body.type) << ";\n";

    // the body fields that differ from the defaults
    if (body.gravityScale != def.gravityScale) code << ind << "rigidbody2d.gravityScale = " << formatFloat(body.gravityScale) << ";\n";
    if (body.linearDamping != def.linearDamping) code << ind << "rigidbody2d.linearDamping = " << formatFloat(body.linearDamping) << ";\n";
    if (body.angularDamping != def.angularDamping) code << ind << "rigidbody2d.angularDamping = " << formatFloat(body.angularDamping) << ";\n";
    if (body.fixedRotation != def.fixedRotation) code << ind << "rigidbody2d.fixedRotation = " << formatBool(body.fixedRotation) << ";\n";
    if (body.collisionDetection != def.collisionDetection) code << ind << "rigidbody2d.collisionDetection = CollisionDetectionMode2D::CONTINUOUS;\n";
    if (body.sleepingMode == RigidbodySleepMode2D::NEVER_SLEEP) code << ind << "rigidbody2d.sleepingMode = RigidbodySleepMode2D::NEVER_SLEEP;\n";
    if (body.sleepingMode == RigidbodySleepMode2D::START_ASLEEP) code << ind << "rigidbody2d.sleepingMode = RigidbodySleepMode2D::START_ASLEEP;\n";
    if (body.useAutoMass != def.useAutoMass) code << ind << "rigidbody2d.useAutoMass = " << formatBool(body.useAutoMass) << ";\n";
    if (body.mass != def.mass) code << ind << "rigidbody2d.mass = " << formatFloat(body.mass) << ";\n";
    if (body.autoCenterOfMass != def.autoCenterOfMass) code << ind << "rigidbody2d.autoCenterOfMass = " << formatBool(body.autoCenterOfMass) << ";\n";
    if (body.centerOfMass != def.centerOfMass) code << ind << "rigidbody2d.centerOfMass = " << formatVector2(body.centerOfMass) << ";\n";
    if (body.autoInertia != def.autoInertia) code << ind << "rigidbody2d.autoInertia = " << formatBool(body.autoInertia) << ";\n";
    if (body.inertia != def.inertia) code << ind << "rigidbody2d.inertia = " << formatFloat(body.inertia) << ";\n";
    if (!body.material.empty()) code << ind << "rigidbody2d.material = " << formatString(body.material) << ";\n";

    if (!body.colliders.empty()) {
        code << ind << "rigidbody2d.colliders.resize(" << body.colliders.size() << ");\n";
    }
    for (size_t i = 0; i < body.colliders.size(); i++) {
        const Collider2D& collider = body.colliders[i];
        const std::string target = "rigidbody2d.colliders[" + std::to_string(i) + "]";
        code << ind << target << ".type = " << formatCollider2DType(collider.type) << ";\n";
        code << ind << target << ".pointA = " << formatVector2(collider.pointA) << ";\n";
        code << ind << target << ".pointB = " << formatVector2(collider.pointB) << ";\n";
        code << ind << target << ".radius = " << formatFloat(collider.radius) << ";\n";
        // any number of points: one statement per 64 so a long terrain compiles everywhere
        const size_t chunk = 64;
        for (size_t start = 0; start < collider.points.size(); start += chunk) {
            const size_t end = std::min(collider.points.size(), start + chunk);
            if (start == 0) {
                code << ind << target << ".points = {";
            } else {
                code << ind << target << ".points.insert(" << target << ".points.end(), {";
            }
            for (size_t j = start; j < end; j++) {
                if (j > start) code << ", ";
                code << formatVector2(collider.points[j]);
            }
            code << (start == 0 ? "};\n" : "});\n");
        }
        code << ind << target << ".loop = " << formatBool(collider.loop) << ";\n";
        code << ind << target << ".density = " << formatFloat(collider.density) << ";\n";
        code << ind << target << ".friction = " << formatFloat(collider.friction) << ";\n";
        code << ind << target << ".restitution = " << formatFloat(collider.restitution) << ";\n";
        if (!collider.material.empty()) code << ind << target << ".material = " << formatString(collider.material) << ";\n";
        code << ind << target << ".sensor = " << formatBool(collider.sensor) << ";\n";
        code << ind << target << ".enableHitEvents = " << formatBool(collider.enableHitEvents) << ";\n";
        code << ind << target << ".contactEvents = " << formatBool(collider.contactEvents) << ";\n";
        code << ind << target << ".preSolveEvents = " << formatBool(collider.preSolveEvents) << ";\n";
        code << ind << target << ".sensorEvents = " << formatBool(collider.sensorEvents) << ";\n";
        code << ind << target << ".categoryBits = " << collider.categoryBits << ";\n";
        code << ind << target << ".maskBits = " << collider.maskBits << ";\n";
        code << ind << target << ".groupIndex = " << collider.groupIndex << ";\n";
        if (collider.receiver != defCollider.receiver) {
            code << ind << target << ".receiver = " << formatEntity(collider.receiver, entityVarNames) << ";\n";
        }
    }

    if (!body.enabled) code << ind << "rigidbody2d.enabled = false;\n";

    addComponentCode(code, ind, sceneName, entityName, entity, "Rigidbody2DComponent", "rigidbody2d", assignExisting);
    return code.str();
}

std::string editor::Factory::createRigidbodyComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<RigidbodyComponent>(entity)) return "";
    RigidbodyComponent& body = scene->getComponent<RigidbodyComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    const std::string bodyValue = beginHeapComponentCode(code, ind, "RigidbodyComponent", "rigidbody");
    code << ind << "rigidbody.type = " << formatBodyType(body.type) << ";\n";
    code << ind << "rigidbody.motionQuality = " << formatRigidbodyMotionQuality(body.motionQuality) << ";\n";
    code << ind << "rigidbody.sensor = " << formatBool(body.sensor) << ";\n";
    code << ind << "rigidbody.gravityFactor = " << formatFloat(body.gravityFactor) << ";\n";
    code << ind << "rigidbody.friction = " << formatFloat(body.friction) << ";\n";
    code << ind << "rigidbody.restitution = " << formatFloat(body.restitution) << ";\n";
    code << ind << "rigidbody.buoyancy = " << formatFloat(body.buoyancy) << ";\n";
    code << ind << "rigidbody.waterDrag = " << formatFloat(body.waterDrag) << ";\n";
    code << ind << "rigidbody.waterAngularDrag = " << formatFloat(body.waterAngularDrag) << ";\n";
    // The bit mask, rather than six OR'd enumerators in the generated source.
    code << ind << "rigidbody.allowedDOFs = " << static_cast<unsigned int>(body.allowedDOFs) << ";\n";
    code << ind << "rigidbody.overrideMassProperties = " << formatBool(body.overrideMassProperties) << ";\n";
    code << ind << "rigidbody.solidBoxSize = " << formatVector3(body.solidBoxSize) << ";\n";
    code << ind << "rigidbody.solidBoxDensity = " << formatFloat(body.solidBoxDensity) << ";\n";
    // the Rigidbody fields (written when not at their defaults)
    const RigidbodyComponent def{};
    auto floatField = [&](const char* name, float value, float defaultValue) {
        if (value != defaultValue) code << ind << "rigidbody." << name << " = " << formatFloat(value) << ";\n";
    };
    auto boolField = [&](const char* name, bool value, bool defaultValue) {
        if (value != defaultValue) code << ind << "rigidbody." << name << " = " << formatBool(value) << ";\n";
    };
    if (!body.material.empty()) code << ind << "rigidbody.material = " << formatString(body.material) << ";\n";
    floatField("linearDamping", body.linearDamping, def.linearDamping);
    floatField("angularDamping", body.angularDamping, def.angularDamping);
    floatField("maxLinearVelocity", body.maxLinearVelocity, def.maxLinearVelocity);
    floatField("maxAngularVelocity", body.maxAngularVelocity, def.maxAngularVelocity);
    boolField("automaticMass", body.automaticMass, def.automaticMass);
    floatField("mass", body.mass, def.mass);
    boolField("automaticCenterOfMass", body.automaticCenterOfMass, def.automaticCenterOfMass);
    if (body.centerOfMass != def.centerOfMass) code << ind << "rigidbody.centerOfMass = " << formatVector3(body.centerOfMass) << ";\n";
    boolField("automaticInertiaTensor", body.automaticInertiaTensor, def.automaticInertiaTensor);
    if (body.inertiaTensor != def.inertiaTensor) code << ind << "rigidbody.inertiaTensor = " << formatVector3(body.inertiaTensor) << ";\n";
    if (body.inertiaTensorRotation != def.inertiaTensorRotation) code << ind << "rigidbody.inertiaTensorRotation = " << formatQuaternion(body.inertiaTensorRotation) << ";\n";
    boolField("allowSleeping", body.allowSleeping, def.allowSleeping);
    boolField("collideKinematicVsNonDynamic", body.collideKinematicVsNonDynamic, def.collideKinematicVsNonDynamic);
    if (body.interpolation != def.interpolation) {
        const char* interpolation = "RigidbodyInterpolation::SCENE";
        if (body.interpolation == RigidbodyInterpolation::NONE) interpolation = "RigidbodyInterpolation::NONE";
        else if (body.interpolation == RigidbodyInterpolation::INTERPOLATE) interpolation = "RigidbodyInterpolation::INTERPOLATE";
        else if (body.interpolation == RigidbodyInterpolation::EXTRAPOLATE) interpolation = "RigidbodyInterpolation::EXTRAPOLATE";
        code << ind << "rigidbody.interpolation = " << interpolation << ";\n";
    }
    if (!body.colliders.empty()) {
        code << ind << "rigidbody.colliders.resize(" << body.colliders.size() << ");\n";
    }

    auto sanitizeSourceForType = [](Collider3DType type, Collider3DSource source) {
        if (type == Collider3DType::CONVEX_HULL){
            if (source == Collider3DSource::RAW_VERTICES || source == Collider3DSource::ENTITY_MESH){
                return source;
            }
            return Collider3DSource::ENTITY_MESH;
        }
        if (type == Collider3DType::MESH){
            if (source == Collider3DSource::RAW_MESH || source == Collider3DSource::ENTITY_MESH){
                return source;
            }
            return Collider3DSource::ENTITY_MESH;
        }
        if (type == Collider3DType::HEIGHTFIELD){
            return Collider3DSource::ENTITY_HEIGHTFIELD;
        }
        return source;
    };

    for (size_t i = 0; i < body.colliders.size(); i++) {
        const Collider3D& collider = body.colliders[i];
        const std::string target = "rigidbody.colliders[" + std::to_string(i) + "]";
        const Collider3DSource source = sanitizeSourceForType(collider.type, collider.source);
        const Entity sourceEntity =
            (source != Collider3DSource::NONE && collider.sourceEntity == NULL_ENTITY)
                ? entity
                : collider.sourceEntity;

        code << ind << target << ".type = " << formatCollider3DType(collider.type) << ";\n";
        code << ind << target << ".position = " << formatVector3(collider.position) << ";\n";
        code << ind << target << ".rotation = " << formatQuaternion(collider.rotation) << ";\n";
        code << ind << target << ".width = " << formatFloat(collider.width) << ";\n";
        code << ind << target << ".height = " << formatFloat(collider.height) << ";\n";
        code << ind << target << ".depth = " << formatFloat(collider.depth) << ";\n";
        code << ind << target << ".radius = " << formatFloat(collider.radius) << ";\n";
        code << ind << target << ".halfHeight = " << formatFloat(collider.halfHeight) << ";\n";
        code << ind << target << ".topRadius = " << formatFloat(collider.topRadius) << ";\n";
        code << ind << target << ".bottomRadius = " << formatFloat(collider.bottomRadius) << ";\n";
        code << ind << target << ".density = " << formatFloat(collider.density) << ";\n";
        code << ind << target << ".source = " << formatCollider3DSource(source) << ";\n";
        code << ind << target << ".sourceEntity = " << formatEntity(sourceEntity, entityVarNames) << ";\n";
        code << ind << target << ".samplesSize = " << collider.samplesSize << ";\n";
        if (collider.isTrigger) code << ind << target << ".isTrigger = true;\n";
        if (!collider.material.empty()) code << ind << target << ".material = " << formatString(collider.material) << ";\n";
        if (collider.receiver != NULL_ENTITY) code << ind << target << ".receiver = " << formatEntity(collider.receiver, entityVarNames) << ";\n";
        // any size: one statement per 64 values so a large mesh compiles everywhere
        const size_t chunk = 64;
        for (size_t start = 0; start < collider.vertices.size(); start += chunk) {
            const size_t end = std::min(collider.vertices.size(), start + chunk);
            if (start == 0) {
                code << ind << target << ".vertices = {";
            } else {
                code << ind << target << ".vertices.insert(" << target << ".vertices.end(), {";
            }
            for (size_t j = start; j < end; j++) {
                if (j > start) code << ", ";
                code << formatVector3(collider.vertices[j]);
            }
            code << (start == 0 ? "};\n" : "});\n");
        }
        for (size_t start = 0; start < collider.indices.size(); start += chunk) {
            const size_t end = std::min(collider.indices.size(), start + chunk);
            if (start == 0) {
                code << ind << target << ".indices = {";
            } else {
                code << ind << target << ".indices.insert(" << target << ".indices.end(), {";
            }
            for (size_t j = start; j < end; j++) {
                if (j > start) code << ", ";
                code << collider.indices[j] << "u";
            }
            code << (start == 0 ? "};\n" : "});\n");
        }
    }

    if (!body.enabled) code << ind << "rigidbody.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "RigidbodyComponent", bodyValue, assignExisting);
    return code.str();
}

std::string editor::Factory::createSpriteAnimationComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<SpriteAnimationComponent>(entity)) return "";
    SpriteAnimationComponent& spriteanim = scene->getComponent<SpriteAnimationComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "SpriteAnimationComponent spriteanim;\n";
    code << ind << "spriteanim.loop = " << formatBool(spriteanim.loop) << ";\n";
    code << ind << "spriteanim.frameIndex = 0;\n";
    code << ind << "spriteanim.frameTimeIndex = 0;\n";
    code << ind << "spriteanim.spriteFrameCount = 0;\n";
    code << ind << "spriteanim.framesSize = " << formatUInt(spriteanim.framesSize) << ";\n";
    for (unsigned int i = 0; i < spriteanim.framesSize; i++) {
        code << ind << "spriteanim.frames[" << i << "] = " << formatInt(spriteanim.frames[i]) << ";\n";
    }
    code << ind << "spriteanim.framesTimeSize = " << formatUInt(spriteanim.framesTimeSize) << ";\n";
    for (unsigned int i = 0; i < spriteanim.framesTimeSize; i++) {
        code << ind << "spriteanim.framesTime[" << i << "] = " << formatInt(spriteanim.framesTime[i]) << ";\n";
    }
    addComponentCode(code, ind, sceneName, entityName, entity, "SpriteAnimationComponent", "spriteanim", assignExisting);
    return code.str();
}

std::string editor::Factory::createModelComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<ModelComponent>(entity)) return "";
    ModelComponent& model = scene->getComponent<ModelComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "ModelComponent modelcomp;\n";
    code << ind << "modelcomp.filename = " << formatString(model.filename) << ";\n";
    code << ind << "modelcomp.mergeStaticMeshes = " << formatBool(model.mergeStaticMeshes) << ";\n";
    // an Animation Player plays the model's clips: no entity animations (9.R15)
    if (!model.animationEntities) code << ind << "modelcomp.animationEntities = false;\n";
    code << ind << "modelcomp.skeleton = " << formatEntity(model.skeleton, entityVarNames) << ";\n";
    if (!model.animations.empty()) {
        code << ind << "modelcomp.animations.clear();\n";
        for (size_t i = 0; i < model.animations.size(); i++) {
            code << ind << "modelcomp.animations.push_back(" << formatEntity(model.animations[i], entityVarNames) << ");\n";
        }
    }
    for (const auto& [id, entity_val] : model.bonesIdMapping) {
        code << ind << "modelcomp.bonesIdMapping[" << formatInt(id) << "] = " << formatEntity(entity_val, entityVarNames) << ";\n";
    }
    for (const auto& [name, entity_val] : model.bonesNameMapping) {
        code << ind << "modelcomp.bonesNameMapping[" << formatString(name) << "] = " << formatEntity(entity_val, entityVarNames) << ";\n";
    }
    for (const auto& [nodeIdx, entity_val] : model.meshNodesMapping) {
        code << ind << "modelcomp.meshNodesMapping[" << formatInt(nodeIdx) << "] = " << formatEntity(entity_val, entityVarNames) << ";\n";
    }
    for (const auto& [nodeIdx, entity_val] : model.nodesIdMapping) {
        code << ind << "modelcomp.nodesIdMapping[" << formatInt(nodeIdx) << "] = " << formatEntity(entity_val, entityVarNames) << ";\n";
    }
    for (const auto& [name, index] : model.morphNameMapping) {
        code << ind << "modelcomp.morphNameMapping[" << formatString(name) << "] = " << formatInt(index) << ";\n";
    }

    if (!model.enabled) code << ind << "modelcomp.enabled = false;\n";

    addComponentCode(code, ind, sceneName, entityName, entity, "ModelComponent", "modelcomp", assignExisting);
    return code.str();
}

// Emit "<target> = { v0, v1, ... };" for a component list member; empty lists
// emit nothing (component default). Shared by the keyframe track components.
template<typename T, typename FormatFn>
static void emitValueList(std::ostringstream& code, const std::string& ind, const std::string& target, const std::vector<T>& values, FormatFn format){
    if (values.empty()) return;
    code << ind << target << " = {";
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0) code << ", ";
        code << format(values[i]);
    }
    code << "};\n";
}

template<typename FormatFn>
static void emitFloatLists(std::ostringstream& code, const std::string& ind, const std::string& target, const std::vector<std::vector<float>>& lists, FormatFn format){
    if (lists.empty()) return;
    code << ind << target << ".resize(" << lists.size() << ");\n";
    for (size_t i = 0; i < lists.size(); i++) {
        emitValueList(code, ind, target + "[" + std::to_string(i) + "]", lists[i], format);
    }
}

std::string editor::Factory::createKeyframeTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<KeyframeTracksComponent>(entity)) return "";
    KeyframeTracksComponent& kf = scene->getComponent<KeyframeTracksComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "KeyframeTracksComponent kfcomp;\n";
    code << ind << "kfcomp.index = " << formatInt(kf.index) << ";\n";
    code << ind << "kfcomp.interpolation = " << formatFloat(kf.interpolation) << ";\n";
    code << ind << "kfcomp.loop = " << formatBool(kf.loop) << ";\n";
    code << ind << "kfcomp.relative = " << formatBool(kf.relative) << ";\n";
    if (!kf.times.empty()) {
        code << ind << "kfcomp.times = {";
        for (size_t i = 0; i < kf.times.size(); i++) {
            if (i > 0) code << ", ";
            code << formatFloat(kf.times[i]);
        }
        code << "};\n";
    }
    if (!kf.easings.empty()) {
        code << ind << "kfcomp.easings = {";
        for (size_t i = 0; i < kf.easings.size(); i++) {
            if (i > 0) code << ", ";
            code << formatEaseType(kf.easings[i]);
        }
        code << "};\n";
    }
    addComponentCode(code, ind, sceneName, entityName, entity, "KeyframeTracksComponent", "kfcomp", assignExisting);
    return code.str();
}

std::string editor::Factory::createTranslateTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<TranslateTracksComponent>(entity)) return "";
    TranslateTracksComponent& tt = scene->getComponent<TranslateTracksComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "TranslateTracksComponent ttcomp;\n";
    emitValueList(code, ind, "ttcomp.values", tt.values, formatVector3);
    // Hermite tangents only exist on GLTF CUBICSPLINE clips
    emitValueList(code, ind, "ttcomp.inTangents", tt.inTangents, formatVector3);
    emitValueList(code, ind, "ttcomp.outTangents", tt.outTangents, formatVector3);
    addComponentCode(code, ind, sceneName, entityName, entity, "TranslateTracksComponent", "ttcomp", assignExisting);
    return code.str();
}

std::string editor::Factory::createRotateTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<RotateTracksComponent>(entity)) return "";
    RotateTracksComponent& rt = scene->getComponent<RotateTracksComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "RotateTracksComponent rtcomp;\n";
    emitValueList(code, ind, "rtcomp.values", rt.values, formatQuaternion);
    // Hermite tangents only exist on GLTF CUBICSPLINE clips
    emitValueList(code, ind, "rtcomp.inTangents", rt.inTangents, formatQuaternion);
    emitValueList(code, ind, "rtcomp.outTangents", rt.outTangents, formatQuaternion);
    addComponentCode(code, ind, sceneName, entityName, entity, "RotateTracksComponent", "rtcomp", assignExisting);
    return code.str();
}

std::string editor::Factory::createScaleTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<ScaleTracksComponent>(entity)) return "";
    ScaleTracksComponent& st = scene->getComponent<ScaleTracksComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "ScaleTracksComponent stcomp;\n";
    emitValueList(code, ind, "stcomp.values", st.values, formatVector3);
    // Hermite tangents only exist on GLTF CUBICSPLINE clips
    emitValueList(code, ind, "stcomp.inTangents", st.inTangents, formatVector3);
    emitValueList(code, ind, "stcomp.outTangents", st.outTangents, formatVector3);
    addComponentCode(code, ind, sceneName, entityName, entity, "ScaleTracksComponent", "stcomp", assignExisting);
    return code.str();
}

std::string editor::Factory::createMorphTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<MorphTracksComponent>(entity)) return "";
    MorphTracksComponent& mt = scene->getComponent<MorphTracksComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "MorphTracksComponent mtcomp;\n";
    emitFloatLists(code, ind, "mtcomp.values", mt.values, formatFloat);
    // Hermite tangents only exist on GLTF CUBICSPLINE clips
    emitFloatLists(code, ind, "mtcomp.inTangents", mt.inTangents, formatFloat);
    emitFloatLists(code, ind, "mtcomp.outTangents", mt.outTangents, formatFloat);
    addComponentCode(code, ind, sceneName, entityName, entity, "MorphTracksComponent", "mtcomp", assignExisting);
    return code.str();
}

std::string editor::Factory::createFluidSimulationComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    (void)entityVarNames;
    if (!scene->findComponent<FluidSimulationComponent>(entity)) return "";
    FluidSimulationComponent& fluid = scene->getComponent<FluidSimulationComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "FluidSimulationComponent fluid;\n";
    if (!fluid.enabled) code << ind << "fluid.enabled = false;\n";
    // only what differs from the defaults (Pavel's); the runtime fields start fresh
    const FluidSimulationComponent defaults;
    if (fluid.output != defaults.output) code << ind << "fluid.output = " << "FluidOutput::" << (fluid.output == FluidOutput::SCREEN ? "SCREEN" : fluid.output == FluidOutput::TEXTURE ? "TEXTURE" : "MATERIAL") << ";\n";
    if (fluid.interactive != defaults.interactive) code << ind << "fluid.interactive = " << formatBool(fluid.interactive) << ";\n";
    if (fluid.aspectRatio != defaults.aspectRatio) code << ind << "fluid.aspectRatio = " << formatFloat(fluid.aspectRatio) << ";\n";
    if (fluid.simResolution != defaults.simResolution) code << ind << "fluid.simResolution = " << formatUInt(fluid.simResolution) << "u" << ";\n";
    if (fluid.dyeResolution != defaults.dyeResolution) code << ind << "fluid.dyeResolution = " << formatUInt(fluid.dyeResolution) << "u" << ";\n";
    if (fluid.densityDissipation != defaults.densityDissipation) code << ind << "fluid.densityDissipation = " << formatFloat(fluid.densityDissipation) << ";\n";
    if (fluid.velocityDissipation != defaults.velocityDissipation) code << ind << "fluid.velocityDissipation = " << formatFloat(fluid.velocityDissipation) << ";\n";
    if (fluid.pressure != defaults.pressure) code << ind << "fluid.pressure = " << formatFloat(fluid.pressure) << ";\n";
    if (fluid.pressureIterations != defaults.pressureIterations) code << ind << "fluid.pressureIterations = " << formatUInt(fluid.pressureIterations) << "u" << ";\n";
    if (fluid.curl != defaults.curl) code << ind << "fluid.curl = " << formatFloat(fluid.curl) << ";\n";
    if (fluid.gradientScale != defaults.gradientScale) code << ind << "fluid.gradientScale = " << formatFloat(fluid.gradientScale) << ";\n";
    if (fluid.simulationRate != defaults.simulationRate) code << ind << "fluid.simulationRate = " << formatFloat(fluid.simulationRate) << ";\n";
    if (fluid.maxSubsteps != defaults.maxSubsteps) code << ind << "fluid.maxSubsteps = " << formatUInt(fluid.maxSubsteps) << "u" << ";\n";
    if (fluid.timeScale != defaults.timeScale) code << ind << "fluid.timeScale = " << formatFloat(fluid.timeScale) << ";\n";
    if (fluid.paused != defaults.paused) code << ind << "fluid.paused = " << formatBool(fluid.paused) << ";\n";
    if (fluid.splatRadius != defaults.splatRadius) code << ind << "fluid.splatRadius = " << formatFloat(fluid.splatRadius) << ";\n";
    if (fluid.splatForce != defaults.splatForce) code << ind << "fluid.splatForce = " << formatFloat(fluid.splatForce) << ";\n";
    if (fluid.colorful != defaults.colorful) code << ind << "fluid.colorful = " << formatBool(fluid.colorful) << ";\n";
    if (fluid.colorUpdateSpeed != defaults.colorUpdateSpeed) code << ind << "fluid.colorUpdateSpeed = " << formatFloat(fluid.colorUpdateSpeed) << ";\n";
    if (fluid.splatsOnStart != defaults.splatsOnStart) code << ind << "fluid.splatsOnStart = " << formatBool(fluid.splatsOnStart) << ";\n";
    if (fluid.randomSplatCount != defaults.randomSplatCount) code << ind << "fluid.randomSplatCount = " << formatUInt(fluid.randomSplatCount) << "u" << ";\n";
    if (fluid.seed != defaults.seed) code << ind << "fluid.seed = " << formatUInt(fluid.seed) << "u" << ";\n";
    if (fluid.shading != defaults.shading) code << ind << "fluid.shading = " << formatBool(fluid.shading) << ";\n";
    if (fluid.backColor != defaults.backColor) code << ind << "fluid.backColor = " << formatVector3(fluid.backColor) << ";\n";
    if (fluid.transparent != defaults.transparent) code << ind << "fluid.transparent = " << formatBool(fluid.transparent) << ";\n";
    if (fluid.bloom != defaults.bloom) code << ind << "fluid.bloom = " << formatBool(fluid.bloom) << ";\n";
    if (fluid.bloomIterations != defaults.bloomIterations) code << ind << "fluid.bloomIterations = " << formatUInt(fluid.bloomIterations) << "u" << ";\n";
    if (fluid.bloomResolution != defaults.bloomResolution) code << ind << "fluid.bloomResolution = " << formatUInt(fluid.bloomResolution) << "u" << ";\n";
    if (fluid.bloomIntensity != defaults.bloomIntensity) code << ind << "fluid.bloomIntensity = " << formatFloat(fluid.bloomIntensity) << ";\n";
    if (fluid.bloomThreshold != defaults.bloomThreshold) code << ind << "fluid.bloomThreshold = " << formatFloat(fluid.bloomThreshold) << ";\n";
    if (fluid.bloomSoftKnee != defaults.bloomSoftKnee) code << ind << "fluid.bloomSoftKnee = " << formatFloat(fluid.bloomSoftKnee) << ";\n";
    if (fluid.sunrays != defaults.sunrays) code << ind << "fluid.sunrays = " << formatBool(fluid.sunrays) << ";\n";
    if (fluid.sunraysResolution != defaults.sunraysResolution) code << ind << "fluid.sunraysResolution = " << formatUInt(fluid.sunraysResolution) << "u" << ";\n";
    if (fluid.sunraysWeight != defaults.sunraysWeight) code << ind << "fluid.sunraysWeight = " << formatFloat(fluid.sunraysWeight) << ";\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "FluidSimulationComponent", "fluid", assignExisting);
    return code.str();
}

static const char* formatLatticeInterpolation(LatticeInterpolation interpolation) {
    switch (interpolation) {
        case LatticeInterpolation::SHARP: return "LatticeInterpolation::SHARP";
        case LatticeInterpolation::SMOOTH: return "LatticeInterpolation::SMOOTH";
        case LatticeInterpolation::CUBIC: break;
    }
    return "LatticeInterpolation::CUBIC";
}

static const char* formatLatticeOutside(LatticeOutside outside) {
    return outside == LatticeOutside::EXTEND ? "LatticeOutside::EXTEND" : "LatticeOutside::FADE";
}

static const char* formatLatticeJiggleAnchor(LatticeJiggleAnchor anchor) {
    switch (anchor) {
        case LatticeJiggleAnchor::NEGATIVE_X: return "LatticeJiggleAnchor::NEGATIVE_X";
        case LatticeJiggleAnchor::POSITIVE_X: return "LatticeJiggleAnchor::POSITIVE_X";
        case LatticeJiggleAnchor::NEGATIVE_Y: return "LatticeJiggleAnchor::NEGATIVE_Y";
        case LatticeJiggleAnchor::POSITIVE_Y: return "LatticeJiggleAnchor::POSITIVE_Y";
        case LatticeJiggleAnchor::NEGATIVE_Z: return "LatticeJiggleAnchor::NEGATIVE_Z";
        case LatticeJiggleAnchor::POSITIVE_Z: return "LatticeJiggleAnchor::POSITIVE_Z";
        case LatticeJiggleAnchor::NONE: break;
    }
    return "LatticeJiggleAnchor::NONE";
}

static const char* formatLatticeStage(LatticeStage stage) {
    return stage == LatticeStage::BEFORE_SKINNING ? "LatticeStage::BEFORE_SKINNING" : "LatticeStage::AFTER_SKINNING";
}

static const char* formatLatticeMaskChannel(LatticeMaskChannel mask) {
    switch (mask) {
        case LatticeMaskChannel::RED: return "LatticeMaskChannel::RED";
        case LatticeMaskChannel::GREEN: return "LatticeMaskChannel::GREEN";
        case LatticeMaskChannel::BLUE: return "LatticeMaskChannel::BLUE";
        case LatticeMaskChannel::ALPHA: return "LatticeMaskChannel::ALPHA";
        case LatticeMaskChannel::INVERTED_RED: return "LatticeMaskChannel::INVERTED_RED";
        case LatticeMaskChannel::INVERTED_GREEN: return "LatticeMaskChannel::INVERTED_GREEN";
        case LatticeMaskChannel::INVERTED_BLUE: return "LatticeMaskChannel::INVERTED_BLUE";
        case LatticeMaskChannel::INVERTED_ALPHA: return "LatticeMaskChannel::INVERTED_ALPHA";
        case LatticeMaskChannel::NONE: break;
    }
    return "LatticeMaskChannel::NONE";
}

std::string editor::Factory::createLatticeComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<LatticeComponent>(entity)) return "";
    LatticeComponent& lattice = scene->getComponent<LatticeComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "LatticeComponent lattice;\n";
    code << ind << "lattice.resolutionX = " << formatInt(lattice.resolutionX) << ";\n";
    code << ind << "lattice.resolutionY = " << formatInt(lattice.resolutionY) << ";\n";
    code << ind << "lattice.resolutionZ = " << formatInt(lattice.resolutionZ) << ";\n";
    // zero offsets are the rest positions: only the moved points are written
    code << ind << "lattice.offsets.assign(" << lattice.offsets.size() << ", Vector3(0.0f, 0.0f, 0.0f));\n";
    for (size_t i = 0; i < lattice.offsets.size(); i++) {
        if (lattice.offsets[i] != Vector3::ZERO) {
            code << ind << "lattice.offsets[" << i << "] = " << formatVector3(lattice.offsets[i]) << ";\n";
        }
    }
    code << ind << "lattice.interpolation = " << formatLatticeInterpolation(lattice.interpolation) << ";\n";
    code << ind << "lattice.outside = " << formatLatticeOutside(lattice.outside) << ";\n";
    code << ind << "lattice.fadeDistance = " << formatFloat(lattice.fadeDistance) << ";\n";
    if (!lattice.poses.empty()) {
        code << ind << "lattice.poses.resize(" << lattice.poses.size() << ");\n";
        for (size_t i = 0; i < lattice.poses.size(); i++) {
            const std::string pose = "lattice.poses[" + std::to_string(i) + "]";
            code << ind << pose << ".name = " << formatString(lattice.poses[i].name) << ";\n";
            emitValueList(code, ind, pose + ".offsets", lattice.poses[i].offsets, formatVector3);
        }
    }
    code << ind << "lattice.jiggle = " << formatBool(lattice.jiggle) << ";\n";
    code << ind << "lattice.jiggleStiffness = " << formatFloat(lattice.jiggleStiffness) << ";\n";
    code << ind << "lattice.jiggleDamping = " << formatFloat(lattice.jiggleDamping) << ";\n";
    code << ind << "lattice.jiggleInertia = " << formatFloat(lattice.jiggleInertia) << ";\n";
    code << ind << "lattice.jiggleAnchor = " << formatLatticeJiggleAnchor(lattice.jiggleAnchor) << ";\n";
    code << ind << "lattice.jiggleMaxOffset = " << formatFloat(lattice.jiggleMaxOffset) << ";\n";
    if (!lattice.enabled) code << ind << "lattice.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "LatticeComponent", "lattice", assignExisting);
    return code.str();
}

std::string editor::Factory::createLatticeModifierComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<LatticeModifierComponent>(entity)) return "";
    LatticeModifierComponent& modifier = scene->getComponent<LatticeModifierComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "LatticeModifierComponent latticemodifier;\n";
    if (!modifier.bindings.empty()) {
        code << ind << "latticemodifier.bindings.resize(" << modifier.bindings.size() << ");\n";
        for (size_t i = 0; i < modifier.bindings.size(); i++) {
            const LatticeBinding& binding = modifier.bindings[i];
            const std::string target = "latticemodifier.bindings[" + std::to_string(i) + "]";
            code << ind << target << ".lattice = " << formatEntity(binding.lattice, entityVarNames) << ";\n";
            code << ind << target << ".weight = " << formatFloat(binding.weight) << ";\n";
            code << ind << target << ".stage = " << formatLatticeStage(binding.stage) << ";\n";
            code << ind << target << ".submesh = " << formatInt(binding.submesh) << ";\n";
            code << ind << target << ".mask = " << formatLatticeMaskChannel(binding.mask) << ";\n";
        }
    }
    code << ind << "latticemodifier.includeChildren = " << formatBool(modifier.includeChildren) << ";\n";
    code << ind << "latticemodifier.subdivisions = " << formatInt(modifier.subdivisions) << ";\n";
    if (!modifier.enabled) code << ind << "latticemodifier.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "LatticeModifierComponent", "latticemodifier", assignExisting);
    return code.str();
}

std::string editor::Factory::createCameraShakerComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    (void)entityVarNames;
    if (!scene->findComponent<CameraShakerComponent>(entity)) return "";
    const CameraShakerComponent& shaker = scene->getComponent<CameraShakerComponent>(entity);
    const CameraShakerComponent defaults;
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "CameraShakerComponent camerashaker;\n";
    if (!shaker.enabled) code << ind << "camerashaker.enabled = false;\n";
    if (shaker.global != defaults.global) code << ind << "camerashaker.global = " << formatBool(shaker.global) << ";\n";
    if (shaker.unscaledTime != defaults.unscaledTime) code << ind << "camerashaker.unscaledTime = " << formatBool(shaker.unscaledTime) << ";\n";
    if (shaker.mode == CameraShakeMode::Mode3D) code << ind << "camerashaker.mode = CameraShakeMode::Mode3D;\n";
    if (shaker.mode == CameraShakeMode::Mode2D) code << ind << "camerashaker.mode = CameraShakeMode::Mode2D;\n";
    code << ind << "camerashaker.positionScale = " << formatFloat(shaker.positionScale) << ";\n";
    code << ind << "camerashaker.rotationScale = " << formatFloat(shaker.rotationScale) << ";\n";
    if (!shaker.startPreset.empty()) code << ind << "camerashaker.startPreset = " << formatString(shaker.startPreset) << ";\n";
    code << ind << "camerashaker.traumaDecay = " << formatFloat(shaker.traumaDecay) << ";\n";
    code << ind << "camerashaker.traumaPower = " << formatFloat(shaker.traumaPower) << ";\n";
    code << ind << "camerashaker.traumaMaxOffset = " << formatVector3(shaker.traumaMaxOffset) << ";\n";
    code << ind << "camerashaker.traumaMaxAngle = " << formatVector3(shaker.traumaMaxAngle) << ";\n";
    code << ind << "camerashaker.traumaFrequency = " << formatFloat(shaker.traumaFrequency) << ";\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "CameraShakerComponent", "camerashaker", assignExisting);
    return code.str();
}

std::string editor::Factory::createCameraFollowComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<CameraFollowComponent>(entity)) return "";
    const CameraFollowComponent& follow = scene->getComponent<CameraFollowComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "CameraFollowComponent camerafollow;\n";
    if (!follow.enabled) code << ind << "camerafollow.enabled = false;\n";
    code << ind << "camerafollow.target = " << formatEntity(follow.target, entityVarNames) << ";\n";
    code << ind << "camerafollow.offset = " << formatVector3(follow.offset) << ";\n";
    if (follow.offsetSpace == FollowOffsetSpace::Target) code << ind << "camerafollow.offsetSpace = FollowOffsetSpace::Target;\n";
    if (follow.offsetSpace == FollowOffsetSpace::TargetYaw) code << ind << "camerafollow.offsetSpace = FollowOffsetSpace::TargetYaw;\n";
    code << ind << "camerafollow.damping = " << formatVector3(follow.damping) << ";\n";
    code << ind << "camerafollow.maxSpeed = " << formatFloat(follow.maxSpeed) << ";\n";
    code << ind << "camerafollow.lookAt = " << formatBool(follow.lookAt) << ";\n";
    code << ind << "camerafollow.lookAtOffset = " << formatVector3(follow.lookAtOffset) << ";\n";
    code << ind << "camerafollow.rotationDamping = " << formatFloat(follow.rotationDamping) << ";\n";
    code << ind << "camerafollow.deadZone = " << formatVector2(follow.deadZone) << ";\n";
    code << ind << "camerafollow.useBounds = " << formatBool(follow.useBounds) << ";\n";
    code << ind << "camerafollow.boundsMin = " << formatVector3(follow.boundsMin) << ";\n";
    code << ind << "camerafollow.boundsMax = " << formatVector3(follow.boundsMax) << ";\n";
    code << ind << "camerafollow.unscaledTime = " << formatBool(follow.unscaledTime) << ";\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "CameraFollowComponent", "camerafollow", assignExisting);
    return code.str();
}

std::string editor::Factory::createLatticeFollowerComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<LatticeFollowerComponent>(entity)) return "";
    LatticeFollowerComponent& follower = scene->getComponent<LatticeFollowerComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "LatticeFollowerComponent latticefollower;\n";
    code << ind << "latticefollower.lattice = " << formatEntity(follower.lattice, entityVarNames) << ";\n";
    code << ind << "latticefollower.rest = " << formatEntity(follower.rest, entityVarNames) << ";\n";
    code << ind << "latticefollower.applyPosition = " << formatBool(follower.applyPosition) << ";\n";
    code << ind << "latticefollower.applyRotation = " << formatBool(follower.applyRotation) << ";\n";
    code << ind << "latticefollower.applyScale = " << formatBool(follower.applyScale) << ";\n";
    code << ind << "latticefollower.weight = " << formatFloat(follower.weight) << ";\n";
    if (!follower.enabled) code << ind << "latticefollower.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "LatticeFollowerComponent", "latticefollower", assignExisting);
    return code.str();
}

template<typename FormatFn>
static void emitVector3Lists(std::ostringstream& code, const std::string& ind, const std::string& target, const std::vector<std::vector<Vector3>>& lists, FormatFn format){
    if (lists.empty()) return;
    code << ind << target << ".resize(" << lists.size() << ");\n";
    for (size_t i = 0; i < lists.size(); i++) {
        emitValueList(code, ind, target + "[" + std::to_string(i) + "]", lists[i], format);
    }
}

std::string editor::Factory::createLatticeTracksComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<LatticeTracksComponent>(entity)) return "";
    LatticeTracksComponent& lt = scene->getComponent<LatticeTracksComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "LatticeTracksComponent ltcomp;\n";
    emitValueList(code, ind, "ltcomp.points", lt.points, formatInt);
    emitVector3Lists(code, ind, "ltcomp.values", lt.values, formatVector3);
    // Hermite tangents only exist on cubic tracks
    emitVector3Lists(code, ind, "ltcomp.inTangents", lt.inTangents, formatVector3);
    emitVector3Lists(code, ind, "ltcomp.outTangents", lt.outTangents, formatVector3);
    addComponentCode(code, ind, sceneName, entityName, entity, "LatticeTracksComponent", "ltcomp", assignExisting);
    return code.str();
}

std::string editor::Factory::createPoseAnimatorComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<PoseAnimatorComponent>(entity)) return "";
    PoseAnimatorComponent& animator = scene->getComponent<PoseAnimatorComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "PoseAnimatorComponent poseanimator;\n";
    code << ind << "poseanimator.animation = " << formatString(animator.animation) << ";\n";
    code << ind << "poseanimator.autoplay = " << formatBool(animator.autoplay) << ";\n";
    code << ind << "poseanimator.speed = " << formatFloat(animator.speed) << ";\n";
    code << ind << "poseanimator.weight = " << formatFloat(animator.weight) << ";\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "PoseAnimatorComponent", "poseanimator", assignExisting);
    return code.str();
}

static const char* formatClipWrapMode(ClipWrapMode wrap) {
    switch (wrap) {
        case ClipWrapMode::Once: return "ClipWrapMode::Once";
        case ClipWrapMode::Loop: return "ClipWrapMode::Loop";
        case ClipWrapMode::PingPong: return "ClipWrapMode::PingPong";
        case ClipWrapMode::ClampForever: return "ClipWrapMode::ClampForever";
        case ClipWrapMode::Default: break;
    }
    return "ClipWrapMode::Default";
}

std::string editor::Factory::createAnimationPlayerComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<AnimationPlayerComponent>(entity)) return "";
    AnimationPlayerComponent& player = scene->getComponent<AnimationPlayerComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "AnimationPlayerComponent animationplayer;\n";
    if (!player.enabled) code << ind << "animationplayer.enabled = false;\n";
    // the runtime loads them by path (the exporter copies every asset file); GUIDs are the editor's
    if (!player.clips.empty()) code << ind << "animationplayer.clips.resize(" << player.clips.size() << ");\n";
    for (size_t i = 0; i < player.clips.size(); i++) {
        const AnimationPlayerClip& entry = player.clips[i];
        const std::string target = "animationplayer.clips[" + std::to_string(i) + "]";
        code << ind << target << ".clip = " << formatString(entry.clip) << ";\n";
        if (!entry.name.empty()) code << ind << target << ".name = " << formatString(entry.name) << ";\n";
        if (entry.wrap != ClipWrapMode::Default) code << ind << target << ".wrap = " << formatClipWrapMode(entry.wrap) << ";\n";
        if (entry.layer != 0) code << ind << target << ".layer = " << entry.layer << ";\n";
        // humanoid playback (anim-retarget 7.1): an exported Retarget entry retargets as in the editor
        if (entry.retarget) code << ind << target << ".retarget = true;\n";
        if (entry.mirror) code << ind << target << ".mirror = true;\n";
        if (entry.footIK) code << ind << target << ".footIK = true;\n";
    }
    // the layers (upstream anim layers spec 6.3): masks load by path, their GUIDs are the editor's
    if (!player.layers.empty()) code << ind << "animationplayer.layers.resize(" << player.layers.size() << ");\n";
    for (size_t i = 0; i < player.layers.size(); i++) {
        const AnimationPlayerLayer& layer = player.layers[i];
        const std::string target = "animationplayer.layers[" + std::to_string(i) + "]";
        code << ind << target << ".name = " << formatString(layer.name) << ";\n";
        if (layer.weight != 1.0f) code << ind << target << ".weight = " << formatFloat(layer.weight) << ";\n";
        if (layer.blend == AnimationLayerBlend::Additive) code << ind << target << ".blend = AnimationLayerBlend::Additive;\n";
        if (!layer.mask.empty()) code << ind << target << ".mask = " << formatString(layer.mask) << ";\n";
    }
    if (!player.defaultClip.empty()) code << ind << "animationplayer.defaultClip = " << formatString(player.defaultClip) << ";\n";
    code << ind << "animationplayer.playAutomatically = " << formatBool(player.playAutomatically) << ";\n";
    code << ind << "animationplayer.speed = " << formatFloat(player.speed) << ";\n";
    code << ind << "animationplayer.defaultFadeTime = " << formatFloat(player.defaultFadeTime) << ";\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "AnimationPlayerComponent", "animationplayer", assignExisting);
    return code.str();
}

std::string editor::Factory::createVertexAnimatorComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<VertexAnimatorComponent>(entity)) return "";
    VertexAnimatorComponent& animator = scene->getComponent<VertexAnimatorComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "VertexAnimatorComponent vertexanimator;\n";
    if (!animator.enabled) code << ind << "vertexanimator.enabled = false;\n";
    // the runtime loads it by path (the exporter copies the .vat and its .vat.glb); the GUID is the editor's
    code << ind << "vertexanimator.animation = " << formatString(animator.animation) << ";\n";
    if (!animator.defaultClip.empty()) code << ind << "vertexanimator.defaultClip = " << formatString(animator.defaultClip) << ";\n";
    code << ind << "vertexanimator.playAutomatically = " << formatBool(animator.playAutomatically) << ";\n";
    code << ind << "vertexanimator.speed = " << formatFloat(animator.speed) << ";\n";
    code << ind << "vertexanimator.randomStart = " << formatBool(animator.randomStart) << ";\n";
    code << ind << "vertexanimator.defaultFadeTime = " << formatFloat(animator.defaultFadeTime) << ";\n";
    if (animator.instanceEvents) code << ind << "vertexanimator.instanceEvents = true;\n";
    if (!animator.attachments.empty()) code << ind << "vertexanimator.attachments.resize(" << animator.attachments.size() << ");\n";
    for (size_t i = 0; i < animator.attachments.size(); i++) {
        const VertexAnimatorAttachment& attachment = animator.attachments[i];
        const std::string target = "vertexanimator.attachments[" + std::to_string(i) + "]";
        code << ind << target << ".socket = " << formatString(attachment.socket) << ";\n";
        code << ind << target << ".entity = " << formatEntity(attachment.entity, entityVarNames) << ";\n";
    }
    // per-instance starts (Create Crowd)
    if (!animator.instanceClips.empty()) {
        code << ind << "vertexanimator.instanceClips = {";
        for (size_t i = 0; i < animator.instanceClips.size(); i++) {
            code << (i ? ", " : "") << formatString(animator.instanceClips[i]);
        }
        code << "};\n";
    }
    if (!animator.instanceSpeeds.empty()) {
        code << ind << "vertexanimator.instanceSpeeds = {";
        for (size_t i = 0; i < animator.instanceSpeeds.size(); i++) {
            code << (i ? ", " : "") << formatFloat(std::isfinite(animator.instanceSpeeds[i]) ? animator.instanceSpeeds[i] : 1.0f);
        }
        code << "};\n";
    }
    addComponentCode(code, ind, sceneName, entityName, entity, "VertexAnimatorComponent", "vertexanimator", assignExisting);
    return code.str();
}

std::string editor::Factory::createComponent(int indentSpaces, EntityRegistry* scene, Entity entity, ComponentType componentType, const fs::path& projectPath, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    // the component's exported code from its reflection table (editor/reflect/Codegen.cpp), or
    // its own writer for those the table does not describe
    const reflect::ComponentDesc* desc = reflect::find(componentType);
    void* comp = desc ? desc->find(scene, entity) : nullptr;
    if (!comp) return "";
    reflect::CodegenContext ctx;
    ctx.indentSpaces = indentSpaces;
    ctx.variable = desc->codegenVar ? desc->codegenVar : "";
    ctx.projectPath = projectPath;
    ctx.sceneName = sceneName;
    ctx.entityName = entityName;
    ctx.assignExisting = assignExisting;
    ctx.entityVarNames = entityVarNames;
    std::string code;
    if (desc->codegen) {
        code = desc->codegen(scene, entity, ctx);
    } else {
        std::ostringstream out;
        const std::string ind = indentation(indentSpaces);
        out << ind << desc->cppName << " " << ctx.variable << ";\n";
        out << reflect::cppAssignments(*desc, comp, ctx);
        addComponentCode(out, ind, sceneName, entityName, entity, desc->cppName, ctx.variable, assignExisting);
        code = out.str();
    }
    return desc->codegenGuard ? guardPhysicsCode(code, desc->codegenGuard) : code;
}

std::string editor::Factory::createAllComponents(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName, std::string entityName, const std::unordered_map<Entity, std::string>* entityVarNames) {
    // Find all components for this entity
    std::vector<ComponentType> components = Catalog::findComponents(scene, entity);

    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "// Entity components initialization\n";

    bool first = true;
    for (ComponentType componentType : components) {
        std::string componentCode = createComponent(indentSpaces, scene, entity, componentType, projectPath, sceneName, entityName, false, entityVarNames);
        if (!componentCode.empty()) {
            if (!first) {
                code << "\n";
            }
            code << componentCode;
            first = false;
        }
    }

    return code.str();
}

const char* const editor::Factory::sceneSourceHeader = "// This file is auto-generated by JasperEngine Editor (scene source 3). Do not edit manually.";

std::string editor::Factory::createScene(int indentSpaces, Scene* scene, uint32_t sceneId, std::string name, std::vector<Entity> entities, Entity camera, const fs::path& projectPath, const fs::path& generatedPath, const std::vector<PrefabInstanceInfo>& prefabInstances, const SceneResources& resources) {
    std::ostringstream out;

    std::string funcId = sceneToFileName(sceneId, name);
    const std::string ind = indentation(indentSpaces);

    // Build lookup sets for prefab entities
    std::set<Entity> prefabRoots;
    std::set<Entity> prefabMemberEntities;
    std::map<Entity, const PrefabInstanceInfo*> rootToInstance;
    std::set<std::string> prefabHeaderIncludes;

    for (const auto& bi : prefabInstances) {
        prefabRoots.insert(bi.rootEntity);
        rootToInstance[bi.rootEntity] = &bi;
        // (a variant's instance: its base levels' GameObjects are the build's too)
        for (const PrefabInstanceInfo* level = &bi; level; level = level->baseLevel.get()) {
            for (Entity member : level->memberEntities) {
                prefabMemberEntities.insert(member);
            }
        }
        std::string headerName = prefabToFileName(bi.prefabPath) + ".h";
        prefabHeaderIncludes.insert(headerName);
    }

    bool usesDefaultSky = false;
    for (Entity entity : entities) {
        if (prefabMemberEntities.count(entity)) continue;
        if (scene->findComponent<SkyboxComponent>(entity)) {
            SkyboxComponent& sky = scene->getComponent<SkyboxComponent>(entity);
            if (sky.texture.getId() == DEFAULT_SKY_ID) {
                usesDefaultSky = true;
                break;
            }
        }
    }

    out << ind << sceneSourceHeader << "\n\n";
    out << ind << "#include \"JasperEngine.h\"\n";
    for (const auto& headerName : prefabHeaderIncludes) {
        out << ind << "#include \"" << headerName << "\"\n";
    }
    writeSkyIncludes(out, usesDefaultSky, generatedPath, ind);
    out << ind << "using namespace jasperengine;\n\n";
    out << ind << "extern \"C\" void initScripts(jasperengine::Scene* scene);\n\n";

    out << ind << "void create_" << funcId << "(Scene* scene){\n";

    const std::string ind2 = indentation(indentSpaces+4);
    const std::string ind3 = indentation(indentSpaces+8);

    // all at once, so a prefab factory never allocates an id the scene uses later
    out << ind2 << "// Scene entities\n";
    Entity lastSceneEntity = NULL_ENTITY;
    for (Entity entity : entities) {
        if (prefabMemberEntities.count(entity)) continue;
        out << ind2 << "scene->recreateEntity(" << entity << ");\n";
        lastSceneEntity = std::max(lastSceneEntity, entity);
    }
    // recreateEntity leaves the allocator where it is, so lift it past those ids by hand
    if (lastSceneEntity != NULL_ENTITY) {
        out << ind2 << "scene->setLastEntity(" << lastSceneEntity << ");\n";
    }
    out << "\n";

    // members only get ids at runtime, so the scene reaches them through the vector
    // each top-level instance fills; nested instances continue it
    std::unordered_map<Entity, std::string> memberVarNames;
    std::unordered_map<Entity, std::string> builtVarNames; // NULL_ENTITY until the instance is built
    std::map<Entity, std::vector<Entity>> rootMembers;
    std::vector<Entity> topLevelRoots;

    auto membersVar = [](Entity root) { return "prefabMembers_" + std::to_string(root); };

    auto mapInstanceMembers = [&](auto&& self, const PrefabInstanceInfo* bi, Entity topRoot, size_t base) -> void {
        for (size_t i = 0; i < bi->orderedMembers.size(); i++) {
            Entity member = bi->orderedMembers[i];
            if (member == NULL_ENTITY) continue;
            memberVarNames[member] = membersVar(topRoot) + "[" + std::to_string(base + i) + "]";
            builtVarNames[member] = "NULL_ENTITY";
            rootMembers[topRoot].push_back(member);
        }
        // the factory appends each nested subtree whether or not this scene has an instance for
        // it; a variant's base subtree first
        size_t next = base + bi->orderedMembers.size();
        if (bi->baseLevel) {
            self(self, bi->baseLevel.get(), topRoot, next);
        }
        next += bi->baseSize;
        for (size_t i = 0; i < bi->nestedSizes.size(); i++) {
            if (bi->nestedSizes[i] == 0) continue;
            auto nestedIt = rootToInstance.find(bi->orderedMembers[i]);
            if (nestedIt != rootToInstance.end()) self(self, nestedIt->second, topRoot, next);
            next += bi->nestedSizes[i];
        }
    };

    for (Entity entity : entities) {
        if (prefabRoots.count(entity) && !prefabMemberEntities.count(entity)) {
            topLevelRoots.push_back(entity);
            mapInstanceMembers(mapInstanceMembers, rootToInstance[entity], entity, 0);
        }
    }

    // Removed GameObject overrides: the factory builds the whole prefab, so the members an
    // instance (or one nested in it) left out are destroyed right after, by their index,
    // with the whole subtree of a nested root that is left out
    std::map<Entity, std::vector<size_t>> absentMembers;
    auto mapAbsentMembers = [&](auto&& self, const PrefabInstanceInfo* bi, Entity topRoot, size_t base) -> void {
        size_t next = base + bi->orderedMembers.size();
        if (bi->baseLevel) {
            self(self, bi->baseLevel.get(), topRoot, next);
        }
        next += bi->baseSize;
        for (size_t i = 0; i < bi->orderedMembers.size(); i++) {
            const size_t nestedSize = i < bi->nestedSizes.size() ? bi->nestedSizes[i] : 0;
            if (bi->orderedMembers[i] == NULL_ENTITY) {
                absentMembers[topRoot].push_back(base + i);
                for (size_t n = 0; n < nestedSize; n++) {
                    absentMembers[topRoot].push_back(next + n);
                }
            } else if (nestedSize > 0) {
                auto nestedIt = rootToInstance.find(bi->orderedMembers[i]);
                if (nestedIt != rootToInstance.end()) self(self, nestedIt->second, topRoot, next);
            }
            next += nestedSize;
        }
    };
    for (Entity root : topLevelRoots) {
        mapAbsentMembers(mapAbsentMembers, rootToInstance[root], root, 0);
    }

    // a component reading a vector not filled yet is added with those references
    // cleared and assigned again once every instance exists
    std::set<Entity> builtRoots;
    std::vector<std::pair<Entity, ComponentType>> deferredComponents;

    auto referencesUnbuilt = [&](const std::string& code) {
        for (Entity root : topLevelRoots) {
            if (!builtRoots.count(root) && code.find(membersVar(root) + "[") != std::string::npos) return true;
        }
        return false;
    };

    auto entityComponents = [&](Entity entity) {
        std::ostringstream code;
        code << ind3 << "// Entity components initialization\n";
        bool first = true;
        for (ComponentType compType : Catalog::findComponents(scene, entity)) {
            std::string componentCode = createComponent(indentSpaces+8, scene, entity, compType, projectPath, "scene", "", false, &memberVarNames);
            if (componentCode.empty()) continue;
            if (referencesUnbuilt(componentCode)) {
                componentCode = createComponent(indentSpaces+8, scene, entity, compType, projectPath, "scene", "", false, &builtVarNames);
                deferredComponents.push_back({entity, compType});
            }
            if (!first) code << "\n";
            code << componentCode;
            first = false;
        }
        return code.str();
    };

    // everything that could read a vector is generated first, so only the instances
    // something references get one
    std::vector<std::pair<Entity, std::string>> entityBlocks;
    for (Entity entity : entities) {
        if (prefabMemberEntities.count(entity)) continue;
        entityBlocks.push_back({entity, entityComponents(entity)});
        if (prefabRoots.count(entity)) {
            builtRoots.insert(entity);
            for (Entity member : rootMembers[entity]) {
                builtVarNames[member] = memberVarNames[member];
            }
        }
    }

    std::ostringstream deferred;
    for (const auto& [entity, compType] : deferredComponents) {
        deferred << ind2 << "{\n";
        deferred << ind3 << "// Entity " << entity << " (" << scene->getEntityName(entity) << ")\n";
        deferred << createComponent(indentSpaces+8, scene, entity, compType, projectPath, "scene", std::to_string(entity), true, &memberVarNames);
        deferred << ind2 << "}\n";
    }

    // overrides wait until every instance exists, nested ones included
    std::ostringstream overrides;
    auto emitInstanceOverrides = [&](auto&& self, const PrefabInstanceInfo* bi) -> void {
        for (const auto& ovr : bi->overrides) {
            auto targetIt = memberVarNames.find(ovr.sceneEntity);
            if (targetIt == memberVarNames.end()) {
                Out::warning("Override on entity %u (%s) has no member in %s, skipped", ovr.sceneEntity, scene->getEntityName(ovr.sceneEntity).c_str(), bi->prefabPath.string().c_str());
                continue;
            }
            overrides << ind2 << "{\n";
            overrides << ind3 << "// Override for " << scene->getEntityName(ovr.sceneEntity) << " (" << ovr.sceneEntity << ")\n";
            for (ComponentType compType : ovr.removedComponents) {
                overrides << guardComponentCode(compType, ind3 + "if (scene->findComponent<" + Catalog::getComponentName(compType) + ">(" + targetIt->second +
                    ")) scene->removeComponent<" + Catalog::getComponentName(compType) + ">(" + targetIt->second + ");\n");
            }
            for (ComponentType compType : ovr.addedComponents) {
                const std::string code = createComponent(indentSpaces+12, scene, ovr.sceneEntity, compType, projectPath, "scene", targetIt->second, false, &memberVarNames);
                if (!code.empty()) {
                    overrides << guardComponentCode(compType, ind3 + "if (!scene->findComponent<" + Catalog::getComponentName(compType) + ">(" + targetIt->second + ")) {\n" +
                        code + ind3 + "}\n");
                }
            }
            for (ComponentType compType : ovr.overriddenComponents) {
                overrides << createComponent(indentSpaces+8, scene, ovr.sceneEntity, compType, projectPath, "scene", targetIt->second, true, &memberVarNames);
            }
            if (ovr.state & PrefabObjectOverrides::StateName) {
                overrides << ind3 << "scene->setEntityName(" << targetIt->second << ", " << formatString(scene->getEntityName(ovr.sceneEntity)) << ");\n";
            }
            if (ovr.state & PrefabObjectOverrides::StateActive) {
                overrides << ind3 << "scene->setEntityActive(" << targetIt->second << ", " << formatBool(scene->isEntityActive(ovr.sceneEntity)) << ");\n";
            }
            if (ovr.state & PrefabObjectOverrides::StateTag) {
                overrides << ind3 << "scene->setEntityTag(" << targetIt->second << ", " << formatString(scene->getEntityTag(ovr.sceneEntity)) << ");\n";
            }
            if (ovr.state & PrefabObjectOverrides::StateLayer) {
                overrides << ind3 << "scene->setEntityLayer(" << targetIt->second << ", " << formatInt(scene->getEntityLayer(ovr.sceneEntity)) << ");\n";
            }
            overrides << ind2 << "}\n";
        }
        if (bi->baseLevel) {
            self(self, bi->baseLevel.get());
        }
        for (Entity member : bi->orderedMembers) {
            auto nestedIt = rootToInstance.find(member);
            if (nestedIt != rootToInstance.end()) self(self, nestedIt->second);
        }
    };
    for (Entity root : topLevelRoots) {
        emitInstanceOverrides(emitInstanceOverrides, rootToInstance[root]);
    }

    // Camera render-to-texture links; deferred so every camera entity already exists
    std::ostringstream links;
    links << createCameraTextureLinks(indentSpaces+4, scene, entities, &memberVarNames, false);

    std::ostringstream cameraCode;
    if (camera != NULL_ENTITY){
        if (std::find(entities.begin(), entities.end(), camera) == entities.end()) {
            cameraCode << "\n";
            cameraCode << ind2 << "// Default Camera " << camera << "\n";
            cameraCode << ind2 << "Entity cameraEntity = scene->createSystemEntity();\n";
            cameraCode << "\n";
            cameraCode << createAllComponents(indentSpaces+4, scene, camera, projectPath, "scene", "cameraEntity", nullptr);
        }else{
            cameraCode << "\n";
            cameraCode << ind2 << "Entity cameraEntity = " << formatEntity(camera, &memberVarNames) << ";\n";
        }
        cameraCode << "\n";
        cameraCode << ind2 << "scene->setCamera(cameraEntity);\n";
    }

    const std::string deferredStr = deferred.str();
    const std::string overridesStr = overrides.str();
    const std::string linksStr = links.str();
    const std::string cameraStr = cameraCode.str();

    auto usesMembers = [&](Entity root) {
        const std::string var = membersVar(root) + "[";
        auto has = [&](const std::string& code) { return code.find(var) != std::string::npos; };
        return has(deferredStr) || has(overridesStr) || has(linksStr) || has(cameraStr)
            || std::any_of(entityBlocks.begin(), entityBlocks.end(), [&](const auto& block) { return has(block.second); });
    };

    std::set<Entity> usedRoots;
    for (Entity root : topLevelRoots) {
        if (usesMembers(root) || absentMembers.count(root)) usedRoots.insert(root);
    }

    if (!usedRoots.empty()) {
        out << ind2 << "// Prefab instance members, by index\n";
        for (Entity root : topLevelRoots) {
            if (usedRoots.count(root)) out << ind2 << "std::vector<Entity> " << membersVar(root) << ";\n";
        }
        out << "\n";
    }

    // Components of the prefab's root GameObject an instance root removed: the build
    // completes the root with what it lacks
    auto writeRootRemovals = [&](const PrefabInstanceInfo* bi, Entity entity) {
        for (ComponentType compType : bi->rootRemovedComponents) {
            out << guardComponentCode(compType, ind3 + "if (scene->findComponent<" + Catalog::getComponentName(compType) + ">(" + std::to_string(entity) +
                ")) scene->removeComponent<" + Catalog::getComponentName(compType) + ">(" + std::to_string(entity) + ");\n");
        }
    };

    bool firstEntity = true;
    for (const auto& [entity, componentsCode] : entityBlocks) {
        if (!firstEntity) {
            out << "\n";
        }
        firstEntity = false;

        std::string entityName = scene->getEntityName(entity);
        out << ind2 << "{\n";
        if (prefabRoots.count(entity)) {
            const PrefabInstanceInfo* bi = rootToInstance[entity];
            out << ind3 << "// Prefab instance: " << bi->prefabPath.string() << " (" << entityName << ")\n";
            out << ind3 << "scene->setEntityName(" << entity << ", " << formatString(entityName) << ");\n";
            out << formatEntityState(ind3, std::to_string(entity), scene, entity) << "\n";
            out << componentsCode;
            out << "\n";
            // The root is the scene's (its own components and state): the build completes it
            if (const PrefabComponent* link = scene->findComponent<PrefabComponent>(entity)) {
                out << ind3 << "{\n";
                out << ind3 << "    PrefabComponent prefab;\n";
                out << ind3 << "    prefab.name = " << formatString(link->name) << ";\n";
                out << ind3 << "    prefab.path = " << formatString(fs::path(link->path).generic_string()) << ";\n";
                if (!link->guid.empty()) {
                    out << ind3 << "    prefab.guid = " << formatString(link->guid) << ";\n";
                }
                out << ind3 << "    scene->addComponent<PrefabComponent>(" << entity << ", prefab);\n";
                out << ind3 << "}\n";
            }
            if (usedRoots.count(entity)) {
                out << ind3 << prefabToBuildFunctionName(bi->prefabPath) << "(scene, " << entity << ", " << membersVar(entity) << ");\n";
                writeRootRemovals(bi, entity);
                auto absentIt = absentMembers.find(entity);
                if (absentIt != absentMembers.end()) {
                    // children before their parents
                    out << ind3 << "// Prefab GameObjects removed from this instance\n";
                    out << ind3 << "const size_t removedMembers[] = {";
                    for (size_t i = absentIt->second.size(); i-- > 0;) {
                        out << absentIt->second[i] << (i > 0 ? ", " : "");
                    }
                    out << "};\n";
                    out << ind3 << "for (size_t index : removedMembers) {\n";
                    out << ind3 << "    if (index < " << membersVar(entity) << ".size() && scene->isEntityCreated(" << membersVar(entity) << "[index]))\n";
                    out << ind3 << "        scene->destroyEntity(" << membersVar(entity) << "[index]);\n";
                    out << ind3 << "}\n";
                }
            } else {
                out << ind3 << prefabToFunctionName(bi->prefabPath) << "(scene, " << entity << ");\n";
                writeRootRemovals(bi, entity);
            }
        } else {
            out << ind3 << "// Entity " << entity << " (" << entityName << ")\n";
            out << ind3 << "scene->setEntityName(" << entity << ", " << formatString(entityName) << ");\n";
            out << formatEntityState(ind3, std::to_string(entity), scene, entity) << "\n";
            out << componentsCode;
        }
        out << ind2 << "}\n";
    }

    if (!deferredStr.empty()) {
        out << "\n";
        out << ind2 << "// References to members of instances built after the entity\n";
        out << deferredStr;
    }
    if (!overridesStr.empty()) {
        out << "\n";
        out << ind2 << "// Prefab member overrides\n";
        out << overridesStr;
    }
    if (!linksStr.empty()) {
        out << "\n";
        out << ind2 << "// Camera render-to-texture links\n";
        out << linksStr;
    }
    out << cameraStr;

    out << "\n";
    out << ind2 << "scene->setBackgroundColor(" << formatVector4(scene->getBackgroundColor()) << ");\n";
    out << ind2 << "scene->setShadowQuality(" << formatShadowQuality(scene->getShadowQuality()) << ");\n";
    if (scene->getShadowDistance() != SceneSettings().shadowDistance)
        out << ind2 << "scene->setShadowDistance(" << formatFloat(scene->getShadowDistance()) << ");\n";
    if (scene->getShadowFadeFraction() != SceneSettings().shadowFadeFraction)
        out << ind2 << "scene->setShadowFadeFraction(" << formatFloat(scene->getShadowFadeFraction()) << ");\n";
    if (scene->isShadowCascadeBlend() != SceneSettings().shadowCascadeBlend)
        out << ind2 << "scene->setShadowCascadeBlend(" << formatBool(scene->isShadowCascadeBlend()) << ");\n";
    out << ind2 << "scene->setMeshLodEnabled(" << formatBool(scene->isMeshLodEnabled()) << ");\n";
    out << ind2 << "scene->setMeshLodThreshold(" << formatFloat(scene->getMeshLodThreshold()) << ");\n";
    if (scene->isMeshLodCrossFade() != SceneSettings().meshLodCrossFade)
        out << ind2 << "scene->setMeshLodCrossFade(" << formatBool(scene->isMeshLodCrossFade()) << ");\n";
    if (scene->getMeshLodCrossFadeDuration() != SceneSettings().meshLodCrossFadeDuration)
        out << ind2 << "scene->setMeshLodCrossFadeDuration(" << formatFloat(scene->getMeshLodCrossFadeDuration()) << ");\n";
    out << ind2 << "scene->setDepthPrepassEnabled(" << formatBool(scene->isDepthPrepassEnabled()) << ");\n";
    if (scene->getSceneColorScale() != SceneSettings().sceneColorScale)
        out << ind2 << "scene->setSceneColorScale(" << formatSceneColorScale(scene->getSceneColorScale()) << ");\n";
    // planar reflections, away from their defaults
    {
        const SceneSettings defaults;
        if (scene->getPlanarReflectionResolution() != defaults.planarReflectionResolution){
            static const char* const names[] = {"OFF", "QUARTER", "HALF", "FULL"};
            out << ind2 << "scene->setPlanarReflectionResolution(PlanarReflectionResolution::" << names[(int)scene->getPlanarReflectionResolution()] << ");\n";
        }
        if (scene->getPlanarReflectionLayers() != defaults.planarReflectionLayers)
            out << ind2 << "scene->setPlanarReflectionLayers(" << scene->getPlanarReflectionLayers() << "u);\n";
        if (scene->getPlanarReflectionShadows() != defaults.planarReflectionShadows)
            out << ind2 << "scene->setPlanarReflectionShadows(" << formatBool(scene->getPlanarReflectionShadows()) << ");\n";
        if (scene->getPlanarReflectionFarClip() != defaults.planarReflectionFarClip)
            out << ind2 << "scene->setPlanarReflectionFarClip(" << formatFloat(scene->getPlanarReflectionFarClip()) << ");\n";
        if (scene->getPlanarReflectionClipOffset() != defaults.planarReflectionClipOffset)
            out << ind2 << "scene->setPlanarReflectionClipOffset(" << formatFloat(scene->getPlanarReflectionClipOffset()) << ");\n";
        if (scene->getMaxPlanarReflections() != defaults.maxPlanarReflections)
            out << ind2 << "scene->setMaxPlanarReflections(" << scene->getMaxPlanarReflections() << ");\n";
    }
    out << ind2 << "scene->setGlobalIllumination(" << formatFloat(scene->getGlobalIlluminationIntensity()) << ");\n";
    out << ind2 << "scene->setGlobalIllumination(" << formatVector3(scene->getGlobalIlluminationColor()) << ");\n";
    if (scene->getMaxLightsPerObject() != SceneSettings().maxLightsPerObject)
        out << ind2 << "scene->setMaxLightsPerObject(" << scene->getMaxLightsPerObject() << ");\n";
    // always written: a Scene made in code is High, a scene saved before Lit Quality is Standard
    out << ind2 << "scene->setLitQuality(" << (scene->getLitQuality() == LitQuality::HIGH ? "LitQuality::HIGH" : "LitQuality::STANDARD") << ");\n";
    if (scene->getPhysicalLightEV100() != SceneSettings().physicalLightEV100)
        out << ind2 << "scene->setPhysicalLightEV100(" << formatFloat(scene->getPhysicalLightEV100()) << ");\n";
    if (scene->isCapsuleShadowsEnabled() != SceneSettings().capsuleShadowsEnabled)
        out << ind2 << "scene->setCapsuleShadowsEnabled(" << formatBool(scene->isCapsuleShadowsEnabled()) << ");\n";
    if (scene->getCapsuleShadowIntensity() != SceneSettings().capsuleShadowIntensity)
        out << ind2 << "scene->setCapsuleShadowIntensity(" << formatFloat(scene->getCapsuleShadowIntensity()) << ");\n";
    if (scene->getCapsuleShadowSoftness() != SceneSettings().capsuleShadowSoftness)
        out << ind2 << "scene->setCapsuleShadowSoftness(" << formatFloat(scene->getCapsuleShadowSoftness()) << ");\n";
    out << ind2 << "scene->setAmbientLight2D(" << formatFloat(scene->getAmbientLight2DIntensity()) << ");\n";
    out << ind2 << "scene->setAmbientLight2D(" << formatVector3(scene->getAmbientLight2DColor()) << ");\n";
    out << ind2 << "scene->setShadow2DQuality(" << formatShadowQuality(scene->getShadow2DQuality()) << ");\n";
    out << ind2 << "scene->setGravity2D(" << formatVector2(scene->getGravity2D()) << ");\n";
    out << ind2 << "scene->setGravity3D(" << formatVector3(scene->getGravity3D()) << ");\n";
    if (scene->isPhysicsInterpolation() != SceneSettings().physicsInterpolation)
        out << ind2 << "scene->setPhysicsInterpolation(" << formatBool(scene->isPhysicsInterpolation()) << ");\n";
    // the scene's own Canvas Scaler (it uses the project default otherwise)
    if (scene->getCanvasScaler() != CanvasScalerSettings())
        out << ind2 << "scene->setCanvasScaler(" << formatCanvasScaler(scene->getCanvasScaler()) << ");\n";
    if (!scene->isCanvasScalerUseProjectDefault())
        out << ind2 << "scene->setCanvasScalerUseProjectDefault(false);\n";
    if (scene->getPhysics2DSubSteps() != SceneSettings().physics2DSubSteps)
        out << ind2 << "scene->setPhysics2DSubSteps(" << scene->getPhysics2DSubSteps() << ");\n";
    out << ind2 << "scene->setLightState(" << formatLightState(scene->getLightState()) << ");\n";
    out << ind2 << "scene->setSSAOEnabled(" << formatBool(scene->isSSAOEnabled()) << ");\n";
    out << ind2 << "scene->setSSAORadius(" << formatFloat(scene->getSSAORadius()) << ");\n";
    out << ind2 << "scene->setSSAOIntensity(" << formatFloat(scene->getSSAOIntensity()) << ");\n";
    out << ind2 << "scene->setSSAOBias(" << formatFloat(scene->getSSAOBias()) << ");\n";
    out << ind2 << "scene->setSSREnabled(" << formatBool(scene->isSSREnabled()) << ");\n";
    out << ind2 << "scene->setSSRMaxDistance(" << formatFloat(scene->getSSRMaxDistance()) << ");\n";
    out << ind2 << "scene->setSSRThickness(" << formatFloat(scene->getSSRThickness()) << ");\n";
    out << ind2 << "scene->setSSRMaxSteps(" << scene->getSSRMaxSteps() << ");\n";
    out << ind2 << "scene->setSSRIntensity(" << formatFloat(scene->getSSRIntensity()) << ");\n";
    out << ind2 << "scene->setSSRBlur(" << formatFloat(scene->getSSRBlur()) << ");\n";
    out << ind2 << "scene->setFixedResolutionSize(" << formatUInt(scene->getFixedResolutionWidth()) << ", " << formatUInt(scene->getFixedResolutionHeight()) << ");\n";
    out << ind2 << "scene->setFixedResolutionFilter(" << formatTextureFilter(scene->getFixedResolutionFilter()) << ");\n";
    out << ind2 << "scene->setFixedResolutionEnabled(" << formatBool(scene->isFixedResolutionEnabled()) << ");\n";
    out << ind2 << "scene->setEnableUIEvents(" << formatUIEventState(scene->getEnableUIEvents()) << ");\n";
    if (!scene->getDefaultMeshShader().empty())
        out << ind2 << "scene->setDefaultMeshShader(" << formatString(scene->getDefaultMeshShader()) << ");\n";
    if (!scene->getDefaultUIShader().empty())
        out << ind2 << "scene->setDefaultUIShader(" << formatString(scene->getDefaultUIShader()) << ");\n";
    if (!scene->getDefaultSkyShader().empty())
        out << ind2 << "scene->setDefaultSkyShader(" << formatString(scene->getDefaultSkyShader()) << ");\n";
    if (!scene->getDefaultPointsShader().empty())
        out << ind2 << "scene->setDefaultPointsShader(" << formatString(scene->getDefaultPointsShader()) << ");\n";
    if (!scene->getDefaultLinesShader().empty())
        out << ind2 << "scene->setDefaultLinesShader(" << formatString(scene->getDefaultLinesShader()) << ");\n";
    if (!scene->getPostProcessPasses().empty()) {
        out << ind2 << "{\n";
        out << ind3 << "std::vector<PostProcessPass> postProcess;\n";
        for (const PostProcessPass& pass : scene->getPostProcessPasses()) {
            out << ind3 << "postProcess.push_back({" << formatString(pass.shader) << ", " << formatBool(pass.enabled) << ", "
                << formatShaderUniforms(pass.uniforms) << "});\n";
            // (phase 4) only when set, so the code of a chain without them is unchanged
            out << formatShaderTextures(indentSpaces + 8, pass.textures, "postProcess.back().textures", projectPath);
            if (pass.scale != 1.0f)
                out << ind3 << "postProcess.back().scale = " << formatFloat(pass.scale) << ";\n";
            if (!pass.shaderGlobalBindings.empty())
                out << ind3 << "postProcess.back().shaderGlobalBindings = " << formatShaderGlobalBindings(pass.shaderGlobalBindings) << ";\n";
        }
        out << ind3 << "scene->setPostProcessPasses(postProcess);\n";
        out << ind2 << "}\n";
    }
    if (scene->getPostProcessing() != PostProcessingSettings()) {
        out << ind2 << "{\n";
        out << ind3 << "PostProcessingSettings postProcessing;\n";
        out << formatPostProcessing(indentSpaces + 8, scene->getPostProcessing(), "postProcessing");
        out << ind3 << "scene->setPostProcessing(postProcessing);\n";
        out << ind2 << "}\n";
    }
    out << ind << "}\n\n";

    // Files SceneManager::preloadScene() loads before the scene is created
    out << ind << "SceneResources resources_" << funcId << "(){\n";
    out << ind2 << "SceneResources resources;\n";
    for (const Texture& texture : resources.textures) {
        out << ind2 << "{\n";
        out << ind3 << "Texture texture;\n";
        out << formatTexture(indentSpaces + 8, texture, "texture", projectPath);
        if (texture.getAlphaBorder() == TextureAlphaBorder::AUTO && texture.isAlphaBorderFixed()) {
            out << ind3 << "texture.setAlphaBorderAuto(true);\n";
        }
        out << ind3 << "resources.textures.push_back(texture);\n";
        out << ind2 << "}\n";
    }
    for (const std::string& sound : resources.sounds) {
        out << ind2 << "resources.sounds.push_back(" << formatString(sound) << ");\n";
    }
    for (const std::string& model : resources.models) {
        out << ind2 << "resources.models.push_back(" << formatString(model) << ");\n";
    }
    for (const std::string& clip : resources.clips) {
        out << ind2 << "resources.clips.push_back(" << formatString(clip) << ");\n";
    }
    for (const std::string& vat : resources.vats) {
        out << ind2 << "resources.vats.push_back(" << formatString(vat) << ");\n";
    }
    for (const std::string& mask : resources.masks) {
        out << ind2 << "resources.masks.push_back(" << formatString(mask) << ");\n";
    }
    for (const std::string& controller : resources.controllers) {
        out << ind2 << "resources.controllers.push_back(" << formatString(controller) << ");\n";
    }
    out << ind2 << "return resources;\n";
    out << ind << "}\n";

    return out.str();
}

std::string editor::Factory::prefabToFunctionName(const fs::path& prefabPath) {
    fs::path noExt = prefabPath;
    noExt.replace_extension();
    std::string pathStr = noExt.generic_string();
    return "create_prefab_" + toIdentifier(pathStr);
}

// a prefix prefabToFunctionName never produces, so the names cannot collide
std::string editor::Factory::prefabToBuildFunctionName(const fs::path& prefabPath) {
    fs::path noExt = prefabPath;
    noExt.replace_extension();
    std::string pathStr = noExt.generic_string();
    return "build_prefab_" + toIdentifier(pathStr);
}

std::string editor::Factory::prefabToFileName(const fs::path& prefabPath) {
    fs::path noExt = prefabPath;
    noExt.replace_extension();
    std::string pathStr = noExt.generic_string();
    return "prefab_" + toIdentifier(pathStr);
}

std::string editor::Factory::sceneToFileName(uint32_t sceneId, const std::string& sceneName) {
    return "scene_" + toIdentifier(sceneName) + "_" + std::to_string(sceneId);
}

std::string editor::Factory::createCameraTextureLinks(int indentSpaces, EntityRegistry* registry, const std::vector<Entity>& entities, const std::unordered_map<Entity, std::string>* entityVarNames, bool namedCamerasOnly) {
    std::ostringstream links;
    const std::string ind = indentation(indentSpaces);
    for (Entity entity : entities) {
        for (ComponentType cpType : Catalog::findComponents(registry, entity)) {
            if (!Catalog::mayHoldReferences(registry, entity, cpType, false, true))
                continue;
            std::map<std::string, PropertyData> properties = Catalog::findEntityProperties(registry, entity, cpType);
            for (auto& [propName, prop] : properties) {
                if (prop.type != PropertyType::Texture || !prop.ref)
                    continue;

                Texture* texture = static_cast<Texture*>(prop.ref);
                Entity fluidEntity = FluidTextureLink::parse(texture->getId());
                if (fluidEntity != NULL_ENTITY) {
                    // a FluidSimulation's output: its simulator object made here when the
                    // fluid has none yet (as FluidSimulation::getTexture does)
                    if (!registry->isEntityCreated(fluidEntity) || !registry->findComponent<FluidSimulationComponent>(fluidEntity))
                        continue;
                    if (namedCamerasOnly && (!entityVarNames || !entityVarNames->count(fluidEntity)))
                        continue;
                    const std::string fluidExpr = "scene->getComponent<FluidSimulationComponent>(" + formatEntity(fluidEntity, entityVarNames) + ")";
                    links << ind << "if (!" << fluidExpr << ".simulator) " << fluidExpr << ".simulator = std::make_shared<FluidSimulator>();\n";
                    links << ind << "scene->getComponent<" << Catalog::getComponentName(cpType) << ">(" << formatEntity(entity, entityVarNames) << ")."
                          << propName << ".setFramebuffer(" << fluidExpr << ".simulator->getOutputFramebuffer());\n";
                    continue;
                }
                Entity videoEntity = VideoTextureLink::parse(texture->getId());
                if (videoEntity != NULL_ENTITY) {
                    // a Video Player's render texture: its object made here when it has none yet
                    if (!registry->isEntityCreated(videoEntity) || !registry->isComponentRegistered<VideoPlayerComponent>() ||
                        !registry->findComponent<VideoPlayerComponent>(videoEntity))
                        continue;
                    if (namedCamerasOnly && (!entityVarNames || !entityVarNames->count(videoEntity)))
                        continue;
                    const std::string videoExpr = "scene->getComponent<VideoPlayerComponent>(" + formatEntity(videoEntity, entityVarNames) + ")";
                    links << ind << "if (!" << videoExpr << ".output) " << videoExpr << ".output = std::make_shared<VideoTextureConverter>();\n";
                    links << ind << "scene->getComponent<" << Catalog::getComponentName(cpType) << ">(" << formatEntity(entity, entityVarNames) << ")."
                          << propName << ".setFramebuffer(" << videoExpr << ".output->getFramebuffer());\n";
                    continue;
                }
                Entity cameraEntity = CameraTextureLink::parse(texture->getId());
                if (cameraEntity == NULL_ENTITY)
                    continue;
                if (!registry->findComponent<CameraComponent>(cameraEntity))
                    continue;
                if (namedCamerasOnly && (!entityVarNames || !entityVarNames->count(cameraEntity)))
                    continue;

                links << ind << "scene->getComponent<" << Catalog::getComponentName(cpType) << ">(" << formatEntity(entity, entityVarNames) << ")."
                      << propName << ".setFramebuffer(scene->getComponent<CameraComponent>(" << formatEntity(cameraEntity, entityVarNames) << ").framebuffer);\n";
            }
        }
    }
    return links.str();
}

std::string editor::Factory::createPrefabHeader(const fs::path& prefabPath) {
    std::string funcName = prefabToFunctionName(prefabPath);
    std::string buildFuncName = prefabToBuildFunctionName(prefabPath);

    std::ostringstream out;
    out << "// This file is auto-generated by JasperEngine Editor. Do not edit manually.\n\n";
    out << "#pragma once\n\n";
    out << "#include \"JasperEngine.h\"\n";
    out << "#include <vector>\n\n";
    out << "using namespace jasperengine;\n\n";
    out << "bool " << funcName << "(Scene* scene, Entity root);\n";
    out << "bool " << buildFuncName << "(Scene* scene, Entity root, std::vector<Entity>& members);\n";
    return out.str();
}

std::vector<Entity> editor::Factory::getPrefabMemberEntities(EntityRegistry* registry, const std::vector<Entity>& registryEntities, Entity root) {
    (void)registry;
    std::vector<Entity> memberEntities;
    for (Entity entity : registryEntities) {
        if (entity == root) continue;
        memberEntities.push_back(entity);
    }
    return memberEntities;
}

std::string editor::Factory::createPrefab(const fs::path& prefabPath, const BakedPrefab& baked, const fs::path& projectPath, const fs::path& generatedPath,
        const std::function<std::string(const fs::path&)>& prefabGuid) {
    std::string funcName = prefabToFunctionName(prefabPath);
    std::string buildFuncName = prefabToBuildFunctionName(prefabPath);
    std::string fileName = prefabToFileName(prefabPath);

    std::ostringstream out;

    Scene* scene = baked.scene.get();
    const Entity root = (scene && scene->isEntityCreated(baked.root)) ? baked.root : NULL_ENTITY;

    // The GameObjects build_prefab_* makes, by slot: e<slot>; the root is the one it is given
    std::unordered_map<Entity, std::string> entityVarNames;
    std::vector<Entity> made;
    for (size_t i = 0; i < baked.slots.size(); i++) {
        const Entity entity = baked.slots[i];
        if (entity != NULL_ENTITY && scene && scene->isEntityCreated(entity) && !entityVarNames.count(entity)) {
            entityVarNames[entity] = "e" + std::to_string(i);
            made.push_back(entity);
        }
    }
    if (root != NULL_ENTITY) {
        entityVarNames[root] = "root";
    }

    // Check for default sky usage
    bool usesDefaultSky = false;
    std::vector<Entity> skyEntities = made;
    if (root != NULL_ENTITY) skyEntities.push_back(root);
    for (Entity entity : skyEntities) {
        SkyboxComponent* sky = scene->findComponent<SkyboxComponent>(entity);
        if (sky && sky->texture.getId() == DEFAULT_SKY_ID) {
            usesDefaultSky = true;
            break;
        }
    }

    // File header
    out << "// This file is auto-generated by JasperEngine Editor. Do not edit manually.\n\n";
    out << "#include \"" << fileName << ".h\"\n";

    writeSkyIncludes(out, usesDefaultSky, generatedPath);

    const std::string ind1 = indentation(4);
    const std::string ind2 = indentation(8);

    // Function definition
    out << "bool " << buildFuncName << "(Scene* scene, Entity root, std::vector<Entity>& members) {\n";

    // The root is the prefab's root GameObject. PrefabManager hands the factory a bare one,
    // which takes everything from it; a scene instance root carries its own components
    // already, and only gets the ones it lacks.
    const std::string stem = prefabPath.stem().string();
    std::string rootName = root != NULL_ENTITY ? scene->getEntityName(root) : stem;
    if (rootName.empty()) rootName = stem.empty() ? "Prefab" : stem;

    out << ind1 << "const bool bareRoot = !scene->findComponent<PrefabComponent>(root);\n";
    out << ind1 << "if (scene->getEntityName(root).empty())\n";
    out << ind1 << "    scene->setEntityName(root, " << formatString(rootName) << ");\n\n";

    out << ind1 << "if (bareRoot) {\n";
    out << ind1 << "    PrefabComponent prefab;\n";
    out << ind1 << "    prefab.name = " << formatString(stem) << ";\n";
    out << ind1 << "    prefab.path = " << formatString(prefabPath.generic_string()) << ";\n";
    if (prefabGuid) {
        const std::string guid = prefabGuid(prefabPath);
        if (!guid.empty()) {
            out << ind1 << "    prefab.guid = " << formatString(guid) << ";\n";
        }
    }
    out << ind1 << "    scene->addComponent<PrefabComponent>(root, prefab);\n";
    if (root != NULL_ENTITY) {
        out << ind1 << "    scene->setEntityActive(root, " << formatBool(scene->isEntityActive(root)) << ");\n";
        out << ind1 << "    scene->setEntityTag(root, " << formatString(scene->getEntityTag(root)) << ");\n";
        out << ind1 << "    scene->setEntityLayer(root, " << formatInt(scene->getEntityLayer(root)) << ");\n";
    }
    out << ind1 << "}\n\n";

    // Phase 1: every GameObject, nested prefabs' included, in member order. A GameObject a
    // layer removed keeps its slot empty.
    out << ind1 << "// Create member entities\n";
    for (size_t i = 0; i < baked.slots.size(); i++) {
        const Entity entity = baked.slots[i];
        auto var = entityVarNames.find(entity);
        if (entity == NULL_ENTITY || var == entityVarNames.end() || var->second != "e" + std::to_string(i)) {
            out << ind1 << "members.push_back(NULL_ENTITY);\n";
            continue;
        }
        out << ind1 << "Entity " << var->second << " = scene->createEntity();\n";
        out << ind1 << "scene->setEntityName(" << var->second << ", " << formatString(scene->getEntityName(entity)) << ");\n";
        out << formatEntityState(ind1, var->second, scene, entity);
        out << ind1 << "members.push_back(" << var->second << ");\n";
    }
    out << "\n";

    // Phase 2: the roots of the nested instances keep their prefab link
    bool hasNestedPrefabs = false;
    for (Entity entity : made) {
        const PrefabComponent* link = scene->findComponent<PrefabComponent>(entity);
        if (!link) {
            continue;
        }
        if (!hasNestedPrefabs) {
            out << ind1 << "// Nested prefab roots\n";
            hasNestedPrefabs = true;
        }
        out << ind1 << "{\n";
        out << ind2 << "PrefabComponent prefab;\n";
        out << ind2 << "prefab.name = " << formatString(link->name) << ";\n";
        out << ind2 << "prefab.path = " << formatString(fs::path(link->path).generic_string()) << ";\n";
        if (!link->guid.empty()) {
            out << ind2 << "prefab.guid = " << formatString(link->guid) << ";\n";
        }
        out << ind2 << "scene->addComponent<PrefabComponent>(" << entityVarNames[entity] << ", prefab);\n";
        out << ind1 << "}\n";
    }
    if (hasNestedPrefabs) {
        out << "\n";
    }

    auto writeComponents = [&](Entity entity, const std::string& varName, bool onlyMissing) {
        std::vector<ComponentType> components = Catalog::findComponents(scene, entity);
        bool first = true;
        for (ComponentType compType : components) {
            // PrefabComponent: the prefab links above
            if (compType == ComponentType::PrefabComponent) continue;

            std::string componentCode;
            if (compType == ComponentType::Transform) {
                componentCode = createTransform(onlyMissing ? 12 : 8, scene, entity, "scene", varName, true, false, &entityVarNames);
            } else {
                componentCode = createComponent(onlyMissing ? 12 : 8, scene, entity, compType, projectPath, "scene", varName, false, &entityVarNames);
            }

            if (!componentCode.empty()) {
                if (!first) out << "\n";
                if (onlyMissing) {
                    out << guardComponentCode(compType, ind2 + "if (!scene->findComponent<" + Catalog::getComponentName(compType) + ">(" + varName + ")) {\n" +
                        componentCode + ind2 + "}\n");
                } else {
                    out << componentCode;
                }
                first = false;
            }
        }
    };

    // Phase 3: the root's components it does not have yet
    if (root != NULL_ENTITY) {
        out << ind1 << "// Components of the root GameObject\n";
        out << ind1 << "{\n";
        writeComponents(root, "root", true);
        out << ind1 << "}\n\n";
    } else {
        // no root GameObject known (never expected): a Transform for the members to hang from
        out << ind1 << "if (!scene->findComponent<Transform>(root))\n";
        out << ind1 << "    scene->addComponent<Transform>(root, {});\n\n";
    }

    // Phase 4: every other GameObject's components, as the instance has them
    for (Entity entity : made) {
        out << ind1 << "// Components for " << scene->getEntityName(entity) << "\n";
        out << ind1 << "{\n";
        writeComponents(entity, entityVarNames[entity], false);
        out << ind1 << "}\n\n";
    }

    // Phase 5: the hierarchy, in Transform order (each child appended to its parent)
    out << ind1 << "// Parent-child hierarchy\n";
    {
        std::vector<std::pair<size_t, Entity>> ordered;
        auto transforms = scene->getComponentArray<Transform>();
        for (Entity entity : made) {
            if (scene->findComponent<Transform>(entity)) {
                ordered.emplace_back(transforms->getIndex(entity), entity);
            }
        }
        std::sort(ordered.begin(), ordered.end());
        for (const auto& [index, entity] : ordered) {
            const Entity parent = scene->getComponent<Transform>(entity).parent;
            auto parentVar = entityVarNames.find(parent);
            const std::string parentName = parentVar != entityVarNames.end() ? parentVar->second : std::string("root");
            out << ind1 << "scene->addEntityChild(" << parentName << ", " << entityVarNames[entity] << ", false);\n";
        }
    }

    // Phase 6: Camera render-to-texture links, once every camera exists (into and out of
    // nested prefabs alike: every GameObject has a variable)
    const std::string links = createCameraTextureLinks(4, scene, skyEntities, &entityVarNames, true);
    if (!links.empty()) {
        out << "\n";
        out << ind1 << "// Render-to-texture cameras\n";
        out << links;
    }

    out << ind1 << "return true;\n";
    out << "}\n\n";

    out << "bool " << funcName << "(Scene* scene, Entity root) {\n";
    out << ind1 << "std::vector<Entity> members;\n";
    out << ind1 << "return " << buildFuncName << "(scene, root, members);\n";
    out << "}\n";

    return out.str();
}

std::string editor::Factory::createParticleSystemComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<ParticleSystemComponent>(entity)) return "";
    ParticleSystemComponent& p = scene->getComponent<ParticleSystemComponent>(entity);
    const ParticleSystemComponent defaults;
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "ParticleSystemComponent particles;\n";
    if (!p.effect.empty()){
        // an asset effect, packaged as a data file and read by path
        code << ind << "particles.effect = " << formatString(p.effect.path) << ";\n";
    }else{
        // the inline effect as its JSON, in pieces short enough for every compiler's literals
        const std::string json = particles::ParticleEffectCodec::write(p.inlineEffect);
        code << ind << "{\n";
        code << ind << "    std::string json;\n";
        for (size_t at = 0; at < json.size(); at += 2000)
            code << ind << "    json += " << formatString(json.substr(at, 2000)) << ";\n";
        code << ind << "    particles::readEffectText(json, particles.inlineEffect);\n";
        code << ind << "}\n";
    }
    if (p.playOnAwake != defaults.playOnAwake) code << ind << "particles.playOnAwake = " << formatBool(p.playOnAwake) << ";\n";
    if (p.seed != defaults.seed) code << ind << "particles.seed = " << formatUInt(p.seed) << ";\n";
    if (p.playbackSpeed != defaults.playbackSpeed) code << ind << "particles.playbackSpeed = " << formatFloat(p.playbackSpeed) << ";\n";
    if (p.useUnscaledTime != defaults.useUnscaledTime) code << ind << "particles.useUnscaledTime = " << formatBool(p.useUnscaledTime) << ";\n";
    for (const ParticleExposedValue& v : p.exposedValues)
        code << ind << "particles.exposedValues.push_back({" << formatString(v.name) << ", " << formatVector4(v.value) << "});\n";
    for (Entity plane : p.collisionPlanes)
        code << ind << "particles.collisionPlanes.push_back(" << formatEntity(plane, entityVarNames) << ");\n";
    if (!p.enabled) code << ind << "particles.enabled = false;\n";

    addComponentCode(code, ind, sceneName, entityName, entity, "ParticleSystemComponent", "particles", assignExisting);
    return code.str();
}

std::string editor::Factory::createWaterVolumeComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting) {
    if (!scene->findComponent<WaterVolumeComponent>(entity)) return "";
    const WaterVolumeComponent& volume = scene->getComponent<WaterVolumeComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "WaterVolumeComponent waterVolume;\n";
    code << ind << "waterVolume.size = " << formatVector3(volume.size) << ";\n";
    code << ind << "waterVolume.surfaceOffset = " << formatFloat(volume.surfaceOffset) << ";\n";
    code << ind << "waterVolume.infinite = " << formatBool(volume.infinite) << ";\n";
    code << ind << "waterVolume.density = " << formatFloat(volume.density) << ";\n";
    code << ind << "waterVolume.flow = " << formatVector3(volume.flow) << ";\n";
    code << ind << "waterVolume.linearDrag = " << formatFloat(volume.linearDrag) << ";\n";
    code << ind << "waterVolume.angularDrag = " << formatFloat(volume.angularDrag) << ";\n";
    code << ind << "waterVolume.waves = " << (volume.waves == WaterVolumeWaves::NONE ? "WaterVolumeWaves::NONE" : "WaterVolumeWaves::FROM_MATERIAL") << ";\n";
    if (!volume.enabled) code << ind << "waterVolume.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "WaterVolumeComponent", "waterVolume", assignExisting);
    return code.str();
}

std::string editor::Factory::createBuoyancyComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting) {
    if (!scene->findComponent<BuoyancyComponent>(entity)) return "";
    const BuoyancyComponent& buoyancy = scene->getComponent<BuoyancyComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    const char* columns = buoyancy.columns == BuoyancyColumns::AUTO_3x3 ? "BuoyancyColumns::AUTO_3x3"
                        : buoyancy.columns == BuoyancyColumns::CUSTOM ? "BuoyancyColumns::CUSTOM" : "BuoyancyColumns::AUTO_2x2";
    code << ind << "BuoyancyComponent buoyancy;\n";
    code << ind << "buoyancy.columns = " << columns << ";\n";
    for (const Vector3& point : buoyancy.customPoints)
        code << ind << "buoyancy.customPoints.push_back(" << formatVector3(point) << ");\n";
    code << ind << "buoyancy.volumeScale = " << formatFloat(buoyancy.volumeScale) << ";\n";
    code << ind << "buoyancy.dragScale = " << formatFloat(buoyancy.dragScale) << ";\n";
    if (!buoyancy.enabled) code << ind << "buoyancy.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "BuoyancyComponent", "buoyancy", assignExisting);
    return code.str();
}

std::string editor::Factory::createCharacterControllerComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting) {
    if (!scene->findComponent<CharacterControllerComponent>(entity)) return "";
    const CharacterControllerComponent& controller = scene->getComponent<CharacterControllerComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "CharacterControllerComponent controller;\n";
    code << ind << "controller.center = " << formatVector3(controller.center) << ";\n";
    code << ind << "controller.height = " << formatFloat(controller.height) << ";\n";
    code << ind << "controller.radius = " << formatFloat(controller.radius) << ";\n";
    code << ind << "controller.slopeLimit = " << formatFloat(controller.slopeLimit) << ";\n";
    code << ind << "controller.stepOffset = " << formatFloat(controller.stepOffset) << ";\n";
    code << ind << "controller.skinWidth = " << formatFloat(controller.skinWidth) << ";\n";
    code << ind << "controller.minMoveDistance = " << formatFloat(controller.minMoveDistance) << ";\n";
    code << ind << "controller.mass = " << formatFloat(controller.mass) << ";\n";
    code << ind << "controller.pushStrength = " << formatFloat(controller.pushStrength) << ";\n";
    code << ind << "controller.pushBodies = " << formatBool(controller.pushBodies) << ";\n";
    code << ind << "controller.movingPlatforms = " << formatBool(controller.movingPlatforms) << ";\n";
    code << ind << "controller.inheritPlatformYaw = " << formatBool(controller.inheritPlatformYaw) << ";\n";
    code << ind << "controller.detectCollisions = " << formatBool(controller.detectCollisions) << ";\n";
    code << ind << "controller.characterCollisions = " << formatBool(controller.characterCollisions) << ";\n";
    code << ind << "controller.enhancedEdgeRemoval = " << formatBool(controller.enhancedEdgeRemoval) << ";\n";
    code << ind << "controller.categoryBits = " << controller.categoryBits << ";\n";
    code << ind << "controller.maskBits = " << controller.maskBits << ";\n";
    if (!controller.enabled) code << ind << "controller.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "CharacterControllerComponent", "controller", assignExisting);
    return code.str();
}

std::string editor::Factory::createCharacterController2DComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting) {
    if (!scene->findComponent<CharacterController2DComponent>(entity)) return "";
    const CharacterController2DComponent& controller = scene->getComponent<CharacterController2DComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "CharacterController2DComponent controller2d;\n";
    code << ind << "controller2d.center = " << formatVector2(controller.center) << ";\n";
    code << ind << "controller2d.height = " << formatFloat(controller.height) << ";\n";
    code << ind << "controller2d.radius = " << formatFloat(controller.radius) << ";\n";
    code << ind << "controller2d.slopeLimit = " << formatFloat(controller.slopeLimit) << ";\n";
    code << ind << "controller2d.stepOffset = " << formatFloat(controller.stepOffset) << ";\n";
    code << ind << "controller2d.skinWidth = " << formatFloat(controller.skinWidth) << ";\n";
    code << ind << "controller2d.minMoveDistance = " << formatFloat(controller.minMoveDistance) << ";\n";
    code << ind << "controller2d.pushStrength = " << formatFloat(controller.pushStrength) << ";\n";
    code << ind << "controller2d.pushBodies = " << formatBool(controller.pushBodies) << ";\n";
    code << ind << "controller2d.movingPlatforms = " << formatBool(controller.movingPlatforms) << ";\n";
    code << ind << "controller2d.categoryBits = " << controller.categoryBits << ";\n";
    code << ind << "controller2d.maskBits = " << controller.maskBits << ";\n";
    if (!controller.enabled) code << ind << "controller2d.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "CharacterController2DComponent", "controller2d", assignExisting);
    return code.str();
}

std::string editor::Factory::createForceFieldComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting) {
    if (!scene->findComponent<ForceFieldComponent>(entity)) return "";
    ForceFieldComponent& f = scene->getComponent<ForceFieldComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "ForceFieldComponent field;\n";
    code << ind << "field.shape = (ForceFieldShapeType)" << formatInt((int)f.shape) << ";\n";
    code << ind << "field.size = " << formatVector3(f.size) << ";\n";
    code << ind << "field.kind = (ForceFieldKind)" << formatInt((int)f.kind) << ";\n";
    code << ind << "field.strength = " << formatFloat(f.strength) << ";\n";
    code << ind << "field.direction = " << formatVector3(f.direction) << ";\n";
    code << ind << "field.turbulence = " << formatFloat(f.turbulence) << ";\n";
    code << ind << "field.pulseFrequency = " << formatFloat(f.pulseFrequency) << ";\n";
    code << ind << "field.frequency = " << formatFloat(f.frequency) << ";\n";
    code << ind << "field.falloff = " << curvecodec::curveCppLiteral(f.falloff) << ";\n";
    code << ind << "field.layers = " << formatUInt(f.layers) << ";\n";
    if (!f.enabled) code << ind << "field.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "ForceFieldComponent", "field", assignExisting);
    return code.str();
}

std::string editor::Factory::createInstancedMeshComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<InstancedMeshComponent>(entity)) return "";
    InstancedMeshComponent& p = scene->getComponent<InstancedMeshComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "InstancedMeshComponent instmesh;\n";
    code << ind << "instmesh.maxInstances = " << formatUInt(p.maxInstances) << ";\n";
    code << ind << "instmesh.cullInstances = " << formatBool(p.cullInstances) << ";\n";
    code << ind << "instmesh.distanceFade = " << formatBool(p.distanceFade) << ";\n";
    code << ind << "instmesh.cullDistance = " << formatFloat(p.cullDistance) << ";\n";
    code << ind << "instmesh.fadeStart = " << formatFloat(p.fadeStart) << ";\n";
    code << ind << "instmesh.fadeEnd = " << formatFloat(p.fadeEnd) << ";\n";
    code << ind << "instmesh.instancedBillboard = " << formatBool(p.instancedBillboard) << ";\n";
    code << ind << "instmesh.instancedCylindricalBillboard = " << formatBool(p.instancedCylindricalBillboard) << ";\n";

    for (size_t i = 0; i < p.instances.size(); i++) {
        const InstanceData& inst = p.instances[i];
        code << ind << "{\n";
        code << ind << "    InstanceData inst;\n";
        code << ind << "    inst.position = " << formatVector3(inst.position) << ";\n";
        code << ind << "    inst.rotation = " << formatQuaternion(inst.rotation) << ";\n";
        code << ind << "    inst.scale = " << formatVector3(inst.scale) << ";\n";
        code << ind << "    inst.color = " << formatVector4(inst.color) << ";\n";
        code << ind << "    inst.textureRect = " << formatRect(inst.textureRect) << ";\n";
        code << ind << "    inst.visible = " << formatBool(inst.visible) << ";\n";
        code << ind << "    instmesh.instances.push_back(inst);\n";
        code << ind << "}\n";
    }

    if (!p.enabled) code << ind << "instmesh.enabled = false;\n";

    addComponentCode(code, ind, sceneName, entityName, entity, "InstancedMeshComponent", "instmesh", assignExisting);
    return code.str();
}

std::string editor::Factory::createPointsComponent(int indentSpaces, EntityRegistry* scene, Entity entity, const fs::path& projectPath, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<PointsComponent>(entity)) return "";
    PointsComponent& p = scene->getComponent<PointsComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);

    code << ind << "PointsComponent pointscomp;\n";
    code << ind << "pointscomp.maxPoints = " << formatUInt(p.maxPoints) << ";\n";
    code << ind << "pointscomp.transparent = " << formatBool(p.transparent) << ";\n";
    code << ind << "pointscomp.autoTransparency = " << formatBool(p.autoTransparency) << ";\n";
    if (!p.customShader.empty())
        code << ind << "pointscomp.customShader = " << formatString(p.customShader) << ";\n";
    if (!p.shaderUniforms.empty())
        code << ind << "pointscomp.shaderUniforms = " << formatShaderUniforms(p.shaderUniforms) << ";\n";
    if (!p.shaderGlobalBindings.empty())
        code << ind << "pointscomp.shaderGlobalBindings = " << formatShaderGlobalBindings(p.shaderGlobalBindings) << ";\n";
    code << formatComponentShaderLibrary(indentSpaces, p.shaderTextures, p.shaderFeatures, &p.blendMode, &p.depthWrite, "pointscomp", projectPath);
    code << formatStencilAndColorWrite(indentSpaces, p.stencil, p.colorWrite, "pointscomp");
    code << formatTexture(indentSpaces, p.texture, "pointscomp.texture", projectPath);

    if (p.numFramesRect > 0) {
        for (unsigned int i = 0; i < p.numFramesRect; i++) {
            const SpriteFrameData& frame = p.framesRect[i];
            code << ind << "pointscomp.framesRect[" << i << "].name = " << formatString(frame.name) << ";\n";
            code << ind << "pointscomp.framesRect[" << i << "].rect = " << formatRect(frame.rect) << ";\n";
        }
        code << ind << "pointscomp.numFramesRect = " << p.numFramesRect << ";\n";
        code << ind << "pointscomp.hasTextureRect = true;\n";
    }

    for (size_t i = 0; i < p.points.size(); i++) {
        const PointData& pt = p.points[i];
        code << ind << "{\n";
        code << ind << "    PointData pt;\n";
        code << ind << "    pt.position = " << formatVector3(pt.position) << ";\n";
        code << ind << "    pt.color = " << formatVector4(pt.color) << ";\n";
        code << ind << "    pt.size = " << formatFloat(pt.size) << ";\n";
        code << ind << "    pt.rotation = " << formatFloat(pt.rotation) << ";\n";
        code << ind << "    pt.textureRect = " << formatRect(pt.textureRect) << ";\n";
        code << ind << "    pt.visible = " << formatBool(pt.visible) << ";\n";
        code << ind << "    pointscomp.points.push_back(pt);\n";
        Rect defaultRect(0, 0, 1, 1);
        Rect ptRect = pt.textureRect;
        if (ptRect != defaultRect) {
            code << ind << "    pointscomp.hasTextureRect = true;\n";
        }
        code << ind << "}\n";
    }

    if (!p.enabled) code << ind << "pointscomp.enabled = false;\n";

    addComponentCode(code, ind, sceneName, entityName, entity, "PointsComponent", "pointscomp", assignExisting);
    return code.str();
}

std::string editor::Factory::createLineRendererComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<LineRendererComponent>(entity)) return "";
    LineRendererComponent& l = scene->getComponent<LineRendererComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);

    unsigned int maxLines = std::max(l.maxLines, static_cast<unsigned int>(l.lines.size()));
    code << ind << "LineRendererComponent linescomp;\n";
    code << ind << "linescomp.maxLines = " << formatUInt(maxLines) << ";\n";
    if (!l.customShader.empty())
        code << ind << "linescomp.customShader = " << formatString(l.customShader) << ";\n";
    if (!l.shaderUniforms.empty())
        code << ind << "linescomp.shaderUniforms = " << formatShaderUniforms(l.shaderUniforms) << ";\n";
    if (!l.shaderGlobalBindings.empty())
        code << ind << "linescomp.shaderGlobalBindings = " << formatShaderGlobalBindings(l.shaderGlobalBindings) << ";\n";
    code << formatComponentShaderLibrary(indentSpaces, l.shaderTextures, l.shaderFeatures, &l.blendMode, &l.depthWrite, "linescomp", fs::path());
    code << formatStencilAndColorWrite(indentSpaces, l.stencil, l.colorWrite, "linescomp");

    for (size_t i = 0; i < l.lines.size(); i++) {
        const LineData& line = l.lines[i];
        code << ind << "{\n";
        code << ind << "    LineData line;\n";
        code << ind << "    line.pointA = " << formatVector3(line.pointA) << ";\n";
        code << ind << "    line.colorA = " << formatVector4(line.colorA) << ";\n";
        code << ind << "    line.pointB = " << formatVector3(line.pointB) << ";\n";
        code << ind << "    line.colorB = " << formatVector4(line.colorB) << ";\n";
        code << ind << "    linescomp.lines.push_back(line);\n";
        code << ind << "}\n";
    }

    // the polyline and its ribbon (lines and trails spec 6.7), away from the defaults
    static const LineRendererComponent defaults;
    if (l.mode != defaults.mode)
        code << ind << "linescomp.mode = LineRendererMode::Polyline;\n";
    if (!l.positions.empty()) {
        code << ind << "linescomp.positions = {";
        for (size_t i = 0; i < l.positions.size(); i++) {
            code << (i > 0 ? ", " : "") << formatVector3(l.positions[i]);
        }
        code << "};\n";
    }
    if (l.loop) code << ind << "linescomp.loop = true;\n";
    if (l.useWorldSpace) code << ind << "linescomp.useWorldSpace = true;\n";
    formatRibbonFields(code, ind, "linescomp", l, defaults);

    if (!l.enabled) code << ind << "linescomp.enabled = false;\n";

    addComponentCode(code, ind, sceneName, entityName, entity, "LineRendererComponent", "linescomp", assignExisting);
    return code.str();
}

std::string editor::Factory::createTrailRendererComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName, std::string entityName, bool assignExisting, const std::unordered_map<Entity, std::string>* entityVarNames) {
    (void)entityVarNames;
    if (!scene->findComponent<TrailRendererComponent>(entity)) return "";
    TrailRendererComponent& t = scene->getComponent<TrailRendererComponent>(entity);
    static const TrailRendererComponent defaults;
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);

    code << ind << "TrailRendererComponent trailcomp;\n";
    if (t.time != defaults.time) code << ind << "trailcomp.time = " << formatFloat(t.time) << ";\n";
    if (t.minVertexDistance != defaults.minVertexDistance) code << ind << "trailcomp.minVertexDistance = " << formatFloat(t.minVertexDistance) << ";\n";
    if (t.emitting != defaults.emitting) code << ind << "trailcomp.emitting = " << (t.emitting ? "true" : "false") << ";\n";
    if (t.autodestruct != defaults.autodestruct) code << ind << "trailcomp.autodestruct = " << (t.autodestruct ? "true" : "false") << ";\n";
    formatRibbonFields(code, ind, "trailcomp", t, defaults);
    if (!t.customShader.empty())
        code << ind << "trailcomp.customShader = " << formatString(t.customShader.path) << ";\n";
    if (!t.shaderUniforms.empty())
        code << ind << "trailcomp.shaderUniforms = " << formatShaderUniforms(t.shaderUniforms) << ";\n";
    code << formatComponentShaderLibrary(indentSpaces, t.shaderTextures, t.shaderFeatures, &t.blendMode, &t.depthWrite, "trailcomp", fs::path());
    code << formatStencilAndColorWrite(indentSpaces, t.stencil, t.colorWrite, "trailcomp");
    if (!t.shaderGlobalBindings.empty())
        code << ind << "trailcomp.shaderGlobalBindings = " << formatShaderGlobalBindings(t.shaderGlobalBindings) << ";\n";
    if (!t.enabled) code << ind << "trailcomp.enabled = false;\n";

    addComponentCode(code, ind, sceneName, entityName, entity, "TrailRendererComponent", "trailcomp", assignExisting);
    return code.str();
}
