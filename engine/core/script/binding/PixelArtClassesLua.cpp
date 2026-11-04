// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The pixel-art family's Lua (docs/specs/S43-palettes.md R42, §6.7): Palette (load, new, its
// colours, ramps and variants) and Palette Swap on Sprite, Tilemap and MeshPolygon
// (palette, paletteRow, paletteCycleSpeed, setPaletteTarget). Indices are the palette's own,
// from 0; colours are linear in the engine's convention (they show as their exact bytes).
//
// Lua is compiled as C: luaL_error longjmps over C++ frames, so the functions that raise errors
// do their work in helpers and raise only from frames holding nothing with a destructor.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "Log.h"
#include "component/MeshComponent.h"
#include "object/MeshPolygon.h"
#include "object/Sprite.h"
#include "object/Tilemap.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteMaterial.h"
#include "pixelart/PixelColor.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace jasperengine;

namespace jasperengine::pixelart {

    // A palette as Lua holds it: the palette, and the name a material's binding knows it by
    // (its asset path, or the runtime key of one made with Palette.new)
    struct LuaPalette {
        std::shared_ptr<const Palette> palette;
        std::string key;
    };

}

#ifndef DISABLE_LUA_BINDINGS
namespace jasperengine::pixelart {
namespace {

    using ErrorText = char[1024];

    void copyError(ErrorText& error, const std::string& text) {
        std::snprintf(error, sizeof(ErrorText), "%s", text.c_str());
    }

    const Palette& paletteOf(const LuaPalette& self) {
        static const Palette empty;
        return self.palette ? *self.palette : empty;
    }

    std::optional<LuaPalette> loadPalette(const std::string& path) {
        std::vector<Diagnostic> diagnostics;
        std::shared_ptr<const Palette> palette = PaletteLibrary::get(path, &diagnostics);
        if (!palette) {
            for (const Diagnostic& diagnostic : diagnostics) {
                if (diagnostic.severity == Diagnostic::Severity::Error)
                    Log::error("Palette.load(\"%s\"): %s", path.c_str(), diagnostic.message.c_str());
            }
            return std::nullopt;
        }
        return LuaPalette{palette, path};
    }

    // Palette.new({ "#000000", "#ff00ff", Vector4(1, 1, 1, 1), ... })
    bool newPalette(lua_State* L, int index, LuaPalette& out, ErrorText& error) {
        if (!lua_istable(L, index)) {
            copyError(error, "Palette.new: give a table of colours (\"#rrggbb\", \"#rrggbbaa\" or Vector4)");
            return false;
        }
        auto palette = std::make_shared<Palette>();
        palette->name = "Runtime";
        palette->source.kind = "runtime";
        const int count = (int)lua_rawlen(L, index);
        for (int i = 1; i <= count; i++) {
            lua_rawgeti(L, index, i);
            Rgba8 color;
            bool ok = false;
            if (lua_type(L, -1) == LUA_TSTRING) {
                ok = parseHex(lua_tostring(L, -1), color);
            } else if (luabridge::isInstance<Vector4>(L, -1)) {
                color = fromEngineLinear(luabridge::Stack<Vector4>::get(L, -1).value());
                ok = true;
            }
            lua_pop(L, 1);
            if (!ok) {
                copyError(error, "Palette.new: colour " + std::to_string(i) + " is not \"#rrggbb\", \"#rrggbbaa\" or a Vector4");
                return false;
            }
            palette->colors.push_back(color);
        }
        std::vector<Diagnostic> diagnostics;
        if (!PaletteCodec::validate(*palette, diagnostics)) {
            for (const Diagnostic& diagnostic : diagnostics) {
                if (diagnostic.severity == Diagnostic::Severity::Error) {
                    copyError(error, "Palette.new: " + diagnostic.message);
                    return false;
                }
            }
        }
        out.palette = palette;
        out.key = PaletteLibrary::addRuntime(out.palette);
        return true;
    }

