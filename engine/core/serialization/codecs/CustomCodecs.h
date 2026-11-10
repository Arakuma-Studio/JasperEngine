// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The cooked encodings of the values whose S08 field is Custom (a codec of its own in the
// editor's reflection tables, editor/reflect/components/*): scripts and their properties, a
// particle system's exposed values and inline effect, lattice poses, nav-grid tile areas,
// curves, gradients, Palette Swap bindings, and the OutOfLineValue wrapper. One file for the writer (the editor's SceneCooker) and
// the reader (the player), so the two never disagree (docs/specs/S30-shipping.md R4).

#include "serialization/CookCodecs.h"

#include "component/LatticeComponent.h"
#include "component/NavGridComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/ScriptComponent.h"
#include "component/TilemapLayerComponent.h"
#include "manager/PrefabAsset.h"
#include "math/Curve.h"
#include "particles/ParticleEffectCodec.h"
#include "pixelart/PaletteBinding.h"
#include "util/OutOfLineValue.h"

#include <variant>

namespace jasperengine::cook {

    // set or not, then the value
    template<typename T>
    struct Codec<OutOfLineValue<T>> {
        static void write(Writer& w, const OutOfLineValue<T>& v) {
            const T* set = const_cast<OutOfLineValue<T>&>(v).editIfSet();
            w.u8(set ? 1 : 0);
            if (set) Codec<T>::write(w, *set);
        }
        static void read(Reader& r, OutOfLineValue<T>& v) {
            if (r.u8()) {
                T value;
                Codec<T>::read(r, value);
                v = value;
            } else {
                v = OutOfLineValue<T>();
            }
        }
    };
    template<typename T>
    struct TextureVisit<OutOfLineValue<T>> {
        static void visit(OutOfLineValue<T>& v, const TextureVisitor& f) {
            if (T* set = v.editIfSet()) TextureVisit<T>::visit(*set, f);
        }
    };

    // a Tilemap Layer's cells (S50 R87): the chunks' raw bytes, in the save order
    template<>
    struct Codec<TileCellsRef> {
        static void write(Writer& w, const TileCellsRef& ref) {
            const TileCells& cells = ref.read();
            const std::vector<ChunkCoord> chunks = cells.sortedChunks();
            w.varint(chunks.size());
            for (const ChunkCoord& coord : chunks) {
                w.varint((uint64_t)(uint32_t)coord.x);
                w.varint((uint64_t)(uint32_t)coord.y);
                const std::string bytes = cells.encodeChunkBytes(coord);
                w.blob(bytes.data(), bytes.size());
            }
        }
        static void read(Reader& r, TileCellsRef& ref) {
            ref = TileCellsRef();
            const uint64_t count = r.varint();
            for (uint64_t i = 0; i < count && !r.failed(); i++) {
                ChunkCoord coord;
                coord.x = (int32_t)(uint32_t)r.varint();
                coord.y = (int32_t)(uint32_t)r.varint();
                const std::span<const uint8_t> bytes = r.blob();
                std::string error;
                if (!r.failed() && !ref.edit().decodeChunkBytes(coord, std::string_view((const char*)bytes.data(), bytes.size()), error)) {
                    r.fail("tilemap chunk: " + error);
                    return;
                }
            }
        }
    };

    // a Palette Swap binding (S43, Material::shaderPalettes): its palettes by path and GUID
    template<>
    struct Codec<pixelart::PaletteTarget> {
        static void write(Writer& w, const pixelart::PaletteTarget& t) {
            Codec<AssetRef>::write(w, t.palette);
            w.string(t.variant);
        }
        static void read(Reader& r, pixelart::PaletteTarget& t) {
            Codec<AssetRef>::read(r, t.palette);
            t.variant = r.string();
        }
    };

    template<>
    struct Codec<pixelart::PaletteBinding> {
        static void write(Writer& w, const pixelart::PaletteBinding& b) {
            Codec<AssetRef>::write(w, b.source);
            Codec<std::vector<pixelart::PaletteTarget>>::write(w, b.targets);
            w.boolean(b.useVariants);
        }
        static void read(Reader& r, pixelart::PaletteBinding& b) {
            Codec<AssetRef>::read(r, b.source);
            Codec<std::vector<pixelart::PaletteTarget>>::read(r, b.targets);
            b.useVariants = r.boolean();
        }
    };

