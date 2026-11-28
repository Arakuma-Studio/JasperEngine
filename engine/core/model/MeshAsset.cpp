// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MeshAsset.h"

#include "component/MeshComponent.h"
#include "io/AtomicFile.h"
#include "io/Data.h"
#include "model/ModelCpuData.h"
#include "serialization/CookStream.h"
#include "serialization/codecs/CookTypes.h"
#include "subsystem/MeshSystem.h"

#include <algorithm>
#include <cstring>
#include <set>

using namespace jasperengine;

namespace {

    constexpr size_t kHeaderBytes = 8;

    Entity noEntity(uint32_t) { return NULL_ENTITY; }

    void setError(std::string* error, const std::string& text) {
        if (error) *error = text;
    }

    // The header and the string table; `body` is what follows them
    bool readFrame(std::span<const uint8_t> bytes, uint16_t& major, uint16_t& minor, std::vector<std::string>& strings,
                   std::span<const uint8_t>& body, std::string* error) {
        if (bytes.size() < kHeaderBytes || std::memcmp(bytes.data(), MeshAsset::MAGIC, 4) != 0) {
            setError(error, "not a .mesh file");
            return false;
        }
        major = static_cast<uint16_t>(bytes[4] | (bytes[5] << 8));
        minor = static_cast<uint16_t>(bytes[6] | (bytes[7] << 8));
        if (major != MeshAsset::FORMAT_MAJOR) {
            setError(error, ".mesh format " + std::to_string(major) + "." + std::to_string(minor) + " (this engine reads " +
                     std::to_string(MeshAsset::FORMAT_MAJOR) + ".x)");
            return false;
        }
        static const std::vector<std::string> none;
        cook::Reader table(bytes.subspan(kHeaderBytes), none, &noEntity);
        const uint64_t count = table.varint();
        for (uint64_t i = 0; i < count && !table.failed(); i++) {
            const std::span<const uint8_t> text = table.blob();
            strings.emplace_back(reinterpret_cast<const char*>(text.data()), text.size());
        }
        if (table.failed()) {
            setError(error, "the string table is cut short");
            return false;
        }
        body = bytes.subspan(bytes.size() - table.remaining());
        return true;
    }

    struct Body {
        AABB bounds;
        unsigned int vertexCount = 0;
        InterleavedBuffer buffer;
        IndexBuffer indices;
        std::vector<ExternalBuffer> external;
        struct Part {
            PrimitiveType primitiveType = PrimitiveType::TRIANGLES;
            unsigned int vertexCount = 0;
            std::map<AttributeType, Attribute> attributes;
        };
        std::vector<Part> submeshes;
    };

    bool readBody(std::span<const uint8_t> bytes, Body& out, std::string* error) {
        uint16_t major = 0, minor = 0;
        std::vector<std::string> strings;
        std::span<const uint8_t> body;
        if (!readFrame(bytes, major, minor, strings, body, error)) return false;
        cook::Reader r(body, strings, &noEntity);
        cook::read(r, out.bounds);
        out.vertexCount = static_cast<unsigned int>(r.varint());
        cook::read(r, out.buffer);
        cook::read(r, out.indices);
        const uint64_t external = r.varint();
        for (uint64_t b = 0; b < external && !r.failed(); b++) {
            ExternalBuffer buffer;
            cook::read(r, buffer);
            out.external.push_back(buffer);
        }
        const uint64_t submeshes = r.varint();
        for (uint64_t s = 0; s < submeshes && !r.failed(); s++) {
            Body::Part part;
            cook::read(r, part.primitiveType);
            part.vertexCount = static_cast<unsigned int>(r.varint());
            cook::read(r, part.attributes);
            out.submeshes.push_back(std::move(part));
        }
        if (r.failed()) {
            setError(error, "the file is cut short or damaged (" + r.error() + ")");
            return false;
        }
        if (out.external.size() > ARRAY_INDEX_LIMIT || out.submeshes.size() > ARRAY_INDEX_LIMIT) {
            setError(error, "more buffers or submeshes than a mesh holds (" + std::to_string(ARRAY_INDEX_LIMIT) + ")");
            return false;
        }
        return true;
    }

}

size_t MeshAsset::geometryBytes(const MeshComponent& mesh) {
    size_t bytes = mesh.buffer.getSize() + mesh.indices.getSize();
    for (unsigned int b = 0; b < mesh.numExternalBuffers; b++) bytes += mesh.eBuffers[b].getSize();
    return bytes;
}

