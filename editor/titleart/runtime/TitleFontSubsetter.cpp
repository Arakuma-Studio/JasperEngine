// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleFontSubsetter.h"

#include "TitleCharacters.h"
#include "TitleRuntimeResources.h"
#include "command/type/SetAssetMetaCmd.h"
#include "io/AtomicFile.h"
#include "titleart/TitleArtHash.h"

#include "hb.h"
#include "hb-ot.h"
#include "hb-subset.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // bumped when the subset's options change, so cached subsets of older options are not reused
    constexpr const char* SUBSET_OPTIONS_VERSION = "1";

    // the name IDs a Reserved Font Name rename rewrites (R57): the family, unique, full and
    // typographic family names and the Mac and WWS families; the PostScript ones apart
    constexpr uint16_t RENAMED_IDS[] = {1, 3, 4, 16, 18, 21};
    constexpr uint16_t POSTSCRIPT_IDS[] = {6, 25};

    std::string hex64(uint64_t h) {
        static const char* digits = "0123456789abcdef";
        std::string out(16, '0');
        for (int i = 0; i < 16; i++) out[(size_t)i] = digits[(h >> (60 - 4 * i)) & 0xf];
        return out;
    }

    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return text;
    }

    // RAII for the HarfBuzz objects of one call
    struct Blob {
        hb_blob_t* blob = nullptr;
        explicit Blob(hb_blob_t* b) : blob(b) {}
        ~Blob() { hb_blob_destroy(blob); }
        Blob(const Blob&) = delete;
        Blob& operator=(const Blob&) = delete;
    };
    struct Face {
        hb_face_t* face = nullptr;
        explicit Face(hb_face_t* f) : face(f) {}
        ~Face() { hb_face_destroy(face); }
        Face(const Face&) = delete;
        Face& operator=(const Face&) = delete;
    };

    hb_blob_t* blobOf(const std::vector<uint8_t>& bytes) {
        return hb_blob_create_or_fail(reinterpret_cast<const char*>(bytes.data()), static_cast<unsigned>(bytes.size()),
                                      HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    }

    std::string nameOf(hb_face_t* face, hb_ot_name_id_t id) {
        unsigned size = hb_ot_name_get_utf8(face, id, HB_LANGUAGE_INVALID, nullptr, nullptr);
        if (size == 0) return std::string();
        std::string text(size + 1, '\0');
        unsigned capacity = size + 1;
        hb_ot_name_get_utf8(face, id, HB_LANGUAGE_INVALID, &capacity, text.data());
        text.resize(capacity);
        return text;
    }

    uint16_t readU16(const uint8_t* p) {
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }
    void writeU16(std::string& out, uint16_t v) {
        out.push_back(static_cast<char>(v >> 8));
        out.push_back(static_cast<char>(v & 0xff));
    }

    bool readInfo(hb_face_t* face, TitleFontInfo& out) {
        out = TitleFontInfo();
        out.glyphCount = static_cast<int>(hb_face_get_glyph_count(face));
        if (out.glyphCount == 0) return false;
        Blob os2(hb_face_reference_table(face, HB_TAG('O', 'S', '/', '2')));
        unsigned length = 0;
        const char* data = hb_blob_get_data(os2.blob, &length);
        if (data && length >= 10) out.fsType = readU16(reinterpret_cast<const uint8_t*>(data) + 8);
        out.family = nameOf(face, HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY);
        if (out.family.empty()) out.family = nameOf(face, HB_OT_NAME_ID_FONT_FAMILY);
        out.copyright = nameOf(face, HB_OT_NAME_ID_COPYRIGHT);
        out.license = nameOf(face, HB_OT_NAME_ID_LICENSE);
        out.licenseUrl = nameOf(face, HB_OT_NAME_ID_LICENSE_URL);
        const std::string licence = lower(out.license);
        const std::string url = lower(out.licenseUrl);
        out.ofl = licence.find("open font license") != std::string::npos || out.license.find("OFL") != std::string::npos ||
                  url.find("/ofl") != std::string::npos || url.find("openfontlicense") != std::string::npos;
        out.reservedFontName = lower(out.copyright).find("reserved font name") != std::string::npos ||
                               lower(out.license).find("reserved font name") != std::string::npos;
        // the library's OFL fonts that declare their Reserved Font Name only in the OFL.txt shipped
        // beside them (NOTICE.md): their name table does not say so
        static const std::set<std::string> RESERVED_IN_LICENSE_FILE = {"Audiowide", "Monoton"};
        if (RESERVED_IN_LICENSE_FILE.count(out.family)) out.ofl = out.reservedFontName = true;
        out.variable = hb_ot_var_has_data(face);
        return true;
    }

    // a name string in a record's encoding: UTF-16BE for Unicode, Windows and ISO 10646 records,
    // one byte per character (ASCII, which Mac Roman shares) for the rest
    std::string encodeName(uint16_t platform, uint16_t encoding, const std::string& ascii) {
        const bool wide = platform == 0 || platform == 3 || (platform == 2 && encoding == 1);
        if (!wide) return ascii;
        std::string out;
        for (char c : ascii) writeU16(out, static_cast<uint16_t>(static_cast<unsigned char>(c)));
        return out;
    }

    // The name table with the reserved names replaced (R57); empty when the table cannot be read.
    // Records keep their order, platform, encoding and language; only the strings change.
    std::string renamedNameTable(const uint8_t* table, size_t size, const std::string& family, const std::string& postscript) {
        if (size < 6) return std::string();
        const uint16_t version = readU16(table);
        const uint16_t count = readU16(table + 2);
        const uint16_t storage = readU16(table + 4);
        size_t header = 6 + 12 * (size_t)count;
        if (version > 1 || header > size) return std::string();
        uint16_t langTags = 0;
        if (version == 1) {
            if (header + 2 > size) return std::string();
            langTags = readU16(table + header);
            header += 2 + 4 * (size_t)langTags;
            if (header > size) return std::string();
        }
        if (storage > size) return std::string();
        auto stringAt = [&](uint16_t length, uint16_t offset, std::string& out) {
            const size_t start = (size_t)storage + offset;
            if (start + length > size) return false;
            out.assign(reinterpret_cast<const char*>(table) + start, length);
            return true;
        };
        std::string records, tags, strings;
        for (size_t i = 0; i < count; i++) {
            const uint8_t* r = table + 6 + 12 * i;
            const uint16_t platform = readU16(r), encoding = readU16(r + 2), language = readU16(r + 4), id = readU16(r + 6);
            std::string text;
            if (std::find(std::begin(RENAMED_IDS), std::end(RENAMED_IDS), id) != std::end(RENAMED_IDS)) {
                text = encodeName(platform, encoding, family);
            } else if (std::find(std::begin(POSTSCRIPT_IDS), std::end(POSTSCRIPT_IDS), id) != std::end(POSTSCRIPT_IDS)) {
                text = encodeName(platform, encoding, postscript);
            } else if (!stringAt(readU16(r + 8), readU16(r + 10), text)) {
                return std::string();
            }
            if (strings.size() + text.size() > 0xffff) return std::string();
            writeU16(records, platform);
            writeU16(records, encoding);
            writeU16(records, language);
            writeU16(records, id);
            writeU16(records, static_cast<uint16_t>(text.size()));
            writeU16(records, static_cast<uint16_t>(strings.size()));
            strings += text;
        }
        for (size_t i = 0; i < langTags; i++) {
            const uint8_t* t = table + 6 + 12 * (size_t)count + 2 + 4 * i;
            std::string text;
            if (!stringAt(readU16(t), readU16(t + 2), text)) return std::string();
            if (strings.size() + text.size() > 0xffff) return std::string();
            writeU16(tags, static_cast<uint16_t>(text.size()));
            writeU16(tags, static_cast<uint16_t>(strings.size()));
            strings += text;
        }
        std::string out;
        writeU16(out, version);
        writeU16(out, count);
        writeU16(out, static_cast<uint16_t>(header));
        out += records;
        if (version == 1) {
            writeU16(out, langTags);
            out += tags;
        }
        out += strings;
        return out;
    }

    bool readFile(const fs::path& path, std::vector<uint8_t>& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return !out.empty();
    }

    // glyph count and family of a font that is about to ship
    void describe(const std::vector<uint8_t>& bytes, int faceIndex, TitleSubsetResult& result) {
        Blob blob(blobOf(bytes));
        Face face(hb_face_create(blob.blob, static_cast<unsigned>(faceIndex)));
        TitleFontInfo info;
        readInfo(face.face, info);
        result.glyphs = info.glyphCount;
        result.family = info.family;
    }

}

