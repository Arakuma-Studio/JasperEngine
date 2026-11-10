// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R32, R47, D10)
// - Purpose: Photoshop's action descriptors (the "Descriptor structure" of the Adobe Photoshop
//   File Formats Specification): the layer effects (`lfx2`, `lmfx`, an ASL style's `Lefx`), text
//   (`TySh`), fill layers and adjustment descriptors. Written from the Adobe document (ag-psd's
//   sources were not available to port, S87 Deviations).
// - A descriptor becomes a JSON object: `_class` (its class id), then each item by its key
//   ("Md  ", "Opct", "frameFXMulti"); values: doub and long as numbers, bool, TEXT as UTF-8,
//   UntF as {_unit: "#Pxl", _value}, UnFl as {_unit, _values}, enum as {_enum, _value}, VlLs and
//   ObAr as arrays, Objc / GlbO as objects, type / GlbC as {_classref}, references as {_ref},
//   tdta / alis / Pth as JSON binary.
// - Untrusted input: every length is checked against the bytes left, nesting stops at 64, a list
//   holds at most 1,000,000 items; failures name the offset.
// - Thread-safety: pure functions.

#pragma once

#include "PsdBytes.h"
#include "titleart/TitleArtDoc.h"

#include <string>

namespace jasperengine::editor::psd {

    using Json = titleart::Json;

    constexpr int DESCRIPTOR_MAX_DEPTH = 64;

    // one descriptor (class name, class id, items); false with r's failure
    bool readDescriptor(ByteReader& r, Json& out, int depth = 0);
    // a u32 descriptor version (16) then the descriptor
    bool readVersionedDescriptor(ByteReader& r, Json& out);

    // ---- values of parsed descriptors ----

    // a number item (doub, long, or a unit value's number); `fallback` when absent
    double descNumber(const Json& d, const char* key, double fallback);
    // a UntF item's unit ("#Pxl", "#Prc", "#Ang", "#Pnt", "#Mlm", "#Rlt"...), "" when not a unit value
    std::string descUnit(const Json& d, const char* key);
    bool descBool(const Json& d, const char* key, bool fallback);
    std::string descText(const Json& d, const char* key);
    // an enum item's value ("Nrml", "OutF"); "" when absent
    std::string descEnum(const Json& d, const char* key);
    const Json* descObject(const Json& d, const char* key);
    const Json* descList(const Json& d, const char* key);
    std::vector<uint8_t> descRaw(const Json& d, const char* key);

    // ---- writing (the PSD writer's and the tests' fixtures) ----

    class DescriptorWriter {
    public:
        explicit DescriptorWriter(ByteWriter& w) : w(w) {}
        // a descriptor: class name "" and class id, then the item count of `items` calls
        void begin(const std::string& classId, uint32_t items);
        void key(const std::string& key);
        void doub(const std::string& key, double v);
        void longItem(const std::string& key, int32_t v);
        void boolean(const std::string& key, bool v);
        void text(const std::string& key, const std::string& v);
        void unitFloat(const std::string& key, const char* unit, double v);
        void enumeration(const std::string& key, const std::string& type, const std::string& value);
        void object(const std::string& key, const std::string& classId, uint32_t items);
        void list(const std::string& key, uint32_t count);
        // a list item's own type tag before its value
        void listObject(const std::string& classId, uint32_t items);
        void raw(const std::string& key, const std::vector<uint8_t>& data);

    private:
        ByteWriter& w;
        void classId(const std::string& id);
    };

}