    template<>
    struct Codec<CurveKey> {
        static void write(Writer& w, const CurveKey& k) {
            w.f32(k.time);
            w.f32(k.value);
            w.f32(k.inTangent);
            w.f32(k.outTangent);
        }
        static void read(Reader& r, CurveKey& k) {
            k.time = r.f32();
            k.value = r.f32();
            k.inTangent = r.f32();
            k.outTangent = r.f32();
        }
    };

    // the keys, then (S27) the wrap modes and each key's tangent modes (CurveKey::mode); a curve
    // cooked before them ends after its keys and reads as clamp, free tangents
    template<>
    struct Codec<Curve> {
        static void write(Writer& w, const Curve& c) {
            Codec<std::vector<CurveKey>>::write(w, c.keys);
            w.u8(static_cast<uint8_t>(c.preWrapMode));
            w.u8(static_cast<uint8_t>(c.postWrapMode));
            for (const CurveKey& k : c.keys) w.u8(k.mode);
        }
        static void read(Reader& r, Curve& c) {
            Codec<std::vector<CurveKey>>::read(r, c.keys);
            if (r.atEnd()) return;
            c.preWrapMode = static_cast<CurveWrapMode>(r.u8());
            c.postWrapMode = static_cast<CurveWrapMode>(r.u8());
            for (CurveKey& k : c.keys) k.mode = r.u8();
        }
    };

    // a colour gradient (the Line and Trail Renderers' colorGradient): its colour keys (time,
    // sRGB rgb), alpha keys (time, alpha), then the mode and the linear flag
    template<>
    struct Codec<GradientColorKey> {
        static void write(Writer& w, const GradientColorKey& k) {
            w.f32(k.time);
            Codec<Vector3>::write(w, k.color);
        }
        static void read(Reader& r, GradientColorKey& k) {
            k.time = r.f32();
            Codec<Vector3>::read(r, k.color);
        }
    };

    template<>
    struct Codec<GradientAlphaKey> {
        static void write(Writer& w, const GradientAlphaKey& k) {
            w.f32(k.time);
            w.f32(k.alpha);
        }
        static void read(Reader& r, GradientAlphaKey& k) {
            k.time = r.f32();
            k.alpha = r.f32();
        }
    };

    template<>
    struct Codec<Gradient> {
        static void write(Writer& w, const Gradient& g) {
            Codec<std::vector<GradientColorKey>>::write(w, g.colorKeys);
            Codec<std::vector<GradientAlphaKey>>::write(w, g.alphaKeys);
            w.u8(static_cast<uint8_t>(g.mode));
            w.u8(g.linear ? 1 : 0);
        }
        static void read(Reader& r, Gradient& g) {
            Codec<std::vector<GradientColorKey>>::read(r, g.colorKeys);
            Codec<std::vector<GradientAlphaKey>>::read(r, g.alphaKeys);
            g.mode = static_cast<GradientMode>(r.u8());
            g.linear = r.u8() != 0;
        }
    };

    // a prefab script property: the GUID and path it names (resolved by the player's PrefabManager)
    template<>
    struct Codec<PrefabAsset> {
        static void write(Writer& w, const PrefabAsset& p) {
            w.string(p.getGuid());
            w.string(p.getPath());
        }
        static void read(Reader& r, PrefabAsset& p) {
            const std::string guid = r.string();
            const std::string path = r.string();
            p = PrefabAsset(guid, path);
        }
    };