    int paletteNew(lua_State* L) {
        ErrorText error;
        bool ok;
        {
            LuaPalette created;
            ok = newPalette(L, 1, created, error);
            if (ok) (void)luabridge::Stack<LuaPalette>::push(L, created);
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 1;
    }

    luabridge::LuaRef namesTable(lua_State* L, const std::vector<std::string>& names) {
        luabridge::LuaRef table = luabridge::newTable(L);
        for (size_t i = 0; i < names.size(); i++) table[i + 1] = names[i];
        return table;
    }

    luabridge::LuaRef rampTable(const LuaPalette& self, const std::string& name, lua_State* L) {
        luabridge::LuaRef table = luabridge::newTable(L);
        const std::vector<Rgba8> colors = paletteOf(self).rampColors(name);
        for (size_t i = 0; i < colors.size(); i++) table[i + 1] = toEngineLinear(colors[i]);
        return table;
    }

    // the meshes a Lua object draws with (its submeshes' materials)
    template<class T>
    MeshComponent* meshOf(lua_State* L) {
        T* object = luabridge::detail::Userdata::get<T>(L, 1, false);
        return object ? &object->template getComponent<MeshComponent>() : nullptr;
    }

    std::string validRows(const std::vector<std::string>& names) {
        std::string text = "0 or \"\" (the original)";
        for (size_t i = 1; i < names.size(); i++) text += ", " + std::to_string(i) + " or \"" + names[i] + "\"";
        return text;
    }

    // sprite.palette = "Team Blue" | 2 | "" | 0
    bool applyPalette(MeshComponent& mesh, lua_State* L, int index, ErrorText& error) {
        if (mesh.numSubmeshes == 0) return true;
        const std::vector<std::string> names = paletteRowNames(mesh.submeshes[0].material);
        int row = -1;
        if (lua_type(L, index) == LUA_TNUMBER) {
            const lua_Number value = lua_tonumber(L, index);
            row = (int)value;
            if ((lua_Number)row != value || row < 0 || row >= (int)names.size()) row = -1;
        } else if (lua_type(L, index) == LUA_TSTRING) {
            const std::string name = lua_tostring(L, index);
            for (size_t i = 0; i < names.size() && row < 0; i++) {
                if (names[i] == name) row = (int)i;
            }
        }
        if (row < 0) {
            const char* given = luaL_tolstring(L, index, nullptr);
            copyError(error, std::string("palette ") + given + " is not one of this sprite's palettes: " + validRows(names));
            lua_pop(L, 1);
            return false;
        }
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++) setPaletteRow(mesh.submeshes[s].material, row);
        mesh.needUpdateShaderUniforms = true;
        return true;
    }

    template<class T>
    std::string getPaletteName(const T* object) {
        const MeshComponent& mesh = const_cast<T*>(object)->template getComponent<MeshComponent>();
        if (mesh.numSubmeshes == 0) return std::string();
        const std::vector<std::string> names = paletteRowNames(mesh.submeshes[0].material);
        const int row = getPaletteRow(mesh.submeshes[0].material);
        return row > 0 && row < (int)names.size() ? names[row] : std::string();
    }

    template<class T>
    int setPaletteName(lua_State* L) {
        ErrorText error;
        bool ok = true;
        if (MeshComponent* mesh = meshOf<T>(L)) ok = applyPalette(*mesh, L, 2, error);
        if (!ok) return luaL_error(L, "%s", error);
        return 0;
    }

    bool applyTarget(MeshComponent& mesh, lua_State* L, ErrorText& error) {
        const int row = (int)luaL_checkinteger(L, 2);
        std::string key;
        if (lua_type(L, 3) == LUA_TSTRING) {
            key = lua_tostring(L, 3);
        } else if (luabridge::isInstance<LuaPalette>(L, 3)) {
            key = luabridge::Stack<LuaPalette>::get(L, 3).value().key;
        } else {
            copyError(error, "setPaletteTarget(row, palette): the palette is a Palette (Palette.load, Palette.new) or a path");
            return false;
        }
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
            std::string message;
            if (!setPaletteTarget(mesh.submeshes[s].material, row, key, &message)) {
                copyError(error, "setPaletteTarget: " + message);
                return false;
            }
        }
        return true;
    }

    template<class T>
    int setPaletteTargetOf(lua_State* L) {
        ErrorText error;
        bool ok = true;
        if (MeshComponent* mesh = meshOf<T>(L)) ok = applyTarget(*mesh, L, error);
        if (!ok) return luaL_error(L, "%s", error);
        return 0;
    }

    template<class T>
    int getRow(const T* object) {
        const MeshComponent& mesh = const_cast<T*>(object)->template getComponent<MeshComponent>();
        return mesh.numSubmeshes > 0 ? getPaletteRow(mesh.submeshes[0].material) : 0;
    }