// ==================================================================== font info and embedding

bool editor::readTitleFontInfo(const std::vector<uint8_t>& bytes, int faceIndex, TitleFontInfo& out, std::string& error) {
    if (bytes.empty()) {
        error = "the font file is empty";
        return false;
    }
    Blob blob(blobOf(bytes));
    if (!blob.blob) {
        error = "the font file cannot be read";
        return false;
    }
    const unsigned faces = hb_face_count(blob.blob);
    if (faces == 0) {
        error = "not a TrueType or OpenType font";
        return false;
    }
    if (faceIndex < 0 || static_cast<unsigned>(faceIndex) >= faces) {
        error = "face " + std::to_string(faceIndex) + " is out of range (the file has " + std::to_string(faces) + ")";
        return false;
    }
    Face face(hb_face_create(blob.blob, static_cast<unsigned>(faceIndex)));
    if (!readInfo(face.face, out)) {
        error = "the font has no glyphs";
        return false;
    }
    return true;
}

TitleFontEmbeddingCheck editor::checkTitleFontEmbedding(const TitleFontInfo& info, bool acknowledged, const std::string& fontPath) {
    TitleFontEmbeddingCheck check;
    const std::string name = "Font " + fontPath + (info.family.empty() ? std::string() : " (" + info.family + ")");
    if ((info.fsType & TITLE_FS_TYPE_RESTRICTED) && !acknowledged) {
        check.ok = false;
        check.code = "E-TITLEART-FONT-RESTRICTED";
        check.message = name + " has a Restricted License embedding permission (OS/2 fsType 0x0002), so a game may not "
                        "embed it: make the titles that use it Baked, or tick \"" + TITLE_FONT_ACK_LABEL +
                        "\" in the font's Inspector if your licence allows it.";
        return check;
    }
    if (info.fsType & TITLE_FS_TYPE_BITMAP_ONLY) {
        check.ok = false;
        check.code = "E-TITLEART-FONT-BITMAP-ONLY";
        check.message = name + " permits only bitmap embedding (OS/2 fsType 0x0200), so its outlines cannot ship: make the "
                        "titles that use it Baked.";
        return check;
    }
    if (info.fsType & TITLE_FS_TYPE_NO_SUBSETTING) {
        check.shipWhole = true;
        check.code = "W-TITLEART-FONT-NO-SUBSET";
        check.message = name + " forbids subsetting (OS/2 fsType 0x0100), so the build ships the whole file: use another "
                        "font, or make its titles Baked, for a smaller build.";
    }
    return check;
}

