// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Written from the Adobe Photoshop File Formats Specification, "Descriptor structure".

#include "PsdDescriptor.h"

using namespace jasperengine::editor::psd;

namespace {

    constexpr uint32_t MAX_ITEMS = 1000000;

    // a class id or key: u32 length, then that many bytes, or 4 bytes when the length is 0
    std::string readId(ByteReader& r) {
        const uint32_t n = r.u32();
        const size_t size = n ? n : 4;
        if (!r.need(size, "a descriptor key passes the end")) return std::string();
        const uint8_t* p = r.view(size);
        return p ? std::string(reinterpret_cast<const char*>(p), size) : std::string();
    }

    bool readValue(ByteReader& r, const std::string& type, Json& out, int depth);

    bool readList(ByteReader& r, Json& out, int depth) {
        const uint32_t count = r.u32();
        if (count > MAX_ITEMS || (uint64_t)count * 4 > r.remaining()) return r.fail("a descriptor list of " + std::to_string(count) + " items passes the end");
        out = Json::array();
        for (uint32_t i = 0; i < count && r.ok(); i++) {
            const std::string type = r.fourcc();
            Json item;
            if (!readValue(r, type, item, depth + 1)) return false;
            out.push_back(std::move(item));
        }
        return r.ok();
    }

    bool readReference(ByteReader& r, Json& out) {
        const uint32_t count = r.u32();
        if (count > MAX_ITEMS || (uint64_t)count * 4 > r.remaining()) return r.fail("a reference of " + std::to_string(count) + " items passes the end");
        Json items = Json::array();
        for (uint32_t i = 0; i < count && r.ok(); i++) {
            const std::string form = r.fourcc();
            Json item = Json{{"_form", form}};
            if (form == "prop") {
                item["name"] = r.unicode();
                item["class"] = readId(r);
                item["key"] = readId(r);
            } else if (form == "Clss") {
                item["name"] = r.unicode();
                item["class"] = readId(r);
            } else if (form == "Enmr") {
                item["name"] = r.unicode();
                item["class"] = readId(r);
                item["type"] = readId(r);
                item["value"] = readId(r);
            } else if (form == "rele") {
                item["name"] = r.unicode();
                item["class"] = readId(r);
                item["offset"] = r.i32();
            } else if (form == "Idnt" || form == "indx") {
                item["value"] = r.i32();
            } else if (form == "name") {
                item["name"] = r.unicode();
                item["class"] = readId(r);
                item["value"] = r.unicode();
            } else {
                return r.fail("unknown reference form '" + form + "'");
            }
            items.push_back(std::move(item));
        }
        out = Json{{"_ref", std::move(items)}};
        return r.ok();
    }

    bool readValue(ByteReader& r, const std::string& type, Json& out, int depth) {
        if (depth > DESCRIPTOR_MAX_DEPTH) return r.fail("descriptors nest deeper than " + std::to_string(DESCRIPTOR_MAX_DEPTH));
        if (type == "Objc" || type == "GlbO") return readDescriptor(r, out, depth + 1);
        if (type == "VlLs") return readList(r, out, depth);
        if (type == "doub") {
            out = r.f64();
        } else if (type == "UntF") {
            const std::string unit = r.fourcc();
            out = Json{{"_unit", unit}, {"_value", r.f64()}};
        } else if (type == "UnFl") {
            const std::string unit = r.fourcc();
            const uint32_t count = r.u32();
            if ((uint64_t)count * 8 > r.remaining()) return r.fail("a unit list of " + std::to_string(count) + " values passes the end");
            Json values = Json::array();
            for (uint32_t i = 0; i < count; i++) values.push_back(r.f64());
            out = Json{{"_unit", unit}, {"_values", values}};
        } else if (type == "TEXT") {
            out = r.unicode();
        } else if (type == "enum") {
            const std::string enumType = readId(r);
            out = Json{{"_enum", enumType}, {"_value", readId(r)}};
        } else if (type == "long") {
            out = r.i32();
        } else if (type == "comp") {
            out = r.i64();
        } else if (type == "bool") {
            out = r.u8() != 0;
        } else if (type == "type" || type == "GlbC") {
            r.unicode();
            out = Json{{"_classref", readId(r)}};
        } else if (type == "obj ") {
            return readReference(r, out);
        } else if (type == "alis" || type == "tdta" || type == "Pth ") {
            const uint64_t n = r.length(false, "a raw descriptor value");
            const std::vector<uint8_t> bytes = r.bytes((size_t)n);
            out = Json::binary(Json::binary_t::container_type(bytes.begin(), bytes.end()));
        } else if (type == "ObAr") {
            // object array: version, class name and id, then items each a key with a unit list or a value
            r.u32();
            r.unicode();
            const std::string cls = readId(r);
            const uint32_t count = r.u32();
            if (count > MAX_ITEMS || (uint64_t)count * 4 > r.remaining()) return r.fail("an object array of " + std::to_string(count) + " items passes the end");
            Json object = Json{{"_class", cls}};
            for (uint32_t i = 0; i < count && r.ok(); i++) {
                const std::string key = readId(r);
                const std::string itemType = r.fourcc();
                Json value;
                if (!readValue(r, itemType, value, depth + 1)) return false;
                object[key] = std::move(value);
            }
            out = Json::array({std::move(object)});
        } else {
            return r.fail("unknown descriptor value type '" + type + "'");
        }
        return r.ok();
    }

}

