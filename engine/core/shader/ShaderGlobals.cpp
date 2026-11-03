// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "ShaderGlobals.h"

#include "Log.h"
#include "pool/TextureDataPool.h"
#include "render/TextureRender.h"
#include "texture/Framebuffer.h"
#include "util/Color.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <cstring>
#include <unordered_map>

using namespace jasperengine;

namespace {

    struct Entry {
        bool exists = false;
        bool declared = false;
        ShaderGlobalType type = ShaderGlobalType::Vector4;
        unsigned int arraySize = 0;
        std::vector<Vector4> values;
        Texture texture;
        // the GPU texture `texture` resolved to (file textures: once per value, cached), or a
        // load that failed (the fallback binds until the value changes; warned once)
        TextureRender* resolved = nullptr;
        // the texture pool's replace epoch `resolved` was read at: a progressive load's full
        // resolution (oe-port's cooked textures, S14), a budget reload or a hot reload swaps the
        // pool entry, whose GPU texture is another one; resolved again then
        uint32_t resolvedEpoch = 0;
        bool textureFailed = false;
        // logged once per global: a refused setter, a truncated array
        bool warnedKind = false;
        bool warnedTruncated = false;
    };

    // a texture value, its resolution started again
    void putTexture(Entry& entry, const Texture& texture) {
        entry.texture = texture;
        entry.resolved = nullptr;
        entry.textureFailed = false;
    }

    // the same image source without the GPU texture and pixels it holds (a context loss)
    Texture sourceOf(const Texture& texture) {
        Texture fresh;
        if (texture.isFramebuffer()) {
            fresh.setFramebuffer(texture.getFramebuffer());
        } else if (!texture.getPath(0).empty()) {
            if (texture.isCubeMap() && texture.getPath(1).empty())
                fresh.setCubeMap(texture.getPath(0));
            else if (texture.isCubeMap())
                fresh.setCubePaths(texture.getPath(4), texture.getPath(5), texture.getPath(1), texture.getPath(0),
                                   texture.getPath(2), texture.getPath(3));
            else
                fresh.setPath(texture.getPath(0));
        } else if (texture.hasData()) {
            fresh.setData(texture.getId(), texture.getData(0));
        }
        return fresh;
    }

    struct State {
        std::vector<std::string> names;              // by id - 1
        std::unordered_map<std::string, int> ids;
        std::vector<Entry> entries;                  // by id - 1
        std::vector<ShaderGlobalDecl> decls;
        std::vector<int> created;                    // ids scripts made, in order
        uint64_t version = 1;
        uint64_t namesVersion = 1;
        std::unordered_map<std::string, std::weak_ptr<GlobalsBlockLayout>> layouts;
    };

    // never destroyed: Framebuffers deleted during static destruction still reach it
    State& state() {
        static State* s = new State();
        return *s;
    }

    // guards State::decls for getDeclarationsSnapshot (the rest is main thread only)
    std::mutex& declsMutex() {
        static std::mutex mutex;
        return mutex;
    }

    Entry* entryOf(int id) {
        State& s = state();
        if (id <= 0 || id > (int)s.entries.size())
            return nullptr;
        return &s.entries[id - 1];
    }

    unsigned int slotsOf(ShaderGlobalType type, unsigned int arraySize) {
        switch (type) {
            case ShaderGlobalType::Matrix4: return 4;
            case ShaderGlobalType::FloatArray:
            case ShaderGlobalType::Vector4Array: return std::max(1u, arraySize);
            case ShaderGlobalType::Texture2D:
            case ShaderGlobalType::TextureCube: return 0;
            default: return 1;
        }
    }

    // the components a scalar or vector type keeps (the others zero)
    Vector4 keepComponents(ShaderGlobalType type, const Vector4& value) {
        switch (type) {
            case ShaderGlobalType::Float:
            case ShaderGlobalType::FloatArray: return Vector4(value.x, 0.0f, 0.0f, 0.0f);
            case ShaderGlobalType::Vector2: return Vector4(value.x, value.y, 0.0f, 0.0f);
            case ShaderGlobalType::Vector3: return Vector4(value.x, value.y, value.z, 0.0f);
            default: return value;
        }
    }

