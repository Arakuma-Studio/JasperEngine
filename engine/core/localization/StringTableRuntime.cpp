// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StringTableRuntime.h"

#include "localization/MessageFormat.h"

#include "json.hpp"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace jasperengine::l10n {

namespace {

    constexpr char kMagic[4] = {'J', 'S', 'T', 'B'};
    constexpr uint32_t kVersion = 1;

    void putU32(std::string& out, uint32_t value) {
        for (int k = 0; k < 4; k++) out.push_back(static_cast<char>((value >> (8 * k)) & 0xff));
    }

    void putU64(std::string& out, uint64_t value) {
        for (int k = 0; k < 8; k++) out.push_back(static_cast<char>((value >> (8 * k)) & 0xff));
    }

    struct Reader {
        std::string_view data;
        size_t pos = 0;
        bool ok = true;

        uint64_t get(int bytes) {
            if (!ok || data.size() - pos < static_cast<size_t>(bytes)) {
                ok = false;
                return 0;
            }
            uint64_t value = 0;
            for (int k = 0; k < bytes; k++) value |= static_cast<uint64_t>(static_cast<unsigned char>(data[pos + k])) << (8 * k);
            pos += bytes;
            return value;
        }
    };

    std::shared_ptr<StringTable> fail(std::string* error, const std::string& message) {
        if (error) *error = message;
        return nullptr;
    }

}