std::vector<uint8_t> MeshAsset::encode(const MeshComponent& mesh) {
    ModelCpuData::ensure(mesh, "saving a .mesh");
    cook::StringTable strings;
    cook::Writer w(strings, [](Entity) { return 0u; });
    // the counts a load gives, not what a frame set (RenderSystem fills them from the buffers): the
    // same geometry gives the same bytes drawn or not
    cook::write(w, mesh.verticesAABB);
    w.varint(mesh.vertexCount != 0 ? mesh.vertexCount : mesh.buffer.getCount());
    cook::write(w, mesh.buffer);
    cook::write(w, mesh.indices);
    w.varint(mesh.numExternalBuffers);
    for (unsigned int b = 0; b < mesh.numExternalBuffers; b++) cook::write(w, mesh.eBuffers[b]);
    w.varint(mesh.numSubmeshes);
    for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
        const Submesh& submesh = mesh.submeshes[s];
        cook::write(w, submesh.primitiveType);
        w.varint(MeshSystem::loadedVertexCount(mesh, submesh));
        cook::write(w, submesh.attributes);
    }

    std::vector<uint8_t> out(MAGIC, MAGIC + 4);
    for (uint16_t value : {FORMAT_MAJOR, FORMAT_MINOR}) {
        out.push_back(static_cast<uint8_t>(value & 0xff));
        out.push_back(static_cast<uint8_t>(value >> 8));
    }
    cook::StringTable unused;
    cook::Writer table(unused, [](Entity) { return 0u; });
    table.varint(strings.strings().size());
    for (const std::string& text : strings.strings()) table.blob(text.data(), text.size());
    out.insert(out.end(), table.data().begin(), table.data().end());
    out.insert(out.end(), w.data().begin(), w.data().end());
    return out;
}

bool MeshAsset::decode(std::span<const uint8_t> bytes, MeshComponent& mesh, std::string* error) {
    Body body;
    if (!readBody(bytes, body, error)) return false;

    mesh.buffer.clearAll();
    mesh.buffer = body.buffer;
    mesh.indices.clearAll();
    mesh.indices = body.indices;
    for (unsigned int b = 0; b < mesh.numExternalBuffers; b++) mesh.eBuffers[b].clearAll();
    for (size_t b = 0; b < body.external.size(); b++) mesh.eBuffers[b] = body.external[b];
    mesh.numExternalBuffers = static_cast<unsigned int>(body.external.size());
    mesh.vertexCount = body.vertexCount;

    const unsigned int before = mesh.numSubmeshes;
    for (size_t s = 0; s < body.submeshes.size(); s++) {
        // a submesh the component did not have starts as a new one (its material the default)
        if (s >= before) mesh.submeshes[s] = Submesh();
        Submesh& submesh = mesh.submeshes[s];
        submesh.primitiveType = body.submeshes[s].primitiveType;
        submesh.vertexCount = body.submeshes[s].vertexCount;
        submesh.attributes = body.submeshes[s].attributes;
    }
    mesh.numSubmeshes = static_cast<unsigned int>(body.submeshes.size());
    mesh.verticesAABB = body.bounds;
    return true;
}

bool MeshAsset::inspect(std::span<const uint8_t> bytes, Info& info, std::string* error) {
    std::vector<std::string> strings;
    std::span<const uint8_t> rest;
    if (!readFrame(bytes, info.major, info.minor, strings, rest, error)) return false;
    Body body;
    if (!readBody(bytes, body, error)) return false;
    info.vertexCount = body.vertexCount;
    info.indexCount = body.indices.getCount();
    info.bounds = body.bounds;
    info.geometryBytes = body.buffer.getSize() + body.indices.getSize();
    std::set<AttributeType> attributes;
    for (const auto& [type, attribute] : body.buffer.getAttributes()) attributes.insert(type);
    for (const ExternalBuffer& buffer : body.external) {
        info.geometryBytes += buffer.getSize();
        for (const auto& [type, attribute] : buffer.getAttributes()) attributes.insert(type);
    }
    info.attributes.assign(attributes.begin(), attributes.end());
    info.submeshes.clear();
    for (const Body::Part& part : body.submeshes) info.submeshes.push_back({part.primitiveType, part.vertexCount});
    return true;
}

bool MeshAsset::load(const std::string& path, MeshComponent& mesh, std::string* error) {
    Data file;
    if (file.open(path.c_str()) != FileErrors::FILEDATA_OK) {
        setError(error, "file not found");
        return false;
    }
    file.seek(0);
    const std::string bytes = file.readString(file.length());
    return decode(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), mesh, error);
}

bool MeshAsset::save(const std::filesystem::path& file, const MeshComponent& mesh, std::string* error) {
    const std::vector<uint8_t> bytes = encode(mesh);
    io::AtomicWriteOptions options;
    options.skipIfUnchanged = true;
    const io::WriteResult written = io::writeFileAtomic(file, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), options);
    if (!written.ok()) {
        setError(error, io::describeWriteError(written));
        return false;
    }
    return true;
}
