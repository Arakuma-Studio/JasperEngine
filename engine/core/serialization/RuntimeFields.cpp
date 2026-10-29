// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RuntimeFields.h"

#include "Log.h"

#include <memory>
#include <mutex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::cook;

namespace jasperengine::cook {

    template<typename T>
    const void* findComponentOf(const EntityRegistry* registry, Entity entity) {
        return const_cast<EntityRegistry*>(registry)->findComponent<T>(entity);
    }

    template<typename T>
    void writeComponentOf(Writer& w, const ComponentTable& table, const void* component, const WriteContext& ctx) {
        writeStruct(w, *table.body, component);
        const size_t at = w.beginLength();
        if (const ComponentHooks* hooks = componentHooks(table.keyHash))
            if (hooks->writeExtra) hooks->writeExtra(w, component, ctx);
        w.endLength(at);
    }

    // heap first: some components hold large fixed-capacity arrays in a game build
    template<typename T>
    void loadComponentOf(Reader& r, const ComponentTable& table, const LoadContext& ctx) {
        auto value = std::make_unique<T>();
        readStruct(r, *table.body, value.get());
        Reader extra = r.record();
        const ComponentHooks* hooks = componentHooks(table.keyHash);
        if (hooks && hooks->readExtra && !extra.atEnd()) hooks->readExtra(extra, value.get(), ctx);
        if (extra.failed()) {
            Log::error("Cooked content: %s: %s", table.cppName, extra.error().c_str());
        }
        if (hooks && hooks->afterLoad) hooks->afterLoad(value.get(), ctx);
        ctx.registry->addComponent<T>(ctx.entity, std::move(*value));
    }

}

#define JE_RF(T, member, id, kind) \
    {id, FieldKind::kind, #member, &writeMember<T, &T::member>, &readMember<T, &T::member>, &visitMemberTextures<T, &T::member>}
#define JE_RF_ENTITY(T, member, id, kind) \
    {id, FieldKind::kind, #member, &writeEntityMember<T, &T::member>, &readEntityMember<T, &T::member>, nullptr}
#define JE_RF_ENTITIES(T, member, id, kind) \
    {id, FieldKind::kind, #member, &writeEntityArrayMember<T, &T::member>, &readEntityArrayMember<T, &T::member>, nullptr}
#define JE_RF_COMPONENT(T, key, hash) \
    {hash, key, #T, &kBody_##T, &findComponentOf<T>, &loadComponentOf<T>, &writeComponentOf<T>}
// the end of the list (a list of none is the bootstrap file's)
#define JE_RF_COMPONENTS_END {0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}

#define JE_RUNTIME_FIELDS_DEFINE
#include "serialization/RuntimeFields.gen.inc"
#undef JE_RUNTIME_FIELDS_DEFINE

std::span<const ComponentTable> cook::componentTables() {
    return std::span<const ComponentTable>(kComponentTables, std::size(kComponentTables) - 1);
}

const ComponentTable* cook::findComponentTable(uint32_t keyHash) {
    for (const ComponentTable& table : componentTables())
        if (table.keyHash == keyHash) return &table;
    return nullptr;
}

const ComponentTable* cook::findComponentTableByName(std::string_view cppName) {
    for (const ComponentTable& table : componentTables())
        if (cppName == table.cppName) return &table;
    return nullptr;
}

void cook::writeComponent(Writer& w, const ComponentTable& table, const void* component, const WriteContext& ctx) {
    w.u32(table.keyHash);
    const size_t at = w.beginLength();
    table.write(w, table, component, ctx);
    w.endLength(at);
}

bool cook::readComponent(Reader& r, const LoadContext& ctx) {
    const uint32_t keyHash = r.u32();
    Reader record = r.record();
    if (r.failed()) return false;
    const ComponentTable* table = findComponentTable(keyHash);
    if (!table) {
        static std::mutex mutex;
        static std::set<uint32_t> warned;
        std::lock_guard<std::mutex> lock(mutex);
        if (warned.insert(keyHash).second)
            Log::warn("Cooked content: unknown component 0x%08x skipped (a newer editor's content?)", keyHash);
        return true;
    }
    table->load(record, *table, ctx);
    if (record.failed()) {
        Log::error("Cooked content: %s of entity %u: %s", table->cppName, ctx.entity, record.error().c_str());
        return false;
    }
    return true;
}