uint64_t StringTable::hashKey(std::string_view key, uint64_t seed) {
    uint64_t hash = 14695981039346656037ull ^ seed;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::shared_ptr<const StringTable> StringTable::fromEntries(const std::string& language, const std::vector<Entry>& entries, uint64_t seed) {
    auto table = std::make_shared<StringTable>();
    table->lang = language;
    table->seed = seed;
    std::vector<std::pair<uint64_t, size_t>> order;
    order.reserve(entries.size());
    size_t bytes = 0;
    for (size_t k = 0; k < entries.size(); k++) {
        order.emplace_back(hashKey(entries[k].key, seed), k);
        bytes += entries[k].text.size();
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // a duplicate hash keeps its first entry (cook() refuses it)
    order.erase(std::unique(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), order.end());
    table->blob.reserve(bytes);
    table->hashes.reserve(order.size());
    table->rows.reserve(order.size() * 2);
    for (const auto& [hash, index] : order) {
        const Entry& entry = entries[index];
        const uint32_t length = static_cast<uint32_t>(std::min<size_t>(entry.text.size(), LENGTH_MASK));
        table->hashes.push_back(hash);
        table->rows.push_back(static_cast<uint32_t>(table->blob.size()));
        table->rows.push_back(length | (MessageFormat::isPlain(entry.text) ? FLAG_PLAIN : 0u) | (entry.needsReview ? FLAG_REVIEW : 0u));
        table->blob.append(entry.text, 0, length);
    }
    return table;
}

std::shared_ptr<const StringTable> StringTable::fromStringTableJson(std::string_view json, std::string* ns, std::string* language, std::string* error) {
    const nlohmann::json doc = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return fail(error, "not a JSON object");
    if (doc.value("format", std::string()) != "jasperengine.stringtable") return fail(error, "not a jasperengine.stringtable");
    if (ns) *ns = doc.value("namespace", std::string());
    const std::string lang = doc.value("source_language", std::string("en"));
    if (language) *language = lang;
    std::vector<Entry> entries;
    const auto it = doc.find("entries");
    if (it != doc.end() && it->is_array()) {
        for (const auto& row : *it) {
            if (!row.is_object() || !row.contains("key") || !row["key"].is_string()) continue;
            entries.push_back({row["key"].get<std::string>(), row.value("text", std::string()), false});
        }
    }
    return fromEntries(lang, entries);
}

std::shared_ptr<const StringTable> StringTable::fromTranslationJson(std::string_view json, std::string* language, std::string* error) {
    const nlohmann::json doc = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return fail(error, "not a JSON object");
    if (doc.value("format", std::string()) != "jasperengine.translation") return fail(error, "not a jasperengine.translation");
    const std::string lang = doc.value("language", std::string());
    if (language) *language = lang;
    std::vector<Entry> entries;
    const auto it = doc.find("entries");
    if (it != doc.end() && it->is_object()) {
        for (const auto& [key, row] : it->items()) {
            if (!row.is_object() || !row.contains("text") || !row["text"].is_string()) continue;
            entries.push_back({key, row["text"].get<std::string>(), row.value("status", std::string()) == "needs_review"});
        }
    }
    return fromEntries(lang, entries);
}

bool StringTable::cook(const std::string& language, const std::vector<Entry>& entries, std::string& out, std::string* error, uint64_t seed) {
    std::vector<std::pair<uint64_t, size_t>> order;
    for (size_t k = 0; k < entries.size(); k++) order.emplace_back(hashKey(entries[k].key, seed), k);
    std::sort(order.begin(), order.end());
    for (size_t k = 1; k < order.size(); k++) {
        if (order[k].first == order[k - 1].first) {
            if (error) {
                *error = "the keys \"" + entries[order[k - 1].second].key + "\" and \"" + entries[order[k].second].key +
                         "\" have the same hash: rename one of them";
            }
            return false;
        }
    }
    const std::shared_ptr<const StringTable> table = fromEntries(language, entries, seed);
    out.clear();
    out.append(kMagic, 4);
    putU32(out, kVersion);
    putU32(out, static_cast<uint32_t>(table->hashes.size()));
    putU64(out, seed);
    putU32(out, static_cast<uint32_t>(language.size()));
    out.append(language);
    for (uint64_t hash : table->hashes) putU64(out, hash);
    for (uint32_t value : table->rows) putU32(out, value);
    putU32(out, static_cast<uint32_t>(table->blob.size()));
    out.append(table->blob);
    return true;
}

std::shared_ptr<const StringTable> StringTable::fromStrbin(std::string_view data, std::string* error) {
    if (data.size() < 4 || std::memcmp(data.data(), kMagic, 4) != 0) return fail(error, "not a cooked string table");
    Reader r{data, 4};
    const uint32_t version = static_cast<uint32_t>(r.get(4));
    if (r.ok && version != kVersion) return fail(error, "cooked string table version " + std::to_string(version) + " is not supported");
    const uint32_t count = static_cast<uint32_t>(r.get(4));
    auto table = std::make_shared<StringTable>();
    table->seed = r.get(8);
    const uint32_t languageSize = static_cast<uint32_t>(r.get(4));
    if (!r.ok || languageSize > 64 || data.size() - r.pos < languageSize) return fail(error, "damaged cooked string table");
    table->lang.assign(data.substr(r.pos, languageSize));
    r.pos += languageSize;
    if (count > (data.size() - r.pos) / 16) return fail(error, "damaged cooked string table");
    table->hashes.resize(count);
    table->rows.resize(static_cast<size_t>(count) * 2);
    for (uint32_t k = 0; k < count; k++) table->hashes[k] = r.get(8);
    for (uint32_t k = 0; k < count * 2; k++) table->rows[k] = static_cast<uint32_t>(r.get(4));
    const uint32_t blobSize = static_cast<uint32_t>(r.get(4));
    if (!r.ok || data.size() - r.pos != blobSize) return fail(error, "damaged cooked string table");
    table->blob.assign(data.substr(r.pos));
    for (uint32_t k = 0; k < count; k++) {
        const uint64_t end = static_cast<uint64_t>(table->rows[2 * k]) + (table->rows[2 * k + 1] & LENGTH_MASK);
        if (end > blobSize || (k > 0 && table->hashes[k] <= table->hashes[k - 1])) return fail(error, "damaged cooked string table");
    }
    return table;
}

bool StringTable::find(std::string_view key, std::string_view& text, bool* needsReview, bool* plain) const {
    const uint64_t hash = hashKey(key, seed);
    const auto it = std::lower_bound(hashes.begin(), hashes.end(), hash);
    if (it == hashes.end() || *it != hash) return false;
    const size_t index = static_cast<size_t>(it - hashes.begin());
    const uint32_t offset = rows[2 * index];
    const uint32_t lengthAndFlags = rows[2 * index + 1];
    text = std::string_view(blob).substr(offset, lengthAndFlags & LENGTH_MASK);
    if (needsReview) *needsReview = (lengthAndFlags & FLAG_REVIEW) != 0;
    if (plain) *plain = (lengthAndFlags & FLAG_PLAIN) != 0;
    return true;
}

}  // namespace jasperengine::l10n
