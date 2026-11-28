// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// 2D sorting and Sprite Batch in Lua (docs/specs/S52-sprite-instancing.md R37, §6.6): SortingLayer
// (layers, nameToId, idToName, getLayerValue), the renderers' sortingLayerId, sortingLayerName and
// sortingOrder (Mesh and Sprite, ParticleSystem, Points, LineRenderer, TrailRenderer), SortingGroup,
// and SpriteBatch with its bulk calls (flat tables, validated once, typed errors).
//
// Lua is compiled as C: luaL_error longjmps over C++ frames, so the functions that raise errors
// do their work in helpers and raise only from frames holding nothing with a destructor.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Log.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/SortingGroupComponent.h"
#include "component/SpriteBatchComponent.h"
#include "component/SpriteComponent.h"
#include "component/TrailRendererComponent.h"
#include "object/LineRenderer.h"
#include "object/Mesh.h"
#include "object/ParticleSystem.h"
#include "object/Points.h"
#include "object/SortingGroup.h"
#include "object/Sprite.h"
#include "object/SpriteBatch.h"
#include "object/TrailRenderer.h"
#include "registry/SortingLayers.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;

#ifndef DISABLE_LUA_BINDINGS
namespace {

    using SortingErrorText = char[1024];

    void sortingCopyError(SortingErrorText& error, const std::string& text) {
        std::snprintf(error, sizeof(SortingErrorText), "%s", text.c_str());
    }

    // ---- renderers: the two fields of their component

    template<class O, class C>
    int getLayerId(const O* object) {
        return const_cast<O*>(object)->template getComponent<C>().sortingLayer;
    }

    template<class O, class C>
    std::string getLayerName(const O* object) {
        return SortingLayers::idToName(getLayerId<O, C>(object));
    }

    template<class O, class C>
    int getOrder(const O* object) {
        return const_cast<O*>(object)->template getComponent<C>().sortingOrder;
    }

    template<class O, class C>
    void setOrder(O* object, int order) {
        object->template getComponent<C>().sortingOrder = std::clamp(order, -32768, 32767);
    }

    template<class O, class C>
    bool applyLayer(lua_State* L, SortingErrorText& error) {
        O* object = luabridge::detail::Userdata::get<O>(L, 1, false);
        if (!object) return true;
        int32_t id = -1;
        if (lua_type(L, 2) == LUA_TSTRING) {
            const std::string name = lua_tostring(L, 2);
            id = SortingLayers::nameToId(name);
            if (id < 0) {
                sortingCopyError(error, "unknown_sorting_layer: no Sorting Layer is named \"" + name +
                                        "\" (Project Settings > Tags and Layers > Sorting Layers; SortingLayer.layers lists them)");
                return false;
            }
        } else if (lua_type(L, 2) == LUA_TNUMBER) {
            id = (int32_t)lua_tointeger(L, 2);
            if (!SortingLayers::isValid(id)) {
                sortingCopyError(error, "unknown_sorting_layer: no Sorting Layer has id " + std::to_string(id) +
                                        " (SortingLayer.layers lists them)");
                return false;
            }
        } else {
            sortingCopyError(error, "sortingLayer: a Sorting Layer name or id");
            return false;
        }
        object->template getComponent<C>().sortingLayer = id;
        return true;
    }

    template<class O, class C>
    int setLayer(lua_State* L) {
        SortingErrorText error;
        if (!applyLayer<O, C>(L, error)) return luaL_error(L, "%s", error);
        return 0;
    }

    // ---- SortingLayer

    luabridge::LuaRef sortingLayersTable(lua_State* L) {
        luabridge::LuaRef layers = luabridge::newTable(L);
        int index = 1;
        for (const SortingLayerInfo& layer : SortingLayers::getLayers()) {
            luabridge::LuaRef entry = luabridge::newTable(L);
            entry["id"] = layer.id;
            entry["name"] = layer.name;
            entry["value"] = SortingLayers::getLayerValue(layer.id);
            layers[index++] = entry;
        }
        return layers;
    }

    // ---- SpriteBatch: flat tables in, typed errors out

