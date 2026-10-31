// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CookCodecs.h"

#include "Log.h"

#include <mutex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::cook;

namespace {

    // forward compatibility within a format major (R4): a field a newer editor wrote is skipped,
    // with one warning per field id for the whole run
    void warnOnce(uint32_t id, const char* table, const char* what) {
        static std::mutex mutex;
        static std::set<uint32_t> warned;
        std::lock_guard<std::mutex> lock(mutex);
        if (warned.insert(id).second)
            Log::warn("Cooked content: %s field 0x%08x of %s skipped (a newer editor's content?)", what, id, table);
    }

}

void cook::writeStruct(Writer& w, const StructTable& table, const void* owner) {
    for (size_t i = 0; i < table.count; i++) {
        const FieldEntry& field = table.fields[i];
        w.u32(field.id);
        w.u8(static_cast<uint8_t>(field.kind));
        const size_t at = w.beginLength();
        field.write(w, owner);
        w.endLength(at);
    }
    w.u32(0);
}

void cook::readStruct(Reader& r, const StructTable& table, void* owner) {
    size_t next = 0;
    while (!r.failed()) {
        const uint32_t id = r.u32();
        if (id == 0 || r.failed()) break;
        const FieldKind kind = static_cast<FieldKind>(r.u8());
        Reader payload = r.record();
        if (r.failed()) break;
        // fields come in table order: the next one first, else a search
        const FieldEntry* field = nullptr;
        if (next < table.count && table.fields[next].id == id) {
            field = &table.fields[next];
        } else {
            for (size_t i = 0; i < table.count; i++) {
                if (table.fields[i].id == id) {
                    field = &table.fields[i];
                    break;
                }
            }
        }
        if (!field) {
            warnOnce(id, table.name, "unknown");
            continue;
        }
        next = static_cast<size_t>(field - table.fields) + 1;
        if (field->kind != kind) {
            warnOnce(id, table.name, "a changed kind of");
            continue;
        }
        field->read(payload, owner);
        if (payload.failed())
            Log::error("Cooked content: %s.%s: %s", table.name, field->name, payload.error().c_str());
        else if (!payload.atEnd())
            Log::error("Cooked content: %s.%s: %zu bytes left unread", table.name, field->name, payload.remaining());
    }
}

void cook::visitStructTextures(const StructTable& table, void* owner, const TextureVisitor& visit) {
    for (size_t i = 0; i < table.count; i++)
        if (table.fields[i].textures) table.fields[i].textures(owner, visit);
}

void Codec<Texture>::write(Writer& w, const Texture& texture) {
    if (texture.empty()) {
        w.u8(0);
        return;
    }
    const bool cube = texture.isCubeMap() || texture.getNumFaces() == 6;
    const std::string p0 = texture.getPath(0);
    if (cube) {
        bool otherFaces = false;
        for (size_t f = 1; f < 6; f++) otherFaces = otherFaces || !texture.getPath(f).empty();
        if (!p0.empty() && !otherFaces) {
            w.u8(2);   // one file holding the six faces
            w.string(p0);
            Codec<Guid>::write(w, texture.getPathGuid(0));
        } else {
            w.u8(3);   // six files, engine face order
            for (size_t f = 0; f < 6; f++) {
                w.string(texture.getPath(f));
                Codec<Guid>::write(w, texture.getPathGuid(f));
            }
        }
    } else if (!p0.empty()) {
        w.u8(1);
        w.string(p0);
        Codec<Guid>::write(w, texture.getPathGuid(0));
    } else {
        // made in memory: a model's or a generated one, rebuilt at runtime from its id
        w.u8(4);
        w.string(texture.getId());
    }
    w.f32(texture.getSvgScale());
    Codec<TextureAlphaBorder>::write(w, texture.getAlphaBorder());
    w.boolean(texture.isAlphaBorderFixed());
    Codec<TextureFilter>::write(w, texture.getMinFilter());
    Codec<TextureFilter>::write(w, texture.getMagFilter());
    Codec<TextureWrap>::write(w, texture.getWrapU());
    Codec<TextureWrap>::write(w, texture.getWrapV());
    w.boolean(texture.isReleaseDataAfterLoad() || texture.isFramebuffer());
}

void Codec<Texture>::read(Reader& r, Texture& texture) {
    const uint8_t source = r.u8();
    if (source == 0) {
        texture = Texture();
        return;
    }
    switch (source) {
        case 1: {
            const std::string path = r.string();
            Guid guid;
            Codec<Guid>::read(r, guid);
            texture.setPath(path);
            if (!guid.isNull()) texture.setPathGuid(0, guid);
            break;
        }
        case 2: {
            const std::string path = r.string();
            Guid guid;
            Codec<Guid>::read(r, guid);
            texture.setCubeMap(path);
            if (!guid.isNull()) texture.setPathGuid(0, guid);
            break;
        }
        case 3: {
            std::string paths[6];
            Guid guids[6];
            for (size_t f = 0; f < 6; f++) {
                paths[f] = r.string();
                Codec<Guid>::read(r, guids[f]);
            }
            // setCubePaths takes (front, back, left, right, up, down); the engine's faces are
            // 0 right, 1 left, 2 top, 3 bottom, 4 front, 5 back
            texture.setCubePaths(paths[4], paths[5], paths[1], paths[0], paths[2], paths[3]);
            for (size_t f = 0; f < 6; f++)
                if (!guids[f].isNull()) texture.setPathGuid(f, guids[f]);
            break;
        }
        case 4:
            texture.setId(r.string());
            break;
        default:
            r.fail("unknown texture source " + std::to_string(source));
            return;
    }
    // after the path setters, which reset the scale and the alpha border
    const float svgScale = r.f32();
    if (svgScale != 1.0f) texture.setSvgScale(svgScale);
    TextureAlphaBorder alphaBorder = TextureAlphaBorder::AUTO;
    Codec<TextureAlphaBorder>::read(r, alphaBorder);
    const bool alphaBorderFixed = r.boolean();
    if (alphaBorder != TextureAlphaBorder::AUTO) texture.setAlphaBorder(alphaBorder);
    else if (alphaBorderFixed) texture.setAlphaBorderAuto(true);
    TextureFilter minFilter{}, magFilter{};
    TextureWrap wrapU{}, wrapV{};
    Codec<TextureFilter>::read(r, minFilter);
    Codec<TextureFilter>::read(r, magFilter);
    Codec<TextureWrap>::read(r, wrapU);
    Codec<TextureWrap>::read(r, wrapV);
    texture.setMinFilter(minFilter);
    texture.setMagFilter(magFilter);
    texture.setWrapU(wrapU);
    texture.setWrapV(wrapV);
    if (!r.boolean()) texture.setReleaseDataAfterLoad(false);
}