    template<class T>
    void setRow(T* object, int row) {
        MeshComponent& mesh = object->template getComponent<MeshComponent>();
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++) setPaletteRow(mesh.submeshes[s].material, row);
        mesh.needUpdateShaderUniforms = true;
    }

    template<class T>
    float getCycleSpeed(const T* object) {
        const MeshComponent& mesh = const_cast<T*>(object)->template getComponent<MeshComponent>();
        return mesh.numSubmeshes > 0 ? getPaletteCycleSpeed(mesh.submeshes[0].material) : 1.0f;
    }

    template<class T>
    void setCycleSpeed(T* object, float speed) {
        MeshComponent& mesh = object->template getComponent<MeshComponent>();
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++) setPaletteCycleSpeed(mesh.submeshes[s].material, speed);
        mesh.needUpdateShaderUniforms = true;
    }

}
}
#endif

void LuaBinding::registerPixelArtClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    using namespace jasperengine::pixelart;

    luabridge::getGlobalNamespace(L)
        .beginClass<LuaPalette>("Palette")
        .addStaticFunction("load", &loadPalette)
        .addStaticFunction("new", &paletteNew)
        .addProperty("count", +[](const LuaPalette* self) -> int { return paletteOf(*self).count(); })
        .addProperty("name", +[](const LuaPalette* self) -> std::string { return paletteOf(*self).name; })
        .addProperty("variants", +[](const LuaPalette* self, lua_State* L) -> luabridge::LuaRef {
            std::vector<std::string> names;
            for (const PaletteVariant& variant : paletteOf(*self).variants) names.push_back(variant.name);
            return namesTable(L, names);
        })
        .addProperty("ramps", +[](const LuaPalette* self, lua_State* L) -> luabridge::LuaRef {
            std::vector<std::string> names;
            for (const PaletteRamp& ramp : paletteOf(*self).ramps) names.push_back(ramp.name);
            return namesTable(L, names);
        })
        .addFunction("getColor", +[](const LuaPalette* self, int index) {
            const Palette& palette = paletteOf(*self);
            return palette.validIndex(index) ? toEngineLinear(palette.colors[index]) : Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        })
        .addFunction("getHex", +[](const LuaPalette* self, int index) {
            const Palette& palette = paletteOf(*self);
            return palette.validIndex(index) ? toHex(palette.colors[index]) : std::string();
        })
        .addFunction("nearest", +[](const LuaPalette* self, const Vector4& color, std::optional<std::string> distance) -> int {
            ColorMetric metric = ColorMetric::OkLab;
            if (distance && !parseColorMetric(*distance, metric))
                Log::warn("Palette:nearest: unknown distance \"%s\" (oklab, ciede2000, rgb): OKLab used", distance->c_str());
            return paletteOf(*self).nearest(fromEngineLinear(color), metric);
        })
        .addFunction("getRamp", &rampTable)
        .addFunction("rampColor", +[](const LuaPalette* self, const std::string& name, float t) {
            const std::vector<Rgba8> colors = paletteOf(*self).rampColors(name);
            return colors.empty() ? Vector4(1.0f, 1.0f, 1.0f, 1.0f) : toEngineLinear(rampAt(colors, t));
        })
        .endClass();

    // one chain per class (not a template taking the name): the Lua API extractor reads the names
    luabridge::getGlobalNamespace(L)
        .beginClass<Sprite>("Sprite")
        .addProperty("palette", &getPaletteName<Sprite>, &setPaletteName<Sprite>)
        .addProperty("paletteRow", &getRow<Sprite>, &setRow<Sprite>)
        .addProperty("paletteCycleSpeed", &getCycleSpeed<Sprite>, &setCycleSpeed<Sprite>)
        .addFunction("setPaletteTarget", &setPaletteTargetOf<Sprite>)
        .endClass()
        .beginClass<Tilemap>("Tilemap")
        .addProperty("palette", &getPaletteName<Tilemap>, &setPaletteName<Tilemap>)
        .addProperty("paletteRow", &getRow<Tilemap>, &setRow<Tilemap>)
        .addProperty("paletteCycleSpeed", &getCycleSpeed<Tilemap>, &setCycleSpeed<Tilemap>)
        .addFunction("setPaletteTarget", &setPaletteTargetOf<Tilemap>)
        .endClass()
        .beginClass<MeshPolygon>("MeshPolygon")
        .addProperty("palette", &getPaletteName<MeshPolygon>, &setPaletteName<MeshPolygon>)
        .addProperty("paletteRow", &getRow<MeshPolygon>, &setRow<MeshPolygon>)
        .addProperty("paletteCycleSpeed", &getCycleSpeed<MeshPolygon>, &setCycleSpeed<MeshPolygon>)
        .addFunction("setPaletteTarget", &setPaletteTargetOf<MeshPolygon>)
        .endClass();
#endif //DISABLE_LUA_BINDINGS
}