bool jasperengine::editor::psd::readDescriptor(ByteReader& r, Json& out, int depth) {
    if (depth > DESCRIPTOR_MAX_DEPTH) return r.fail("descriptors nest deeper than " + std::to_string(DESCRIPTOR_MAX_DEPTH));
    r.unicode();
    const std::string cls = readId(r);
    const uint32_t count = r.u32();
    if (count > MAX_ITEMS || (uint64_t)count * 8 > r.remaining() + 8) {
        if (r.ok()) return r.fail("a descriptor of " + std::to_string(count) + " items passes the end");
        return false;
    }
    out = Json{{"_class", cls}};
    for (uint32_t i = 0; i < count && r.ok(); i++) {
        const std::string key = readId(r);
        const std::string type = r.fourcc();
        Json value;
        if (!readValue(r, type, value, depth)) return false;
        out[key] = std::move(value);
    }
    return r.ok();
}

bool jasperengine::editor::psd::readVersionedDescriptor(ByteReader& r, Json& out) {
    const uint32_t version = r.u32();
    if (r.ok() && version != 16) return r.fail("descriptor version " + std::to_string(version) + " is not 16");
    return readDescriptor(r, out, 0);
}

double jasperengine::editor::psd::descNumber(const Json& d, const char* key, double fallback) {
    auto it = d.find(key);
    if (it == d.end()) return fallback;
    if (it->is_number()) return it->get<double>();
    if (it->is_object() && it->contains("_value") && (*it)["_value"].is_number()) return (*it)["_value"].get<double>();
    return fallback;
}

std::string jasperengine::editor::psd::descUnit(const Json& d, const char* key) {
    auto it = d.find(key);
    if (it == d.end() || !it->is_object() || !it->contains("_unit")) return "";
    return (*it)["_unit"].get<std::string>();
}

bool jasperengine::editor::psd::descBool(const Json& d, const char* key, bool fallback) {
    auto it = d.find(key);
    return it != d.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

std::string jasperengine::editor::psd::descText(const Json& d, const char* key) {
    auto it = d.find(key);
    return it != d.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string jasperengine::editor::psd::descEnum(const Json& d, const char* key) {
    auto it = d.find(key);
    if (it == d.end() || !it->is_object() || !it->contains("_value") || !(*it)["_value"].is_string()) return "";
    return (*it)["_value"].get<std::string>();
}

const Json* jasperengine::editor::psd::descObject(const Json& d, const char* key) {
    auto it = d.find(key);
    return it != d.end() && it->is_object() && it->contains("_class") ? &*it : nullptr;
}

const Json* jasperengine::editor::psd::descList(const Json& d, const char* key) {
    auto it = d.find(key);
    return it != d.end() && it->is_array() ? &*it : nullptr;
}

std::vector<uint8_t> jasperengine::editor::psd::descRaw(const Json& d, const char* key) {
    auto it = d.find(key);
    if (it == d.end() || !it->is_binary()) return {};
    const auto& bin = it->get_binary();
    return std::vector<uint8_t>(bin.begin(), bin.end());
}

// ---- writing ----

void DescriptorWriter::classId(const std::string& id) {
    if (id.size() == 4) {
        w.u32(0);
    } else {
        w.u32((uint32_t)id.size());
    }
    w.bytes(reinterpret_cast<const uint8_t*>(id.data()), id.size());
}

void DescriptorWriter::begin(const std::string& cls, uint32_t items) {
    w.unicode("", true);
    classId(cls);
    w.u32(items);
}

void DescriptorWriter::key(const std::string& k) {
    classId(k);
}

void DescriptorWriter::doub(const std::string& k, double v) {
    key(k);
    w.fourcc("doub");
    w.f64(v);
}

void DescriptorWriter::longItem(const std::string& k, int32_t v) {
    key(k);
    w.fourcc("long");
    w.i32(v);
}

void DescriptorWriter::boolean(const std::string& k, bool v) {
    key(k);
    w.fourcc("bool");
    w.u8(v ? 1 : 0);
}

void DescriptorWriter::text(const std::string& k, const std::string& v) {
    key(k);
    w.fourcc("TEXT");
    w.unicode(v, true);
}

void DescriptorWriter::unitFloat(const std::string& k, const char* unit, double v) {
    key(k);
    w.fourcc("UntF");
    w.fourcc(unit);
    w.f64(v);
}

void DescriptorWriter::enumeration(const std::string& k, const std::string& type, const std::string& value) {
    key(k);
    w.fourcc("enum");
    classId(type);
    classId(value);
}

void DescriptorWriter::object(const std::string& k, const std::string& cls, uint32_t items) {
    key(k);
    w.fourcc("Objc");
    begin(cls, items);
}

void DescriptorWriter::list(const std::string& k, uint32_t count) {
    key(k);
    w.fourcc("VlLs");
    w.u32(count);
}

void DescriptorWriter::listObject(const std::string& cls, uint32_t items) {
    w.fourcc("Objc");
    begin(cls, items);
}

void DescriptorWriter::raw(const std::string& k, const std::vector<uint8_t>& data) {
    key(k);
    w.fourcc("tdta");
    w.u32((uint32_t)data.size());
    w.bytes(data);
}