    // the numbers of the table at `index` (false: not a table of numbers)
    bool readNumbers(lua_State* L, int index, std::vector<float>& out) {
        if (!lua_istable(L, index)) return false;
        const size_t count = (size_t)lua_rawlen(L, index);
        out.resize(count);
        for (size_t i = 0; i < count; i++) {
            lua_rawgeti(L, index, (lua_Integer)i + 1);
            int isNumber = 0;
            const lua_Number value = lua_tonumberx(L, -1, &isNumber);
            lua_pop(L, 1);
            if (!isNumber) return false;
            out[i] = (float)value;
        }
        return true;
    }

    // frames: indices or the Sprite's frame names
    bool readFrames(lua_State* L, int index, const SpriteBatch& batch, std::vector<int32_t>& out, SortingErrorText& error) {
        if (!lua_istable(L, index)) {
            sortingCopyError(error, "setFrames: a table of frame indices or names");
            return false;
        }
        const size_t count = (size_t)lua_rawlen(L, index);
        out.resize(count);
        for (size_t i = 0; i < count; i++) {
            lua_rawgeti(L, index, (lua_Integer)i + 1);
            if (lua_type(L, -1) == LUA_TSTRING) {
                const std::string name = lua_tostring(L, -1);
                lua_pop(L, 1);
                const int frame = batch.frameId(name);
                if (frame < 0) {
                    sortingCopyError(error, "unknown_frame: setFrames: the Sprite has no frame \"" + name + "\"");
                    return false;
                }
                out[i] = frame;
            } else {
                int isNumber = 0;
                const lua_Integer value = lua_tointegerx(L, -1, &isNumber);
                lua_pop(L, 1);
                if (!isNumber) {
                    sortingCopyError(error, "setFrames: entry " + std::to_string(i + 1) + " is not a frame index or name");
                    return false;
                }
                out[i] = (int32_t)value;
            }
        }
        return true;
    }

    SpriteBatch* batchOf(lua_State* L) {
        return luabridge::detail::Userdata::get<SpriteBatch>(L, 1, false);
    }

    enum class BatchStream { POSITIONS, ROTATIONS, SCALES, COLORS, FRAMES, FX };

    bool applyStream(lua_State* L, BatchStream stream, SortingErrorText& error) {
        SpriteBatch* batch = batchOf(L);
        if (!batch) return true;
        static const char* const names[] = {"setPositions", "setRotations", "setScales", "setColors", "setFrames", "setFx"};
        const char* name = names[(int)stream];
        std::string result;
        if (stream == BatchStream::FRAMES) {
            std::vector<int32_t> frames;
            if (!readFrames(L, 2, *batch, frames, error)) return false;
            result = batch->setFrames(frames);
        } else if (stream == BatchStream::FX) {
            if (lua_type(L, 2) != LUA_TSTRING) {
                sortingCopyError(error, "setFx(slot, amounts): the slot is \"palette\", \"flash\", \"fade\" or \"dissolve\"");
                return false;
            }
            const std::string slot = lua_tostring(L, 2);
            std::vector<float> values;
            if (!readNumbers(L, 3, values)) {
                sortingCopyError(error, "setFx(slot, amounts): amounts is a table of numbers");
                return false;
            }
            result = batch->setFx(slot, values);
        } else {
            std::vector<float> values;
            if (!readNumbers(L, 2, values)) {
                sortingCopyError(error, std::string(name) + ": a table of numbers");
                return false;
            }
            const int stride = lua_isnoneornil(L, 3) ? (stream == BatchStream::POSITIONS ? 2 : 1) : (int)luaL_checkinteger(L, 3);
            if (stream == BatchStream::POSITIONS) result = batch->setPositions(values, stride);
            else if (stream == BatchStream::ROTATIONS) result = batch->setRotations(values);
            else if (stream == BatchStream::SCALES) result = batch->setScales(values, stride);
            else result = batch->setColors(values);
        }
        if (!result.empty()) {
            sortingCopyError(error, "SpriteBatch:" + std::string(name) + ": " + result);
            return false;
        }
        return true;
    }

    template<BatchStream S>
    int batchStream(lua_State* L) {
        SortingErrorText error;
        if (!applyStream(L, S, error)) return luaL_error(L, "%s", error);
        return 0;
    }

