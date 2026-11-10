// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleMeshFile.h"

#include <cstring>

using namespace jasperengine::titleart;

namespace {

    enum Tag : uint32_t { Vertices = 1, Wrap, Indices, Slots, Spans, Parts, Rigs, Bounds, Stats };

    // little-endian writes and reads (every engine target is little-endian; the bytes are spelled
    // out so the file never depends on the host)
    struct Writer {
        std::vector<uint8_t> out;
        void u8(uint8_t v) { out.push_back(v); }
        void u16(uint16_t v) {
            u8((uint8_t)v);
            u8((uint8_t)(v >> 8));
        }
        void u32(uint32_t v) {
            for (int i = 0; i < 4; i++) u8((uint8_t)(v >> (8 * i)));
        }
        void u64(uint64_t v) {
            for (int i = 0; i < 8; i++) u8((uint8_t)(v >> (8 * i)));
        }
        void f32(float f) {
            uint32_t v;
            std::memcpy(&v, &f, 4);
            u32(v);
        }
        size_t begin(Tag tag) {
            u32(tag);
            u32(0);
            return out.size();
        }
        void end(size_t start) {
            const uint32_t length = (uint32_t)(out.size() - start);
            for (int i = 0; i < 4; i++) out[start - 4 + i] = (uint8_t)(length >> (8 * i));
        }
    };

    struct Reader {
        const std::vector<uint8_t>& in;
        size_t at = 0;
        bool ok = true;
        bool need(size_t n) {
            if (at + n > in.size()) ok = false;
            return ok;
        }
        uint8_t u8() { return need(1) ? in[at++] : 0; }
        uint16_t u16() {
            if (!need(2)) return 0;
            const uint16_t v = (uint16_t)(in[at] | in[at + 1] << 8);
            at += 2;
            return v;
        }
        uint32_t u32() {
            if (!need(4)) return 0;
            uint32_t v = 0;
            for (int i = 0; i < 4; i++) v |= (uint32_t)in[at + i] << (8 * i);
            at += 4;
            return v;
        }
        uint64_t u64() {
            if (!need(8)) return 0;
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= (uint64_t)in[at + i] << (8 * i);
            at += 8;
            return v;
        }
        float f32() {
            const uint32_t v = u32();
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }
    };

    void writeVertex(Writer& w, const TitleMeshVertex& v) {
        for (float p : v.position) w.f32(p);
        for (int16_t n : v.normal) w.u16((uint16_t)n);
        for (uint16_t id : v.ids) w.u16(id);
    }

    TitleMeshVertex readVertex(Reader& r) {
        TitleMeshVertex v;
        for (float& p : v.position) p = r.f32();
        for (int16_t& n : v.normal) n = (int16_t)r.u16();
        for (uint16_t& id : v.ids) id = r.u16();
        return v;
    }

}

std::vector<uint8_t> jasperengine::titleart::writeTitleMesh(const TitleMesh& mesh) {
    Writer w;
    w.out.insert(w.out.end(), {'J', 'T', 'M', '1'});
    w.u32(TITLE_MESH_FILE_VERSION);
    w.u8((uint8_t)mesh.tier);
    w.u64(mesh.sourceHash);
    w.u32(TITLE_3D_VERSION);
    size_t s = w.begin(Vertices);
    w.u32((uint32_t)mesh.vertices.size());
    for (const TitleMeshVertex& v : mesh.vertices) writeVertex(w, v);
    w.end(s);
    s = w.begin(Wrap);
    w.u32((uint32_t)mesh.wrap.size());
    for (uint16_t h : mesh.wrap) w.u16(h);
    w.end(s);
    s = w.begin(Indices);
    w.u8(mesh.wideIndices ? 1 : 0);
    w.u32((uint32_t)mesh.indexCount());
    for (size_t i = 0; i < mesh.indexCount(); i++) {
        if (mesh.wideIndices) w.u32(mesh.indices32[i]);
        else w.u16(mesh.indices16[i]);
    }
    w.end(s);
    s = w.begin(Slots);
    w.u32((uint32_t)mesh.slots.size());
    for (const TitleMeshSlot& slot : mesh.slots) {
        w.u32(slot.firstIndex);
        w.u32(slot.indexCount);
        w.u8((uint8_t)slot.role);
        w.u16((uint16_t)slot.band);
        w.u16(slot.block);
    }
    w.end(s);
    s = w.begin(Spans);
    w.u32((uint32_t)mesh.spans.size());
    for (const TitleMeshSpan& span : mesh.spans) {
        for (float f : span.line) w.f32(f);
        for (float f : span.chr) w.f32(f);
    }
    w.end(s);
    s = w.begin(Parts);
    w.u32((uint32_t)mesh.parts.size());
    for (const TitleMeshPart& part : mesh.parts) {
        w.f32(part.zFront);
        w.f32(part.zBack);
        w.u16(part.rig);
        w.u16(part.flags);
    }
    w.end(s);
    s = w.begin(Rigs);
    w.u32((uint32_t)mesh.rigs.size());
    for (const TitleMeshRig& rig : mesh.rigs) {
        w.u32(rig.charIndex);
        for (float f : rig.pivot) w.f32(f);
    }
    w.end(s);
    s = w.begin(Bounds);
    for (float f : mesh.boundsMin) w.f32(f);
    for (float f : mesh.boundsMax) w.f32(f);
    w.f32(mesh.pivotZ);
    w.end(s);
    s = w.begin(Stats);
    w.u32(mesh.stats.triangles);
    w.u32(mesh.stats.vertices);
    w.u32(mesh.stats.solids);
    w.f32(mesh.stats.buildMs);
    w.u8(mesh.stats.fallbackUsed ? 1 : 0);
    w.end(s);
    return std::move(w.out);
}