bool editor::titleFontEmbeddingAcknowledged(const fs::path& fontMetaOrFontPath) {
    fs::path font = fontMetaOrFontPath;
    if (font.extension() == ".meta") font.replace_extension();
    const YAML::Node value = SetAssetMetaCmd::read(font, TITLE_FONT_ACK_META_KEY);
    if (!value || !value.IsScalar()) return false;
    bool acknowledged = false;
    return YAML::convert<bool>::decode(value, acknowledged) && acknowledged;
}

std::string editor::titleFontHash(const std::vector<uint8_t>& bytes) {
    return hex64(titleart::fnv1a64(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
}

// ==================================================================== subsetter

TitleFontSubsetter::TitleFontSubsetter(fs::path cacheDir) : cacheDir(std::move(cacheDir)) {}

fs::path TitleFontSubsetter::projectCacheDir(const fs::path& projectPath) {
    return projectPath / ".jasperengine" / "cache" / "titleart" / "subsets";
}

TitleSubsetResult TitleFontSubsetter::subset(const TitleSubsetRequest& request) {
    const auto start = std::chrono::steady_clock::now();
    TitleSubsetResult result;
    auto finish = [&]() -> TitleSubsetResult {
        result.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return result;
    };

    TitleFontInfo info;
    if (!readTitleFontInfo(request.bytes, request.faceIndex, info, result.error)) return finish();
    if (info.fsType & TITLE_FS_TYPE_NO_SUBSETTING) {
        result.ok = true;
        result.whole = true;
        result.bytes = request.bytes;
        result.glyphs = info.glyphCount;
        result.family = info.family;
        return finish();
    }

    const std::string fontHash = request.fontHash.empty() ? titleFontHash(request.bytes) : request.fontHash;
    const bool rename = request.renameReserved && info.ofl && info.reservedFontName;
    const std::string shortHash = fontHash.substr(0, 8);
    TitleCharacterSet set;
    for (uint32_t cp : request.codepoints) set.add(cp);
    const fs::path cached = cacheDir.empty() ? fs::path()
        : cacheDir / (fontHash + "-" + std::to_string(request.faceIndex) + "-" + set.hash() + (rename ? "-r" : "") + "-v" +
                      SUBSET_OPTIONS_VERSION + ".font");
    std::error_code ec;
    if (!cached.empty() && fs::is_regular_file(cached, ec) && readFile(cached, result.bytes)) {
        result.ok = true;
        result.cached = true;
        result.renamed = rename;
        describe(result.bytes, 0, result);
        return finish();
    }

    Blob blob(blobOf(request.bytes));
    Face face(hb_face_create(blob.blob, static_cast<unsigned>(request.faceIndex)));
    hb_subset_input_t* input = hb_subset_input_create_or_fail();
    if (!input) {
        result.error = "out of memory";
        return finish();
    }
    hb_set_t* unicodes = hb_subset_input_unicode_set(input);
    for (uint32_t cp : set.list()) hb_set_add(unicodes, cp);
    // every name ID, language and layout feature: a title may use any feature (a layer's
    // `features`), and the credits and the rename read the names
    for (hb_subset_sets_t which : {HB_SUBSET_SETS_NAME_ID, HB_SUBSET_SETS_NAME_LANG_ID, HB_SUBSET_SETS_LAYOUT_FEATURE_TAG,
                                   HB_SUBSET_SETS_LAYOUT_SCRIPT_TAG}) {
        hb_set_t* all = hb_subset_input_set(input, which);
        hb_set_clear(all);
        hb_set_invert(all);
    }
    hb_subset_input_set_flags(input, HB_SUBSET_FLAGS_NO_HINTING | HB_SUBSET_FLAGS_NAME_LEGACY);
    Face subsetFace(hb_subset_or_fail(face.face, input));
    hb_subset_input_destroy(input);
    if (!subsetFace.face) {
        result.error = "HarfBuzz could not subset the font";
        return finish();
    }
    if (rename) {
        Blob name(hb_face_reference_table(subsetFace.face, HB_TAG('n', 'a', 'm', 'e')));
        unsigned length = 0;
        const char* data = hb_blob_get_data(name.blob, &length);
        const std::string renamed = data ? renamedNameTable(reinterpret_cast<const uint8_t*>(data), length, "TA " + shortHash, "TA-" + shortHash)
                                         : std::string();
        if (renamed.empty()) {
            result.error = "the subset's name table cannot be rewritten (Reserved Font Name)";
            return finish();
        }
        Blob replacement(hb_blob_create(renamed.data(), static_cast<unsigned>(renamed.size()), HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr));
        hb_face_builder_add_table(subsetFace.face, HB_TAG('n', 'a', 'm', 'e'), replacement.blob);
    }
    Blob out(hb_face_reference_blob(subsetFace.face));
    unsigned length = 0;
    const char* data = hb_blob_get_data(out.blob, &length);
    if (!data || length == 0) {
        result.error = "HarfBuzz wrote an empty subset";
        return finish();
    }
    result.bytes.assign(data, data + length);
    result.ok = true;
    result.renamed = rename;
    describe(result.bytes, 0, result);
    if (!cached.empty()) {
        // a cache miss next time is the only cost of a failed write
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        io::writeFileAtomic(cached, std::string_view(reinterpret_cast<const char*>(result.bytes.data()), result.bytes.size()), options);
    }
    return finish();
}

// ==================================================================== credits

TitleFontCredit editor::titleFontCredit(const TitleFontInfo& info, const std::string& file, bool subset) {
    TitleFontCredit credit;
    credit.family = info.family;
    credit.file = file;
    credit.copyright = info.copyright;
    credit.license = info.license;
    credit.licenseUrl = info.licenseUrl;
    credit.ofl = info.ofl;
    credit.subset = subset;
    return credit;
}

std::string editor::titleFontCreditsText(const std::vector<TitleFontCredit>& credits, const std::string& projectName) {
    std::vector<TitleFontCredit> sorted = credits;
    std::sort(sorted.begin(), sorted.end(), [](const TitleFontCredit& a, const TitleFontCredit& b) { return a.file < b.file; });
    std::string out = "Third-party fonts in " + projectName + "\n\n"
                      "The fonts this build ships for its Title Art, from each font's name table.\n"
                      "Generated by JasperEngine; regenerate it by exporting again.\n";
    bool anyOfl = false;
    auto field = [&out](const char* label, const std::string& value) {
        out += std::string("  ") + label + ": " + (value.empty() ? std::string("(not stated in the font)") : value) + "\n";
    };
    for (const TitleFontCredit& credit : sorted) {
        out += "\n" + (credit.family.empty() ? credit.file : credit.family) + "\n";
        field("File", credit.file);
        field("Copyright", credit.copyright);
        field("Licence", credit.license);
        field("Licence URL", credit.licenseUrl);
        out += std::string("  Shipped: ") + (credit.subset ? "subset (only the characters its titles use)" : "whole") + "\n";
        anyOfl = anyOfl || credit.ofl;
    }
    if (anyOfl) {
        out += "\nThe fonts above under the SIL Open Font License are distributed under this licence:\n\n";
        out += std::string(titleOflText());
        if (out.back() != '\n') out += "\n";
    }
    return out;
}