    bool resolveFrameId(lua_State* L, int& frame, SortingErrorText& error) {
        SpriteBatch* batch = batchOf(L);
        const std::string name = luaL_checkstring(L, 2);
        frame = batch ? batch->frameId(name) : -1;
        if (frame < 0) {
            sortingCopyError(error, "unknown_frame: SpriteBatch:frameId: the Sprite has no frame \"" + name + "\"");
            return false;
        }
        return true;
    }

    int batchFrameId(lua_State* L) {
        SortingErrorText error;
        int frame = -1;
        if (!resolveFrameId(L, frame, error)) return luaL_error(L, "%s", error);
        lua_pushinteger(L, frame);
        return 1;
    }

    // add(x, y [, frame]): frame an index or a name; returns the instance's index
    bool applyAdd(lua_State* L, int& index, SortingErrorText& error) {
        SpriteBatch* batch = batchOf(L);
        if (!batch) return true;
        const float x = (float)luaL_checknumber(L, 2);
        const float y = (float)luaL_checknumber(L, 3);
        int frame = 0;
        if (lua_type(L, 4) == LUA_TSTRING) {
            const std::string name = lua_tostring(L, 4);
            frame = batch->frameId(name);
            if (frame < 0) {
                sortingCopyError(error, "unknown_frame: SpriteBatch:add: the Sprite has no frame \"" + name + "\"");
                return false;
            }
        } else if (!lua_isnoneornil(L, 4)) {
            frame = (int)luaL_checkinteger(L, 4);
        }
        index = batch->add(x, y, frame);
        if (index < 0) {
            sortingCopyError(error, "frame_range: SpriteBatch:add: frame " + std::to_string(frame) + " is out of range (frameCount " +
                                    std::to_string(batch->getFrameCount()) + ")");
            return false;
        }
        return true;
    }

    int batchAdd(lua_State* L) {
        SortingErrorText error;
        int index = -1;
        if (!applyAdd(L, index, error)) return luaL_error(L, "%s", error);
        lua_pushinteger(L, index);
        return 1;
    }

}
#endif //DISABLE_LUA_BINDINGS