std::shared_ptr<TitleMesh> jasperengine::titleart::readTitleMesh(const std::vector<uint8_t>& bytes, std::string* error) {
    auto fail = [&](const std::string& message) -> std::shared_ptr<TitleMesh> {
        if (error) *error = message;
        return nullptr;
    };
    if (bytes.size() < 21 || std::memcmp(bytes.data(), "JTM1", 4) != 0) return fail("corrupt: not a .titlemesh");
    Reader r{bytes};
    r.at = 4;
    const uint32_t version = r.u32();
    if (version != TITLE_MESH_FILE_VERSION) return fail("unknown_version: .titlemesh version " + std::to_string(version));
    auto mesh = std::make_shared<TitleMesh>();
    const uint8_t tier = r.u8();
    if (tier > (uint8_t)TitleTier::Low) return fail("corrupt: tier " + std::to_string(tier));
    mesh->tier = (TitleTier)tier;
    mesh->sourceHash = r.u64();
    if (r.u32() != TITLE_3D_VERSION) return fail("unknown_version: a mesh of another 3D title version");
    while (r.ok && r.at < bytes.size()) {
        const uint32_t tag = r.u32();
        const uint32_t length = r.u32();
        if (!r.need(length)) break;
        const size_t end = r.at + length;
        switch (tag) {
            case Vertices: {
                const uint32_t n = r.u32();
                if ((uint64_t)n * 24 > length) return fail("corrupt: vertices");
                mesh->vertices.reserve(n);
                for (uint32_t i = 0; i < n; i++) mesh->vertices.push_back(readVertex(r));
                break;
            }
            case Wrap: {
                const uint32_t n = r.u32();
                for (uint32_t i = 0; i < n && r.ok; i++) mesh->wrap.push_back(r.u16());
                break;
            }
            case Indices: {
                mesh->wideIndices = r.u8() != 0;
                const uint32_t n = r.u32();
                for (uint32_t i = 0; i < n && r.ok; i++) {
                    if (mesh->wideIndices) mesh->indices32.push_back(r.u32());
                    else mesh->indices16.push_back(r.u16());
                }
                break;
            }
            case Slots: {
                const uint32_t n = r.u32();
                for (uint32_t i = 0; i < n && r.ok; i++) {
                    TitleMeshSlot slot;
                    slot.firstIndex = r.u32();
                    slot.indexCount = r.u32();
                    slot.role = (TitleSlotRole)r.u8();
                    slot.band = (int16_t)r.u16();
                    slot.block = r.u16();
                    mesh->slots.push_back(slot);
                }
                break;
            }
            case Spans: {
                const uint32_t n = r.u32();
                for (uint32_t i = 0; i < n && r.ok; i++) {
                    TitleMeshSpan span;
                    for (float& f : span.line) f = r.f32();
                    for (float& f : span.chr) f = r.f32();
                    mesh->spans.push_back(span);
                }
                break;
            }
            case Parts: {
                const uint32_t n = r.u32();
                for (uint32_t i = 0; i < n && r.ok; i++) {
                    TitleMeshPart part;
                    part.zFront = r.f32();
                    part.zBack = r.f32();
                    part.rig = r.u16();
                    part.flags = r.u16();
                    mesh->parts.push_back(part);
                }
                break;
            }
            case Rigs: {
                const uint32_t n = r.u32();
                for (uint32_t i = 0; i < n && r.ok; i++) {
                    TitleMeshRig rig;
                    rig.charIndex = r.u32();
                    for (float& f : rig.pivot) f = r.f32();
                    mesh->rigs.push_back(rig);
                }
                break;
            }
            case Bounds:
                for (float& f : mesh->boundsMin) f = r.f32();
                for (float& f : mesh->boundsMax) f = r.f32();
                mesh->pivotZ = r.f32();
                break;
            case Stats:
                mesh->stats.triangles = r.u32();
                mesh->stats.vertices = r.u32();
                mesh->stats.solids = r.u32();
                mesh->stats.buildMs = r.f32();
                mesh->stats.fallbackUsed = r.u8() != 0;
                break;
            default:
                break;  // a later version's section: skipped by its length
        }
        if (!r.ok || r.at > end) return fail("corrupt: section " + std::to_string(tag));
        r.at = end;
    }
    if (!r.ok) return fail("corrupt: truncated");
    const uint32_t vertexCount = (uint32_t)mesh->vertices.size();
    for (size_t i = 0; i < mesh->indexCount(); i++) {
        if (mesh->index(i) >= vertexCount) return fail("corrupt: an index past the vertices");
    }
    return mesh;
}