    // the variant's index, then its value
    template<>
    struct Codec<ScriptPropertyValue> {
        static void write(Writer& w, const ScriptPropertyValue& v) {
            w.u8(static_cast<uint8_t>(v.index()));
            std::visit([&w](const auto& value) {
                using V = std::decay_t<decltype(value)>;
                if constexpr (!std::is_same_v<V, std::monostate>) Codec<V>::write(w, value);
            }, v);
        }
        static void read(Reader& r, ScriptPropertyValue& v) {
            const uint8_t index = r.u8();
            switch (index) {
                case 0: v = std::monostate{}; break;
                case 1: { bool x = false; Codec<bool>::read(r, x); v = x; break; }
                case 2: { int x = 0; Codec<int>::read(r, x); v = x; break; }
                case 3: { float x = 0; Codec<float>::read(r, x); v = x; break; }
                case 4: { std::string x; Codec<std::string>::read(r, x); v = x; break; }
                case 5: { Vector2 x; Codec<Vector2>::read(r, x); v = x; break; }
                case 6: { Vector3 x; Codec<Vector3>::read(r, x); v = x; break; }
                case 7: { Vector4 x; Codec<Vector4>::read(r, x); v = x; break; }
                case 8: { EntityReference x; Codec<EntityReference>::read(r, x); v = x; break; }
                case 9: { PrefabAsset x; Codec<PrefabAsset>::read(r, x); v = x; break; }
                case 10: { AssetRef x; Codec<AssetRef>::read(r, x); v = x; break; }
                default: r.fail("a script property value of kind " + std::to_string(index)); break;
            }
        }
    };
    static_assert(std::variant_size_v<ScriptPropertyValue> == 11, "a new ScriptPropertyValue kind needs its cooked encoding");

    template<>
    struct Codec<ScriptProperty> {
        static void write(Writer& w, const ScriptProperty& p) {
            w.string(p.name);
            w.string(p.displayName);
            Codec<ScriptPropertyType>::write(w, p.type);
            Codec<ScriptPropertyValue>::write(w, p.value);
            Codec<ScriptPropertyValue>::write(w, p.defaultValue);
            w.string(p.ptrTypeName);
            w.string(p.assetType);
        }
        static void read(Reader& r, ScriptProperty& p) {
            p.name = r.string();
            p.displayName = r.string();
            Codec<ScriptPropertyType>::read(r, p.type);
            Codec<ScriptPropertyValue>::read(r, p.value);
            Codec<ScriptPropertyValue>::read(r, p.defaultValue);
            p.ptrTypeName = r.string();
            p.assetType = r.string();
        }
    };

    template<>
    struct Codec<ScriptEntry> {
        static void write(Writer& w, const ScriptEntry& s) {
            Codec<ScriptType>::write(w, s.type);
            Codec<AssetRef>::write(w, s.path);
            Codec<AssetRef>::write(w, s.headerPath);
            w.string(s.className);
            w.u8(s.enabled ? 1 : 0);
            Codec<std::vector<ScriptProperty>>::write(w, s.properties);
        }
        static void read(Reader& r, ScriptEntry& s) {
            Codec<ScriptType>::read(r, s.type);
            Codec<AssetRef>::read(r, s.path);
            Codec<AssetRef>::read(r, s.headerPath);
            s.className = r.string();
            s.enabled = r.u8() != 0;
            Codec<std::vector<ScriptProperty>>::read(r, s.properties);
        }
    };

    template<>
    struct Codec<ParticleExposedValue> {
        static void write(Writer& w, const ParticleExposedValue& v) {
            w.string(v.name);
            Codec<Vector4>::write(w, v.value);
        }
        static void read(Reader& r, ParticleExposedValue& v) {
            v.name = r.string();
            Codec<Vector4>::read(r, v.value);
        }
    };

    template<>
    struct Codec<LatticePose> {
        static void write(Writer& w, const LatticePose& p) {
            w.string(p.name);
            Codec<std::vector<Vector3>>::write(w, p.offsets);
        }
        static void read(Reader& r, LatticePose& p) {
            p.name = r.string();
            Codec<std::vector<Vector3>>::read(r, p.offsets);
        }
    };

    template<>
    struct Codec<NavGridTileArea> {
        static void write(Writer& w, const NavGridTileArea& a) {
            w.string(a.rect);
            Codec<int>::write(w, a.area);
        }
        static void read(Reader& r, NavGridTileArea& a) {
            a.rect = r.string();
            Codec<int>::read(r, a.area);
        }
    };

    // the effect document as its .particles text (the engine's own reader and writer)
    template<>
    struct Codec<particles::ParticleEffectDocument> {
        static void write(Writer& w, const particles::ParticleEffectDocument& d) { w.string(particles::ParticleEffectCodec::write(d)); }
        static void read(Reader& r, particles::ParticleEffectDocument& d) {
            const std::string text = r.string();
            std::vector<particles::Diagnostic> diagnostics;
            d = particles::ParticleEffectDocument();
            if (!particles::ParticleEffectCodec::read(text, d, diagnostics)) r.fail("an inline particle effect that does not parse");
        }
    };

}
