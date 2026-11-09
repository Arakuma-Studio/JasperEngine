// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Helpers shared by the port of Pachislop's project migration (src/io/project.ts and
// src/io/migrate/*.ts by SafEight, used with the author's permission): the loose-JSON readers its
// TypeScript uses (`isObj`, `filled`, typed reads with the removed effects' defaults) and the ids
// the migration mints.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace jasperengine::editor::pachislop {

    using Json = titleart::Json;

    inline bool isObj(const Json& v) { return v.is_object(); }

    // `defaults` overlaid by the members present in `v` (shallow): Pachislop's `filled`
    inline Json filled(Json defaults, const Json& v) {
        if (!v.is_object()) return defaults;
        for (auto it = v.begin(); it != v.end(); ++it) defaults[it.key()] = it.value();
        return defaults;
    }

    // `object[key]` when it is a number (finite), else `fallback`
    inline double num(const Json& object, const char* key, double fallback) {
        if (!object.is_object()) return fallback;
        auto it = object.find(key);
        return it != object.end() && it->is_number() && std::isfinite(it->get<double>()) ? it->get<double>() : fallback;
    }
    inline std::string str(const Json& object, const char* key, const std::string& fallback = {}) {
        if (!object.is_object()) return fallback;
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
    }
    inline bool boolean(const Json& object, const char* key, bool fallback) {
        if (!object.is_object()) return fallback;
        auto it = object.find(key);
        return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }
    inline bool has(const Json& object, const char* key) { return object.is_object() && object.contains(key); }

    // The ids the migration mints (Pachislop: crypto.randomUUID().slice(0, 8)): deterministic, so a
    // file imports to the same document every time
    class IdMint {
    public:
        std::string next() {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "mg%06x", counter++ & 0xffffff);
            return buffer;
        }
    private:
        unsigned counter = 1;
    };

}