    std::vector<Vector4> defaultValuesOf(const ShaderGlobalDecl& decl) {
        const unsigned int slots = slotsOf(decl.type, decl.arraySize);
        std::vector<Vector4> values = decl.defaultValue;
        if (decl.type == ShaderGlobalType::Matrix4 && values.empty()) {
            values = {Vector4(1, 0, 0, 0), Vector4(0, 1, 0, 0), Vector4(0, 0, 1, 0), Vector4(0, 0, 0, 1)};
        }
        values.resize(slots, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        for (Vector4& value : values) value = keepComponents(decl.type, value);
        return values;
    }

    Texture defaultTextureOf(ShaderGlobalType type, const std::string& path) {
        Texture texture;
        if (path.empty())
            return texture;
        if (type == ShaderGlobalType::TextureCube)
            texture.setCubeMap(path);
        else
            texture.setPath(path);
        return texture;
    }

    const ShaderGlobalDecl* findDecl(const std::string& name) {
        for (const ShaderGlobalDecl& decl : state().decls) {
            if (decl.name == name)
                return &decl;
        }
        return nullptr;
    }

    void changed() {
        state().version++;
    }

    bool assign(Entry& entry, const std::vector<Vector4>& values) {
        if (entry.values == values)
            return true;
        entry.values = values;
        changed();
        return true;
    }

    // the global of id, made with this kind when it does not exist yet (a script's first set)
    Entry* ensure(int id, ShaderGlobalType type, unsigned int arraySize) {
        Entry* entry = entryOf(id);
        if (!entry) {
            Log::error("Shader globals: unknown id %d (use Shader.propertyToID)", id);
            return nullptr;
        }
        if (!entry->exists) {
            entry->exists = true;
            entry->declared = false;
            entry->type = type;
            entry->arraySize = ShaderGlobals::isArray(type) ? std::clamp(arraySize, 1u, ShaderGlobals::MAX_ARRAY_SIZE) : 0;
            entry->values.assign(slotsOf(type, entry->arraySize), Vector4(0.0f, 0.0f, 0.0f, 0.0f));
            if (type == ShaderGlobalType::Matrix4)
                entry->values = {Vector4(1, 0, 0, 0), Vector4(0, 1, 0, 0), Vector4(0, 0, 1, 0), Vector4(0, 0, 0, 1)};
            entry->texture = Texture();
            entry->warnedKind = false;
            entry->warnedTruncated = false;
            state().created.push_back(id);
            state().namesVersion++;
            changed();
        }
        return entry;
    }

    bool refuse(Entry& entry, int id, const char* setter) {
        if (!entry.warnedKind) {
            entry.warnedKind = true;
            Log::error("Shader global '%s' is %s: %s cannot set it", ShaderGlobals::nameOf(id).c_str(),
                       ShaderGlobals::getTypeName(entry.type), setter);
        }
        return false;
    }

    // a scalar or vector value into a global of any non-matrix, non-texture kind
    bool setLeading(int id, ShaderGlobalType madeAs, const Vector4& value, const char* setter) {
        Entry* entry = ensure(id, madeAs, 0);
        if (!entry)
            return false;
        if (entry->type == ShaderGlobalType::Matrix4 || ShaderGlobals::isTexture(entry->type))
            return refuse(*entry, id, setter);
        std::vector<Vector4> values = entry->values;
        if (values.empty())
            values.resize(1);
        values[0] = keepComponents(entry->type, value);
        return assign(*entry, values);
    }

    bool setArray(int id, ShaderGlobalType madeAs, const Vector4* values, size_t count, const char* setter) {
        if (count == 0) {
            Log::error("Shader global '%s': %s needs at least one value", ShaderGlobals::nameOf(id).c_str(), setter);
            return false;
        }
        Entry* entry = ensure(id, madeAs, (unsigned int)std::min<size_t>(count, ShaderGlobals::MAX_ARRAY_SIZE));
        if (!entry)
            return false;
        if (!ShaderGlobals::isArray(entry->type))
            return refuse(*entry, id, setter);
        const size_t size = entry->values.size();
        if (count > size && !entry->warnedTruncated) {
            entry->warnedTruncated = true;
            Log::warn("Shader global '%s' holds %u elements: the %u given are truncated", ShaderGlobals::nameOf(id).c_str(),
                      (unsigned int)size, (unsigned int)count);
        }
        std::vector<Vector4> stored = entry->values;
        for (size_t i = 0; i < std::min(count, size); i++)
            stored[i] = keepComponents(entry->type, values[i]);
        return assign(*entry, stored);
    }

    void writeFloats(std::vector<uint8_t>& bytes, unsigned int offset, const Vector4& value, int components) {
        if ((size_t)offset + components * 4 > bytes.size())
            return;
        const float c[4] = {value.x, value.y, value.z, value.w};
        memcpy(bytes.data() + offset, c, components * 4);
    }

    std::string layoutSignature(const std::string& blockName, unsigned int size, const std::vector<GlobalsBlockLayout::Member>& members) {
        std::string signature = blockName + ":" + std::to_string(size);
        for (const GlobalsBlockLayout::Member& member : members) {
            signature += ";" + member.name + "," + std::to_string((int)member.type) + "," + std::to_string(member.offset) + "," +
                         std::to_string(member.arrayCount);
        }
        return signature;
    }

    const char* glslScalarType(ShaderGlobalType type) {
        switch (type) {
            case ShaderGlobalType::Float:
            case ShaderGlobalType::FloatArray: return "float";
            case ShaderGlobalType::Vector2: return "vec2";
            case ShaderGlobalType::Vector3: return "vec3";
            case ShaderGlobalType::Matrix4: return "mat4";
            default: return "vec4";
        }
    }

}

// ---------------------------------------------------------------------------- names and ids

int ShaderGlobals::propertyToID(const std::string& name) {
    if (name.empty())
        return 0;
    State& s = state();
    auto it = s.ids.find(name);
    if (it != s.ids.end())
        return it->second;
    s.names.push_back(name);
    s.entries.emplace_back();
    const int id = (int)s.names.size();
    s.ids[name] = id;
    return id;
}

int ShaderGlobals::findID(const std::string& name) {
    State& s = state();
    auto it = s.ids.find(name);
    return it != s.ids.end() ? it->second : 0;
}

const std::string& ShaderGlobals::nameOf(int id) {
    static const std::string none;
    State& s = state();
    if (id <= 0 || id > (int)s.names.size())
        return none;
    return s.names[id - 1];
}

bool ShaderGlobals::isValidName(const std::string& name, std::string* error) {
    auto fail = [error](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (name.empty())
        return fail("a global needs a name");
    if (name.size() > 64)
        return fail("'" + name + "' is longer than 64 characters");
    const unsigned char first = (unsigned char)name[0];
    if (!(std::isalpha(first) || first == '_'))
        return fail("'" + name + "' must start with a letter or '_'");
    for (unsigned char c : name) {
        if (!(std::isalnum(c) || c == '_'))
            return fail("'" + name + "' may only have letters, digits and '_'");
    }
    if (name.rfind("gl_", 0) == 0 || name.rfind("u_", 0) == 0)
        return fail("'" + name + "' starts with '" + name.substr(0, name.find('_') + 1) + "', kept for engine and GLSL names");
    if (name.find("__") != std::string::npos)
        return fail("'" + name + "' has '__', which GLSL keeps");
    return true;
}

// ---------------------------------------------------------------------------- declarations

std::string ShaderGlobals::validate(const std::vector<ShaderGlobalDecl>& decls) {
    if (decls.size() > MAX_DECLARED)
        return "at most " + std::to_string(MAX_DECLARED) + " shader globals (" + std::to_string(decls.size()) + " declared)";
    for (size_t i = 0; i < decls.size(); i++) {
        const ShaderGlobalDecl& decl = decls[i];
        std::string error;
        if (!isValidName(decl.name, &error))
            return error;
        for (size_t j = 0; j < i; j++) {
            if (decls[j].name == decl.name)
                return "'" + decl.name + "' is declared twice";
        }
        if (isArray(decl.type) && (decl.arraySize < 1 || decl.arraySize > MAX_ARRAY_SIZE))
            return "'" + decl.name + "': an array holds 1 to " + std::to_string(MAX_ARRAY_SIZE) + " elements";
    }
    return "";
}

ShaderGlobalDecl ShaderGlobals::makeDecl(const std::string& name, ShaderGlobalType type, unsigned int arraySize) {
    ShaderGlobalDecl decl;
    decl.name = name;
    decl.type = type;
    decl.arraySize = isArray(type) ? std::clamp(arraySize, 1u, MAX_ARRAY_SIZE) : 0;
    if (type == ShaderGlobalType::Color)
        decl.defaultValue = {Vector4(0.0f, 0.0f, 0.0f, 1.0f)};
    decl.defaultValue = defaultValuesOf(decl);
    return decl;
}

void ShaderGlobals::declare(const std::vector<ShaderGlobalDecl>& decls) {
    State& s = state();
    std::vector<ShaderGlobalDecl> valid;
    for (const ShaderGlobalDecl& decl : decls) {
        std::vector<ShaderGlobalDecl> candidate = valid;
        candidate.push_back(decl);
        const std::string error = validate(candidate);
        if (!error.empty()) {
            Log::error("Shader globals: %s (skipped)", error.c_str());
            continue;
        }
        ShaderGlobalDecl normalized = decl;
        if (!isArray(normalized.type))
            normalized.arraySize = 0;
        if (!isTexture(normalized.type))
            normalized.defaultTexture.clear();
        normalized.defaultValue = defaultValuesOf(normalized);
        valid.push_back(normalized);
    }

    // the globals no longer declared are dropped
    for (const ShaderGlobalDecl& old : s.decls) {
        const bool kept = std::any_of(valid.begin(), valid.end(), [&old](const ShaderGlobalDecl& d) { return d.name == old.name; });
        if (kept)
            continue;
        if (Entry* entry = entryOf(findID(old.name))) {
            *entry = Entry();
        }
    }
    for (const ShaderGlobalDecl& decl : valid) {
        const int id = propertyToID(decl.name);
        Entry& entry = *entryOf(id);
        const ShaderGlobalDecl* old = findDecl(decl.name);
        const bool unchanged = entry.exists && entry.declared && old && *old == decl;
        if (!unchanged) {
            entry = Entry();
            entry.exists = true;
            entry.type = decl.type;
            entry.arraySize = decl.arraySize;
            entry.values = decl.defaultValue;
            putTexture(entry, defaultTextureOf(decl.type, decl.defaultTexture));
        }
        entry.declared = true;
        s.created.erase(std::remove(s.created.begin(), s.created.end(), id), s.created.end());
    }
    {
        std::lock_guard<std::mutex> lock(declsMutex());
        s.decls = valid;
    }
    s.namesVersion++;
    changed();
}

const std::vector<ShaderGlobalDecl>& ShaderGlobals::getDeclarations() {
    return state().decls;
}

std::vector<ShaderGlobalDecl> ShaderGlobals::getDeclarationsSnapshot() {
    std::lock_guard<std::mutex> lock(declsMutex());
    return state().decls;
}

void ShaderGlobals::resetToDefaults() {
    State& s = state();
    for (int id : s.created) {
        if (Entry* entry = entryOf(id))
            *entry = Entry();
    }
    s.created.clear();
    for (const ShaderGlobalDecl& decl : s.decls) {
        Entry* entry = entryOf(propertyToID(decl.name));
        entry->exists = true;
        entry->declared = true;
        entry->type = decl.type;
        entry->arraySize = decl.arraySize;
        entry->values = decl.defaultValue;
        putTexture(*entry, defaultTextureOf(decl.type, decl.defaultTexture));
        entry->warnedKind = false;
        entry->warnedTruncated = false;
    }
    s.namesVersion++;
    changed();
}

void ShaderGlobals::clear() {
    State& s = state();
    for (Entry& entry : s.entries)
        entry = Entry();
    {
        std::lock_guard<std::mutex> lock(declsMutex());
        s.decls.clear();
    }
    s.created.clear();
    s.namesVersion++;
    changed();
}

void ShaderGlobals::releaseTextures() {
    // the GPU textures and pixels go (the pools are cleared next); the paths and render targets
    // stay, so the textures load again with the next view (an Android context loss)
    for (Entry& entry : state().entries) {
        if (!entry.texture.empty())
            putTexture(entry, sourceOf(entry.texture));
    }
    changed();
}

void ShaderGlobals::forgetFramebuffer(const Framebuffer* framebuffer) {
    if (!framebuffer)
        return;
    State& s = state();
    for (size_t i = 0; i < s.entries.size(); i++) {
        Entry& entry = s.entries[i];
        if (!entry.texture.isFramebuffer() || entry.texture.getFramebuffer() != framebuffer)
            continue;
        const ShaderGlobalDecl* decl = entry.declared ? findDecl(s.names[i]) : nullptr;
        Log::warn("Shader global '%s': its render target was destroyed; it binds its %s now", s.names[i].c_str(),
                  decl && !decl->defaultTexture.empty() ? "default texture" : "fallback");
        putTexture(entry, decl ? defaultTextureOf(decl->type, decl->defaultTexture) : Texture());
        changed();
    }
}

// ---------------------------------------------------------------------------- values

bool ShaderGlobals::setFloat(int id, float value) {
    return setLeading(id, ShaderGlobalType::Float, Vector4(value, 0.0f, 0.0f, 0.0f), "setGlobalFloat");
}

bool ShaderGlobals::setVector(int id, const Vector4& value) {
    return setLeading(id, ShaderGlobalType::Vector4, value, "setGlobalVector");
}

bool ShaderGlobals::setColor(int id, const Vector4& srgb) {
    // a negative component would be NaN through the gamma curve: 0
    const Vector4 clamped(std::max(0.0f, srgb.x), std::max(0.0f, srgb.y), std::max(0.0f, srgb.z), srgb.w);
    return setLeading(id, ShaderGlobalType::Color, Color::sRGBToLinear(clamped), "setGlobalColor");
}

bool ShaderGlobals::setMatrix(int id, const Matrix4& value) {
    Entry* entry = ensure(id, ShaderGlobalType::Matrix4, 0);
    if (!entry)
        return false;
    if (entry->type != ShaderGlobalType::Matrix4)
        return refuse(*entry, id, "setGlobalMatrix");
    return assign(*entry, {value.column(0), value.column(1), value.column(2), value.column(3)});
}

bool ShaderGlobals::setFloatArray(int id, const float* values, size_t count) {
    std::vector<Vector4> vectors(count);
    for (size_t i = 0; i < count; i++)
        vectors[i] = Vector4(values[i], 0.0f, 0.0f, 0.0f);
    return setArray(id, ShaderGlobalType::FloatArray, vectors.data(), count, "setGlobalFloatArray");
}

bool ShaderGlobals::setVectorArray(int id, const Vector4* values, size_t count) {
    return setArray(id, ShaderGlobalType::Vector4Array, values, count, "setGlobalVectorArray");
}

bool ShaderGlobals::setTexture(int id, const Texture& texture) {
    Entry* entry = ensure(id, texture.isCubeMap() ? ShaderGlobalType::TextureCube : ShaderGlobalType::Texture2D, 0);
    if (!entry)
        return false;
    if (!isTexture(entry->type))
        return refuse(*entry, id, "setGlobalTexture");
    if (!texture.empty() && texture.isCubeMap() != (entry->type == ShaderGlobalType::TextureCube)) {
        Log::error("Shader global '%s' is %s: it takes %s", nameOf(id).c_str(), getTypeName(entry->type),
                   entry->type == ShaderGlobalType::TextureCube ? "a cube map" : "a 2D texture or a render target");
        return false;
    }
    if (entry->texture == texture)
        return true;
    putTexture(*entry, texture);
    changed();
    return true;
}

bool ShaderGlobals::clearTexture(int id) {
    Entry* entry = entryOf(id);
    if (!entry || !entry->exists || !isTexture(entry->type))
        return false;
    const ShaderGlobalDecl* decl = entry->declared ? findDecl(nameOf(id)) : nullptr;
    const Texture fallback = decl ? defaultTextureOf(decl->type, decl->defaultTexture) : Texture();
    if (entry->texture == fallback)
        return true;
    putTexture(*entry, fallback);
    changed();
    return true;
}

bool ShaderGlobals::setValues(int id, const std::vector<Vector4>& values) {
    Entry* entry = entryOf(id);
    if (!entry || !entry->exists || isTexture(entry->type))
        return false;
    std::vector<Vector4> stored = values;
    stored.resize(entry->values.size(), Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    if (entry->type != ShaderGlobalType::Matrix4) {
        for (Vector4& value : stored) value = keepComponents(entry->type, value);
    }
    return assign(*entry, stored);
}

bool ShaderGlobals::exists(int id) {
    const Entry* entry = entryOf(id);
    return entry && entry->exists;
}

bool ShaderGlobals::isDeclared(int id) {
    const Entry* entry = entryOf(id);
    return entry && entry->exists && entry->declared;
}

bool ShaderGlobals::get(int id, ShaderGlobalValue& out) {
    const Entry* entry = entryOf(id);
    if (!entry || !entry->exists)
        return false;
    out.type = entry->type;
    out.arraySize = entry->arraySize;
    out.values = entry->values;
    out.texture = entry->texture;
    out.declared = entry->declared;
    return true;
}

bool ShaderGlobals::getVector(int id, Vector4& out) {
    const Entry* entry = entryOf(id);
    if (!entry || !entry->exists || isTexture(entry->type) || entry->type == ShaderGlobalType::Matrix4 || entry->values.empty())
        return false;
    out = entry->values[0];
    return true;
}

TextureRender* ShaderGlobals::getTextureRender(int id, bool cube, TextureRender* fallback) {
    Entry* entry = entryOf(id);
    if (!entry || !entry->exists || !isTexture(entry->type) || entry->texture.empty() ||
            (entry->type == ShaderGlobalType::TextureCube) != cube)
        return fallback;
    // a render target: its colour texture (made again on a resize), a pointer read
    if (entry->texture.isFramebuffer()) {
        TextureRender* render = entry->texture.getRender(fallback);
        return (render && render->isCreated()) ? render : fallback;
    }
    if (entry->textureFailed)
        return fallback;
    if (entry->resolved && entry->resolvedEpoch == TextureDataPool::getReplaceEpoch())
        return entry->resolved->isCreated() ? entry->resolved : fallback;
    entry->resolved = nullptr;
    // resolved once per value: loading, then the pool's texture (or a failure, remembered)
    const TextureLoadResult result = entry->texture.load();
    if (result.state == ResourceLoadState::Loading)
        return fallback;
    if (result.state != ResourceLoadState::Finished) {
        entry->textureFailed = true;
        Log::warn("Shader global '%s': texture '%s' could not be loaded; it binds the %s fallback", nameOf(id).c_str(),
                  entry->texture.getPath(0).c_str(), cube ? "black cube" : "white");
        return fallback;
    }
    TextureRender* render = entry->texture.getRender(nullptr);
    if (!render)
        return fallback;
    entry->resolved = render;
    entry->resolvedEpoch = TextureDataPool::getReplaceEpoch();
    return render->isCreated() ? render : fallback;
}

bool ShaderGlobals::hasTextureFailed(int id) {
    const Entry* entry = entryOf(id);
    return entry && entry->textureFailed;
}

std::vector<std::string> ShaderGlobals::getNames() {
    State& s = state();
    std::vector<std::string> names;
    for (const ShaderGlobalDecl& decl : s.decls)
        names.push_back(decl.name);
    for (int id : s.created) {
        if (exists(id))
            names.push_back(nameOf(id));
    }
    return names;
}

uint64_t ShaderGlobals::getVersion() {
    return state().version;
}

uint64_t ShaderGlobals::getNamesVersion() {
    return state().namesVersion;
}

// ---------------------------------------------------------------------------- shaders

void GlobalsBlockLayout::update() {
    if (packedVersion != ShaderGlobals::getVersion())
        pack();
}

void GlobalsBlockLayout::pack() {
    bytes.assign(size, 0);
    for (const Member& member : members) {
        const Entry* entry = entryOf(member.globalId);
        if (!entry || !entry->exists || ShaderGlobals::isTexture(entry->type))
            continue;
        const bool matrix = entry->type == ShaderGlobalType::Matrix4;
        const unsigned int count = std::max(1u, member.arrayCount);
        int components = 0;
        switch (member.type) {
            case ShaderUniformType::FLOAT:  components = 1; break;
            case ShaderUniformType::FLOAT2: components = 2; break;
            case ShaderUniformType::FLOAT3: components = 3; break;
            case ShaderUniformType::FLOAT4: components = 4; break;
            case ShaderUniformType::MAT4:
                // a matrix global's four columns (a mat4 of another global reads zero)
                if (matrix && entry->values.size() >= 4) {
                    for (unsigned int c = 0; c < 4; c++)
                        writeFloats(bytes, member.offset + c * 16, entry->values[c], 4);
                }
                continue;
            default:
                continue; // int, mat3: zero (check_shader warns)
        }
        if (matrix)
            continue;
        // std140: an array element per 16 bytes; a longer array than the global's reads zero past it
        for (unsigned int e = 0; e < count && e < entry->values.size(); e++)
            writeFloats(bytes, member.offset + e * 16, entry->values[e], components);
    }
    packedVersion = ShaderGlobals::getVersion();
}

std::shared_ptr<GlobalsBlockLayout> ShaderGlobals::acquireLayout(ShaderData& shaderData, const std::string& blockName) {
    unsigned int sizeBytes = 0;
    const std::vector<ShaderUniform>* reflected = shaderData.getUniformBlockMembers(blockName, sizeBytes);
    if (!reflected || sizeBytes == 0 || reflected->empty())
        return nullptr;

    std::vector<GlobalsBlockLayout::Member> members;
    members.reserve(reflected->size());
    for (const ShaderUniform& uniform : *reflected) {
        GlobalsBlockLayout::Member member;
        member.name = ShaderData::getUniformShortName(uniform.name);
        member.offset = uniform.offset;
        member.type = uniform.type;
        member.arrayCount = std::max(1u, uniform.arrayCount);
        members.push_back(member);
    }
    const unsigned int size = ((sizeBytes + 15) / 16) * 16;
    const std::string signature = layoutSignature(blockName, size, members);

    State& s = state();
    auto found = s.layouts.find(signature);
    if (found != s.layouts.end()) {
        if (std::shared_ptr<GlobalsBlockLayout> layout = found->second.lock())
            return layout;
    }
    // the layouts no program holds any more
    for (auto it = s.layouts.begin(); it != s.layouts.end();) {
        if (it->second.expired())
            it = s.layouts.erase(it);
        else
            ++it;
    }

    std::shared_ptr<GlobalsBlockLayout> layout = std::make_shared<GlobalsBlockLayout>();
    layout->size = size;
    layout->members = std::move(members);
    // a member whose global does not exist yet resolves as soon as a script sets it
    for (GlobalsBlockLayout::Member& member : layout->members)
        member.globalId = propertyToID(member.name);
    layout->pack();
    s.layouts[signature] = layout;
    return layout;
}

std::string ShaderGlobals::getGlslMember(ShaderGlobalType type, unsigned int arraySize, const std::string& name) {
    // a float array is a vec4 array read in .x: the GL targets flatten a block and take only vec4
    // (and mat4) arrays in it, and std140 gives a float array the same 16-byte stride
    if (type == ShaderGlobalType::FloatArray)
        return "vec4 " + name + "[" + std::to_string(std::max(1u, arraySize)) + "]";
    std::string member = std::string(glslScalarType(type)) + " " + name;
    if (isArray(type))
        member += "[" + std::to_string(std::max(1u, arraySize)) + "]";
    return member;
}

unsigned int ShaderGlobals::getSlotCount(ShaderGlobalType type, unsigned int arraySize) {
    return slotsOf(type, arraySize);
}

std::string ShaderGlobals::getGlslBlock(const std::vector<ShaderGlobalDecl>& decls, ShaderStageType stage, const std::vector<std::string>& only) {
    auto wanted = [&only](const std::string& name) {
        return only.empty() || std::find(only.begin(), only.end(), name) != only.end();
    };
    std::string members;
    std::string textures;
    for (const ShaderGlobalDecl& decl : decls) {
        if (!wanted(decl.name))
            continue;
        if (isTexture(decl.type)) {
            textures += std::string("uniform ") + (decl.type == ShaderGlobalType::TextureCube ? "textureCube " : "texture2D ") + decl.name + ";\n";
            textures += "uniform sampler " + decl.name + "_smp;\n";
        } else {
            members += "    " + getGlslMember(decl.type, decl.arraySize, decl.name) + ";\n";
        }
    }
    std::string glsl;
    if (!members.empty()) {
        glsl += std::string("uniform ") + (stage == ShaderStageType::VERTEX ? VERTEX_BLOCK : FRAGMENT_BLOCK) + " {\n" + members + "} globals;\n";
    }
    if (stage == ShaderStageType::FRAGMENT)
        glsl += textures;
    return glsl;
}

std::vector<ShaderGlobalDiagnostic> ShaderGlobals::checkShader(const ShaderData& shaderData, const std::vector<ShaderGlobalDecl>& decls) {
    std::vector<ShaderGlobalDiagnostic> diagnostics;
    auto declOf = [&decls](const std::string& name) -> const ShaderGlobalDecl* {
        for (const ShaderGlobalDecl& decl : decls) {
            if (decl.name == name) return &decl;
        }
        return nullptr;
    };
    auto add = [&diagnostics](const char* code, bool error, const std::string& message) {
        for (const ShaderGlobalDiagnostic& d : diagnostics) {
            if (d.message == message) return; // both stages
        }
        diagnostics.push_back({code, error, message});
    };
    for (const ShaderStage& stage : shaderData.stages) {
        for (const ShaderUniformBlock& block : stage.uniformblocks) {
            if (block.name != VERTEX_BLOCK && block.name != FRAGMENT_BLOCK)
                continue;
            if (block.sizeBytes > MAX_BLOCK_BYTES) {
                add("globals-block-too-large", true, block.name + " is " + std::to_string(block.sizeBytes) + " bytes, over the " +
                    std::to_string(MAX_BLOCK_BYTES) + " (64 vec4) a globals block may take: declare only the globals the stage reads");
            }
            for (const ShaderUniform& uniform : block.uniforms) {
                const std::string member = ShaderData::getUniformShortName(uniform.name);
                const std::string where = "'" + member + "' of " + block.name;
                const ShaderGlobalDecl* decl = declOf(member);
                if (!decl) {
                    add("global-undeclared", false, where + " is not a declared global (Project Settings > Graphics > Shader Globals): "
                        "it reads 0 until a script sets it");
                    continue;
                }
                const char* declared = getTypeName(decl->type);
                const unsigned int count = std::max(1u, uniform.arrayCount);
                int components = 0;
                switch (uniform.type) {
                    case ShaderUniformType::FLOAT: components = 1; break;
                    case ShaderUniformType::FLOAT2: components = 2; break;
                    case ShaderUniformType::FLOAT3: components = 3; break;
                    case ShaderUniformType::FLOAT4: components = 4; break;
                    case ShaderUniformType::MAT4: components = 16; break;
                    default: break;
                }
                if (count > 1 && components > 0 && components < 4 && components != 16) {
                    add("global-type-mismatch", false, where + " is an array of " + std::to_string(components) +
                        "-component elements: the OpenGL and WebGL targets take only vec4 and mat4 arrays in a block "
                        "(declare vec4 " + member + "[" + std::to_string(count) + "], a float array in .x)");
                    continue;
                }
                auto mismatch = [&](const std::string& reads) {
                    add("global-type-mismatch", false, where + " does not match the global's type " + declared + ": " + reads);
                };
                if (isTexture(decl->type)) {
                    mismatch("a texture global binds to a texture uniform of its name, the member reads 0");
                } else if (components == 0) {
                    mismatch("only float, vec2, vec3, vec4, mat4 and float / vec4 array members are filled, it reads 0");
                } else if (decl->type == ShaderGlobalType::Matrix4 || components == 16) {
                    if (decl->type != ShaderGlobalType::Matrix4 || components != 16 || count > 1)
                        mismatch("a mat4 member reads a matrix4 global and nothing else, it reads 0");
                } else if (isArray(decl->type)) {
                    // a float array reads as float x[N] or as vec4 x[N] (in .x; GL's only kind)
                    const bool fits = components == 4 || (decl->type == ShaderGlobalType::FloatArray && components == 1);
                    if (count == 1)
                        mismatch("it reads element 0 of the array");
                    else if (!fits)
                        mismatch("each element reads the leading components of the global's");
                    else if (count != decl->arraySize)
                        mismatch(count > decl->arraySize ? "the elements past the global's " + std::to_string(decl->arraySize) + " read 0"
                                                         : "it reads the first " + std::to_string(count) + " of the global's " +
                                                           std::to_string(decl->arraySize) + " elements");
                } else {
                    const int expected = getComponentCount(decl->type);
                    if (count > 1)
                        mismatch("element 0 reads the global, the others 0");
                    else if (components < expected)
                        mismatch("it reads the first " + std::to_string(components) + " of its " + std::to_string(expected) + " components");
                    else if (components > expected)
                        mismatch("the components past its " + std::to_string(expected) + " read 0");
                }
            }
        }
        // a texture uniform named like a declared global of another kind
        for (const ShaderTexture& texture : stage.textures) {
            const ShaderGlobalDecl* decl = declOf(texture.name);
            if (!decl)
                continue;
            const bool cube = texture.type == TextureType::TEXTURE_CUBE;
            if (!isTexture(decl->type)) {
                add("global-type-mismatch", false, "texture '" + texture.name + "' is named like the " + getTypeName(decl->type) +
                    " global: it binds the default texture");
            } else if (cube != (decl->type == ShaderGlobalType::TextureCube)) {
                add("global-type-mismatch", false, "texture '" + texture.name + "' is a " + (cube ? "textureCube" : "texture2D") +
                    " but the global is " + getTypeName(decl->type) + ": it binds the default texture");
            }
        }
    }
    return diagnostics;
}

// ---------------------------------------------------------------------------- bindings

bool ShaderGlobals::setBinding(ShaderGlobalBindings& bindings, const std::string& member, const std::string& global) {
    if (member.empty())
        return false;
    auto it = std::find_if(bindings.begin(), bindings.end(), [&member](const std::pair<std::string, std::string>& b) { return b.first == member; });
    if (global.empty()) {
        if (it == bindings.end())
            return false;
        bindings.erase(it);
        return true;
    }
    std::string error;
    if (!isValidName(global, &error)) {
        Log::error("Shader global binding of '%s': %s", member.c_str(), error.c_str());
        return false;
    }
    const Entry* entry = entryOf(findID(global));
    if (entry && entry->exists && (isTexture(entry->type) || entry->type == ShaderGlobalType::Matrix4)) {
        Log::error("Shader global '%s' is %s: a value member cannot follow it", global.c_str(), getTypeName(entry->type));
        return false;
    }
    if (it != bindings.end()) {
        if (it->second == global)
            return false;
        it->second = global;
        return true;
    }
    bindings.push_back({member, global});
    return true;
}

std::string ShaderGlobals::getBinding(const ShaderGlobalBindings& bindings, const std::string& member) {
    for (const auto& binding : bindings) {
        if (binding.first == member)
            return binding.second;
    }
    return "";
}

bool ShaderGlobals::isOutdated(const ShaderGlobalBindings& bindings, uint64_t versionSeen) {
    return !bindings.empty() && versionSeen != state().version;
}

bool ShaderGlobals::getBoundValue(const std::string& global, Vector4& out) {
    out = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    return getVector(findID(global), out);
}

// ---------------------------------------------------------------------------- types

const char* ShaderGlobals::getTypeName(ShaderGlobalType type) {
    switch (type) {
        case ShaderGlobalType::Float: return "float";
        case ShaderGlobalType::Vector2: return "vector2";
        case ShaderGlobalType::Vector3: return "vector3";
        case ShaderGlobalType::Vector4: return "vector4";
        case ShaderGlobalType::Color: return "color";
        case ShaderGlobalType::Matrix4: return "matrix4";
        case ShaderGlobalType::FloatArray: return "float_array";
        case ShaderGlobalType::Vector4Array: return "vector4_array";
        case ShaderGlobalType::Texture2D: return "texture2d";
        case ShaderGlobalType::TextureCube: return "texturecube";
    }
    return "vector4";
}

bool ShaderGlobals::parseTypeName(const std::string& text, ShaderGlobalType& type) {
    static const ShaderGlobalType all[] = {
        ShaderGlobalType::Float, ShaderGlobalType::Vector2, ShaderGlobalType::Vector3, ShaderGlobalType::Vector4,
        ShaderGlobalType::Color, ShaderGlobalType::Matrix4, ShaderGlobalType::FloatArray, ShaderGlobalType::Vector4Array,
        ShaderGlobalType::Texture2D, ShaderGlobalType::TextureCube};
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (ShaderGlobalType candidate : all) {
        if (lower == getTypeName(candidate)) {
            type = candidate;
            return true;
        }
    }
    return false;
}

bool ShaderGlobals::isTexture(ShaderGlobalType type) {
    return type == ShaderGlobalType::Texture2D || type == ShaderGlobalType::TextureCube;
}

bool ShaderGlobals::isArray(ShaderGlobalType type) {
    return type == ShaderGlobalType::FloatArray || type == ShaderGlobalType::Vector4Array;
}

int ShaderGlobals::getComponentCount(ShaderGlobalType type) {
    switch (type) {
        case ShaderGlobalType::Float:
        case ShaderGlobalType::FloatArray: return 1;
        case ShaderGlobalType::Vector2: return 2;
        case ShaderGlobalType::Vector3: return 3;
        case ShaderGlobalType::Matrix4: return 16;
        case ShaderGlobalType::Texture2D:
        case ShaderGlobalType::TextureCube: return 0;
        default: return 4;
    }
}