void LuaBinding::registerSortingClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS
    // The raw component getters return borrowed references that need registered metatables.
    luabridge::getGlobalNamespace(L)
        .beginClass<SortingGroupComponent>("SortingGroupComponent")
        .endClass()
        .beginClass<SpriteBatchComponent>("SpriteBatchComponent")
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<SortingLayers>("SortingLayer")
        .addStaticProperty("layers", &sortingLayersTable)
        .addStaticFunction("nameToId", &SortingLayers::nameToId)
        .addStaticFunction("idToName", &SortingLayers::idToName)
        .addStaticFunction("getLayerValue", &SortingLayers::getLayerValue)
        .endClass();

    // one chain per class (not a template taking the name): the Lua API extractor reads the names
    luabridge::getGlobalNamespace(L)
        .beginClass<Mesh>("Mesh")
        .addProperty("sortingLayerId", &getLayerId<Mesh, MeshComponent>, &setLayer<Mesh, MeshComponent>)
        .addProperty("sortingLayerName", &getLayerName<Mesh, MeshComponent>, &setLayer<Mesh, MeshComponent>)
        .addProperty("sortingOrder", &getOrder<Mesh, MeshComponent>, &setOrder<Mesh, MeshComponent>)
        .endClass()
        .beginClass<Sprite>("Sprite")
        .addProperty("spriteSortPoint", +[](const Sprite* self){ return const_cast<Sprite*>(self)->getComponent<SpriteComponent>().spriteSortPoint; },
                     +[](Sprite* self, SpriteSortPoint point){ self->getComponent<SpriteComponent>().spriteSortPoint = point; })
        .endClass()
        .beginClass<ParticleSystem>("ParticleSystem")
        .addProperty("sortingLayerId", &getLayerId<ParticleSystem, ParticleSystemComponent>, &setLayer<ParticleSystem, ParticleSystemComponent>)
        .addProperty("sortingLayerName", &getLayerName<ParticleSystem, ParticleSystemComponent>, &setLayer<ParticleSystem, ParticleSystemComponent>)
        .addProperty("sortingOrder", &getOrder<ParticleSystem, ParticleSystemComponent>, &setOrder<ParticleSystem, ParticleSystemComponent>)
        .endClass()
        .beginClass<Points>("Points")
        .addProperty("sortingLayerId", &getLayerId<Points, PointsComponent>, &setLayer<Points, PointsComponent>)
        .addProperty("sortingLayerName", &getLayerName<Points, PointsComponent>, &setLayer<Points, PointsComponent>)
        .addProperty("sortingOrder", &getOrder<Points, PointsComponent>, &setOrder<Points, PointsComponent>)
        .endClass()
        .beginClass<LineRenderer>("LineRenderer")
        .addProperty("sortingLayerId", &getLayerId<LineRenderer, LineRendererComponent>, &setLayer<LineRenderer, LineRendererComponent>)
        .addProperty("sortingLayerName", &getLayerName<LineRenderer, LineRendererComponent>, &setLayer<LineRenderer, LineRendererComponent>)
        .addProperty("sortingOrder", &getOrder<LineRenderer, LineRendererComponent>, &setOrder<LineRenderer, LineRendererComponent>)
        .endClass()
        .beginClass<TrailRenderer>("TrailRenderer")
        .addProperty("sortingLayerId", &getLayerId<TrailRenderer, TrailRendererComponent>, &setLayer<TrailRenderer, TrailRendererComponent>)
        .addProperty("sortingLayerName", &getLayerName<TrailRenderer, TrailRendererComponent>, &setLayer<TrailRenderer, TrailRendererComponent>)
        .addProperty("sortingOrder", &getOrder<TrailRenderer, TrailRendererComponent>, &setOrder<TrailRenderer, TrailRendererComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<SortingGroup, EntityHandle>("SortingGroup")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &SortingGroup::isEnabled, &SortingGroup::setEnabled)
        .addProperty("sortingLayerId", &SortingGroup::getSortingLayerId, &SortingGroup::setSortingLayerId)
        .addProperty("sortingLayerName", &SortingGroup::getSortingLayerName, &SortingGroup::setSortingLayerName)
        .addProperty("sortingOrder", &SortingGroup::getSortingOrder, &SortingGroup::setSortingOrder)
        .addFunction("getSortingGroupComponent", &SortingGroup::getComponent<SortingGroupComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<SpriteBatch, EntityHandle>("SpriteBatch")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &SpriteBatch::isEnabled, &SpriteBatch::setEnabled)
        .addProperty("mode", &SpriteBatch::getMode, &SpriteBatch::setMode)
        .addProperty("space", &SpriteBatch::getSpace, &SpriteBatch::setSpace)
        .addProperty("capacity", &SpriteBatch::getCapacity, &SpriteBatch::setCapacity)
        .addProperty("sortInstances", &SpriteBatch::isSortInstances, &SpriteBatch::setSortInstances)
        .addProperty("count", &SpriteBatch::getCount)
        .addProperty("frameCount", &SpriteBatch::getFrameCount)
        .addFunction("frameId", &batchFrameId)
        .addFunction("setPositions", &batchStream<BatchStream::POSITIONS>)
        .addFunction("setRotations", &batchStream<BatchStream::ROTATIONS>)
        .addFunction("setScales", &batchStream<BatchStream::SCALES>)
        .addFunction("setFrames", &batchStream<BatchStream::FRAMES>)
        .addFunction("setColors", &batchStream<BatchStream::COLORS>)
        .addFunction("setFx", &batchStream<BatchStream::FX>)
        .addFunction("add", &batchAdd)
        .addFunction("set", &SpriteBatch::set)
        .addFunction("setFrame", &SpriteBatch::setFrame)
        .addFunction("remove", &SpriteBatch::remove)
        .addFunction("clear", &SpriteBatch::clear)
        .addFunction("getSpriteBatchComponent", &SpriteBatch::getComponent<SpriteBatchComponent>)
        .endClass();
#endif //DISABLE_LUA_BINDINGS
}
